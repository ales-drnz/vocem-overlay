// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The injected code's debug log, for both paths: the tag is the caller's, and
// VOCEM_DEBUG and VOCEM_LOG_FILE are read once per process.
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
// "[<tag> <pid>] <message>" appended to VOCEM_LOG_FILE when that is set. Each
// is formatted on the stack and written whole; a message is cut at about a
// kilobyte. No allocation, no lock.
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
