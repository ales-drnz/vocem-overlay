# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The shapes a test in this directory takes, each spelled once.
#
# tests/CMakeLists.txt ran to 1600 lines before this file existed: sixty-odd
# add_executable blocks each naming ${CMAKE_SOURCE_DIR}/include, nine identical
# fifteen-line "ImGui core + defines" blocks, eleven -m32 twins each repeating
# their flags and their skip fallback, and the skip discipline of entry 123 in
# five spellings of SKIP_REGULAR_EXPRESSION. A test added by copying its
# neighbour copied whichever spelling the neighbour had -- which is how twelve
# window tests came to report a skip as a pass (entry 123), and how a
# 32-bit twin could be built with flags the shipped shim does not have.
#
#   vocem_test(<name> ...)        an executable vocem_<name> and the test <name>
#   vocem_test_run(<name> ...)    another test on an executable already built
#   vocem_m32_twin(<name> ...)    the same sources at -m32, or an honest skip
#   vocem_script_test(<name> ...) a cmake -P script, with the one skip spelling
#   vocem_skip(<name> <reason>)   a test that only says why it did not run
#
# Every shape but the skip also takes the two words that decide what may run
# beside it under `ctest -j` (DESIGN 193):
#   LOCK <resource>...  ctest's RESOURCE_LOCK: never at the same time as
#                       another test holding the same name
#   SERIAL              ctest's RUN_SERIAL: never beside anything at all
# Neither is a default. The suite runs in parallel because it was MEASURED to
# (eight -j8/-j16 runs), and each lock names the contention a run showed or the
# inventory found -- a lock nobody measured is how the next one would hide.
#
# Two rules every shape carries. A skip is exit code 77 from a compiled probe
# and the STATUS-prefixed `-- skip ` line from a script or a fallback -- never
# the bare word, which two passing tests speak in sentences. And a 32-bit
# twin links the 32-bit build of whatever its 64-bit original links
# (vocem_common32, the -m32 ImGui core), so a difference between the two is the
# architecture's and nothing else's (the widths.cpp pattern, entries 30/33/34).

set(IMGUI_DIR "${PROJECT_SOURCE_DIR}/third_party/imgui")

# Where the 32-bit injected libraries are built. A tree of its own, configured
# with -m32 and tests off (CLAUDE.md); ctest does not cover it, so the tests
# that need its artefacts name them here and resolve them WHEN THEY RUN --
# a find_library at configure time made a build32 created afterwards invisible
# and one deleted afterwards a stale path (with_build32.cmake).
set(VOCEM_BUILD32_DIR "${CMAKE_SOURCE_DIR}/build32" CACHE PATH
    "The -m32 build tree whose injected libraries the 32-bit probes run against")

# --- Dear ImGui's core, once per width ---------------------------------------
# Object libraries: a test that draws links the four core units and gets the
# include directory and the three definitions with them. The definitions are
# the ones every ImGui in this project is built under (gl/, layer/, common/):
# IMGUI_USE_WCHAR32 changes the width of every glyph index, so an object built
# without it beside one built with it would be a silent layout mismatch.
# Third-party code is built with -w so its output does not drown ours.
function(_vocem_imgui_core target)
    add_library(${target} OBJECT
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp")
    target_include_directories(${target} PUBLIC "${IMGUI_DIR}")
    target_compile_definitions(${target} PUBLIC
        IMGUI_USE_WCHAR32 IMGUI_DISABLE_FILE_FUNCTIONS IMGUI_DISABLE_DEBUG_TOOLS)
    target_compile_options(${target} PRIVATE -w ${ARGN})
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

_vocem_imgui_core(vocem_imgui_core)
if(VOCEM_HAVE_M32)
    _vocem_imgui_core(vocem_imgui_core_m32 -m32)
endif()

# --- a test that only says why it did not run --------------------------------
# ctest's own Skipped state, which the suite's discipline (zero skips on this
# machine) refuses instead of counting as a pass. `-- skip ` is STATUS's own
# prefix, so a script's message(STATUS "skip ...") and this echo are one
# spelling.
# The reason is every remaining argument joined, not the first one: callers
# write it as several adjacent strings the way every message() in this suite
# does, and taking one parameter silently dropped the rest -- vocem_m32_twin's
# own call passes two, and the half naming entries 30/33/34 never reached the
# output. Unreachable on this machine, where VOCEM_HAVE_M32 is set, which is
# why nobody saw it.
function(vocem_skip name)
    string(JOIN "" reason ${ARGN})
    if(reason STREQUAL "")
        message(FATAL_ERROR "vocem_skip(${name}): a skip with no reason is not a skip")
    endif()
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -E echo "-- skip ${reason}")
    set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "-- skip ")
