// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocem/overlay_log.h: the injected code's one debug logger. An object of its
// own in vocem_common, so a consumer that only logs (the layer's texture cache,
// and the tests that compile it alone) pulls this and nothing of the session.

#include "vocem/overlay_log.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vocem {

namespace {

bool debug_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("VOCEM_DEBUG");
        return env && env[0] == '1';
    }();
    return enabled;
}

// All of one line, in one write(2), or as few as the kernel allows. A line
// is formatted whole into the caller's stack first: the logger used to spend
// three stdio calls per line (prefix, message, newline) with nothing between
// two threads, and stderr being unbuffered each was a write of its own --
// 56-58 thousand of 200000 lines malformed with two threads logging, and the
// file tore too (tests/overlay_log_lines.cpp). One write per line to an
// O_APPEND file is also what keeps two PROCESSES sharing the file from
// interleaving inside a line; to a pipe, a line under PIPE_BUF is atomic.
void write_whole(int fd, const char* text, size_t length) {
    while (length > 0) {
        const ssize_t written = ::write(fd, text, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        text += written;
        length -= static_cast<size_t>(written);
    }
}

// Where the debug log can actually be read. stderr is the natural home, but a
// launcher that pipes its child's stderr into an internal pane swallows it --
// the Minecraft launcher does, measured: the game's fd 2 is a pipe the launcher
// never writes anywhere readable. With VOCEM_LOG_FILE set the same lines are
// appended there too, one open per process, pid on every line because every
// drawing process in the session shares the one file. A descriptor, not a
// FILE: O_APPEND for the sharing above, O_CLOEXEC so it does not ride into
// everything the game execs (entry 98's reason for the journal's). A path
// that cannot be opened is said once, on stderr, whether or not VOCEM_DEBUG
// is set: the variable is set precisely when stderr is the only other place
// anything can be read, and silence there is the defect entry 38 describes.
int debug_file() {
    static const int fd = []() -> int {
        const char* path = std::getenv("VOCEM_LOG_FILE");
        if (!path || !path[0]) {
            return -1;
        }
        const int opened =
            ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOCTTY, 0600);
        if (opened < 0) {
            char line[640];
            const int length = std::snprintf(
                line, sizeof(line), "[vocem %d] VOCEM_LOG_FILE=%s cannot be opened: %s\n",
                static_cast<int>(::getpid()), path, std::strerror(errno));
            if (length > 0) {
                write_whole(2, line,
                            static_cast<size_t>(length) < sizeof(line) ? static_cast<size_t>(length)
                                                                        : sizeof(line) - 1);
            }
        }
        return opened;
    }();
    return fd;
}

// A line cut to this many bytes, newline included. Under PIPE_BUF (4096) so a
// line to a pipe is one atomic write; far over anything the overlay says.
constexpr size_t kLineCapacity = 1024;

// "<prefix><message>\n" into `line`, the message cut rather than overflowed.
size_t format_line(char* line, const char* prefix, const char* format, std::va_list arguments) {
    int used = std::snprintf(line, kLineCapacity, "%s", prefix);
    if (used < 0) {
        used = 0;
    }
    size_t length = static_cast<size_t>(used) < kLineCapacity - 1 ? static_cast<size_t>(used)
                                                                   : kLineCapacity - 2;
    const int message = std::vsnprintf(line + length, kLineCapacity - 1 - length, format, arguments);
    if (message > 0) {
        length += static_cast<size_t>(message) < kLineCapacity - 1 - length
                      ? static_cast<size_t>(message)
                      : kLineCapacity - 2 - length;
    }
    line[length++] = '\n';
    return length;
}

}  // namespace

bool overlay_log_wanted() { return debug_enabled() || debug_file() >= 0; }

void overlay_log(const char* tag, const char* format, ...) {
    char line[kLineCapacity];
    char prefix[96];
    if (debug_enabled()) {
        std::snprintf(prefix, sizeof(prefix), "[%s] ", tag);
        std::va_list arguments;
        va_start(arguments, format);
        const size_t length = format_line(line, prefix, format, arguments);
        va_end(arguments);
        write_whole(2, line, length);
    }
    if (const int fd = debug_file(); fd >= 0) {
        std::snprintf(prefix, sizeof(prefix), "[%s %d] ", tag, static_cast<int>(::getpid()));
        std::va_list arguments;
        va_start(arguments, format);
        const size_t length = format_line(line, prefix, format, arguments);
        va_end(arguments);
        write_whole(fd, line, length);
    }
}

}  // namespace vocem
