#!/bin/bash
# Build a native Windows (mingw) stunnel.exe with w64devkit + Perl.
#
# Unlike the Linux _bundle_and_build.sh, this runs from a Unix-ish shell ON
# Windows with the w64devkit mingw toolchain (gcc, make, windres, ar) on PATH.
#
# IMPORTANT: OpenSSL's Configure needs a FULL Perl (ExtUtils::MakeMaker,
# Locale::Maketext::Simple, IPC::Cmd, ...). Git-for-Windows' bundled Perl is too
# minimal and fails at "Configuring OpenSSL". Install Strawberry Perl
# (https://strawberryperl.com) and run this so its perl.exe is first on PATH.
#
#   # e.g. from Git Bash, with Strawberry perl + w64devkit on PATH:
#   W64DEVKIT=/c/path/to/w64devkit/bin bash _bundle_and_build_windows.sh
#
# Uses the VENDORED OpenSSL source (stunnel/openssl-3.5.1.tar.gz) - no download.
# Result: stunnel-5.75/bin/MGW32/stunnel.exe  (statically linked OpenSSL).
# The OpenSSL build under win-build/ is large and can be deleted afterwards.
set -e

# w64devkit toolchain (skip if gcc is already on PATH)
if [ -n "${W64DEVKIT:-}" ]; then
  export PATH="$W64DEVKIT:$PATH"
fi
command -v gcc  >/dev/null || { echo "ERROR: gcc (w64devkit) not on PATH - set W64DEVKIT=<w64devkit>/bin"; exit 1; }
command -v perl >/dev/null || { echo "ERROR: perl not on PATH"; exit 1; }
command -v make >/dev/null || { echo "ERROR: make not on PATH"; exit 1; }

# OpenSSL's Configure needs a FULL Perl - Git-for-Windows' bundled Perl is too minimal.
perl -MExtUtils::MakeMaker -MLocale::Maketext::Simple -e 1 2>/dev/null || {
  echo "ERROR: this Perl lacks modules OpenSSL needs (ExtUtils::MakeMaker, Locale::Maketext::Simple)."
  echo "       Git-for-Windows' Perl is too minimal - install Strawberry Perl (https://strawberryperl.com)"
  echo "       and run this from a shell where its perl.exe is first on PATH."
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
  perl Configure mingw64 no-shared no-tests no-docs no-apps
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
env -u windir make -f mingw.mak \
  SSLDIR="$OSSL" \
  SSLINC="$OSSL/include" \
  SSLLIBS="-L$OSSL -lssl -lcrypto -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -luser32" \
  CFLAGS="-g -O2 -Wall -D_WIN32_WINNT=0x0501 -I$OSSL/include -Wno-error=incompatible-pointer-types"

echo "== RESULT =="
ls -la "$STUNNEL/bin/MGW32/"*.exe 2>&1
file "$STUNNEL/bin/MGW32/stunnel.exe" 2>/dev/null || true
echo DONE
