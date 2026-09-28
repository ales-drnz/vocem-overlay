// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// When an avatar file is worth trying again, and when it is not.
//
// "The file would not load" is not final: somebody who joins a channel is drawn
// on the next frame, while the daemon is still downloading their picture. The
// difference is on disk: the daemon renames avatars into place, so a file that
// exists is complete and one that fails to decode stays broken, while a file that
// is not there yet is worth looking for again.

#ifndef VOCEM_AVATAR_FILE_H
#define VOCEM_AVATAR_FILE_H

#include <sys/stat.h>

#include "vocem/clock.h"

namespace vocem {

// How often to look again for a file that has not appeared, and how long to keep
// looking. A download that has not finished in half a minute is not going to.
constexpr double kAvatarRetrySeconds = 0.5;
constexpr double kAvatarGiveUpSeconds = 30.0;

inline bool avatar_file_exists(const char* path) {
    struct stat info {};
    return ::stat(path, &info) == 0 && info.st_size > 0;
}

// The one clock, under the name this policy's callers already use.
inline double avatar_now_seconds() {
    return monotonic_seconds();
}

// The wait for one face that has not arrived: when it was first asked for and
// when to look again. Shared by both paths' caches; plain arithmetic, nothing
// allocated.
struct AvatarWait {
    double first_asked = 0.0;
    // The first look is immediate: somebody who joins is drawn on the next
    // frame and their picture may already be there.
    double next_attempt = 0.0;

    static AvatarWait start(double now) { return AvatarWait{now, now}; }

    // Whether this frame should look at all. A stat() on every frame for every
    // face is not free, and this runs inside somebody's game.
    bool due(double now) const { return now >= next_attempt; }

    // The file was not there. True while it is worth looking again (the next
    // look scheduled); false once the download has had its thirty seconds, and
    // the face stays the placeholder for the session.
    bool missed(double now) {
        if (now - first_asked >= kAvatarGiveUpSeconds) {
            return false;
        }
        next_attempt = now + kAvatarRetrySeconds;
        return true;
    }
};

}  // namespace vocem

#endif  // VOCEM_AVATAR_FILE_H
