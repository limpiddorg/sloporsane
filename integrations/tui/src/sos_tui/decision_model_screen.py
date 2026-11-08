"""Draft-only decision API selection. Credentials never enter conversation messages."""

from dataclasses import dataclass, field
from urllib.parse import urlsplit

from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Input, OptionList, Static
from textual.widgets.option_list import Option

from .dialog_screen import DialogScreen, dialog_row


PROTOCOLS = (("openai_decisions", "OpenAI Decisions", "https://api.openai.com/v1/decisions"),
             ("typesafe", "TypeSafe", "https://api.typesafe.ai/v1/systemone"))


@dataclass
class DecisionModelDraft:
    config: dict[str, str]
    key: str = field(default="", repr=False)


class DecisionModelScreen(DialogScreen):
    BINDINGS = [Binding("escape", "close", show=False, priority=True)]
    DEFAULT_CSS = """
    DecisionModelScreen .popup-dialog { overflow-y: auto; }
    DecisionModelScreen #decision-protocols { height: 4; margin: 1 0; }
    DecisionModelScreen #decision-fields { height: auto; display: none; }
    DecisionModelScreen #decision-fields Static { height: auto; }
    DecisionModelScreen Input, DecisionModelScreen Input:focus {
        height: 1; min-height: 1; border: none; padding: 0 1; margin: 0 0 1 0; }
    DecisionModelScreen #decision-actions { height: 1; }
    DecisionModelScreen #decision-status { margin-top: 1; }
    """

    def __init__(self, config: dict[str, str], key: str = "", *, saved_identity: tuple[str, str], has_saved_key: bool) -> None:
        super().__init__()
        self.initial = dict(config)
        self.initial_key = key
        self.saved_identity = saved_identity
        self.has_saved_key = has_saved_key
        self.protocol = config["protocol"]
        self.editing = False

    def compose(self) -> ComposeResult:
        with Vertical(classes="popup-dialog"):
            yield Static("Change decision model", classes="popup-heading")
            yield OptionList(*(Option(dialog_row(label), id=protocol) for protocol, label, _ in PROTOCOLS),
                             id="decision-protocols", classes="popup-list")
            with Vertical(id="decision-fields"):
                yield Static("", id="decision-api", classes="popup-section")
                yield Static("Endpoint URL (complete API endpoint)")
                yield Input(id="decision-endpoint", placeholder="https://…")
                yield Static("Model ID")
                yield Input(id="decision-model", placeholder="Model identifier")
                yield Static("API key")
                yield Input(id="decision-key", password=True, placeholder="API key")
                yield Static("", id="decision-key-hint", markup=False)
                with Horizontal(id="decision-actions"):
                    yield Button("Use model", id="decision-use")
            yield Static("↑/↓ Select API · Enter Continue · Esc Cancel", id="decision-status",
                         classes="popup-status", markup=False)

    def on_mount(self) -> None:
        listing = self.query_one(OptionList)
        listing.highlighted = next((i for i, item in enumerate(PROTOCOLS) if item[0] == self.protocol), 0)
        listing.focus()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        self.protocol, label, endpoint = PROTOCOLS[event.option_index]
        same = self.protocol == self.initial["protocol"]
        self.query_one("#decision-api", Static).update(label)
        self.query_one("#decision-endpoint", Input).value = self.initial["endpoint"] if same else endpoint
        self.query_one("#decision-model", Input).value = self.initial["model"] if same else ""
        self.query_one("#decision-key", Input).value = self.initial_key if same else ""
        self.query_one(OptionList).display = False
        self.query_one("#decision-fields").display = True
        self.editing = True
        self.update_key_hint()
        self.query_one("#decision-endpoint").focus()
        self.status("Tab Next · Enter Continue · Esc Back · Changes apply only after Save in AI Check")

    def status(self, text: str) -> None:
        self.query_one("#decision-status", Static).update(text)

    def reusable_key(self) -> bool:
        endpoint = self.query_one("#decision-endpoint", Input).value.strip()
        return self.has_saved_key and (self.protocol, endpoint) == self.saved_identity

    def update_key_hint(self) -> None:
        hint = "Leave blank to keep the saved key for this API and endpoint." if self.reusable_key() else "A key is required for this API and endpoint."
        self.query_one("#decision-key-hint", Static).update(hint)

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id == "decision-endpoint" and self.editing:
            self.update_key_hint()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        event.stop()
        fields = ("decision-endpoint", "decision-model", "decision-key")
        if event.input.id in fields[:-1]:
            self.query_one(f"#{fields[fields.index(event.input.id) + 1]}").focus()
        else:
            self.apply_model()

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        if event.button.id == "decision-use":
            self.apply_model()

    def apply_model(self) -> None:
        endpoint = self.query_one("#decision-endpoint", Input).value.strip()
        model = self.query_one("#decision-model", Input).value.strip()
        key = self.query_one("#decision-key", Input).value.strip()
        try:
            url = urlsplit(endpoint)
            _ = url.port  # Validate malformed port syntax too.
            valid = (len(endpoint) <= 4096 and url.hostname and not url.username and not url.password
                     and "@" not in endpoint and "#" not in endpoint
                     and (url.scheme == "https" or (url.scheme == "http" and url.hostname in ("localhost", "127.0.0.1", "::1")))
                     and not any(char.isspace() or ord(char) < 32 for char in endpoint))
        except ValueError:
            valid = False
        if not valid:
            self.status("Use HTTPS, or HTTP on loopback, without URL credentials or fragments.")
            self.query_one("#decision-endpoint").focus()
            return
        if not model or len(model) > 256 or any(char.isspace() or ord(char) < 32 for char in model):
            self.status("Enter a model ID without whitespace (up to 256 characters).")
            self.query_one("#decision-model").focus()
            return
        if not key and not self.reusable_key():
            self.status("Enter an API key for this API and endpoint.")
            self.query_one("#decision-key").focus()
            return
        if len(key) > 16384 or any(char.isspace() or ord(char) < 32 for char in key):
            self.status("Enter an API key without whitespace.")
            self.query_one("#decision-key").focus()
            return
        result = DecisionModelDraft({"protocol": self.protocol, "endpoint": endpoint, "model": model}, key)
        self.clear_secrets()
        self.dismiss(result)

    def clear_secrets(self) -> None:
        self.initial_key = ""
        for field in self.query("#decision-key"):
            field.value = ""

    def on_unmount(self) -> None:
        self.clear_secrets()

    def action_close(self) -> None:
        self.clear_secrets()
        if self.editing:
            self.editing = False
            self.query_one("#decision-fields").display = False
            listing = self.query_one(OptionList)
            listing.display = True
            listing.focus()
            self.status("↑/↓ Select API · Enter Continue · Esc Cancel")
        else:
            self.dismiss(None)
