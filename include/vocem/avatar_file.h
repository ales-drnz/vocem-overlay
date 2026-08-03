// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// When an avatar file is worth trying again, and when it is not.
//
// The two injected paths cache avatar textures, and both of them used to treat
// "the file would not load" as final. It is not: somebody who joins a channel is
// drawn on the very next frame, and the daemon is still downloading their picture
// at that moment. The file appears half a second later and nothing ever looked
// again -- so everybody present when the game started had a face and everybody who
// arrived afterwards was a grey disc for the rest of the session.
//
// The two cases have to be told apart, and the difference is on disk: the daemon
// writes avatars with a rename into place, so a file that exists is complete. A
// file that fails to decode is broken and asking again will not fix it. A file
// that is not there yet is simply not there yet.

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

}  // namespace vocem

#endif  // VOCEM_AVATAR_FILE_H
