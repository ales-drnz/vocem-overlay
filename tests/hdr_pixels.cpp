// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The HDR pipelines executed on the GPU, pixels read back and compared.
//
// A previous verification held the shader ARITHMETIC: hdr.frag's formulas were
// replicated line for line and checked against an independently derived
// ST 2084 EOTF inverted by bisection, and hdr_shaders.h was shown byte-equal
// to recompiling hdr.frag. What no test had measured was GPU EXECUTION of the
// shipped SPIR-V through the project's own pipeline-creation path -- the
// specialization-constant plumbing, the vertex interface, the blend state --
// which is the part the arithmetic proof cannot reach. This test builds the
// pipelines with hdr_pipeline_create (the exact code the layer runs, compiled
// in, not copied), draws a quad of known sRGB colours to an offscreen
// R16G16B16A16_SFLOAT target on a surfaceless device, and reads the pixels
// back against references recomputed here from the standard constants
// (SMPTE ST 2084, IEC 61966-2-1, BT.2087).
//
// The references are recomputed in double precision from the raw fractions
// and asserted to 1e-6 against hardcoded anchors, so a typo in either copy
// cannot pass. That gate has already earned its keep: an earlier hand-derived
// anchor claimed PQ of the 43.45-nit mid-grey was 0.44607861, which is in
// fact PQ of 53.1 nits -- the recomputation refused it (correct: 0.42701897).
//
// No window, no WSI, no display: instance without extensions, first physical
// device with a graphics queue. Where no loader or no device exists (headless
// CI), the test reports itself skipped rather than passing vacuously.
//
// Tolerance for GPU output vs reference: 1e-3 absolute plus 1e-3 relative.
// The target is 16-bit float, whose storage quantisation (ulp 2^-11 relative,
// so at most ~0.05%) dominates every other error term; the fp32 shader math
// itself agrees with the double reference to ~1e-6.

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hdr_pipeline.h"
#include "imgui.h"

