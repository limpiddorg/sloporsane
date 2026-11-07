"""Native clipboard complement to Textual's OSC 52 terminal support."""

import asyncio
import os
import shutil
import sys


def copy_command() -> list[str] | None:
    # Over SSH the clipboard belongs to the client, reached through OSC 52.
    if os.environ.get("SSH_CONNECTION") or os.environ.get("SSH_TTY"):
        return None
    if sys.platform == "darwin":
        return ["pbcopy"] if shutil.which("pbcopy") else None
    if sys.platform == "linux":
        if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-copy"):
            return ["wl-copy"]
        if os.environ.get("DISPLAY"):
            if shutil.which("xclip"):
                return ["xclip", "-selection", "clipboard"]
            if shutil.which("xsel"):
                return ["xsel", "--clipboard", "--input"]
    if sys.platform == "win32" and shutil.which("powershell.exe"):
        return ["powershell.exe", "-NonInteractive", "-NoProfile", "-Command",
                "[Console]::InputEncoding = [System.Text.Encoding]::UTF8; "
                "Set-Clipboard -Value ([Console]::In.ReadToEnd())"]
    return None


async def copy_native(text: str) -> None:
    command = copy_command()
    if command is None:
        return
    try:
        process = await asyncio.create_subprocess_exec(
            *command, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
        )
    except OSError:
        return  # OSC 52 has already been sent; native support is optional.
    try:
        await asyncio.wait_for(process.communicate(text.encode("utf-8")), timeout=2)
    except (TimeoutError, OSError):
        pass
    finally:
        if process.returncode is None:
            try:
                process.kill()
            except ProcessLookupError:
                pass
            await process.wait()
