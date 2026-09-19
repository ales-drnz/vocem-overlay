// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Vocem Overlay - OpenGL interposer.
//
// Vulkan has a layer mechanism; OpenGL has nothing of the sort, so the overlay
// gets in by symbol interposition. Three levels, because applications reach GL in
// three different ways:
//
//   1. Direct calls resolved by the dynamic linker  -> LD_PRELOAD is enough.
//   2. glXGetProcAddress / eglGetProcAddress lookups -> those are hooked too.
//   3. dlsym() called by the application itself      -> dlsym is hooked as
//      well, IN THE SHIM (gl/src/vocem_gl_shim.cpp): SDL, GLFW and glad
//      dlopen their GL and dlsym on that handle, invisible to interposition
//      by construction. All three doors live in the shim since the entry-45
//      rebuild took this file's own hook table away; this library only draws
//      when the shim hands it a frame.
//
// The panel is the shared implementation in common/src/panel.cpp: this file only
// deals with getting a frame, a size, and a texture upload path.

#include <dlfcn.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "real_dlsym.h"
#include "vocem/apps.h"
#include "vocem/avatar_file.h"
#include "vocem/avatar_key.h"
#include "vocem/avatar_rgba.h"
#include "vocem/clock.h"
#include "vocem/draw_decision.h"
#include "vocem/flatpak.h"
#include "vocem/fonts.h"
#include "vocem/journal.h"
#include "vocem/live_config.h"
#include "vocem/overlay_log.h"
#include "vocem/overlay_session.h"
#include "vocem/panel.h"
#include "vocem/shared_state.h"
#include "vocem/shm.h"
#include "vocem/state_poll.h"

// No image parser in here, on purpose. The cache is raw RGBA at one fixed size
// (vocem/avatar_rgba.h); the daemon is the only process that ever decodes a PNG.
// stb_image lived in this file for four packages, parsing internet-supplied
// bytes inside every game the overlay drew in.

// A minimal slice of the GL and window-system ABIs. Declaring what we use avoids
// a build dependency on the GL headers of whichever driver is installed.
extern "C" {
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef void GLvoid;
}

namespace {

constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_VERSION = 0x1F02;

using PFN_glGenTextures = void (*)(GLsizei, GLuint*);
using PFN_glBindTexture = void (*)(GLenum, GLuint);
using PFN_glTexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                  const GLvoid*);
using PFN_glTexParameteri = void (*)(GLenum, GLenum, GLint);
using PFN_glGetIntegerv = void (*)(GLenum, GLint*);
using PFN_glPixelStorei = void (*)(GLenum, GLint);
using PFN_glBindBuffer = void (*)(GLenum, GLuint);
using PFN_glBindFramebuffer = void (*)(GLenum, GLuint);
using PFN_glIsEnabled = unsigned char (*)(GLenum);
using PFN_glEnable = void (*)(GLenum);
using PFN_glDisable = void (*)(GLenum);
using PFN_glGetString = const unsigned char* (*)(GLenum);

// One logger for both injected paths (vocem/overlay_log.h): VOCEM_DEBUG on
// stderr, VOCEM_LOG_FILE appended with the pid, both read once. The tag is
// what tests/gl_probe_witness.cmake matches our lines by.
#define VOCEM_GLOG(...) VOCEM_OVERLAY_LOG("vocem/gl", __VA_ARGS__)

// The state poll's one-line events, through this path's own log (pid-stamped
// file logging included). vocem/state_poll.h takes a function pointer so the
// shared loop does not know either path's macro.
void gl_poll_log(const char* line) { VOCEM_GLOG("%s", line); }

bool overlay_disabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("VOCEM_DISABLE");
        return env && env[0] == '1';
    }();
    return disabled;
}

// The real dlsym, reached through dlvsym so it bypasses the interposed dlsym
// in the shim -- which is in this process, even though this file no longer
// hooks anything itself (entry 45 removed its table). Every internal lookup
// must go through this: asking the interposed dlsym for "glXSwapBuffers"
// would hand back the shim's hook, and the hook would call itself for every
// frame until the stack ran out.
//
// The version is looked for and not assumed -- see real_dlsym.h. This was the
// **second** copy of that line, and it outlived the fix to the first by exactly as
// long as it took somebody to start a 32-bit game: the shim was corrected, this was
// not, and here the failure is quiet rather than fatal. Nothing crashes. Every
// `next_symbol` below returns null, so `glXQueryDrawable` is not found, the drawable
// comes back 0x0, `glGetIntegerv` is not found either so the fallback cannot run,
// and `draw()` returns having drawn nothing -- once per frame, for the life of the
// game. The overlay is loaded, is detected, says it is drawing, and is not there.
void* real_dlsym(void* handle, const char* name) {
    using PFN_dlsym = void* (*)(void*, const char*);
    // A null is not remembered -- the shim's rule, applied here too: a static
    // initialised once from a failed lookup would answer null for the life
    // of the process, and the whole point of looking rather than assuming
    // (real_dlsym.h) is lost if the first look is the only one.
    static PFN_dlsym real = nullptr;
    if (!real) {
        static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
        for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]) && !real; ++i) {
            real = reinterpret_cast<PFN_dlsym>(dlvsym(RTLD_NEXT, "dlsym", versions[i]));
        }
    }
    return real ? real(handle, name) : nullptr;
}

// Resolves a GL or GLX function this library does *not* interpose.
//
// RTLD_NEXT first, which is right when this library was preloaded directly, and
// then RTLD_DEFAULT, which is what makes it work when it was not. That second
// attempt is not belt-and-braces: it is the case that matters. In a real install
// nothing preloads this library -- the shim does, and it brings this one in
// with dlopen(RTLD_LOCAL) on the first GL frame. RTLD_NEXT inside a library loaded
// that way searches the objects after it in *its own* local scope, which does not
// contain libGL, so every lookup returned null.
//
// The visible effect was that glXQueryDrawable could not be found, the drawable
// size came back as 0x0, the fallback through glGetIntegerv could not be found
// either, and draw() returned before drawing anything. The overlay never appeared
// in an OpenGL game, and cost so little that the measurement looked like success.
//
// Only safe for functions the SHIM does not hook: asking RTLD_DEFAULT for
// glXSwapBuffers would find its interposed copy -- ahead of libGL in the
// global scope -- and call it forever.
template <typename Fn>
Fn next_symbol(const char* name) {
    if (void* found = real_dlsym(RTLD_NEXT, name)) {
        return reinterpret_cast<Fn>(found);
    }
    return reinterpret_cast<Fn>(real_dlsym(RTLD_DEFAULT, name));
}

