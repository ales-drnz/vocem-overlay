// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The faces must not change when the decoder does.
//
// The daemon decoded PNGs with stb_image up to 0.1.0-53 and decodes them with
// Wuffs after it. That swap is invisible in every other test: the cache file has
// the same name and the same length whichever decoder wrote it, so "it builds and
// the tests are green" would be true of a decoder that swapped red for blue,
// premultiplied the alpha, or rounded a sixteen-bit sample the other way. Every
// one of those is a change to somebody's face that nobody would report as a bug
// -- they would just think the overlay looked slightly wrong.
//
// So this holds the new decoder to two references at once:
//
//   * **the picture**, computed from the same samples the encoder used
//     (`tests/png_writer.h`), which is the reference that cannot itself be wrong;
//   * **the old decoder**, stb_image, compiled into this test and only into this
//     test, byte for byte through the whole pipeline including the box resample.
//
// Both matter. The picture catches the two decoders being wrong together; stb
// catches the pipeline changing in a way the picture cannot see.
//
// Colour types are walked because a CDN is not obliged to keep serving one:
// RGBA8 is what Discord sends today, and RGB8, grey, grey+alpha, palette and
// sixteen-bit are what the format allows. Interlaced images are NOT covered --
// the encoder here cannot make one -- and that is written down rather than
// implied.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "avatar_decode.h"  // the daemon's decoder: Wuffs

#include "png_writer.h"
#include "vocem/avatar_rgba.h"

// The old decoder, for this test only. Configured exactly as the daemon
// configured it while it shipped, ceiling included.
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)
#define STBI_MAX_DIMENSIONS VOCEM_AVATAR_MAX_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace {

using vocem_test::PngKind;

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%-62s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition) {
        ++g_failures;
    }
}

const char* kind_name(PngKind kind) {
    switch (kind) {
        case PngKind::kRgba8: return "RGBA 8-bit";
        case PngKind::kRgb8: return "RGB 8-bit (no alpha)";
        case PngKind::kGrey8: return "grey 8-bit";
        case PngKind::kGreyA8: return "grey+alpha 8-bit";
        case PngKind::kRgba16: return "RGBA 16-bit";
        case PngKind::kPalette8: return "palette";
    }
    return "?";
}

bool read_file(const std::string& path, std::vector<unsigned char>& into) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0 || info.st_size <= 0) {
        return false;
    }
    into.resize(static_cast<size_t>(info.st_size));
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    const size_t got = std::fread(into.data(), 1, into.size(), file);
    std::fclose(file);
    return got == into.size();
}

// The old pipeline, end to end: stb's decode into the project's own writer.
bool store_with_stb(const std::string& png, const std::string& destination) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()),
                              static_cast<int>(png.size()), &width, &height, &channels, 4);
    if (!pixels) {
        return false;
    }
    const bool stored = vocem::avatar_rgba_write(destination.c_str(), pixels,
                                                 static_cast<uint32_t>(width),
                                                 static_cast<uint32_t>(height));
    stbi_image_free(pixels);
    return stored;
}

// The largest channel difference between two buffers of the same length.
int worst_difference(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    if (a.size() != b.size()) {
        return 256;
    }
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (d > worst) {
            worst = d;
        }
    }
    return worst;
}

}  // namespace

