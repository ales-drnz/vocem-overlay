// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay library as the shim sees it, and nothing else: the four entry
// points the shim resolves, counting. Named by VOCEM_GL_LIBRARY in
// tests/shim_chain.cpp, so "the shim handed us a frame" is a number rather
// than a pixel, and a frame handed over twice is a two -- and nothing needs a
// display, a context or the state segment.

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {
int g_presents = 0;
}  // namespace

EXPORT void vocem_gl_present_glx(void*, unsigned long) {
    ++g_presents;
}

EXPORT void vocem_gl_present_egl(void*, void*) {
    ++g_presents;
}

EXPORT void vocem_gl_context_destroyed(void*, void*) {}

EXPORT void vocem_gl_egl_context_destroyed(void*, void*) {}

EXPORT int vocem_stub_presents() {
    return g_presents;
}
