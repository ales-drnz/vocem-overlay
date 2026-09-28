// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The font texture's copies, against a GPU that is behind (DESIGN 194).
//
// Entry 192 took the font atlas's texture away from the ImGui backend so that
// a new colour emoji goes up as its squares, "with no wait at all". Two things
// the layer's own scenes could not see, both on a GPU that has not finished
// the frames before:
//
//   * **The copies waited for each other.** One copy could be in flight, and
//     the next update began with WaitForFences(UINT64_MAX) on it -- a fence
//     that covers everything submitted before it on the game's queue, the
//     game's own frames included. The panel appearing in a channel that
//     already has an emoji in it is exactly that: the whole atlas goes up in
//     one post-present phase and the emoji is folded in the next. The scenes
//     space their arrivals out and start from a channel with none, so the fence
//     had always signalled by then.
//   * **A replacement that failed freed what ImGui was drawing with.** The
//     whole upload destroyed the old image and its descriptor set first and
//     allocated 64 MB twice after, so a failure -- memory short, the one moment
//     the allocations are largest -- left io.Fonts->TexID naming a freed set,
//     bound on the next frame: entry 46's class of crash.
//
// Both measured through the real TextureCache against the stub device
// (tests/vk_stub_device.h), with GetFenceStatus answering VK_NOT_READY.

#include <stdio.h>
#include <string.h>

#include "vk_stub_device.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

int main() {
    using vk_stub::fake_handle;
    vocem::TextureCache cache;
    check(cache.init(vk_stub::device(), fake_handle<VkPhysicalDevice>(0x2001),
                     fake_handle<VkQueue>(0x2002), 0, vk_stub::resolve, nullptr,
                     vk_stub::stub_set_loader_data),
          "the cache initialises against the stub Vulkan");

    constexpr uint32_t kSide = 64;
    static unsigned char atlas[kSide * kSide * 4];
    memset(atlas, 0x40, sizeof(atlas));
    vocem::AtlasRegion square{};
    square.x = 8;
    square.y = 8;
    square.width = 16;
    square.height = 16;

    // The GPU is behind from here on: nothing submitted has finished.
    vk_stub::fence_status = VK_NOT_READY;

    const ImTextureID first = cache.upload_font_atlas(atlas, kSide, kSide);
    check(first != 0, "the whole atlas goes up");
    cache.process_pending();  // the post-present phase that follows

    // The panel's first frame found an emoji already in the channel: folded,
    // and its square copied in the next post-present phase. And another, the
    // frame after.
    const int waits_before = vk_stub::fence_waits;
    const int idles_before = vk_stub::queue_idles;
    const bool fold_one = cache.update_font_atlas(atlas, kSide, kSide, &square, 1);
    cache.process_pending();
    const bool fold_two = cache.update_font_atlas(atlas, kSide, kSide, &square, 1);
    cache.process_pending();
    check(fold_one && fold_two, "both folds are copied into the live image");
    printf("     with the GPU behind, the two folds waited %d time(s) on a fence and %d on the "
           "queue\n", vk_stub::fence_waits - waits_before, vk_stub::queue_idles - idles_before);
    check(vk_stub::fence_waits == waits_before && vk_stub::queue_idles == idles_before,
          "a fold does not wait for the copies before it, nor for the game's frames under them");

    // The GPU catches up: every staging buffer goes, on a call that waits for
    // nothing.
    vk_stub::fence_status = VK_SUCCESS;
    cache.process_pending();
    printf("     staging buffers: %d made, %d freed once their fences signalled\n",
           vk_stub::buffers_created, vk_stub::buffers_destroyed);
    check(vk_stub::buffers_created == 3 && vk_stub::buffers_destroyed == 3,
          "and each copy's staging buffer is freed once its own fence has signalled");

    // A rebuild whose new image cannot be made. The descriptor ImGui is
    // drawing with must still be the live one afterwards.
    const int removed_before = vk_stub::remove_texture_calls;
    const int images_gone_before = vk_stub::images_destroyed;
    vk_stub::fail_create_image = true;
    const ImTextureID failed = cache.upload_font_atlas(atlas, kSide, kSide);
    printf("     after a failed replacement: %d descriptor set(s) and %d image(s) freed\n",
           vk_stub::remove_texture_calls - removed_before,
           vk_stub::images_destroyed - images_gone_before);
    check(failed == 0, "a replacement whose image cannot be made reports the failure");
    check(vk_stub::remove_texture_calls == removed_before &&
              vk_stub::images_destroyed == images_gone_before,
          "and leaves the descriptor set and the image ImGui draws with alive");

    // The next attempt works, and only then does the old one go -- once.
    const ImTextureID second = cache.upload_font_atlas(atlas, kSide, kSide);
    check(second != 0 && second != first, "the next attempt goes up as a new texture");
    check(vk_stub::remove_texture_calls == removed_before + 1,
          "and the old descriptor set is freed then, exactly once");

    cache.shutdown();
    check(vk_stub::buffers_created == vk_stub::buffers_destroyed,
          "nothing staged is left behind at shutdown");
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
