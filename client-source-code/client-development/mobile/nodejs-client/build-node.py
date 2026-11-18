#!/usr/bin/env python3
"""
this script builds bundle.js, the chat client for Node.js on Android. it reads node-template.js and replaces each
@@INCLUDE line with the named file, in the same way as ../../browser/build.cjs builds the web page.

the bundle holds the browser page's JavaScript without the parts that draw the page. ui.js and sounds.js are left
out, because their global names would replace the stand-ins of dom-shim.js. the audio, WebAssembly and helper thread
files are left out, because voice is handled by the page in the app window.

paths that start with node/ are files of this folder. all other paths are under ../../browser/main
"""
import base64
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.normpath(os.path.join(HERE, "..", "..", "browser"))
SRC = os.path.join(DEV, "main")
TEMPLATE = os.path.join(HERE, "node-template.js")
OUT = os.path.join(HERE, "bundle.js")

INCLUDE_RE = re.compile(r"^\s*/\*\s*@@INCLUDE:\s*(.+?)\s*@@\s*\*/\s*$")
# same token build.cjs resolves: rsa-crypto.js carries its wasm as @@WASM:wasm/rsa_keygen.wasm@@
WASM_RE = re.compile(r"@@WASM:([^@]+)@@")


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def resolve(rel):
    if rel.startswith("node/"):
        return os.path.join(HERE, rel[len("node/"):])
    return os.path.join(SRC, rel)


def input_paths():
    paths = {os.path.abspath(__file__), TEMPLATE}
    for runtime in ["android-main.js", "android-bridge-client.js", "loopback-ui-server.js",
                    "loopback-transfer.js", "websocket-adapter.js", "transport-limits.js", "load-bundle.js"]:
        paths.add(os.path.join(HERE, runtime))
    for line in read_text(TEMPLATE).split("\n"):
        match = INCLUDE_RE.match(line)
        if match:
            source = resolve(match.group(1).strip())
            paths.add(source)
            for wasm in WASM_RE.finditer(read_text(source)):
                paths.add(os.path.join(SRC, wasm.group(1).strip()))
    lock_path = os.path.join(HERE, "vendor", "ws-lock.json")
    paths.add(lock_path)
    lock = json.loads(read_text(lock_path))
    for relative in lock["files"]:
        paths.add(os.path.join(HERE, "vendor", "ws", relative))
    return sorted(os.path.abspath(path) for path in paths)


def verify_vendor():
    lock = json.loads(read_text(os.path.join(HERE, "vendor", "ws-lock.json")))
    for relative, expected in lock["files"].items():
        source = os.path.join(HERE, "vendor", "ws", relative)
        with open(source, "rb") as vendor_file:
            actual = hashlib.sha256(vendor_file.read()).hexdigest()
        if actual != expected:
            raise ValueError("vendored ws checksum mismatch: " + relative)


def main():
    if "--list-inputs" in sys.argv:
        print("\n".join(input_paths()))
        return 0
    verify_vendor()
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

    # re-encode the wasm binaries into the inline base64 strings, exactly like build.cjs does
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
