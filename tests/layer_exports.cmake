# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The Vulkan layer exports what the loader asks of it and nothing else.
#
# Entry 16's rule, for the library the loader dlopens into every Vulkan
# process: the negotiation entry point and the two GetProcAddr functions it
# hands back. Hidden visibility holds the layer's own code to that and cannot
# hold the libstdc++ templates it instantiates, which namespace std declares
# with default visibility -- a build without LTO exported four of them at 64
# bits and six at 32 (entry 195), and nothing counted them. Both widths when
# both are built; the 32-bit half is a skip said at the end when it is not.

set(allowed vkNegotiateLoaderLayerInterfaceVersion vocem_GetInstanceProcAddr
            vocem_GetDeviceProcAddr)
if(NOT EXISTS "${LIBRARY}")
    message(STATUS "skip the Vulkan layer was not built")
    return()
endif()
set(libraries "${LIBRARY}")
if(LIBRARY32 AND EXISTS "${LIBRARY32}")
    list(APPEND libraries "${LIBRARY32}")
endif()
foreach(library IN LISTS libraries)
    execute_process(COMMAND nm -D --defined-only "${library}"
                    OUTPUT_VARIABLE symbols RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "nm could not read ${library}")
    endif()
    string(REGEX MATCHALL "[0-9a-f]+ [A-Za-z] [^\n]+" defined_raw "${symbols}")
    set(found 0)
    foreach(line IN LISTS defined_raw)
        string(REGEX REPLACE "^[0-9a-f]+ [A-Za-z] " "" name "${line}")
        list(FIND allowed "${name}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "${library} exports ${name}: a layer exports what the loader "
                                "asks of it and nothing else (entry 16)")
        endif()
        math(EXPR found "${found} + 1")
    endforeach()
    list(LENGTH allowed allowed_count)
    if(NOT found EQUAL allowed_count)
        message(FATAL_ERROR "${library} exports ${found} names where the loader needs "
                            "${allowed_count}: ${allowed}")
    endif()
    message(STATUS "ok ${library}: exactly the ${allowed_count} names the loader asks for")
endforeach()
if(LIBRARY32 AND NOT EXISTS "${LIBRARY32}")
    message(STATUS "skip ${LIBRARY32} is not built: held at one width only, and the 32-bit "
                   "half is the one that has failed invisibly (entries 30/33/34)")
endif()
