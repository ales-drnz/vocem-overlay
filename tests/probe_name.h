// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What this probe is called, asked of the kernel rather than written out.
//
// A probe that wants the overlay in itself writes `shown_apps = <its own
// name>` into a scratch config.ini, and `vocem::listed()` compares exactly --
// so a literal is right only for as long as nobody renames the binary, and
// wrong in one specific way that has already cost a measurement. Entry 129:
// built at -m32 the executable is `vocem_vk_present_draw32`, the literal said
// `vocem_vk_present_draw`, the detection quite correctly declined a process
// nobody had asked for, and the run read as "the 32-bit Vulkan layer loads and
// draws nothing" -- entries 30/33/34's defect precisely -- until somebody read
// the log and found `not drawing ...: does not look like a game`. "A test that
// hardcodes its own binary's name is a test that lies the day the binary is
// renamed, and this one lied about the defect class the project has been most
// careful about."
//
// That entry fixed the one probe it was about and nine others kept their
// literals, including two written the same month; `gl_avatar_width` survived
// only because `comm` truncates at fifteen characters, which nothing had
// written down. This is the one spelling.
//
// The name the overlay's own detection uses is the process name -- `comm`,
// which the kernel cuts to TASK_COMM_LEN-1 -- and `vocem::listed()` matches a
// rule against the process name OR the executable's name, so the untruncated
// basename of /proc/self/exe is what a rule should say: it matches the
// executable half exactly, whatever its length.

#ifndef VOCEM_TEST_PROBE_NAME_H
#define VOCEM_TEST_PROBE_NAME_H

#include <string.h>
#include <unistd.h>

#include <string>

namespace vocem_test {

// This executable's basename. `fallback` is used only if /proc is not there to
// ask, which on this machine it always is; a probe passing its own old literal
// keeps the behaviour it had rather than silently asking for nothing.
inline std::string own_name(const char* fallback = "") {
    char self[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return fallback;
    }
    self[n] = '\0';
    const char* slash = ::strrchr(self, '/');
    return slash ? slash + 1 : self;
}

}  // namespace vocem_test

#endif  // VOCEM_TEST_PROBE_NAME_H
