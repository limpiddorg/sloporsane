"""Chat drawers that toggle by mouse without a keyboard-focus round trip."""

from textual.widgets import Collapsible


class ChatCollapsible(Collapsible):
    DEFAULT_CSS = """
    ChatCollapsible { border: none; padding: 0; }
    ChatCollapsible > CollapsibleTitle { padding: 0 1 0 0; }
    ChatCollapsible > Contents { padding: 0 0 0 4; }
    """

    def on_mount(self) -> None:
        # Keep the title keyboard-focusable, but don't restyle the entire drawer
        # on a mouse click just to hand focus back to the composer afterwards.
        self.query_one("CollapsibleTitle").FOCUS_ON_CLICK = False


class CompactionDrawer(ChatCollapsible):
    """Inline compaction checkpoint, without Discussion's horizontal border."""
    DEFAULT_CSS = """
    CompactionDrawer { height: auto; border: none; margin: 0 1 0 3; padding: 0; }
    CompactionDrawer > CollapsibleTitle,
    CompactionDrawer > CollapsibleTitle:hover,
    CompactionDrawer > CollapsibleTitle:focus { color: $slop; text-style: bold; padding: 0; margin: 0; }
    CompactionDrawer > Contents { padding: 0 0 0 4; }
    """

    def __init__(self):
        super().__init__(title="Compaction", collapsed=True, collapsed_symbol="▸", expanded_symbol="▾")
        self.running = True
        self.frame = 0
        self.summary = None
        self.summary_view = None

    def on_mount(self):
        super().on_mount()
        self.set_interval(.2, self.animate)
        if self.summary is not None:
            self.call_after_refresh(self.finish, self.summary)

    def animate(self):
        if self.running:
            self.frame += 1
            self.title = "Compaction " + ("-", "\\", "|", "/")[self.frame % 4]

    def set_result(self, text):
        self.summary = text
        self.running = False
        self.title = "Compaction"
        if self.is_mounted:
            self.call_after_refresh(self.finish, text)

    async def finish(self, text):
        from .response_markdown import ResponseMarkdown
        self.summary = text
        self.running = False
        self.title = "Compaction"
        # Deferred completions can outlive a step view. Never query or mount
        # children of a drawer that navigation has already removed.
        if not self.is_attached:
            return
        contents = next(iter(self.query(Collapsible.Contents)), None)
        if contents is None or not contents.is_attached:
            return
        if self.summary_view is not None:
            if self.summary_view._markdown != text:
                self.summary_view.update(text)
            return
        self.summary_view = ResponseMarkdown(text)
        await contents.mount(self.summary_view)


def compaction_record(message):
    if message.get("compaction") is not None:
        return "completed", "\n".join(p["text"] for p in message["content"] if p["type"] == "text")
    if message.get("id", "").startswith("compaction-status-"):
        text = "\n".join(p["text"] for p in message["content"] if p["type"] == "text")
        return text.split("\n", 1)[0], text
    return None
