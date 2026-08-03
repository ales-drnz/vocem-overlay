// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar cache format, held from both ends.
//
// The daemon writes it, both widths of the injected code read it, and the whole
// format is "exactly this many bytes": there is no header, so the strictness of
// the reader *is* the file format. What this holds:
//
//   * a 64x64 image survives the roundtrip byte for byte;
//   * a larger decode is resampled down and a smaller one up, because nothing
//     forces the CDN to keep honouring ?size=64 -- the box filter's output is
//     checked where it is exact (solid quadrants), not eyeballed;
//   * everything that is not exactly the one size -- truncated, padded, absent
//     -- is refused by the reader, which is what stands between a game process
//     and interpreting garbage as pixels now that no parser is in there.
//
// The writer goes through .part-and-rename like the daemon's other files; the
// test watches for the temporary to be gone afterwards, because a leftover
// .part would be an ordinary-looking file one directory listing away from being
// mistaken for cache.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vocem/avatar_rgba.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// A solid colour per quadrant, at any square size: box-filtering it at any
// ratio must reproduce the quadrant colours exactly, which makes the resample
// checkable to the byte instead of "looks right".
void quadrants(unsigned char* rgba, unsigned size) {
    const unsigned char colours[4][4] = {
        {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 128}};
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            const unsigned quadrant = (y >= size / 2 ? 2 : 0) + (x >= size / 2 ? 1 : 0);
            memcpy(rgba + (static_cast<size_t>(y) * size + x) * 4, colours[quadrant], 4);
        }
    }
}

const unsigned char* at(const unsigned char* rgba, unsigned x, unsigned y) {
    return rgba + (static_cast<size_t>(y) * vocem::kAvatarPixels + x) * 4;
}

bool pixel_is(const unsigned char* p, unsigned char r, unsigned char g, unsigned char b,
              unsigned char a) {
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

bool file_gone(const char* path) {
    return access(path, F_OK) != 0;
}

}  // namespace

int main() {
    char dir[] = "/tmp/vocem-avatar-rgba-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/probe.rgba", dir);
    char part[640];
    snprintf(part, sizeof(part), "%s.part", path);

    static unsigned char image[256 * 256 * 4];
    static unsigned char loaded[vocem::kAvatarRgbaBytes];

    // 1. Identity: what goes in at the native size comes back byte for byte.
    quadrants(image, vocem::kAvatarPixels);
    check(vocem::avatar_rgba_write(path, image, vocem::kAvatarPixels, vocem::kAvatarPixels),
          "a 64x64 image writes");
    check(file_gone(part), "and leaves no .part behind");
    check(vocem::avatar_rgba_load(path, loaded), "and reads back");
    check(memcmp(image, loaded, vocem::kAvatarRgbaBytes) == 0, "byte for byte");

    // 2. Down: a 256x256 decode -- the CDN deciding not to honour ?size= -- is
    // resampled, and solid quadrants come through exact.
    quadrants(image, 256);
    check(vocem::avatar_rgba_write(path, image, 256, 256), "a 256x256 image writes");
    check(vocem::avatar_rgba_load(path, loaded), "and reads back at the one size");
    check(pixel_is(at(loaded, 16, 16), 255, 0, 0, 255), "top-left quadrant survives the box filter");
    check(pixel_is(at(loaded, 48, 16), 0, 255, 0, 255), "top-right too");
    check(pixel_is(at(loaded, 16, 48), 0, 0, 255, 255), "bottom-left too");
    check(pixel_is(at(loaded, 48, 48), 255, 255, 0, 128),
          "and the alpha channel is carried, not assumed opaque");

    // 3. Up: smaller than the format -- unlikely, but the CDN's answer is not
    // ours to assume -- still produces the one size.
    quadrants(image, 32);
    check(vocem::avatar_rgba_write(path, image, 32, 32), "a 32x32 image writes");
    check(vocem::avatar_rgba_load(path, loaded), "and reads back at the one size");
    check(pixel_is(at(loaded, 16, 16), 255, 0, 0, 255), "scaled up, the quadrants still hold");

    // 4. The reader's strictness is the format. A byte short, a byte long, or
    // not there at all: all three are "no picture", never "interpret it anyway".
    quadrants(image, vocem::kAvatarPixels);
    FILE* file = fopen(path, "wb");
    fwrite(image, 1, vocem::kAvatarRgbaBytes - 1, file);
    fclose(file);
    check(!vocem::avatar_rgba_load(path, loaded), "one byte short is refused");

    file = fopen(path, "wb");
    fwrite(image, 1, vocem::kAvatarRgbaBytes, file);
    fputc(0, file);
    fclose(file);
    check(!vocem::avatar_rgba_load(path, loaded), "one byte long is refused");

    unlink(path);
    check(!vocem::avatar_rgba_load(path, loaded), "absent is refused");

    // 5. A writer handed nonsense refuses rather than writing nonsense.
    check(!vocem::avatar_rgba_write(path, nullptr, 64, 64), "a null image does not write");
    check(!vocem::avatar_rgba_write(path, image, 0, 64), "nor a zero width");
    check(file_gone(path), "and nothing appeared on disk for either");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
