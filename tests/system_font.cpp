// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay draws in the font the settings name, and says so when it cannot.
//
// Four claims, all measured against a real font file on this machine rather
// than against a fixture, because the thing being tested is reading somebody
// else's font:
//
//   1. A chosen file is what the atlas is built from -- the same string does not
//      measure the same as it does in the carried Inter.
//   2. Inter is merged underneath it, so a script the chosen face does not carry
//      is still drawn instead of turning into question marks. Measured on the
//      atlas's own bookkeeping: the body font is built from one source more than
//      the built-in build uses.
//   3. A path that cannot be read falls back to Inter **and says why**. A font
//      that quietly does not load is a setting that quietly does nothing, which
//      is entry 38's lesson in a new place.
//   4. The dead band that decides "nothing to do" compares the font as well as
//      the size. Without that, a font picked in the window would not reach a
//      running game until its resolution changed (entry 37's shape).
//
// Cases 5 to 8 are the other half: a path is a line in a text file, so the
// bytes behind it are an input. Each of the four is a way stb_truetype was
// handed something it does not check -- a file that is not a font, a font it
// cannot rasterise, a real font cut short, a collection whose version it will
// not follow -- and three of the four crashed the process before the check that
// answers them existed.
//
// Against the 0.1.2 library there is no such thing as a chosen font at all, so
// every claim here fails to compile rather than to run -- which is the honest
// state of a feature that did not exist.
//
// Skips with 77 when no usable font is installed: this measures the machine's
// own fonts, and a machine without any is not a failing overlay.

#include <dirent.h>
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// Somewhere on this machine, a real outline font. The list is by directory
// rather than by family: what matters is that the bytes are a font somebody
// installed, not which one it is.
void collect_fonts(const std::string& directory, std::vector<std::string>& out, int depth) {
    if (depth > 3 || out.size() >= 64) {
        return;
    }
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        return;
    }
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = directory + "/" + entry->d_name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            collect_fonts(path, out, depth + 1);
            continue;
        }
        const size_t length = path.size();
        // Static TTFs only: a variable font's default instance is what
        // stb_truetype draws, which is a fine thing to draw and a poor thing to
        // measure a claim about, and .otf is CFF, which stb_truetype does not
        // rasterise at all.
        if (length > 4 && std::strcmp(path.c_str() + length - 4, ".ttf") == 0) {
            out.push_back(path);
        }
        if (out.size() >= 64) {
            break;
        }
    }
    closedir(dir);
}

// A file of exactly these bytes, padded with zeroes to `size` -- which is past
// the hundred bytes ImGui's own "is this really a font" heuristic wants to see,
// so the case being measured is the one named rather than that check.
void write_file(const char* path, const unsigned char* head, size_t head_size, size_t size) {
    FILE* file = std::fopen(path, "wb");
    if (!file) {
        std::printf("  FAIL  could not write %s\n", path);
        ++failures;
        return;
    }
    std::fwrite(head, 1, head_size, file);
    for (size_t i = head_size; i < size; ++i) {
        std::fputc(0, file);
    }
    std::fclose(file);
}

// The first `bytes` of a real font, and nothing after them: a file that was cut
// off rather than a file that was never a font. An interrupted copy, a font
// manager still writing, a partly synced home directory -- and a hand-edited
// settings file pointing at any of them.
bool write_prefix(const char* path, const std::string& source, size_t bytes) {
    FILE* in = std::fopen(source.c_str(), "rb");
    if (!in) {
        return false;
    }
    std::vector<unsigned char> head(bytes);
    const size_t got = std::fread(head.data(), 1, bytes, in);
    std::fclose(in);
    if (got != bytes) {
        return false;  // the chosen font is smaller than this cut: not this case
    }
    FILE* out = std::fopen(path, "wb");
    if (!out) {
        return false;
    }
    std::fwrite(head.data(), 1, got, out);
    std::fclose(out);
    return true;
}

