// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the peer says reaches the daemon's log as text, one line of it.
//
// sanitise_text() is the rule for anything from Discord that is shown, logged
// or published (daemon/src/text.h, entry 133), and the refusal of an RPC
// command nobody was waiting on logged the peer's own `cmd` string without it:
// `LOG("rpc refused %s: %s", command, data.dump())`. dump() escapes the data;
// the command went in raw. A peer -- Discord, or whatever answered on the port
// -- choosing `cmd` wrote whole lines into the journal the Debug section shows,
// and terminal escapes into whatever displays it.
//
// Held with the real vocemd against the stub Discord, sandboxed like
// daemon_notification: after the session is up, one ERROR for a command whose
// name carries a newline, a forged log prefix and the one-character CSI.
//
// And the data beside it. The first fix left `data.dump()` on the same line
// raw, under a comment saying dump() "escapes every control character": it
// escapes those below U+0020 and nothing else, so the C1 controls (U+009B the
// CSI, U+0085 NEXT LINE) and U+2028 went into the log as UTF-8 (the second
// round's refutation, measured with exactly this message). The error's
// message carries all three now.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "discord_stub.h"
#include "unit_confinement.h"
#include "probe_alarm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

void write_file(const std::string& path, const std::string& body) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    fwrite(body.data(), 1, body.size(), file);
    fclose(file);
}

std::string read_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }
    fclose(file);
    return out;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_daemon_confinement(); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "the peer's words in the daemon's log");

    char root[] = "/tmp/vocem-log-lines-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    write_file(base + "/state/vocem/token", "test-token\n");
    write_file(base + "/config/vocem/config.ini", "");
    const std::string log = base + "/daemon.log";

    const int discord = vocem_test::listen_on(6463);
    if (discord < 0) {
        printf("FAIL cannot listen on loopback -- is the sandbox missing --unshare-net?\n");
        return 1;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 2);
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }
    const int fd = accept(discord, nullptr, nullptr);
    std::string buffer;
    const bool up =
        fd >= 0 && vocem_test::accept_upgrade(fd) &&
        vocem_test::bring_up_session(fd, buffer,
                                     R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,)"
                                     R"("nonce":"chan","data":null})",
                                     vocem_test::monotonic() + 10.0);
    check(up, "the session came up against the stub");

    // \n, a forged prefix, and U+009B (the one-character CSI) then "31m".
    vocem_test::send_text(fd, "{\"cmd\":\"NOPE\\n[vocemd] authorised FORGED\\u009b31m\","
                              "\"evt\":\"ERROR\",\"nonce\":null,"
                              "\"data\":{\"code\":4000,"
                              "\"message\":\"no\\u009b31mRED LS\\u2028NEL\\u0085\"}}");
    // The refusal is logged on receipt; give it the recv loop's second.
    for (int i = 0; i < 30 && read_file(log).find("rpc refused") == std::string::npos; ++i) {
        usleep(100 * 1000);
    }
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    if (fd >= 0) {
        close(fd);
    }
    close(discord);

    const std::string said = read_file(log);
    const size_t refused = said.find("rpc refused");
    const size_t end = refused == std::string::npos ? std::string::npos : said.find('\n', refused);
    printf("--  the daemon logged: %s\n",
           refused == std::string::npos ? "(no refusal)"
                                        : said.substr(refused, end - refused).c_str());
    check(refused != std::string::npos, "the refused command is logged");
    check(said.find("\n[vocemd] authorised FORGED") == std::string::npos,
          "and the peer's newline does not start a line of the daemon's log");
    check(said.find("\xC2\x9B") == std::string::npos,
          "and the one-character CSI does not reach the log");
    check(said.find("\xE2\x80\xA8") == std::string::npos &&
              said.find("\xC2\x85") == std::string::npos,
          "nor the line separator or NEXT LINE in the error's data");
    check(said.find("RED LS") != std::string::npos,
          "while the data's words are still logged");

    (void)!system(("rm -rf '" + base + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
