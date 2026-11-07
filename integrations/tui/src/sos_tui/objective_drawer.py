"""A frozen Objective Plan at the end of a completed Discussion turn."""

from copy import deepcopy

from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Collapsible, Rule

from .chat_collapsible import ChatCollapsible
from .response_markdown import ResponseMarkdown


class ObjectivePlanDrawer(ChatCollapsible):
    DEFAULT_CSS = """
    ObjectivePlanDrawer { height: auto; margin: 0 1; }
    ObjectivePlanDrawer > Rule.objective-separator.-horizontal { height: 1; margin: 0; padding: 0; color: $muted; }
    ObjectivePlanDrawer.updated > Rule.objective-separator.-horizontal { color: $sane; }
    ObjectivePlanDrawer .objective-header { height: 1; margin: 0 1; }
    ObjectivePlanDrawer > Contents { margin: 0 1; }
    ObjectivePlanDrawer .objective-header CollapsibleTitle { width: 1fr; }
    .conversation-history ObjectivePlanDrawer CollapsibleTitle,
    .conversation-history ObjectivePlanDrawer CollapsibleTitle:hover,
    .conversation-history ObjectivePlanDrawer CollapsibleTitle:focus { color: $muted; text-style: bold; }
    .conversation-history ObjectivePlanDrawer.updated CollapsibleTitle,
    .conversation-history ObjectivePlanDrawer.updated CollapsibleTitle:hover,
    .conversation-history ObjectivePlanDrawer.updated CollapsibleTitle:focus { color: $sane; }
    ObjectivePlanDrawer .objective-document { height: auto; border-left: outer $sane;
        background: $sane 6%; padding: 0 1; }
    ObjectivePlanDrawer ResponseMarkdown { height: auto; margin: 0; padding: 0; }
    ObjectivePlanDrawer .objective-link { height: 1; min-height: 1; width: auto; min-width: 0;
        border: none; padding: 0 1; background: $depth-md; color: white; }
    ObjectivePlanDrawer .objective-actions { height: auto; align: center middle; margin: 1 0; }
    ObjectivePlanDrawer .objective-actions Button { height: 1; min-height: 1; width: 33;
        max-width: 100%; min-width: 0; border: none; padding: 0 1; background: $depth-md; color: white; }
    """

    def __init__(self, plan: dict, updated: bool):
        self.plan = deepcopy(plan)
        self.updated = updated
        self.built = False
        super().__init__(title="Objective plan" + (" · updated" if updated else ""), collapsed=True,
                         collapsed_symbol="▸", expanded_symbol="▾", classes="chat-message")
        self.set_class(updated, "updated")

    def compose(self):
        yield Rule(classes="objective-separator")
        with Horizontal(classes="objective-header"):
            yield self._title
            button = Button("Implementation", classes="objective-link", compact=True)
            button.can_focus = False
            button.display = False
            yield button
        yield Collapsible.Contents()

    def on_mount(self):
        super().on_mount()
        self.sync()

    @property
    def selected_id(self):
        return self.plan["id"]

    def selected_branch(self):
        return next((branch["id"] for branch in self.app.runtime_info.get("implementation_branches", [])
                     if branch["objective_id"] == self.selected_id), "")

    def sync(self):
        if not self.is_mounted:
            return
        info = self.app.runtime_info
        branch = self.selected_branch()
        preview = info.get("implementation_view", False)
        self.query_one(".objective-link").display = bool(branch) and not preview
        if self.built:
            self.query_one(".objective-actions").display = not branch and not preview
        active = info.get("active_implementation_id", "")
        disabled = self.app.busy or not self.app.connected or bool(active and not branch)
        for button in self.query(Button):
            button.disabled = disabled

    async def on_collapsible_expanded(self, event: Collapsible.Expanded):
        if event.collapsible is not self or self.built:
            return
        text = "\n".join(part["text"] for part in self.plan["content"] if part["type"] == "text")
        if self.plan.get("objective_stack"):
            text += "\n\n## Stack\n\n" + self.plan["objective_stack"]
        actions = Vertical(Button("Implementation plan", classes="objective-implement", compact=True),
                           Button("Implement (Auto plan)", classes="objective-auto-implement", compact=True),
                           classes="objective-actions")
        document = Vertical(ResponseMarkdown(text), actions, classes="objective-document")
        await self.query_one(Collapsible.Contents).mount(document)
        for button in self.query(Button):
            button.can_focus = False
        self.built = True
        self.sync()

    async def on_button_pressed(self, event: Button.Pressed):
        event.stop()
        branch, objective_id = self.selected_branch(), self.selected_id
        if event.button.has_class("objective-link"):
            if branch:
                await self.app.implementation.request("implementation.view", branch)
        elif event.button.has_class("objective-auto-implement"):
            from .implementation_workspace import ImplementationModeScreen

            async def chosen(choice):
                if choice is not None:
                    await self.app.implementation.request("implementation.view" if branch else "implementation.open",
                        branch, objective_id=objective_id, auto_plan_pace=("step_by_step", "everything")[choice])
            self.app.push_screen(ImplementationModeScreen(), chosen)
        else:
            await self.app.implementation.request("implementation.view" if branch else "implementation.open",
                                                 branch, generate=True, objective_id=objective_id)
