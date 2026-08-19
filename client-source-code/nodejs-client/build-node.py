#!/usr/bin/env python3
"""
Glues the client sources into bundle.js for nodejs-mobile - same @@INCLUDE mechanism as
client-development/build.py, different include list.

In: dom-shim, crypto vendors, utils, encoding, audio-opus-glue (for custom_typeof), messages, main.
Out: ui.js and sounds.js (their `var UI` / `var g_sound_effects` would shadow the shim - see
dom-shim.js), plus all webaudio/wasm/worker files - voice stays in the WebView.

Paths starting with `node/` resolve next to this script, everything else under client-development/src.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.normpath(os.path.join(HERE, "..", "client-development"))
SRC = os.path.join(DEV, "src")
TEMPLATE = os.path.join(HERE, "node-template.js")
OUT = os.path.join(HERE, "bundle.js")

INCLUDE_RE = re.compile(r"^\s*/\*\s*@@INCLUDE:\s*(.+?)\s*@@\s*\*/\s*$")


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def resolve(rel):
    if rel.startswith("node/"):
        return os.path.join(HERE, rel[len("node/"):])
    return os.path.join(SRC, rel)


def main():
    print("build-node.py: gluing -> %s" % os.path.relpath(OUT, HERE))
    template = read_text(TEMPLATE)

    out = []
    n_inc = 0
    for line in template.split("\n"):
        m = INCLUDE_RE.match(line)
        if m:
            rel = m.group(1).strip()
            path = resolve(rel)
            if not os.path.exists(path):
                print("  MISSING  %s  (looked in %s)" % (rel, path))
                return 1
            text = read_text(path)
            n_inc += 1
            print("  include %2d  %-44s %9d bytes" %
                  (n_inc, rel, len(text.encode("utf-8"))))
            out.append(text)
        else:
            out.append(line)

    assembled = "\n".join(out)

    if os.path.exists(OUT):
        os.remove(OUT)
    with open(OUT, "w", encoding="utf-8", newline="") as f:
        f.write(assembled)

    print("-" * 60)
    print("done: %d includes -> %s (%d bytes)" %
          (n_inc, os.path.relpath(OUT, HERE), len(assembled.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
