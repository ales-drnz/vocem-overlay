// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Colour emoji, in a real GL process's own framebuffer.
//
// `tests/emoji_atlas.cpp` proves the bank's pixels reach the atlas; that is not
// the same claim as "a name with sushi in it comes out in colour in a game", and
// the difference is the whole library between the two. This probe is the second
// claim, and it is the shape of check that verifies a *packaged* library: point
// `VOCEM_GL_LIBRARY` at the shipped file and this measures what ships.
//
// The measurement is differential, because a colour detector cannot work here --
// the speaking ring is green and the muted badge is red, which is why DESIGN
// forbids colour detectors on captures. So the same scene is drawn twice in two
// processes, identical in everything except whether a bank is reachable, and the
// pixels that DIFFER between the two frames are exactly the emoji glyphs: colour
// in one, the monochrome font's white in the other. Rings, badges, placeholders
// and text are byte-identical in both and cancel out.
//
// Nothing is animated in the published state on purpose (nobody speaking, no
// notification): an animation would make the two frames differ for a reason that
// is not the bank.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>
#include <vector>

#include "private_shm.h"
#include "vocem/shm.h"

namespace {

const int W = 360;
const int H = 360;

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

using PFN_glXChooseVisual = XVisualInfo* (*)(Display*, int, int*);
using PFN_glXCreateContext = void* (*)(Display*, XVisualInfo*, void*, int);
using PFN_glXMakeCurrent = int (*)(Display*, XID, void*);
using PFN_glXDestroyContext = void (*)(Display*, void*);
using PFN_glXSwapBuffers = void (*)(Display*, XID);
using PFN_glClearColor = void (*)(float, float, float, float);
using PFN_glClear = void (*)(unsigned int);
using PFN_glReadBuffer = void (*)(unsigned int);
using PFN_glReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int, void*);

// One process's worth of drawing: the game's half done the way GLFW and LWJGL do
// it (dlopen RTLD_LOCAL, every symbol off the handle through the interposed
// dlsym), then the front buffer written out raw for the parent to compare.
// Returns 0 on success, 77 when the machine cannot do it at all.
int draw_and_dump(const char* bank, const char* out_path) {
    // A null bank means "resolve it the way an installed library does": no
    // environment override, the compiled-in default path. Every leg used to
    // set the variable, which meant the default-path fallback -- the one a
    // real install lives on -- had no witness anywhere (entry 38's wrong
    // witness, in miniature).
    if (bank) {
        setenv("VOCEM_EMOJI_BANK", bank, 1);
    } else {
        unsetenv("VOCEM_EMOJI_BANK");
    }

    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gl) {
        return 77;
    }
    auto* choose = reinterpret_cast<PFN_glXChooseVisual>(dlsym(gl, "glXChooseVisual"));
    auto* create = reinterpret_cast<PFN_glXCreateContext>(dlsym(gl, "glXCreateContext"));
    auto* make_current = reinterpret_cast<PFN_glXMakeCurrent>(dlsym(gl, "glXMakeCurrent"));
    auto* destroy = reinterpret_cast<PFN_glXDestroyContext>(dlsym(gl, "glXDestroyContext"));
    auto* swap = reinterpret_cast<PFN_glXSwapBuffers>(dlsym(gl, "glXSwapBuffers"));
    auto* clear_colour = reinterpret_cast<PFN_glClearColor>(dlsym(gl, "glClearColor"));
    auto* clear = reinterpret_cast<PFN_glClear>(dlsym(gl, "glClear"));
    auto* read_buffer = reinterpret_cast<PFN_glReadBuffer>(dlsym(gl, "glReadBuffer"));
    auto* read_pixels = reinterpret_cast<PFN_glReadPixels>(dlsym(gl, "glReadPixels"));
    if (!choose || !create || !make_current || !destroy || !swap || !clear_colour || !clear ||
        !read_buffer || !read_pixels) {
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        return 77;
    }
    int attrs[] = {4 /*GLX_RGBA*/, 5 /*GLX_DOUBLEBUFFER*/, 0};
    XVisualInfo* visual = choose(display, DefaultScreen(display), attrs);
    if (!visual) {
        return 1;
    }
    XSetWindowAttributes swa;
    swa.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    // Override-redirect and parked far off screen: nothing appears on the desktop
    // and no focus is taken -- the owner may be in a game while this runs.
    swa.override_redirect = True;
    Window window = XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, W, H, 0,
                                  visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWOverrideRedirect, &swa);
    XMapWindow(display, window);
    void* context = create(display, visual, nullptr, 1);
    if (!context) {
        return 1;
    }
    make_current(display, window, context);

    // The atlas is rebuilt when a new bank emoji is seen, and the rebuild lands a
    // frame later; 60 frames leaves that behind by a wide margin.
    for (int frame = 0; frame < 60; ++frame) {
        clear_colour(0.10f, 0.15f, 0.20f, 1.0f);
        clear(0x00004000 /*GL_COLOR_BUFFER_BIT*/);
        swap(display, window);
        usleep(16000);
    }

    static unsigned char pixels[W * H * 4];
    read_buffer(0x0404 /*GL_FRONT*/);
    read_pixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels);

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        return 1;
    }
    fwrite(pixels, 1, sizeof(pixels), out);
    fclose(out);

    destroy(display, context);
    XCloseDisplay(display);
    return 0;
}

