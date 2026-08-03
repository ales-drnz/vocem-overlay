# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# The emoji bank is a Modified Version of Noto Color Emoji under the OFL 1.1,
# and a Modified Version travels with its licence. The bank shipped through
# 0.1.0-52's sources with no licence text anywhere -- found by the image-system
# review, not by a complaint, which is exactly why it gets a test: an absent
# file is invisible until somebody goes looking.
#
# Held here: the licence text sits beside the bank, carries the original
# copyright line and the OFL name, and the install rule ships the two together.

set(emoji_dir "${SOURCE_DIR}/common/emoji")

if(NOT EXISTS "${emoji_dir}/emoji_bank.rgba")
    message(FATAL_ERROR "emoji bank missing: ${emoji_dir}/emoji_bank.rgba")
endif()

set(licence "${emoji_dir}/OFL-NotoColorEmoji.txt")
if(NOT EXISTS "${licence}")
    message(FATAL_ERROR
        "the emoji bank ships without its licence: ${licence} does not exist")
endif()

file(READ "${licence}" licence_text)
if(NOT licence_text MATCHES "Copyright 2013 Google LLC")
    message(FATAL_ERROR
        "${licence} does not carry the original copyright notice "
        "(expected 'Copyright 2013 Google LLC')")
endif()
if(NOT licence_text MATCHES "SIL Open Font License, Version 1.1")
    message(FATAL_ERROR
        "${licence} is not the SIL Open Font License, Version 1.1")
endif()

# The install rule must ship the licence in the same FILES list as the bank:
# a licence in the repo that the package leaves behind is the defect again,
# one directory over.
file(READ "${SOURCE_DIR}/CMakeLists.txt" top)
if(NOT top MATCHES "emoji_bank\\.rgba[^)]*OFL-NotoColorEmoji\\.txt")
    message(FATAL_ERROR
        "CMakeLists.txt installs emoji_bank.rgba without "
        "OFL-NotoColorEmoji.txt beside it")
endif()
