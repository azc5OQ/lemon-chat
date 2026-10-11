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

# =============================================================================
# SETUP | Script location and required shell
# =============================================================================

HERE="$(cd "$(dirname "$0")" && pwd)"

# A bare "bash" from cmd may be w64devkit's shell or WSL, not Git Bash.
if [ ! -x /usr/bin/perl ] || [ ! -x /usr/bin/cygpath ]
then
    echo "ERROR: this build needs Git for Windows' Bash and MSYS Perl."
    echo "       From cmd, run _bundle_and_build_windows.bat to select the correct shell."
    exit 1
fi

# =============================================================================
# TOOLCHAIN | MinGW compiler and Git's MSYS Perl
# =============================================================================

# W64DEVKIT may be a Windows path from the .bat launcher or a Git Bash path.
if [ -n "${W64DEVKIT:-}" ]
then
    W64DEVKIT="$(/usr/bin/cygpath -u "$W64DEVKIT")"
    export PATH="$W64DEVKIT:$PATH"
fi

# Select MSYS Perl before any native Windows Perl inherited through PATH.
export PATH="/usr/bin:$PATH"
for required_tool in gcc perl make windres
do
    if ! command -v "$required_tool" >/dev/null
    then
        echo "ERROR: $required_tool is not on PATH. Set W64DEVKIT to the w64devkit bin directory."
        exit 1
    fi
done

if ! perl -MFile::Spec::Functions=rel2abs -e 'exit(rel2abs(".") =~ m|^/| ? 0 : 1)' 2>/dev/null
then
    echo "ERROR: Perl produces Windows paths; OpenSSL's mingw build needs Git's MSYS Perl."
    exit 1
fi

# the vendored modules; MSYS2_ENV_CONV_EXCL keeps make from turning PERL5LIB into a C:\ path,
# which the MSYS perl would then split at the drive colon
export PERL5LIB="$HERE/perl-extra-lib${PERL5LIB:+:$PERL5LIB}"
export MSYS2_ENV_CONV_EXCL="PERL5LIB${MSYS2_ENV_CONV_EXCL:+;$MSYS2_ENV_CONV_EXCL}"
if ! perl \
    -MExtUtils::MakeMaker \
    -MIPC::Cmd \
    -MLocale::Maketext::Simple \
    -MPod::Usage \
    -e 1 2>/dev/null
then
    echo "ERROR: Perl cannot load OpenSSL's modules from $HERE/perl-extra-lib."
    exit 1
fi

# =============================================================================
# SETTINGS | Vendored sources and build output
# =============================================================================

STUNNEL="$HERE/stunnel-5.75"
OSSL_VER="3.5.1"
BUILD="$HERE/win-build"
OSSL="$BUILD/openssl-$OSSL_VER"
NJOBS="${NJOBS:-${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}}"
echo "== toolchain =="
echo "  gcc     $(gcc -dumpversion)"
echo "  perl    $(perl -e 'print $^V')"
echo "  make    $(make --version | head -1)"
echo "  windres $(command -v windres)"
echo "  jobs    $NJOBS"

# Validate the same tools and Perl modules used by the build without compiling.
if [ "${1:-}" = "--check-tools" ]
then
    exit 0
fi

# =============================================================================
# BUILD 1/2 | OpenSSL, statically linked for native Windows
# =============================================================================

if [ ! -f "$OSSL/libcrypto.a" ] || [ ! -f "$OSSL/libssl.a" ]
then
    mkdir -p "$BUILD"
    if [ ! -d "$OSSL" ]
    then
        if [ ! -f "$HERE/openssl-$OSSL_VER.tar.gz" ]
        then
            echo "ERROR: vendored openssl-$OSSL_VER.tar.gz is missing"
            exit 1
        fi
        echo "== extracting vendored OpenSSL $OSSL_VER source =="
        tar xzf "$HERE/openssl-$OSSL_VER.tar.gz" -C "$BUILD"
    fi
    cd "$OSSL"
    # An interrupted build may already have hundreds of compiled files. Keep
    # its Makefile and configuration so make can continue with the missing work.
    # Running Configure again would refresh files that those objects depend on.
    if [ ! -f Makefile ] || [ ! -f configdata.pm ]
    then
        echo "== configuring OpenSSL (mingw64, static) =="
        perl Configure mingw64 \
            no-shared \
            no-tests \
            no-docs \
            no-apps \
            no-asm
    else
        echo "== keeping the existing OpenSSL configuration =="
    fi
    echo "== building missing or changed OpenSSL files =="
    make -j"$NJOBS"
else
    echo "== OpenSSL already built, skipping =="
fi

# =============================================================================
# BUILD 2/2 | stunnel GUI and console executables
# =============================================================================

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

# =============================================================================
# RESULT | Built files; the parent script copies these into buildresult
# =============================================================================

echo "== RESULT =="
ls -la "$STUNNEL/bin/MGW32/"*.exe 2>&1
file "$STUNNEL/bin/MGW32/stunnel.exe" 2>/dev/null || true
echo DONE
