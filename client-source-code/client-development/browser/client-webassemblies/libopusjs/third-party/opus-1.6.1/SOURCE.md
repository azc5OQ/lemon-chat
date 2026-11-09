# Source provenance

Official Opus 1.6.1 release:
https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz

SHA-256: `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`

The release checksum was verified before extraction. `COPYING` and `AUTHORS`
are retained. The C files listed in `../../sources.json` come unchanged from
OPUS_SOURCES, OPUS_SOURCES_FLOAT, CELT_SOURCES, SILK_SOURCES and SILK_SOURCES_FLOAT
in the upstream makefile lists, excluding unused multistream/projection/mapping
APIs. Required generic headers are retained. Examples, tests, generated build
systems, fixed-point/SIMD/assembly code and DNN model trees are omitted.
Build flags are in `../../build.cjs`; the upstream source files are unmodified.

This replaces the 1.5.2 source tar previously stored here. Its original archive
SHA-256 was `55be60327e2a2613bc0fa605a2d62a241ac9ef694690e8f09572e550142757b6`.
The shipped old binary and loader remain in `../../../prebuilt-backup`.
