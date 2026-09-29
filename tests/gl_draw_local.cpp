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
//
// Two more things the same miniature game measures, since 0.1.8 (DESIGN entry
// 132):
//
//   * **How many textures the overlay holds** after its first frames: a census
//     of glIsTexture over the low names. The scene has no faces, so the answer
//     is the font atlas and nothing else -- one. The library shipped 0.1.7
//     built the atlas texture twice on the frame its backend came up (once
//     explicitly, once again inside the backend's own NewFrame) and orphaned
//     the first, 16 to 64 MB of RGBA, for the life of the game's context: two.
//   * **VOCEM_GL_SCENARIO=second-context**: a second, unshared GLX context is
//     created, made current and destroyed -- a loader thread's helper context,
//     a splash screen's -- and the frames continue in the first. The overlay's
//     backend lives in the first context and must not notice: the shipped
//     library tore it down for ANY context's death, made the dying one current
//     with the drawing one's drawable to do so, and rebuilt everything on the
//     next frame. Two witnesses: "OpenGL backend ready" once in the log, and
//     zero calls to glXMakeCurrent from the overlay, counted by an interposed
//     definition in this executable (the gl_avatar_quiet.cpp trick: the
//     overlay resolves the name through RTLD_DEFAULT, where the main program
//     comes first).
//   * **VOCEM_GL_SCENARIO=two-windows**: a second window with a second,
//     unshared context PRESENTS, the way a game with a tool window or an
//     emulator with a debugger does. The backend lives in the first context,
//     and its objects' names mean something else in the second -- they are
//     that context's own textures, programs and buffers, or nothing. The
//     second context makes eight textures of its own, one texel of a known
//     colour each, and presents while a new colour emoji arrives (a fold,
//     glTexSubImage2D into the atlas's name) and the display changes (a whole
//     upload, glDeleteTextures of the atlas's name). Three witnesses in that
//     context: no pixel of the overlay's, no GL error, and all eight textures
//     alive with their texel -- and the first window drawing again afterwards,
//     so an overlay that simply stopped would not pass. Against the library
//     of 0.1.10-6: texture 1 became a 2048x4096 atlas and 45 GL errors were
//     left in the second context's queue, one per swap (entry 210).

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
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;
using vocem_test::write_file;

namespace {

// The overlay's own glXMakeCurrent calls. This probe never calls the name
// through the global scope -- its own calls go through the pointer it took
// off the private handle -- so everything counted here is the overlay's.
using PFN_glXMakeCurrent_real = int (*)(Display*, XID, void*);
PFN_glXMakeCurrent_real g_real_make_current = nullptr;
volatile long g_overlay_make_current = 0;

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

// The system's libGL.so.1 of this process's width, found by reading ELF
// headers rather than by loading it: once loaded under its own name, a second
// dlopen by any other path is answered with the same object and the same name,
// and the soname scene would measure nothing.
const char* system_libgl_for_this_width() {
    static const char* const candidates[] = {
        "/usr/lib/libGL.so.1",       "/usr/lib32/libGL.so.1",
        "/usr/lib64/libGL.so.1",     "/usr/lib/x86_64-linux-gnu/libGL.so.1",
        "/usr/lib/i386-linux-gnu/libGL.so.1",
    };
    const unsigned char wanted = sizeof(void*) == 8 ? 2 : 1;  // EI_CLASS
    for (const char* candidate : candidates) {
        FILE* file = fopen(candidate, "rb");
        if (!file) {
            continue;
        }
        unsigned char header[5] = {};
        const bool read = fread(header, 1, sizeof(header), file) == sizeof(header);
        fclose(file);
        if (read && memcmp(header, "\x7f" "ELF", 4) == 0 && header[4] == wanted) {
            return candidate;
        }
    }
    return nullptr;
}

}  // namespace

extern "C" {

// The interposition (see the header). Exported from the executable, which is
// what -rdynamic is for in tests/CMakeLists.txt.
__attribute__((visibility("default"))) int glXMakeCurrent(Display* display, XID drawable,
                                                          void* context) {
    ++g_overlay_make_current;
    return g_real_make_current ? g_real_make_current(display, drawable, context) : 0;
}

}  // extern "C"

