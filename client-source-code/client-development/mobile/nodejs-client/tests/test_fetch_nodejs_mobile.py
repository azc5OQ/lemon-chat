"""runtime download and publication regression checks"""

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile


SCRIPT = Path(__file__).resolve().parents[1] / "fetch-nodejs-mobile.py"
SPEC = importlib.util.spec_from_file_location("fetch_nodejs_mobile", SCRIPT)
FETCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FETCH)


def elf_library(abi, alignment=16384):
    """make a minimal ELF fixture with one loadable segment"""
    elf_class, machine = {"arm64-v8a": (2, 183), "armeabi-v7a": (1, 40), "x86_64": (2, 62)}[abi]
    data = bytearray(128)
    data[:6] = b"\x7fELF" + bytes([elf_class, 1])
    struct.pack_into("<H", data, 18, machine)
    if elf_class == 2:
        struct.pack_into("<Q", data, 32, 64)
        struct.pack_into("<HH", data, 54, 56, 1)
        struct.pack_into("<Q", data, 112, alignment)
    else:
        struct.pack_into("<I", data, 28, 64)
        struct.pack_into("<HH", data, 42, 32, 1)
        struct.pack_into("<I", data, 92, alignment)
    struct.pack_into("<I", data, 64, 1)
    return bytes(data)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="node runtime tests ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "runtime archive.zip"
        self.output = self.root / "installed runtime"
        self.abis = ["arm64-v8a", "armeabi-v7a", "x86_64"]
        self.headers = {
            "include/node/node_version.h": b"#define NODE_MAJOR_VERSION 24\n#define NODE_MINOR_VERSION 22\n#define NODE_PATCH_VERSION 1\n",
            "include/node/node.h": b"fixture header\n",
        }
        self.libraries = {abi: elf_library(abi) for abi in self.abis}
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, data in self.headers.items():
                archive.writestr(name, data)
            for abi, data in self.libraries.items():
                archive.writestr("bin/" + abi + "/libnode.so", data)
        self.pin = {
            "version": "24.22.1-0",
            "upstream_version": "24.22.1",
            "flavor": "full",
            "supported_abis": self.abis,
            "default_abis": self.abis[:2],
            "archive_size": self.archive.stat().st_size,
            "archive_sha256": FETCH.sha256_file(self.archive),
            "header_count": len(self.headers),
            "header_tree_sha256": FETCH.tree_hash({name: hashlib.sha256(data).hexdigest() for name, data in self.headers.items()}),
            "library_sha256": {abi: hashlib.sha256(data).hexdigest() for abi, data in self.libraries.items()},
        }

    def install(self, abis=None):
        FETCH.install_runtime(self.output, self.pin, abis or self.abis, self.archive)

    def test_all_architectures_and_matching_headers(self):
        self.install()
        manifest = FETCH.validate_runtime(self.output, self.pin, self.abis)
        self.assertEqual(len(manifest["files"]), 5)
        self.assertEqual((self.output / "include/node/node.h").read_bytes(), self.headers["include/node/node.h"])

    def test_selected_architecture_does_not_install_others(self):
        self.install(["x86_64"])
        self.assertEqual([path.name for path in (self.output / "jniLibs").iterdir()], ["x86_64"])

    def test_reuse_is_offline_and_does_not_write(self):
        self.install()
        before = {path: path.stat().st_mtime_ns for path in self.output.rglob("*")}
        with patch.object(FETCH, "obtain_archive", side_effect=AssertionError("download attempted")):
            FETCH.install_runtime(self.output, self.pin, self.abis)
            FETCH.install_runtime(self.output, self.pin, self.abis, check=True)
        self.assertEqual(before, {path: path.stat().st_mtime_ns for path in self.output.rglob("*")})

    def test_missing_check_does_not_write_or_download(self):
        with patch.object(FETCH, "obtain_archive", side_effect=AssertionError("download attempted")):
            with self.assertRaisesRegex(ValueError, "not installed"):
                FETCH.install_runtime(self.output, self.pin, self.abis, check=True)
        self.assertFalse(self.output.exists())

    def test_bad_archive_does_not_publish(self):
        self.pin["archive_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.install()
        self.assertFalse(self.output.exists())

    def test_failed_extraction_leaves_no_output(self):
        with patch.object(FETCH, "prepare_runtime", side_effect=OSError("interrupted")):
            with self.assertRaisesRegex(OSError, "interrupted"):
                self.install()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob(".node-runtime-*")), [])

    def test_old_or_changed_runtime_is_not_overwritten(self):
        self.install()
        header = self.output / "include/node/node.h"
        header.write_text("old header", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "files changed"):
            self.install()
        self.assertEqual(header.read_text(), "old header")

    def test_changed_manifest_cannot_bypass_header_pin(self):
        self.install()
        header = self.output / "include/node/node.h"
        header.write_text("different", encoding="utf-8")
        manifest_path = self.output / "runtime.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["include/node/node.h"] = FETCH.sha256_file(header)
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "headers do not match"):
            FETCH.validate_runtime(self.output, self.pin, self.abis)

    def test_changed_manifest_cannot_bypass_library_pin(self):
        self.install()
        library = self.output / "jniLibs/x86_64/libnode.so"
        library.write_bytes(b"changed")
        manifest_path = self.output / "runtime.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["files"]["jniLibs/x86_64/libnode.so"] = FETCH.sha256_file(library)
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "libraries do not match"):
            FETCH.validate_runtime(self.output, self.pin, self.abis)

    def test_different_architectures_cannot_reuse_directory(self):
        self.install()
        with self.assertRaisesRegex(ValueError, "architectures do not match"):
            FETCH.validate_runtime(self.output, self.pin, ["x86_64"])

    def test_wrong_header_version_does_not_publish(self):
        self.pin["upstream_version"] = "18.20.4"
        with self.assertRaisesRegex(ValueError, "wrong Node version"):
            self.install()
        self.assertFalse(self.output.exists())

    def test_unsafe_archive_paths_are_rejected(self):
        for name in ("include/../../escape", "/include/escape", "include\\escape", "include/C:escape"):
            with self.subTest(name=name):
                stream = io.BytesIO()
                with zipfile.ZipFile(stream, "w") as archive:
                    entry = zipfile.ZipInfo("placeholder")
                    entry.filename = name
                    archive.writestr(entry, b"bad")
                stream.seek(0)
                with zipfile.ZipFile(stream) as archive:
                    with self.assertRaisesRegex(ValueError, "Unsafe"):
                        FETCH.safe_archive_paths(archive)

    def test_archive_links_and_duplicate_paths_are_rejected(self):
        for duplicate in (True, False):
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, "w") as archive:
                if duplicate:
                    archive.writestr("include/Node.h", b"one")
                    archive.writestr("include/node.h", b"two")
                else:
                    entry = zipfile.ZipInfo("include/link")
                    entry.create_system = 3
                    entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(entry, b"../../escape")
            stream.seek(0)
            with zipfile.ZipFile(stream) as archive:
                with self.assertRaises(ValueError):
                    FETCH.safe_archive_paths(archive)

    def test_native_alignment_and_architecture_are_checked(self):
        library = self.root / "test.so"
        library.write_bytes(elf_library("arm64-v8a", 4096))
        with self.assertRaisesRegex(ValueError, "16 KB"):
            FETCH.validate_elf(library, "arm64-v8a")
        library.write_bytes(elf_library("arm64-v8a"))
        with self.assertRaisesRegex(ValueError, "architecture"):
            FETCH.validate_elf(library, "x86_64")

    def test_abi_validation(self):
        self.assertEqual(FETCH.parse_abis(None, self.pin), self.abis[:2])
        for value in ("", "armeabi", "armv6", "x86_64,x86_64", "x86_64,../bad"):
            with self.assertRaises(ValueError):
                FETCH.parse_abis(value, self.pin)

    def test_http_redirect_is_rejected(self):
        handler = FETCH.HttpsRedirect()
        with self.assertRaisesRegex(ValueError, "HTTPS"):
            handler.redirect_request(None, None, 302, "redirect", {}, "http://example.com/runtime.zip")

    def test_download_is_verified_and_reused(self):
        self.pin["archive_url"] = "https://example.com/runtime.zip"
        payload = self.archive.read_bytes()
        opener = unittest.mock.Mock()
        opener.open.return_value = io.BytesIO(payload)
        with patch.object(FETCH, "cache_directory", return_value=self.root / "cache"), patch.object(FETCH.urllib.request, "build_opener", return_value=opener):
            result = FETCH.obtain_archive(self.pin, None)
            self.assertEqual(result.read_bytes(), payload)
            FETCH.obtain_archive(self.pin, None)
            self.assertEqual(opener.open.call_count, 1)

    def test_oversized_download_is_not_cached(self):
        self.pin["archive_url"] = "https://example.com/runtime.zip"
        opener = unittest.mock.Mock()
        opener.open.return_value = io.BytesIO(self.archive.read_bytes() + b"unexpected")
        cache = self.root / "cache"
        with patch.object(FETCH, "cache_directory", return_value=cache), patch.object(FETCH.urllib.request, "build_opener", return_value=opener):
            with self.assertRaisesRegex(ValueError, "exceeded"):
                FETCH.obtain_archive(self.pin, None)
        self.assertEqual(list(cache.iterdir()), [])

    def test_interrupted_download_is_not_cached(self):
        self.pin["archive_url"] = "https://example.com/runtime.zip"
        response = unittest.mock.MagicMock()
        response.__enter__.return_value = response
        response.read.side_effect = [b"partial", OSError("connection closed")]
        opener = unittest.mock.Mock()
        opener.open.return_value = response
        cache = self.root / "cache"
        with patch.object(FETCH, "cache_directory", return_value=cache), patch.object(FETCH.urllib.request, "build_opener", return_value=opener):
            with self.assertRaisesRegex(OSError, "connection closed"):
                FETCH.obtain_archive(self.pin, None)
        self.assertEqual(list(cache.iterdir()), [])

    def test_concurrent_complete_install_is_reused(self):
        prepare = FETCH.prepare_runtime

        def competitor(archive, staging, pin, abis):
            prepare(archive, staging, pin, abis)
            shutil.copytree(staging, self.output)

        with patch.object(FETCH, "prepare_runtime", side_effect=competitor):
            self.install()
        FETCH.validate_runtime(self.output, self.pin, self.abis)
        self.assertEqual(list(self.root.glob(".node-runtime-*")), [])

    def test_pinned_license_notices_are_required(self):
        notice = self.root / "licenses" / "node.txt"
        notice.parent.mkdir()
        notice.write_text("license fixture", encoding="utf-8")
        self.pin["notices"] = [{"source": "licenses/node.txt", "path": "notices/node.txt", "sha256": FETCH.sha256_file(notice)}]
        with patch.object(FETCH, "HERE", self.root):
            self.install()
        FETCH.validate_runtime(self.output, self.pin, self.abis)
        self.assertEqual((self.output / "notices/node.txt").read_text(), "license fixture")
        (self.output / "notices/node.txt").unlink()
        with self.assertRaisesRegex(ValueError, "files changed"):
            FETCH.validate_runtime(self.output, self.pin, self.abis)


if __name__ == "__main__":
    unittest.main()
