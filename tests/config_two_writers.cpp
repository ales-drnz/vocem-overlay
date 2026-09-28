// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Two processes writing config.ini at once, through the one writer
// (Config::edit_file), each a key of its own.
//
// Two windows can run since a window that cannot listen on its socket opens
// anyway (entry 295), and each writes the file on every click. The writer read
// the file, edited it and wrote `<file>.tmp`, a fixed name, deleting one
// already there: the second writer deleted the first's temporary, the first's
// rename then moved the second's file or failed, and neither read-edit-write
// excluded the other, so a write that answered true could be undone by the
// other process writing what it had read before it. The 2026-09-28 refutation
// pass: two processes, 300 switches each, 7-12 refused and 3-5 answered true
// and never reached the file.
//
// Here each writer counts: its edit reads its own key from the file as it
// stands, adds one and writes it back. Every write that answered true is one
// on the file at the end, or a write was lost; and none is refused, because
// nothing but the other writer stands in the way.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "vocem/config.h"
#include "vocem_check.h"

using vocem_test::check;

namespace {

constexpr int kWrites = 300;

// One writer: kWrites increments of `key`, the count of those that answered
// true written to `report`.
void writer(std::string vocem::Config::*key, int start, int report) {
    char go = 0;
    if (read(start, &go, 1) != 1) {
        _exit(2);
    }
    int written = 0;
    for (int i = 0; i < kWrites; ++i) {
        const bool ok = vocem::Config::edit_file([key](vocem::Config& disk) {
            disk.*key = std::to_string(atoi((disk.*key).c_str()) + 1);
        });
        written += ok ? 1 : 0;
    }
    if (write(report, &written, sizeof(written)) != sizeof(written)) {
        _exit(3);
    }
    _exit(0);
}

}  // namespace

int main() {
    char root[] = "/tmp/vocem-config-writers-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", root, 1);
    unsetenv("FLATPAK_ID");
    int start[2];
    int reports[2][2];
    if (pipe(start) != 0 || pipe(reports[0]) != 0 || pipe(reports[1]) != 0) {
        printf("FAIL pipe\n");
        return 1;
    }
    std::string vocem::Config::*keys[2] = {&vocem::Config::shown_apps,
                                            &vocem::Config::hidden_apps};
    pid_t children[2];
    for (int w = 0; w < 2; ++w) {
        children[w] = fork();
        if (children[w] == 0) {
            writer(keys[w], start[0], reports[w][1]);
        }
    }
    // Both at once.
    const char go[2] = {1, 1};
    if (write(start[1], go, 2) != 2) {
        printf("FAIL could not start the writers\n");
        return 1;
    }
    int answered[2] = {-1, -1};
    for (int w = 0; w < 2; ++w) {
        int status = 0;
        waitpid(children[w], &status, 0);
        if (read(reports[w][0], &answered[w], sizeof(int)) != sizeof(int)) {
            answered[w] = -1;
        }
    }
    vocem::Config on_disk;
    const bool loaded = on_disk.load();
    const int counted[2] = {atoi(on_disk.shown_apps.c_str()), atoi(on_disk.hidden_apps.c_str())};
    int leftovers = 0;
    std::string vocem_dir = std::string(root) + "/vocem";
    if (DIR* dir = opendir(vocem_dir.c_str())) {
        while (dirent* entry = readdir(dir)) {
            leftovers += strstr(entry->d_name, ".tmp") ? 1 : 0;
        }
        closedir(dir);
    }
    for (int w = 0; w < 2; ++w) {
        printf("     writer %d: %d of %d writes answered true, %d on the file\n", w, answered[w],
               kWrites, counted[w]);
    }
    check(loaded, "the file reads whole at the end");
    check(answered[0] == kWrites && answered[1] == kWrites,
          "no write was refused: the other writer is no reason to refuse one");
    check(counted[0] == answered[0] && counted[1] == answered[1],
          "every write that answered true is on the file");
    check(leftovers == 0, "and no temporary is left beside it");

    char cleanup[200];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        printf("     (the scratch root %s outlived the test)\n", root);
    }
    return vocem_test::finish();
}
