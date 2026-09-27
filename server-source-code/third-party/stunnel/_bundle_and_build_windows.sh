#!/bin/bash
# Build native Windows (mingw) stunnel.exe and tstunnel.exe with w64devkit, from Git Bash.
#
# Unlike the Linux _bundle_and_build.sh, this runs from a Unix-ish shell ON
# Windows with the w64devkit mingw toolchain (gcc, make, windres, ar) on PATH.
#
# Perl: OpenSSL's Configure for a mingw target refuses a native Windows Perl
# (Strawberry, ActiveState), because it produces C:\ style paths. It needs the
# MSYS Perl that Git for Windows ships, which lacks a few pure-Perl modules
# (ExtUtils::MakeMaker, IPC::Cmd, Locale::Maketext::Simple, Pod::Usage, ...).
# Those modules are vendored in perl-extra-lib/ next to this script, so nothing
# has to be installed: Git for Windows plus w64devkit is enough.
#
#   # from Git Bash:
#   W64DEVKIT=/c/path/to/w64devkit/bin bash _bundle_and_build_windows.sh
#
# Uses the VENDORED OpenSSL source (stunnel/openssl-3.5.1.tar.gz) - no download.
# OpenSSL is built without assembly (no-asm): its assembly generators need a
# shell pipeline that breaks under the MSYS Perl, and stunnel does not need them.
# Result: stunnel-5.75/bin/MGW32/stunnel.exe (gui) and tstunnel.exe (console),
# both with OpenSSL linked statically. The OpenSSL build under win-build/ is
# large and can be deleted afterwards.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"

# w64devkit toolchain (skip if gcc is already on PATH)
if [ -n "${W64DEVKIT:-}" ]; then
  export PATH="$W64DEVKIT:$PATH"
fi
# the MSYS perl first, ahead of any native Windows perl that may sit on PATH
if [ -x /usr/bin/perl ]; then
  export PATH="/usr/bin:$PATH"
fi
command -v gcc  >/dev/null || { echo "ERROR: gcc (w64devkit) not on PATH - set W64DEVKIT=<w64devkit>/bin"; exit 1; }
command -v perl >/dev/null || { echo "ERROR: perl not on PATH (Git for Windows provides it in Git Bash)"; exit 1; }
command -v make >/dev/null || { echo "ERROR: make not on PATH"; exit 1; }

# the vendored modules; MSYS2_ENV_CONV_EXCL keeps make from turning PERL5LIB into a C:\ path,
# which the MSYS perl would then split at the drive colon
export PERL5LIB="$HERE/perl-extra-lib${PERL5LIB:+:$PERL5LIB}"
export MSYS2_ENV_CONV_EXCL="PERL5LIB${MSYS2_ENV_CONV_EXCL:+;$MSYS2_ENV_CONV_EXCL}"

perl -MFile::Spec::Functions=rel2abs -e 'exit(rel2abs(".") =~ m|^/| ? 0 : 1)' 2>/dev/null || {
  echo "ERROR: this perl produces Windows paths; OpenSSL's mingw build needs the MSYS perl from Git for Windows."
  echo "       run this from Git Bash, where /usr/bin/perl exists."
  exit 1
}
perl -MExtUtils::MakeMaker -MIPC::Cmd -MLocale::Maketext::Simple -MPod::Usage -e 1 2>/dev/null || {
  echo "ERROR: perl cannot load the modules OpenSSL needs even with $HERE/perl-extra-lib; is that directory intact?"
  exit 1
}

HERE="$(cd "$(dirname "$0")" && pwd)"
STUNNEL="$HERE/stunnel-5.75"
OSSL_VER="3.5.1"
BUILD="$HERE/win-build"
OSSL="$BUILD/openssl-$OSSL_VER"

echo "== toolchain =="
echo "  gcc     $(gcc -dumpversion)"
echo "  perl    $(perl -e 'print $^V')"
echo "  make    $(make --version | head -1)"
echo "  windres $(command -v windres || echo MISSING)"

# ---- 1. OpenSSL (static, mingw64) from the VENDORED source ----
if [ ! -f "$OSSL/libcrypto.a" ]; then
  mkdir -p "$BUILD"
  if [ ! -d "$OSSL" ]; then
    [ -f "$HERE/openssl-$OSSL_VER.tar.gz" ] || { echo "ERROR: vendored openssl-$OSSL_VER.tar.gz is missing"; exit 1; }
    echo "== extracting vendored OpenSSL $OSSL_VER source =="
    tar xzf "$HERE/openssl-$OSSL_VER.tar.gz" -C "$BUILD"
  fi
  cd "$OSSL"
  echo "== configuring OpenSSL (mingw64, static) =="
  perl Configure mingw64 no-shared no-tests no-docs no-apps no-asm
  echo "== building OpenSSL (slow part) =="
  make -j"$(nproc 2>/dev/null || echo 4)"
else
  echo "== OpenSSL already built, skipping =="
fi

# ---- 2. stunnel via its mingw makefile, linked against the static OpenSSL ----
echo "== building stunnel.exe =="
cd "$STUNNEL/src"
# 'env -u windir' forces mingw.mak's portable branch (mkdir/rm/cp instead of GnuWin32);
# -Wno-error=incompatible-pointer-types keeps the bundled WT patch building on gcc 15.
# stunnel includes openssl/applink.c, which only "make install" would place under include/; the build tree keeps it in ms/
cp -f "$OSSL/ms/applink.c" "$OSSL/include/openssl/applink.c"
# the native mingw gcc does not understand /d/... paths: hand it D:/... (cygpath is part of Git for Windows)
OSSL_WIN="$(cygpath -m "$OSSL" 2>/dev/null || echo "$OSSL")"
env -u windir make -f mingw.mak \
  SSLDIR="$OSSL_WIN" \
  SSLINC="$OSSL_WIN/include" \
  SSLLIBS="-L$OSSL_WIN -lssl -lcrypto -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -luser32" \
  CFLAGS="-g -O2 -Wall -D_WIN32_WINNT=0x0501 -I$OSSL_WIN/include -Wno-error=incompatible-pointer-types"

echo "== RESULT =="
ls -la "$STUNNEL/bin/MGW32/"*.exe 2>&1
file "$STUNNEL/bin/MGW32/stunnel.exe" 2>/dev/null || true
echo DONE