// The third road, for the games where the two above find nothing at all.
//
// Minecraft, and everything else built on GLFW, LWJGL or SDL, opens its GL
// library with dlopen(RTLD_LOCAL): no GL symbol is in the global scope, so
// RTLD_NEXT and RTLD_DEFAULT both come back empty -- for every function this
// library does not interpose. glXQueryDrawable was null, the fallback through
// glGetIntegerv was null, the drawable read 0x0, and the overlay logged
// "no drawable size: nothing drawn this frame" once per frame for the life of
// the game. Measured on Minecraft 26.2, from inside, the first time the debug
// log could be read out of a launcher that swallows the game's stderr. glxgears
// had been the wrong witness all along: it *links* libGL, and for the loaders
// that matter -- the level-3 door the shim exists for -- linking is the
// exception.
//
// What is always reachable is a *dispatcher*: RTLD_DEFAULT finds our own
// exported glXGetProcAddress/eglGetProcAddress (the shim's, or this library's
// when it was preloaded directly), and those forward to the real dispatcher
// from the moment the application's own resolution revealed it -- which happens
// before the first frame, because resolving the dispatcher is how such a game
// finds every other function too. Asked only for names we do not interpose: for
// an interposed one the hook would answer with itself.
//
// The result is not cached when it is null -- the dispatcher becomes known when
// the application reveals it, and a null must not outlive that moment.
void* dispatcher_symbol(const char* name) {
    using PFN_lookup = void* (*)(const char*);
    static PFN_lookup glx_arb = nullptr;
    static PFN_lookup glx = nullptr;
    static PFN_lookup egl = nullptr;
    if (!glx_arb) {
        glx_arb = reinterpret_cast<PFN_lookup>(real_dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));
    }
    // Both GLX spellings, because each of the shim's dispatcher slots is
    // filled by the application's own resolution of THAT name (the dlsym
    // door): a loader that asks only for the plain glXGetProcAddress leaves
    // the ARB slot empty, and asking the ARB export alone would find nothing
    // in exactly that process.
    if (!glx) {
        glx = reinterpret_cast<PFN_lookup>(real_dlsym(RTLD_DEFAULT, "glXGetProcAddress"));
    }
    if (!egl) {
        egl = reinterpret_cast<PFN_lookup>(real_dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    }
    for (PFN_lookup lookup : {glx_arb, glx, egl}) {
        if (lookup) {
            if (void* found = lookup(name)) {
                return found;
            }
        }
    }
    return nullptr;
}

// The full resolution for a GL function this library does not interpose, with
// the retry the third road requires: a null is asked again on the next call,
// never remembered.
template <typename Fn>
Fn gl_symbol(const char* name) {
    if (Fn found = next_symbol<Fn>(name)) {
        return found;
    }
    return reinterpret_cast<Fn>(dispatcher_symbol(name));
}

// ---------------------------------------------------------------------------
// GL texture provider
// ---------------------------------------------------------------------------

// The application's pixel-store state, neutralised for one upload and put back.
//
// glTexImage2D does not read a client pointer as a plain array: it reads it
// through GL_UNPACK_*, which is application state that survives a swap. A game
// that left GL_UNPACK_ROW_LENGTH at 2048 makes a 64x64 upload read half a
// megabyte from our 16 KB buffer -- measured as a SIGSEGV attributed to the game
// -- and a game with a pixel-unpack buffer bound makes the pointer an offset into
// *its* buffer, so the upload silently fails and pushes GL_INVALID_OPERATION into
// the game's error queue. ImGui's own backend carries the same lesson in a
// comment from 2016 ("SDL changes it"), and neutralises these only inside
// CreateDeviceObjects -- which is not the path a font-atlas rebuild takes.
//
// Every value is read before it is written and written back afterwards: this runs
// inside somebody else's renderer and rule 12 is that we leave no state changed.
class PixelStoreGuard {
public:
    // `es` is 0 for desktop GL, or the OpenGL ES major version. Which of these
    // names exist depends on it, and asking for one that does not is
    // GL_INVALID_ENUM -- an error in the game's queue, which is the very thing
    // this class is here to avoid. ES 2 has the alignment alone; ES 3 adds the
    // row length and the two skips but has neither of the byte-order flags;
    // desktop GL has all six and the pixel-unpack buffer, which ES gained in 3.0.
    PixelStoreGuard(PFN_glGetIntegerv get, PFN_glPixelStorei set, PFN_glBindBuffer bind, int es)
        : get_(get), set_(set), bind_(es == 2 ? nullptr : bind),
          count_(es == 2 ? 1u : (es >= 3 ? 4u : kCount)) {
        if (!get_ || !set_) {
            return;  // without both, nothing here can be done safely
        }
        for (unsigned i = 0; i < count_; ++i) {
            get_(kNames[i], &saved_[i]);
            if (saved_[i] != kNeutral[i]) {
                set_(kNames[i], kNeutral[i]);
            }
        }
        if (bind_) {
            get_(kPixelUnpackBufferBinding, &buffer_);
            if (buffer_ != 0) {
                bind_(kPixelUnpackBuffer, 0);
            }
        }
        active_ = true;
    }

    ~PixelStoreGuard() {
        if (!active_) {
            return;
        }
        for (unsigned i = 0; i < count_; ++i) {
            if (saved_[i] != kNeutral[i]) {
                set_(kNames[i], saved_[i]);
            }
        }
        if (bind_ && buffer_ != 0) {
            bind_(kPixelUnpackBuffer, static_cast<GLuint>(buffer_));
        }
    }

    PixelStoreGuard(const PixelStoreGuard&) = delete;
    PixelStoreGuard& operator=(const PixelStoreGuard&) = delete;

    // False when the entry points were not there: the caller must not upload.
    bool ok() const { return active_; }

private:
    // In the order they became available: the alignment alone on ES 2, the row
    // length and the skips from ES 3, the byte-order flags on desktop GL only.
    static constexpr unsigned kCount = 6;
    static constexpr GLenum kNames[kCount] = {
        0x0CF5,  // GL_UNPACK_ALIGNMENT
        0x0CF2,  // GL_UNPACK_ROW_LENGTH
        0x0CF3,  // GL_UNPACK_SKIP_ROWS
        0x0CF4,  // GL_UNPACK_SKIP_PIXELS
        0x0CF0,  // GL_UNPACK_SWAP_BYTES
        0x0CF1,  // GL_UNPACK_LSB_FIRST
    };
    static constexpr GLint kNeutral[kCount] = {4, 0, 0, 0, 0, 0};
    static constexpr GLenum kPixelUnpackBuffer = 0x88EC;
    static constexpr GLenum kPixelUnpackBufferBinding = 0x88EF;

    PFN_glGetIntegerv get_ = nullptr;
    PFN_glPixelStorei set_ = nullptr;
    PFN_glBindBuffer bind_ = nullptr;
    unsigned count_ = kCount;
    GLint saved_[kCount] = {0};
    GLint buffer_ = 0;
    bool active_ = false;
};

// The application's sRGB-write state, switched off for the overlay's draw and
// put back.
//
// With GL_FRAMEBUFFER_SRGB enabled and an sRGB-capable drawable, the hardware
// applies linear->sRGB to whatever the fragment shader writes. ImGui's colours
// are already sRGB, so they get encoded twice: measured on the Vulkan side,
// where the same thing happens through the swapchain's format, the panel's own
// 79,84,92 came back 151,155,162 -- a light grey where the theme asks for dark
// slate. White and fully saturated colours are fixed points, which is why this
// hides: the text looks right and the box does not.
//
// The Vulkan half answers this in the shader, because there the encoding is the
// attachment's own property and cannot be switched off. Here it is a switch, so
// it is switched -- and switched back, because it is the game's (rule 12).
// ImGui's own GL backend does not touch it (0 occurrences in
// imgui_impl_opengl3.cpp); MangoHud's fork of that backend saves, clears and
// restores it in exactly this way, which is where the question came from.
//
// Desktop GL only. On OpenGL ES the enum is not core -- it arrives with
// EXT_sRGB_write_control -- and asking for one that does not exist is a
// GL_INVALID_ENUM in the game's error queue, which is the fault PixelStoreGuard
// exists to avoid.
class SrgbWriteGuard {
public:
    // `es` is 0 for desktop GL, or the OpenGL ES major version; `major` is the
    // desktop GL major version. GL_FRAMEBUFFER_SRGB is core from 3.0
    // (ARB_framebuffer_sRGB before it), and asking a 2.1 context about it is
    // GL_INVALID_ENUM in the game's queue, once per frame -- the fault
    // PixelStoreGuard's comment says this class exists to avoid.
    SrgbWriteGuard(PFN_glIsEnabled is_enabled, PFN_glEnable enable, PFN_glDisable disable, int es,
                   int major)
        : enable_(enable) {
        if (es != 0 || major < 3 || !is_enabled || !enable || !disable) {
            return;
        }
        was_enabled_ = is_enabled(kFramebufferSrgb) != 0;
        if (was_enabled_) {
            disable(kFramebufferSrgb);
        }
    }

