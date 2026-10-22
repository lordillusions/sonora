# ADR 0004 — Own retained widget tree instead of juce::Component for the main UI

**Status:** accepted (2026-10-13)

`ui::Widget` is a ~300-line retained tree drawn with Skia. Reasons: one GPU surface for the whole
window (performance, like Live), full control of pixel look, trivial off-screen rendering for tests
(`ui_snapshot`). Input comes from `gfx::SkiaCanvasComponent`. Accessibility is a known gap (future work).

ADR 0008 adds a bounded native Assistant dock beside the canvas for OS text composition and accessibility. It does not
replace the DAW widget tree or embed native controls over the Metal surface.
