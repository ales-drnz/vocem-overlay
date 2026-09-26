// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A publish must not blank a game's frame, and a foreign ABI must be said even
// after a contention was.
//
// The reader used to give up after eight tries with no pause between them. A
// publish holds the sequence odd for a few microseconds -- eight loads take a
// few nanoseconds -- so any frame whose read landed inside a publish got
// nullptr from StatePoll::poll and drew nothing, and the snapshot it returned
// last time had been half overwritten by the failed copy. The review measured
// 7555 failed reads in 144.7 million at 20 publishes a second, which at 144 fps
// is a blank frame every three to seven minutes. And the one-shot log line
// said "abi mismatch or writer contention" on the first contention and never
// again, so a real ABI mismatch later in the process was silent (entry 55's
// silence, entry 127).
//
// Here a writer thread publishes a self-checking state -- every field carries
// the same generation number -- about ten thousand times a second, while
// StatePoll::poll runs a million times beside it in a private /dev/shm. Held:
//   * no poll after the first successful one returns nullptr;
//   * every snapshot it returns is whole (one generation throughout);
//   * a segment with a foreign ABI is refused, and SAID with its version, even
//     though contention came first; and said again after a reattachment.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem/shm.h"
#include "vocem/state_poll.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The poll's log, kept: the lines are the second half of the measurement.
char g_lines[64][256];
std::atomic<int> g_line_count{0};

void capture(const char* line) {
    const int at = g_line_count.load();
    if (at < 64) {
        std::snprintf(g_lines[at], sizeof(g_lines[at]), "%s", line);
        g_line_count.store(at + 1);
    }
    std::printf("     log: %s\n", line);
}

int lines_mentioning(const char* needle) {
    int found = 0;
    for (int i = 0; i < g_line_count.load() && i < 64; ++i) {
        if (std::strstr(g_lines[i], needle)) {
            ++found;
        }
    }
    return found;
}

constexpr uint32_t kUsers = 12;

void publish_generation(vocem::StateWriter& writer, uint32_t generation) {
    writer.publish([&](vocem::SharedState& s) {
        s.connected = 1;
        s.in_channel = 1;
        std::snprintf(s.channel_name, sizeof(s.channel_name), "generation %u", generation);
        for (uint32_t i = 0; i < kUsers; ++i) {
            vocem::User& u = s.users[i];
            u.id = static_cast<uint64_t>(generation) * 100 + i;
            std::snprintf(u.name, sizeof(u.name), "Somebody with a long name %u", generation);
            std::snprintf(u.avatar_hash, sizeof(u.avatar_hash),
                          "a_0123456789abcdef0123456789abcdef");
        }
        s.user_count = kUsers;
        s.display_height = generation;
    });
}

// One generation in every field, or the snapshot is a tear.
bool whole(const vocem::Snapshot& s) {
    unsigned generation = 0;
    if (std::sscanf(s.channel_name, "generation %u", &generation) != 1 ||
        s.user_count != kUsers || s.display_height != generation) {
        return false;
    }
    char name[vocem::kNameCapacity];
    std::snprintf(name, sizeof(name), "Somebody with a long name %u", generation);
    for (uint32_t i = 0; i < kUsers; ++i) {
        if (s.users[i].id != static_cast<uint64_t>(generation) * 100 + i ||
            std::strcmp(s.users[i].name, name) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(120, "polling beside a publishing writer");

    auto* writer = new vocem::StateWriter;
    if (!writer->open()) {
        std::printf("FAIL the private segment does not open\n");
        return 1;
    }
    publish_generation(*writer, 1);

    std::atomic<bool> stop{false};
    std::atomic<long> publishes{0};
    std::thread daemon([&] {
        uint32_t generation = 2;
        while (!stop.load(std::memory_order_relaxed)) {
            publish_generation(*writer, generation++);
            publishes.fetch_add(1, std::memory_order_relaxed);
            // About ten thousand publishes a second: five hundred times the
            // daemon's real rate, so a million polls meet thousands of them.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    vocem::StatePoll poll(&capture);
    const long kPolls = 1000000;
    long before_attach = 0;
    long blank_after_attach = 0;
    long torn = 0;
    bool attached = false;
    for (long i = 0; i < kPolls; ++i) {
        const vocem::Snapshot* snapshot = poll.poll();
        if (!snapshot) {
            if (attached) {
                ++blank_after_attach;
            } else {
                ++before_attach;
            }
            continue;
        }
        attached = true;
        if (!whole(*snapshot)) {
            ++torn;
        }
    }
    stop = true;
    daemon.join();
    std::printf("     %ld polls beside %ld publishes: %ld blank before the first read, "
                "%ld blank after it, %ld torn\n",
                kPolls, publishes.load(), before_attach, blank_after_attach, torn);
    check(attached, "the poll attached to the segment");
    check(blank_after_attach == 0, "no frame after the first read comes back blank");
    check(torn == 0, "every snapshot handed back is one generation throughout");

    // Now a foreign ABI, after the contention above had its chance to be the
    // one thing the log ever says.
    const uint32_t foreign = vocem::kAbiVersion + 7;
    char foreign_text[32];
    std::snprintf(foreign_text, sizeof(foreign_text), "version %u", foreign);
    writer->publish([&](vocem::SharedState& s) { s.abi_version = foreign; });
    bool refused = true;
    for (int i = 0; i < 100; ++i) {
        if (poll.poll()) {
            refused = false;
        }
    }
    check(refused, "a segment of a foreign ABI is refused, not served from the last good read");
    check(lines_mentioning(foreign_text) == 1,
          "the refusal is said, with the version it met, even after contention");

    // A daemon of the same foreign ABI comes back: the reattachment is a new
    // segment, and the refusal of it is news again.
    writer->close();
    vocem::StateWriter::unlink_segment();
    delete writer;
    writer = new vocem::StateWriter;
    check(writer->open(), "a second daemon's segment opens");
    writer->publish([&](vocem::SharedState& s) { s.abi_version = foreign; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (lines_mentioning(foreign_text) < 2 && std::chrono::steady_clock::now() < deadline) {
        poll.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(lines_mentioning(foreign_text) == 2,
          "the refusal is said again for the segment it reattached to");

    writer->close();
    vocem::StateWriter::unlink_segment();
    delete writer;
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
