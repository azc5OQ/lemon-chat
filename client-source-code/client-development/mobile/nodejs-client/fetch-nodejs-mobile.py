#!/usr/bin/env python3
"""install the pinned Android Node runtime with its matching headers"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile


HERE = Path(__file__).resolve().parent
PIN_FILE = HERE / "nodejs-mobile-runtime.json"
MANIFEST_NAME = "runtime.json"
CHUNK_SIZE = 1024 * 1024


def sha256_file(path):
    """hash a file without loading the library into memory"""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(CHUNK_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def load_pin():
    """read the shared version and archive pin"""
    with PIN_FILE.open(encoding="utf-8") as source:
        pin = json.load(source)
    for key in ("archive_sha256", "header_tree_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", pin[key]):
            raise ValueError("Invalid SHA256 in " + PIN_FILE.name)
    if not pin["archive_url"].startswith("https://github.com/fogtape/nodejs-mobile/releases/download/"):
        raise ValueError("Unexpected runtime download URL")
    if not set(pin["default_abis"]).issubset(pin["supported_abis"]):
        raise ValueError("Default runtime architectures are not supported")
    return pin


def parse_abis(value, pin):
    """reject unsupported or repeated target architectures"""
    abis = pin["default_abis"] if value is None else value.split(",")
    if not abis or len(set(abis)) != len(abis) or any(abi not in pin["supported_abis"] for abi in abis):
        raise ValueError("Choose a comma separated subset of " + ", ".join(pin["supported_abis"]) + ". ARMv6 is unsupported")
    return sorted(abis)


def tree_hash(files):
    """bind header paths and contents to the release pin"""
    text = "".join(name + "\0" + digest + "\n" for name, digest in sorted(files.items()))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def expected_metadata(pin, abis):
    """describe the runtime selected by this build"""
    return {
        "format": 1,
        "version": pin["version"],
        "upstream_version": pin["upstream_version"],
        "flavor": pin["flavor"],
        "archive_sha256": pin["archive_sha256"],
        "abis": abis,
    }


def validate_runtime(directory, pin, abis):
    """verify every installed file against the release and manifest"""
    manifest_path = directory / MANIFEST_NAME
    if not manifest_path.is_file() or manifest_path.stat().st_size > 128 * 1024:
        raise ValueError("Runtime manifest is missing or invalid: " + str(directory))
    with manifest_path.open(encoding="utf-8") as source:
        manifest = json.load(source)
    for key, value in expected_metadata(pin, abis).items():
        if manifest.get(key) != value:
            raise ValueError("Runtime version or architectures do not match: " + str(directory))
    actual = {}
    for path in directory.rglob("*"):
        if path.is_symlink():
            raise ValueError("Unexpected link in installed runtime: " + str(path))
        if path.is_file() and path != manifest_path:
            actual[path.relative_to(directory).as_posix()] = sha256_file(path)
    if manifest.get("files") != actual:
        raise ValueError("Installed runtime files changed: " + str(directory))
    headers = {name: digest for name, digest in actual.items() if name.startswith("include/")}
    if len(headers) != pin["header_count"] or tree_hash(headers) != pin["header_tree_sha256"]:
        raise ValueError("Installed headers do not match the pinned release")
    expected_libraries = {"jniLibs/" + abi + "/libnode.so": pin["library_sha256"][abi] for abi in abis}
    expected_libraries.update({notice["path"]: notice["sha256"] for notice in pin.get("notices", [])})
    libraries = {name: digest for name, digest in actual.items() if not name.startswith("include/")}
    if libraries != expected_libraries:
        raise ValueError("Installed native libraries do not match the pinned release")
    return manifest


def validate_archive(path, pin):
    """reject a changed or incomplete runtime download"""
    if path.stat().st_size != pin["archive_size"] or sha256_file(path) != pin["archive_sha256"]:
        raise ValueError("Runtime archive checksum or size does not match the pinned release: " + str(path))


class HttpsRedirect(urllib.request.HTTPRedirectHandler):
    """keep download redirects on HTTPS"""

    def redirect_request(self, request, response, code, message, headers, url):
        if urllib.parse.urlsplit(url).scheme != "https":
            raise ValueError("Runtime download redirected away from HTTPS")
        return super().redirect_request(request, response, code, message, headers, url)


def cache_directory():
    """keep downloaded archives outside the checkout"""
    override = os.environ.get("LEMON_NODE_RUNTIME_CACHE")
    if override:
        return Path(override).expanduser().resolve()
    if os.name == "nt":
        root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    elif sys.platform == "darwin":
        root = Path.home() / "Library" / "Caches"
    else:
        root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return root / "lemon-chat" / "nodejs-mobile"


def obtain_archive(pin, provided):
    """reuse a verified archive or download into a temporary file"""
    if provided:
        archive = Path(provided).expanduser().resolve()
        validate_archive(archive, pin)
        return archive
    cache = cache_directory()
    archive = cache / (pin["archive_sha256"] + ".zip")
    if archive.is_file():
        validate_archive(archive, pin)
        return archive
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".download-", dir=cache) as temporary:
        partial = Path(temporary) / "runtime.zip"
        request = urllib.request.Request(pin["archive_url"], headers={"User-Agent": "LemonChat-runtime-build"})
        print("Downloading " + pin["archive_url"], file=sys.stderr)
        opener = urllib.request.build_opener(HttpsRedirect())
        with opener.open(request, timeout=60) as response, partial.open("wb") as target:
            count = 0
            while True:
                block = response.read(CHUNK_SIZE)
                if not block:
                    break
                count += len(block)
                if count > pin["archive_size"]:
                    raise ValueError("Runtime download exceeded its pinned size")
                target.write(block)
            target.flush()
            os.fsync(target.fileno())
        validate_archive(partial, pin)
        try:
            os.replace(partial, archive)
        except OSError:
            if not archive.is_file():
                raise
            validate_archive(archive, pin)
    return archive


def safe_archive_paths(archive):
    """reject archive paths that could escape or overwrite another entry"""
    names = set()
    total_size = 0
    entries = archive.infolist()
    if len(entries) > 4096:
        raise ValueError("Runtime archive contains too many entries")
    for entry in entries:
        name = entry.orig_filename
        path = PurePosixPath(name)
        if not name or "\\" in name or ":" in name or path.is_absolute() or ".." in path.parts or "\0" in name:
            raise ValueError("Unsafe runtime archive path: " + name)
        normalized = path.as_posix().casefold()
        if normalized in names:
            raise ValueError("Duplicate runtime archive path: " + name)
        names.add(normalized)
        mode = entry.external_attr >> 16
        if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
            raise ValueError("Unexpected link or special file in runtime archive")
        total_size += entry.file_size
        if total_size > 512 * 1024 * 1024:
            raise ValueError("Runtime archive exceeds the extraction limit")
    return entries


def validate_elf(path, abi):
    """check the target architecture and native page alignment"""
    expected = {"armeabi-v7a": (1, 40), "arm64-v8a": (2, 183), "x86_64": (2, 62)}
    with path.open("rb") as source:
        header = source.read(64)
        elf_class, machine = expected[abi]
        if len(header) < 64 or header[:4] != b"\x7fELF" or header[4] != elf_class or header[5] != 1 or struct.unpack_from("<H", header, 18)[0] != machine:
            raise ValueError("Runtime library has the wrong architecture: " + abi)
        if elf_class == 2:
            offset = struct.unpack_from("<Q", header, 32)[0]
            entry_size, count = struct.unpack_from("<HH", header, 54)
            expected_size = 56
        else:
            offset = struct.unpack_from("<I", header, 28)[0]
            entry_size, count = struct.unpack_from("<HH", header, 42)
            expected_size = 32
        if entry_size != expected_size or count < 1 or count > 128 or offset + count * entry_size > path.stat().st_size:
            raise ValueError("Invalid native program header table")
        source.seek(offset)
        loads = 0
        for _ in range(count):
            entry = source.read(entry_size)
            if struct.unpack_from("<I", entry)[0] == 1:
                alignment = struct.unpack_from("<Q" if elf_class == 2 else "<I", entry, 48 if elf_class == 2 else 28)[0]
                if alignment < 16384:
                    raise ValueError("Runtime library lacks 16 KB page alignment: " + abi)
                loads += 1
        if not loads:
            raise ValueError("Runtime library has no loadable segments")


def prepare_runtime(archive_path, staging, pin, abis):
    """extract and validate the complete runtime before publication"""
    files = {}
    with zipfile.ZipFile(archive_path) as archive:
        entries = safe_archive_paths(archive)
        for entry in entries:
            if entry.is_dir():
                continue
            name = entry.filename
            destination = None
            if name.startswith("include/"):
                destination = staging / name
            else:
                for abi in abis:
                    if name == "bin/" + abi + "/libnode.so":
                        destination = staging / "jniLibs" / abi / "libnode.so"
                        break
            if destination is None:
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, destination.open("xb") as target:
                shutil.copyfileobj(source, target, CHUNK_SIZE)
                target.flush()
                os.fsync(target.fileno())
            files[destination.relative_to(staging).as_posix()] = sha256_file(destination)
    version_header = (staging / "include" / "node" / "node_version.h").read_text(encoding="utf-8")
    versions = [re.search(r"^#define NODE_" + part + r"_VERSION\s+(\d+)\s*$", version_header, re.MULTILINE) for part in ("MAJOR", "MINOR", "PATCH")]
    if not all(versions) or ".".join(match.group(1) for match in versions) != pin["upstream_version"]:
        raise ValueError("Runtime headers contain the wrong Node version")
    for abi in abis:
        validate_elf(staging / "jniLibs" / abi / "libnode.so", abi)
    for notice in pin.get("notices", []):
        source = (HERE / notice["source"]).resolve()
        destination = staging / notice["path"]
        if HERE not in source.parents or PurePosixPath(notice["path"]).is_absolute() or ".." in PurePosixPath(notice["path"]).parts:
            raise ValueError("Invalid runtime notice path")
        if sha256_file(source) != notice["sha256"]:
            raise ValueError("Runtime license notice does not match its pin")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        files[notice["path"]] = notice["sha256"]
    manifest = expected_metadata(pin, abis)
    manifest["files"] = files
    with (staging / MANIFEST_NAME).open("x", encoding="utf-8") as target:
        json.dump(manifest, target, indent=2, sort_keys=True)
        target.write("\n")
        target.flush()
        os.fsync(target.fileno())
    validate_runtime(staging, pin, abis)


def install_runtime(output, pin, abis, provided=None, check=False):
    """publish one verified directory without replacing an existing runtime"""
    if output.exists():
        validate_runtime(output, pin, abis)
        return
    if check:
        raise ValueError("Runtime is not installed: " + str(output))
    archive = obtain_archive(pin, provided)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".node-runtime-", dir=output.parent) as temporary:
        staging = Path(temporary) / "ready"
        staging.mkdir()
        prepare_runtime(archive, staging, pin, abis)
        try:
            staging.rename(output)
        except OSError:
            if not output.is_dir():
                raise
            validate_runtime(output, pin, abis)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", action="store_true", help="print the pinned release metadata without downloading")
    parser.add_argument("--abis", help="comma separated target architectures, default both ARM architectures")
    parser.add_argument("--output-dir", type=Path, help="immutable directory for matching headers and libraries")
    parser.add_argument("--archive", help="use a local archive with the pinned checksum")
    parser.add_argument("--check", action="store_true", help="verify an installed runtime without writing or downloading")
    arguments = parser.parse_args(argv)
    try:
        pin = load_pin()
        if arguments.metadata:
            print(json.dumps(pin, sort_keys=True))
            return 0
        abis = parse_abis(arguments.abis, pin)
        output = arguments.output_dir or HERE.parent / "android" / "app" / "build" / "generated" / "node-runtime" / pin["version"] / "_".join(abis)
        output = output.expanduser().resolve()
        install_runtime(output, pin, abis, arguments.archive, arguments.check)
        print("Verified Node " + pin["upstream_version"] + " mobile " + pin["version"] + " (" + ", ".join(abis) + "): " + str(output))
        return 0
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print("Runtime setup failed: " + str(error), file=sys.stderr)
        print("Existing runtime directories were not replaced. Use a fresh generated output directory if its contents were changed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
