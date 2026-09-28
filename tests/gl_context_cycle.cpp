// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What a GL CONTEXT costs, which nothing here measured before.
//
// The project counts what a frame costs (gl_noop_quiet: X requests per present
// in a process the overlay declines) and what an ask costs (apps_cost:
// allocations per verdict). Between them sits the thing a game actually does
// that neither one sees: destroy the context it was drawing in and make
// another. A loading screen does it, a renderer that switches API does it,
// every Qt application on Wayland does it, and the overlay charged for it
// twice over.
//
//   * **It crashed.** Reproduced 3/3 in 0.1.10-1: the second context's first
//     present died with SIGSEGV in ImGui::SetCurrentFont, called from
//     build_notification's PushFont. Entry 37's dead band recognised a fresh
//     atlas by it being EMPTY, and since 0.1.8 the GL path builds its
//     renderer's device objects -- font texture included -- inside
//     ensure_backend(), before the fonts are asked for. Building the font
//     texture builds the atlas, which puts ImGui's default font in it, so the
//     fresh atlas was never empty and the dead band held: fonts().body went on
//     pointing into the atlas that died with the first context. The address had
//     even been recycled, which is why nothing reported anything.
//     tests/fonts_lifecycle.cpp holds that half without a GL context.
//   * **It rebuilt the world.** Every context paid the whole atlas again:
//     122 ms to rasterise two weights of ~21,000 codepoints into 4096x4096 and
//     11 ms to widen it to RGBA (measured twice, common/src/fonts.cpp's own
//     sizes), against ~1 ms for the same present with VOCEM_DISABLE=1.
//
// **The witness is a count, not a clock.** The wall time of a present on this
// machine is a GPU's answer as much as ours -- the same first present measured
// 139 to 206 ms across runs -- so an assertion on it would be a threshold
// nobody could defend. The overlay says "font atlas built" once per
// rasterisation instead (VOCEM_DEBUG, into VOCEM_LOG_FILE), and this counts the
// lines: one for however many contexts the game goes through. The times are
// printed beside it because the number belongs in the record, and asserted
// nowhere.
//
// Two scenarios on one probe, both cycling four EGL contexts:
//
//   * default -- the configuration names this probe, so the overlay draws. It
//     must survive every cycle, and build its atlas once.
//   * VOCEM_GL_SCENARIO=declining -- the configuration names somebody else. The
//     overlay must decide once and then cost nothing per context: no backend,
//     no atlas, and not a second verdict.
//
// EGL with a pbuffer rather than GLX with a window: the crash arrived through
// eglSwapBuffers, no window is needed for it, and the owner may be in a game
// while this runs.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"

#include <string>
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// How many contexts the game goes through, and how many frames it presents in
// each. Two frames would do -- the panel is built from the first -- and three
// keeps the count independent of that detail.
constexpr int kCycles = 4;
constexpr int kFrames = 3;
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

unsigned char g_pixels[kWidth * kHeight * 4];

// Everything in the pbuffer that is not the colour the probe cleared it to. The
// only other thing drawing in here is the overlay.
long foreign_pixels() {
    long count = 0;
    for (long i = 0; i < static_cast<long>(kWidth) * kHeight; ++i) {
        const int r = g_pixels[i * 4 + 0];
        const int g = g_pixels[i * 4 + 1];
        const int b = g_pixels[i * 4 + 2];
        if (r > 26 + 12 || r < 26 - 12 || g > 38 + 12 || g < 38 - 12 || b > 51 + 12 ||
            b < 51 - 12) {
            ++count;
        }
    }
    return count;
}

void write_file(const char* path, const char* contents) {
    if (FILE* file = fopen(path, "w")) {
        fputs(contents, file);
        fclose(file);
    }
}

// How many lines of a file contain `needle`.
long lines_containing(const char* path, const char* needle) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            ++count;
        }
    }
    fclose(file);
    return count;
}

double milliseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * 1000.0 +
           static_cast<double>(now.tv_nsec) / 1e6;
}

}  // namespace

