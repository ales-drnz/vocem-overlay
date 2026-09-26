# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The two injected heavy libraries carry their C++ runtime inside them.
#
# libvocem_gl.so and libvocem_vk.so are loaded into games, and a game's runtime
# can put an older libstdc++.so.6 first on the search path: the 0.1.10 builds
# NEEDED libstdc++.so.6 at GLIBCXX_3.4.29 (3.4.30 as packaged), and with the
# Steam scout runtime's 6.0.21 ahead of the system's ld.so refused the whole
# library -- the game had no overlay, and nothing said so
# (tests/gl_old_libstdcxx.cpp is the behaviour; this is the artefact). Both are
# linked with -static-libstdc++ -static-libgcc now (the top-level
# CMakeLists.txt), and their version scripts keep the copy's symbols local.
#
# Held here: no NEEDED entry naming libstdc++ or libgcc_s, and no dynamic symbol
# bound to a GLIBCXX_, CXXABI_ or GCC_ version, at both widths. The 32-bit files
# are build32's, looked for now; a missing one is said at the end, after the
# 64-bit half has been held.

# GL and VK are this tree's; GL32 and VK32 are build32's (one argument each:
# a list would be split on its way through add_test).
set(LIBRARIES "${GL}" "${VK}")
set(LIBRARIES32 "${GL32}" "${VK32}")
set(held 0)
foreach(library IN LISTS LIBRARIES LIBRARIES32)
    if(NOT EXISTS "${library}")
        continue()
    endif()
    execute_process(COMMAND readelf -d "${library}" OUTPUT_VARIABLE dynamic RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "readelf could not read ${library}")
    endif()
    string(REGEX MATCHALL "\\[(libstdc\\+\\+|libgcc_s)[^]]*\\]" needed "${dynamic}")
    if(needed)
        message(FATAL_ERROR "${library} NEEDS ${needed}: a game whose runtime ships an older "
                            "C++ runtime first on its search path refuses the whole library")
    endif()
    execute_process(COMMAND objdump -T "${library}" OUTPUT_VARIABLE symbols RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "objdump could not read ${library}")
    endif()
    string(REGEX MATCHALL "(GLIBCXX|CXXABI|GCC)_[0-9.]+" versions "${symbols}")
    if(versions)
        list(REMOVE_DUPLICATES versions)
        message(FATAL_ERROR "${library} binds symbols of the C++ runtime's versions ${versions}")
    endif()
    message(STATUS "ok ${library}: its C++ runtime is inside it")
    math(EXPR held "${held} + 1")
endforeach()

if(held EQUAL 0)
    message(FATAL_ERROR "none of the injected libraries was found: ${LIBRARIES}")
endif()
foreach(library IN LISTS LIBRARIES32)
    if(NOT EXISTS "${library}")
        message(STATUS "skip ${library} is not built: the runtime was held at one width only, "
                       "and the 32-bit half is the one that has failed invisibly "
                       "(entries 30/33/34)")
    endif()
endforeach()
