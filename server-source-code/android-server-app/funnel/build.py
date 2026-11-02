"""Build the packaged Android Funnel executable from pinned Go dependencies."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil


def write_notices(go, source, environment, build_module, destination):
    modules = subprocess.check_output(
        [go, "list", "-mod=readonly", "-modfile=" + str(build_module), "-deps",
         "-f", "{{with .Module}}{{.Path}}|{{.Dir}}{{end}}", "."],
        cwd=source, env=environment, text=True)
    sections = []
    for entry in sorted(set(modules.splitlines())):
        if "|" not in entry:
            continue
        name, directory = entry.split("|", 1)
        if name == "lemonchat/android-funnel":
            continue
        for path in sorted(Path(directory).iterdir()):
            basename = path.name.upper().split(".", 1)[0]
            if path.is_file() and (basename in ("LICENSE", "LICENCE", "COPYING", "NOTICE") or basename.startswith("LICENSE-")):
                sections.append(name + " / " + path.name + "\n\n" + path.read_text(encoding="utf-8", errors="replace"))
    go_root = subprocess.check_output([go, "env", "GOROOT"], env=environment, text=True).strip()
    sections.append("Go / LICENSE\n\n" + (Path(go_root) / "LICENSE").read_text(encoding="utf-8"))
    destination.write_text("\n\n".join(sections), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--abi", choices=["arm64-v8a", "armeabi-v7a", "x86_64"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--go", default="go")
    options = parser.parse_args()
    source = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    generated = output.parent.parent.parent / "funnel-build" / options.abi
    generated.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    module = json.loads(subprocess.check_output(
        [options.go, "mod", "download", "-json", "tailscale.com"], cwd=source, env=environment))
    if module.get("Version") != "v1.104.1":
        raise RuntimeError("Review the Android certificate fix before changing Tailscale versions.")
    module_path = generated / "tailscale"
    shutil.copytree(module["Dir"], module_path, dirs_exist_ok=True, copy_function=shutil.copyfile)
    replacements = {
        "cert.go": ("//go:build !ios && !android && !js && !ts_omit_acme",
                    "//go:build !ios && !js && !ts_omit_acme"),
        "disabled_stubs.go": ("//go:build ios || android || js", "//go:build ios || js"),
    }
    # Upstream PR 21720 enables the certificate endpoint on Android.
    # Patch a build copy and leave the downloaded module unchanged.
    for filename, (old, new) in replacements.items():
        original = module_path / "ipn" / "localapi" / filename
        content = original.read_text(encoding="utf-8")
        if content.count(old) != 1:
            raise RuntimeError("Unexpected Tailscale certificate source: " + str(original))
        original.write_text(content.replace(old, new), encoding="utf-8")
    build_module = generated / "go.mod"
    build_module.write_text((source / "go.mod").read_text(encoding="utf-8") +
                           "\nreplace tailscale.com => " + json.dumps(str(module_path)) + "\n", encoding="utf-8")
    shutil.copyfile(source / "go.sum", generated / "go.sum")
    platforms = {"arm64-v8a": ("arm64", "aarch64-linux-android"),
                 "armeabi-v7a": ("arm", "armv7a-linux-androideabi"),
                 "x86_64": ("amd64", "x86_64-linux-android")}
    arch, target = platforms[options.abi]
    host = "windows-x86_64" if os.name == "nt" else ("darwin-x86_64" if os.uname().sysname == "Darwin" else "linux-x86_64")
    compiler = options.ndk.resolve() / "toolchains" / "llvm" / "prebuilt" / host / "bin" / ("clang.exe" if os.name == "nt" else "clang")
    if not compiler.is_file():
        raise RuntimeError("Android NDK compiler not found: " + str(compiler))
    environment.update(GOOS="android", GOARCH=arch, GOARM="7", CGO_ENABLED="1",
                       CC='"' + str(compiler) + '" --target=' + target + '26')
    subprocess.run([options.go, "build", "-mod=readonly", "-trimpath", "-buildmode=pie",
                    "-modfile=" + str(build_module),
                    "-ldflags=-s -w -extldflags=-Wl,-z,max-page-size=16384",
                    "-o", str(output), "."], cwd=source, env=environment, check=True)
    write_notices(options.go, source, environment, build_module, generated / "tailscale-notices.txt")
    print("Built " + str(output))


if __name__ == "__main__":
    main()
