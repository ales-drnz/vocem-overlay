// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A probe's own deadline, and a probe that says when it hit it.
//
// Every long probe here arms alarm() so that a hang is bounded rather than left
// to ctest's default. The default disposition of SIGALRM is to kill the
// process, so what the suite saw was: every one of the probe's own `ok` lines,
// then nothing, then a dead process -- which reads exactly like a hang and is
// not one, and which CLAUDE.md spends a paragraph teaching the next reader to
// recognise by eye ("a draw test that passes its checks and dies at exactly its
// alarm; the answer is to re-run it alone before believing anything"). Two real
// runs were diagnosed that way on 2026-08-17.
//
// A handler is five lines and retires the paragraph: the probe says which
// deadline it hit and how long it had, on its own output, and exits non-zero
// rather than being signalled -- so ctest reports a failure with a reason in it
// instead of "Subprocess aborted".
//
// Async-signal-safe on purpose: write(2) on a preformatted buffer and _exit(2),
// nothing else. printf from a handler is not safe and this runs in probes that
// are inside somebody's GL driver.
//
// NOT for the deliberate ones. Several tests fork a child, arm a short alarm in
// it, and use its death as the measurement -- "against the defective binary this
// test does not fail, it hangs, which is what its TIMEOUT is for" (entry 98).
// Those keep the bare alarm(): there the kill is the result.

#ifndef VOCEM_TEST_PROBE_ALARM_H
#define VOCEM_TEST_PROBE_ALARM_H

#include <signal.h>
#include <string.h>
#include <unistd.h>

namespace vocem_test {
namespace detail {

inline char g_alarm_message[128];
inline size_t g_alarm_length = 0;

inline void on_alarm(int) {
    if (g_alarm_length) {
        ssize_t ignored = ::write(2, g_alarm_message, g_alarm_length);
        (void)ignored;
    }
    _exit(1);
}

}  // namespace detail

// Arms the probe's deadline and makes hitting it legible. Call once, at the top
// of main, after any sandbox re-exec (a re-exec would lose the handler).
inline void set_alarm(unsigned seconds, const char* what) {
    // Built now rather than in the handler, which may not format anything.
    const char prefix[] = "\nFAIL the probe hit its own alarm (";
    const char middle[] = " s): ";
    char digits[16];
    size_t digit_count = 0;
    unsigned value = seconds;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && digit_count < sizeof(digits));

    char* out = detail::g_alarm_message;
    const char* const end = detail::g_alarm_message + sizeof(detail::g_alarm_message) - 2;
    for (const char* p = prefix; *p && out < end; ++p) {
        *out++ = *p;
    }
    while (digit_count && out < end) {
        *out++ = digits[--digit_count];
    }
    for (const char* p = middle; *p && out < end; ++p) {
        *out++ = *p;
    }
    for (const char* p = what; what && *p && out < end; ++p) {
        *out++ = *p;
    }
    *out++ = '\n';
    detail::g_alarm_length = static_cast<size_t>(out - detail::g_alarm_message);

    struct sigaction action {};
    action.sa_handler = detail::on_alarm;
    sigemptyset(&action.sa_mask);
    // No SA_RESTART: the point is to interrupt whatever is blocking.
    action.sa_flags = 0;
    sigaction(SIGALRM, &action, nullptr);
    ::alarm(seconds);
}

}  // namespace vocem_test

#endif  // VOCEM_TEST_PROBE_ALARM_H
