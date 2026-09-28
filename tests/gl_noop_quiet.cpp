// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A process the overlay declines to draw in must be quiet on the X wire.
//
// The GL present hook used to ask the drawable's size -- two glXQueryDrawable,
// each an X protocol request -- before asking whether this process is one the
// overlay draws in at all. Every GL process of the session paid it, every
// frame, forever: measured on the library shipped in 0.1.0-50, the no-op path
// put 3 X requests on the wire per swap where VOCEM_DISABLE=1 put 0, and each
// glXQueryDrawable pair costs ~33 us of round trip on this machine. The size
// is now queried only after a frame has decided it will draw.
//
// Wall-clock timing of a swap is compositor noise; X request serials are not.
// The measurement: NextRequest() around a swap loop, in two child processes of
// this test -- one with the overlay disabled (the baseline: whatever the
// driver itself puts on the wire), one with it active but declining (this
// binary is not a game and is not in shown_apps). The two counts must match:
// declining must cost nothing the disabled path does not.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <GL/glx.h>
#include <X11/Xlib.h>

#include "probe_alarm.h"
#include "vocem_check.h"
#include "gl_window.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// Child mode: count X requests across a swap loop and print the per-frame rate.
int count_requests() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        return 2;
    }
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, None};
    XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), attrs);
    if (!vi) {
        return 2;
    }
    Window win = vocem_test::offscreen_window(dpy, vi, 320, 240);
    GLXContext ctx = glXCreateContext(dpy, vi, nullptr, True);
    if (!ctx) {
        return 2;
    }
    glXMakeCurrent(dpy, win, ctx);

    // Warm up past the overlay's own first-frame work (loading the heavy
    // library, the one-off decision, the application record).
    for (int i = 0; i < 60; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, win);
    }

    enum { FRAMES = 500 };
    const unsigned long before = NextRequest(dpy);
    for (int i = 0; i < FRAMES; ++i) {
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, win);
    }
    const unsigned long after = NextRequest(dpy);
    printf("%.3f\n", static_cast<double>(after - before) / FRAMES);

    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}

// Parent mode: run self in child mode with a chosen environment, read the rate.
// Started and read in two halves so the two children run at once: each counts
// the requests on its own X connection, so a neighbour cannot move its figure,
// and the 1120 vsync-paced swaps of the two -- 8 s one after the other, which
// was this test's whole cost -- overlap instead (DESIGN 193).
FILE* start_child(const char* self, const char* environment) {
    char command[4400];
    snprintf(command, sizeof(command), "%s %s --count-requests", environment, self);
    return popen(command, "r");
}

bool child_rate(FILE* pipe, double& rate) {
    if (!pipe) {
        return false;
    }
    char line[64] = {0};
    const bool got = fgets(line, sizeof(line), pipe) != nullptr;
    const int status = pclose(pipe);
    if (!got || status != 0) {
        return false;
    }
    rate = atof(line);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--count-requests") == 0) {
        return count_requests();
    }

    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no X wire to keep quiet\n");
        return 77;
    }
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }

    // A config of this test's own, with this binary NOT in shown_apps: the case
    // under measurement is precisely "the overlay declines".
    char root[] = "/tmp/vocem-noop-quiet-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    if (FILE* file = fopen(path, "w")) {
        fputs("enabled = true\n", file);
        fclose(file);
    }
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);

    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        printf("FAIL cannot find my own binary\n");
        return 1;
    }
    self[n] = '\0';

    vocem_test::set_alarm(120, "a declining process's per-frame cost");

    double disabled = 0.0;
    double declining = 0.0;
    // The declining child also logs to a file, which is the positive control:
    // "declining" and "disabled" make the same silence on the X wire, so
    // without this the comparison below is satisfied by a library that never
    // loaded at all (shim_disable.cpp carries the same control for its own
    // comparison). The line matched is "not drawing in" whole -- entry 54's
    // probe once matched "drawing in" inside it and reported the opposite.
    char decline_env[900];
    char decline_log[700];
    snprintf(decline_log, sizeof(decline_log), "%s/decline.log", root);
    snprintf(decline_env, sizeof(decline_env), "VOCEM_DEBUG=1 VOCEM_LOG_FILE=%s", decline_log);
    FILE* disabled_child = start_child(self, "VOCEM_DISABLE=1");
    FILE* declining_child = start_child(self, decline_env);
    const bool disabled_ran = child_rate(disabled_child, disabled);
    const bool declining_ran = child_rate(declining_child, declining);
    if (!disabled_ran) {
        printf("skip the disabled baseline could not run (no GLX here?)\n");
        return 77;
    }
    if (!declining_ran) {
        printf("FAIL the active child could not run\n");
        return 1;
    }
    bool declined_on_record = false;
    if (FILE* log = fopen(decline_log, "r")) {
        char line[512];
        while (fgets(line, sizeof(line), log)) {
            if (strstr(line, "not drawing in")) {
                declined_on_record = true;
                break;
            }
        }
        fclose(log);
    }
    check(declined_on_record,
          "the declining child loaded the overlay and said why it declined");
    printf("     X requests per frame: disabled %.3f, declining %.3f\n", disabled, declining);
    check(declining <= disabled + 0.01,
          "a declined process puts nothing on the X wire the disabled one does not");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        // best-effort scratch cleanup
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
