// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A dispatcher asked for the OTHER family's name, and what the shim remembers.
//
// glvnd's dispatchers do not answer "no" to a name they do not know: they hand
// out a libGLdispatch stub for a GL extension function. Measured on this
// machine, glXGetProcAddressARB("eglSwapBuffers") and
// eglGetProcAddress("glXSwapBuffers") both come back non-null from
// libGLdispatch.so.0. The shim's dispatch hook asked the real dispatcher,
// found the name in its table, saw a pointer in a system GL library -- and
// remembered the stub as the real eglSwapBuffers. First sighting wins, so the
// application's own correct dlsym a moment later could not replace it, and
// every present went to a GL stub that does nothing: the swap never happened.
//
// This probe does exactly that, in both directions, the way a loader that
// probes names does: the other family's dispatcher is asked first, then the
// application resolves the present function off its own RTLD_LOCAL handle.
// Three witnesses:
//
//   * the shim's remembered pointer for each present function, read out of its
//     table (the `g_hooks` symbol in its .symtab; a stripped shim is a skip)
//     and named by dladdr: libEGL for eglSwapBuffers, and not libGLdispatch
//     for glXSwapBuffers;
//   * behaviour: the shim's eglSwapBuffers called with no display must reach
//     the real one, which says EGL_BAD_DISPLAY -- the stub says nothing;
//   * and the other family's name is answered with the dispatcher's own
//     answer, never the shim's hook.
//
// VOCEM_GL_LIBRARY names a file that does not exist, so the present this makes
// loads no overlay: nothing here touches a state segment.

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// The shim's table entry, as gl/src/vocem_gl_shim.cpp declares it. The size
// of the symbol is checked against it below, so a layout change is a FAIL
// that says so rather than a wrong reading.
struct Hook {
    const char* name;
    void* hook;
    void* seen;
    void* next;
    int next_attempted;
    void* chain;
    int next_chain;
    int said_not_followed;
};

const char* base_name(const void* pointer) {
    Dl_info info{};
    if (!pointer || dladdr(pointer, &info) == 0 || !info.dli_fname) {
        return "(nothing)";
    }
    const char* slash = strrchr(info.dli_fname, '/');
    return slash ? slash + 1 : info.dli_fname;
}

// The shim's g_hooks, found through its own file's symbol table.
Hook* find_table(const char* path, char* base, size_t& count) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return nullptr;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    fclose(file);
    if (bytes.size() < sizeof(ElfW(Ehdr))) {
        return nullptr;
    }
    const auto* header = reinterpret_cast<const ElfW(Ehdr)*>(bytes.data());
    const auto* sections = reinterpret_cast<const ElfW(Shdr)*>(bytes.data() + header->e_shoff);
    for (unsigned i = 0; i < header->e_shnum; ++i) {
        if (sections[i].sh_type != SHT_SYMTAB) {
            continue;
        }
        const ElfW(Shdr)& strings = sections[sections[i].sh_link];
        const auto* symbols =
            reinterpret_cast<const ElfW(Sym)*>(bytes.data() + sections[i].sh_offset);
        const size_t n = sections[i].sh_size / sizeof(ElfW(Sym));
        for (size_t s = 0; s < n; ++s) {
            const char* name =
                reinterpret_cast<const char*>(bytes.data() + strings.sh_offset + symbols[s].st_name);
            if (strstr(name, "g_hooks")) {
                if (symbols[s].st_size % sizeof(Hook) != 0) {
                    printf("FAIL the shim's g_hooks is %zu bytes, not a whole number of the %zu-byte "
                           "entries this probe reads: the layout changed\n",
                           static_cast<size_t>(symbols[s].st_size), sizeof(Hook));
                    // Counted, so main() reports the failure it printed: it
                    // used to fall through to the "no symbol table" skip
                    // (entry 123's shape).
                    ++failures;
                    return nullptr;
                }
                count = symbols[s].st_size / sizeof(Hook);
                return reinterpret_cast<Hook*>(base + symbols[s].st_value);
            }
        }
    }
    return nullptr;
}

