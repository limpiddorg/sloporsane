# Stable scrolling in Textual

The scroll extent must describe the document, not the currently mounted window.
Wheel and keyboard movement still use terminal lines. Loading or evicting a window
must preserve the visible reading anchor.

## Shared components

`src/sos_tui/stable_scroll.py` provides `StableScroll`, a `VerticalScroll` subclass.
Fully loaded documents use the ordinary native scrollbar. Before replacing Markdown,
`ResponseMarkdown.update()` retains a reading anchor; the completed layout restores
it. A `scroll_key` on a widget lets that anchor survive reconstruction. Keys must be
stable and unique within the viewport. Explicit view changes should reset to their
intended position rather than retaining an unrelated document's anchor.

Windowed adapters override `scroll_extent()` to return `ScrollExtent(total, position,
visible)` in their own logical units, and `seek(position)` to load/position the target.
The scrollbar then remains independent of the loaded widgets' heights. Keep all
Textual scrollbar integration in `StableScroll`, including the framework-private
layout/update hooks. Tests must cover both scrollbar display and drag destinations
when changing the Textual dependency.

`src/sos_tui/scroll_window.py` provides `ScrollWindow` for documents whose row heights
can be measured without mounting them. It maps offsets to rows, selects a bounded
window with overscan, computes top/bottom spacer heights, and converts reading
anchors after a width change. The native scrollbar can then use the exact full
physical extent; no logical scrollbar override is necessary.

## Current adapters

- Discussion, its read-only preview and the implementation-planning chat use daemon
  display-record offsets. Grouped Activities count once. The thumb uses a fixed
  three-record span rather than pretending unloaded message heights are known.
  Existing overlapping-page navigation retains its widget anchor; distant seeks
  use `session.history` with `direction: "at"`.
- Historical code measures wrapped rows once per document/width, renders only a
  bounded window, and pads it to the full document extent. Jumping to a distant
  line does not require successively rendering intermediate windows.
- Review and plan documents already receive their complete records/text. They use
  the native full extent and shared reading anchors, not artificial message counts.
  Review still follows the response only when the reader is at the bottom.

For a future paginated view, expose its global extent and implement direct seeking
in the data source. Do not fake a global range by growing the loaded window. For an
already available document, prefer measured row offsets and bounded rendering.
