// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The clean-exit rename through the destructor's real door.
//
// tests/journal.cpp calls journal_end() directly, which proves the function
// and not the exit path -- and the exit path is where the first version
// failed: the ELF destructor runs after every function-local static's
// destructor, the journal path lived in a static std::string, and the rename
// read freed memory and silently did nothing. Every clean session stayed
// `.running` and read as a crash in the Debug section; history could never
// populate. Found by the fresh-context attack pass, on the real built
// libraries, after 59 green tests.
//
// So this walks the door itself: a child process dlopens a probe library (the
// shim's own way of loading the heavy library), begins a journal through an
// exported call, and exits NORMALLY -- static destructors first, _dl_fini
// after, exactly the order a game unwinds in. The parent then asks the
// filesystem. Against the std::string header the `.running` file survives the
// exit and this fails.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

bool file_exists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

}  // namespace

int main() {
    const char* probe = getenv("VOCEM_JOURNAL_EXITPROBE");
    if (!probe || !*probe) {
        printf("skip meant to run with VOCEM_JOURNAL_EXITPROBE pointing at the probe library\n");
        return 77;
    }

    char root[] = "/tmp/vocem-journal-exit-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("XDG_CACHE_HOME", root, 1);

    pid_t child = fork();
    if (child == 0) {
        void* handle = dlopen(probe, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fprintf(stderr, "dlopen: %s\n", dlerror());
            _exit(2);
        }
        using ProbeBegin = void (*)(const char*);
        auto begin = reinterpret_cast<ProbeBegin>(dlsym(handle, "probe_begin"));
        if (!begin) {
            _exit(2);
        }
        begin("exit_probe_child");
        // A NORMAL exit: static destructors, then _dl_fini and the probe's
        // ELF destructor -- the order a game unwinds in. Not _exit, which
        // would skip the very door under test.
        exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the probe child exited cleanly");

    const std::string dir = std::string(root) + "/vocem/journal";
    char running[600];
    snprintf(running, sizeof(running), "%s/%d.running", dir.c_str(), (int)child);
    char done[600];
    snprintf(done, sizeof(done), "%s/%d.done", dir.c_str(), (int)child);

    check(!file_exists(running),
          "the destructor's rename actually ran: no .running survives a clean exit");
    check(file_exists(done), "and the session is history, not evidence");

    bool clean_exit_noted = false;
    if (FILE* file = fopen(done, "r")) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, "clean exit")) {
                clean_exit_noted = true;
            }
        }
        fclose(file);
    }
    check(clean_exit_noted, "with the clean exit written in it");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        // best-effort scratch cleanup
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
