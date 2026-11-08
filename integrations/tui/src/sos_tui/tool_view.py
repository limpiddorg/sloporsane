"""One expandable tool call, with arguments and a tool-specific result."""

import json
import re
import shlex
from typing import Any, Sequence

from rich.text import Text
from rich.style import Style
from textual import work
from textual.containers import Vertical
from textual.highlight import guess_language
from textual.widget import Widget
from textual.widgets import Collapsible, Rule, Static

from .config import ALGAE_MD, BOLD_MD, DEPTH_MD, MISC_MD, MUTED_COLOR, SANE_COLOR, SLOP_COLOR, TITLE_MD
from .chat_collapsible import ChatCollapsible
from .response_markdown import CodeFence, ResponseMarkdown
from .patch_diff import patch_widgets
from .code_colors import highlighted_lines
from .tool_file_paths import FILE_TOOLS, ToolPathTitle, append_title_path, file_path, file_result, tool_files
from .workspace_path import WorkspacePath
from .turn_activity import ProcessWaiting


def plain(value: Any) -> str:
    return value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=2)


def tool_display_name(tool: str, server: Any = None, mcp_tool: Any = None) -> str:
    if isinstance(server, str) and isinstance(mcp_tool, str):
        return f"mcp • {server} • {mcp_tool}"
    return "mcp" if tool.startswith("mcp_") else tool


def permission_segments(records: Sequence[dict[str, Any]]) -> list[tuple[str, str]]:
    labels = {"yes": "Human", "no": "Human Refused", "always": "Human",
              "ai_accepted": "AI", "ai_refused": "AI", "manually_approved": "Human"}
    displayed: list[tuple[str, str]] = []
    for record in records:
        decision = record.get("decision")
        label = labels.get(decision)
        confidence = record.get("confidence")
        if label is not None and type(confidence) in (int, float) and 0 <= confidence <= 1:
            score = f"{confidence * 100:.12g}%"
            label += f" {score}" if decision.startswith("ai_") else f" (AI {score})"
        color = SLOP_COLOR if decision in ("no", "ai_refused") else SANE_COLOR
        # Several permission checks may share a verdict within one tool call.
        if label is not None and (label, color) not in displayed:
            displayed.append((label, color))
    return displayed


def permission_suffix(records: Sequence[dict[str, Any]]) -> str:
    return "".join(" • " + label for label, _ in permission_segments(records))


def styled_tool_title(title: str, records: Sequence[dict[str, Any]] = ()) -> Text:
    """Tool titles are never bold; outcomes and authorization retain their colors."""
    text = Text(title, style=Style(bold=False))
    for match in re.finditer(r"(?:[·•] )(Completed|Failed|Exit code -?\d+)(?= [·•]|$)", title):
        label = match[1]
        color = SANE_COLOR if label in ("Completed", "Exit code 0") else SLOP_COLOR
        text.stylize(Style(color=color, bold=False), match.start(1), match.end(1))
    offset = title.find(permission_suffix(records))
    if offset >= 0:
        for label, color in permission_segments(records):
            offset += 3  # Bullet separator.
            text.stylize(Style(color=color, bold=False), offset, offset + len(label))
            offset += len(label)
    return text


def skill_arguments(arguments: dict[str, Any], metadata: dict[str, Any]) -> Text:
    text = Text()
    text.append(str(arguments.get("name", "")), style=ALGAE_MD)
    names = metadata.get("argument_names", [])
    values = metadata.get("argument_values")
    raw = arguments.get("input", "")
    if not names:
        values = [raw] if raw else []
    elif values is None:
        try:
            values = shlex.split(raw)
        except ValueError:
            values = [raw]
    count = max(len(names), len(values))
    for index in range(count):
        label = names[index] if index < len(names) else "argument" if count == 1 else f"argument {index}"
        text.append(f"\n{label}: ", style=MISC_MD)
        if index < len(values):
            text.append(values[index])
    return text


