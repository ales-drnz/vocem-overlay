# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Every package version a test names is in packaging/EXEMPLARS.md.
#
# The historical packages are not in git and cannot be rebuilt (the file says
# which), and the tests name them as the defective binary a fix was refuted
# against. EXEMPLARS.md is the list a clean-up checks before deleting anything,
# and it is regenerated from a grep -- which nobody re-ran: 0.1.10-5, the
# exemplar of the freeze (DESIGN 191/192) named by three tests, was not in it,
# nor were 0.1.10-3 and -6 that DESIGN names (DESIGN 205). DESIGN.md is not in
# git, so only the tests' half of the grep is held here.
#
# Expects SOURCE_DIR.

file(READ "${SOURCE_DIR}/packaging/EXEMPLARS.md" table)
set(listed "")
string(REGEX MATCHALL "\n\\| [0-9][^|]*\\|" first_cells "${table}")
foreach(cell IN LISTS first_cells)
    # "| 0.1.0-41, -42 |" lists two packages of one version.
    if(cell MATCHES "([0-9]+\\.[0-9]+\\.[0-9]+)-([0-9]+)")
        set(version "${CMAKE_MATCH_1}")
        string(REGEX MATCHALL "-[0-9]+" releases "${cell}")
        foreach(release IN LISTS releases)
            list(APPEND listed "${version}${release}")
        endforeach()
    endif()
endforeach()
list(LENGTH listed listed_count)
if(listed_count LESS 15)
    message(FATAL_ERROR "read ${listed_count} packages out of EXEMPLARS.md's tables: the parse "
                        "is broken, not the table")
endif()

file(GLOB_RECURSE sources "${SOURCE_DIR}/tests/*")
set(missing "")
set(cited_count 0)
foreach(source IN LISTS sources)
    file(READ "${source}" text)
    string(REGEX MATCHALL "0\\.1\\.[0-9]+-[0-9]+" cited "${text}")
    foreach(package IN LISTS cited)
        math(EXPR cited_count "${cited_count} + 1")
        list(FIND listed "${package}" at)
        if(at EQUAL -1)
            file(RELATIVE_PATH where "${SOURCE_DIR}" "${source}")
            list(APPEND missing "${package} (${where})")
        endif()
    endforeach()
endforeach()
if(cited_count LESS 10)
    message(FATAL_ERROR "found ${cited_count} package citations in tests/: the walk is broken")
endif()
list(REMOVE_DUPLICATES missing)
if(missing)
    string(REPLACE ";" ", " missing "${missing}")
    message(FATAL_ERROR "tests name packages packaging/EXEMPLARS.md does not list, so a clean-up "
                        "may delete the only defective binary they were refuted against: ${missing}")
endif()
message(STATUS "ok ${cited_count} citations in tests/, every package among the ${listed_count} "
               "EXEMPLARS.md lists")
