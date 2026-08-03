// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The symbol versions glibc gives `dlsym`, and the order to try them in.
//
// This is a header rather than a list in one .cpp because two things have to agree
// about it: the shim, which the whole session is preloaded with, and the test that
// checks the resolution actually works on the architecture being built -- including
// the 32-bit one, which is built in a tree of its own and had no test at all.
//
// **Getting this wrong is not a degraded overlay. It is every `dlsym` in the
// process returning null.** The shim interposes `dlsym`, so an application that
// asks for any symbol at all gets our answer, and our answer is whatever the real
// one gave us. With no real one, the answer is null for everything.
//
// It happened. `GLIBC_2.2.5` was hardcoded, which is the version on x86-64 and does
// not exist on i386 -- measured here: `/usr/lib/libc.so.6` carries
// `dlsym@@GLIBC_2.34` and `dlsym@GLIBC_2.2.5`, `/usr/lib32/libc.so.6` carries
// `dlsym@GLIBC_2.0` and `dlsym@@GLIBC_2.34`. The Steam client's own binary is
// `ubuntu12_32/steam`; it asked `dlsym` for `setenv`, got null, and shut down one
// second after starting, with `CProcessEnvironmentManager ERROR: dlsym 'setenv'
// failed` as the only clue.
//
// The architecture's own version goes first. `GLIBC_2.34` follows because that is
// the version everything has carried since libdl was merged into libc, so an
// architecture nobody thought of here still resolves rather than failing silently.
// The rest are there so the list is exhaustive instead of clever.

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
