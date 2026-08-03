// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A second EGL, for tests/shim_two_egls.cpp.
//
// It plays the part ANGLE plays inside every Electron application: a library
// that exports its own `eglGetProcAddress`, opened privately, *before* the
// system's libEGL enters the process. Its dispatcher answers every name with a
// marker of its own, so a test can tell at a glance whose pointer it was handed.

static void stub_marker(void) {}

extern "C" {

__attribute__((visibility("default"))) void* eglGetProcAddress(const char* name) {
    (void)name;
    return (void*)&stub_marker;
}

// The same marker, reachable directly, so the test can compare against it.
__attribute__((visibility("default"))) void* vocem_stub_marker(void) {
    return (void*)&stub_marker;
}

}  // extern "C"
