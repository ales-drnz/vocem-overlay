// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The shim's first lookup of a real function, asked by several threads at once.
//
// real_for() looks a hooked name up through RTLD_NEXT once and remembers that it
// tried, so a null answer is neither cached nor asked for again forever (entry
// 36). The 0.1.10 shim wrote "tried" BEFORE the lookup had an answer: a second
// thread arriving in that window read tried = 1, next = null, and was told the
// real function did not exist. For a dispatcher that is a null
// eglGetProcAddress("glClear") handed to a game -- measured by the review with
// sixteen threads asking at once, some thread got NULL in 181 of 300 processes
// against the build's shim and 125 of 200 against build32's; 0 of 300 without
// the shim. The present hooks go through the same function, so the same window
// skipped a real swap.
//
// The window exists once per process -- the first ask -- so the measurement is
// many processes: each child is forked from a parent that has never asked, so
// it starts with the shim's table untouched, and sixteen of its threads ask at
// a barrier. Any null is a failure, and so is a null asked for afterwards.
//
// Needs the shim preloaded (VOCEM_SHIM_PRELOADED) and libEGL; says why when it
// cannot measure (exit 77). Nothing here presents, so the overlay is never
// loaded and no state segment is touched.

#include <EGL/egl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>

#include "probe_alarm.h"

namespace {

constexpr int kThreads = 16;
constexpr int kProcesses = 300;

pthread_barrier_t g_barrier;
std::atomic<int> g_nulls{0};

void* ask(void*) {
    pthread_barrier_wait(&g_barrier);
    if (!eglGetProcAddress("glClear")) {
        g_nulls.fetch_add(1);
    }
    return nullptr;
}

// One process's worth: sixteen first asks at once, then one more alone.
// The exit code is the number of threads told null (capped), plus 100 when
// the late ask was null too -- a null remembered for good.
int child() {
    pthread_barrier_init(&g_barrier, nullptr, kThreads);
    pthread_t threads[kThreads];
    for (pthread_t& thread : threads) {
        if (pthread_create(&thread, nullptr, &ask, nullptr) != 0) {
            return 120;
        }
    }
    for (pthread_t thread : threads) {
        pthread_join(thread, nullptr);
    }
    const int nulls = g_nulls.load();
    return (nulls > 99 ? 99 : nulls) + (eglGetProcAddress("glClear") ? 0 : 100);
}

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded\n");
        return 77;
    }
    vocem_test::set_alarm(120, "three hundred processes of sixteen first lookups");

    // Whether this machine can answer at all, asked in a child so the parent's
    // shim stays untouched: a system without an EGL that knows glClear has
    // nothing to measure, and that is a skip rather than a pass.
    {
        const pid_t pid = fork();
        if (pid == 0) {
            _exit(eglGetProcAddress("glClear") ? 0 : 1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("skip eglGetProcAddress(\"glClear\") is null even when asked alone\n");
            return 77;
        }
    }

    int processes_with_null = 0;
    int threads_told_null = 0;
    int late_nulls = 0;
    int broken = 0;
    for (int run = 0; run < kProcesses; ++run) {
        const pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL fork\n");
            return 1;
        }
        if (pid == 0) {
            _exit(child());
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) == 120) {
            ++broken;
            continue;
        }
        const int code = WEXITSTATUS(status);
        if (code >= 100) {
            ++late_nulls;
        }
        if (code % 100 != 0) {
            ++processes_with_null;
            threads_told_null += code % 100;
        }
    }

    printf("     %d of %d processes had a thread told eglGetProcAddress(\"glClear\") is null "
           "(%d threads in all); %d answered null when asked again alone; %d did not run\n",
           processes_with_null, kProcesses, threads_told_null, late_nulls, broken);
    const bool ok = processes_with_null == 0 && late_nulls == 0 && broken == 0;
    printf("%s a first lookup asked by %d threads at once answers every one of them\n",
           ok ? "ok  " : "FAIL", kThreads);
    return ok ? 0 : 1;
}
