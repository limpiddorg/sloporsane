"""Live construction timeline, distinct from the final validation summary."""
import asyncio
from time import monotonic

from rich.text import Text
from textual.color import Color
from textual.containers import Horizontal, Vertical, VerticalScroll
from .stable_scroll import StableScroll
from textual.widgets import Button, Collapsible, Static

from .chat_collapsible import ChatCollapsible, CompactionDrawer, compaction_record
from .config import TITLE_MD, MISC_MD
from .response_markdown import ResponseMarkdown
from .scan_animation import validation_sweep
from .tool_view import ReadView, ToolView
from .turn_activity import TurnActivity, format_duration


def performance_summary(part):
    if not part.get("complete") or part.get("duration_ms") is None:
        return ""
    summary = format_duration(part["duration_ms"])
    model = part.get("model_statistics", {})
    if model.get("output_tokens") is not None and model.get("duration_ms", 0) > 0:
        rate = f'{model["output_tokens"] * 1000 / model["duration_ms"]:.1f}'.removesuffix(".0")
        summary += f" | {rate} tok/s"
    return summary


class StepActivityMessage(ChatCollapsible):
    """Like historical Slop reasoning, defer Markdown until this drawer opens."""

    def __init__(self, text, title):
        super().__init__(title=title, classes="reasoning", collapsed=True,
                         collapsed_symbol="▸", expanded_symbol="▾")
        self.text = text
        self.built = False

    async def on_collapsible_expanded(self, event):
        if event.collapsible is self and not self.built:
            self.built = True
            await self.query_one(Collapsible.Contents).mount(ResponseMarkdown(self.text))


class SubstepActivity(TurnActivity):
    def __init__(self, messages):
        super().__init__()
        self.tools = {}
        self.messages = []
        self.rendered_messages = 0
        self.append_messages(messages)

    def append_messages(self, messages):
        for message in messages[len(self.messages):]:
            if message.get("model_call"):
                self.toggle.record_model_call(message["model_call"])
            for part in message["content"]:
                if part["type"] == "tool_call" and part["toolName"] == "wait":
                    status = part["arguments"].get("status")
                    self.toggle.set_process_waiting(part["callId"], True,
                                                   status if isinstance(status, str) else "Waiting for process completion")
                elif part["type"] == "tool_result":
                    self.toggle.set_process_waiting(part["callId"], False)
                kind = self.part_kind(part)
                if kind is not None:
                    self.toggle.step_count += 1
                    self.toggle.record_activity(kind)
            if message.get("metadata"):
                self.toggle.finish_generation(message["metadata"])
        self.messages = list(messages)
        if self.toggle.expanded:
            self.show_details()
        elapsed = [message["elapsed_ms"] for message in messages if "elapsed_ms" in message]
        if elapsed:
            self.toggle.duration_ms = max(0, elapsed[-1] - elapsed[0])
            self.toggle.update_elapsed()

    @staticmethod
    def part_kind(part):
        if part["type"] in ("text", "reasoning") and part.get("text", "").strip():
            return "reasoning"
        if part["type"] == "tool_call":
            tool = part["toolName"]
            if tool not in ("report_implementation_progress", "complete_implementation_step"):
                return "read" if tool == "read" else "skill" if tool == "skill" else "tools"
        return None

    def on_activity_toggle_toggled(self, event):
        event.prevent_default()
        super().on_activity_toggle_toggled(event)
        if self.toggle.expanded:
            self.show_details()

    def show_details(self):
        # Collapsed Activities need only their counters, not hundreds of widgets.
        for message in self.messages[self.rendered_messages:]:
            for part in message["content"]:
                self.add_part(part)
        self.rendered_messages = len(self.messages)
        if self.body.is_mounted and self.pending:
            self.body.mount_all(self.pending)
            self.pending.clear()

    def append_detail(self, widget):
        self.pending.append(widget)

    def add_part(self, part):
        kind = part["type"]
        if kind in ("text", "reasoning") and part.get("text", "").strip():
            self.append_detail(StepActivityMessage(part["text"], "Message" if kind == "text" else "Reasoning"))
        elif kind == "tool_call":
            if part["toolName"] in ("report_implementation_progress", "complete_implementation_step"):
                return
            view = ReadView(part["callId"], part["arguments"]) if part["toolName"] == "read" else ToolView(
                part["callId"], part["toolName"], part["arguments"], lazy_arguments=True)
            self.tools[part["callId"]] = view
            self.append_detail(view)
        elif kind == "tool_result" and part["callId"] in self.tools:
            self.tools.pop(part["callId"]).set_result(part)