namespace {

// The target this runs against, chosen at build time so one source answers two
// different questions with the same machinery (tests/CMakeLists.txt builds both):
//
//   * the float target measures what the shader PUTS OUT -- the arithmetic and
//     the specialization plumbing, with nothing between the shader and the
//     readback;
//   * the sRGB target measures the ROUND TRIP -- there the format itself carries
//     the encoding, the hardware applies linear->sRGB to whatever is written,
//     and mode 3 exists precisely so that what comes back out is what ImGui put
//     in. Against the shader before mode 3 existed the panel's 79,84,92 came
//     back 151,155,162.
#ifdef VOCEM_SRGB_TARGET
constexpr VkFormat kTargetFormat = VK_FORMAT_R8G8B8A8_SRGB;
#else
constexpr VkFormat kTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#endif

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

[[noreturn]] void die(const char* what) {
    printf("FAIL %s\n", what);
    exit(1);
}

// ----- Reference math, double precision, from the raw standard fractions ----

double srgb_to_linear(double c) {  // IEC 61966-2-1, the piecewise EOTF.
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double pq_of_nits(double nits) {  // SMPTE ST 2084 inverse EOTF.
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 4096.0 * 32.0;
    const double c3 = 2392.0 / 4096.0 * 32.0;
    double y = nits / 10000.0;
    y = y < 0.0 ? 0.0 : (y > 1.0 ? 1.0 : y);
    const double ym = std::pow(y, m1);
    return std::pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

void bt709_to_bt2020(const double in[3], double out[3]) {  // BT.2087.
    out[0] = 0.6274 * in[0] + 0.3293 * in[1] + 0.0433 * in[2];
    out[1] = 0.0691 * in[0] + 0.9195 * in[1] + 0.0114 * in[2];
    out[2] = 0.0164 * in[0] + 0.0880 * in[1] + 0.8956 * in[2];
}

// What hdr.frag promises for one sRGB colour, per mode.
void reference(int mode, double nits, const double srgb[3], double out[3]) {
    double lin[3] = {srgb_to_linear(srgb[0]), srgb_to_linear(srgb[1]), srgb_to_linear(srgb[2])};
    if (mode == 3) {  // an sRGB-format attachment: hand over linear, primaries stay.
        for (int i = 0; i < 3; ++i) out[i] = lin[i];
    } else if (mode == 1) {  // scRGB: linear, 1.0 = 80 nits, primaries stay BT.709.
        for (int i = 0; i < 3; ++i) out[i] = lin[i] * nits / 80.0;
    } else {  // HDR10: BT.2020 primaries, PQ transfer.
        double wide[3];
        bt709_to_bt2020(lin, wide);
        for (int i = 0; i < 3; ++i) out[i] = pq_of_nits(wide[i] * nits);
    }
}

float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1fu;
    const uint32_t man = h & 0x3ffu;
    float v;
    if (exp == 0) {
        v = std::ldexp(static_cast<float>(man), -24);
    } else if (exp == 31) {
        v = man ? NAN : INFINITY;
    } else {
        v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
    }
    return sign ? -v : v;
}

// ----- Vulkan, loaded at runtime so a machine without a loader skips --------

#define VOCEM_VK_FUNCS(X)               \
    X(vkEnumeratePhysicalDevices)       \
    X(vkGetPhysicalDeviceProperties)    \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice)                   \
    X(vkDestroyInstance)                \
    X(vkGetDeviceProcAddr)

#define VOCEM_VK_DEV_FUNCS(X)      \
    X(vkDestroyDevice)             \
    X(vkGetDeviceQueue)            \
    X(vkQueueSubmit)               \
    X(vkQueueWaitIdle)             \
    X(vkCreateShaderModule)        \
    X(vkDestroyShaderModule)       \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout)\
    X(vkCreatePipelineLayout)      \
    X(vkDestroyPipelineLayout)     \
    X(vkCreateGraphicsPipelines)   \
    X(vkDestroyPipeline)           \
    X(vkCreateRenderPass)          \
    X(vkDestroyRenderPass)         \
    X(vkCreateImage)               \
    X(vkDestroyImage)              \
    X(vkCreateImageView)           \
    X(vkDestroyImageView)          \
    X(vkCreateFramebuffer)         \
    X(vkDestroyFramebuffer)        \
    X(vkCreateBuffer)              \
    X(vkDestroyBuffer)             \
    X(vkAllocateMemory)            \
    X(vkFreeMemory)                \
    X(vkBindImageMemory)           \
    X(vkBindBufferMemory)          \
    X(vkMapMemory)                 \
    X(vkGetImageMemoryRequirements)\
    X(vkGetBufferMemoryRequirements)\
    X(vkCreateSampler)             \
    X(vkDestroySampler)            \
    X(vkCreateDescriptorPool)      \
    X(vkDestroyDescriptorPool)     \
    X(vkAllocateDescriptorSets)    \
    X(vkUpdateDescriptorSets)      \
    X(vkCreateCommandPool)         \
    X(vkDestroyCommandPool)        \
    X(vkAllocateCommandBuffers)    \
    X(vkResetCommandBuffer)        \
    X(vkBeginCommandBuffer)        \
    X(vkEndCommandBuffer)          \
    X(vkCmdPipelineBarrier)        \
    X(vkCmdCopyBufferToImage)      \
    X(vkCmdCopyImageToBuffer)      \
    X(vkCmdBeginRenderPass)        \
    X(vkCmdEndRenderPass)          \
    X(vkCmdBindPipeline)           \
    X(vkCmdBindDescriptorSets)     \
    X(vkCmdBindVertexBuffers)      \
    X(vkCmdBindIndexBuffer)        \
    X(vkCmdPushConstants)          \
    X(vkCmdSetViewport)            \
    X(vkCmdSetScissor)             \
    X(vkCmdDrawIndexed)

struct Vk {
#define VOCEM_DECLARE(name) PFN_##name name = nullptr;
    VOCEM_VK_FUNCS(VOCEM_DECLARE)
    VOCEM_VK_DEV_FUNCS(VOCEM_DECLARE)
#undef VOCEM_DECLARE
};

Vk vk;

uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& props, uint32_t type_bits,
                          VkMemoryPropertyFlags wanted) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    die("no suitable memory type");
}

// The target: three 16-pixel bands, one per test colour, sampled at centre.
constexpr uint32_t kWidth = 48;
constexpr uint32_t kHeight = 16;
constexpr uint32_t kBand = 16;

}  // namespace