def format_arguments(arguments: dict[str, Any], tool: str | None = None,
                     metadata: dict[str, Any] | None = None) -> Text:
    if tool == "skill":
        return skill_arguments(arguments, metadata or {})
    if tool in ("grep", "glob"):
        text = Text()
        text.append(str(arguments.get("path") or "."), style=DEPTH_MD)
        text.append("\n")
        text.append(str(arguments.get("pattern", "")), style=MISC_MD)
        return text
    if tool in ("bash", "wait"):
        default_timeout = 3600000 if tool == "wait" else 120000
        arguments = {"command": arguments.get("command", ""), "timeout": f"{arguments.get('timeout', default_timeout)} ms",
                     **({"workdir": arguments["workdir"]} if "workdir" in arguments else {})}
    text = Text()
    for index, (name, value) in enumerate(arguments.items()):
        if index:
            text.append("\n")
        text.append(name, style=MISC_MD)
        rendered = plain(value)
        text.append(": " if "\n" not in rendered else ":\n")
        text.append(rendered)
    if not arguments:
        text.append("No arguments", style="dim")
    return text


def result_widgets(tool: str, arguments: dict[str, Any], result: dict[str, Any]) -> list[Widget]:
    output = plain(result["output"])
    metadata = result.get("metadata") or {}
    widgets: list[Widget] = []
    if result["isError"]:
        refusals = [record for record in metadata.get("permissions", []) if record.get("decision") == "ai_refused"]
        if refusals:
            # The original output instructs the model how to respect a refusal.
            # Keep it intact in the transcript, not in the user-facing result.
            output = ("Action not executed: AI verification unavailable."
                      if refusals[-1].get("confidence") is None else
                      "Action not executed: refused by AI verification.")
        widgets.append(Static(Text(output, style=SLOP_COLOR)))
    elif tool == "write" and isinstance(arguments.get("content"), str):
        path = metadata.get("filepath") or arguments.get("filePath", "Unknown file")
        widgets.append(WorkspacePath(Text(path, style=DEPTH_MD)))
        widgets.append(Static(Text("\n").join(highlighted_lines(path, arguments["content"])),
                              classes="tool-code"))
    elif tool == "apply_patch" and metadata.get("files"):
        widgets.extend(patch_widgets(metadata))
    elif tool == "edit" and isinstance(metadata.get("diff"), str) and metadata["diff"]:
        widgets.extend(patch_widgets({"files": [{
            "filePath": metadata.get("filepath") or arguments.get("filePath", "Unknown file"),
            "type": "edit", "patch": metadata["diff"],
        }]}))
    elif tool in ("write", "edit", "apply_patch"):
        widgets.append(Static(Text(output)))
        if metadata.get("filepath"):
            widgets.append(WorkspacePath(Text(metadata["filepath"], style=DEPTH_MD)))
        if isinstance(metadata.get("diff"), str) and metadata["diff"]:
            widgets.append(Static(Text("Diff", style=f"bold {TITLE_MD}")))
            widgets.append(Static(CodeFence.highlight(metadata["diff"], "diff"), classes="tool-code"))
    elif tool == "read" and isinstance(metadata.get("display"), dict):
        display = metadata["display"]
        widgets.append(WorkspacePath(Text(display.get("path", ""), style=DEPTH_MD)))
        if display.get("type") == "file":
            source = display.get("text", "")
            language = guess_language(source, display.get("path"))
            code = CodeFence.highlight(source, language)
            start = display.get("lineStart", 1)
            lines = code.split("\n") if source else []
            width = len(str(start + max(0, len(lines) - 1)))
            numbered = Text()
            for offset, line in enumerate(lines):
                numbered.append(f"{start + offset:>{width}}  ", style=DEPTH_MD)
                # Resolve our explicit syntax styles into Rich spans.
                for segment in line.render_segments():
                    numbered.append(segment.text, style=segment.style)
                if offset < len(lines) - 1:
                    numbered.append("\n")
            widgets.append(Static(numbered if source else Text("Empty file", style="dim"), classes="tool-code"))
            widgets.append(Static(Text(f"Lines {display.get('lineStart', 1)}–{display.get('lineEnd', 0)} · {display.get('totalLines', '?')} total", style="dim")))
        elif display.get("type") == "directory":
            entries = Text()
            for entry in display.get("entries", []):
                entries.append(entry + "\n", style=DEPTH_MD)
            widgets.append(Static(entries))
            widgets.append(Static(Text(f"{display.get('totalEntries', '?')} entries", style="dim")))
        else:
            widgets.append(Static(Text(output)))
    elif tool in ("bash", "wait"):
        # The same status is presented separately using structured metadata.
        if "exit" in metadata:
            output = re.sub(r"\n\n<shell_metadata>\n.*?</shell_metadata>$", "", output, flags=re.DOTALL)
        status = "Timed out" if metadata.get("timedOut") else f"Exit code: {metadata.get('exit', 'unknown')}"
        failed = metadata.get("timedOut") or metadata.get("exit") not in (None, 0)
        widgets.append(Static(Text(status, style=SLOP_COLOR if failed else ALGAE_MD)))
        widgets.append(Static(Text(output), classes="tool-code"))
    elif tool in ("glob", "grep"):
        if tool == "grep":
            lines = output.splitlines()
            count = metadata.get("matches")
            if count is None and lines and (match := re.fullmatch(r"Found (\d+) matches", lines[0])):
                count = match[1]
            if count is None and lines and lines[0] == "No files found":
                count = 0
            if count is not None and lines:
                lines[0] = f"{count} Matches"
            output = "\n".join(line for line in lines if line)
        text = Text()
        for line in output.splitlines(keepends=True):
            style = None
            if tool == "grep" and re.match(r"\s+Line \d+:", line):
                prefix, body = line.split(":", 1)
                text.append(prefix + ":", style=TITLE_MD)
                text.append(body)
                continue
            if tool == "glob" or line.rstrip().endswith(":"):
                style = DEPTH_MD
            text.append(line, style=style)
        widgets.append(Static(text))
    elif tool == "webfetch":
        widgets.append(Static(Text(metadata.get("url", arguments.get("url", "")), style=DEPTH_MD)))
        format = arguments.get("format", "markdown")
        if format == "markdown":
            widgets.append(ResponseMarkdown(output))
        elif format == "html":
            widgets.append(Static(CodeFence.highlight(output, "html"), classes="tool-code"))
        else:
            widgets.append(Static(Text(output)))
    else:
        widgets.append(Static(Text(output)))

    if metadata.get("truncated"):
        message = "Output truncated"
        if metadata.get("outputPath"):
            message += f" · Full output: {metadata['outputPath']}"
        widgets.append(Static(Text(message, style=BOLD_MD)))
    for attachment in result.get("attachments", []):
        widgets.append(Static(Text(f"Attachment · {attachment['mime']}", style="dim")))
    return widgets


