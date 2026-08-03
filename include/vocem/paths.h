// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Where things live. Shared so that the daemon, the CLI and the interface cannot
// disagree about it -- a second copy of a path is a bug waiting for someone to fix
// only one of them.

#ifndef VOCEM_PATHS_H
#define VOCEM_PATHS_H

#include <sys/stat.h>

#include <cstdlib>
#include <string>

namespace vocem {

// mkdir -p, by hand, once. It existed in four hand-written copies -- the
// daemon's auth and avatar writers, the settings writer, the app records --
// three at 0700 and one at 0755 for no reason anybody could name, which is the
// entry-33 shape wearing permission bits. No filesystem library, because two
// of the callers run inside other people's games. 0700 for everything: every
// directory this project creates holds the user's own private state.
inline void make_directories(const std::string& path) {
    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!partial.empty()) {
                ::mkdir(partial.c_str(), 0700);
            }
        }
        if (i < path.size()) {
            partial.push_back(path[i]);
        }
    }
}

inline std::string state_home() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return xdg;
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/state";
}

// The Discord access token. A credential: created with mode 0600.
inline std::string token_path() { return state_home() + "/vocem/token"; }

}  // namespace vocem

#endif  // VOCEM_PATHS_H