int main() {
    char pattern[] = "/tmp/vocem_decoders_agree_XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    if (!dir) {
        std::printf("no scratch directory\n");
        return 77;
    }
    const std::string root(dir);
    const std::string wuffs_out = root + "/wuffs.rgba";
    const std::string stb_out = root + "/stb.rgba";

    const PngKind kinds[] = {PngKind::kRgba8,   PngKind::kRgb8,    PngKind::kGrey8,
                             PngKind::kGreyA8,  PngKind::kRgba16,  PngKind::kPalette8};

    for (PngKind kind : kinds) {
        // 64x64: the size Discord is asked for, so the writer copies rather than
        // resamples and the comparison is the decoder's own output.
        const std::string png = vocem_test::make_png(64, 64, kind);
        char label[128];

        ::unlink(wuffs_out.c_str());
        ::unlink(stb_out.c_str());
        const bool by_wuffs = vocem::avatar_decode_and_store(
            reinterpret_cast<const unsigned char*>(png.data()), png.size(), wuffs_out.c_str());
        std::snprintf(label, sizeof(label), "%s: the daemon's decoder stores it", kind_name(kind));
        check(by_wuffs, label);
        if (!by_wuffs) {
            continue;
        }

        std::vector<unsigned char> got;
        std::snprintf(label, sizeof(label), "%s: and the file is the format's size",
                      kind_name(kind));
        check(read_file(wuffs_out, got) && got.size() == vocem::kAvatarRgbaBytes, label);

        // Against the picture itself.
        const std::vector<unsigned char> want = vocem_test::expected_rgba(64, 64, kind);
        const int off_picture = worst_difference(got, want);
        std::snprintf(label, sizeof(label), "%s: every channel matches the picture (worst %d)",
                      kind_name(kind), off_picture);
        check(off_picture == 0, label);

        // Against the decoder that used to do this job.
        const bool by_stb = store_with_stb(png, stb_out);
        std::snprintf(label, sizeof(label), "%s: stb_image stores it too", kind_name(kind));
        check(by_stb, label);
        if (by_stb) {
            std::vector<unsigned char> old;
            if (read_file(stb_out, old)) {
                const int between = worst_difference(got, old);
                std::snprintf(label, sizeof(label),
                              "%s: the two decoders agree byte for byte (worst %d)",
                              kind_name(kind), between);
                check(between == 0, label);
            }
        }
    }

    // A size that goes through the box resample as well, so the comparison covers
    // the whole pipeline and not only the decode.
    {
        const std::string png = vocem_test::make_png(256, 256, PngKind::kRgba8);
        ::unlink(wuffs_out.c_str());
        ::unlink(stb_out.c_str());
        const bool a = vocem::avatar_decode_and_store(
            reinterpret_cast<const unsigned char*>(png.data()), png.size(), wuffs_out.c_str());
        const bool b = store_with_stb(png, stb_out);
        check(a && b, "256x256 stored by both, through the box resample");
        std::vector<unsigned char> mine;
        std::vector<unsigned char> theirs;
        if (a && b && read_file(wuffs_out, mine) && read_file(stb_out, theirs)) {
            const int between = worst_difference(mine, theirs);
            char label[128];
            std::snprintf(label, sizeof(label),
                          "and the resampled results are identical (worst %d)", between);
            check(between == 0, label);
        }
    }

    // Both decoders must refuse the same rubbish. A decoder that accepts what the
    // other rejects is a decoder that has just widened the attack surface.
    struct Rubbish {
        std::string bytes;
        const char* what;
    };
    const std::string good = vocem_test::make_png(64, 64, PngKind::kRgba8);
    const Rubbish rubbish[] = {
        {std::string(), "nothing at all"},
        {std::string(4096, '\x01'), "four kilobytes of 0x01"},
        {good.substr(0, good.size() / 2), "half a PNG"},
        {good.substr(0, 30), "a header and nothing else"},
        {vocem_test::make_png(2048, 2048, PngKind::kRgba8, true), "a picture past the ceiling"},
    };
    for (const Rubbish& item : rubbish) {
        ::unlink(wuffs_out.c_str());
        ::unlink(stb_out.c_str());
        const bool mine = vocem::avatar_decode_and_store(
            reinterpret_cast<const unsigned char*>(item.bytes.data()), item.bytes.size(),
            wuffs_out.c_str());
        const bool theirs = store_with_stb(item.bytes, stb_out);
        char label[160];
        std::snprintf(label, sizeof(label), "both decoders refuse %s", item.what);
        check(!mine && !theirs, label);
    }

    ::unlink(wuffs_out.c_str());
    ::unlink(stb_out.c_str());
    ::rmdir(root.c_str());

    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
