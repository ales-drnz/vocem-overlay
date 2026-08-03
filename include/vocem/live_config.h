// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Keeps the user settings current inside a game process without ever reading the
// file on the hot path more than it has to.
//
// The GUI writes config.ini and expects the overlay to follow within a second or
// two. Polling the modification time is one stat() call, so it is done on a timer
// rather than per frame, and the file is only parsed when the timestamp moved.

#ifndef VOCEM_LIVE_CONFIG_H
#define VOCEM_LIVE_CONFIG_H

#include "vocem/clock.h"
#include "vocem/config.h"

namespace vocem {

class LiveConfig {
public:
    // Reloads at most once every `interval_seconds`, and only if the file changed.
    const Config& current(double interval_seconds = 2.0) {
        const double now = monotonic_seconds();
        if (!loaded_) {
            loaded_ = true;
            last_check_ = now;
            last_mtime_ = Config::mtime();
            config_.load();
            return config_;
        }
        if (now - last_check_ >= interval_seconds) {
            last_check_ = now;
            const long long mtime = Config::mtime();
            if (mtime != last_mtime_) {
                last_mtime_ = mtime;
                config_ = Config{};
                config_.load();
            }
        }
        return config_;
    }

private:
    Config config_;
    double last_check_ = 0.0;
    long long last_mtime_ = 0;
    bool loaded_ = false;
};

}  // namespace vocem

#endif  // VOCEM_LIVE_CONFIG_H
