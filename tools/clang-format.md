# clang-format

The formatting scripts of this folder use clang-format 23.1.5 for C, C++ and Java and, before a commit, js-beautify
2.0.3 for JavaScript. The workflow and the rules are described in the [project README](../README.md#code-formatting) and
in [docs/committing.md](../docs/committing.md). Before every commit, run `format-before-commit.bat` or
`format-before-commit.sh`.

| File | Purpose |
|---|---|
| `format-before-commit.bat`, `format-before-commit.sh` | Run before every commit. They format the C, C++, Java and JavaScript files that differ from the last commit with `before-commit.clang-format` and `before-commit.jsbeautifyrc`. |
| `format-codebase.bat`, `format-codebase.sh` | Format the C, C++ and Java files of `server-source-code`, `client-source-code` or both with the own rules of `.clang-format`. |
| `install-clang-format.bat`, `install-clang-format.sh` | Download clang-format 23.1.5 without formatting any file. |
| `before-commit.clang-format` | The C, C++ and Java project rules. Not to be changed. |
| `before-commit.jsbeautifyrc` | The JavaScript project rules for js-beautify. Not to be changed. |
| `js-beautify-downloads.txt` | The address and SHA-256 value of the js-beautify download. |
| `.clang-format` | Own rules, which may be changed. |
| `format_codebase.py` | The script that the files above run. It needs Python 3.8 or newer. |
| `clang-format-downloads.txt` | The addresses and SHA-256 values of the clang-format downloads. |

The downloads are the clang-format packages of PyPI (https://pypi.org/project/clang-format/23.1.5/), about 2 MB each,
which hold the clang-format program of LLVM 23.1.5. Packages exist for Windows (x64, x86, ARM64), Linux (x86-64,
ARM64, ARMv7, x86, ppc64le, s390x, with glibc or musl) and macOS (Intel and Apple processors). A package is checked
against its SHA-256 and unpacked into `clang-format-23.1.5/<system>` in this folder together with its license files,
one folder per system, so that a checkout used from Windows and WSL keeps both programs. The folder is not part of
the repository.

Before a commit only clang-format 23.1.5 is used, because other versions can format differently. With own rules,
the `CLANG_FORMAT` environment variable or a clang-format 14 or newer on the PATH can be used instead of the download.

Both clang-format rules files have a second section for Java (`Language: Java`), which sets only the options that
differ from the C and C++ section. clang-format always puts the opening brace of a Java lambda body on the line of its
arrow (`->`) and the opening brace of an anonymous class on the line of `new X()`, writes array literals as
`new int[] {1, 2}` and starts a new line before each `+` that joins two string literals. No option changes this.

Editors that format on save look for `.clang-format` in the folder of the file and the folders above it, which do not
include this folder. Point the clang-format setting of the editor to `tools/.clang-format` to use the own rules there.

The scripts pass these options on to `format_codebase.py`.

| Option | Effect |
|---|---|
| `--check` | lists the files that would change and changes nothing, exit code 1 when any would |
| `--target server`, `--target client`, `--target all` | the part to format with own rules, asked when omitted |
| `--list` | lists the files that would be formatted |
| `--style-file PATH` | other own rules than `.clang-format` |
| `--download-tools`, `--no-download-tools` | download clang-format without asking, or never |
