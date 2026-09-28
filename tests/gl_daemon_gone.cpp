// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Quit means quit inside the game too: what a process hands back when the
// daemon stops.
//
// The tray's Quit stops the daemon, which publishes a cleared state and unlinks
// its segment, and the overlay leaves the screen within a second. That was the
// whole of it, and it left the interesting half undone. Measured before this
// existed, twice, in a miniature game drawing a three-person channel: the
// process sat at **139.7 MB** of Pss against **22.3 MB** for the same process
// with no overlay in it, and 1200 presents after the segment was unlinked it
// was still at **139.7 MB** -- the font atlas (16 MB alpha8 + 64 MB RGBA32 at
// 4096x4096), the renderer's objects and a texture per face, all held for the
// rest of the process's life. The log even said `state segment replaced or
// gone: detaching`: the overlay knew, and kept everything.
//
// Both injected paths now treat a daemon that went away exactly as they treat
// the switch being turned off, because from inside somebody else's process it
// is the same situation: a guest that has been asked to leave should not still
// be holding 80 MB of rasterised glyphs. The transition is reported once by
// `vocem::StatePoll::daemon_left()` -- one spelling, because two copies of that
// bookkeeping would drift -- and the cadence it arrives on is a second by the
// clock, where it used to be 300 presents, which is 2 s at 144 frames and 10 s
// at thirty.
//
// **The assertion is memory, and it has to be**: a count would not do here,
// because what was wrong was not how many times something ran but that 117 MB
// stayed mapped. The probe reads its own `/proc/self/smaps_rollup` before and
// after, in one process, so the two numbers are the same process's and nothing
// external can move them; the threshold is 40 MB against a measured 64 MB of
// atlas alone (80 MB before entry 207 freed the alpha8 image), which is far enough from the noise (the two readings before the
// fix differed by 1 kB) to mean only one thing. The log line is counted beside
// it, which is the half that says the code ran on purpose rather than by luck.
//
// **And the return leg is a count, because that is where the cost is.** The
// first version of this test drove a daemon away and brought one back and
// measured only that the overlay came back -- which it did, by rasterising the
// whole atlas again. Measured here, twice, before that was fixed: `font atlas
// built` twice across one stop-and-return, 133 ms of it, and the process 8,188
// kB heavier afterwards than it had been before, identical to the byte on both
// runs. A daemon that is REPLACED rather than stopped is not a reason to throw
// an atlas away, and `systemctl --user restart vocemd` is what CLAUDE.md's own
// delivery rule asks the owner to run with a game open. So the atlas must be
// built exactly once for the whole run, and the process must come back no
// heavier than it went.

#include <malloc.h>
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

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

unsigned char g_pixels[kWidth * kHeight * 4];

void write_file(const char* path, const char* contents) {
    if (FILE* file = fopen(path, "w")) {
        fputs(contents, file);
        fclose(file);
    }
}

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

// This process's own proportional share of memory, in kB. Pss rather than Rss
// because the atlas is this process's alone while the libraries' code pages are
// shared with every other process that has them, and it is the atlas that is
// the question.
long own_pss() {
    FILE* file = fopen("/proc/self/smaps_rollup", "r");
    if (!file) {
        return -1;
    }
    long pss = -1;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "Pss:", 4) == 0) {
            pss = atol(line + 4);
            break;
        }
    }
    fclose(file);
    return pss;
}

double seconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

// Everything in the pbuffer that is not the colour this probe cleared it to.
long foreign_pixels() {
    long count = 0;
    for (long i = 0; i < static_cast<long>(kWidth) * kHeight; ++i) {
        const int r = g_pixels[i * 4 + 0];
        const int g = g_pixels[i * 4 + 1];
        const int b = g_pixels[i * 4 + 2];
        if (r > 38 || g > 50 || b > 63 || r < 14 || g < 26 || b < 39) {
            ++count;
        }
    }
    return count;
}

}  // namespace

