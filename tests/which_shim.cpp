// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which shim answers for the present functions in this process: the object
// that defines glXSwapBuffers and eglSwapBuffers first in the global scope,
// named by dladdr. dev_run_gl.cmake runs it through scripts/dev-run-gl.sh.

#include <dlfcn.h>
#include <stdio.h>

int main() {
    static const char* const names[] = {"glXSwapBuffers", "eglSwapBuffers"};
    for (const char* name : names) {
        void* found = dlsym(RTLD_DEFAULT, name);
        Dl_info info{};
        const char* where = found && dladdr(found, &info) && info.dli_fname ? info.dli_fname
                                                                             : "(nothing)";
        printf("%s %s\n", name, where);
    }
    return 0;
}