    ~SrgbWriteGuard() {
        if (was_enabled_ && enable_) {
            enable_(kFramebufferSrgb);
        }
    }

    SrgbWriteGuard(const SrgbWriteGuard&) = delete;
    SrgbWriteGuard& operator=(const SrgbWriteGuard&) = delete;

private:
    static constexpr GLenum kFramebufferSrgb = 0x8DB9;

    PFN_glEnable enable_ = nullptr;
    bool was_enabled_ = false;
};

class GlAvatarProvider : public vocem::AvatarProvider {
public:
    bool resolve(int es_version, int gl_major) {
        es_ = es_version;
        gl_major_ = gl_major;
        if (resolved_) {
            return true;
        }
        gen_textures_ = gl_symbol<PFN_glGenTextures>("glGenTextures");
        bind_texture_ = gl_symbol<PFN_glBindTexture>("glBindTexture");
        tex_image_ = gl_symbol<PFN_glTexImage2D>("glTexImage2D");
        tex_parameter_ = gl_symbol<PFN_glTexParameteri>("glTexParameteri");
        get_integer_ = gl_symbol<PFN_glGetIntegerv>("glGetIntegerv");
        // The two that make an upload safe in somebody else's renderer. Core
        // since GL 1.0 and 1.5; a context without them gets no avatars rather
        // than an upload that reads wherever the game's state points.
        pixel_store_ = gl_symbol<PFN_glPixelStorei>("glPixelStorei");
        bind_buffer_ = gl_symbol<PFN_glBindBuffer>("glBindBuffer");
        // Core since GL 1.0, and not part of `resolved_`: without them the
        // overlay draws exactly as it did before SrgbWriteGuard existed, which
        // is a wrong colour rather than a missing avatar.
        is_enabled_ = gl_symbol<PFN_glIsEnabled>("glIsEnabled");
        enable_ = gl_symbol<PFN_glEnable>("glEnable");
        disable_ = gl_symbol<PFN_glDisable>("glDisable");
        resolved_ = gen_textures_ && bind_texture_ && tex_image_ && tex_parameter_ &&
                    get_integer_ && pixel_store_;
        if (!resolved_) {
            VOCEM_GLOG("no avatars: glPixelStorei or a texture entry point is missing");
        }
        return resolved_;
    }

    // For the font atlas, which the ImGui backend uploads through the same
    // client-pointer path and with the same exposure.
    PixelStoreGuard pixel_store_guard() const {
        return PixelStoreGuard(get_integer_, pixel_store_, bind_buffer_, es_);
    }

    // For the overlay's own draw: the sRGB write state is the game's, and the
    // overlay's colours are already encoded.
    SrgbWriteGuard srgb_write_guard() const {
        return SrgbWriteGuard(is_enabled_, enable_, disable_, es_, gl_major_);
    }

    // The two things the framebuffer retarget in draw() needs, ES-aware: the
    // target to bind and the binding to read back. GL_FRAMEBUFFER binds BOTH
    // the draw and the read framebuffer, and GL_FRAMEBUFFER_BINDING reads the
    // draw one, so binding GL_FRAMEBUFFER to 0 and putting "the binding" back
    // restored the draw side and left the read side pointing at whatever the
    // overlay drew into -- a game that presents by blitting from its own
    // read framebuffer had that binding clobbered every frame (rule 12).
    // Desktop GL and ES 3 have the two targets; ES 2 has only the one, where
    // there is nothing else to clobber.
    GLenum draw_framebuffer_target() const {
        return es_ == 2 ? GL_FRAMEBUFFER : 0x8CA9;  // GL_DRAW_FRAMEBUFFER
    }

    // Called once per drawn frame, before the panel is built: the budget below is
    // per frame and something has to say when a frame starts.
    void begin_frame() { taken_on_this_frame_ = 0; }

