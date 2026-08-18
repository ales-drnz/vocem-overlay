# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The vocem_gl_* names are a two-file contract, held in both directions.
#
# The shim resolves them as strings (gl/src/vocem_gl_shim.cpp) and the heavy
# library defines them as symbols (gl/src/vocem_gl.cpp); nothing else relates
# the two. A rename on one side -- and the pair is inviting exactly that, GLX's
# teardown being vocem_gl_context_destroyed where EGL's is
# vocem_gl_egl_context_destroyed -- leaves load_overlay() with a null pointer
# and produces entry 38's failure to the letter: shim loads, detection right,
# present hooks run, nothing draws, nothing says why. DESIGN's entry 45 states
# the invariant ("it exports exactly the four vocem_gl_* entry points the shim
# resolves"); this is that sentence as a measurement against the built
# artefact, at both widths when both are built.
#
# Both directions matter: a name asked and not exported is the dead overlay
# above; a vocem_gl_* export the shim never asks for is a second hook table
# starting to grow back (entry 45 deleted one).

if(NOT EXISTS "${LIBRARY}")
    message(STATUS "skip the heavy GL library was not built")
    return()
endif()

file(READ "${SOURCE_DIR}/gl/src/vocem_gl_shim.cpp" shim)
# Only resolution sites: the quoted names handed to real_dlsym.
string(REGEX MATCHALL "real_dlsym\\(handle, \"vocem_gl_[a-z_]+\"" asked_raw "${shim}")
set(asked "")
foreach(hit IN LISTS asked_raw)
    string(REGEX MATCH "vocem_gl_[a-z_]+" name "${hit}")
    list(APPEND asked "${name}")
endforeach()
list(REMOVE_DUPLICATES asked)
list(LENGTH asked asked_count)
if(asked_count LESS 4)
    message(FATAL_ERROR "only ${asked_count} vocem_gl_* resolutions found in the shim -- "
                        "the parse has stopped matching the source, which is not agreement")
endif()

foreach(library "${LIBRARY}" "${LIBRARY32}")
    if(NOT EXISTS "${library}")
        continue()
    endif()
    execute_process(COMMAND nm -D --defined-only "${library}"
                    OUTPUT_VARIABLE symbols RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "nm could not read ${library}")
    endif()
    # Asked -> exported.
    foreach(name IN LISTS asked)
        if(NOT symbols MATCHES " T ${name}\n")
            message(FATAL_ERROR "${library} does not export ${name}, which the shim resolves: "
                                "the overlay would load, hook, and silently never draw")
        endif()
    endforeach()
    # Exported -> asked.
    string(REGEX MATCHALL " T (vocem_gl_[a-z_]+)" exported_raw "${symbols}")
    foreach(hit IN LISTS exported_raw)
        string(REGEX MATCH "vocem_gl_[a-z_]+" name "${hit}")
        list(FIND asked "${name}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "${library} exports ${name} and the shim never resolves it: "
                                "a second hook table is growing back (entry 45)")
        endif()
    endforeach()
    message(STATUS "ok ${library}: exactly the ${asked_count} names the shim resolves")
endforeach()
