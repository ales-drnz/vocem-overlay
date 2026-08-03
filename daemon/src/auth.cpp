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
#include <fstream>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace vocem {
namespace {

constexpr const char* kTokenEndpoint = "https://streamkit.discord.com/overlay/token";

}  // namespace

std::string load_token() {
    std::ifstream file(token_path());
    if (!file) {
        return {};
    }
    std::string token;
    std::getline(file, token);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
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
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }
    const std::string payload = token + "\n";
    const ssize_t written = ::write(fd, payload.data(), payload.size());
    ::close(fd);
    return written == static_cast<ssize_t>(payload.size());
}

void forget_token() { ::unlink(token_path().c_str()); }

std::string exchange_code_for_token(const std::string& code) {
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "vocem-overlay/0.1");

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

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
