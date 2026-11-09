# FreeType 2.15.0

Official release: https://download.savannah.gnu.org/releases/freetype/freetype-2.15.0.tar.xz

SHA-256: `93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5`

License: FreeType License (`docs/FTL.TXT`). The source is unchanged except enabling
`TT_CONFIG_OPTION_GPOS_KERNING` in `include/freetype/config/ftoption.h` to preserve
pair kerning previously handled by stb_truetype. Optional compressed/bitmap font
and external shaping dependencies are disabled in the desktop CMake build.
