// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// VOCEM_CAPTURE_FRAME reads the frame back through the game's state, not ours.
//
// The capture is the one honest check of what the overlay looks like (CLAUDE.md,
// "Draw verification"), and it was a glReadPixels into a width*height*4 buffer
// with nothing around it. glReadPixels reads from the READ framebuffer, and
// writes through GL_PACK_* and the GL_PIXEL_PACK_BUFFER binding -- all of it
// the game's state, surviving the swap:
//
//   * `read-framebuffer`: the game leaves its own framebuffer object bound for
//     reading (a post-process chain that blits from it) and GL_PACK_ROW_LENGTH
//     at 4096 with a few skips. The overlay draws into framebuffer 0 and the
//     capture read the game's object instead -- and wrote rows 16 KB apart into
//     a buffer sized for 1440-byte rows, far past its end, in the game.
//   * `pack-buffer`: the game leaves a 16-byte pixel-pack buffer bound. The
//     pointer became an offset into it: GL_INVALID_OPERATION in the game's
//     queue, and a capture of whatever the heap held.
//
// Both scenes present with a channel on screen until the overlay says it
// captured, then require: a capture with the frame the game presents in it --
// mostly its clear colour, and the overlay's pixels -- the game's read binding,
// pack state and pack buffer exactly as it left them, and no GL error. The
// defect was found by reading, not in a game; the scenes are what it predicts.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

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

double now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) / 1e6;
}

using PFN_glXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFN_glXCreateContext = void* (*)(Display*, XVisualInfo*, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_proc = void* (*)(const unsigned char*);

constexpr unsigned kReadFramebuffer = 0x8CA8;
constexpr unsigned kDrawFramebuffer = 0x8CA9;
constexpr unsigned kReadFramebufferBinding = 0x8CAA;
constexpr unsigned kPixelPackBuffer = 0x88EB;
constexpr unsigned kPixelPackBufferBinding = 0x88ED;
constexpr unsigned kPackAlignment = 0x0D05;
constexpr unsigned kPackRowLength = 0x0D02;
constexpr unsigned kPackSkipRows = 0x0D03;
constexpr unsigned kPackSkipPixels = 0x0D04;

}  // namespace

int main() {
    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no GLX drawable\n");
        return 77;
    }
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "";
    const bool pack_buffer = strcmp(scenario, "pack-buffer") == 0;
    if (!pack_buffer && strcmp(scenario, "read-framebuffer") != 0) {
        printf("FAIL VOCEM_GL_SCENARIO is neither read-framebuffer nor pack-buffer\n");
        return 1;
    }
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(30, "the capture of a frame");

    char root[] = "/tmp/vocem-gl-capture-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule =
        "enabled = true\nshown_apps = " + vocem_test::own_name("vocem_gl_capture_state") + "\n";
    if (FILE* file = fopen(path, "w")) {
        fputs(rule.c_str(), file);
        fclose(file);
    }
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);
    static char log_path[700];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path, 1);
    static char capture_path[700];
    snprintf(capture_path, sizeof(capture_path), "%s/capture.ppm", root);
    setenv("VOCEM_CAPTURE_FRAME", capture_path, 1);

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "capture");
        state.user_count = 2;
        for (uint32_t i = 0; i < 2; ++i) {
            state.users[i].id = 700 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Capture %u", i + 1);
        }
    });

    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gl) {
        printf("skip libGL.so.1 is not installed\n");
        return 77;
    }
    auto* choose = reinterpret_cast<PFN_glXChooseVisual>(dlsym(gl, "glXChooseVisual"));
    auto* create = reinterpret_cast<PFN_glXCreateContext>(dlsym(gl, "glXCreateContext"));
    auto* make_current = reinterpret_cast<PFN_glXMakeCurrent>(dlsym(gl, "glXMakeCurrent"));
    auto* swap = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(gl, "glXSwapBuffers"));
    auto* proc = reinterpret_cast<PFN_proc>(dlsym(gl, "glXGetProcAddressARB"));
    if (!choose || !create || !make_current || !swap || !proc) {
        printf("FAIL GLX does not resolve off the private handle\n");
        return 1;
    }
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip the display did not open\n");
        return 77;
    }
    int attributes[] = {4 /*GLX_RGBA*/, 5 /*GLX_DOUBLEBUFFER*/, 0};
    XVisualInfo* visual = choose(display, DefaultScreen(display), attributes);
    if (!visual) {
        printf("skip no double-buffered visual\n");
        return 77;
    }
    constexpr int W = 360;
    constexpr int H = 360;
    XSetWindowAttributes swa;
    swa.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    // Override-redirect and parked off screen: nothing appears, no focus.
    swa.override_redirect = True;
    const Window window =
        XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, W, H, 0,
                      visual->depth, InputOutput, visual->visual, CWColormap | CWOverrideRedirect,
                      &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    if (!context || !make_current(display, window, context)) {
        printf("skip no GLX context\n");
        return 77;
    }