int main() {
    // The env plumbing, proven once: hdr_sdr_nits() reads VOCEM_HDR_NITS on
    // its first call and caches (hdr_pipeline.cpp:13-17), so the variable must
    // be set before that call -- exactly the constraint the layer lives under.
    // The per-nits pipelines below take sdr_nits as the parameter the layer
    // passes (vocem_layer.cpp:742: hdr_pipeline_create(..., hdr_sdr_nits(),..)),
    // which is what lets one process measure several white levels.
    setenv("VOCEM_HDR_NITS", "100", 1);
    check(vocem::hdr_sdr_nits() == 100.0f,
          "VOCEM_HDR_NITS set before the first ask reaches hdr_sdr_nits()");

    // Keep the installed implicit layers out of the instrument: this test IS
    // the vocem code under test (statically compiled in), and MangoHud has no
    // business in the dispatch chain of a measurement.
    setenv("VOCEM_DISABLE", "1", 1);
    unsetenv("MANGOHUD");

    // ---- Anchors: the recomputed references against hardcoded values ------
    check(std::fabs(pq_of_nits(203.0) - 0.5806889) < 1e-6,
          "anchor: PQ code of 203 nits = 0.5806889");
    check(std::fabs(pq_of_nits(100.0) - 0.5080784215) < 1e-6,
          "anchor: PQ code of 100 nits = 0.5080784215");
    check(std::fabs(pq_of_nits(1000.0) - 0.7518270962) < 1e-6,
          "anchor: PQ code of 1000 nits = 0.7518270962");
    check(std::fabs(srgb_to_linear(0.5) - 0.21404114) < 1e-6,
          "anchor: sRGB 0.5 decodes to linear 0.21404114");
    check(std::fabs(srgb_to_linear(0.5) * 203.0 / 80.0 - 0.54313) < 1e-6,
          "anchor: mid-grey at 203 nits on scRGB = 0.54313");
    // 0.42701897, not the hand-derived 0.44607861 this gate refused: that one
    // is PQ of 53.1 nits, and catching it is what the gate is for.
    check(std::fabs(pq_of_nits(srgb_to_linear(0.5) * 203.0) - 0.42701897) < 1e-6,
          "anchor: mid-grey at 203 nits on PQ = 0.42701897 (43.45 nits)");
    if (failures) {
        printf("reference math refused; not asking the GPU\n");
        return 1;
    }

    // ---- Loader, instance, device: skip politely where there is none ------
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) skip("no Vulkan loader (libvulkan.so.1)");
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!gipa) skip("loader has no vkGetInstanceProcAddr");
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create_instance) skip("loader has no vkCreateInstance");

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vocem_hdr_pixels";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        skip("vkCreateInstance failed (no usable Vulkan here)");
    }
