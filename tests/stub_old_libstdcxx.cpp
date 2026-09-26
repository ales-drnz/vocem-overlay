// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A libstdc++.so.6 that is too old: the soname a game's runtime ships, with the
// version nodes of GCC 5's libstdc++ 6.0.21 (GLIBCXX up to 3.4.21, CXXABI up to
// 1.3.9) and none of the newer ones -- the shape of the Steam scout runtime's
// copy the 0.1.10 review loaded ahead of the system's. One symbol per node
// (stub_old_libstdcxx.map), because a node with nothing in it is not emitted.
// Nothing here is ever called: ld.so refuses a library that needs a newer node
// before it binds a single symbol, which is exactly the refusal
// gl_old_libstdcxx measures. Linked with -nostdlib++, so the stub needs no C++
// runtime -- it would otherwise NEED the very soname it carries.

extern "C" {

int vocem_stub_glibcxx_3_4() { return 0; }
int vocem_stub_glibcxx_3_4_1() { return 0; }
int vocem_stub_glibcxx_3_4_2() { return 0; }
int vocem_stub_glibcxx_3_4_3() { return 0; }
int vocem_stub_glibcxx_3_4_4() { return 0; }
int vocem_stub_glibcxx_3_4_5() { return 0; }
int vocem_stub_glibcxx_3_4_6() { return 0; }
int vocem_stub_glibcxx_3_4_7() { return 0; }
int vocem_stub_glibcxx_3_4_8() { return 0; }
int vocem_stub_glibcxx_3_4_9() { return 0; }
int vocem_stub_glibcxx_3_4_10() { return 0; }
int vocem_stub_glibcxx_3_4_11() { return 0; }
int vocem_stub_glibcxx_3_4_12() { return 0; }
int vocem_stub_glibcxx_3_4_13() { return 0; }
int vocem_stub_glibcxx_3_4_14() { return 0; }
int vocem_stub_glibcxx_3_4_15() { return 0; }
int vocem_stub_glibcxx_3_4_16() { return 0; }
int vocem_stub_glibcxx_3_4_17() { return 0; }
int vocem_stub_glibcxx_3_4_18() { return 0; }
int vocem_stub_glibcxx_3_4_19() { return 0; }
int vocem_stub_glibcxx_3_4_20() { return 0; }
int vocem_stub_glibcxx_3_4_21() { return 0; }
int vocem_stub_cxxabi_1_3() { return 0; }
int vocem_stub_cxxabi_1_3_1() { return 0; }
int vocem_stub_cxxabi_1_3_2() { return 0; }
int vocem_stub_cxxabi_1_3_3() { return 0; }
int vocem_stub_cxxabi_1_3_4() { return 0; }
int vocem_stub_cxxabi_1_3_5() { return 0; }
int vocem_stub_cxxabi_1_3_6() { return 0; }
int vocem_stub_cxxabi_1_3_7() { return 0; }
int vocem_stub_cxxabi_1_3_8() { return 0; }
int vocem_stub_cxxabi_1_3_9() { return 0; }

}  // extern "C"
