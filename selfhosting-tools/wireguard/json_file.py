"""Small, atomic JSON writer for the POSIX host scripts (Python 3)."""

import json
import os
import stat
import sys
import tempfile


def write(path, pairs, merge):
    """Merge JSON values into settings, or replace saved answers with strings."""
    data = {}
    original = None
    if merge:
        with open(path, encoding="utf-8") as source:
            original = source.read()
        data = json.loads(original)
    for pair in pairs:
        key, value = pair.split("=", 1)
        data[key] = json.loads(value) if merge else value
    # Stay on the destination filesystem so os.replace can swap the file atomically.
    fd, temporary = tempfile.mkstemp(prefix=".tunnel-json-", dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as target:
            if os.path.exists(path):
                info = os.stat(path)
                if hasattr(os, "geteuid") and os.geteuid() == 0:
                    os.chown(temporary, info.st_uid, info.st_gid)
                os.chmod(temporary, stat.S_IMODE(info.st_mode))
            json.dump(data, target, indent=2, ensure_ascii=False)
            target.write("\n")
            target.flush()
            os.fsync(target.fileno())
        # Catch edits made while preparing the replacement. This is not a file lock;
        # the server should be stopped while its settings are being changed.
        if merge:
            with open(path, encoding="utf-8") as source:
                if source.read() != original:
                    raise RuntimeError("settings changed during setup; stop the server and retry")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


if __name__ == "__main__":
    write(sys.argv[2], sys.argv[3:], sys.argv[1] == "settings")