endfunction()

# The keywords vocem_test and vocem_m32_twin share.
set(_VOCEM_TEST_FLAGS SKIP IMGUI NO_TEST SERIAL)
set(_VOCEM_TEST_ONE TIMEOUT)
set(_VOCEM_TEST_MANY SOURCES LINK ENV ARGS DEFINES INCLUDE COMPILE_OPTIONS LINK_OPTIONS BUILD32 LOCK)

# LOCK and SERIAL, spelled once for every shape.
function(_vocem_sharing name serial locks)
    if(serial)
        set_tests_properties(${name} PROPERTIES RUN_SERIAL ON)
    endif()
    if(locks)
        set_tests_properties(${name} PROPERTIES RESOURCE_LOCK "${locks}")
    endif()
endfunction()

# The test itself, for vocem_test and vocem_test_run alike. With BUILD32 files
# it runs through with_build32.cmake, which looks for them when the test runs
# and skips out loud, by file name, when they are not there; a scenario of a
# probe that needs the 32-bit tree needs it exactly as the probe does, and the
# scenarios' twins once went straight to the executable instead, so a machine
# without build32 read nine FAILs where the probe itself read one skip.
function(_vocem_add_test name target build32 env args skip)
    if(build32)
        add_test(NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
                    "-DPROBE=$<TARGET_FILE:${target}>"
                    "-DREQUIRES=${build32}"
                    "-DENV=${env}"
                    "-DARGS=${args}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/with_build32.cmake")
        set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "-- skip ")
    else()
        add_test(NAME ${name} COMMAND ${target} ${args})
        if(env)
            set_tests_properties(${name} PROPERTIES ENVIRONMENT "${env}")
        endif()
        if(skip)
            set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endfunction()

