// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The things that have to come out the same at 32 bits as at 64.
//
// The overlay is installed twice -- `/usr/lib` and `/usr/lib32` -- because a Windows
// game under Proton is often a 32-bit binary, and The Binding of Isaac is one. Both
// halves talk to a **64-bit daemon**: they read the same shared-memory segment and
// look for the same files in the same cache. Anything that turns a 64-bit value into
// text therefore has to produce the same text on both, or the two halves quietly
// disagree about the name of a file and nobody finds out.
//
// That is not a hypothetical either. `avatar_cache_path` formatted a Discord id with
// `%lu` and an `unsigned long`, which is sixty-four bits on x86-64 and thirty-two on
// i386. The daemon wrote `1018972252676554842_<hash>.png`; the 32-bit game looked for
// `1959002202_<hash>.png`, found nothing, and drew a grey circle where every face
// should have been. No error anywhere -- a missing avatar file is an ordinary thing
// that the code is written to shrug at.
//
// This file is built twice, and the second build is the one that matters. It is the
// third defect in a row to live in the half that had no tests; the pattern is not
// "check the dlsym version" or "check the format string", it is **build it for both
// and compare**.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "vocem/avatar_rgba.h"
#include "vocem/shared_state.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    printf("built for %zu-bit (unsigned long is %zu bits)\n", sizeof(void*) * 8,
           sizeof(unsigned long) * 8);

    // A fixed cache directory, so the expected strings below are the whole answer
    // and not a function of whoever is running this.
    setenv("XDG_CACHE_HOME", "/cache", 1);

    // Real ids, from a real channel. The first is the one that was measured
    // truncated: 310503940594860049 came out as 1485045777 on i386.
    struct Case {
        unsigned long long id;
        const char* hash;
        const char* expected;
    };
    const Case cases[] = {
        {310503940594860049ULL, "a71d433becd902959baa0b8e59e9095c",
         "/cache/vocem/avatars/310503940594860049_a71d433becd902959baa0b8e59e9095c.png"},
        {1018972252676554842ULL, "d907b387c7c707262918a6a6709980f7",
         "/cache/vocem/avatars/1018972252676554842_d907b387c7c707262918a6a6709980f7.png"},
        // The largest id that fits in sixty-four bits, which is where a width
        // mistake is loudest.
        {18446744073709551615ULL, "ffffffffffffffffffffffffffffffff",
         "/cache/vocem/avatars/18446744073709551615_ffffffffffffffffffffffffffffffff.png"},
        // And one that fits in thirty-two, so a fix that merely swapped the
        // specifier the other way round would still be caught by the ones above.
        {42ULL, "0123456789abcdef0123456789abcdef",
         "/cache/vocem/avatars/42_0123456789abcdef0123456789abcdef.png"},
    };

    for (const Case& one : cases) {
        char path[768];
        vocem::avatar_cache_path(path, sizeof(path), one.id, one.hash);
        const bool same = strcmp(path, one.expected) == 0;
        if (!same) {
            printf("     wanted %s\n     got    %s\n", one.expected, path);
        }
        check(same, "the avatar's path is the same string the daemon wrote");
    }

    // The fallback face, which is picked from the id's high bits -- so it is a
    // second thing that a truncated id would silently get wrong, in a way that looks
    // like a working feature.
    char fallback[768];
    vocem::avatar_cache_path(fallback, sizeof(fallback), 310503940594860049ULL, "");
    check(strcmp(fallback, "/cache/vocem/avatars/default_4.png") == 0,
          "and the default face comes from the whole id, not half of it");

    // The raw-RGBA cache is the successor of the paths above and lives one format
    // change away from the same mistake: the 64-bit daemon writes the file, both
    // widths of the injected code look for it by name.
    for (const Case& one : cases) {
        char png_path[768];
        char rgba_path[768];
        vocem::avatar_cache_path(png_path, sizeof(png_path), one.id, one.hash);
        vocem::avatar_rgba_path(rgba_path, sizeof(rgba_path), one.id, one.hash);
        std::string expected = one.expected;
        expected.replace(expected.rfind(".png"), 4, ".rgba");
        const bool same = expected == rgba_path;
        if (!same) {
            printf("     wanted %s\n     got    %s\n", expected.c_str(), rgba_path);
        }
        check(same, "the rgba path is the png path with the format's own extension");
        (void)png_path;
    }
    char rgba_fallback[768];
    vocem::avatar_rgba_path(rgba_fallback, sizeof(rgba_fallback), 310503940594860049ULL, "");
    check(strcmp(rgba_fallback, "/cache/vocem/avatars/default_4.rgba") == 0,
          "and the default face's rgba name also comes from the whole id");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
