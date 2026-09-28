// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay uploads a picture into a renderer whose pixel-store state is the
// game's, not ours.
//
// glTexImage2D with a client pointer does not read that pointer as a plain array:
// it reads it through GL_UNPACK_ROW_LENGTH, SKIP_ROWS, SKIP_PIXELS, ALIGNMENT and
// the GL_PIXEL_UNPACK_BUFFER binding, all of which are application state that
// survives a swap. The overlay saved and restored exactly one thing, the 2D
// texture binding, and read the rest of it out of whatever the game had left:
//
//   * with GL_UNPACK_ROW_LENGTH at 2048 a 64x64 upload reads about half a
//     megabyte out of a 16 KB buffer -- measured as a SIGSEGV, attributed to the
//     game;
//   * with a pixel-unpack buffer bound the pointer becomes an offset into the
//     *game's* buffer, so the picture never uploads and GL_INVALID_OPERATION is
//     pushed into the game's own error queue;
//   * and the ImGui backend's font-atlas path sets GL_UNPACK_ROW_LENGTH to 0 and
//     never restores it, so every atlas rebuild silently changed the game's.
//
// This is not hypothetical state: ImGui's own backend carries a comment from 2016
// about SDL leaving a row length behind, and streaming engines keep a PBO bound
// across frames.
//
// The probe is a miniature game -- an override-redirect window off-screen, the
// real shim preloaded, the real library -- that sets a hostile unpack state, runs
// frames until the overlay has uploaded a face, and then asks GL what it finds.
// Three assertions, each of which fails against the library before the guard:
// no error was pushed, the state is exactly as the game left it, and the picture
// did upload.
//
// The context here is a **desktop GLX** one, and the error assertion is about
// desktop GL alone. On an OpenGL ES 3 context the vendored ImGui backend leaves
// GL_INVALID_ENUM in the queue on its own account -- GL_CONTEXT_PROFILE_MASK and
// GL_PRIMITIVE_RESTART, measured, and present in the shipped 0.1.0-67 library too,
// so it is older than anything here. An ES variant of this probe is the way to
// hold that half, and it does not exist: said here rather than left for somebody
// to discover that this witness only ever watched the easy context.

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"

#include <string>
#include "vocem/avatar_rgba.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;
using vocem_test::write_file;

namespace {

long uploads_reported(const char* log_path) {
    FILE* file = fopen(log_path, "r");
    if (!file) {
        return 0;
    }
    long count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "uploaded avatar")) {
            ++count;
        }
    }
    fclose(file);
    return count;
}

