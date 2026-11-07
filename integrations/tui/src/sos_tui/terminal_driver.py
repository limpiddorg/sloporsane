"""Extended keyboard fallback for Textual's Unix terminal driver."""

from textual.drivers.linux_driver import LinuxDriver


class TerminalDriver(LinuxDriver):
    def write(self, data: str) -> None:
        # Alternate-screen entry enables xterm modifyOtherKeys as fallback;
        # Textual's later Kitty push takes precedence where supported.
        if data == "\x1b[?1049h":
            super().write(data)
            super().write("\x1b[>4;2m")
            return
        if data == "\x1b[?1049l":
            # Textual has popped Kitty's stack; restore xterm defaults for the shell/editor.
            super().write("\x1b[>4m")
        super().write(data)