# The one definition behind vocem_test and vocem_m32_twin.
#
#   SOURCES         default <name>.cpp
#   LINK            libraries; vocem_common becomes vocem_common32 at -m32
#   IMGUI           link the ImGui core of this width (and take its defines)
#   ENV             the test's environment, one KEY=VALUE per item
#   ARGS            arguments to the executable
#   DEFINES, INCLUDE, COMPILE_OPTIONS, LINK_OPTIONS -- as the target properties
#   SKIP            the probe exits 77 to say it could not measure here
#   TIMEOUT         seconds
#   NO_TEST         build the executable only (a probe another test drives)
#   BUILD32 <files> the test needs these artefacts of the 32-bit tree: it runs
#                   through with_build32.cmake, which looks for them at run time
#                   and skips out loud when they are not there
function(_vocem_define name m32)
    cmake_parse_arguments(PARSE_ARGV 2 T
        "${_VOCEM_TEST_FLAGS}" "${_VOCEM_TEST_ONE}" "${_VOCEM_TEST_MANY}")
    if(T_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vocem_test(${name}): unknown arguments ${T_UNPARSED_ARGUMENTS}")
    endif()
    set(target vocem_${name})
    if(NOT T_SOURCES)
        set(T_SOURCES ${name}.cpp)
    endif()
    add_executable(${target} ${T_SOURCES})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/include" ${T_INCLUDE})
    if(T_DEFINES)
        target_compile_definitions(${target} PRIVATE ${T_DEFINES})
    endif()
    if(T_COMPILE_OPTIONS)
        target_compile_options(${target} PRIVATE ${T_COMPILE_OPTIONS})
    endif()
    if(T_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${T_LINK_OPTIONS})
    endif()
    set(libraries ${T_LINK})
    if(m32)
        target_compile_options(${target} PRIVATE -m32)
        target_link_options(${target} PRIVATE -m32)
        list(TRANSFORM libraries REPLACE "^vocem_common$" "vocem_common32")
        if(T_IMGUI)
            list(APPEND libraries vocem_imgui_core_m32)
        endif()
    elseif(T_IMGUI)
        list(APPEND libraries vocem_imgui_core)
    endif()
    if(libraries)
        target_link_libraries(${target} PRIVATE ${libraries})
    endif()
    if(T_NO_TEST)
        return()
    endif()

    _vocem_add_test(${name} ${target} "${T_BUILD32}" "${T_ENV}" "${T_ARGS}" "${T_SKIP}")
    if(T_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
    endif()
    _vocem_sharing(${name} "${T_SERIAL}" "${T_LOCK}")
endfunction()

# An executable vocem_<name> and the test <name> that runs it. Keywords above.
function(vocem_test name)
    set_property(GLOBAL PROPERTY VOCEM_TEST_ARGS_${name} "${ARGN}")
    set_property(GLOBAL PROPERTY VOCEM_TEST_DEFINED_${name} ON)
    _vocem_define(${name} OFF ${ARGN})
endfunction()

# Another test on an executable vocem_test already built: the same probe under
# another environment or with other arguments.
#   TARGET <name>   the vocem_test whose executable runs (default: <name>)
#   ENV, ARGS, SKIP, TIMEOUT, BUILD32 as above
#   SETUP <fixture>     this run produces something other runs read
#   REQUIRES <fixture>  this run reads what a SETUP run produced: ctest runs
#                       that one first even under -R, and does not run this one
#                       when it failed (DEPENDS only orders two tests that
#                       are both selected).
function(vocem_test_run name)
    cmake_parse_arguments(PARSE_ARGV 1 R "SKIP;SERIAL" "TARGET;TIMEOUT"
        "ENV;ARGS;LOCK;BUILD32;SETUP;REQUIRES")
    if(R_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vocem_test_run(${name}): unknown arguments ${R_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT R_TARGET)
        message(FATAL_ERROR "vocem_test_run(${name}) needs TARGET")
    endif()
    # Remembered so that vocem_m32_twin_run can twin a SCENARIO and not only a
    # test. Nothing recorded this, so `vk_present_draw` had eight scenarios at
    # 64 bits and one at 32 -- in the project whose own law is that the 64-bit
    # build passing is evidence about nothing (entries 30/33/34).
    set_property(GLOBAL PROPERTY VOCEM_RUN_ARGS_${name} "${ARGN}")
    set_property(GLOBAL PROPERTY VOCEM_RUN_DEFINED_${name} ON)
    _vocem_add_test(${name} vocem_${R_TARGET} "${R_BUILD32}" "${R_ENV}" "${R_ARGS}" "${R_SKIP}")
    if(R_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${R_TIMEOUT})
    endif()
    if(R_SETUP)
        set_tests_properties(${name} PROPERTIES FIXTURES_SETUP "${R_SETUP}")
    endif()
    if(R_REQUIRES)
        set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${R_REQUIRES}")
    endif()
    _vocem_sharing(${name} "${R_SERIAL}" "${R_LOCK}")
endfunction()

# The 32-bit twin of a vocem_test_run: the same scenario against the 32-bit
# executable of the same probe. TARGET names the *64-bit* run to twin, and its
# ENV/ARGS are taken from it unless given again here -- which is what an ENV
# naming build32's libraries is for.
#
# Why it exists: vocem_m32_twin twins a TEST, so a probe whose behaviour is
# chosen by an environment variable had exactly one of its scenarios measured
# at 32 bits. The widths differ in a pointer and a size_t, and every one of
# these scenarios is about lifetimes and bookkeeping.
function(vocem_m32_twin_run name)
    cmake_parse_arguments(PARSE_ARGV 1 O "" "TARGET" "ENV")
    if(O_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vocem_m32_twin_run(${name}): unknown ${O_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT O_TARGET)
        message(FATAL_ERROR "vocem_m32_twin_run(${name}) needs TARGET")
    endif()
    get_property(defined GLOBAL PROPERTY VOCEM_RUN_DEFINED_${O_TARGET})
    if(NOT defined)
        message(FATAL_ERROR "vocem_m32_twin_run(${name}): no vocem_test_run(${O_TARGET})")
    endif()
    get_property(stored GLOBAL PROPERTY VOCEM_RUN_ARGS_${O_TARGET})
    cmake_parse_arguments(R "SKIP;SERIAL" "TARGET;TIMEOUT" "ENV;ARGS;LOCK;BUILD32;SETUP;REQUIRES"
        ${stored})
    if(NOT VOCEM_HAVE_M32)
        vocem_skip(${name}32
            "no 32-bit toolchain, so the ${O_TARGET} scenario is measured at one width only -- "
            "the width that has failed invisibly three times (entries 30/33/34)")
        return()
    endif()
    if(NOT TARGET vocem_${R_TARGET}32)
        message(FATAL_ERROR
            "vocem_m32_twin_run(${name}): there is no vocem_${R_TARGET}32 to run -- the probe "
            "itself needs a vocem_m32_twin before its scenarios can have one")
    endif()
    set(args TARGET ${R_TARGET}32)
    if(DEFINED O_ENV)
        list(APPEND args ENV ${O_ENV})
    elseif(R_ENV)
        list(APPEND args ENV ${R_ENV})
    endif()
    if(R_ARGS)
        list(APPEND args ARGS ${R_ARGS})
    endif()
    if(R_TIMEOUT)
        list(APPEND args TIMEOUT ${R_TIMEOUT})
    endif()
    if(R_SKIP)
        list(APPEND args SKIP)
    endif()
    if(R_SERIAL)
        list(APPEND args SERIAL)
    endif()
    if(R_LOCK)
        list(APPEND args LOCK ${R_LOCK})
    endif()
    # The 32-bit tree's artefacts the probe's own twin named: a scenario of that
    # probe runs against the same ones.
    get_property(build32 GLOBAL PROPERTY VOCEM_TWIN_BUILD32_${R_TARGET})
    if(build32)
        list(APPEND args BUILD32 ${build32})
    endif()
    vocem_test_run(${name}32 ${args})
endfunction()

# The same sources as vocem_test(<name>) at -m32: executable vocem_<name>32,
# test <name>32, every library swapped for its 32-bit build. Without a 32-bit
# toolchain the test exists and reports itself skipped, so a machine that
# measures one width only says so in ctest rather than passing vacuously.
#   ENV <items>      replaces the original's environment (a shim of the other
#                    width, the 32-bit tree's library)
#   BUILD32 <files>  the 32-bit tree's artefacts this width needs (see above)
function(vocem_m32_twin name)
    cmake_parse_arguments(PARSE_ARGV 1 O "" "" "ENV;BUILD32")
    if(O_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vocem_m32_twin(${name}): unknown arguments ${O_UNPARSED_ARGUMENTS}")
    endif()
    get_property(defined GLOBAL PROPERTY VOCEM_TEST_DEFINED_${name})
    if(NOT defined)
        message(FATAL_ERROR "vocem_m32_twin(${name}): no vocem_test(${name}) to twin")
    endif()
    get_property(stored GLOBAL PROPERTY VOCEM_TEST_ARGS_${name})
    cmake_parse_arguments(T "${_VOCEM_TEST_FLAGS}" "${_VOCEM_TEST_ONE}" "${_VOCEM_TEST_MANY}" ${stored})
    if(NOT VOCEM_HAVE_M32)
        if(NOT T_NO_TEST)
            vocem_skip(${name}32
                "no 32-bit toolchain, so ${name} is measured at one width only -- "
                "the width that has failed invisibly three times (entries 30/33/34)")
        endif()
        return()
    endif()
    if(DEFINED O_ENV)
        set(T_ENV ${O_ENV})
    endif()
    if(DEFINED O_BUILD32)
        set(T_BUILD32 ${O_BUILD32})
    endif()
    set_property(GLOBAL PROPERTY VOCEM_TWIN_BUILD32_${name} "${T_BUILD32}")
    if(NOT T_SOURCES)
        set(T_SOURCES ${name}.cpp)
    endif()
    set(args SOURCES ${T_SOURCES})
    foreach(keyword IN LISTS _VOCEM_TEST_MANY)
        if(NOT keyword STREQUAL "SOURCES" AND T_${keyword})
            list(APPEND args ${keyword} ${T_${keyword}})
        endif()
    endforeach()
    foreach(flag IN LISTS _VOCEM_TEST_FLAGS)
        if(T_${flag})
            list(APPEND args ${flag})
        endif()
    endforeach()
    if(T_TIMEOUT)
        list(APPEND args TIMEOUT ${T_TIMEOUT})
    endif()
    _vocem_define(${name}32 ON ${args})
endfunction()

# A test that is a cmake -P script. Every script gets SOURCE_DIR and
# CMAKE_CURRENT_BINARY_DIR (the window-driving ones refuse to run without a
# test directory, entry 137); ARGS adds the rest as -DNAME=value. The skip
# spelling is set here, once, for every script -- entry 123 is what its
# absence cost.
#   SCRIPT <file>   default <name>.cmake, in this directory
function(vocem_script_test name)
    cmake_parse_arguments(PARSE_ARGV 1 S "SERIAL" "SCRIPT;TIMEOUT" "ARGS;LOCK")
    if(S_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vocem_script_test(${name}): unknown arguments ${S_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT S_SCRIPT)
        set(S_SCRIPT ${name}.cmake)
    endif()
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                ${S_ARGS}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/${S_SCRIPT}")
    set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "-- skip ")
    if(S_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${S_TIMEOUT})
    endif()
    _vocem_sharing(${name} "${S_SERIAL}" "${S_LOCK}")
endfunction()
