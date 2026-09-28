// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One context the backend cannot live in, and the overlay in every other.
//
// 0.1.11's first round made "ready" mean "the program linked": a context whose
// program does not link sets the backend's failure flag and returns. But the
// flag was set before the context was remembered as the owner, and only an
// owner's teardown -- or the switch, or the daemon stopping -- clears it: a
// splash or helper context the overlay cannot draw in took the overlay away
// from the whole process for its whole life. 0.1.10 had no such flag and moved
// on at the hand-over.
//
// The failing context is made deterministic with Mesa's software renderer on
// EGL, where MESA_GL_VERSION_OVERRIDE=2.1 turns a desktop OpenGL context into
// one without GLSL 1.30 -- the backend's desktop default, "#version 130" -- while
// an OpenGL ES 2 context of the same display is untouched and gets "#version
// 100". So: F (desktop, 2.1) presents first and fails to link; then G (ES 2)
// presents. VOCEM_GL_SCENARIO=silent: F falls silent and G presents for longer
// than the hand-over. `destroyed`: F is destroyed, then G presents. Either way
// G must end up with the overlay: a "backend ready" line and overlay pixels in
// its frame -- before the fix, neither, however long G presents.

#include <EGL/egl.h>
#include <GLES2/gl2.h>
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

constexpr int kWidth = 320;
constexpr int kHeight = 240;

}  // namespace

int main() {
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    if (!getenv("MESA_GL_VERSION_OVERRIDE")) {
        printf("skip meant to run on Mesa with MESA_GL_VERSION_OVERRIDE=2.1\n");
        return 77;
    }
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "silent";
    const bool destroy_scene = strcmp(scenario, "destroyed") == 0;
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "a failed context and a good one");

    char root[] = "/tmp/vocem-gl-failed-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    vocem_test::overlay_config_home(root, vocem_test::own_name("vocem_gl_failed_context"));
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
        snprintf(state.channel_name, sizeof(state.channel_name), "failed");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Failed");
    });

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        printf("skip no EGL display here\n");
        return 77;
    }
    const char* vendor = eglQueryString(display, EGL_VENDOR);
    printf("     EGL vendor: %s\n", vendor ? vendor : "(none)");
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                                        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0) {
        printf("skip no RGBA8 pbuffer configuration for both OpenGL and OpenGL ES 2 here\n");
        return 77;
    }
    const EGLint surface_attributes[] = {EGL_WIDTH, kWidth, EGL_HEIGHT, kHeight, EGL_NONE};
    EGLSurface surfaces[2];
    EGLContext contexts[2];
    eglBindAPI(EGL_OPENGL_API);
    contexts[0] = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    contexts[1] = eglCreateContext(display, config, EGL_NO_CONTEXT, es2);
    for (int i = 0; i < 2; ++i) {
        surfaces[i] = eglCreatePbufferSurface(display, config, surface_attributes);
        if (surfaces[i] == EGL_NO_SURFACE || contexts[i] == EGL_NO_CONTEXT) {
            printf("skip a desktop and an ES 2 pbuffer context cannot be made here\n");
            return 77;
        }
    }
    const auto make_current = [&](int who) {
        eglBindAPI(who == 0 ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
        eglMakeCurrent(display, surfaces[who], surfaces[who], contexts[who]);
    };
    const auto present_for = [&](int who, double seconds) {
        make_current(who);
        const double end = now_ms() + seconds * 1000.0;
        while (now_ms() < end) {
            glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surfaces[who]);
            usleep(16000);
        }
    };

    make_current(0);
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    printf("     F: %s\n", version ? version : "(no version)");
    present_for(0, 0.5);
    const long refused = lines_containing(log_path, "did not link") +
                         lines_containing(log_path, "failed to initialise") +
                         lines_containing(log_path, "could not create its GL objects");
    check(refused >= 1, "the backend could not be made in F");
    check(lines_containing(log_path, "OpenGL backend ready") == 0, "and F has none");

    if (destroy_scene) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, contexts[0]);
        present_for(1, 1.5);
    } else {
        present_for(1, 3.5);  // longer than the hand-over, F silent all along
    }
    // G's own frame, read back from its pbuffer after its last present: the
    // probe clears to pure blue and draws nothing else.
    make_current(1);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(display, surfaces[1]);
    static unsigned char pixels[kWidth * kHeight * 4];
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    long overlay = 0;
    for (int i = 0; i < kWidth * kHeight; ++i) {
        const unsigned char* p = pixels + i * 4;
        overlay += (p[0] != 0 || p[1] != 0 || p[2] != 255) ? 1 : 0;
    }
    const long ready = lines_containing(log_path, "OpenGL backend ready");
    printf("     after F %s: %ld backend(s) ready, %ld overlay pixel(s) in G's frame\n",
           destroy_scene ? "was destroyed" : "fell silent", ready, overlay);
    check(ready == 1, "the backend came up in G");
    check(overlay > 0, "and G's frame carries the overlay");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
