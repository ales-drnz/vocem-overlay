// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// An object at the daemon's names that another user could have made or can
// write is refused by everybody who opens it.
//
// /dev/shm is writable by every local user and the names are predictable
// (`/vocem-<uid>`, `/vocem-note-<uid>`). The writers opened them O_CREAT with
// no look at what they found, and the readers opened them with no look either,
// so another user who made the name first chose the channel every game drew,
// and could read what the daemon wrote into it. vocem/shm.h's
// segment_trust_problem() is the one rule: owner is this uid, a regular file,
// and no bits for group or others -- the daemon creates both objects 0600.
//
// A second uid is not available to a test, so the rule is held two ways: as a
// function over `struct stat` (every case, another owner included), and on
// real objects in a private /dev/shm made with a mode the daemon never uses
// (0666 and 0640), which every opener must refuse -- and then accept once the
// same object is 0600.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem/note.h"
#include "vocem/shm.h"
#include "vocem/state_poll.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

struct stat regular(uid_t owner, mode_t permissions) {
    struct stat info {};
    info.st_uid = owner;
    info.st_mode = S_IFREG | permissions;
    return info;
}

// A segment made the way a squatter would: the right name, a full size, a
// valid-looking state, and a mode of the squatter's choosing.
void plant(const char* name, size_t size, mode_t mode) {
    shm_unlink(name);
    const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0 || fchmod(fd, mode) != 0) {
        std::printf("FAIL cannot plant %s\n", name);
        _exit(1);
    }
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped != MAP_FAILED) {
        if (size == sizeof(vocem::SharedState)) {
            auto* state = static_cast<vocem::SharedState*>(mapped);
            state->abi_version = vocem::kAbiVersion;
            state->connected = 1;
            state->in_channel = 1;
            state->user_count = 1;
            std::snprintf(state->channel_name, sizeof(state->channel_name), "planted");
        } else {
            auto* note = static_cast<vocem::NoteShared*>(mapped);
            note->abi_version = vocem::kNoteAbiVersion;
            note->serial = 42;
            std::snprintf(note->body, sizeof(note->body), "planted words");
        }
        munmap(mapped, size);
    }
    close(fd);
}

char g_said[256];
void capture(const char* line) {
    std::snprintf(g_said, sizeof(g_said), "%s", line);
    std::printf("     log: %s\n", line);
}

}  // namespace

int main() {
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(30, "opening planted segments");

    // The rule itself, every case, another owner included.
    const uid_t me = getuid();
    const uid_t other = me + 1;
    check(!vocem::segment_trust_problem(regular(me, 0600), me), "0600, mine: trusted");
    check(!vocem::segment_trust_problem(regular(me, 0400), me), "0400, mine: trusted");
    check(vocem::segment_trust_problem(regular(other, 0600), me) != nullptr,
          "0600, another user's: refused");
    check(vocem::segment_trust_problem(regular(other, 0666), me) != nullptr,
          "0666, another user's: refused");
    check(vocem::segment_trust_problem(regular(me, 0666), me) != nullptr,
          "0666, mine: refused (others can write it)");
    check(vocem::segment_trust_problem(regular(me, 0620), me) != nullptr,
          "0620, mine: refused (the group can write it)");
    check(vocem::segment_trust_problem(regular(me, 0640), me) != nullptr,
          "0640, mine: refused (the daemon never makes it readable to others)");
    struct stat fifo = regular(me, 0600);
    fifo.st_mode = S_IFIFO | 0600;
    check(vocem::segment_trust_problem(fifo, me) != nullptr, "a FIFO: refused");

    char state_name[64];
    vocem::shm_name(state_name, sizeof(state_name), me);
    char note_name[64];
    vocem::note_shm_name(note_name, sizeof(note_name), me);

    for (const mode_t mode : {mode_t(0666), mode_t(0640)}) {
        std::printf("     -- planted at %04o\n", static_cast<unsigned>(mode));
        plant(state_name, sizeof(vocem::SharedState), mode);
        {
            vocem::StateReader reader;
            check(!reader.open(), "a reader refuses the planted state segment");
            check(reader.refusal() != nullptr, "and can say why");
        }
        check(vocem::peek_abi_version() == 0, "the version peek does not report it as the daemon's");
        {
            g_said[0] = '\0';
            vocem::StatePoll poll(&capture);
            check(poll.poll() == nullptr, "the poll draws nothing from it");
            check(std::strstr(g_said, "refusing") != nullptr, "and says it refused");
        }
        {
            vocem::StateWriter writer;
            check(!writer.open(), "the daemon's writer refuses to publish into it");
        }

        plant(note_name, sizeof(vocem::NoteShared), mode);
        {
            vocem::NoteReader reader;
            const char* words = reader.body_for(42);
            check(words[0] == '\0', "a game takes no words from the planted note");
            check(reader.refusal() != nullptr, "and can say why");
        }
        {
            vocem::NoteWriter writer;
            writer.publish(43, "the user's message");
            const int fd = shm_open(note_name, O_RDONLY, 0);
            bool untouched = false;
            if (fd >= 0) {
                void* mapped = mmap(nullptr, sizeof(vocem::NoteShared), PROT_READ, MAP_SHARED, fd, 0);
                if (mapped != MAP_FAILED) {
                    untouched = static_cast<const vocem::NoteShared*>(mapped)->serial == 42;
                    munmap(mapped, sizeof(vocem::NoteShared));
                }
                close(fd);
            }
            check(untouched, "the daemon's note writer does not write a message into it");
            // ~NoteWriter clears and unlinks: plant again below.
        }
    }

    // The same objects at 0600 are the daemon's own shape, and are read.
    plant(state_name, sizeof(vocem::SharedState), 0600);
    {
        vocem::StateReader reader;
        vocem::Snapshot snapshot;
        check(reader.open() && reader.read(snapshot) &&
                  std::strcmp(snapshot.channel_name, "planted") == 0,
              "at 0600 and this uid the state segment is read");
    }
    plant(note_name, sizeof(vocem::NoteShared), 0600);
    {
        vocem::NoteReader reader;
        check(std::strcmp(reader.body_for(42), "planted words") == 0,
              "at 0600 and this uid the note is read");
    }
    shm_unlink(state_name);
    shm_unlink(note_name);
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
