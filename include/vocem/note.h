// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The text of a message, and the only piece of this project that is deliberately
// hard to reach.
//
// The overlay draws the message in the game's own frame, so **a game the user
// allows the overlay into can read the message the overlay draws there**;
// nothing prevents that. What this file bounds is everything around that moment:
// the state segment is mapped by every Vulkan and OpenGL process in the session,
// so the words must not live there. They live in a segment of their own:
//
//   * it exists only while a toast is on screen: the daemon creates it when a
//     message arrives and unlinks it when the toast has outlived its seconds;
//   * nothing maps it as a matter of course: the injected code opens it only
//     once it has decided to draw that toast in that process, so an excluded
//     game or a non-game never touches it;
//   * a reader copies and closes at once: a few syscalls per message, none per
//     frame, no mapping left behind;
//   * the copy is wiped when the toast ends.
//
// The exposure is "the process drawing it, while it draws it", not "every
// graphical process, always". Both halves live here so the format cannot drift;
// the reader follows the injected code's rules: no allocation, no parser, a size
// it fixes rather than trusts, and a seqlock rather than a lock.

#ifndef VOCEM_NOTE_H
#define VOCEM_NOTE_H

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "vocem/flatpak.h"
#include "vocem/shared_state.h"
#include "vocem/shm.h"

namespace vocem {

// Bumped on any layout change here, like the state segment's own version: a
// reader that meets a version it does not know draws no text rather than
// guessing at bytes.
constexpr uint32_t kNoteAbiVersion = 1;

struct NoteShared {
    uint32_t abi_version;
    uint32_t _reserved;  // explicit, so both widths agree (entry 55)
    std::atomic<uint32_t> sequence;
    uint32_t _reserved2;
    uint64_t serial;  // matches Notification::serial in the state segment
    char body[kNotificationBodyCapacity];
};

static_assert(sizeof(NoteShared) == 16 + 8 + kNotificationBodyCapacity,
              "the note's layout must be the same at both widths");

inline void note_shm_name(char* out, size_t capacity, unsigned int uid) {
    std::snprintf(out, capacity, "/vocem-note-%u", uid);
}

// ---------------------------------------------------------------------------
// Writer -- vocemd only.
// ---------------------------------------------------------------------------

class NoteWriter {
public:
    // Called after every publish and every clear, with the words or nullptr:
    // the daemon's Flatpak mirrors hang here, so there is one publish path. A
    // plain function pointer, because this header is compiled into games and
    // nothing in it may allocate.
    void (*on_publish)(uint64_t serial, const char* body, void*) = nullptr;
    void* on_publish_context = nullptr;

    // Publishes the text of one message, creating the segment if it is not
    // there. Called when a notification arrives, never on a timer.
    void publish(uint64_t serial, const char* body) {
        if (!open()) {
            return;
        }
        uint32_t seq = note_->sequence.load(std::memory_order_relaxed);
        note_->sequence.store(seq + 1, std::memory_order_release);  // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        note_->abi_version = kNoteAbiVersion;
        note_->serial = serial;
        copy_string(note_->body, kNotificationBodyCapacity, body, body ? std::strlen(body) : 0);
        std::atomic_thread_fence(std::memory_order_release);
        note_->sequence.store(seq + 2, std::memory_order_release);  // even: stable

        if (on_publish) {
            on_publish(serial, body, on_publish_context);
        }
    }

    // The toast has outlived its seconds: the words go away. Both halves --
    // scrubbing what is mapped, so a reader that already opened it sees
    // nothing, and unlinking the name, so nothing new can open it at all.
    void clear() {
        if (note_) {
            uint32_t seq = note_->sequence.load(std::memory_order_relaxed);
            note_->sequence.store(seq + 1, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_release);
            note_->serial = 0;
            std::memset(note_->body, 0, sizeof(note_->body));
            std::atomic_thread_fence(std::memory_order_release);
            note_->sequence.store(seq + 2, std::memory_order_release);
        }
        close();
        unlink_note();
        if (on_publish) {
            on_publish(0, nullptr, on_publish_context);
        }
    }

