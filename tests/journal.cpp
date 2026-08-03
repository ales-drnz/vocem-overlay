// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The session journal: history when a process unwinds, evidence when it dies,
// counters while it lives.
//
// This grew out of the crash-marker test when the Debug section replaced the
// crash pop-up. The mechanism stays deliberately dumb -- no signal handler
// inside somebody else's game; the JVM owns SIGSEGV -- and the one behavioural
// change is the point of the rework: a clean exit no longer deletes the
// journal, it renames it into history (`.done`), because the Debug section
// reads finished sessions too. Against the crash-marker header a clean exit
// leaves nothing, and the history checks here fail. What this holds:
//
//   * a clean life leaves `.done` history (with its notes) and no `.running`;
//   * a death mid-life leaves `.running`, last note included;
//   * the crash scanner reports the dead child's journal and nobody else's --
//     not a live process's, not a recycled pid wearing the wrong name --
//     and journals in the legacy crash-marker directory are still found;
//   * the stat file holds the counters while alive and goes with the rename;
//   * history is pruned to its keep, oldest first.

#define VOCEM_JOURNAL_SCANNER
#include "vocem/journal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

bool file_exists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

}  // namespace

int main() {
    char root[] = "/tmp/vocem-journal-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("XDG_CACHE_HOME", root, 1);
    const std::string dir = vocem::journal_dir();

    // A clean life: begin, a note, counters, end. The journal becomes history;
    // the stat file does not outlive it.
    pid_t clean = fork();
    if (clean == 0) {
        vocem::journal_begin("opengl", "clean_child");
        vocem::journal_note("backend ready");
        vocem::journal_stat(1200, 640);
        vocem::journal_end();
        _exit(0);
    }
    int status = 0;
    waitpid(clean, &status, 0);

    char clean_running[600];
    snprintf(clean_running, sizeof(clean_running), "%s/%d.running", dir.c_str(), (int)clean);
    char clean_done[600];
    snprintf(clean_done, sizeof(clean_done), "%s/%d.done", dir.c_str(), (int)clean);
    char clean_stat[600];
    snprintf(clean_stat, sizeof(clean_stat), "%s/%d.stat", dir.c_str(), (int)clean);
    check(!file_exists(clean_running), "a clean exit closes its journal");
    check(file_exists(clean_done), "and leaves it as history rather than deleting it");
    check(!file_exists(clean_stat), "the stat file goes with it");
    bool history_notes = false;
    if (FILE* file = fopen(clean_done, "r")) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, "backend ready")) {
                history_notes = true;
            }
        }
        fclose(file);
    }
    check(history_notes, "history keeps the session's notes");

    // A death mid-life: begin, a note naming the work in progress, and _exit
    // with no unwinding -- the shape of a crash, from the journal's side.
    // Forked before the parent opens its own journal, so the child's begin()
    // is a real first one.
    pid_t doomed = fork();
    if (doomed == 0) {
        vocem::journal_begin("vulkan", "doomed_child");
        vocem::journal_note("uploading avatar 42_cafe.rgba");
        _exit(1);
    }
    waitpid(doomed, &status, 0);
    char doomed_running[600];
    snprintf(doomed_running, sizeof(doomed_running), "%s/%d.running", dir.c_str(), (int)doomed);

    // While alive, the stat file carries the counters and the scanner reads
    // them back.
    vocem::journal_begin("opengl", "vocem_journal");
    vocem::journal_stat(500, 250);
    long frames = 0;
    long drawn = 0;
    check(vocem::journal_read_stat((int)getpid(), frames, drawn) && frames == 500 &&
              drawn == 250,
          "the stat file round-trips the counters");

    // Games fork -- launchers, updaters -- and a forked child inherits the
    // parent's open journal AND the destructor that ends it. A child that
    // unwinds cleanly must not archive the parent's living journal: without
    // the owner-pid guard, this rename-under-the-parent is exactly what
    // happened, and the live-instances list lost the game while it played.
    pid_t forked = fork();
    if (forked == 0) {
        vocem::journal_end();  // what the destructor runs in the child
        vocem::journal_stat(9999, 9999);
        _exit(0);
    }
    waitpid(forked, &status, 0);
    char own_running[600];
    snprintf(own_running, sizeof(own_running), "%s/%d.running", dir.c_str(), (int)getpid());
    check(file_exists(own_running),
          "a forked child's clean exit leaves the parent's journal alive");
    check(vocem::journal_read_stat((int)getpid(), frames, drawn) && frames == 500,
          "and the parent's counters untouched");

    // A recycled pid: a dead process's journal wearing a live pid and somebody
    // else's name. And a journal in the LEGACY crash-marker directory, the
    // shape an upgrade leaves behind.
    char recycled[600];
    snprintf(recycled, sizeof(recycled), "%s/1%d.running", dir.c_str(), (int)getpid());
    if (FILE* file = fopen(recycled, "w")) {
        fprintf(file, "process = long_gone_game\npid = 1%d\napi = opengl\n--\n", (int)getpid());
        fclose(file);
    }
    const std::string legacy = vocem::journal_legacy_dir();
    vocem::make_directories(legacy);
    char legacy_running[600];
    snprintf(legacy_running, sizeof(legacy_running), "%s/4194305.running", legacy.c_str());
    if (FILE* file = fopen(legacy_running, "w")) {
        fprintf(file, "process = pre_upgrade_game\npid = 4194305\npath = opengl\n--\n");
        fclose(file);
    }

    const auto crashes = vocem::journal_crashes();
    bool found_doomed = false;
    bool found_self = false;
    bool found_legacy = false;
    for (const vocem::JournalEntry& entry : crashes) {
        if (entry.pid == (int)doomed) {
            found_doomed = true;
            check(entry.process == "doomed_child", "with the process name it recorded");
        }
        if (entry.pid == (int)getpid()) {
            found_self = true;
        }
        if (entry.pid == 4194305) {
            found_legacy = true;
        }
    }
    check(found_doomed, "the scanner reports the dead process's journal");
    check(!found_self, "and never a living one's");
    check(found_legacy, "a journal from the legacy crash directory is still found");
    check(crashes.size() == 3, "the recycled pid with the wrong name is reported too");

    // The live walk is the other half of the same question.
    const auto live = vocem::journal_live();
    bool live_self = false;
    for (const vocem::JournalEntry& entry : live) {
        if (entry.pid == (int)getpid()) {
            live_self = true;
        }
    }
    check(live_self, "the live walk finds the living process");

    // The last note is in the file, which is what the Debug section previews.
    bool last_note_present = false;
    if (FILE* file = fopen(doomed_running, "r")) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, "uploading avatar 42_cafe.rgba")) {
                last_note_present = true;
            }
        }
        fclose(file);
    }
    check(last_note_present, "the journal's last line names the work in progress");

    // The parent's own journal closes here: the pruner child below must fork
    // with no journal open, or its begin() -- and with it the prune under
    // test -- would be a no-op on the inherited file.
    vocem::journal_end();

    // History is bounded: seed well past the keep, then let a fresh process's
    // begin() prune. The newest survive; the oldest go.
    for (int i = 0; i < vocem::kJournalHistoryKeep + 5; ++i) {
        char old_done[600];
        snprintf(old_done, sizeof(old_done), "%s/%d.done", dir.c_str(), 100000 + i);
        if (FILE* file = fopen(old_done, "w")) {
            fprintf(file, "process = old_%d\npid = %d\napi = opengl\n--\n", i, 100000 + i);
            fclose(file);
        }
        struct timespec times[2];
        times[0].tv_sec = time(nullptr) - 10000 + i;
        times[0].tv_nsec = 0;
        times[1] = times[0];
        utimensat(AT_FDCWD, old_done, times, 0);
    }
    pid_t pruner = fork();
    if (pruner == 0) {
        vocem::journal_begin("opengl", "pruner_child");
        vocem::journal_end();
        _exit(0);
    }
    waitpid(pruner, &status, 0);
    const auto history = vocem::journal_history();
    check((int)history.size() <= vocem::kJournalHistoryKeep,
          "history is pruned to its keep");
    bool newest_kept = false;
    for (const vocem::JournalEntry& entry : history) {
        if (entry.pid == (int)pruner || entry.pid == (int)clean) {
            newest_kept = true;
        }
    }
    check(newest_kept, "and the newest sessions survive the prune");
    check(history.size() >= 2 && history.front().when >= history.back().when,
          "history comes back newest first");

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        // best-effort scratch cleanup
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
