// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The daemon's side of the avatar chain, from the outside: the only place in
// this project where bytes from the internet meet a parser.
//
// Two defects are held here, both found by a hostile review of the chain rather
// than by a crash, and both about *cost* rather than about a wrong answer --
// which is why a test that only compares booleans could not have caught either:
//
//   * A decompression bomb. `stbi_load_from_memory` was called with stb's
//     default 16-million-pixel ceiling and the plausibility check ran
//     afterwards, so a 260 KB PNG declaring 8192x8192 was inflated in full --
//     a quarter of a gigabyte of daemon memory -- and only then refused. The
//     measurement is the instrument: stb's allocator is counted here, so the
//     assertion is "refused without allocating", not "refused". Against the
//     module as it shipped through 0.1.0-52 the count for one 2048x2048 PNG is
//     tens of megabytes; with the ceiling handed to the decoder it is zero. The
//     claim outlived the decoder it was written against: the same assertion holds
//     for Wuffs, whose own max-dimension argument refuses the picture out of the
//     image config.
//
//   * The migration read. A .png left by an older format was read with a plain
//     fopen loop and no cap, from a directory any process running as the user
//     can write, and it deleted the file afterwards whatever happened. A
//     symlink to /dev/zero therefore made the read never end: the download
//     thread stopped servicing the queue for good and `stop()` joined a thread
//     that could not finish. The cases are walked here with an alarm, so the
//     old behaviour fails as a timeout rather than hanging ctest.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "avatar_decode.h"

#include "vocem/avatar_rgba.h"

// What the decoder asks for, through the header's own hook. This is the whole
// difference between "the daemon refused a bomb" and "the daemon refused a bomb
// after paying for it" -- and it is a hook rather than an allocator override so
// that the measurement survives a change of decoder, which this file has already
// outlived once (stb_image to Wuffs).
namespace {
size_t g_requested = 0;
void count_allocation(size_t bytes) { g_requested += bytes; }
}  // namespace

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%-58s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition) {
        ++g_failures;
    }
}

void put32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void put_chunk(std::string& out, const char* type, const std::string& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.append(type, 4);
    out.append(data);
    put32(out, static_cast<uint32_t>(
                   ::crc32(0, reinterpret_cast<const Bytef*>(out.data() + start),
                           static_cast<uInt>(4 + data.size()))));
}

// A real, valid PNG: the bomb has to be one, or the decoder refuses it for a
// reason that has nothing to do with its size.
std::string make_png(uint32_t width, uint32_t height, bool flat = false) {
    std::string raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        for (uint32_t x = 0; x < width; ++x) {
            raw.push_back(static_cast<char>(flat ? 0x20 : (x & 0xff)));
            raw.push_back(static_cast<char>(flat ? 0x20 : (y & 0xff)));
            raw.push_back(static_cast<char>(0x40));
            raw.push_back(static_cast<char>(0xff));
        }
    }
    uLongf packed_size = ::compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> packed(packed_size);
    if (::compress2(packed.data(), &packed_size, reinterpret_cast<const Bytef*>(raw.data()),
                    static_cast<uLong>(raw.size()), 1) != Z_OK) {
        return {};
    }

    std::string header;
    put32(header, width);
    put32(header, height);
    header.push_back(8);  // bit depth
    header.push_back(6);  // colour type: RGBA
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);

    std::string png("\x89PNG\r\n\x1a\n", 8);
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", std::string(reinterpret_cast<const char*>(packed.data()), packed_size));
    put_chunk(png, "IEND", std::string());
    return png;
}

std::string scratch_dir() {
    char pattern[] = "/tmp/vocem_avatar_decode_XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    return dir ? std::string(dir) : std::string();
}

bool decode(const std::string& png, const std::string& destination) {
    return vocem::avatar_decode_and_store(reinterpret_cast<const unsigned char*>(png.data()),
                                          png.size(), destination.c_str());
}

long file_size(const std::string& path) {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 ? static_cast<long>(info.st_size) : -1;
}

void write_file(const std::string& path, const std::string& bytes) {
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

}  // namespace

