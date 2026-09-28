// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The contract between vocemd and the in-game code.
//
// This struct is mapped into the address space of every game the layer attaches
// to, so its rules are strict:
//
//   * Fixed size, fixed layout, no pointers, no heap. A reader inside a game
//     process must be able to consume it without allocating.
//   * Versioned. A reader that does not recognise abi_version must refuse to
//     interpret the segment rather than guess.
//   * Lock-free for the reader. A seqlock lets the layer read a consistent
//     snapshot without ever blocking the present path, and without a misbehaving
//     game being able to stall the daemon.
//
// Only vocemd writes. Everything else opens the segment read-only.

#ifndef VOCEM_SHARED_STATE_H
#define VOCEM_SHARED_STATE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vocem/flatpak.h"

namespace vocem {

// Bump on any layout change. Readers compare exactly, never "greater than".
// v2 User::avatar_hash, v3 SharedState::status, v4 the notification slot,
// v5 display_height, v6 the same layout at 32 and 64 bits (_layout_padding).
constexpr uint32_t kAbiVersion = 6;

constexpr uint32_t kMaxUsers = 24;
constexpr uint32_t kNameCapacity = 40;      // bytes, UTF-8, NUL-terminated
constexpr uint32_t kChannelCapacity = 64;
// Discord avatar hashes are 32 hex characters, or "a_" plus 32 for animated ones.
constexpr uint32_t kAvatarHashCapacity = 40;
constexpr uint32_t kNotificationTitleCapacity = 72;
constexpr uint32_t kNotificationBodyCapacity = 192;

// What the daemon is currently doing. The interface needs this to know whether to
// offer a button and which one: "not connected" is not a single situation, and
// telling a user to wait when they should be clicking Accept is a bad interface.
enum class DaemonStatus : uint32_t {
    WaitingForDiscord = 0,   // Discord is not running, or its RPC is unreachable
    Authorising = 1,         // the prompt is on screen in Discord, waiting for the user
    Connected = 2,           // authenticated and following the voice channel
    AuthorisationRefused = 3,  // the user declined; nothing will happen until they retry
};

// Per-user flags. Deliberately a bitfield rather than separate bools so the
// struct can grow states without changing size.
enum UserFlags : uint32_t {
    kFlagSpeaking = 1u << 0,
    kFlagMuted = 1u << 1,
    kFlagDeafened = 1u << 2,
    kFlagSelf = 1u << 3,
};

struct User {
    uint64_t id;
    uint32_t flags;
    uint32_t _reserved;
    char name[kNameCapacity];
    // Identifies the cached avatar. The daemon downloads the image and writes the
    // hash here; the layer derives the file path from id + hash. Empty means the
    // user has no custom avatar and the default one should be used.
    char avatar_hash[kAvatarHashCapacity];
};

// The most recent direct message or mention. Only one slot: a toast that is already
// gone does not need to be remembered, and a queue in shared memory would mean the
// layer deciding what to discard -- which is policy, and policy belongs in the
// daemon.
struct Notification {
    // Increments with each notification. The layer compares it against what it drew
    // last to know a new one arrived; 0 means none has ever been received.
    uint64_t serial;
    uint64_t user_id;    // the author, for the avatar
    // CLOCK_MONOTONIC seconds, which both processes read from the same clock, so the
    // layer can decide when the toast has outlived its welcome without any handshake.
    double received;
    char title[kNotificationTitleCapacity];
    char body[kNotificationBodyCapacity];
    char avatar_hash[kAvatarHashCapacity];
};

struct SharedState {
    // Read first, before anything else, and bail out if it does not match.
    uint32_t abi_version;

    // Seqlock: odd means a write is in progress, even means stable. A reader
    // samples it before and after copying and retries if the values differ.
    std::atomic<uint32_t> sequence;

    uint32_t connected;   // 1 when vocemd holds a live Discord RPC connection
    uint32_t in_channel;  // 1 when the local user is in a voice channel
    uint32_t user_count;
    uint32_t status;      // DaemonStatus

    // The largest connected display's mode height, read from /sys/class/drm by
    // the daemon (vocem/display.h): the overlay sizes its atlas by the display,
    // not the window it is drawn in. Zero when no mode could be read, and the
    // reader then sizes from the drawable.
    uint32_t display_height;

    char channel_name[kChannelCapacity];

    // Explicit, and load-bearing: `User` begins with a `uint64_t`, aligned to 8
    // bytes on x86-64 and to **4** on i386. Without this field the compiler pads
    // four bytes here at 64 bits and none at 32, so `users` would begin at 96 in
    // the daemon and at 92 in a 32-bit game, which then reads every row four
    // bytes early (entry 55).
    uint32_t _layout_padding;

