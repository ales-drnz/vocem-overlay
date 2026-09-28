// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The token file, read and written the way the rest of the project reads and
// writes files under the user's own directories (daemon/src/auth.cpp).
//
// It was the one file the daemon opened with nothing in the way: an ifstream
// and a getline, following whatever the path was and reading until a newline
// arrived. Entry 47 closed exactly this shape for the avatar cache -- a link to
// /dev/zero read 3 GB in 3 seconds and the daemon could not be restarted -- and
// the token, one directory over, kept it. And a file holding anything but a
// token was handed to the JSON serialiser whole, whose dump() refuses invalid
// UTF-8 by throwing: one 0xFF byte in the file ended the daemon on its first
// READY, and again every ten seconds under Restart=on-failure (entry 133).
//
// Compiles auth.cpp directly, like daemon_ws_bounds compiles websocket.cpp:
// the token exchange in the same file needs libcurl, which is linked, and is
// never called.

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "auth.h"
#include "vocem/paths.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

void write_bytes(const std::string& path, const char* bytes, size_t length) {
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes, 1, length, file);
        std::fclose(file);
    }
}

// Whether load_token() comes back at all, in a child with an alarm on it, so
// that "it blocked" and "it read for ever" are results rather than a hung
// test. Returns what the child read, or "<hung>".
std::string load_within(int seconds) {
    int channel[2] = {-1, -1};
    if (::pipe(channel) != 0) {
        return "<no pipe>";
    }
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(channel[0]);
        ::alarm(static_cast<unsigned>(seconds));
        const std::string token = vocem::load_token();
        (void)!::write(channel[1], token.c_str(), token.size());
        ::close(channel[1]);
        ::_exit(0);
    }
    ::close(channel[1]);
    std::string answer;
    char buffer[512];
    ssize_t got = 0;
    while ((got = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
        answer.append(buffer, static_cast<size_t>(got));
    }
    ::close(channel[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (!WIFEXITED(status)) {
        return "<hung>";
    }
    return answer;
}

}  // namespace

int main() {
    char root[] = "/tmp/vocem-token-XXXXXX";
    if (!mkdtemp(root)) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("XDG_STATE_HOME", root, 1);
    const std::string path = vocem::token_path();
    check(path.rfind(root, 0) == 0, "the token lives under XDG_STATE_HOME");

    // The ordinary life: stored, read back, 0600, no temporary left behind.
    check(vocem::save_token("abc.DEF-123_456"), "a token is stored");
    struct stat info {};
    check(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "with mode 0600 from the moment it exists");
    check(::access((path + ".part").c_str(), F_OK) != 0, "and no temporary beside it");
    check(vocem::load_token() == "abc.DEF-123_456", "and read back whole");
    check(vocem::save_token("second"), "a second store replaces the first");
    check(vocem::load_token() == "second", "atomically: the new one is what is read");

    // A file that is not a token.
    write_bytes(path, "abc\xFF" "def\n", 8);
    check(load_within(5).empty(),
          "a byte that is not UTF-8 makes the file no token: refused, not handed to dump()");
    write_bytes(path, "spaced token\n", 13);
    check(load_within(5).empty(), "and so does a space");
    write_bytes(path, "\n", 1);
    check(load_within(5).empty(), "an empty line is no token");
    std::string huge(8192, 'a');
    write_bytes(path, huge.c_str(), huge.size());
    check(load_within(5).empty(), "nor is eight kilobytes of one");

    // Entry 47's shapes, at this path.
    ::unlink(path.c_str());
    check(::symlink("/dev/zero", path.c_str()) == 0, "a link to /dev/zero can be planted");
    const std::string zero = load_within(5);
    check(zero.empty() && zero != "<hung>",
          "and is refused at once rather than read until memory runs out");
    ::unlink(path.c_str());
    check(::mkfifo(path.c_str(), 0600) == 0, "a FIFO can be planted");
    const std::string fifo = load_within(5);
    check(fifo != "<hung>", "and does not hold the daemon at its first line");
    check(fifo.empty(), "nor is it a token");
    ::unlink(path.c_str());

    // A link at the temporary's name must not steer the write.
    const std::string elsewhere = std::string(root) + "/elsewhere";
    check(::symlink(elsewhere.c_str(), (path + ".part").c_str()) == 0,
          "a link can be planted where the temporary goes");
    check(vocem::save_token("after-the-link"), "the store still succeeds");
    check(::access(elsewhere.c_str(), F_OK) != 0, "and nothing was written through the link");
    check(vocem::load_token() == "after-the-link", "the token is where it belongs");

    vocem::forget_token();
    check(vocem::load_token().empty(), "a forgotten token is gone");

    char cleanup[600];
    std::snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    (void)!system(cleanup);
    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
