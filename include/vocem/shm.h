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
#include <cstdio>
#include <cstring>

#include "vocem/clock.h"
#include "vocem/shared_state.h"

namespace vocem {

// One pause in a spin: tells the core this is a wait, so a sibling hyperthread
// (perhaps the daemon's publish) gets the pipeline. Not a syscall.
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Whether a segment -- or its copy across the Flatpak bridge -- may be trusted,
// from its fstat: nullptr when it is this user's own and nobody else can open
// it, otherwise the reason, for the log.
//
// /dev/shm names are predictable (`/vocem-<uid>`, `/vocem-note-<uid>`) and any
// local user can create them: another user's object at the name would feed our
// games a channel of their choosing, or receive the user's channel and messages
// from the daemon. fs.protected_regular refuses the daemon's O_CREAT open of
// such an object but not a reader's plain one, and is a sysctl, not a promise.
// So every opener asks: owner this uid, and no group or other permission bits
// (the daemon and the bridge create everything 0600).
inline const char* segment_trust_problem(const struct stat& info, uid_t uid) {
    if (info.st_uid != uid) {
        return "it belongs to another user";
    }
    if (!S_ISREG(info.st_mode)) {
        return "it is not a regular file";
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return "its mode lets other users open it (the daemon creates it 0600)";
    }
    return nullptr;
}

// The same question of an open descriptor, as this process's own uid asks it.
// Callers keep the answer and say it: a refusal nobody hears looks exactly
// like "no daemon" (entry 55).
inline const char* descriptor_trust_problem(int fd) {
    struct stat info {};
    if (fstat(fd, &info) != 0) {
        return "it cannot be examined";
    }
    return segment_trust_problem(info, getuid());
}

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
        // Somebody else's object at our name is not ours to publish into, and
        // not ours to unlink either: refuse, say why, and leave it.
        if (const char* problem = descriptor_trust_problem(fd_)) {
            std::fprintf(stderr, "vocemd: refusing the state segment %s: %s\n", name, problem);
            refusal_ = problem;
            close();
            return false;
        }
        // A creation that fails takes the name with it: shm_open creates the
        // object at zero bytes, and a name left unsized is a segment every
        // reader would map and die touching.
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

        // A fresh segment is zeroed; a reused one (a daemon that died without its
        // unlink) may hold stale data and already be mapped by every running game.
        // So the clear is a write like any other, under the seqlock: odd before
        // the first field, even after the last, and the count carried on rather
        // than reset, so a read spanning the clear or a restart cannot see the
        // same sequence twice (tests/shm_reopen_clear.cpp). An odd count left by a
        // daemon that died mid-publish already says "writing". Fields are cleared
        // one by one because the struct holds an atomic.
        uint32_t seq = state_->sequence.load(std::memory_order_acquire);
        if ((seq & 1u) == 0) {
            seq = state_->sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        std::atomic_thread_fence(std::memory_order_release);
        state_->abi_version = kAbiVersion;
        state_->connected = 0;
        state_->in_channel = 0;
        state_->user_count = 0;
        state_->status = static_cast<uint32_t>(DaemonStatus::WaitingForDiscord);
        state_->display_height = 0;
        std::memset(state_->channel_name, 0, sizeof(state_->channel_name));
        std::memset(state_->users, 0, sizeof(state_->users));
        std::memset(&state_->notification, 0, sizeof(state_->notification));
        std::atomic_thread_fence(std::memory_order_release);
        state_->sequence.store(seq + 1, std::memory_order_release);  // even: stable
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

    // Why the last open() refused an object that was at the name, or nullptr.
    const char* refusal() const { return refusal_; }

    // The canonical segment, for the mirrors to be copied from. Read-only to
    // everything but publish().
    const SharedState* state() const { return state_; }

    // Called with the segment as it stands, after every publish: the daemon's
    // Flatpak mirrors hang here (daemon/src/flatpak_bridge.h), so a sandboxed
    // game is never staler than a host one and there is one publish path. A plain
    // function pointer: this header is compiled into games and may not allocate.
    void (*on_publish)(const SharedState&, void*) = nullptr;
    void* on_publish_context = nullptr;

    // Publish under the seqlock. The callback must not block: readers spin while
    // the sequence is odd.
    template <typename Fn>
    void publish(Fn&& mutate) {
        if (!state_) {
            return;
        }
        // Odd: writing. An acq_rel read-modify-write, not a release store: a
        // release store lets the writes that follow be hoisted above it, a torn
        // read no reader can detect. Harmless on x86; written for the model.
        const uint32_t seq = state_->sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);

        mutate(*state_);

        std::atomic_thread_fence(std::memory_order_release);
        state_->sequence.store(seq + 2, std::memory_order_release);  // even: stable

        // After the segment is stable, never during: a mirror copied from a
        // half-written source would carry the tear across with a clean sequence.
        if (on_publish) {
            on_publish(*state_, on_publish_context);
        }
    }

    ~StateWriter() { close(); }

private:
    int fd_ = -1;
    SharedState* state_ = nullptr;
    const char* refusal_ = nullptr;
};

