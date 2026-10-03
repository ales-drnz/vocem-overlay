# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The shim as built, and the link that builds it.
#
# The shim is preloaded into every process of the session, so what it is made of
# is a property of the session: one NEEDED (libc), exactly its twelve hooks
# exported, no __cxa_guard (a function-local static with a dynamic initialiser
# pulls the C++ runtime in), and nothing undefined that libc does not answer.
# CLAUDE.md asks for the first three on every package with readelf and nm --
# by hand, and no ctest asked at all. Held here on the three shims this project
# builds: this tree's, the tests' own -m32 twin, and build32's (looked for now).
#
# And the link itself. The shim is linked with -nostdlib++, and until 0.1.11
# nothing refused an undefined symbol: a toy using `new` linked with the shim's
# own flags SUCCEEDED, with `U _Znam` and a single NEEDED, and preloaded under
# LD_BIND_NOW=1 every C program failed to start ("symbol lookup error"). The
# shim is linked with --no-undefined now; this compiles such a toy with the
# flags the shim targets actually carry and requires the link to be refused.
#
# Expects CXX, SHIM, SHIM_TWIN32, SHIM_BUILD32, and the targets' compile and
# link options joined with `|` (FLAGS, LINK_FLAGS, FLAGS32, LINK_FLAGS32).

set(exports
    dlsym eglDestroyContext eglGetProcAddress eglSwapBuffers eglSwapBuffersWithDamageEXT
    eglSwapBuffersWithDamageKHR eglTerminate glXDestroyContext glXGetProcAddress
    glXGetProcAddressARB glXSwapBuffers glXSwapBuffersMscOML)
list(SORT exports)
# Weak references the toolchain's own start files leave in every shared object;
# nothing calls them unless they are defined.
set(weak_allowed __gmon_start__ _ITM_deregisterTMCloneTable _ITM_registerTMCloneTable)

set(held 0)
foreach(shim IN ITEMS "${SHIM}" "${SHIM_TWIN32}" "${SHIM_BUILD32}")
    if(NOT shim OR NOT EXISTS "${shim}")
        continue()
    endif()
    execute_process(COMMAND readelf -d "${shim}" OUTPUT_VARIABLE dynamic RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "readelf could not read ${shim}")
    endif()
    # The NEEDED lines alone (the SONAME line carries a bracketed name too); the
    # tag is matched and not the words beside it, which are in the locale.
    string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]]+\\]" needed_lines "${dynamic}")
    set(needed "")
    foreach(line IN LISTS needed_lines)
        string(REGEX MATCH "\\[[^]]+\\]" name "${line}")
        list(APPEND needed "${name}")
    endforeach()
    if(NOT needed STREQUAL "[libc.so.6]")
        message(FATAL_ERROR "${shim} NEEDS ${needed}: the shim is preloaded into every process "
                            "of the session and needs libc alone")
    endif()

    execute_process(COMMAND nm -D --defined-only "${shim}" OUTPUT_VARIABLE defined)
    string(REGEX MATCHALL "[0-9a-f]+ [A-Za-z] [^\n]+" lines "${defined}")
    set(names "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^[0-9a-f]+ [A-Za-z] " "" name "${line}")
        string(REGEX REPLACE "@.*" "" name "${name}")
        list(APPEND names "${name}")
    endforeach()
    list(SORT names)
    if(NOT names STREQUAL exports)
        message(FATAL_ERROR "${shim} exports ${names}; the shim exports exactly its twelve "
                            "hooks: ${exports}")
    endif()

    execute_process(COMMAND nm -D "${shim}" OUTPUT_VARIABLE all_symbols)
    if(all_symbols MATCHES "__cxa_guard")
        message(FATAL_ERROR "${shim} references __cxa_guard: a function-local static with a "
                            "dynamic initialiser, which needs the C++ runtime")
    endif()

    execute_process(COMMAND nm -D --undefined-only "${shim}" OUTPUT_VARIABLE undefined)
    string(REGEX MATCHALL "[Uw] [^\n]+" references "${undefined}")
    foreach(reference IN LISTS references)
        string(REGEX REPLACE "^[Uw] " "" name "${reference}")
        if(name MATCHES "@GLIBC_")
            continue()
        endif()
        if(reference MATCHES "^w " AND name IN_LIST weak_allowed)
            continue()
        endif()
        message(FATAL_ERROR "${shim} leaves ${name} undefined, which libc does not answer: "
                            "preloaded under LD_BIND_NOW every program would fail to start")
    endforeach()
    # The newest glibc the shim needs, held where it is: GLIBC_2.34, where libdl
    # joined libc. Not because a known place runs the shim on an older libc --
    # pressure-vessel brings the host's -- but because the one file loaded into
    # every process of the session should need what it needs on purpose, and a
    # compiler or a makepkg flag raises it silently: the heavy libraries went
    # to GLIBC_2.43 that way (acosf, atan2f, sqrtf; readelf, 0.1.12-2), and
    # -static-libstdc++ brought _dl_find_object@GLIBC_2.35 with it (entry 234).
    # Raising it is a decision; change this number with the reason beside it
    # (entry 307).
    execute_process(COMMAND nm -D --with-symbol-versions --undefined-only "${shim}"
                    OUTPUT_VARIABLE versioned)
    string(REGEX MATCHALL "@GLIBC_[0-9]+\\.[0-9]+(\\.[0-9]+)?" glibc_versions "${versioned}")
    set(newest "0")
    foreach(version IN LISTS glibc_versions)
        string(REPLACE "@GLIBC_" "" version "${version}")
        if(version VERSION_GREATER newest)
            set(newest "${version}")
        endif()
    endforeach()
    if(newest VERSION_GREATER "2.34")
        string(REGEX MATCHALL "[^\n ]+@GLIBC_${newest}" culprits "${versioned}")
        message(FATAL_ERROR "${shim} needs GLIBC_${newest} (${culprits}); the shim has needed "
                            "2.34 at most, and a rise is a decision, not a side effect")
    endif()
    message(STATUS "ok ${shim}: libc alone, exactly the twelve hooks, no guard, "
                   "nothing undefined outside libc, glibc ${newest} at most")
    math(EXPR held "${held} + 1")
