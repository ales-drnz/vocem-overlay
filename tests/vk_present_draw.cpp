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

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xlib.h>

#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

#include "private_shm.h"
#include "vocem/shm.h"

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
        if (!strstr(line, "libVkLayer") && !strstr(line, "libvocem_vk.so") &&
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

int main() {
    // The control pass is this same binary with the variable set: the loader
    // then refuses the layer on its disable_environment, so nothing of ours is
    // in the chain and the scene must come back empty.
    const bool control = getenv("VOCEM_DISABLE") != nullptr;

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

    alarm(180);

    char root[] = "/tmp/vocem-vk-present-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[700];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    // shown_apps names THIS binary, read off /proc/self/exe rather than written
    // out as a literal. The literal cost a measurement: built at -m32 the
    // executable is vocem_vk_present_draw32, the literal said
    // vocem_vk_present_draw, the detection quite correctly declined a process
    // nobody had asked for -- and a run that reported "the 32-bit Vulkan layer
    // loads and draws nothing" looked exactly like entries 30/33/34's defect
    // until the log was read (`not drawing ...: does not look like a game`).
    char self[4096] = {0};
    if (const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1); n > 0) {
        self[n] = '\0';
    }
    const char* slash = strrchr(self, '/');
    const char* own_name = slash ? slash + 1 : "vocem_vk_present_draw";
    char config[900];
    snprintf(config, sizeof(config), "enabled = true\nshown_apps = %s\n", own_name);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    write_file(path, config);
    setenv("XDG_CONFIG_HOME", root, 1);
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
                if (copy_file(cursor, destination)) {
                    printf("     also in the chain: %s\n", cursor);
                    ++added;
                } else {
                    printf("FAIL could not copy the extra manifest %s\n", cursor);
                    return 1;
                }
            }
            cursor = separator ? separator + 1 : nullptr;
        }
    }
    setenv("VK_IMPLICIT_LAYER_PATH", path, 1);
    // The dev manifest is gated on VOCEM=1 (enable_environment). Where
    // VOCEM_DISABLE is also set the loader refuses the layer regardless, which
    // is the control.
    setenv("VOCEM", "1", 1);
    // Not a hint: MangoHud's layer reads this and would draw into the frame this
    // test counts.
    unsetenv("MANGOHUD");
    setenv("VOCEM_DEBUG", "1", 1);

    // ---- The channel, published by this process -----------------------------
    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        snprintf(state.channel_name, sizeof(state.channel_name), "present-hook");
        state.user_count = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            state.users[i].id = 700 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Present %u", i + 1);
        }
    });
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

    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vocem_vk_present_draw";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = 2;
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
    // being 40 away from its background. 1232 pixels against 652, and neither
    // number was wrong: they were two formats, and the comparison was measuring
    // the format rather than gamescope. Holding the format fixed is what makes
    // the counts comparable across runs; the sRGB path itself has its own test
    // (hdr_srgb_pixels), and gamescope users exercise it by default, which is
    // worth knowing on its own.
    VkSurfaceFormatKHR chosen{};
    bool have_format = false;
    for (int pass = 0; pass < 2 && !have_format; ++pass) {
        for (uint32_t i = 0; i < format_count && !have_format; ++i) {
            const bool unorm = formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
                               formats[i].format == VK_FORMAT_R8G8B8A8_UNORM;
            const bool srgb = formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
                              formats[i].format == VK_FORMAT_R8G8B8A8_SRGB;
            if ((pass == 0 && unorm) || (pass == 1 && srgb)) {
                chosen = formats[i];
                have_format = true;
            }
        }
    }
    if (!have_format) {
        skip("the surface offers no 8-bit-per-channel format to read back");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
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
    // (vocem_layer.cpp:537); asking for it here keeps the probe honest about
    // what a game requests. TRANSFER_SRC is this test's own, for the read-back.
    swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (vk.vkCreateSwapchainKHR(device, &swap_info, nullptr, &swapchain) != VK_SUCCESS) {
        printf("FAIL vkCreateSwapchainKHR\n");
        return 1;
    }
    uint32_t image_count = 0;
    vk.vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    VkImage images[8];
    if (image_count > 8) {
        image_count = 8;
    }
    vk.vkGetSwapchainImagesKHR(device, swapchain, &image_count, images);
    printf("     swapchain: %ux%u, format %d, %u images\n", extent.width, extent.height,
           (int)chosen.format, image_count);

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

    // ---- The frames --------------------------------------------------------
    // 45 of them: the overlay skips its first thirty while ImGui sizes itself,
    // and a few more make the count independent of that detail. No semaphores --
    // acquire waits on a fence and the queue is drained between frames, so the
    // ordering this test needs is the simplest kind that is still correct.
    for (int frame = 0; frame < kFrames; ++frame) {
        uint32_t index = 0;
        const VkResult acquired =
            vk.vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, fence, &index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            printf("FAIL vkAcquireNextImageKHR returned %d on frame %d\n", (int)acquired, frame);
            return 1;
        }
        vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vk.vkResetFences(device, 1, &fence);

        vk.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &begin);
        // UNDEFINED as the old layout: the previous contents are of no interest
        // and discarding them is what a game clearing its frame does.
        image_barrier(cmd, images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vk.vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1,
                                &whole);
        image_barrier(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        const VkResult presented = vk.vkQueuePresentKHR(queue, &present);
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
            printf("FAIL vkQueuePresentKHR returned %d on frame %d\n", (int)presented, frame);
            return 1;
        }
        vk.vkQueueWaitIdle(queue);
        usleep(4000);
    }

    // ---- The frame, read back ----------------------------------------------
    // Acquired rather than grabbed: once acquire hands an image back it is ours
    // again and the presentation engine is done with it, so the read is not a
    // race. Every image in the swapchain has carried the overlay by now -- 45
    // frames over a handful of images -- so whichever comes back is a presented,
    // overlaid frame.
    uint32_t index = 0;
    if (vk.vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, fence, &index) !=
        VK_SUCCESS) {
        printf("FAIL could not acquire an image to read back\n");
        return 1;
    }
    vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vk.vkResetFences(device, 1, &fence);

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
        return 1;
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
    image_barrier(cmd, images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = extent.width;
    region.imageExtent.height = extent.height;
    region.imageExtent.depth = 1;
    vk.vkCmdCopyImageToBuffer(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                              &region);
    image_barrier(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
    int background = 0;
    for (int value = 1; value < 256; ++value) {
        if (histogram[value] > histogram[background]) {
            background = value;
        }
    }
    long foreign = 0;
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

    printf("     background byte: %d, foreign pixels: %ld\n", background, foreign);
    if (control) {
        check(foreign == 0,
              "with the layer out of the chain the presented frame is exactly what the game "
              "painted");
    } else {
        check(foreign > 500, "the overlay drew into a presented swapchain image");
    }

    vk.vkDeviceWaitIdle(device);
    vk.vkFreeMemory(device, memory, nullptr);
    vk.vkDestroyBuffer(device, readback, nullptr);
    vk.vkDestroyFence(device, fence, nullptr);
    vk.vkDestroyCommandPool(device, pool, nullptr);
    vk.vkDestroySwapchainKHR(device, swapchain, nullptr);
    vk.vkDestroyDevice(device, nullptr);
    vk.vkDestroySurfaceKHR(instance, surface, nullptr);
    vk.vkDestroyInstance(instance, nullptr);
    XCloseDisplay(display);

    char cleanup[800];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        printf("     (the scratch root %s outlived the test)\n", root);
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
