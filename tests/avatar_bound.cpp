// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar cache's memory is bounded against the peer that feeds it.
//
// Every key in AvatarCache::known_ arrives off the RPC socket -- a voice
// event's participant or a notification's author -- and nothing removes one
// until its download fails. The participant path has its own ceiling in
// main.cpp; the notification path reaches the same containers through
// Session::notify() with no bound of its own, so a peer on the RPC port
// emitting a fresh author id per message grew known_ and queue_ without limit,
// against a unit that carries MemoryMax=128M. This asks the real AvatarCache,
// through its own request(), that the growth stops at the ceiling.
//
// The worker is stopped before the requests: with it running, what remains
// tracked would depend on how many downloads had already failed (each failure
// erases its key), and the reading would not be the same twice. Stopped, every
// admitted key stays, which is the worst case and the reproducible one. The
// cache directory is pointed into a scratch XDG_CACHE_HOME so nothing of the
// user's is read or written.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "avatars.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

int main() {
    char scratch[] = "/tmp/vocem-avatar-bound-XXXXXX";
    if (!mkdtemp(scratch)) {
        std::perror("mkdtemp");
        return 1;
    }
    setenv("XDG_CACHE_HOME", scratch, 1);

    vocem::AvatarCache cache;
    cache.stop();

    // Far past the ceiling, each id distinct, each with a plausible hash so the
    // request is the expensive shape (a download would have been queued).
    for (uint64_t id = 1; id <= 5000; ++id) {
        cache.request(id, "00000000000000000000000000000000");
    }
    const size_t after_flood = cache.tracked();
    std::printf("  tracked after 5000 distinct ids: %zu\n", after_flood);
    check(after_flood <= 1024, "the flood stops at the ceiling");
    check(after_flood >= 1000, "and the ceiling is not doing the deduplication's job");

    // The same id again is remembered once, not twice: the ceiling must never
    // be reached by repetition, which is the ordinary case.
    const size_t before_repeat = cache.tracked();
    cache.request(1, "00000000000000000000000000000000");
    check(cache.tracked() == before_repeat, "a repeated id is not remembered again");

    if (failures == 0) {
        std::printf("avatar bound: every check holds\n");
        return 0;
    }
    std::printf("avatar bound: %d failures\n", failures);
    return 1;
}
