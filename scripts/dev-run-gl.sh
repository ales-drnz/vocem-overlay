#!/bin/sh
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Run an OpenGL application against the interposer in the build tree.
#
#   scripts/dev-run-gl.sh glxgears
#   scripts/dev-run-gl.sh minecraft-launcher
#
# This preloads the shim, which is what a packaged install preloads session-wide,
# so what gets exercised here is the real path rather than a shortcut. The shim
# dlopens libvocem_gl.so, hence LD_LIBRARY_PATH pointing at the build tree.
#
# Launchers can be passed directly: they are in hidden_apps, so the panel is not
# drawn on the launcher itself, and the game it starts inherits the environment.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
shim="$root/build/gl/libvocem_gl_shim.so"

if [ ! -f "$shim" ]; then
    echo "error: $shim not found. Build first:" >&2
    echo "  cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <application> [args...]" >&2
    exit 2
fi

LD_LIBRARY_PATH="$root/build/gl${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$shim" \
VOCEM_DEBUG="${VOCEM_DEBUG:-1}" \
exec "$@"
