#!/bin/sh
# DragonGL+ - static OpenSSL and libsodium for the portable packages
# Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
#
#   tools/build_deps.sh <linux|windows> <prefix>
#
# The server (and the client, for TLS) of the portable packages link
# OpenSSL 3 and libsodium statically, so the archives need nothing from
# the system. This script downloads the pinned releases, checks their
# SHA-256, and installs the static libraries, headers and pkg-config
# files under <prefix> (lib/, include/). It does nothing when <prefix>
# already holds the same versions (CI caches it).
#
#   linux   : native build (the release runs it in the Ubuntu 18.04
#             container of tools/build_release.sh: glibc 2.27)
#   windows : cross build with MinGW-w64 (x86_64-w64-mingw32-*)
set -eu

TARGET=${1:?usage: build_deps.sh <linux|windows> <prefix>}
PREFIX=${2:?usage: build_deps.sh <linux|windows> <prefix>}

OPENSSL_VERSION=3.0.15
OPENSSL_SHA256=23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz
SODIUM_VERSION=1.0.20
SODIUM_SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
SODIUM_URL=https://github.com/jedisct1/libsodium/releases/download/$SODIUM_VERSION-RELEASE/libsodium-$SODIUM_VERSION.tar.gz

STAMP="openssl-$OPENSSL_VERSION libsodium-$SODIUM_VERSION $TARGET"
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
if [ -f "$PREFIX/.deps-stamp" ] && [ "$(cat "$PREFIX/.deps-stamp")" = "$STAMP" ]; then
    echo "build_deps: $STAMP already in $PREFIX"
    exit 0
fi
JOBS=$(nproc 2>/dev/null || echo 2)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fetch() { # url sha256 file: each download in its own directory
    mkdir -p "$WORK/dl"
    curl -fsSL --retry 3 -o "$WORK/dl/$3" "$1"
    echo "$2  $WORK/dl/$3" | sha256sum -c -
}

fetch "$OPENSSL_URL" "$OPENSSL_SHA256" openssl.tar.gz
fetch "$SODIUM_URL" "$SODIUM_SHA256" libsodium.tar.gz
mkdir -p "$WORK/openssl" "$WORK/sodium"
tar -xzf "$WORK/dl/openssl.tar.gz" -C "$WORK/openssl" --strip-components=1
tar -xzf "$WORK/dl/libsodium.tar.gz" -C "$WORK/sodium" --strip-components=1

case "$TARGET" in
linux)
    OSSL_TARGET=linux-x86_64
    OSSL_CROSS=""
    SODIUM_HOST=""
    ;;
windows)
    OSSL_TARGET=mingw64
    OSSL_CROSS=--cross-compile-prefix=x86_64-w64-mingw32-
    SODIUM_HOST=--host=x86_64-w64-mingw32
    ;;
*)
    echo "unknown target: $TARGET" >&2
    exit 1
    ;;
esac

echo "build_deps: OpenSSL $OPENSSL_VERSION ($TARGET)"
(cd "$WORK/openssl" &&
    ./Configure "$OSSL_TARGET" $OSSL_CROSS no-shared no-tests no-module \
        -fPIC --prefix="$PREFIX" --libdir=lib --openssldir="$PREFIX/ssl" >/dev/null &&
    make -j"$JOBS" >/dev/null &&
    make install_sw >/dev/null)

echo "build_deps: libsodium $SODIUM_VERSION ($TARGET)"
(cd "$WORK/sodium" &&
    ./configure $SODIUM_HOST --disable-shared --enable-static --with-pic \
        --prefix="$PREFIX" --libdir="$PREFIX/lib" >/dev/null &&
    make -j"$JOBS" >/dev/null &&
    make install >/dev/null)

echo "$STAMP" >"$PREFIX/.deps-stamp"
echo "build_deps: installed in $PREFIX"
