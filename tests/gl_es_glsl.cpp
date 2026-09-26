// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// An OpenGL ES game in which the overlay cannot ask what the context is until
// after its backend has chosen a shader language.
//
// The backend's GLSL has to match the context: "#version 300 es" for ES 3,
// "#version 100" for ES 2, and ImGui's own default, "#version 130", only for
// desktop GL. The overlay reads GL_VERSION before it initialises the backend
// to choose -- but in a game that opens libEGL and libGLESv2 RTLD_LOCAL and
// never resolves a dispatcher of its own, glGetString cannot be found at that
// moment: nothing GL is in the global scope and the shim has no dispatcher to
// forward to. The backend was initialised with no version, took #version 130,
// and the ES context refused it; the version read AFTER Init (by then ImGui's
// own loader had revealed a dispatcher) said "OpenGL ES", and nothing was done
// with it. "OpenGL backend ready" was logged over a program that never linked,
// and every frame's draw was rejected into the game's error queue.
//
// This probe is that game: EGL and GLES from their own handles, a pbuffer, no
// eglGetProcAddress ever asked. It publishes a channel, presents until the
// atlas is up, and then looks at the back buffer the overlay drew into (the
// pbuffer's swap leaves it in place). Required: the overlay's pixels -- 0
// against the 0.1.10 library at both widths, over 42 frames -- and a log that
// says the ES context was seen and the program linked.
//
// The game's error queue is counted and printed, and NOT asserted: on NVIDIA
// the dead program was 42 errors in 42 frames and the fixed library leaves 0,
// but Mesa's ES 3.2 (the 32-bit run here) flags the vendored backend's own
// glIsEnabled/glDisable(GL_PRIMITIVE_RESTART), a desktop-only enum, on every
// frame whatever the program -- the known ES3 errors DESIGN entry 68 names.
// Asserting the queue would measure that, not this.

#include <EGL/egl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

void print_lines(const char* path, const char* needle) {
    if (FILE* file = fopen(path, "r")) {
        char line[1024];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, needle)) {
                printf("     log: %s", line);
            }
        }
        fclose(file);
    }
}

double now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) / 1e6;
}

}  // namespace

#define RESOLVE(handle, type, name) auto name = reinterpret_cast<type>(dlsym(handle, #name))

int main() {
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(30, "an ES game with no dispatcher of its own");

    char root[] = "/tmp/vocem-gl-es-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule =
        "enabled = true\nshown_apps = " + vocem_test::own_name("vocem_gl_es_glsl") + "\n";
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
        snprintf(state.channel_name, sizeof(state.channel_name), "es");
        state.user_count = 2;
        for (uint32_t i = 0; i < 2; ++i) {
            state.users[i].id = 900 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Es %u", i + 1);
        }
    });

    // The game's half: its own handles, and no dispatcher asked for, ever.
    void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    void* gles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!egl || !gles) {
        printf("skip libEGL.so.1 and libGLESv2.so.2 are not both installed\n");
        return 77;
    }
    RESOLVE(egl, EGLDisplay (*)(void*), eglGetDisplay);
    RESOLVE(egl, EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*), eglInitialize);
    RESOLVE(egl, EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*),
            eglChooseConfig);
    RESOLVE(egl, EGLBoolean (*)(EGLenum), eglBindAPI);
    RESOLVE(egl, EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*), eglCreatePbufferSurface);
    RESOLVE(egl, EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*),
            eglCreateContext);
    RESOLVE(egl, EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext), eglMakeCurrent);
    RESOLVE(egl, EGLBoolean (*)(EGLDisplay, EGLSurface), eglSwapBuffers);
    RESOLVE(gles, void (*)(float, float, float, float), glClearColor);
    RESOLVE(gles, void (*)(unsigned), glClear);
    RESOLVE(gles, unsigned (*)(), glGetError);
    RESOLVE(gles, void (*)(int, int, int, int, unsigned, unsigned, void*), glReadPixels);
    if (!eglGetDisplay || !eglInitialize || !eglChooseConfig || !eglBindAPI ||
        !eglCreatePbufferSurface || !eglCreateContext || !eglMakeCurrent || !eglSwapBuffers ||
        !glClearColor || !glClear || !glGetError || !glReadPixels) {
        printf("FAIL EGL and GLES do not resolve off their own handles\n");
        return 1;
    }
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        printf("skip no EGL display here\n");
        return 77;
    }
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0) {
        printf("skip no RGBA8 ES pbuffer configuration here\n");
        return 77;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    constexpr int W = 360;
    constexpr int H = 360;
    const EGLint surface_attributes[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    // ES 3: the version the review's ES game ran and the one whose GLSL is
    // "#version 300 es"; a driver without it gives ES 2, which is also ES.
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        printf("skip no ES pbuffer context here\n");
        return 77;
    }

    long errors = 0;
    const auto frame = [&]() {
        glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
        glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
        eglSwapBuffers(display, surface);
        while (glGetError() != 0) {
            ++errors;
        }
        usleep(16000);
    };
    const double deadline = now_ms() + 8000.0;
    while (now_ms() < deadline && lines_containing(log_path, "font texture uploaded whole") < 1) {
        frame();
    }
    const long errors_before_drawing = errors;
    for (int i = 0; i < 40; ++i) {
        frame();
    }
    // The overlay drew into the back buffer before the swap, and a pbuffer's
    // swap leaves it where it is: this frame's clear has not happened yet.
    static unsigned char pixels[W * H * 4];
    glReadPixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels);
    long foreign = 0;
    for (long i = 0; i < static_cast<long>(W) * H; ++i) {
        const int dr = abs(pixels[i * 4 + 0] - 26);
        const int dg = abs(pixels[i * 4 + 1] - 38);
        const int db = abs(pixels[i * 4 + 2] - 51);
        if (dr > 40 || dg > 40 || db > 40) {
            ++foreign;
        }
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    print_lines(log_path, "context:");
    print_lines(log_path, "GLSL");
    print_lines(log_path, "backend");
    printf("     %ld GL error(s) in the game's queue after its swaps (%ld before the atlas was "
           "up), %ld pixel(s) of the overlay's\n",
           errors, errors_before_drawing, foreign);
    check(lines_containing(log_path, "context: OpenGL ES") >= 1,
          "the overlay learned the context is OpenGL ES");
    check(lines_containing(log_path, "did not link") == 0,
          "the backend's shader program linked in the ES context");
    check(foreign > 0, "and draws: its shader program is one the ES context accepted");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
