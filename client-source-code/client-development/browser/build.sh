#!/bin/sh
# Glue the split sources in src/ back into the shipped ../../client.html (client-source-code/client.html)
exec python3 "$(dirname "$0")/build.py" "$@"
