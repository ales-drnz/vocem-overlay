// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A Vulkan layer that sits BELOW the overlay's and writes down what the overlay
// asks of the chain. The rules this project holds its layer to -- no wait inside
// vkQueuePresentKHR (rules 8 and 10), a fence handed to vkQueueSubmit unsignalled,
// a pipeline drawn only inside a render pass it is compatible with -- were all
// asserted in comments and measured by nothing: this machine has no Khronos
// validation layer installed, and vk_present_draw reads pixels, which every one
// of those faults leaves exactly as they were. Three of them were found by
// reading (DESIGN entry 131) and this is the instrument that would have found
// them by measuring.
//
// It is not a validation layer. It knows the handful of calls the rules are
// about and nothing else, and it reports to a file named by
// VOCEM_WITNESS_REPORT, one line per observation, each stamped with
// CLOCK_MONOTONIC nanoseconds:
//
//   <ns> present waits=N sems=<h,..>    vkQueuePresentKHR reached this layer, with the
//                                       semaphores it waits on -- from below the overlay
//                                       these are the ones the overlay handed down
//   <ns> submit fence=<h> signals=<h,..> vkQueueSubmit, with the fence it was given and
//                                       the semaphores it signals
//   <ns> wait <function> [fences=<h,..>] vkQueueWaitIdle, vkDeviceWaitIdle, or a
//                                       vkWaitForFences that actually had to wait, with
//                                       the fences it waited on
//   <ns> submit-signalled-fence         vkQueueSubmit given a fence already signalled
//   <ns> renderpass-created             vkCreateRenderPass
//   <ns> pipeline-created               vkCreateGraphicsPipelines
//   <ns> pipeline-renderpass-mismatch   vkCmdBindPipeline inside a render pass whose
//                                       attachment formats differ from the pass the
//                                       pipeline was created against
//
// The stamps are the whole method. From BELOW the overlay there is no nesting
// to observe: the overlay records, submits and waits before it hands the
// present down, so by the time this layer's own present hook runs the work is
// already done -- the first version of this file kept a depth counter around
// its present and counted the overlay's submits as "outside", every one of
// them, because from underneath they are. What the probe knows instead is the
// wall-clock interval of each of ITS OWN vkQueuePresentKHR calls, and a submit
// or a wait stamped inside one of those intervals happened inside the present,
// whoever made it. The probe is single-threaded and makes its own waits outside
// its presents, so the attribution is exact.
//
// `pipeline-created` is the positive control: the probe creates no pipeline
// and the overlay creates at least one, so a report without one means this
// layer sat ABOVE the overlay (where nothing the overlay does passes through
// it) and saw nothing of what it was asked to watch. Where it sits is not
// decided by its manifest's file name -- measured sorting first and last, it
// came out on top both times -- but by the loader's override meta-layer,
// whose component_layers list is the chain in order; the probe writes that
// manifest (vk_present_draw.cpp). Loaded only where VOCEM_WITNESS=1 is set,
// through the manifest tests/CMakeLists.txt generates. VOCEM_WITNESS_TRACE=1
// adds every vkGetDeviceProcAddr query and every pipeline bind to the report,
// for reading by hand.
//
// It can also refuse one call, which is how a failure the overlay has to
// survive is made to happen on a machine where it does not:
// VOCEM_WITNESS_FAIL_SAMPLER=N answers the process's Nth vkCreateSampler with
// VK_ERROR_OUT_OF_DEVICE_MEMORY and reports `sampler-refused device=<h>`, the
// device as the application holds it (the loader's handle, the same above and
// below every layer). The overlay's backend creates the first (ImGui's own)
// and its texture cache the second, so 2 is "the texture cache did not come
// up" (vk_present_draw's no-texture-cache scene).
// VOCEM_WITNESS_FAIL_SAMPLER_RACE=1 also stages a race around the refusal
// (witness_CreateSampler says which; the failed-threads scene).

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <sched.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {

struct InstanceData {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
};

struct RenderPassShape {
    std::vector<VkFormat> formats;
    std::vector<VkSampleCountFlagBits> samples;
    bool operator==(const RenderPassShape& other) const {
        return formats == other.formats && samples == other.samples;
    }
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkGetFenceStatus GetFenceStatus = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    // What was created, by handle. A destroyed handle the driver reuses is
    // simply overwritten by the next creation, which is the right answer for
    // `passes` -- and the reason a pipeline remembers the SHAPE of the pass it
    // was built against rather than the handle: the first version compared
    // handles first, and a swapchain recreated after its predecessor was
    // destroyed got the same VkRenderPass value back from the driver, so the
    // stock pipeline built for the old pass and the new pass "matched" by
    // handle and the mismatch this exists to see was never reported.
    std::unordered_map<VkRenderPass, RenderPassShape> passes;
    std::unordered_map<VkPipeline, RenderPassShape> pipelines;
    std::unordered_map<VkCommandBuffer, VkRenderPass> recording;
};

std::mutex g_lock;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;
// Presents that reached this layer, any device (VOCEM_WITNESS_FAIL_SAMPLER_RACE).
std::atomic<unsigned long> g_presents{0};

void* dispatch_key(void* handle) { return *reinterpret_cast<void**>(handle); }

// A handle as the report spells it. Non-dispatchable handles are pointers at
// 64 bits and integers at 32; a C-style cast reads both.
template <typename Handle>
unsigned long long handle_value(Handle handle) {
    return (unsigned long long)handle;
}

// Appends ` <key>=<h>,<h>,...` to `line`, as many as fit.
template <typename Handle>
void append_handles(char* line, size_t capacity, const char* key, const Handle* handles,
                    uint32_t count) {
    size_t used = std::strlen(line);
    int written = std::snprintf(line + used, capacity - used, " %s=", key);
    if (written < 0) {
        return;
    }
    used += static_cast<size_t>(written);
    for (uint32_t i = 0; i < count && used < capacity; ++i) {
        written = std::snprintf(line + used, capacity - used, "%s0x%llx", i ? "," : "",
                                handle_value(handles[i]));
        if (written < 0) {
            return;
        }
        used += static_cast<size_t>(written);
    }
}

DeviceData* find_device(void* dispatchable) {
    auto it = g_devices.find(dispatch_key(dispatchable));
    return it == g_devices.end() ? nullptr : &it->second;
}

void report(const char* line) {
    static const char* path = std::getenv("VOCEM_WITNESS_REPORT");
    if (!path || !path[0]) {
        return;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (FILE* file = std::fopen(path, "a")) {
        std::fprintf(file, "%lld %s\n",
                     static_cast<long long>(now.tv_sec) * 1000000000LL +
                         static_cast<long long>(now.tv_nsec),
                     line);
        std::fclose(file);
    }
}

VkLayerInstanceCreateInfo* instance_chain(const VkInstanceCreateInfo* info) {
    auto* item = static_cast<const VkLayerInstanceCreateInfo*>(info->pNext);
    while (item) {
        if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            item->function == VK_LAYER_LINK_INFO) {
            return const_cast<VkLayerInstanceCreateInfo*>(item);
        }
        item = static_cast<const VkLayerInstanceCreateInfo*>(item->pNext);
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* device_chain(const VkDeviceCreateInfo* info) {
    auto* item = static_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
    while (item) {
        if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            item->function == VK_LAYER_LINK_INFO) {
            return const_cast<VkLayerDeviceCreateInfo*>(item);
        }
        item = static_cast<const VkLayerDeviceCreateInfo*>(item->pNext);
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL witness_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkInstance* pInstance) {
    VkLayerInstanceCreateInfo* link = instance_chain(pCreateInfo);
    if (!link || !link->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const VkResult result = create(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }
    InstanceData data;
    data.gipa = next_gipa;
    data.DestroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*pInstance, "vkDestroyInstance"));
    std::lock_guard<std::mutex> guard(g_lock);
    g_instances[dispatch_key(*pInstance)] = data;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL witness_DestroyInstance(VkInstance instance,
                                                   const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyInstance destroy = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) {
            destroy = it->second.DestroyInstance;
            g_instances.erase(it);
        }
    }
    if (destroy) {
        destroy(instance, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL witness_CreateDevice(VkPhysicalDevice physicalDevice,
                                                    const VkDeviceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkDevice* pDevice) {
    VkLayerDeviceCreateInfo* link = device_chain(pCreateInfo);
    if (!link || !link->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const VkResult result = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }
    DeviceData data;
    data.device = *pDevice;
    data.gdpa = next_gdpa;
#define WITNESS_LOAD(name) \
    data.name = reinterpret_cast<PFN_vk##name>(next_gdpa(*pDevice, "vk" #name))
    WITNESS_LOAD(DestroyDevice);
    WITNESS_LOAD(QueuePresentKHR);
    WITNESS_LOAD(QueueSubmit);
    WITNESS_LOAD(QueueWaitIdle);
    WITNESS_LOAD(DeviceWaitIdle);
    WITNESS_LOAD(WaitForFences);
    WITNESS_LOAD(GetFenceStatus);
    WITNESS_LOAD(CreateRenderPass);
    WITNESS_LOAD(CreateGraphicsPipelines);
    WITNESS_LOAD(CmdBeginRenderPass);
    WITNESS_LOAD(CmdEndRenderPass);
    WITNESS_LOAD(CmdBindPipeline);
    WITNESS_LOAD(CreateSampler);
#undef WITNESS_LOAD
    std::lock_guard<std::mutex> guard(g_lock);
    g_devices[dispatch_key(*pDevice)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL witness_DestroyDevice(VkDevice device,
                                                 const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyDevice destroy = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            destroy = it->second.DestroyDevice;
            g_devices.erase(it);
        }
    }
    if (destroy) {
        destroy(device, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL witness_QueuePresentKHR(VkQueue queue,
                                                       const VkPresentInfoKHR* pPresentInfo) {
    PFN_vkQueuePresentKHR next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(queue)) {
            next = dev->QueuePresentKHR;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    g_presents++;
    char line[512];
    std::snprintf(line, sizeof(line), "present waits=%u", pPresentInfo->waitSemaphoreCount);
    // Which semaphores: from below the overlay, the ones it handed down -- its
    // own where it drew, the application's where it passed the frame through.
    append_handles(line, sizeof(line), "sems", pPresentInfo->pWaitSemaphores,
                   pPresentInfo->waitSemaphoreCount);
    report(line);
    return next(queue, pPresentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL witness_QueueSubmit(VkQueue queue, uint32_t submitCount,
                                                   const VkSubmitInfo* pSubmits, VkFence fence) {
    PFN_vkQueueSubmit next = nullptr;
    PFN_vkGetFenceStatus status = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(queue)) {
            next = dev->QueueSubmit;
            status = dev->GetFenceStatus;
            device = dev->device;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    {
        // The fence and the signalled semaphores, so the probe can tell the
        // overlay's own fence from any other in a later wait, and match a
        // present's wait semaphore to the submit that signals it.
        char line[512];
        std::snprintf(line, sizeof(line), "submit fence=0x%llx", handle_value(fence));
        for (uint32_t i = 0; i < submitCount; ++i) {
            append_handles(line, sizeof(line), "signals", pSubmits[i].pSignalSemaphores,
                           pSubmits[i].signalSemaphoreCount);
        }
        report(line);
    }
    // Asked of the driver, not inferred from the calls seen: a fence's state
    // is the one thing the chain below can answer exactly.
    if (fence != VK_NULL_HANDLE && status && status(device, fence) == VK_SUCCESS) {
        report("submit-signalled-fence");
    }
    return next(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL witness_QueueWaitIdle(VkQueue queue) {
    PFN_vkQueueWaitIdle next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(queue)) {
            next = dev->QueueWaitIdle;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    report("wait vkQueueWaitIdle");
    return next(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL witness_DeviceWaitIdle(VkDevice device) {
    PFN_vkDeviceWaitIdle next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->DeviceWaitIdle;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    report("wait vkDeviceWaitIdle");
    return next(device);
}

VKAPI_ATTR VkResult VKAPI_CALL witness_WaitForFences(VkDevice device, uint32_t fenceCount,
                                                     const VkFence* pFences, VkBool32 waitAll,
                                                     uint64_t timeout) {
    PFN_vkWaitForFences next = nullptr;
    PFN_vkGetFenceStatus status = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->WaitForFences;
            status = dev->GetFenceStatus;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // The overlay waits on its OWN fence inside the present, once its previous
    // submission for the same image is outstanding -- rule 8 allows exactly
    // that wait ("the only wait is on our own fence"). What it must not do is
    // block: a wait on a fence that is already signalled returns at once and
    // is not reported; one that actually has to wait is.
    if (status) {
        bool all_signalled = true;
        for (uint32_t i = 0; i < fenceCount; ++i) {
            if (status(device, pFences[i]) != VK_SUCCESS) {
                all_signalled = false;
            }
        }
        if (!all_signalled) {
            char line[512];
            std::snprintf(line, sizeof(line), "wait vkWaitForFences");
            append_handles(line, sizeof(line), "fences", pFences, fenceCount);
            report(line);
        }
    }
    return next(device, fenceCount, pFences, waitAll, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL witness_CreateRenderPass(VkDevice device,
                                                        const VkRenderPassCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkRenderPass* pRenderPass) {
    PFN_vkCreateRenderPass next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->CreateRenderPass;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = next(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) {
        report("renderpass-created");
        RenderPassShape shape;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
            shape.formats.push_back(pCreateInfo->pAttachments[i].format);
            shape.samples.push_back(pCreateInfo->pAttachments[i].samples);
        }
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            dev->passes[*pRenderPass] = shape;
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL witness_CreateSampler(VkDevice device,
                                                     const VkSamplerCreateInfo* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator,
                                                     VkSampler* pSampler) {
    static int created = 0;
    PFN_vkCreateSampler next = nullptr;
    int nth = 0;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->CreateSampler;
        }
        nth = ++created;
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const char* refuse = std::getenv("VOCEM_WITNESS_FAIL_SAMPLER");
    if (refuse && std::atoi(refuse) == nth) {
        // VOCEM_WITNESS_FAIL_SAMPLER_RACE=1 (the failed-threads scene): the
        // refusal waits for a present of another device -- whose thread then
        // goes on to the overlay's prepare() and waits there, on the lock
        // this thread holds -- and 50 ms more, and the refused thread drops
        // to SCHED_IDLE. On one CPU the thread it wakes by giving that lock
        // back then runs before it does: the interleaving a loaded machine
        // gives now and then, every time.
        if (const char* race = std::getenv("VOCEM_WITNESS_FAIL_SAMPLER_RACE");
            race && std::strcmp(race, "1") == 0) {
            const unsigned long seen = g_presents.load();
            const timespec millisecond{0, 1000000};
            for (int waited = 0; waited < 2000 && g_presents.load() == seen; ++waited) {
                nanosleep(&millisecond, nullptr);
            }
            const timespec settle{0, 50000000};
            nanosleep(&settle, nullptr);
            const sched_param none{};
            sched_setscheduler(0, SCHED_IDLE, &none);
        }
        char line[64];
        std::snprintf(line, sizeof(line), "sampler-refused device=0x%llx", handle_value(device));
        report(line);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return next(device, pCreateInfo, pAllocator, pSampler);
}

VKAPI_ATTR VkResult VKAPI_CALL witness_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    PFN_vkCreateGraphicsPipelines next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->CreateGraphicsPipelines;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result =
        next(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    if (result == VK_SUCCESS) {
        report("pipeline-created");
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            for (uint32_t i = 0; i < createInfoCount; ++i) {
                auto pass = dev->passes.find(pCreateInfos[i].renderPass);
                if (pass != dev->passes.end()) {
                    dev->pipelines[pPipelines[i]] = pass->second;
                }
            }
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL witness_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                      const VkRenderPassBeginInfo* pRenderPassBegin,
                                                      VkSubpassContents contents) {
    PFN_vkCmdBeginRenderPass next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(commandBuffer)) {
            next = dev->CmdBeginRenderPass;
            dev->recording[commandBuffer] = pRenderPassBegin->renderPass;
        }
    }
    if (next) {
        next(commandBuffer, pRenderPassBegin, contents);
    }
}

VKAPI_ATTR void VKAPI_CALL witness_CmdEndRenderPass(VkCommandBuffer commandBuffer) {
    PFN_vkCmdEndRenderPass next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(commandBuffer)) {
            next = dev->CmdEndRenderPass;
            dev->recording.erase(commandBuffer);
        }
    }
    if (next) {
        next(commandBuffer);
    }
}

VKAPI_ATTR void VKAPI_CALL witness_CmdBindPipeline(VkCommandBuffer commandBuffer,
                                                   VkPipelineBindPoint pipelineBindPoint,
                                                   VkPipeline pipeline) {
    PFN_vkCmdBindPipeline next = nullptr;
    bool mismatch = false;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(commandBuffer)) {
            next = dev->CmdBindPipeline;
            if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
                auto inside = dev->recording.find(commandBuffer);
                auto built = dev->pipelines.find(pipeline);
                if (inside != dev->recording.end() && built != dev->pipelines.end()) {
                    // Compatibility is by shape, not by handle (Vulkan 1.4,
                    // "Render Pass Compatibility"): the same attachment formats
                    // and sample counts, in the same order.
                    auto current = dev->passes.find(inside->second);
                    if (current != dev->passes.end() && !(current->second == built->second)) {
                        mismatch = true;
                    }
                }
            }
        }
    }
    if (mismatch) {
        report("pipeline-renderpass-mismatch");
    }
    if (std::getenv("VOCEM_WITNESS_TRACE")) {
        std::lock_guard<std::mutex> guard(g_lock);
        DeviceData* dev = find_device(commandBuffer);
        char line[256];
        std::snprintf(line, sizeof(line), "bind device=%d inside=%d known=%d",
                      dev ? 1 : 0,
                      dev && dev->recording.count(commandBuffer) ? 1 : 0,
                      dev && dev->pipelines.count(pipeline) ? 1 : 0);
        report(line);
    }
    if (next) {
        next(commandBuffer, pipelineBindPoint, pipeline);
    }
}

struct NameAndFunction {
    const char* name;
    PFN_vkVoidFunction func;
};

#define WITNESS_ENTRY(name) \
    { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(witness_##name) }

const NameAndFunction kIntercepted[] = {
    WITNESS_ENTRY(CreateInstance),     WITNESS_ENTRY(DestroyInstance),
    WITNESS_ENTRY(CreateDevice),       WITNESS_ENTRY(DestroyDevice),
    WITNESS_ENTRY(QueuePresentKHR),    WITNESS_ENTRY(QueueSubmit),
    WITNESS_ENTRY(QueueWaitIdle),      WITNESS_ENTRY(DeviceWaitIdle),
    WITNESS_ENTRY(WaitForFences),      WITNESS_ENTRY(CreateRenderPass),
    WITNESS_ENTRY(CreateGraphicsPipelines), WITNESS_ENTRY(CmdBeginRenderPass),
    WITNESS_ENTRY(CmdEndRenderPass),   WITNESS_ENTRY(CmdBindPipeline),
    WITNESS_ENTRY(CreateSampler),
};

#undef WITNESS_ENTRY

PFN_vkVoidFunction intercepted(const char* name) {
    for (const NameAndFunction& entry : kIntercepted) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.func;
        }
    }
    return nullptr;
}

}  // namespace

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
witness_GetDeviceProcAddr(VkDevice device, const char* pName) {
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->gdpa;
        }
    }
    if (std::getenv("VOCEM_WITNESS_TRACE")) {
        char line[256];
        std::snprintf(line, sizeof(line), "gdpa %s %s", pName, next ? "known-device" : "UNKNOWN-DEVICE");
        report(line);
    }
    if (!next) {
        return nullptr;
    }
    PFN_vkVoidFunction below = next(device, pName);
    if (!below) {
        return nullptr;
    }
    if (PFN_vkVoidFunction func = intercepted(pName)) {
        return func;
    }
    return below;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
witness_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(witness_GetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(witness_GetDeviceProcAddr);
    }
    if (PFN_vkVoidFunction func = intercepted(pName)) {
        return func;
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    PFN_vkGetInstanceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) {
            next = it->second.gipa;
        }
    }
    return next ? next(instance, pName) : nullptr;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }
    pVersionStruct->pfnGetInstanceProcAddr = witness_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = witness_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

}  // extern "C"
