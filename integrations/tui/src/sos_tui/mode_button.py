"""Mode button with the reusable encoding/decoding scan while busy."""

from time import monotonic

from rich.style import Style
from rich.text import Text
from textual.timer import Timer
from textual.message import Message
from textual.strip import Strip
from textual.widgets import Button

from .config import MODE_COLORS
from .scan_animation import ScanAnimation, WHITE


class ModeButton(Button):
    FRAME_INTERVAL = 1 / 30
    TRAVEL_SECONDS = 1.5  # One complete round trip.

    class SpinnerFrame(Message):
        def __init__(self, index: int) -> None:
            super().__init__()
            self.index = index

    class ConfirmationFrame(Message):
        pass

    def __init__(self, **kwargs) -> None:
        super().__init__("Sane", **kwargs)
        self.active_effect_duration = 0
        self.mode = "sane"
        self.mode_background = None
        self.plan_ready = False
        self.implementation_view = False
        self.implementation_plan_ready = False
        self.busy = False
        self.confirming = False
        self.phase = 0.0
        self.spinner_frame = 0
        self.last_frame = 0.0
        self.animation_timer: Timer | None = None
        self.scan = ScanAnimation()
        self.scan_started = monotonic()
        self.scan_pressed = False

    def on_mount(self) -> None:
        self.animation_timer = self.set_interval(self.FRAME_INTERVAL, self.tick, pause=not (self.busy or self.confirming))

    def set_confirmation_active(self, active: bool) -> None:
        self.confirming = active
        if active and not self.busy:
            self.last_frame = monotonic()
        self.update_animation()

    def update_animation(self) -> None:
        if self.animation_timer is not None:
            if self.busy or self.confirming:
                self.animation_timer.resume()
            else:
                self.animation_timer.pause()

    def set_mode(self, mode: str) -> None:
        self.mode = mode
        self.update_label()
        self.styles.background = self.mode_background or MODE_COLORS[self.mode]
        self.refresh()

    def update_label(self) -> None:
        self.label = "Cancel" if self.busy else "Sane" if self.implementation_view or self.mode == "sane" else "Slop"

    def set_plan_ready(self, ready: bool) -> None:
        self.plan_ready = ready
        self.update_label()

    def set_busy(self, busy: bool) -> None:
        if busy == self.busy:
            return
        self.busy = busy
        self.update_label()
        self.phase = 0.0
        self.spinner_frame = 0
        self.last_frame = monotonic()
        self.scan_started = self.last_frame
        self.update_animation()
        self.refresh()

    def tick(self) -> None:
        now = monotonic()
        elapsed = now - self.last_frame
        self.last_frame = now
        self.phase = (self.phase + elapsed / self.TRAVEL_SECONDS) % 1.0
        # Share this timer with Activity, at eight spinner frames per second.
        frame = int(self.phase * 12) % 4
        if self.busy and frame != self.spinner_frame:
            self.spinner_frame = frame
            self.post_message(self.SpinnerFrame(frame))
        if self.confirming:
            self.post_message(self.ConfirmationFrame())
        else:
            self.refresh()

    def render(self) -> Text:
        width = self.content_size.width
        base, accent = self.background_colors
        ink = base + self.styles.color
        if self.busy and not (self.mouse_hover or self.scan_pressed):
            return self.scan.render(monotonic() - self.scan_started, width, str(self.label), accent, ink)
        if self.busy:
            accent = accent.darken(0.12) if self.scan_pressed else accent.blend(WHITE, 0.22)
        label = str(self.label)[:width]
        start = (width - len(label)) // 2
        return Text(no_wrap=True).append((" " * start + label).ljust(width),
                                        style=Style(color=ink.rich_color, bgcolor=accent.rich_color, bold=True))

    def on_mouse_down(self) -> None:
        self.scan_pressed = True
        self.refresh()

    def on_mouse_up(self) -> None:
        self.scan_pressed = False
        self.refresh()

    def on_leave(self) -> None:
        self.scan_pressed = False
        self.refresh()

    def render_line(self, y: int) -> Strip:
        # Draw the full row directly, without extra cells from text alignment.
        return Strip(self.render().render(self.app.console), self.content_size.width)
