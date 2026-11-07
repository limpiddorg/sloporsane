"""Deterministic scan frames, independent of Textual timers and button actions."""

import unittest
from textual.color import Color
from sos_tui.scan_animation import ScanAnimation, validation_sweep


class ScanAnimationTest(unittest.TestCase):
    def setUp(self):
        self.scan = ScanAnimation()
        self.accent = Color.parse("#e5405e")
        self.ink = Color.parse("#1b1b1b")

    def frame(self, time, width=12, label="Cancel"):
        return self.scan.render(time, width, label, self.accent, self.ink)

    def half_cycle(self, label="Cancel"):
        return (self.scan.sweep + self.scan.beat + self.scan.step * (len(label) - 1)
                + self.scan.settle + self.scan.rest)

    def test_initial_hold_and_rest_have_the_supplied_timing(self):
        self.assertEqual(self.frame(0).plain, "   Cancel   ")
        encoded = self.frame(1.0)
        self.assertNotEqual(encoded.plain.strip(), "Cancel")
        self.assertEqual(encoded, self.frame(1.1))  # Stable during encoded beat.
        self.assertEqual(self.frame(2.0).plain, "   Cancel   ")
        letters = encoded.plain.strip()
        self.assertTrue(all(a != b for a, b in zip(letters, "Cancel")))
        self.assertTrue(all(a != b for a, b in zip(letters, letters[1:])))

    def test_sweep_and_decoding_reverse_on_next_half_cycle(self):
        half = self.half_cycle()
        forward = self.frame(1.36).plain.strip()
        backward = self.frame(half + 1.36).plain.strip()
        self.assertEqual(forward[0], "C")
        self.assertNotEqual(forward[-1], "l")
        self.assertEqual(backward[-1], "l")
        self.assertNotEqual(backward[0], "C")
        self.assertNotEqual(self.frame(1.0).plain, self.frame(half + 1.0).plain)
        left = [span.style.bgcolor for span in self.frame(0.3).spans]
        right = [span.style.bgcolor for span in self.frame(half + 0.3).spans]
        self.assertEqual(left, right[::-1])

    def test_frames_do_not_depend_on_previous_calls_or_frame_rate(self):
        expected = self.frame(3.0)
        for time in (0.0, 0.5, 0.9, 9.0, 100.0):
            self.frame(time)
        self.assertEqual(self.frame(3.0), expected)

    def test_wave_has_a_bright_front_and_fades_towards_its_tail(self):
        frame = self.frame(0.45, width=15)
        colours = [span.style.bgcolor.get_truecolor() for span in frame.spans]
        for column in range(5, 9):
            self.assertLess(colours[column].green, colours[column + 1].green)
        self.assertEqual(colours[4], self.accent.rich_color.get_truecolor())
        self.assertEqual(colours[10], self.accent.rich_color.get_truecolor())
        half = self.half_cycle()
        backwards = [span.style.bgcolor.get_truecolor() for span in self.frame(half + 0.45, width=15).spans]
        for forward, backward in zip(colours, backwards[::-1]):
            for a, b in zip(forward, backward):
                self.assertLessEqual(abs(a - b), 1)  # Floating-point blend rounding.

    def test_all_labels_stay_centred_even_while_scrambled(self):
        for label in ("Slop", "Go Sane", "Cancel"):
            for width in (7, 12, 15, 23):
                left = (width - len(label)) // 2
                for time in (0, 0.3, 1.0, 1.4, 2.0, 3.0):
                    with self.subTest(label=label, width=width, time=time):
                        text = self.frame(time, width, label).plain
                        self.assertEqual(len(text) - len(text.lstrip()), left)
                        self.assertEqual(len(text) - len(text.rstrip()), width - left - len(label))

    def test_clipping_and_custom_palette(self):
        for width in (0, 1, 4, 12, 30):
            for time in (0, 0.45, 1.3, 2, 100):
                self.assertEqual(self.frame(time, width).cell_len, width)
        green = Color.parse("#3fb68b")
        frame = self.scan.render(2.0, 12, "Working", green, self.ink)
        self.assertEqual(frame.plain.strip(), "Working")
        self.assertTrue(all(span.style.bgcolor == green.rich_color for span in frame.spans))
        self.assertTrue(all(span.style.bold for span in frame.spans))

    def test_validation_reflection_is_white_and_round_trip_without_changing_text(self):
        label = "Validating".center(40)
        bar = "[" + "#" * 38 + "]"
        peaks = []
        for phase in (.15, .25, .35, .65, .75, .85):
            title = validation_sweep(label, phase, self.accent)
            progress = validation_sweep(bar, phase, self.accent)
            self.assertEqual(title.plain, label)
            self.assertEqual(progress.plain, bar)
            self.assertEqual(title.spans, progress.spans)
            self.assertTrue(all(span.style.bgcolor is None for span in title.spans))
            colours = [span.style.color.get_truecolor().green for span in title.spans]
            self.assertGreater(max(colours), 245)
            peaks.append(max(range(len(colours)), key=colours.__getitem__))
        self.assertGreater(peaks[0], peaks[1])
        self.assertGreater(peaks[1], peaks[2])
        self.assertLess(peaks[3], peaks[4])
        self.assertLess(peaks[4], peaks[5])
        self.assertEqual(peaks[:3], peaks[3:][::-1])
        self.assertEqual(validation_sweep("", .5, self.accent).plain, "")