int main() {
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "";
    const bool declining = strcmp(scenario, "declining") == 0;
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }

    // A private /dev/shm, so the channel published here can never be mistaken
    // for the daemon's by anything real -- answered by looking at /dev/shm and
    // never by a variable this process could have been handed (private_shm.h).
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    vocem_test::set_alarm(120, "four GL contexts");

    // A world of this test's own. `shown_apps` is the whole difference between
    // the two scenarios: a probe is not a game, so without being named it is
    // declined, which is exactly the process the second half measures.
    char root[] = "/tmp/vocem-gl-context-cycle-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // The rule names THIS binary, read off /proc/self/exe (probe_name.h,
    // entry 129) -- or, in the declining scenario, deliberately somebody else.
    const std::string rule =
        "enabled = true\nshown_apps = " +
        (declining ? std::string("somebody_else") :
                     vocem_test::own_name("vocem_gl_context_cycle")) + "\n";
    write_file(path, rule.c_str());
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);
    char log_path[700];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path, 1);

    // The channel, published by this process: the reader in the overlay cannot
    // tell it from the daemon, which is the point. The display height is the
    // owner's own, because it is what the atlas is sized from (entry 39) and a
    // rebuild is the thing being counted -- at 2160 the atlas is the 4096x4096
    // one fonts.cpp measures, not a small one nobody would notice.
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 2160;
        snprintf(state.channel_name, sizeof(state.channel_name), "context-cycle");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 700 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Cycle %u", i + 1);
        }
    });

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        printf("skip no EGL display from inside the sandbox\n");
        return 77;
    }
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                   EGL_RED_SIZE,        8,
                                   EGL_GREEN_SIZE,      8,
                                   EGL_BLUE_SIZE,       8,
                                   EGL_ALPHA_SIZE,      8,
                                   EGL_NONE};
    EGLConfig config;
    EGLint config_count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &config_count) || config_count == 0) {
        printf("skip no pbuffer configuration on this driver\n");
        return 77;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint surface_attrs[] = {EGL_WIDTH, kWidth, EGL_HEIGHT, kHeight, EGL_NONE};
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

    // Four whole lives of a GL context, each one created, presented in and
    // destroyed. Against 0.1.10-1 the second one does not return from its first
    // eglSwapBuffers.
    double first_present[kCycles] = {};
    long drawn[kCycles] = {};
    int survived = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
        EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT) {
            printf("skip the driver refused a pbuffer context\n");
            return 77;
        }
        if (!eglMakeCurrent(display, surface, surface, context)) {
            printf("skip the driver refused to make the pbuffer context current\n");
            return 77;
        }
        for (int frame = 0; frame < kFrames; ++frame) {
            glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const double before = milliseconds();
            eglSwapBuffers(display, surface);
            const double after = milliseconds();
            if (frame == 0) {
                first_present[cycle] = after - before;
            }
        }
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
        drawn[cycle] = foreign_pixels();
        // The FIRST context waits for its panel. The first atlas of a process is
        // rasterised on a worker now (entry 192), so the panel appears about a
        // tenth of a second after the first frame with a channel on it instead
        // of the game standing still for it -- three frames are no longer
        // enough, and the later contexts are compared against this one, so a
        // first context read before its panel would make every comparison
        // below pass on nothing. Presented until the panel is there, two
        // seconds at most. The later contexts get their three frames as before:
        // the atlas outlives the context, and their panel must be there at once.
        if (cycle == 0 && !declining) {
            const double deadline = milliseconds() + 2000.0;
            while (drawn[0] <= 10000 && milliseconds() < deadline) {
                glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                eglSwapBuffers(display, surface);
                glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
                drawn[0] = foreign_pixels();
            }
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        ++survived;
    }
    check(survived == kCycles,
          "the process lives through every context it creates and destroys");

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        printf("     cycle %d: first present %.1f ms, %ld pixels the probe did not paint\n", cycle,
               first_present[cycle], drawn[cycle]);
    }

    const long ready = lines_containing(log_path, "OpenGL backend ready");
    const long built = lines_containing(log_path, "font atlas built");
    // Both spellings of the verdict end in "drawing in '", which is what
    // gl_noop_quiet matches too: one line means the decision was taken once and
    // not once per context.
    const long verdicts = lines_containing(log_path, "drawing in '");
    printf("     the log says: backend ready %ld, atlas built %ld, verdict %ld\n", ready, built,
           verdicts);
    check(verdicts == 1, "the verdict is taken once, not once per context");

    if (declining) {
        check(lines_containing(log_path, "not drawing in '") == 1,
              "the declining process says it is not drawing here");
        check(ready == 0, "and never brings a backend up");
        check(built == 0, "and never rasterises an atlas");
    } else {
        check(ready == kCycles, "a backend comes up in each context, since each one is new");
        check(built == 1, "and the atlas is rasterised once for all of them");
        // What actually reached the pbuffer, which is the check that does not
        // depend on the allocator's mood. A dangling font pointer is a crash
        // only when the freed memory has been reused in the wrong way -- it was
        // 3/3 in the probe this was found with and 0/4 here -- but the overlay
        // draws from a dead atlas either way, and that is visible: against
        // 0.1.10-1 the first context put 19,603 pixels on the pbuffer and every
        // later one 5,376, the panel's furniture with none of its text.
        check(drawn[0] > 10000, "the overlay drew a panel in the first context");
        for (int cycle = 1; cycle < kCycles; ++cycle) {
            check(drawn[cycle] > drawn[0] - drawn[0] / 10,
                  "and draws the same panel in every context after it");
        }
    }

    eglTerminate(display);
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        printf("     (the scratch directory %s outlived the test)\n", root);
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
