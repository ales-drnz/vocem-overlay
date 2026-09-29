// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The shim and another OpenGL interposer in one process: does each get every
// frame, once, and does the frame still reach the system's library.
//
// The chain is "application -> shim -> another interposer -> system GL", the
// session's order when a game is started through the `mangohud` wrapper or
// Prism Launcher's "Enable MangoHud": the session's preload comes first and the
// wrapper appends. The other interposer is tests/stub_chain_overlay.cpp in
// five shapes (see its header), and a library that looks the present up for
// the game and hooks nothing (stub_chain_lookup.cpp); what the shim did with a
// frame is counted by
// tests/stub_gl_counter.cpp standing in for the overlay library. EGL only, and
// on EGL_NO_DISPLAY, which the system library refuses cleanly: no display, no
// context, no state segment, and the GLX half shares every line that decides
// where a frame goes (real_for, dispatch, the dlsym hook).
//
// Each scenario is a fresh process -- the preload is decided at exec, and the
// shim's slots are filled once per process -- presenting three frames through
// one door:
//
//   door 1   the application linked libEGL and calls eglSwapBuffers (the
//            second executable, vocem_shim_chain_linked)
//   door 2   dlopen(RTLD_LOCAL) + dlsym("eglGetProcAddress") on that handle,
//            then the dispatcher asked for the present
//   door 3   dlopen(RTLD_LOCAL) + dlsym("eglSwapBuffers") on that handle: the
//            door SDL, GLFW and LWJGL use, the one entry 38 says carries the
//            weight -- and the one on which the shim used to cut the chain
//   damage   door 2 for eglSwapBuffersWithDamageEXT
//
// Asserted: the shim hands each frame to the overlay library exactly once; the
// other interposer is given each frame exactly as often as it would be alone
// (its own control run, no shim); nothing loops; and where the other
// interposer forwarded is the system's library.
//
// Needs libEGL.so.1 and the two stub libraries; says why and exits 77 when it
// cannot measure.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#ifdef CHAIN_LINKED
#include <EGL/egl.h>
#endif

namespace {

constexpr int kFrames = 3;

using PFN_swap = unsigned (*)(void*, void*);
using PFN_damage = unsigned (*)(void*, void*, int*, int);
using PFN_proc = void* (*)(const char*);

// ---- the child: present, then say what happened -------------------------

int presents_so_far() {
    const char* library = getenv("VOCEM_GL_LIBRARY");
    void* handle = library ? dlopen(library, RTLD_NOW | RTLD_NOLOAD) : nullptr;
    if (!handle) {
        return 0;  // never loaded: the shim never had a frame to hand over
    }
    auto count = reinterpret_cast<int (*)()>(dlsym(handle, "vocem_stub_presents"));
    return count ? count() : -1;
}

// A counter of one particular library, when two stubs export the same names:
// VOCEM_CHAIN_WATCH names it, and its own handle answers for it.
int watched_calls() {
    const char* path = getenv("VOCEM_CHAIN_WATCH");
    void* handle = path && path[0] ? dlopen(path, RTLD_NOW | RTLD_NOLOAD) : nullptr;
    auto calls = handle ? reinterpret_cast<int (*)()>(dlsym(handle, "vocem_chain_calls")) : nullptr;
    return calls ? calls() : -1;
}

int report() {
    auto calls = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "vocem_chain_calls"));
    auto target = reinterpret_cast<const char* (*)()>(dlsym(RTLD_DEFAULT, "vocem_chain_target"));
    auto nulls = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "vocem_chain_nulls"));
    printf("presents=%d chain=%d target=%s nulls=%d watched=%d\n", presents_so_far(),
           calls ? calls() : -1, target && target()[0] ? target() : "-", nulls ? nulls() : -1,
           watched_calls());
    return 0;
}

