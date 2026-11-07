"""Skill scripts are a display category, never a different execution tool."""

import unittest
from unittest.mock import patch

from sos_tui.app import SosApp
from sos_tui.config import ALGAE_MD
from sos_tui.tool_view import ToolView
from textual.color import Color
from textual.app import App


class SkillNameTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        await self.enterAsyncContext(App().run_test())

    def test_skill_name_is_visible_for_every_execution_status(self):
        view = ToolView('call', 'skill', {'name': 'check-api', 'input': 'src'})
        self.assertEqual(view.title, 'skill · check-api · Pending')
        for result, status in (({'isError': False}, 'Completed'), ({'isError': True}, 'Failed'),
                               ({'isError': False, 'metadata': {'cancelled': True}}, 'Not run'),
                               ({'isError': False, 'metadata': {'cancelled': True, 'started': True}}, 'Cancelled')):
            view.set_result(result)
            self.assertEqual(view.title, f'skill · check-api · {status}')

    def test_historical_skill_name_does_not_require_loading_arguments(self):
        from sos_tui.history_detail import HistoryToolView
        view = HistoryToolView('group', {'activityIndex': 0, 'callId': 'c', 'toolName': 'skill',
            'arguments': {'name': 'check-api'}, 'result': {'isError': False}})
        self.assertEqual(view.title, 'skill · check-api · Completed')
        self.assertFalse(view.details_loaded)


class SkillScriptTest(unittest.TestCase):
    def test_nested_path_and_jev_suffix(self):
        view = ToolView('call', 'bash', {}, skill_script='test-full', skill_script_path='nested/my_script.py')
        self.assertEqual(view.title, 'test-full script · nested/my_script.py · Pending')
        view.record_permission({'permissionId': 'permission', 'decision': 'ai_accepted'})
        self.assertEqual(view.title, 'test-full script · nested/my_script.py • AI • Pending')
        view.set_result({'isError': False, 'metadata': {'exit': 0, 'permissions': [
            {'id': 'permission', 'decision': 'ai_accepted'}]}})
        self.assertEqual(view.title, 'test-full script · nested/my_script.py • AI • Completed · Exit code 0')

    def test_older_daemon_without_script_path_does_not_invent_filename(self):
        view = ToolView('call', 'bash', {}, skill_script='demo')
        self.assertEqual(view.title, 'demo script · Pending')

    def test_titles_preserve_bash_result_semantics(self):
        arguments = {'command': 'python3 .sos/skills/demo/scripts/run.py'}
        view = ToolView('call', 'bash', arguments, skill_script='demo', skill_script_path='run.py')
        self.assertEqual(view.base_title, 'demo script · run.py · Pending')
        self.assertEqual(view.tool, 'bash')
        self.assertEqual(view.arguments, arguments)
        self.assertTrue(view.has_class('skill-call'))
        for metadata, status in (({'exit': 0}, 'Completed · Exit code 0'), ({'exit': 1}, 'Completed · Exit code 1'),
                                 ({'cancelled': True}, 'Not run'),
                                 ({'cancelled': True, 'started': True}, 'Cancelled')):
            view.set_result({'isError': False, 'metadata': metadata})
            self.assertEqual(view.base_title, f'demo script · run.py · {status}')


