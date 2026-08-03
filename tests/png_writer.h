// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A PNG encoder for tests, small enough to read in one sitting.
//
// The tests that feed the daemon's decoder need PNGs of shapes a hostile or
// merely unusual CDN can produce -- a bomb's dimensions, sixteen bits a channel,
// a palette, no alpha at all -- and building them in the test is better than
// carrying binary fixtures: what the bytes are is visible in the source, and a
// bomb is generated rather than committed.
//
// One copy, in a header, because the first two callers wrote the same forty lines
// and this project has an entry about that (33).
//
// What it does NOT emit, said plainly so no test claims coverage it lacks:
// interlaced (Adam7) images, ancillary chunks beyond PLTE, and anything with a
// filter other than "none" on every row.

#ifndef VOCEM_TESTS_PNG_WRITER_H
#define VOCEM_TESTS_PNG_WRITER_H

#include <zlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vocem_test {

enum class PngKind {
    kRgba8,     // colour type 6, depth 8 -- what Discord serves
    kRgb8,      // colour type 2: no alpha channel at all
    kGrey8,     // colour type 0
    kGreyA8,    // colour type 4
    kRgba16,    // colour type 6, depth 16
    kPalette8,  // colour type 3 with a PLTE of four entries
};

namespace detail {

inline void put32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

inline void put_chunk(std::string& out, const char* type, const std::string& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.append(type, 4);
    out.append(data);
    put32(out, static_cast<uint32_t>(::crc32(0, reinterpret_cast<const Bytef*>(out.data() + start),
                                            static_cast<uInt>(4 + data.size()))));
}

// The pixel a given position carries, in the eight-bit RGBA the tests reason in.
// `flat` is what a decompression bomb is made of: one colour, so the wire cost
// collapses and the memory cost does not.
inline void sample(uint32_t x, uint32_t y, bool flat, unsigned char out[4]) {
    out[0] = static_cast<unsigned char>(flat ? 0x20 : (x * 7) & 0xff);
    out[1] = static_cast<unsigned char>(flat ? 0x20 : (y * 5) & 0xff);
    out[2] = static_cast<unsigned char>(flat ? 0x40 : ((x + y) * 3) & 0xff);
    out[3] = static_cast<unsigned char>(flat ? 0xff : (0x40 + ((x ^ y) & 0x7f)));
}

}  // namespace detail

// A valid PNG of the requested shape, or an empty string if zlib refused.
inline std::string make_png(uint32_t width, uint32_t height, PngKind kind = PngKind::kRgba8,
                            bool flat = false) {
    static const unsigned char kPalette[4][3] = {
        {0x20, 0x20, 0x40}, {0xd0, 0x40, 0x30}, {0x30, 0xa0, 0x50}, {0xf0, 0xf0, 0xf0}};

    std::string raw;
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        for (uint32_t x = 0; x < width; ++x) {
            unsigned char p[4];
            detail::sample(x, y, flat, p);
            switch (kind) {
                case PngKind::kRgba8:
                    raw.append(reinterpret_cast<const char*>(p), 4);
                    break;
                case PngKind::kRgb8:
                    raw.append(reinterpret_cast<const char*>(p), 3);
                    break;
                case PngKind::kGrey8:
                    raw.push_back(static_cast<char>(p[0]));
                    break;
                case PngKind::kGreyA8:
                    raw.push_back(static_cast<char>(p[0]));
                    raw.push_back(static_cast<char>(p[3]));
                    break;
                case PngKind::kRgba16:
                    // Big-endian sixteen-bit samples whose high byte is the
                    // eight-bit value, so a decoder that reduces by taking the
                    // high byte lands exactly on the kRgba8 image.
                    for (int c = 0; c < 4; ++c) {
                        raw.push_back(static_cast<char>(p[c]));
                        raw.push_back(static_cast<char>(p[c]));
                    }
                    break;
                case PngKind::kPalette8:
                    raw.push_back(static_cast<char>((x + y) & 0x03));
                    break;
            }
        }
    }

    uLongf packed_size = ::compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> packed(packed_size);
    if (::compress2(packed.data(), &packed_size, reinterpret_cast<const Bytef*>(raw.data()),
                    static_cast<uLong>(raw.size()), 1) != Z_OK) {
        return {};
    }

    unsigned char depth = 8;
    unsigned char colour = 6;
    switch (kind) {
        case PngKind::kRgba8: break;
        case PngKind::kRgb8: colour = 2; break;
        case PngKind::kGrey8: colour = 0; break;
        case PngKind::kGreyA8: colour = 4; break;
        case PngKind::kRgba16: depth = 16; break;
        case PngKind::kPalette8: colour = 3; break;
    }

    std::string header;
    detail::put32(header, width);
    detail::put32(header, height);
    header.push_back(static_cast<char>(depth));
    header.push_back(static_cast<char>(colour));
    header.push_back(0);  // compression: deflate
    header.push_back(0);  // filter method 0
    header.push_back(0);  // no interlacing

    std::string png("\x89PNG\r\n\x1a\n", 8);
    detail::put_chunk(png, "IHDR", header);
    if (kind == PngKind::kPalette8) {
        std::string plte;
        for (const auto& entry : kPalette) {
            plte.append(reinterpret_cast<const char*>(entry), 3);
        }
        detail::put_chunk(png, "PLTE", plte);
    }
    detail::put_chunk(png, "IDAT",
                      std::string(reinterpret_cast<const char*>(packed.data()), packed_size));
    detail::put_chunk(png, "IEND", std::string());
    return png;
}

// What the image above should decode to, as straight eight-bit RGBA: the answer
// computed from the same samples the encoder used, so a test can hold a decoder
// to the picture rather than to another decoder.
inline std::vector<unsigned char> expected_rgba(uint32_t width, uint32_t height, PngKind kind,
                                                bool flat = false) {
    static const unsigned char kPalette[4][3] = {
        {0x20, 0x20, 0x40}, {0xd0, 0x40, 0x30}, {0x30, 0xa0, 0x50}, {0xf0, 0xf0, 0xf0}};
    std::vector<unsigned char> out(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            unsigned char p[4];
            detail::sample(x, y, flat, p);
            unsigned char* q = out.data() + (static_cast<size_t>(y) * width + x) * 4;
            switch (kind) {
                case PngKind::kRgba8:
                case PngKind::kRgba16:
                    q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
                    break;
                case PngKind::kRgb8:
                    q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 0xff;
                    break;
                case PngKind::kGrey8:
                    q[0] = q[1] = q[2] = p[0]; q[3] = 0xff;
                    break;
                case PngKind::kGreyA8:
                    q[0] = q[1] = q[2] = p[0]; q[3] = p[3];
                    break;
                case PngKind::kPalette8: {
                    const unsigned char index = static_cast<unsigned char>((x + y) & 0x03);
                    q[0] = kPalette[index][0];
                    q[1] = kPalette[index][1];
                    q[2] = kPalette[index][2];
                    q[3] = 0xff;
                    break;
                }
            }
        }
    }
    return out;
}

}  // namespace vocem_test

#endif  // VOCEM_TESTS_PNG_WRITER_H