#ifdef CHAIN_LINKED
int child_linked() {
    for (int frame = 0; frame < kFrames; ++frame) {
        eglSwapBuffers(EGL_NO_DISPLAY, EGL_NO_SURFACE);
    }
    return report();
}
#else
int child(const char* door) {
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl) {
        printf("no-egl\n");
        return 0;
    }
    if (strncmp(door, "lookup", 6) == 0) {
        // Through a library that looks the present up for us (SDL's shape).
        auto present = reinterpret_cast<void (*)(int, int)>(
            dlsym(RTLD_DEFAULT, "vocem_lookup_present"));
        if (present) {
            present(kFrames, door[6] == '2' ? 2 : 3);
        }
        return report();
    }
    if (strcmp(door, "3") == 0) {
        PFN_swap swap = reinterpret_cast<PFN_swap>(dlsym(egl, "eglSwapBuffers"));
        for (int frame = 0; swap && frame < kFrames; ++frame) {
            swap(nullptr, nullptr);
        }
        return report();
    }
    PFN_proc get_proc = reinterpret_cast<PFN_proc>(dlsym(egl, "eglGetProcAddress"));
    if (!get_proc) {
        printf("no-dispatcher\n");
        return 0;
    }
    if (strcmp(door, "2") == 0) {
        PFN_swap swap = reinterpret_cast<PFN_swap>(get_proc("eglSwapBuffers"));
        for (int frame = 0; swap && frame < kFrames; ++frame) {
            swap(nullptr, nullptr);
        }
    } else {
        PFN_damage swap = reinterpret_cast<PFN_damage>(get_proc("eglSwapBuffersWithDamageEXT"));
        for (int frame = 0; swap && frame < kFrames; ++frame) {
            swap(nullptr, nullptr, nullptr, 0);
        }
    }
    return report();
}
#endif

}  // namespace

#ifdef CHAIN_LINKED
int main() {
    return child_linked();
}
#else

namespace {

// ---- the parent: one process per scenario ---------------------------------

struct Outcome {
    bool ran = false;
    int exit_code = -1;
    int signal = 0;
    int presents = -1;
    int chain = -1;
    int nulls = -1;
    int watched = -1;
    std::string target;
    std::string text;
};

Outcome run(const char* door, const std::string& preload, const char* extra_env = nullptr,
            const char* argv0 = "shim_chain") {
    Outcome outcome;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return outcome;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_fds[1], 1);
        dup2(pipe_fds[1], 2);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        unsetenv("VOCEM_DISABLE");
        unsetenv("VOCEM_NO_DLSYM");
        if (extra_env) {
            putenv(const_cast<char*>(extra_env));
        }
        if (preload.empty()) {
            unsetenv("LD_PRELOAD");
        } else {
            setenv("LD_PRELOAD", preload.c_str(), 1);
        }
        if (strcmp(door, "1") == 0) {
            const char* linked = getenv("VOCEM_CHAIN_LINKED");
            execl(linked, linked, static_cast<char*>(nullptr));
        } else {
            execl("/proc/self/exe", argv0, "child", door, static_cast<char*>(nullptr));
        }
        _exit(126);
    }
    close(pipe_fds[1]);
    char buffer[4096];
    ssize_t got = 0;
    while ((got = read(pipe_fds[0], buffer, sizeof buffer)) > 0) {
        outcome.text.append(buffer, static_cast<size_t>(got));
    }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    outcome.ran = true;
    if (WIFEXITED(status)) {
        outcome.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outcome.signal = WTERMSIG(status);
    }
    const size_t at = outcome.text.find("presents=");
    if (at != std::string::npos) {
        char target[256] = "";
        sscanf(outcome.text.c_str() + at, "presents=%d chain=%d target=%255s nulls=%d watched=%d",
               &outcome.presents, &outcome.chain, target, &outcome.nulls, &outcome.watched);
        outcome.target = target;
    }
    return outcome;
}

int g_failures = 0;

void expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

const char* describe(const Outcome& o, char* line, size_t size) {
    if (o.signal) {
        snprintf(line, size, "killed by signal %d", o.signal);
    } else if (o.exit_code == 99) {
        snprintf(line, size, "a loop (the interposer re-entered itself)");
    } else if (o.presents < 0 && o.chain < 0) {
        snprintf(line, size, "no report, exit %d: %s", o.exit_code, o.text.c_str());
    } else {
        snprintf(line, size, "shim handed over %d, the other interposer was given %d, "
                 "it forwarded to %s", o.presents, o.chain, o.target.c_str());
    }
    return line;
}

