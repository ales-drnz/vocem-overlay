// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What almost every probe in this directory wrote for itself: one line per
// claim ("ok  " or "FAIL"), a count of the failures that becomes the exit
// code, whole files written and read, and a scratch directory.
//
// A probe takes them with `using vocem_test::check;` and
// `using vocem_test::failures;`, so its own lines read as they did.

#ifndef VOCEM_TESTS_VOCEM_CHECK_H
#define VOCEM_TESTS_VOCEM_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <initializer_list>
#include <string>

namespace vocem_test {

// Failed claims so far. A forked child has its own copy, as it had of the
// global each probe used to define.
inline int failures = 0;

inline void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

inline void check(bool condition, const std::string& what) {
    check(condition, what.c_str());
}

// The closing line and the exit code every probe ends on.
inline int finish() {
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

// The whole of `contents` at `path`, replacing what was there. False when the
// file cannot be opened or the write came up short.
inline bool write_file(const std::string& path, const std::string& contents) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const bool whole = fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    return fclose(file) == 0 && whole;
}

// The whole file, or empty when it cannot be opened.
inline std::string read_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }
    fclose(file);
    return out;
}

// A fresh directory /tmp/<prefix>-XXXXXX with each of `leaves` made under it
// (in order, so "/a" comes before "/a/b"). Empty when mkdtemp fails.
inline std::string scratch_dir(const char* prefix, std::initializer_list<const char*> leaves = {}) {
    std::string pattern = std::string("/tmp/") + prefix + "-XXXXXX";
    if (!mkdtemp(&pattern[0])) {
        return {};
    }
    for (const char* leaf : leaves) {
        mkdir((pattern + leaf).c_str(), 0700);
    }
    return pattern;
}

inline void remove_tree(const std::string& path) {
    if (path.rfind("/tmp/", 0) == 0) {
        (void)!system(("rm -rf '" + path + "'").c_str());
    }
}

// The settings of an overlay under test: `root`/vocem/config.ini saying
// `enabled = true` and `shown_apps = <program>` (probes are not games, and
// the detection is right to ignore them), $XDG_CONFIG_HOME at `root` and
// $XDG_CACHE_HOME at `root`/cache.
inline bool overlay_config_home(const std::string& root, const std::string& program) {
    mkdir((root + "/vocem").c_str(), 0700);
    mkdir((root + "/cache").c_str(), 0700);
    const bool written = write_file(root + "/vocem/config.ini",
                                    "enabled = true\nshown_apps = " + program + "\n");
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("XDG_CACHE_HOME", (root + "/cache").c_str(), 1);
    return written;
}

}  // namespace vocem_test

#endif  // VOCEM_TESTS_VOCEM_CHECK_H
