#!/bin/sh
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Why the overlay is or is not in a running program.
#
#   scripts/vocem-why.sh balatro      # while the game is running
#   scripts/vocem-why.sh              # everything the overlay has ever been in
#
# This exists because the registry cannot answer the question on its own. It lists
# what the overlay was *loaded into*, so a program that never appears there is
# ambiguous in the worst way: it might never have been started, or it might have run
# for an hour without our code ever reaching it. Those two look identical from the
# outside and have completely different causes, and no amount of reasoning from
# another machine can tell them apart.
#
# Everything below is read from /proc and from the registry. Nothing is started,
# nothing is changed, and it can be run by the person having the problem.
#
# In English like everything else the project prints: this script's output ends up
# pasted into bug reports read next to the overlay's own log lines, and a report
# half in one language and half in another is harder on everyone including its
# author. (It started out in Italian, being written mid-chase on an Italian
# desktop.)

set -u

cache="${XDG_CACHE_HOME:-$HOME/.cache}/vocem/apps"

echo "== vocem-why =="
if pacman -Q vocem-overlay 2>/dev/null; then :; else echo "package: not installed via pacman"; fi
echo "session: LD_PRELOAD=${LD_PRELOAD:-(empty)}"
case "${LD_PRELOAD:-}" in
    *vocem*) ;;
    *) echo "  !! this shell has no preload: if the session is the same,"
       echo "     no OpenGL program can have the overlay. Log out and back in." ;;
esac
echo "daemon: $(systemctl --user is-active vocemd.service 2>/dev/null || echo unknown)"
echo

if [ "$#" -eq 0 ]; then
    echo "== what it has seen so far =="
    if [ -d "$cache" ]; then
        for f in "$cache"/*; do
            [ -e "$f" ] || continue
            name=$(sed -n 's/^name = //p' "$f")
            why=$(sed -n 's/^why = //p' "$f")
            api=$(sed -n 's/^api = //p' "$f")
            game=$(sed -n 's/^game = //p' "$f")
            printf '  %-20s %-7s game=%-5s %s\n' "$name" "$api" "$game" "$why"
        done
    else
        echo "  (registry empty: no process has presented a frame with us inside yet)"
    fi
    echo
    echo "For one program in particular, while it is running:  $0 <name>"
    exit 0
fi

target=$1
pids=$(pgrep -f "$target" 2>/dev/null)
if [ -z "$pids" ]; then
    echo "== '$target' is not running =="
    echo "Start it and run this again while it is open: almost everything that"
    echo "matters can only be read from a live process."
    exit 1
fi

for pid in $pids; do
    comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
    echo "== $comm (pid $pid) =="

    # 1. Are we inside it? This is the question the registry cannot answer.
    shim=$(grep -c "libvocem_gl_shim" "/proc/$pid/maps" 2>/dev/null || echo 0)
    lib=$(grep -c "libvocem_gl\.so" "/proc/$pid/maps" 2>/dev/null || echo 0)
    vk=$(grep -c "libvocem_vk" "/proc/$pid/maps" 2>/dev/null || echo 0)
    echo "  shim loaded:       $([ "$shim" -gt 0 ] && echo yes || echo NO)"
    echo "  GL overlay:        $([ "$lib" -gt 0 ] && echo yes || echo no)"
    echo "  Vulkan layer:      $([ "$vk" -gt 0 ] && echo yes || echo no)"

    # 2. What it draws with, which decides which of the two paths should hook it.
    printf '  graphics stack:    '
    awk '{print $NF}' "/proc/$pid/maps" 2>/dev/null | grep '^/' | xargs -n1 basename 2>/dev/null \
        | grep -E '^lib(vulkan|GLX|EGL|GL)\.' | sort -u | tr '\n' ' '
    echo

    # 3. In what environment, which is where the detection lives.
    env_of() { tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n "s/^$1=//p" | head -1; }
    echo "  SteamAppId:        $(env_of SteamAppId)"
    echo "  FLATPAK_ID:        $(env_of FLATPAK_ID)"
    echo "  LD_PRELOAD:        $(env_of LD_PRELOAD)"

    # 4. And what it wrote about itself, if it got that far.
    record=$(grep -l "^name = $comm\$" "$cache"/* 2>/dev/null | head -1)
    if [ -n "$record" ]; then
        echo "  registry:          $(sed -n 's/^why = //p' "$record") (game=$(sed -n 's/^game = //p' "$record"))"
    else
        echo "  registry:          no record"
        if [ "$shim" -gt 0 ] && [ "$lib" -eq 0 ]; then
            echo "    -> the shim is there but never loaded the overlay: this process has"
            echo "       not yet presented a frame through a function we hook."
        fi
        if [ "$shim" -eq 0 ] && [ "$vk" -eq 0 ]; then
            echo "    -> we are not inside it at all: a sandbox (Flatpak/Snap), or the"
            echo "       program started before the preload existed in the session."
        fi
    fi
    echo
done
