// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay must draw in a game that dlopens its GL library RTLD_LOCAL --
// which is Minecraft, and every other title built on GLFW, LWJGL or SDL: the
// level-3 door, the one the dlsym hook exists for. For those games no GL symbol
// is in the global scope, so the heavy library's RTLD_NEXT/RTLD_DEFAULT
// resolution found nothing it does not interpose: glXQueryDrawable null, the
// glGetIntegerv fallback null, the drawable 0x0, and "no drawable size: nothing
// drawn this frame" once per frame for the life of the game. The overlay was
// loaded, detected the game, attached to the state, built its backend -- and
// never put a pixel on screen.
//
// Measured on Minecraft 26.2 from inside, via VOCEM_LOG_FILE, after its
// launcher's stderr pipe had hidden the log for a whole evening. glxgears had
// been the standing witness for "the GL path draws", and it links libGL: for
// the loaders that matter, linking is the exception.
//
// This probe is that class of game in miniature. Every GL and GLX call goes
// through dlopen(RTLD_LOCAL) + dlsym on the handle -- the interposed dlsym, as
// in a real game -- and nothing GL is linked. It publishes a channel of its own
// into a private /dev/shm (bwrap), lets the overlay draw over 45 frames, then
// reads its front buffer back and requires pixels it did not paint itself.
// Against the library as shipped in 0.1.0-44 the count is zero.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "private_shm.h"
#include "vocem/shm.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void write_file(const char* path, const char* contents) {
    if (FILE* file = fopen(path, "w")) {
        fputs(contents, file);
        fclose(file);
    }
}

// The shapes of what is resolved off the handle. GLX types reduced to void*.
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

    // A private /dev/shm, so the channel published here can never be mistaken
    // for the daemon's by anything real.
    // Isolated or not is answered by looking at /dev/shm, never by a variable
    // this process could have been handed: a forged sentinel once let a probe
    // publish its fake channel into the live daemon's segment while the owner
    // was playing (private_shm.h tells that story). The variable now only says
    // whether the sandbox has already been attempted.
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    alarm(60);

    // A world of this test's own: a config that lets the overlay draw here, and
    // a cache directory nobody misses.
    char root[] = "/tmp/vocem-gl-draw-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    write_file(path, "enabled = true\nshown_apps = vocem_gl_draw_local\n");
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);

    // The channel, published by this process: the reader in the overlay cannot
    // tell it from the daemon, which is the point.
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        snprintf(state.channel_name, sizeof(state.channel_name), "dlopen-local");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 500 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Local %u", i + 1);
        }
    });

    // The game's half, exactly as GLFW and LWJGL do it: the GL library arrives
    // by dlopen(RTLD_LOCAL), and every symbol by dlsym on that handle -- which
    // is the interposed dlsym, as in a real game.
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
    // Override-redirect and parked far off screen: the window manager never
    // sees it, nothing appears and no focus is taken -- the owner may be in a
    // game while this runs. Under a compositor every window renders into a
    // buffer of its own, so the front-buffer read-back is unaffected.
    swa.override_redirect = True;
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, W, H, 0,
                                  visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWOverrideRedirect, &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    check(context != nullptr, "and a context comes up");
    make_current(display, window, context);

    // 45 frames: the overlay skips its first thirty while ImGui sizes itself,
    // and a few more make the count independent of that detail.
    for (int frame = 0; frame < 45; ++frame) {
        clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
        clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
        swap(display, window);
        usleep(16000);
    }

    // What ended up on screen, read out of the front buffer the way the capture
    // aid reads it. Anything that is not the clear colour was drawn by somebody
    // else, and the only somebody else in here is the overlay.
    static unsigned char pixels[360 * 360 * 4];
    read_buffer(0x0404 /*GL_FRONT*/);
    read_pixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels);
    long foreign = 0;
    for (long i = 0; i < (long)W * H; ++i) {
        const unsigned char r = pixels[i * 4 + 0];
        const unsigned char g = pixels[i * 4 + 1];
        const unsigned char b = pixels[i * 4 + 2];
        const int dr = r > 26 ? r - 26 : 26 - r;
        const int dg = g > 38 ? g - 38 : 38 - g;
        const int db = b > 51 ? b - 51 : 51 - b;
        if (dr > 40 || dg > 40 || db > 40) {
            ++foreign;
        }
    }
    printf("     foreign pixels: %ld\n", foreign);
    check(foreign > 500,
          "the overlay drew in a process whose GL lives behind dlopen(RTLD_LOCAL)");

    destroy(display, context);
    XCloseDisplay(display);
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
