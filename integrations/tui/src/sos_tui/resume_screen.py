"""Session picker with keyboard navigation and a read-only prompt preview."""

from datetime import datetime
from typing import Any

from rich.align import Align
from rich.table import Table
from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Vertical, VerticalScroll
from textual.message import Message
from textual.widgets import OptionList, Rule, Static
from textual.widgets.option_list import Option
from .dialog_screen import DialogScreen


class ResumeScreen(DialogScreen):
    DEFAULT_CSS = """
    ResumeScreen #resume-dialog { height: 80%; }
    ResumeScreen #resume-picker { height: 1fr; margin: 1 0; }
    ResumeScreen #resume-sessions { height: 1fr; overflow-y: scroll; }
    ResumeScreen #resume-sessions > .option-list--option-disabled,
    ResumeScreen #resume-sessions > .option-list--separator { color: $misc-md; }
    ResumeScreen #resume-preview-scroll { height: 4; max-height: 25%; min-height: 1; margin: 1 0;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $background; }
    ResumeScreen #resume-preview { height: auto; color: $foreground; }
    """

    class PreviewRequested(Message):
        def __init__(self, session_id: str) -> None:
            super().__init__()
            self.session_id = session_id

    class ResumeRequested(Message):
        def __init__(self, session_id: str) -> None:
            super().__init__()
            self.session_id = session_id

    def __init__(self) -> None:
        super().__init__()
        self.selected_id: str | None = None
        self.attaching = False

    def compose(self) -> ComposeResult:
        with Vertical(id="resume-dialog", classes="popup-dialog"):
            yield Static("Resume Session", id="resume-heading", classes="popup-heading")
            with Vertical(id="resume-picker", classes="popup-picker"):
                yield Static(self.session_row("Title", "Last activity"), id="resume-columns", classes="popup-columns")
                yield Rule(line_style="solid", classes="popup-separator")
                yield OptionList(id="resume-sessions", classes="popup-list")
            yield Static("Last user request", id="resume-preview-heading", classes="popup-section")
            with VerticalScroll(id="resume-preview-scroll"):
                yield Static("", id="resume-preview", markup=False)
            yield Static("Loading sessions…", id="resume-status", classes="popup-status", markup=False)

    @staticmethod
    def session_row(title: str, activity: str) -> Table:
        row = Table.grid(expand=True, padding=(0, 1))
        row.add_column(ratio=1, no_wrap=True, overflow="ellipsis")
        row.add_column(width=17, justify="right", no_wrap=True)
        row.add_row(Text(title), Text(activity))
        return row

    def set_sessions(self, sessions: list[dict[str, Any]]) -> None:
        options = self.query_one(OptionList)
        self.selected_id = None
        self.query_one("#resume-preview", Static).update("")
        options.clear_options()
        recent = sorted((item for item in sessions if item.get("terminalLastOpenedAt", 0)),
                        key=lambda item: item["terminalLastOpenedAt"], reverse=True)
        others = sorted((item for item in sessions if not item.get("terminalLastOpenedAt", 0)),
                        key=lambda item: item["updatedAt"], reverse=True)
        if recent:
            options.add_option(Option(Align.center(Text("Invoked from this terminal session", style="bold")), disabled=True))
        self.add_sessions(options, recent)
        if recent and others:
            options.add_option(None)
        self.add_sessions(options, others)
        self.set_status("↑/↓ Select · Enter Resume · Esc Close" if sessions else "No saved sessions · Esc Close")
        options.highlighted = (1 if recent else 0) if sessions else None
        options.focus()

    def add_sessions(self, options: OptionList, sessions: list[dict[str, Any]]) -> None:
        for session in sessions:
            timestamp = datetime.fromtimestamp(session["updatedAt"] / 1000).strftime("%H:%M  %d/%m/%Y")
            row = self.session_row(" ".join(session["title"].split()) or "Untitled session", timestamp)
            options.add_option(Option(row, id=session["id"]))

    def set_status(self, text: str) -> None:
        self.query_one("#resume-status", Static).update(text)

    def set_preview(self, session_id: str, text: str) -> None:
        if session_id != self.selected_id:
            return
        self.query_one("#resume-preview", Static).update(text or "No user request yet")
        self.query_one("#resume-preview-scroll", VerticalScroll).scroll_home(animate=False)

    def on_option_list_option_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        event.stop()
        if event.option.disabled or event.option.id is None:
            return
        if event.option_index == 1 and event.option_list.get_option_at_index(0).disabled:
            event.option_list.scroll_home(animate=False, immediate=True)
        self.selected_id = event.option.id
        self.query_one("#resume-preview", Static).update("Loading…")
        self.post_message(self.PreviewRequested(self.selected_id))

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        if self.attaching or event.option.disabled or event.option.id is None:
            return
        self.attaching = True
        self.query_one(OptionList).disabled = True
        self.set_status("Resuming session…")
        self.post_message(self.ResumeRequested(event.option.id))

    def attach_failed(self, error: str) -> None:
        self.attaching = False
        self.set_status(error)
        self.query_one(OptionList).disabled = False
        self.query_one(OptionList).focus()
