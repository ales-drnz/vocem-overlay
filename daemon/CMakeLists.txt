# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

find_package(CURL REQUIRED)

add_executable(vocemd
    src/main.cpp
    src/websocket.cpp
    src/avatars.cpp
    src/auth.cpp
    src/flatpak_bridge.cpp
)

target_include_directories(vocemd PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/third_party"
)

target_compile_options(vocemd PRIVATE -Wall -Wextra)

# GCC 16 reports a spurious -Warray-bounds inside libstdc++ when it inlines the
# destruction of a std::mutex held by AvatarCache's queue at -O2. The warning
# points at standard library internals, not at our code, so it is silenced for
# this translation unit only rather than project-wide.
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_OPTIONS "-Wno-array-bounds")

# The vendored decoder (Wuffs, via avatar_decode.h) compiles a number of helpers
# this build does not call, since the module list is broader than one call site
# needs. It lives in this process only -- the daemon is the only thing that
# decodes.
set_source_files_properties(src/avatars.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

# shm_open lives in librt on older glibc and in libc on current ones.
target_link_libraries(vocemd PRIVATE CURL::libcurl Threads::Threads)

find_library(VOCEM_RT_LIBRARY rt)
if(VOCEM_RT_LIBRARY)
    target_link_libraries(vocemd PRIVATE ${VOCEM_RT_LIBRARY})
endif()

install(TARGETS vocemd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
