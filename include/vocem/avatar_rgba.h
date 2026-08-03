// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar cache format, whole, in one file.
//
// Avatars used to be cached as the PNG the CDN served, and decoded with
// stb_image *inside the game's process* -- a single-header parser with a history
// of memory-safety fixes, fed bytes from the internet, on the wrong side of the
// trust boundary. The daemon is the right side: it already owns the download, it
// runs as an ordinary unprivileged process of its own, and a fault there kills a
// voice overlay rather than somebody's game.
//
// So the daemon now decodes, and what reaches disk is the least interpretable
// thing there is: raw RGBA at one fixed size. The reader in the game does not
// parse anything -- it checks that the file is exactly the one size the format
// allows and copies bytes. There is no header to version and no dimensions to
// trust: a file of any other length is not a picture, end of story. If the
// format ever has to change, the extension changes with it and old files simply
// stop being found, which the retry policy already treats as "not there yet"
// and, after thirty seconds, as a grey disc -- never as garbage interpreted.
//
// Everything about the format lives in this one header -- the size, the path,
// the reader, the writer -- because the last cache format kept its knowledge in
// three files and entry 33 is what that cost. The daemon is 64-bit and the games
// are both widths, so the path formatting here is held by tests/widths.cpp at
// both, like every other string both halves must agree on.

#ifndef VOCEM_AVATAR_RGBA_H
#define VOCEM_AVATAR_RGBA_H

#include <stdio.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstring>

#include "vocem/shared_state.h"

namespace vocem {

// 64 pixels is what the download asked the CDN for all along: enough for a 40px
// panel avatar on a 4K display. Fixed, so the file size is the whole validation.
constexpr uint32_t kAvatarPixels = 64;
constexpr uint32_t kAvatarRgbaBytes = kAvatarPixels * kAvatarPixels * 4;

// The same two spellings avatar_cache_path() had, with the format's own
// extension. `%llu` and not `%lu`, for the reason entry 34 paid for: a Discord
// id needs all sixty-four bits and `unsigned long` has thirty-two on i386.
inline void avatar_rgba_path(char* out, size_t capacity, uint64_t user_id,
                             const char* avatar_hash) {
    char dir[512];
    avatar_cache_dir(dir, sizeof(dir));
    if (avatar_hash_is_sane(avatar_hash)) {
        std::snprintf(out, capacity, "%s/%llu_%s.rgba", dir,
                      static_cast<unsigned long long>(user_id), avatar_hash);
    } else {
        std::snprintf(out, capacity, "%s/default_%u.rgba", dir,
                      static_cast<unsigned>((user_id >> 22) % 6));
    }
}

// The game's whole decoder. Exactly kAvatarRgbaBytes or nothing: a short file, a
// long file and a half-written file are all refused the same way -- though the
// last cannot occur, because the writer renames into place.
inline bool avatar_rgba_load(const char* path, unsigned char* out) {
    struct stat info {};
    if (::stat(path, &info) != 0 || info.st_size != static_cast<off_t>(kAvatarRgbaBytes)) {
        return false;
    }
    FILE* file = ::fopen(path, "rb");
    if (!file) {
        return false;
    }
    const bool complete = ::fread(out, 1, kAvatarRgbaBytes, file) == kAvatarRgbaBytes;
    ::fclose(file);
    return complete;
}

// The daemon's half: normalise whatever the decode produced to the format's one
// size and rename it into place. Only the daemon calls this; it is in the same
// header so the format cannot drift apart the way the dlsym version did.
//
// The resample is a box filter with integer accumulation: every source pixel
// inside the destination pixel's footprint contributes once, which is right for
// scaling down (the CDN honours ?size=64, but nothing forces it to keep doing
// so) and degrades to nearest-neighbour for scaling up, which for a picture
// this small is not worth more code.
inline bool avatar_rgba_write(const char* path, const unsigned char* rgba, uint32_t width,
                              uint32_t height) {
    if (!rgba || width == 0 || height == 0) {
        return false;
    }

    static_assert(kAvatarRgbaBytes == kAvatarPixels * kAvatarPixels * 4, "one size, one truth");
    unsigned char scaled[kAvatarRgbaBytes];
    const unsigned char* pixels = rgba;
    if (width != kAvatarPixels || height != kAvatarPixels) {
        for (uint32_t y = 0; y < kAvatarPixels; ++y) {
            uint32_t y0 = y * height / kAvatarPixels;
            uint32_t y1 = (y + 1) * height / kAvatarPixels;
            if (y1 <= y0) {
                y1 = y0 + 1;
            }
            for (uint32_t x = 0; x < kAvatarPixels; ++x) {
                uint32_t x0 = x * width / kAvatarPixels;
                uint32_t x1 = (x + 1) * width / kAvatarPixels;
                if (x1 <= x0) {
                    x1 = x0 + 1;
                }
                uint32_t sum[4] = {0, 0, 0, 0};
                for (uint32_t sy = y0; sy < y1; ++sy) {
                    for (uint32_t sx = x0; sx < x1; ++sx) {
                        const unsigned char* p = rgba + (static_cast<size_t>(sy) * width + sx) * 4;
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                        sum[3] += p[3];
                    }
                }
                const uint32_t count = (y1 - y0) * (x1 - x0);
                unsigned char* out = scaled + (static_cast<size_t>(y) * kAvatarPixels + x) * 4;
                out[0] = static_cast<unsigned char>(sum[0] / count);
                out[1] = static_cast<unsigned char>(sum[1] / count);
                out[2] = static_cast<unsigned char>(sum[2] / count);
                out[3] = static_cast<unsigned char>(sum[3] / count);
            }
        }
        pixels = scaled;
    }

    // Temporary-and-rename, like every other file the daemon writes: the game
    // must never observe a partial file, and the size check above is only a
    // guarantee because of this.
    char temporary[832];
    std::snprintf(temporary, sizeof(temporary), "%s.part", path);
    FILE* file = ::fopen(temporary, "wb");
    if (!file) {
        return false;
    }
    const bool written = ::fwrite(pixels, 1, kAvatarRgbaBytes, file) == kAvatarRgbaBytes;
    ::fclose(file);
    if (!written || ::rename(temporary, path) != 0) {
        ::remove(temporary);
        return false;
    }
    return true;
}

}  // namespace vocem

#endif  // VOCEM_AVATAR_RGBA_H
