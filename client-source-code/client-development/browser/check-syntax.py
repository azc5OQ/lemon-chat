#!/usr/bin/env python3
"""
Syntax guard for the split client sources. Run it before and after editing ANY file under src/.

  python check-syntax.py        exit 0 = ok, 1 = broken

WHY THIS EXISTS

Every .js under src/scripts is a FRAGMENT, not a module. vendor/aes-js.js opens
`function moduleFactory()` and app/main.js closes it with `}));`, so app/messages.js
and app/ui.js sit INSIDE that function body and start at 12-space indent on a bare
`var x = {`. None of them parses on its own; only the concatenation does.

That matters more than it looks, because the five webworkers are built by
re-serialising that same function:

    audio-opus-glue.js:  code = moduleFactory.toString()
                         -> patch THREAD_NAME
                         -> URL.createObjectURL(new Blob(['(', code, ')();']))
                         -> new Worker(url)

So ONE unbalanced brace in ONE fragment takes out the page AND all five workers at
once. Eyeballing does not reliably catch it. This does: it expands the same
@@INCLUDE tokens build.py uses, in memory, and runs `node --check` on each script
block. It never writes client.html, so it is safe to run as often as you like.

It also asserts the THREAD_NAME patch invariant, which is order- and length-sensitive
and fails SILENTLY (workers end up with no onmessage handler at all, not an error).

Requires node on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
TEMPLATE = os.path.join(SRC, "template.html")

INCLUDE_RE = re.compile(r"^\s*/\*\s*@@INCLUDE:\s*(.+?)\s*@@\s*\*/\s*$")

THREAD_NAME_LITERAL = "var THREAD_NAME = "
# the patcher overwrites the padding in place with the worker name, so the padding
# must stay at least as long as the longest name it is ever given
LONGEST_WORKER_NAME = "data_processing_worker"


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def collect_script_blocks():
    """
    [(ordered_includes, expanded_source)] for each contiguous run of .js includes.
    A run IS a <script> block: template.html puts one group of include lines per
    block and nothing else between them. Grouping on the includes avoids parsing
    HTML, and the .style includes drop out for free.
    """
    template = read_text(TEMPLATE)
    blocks = []
    includes = []

    def flush():
        if not includes:
            return
        body = [read_text(os.path.join(SRC, rel)) for rel in includes]
        blocks.append((list(includes), "\n".join(body)))
        del includes[:]

    for line in template.split("\n"):
        m = INCLUDE_RE.match(line)
        if m:
            rel = m.group(1).strip()
            if rel.endswith(".js"):
                includes.append(rel)
            else:
                flush()
        elif line.strip() == "":
            continue
        else:
            flush()

    flush()
    return blocks


def node_check(source, label):
    fd, path = tempfile.mkstemp(suffix=".js", dir=HERE)
    os.close(fd)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(source)
    try:
        proc = subprocess.Popen(["node", "--check", path],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                universal_newlines=True)
        out, err = proc.communicate()
        if proc.returncode != 0:
            print("FAIL  %s" % label)
            print((err or out).strip())
            return False
        print("ok    %s" % label)
        return True
    finally:
        os.remove(path)


def check_thread_name_invariant():
    """
    create_new_webworker_in_same_file patches the SECOND occurrence of
    'var THREAD_NAME = ' in moduleFactory.toString(). First must be the string
    literal in audio-opus-glue.js, second the real declaration in worker-entry.js.
    Any other arrangement leaves every worker without an onmessage handler, silently.
    """
    ok = True
    hits = []

    for root, _dirs, files in os.walk(os.path.join(SRC, "scripts")):
        for name in sorted(files):
            if name.endswith(".js"):
                path = os.path.join(root, name)
                count = read_text(path).count(THREAD_NAME_LITERAL)
                for _ in range(count):
                    hits.append(os.path.relpath(path, SRC).replace("\\", "/"))

    # order them the way the bundle sees them, using template.html's include order
    order = [rel for block in collect_script_blocks() for rel in block[0]]
    hits.sort(key=lambda rel: order.index(rel) if rel in order else len(order))

    if len(hits) != 2:
        print("FAIL  THREAD_NAME: expected exactly 2 occurrences of %r, found %d: %s"
              % (THREAD_NAME_LITERAL, len(hits), hits))
        ok = False
    elif "audio-opus-glue.js" not in hits[0] or "worker-entry.js" not in hits[1]:
        print("FAIL  THREAD_NAME: wrong order. expected "
              "[audio-opus-glue.js, worker-entry.js], got %s" % hits)
        ok = False
    else:
        print("ok    THREAD_NAME patch target: %s then %s" % (hits[0], hits[1]))

    entry = read_text(os.path.join(SRC, "scripts/app/worker-entry.js"))
    m = re.search(r'var THREAD_NAME = "( *)"', entry)
    if m is None:
        print("FAIL  THREAD_NAME: worker-entry.js declaration is not the expected spaces literal")
        ok = False
    else:
        pad = len(m.group(1))
        if pad < len(LONGEST_WORKER_NAME):
            print("FAIL  THREAD_NAME: padding is %d spaces, needs >= %d for %r"
                  % (pad, len(LONGEST_WORKER_NAME), LONGEST_WORKER_NAME))
            ok = False
        else:
            print("ok    THREAD_NAME padding: %d spaces (longest name %d)"
                  % (pad, len(LONGEST_WORKER_NAME)))

    return ok


def main():
    blocks = collect_script_blocks()
    if not blocks:
        print("FAIL  no <script> blocks with .js includes found in template.html")
        return 1

    all_ok = True
    for includes, source in blocks:
        label = "script block: %s" % ", ".join(os.path.basename(i) for i in includes)
        if len(label) > 110:
            label = label[:107] + "..."
        if not node_check(source, label):
            all_ok = False

    if not check_thread_name_invariant():
        all_ok = False

    print("-" * 60)
    print("SYNTAX GUARD: %s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
