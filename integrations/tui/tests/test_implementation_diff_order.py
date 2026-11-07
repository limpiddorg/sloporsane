"""Chronological display does not change the canonical hunk indexes used by review."""
import unittest

from sos_tui.implementation_code import change_zones


def ordered_diffs():
    source = "".join(f"line {index}\n" for index in range(90))
    return [
        {"path": "a.py", "patch": "@@ -41 +41 @@\n-old\n+second\n", "after": source,
         "hunk_order": [2]},
        {"path": "z.py", "patch": "@@ -6 +6 @@\n-old\n+third\n@@ -71 +71 @@\n-old\n+first\n",
         "after": source, "hunk_order": [3, 1]},
    ]


class ImplementationDiffOrderTest(unittest.TestCase):
    def test_zones_follow_construction_order_across_files_and_keep_review_hunk_indexes(self):
        zones = change_zones(ordered_diffs())
        self.assertEqual([(zone.path, zone.hunk, zone.order) for zone in zones],
                         [("z.py", 1, 1), ("a.py", 0, 2), ("z.py", 0, 3)])
        self.assertGreater(zones[0].center, zones[2].center)
        self.assertEqual([zone.label for zone in zones], ["z.py:71", "a.py:41", "z.py:6"])

    def test_legacy_diffs_keep_their_existing_order(self):
        diffs = ordered_diffs()
        for diff in diffs:
            del diff["hunk_order"]
        self.assertEqual([(zone.path, zone.hunk) for zone in change_zones(diffs)],
                         [("a.py", 0), ("z.py", 0), ("z.py", 1)])

    def test_fallback_patch_uses_oldest_order_when_historical_contents_are_unavailable(self):
        diffs = ordered_diffs()
        for diff in diffs:
            del diff["after"]
        zones = change_zones(diffs)
        self.assertEqual([(zone.path, zone.order) for zone in zones], [("z.py", 1), ("a.py", 2)])

    def test_binary_and_metadata_zones_can_be_interleaved_with_textual_zones(self):
        diffs = [{"path": "image.png", "patch": "Binary contents changed", "hunk_order": [4]}, *ordered_diffs()]
        self.assertEqual([zone.path for zone in change_zones(diffs)], ["z.py", "a.py", "z.py", "image.png"])
