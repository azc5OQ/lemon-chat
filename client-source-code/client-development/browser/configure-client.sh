#!/bin/sh
# starts tools/configure-client.py with Python 3.8 or newer
# without options the script asks its questions; with options it asks nothing
# example: sh configure-client.sh --help
set -eu
browser_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# find a working python and run the script
for candidate in python3 python
do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>/dev/null
    then
        exec "$candidate" "$browser_directory/tools/configure-client.py" "$@"
    fi
done

# no python found
echo '[!] Python 3.8 or newer was not found. Install it, then start this file again.' >&2
exit 1
