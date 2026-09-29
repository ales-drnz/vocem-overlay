#!/bin/sh
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Why the overlay is or is not in a running program.
#
#   scripts/vocem-why.sh balatro      # while the game is running
#   scripts/vocem-why.sh              # everything the overlay has ever been in
#   scripts/vocem-why.sh --system     # after a system update: what it took away
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
# pasted into bug reports read next to the overlay's own log lines.

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
# has never heard of with exit 4, both having printed the word on stdout, so
# `|| echo unknown` would print two lines. Take the output; fall back only when
# there is none (entry 116).
daemon_state=$(systemctl --user is-active vocemd.service 2>/dev/null)
[ -n "$daemon_state" ] || daemon_state=unknown
echo "daemon: $daemon_state"

# What the user decided, which the overlay obeys before anything measured
# below: the master switch here, the two lists per process further down. Read
# the way vocem/config.h reads it: a key is the text before '=' on a line that does not
# open with '#', '[' or ';', both sides trimmed, surrounding quotes dropped, and
# the LAST occurrence wins.
config="${XDG_CONFIG_HOME:-$HOME/.config}/vocem/config.ini"
config_value() {
    [ -r "$config" ] || return 0
    awk -v wanted="$1" '
        /^[#;[]/ { next }
        {
            at = index($0, "=")
            if (!at) next
            key = substr($0, 1, at - 1); value = substr($0, at + 1)
            gsub(/^[ \t]+|[ \t]+$/, "", key); gsub(/^[ \t]+|[ \t\r]+$/, "", value)
            if (length(value) >= 2 && value ~ /^".*"$/) value = substr(value, 2, length(value) - 2)
            n = split(wanted, names, " ")
            for (i = 1; i <= n; i++) if (key == names[i]) { last = value; found = 1 }
        }
        END { if (found) print last; exit !found }' "$config"
}
# true/yes/on/1 in any case is on; a missing key is the default, on; anything
# else is off (config.h, as_bool) -- an empty value included, which is why
# config_value's status, not an empty answer, says whether the line is there.
switch_state() {
    case $(printf '%s' "$1" | tr 'A-Z' 'a-z') in
        true|yes|on|1) echo on ;;
        *) echo off ;;
    esac
}
switched_off=
enabled=
if [ -r "$config" ]; then
    echo "settings: $config"
    if ! enabled=$(config_value enabled); then
        echo "  overlay switch:    on (no 'enabled' line: the default)"
    elif [ "$(switch_state "$enabled")" = on ]; then
        echo "  overlay switch:    on (enabled = $enabled)"
    else
        switched_off=yes
        echo "  overlay switch:    OFF (enabled = $enabled): nothing is drawn in any program"
    fi
else
    echo "settings: $config is not there, so every setting is its default"
fi
hidden_list=$(config_value "hidden_apps gl_blacklist")
shown_list=$(config_value shown_apps)
echo