    ImTextureID texture(uint64_t user_id, const char* avatar_hash) override {
        if (!resolved_) {
            return 0;
        }
        // A POD key, so the steady-state lookup below allocates nothing: the
        // formatted std::string this used to be was one malloc and free per
        // visible face per frame (vocem/avatar_key.h says why).
        const vocem::AvatarKey key = vocem::AvatarKey::make(user_id, avatar_hash);

        auto it = textures_.find(key);
        if (it != textures_.end()) {
            return it->second;
        }

        // Waiting for a file that has not appeared: nothing to do until its next
        // look is due, and stat() on every frame for every face is not free.
        auto waiting = waiting_.find(key);
        if (waiting != waiting_.end() && !waiting->second.due(vocem::avatar_now_seconds())) {
            return 0;
        }

        // At most one picture taken on per frame -- the rule the Vulkan cache has
        // stated in its own header since it was written, and which this side never
        // adopted. Measured before it was adopted here: with six faces landing on
        // disk together, one frame read and uploaded all six
        // (tests/gl_avatar_quiet.cpp). A channel filling up is exactly when that
        // happens, and a few frames of grey discs is invisible where a hitch is
        // not. The check sits above the stat as well as above the read, so a
        // deferred face costs this frame nothing at all.
        if (taken_on_this_frame_ >= kFacesPerFrame) {
            return 0;
        }

        // Uploading here is safe in a way the Vulkan path is not: a GL texture
        // upload does not need a queue submit or a fence, so it can happen inline
        // as long as we restore the binding the application had.
        char path[768];
        vocem::avatar_rgba_path(path, sizeof(path), user_id, avatar_hash);

        // Not there yet is not the same as broken. Somebody who joins is drawn on
        // the next frame, while the daemon is still downloading their picture, and
        // writing that off as a permanent failure is what left them a grey disc
        // for the rest of the session. Looked at again twice a second rather than
        // on every frame: this runs inside somebody's game.
        const double now = vocem::avatar_now_seconds();
        if (!vocem::avatar_file_exists(path)) {
            // `waiting` is the lookup from above: the map has not changed since.
            // The policy -- look again in half a second, give up after thirty --
            // is vocem/avatar_file.h's, one spelling with the Vulkan cache.
            if (waiting == waiting_.end()) {
                waiting = waiting_.emplace(key, vocem::AvatarWait::start(now)).first;
            }
            if (!waiting->second.missed(now)) {
                VOCEM_GLOG("gave up waiting for %s", path);
                waiting_.erase(waiting);
                textures_.emplace(key, static_cast<ImTextureID>(0));
            }
            return 0;
        }
        waiting_.erase(key);

        // Noted before the work, not after: if the upload is what kills the
        // process, the journal's last line has to name it (entry 46's crash
        // was exactly an avatar upload, and nothing anywhere said so).
        {
            char note[840];
            std::snprintf(note, sizeof(note), "uploading avatar %s", path);
            vocem::journal_note(note);
        }

        // Not a decode: a size check and a copy. Anything that is not exactly
        // the format's one size is refused, and asking again cannot fix it --
        // the daemon renames into place, so what exists is complete.
        static unsigned char pixels[vocem::kAvatarRgbaBytes];
        if (!vocem::avatar_rgba_load(path, pixels)) {
            // The read happened, so it counts against the frame's budget even
            // though nothing came of it.
            ++taken_on_this_frame_;
            textures_.emplace(key, static_cast<ImTextureID>(0));
            return 0;
        }

        // The application's unpack state decides what this call reads out of
        // `pixels`; neutralised for the upload and put back afterwards.
        const PixelStoreGuard unpack = pixel_store_guard();
        if (!unpack.ok()) {
            ++taken_on_this_frame_;
            textures_.emplace(key, static_cast<ImTextureID>(0));
            return 0;
        }

        GLint previous = 0;
        get_integer_(GL_TEXTURE_BINDING_2D, &previous);

        GLuint name = 0;
        gen_textures_(1, &name);
        bind_texture_(GL_TEXTURE_2D, name);
        tex_parameter_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        tex_parameter_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        tex_image_(GL_TEXTURE_2D, 0, GL_RGBA, vocem::kAvatarPixels, vocem::kAvatarPixels, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        bind_texture_(GL_TEXTURE_2D, static_cast<GLuint>(previous));

        ++taken_on_this_frame_;
        VOCEM_GLOG("uploaded avatar %s (%ux%u)", path, vocem::kAvatarPixels, vocem::kAvatarPixels);

        const ImTextureID id = static_cast<ImTextureID>(name);
        textures_.emplace(key, id);
        return id;
    }

    PFN_glGetIntegerv get_integer() const { return get_integer_; }

    // Every texture name we hold belongs to a GL context. Forget them.
    //
    // No `glDeleteTextures` even when the context is still alive: the two callers
    // are a context being destroyed, where the names are already meaningless, and
    // the user switching the overlay off, where the tidiest thing is not to touch
    // the application's GL state on the way out. The pictures cost a few hundred
    // kilobytes and are re-uploaded from the cache on the first frame after the
    // overlay comes back, which is the same path that put them there to begin with.
    void forget() {
        textures_.clear();
        waiting_.clear();
        resolved_ = false;
        gen_textures_ = nullptr;
        bind_texture_ = nullptr;
        tex_image_ = nullptr;
        tex_parameter_ = nullptr;
        get_integer_ = nullptr;
        pixel_store_ = nullptr;
        bind_buffer_ = nullptr;
        is_enabled_ = nullptr;
        enable_ = nullptr;
        disable_ = nullptr;
    }

private:
    // One picture taken on per frame, the Vulkan cache's rule (see texture()).
    static constexpr int kFacesPerFrame = 1;
    int taken_on_this_frame_ = 0;
    bool resolved_ = false;
    PFN_glGenTextures gen_textures_ = nullptr;
    PFN_glBindTexture bind_texture_ = nullptr;
    PFN_glTexImage2D tex_image_ = nullptr;
    PFN_glTexParameteri tex_parameter_ = nullptr;
    PFN_glGetIntegerv get_integer_ = nullptr;
    PFN_glPixelStorei pixel_store_ = nullptr;
    PFN_glBindBuffer bind_buffer_ = nullptr;
    PFN_glIsEnabled is_enabled_ = nullptr;
    PFN_glEnable enable_ = nullptr;
    PFN_glDisable disable_ = nullptr;
    int es_ = 0;  // 0 desktop GL, otherwise the OpenGL ES major version
    int gl_major_ = 0;  // the desktop GL major version, 0 when unknown or ES
    std::unordered_map<vocem::AvatarKey, ImTextureID, vocem::AvatarKeyHash> textures_;

    // Faces the daemon has been asked for and has not finished fetching.
    std::unordered_map<vocem::AvatarKey, vocem::AvatarWait, vocem::AvatarKeyHash> waiting_;
};

// ---------------------------------------------------------------------------
// Overlay state, one per process
// ---------------------------------------------------------------------------

// The context the backend's GL objects live in, and the display it belongs
// to. Read by the two teardown hooks below before they touch anything, which
// is why these are plain globals rather than members: a process that never
// built a backend -- every browser and compositor of the session, which get
// our eglDestroyContext hook through the dispatcher door -- must be able to
// answer "not mine" without constructing the overlay, let alone making a
// dying context current on the calling thread. Written under g_gl_lock;
// read with the atomic builtins so the hooks can look before they lock.
void* g_owner_context = nullptr;
void* g_owner_display = nullptr;
int g_owner_egl = 0;

class GlOverlay {
public:
    // Asks the windowing system how large the drawable is. Passed in rather
    // than asked up front: on GLX each of the two queries is an X server round
    // trip -- measured on this machine, 33 us the pair, every frame, in every
    // GL process of the session -- so the question is only asked once a frame
    // has decided it will actually draw. The handle is an XID or an
    // EGLSurface, opaque here either way.
    using SizeQuery = void (*)(void* display, void* handle, uint32_t& width, uint32_t& height);

