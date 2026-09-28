// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The symbol versions glibc gives `dlsym`, and the order to try them in.
//
// A header because two things must agree on it: the shim, and
// tests/shim_dlsym.cpp, which checks the resolution at both widths.
//
// **Getting this wrong is not a degraded overlay.** The shim interposes `dlsym`,
// so with no real one every `dlsym` in the process returns null.
// The version is per architecture: x86-64's libc carries `GLIBC_2.2.5`, i386's
// `GLIBC_2.0`, both `GLIBC_2.34`. The architecture's own goes first;
// `GLIBC_2.34`, the version since libdl was merged into libc, follows so an
// architecture nobody thought of still resolves; the rest make the list
// exhaustive rather than clever.

#ifndef VOCEM_REAL_DLSYM_H
#define VOCEM_REAL_DLSYM_H

#define VOCEM_DLSYM_VERSIONS                             \
    {                                                    \
        /* This architecture's own, first. */            \
        VOCEM_DLSYM_NATIVE_VERSION,                      \
        /* Since libdl was merged into libc. */          \
        "GLIBC_2.34",                                    \
        /* Everything else, so the list is exhaustive. */\
        "GLIBC_2.2.5", "GLIBC_2.0", "GLIBC_2.17",        \
    }

#if defined(__i386__) || defined(__arm__) || defined(__m68k__)
#define VOCEM_DLSYM_NATIVE_VERSION "GLIBC_2.0"
#elif defined(__aarch64__) || defined(__riscv)
#define VOCEM_DLSYM_NATIVE_VERSION "GLIBC_2.17"
#else
#define VOCEM_DLSYM_NATIVE_VERSION "GLIBC_2.2.5"
#endif

#endif  // VOCEM_REAL_DLSYM_H