int main() {
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }

    // A private /dev/shm: the segment this test publishes and then unlinks must
    // never be one anything real is reading (private_shm.h carries the
    // post-mortem of the day it was).
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    vocem_test::set_alarm(120, "a daemon leaving and coming back");

    char root[] = "/tmp/vocem-gl-daemon-gone-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // The rule names THIS binary, read off /proc/self/exe: a literal is
    // wrong the day the binary is renamed and wrong at -m32 today
    // (tests/probe_name.h, entry 129).
    const std::string rule = "enabled = true\nshown_apps = " +
                             vocem_test::own_name("vocem_gl_daemon_gone") + "\n";
    write_file(path, rule.c_str());
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);
    char log_path[700];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path, 1);

    // The display height is the owner's own: it is what the atlas is sized from
    // (entry 39), and at 2160 the atlas is the 4096x4096 one whose 64 MB this
    // test is about.
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 2160;
        snprintf(state.channel_name, sizeof(state.channel_name), "quit-means-quit");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 900 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Ospite %u", i + 1);
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
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        printf("skip the driver refused a pbuffer context\n");
        return 77;
    }

    // Presents until `until`, at something like a frame rate, so the overlay's
    // once-a-second cadence is a second of real time and not a count of frames.
    const auto present_until = [&](double until) {
        while (seconds() < until) {
            glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surface);
            usleep(3000);
        }
    };

    // Presents until `ready()` answers or `deadline` passes, asking every
    // twentieth frame (~60 ms): for the waits that only wait for something to
    // happen, so they end when it has. They were fixed spans -- 5 and 3 s --
    // that the suite paid in full on every run whatever the overlay did (DESIGN
    // 193). The spans that ARE part of a claim stay spans: the first 1.5 s
    // (below), the second in which the drawing must stop, the restart's gap,
    // and the four seconds in which a release must NOT happen.
    const auto present_until_ready = [&](double deadline, const auto& ready) {
        for (int frame = 0; seconds() < deadline; ++frame) {
            glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surface);
            usleep(3000);
            if (frame % 20 == 19 && ready()) {
                return;
            }
        }
    };
    const auto panel_up = [&] {
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
        return foreign_pixels() > 10000;
    };

    // A span and not a condition, measured: the panel is up well before the
    // memory behind it is all there -- at 32 bits the first frame with a panel
    // read 308 MB of Pss where the same process holds 372 MB a second later --
    // and "what it was holding" has to be read once the holding is done, or the
    // hand-back below is a difference against a moment in the middle of a build.
    present_until(seconds() + 1.5);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    const long drawn = foreign_pixels();
    const long held = own_pss();
    const long held_heap = static_cast<long>(mallinfo2().hblkhd / 1024);
    printf("     drawing: %ld kB of Pss, %ld pixels the probe did not paint\n", held, drawn);
    check(drawn > 10000, "the overlay is drawing a panel in this process");
    check(held > 60 * 1024, "and holding an atlas' worth of memory for it");

    // Quit: exactly what vocemd does when it is told to stop -- the name goes
    // first, and the mapping this process holds outlives it (that is the whole
    // reason still_current() exists).
    printf("     >>> the daemon stops: the segment is unlinked\n");
    writer.close();
    vocem::StateWriter::unlink_segment();

    // The drawing has to stop at once; the memory is handed back after
    // StatePoll::kReleaseAfterSeconds, which exists so that a daemon merely
    // restarting does not cost an atlas. Long enough here to clear it with room
    // to spare, because a threshold sitting on its own constant is a flake.
    present_until(seconds() + 1.0);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    check(foreign_pixels() == 0, "the overlay stops drawing within a second of the daemon going");

    // The release comes StatePoll::kReleaseAfterSeconds after the poll that
    // found the segment gone -- two to three seconds after the unlink -- and
    // the line is written in the same swap that releases, just before it. So
    // the wait ends at the line (or five seconds, the old fixed span, as the
    // deadline), and a quarter of a second of frames after it lets anything the
    // driver hands back late be handed back before Pss is read.
    present_until_ready(seconds() + 5.0, [&] {
        return lines_containing(log_path, "releasing the backend and the font atlas") > 0;
    });
    present_until(seconds() + 0.25);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    const long after_drawn = foreign_pixels();
    const long after = own_pss();
    const long after_heap = static_cast<long>(mallinfo2().hblkhd / 1024);
    printf("     after the quit: %ld kB of Pss, %ld pixels the probe did not paint\n", after,
           after_drawn);

    check(after_drawn == 0, "the overlay stops drawing");
    // Two figures, and one of them is the assertion. The Pss difference is the
    // whole process's -- the driver's own mappings among it, still arriving
    // while the panel is up -- and at 32 bits under the suite's -j16 it read
    // anywhere from 55,631 to 85,521 kB over four runs, and 20,118 once beside
    // foreign load: noise the size of the claim once entry 207 took 16 MB out
    // of the atlas. The heap's mmapped blocks are the atlas and nothing the
    // driver chooses: counted exactly, as vk_present_draw counts them after
    // the last instance (entry 211). Printed both, asserted on the heap.
    printf("     handed back: %ld kB of Pss, %ld kB of mmapped heap (%ld -> %ld)\n", held - after,
           held_heap - after_heap, held_heap, after_heap);
    check(held_heap - after_heap > 40 * 1024,
          "and hands back what it was holding, instead of keeping it for the life of the process");

    const long said = lines_containing(log_path, "releasing the backend and the font atlas");
    printf("     the log says it released %ld time(s)\n", said);
    check(said == 1, "and says so once, which is what makes it deliberate");

    // And the other half of "Quit means quit": opening the window again starts
    // the daemon, and a game still running has to take the overlay back by
    // itself. Handing everything back would be a fine way to break that, so it
    // is asked here rather than hoped for.
    printf("     >>> the daemon comes back\n");
    check(writer.open(), "a second daemon publishes a segment of its own");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 2160;
        snprintf(state.channel_name, sizeof(state.channel_name), "e-torna");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 900 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Ospite %u", i + 1);
        }
    });
    present_until_ready(seconds() + 5.0, panel_up);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    const long again = foreign_pixels();
    const long back = own_pss();
    printf("     back: %ld kB of Pss, %ld pixels the probe did not paint\n", back, again);
    check(again > 10000, "the overlay comes back on its own when a daemon returns");

    // What the return cost. This daemon went away for long enough to be
    // believed, so the atlas really was handed back and really is built a second
    // time here -- one build for the first daemon, one for the second. What must
    // never happen is a third, and the leg below is the one that would have
    // produced it.
    const long built = lines_containing(log_path, "font atlas built");
    printf("     the atlas was rasterised %ld time(s) so far\n", built);
    check(built == 2, "an atlas per daemon, and no more");

    // --- and the case that is NOT a quit: `systemctl --user restart vocemd` ---
    //
    // The daemon unlinks, exits, and another takes its place a moment later.
    // From inside the game that is a different object at the same name, and the
    // whole of the difference used to be discarded at StateReader's return: the
    // reader saw "not the segment I mapped", the poll called it a departure, and
    // the game threw away an atlas it was about to need again.
    //
    // Modelled the way a restart actually looks -- gone and back well inside
    // StatePoll::kReleaseAfterSeconds -- and the assertion is that this costs
    // nothing at all: no release, no third rasterisation, and the overlay still
    // drawing at the end of it.
    printf("     >>> the daemon restarts: unlinked and replaced inside the grace\n");
    const long before_restart = own_pss();
    writer.close();
    vocem::StateWriter::unlink_segment();
    present_until(seconds() + 0.4);
    check(writer.open(), "a third daemon publishes a segment of its own");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 2160;
        snprintf(state.channel_name, sizeof(state.channel_name), "riavviato");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 950 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Ancora %u", i + 1);
        }
    });
    present_until(seconds() + 4.0);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    const long after_restart_drawn = foreign_pixels();
    const long after_restart = own_pss();
    const long rebuilt = lines_containing(log_path, "font atlas built");
    const long released = lines_containing(log_path, "releasing the backend and the font atlas");
    printf("     after the restart: %ld kB of Pss (was %ld), %ld pixels, atlas built %ld time(s), "
           "released %ld time(s)\n",
           after_restart, before_restart, after_restart_drawn, rebuilt, released);

    check(after_restart_drawn > 10000, "the overlay is still drawing across a daemon restart");
    check(rebuilt == 2, "and the restart does not rasterise the atlas again");
    check(released == 1, "nor hand anything back: a replaced daemon is not a departed one");
    // Printed, and deliberately NOT asserted on.
    //
    // It was asserted on -- "the restart cost is under 4 MB", on a reading of
    // 8,188 kB for one release-and-rebuild that had been taken twice and
    // agreed. Across a full suite it failed about one run in four, and the
    // readings show why: the same restart measures **0 kB**, **-832 kB** and
    // **-30,668 kB** on this machine. A process holding a 64 MB texture, a
    // driver and an allocator does not have a Pss that stands still between
    // two samples four seconds apart, so a 4 MB window around zero was
    // measuring the noise. Entry 31 is the rule and this test was breaking it:
    // a figure that cannot be taken twice is not one to assert on.
    //
    // What carries the claim instead is already above and is exact: the atlas
    // was rasterised twice for two daemons and not a third time, and the
    // release line appeared once. That is entry 145's own lesson -- it asserts
    // a COUNT and not a clock, for the same reason. The number stays in the
    // output because somebody reading a failure wants it.
    printf("     the restart cost %ld kB (printed, not asserted: see the comment)\n",
           after_restart - before_restart);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        printf("     (the scratch directory %s outlived the test)\n", root);
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
