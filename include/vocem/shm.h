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
// -- quite possibly the daemon's publish -- gets the pipeline. Not a syscall,
// not a yield to the scheduler; `pause` is `rep; nop` and exists on every x86
// both widths build for.
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
// /dev/shm is a directory every local user can create names in, and the names
// are predictable (`/vocem-<uid>`, `/vocem-note-<uid>`). Another user who made
// the name first would have had every game of ours read a channel of their
// choosing, and a daemon that opened it with O_CREAT would have published the
// user's voice channel and messages into an object that user can read. The
// kernel's fs.protected_regular=1 (this machine's setting) refuses the
// daemon's O_CREAT open of such an object but not a reader's plain one, and
// it is a sysctl, not a promise. So every opener asks: the owner must be this
// uid, and the mode must give group and others nothing. Nothing is lost by
// the second half: the daemon creates the segment at 0600 and the bridge its
// copies at 0600 (measured: `-rw------- 1000 1000 /dev/shm/vocem-1000`), so a
// wider mode is an object this project did not make.
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

        // A fresh segment is zeroed; a reused one -- a daemon that died
        // without its unlink -- may hold stale data, and may already be mapped
        // by every running game, which go on reading the same inode. So the
        // clear is a write like any other, under the seqlock: odd before the
        // first field, even after the last, and the count carried on rather
        // than reset. It used to leave the sequence alone during the clear and
        // store 0 at the end, which let a copy that began before the clear and
        // ended inside it pass the check (the same sequence both times), and
        // made the counter run 0, 2, 0, 2 across restarts -- a reader's
        // "before" of one life equal to its "after" of the next.
        // tests/shm_reopen_clear.cpp accepted 9291 half-cleared snapshots in
        // three seconds of that. An odd count left by a daemon that died
        // mid-publish is already "writing" and stays so until the clear ends.
        // Fields are cleared individually because the struct holds an atomic
        // and is not trivially copyable as a whole.
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
    const char* refusal_ = nullptr;
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

    // Why the last open() refused an object that WAS at the name, or nullptr
    // when it found none (or attached). The caller says it; this header has
    // no log of its own.
    const char* refusal() const { return refusal_; }

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

    // What a read found. Three ways not to have a snapshot, and they mean
    // different things to a caller: no segment is "no daemon"; a foreign ABI
    // is a daemon this reader must refuse, and the refusal has to be SAID
    // (entry 55's silence); a busy writer is a daemon mid-publish, and the
    // right answer to that is the previous snapshot, not a blank frame.
    enum class Read {
        Ok,
        NotAttached,
        ForeignAbi,
        Busy,
    };

    // How long a read waits for a publish to finish before it answers Busy.
    // A publish holds the sequence odd for a few microseconds (2.6 us measured
    // for twelve participants, the review's seqlock_contention probe); the
    // old bound was eight bare loads, a few nanoseconds, so a frame whose read
    // landed inside a publish simply failed -- 7555 failed reads in 144.7
    // million at 20 publishes a second, a blank frame every three to seven
    // minutes at 144 fps. Twenty microseconds covers a publish several times
    // over and is still nothing against a frame. The clock is the vDSO's, not
    // a syscall, and it is asked once per 64 pauses. A writer that died or
    // was descheduled mid-publish is still bounded: this answers Busy and the
    // caller keeps what it had.
    static constexpr double kBusyBudgetSeconds = 20e-6;
    static constexpr int kMaxCopies = 16;

    // Lock-free consistent read into `out`. On anything but Ok, `out` may hold
    // a torn copy and must not be used -- which is why StatePoll reads into a
    // scratch snapshot and keeps its last good one apart.
    //
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
    // lifecycle; state_poll_contention for a writer publishing beside the
    // reader), and the writer's side of the same contract is StateWriter::
    // publish above.
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
    const char* refusal_ = nullptr;
};

}  // namespace vocem

#endif  // VOCEM_SHM_H
