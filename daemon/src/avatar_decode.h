// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The trust boundary of this project, in one file: bytes that came from the
// internet turn into pixels here and nowhere else. The PNG parser used to run
// inside every game's process; the games now read raw RGBA at a fixed size
// (vocem/avatar_rgba.h) and parse nothing at all. A decoder fault here kills a
// voice overlay that systemd restarts -- not somebody's game.
//
// It is a header rather than a block inside avatars.cpp because the policy has
// to be something the tests run, and because the ceiling and the decode belong
// together: the ceiling was in avatars.cpp while the arithmetic it protects was
// in avatar_rgba.h, which is entry 33's shape (knowledge of one format spread
// across two files, only one of them corrected).
//
// The decoder's own configuration lives here too, so there is one spelling of
// it: the module list Wuffs compiles and the dimension ceiling it enforces.

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
// *decoder* setting and not a check afterwards, which is the whole point: the
// decoder refuses the picture while reading its header, so a hostile 8192x8192
// avatar costs nothing. The check that used to live after the decode ran once
// the 256 MB had already been allocated and the whole image inflated -- a
// megabyte of crafted PNG for a quarter-gigabyte of daemon memory, from anybody
// who shares a voice channel with the user (entry 47).
#define VOCEM_AVATAR_MAX_DIMENSION 1024

// The decoder is Wuffs (third_party/wuffs), not stb_image. Both decode this PNG
// correctly; the difference is what happens the day one of them is wrong about a
// crafted one. Wuffs' decoders are written in a memory-safe language whose
// bounds and overflow checks are proved at compile time and transpiled to C, it
// is maintained (Google, and Chromium's Skia uses it for image decoding), and on
// PNG it is faster than libpng. stb_image, measured off the public record, has
// had no commit since 2024-05-31 and its maintainer did not answer either the
// 2023 GitHub Security Lab disclosure or the 2026 reports -- so the risk there
// was never a known open PNG hole, it was that the next one would go unfixed in
// the one place this project parses bytes from the internet.
//
// The vendored file calls itself `0.4.0-alpha.10`: that is the label Wuffs puts
// on its release C files, and it is said here rather than smoothed over. What
// the label costs and does not cost is written in DESIGN.
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
// picture, when a test has installed it. The seam exists because the property
// worth testing is not "the bomb was refused" but "the bomb was refused without
// allocating" -- and that has to stay measurable across a change of decoder,
// which is exactly the change this file has just been through.
using AvatarDecodeAllocHook = void (*)(size_t bytes);
inline AvatarDecodeAllocHook& avatar_decode_alloc_hook() {
    static AvatarDecodeAllocHook hook = nullptr;
    return hook;
}

namespace detail {

// Straight (unpremultiplied) RGBA, because that is what avatar_rgba_write's box
// filter averages and what the games' textures hold. Wuffs' default is premul
// BGRA; taking the default would put every face in the wrong channel order and
// darken every translucent edge, which is the sort of thing a decoder swap gets
// wrong quietly -- so tests/avatar_decoders_agree.cpp compares the pixels of
// both decoders rather than trusting this line.
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
    // stride, so the rows are gathered when it did. Padding has not been seen at
    // these sizes, which is exactly why the branch is here rather than assumed
    // away.
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
//     followed. Without it the previous version read whatever the link pointed
//     at -- and a link to /dev/zero made the read loop forever, appending
//     zeroes: the download thread never returned, the avatar queue stalled for
//     good, and `stop()` joined a thread that would never finish, so the daemon
//     could not even be restarted.
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
