// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A privately-shipped EGL, for tests/shim_private_dispatch.cpp.
//
// Where `stub_egl.cpp` plays ANGLE arriving *privately* (RTLD_LOCAL, invisible
// to RTLD_NEXT), this one is meant to be opened RTLD_GLOBAL, so that it lands in
// the shim's own RTLD_NEXT search order and fills the `next` slot the shim
// deliberately does not vet. That is the only way to put a pointer the system's
// GL stack does not own behind the shim's dispatch hooks.
//
// It exports a dispatcher and the two names the test asks that dispatcher for,
// each answering with a marker of its own so the test can say whose pointer it
// was handed rather than merely that it got one. Its SONAME, and its file
// name, are nothing `is_system_gl` recognises, which is the whole point: this library is exactly
// what entry 36 calls a private GL.

static void swap_marker(void) {}
static void proc_marker(void) {}

extern "C" {

__attribute__((visibility("default"))) void* eglGetProcAddress(const char* name) {
    (void)name;
    // Every name answers with the same marker, as the other stub does: what the
    // test asks is whose library answered, not which function.
    return (void*)&swap_marker;
}

__attribute__((visibility("default"))) void eglSwapBuffers(void) {}

// The markers, reachable directly, so the test can compare against them.
__attribute__((visibility("default"))) void* vocem_private_swap_marker(void) {
    return (void*)&swap_marker;
}

__attribute__((visibility("default"))) void* vocem_private_proc_marker(void) {
    return (void*)&proc_marker;
}

}  // extern "C"
