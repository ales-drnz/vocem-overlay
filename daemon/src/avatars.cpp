// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "avatars.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include <curl/curl.h>

#include "vocem/avatar_rgba.h"
#include "curl_sink.h"
#include "log.h"
#include "vocem/avatar_file.h"
#include "vocem/paths.h"
#include "vocem/shared_state.h"

// The PNG parser lives in avatar_decode.h and only there, with the decoder's
// own dimension ceiling beside it; this file is what feeds it.
#include "avatar_decode.h"

namespace vocem {

namespace {

// A ceiling on what the RPC port can make this process remember. Every key
// arrives off the socket -- a voice event's participant or a notification's
// author -- and nothing removes one until its download fails, so a peer
// emitting a fresh author id per message grew `known_` (and with it `queue_`,
// which only ever holds keys `known_` just admitted) without limit, against a
// unit that carries MemoryMax=128M. The participant path has its own ceiling
// in main.cpp; this is the same answer for the container both paths land in.
// The number is far above any real session -- a channel holds kMaxUsers faces
// and a busy evening of messages is dozens -- so hitting it is a statement
// about the peer, and it is said in the log once.
constexpr size_t kAvatarKnownCeiling = 1024;

}  // namespace

AvatarCache::AvatarCache() {
    char dir[512];
    avatar_cache_dir(dir, sizeof(dir));
    cache_dir_ = dir;
    vocem::make_directories(cache_dir_);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    thread_ = std::thread([this] { worker(); });
}

AvatarCache::~AvatarCache() {
    stop();
    curl_global_cleanup();
}

void AvatarCache::stop() {
    {
        // Under the lock, not beside it: the worker evaluates the predicate with
        // the lock held and then blocks, and a notify landing between those two
        // steps is lost -- after which join() waits for a thread that is asleep
        // for good, and systemctl stop ends in SIGKILL with the segment still
        // published.
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_.exchange(true)) {
            return;
        }
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AvatarCache::request(uint64_t user_id, const std::string& avatar_hash) {
    char path[768];
    avatar_rgba_path(path, sizeof(path), user_id, avatar_hash.c_str());
    const std::string key = path;

    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (known_.count(key) != 0) {
            return;
        }
        if (known_.size() >= kAvatarKnownCeiling) {
            // Said once: silence here would be a face that never arrives with
            // nothing anywhere explaining why.
            static bool said = false;
            if (!said) {
                said = true;
                LOG("avatar cache ceiling reached (%zu ids): new faces stay the placeholder",
                    known_.size());
            }
            return;
        }
        known_.insert(key);
        if (vocem::avatar_file_exists(key.c_str())) {
            return;  // already on disk from a previous session
        }
        queue_.push({user_id, avatar_hash});
    }
    wake_.notify_one();
}

size_t AvatarCache::tracked() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return known_.size();
}

bool AvatarCache::download(const std::string& url, std::string& body) {
    // Into memory, not onto disk: what the CDN serves is input to a parser, and
    // the only thing this cache writes to disk any more is the parser's output.
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    // A 64-pixel PNG is a few kilobytes; a megabyte is not an avatar, whatever
    // the server says it is. The limit is in the write callback because
    // CURLOPT_MAXFILESIZE only reads the length a server advertises, and a
    // chunked response advertises none -- so the option alone let a hostile or
    // hijacked host stream for the whole 15-second timeout.
    CurlSink sink;
    sink.limit = 1024u * 1024u;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vocem::curl_sink_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
    // A redirect may not change what protocol this is: an avatar arrives over
    // HTTPS or not at all.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, VOCEM_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 1024L * 1024L);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || sink.exceeded || sink.body.empty()) {
        body.clear();
        return false;
    }
    body = std::move(sink.body);
    return true;
}

void AvatarCache::worker() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
            // Stopping means stopping. This used to drain the queue first, so a
            // channel filling up against an unreachable CDN made shutdown as long
            // as twenty-four downloads at fifteen seconds each -- well past the
            // unit's stop timeout, and a SIGKILL skips the unlink that takes the
            // segment away. A face nobody downloaded arrives on the next start.
            if (stopping_.load()) {
                return;
            }
            request = queue_.front();
            queue_.pop();
        }

        char path[768];
        avatar_rgba_path(path, sizeof(path), request.user_id, request.hash.c_str());

        // A cache written before the format changed has the same picture as a
        // PNG. Decoding that is strictly better than downloading it again --
        // and it clears the old file either way, since the games no longer look
        // for it.
        {
            char old_png[768];
            avatar_cache_path(old_png, sizeof(old_png), request.user_id, request.hash.c_str());
            if (vocem::avatar_file_exists(old_png)) {
                std::string png;
                // Bounded, regular-file-only and never through a symlink: the
                // reasons are in avatar_decode.h, and one of them is a hang
                // this thread could not come back from.
                const bool readable = avatar_read_local_png(old_png, png);
                const bool migrated =
                    readable &&
                    avatar_decode_and_store(reinterpret_cast<const unsigned char*>(png.data()),
                                            png.size(), path);
                if (readable) {
                    // Read and understood, or read and rubbish: either way the
                    // old file has said all it can and no game looks for it any
                    // more. A file that could not be *read* is left alone --
                    // deleting it would throw away a picture over a transient
                    // error, and the CDN fetch below covers this session.
                    ::unlink(old_png);
                }
                if (migrated) {
                    continue;
                }
                // A cached PNG that will not decode falls through to the CDN.
            }
        }

        std::string url;
        // The same check the cache path makes: an implausible hash means the
        // default avatar rather than a URL built out of whatever arrived.
        if (!avatar_hash_is_sane(request.hash.c_str())) {
            // Discord's default avatars are indexed by account id.
            url = "https://cdn.discordapp.com/embed/avatars/" +
                  std::to_string((request.user_id >> 22) % 6) + ".png";
        } else {
            // Animated avatars ("a_" prefix) are fetched as a static PNG: an
            // animated overlay is not worth the frame budget.
            url = "https://cdn.discordapp.com/avatars/" + std::to_string(request.user_id) + "/" +
                  request.hash + ".png?size=" + std::to_string(kAvatarPixels);
        }

        std::string body;
        const bool fetched = download(url, body);
        const bool stored =
            fetched &&
            avatar_decode_and_store(reinterpret_cast<const unsigned char*>(body.data()),
                                    body.size(), path);
        if (!stored) {
            // Which half failed is the whole diagnosis -- a CDN that cannot be
            // reached and a payload that will not decode are different
            // problems, and the user-visible symptom (a grey disc that never
            // fills in) is identical for both. auth.cpp names its two failure
            // modes; this path was the one that did not.
            LOG("avatar for %llu not %s", static_cast<unsigned long long>(request.user_id),
                fetched ? "decodable: the CDN's bytes were refused"
                        : "downloaded: the CDN could not be reached or refused the request");
            // Forget the key so a later attempt can retry; a transient CDN error
            // should not blank someone's avatar for the rest of the session.
            std::lock_guard<std::mutex> guard(mutex_);
            known_.erase(path);
        }
    }
}

}  // namespace vocem
