// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Is the INSTALLED layer in this process's dispatch chain, and is it this
// process's architecture? One instance, no surface, no window, no draw -- so it
// runs anywhere a game runs, which is the point: the interesting places are
// inside Steam's container and inside gamescope, and a probe that needs a
// display cannot go everywhere.
//
// It measures what ships rather than the build tree, deliberately and unusually.
// The claim is about the installed *pair* of manifests -- both carry the name
// VK_LAYER_VOCEM_overlay, the loader discards one, and the survivor names the
// library by bare soname so ld.so resolves it per ELF class. Nothing about that
// can be measured against a build tree with an absolute path in its manifest, and
// nothing about it is settled by the 64-bit half working (entries 30/33/34).
//
// The witness is the mapped path, not the enumerated name. The name appears in
// vkEnumerateInstanceLayerProperties whether or not the library behind it was
// loadable, and "the loader knows a layer by that name" is not "this process got
// our code at its own width" -- which is exactly the distinction the whole
// duplicate-name question turns on. /proc/self/maps answers the second question.
// Prints the path; the caller decides whether lib or lib32 was the right answer,
// because only the caller knows which architecture it launched.

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main() {
    printf("probe: %zu-bit\n", sizeof(void*) * 8);

    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        printf("skip no Vulkan loader (libvulkan.so.1) at this width\n");
        return 77;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!gipa) {
        printf("skip loader has no vkGetInstanceProcAddr\n");
        return 77;
    }
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        printf("skip loader has no vkCreateInstance\n");
        return 77;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vocem_vk_layer_loads";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = create(&info, nullptr, &instance);
    if (created != VK_SUCCESS) {
        printf("skip vkCreateInstance failed (%d): no usable Vulkan at this width\n",
               (int)created);
        return 77;
    }

    // The answer. An implicit layer is loaded during vkCreateInstance, so by here
    // its library is mapped or it never arrived.
    bool found = false;
    if (FILE* maps = fopen("/proc/self/maps", "r")) {
        char line[2048];
        while (fgets(line, sizeof(line), maps)) {
            if (!strstr(line, "libvocem_vk.so")) {
                continue;
            }
            char* start = strchr(line, '/');
            if (!start) {
                continue;
            }
            if (char* end = strchr(start, '\n')) {
                *end = '\0';
            }
            printf("probe: mapped %s\n", start);
            found = true;
            break;
        }
        fclose(maps);
    }
    if (!found) {
        printf("probe: mapped nothing of ours\n");
    }

    if (auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))) {
        destroy(instance, nullptr);
    }
    return found ? 0 : 1;
}
