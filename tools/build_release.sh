#!/bin/sh
# DRAGON GL - build the portable Linux and Windows packages
# Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
#
#   tools/build_release.sh [linux] [windows]      (default: both)
#
# linux   : built inside an Ubuntu 18.04 container (glibc 2.27) when
#           Docker is available, so the binary runs on practically every
#           distribution of the last years; otherwise with the host
#           compiler (then it needs a glibc at least as new as the host).
#           Needs: docker, or gcc + cmake + X11 development headers.
# windows : cross-compiled with MinGW-w64 (static, no DLL to ship).
#           Needs: x86_64-w64-mingw32-gcc, cmake, ninja or make.
#
# Archives are written to dist/.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
TARGETS=${*:-linux windows}
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"

for t in $TARGETS; do
case "$t" in
linux)
    if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
        echo "== Linux (Ubuntu 18.04 container)"
        docker run --rm -v "$ROOT":/src:ro -v "$ROOT/build-linux":/out \
            -e DEBIAN_FRONTEND=noninteractive ubuntu:18.04 sh -c '
            set -e
            apt-get update -qq
            apt-get install -y -qq gcc make python3-pip libx11-dev libxrandr-dev \
                libxinerama-dev libxcursor-dev libxi-dev libxext-dev >/dev/null
            python3 -m pip install -q -U "pip<22" >/dev/null 2>&1
            python3 -m pip install -q "cmake<3.28" >/dev/null 2>&1
            cmake -S /src -B /out -DCMAKE_BUILD_TYPE=Release -DDRAGONGL_PORTABLE=ON
            cmake --build /out -j"$(nproc)"'
    else
        echo "== Linux (host compiler)"
        cmake -S . -B build-linux $GEN -DCMAKE_BUILD_TYPE=Release -DDRAGONGL_PORTABLE=ON
        cmake --build build-linux -j
    fi
    tools/package.sh build-linux linux dist
    ;;
windows)
    echo "== Windows (MinGW-w64 cross build)"
    cmake -S . -B build-windows $GEN -DCMAKE_BUILD_TYPE=Release -DDRAGONGL_PORTABLE=ON \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
    cmake --build build-windows -j
    tools/package.sh build-windows windows dist
    ;;
*) echo "unknown target: $t (linux|windows)" >&2; exit 1 ;;
esac
done