float width_of(ImFont* font, float size) {
    return font ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, "Participant 1 gjqQWM").x : 0.0f;
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    // The context the injected paths create: with the fonts module's own
    // atlas, which is what makes the rebuild dead band a promise at all
    // (vocem/fonts.h).
    ImGui::CreateContext(vocem::fonts_atlas());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);

    const float size = 24.0f;

    // The built-in build first: its measurements are what a chosen font has to
    // differ from, and its source count is what the merge has to exceed.
    check(vocem::ensure_fonts(size, 16.0f), "the built-in build reports having built");
    const float inter_width = width_of(vocem::fonts().body, size);
    const short inter_sources = vocem::fonts().body ? vocem::fonts().body->SourcesCount : 0;
    check(inter_width > 0.0f, "the built-in font measures something");
    check(vocem::fonts_font_status() == nullptr,
          "the built-in font has nothing to explain");

    std::vector<std::string> candidates;
    for (const char* root : {"/usr/share/fonts", "/usr/local/share/fonts"}) {
        collect_fonts(root, candidates, 0);
    }

    // A font that measures exactly as Inter does would make claim 1 unprovable,
    // so the first candidate that does not is the one used -- and if every
    // installed font measures the same as Inter, this machine cannot answer the
    // question and says so rather than passing.
    std::string chosen;
    float chosen_width = 0.0f;
    for (const std::string& candidate : candidates) {
        if (!vocem::ensure_fonts(size, 16.0f, candidate.c_str(), candidate.c_str())) {
            continue;  // refused as unchanged: not possible here, but not a font either
        }
        if (vocem::fonts_font_status() != nullptr) {
            continue;  // unreadable or unrasterisable: the next candidate
        }
        const float width = width_of(vocem::fonts().body, size);
        if (width > 0.0f && std::fabs(width - inter_width) > 1.0f) {
            chosen = candidate;
            chosen_width = width;
            break;
        }
    }

    if (chosen.empty()) {
        std::printf("skip no installed .ttf measures differently from the carried Inter\n");
        return 77;
    }
    std::printf("  chosen font: %s\n", chosen.c_str());
    std::printf("  the same string: %.2f built-in, %.2f chosen\n", inter_width, chosen_width);

    // 1. The atlas is built from the file.
    check(std::fabs(chosen_width - inter_width) > 1.0f,
          "the chosen font draws the string at its own width, not Inter's");

    // 2. Inter is merged underneath it. One source more than the built-in build
    //    has, which is the merge, and it is the *body* font that is asked --
    //    a merged source belongs to the font it was merged into (entry 26).
    check(vocem::fonts().body != nullptr && vocem::fonts().body->SourcesCount > inter_sources,
          "Inter is merged under the chosen font as its fallback");
    check(vocem::fonts().strong != nullptr && vocem::fonts().strong->SourcesCount > inter_sources,
          "and under the heavier weight as well");

    // 4a. The dead band knows the font: same size, same file, no rebuild.
    check(!vocem::ensure_fonts(size, 16.0f, chosen.c_str(), chosen.c_str()),
          "the same font at the same size does not rebuild");
    // 4b. And a changed file rebuilds at an unchanged size, which is the whole
    //     point: the window writes a path, the game's next post-present sees it.
    check(vocem::ensure_fonts(size, 16.0f, nullptr, nullptr),
          "dropping back to the built-in font rebuilds");
    check(std::fabs(width_of(vocem::fonts().body, size) - inter_width) < 0.01f,
          "and the built-in font measures what it did before");

    // 3. A path that cannot be read: Inter, and a reason.
    check(vocem::ensure_fonts(size, 16.0f, "/nonexistent/not-a-font.ttf",
                              "/nonexistent/not-a-font.ttf"),
          "an unreadable font rebuilds rather than keeping the old atlas");
    check(vocem::fonts_font_status() != nullptr,
          "an unreadable font is explained rather than silently ignored");
    if (const char* reason = vocem::fonts_font_status()) {
        std::printf("  refusal: %s\n", reason);
    }
    check(vocem::fonts().body != nullptr, "there is still a font to draw with");
    check(std::fabs(width_of(vocem::fonts().body, size) - inter_width) < 0.01f,
          "and it is the carried Inter");
    check(vocem::fonts().body->SourcesCount == inter_sources,
          "with nothing merged under it that is not usually there");

    // A directory is not a font, and is the mistake a file dialog makes easy.
    check(vocem::ensure_fonts(size, 16.0f, "/usr/share/fonts", "/usr/share/fonts"),
          "a directory rebuilds");
    check(vocem::fonts_font_status() != nullptr, "a directory is refused out loud");

    // 5. A file that exists, is regular, is past ImGui's hundred-byte heuristic
    //    and is not a font. This is the one the reachable-through-a-text-file
    //    argument is about, and against the library before this test it did not
    //    fail -- it crashed: stbtt_GetFontOffsetForIndex answers -1, ImGui's
    //    assertion on that is compiled out of a release build, and stb parses
    //    from data-1 with a table count read out of whatever followed
    //    (SIGSEGV at imstb_truetype.h:1313, fontstart 4294967295, inside
    //    ensure_fonts, which is inside somebody's game).
    {
        const char* path = "vocem-not-a-font.ttf";
        static const unsigned char text[] = "this file is not a font at all";
        write_file(path, text, sizeof(text) - 1, 200);
        check(vocem::ensure_fonts(size, 16.0f, path, path), "a file that is not a font rebuilds");
        check(vocem::fonts_font_status() != nullptr,
              "a file that is not a font is refused out loud");
        if (const char* reason = vocem::fonts_font_status()) {
            std::printf("  refusal: %s\n", reason);
        }
        check(std::fabs(width_of(vocem::fonts().body, size) - inter_width) < 0.01f,
              "and the carried Inter is what draws instead");
        std::remove(path);
    }

    // 6. And a file whose header *is* a font's and whose contents the rasteriser
    //    still cannot use: an sfnt signature with no tables under it, which is
    //    what a variable OpenType looks like to stb_truetype (CFF2 is not
    //    implemented -- measured on Cantarell-VF.otf, which produced an atlas of
    //    0x0 pixels and every font pointer unloaded). Nothing checked Build()'s
    //    answer, so the overlay drew no text at all and said nothing: exactly
    //    the silence entry 38 is about, one layer further in.
    {
        const char* path = "vocem-empty-sfnt.ttf";
        // "OTTO", then a table count of zero: a well-formed offset table with
        // nothing in it, which passes every header check and has no glyphs.
        static const unsigned char sfnt[] = {'O', 'T', 'T', 'O', 0, 0};
        write_file(path, sfnt, sizeof(sfnt), 200);
        check(vocem::ensure_fonts(size, 16.0f, path, path), "an unrasterisable font rebuilds");
        check(vocem::fonts_font_status() != nullptr,
              "a font the rasteriser refuses is explained rather than left blank");
        if (const char* reason = vocem::fonts_font_status()) {
            std::printf("  refusal: %s\n", reason);
        }
        check(std::fabs(width_of(vocem::fonts().body, size) - inter_width) < 0.01f,
              "and the carried Inter is what draws instead");
        // The atlas is the claim: an overlay with no pixels draws nothing, which
        // is what this whole case exists to stop.
        unsigned char* pixels = nullptr;
        int atlas_width = 0;
        int atlas_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
        check(pixels != nullptr && atlas_width > 0 && atlas_height > 0,
              "and the atlas has pixels in it");

        // And the refusal is REMEMBERED. The fallback forgets the paths it could
        // not use, so a dead band that compared those paths never matched the
        // settings again: every frame rebuilt the whole atlas and re-uploaded the
        // font texture -- 61 ms per call measured here, at 24 px, once per frame
        // inside somebody's game, with a vkQueueWaitIdle behind it on the Vulkan
        // side. The dead band compares what was ASKED for.
        check(!vocem::ensure_fonts(size, 16.0f, path, path),
              "and asking for the same unrasterisable font again does not rebuild");
        std::remove(path);
    }

    // 7. A real font, cut short. stb_truetype does no bounds checking of its
    //    own, so a file with a font's header and a table directory pointing past
    //    the end of what was read is a read past the end of the buffer: measured
    //    against the library that shipped 0.1.3 as SIGSEGV at every one of these
    //    three lengths. This is the reachable-by-accident half of entry 92 --
    //    an interrupted copy, a file still being written -- where case 5 is the
    //    reachable-by-mistake half.
    for (size_t bytes : {size_t{400}, size_t{1024}, size_t{4096}}) {
        // A name of its own per length: the dead band compares the path, so
        // three cuts under one filename would be one build and two no-ops.
        char path_buffer[64];
        std::snprintf(path_buffer, sizeof(path_buffer), "vocem-truncated-%zu.ttf", bytes);
        const char* path = path_buffer;
        if (!write_prefix(path, chosen, bytes)) {
            continue;  // the chosen font is smaller than this cut
        }
        check(vocem::ensure_fonts(size, 16.0f, path, path), "a truncated font rebuilds");
        check(vocem::fonts_font_status() != nullptr,
              "a truncated font is refused out loud rather than parsed past its end");
        check(std::fabs(width_of(vocem::fonts().body, size) - inter_width) < 0.01f,
              "and the carried Inter is what draws instead");
        std::remove(path);
    }

    // 8. A collection whose header version stb_truetype does not follow.
    //    stbtt_GetFontOffsetForIndex reads a `ttcf` only at version 1.0 or 2.0
    //    (imstb_truetype.h:1333) and answers -1 for anything else -- the same -1
    //    that case 5 is about, arriving through a different door, and the header
    //    check that answered case 5 did not ask this question.
    {
        const char* path = "vocem-odd-collection.ttf";
        static const unsigned char ttc[] = {
            't', 't', 'c', 'f',       // the collection tag
            0, 3, 0, 0,               // version 3.0: not one stb follows
            0, 0, 0, 1,               // one face
            0, 0, 0, 16,              // whose offset table is at 16
            0, 1, 0, 0, 0, 0, 0, 0,   // a well-formed, empty OpenType directory
        };
        write_file(path, ttc, sizeof(ttc), 200);
        check(vocem::ensure_fonts(size, 16.0f, path, path), "an odd collection rebuilds");
        check(vocem::fonts_font_status() != nullptr,
              "a collection stb_truetype will not follow is refused before it is handed over");
        check(std::fabs(width_of(vocem::fonts().body, size) - inter_width) < 0.01f,
              "and the carried Inter is what draws instead");
        std::remove(path);
    }

    // And a font that loads clears the explanation: a refusal that outlived the
    // setting it was about would be a log that lies from then on.
    check(vocem::ensure_fonts(size, 16.0f, chosen.c_str(), chosen.c_str()),
          "the chosen font builds again");
    check(vocem::fonts_font_status() == nullptr,
          "a font that loads leaves nothing to explain");

    ImGui::DestroyContext();
    if (failures > 0) {
        std::printf("system font: %d failures\n", failures);
        return 1;
    }
    std::printf("system font: the chosen face is drawn, and a refusal is said out loud\n");
    return 0;
}
