"""Window eviction, lazy activities, and bidirectional page replacement."""

import asyncio
import json
import unittest
from unittest.mock import patch

from textual.app import App
from textual import events
from sos_tui.history_window import HistoryWindow
from sos_tui.history_viewport import HistoryActivity, HistoryViewport
from sos_tui.tool_view import ToolView
from sos_tui.user_message import UserMessage
from sos_tui.app import SosApp
from sos_tui.message_input import MessageInput
from sos_tui.response_markdown import ResponseMarkdown
from sos_tui.history_viewport import HistoryCompaction
from sos_tui.history_loading_bar import HistoryLoadingBar
from sos_tui.config import QUOTE_BACKGROUND_COLOR, MISC_MD


def page(start=0, count=50, older=False, newer=False):
    return {"session": {"id": "s"}, "page": {"first": f"m{start}", "last": f"m{start + count - 1}",
            "hasOlder": older, "hasNewer": newer},
            "messages": [{"id": f"m{i}", "role": "user", "content": [{"type": "text", "text": f"Message {i}"}]}
                         for i in range(start, start + count)]}


def group(loaded=False):
    result = {"id": "g", "role": "activity", "content": [], "count": 1, "kinds": ["tools"]}
    if loaded:
        result["activities"] = [{"kind": "tool", "callId": "c", "toolName": "bash", "arguments": {"command": "true"},
                                  "result": {"output": "ok", "isError": False}}]
    return result


def respond(window, request, result, purpose="page", group_id="", activity_index=None):
    base = {"requestId": request["requestId"], "sessionId": "s"}
    window.accept({**base, "type": "session.attach_started", "session": {"id": "s"},
                   "messageCount": len(result["messages"]), "page": result.get("page", {}),
                   "purpose": purpose, "groupId": group_id, "activityIndex": activity_index})
    for index, record in enumerate(result["messages"]):
        window.accept({**base, "type": "session.history_chunk", "messageIndex": index, "partIndex": 0,
                       "last": True, "data": json.dumps(record)})
    window.accept({**base, "type": "session.attached", "messageCount": len(result["messages"])})


class HistoryWindowTest(unittest.IsolatedAsyncioTestCase):
    async def test_purge_drops_preloaded_and_requested_payloads_without_losing_window(self):
        window = HistoryWindow()
        initial = page()
        initial["messages"].append(group(True))
        window.install(initial)
        bounds = dict(window.page)
        window.clear_activity_cache()
        self.assertNotIn("activities", window.records[-1])
        self.assertEqual(window.page, bounds)
        self.assertEqual(window.session_id, "s")
        sent = []
        async def send(request):
            sent.append(request)
        task = asyncio.create_task(window.activities(send, "g"))
        while not sent:
            await asyncio.sleep(0)
        window.clear_activity_cache()
        with self.assertRaises(asyncio.CancelledError):
            await task
        respond(window, sent[0], {"messages": [group(True)]}, "activities", "g")
        self.assertNotIn("activities", window.records[-1])
        self.assertFalse(window.groups)
        self.assertFalse(window.pending)

    async def test_navigation_uses_stable_anchors_and_releases_previous_records(self):
        window = HistoryWindow()
        window.install(page(100, older=True))
        sent = []
        async def send(request):
            sent.append(request)
            respond(window, request, page(50, older=True, newer=True))
        old = window.records
        result = await window.navigate(send, "before")
        self.assertEqual(sent[0]["anchor"], "m100")
        self.assertIs(window.records, old)  # Do not replace a visible page before rendering succeeds.
        window.install(result)
        self.assertEqual(len(window.records), 50)
        self.assertIsNot(window.records, old)
        self.assertFalse(window.latest)
        self.assertFalse(window.transfers)

    async def test_lazy_activity_is_cached_then_evicted_even_after_explicit_loading(self):
        window = HistoryWindow()
        initial = page()
        initial["messages"].append(group())
        window.install(initial)
        sent = []
        async def send(request):
            sent.append(request)
            respond(window, request, {"messages": [group(True)]}, "activities", "g")
        await window.activities(send, "g")
        await window.activities(send, "g")
        self.assertEqual(len(sent), 1)
        window.install(page(50))
        self.assertFalse(any(item["id"] == "g" for item in window.records))

    async def test_late_partial_transfers_are_ignored_after_invalidation(self):
        window = HistoryWindow()
        initial = page()
        initial["messages"].append(group())
        window.install(initial)
        started = asyncio.Event()
        requests = []
        async def send(request):
            requests.append(request)
            started.set()
        task = asyncio.create_task(window.activities(send, "g"))
        await started.wait()
        window.invalidate()
        with self.assertRaises(asyncio.CancelledError):
            await task
        respond(window, requests[0], {"messages": [group(True)]}, "activities", "g")
        self.assertNotIn("activities", window.records[-1])
        self.assertFalse(window.transfers)

    async def test_request_failure_preserves_page_and_allows_retry(self):
        window = HistoryWindow()
        window.install(page())
        async def send(request):
            window.accept({"type": "request.failed", "requestId": request["requestId"], "error": "failed"})
        with self.assertRaises(ValueError):
            await window.navigate(send, "before")
        self.assertEqual(window.records[0]["id"], "m0")
        self.assertFalse(window.loading)
        self.assertFalse(window.pending)


