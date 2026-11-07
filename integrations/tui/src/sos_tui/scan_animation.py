"""Reusable, time-based scan renderer for a single-row labelled control.

No timers or input handling: the owner supplies elapsed seconds, dimensions and
CSS colours. The encoding sweep alternates direction after each decode/rest.
"""

from dataclasses import dataclass
import random

from rich.style import Style
from rich.text import Text
from textual.color import Color

WHITE = Color(255, 255, 255)
CIPHER = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/!?*"


def clamp(value: float) -> float:
    return max(0.0, min(1.0, value))


def ease(value: float) -> float:
    return value * value * (3 - 2 * value)


def validation_sweep(label: str, phase: float, accent: Color) -> Text:
    """A white round-trip reflection, shared by the label and full bar."""
    travel = 1 - abs(2 * phase - 1)
    head = 1.1 - travel * 1.2
    text = Text(no_wrap=True)
    for column, character in enumerate(label):
        distance = abs((column + 0.5) / len(label) - head)
        wave = ease(clamp(1 - distance / 0.2))
        text.append(character, style=Style(color=accent.blend(WHITE, wave).rich_color, bold=True))
    return text


@dataclass(frozen=True)
class ScanAnimation:
    """Render the supplied scan effect without owning a widget or its clock."""

    sweep: float = 0.9
    beat: float = 0.3
    step: float = 0.075
    settle: float = 0.12
    rest: float = 0.3

    def render(self, elapsed: float, width: int, label: str, accent: Color, ink: Color) -> Text:
        width = max(0, width)
        label = label[:width]
        count = len(label)
        start = (width - count) // 2
        decode_duration = self.step * max(count - 1, 0) + self.settle
        half_cycle = self.sweep + self.beat + decode_duration + self.rest
        half_index, moment = divmod(max(0.0, elapsed), half_cycle)
        forward = int(half_index) % 2 == 0
        tick = int(max(0.0, elapsed) * 20)
        tail = 5.0
        head = None
        if moment < self.sweep:
            distance = ease(moment / self.sweep) * (width + tail)
            head = distance if forward else width - distance

        def state(index: int) -> str:
            if head is not None:
                position = start + index + 0.5
                return "encoded" if (position < head if forward else position > head) else "plain"
            if moment < self.sweep + self.beat:
                return "encoded"
            if moment < self.sweep + self.beat + decode_duration:
                rank = index if forward else count - 1 - index
                progress = moment - self.sweep - self.beat - rank * self.step
                return "encoded" if progress < 0 else "churn" if progress < self.settle else "plain"
            return "plain"

        rng = random.Random(int(half_index) * 1_000_003 + 17)
        sequence = []
        for index in range(count):
            avoid = set(label[max(index - 1, 0):index + 2])
            if sequence:
                avoid.add(sequence[-1])
            sequence.append(rng.choice([glyph for glyph in CIPHER if glyph not in avoid]))

        peak = accent.blend(WHITE, 0.62)
        muted = ink.blend(accent, 0.22)
        text = Text(no_wrap=True)
        previous = ""
        for column in range(width):
            # A bright leading edge, fading only behind it. Blend with the
            # accent rather than the terminal background to keep the chip solid.
            behind = (head - column - 0.5 if forward else column + 0.5 - head) if head is not None else -1
            wave = ease(clamp(1 - behind / tail)) if behind >= 0 else 0.0
            background = accent.blend(peak, wave)
            index = column - start
            foreground = ink
            character = label[index] if 0 <= index < count else " "
            if 0 <= index < count:
                status = state(index)
                if status == "churn" or wave > 0.25:
                    right = sequence[index + 1] if index + 1 < count else ""
                    following = label[index + 1] if index + 1 < count else ""
                    pool = [glyph for glyph in CIPHER if glyph not in (previous, right, character, following)]
                    character = random.Random(tick * 7919 + column).choice(pool)
                elif status == "encoded":
                    character, foreground = sequence[index], muted
            text.append(character, style=Style(color=foreground.rich_color, bgcolor=background.rich_color, bold=True))
            previous = character
        return text
