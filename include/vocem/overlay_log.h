// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The injected code's debug log, in one place.
//
// Four macros used to do this -- VOCEM_GLOG in the heavy GL library, VOCEM_LOG
// in the layer, VOCEM_RLOG in its renderer and VOCEM_TLOG in its texture cache
// -- each with a getenv-once static of its own for VOCEM_DEBUG, and only the GL
// one knew VOCEM_LOG_FILE, which exists because a launcher can swallow a game's
// stderr (the Minecraft launcher does: the game's fd 2 is a pipe the launcher
// never writes anywhere readable). A Vulkan game under the same launcher had
// no way to be read at all. One logger now: the tag is the caller's, the two
// variables are read once, and the file half is the same for both paths --
// appended, line-buffered, pid on every line because every process in the
// session that draws shares the one file.
//
// Off unless VOCEM_DEBUG=1 or VOCEM_LOG_FILE is set; the macro asks before
// evaluating its arguments, so a line that is not wanted costs a load and a
// branch. Not for the shim (gl/src/vocem_gl_shim.cpp), which may not carry the
// C++ runtime a function-local static needs.

#ifndef VOCEM_OVERLAY_LOG_H
#define VOCEM_OVERLAY_LOG_H

namespace vocem {

// Whether any line would go anywhere: VOCEM_DEBUG=1, or a VOCEM_LOG_FILE that
// opened. Read once per process.
bool overlay_log_wanted();

// One line: "[<tag>] <message>" on stderr when VOCEM_DEBUG=1, and
// "[<tag> <pid>] <message>" appended to VOCEM_LOG_FILE when that is set.
void overlay_log(const char* tag, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace vocem

#define VOCEM_OVERLAY_LOG(tag, ...)                          \
    do {                                                      \
        if (::vocem::overlay_log_wanted()) {                  \
            ::vocem::overlay_log(tag, __VA_ARGS__);           \
        }                                                     \
    } while (0)

#endif  // VOCEM_OVERLAY_LOG_H
