# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

add_executable(vocem src/main.cpp)

target_include_directories(vocem PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_options(vocem PRIVATE -Wall -Wextra)

find_library(VOCEM_RT_LIBRARY rt)
if(VOCEM_RT_LIBRARY)
    target_link_libraries(vocem PRIVATE ${VOCEM_RT_LIBRARY})
endif()

install(TARGETS vocem RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
