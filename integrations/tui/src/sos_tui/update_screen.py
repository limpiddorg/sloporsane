"""An update is opt-in because installation closes all of this user's SOS sessions."""
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Static

from .dialog_screen import DialogScreen
from .updates import VERSION


class UpdateScreen(DialogScreen):
    BINDINGS = [("escape", "dismiss_update", "Not now"),
                ("left,right", "switch_choice", "Choose")]

    def __init__(self, version):
        super().__init__()
        self.version = version

    def compose(self):
        with Vertical(classes="popup-dialog"):
            yield Static("SOS update available", classes="popup-heading")
            yield Static(f"Version {VERSION.removeprefix('v')} → {self.version.removeprefix('v')}", markup=False)
            yield Static("All running SOS applications and the SOS daemon for your user will be closed. "
                         "SOS will reopen in this terminal after installation.", classes="popup-status")
            with Horizontal(classes="update-choices"):
                yield Button("Not now", id="update-later")
                yield Button("Update", id="update-install")

    DEFAULT_CSS = "UpdateScreen .update-choices { height: 1; margin-top: 1; }"

    def on_mount(self):
        self.query_one("#update-later").focus()

    def action_switch_choice(self):
        self.query_one("#update-install" if self.query_one("#update-later").has_focus else "#update-later").focus()

    def action_dismiss_update(self):
        self.dismiss(False)

    def on_button_pressed(self, event):
        event.stop()
        self.dismiss(event.button.id == "update-install")
