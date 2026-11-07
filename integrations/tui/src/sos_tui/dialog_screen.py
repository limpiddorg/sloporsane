"""Shared visual conventions for application dialogs."""

from textual.screen import ModalScreen
from textual import events, on
from textual.binding import Binding
from textual.widgets import OptionList
from rich.table import Table
from rich.text import Text

from .config import BOLD_MD


def dialog_row(label: str, status: str = "", detail: str | None = None, *, detail_style: str = "", status_width: int = 9) -> Table:
    """Shared picker row: label, optional detail, and a right-aligned coral status."""
    row = Table.grid(expand=True, padding=(0, 1))
    row.add_column(ratio=1, no_wrap=True, overflow="ellipsis")
    cells = [Text(label)]
    if detail is not None:
        row.add_column(width=22, justify="left", no_wrap=True, overflow="ellipsis")
        cells.append(Text(detail, style=detail_style))
    row.add_column(width=status_width, justify="right", no_wrap=True)
    cells.append(Text(status, style=BOLD_MD))
    row.add_row(*cells)
    return row


class DialogScreen(ModalScreen[None]):
    BINDINGS = [Binding("up", "navigate_selection(-1)", show=False, priority=True),
                Binding("down", "navigate_selection(1)", show=False, priority=True)]

    _last_selection: OptionList | None = None

    @on(events.DescendantFocus)
    def remember_selection(self, event: events.DescendantFocus) -> None:
        if isinstance(event.widget, OptionList):
            self._last_selection = event.widget

    def active_selection(self) -> OptionList | None:
        lists = [listing for listing in self.query(OptionList)
                 if not listing.is_disabled and listing.display
                 and all(parent.display for parent in listing.ancestors)]
        if isinstance(self.focused, OptionList) and self.focused in lists:
            return self.focused
        if self._last_selection in lists:
            return self._last_selection
        return lists[0] if lists else None

    def check_action(self, action: str, parameters: tuple[object, ...]) -> bool | None:
        if action == "navigate_selection":
            return self.active_selection() is not None
        return super().check_action(action, parameters)

    def action_navigate_selection(self, direction: int) -> None:
        self.move_selection(direction)

    def move_selection(self, direction: int, *, focus: bool = True) -> None:
        listing = self.active_selection()
        if listing is None:
            return
        if focus:
            listing.focus()
        if direction < 0:
            listing.action_cursor_up()
        else:
            listing.action_cursor_down()

    DEFAULT_CSS = """
    DialogScreen { align: center middle; background: $background 70%; }
    DialogScreen .popup-dialog { width: 85%; max-width: 100; height: auto; max-height: 95%;
        background: $background; border: solid $muted; padding: 1 2;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $background; }
    DialogScreen .popup-heading { height: 1; text-style: bold; color: $bold-md; }
    DialogScreen .popup-section { height: 1; color: $title-md; }
    DialogScreen .popup-picker { height: auto; border: solid $muted; padding: 0;
        background: $background; }
    DialogScreen .popup-columns { height: 1; color: $title-md;
        background: $content-background; padding: 0 1 0 0; }
    DialogScreen Rule.popup-separator.-horizontal { height: 1; margin: 0; padding: 0;
        color: $muted; background: $background; }
    DialogScreen .popup-status { height: auto; color: $muted; }
    DialogScreen .popup-list, DialogScreen .popup-list:focus {
        border: solid $muted; padding: 0; background: $background; color: $foreground;
        background-tint: transparent;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $background; }
    DialogScreen .popup-list > .option-list--option { padding: 0; }
    DialogScreen .popup-list > .option-list--option-highlighted,
    DialogScreen .popup-list > .option-list--option-hover-highlighted,
    DialogScreen .popup-list:focus > .option-list--option-highlighted {
        background: $depth-md; color: $foreground; text-style: bold; }
    DialogScreen .popup-list > .option-list--option-hover { background: $content-background; color: $foreground; }
    DialogScreen .popup-picker > .popup-list,
    DialogScreen .popup-picker > .popup-list:focus { border: none; }
    DialogScreen Input { background: $input-background; color: $foreground; border: solid $muted; }
    DialogScreen Input:focus { border: solid $muted; }
    DialogScreen Button {
        width: auto; min-width: 0; height: 1; padding: 0 1;
        border: none !important; text-style: bold !important;
        background: $content-background !important; color: $foreground !important;
        background-tint: transparent !important; tint: transparent !important;
        text-opacity: 100%; opacity: 1;
    }
    DialogScreen Button:hover { background: $input-background !important; }
    DialogScreen Button:focus { background: $depth-md !important; color: $foreground !important;
        text-style: bold !important; }
    DialogScreen Button.-active {
        background: #3A4655 !important; color: $foreground !important; text-style: bold !important;
    }
    DialogScreen Button:disabled {
        background: $content-background !important; color: $muted !important;
        text-style: bold !important; text-opacity: 100%;
    }
    """