class SkillScriptLiveTest(unittest.IsolatedAsyncioTestCase):
    async def test_synthetic_skill_pair_is_one_user_triggered_activity(self):
        from sos_tui.history_detail import HistoryToolView
        from sos_tui.history_viewport import HistoryActivity, HistoryViewport
        from textual.widgets import Static
        app = SosApp()
        arguments = {'name': 'demo', 'arguments': {'subject': 'value'}}
        result = {'isError': False, 'output': '[bold]Exact skill content[/bold]', 'metadata': {}}
        with patch.object(app, 'listen'):
            async with app.run_test(size=(120, 40)) as pilot:
                app.display_message({'id': 'user', 'role': 'user', 'provider_text': 'Call the skill demo',
                                     'content': [{'type': 'text', 'text': '//demo value'}]})
                app.display_message({'id': 'call', 'role': 'assistant', 'user_skill': 'demo', 'content': [
                    {'type': 'tool_call', 'callId': 'call', 'toolName': 'skill', 'arguments': arguments}]})
                app.display_message({'id': 'result', 'role': 'tool', 'content': [
                    {'type': 'tool_result', 'callId': 'call', **result}]})
                await pilot.pause()
                self.assertEqual(len(app.query(ToolView)), 1)
                view = app.query_one(ToolView)
                self.assertEqual(view.title, 'skill · demo · Completed')
                self.assertEqual(view.result, {'type': 'tool_result', 'callId': 'call', **result})
                self.assertFalse(app.pending_tools)
                self.assertTrue(app.current_activity.toggle.has_skills)
                self.assertEqual(app.current_activity.toggle.step_count, 1)
                view.collapsed = False
                await pilot.pause()
                self.assertEqual(str(view.results.query_one(Static).render()), result['output'])
                record = {'id': 'user/activity/0', 'role': 'activity', 'kinds': ['skill'], 'count': 1,
                          'activities': [{'kind': 'tool', 'toolName': 'skill', 'callId': 'call',
                                          'user_skill': 'demo', 'activityIndex': 0,
                                          'result': {'isError': False, 'metadata': {}}}]}
                await app.query_one(HistoryViewport).replace({'session': {'id': 'saved'}, 'page': {}, 'messages': [record]})
                await app.query_one(HistoryActivity).populate(record)
                restored = app.query_one(HistoryToolView)
                self.assertEqual(restored.title, view.title)
                await restored.load_detail({**record['activities'][0], 'arguments': arguments, 'result': result})
                self.assertEqual(restored.arguments, arguments)
                self.assertEqual(restored.result, result)

    async def test_system_messages_do_not_create_skill_activities(self):
        app = SosApp()
        with patch.object(app, 'listen'):
            async with app.run_test(size=(120, 40)) as pilot:
                app.display_message({'id': 'system', 'role': 'system', 'user_skill': 'demo',
                                     'content': [{'type': 'text', 'text': 'System instructions'}]})
                await pilot.pause()
                self.assertFalse(app.query(ToolView))
                self.assertIsNone(app.current_activity)
                self.assertFalse(app.pending_tools)

    async def test_fresh_resume_uses_daemon_classification(self):
        from sos_tui.history_viewport import HistoryActivity, HistoryViewport
        app = SosApp()
        with patch.object(app, 'listen'):
            async with app.run_test(size=(120, 40)) as pilot:
                viewport = app.query_one(HistoryViewport)
                record = {'id': 'group', 'role': 'activity', 'kinds': ['skill'], 'count': 1,
                          'activities': [{'kind': 'tool', 'toolName': 'bash', 'callId': 'call',
                                          'activityIndex': 0,
                                          'skill_script': 'demo', 'skill_script_path': 'run.py',
                                          'result': {'isError': False, 'metadata': {'exit': 0, 'permissions': [
                                              {'id': 'permission', 'decision': 'ai_accepted'}]}}}]}
                await viewport.replace({'session': {'id': 'saved'}, 'page': {}, 'messages': [record]})
                drawer = app.query_one(HistoryActivity)
                self.assertTrue(drawer.toggle.has_skills)
                self.assertFalse(drawer.toggle.has_tools)
                await drawer.populate(record)
                await pilot.pause()
                view = app.query_one(ToolView)
                self.assertEqual(view.base_title, 'demo script · run.py · Completed · Exit code 0')
                self.assertEqual(view.title, 'demo script · run.py • AI • Completed · Exit code 0')
                self.assertEqual(view.arguments, {})
                self.assertEqual(view.tool, 'bash')
                self.assertEqual(view.query_one('CollapsibleTitle').styles.color, Color.parse(ALGAE_MD))
                self.assertTrue(drawer.toggle.has_skills)
                self.assertFalse(drawer.toggle.has_tools)

    async def test_live_script_has_algae_title_and_square(self):
        app = SosApp()
        with patch.object(app, 'listen'):
            async with app.run_test(size=(120, 40)) as pilot:
                app.display_message({'id': 'call-message', 'role': 'assistant', 'content': [
                    {'type': 'tool_call', 'callId': 'call', 'toolName': 'bash', 'skill_script': 'demo', 'skill_script_path': 'run.py',
                     'arguments': {'command': 'python3 .sos/skills/demo/scripts/run.py'}}]})
                await pilot.pause()
                view = app.query_one(ToolView)
                self.assertEqual(view.base_title, 'demo script · run.py · Pending')
                self.assertEqual(view.query_one('CollapsibleTitle').styles.color, Color.parse(ALGAE_MD))
                self.assertTrue(app.current_activity.toggle.has_skills)
                self.assertFalse(app.current_activity.toggle.has_tools)

                from sos_tui.history_viewport import HistoryActivity, HistoryViewport
                viewport = app.query_one(HistoryViewport)
                viewport.window.session_id = 'session'
                record = {'id': 'group', 'role': 'activity', 'kinds': ['skill'], 'count': 1,
                          'activities': [{'kind': 'tool', 'toolName': 'bash', 'callId': 'call',
                                          'skill_script': 'demo', 'skill_script_path': 'run.py', 'activityIndex': 0, 'result': {'isError': False, 'metadata': {'exit': 0}}}]}
                await viewport.replace({'session': {'id': 'session'}, 'page': {}, 'messages': [record]})
                drawer = app.query_one(HistoryActivity)
                await drawer.populate(record)
                await pilot.pause()
                restored = app.query_one(ToolView)
                self.assertEqual(restored.base_title, 'demo script · run.py · Completed · Exit code 0')
                self.assertEqual(restored.arguments, {})  # No eager detail fetch needed.
                self.assertEqual(restored.query_one('CollapsibleTitle').styles.color, Color.parse(ALGAE_MD))
                await viewport.replace({'session': {'id': 'other'}, 'page': {}, 'messages': []})
