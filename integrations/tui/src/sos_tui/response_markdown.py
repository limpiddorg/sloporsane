"""Response Markdown with an explicit code palette, independent of Textual's theme."""

from time import monotonic
from rich.text import Text
from textual.content import Content
from textual.widgets import Markdown, Static
from textual.widgets._markdown import MarkdownFence, MarkdownBlock

from .config import MUTED_COLOR
from .code_colors import highlighted_code
from .turn_activity import format_duration, reasoning_label


class GenerationFooter(Static):
    DEFAULT_CSS = """
    GenerationFooter { height: 1; width: 1fr; text-align: right; color: $muted; margin: 0; padding: 0; }
    """

    def __init__(self, metadata: dict, started_at: float | None = None, prefix: Text | None = None) -> None:
        super().__init__()
        self.prefix = prefix
        self.started_at = started_at
        self.rendered_label: str | None = None
        self.set_generation(metadata)

    def set_generation(self, metadata: dict) -> None:
        self.metadata = dict(metadata)
        self.update_elapsed()

    def update_elapsed(self) -> None:
        duration = self.metadata.get("duration_ms")
        if self.started_at is not None:
            duration = int((monotonic() - self.started_at) * 1000)
        text = f"{self.metadata['model']} | {reasoning_label(self.metadata.get('reasoning'))}"
        if duration is not None:
            text += f" | {format_duration(duration)}"
        if text != self.rendered_label:
            self.rendered_label = text
            label = Text(text, style=MUTED_COLOR)
            if self.prefix is not None:
                label = self.prefix + Text(" | ", style=MUTED_COLOR) + label
            self.update(label)

    def finish_generation(self, metadata: dict | None = None) -> None:
        if metadata is not None:
            self.metadata = dict(metadata)
        if self.started_at is not None and (metadata is None or metadata.get("duration_ms") is None):
            self.metadata["duration_ms"] = int((monotonic() - self.started_at) * 1000)
        self.started_at = None
        self.update_elapsed()


class CodeFence(MarkdownFence):
    @classmethod
    def highlight(cls, code: str, language: str, ansi: bool = False, dark: bool = False) -> Content:
        return Content.from_rich_text(highlighted_code(code, language=language))


class ResponseMarkdown(Markdown):
    BLOCKS = {**Markdown.BLOCKS, "fence": CodeFence, "code_block": CodeFence}

    def update(self, markdown):
        from .stable_scroll import StableScroll
        viewport = next((parent for parent in self.ancestors if isinstance(parent, StableScroll)), None)
        if viewport is not None:
            viewport.retain_reading_position()
        return super().update(markdown)

    def __init__(self, *args, generation: dict | None = None, footer_prefix: Text | None = None, **kwargs) -> None:
        self.generation = generation
        self.footer_prefix = footer_prefix
        self.external_footer: GenerationFooter | None = None
        super().__init__(*args, **kwargs)

    def separate_generation_footer(self) -> GenerationFooter | None:
        """Let the history place a turn drawer between the text and its metadata."""
        if self.generation is None:
            return None
        if self.external_footer is None:
            self.external_footer = GenerationFooter(self.generation, prefix=self.footer_prefix)
            for footer in self.query(GenerationFooter):
                footer.display = False
                footer.remove()
        return self.external_footer

    def on_markdown_table_of_contents_updated(self, event: Markdown.TableOfContentsUpdated) -> None:
        # This event arrives after Markdown has mounted its parsed blocks.
        # Keep spacing inside the document, but not outside its first/last block.
        if event.markdown is self and self.children:
            for block in self.query(MarkdownBlock):
                block.scroll_key = (self, type(block), block.source_range)
            first, last = self.children[0], self.children[-1]
            margin = first.styles.margin
            first.styles.margin = (0, margin.right, margin.bottom, margin.left)
            margin = last.styles.margin
            last.styles.margin = (margin.top, margin.right, 0, margin.left)
            if self.generation is not None and self.external_footer is None and not self.query(GenerationFooter):
                self.mount(GenerationFooter(self.generation, prefix=self.footer_prefix))
