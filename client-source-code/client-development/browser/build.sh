#!/bin/sh
# Glue the split sources in src/ back into the shipped ../../client.html (client-source-code/client.html)
exec node "$(dirname "$0")/build.cjs" "$@"
