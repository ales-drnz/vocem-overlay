// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The backend moving between two contexts, and what it leaves behind in each.
//
// Entry 210 made the backend move once its context has not presented for
// kHandOverSeconds, to the context that does: a game that shows its loading
// screen from one context and the game from another, keeping the first alive.
// The move released the old backend WITHOUT a current context -- the old
// context is not current where the move happens -- so its program, buffers and
// font texture stayed in that context for its whole life, and coming back
// built a whole new set beside them. Measured by the 0.1.10 review: the
// textures alive in the first context went 1, 2, 3, 4 over three round trips,
// a 16 to 64 MB atlas each.
//
// Two unshared EGL contexts on pbuffers take turns presenting, each silent for
// longer than the hand-over while the other presents. After every turn, the
// context that just presented is made current (no present) and its live
// texture names are counted: the scene has no faces, so the overlay's share of
// either context is the atlas, one texture, however many times the backend has
// been there before. At kHandOverSeconds = 2 the scene takes about eleven
// seconds; the interval is the library's own and is not overridden here.
//
// VOCEM_GL_SCENARIO=worker (and worker-destroy): a backend left in A is torn
// down while the atlas worker is rasterising for B. Its teardown destroys the
// left ImGui context -- SetCurrentContext to it, DestroyContext -- and the
// worker's every ImGui allocation reads that same global context pointer to
// count itself, so a worker still running can write into a context that has
// just been freed. The release path joins the worker first (entry 192);
// reclaim_left() and forget_left() did not. The scene leaves A behind, switches
// the overlay off and on in B (the atlas goes with the switch, so the next
// build is the worker's), and the moment the log says the worker started, A
// presents (`worker`: reclaim_left) or is destroyed (`worker-destroy`:
// forget_left). The witness is the worker's own "font atlas built" line: it
// must already be in the log when the call that tore the left backend down
// returns -- that is, the teardown waited for the worker.

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
    vocem_test::set_alarm(60, "two contexts taking turns");
    const char* scenario = getenv("VOCEM_GL_SCENARIO") ? getenv("VOCEM_GL_SCENARIO") : "";
    const bool worker_scene = strncmp(scenario, "worker", 6) == 0;
    const bool destroy_scene = strcmp(scenario, "worker-destroy") == 0;

    char root[] = "/tmp/vocem-gl-handover-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule =
        "enabled = true\nshown_apps = " + vocem_test::own_name("vocem_gl_handover") + "\n";
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
        // The display's height from the first frame, so every backend builds
        // against one atlas size and nothing but the move is measured.
        state.display_height = 1080;
        snprintf(state.channel_name, sizeof(state.channel_name), "handover");
        state.user_count = 1;
        state.users[0].id = 7;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Handover");
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
    EGLSurface surfaces[2];
    EGLContext contexts[2];
    for (int i = 0; i < 2; ++i) {
        surfaces[i] = eglCreatePbufferSurface(display, config, surface_attributes);
        contexts[i] = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surfaces[i] == EGL_NO_SURFACE || contexts[i] == EGL_NO_CONTEXT) {
            printf("skip two pbuffer contexts cannot be made here\n");
            return 77;
        }
    }
    const auto present_for = [&](int who, double seconds) {
        eglMakeCurrent(display, surfaces[who], surfaces[who], contexts[who]);
        const double end = now_ms() + seconds * 1000.0;
        while (now_ms() < end) {
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surfaces[who]);
            usleep(16000);
        }
    };
    const auto write_config = [&](bool enabled) {
        char config_path[700];
        snprintf(config_path, sizeof(config_path), "%s/vocem/config.ini", root);
        const std::string text = std::string("enabled = ") + (enabled ? "true" : "false") +
                                 "\nshown_apps = " + vocem_test::own_name("vocem_gl_handover") +
                                 "\n";
        if (FILE* file = fopen(config_path, "w")) {
            fputs(text.c_str(), file);
            fclose(file);
        }
    };
    const auto textures = [&](int who) {
        eglMakeCurrent(display, surfaces[who], surfaces[who], contexts[who]);
        int alive = 0;
        for (GLuint name = 1; name <= 256; ++name) {
            alive += glIsTexture(name) ? 1 : 0;
        }
        return alive;
    };

    if (worker_scene) {
        present_for(0, 1.0);  // A builds the backend, and the first atlas
        present_for(1, 2.5);  // A falls silent: the backend moves to B, A is left
        check(lines_containing(log_path, "moving the overlay") == 1, "the backend moved to B");
        // Off, then on, in B: the switch gives the atlas back, so the next one
        // is the worker's. LiveConfig asks every two seconds.
        write_config(false);
        present_for(1, 2.5);
        check(lines_containing(log_path, "switched off: releasing") == 1,
              "the overlay was switched off in B");
        write_config(true);
        const long started_before = lines_containing(log_path, "off the game's thread");
        eglMakeCurrent(display, surfaces[1], surfaces[1], contexts[1]);
        const double deadline = now_ms() + 5000.0;
        while (lines_containing(log_path, "off the game's thread") == started_before &&
               now_ms() < deadline) {
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surfaces[1]);
            usleep(16000);
        }
        check(lines_containing(log_path, "off the game's thread") == started_before + 1,
              "the overlay came back in B and the worker started rasterising");
        const long built_before = lines_containing(log_path, "font atlas built at");
        const long teardowns_before = lines_containing(log_path, "the backend left in context");
        if (destroy_scene) {
            eglDestroyContext(display, contexts[0]);  // A is not current
        } else {
            eglMakeCurrent(display, surfaces[0], surfaces[0], contexts[0]);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surfaces[0]);
        }
        const bool worker_done_by_then =
            lines_containing(log_path, "font atlas built at") > built_before;
        const bool torn_down =
            lines_containing(log_path, "the backend left in context") == teardowns_before + 1;
        printf("     %s, the worker's build %s\n",
               destroy_scene ? "A destroyed" : "A presented",
               worker_done_by_then ? "had finished"
                                   : "was still running");
        printf("     the left backend was %s\n",
               torn_down ? (worker_done_by_then ? "torn down after the build"
                                                : "torn down DURING the build")
                         : "not torn down yet: it waits for a present after the build");
        check(!torn_down || worker_done_by_then,
              "no left ImGui context was destroyed while the worker was rasterising");
        if (destroy_scene) {
            check(torn_down, "A's destruction tore down the backend left in it");
        }
        present_for(1, 1.0);  // the worker finishes, B draws
        if (!destroy_scene) {
            eglMakeCurrent(display, surfaces[0], surfaces[0], contexts[0]);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(display, surfaces[0]);
            check(lines_containing(log_path, "the backend left in context") ==
                      teardowns_before + 1,
                  "A's present after the build tore down the backend left in it");
        }
        // The live backend in B took the worker's atlas up whole, and has it.
        check(lines_containing(log_path, "font texture uploaded whole") >= 2,
              "B uploaded the worker's atlas");
        const int b_textures = textures(1);
        printf("     B holds %d texture(s)\n", b_textures);
        check(b_textures == 1, "and holds it, the one texture of the atlas");
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        char cleanup[700];
        snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
        system(cleanup);
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }

    present_for(0, 1.0);
    const int first = textures(0);
    printf("     context A, first life: %d texture(s)\n", first);
    check(first == 1, "the overlay holds one texture in the first context: the atlas");
    int worst[2] = {first, 0};
    constexpr int kRounds = 2;
    for (int round = 1; round <= kRounds; ++round) {
        present_for(1, 2.5);
        const int b = textures(1);
        present_for(0, 2.5);
        const int a = textures(0);
        worst[0] = a > worst[0] ? a : worst[0];
        worst[1] = b > worst[1] ? b : worst[1];
        printf("     round %d: context B held %d texture(s) after its turn, context A %d after "
               "its own; %ld hand-over(s) so far\n",
               round, b, a, lines_containing(log_path, "moving the overlay"));
    }
    const long reclaimed = lines_containing(log_path, "deleted the backend left in context");

    // And a context the overlay has left, destroyed rather than presented
    // again: the teardown hook deletes what is in it when it is current, and
    // the backend living in the other context goes on drawing untouched -- the
    // left backend's Shutdown zeroes the shared atlas's texture name, and a
    // live backend that lost it would upload the atlas again.
    present_for(1, 2.5);  // A is left behind once more
    const long uploads_before = lines_containing(log_path, "font texture uploaded whole");
    eglMakeCurrent(display, surfaces[0], surfaces[0], contexts[0]);
    eglDestroyContext(display, contexts[0]);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    present_for(1, 0.5);
    const int b_after = textures(1);
    const long deleted_at_destroy =
        lines_containing(log_path, "deleted the backend left in context") - reclaimed - 1;
    printf("     after the left context died: %ld teardown(s) at its destruction, context B "
           "holds %d texture(s), %ld atlas upload(s) since\n",
           deleted_at_destroy, b_after,
           lines_containing(log_path, "font texture uploaded whole") - uploads_before);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    check(reclaimed == 2 * kRounds - 1,
          "every return to a context deleted what the backend had left there");
    check(deleted_at_destroy == 1,
          "a left context destroyed while current has the backend's objects deleted then");
    check(b_after == 1 && lines_containing(log_path, "font texture uploaded whole") ==
                              uploads_before,
          "and the backend in the other context goes on without a rebuild");
    check(lines_containing(log_path, "moving the overlay") == 2 * kRounds + 1,
          "the backend moved at every turn, so every turn measured a hand-over");
    check(worst[0] == 1,
          "coming back to the first context leaves one texture there, not one per visit");
    check(worst[1] == 1, "and the second context holds one texture after each of its turns");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
