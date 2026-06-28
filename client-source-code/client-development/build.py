#!/usr/bin/env python3
"""
Glues the split sources under src/ back into a single client.html.

  src/template.html        HTML shell with @@INCLUDE path@@ tokens
  src/**/*.js, *.style     pure source, one per token (no HTML tags)
  src/wasm/*.wasm          binaries, re-encoded to base64 at @@WASM path@@ tokens

Output: ../client.html at the repo root -- i.e. the shipped client itself,
removed and regenerated on every run. There is no separate client-build.html;
this writes straight to the file you ship. Edit the sources or template.html,
then re-run this. The output name/location is intentionally easy to change --
see OUT below.
"""
import base64
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
TEMPLATE = os.path.join(SRC, "template.html")
# The build output IS the shipped client: overwrite ../client.html at the root.
OUT = os.path.normpath(os.path.join(HERE, "..", "client.html"))
# Older builds wrote here; delete any leftover so only the single OUT remains.
LEGACY_OUT = os.path.join(SRC, "client-build.html")

# marker is a /* ... */ block comment so it is valid CSS *and* JS (no VS Code
# red squiggles inside <style>/<script>); the whole line is swapped for the file.
INCLUDE_RE = re.compile(r"^\s*/\*\s*@@INCLUDE:\s*(.+?)\s*@@\s*\*/\s*$")
WASM_RE = re.compile(r"@@WASM:([^@]+)@@")


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def main():
    print("build.py: gluing src/ -> %s" % os.path.relpath(OUT, HERE))
    print("reading template : %s" % os.path.relpath(TEMPLATE, HERE))
    template = read_text(TEMPLATE)

    # 1. expand each include token (a whole line) with its source file's bytes
    out = []
    n_inc = 0
    for line in template.split("\n"):
        m = INCLUDE_RE.match(line)
        if m:
            rel = m.group(1).strip()
            text = read_text(os.path.join(SRC, rel))
            n_inc += 1
            print("  include %2d  %-44s %9d bytes" %
                  (n_inc, rel, len(text.encode("utf-8"))))
            out.append(text)
        else:
            out.append(line)
    assembled = "\n".join(out)

    # 2. re-encode wasm binaries back into the inline base64 strings
    n_wasm = [0]

    def sub_wasm(m):
        rel = m.group(1).strip()
        with open(os.path.join(SRC, rel), "rb") as f:
            raw = f.read()
        b64 = base64.b64encode(raw).decode("ascii")
        n_wasm[0] += 1
        print("  wasm    %2d  %-44s %9d bytes -> %d base64 chars" %
              (n_wasm[0], rel, len(raw), len(b64)))
        return b64
    assembled = WASM_RE.sub(sub_wasm, assembled)

    # 3. drop the legacy src/client-build.html so only one output file remains
    if os.path.exists(LEGACY_OUT):
        print("removing legacy artifact: %s" % os.path.relpath(LEGACY_OUT, HERE))
        os.remove(LEGACY_OUT)

    # 4. write fresh output (delete first) -- this overwrites the shipped client
    if os.path.exists(OUT):
        print("removing old output: %s" % os.path.relpath(OUT, HERE))
        os.remove(OUT)
    with open(OUT, "w", encoding="utf-8", newline="") as f:
        f.write(assembled)
    print("-" * 60)
    print("done: %d includes + %d wasm -> %s (%d bytes)" %
          (n_inc, n_wasm[0], os.path.relpath(OUT, HERE),
           len(assembled.encode("utf-8"))))


if __name__ == "__main__":
    main()