// The segment's version field, read raw without attaching. A refusal of a
// foreign ABI looks exactly like "no daemon" from outside, so the Debug section
// needs to SAY which version it met. 0 when there is no segment at all.
inline uint32_t peek_abi_version() {
    char name[64];
    shm_name(name, sizeof(name), getuid());
    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    if (descriptor_trust_problem(fd)) {
        ::close(fd);
        return 0;  // not the daemon's, so it has no version worth reporting
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
        refusal_ = nullptr;
        fd_ = open_segment();
        if (fd_ < 0) {
            return false;
        }
        // Another user's object at the name, or one others can write: every
        // field below would be theirs to choose (segment_trust_problem).
        if (const char* problem = descriptor_trust_problem(fd_)) {
            refusal_ = problem;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        // How long the object actually is, before mapping a page that may not be
        // backed: mmap past the end succeeds and the first touch raises SIGBUS,
        // and a daemon killed between shm_open and ftruncate leaves the name at
        // zero bytes -- which would kill every game in the session at once.
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

    // Why the last open() refused an object that WAS at the name, or nullptr
    // when it found none (or attached). The caller says it; this header has
    // no log of its own.
    const char* refusal() const { return refusal_; }

    // What the name says about the object this mapping came from.
    //
    // Unlinking removes the name and not the pages, so a reader that mapped once
    // keeps its private copy: a stopped daemon leaves it a cleared state forever,
    // and a restarted one creates an object this mapping never sees. One
    // shm_open and two fstats, compared by inode, let a game drop the orphaned
    // pages and attach to the living segment; callers ask on the reopen cadence,
    // never per frame. Replaced is kept apart from Gone so a daemon restart does
    // not make every game rebuild and re-upload its font atlas.
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

    // For the CLI and the settings window, which have nothing to hand back.
    bool still_current() const { return segment_state() == Segment::Current; }

    // What a read found. No segment is "no daemon"; a foreign ABI is a daemon
    // this reader must refuse, and the refusal has to be SAID; a busy writer is
    // a daemon mid-publish, answered with the previous snapshot, not a blank
    // frame.
    enum class Read {
        Ok,
        NotAttached,
        ForeignAbi,
        Busy,
    };

    // How long a read waits for a publish to finish before it answers Busy. A
    // publish holds the sequence odd for a few microseconds; twenty covers it
    // several times over and is nothing against a frame. The clock is the vDSO's,
    // asked once per 64 pauses. A writer that died mid-publish still ends in
    // Busy, and the caller keeps what it had.
    static constexpr double kBusyBudgetSeconds = 20e-6;
    static constexpr int kMaxCopies = 16;

    // Lock-free consistent read into `out`. On anything but Ok, `out` may hold
    // a torn copy and must not be used -- which is why StatePoll reads into a
    // scratch snapshot and keeps its last good one apart.
    //
    // By the letter of the C++ standard the copies below are a data race: they
    // read plain fields another process may be writing. That is the seqlock's
    // shape -- the copy may be torn, and the sequence compared across it (acquire
    // load before, acquire fence and load after, against StateWriter::publish's
    // acq_rel fetch_add and fences) says whether it was; a torn copy is retried,
    // never trusted. Atomic fields would add nothing but a relaxed load per byte
    // on the present path (tests/shared_state_layout.cpp, state_poll_contention).
    Read read_state(Snapshot& out) const {
        if (!state_) {
            return Read::NotAttached;
        }
        if (state_->abi_version != kAbiVersion) {
            return Read::ForeignAbi;
        }
        double give_up = 0.0;  // asked of the clock only once a wait begins
        int pauses = 0;
        for (int copies = 0; copies < kMaxCopies;) {
            const uint32_t before = state_->sequence.load(std::memory_order_acquire);
            if (before & 1u) {
                // A publish in progress: wait for it rather than count it as
                // a try, with the clock as the bound.
                cpu_relax();
                if ((++pauses & 63) == 0) {
                    const double now = monotonic_seconds();
                    if (give_up == 0.0) {
                        give_up = now + kBusyBudgetSeconds;
                    } else if (now >= give_up) {
                        return Read::Busy;
                    }
                }
                continue;
            }
            ++copies;

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

            // Every text field ends here, whatever the segment holds: this is
            // memory another process wrote, and the panel walks these to a NUL.
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
                return Read::Ok;
            }
        }
        return Read::Busy;
    }

    // The old question, for the callers with nothing to keep between reads
    // (the CLI, the settings window, the tests): true only for Ok.
    bool read(Snapshot& out) const { return read_state(out) == Read::Ok; }

    // The ABI word the segment carries, for a refusal that says what it met.
    uint32_t abi_version() const { return state_ ? state_->abi_version : 0; }

    ~StateReader() { close(); }

private:
    // The segment, by whichever of its two names this process can reach: inside
    // a Flatpak /dev/shm is private, so the reader opens the daemon's mirror in
    // the bridge directory (vocem/flatpak.h). Everything after the open is the
    // same code; only the name differs.
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
    const char* refusal_ = nullptr;
};

}  // namespace vocem

#endif  // VOCEM_SHM_H
