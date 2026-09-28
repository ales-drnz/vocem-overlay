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
//   * `shared`: SDL_GL_DeleteContext and glfwDestroyWindow make NULL current
//     and then destroy -- the standard order -- and a game with a loader
//     context shares its objects with every window context it makes. The
//     first round's fix answered "not current, so drop without GL", and the
//     backend's program, buffers and 16-64 MB font texture stayed in the
//     share group for good, one set per owner: 4 textures, 4 programs and
//     8 buffers after 4 owners, counted with the loader context current.
//     The overlay now deletes them in the dying context, made current on a
//     1x1 pbuffer of its own configuration under a trapped X error handler,
//     and puts back what was current.
//
// Built a second time with VOCEM_PROBE_PRIVATE_X11 (gl_destroy_private_x11),
// the probe does not link libX11: it dlopens it RTLD_LOCAL, as SDL and GLFW
// do, so Xlib is in no global scope and the overlay has to find its error
// handler functions among the loaded objects. Without that, the `shared`
// scene there leaks exactly as it did before the fix.
//
// The witness is the process's own exit status, printed as "SURVIVED" only by
// code that runs after the destroy; what is current on the destroying thread
// afterwards, which must be what the game left there; and, in `shared`, the
// names left in the share group. The overlay's glXMakeCurrent and
// glXMakeContextCurrent calls are counted by interposed definitions in this
// executable (gl_draw_local.cpp's trick) and printed: the overlay never
// makes a context current on the game's drawable, only on its own pbuffer.

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
#include "vocem/avatar_rgba.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

#ifdef VOCEM_PROBE_PRIVATE_X11
// Xlib reached only through a private handle: every call below goes through
// these pointers, filled from dlopen("libX11.so.6", RTLD_LOCAL).
namespace private_x11 {
decltype(&::XInitThreads) init_threads;
decltype(&::XOpenDisplay) open_display;
decltype(&::XCreateColormap) create_colormap;
decltype(&::XCreateWindow) create_window;
decltype(&::XMapWindow) map_window;
decltype(&::XSync) sync;
bool load() {
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!x11) {
        return false;
    }
    init_threads = reinterpret_cast<decltype(init_threads)>(dlsym(x11, "XInitThreads"));
    open_display = reinterpret_cast<decltype(open_display)>(dlsym(x11, "XOpenDisplay"));
    create_colormap = reinterpret_cast<decltype(create_colormap)>(dlsym(x11, "XCreateColormap"));
    create_window = reinterpret_cast<decltype(create_window)>(dlsym(x11, "XCreateWindow"));
    map_window = reinterpret_cast<decltype(map_window)>(dlsym(x11, "XMapWindow"));
    sync = reinterpret_cast<decltype(sync)>(dlsym(x11, "XSync"));
    return init_threads && open_display && create_colormap && create_window && map_window &&
           sync;
}
}  // namespace private_x11
#define XInitThreads private_x11::init_threads
#define XOpenDisplay private_x11::open_display
#define XCreateColormap private_x11::create_colormap
#define XCreateWindow private_x11::create_window
#define XMapWindow private_x11::map_window
#define XSync private_x11::sync
#endif

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

using PFN_glXMakeContextCurrent = int (*)(Display*, XID, XID, void*);
using PFN_glXGetCurrentContext = void* (*)();
using PFN_glXGetCurrentDrawable = XID (*)();
using PFN_glIs = unsigned char (*)(unsigned int);
using PFN_getProcAddress = void* (*)(const unsigned char*);

PFN_glXMakeCurrent g_real_make_current = nullptr;
PFN_glXMakeContextCurrent g_real_make_context_current = nullptr;
volatile long g_overlay_make_current = 0;
volatile long g_overlay_make_context_current = 0;

PFN_glXDestroyContext g_destroy = nullptr;
Display* g_display = nullptr;
void* g_dying = nullptr;
void* (*g_current_context)() = nullptr;
bool g_destroyer_left_nothing = false;

void* destroyer(void*) {
    g_destroy(g_display, g_dying);
    XSync(g_display, False);
    g_destroyer_left_nothing = g_current_context() == nullptr;
    return nullptr;
}

#ifdef VOCEM_PROBE_PRIVATE_X11
void* real_dlsym_global(const char* name) { return dlsym(RTLD_DEFAULT, name); }
#endif

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

__attribute__((visibility("default"))) int glXMakeContextCurrent(Display* display, XID draw,
                                                                 XID read, void* context) {
    ++g_overlay_make_context_current;
    return g_real_make_context_current
               ? g_real_make_context_current(display, draw, read, context)
               : 0;
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
    const bool shared_scene = strcmp(scenario, "shared") == 0;
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "the owner context's destruction");
#ifdef VOCEM_PROBE_PRIVATE_X11
    if (!private_x11::load()) {
        printf("skip libX11.so.6 does not load privately\n");
        return 77;
    }
    if (real_dlsym_global("XSetErrorHandler")) {
        printf("FAIL Xlib is in the global scope, so this is not the private-libX11 game\n");
        return 1;
    }
