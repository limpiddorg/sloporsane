"""Command and quote suggestions controlled from the composer, without taking focus."""

from collections.abc import Callable
from rich.text import Text
from rich.table import Table
from textual.widgets import OptionList
from textual.widgets.option_list import Option

from .config import ALGAE_MD, MISC_MD
from .slash_commands import COMMAND_DESCRIPTIONS, matching_commands
from .skills import completion_token, is_skill_command_position, skill_commands, skill_display_name
from .skill_details import SkillDetails
from .input_error_notice import InputErrorNotice


class CommandSuggestions(OptionList, can_focus=False):
    FOCUS_ON_CLICK = False
    DEFAULT_CSS = """
    CommandSuggestions { display: none; height: auto; max-height: 7; margin: 0 1;
        border: none; padding: 0; background: $input-background; color: $foreground;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $input-background; }
    CommandSuggestions > .option-list--option { padding: 0 1; }
    CommandSuggestions > .option-list--option-highlighted,
    CommandSuggestions > .option-list--option-hover-highlighted {
        background: $depth-md; color: $foreground; text-style: bold; }
    CommandSuggestions > .option-list--option-hover { background: $content-background; color: $foreground; }
    """

    def __init__(self, quote_source: Callable[[], str] = lambda: "", skills: Callable[[], list[dict]] = lambda: [], details: SkillDetails | None = None, commands=None, **kwargs) -> None:
        super().__init__(**kwargs)
        self.quote_source = quote_source
        self.skills = skills
        self.details = details
        self.commands = commands
        self.catalog: dict[str, dict] = {}
        self.query_text = ""
        self.matches: tuple[str, ...] = ()
        self.quoting = False
        self.first_word = True
        self.cursor = 0
        self.inserted_text: str | None = None
        self.dismissed_text: str | None = None
        self.on_choice: Callable[[], None] = lambda: None

    def update_query(self, text: str, *, quoting: bool = False, cursor: int | None = None) -> None:
        cursor = len(text) if cursor is None else cursor
        catalog = skill_commands(self.skills())
        if catalog == self.catalog and cursor == self.cursor and (text == self.inserted_text or text == self.dismissed_text):
            return
        self.inserted_text = self.dismissed_text = None
        matches = (tuple(line for line in self.quote_source().splitlines() if line.strip())
                   if quoting else ())
        token = completion_token(text, cursor)
        first_word = token is not None and not text[:token[0]].strip()
        skill_position = token is not None and is_skill_command_position(text[:token[0]], self.skills())
        if not quoting and token is not None:
            prefix = token[2]
            native = matching_commands(prefix) if first_word else ()
            if self.commands is not None:
                native = tuple(command for command in native if command in self.commands)
            skills = tuple(name for name in catalog if name.startswith(prefix)) if skill_position or len(prefix) > 1 else ()
            matches = tuple(sorted(skills)) + tuple(sorted(set(native) - set(skills)))
        if (text == self.query_text and cursor == self.cursor and matches == self.matches
                and quoting == self.quoting and catalog == self.catalog):
            return
        self.cursor = cursor
        self.first_word = first_word
        catalog_changed = catalog != self.catalog
        self.catalog = catalog
        self.query_text = text
        if matches != self.matches or quoting != self.quoting or catalog_changed:
            self.matches = matches
            # Quote text is literal, and repeated paragraphs are distinct options.
            self.set_options(Option(self.option_label(value, quoting),
                                    id=str(index) if quoting else value)
                             for index, value in enumerate(matches))
        self.quoting = quoting
        self.highlighted = 0 if quoting and matches else None
        self.display = bool(matches)
        self.update_details()

    def option_label(self, value: str, quoting: bool) -> Text | Table:
        if quoting:
            return Text(value)
        if value not in self.catalog:
            return Text(value, style=MISC_MD)
        row = Table.grid(expand=True, padding=(0, 1))
        row.add_column(ratio=1, no_wrap=True, overflow="ellipsis")
        row.add_column(width=5, justify="right", no_wrap=True)
        row.add_row(Text("/" + skill_display_name(self.catalog[value]), style=ALGAE_MD),
                    Text("Skill", style=f"bold {ALGAE_MD}"))
        return row

    def keep_choices(self, text: str, cursor: int) -> None:
        self.inserted_text = text
        self.query_text = text
        self.cursor = cursor

    def dismiss(self, text: str, *, cursor: int | None = None) -> None:
        self.inserted_text = None
        self.dismissed_text = self.query_text = text
        self.cursor = len(text) if cursor is None else cursor
        self.display = False
        self.update_details()

    def update_details(self) -> None:
        if self.details is not None and self.details.is_mounted:
            command = self.selected_command
            if command is None and self.display and not self.quoting and self.matches:
                candidate = self.matches[0]
                if candidate in self.catalog:
                    command = candidate
            if command in COMMAND_DESCRIPTIONS:
                self.details.show_command(command, COMMAND_DESCRIPTIONS[command])
            else:
                self.details.show_skill(self.catalog.get(command))
        if self.is_mounted:
            for notice in self.screen.query(InputErrorNotice):
                notice.schedule_reposition()

    def on_option_list_option_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        self.update_details()

    @property
    def selected_command(self) -> str | None:
        option = self.highlighted_option if self.display and not self.quoting else None
        return option.id if option is not None else None

    @property
    def selected_quote(self) -> str | None:
        if self.display and self.quoting and self.highlighted is not None:
            return self.matches[self.highlighted]
        return None

    def select_next(self, direction: int = 1) -> None:
        if self.display and self.matches:
            if self.highlighted is None:
                self.highlighted = 0 if direction > 0 else len(self.matches) - 1
            else:
                self.highlighted = (self.highlighted + direction) % len(self.matches)
            self.on_choice()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        event.stop()
        self.on_choice()
