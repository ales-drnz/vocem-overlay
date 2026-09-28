// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A daemon that reopens a segment it did not unlink clears it under the
// seqlock, like any other write.
//
// StateWriter::open() reuses the object when the name is still there -- a
// daemon that crashed, or was killed, never reached its unlink -- and clears
// every field of it. It did that with the sequence untouched, and stored 0
// only at the end: a game already mapped to that object (it keeps reading
// the same inode) could start a copy before the clear, finish it during,
// see the same sequence before and after, and accept a snapshot that says
// twelve people are in the channel while their names and ids are already
// zeroes. Worse where the old sequence was 0 -- a daemon that died before
// its first publish -- because the final store changes nothing at all.
//
// Two measurements:
//   * deterministic, the protocol property: a reader's sequence taken before
//     open() and after it must differ, or no copy spanning the clear can be
//     told from a clean one. Fails before the fix on a segment whose old
//     sequence is 0.
//   * statistical, the tear itself: a reader thread copies beside a writer
//     that publishes, closes and reopens the same object in a loop, and
//     every snapshot it accepts must be either a whole generation or wholly
//     cleared.
// Private /dev/shm.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

constexpr uint32_t kUsers = 12;

void publish_generation(vocem::StateWriter& writer, uint32_t generation) {
    writer.publish([&](vocem::SharedState& s) {
        s.connected = 1;
        s.in_channel = 1;
        std::snprintf(s.channel_name, sizeof(s.channel_name), "generation %u", generation);
        for (uint32_t i = 0; i < kUsers; ++i) {
            s.users[i].id = static_cast<uint64_t>(generation) * 100 + i + 1;
            std::snprintf(s.users[i].name, sizeof(s.users[i].name), "name %u", generation);
        }
        s.user_count = kUsers;
    });
}

// A whole generation, or the state open() leaves: nothing else is a state
// the daemon ever published.
bool legitimate(const vocem::Snapshot& s) {
    if (s.user_count == 0 && !s.connected && !s.in_channel && s.channel_name[0] == '\0') {
        for (uint32_t i = 0; i < vocem::kMaxUsers; ++i) {
            if (s.users[i].id != 0 || s.users[i].name[0] != '\0') {
                return false;
            }
        }
        return true;
    }
    unsigned generation = 0;
    if (std::sscanf(s.channel_name, "generation %u", &generation) != 1 || s.user_count != kUsers) {
        return false;
    }
    char name[vocem::kNameCapacity];
    std::snprintf(name, sizeof(name), "name %u", generation);
    for (uint32_t i = 0; i < kUsers; ++i) {
        if (s.users[i].id != static_cast<uint64_t>(generation) * 100 + i + 1 ||
            std::strcmp(s.users[i].name, name) != 0) {
            return false;
        }
    }
    return true;
}

// The sequence word, read the way a reader reads it.
uint32_t sequence_of(const char* name) {
    const int fd = shm_open(name, O_RDONLY, 0);
    uint32_t value = 0xffffffffu;
    if (fd >= 0) {
        if (pread(fd, &value, sizeof(value),
                  static_cast<off_t>(offsetof(vocem::SharedState, sequence))) !=
            static_cast<ssize_t>(sizeof(value))) {
            value = 0xffffffffu;
        }
        close(fd);
    }
    return value;
}

}  // namespace

int main() {
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "reopening a segment beside a reader");
    char name[64];
    vocem::shm_name(name, sizeof(name), getuid());

    // Deterministic: a daemon that died before its first publish leaves its
    // segment at sequence 0, and the next daemon reopens and clears it.
    {
        vocem::StateWriter first;
        check(first.open(), "a first daemon's segment opens");
        first.close();  // killed: no unlink
        const uint32_t before = sequence_of(name);
        vocem::StateWriter second;
        check(second.open(), "the next daemon reopens the same object");
        const uint32_t after = sequence_of(name);
        std::printf("     sequence %u before the reopening clear, %u after\n", before, after);
        check(before != after,
              "a reader's before and after differ across the clear, so a copy spanning it is retried");
        check((after & 1u) == 0, "and the segment is left stable (even)");
        second.close();
    }

    // Statistical: the tear itself.
    std::atomic<bool> stop{false};
    std::atomic<long> accepted{0};
    std::atomic<long> torn{0};
    vocem::StateReader reader;
    {
        vocem::StateWriter setup;
        setup.open();
        publish_generation(setup, 1);
        setup.close();
    }
    check(reader.open(), "a game maps the segment");
    std::thread game([&] {
        vocem::Snapshot snapshot;
        while (!stop.load(std::memory_order_relaxed)) {
            if (reader.read_state(snapshot) == vocem::StateReader::Read::Ok) {
                accepted.fetch_add(1, std::memory_order_relaxed);
                if (!legitimate(snapshot)) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });
    long reopenings = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint32_t generation = 2;
    while (std::chrono::steady_clock::now() < end) {
        vocem::StateWriter writer;
        if (!writer.open()) {
            break;
        }
        ++reopenings;
        publish_generation(writer, generation++);
        writer.close();  // and the next iteration reopens the same object
    }
    stop = true;
    game.join();
    std::printf("     %ld reopenings, %ld snapshots accepted, %ld of them torn\n", reopenings,
                accepted.load(), torn.load());
    check(reopenings > 1000 && accepted.load() > 1000, "the race was actually run");
    check(torn.load() == 0, "no accepted snapshot is half cleared");

    reader.close();
    vocem::StateWriter::unlink_segment();
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