class SubstepTimeline(Vertical):
    """Keep append-only Activity segments on either side of checkpoints."""
    DEFAULT_CSS = "SubstepTimeline { height: auto; }"

    def __init__(self):
        super().__init__()
        self.messages = []
        self.current = SubstepActivity([])
        self.current.display = False
        self.segments = [self.current]
        self.compaction = None

    def compose(self):
        yield from self.segments

    @property
    def toggle(self):
        return self.current.toggle

    @property
    def body(self):
        return self.current.body

    @property
    def tools(self):
        return self.current.tools

    @property
    def rendered_messages(self):
        return self.current.rendered_messages

    @property
    def compacting(self):
        return self.compaction is not None and self.compaction.running

    def add_segment(self, widget):
        self.segments.append(widget)
        if self.is_mounted:
            self.mount(widget)

    def append_messages(self, messages):
        for message in messages[len(self.messages):]:
            checkpoint = compaction_record(message)
            if checkpoint is None:
                if self.compaction is not None:
                    self.compaction = None
                    self.current = SubstepActivity([])
                    self.add_segment(self.current)
                self.current.display = True
                self.current.append_messages(self.current.messages + [message])
                continue
            status, text = checkpoint
            self.current.toggle.finish_generation()
            if not self.current.messages:
                self.current.display = False
            if status == "started" or self.compaction is None:
                self.compaction = CompactionDrawer()
                self.add_segment(self.compaction)
            if status != "started":
                self.compaction.set_result(text)
        self.messages = list(messages)

    def start_generation(self, info, started):
        if self.compacting:
            return
        if self.compaction is not None:
            self.compaction = None
            self.current = SubstepActivity([])
            self.add_segment(self.current)
        self.current.display = True
        self.toggle.start_generation(info, started)

    def finish_generation(self, duration_ms=None):
        self.toggle.finish_generation()
        if duration_ms is not None and len(self.segments) == 1:
            self.toggle.duration_ms = duration_ms
            self.toggle.update_elapsed()
        if self.compacting:
            self.compaction.running = False
            self.compaction.title = "Compaction"


def substep_progress(parts):
    done = sum(bool(part.get("finalized") or part.get("unnecessary") or part["complete"]) for part in parts)
    return done, len(parts)


UNNECESSARY_DESCRIPTION = "No longer necessary given the changes in previous steps."


class StepProgress(Static):
    """Calculate the text at paint time, after the layout has its final width."""
    done = total = 0

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.validating = False
        self.started_at = 0.0
        self.timer = None

    def on_mount(self):
        self.timer = self.set_interval(1 / 30, self.refresh, pause=not self.validating)

    def set_validating(self, validating):
        if validating == self.validating:
            return
        self.validating = validating
        self.styles.height = 2 if validating else 1
        if validating:
            self.started_at = monotonic()
        if self.timer is not None:
            self.timer.resume() if validating else self.timer.pause()
        self.refresh()

    def render(self):
        width = max(1, self.size.width - 2)
        filled = width * self.done // self.total if self.total else 0
        bar = "[" + "#" * filled + "-" * (width - filled) + "]"
        if not self.validating:
            return Text(bar)
        phase = (monotonic() - self.started_at) % 1  # One second per round trip.
        accent = Color.parse(MISC_MD)
        label = "Validating"[:len(bar)].center(len(bar))
        return validation_sweep(label, phase, accent).append("\n").append(
            validation_sweep(bar, phase, accent))


def construction_state(plan, built):
    """Preparing starts a fresh presentation before the accepted pass arrives."""
    pending = built.get("pending_correction", "")
    request = pending or built.get("correction_request", "")
    if pending:
        empty = {"complete": False, "explanation": "", "activity": []}
        built = dict(built, whole=empty, substeps=[dict(empty) for _ in plan["substeps"]],
                      validation=dict(empty), preparing=True)
    return built, request