#define VOCEM_LOAD_INSTANCE(name) \
    vk.name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); \
    if (!vk.name) skip("instance function missing: " #name);
    VOCEM_VK_FUNCS(VOCEM_LOAD_INSTANCE)
#undef VOCEM_LOAD_INSTANCE

    uint32_t gpu_count = 0;
    vk.vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (gpu_count == 0) skip("no physical device (headless CI has no GPU)");
    VkPhysicalDevice gpus[16];
    if (gpu_count > 16) gpu_count = 16;
    vk.vkEnumeratePhysicalDevices(instance, &gpu_count, gpus);

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    for (uint32_t i = 0; i < gpu_count && gpu == VK_NULL_HANDLE; ++i) {
        uint32_t family_count = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, nullptr);
        VkQueueFamilyProperties families[32];
        if (family_count > 32) family_count = 32;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, families);
        for (uint32_t f = 0; f < family_count; ++f) {
            if (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gpu = gpus[i];
                queue_family = f;
                break;
            }
        }
    }
    if (gpu == VK_NULL_HANDLE) skip("no device with a graphics queue");

    VkPhysicalDeviceProperties gpu_props{};
    vk.vkGetPhysicalDeviceProperties(gpu, &gpu_props);
    printf("device: %s\n", gpu_props.deviceName);

    // R16G16B16A16_SFLOAT colour attachment with blending is mandated by the
    // spec, but a measurement does not lean on "mandated": ask.
    VkFormatProperties fmt_props{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu, kTargetFormat, &fmt_props);
    if (!(fmt_props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
        skip("device cannot blend into the target format");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    if (vk.vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS) {
        skip("vkCreateDevice failed");
    }
#define VOCEM_LOAD_DEVICE(name) \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device, #name)); \
    if (!vk.name) die("device function missing: " #name);
    VOCEM_VK_DEV_FUNCS(VOCEM_LOAD_DEVICE)
#undef VOCEM_LOAD_DEVICE

    // From here on the device exists: a failure is a failure, never a skip.
    VkQueue queue = VK_NULL_HANDLE;
    vk.vkGetDeviceQueue(device, queue_family, 0, &queue);
    VkPhysicalDeviceMemoryProperties mem_props{};
    vk.vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);

    // ---- Render pass, mirroring the layer's (vocem_layer.cpp) except that
    // the target is cleared (nothing underneath) and ends TRANSFER_SRC ------
    VkAttachmentDescription attachment{};
    attachment.format = kTargetFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 2;
    rp_info.pDependencies = deps;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (vk.vkCreateRenderPass(device, &rp_info, nullptr, &render_pass) != VK_SUCCESS) {
        die("render pass creation");
    }

    // ---- The device functions handed to the code under test, resolved the
    // way the layer resolves its own: through GetDeviceProcAddr -------------
    vocem::HdrDeviceFunctions fn;
    fn.CreateShaderModule = vk.vkCreateShaderModule;
    fn.DestroyShaderModule = vk.vkDestroyShaderModule;
    fn.CreateDescriptorSetLayout = vk.vkCreateDescriptorSetLayout;
    fn.DestroyDescriptorSetLayout = vk.vkDestroyDescriptorSetLayout;
    fn.CreatePipelineLayout = vk.vkCreatePipelineLayout;
    fn.DestroyPipelineLayout = vk.vkDestroyPipelineLayout;
    fn.CreateGraphicsPipelines = vk.vkCreateGraphicsPipelines;
    fn.DestroyPipeline = vk.vkDestroyPipeline;

    // ---- The degrade path: an unrecognised space routes to the identity ----
    // Mode 0 is unconverted colours, and since entry 131 it is a pipeline of
    // the swapchain's own rather than "keep the stock one": the stock pipeline
    // is compatible with the first swapchain's render pass alone, and a later
    // swapchain in another format needs one built against its own.
    check(vocem::hdr_mode_for(VK_COLOR_SPACE_DOLBYVISION_EXT) == 0,
          "unrecognised colour space routes to mode 0 (unconverted)");
    vocem::HdrPipeline identity;
    check(vocem::hdr_pipeline_create(fn, device, render_pass, 0, 203.0f, identity) &&
              identity.pipeline != VK_NULL_HANDLE,
          "mode 0 builds the identity pipeline, against this render pass");
    vocem::hdr_pipeline_destroy(fn, device, identity);
    check(!vocem::hdr_pipeline_create(fn, device, render_pass, 7, 203.0f, identity),
          "and a mode that does not exist builds nothing");

    // ---- Offscreen target ------------------------------------------------
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kTargetFormat;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage target = VK_NULL_HANDLE;
    if (vk.vkCreateImage(device, &image_info, nullptr, &target) != VK_SUCCESS) {
        die("target image creation");
    }
    VkMemoryRequirements reqs{};
    vk.vkGetImageMemoryRequirements(device, target, &reqs);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex =
        find_memory_type(mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    if (vk.vkAllocateMemory(device, &alloc, nullptr, &target_memory) != VK_SUCCESS ||
        vk.vkBindImageMemory(device, target, target_memory, 0) != VK_SUCCESS) {
        die("target memory");
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = target;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kTargetFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView target_view = VK_NULL_HANDLE;
    if (vk.vkCreateImageView(device, &view_info, nullptr, &target_view) != VK_SUCCESS) {
        die("target view");
    }
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &target_view;
    fb_info.width = kWidth;
    fb_info.height = kHeight;
    fb_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vk.vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS) {
        die("framebuffer");
    }

    // ---- Host-visible buffers: quad vertices/indices, upload, readback ----
    auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                           VkDeviceMemory& memory, void** mapped) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        if (vk.vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) die("buffer");
        VkMemoryRequirements r{};
        vk.vkGetBufferMemoryRequirements(device, buffer, &r);
        VkMemoryAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        a.allocationSize = r.size;
        a.memoryTypeIndex = find_memory_type(
            mem_props, r.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vk.vkAllocateMemory(device, &a, nullptr, &memory) != VK_SUCCESS ||
            vk.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
            vk.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
            die("buffer memory");
        }
    };

    // Three bands, three sRGB inputs. Vertex colours are R8G8B8A8_UNORM --
    // exactly the attribute ImGui's draw data carries -- so the mid-grey the
    // GPU actually receives is 128/255, not 0.5; the reference uses 128/255.
    const uint8_t colours[3][4] = {
        {255, 255, 255, 255},  // white
        {128, 128, 128, 255},  // mid-grey (128/255 = 0.50196: what UNORM8 has)
        {255, 0, 0, 255},      // saturated red
    };
    const char* colour_names[3] = {"white", "grey128", "red"};

    ImDrawVert vertices[12];
    ImDrawIdx indices[18];
    for (int band = 0; band < 3; ++band) {
        const float x0 = static_cast<float>(band * kBand);
        const float x1 = static_cast<float>((band + 1) * kBand);
        ImU32 col;
        std::memcpy(&col, colours[band], 4);  // R,G,B,A bytes in memory order
        const ImVec2 corners[4] = {{x0, 0.0f}, {x1, 0.0f}, {x1, kHeight}, {x0, kHeight}};
        for (int v = 0; v < 4; ++v) {
            vertices[band * 4 + v].pos = corners[v];
            vertices[band * 4 + v].uv = ImVec2(0.5f, 0.5f);  // the white texel
            vertices[band * 4 + v].col = col;
        }
        const ImDrawIdx base = static_cast<ImDrawIdx>(band * 4);
        const ImDrawIdx quad[6] = {base, static_cast<ImDrawIdx>(base + 1),
                                   static_cast<ImDrawIdx>(base + 2), base,
                                   static_cast<ImDrawIdx>(base + 2),
                                   static_cast<ImDrawIdx>(base + 3)};
        std::memcpy(indices + band * 6, quad, sizeof(quad));
    }

    VkBuffer vertex_buffer, index_buffer, staging_buffer, readback_buffer;
    VkDeviceMemory vertex_memory, index_memory, staging_memory, readback_memory;
    void* map = nullptr;
    make_buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_memory,
                &map);
    std::memcpy(map, vertices, sizeof(vertices));
    make_buffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_memory,
                &map);
    std::memcpy(map, indices, sizeof(indices));
    make_buffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer, staging_memory, &map);
    std::memset(map, 0xff, 4);  // the 1x1 white texel: sampling is identity
    void* readback = nullptr;
