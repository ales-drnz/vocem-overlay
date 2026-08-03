// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Avatar fetching and caching.
//
// Downloads belong here, in the daemon, and never anywhere near a game process.
// The layer only ever opens a file that already exists on disk.
//
// Work happens on a background thread: a stalled CDN must not delay voice events.
// The RPC loop hands over a request and forgets about it; the file appears in the
// cache when it appears, and the layer picks it up on a later frame.

#ifndef VOCEM_AVATARS_H
#define VOCEM_AVATARS_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>

namespace vocem {

class AvatarCache {
public:
    AvatarCache();
    ~AvatarCache();

    AvatarCache(const AvatarCache&) = delete;
    AvatarCache& operator=(const AvatarCache&) = delete;

    // Queues a download unless the file is already cached or already queued.
    // An empty hash requests Discord's default avatar for that account.
    void request(uint64_t user_id, const std::string& avatar_hash);

    void stop();

private:
    struct Request {
        uint64_t user_id;
        std::string hash;
    };

    void worker();
    bool download(const std::string& url, std::string& body);

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::queue<Request> queue_;
    std::unordered_set<std::string> known_;  // cached or in flight
    std::atomic<bool> stopping_{false};
    std::string cache_dir_;
};

}  // namespace vocem

#endif  // VOCEM_AVATARS_H
