// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The one CURLOPT_WRITEFUNCTION callback. It existed twice, identical to the
// character, in the two files of this binary that download anything -- the
// entry-33 shape at its smallest.
//
// It carries a byte limit because `CURLOPT_MAXFILESIZE` does not: that option
// acts on the length a server *advertises*, and a response with no length
// (chunked, which any server may choose) advertises nothing -- so the callback
// was the only place a body could be bounded, and it bounded nothing. It also
// may not let an exception out: it is called from libcurl's own C frames, where
// an unwind is undefined behaviour, and `append`'s bad_alloc had nothing
// between it and std::terminate.

#ifndef VOCEM_CURL_SINK_H
#define VOCEM_CURL_SINK_H

#include <cstddef>

// The one User-Agent, beside the one write callback, for the same reason: it
// existed twice, identical to the character, in the two files of this binary
// that download anything -- and both copies said 0.1 while the package said
// 0.1.5, because a hand-written version is a version nobody re-measures
// (entry 110's shape, in the one string the daemon shows a server it does not
// control). The number comes from CMakeLists.txt's project(VERSION) through
// VOCEM_VERSION, the same road the window's About page takes, and
// tests/version_agrees.cmake holds the definition to the file.
#define VOCEM_USER_AGENT "vocem-overlay/" VOCEM_VERSION
#include <string>

namespace vocem {

// Where a download lands and how much of it is allowed to. Every caller names
// its own limit: an avatar is a few kilobytes, a token exchange a few hundred
// bytes, and neither has a legitimate megabyte in it.
struct CurlSink {
    std::string body;
    size_t limit = 0;
    bool exceeded = false;
};

// Returning fewer bytes than were offered is how a write callback tells libcurl
// to abort the transfer (CURLE_WRITE_ERROR), which is what every refusal here
// wants: no partial body is worth keeping.
inline size_t curl_sink_write(char* data, size_t size, size_t count, void* user_data) {
    auto* sink = static_cast<CurlSink*>(user_data);
    if (size != 0 && count > static_cast<size_t>(-1) / size) {
        sink->exceeded = true;  // not a size any body has
        return 0;
    }
    const size_t bytes = size * count;
    if (bytes > sink->limit || sink->body.size() > sink->limit - bytes) {
        sink->exceeded = true;
        return 0;
    }
    try {
        sink->body.append(data, bytes);
    } catch (...) {
        sink->exceeded = true;
        return 0;
    }
    return bytes;
}

}  // namespace vocem

#endif  // VOCEM_CURL_SINK_H