# Whether a comma-separated list names this process -- by its process name or
# its executable's, as vocem/apps.h matches -- printing the entry that did.
list_names() {
    for candidate in "$2" "$3"; do
        [ -n "$candidate" ] || continue
        if printf '%s\n' "$1" | tr ',' '\n' | sed 's/^[ \t]*//; s/[ \t]*$//' \
            | grep -Fxq -- "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

# --system: what an update of the system can take away without a word. Every
# check here is a read; each "!!" line is something that stops the overlay, or
# the window, and says what to do. Exit status 1 when there is one.
#
# The shapes it looks for were each measured on 2026-09-29, and each one is
# silent from inside a game: a system library the dynamic linker no longer
# satisfies (a Qt minor dropping a private symbol the window needs, a glibc
# older than the libraries were built against), a layer manifest the Vulkan
# loader skips, an NVIDIA driver updated without a reboot (every GL and Vulkan
# program fails, this overlay included), and a Flatpak runtime whose Vulkan
# layer extension point has no branch of the extension installed -- that
# runtime's games get no layer, and nothing is there to say so.
if [ "${1:-}" = "--system" ]; then
    problems=0
    problem() {
        echo "  !! $*"
        problems=$((problems + 1))
    }

    echo "== the session's preload =="
    # The manager's environment is what a program started from the desktop
    # inherits after the next login; this shell's is what it inherited at the
    # last one. Each entry is expanded for both widths, as ld.so does with $LIB.
    manager=$(systemctl --user show-environment 2>/dev/null | sed -n 's/^LD_PRELOAD=//p' | head -1)
    manager=$(printf '%s' "$manager" | sed "s/^\\\$'//; s/'\$//")
    echo "  manager: ${manager:-(empty)}"
    case "$manager" in
        *libvocem_gl_shim*) ;;
        *) problem "the session manager preloads no shim: OpenGL games started after the next login get no overlay" ;;
    esac
    for entry in $(printf '%s' "$manager" | tr ': ' '\n\n'); do
        case "$entry" in *libvocem_gl_shim*) ;; *) continue ;; esac
        for lib in lib lib32; do
            path=$(printf '%s' "$entry" | sed "s/\\\$LIB/$lib/g; s/\\\${LIB}/$lib/g")
            if [ -f "$path" ]; then
                echo "  ok  $path"
            else
                problem "$path is not there: ld.so prints 'cannot be preloaded' into every $lib program"
            fi
        done
    done
    echo

    echo "== the installed files against the installed system =="
    # ldd -r resolves every versioned symbol and every function the way ld.so
    # will: a line saying "undefined symbol" or "version ... not found" is a
    # program or library that will not start, or will not load into a game.
    if pacman -Qq vocem-overlay >/dev/null 2>&1; then
        files=$(pacman -Qlq vocem-overlay 2>/dev/null)
    else
        prefix=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
        files=$(ls "$prefix"/bin/vocem* "$prefix"/lib/libvocem_* "$prefix"/lib32/libvocem_* 2>/dev/null)
    fi
    if ! command -v ldd >/dev/null 2>&1; then
        echo "  (ldd is not installed: not checked)"
    else
        checked=0
        for file in $files; do
            [ -f "$file" ] || continue
            case "$file" in
                */bin/vocem|*/bin/vocemd|*/bin/vocem-config|*/libvocem_*.so) ;;
                *) continue ;;
            esac
            checked=$((checked + 1))
            missing=$(LC_ALL=C ldd -r "$file" 2>&1 | grep -E 'undefined symbol|not found' | head -3)
            if [ -n "$missing" ]; then
                problem "$file no longer links against this system:"
                printf '%s\n' "$missing" | sed 's/^[[:space:]]*/       /'
                case "$file" in
                    */vocem-config) echo "     -> rebuild and reinstall the package: the window was built against another Qt" ;;
                    *) echo "     -> rebuild and reinstall the package" ;;
                esac
            else
                echo "  ok  $file"
            fi
        done
        [ "$checked" -gt 0 ] || problem "no installed file of this project was found to check"
    fi
    echo

    echo "== the Vulkan layer =="
    # The loader skips an implicit layer whose manifest has no
    # disable_environment (Vulkan-Loader 1.4.357), and says so only under
    # VK_LOADER_DEBUG.
    found_manifest=
    for manifest in /usr/share/vulkan/implicit_layer.d/VkLayer_vocem_overlay*.json \
                    /etc/vulkan/implicit_layer.d/VkLayer_vocem_overlay*.json \
                    "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/VkLayer_vocem_overlay*.json; do
        [ -f "$manifest" ] || continue
        found_manifest=yes
        if grep -q '"disable_environment"' "$manifest"; then
            echo "  ok  $manifest"
        else
            problem "$manifest has no disable_environment: the Vulkan loader skips the layer in every game"
        fi
    done
    [ -n "$found_manifest" ] || problem "no layer manifest installed: Vulkan games get no overlay"
    echo

    echo "== the graphics driver =="
    # NVIDIA's kernel module and its userspace are one version or no GL and no
    # Vulkan program starts at all. A driver package updated without a reboot
    # is exactly that, and looks like every game being broken.
    if [ -r /proc/driver/nvidia/version ]; then
        # "NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  615.71.09 ...":
        # the first dotted number, since the architecture has digits too.
        module=$(head -n 1 /proc/driver/nvidia/version | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)
        # The newest installed, by version: a leftover older file sorts first
        # by name and would read as a driver waiting for a reboot.
        userspace=$(ls /usr/lib/libGLX_nvidia.so.[0-9]*.[0-9]* 2>/dev/null | sed 's/.*libGLX_nvidia\.so\.//' | sort -V | tail -1)
        if [ -z "$module" ] || [ -z "$userspace" ]; then
            echo "  NVIDIA: module '${module:-?}', userspace '${userspace:-?}' (could not compare)"
        elif [ "$module" = "$userspace" ]; then
            echo "  ok  NVIDIA $module, module and userspace agree"
        else
            problem "NVIDIA module $module, userspace $userspace: the driver was updated and the machine not restarted -- no GL or Vulkan program works until it is, this overlay included"
        fi
    else
        echo "  (no NVIDIA kernel module: nothing to compare)"
    fi
    echo

    echo "== Flatpak games =="
    # A runtime mounts the extension at the version of the extension point it
    # declares; a branch that is not installed is not mounted, and the games on
    # that runtime simply have no layer.
    if command -v flatpak >/dev/null 2>&1; then
        refs=$(flatpak list --runtime --columns=ref 2>/dev/null | sort -u)
        installed=$(printf '%s\n' "$refs" | sed -n 's|^org\.freedesktop\.Platform\.VulkanLayer\.VocemOverlay/[^/]*/||p' | sort -u)
        for ref in $(printf '%s\n' "$refs" | grep -E '^org\.[a-z]+\.Platform/'); do
            point=$(flatpak info -m "$ref" 2>/dev/null \
                | awk '/^\[Extension org\.freedesktop\.Platform\.VulkanLayer\]/ { inside = 1; next }
                       /^\[/ { inside = 0 }
                       inside && /^version *=/ { sub(/^version *= */, ""); print; exit }')
            [ -n "$point" ] || continue
            if printf '%s\n' "$installed" | grep -Fxq "$point"; then
                echo "  ok  $ref: the $point extension is installed"
            elif [ -z "$installed" ]; then
                echo "  $ref mounts layers at $point; the extension is not installed at all (README 1.4)"
            else
                problem "$ref mounts layers at $point and the extension's $point branch is not installed: its Vulkan games get no overlay -- flatpak install --user vocem-overlay org.freedesktop.Platform.VulkanLayer.VocemOverlay//$point"
            fi
        done
    else
        echo "  (flatpak is not installed)"
    fi
    echo

    if [ "$problems" -gt 0 ]; then
        echo "$problems problem(s) above."
        exit 1
    fi
    echo "Nothing found that an update has taken away."
    exit 0
fi

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
# that was just typed. Our own two pids and anything running this script are
# dropped for the same reason.
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
    # `grep -c` prints 0 and exits 1 when it finds nothing, so `|| echo 0` would
    # put a second line in the variable (entry 99).
    count_in_maps() {
        found=$(grep -c "$1" "/proc/$pid/maps" 2>/dev/null | head -1)
        [ -n "$found" ] || found=0
        echo "$found"
    }
    # And whether the question could be asked at all, which is a different
    # answer from "no": an unreadable /proc/<pid>/maps gives grep nothing to
    # count, and must not be reported as "not inside it".
    # Readable AND with something in it. A kernel thread's map is readable and
    # empty, and "no overlay in a kworker" is true but not an answer anybody
    # came here for; an unreadable one is another user's process, which is the
    # case somebody reaching for this tool is most likely to be confused by.
    #
    # Not `[ -s ]`: every file under /proc reports a size of zero, so that test
    # calls plasmashell's own map empty. The first line is the measurement.
    if [ -n "$(head -n 1 "/proc/$pid/maps" 2>/dev/null)" ]; then
        maps_readable=yes
    else
        maps_readable=no
    fi
    shim=$(count_in_maps "libvocem_gl_shim")
    lib=$(count_in_maps "libvocem_gl\.so")
    vk=$(count_in_maps "libvocem_vk")
    if [ "$maps_readable" = no ]; then
        echo "  shim loaded:       unknown (cannot read /proc/$pid/maps)"
        echo "  GL overlay:        unknown"
        echo "  Vulkan layer:      unknown"
        echo "  environment:       unknown (cannot read /proc/$pid/environ)"
        echo
        echo "  Its memory map is empty or cannot be read from here: another user's"
        echo "  process, or a kernel thread, which has no libraries at all. Nothing"
        echo "  above is a measurement of the overlay -- run this as the user who owns"
        echo "  the process."
        echo
        continue
    fi
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
    # By this point the map was readable, so an empty value here means the
    # variable is not set rather than that nothing could be read.
    env_of() {
        # The redirect is what fails when /proc/<pid>/environ is another user's,
        # and a redirect fails in the SHELL: `2>/dev/null` on the command does
        # not silence it. cat's own error is silenced the ordinary way.
        cat "/proc/$pid/environ" 2>/dev/null | tr '\0' '\n' | sed -n "s/^$1=//p" | head -1
    }
    echo "  SteamAppId:        $(env_of SteamAppId)"
    echo "  FLATPAK_ID:        $(env_of FLATPAK_ID)"
    echo "  LD_PRELOAD:        $(env_of LD_PRELOAD)"

    # 4. What the user decided about it. VOCEM_DISABLE is read twice, two
    # ways: the OpenGL path is off when the value starts with 1, and the
    # Vulkan loader drops the layer when the variable is SET at all -- it is
    # the manifest's disable_environment, and the loader compares no value
    # (VK_LOADER_DEBUG=layer shows it). So "0" and an empty value still turn
    # the Vulkan overlay off, and an empty value is not "not set". The lists
    # are matched against the process name and the executable's, and hidden
    # wins over shown (vocem/apps.h, draw_here).
    disable=$(env_of VOCEM_DISABLE)
    if ! cat "/proc/$pid/environ" 2>/dev/null | tr '\0' '\n' | grep -q '^VOCEM_DISABLE='; then
        echo "  VOCEM_DISABLE:     (not set)"
    else
        case "$disable" in
            1*) echo "  VOCEM_DISABLE:     $disable -> the overlay is switched off in this process" ;;
            *)  echo "  VOCEM_DISABLE:     '$disable' -> the Vulkan layer is switched off in this process"
                echo "    (the loader drops it for any value; the OpenGL path wants one starting with 1)" ;;
        esac
    fi
    binary=$(basename "$(readlink "/proc/$pid/exe" 2>/dev/null)" 2>/dev/null)
    if hidden_by=$(list_names "$hidden_list" "$comm" "$binary"); then
        echo "  hidden_apps:       names it ('$hidden_by') -> never drawn here"
    else
        echo "  hidden_apps:       does not name it"
    fi
    if shown_by=$(list_names "$shown_list" "$comm" "$binary"); then
        if [ -n "${hidden_by:-}" ]; then
            echo "  shown_apps:        names it ('$shown_by'), but hidden_apps wins"
        else
            echo "  shown_apps:        names it ('$shown_by') -> drawn even if not a game"
        fi
    else
        echo "  shown_apps:        does not name it"
    fi
    hidden_by=
    if [ -n "$switched_off" ]; then
        echo "  overlay switch:    OFF (enabled = $enabled) -> nothing is drawn anywhere"
    fi
    if [ -n "$(env_of FLATPAK_ID)" ]; then
        echo "    (a Flatpak reads the daemon's mirrored copy of the settings, not the file above)"
    fi

    # 5. And what it wrote about itself, if it got that far.
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