using PFN_glXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFN_glXCreateContext = void* (*)(Display*, XVisualInfo*, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXDestroyContext = void (*)(Display*, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_glClearColor = void (*)(float, float, float, float);
using PFN_glClear = void (*)(unsigned int);
using PFN_glPixelStorei = void (*)(unsigned int, int);
using PFN_glGetIntegerv = void (*)(unsigned int, int*);
using PFN_glGetError = unsigned int (*)();
using PFN_glGenBuffers = void (*)(int, unsigned int*);
using PFN_glBindBuffer = void (*)(unsigned int, unsigned int);
using PFN_glBufferData = void (*)(unsigned int, long, const void*, unsigned int);
using PFN_glGenFramebuffers = void (*)(int, unsigned int*);
using PFN_glBindFramebuffer = void (*)(unsigned int, unsigned int);
using PFN_glGenRenderbuffers = void (*)(int, unsigned int*);
using PFN_glBindRenderbuffer = void (*)(unsigned int, unsigned int);
using PFN_glRenderbufferStorage = void (*)(unsigned int, unsigned int, int, int);
using PFN_glFramebufferRenderbuffer = void (*)(unsigned int, unsigned int, unsigned int,
                                               unsigned int);

// The two framebuffer targets and their bindings. GL_FRAMEBUFFER binds both;
// the overlay used to bind it to 0 for its draw and put "the binding" back,
// which restored the draw side and left the read side pointing at the window.
constexpr unsigned int kReadFramebuffer = 0x8CA8;
constexpr unsigned int kDrawFramebuffer = 0x8CA9;
constexpr unsigned int kReadFramebufferBinding = 0x8CAA;
constexpr unsigned int kDrawFramebufferBinding = 0x8CA6;
constexpr unsigned int kRenderbuffer = 0x8D41;
constexpr unsigned int kColorAttachment0 = 0x8CE0;
constexpr unsigned int kRgba8 = 0x8058;

constexpr unsigned int kUnpackRowLength = 0x0CF2;
constexpr unsigned int kUnpackSkipRows = 0x0CF3;
constexpr unsigned int kUnpackSkipPixels = 0x0CF4;
constexpr unsigned int kUnpackAlignment = 0x0CF5;
constexpr unsigned int kPixelUnpackBuffer = 0x88EC;
constexpr unsigned int kPixelUnpackBufferBinding = 0x88EF;

// What the miniature game leaves bound. The row length is the one that turns an
// upload into an out-of-bounds read; the rest are here because restoring one and
// not the others is the same defect with a smaller blast radius.
constexpr int kGameRowLength = 2048;
constexpr int kGameSkipRows = 2;
constexpr int kGameSkipPixels = 3;
constexpr int kGameAlignment = 1;

}  // namespace

int main() {
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    {
        char holds[256] = {0};
        if (!vocem_test::shm_is_private(holds, sizeof(holds))) {
            vocem_test::shm_explain_refusal(holds);
            return 1;
        }
    }
    vocem_test::set_alarm(180, "a game's hostile pixel-store state");

    char root[] = "/tmp/vocem-gl-unpack-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[900];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // The rule names THIS binary, read off /proc/self/exe: a literal is
    // wrong the day the binary is renamed and wrong at -m32 today
    // (tests/probe_name.h, entry 129).
    const std::string rule = "enabled = true\nshown_apps = " +
                             vocem_test::own_name("vocem_gl_unpack_state") + "\n";
    write_file(path, rule.c_str());
    setenv("XDG_CONFIG_HOME", root, 1);
    char cache[800];
    snprintf(cache, sizeof(cache), "%s/cache", root);
    mkdir(cache, 0700);
    setenv("XDG_CACHE_HOME", cache, 1);
    snprintf(path, sizeof(path), "%s/vocem", cache);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/avatars", cache);
    mkdir(path, 0700);

    static char log_path[900];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path, 1);

    // One participant with a picture already on disk, so the upload happens on
    // the first frame that can take it rather than after a download.
    const uint64_t kUser = 4242;
    const char* kHash = "abcdef0123";
    {
        unsigned char rgba[vocem::kAvatarRgbaBytes];
        memset(rgba, 0x7f, sizeof(rgba));
        char avatar_path[900];
        vocem::avatar_rgba_path(avatar_path, sizeof(avatar_path), kUser, kHash);
        if (!vocem::avatar_rgba_write(avatar_path, rgba, vocem::kAvatarPixels,
                                      vocem::kAvatarPixels)) {
            printf("FAIL could not place the avatar the upload is supposed to take\n");
            return 1;
        }
    }

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([&](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "unpack");
        state.user_count = 1;
        state.users[0].id = kUser;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Face");
        snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash), "%s", kHash);
    });
    if (failures) {
        return 1;
    }

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
    auto* pixel_store = reinterpret_cast<PFN_glPixelStorei>(dlsym(gl, "glPixelStorei"));
    auto* get_integer = reinterpret_cast<PFN_glGetIntegerv>(dlsym(gl, "glGetIntegerv"));
    auto* get_error = reinterpret_cast<PFN_glGetError>(dlsym(gl, "glGetError"));
    auto* gen_buffers = reinterpret_cast<PFN_glGenBuffers>(dlsym(gl, "glGenBuffers"));
    auto* bind_buffer = reinterpret_cast<PFN_glBindBuffer>(dlsym(gl, "glBindBuffer"));
    auto* buffer_data = reinterpret_cast<PFN_glBufferData>(dlsym(gl, "glBufferData"));
    auto* gen_framebuffers =
        reinterpret_cast<PFN_glGenFramebuffers>(dlsym(gl, "glGenFramebuffers"));
    auto* bind_framebuffer =
        reinterpret_cast<PFN_glBindFramebuffer>(dlsym(gl, "glBindFramebuffer"));
    auto* gen_renderbuffers =
        reinterpret_cast<PFN_glGenRenderbuffers>(dlsym(gl, "glGenRenderbuffers"));
    auto* bind_renderbuffer =
        reinterpret_cast<PFN_glBindRenderbuffer>(dlsym(gl, "glBindRenderbuffer"));
    auto* renderbuffer_storage =
        reinterpret_cast<PFN_glRenderbufferStorage>(dlsym(gl, "glRenderbufferStorage"));
    auto* framebuffer_renderbuffer =
        reinterpret_cast<PFN_glFramebufferRenderbuffer>(dlsym(gl, "glFramebufferRenderbuffer"));
    if (!choose || !create || !make_current || !destroy || !swap || !clear_colour || !clear ||
        !pixel_store || !get_integer || !get_error || !gen_buffers || !bind_buffer ||
        !buffer_data || !gen_framebuffers || !bind_framebuffer || !gen_renderbuffers ||
        !bind_renderbuffer || !renderbuffer_storage || !framebuffer_renderbuffer) {
        printf("FAIL a GL function did not resolve off the private handle\n");
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip the display did not open from inside the sandbox\n");
        return 77;
    }
    int attrs[] = {4 /*GLX_RGBA*/, 5 /*GLX_DOUBLEBUFFER*/, 0};
    XVisualInfo* visual = choose(display, DefaultScreen(display), attrs);
    if (!visual) {
        printf("FAIL no double-buffered visual\n");
        return 1;
    }
    const int W = 400, H = 400;
    XSetWindowAttributes swa;
    swa.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    swa.override_redirect = True;  // nothing on the owner's desktop
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, W, H, 0,
                                  visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWOverrideRedirect, &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    make_current(display, window, context);

    // The miniature game's own state, set once and never touched again.
    unsigned int pbo = 0;
    gen_buffers(1, &pbo);
    bind_buffer(kPixelUnpackBuffer, pbo);
    buffer_data(kPixelUnpackBuffer, 1 << 16, nullptr, 0x88E0 /*GL_STREAM_DRAW*/);
    pixel_store(kUnpackRowLength, kGameRowLength);
    pixel_store(kUnpackSkipRows, kGameSkipRows);
    pixel_store(kUnpackSkipPixels, kGameSkipPixels);
    pixel_store(kUnpackAlignment, kGameAlignment);
    // And its framebuffers: one bound for reading, another for drawing, the
    // shape of a game that composes its frame in an object of its own and
    // blits it to the window. The overlay retargets its draw to the window
    // and must put BOTH bindings back as they were.
    unsigned int read_fbo = 0;
    unsigned int draw_fbo = 0;
    for (unsigned int* fbo : {&read_fbo, &draw_fbo}) {
        unsigned int storage = 0;
        gen_renderbuffers(1, &storage);
        bind_renderbuffer(kRenderbuffer, storage);
        renderbuffer_storage(kRenderbuffer, kRgba8, W, H);
        gen_framebuffers(1, fbo);
        bind_framebuffer(kDrawFramebuffer, *fbo);
        framebuffer_renderbuffer(kDrawFramebuffer, kColorAttachment0, kRenderbuffer, storage);
    }
    bind_framebuffer(kReadFramebuffer, read_fbo);
    bind_framebuffer(kDrawFramebuffer, draw_fbo);
    while (get_error() != 0) {
    }  // the game's queue starts empty, so anything in it afterwards is ours

    for (int frame = 0; frame < 90 && uploads_reported(log_path) == 0; ++frame) {
        clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
        clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
        swap(display, window);
        usleep(16000);
    }

    // ---- what GL has to say about it.
    const unsigned int error = get_error();
    int row_length = -1, skip_rows = -1, skip_pixels = -1, alignment = -1, bound = -1;
    int read_bound = -1, draw_bound = -1;
    get_integer(kUnpackRowLength, &row_length);
    get_integer(kUnpackSkipRows, &skip_rows);
    get_integer(kUnpackSkipPixels, &skip_pixels);
    get_integer(kUnpackAlignment, &alignment);
    get_integer(kPixelUnpackBufferBinding, &bound);
    get_integer(kReadFramebufferBinding, &read_bound);
    get_integer(kDrawFramebufferBinding, &draw_bound);
    const long uploads = uploads_reported(log_path);

    printf("     after the overlay drew: error 0x%04x, row length %d, skip %d/%d, alignment %d, "
           "unpack buffer %d, read framebuffer %d (was %u), draw framebuffer %d (was %u), "
           "uploads %ld\n",
           error, row_length, skip_rows, skip_pixels, alignment, bound, read_bound, read_fbo,
           draw_bound, draw_fbo, uploads);

    check(error == 0, "the overlay pushed no GL error into the game's queue");
    check(row_length == kGameRowLength && skip_rows == kGameSkipRows &&
              skip_pixels == kGameSkipPixels && alignment == kGameAlignment,
          "the game's pixel-store state is exactly as it left it");
    check(bound == static_cast<int>(pbo), "and its pixel-unpack buffer is still bound");
    check(draw_bound == static_cast<int>(draw_fbo),
          "its draw framebuffer is bound again after the overlay drew into the window");
    check(read_bound == static_cast<int>(read_fbo),
          "and its READ framebuffer was never touched: the two bindings are two bindings");
    check(uploads > 0, "and the picture did upload, rather than being quietly skipped");

    make_current(display, 0, nullptr);
    destroy(display, context);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
