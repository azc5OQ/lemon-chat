# Desktop icons

These assets belong to the desktop client. The toolbar's server, channel, profile,
audio and settings shapes are adapted from the browser's `theme-bluebell.style`.
Bluebell and Hybrid v2 use the same rounded geometry. The desktop copies use a
single neutral stroke color to match the graphite toolbar. The brand, away and
voice-state drawings follow the same stroke treatment.

Edit the SVG sources to change a shape or its color. The matching PNGs include
transparent padding and are rendered at four times their display size. The native
theme loads these PNGs from disk. Normal desktop builds copy them without needing
Node or an SVG converter.

To regenerate them, install Playwright for this optional asset-editing step and run
`node tools/render-icons.cjs` from the desktop directory. It uses an installed Edge
on Windows, or Playwright's Chromium. Set `BROWSER_EXECUTABLE` to use another local
Chromium executable. An optional argument specifies the Playwright module path.

Classic selects the channel and voice-state images through CSS. Themes without
these image rules retain the native voice badges. The toolbar no longer displays
an application logo. The older brand assets remain available as source artwork.

Midnight uses the `-midnight` variants for its channel and voice icons.
Their lighter strokes remain visible on charcoal surfaces; the shapes and PNG
dimensions match the original icons. They use the same rendering command above.
