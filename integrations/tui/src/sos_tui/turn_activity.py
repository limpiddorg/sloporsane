"""Activity drawer between chat texts, with indented content below its toggle."""

from rich.text import Text
from time import monotonic
from typing import Any
from textual.app import ComposeResult
from textual.binding import Binding
from textual.color import Color
from textual.containers import Vertical
from textual.events import Click
from textual.message import Message
from textual.timer import Timer
from textual.widget import Widget
from textual.widgets import Collapsible, Rule, Static

from .config import ALGAE_MD, BOLD_MD, MISC_MD, MUTED_COLOR, SLOP_COLOR, TITLE_MD


class ProcessWaiting(Message):
    def __init__(self, call_id: str, waiting: bool, status: str = "Waiting for process completion") -> None:
        super().__init__()
        self.call_id = call_id
        self.waiting = waiting
        self.status = status


def reasoning_label(effort: str | None) -> str:
    return "No reasoning" if effort == "none" else effort or "—"


def format_duration(milliseconds: int) -> str:
    seconds = max(0, milliseconds // 1000)
    if seconds < 60:
        return f"{seconds}s"
    minutes, seconds = divmod(seconds, 60)
    if minutes < 60:
        return f"{minutes}m {seconds:02d}s"
    hours, minutes = divmod(minutes, 60)
    return f"{hours}h {minutes:02d}m"


class ActivityToggle(Static, can_focus=True):
    SPINNER_FRAMES = ("-", "\\", "|", "/")
    BINDINGS = [Binding("enter,space", "toggle", show=False)]
    ALLOW_SELECT = False
    FOCUS_ON_CLICK = False

    class Toggled(Message):
        pass

    def __init__(self) -> None:
        super().__init__()
        self.expanded = False
        self.is_terminal = True
        self.spinner = "-"
        self.has_tools = False
        self.has_reasoning = False
        self.has_reads = False
        self.has_skills = False
        self.has_refused = False
        self.model: str | None = None
        self.reasoning: str | None = None
        self.started_at: float | None = None
        self.duration_ms: int | None = None
        self.duration_label = ""
        self.model_duration_ms = 0
        self.output_tokens: int | None = 0
        self.model_call_count = 0
        self.step_count = 0
        self.waiting_processes: set[str] = set()
        self.waiting_labels: dict[str, str] = {}
        self.wait_status = Static("", classes="process-wait-status", markup=False)
        self.wait_status.display = False
        self.wait_started = 0.0
        self.wait_timer: Timer | None = None

    def on_mount(self) -> None:
        self.update_wait_animation()

    def set_process_waiting(self, call_id: str, waiting: bool, status: str = "Waiting for process completion") -> None:
        was_waiting = bool(self.waiting_processes)
        if waiting:
            self.waiting_processes.add(call_id)
            self.waiting_labels[call_id] = " ".join(status.split()[:5]).rstrip(". …") or "Waiting for process completion"
        else:
            self.waiting_processes.discard(call_id)
            self.waiting_labels.pop(call_id, None)
        if not was_waiting and self.waiting_processes:
            self.wait_started = monotonic()
        self.update_wait_animation()
        self.refresh()

    def update_wait_animation(self) -> None:
        active = bool(self.waiting_processes) and self.started_at is not None and self.is_terminal
        if active and self.is_mounted and self.wait_timer is None:
            self.wait_timer = self.set_interval(1 / 20, self.refresh_wait_status, pause=True)
        self.wait_status.display = active
        if active:
            self.refresh_wait_status()
        if self.wait_timer is not None:
            if active:
                self.wait_timer.resume()
            else:
                self.wait_timer.pause()

    def refresh_wait_status(self) -> None:
        self.wait_status.update(self.waiting_label())

    def waiting_label(self) -> Text:
        elapsed = max(0.0, monotonic() - self.wait_started)
        label = next(iter(self.waiting_labels.values()), "Waiting for process completion")
        text = Text(label, style=MUTED_COLOR)
        # A soft, one-way highlight changes foreground only, never the dots or background.
        center = (elapsed % 3.5) / 3.5 * (len(label) + 8) - 4
        base = Color.parse(MUTED_COLOR)
        for index in range(len(label)):
            strength = max(0.0, 1 - abs(index - center) / 4) * .9
            if strength:
                text.stylize(base.blend(Color.parse("#ffffff"), strength).hex, index, index + 1)
        text.append(("." * (int(elapsed / .5) % 3 + 1)).ljust(3), style=MUTED_COLOR)
        return text

    def render(self) -> Text:
        text = Text(f"{'▾' if self.expanded else '▸'} Activity")
        if self.has_tools:
            text.append(" ▪", style=BOLD_MD)
        if self.has_reasoning:
            text.append(" ▪", style=MISC_MD)
        if self.has_reads:
            text.append(" ▪", style=TITLE_MD)
        if self.has_skills:
            text.append(" ▪", style=ALGAE_MD)
        if self.has_refused:
            text.append(" ▪", style=SLOP_COLOR)
        if self.is_terminal and self.started_at is not None:
            text.append(f" {self.spinner}")
        if self.step_count:
            text.append(f" {self.step_count} {'step' if self.step_count == 1 else 'steps'}")
            if self.duration_label:
                text.append(" |")
        if self.duration_label:
            text.append(f" {self.duration_label}")
        if self.duration_label and self.model_call_count and self.output_tokens is not None and self.model_duration_ms > 0:
            rate = f"{self.output_tokens * 1000 / self.model_duration_ms:.1f}".removesuffix(".0")
            text.append(f" - {rate} tok/s")
        text.truncate(self.content_size.width, overflow="crop")
        return text

    def set_spinner(self, index: int | None) -> None:
        character = "-" if index is None else self.SPINNER_FRAMES[index % 4]
        if character != self.spinner:
            self.spinner = character
            self.refresh()

    def record_activity(self, kind: str) -> None:
        if kind == "tools":
            self.has_tools = True
        elif kind == "reasoning":
            self.has_reasoning = True
        elif kind == "read":
            self.has_reads = True
        elif kind == "skill":
            self.has_skills = True
        elif kind == "refused":
            self.has_refused = True
        self.refresh()

    def start_generation(self, info: dict[str, Any], started_at: float) -> None:
        self.started_at = started_at
        self.update_wait_animation()
        self.duration_ms = None
        self.model = info.get("model", "—")
        self.reasoning = info.get("reasoning", "—")
        self.update_elapsed()

    def record_model_call(self, call: dict[str, Any] | None) -> None:
        self.model_call_count += 1
        if call is None:
            self.output_tokens = None
        else:
            self.model = call["model"]
            self.reasoning = call["reasoning"]
            duration = self.model_duration_ms + call["duration_ms"]
            tokens = call.get("output_tokens")
            if duration > 2**64 - 1:
                self.output_tokens = None
            else:
                self.model_duration_ms = duration
                if self.output_tokens is None or tokens is None or self.output_tokens + tokens > 2**64 - 1:
                    self.output_tokens = None
                else:
                    self.output_tokens += tokens
        self.refresh()

    def update_details(self, info: dict[str, Any]) -> None:
        if self.started_at is not None:
            self.model = info.get("model", self.model)
            self.reasoning = info.get("reasoning", self.reasoning)
            self.refresh()

    def update_elapsed(self) -> None:
        duration = self.duration_ms
        if self.started_at is not None:
            duration = int((monotonic() - self.started_at) * 1000)
        label = format_duration(duration) if duration is not None else ""
        if label != self.duration_label:
            self.duration_label = label
            self.refresh()

    def finish_generation(self, metadata: dict[str, Any] | None = None) -> None:
        if metadata is not None:
            self.model = metadata["model"]
            self.reasoning = metadata["reasoning"]
            self.duration_ms = metadata["duration_ms"]
        elif self.started_at is not None:
            self.duration_ms = int((monotonic() - self.started_at) * 1000)
        self.started_at = None
        self.waiting_processes.clear()
        self.waiting_labels.clear()
        self.update_wait_animation()
        self.update_elapsed()
        self.refresh()

    def on_click(self, event: Click) -> None:
        event.stop()
        self.action_toggle()

    def action_toggle(self) -> None:
        self.post_message(self.Toggled())


class ActivitySeparator(Static):
    def render(self) -> Text:
        return Text("─" * self.content_size.width)


class TurnActivity(Vertical):
    DEFAULT_CSS = """
    TurnActivity { height: auto; margin: 0; padding: 0; }
    TurnActivity > .activity-separator { height: 1; margin: 0; color: $muted; }
    TurnActivity > .activity-content { height: auto; margin: 0 1 0 3; padding: 0 0 0 2; }
    TurnActivity > .process-wait-status { height: 1; margin: 0 1 0 5; padding: 0; color: $muted; }
    TurnActivity > .manual-refusals { height: auto; margin: 0 1 0 3; padding: 0 2 0 2; }
    TurnActivity > .refusals-heading, TurnActivity > .history-heading { height: 1; margin: 0 1 0 3; padding: 0 5 0 2; text-style: bold; }
    TurnActivity > Rule.approval-separator.-horizontal { height: 1; margin: 0 0 0 5; color: $muted; }
    ActivityToggle, ActivityToggle:hover, ActivityToggle:focus {
        height: 1; margin: 0 1 0 3; padding: 0; color: $muted; background: $background;
        background-tint: transparent; tint: transparent; text-style: none; pointer: pointer;
    }
    """

    def __init__(self) -> None:
        super().__init__()
        self.activity_separator = ActivitySeparator(classes="activity-separator")
        self.activity_separator.display = False
        self.body = Vertical(classes="activity-content")
        self.body.display = False
        self.refusals = Vertical(classes="manual-refusals")
        self.refusals.display = False
        self.refusals_heading = Static(Text("Refused tools", style=SLOP_COLOR), classes="refusals-heading")
        self.refusals_heading.display = False
        self.separator = Rule(line_style="solid", classes="approval-separator")
        self.separator.display = False
        self.history_heading = Static(Text("History", style=BOLD_MD), classes="history-heading")
        self.history_heading.display = False
        self.refused_views: list[Widget] = []
        self.toggle = ActivityToggle()
        self.pending: list[Widget] = []

    def compose(self) -> ComposeResult:
        yield self.activity_separator
        yield self.toggle
        yield self.refusals_heading
        yield self.refusals
        yield self.separator
        yield self.history_heading
        yield self.body
        yield self.toggle.wait_status

    def on_mount(self) -> None:
        if self.refused_views and not self.refusals.children:
            self.refusals.mount_all(self.refused_views)
        if self.pending:
            self.body.mount_all(self.pending)
            self.pending.clear()

    def add_activity(self, widget: Widget, kind: str) -> None:
        self.toggle.step_count += 1
        self.toggle.record_activity(kind)
        if self.body.is_mounted:
            self.body.mount(widget)
        else:
            self.pending.append(widget)

    def on_process_waiting(self, event: ProcessWaiting) -> None:
        event.stop()
        self.toggle.set_process_waiting(event.call_id, event.waiting, event.status)

    def add_refusal(self, widget: Widget) -> None:
        self.toggle.record_activity("refused")
        self.refused_views.append(widget)
        if self.refusals.is_mounted:
            self.refusals.mount(widget)
        self.update_refusal_visibility()

    def update_refusal_visibility(self) -> None:
        self.refusals.display = self.separator.display = self.toggle.expanded and bool(self.refused_views)
        self.refusals_heading.display = self.refusals.display
        self.history_heading.display = self.refusals.display
        if not self.toggle.expanded:
            for row in self.refusals.query(Collapsible):
                row.collapsed = True

    def on_activity_toggle_toggled(self, event: ActivityToggle.Toggled) -> None:
        event.stop()
        self.body.display = not self.body.display
        self.toggle.expanded = self.body.display
        self.update_refusal_visibility()
        self.toggle.refresh()
