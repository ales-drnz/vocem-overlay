// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A journal's suffix is the end of its name, not the first ".running" or
// ".done" anywhere in its path.
//
// journal_stat_path_for() and journal_end() found the suffix with strstr over
// the WHOLE path, so a cache directory whose path carries either word -- a
// Flatpak game's `~/.var/app/org.running.Game/cache`, a user's `backup.done`
// -- was cut there: the stat file went to `~/.var/app/org.stat`, and the
// clean-exit rename moved the journal out of the journal directory to
// `~/.var/app/org.done` (the review's stat_path probe). The scanner's
// filters had the same first-match shape on the file name.
//
// Held here: the path mapping for the probe's cases, and a whole journal
// life -- begin, stat, end -- under an XDG_CACHE_HOME named with both words,
// after which the history is `<pid>.done` inside the journal directory and
// nothing was written above it. And a suffixed name, `<pid>-<n>.done`, is
// listed under its pid, which is what journal_begin's comment says.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define VOCEM_JOURNAL_SCANNER
#include "probe_alarm.h"
#include "vocem/journal.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

std::string stat_for(const char* path) {
    char out[560];
    vocem::journal_stat_path_for(path, out, sizeof(out));
    return out;
}

bool exists(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

}  // namespace

int main() {
    vocem_test::set_alarm(30, "a journal under a path that says running and done");

    check(stat_for("/home/u/.var/app/org.running.Game/cache/vocem/journal/812.running") ==
              "/home/u/.var/app/org.running.Game/cache/vocem/journal/812.stat",
          "a directory called org.running.Game keeps its name");
    check(stat_for("/home/u/backup.done/cache/vocem/journal/812.done") ==
              "/home/u/backup.done/cache/vocem/journal/812.stat",
          "a directory called backup.done keeps its name");
    check(stat_for("/home/u/.cache/vocem/journal/812-2.running") ==
              "/home/u/.cache/vocem/journal/812-2.stat",
          "a suffixed journal's stat file sits beside it");
    check(stat_for("/home/u/.cache/vocem/journal/812.running.old").empty(),
          "a name that does not END in a journal suffix has no stat file");

    char scratch_template[] = "/tmp/vocem-journal-suffix-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string base = std::string(scratch) + "/org.running.Game";
    const std::string cache = base + "/cache.done";
    setenv("XDG_CACHE_HOME", cache.c_str(), 1);
    const std::string dir = vocem::journal_dir();

    check(vocem::journal_begin("opengl", "probe"), "the journal opens");
    vocem::journal_stat(10, 5);
    char pid_name[32];
    std::snprintf(pid_name, sizeof(pid_name), "%d", static_cast<int>(getpid()));
    check(exists(dir + "/" + pid_name + ".stat"), "the stat file is beside the journal");
    check(!exists(std::string(scratch) + "/org.stat"), "and not above the cache directory");
    check(!exists(base + "/cache.stat"), "nor in place of the cache directory's name");

    vocem::journal_end();
    check(exists(dir + "/" + pid_name + ".done"),
          "the clean exit leaves <pid>.done in the journal directory");
    check(!exists(std::string(scratch) + "/org.done"), "and did not rename the journal above it");
    check(!exists(dir + "/" + pid_name + ".stat"), "and removed the stat file");
    const std::vector<vocem::JournalEntry> history = vocem::journal_history();
    check(history.size() == 1 && history[0].pid == static_cast<int>(getpid()),
          "the history lists this session");

    // A suffixed journal -- the name journal_begin takes when `<pid>.running`
    // is there already -- is listed under its process's pid: what the comment
    // in journal_begin says the scanner does, held rather than said.
    if (std::FILE* suffixed = std::fopen((dir + "/4242-3.done").c_str(), "w")) {
        std::fprintf(suffixed, "process = other\npid = 4242\napi = vulkan\nstarted = ?\n--\n");
        std::fclose(suffixed);
    }
    bool listed = false;
    for (const vocem::JournalEntry& entry : vocem::journal_history()) {
        listed = listed || (entry.pid == 4242 && entry.process == "other");
    }
    check(listed, "a suffixed journal is listed under its process's pid");

    std::string cleanup = std::string("rm -rf '") + scratch + "'";
    if (std::system(cleanup.c_str()) != 0) {
        std::printf("     (could not remove %s)\n", scratch);
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
