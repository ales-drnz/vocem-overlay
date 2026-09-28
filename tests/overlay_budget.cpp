// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the OpenGL overlay costs the process it draws in, as a budget.
//
// The version comparison of 28 Sep 2026 measured every package from 0.1.0 on
// with a miniature game and found that nothing had ever asked this as a
// question: a process with the overlay drawing held about 130 MB more than the
// same process with VOCEM_DISABLE=1, 40% more since 0.1.6, and nobody had
// chosen that number. This probe makes it one that is chosen. The same binary
// runs three times as its own child, in one private /dev/shm, against one
// published channel:
//
//   prime  overlay on, numbers thrown away: it fills this run's private
//          shader cache (below);
//   off    VOCEM_DISABLE=1, the shim loaded and switched off;
//   on     the overlay drawing.
//
// and the difference on minus off is held under two budgets: resident memory
// and CPU per frame in the steady state.
//
// **The shader cache is primed, and has to be.** The NVIDIA driver keeps the
// heap of its GLSL compiler for the life of the process once it has compiled
// anything. An allocation census of the comparison's probe put 36.3 MB of live
// heap under the two glCompileShader calls in
// ImGui_ImplOpenGL3_CreateDeviceObjects, and a standalone GLX program
// compiling the same two GLSL 130 shaders kept 37.8 MB with an empty cache and
// 0.6 MB with a warm one. The comparison's harness gave every run a fresh
// XDG_CACHE_HOME, so every run was a first launch: 40 of its 130 MB were the
// driver's compiler (129.8 MB cold, 89.5 MB warm, same probe), and 19.7 MB of
// its baseline too. A game pays that once per executable and driver, not every
// run, so what is budgeted here is the warm case, with the cold one printed.
//
// **The display height is fixed at 2160**, as in gl_daemon_gone: it is what
// sizes the atlas (entry 39), and at 2160 the atlas is 4096x2611, whose RGBA
// copy is 43 MB of the total. A budget that followed the machine's display
// would be a different budget on every machine.
//
// **The CPU is the steady state, after the overlay is up.** The comparison's
// "+55 us per frame since 0.1.10" was not a per-frame cost. Its probe began
// timing 300 frames in, 12 to 40 ms at clear-only speed, and from 0.1.10 the
// first atlas is rasterised on a worker (entry 192) that was still running
// then: a second thread alive at frame 300 in every such run, gone within the
// next 50 frames. The worker's CPU and the first whole-atlas upload landed
// inside the timed window instead of in the warm-up. With 20,000 warm-up
// frames the step is gone: on minus off, three runs each, +99/+79/+79 us for
// 0.1.8, +88/+97/+82 for 0.1.10, +75/+83/+93 for 0.1.11. Here the timing
// starts only once the panel is on screen and a second has gone by.
//
// Measured on this machine (RTX 4070 SUPER, driver 615.71.09, 28 Sep 2026):
// the thresholds below say what they were set from.

#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VK_USE_PLATFORM_XLIB_KHR
#include <GL/gl.h>
#include <GL/glx.h>
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "private_shm.h"
#include "probe_alarm.h"
#include "probe_name.h"

#include <string>
#include "vocem/shm.h"

namespace {

// Resident memory the overlay may add to a process, warm shader cache, at a
// 2160-line display. Measured twice at each width: +72,764 and +72,808 kB at
// 64 bits, +73,408 and +73,936 kB at 32 (the atlas's RGBA copy, 4096x2611, is
// 43 MB of it, the driver's upload staging about 13, the fonts, the lookup
// tables and the library the rest). 88 MB sits ~15 MB above that and below
// the 96.6 MB the same probe read with a power-of-two atlas, so the test also
// holds the atlas at the height its glyphs need; a deliberate 64 MB leak in a
// scratch copy of the library read +162,160 kB.
constexpr long kRssBudgetKb = 88 * 1024;

// CPU per frame the overlay may add in the steady state, all threads.
// Measured +78.1 and +74.8 us at 64 bits, +132.0 and +129.1 us at 32, on a
// machine other work was loading; a scratch library spinning half a
// millisecond per present read +571.7 us and failed. The margin is wide on
// purpose: this machine is shared with games and builds, and a budget that
// fails on somebody else's load is noise (entry 31). What it catches is a
// per-frame cost of the size a regression has -- a rebuild, a file read, a
// spin -- which is fractions of a millisecond and up.
constexpr double kCpuBudgetUs = 400.0;

// The same two for the Vulkan layer (VOCEM_BUDGET_API=vk), measured the same
// way: +52,948 and +52,972 kB at 64 bits, +52,240 and +52,096 kB at 32; CPU
// +42 to +80 us at 64 bits, +156 to +173 at 32. 68 MB sits ~15 MB above and
// below the 76.6 MB of a power-of-two atlas; the scratch 64 MB leak built into
// the layer read +142,220 kB.
constexpr long kVkRssBudgetKb = 68 * 1024;
constexpr double kVkCpuBudgetUs = 400.0;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kSteadyFrames = 1500;

unsigned char g_pixels[kWidth * kHeight * 4];
int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

double seconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

double cpu_us(int who) {
    rusage usage{};
    getrusage(who, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1e6 +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
}

long kb_field(const char* path, const char* key) {
    FILE* file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    long value = -1;
    const size_t length = strlen(key);
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, length) == 0) {
            value = atol(line + length);
            break;
        }
    }
    fclose(file);
    return value;
}

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