#define GL(type, name) auto* name = reinterpret_cast<type>(proc(reinterpret_cast<const unsigned char*>(#name)))
    GL(void (*)(float, float, float, float), glClearColor);
    GL(void (*)(unsigned), glClear);
    GL(void (*)(unsigned, int), glPixelStorei);
    GL(void (*)(unsigned, int*), glGetIntegerv);
    GL(unsigned (*)(), glGetError);
    GL(void (*)(int, unsigned*), glGenFramebuffers);
    GL(void (*)(unsigned, unsigned), glBindFramebuffer);
    GL(void (*)(unsigned, unsigned, unsigned, unsigned, int), glFramebufferTexture2D);
    GL(void (*)(int, unsigned*), glGenTextures);
    GL(void (*)(unsigned, unsigned), glBindTexture);
    GL(void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*), glTexImage2D);
    GL(void (*)(int, unsigned*), glGenBuffers);
    GL(void (*)(unsigned, unsigned), glBindBuffer);
    GL(void (*)(unsigned, long, const void*, unsigned), glBufferData);
#undef GL
    if (!glClearColor || !glClear || !glPixelStorei || !glGetIntegerv || !glGetError ||
        !glGenFramebuffers || !glBindFramebuffer || !glFramebufferTexture2D || !glGenTextures ||
        !glBindTexture || !glTexImage2D || !glGenBuffers || !glBindBuffer || !glBufferData) {
        printf("skip this context lacks framebuffer or buffer objects\n");
        return 77;
    }

    // The game's state, left behind across every swap.
    unsigned framebuffer = 0;
    unsigned buffer = 0;
    if (pack_buffer) {
        glGenBuffers(1, &buffer);
        glBindBuffer(kPixelPackBuffer, buffer);
        glBufferData(kPixelPackBuffer, 16, nullptr, 0x88E1 /*GL_STREAM_READ*/);
    } else {
        unsigned texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(0x0DE1, texture);
        glTexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, nullptr);
        glBindTexture(0x0DE1, 0);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, framebuffer);
        glFramebufferTexture2D(0x8D40, 0x8CE0 /*COLOR_ATTACHMENT0*/, 0x0DE1, texture, 0);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);  // magenta, in the game's own object
        glClear(0x4000);
        glBindFramebuffer(kDrawFramebuffer, 0);  // presents from 0, reads from its own
        glPixelStorei(kPackRowLength, 4096);
        glPixelStorei(kPackAlignment, 8);
        glPixelStorei(kPackSkipRows, 3);
        glPixelStorei(kPackSkipPixels, 5);
    }
    while (glGetError() != 0) {
    }

    long errors = 0;
    const double deadline = now_ms() + 10000.0;
    int frames = 0;
    while (now_ms() < deadline && lines_containing(log_path, "captured a frame") < 1) {
        glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
        glClear(0x4000);
        swap(display, window);
        while (glGetError() != 0) {
            ++errors;
        }
        ++frames;
        usleep(16000);
    }

    int read_binding = -1;
    int row_length = -1;
    int alignment = -1;
    int skip_rows = -1;
    int skip_pixels = -1;
    int pack_binding = -1;
    glGetIntegerv(kReadFramebufferBinding, &read_binding);
    glGetIntegerv(kPackRowLength, &row_length);
    glGetIntegerv(kPackAlignment, &alignment);
    glGetIntegerv(kPackSkipRows, &skip_rows);
    glGetIntegerv(kPackSkipPixels, &skip_pixels);
    glGetIntegerv(kPixelPackBufferBinding, &pack_binding);

    // The capture: the frame the game presents is its clear colour with the
    // overlay on it.
    long clear_pixels = 0;
    long foreign = 0;
    bool readable = false;
    if (FILE* file = fopen(capture_path, "rb")) {
        int width = 0;
        int height = 0;
        int depth = 0;
        if (fscanf(file, "P6 %d %d %d", &width, &height, &depth) == 3 && width == W &&
            height == H && depth == 255 && fgetc(file) == '\n') {
            static unsigned char rgb[W * H * 3];
            readable = fread(rgb, 1, sizeof(rgb), file) == sizeof(rgb);
            for (long i = 0; readable && i < static_cast<long>(W) * H; ++i) {
                const int dr = abs(rgb[i * 3 + 0] - 26);
                const int dg = abs(rgb[i * 3 + 1] - 38);
                const int db = abs(rgb[i * 3 + 2] - 51);
                if (dr <= 4 && dg <= 4 && db <= 4) {
                    ++clear_pixels;
                } else if (dr > 40 || dg > 40 || db > 40) {
                    ++foreign;
                }
            }
        }
        fclose(file);
    }
    printf("     %d frame(s); capture: %s, %ld pixel(s) of the game's clear colour, %ld of the "
           "overlay's; %ld GL error(s)\n",
           frames, readable ? "read" : "missing or malformed", clear_pixels, foreign, errors);
    printf("     after: read framebuffer %d (the game's %u), pack row length %d, alignment %d, "
           "skips %d/%d, pack buffer %d (the game's %u)\n",
           read_binding, framebuffer, row_length, alignment, skip_rows, skip_pixels, pack_binding,
           buffer);
    check(readable, "the overlay wrote a capture of the frame's size");
    check(clear_pixels > static_cast<long>(W) * H / 2 && foreign > 100,
          "and it holds the frame the game presents, with the overlay on it");
    check(errors == 0, "the capture left no GL error in the game's queue");
    if (pack_buffer) {
        check(pack_binding == static_cast<int>(buffer), "the game's pack buffer is bound again");
    } else {
        check(read_binding == static_cast<int>(framebuffer),
              "the game's read framebuffer is bound again");
        check(row_length == 4096 && alignment == 8 && skip_rows == 3 && skip_pixels == 5,
              "and its pack state is as it left it");
    }

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