class ImplementationStepView(StableScroll):
    DEFAULT_CSS = """
    ImplementationStepView { height: 1fr; padding: 0; scrollbar-gutter: stable; }
    ImplementationStepView > .step-construction { height: auto; min-height: 100%; align: left top; }
    ImplementationStepView .step-overview { height: auto; width: 100%; margin: 1 1; text-align: left; }
    ImplementationStepView .substep-construction { height: auto; width: 100%; margin: 1 0; }
    ImplementationStepView .step-validation { height: auto; width: 100%; margin: 1 0; }
    ImplementationStepView .step-preparation { height: auto; margin: 0 3; }
    ImplementationStepView .step-progress { height: auto; width: 80%; margin: 0 3; }
    ImplementationStepView .step-progress-count { width: 7; margin-right: 1; color: $misc-md; }
    ImplementationStepView .step-progress-bar { width: 1fr; height: 1; color: $misc-md; }
    ImplementationStepView .substep-title, ImplementationStepView .substep-description,
    ImplementationStepView .validation-title { height: auto; margin: 0 1; }
    ImplementationStepView .reasoning > CollapsibleTitle { color: $misc-md; }
    """

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.signature = None
        self.clock = {}
        self.active = None
        self.preparing = False
        self.preparation = None
        self.frame = 0
        self.render_lock = asyncio.Lock()
        self.step_index = None
        self.display_revision = None
        self.rows = []
        self.overview = None
        self.action = None
        self.progress = None
        self.progress_count = None
        self.progress_value = (0, 0)
        self.validation_activity = None
        self.rendered_version = None

    def on_mount(self):
        self.set_interval(.2, self.update_activity_spinner)

    def observe_execution(self, state):
        """Anchor the active timer when state arrives, even off screen."""
        if not state or state["status"] != "running" or state.get("preparing"):
            return
        index = state["work_step"]
        if index >= len(state["steps"]):
            return
        step = state["steps"][index]
        revision = (step.get("identity", index), step.get("display_revision", 0))
        revisions = getattr(self, "clock_revisions", {})
        if revisions.get(index) != revision:
            self.clock = {key: value for key, value in self.clock.items() if key[0] != index}
        revisions[index] = revision
        self.clock_revisions = revisions
        substep = "validation" if step.get("validation", {}).get("started") else state.get("work_substep", 0)
        key = (index, substep)
        if "active_duration_ms" in state:
            self.clock[key] = monotonic() - state["active_duration_ms"] / 1000
        else:
            self.clock.setdefault(key, monotonic())

    def update_activity_spinner(self):
        if self.display and self.preparing:
            self.frame += 1
            spinner = "|/-\\"[self.frame % 4]
            self.preparation.update(Text(f"Preparing implementation... {spinner}", style=MISC_MD))
        if self.active is not None and self.display and not self.active.compacting:
            self.frame += 1
            self.active.toggle.set_spinner(self.frame)
            self.active.toggle.update_elapsed()

    def on_resize(self):
        self.call_after_refresh(self.render_progress)

    def render_progress(self):
        if self.progress is not None:
            self.progress.done, self.progress.total = self.progress_value
            self.progress.refresh()

    async def show_step(self, index, plan, built, running, info, bullet_description, can_resume=False, can_cancel=False):
        async with self.render_lock:
            await self._show_step(index, plan, built, running, info, bullet_description, can_resume, can_cancel)

    async def _show_step(self, index, plan, built, running, info, bullet_description, can_resume, can_cancel):
        selected_state, request = construction_state(plan, built)
        await self.render_version(index, plan, selected_state, running, info,
                                  bullet_description, can_resume, can_cancel, request)

    async def render_version(self, index, plan, built, running, info, bullet_description, can_resume, can_cancel, request):
        signature = (request,
                     self.step_signature(index, plan, built, running, can_resume, can_cancel))
        if signature == self.signature:
            return
        revision = built.get("display_revision", 0)
        if (self.step_index != index or self.display_revision != revision or len(self.rows) != len(plan["substeps"])
                or self.rendered_version != request):
            self.scroll_home(animate=False)
            await self.remove_children()
            self.overview = Static(classes="step-overview")
            self.rows = []
            self.progress = StepProgress(classes="step-progress-bar", markup=False)
            self.progress_count = Static(classes="step-progress-count", markup=False)
            self.preparation = Static(Text("Preparing implementation...", style=MISC_MD), classes="step-preparation")
            children = [self.overview, self.preparation,
                        Horizontal(self.progress_count, self.progress, classes="step-progress")]
            for substep in range(len(plan["substeps"])):
                activity = SubstepTimeline()
                activity.id = f"construction-{index}-{substep}"
                title = Static(classes="substep-title")
                description = Static(classes="substep-description")
                row = Vertical(title, activity, description, classes="substep-construction")
                self.rows.append((row, title, activity, description))
                children.append(row)
            self.validation_activity = SubstepTimeline()
            children.append(Vertical(
                Static(Text("Validating the step", style=f"bold {MISC_MD}"), classes="validation-title"),
                self.validation_activity, classes="step-validation"))
            self.action = Button("", id="implementation-step-action", compact=True)
            self.action.display = False
            children.append(self.action)
            await self.mount(Vertical(*children, classes="step-construction"))
            self.step_index = index
            self.display_revision = revision
            self.rendered_version = request
        self.update_step(index, plan, built, running, info, bullet_description, can_resume, can_cancel, request)
        self.signature = signature

    @staticmethod
    def step_signature(index, plan, built, running, can_resume, can_cancel):
        # Activity records are append-only. Diff patches, repository files and
        # tool outputs do not belong to this overview's render fingerprint.
        def part_signature(part):
            return tuple(part.get(key) for key in (
                "complete", "reviewed", "explanation", "started", "announced", "unnecessary", "finalized")) + (
                len(part.get("activity", [])),)
        return (index, plan["title"], plan["objective"], tuple(p["title"] for p in plan["substeps"]),
                tuple(segment["revision"] for segment in built.get("construction_history", [])),
                built.get("display_revision", 0), built.get("preparing", False), running, can_resume, can_cancel,
                part_signature(built["whole"]), tuple(part_signature(p) for p in built["substeps"]),
                part_signature(built.get("validation", {})))

    def update_step(self, index, plan, built, running, info, bullet_description, can_resume, can_cancel, request):
        overview = Text()
        overview.append(plan["title"] + "\n", style=f"bold {TITLE_MD}")
        overview.append("\n")
        summary = request or built["whole"]["explanation"] or plan["objective"]
        overview.append(bullet_description(summary), style=MISC_MD)
        self.overview.update(overview)
        self.active = None
        self.preparing = running and built.get("preparing", False)
        self.preparation.display = self.preparing
        parts = built["substeps"]
        done, total = substep_progress(parts)
        validation = built.get("validation", {})
        validating = validation.get("started", False)
        if validating or built["whole"]["complete"]:
            done = total
        self.progress_value = (done, total)
        self.progress.set_validating(validating and running and not validation.get("complete", False)
                                     and not built["whole"]["complete"])
        self.progress_count.styles.margin = (int(self.progress.validating), 1, 0, 0)
        self.render_progress()
        self.call_after_refresh(self.render_progress)
        self.progress_count.update(f"{done}/{len(parts)}")
        current = max((i for i, part in enumerate(parts) if part.get("started") and not part.get("unnecessary")), default=0)
        for substep, (definition, part, widgets) in enumerate(zip(plan["substeps"], parts, self.rows)):
            row, title, activity, description = widgets
            unnecessary = part.get("unnecessary", False)
            row.display = True
            title.update(Text(f'{substep + 1}. {definition["title"]}',
                              style="dim strike" if unnecessary else f"bold {TITLE_MD}"))
            activity.append_messages(part.get("activity", []))
            active = substep == current and not unnecessary and not validating and not self.preparing and (running or can_resume)
            activity.display = bool(activity.messages) or active
            if running and active:
                self.active = activity
                started = self.clock.setdefault((index, substep), monotonic())
                activity.start_generation(info, started)
            else:
                activity.finish_generation(part.get("duration_ms"))
            description.display = bool(part["complete"] or part.get("announced") or unnecessary)
            explanation = bullet_description(part["explanation"])
            if unnecessary:
                explanation = UNNECESSARY_DESCRIPTION
            description.update(Text(explanation))
        self.show_validation(index, built.get("validation", {}), running, can_resume, info)
        self.action.display = can_resume or can_cancel
        self.action.label = "Continue" if can_resume else "Cancel"
        self.action.set_class(can_resume, "implementation-continue")
        self.action.set_class(can_cancel, "implementation-cancel")
        if not can_resume:
            self.action.disabled = False

    def show_validation(self, index, validation, running, can_resume, info):
        activity = self.validation_activity
        activity.append_messages(validation.get("activity", []))
        active = validation.get("started", False) and (running or can_resume)
        activity.display = bool(activity.messages) or active
        if running and active:
            self.active = activity
            started = self.clock.setdefault((index, "validation"), monotonic())
            activity.start_generation(info, started)
        else:
            activity.finish_generation(validation.get("duration_ms"))
