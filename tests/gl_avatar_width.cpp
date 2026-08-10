// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A face, at both widths, through the GL path.
//
// The owner reported a grey disc where his own picture should be, in The Binding
// of Isaac. Its record says what it is: `api = opengl`, executable
// `.../wine/i386-unix/wine-preloader` -- a **32-bit GL** process (wined3d rather
// than DXVK), which is the half of this install that has failed invisibly three
// times already. Entry 30 was `dlvsym` resolving to nothing on i386; entry 33 was
// the same line in a second file, where the failure is silent; entry 34 was an id
// formatted with `%lu` and truncated to 32 bits, so the game asked for a filename
// the 64-bit daemon never wrote. Each was invisible in the half that was covered.
//
// Which is why this probe exists at both widths rather than at 64 bits with a
// paragraph of reasoning: an avatar file that IS on disk, in the format the daemon
// writes, and the question of whether the overlay puts it on screen. The witness
// is the overlay's own log rather than the pixels, because the picture drawn here
// is a flat colour and telling it from the grey placeholder by colour is exactly
// the kind of detector DESIGN forbids on captures; `uploaded avatar` is the
// library saying it did the thing.
//
// The failure this is built to catch is not the path formatting -- widths.cpp
// already holds that -- but everything after it: the resolution of
// glGenTextures/glTexImage2D through gl_symbol in a process where nothing GL is in
// the global scope, at a width where that resolution has never been watched.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "private_shm.h"
#include "vocem/avatar_rgba.h"
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

long lines_saying(const char* path, const char* needle, const char* unless = nullptr) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return 0;
    }
    long count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle) && (!unless || !strstr(line, unless))) {
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

// The id is deliberately one that needs all sixty-four bits: entry 34's defect
// was invisible for every id that happens to fit in thirty-two.
const uint64_t kUserId = 1018972252676554842ull;
const char* const kHash = "d907b387c7c707262918a6a6709980f7";

}  // namespace

int main() {
    printf("     this build is %zu-bit\n", sizeof(void*) * 8);

    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no GLX drawable to draw into\n");
        return 77;
    }
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }

    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    alarm(120);

    char root[] = "/tmp/vocem-gl-avatar-width-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[800];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // The kernel truncates /proc/self/comm to fifteen characters, so both builds
    // of this probe are called `vocem_gl_avatar` -- and the 32-bit binary's file
    // name is `..._width32`, which the 64-bit spelling would not match. Naming the
    // truncated form covers both, and getting this wrong is how the first run of
    // this probe "measured" a library that had declined to draw at all.
    write_file(path, "enabled = true\nshown_apps = vocem_gl_avatar\n");
    setenv("XDG_CONFIG_HOME", root, 1);
    char cache[800];
    snprintf(cache, sizeof(cache), "%s/cache", root);
    mkdir(cache, 0700);
    setenv("XDG_CACHE_HOME", cache, 1);
    snprintf(path, sizeof(path), "%s/vocem", cache);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/avatars", cache);
    mkdir(path, 0700);
    char log_path[900];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_LOG_FILE", log_path, 1);

    // The picture, written the way the daemon writes it: the format's exact size,
    // at the path the *reader* asks for -- built here with the same one-place
    // spelling, so a probe cannot accidentally agree with a wrong reader.
    static unsigned char pixels[vocem::kAvatarRgbaBytes];
    for (uint32_t i = 0; i < vocem::kAvatarPixels * vocem::kAvatarPixels; ++i) {
        pixels[i * 4 + 0] = 0x20;
        pixels[i * 4 + 1] = 0xd0;
        pixels[i * 4 + 2] = 0x40;
        pixels[i * 4 + 3] = 0xff;
    }
    char face_path[900];
    vocem::avatar_rgba_path(face_path, sizeof(face_path), kUserId, kHash);
    printf("     the picture goes to %s\n", face_path);
    if (FILE* out = fopen(face_path, "wb")) {
        fwrite(pixels, 1, sizeof(pixels), out);
        fclose(out);
    }
    struct stat face_info{};
    check(stat(face_path, &face_info) == 0 &&
              face_info.st_size == static_cast<off_t>(vocem::kAvatarRgbaBytes),
          "the picture is on disk at the format's exact size");

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "one face");
        state.user_count = 1;
        state.users[0].id = kUserId;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Owner");
        snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash), "%s", kHash);
    });
    if (failures) {
        return 1;
    }

    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gl) {
        printf("skip libGL.so.1 is not installed for this width\n");
        return 77;
    }
    auto* choose = reinterpret_cast<PFN_glXChooseVisual>(dlsym(gl, "glXChooseVisual"));
    auto* create = reinterpret_cast<PFN_glXCreateContext>(dlsym(gl, "glXCreateContext"));
    auto* make_current = reinterpret_cast<PFN_glXMakeCurrent>(dlsym(gl, "glXMakeCurrent"));
    auto* destroy = reinterpret_cast<PFN_glXDestroyContext>(dlsym(gl, "glXDestroyContext"));
    auto* swap = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(gl, "glXSwapBuffers"));
    auto* clear_colour = reinterpret_cast<PFN_glClearColor>(dlsym(gl, "glClearColor"));
    auto* clear = reinterpret_cast<PFN_glClear>(dlsym(gl, "glClear"));
    if (!choose || !create || !make_current || !destroy || !swap || !clear_colour || !clear) {
        printf("skip GL did not resolve for this width\n");
        return 77;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip the display did not open from inside the sandbox\n");
        return 77;
    }
    int attrs[] = {4 /*GLX_RGBA*/, 5 /*GLX_DOUBLEBUFFER*/, 0};
    XVisualInfo* visual = choose(display, DefaultScreen(display), attrs);
    if (!visual) {
        printf("skip no double-buffered visual\n");
        return 77;
    }
    XSetWindowAttributes swa;
    swa.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    swa.override_redirect = True;  // nothing on the owner's desktop
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, 360, 360,
                                  0, visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWOverrideRedirect, &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    if (!context) {
        printf("skip no GL context for this width\n");
        return 77;
    }
    make_current(display, window, context);

    for (int frame = 0; frame < 60; ++frame) {
        clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
        clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
        swap(display, window);
        usleep(16000);
    }

    // What the library says it did. Three outcomes are told apart on purpose,
    // because "no face" has three different causes and they need different fixes.
    // "not drawing in X: reason" contains "drawing in": the first version of this
    // counted the refusal as a success, which is the same trap in miniature.
    const long drew = lines_saying(log_path, "drawing in", "not drawing");
    const long declined = lines_saying(log_path, "not drawing in");
    const long uploaded = lines_saying(log_path, "uploaded avatar");
    const long gave_up = lines_saying(log_path, "gave up waiting");
    printf("     log: %ld 'drawing in', %ld 'not drawing in', %ld 'uploaded avatar', "
           "%ld 'gave up waiting'\n", drew, declined, uploaded, gave_up);

    check(declined == 0, "the overlay did not decline this process");
    check(drew > 0, "the overlay drew in this process");
    check(gave_up == 0, "and did not give up waiting for a file that is right there");
    check(uploaded > 0, "and it uploaded the face");

    destroy(display, context);
    XCloseDisplay(display);
    char cleanup[900];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
