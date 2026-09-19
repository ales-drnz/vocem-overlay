# The Vulkan layer extension

The overlay reaches a game that is itself a Flatpak through a Vulkan layer
extension, which Flatpak mounts into other applications' sandboxes. This
directory holds its manifest. Everything else about the project, the daemon, the
settings window and the host libraries, is installed on the host as usual.

Published from this repository's own pages, at
<https://ales-drnz.github.io/vocem-overlay/>. Not on Flathub.

## When to rebuild it

It carries the three injected libraries at both widths, the two layer
manifests, the emoji bank with its sequence table, and the licence texts those
libraries oblige; nothing from `daemon/`, `cli/` or `gui/`. So it is rebuilt and
republished when a release touched `gl/`, `layer/`, `common/`, `include/` or
`third_party/`, the same rule that decides whether `build32` has to be rebuilt,
and left alone otherwise:

```sh
git diff --name-only <previous tag>..HEAD -- gl layer common include third_party \
    CMakeLists.txt flatpak
```

`CMakeLists.txt` and `flatpak/` are in that list because the payload is decided
there: the manifest's own `post-install` is everything the extension carries
that is not a library, and 0.1.9 shipped without `emoji_sequences.bin` because
of one missing line in it. A change to either is a change to the extension even
when no library moved.

The one coupling that is not optional: a change to `kAbiVersion` means the
extension must be republished, or a Flatpak game reads a segment it refuses.

## Build

Once, the toolchain:

```sh
flatpak install --user flathub org.flatpak.Builder org.freedesktop.Sdk.Compat.i386 org.freedesktop.Sdk.Extension.toolchain-i386 org.freedesktop.Platform.Compat.i386
```

Then, from the top of the checkout, with the signing key and the project's own
version:

```sh
flatpak run --filesystem="$PWD" org.flatpak.Builder --force-clean --gpg-sign=FA67BB03AECF6941 --subject="Vocem Overlay $(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9.]*\)$/\1/p' CMakeLists.txt | head -1)" --body="Built from $(git rev-parse --short HEAD) of https://github.com/ales-drnz/vocem-overlay" --repo=vocem-flatpak-repo vocem-flatpak-build flatpak/vulkanlayer/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml
```

The version in the subject comes from `CMakeLists.txt`, which is the one place
this project states its release (entry 110). It used to come from
`packaging/PKGBUILD`, which says outright that it stays one release behind while
the next one is being written: without the subject `flatpak info` shows a
generated line and nobody can tell whether the fix a release announced is in the
copy they have, so a subject naming the wrong release is the same defect
politely.

The build directory, the exported repository and `.flatpak-builder` all sit
beside the manifest, on one filesystem. Do not move any of them to `/tmp`: the
builder's sandbox has a `/tmp` of its own, and flatpak-builder refuses when its
state directory and its target are on different filesystems. `.gitignore`
carries all three.

The `type: dir` source is the checkout itself, and a special file anywhere
under it fails the build, saying it cannot copy a special file, before the
first module runs. 0.1.8's first export failed on a socket the single-instance
test had left under `build/tests`: the test now removes it, and the manifest's
`skip` list leaves the build trees, `.flatpak-builder` and the repository out
of the copy. `find . -path ./.git -prune -o -type s -print` before building is
the check.

`--subject` is not decoration. The branch is `25.08`, which says which runtime
the extension fits and nothing about what is in it, so without a subject
`flatpak info` reads "Export org.freedesktop.Platform.VulkanLayer.VocemOverlay"
and a user has no way to tell whether the fix a release announced is in the copy
they have. With it, `flatpak info` says `Vocem Overlay 0.1.4`.

**The command finishes before it exits, and one run has sat there for twenty-five
minutes with the work already done.** Read the artefact rather than the clock:
the export is complete when

```sh
ostree --repo=vocem-flatpak-repo log runtime/org.freedesktop.Platform.VulkanLayer.VocemOverlay/x86_64/25.08
```

prints the commit with this release's subject on it.

Stop it then, and look in `~/.gnupg/public-keys.d/` before the next step: the
sandboxed gpg that signed the commit leaves `pubring.db.lock` behind, naming a
pid of the sandbox's own namespace. On the host that pid is somebody else (it
was `kauditd` at 0.1.8), so the host's gpg believes the lock is held and waits
on it forever, and `flatpak build-update-repo --gpg-sign=` reports "No gpg key
found" instead of waiting. With the builder gone the lock is stale: remove it
(and its `.#lk…` twin) and `gpg --list-secret-keys FA67BB03AECF6941` answers
at once.

The i386 half is cross-compiled by the second module. It cannot be built on the
host instead: the layer must match the runtime's ABI, and a library built
against the host's glibc will not load inside the runtime.

## Publish

```sh
flatpak build-update-repo --generate-static-deltas --prune --gpg-sign=FA67BB03AECF6941 vocem-flatpak-repo
```

Two deltas come out of this, and both are wanted: the from-empty one, which is
the whole download for somebody installing today, and the incremental one from
the previous release, which is what `flatpak update` takes. Measured at 0.1.5:
`884edf4e35-258698c0c2` and `258698c0c2`. Leave `--prune-depth` alone; at 0 it
drops the parent commit, and the parent is what the incremental delta is against.

This paragraph used to say the repository is rebuilt from scratch every release,
so that a user's history is always unrelated to it and the from-empty delta is
the only one. That is not what the command does -- `--force-clean` cleans the
build directory and never the repository -- and it is not what happened at 0.1.5,
which exported on top of 0.1.4's commit. Keeping the parent is the better of the
two anyway: an existing user downloads the difference rather than the whole
thing. The cost is the size served, 7 MB at one commit and 24 MB at two.

The `gh-pages` branch carries `repo/`, `vocem-overlay-layer.flatpakref`,
`index.html` and `.nojekyll`, and is force-pushed whole at each release so it
never accumulates old objects. GitHub Pages then takes minutes, not seconds, to
serve what was pushed.

**Sign it.** Without a `GPGKey` field in the `.flatpakref`, Flatpak marks the
remote it creates `no-gpg-verify` and says nothing about it. The public half of
the key is in that file; the private half stays on the release machine, which is
why the build is not run from CI.

Then verify what is served rather than what was built: pull the ref over HTTPS
into a scratch `ostree` repository, twice. With the public key imported the pull
must succeed and the commit must carry this release's subject; without it the
same pull must be refused for the signature. A pull that succeeds either way has
proved nothing.

Nothing is attached to the GitHub release. The extension has no version of its
own: on Flatpak the number is `25.08`, which is the runtime's extension point,
and `flatpak update` simply takes what is newest on the branch.

## The branch

`25.08` is the version of the *extension point* the runtime declares, not the
runtime's own name, so one branch covers several runtimes:

| Runtime | Used by | Extension point |
| --- | --- | --- |
| `org.freedesktop.Platform/25.08` | Steam, Heroic | 25.08 |
| `org.gnome.Platform/50`, `/49` | Sober, Lutris, Bottles | 25.08 |
| `org.kde.Platform/6.10` | PrismLauncher | 25.08 |
| `org.kde.Platform/6.9`, `org.freedesktop.Platform/24.08` | older applications | 24.08 |

A second branch is only needed for the last row.

## What the user does

For Vulkan, install the extension and nothing else. For OpenGL, which has no
extension mechanism, the interposer has to be preloaded by hand: the command is
in the project's `README.md`, which is the one place it is written, so that it
follows the project if the method changes. Both paths need `vocemd` running on
the host.