Hook* entry(Hook* table, size_t count, const char* name) {
    for (size_t i = 0; i < count; ++i) {
        if (table[i].name && strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded\n");
        return 77;
    }
    // No overlay may load when the probe calls the present hook below.
    setenv("VOCEM_GL_LIBRARY", "/nonexistent/vocem-shim-dispatch-family/libvocem_gl.so", 1);

    void* ours = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    Dl_info shim{};
    if (!ours || dladdr(ours, &shim) == 0 || !strstr(shim.dli_fname, "vocem_gl_shim")) {
        printf("FAIL the shim is not first in the global scope\n");
        return 1;
    }
    size_t count = 0;
    Hook* table = find_table(shim.dli_fname, static_cast<char*>(shim.dli_fbase), count);
    if (!table) {
        if (failures == 0) {
            printf("skip %s has no symbol table to find the shim's hook table in\n",
                   shim.dli_fname);
            return 77;
        }
        return 1;
    }
    Hook* egl_swap = entry(table, count, "eglSwapBuffers");
    Hook* glx_swap = entry(table, count, "glXSwapBuffers");
    if (!egl_swap || !glx_swap) {
        printf("FAIL the shim's table has no eglSwapBuffers or glXSwapBuffers entry\n");
        return 1;
    }

    void* glx = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!glx || !egl) {
        printf("skip libGL.so.1 and libEGL.so.1 are not both installed\n");
        return 77;
    }
    using PFN_gpa = void* (*)(const char*);
    auto* glx_gpa = reinterpret_cast<PFN_gpa>(dlsym(glx, "glXGetProcAddressARB"));
    auto* egl_gpa = reinterpret_cast<PFN_gpa>(dlsym(egl, "eglGetProcAddress"));
    if (!glx_gpa || !egl_gpa) {
        printf("FAIL the dispatchers do not resolve\n");
        return 1;
    }

    // The other family first, as a loader probing names does...
    void* egl_via_glx = glx_gpa("eglSwapBuffers");
    void* glx_via_egl = egl_gpa("glXSwapBuffers");
    printf("     glXGetProcAddressARB(\"eglSwapBuffers\") -> %s; "
           "eglGetProcAddress(\"glXSwapBuffers\") -> %s\n",
           base_name(egl_via_glx), base_name(glx_via_egl));
    // ...then the application's own resolution, off its own handles.
    void* game_egl_swap = dlsym(egl, "eglSwapBuffers");
    void* game_glx_swap = dlsym(glx, "glXSwapBuffers");
    const void* egl_seen = __atomic_load_n(&egl_swap->seen, __ATOMIC_ACQUIRE);
    const void* glx_seen = __atomic_load_n(&glx_swap->seen, __ATOMIC_ACQUIRE);
    printf("     the shim forwards eglSwapBuffers to %s and glXSwapBuffers to %s\n",
           base_name(egl_seen), base_name(glx_seen));

    check(strncmp(base_name(egl_seen), "libEGL.so.", 10) == 0,
          "the shim forwards eglSwapBuffers to libEGL, not to a GL dispatch stub");
    check(glx_seen && strncmp(base_name(glx_seen), "libGLdispatch", 13) != 0,
          "and glXSwapBuffers to the GLX library, not to a GL dispatch stub");
    check(egl_via_glx != ours && glx_via_egl != dlsym(RTLD_DEFAULT, "glXSwapBuffers"),
          "a dispatcher asked for the other family's name gives its own answer, not the shim's "
          "hook");

    // Behaviour: the present the application got must reach the real one.
    using PFN_egl_swap = unsigned int (*)(void*, void*);
    using PFN_egl_error = int (*)();
    auto* egl_error = reinterpret_cast<PFN_egl_error>(dlsym(egl, "eglGetError"));
    if (egl_error && game_egl_swap) {
        egl_error();  // cleared
        reinterpret_cast<PFN_egl_swap>(game_egl_swap)(nullptr, nullptr);
        const int error = egl_error();
        printf("     eglSwapBuffers(no display) through the shim left EGL error 0x%x\n", error);
        check(error == 0x3008 /*EGL_BAD_DISPLAY*/,
              "the application's eglSwapBuffers reaches the real one, which refuses no display");
    } else {
        check(false, "eglGetError and eglSwapBuffers resolve off libEGL");
    }
    (void)game_glx_swap;

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