#ifdef VOCEM_SRGB_TARGET
    const VkDeviceSize readback_size = kWidth * kHeight * 4;  // 4 x unorm8
#else
    const VkDeviceSize readback_size = kWidth * kHeight * 8;  // 4 x fp16
#endif
    make_buffer(readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_buffer, readback_memory,
                &readback);

    // ---- The 1x1 white texture, as ImGui's own white pixel ----------------
    VkImageCreateInfo tex_info{};
    tex_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tex_info.imageType = VK_IMAGE_TYPE_2D;
    tex_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    tex_info.extent = {1, 1, 1};
    tex_info.mipLevels = 1;
    tex_info.arrayLayers = 1;
    tex_info.samples = VK_SAMPLE_COUNT_1_BIT;
    tex_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    tex_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    tex_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage texture = VK_NULL_HANDLE;
    if (vk.vkCreateImage(device, &tex_info, nullptr, &texture) != VK_SUCCESS) die("texture");
    vk.vkGetImageMemoryRequirements(device, texture, &reqs);
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex =
        find_memory_type(mem_props, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory texture_memory = VK_NULL_HANDLE;
    if (vk.vkAllocateMemory(device, &alloc, nullptr, &texture_memory) != VK_SUCCESS ||
        vk.vkBindImageMemory(device, texture, texture_memory, 0) != VK_SUCCESS) {
        die("texture memory");
    }
    view_info.image = texture;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageView texture_view = VK_NULL_HANDLE;
    if (vk.vkCreateImageView(device, &view_info, nullptr, &texture_view) != VK_SUCCESS) {
        die("texture view");
    }
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = 1000.0f;  // ImGui's own sampler, restated
    VkSampler sampler = VK_NULL_HANDLE;
    if (vk.vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
        die("sampler");
    }

    // ---- Command pool and buffer ------------------------------------------
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vk.vkCreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
        die("command pool");
    }
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vk.vkAllocateCommandBuffers(device, &cmd_info, &cmd) != VK_SUCCESS) {
        die("command buffer");
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    // Upload the white texel once.
    vk.vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {1, 1, 1};
    vk.vkCmdCopyBufferToImage(cmd, staging_buffer, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
    vk.vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vk.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
        vk.vkQueueWaitIdle(queue) != VK_SUCCESS) {
        die("texture upload submit");
    }

    // ---- The measurements -------------------------------------------------
    struct Case {
        int mode;
        float nits;
        const char* name;
    };
#ifdef VOCEM_SRGB_TARGET
    // One mode, because one mode is what an sRGB-format swapchain ever gets --
    // and the nits are not part of it: mode 3 is not an HDR conversion, it is
    // the encoding the attachment does being handed the values it expects.
    const Case cases[] = {
        {3, 203.0f, "mode3 sRGB attachment"},
    };
#else
    const Case cases[] = {
        {2, 203.0f, "mode2 HDR10 203n (default)"},
        {2, 100.0f, "mode2 HDR10 100n"},
        {2, 1000.0f, "mode2 HDR10 1000n"},
        {1, 203.0f, "mode1 scRGB 203n (default)"},
        {1, 80.0f, "mode1 scRGB 80n"},
        {1, 1000.0f, "mode1 scRGB 1000n"},
        {3, 203.0f, "mode3 sRGB attachment (what the shader puts out)"},
    };
#endif

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

    for (const Case& c : cases) {
        // Through the project's own path: shipped SPIR-V, the specialization
        // constants folded at creation -- the exact call the layer makes.
        vocem::HdrPipeline pipeline;
        char line[160];
        snprintf(line, sizeof(line), "%s: pipeline builds through hdr_pipeline_create", c.name);
        if (!vocem::hdr_pipeline_create(fn, device, render_pass, c.mode, c.nits, pipeline)) {
            check(false, line);
            continue;
        }
        check(true, line);

        // One descriptor set, allocated against the first pipeline's layout
        // and bound with every later one: layout compatibility is by
        // definition, which is the premise the layer itself stands on.
        if (descriptor_pool == VK_NULL_HANDLE) {
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
            VkDescriptorPoolCreateInfo dp_info{};
            dp_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dp_info.maxSets = 1;
            dp_info.poolSizeCount = 1;
            dp_info.pPoolSizes = &size;
            if (vk.vkCreateDescriptorPool(device, &dp_info, nullptr, &descriptor_pool) !=
                VK_SUCCESS) {
                die("descriptor pool");
            }
            VkDescriptorSetAllocateInfo ds_info{};
            ds_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ds_info.descriptorPool = descriptor_pool;
            ds_info.descriptorSetCount = 1;
            ds_info.pSetLayouts = &pipeline.descriptors;
            if (vk.vkAllocateDescriptorSets(device, &ds_info, &descriptor_set) != VK_SUCCESS) {
                die("descriptor set");
            }
            VkDescriptorImageInfo image{};
            image.sampler = sampler;
            image.imageView = texture_view;
            image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            vk.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }

        vk.vkResetCommandBuffer(cmd, 0);
        vk.vkBeginCommandBuffer(cmd, &begin);
        VkClearValue clear{};
        VkRenderPassBeginInfo rp_begin{};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.renderPass = render_pass;
        rp_begin.framebuffer = framebuffer;
        rp_begin.renderArea = {{0, 0}, {kWidth, kHeight}};
        rp_begin.clearValueCount = 1;
        rp_begin.pClearValues = &clear;
        vk.vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight),
                            0.0f, 1.0f};
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
        vk.vkCmdSetScissor(cmd, 0, 1, &scissor);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1,
                                   &descriptor_set, 0, nullptr);
        const VkDeviceSize zero_offset = 0;
        vk.vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &zero_offset);
        vk.vkCmdBindIndexBuffer(cmd, index_buffer, 0,
                                sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16
                                                       : VK_INDEX_TYPE_UINT32);
        // ImGui's push constants: scale and translate from pixels to NDC.
        const float push[4] = {2.0f / kWidth, 2.0f / kHeight, -1.0f, -1.0f};
        vk.vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
                              push);
        vk.vkCmdDrawIndexed(cmd, 18, 1, 0, 0, 0);
        vk.vkCmdEndRenderPass(cmd);
        copy = VkBufferImageCopy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kWidth, kHeight, 1};
        vk.vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  readback_buffer, 1, &copy);
        VkMemoryBarrier host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                0, 1, &host_barrier, 0, nullptr, 0, nullptr);
        vk.vkEndCommandBuffer(cmd);
        if (vk.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
            vk.vkQueueWaitIdle(queue) != VK_SUCCESS) {
            die("draw submit");
        }

