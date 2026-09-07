// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocem/overlay_log.h: the injected code's one debug logger. An object of its
// own in vocem_common, so a consumer that only logs (the layer's texture cache,
// and the tests that compile it alone) pulls this and nothing of the session.

#include "vocem/overlay_log.h"

#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace vocem {

namespace {

bool debug_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("VOCEM_DEBUG");
        return env && env[0] == '1';
    }();
    return enabled;
}

// Where the debug log can actually be read. stderr is the natural home, but a
// launcher that pipes its child's stderr into an internal pane swallows it --
// the Minecraft launcher does, measured: the game's fd 2 is a pipe the launcher
// never writes anywhere readable. With VOCEM_LOG_FILE set the same lines are
// appended there too, one open per process, line-buffered, pid on every line
// because every drawing process in the session shares the one file.
std::FILE* debug_file() {
    static std::FILE* file = []() -> std::FILE* {
        const char* path = std::getenv("VOCEM_LOG_FILE");
        if (!path || !path[0]) {
            return nullptr;
        }
        std::FILE* opened = std::fopen(path, "a");
        if (opened) {
            setvbuf(opened, nullptr, _IOLBF, 0);
        }
        return opened;
    }();
    return file;
}

}  // namespace

bool overlay_log_wanted() { return debug_enabled() || debug_file() != nullptr; }

void overlay_log(const char* tag, const char* format, ...) {
    if (debug_enabled()) {
        std::va_list arguments;
        va_start(arguments, format);
        std::fprintf(stderr, "[%s] ", tag);
        std::vfprintf(stderr, format, arguments);
        std::fputc('\n', stderr);
        va_end(arguments);
    }
    if (std::FILE* file = debug_file()) {
        std::va_list arguments;
        va_start(arguments, format);
        std::fprintf(file, "[%s %d] ", tag, static_cast<int>(::getpid()));
        std::vfprintf(file, format, arguments);
        std::fputc('\n', file);
        va_end(arguments);
    }
}

}  // namespace vocem
