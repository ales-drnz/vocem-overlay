# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Neither PKGBUILD writes a package over a defective exemplar, by either door.
#
# _refuse_to_replace_an_exemplar was called from build() alone. `makepkg -R`
# (repackage) skips build() and runs package(), then writes the package file
# -- the one EXEMPLARS.md says cannot be rebuilt -- so the guard did not stand
# on that door.
#
# Each recipe is sourced into bash with a scratch startdir: an EXEMPLARS.md
# that lists the recipe's own pkgver-pkgrel, an existing package of that name
# beside it, and cmake/install stubbed to log. package() must refuse (non-zero)
# before running anything; the same with build(), which already did. Nothing
# touches packaging/: startdir, srcdir and pkgdir are scratch directories.

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
    message(FATAL_ERROR
        "CMAKE_CURRENT_BINARY_DIR is '${CMAKE_CURRENT_BINARY_DIR}', which is not a "
        "test directory: pass -DCMAKE_CURRENT_BINARY_DIR=<build>/tests, or run this "
        "through ctest, so the scratch files land in the build tree")
endif()
find_program(BASH bash)
if(NOT BASH)
    message(STATUS "skip bash is missing")
    return()
endif()

set(scratch "${CMAKE_CURRENT_BINARY_DIR}/package_refuses_exemplar")
set(problems "")
foreach(recipe PKGBUILD PKGBUILD.local)
    foreach(door build package)
        file(REMOVE_RECURSE "${scratch}")
        file(MAKE_DIRECTORY "${scratch}/start" "${scratch}/src/vocem-overlay" "${scratch}/pkg")
        file(WRITE "${scratch}/drive.sh" [=[
set -u
startdir="$1/start"; srcdir="$1/src"; pkgdir="$1/pkg"; log="$1/log"
source "$2"
PKGDEST="$startdir"
error() { printf 'ERROR '; printf "$@"; printf '\n'; }
cmake() { echo "cmake $*" >> "$log"; }
install() { echo "install $*" >> "$log"; }
printf '| %s-%s | a defective exemplar |\n' "$pkgver" "$pkgrel" > "$startdir/EXEMPLARS.md"
: > "$startdir/$pkgname-$pkgver-$pkgrel-x86_64.pkg.tar.zst"
: > "$log"
"$3"
status=$?
echo "status $status"
echo "ran $(wc -l < "$log")"
]=])
        execute_process(COMMAND "${BASH}" "${scratch}/drive.sh" "${scratch}"
                                "${SOURCE_DIR}/packaging/${recipe}" "${door}"
            OUTPUT_VARIABLE out ERROR_VARIABLE err)
        string(STRIP "${out}" out)
        message("${recipe} ${door}(): ${out}")
        if(NOT out MATCHES "status [1-9]" OR NOT out MATCHES "ran 0")
            list(APPEND problems "${recipe}: ${door}() did not refuse before running anything (${out})")
        endif()
        if(NOT out MATCHES "ERROR .*defective exemplar")
            list(APPEND problems "${recipe}: ${door}() did not say why")
        endif()
    endforeach()
endforeach()
file(REMOVE_RECURSE "${scratch}")

if(problems)
    foreach(problem IN LISTS problems)
        message("FAIL ${problem}")
    endforeach()
    message(FATAL_ERROR "a package listed in EXEMPLARS.md can be written over")
endif()
message(STATUS "ok build() and package() of both recipes refuse to replace an exemplar")