endforeach()
if(held EQUAL 0)
    message(FATAL_ERROR "no shim was found to hold: ${SHIM}")
endif()

# The link refuses what the artefact checks above would only catch afterwards.
set(work "${CMAKE_CURRENT_BINARY_DIR}/shim_artifact")
file(MAKE_DIRECTORY "${work}")
file(WRITE "${work}/toy.cpp"
    "extern \"C\" int toy(int n) { int* p = new int[n]; int r = p[0]; delete[] p; return r; }\n")
foreach(width IN ITEMS 64 32)
    if(width STREQUAL "64")
        set(compile "${FLAGS}")
        set(link "${LINK_FLAGS}")
    else()
        if(NOT FLAGS32)
            continue()
        endif()
        set(compile "${FLAGS32}")
        set(link "${LINK_FLAGS32}")
    endif()
    string(REPLACE "|" ";" compile "${compile}")
    string(REPLACE "|" ";" link "${link}")
    execute_process(
        COMMAND "${CXX}" -shared -fPIC ${compile} ${link} -o "${work}/libtoy${width}.so"
                "${work}/toy.cpp"
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(status EQUAL 0)
        message(FATAL_ERROR "a library using `new` linked with the shim's own flags (${width}-bit: "
                            "${compile} ${link}) and the link did not refuse it: the shim's link "
                            "would pass an undefined C++ runtime symbol straight into every "
                            "process of the session")
    endif()
    if(NOT err MATCHES "undefined reference")
        message(FATAL_ERROR "the ${width}-bit toy failed to link for another reason:\n${err}")
    endif()
    message(STATUS "ok the shim's ${width}-bit link refuses an undefined symbol")
endforeach()

if(SHIM_BUILD32 AND NOT EXISTS "${SHIM_BUILD32}")
    message(STATUS "skip ${SHIM_BUILD32} is not built: build32's shim was not held, and the "
                   "32-bit half is the one that has failed invisibly (entries 30/33/34)")
endif()