    void close() {
        if (note_) {
            munmap(note_, sizeof(NoteShared));
            note_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    static void unlink_note() {
        char name[64];
        note_shm_name(name, sizeof(name), getuid());
        shm_unlink(name);
    }

    ~NoteWriter() { clear(); }

private:
    bool open() {
        if (note_) {
            return true;
        }
        char name[64];
        note_shm_name(name, sizeof(name), getuid());
        fd_ = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            return false;
        }
        // The words of a message are the last thing to write into an object
        // another user made at our name (segment_trust_problem, shm.h).
        // Refused and said, once per object met, and left where it is.
        if (const char* problem = descriptor_trust_problem(fd_)) {
            if (refusal_ != problem) {
                std::fprintf(stderr, "vocemd: refusing the note segment %s: %s\n", name,
                             problem);
            }
            refusal_ = problem;
            close();
            return false;
        }
        refusal_ = nullptr;
        // As in shm.h: a name created and not sized is a name every reader maps
        // and dies touching, so a failed creation unlinks it.
        if (ftruncate(fd_, sizeof(NoteShared)) != 0) {
            close();
            shm_unlink(name);
            return false;
        }
        void* mapped =
            mmap(nullptr, sizeof(NoteShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) {
            close();
            shm_unlink(name);
            return false;
        }
        note_ = static_cast<NoteShared*>(mapped);
        return true;
    }

    int fd_ = -1;
    NoteShared* note_ = nullptr;
    const char* refusal_ = nullptr;
};

// ---------------------------------------------------------------------------
// Reader -- the injected code, and only while it draws.
// ---------------------------------------------------------------------------

// Holds at most one message's text, for as long as that message is on screen
// in this process. Not a cache in the usual sense: forgetting is the point.
class NoteReader {
public:
    // The text for this toast, or an empty string. Opens the segment at most once
    // per message (a repeated frame costs nothing) and closes it before
    // returning, so no mapping of the words outlives the read.
    const char* body_for(uint64_t serial) {
        if (serial == 0) {
            forget();
            return body_;
        }
        if (serial == have_) {
            return body_;
        }
        have_ = serial;
        refusal_ = nullptr;
        std::memset(body_, 0, sizeof(body_));

        const int fd = open_note();
        if (fd < 0) {
            return body_;  // nothing published: the toast is a name alone
        }
        // Words from an object another user could have written are not the
        // message (segment_trust_problem, shm.h): the toast is a name alone,
        // and the caller can say why.
        if (const char* problem = descriptor_trust_problem(fd)) {
            refusal_ = problem;
            ::close(fd);
            return body_;
        }
        // An object shorter than the struct maps fine and raises SIGBUS on the
        // first touch; this name is recreated for every message, so the
        // zero-byte window opens once per notification.
        struct stat info {};
        if (fstat(fd, &info) != 0 || static_cast<size_t>(info.st_size) < sizeof(NoteShared)) {
            ::close(fd);
            return body_;
        }
        void* mapped = mmap(nullptr, sizeof(NoteShared), PROT_READ, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            ::close(fd);
            return body_;
        }
        const auto* note = static_cast<const NoteShared*>(mapped);
        // The same bounded seqlock read the state segment uses: a writer
        // caught mid-update must not spin a game's render thread forever.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t before = note->sequence.load(std::memory_order_acquire);
            if (before & 1u) {
                continue;
            }
            if (note->abi_version != kNoteAbiVersion || note->serial != serial) {
                break;  // a version we do not know, or a message we are not drawing
            }
            std::memcpy(body_, note->body, sizeof(body_));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (note->sequence.load(std::memory_order_acquire) == before) {
                break;
            }
            std::memset(body_, 0, sizeof(body_));
        }
        body_[sizeof(body_) - 1] = '\0';
        munmap(const_cast<NoteShared*>(note), sizeof(NoteShared));
        ::close(fd);
        return body_;
    }

    // The toast is gone: so are the words, out of this process's memory. The
    // caller does this the moment it stops drawing the message.
    void forget() {
        if (have_ != 0) {
            std::memset(body_, 0, sizeof(body_));
            have_ = 0;
        }
    }

    // Why the words of the current message were refused, or nullptr when
    // they were not (an empty body then means nothing was published).
    const char* refusal() const { return refusal_; }

    ~NoteReader() { forget(); }

private:
    // The words, by whichever of their two names this process can reach: inside
    // a Flatpak /dev/shm is a private tmpfs, so the reader opens the bridge copy,
    // which the daemon removes when it unlinks the segment.
    static int open_note() {
        char path[512];
        if (bridge_in_use() && bridge_path(path, sizeof(path), kBridgeNoteName)) {
            return ::open(path, O_RDONLY | O_CLOEXEC);
        }
        char name[64];
        note_shm_name(name, sizeof(name), getuid());
        return shm_open(name, O_RDONLY, 0);
    }

    uint64_t have_ = 0;
    const char* refusal_ = nullptr;
    char body_[kNotificationBodyCapacity] = {0};
};

}  // namespace vocem

#endif  // VOCEM_NOTE_H
