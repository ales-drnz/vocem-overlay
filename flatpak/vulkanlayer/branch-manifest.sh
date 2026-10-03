#!/bin/sh
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The extension's manifest for one branch, written beside the manifest it is
# made from.
#
#   flatpak/vulkanlayer/branch-manifest.sh 26.08
#
# A runtime looks for the extension at the version of the extension POINT it
# declares, and freedesktop 26.08 (and GNOME 51 on top of it) declares 26.08,
# where 25.08, GNOME 50 and KDE 6.11 declare 25.08. A branch the runtime does
# not ask for is never mounted, and nothing says so: Flathub's Steam moved to
# 26.08 while this project published 25.08 alone. So every branch in
# `branches` is built and published, from ONE manifest -- the source file is
# the first branch, and the others differ from it in `runtime-version` and
# `branch` and nothing else (tests/flatpak_branches.cmake, entry 306).
# flatpak-builder has no option for either, which is why a file is written
# rather than a flag passed. The file sits beside the source because the manifest's own source
# path (`../..`) is relative to where it lives; .gitignore keeps it out.
#
# Prints the path of the file it wrote.

set -eu

branch=${1:?usage: branch-manifest.sh <branch>}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source="$here/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml"

if ! grep -Fqx -- "$branch" "$here/branches"; then
    echo "branch-manifest.sh: $branch is not in $here/branches" >&2
    exit 1
fi

# The source's own branch, read rather than assumed.
current=$(sed -n "s/^branch: '\\([0-9.]*\\)'\$/\\1/p" "$source")
if [ -z "$current" ]; then
    echo "branch-manifest.sh: no branch line in $source" >&2
    exit 1
fi
if [ "$branch" = "$current" ]; then
    echo "$source"
    exit 0
fi

out="$here/org.freedesktop.Platform.VulkanLayer.VocemOverlay-$branch.yml"
sed -e "s/^runtime-version: '$current'\$/runtime-version: '$branch'/" \
    -e "s/^branch: '$current'\$/branch: '$branch'/" "$source" > "$out"
echo "$out"
