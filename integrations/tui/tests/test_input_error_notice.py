"""Rejected-input feedback uses SLOP without taking focus or changing drafts."""

import unittest
from unittest.mock import patch

from sos_tui.app import SosApp
from sos_tui.input_error_notice import InputErrorNotice
from sos_tui.message_input import MessageInput
from sos_tui.config import SLOP_COLOR, TITLE_MD


class InputErrorNoticeTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = SosApp()
        self.enterContext(patch.object(self.app, "listen"))
        self.pilot = await self.enterAsyncContext(self.app.run_test(size=(100, 30)))
        self.entry = self.app.query_one(MessageInput)
        self.entry.disabled = False
        self.entry.focus()

    async def test_full_width_half_band_and_draft_preserved(self):
        self.entry.paste_text("First line\nSecond line\nThird line")
        selection, blocks = self.entry.selection, self.entry.paste_blocks
        before = self.app.query_one("#chat").region
        await self.app.show_input_error("This model does not support image input. [literal]")
        await self.pilot.pause()
        notice = self.app.screen.query_one(InputErrorNotice)
        self.assertEqual(notice.region.width, 98)
        self.assertEqual(notice.region.x, 1)
        self.assertEqual(notice.region.bottom, self.app.query_one("#agent-info").region.y)
        self.assertEqual(notice.styles.border_left[0], "outer")
        self.assertEqual(notice.styles.border_left[1].hex, SLOP_COLOR)
        self.assertTrue(notice.render_lines(notice.region.size.region)[0].text.startswith("▌"))
        self.assertIn("[literal]", str(notice.render()))
        self.assertIs(self.app.focused, self.entry)
        self.assertEqual(self.entry.selection, selection)
        self.assertEqual(self.entry.paste_blocks, blocks)
        self.assertEqual(self.app.query_one("#chat").region, before)
        await self.pilot.resize_terminal(45, 24)
        await self.pilot.pause()
        self.assertEqual(notice.region.width, 43)
        self.assertEqual(notice.region.bottom, self.app.query_one("#agent-info").region.y)

    async def test_startup_warning_keeps_slash_navigation_responsive_and_settles(self):
        errors = [f"/workspace/.sos/skills/test-{index}/SKILL.md: Required argument "
                  '\"audience\" has no {audience} placeholder.' for index in range(3)]
        self.app.update_agent_info({"skill_errors": errors})
        await self.pilot.pause()
        notice = self.app.screen.query_one(InputErrorNotice)
        notice.hide_timer.stop()
        await self.pilot.press("/", "down", "down", "up")
        await self.pilot.pause()
        self.assertEqual(self.entry.text, "/btw")
        self.assertIs(self.app.focused, self.entry)
        self.assertLessEqual(notice.region.bottom, self.app.query_one("SkillDetails").region.y)
        with patch.object(notice, "reposition", wraps=notice.reposition) as reposition:
            await self.pilot.pause(0.1)
            reposition.assert_not_called()
        notice.hide()
        with patch.object(notice.screen, "query") as query:
            notice.reposition()
            query.assert_not_called()

    async def test_position_requests_are_coalesced_and_hidden_notice_does_no_work(self):
        await self.app.show_input_error("Startup warning")
        await self.pilot.pause()
        notice = self.app.screen.query_one(InputErrorNotice)
        with patch.object(notice, "call_after_refresh", return_value=True) as schedule:
            for _ in range(10):
                notice.schedule_reposition()
            schedule.assert_called_once()
            notice.finish_reposition()
            notice.hide()
            schedule.reset_mock()
            notice.schedule_reposition()
            schedule.assert_not_called()

    async def test_replaces_error_and_restarts_timer_without_stacking(self):
        await self.app.show_input_error("First")
        notice = self.app.screen.query_one(InputErrorNotice)
        first_timer = notice.hide_timer
        await self.app.show_input_error("Second")
        self.assertEqual(len(self.app.screen.query(InputErrorNotice)), 1)
        self.assertIsNot(notice.hide_timer, first_timer)
        self.assertEqual(str(notice.render()), "Second")
        notice.hide()
        self.assertFalse(notice.display)

    async def test_skill_diagnostics_use_warning_band_once_per_change(self):
        errors = ['Required argument "name" has no {name} placeholder.',
                  'Placeholder {unknown} has no declared argument.']
        self.app.update_agent_info({"skill_errors": errors})
        await self.pilot.pause()
        notice = self.app.screen.query_one(InputErrorNotice)
        self.assertEqual(str(notice.render()), "Skills:\n" + "\n".join(errors))
        self.assertEqual(notice.styles.border_left[1].hex, TITLE_MD.upper())
        self.assertIs(self.app.focused, self.entry)
        timer = notice.hide_timer
        self.app.update_agent_info({"skill_errors": errors, "model": "test"})
        await self.pilot.pause()
        self.assertIs(notice.hide_timer, timer)
        self.app.update_agent_info({"skill_errors": []})
        self.app.update_agent_info({"skill_errors": errors})
        await self.pilot.pause()
        self.assertIsNot(notice.hide_timer, timer)
