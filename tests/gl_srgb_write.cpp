// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay's colours in a game that writes sRGB.
//
// With GL_FRAMEBUFFER_SRGB enabled on an sRGB-capable drawable, the hardware
// applies linear->sRGB to whatever a fragment shader writes. ImGui's colours are
// already sRGB, so leaving the switch alone encodes them a second time: white
// and fully saturated colours are fixed points and look right, and everything in
// between is lifted. The panel's own surface is in between.
//
// The Vulkan half of this project meets the same thing through the swapchain's
// format and answers it in the shader, because there the encoding is the
// attachment's property and cannot be switched off (hdr.frag, mode 3;
// tests/hdr_pixels.cpp measures the round trip). Here it is a switch, so
// SrgbWriteGuard switches it -- and back, because it is the game's (rule 12).
// ImGui's own GL backend never looks at it; MangoHud's fork of that backend
// saves, clears and restores it, which is where the question came from.
//
// The probe is a miniature game, as gl_draw_local's is: private /dev/shm, its
// own channel, its own settings. Two things are different -- the drawable is
// asked to be sRGB-capable, and the panel is asked to be opaque, because a
// surface at the default opacity has no colour to measure.
//
// What is measured is the panel's surface, by counting pixels of the theme's
// own colour against pixels of the sRGB encode of that colour. Against the
// library before SrgbWriteGuard, the second count is the panel and the first is
// empty. It skips 77 rather than failing where the drawable cannot be
// sRGB-capable: what it would then measure is nothing at all.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"

#include <string>
#include "vocem/config.h"
#include "vocem/shm.h"
#include "vocem/theme.h"

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

// IEC 61966-2-1, the same encode the hardware applies on the way to an sRGB
// drawable. Written here rather than taken from the shader, so the number this
// test looks for is derived independently of the code it is judging.
unsigned char srgb_encode(unsigned char value) {
    const double linear = value / 255.0;
    const double encoded = linear <= 0.0031308 ? linear * 12.92
                                               : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    const double scaled = encoded * 255.0 + 0.5;
    return static_cast<unsigned char>(scaled > 255.0 ? 255.0 : scaled);
}

