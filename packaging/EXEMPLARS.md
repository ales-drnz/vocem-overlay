# The defective exemplars

The historical packages beside this file, `vocem-overlay-0.1.0-*.pkg.tar.zst`,
are not in git (`.gitignore` keeps every package out) and exist on the owner's
disk alone. They are kept because the law in DESIGN.md asks every fixed defect
for a test that fails against the defective binary, and for the pre-git era the
packaged artifact is the only defective binary there is.

Which of them anything still names, so that a clean-up knows what it may not
touch. Found with
`grep -rohE '0\.1\.[0-9]+-[0-9]+' DESIGN.md CLAUDE.md tests/ | sort -u -V` --
the version that stood here was hardcoded to `0\.1\.0-`, so it could not find
a later exemplar even in principle, and six had quietly become one. The rest of
the 0.1.0 series is kept with them, since a citation can be added at any time
and a package cannot be rebuilt.

| Package | Cited by |
| --- | --- |
| 0.1.0-10 | DESIGN entry 129 (the Vulkan present hook, refuted at 0 pixels) |
| 0.1.0-41, -42 | DESIGN entries 35 and 36 (the two Electron freezes) |
| 0.1.0-43 | `tests/CMakeLists.txt`, daemon_notification (the body published unconditionally) |
| 0.1.0-44 | `tests/gl_draw_local.cpp` (the level-3 door, zero pixels) |
| 0.1.0-50 | `tests/CMakeLists.txt`, gl_noop_quiet and gl_toast_alone |
| 0.1.0-52 | `tests/CMakeLists.txt`, emoji_bank_licence |
| 0.1.0-53 | DESIGN entry 53 (the forged sentinel) |
| 0.1.0-59, -61 | DESIGN (the seqlock and the display height) |
| 0.1.0-64, -65 | `tests/CMakeLists.txt`, the window tests (padding, empty views, the moved channel) |
| 0.1.0-67 | `tests/gl_unpack_state.cpp` (the ES3 GL_INVALID_ENUM the backend leaves) |

## And the later ones, which this file used to give away

| Package | Cited by | Rebuildable? |
| --- | --- | --- |
| 0.1.3-1 | DESIGN entry 94 (the 32-bit hardening, 0 `_chk` symbols) | **yes** -- `PKGBUILD` at `#tag=v0.1.3` |
| 0.1.3-7 | `tests/daemon_reconnect.cpp`, `tests/CMakeLists.txt`, DESIGN 103 (2215 connections in five seconds) | **no** -- built from `PKGBUILD.local` two commits before the v0.1.3 tag |
| 0.1.4-1 | `tests/display_change_while_open.cmake`, `tests/daemon_note_expiry.cpp`, `tests/version_agrees.cmake`, DESIGN 103/110/111/112 | **yes** -- `PKGBUILD` at `#tag=v0.1.4` |
| 0.1.8-1 | DESIGN 138/139/140 (the bridge's drawing flag, the window's four spawns, two windows) | **no** -- its recipe hash is in no git object at all |
| 0.1.10-1 | `tests/fonts_lifecycle.cpp`, `tests/gl_context_cycle.cpp`, `tests/CMakeLists.txt`, DESIGN 144/145 | from a commit, not a tag: there is no `v0.1.10` |
| 0.1.10-2 | `tests/gl_inside_gamescope.cmake`, `tests/vk_present_draw.cpp`, DESIGN 147, and entries 159-164's daemon measurements | **no** -- built from an uncommitted working tree |
| 0.1.10-3 | DESIGN 148 (the release path's wait, refutation attempted and empty) and 150 (a restart read as a stop) | **no** -- its recipe is in no git object |
| 0.1.10-5 | `tests/CMakeLists.txt`, `tests/vk_present_draw.cpp`, `tests/gl_draw_local.cpp`, DESIGN 191/192 (the freeze: 7 and 8 rebuilds) | **no** -- built from the working tree five minutes before the commit that carries its recipe (`c35195f`) |
| 0.1.10-6 | DESIGN 194-205 (the release review: every refutation of it ran against this package, installed) | **no** -- built from the working tree 74 minutes before the commit that carries its recipe (`3f6db50`) |
| 0.1.10-7 | `tests/installed_runpath.cmake`, `tests/package_depends_needed.cmake` (the window's RUNPATH, the Vulkan loaders in depends), and every refutation of the 2026-09-26 review, which ran against this package installed | its .BUILDINFO recipe hash is `PKGBUILD.local` at `a97cb01`, the commit `v0.1.10` is on (sha256 05101331...) |
| 0.1.11-1 | `tests/daemon_unit_peer.cpp`, `tests/CMakeLists.txt`, `tests/unit_hardening.cmake`, entry 285 (the daemon under its own unit refused the real Discord; the new test fails against this package's vocemd and unit file, installed) | its .BUILDINFO recipe hash is `PKGBUILD.local` at `642a3c6` (sha256 ac1ed3c8...) |

**So "a tag can be rebuilt" was true of two of the six.** The sentence that
stood here -- "The packages from 0.1.2 on are rebuilds of tagged releases; a
tag can be rebuilt with `PKGBUILD`, so they are under no such rule" -- gave a
clean-up permission to delete the only defective binaries for entries 103, 138,
139, 140, 144, 145, 146, 147 and the September repairs. Measured corpus-wide:
**73 of the 88 packages here were built from a recipe that is not in git** (63
pre-git plus ten intermediate pkgrels), and three of them -- 0.1.8-1, 0.1.8-2
and 0.1.10-2 -- from a recipe that exists in no blob anywhere in this
repository. (74 of 91 on 2026-09-22, recounted the same way: every package's
`.BUILDINFO` `pkgbuild_sha256sum` against every `PKGBUILD` and
`PKGBUILD.local` blob in the history.) `packaging/*.pkg.tar.*` is in `.gitignore`, so no copy exists
anywhere else either.

**Nothing in this directory is deleted without checking this table first**, and
the table is regenerated with the grep above rather than edited by hand.
`tests/exemplars_listed.cmake` holds the half of that grep git can see: every
package version `tests/` names has a row here. It is the half that went stale
for 0.1.10-3, -5 and -6, which DESIGN and three tests named and this table did
not (DESIGN 205).

## What an exemplar cannot refute for ever

`kAbiVersion` has been 6 since the first commit, so 0.1.0-10 -- entry 129's
exemplar -- refutes "did anything reach the framebuffer" rather than the
present hook itself: its failure reads `state read failed (abi mismatch or
writer contention)`. **Every future ABI bump converts one more historical
exemplar's refutation into an ABI mismatch**, while the record goes on reading
"the test fails against the old binary". Written here because it is true of the
artifacts and of nothing in DESIGN.

Entries 144 and 145 leave no artifact trace at all: the fonts module is a
statically linked, hidden-visibility unit, so there is no cheap way to tell
0.1.10-1's defective copy from -2's fixed one except by running the test --
which makes those tests the whole record.
