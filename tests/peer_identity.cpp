// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Who the daemon believes is on the other end of its RPC connection.
//
// Before it sends AUTHENTICATE with a token whose scopes include messages.read,
// the daemon asks the kernel who owns the socket at the far end and refuses a
// peer it can identify as somebody else. That check is only worth having if it is
// right about three things, and this measures each of them:
//
//   1. **the real thing.** A genuine loopback connection to a listener this
//      process owns must come back Found, with this process's uid, read out of
//      the real /proc/net/tcp. A parser that answers Unknown here would make the
//      daemon fall back to "cannot tell" on every connection, which is a check
//      that quietly does not happen.
//   2. **a dual-stack listener.** A server socket bound to :: accepts an IPv4
//      connection into a socket the kernel lists in /proc/net/tcp6, as
//      ::ffff:127.0.0.1. Looking only at /proc/net/tcp answers Unknown for a
//      perfectly ordinary Discord client -- the first version of this code did
//      exactly that, and this case is why there is a second file to look in.
//   3. **somebody else.** A row owned by another uid must come back Found with
//      that uid, which is the one case where the daemon refuses; and no matching
//      row at all must be Unknown rather than a wrong answer, because Unknown is
//      not treated as hostile.
//
// The fixtures are /proc/net files written by hand, which is the only way to
// arrange a foreign uid without a second user.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "peer_cgroup.h"
#include "peer_identity.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

std::string write_fixture(const char* directory, const char* name, const std::string& body) {
    std::string path = std::string(directory) + "/" + name;
    if (std::FILE* file = std::fopen(path.c_str(), "w")) {
        std::fputs("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid\n",
                   file);
        std::fputs(body.c_str(), file);
        std::fclose(file);
    }
    return path;
}

}  // namespace