    User users[kMaxUsers];
    Notification notification;
};

// The layout is the ABI, so it is asserted rather than trusted, and these
// numbers are the same at both widths; moving any of them is a kAbiVersion bump.
static_assert(sizeof(User) == 96, "User's layout is part of the ABI");
static_assert(offsetof(User, id) == 0, "User::id moved");
static_assert(offsetof(User, flags) == 8, "User::flags moved");
static_assert(offsetof(User, name) == 16, "User::name moved");
static_assert(offsetof(User, avatar_hash) == 56, "User::avatar_hash moved");
static_assert(sizeof(Notification) == 328, "Notification's layout is part of the ABI");
static_assert(offsetof(SharedState, channel_name) == 28, "channel_name moved");
static_assert(offsetof(SharedState, users) == 96, "users moved -- see _layout_padding");
static_assert(offsetof(SharedState, notification) == 2400, "notification moved");
static_assert(sizeof(SharedState) == 2728, "the segment's size is part of the ABI");

// Snapshot taken by readers. Same data, without the atomics, so it can be freely
// copied around inside the layer.
struct Snapshot {
    DaemonStatus status = DaemonStatus::WaitingForDiscord;
    Notification notification = {};
    bool connected = false;
    bool in_channel = false;
    uint32_t user_count = 0;
    uint32_t display_height = 0;
    char channel_name[kChannelCapacity] = {};
    User users[kMaxUsers] = {};
};

// Text into a fixed-capacity field, cut on a character and never mid-character:
// half a UTF-8 sequence draws as a question mark, and names and messages are
// full of emoji and accents. Everything that lands in the segment or the note
// goes through it. Continuation bytes are 0b10xxxxxx: step back over them, and
// over the lead byte they belong to when it could not fit whole.
inline void copy_string(char* dest, size_t capacity, const char* source, size_t source_length) {
    if (capacity == 0) {
        return;
    }
    if (!source) {
        dest[0] = '\0';
        return;
    }
    size_t length = source_length < capacity - 1 ? source_length : capacity - 1;
    if (length < source_length) {
        while (length > 0 && (static_cast<unsigned char>(source[length]) & 0xc0) == 0x80) {
            --length;
        }
    }
    std::memcpy(dest, source, length);
    dest[length] = '\0';
}

inline void copy_string(char* dest, size_t capacity, const std::string& source) {
    copy_string(dest, capacity, source.data(), source.size());
}

// The POSIX shared memory object name. One per user so several sessions on the
// same machine cannot collide.
inline void shm_name(char* out, size_t capacity, unsigned int uid) {
    std::snprintf(out, capacity, "/vocem-%u", uid);
}

// Where the daemon caches avatar images and the layer reads them from, derived
// the same way on both sides so the path never travels through the ABI. Inside a
// Flatpak game the overlay reads the daemon's copies across the bridge instead
// (vocem/flatpak.h).
inline void avatar_cache_dir(char* out, size_t capacity) {
    if (bridge_in_use() && bridge_path(out, capacity, kBridgeAvatarsName)) {
        return;
    }
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        std::snprintf(out, capacity, "%s/vocem/avatars", xdg);
        return;
    }
    const char* home = std::getenv("HOME");
    std::snprintf(out, capacity, "%s/.cache/vocem/avatars", home ? home : "/tmp");
}

// Discord avatar hashes are hexadecimal, optionally prefixed with "a_", but this
// value arrives over the wire and is pasted into a file path and a URL. Anything
// outside that alphabet is treated as "no hash" (a default avatar), so no remote
// value can steer a write outside the cache directory.
inline bool avatar_hash_is_sane(const char* hash) {
    if (!hash || hash[0] == '\0') {
        return false;
    }
    for (uint32_t i = 0; i < kAvatarHashCapacity; ++i) {
        const char c = hash[i];
        if (c == '\0') {
            return i > 0;
        }
        const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F') || c == '_';
        if (!allowed) {
            return false;
        }
    }
    return false;  // never terminated within the field
}

// The OLD cache's path: PNG, from before the format became raw RGBA
// (vocem/avatar_rgba.h owns the living format and its path). Kept only so the
// daemon can migrate an old cache (avatars.cpp reads and unlinks these); no
// reader looks here. A user with no custom avatar gets one of Discord's
// defaults, indexed from the account id as the client does.
inline void avatar_cache_path(char* out, size_t capacity, uint64_t user_id,
                              const char* avatar_hash) {
    char dir[512];
    avatar_cache_dir(dir, sizeof(dir));
    if (avatar_hash_is_sane(avatar_hash)) {
        // `%llu`, not `%lu`: a snowflake id needs all 64 bits, and `unsigned long`
        // is 32 on i386, where the file name would lose the id's top half
        // (tests/widths.cpp holds both widths).
        std::snprintf(out, capacity, "%s/%llu_%s.png", dir,
                      static_cast<unsigned long long>(user_id), avatar_hash);
    } else {
        std::snprintf(out, capacity, "%s/default_%u.png", dir,
                      static_cast<unsigned>((user_id >> 22) % 6));
    }
}

}  // namespace vocem

#endif  // VOCEM_SHARED_STATE_H
