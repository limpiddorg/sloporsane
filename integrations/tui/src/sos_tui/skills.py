"""Skill commands preserve raw input; only the shared daemon loader expands it."""

import re
from .slash_commands import COMMANDS

MAX_STACKED_SKILLS = 6


def skill_commands(skills: list[dict]) -> dict[str, dict]:
    return {f"/{skill['name']}": skill for skill in skills if f"/{skill['name']}" not in COMMANDS}


def skill_display_name(skill: dict) -> str:
    hint = skill.get("argument-hint", "")
    return skill["name"] + (f" {hint}" if hint else "")


def completion_token(text: str, cursor: int) -> tuple[int, int, str] | None:
    """Find a slash token at the cursor, excluding paths and URL fragments."""
    match = re.search(r"(?<![\w./\\-])(?<!\w:)/[\w-]*$", text[:cursor])
    if match is None:
        return None
    tail = re.match(r"[\w-]*", text[cursor:])
    return match.start(), cursor + len(tail[0]), match[0]


def skill_reference_spans(text: str, skills: list[dict]) -> tuple[tuple[int, int], ...]:
    """Return character ranges [start, end) of known references, without parsing.

    Word characters, hyphens and path separators cannot adjoin a reference.
    Protocol prefixes such as https: are excluded. A trailing period is
    punctuation unless it introduces a filename extension.
    Offsets address the original text, including whitespace and newlines.
    """
    commands = skill_commands(skills)
    if not commands:
        return ()
    references = "|".join(re.escape(command) for command in commands)
    pattern = rf"(?<![\w./\\-])(?<!\w:)(?:{references})(?![\w/\\-]|\.\w)"
    return tuple(match.span() for match in re.finditer(pattern, text))


def skill_command_chain(text: str, skills: list[dict]) -> tuple[list[dict], str]:
    """Only consecutive, unquoted known skills at the start form a chain."""
    catalog = skill_commands(skills)
    chain = []
    remaining = text.lstrip()
    while len(chain) < MAX_STACKED_SKILLS:
        parts = remaining.split(maxsplit=1)
        if not parts or parts[0] not in catalog:
            break
        chain.append(catalog[parts[0]])
        remaining = parts[1] if len(parts) > 1 else ""
    return chain, remaining


def is_skill_command_position(prefix: str, skills: list[dict]) -> bool:
    chain, remaining = skill_command_chain(prefix, skills)
    return not remaining.strip() and len(chain) < MAX_STACKED_SKILLS


def parse_skill_command(text: str, skills: list[dict]) -> dict | None:
    """Preserve the complete tail of a single invocation, without argument binding."""
    parts = text.lstrip().split(maxsplit=1)
    skill = skill_commands(skills).get(parts[0]) if parts else None
    return {"name": skill["name"], "input": parts[1] if len(parts) > 1 else ""} if skill else None


def parse_skill_commands(text: str, skills: list[dict]) -> list[dict]:
    chain, tail = skill_command_chain(text, skills)
    return [{"name": skill["name"], "input": tail} for skill in chain]
