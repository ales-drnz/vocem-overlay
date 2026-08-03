// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The daemon's log, in one place.
//
// These two macros lived inside main.cpp's anonymous namespace while main.cpp
// was the only thing that logged. The Flatpak bridge is a second translation
// unit with things to say -- how many sandboxes it serves, and every one it
// refuses and why -- and a second copy of the spelling is how a prefix drifts.

#ifndef VOCEM_DAEMON_LOG_H
#define VOCEM_DAEMON_LOG_H

#include <cstdio>
#include <cstdlib>

namespace vocem {

inline bool verbose() {
    static const bool value = [] {
        const char* env = std::getenv("VOCEM_DEBUG");
        return env && env[0] == '1';
    }();
    return value;
}

}  // namespace vocem

#define LOG(...)                                       \
    do {                                               \
        std::fprintf(stderr, "[vocemd] " __VA_ARGS__); \
        std::fputc('\n', stderr);                      \
    } while (0)

#define DBG(...)              \
    do {                      \
        if (vocem::verbose()) { \
            LOG(__VA_ARGS__); \
        }                     \
    } while (0)

#endif  // VOCEM_DAEMON_LOG_H
