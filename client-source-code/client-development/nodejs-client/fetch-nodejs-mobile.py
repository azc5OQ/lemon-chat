#!/usr/bin/env python3
"""
Fetches the nodejs-mobile prebuilt android runtime (libnode.so per abi + headers) into the android
project. ~120 MB of third-party binaries, deliberately not committed - run once after cloning:

    python fetch-nodejs-mobile.py

Keep the headers and the .so from the SAME release - node.h's Start() signature is the contract.
Do not chase a newer node: nodejs-mobile is a FORK, and node 18's missing WebSocket is already
handled by mini-ws.js.
"""
import io
import os
import shutil
import sys
import urllib.request
import zipfile

NODEJS_MOBILE_VERSION = "v18.20.4"
RELEASE_URL = ("https://github.com/nodejs-mobile/nodejs-mobile/releases/download/"
               + NODEJS_MOBILE_VERSION
               + "/nodejs-mobile-" + NODEJS_MOBILE_VERSION + "-android.zip")

# must match android/app/build.gradle's ndk.abiFilters
WANTED_ABIS = ["arm64-v8a", "armeabi-v7a"]

HERE = os.path.dirname(os.path.abspath(__file__))
ANDROID_MAIN = os.path.normpath(os.path.join(HERE, "..", "android", "app", "src", "main"))
JNI_LIBS = os.path.join(ANDROID_MAIN, "jniLibs")
INCLUDE_DEST = os.path.join(ANDROID_MAIN, "cpp", "nodejs-mobile-include")


def warn_if_abis_disagree_with_gradle():
    """WANTED_ABIS and build.gradle's ndk.abiFilters must stay in step; catch the common case of
    editing one and forgetting the other."""
    import re

    gradle_file = os.path.join(ANDROID_MAIN, "..", "..", "build.gradle")

    try:
        with open(gradle_file, "r", encoding="utf-8") as f:
            gradle_text = f.read()
    except OSError:
        return

    match = re.search(r"abiFilters\s+(.+)", gradle_text)
    if not match:
        return

    gradle_abis = sorted(re.findall(r"'([\w-]+)'", match.group(1)))

    if gradle_abis and gradle_abis != sorted(WANTED_ABIS):
        print("WARNING: abi lists disagree and the apk will be broken for the difference:")
        print("  this script fetches: %s" % sorted(WANTED_ABIS))
        print("  build.gradle ships:  %s" % gradle_abis)
        print("  an abi in gradle but not fetched -> UnsatisfiedLinkError on those devices")


def main():
    print("fetching nodejs-mobile " + NODEJS_MOBILE_VERSION)
    print("  " + RELEASE_URL)

    warn_if_abis_disagree_with_gradle()

    try:
        with urllib.request.urlopen(RELEASE_URL) as response:
            payload = response.read()
    except Exception as download_failed:
        print("FAILED to download: %s" % download_failed)
        return 1

    print("  downloaded %.1f MB" % (len(payload) / (1024.0 * 1024.0)))

    archive = zipfile.ZipFile(io.BytesIO(payload))

    # the .so, one per wanted abi
    for abi in WANTED_ABIS:
        member = "bin/%s/libnode.so" % abi
        destination_dir = os.path.join(JNI_LIBS, abi)
        destination = os.path.join(destination_dir, "libnode.so")

        if not os.path.exists(destination_dir):
            os.makedirs(destination_dir)

        try:
            with archive.open(member) as source, open(destination, "wb") as target:
                shutil.copyfileobj(source, target)
        except KeyError:
            print("FAILED: %s is not in the release archive" % member)
            return 1

        print("  %-14s -> %s (%.1f MB)"
              % (abi, os.path.relpath(destination, HERE),
                 os.path.getsize(destination) / (1024.0 * 1024.0)))

    # the headers native-lib.cpp compiles against
    if os.path.exists(INCLUDE_DEST):
        shutil.rmtree(INCLUDE_DEST)

    header_count = 0
    for member in archive.namelist():
        if not member.startswith("include/") or member.endswith("/"):
            continue

        relative = member[len("include/"):]
        destination = os.path.join(INCLUDE_DEST, relative.replace("/", os.sep))
        parent = os.path.dirname(destination)

        if not os.path.exists(parent):
            os.makedirs(parent)

        with archive.open(member) as source, open(destination, "wb") as target:
            shutil.copyfileobj(source, target)

        header_count += 1

    print("  headers        -> %s (%d files)"
          % (os.path.relpath(INCLUDE_DEST, HERE), header_count))

    print("-" * 60)
    print("done. now run `python build-node.py`, then build the apk.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
