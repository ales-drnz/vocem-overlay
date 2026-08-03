# Wuffs

The PNG decoder the daemon uses, and the only parser in this project that is fed
bytes from the internet. It replaced stb_image after 0.1.0-53; why, and what was
measured before the swap was kept, is DESIGN entry 51.

| | |
| --- | --- |
| File | `wuffs-v0.4.c` — the upstream single-file release, amalgamated |
| Source | `https://raw.githubusercontent.com/google/wuffs/main/release/c/wuffs-v0.4.c` |
| Version string | `0.4.0-alpha.10+3966.20260623` (`WUFFS_VERSION_STRING`) |
| sha256 | `1f8039ef82911604c063f6ac2ed57254bdb17d742aebdeae06356530d4a0fde7` |
| Fetched | 2026-07-30 |
| Licence | `LICENSE` here: dual MIT / Apache 2.0. Taken under MIT, which is this project's own |

**On the `alpha` in that version string.** Wuffs labels its release C files this
way; it is said here rather than smoothed over, because a dependency's own words
about its stability are part of what is being depended on. What the label does not
change: the decoders are generated from a memory-safe language whose bounds and
overflow checks are proved when the C is produced, the project is maintained by
Google and its PNG decoder is deployed in Chromium's image pipeline, and the
output of this exact file was compared against stb_image byte for byte on six
colour types before it was kept (`tests/avatar_decoders_agree.cpp`). What it does
change: the API may move between releases, so the version is pinned here and a
bump is a change somebody reads, not a `curl` in a script.

`daemon/src/avatar_decode.h` carries the whole configuration -- which modules are
compiled, the pixel format asked for, and the dimension ceiling handed to
`DecodeImage` -- so there is one spelling of it.
