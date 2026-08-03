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

    static AvatarKey make(uint64_t user_id, const char* avatar_hash) {
        AvatarKey key;
        key.id = user_id;
        if (avatar_hash && avatar_hash[0]) {
            std::strncpy(key.hash, avatar_hash, sizeof(key.hash) - 1);
        }
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