class ViewportApp(App):
    def get_css_variables(self):
        return {**super().get_css_variables(), "muted": "#999999", "content-background": "#111111",
                "quote-background": QUOTE_BACKGROUND_COLOR, "sane": "#05b07d"}
    def __init__(self):
        super().__init__()
        self.window = HistoryWindow()
        self.sent = []
        self.directions = []

    async def send(self, request):
        self.sent.append(request)
        respond(self.window, request, {"messages": [group(True)]}, "activities", "g")

    def compose(self):
        yield HistoryViewport(self.window, self.send, id="chat")

    def on_history_viewport_navigate(self, event):
        self.directions.append(event.direction)


class HistoryViewportTest(unittest.IsolatedAsyncioTestCase):
    async def test_mouse_and_keyboard_scrolling_remain_available(self):
        app = ViewportApp()
        async with app.run_test(size=(80, 8)) as pilot:
            viewport = app.query_one(HistoryViewport)
            await viewport.replace(page(count=20))
            viewport.scroll_home(animate=False, immediate=True)
            viewport.focus()
            await pilot.pause()
            self.assertTrue(viewport.has_focus)
            await pilot.press("pagedown")
            await pilot.pause()
            self.assertGreater(viewport.scroll_y, 0)
            viewport.scroll_home(animate=False, immediate=True)
            await pilot.pause()
            await pilot._post_mouse_events([events.MouseScrollDown], viewport, offset=(5, 1))
            await pilot.pause()
            self.assertGreater(viewport.scroll_y, 0)

    async def test_scroll_waits_for_fetch_layout_and_paint_then_ignores_buffered_inputs(self):
        app = ViewportApp()
        async with app.run_test(size=(80, 8)) as pilot:
            viewport = app.query_one(HistoryViewport)
            await viewport.replace(page(100, count=20, older=True))
            viewport.scroll_home(animate=False, immediate=True)
            await pilot.pause()
            anchor = viewport.children[0]
            y = anchor.region.y
            viewport.boundary("before")
            self.assertTrue(viewport.scroll_locked)
            viewport.set_history_loading(True)
            viewport.window.loading = True
            old_wheel = events.MouseScrollDown(viewport, 0, 0, 0, 1, 0, False, False, False)
            old_key = events.Key("down", None)
            for action in (viewport.action_scroll_down, viewport.action_page_down, viewport.action_scroll_end,
                           viewport.action_scroll_up, viewport.action_page_up, viewport.action_scroll_home):
                action()
            viewport._on_mouse_scroll_down(old_wheel)
            await pilot.pause()
            self.assertEqual(viewport.scroll_y, 0)
            self.assertEqual(app.directions, ["before"])
            viewport.window.loading = False
            finished = asyncio.Event()
            callbacks = []
            original_finish = viewport.finish_positioning
            def hold_completion(positioned):
                callbacks.append(positioned)
                finished.set()
            with patch.object(viewport, "finish_positioning", hold_completion):
                task = asyncio.create_task(viewport.replace(page(90, count=20, older=True, newer=True), "before"))
                try:
                    await asyncio.wait_for(finished.wait(), 2)
                    self.assertFalse(task.done())
                    self.assertTrue(viewport.scroll_locked)
                    self.assertEqual(anchor.region.y, y)
                    position = viewport.scroll_y
                    viewport.action_scroll_down()
                    viewport._on_mouse_scroll_down(old_wheel)
                    self.assertEqual(viewport.scroll_y, position)
                finally:
                    for callback in callbacks:
                        original_finish(callback)
                    await asyncio.wait_for(task, 2)
            self.assertTrue(viewport.scroll_locked)  # The app still owns the fetch lock.
            viewport.set_history_loading(False)
            self.assertFalse(viewport.scroll_locked)
            position = viewport.scroll_y
            viewport._on_mouse_scroll_down(old_wheel)
            self.assertTrue(viewport.ignore_scroll_event(old_key))
            viewport.boundary("after", old_wheel.time)
            await pilot.pause()
            self.assertEqual(viewport.scroll_y, position)
            self.assertEqual(app.directions, ["before"])
            fresh = events.MouseScrollUp(viewport, 0, 0, 0, -1, 0, False, False, False)
            viewport._on_mouse_scroll_up(fresh)
            await pilot.pause()
            self.assertLess(viewport.scroll_y, position)

    async def test_purge_drains_inflight_population_so_old_widgets_cannot_reappear(self):
        app = ViewportApp()
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            initial = page(0, 1)
            initial["messages"].append(group(True))
            await viewport.replace(initial)
            await pilot.pause()
            drawer = app.query_one(HistoryActivity)
            entered, release = asyncio.Event(), asyncio.Event()
            original = drawer.populate
            async def delayed_population(record):
                entered.set()
                await release.wait()
                await original(record)
            with patch.object(drawer, "populate", delayed_population):
                drawer.toggle.action_toggle()
                await asyncio.wait_for(entered.wait(), 2)
                try:
                    await asyncio.wait_for(viewport.purge_activities(), 2)
                finally:
                    release.set()
            await pilot.pause()
            self.assertFalse(drawer.loading)
            self.assertFalse(drawer.built)
            self.assertEqual(len(drawer.body.children), 0)
            self.assertNotIn("activities", app.window.records[-1])

    async def test_purge_keeps_drawer_but_clears_its_widgets_and_refetches_on_open(self):
        app = ViewportApp()
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            initial = page(0, 1)
            initial["messages"].append(group(True))
            await viewport.replace(initial)
            await pilot.pause()
            user = app.query_one(UserMessage)
            drawer = app.query_one(HistoryActivity)
            drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertTrue(drawer.built)
            self.assertEqual(app.sent, [])
            await viewport.purge_activities()
            self.assertIs(app.query_one(UserMessage), user)
            self.assertIs(app.query_one(HistoryActivity), drawer)
            self.assertFalse(drawer.toggle.expanded)
            self.assertFalse(drawer.built)
            self.assertEqual(len(drawer.body.children), 0)
            self.assertNotIn("activities", app.window.records[-1])
            self.assertNotIn("activities", viewport.rendered[-1][0])
            drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertTrue(drawer.built)
            self.assertEqual(len(app.sent), 1)

    async def test_preloaded_last_group_opens_locally_but_older_group_is_requested(self):
        app = ViewportApp()
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            result = page(0, 2)
            latest = {**group(True), "id": "last-group"}
            result["messages"].insert(1, group())
            result["messages"].append(latest)
            await viewport.replace(result)
            await pilot.pause()
            old_drawer, latest_drawer = list(app.query(HistoryActivity))
            self.assertEqual(len(app.query(ToolView)), 0)
            latest_drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertTrue(latest_drawer.built)
            self.assertEqual(len(latest_drawer.query(ToolView)), 1)
            self.assertEqual(app.sent, [])
            old_drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertEqual(len(app.sent), 1)
            self.assertEqual(app.sent[0]["type"], "session.activities")
            self.assertEqual(app.sent[0]["groupId"], "g")
            self.assertTrue(old_drawer.built)

    async def test_sliding_keeps_open_retained_activity_and_its_loaded_payload(self):
        incomplete = group(True)
        del incomplete["activities"][0]["result"]
        self.assertFalse(HistoryViewport.same_record(incomplete, group()))
        app = ViewportApp()
        async with app.run_test(size=(80, 20)) as pilot:
            viewport = app.query_one(HistoryViewport)
            initial = page(100, older=True)
            initial["messages"].insert(1, group(True))
            await viewport.replace(initial)
            await pilot.pause()
            drawer = app.query_one(HistoryActivity)
            await drawer.populate(initial["messages"][1])
            drawer.toggle.action_toggle()
            viewport.scroll_home(animate=False, immediate=True)
            await pilot.pause()
            y = drawer.region.y
            tool = drawer.query_one(ToolView)
            older = page(75, older=True, newer=True)
            older["messages"].insert(26, group())
            await viewport.replace(older, "before")
            await pilot.pause()
            self.assertIs(app.query_one(HistoryActivity), drawer)
            self.assertIs(drawer.query_one(ToolView), tool)
            self.assertTrue(drawer.toggle.expanded)
            self.assertEqual(drawer.region.y, y)
            self.assertIn("activities", app.window.records[26])

    async def test_overlapping_windows_keep_scroll_anchor_and_do_not_bounce_back(self):
        app = ViewportApp()
        async with app.run_test(size=(80, 20)) as pilot:
            viewport = app.query_one(HistoryViewport)
            await viewport.replace(page(100, older=True))
            await pilot.pause()
            viewport.scroll_home(animate=False, immediate=True)
            await pilot.pause()
            retained = dict((record["id"], widget) for record, widget in viewport.rendered)
            y = retained["m100"].region.y
            painted_positions = []
            refresh = app.screen._compositor_refresh
            def record_paint(*args, **kwargs):
                if app.window.page.get("first") == "m75":
                    painted_positions.append(retained["m100"].region.y)
                return refresh(*args, **kwargs)
            with patch.object(app.screen, "_compositor_refresh", record_paint):
                await viewport.replace(page(75, older=True, newer=True), "before")
            self.assertTrue(painted_positions)
            self.assertTrue(all(position == y for position in painted_positions), painted_positions)
            await pilot.pause()
            self.assertEqual(retained["m100"].region.y, y)
            self.assertEqual(len(viewport.children), 50)
            self.assertEqual(sum(widget in viewport.children for widget in retained.values()), 25)
            self.assertGreater(viewport.scroll_y, 0)
            self.assertLess(viewport.scroll_y, viewport.max_scroll_y)
            viewport.action_scroll_down()
            await pilot.pause()
            self.assertEqual(app.directions, [])
            viewport.scroll_end(animate=False, immediate=True)
            await pilot.pause()
            y = retained["m124"].region.y
            await viewport.replace(page(100, older=True), "after")
            await pilot.pause()
            self.assertEqual(retained["m124"].region.y, y)
            self.assertGreater(viewport.scroll_y, 0)
            self.assertLess(viewport.scroll_y, viewport.max_scroll_y)
            viewport.action_scroll_up()
            await pilot.pause()
            self.assertEqual(app.directions, [])

    async def test_unchanged_widgets_are_reused_and_closed_summaries_are_lazy(self):
        app = ViewportApp()
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            result = page(0, 1)
            result["messages"].append({"id": "summary", "role": "assistant", "compaction": {},
                                       "content": [{"type": "text", "text": "## Summary\n\n" * 100}]})
            await viewport.replace(result)
            user = app.query_one(UserMessage)
            summary = app.query_one(HistoryCompaction)
            self.assertEqual(len(summary.query(ResponseMarkdown)), 0)
            await viewport.replace(result)
            self.assertIs(app.query_one(UserMessage), user)
            self.assertIs(app.query_one(HistoryCompaction), summary)
            summary.collapsed = False
            await pilot.pause()
            self.assertEqual(len(summary.query(ResponseMarkdown)), 1)
            await viewport.replace(page(50))
            self.assertFalse(summary.is_attached)
            self.assertEqual(len(viewport.rendered), 50)

    async def test_old_page_remains_visible_until_markdown_is_ready(self):
        app = ViewportApp()
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            await viewport.replace(page(0, 1))
            await pilot.pause()
            old = app.query_one(UserMessage)
            region = old.region
            entered, release = asyncio.Event(), asyncio.Event()
            original = ResponseMarkdown._on_mount
            async def slow_mount(widget, event):
                entered.set()
                await release.wait()
                await original(widget, event)
            result = page(50, 1)
            result["messages"][0]["role"] = "assistant"
            with patch.object(ResponseMarkdown, "_on_mount", slow_mount):
                task = asyncio.create_task(viewport.replace(result))
                await asyncio.wait_for(entered.wait(), 2)
                try:
                    # pilot.pause waits for idle message queues, including this
                    # deliberately blocked mount. Let the compositor run instead.
                    await asyncio.sleep(0.05)
                    self.assertTrue(old.is_attached)
                    self.assertTrue(old.display)
                    self.assertEqual(old.region, region)
                    self.assertEqual(app.window.page["first"], "m0")
                    self.assertFalse(app.query_one(ResponseMarkdown).display)
                finally:
                    release.set()
                    await task
            self.assertFalse(old.is_attached)
            self.assertTrue(app.query_one(ResponseMarkdown).display)
            self.assertEqual(app.window.page["first"], "m50")

    async def test_closed_drawers_have_no_tool_widgets_and_fetch_once_on_open(self):
        app = ViewportApp()
        async with app.run_test() as pilot:
            viewport = app.query_one(HistoryViewport)
            result = page(0, 1)
            result["messages"].append(group())
            await viewport.replace(result)
            await pilot.pause()
            self.assertEqual(len(app.query(ToolView)), 0)
            drawer = app.query_one(HistoryActivity)
            drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertEqual(len(app.sent), 1)
            self.assertEqual(len(app.query(ToolView)), 1)
            drawer.toggle.action_toggle()
            drawer.toggle.action_toggle()
            await pilot.pause()
            self.assertEqual(len(app.sent), 1)
            viewport.close_activities()
            self.assertFalse(drawer.toggle.expanded)
            await viewport.replace(page(50))
            await pilot.pause()
            self.assertEqual(len(app.query(ToolView)), 0)
            self.assertEqual(len(app.query(HistoryActivity)), 0)
            self.assertEqual(len(app.query(UserMessage)), 50)

    async def test_replacement_positions_boundary_without_triggering_navigation(self):
        app = ViewportApp()
        async with app.run_test(size=(80, 20)) as pilot:
            viewport = app.query_one(HistoryViewport)
            await viewport.replace(page(50, older=True, newer=True), "before")
            await pilot.pause()
            self.assertEqual(viewport.scroll_y, viewport.max_scroll_y)
            self.assertEqual(app.directions, [])
            viewport.boundary("after")
            self.assertEqual(viewport.navigation_pending, True)
            await pilot.pause()
            self.assertEqual(app.directions, ["after"])
            await viewport.replace(page(100, older=True), "after")
            await pilot.pause()
            self.assertEqual(viewport.scroll_y, 0)
            self.assertEqual(len(app.query(UserMessage)), 50)
            self.assertEqual(app.directions, ["after"])


