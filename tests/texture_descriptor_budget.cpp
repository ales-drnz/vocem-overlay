// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar cache must never outgrow the descriptor pool it draws from.
//
// The backend's descriptor pool is created with a fixed number of sets, and
// ImGui_ImplVulkan_AddTexture on an exhausted pool does not fail -- it ignores
// the allocation error and hands vkUpdateDescriptorSets an uninitialised
// descriptor set, which the NVIDIA driver dereferences and dies on. That is
// not a theory: it is the Minecraft crash of 2026-07-29, five times over,
// symbolised frame by frame out of the core dump -- vocem_QueuePresentKHR ->
// process_uploads -> process_pending -> upload -> AddTexture -> SIGSEGV in
// libnvidia-glcore, SEGV_MAPERR at 0x108. The pool held 8 sets: the font
// atlas plus seven faces, and the eighth person to join a call -- the owner's
// friend, whose picture was mid-download -- was the crash.
//
// The cache therefore budgets itself: at most kMaxAvatarDescriptors live
// descriptor sets, and a face beyond the budget stays the grey placeholder --
// logged, never crashed into. This test walks more distinct faces than the
// budget through the stub Vulkan and counts what reaches AddTexture.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vk_stub_device.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

using vk_stub::fake_handle;
int& add_texture_calls = vk_stub::add_texture_calls;
int& remove_texture_calls = vk_stub::remove_texture_calls;
}  // namespace

int main() {
    char root[] = "/tmp/vocem-descriptor-budget-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL could not create a scratch directory\n");
        return 1;
    }
    setenv("XDG_CACHE_HOME", root, 1);

    char dir[512];
    vocem::avatar_cache_dir(dir, sizeof(dir));
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/vocem", root);
    mkdir(parent, 0700);
    mkdir(dir, 0700);

    static unsigned char pixels[vocem::kAvatarRgbaBytes];
    memset(pixels, 0x55, sizeof(pixels));

    vocem::TextureCache cache;
    check(cache.init(vk_stub::device(), fake_handle<VkPhysicalDevice>(0x2001),
                     fake_handle<VkQueue>(0x2002), 0, vk_stub::resolve, nullptr,
                     vk_stub::stub_set_loader_data),
          "the cache initialises against the stub Vulkan");

    // More distinct faces than the budget, every file already on disk -- the
    // situation five Minecraft crashes were made of, in miniature.
    const uint32_t kFaces = vocem::TextureCache::kMaxAvatarDescriptors + 5;
    const char* hash = "0123456789abcdef0123456789abcdef";
    for (uint32_t i = 0; i < kFaces; ++i) {
        char path[768];
        vocem::avatar_rgba_path(path, sizeof(path), 1000 + i, hash);
        if (!vocem::avatar_rgba_write(path, pixels, vocem::kAvatarPixels, vocem::kAvatarPixels)) {
            printf("FAIL could not write avatar file %u\n", i);
            return 1;
        }
        cache.get(1000 + i, hash);
    }

    // One upload per post-present pass, as in a game.
    for (uint32_t i = 0; i < kFaces + 8; ++i) {
        cache.process_pending();
    }

    printf("     AddTexture calls: %d (budget %u)\n", add_texture_calls,
           vocem::TextureCache::kMaxAvatarDescriptors);
    check(add_texture_calls <= static_cast<int>(vocem::TextureCache::kMaxAvatarDescriptors),
          "the cache never asks for more descriptor sets than its budget");
    // And that it asked for exactly that many, which is the precondition of
    // every check here: they are all ceilings, so a cache that never uploaded a
    // face at all -- 0 calls -- passed each of them, and would have gone on
    // passing while entry 46's pool was never approached (DESIGN 193, found by
    // the suite's review and refuted by a mutation that stops every upload).
    check(add_texture_calls == static_cast<int>(vocem::TextureCache::kMaxAvatarDescriptors),
          "and it did fill the budget, so the ceilings above were reached and not merely "
          "not exceeded");

    // The faces beyond the budget must resolve -- to the placeholder, at once,
    // not to a retry loop that will exhaust the pool a second later.
    int beyond_with_texture = 0;
    for (uint32_t i = vocem::TextureCache::kMaxAvatarDescriptors; i < kFaces; ++i) {
        if (cache.get(1000 + i, hash) != 0) {
            ++beyond_with_texture;
        }
    }
    check(beyond_with_texture == 0, "a face beyond the budget draws the placeholder");
    check(add_texture_calls <= static_cast<int>(vocem::TextureCache::kMaxAvatarDescriptors),
          "and asking again does not grow the count");

    cache.shutdown();
    check(remove_texture_calls == add_texture_calls,
          "every set taken from the pool goes back to it");

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        // best-effort scratch cleanup
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