// Everything in the pbuffer that is not the colour this probe cleared it to.
// Read from the front buffer, which is the frame just presented, overlay
// included. A front buffer foreign in every row is not the overlay, which
// never covers the window: it is one the compositor has not handed back yet
// (measured by the comparison's probe on the first frames), and counts as 0.
long foreign_pixels() {
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    glReadBuffer(GL_BACK);
    long count = 0;
    int rows = 0;
    for (int y = 0; y < kHeight; ++y) {
        bool busy = false;
        for (int x = 0; x < kWidth; ++x) {
            const unsigned char* p = g_pixels + 4 * (static_cast<long>(y) * kWidth + x);
            if (p[0] > 38 || p[1] > 50 || p[2] > 63 || p[0] < 14 || p[1] < 26 || p[2] < 39) {
                ++count;
                busy = true;
            }
        }
        rows += busy ? 1 : 0;
    }
    return rows == kHeight ? 0 : count;
}

struct Memory {
    long rss = 0;
    long pss = 0;
    long anon = 0;
    size_t heap = 0;
    size_t mmapped = 0;
    long threads = 0;
};

// Read once the panel has been up for a second, before the timed frames.
Memory take_memory() {
    Memory memory;
    memory.rss = kb_field("/proc/self/smaps_rollup", "Rss:");
    memory.pss = kb_field("/proc/self/smaps_rollup", "Pss:");
    memory.anon = kb_field("/proc/self/smaps_rollup", "Anonymous:");
    const struct mallinfo2 heap = mallinfo2();
    memory.heap = (heap.arena + heap.hblkhd) / 1024;
    memory.mmapped = heap.hblkhd / 1024;
    memory.threads = kb_field("/proc/self/status", "Threads:");
    // For somebody reading a failure: this child's whole map, beside its log.
    if (const char* keep = getenv("VOCEM_BUDGET_SMAPS")) {
        FILE* in = fopen("/proc/self/smaps", "r");
        FILE* out = fopen(keep, "w");
        char line[4096];
        while (in && out && fgets(line, sizeof(line), in)) {
            fputs(line, out);
        }
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
    }
    return memory;
}

// The one line a child prints, which the parent parses.
[[noreturn]] void report(const char* role, const Memory& memory, double process, double thread,
                         double wall, long drawn, double up_after, const char* renderer,
                         int layer_mapped = -1) {
    printf("measure role=%s rss_kb=%ld pss_kb=%ld anon_kb=%ld heap_kb=%zu mmapped_kb=%zu "
           "threads=%ld cpu_us=%.1f main_cpu_us=%.1f wall_us=%.1f pixels=%ld up_after_s=%.2f "
           "layer_mapped=%d renderer=%s\n",
           role, memory.rss, memory.pss, memory.anon, memory.heap, memory.mmapped,
           memory.threads, process, thread, wall, drawn, up_after, layer_mapped,
           renderer ? renderer : "?");
    fflush(stdout);
    // The numbers are taken; the driver's teardown is not what is measured.
    _exit(0);
}