class HistoryAppTest(unittest.IsolatedAsyncioTestCase):
    async def test_history_clicks_do_not_focus_or_restyle_the_viewport(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                app.connected = True
                entry = app.query_one(MessageInput)
                entry.disabled = False
                entry.focus()
                viewport = app.query_one(HistoryViewport)
                result = page(count=20)
                for message in result["messages"]:
                    message.update(role="assistant", content=[{"type": "text", "text": "# Heading\n\nA paragraph.\n\nAnother paragraph."}])
                await viewport.replace(result)
                await pilot.pause()
                for state in ("idle", "busy", "loading"):
                    with self.subTest(state=state):
                        app.busy = state == "busy"
                        loading = app.begin_history_loading() if state == "loading" else None
                        await pilot.pause()
                        focused = app.screen.focused
                        with patch.object(viewport, "watch_has_focus", wraps=viewport.watch_has_focus) as focus_change, \
                             patch.object(viewport, "update_node_styles", wraps=viewport.update_node_styles) as restyle:
                            # Both the empty padding and rendered Markdown must leave focus alone.
                            await pilot.click(viewport, offset=(0, 1))
                            await pilot.click(viewport, offset=(5, 2))
                            self.assertIs(app.screen.focused, focused)
                            focus_change.assert_not_called()
                            restyle.assert_not_called()
                        if loading is not None:
                            app.end_history_loading(loading)

    async def test_history_selection_copy_paste_and_links_keep_working(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(100, 40)) as pilot:
                app.connected = True
                entry = app.query_one(MessageInput)
                entry.disabled = False
                entry.focus()
                result = page(count=1)
                result["messages"][0].update(role="assistant", content=[{
                    "type": "text", "text": "Copy this text.\n\nSecond paragraph.\n\n[Open link](https://example.com/)"}])
                await app.query_one(HistoryViewport).replace(result)
                await pilot.pause()
                first, second, link = app.query_one(ResponseMarkdown).children
                await pilot.mouse_down(first, offset=(0, 0))
                await pilot.mouse_up(second, offset=(17, 0))
                selected = app.clipboard
                self.assertIn("Copy this text.", selected)
                self.assertIn("Second paragraph.", selected)
                self.assertTrue(entry.has_focus)
                self.assertIsNone(app.screen.get_selected_text())
                await pilot.click(entry)
                app.post_message(events.Paste(app.clipboard))
                await pilot.pause()
                self.assertEqual(entry.text, selected)
                with patch.object(app, "open_url") as open_url:
                    await pilot.click(link, offset=(2, 0))
                    open_url.assert_called_once_with("https://example.com/")
                self.assertTrue(entry.has_focus)

    async def test_live_messages_slide_into_window_and_evict_only_oldest_messages(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(80, 24)) as pilot:
                app.connected = True
                app.session_id = "s"
                viewport = app.query_one(HistoryViewport)
                limit = viewport.WINDOW_SIZE
                await viewport.replace(page(count=limit))
                await pilot.pause()
                old = list(app.query(UserMessage))
                app.display_live_messages([{"id": f"m{limit}", "role": "user", "content": [{"type": "text", "text": "Next request"}]}])
                await pilot.pause()
                self.assertEqual(len(app.query(UserMessage)), limit)
                self.assertFalse(old[0].is_attached)
                self.assertTrue(all(widget.is_attached for widget in old[1:]))
                self.assertEqual(app.history_window.page["first"], "m1")
                self.assertEqual(app.history_window.page["last"], f"m{limit}")
                self.assertTrue(app.history_window.page["hasOlder"])
                for _ in range(5):
                    if viewport.scroll_y == viewport.max_scroll_y:
                        break
                    await pilot.pause()
                self.assertEqual(viewport.scroll_y, viewport.max_scroll_y)
                # Several text parts in one assistant message still count as one
                # message, matching the daemon's pagination unit.
                app.display_live_messages([{"id": f"m{limit + 1}", "role": "assistant", "content": [
                    {"type": "text", "text": "First part"}, {"type": "text", "text": "Second part"}]}])
                await pilot.pause()
                self.assertEqual(len(set(viewport.message_ids.values())), limit)
                self.assertEqual(len(app.query(UserMessage)), limit - 1)
                self.assertFalse(old[1].is_attached)
                self.assertEqual(app.history_window.page["first"], "m2")
                self.assertEqual(app.history_window.page["last"], f"m{limit + 1}")
                for _ in range(5):
                    if viewport.scroll_y == viewport.max_scroll_y:
                        break
                    await pilot.pause()
                self.assertEqual(viewport.scroll_y, viewport.max_scroll_y)

    async def test_scroll_pagination_is_silent_and_preserves_editing(self):
        for direction in ("before", "after"):
            for outcome in ("success", "failure", "cancel"):
                with self.subTest(direction=direction, outcome=outcome):
                    app = SosApp()
                    with patch.object(app, "listen"):
                        async with app.run_test(size=(80, 24)) as pilot:
                            app.connected = True
                            app.session_id = "s"
                            entry = app.query_one(MessageInput)
                            entry.disabled = False
                            entry.text = "Draft"
                            entry.move_cursor((0, 5))
                            entry.focus()
                            app.update_agent_info({"last_turn_used_context": 1200, "total_context": 100000})
                            viewport = app.query_one(HistoryViewport)
                            await viewport.replace(page(10, 20, older=True, newer=True))
                            await pilot.pause()
                            old = list(app.query(UserMessage))
                            context_visible = app.query_one("#context-info").display
                            regions = (viewport.region, app.query_one("#composer").region,
                                       app.query_one("#context-details").region)
                            sent = []
                            async def send(request):
                                sent.append(request)
                            app.client.send = send
                            with patch.object(app, "set_status") as status, patch.object(entry, "focus") as focus:
                                task = asyncio.create_task(app.load_history_page(direction))
                                while not sent:
                                    await asyncio.sleep(0)
                                await pilot.pause()
                                self.assertFalse(app.query_one("#history-progress").display)
                                self.assertEqual(app.query_one("#context-info").display, context_visible)
                                self.assertFalse(entry.disabled)
                                self.assertTrue(entry.has_focus)
                                self.assertTrue(viewport.scroll_locked)
                                self.assertTrue(all(widget.is_attached for widget in old))
                                self.assertEqual(regions, (viewport.region, app.query_one("#composer").region,
                                                           app.query_one("#context-details").region))
                                status.assert_not_called()
                                await pilot.press("x", "enter")
                                self.assertEqual(entry.text, "Draftx")
                                self.assertEqual(len(sent), 1)  # Editing is allowed, concurrent submission is not.
                                if outcome == "success":
                                    respond(app.history_window, sent[0], page(0 if direction == "before" else 20, 20,
                                                                           older=True, newer=True))
                                elif outcome == "failure":
                                    app.history_window.accept({"type": "request.failed", "requestId": sent[0]["requestId"],
                                                               "error": "failed"})
                                else:
                                    task.cancel()
                                self.assertEqual(await task, outcome == "success")
                                await pilot.pause()
                                if outcome == "failure":
                                    status.assert_called_once_with("Could not load history: failed")
                                else:
                                    status.assert_not_called()
                                focus.assert_not_called()
                            self.assertFalse(app.query_one("#history-progress").display)
                            self.assertFalse(entry.disabled)
                            self.assertTrue(entry.has_focus)
                            self.assertEqual(entry.text, "Draftx")
                            self.assertFalse(viewport.scroll_locked)
                            self.assertFalse(app.history_rendering)
                            self.assertFalse(app.history_loads)
                            if outcome != "success":
                                self.assertTrue(all(widget.is_attached for widget in old))

    async def test_loading_bar_preserves_draft_and_layout_and_recovers_on_failure(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(80, 24)) as pilot:
                app.connected = True
                app.session_id = "s"
                # This test exercises centering in a full-width conversation.
                app.set_mode("slop")
                entry = app.query_one(MessageInput)
                entry.disabled = False
                entry.text = "A draft\nwith three\nlines"
                viewport = app.query_one(HistoryViewport)
                await viewport.replace(page(0, 1))
                await pilot.pause()
                chat_region = viewport.region
                composer_region = app.query_one("#composer").region
                old = app.query_one(UserMessage)
                sent = []
                async def send(request):
                    sent.append(request)
                app.client.send = send
                task = asyncio.create_task(app.load_history_page("latest"))
                while not sent:
                    await asyncio.sleep(0)
                await pilot.pause()
                progress = app.query_one("#history-progress")
                self.assertTrue(progress.display)
                self.assertTrue(entry.visible)
                self.assertTrue(entry.disabled)
                self.assertEqual(viewport.region, chat_region)
                self.assertEqual(app.query_one("#composer").region, composer_region)
                self.assertIsInstance(progress, HistoryLoadingBar)
                self.assertEqual(progress.region.y, app.query_one("#agent-info").region.y)
                bar = app.query_one("#agent-info")
                self.assertLessEqual(abs(progress.region.x + progress.region.width / 2 - (bar.region.x + bar.region.width / 2)), 1)
                self.assertFalse(app.query_one("#context-info").display)
                self.assertFalse(app.query_one("#context-percent").display)
                progress.timer.pause()
                progress.phase = 16
                self.assertEqual(progress.render().plain, "[" + "#" * 11 + " " * 11 + "]")
                self.assertEqual(progress.render().style, f"bold {MISC_MD}")
                progress.phase = 32
                self.assertEqual(progress.render().plain, "[" + "#" * 22 + "]")
                self.assertTrue(app.query_one("#mode-indicator").visible)
                # Incoming usage updates must not replace the loader, and must
                # be displayed when loading ends rather than restoring stale text.
                app.update_agent_info({"last_turn_used_context": 1200, "total_context": 100000})
                self.assertFalse(app.query_one("#context-info").display)
                self.assertTrue(progress.display)
                self.assertTrue(old.is_attached)
                await pilot.resize_terminal(32, 18)
                await pilot.pause()
                chat_region = viewport.region
                self.assertGreaterEqual(progress.region.width, 3)
                self.assertLessEqual(progress.region.right, app.query_one("#mode-button").region.x)
                app.history_window.accept({"type": "request.failed", "requestId": sent[0]["requestId"], "error": "failed"})
                self.assertFalse(await task)
                await pilot.pause()
                self.assertFalse(progress.display)
                self.assertTrue(app.query_one("#mode-indicator").visible)
                self.assertTrue(entry.visible)
                self.assertFalse(entry.disabled)
                self.assertEqual(entry.text, "A draft\nwith three\nlines")
                self.assertEqual(viewport.region, chat_region)
                self.assertFalse(app.history_loads)
                self.assertFalse(viewport.scroll_locked)
                await pilot.resize_terminal(80, 24)
                await pilot.pause()
                self.assertTrue(app.query_one("#context-info").display)
                self.assertIn("1 200 / 100 000", str(app.query_one("#context-info").render()))
                task = asyncio.create_task(app.load_history_page("latest"))
                while len(sent) < 2:
                    await asyncio.sleep(0)
                task.cancel()
                self.assertFalse(await task)
                self.assertFalse(progress.display)
                self.assertTrue(entry.visible)
                self.assertFalse(app.history_loads)
                self.assertFalse(viewport.scroll_locked)

    async def test_live_events_do_not_move_an_old_page_and_return_in_next_snapshot(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test(size=(80, 20)) as pilot:
                app.connected = True
                app.session_id = "s"
                viewport = app.query_one(HistoryViewport)
                await viewport.replace(page(0, older=False, newer=True), "after")
                await pilot.pause()
                app.handle_event({"type": "provider.message_arrived", "messages": [
                    {"id": "live", "role": "assistant", "content": [{"type": "text", "text": "New answer"}]}]})
                await pilot.pause()
                self.assertEqual(viewport.scroll_y, 0)
                self.assertEqual(app.history_window.page["last"], "m49")
                self.assertEqual(len(app.query(UserMessage)), 50)
                async def send(request):
                    respond(app.history_window, request, page(50, older=True))
                app.client.send = send
                self.assertTrue(await app.load_history_page("after"))
                self.assertEqual(app.history_window.page["first"], "m50")
                self.assertEqual(len(app.query(UserMessage)), 50)

    async def test_snapshot_discards_only_preceding_live_events_and_keeps_following_ones(self):
        app = SosApp()
        with patch.object(app, "listen"):
            async with app.run_test() as pilot:
                app.connected = True
                app.session_id = "s"
                sent = []
                async def send(request):
                    sent.append(request)
                app.client.send = send
                task = asyncio.create_task(app.load_history_page("latest"))
                while not sent:
                    await asyncio.sleep(0)
                message = {"id": "already", "role": "assistant", "content": [{"type": "text", "text": "Already in snapshot"}]}
                app.display_live_messages([message])
                base = {"requestId": sent[0]["requestId"], "sessionId": "s"}
                app.handle_event({**base, "type": "session.attach_started", "purpose": "page", "session": {"id": "s"},
                                  "page": {}, "messageCount": 1})
                app.handle_event({**base, "type": "session.history_chunk", "messageIndex": 0, "partIndex": 0,
                                  "last": True, "data": json.dumps(message)})
                app.handle_event({**base, "type": "session.attached", "messageCount": 1})
                app.display_live_messages([{**message, "id": "following", "content": [{"type": "text", "text": "After snapshot"}]}])
                self.assertTrue(await task)
                await pilot.pause()
                from sos_tui.response_markdown import ResponseMarkdown
                self.assertEqual(len(app.query_one(HistoryViewport).query(ResponseMarkdown)), 2)
                self.assertFalse(app.history_live)
