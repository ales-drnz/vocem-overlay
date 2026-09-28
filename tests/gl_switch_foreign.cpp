// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The switch turned off -- or the daemon stopping -- noticed in a present
// from a context the backend does not live in, and what the overlay does to
// that context.
//
// Both are noticed by whichever present comes first after the settings'
// two-second stat or the state poll, and both hand the backend back from
// inside that present. It used to be `release(true)`: shut the backend down
// with GL calls in the context current NOW. A game presenting two unshared
// contexts (entry 210's scene: a tool window, an emulator's debugger) has the
// second one current about half the time, and there the backend's names --
// its program, its buffers, its font texture -- are that context's own
// objects: entry 237's defect, through the switch rather than eglTerminate.
// The backend is now shut down in its own context and nowhere else: from a
// foreign present it is left there (move_away) and deleted the next time that
// context presents (reclaim_left).
//
// The scene: the overlay comes up in context 1; context 2, unshared, makes 16
// textures, 16 buffers and 4 programs of its own and presents every 5 ms;
// context 1 presents once a second, so it stays the owner. The settings
// switch goes off (VOCEM_GL_SCENARIO=switch) or the segment is unlinked
// (=daemon). The probe knows which present noticed, because the release is
// logged inside it; an attempt noticed by context 1's own present is said and
// taken again. After: context 2 still holds every object, and context 1's
// next present deletes what was left in it, once.

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

// A log not written yet has no lines.
long count_lines(const char* path, const char* needle) {
    const long count = lines_containing(path, needle);
    return count < 0 ? 0 : count;
}

double now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) / 1e6;
}

char g_config[700];
std::string g_rule;

void write_config(bool enabled) {
    if (FILE* file = fopen(g_config, "w")) {
        fprintf(file, "enabled = %s\n%s", enabled ? "true" : "false", g_rule.c_str());
        fclose(file);
    }
}

void publish(vocem::StateWriter& writer) {
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "switch");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Switch");
    });
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
    const char* scenario = getenv("VOCEM_GL_SCENARIO");
    const bool daemon = scenario && strcmp(scenario, "daemon") == 0;
    vocem_test::set_alarm(60, daemon ? "the daemon stopping in a foreign present"
                                     : "the switch turned off in a foreign present");

    char root[] = "/tmp/vocem-gl-switch-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(g_config, sizeof(g_config), "%s/vocem/config.ini", root);
    g_rule = "shown_apps = " + vocem_test::own_name("vocem_gl_switch_foreign") + "\n";
    write_config(true);
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
    publish(writer);

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
    EGLSurface second_surface = eglCreatePbufferSurface(display, config, surface_attributes);
    EGLContext second = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (first_surface == EGL_NO_SURFACE || first == EGL_NO_CONTEXT ||
        second_surface == EGL_NO_SURFACE || second == EGL_NO_CONTEXT) {
        printf("skip no two ES 2 pbuffer contexts here\n");
        return 77;
    }
    const auto present = [&](bool in_first) {
        EGLSurface surface = in_first ? first_surface : second_surface;
        eglMakeCurrent(display, surface, surface, in_first ? first : second);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, surface);
    };
    // Context 1 until the backend is up in it with the worker's atlas.
    const auto bring_up = [&]() {
        const long uploads = count_lines(log_path, "font texture uploaded whole");
        const double deadline = now_ms() + 8000.0;
        while (now_ms() < deadline &&
               count_lines(log_path, "font texture uploaded whole") <= uploads) {
            present(true);
            usleep(5000);
        }
        for (int frame = 0; frame < 5; ++frame) {
            present(true);
        }
        return count_lines(log_path, "font texture uploaded whole") > uploads;
    };
    check(bring_up(), "the backend came up in the first context with its atlas");

    // The game's second, unshared context, with objects of its own -- the
    // same low names the backend holds in the first.
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
        eglMakeCurrent(display, second_surface, second_surface, second);
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
    printf("     the second context holds %d/16 textures, %d/16 buffers, %d/4 programs\n", t, b,
           p);

    const char* const released = daemon ? "the daemon stopped: releasing" : "switched off: releasing";
    bool noticed_in_second = false;
    for (int attempt = 1; attempt <= 3 && !noticed_in_second; ++attempt) {
        const long before = count_lines(log_path, released);
        printf("     >>> attempt %d: %s\n", attempt,
               daemon ? "the segment is unlinked" : "the switch goes off");
        if (daemon) {
            writer.close();
            vocem::StateWriter::unlink_segment();
        } else {
            write_config(false);
        }
        double next_first = now_ms() + 1000.0;
        const double deadline = now_ms() + 8000.0;
        bool noticed = false;
        while (!noticed && now_ms() < deadline) {
            const bool in_first = now_ms() >= next_first;
            if (in_first) {
                next_first += 1000.0;
            }
            present(in_first);
            if (count_lines(log_path, released) > before) {
                noticed = true;
                noticed_in_second = !in_first;
                printf("     noticed by a present of context %d\n", in_first ? 1 : 2);
            }
            usleep(5000);
        }
        check(noticed, "the release was noticed within eight seconds");
        if (!noticed) {
            break;
        }
        if (!noticed_in_second && attempt < 3) {
            // The owner's own present noticed: nothing foreign was asked.
            // On again, and the backend back in context 1, for another try.
            if (daemon) {
                check(writer.open(), "the private state segment opens again");
                publish(writer);
            } else {
                write_config(true);
            }
            check(bring_up(), "the backend came back in the first context");
        }
    }
    check(noticed_in_second, "a present of the second context noticed the release");

    census(t, b, p);
    printf("     after the release: %d/16 textures, %d/16 buffers, %d/4 programs\n", t, b, p);
    check(t == 16 && b == 16 && p == 4,
          "the release leaves every object of the context the backend does not live in");

    const long deleted_before = count_lines(log_path, "deleted the backend left in context");
    for (int frame = 0; frame < 3; ++frame) {
        present(true);
    }
    const long deleted = count_lines(log_path, "deleted the backend left in context") -
                         deleted_before;
    printf("     context 1's next presents deleted %ld backend(s) left in it\n", deleted);
    check(deleted == 1, "the backend's own context deletes it, once, when it presents again");
    census(t, b, p);
    check(t == 16 && b == 16 && p == 4, "and the second context still holds everything");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, second);
    eglDestroyContext(display, first);
    eglTerminate(display);

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
