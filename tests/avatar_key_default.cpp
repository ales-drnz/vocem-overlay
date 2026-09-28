// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// People with no avatar share the picture they are drawn with, so they share
// its cache key.
//
// Entry 184: AvatarKey::make keyed by the user's id while the FILE collapsed an
// absent or insane hash to default_<(id >> 22) % 6>.rgba, so a full channel of
// 24 people without avatars was 24 textures of one picture -- 24 uploads, and
// on the Vulkan side 24 images, views and descriptor sets. The key follows the
// file now, through the same avatar_hash_is_sane test. Against the key as it
// stood the first check fails: two strangers with no avatar had two keys.

#include <stdio.h>

#include <unordered_set>

#include "vocem/avatar_key.h"
#include "vocem/shared_state.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

int main() {
    using vocem::AvatarKey;
    // Two ids whose default picture is the same one: (id >> 22) % 6 == 0.
    const uint64_t first = 700;
    const uint64_t second = 800;
    check(AvatarKey::make(first, "") == AvatarKey::make(second, ""),
          "two people with no avatar and the same default picture share one key");
    check(AvatarKey::make(first, nullptr) == AvatarKey::make(first, ""),
          "a missing hash and an empty one are the same answer");
    check(AvatarKey::make(first, "../../etc") == AvatarKey::make(second, ""),
          "an insane hash is drawn from the default picture, and keyed as it");

    // Another of the six pictures is another key.
    const uint64_t other = (1ull << 22) * 1 + 5;  // (id >> 22) % 6 == 1
    check(!(AvatarKey::make(other, "") == AvatarKey::make(first, "")),
          "a different default picture is a different key");

    // A real avatar is still the person's own.
    const char* hash = "0123456789abcdef0123456789abcdef";
    check(!(AvatarKey::make(first, hash) == AvatarKey::make(second, hash)),
          "the same hash on two people is two keys: the file is named by the id");
    check(!(AvatarKey::make(first, hash) == AvatarKey::make(first, "")),
          "and a person's own picture is not their default one");

    // A full channel with no avatars: at most the six pictures there are.
    std::unordered_set<AvatarKey, vocem::AvatarKeyHash> keys;
    for (uint64_t i = 0; i < vocem::kMaxUsers; ++i) {
        keys.insert(AvatarKey::make(1018972252676554842ull + i * (1ull << 22), ""));
    }
    printf("     %zu key(s) for a full channel of %u people without avatars\n", keys.size(),
           vocem::kMaxUsers);
    check(keys.size() <= 6, "a full channel without avatars is at most six textures");

    printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