#ifdef VOCEM_SRGB_TARGET
        // The round trip: what the attachment stores has to be the byte ImGui
        // asked for. One unit of slack is the 8-bit quantisation of a value
        // that went out through a float shader and came back through the
        // hardware's encode.
        const uint8_t* stored = static_cast<const uint8_t*>(readback);
        for (int band = 0; band < 3; ++band) {
            const uint32_t x = band * kBand + kBand / 2;
            const uint32_t y = kHeight / 2;
            const uint8_t* px = stored + (y * kWidth + x) * 4;
            for (int i = 0; i < 3; ++i) {
                const int got = px[i];
                const int want = colours[band][i];
                snprintf(line, sizeof(line), "%s %s ch%d: stored %d, ImGui asked for %d", c.name,
                         colour_names[band], i, got, want);
                check(got >= want - 1 && got <= want + 1, line);
            }
        }
#else
        const uint16_t* pixels = static_cast<const uint16_t*>(readback);
        for (int band = 0; band < 3; ++band) {
            const uint32_t x = band * kBand + kBand / 2;
            const uint32_t y = kHeight / 2;
            const uint16_t* px = pixels + (y * kWidth + x) * 4;
            double srgb[3], want[3];
            for (int i = 0; i < 3; ++i) srgb[i] = colours[band][i] / 255.0;
            reference(c.mode, c.nits, srgb, want);
            for (int i = 0; i < 3; ++i) {
                const double got = half_to_float(px[i]);
                const double tol = 1e-3 + 1e-3 * std::fabs(want[i]);
                snprintf(line, sizeof(line), "%s %s ch%d: got %.6f want %.6f (tol %.4f)", c.name,
                         colour_names[band], i, got, want[i], tol);
                check(std::fabs(got - want[i]) <= tol, line);
            }
            const double alpha = half_to_float(px[3]);
            snprintf(line, sizeof(line), "%s %s alpha: got %.6f want 1.0", c.name,
                     colour_names[band], alpha);
            check(std::fabs(alpha - 1.0) <= 1e-3, line);
        }
