"""Connect to a shared daemon, starting the bundled executable when necessary."""

import asyncio
import fcntl
import os
from pathlib import Path
import stat
import subprocess
import threading
from collections.abc import Awaitable, Callable


BUNDLED_DAEMON = Path(__file__).parent / "bin" / "sosd"
STARTUP_TIMEOUT = 10.0
RETRY_INTERVAL = 0.05


def daemon_log_path() -> Path:
    state = Path(os.environ.get("XDG_STATE_HOME", ""))
    if not state.is_absolute():
        state = Path.home() / ".local" / "state"
    return state / "sos" / "sosd.log"


async def connect_or_start(connect: Callable[[], Awaitable[None]], socket_path: str) -> None:
    try:
        await connect()
        return
    except (FileNotFoundError, ConnectionError):
        pass

    socket = Path(socket_path).absolute()
    process: subprocess.Popen | None = None
    connected = False
    log_path = daemon_log_path()
    try:
        async with asyncio.timeout(STARTUP_TIMEOUT):
            # Keep this lock file: deleting it could create two independent startup locks.
            descriptor = os.open(str(socket) + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
            with os.fdopen(descriptor, "a") as lock:
                while True:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        await asyncio.sleep(RETRY_INTERVAL)

                # Another TUI may have started it while we were waiting for the lock.
                try:
                    await connect()
                    return
                except (FileNotFoundError, ConnectionError):
                    pass

                if not BUNDLED_DAEMON.is_file() or not os.access(BUNDLED_DAEMON, os.X_OK):
                    raise FileNotFoundError("Bundled sosd is missing or not executable; rebuild and install the TUI")
                try:
                    mode = socket.lstat().st_mode
                except FileNotFoundError:
                    pass
                else:
                    if not stat.S_ISSOCK(mode):
                        raise OSError(f"Daemon socket path is occupied by a non-socket file: {socket}")
                    socket.unlink()  # An existing socket that refused connection is stale.

                log_path.parent.mkdir(parents=True, exist_ok=True)
                with log_path.open("ab") as log:
                    process = subprocess.Popen(
                        [str(BUNDLED_DAEMON.resolve()), str(socket)],
                        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                        start_new_session=True, cwd="/",
                    )
                    log.write(f"[tui] Started sosd (pid {process.pid})\n".encode())
                    log.flush()
                # Reap it if it exits while this TUI is still alive, without tying its lifetime to ours.
                threading.Thread(target=process.wait, daemon=True).start()
                while True:
                    try:
                        await connect()
                        connected = True
                        return
                    except (FileNotFoundError, ConnectionError):
                        if process.poll() is not None:
                            raise ConnectionError(f"sosd exited with code {process.returncode}; see {log_path}")
                        await asyncio.sleep(RETRY_INTERVAL)
    except TimeoutError as error:
        raise ConnectionError(f"Timed out waiting for sosd; see {log_path}") from error
    finally:
        if process is not None and not connected and process.poll() is None:
            try:
                process.terminate()
            except ProcessLookupError:
                pass
