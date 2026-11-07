"""Inspect the rendered picker rows rather than their Rich object repr."""

from rich.console import Console


def row_text(row, width=80):
    return "".join(segment.text for segment in Console(width=width).render(row)).rstrip("\n")
