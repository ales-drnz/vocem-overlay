// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Mapping helpers for the shared state segment. Header-only so the layer can
// include it without linking anything: the in-game side must stay free of
// dependencies.

#ifndef VOCEM_SHM_H
#define VOCEM_SHM_H

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>

#include "vocem/shared_state.h"

namespace vocem {

// ---------------------------------------------------------------------------
// Writer -- vocemd only.
// ---------------------------------------------------------------------------

class StateWriter {
public:
    bool open() {
        char name[64];
        shm_name(name, sizeof(name), getuid());
        fd_ = shm_open(name, O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            return false;
        }
        // A creation that fails takes the name with it. shm_open creates the
        // object at zero bytes and ftruncate gives it its size; leaving the name
        // behind after a failure here publishes a segment every reader in the
        // session would map and then die touching.
        if (ftruncate(fd_, sizeof(SharedState)) != 0) {
            close();
            shm_unlink(name);
            return false;
        }
        void* mapped =
            mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) {
            close();
            shm_unlink(name);
            return false;
        }
        state_ = static_cast<SharedState*>(mapped);

        // A fresh segment is zeroed; a reused one may hold stale data. Either
        // way, start from a defined state with a stable (even) sequence. Fields
        // are cleared individually because the struct holds an atomic and is not
        // trivially copyable as a whole.
        state_->abi_version = kAbiVersion;
        state_->connected = 0;
        state_->in_channel = 0;
        state_->user_count = 0;
        state_->status = static_cast<uint32_t>(DaemonStatus::WaitingForDiscord);
        state_->display_height = 0;
        std::memset(state_->channel_name, 0, sizeof(state_->channel_name));
        std::memset(state_->users, 0, sizeof(state_->users));
        std::memset(&state_->notification, 0, sizeof(state_->notification));
        state_->sequence.store(0, std::memory_order_release);
        return true;
    }