using PFN_glXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFN_glXCreateContext = void* (*)(Display*, XVisualInfo*, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXDestroyContext = void (*)(Display*, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_glClearColor = void (*)(float, float, float, float);
using PFN_glClear = void (*)(unsigned int);
using PFN_glEnable = void (*)(unsigned int);
using PFN_glIsEnabled = unsigned char (*)(unsigned int);
using PFN_glReadBuffer = void (*)(unsigned int);
using PFN_glReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int, void*);

constexpr unsigned int kFramebufferSrgb = 0x8DB9;

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
    // for the daemon's by anything real -- and asked of /dev/shm itself, never
    // of a variable this process could have been handed (private_shm.h).
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    vocem_test::set_alarm(60, "the sRGB write guard");

    char root[] = "/tmp/vocem-gl-srgb-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // Opaque, and no channel name: the panel's surface is what this measures,
    // and at the default opacity there is no surface to measure. Everything
    // else stays at its default, so the colour looked for is the theme's.
    // The rule names THIS binary, read off /proc/self/exe (probe_name.h,
    // entry 129).
    const std::string rule = "enabled = true\nshown_apps = " +
                             vocem_test::own_name("vocem_gl_srgb_write") +
                             "\nopacity = 1.0\nshow_channel_name = false\n";
    write_file(path, rule.c_str());
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        snprintf(state.channel_name, sizeof(state.channel_name), "srgb");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 700 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Local %u", i + 1);
        }
    });

    // The game's half. Through dlopen + dlsym on the handle, as gl_draw_local
    // does and as a real loader does.
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
    auto* enable = reinterpret_cast<PFN_glEnable>(dlsym(gl, "glEnable"));
    auto* is_enabled = reinterpret_cast<PFN_glIsEnabled>(dlsym(gl, "glIsEnabled"));
    auto* read_buffer = reinterpret_cast<PFN_glReadBuffer>(dlsym(gl, "glReadBuffer"));
    auto* read_pixels = reinterpret_cast<PFN_glReadPixels>(dlsym(gl, "glReadPixels"));
    check(choose && create && make_current && destroy && swap && clear_colour && clear && enable &&
              is_enabled && read_buffer && read_pixels,
          "every GL function resolves off the private handle");
    if (failures) {
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip the display did not open from inside the sandbox\n");
        return 77;
    }
    // An ordinary double-buffered visual, exactly gl_draw_local's. Asking for
    // GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB here was tried and taken out again: on
    // this driver glXChooseVisual answers NULL for that attribute list about
    // one run in three, with the overlay switched off and with no shim loaded
    // at all -- measured, so not ours, but a measurement that cannot be taken
    // twice is not a measurement (entry 31). Whether the drawable encodes is
    // asked of the drawable below instead, which is the honest question anyway.
    int attrs[] = {4 /*GLX_RGBA*/, 5 /*GLX_DOUBLEBUFFER*/, 0};
    XVisualInfo* visual = choose(display, DefaultScreen(display), attrs);
    if (!visual) {
        printf("skip no double-buffered visual on this display\n");
        return 77;
    }
    const int W = 360, H = 360;
    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(display, RootWindow(display, visual->screen), visual->visual,
                                   AllocNone);
    // Override-redirect and parked far off screen: nothing appears and no focus
    // is taken -- the owner may be in a game while this runs.
    swa.override_redirect = True;
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, W, H, 0,
                                  visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWOverrideRedirect, &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    check(context != nullptr, "and a context comes up");
    make_current(display, window, context);

    // The game turns sRGB writing on and leaves it on, which is what a game
    // that renders in linear light does.
    enable(kFramebufferSrgb);

    // And then the drawable is asked whether that meant anything, by measuring
    // rather than by believing it.
    //
    // glXChooseVisual takes GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB and is free to
    // ignore it, and glIsEnabled answers about the switch and not about the
    // drawable -- so the first version of this test enabled the state on a
    // linear drawable, measured nothing, and passed against the library with
    // the guard removed. A test that passes in both directions is worse than no
    // test. What is asked instead is the only question that matters: does a
    // known value written by this process come back encoded. A clear is a write
    // and is converted like any other (GL 4.6 spec, 17.3.9 "sRGB Conversion").
    //
    // Asked of the BACK buffer, before any swap. It was asked of the front
    // buffer right after the first swap, and that read is where this probe died
    // of SIGFPE inside libnvidia-glcore in two full -j16 runs of seventeen: a
    // `div` whose divisor is the pitch of a front-buffer surface the driver has
    // not sized yet, every field of it zero in both cores. Measured without
    // this project at all -- no shim, VOCEM_DISABLE=1, a replica of this
    // opening -- it happened 2 times in 1504 under the suite's load, at the
    // same address (DESIGN 208). The clear is converted on the way into the
    // back buffer exactly as into any other, so the question is the same one.
    {
        clear_colour(0.5f, 0.5f, 0.5f, 1.0f);
        clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
        unsigned char probe[4] = {0, 0, 0, 0};
        read_buffer(0x0405 /*GL_BACK*/);
        read_pixels(W / 2, H / 2, 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, probe);
        swap(display, window);
        // 0.5 linear encodes to 188; a drawable that stores 128 is not encoding.
        printf("     a mid grey written with sRGB on comes back %d\n", probe[0]);
        if (probe[0] < 180 || probe[0] > 196) {
            printf("skip this drawable does not encode sRGB on write, so there is nothing "
                   "here to measure\n");
            return 77;
        }
    }

    for (int frame = 0; frame < 45; ++frame) {
        clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
        clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
        swap(display, window);
        usleep(16000);
    }

    // And finds it on afterwards: the overlay borrowed it for its own draw and
    // has to give it back (rule 12). This half passes against the library
    // before the guard existed as well -- it never touched the switch -- and it
    // is here so that the cure cannot become a second defect.
    check(is_enabled(kFramebufferSrgb) != 0,
          "the game's sRGB write state survives the overlay's frame");

    const vocem::Config config;
    const vocem::Theme theme = vocem::theme_for(config);
    const vocem::Colour want = theme.panel_surface;
    const unsigned char lifted[3] = {srgb_encode(want.r), srgb_encode(want.g),
                                     srgb_encode(want.b)};
    printf("     the panel's surface is %d,%d,%d; encoded twice it would be %d,%d,%d\n", want.r,
           want.g, want.b, lifted[0], lifted[1], lifted[2]);

    static unsigned char pixels[360 * 360 * 4];
    read_buffer(0x0404 /*GL_FRONT*/);
    read_pixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels);
    long as_asked = 0;
    long encoded_twice = 0;
    for (long i = 0; i < (long)W * H; ++i) {
        const unsigned char* px = pixels + i * 4;
        // Two units of slack per channel: the surface is blended over the
        // scene at alpha 1, which is exact, and 8-bit rounding is the rest.
        const auto near = [px](const unsigned char* target) {
            for (int c = 0; c < 3; ++c) {
                const int delta = px[c] > target[c] ? px[c] - target[c] : target[c] - px[c];
                if (delta > 2) {
                    return false;
                }
            }
            return true;
        };
        const unsigned char plain[3] = {want.r, want.g, want.b};
        if (near(plain)) {
            ++as_asked;
        } else if (near(lifted)) {
            ++encoded_twice;
        }
    }
    printf("     pixels at the theme's colour: %ld; at the doubly encoded one: %ld\n", as_asked,
           encoded_twice);

    // The panel is the largest flat area the overlay draws, so a few hundred
    // pixels of it is the whole claim: either the surface reached the drawable
    // as the theme asked for it, or it did not.
    check(as_asked > 500, "the panel's surface reaches an sRGB drawable as the theme asked for it");
    check(encoded_twice < 50, "and not lifted by an encode it had already had");

    destroy(display, context);
    XCloseDisplay(display);
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
