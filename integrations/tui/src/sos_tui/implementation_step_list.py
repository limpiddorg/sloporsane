"""A step list whose complete height belongs to its external scrolling menu."""
from textual.widgets import OptionList


class ImplementationStepList(OptionList):
    def _resolve_extrema(self, container, viewport, width_fraction, height_fraction):
        # OptionList's 100% max-height clips rows; the outer menu must own the full extent.
        extrema = super()._resolve_extrema(container, viewport, width_fraction, height_fraction)
        return extrema._replace(max_height=None)
