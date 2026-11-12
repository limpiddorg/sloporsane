"""Short transient endpoints, independent of checkout and platform TMPDIR length."""
from contextlib import contextmanager
from pathlib import Path
import tempfile


@contextmanager
def socket_directory():
    # /tmp remains short even when macOS TMPDIR or the worktree path is long.
    with tempfile.TemporaryDirectory(prefix='sos-nvim-', dir='/tmp') as directory:
        yield Path(directory)
