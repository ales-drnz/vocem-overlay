// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A toast must reach the screen when nobody is in a voice channel.
//
// The notification is documented as a feature of its own -- vocem/panel.h:
// "either can be switched off without the other" -- and a "somebody wrote to
// you" toast matters most precisely when you are not already in the channel.
// Both injection paths guarded the whole frame with "in a voice channel with
// somebody", written once per path, so build_notification was unreachable in
// exactly that situation: a DM outside a voice channel drew nothing, ever,
// with nothing logged. Found by a fresh-context audit reading the guards
// against panel.h's promise, not by a user report -- an unreachable feature
// looks identical to an idle one from outside.
//
// Same miniature game as gl_draw_local.cpp -- GL behind dlopen(RTLD_LOCAL), a
// private /dev/shm, the front buffer read back -- but the state it publishes
// is a fresh notification and an empty channel. Against the library shipped in
// 0.1.0-50 the foreign pixel count is zero.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"

#include <string>
#include "vocem/note.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;
using vocem_test::write_file;

namespace {

double monotonic_now() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

using PFN_glXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFN_glXCreateContext = void* (*)(Display*, XVisualInfo*, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXDestroyContext = void (*)(Display*, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_glClearColor = void (*)(float, float, float, float);
using PFN_glClear = void (*)(unsigned int);
using PFN_glReadBuffer = void (*)(unsigned int);
using PFN_glReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int, void*);

}  // namespace

int main() {
    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no GLX drawable to draw into\n");
        return 77;
    }
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }

    // Isolated or not is answered by looking at /dev/shm, never by a variable
    // this process could have been handed: a forged sentinel once let a probe
    // publish its fake channel into the live daemon's segment while the owner
    // was playing (private_shm.h tells that story). The variable now only says
    // whether the sandbox has already been attempted.
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    vocem_test::set_alarm(60, "a toast outside a channel");

    char root[] = "/tmp/vocem-toast-alone-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // A long toast, so the read-back at frame 45 is nowhere near its exit.
    // The rule names THIS binary, read off /proc/self/exe (probe_name.h,
    // entry 129).
    const std::string rule = "enabled = true\nshown_apps = " +
                             vocem_test::own_name("vocem_gl_toast_alone") +
                             "\nnotification_seconds = 30\n";
    write_file(path, rule.c_str());
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);

    // The state a DM arriving at the desktop produces: connected, NOT in any
    // voice channel, and one fresh notification.
    vocem::StateWriter writer;
    vocem::NoteWriter note;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 0;
        state.status = 2;  // Connected
        state.user_count = 0;
        state.notification.serial = 1;
        state.notification.user_id = 700;
        state.notification.received = monotonic_now();
        snprintf(state.notification.title, sizeof(state.notification.title), "Somebody");
        // Never here: the state segment carries who wrote, never what.
        state.notification.body[0] = '\0';
    });

    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gl) {
        printf("skip libGL.so.1 is not installed\n");
        return 77;
    }
    auto* choose = reinterpret_cast<PFN_glXChooseVisual>(dlsym(gl, "glXChooseVisual"));
    auto* create = reinterpret_cast<PFN_glXCreateContext>(dlsym(gl, "glXCreateContext"));
    auto* make_current = reinterpret_cast<PFN_glXMakeCurrent>(dlsym(gl, "glXMakeCurrent"));
    auto* destroy = reinterpret_cast<PFN_glXDestroyContext>(dlsym(gl, "glXDestroyContext"));
    auto* swap = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(gl, "glXSwapBuffers"));
    auto* clear_colour = reinterpret_cast<PFN_glClearColor>(dlsym(gl, "glClearColor"));
    auto* clear = reinterpret_cast<PFN_glClear>(dlsym(gl, "glClear"));
    auto* read_buffer = reinterpret_cast<PFN_glReadBuffer>(dlsym(gl, "glReadBuffer"));
    auto* read_pixels = reinterpret_cast<PFN_glReadPixels>(dlsym(gl, "glReadPixels"));
    check(choose && create && make_current && destroy && swap && clear_colour && clear &&
              read_buffer && read_pixels,
          "every GL function resolves off the private handle");
    if (failures) {
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip the display did not open from inside the sandbox\n");
        return 77;
    }
    int attrs[] = {4 /*GLX_RGBA*/, 5 /*GLX_DOUBLEBUFFER*/, 0};
    XVisualInfo* visual = choose(display, DefaultScreen(display), attrs);
    check(visual != nullptr, "a double-buffered visual exists");
    if (!visual) {
        return 1;
    }
    const int W = 360, H = 360;
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(display, RootWindow(display, visual->screen), visual->visual,
                                   AllocNone);
    // Override-redirect and parked far off screen: nothing appears on the
    // desktop and no focus is taken -- the owner may be in a game while this
    // runs. Composited windows render into their own buffer, so the
    // front-buffer read-back still sees the overlay.
    swa.override_redirect = True;
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, W, H, 0,
                                  visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWOverrideRedirect, &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    check(context != nullptr, "and a context comes up");
    make_current(display, window, context);

    // Two lives of the same toast: with the message's text, and with the body
    // the daemon publishes when the user has not opted in -- empty. The old
    // version of this test counted foreign pixels once, which a textless box
    // passes: a toast that lost its words was indistinguishable from a whole
    // one. The instrument is the differential (colour detectors do not work on
    // captures): same scene, same box -- the box's height is floored by the
    // avatar, measured, so it does not shrink -- and the only thing that
    // changes between the phases is the body, so a stable surplus of foreign
    // pixels in the first is the body's glyphs and nothing else. A library
    // that draws the box and loses the words measures zero surplus.
    static unsigned char pixels[360 * 360 * 4];
    long foreign[2] = {0, 0};
    int busy_rows[2] = {0, 0};
    for (int phase = 0; phase < 2; ++phase) {
        // The words go where the daemon puts them -- the note segment, which
        // the library opens only because it is about to draw this toast -- and
        // never into the state segment, which is the whole point of that
        // arrangement (vocem/note.h). Phase 1 publishes no note at all, which
        // is the shape of a message whose toast has already outlived it.
        writer.publish([phase](vocem::SharedState& state) {
            state.notification.serial = phase + 1;
            state.notification.received = monotonic_now();
            snprintf(state.notification.title, sizeof(state.notification.title), "Somebody");
            state.notification.body[0] = '\0';
        });
        if (phase == 0) {
            note.publish(1, "wrote to you");
        } else {
            note.clear();
        }
        for (int frame = 0; frame < 45; ++frame) {
            clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
            clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
            swap(display, window);
            usleep(16000);
        }
        read_buffer(0x0404 /*GL_FRONT*/);
        read_pixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels);
        for (int row = 0; row < H; ++row) {
            long in_row = 0;
            for (int column = 0; column < W; ++column) {
                const long i = (long)row * W + column;
                const unsigned char r = pixels[i * 4 + 0];
                const unsigned char g = pixels[i * 4 + 1];
                const unsigned char b = pixels[i * 4 + 2];
                const int dr = r > 26 ? r - 26 : 26 - r;
                const int dg = g > 38 ? g - 38 : 38 - g;
                const int db = b > 51 ? b - 51 : 51 - b;
                if (dr > 40 || dg > 40 || db > 40) {
                    ++in_row;
                }
            }
            foreign[phase] += in_row;
            if (in_row > 3) {
                ++busy_rows[phase];
            }
        }
        printf("     phase %d (%s): foreign pixels %ld, busy rows %d\n", phase,
               phase == 0 ? "with body" : "title only", foreign[phase], busy_rows[phase]);
    }
    check(foreign[0] > 500, "the toast reached the screen with nobody in a voice channel");
    check(foreign[1] > 350, "the title-only toast reached the screen too");
    check(foreign[0] >= foreign[1] + 50,
          "the body's glyphs are in the frame: the with-body capture carries more ink");

    destroy(display, context);
    XCloseDisplay(display);
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
