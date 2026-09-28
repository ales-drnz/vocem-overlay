// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What a drawn frame costs the filesystem, on the path entry 49 stopped at.
//
// `fonts_frame_quiet` proved the emoji half is silent in the steady state with a
// filter that kills on any file syscall. The avatar half cannot be measured that
// way, because it is *supposed* to touch files -- the question is how often, and
// the honest answer needs a count rather than a verdict. So this probe interposes
// the file syscalls in its own executable (a definition in the main program wins
// over the dlopened library's lookups) and counts them per frame, around three
// phases of the one thing that actually happens in a game:
//
//   1. **nobody's picture has arrived yet.** The daemon is still downloading, and
//      the overlay looks again twice a second -- so the cost per frame must be a
//      fraction of the faces, not one stat per face per frame.
//   2. **the pictures land.** Whatever the policy is here, the frame that notices
//      them must not read and upload all of them at once: the Vulkan side caps
//      itself at one upload per pass and says so in its header; the GL side never
//      adopted that, and this is where the difference shows as a number.
//   3. **everything is resolved.** Faces cached, or given up on and cached as
//      negatives: from here a drawn frame must cost *nothing at all*, or the
//      overlay is paying rent in every frame of somebody's game forever.
//
// Two instruments, because one does not reach: `stat` is called by the overlay
// directly and an interposed definition here catches every one of them, filtered
// to this test's own avatar directory so the config file's own two-second stat
// (deliberate, documented) stays out of the numbers. The *opens* are invisible
// this way, and that is worth writing down rather than discovering twice:
// `avatar_rgba_load` opens through `fopen`, and glibc reaches the openat syscall
// internally, never through a PLT an executable can interpose. So the uploads are
// counted where the overlay itself reports them -- `VOCEM_LOG_FILE`, one line per
// upload, which is a witness the overlay writes rather than one the test infers.
//
// The numbers this prints are the finding; the assertions are the parts of it
// that must not regress.

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "probe_alarm.h"
#include "probe_name.h"

#include <string>
#include "vocem/avatar_rgba.h"
#include "private_shm.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// The overlay asking "has it arrived?". Its own reads are counted elsewhere: see
// the note at the top of this file.
volatile long g_stats = 0;

void write_file(const char* path, const char* contents) {
    if (FILE* file = fopen(path, "w")) {
        fputs(contents, file);
        fclose(file);
    }
}

