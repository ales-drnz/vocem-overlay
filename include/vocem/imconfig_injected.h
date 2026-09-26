// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Dear ImGui's user configuration for every object that goes into a game: the
// two injected libraries and vocem_common, named to imgui.h through
// IMGUI_USER_CONFIG by their CMakeLists.
//
// ImGui's IM_ASSERT is assert() (imgui.h), hundreds of them in the units the
// injected libraries compile, and every one is an abort() in somebody's game if
// it fires (rule 7). The three CMakeLists used to say so with a
// "IM_ASSERT(_EXPR)=((void)0)" compile definition, which CMake drops with a
// warning -- a function-style macro cannot be passed on a command line -- so
// the rule held only where a build type defined NDEBUG. A Debug build of
// libvocem_gl.so and libvocem_vk.so imported __assert_fail (measured with
// nm -D, one reference each); with this header neither does.
//
// VOCEM_IMCONFIG_INJECTED lets a source that includes imgui.h refuse to compile
// without this file in effect, which is the check that holds the rule in every
// build type: the tests' own build is RelWithDebInfo, where NDEBUG would hide
// its absence.

#pragma once

#define IM_ASSERT(_EXPR) ((void)0)
#define VOCEM_IMCONFIG_INJECTED 1
