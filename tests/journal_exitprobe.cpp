// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The journal's life exactly as an injected library lives it: begun from an
// exported call mid-life, ended by an ELF destructor at process exit. The
// test (journal_exit.cpp) dlopens this the way the shim dlopens the heavy
// library, because that is the configuration whose exit order broke: glibc
// runs _dl_fini LAST, after every function-local static's destructor, and a
// journal path kept in a static std::string was freed memory by the time the
// rename ran -- so the rename silently did nothing, every clean session
// stayed `.running`, and the repo's own test never noticed because it called
// journal_end() directly. The wrong witness, entry 38's shape, in the test
// that existed to prevent it.

#include "vocem/journal.h"

extern "C" void probe_begin(const char* process) {
    vocem::journal_begin("opengl", process);
    vocem::journal_note("probe alive");
}

namespace {

__attribute__((destructor)) void probe_journal_close() { vocem::journal_end(); }

}  // namespace
