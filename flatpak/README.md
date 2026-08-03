# The Vulkan layer extension

The overlay reaches a game that is itself a Flatpak through a Vulkan layer
extension, which Flatpak mounts into other applications' sandboxes. This
directory holds its manifest. Everything else about the project — the daemon,
the settings window, the host libraries — is installed on the host as usual.

Published from this repository's own pages, at
<https://ales-drnz.github.io/vocem-overlay/>. Not on Flathub.

## When to rebuild it

It carries the three injected libraries, the two layer manifests and the emoji
bank, and nothing from `daemon/`, `cli/` or `gui/`. So it is rebuilt and
republished when a release touched `gl/`, `layer/`, `common/`, `include/` or
`third_party/` — the same rule that decides whether `build32` has to be rebuilt
— and left alone otherwise:

```sh
git diff --name-only <previous tag>..HEAD -- gl layer common include third_party
```

The one coupling that is not optional: a change to `kAbiVersion` means the
extension must be republished, or a Flatpak game reads a segment it refuses.

## Build

Once, the toolchain:

```sh
flatpak install --user flathub org.flatpak.Builder org.freedesktop.Sdk.Compat.i386 org.freedesktop.Sdk.Extension.toolchain-i386 org.freedesktop.Platform.Compat.i386
```

Then, from the top of the checkout, with the signing key:

```sh
flatpak run --filesystem="$PWD" org.flatpak.Builder --force-clean --gpg-sign=FA67BB03AECF6941 --repo=/tmp/vocem-flatpak-repo /tmp/vocem-flatpak-build flatpak/vulkanlayer/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml
```

The i386 half is cross-compiled by the second module. It cannot be built on the
host instead: the layer must match the runtime's ABI, and a library built
against the host's glibc will not load inside the runtime.

## Publish

```sh
flatpak build-update-repo --generate-static-deltas --prune --gpg-sign=FA67BB03AECF6941 /tmp/vocem-flatpak-repo
```

About 7 MB. The `gh-pages` branch carries `repo/`, `vocem-overlay-layer.flatpakref`
and `index.html`, and is force-pushed whole at each release so it never
accumulates old objects.

**Sign it.** Without a `GPGKey` field in the `.flatpakref`, Flatpak marks the
remote it creates `no-gpg-verify` and says nothing about it. The public half of
the key is in that file; the private half stays on the release machine, which is
why the build is not run from CI.

Nothing is attached to the GitHub release. The extension has no version of its
own — on Flatpak the number is `25.08`, which is the runtime's extension point,
and `flatpak update` simply takes what is newest on the branch — so a numbered
file attached to a numbered release is a snapshot of something that is not a
snapshot. It was tried once and went stale within hours.

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

For Vulkan, install it and nothing else. For OpenGL, which has no extension
mechanism, the interposer has to be preloaded by hand:

```sh
flatpak override --user --env=LD_PRELOAD=libvocem_gl_shim.so --env=LD_LIBRARY_PATH=/usr/lib/extensions/vulkan/VocemOverlay/lib/x86_64-linux-gnu:/usr/lib/extensions/vulkan/VocemOverlay/lib/i386-linux-gnu
```

A bare soname with both architecture directories on the search path, so the
loader takes the one matching the process. Do not rewrite it to use the linker's
`$LIB` token as the host's session preload does: `$LIB` does not expand under
`/usr/lib/extensions`.

Both paths need `vocemd` running on the host.
