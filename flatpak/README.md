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

Then, from the top of the checkout, with the signing key and the project's own
version:

```sh
flatpak run --filesystem="$PWD" org.flatpak.Builder --force-clean --gpg-sign=FA67BB03AECF6941 --subject="Vocem Overlay $(grep -m1 '^pkgver=' packaging/PKGBUILD | cut -d= -f2)" --body="Built from $(git rev-parse --short HEAD) of https://github.com/ales-drnz/vocem-overlay" --repo=vocem-flatpak-repo vocem-flatpak-build flatpak/vulkanlayer/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml
```

**The build directory and the repository are beside the manifest, not under
`/tmp`, and that is load-bearing twice over.** This command named `/tmp` paths
until 0.1.3, and neither of the two faults that caused was visible in its
output:

* `flatpak run` gives the application its own `/tmp`. `--filesystem=host`,
  which `org.flatpak.Builder` already carries, does not cover it -- measured:
  from inside the sandbox `/tmp` is empty and the host's directory of the same
  name is not there. So the exported repository would have been written into a
  tmpfs that is discarded when the builder exits.
* flatpak-builder refuses outright when its state directory and the target
  directory are on different filesystems, which they were: `.flatpak-builder`
  sits beside the manifest on the disk, `/tmp` here is tmpfs. That refusal is
  what actually happened, and it is the lucky half -- it stops the command
  before the first fault can lose anything.

Keeping all three in the checkout answers both at once: one filesystem, one
`--filesystem="$PWD"`, no `--state-dir`, and nothing left in a directory the
machine empties overnight. `.gitignore` carries all three. The move was checked
rather than assumed: the repository it exports has the same `ContentChecksum`
as the one built the old way, so what changed is where the work happens and not
what ships.

**The command does not exit when it is done, and that is not a failure.**
Signing makes gpg start `gpg-agent` and `keyboxd`, and it starts them *inside*
the sandbox; `bwrap` does not exit until every process in its namespace has
gone, and those two are daemons, so they never do. Measured at 0.1.4: the
export finished and the wrapper then sat in `do_wait` with no child doing any
work for twenty-five minutes, until it was killed. Read the state before
waiting on it: the repository is complete when
`ostree --repo=vocem-flatpak-repo log runtime/org.freedesktop.Platform.VulkanLayer.VocemOverlay/x86_64/25.08`
prints the commit with the subject this release's `--subject` asked for. Then
kill the two daemons by pid -- `gpgconf --kill all` does not reach them, it
talks to the host's socket and theirs is inside the sandbox -- and the wrapper
exits 0 on its own. Nothing about the export is affected either way.

`--subject` is not decoration. The branch is `25.08`, which says which runtime
the extension fits and nothing about what is in it, so without a subject
`flatpak info` reads "Export org.freedesktop.Platform.VulkanLayer.VocemOverlay"
and a user has no way to tell whether the fix a release announced is in the
copy they have. With it, `flatpak info` says `Vocem Overlay 0.1.3`.

The i386 half is cross-compiled by the second module. It cannot be built on the
host instead: the layer must match the runtime's ABI, and a library built
against the host's glibc will not load inside the runtime.

## Publish

```sh
flatpak build-update-repo --generate-static-deltas --prune --gpg-sign=FA67BB03AECF6941 vocem-flatpak-repo
```

About 7 MB. The `gh-pages` branch carries `repo/`, `vocem-overlay-layer.flatpakref`
and `index.html`, and is force-pushed whole at each release so it never
accumulates old objects.

`--generate-static-deltas` earns its place, and not the way it looks. Because
the repository is built from scratch every release, it never contains the
commit a user already has, so their history is *unrelated* in OSTree's sense --
and the from-empty ("scratch") delta is exactly what OSTree documents for that
case: one file instead of the several dozen object requests archive-z2 would
otherwise cost over HTTP. Measured at 0.1.3: 6 766 555 bytes in the from-empty
delta. What is NOT here is a small incremental delta between releases; that
would need the previous release's commit to still be in the repository, which
is a different arrangement from the one the Flatpak documentation shows and
would buy a few megabytes once per release. Leave `--prune-depth` alone: at 0
it drops the parent commit, which removes the only incremental delta there is.

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
