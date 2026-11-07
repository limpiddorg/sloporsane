"""Agent information, hiding lower-priority fields when space runs out."""

from typing import Any

from rich.cells import cell_len
from textual.app import ComposeResult
from textual.containers import Horizontal
from textual.widgets import Static

from .mode_button import ModeButton
from .history_loading_bar import HistoryLoadingBar
from .confirmation_bar import ConfirmationBar, ConfirmationKind
from .turn_activity import reasoning_label


class AgentInfoBar(Horizontal):
    BUTTON_WIDTH = 15
    DEFAULT_CSS = """
    AgentInfoBar { height: 1; padding: 0 0 0 1; overflow: hidden; }
    AgentInfoBar.-confirming { padding: 0; }
    AgentInfoBar > Horizontal { height: 1; overflow: hidden; }
    AgentInfoBar Static { width: auto; height: 1; color: $muted; }
    AgentInfoBar #mode-actions { align: right top; }
    AgentInfoBar #mode-button { width: 23; min-width: 0; height: 1; border: none; padding: 0;
                               content-align: center middle; background: $sane; color: $background; }
    """

    def __init__(self, *, show_mode_action: bool = True, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self.show_mode_action = show_mode_action
        self.labels = {"model-info": "—", "reasoning-info": " | —",
                       "context-info": "", "context-percent": "-"}
        self.history_loading = False

    def compose(self) -> ComposeResult:
        with Horizontal(id="model-details"):
            yield Static(self.labels["model-info"], id="model-info", markup=False)
            yield Static(self.labels["reasoning-info"], id="reasoning-info", markup=False)
        with Horizontal(id="context-details"):
            yield Static(self.labels["context-info"], id="context-info", markup=False)
            yield Static(self.labels["context-percent"], id="context-percent", markup=False)
            yield HistoryLoadingBar(id="history-progress")
        with Horizontal(id="mode-actions"):
            yield ModeButton(id="mode-button")
        yield ConfirmationBar(id="confirmation-bar")

    def show_confirmation(self, kind: ConfirmationKind, deadline: float | None, duration: float) -> None:
        self.query_one(ConfirmationBar).start(kind, deadline, duration)
        self.query_one(ModeButton).set_confirmation_active(True)
        self.reflow()

    def hide_confirmation(self, kind: ConfirmationKind) -> None:
        bar = self.query_one(ConfirmationBar)
        if bar.kind == kind:
            bar.stop()
            self.query_one(ModeButton).set_confirmation_active(False)
            self.reflow()

    def on_mode_button_confirmation_frame(self, event: ModeButton.ConfirmationFrame) -> None:
        event.stop()
        bar = self.query_one(ConfirmationBar)
        if bar.deadline is not None:
            bar.refresh()  # Share the pulse clock; don't reflow the row on each frame.

    def set_mode(self, mode: str) -> None:
        self.query_one(ModeButton).set_mode(mode)

    def set_history_loading(self, loading: bool) -> None:
        self.history_loading = loading
        bar = self.query_one(HistoryLoadingBar)
        if loading:
            bar.start()
        else:
            bar.stop()
        self.reflow()

    def update_info(self, info: dict[str, Any]) -> None:
        previous = self.labels.copy()
        if "model" in info and "reasoning" in info:
            self.labels["model-info"] = info["model"]
            label = reasoning_label(info["reasoning"])
            self.labels["reasoning-info"] = f" | {label}"
        if "last_turn_used_context" in info and "total_context" in info:
            used, total = info["last_turn_used_context"], info["total_context"]
            used_label = "—" if used is None else f"{used:,}".replace(",", " ")
            total_label = "—" if total is None else f"{total:,}".replace(",", " ")
            unknown = used is None and total is None
            self.labels["context-info"] = "" if unknown else f"{used_label} / {total_label}"
            self.labels["context-percent"] = "-" if unknown else (
                "[—]" if used is None or not total else f"[{(200 * used + total) // (2 * total)}%]"
            )
        changed = {id: label for id, label in self.labels.items() if label != previous[id]}
        for id, label in changed.items():
            self.query_one("#" + id, Static).update(label)
        if changed:
            self.reflow()

    def on_resize(self) -> None:
        self.reflow()

    def reflow(self) -> None:
        confirmation = self.query_one(ConfirmationBar)
        confirming = confirmation.kind is not None
        self.set_class(confirming, "-confirming")
        confirmation.display = confirming
        for id in ("model-details", "context-details", "mode-actions"):
            self.query_one("#" + id).display = not confirming
        if confirming:
            return
        width = self.content_size.width
        if not width:
            return
        button_width = min(self.BUTTON_WIDTH, width) if self.show_mode_action else 0
        self.query_one("#mode-actions").display = self.show_mode_action
        visible = {id for id, label in self.labels.items() if label}
        if self.history_loading:
            visible.difference_update(("context-info", "context-percent"))

        def sizes() -> tuple[int, int, int]:
            left = sum(cell_len(self.labels[id]) for id in ("model-info", "reasoning-info") if id in visible)
            center = sum(cell_len(self.labels[id]) for id in ("context-info", "context-percent") if id in visible)
            if "context-info" in visible and "context-percent" in visible:
                center += 1
            if self.history_loading:
                center = min(24, max(0, width - button_width - 1))
                if center < 3:
                    center = 0
            # One space between each occupied group: model, context, button.
            required = left + center + button_width + bool(left) + (bool(center) if button_width else 0)
            return left, center, required

        for id in ("context-info", "reasoning-info", "context-percent", "model-info"):
            if sizes()[2] <= width:
                break
            visible.discard(id)
        left, center, _ = sizes()
        if self.history_loading:
            # Prefer a centered loader over long model labels.
            for id in ("reasoning-info", "model-info"):
                if left + bool(left) <= (width - center) // 2:
                    break
                visible.discard(id)
                left, center, _ = sizes()
        for id in self.labels:
            self.query_one("#" + id).display = id in visible
        self.query_one("#context-percent").styles.margin = (0, 0, 0, int("context-info" in visible))
        # Keep context centered where possible, moving it only to avoid overlap.
        center_start = max(left + bool(left), min((width - center) // 2, width - button_width - 1 - center)) if center else width - button_width
        self.query_one("#model-details").styles.width = center_start
        self.query_one("#context-details").styles.width = center
        self.query_one("#context-details").display = bool(center)
        self.query_one(HistoryLoadingBar).display = self.history_loading and bool(center)
        self.query_one("#mode-actions").styles.width = width - center_start - center
        self.query_one("#mode-button").styles.width = button_width