using PFN_glXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFN_glXCreateContext = void* (*)(Display*, XVisualInfo*, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXDestroyContext = void (*)(Display*, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_glClearColor = void (*)(float, float, float, float);
using PFN_glClear = void (*)(unsigned int);

// How many avatars the overlay says it has uploaded so far, read out of its own
// log. One line per upload, written before the work (entry 46's rule).
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

}  // namespace

// The interposition. Only the paths under this test's own cache directory are
// counted: the loader, the config file and X's own traffic go through the same
// functions and would drown the signal.
namespace {

const char* g_watched = nullptr;

bool watched(const char* path) {
    return g_watched && path && strstr(path, g_watched) != nullptr;
}

}  // namespace

extern "C" {

int stat(const char* path, struct stat* out) {
    using PFN = int (*)(const char*, struct stat*);
    static PFN real = reinterpret_cast<PFN>(dlsym(RTLD_NEXT, "stat"));
    if (watched(path)) {
        ++g_stats;
    }
    return real(path, out);
}

}  // extern "C"

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

    vocem_test::set_alarm(180, "drawing and counting avatar reads");

    char root[] = "/tmp/vocem-gl-avatar-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[700];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // The rule names THIS binary, read off /proc/self/exe: a literal is
    // wrong the day the binary is renamed and wrong at -m32 today
    // (tests/probe_name.h, entry 129).
    const std::string rule = "enabled = true\nshown_apps = " +
                             vocem_test::own_name("vocem_gl_avatar_quiet") + "\n";
    write_file(path, rule.c_str());
    setenv("XDG_CONFIG_HOME", root, 1);
    char cache[700];
    snprintf(cache, sizeof(cache), "%s/cache", root);
    mkdir(cache, 0700);
    setenv("XDG_CACHE_HOME", cache, 1);
    char avatars[700];
    snprintf(avatars, sizeof(avatars), "%s/vocem", cache);
    mkdir(avatars, 0700);
    snprintf(avatars, sizeof(avatars), "%s/vocem/avatars", cache);
    mkdir(avatars, 0700);
    // Only avatar files are counted, so the config file's own stat cadence and
    // every unrelated open stay out of the numbers.
    static char watched[700];
    snprintf(watched, sizeof(watched), "/vocem/avatars/");
    g_watched = watched;
    // The overlay's own report of its uploads. VOCEM_LOG_FILE exists because the
    // Minecraft launcher swallows a game's stderr (entry 38); here it is the only
    // witness that can see through glibc's internal open.
    static char log_path[800];
    snprintf(log_path, sizeof(log_path), "%s/overlay.log", root);
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path, 1);

    const uint32_t kFaces = 6;
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([kFaces](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "avatars");
        state.user_count = kFaces;
        for (uint32_t i = 0; i < kFaces; ++i) {
            state.users[i].id = 900 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Face %u", i + 1);
            snprintf(state.users[i].avatar_hash, sizeof(state.users[i].avatar_hash),
                     "abcdef01%04u", i);
        }
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
    if (!choose || !create || !make_current || !destroy || !swap || !clear_colour || !clear) {
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

    auto run_frames = [&](int frames, long* stats, long* uploads) {
        const long stats0 = g_stats;
        const long uploads0 = uploads_reported(log_path);
        for (int frame = 0; frame < frames; ++frame) {
            clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
            clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
            swap(display, window);
            usleep(16000);
        }
        *stats = g_stats - stats0;
        *uploads = uploads_reported(log_path) - uploads0;
    };

    // Let the overlay come up and settle before anything is counted: the backend,
    // the atlas and the first look at each face all happen once.
    long stats = 0, uploads = 0;
    run_frames(45, &stats, &uploads);

    // --- 1. waiting for pictures that have not arrived -------------------
    // 60 frames is about a second, so with a half-second retry the honest budget
    // is roughly two looks per face, not sixty.
    run_frames(60, &stats, &uploads);
    printf("     waiting, 60 frames, %u faces: %ld stats, %ld uploads\n", kFaces, stats, uploads);
    check(stats < static_cast<long>(kFaces) * 8,
          "a face that has not arrived is looked at on a cadence, not every frame");
    check(uploads == 0, "and nothing is uploaded while there is nothing to upload");

    // --- 2. the pictures land --------------------------------------------
    static unsigned char face[vocem::kAvatarRgbaBytes];
    for (uint32_t i = 0; i < vocem::kAvatarPixels * vocem::kAvatarPixels; ++i) {
        face[i * 4 + 0] = static_cast<unsigned char>(0x30 + (i & 0x3f));
        face[i * 4 + 1] = 0x80;
        face[i * 4 + 2] = static_cast<unsigned char>(0xc0 - (i & 0x3f));
        face[i * 4 + 3] = 0xff;
    }
    for (uint32_t i = 0; i < kFaces; ++i) {
        char file[900];
        char hash[32];
        snprintf(hash, sizeof(hash), "abcdef01%04u", i);
        vocem::avatar_rgba_path(file, sizeof(file), 900 + i, hash);
        FILE* out = fopen(file, "wb");
        if (out) {
            fwrite(face, 1, sizeof(face), out);
            fclose(out);
        }
    }
    // One frame at a time, so the burst is visible as a number rather than
    // averaged away: how many pictures does a single frame take on?
    long worst_in_a_frame = 0;
    long picked_up = 0;
    for (int frame = 0; frame < 120; ++frame) {
        long s = 0, u = 0;
        run_frames(1, &s, &u);
        picked_up += u;
        if (u > worst_in_a_frame) {
            worst_in_a_frame = u;
        }
    }
    printf("     arriving: %ld of %u pictures taken on, busiest single frame %ld\n", picked_up,
           kFaces, worst_in_a_frame);
    check(picked_up >= static_cast<long>(kFaces), "every picture is picked up once it exists");
    check(worst_in_a_frame <= 1,
          "and a frame takes on at most one of them, as the Vulkan side does");

    // --- 3. everything resolved -----------------------------------------
    run_frames(60, &stats, &uploads);
    printf("     resolved, 60 frames: %ld stats, %ld uploads\n", stats, uploads);
    check(stats == 0 && uploads == 0,
          "a frame whose faces are all resolved asks the filesystem nothing");

    destroy(display, context);
    XCloseDisplay(display);
    char cleanup[800];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
