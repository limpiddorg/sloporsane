"""Selection preview using catalog data only, never loading or executing a skill."""

from rich.text import Text
from textual.containers import Horizontal, VerticalScroll
from textual.widgets import Static

from .config import ALGAE_MD, MISC_MD, MUTED_COLOR


class SkillDetails(VerticalScroll, can_focus=False):
    FOCUS_ON_CLICK = False
    DEFAULT_CSS = """
    SkillDetails { display: none; height: 50vh; margin: 0 1; padding: 1 2;
        border: none; border-top: solid $title-md; border-bottom: solid $title-md;
        background: $input-background;
        scrollbar-size: 1 1; scrollbar-color: $muted; scrollbar-background: $input-background; }
    SkillDetails Static { height: auto; }
    SkillDetails.command-preview { height: auto; max-height: 10; padding: 0 2; }
    SkillDetails Horizontal { height: auto; }
    SkillDetails #skill-name { width: 1fr; }
    SkillDetails #skill-origin { width: auto; text-align: right; }
    SkillDetails #skill-compatibility { display: none; color: $muted; }
    """

    def compose(self):
        with Horizontal():
            yield Static(markup=False, id="skill-name")
            yield Static(markup=False, id="skill-origin")
        yield Static(markup=False, id="skill-compatibility")
        yield Static(markup=False, id="skill-body")

    def show_skill(self, skill: dict | None) -> None:
        self.remove_class("command-preview")
        self.display = skill is not None
        if skill is None:
            return
        self._show_header(skill)
        text = Text(f"\n{skill.get('description', '')}")
        self._append_arguments(text, skill)
        for category in ("scripts", "references", "assets"):
            self._append_resources(text, skill, category)
        self._append_metadata(text, skill)
        self.query_one("#skill-body", Static).update(text)
        self.scroll_home(animate=False)

    def show_command(self, command: str, description: str) -> None:
        self.add_class("command-preview")
        self.display = True
        self.query_one("#skill-name", Static).update(Text(command.removeprefix("/"), style=f"bold {MISC_MD}"))
        self.query_one("#skill-origin", Static).update("")
        self.query_one("#skill-compatibility", Static).display = False
        self.query_one("#skill-body", Static).update(Text(description))
        self.scroll_home(animate=False)

    def _show_header(self, skill: dict) -> None:
        title = Text().append(skill["name"], style=f"bold {ALGAE_MD}")
        if skill.get("license"):
            title.append(f" [{skill['license']}]", style=MUTED_COLOR)
        self.query_one("#skill-name", Static).update(title)
        origin = f"[{skill['source']}]" if skill.get("source") else ""
        self.query_one("#skill-origin", Static).update(Text(origin, style="dim"))
        compatibility = self.query_one("#skill-compatibility", Static)
        compatibility.update(skill.get("compatibility", ""))
        compatibility.display = bool(skill.get("compatibility"))

    def _append_arguments(self, text: Text, skill: dict) -> None:
        descriptions = skill.get("argument-descriptions", {})
        if not descriptions:
            return
        names = list(dict.fromkeys([*skill.get("arguments", []), *descriptions]))
        text.append("\n\nArguments", style="bold")
        for position, name in enumerate(names, start=1):
            text.append(f"\n\n{position}. {name}")
            if name in descriptions:
                text.append(f"\n{descriptions[name]}")

    def _append_metadata(self, text: Text, skill: dict) -> None:
        if not skill.get("metadata"):
            return
        text.append("\n\nMetadata\n", style="bold")
        text.append("\n".join(f"{key}: {value}" for key, value in skill["metadata"].items()))

    def _append_resources(self, text: Text, skill: dict, category: str) -> None:
        paths = skill.get(category, [])
        error = skill.get(f"{category}_error")
        if not paths and not error:
            return
        text.append(f"\n\n{category.title()}\n", style="bold")
        if error:
            text.append(f"Unavailable: {error}")
        else:
            text.append("\n".join(f"- {path}" for path in paths))