int main() {
    const std::string dir = scratch_dir();
    if (dir.empty()) {
        std::printf("no scratch directory\n");
        return 77;
    }
    // The old read loop never returns on a streaming file; an alarm turns that
    // into a failure ctest can report instead of a run that never ends.
    ::alarm(60);

    vocem::avatar_decode_alloc_hook() = count_allocation;

    const std::string out = dir + "/face.rgba";

    // An ordinary avatar: decoded and stored at exactly the one size the format
    // has, which is what a game checks for.
    check(decode(make_png(64, 64), out), "64x64 PNG stored");
    check(file_size(out) == static_cast<long>(vocem::kAvatarRgbaBytes),
          "stored file is exactly the format's size");

    // The ceiling is the decoder's, and it is inclusive: the largest picture
    // the daemon will look at still works.
    ::unlink(out.c_str());
    check(decode(make_png(vocem::kAvatarMaxDecodedDimension, vocem::kAvatarMaxDecodedDimension),
                 out),
          "1024x1024 PNG (the ceiling itself) stored");

    // A picture past the ceiling: refused, and refused *before* the decoder
    // allocates its image. This is the bomb, scaled down to something a test
    // can build quickly -- the mechanism does not care how far past the
    // ceiling the declaration is.
    // One colour, which is what a bomb is made of: the wire cost collapses and
    // the memory cost does not.
    const std::string bomb = make_png(2048, 2048, true);
    // The asymmetry is the attack: what arrives is small, what it would cost is
    // not. (A hostile picture compresses far better than this test's gradient --
    // 8192x8192 of one colour is 260 KB for 256 MB.)
    const size_t bomb_pixels_bytes = 2048u * 2048u * 4u;
    check(!bomb.empty() && bomb.size() * 8 < bomb_pixels_bytes,
          "the 2048x2048 PNG is a fraction of the memory it asks for");
    std::printf("    %zu bytes on the wire for %zu bytes of pixels\n", bomb.size(),
                bomb_pixels_bytes);
    ::unlink(out.c_str());
    g_requested = 0;
    const bool refused = !decode(bomb, out);
    const size_t asked = g_requested;
    check(refused, "2048x2048 PNG refused");
    check(asked < 64u * 1024u, "refused without allocating the image");
    std::printf("    the decoder asked for %zu bytes on the refusal\n", asked);
    check(file_size(out) < 0, "nothing written for a refused picture");

    // Non-square is stretched rather than refused, deliberately: the format has
    // one size and the resample is what makes that true.
    check(decode(make_png(128, 32), out), "128x32 PNG stored");
    check(file_size(out) == static_cast<long>(vocem::kAvatarRgbaBytes),
          "a non-square source still lands at the format's size");

    // Rubbish of every shape, all refused without a crash.
    check(!decode(std::string(), out), "empty input refused");
    check(!decode(std::string(4096, '\x01'), out), "random bytes refused");
    const std::string good = make_png(64, 64);
    check(!decode(good.substr(0, good.size() / 2), out), "truncated PNG refused");
    check(!decode(good.substr(0, 30), out), "header-only PNG refused");

    // --- the migration read ---------------------------------------------
    const std::string legacy = dir + "/legacy.png";
    write_file(legacy, good);
    std::string read_back;
    check(vocem::avatar_read_local_png(legacy.c_str(), read_back) && read_back == good,
          "a real cached PNG is read whole");

    // A symlink is refused rather than followed. Pointed at /dev/zero, the old
    // loop appended zeroes until memory ran out and never returned.
    const std::string link = dir + "/zero.png";
    ::symlink("/dev/zero", link.c_str());
    read_back = "x";
    check(!vocem::avatar_read_local_png(link.c_str(), read_back) && read_back.empty(),
          "a symlink to /dev/zero is refused, not read");

    // A fifo would block inside open() without O_NONBLOCK, which is a hang in
    // the same thread by another route.
    const std::string fifo = dir + "/fifo.png";
    ::mkfifo(fifo.c_str(), 0600);
    check(!vocem::avatar_read_local_png(fifo.c_str(), read_back), "a fifo is refused, not opened");

    // Bounded before it is in memory.
    const std::string big = dir + "/big.png";
    write_file(big, std::string(vocem::kAvatarLocalPngLimit + 1, '\0'));
    check(!vocem::avatar_read_local_png(big.c_str(), read_back), "an oversized file is refused");

    const std::string absent = dir + "/nothing.png";
    check(!vocem::avatar_read_local_png(absent.c_str(), read_back), "an absent file is refused");

    ::unlink(out.c_str());
    ::unlink(legacy.c_str());
    ::unlink(link.c_str());
    ::unlink(fifo.c_str());
    ::unlink(big.c_str());
    ::rmdir(dir.c_str());

    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
