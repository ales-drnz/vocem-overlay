# The defective exemplars

The historical packages beside this file, `vocem-overlay-0.1.0-*.pkg.tar.zst`,
are not in git (`.gitignore` keeps every package out) and exist on the owner's
disk alone. They are kept because the law in DESIGN.md asks every fixed defect
for a test that fails against the defective binary, and for the pre-git era the
packaged artifact is the only defective binary there is.

Which of them anything still names, so that a clean-up knows what it may not
touch. Found with `grep -ohE '0\.1\.0-[0-9]+' DESIGN.md tests/ CLAUDE.md`;
the rest of the 0.1.0 series is kept with them, since a citation can be added
at any time and a package cannot be rebuilt.

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

The packages from 0.1.2 on are rebuilds of tagged releases; a tag can be
rebuilt with `PKGBUILD`, so they are under no such rule.
