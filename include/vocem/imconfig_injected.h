// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Dear ImGui's user configuration for every object that goes into a game: the
// two injected libraries and vocem_common, named to imgui.h through
// IMGUI_USER_CONFIG by their CMakeLists.
//
// ImGui's IM_ASSERT is assert(), and every one is an abort() in somebody's game
// if it fires (rule 7). A header, not a -D: CMake drops a function-style macro
// from compile definitions with a warning. VOCEM_IMCONFIG_INJECTED
// lets a source that includes imgui.h refuse to compile without this file, which
// holds the rule in every build type, NDEBUG or not.

#pragma once

#define IM_ASSERT(_EXPR) ((void)0)
#define VOCEM_IMCONFIG_INJECTED 1