// One child: a game that presents, reads its own cost, and prints one line.
int measure_gl(const char* role) {
    vocem_test::set_alarm(60, "one measured child");
    // GLX and a window, as most OpenGL games are, and as the comparison's probe
    // was: at 32 bits this machine's EGL answers with Mesa's software
    // rasteriser (measured: "OpenGL ES 3.2 Mesa", 34 threads, 4 ms of CPU per
    // frame), which would budget llvmpipe rather than the overlay. The window
    // is override-redirect and parked off screen: never visible, never focused.
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip no X display\n");
        return 77;
    }
    int visual_attrs[] = {GLX_RGBA,     GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                          GLX_BLUE_SIZE, 8,                None};
    XVisualInfo* visual = glXChooseVisual(display, DefaultScreen(display), visual_attrs);
    if (!visual) {
        printf("skip no double-buffered RGBA GLX visual\n");
        return 77;
    }
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.colormap =
        XCreateColormap(display, RootWindow(display, visual->screen), visual->visual, AllocNone);
    attributes.override_redirect = True;
    const Window window =
        XCreateWindow(display, RootWindow(display, visual->screen), -4000, 0, kWidth, kHeight, 0,
                      visual->depth, InputOutput, visual->visual, CWColormap | CWOverrideRedirect,
                      &attributes);
    XMapWindow(display, window);
    GLXContext context = glXCreateContext(display, visual, nullptr, True);
    if (!context || !glXMakeCurrent(display, window, context)) {
        printf("skip the driver refused a GLX context\n");
        return 77;
    }
    using SwapIntervalEXT = void (*)(Display*, GLXDrawable, int);
    if (const auto interval = reinterpret_cast<SwapIntervalEXT>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")))) {
        interval(display, window, 0);
    }
    glViewport(0, 0, kWidth, kHeight);
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto frame = [&] {
        glClearColor(0.10f, 0.15f, 0.20f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(display, window);
    };

    // Until the panel is up (the overlay) or for as long as it would take
    // (the control), paced like a game; then a second more, because the
    // memory behind a panel is not all there on its first frame
    // (gl_daemon_gone measured 308 MB against 372 a second later at 32 bits).
    const bool control = getenv("VOCEM_DISABLE") != nullptr;
    const double start = seconds();
    double up_after = -1.0;
    for (int n = 0; seconds() < start + 15.0; ++n) {
        frame();
        usleep(2000);
        if (n % 20 == 19) {
            if (!control && foreign_pixels() > 10000) {
                up_after = seconds() - start;
                break;
            }
            if (control && seconds() > start + 1.0) {
                break;
            }
        }
    }
    const double settle = seconds() + 1.0;
    while (seconds() < settle) {
        frame();
        usleep(2000);
    }

    const Memory memory = take_memory();

    // The steady state, unpaced: what a frame costs once nothing is arriving.
    glFinish();
    const double process0 = cpu_us(RUSAGE_SELF);
    const double thread0 = cpu_us(RUSAGE_THREAD);
    const double wall0 = seconds();
    for (int n = 0; n < kSteadyFrames; ++n) {
        frame();
    }
    glFinish();
    const double wall = (seconds() - wall0) * 1e6 / kSteadyFrames;
    const double process = (cpu_us(RUSAGE_SELF) - process0) / kSteadyFrames;
    const double thread = (cpu_us(RUSAGE_THREAD) - thread0) / kSteadyFrames;
    const long drawn = foreign_pixels();
    report(role, memory, process, thread, wall, drawn, up_after, renderer);
}

// The same game in Vulkan: an override-redirect window parked off screen, a
// swapchain, a clear per frame -- the comparison's vkprobe. The layer is the
// build tree's, alone in a scratch VK_IMPLICIT_LAYER_PATH the parent made.
void vk_barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                VkAccessFlags source, VkAccessFlags destination) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

int measure_vk(const char* role) {
    vocem_test::set_alarm(60, "one measured Vulkan child");
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        printf("skip no X display\n");
        return 77;
    }
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.override_redirect = True;
    const Window window =
        XCreateWindow(display, DefaultRootWindow(display), -4000, 0, kWidth, kHeight, 0,
                      CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect, &attributes);
    XMapWindow(display, window);
    XFlush(display);

