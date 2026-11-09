# Desktop fonts

These are desktop-owned copies of the 18 families embedded in the browser's
`main/styles/fonts.style`. The desktop build copies this folder beside the
executable; it does not read browser sources or download fonts.

The browser's WOFF2 containers were unpacked into TrueType files with FontTools
4.63.0. Inter, Manrope and Plus Jakarta Sans were instantiated at regular weight
400 because the desktop renderer does not select variable-font axes. The other
faces retain their original outlines and metrics. FontTools is not a desktop
build requirement.

`Inter Medium.ttf` (500) and `Manrope SemiBold.ttf` (600) are additional static
weights from those same variable fonts. The desktop theme uses them for controls
and headings instead of synthesizing bold text. Their accompanying license
notices come from the same families.

Each `*-LICENSE.txt` preserves the attribution and license information from that
font's name table. Yellowtail uses Apache 2.0; the other 17 families use SIL OFL
1.1. The full license texts are in `Apache-2.0.txt` and `OFL-1.1.txt`.
Space Grotesk and its license are copied separately from Meek UI as UI fallbacks.

To add, remove or replace fonts in a running client, see
[Chat fonts and colors](../README.md#chat-fonts-and-colors).
