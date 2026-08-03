#!/bin/sh
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Run a Vulkan application against the layer in the build tree, without
# installing anything.
#
#   scripts/dev-run.sh vkcube
#   scripts/dev-run.sh vkcube --c 240
#
# Note VK_ADD_IMPLICIT_LAYER_PATH rather than VK_ADD_LAYER_PATH: the latter is
# only consulted for explicit layers, and ours is implicit by design.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manifest_dir="$root/build/dev"

if [ ! -f "$manifest_dir/VkLayer_vocem_overlay.json" ]; then
    echo "error: build tree manifest not found. Build first:" >&2
    echo "  cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <application> [args...]" >&2
    exit 2
fi

VK_ADD_IMPLICIT_LAYER_PATH="$manifest_dir" \
VOCEM=1 \
VOCEM_DEBUG="${VOCEM_DEBUG:-1}" \
exec "$@"
