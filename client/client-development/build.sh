#!/bin/sh
# Glue the split sources in src/ back into src/client-build.html
exec python3 "$(dirname "$0")/build.py" "$@"
