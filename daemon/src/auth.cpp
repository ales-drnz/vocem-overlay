// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "auth.h"

#include "curl_sink.h"
#include "vocem/paths.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace vocem {
namespace {

constexpr const char* kTokenEndpoint = "https://streamkit.discord.com/overlay/token";

// Longer than any token Discord has issued (they are a few dozen characters);
// a file larger than this is not one.
constexpr size_t kTokenMaxBytes = 4096;

// The daemon's own log macro lives in log.h; this file is also compiled into
// tests/token_file.cpp on its own, where that header's prefix is not wanted.
#define LOG_TOKEN(text) std::fprintf(stderr, "[vocemd] %s\n", text)

}  // namespace

// A token is an OAuth bearer token: printable ASCII, a few dozen characters.
// Anything else in the file is not a token, whatever put it there.
bool token_is_well_formed(const std::string& token) {
    if (token.empty() || token.size() > kTokenMaxBytes) {
        return false;
    }
    for (const char c : token) {
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

std::string load_token() {
    // The same discipline as the avatar cache's local read (entry 47): the
    // path is under the user's own state directory, and a file there that
    // is not a regular file is not a token. O_NONBLOCK so a FIFO does not
    // hold the daemon at its very first line; O_NOFOLLOW so a link does not
    // send the read elsewhere; fstat so the size is known before anything is
    // read, and a symbolic link to /dev/zero -- which the old ifstream +
    // getline read until MemoryMax -- is refused as not regular.
    const int fd = ::open(token_path().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size > static_cast<off_t>(kTokenMaxBytes + 2)) {
        ::close(fd);
        LOG_TOKEN("the token file is not a small regular file: ignoring it");
        return {};
    }
    char buffer[kTokenMaxBytes + 3];
    const ssize_t got = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (got <= 0) {
        return {};
    }
    std::string token(buffer, static_cast<size_t>(got));
    if (const size_t end = token.find_first_of("\r\n"); end != std::string::npos) {
        token.resize(end);
    }
    if (!token_is_well_formed(token)) {
        // Said, because what follows -- asking Discord for authorisation
        // again -- looks exactly like a first run to the user, and a file
        // that is being ignored is the one thing they could not guess.
        LOG_TOKEN("the token file does not hold a token: ignoring it");
        return {};
    }
    return token;
}

bool save_token(const std::string& token) {
    const std::string path = token_path();
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        vocem::make_directories(path.substr(0, slash));
    }

    // Create with 0600 from the start rather than fixing the mode afterwards:
    // between the two there would be a window where the token is world-readable.
    // Written to a temporary and renamed into place, like every other file
    // this project writes: a crash between open and write used to leave an
    // empty token, and O_NOFOLLOW keeps a link at that name from steering the
    // write. O_EXCL, so a temporary somebody else left is not written over.
    const std::string temporary = path + ".part";
    ::unlink(temporary.c_str());
    const int fd =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const std::string payload = token + "\n";
    const ssize_t written = ::write(fd, payload.data(), payload.size());
    const bool flushed = ::fsync(fd) == 0;
    ::close(fd);
    if (written != static_cast<ssize_t>(payload.size()) || !flushed ||
        ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

void forget_token() { ::unlink(token_path().c_str()); }

std::string exchange_code_for_token(const std::string& code,
                                    const volatile std::sig_atomic_t* stop) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return {};
    }

    const nlohmann::json body{{"code", code}};
    const std::string payload = body.dump();
    // A token response is a few hundred bytes of JSON; anything past 64 KB is
    // not the endpoint answering the question that was asked.
    CurlSink sink;
    sink.limit = 64u * 1024u;

    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, kTokenEndpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vocem::curl_sink_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, VOCEM_USER_AGENT);
    // Required of a multi-threaded program by libcurl's own documentation:
    // without it a resolver timeout is delivered by SIGALRM to whichever
    // thread is unlucky. Inert with the threaded resolver Arch builds, which
    // is exactly the kind of build assumption to write down rather than rely
    // on.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // The way out on a stop: the same progress callback the avatar worker has
    // (avatars.cpp, entry 134). Without it a SIGTERM during this exchange --
    // the one transfer the daemon makes on its main thread -- waited for the
    // connect timeout, past the unit's TimeoutStopSec, and the daemon was
    // killed with its segment published.
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<volatile std::sig_atomic_t*>(stop));
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                     +[](void* flag, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                         const auto* stopping = static_cast<const volatile std::sig_atomic_t*>(flag);
                         return stopping && *stopping ? 1 : 0;
                     });

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result == CURLE_ABORTED_BY_CALLBACK) {
        std::fprintf(stderr, "[vocemd] token exchange abandoned: the daemon was told to stop\n");
        return {};
    }
    if (result != CURLE_OK || status < 200 || status >= 300 || sink.exceeded) {
        std::fprintf(stderr, "[vocemd] token exchange failed (HTTP %ld%s)\n", status,
                     sink.exceeded ? ", response too large" : "");
        return {};
    }

    const nlohmann::json parsed = nlohmann::json::parse(sink.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("access_token") ||
        !parsed["access_token"].is_string()) {
        // The usual cause is the user declining the prompt, in which case Discord
        // never issued a usable code.
        std::fprintf(stderr, "[vocemd] no access token in the exchange response\n");
        return {};
    }
    return parsed["access_token"].get<std::string>();
}

}  // namespace vocem