#define BUDGET_VK(call)                                  \
    do {                                                 \
        if ((call) != VK_SUCCESS) {                      \
            printf("skip %s did not succeed\n", #call); \
            return 77;                                   \
        }                                                \
    } while (0)

    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "overlay_budget";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    VkInstance instance;
    BUDGET_VK(vkCreateInstance(&instance_info, nullptr, &instance));
    VkXlibSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    surface_info.dpy = display;
    surface_info.window = window;
    VkSurfaceKHR surface;
    BUDGET_VK(vkCreateXlibSurfaceKHR(instance, &surface_info, nullptr, &surface));
    uint32_t gpu_count = 8;
    VkPhysicalDevice gpus[8];
    BUDGET_VK(vkEnumeratePhysicalDevices(instance, &gpu_count, gpus));
    if (gpu_count == 0) {
        printf("skip no Vulkan device\n");
        return 77;
    }
    const VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu, &properties);
    uint32_t family_count = 16;
    VkQueueFamilyProperties families[16];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count && family == UINT32_MAX; ++i) {
        VkBool32 presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &presents);
        if (presents && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i;
        }
    }
    if (family == UINT32_MAX) {
        printf("skip no queue family both draws and presents to this window\n");
        return 77;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    VkDevice device;
    BUDGET_VK(vkCreateDevice(gpu, &device_info, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    uint32_t format_count = 64;
    VkSurfaceFormatKHR formats[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, formats);
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            format = formats[i];
        }
    }
    if (format.format != VK_FORMAT_B8G8R8A8_UNORM) {
        printf("skip no B8G8R8A8_UNORM surface format to read back\n");
        return 77;
    }
    uint32_t mode_count = 8;
    VkPresentModeKHR modes[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &mode_count, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < mode_count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            mode = modes[i];
        }
    }
    for (uint32_t i = 0; i < mode_count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            mode = modes[i];
        }
    }
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &capabilities);
    VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchain_info.surface = surface;
    swapchain_info.minImageCount = capabilities.minImageCount < 3 ? 3 : capabilities.minImageCount;
    if (capabilities.maxImageCount && swapchain_info.minImageCount > capabilities.maxImageCount) {
        swapchain_info.minImageCount = capabilities.maxImageCount;
    }
    swapchain_info.imageFormat = format.format;
    swapchain_info.imageColorSpace = format.colorSpace;
    swapchain_info.imageExtent = {static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight)};
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = mode;
    swapchain_info.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    BUDGET_VK(vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain));
    uint32_t image_count = 8;
    VkImage images[8];
    BUDGET_VK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));

    constexpr int kSlots = 2;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool;
    BUDGET_VK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBuffer commands[kSlots + 1];
    VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = pool;
    allocate_info.commandBufferCount = kSlots + 1;
    BUDGET_VK(vkAllocateCommandBuffers(device, &allocate_info, commands));
    VkSemaphore acquired[kSlots];
    VkSemaphore rendered[8];
    VkFence fences[kSlots];
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kSlots; ++i) {
        BUDGET_VK(vkCreateSemaphore(device, &semaphore_info, nullptr, &acquired[i]));
        BUDGET_VK(vkCreateFence(device, &fence_info, nullptr, &fences[i]));
    }
    for (uint32_t i = 0; i < image_count; ++i) {
        BUDGET_VK(vkCreateSemaphore(device, &semaphore_info, nullptr, &rendered[i]));
    }
    // The same colour as the GL child, which the readback below expects.
    const VkClearColorValue clear{{0.10f, 0.15f, 0.20f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    int frame_number = 0;
    const auto frame = [&]() -> bool {
        const int slot = frame_number++ % kSlots;
        vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fences[slot]);
        uint32_t index = 0;
        if (vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[slot], VK_NULL_HANDLE,
                                  &index) != VK_SUCCESS) {
            return false;
        }
        const VkCommandBuffer command = commands[slot];
        vkResetCommandBuffer(command, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command, &begin);
        vk_barrier(command, images[index], VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                             1, &range);
        vk_barrier(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        vkEndCommandBuffer(command);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[slot];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[index];
        if (vkQueueSubmit(queue, 1, &submit, fences[slot]) != VK_SUCCESS) {
            return false;
        }
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        const VkResult presented = vkQueuePresentKHR(queue, &present);
        return presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
    };

    // Until the layer says the atlas went up (the overlay) or a second (the
    // control), paced like a game; then a second more.
    const bool control = getenv("VOCEM_DISABLE") != nullptr;
    const char* log_path = getenv("VOCEM_LOG_FILE");
    const double start = seconds();
    double up_after = -1.0;
    for (int n = 0; seconds() < start + 15.0; ++n) {
        if (!frame()) {
            printf("skip a present failed\n");
            return 77;
        }
        usleep(2000);
        if (n % 20 == 19) {
            if (!control && log_path &&
                lines_containing(log_path, "font texture uploaded whole") > 0) {
                up_after = seconds() - start;
                break;
            }
            if (control && seconds() > start + 1.0) {
                break;
            }
        }
    }
    const double settle = seconds() + 1.0;
    while (seconds() < settle) {
        frame();
        usleep(2000);
    }
    vkDeviceWaitIdle(device);
    const Memory memory = take_memory();

    const double process0 = cpu_us(RUSAGE_SELF);
    const double thread0 = cpu_us(RUSAGE_THREAD);
    const double wall0 = seconds();
    for (int n = 0; n < kSteadyFrames; ++n) {
        frame();
    }
    vkDeviceWaitIdle(device);
    const double wall = (seconds() - wall0) * 1e6 / kSteadyFrames;
    const double process = (cpu_us(RUSAGE_SELF) - process0) / kSteadyFrames;
    const double thread = (cpu_us(RUSAGE_THREAD) - thread0) / kSteadyFrames;

    // Read back an image re-acquired after the last present: what it holds is
    // the last frame presented from it, overlay included (the comparison's
    // vkprobe, 53k foreign pixels with the overlay, 0 without).
    VkFence acquired_fence;
    VkFenceCreateInfo plain_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    BUDGET_VK(vkCreateFence(device, &plain_fence, nullptr, &acquired_fence));
    uint32_t index = 0;
    BUDGET_VK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                    acquired_fence, &index));
    vkWaitForFences(device, 1, &acquired_fence, VK_TRUE, UINT64_MAX);
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer;
    BUDGET_VK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory_properties);
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory_info.allocationSize = requirements.size;
    memory_info.memoryTypeIndex = type;
    VkDeviceMemory buffer_memory;
    BUDGET_VK(vkAllocateMemory(device, &memory_info, nullptr, &buffer_memory));
    vkBindBufferMemory(device, buffer, buffer_memory, 0);
    const VkCommandBuffer command = commands[kSlots];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(command, &begin);
    vk_barrier(command, images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight), 1};
    vkCmdCopyImageToBuffer(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                           1, &region);
    vkEndCommandBuffer(command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    BUDGET_VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    vkQueueWaitIdle(queue);
    unsigned char* mapped = nullptr;
    BUDGET_VK(vkMapMemory(device, buffer_memory, 0, buffer_info.size, 0,
                          reinterpret_cast<void**>(&mapped)));
    // BGRA: the clear is B 51, G 38, R 26 give or take.
    long drawn = 0;
    for (long i = 0; i < static_cast<long>(kWidth) * kHeight; ++i) {
        const int b = mapped[i * 4 + 0];
        const int g = mapped[i * 4 + 1];
        const int r = mapped[i * 4 + 2];
        if (r > 38 || g > 50 || b > 63 || r < 14 || g < 26 || b < 39) {
            ++drawn;
        }
    }
#undef BUDGET_VK
    // Which layer library is in this process, read rather than trusted: the
    // packaged manifest carries the same layer name (vk_present_draw says so).
    int layer_mapped = 0;
    if (const char* wanted = getenv("VOCEM_VK_LIBRARY")) {
        char real[4096];
        const char* name = realpath(wanted, real) ? real : wanted;
        FILE* maps = fopen("/proc/self/maps", "r");
        char line[4096];
        while (maps && fgets(line, sizeof(line), maps)) {
            if (strstr(line, name)) {
                layer_mapped = 1;
                break;
            }
        }
        if (maps) {
            fclose(maps);
        }
    }
    report(role, memory, process, thread, wall, drawn, up_after, properties.deviceName,
           layer_mapped);
}

