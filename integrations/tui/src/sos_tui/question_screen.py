"""A consequential choice requested by the agent during planning."""

from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Vertical
from textual.widgets import OptionList, Static

from .dialog_screen import DialogScreen


class QuestionScreen(DialogScreen):
    BINDINGS = [Binding("escape", "dismiss_question", "Back")]

    def __init__(self, event: dict) -> None:
        super().__init__()
        self.question_id = event["questionId"]
        self.session_id = event["sessionId"]
        self.question = event["question"]
        self.options = event["options"]

    def compose(self) -> ComposeResult:
        with Vertical(classes="popup-dialog"):
            yield Static(self.question, classes="popup-status")
            yield OptionList(*self.options, classes="popup-list")
            yield Static("↑/↓ Choose · Enter Answer · Esc Dismiss", classes="popup-status")

    def on_mount(self) -> None:
        self.query_one(OptionList).focus()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        self.dismiss(event.option_index)

    def action_dismiss_question(self) -> None:
        self.dismiss(None)
