// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the detection costs in the process that pays it -- which is every OpenGL
// and Vulkan process in the session, the compositor and the portals and the
// browser included, not only the games.
//
// `tests/gl_noop_quiet.cpp` holds "this costs nothing when it is switched off".
// There was no equivalent for "this costs nothing when it decided no", and the
// page and the header both make that claim: the verdict is worked out once, the
// record is written once, and the per-frame ask is "two string comparisons".
//
// Allocations are what is asserted, because they are the same number on every
// machine and a wall clock is not: the steady-state ask must not allocate, which
// is exactly what would break if anybody made DrawDecision::refresh call
// draw_here() again -- draw_here builds two std::strings out of
// /proc/self/exe every time it is asked. The timings are measured and printed
// beside them, because a number nobody prints is a number nobody checks.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <new>

#include <sys/stat.h>
#include <unistd.h>

#include "vocem/draw_decision.h"

namespace {

int failures = 0;
long allocations = 0;
bool counting = false;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The window in which allocations are counted, so that the printf of the result
// is never itself part of the measurement.
struct Counted {
    Counted() {
        allocations = 0;
        counting = true;
    }
    long done() {
        counting = false;
        return allocations;
    }
};

using Clock = std::chrono::steady_clock;

double microseconds(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::micro>(to - from).count();
}

}  // namespace

void* operator new(size_t size) {
    if (counting) {
        ++allocations;
    }
    void* memory = std::malloc(size ? size : 1);
    if (!memory) {
        throw std::bad_alloc();
    }
    return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }

int main() {
    // A probe is not a game and must not be treated as one: no launcher in the
    // environment, no entry naming this binary.
    for (const char* variable : {"SteamAppId", "SteamGameId", "GAMEID", "LUTRIS_GAME_UUID",
                                 "HEROIC_APP_NAME", "INST_MC_DIR", "ITCHIO_APP",
                                 "ENABLE_GAMESCOPE_WSI", "FLATPAK_ID",
                                 "GIO_LAUNCHED_DESKTOP_FILE"}) {
        ::unsetenv(variable);
    }
    char root[] = "/tmp/vocem-apps-cost-XXXXXX";
    if (!::mkdtemp(root)) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    ::setenv("XDG_CACHE_HOME", root, 1);
    ::setenv("XDG_CONFIG_HOME", root, 1);

    // 1. The verdict, which is what a process pays for being asked at all: its
    //    own name, its own executable, its command line, a cgroup line and a
    //    handful of stat() calls looking for a desktop entry. This probe is the
    //    expensive case on purpose -- nothing names it, so it falls all the way
    //    through to the pass over the installed entries. A process a launcher
    //    said something about pays a fraction of it.
    const Clock::time_point before_verdict = Clock::now();
    const bool game = vocem::looks_like_game();
    const double verdict_us = microseconds(before_verdict, Clock::now());
    check(!game, "a probe is not a game, so this is the cost of deciding no");
    std::printf("     the verdict, once:            %.1f us (%s)\n", verdict_us,
                vocem::game_verdict().reason.c_str());

    {
        Counted counted;
        for (int repeat = 0; repeat < 1000; ++repeat) {
            vocem::looks_like_game();
        }
        check(counted.done() == 0, "and asking again is free: it is decided once");
    }

    // 2. The record, which every process writes whether it draws or not -- the
    //    directory, a file, and a rename.
    const Clock::time_point before_record = Clock::now();
    vocem::record_application("opengl");
    const double record_us = microseconds(before_record, Clock::now());
    std::printf("     the record, once:             %.1f us\n", record_us);

    {
        Counted counted;
        for (int repeat = 0; repeat < 1000; ++repeat) {
            vocem::record_application("opengl");
        }
        check(counted.done() == 0, "written once, not once per frame, and not re-decided either");
    }

    // 3. The per-frame ask, which is the one that happens at the frame rate. The
    //    lists are only walked when they were edited; everything else is two
    //    string comparisons against what was answered last time.
    vocem::Config config;
    vocem::DrawDecision decision;
    check(decision.refresh(config), "the first ask computes and says so");
    {
        Counted counted;
        const Clock::time_point before = Clock::now();
        constexpr int frames = 200000;
        for (int frame = 0; frame < frames; ++frame) {
            decision.refresh(config);
        }
        const double each = microseconds(before, Clock::now()) * 1000.0 / frames;
        const long allocated = counted.done();
        std::printf("     the ask, per frame:           %.1f ns\n", each);
        check(allocated == 0, "and it allocates nothing: no /proc, no strings, no lists walked");
    }

    // 4. An edited list is the only thing that recomputes -- and it must, in the
    //    running process, which is what the page promises.
    config.shown_apps = vocem::process_name();
    check(!decision.refresh(config), "an edited list recomputes without claiming to be the first");
    check(decision.allowed(), "and the running process follows it");

    vocem::forget_applications();
    ::rmdir((std::string(root) + "/vocem/apps").c_str());
    ::rmdir((std::string(root) + "/vocem").c_str());
    ::rmdir(root);

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
