// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar cache format, whole, in one file.
//
// The daemon decodes: the image parser stays on its side of the
// trust boundary, and what reaches disk is raw RGBA at one fixed size. The
// reader in the game parses nothing -- it checks that the file is exactly the
// one size the format allows and copies bytes. No header, no dimensions to
// trust: a file of any other length is not a picture. If the format changes,
// the extension changes with it and old files stop being found, which the
// retry policy treats as "not there yet" and, after thirty seconds, as a grey
// disc.
//
// Size, path, reader and writer all live here so the format cannot drift
// across files. The daemon is 64-bit and the games are both widths,
// so the path formatting is held by tests/widths.cpp at both.

#ifndef VOCEM_AVATAR_RGBA_H
#define VOCEM_AVATAR_RGBA_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>

#include "vocem/shared_state.h"

namespace vocem {

// 64 pixels is what the download asks the CDN for: enough for a 40px panel
// avatar on a 4K display. Fixed, so the file size is the whole validation.
constexpr uint32_t kAvatarPixels = 64;
constexpr uint32_t kAvatarRgbaBytes = kAvatarPixels * kAvatarPixels * 4;

// `%llu` and not `%lu`: a Discord id needs all sixty-four bits and
// `unsigned long` has thirty-two on i386 (entry 34).
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

// The game's whole decoder (entry 51). Exactly kAvatarRgbaBytes or nothing: a short file, a
// long file and a half-written file are all refused the same way (the last
// cannot occur: the writer renames into place).
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
// size and rename it into place. Only the daemon calls this.
//
// The resample is a box filter with integer accumulation: right for scaling
// down (nothing forces the CDN to keep honouring ?size=64), nearest-neighbour
// for scaling up, which for a picture this small is enough.
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

    // Temporary-and-rename: the game must never observe a partial file, and
    // the reader's size check relies on it. The temporary is created, never
    // opened: whatever is at the name goes first, and O_CREAT|O_EXCL|O_NOFOLLOW
    // makes a fresh regular file or fails, so a planted link or FIFO is never
    // followed or waited on (entry 245).
    char temporary[832];
    std::snprintf(temporary, sizeof(temporary), "%s.part", path);
    ::unlink(temporary);
    const int fd = ::open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    size_t done = 0;
    while (done < kAvatarRgbaBytes) {
        const ssize_t wrote = ::write(fd, pixels + done, kAvatarRgbaBytes - done);
        if (wrote <= 0) {
            break;
        }
        done += static_cast<size_t>(wrote);
    }
    const bool written = ::close(fd) == 0 && done == kAvatarRgbaBytes;
    if (!written || ::rename(temporary, path) != 0) {
        ::unlink(temporary);
        return false;
    }
    return true;
}

}  // namespace vocem

#endif  // VOCEM_AVATAR_RGBA_H
