// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay released while its first font atlas is still being rasterised,
// and how long the game's swap is held for it.
//
// The first atlas is built on a worker beside the game (entry 192), and every
// teardown of the backend joins that worker first (vocem/atlas_owner.h). On
// OpenGL three of those teardowns are noticed inside a swap: the switch turned
// off, the daemon stopping, and the hand-over to another context once the
// holder has been silent. Joining there held the game's glXSwapBuffers /
// eglSwapBuffers for the rest of the build -- 142-151 ms for a 2160-line
// display, measured by the 2026-09-28 refutation pass. A release noticed
// mid-build now waits for the build without blocking: the frame goes out
// without the overlay and a later present, once the worker is done, releases.
//
// The scene makes the build as long as it likes: the process on one CPU, the
// worker (the thread named vocem-atlas) moved to SCHED_IDLE, and a thread of
// the probe's spinning beside it for five seconds from the moment the release
// is asked, so the worker barely runs until the spinner stops. Then
// VOCEM_GL_SCENARIO=switch turns the switch off, =daemon unlinks the segment,
// =handover lets the backend's context fall silent while a second one
// presents. Measured: the longest swap between the question and the spinner
// stopping. Holding the swap for the build is seconds here; not holding it is
// a few milliseconds. And the release still happens, once the build is done.

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

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
        return 0;  // a log not written yet has no lines
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
        snprintf(state.channel_name, sizeof(state.channel_name), "mid-build");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Build");
    });
}

// The atlas worker, found by the name it gives itself, moved to SCHED_IDLE.
bool idle_the_worker() {
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks) {
        return false;
    }
    bool found = false;
    while (dirent* entry = readdir(tasks)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        char name[64] = {0};
        if (FILE* comm = fopen(path, "r")) {
            if (!fgets(name, sizeof(name), comm)) {
                name[0] = 0;
            }
            fclose(comm);
        }
        if (strncmp(name, "vocem-atlas", 11) == 0) {
            const sched_param none{};
            found = sched_setscheduler(atoi(entry->d_name), SCHED_IDLE, &none) == 0;
        }
    }
    closedir(tasks);
    return found;
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
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "";
    const bool daemon = strcmp(scenario, "daemon") == 0;
    const bool handover = strcmp(scenario, "handover") == 0;
    if (!daemon && !handover && strcmp(scenario, "switch") != 0) {
        printf("FAIL VOCEM_GL_SCENARIO is switch, daemon or handover\n");
        return 1;
    }
    vocem_test::set_alarm(90, "a release asked while the first atlas is rasterised");

    // One CPU for the whole process, every thread made from here on included.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) {
                cpu_set_t one;
                CPU_ZERO(&one);
                CPU_SET(cpu, &one);
                if (sched_setaffinity(0, sizeof(one), &one) == 0) {
                    printf("     the process runs on CPU %d alone\n", cpu);
                }
                break;
            }
        }
    }

    char root[] = "/tmp/vocem-gl-midbuild-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(g_config, sizeof(g_config), "%s/vocem/config.ini", root);
    g_rule = "shown_apps = " + vocem_test::own_name("vocem_gl_release_mid_build") + "\n";
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
    // One frame; the swap's duration in milliseconds.
    const auto present = [&](bool in_first) {
        EGLSurface surface = in_first ? first_surface : second_surface;
        eglMakeCurrent(display, surface, surface, in_first ? first : second);
        glClear(GL_COLOR_BUFFER_BIT);
        const double before = now_ms();
        eglSwapBuffers(display, surface);
        return now_ms() - before;
    };

    // The first frames, in context 1, until the worker has been started.
    const double start_deadline = now_ms() + 8000.0;
    while (now_ms() < start_deadline && lines_containing(log_path, "off the game's thread") == 0) {
        present(true);
        usleep(5000);
    }
    const bool idled = idle_the_worker();
    check(lines_containing(log_path, "off the game's thread") == 1,
          "the first atlas went to the worker");
    check(idled, "the worker was found by its name and moved to SCHED_IDLE");

    // The spinner: from now until five seconds after the release is asked.
    std::atomic<double> spin_until{now_ms() + 60000.0};
    std::thread spinner([&] {
        volatile unsigned long sink = 0;
        while (now_ms() < spin_until.load()) {
            for (int i = 0; i < 100000; ++i) {
                sink = sink + static_cast<unsigned long>(i);
            }
        }
    });
    // A few frames of context 1 with the build starved.
    for (int frame = 0; frame < 5; ++frame) {
        present(true);
        usleep(5000);
    }
    const long built_at_ask = lines_containing(log_path, "font atlas built");
    check(built_at_ask == 0, "the build is still running when the release is asked");

    const char* released = daemon     ? "the daemon stopped: releasing"
                           : handover ? "moving the overlay to the one that does"
                                      : "switched off: releasing";
    const double asked = now_ms();
    spin_until.store(asked + 5000.0);
    if (daemon) {
        writer.close();
        vocem::StateWriter::unlink_segment();
        printf("     >>> the segment is unlinked, the build starved\n");
    } else if (handover) {
        printf("     >>> context 1 falls silent, context 2 presents, the build starved\n");
    } else {
        write_config(false);
        printf("     >>> the switch goes off, the build starved\n");
    }
    // Context 1 goes on presenting, except in the hand-over, where context 2
    // does and context 1 is the silent holder.
    double longest = 0.0;
    int frames = 0;
    while (now_ms() < spin_until.load()) {
        const double took = present(!handover);
        longest = took > longest ? took : longest;
        ++frames;
        usleep(5000);
    }
    spinner.join();
    const long noticed_in_window = lines_containing(log_path, released);
    printf("     %d frames while the build was starved (5 s); the longest swap took %.1f ms\n",
           frames, longest);

    // The spinner is gone: the build finishes, and the release happens.
    const double release_deadline = now_ms() + 10000.0;
    while (now_ms() < release_deadline && lines_containing(log_path, released) == 0) {
        present(!handover);
        usleep(5000);
    }
    const long said = lines_containing(log_path, released);
    const long built = lines_containing(log_path, "font atlas built");
    printf("     said \"%s\" %ld time(s) (%ld of them during the starved build), \"font atlas "
           "built\" %ld time(s)\n", released, said, noticed_in_window, built);
    check(longest < 500.0,
          "no swap was held for the build: the release waited for it without blocking");
    check(noticed_in_window == 0, "nothing was released while the build ran");
    check(said == 1, "and the release happened, once, when the build was done");
    check(built >= 1, "the build finished once its CPU came back");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, second);
    eglDestroyContext(display, first);
    eglTerminate(display);

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        printf("     (the scratch root %s outlived the test)\n", root);
    }
    return vocem_test::finish();
}
