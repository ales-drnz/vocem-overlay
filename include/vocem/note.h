// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The text of a message, and the only piece of this project that is deliberately
// hard to reach.
//
// The overlay draws the message in the game's own frame, which means the game's
// process holds those words at the moment they are drawn. That much is not
// negotiable and is written down rather than glossed: **a game the user allows
// the overlay into can read the message the overlay draws there.** What IS
// negotiable -- and what this file exists for -- is everything around that
// moment, because the first design put the text in the shared state segment,
// and that segment is mapped by *every* Vulkan and OpenGL process on the
// machine, for the whole session, whether or not it ever draws anything. The
// text of every message the user received sat in the address space of the
// browser, the compositor, the launcher and every game that had merely started.
// The answer then was a switch defaulting to off, which meant the feature was
// off; the owner's answer now is that the message must always be shown, and
// that a game must never have that text lying around.
//
// So the words live here instead, in a segment of their own:
//
//   * it exists only while a toast is on screen. The daemon creates it when a
//     message arrives and unlinks it once the toast has outlived its seconds,
//     so between messages there is nothing to read anywhere;
//   * nothing maps it as a matter of course. The injected code opens it only
//     when it has already decided to draw that toast in that process -- a game
//     the user excluded, and every non-game, never touch it;
//   * a reader takes its copy and closes immediately: three syscalls per
//     message, none per frame, and no mapping left behind;
//   * the copy is wiped when the toast ends, so the words are in the game's
//     memory for the seconds they are on its screen and not a minute longer.
//
// What that buys, exactly: the exposure goes from "every graphical process,
// always" to "the process that is drawing it, while it is drawing it". What it
// does not buy is secrecy from the game being played, which no design can
// while the drawing happens inside it.
//
// Both halves live in this one file so the format cannot drift (entry 33), and
// the reader is written to the injected code's rules: no allocation, no parser,
// a size the reader fixes rather than trusts, and a seqlock rather than a lock.

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

#include "vocem/shared_state.h"

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
};

// ---------------------------------------------------------------------------
// Reader -- the injected code, and only while it draws.
// ---------------------------------------------------------------------------

// Holds at most one message's text, for as long as that message is on screen
// in this process. Not a cache in the usual sense: forgetting is the point.
class NoteReader {
public:
    // The text for this toast, or an empty string. Opens the segment at most
    // once per message -- a repeated frame costs nothing at all, which is the
    // property the present path needs -- and closes it before returning, so a
    // process holds no mapping of the words it is not currently drawing.
    const char* body_for(uint64_t serial) {
        if (serial == 0) {
            forget();
            return body_;
        }
        if (serial == have_) {
            return body_;
        }
        have_ = serial;
        std::memset(body_, 0, sizeof(body_));

        char name[64];
        note_shm_name(name, sizeof(name), getuid());
        const int fd = shm_open(name, O_RDONLY, 0);
        if (fd < 0) {
            return body_;  // nothing published: the toast is a name alone
        }
        // The same guard the state reader has: an object shorter than the struct
        // maps fine and raises SIGBUS on the first touch, and this name is
        // unlinked and recreated for every message, so the window in which it
        // exists at zero bytes opens once per notification.
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

    ~NoteReader() { forget(); }

private:
    uint64_t have_ = 0;
    char body_[kNotificationBodyCapacity] = {0};
};

}  // namespace vocem

#endif  // VOCEM_NOTE_H
