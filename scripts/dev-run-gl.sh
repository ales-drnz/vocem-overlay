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
# front. The session preloads the installed one (environment.d), and the first
# definition in the global scope wins: left there, the installed shim would take
# every hook and load this tree's heavy library, a mixed stack (entry 242).
# Whatever else is preloaded stays, after ours, in the order the session and
# the mangohud wrapper produce.
#
# Ours is preloaded as the session's is, through ld.so's literal $LIB: a
# directory in the build tree holds `lib` (this tree's build/gl) and `lib32`
# (build32/gl when it has a shim, the installed /usr/lib32 otherwise), and
# LD_PRELOAD names <that directory>/$LIB/libvocem_gl_shim.so, so a 32-bit child
# -- a 32-bit game, or one a 64-bit launcher starts -- gets a 32-bit shim
# (entry 265). The heavy libraries are found the same way: LD_LIBRARY_PATH
# carries both trees, and ld.so passes over the one of the other width.
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

preload="$root/build/dev-gl-preload"
mkdir -p "$preload"
ln -sfn "$root/build/gl" "$preload/lib"
if [ -f "$root/build32/gl/libvocem_gl_shim.so" ]; then
    ln -sfn "$root/build32/gl" "$preload/lib32"
else
    ln -sfn /usr/lib32 "$preload/lib32"
fi

# Single quotes: `$LIB` is ld.so's to expand, per process, not the shell's.
LD_LIBRARY_PATH="$root/build/gl:$root/build32/gl${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
LD_PRELOAD="$preload/"'$LIB'"/libvocem_gl_shim.so${others:+:$others}" \
VOCEM_DEBUG="${VOCEM_DEBUG:-1}" \
exec "$@"