class ToolView(ChatCollapsible):
    DEFAULT_CSS = f"""
    ToolView Rule.tool-separator.-horizontal {{ height: 1; margin: 0; padding: 0; color: {MUTED_COLOR}; }}
    ToolView.multi-file-patch > Contents {{ display: none; }}
    ToolView .tool-file-rows {{ height: auto; }}
    """

    def _watch_title(self, title: str) -> None:
        self._title.label = self.title_text()

    @property
    def activity_kind(self) -> str:
        return "skill" if self.tool == "skill" or self.skill_script is not None else "tools"

    @property
    def display_name(self) -> str:
        if self.mcp_identity is not None:
            return tool_display_name(self.tool, *self.mcp_identity)
        name = self.skill_script
        if name is None:
            return tool_display_name(self.tool)
        label = f"{name} script"
        return f"{label} · {self.skill_script_path}" if self.skill_script_path else label

    def __init__(self, call_id: str, tool: str, arguments: dict[str, Any], *, skill_script: str | None = None,
                 skill_script_path: str | None = None, user_skill: str | None = None,
                 lazy_arguments: bool = False, file_index: int | None = None) -> None:
        self.file_index = file_index
        self.file_views: list[ToolView] = []
        self.file_sync_pending = self.file_sync_running = False
        self.multi_file = False
        self.file_rows = Vertical(classes="tool-file-rows")
        self.file_rows.display = False
        self.user_skill = user_skill
        self.mcp_identity: tuple[str, str] | None = None
        self.skill_script = skill_script
        self.skill_script_path = skill_script_path
        self.call_id = call_id
        self.tool = tool
        self.arguments = arguments
        self.arguments_rendered = not lazy_arguments
        self.result: dict[str, Any] | None = None
        self.rendered = False
        self.permissions: list[dict[str, Any]] = []
        self.base_title = self.status_title("Pending")
        self.results = Vertical(Static("Waiting for result…", classes="tool-waiting"), classes="tool-results")
        result_only = tool in ("apply_patch", "edit", "write")
        self.arguments_view = Static(format_arguments(arguments, tool) if self.arguments_rendered and not result_only else "", classes="tool-arguments")
        arguments_widgets = [Static(Text("Arguments", style=f"bold {TITLE_MD}")), self.arguments_view]
        result_widgets_header = [Rule(line_style="solid", classes="tool-separator")]
        if result_only:
            arguments_widgets = []
            result_widgets_header = []
        elif tool in ("bash", "wait", "grep", "glob", "skill"):
            arguments_widgets = [self.arguments_view]
        self.call_headers = [*arguments_widgets, *result_widgets_header]
        super().__init__(
            *arguments_widgets,
            *result_widgets_header,
            self.results,
            title=self.base_title, collapsed=True, collapsed_symbol="▸", expanded_symbol="▾",
            classes="tool-call skill-call" if self.activity_kind == "skill" else "tool-call",
        )
        if tool in FILE_TOOLS or tool in ("bash", "wait", "grep", "glob", "rerun_approved_action"):
            self._title = ToolPathTitle(self)
        self.update_file_layout()

    def compose(self):
        yield from super().compose()
        # File drawers are siblings of Contents, never inside an outer drawer.
        # Closing or restoring that drawer must not hide or indent the files.
        if self.tool in ("apply_patch", "rerun_approved_action"):
            yield self.file_rows

    def on_mount(self) -> None:
        super().on_mount()
        if self.tool == "wait" and self.result is None:
            status = self.arguments.get("status")
            self.post_message(ProcessWaiting(self.call_id, True,
                                            status if isinstance(status, str) else "Waiting for process completion"))
        if self.multi_file:
            self.request_file_sync()

    def display_path(self) -> str:
        files = tool_files(self.tool, self.arguments, self.result)
        index = self.file_index if self.file_index is not None else 0
        return file_path(files[index]) if index < len(files) and (self.file_index is not None or len(files) == 1) else ""

    def title_text(self, width: int | None = None) -> Text:
        if self.tool in ("bash", "wait"):
            return self.bash_title_text(width)
        text = styled_tool_title(self.authorized_title(), self.permissions)
        if self.tool in ("grep", "glob"):
            pattern = Text(str(self.arguments.get("pattern", "")).replace("\n", " "),
                           style=Style(color=MUTED_COLOR, bold=False))
            if pattern:
                if width is not None:
                    text.truncate(max(0, width - min(8, width)), overflow="ellipsis")
                    budget = max(0, width - text.cell_len - 3)
                    if pattern.cell_len > budget:
                        suffix = "." * min(3, budget)
                        pattern.truncate(max(0, budget - len(suffix)), overflow="crop")
                        pattern.append(suffix)
                text.append(" • ")
                text.append_text(pattern)
            return text
        path = self.display_path()
        if path:
            return append_title_path(text, path, width)
        return text

    def bash_title_text(self, width: int | None = None) -> Text:
        before, separator, exit_code = self.authorized_title().partition(" · Exit code ")
        text = styled_tool_title(before, self.permissions)
        tail = styled_tool_title(separator + exit_code) if separator else Text()
        label = self.arguments.get("mainCommand")
        if isinstance(label, str) and label.strip():
            command = Text(label.replace("\n", " ").replace("\r", " "), style=Style(color=MUTED_COLOR, bold=False))
            if width is not None:
                budget = max(0, width - text.cell_len - tail.cell_len - 3)
                command.truncate(budget, overflow="ellipsis")
            text.append(" • ")
            text.append_text(command)
        text.append_text(tail)
        if width is not None:
            text.truncate(width, overflow="ellipsis")
        return text

    def update_file_layout(self) -> None:
        if self.tool in FILE_TOOLS:
            for widget in self.call_headers:
                widget.display = False
        self.multi_file = self.file_index is None and self.tool == "apply_patch" and len(tool_files(self.tool, self.arguments, self.result)) > 1
        self.set_class(self.multi_file, "multi-file-patch")
        self._title.display = not self.multi_file
        self.file_rows.display = self.multi_file
        self.results.display = not self.multi_file
        if self.multi_file:
            self.collapsed = False
            if self.is_mounted:
                self.request_file_sync()

    def new_file_view(self, index: int) -> "ToolView":
        view = ToolView(self.call_id, self.tool, self.arguments, file_index=index)
        view.permissions = list(self.permissions)
        if self.result is not None:
            view.set_result(self.result)
        return view

    def request_file_sync(self) -> None:
        self.file_sync_pending = True
        if not self.file_sync_running:
            self.file_sync_running = True
            self.sync_file_views()

    @work(group="patch-file-rows")
    async def sync_file_views(self) -> None:
        try:
            while self.file_sync_pending:
                self.file_sync_pending = False
                await self.update_file_views()
        finally:
            self.file_sync_running = False

    async def update_file_views(self) -> None:
        files = tool_files(self.tool, self.arguments, self.result)
        while len(self.file_views) < len(files):
            view = self.new_file_view(len(self.file_views))
            self.file_views.append(view)
            await self.file_rows.mount(view)
        for view in self.file_views[len(files):]:
            await view.remove()
        del self.file_views[len(files):]
        for view in self.file_views:
            view.arguments = self.arguments
            view.permissions = list(self.permissions)
            if self.result is not None:
                view.set_result(self.result)
            else:
                view.title = view.authorized_title()

    def status_title(self, status: str) -> str:
        if self.mcp_identity is not None or self.tool.startswith("mcp_"):
            return f"{self.display_name} • {status}"
        if self.tool == "skill":
            name = self.arguments.get("name") or self.user_skill
            return f"skill · {name} · {status}" if name else f"skill · {status}"
        return f"{self.display_name} · {status}"

    def authorized_title(self) -> str:
        permissions = permission_suffix(self.permissions)
        if not permissions:
            return self.base_title
        prefix = self.status_title("")
        return prefix[:-3] + permissions + " • " + self.base_title[len(prefix):]

    def set_result(self, result: dict[str, Any]) -> None:
        self.result = result
        if self.tool == "wait" and self.is_mounted:
            self.post_message(ProcessWaiting(self.call_id, False))
        self.rendered = False
        metadata = result.get("metadata") or {}
        self.update_mcp_identity(metadata.get("mcp_server"), metadata.get("mcp_tool"))
        if replay := metadata.get("approved_action"):
            self.tool = replay["tool"]
            if "arguments" in replay:
                self.arguments = replay["arguments"]
                if self.arguments_rendered:
                    self.arguments_view.update(format_arguments(self.arguments, self.tool))
        if self.tool == "skill" and self.arguments_rendered:
            self.arguments_view.update(format_arguments(self.arguments, self.tool, metadata))
        command_returned = (self.tool in ("bash", "wait") and type(metadata.get("exit")) is int
                            and not metadata.get("timedOut") and not metadata.get("cancelled"))
        failed = (result["isError"] and not command_returned) or (self.tool in ("bash", "wait") and metadata.get("timedOut"))
        self.set_class(self.activity_kind == "skill", "skill-call")
        self.base_title = self.status_title('Failed' if failed else 'Completed')
        if metadata.get("cancelled"):
            self.base_title = self.status_title('Cancelled' if metadata.get('started') else 'Not run')
        if command_returned:
            self.base_title += f" · Exit code {metadata['exit']}"
        self.permissions = metadata.get("permissions", self.permissions)
        self.title = self.authorized_title()
        self.update_file_layout()
        self._title.refresh()
        if self.is_mounted and not self.collapsed:
            self.render_result()

    def record_permission(self, event: dict[str, Any]) -> None:
        details = event.get("details") or {}
        if self.update_mcp_identity(details.get("server"), details.get("mcp_tool")):
            self.base_title = self.status_title("Pending")
        if self.tool == "rerun_approved_action" and event.get("decision") == "manually_approved":
            self.tool = event["tool"]
            self.arguments = event["details"]["arguments"]
            if self.arguments_rendered:
                self.arguments_view.update(format_arguments(self.arguments, self.tool))
            self.set_class(self.activity_kind == "skill", "skill-call")
            self.base_title = self.status_title("Pending")
        self.permissions.append({"id": event["permissionId"], "decision": event["decision"],
                                 **({"confidence": event["confidence"]} if "confidence" in event else {})})
        self.title = self.authorized_title()
        self.update_file_layout()

    def update_mcp_identity(self, server: Any, tool: Any) -> bool:
        if isinstance(server, str) and isinstance(tool, str):
            self.mcp_identity = (server, tool)
            return True
        return False

    def on_collapsible_expanded(self, event: Collapsible.Expanded) -> None:
        if self.multi_file:
            return
        if event.collapsible is self and not self.arguments_rendered:
            self.arguments_view.update(format_arguments(self.arguments, self.tool,
                                                       (self.result or {}).get("metadata")))
            self.arguments_rendered = True
        if event.collapsible is self and self.result is not None and not self.rendered:
            self.render_result()

    @work(exclusive=True)
    async def render_result(self) -> None:
        if self.result is None or self.multi_file:
            return
        widgets = result_widgets(self.tool, self.arguments, file_result(self.result, self.file_index))
        await self.results.remove_children()
        await self.results.mount_all(widgets)
        self.rendered = True