#endif
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

    // In `shared`, the participant has a face on disk: its texture is one of
    // the backend's names in the share group, as the font texture is.
    const char* kHash = "abcdef0123";
    if (shared_scene) {
        snprintf(path, sizeof(path), "%s/cache/vocem", root);
        mkdir(path, 0700);
        snprintf(path, sizeof(path), "%s/cache/vocem/avatars", root);
        mkdir(path, 0700);
        unsigned char rgba[vocem::kAvatarRgbaBytes];
        memset(rgba, 0x7f, sizeof(rgba));
        char avatar_path[900];
        vocem::avatar_rgba_path(avatar_path, sizeof(avatar_path), 7, kHash);
        if (!vocem::avatar_rgba_write(avatar_path, rgba, vocem::kAvatarPixels,
                                      vocem::kAvatarPixels)) {
            printf("FAIL could not place the participant's face\n");
            return 1;
        }
    }

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([&](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "destroy");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Destroy");
        if (shared_scene) {
            snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash), "%s", kHash);
        }
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
    auto* current_context =
        reinterpret_cast<PFN_glXGetCurrentContext>(dlsym(gl, "glXGetCurrentContext"));
    auto* current_drawable =
        reinterpret_cast<PFN_glXGetCurrentDrawable>(dlsym(gl, "glXGetCurrentDrawable"));
    auto* current_read =
        reinterpret_cast<PFN_glXGetCurrentDrawable>(dlsym(gl, "glXGetCurrentReadDrawable"));
    auto* proc_address =
        reinterpret_cast<PFN_getProcAddress>(dlsym(gl, "glXGetProcAddressARB"));
    g_real_make_context_current =
        reinterpret_cast<PFN_glXMakeContextCurrent>(dlsym(gl, "glXMakeContextCurrent"));
    if (!choose || !visual_of || !create || !make_current || !g_destroy || !swap || !clear ||
        !current_context || !current_drawable || !current_read || !proc_address ||
        !g_real_make_context_current) {
        printf("FAIL the GLX functions do not resolve off the private handle\n");
        return 1;
    }
    g_real_make_current = make_current;
    g_current_context = current_context;
    const long context_calls_start = g_overlay_make_context_current;

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
    const auto frames = [&](Window window, int n) {
        for (int i = 0; i < n; ++i) {
            clear(0x4000);
            swap(display, window);
            usleep(16000);
        }
    };
    if (shared_scene) {
        // A loader context that never presents, and window contexts sharing
        // its objects, each destroyed SDL's way: NULL current, then destroy.
        void* loader = create(display, config_a, kGlxRgbaType, nullptr, True);
        if (!loader) {
            printf("FAIL no loader context\n");
            return 1;
        }
        constexpr int kOwners = 4;
        long restored = 0;
        for (int round = 0; round < kOwners; ++round) {
            void* shared_owner = create(display, config_a, kGlxRgbaType, loader, True);
            if (!shared_owner) {
                printf("FAIL no context sharing the loader's objects\n");
                return 1;
            }
            make_current(display, window_a, shared_owner);
            frames(window_a, 45);
            make_current(display, 0, nullptr);
            g_destroy(display, shared_owner);
            XSync(display, False);
            if (current_context() == nullptr && current_drawable() == 0) {
                ++restored;
            }
        }
        printf("SURVIVED: the process is alive after %d owners destroyed SDL's way\n", kOwners);
        check(lines_containing(log_path, "OpenGL backend ready") == kOwners,
              "the backend came up in every window context");
        check(lines_containing(log_path, "uploaded avatar") >= kOwners,
              "and each one uploaded the participant's face");
        check(restored == kOwners, "nothing was left current after any of the destroys");
        printf("     the overlay called glXMakeCurrent %ld and glXMakeContextCurrent %ld "
               "time(s)\n",
               g_overlay_make_current, g_overlay_make_context_current - context_calls_start);

        // What the share group still holds, asked with the loader current.
        make_current(display, window_a, loader);
        auto* is_texture = reinterpret_cast<PFN_glIs>(dlsym(gl, "glIsTexture"));
        auto* is_program =
            reinterpret_cast<PFN_glIs>(proc_address(reinterpret_cast<const unsigned char*>(
                "glIsProgram")));
        auto* is_buffer =
            reinterpret_cast<PFN_glIs>(proc_address(reinterpret_cast<const unsigned char*>(
                "glIsBuffer")));
        if (!is_texture || !is_program || !is_buffer) {
            printf("FAIL glIsTexture/glIsProgram/glIsBuffer do not resolve\n");
            return 1;
        }
        long textures = 0;
        long programs = 0;
        long buffers = 0;
        for (unsigned int name = 1; name <= 4096; ++name) {
            textures += is_texture(name) ? 1 : 0;
            programs += is_program(name) ? 1 : 0;
            buffers += is_buffer(name) ? 1 : 0;
        }
        printf("     left in the share group after %d owners: %ld texture(s), %ld program(s), "
               "%ld buffer(s)\n",
               kOwners, textures, programs, buffers);
        check(textures == 0 && programs == 0 && buffers == 0,
              "no owner left its backend's objects in the share group");
        make_current(display, 0, nullptr);
        g_destroy(display, loader);
        XSync(display, False);

        char cleanup[700];
        snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
        system(cleanup);
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }

    void* owner = create(display, config_a, kGlxRgbaType, nullptr, True);
    if (!owner) {
        printf("FAIL no context\n");
        return 1;
    }
    make_current(display, window_a, owner);
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
        check(g_destroyer_left_nothing, "nothing was left current on the destroying thread");
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
        check(current_context() == recreated && current_drawable() == window_b &&
                  current_read() == window_b,
              "the recreated context is still current on its window");
        frames(window_b, 45);
        check(lines_containing(log_path, "OpenGL backend ready") == 2,
              "and the overlay came up again in the recreated context");
        make_current(display, 0, nullptr);
        g_destroy(display, recreated);
        XSync(display, False);
    }
    printf("     the overlay called glXMakeCurrent %ld and glXMakeContextCurrent %ld time(s) "
           "around the destruction\n",
           g_overlay_make_current - calls_before,
           g_overlay_make_context_current - context_calls_start);
    check(g_overlay_make_current == calls_before,
          "the overlay made nothing current on the game's drawable");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
