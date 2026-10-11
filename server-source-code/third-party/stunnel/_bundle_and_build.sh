#!/bin/bash
# Build the bundled OpenSSL + the patched stunnel, fully offline.
#
# - Extracts the VENDORED OpenSSL source (stunnel/openssl-3.5.1.tar.gz), builds it
#   STATIC (no-shared, -fPIC) and installs just the libs+headers into stunnel/openssl/.
# - Then configures stunnel --with-ssl=stunnel/openssl and builds it, so the
#   resulting binary carries its own crypto (no system libssl needed).
#
# No network needed: the OpenSSL source is vendored, nothing is downloaded.
# Re-runnable: if stunnel/openssl/ already has the libs, the OpenSSL step is
# skipped. The heavy OpenSSL compile happens on the fast filesystem (/tmp).
#
#   NJOBS=2 bash _bundle_and_build.sh
set -euo pipefail

# =============================================================================
# SETTINGS | Vendored source, install prefix, and temporary build directory
# =============================================================================

HERE="$(cd "$(dirname "$0")" && pwd)"
STUNNEL_DIR="$HERE/stunnel-5.75"
SSL_PREFIX="$HERE/openssl"          # the bundle (include/ + lib/) kept in the dir
BUILD_ROOT="/tmp/openssl-src"       # fast native-fs build area
NJOBS="${NJOBS:-2}"
step()
{
    echo
    echo "==================== $* ===================="
}

# =============================================================================
# BUILD 1/2 | OpenSSL static libraries and headers
# =============================================================================

OSSL_VER="3.5.1"
if [ ! -f "$SSL_PREFIX/lib/libssl.a" ] && [ ! -f "$SSL_PREFIX/lib64/libssl.a" ]
then
    if [ ! -f "$HERE/openssl-$OSSL_VER.tar.gz" ]
    then
        echo "ERROR: vendored $HERE/openssl-$OSSL_VER.tar.gz is missing"
        exit 1
    fi
    step "extracting vendored OpenSSL $OSSL_VER source"
    mkdir -p "$BUILD_ROOT"
    rm -rf "$BUILD_ROOT/openssl-$OSSL_VER"
    tar -xzf "$HERE/openssl-$OSSL_VER.tar.gz" -C "$BUILD_ROOT"
    step "building OpenSSL $OSSL_VER (static, -fPIC)"
    cd "$BUILD_ROOT/openssl-$OSSL_VER"
    ./config \
        no-shared \
        no-tests \
        no-docs \
        -fPIC \
        --prefix="$SSL_PREFIX" \
        --openssldir="$SSL_PREFIX/ssl"
    make -j"$NJOBS"
    make install_sw
fi

# openssl may install to lib/ or lib64/; make sure a lib/ exists for stunnel
if [ -d "$SSL_PREFIX/lib64" ] && [ ! -e "$SSL_PREFIX/lib" ]
then
    ln -s lib64 "$SSL_PREFIX/lib"
fi

step "bundled OpenSSL"
ls -la "$SSL_PREFIX"/lib*/libssl.a "$SSL_PREFIX"/lib*/libcrypto.a 2>/dev/null || true

# =============================================================================
# BUILD 2/2 | Patched stunnel using the bundled OpenSSL
# =============================================================================

step "configuring + building stunnel against bundled OpenSSL"
cd "$STUNNEL_DIR"
make distclean >/dev/null 2>&1 || true

# gcc-14+ turns -Wincompatible-pointer-types into a default ERROR; the bundled
# WT X-Forwarded patch passes &c->ssl_ptr (size_t*) where buffer_insert wants
# int* — harmless for handshake-sized buffers and a warning on older gcc, so
# keep it a warning here too rather than patch upstream's buffer_insert.
# Restore the execute bit, which may be missing after a checkout on Windows.
chmod +x ./configure
./configure \
    --with-ssl="$SSL_PREFIX" \
    CFLAGS="-g -O2 -Wno-error=incompatible-pointer-types"

# Keep the committed configure files. Fresh checkout timestamps can otherwise
# trigger unavailable autotools versions; the colon makes regeneration a no-op.
make -j"$NJOBS" \
    ACLOCAL=: \
    AUTOCONF=: \
    AUTOHEADER=: \
    AUTOMAKE=:

# =============================================================================
# RESULT | Executable and dynamic-library diagnostics
# =============================================================================

step "RESULT"
ls -la "$STUNNEL_DIR/src/stunnel" 2>/dev/null || true
file "$STUNNEL_DIR/src/stunnel" 2>/dev/null || true
echo "--- runtime ssl/crypto deps (want: none = self-contained) ---"
ldd "$STUNNEL_DIR/src/stunnel" 2>/dev/null | grep -iE 'libssl|libcrypto' && echo "WARN: links system ssl/crypto" || echo "OK: no dynamic libssl/libcrypto"
echo "DONE"
