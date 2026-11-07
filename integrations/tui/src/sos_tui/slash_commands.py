"""Command names shared by completion and local input validation."""

COMMAND_DESCRIPTIONS = {
    "/worktree": "Switch this conversation to a Git worktree, or create one from committed HEAD.",
    "/mcps": "Add and manage MCP servers, trust, connections and credentials.",
    "/btw": "Ask a side question without interrupting the main conversation.",
    "/compact": "Summarize the conversation to free up model context. The visible chat history is kept.",
    "/connect": "Connect an AI provider or replace its credentials. Other connected providers stay available.",
    "/model": "Choose the model, reasoning level and context limit. These preferences apply across sessions.",
    "/new": "Start a new conversation. Your current session remains available through /resume.",
    "/permissions": "Review and manage the permissions that control tool actions.",
    "/recap": "Request a recap of the conversation to help you pick up where you left off.",
    "/resume": "Browse saved sessions and reopen a previous conversation.",
    "/undo": "Undo the last user turn and restore its file changes. The original message returns to the composer.",
    "/redo": "Restore a turn removed by /undo, including its messages and files, without rerunning tools. Sending a new message discards redo.",
}

COMMANDS = tuple(sorted(COMMAND_DESCRIPTIONS))


def matching_commands(text: str) -> tuple[str, ...]:
    prefix = text.lstrip()
    if not prefix.startswith("/") or any(character.isspace() for character in prefix):
        return ()
    return tuple(command for command in COMMANDS if command.startswith(prefix))
