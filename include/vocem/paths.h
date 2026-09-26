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

// TASK_COMM_LEN - 1: the longest name /proc/<pid>/comm can hold, which is the
// truncation every process-name comparison and every record name must respect.
// It was spelled as a bare 15 in five places (apps.h twice, journal.h, the
// Flatpak bridge's field bound, the window's desktop-entry guard), free to
// drift -- and one copy drifting silently drops records or stops recognising a
// truncated name. Here because this header is the shared floor those files
// already stand on.
constexpr size_t kCommLength = 15;

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

// Where the user's own settings and credentials live when there is no HOME
// and no XDG variable to say: nowhere. A directory under /dev/null, which no
// file can ever exist beneath (ENOTDIR, for root as well), so a read finds
// nothing and a write fails instead of landing somewhere. The fallback used
// to be ".", the working directory of whatever process asked -- for the
// settings that is a game, started without HOME by a service or `env -i`,
// in a directory somebody else may write, reading a config.ini that names a
// font file for stb_truetype (entry 135 removed the same fallback from the
// record and journal directories, which write to /tmp instead; files READ
// from a shared directory are the ones that must not be anywhere at all).
inline constexpr const char* kNoHomeDirectory = "/dev/null";

inline std::string state_home() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return xdg;
    }
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.local/state" : std::string(kNoHomeDirectory);
}

// The Discord access token. A credential: created with mode 0600.
inline std::string token_path() { return state_home() + "/vocem/token"; }

}  // namespace vocem

#endif  // VOCEM_PATHS_H