int spread(const unsigned char* p) {
    const int rg = p[0] > p[1] ? p[0] - p[1] : p[1] - p[0];
    const int gb = p[1] > p[2] ? p[1] - p[2] : p[2] - p[1];
    const int rb = p[0] > p[2] ? p[0] - p[2] : p[2] - p[0];
    int best = rg > gb ? rg : gb;
    return best > rb ? best : rb;
}

bool read_raw(const char* path, unsigned char* into, size_t bytes) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    const size_t got = fread(into, 1, bytes, file);
    fclose(file);
    return got == bytes;
}

}  // namespace

int main(int argc, char** argv) {
    // The child of the container leg below: draw one frame with no override, so
    // the candidates in vocem/emoji_bank.h are what has to find the bank.
    if (argc > 2 && strcmp(argv[1], "--draw-through-run-host") == 0) {
        return draw_and_dump(nullptr, argv[2]);
    }
    if (!getenv("DISPLAY")) {
        printf("skip no DISPLAY, so no GLX drawable to draw into\n");
        return 77;
    }
    if (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    const char* bank = getenv("VOCEM_EMOJI_BANK");
    if (!bank || !bank[0]) {
        printf("skip meant to run with VOCEM_EMOJI_BANK pointing at a bank\n");
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

    alarm(120);

    char root[] = "/tmp/vocem-gl-emoji-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    // The channel name is shown and carries an emoji of its own: the header is
    // drawn in the heavier weight, and a merged glyph belongs to the font it
    // was merged into (entry 26) -- without this, the strong weight had no
    // frame-level witness and only the atlas test covered it.
    write_file(path,
               "enabled = true\nshown_apps = vocem_gl_emoji_colour\n"
               "show_channel_name = true\n");
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);

    // Two names carrying emoji the bank has and Inter does not -- sushi and
    // chopsticks. Nobody speaking and no notification: see the header.
    // display_height is published so the atlas is built at a real display's
    // size rather than at this small window's, which is entry 39's rule and also
    // gives the glyphs enough pixels to measure.
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 1080;
        // The star, U+2B50: below U+FFFF (entry 25's class) and in the bank.
        snprintf(state.channel_name, sizeof(state.channel_name), "room \xE2\xAD\x90");
        state.user_count = 2;
        state.users[0].id = 700;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "One \xF0\x9F\x8D\xA3");
        state.users[1].id = 701;
        snprintf(state.users[1].name, sizeof(state.users[1].name), "Two \xF0\x9F\xA5\xA2");
    });
    if (failures) {
        return 1;
    }

    // The two frames, one process each: a context's life per frame set, and the
    // bank decided before the library is ever loaded.
    char with_path[700];
    char without_path[700];
    snprintf(with_path, sizeof(with_path), "%s/with.raw", root);
    snprintf(without_path, sizeof(without_path), "%s/without.raw", root);
    char missing[700];
    snprintf(missing, sizeof(missing), "%s/no-such-bank.rgba", root);

    int codes[2] = {0, 0};
    const char* banks[2] = {bank, missing};
    const char* outs[2] = {with_path, without_path};
    for (int i = 0; i < 2; ++i) {
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            _exit(draw_and_dump(banks[i], outs[i]));
        }
        int status = 0;
        waitpid(pid, &status, 0);
        codes[i] = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    if (codes[0] == 77 || codes[1] == 77) {
        printf("skip the drawing half could not run here (no libGL or no display inside)\n");
        return 77;
    }
    check(codes[0] == 0 && codes[1] == 0, "both frames were drawn and read back");
    if (failures) {
        return 1;
    }

    static unsigned char with_bank[W * H * 4];
    static unsigned char without_bank[W * H * 4];
    check(read_raw(with_path, with_bank, sizeof(with_bank)) &&
              read_raw(without_path, without_bank, sizeof(without_bank)),
          "both frames are the size they should be");
    if (failures) {
        return 1;
    }

    long differing = 0;
    long colour_with = 0;
    long colour_without = 0;
    for (long i = 0; i < (long)W * H; ++i) {
        const unsigned char* a = with_bank + i * 4;
        const unsigned char* b = without_bank + i * 4;
        const int dr = a[0] > b[0] ? a[0] - b[0] : b[0] - a[0];
        const int dg = a[1] > b[1] ? a[1] - b[1] : b[1] - a[1];
        const int db = a[2] > b[2] ? a[2] - b[2] : b[2] - a[2];
        if (dr < 24 && dg < 24 && db < 24) {
            continue;
        }
        ++differing;
        if (spread(a) > 40) {
            ++colour_with;
        }
        if (spread(b) > 40) {
            ++colour_without;
        }
    }

    printf("     pixels differing between the two frames: %ld\n", differing);
    printf("     of those, coloured with the bank: %ld, without it: %ld\n", colour_with,
           colour_without);

    // The two frames must differ at all: a library that ignores the bank draws
    // the same monochrome glyph either way, which is the defect this exists for.
    check(differing > 100, "the bank changes what reaches the framebuffer");
    // And the difference must be colour, in the frame that had the bank. The
    // monochrome font draws these glyphs in the text's own ink, so its spread is
    // near zero; the sushi is not.
    check(colour_with > 40, "the glyphs the bank supplied are coloured in the game's own frame");
    check(colour_with > colour_without * 4,
          "and the run without a bank drew them monochrome, as it always did");

    // The default path, where an installed library actually finds its bank.
    // Only when this machine has one installed: a build tree on a machine
    // without the package still runs the two legs above.
    struct stat installed {};
    if (stat(VOCEM_EMOJI_BANK_PATH, &installed) == 0) {
        char default_path[700];
        snprintf(default_path, sizeof(default_path), "%s/default.raw", root);
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            _exit(draw_and_dump(nullptr, default_path));
        }
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "the default-path frame was drawn and read back");
        static unsigned char with_default[W * H * 4];
        if (read_raw(default_path, with_default, sizeof(with_default))) {
            long colour_default = 0;
            for (long i = 0; i < (long)W * H; ++i) {
                const unsigned char* a = with_default + i * 4;
                const unsigned char* b = without_bank + i * 4;
                const int dr = a[0] > b[0] ? a[0] - b[0] : b[0] - a[0];
                const int dg = a[1] > b[1] ? a[1] - b[1] : b[1] - a[1];
                const int db = a[2] > b[2] ? a[2] - b[2] : b[2] - a[2];
                if ((dr >= 24 || dg >= 24 || db >= 24) && spread(a) > 40) {
                    ++colour_default;
                }
            }
            printf("     coloured through the installed default path: %ld\n", colour_default);
            check(colour_default > 40,
                  "the compiled-in default path finds the installed bank, no variable set");
        } else {
            check(false, "the default-path frame is the size it should be");
        }
    } else {
        printf("     (no bank installed at %s; the default-path leg has nothing to test)\n",
               VOCEM_EMOJI_BANK_PATH);
    }

    // And the leg that was missing, which is the one a Steam title actually
    // runs in.
    //
    // pressure-vessel gives the game a container with its own /usr and mounts
    // the host at /run/host. Measured with no game launched:
    // `/usr/share/vocem/emoji_bank.rgba` is ABSENT in there and
    // `/run/host/usr/share/vocem/emoji_bank.rgba` is PRESENT. The shim has had
    // that fallback for the heavy library since entry 31; the bank had one
    // hardcoded path and no fallback, so every Steam game drew the overlay with
    // monochrome emoji while the same game outside the container drew them in
    // colour -- "sometimes coloured and sometimes not", from a chair.
    //
    // Built here rather than described: bwrap with a tmpfs over the compiled-in
    // path's directory, so that path is as absent as it is in the container, and
    // the real bank bound at /run/host + the same path. Nothing of the shape is
    // assumed; the process has to find it the way it would in there.
    {
        char container_path[700];
        snprintf(container_path, sizeof(container_path), "%s/container.raw", root);
        std::string compiled = VOCEM_EMOJI_BANK_PATH;
        const std::string compiled_dir = compiled.substr(0, compiled.find_last_of('/'));
        const std::string host_copy = std::string("/run/host") + VOCEM_EMOJI_BANK_PATH;
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            fflush(stdout);
            const pid_t pid = fork();
            if (pid == 0) {
                // The compiled-in path is hidden only where it exists: bwrap
                // creates its own mountpoints and cannot mkdir into a read-only
                // /usr/local, so asking for a tmpfs over a directory that is not
                // there fails the whole sandbox. In a build tree that path is
                // already absent, which is the same state the container is in.
                struct stat there {};
                const bool hide = ::stat(compiled_dir.c_str(), &there) == 0;
                std::vector<const char*> args = {"bwrap", "--dev-bind", "/", "/"};
                if (hide) {
                    args.push_back("--tmpfs");
                    args.push_back(compiled_dir.c_str());
                }
                // /run/host does not exist on a host, and bwrap builds its own
                // mountpoints -- which it cannot do inside a root-owned /run.
                // A tmpfs over /run gives it somewhere to build, and the
                // session's own /run/user goes back on top of it because the X
                // connection is reached through it (without that the child
                // opens no display and the leg measures nothing).
                args.push_back("--tmpfs");
                args.push_back("/run");
                args.push_back("--dev-bind");
                args.push_back("/run/user");
                args.push_back("/run/user");
                args.push_back("--ro-bind");
                args.push_back(bank);
                args.push_back(host_copy.c_str());
                args.push_back("--die-with-parent");
                args.push_back(self);
                args.push_back("--draw-through-run-host");
                args.push_back(container_path);
                args.push_back(nullptr);
                execvp("bwrap", const_cast<char* const*>(args.data()));
                _exit(76);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            if (code == 76 || code == 77) {
                printf("     (the container shape could not be built here; leg skipped)\n");
            } else {
                check(code == 0, "the container-shaped frame was drawn and read back");
                static unsigned char in_container[W * H * 4];
                if (read_raw(container_path, in_container, sizeof(in_container))) {
                    long colour_container = 0;
                    for (long i = 0; i < (long)W * H; ++i) {
                        const unsigned char* a = in_container + i * 4;
                        const unsigned char* b = without_bank + i * 4;
                        const int dr = a[0] > b[0] ? a[0] - b[0] : b[0] - a[0];
                        const int dg = a[1] > b[1] ? a[1] - b[1] : b[1] - a[1];
                        const int db = a[2] > b[2] ? a[2] - b[2] : b[2] - a[2];
                        if ((dr >= 24 || dg >= 24 || db >= 24) && spread(a) > 40) {
                            ++colour_container;
                        }
                    }
                    printf("     coloured with the bank reachable only under /run/host: %ld\n",
                           colour_container);
                    check(colour_container > 40,
                          "a game inside a container finds the bank the host mounted for it");
                } else {
                    check(false, "the container-shaped frame is the size it should be");
                }
            }
        }
    }

    char cleanup[800];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
