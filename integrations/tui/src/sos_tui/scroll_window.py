"""A measured document extent independent of its bounded rendering window."""
from bisect import bisect_right
from itertools import accumulate


class ScrollWindow:
    def __init__(self, heights):
        self.offsets = [0, *accumulate(max(1, height) for height in heights)]

    @property
    def total(self):
        return self.offsets[-1]

    def item_at(self, offset):
        return max(0, min(len(self.offsets) - 2, bisect_right(self.offsets, offset) - 1))

    def around(self, offset, height, margin):
        return (max(0, self.item_at(offset) - margin),
                min(len(self.offsets) - 1, self.item_at(offset + height) + margin + 1))

    def padding(self, start, end):
        return self.offsets[start], self.total - self.offsets[end]

    def anchor(self, offset):
        index = self.item_at(offset)
        return index, offset - self.offsets[index]

    def position(self, anchor):
        index, within = anchor
        index = min(index, len(self.offsets) - 2)
        return self.offsets[index] + min(within, self.offsets[index + 1] - self.offsets[index] - 1)
