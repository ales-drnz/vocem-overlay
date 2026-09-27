// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The injected heavy libraries loaded and unloaded in a loop, and what each
// cycle leaves on the heap.
//
// The Vulkan loader dlopens a layer at vkCreateInstance and dlcloses it at
// vkDestroyInstance, and games do that more than once -- Minecraft 26.x probes
// Vulkan at startup and destroys the instance. Since the two heavy libraries
// carry their own libstdc++ (-static-libstdc++, since 0.1.11), each
// load runs that copy's constructor for the exception emergency pool -- a
// ~73 KB malloc -- and libstdc++ never frees it on unload: its destructor
// leaves the arena to __gnu_cxx::__freeres(), which only memory checkers call.
// Measured by the 0.1.11 refutation pass through the real loader: +3.86 MB of
// heap over 50 vkCreateInstance/vkDestroyInstance cycles; 937 B per cycle with
// the 0.1.10 libraries, which NEEDED the system libstdc++ and its one pool.
//
// The libraries are VOCEM_GL_LIBRARY and VOCEM_VK_LIBRARY. For each: five cycles to settle, then forty
// measured with mallinfo2's in-use bytes, and a check that the library really
// left the address space after each dlclose -- a library that cannot be
// unloaded would make any heap figure meaningless. The bound is 1 KB per cycle,
// against the 73.7 KB (12.8 KB at 32 bits) the defect cost.

#include <dlfcn.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool mapped(const char* path) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        return false;
    }
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof(line), maps)) {
        found = strstr(line, path) != nullptr;
    }
    fclose(maps);
    return found;
}

size_t in_use() { return mallinfo2().uordblks; }

}  // namespace

int main() {
    const char* libraries[] = {getenv("VOCEM_GL_LIBRARY"), getenv("VOCEM_VK_LIBRARY")};
    if (!libraries[0] || !libraries[1]) {
        printf("FAIL VOCEM_GL_LIBRARY and VOCEM_VK_LIBRARY name the libraries to cycle\n");
        return 1;
    }
    int failures = 0;
    for (const char* path : libraries) {
        constexpr int kSettle = 5;
        constexpr int kCycles = 40;
        constexpr long kBoundPerCycle = 1024;
        int stayed = 0;
        size_t before = 0;
        for (int cycle = 0; cycle < kSettle + kCycles; ++cycle) {
            if (cycle == kSettle) {
                before = in_use();
            }
            void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!library) {
                printf("FAIL %s does not load: %s\n", path, dlerror());
                return 1;
            }
            dlclose(library);
            stayed += mapped(path) ? 1 : 0;
        }
        const long grown = static_cast<long>(in_use()) - static_cast<long>(before);
        const long per_cycle = grown / kCycles;
        printf("     %s: %ld bytes of heap kept over %d load/unload cycles, %ld per cycle; "
               "still mapped after %d of %d dlclose\n",
               path, grown, kCycles, per_cycle, stayed, kSettle + kCycles);
        if (stayed != 0) {
            printf("FAIL %s was not unloaded, so the heap figure measures nothing\n", path);
            ++failures;
        } else if (per_cycle > kBoundPerCycle) {
            printf("FAIL %s keeps %ld bytes per load/unload cycle (bound %ld)\n", path,
                   per_cycle, kBoundPerCycle);
            ++failures;
        } else {
            printf("ok   %s gives back what it took\n", path);
        }
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
