# Opus source

This is the official Opus 1.6.1 release, kept separately from the browser and
desktop client's dependencies. Upstream source files are unchanged.

- Source: https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
- Released: January 14, 2026
- SHA-256: `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`
- License: [COPYING](COPYING)

The server build scripts compile the static library with CPU optimizations
enabled. On x86, Opus checks the running CPU before selecting SSE4.1 or AVX2.
Optional neural features remain disabled; building the server does not download
neural model files.