int main() {
    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no GLX drawable to draw into\n");
        return 77;
    }
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "";
    const bool second_context = strcmp(scenario, "second-context") == 0;
    const bool arrivals = strcmp(scenario, "arrivals") == 0;
    const bool early_exit = strcmp(scenario, "early-exit") == 0;
    const bool two_windows = strcmp(scenario, "two-windows") == 0;
    const bool soname = strcmp(scenario, "soname") == 0;
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

    vocem_test::set_alarm(60, "the dlopen/dlsym door");

    // A world of this test's own: a config that lets the overlay draw here, and
    // a cache directory nobody misses.
    char root[] = "/tmp/vocem-gl-draw-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    // The rule names THIS binary, read off /proc/self/exe: a literal is
    // wrong the day the binary is renamed and wrong at -m32 today
    // (tests/probe_name.h, entry 129).
    vocem_test::overlay_config_home(root, vocem_test::own_name("vocem_gl_draw_local"));
    // The overlay's own log, for counting how many times its backend came up.
    static char log_path[700];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path, 1);

    // The channel, published by this process: the reader in the overlay cannot
    // tell it from the daemon, which is the point.
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([arrivals, early_exit](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        // The arrivals scene publishes the owner's display from the first
        // frame, as vocemd always does: otherwise the first arrival is also a
        // change of size, which is a real rebuild and not what it measures.
        // And so does early-exit, for a reason of its own: its whole subject is
        // a context that dies while the atlas worker is still rasterising, and
        // the drawable's own height gives an 11 px atlas that is built in a few
        // frames. Under a loaded machine (the suite at -j16) two frames were
        // slower than that build, the atlas landed before the teardown, and the
        // scene's own precondition failed in two runs of three (DESIGN 193). At
        // the display's 32 px the build is ~120 ms against two frames.
        if (arrivals || early_exit) {
            state.display_height = 2160;
        }
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
    //
    // VOCEM_GL_SCENARIO=soname opens the SAME system library through its
    // unversioned development link, `libGL.so` beside `libGL.so.1`, the name a
    // game may ask for. The shim judged the file's name, so the library was
    // taken for a private GL and this scene drew nothing and said nothing,
    // VOCEM_DEBUG included (0.1.12-2: 0 pixels, 0 lines, both widths). It
    // judges the SONAME too now, for an object in libc's own directory.
    void* gl = nullptr;
    if (soname) {
        const char* found = system_libgl_for_this_width();
        if (!found) {
            printf("skip no libGL.so.1 of this width in the system's library directories\n");
            return 77;
        }
        static char link_path[700];
        snprintf(link_path, sizeof(link_path), "%.*s/libGL.so",
                 static_cast<int>(strrchr(found, '/') - found), found);
        struct stat link_stat;
        struct stat real_stat;
        if (stat(link_path, &link_stat) != 0 || stat(found, &real_stat) != 0 ||
            link_stat.st_ino != real_stat.st_ino || link_stat.st_dev != real_stat.st_dev) {
            printf("skip %s is not a link to %s here\n", link_path, found);
            return 77;
        }
        printf("     opening %s (the same file as %s)\n", link_path, found);
        gl = dlopen(link_path, RTLD_LAZY | RTLD_LOCAL);
    } else {
        gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    }
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
    using PFN_glIsTexture = unsigned char (*)(unsigned int);
    auto* is_texture = reinterpret_cast<PFN_glIsTexture>(dlsym(gl, "glIsTexture"));
    check(choose && create && make_current && destroy && swap && clear_colour && clear &&
              read_buffer && read_pixels && is_texture,
          "every GL function resolves off the private handle");
    if (failures) {
        return 1;
    }
    g_real_make_current = make_current;

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
    // The longest swap of these frames is printed by the arrivals scene: the
    // first frame with a channel on it is where the atlas is built.
    double first_frames_worst_ms = 0.0;
    const auto run_frames = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
            clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
            timespec before{};
            timespec after{};
            clock_gettime(CLOCK_MONOTONIC, &before);
            swap(display, window);
            clock_gettime(CLOCK_MONOTONIC, &after);
            const double span = static_cast<double>(after.tv_sec - before.tv_sec) * 1e3 +
                                static_cast<double>(after.tv_nsec - before.tv_nsec) / 1e6;
            first_frames_worst_ms = span > first_frames_worst_ms ? span : first_frames_worst_ms;
            usleep(16000);
        }
    };
    if (early_exit) {
        // vk_present_draw's early-exit, on the OpenGL door: one frame starts
        // the atlas worker, then the context dies and the process exits inside
        // the build. glXDestroyContext reaches release(), which has to wait for
        // the worker; exit reaches the library's destructor, which has to wait
        // too. A guard on the worker's hazard, not a refutation: a library with
        // no worker passes it trivially.
        //
        // ONE frame, and it was two: the second frame was a race between the
        // build and the frame clock, and under a loaded machine (the suite at
        // -j16) the second swap came after the atlas was done, so it was
        // uploaded, the teardown met nothing mid-build, and the precondition
        // below failed in three runs of eight -- at 11 px and still at 32 px
        // (DESIGN 193). The worker starts in the first frame (measured, 3 of 3),
        // and with no second frame there is no present for an upload to happen
        // in: the teardown meets the build by construction rather than by
        // timing. A frame drawn DURING the build is every other scene's warm-up.
        run_frames(1);
        make_current(display, 0, nullptr);
        destroy(display, context);
        XCloseDisplay(display);
        const long started = lines_containing(log_path, "off the game's thread");
        const long ready = lines_containing(log_path, "font texture uploaded whole");
        printf("     the overlay started the atlas worker %ld time(s), uploaded an atlas %ld "
               "time(s)\n", started, ready);
        check(started == 1, "the atlas worker was running when the context died");
        // Guaranteed by the single frame above, and kept: it is what says so if
        // the scene is ever changed back into a race.
        check(ready == 0, "and no atlas had reached the GPU, so the teardown met it mid-build");
        char cleanup[700];
        snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
        system(cleanup);
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }
    run_frames(45);
    if (arrivals) {
        printf("     >>> the panel first appears: the longest swap took %.1f ms\n",
               first_frames_worst_ms);
    }

    // The texture census, with the backend up and no faces in the scene: the
    // font atlas is the one texture the overlay should hold.
    {
        long textures = 0;
        for (unsigned int name = 1; name <= 64; ++name) {
            if (is_texture(name)) {
                ++textures;
            }
        }
        printf("     textures alive in the low names: %ld\n", textures);
        check(textures == 1, "the overlay holds one texture, the atlas, and no orphaned twin");
    }

    if (second_context) {
        void* helper = create(display, visual, nullptr, 1);
        check(helper != nullptr, "a second, unshared context comes up");
        const long before = g_overlay_make_current;
        make_current(display, window, helper);
        make_current(display, window, context);
        // Destroyed while NOT current, through the shim's hook, as a loader
        // thread's context usually is: that is the case where the shipped
        // library made the dying context current to tear down a backend that
        // had never lived in it.
        destroy(display, helper);
        printf("     the overlay called glXMakeCurrent %ld time(s) while the helper died\n",
               g_overlay_make_current - before);
        check(g_overlay_make_current - before == 0,
              "a context that is not the backend's is not made current by the overlay");
        run_frames(15);
    }

    if (two_windows) {
        using PFN_glGenTextures = void (*)(int, unsigned int*);
        using PFN_glBindTexture = void (*)(unsigned int, unsigned int);
        using PFN_glTexImage2D = void (*)(unsigned int, int, int, int, int, int, unsigned int,
                                          unsigned int, const void*);
        using PFN_glGetTexImage = void (*)(unsigned int, int, unsigned int, unsigned int, void*);
        using PFN_glGetError = unsigned int (*)();
        using PFN_glGetTexLevelParameteriv = void (*)(unsigned int, int, unsigned int, int*);
        auto* gen_textures = reinterpret_cast<PFN_glGenTextures>(dlsym(gl, "glGenTextures"));
        auto* bind_texture = reinterpret_cast<PFN_glBindTexture>(dlsym(gl, "glBindTexture"));
        auto* tex_image = reinterpret_cast<PFN_glTexImage2D>(dlsym(gl, "glTexImage2D"));
        auto* get_tex_image = reinterpret_cast<PFN_glGetTexImage>(dlsym(gl, "glGetTexImage"));
        auto* get_error = reinterpret_cast<PFN_glGetError>(dlsym(gl, "glGetError"));
        auto* level_parameter = reinterpret_cast<PFN_glGetTexLevelParameteriv>(
            dlsym(gl, "glGetTexLevelParameteriv"));
        check(gen_textures && bind_texture && tex_image && get_tex_image && get_error &&
                  level_parameter,
              "the texture functions resolve off the private handle");
        if (failures) {
            return 1;
        }
        Window window_b = XCreateWindow(display, RootWindow(display, visual->screen), -4000,
                                        400, W, H, 0, visual->depth, InputOutput,
                                        visual->visual, CWColormap | CWOverrideRedirect, &swa);
        XMapWindow(display, window_b);
        void* context_b = create(display, visual, nullptr, 1);
        check(context_b != nullptr, "a second window's unshared context comes up");
        make_current(display, window_b, context_b);

        // The second context's own textures. A fresh context hands out names
        // from 1, and so did the first one when the overlay made its atlas.
        constexpr int kOwn = 8;
        unsigned int own[kOwn] = {};
        gen_textures(kOwn, own);
        for (int i = 0; i < kOwn; ++i) {
            const unsigned char texel[4] = {static_cast<unsigned char>(20 + 20 * i), 7, 9, 255};
            bind_texture(0x0DE1 /*GL_TEXTURE_2D*/, own[i]);
            tex_image(0x0DE1, 0, 0x1908 /*GL_RGBA*/, 1, 1, 0, 0x1908, 0x1401, texel);
        }
        bind_texture(0x0DE1, own[0]);
        while (get_error() != 0) {
        }

        long errors = 0;
        // Both windows presenting, the second nine frames in ten -- a game with
        // a tool window -- unless `alone`: then the first is silent, which is
        // what the hand-over below is about. The first window's frames keep the
        // owner fresh here however slowly a loaded machine runs this.
        const auto frames_b = [&](int frames, bool alone = false) {
            for (int frame = 0; frame < frames; ++frame) {
                if (!alone && frame % 10 == 0) {
                    make_current(display, window, context);
                    clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
                    clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
                    swap(display, window);
                    make_current(display, window_b, context_b);
                }
                clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
                clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
                swap(display, window_b);
                while (get_error() != 0) {
                    ++errors;
                }
                usleep(16000);
            }
        };
        const auto publish = [&](const char* third, uint32_t display_height) {
            writer.publish([&](vocem::SharedState& state) {
                state.connected = 1;
                state.in_channel = 1;
                state.status = 2;  // Connected
                state.display_height = display_height;
                snprintf(state.channel_name, sizeof(state.channel_name), "dlopen-local");
                state.user_count = 3;
                for (uint32_t i = 0; i < 3; ++i) {
                    state.users[i].id = 500 + i;
                    snprintf(state.users[i].name, sizeof(state.users[i].name), "Local %u",
                             i + 1);
                }
                snprintf(state.users[2].name, sizeof(state.users[2].name), "%s", third);
            });
        };
        frames_b(15);
        publish("Local 3 \xF0\x9F\x94\xA5", 0);  // fire: a fold, were anything drawn here
        frames_b(15);
        publish("Local 3 \xF0\x9F\x94\xA5", 1440);  // a new size: a whole upload
        frames_b(15);

        static unsigned char pixels_b[360 * 360 * 4];
        read_buffer(0x0404 /*GL_FRONT*/);
        read_pixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels_b);
        long foreign_b = 0;
        for (long i = 0; i < (long)W * H; ++i) {
            const int dr = pixels_b[i * 4 + 0] > 26 ? pixels_b[i * 4 + 0] - 26 : 26 - pixels_b[i * 4 + 0];
            const int dg = pixels_b[i * 4 + 1] > 38 ? pixels_b[i * 4 + 1] - 38 : 38 - pixels_b[i * 4 + 1];
            const int db = pixels_b[i * 4 + 2] > 51 ? pixels_b[i * 4 + 2] - 51 : 51 - pixels_b[i * 4 + 2];
            if (dr > 40 || dg > 40 || db > 40) {
                ++foreign_b;
            }
        }
        long intact = 0;
        for (int i = 0; i < kOwn; ++i) {
            unsigned char texel[4] = {0, 0, 0, 0};
            if (!is_texture(own[i])) {
                printf("     texture %u is gone\n", own[i]);
                continue;
            }
            bind_texture(0x0DE1, own[i]);
            // Its size first: read whole, a texture that is no longer one
            // texel is written into a four-byte buffer (the first version of
            // this check crashed there, on a 4096x4096 atlas under name 1).
            int width = 0;
            int height = 0;
            level_parameter(0x0DE1, 0, 0x1000 /*GL_TEXTURE_WIDTH*/, &width);
            level_parameter(0x0DE1, 0, 0x1001 /*GL_TEXTURE_HEIGHT*/, &height);
            if (width != 1 || height != 1) {
                printf("     texture %u is %dx%d now, not the 1x1 it was made\n", own[i], width,
                       height);
                continue;
            }
            get_tex_image(0x0DE1, 0, 0x1908, 0x1401, texel);
            if (texel[0] == 20 + 20 * i && texel[1] == 7 && texel[2] == 9) {
                ++intact;
            } else {
                printf("     texture %u holds %d,%d,%d where it was given %d,7,9\n", own[i],
                       texel[0], texel[1], texel[2], 20 + 20 * i);
            }
        }
        while (get_error() != 0) {
        }
        printf("     in the second window: %ld foreign pixel(s), %ld GL error(s) after its "
               "swaps, %ld of %d of its own textures intact\n",
               foreign_b, errors, intact, kOwn);
        check(foreign_b == 0, "the overlay draws nothing in a context its backend does not live in");
        check(errors == 0, "and leaves no GL error in that context's queue");
        check(intact == kOwn, "and that context's own textures are untouched, name for name");
        check(lines_containing(log_path, "does not live in") >= 1,
              "and the log says why the second window has no overlay");

        // And the second window alone, for longer than the hand-over's two
        // seconds: the first context is alive and silent -- a loading screen
        // kept for its loader -- and the overlay has to move to the window the
        // game is actually showing, or that window has none for the session.
        const long moved_before = lines_containing(log_path, "moving the overlay");
        frames_b(180, true);  // ~3 s at 16 ms a frame
        read_buffer(0x0404 /*GL_FRONT*/);
        read_pixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels_b);
        long moved = 0;
        for (long i = 0; i < (long)W * H; ++i) {
            const int dr = pixels_b[i * 4 + 0] > 26 ? pixels_b[i * 4 + 0] - 26 : 26 - pixels_b[i * 4 + 0];
            const int dg = pixels_b[i * 4 + 1] > 38 ? pixels_b[i * 4 + 1] - 38 : 38 - pixels_b[i * 4 + 1];
            const int db = pixels_b[i * 4 + 2] > 51 ? pixels_b[i * 4 + 2] - 51 : 51 - pixels_b[i * 4 + 2];
            if (dr > 40 || dg > 40 || db > 40) {
                ++moved;
            }
        }
        printf("     the second window alone for three seconds: %ld foreign pixel(s), %ld GL "
               "error(s) in all\n", moved, errors);
        check(lines_containing(log_path, "moving the overlay") == moved_before + 1,
              "once the first context has been silent, the overlay moves to the one presenting");
        check(moved > 500, "and draws there");
        check(errors == 0, "with no GL error left in that context's queue");

        make_current(display, window, context);
        destroy(display, context_b);
        // The first window again: the second context is gone, so its backend
        // went with it (the destroy hook) and the first gets one of its own.
        run_frames(45);
    }

    // Somebody joins, six times, each with a colour emoji nobody in the channel
    // had, and then a message arrives with one more -- vk_present_draw's
    // `arrivals` scene, on the OpenGL door. The overlay draws INSIDE the
    // game's glXSwapBuffers, atlas work included, so the longest swap after
    // an arrival is the hitch the game feels: printed, not asserted (entries
    // 145 and 185). The overlay's own lines are the count (entry 191).
    long arrival_swaps = 0;
    if (arrivals) {
        static const char* const kJoined[] = {
            "Arriva \xF0\x9F\x98\x80",  // grin
            "Arriva \xF0\x9F\x94\xA5",  // fire
            "Arriva \xF0\x9F\x8E\xAE",  // gamepad
            "Arriva \xF0\x9F\x9A\x80",  // rocket
            "Arriva \xF0\x9F\x8D\x95",  // pizza
            "Arriva \xF0\x9F\x90\xB1",  // cat
        };
        constexpr int kJoinedCount = static_cast<int>(sizeof(kJoined) / sizeof(kJoined[0]));
        for (int joined = 1; joined <= kJoinedCount + 1; ++joined) {
            const bool message = joined > kJoinedCount;
            const int users = message ? kJoinedCount : joined;
            timespec monotonic{};
            clock_gettime(CLOCK_MONOTONIC, &monotonic);
            const double now_seconds =
                static_cast<double>(monotonic.tv_sec) + static_cast<double>(monotonic.tv_nsec) / 1e9;
            writer.publish([&](vocem::SharedState& state) {
                state.connected = 1;
                state.in_channel = 1;
                state.status = 2;  // Connected
                // What vocemd publishes on the owner's machine (a 3840x2160
                // display), so the atlas is the size a game there builds and
                // the printed times are that game's, not a 360-pixel window's.
                state.display_height = 2160;
                snprintf(state.channel_name, sizeof(state.channel_name), "dlopen-local");
                state.user_count = 3 + static_cast<uint32_t>(users);
                for (uint32_t i = 0; i < 3; ++i) {
                    state.users[i].id = 500 + i;
                    snprintf(state.users[i].name, sizeof(state.users[i].name), "Local %u", i + 1);
                }
                for (int i = 0; i < users; ++i) {
                    state.users[3 + i].id = 600 + static_cast<uint64_t>(i);
                    snprintf(state.users[3 + i].name, sizeof(state.users[3 + i].name), "%s",
                             kJoined[i]);
                }
                if (message) {
                    state.notification.serial = 1;
                    state.notification.user_id = 900;
                    state.notification.received = now_seconds;
                    snprintf(state.notification.title, sizeof(state.notification.title),
                             "Messaggio \xF0\x9F\x93\xA3");  // megaphone
                }
            });
            double worst_ms = 0.0;
            for (int frame = 0; frame < 8; ++frame) {
                clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
                clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
                timespec before{};
                timespec after{};
                clock_gettime(CLOCK_MONOTONIC, &before);
                swap(display, window);
                clock_gettime(CLOCK_MONOTONIC, &after);
                const double span = static_cast<double>(after.tv_sec - before.tv_sec) * 1e3 +
                                    static_cast<double>(after.tv_nsec - before.tv_nsec) / 1e6;
                worst_ms = span > worst_ms ? span : worst_ms;
                usleep(16000);
            }
            ++arrival_swaps;
            printf("     >>> %s: the longest swap after it took %.1f ms\n",
                   message ? "a message arrives" : "somebody joins", worst_ms);
        }
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
    if (second_context) {
        const long ready = lines_containing(log_path, "OpenGL backend ready");
        printf("     the overlay said \"OpenGL backend ready\" %ld time(s)\n", ready);
        check(ready == 1, "and its backend came up once: the helper's death did not tear it down");
    }

    if (arrivals) {
        // "font atlas built" is the rasteriser; "folded" is an emoji put into
        // space the build reserved. Against the 0.1.10-5 package's library the
        // first reads eight -- the first frame's build and one per arrival.
        const long built = lines_containing(log_path, "font atlas built");
        const long folded = lines_containing(log_path, "folded into the font atlas");
        printf("     the overlay said \"font atlas built\" %ld time(s), \"folded\" %ld time(s)\n",
               built, folded);
        check(built == 1,
              "the atlas was rasterised once, for the first frame, and not again for anybody "
              "joining or any message arriving");
        check(folded >= arrival_swaps,
              "and every arrival's emoji reached the atlas, folded in (the positive control)");
        // As the squares it changed, not the whole atlas (entry 192).
        const long whole = lines_containing(log_path, "font texture uploaded whole");
        const long in_place = lines_containing(log_path, "copied in place");
        printf("     the font texture went up whole %ld time(s), in place %ld time(s)\n", whole,
               in_place);
        check(whole == 1, "the font texture was uploaded whole once, for the first frame");
        check(in_place >= arrival_swaps,
              "and every arrival after that copied only its folded squares into it");
        // And the first atlas was rasterised OFF the game's thread (entry 192):
        // 113 ms of stb_truetype that stood the game still in the frame the
        // panel first appeared. A fallback is allowed where no thread can be
        // had, and says so; on this machine one always can.
        const long off_thread = lines_containing(log_path, "off the game's thread");
        const long no_thread = lines_containing(log_path, "no thread for the font atlas");
        printf("     the atlas was rasterised off the game's thread %ld time(s), on it %ld\n",
               off_thread, no_thread);
        check(off_thread == 1 && no_thread == 0,
              "the first font atlas was rasterised on a worker, not in the game's frame");
    }

    destroy(display, context);
    XCloseDisplay(display);
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
