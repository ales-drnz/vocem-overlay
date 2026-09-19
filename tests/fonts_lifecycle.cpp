// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The fonts must survive the death of the ImGui context they were built in.
//
// Both injected paths destroy their ImGui context and build a fresh one in the
// same process: the GL overlay when the user switches the overlay off and back
// on and when the game destroys the context it was drawing in (release() ->
// ensure_backend()), the Vulkan layer when a game destroys its device and
// creates another (shutdown() -> init). The fonts module caches ImFont pointers
// and the size the atlas was built at, and its rebuild dead band has to know
// whether those pointers still point at anything.
//
// It has now been wrong about that twice, in the two ways available from
// outside:
//
//   * **By size alone.** Same size, fresh empty atlas: "nothing to do", and
//     fonts().body still aimed into the atlas that died with the old context.
//     Measured in a live GLX process -- switching the overlay off worked,
//     switching it back on crashed the *game* in ImGui::SetCurrentFont, first
//     frame, every time. Entry 37 added "and the atlas is not empty".
//   * **By emptiness.** 0.1.8 made the GL path build its renderer's device
//     objects -- font texture included -- inside ensure_backend(), before asking
//     for the fonts. Building that texture builds the atlas, which puts ImGui's
//     own default font in it, so the fresh atlas was never empty by the time
//     ensure_fonts() was asked and the dead band held again. Same crash, same
//     line, four releases later; reproduced 3/3 in 0.1.10-1. Comparing addresses
//     would not have caught it either: the fresh atlas landed at the same
//     address as the dead one and its default font at the address of the dead
//     heavier weight.
//
// So the module owns the atlas now (vocem/fonts.h), the contexts are created
// with it, and the question the dead band could not answer no longer exists.
// What this test holds is the lifecycle that asked it: build, destroy the
// context, build another over the top, and require that what fonts() hands out
// lives in the atlas the current context is actually drawing from.
//
// The checks compare addresses only. Nothing dereferences a possibly-stale
// pointer, so against a broken fonts module this fails instead of crashing.

#include <stdio.h>

#include "imgui.h"
#include "vocem/fonts.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// Address-only membership: is this font one of the current atlas's own?
bool in_current_atlas(const ImFont* font) {
    if (!font) {
        return false;
    }
    const ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (int i = 0; i < atlas->Fonts.Size; ++i) {
        if (atlas->Fonts[i] == font) {
            return true;
        }
    }
    return false;
}

// A context the way both injected paths create one.
void create_context() { ImGui::CreateContext(vocem::fonts_atlas()); }

}  // namespace

int main() {
    // First life: an ordinary build.
    create_context();
    check(ImGui::GetIO().Fonts == vocem::fonts_atlas(),
          "a context created the injected paths' way draws from the module's atlas");
    check(vocem::ensure_fonts(16.0f, 16.0f), "the first build reports having built");
    check(vocem::fonts().body != nullptr, "and produced a body font");
    check(in_current_atlas(vocem::fonts().body), "that lives in the current atlas");

    // Same context, same size: the dead band exists so a slider being dragged
    // does not rebuild the atlas every frame, and it must keep working.
    check(!vocem::ensure_fonts(16.0f, 16.0f),
          "the same size in the same context rebuilds nothing");

    // Second life: the toggle, the game's own context cycle, or the second
    // Vulkan device. The context dies and a fresh one appears, at exactly the
    // same font size -- and there is nothing to rebuild, because the atlas it
    // draws from never died.
    ImGui::DestroyContext();
    create_context();
    check(!vocem::ensure_fonts(16.0f, 16.0f),
          "after the context is recreated, the same size rebuilds nothing");
    check(in_current_atlas(vocem::fonts().body),
          "and the body font lives in the new context's atlas");
    check(in_current_atlas(vocem::fonts().strong),
          "and so does the heavier weight");

    // Third life: the same recreation with the renderer's backend having asked
    // for the pixels first, which is the order the GL path has run in since
    // 0.1.8 and the order that crashed. GetTexDataAsRGBA32 is what
    // ImGui_ImplOpenGL3_CreateFontsTexture calls, and on a surviving atlas it
    // hands back what is already there rather than rasterising again.
    ImGui::DestroyContext();
    create_context();
    {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        check(pixels != nullptr && ImGui::GetIO().Fonts->Fonts.Size > 0,
              "the renderer finds an atlas with pixels and fonts already in it");
    }
    check(!vocem::ensure_fonts(16.0f, 16.0f),
          "and the same size still rebuilds nothing");
    check(in_current_atlas(vocem::fonts().body),
          "and the body font lives in that atlas");
    check(in_current_atlas(vocem::fonts().strong),
          "and so does the heavier weight");

    // A size change still rebuilds -- the dead band is a dead band, not a latch.
    check(vocem::ensure_fonts(24.0f, 16.0f), "a different size rebuilds");
    check(in_current_atlas(vocem::fonts().body), "into the same atlas");

    // Switched off: the pixels go back, and the next build starts from nothing.
    vocem::fonts_release();
    check(vocem::fonts().body == nullptr && vocem::fonts().pixel_size == 0.0f,
          "being released forgets the fonts it was holding");
    check(vocem::ensure_fonts(24.0f, 16.0f), "and the same size builds again after it");
    check(in_current_atlas(vocem::fonts().body), "into the atlas the context draws from");

    // Only now is it safe to actually use one, which is what build_panel does on
    // the first frame after the overlay comes back.
    if (failures == 0) {
        ImGui::GetIO().Fonts->Build();
        ImGui::GetIO().DisplaySize = ImVec2(640.0f, 480.0f);
        ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(1));
        ImGui::NewFrame();
        ImGui::PushFont(const_cast<ImFont*>(vocem::fonts().strong));
        ImGui::PopFont();
        ImGui::EndFrame();
        printf("ok   and PushFont on the rebuilt font draws no blood\n");
    }

    ImGui::DestroyContext();
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
