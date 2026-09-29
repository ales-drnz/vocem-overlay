// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A library that looks the present up on the game's behalf and hooks nothing
// -- SDL's shape -- for tests/shim_chain.cpp. Preloaded as well (Steam users
// put SDL2 in LD_PRELOAD as a fix), it is in LD_PRELOAD without being an
// interposer: the refutation of the chain change measured the shim treating
// every lookup it made as "another interposer asking" and never drawing.

#include <dlfcn.h>

#define EXPORT extern "C" __attribute__((visibility("default")))

using PFN_swap = unsigned (*)(void*, void*);
using PFN_proc = void* (*)(const char*);

// Presents `frames` frames through the door named: 3 (dlsym on the handle) or
// 2 (the handle's eglGetProcAddress).
EXPORT void vocem_lookup_present(int frames, int door) {
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl) {
        return;
    }
    PFN_swap swap = nullptr;
    if (door == 2) {
        PFN_proc get_proc = reinterpret_cast<PFN_proc>(dlsym(egl, "eglGetProcAddress"));
        swap = get_proc ? reinterpret_cast<PFN_swap>(get_proc("eglSwapBuffers")) : nullptr;
    } else {
        swap = reinterpret_cast<PFN_swap>(dlsym(egl, "eglSwapBuffers"));
    }
    for (int frame = 0; swap && frame < frames; ++frame) {
        swap(nullptr, nullptr);
    }
}