    // Draws into the current framebuffer. Called from the swap hooks, before the
    // real swap: whatever we add lands in the frame about to be shown.
    // `egl` says which window system the present came through: the owner
    // context is remembered by the same API's "get current" call.
    void draw(SizeQuery query_size, void* display, void* handle, bool egl) {
        if (overlay_disabled()) {
            return;
        }

        // Before the first thing that derives a path. Inside a Flatpak game the
        // segment, config.ini and the avatar cache are all on the far side of
        // the sandbox, and this is what points the three lookups at the copies
        // the daemon puts where they can be reached (vocem/flatpak.h).
        session_.enter_flatpak_bridge_once();

        // One stat() every couple of seconds, not per frame.
        const vocem::Config& config = config_.current();

        // Written down before any of the decisions below, so that an application
        // appears in the window's list whether or not the overlay is allowed to
        // draw in it, and whether or not there is anything to draw right now --
        // otherwise nothing is listed until somebody happens to be in a voice
        // channel, which is exactly when nobody is reading the list.
        vocem::record_application("opengl");

        // Whether the overlay belongs in this frame, asked **every** frame.
        //
        // This used to be decided once and never again, and the page said so: a
        // change took effect the next time the game started. That is not what the
        // switch looks like it does, and it is not what the tray icon looks like it
        // does either -- Discord's overlay goes off and comes back in the running
        // game, and so should this one. The cost of asking is two string
        // comparisons against the configuration the process already re-reads every
        // couple of seconds; the lists themselves are only walked when one of them
        // has actually changed, because walking them is the expensive half and it
        // does not need doing at 144 frames a second.
        //
        // Turning it off releases the GL state rather than merely skipping the
        // drawing -- see `release()`. Turning it back on costs one frame, in which
        // the backend is built again from nothing.
        // The decision, its evidence and the word to the daemon across the
        // bridge are one spelling with the Vulkan layer's now
        // (vocem/overlay_session.h).
        // decide() carries the master switch itself now: spelling it here as
        // well short-circuited past the sentence that tells the daemon whether
        // this sandbox is drawing (vocem/overlay_session.h).
        const bool want = session_.decide(config);
        if (want) {
            // The session's journal (vocem/journal.h): opened at the first
            // frame the overlay draws in this process, closed into history on
            // a clean exit by the destructor below -- and left behind, still
            // `.running`, by a crash, which is the detection.
            session_.journal_begin_once();
        }
        if (drawing_ >= 0 && want != (drawing_ == 1)) {
            VOCEM_GLOG("%s in '%s'", want ? "switched on" : "switched off",
                       vocem::process_name().c_str());
            vocem::journal_note(want ? "switched on" : "switched off");
            if (!want) {
                // A present hook is the one place where the application's context
                // is guaranteed current, so this is the good moment to hand back
                // everything that lives in it.
                //
                // Said in the same words the daemon-stopped case below uses, and
                // in the same words the Vulkan layer uses: the two paths release
                // for the same two reasons now, and a reader chasing one of them
                // should not have to know which half of the overlay wrote the
                // line. What it names is what actually goes -- the backend and
                // the atlas -- rather than "everything", which the swapchain's
                // and the context's own objects are not.
                VOCEM_GLOG("switched off: releasing the backend and the font atlas");
                release(true);
                // The atlas does not live in the context: it belongs to the
                // fonts module and survives release() on purpose, so that a game
                // cycling its context pays nothing. Being switched off is the
                // other case, and there 80 MB of rasterised glyphs in somebody
                // else's process is exactly what release() exists to give back.
                vocem::fonts_release();
            }
        }
        drawing_ = want ? 1 : 0;
        if (!want) {
            return;
        }

        // The Debug section's frame and draw counters, written every few
        // seconds. The write is two file syscalls and a rename -- inside the
        // budget this side already spends on the avatar path (entry 52) and
        // throttled far below it; the counting itself is two integers.
        session_.frame_seen();

        vocem::Snapshot* snapshot = poll_state();
        if (!snapshot) {
            // The daemon stopped -- the tray's Quit, or `systemctl --user stop`.
            // Not drawing is not enough: this process is holding a backend, an
            // atlas and a texture per face on the daemon's behalf, and measured
            // before this existed it went on holding all of it -- 139.7 MB
            // against 22 MB for the same process without the overlay -- for the
            // rest of its life. The same handing back as the switch being turned
            // off, because from the guest's side it is the same situation; a
            // present hook is the one place the application's context is
            // guaranteed current, which is what makes it safe here.
            if (state_poll_.daemon_left()) {
                VOCEM_GLOG("the daemon stopped: releasing the backend and the font atlas");
                vocem::journal_note("daemon stopped: released");
                release(true);
                vocem::fonts_release();
            }
            return;
        }
        // One clock for the whole frame: the animation step, the panel's motion
        // and the toast's age must agree about what time it is.
        const double now = vocem::monotonic_seconds();
        // Either feature is a reason to spend the frame -- the guard used to ask
        // only about the voice channel, which made a toast outside one
        // unreachable (vocem/panel.h says why there is one spelling of this).
        const bool panel_frame = vocem::panel_wanted(*snapshot, config);
        const bool toast_frame = vocem::notification_wanted(*snapshot, config, now);
        if (!panel_frame && !toast_frame) {
            session_.note_forget();
            return;
        }
        // The message's words, fetched only now: this process has decided it
        // will draw this toast, which is the one condition under which the
        // note segment is opened at all (vocem/note.h). Three syscalls per
        // message, none per frame -- the snapshot is this process's own copy,
        // so writing the text into it touches nothing anybody else can see,
        // and forget() above wipes it the moment the toast is over.
        if (toast_frame) {
            // The snapshot is this process's own copy -- poll_state() reads
            // the segment into it -- so filling the body here reaches nobody
            // else. A message that arrived without its words is said once, in
            // the session (the one failure this path has that looks like
            // success).
            std::snprintf(snapshot->notification.body, sizeof(snapshot->notification.body),
                          "%s", session_.note_words(snapshot->notification.serial));
        } else {
            session_.note_forget();
        }
        if (!ensure_backend(egl)) {
            return;
        }
        // Only now is the drawable's size worth two X round trips: every path
        // above this line returns without drawing, and used to pay for the
        // answer anyway.
        uint32_t width = 0;
        uint32_t height = 0;
        query_size(display, handle, width, height);
        if (width == 0 || height == 0) {
            // Some drivers report nothing useful for the drawable; fall back to
            // the viewport the application last set.
            GLint viewport[4] = {0, 0, 0, 0};
            if (PFN_glGetIntegerv get_integer = avatars_.get_integer()) {
                get_integer(GL_VIEWPORT, viewport);
                width = static_cast<uint32_t>(viewport[2]);
                height = static_cast<uint32_t>(viewport[3]);
            }
            if (width == 0 || height == 0) {
                VOCEM_GLOG("no drawable size: nothing drawn this frame");
                return;
            }
            VOCEM_GLOG("size came from the viewport: %ux%u", width, height);
        }

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        // The measured time since the last presented frame, one spelling with
        // the Vulkan side (vocem/overlay_session.h); release() resets it, so the
        // frame that brings the overlay back does not measure the whole absence
        // as one animation step. `now` is the frame's one clock, taken above.
        io.DeltaTime = session_.delta_time(now);

        // Rasterise the atlas at the size this drawable needs. Unlike the Vulkan
        // side there is nothing to defer to: replacing a GL texture is immediate,
        // and the backend saves and restores GL_TEXTURE_BINDING_2D around it
        // (rule 12), so the application's state is untouched.
        // Recreated explicitly rather than left to NewFrame: the backend only
        // builds the font texture as part of creating its device objects, and
        // those already exist by now.
        // Which colour emoji this frame's text needs: noted before the atlas
        // question, so a name with a new emoji triggers the same rebuild a
        // size change does.
        // Everything below touches GL through the ImGui backend, and the backend
        // reads and writes the application's pixel-store state: CreateFontsTexture
        // sets GL_UNPACK_ROW_LENGTH to 0 and never restores it, and it is reached
        // both from the rebuild below and lazily from NewFrame the first time. One
        // guard over the whole of it, so the game gets its state back whichever
        // path ran. Measured against the library before it: a game that left the
        // row length at 2048 found it at 0 after the overlay's first frame.
        const PixelStoreGuard unpack = avatars_.pixel_store_guard();

        vocem::fonts_note_emoji_in(*snapshot);
        // Sized by the display, not by the window: a window is where the overlay
        // is drawn, not how large it should be, and sizing from the drawable
        // made every resize rubber-band the whole panel -- text, pictures,
        // spacing, all of it, since every distance is a multiple of ui_scale().
        // The daemon publishes the display's mode height; sizing_height() says
        // when the drawable wins instead (zero display, or a supersampled
        // drawable taller than the display and headed for a downscale).
        const float wanted_pixels = vocem::font_pixel_size(
            vocem::sizing_height(snapshot->display_height, height), config.scale,
            config.font_size);
        if (vocem::ensure_fonts(wanted_pixels, config.font_size, config.font_path.c_str(),
                                config.font_path_strong.c_str())) {
            // The expensive thing this process does, said out loud: 133 ms of
            // rasterising, and nothing said so until a context cycle was found
            // paying it every time. tests/gl_context_cycle.cpp counts these
            // lines, which is why it can assert a count instead of a clock.
            VOCEM_GLOG("font atlas built at %.0f px", static_cast<double>(wanted_pixels));
            vocem::configure_style(config);
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }

        // Why there are no colour emoji, and why the text is not in the font
        // the settings name: said once per change, the same way on both paths.
        session_.say_font_statuses();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        // The avatar budget is per frame; this is where a frame starts.
        avatars_.begin_frame();
        if (panel_frame) {
            vocem::build_panel(*snapshot, config, width, height, &avatars_, now);
        }
        vocem::build_notification(*snapshot, config, width, height, &avatars_, now);
        ImGui::Render();

        // Into the framebuffer that is about to be presented, which is not
        // necessarily the one the application left bound.
        //
        // ImGui's OpenGL backend draws into whatever is bound and saves a dozen
        // pieces of state around it -- the framebuffer binding is not one of them.
        // An application that swaps with its own framebuffer object still bound
        // therefore gets the overlay drawn into that object, which is then
        // discarded, and the overlay is invisible for a reason nothing reports.
        // Restored immediately, like every other piece of state this touches
        // (rule 12).
        // The DRAW target alone where the API has one (GlAvatarProvider says
        // what binding GL_FRAMEBUFFER to 0 did to the READ side).
        GLint previous_framebuffer = 0;
        const GLenum draw_target = avatars_.draw_framebuffer_target();
        const bool retarget = bind_framebuffer_ && avatars_.get_integer();
        if (retarget) {
            avatars_.get_integer()(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
            if (previous_framebuffer != 0) {
                bind_framebuffer_(draw_target, 0);
            }
        }

        {
            // After the framebuffer is chosen and around the draw alone: the
            // switch is per-draw state, and the uploads above are not affected
            // by it. ImGui's own backend never looks at it, so the overlay's
            // already-sRGB colours would be encoded a second time in every game
            // that leaves it on (SrgbWriteGuard says what that measures).
            const SrgbWriteGuard srgb = avatars_.srgb_write_guard();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        if (retarget && previous_framebuffer != 0) {
            bind_framebuffer_(draw_target, static_cast<GLuint>(previous_framebuffer));
        }

        capture_if_asked(width, height);
        session_.frame_drawn();
    }

    // Give back everything that belongs to a GL context, and be ready to build it
    // again on the next frame that wants one.
    //
    // Two callers, one operation, which is why it is one function:
    //
    //   * **The application destroyed the context** our objects live in. Every
    //     texture name and every object inside ImGui's backend refers to something
    //     that no longer exists, and using one after that is undefined rather than
    //     merely wrong.
    //   * **The user switched the overlay off.** Not drawing is not enough: this
    //     code is a guest in somebody else's process, and a guest that has been
    //     asked to leave should not still be holding a shader program, a vertex
    //     buffer and a texture per person in the channel.
    //
    // `gl_current` says whether a GL context is current *right now* and safe to
    // call into. Inside a present hook it is; inside `glXDestroyContext` it is only
    // if the caller made the dying context current for us, and it says so.
    void release(bool gl_current) {
        if (backend_ready_ && gl_current) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        avatars_.forget();
        backend_ready_ = false;
        forget_owner();
        session_.reset_clock();
        // A backend that failed against one context deserves a fresh attempt
        // against the next: the failure was about that context, not about us.
        failed_ = false;
        bind_framebuffer_ = nullptr;
        capture_warmup_frames_ = 0;

        // Without a current context ImGui's backend cannot be shut down, because
        // shutting it down means deleting GL objects. Its own small heap block is
        // then leaked once per context destruction -- a hundred-odd bytes, not
        // once per frame -- and the GL objects it named are gone with the context
        // regardless. Destroying the ImGui context here is what makes the next
        // `Init` start from nothing rather than from a half-torn-down backend.
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    }

    // Whether the backend lives in `context` on `display` -- or, with a null
    // context (eglTerminate), anywhere on `display`.
    static bool owns(void* display, void* context, bool egl) {
        void* owner = __atomic_load_n(&g_owner_context, __ATOMIC_ACQUIRE);
        if (!owner || __atomic_load_n(&g_owner_egl, __ATOMIC_ACQUIRE) != (egl ? 1 : 0)) {
            return false;
        }
        if (!context) {
            return __atomic_load_n(&g_owner_display, __ATOMIC_ACQUIRE) == display;
        }
        return owner == context;
    }

private:
    // A development aid: with VOCEM_CAPTURE_FRAME set to a path, the first frame
    // that carries the overlay is read back out of the game's own framebuffer and
    // written there as a PPM.
    //
    // This exists because there is no other honest way to check what the overlay
    // actually looks like. Screenshot tools on Wayland capture the wrong window or
    // need a portal prompt, and "it should render correctly" is not a claim worth
    // making about code that draws inside somebody else's frame. Reading the
    // framebuffer after the draw shows exactly what the game shows.
    //
    // Once per process, and nothing at all when the variable is unset.
    void capture_if_asked(uint32_t width, uint32_t height) {
        static const char* target = std::getenv("VOCEM_CAPTURE_FRAME");
        if (!target || !target[0] || captured_ || width == 0 || height == 0) {
            return;
        }
        // Not the first frame: ImGui hides a window for a frame while it works out
        // its own size, so a capture taken immediately shows the game with no
        // overlay on it and looks exactly like a failure.
        if (++capture_warmup_frames_ < 30) {
            return;
        }
        captured_ = true;

        using PFN_glReadPixels = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
        static PFN_glReadPixels read_pixels = nullptr;
        if (!read_pixels) {
            read_pixels = gl_symbol<PFN_glReadPixels>("glReadPixels");
        }
        if (!read_pixels) {
            return;
        }

        // RGBA rather than RGB: OpenGL ES only guarantees RGBA/UNSIGNED_BYTE for a
        // read from the default framebuffer, and asking it for RGB produced an
        // image that looked like a fault in the overlay rather than in the capture.
        constexpr GLenum GL_RGBA_FORMAT = 0x1908;
        const size_t count = static_cast<size_t>(width) * height * 4;
        unsigned char* pixels = static_cast<unsigned char*>(std::malloc(count));
        if (!pixels) {
            return;
        }
        read_pixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                    GL_RGBA_FORMAT, GL_UNSIGNED_BYTE, pixels);

        if (std::FILE* file = std::fopen(target, "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", width, height);
            // GL's origin is bottom-left and an image file's is top-left; the alpha
            // is dropped a pixel at a time on the way out.
            for (uint32_t row = height; row-- > 0;) {
                const unsigned char* line = pixels + static_cast<size_t>(row) * width * 4;
                for (uint32_t column = 0; column < width; ++column) {
                    std::fwrite(line + static_cast<size_t>(column) * 4, 1, 3, file);
                }
            }
            std::fclose(file);
            VOCEM_GLOG("captured a frame to %s", target);
        }
        std::free(pixels);
    }

    // One spelling with the Vulkan layer's, in vocem/state_poll.h -- the loop
    // had drifted apart once already (the layer said why a read failed, this
    // path did not).
    vocem::Snapshot* poll_state() { return state_poll_.poll(); }

    bool ensure_backend(bool egl) {
        if (backend_ready_) {
            return true;
        }
        if (failed_) {
            return false;
        }

        if (!ImGui::GetCurrentContext()) {
            IMGUI_CHECKVERSION();
            // With the fonts module's atlas, not one of the context's own: the
            // atlas has to outlive the context, because this is a context that
            // dies and comes back (vocem/fonts.h says what the alternatives
            // cost, both in a crash and in 133 ms per context).
            ImGui::CreateContext(vocem::fonts_atlas());
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.BackendPlatformName = "vocem-gl";
            vocem::configure_style(config_.current());
        }

        // The shader source has to match the context, and the backend cannot work
        // that out on its own: it detects an ES context at runtime for its feature
        // flags, but the GLSL version it compiles with is chosen at build time, and
        // for a file built against desktop GL that is "#version 130". Handed to an
        // OpenGL ES context the shader does not compile, the program does not link,
        // and every draw call after it is rejected with GL_INVALID_OPERATION --
        // silently, since the backend's own checks are assertions and this is built
        // with them off.
        //
        // Measured inside a Qt application on Wayland, which runs on OpenGL ES 3.2
        // through EGL: the overlay was building nine hundred vertices a frame and
        // not one of them reached the screen. Native games that go through GLX get
        // a desktop context and were never affected, which is why this went
        // unnoticed.
        const char* glsl_version = nullptr;
        int es_version = 0;  // 0 desktop GL, otherwise the OpenGL ES major version
        int gl_major = 0;    // the desktop GL major version, for what a context can be asked
        const auto read_version = [&]() -> bool {
            PFN_glGetString get_string = gl_symbol<PFN_glGetString>("glGetString");
            if (!get_string) {
                return false;
            }
            const char* version = reinterpret_cast<const char*>(get_string(GL_VERSION));
            if (!version) {
                return false;
            }
            if (std::strncmp(version, "OpenGL ES 2", 11) == 0) {
                glsl_version = "#version 100";
                es_version = 2;
            } else if (std::strncmp(version, "OpenGL ES", 9) == 0) {
                glsl_version = "#version 300 es";
                es_version = 3;
            } else if (version[0] >= '1' && version[0] <= '9') {
                gl_major = version[0] - '0';  // "4.6.0 NVIDIA 580.82.07"
            }
            VOCEM_GLOG("context: %s", version);
            return true;
        };
        const bool version_known = read_version();

        if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
            VOCEM_GLOG("ImGui OpenGL3 backend failed to initialise");
            failed_ = true;
            return false;
        }
        // Asked again after Init where the first ask found nothing: in a game
        // that never resolved a dispatcher of its own (this project's own GLX
        // probes are that game), the third road has nothing to forward to
        // until ImGui's loader resolves glXGetProcAddressARB through the
        // dlsym door and the shim remembers it -- which Init has just done.
        // The GLSL version is settled by then, but the GL major, which gates
        // what the context may be asked (SrgbWriteGuard), still needs the
        // answer.
        if (!version_known) {
            read_version();
        }
        // The backend's GL objects -- shader, buffers and the font texture --
        // built HERE, not left to its NewFrame. draw() replaces the font
        // texture explicitly once ensure_fonts() has rasterised the atlas at
        // the size this output needs, and it did so on the first frame before
        // NewFrame had built the device objects; NewFrame then built them,
        // font texture included, over the one draw() had just made. One whole
        // atlas -- 16 to 64 MB of RGBA at the sizes fonts.cpp measures --
        // orphaned per backend build: per overlay toggle, per context
        // recreation, for the life of the game's context. The comment above
        // the replace used to claim the objects "already exist by now"; this is
        // what makes it true. tests/gl_draw_local.cpp counts the textures.
        avatars_.resolve(es_version, gl_major);
        {
            // Under the pixel-store guard, exactly as draw() keeps the backend's
            // font upload: CreateFontsTexture zeroes GL_UNPACK_ROW_LENGTH and
            // never restores it, and this call is what makes it now. The first
            // version of this block ran it bare, and gl_unpack_state caught the
            // game's row length at 0 the same minute.
            const PixelStoreGuard unpack = avatars_.pixel_store_guard();
            if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
                VOCEM_GLOG("ImGui OpenGL3 backend could not create its GL objects");
                ImGui_ImplOpenGL3_Shutdown();
                failed_ = true;
                return false;
            }
        }
        // Resolved once, beside the rest: a context without it is older than
        // framebuffer objects, in which case there is nothing to retarget.
        bind_framebuffer_ = gl_symbol<PFN_glBindFramebuffer>("glBindFramebuffer");
        // Whose context this backend now lives in, so that the teardown hooks
        // can tell that context's death from any other's (see them below).
        remember_owner(egl);
        backend_ready_ = true;
        VOCEM_GLOG("OpenGL backend ready");
        vocem::journal_note("OpenGL backend ready");
        return true;
    }

private:
    // Asked once, when the backend comes up, of the API the present arrived
    // through: which context is current right now is the one the backend's
    // objects were just created in.
    void remember_owner(bool egl) {
        using PFN_current = void* (*)();
        void* context = nullptr;
        void* display = nullptr;
        if (egl) {
            if (auto current = gl_symbol<PFN_current>("eglGetCurrentContext")) {
                context = current();
            }
            if (auto current = gl_symbol<PFN_current>("eglGetCurrentDisplay")) {
                display = current();
            }
        } else {
            if (auto current = gl_symbol<PFN_current>("glXGetCurrentContext")) {
                context = current();
            }
            if (auto current = gl_symbol<PFN_current>("glXGetCurrentDisplay")) {
                display = current();
            }
        }
        __atomic_store_n(&g_owner_display, display, __ATOMIC_RELEASE);
        __atomic_store_n(&g_owner_egl, egl ? 1 : 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_owner_context, context, __ATOMIC_RELEASE);
        VOCEM_GLOG("backend belongs to %s context %p", egl ? "EGL" : "GLX", context);
    }