bool clean(const Outcome& o) {
    return o.ran && o.signal == 0 && o.exit_code == 0 && o.presents >= 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "child") == 0) {
        return child(argv[2]);
    }

    const char* shim = getenv("VOCEM_CHAIN_SHIM");
    const char* own = getenv("VOCEM_CHAIN_OWN_HANDLE");
    const char* symbolic = getenv("VOCEM_CHAIN_SYMBOLIC");
    const char* asks = getenv("VOCEM_CHAIN_ASKS");
    const char* lookup = getenv("VOCEM_CHAIN_LOOKUP");
    const char* next = getenv("VOCEM_CHAIN_NEXT");
    const char* dispatcher = getenv("VOCEM_CHAIN_DISPATCHER");
    if (!shim || !own || !symbolic || !asks || !lookup || !next || !dispatcher || !getenv("VOCEM_GL_LIBRARY") ||
        !getenv("VOCEM_CHAIN_LINKED")) {
        printf("skip the shim, the stub interposers or the counting library were not named\n");
        return 77;
    }
    if (!dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL)) {
        printf("skip libEGL.so.1 is not installed, so there is no system library to chain to\n");
        return 77;
    }

    const std::string s = shim;
    char line[512];

    // --- the shim alone: the baseline every other row is compared to --------
    printf("-- the shim alone\n");
    for (const char* door : {"1", "2", "3"}) {
        const Outcome o = run(door, s);
        printf("     door %s: %s\n", door, describe(o, line, sizeof line));
        char what[160];
        snprintf(what, sizeof what, "door %s, shim alone: every frame handed over once", door);
        expect(clean(o) && o.presents == kFrames && o.chain == -1, what);
    }

    // --- each interposer alone, the control: what it gets without us --------
    struct Shape {
        const char* name;
        const char* path;
    };
    const Shape shapes[] = {{"own handle (MangoHud's shape)", own},
                            {"own handle, -Bsymbolic", symbolic},
                            {"dlsym(RTLD_NEXT)", next},
                            {"via the global eglGetProcAddress", dispatcher}};
    for (const Shape& shape : shapes) {
        printf("-- another interposer, %s\n", shape.name);
        for (const char* door : {"1", "2", "3"}) {
            const Outcome alone = run(door, shape.path);
            const Outcome chained = run(door, s + ":" + shape.path);
            printf("     door %s alone:      %s\n", door, describe(alone, line, sizeof line));
            printf("     door %s after us:   %s\n", door, describe(chained, line, sizeof line));
            char what[200];
            snprintf(what, sizeof what,
                     "door %s, %s after the shim: the shim hands every frame over once", door,
                     shape.name);
            expect(clean(chained) && chained.presents == kFrames, what);
            snprintf(what, sizeof what,
                     "door %s, %s after the shim: it is given every frame it is given alone (%d)",
                     door, shape.name, clean(alone) ? alone.chain : -1);
            expect(clean(alone) && clean(chained) && chained.chain == alone.chain, what);
            if (clean(alone) && alone.chain > 0) {
                snprintf(what, sizeof what,
                         "door %s, %s after the shim: and forwards where it does alone (%s)", door,
                         shape.name, alone.target.c_str());
                expect(chained.target == alone.target, what);
            }
        }
    }

    // --- the damage-aware present: only reachable through a dispatcher -------
    printf("-- eglSwapBuffersWithDamageEXT through the dispatcher\n");
    for (const char* path : {own, symbolic}) {
        const Outcome alone = run("damage", path);
        const Outcome chained = run("damage", s + ":" + path);
        const char* shape = path == own ? "MangoHud's shape" : "MangoHud's shape, -Bsymbolic";
        printf("     %s alone:      %s\n", shape, describe(alone, line, sizeof line));
        printf("     %s after us:   %s\n", shape, describe(chained, line, sizeof line));
        char what[200];
        snprintf(what, sizeof what, "damage, %s after the shim: every frame handed over once",
                 shape);
        expect(clean(chained) && chained.presents == kFrames, what);
        snprintf(what, sizeof what, "damage, %s after the shim: given what it is given alone",
                 shape);
        expect(clean(alone) && clean(chained) && chained.chain == alone.chain &&
                   chained.target == alone.target,
               what);
    }

    // --- the switches: off means out of the way, chain included -------------
    // VOCEM_NO_DLSYM=1 leaves door 3 to whatever the next dlsym answers. The
    // -Bsymbolic shape answers with its own hook and the shim is not in that
    // frame; MangoHud's own binding answers with OURS (see the stub), so there
    // the shim still draws -- the other interposer chose that, not us.
    printf("-- VOCEM_DISABLE=1 and VOCEM_NO_DLSYM=1, MangoHud's shape after the shim, door 3\n");
    for (const char* path : {own, symbolic}) {
        const char* shape = path == own ? "MangoHud's shape" : "MangoHud's shape, -Bsymbolic";
        const Outcome alone = run("3", path);
        const Outcome disabled = run("3", s + ":" + path, "VOCEM_DISABLE=1");
        const Outcome no_dlsym = run("3", s + ":" + path, "VOCEM_NO_DLSYM=1");
        printf("     %s, VOCEM_DISABLE=1:  %s\n", shape, describe(disabled, line, sizeof line));
        printf("     %s, VOCEM_NO_DLSYM=1: %s\n", shape, describe(no_dlsym, line, sizeof line));
        char what[200];
        snprintf(what, sizeof what,
                 "VOCEM_DISABLE=1, %s: the shim draws nothing, the other is as if alone", shape);
        expect(clean(disabled) && disabled.presents == 0 && disabled.chain == alone.chain &&
                   disabled.target == alone.target,
               what);
        snprintf(what, sizeof what, "VOCEM_NO_DLSYM=1, %s: the other interposer is as if alone",
                 shape);
        expect(clean(no_dlsym) && no_dlsym.chain == alone.chain &&
                   no_dlsym.target == alone.target &&
                   (path == own || no_dlsym.presents == 0),
               what);
    }

    // --- what the refutation of the chain change found ----------------------
    // Each of these passed against the shim before the chain was followed and
    // failed against the first version that followed it.

    // A preloaded library that is not an interposer -- SDL put in LD_PRELOAD
    // as a fix -- looking the present up for the game, after us or ahead of
    // us: the game's lookup, answered with our hook.
    printf("-- a preloaded library that hooks nothing looks the present up for the game\n");
    for (const char* door : {"lookup3", "lookup2"}) {
        for (bool after : {true, false}) {
            const std::string preload =
                after ? s + ":" + lookup : std::string(lookup) + ":" + s;
            const Outcome o = run(door, preload);
            printf("     %s, preloaded %s us: %s\n", door, after ? "after" : "ahead of",
                   describe(o, line, sizeof line));
            char what[200];
            snprintf(what, sizeof what,
                     "%s, SDL's shape preloaded %s the shim: every frame handed over once", door,
                     after ? "after" : "ahead of");
            expect(clean(o) && o.presents == kFrames, what);
        }
    }

    // An interposer ahead of us and MangoHud's shape behind: the table's hooks
    // must be ours, not the first definition in the global scope.
    printf("-- dlsym(RTLD_NEXT) ahead of the shim, MangoHud's shape behind it\n");
    for (const char* door : {"1", "3"}) {
        const std::string both = std::string(next) + ":" + own;
        const std::string all = std::string(next) + ":" + s + ":" + own;
        const std::string watch = std::string("VOCEM_CHAIN_WATCH=") + own;
        const Outcome alone = run(door, both, watch.c_str());
        const Outcome o = run(door, all, watch.c_str());
        printf("     door %s without us: %s; the one behind was given %d\n", door,
               describe(alone, line, sizeof line), alone.watched);
        printf("     door %s with us:    %s; the one behind was given %d\n", door,
               describe(o, line, sizeof line), o.watched);
        char what[200];
        snprintf(what, sizeof what,
                 "door %s, between two interposers: the shim hands every frame over once", door);
        expect(clean(o) && o.presents == kFrames, what);
        // Without us the one behind may be given nothing: the one ahead
        // forwards with dlsym(RTLD_NEXT), which reaches the behind one's own
        // dlsym and is resolved relative to IT (MangoHud's "RTLD_NEXT is still
        // broken"). With us that lookup is ours, answered for its real caller.
        // The first version of the chain gave the one ahead every frame twice
        // and the one behind none, at door 3: the table's "hook" was the one
        // ahead's function.
        snprintf(what, sizeof what,
                 "door %s, between two interposers: both others are given every frame, once", door);
        expect(clean(alone) && clean(o) && o.chain == kFrames && o.watched == kFrames, what);
    }

    // The session's own value starts with a colon, and dladdr names the
    // executable by argv[0]: an empty entry must never match an empty name.
    printf("-- the session's LD_PRELOAD (a leading colon) and an empty argv[0]\n");
    for (const char* door : {"2", "3"}) {
        const Outcome o = run(door, ":" + s, nullptr, "");
        printf("     door %s: %s\n", door, describe(o, line, sizeof line));
        char what[160];
        snprintf(what, sizeof what,
                 "door %s, argv[0] empty: the game is not taken for another interposer", door);
        expect(clean(o) && o.presents == kFrames, what);
    }

    // A link asking the global dispatcher for a name it does not hook, inside
    // its own present: an answer, not a null.
    printf("-- MangoHud's shape asks the global eglGetProcAddress inside its present\n");
    for (const char* door : {"1", "3"}) {
        const Outcome alone = run(door, asks);
        const Outcome o = run(door, s + ":" + asks);
        printf("     door %s alone:    %s, %d nulls\n", door, describe(alone, line, sizeof line),
               alone.nulls);
        printf("     door %s after us: %s, %d nulls\n", door, describe(o, line, sizeof line),
               o.nulls);
        char what[160];
        snprintf(what, sizeof what,
                 "door %s: the link is given every frame and every name it asks for", door);
        expect(clean(alone) && clean(o) && o.presents == kFrames && o.chain == alone.chain &&
                   o.nulls == alone.nulls && o.nulls == 0,
               what);
    }

    // Another copy of the shim behind this one (a second prefix, a build tree):
    // one hand-over per frame on the dlsym and dispatcher doors. Door 1 hands
    // the frame to both copies' exports, as it always did, and is not asserted.
    printf("-- another copy of the shim behind this one\n");
    {
        const std::string copy_dir = "shim_chain_copy";
        mkdir(copy_dir.c_str(), 0700);
        char resolved[4096];
        const std::string copy = std::string(realpath(copy_dir.c_str(), resolved) ? resolved : "") +
                                 "/" + (strrchr(shim, '/') ? strrchr(shim, '/') + 1 : shim);
        bool copied = false;
        if (FILE* in = fopen(shim, "rb")) {
            if (FILE* out = fopen(copy.c_str(), "wb")) {
                char chunk[65536];
                size_t got = 0;
                copied = true;
                while ((got = fread(chunk, 1, sizeof chunk, in)) > 0) {
                    copied = copied && fwrite(chunk, 1, got, out) == got;
                }
                fclose(out);
            }
            fclose(in);
        }
        expect(copied, "a copy of the shim was made to preload behind it");
        for (const char* door : {"2", "3"}) {
            const Outcome o = run(door, s + ":" + copy);
            printf("     door %s: %s\n", door, describe(o, line, sizeof line));
            char what[160];
            snprintf(what, sizeof what, "door %s, two copies of the shim: every frame handed over once",
                     door);
            expect(copied && clean(o) && o.presents == kFrames, what);
        }
        unlink(copy.c_str());
        rmdir(copy_dir.c_str());
    }

    // --- the other order: asserted as measured, not as wished for ------------
    // The interposer ahead answers the application's dlsym with its own hook
    // and forwards to its own terminal pointer; nothing the shim does behind it
    // can change that. vocem-run puts the shim first for this reason.
    printf("-- the other order, MangoHud's shape ahead of the shim, door 3\n");
    {
        const Outcome o = run("3", std::string(own) + ":" + s);
        printf("     %s\n", describe(o, line, sizeof line));
        expect(clean(o) && o.presents == 0 && o.chain == kFrames,
               "behind another interposer the shim never sees a frame, and the other is unharmed");
    }

    printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

#endif  // CHAIN_LINKED
