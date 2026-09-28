// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The Vulkan half draws. Measured through its own present hook, on a real
// swapchain, for the first time.
//
// This is the project's oldest hole, and DESIGN admits it three times: nothing
// in tests/ ever created a swapchain and called vkQueuePresentKHR, so
// vocem_QueuePresentKHR -- the entry point the whole Vulkan half hangs from --
// had no test at all. hdr_pixels reaches a real device and measures the colour
// pipeline, which is the arithmetic *inside* the draw on a surfaceless device;
// it never asks whether the hook fires. Entry 114's rule-8 fix was declared
// untestable-and-therefore-unchanged on exactly this absence, and the gamescope
// question -- gamescope replaces the WSI implementation under us with
// VK_LAYER_FROG_gamescope_wsi, which is where our hook lives -- cannot be
// answered without an instrument that watches a presented frame.
//
// The shape is gl_draw_local's, one API over: publish a channel into a private
// /dev/shm, let the overlay draw over 45 frames, read a frame back and require
// pixels this process did not paint. What differs is everything about the
// witness being trustworthy:
//
//   * **The chain is built, not inherited.** VK_IMPLICIT_LAYER_PATH is pointed
//     at a scratch directory holding this build's manifest, NVIDIA's, and
//     whatever VOCEM_VK_EXTRA_MANIFESTS names. MangoHud and Steam's overlay are
//     left out on purpose: gl_beside_mangohud.cmake is the record of what a
//     foreign interposer does to a pixel count -- it passes, with somebody
//     else's pixels. An instrument that can be contaminated is not an
//     instrument. The other edge of the same knife cut once already: the first
//     gamescope run excluded gamescope's OWN WSI layer along with the
//     interposers, and reported a confident success about a process that had
//     merely been launched by gamescope. Hence the extra-manifest door, and
//     hence every mapped layer library being printed rather than assumed.
//   * **Which library answered is checked, not assumed.** Both the installed
//     manifest and this one carry the name VK_LAYER_VOCEM_overlay and the loader
//     keeps whichever it finds first (measured: it says so out loud under
//     VK_LOADER_DEBUG), so a run could silently measure the *packaged* layer
//     while reporting on the build tree. /proc/self/maps says which .so is
//     mapped, and the run refuses if it is not the one it was pointed at.
//   * **The same binary is its own control.** With VOCEM_DISABLE=1 in the
//     environment the loader does not load the layer at all, and then this scene
//     must come back empty: same window, same clear, same 45 presents, zero
//     foreign pixels and nothing mapped. Registered as vk_present_disabled, it
//     is also the first test of entry 35's promise on this side -- that the
//     variable switches off drawing and not merely some of it. Without it a
//     count above the threshold could have come from anything else on this
//     machine that draws into a swapchain.
//
// Not a child process, which is where this started and why the note is here: a
// control forked from the measuring pass inherits its private /dev/shm, and that
// tmpfs by then contains `vocem-<uid>` -- published by the parent, under the name
// the daemon uses, which is the point of it. private_shm.h looks for exactly that
// name to decide whether publishing is safe, so the child concluded it was about
// to overwrite the live daemon and refused, correctly. The two passes are
// independent runs, each with a sandbox and a segment of its own.
//
// The background level is measured out of the frame (the modal byte) instead of
// being derived from the clear colour: the swapchain format may be _SRGB, where
// vkCmdClearColorImage encodes what it is given, and a test that computes the
// expected grey is a test that can be wrong about the encoding rather than about
// the overlay. The clear is neutral grey so channel order never enters into it.
//
// Three more things the same probe can do, each chosen by the environment so
// there is one source and not four (the widths.cpp pattern):
//
//   * VOCEM_VK_WITNESS_MANIFEST names the manifest of tests/vk_witness_layer.cpp,
//     which is put into the chain BELOW the overlay's layer and writes down
//     what the overlay asks of it. After the run the report is read: no wait
//     inside a present, no submit with a signalled fence, no pipeline bound in
//     a render pass it is incompatible with -- and at least one submit inside a
//     present, which is the overlay's own draw and the proof the witness sat
//     under it rather than over it. Pixels cannot see any of those; the layer
//     shipped 0.1.7 with all three (DESIGN entry 131).
//   * VOCEM_VK_SCENARIO=recreate: after the frames the swapchain is recreated
//     in the OTHER 8-bit format (sRGB where it was UNORM and the reverse), the
//     old one handed over as oldSwapchain, and the frames run again. The
//     overlay must draw into the second one too, through a pipeline compatible
//     with its render pass: ImGui's stock pipeline was built for the first
//     format, and the witness is what tells a wrong pipeline from a right one.
//     VOCEM_VK_FORMAT_FIRST=srgb starts with the sRGB format, which is the
//     direction where the stock pipeline used to be drawn into a UNORM pass.
//   * VOCEM_VK_SCENARIO=second-device: a second VkDevice is created on the same
//     adapter and destroyed at once, the way a helper device comes and goes,
//     and the frames continue. The overlay's backend belongs to the first
//     device and must survive the second one's death: it used to be torn down
//     for any device destroyed in the process, and rebuilt -- "backend ready"
//     twice in the log, and every avatar re-uploaded -- on the next present.
//     The layer's log goes to this process's stderr, which is duplicated into
//     a file so the count can be read.
//   * VOCEM_VK_SCENARIO=in-flight: the frames are chained the way a game chains
//     them -- acquire signals a semaphore, the clear waits on it and signals
//     another, the present waits on that, two frames in flight on a fence each,
//     no idle wait anywhere in the loop. The default loop idles the queue
//     before and after every present and waits on nothing, so the layer's two
//     synchronisation moves -- the wait on its own per-image fence when its
//     previous submit for that image is still in flight, and the substitution
//     of its own semaphore for the application's in the present -- were
//     exercised by nothing. With the witness in the chain, every present the
//     overlay drew must wait on the semaphore the overlay's own submit signalled
//     and on nothing else, every present it passed through must wait on this
//     probe's own, and the only wait allowed on the present path is
//     vkWaitForFences on a fence the overlay itself submitted.
//   * VOCEM_VK_SCENARIO=daemon-gone: after the frames, the segment this probe
//     publishes is unlinked -- which is exactly what vocemd does when the tray's
//     Quit stops it -- and the frames go on for two and a half seconds, because
//     the poll's cadence is a second of real time. The layer must hand back the
//     backend, the font atlas and every face it is holding on that daemon's
//     behalf, say so once, and draw nothing into the frame that is read back.
//     Against the layer shipped in 0.1.10-2 it kept all of it for the life of
//     the process (DESIGN entry 146; the OpenGL half is tests/gl_daemon_gone.cpp,
//     which can weigh the memory because it is the only process using it).
//   * VOCEM_VK_SCENARIO=flatpak-off: the process wears FLATPAK_ID and an
//     XDG_RUNTIME_DIR of this test's own, so the layer enters the bridge
//     (vocem/flatpak.h) and reads its settings from the bridge's copy of
//     config.ini -- which says `enabled = false`. Nothing is drawn, and the
//     `request` file the layer writes for the daemon has to say `drawing=0`:
//     the daemon serves a sandbox that is drawing the channel and every face,
//     and one that is not its settings and a cleared state. The layer used to
//     tell it `allowed` where the GL side told it `enabled && allowed`, so a
//     Flatpak game with the master switch off kept receiving the channel from
//     a daemon that believed it was drawing (DESIGN entry 138). Against the
//     layer as shipped in 0.1.8 the file reads `drawing=1`.
//   * VOCEM_VK_SCENARIO=second-presenter: a second device on the same adapter
//     with a window and a swapchain of its own PRESENTS beside the first. The
//     renderer belongs to the first device; the second is passed through while
//     both present, takes the overlay over once the first has been silent for
//     the hand-over interval (entry 210's rule, on this side), and gives it
//     back when it is destroyed. Against the layer before this scene existed
//     the second device's frames were drawn with the first device's buffers:
//     VUID-vkCmdBindVertexBuffers-commonparent, then SIGSEGV.
//   * VOCEM_VK_SCENARIO=failed-presenter: second-presenter with the first
//     device's texture cache refused by the witness, so the renderer cannot be
//     made there. The first device holds the overlay all the same, the second
//     gets it once the first falls silent, and the first gets it back when the
//     second is gone. Against the layer before this scene existed the failure
//     was the whole process's and nothing was drawn on either device.
//   * VOCEM_VK_SCENARIO=no-texture-cache: the witness refuses the texture
//     cache's sampler, so the cache does not come up. The renderer used to
//     fall back to ImGui's stock font upload, whose command buffer is
//     allocated through the chain and never registered with the loader (rule
//     5, entry 42); it declines to draw now, says so once, and the frame is
//     the game's own.
//   * VOCEM_VK_SCENARIO=deferred: the plain scene on a swapchain created with
//     VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT, where an image has
//     no memory until it is first acquired. The layer made a view and a
//     framebuffer of every image at the first present; against that layer
//     every overlay submit fails, the frame reads zero foreign pixels, and the
//     validation layer below it (vk_present_validated) reports the device lost.

#include <dlfcn.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include <X11/Xlib.h>

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"
#include "vocem/avatar_rgba.h"
#include "vocem/shm.h"

// The layer's file calls on the emoji bank, stamped (tests/vk_file_witness.cpp).
extern "C" int vocem_file_witness_stamps(const long long** stamps);

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

[[noreturn]] void skip(const char* why) {
    printf("skip %s\n", why);
    exit(77);
}

void write_file(const char* path, const char* contents) {
    if (FILE* file = fopen(path, "w")) {
        fputs(contents, file);
        fclose(file);
    }
}

bool copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) {
        return false;
    }
    FILE* out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        fwrite(buffer, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return true;
}

// A manifest handed in through VOCEM_VK_EXTRA_MANIFESTS, copied into this
// chain's implicit directory. The loader refuses an implicit layer whose
// manifest has no disable_environment -- measured with VK_LOADER_DEBUG=layer on
// the Khronos validation layer, loader 1.4.357: "doesn't contain required layer
// object disable_environment in the manifest JSON file, skipping this layer" --
// and an EXPLICIT manifest never carries one, so the copy is given one, inside
// its "layer" object. Without it vk_present_validated measured a chain the
// validation layer was never in (entry 143). gamescope's WSI manifest is
// implicit already and carries its own, so it goes through untouched.
// `given_switch` says whether the copy differs from the file.
bool copy_as_implicit(const char* from, const char* to, bool& given_switch) {
    given_switch = false;
    FILE* in = fopen(from, "rb");
    if (!in) {
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        text.append(buffer, n);
    }
    fclose(in);
    if (text.find("\"disable_environment\"") == std::string::npos) {
        const size_t key = text.find("\"layer\"");
        const size_t brace = key == std::string::npos ? std::string::npos : text.find('{', key);
        if (brace != std::string::npos) {
            text.insert(brace + 1,
                        "\n        \"disable_environment\": { \"VOCEM_VK_NO_EXTRA\": \"1\" },");
            given_switch = true;
        }
    }
    FILE* out = fopen(to, "wb");
    if (!out) {
        return false;
    }
    const bool written = fwrite(text.data(), 1, text.size(), out) == text.size();
    fclose(out);
    return written;
}

// Every Vulkan layer library mapped into this process, printed, with the vocem
// one handed back.
//
// Two jobs in one scan. The loader's choice between two manifests of the same
// name is not observable any other way, and measuring the wrong binary is one of
// the failures this file exists to refuse. The other is the composition of the
// instrument itself: a gamescope run that quietly lacked gamescope's own WSI
// layer would satisfy every assertion below while measuring nothing about
// gamescope -- so what loaded gets printed, every time, rather than assumed from
// what was asked for. Read out of the maps and not out of
// vkEnumerateInstanceLayerProperties, because the loader knows a layer by name
// whether or not its library could be loaded, and the name is not the question.
bool mapped_layers(char* vocem_out, size_t capacity) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    char line[2048];
    char seen[16][1024];
    int seen_count = 0;
    bool found = false;
    while (fgets(line, sizeof(line), maps)) {
        if (!strstr(line, "libVkLayer") && !strstr(line, "libvocem_vk") &&
            !strstr(line, "libMangoHud")) {
            continue;
        }
        char* start = strchr(line, '/');
        if (!start) {
            continue;
        }
        if (char* end = strchr(start, '\n')) {
            *end = '\0';
        }
        // One library owns several mappings; report it once.
        bool already = false;
        for (int i = 0; i < seen_count; ++i) {
            if (strcmp(seen[i], start) == 0) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        if (seen_count < 16) {
            snprintf(seen[seen_count], sizeof(seen[0]), "%s", start);
            ++seen_count;
        }
        printf("     mapped layer: %s\n", start);
        if (strstr(start, "libvocem_vk.so")) {
            snprintf(vocem_out, capacity, "%s", start);
            found = true;
        }
    }
    fclose(maps);
    return found;
}

// ----- Vulkan, resolved at runtime so a machine without a loader skips -------

#define VOCEM_VK_INSTANCE_FUNCS(X)               \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceProperties)             \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkGetPhysicalDeviceMemoryProperties)       \
    X(vkCreateXlibSurfaceKHR)                    \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    X(vkEnumerateDeviceExtensionProperties)      \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)                       \
    X(vkDestroySurfaceKHR)                       \
    X(vkDestroyInstance)

#define VOCEM_VK_DEVICE_FUNCS(X)     \
    X(vkGetDeviceQueue)              \
    X(vkCreateSwapchainKHR)          \
    X(vkGetSwapchainImagesKHR)       \
    X(vkAcquireNextImageKHR)         \
    X(vkQueuePresentKHR)             \
    X(vkDestroySwapchainKHR)         \
    X(vkCreateCommandPool)           \
    X(vkDestroyCommandPool)          \
    X(vkAllocateCommandBuffers)      \
    X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer)            \
    X(vkResetCommandBuffer)          \
    X(vkCmdPipelineBarrier)          \
    X(vkCmdClearColorImage)          \
    X(vkCmdCopyImageToBuffer)        \
    X(vkQueueSubmit)                 \
    X(vkQueueWaitIdle)               \
    X(vkDeviceWaitIdle)              \
    X(vkCreateFence)                 \
    X(vkDestroyFence)                \
    X(vkCreateSemaphore)             \
    X(vkDestroySemaphore)            \
    X(vkWaitForFences)               \
    X(vkResetFences)                 \
    X(vkCreateBuffer)                \
    X(vkDestroyBuffer)               \
    X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory)              \
    X(vkFreeMemory)                  \
    X(vkBindBufferMemory)            \
    X(vkMapMemory)                   \
    X(vkUnmapMemory)                 \
    X(vkDestroyDevice)

struct Vk {
#define VOCEM_DECLARE(name) PFN_##name name = nullptr;
    VOCEM_VK_INSTANCE_FUNCS(VOCEM_DECLARE)
    VOCEM_VK_DEVICE_FUNCS(VOCEM_DECLARE)
#undef VOCEM_DECLARE
};

Vk vk;

