// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The embedded font has to actually rasterise. A missing or truncated TTF does not
// crash ImGui: it falls back, or it builds an atlas with nothing in it, and the
// first anyone would know is a game with no names in the panel.
//
// So this builds the atlas the way the overlay does, off-screen, and checks the
// glyphs are there. With a path argument it also writes the atlas out as a PGM,
// which is how the rendering was looked at rather than assumed.
//
//     ./vocem_font_atlas /tmp/atlas.pgm

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imgui.h"
#include "vocem/fonts.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++failures;
    }
}

// A glyph is present when the atlas has an entry for it that is not the fallback.
void check_glyph(ImFont* font, ImWchar code, const char* what) {
    const ImFontGlyph* glyph = font->FindGlyphNoFallback(code);
    check(glyph != nullptr, what);
}

}  // namespace

int main(int argc, char** argv) {
    IMGUI_CHECKVERSION();
    // The context the injected paths create: with the fonts module's own
    // atlas, which is what makes the rebuild dead band a promise at all
    // (vocem/fonts.h).
    ImGui::CreateContext(vocem::fonts_atlas());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);

    // What a 4K output asks for, which is the case the bitmap font was ugly in.
    const float size = vocem::font_pixel_size(2160, 1.0f, 16.0f);
    std::printf("font_pixel_size(2160, 1.0) = %.1f\n", size);
    check(size > 25.0f && size < 40.0f, "a 4K output gets a large-but-sane size");
    check(vocem::font_pixel_size(1080, 1.0f, 16.0f) > 14.0f, "1080p gets readable text");
    check(vocem::font_pixel_size(240, 1.0f, 16.0f) >= 11.0f, "a tiny window is clamped upwards");
    check(vocem::font_pixel_size(4320, 3.0f, 16.0f) <= 64.0f, "8K at 3x scale is clamped downwards");

    check(vocem::ensure_fonts(size, 16.0f), "the first build reports a change");
    check(!vocem::ensure_fonts(size, 16.0f), "an unchanged size does not rebuild");
    check(vocem::ui_scale() > 0.0f, "the layout has a scale to work in");
    check(vocem::ensure_fonts(size * 2.0f, 16.0f), "a different size does rebuild");
    check(vocem::ensure_fonts(size, 16.0f), "and back again");

    const vocem::Fonts& fonts = vocem::fonts();
    check(fonts.body != nullptr, "the body font exists");
    check(fonts.strong != nullptr, "the strong font exists");
    check(fonts.body != fonts.strong, "the two weights are distinct fonts");
    check(fonts.pixel_size == size, "the recorded size matches what was asked for");

    // Both weights, and not only the body: the channel name is drawn in the
    // heavier one, and it is exactly where an emoji or a middle dot turns up. A
    // merged glyph belongs to the font it was merged into, so having it in one
    // says nothing about the other.
    for (ImFont* font : {fonts.body, fonts.strong}) {
        if (!font) {
            continue;
        }
        // Not the built-in bitmap: ProggyClean has no accented or Cyrillic glyphs,
        // so these are what tells the two apart.
        check_glyph(font, 'A', "Latin capital A");
        check_glyph(font, 0x00E0, "a-grave, for Italian display names");
        check_glyph(font, 0x0414, "Cyrillic De");
        check_glyph(font, 0x03A9, "Greek Omega");

        // The symbols a name gets decorated with. Every one of these was drawn as
        // a question mark while the range list stopped at the arrows.
        check_glyph(font, 0x2605, "a black star");
        check_glyph(font, 0x2713, "a tick");
        check_glyph(font, 0x2022, "a bullet");
        check_glyph(font, 0x2665, "a heart suit");

        // What a Discord channel name is written with, and Inter has none of.
        check_glyph(font, 0x30FB, "the katakana middle dot");
        check_glyph(font, 0x3002, "the ideographic full stop");
        check_glyph(font, 0xFF01, "a fullwidth exclamation mark");

        // Emoji, which need ImWchar to be 32 bits wide: without IMGUI_USE_WCHAR32
        // the range is truncated and none of these exist.
        check_glyph(font, 0x1F50A, "the loudspeaker emoji");
        // The emoji that live below U+1F000, among the symbols, which a range
        // starting at U+1F000 left as question marks.
        check_glyph(font, 0x2B50, "the star emoji");
        check_glyph(font, 0x2705, "the heavy tick emoji");
        check_glyph(font, 0x1F3AE, "the game controller emoji");
        check_glyph(font, 0x1F600, "a grinning face");
        // Not a glyph anybody sees: it is what keeps a question mark from being
        // drawn after every emoji that carries one.
        check_glyph(font, 0xFE0F, "the emoji variation selector");

        check(font->FindGlyphNoFallback(0x4E2D) == nullptr,
              "whole CJK is still deliberately absent");
    }

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    // A build leaves the RGBA32 copy widened, where the build ran, for the
    // whole upload that follows it; once that upload is done the module hands
    // the copy back and the alpha8 image -- a quarter of its size, 11 MB
    // against 43 at a 2160-line display -- is the atlas until the next whole
    // upload widens it again (entry 207 kept the RGBA copy instead, for the
    // life of the process). Asked of the atlas's fields BEFORE reading pixels,
    // because asking ImGui for pixels widens or rebuilds whatever is missing.
    check(io.Fonts->TexPixelsAlpha8 != nullptr && io.Fonts->TexPixelsRGBA32 != nullptr,
          "a build leaves the RGBA32 copy widened for its upload, beside the alpha8 image");
    const auto sum = [](const unsigned char* bytes, size_t count) {
        unsigned long long total = 1469598103934665603ull;  // FNV-1a
        for (size_t i = 0; i < count; ++i) {
            total = (total ^ bytes[i]) * 1099511628211ull;
        }
        return total;
    };
    const unsigned long long built =
        sum(reinterpret_cast<const unsigned char*>(io.Fonts->TexPixelsRGBA32),
            static_cast<size_t>(io.Fonts->TexWidth) * io.Fonts->TexHeight * 4);
    vocem::fonts_atlas_uploaded();
    check(io.Fonts->TexPixelsRGBA32 == nullptr && io.Fonts->TexPixelsAlpha8 != nullptr,
          "after the whole upload the atlas holds its alpha8 image and not the RGBA32 copy");
    check(vocem::fonts_atlas_rgba(&pixels, &width, &height),
          "the next whole upload widens it again");
    check(pixels != nullptr && sum(pixels, static_cast<size_t>(width) * height * 4) == built,
          "  into the same pixels, byte for byte");
    std::printf("atlas: %dx%d\n", width, height);
    check(pixels != nullptr && width > 0 && height > 0, "the atlas has pixels");
    check(width <= 4096 && height <= 4096, "the atlas fits a conservative texture limit");

    // Something is actually drawn in it: an atlas of the right size full of zeroes
    // would pass every check above.
    long ink = 0;
    for (int i = 0; i < width * height; ++i) {
        ink += pixels[i * 4 + 3];  // the coverage is the alpha channel
    }
    std::printf("average coverage: %.1f%%\n",
                100.0 * static_cast<double>(ink) / (255.0 * width * height));
    check(ink > 0, "the atlas is not blank");

    if (argc > 1) {
        // PGM: eight lines of header and a byte per pixel, so no image library is
        // needed to write something that can be looked at.
        if (std::FILE* file = std::fopen(argv[1], "wb")) {
            std::fprintf(file, "P5\n%d %d\n255\n", width, height);
            for (int i = 0; i < width * height; ++i) {
                std::fputc(pixels[i * 4 + 3], file);
            }
            std::fclose(file);
            std::printf("wrote %s\n", argv[1]);
        }
    }

    ImGui::DestroyContext();
    std::printf(failures == 0 ? "OK\n" : "%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
