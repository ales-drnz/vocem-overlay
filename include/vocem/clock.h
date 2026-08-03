// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The one monotonic clock, spelled once.
//
// The daemon stamps Notification::received with CLOCK_MONOTONIC and the games
// decide a toast's age against the same clock -- shared_state.h relies on the
// two sides reading the *same* clock, which makes this exactly the kind of
// value entry 33 is about: it existed in five hand-written copies across the
// daemon, both injected paths and two headers, identical but free to drift.
// One spelling here; everything else includes it.

#ifndef VOCEM_CLOCK_H
#define VOCEM_CLOCK_H

#include <time.h>

namespace vocem {

inline double monotonic_seconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

}  // namespace vocem

#endif  // VOCEM_CLOCK_H