class ReadView(Static):
    """A read is summarized by its path rather than an expandable result."""

    DEFAULT_CSS = "ReadView { height: 1; }"

    def __init__(self, call_id: str, arguments: dict[str, Any]) -> None:
        self.call_id = call_id
        self.arguments = arguments
        self.path = arguments.get("filePath")
        self.permissions: list[dict[str, Any]] = []
        self.failed = False
        super().__init__(self.label_text(), classes="chat-message read-call")

    def set_result(self, result: dict[str, Any]) -> None:
        self.failed = result["isError"]
        self.permissions = (result.get("metadata") or {}).get("permissions", self.permissions)
        self.update_label()

    def record_permission(self, event: dict[str, Any]) -> None:
        self.permissions.append({"id": event["permissionId"], "decision": event["decision"],
                                 **({"confidence": event["confidence"]} if "confidence" in event else {})})
        self.update_label()

    def update_label(self) -> None:
        self.update(self.label_text())

    def render(self) -> Text:
        return self.label_text(self.content_size.width or None)

    def on_resize(self) -> None:
        self.refresh()

    def label_text(self, width: int | None = None) -> Text:
        text = Text("read", style=Style(color=TITLE_MD, bold=False), no_wrap=True, overflow="crop")
        text.append(styled_tool_title(permission_suffix(self.permissions), self.permissions))
        if self.failed:
            text.append(" · ")
            text.append("Failed", style=Style(color=SLOP_COLOR, bold=False))
        if self.path:
            return append_title_path(text, self.path, width, separator=" · ")
        if width is not None:
            text.truncate(width, overflow="ellipsis")
        return text
