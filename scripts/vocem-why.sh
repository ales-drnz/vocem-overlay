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
# `systemctl is-active` reports a STATE, so its exit status *is* that state
# rather than an error: "inactive" comes back with exit 3, and a unit systemd
# has never heard of with exit 4, both having printed the word on stdout. So
# `|| echo unknown` ran the fallback as well as the command and put two lines
# where one belongs -- and it did it precisely when the daemon is down, which is
# the situation somebody runs this tool in. Entry 99 is the same mistake with
# `grep -c`, five lines further down, found and fixed while this one stood.
# Take the output; fall back only when there is none.
daemon_state=$(systemctl --user is-active vocemd.service 2>/dev/null)
[ -n "$daemon_state" ] || daemon_state=unknown
echo "daemon: $daemon_state"
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
# The exact process name first, and only then the whole command line -- because
# `pgrep -f` matches this script's own shell, whose command line contains the name
# that was just typed. Run against `plasmashell` it answered about the shell as
# well, with "shim loaded: yes" and no record, which is the shape of the report
# somebody would paste into a bug. Our own two pids and anything running this
# script are dropped for the same reason.
pids=$(pgrep -x "$target" 2>/dev/null)
if [ -z "$pids" ]; then
    pids=$(pgrep -f "$target" 2>/dev/null | while read -r candidate; do
        [ "$candidate" = "$$" ] && continue
        [ "$candidate" = "$PPID" ] && continue
        tr '\0' ' ' < "/proc/$candidate/cmdline" 2>/dev/null | grep -q 'vocem-why' && continue
        echo "$candidate"
    done)
fi
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
    #
    # `grep -c` prints 0 and exits 1 when it finds nothing, so `|| echo 0` put a
    # second line in the variable and every comparison below then said
    # "[: 0\n0: integer expected" -- in the middle of the answer, on almost every
    # process, since most of them have no Vulkan layer in them.
    count_in_maps() {
        found=$(grep -c "$1" "/proc/$pid/maps" 2>/dev/null | head -1)
        [ -n "$found" ] || found=0
        echo "$found"
    }
    shim=$(count_in_maps "libvocem_gl_shim")
    lib=$(count_in_maps "libvocem_gl\.so")
    vk=$(count_in_maps "libvocem_vk")
    # `A && echo x || echo y` is the same shape as the fault above and is sound
    # here, which is worth saying rather than leaving to be re-derived: the first
    # branch is a `[` test, so it either succeeds and `echo x` runs alone, or it
    # fails and only `echo y` does. The fault needs a first branch that prints
    # AND fails, which is what `systemctl is-active` and `grep -c` both do.
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
    # The record's file name is the process name with everything outside
    # [A-Za-z0-9._-] turned into an underscore (vocem/apps.h), so it can be
    # named rather than searched for -- a name like `Risk_of_Rain_2.` is a
    # regular expression in which every dot matches the next record along.
    record="$cache/$(printf '%s' "$comm" | tr -c 'A-Za-z0-9._-' '_')"
    [ -f "$record" ] || record=$(grep -l "^name = $comm\$" "$cache"/* 2>/dev/null | head -1)
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
