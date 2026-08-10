# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# There is one place that turns a box's coordinate back into a position fraction,
# and dividing by the view's size is not it.
#
# This test exists because the wrong line was written three times. The panel's
# placement became "a fraction is a place between the margins"
# (vocem/placement.h), whose inverse is `(position - inset) / (extent - box -
# 2*inset)`. The window kept the inverse of the *old* forward map --
# `panel.x / stage.width` -- in the drag handler, which made a live drag fight the
# placement and flash; that was fixed, and the identical line in the **release**
# handler was not, so letting go of the panel wrote a fraction nobody had asked
# for and it jumped somewhere unrelated to the pointer. Two shipped packages, one
# line, three copies.
#
# So the rule is about the duplication rather than about the value, which is
# entry 33's lesson: a test that checks the arithmetic is a test of the copy it
# happens to look at, and what actually goes wrong is the second copy. Nothing
# under gui/qml may divide a coordinate by a view's extent; `fractionFrom` --
# mirroring `fraction_within` -- is the only way back.

file(GLOB_RECURSE qml_files "${SOURCE_DIR}/gui/qml/*.qml")
# A purely negative test over a glob is a test that passes on an empty set: move
# the directory and this would go on approving nothing. The floor is well under
# the real count (34 files today) and exists only to catch that.
list(LENGTH qml_files qml_count)
if(qml_count LESS 10)
    message(FATAL_ERROR "one_placement_inverse: only ${qml_count} QML files found under "
                        "${SOURCE_DIR}/gui/qml -- the walk has lost the pages, which is "
                        "not agreement")
endif()

set(offences "")
foreach(file IN LISTS qml_files)
    file(READ "${file}" text)
    # `something.x / stage.width` and every spelling of it that has been written or
    # is a plausible next attempt. The pattern is deliberately about the shape
    # (a coordinate over an extent) rather than about one variable's name.
    if(text MATCHES "\\.[xy] */ *(stage|root|map|view)\\.(width|height)")
        list(APPEND offences "${file}")
    endif()
endforeach()

if(offences)
    message(FATAL_ERROR
        "a coordinate is divided by a view's extent, which is the inverse of the "
        "placement arithmetic that was replaced -- use fractionFrom(), the one "
        "spelling (tests/one_placement_inverse.cmake says what this cost):\n"
        "  ${offences}")
endif()

# And the forward direction: the map must place the panel through the shared
# function rather than by multiplying a fraction by the whole view, which is the
# same mistake facing the other way.
foreach(file IN LISTS qml_files)
    file(READ "${file}" text)
    if(text MATCHES "config\\.position[XY] *\\* *(stage|root)\\.(width|height)")
        message(FATAL_ERROR
            "a position fraction is multiplied by a view's extent in ${file}: that "
            "is the placement this project replaced, and it puts a middle anchor's "
            "box below the middle. Use placeWithin().")
    endif()
endforeach()

message(STATUS "ok one_placement_inverse: ${qml_count} QML files, none divides by a view's extent")
