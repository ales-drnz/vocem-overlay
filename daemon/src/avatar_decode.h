// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The trust boundary of this project, in one file: bytes that came from the
// internet turn into pixels here and nowhere else. The games read raw RGBA at
// a fixed size (vocem/avatar_rgba.h) and parse nothing at all, so a decoder
// fault here kills a voice overlay that systemd restarts -- not a game.
//
// A header rather than a block inside avatars.cpp so the tests run the policy,
// and so the ceiling sits beside the decode it protects: knowledge of one
// format spread across two files is how one of them goes uncorrected. The
// decoder's own configuration lives here too: the module list Wuffs compiles
// and the dimension ceiling it enforces.

#ifndef VOCEM_AVATAR_DECODE_H
#define VOCEM_AVATAR_DECODE_H

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <string>

#include "vocem/avatar_rgba.h"

// The largest picture this daemon will decode, in pixels per side. It is a
// *decoder* setting and not a check afterwards: the decoder refuses the picture
// while reading its header, so a hostile 8192x8192 avatar from anybody in the
// voice channel costs no memory (entry 47).
#define VOCEM_AVATAR_MAX_DIMENSION 1024

// The decoder is Wuffs (third_party/wuffs), not stb_image: its bounds and
// overflow checks are proved at compile time, it is maintained and used by
// Chromium, and this is the one place the project parses bytes from the
// internet. The vendored file calls itself `0.4.0-alpha.10`, the
// label Wuffs puts on its release C files.
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__AUX__BASE
#define WUFFS_CONFIG__MODULE__AUX__IMAGE
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#include "wuffs/wuffs-v0.4.c"

namespace vocem {

constexpr uint32_t kAvatarMaxDecodedDimension = VOCEM_AVATAR_MAX_DIMENSION;

// A cached PNG this daemon wrote before the format changed is at most this
// large; nothing else in the cache directory is worth reading.
constexpr size_t kAvatarLocalPngLimit = 1024u * 1024u;

// Called with the byte count every time the decoder is about to allocate a
// picture, when a test has installed it. The property worth testing is not
// "the bomb was refused" but "the bomb was refused without allocating", and
// this seam keeps it measurable whatever the decoder.
using AvatarDecodeAllocHook = void (*)(size_t bytes);
inline AvatarDecodeAllocHook& avatar_decode_alloc_hook() {
    static AvatarDecodeAllocHook hook = nullptr;
    return hook;
}

namespace detail {

// Straight (unpremultiplied) RGBA, because that is what avatar_rgba_write's box
// filter averages and what the games' textures hold. Wuffs' default is premul
// BGRA, which would swap channels and darken translucent edges quietly;
// tests/avatar_decoders_agree.cpp compares the pixels rather than trusting
// this line.
class AvatarDecodeCallbacks : public wuffs_aux::DecodeImageCallbacks {
public:
    wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config&) override {
        return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
    }

    AllocPixbufResult AllocPixbuf(const wuffs_base__image_config& image_config,
                                  bool allow_uninitialized_memory) override {
        if (AvatarDecodeAllocHook hook = avatar_decode_alloc_hook()) {
            const uint64_t w = image_config.pixcfg.width();
            const uint64_t h = image_config.pixcfg.height();
            hook(static_cast<size_t>(w * h * 4));
        }
        return wuffs_aux::DecodeImageCallbacks::AllocPixbuf(image_config,
                                                            allow_uninitialized_memory);
    }
};

}  // namespace detail

// Decode a PNG and normalise it into the cache format. The bytes may come from
// the CDN or from a .png this cache wrote before the format changed; either way
// nothing downstream ever sees them.
inline bool avatar_decode_and_store(const unsigned char* png, size_t bytes,
                                    const char* destination) {
    if (!png || bytes == 0) {
        return false;
    }
    detail::AvatarDecodeCallbacks callbacks;
    wuffs_aux::sync_io::MemoryInput input(png, bytes);
    // The ceiling is the library's own argument: a picture past it fails as
    // MaxInclDimensionExceeded out of the image config, before any pixel memory
    // is asked for. Nothing else is opted into -- no metadata, no animation, one
    // frame, which is all an avatar is.
    wuffs_aux::DecodeImageResult result = wuffs_aux::DecodeImage(
        callbacks, input, wuffs_aux::DecodeImageArgQuirks::DefaultValue(),
        wuffs_aux::DecodeImageArgFlags::DefaultValue(),
        wuffs_aux::DecodeImageArgPixelBlend::DefaultValue(),
        wuffs_aux::DecodeImageArgBackgroundColor(0xFF000000),
        wuffs_aux::DecodeImageArgMaxInclDimension(kAvatarMaxDecodedDimension));
    if (!result.error_message.empty()) {
        return false;
    }

    const uint32_t width = result.pixbuf.pixcfg.width();
    const uint32_t height = result.pixbuf.pixcfg.height();
    if (width == 0 || height == 0 || width > kAvatarMaxDecodedDimension ||
        height > kAvatarMaxDecodedDimension) {
        return false;
    }
    const wuffs_base__table_u8 plane = result.pixbuf.plane(0);
    if (!plane.ptr || plane.width < static_cast<size_t>(width) * 4 ||
        plane.height < height) {
        return false;
    }
    // avatar_rgba_write wants tightly packed rows; a decoder is free to pad its
    // stride, so the rows are gathered when it did.
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    if (plane.stride == row_bytes) {
        return avatar_rgba_write(destination, plane.ptr, width, height);
    }
    std::string packed;
    try {
        packed.resize(row_bytes * height);
    } catch (...) {
        return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(&packed[y * row_bytes], plane.ptr + static_cast<size_t>(y) * plane.stride,
                    row_bytes);
    }
    return avatar_rgba_write(destination,
                             reinterpret_cast<const unsigned char*>(packed.data()), width,
                             height);
}

// Reads a .png this cache wrote in an older format, for migration. Everything
// about this read is defensive, because the cache directory is an ordinary
// directory in the user's home that any process running as the user can write:
//
//   * `O_NOFOLLOW`, so a symlink planted at the name is refused rather than
//     followed -- a link to /dev/zero would otherwise read forever and leave
//     the download thread, and `stop()` joining it, stuck for good.
//   * a regular-file check, for the same reason at one remove (a device
//     streams), with `O_NONBLOCK` on the open so that a *fifo* planted at the
//     name cannot block inside `open` itself, before the check can refuse it.
//     On a regular file the flag changes nothing.
//   * a size limit, so the file is bounded before it is in memory.
//
// Returns false and leaves `out` empty on any of those.
inline bool avatar_read_local_png(const char* path, std::string& out) {
    out.clear();
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<size_t>(info.st_size) > kAvatarLocalPngLimit) {
        ::close(fd);
        return false;
    }
    const size_t bytes = static_cast<size_t>(info.st_size);
    bool complete = false;
    try {
        out.resize(bytes);
        size_t done = 0;
        while (done < bytes) {
            const ssize_t got = ::read(fd, &out[done], bytes - done);
            if (got <= 0) {
                break;
            }
            done += static_cast<size_t>(got);
        }
        complete = done == bytes;
    } catch (...) {
        complete = false;
    }
    ::close(fd);
    if (!complete) {
        out.clear();
    }
    return complete;
}

}  // namespace vocem

#endif  // VOCEM_AVATAR_DECODE_H
