// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The context the overlay's backend lives in, destroyed while it is NOT current
// on the thread that destroys it -- and the game has to survive it.
//
// vocem_gl_context_destroyed used to do MangoHud's dance whenever the dying
// owner was not current: glXMakeCurrent(display, <the current drawable>,
// <the dying context>), tear down, put the previous context back. Both halves
// of that are X requests that can fail, and a GLX failure is an X error: with
// no handler of its own installed -- most games -- Xlib's default handler
// prints it and calls exit(1). The overlay killed the game in its own
// glXDestroyContext. Two ways, one per scenario (VOCEM_GL_SCENARIO):
//
//   * `visual`: the game recreates its window and context with another
//     framebuffer configuration (a settings change to MSAA), makes the new
//     pair current, then destroys the old context. The dance made the old
//     context current on the NEW window: BadMatch, X_GLXMakeCurrent, exit 1
//     on NVIDIA (VOCEM_DISABLE=1: exit 0). The overlay then has to come up
//     again in the new context, or the fix is just an overlay that stopped.
//   * `thread`: the owner, still current on the render thread, destroyed from
//     another thread -- legal, it is only marked for destruction. The dance
//     ran on the destroying thread, where nothing is current: glXMakeCurrent
//     of a context current elsewhere is BadAccess, exit 1 -- measured under
//     Mesa's software GLX (`__GLX_VENDOR_LIBRARY_NAME=mesa`,
//     `LIBGL_ALWAYS_SOFTWARE=1`), which is how tests/CMakeLists.txt runs it.
//
// The witness is the process's own exit status, printed as "SURVIVED" only by
// code that runs after the destroy; and the overlay's glXMakeCurrent calls,
// counted by an interposed definition in this executable (gl_draw_local.cpp's
// trick), which must be zero: the fix makes the dying context current nowhere.

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"
#include "vocem/shm.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
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