    void close() {
        if (state_) {
            munmap(state_, sizeof(SharedState));
            state_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Unlink so a stale segment cannot outlive the daemon and make the layer
    // believe a dead session is still live.
    static void unlink_segment() {
        char name[64];
        shm_name(name, sizeof(name), getuid());
        shm_unlink(name);
    }

    bool valid() const { return state_ != nullptr; }

    // The canonical segment, for the mirrors to be copied from. Read-only to
    // everything but publish().
    const SharedState* state() const { return state_; }

    // Called with the segment as it stands, after every publish. The daemon
    // hangs its Flatpak mirrors here (daemon/src/flatpak_bridge.h) so that a
    // sandboxed game is never served a staler state than a host one, and so that
    // there is one publish path rather than two to keep in step. A plain
    // function pointer: this header is compiled into games, and nothing in it
    // may allocate.
    void (*on_publish)(const SharedState&, void*) = nullptr;
    void* on_publish_context = nullptr;

    // Publish under the seqlock. The callback must not block: readers spin while
    // the sequence is odd.
    template <typename Fn>
    void publish(Fn&& mutate) {
        if (!state_) {
            return;
        }
        // Odd: writing. A read-modify-write with acquire semantics on this
        // side, not a release store: a release store lets the writes that
        // follow it be hoisted above it, and the data of a seqlock hoisted
        // above the odd count is a torn read no reader can detect. Harmless on
        // x86, where stores are not reordered with stores; written for the
        // model rather than the machine.
        const uint32_t seq = state_->sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);

        mutate(*state_);

        std::atomic_thread_fence(std::memory_order_release);
        state_->sequence.store(seq + 2, std::memory_order_release);  // even: stable

        // After the segment is stable, never during: a mirror copied from a
        // half-written source would carry the tear across the boundary with a
        // sequence that says it did not.
        if (on_publish) {
            on_publish(*state_, on_publish_context);
        }
    }

    ~StateWriter() { close(); }

private:
    int fd_ = -1;
    SharedState* state_ = nullptr;
};

// The segment's version field, read raw and without attaching. A reader that
// meets a segment from another ABI refuses it -- correctly -- but the refusal
// looks exactly like "no daemon" from outside, so the Debug section needs to
// SAY which version it met. Returns 0 when there is no segment at all.
inline uint32_t peek_abi_version() {
    char name[64];
    shm_name(name, sizeof(name), getuid());
    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    uint32_t version = 0;
    const ssize_t got = pread(fd, &version, sizeof(version),
                              static_cast<off_t>(offsetof(SharedState, abi_version)));
    ::close(fd);
    return got == static_cast<ssize_t>(sizeof(version)) ? version : 0;
}

// ---------------------------------------------------------------------------
// Reader -- the layer and the CLI.
// ---------------------------------------------------------------------------

class StateReader {
public:
    // Returns false when the daemon is not running, which is a normal condition:
    // the caller must then behave as if there were nothing to draw.
    bool open() {
        fd_ = open_segment();
        if (fd_ < 0) {
            return false;
        }
        // How long the object actually is, before mapping a page that may not be
        // backed. mmap past the end of a shared memory object succeeds and the
        // first touch raises SIGBUS -- and the writer creates the name with
        // shm_open and sizes it with ftruncate afterwards, so a daemon killed
        // between those two calls leaves a name at zero bytes. Measured: a
        // zero-length segment killed the reading process outright, which here
        // means every game in the session dying at once because a daemon died at
        // the wrong microsecond.
        struct stat info {};
        if (fstat(fd_, &info) != 0 || static_cast<size_t>(info.st_size) < sizeof(SharedState)) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        void* mapped = mmap(nullptr, sizeof(SharedState), PROT_READ, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        state_ = static_cast<const SharedState*>(mapped);
        return true;
    }

    void close() {
        if (state_) {
            munmap(const_cast<SharedState*>(state_), sizeof(SharedState));
            state_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool valid() const { return state_ != nullptr; }

    // What the name says about the object this mapping came from.
    //
    // Unlinking removes the name and not the pages, so a reader that mapped once
    // keeps reading its private copy of history: a daemon that stopped leaves it
    // a cleared state forever, and a daemon that started *again* creates a new
    // object this mapping will never see. Only a look at the name can tell --
    // one shm_open and two fstats, compared by inode -- so a game that outlives
    // a daemon restart can notice, drop the orphaned pages, and attach to the
    // living segment. Not free: callers keep it off the per-frame path and ask
    // on the same cadence as the reopen retries.
    //
    // Three answers, not two. The distinction was always computed here and
    // thrown away at the return, and the caller then had to treat a daemon that
    // had been REPLACED exactly as it treats one that is GONE -- so a
    // `systemctl --user restart vocemd`, which CLAUDE.md's own delivery rule
    // asks the owner to run, made every running game hand back its font atlas
    // and rasterise it again: 133 ms and a fresh 64 MB upload, per game, per
    // restart. Measured on tests/gl_daemon_gone.cpp, twice: two "font atlas
    // built" lines across one stop-and-return where one is correct.
    enum class Segment {
        Current,   // the name still stands behind these pages
        Replaced,  // a different object is at the name: a daemon came back
        Gone,      // no name at all: a daemon stopped
    };

    Segment segment_state() const {
        if (!state_ || fd_ < 0) {
            return Segment::Gone;
        }
        const int named = open_segment();
        if (named < 0) {
            return Segment::Gone;
        }
        struct stat ours {};
        struct stat theirs {};
        const bool same = fstat(fd_, &ours) == 0 && fstat(named, &theirs) == 0 &&
                          ours.st_dev == theirs.st_dev && ours.st_ino == theirs.st_ino;
        ::close(named);
        return same ? Segment::Current : Segment::Replaced;
    }

    // The old question, kept because the CLI and the settings window ask exactly
    // it and neither has anything to hand back.
    bool still_current() const { return segment_state() == Segment::Current; }

    // Lock-free consistent read. Bounded retries: a writer crashing mid-update
    // must not spin a game's render thread forever.
    // A word on what this is in the C++ memory model, so nobody "fixes" it: the
    // copies below read plain fields another process may be writing at that
    // moment, which is a data race by the letter of the standard. It is the
    // seqlock's own shape -- the copy is allowed to be torn, and the sequence
    // compared across it (acquire load before, acquire fence and load after,
    // against a writer that fetch_adds with acq_rel and fences its stores) is
    // what says whether it was; a torn copy is retried, never trusted. Making
    // the fields atomic would buy nothing the check does not already give and
    // would put a relaxed atomic load per byte on the present path. Measured
    // rather than argued (tests/shared_state_layout.cpp at both widths, the
    // segment crossed between them; shm_reattach and shm_short_segment for the
    // lifecycle), and the writer's side of the same contract is StateWriter::
    // publish above.
    bool read(Snapshot& out) const {
        if (!state_ || state_->abi_version != kAbiVersion) {
            return false;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint32_t before = state_->sequence.load(std::memory_order_acquire);
            if (before & 1u) {
                continue;  // write in progress
            }

            out.status = static_cast<DaemonStatus>(state_->status);
            out.connected = state_->connected != 0;
            out.in_channel = state_->in_channel != 0;
            out.user_count = state_->user_count;
            out.display_height = state_->display_height;
            if (out.user_count > kMaxUsers) {
                out.user_count = kMaxUsers;
            }
            std::memcpy(out.channel_name, state_->channel_name, kChannelCapacity);
            std::memcpy(out.users, state_->users, sizeof(out.users));
            std::memcpy(&out.notification, &state_->notification, sizeof(out.notification));

            // Every text field ends here, whatever the segment holds. The writer
            // terminates, but this is a reader of memory another process wrote and
            // the whole panel walks these to a NUL -- the hash already had this
            // guard (avatar_hash_is_sane) and the note segment already forces a
            // terminator; the state reader was the one that trusted its writer.
            out.channel_name[kChannelCapacity - 1] = '\0';
            out.notification.title[kNotificationTitleCapacity - 1] = '\0';
            out.notification.body[kNotificationBodyCapacity - 1] = '\0';
            out.notification.avatar_hash[kAvatarHashCapacity - 1] = '\0';
            for (uint32_t i = 0; i < kMaxUsers; ++i) {
                out.users[i].name[kNameCapacity - 1] = '\0';
                out.users[i].avatar_hash[kAvatarHashCapacity - 1] = '\0';
            }

            std::atomic_thread_fence(std::memory_order_acquire);
            if (state_->sequence.load(std::memory_order_acquire) == before) {
                return true;
            }
        }
        return false;
    }

    ~StateReader() { close(); }

private:
    // The segment, by whichever of its two names this process can reach.
    //
    // A game inside a Flatpak has a private /dev/shm, so shm_open() there opens
    // nothing however healthy the daemon is; what it can reach is the mirror the
    // daemon wrote into the one directory that crosses the sandbox
    // (vocem/flatpak.h). The mapping is MAP_SHARED over the same inode on both
    // sides, so everything below this line -- the size check, the seqlock, the
    // exact comparison of abi_version -- is the same code answering the same
    // question. Only the name differs.
    static int open_segment() {
        char path[512];
        if (bridge_in_use() && bridge_path(path, sizeof(path), kBridgeStateName)) {
            return ::open(path, O_RDONLY | O_CLOEXEC);
        }
        char name[64];
        shm_name(name, sizeof(name), getuid());
        return shm_open(name, O_RDONLY, 0);
    }

    int fd_ = -1;
    const SharedState* state_ = nullptr;
};

}  // namespace vocem

#endif  // VOCEM_SHM_H