#endif

        vocem::hdr_pipeline_destroy(fn, device, pipeline);
    }

    // Spot anchors straight against the GPU-facing reference, so the case
    // table above cannot drift away from the numbers this file promises.
    {
        const double white[3] = {1.0, 1.0, 1.0};
        double out[3];
        reference(2, 203.0, white, out);
        check(std::fabs(out[0] - 0.5806889) < 1e-6, "reference(mode2, 203, white) hits the anchor");
        reference(1, 80.0, white, out);
        check(std::fabs(out[0] - 1.0) < 1e-9, "reference(mode1, 80, white) = 1.0 exactly");
        reference(1, 1000.0, white, out);
        check(std::fabs(out[0] - 12.5) < 1e-9, "reference(mode1, 1000, white) = 12.5 exactly");
    }

    // Teardown: the device idles above; order is children before parents.
    if (descriptor_pool != VK_NULL_HANDLE) {
        vk.vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    }
    vk.vkDestroyCommandPool(device, pool, nullptr);
    vk.vkDestroySampler(device, sampler, nullptr);
    vk.vkDestroyImageView(device, texture_view, nullptr);
    vk.vkDestroyImage(device, texture, nullptr);
    vk.vkFreeMemory(device, texture_memory, nullptr);
    vk.vkDestroyBuffer(device, vertex_buffer, nullptr);
    vk.vkFreeMemory(device, vertex_memory, nullptr);
    vk.vkDestroyBuffer(device, index_buffer, nullptr);
    vk.vkFreeMemory(device, index_memory, nullptr);
    vk.vkDestroyBuffer(device, staging_buffer, nullptr);
    vk.vkFreeMemory(device, staging_memory, nullptr);
    vk.vkDestroyBuffer(device, readback_buffer, nullptr);
    vk.vkFreeMemory(device, readback_memory, nullptr);
    vk.vkDestroyFramebuffer(device, framebuffer, nullptr);
    vk.vkDestroyImageView(device, target_view, nullptr);
    vk.vkDestroyImage(device, target, nullptr);
    vk.vkFreeMemory(device, target_memory, nullptr);
    vk.vkDestroyRenderPass(device, render_pass, nullptr);
    vk.vkDestroyDevice(device, nullptr);
    vk.vkDestroyInstance(instance, nullptr);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
