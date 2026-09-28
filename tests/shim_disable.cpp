// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// VOCEM_DISABLE=1 has to switch off the interposition, not just the drawing.
//
// It did not, twice. In -40 it gated the draw and left the dispatch substitution
// in place, so the documented way to take the overlay out of a process kept the
// part that was breaking the process (rule 35). Before that, VOCEM_DISABLE and
// VOCEM_NO_DLSYM both still routed every lookup through a null real_dlsym on
// i386 (rule 30). A switch that turns the overlay off has to turn off everything
// the overlay does to other people's symbol resolution, or it is documentation
// rather than a switch.
//
// Run with the shim preloaded and VOCEM_DISABLE=1: every lookup an application
// can make must come back with the real pointer, never with a hook of ours. The
// one thing that cannot be switched off is the exported symbols themselves --
// an application linked against libEGL resolves `eglSwapBuffers` to our copy at
// relocation time no matter what -- so what the hooks *do* when disabled is
// forward faithfully, which the last check exercises end to end.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real_dlsym.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// The real dlsym, reached the way the shim reaches it, so the shim's exports can
// be told apart from what the application should be seeing.
void* bypass_dlsym(void* handle, const char* name) {
    using PFN_dlsym = void* (*)(void*, const char*);
    static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
    PFN_dlsym real = nullptr;
    for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]) && !real; ++i) {
        real = reinterpret_cast<PFN_dlsym>(dlvsym(RTLD_DEFAULT, "dlsym", versions[i]));
    }
    return real ? real(handle, name) : nullptr;
}

using PFN_get_proc = void* (*)(const char*);

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded; set VOCEM_SHIM_PRELOADED=1\n");
        return 77;
    }
    const char* env = getenv("VOCEM_DISABLE");
    if (!env || env[0] != '1') {
        printf("skip meant to run with VOCEM_DISABLE=1 set\n");
        return 77;
    }

    void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!egl) {
        printf("skip libEGL.so.1 is not installed\n");
        return 77;
    }

    // What the application should see: the library's own functions, fetched
    // around the interposed dlsym.
    void* real_swap = bypass_dlsym(egl, "eglSwapBuffers");
    void* real_get_proc = bypass_dlsym(egl, "eglGetProcAddress");
    // And what it must not see: ours.
    void* our_swap = bypass_dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    void* our_get_proc = bypass_dlsym(RTLD_DEFAULT, "eglGetProcAddress");
    check(real_swap && real_get_proc && our_swap && our_get_proc,
          "both spellings of both functions resolve");
    check(real_swap != our_swap, "and the shim's copies are distinguishable from the real ones");

    // The interposed dlsym, with the switch thrown: hands back the real thing.
    void* answered_swap = dlsym(egl, "eglSwapBuffers");
    check(answered_swap == real_swap,
          "dlsym on a private handle answers the library's own present function");
    void* answered_get_proc = dlsym(egl, "eglGetProcAddress");
    check(answered_get_proc == real_get_proc,
          "and the library's own dispatch function");

    // The dispatch hooks cannot be un-exported, so disabled means: forward
    // faithfully, substitute nothing.
    PFN_get_proc hook = reinterpret_cast<PFN_get_proc>(our_get_proc);
    void* through_hook = hook("eglSwapBuffers");
    check(through_hook != our_swap,
          "the dispatch hook, if reached anyway, does not hand out our present hook");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