// GLX reduced to void*; the constants are GLX 1.3's.
using GLXFBConfig = void*;
using PFN_glXChooseFBConfig = GLXFBConfig* (*)(Display*, int, const int*, int*);
using PFN_glXGetVisualFromFBConfig = XVisualInfo* (*)(Display*, GLXFBConfig);
using PFN_glXCreateNewContext = void* (*)(Display*, GLXFBConfig, int, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXDestroyContext = void (*)(Display*, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_glClear = void (*)(unsigned int);
constexpr int kGlxDoublebuffer = 5;
constexpr int kGlxRenderType = 0x8011;
constexpr int kGlxRgbaBit = 0x1;
constexpr int kGlxDrawableType = 0x8010;
constexpr int kGlxWindowBit = 0x1;
constexpr int kGlxSampleBuffers = 100000;
constexpr int kGlxSamples = 100001;
constexpr int kGlxRgbaType = 0x8014;

PFN_glXMakeCurrent g_real_make_current = nullptr;
volatile long g_overlay_make_current = 0;

PFN_glXDestroyContext g_destroy = nullptr;
Display* g_display = nullptr;
void* g_dying = nullptr;

void* destroyer(void*) {
    g_destroy(g_display, g_dying);
    XSync(g_display, False);
    return nullptr;
}

}  // namespace

extern "C" {

// The overlay resolves glXMakeCurrent through RTLD_DEFAULT, where the main
// program comes first (-rdynamic); this probe's own calls go through the
// pointer taken off the private handle, so everything counted is the overlay's.
__attribute__((visibility("default"))) int glXMakeCurrent(Display* display, XID drawable,
                                                          void* context) {
    ++g_overlay_make_current;
    return g_real_make_current ? g_real_make_current(display, drawable, context) : 0;
}

}  // extern "C"

int main() {
    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no GLX drawable\n");
        return 77;
    }
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "visual";
    const bool thread_scene = strcmp(scenario, "thread") == 0;
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "the owner context's destruction");
    XInitThreads();

    char root[] = "/tmp/vocem-gl-destroy-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule =
        "enabled = true\nshown_apps = " + vocem_test::own_name("vocem_gl_destroy_owner") + "\n";
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

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "destroy");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Destroy");
    });

    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gl) {
        printf("skip libGL.so.1 is not installed\n");
        return 77;
    }
    auto* choose = reinterpret_cast<PFN_glXChooseFBConfig>(dlsym(gl, "glXChooseFBConfig"));
    auto* visual_of =
        reinterpret_cast<PFN_glXGetVisualFromFBConfig>(dlsym(gl, "glXGetVisualFromFBConfig"));
    auto* create = reinterpret_cast<PFN_glXCreateNewContext>(dlsym(gl, "glXCreateNewContext"));
    auto* make_current = reinterpret_cast<PFN_glXMakeCurrent>(dlsym(gl, "glXMakeCurrent"));
    g_destroy = reinterpret_cast<PFN_glXDestroyContext>(dlsym(gl, "glXDestroyContext"));
    auto* swap = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(gl, "glXSwapBuffers"));
    auto* clear = reinterpret_cast<PFN_glClear>(dlsym(gl, "glClear"));
    if (!choose || !visual_of || !create || !make_current || !g_destroy || !swap || !clear) {
        printf("FAIL the GLX functions do not resolve off the private handle\n");
        return 1;
    }
    g_real_make_current = make_current;

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip the display did not open from inside the sandbox\n");
        return 77;
    }
    g_display = display;
    const int plain[] = {kGlxDoublebuffer, 1, kGlxRenderType, kGlxRgbaBit, kGlxDrawableType,
                         kGlxWindowBit, 0};
    const int msaa[] = {kGlxDoublebuffer, 1, kGlxRenderType, kGlxRgbaBit, kGlxDrawableType,
                        kGlxWindowBit, kGlxSampleBuffers, 1, kGlxSamples, 4, 0};
    int count = 0;
    GLXFBConfig* plain_configs = choose(display, DefaultScreen(display), plain, &count);
    if (!plain_configs || count == 0) {
        printf("skip no double-buffered window configuration\n");
        return 77;
    }
    const GLXFBConfig config_a = plain_configs[0];
    XVisualInfo* visual_a = visual_of(display, config_a);
    const auto window_for = [&](XVisualInfo* visual, int y) {
        XSetWindowAttributes swa;
        swa.colormap =
            XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
        // Override-redirect and parked off screen: nothing appears, no focus.
        swa.override_redirect = True;
        swa.border_pixel = 0;
        const Window window = XCreateWindow(
            display, RootWindow(display, visual->screen), -4000, y, 320, 240, 0, visual->depth,
            InputOutput, visual->visual, CWColormap | CWOverrideRedirect | CWBorderPixel, &swa);
        XMapWindow(display, window);
        return window;
    };
    const Window window_a = window_for(visual_a, 0);
    void* owner = create(display, config_a, kGlxRgbaType, nullptr, True);
    if (!owner) {
        printf("FAIL no context\n");
        return 1;
    }
    make_current(display, window_a, owner);
    const auto frames = [&](Window window, int n) {
        for (int i = 0; i < n; ++i) {
            clear(0x4000);
            swap(display, window);
            usleep(16000);
        }
    };
    frames(window_a, 45);
    check(lines_containing(log_path, "OpenGL backend ready") == 1,
          "the backend came up in the first context");
    const long calls_before = g_overlay_make_current;

    if (thread_scene) {
        // Destroyed from another thread while current on this one.
        g_dying = owner;
        pthread_t thread;
        pthread_create(&thread, nullptr, &destroyer, nullptr);
        pthread_join(thread, nullptr);
        printf("SURVIVED: the process is alive after the other thread's glXDestroyContext\n");
        make_current(display, 0, nullptr);
        XSync(display, False);
    } else {
        // Another configuration, current on another window; the old one dies.
        GLXFBConfig* msaa_configs = choose(display, DefaultScreen(display), msaa, &count);
        if (!msaa_configs || count == 0) {
            printf("skip no multisampled window configuration to recreate the window with\n");
            return 77;
        }
        XVisualInfo* visual_b = visual_of(display, msaa_configs[0]);
        printf("     visuals 0x%lx and 0x%lx\n", visual_a->visualid, visual_b->visualid);
        const Window window_b = window_for(visual_b, 300);
        void* recreated = create(display, msaa_configs[0], kGlxRgbaType, nullptr, True);
        make_current(display, window_b, recreated);
        g_destroy(display, owner);
        XSync(display, False);
        printf("SURVIVED: the process is alive after glXDestroyContext of the owner\n");
        frames(window_b, 45);
        check(lines_containing(log_path, "OpenGL backend ready") == 2,
              "and the overlay came up again in the recreated context");
        make_current(display, 0, nullptr);
        g_destroy(display, recreated);
        XSync(display, False);
    }
    printf("     the overlay called glXMakeCurrent %ld time(s) around the destruction\n",
           g_overlay_make_current - calls_before);
    check(g_overlay_make_current == calls_before,
          "the overlay made no context current to tear down in a dying one");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
