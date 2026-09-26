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
# Any shim already on LD_PRELOAD is taken OFF it first, and this tree's goes in
# front. The session preloads the installed one (environment.d), and this
# script used to append the build tree's after it: the first definition in the
# global scope wins, so the installed shim took every hook -- and, through
# LD_LIBRARY_PATH, loaded this tree's heavy library. A mixed stack, in the
# script a developer runs to test a change to the hooks
# (tests/dev_run_gl.cmake). Whatever else is preloaded stays, after ours, the
# order the session and the mangohud wrapper produce.
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

# ld.so splits LD_PRELOAD on colons and spaces; so does this. No globbing: an
# entry is a path, `$LIB` included, never a pattern.
others=""
set -f
old_ifs=$IFS
IFS=': '
for entry in ${LD_PRELOAD:-}; do
    case "$entry" in
        *libvocem_gl_shim.so) ;;
        *) others="${others:+$others:}$entry" ;;
    esac
done
IFS=$old_ifs
set +f

LD_LIBRARY_PATH="$root/build/gl${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
LD_PRELOAD="$shim${others:+:$others}" \
VOCEM_DEBUG="${VOCEM_DEBUG:-1}" \
exec "$@"