constexpr uint32_t kWidth = 360;
constexpr uint32_t kHeight = 360;
constexpr int kFrames = 45;

// The clear: neutral grey, so a B8G8R8A8 swapchain and an R8G8B8A8 one give the
// same three bytes and channel order never has to be reasoned about.
constexpr float kGrey = 0.10f;

uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props, uint32_t type_bits,
                          VkMemoryPropertyFlags wanted) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
}

}  // namespace

// The wall-clock intervals of this probe's own vkQueuePresentKHR calls, for
// reading the witness's report against (vk_witness_layer.cpp says why the
// witness cannot know this itself). `handed_down` is the witness's own
// `present` stamp inside the interval: the moment the overlay's layer passed
// the present on. What the layer does BEFORE that moment is on the present
// path -- rule 8's ground -- and what it does after, before returning to the
// application, is the post-present phase rule 10 sends the expensive work to.
// One instrument tells the two apart; a wait after the hand-down is the
// design working, a wait before it is the fault.
struct Interval {
    long long from;
    long long to;
    long long handed_down;
};
Interval g_presents[2048];
int g_present_count = 0;
// When the scene's own frames began, after the warm-up. The chain analysis
// reads only what came after: the warm-up presents with no semaphores of the
// probe's, and the rule for a chained frame is not the rule for those.
long long g_scene_began = 0;
// The intervals of this probe's own vkDestroySwapchainKHR calls, read against
// the witness's `wait` stamps: the layer tears its per-swapchain resources
// down inside that call, and a vkDeviceWaitIdle there needs every queue of the
// device externally synchronised, where the application synchronises only the
// swapchain.
Interval g_destroys[16];
int g_destroy_count = 0;

long long now_ns() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000000000LL + static_cast<long long>(now.tv_nsec);
}

// Whether `stamp` falls on the present path: inside one of the probe's
// presents and before the layer handed it down. A present the witness never
// stamped (it passed through undrawn) counts as a whole.
bool on_present_path(long long stamp) {
    for (int i = 0; i < g_present_count; ++i) {
        const Interval& interval = g_presents[i];
        if (stamp >= interval.from && stamp <= interval.to) {
            return interval.handed_down == 0 || stamp < interval.handed_down;
        }
    }
    return false;
}

bool inside_a_present(long long stamp) {
    for (int i = 0; i < g_present_count; ++i) {
        if (stamp >= g_presents[i].from && stamp <= g_presents[i].to) {
            return true;
        }
    }
    return false;
}

// Reads the witness's `present` stamps into the intervals they fall in.
void place_hand_downs(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        long long stamp = 0;
        char word[128] = {0};
        if (sscanf(line, "%lld %127s", &stamp, word) != 2 || strcmp(word, "present") != 0) {
            continue;
        }
        for (int i = 0; i < g_present_count; ++i) {
            if (stamp >= g_presents[i].from && stamp <= g_presents[i].to) {
                g_presents[i].handed_down = stamp;
                break;
            }
        }
    }
    fclose(file);
}

// How many report lines carry `event` (the word after the stamp, compared
// whole -- "submit" is not "submit-signalled-fence"), how many of those fell
// inside one of this probe's presents at all, and how many on the present
// path proper. -1 when the report does not exist.
long count_events(const char* path, const char* event, long* inside, long* on_path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long count = 0;
    if (inside) {
        *inside = 0;
    }
    if (on_path) {
        *on_path = 0;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        long long stamp = 0;
        char word[128] = {0};
        if (sscanf(line, "%lld %127s", &stamp, word) != 2 || strcmp(word, event) != 0) {
            continue;
        }
        ++count;
        if (inside && inside_a_present(stamp)) {
            ++*inside;
        }
        if (on_path && on_present_path(stamp)) {
            ++*on_path;
        }
    }
    fclose(file);
    return count;
}

// How many report lines carry `event` and `detail` (a word of the line, e.g.
// the function a `wait` names) inside one of the probe's vkDestroySwapchainKHR
// calls. -1 when the report does not exist.
long events_inside_destroys(const char* path, const char* event, const char* detail) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        long long stamp = 0;
        char word[128] = {0};
        if (sscanf(line, "%lld %127s", &stamp, word) != 2 || strcmp(word, event) != 0 ||
            !strstr(line, detail)) {
            continue;
        }
        for (int i = 0; i < g_destroy_count; ++i) {
            if (stamp >= g_destroys[i].from && stamp <= g_destroys[i].to) {
                ++count;
                break;
            }
        }
    }
    fclose(file);
    return count;
}

// A handle as the witness spells it (a C-style cast reads a pointer and an
// integer alike; non-dispatchable handles are one or the other by width).
template <typename Handle>
unsigned long long handle_value(Handle handle) {
    return (unsigned long long)handle;
}

// Which of this probe's presents last showed each swapchain image, by the
// image's handle: 1 for the first present of the run, counting every present
// the probe makes. The read-back asks it, because acquiring an image says only
// that the presentation engine is done with it -- not when it was last
// presented. With FIFO and three images the engine here can keep one image
// for many frames while it hands the other two back in turn (measured,
// NVIDIA under Xwayland: every frame after a hand-over's build went to images
// 1 and 2, image 0 held since the last present before the renderer existed),
// and the read-back then acquired that one: a frame presented before the overlay could draw,
// read as the overlay failing to draw.
struct ImagePresent {
    unsigned long long image;
    long long sequence;
};
ImagePresent g_image_presents[32];
int g_image_present_count = 0;
long long g_present_sequence = 0;

void note_presented(VkImage image) {
    ++g_present_sequence;
    const unsigned long long key = handle_value(image);
    for (int i = 0; i < g_image_present_count; ++i) {
        if (g_image_presents[i].image == key) {
            g_image_presents[i].sequence = g_present_sequence;
            return;
        }
    }
    if (g_image_present_count < 32) {
        g_image_presents[g_image_present_count++] = {key, g_present_sequence};
    }
}

// 0 for an image this probe never presented.
long long last_presented(VkImage image) {
    const unsigned long long key = handle_value(image);
    for (int i = 0; i < g_image_present_count; ++i) {
        if (g_image_presents[i].image == key) {
            return g_image_presents[i].sequence;
        }
    }
    return 0;
}

// A destroyed swapchain's images: a later swapchain may be handed the same
// handles, and an image it never presented must not inherit their history.
void forget_presented(const VkImage* images, uint32_t count) {
    for (uint32_t k = 0; k < count; ++k) {
        const unsigned long long key = handle_value(images[k]);
        for (int i = 0; i < g_image_present_count; ++i) {
            if (g_image_presents[i].image == key) {
                g_image_presents[i] = g_image_presents[--g_image_present_count];
                break;
            }
        }
    }
}

// The semaphores this probe presents with in the in-flight scenario, so the
// analysis below can tell "the overlay passed the frame through" from "the
// overlay substituted its own".
unsigned long long g_own_semaphores[4];
int g_own_semaphore_count = 0;

// Reads the handles after `key=` in a report line into `out`; how many.
int parse_handles(const char* line, const char* key, unsigned long long* out, int capacity) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), " %s=", key);
    const char* at = strstr(line, pattern);
    if (!at) {
        return 0;
    }
    at += strlen(pattern);
    int count = 0;
    while (*at && *at != ' ' && *at != '\n' && count < capacity) {
        char* end = nullptr;
        out[count++] = strtoull(at, &end, 16);
        if (end == at) {
            break;
        }
        at = *end == ',' ? end + 1 : end;
    }
    return count;
}

// What the in-flight scenario holds the chain to, read out of the witness's
// report against this probe's present intervals:
//   * a present the overlay drew into (an in-interval submit precedes it) waits
//     on exactly the semaphore that submit signalled -- the layer's own, and
//     nothing of the probe's left in the list;
//   * a present the overlay passed through waits on this probe's own semaphore;
//   * a wait on the present path is vkWaitForFences on a fence the overlay
//     itself submitted, and nothing else -- rule 8's "the only wait is on our
//     own fence", said as a measurement.
struct ChainReport {
    long presents_drawn = 0;
    long presents_passed = 0;
    long present_mismatches = 0;
    long own_fence_waits_on_path = 0;
    long foreign_waits_on_path = 0;
};

ChainReport analyse_chain(const char* path) {
    ChainReport out;
    FILE* file = fopen(path, "r");
    if (!file) {
        return out;
    }
    // The overlay's own submits: fence and signals, per present interval.
    struct Submit {
        long long stamp;
        unsigned long long fence;
        unsigned long long signals[4];
        int signal_count;
    };
    static Submit submits[4096];
    int submit_count = 0;
    char line[1024];
    // Two passes, because a present line is read against the submits before it
    // and a wait against the fences submitted before it: the report is in
    // order, so one pass with a growing table is the same thing.
    while (fgets(line, sizeof(line), file)) {
        long long stamp = 0;
        char word[128] = {0};
        if (sscanf(line, "%lld %127s", &stamp, word) != 2) {
            continue;
        }
        if (stamp < g_scene_began) {
            continue;  // the warm-up's frames, not the scene's
        }
        if (strcmp(word, "submit") == 0) {
            if (!inside_a_present(stamp) || submit_count >= 4096) {
                continue;  // the probe's own, outside its presents
            }
            Submit& s = submits[submit_count++];
            s.stamp = stamp;
            unsigned long long fence = 0;
            parse_handles(line, "fence", &fence, 1);
            s.fence = fence;
            s.signal_count = parse_handles(line, "signals", s.signals, 4);
            continue;
        }
        if (strcmp(word, "present") == 0) {
            unsigned long long sems[4];
            const int count = parse_handles(line, "sems", sems, 4);
            // The overlay's submit inside the same interval, if any.
            const Submit* drew = nullptr;
            for (int i = submit_count - 1; i >= 0; --i) {
                if (submits[i].stamp <= stamp) {
                    for (int k = 0; k < g_present_count; ++k) {
                        const Interval& interval = g_presents[k];
                        if (stamp >= interval.from && stamp <= interval.to &&
                            submits[i].stamp >= interval.from && submits[i].stamp <= interval.to) {
                            drew = &submits[i];
                        }
                    }
                    break;
                }
            }
            if (drew) {
                ++out.presents_drawn;
                const bool exact = count == 1 && drew->signal_count == 1 &&
                                   sems[0] == drew->signals[0];
                if (!exact) {
                    ++out.present_mismatches;
                }
            } else {
                ++out.presents_passed;
                bool own = count == 1;
                if (own) {
                    own = false;
                    for (int i = 0; i < g_own_semaphore_count; ++i) {
                        own = own || g_own_semaphores[i] == sems[0];
                    }
                }
                if (!own) {
                    ++out.present_mismatches;
                }
            }
            continue;
        }
        if (strcmp(word, "wait") == 0 && on_present_path(stamp)) {
            unsigned long long fences[4];
            const int count = strstr(line, "vkWaitForFences") ? parse_handles(line, "fences", fences, 4) : 0;
            bool all_own = count > 0;
            for (int i = 0; i < count && all_own; ++i) {
                bool found = false;
                for (int j = 0; j < submit_count; ++j) {
                    found = found || submits[j].fence == fences[i];
                }
                all_own = found;
            }
            if (all_own) {
                ++out.own_fence_waits_on_path;
            } else {
                ++out.foreign_waits_on_path;
            }
        }
    }
    fclose(file);
    return out;
}

// And how many contain it anywhere: the layer's log lines carry a prefix.
long lines_containing(const char* path, const char* needle) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            ++count;
        }
    }
    fclose(file);
    return count;
}