struct Result {
    bool ok = false;
    long rss = 0;
    long pss = 0;
    long heap = 0;
    long mmapped = 0;
    double cpu = 0;
    double main_cpu = 0;
    long pixels = 0;
    bool layer = false;
};

// Runs this binary as the `role` child and reads its line back.
Result run_child(const char* role, bool disabled, const char* log_path) {
    Result result;
    int out[2];
    if (pipe(out) != 0) {
        return result;
    }
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return result;
    }
    self[n] = '\0';
    const pid_t child = fork();
    if (child == 0) {
        dup2(out[1], 1);
        close(out[0]);
        close(out[1]);
        setenv("VOCEM_BUDGET_ROLE", role, 1);
        setenv("VOCEM_LOG_FILE", log_path, 1);
        if (getenv("VOCEM_BUDGET_KEEP")) {
            const std::string smaps = std::string(log_path) + ".smaps";
            setenv("VOCEM_BUDGET_SMAPS", smaps.c_str(), 1);
        }
        if (disabled) {
            setenv("VOCEM_DISABLE", "1", 1);
        }
        execl(self, self, (char*)nullptr);
        _exit(127);
    }
    close(out[1]);
    std::string text;
    char buffer[4096];
    ssize_t got;
    while ((got = read(out[0], buffer, sizeof(buffer))) > 0) {
        text.append(buffer, static_cast<size_t>(got));
    }
    close(out[0]);
    int status = 0;
    waitpid(child, &status, 0);
    printf("     %s: %s", role, text.empty() ? "(nothing)\n" : text.c_str());
    if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
        result.ok = false;
        result.pixels = -77;
        return result;
    }
    const char* line = strstr(text.c_str(), "measure role=");
    if (!line) {
        return result;
    }
    const auto field = [&](const char* key) -> double {
        const char* at = strstr(line, key);
        return at ? atof(at + strlen(key)) : -1.0;
    };
    result.rss = static_cast<long>(field(" rss_kb="));
    result.pss = static_cast<long>(field(" pss_kb="));
    result.heap = static_cast<long>(field(" heap_kb="));
    result.mmapped = static_cast<long>(field(" mmapped_kb="));
    result.cpu = field(" cpu_us=");
    result.main_cpu = field(" main_cpu_us=");
    result.pixels = static_cast<long>(field(" pixels="));
    result.layer = field(" layer_mapped=") == 1.0;
    result.ok = result.rss > 0;
    return result;
}

}  // namespace

