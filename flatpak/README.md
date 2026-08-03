# Flatpak

This is not a way of distributing Vocem Overlay. The release channel is the AUR
and it stays there. What lives here is one **feature**: a Vulkan layer
extension, which is the only mechanism by which the overlay can reach a game
that is itself a Flatpak. It is built from this checkout and published beside a
release, not through Flathub -- DESIGN entry 85 records the decision and its
reasons, and it was the owner's.

`vulkanlayer/` holds the manifest. There is one, and it builds from the working
tree, so there is no second copy to drift away from it; entry 76 is what letting
`PKGBUILD` and `PKGBUILD.local` drift cost.

## Building it

Needs `org.flatpak.Builder`, plus the i386 compatibility and cross-toolchain
extensions for the 32-bit half:

```sh
flatpak install --user flathub org.flatpak.Builder org.freedesktop.Sdk.Compat.i386 org.freedesktop.Sdk.Extension.toolchain-i386 org.freedesktop.Platform.Compat.i386
```

Then, from the top of the checkout:

```sh
flatpak run --filesystem="$PWD" org.flatpak.Builder --force-clean --repo=/tmp/vocem-flatpak-repo /tmp/vocem-flatpak-build flatpak/vulkanlayer/org.freedesktop.Platform.VulkanLayer.VocemOverlay.yml
```

## Publishing it

The repository the build already produced **is** the thing to publish: it is a
directory of ordinary files served over HTTPS, and Flatpak's own documentation
says so -- "hosting a repository is the preferred way to distribute an
application, since repositories allow applications to be updated". Measured
here: 6.7 MB and 61 files after

```sh
flatpak build-update-repo --generate-static-deltas --prune /tmp/vocem-flatpak-repo
```

which is nothing for GitHub Pages. Publish it from a Pages *artifact* rather
than by committing it to a branch, or every version's objects end up in the git
history for ever.

It is served from the `gh-pages` branch, which carries `repo/`, the
`.flatpakref` and an `index.html`, and is force-pushed whole at each release so
the branch never accumulates old objects. Live at
<https://ales-drnz.github.io/vocem-overlay/>.

`vulkanlayer/vocem-overlay-layer.flatpakref` is the whole user-facing
installation:

```sh
flatpak install --user https://ales-drnz.github.io/vocem-overlay/vocem-overlay-layer.flatpakref
```

Measured from the public address, on a machine with nothing of it installed: it
adds the remote and installs the extension in one step, both architectures land
inside a real sandbox, and `flatpak update` carries it from then on. On a
desktop the file can simply be opened.

**It is signed**, with the key whose public half is in the `GPGKey` field of
that file (`FA67BB03AECF6941`). The private half stays on the release machine
and never leaves it. Without the field, Flatpak sets `no-gpg-verify` on the
remote it creates and takes whatever the server hands it, quietly -- measured
on the remote it made before the key was there. `build-update-repo` and the
build both take `--gpg-sign=`.

Building and publishing from a CI workflow would mean putting that private key
into a repository secret. It is not done, and it is the reason: the key is on
one machine on purpose.

### The offline alternative

A single-file bundle, for somebody with no network path to the repository:

```sh
flatpak build-bundle --runtime /tmp/vocem-flatpak-repo vocem-overlay-flatpak-layer.flatpak org.freedesktop.Platform.VulkanLayer.VocemOverlay 25.08
```

About three megabytes, installed with `flatpak install --user ./that-file`.
Measured to work end to end -- both architectures and both layer manifests
appear inside a real sandbox and the overlay draws -- but a bundle carries no
remote, so it never updates itself. It is the fallback, not the plan.

### Why the AUR package cannot just do this itself

It cannot, and the measurement is one line. The extension has to be built
against the *runtime's* ABI, not the host's: the layer this machine builds for
`/usr/lib` refuses to load inside `org.freedesktop.Platform//25.08` with

```
libvocem_vk.so: /usr/lib/x86_64-linux-gnu/libm.so.6: version `GLIBC_2.43' not found
```

-- host glibc 2.44, runtime glibc 2.42. So pointing a sandboxed game's loader at
the host's copy with `VK_ADD_IMPLICIT_LAYER_PATH`, which both loaders do
support, gets a layer that cannot be loaded. The extension is built in the SDK
by whoever cuts the release, and only the result travels.

## Which branch

The branch is the version of the extension *point* the runtime declares, not the
runtime's own name. Measured against Flathub's own runtimes:
`org.freedesktop.Platform/25.08` (Steam, Heroic), `org.gnome.Platform/50`
(Sober) and `/49` (Lutris, Bottles) and `org.kde.Platform/6.10`
(PrismLauncher) all declare it at **25.08**, so the one branch built here
reaches all of them. Only `org.kde.Platform/6.9` and
`org.freedesktop.Platform/24.08` would need a second branch.

## What the user does after installing it

Nothing, for a Flatpak game that renders with Vulkan. The extension is mounted
into every application on a matching runtime and the layer is on by default.

OpenGL has no extension mechanism of any kind, so a Flatpak game that renders
with OpenGL needs the preload named by hand, once -- per application, or for all
of them with no application named:

```sh
flatpak override --user --env=LD_PRELOAD=libvocem_gl_shim.so --env=LD_LIBRARY_PATH=/usr/lib/extensions/vulkan/VocemOverlay/lib/x86_64-linux-gnu:/usr/lib/extensions/vulkan/VocemOverlay/lib/i386-linux-gnu
```

A bare soname and both architecture directories, rather than the linker's `$LIB`
token that the session preload on the host uses. Measured: `$LIB` expands in
`/usr/$LIB/...` and does **not** expand under `/usr/lib/extensions/...`, so the
one-value trick that works on the host does not work here -- MangoHud's own
Flatpak wrapper spells it the way that does not work. With a soname the loader
searches both directories and loads the one whose ABI matches the process,
silently ignoring the other, so a 32-bit game under Proton and a 64-bit native
game take the same two variables.

Both need `vocemd` running **on the host**: the daemon holds the Discord
connection and is the only thing that can. Installing the AUR package is what
puts it there.
