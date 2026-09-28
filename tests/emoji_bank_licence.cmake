# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The emoji bank is a Modified Version of Noto Color Emoji under the OFL 1.1,
# and a Modified Version travels with its licence. The bank shipped through
# 0.1.0-52 with no licence text anywhere.
#
# Held on what an install puts down, in a scratch DESTDIR: wherever the bank
# lands, the licence lands beside it, carrying the original copyright line and
# the OFL's name. Expects BUILD_DIR.

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests")
endif()

set(destdir "${CMAKE_CURRENT_BINARY_DIR}/installed-emoji-licence")
file(REMOVE_RECURSE "${destdir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix /usr
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    ENVIRONMENT_MODIFICATION "DESTDIR=set:${destdir}")
if(NOT status EQUAL 0)
    message("${output}${errors}")
    message(FATAL_ERROR "cmake --install into a scratch DESTDIR failed")
endif()

file(GLOB_RECURSE banks "${destdir}/*/emoji_bank.rgba")
list(LENGTH banks bank_count)
if(NOT bank_count EQUAL 1)
    file(REMOVE_RECURSE "${destdir}")
    message(FATAL_ERROR "the install put down ${bank_count} emoji banks, expected one")
endif()
get_filename_component(bank_dir "${banks}" DIRECTORY)
set(licence "${bank_dir}/OFL-NotoColorEmoji.txt")
string(REPLACE "${destdir}" "" shown "${bank_dir}")
if(NOT EXISTS "${licence}")
    file(REMOVE_RECURSE "${destdir}")
    message(FATAL_ERROR "the bank is installed in ${shown} without its licence beside it")
endif()
file(READ "${licence}" licence_text)
file(REMOVE_RECURSE "${destdir}")
if(NOT licence_text MATCHES "Copyright 2013 Google LLC")
    message(FATAL_ERROR "the installed licence does not carry the original copyright "
                        "notice (expected 'Copyright 2013 Google LLC')")
endif()
if(NOT licence_text MATCHES "SIL Open Font License, Version 1.1")
    message(FATAL_ERROR "the installed licence is not the SIL Open Font License, Version 1.1")
endif()
message(STATUS "the bank installs to ${shown} with its OFL 1.1 licence beside it")