int main() {
    if (const char* role = getenv("VOCEM_BUDGET_ROLE")) {
        // A child of the sandboxed parent below, which published the channel
        // and never touches GL itself. It only reads the segment.
        const char* api = getenv("VOCEM_BUDGET_API");
        return api && strcmp(api, "vk") == 0 ? measure_vk(role) : measure_gl(role);
    }
    const char* api = getenv("VOCEM_BUDGET_API");
    const bool vulkan = api && strcmp(api, "vk") == 0;
    if (vulkan && !getenv("VOCEM_VK_MANIFEST")) {
        printf("skip meant to run with VOCEM_VK_MANIFEST naming the layer to weigh\n");
        return 77;
    }
    if (!vulkan && (!getenv("VOCEM_GL_LIBRARY") || !getenv("VOCEM_SHIM_PRELOADED"))) {
        printf("skip meant to run with the shim preloaded and VOCEM_GL_LIBRARY set\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(170, "three measured children");

    char root[] = "/tmp/vocem-overlay-budget-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/vocem", root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/vocem/config.ini", root);
    const std::string rule = "enabled = true\nshown_apps = " +
                             vocem_test::own_name("vocem_overlay_budget") + "\n";
    if (FILE* file = fopen(path, "w")) {
        fputs(rule.c_str(), file);
        fclose(file);
    }
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0700);
    setenv("XDG_CACHE_HOME", path, 1);
    // The driver's shader cache: this run's own, so "warm" means warmed by the
    // prime child below and nothing else. NVIDIA's variables and Mesa's.
    snprintf(path, sizeof(path), "%s/shaders", root);
    mkdir(path, 0700);
    setenv("__GL_SHADER_DISK_CACHE", "1", 1);
    setenv("__GL_SHADER_DISK_CACHE_PATH", path, 1);
    setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", 1);
    setenv("MESA_SHADER_CACHE_DIR", path, 1);
    unsetenv("MANGOHUD");
    setenv("VOCEM_DEBUG", "1", 1);
    if (vulkan) {
        // The build tree's layer alone in the implicit set: not the packaged
        // one (same layer name), not MangoHud. The dev manifest is gated on
        // VOCEM=1; VOCEM_DISABLE=1 makes the loader refuse it, which is the
        // control. The session's own GL shim has no business in a Vulkan game.
        snprintf(path, sizeof(path), "%s/layers", root);
        mkdir(path, 0700);
        char destination[700];
        snprintf(destination, sizeof(destination), "%s/vocem.json", path);
        FILE* from = fopen(getenv("VOCEM_VK_MANIFEST"), "r");
        FILE* to = fopen(destination, "w");
        char chunk[4096];
        size_t got = 0;
        while (from && to && (got = fread(chunk, 1, sizeof(chunk), from)) > 0) {
            fwrite(chunk, 1, got, to);
        }
        if (from) {
            fclose(from);
        }
        if (to) {
            fclose(to);
        }
        setenv("VK_IMPLICIT_LAYER_PATH", path, 1);
        setenv("VOCEM", "1", 1);
        unsetenv("LD_PRELOAD");
    }

    vocem::StateWriter writer;
    check(writer.open(), "the private state segment opens");
    writer.publish([](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        state.display_height = 2160;
        // A colour emoji in the channel's name, so the bank and a fold are
        // part of what is weighed.
        snprintf(state.channel_name, sizeof(state.channel_name), "budget \xF0\x9F\x8E\xAE");
        state.user_count = 5;
        for (uint32_t i = 0; i < 5; ++i) {
            state.users[i].id = 700 + i;
            snprintf(state.users[i].name, sizeof(state.users[i].name), "Ospite %u", i + 1);
        }
        state.users[1].flags = vocem::kFlagSpeaking;
    });

    char prime_log[700];
    char off_log[700];
    char on_log[700];
    snprintf(prime_log, sizeof(prime_log), "%s/prime.log", root);
    snprintf(off_log, sizeof(off_log), "%s/off.log", root);
    snprintf(on_log, sizeof(on_log), "%s/on.log", root);

    const Result prime = run_child("prime", false, prime_log);
    const Result off = run_child("off", true, off_log);
    const Result on = run_child("on", false, on_log);
    if (prime.pixels == -77 || off.pixels == -77 || on.pixels == -77) {
        printf("skip a child could not draw here\n");
        return 77;
    }
    check(prime.ok && off.ok && on.ok, "all three children measured themselves");
    check(off.pixels == 0, "the control drew nothing (VOCEM_DISABLE=1)");
    check(on.pixels > 10000, "the overlay drew a panel in the measured child");
    if (vulkan) {
        check(on.layer, "and the layer mapped in it is the one VOCEM_VK_LIBRARY names");
        check(lines_containing(on_log, "rasterising the font atlas") == 1 &&
                  lines_containing(on_log, "font atlas rebuilt") == 0,
              "and rasterised its atlas exactly once");
    } else {
        check(lines_containing(on_log, "font atlas built") == 1,
              "and rasterised its atlas exactly once");
    }
    const long rss_budget = vulkan ? kVkRssBudgetKb : kRssBudgetKb;
    const double cpu_budget = vulkan ? kVkCpuBudgetUs : kCpuBudgetUs;

    const long rss = on.rss - off.rss;
    const double cpu = on.cpu - off.cpu;
    printf("     cold shader cache (prime - off): %+ld kB Rss, %+ld kB heap -- printed, not budgeted\n",
           prime.rss - off.rss, prime.heap - off.heap);
    printf("     overlay on - off: %+ld kB Rss, %+ld kB Pss, %+ld kB heap (%+ld kB of it mmapped), "
           "%+.1f us CPU per frame (%+.1f us on the game's thread)\n",
           rss, on.pss - off.pss, on.heap - off.heap, on.mmapped - off.mmapped, cpu,
           on.main_cpu - off.main_cpu);
    char said[256];
    snprintf(said, sizeof(said), "the overlay adds %ld kB of Rss, within the %ld kB budget", rss,
             rss_budget);
    check(rss <= rss_budget, said);
    snprintf(said, sizeof(said),
             "the overlay adds %.1f us of CPU per frame, within the %.0f us budget", cpu,
             cpu_budget);
    check(cpu <= cpu_budget, said);

    writer.close();
    vocem::StateWriter::unlink_segment();
    if (getenv("VOCEM_BUDGET_KEEP")) {
        printf("     logs and maps kept in %s (VOCEM_BUDGET_KEEP)\n", root);
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }
    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        printf("     (the scratch directory %s outlived the test)\n", root);
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
