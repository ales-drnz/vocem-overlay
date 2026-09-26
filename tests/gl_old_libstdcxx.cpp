// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Whether the overlay library loads in a game whose runtime ships an older C++
// runtime, and whether the shim says so when a load fails.
//
// Two scenes (VOCEM_GL_SCENARIO), both presenting a few GLX frames with the
// shim's stderr captured into a file:
//
//   * `old-runtime`, run with LD_LIBRARY_PATH pointing at a synthetic
//     libstdc++.so.6 that has libstdc++ 6.0.21's version nodes and none newer
//     (stub_old_libstdcxx.cpp) -- the Steam scout runtime's shape, which the
//     0.1.10 review put ahead of the system's. The heavy library NEEDED
//     libstdc++ at GLIBCXX_3.4.29, ld.so refused it, and the shim said nothing
//     at all: no line, no application record. It carries its runtime inside
//     now, so the library loads and says its first line about this process
//     ("drawing in"/"not drawing in") -- the verdict this scene requires.
//   * `unloadable`, run with VOCEM_GL_LIBRARY naming a file that does not
//     exist. Under VOCEM_DEBUG the shim says, once, which path it could not
//     load and ld.so's own reason, and then that the process has no overlay.
//     And nothing else loads in its place: the 0.1.10 shim went on to the
//     installed library after a failed override, so a development path that
//     failed was answered by the package's overlay without a word.
//
// This executable carries its own C++ runtime (-static-libstdc++): under the
// stub on LD_LIBRARY_PATH a dynamically linked C++ probe would not start at all.
// GLX and not EGL, because of the driver: at 32 bits this machine's EGL display
// comes from Mesa, whose libgallium needs GLIBCXX_3.4.26 and cannot load under
// the stub any more than the overlay could (measured) -- NVIDIA's GLX needs no
// C++ runtime at either width.
// No state is published; the private /dev/shm keeps the overlay's reader away
// from the live daemon's segment all the same.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <GL/glx.h>
#include <X11/Xlib.h>

#include <string>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"

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
    char line[2048];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            ++count;
        }
    }
    fclose(file);
    return count;
}

void print_file(const char* path) {
    if (FILE* file = fopen(path, "r")) {
        char line[2048];
        while (fgets(line, sizeof(line), file)) {
            printf("     | %s", line);
        }
        fclose(file);
    }
}

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED") || !getenv("VOCEM_GL_LIBRARY")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "";
    const bool unloadable = strcmp(scenario, "unloadable") == 0;
    if (!unloadable && strcmp(scenario, "old-runtime") != 0) {
        printf("FAIL VOCEM_GL_SCENARIO is neither old-runtime nor unloadable\n");
        return 1;
    }
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "a few GLX frames");

    char root[] = "/tmp/vocem-gl-runtime-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule =
        "enabled = true\nshown_apps = " + vocem_test::own_name("vocem_gl_old_libstdcxx") + "\n";
    if (FILE* file = fopen(path, "w")) {
        fputs(rule.c_str(), file);
        fclose(file);
    }
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);
    setenv("VOCEM_DEBUG", "1", 1);

    // Everything written to stderr from here on -- the shim's lines and the
    // overlay's -- lands in a file this process reads back afterwards.
    static char err_path[700];
    snprintf(err_path, sizeof(err_path), "%s/stderr.log", root);
    const int saved_stderr = dup(2);
    const int err_fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (err_fd < 0 || saved_stderr < 0 || dup2(err_fd, 2) < 0) {
        printf("FAIL stderr could not be captured\n");
        return 1;
    }
    close(err_fd);

    const auto give_up = [&](const char* why) {
        dup2(saved_stderr, 2);
        print_file(err_path);
        printf("skip %s\n", why);
        return 77;
    };
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        return give_up("no X display here");
    }
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, None};
    XVisualInfo* visual = glXChooseVisual(display, DefaultScreen(display), attributes);
    if (!visual) {
        return give_up("no double-buffered GLX visual here (a driver that needs the C++ "
                       "runtime itself cannot load under the stub)");
    }
    XSetWindowAttributes swa;
    swa.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    // Override-redirect and parked off screen: nothing appears, no focus.
    swa.override_redirect = True;
    const Window window =
        XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, 64, 64, 0,
                      visual->depth, InputOutput, visual->visual, CWColormap | CWOverrideRedirect,
                      &swa);
    XMapWindow(display, window);
    GLXContext context = glXCreateContext(display, visual, nullptr, True);
    if (!context || !glXMakeCurrent(display, window, context)) {
        return give_up("no GLX context here");
    }
    for (int frame = 0; frame < 3; ++frame) {
        glXSwapBuffers(display, window);
        usleep(16000);
    }
    glXMakeCurrent(display, None, nullptr);
    glXDestroyContext(display, context);
    XCloseDisplay(display);
    dup2(saved_stderr, 2);

    printf("     what the process said on stderr:\n");
    print_file(err_path);
    const long shim_lines = lines_containing(err_path, "[vocem/gl-shim]");
    const long overlay_lines = lines_containing(err_path, "[vocem/gl]");
    if (unloadable) {
        const char* wanted = getenv("VOCEM_GL_LIBRARY");
        check(lines_containing(err_path, "[vocem/gl-shim] could not load ") == 1 &&
                  lines_containing(err_path, wanted) == 1,
              "the shim says once which overlay library it could not load, and why");
        check(lines_containing(err_path, "[vocem/gl-shim] no overlay in this process") == 1,
              "and that this process has no overlay");
        check(shim_lines == 2, "and says nothing else");
        check(overlay_lines == 0,
              "and no other overlay library was loaded in place of the one asked for");
    } else {
        check(shim_lines == 0, "the shim had no failure to report");
        check(lines_containing(err_path, "drawing in '") >= 1,
              "the overlay library loaded under the old C++ runtime and gave its verdict on "
              "this process");
    }

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
