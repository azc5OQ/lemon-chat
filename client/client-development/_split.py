#!/usr/bin/env python3
"""
One-time bootstrap: splits the monolithic client.html into per-library source
files under src/, plus a template.html that build.py reassembles.

Reads ../client.html and writes into ./src/. Safe to re-run. After editing the
split sources you use build.py, not this script.

NOTE: build.py now writes its output back to ../client.html, so that file is no
longer a pristine original -- re-running this would re-split the latest build.
The untouched original is preserved in git history.

Byte-exactness: every source file holds the EXACT text of its original line
range (no trailing newline). template.html keeps all the HTML shell (head,
<style>/<script> tags, inter-block whitespace, body) verbatim, with one
@@INCLUDE ...@@ token per source segment. The two WebAssembly base64 blobs are
decoded to real .wasm files and replaced inline with @@WASM ...@@ tokens.
"""
import base64
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ORIGINAL = os.path.join(HERE, "..", "client.html")
SRC = os.path.join(HERE, "src")

# (start_line, end_line, relative_path)  -- 1-based, inclusive. INNER content of
# each <style>/<script> block; the big main script is cut at library seams.
SEGMENTS = [
    # ---- styles ----
    (19,    93,    "styles/fonts.style"),
    (98,    1869,  "styles/theme-default.style"),
    (1875,  4448,  "styles/theme-light.style"),
    (4454,  6249,  "styles/theme-default-mobile.style"),
    # ---- script 1: BigInteger.js ----
    (6254,  7707,  "scripts/vendor/biginteger.js"),
    # ---- script 2: ws-audio-api derived audio setup ----
    (7711,  7866,  "scripts/app/audio-setup.js"),
    # ---- script 3: app + vendored libs, interleaved ----
    (7871,  7913,  "scripts/app/utils.js"),
    (7914,  8911,  "scripts/vendor/aes-js.js"),
    (8912,  9195,  "scripts/app/encoding.js"),
    (9196,  11345, "scripts/vendor/jsbn.js"),
    (11346, 11523, "scripts/vendor/sha256.js"),
    (11524, 11722, "scripts/vendor/sha1.js"),
    (11723, 11989, "scripts/vendor/md5.js"),
    (11990, 12285, "scripts/vendor/rsa.js"),
    (12286, 12451, "scripts/vendor/rsa-sign.js"),
    (12452, 13021, "scripts/vendor/cryptico.js"),
    (13022, 13640, "scripts/vendor/js-sha256.js"),
    (13641, 15506, "scripts/vendor/libopus.emscripten.js"),
    (15507, 16537, "scripts/app/audio-opus-glue.js"),
    (16538, 16597, "scripts/vendor/minimp3.js"),
    (16598, 16640, "scripts/app/minimp3-worker.js"),
    (16641, 26125, "scripts/app/main.js"),
    # ---- script 4: dev-console globals + Android bridge ----
    (26129, 26172, "scripts/app/dev-android.js"),
]

# WebAssembly blobs embedded inline as `name = <quote>base64<quote>`.
WASM = [
    ("libopus_webassembly_base64 = '",      "'",  "wasm/libopus.wasm"),
    ('mp3_decoder_webassembly_base64 = "',  '"',  "wasm/mp3_decoder.wasm"),
]


def extract_wasm(content):
    """Replace any embedded wasm base64 in `content` with a @@WASM ...@@ token,
    writing the decoded bytes to a real .wasm file. Returns updated content."""
    for marker, closing, wasm_rel in WASM:
        pos = content.find(marker)
        if pos == -1:
            continue
        start = pos + len(marker)
        end = content.index(closing, start)
        b64 = content[start:end]
        raw = base64.b64decode(b64)
        # the build re-encodes; make sure it round-trips byte-for-byte
        assert base64.b64encode(raw).decode("ascii") == b64, \
            "base64 for %s is not canonical; cannot round-trip" % wasm_rel
        out = os.path.join(SRC, wasm_rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(raw)
        content = content[:start] + "@@WASM:%s@@" % wasm_rel + content[end:]
        print("  wasm  %-26s %8d bytes" % (wasm_rel, len(raw)))
    return content


def main():
    with open(ORIGINAL, "r", encoding="utf-8", newline="") as f:
        lines = f.read().split("\n")
    n = len(lines)

    segs = sorted(SEGMENTS)
    prev = 0
    for s, e, p in segs:
        assert 1 <= s <= e <= n, "bad range %d-%d for %s" % (s, e, p)
        assert s > prev, "overlap/out-of-order at %s (%d <= %d)" % (p, s, prev)
        prev = e
    starts = {s: (e, p) for s, e, p in segs}

    # write source files
    for s, e, p in segs:
        content = "\n".join(lines[s - 1:e])
        content = extract_wasm(content)
        out = os.path.join(SRC, p)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="") as f:
            f.write(content)
        print("  file  %-40s lines %d-%d" % (p, s, e))

    # build template: verbatim shell, one token per segment
    pieces = []
    i = 1
    while i <= n:
        if i in starts:
            e, p = starts[i]
            pieces.append("/* @@INCLUDE: %s @@ */" % p)
            i = e + 1
        else:
            pieces.append(lines[i - 1])
            i += 1
    with open(os.path.join(SRC, "template.html"), "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(pieces))
    print("  file  template.html")
    print("done: %d source files + template" % len(segs))


if __name__ == "__main__":
    main()
