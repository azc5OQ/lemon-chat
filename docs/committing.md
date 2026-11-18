# Committing

Every commit keeps the formatting of the project. Changes that only come from another style, another editor or
another version of a formatting tool do not belong in a commit.

## Before every commit

1. Run the format before commit from the repository root.

   | System | Command |
   |---|---|
   | Windows | `tools\format-before-commit.bat` |
   | Linux and macOS | `sh tools/format-before-commit.sh` |

2. Add the files that it changed again with `git add`. Git commits the version that was added, not the version on
   the disk.
3. Commit.

The script formats every C, C++, Java and JavaScript file that differs from the last commit or is new. It does not format
files in folders named `third-party`, `vendor` or `legacy-reference`, the Node.js headers in `nodejs-mobile-include`,
build output, links, empty files and the files that Git ignores. It needs Python 3.8 or newer and Git.

`--check` lists the files that would change and changes nothing. It ends with exit code 1 when a file would change,
so it can serve as a Git pre-commit hook, for example as this `.git/hooks/pre-commit` file.

```sh
#!/bin/sh
exec sh tools/format-before-commit.sh --check
```

## Rules

| Files | Rules | Tool |
|---|---|---|
| `.c`, `.h`, `.cpp`, `.hpp` | `tools/before-commit.clang-format` | clang-format 23.1.5 |
| `.java` | the Java section of `tools/before-commit.clang-format` | clang-format 23.1.5 |
| `.js`, `.cjs`, `.mjs` | `tools/before-commit.jsbeautifyrc` | js-beautify 2.0.3 |

The two rules files hold the project style and are not to be changed. The script reads them as they are in the last
commit, so a local change of these files has no effect. It uses only these tool versions, so the result is the same
on Windows, Linux and macOS. When a tool is missing, the script offers to download it into the `tools` folder. The
downloads are packages of PyPI, checked against the SHA-256 values in `tools/clang-format-downloads.txt` and
`tools/js-beautify-downloads.txt`.

The C and C++ rules are listed in the [project README](../README.md#code-formatting). Java files use the same rules with
two differences, which the second section of `tools/before-commit.clang-format` sets.

- Array literals have no spaces inside their braces, for example `new int[] {1, 2, 3}`.
- Annotations such as `@Override` stay on their own line before methods and fields.

clang-format always puts the opening brace of a Java lambda body on the line of its arrow (`->`) and the opening brace
of an anonymous class on the line of `new X()`, writes a space before the brace of an array literal and starts a new
line before each `+` that joins two string literals. No option changes this. As long as the rules file in the last
commit has no Java section, the script leaves the Java files unchanged.

These are the JavaScript rules.

- Indentation with 4 spaces, no tabs.
- Every opening brace of a block on its own line, also after function expressions such as `function(resolve)` and
  after `try`, `catch` and `else`. The opening brace of an object literal that spans several lines also goes on its
  own line. Braces written on one line, such as `{ type: "x" }`, stay on one line.
- No space between `function` and its parenthesis. A space between `if`, `for`, `while` or `switch` and the
  parenthesis.
- No line length limit, so lines are not wrapped. Line breaks inside statements stay where they are written.
- At most two empty lines in a row.
- The file ends with a line break. The line endings (LF or CRLF) and a byte order mark stay as they are.

## Own style while working

Programmers may format the code in their own style while they work. For C, C++ and Java, change `tools/.clang-format`
and run `tools\format-codebase.bat` or `sh tools/format-codebase.sh`, which ask which part of the repository to format.
The format before commit then restores the project style of the changed files. `format-codebase` formats C, C++ and
Java only.

Editors that format on save look for `.clang-format` in the folder of a file and in the folders above it, which do
not include `tools`. Point the clang-format setting of the editor to `tools/.clang-format` to use the own rules there.