int main() {
    // The control pass is this same binary with the variable set: the loader
    // then refuses the layer on its disable_environment, so nothing of ours is
    // in the chain and the scene must come back empty.
    const bool control = getenv("VOCEM_DISABLE") != nullptr;
    const char* scenario = getenv("VOCEM_VK_SCENARIO") ? getenv("VOCEM_VK_SCENARIO") : "";
    const bool recreate = strcmp(scenario, "recreate") == 0;
    const bool second_device = strcmp(scenario, "second-device") == 0;
    const bool flatpak_off = strcmp(scenario, "flatpak-off") == 0;
    const bool in_flight = strcmp(scenario, "in-flight") == 0;
    const bool daemon_gone = strcmp(scenario, "daemon-gone") == 0;
    const bool idle = strcmp(scenario, "idle") == 0;
    const bool arrivals = strcmp(scenario, "arrivals") == 0;
    const bool early_exit = strcmp(scenario, "early-exit") == 0;
    const bool deferred = strcmp(scenario, "deferred") == 0;
    // failed-presenter is second-presenter with the first device's renderer
    // refused (the no-texture-cache scene's witness), see its checks below.
    const bool failed_presenter = strcmp(scenario, "failed-presenter") == 0;
    const bool second_presenter =
        strcmp(scenario, "second-presenter") == 0 || failed_presenter;
    const bool second_queue = strcmp(scenario, "second-queue") == 0;
    const bool alternate_queue = strcmp(scenario, "alternate-queue") == 0;
    const bool no_cache = strcmp(scenario, "no-texture-cache") == 0;
    const bool srgb_first = getenv("VOCEM_VK_FORMAT_FIRST") &&
                            strcmp(getenv("VOCEM_VK_FORMAT_FIRST"), "srgb") == 0;
    const char* witness_manifest = getenv("VOCEM_VK_WITNESS_MANIFEST");

    if (!getenv("DISPLAY")) {
        skip("no DISPLAY, so no surface to present to");
    }
    // The manifest of the layer under test. Required rather than defaulted: a
    // run that silently measured whatever is installed would be the wrong
    // witness with no sign of it.
    const char* manifest = getenv("VOCEM_VK_MANIFEST");
    if (!manifest) {
        skip("meant to run with VOCEM_VK_MANIFEST pointing at the layer manifest to measure");
    }

    // A private /dev/shm before anything is published -- the whole argument is
    // in private_shm.h, and it is an incident rather than a precaution.
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }

    vocem_test::set_alarm(180, "the Vulkan present hook");

    char root[] = "/tmp/vocem-vk-present-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[700];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    // shown_apps names THIS binary, read off /proc/self/exe rather than written
    // out as a literal -- through tests/probe_name.h, which is the one spelling
    // of it (entry 168). This file kept a hand-rolled copy of that readlink for
    // a release after the header was written FOR it: entry 168 swept the nine
    // OpenGL probes and left the Vulkan one, which is the probe entry 129 is
    // about and where the rule came from.
    // The literal cost a measurement: built at -m32 the
    // executable is vocem_vk_present_draw32, the literal said
    // vocem_vk_present_draw, the detection quite correctly declined a process
    // nobody had asked for -- and a run that reported "the 32-bit Vulkan layer
    // loads and draws nothing" looked exactly like entries 30/33/34's defect
    // until the log was read (`not drawing ...: does not look like a game`).
    const std::string own_name = vocem_test::own_name("vocem_vk_present_draw");
    char config[900];
    snprintf(config, sizeof(config), "enabled = %s\nshown_apps = %s\n",
             flatpak_off ? "false" : "true", own_name.c_str());
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    write_file(path, config);
    setenv("XDG_CONFIG_HOME", root, 1);
    // The sandbox's shape, built rather than described: a runtime directory
    // with the application's own `app/<id>` in it, which is the one directory
    // Flatpak bind-mounts host<->sandbox, and the bridge's copy of the settings
    // where the layer will look for them once it has entered the bridge. The
    // layer asks /.flatpak-info first and FLATPAK_ID second; bwrap's root is the
    // host's here, which has no such file.
    static const char* const kFlatpakId = "io.vocem.test.FlatpakOff";
    char request_path[900] = {0};
    if (flatpak_off) {
        snprintf(path, sizeof(path), "%s/run", root);
        mkdir(path, 0700);
        snprintf(path, sizeof(path), "%s/run/app", root);
        mkdir(path, 0700);
        snprintf(path, sizeof(path), "%s/run/app/%s", root, kFlatpakId);
        mkdir(path, 0700);
        snprintf(path, sizeof(path), "%s/run/app/%s/vocem", root, kFlatpakId);
        mkdir(path, 0700);
        snprintf(path, sizeof(path), "%s/run/app/%s/vocem/config.ini", root, kFlatpakId);
        write_file(path, config);
        snprintf(request_path, sizeof(request_path), "%s/run/app/%s/vocem/request", root,
                 kFlatpakId);
        snprintf(path, sizeof(path), "%s/run", root);
        setenv("XDG_RUNTIME_DIR", path, 1);
        setenv("FLATPAK_ID", kFlatpakId, 1);
    }
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);

    // ---- The dispatch chain, built rather than inherited --------------------
    // Only this build's layer and NVIDIA's: MangoHud and Steam's overlay are
    // interposers of the same kind, and gl_beside_mangohud.cmake is the record
    // of what one of them does to a pixel count. The control pass builds the
    // very same directory, so the only difference between the two runs is the
    // variable -- which is what makes it a control rather than a second setup.
    snprintf(path, sizeof(path), "%s/layers", root);
    mkdir(path, 0700);
    char destination[800];
    snprintf(destination, sizeof(destination), "%s/layers/vocem.json", root);
    if (!copy_file(manifest, destination)) {
        printf("FAIL could not copy the manifest %s\n", manifest);
        return 1;
    }
    // NVIDIA's implicit manifest is copied so the scratch directory mirrors the
    // installed implicit set minus the interposers. The claim that used to stand
    // here -- that presentation on this machine goes through those layers, so a
    // chain without them measures a different machine -- was wrong, and reading
    // the file settles it: VK_LAYER_NV_optimus and VK_LAYER_NV_present are gated
    // behind __NV_PRIME_RENDER_OFFLOAD and NVPRESENT_ENABLE_SMOOTH_MOTION, and
    // neither is set here, so both are present and inert in every run of this
    // test. Kept because copying it costs nothing; believed to matter, it would
    // have been a reason that was never true. The mapped-layer lines below say
    // what actually loaded.
    snprintf(destination, sizeof(destination), "%s/layers/nvidia_layers.json", root);
    copy_file("/usr/share/vulkan/implicit_layer.d/nvidia_layers.json", destination);
    // Anything else the caller wants in the chain, which is how the gamescope
    // question gets asked at all. gamescope's own WSI layer replaces the
    // swapchain implementation and our hook lives on the present path, so
    // leaving it out in the name of a clean instrument measures a process that
    // merely RAN under gamescope -- which is what the first attempt did, and it
    // reported success. Separated by ';', the way CMake passes a list.
    if (const char* extra = getenv("VOCEM_VK_EXTRA_MANIFESTS")) {
        char list[4096];
        snprintf(list, sizeof(list), "%s", extra);
        int added = 0;
        char* cursor = list;
        while (cursor && *cursor) {
            char* separator = strchr(cursor, ';');
            if (separator) {
                *separator = '\0';
            }
            if (*cursor) {
                snprintf(destination, sizeof(destination), "%s/layers/extra%d.json", root, added);
                bool given_switch = false;
                if (copy_as_implicit(cursor, destination, given_switch)) {
                    printf("     also in the chain: %s%s\n", cursor,
                           given_switch ? " (given a disable_environment to load as implicit)"
                                        : "");
                    ++added;
                } else {
                    printf("FAIL could not copy the extra manifest %s\n", cursor);
                    return 1;
                }
            }
            cursor = separator ? separator + 1 : nullptr;
        }
    }
    // The witness, BELOW the overlay. The order of two implicit layers is not
    // something a manifest's file name decides -- measured both ways, with the
    // witness's manifest sorting first and last, it came out on top of the
    // overlay both times (one vkGetDeviceProcAddr query per name reached it,
    // the loader's own; the overlay's never did). What does decide it is the
    // loader's override meta-layer: a manifest named VK_LAYER_LUNARG_override
    // whose component_layers list IS the chain, first entry nearest the
    // application. Whether that held is what the positive control in the
    // report says, every run.
    //
    // VOCEM_VK_BELOW names one more layer to put under the overlay the same way
    // -- the Khronos validation layer, for vk_present_validated. An extra
    // manifest alone lands it wherever the loader likes, and measured with
    // VK_LOADER_DEBUG=layer it lands ABOVE the overlay: Application ->
    // validation -> overlay -> driver, a chain in which the validation layer
    // checks the probe's calls and never sees one command buffer, submit or
    // barrier of the overlay's own (entry 192).
    char witness_report[800] = {0};
    const char* below = getenv("VOCEM_VK_BELOW");
    const bool have_witness = witness_manifest && witness_manifest[0];
    if (have_witness) {
        snprintf(destination, sizeof(destination), "%s/layers/witness.json", root);
        if (!copy_file(witness_manifest, destination)) {
            printf("FAIL could not copy the witness manifest %s\n", witness_manifest);
            return 1;
        }
    }
    if (have_witness || (below && below[0])) {
        std::string components = "\"VK_LAYER_VOCEM_overlay\"";
        if (below && below[0]) {
            components += std::string(", \"") + below + "\"";
        }
        if (have_witness) {
            components += ", \"VK_LAYER_VOCEM_witness\"";
        }
        snprintf(destination, sizeof(destination), "%s/layers/VkLayer_override.json", root);
        const std::string override_manifest =
            "{\n"
            "    \"file_format_version\": \"1.1.2\",\n"
            "    \"layer\": {\n"
            "        \"name\": \"VK_LAYER_LUNARG_override\",\n"
            "        \"type\": \"GLOBAL\",\n"
            "        \"api_version\": \"1.4.350\",\n"
            "        \"implementation_version\": \"1\",\n"
            "        \"description\": \"vocem test chain: the overlay above what watches it\",\n"
            "        \"component_layers\": [" + components + "],\n"
            "        \"disable_environment\": { \"VOCEM_VK_NO_OVERRIDE\": \"1\" }\n"
            "    }\n"
            "}\n";
        write_file(destination, override_manifest.c_str());
        if (below && below[0]) {
            printf("     below the overlay: %s\n", below);
        }
    }
    if (no_cache || failed_presenter) {
        if (!have_witness) {
            skip("the no-texture-cache and failed-presenter scenes need the witness layer, "
                 "which refuses the sampler");
        }
        // The overlay's backend makes the process's first sampler (ImGui's
        // own) and its texture cache the second; the witness refuses that one.
        setenv("VOCEM_WITNESS_FAIL_SAMPLER", "2", 1);
    }
    if (have_witness) {
        snprintf(witness_report, sizeof(witness_report), "%s/witness.txt", root);
        setenv("VOCEM_WITNESS", "1", 1);
        setenv("VOCEM_WITNESS_REPORT", witness_report, 1);
        printf("     witness in the chain, reporting to %s\n", witness_report);
    }
    setenv("VK_IMPLICIT_LAYER_PATH", path, 1);
    // The dev manifest is gated on VOCEM=1 (enable_environment). Where
    // VOCEM_DISABLE is also set the loader refuses the layer regardless, which
    // is the control.
    setenv("VOCEM", "1", 1);
    // The layer's own log, sent into a file this process can read back, in the
    // scenarios that count its lines ("backend ready" for second-device,
    // "handing back" for daemon-gone). Only there: vk_inside_gamescope.cmake
    // reads the same log off this process's stderr for its own witness
    // ("drawing panel:"), and the first version redirected it unconditionally,
    // which made that test report the hook drew nothing. VOCEM_VK_KEEP_STDERR=1
    // leaves stderr alone whatever the scenario, for reading the loader's own
    // VK_LOADER_DEBUG output by hand.
    char stderr_log[800];
    snprintf(stderr_log, sizeof(stderr_log), "%s/stderr.txt", root);
    if ((second_device || daemon_gone || idle || arrivals || early_exit) &&
        !getenv("VOCEM_VK_KEEP_STDERR")) {
        if (FILE* teed = fopen(stderr_log, "w")) {
            fflush(stderr);
            dup2(fileno(teed), 2);
            fclose(teed);
        }
    }
    // Not a hint: MangoHud's layer reads this and would draw into the frame this
    // test counts.
    unsetenv("MANGOHUD");
    setenv("VOCEM_DEBUG", "1", 1);
    // The layer's log again, into a file of its own, in EVERY scenario: the
    // one logger appends each line there as well as to stderr
    // (common/src/overlay_log.cpp, line-buffered), so this is how the probe
    // waits for the layer rather than for a clock -- the warm-up below ends at
    // "backend ready", the daemon-gone scene at the release and at the second
    // build -- without taking stderr away from vk_inside_gamescope, which reads
    // its witness there (the paragraph above).
    char layer_log[800];
    snprintf(layer_log, sizeof(layer_log), "%s/layer.log", root);
    setenv("VOCEM_LOG_FILE", layer_log, 1);

    // The arrivals scene's faces, in a cache of its own: nobody in it has an
    // avatar hash, so every one of them -- the nine in the channel and the
    // message's author -- is drawn from default_<(id >> 22) % 6>.rgba, which for
    // these small ids is default_0 for all ten. That is entry 184's measurement
    // made a scene (ten cache keys for one file), and it is also what gives the
    // texture cache something to upload at all: the scenes used to read the
    // owner's real ~/.cache, where default_0 happens not to exist.
    if (arrivals) {
        char cache[800];
        snprintf(cache, sizeof(cache), "%s/cache", root);
        mkdir(cache, 0700);
        setenv("XDG_CACHE_HOME", cache, 1);
        char dir[900];
        snprintf(dir, sizeof(dir), "%s/vocem", cache);
        mkdir(dir, 0700);
        snprintf(dir, sizeof(dir), "%s/vocem/avatars", cache);
        mkdir(dir, 0700);
        char face[1024];
        vocem::avatar_rgba_path(face, sizeof(face), 700, "");
        static unsigned char pixels[vocem::kAvatarRgbaBytes];
        memset(pixels, 0x7f, sizeof(pixels));
        check(vocem::avatar_rgba_write(face, pixels, vocem::kAvatarPixels, vocem::kAvatarPixels),
              "the scene's default picture is in its own avatar cache");
    }

    // ---- The channel, published by this process -----------------------------
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    if (idle) {
        // A daemon that is up and connected, with the owner out of every voice
        // channel: a segment to attach to and nothing on it worth a frame.
        writer.publish([](vocem::SharedState& state) {
            state.connected = 1;
            state.in_channel = 0;
            state.status = 2;  // Connected
            state.user_count = 0;
        });
    } else {
        writer.publish([arrivals, early_exit](vocem::SharedState& state) {
            state.connected = 1;
            state.in_channel = 1;
            state.status = 2;  // Connected
            // The arrivals scene publishes the owner's display from the first
            // frame, as vocemd always does: otherwise the first arrival is also
            // a change of size, which is a real rebuild and not what it measures.
            // And so does early-exit, for a reason of its own: its whole subject is
            // a context that dies while the atlas worker is still rasterising, and
            // the drawable's own height gives an 11 px atlas that is built in a few
            // frames. Under a loaded machine (the suite at -j16) two frames were
            // slower than that build, the atlas landed before the teardown, and the
            // scene's own precondition failed in two runs of three (DESIGN 193). At
            // the display's 32 px the build is ~120 ms against two frames.
            if (arrivals || early_exit) {
                state.display_height = 2160;
            }
            snprintf(state.channel_name, sizeof(state.channel_name), "present-hook");
            state.user_count = 3;
            for (uint32_t i = 0; i < 3; ++i) {
                state.users[i].id = 700 + i;
                snprintf(state.users[i].name, sizeof(state.users[i].name), "Present %u", i + 1);
            }
        });
    }
    if (failures) {
        return 1;
    }

    // ---- Loader, window, instance ------------------------------------------
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        skip("no Vulkan loader (libvulkan.so.1)");
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!gipa) {
        skip("loader has no vkGetInstanceProcAddr");
    }
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create_instance) {
        skip("loader has no vkCreateInstance");
    }

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        skip("the display did not open from inside the sandbox");
    }
    // Override-redirect and parked far off screen: no window manager sees it,
    // nothing appears and no focus is taken -- the owner may be in a game while
    // this runs.
    XSetWindowAttributes attributes;
    attributes.override_redirect = True;
    attributes.background_pixel = 0;
    const int screen = DefaultScreen(display);
    Window window =
        XCreateWindow(display, RootWindow(display, screen), -4000, 0, kWidth, kHeight, 0,
                      CopyFromParent, InputOutput, CopyFromParent,
                      CWOverrideRedirect | CWBackPixel, &attributes);
    XMapWindow(display, window);
    XSync(display, False);

    // The deferred scene asks for VK_EXT_swapchain_maintenance1, whose
    // instance half is VK_EXT_surface_maintenance1 -- which needs
    // VK_KHR_get_surface_capabilities2 -- and whose device half needs
    // VK_KHR_get_physical_device_properties2 on the instance. Without the last
    // one the validation layer reports the PROBE (VUID-vkCreateDevice-
    // ppEnabledExtensionNames-01387), which is a message about this file and
    // would read as one about the overlay.
    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
                                         VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                                         VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                                         VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
    const uint32_t instance_extension_count = deferred ? 5 : 2;
    if (deferred) {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
        VkExtensionProperties offered[256];
        uint32_t offered_count = 256;
        if (!enumerate || enumerate(nullptr, &offered_count, offered) < 0) {
            skip("the loader cannot list its instance extensions");
        }
        for (uint32_t i = 2; i < instance_extension_count; ++i) {
            bool found = false;
            for (uint32_t k = 0; k < offered_count && !found; ++k) {
                found = strcmp(offered[k].extensionName, instance_extensions[i]) == 0;
            }
            if (!found) {
                printf("     missing instance extension: %s\n", instance_extensions[i]);
                skip("the deferred scene needs VK_EXT_surface_maintenance1 and its dependencies");
            }
        }
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vocem_vk_present_draw";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = instance_extension_count;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        skip("vkCreateInstance failed (no usable Vulkan, or no Xlib surface support)");
    }
