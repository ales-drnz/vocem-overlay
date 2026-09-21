// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The key both avatar caches index by, as a fixed-size POD.
//
// It used to be "%llu_%s" formatted into a std::string, spelled independently
// in the GL provider and the Vulkan TextureCache -- the entry-33 shape -- and,
// at ~50 characters, past the small-string optimisation: one malloc and one
// free per visible face, per frame, in both injected libraries, on the path
// the project's own rules say must not allocate. C++17's unordered_map has no
// heterogeneous lookup to hand a stack buffer to, so the key is a POD the map
// hashes and compares without ever allocating.

#ifndef VOCEM_AVATAR_KEY_H
#define VOCEM_AVATAR_KEY_H

#include <cstdint>
#include <cstring>

#include "vocem/shared_state.h"

namespace vocem {

struct AvatarKey {
    uint64_t id = 0;
    // Zero-padded to its full capacity, so equality is memcmp and the hash can
    // walk the whole field without carrying a length. An absent hash is all
    // zeroes, which no sane hash (hex and underscores) can collide with.
    char hash[kAvatarHashCapacity] = {};

    // Somebody with no avatar -- or one whose hash is not sane -- is drawn from
    // default_<(id >> 22) % 6>.rgba, and the key says so: one of six keys for
    // the six pictures, whoever wears them. It used to be the user's own id,
    // which made a channel of 24 people without avatars 24 textures of one
    // file, 24 uploads and, on the Vulkan side, 24 images, views and
    // descriptor sets (entry 184, measured and left; closed by entry 192). The
    // test is avatar_hash_is_sane, the same one avatar_rgba_path decides the
    // file with, so the key and the file cannot disagree about what a picture
    // is. Both caches derive the path from the arguments, never from the key,
    // which is why collapsing it costs the loader nothing.
    //
    // The six ids sit at the top of the 64-bit range. A Discord snowflake is a
    // millisecond timestamp shifted left 22 bits and stays below 2^63 until
    // the year 2084; the tags are within six of 2^64.
    static constexpr uint64_t kDefaultPictureTag = ~0ull - 5;

    static AvatarKey make(uint64_t user_id, const char* avatar_hash) {
        AvatarKey key;
        if (!avatar_hash_is_sane(avatar_hash)) {
            key.id = kDefaultPictureTag + (user_id >> 22) % 6;
            return key;
        }
        key.id = user_id;
        std::strncpy(key.hash, avatar_hash, sizeof(key.hash) - 1);
        return key;
    }

    bool operator==(const AvatarKey& other) const {
        return id == other.id && std::memcmp(hash, other.hash, sizeof(hash)) == 0;
    }
};

struct AvatarKeyHash {
    size_t operator()(const AvatarKey& key) const {
        // FNV-1a over the id and the padded hash bytes.
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](const void* data, size_t n) {
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < n; ++i) {
                h ^= bytes[i];
                h *= 1099511628211ull;
            }
        };
        mix(&key.id, sizeof(key.id));
        mix(key.hash, sizeof(key.hash));
        return static_cast<size_t>(h);
    }
};

}  // namespace vocem

#endif  // VOCEM_AVATAR_KEY_H
