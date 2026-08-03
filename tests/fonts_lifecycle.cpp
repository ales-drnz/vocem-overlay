// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The fonts must survive the death of the ImGui context they were built in.
//
// Both injected paths destroy their ImGui context and build a fresh one in the
// same process: the GL overlay every time the user switches the overlay off and
// back on (release() -> ensure_backend()), the Vulkan layer when a game destroys
// its device and creates another (shutdown() -> init). The fonts module caches
// ImFont pointers and the size the atlas was built at -- and its rebuild dead
// band used to look at the size alone. Same size, fresh empty atlas: it answered
// "nothing to do" and left fonts().body pointing into the atlas that died with
// the old context. The first PushFont dereferenced it.
//
// Measured in a live GLX process before it was understood: switching the overlay
// off worked, switching it back on crashed the *game* with SIGSEGV in
// ImGui::SetCurrentFont, first frame, every time -- found by a probe that read
// its own front buffer back to watch the overlay appear and disappear. This test
// is the same lifecycle without a GL context, since the fault is entirely
// CPU-side: build, destroy the context, build again, and require that the second
// build actually happened and produced fonts that live in the *current* atlas.
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

}  // namespace

int main() {
    // First life: an ordinary build.
    ImGui::CreateContext();
    check(vocem::ensure_fonts(16.0f, 16.0f), "the first build reports having built");
    check(vocem::fonts().body != nullptr, "and produced a body font");
    check(in_current_atlas(vocem::fonts().body), "that lives in the current atlas");

    // Same context, same size: the dead band exists so a slider being dragged
    // does not rebuild the atlas every frame, and it must keep working.
    check(!vocem::ensure_fonts(16.0f, 16.0f),
          "the same size in the same context rebuilds nothing");

    // Second life: the toggle, or the second Vulkan device. The context dies and
    // a fresh one appears, at exactly the same font size.
    ImGui::DestroyContext();
    ImGui::CreateContext();
    check(vocem::ensure_fonts(16.0f, 16.0f),
          "after the context is recreated, the same size builds again");
    check(in_current_atlas(vocem::fonts().body),
          "and the body font lives in the new context's atlas");
    check(in_current_atlas(vocem::fonts().strong),
          "and so does the heavier weight");

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
