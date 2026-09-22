# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Runs tests/rpc_probe_frames.py against scripts/rpc-probe.py (DESIGN 206).
# Expects SOURCE_DIR.

find_program(PYTHON3 python3)
if(NOT PYTHON3)
    message(STATUS "skip python3 is not installed, and rpc-probe.py is a Python script")
    return()
endif()
execute_process(
    COMMAND "${PYTHON3}" "${SOURCE_DIR}/tests/rpc_probe_frames.py"
            "${SOURCE_DIR}/scripts/rpc-probe.py"
    RESULT_VARIABLE status TIMEOUT 60)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "rpc-probe.py's client lost the stream: ${status}")
endif()
