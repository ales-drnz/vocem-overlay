// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A segment shorter than the struct is not a segment: what the reader inside a
// game does when it finds one.
//
// mmap past the end of a shared memory object succeeds, and touching a page that
// lies beyond it raises SIGBUS. The writer creates the name with shm_open and
// gives it its size with ftruncate afterwards, so between those two calls
// /vocem-<uid> exists at zero bytes -- and a daemon killed in that window, or one
// whose ftruncate failed on a full /dev/shm, used to leave the name behind. Every
// OpenGL and Vulkan process in the session then died on its first read, all at
// once, and stayed dying until something recreated or removed the object.
//
// Measured here rather than reasoned about: the read runs in a forked child, so a
// SIGBUS is a test result and not a dead test. The same case for the note segment,
// which is unlinked and recreated for every message and therefore opens that
// window once per notification.
//
// Runs in a private /dev/shm (tests/private_shm.h re-execs under bwrap): it
// creates and destroys the very names the running daemon publishes under.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <initializer_list>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem/note.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// Creates the named object at exactly `bytes` bytes.
bool place_stub(const char* name, off_t bytes) {
    ::shm_unlink(name);
    const int fd = ::shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return false;
    }
    const bool sized = ::ftruncate(fd, bytes) == 0;
    ::close(fd);
    return sized;
}

// Runs `body` in a child. Returns 0 when it exited cleanly, -signal when a signal
// killed it, or 1 for a non-zero exit: a SIGBUS has to be observable rather than
// fatal to the run.
template <typename Body>
int in_child(Body body) {
    const pid_t child = ::fork();
    if (child == 0) {
        ::_exit(body() ? 0 : 2);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    if (WIFSIGNALED(status)) {
        return -WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

const char* outcome(int status) {
    static char text[64];
    if (status < 0) {
        std::snprintf(text, sizeof(text), "killed by signal %d", -status);
    } else {
        std::snprintf(text, sizeof(text), "exited %d", status);
    }
    return text;
}

}  // namespace

int main() {
    // Never against the real daemon's segment: this test creates and destroys the
    // very names it publishes under. Re-exec under bwrap, as shm_reattach does;
    // the sentinel is set by this code and never by hand (entry 53).
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    // Belt and braces: the sentinel says sandboxed, this asks /proc.
    {
        char holds[256] = {0};
        if (!vocem_test::shm_is_private(holds, sizeof(holds))) {
            vocem_test::shm_explain_refusal(holds);
            return 1;
        }
    }
    vocem_test::set_alarm(60, "a segment shorter than its struct");

    char state_name[64];
    vocem::shm_name(state_name, sizeof(state_name), getuid());
    char note_name[64];
    vocem::note_shm_name(note_name, sizeof(note_name), getuid());

    // ---- the state segment, at every length short of the struct.
    for (const off_t bytes : {off_t{0}, off_t{1}, off_t{100},
                              static_cast<off_t>(sizeof(vocem::SharedState)) - 1}) {
        if (!place_stub(state_name, bytes)) {
            std::printf("FAIL could not create a %lld-byte stub segment\n",
                        static_cast<long long>(bytes));
            ++failures;
            continue;
        }
        const int status = in_child([] {
            vocem::StateReader reader;
            if (!reader.open()) {
                return true;  // refused before mapping: the whole point
            }
            vocem::Snapshot snapshot{};
            reader.read(snapshot);  // the touch that used to raise SIGBUS
            return false;           // opened a segment that cannot hold the struct
        });
        char what[128];
        std::snprintf(what, sizeof(what),
                      "a %lld-byte state segment is refused, not mapped (%s)",
                      static_cast<long long>(bytes), outcome(status));
        check(status == 0, what);
    }
    ::shm_unlink(state_name);

    // ---- the note segment, same shape.
    for (const off_t bytes : {off_t{0}, off_t{16}}) {
        if (!place_stub(note_name, bytes)) {
            std::printf("FAIL could not create a %lld-byte stub note\n",
                        static_cast<long long>(bytes));
            ++failures;
            continue;
        }
        const int status = in_child([] {
            vocem::NoteReader reader;
            const char* body = reader.body_for(1);
            return body[0] == '\0';
        });
        char what[128];
        std::snprintf(what, sizeof(what),
                      "a %lld-byte note segment draws nothing rather than killing the game (%s)",
                      static_cast<long long>(bytes), outcome(status));
        check(status == 0, what);
    }
    ::shm_unlink(note_name);

    // ---- and a full-length one still works, so the guard did not close the door.
    {
        vocem::StateWriter writer;
        check(writer.open(), "a real segment still opens");
        writer.publish([](vocem::SharedState& state) {
            state.connected = 1;
            state.user_count = 1;
            std::snprintf(state.channel_name, sizeof(state.channel_name), "Channel");
        });
        vocem::StateReader reader;
        vocem::Snapshot snapshot{};
        check(reader.open() && reader.read(snapshot) && snapshot.connected,
              "and reads back what the writer published");
        reader.close();
        writer.close();
        vocem::StateWriter::unlink_segment();
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
