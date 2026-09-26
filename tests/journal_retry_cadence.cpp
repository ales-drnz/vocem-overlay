// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A journal that cannot be created is not asked for again on every frame.
//
// OverlaySession::journal_begin_once() is called by the GL hook on every
// drawn frame until the journal is open, and journal_begin() walks the
// journal directory twice (the prune of `.done` and of dead `.running`)
// before it tries the open. With a cache directory the game cannot write --
// a read-only home, a full disk, a sandbox that mounts it read-only -- that
// was 166 us and two opendir() per frame, for the life of the game, and not
// a word in the log (the review's journal_retry probe).
//
// Held here: 100 frames' worth of journal_begin_once() against a read-only
// journal directory make at most the one attempt's two directory walks, and
// the refusal is said once.

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "probe_alarm.h"
#include "vocem/journal.h"
#include "vocem/overlay_session.h"

// Every opendir() of this process, the vocem_common archive's included:
// the executable's definition is the one its calls resolve to.
static long g_opendirs = 0;
extern "C" DIR* opendir(const char* name) {
    ++g_opendirs;
    using Real = DIR* (*)(const char*);
    static Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "opendir"));
    return real(name);
}

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

int count_lines_with(const std::string& path, const char* needle) {
    int found = 0;
    if (std::FILE* file = std::fopen(path.c_str(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            if (std::strstr(line, needle)) {
                ++found;
            }
        }
        std::fclose(file);
    }
    return found;
}

}  // namespace

int main() {
    vocem_test::set_alarm(60, "asking for a journal that cannot be created");
    if (getuid() == 0) {
        std::printf("skip root writes into a read-only directory, so there is no refusal to see\n");
        return 77;
    }
    char scratch_template[] = "/tmp/vocem-journal-retry-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string cache = std::string(scratch) + "/cache";
    const std::string dir = cache + "/vocem/journal";
    vocem::make_directories(dir);
    chmod(dir.c_str(), 0555);  // a cache that cannot take a new file
    setenv("XDG_CACHE_HOME", cache.c_str(), 1);
    const std::string err_path = std::string(scratch) + "/stderr";
    setenv("VOCEM_DEBUG", "1", 1);
    unsetenv("VOCEM_LOG_FILE");
    std::fflush(stdout);
    const int saved_stderr = dup(2);
    const int err = open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(err, 2);
    close(err);

    vocem::OverlaySession session("opengl", "vocem/test");
    g_opendirs = 0;
    for (int frame = 0; frame < 100; ++frame) {
        session.journal_begin_once();
    }
    const long walks = g_opendirs;

    dup2(saved_stderr, 2);
    close(saved_stderr);
    chmod(dir.c_str(), 0755);
    std::printf("     100 frames: %ld opendir, journal %s\n", walks,
                vocem::detail::journal_file() ? "open" : "not open");
    check(!vocem::detail::journal_file(), "the journal could not be created (the premise)");
    check(walks <= 2, "a hundred frames walk the directory at most once (two opendir)");
    const int said = count_lines_with(err_path, "journal");
    std::printf("     log lines about the journal: %d\n", said);
    check(said == 1, "the refusal is said, once");

    std::string cleanup = std::string("rm -rf '") + scratch + "'";
    if (std::system(cleanup.c_str()) != 0) {
        std::printf("     (could not remove %s)\n", scratch);
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
