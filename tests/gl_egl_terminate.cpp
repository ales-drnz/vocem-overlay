// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// eglTerminate of the backend's display while ANOTHER context on it is current,
// and what the overlay does to that context.
//
// The EGL teardown hook is told of an eglTerminate with no context, and it
// used to shut the backend down -- deleting its program, buffers and font
// texture by name -- whenever ANY context was current on the calling thread:
// `release(previous != nullptr)`. In a context the backend does not live in
// those names are that context's own objects. Measured by the 0.1.10 review:
// the game's second, unshared context lost 1 of its 16 textures, 2 of its 16
// buffers and 1 of its 4 programs to one eglTerminate (legal with a context
// current: it stays usable until released). The backend is shut down with GL
// calls only when the context current is the one it lives in now.
//
// The scene: the overlay comes up in context 1 and uploads its atlas; context
// 2, unshared, makes 16 textures, 16 buffers and 4 programs of its own and is
// current when eglTerminate runs. All of them must still be there after.

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

}  // namespace

int main() {
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(30, "an eglTerminate beside another context");

    char root[] = "/tmp/vocem-gl-terminate-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule =
        "enabled = true\nshown_apps = " + vocem_test::own_name("vocem_gl_egl_terminate") + "\n";
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
        snprintf(state.channel_name, sizeof(state.channel_name), "terminate");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Terminate");
    });

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
        printf("skip no RGBA8 ES 2 pbuffer configuration here\n");
        return 77;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint surface_attributes[] = {EGL_WIDTH, 320, EGL_HEIGHT, 240, EGL_NONE};
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLSurface first_surface = eglCreatePbufferSurface(display, config, surface_attributes);
    EGLContext first = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (first_surface == EGL_NO_SURFACE || first == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, first_surface, first_surface, first)) {
        printf("skip no ES 2 pbuffer context here\n");
        return 77;
    }
    const double deadline = now_ms() + 5000.0;
    while (now_ms() < deadline &&
           lines_containing(log_path, "font texture uploaded whole") < 1) {
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, first_surface);
        usleep(5000);
    }
    for (int frame = 0; frame < 5; ++frame) {
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, first_surface);
    }
    check(lines_containing(log_path, "OpenGL backend ready") == 1 &&
              lines_containing(log_path, "font texture uploaded whole") >= 1,
          "the backend came up in the first context with its atlas");

    // The game's second, unshared context, with objects of its own -- the
    // same low names the backend holds in the first.
    EGLSurface second_surface = eglCreatePbufferSurface(display, config, surface_attributes);
    EGLContext second = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    eglMakeCurrent(display, second_surface, second_surface, second);
    GLuint textures[16];
    GLuint buffers[16];
    GLuint programs[4];
    glGenTextures(16, textures);
    for (GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenBuffers(16, buffers);
    for (GLuint buffer : buffers) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (GLuint& program : programs) {
        program = glCreateProgram();
    }
    const auto census = [&](int& t, int& b, int& p) {
        t = b = p = 0;
        for (int i = 0; i < 16; ++i) {
            t += glIsTexture(textures[i]) ? 1 : 0;
            b += glIsBuffer(buffers[i]) ? 1 : 0;
        }
        for (GLuint program : programs) {
            p += glIsProgram(program) ? 1 : 0;
        }
    };
    int t = 0;
    int b = 0;
    int p = 0;
    census(t, b, p);
    printf("     before eglTerminate, the second context holds %d/16 textures, %d/16 buffers, "
           "%d/4 programs\n", t, b, p);
    eglTerminate(display);  // legal with a context current: it stays usable until released
    census(t, b, p);
    printf("     after eglTerminate: %d/16 textures, %d/16 buffers, %d/4 programs\n", t, b, p);
    check(t == 16 && b == 16 && p == 4,
          "eglTerminate leaves every object of a context the backend does not live in");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