    void forget_owner() {
        __atomic_store_n(&g_owner_context, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
        __atomic_store_n(&g_owner_display, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
    }

    vocem::StatePoll state_poll_{&gl_poll_log};
    GlAvatarProvider avatars_;
    vocem::LiveConfig config_;
    // The bookkeeping both injected paths keep alike -- the bridge, the
    // decision, the journal, the frame counters, the toast's words, the frame
    // clock (vocem/overlay_session.h). This side calls all of it inline, in the
    // present hook, which is its rule (entry 52's budget); the layer spreads
    // the same calls over its two phases.
    vocem::OverlaySession session_{"opengl", "vocem/gl"};
    // What the last frame concluded about whether the overlay belongs here, so the
    // moment it changes can be noticed. -1 until the first frame has asked.
    int drawing_ = -1;
    bool backend_ready_ = false;
    bool captured_ = false;
    int capture_warmup_frames_ = 0;
    bool failed_ = false;
    PFN_glBindFramebuffer bind_framebuffer_ = nullptr;
};

GlOverlay& overlay() {
    // Never destroyed. A function-local object would be destroyed at exit(),
    // which a game may call from any thread while its render thread is still
    // inside draw() under the lock: the maps and the state poll would be torn
    // down under it. Nothing in here needs a destructor to run -- the journal
    // is closed by the ELF destructor at the end of this file, and every GL
    // object dies with the game's context -- so the instance is simply left.
    static GlOverlay* instance = new GlOverlay;
    return *instance;
}

// ---------------------------------------------------------------------------
// GLX
// ---------------------------------------------------------------------------

// Only the query is needed here: the swap and dispatcher signatures that used
// to sit beside it were the deleted hook table's (entry 45), and a leftover
// signature is what makes growing that table back a two-line change.
using PFN_glXQueryDrawable = void (*)(void*, unsigned long, int, unsigned int*);

constexpr int kGlxWidth = 0x801D;
constexpr int kGlxHeight = 0x801E;

void query_glx_size(void* display, unsigned long drawable, uint32_t& width, uint32_t& height) {
    // Asked again while null: the dispatcher this resolves through becomes
    // known when the application reveals it, and a first frame can precede that.
    static PFN_glXQueryDrawable query = nullptr;
    if (!query) {
        query = gl_symbol<PFN_glXQueryDrawable>("glXQueryDrawable");
    }
    width = 0;
    height = 0;
    if (!query) {
        return;
    }
    unsigned int value = 0;
    query(display, drawable, kGlxWidth, &value);
    width = value;
    query(display, drawable, kGlxHeight, &value);
    height = value;
}

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

using PFN_eglQuerySurface = unsigned int (*)(void*, void*, int, int*);  // as above: only the query

constexpr int kEglWidth = 0x3057;
constexpr int kEglHeight = 0x3056;

void query_egl_size(void* display, void* surface, uint32_t& width, uint32_t& height) {
    static PFN_eglQuerySurface query = nullptr;
    if (!query) {
        query = gl_symbol<PFN_eglQuerySurface>("eglQuerySurface");
    }
    width = 0;
    height = 0;
    if (!query) {
        return;
    }
    int value = 0;
    if (query(display, surface, kEglWidth, &value)) {
        width = static_cast<uint32_t>(value);
    }
    if (query(display, surface, kEglHeight, &value)) {
        height = static_cast<uint32_t>(value);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The shim's entry points
// ---------------------------------------------------------------------------

// Everything else in this library is hidden (-fvisibility=hidden), so only the
// names below are exported. That is not tidiness: this library carries its own
// copy of ImGui, and it used to export all thousand-odd of its symbols. Loaded
// through the shim that never mattered, because the shim dlopens it RTLD_LOCAL --
// but preloaded directly, as vocem-run used to do, those symbols would sit at the
// front of the global lookup order and a game with its own dynamically linked
// ImGui would have found ours instead.
//
// This library used to carry its own interposed glXSwapBuffers, dlsym and the
// proc-address hooks as well, "for direct preloading of this library, which is
// what the development scripts do" -- and no script had preloaded it since
// vocem-run switched to the shim. Every path into a process preloads the shim
// (environment.d, vocem-run, dev-run-gl.sh, the tests), the shim dlopens this
// library RTLD_LOCAL and resolves exactly the vocem_gl_* names below, so those
// eighty lines were a second, unreachable copy of the shim's hook table --
// free to drift from the real one, which is the entry-33 shape.
#define VOCEM_EXPORT __attribute__((visibility("default")))

// One lock over everything this library exports.
//
// The four entry points below reach one process-wide overlay -- an ImGui context,
// two maps, GL objects -- and an application may call them from different threads:
// a worker loading resources destroys its context while the render thread is
// between NewFrame and RenderDrawData, and release() then runs
// ImGui_ImplOpenGL3_Shutdown and DestroyContext underneath it. The Vulkan half
// has taken a lock for exactly this since it was written; this half had none at
// all. It is not a lock-free path -- draw() already reads files -- so the cost is
// a contended mutex on a path that is already making syscalls.
static std::mutex g_gl_lock;

extern "C" {

// Called by the shim, which is the only thing actually preloaded.
VOCEM_EXPORT void vocem_gl_present_glx(void* display, unsigned long drawable) {
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    // The size is not queried here: on GLX it is two X round trips, and draw()
    // asks for it only once the frame has decided to draw (33 us a pair,
    // measured -- a real cost in every GL process that never draws).
    overlay().draw(
        [](void* dpy, void* handle, uint32_t& w, uint32_t& h) {
            query_glx_size(dpy, reinterpret_cast<unsigned long>(handle), w, h);
        },
        display, reinterpret_cast<void*>(drawable), false);
}

VOCEM_EXPORT void vocem_gl_present_egl(void* display, void* surface) {
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    overlay().draw(
        [](void* dpy, void* handle, uint32_t& w, uint32_t& h) {
            query_egl_size(dpy, handle, w, h);
        },
        display, surface, true);
}

// A GL context is going away, and everything we built lives in one.
//
// Called from the shim before the real destroy runs, so the context still exists
// and can be made current -- which is the only way to delete what is in it. That is
// the dance MangoHud does in its own `glXDestroyContext`: remember what is current,
// make the dying context current, tear down, put the previous one back.
//
// If any part of that is unavailable the state is dropped without calling GL. The
// objects die with the context either way; what is lost is ImGui's own small heap
// block, once, which is a better trade than issuing GL calls into a context that
// may not be current.
//
// Without this the overlay held texture names and a shader program belonging to a
// context that no longer existed, and used them on the next frame. Nothing said so:
// the driver is entitled to do anything at all with a stale name, and mostly it
// draws nothing.
VOCEM_EXPORT void vocem_gl_context_destroyed(void* display, void* context) {
    // Only the context the backend lives in. Every context a game destroys
    // used to reach the release below -- a loader thread's helper context, a
    // splash screen's, SDL's probe context -- and the dance underneath made
    // the dying one current with the drawing one's drawable, deleted the
    // backend's names in a context that never held them, and left the next
    // frame to rebuild the whole backend (an atlas rasterised again, every
    // face uploaded again) for a context that had never been touched. And it
    // reached here in every process that ever presented, browsers included,
    // whether or not a backend existed at all. Asked before the lock and
    // before the overlay is so much as constructed: "not mine" costs one
    // atomic load. tests/gl_draw_local.cpp destroys a second context and
    // counts the rebuilds.
    if (!GlOverlay::owns(display, context, false)) {
        return;
    }
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    vocem::journal_note("GLX context destroyed");
    using PFN_glXGetCurrentContext = void* (*)();
    using PFN_glXGetCurrentDrawable = unsigned long (*)();
    using PFN_glXMakeCurrent = int (*)(void*, unsigned long, void*);
    static PFN_glXGetCurrentContext current_context = nullptr;
    static PFN_glXGetCurrentDrawable current_drawable = nullptr;
    static PFN_glXMakeCurrent make_current = nullptr;
    if (!current_context) {
        current_context = gl_symbol<PFN_glXGetCurrentContext>("glXGetCurrentContext");
    }
    if (!current_drawable) {
        current_drawable = gl_symbol<PFN_glXGetCurrentDrawable>("glXGetCurrentDrawable");
    }
    if (!make_current) {
        make_current = gl_symbol<PFN_glXMakeCurrent>("glXMakeCurrent");
    }

    if (!display || !context || !current_context || !current_drawable || !make_current) {
        overlay().release(false);
        return;
    }

    void* previous = current_context();
    const unsigned long drawable = current_drawable();
    if (previous == context) {
        // Already current: nothing to swap, and nothing to put back afterwards
        // either, since the context is about to stop existing.
        overlay().release(true);
        return;
    }
    if (make_current(display, drawable, context)) {
        overlay().release(true);
        make_current(display, drawable, previous);
    } else {
        overlay().release(false);
    }
}

// The EGL side of the same thing. `eglDestroyContext` and `eglTerminate` both end
// with our objects gone; `eglTerminate` takes the whole display with it, so there
// is nothing to make current and nothing to put back.
VOCEM_EXPORT void vocem_gl_egl_context_destroyed(void* display, void* context) {
    // As above: the backend's own context, or -- with no context, which is
    // eglTerminate -- the backend's own display. Anything else is somebody
    // else's business, including every EGL context Chromium's ANGLE creates
    // and destroys in a browser that will never draw a frame of ours.
    if (!GlOverlay::owns(display, context, true)) {
        return;
    }
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    using PFN_eglGetCurrentContext = void* (*)();
    using PFN_eglGetCurrentSurface = void* (*)(int);
    using PFN_eglMakeCurrent = unsigned int (*)(void*, void*, void*, void*);
    static PFN_eglGetCurrentContext current_context = nullptr;
    static PFN_eglGetCurrentSurface current_surface = nullptr;
    static PFN_eglMakeCurrent make_current = nullptr;
    if (!current_context) {
        current_context = gl_symbol<PFN_eglGetCurrentContext>("eglGetCurrentContext");
    }
    if (!current_surface) {
        current_surface = gl_symbol<PFN_eglGetCurrentSurface>("eglGetCurrentSurface");
    }
    if (!make_current) {
        make_current = gl_symbol<PFN_eglMakeCurrent>("eglMakeCurrent");
    }

    constexpr int kEglDraw = 0x3059;
    constexpr int kEglRead = 0x305A;

    if (!display || !current_context || !current_surface || !make_current) {
        overlay().release(false);
        return;
    }
    void* previous = current_context();
    if (!context || previous == context) {
        overlay().release(previous != nullptr);
        return;
    }
    void* draw = current_surface(kEglDraw);
    void* read = current_surface(kEglRead);
    if (make_current(display, draw, read, context)) {
        overlay().release(true);
        make_current(display, draw, read, previous);
    } else {
        overlay().release(false);
    }
}

}  // extern "C"

namespace {

// The clean end of the journal: a process that unwinds normally runs this and
// takes its crash marker with it; a crash does not, which is the mechanism.
__attribute__((destructor)) void vocem_gl_journal_close() {
    vocem::journal_end();
}

}  // namespace
