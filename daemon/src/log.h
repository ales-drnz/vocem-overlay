// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The daemon's log, in one place.
//
// Every translation unit that logs (main.cpp, the Flatpak bridge and the rest)
// uses these two macros, so the prefix has one spelling and cannot drift.

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