int main() {
    char root[] = "/tmp/vocem-peer-XXXXXX";
    if (!mkdtemp(root)) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }

    // ---- 1. a real connection, read out of the real /proc.
    {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(listener, 1);
        socklen_t length = sizeof(addr);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length);

        const int client = ::socket(AF_INET, SOCK_STREAM, 0);
        const bool connected =
            ::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        const int served = connected ? ::accept(listener, nullptr, nullptr) : -1;

        sockaddr_in ours{};
        sockaddr_in theirs{};
        length = sizeof(ours);
        ::getsockname(client, reinterpret_cast<sockaddr*>(&ours), &length);
        length = sizeof(theirs);
        ::getpeername(client, reinterpret_cast<sockaddr*>(&theirs), &length);

        const vocem::PeerIdentity identity =
            vocem::socket_owner(theirs.sin_addr.s_addr, ntohs(theirs.sin_port),
                                ours.sin_addr.s_addr, ntohs(ours.sin_port));
        check(connected, "a loopback connection to a listener of our own");
        check(identity.outcome == vocem::PeerOwner::Found,
              "and the kernel names its owner rather than leaving it unknown");
        check(identity.uid == static_cast<long>(getuid()),
              "which is this process, since this process is the listener");

        // The socket's cgroup, asked of the kernel (peer_cgroup.h): the
        // accepted end was cloned from our listener, so it is ours.
        const vocem::PeerCgroup cgroup =
            vocem::socket_cgroup(theirs.sin_addr.s_addr, ntohs(theirs.sin_port),
                                 ours.sin_addr.s_addr, ntohs(ours.sin_port));
        std::string own;
        if (std::FILE* file = std::fopen("/proc/self/cgroup", "r")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), file)) {
                if (std::strncmp(line, "0::", 3) == 0) {
                    own.assign(line + 3, std::strcspn(line + 3, "\n"));
                }
            }
            std::fclose(file);
        }
        std::printf("--  the peer's cgroup: %s (%s)\n", cgroup.path.c_str(),
                    cgroup.failed ? cgroup.failed : "answered");
        check(cgroup.known && !own.empty() && cgroup.path == own,
              "sock_diag names the peer socket's cgroup, and it is this process's own");

        if (served >= 0) ::close(served);
        ::close(client);
        ::close(listener);
    }

    // ---- 2. a dual-stack listener: the row lives in /proc/net/tcp6.
    {
        const std::string empty = write_fixture(root, "tcp-empty", "");
        // 127.0.0.1:6463 <- 127.0.0.1:54321, as an IPv4-mapped v6 pair.
        const std::string six = write_fixture(
            root, "tcp6-mapped",
            "   0: 0000000000000000FFFF00000100007F:193F "
            "0000000000000000FFFF00000100007F:D431 01 00000000:00000000 00:00000000 00000000  "
            "1000        0 12345 1 0000000000000000 20 0 0 10 -1\n");
        const vocem::PeerIdentity identity = vocem::socket_owner(
            htonl(INADDR_LOOPBACK), 0x193F, htonl(INADDR_LOOPBACK), 0xD431, empty.c_str(),
            six.c_str());
        check(identity.outcome == vocem::PeerOwner::Found,
              "a peer accepted by a dual-stack listener is found in /proc/net/tcp6");
        check(identity.uid == 1000, "with the uid that row carries");
    }

    // ---- 3. somebody else, and nobody at all.
    {
        const std::string foreign = write_fixture(
            root, "tcp-foreign",
            "   0: 0100007F:193F 0100007F:D431 01 00000000:00000000 00:00000000 00000000  "
            "4242        0 99999 1 0000000000000000 20 0 0 10 -1\n");
        const vocem::PeerIdentity identity =
            vocem::socket_owner(htonl(INADDR_LOOPBACK), 0x193F, htonl(INADDR_LOOPBACK), 0xD431,
                                foreign.c_str(), nullptr);
        check(identity.outcome == vocem::PeerOwner::Found && identity.uid == 4242,
              "a listener owned by another user is named, which is what the daemon refuses on");

        const vocem::PeerIdentity absent =
            vocem::socket_owner(htonl(INADDR_LOOPBACK), 0x0001, htonl(INADDR_LOOPBACK), 0x0002,
                                foreign.c_str(), nullptr);
        check(absent.outcome == vocem::PeerOwner::Unknown,
              "no matching row is unknown, not a wrong uid");

        const vocem::PeerIdentity unreadable = vocem::socket_owner(
            htonl(INADDR_LOOPBACK), 0x193F, htonl(INADDR_LOOPBACK), 0xD431,
            "/nonexistent/proc/net/tcp", "/nonexistent/proc/net/tcp6");
        check(unreadable.outcome == vocem::PeerOwner::Unknown,
              "and /proc that cannot be read is unknown, which the daemon does not treat as "
              "hostile");
    }

    // ---- the scope a Flatpak starts every sandbox in, read off a path.
    {
        const char* base = "/user.slice/user-1000.slice/user@1000.service/app.slice/";
        check(vocem::flatpak_of_cgroup(std::string(base) +
                                       "app-flatpak-com.rtosta.zapzap-1403059217.scope") ==
                  "com.rtosta.zapzap",
              "app-flatpak-<id>-<n>.scope names the id (the shape measured with ZapZap)");
        check(vocem::flatpak_of_cgroup(std::string(base) +
                                       "app-flatpak-com.discordapp.Discord-12.scope") ==
                  "com.discordapp.Discord",
              "Discord's own Flatpak scope names Discord's id");
        check(vocem::flatpak_of_cgroup(std::string(base) + "app-discord-123224.scope").empty(),
              "the host Discord's scope (measured live) is not a Flatpak");
        check(vocem::flatpak_of_cgroup(std::string(base) + "app-flatpak-org.x.Y.scope").empty() &&
                  vocem::flatpak_of_cgroup(std::string(base) + "app-flatpak-org.x.Y-12.service")
                      .empty() &&
                  vocem::flatpak_of_cgroup("").empty(),
              "a scope without its number, a service, and nothing at all are not Flatpaks");
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
