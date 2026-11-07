"""A message editor that grows with its wrapped content."""

from textual.binding import Binding
from textual import events
from textual.message import Message
from textual.widgets import TextArea
from textual.document._document import Selection
from textual.document._edit import Edit
from textual.strip import Strip
from textual.geometry import Offset
from rich.segment import Segment
from rich.style import Style

from .command_suggestions import CommandSuggestions
from .skills import completion_token, skill_command_chain, skill_reference_spans
from .config import ALGAE_MD, TITLE_MD, TEXT_COLOR, MUTED_COLOR, INPUT_QUOTE_BACKGROUND_COLOR
from .paste_blocks import Paste, PathPaste, PasteBlock, PasteEdit, compact_paste, expand_range
from .paste_preview import PastePreview
from .images import ImagePaste, pasted_paths, clipboard_image, check_budget


class MessageInput(TextArea):
    DEFAULT_CSS = "MessageInput .text-area--suggestion { color: $muted; }"
    MAX_VISIBLE_LINES = 6
    BINDINGS = [
        Binding("ctrl+v", "paste_image", show=False, priority=True),
    ]

    def __init__(self, *args, suggestions: CommandSuggestions | None = None, **kwargs) -> None:
        self.suggestions = suggestions
        self.paste_blocks: tuple[PasteBlock, ...] = ()
        self.pasted_ranges: tuple[tuple[int, int], ...] = ()
        super().__init__(*args, **kwargs)

    @property
    def expanded_text(self) -> str:
        return expand_range(self.text, self.paste_blocks, 0, len(self.text))

    @property
    def command_text(self) -> str:
        """Only a command token typed outside pasted spans can be interpreted."""
        text = self.text.lstrip()
        if not text.startswith("/"):
            return ""
        start = len(self.text) - len(text)
        end = start + len(text.split(maxsplit=1)[0])
        if any(left < end and right > start for left, right in self.pasted_ranges):
            return ""
        return self.expanded_text.strip()

    @property
    def selected_text(self) -> str:
        start, end = sorted(self.document.get_index_from_location(location) for location in self.selection)
        return expand_range(self.text, self.paste_blocks, start, end)

    @property
    def images(self) -> tuple[ImagePaste, ...]:
        return tuple(block.paste for block in self.paste_blocks if isinstance(block.paste, ImagePaste))

    def add_images(self, images: tuple[ImagePaste, ...]) -> None:
        self.add_attachments(images)

    def add_attachments(self, attachments: tuple[ImagePaste | PathPaste, ...]) -> None:
        if self.read_only or self.disabled:
            return
        start, end = sorted(self.document.get_index_from_location(location) for location in self.selection)
        retained = tuple(block.paste for block in self.paste_blocks if isinstance(block.paste, ImagePaste)
                         and not (start < block.end and end > block.start))
        images = tuple(item for item in attachments if isinstance(item, ImagePaste))
        check_budget(retained + images)
        self.history.checkpoint()
        separate_paths = any(isinstance(item, PathPaste) for item in attachments)
        for index, item in enumerate(attachments):
            if index and separate_paths:
                self.edit(PasteEdit(Edit(" ", *self.selection, False), pasted=True))
            self.edit(PasteEdit(Edit(item.label, *self.selection, False), item))
        self.history.checkpoint()

    def restore_images(self, text: str, images: tuple[ImagePaste, ...], location=(0, 0), *, pasted: bool = False) -> None:
        # Restore captured payloads; only labels within the recovered draft identify images.
        self.move_cursor(location)
        for image in images:
            before, found, text = text.partition(image.label)
            if not found:
                text = before
                before = ""
            self.edit(PasteEdit(Edit(before, *self.selection, False), pasted=pasted))
            self.edit(PasteEdit(Edit(image.label, *self.selection, False), image))
        self.edit(PasteEdit(Edit(text, *self.selection, False), pasted=pasted))

    async def action_paste_image(self) -> None:
        if self.read_only or self.disabled:
            return
        driver = self.app._driver
        if driver is None or driver.is_headless or driver.is_web:
            await self.app.show_input_error("Native clipboard images require a local terminal")
            return
        try:
            self.add_images((await clipboard_image(),))
        except (OSError, ValueError, TimeoutError) as error:
            await self.app.show_input_error(str(error))

    def load_text(self, text: str) -> None:
        self.paste_blocks = ()
        self.pasted_ranges = ()
        super().load_text(text)

    def refresh_skill_references(self) -> None:
        # TextArea caches rendered strips independently of its document text.
        self._line_cache.clear()
        self.refresh()

    def get_line(self, line_index: int):
        line = super().get_line(line_index)
        start = self.document.get_index_from_location((line_index, 0))
        if self.suggestions is not None and not self._is_quote_row(line_index):
            for left, right in skill_reference_spans(line.plain, self.suggestions.skills()):
                # Preview labels are paste atoms, not editable skill references.
                if not any(block.start < start + right and block.end > start + left
                           for block in self.paste_blocks):
                    line.stylize(ALGAE_MD, left, right)
        if not self.show_line_numbers and (line.plain == ">" or line.plain.startswith("> ")):
            # Preserve prefix width so editing, hit testing and copying use Markdown positions.
            line.plain = " " + line.plain[1:]
        for block in self.paste_blocks:
            if start <= block.start < start + len(line):
                line.stylize(TITLE_MD, block.start - start, block.end - start)
        return line

    @property
    def gutter_width(self) -> int:
        # Let the editor account for the quote bar in wrapping and hit testing.
        return super().gutter_width if self.show_line_numbers else 3

    def _is_quote_row(self, row: int) -> bool:
        text = self.document.get_line(row)
        return text == ">" or text.startswith("> ")

    def _visual_prefix_width(self, offset: int) -> int:
        rows = self.wrapped_document._offset_to_line_info
        if 0 <= offset < len(rows):
            row, section = rows[offset]
            if self._is_quote_row(row):
                # The first section already contains the two Markdown prefix cells.
                return 1 if section == 0 else 3
        return 0

    @property
    def cursor_screen_offset(self) -> Offset:
        offset = super().cursor_screen_offset
        if not self.show_line_numbers:
            offset += Offset(self._visual_prefix_width(self._cursor_offset[1]) - self.gutter_width, 0)
        return offset

    def get_target_document_location(self, event):
        location = super().get_target_document_location(event)
        if not self.show_line_numbers:
            y = event.y + self.scroll_offset.y - self.gutter.top
            location = self.wrapped_document.offset_to_location(Offset(
                event.x + self.scroll_offset.x - self.gutter.left - self._visual_prefix_width(y), y))
        return location

    def render_line(self, y: int) -> Strip:
        strip = super().render_line(y)
        if self.show_line_numbers:
            return strip
        offset = y + self.scroll_offset.y
        rows = self.wrapped_document._offset_to_line_info
        quoted = False
        if 0 <= offset < len(rows):
            row, _ = rows[offset]
            quoted = self._is_quote_row(row)
        if not quoted:
            return strip
        theme = self._theme
        base = theme.base_style if theme and theme.base_style is not None else self.rich_style
        quote_style = Style(color=TEXT_COLOR, bgcolor=INPUT_QUOTE_BACKGROUND_COLOR)
        # Preserve selection and cursor backgrounds.
        strip = Strip(Segment(segment.text,
                              (segment.style or Style()) + quote_style
                              if segment.style is None or segment.style.bgcolor == base.bgcolor
                              else segment.style + Style(color=TEXT_COLOR),
                              segment.control) for segment in strip)
        prefix = "▌" + " " * (self._visual_prefix_width(offset) - 1)
        band = Strip([Segment(prefix, Style(color=MUTED_COLOR, bgcolor=INPUT_QUOTE_BACKGROUND_COLOR))])
        return Strip.join([band, strip]).crop(0, self.size.width)

    def _atomic_range(self, start: int, end: int, *, deleting: bool) -> tuple[int, int]:
        if deleting and start < end:
            # Adjacent quoted lines form one block, including its final newline.
            offset = 0
            quote_start = None
            for line in self.text.splitlines(keepends=True):
                quoted = line.rstrip("\r\n") == ">" or line.startswith("> ")
                if quoted and quote_start is None:
                    quote_start = offset
                if not quoted and quote_start is not None:
                    if start < offset and end > quote_start:
                        start, end = min(start, quote_start), max(end, offset)
                    quote_start = None
                offset += len(line)
            if quote_start is not None and start < offset and end > quote_start:
                start, end = min(start, quote_start), max(end, offset)
        for block in self.paste_blocks:
            if start < block.end and end > block.start:
                start, end = min(start, block.start), max(end, block.end)
        return start, end

    def edit(self, edit: Edit):
        menu = self.suggestions
        chosen = menu is not None and menu.inserted_text == self.text and menu.display
        if chosen:
            if menu.quoting:
                self._finish_quote()
                edit.from_location = edit.to_location = self.cursor_location
            else:
                menu.dismiss(self.text, cursor=self.document.get_index_from_location(self.cursor_location))
        start = self.document.get_index_from_location(edit.top)
        end = self.document.get_index_from_location(edit.bottom)
        row, column = edit.top
        line = self.document.get_line(row)
        # Materialize the selected quote before applying the incoming insertion
        # below it. Keep the original edit (including pasted payload metadata).
        cancel_quote = bool(edit.text and start == end and column == len(line) and line.strip() == ">")
        if cancel_quote:
            self.update_suggestions()
            if self.suggestions is not None and self.suggestions.selected_quote is not None:
                self.action_submit()
                edit.from_location = edit.to_location = self.cursor_location
                return self.edit(edit)
            # No source available: leave the prompt as ordinary input.
            start -= len(line) - line.index(">")
            self.history.checkpoint()
        start, end = self._atomic_range(start, end, deleting=not edit.text)
        atomic_delete = not edit.text and (self.document.get_location_from_index(start) != edit.top
                                          or self.document.get_location_from_index(end) != edit.bottom)
        if atomic_delete:
            self.history.checkpoint()
        edit.from_location = self.document.get_location_from_index(start)
        edit.to_location = self.document.get_location_from_index(end)
        result = super().edit(edit if isinstance(edit, PasteEdit) else PasteEdit(edit))
        if atomic_delete or cancel_quote:
            self.history.checkpoint()
        if chosen:
            menu.dismiss(self.text, cursor=self.document.get_index_from_location(self.cursor_location))
        return result

    def _redo_batch(self, edits) -> None:
        # TextArea refreshes scrollbars before restoring the post-edit selection.
        # A block deletion can remove the cursor's row during that refresh.
        if (len(edits) == 1 and not edits[0].text and not edits[0].maintain_selection_offset
                and edits[0].top[0] != edits[0].bottom[0]):
            self.move_cursor(edits[0].top)
        super()._redo_batch(edits)

    def _watch_selection(self, previous: Selection, selection: Selection) -> None:
        start, end = (self.document.get_index_from_location(location) for location in selection)
        if start == end:
            for block in self.paste_blocks:
                if block.start < end < block.end:
                    start = end = block.start if selection.end < previous.end else block.end
        else:
            low, high = sorted((start, end))
            for block in self.paste_blocks:
                if low < block.end and high > block.start:
                    low, high = min(low, block.start), max(high, block.end)
            start, end = (low, high) if start < end else (high, low)
        snapped = Selection(self.document.get_location_from_index(start), self.document.get_location_from_index(end))
        if snapped != selection:
            self.selection = snapped
            return
        super()._watch_selection(previous, selection)

    def paste_text(self, text: str) -> None:
        if self.read_only or self.disabled:
            return
        try:
            attachments = pasted_paths(text)
            if attachments:
                self.add_attachments(attachments)
                return
        except ValueError as error:
            self.app.call_later(self.app.show_input_error, str(error))
            return
        if compact_paste(text):
            paste = Paste(text)
            self.history.checkpoint()
            self.edit(PasteEdit(Edit(paste.label, *self.selection, False), paste))
            self.history.checkpoint()
        else:
            self.edit(PasteEdit(Edit(text, *self.selection, False), pasted=True))

    async def _on_paste(self, event: events.Paste) -> None:
        event.prevent_default()
        event.stop()
        self.paste_text(event.text)

    def action_paste(self) -> None:
        self.paste_text(self.app.clipboard)

    def action_cut(self) -> None:
        if self.read_only or self.disabled:
            return
        if self.selection.is_empty:
            row, _ = self.cursor_location
            start = (row, 0)
            end = (row + 1, 0) if row + 1 < self.document.line_count else self.document.end
        else:
            start, end = sorted(self.selection)
        first, last = self._atomic_range(self.document.get_index_from_location(start),
                                        self.document.get_index_from_location(end), deleting=True)
        start, end = self.document.get_location_from_index(first), self.document.get_location_from_index(last)
        text = expand_range(self.text, self.paste_blocks, first, last)
        self.delete(start, end, maintain_selection_offset=False)
        if text:
            self.app.copy_to_clipboard(text)

    def on_click(self, event: events.Click) -> None:
        index = self.document.get_index_from_location(self.get_target_document_location(event))
        block = next((block for block in self.paste_blocks if block.start <= index < block.end), None)
        if block is None or event.button != 1:
            return
        event.prevent_default()
        event.stop()
        self.app.push_screen(PastePreview(block.paste.details if isinstance(block.paste, ImagePaste) else block.paste.text,
                                         lambda: self.remove_paste(block.paste),
                                          "Image" if isinstance(block.paste, ImagePaste) else
                                          "Path" if isinstance(block.paste, PathPaste) else "Pasted text"))

    def remove_paste(self, paste: Paste) -> None:
        if self.read_only or self.disabled:
            return
        block = next((block for block in self.paste_blocks if block.paste is paste), None)
        if block is not None:
            self.history.checkpoint()
            self.delete(self.document.get_location_from_index(block.start),
                        self.document.get_location_from_index(block.end), maintain_selection_offset=False)
            self.history.checkpoint()

    class Submitted(Message):
        def __init__(self, text: str, *, interpret_commands: bool = True) -> None:
            super().__init__()
            self.text = text
            self.interpret_commands = interpret_commands

    def on_mount(self) -> None:
        if self.suggestions is not None:
            self.watch(self.suggestions, "highlighted", self.update_suggestion)
            self.suggestions.on_choice = self.apply_selected_suggestion

    def apply_selected_suggestion(self) -> None:
        menu = self.suggestions
        if menu is None or not self.has_focus or self.disabled or self.read_only:
            return
        quote = menu.selected_quote
        command = menu.selected_command
        if quote is None and command is None:
            return
        if quote is not None:
            row, _ = self.cursor_location
            start, end = (row, 0), (row, len(self.document.get_line(row)))
            replacement = f"> {quote}"
        else:
            token = completion_token(self.text, self.document.get_index_from_location(self.cursor_location))
            if token is None:
                return
            start, end = (self.document.get_location_from_index(index) for index in token[:2])
            replacement = command
        self.history.checkpoint()
        # Bypass quote atom expansion: choosing a line replaces that line only,
        # not adjacent quote blocks. PasteEdit still preserves other paste atoms.
        super().edit(PasteEdit(Edit(replacement, start, end, False)))
        self.history.checkpoint()
        menu.keep_choices(self.text, self.document.get_index_from_location(self.cursor_location))
        self.update_suggestion()

    def _finish_quote(self) -> bool:
        menu = self.suggestions
        if menu is None or menu.selected_quote is None:
            return False
        row, _ = self.cursor_location
        replacement = f"> {menu.selected_quote}\n"
        menu.dismiss(self.text, cursor=self.document.get_index_from_location(self.cursor_location))
        self.history.checkpoint()
        super().edit(PasteEdit(Edit(replacement, (row, 0),
                                   (row, len(self.document.get_line(row))), False)))
        self.history.checkpoint()
        menu.dismiss(self.text, cursor=self.document.get_index_from_location(self.cursor_location))
        self.update_suggestion()
        return True

    def update_suggestion(self) -> None:
        # Only argument hints and the initial, unchosen preview remain ghost text.
        # Navigating a list writes its selection into the document instead.
        suffix = ""
        menu = self.suggestions
        if (menu is not None and self.has_focus and not self.disabled
                and self.selection.is_empty and self.cursor_location == self.document.end
                and menu.query_text == self.text):
            prefix = self.text.lstrip()
            if menu.selected_quote is not None and menu.inserted_text is None:
                suffix = " " + menu.selected_quote
            if menu.selected_quote is None:
                suffix = self._skill_preview(prefix)
        self.suggestion = suffix

    def _skill_preview(self, prefix: str) -> str:
        suffix = ""
        menu = self.suggestions
        if menu is None or not prefix.startswith("/"):
            return suffix
        chain, arguments = skill_command_chain(prefix, menu.skills())
        skill = chain[-1] if chain else None
        if prefix == "/" and menu.matches and menu.matches[0] in menu.catalog:
            preview_command = menu.matches[0]
            suffix = preview_command[len(prefix):]
            skill = menu.catalog[preview_command]
            arguments = ""
        if skill is None:
            return suffix
        hint = skill.get("argument-hint", "") if not arguments.strip() else ""
        if hint:
            separator = "" if not suffix and prefix[-1].isspace() else " "
            suffix += separator + hint
        return suffix

    def action_cursor_right(self, select: bool = False) -> None:
        # Native TextArea accepts suggestions with Right. This is a visual preview
        # only: preserve cursor/selection navigation and leave execution to Enter.
        self.suggestion = ""
        super().action_cursor_right(select)
        self.update_suggestion()

    def on_text_area_selection_changed(self, event: TextArea.SelectionChanged) -> None:
        self.update_suggestions()

    def action_submit(self) -> None:
        if self.read_only or self.disabled:
            return
        self.update_suggestions()
        if self._finish_quote():
            return
        if self.suggestions is not None:
            self.suggestions.dismiss(self.text)
        self.post_message(self.Submitted(self.expanded_text, interpret_commands=bool(self.command_text)))

    def action_cursor_up(self, select: bool = False) -> None:
        self.update_suggestions()
        if not select and self.suggestions is not None and self.suggestions.display:
            self.suggestions.action_cursor_up()
            self.apply_selected_suggestion()
        else:
            super().action_cursor_up(select)

    def action_cursor_down(self, select: bool = False) -> None:
        self.update_suggestions()
        if not select and self.suggestions is not None and self.suggestions.display:
            self.suggestions.action_cursor_down()
            self.apply_selected_suggestion()
        else:
            super().action_cursor_down(select)

    def update_suggestions(self) -> None:
        if self.suggestions is not None:
            row, column = self.cursor_location
            line = self.document.get_line(row)
            active = self.has_focus and not self.disabled and not self.read_only
            quoting = active and self.selection.is_empty and column == len(line) and (
                line.strip() == ">" or (self.suggestions.quoting and self.suggestions.inserted_text == self.text))
            cursor = self.document.get_index_from_location(self.cursor_location)
            token = completion_token(self.text, cursor)
            protected = token is not None and any(left < token[1] and right > token[0]
                                                   for left, right in self.pasted_ranges)
            available = active and self.selection.is_empty and not protected
            self.suggestions.update_query(self.text if available else "", quoting=quoting,
                                          cursor=cursor if available else 0)
        self.update_suggestion()

    def on_focus(self) -> None:
        self.call_after_refresh(self.update_suggestions)

    def on_blur(self) -> None:
        if self.suggestions is not None:
            self.suggestions.update_query("")
        self.suggestion = ""

    def watch_disabled(self, disabled: bool) -> None:
        super().watch_disabled(disabled)
        self.update_suggestions()

    def action_newline(self) -> None:
        if not self.read_only and not self.disabled:
            self.replace("\n", *self.selection)

    def on_mouse_up(self, event: events.MouseUp) -> None:
        if event.button == 1 and self.selected_text:
            self.app.copy_to_clipboard(self.selected_text)

    def on_key(self, event: events.Key) -> None:
        menu = self.suggestions
        if (menu is not None and menu.inserted_text == self.text and menu.display
                and event.key not in ("tab", "shift+tab", "up", "down", "enter")):
            if menu.quoting:
                self._finish_quote()
                if not event.is_printable and event.key not in ("shift+enter", "ctrl+j", "shift+\r"):
                    event.stop()
                    event.prevent_default()
                    return
            elif not event.is_printable and event.key not in (
                    "backspace", "ctrl+h", "delete", "ctrl+w", "ctrl+u", "ctrl+k", "ctrl+x",
                    "shift+enter", "ctrl+j", "shift+\r"):
                menu.dismiss(self.text, cursor=self.document.get_index_from_location(self.cursor_location))
        # modifyOtherKeys may decode Ctrl+H/W with a printable character.
        # Consume it before TextArea inserts that character instead of deleting.
        if event.key in ("ctrl+h", "ctrl+w"):
            event.stop()
            event.prevent_default()
            if not self.read_only and not self.disabled:
                if event.key == "ctrl+h":
                    self.action_delete_left()
                else:
                    self.action_delete_word_left()
            return
        # Queue Enter with printable keys; app-level bindings can overtake preceding text.
        # Textual 8.2 decodes xterm's CSI 27;2;13~ as shift+CR.
        if event.key in ("enter", "shift+enter", "ctrl+j", "shift+\r"):
            event.stop()
            event.prevent_default()
            if event.key == "enter":
                self.action_submit()
            else:
                self.action_newline()

    def resize_to_content(self) -> None:
        self.styles.height = min(self.MAX_VISIBLE_LINES, max(1, self.wrapped_document.height))
        self.scroll_cursor_visible()

    def on_text_area_changed(self, event: TextArea.Changed) -> None:
        self.resize_to_content()
        self.update_suggestions()

    def on_resize(self) -> None:
        # TextArea first recomputes wrapping for the new available width.
        self.call_after_refresh(self.resize_to_content)