#define VOCEM_LOAD_INSTANCE(name)                                  \
    vk.name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); \
    if (!vk.name) skip("instance function missing: " #name);
    VOCEM_VK_INSTANCE_FUNCS(VOCEM_LOAD_INSTANCE)
#undef VOCEM_LOAD_INSTANCE

    // Which library the loader picked for our layer name -- the choice both
    // manifests make ambiguous, and the one thing that would make every number
    // below describe a different build than the one under test.
    char mapped[4096] = {0};
    const bool have_mapped = mapped_layers(mapped, sizeof(mapped));
    if (control) {
        printf("     layer library: %s\n", have_mapped ? mapped : "(none mapped)");
        check(!have_mapped, "VOCEM_DISABLE keeps the layer out of the process entirely");
    } else {
        printf("     layer library: %s\n", have_mapped ? mapped : "(none mapped)");
        check(have_mapped, "the layer library is mapped into this process");
        if (const char* expect = getenv("VOCEM_VK_LIBRARY"); expect && have_mapped) {
            check(strcmp(mapped, expect) == 0,
                  "and it is the library this run was pointed at, not another build's");
            if (strcmp(mapped, expect) != 0) {
                printf("     asked for: %s\n", expect);
            }
        }
    }
    if (failures) {
        return 1;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkXlibSurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surface_info.dpy = display;
    surface_info.window = window;
    if (vk.vkCreateXlibSurfaceKHR(instance, &surface_info, nullptr, &surface) != VK_SUCCESS) {
        skip("vkCreateXlibSurfaceKHR failed");
    }

    uint32_t gpu_count = 0;
    vk.vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (gpu_count == 0) {
        skip("no physical device");
    }
    VkPhysicalDevice gpus[16];
    if (gpu_count > 16) {
        gpu_count = 16;
    }
    vk.vkEnumeratePhysicalDevices(instance, &gpu_count, gpus);

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    uint32_t family_queues = 0;
    for (uint32_t i = 0; i < gpu_count && gpu == VK_NULL_HANDLE; ++i) {
        uint32_t family_count = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, nullptr);
        VkQueueFamilyProperties families[32];
        if (family_count > 32) {
            family_count = 32;
        }
        vk.vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, families);
        for (uint32_t f = 0; f < family_count; ++f) {
            VkBool32 presentable = VK_FALSE;
            vk.vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], f, surface, &presentable);
            if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentable) {
                gpu = gpus[i];
                queue_family = f;
                family_queues = families[f].queueCount;
                break;
            }
        }
    }
    if (gpu == VK_NULL_HANDLE) {
        skip("no device with a graphics queue that can present to this surface");
    }

    VkPhysicalDeviceProperties gpu_props{};
    vk.vkGetPhysicalDeviceProperties(gpu, &gpu_props);
    printf("     device: %s\n", gpu_props.deviceName);

    VkSurfaceCapabilitiesKHR caps{};
    vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        skip("this surface's images cannot be a transfer source, so no frame can be read back");
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        skip("this surface's images cannot be a transfer destination, so no frame can be "
             "cleared to a known colour");
    }

    uint32_t format_count = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, nullptr);
    if (format_count == 0) {
        skip("the surface offers no formats");
    }
    VkSurfaceFormatKHR formats[64];
    if (format_count > 64) {
        format_count = 64;
    }
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, formats);
    // UNORM before sRGB, in two passes rather than first-match, and the reason is
    // a measurement that went wrong. gamescope's WSI layer offers a different
    // list from the plain XWayland surface: first-match took B8G8R8A8_UNORM
    // (format 44) plainly and B8G8R8A8_SRGB (50) under gamescope, so the two runs
    // cleared to the same float and stored different bytes -- 25 against 89, the
    // sRGB encode of the same grey -- and the panel's own dark surface stopped
    // being 40 away from its background. 1232 pixels against 652 then, and neither
    // number was wrong: they were two formats, and the comparison was measuring
    // the format rather than gamescope. Holding the format fixed is what makes
    // the counts comparable across runs; the sRGB path itself has its own test
    // (hdr_srgb_pixels), and gamescope users exercise it by default, which is
    // worth knowing on its own.
    VkSurfaceFormatKHR chosen{};
    VkSurfaceFormatKHR other{};  // the other 8-bit family, for the recreate scenario
    bool have_format = false;
    bool have_other = false;
    for (uint32_t i = 0; i < format_count; ++i) {
        const bool unorm = formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
                           formats[i].format == VK_FORMAT_R8G8B8A8_UNORM;
        const bool srgb = formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
                          formats[i].format == VK_FORMAT_R8G8B8A8_SRGB;
        // UNORM preferred (the comment above says why), sRGB first only on
        // request; the other family is kept for the recreate scenario.
        const bool first = srgb_first ? srgb : unorm;
        const bool second = srgb_first ? unorm : srgb;
        if (first && !have_format) {
            chosen = formats[i];
            have_format = true;
        } else if (second && !have_other) {
            other = formats[i];
            have_other = true;
        }
    }
    if (!have_format && have_other) {
        // Only one family on offer: it is the first, and there is no other.
        chosen = other;
        have_format = true;
        have_other = false;
    }
    if (!have_format) {
        skip("the surface offers no 8-bit-per-channel format to read back");
    }
    if (recreate && !have_other) {
        skip("the surface offers only one 8-bit format, so there is nothing to recreate into");
    }

    // The second-queue scene presents a second window from a second queue of
    // the same family, on the same device.
    if ((second_queue || alternate_queue) && family_queues < 2) {
        skip("the presenting family has one queue, so there is no second queue to present from");
    }
    const float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = second_queue || alternate_queue ? 2 : 1;
    queue_info.pQueuePriorities = priorities;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                       VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1{};
    maintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
    maintenance1.swapchainMaintenance1 = VK_TRUE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    if (deferred) {
        VkExtensionProperties offered[512];
        uint32_t offered_count = 512;
        vk.vkEnumerateDeviceExtensionProperties(gpu, nullptr, &offered_count, offered);
        bool found = false;
        for (uint32_t k = 0; k < offered_count && !found; ++k) {
            found = strcmp(offered[k].extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0;
        }
        if (!found) {
            skip("the device offers no VK_EXT_swapchain_maintenance1, so no deferred swapchain");
        }
        device_info.enabledExtensionCount = 2;
        device_info.pNext = &maintenance1;
    }
    VkDevice device = VK_NULL_HANDLE;
    if (vk.vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS) {
        skip("vkCreateDevice failed");
    }
#define VOCEM_LOAD_DEVICE(name)                                                    \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device, #name)); \
    if (!vk.name) {                                                                \
        printf("FAIL device function missing: " #name "\n");                       \
        return 1;                                                                  \
    }
    VOCEM_VK_DEVICE_FUNCS(VOCEM_LOAD_DEVICE)
#undef VOCEM_LOAD_DEVICE

    // From here on the device exists: a failure is a failure, never a skip.
    VkQueue queue = VK_NULL_HANDLE;
    vk.vkGetDeviceQueue(device, queue_family, 0, &queue);
    // The alternate-queue scene's second queue of the same family, which
    // presents every other frame of the ONE swapchain.
    VkQueue queue_alternate = VK_NULL_HANDLE;
    if (alternate_queue) {
        vk.vkGetDeviceQueue(device, queue_family, 1, &queue_alternate);
    }
    // Set by the in-flight loop below to present every other frame on it.
    VkQueue alternate_with = VK_NULL_HANDLE;
    // The image the last read_back acquired.
    uint32_t read_index = 0;
    // The first present (note_presented's count) whose frame the scene holds
    // the overlay to: an image the read-back acquires that was last presented
    // before it is given a frame first. 0 -- every presented frame counts --
    // until a scene has waited for the layer to say its renderer is up, and
    // from then on the second present after the batch the line was found
    // after: the line comes from the post-present phase of that batch's last
    // present at the latest, and the first frame a new ImGui context draws is
    // empty while it sizes its window (measured: 0 vertices on that frame
    // after every build).
    long long drawn_from = 0;
    const auto overlay_from_next_frames = [&]() { drawn_from = g_present_sequence + 2; };

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = kWidth;
        extent.height = kHeight;
    }

    VkSwapchainCreateInfoKHR swap_info{};
    swap_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swap_info.surface = surface;
    swap_info.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    if (caps.maxImageCount && swap_info.minImageCount > caps.maxImageCount) {
        swap_info.minImageCount = caps.maxImageCount;
    }
    swap_info.imageFormat = chosen.format;
    swap_info.imageColorSpace = chosen.colorSpace;
    swap_info.imageExtent = extent;
    swap_info.imageArrayLayers = 1;
    // COLOR_ATTACHMENT is what the layer needs and it adds the bit itself
    // (vocem_layer.cpp, where the swapchain is created); asking for it here
    // keeps the probe honest about what a game requests. TRANSFER_SRC and
    // TRANSFER_DST are this test's own: the read-back, and the clear to a
    // known colour with vkCmdClearColorImage. The clear went without its bit
    // for as long as nothing looked -- the validation layer, once it actually
    // loaded, reported VUID-vkCmdClearColorImage-image-00002 on every frame
    // (entry 143), the probe's fault and not the overlay's.
    swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swap_info.preTransform = caps.currentTransform;
    swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
    swap_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // the one mode always supported
    // FALSE, not TRUE: the window is parked off screen on purpose, and a clipped
    // swapchain is allowed to leave invisible pixels undefined -- which is every
    // pixel this test counts.
    swap_info.clipped = VK_FALSE;
    // The deferred scene: an image of this swapchain has no memory until it is
    // first acquired (VK_EXT_swapchain_maintenance1), so nothing may be made
    // from an image before then -- a view of it included. The layer used to
    // make a view and a framebuffer for every image at the first present,
    // when only the image being presented had ever been acquired: measured,
    // "overlay submit failed" on every frame, zero foreign pixels, and the
    // validation layer below it reporting the device lost.
    if (deferred) {
        swap_info.flags |= VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT;
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (vk.vkCreateSwapchainKHR(device, &swap_info, nullptr, &swapchain) != VK_SUCCESS) {
        printf("FAIL vkCreateSwapchainKHR\n");
        return 1;
    }
    uint32_t image_count = 0;
    vk.vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    // One spelling of the probe's own cap: the images array, the clamp after a
    // recreation, and the per-image present semaphores all have to agree, and
    // they were three literal 8s.
    constexpr uint32_t kMaxSwapchainImages = 8;
    VkImage images[kMaxSwapchainImages];
    if (image_count > kMaxSwapchainImages) {
        image_count = kMaxSwapchainImages;
    }
    vk.vkGetSwapchainImagesKHR(device, swapchain, &image_count, images);
    printf("     swapchain: %ux%u, format %d, %u images\n", extent.width, extent.height,
           (int)chosen.format, image_count);
    // Which queue last presented each image: 0 the first, 1 the alternate.
    int image_queue[kMaxSwapchainImages];
    for (int& which : image_queue) {
        which = 0;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vk.vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vk.vkAllocateCommandBuffers(device, &cmd_info, &cmd);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vk.vkCreateFence(device, &fence_info, nullptr, &fence);

    VkClearColorValue grey{};
    grey.float32[0] = kGrey;
    grey.float32[1] = kGrey;
    grey.float32[2] = kGrey;
    grey.float32[3] = 1.0f;
    VkImageSubresourceRange whole{};
    whole.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    whole.levelCount = 1;
    whole.layerCount = 1;

    // One frame into an image already acquired: cleared, handed to the
    // present, the queue drained after it. Shared by the frame loop and by the
    // read-back, which gives a frame to an image it acquired too stale to read.
    const auto paint_and_present = [&](VkSwapchainKHR chain, VkImage* chain_images,
                                       uint32_t index) -> VkResult {
        vk.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &begin);
        // UNDEFINED as the old layout: the previous contents are of no
        // interest and discarding them is what a game clearing its frame does.
        image_barrier(cmd, chain_images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.vkCmdClearColorImage(cmd, chain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &grey, 1, &whole);
        image_barrier(cmd, chain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        vk.vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vk.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        vk.vkQueueWaitIdle(queue);

        // The call this whole file exists for: the layer's hook draws here.
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.swapchainCount = 1;
        present.pSwapchains = &chain;
        present.pImageIndices = &index;
        const long long before = now_ns();
        const VkResult presented = vk.vkQueuePresentKHR(queue, &present);
        const long long after = now_ns();
        if (g_present_count < 2048) {
            g_presents[g_present_count++] = {before, after, 0};
        }
        note_presented(chain_images[index]);
        if (index < kMaxSwapchainImages) {
            image_queue[index] = queue == queue_alternate ? 1 : 0;
        }
        vk.vkQueueWaitIdle(queue);
        return presented;
    };
    // ---- The frames --------------------------------------------------------
    // 45 of them, and at least a second's worth (see below): the overlay skips
    // its first frames while ImGui sizes itself, and the first atlas is built
    // off the game's thread. No semaphores --
    // acquire waits on a fence and the queue is drained between frames, so the
    // ordering this test needs is the simplest kind that is still correct.
    // A lambda because the recreate and second-device scenarios run it again
    // on a second swapchain, or after the second device is gone.
    const auto run_frames = [&](VkSwapchainKHR chain, VkImage* chain_images, int frames) -> bool {
        for (int frame = 0; frame < frames; ++frame) {
            uint32_t index = 0;
            const VkResult acquired =
                vk.vkAcquireNextImageKHR(device, chain, UINT64_MAX, VK_NULL_HANDLE, fence, &index);
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
                printf("FAIL vkAcquireNextImageKHR returned %d on frame %d\n", (int)acquired,
                       frame);
                return false;
            }
            vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vk.vkResetFences(device, 1, &fence);
            const VkResult presented = paint_and_present(chain, chain_images, index);
            if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
                printf("FAIL vkQueuePresentKHR returned %d on frame %d\n", (int)presented, frame);
                return false;
            }
            usleep(4000);
        }
        return true;
    };
    // The chained loop: what a game does. Two frames in flight, each on a fence
    // of its own; acquire signals a semaphore the clear waits on, the clear
    // signals one the present waits on, and nothing idles the queue. The layer
    // substitutes its own semaphore for the present's and waits on its own
    // per-image fence when its previous submit for that image is still in
    // flight -- both unexercised by the loop above, which idles before and
    // after every present and presents with no semaphore at all.
    const auto run_frames_in_flight = [&](VkSwapchainKHR chain, VkImage* chain_images,
                                          int frames) -> bool {
        constexpr int kSlots = 2;
        VkSemaphore acquired[kSlots] = {};
        VkFence slot_fence[kSlots] = {};
        VkCommandBuffer slot_cmd[kSlots] = {};
        // The semaphore the PRESENT waits on is indexed by the acquired image,
        // not by the in-flight slot.
        //
        // It was indexed by slot, and with two slots over three images that is
        // the classic swapchain-semaphore-reuse hazard: a binary semaphore
        // handed to a present is not free again until the presentation engine
        // has consumed it, and the only thing that says so is re-acquiring that
        // image. Cycling two of them by slot signals one again while an older
        // present of a different image may still be waiting on it. Invisible
        // until vk_present_validated ran anything but the default loop --
        // 4 x VUID-vkQueueSubmit-pSignalSemaphores-00067, and the validation
        // layer's own hint is exactly this fix ("use a separate semaphore per
        // swapchain image; index these semaphores using the index of the
        // acquired image"). The probe's fault and not the layer's, which is the
        // shape entry 143 met from the other end.
        VkSemaphore done[kMaxSwapchainImages] = {};
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo signalled{};
        signalled.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        signalled.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkCommandBufferAllocateInfo slots_info = cmd_info;
        slots_info.commandBufferCount = kSlots;
        vk.vkAllocateCommandBuffers(device, &slots_info, slot_cmd);
        g_own_semaphore_count = 0;
        for (int k = 0; k < kSlots; ++k) {
            vk.vkCreateSemaphore(device, &sem_info, nullptr, &acquired[k]);
            vk.vkCreateFence(device, &signalled, nullptr, &slot_fence[k]);
        }
        for (uint32_t k = 0; k < image_count; ++k) {
            vk.vkCreateSemaphore(device, &sem_info, nullptr, &done[k]);
            g_own_semaphores[g_own_semaphore_count++] = handle_value(done[k]);
            printf("     probe presents image %u with semaphore 0x%llx\n", k,
                   handle_value(done[k]));
        }
        bool ok = true;
        for (int frame = 0; frame < frames && ok; ++frame) {
            const int slot = frame % kSlots;
            // This slot's previous frame, two presents ago, must be done with
            // its command buffer before it is recorded again -- the probe's own
            // wait, outside any present.
            vk.vkWaitForFences(device, 1, &slot_fence[slot], VK_TRUE, UINT64_MAX);
            vk.vkResetFences(device, 1, &slot_fence[slot]);
            uint32_t index = 0;
            const VkResult got = vk.vkAcquireNextImageKHR(device, chain, UINT64_MAX,
                                                          acquired[slot], VK_NULL_HANDLE, &index);
            if (got != VK_SUCCESS && got != VK_SUBOPTIMAL_KHR) {
                printf("FAIL vkAcquireNextImageKHR returned %d on frame %d\n", (int)got, frame);
                ok = false;
                break;
            }
            vk.vkResetCommandBuffer(slot_cmd[slot], 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk.vkBeginCommandBuffer(slot_cmd[slot], &begin);
            image_barrier(slot_cmd[slot], chain_images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vk.vkCmdClearColorImage(slot_cmd[slot], chain_images[index],
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1, &whole);
            image_barrier(slot_cmd[slot], chain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            vk.vkEndCommandBuffer(slot_cmd[slot]);

            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &acquired[slot];
            submit.pWaitDstStageMask = &stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &slot_cmd[slot];
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &done[index];
            // The alternate-queue scene: every other frame, submitted and
            // presented on the family's second queue.
            const VkQueue frame_queue =
                alternate_with != VK_NULL_HANDLE && (frame % 2) == 1 ? alternate_with : queue;
            vk.vkQueueSubmit(frame_queue, 1, &submit, slot_fence[slot]);

            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &done[index];
            present.swapchainCount = 1;
            present.pSwapchains = &chain;
            present.pImageIndices = &index;
            const long long before = now_ns();
            const VkResult presented = vk.vkQueuePresentKHR(frame_queue, &present);
            const long long after = now_ns();
            if (g_present_count < 2048) {
                g_presents[g_present_count++] = {before, after, 0};
            }
            note_presented(chain_images[index]);
            if (index < kMaxSwapchainImages) {
                image_queue[index] = frame_queue == queue_alternate ? 1 : 0;
            }
            if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
                printf("FAIL vkQueuePresentKHR returned %d on frame %d\n", (int)presented, frame);
                ok = false;
            }
        }
        // Outside every present: the probe's own end-of-scene idle, so the
        // semaphores can be destroyed and the read-back below re-acquire.
        vk.vkDeviceWaitIdle(device);
        for (int k = 0; k < kSlots; ++k) {
            vk.vkDestroySemaphore(device, acquired[k], nullptr);
            vk.vkDestroyFence(device, slot_fence[k], nullptr);
        }
        for (uint32_t k = 0; k < image_count; ++k) {
            vk.vkDestroySemaphore(device, done[k], nullptr);
        }
        return ok;
    };
    // ---- The frame, read back ----------------------------------------------
    // A lambda because the second-presenter scene reads a frame of its second
    // device's swapchain as well, through the same device variables the frame
    // loops use (which that scene points at the second device while it runs).
    // Writes the modal byte and the count of pixels more than 40 away from it;
    // false when no frame could be read.
    // `give_back` presents the image read again afterwards, for a swapchain
    // whose frames go on: an image acquired and never presented is one fewer
    // the loop can acquire.
    const auto read_back = [&](VkSwapchainKHR chain, VkImage* chain_images, int& background,
                               long& foreign, bool give_back) -> bool {
        background = 0;
        foreign = 0;
        // Acquired rather than grabbed: once acquire hands an image back it is ours
        // again and the presentation engine is done with it, so the read is not a
        // race. But acquire says nothing about WHEN the image was last presented:
        // this used to say that every image had carried the overlay by then, "45
        // frames over a handful of images", and the engine is free to keep one
        // image back while it cycles the others. After a hand-over the frames
        // after the build all went to images 1 and 2 and the read-back got image
        // 0, held since the last frame before the new renderer existed: 0
        // foreign pixels, 6 runs in 75 of the hand-over scenes, while the layer
        // had drawn into every one of the owner's presents after "backend
        // ready" (instrumented, 10 runs, 0 missed). An image last
        // presented before `drawn_from` -- or never -- is given a frame first and
        // the acquire asked again; each such frame makes one more image current,
        // so the swapchain's size bounds it.
        uint32_t index = 0;
        for (uint32_t given = 0;; ++given) {
            if (vk.vkAcquireNextImageKHR(device, chain, UINT64_MAX, VK_NULL_HANDLE, fence,
                                         &index) != VK_SUCCESS) {
                printf("FAIL could not acquire an image to read back\n");
                return false;
            }
            vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vk.vkResetFences(device, 1, &fence);
            const long long last = last_presented(chain_images[index]);
            if (last > 0 && last >= drawn_from) {
                break;
            }
            if (given >= kMaxSwapchainImages) {
                printf("FAIL no image of the swapchain was presented after the scene's mark, "
                       "%u frames given\n", given);
                return false;
            }
            printf("     the read-back acquired image %u, %s: given a frame first\n", index,
                   last == 0 ? "never presented"
                             : "last presented before the overlay could draw it");
            const VkResult presented = paint_and_present(chain, chain_images, index);
            if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
                printf("FAIL vkQueuePresentKHR returned %d on the read-back's frame\n",
                       (int)presented);
                return false;
            }
        }
        read_index = index;

        const VkDeviceSize bytes = (VkDeviceSize)extent.width * extent.height * 4;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer readback = VK_NULL_HANDLE;
        vk.vkCreateBuffer(device, &buffer_info, nullptr, &readback);
        VkMemoryRequirements requirements{};
        vk.vkGetBufferMemoryRequirements(device, readback, &requirements);
        VkPhysicalDeviceMemoryProperties mem_props{};
        vk.vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
        const uint32_t type = find_memory_type(
            mem_props, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == UINT32_MAX) {
            printf("FAIL no host-visible memory type for the read-back buffer\n");
            vk.vkDestroyBuffer(device, readback, nullptr);
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        vk.vkAllocateMemory(device, &allocate, nullptr, &memory);
        vk.vkBindBufferMemory(device, readback, memory, 0);

        vk.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &begin);
        image_barrier(cmd, chain_images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = extent.width;
        region.imageExtent.height = extent.height;
        region.imageExtent.depth = 1;
        vk.vkCmdCopyImageToBuffer(cmd, chain_images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                                  &region);
        image_barrier(cmd, chain_images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
        vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vk.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        vk.vkQueueWaitIdle(queue);

        void* mapped_memory = nullptr;
        vk.vkMapMemory(device, memory, 0, bytes, 0, &mapped_memory);
        const unsigned char* pixels = static_cast<const unsigned char*>(mapped_memory);

        // The background, measured rather than derived: the modal byte of the first
        // channel is the grey this test cleared to, whatever the format's encoding
        // did to it.
        long histogram[256] = {0};
        const long total = (long)extent.width * extent.height;
        for (long i = 0; i < total; ++i) {
            ++histogram[pixels[i * 4]];
        }
        for (int value = 1; value < 256; ++value) {
            if (histogram[value] > histogram[background]) {
                background = value;
            }
        }
        for (long i = 0; i < total; ++i) {
            const int r = pixels[i * 4 + 0];
            const int g = pixels[i * 4 + 1];
            const int b = pixels[i * 4 + 2];
            const int dr = r > background ? r - background : background - r;
            const int dg = g > background ? g - background : background - g;
            const int db = b > background ? b - background : background - b;
            if (dr > 40 || dg > 40 || db > 40) {
                ++foreign;
            }
        }
        vk.vkUnmapMemory(device, memory);
        vk.vkFreeMemory(device, memory, nullptr);
        vk.vkDestroyBuffer(device, readback, nullptr);
        if (give_back) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.swapchainCount = 1;
            present.pSwapchains = &chain;
            present.pImageIndices = &index;
            vk.vkQueuePresentKHR(queue, &present);
            vk.vkQueueWaitIdle(queue);
            note_presented(chain_images[index]);
            if (index < kMaxSwapchainImages) {
                image_queue[index] = queue == queue_alternate ? 1 : 0;
            }
        }
        return true;
    };
    if (early_exit) {
        // One frame with a channel on it, which starts the atlas worker (entry
        // 192). Then the game
        // goes away at once -- device, swapchain, instance, process -- inside
        // the ~113 ms the build takes. vocem_DestroyDevice has to wait for the
        // worker before tearing the context down, and the layer's destructor
        // before the library can be unmapped; a crash, a hang or a non-zero
        // exit here is the failure. It cannot fail against a layer that has no
        // worker, which is said rather than implied: this is a guard on the
        // hazard the worker brings, not a refutation of anything older.
        //
        // ONE frame, and it was two: the second was a race between the build
        // and the frame clock, and on the OpenGL twin of this scene it lost
        // under load (the suite at -j16), so the atlas was up before the
        // teardown and the scene measured nothing (DESIGN 193). The worker is
        // started by the first present (measured, 3 of 3), and with no second
        // there is none for the renderer to finish on.
        if (!run_frames(swapchain, images, 1)) {
            return 1;
        }
        vk.vkDeviceWaitIdle(device);
        vk.vkDestroyFence(device, fence, nullptr);
        vk.vkDestroyCommandPool(device, pool, nullptr);
        {
            const long long destroy_from = now_ns();
            vk.vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (g_destroy_count < 16) {
                g_destroys[g_destroy_count++] = {destroy_from, now_ns(), 0};
            }
        }
        vk.vkDestroyDevice(device, nullptr);
        vk.vkDestroySurfaceKHR(instance, surface, nullptr);
        vk.vkDestroyInstance(instance, nullptr);
        XCloseDisplay(display);
        fflush(stderr);
        const long started = lines_containing(stderr_log, "off the game's thread");
        const long ready = lines_containing(stderr_log, "backend ready");
        printf("     the layer started the atlas worker %ld time(s), said \"backend ready\" %ld "
               "time(s)\n", started, ready);
        check(started == 1, "the atlas worker was running when the game went away");
        check(ready == 0, "and the renderer was never finished, so the teardown met it mid-build");
        // And what it left behind: entry 211's measurement, in the one window
        // entry 211 did not cover. The last vkDestroyInstance handed the atlas
        // back only where fonts_build_count() was above zero, and the count
        // moves when Build() RETURNS -- so an instance destroyed inside the
        // first build's ~113 ms skipped the teardown, the worker finished the
        // build for nobody, and the atlas and the ImGui context stayed mapped
        // after the loader had unloaded the only library that knew of them.
        const size_t mapped_kb = mallinfo2().hblkhd / 1024;
        printf("     heap blocks still mapped after the last instance: %zu kB\n", mapped_kb);
        check(mapped_kb < 1024,
              "an instance destroyed while the first atlas was being rasterised hands it back "
              "all the same");
        char cleanup[800];
        snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
        if (system(cleanup) != 0) {
            printf("     (the scratch root %s outlived the test)\n", root);
        }
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }
    // A second of plain frames first. The first atlas is rasterised on a worker
    // now (entry 192), so the panel appears ~113 ms after the first frame with
    // a channel on it, and longer on a loaded machine; the 45 frames below are
    // about 250 ms, which was twice that alone and not always enough inside
    // the full suite (vk_inside_gamescope failed on it once, passed alone).
    // Before the scene's own frames and not after them: the in-flight scene
    // chains its 45 through semaphores it creates per call, and splitting that
    // call made the witness pair presents with a previous call's recycled
    // handles (5 "mismatched", measured). A deadline, not a measurement.
    //
    // The in-flight scene is the exception: it is the one that checks the
    // frames the overlay passes through untouched while its renderer is still
    // being built -- each must wait on the probe's own semaphore -- so its
    // build has to happen INSIDE its chained frames. One call, long enough to
    // contain the build with room: 300 chained frames.
    if (in_flight) {
        if (!run_frames_in_flight(swapchain, images, 300)) {
            return 1;
        }
    } else {
        // Until the layer says its renderer is up -- or, in the idle scene, that
        // it has nothing to draw -- and then ten frames more, so the frames the
        // scene measures start after the build's own first frames and not on
        // them; never longer than the second this used to be. It was the whole
        // second in every scene, ~20 launches a suite, whatever the layer had
        // done (DESIGN 193). The scenes where the layer never says either line
        // -- disabled (not loaded), flatpak-off (switched off) -- still spend
        // the full second, which in both is the window their zero is counted
        // over.
        const char* ready_line = idle ? "nothing to draw" : "backend ready";
        const long long warm_until = now_ns() + 1000000000LL;
        while (now_ns() < warm_until) {
            if (!run_frames(swapchain, images, 5)) {
                return 1;
            }
            if (lines_containing(layer_log, ready_line) > 0) {
                overlay_from_next_frames();
                if (!run_frames(swapchain, images, 10)) {
                    return 1;
                }
                break;
            }
        }
        g_scene_began = now_ns();
        if (!run_frames(swapchain, images, kFrames)) {
            return 1;
        }
    }

    // ---- The scenarios, before the read-back --------------------------------
    if (second_device) {
        // A helper device, made and unmade beside the presenting one. The
        // overlay's backend lives on `device`; this one has no swapchain, no
        // frame, and nothing of ours should notice it going.
        VkDevice helper = VK_NULL_HANDLE;
        if (vk.vkCreateDevice(gpu, &device_info, nullptr, &helper) != VK_SUCCESS) {
            printf("FAIL the second device could not be created\n");
            return 1;
        }
        auto destroy_helper =
            reinterpret_cast<PFN_vkDestroyDevice>(vk.vkGetDeviceProcAddr(helper, "vkDestroyDevice"));
        auto helper_idle =
            reinterpret_cast<PFN_vkDeviceWaitIdle>(vk.vkGetDeviceProcAddr(helper, "vkDeviceWaitIdle"));
        if (helper_idle) {
            helper_idle(helper);
        }
        if (destroy_helper) {
            destroy_helper(helper, nullptr);
        }
        printf("     a second device came and went; the frames continue\n");
        if (!run_frames(swapchain, images, 15)) {
            return 1;
        }
    }
    // Filled by the second-presenter scene, checked with the others below.
    long second_idle_foreign = -1;
    long second_drawn_foreign = -1;
    if (second_presenter || second_queue) {
        // second-queue is the same scene on ONE device: the second window's
        // swapchain belongs to the first device and is presented from the
        // family's second queue. The texture cache uploads to, and orders the
        // font image's copies on, the queue the renderer was built for, and a
        // present holds no synchronisation for any other queue: a present on
        // queue 1 passed the family check and was drawn with queue 0's
        // uploads behind it.
        //
        // A second device that PRESENTS, beside the first, which stays alive:
        // a launcher window, a game's second adapter, a tool. The overlay's
        // renderer -- its vertex ring, its pipeline, its font image and every
        // face -- belongs to the first device, and nothing compared the two:
        // the second device's command buffers were recorded with the first
        // device's buffers (VUID-vkCmdBindVertexBuffers-commonparent under the
        // validation layer) and the process died of SIGSEGV. Three phases:
        // both present and the second is left alone; the first falls silent
        // and after kHandOverSeconds the overlay moves to the second (entry
        // 210's hand-over, on this side); the second goes away and the first
        // takes the overlay back.
        Window window_b =
            XCreateWindow(display, RootWindow(display, screen), -4000, 400, kWidth, kHeight, 0,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWBackPixel, &attributes);
        XMapWindow(display, window_b);
        XSync(display, False);
        VkXlibSurfaceCreateInfoKHR surface_b_info = surface_info;
        surface_b_info.window = window_b;
        VkSurfaceKHR surface_b = VK_NULL_HANDLE;
        if (vk.vkCreateXlibSurfaceKHR(instance, &surface_b_info, nullptr, &surface_b) !=
            VK_SUCCESS) {
            printf("FAIL the second window's surface\n");
            return 1;
        }
        VkBool32 presentable_b = VK_FALSE;
        vk.vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queue_family, surface_b, &presentable_b);
        VkSurfaceCapabilitiesKHR caps_b{};
        vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface_b, &caps_b);
        VkDevice device_b = second_queue ? device : VK_NULL_HANDLE;
        if (!presentable_b ||
            (!second_queue &&
             vk.vkCreateDevice(gpu, &device_info, nullptr, &device_b) != VK_SUCCESS)) {
            printf("FAIL the second presenting device could not be made\n");
            return 1;
        }
        const Vk table_a = vk;
        const VkDevice device_a = device;
        const VkQueue queue_a = queue;
        const VkCommandBuffer cmd_a = cmd;
        const VkFence fence_a = fence;
        Vk table_b = vk;
#define VOCEM_LOAD_DEVICE_B(name) \
        table_b.name = reinterpret_cast<PFN_##name>(table_a.vkGetDeviceProcAddr(device_b, #name));
        VOCEM_VK_DEVICE_FUNCS(VOCEM_LOAD_DEVICE_B)
#undef VOCEM_LOAD_DEVICE_B
        VkQueue queue_b = VK_NULL_HANDLE;
        table_b.vkGetDeviceQueue(device_b, queue_family, second_queue ? 1 : 0, &queue_b);
        VkSwapchainCreateInfoKHR swap_b = swap_info;
        swap_b.surface = surface_b;
        VkSwapchainKHR swapchain_b = VK_NULL_HANDLE;
        if (table_b.vkCreateSwapchainKHR(device_b, &swap_b, nullptr, &swapchain_b) != VK_SUCCESS) {
            printf("FAIL the second device's swapchain\n");
            return 1;
        }
        uint32_t count_b = 0;
        table_b.vkGetSwapchainImagesKHR(device_b, swapchain_b, &count_b, nullptr);
        VkImage images_b[kMaxSwapchainImages];
        if (count_b > kMaxSwapchainImages) {
            count_b = kMaxSwapchainImages;
        }
        table_b.vkGetSwapchainImagesKHR(device_b, swapchain_b, &count_b, images_b);
        VkCommandPoolCreateInfo pool_b_info = pool_info;
        VkCommandPool pool_b = VK_NULL_HANDLE;
        table_b.vkCreateCommandPool(device_b, &pool_b_info, nullptr, &pool_b);
        VkCommandBufferAllocateInfo cmd_b_info = cmd_info;
        cmd_b_info.commandPool = pool_b;
        VkCommandBuffer cmd_b = VK_NULL_HANDLE;
        table_b.vkAllocateCommandBuffers(device_b, &cmd_b_info, &cmd_b);
        VkFence fence_b = VK_NULL_HANDLE;
        table_b.vkCreateFence(device_b, &fence_info, nullptr, &fence_b);
        // The frame loops and the read-back reach the device through these
        // variables, by reference: pointing them at B is how B's frames go.
        const auto use = [&](bool b) {
            vk = b ? table_b : table_a;
            device = b ? device_b : device_a;
            queue = b ? queue_b : queue_a;
            cmd = b ? cmd_b : cmd_a;
            fence = b ? fence_b : fence_a;
        };
        printf("     a second device presents: %u images, beside the first\n", count_b);

        // Both present, alternately: the overlay stays where its renderer is.
        for (int frame = 0; frame < 30; ++frame) {
            use(false);
            if (!run_frames(swapchain, images, 1)) {
                return 1;
            }
            use(true);
            if (!run_frames(swapchain_b, images_b, 1)) {
                return 1;
            }
        }
        int background_b = 0;
        if (!read_back(swapchain_b, images_b, background_b, second_idle_foreign, true)) {
            return 1;
        }
        printf("     while both present: the second window's frame has %ld foreign pixels\n",
               second_idle_foreign);

        // The first falls silent and stays alive; the second goes on alone.
        // Until the layer says it moved and has built again on the second
        // device, and ten frames more; twelve seconds at most, a deadline and
        // not a measurement. It is 2.0 s on this machine, alone and under the
        // validation layer alike; the first version's five was missed once,
        // under the validation layer beside a parallel run of the Vulkan
        // tests, and the build then landed on the read-back's own present.
        const long ready_before = lines_containing(layer_log, "backend ready");
        const long long alone_from = now_ns();
        const long long alone_until = alone_from + 12000000000LL;
        long long moved_after = -1;
        while (now_ns() < alone_until) {
            if (!run_frames(swapchain_b, images_b, 5)) {
                return 1;
            }
            if (lines_containing(layer_log, "backend ready") > ready_before) {
                moved_after = now_ns() - alone_from;
                overlay_from_next_frames();
                if (!run_frames(swapchain_b, images_b, 10)) {
                    return 1;
                }
                break;
            }
        }
        printf("     the second window alone: built for it %.1f s after the first fell silent\n",
               moved_after < 0 ? -1.0 : static_cast<double>(moved_after) / 1e9);
        if (!read_back(swapchain_b, images_b, background_b, second_drawn_foreign, true)) {
            return 1;
        }
        printf("     the first silent: the second window's frame has %ld foreign pixels\n",
               second_drawn_foreign);

        // The second goes away, and the first presents again.
        vk.vkDeviceWaitIdle(device_b);
        {
            const long long destroy_from = now_ns();
            vk.vkDestroySwapchainKHR(device_b, swapchain_b, nullptr);
            if (g_destroy_count < 16) {
                g_destroys[g_destroy_count++] = {destroy_from, now_ns(), 0};
            }
        }
        forget_presented(images_b, count_b);
        vk.vkDestroyFence(device_b, fence_b, nullptr);
        vk.vkDestroyCommandPool(device_b, pool_b, nullptr);
        if (!second_queue) {
            vk.vkDestroyDevice(device_b, nullptr);
        }
        use(false);
        vk.vkDestroySurfaceKHR(instance, surface_b, nullptr);
        XDestroyWindow(display, window_b);
        printf("     the second device is gone; the first presents again\n");
        // On one device nothing is destroyed with the second swapchain: the
        // renderer stays on queue 1 until queue 0 has presented alone for the
        // hand-over interval and moves it back. Eight seconds at most, a
        // deadline and not a measurement.
        const long ready_again = lines_containing(layer_log, "backend ready");
        const long long back_until = now_ns() + 8000000000LL;
        while (now_ns() < back_until) {
            if (!run_frames(swapchain, images, 5)) {
                return 1;
            }
            if (lines_containing(layer_log, "backend ready") > ready_again) {
                overlay_from_next_frames();
                if (!run_frames(swapchain, images, 10)) {
                    return 1;
                }
                break;
            }
        }
    }
    // Filled by the alternate-queue scene: the fewest foreign pixels in a frame
    // last presented by each queue, -1 for a queue none of the frames read had.
    long alternate_foreign[2] = {-1, -1};
    if (alternate_queue) {
        // ONE swapchain, presented by two queues of the same family on the
        // same device, turn and turn about (the 0.1.11 refutation pass's
        // finding: the owner rule is (device, queue), so every other frame
        // is not the owner's): frames chained two in flight, and
        // people arriving with colour emoji meanwhile, so the font image is
        // copied into while frames of both queues may be sampling it (the
        // arrivals scene's hazard, entry 192, across two queues). The
        // renderer is built on the first queue (the warm-up above). Then
        // every frame read back is attributed to the queue that last
        // presented its image.
        static const char* const kAlternate[] = {
            "Alterna \xF0\x9F\x98\x80", "Alterna \xF0\x9F\x94\xA5",
            "Alterna \xF0\x9F\x8E\xAE", "Alterna \xF0\x9F\x9A\x80"};
        alternate_with = queue_alternate;
        for (int joined = 1; joined <= 4; ++joined) {
            writer.publish([&](vocem::SharedState& state) {
                state.connected = 1;
                state.in_channel = 1;
                state.status = 2;  // Connected
                snprintf(state.channel_name, sizeof(state.channel_name), "present-hook");
                state.user_count = 3 + static_cast<uint32_t>(joined);
                for (uint32_t i = 0; i < 3; ++i) {
                    state.users[i].id = 700 + i;
                    snprintf(state.users[i].name, sizeof(state.users[i].name), "Present %u",
                             i + 1);
                }
                for (int i = 0; i < joined; ++i) {
                    state.users[3 + i].id = 800 + static_cast<uint64_t>(i);
                    snprintf(state.users[3 + i].name, sizeof(state.users[3 + i].name), "%s",
                             kAlternate[i]);
                }
            });
            if (!run_frames_in_flight(swapchain, images, 12)) {
                return 1;
            }
        }
        alternate_with = VK_NULL_HANDLE;
        // Then frames read back: each image acquired is read, then painted
        // afresh and presented by the two queues in turn, so what the next
        // read of it finds is that queue's frame. Until a frame of each queue
        // has been read, a deadline of four rounds of the swapchain.
        const VkQueue first_queue = queue;
        for (uint32_t k = 0; k < 4 * image_count &&
                             (alternate_foreign[0] < 0 || alternate_foreign[1] < 0);
             ++k) {
            int background_k = 0;
            long foreign_k = 0;
            if (!read_back(swapchain, images, background_k, foreign_k, false)) {
                return 1;
            }
            const int which = read_index < kMaxSwapchainImages ? image_queue[read_index] : 0;
            printf("     image %u, last presented by queue %d: %ld foreign pixels\n", read_index,
                   which, foreign_k);
            if (alternate_foreign[which] < 0 || foreign_k < alternate_foreign[which]) {
                alternate_foreign[which] = foreign_k;
            }
            const VkQueue next = (k % 2) == 0 ? queue_alternate : first_queue;
            vk.vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk.vkBeginCommandBuffer(cmd, &begin);
            image_barrier(cmd, images[read_index], VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vk.vkCmdClearColorImage(cmd, images[read_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &grey, 1, &whole);
            image_barrier(cmd, images[read_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            vk.vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            vk.vkQueueSubmit(next, 1, &submit, VK_NULL_HANDLE);
            vk.vkQueueWaitIdle(next);
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &read_index;
            vk.vkQueuePresentKHR(next, &present);
            vk.vkQueueWaitIdle(next);
            note_presented(images[read_index]);
            image_queue[read_index] = next == queue_alternate ? 1 : 0;
        }
    }
    if (recreate) {
        VkSwapchainCreateInfoKHR again = swap_info;
        again.imageFormat = other.format;
        again.imageColorSpace = other.colorSpace;
        again.oldSwapchain = swapchain;
        VkSwapchainKHR replacement = VK_NULL_HANDLE;
        if (vk.vkCreateSwapchainKHR(device, &again, nullptr, &replacement) != VK_SUCCESS) {
            printf("FAIL vkCreateSwapchainKHR for the recreated swapchain (format %d)\n",
                   (int)other.format);
            return 1;
        }
        vk.vkDeviceWaitIdle(device);
        {
            const long long destroy_from = now_ns();
            vk.vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (g_destroy_count < 16) {
                g_destroys[g_destroy_count++] = {destroy_from, now_ns(), 0};
            }
        }
        swapchain = replacement;
        forget_presented(images, image_count);
        image_count = 0;
        vk.vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        if (image_count > kMaxSwapchainImages) {
            image_count = kMaxSwapchainImages;
        }
        vk.vkGetSwapchainImagesKHR(device, swapchain, &image_count, images);
        printf("     swapchain recreated: format %d -> %d, %u images\n", (int)chosen.format,
               (int)other.format, image_count);
        if (!run_frames(swapchain, images, kFrames)) {
            return 1;
        }
    }
    if (daemon_gone) {
        // The tray's Quit, as vocemd performs it: the name goes first, and the
        // mapping this game holds outlives it.
        //
        // Driven with run_frames_in_flight, not run_frames. The idling loop
        // calls vkQueueWaitIdle after every present, so by the time the layer's
        // post-present release executes the GPU is provably finished and
        // destroying the renderer's images, buffers, pipeline and descriptor
        // pool is legal whatever the layer does. That is the shape of a test
        // that cannot see the defect it is pointed at: the release fires on an
        // arbitrary present of a live, presenting device, and the only thing
        // that makes it safe is a wait the layer has to perform itself. Chained
        // frames with two in flight and no idle anywhere is what a game does,
        // and it is what the Khronos layer needs in the chain to have anything
        // to say (vk_present_validated runs this scenario for that reason).
        printf("     >>> the daemon stops: the segment is unlinked\n");
        writer.close();
        vocem::StateWriter::unlink_segment();
        // Until the release, and one second more; four seconds at most, which
        // is what this was as a fixed span. The release comes two to three
        // seconds after the unlink (StatePoll: a one-second poll, then
        // kReleaseAfterSeconds), so the fixed span left one to two seconds
        // after it -- and those are the part that counts: a layer that builds
        // its backend again with no daemon does it there, on the presents after
        // the release. The second after the line is the least the old span ever
        // gave that half.
        const long long until = now_ns() + 4000000000LL;
        long long after_release = 0;
        while (now_ns() < until && (after_release == 0 || now_ns() < after_release)) {
            if (!run_frames_in_flight(swapchain, images, 10)) {
                return 1;
            }
            if (after_release == 0 &&
                lines_containing(layer_log, "releasing the backend and the font atlas") > 0) {
                after_release = now_ns() + 1000000000LL;
            }
        }
        // And the other half of what Quit promises: opening the window again
        // starts the daemon, and a game still running has to take the overlay
        // back by itself. Handing everything back would be a fine way to break
        // that, so it is asked here rather than hoped for -- DESIGN said both
        // halves did this while only the OpenGL one did.
        printf("     >>> the daemon comes back\n");
        if (!writer.open()) {
            printf("FAIL a second daemon could not publish a segment of its own\n");
            return 1;
        }
        writer.publish([](vocem::SharedState& state) {
            state.connected = 1;
            state.in_channel = 1;
            state.status = 2;  // Connected
            state.display_height = 1080;
            snprintf(state.channel_name, sizeof(state.channel_name), "present-hook");
            state.user_count = 3;
            for (uint32_t i = 0; i < 3; ++i) {
                state.users[i].id = 700 + i;
                snprintf(state.users[i].name, sizeof(state.users[i].name), "Ritorno %u", i + 1);
            }
        });
        // Until the second "backend ready" and half a second of frames after it
        // for the read-back to see the panel; three seconds at most, the old
        // fixed span.
        const long long back_until = now_ns() + 3000000000LL;
        long long after_ready = 0;
        while (now_ns() < back_until && (after_ready == 0 || now_ns() < after_ready)) {
            if (!run_frames_in_flight(swapchain, images, 10)) {
                return 1;
            }
            if (after_ready == 0 && lines_containing(layer_log, "backend ready") >= 2) {
                after_ready = now_ns() + 500000000LL;
                overlay_from_next_frames();
            }
        }
    }

    // What each arrival cost the game, in the probe's own present calls: the
    // layer's post-present work runs before vkQueuePresentKHR returns, so the
    // longest call after an event is the hitch a game would feel. PRINTED and
    // not asserted -- a present's wall time here is the GPU's answer as much
    // as ours (entries 145 and 185); what is asserted is the layer's own count.
    double arrival_worst_ms[8] = {};
    int arrival_events = 0;
    if (arrivals) {
        // The first frame with a channel on it is where the renderer and the
        // atlas are built -- the one cost the fold cannot remove. Printed here
        // because nothing else in the suite states it.
        {
            long long worst = 0;
            for (int i = 0; i < g_present_count; ++i) {
                const long long span = g_presents[i].to - g_presents[i].from;
                worst = span > worst ? span : worst;
            }
            printf("     >>> the panel first appears: the longest present took %.1f ms\n",
                   static_cast<double>(worst) / 1.0e6);
        }
        // People join one at a time, each with a colour emoji in the name that
        // nobody in the channel had, and then a message arrives with one more
        // in its sender line -- the owner's report, "when somebody joins the
        // call or a message arrives the game freezes for half a second", as a
        // scene. Each is a codepoint the atlas does not hold yet.
        static const char* const kJoined[] = {
            "Arriva \xF0\x9F\x98\x80",  // grin
            "Arriva \xF0\x9F\x94\xA5",  // fire
            "Arriva \xF0\x9F\x8E\xAE",  // gamepad
            "Arriva \xF0\x9F\x9A\x80",  // rocket
            "Arriva \xF0\x9F\x8D\x95",  // pizza
            "Arriva \xF0\x9F\x90\xB1",  // cat
        };
        constexpr int kJoinedCount = static_cast<int>(sizeof(kJoined) / sizeof(kJoined[0]));
        const auto worst_since = [&](int first) {
            long long worst = 0;
            for (int i = first; i < g_present_count; ++i) {
                const long long span = g_presents[i].to - g_presents[i].from;
                worst = span > worst ? span : worst;
            }
            return static_cast<double>(worst) / 1.0e6;
        };
        for (int joined = 1; joined <= kJoinedCount + 1; ++joined) {
            const bool message = joined > kJoinedCount;
            const int users = message ? kJoinedCount : joined;
            timespec monotonic{};
            clock_gettime(CLOCK_MONOTONIC, &monotonic);
            const double now_seconds =
                static_cast<double>(monotonic.tv_sec) + static_cast<double>(monotonic.tv_nsec) / 1e9;
            writer.publish([&](vocem::SharedState& state) {
                state.connected = 1;
                state.in_channel = 1;
                state.status = 2;  // Connected
                // What vocemd publishes on the owner's machine (a 3840x2160
                // display), so the atlas is the size a game there builds and
                // the printed times are that game's, not a 360-pixel window's.
                state.display_height = 2160;
                snprintf(state.channel_name, sizeof(state.channel_name), "present-hook");
                state.user_count = 3 + static_cast<uint32_t>(users);
                for (uint32_t i = 0; i < 3; ++i) {
                    state.users[i].id = 700 + i;
                    snprintf(state.users[i].name, sizeof(state.users[i].name), "Present %u",
                             i + 1);
                }
                for (int i = 0; i < users; ++i) {
                    state.users[3 + i].id = 800 + static_cast<uint64_t>(i);
                    snprintf(state.users[3 + i].name, sizeof(state.users[3 + i].name), "%s",
                             kJoined[i]);
                }
                if (message) {
                    state.notification.serial = 1;
                    state.notification.user_id = 900;
                    state.notification.received = now_seconds;
                    snprintf(state.notification.title, sizeof(state.notification.title),
                             "Messaggio \xF0\x9F\x93\xA3");  // megaphone
                }
            });
            const int first = g_present_count;
            // Chained, two in flight, no idle anywhere -- a game's shape, and
            // the only one in which the font image is still being sampled when
            // the fold's copy is submitted. The idling loop drains the queue
            // after every present, so a copy with no barrier at all would be
            // just as correct there and the validation layer would have nothing
            // to object to: measured, a mutation that dropped the barrier's
            // dependency on the earlier reads passed under run_frames.
            if (!run_frames_in_flight(swapchain, images, 8)) {
                return 1;
            }
            arrival_worst_ms[arrival_events++] = worst_since(first);
            printf("     >>> %s: the longest present after it took %.1f ms\n",
                   message ? "a message arrives" : "somebody joins", arrival_worst_ms[arrival_events - 1]);
        }
    }

    int background = 0;
    long foreign = 0;
    if (!read_back(swapchain, images, background, foreign, false)) {
        return 1;
    }

    // The count, against what this project has written down about it. The
    // checks below are a FLOOR -- 500 -- and a floor is the right shape, since
    // any deliberate change to what the panel draws moves the number and this
    // probe must not have to be edited for every one of them. What a floor
    // cannot do is notice a move, and one happened: entry 129 recorded 1232,
    // entry 143's quieter idle avatars took it to 733, and DESIGN went on
    // quoting 1232 in four places (entry 165). So the figure is printed beside
    // the measurement, and a run whose count has left it says so in its own
    // output. What WOULD catch a halving is a same-pass comparison, which
    // vk_inside_gamescope and gl_beside_mangohud have and a standalone run does
    // not -- said here rather than left to be assumed.
    constexpr long kRecordedForeign = 733;
    // And the comparison belongs to the scene the figure was taken in. What
    // DESIGN records is the PLAIN panel over this probe's own clear colour;
    // two scenes deliberately draw something else, and both tripped the note on
    // every green run. Measured, all ten scenes: `recreate` clears through the
    // other format and reads background 89 against 25, so "foreign" counts a
    // different thing -- **1775**; `daemon-gone` reads back the frame after a
    // daemon RETURNED, which is a second snapshot's panel -- **1404**. Every
    // other scene reads exactly 733, at both widths. So the note fired four
    // times in every suite run (the two 32-bit twins included) and was wrong
    // four times, which is the failure entry 165 exists to prevent arriving
    // from the other side: a warning that is always there is a warning nobody
    // reads, and the day 733 really moves it says what it has been saying all
    // along. The figure is still PRINTED for every scene -- that costs nothing
    // and is how somebody sees 1775 at all.
    const bool plain_scene = !recreate && !daemon_gone && !arrivals;
    printf("     background byte: %d, foreign pixels: %ld (DESIGN records %ld for the plain "
           "scene)\n", background, foreign, kRecordedForeign);
    if (plain_scene && foreign > 0 &&
        (foreign * 10 < kRecordedForeign * 9 || foreign * 9 > kRecordedForeign * 10)) {
        printf("     note: that is more than a tenth away from the recorded figure. If the "
               "drawing changed on purpose, this constant and DESIGN's entries 129, 130 and "
               "165 move with it.\n");
    }
    if (control) {
        check(foreign == 0,
              "with the layer out of the chain the presented frame is exactly what the game "
              "painted");
    } else if (flatpak_off) {
        check(foreign == 0, "with the master switch off in the bridge's settings nothing is drawn");
    } else if (daemon_gone) {
        // The frame read back is the one after the daemon CAME BACK: the empty
        // frame in between is asserted through the layer's own lines below,
        // because "nothing was drawn" is also what a layer that never loaded
        // looks like (entry 38) and this scenario's whole point is the return.
        check(foreign > 500, "the overlay draws again once a daemon returns");
    } else if (idle) {
        check(foreign == 0, "with nobody in a channel the presented frame is the game's own");
    } else if (in_flight) {
        check(foreign > 500, "the overlay drew into a frame chained through semaphores, two in flight");
    } else if (recreate) {
        check(foreign > 500, "the overlay drew into the RECREATED swapchain, in the other format");
    } else if (second_device) {
        check(foreign > 500, "the overlay still drew after a second device came and went");
    } else if (alternate_queue) {
        const long foreign_said = lines_containing(layer_log, "not drawing on device");
        const long moved = lines_containing(layer_log, "moving the overlay");
        const long ready = lines_containing(layer_log, "backend ready");
        printf("     one swapchain, two queues in turn: the fewest foreign pixels in a frame of "
               "queue 0 is %ld, of queue 1 %ld; \"not drawing on device\" %ld time(s), "
               "\"moving the overlay\" %ld, \"backend ready\" %ld\n",
               alternate_foreign[0], alternate_foreign[1], foreign_said, moved, ready);
        // What is held here is the owner rule, and a flicker with it: the
        // renderer's queue's frames carry the overlay and the other queue's
        // are passed through whole, said once, with no hand-over back and
        // forth (which would rebuild the backend every 2 s). Drawing the
        // other queue's frames too was measured (0.1.11's second round): the
        // flicker goes (1773 pixels in its frames), but the texture cache
        // orders the font image's in-place copies and its rebuild's wait on
        // the renderer's queue alone, so a draw on another queue has no
        // ordering against either -- and the validation layer, clean in
        // every run of this scene as it stands, reported 1-2
        // SYNC-HAZARD-WRITE-AFTER-PRESENT per run of that variant.
        check(alternate_foreign[0] > 500,
              "the frames of the renderer's queue carry the overlay");
        check(alternate_foreign[1] == 0,
              "the other queue's frames are passed through whole, not drawn half");
        check(foreign_said == 1, "the other queue is told once, not per frame");
        check(moved == 0 && ready == 1,
              "and the backend stays where it was built: no hand-over while both present");
    } else if (second_queue) {
        check(second_idle_foreign == 0,
              "while both queues present, the second queue's frame is its own: the renderer "
              "uploads on the first");
        check(second_drawn_foreign > 500,
              "once the first queue fell silent, the overlay moved to the second and drew there");
        check(foreign > 500, "and moved back to the first once it presented alone again");
        const long foreign_said = lines_containing(layer_log, "not drawing on device");
        const long moved = lines_containing(layer_log, "moving the overlay");
        const long ready = lines_containing(layer_log, "backend ready");
        printf("     the layer said \"not drawing on device\" %ld time(s), \"moving the overlay\" "
               "%ld time(s), \"backend ready\" %ld time(s)\n", foreign_said, moved, ready);
        check(foreign_said == 2,
              "each queue was passed through, said once, while the other owned the renderer");
        check(moved == 2, "the overlay moved to the second queue and back");
        check(ready == 3, "built on the first queue, on the second, and on the first again");
    } else if (failed_presenter) {
        // The renderer could not be made on the first device (its texture
        // cache's sampler refused). That failure belongs to the first device:
        // it holds the overlay as a working renderer would, so the second is
        // passed through while both present, and gives it up when it falls
        // silent or is destroyed. It used to be the whole process's: no other
        // device was ever built for, nothing moved, nothing was drawn anywhere
        // (entry 263's defect, on this side).
        const long refused = count_events(witness_report, "sampler-refused", nullptr, nullptr);
        const long declined = lines_containing(layer_log, "texture cache did not come up");
        const long moved = lines_containing(layer_log, "moving the overlay");
        const long ready = lines_containing(layer_log, "backend ready");
        printf("     the witness refused %ld sampler(s); the layer declined %ld time(s), said "
               "\"moving the overlay\" %ld time(s) and \"backend ready\" %ld time(s)\n",
               refused, declined, moved, ready);
        check(refused == 1, "the first device's texture cache was refused (the precondition)");
        check(declined == 1, "the layer declined on the first device, said once");
        check(second_idle_foreign == 0,
              "while both present, the second device's frame is its own: the first holds the "
              "overlay where it could not be made");
        check(second_drawn_foreign > 500,
              "once the first fell silent, the overlay moved to the second device and drew there");
        check(foreign > 500, "and was built on the first again once the second was gone");
        check(moved == 1, "moved once, after the first device fell silent");
        check(ready == 2, "built on the second device, then on the first");
    } else if (second_presenter) {
        check(second_idle_foreign == 0,
              "while both devices present, the second's frame is its own: the renderer lives on "
              "the first");
        check(second_drawn_foreign > 500,
              "once the first fell silent, the overlay moved to the second device and drew there");
        check(foreign > 500, "and came back to the first once the second was gone");
        const long foreign_said = lines_containing(layer_log, "not drawing on device");
        const long moved = lines_containing(layer_log, "moving the overlay");
        const long ready = lines_containing(layer_log, "backend ready");
        printf("     the layer said \"not drawing on device\" %ld time(s), \"moving the overlay\" "
               "%ld time(s), \"backend ready\" %ld time(s)\n", foreign_said, moved, ready);
        check(foreign_said == 1, "the second device was passed through, said once");
        check(moved == 1, "the overlay was moved once, after the first device fell silent");
        check(ready == 3,
              "built on the first device, on the second, and on the first again after the second "
              "was destroyed");
    } else if (no_cache) {
        const long refused = count_events(witness_report, "sampler-refused", nullptr, nullptr);
        const long declined = lines_containing(layer_log, "texture cache did not come up");
        const long ready = lines_containing(layer_log, "backend ready");
        printf("     the witness refused %ld sampler(s); the layer said it declined %ld time(s) and "
               "\"backend ready\" %ld time(s)\n", refused, declined, ready);
        check(refused == 1, "the texture cache's sampler was refused (the scene's precondition)");
        check(declined == 1, "the layer said once that it will not draw without its texture cache");
        check(ready == 0 && foreign == 0,
              "and drew nothing: no backend whose stock font upload allocates a command buffer "
              "the loader never registered");
    } else if (deferred) {
        check(foreign > 500,
              "the overlay drew into a swapchain whose images get their memory at first acquire");
        const long failed = lines_containing(layer_log, "overlay submit failed");
        printf("     the layer said \"overlay submit failed\" %ld time(s)\n", failed);
        check(failed == 0, "and none of its submits failed on the way");
    } else {
        check(foreign > 500, "the overlay drew into a presented swapchain image");
    }

    vk.vkDeviceWaitIdle(device);
    vk.vkDestroyFence(device, fence, nullptr);
    vk.vkDestroyCommandPool(device, pool, nullptr);
    {
        const long long destroy_from = now_ns();
        vk.vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (g_destroy_count < 16) {
            g_destroys[g_destroy_count++] = {destroy_from, now_ns(), 0};
        }
    }
    vk.vkDestroyDevice(device, nullptr);
    vk.vkDestroySurfaceKHR(instance, surface, nullptr);
    vk.vkDestroyInstance(instance, nullptr);
    XCloseDisplay(display);
    // What is left of the overlay once the last instance is gone, and the
    // loader has unloaded the layer: the font atlas is a heap block far over
    // malloc's mmap threshold, so it is counted exactly in the mmapped heap.
    // Measured against the layer before the last instance handed it back: 8197
    // kB in the plain scene (the atlas at this window's size), 16389 kB after
    // daemon-gone rebuilt it at 16 px, 1 kB in the idle scene, which never built
    // one -- the same to the kB on every run. A count and not a clock (entry
    // 211).
    {
        const size_t mapped_kb = mallinfo2().hblkhd / 1024;
        printf("     heap blocks still mapped after the last instance: %zu kB\n", mapped_kb);
        check(mapped_kb < 1024,
              "the font atlas does not outlive the last instance, whose library the loader "
              "unloads with the only pointer to it");
    }

    // ---- What the chain saw -------------------------------------------------
    // Read after the instance is gone, so every line the witness had to write
    // has been written.
    // Filled by the witness block for the arrivals scene: waits inside a present
    // but after the hand-down, the post-present phase. -1 without a witness
    // (the 32-bit twin runs the plain chain).
    long waits_inside_arrivals = -1;
    // And the bank's file calls, read against the same hand-downs: on the
    // present path, or after the hand-down. -1 without a witness.
    long bank_calls_on_path = -1;
    long bank_calls_after = -1;
    if (witness_report[0]) {
        place_hand_downs(witness_report);
        const long long* file_stamps = nullptr;
        const int file_calls = vocem_file_witness_stamps(&file_stamps);
        bank_calls_on_path = 0;
        bank_calls_after = 0;
        for (int i = 0; i < file_calls; ++i) {
            if (on_present_path(file_stamps[i])) {
                ++bank_calls_on_path;
            } else if (inside_a_present(file_stamps[i])) {
                ++bank_calls_after;
            }
        }
        long submits_inside = 0;
        long submits_on_path = 0;
        long waits_inside = 0;
        long waits_on_path = 0;
        const long presents = count_events(witness_report, "present", nullptr, nullptr);
        const long submits =
            count_events(witness_report, "submit", &submits_inside, &submits_on_path);
        const long waits = count_events(witness_report, "wait", &waits_inside, &waits_on_path);
        waits_inside_arrivals = waits_inside - waits_on_path;
        const long signalled =
            count_events(witness_report, "submit-signalled-fence", nullptr, nullptr);
        const long pipelines = count_events(witness_report, "pipeline-created", nullptr, nullptr);
        const long mismatches =
            count_events(witness_report, "pipeline-renderpass-mismatch", nullptr, nullptr);
        printf("     witness: %ld presents; %ld submits, %ld inside a present, %ld of those on the "
               "present path; %ld waits, %ld inside a present, %ld of those on the present path; "
               "%ld submits with a signalled fence; %ld pipelines created; %ld pipeline/render "
               "pass mismatches\n",
               presents, submits, submits_inside, submits_on_path, waits, waits_inside,
               waits_on_path, signalled, pipelines, mismatches);
        check(presents > 0, "the witness saw the presents at all");
        // The idle scenario is the one where the overlay deliberately builds
        // nothing and submits nothing, so these two positive controls are the
        // assertion turned around: there they say the overlay really did stay
        // out, and anywhere else they say the witness really was underneath it.
        if (idle || no_cache) {
            // The pipeline the witness counts is the SWAPCHAIN's, built by
            // build_swapchain_resources whether or not there is anything on the
            // screen -- deliberately, so that the first frame with something on
            // it does not stall. What must not be built is the renderer, and
            // that is asserted by "backend ready" below, where it belongs.
            check(pipelines > 0,
                  "the swapchain's own resources were built, so the witness sat below us");
            check(submits_on_path == 0,
                  "and nothing was submitted, which is what an idle channel should cost");
        } else {
            check(pipelines > 0,
                  "and a pipeline being created, which only the overlay does: it sat BELOW the "
                  "overlay's layer");
            check(submits_on_path > 0,
                  "the overlay's own submit fell on the present path, so the intervals are the "
                  "right instrument");
        }
        if (in_flight) {
            // With frames in flight the layer's wait on its OWN fence is allowed
            // to block, and is the one wait rule 8 allows; what the chain saw
            // is held to that sentence rather than to "no wait at all".
            const ChainReport chain = analyse_chain(witness_report);
            printf("     chain: %ld presents drawn (waiting on the overlay's semaphore), %ld passed "
                   "through (waiting on the probe's), %ld mismatched; on the present path %ld "
                   "waits on the overlay's own fence and %ld on anything else\n",
                   chain.presents_drawn, chain.presents_passed, chain.present_mismatches,
                   chain.own_fence_waits_on_path, chain.foreign_waits_on_path);
            check(chain.presents_drawn > 0,
                  "the witness saw presents the overlay had drawn into and handed down");
            check(chain.present_mismatches == 0,
                  "every drawn present waits on exactly the semaphore the overlay's submit "
                  "signalled, and every pass-through on the probe's own");
            check(chain.foreign_waits_on_path == 0,
                  "the only wait on the present path is vkWaitForFences on a fence the overlay "
                  "itself submitted (rule 8)");
        } else {
            check(waits_on_path == 0,
                  "no queue, device or fence wait ran on the present path (rules 8 and 10): what "
                  "waits, waits after the present was handed down");
        }
        // And no device-wide wait inside the application's own
        // vkDestroySwapchainKHR: the layer tears down its per-swapchain
        // resources there, and a present or a destroy synchronises the one
        // swapchain, not every queue of the device (the waits it may make are
        // on its own fences).
        const long idle_in_destroy =
            events_inside_destroys(witness_report, "wait", "vkDeviceWaitIdle");
        printf("     %d swapchain destroy call(s); vkDeviceWaitIdle inside them: %ld\n",
               g_destroy_count, idle_in_destroy);
        check(g_destroy_count > 0 && idle_in_destroy == 0,
              "no vkDeviceWaitIdle inside vkDestroySwapchainKHR: the swapchain's teardown waits "
              "on its own fences");
        check(signalled == 0, "no fence was handed to vkQueueSubmit already signalled");
        check(mismatches == 0,
              "no pipeline was bound in a render pass it was not built against");
    }
    if (flatpak_off) {
        // What the layer told the daemon. The request is the bridge's whole
        // contract from this side: `pid=` says the bridge was entered at all
        // (the positive control), `drawing=` says whether the daemon will serve
        // this sandbox the channel and the faces or a cleared state.
        char body[2048] = {0};
        if (FILE* request = fopen(request_path, "r")) {
            const size_t read = fread(body, 1, sizeof(body) - 1, request);
            body[read] = '\0';
            fclose(request);
        }
        printf("     the bridge request reads: %s", body[0] ? body : "(no request file)\n");
        check(strstr(body, "pid=") != nullptr,
              "the layer entered the bridge and wrote its request");
        check(strstr(body, "drawing=0\n") != nullptr && strstr(body, "drawing=1") == nullptr,
              "and told the daemon it is NOT drawing: the master switch is off, so the daemon "
              "serves this sandbox a cleared state rather than the channel");
    }
    if (daemon_gone) {
        fflush(stderr);
        // The frame read back is empty either way, so "it drew nothing" proves
        // nothing on its own -- it is what a layer that never loaded looks like
        // too (entry 38). The layer's own line is what says it got as far as
        // having something to hand back.
        const long ready = lines_containing(stderr_log, "backend ready");
        printf("     the layer said \"backend ready\" %ld time(s)\n", ready);
        check(ready == 2,
              "the layer was drawing before the daemon stopped, and built its backend again "
              "for the daemon that came back");
        const long handed = lines_containing(stderr_log, "releasing the backend and the font atlas");
        printf("     the layer released what it held %ld time(s)\n", handed);
        // Two and not three, which is the nothing-to-draw gate asked here: this
        // scenario is the one that spends four seconds with a segment and no
        // channel on it, and a layer that decides `needs_init` from the
        // swapchain and the verdict alone -- everything except whether there is
        // anything on the screen -- says "backend ready" a third time inside
        // that window. The `idle` scene below makes the same claim from zero.
        check(handed == 1,
              "and hands back the backend, the atlas and every face when the daemon stops, "
              "instead of holding them for the life of the game");
    }
    if (idle) {
        fflush(stderr);
        // A daemon publishing, the owner simply not in a voice channel -- the
        // ordinary state of a machine with the tray icon up. The renderer used
        // to be built from the swapchain and the application's verdict alone,
        // which is everything except whether there is anything to draw, so this
        // game paid 133 ms of atlas and 80 MB of pixels for a panel that was
        // never going to appear. The OpenGL path has always returned above
        // ensure_backend() here.
        const long ready = lines_containing(stderr_log, "backend ready");
        printf("     the layer said \"backend ready\" %ld time(s)\n", ready);
        check(ready == 0,
              "with a daemon publishing and nothing on the segment to draw, the renderer is "
              "not built at all");
        check(lines_containing(stderr_log, "nothing to draw") == 1,
              "and the layer says once why it is spending no frames");
    }
    if (arrivals) {
        fflush(stderr);
        // The count that tells the two costs apart, in the layer's own words
        // (entry 191): "rebuilt" is the rasteriser run again, 125-145 ms of
        // CPU; "folded" is the emoji put into space the build had reserved.
        // Against the layer as it stood -- the 0.1.10-5 package is the
        // exemplar -- every arrival is a rebuild and this reads seven.
        const long rebuilt = lines_containing(stderr_log, "font atlas rebuilt");
        const long folded = lines_containing(stderr_log, "folded into the font atlas");
        const long ready = lines_containing(stderr_log, "backend ready");
        printf("     the layer said \"font atlas rebuilt\" %ld time(s), \"folded\" %ld time(s), "
               "\"backend ready\" %ld time(s)\n", rebuilt, folded, ready);
        check(ready == 1, "the renderer was built once, for the first frame with a channel on it");
        check(rebuilt == 0,
              "nobody joining and no message arriving rasterised the font atlas again");
        check(folded >= arrival_events,
              "and every arrival's emoji reached the atlas, folded in (the positive control: a "
              "layer that noticed nothing would also rebuild nothing)");
        // And reached the GPU as the squares it changed, not as the whole atlas:
        // the stock upload is 64 MB between two vkQueueWaitIdle on the game's
        // queue, 36 to 43 ms of every arrival once the rebuild was gone.
        const long whole = lines_containing(stderr_log, "font texture uploaded whole");
        const long in_place = lines_containing(stderr_log, "copied in place");
        printf("     the font texture went up whole %ld time(s), in place %ld time(s)\n", whole,
               in_place);
        check(whole == 1, "the font texture was uploaded whole once, when the renderer was built");
        check(in_place >= arrival_events,
              "and every arrival after that copied only its folded squares into it");
        // And the first atlas was rasterised OFF the game's thread (entry 192):
        // 113 ms of stb_truetype that stood the game still in the frame the
        // panel first appeared. A fallback is allowed where no thread can be
        // had, and says so; on this machine one always can.
        // The faces (entry 184 and the upload's own wait). Ten people with no
        // avatar share one picture on disk; each upload of it is a staging
        // buffer, a copy and a descriptor set, and each used to end in
        // WaitForFences on the game's queue after the present.
        const long faces = lines_containing(stderr_log, "texture] uploaded ");
        const long post_present_waits = waits_inside_arrivals;
        printf("     the default picture was uploaded %ld time(s); the witness saw %ld wait(s) "
               "inside a present after the hand-down\n", faces, post_present_waits);
        check(faces == 1, "ten people with no avatar share one upload of the one picture they share");
        if (post_present_waits >= 0) {
            check(post_present_waits == 0,
                  "and nothing the overlay uploads makes the game's thread wait for the GPU");
        } else {
            printf("     (no witness in this chain, so the waits are not counted here)\n");
        }
        // The bank's file work: none on the present path. Every arrival is a
        // codepoint nobody had shown, and noting one opened the bank, read its
        // sequence table and binary-searched it with preads inside the present
        // -- where overlay_renderer.cpp said the layer took no file work. The
        // positive control is the post-present count: the bank WAS read, after
        // the hand-down, which is where the verdicts and the folds belong.
        if (bank_calls_on_path >= 0) {
            printf("     the emoji bank's file calls: %ld on the present path, %ld after the "
                   "hand-down\n", bank_calls_on_path, bank_calls_after);
            check(bank_calls_after > 0,
                  "the bank was read after the hand-down (the file witness sees the layer's calls)");
            check(bank_calls_on_path == 0,
                  "and not once on the present path: a new emoji costs the present no file work");
        }
        const long off_thread = lines_containing(stderr_log, "off the game's thread");
        const long no_thread = lines_containing(stderr_log, "no thread for the font atlas");
        printf("     the atlas was rasterised off the game's thread %ld time(s), on it %ld\n",
               off_thread, no_thread);
        check(off_thread == 1 && no_thread == 0,
              "the first font atlas was rasterised on a worker, not in the game's frame");
    }
    if (second_device) {
        fflush(stderr);
        const long ready = lines_containing(stderr_log, "backend ready");
        printf("     the layer said \"backend ready\" %ld time(s)\n", ready);
        check(ready == 1,
              "the backend was built once: a helper device's death did not tear it down");
    }

    // VOCEM_VK_KEEP_ROOT=1 keeps the scratch directory -- the witness report
    // and the layer's log above all -- for reading by hand.
    char cleanup[800];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (getenv("VOCEM_VK_KEEP_ROOT")) {
        printf("     (the scratch root %s is kept)\n", root);
    } else if (system(cleanup) != 0) {
        printf("     (the scratch root %s outlived the test)\n", root);
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
