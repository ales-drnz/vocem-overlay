// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Vocem Overlay - OpenGL interposer.
//
// Vulkan has a layer mechanism; OpenGL has nothing of the sort, so the overlay
// gets in by symbol interposition, through three doors, all of them in the
// shim (gl/src/vocem_gl_shim.cpp):
//
//   1. Direct calls resolved by the dynamic linker  -> LD_PRELOAD is enough.
//   2. glXGetProcAddress / eglGetProcAddress lookups -> those are hooked too.
//   3. dlsym() called by the application itself      -> dlsym is hooked as
//      well: SDL, GLFW and glad dlopen their GL and dlsym on that handle,
//      invisible to interposition by construction.
//
// This library hooks nothing; it draws when the shim hands it a frame. The
// panel is the shared implementation in common/src/panel.cpp: this file only
// deals with getting a frame, a size, and a texture upload path.

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#ifndef VOCEM_IMCONFIG_INJECTED
#error "vocem/imconfig_injected.h is not in effect: IM_ASSERT would be assert() inside a game"
#endif
#include "real_dlsym.h"
#include "vocem/apps.h"
#include "vocem/atlas_owner.h"
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

// No image parser in here, on purpose: the cache is raw RGBA at one fixed size
// (vocem/avatar_rgba.h), and the daemon is the only process that decodes a PNG.

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
using PFN_glTexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                     const void*);
using PFN_glIsEnabled = unsigned char (*)(GLenum);
using PFN_glEnable = void (*)(GLenum);
using PFN_glDisable = void (*)(GLenum);
using PFN_glGetString = const unsigned char* (*)(GLenum);
using PFN_glDeleteTextures = void (*)(GLsizei, const GLuint*);

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

// The real dlsym, reached through dlvsym so it bypasses the shim's interposed
// dlsym, which is in this process. Every internal lookup must go through this:
// the interposed dlsym asked for "glXSwapBuffers" would hand back the shim's
// hook, and the hook would call itself every frame until the stack ran out.
// The version is looked for, not assumed (real_dlsym.h): wrong here, every
// lookup below is null and the overlay silently draws nothing.
void* real_dlsym(void* handle, const char* name) {
    using PFN_dlsym = void* (*)(void*, const char*);
    // A null is not remembered: a failed first look must not answer null for
    // the life of the process.
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
// RTLD_NEXT first, right when this library was preloaded directly, then
// RTLD_DEFAULT, which is the case that matters: the shim brings this library
// in with dlopen(RTLD_LOCAL), and RTLD_NEXT then searches *its own* local
// scope, which does not contain libGL.
//
// Only safe for functions the SHIM does not hook: RTLD_DEFAULT asked for
// glXSwapBuffers would find its interposed copy -- ahead of libGL in the
// global scope -- and call it forever.
template <typename Fn>
Fn next_symbol(const char* name) {
    if (void* found = real_dlsym(RTLD_NEXT, name)) {
        return reinterpret_cast<Fn>(found);
    }
    return reinterpret_cast<Fn>(real_dlsym(RTLD_DEFAULT, name));
}

// The third road, for the games where the two above find nothing at all:
// everything built on GLFW, LWJGL or SDL opens its GL library with
// dlopen(RTLD_LOCAL), so no GL symbol is in the global scope (glxgears links
// libGL, and is the wrong witness for this).
//
// What is always reachable is a *dispatcher*: RTLD_DEFAULT finds the shim's
// exported glXGetProcAddress/eglGetProcAddress, which forward to the real
// dispatcher once the application's own resolution has revealed it -- before
// its first frame, since that is how it finds every other function. Asked
// only for names we do not interpose (the hook would answer with itself). A
// null is not cached: it must not outlive the dispatcher becoming known.
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
// glTexImage2D reads a client pointer through GL_UNPACK_*, application state
// that survives a swap: a game's wide GL_UNPACK_ROW_LENGTH makes a 64x64
// upload read far past our 16 KB buffer, and a bound pixel-unpack buffer
// makes the pointer an offset into *its* buffer and pushes
// GL_INVALID_OPERATION into the game's error queue (tests/gl_unpack_state.cpp).
// ImGui's backend neutralises these only in CreateDeviceObjects, not on a
// font-atlas rebuild.
//
// Every value is read before it is written and written back afterwards
// (rule 12). `Pack` is the same guard for glReadPixels, which writes through
// GL_PACK_* and the pixel-pack buffer (the frame capture).
class PixelStoreGuard {
public:
    enum class Direction { Unpack, Pack };

    // `es` is 0 for desktop GL, or the OpenGL ES major version. Which of these
    // names exist depends on it, and asking for one that does not is
    // GL_INVALID_ENUM -- an error in the game's queue, which is the very thing
    // this class is here to avoid. ES 2 has the alignment alone; ES 3 adds the
    // row length and the two skips but has neither of the byte-order flags;
    // desktop GL has all six and the pixel-unpack buffer, which ES gained in 3.0.
    PixelStoreGuard(PFN_glGetIntegerv get, PFN_glPixelStorei set, PFN_glBindBuffer bind, int es,
                    Direction direction = Direction::Unpack)
        : get_(get), set_(set), bind_(es == 2 ? nullptr : bind),
          count_(es == 2 ? 1u : (es >= 3 ? 4u : kCount)),
          names_(direction == Direction::Pack ? kPackNames : kNames),
          buffer_target_(direction == Direction::Pack ? kPixelPackBuffer : kPixelUnpackBuffer),
          buffer_binding_(direction == Direction::Pack ? kPixelPackBufferBinding
                                                       : kPixelUnpackBufferBinding) {
        if (!get_ || !set_) {
            return;  // without both, nothing here can be done safely
        }
        for (unsigned i = 0; i < count_; ++i) {
            get_(names_[i], &saved_[i]);
            if (saved_[i] != kNeutral[i]) {
                set_(names_[i], kNeutral[i]);
            }
        }
        if (bind_) {
            get_(buffer_binding_, &buffer_);
            if (buffer_ != 0) {
                bind_(buffer_target_, 0);
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
                set_(names_[i], saved_[i]);
            }
        }
        if (bind_ && buffer_ != 0) {
            bind_(buffer_target_, static_cast<GLuint>(buffer_));
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
    // The same six for glReadPixels, in the same order.
    static constexpr GLenum kPackNames[kCount] = {
        0x0D05,  // GL_PACK_ALIGNMENT
        0x0D02,  // GL_PACK_ROW_LENGTH
        0x0D03,  // GL_PACK_SKIP_ROWS
        0x0D04,  // GL_PACK_SKIP_PIXELS
        0x0D00,  // GL_PACK_SWAP_BYTES
        0x0D01,  // GL_PACK_LSB_FIRST
    };
    static constexpr GLint kNeutral[kCount] = {4, 0, 0, 0, 0, 0};
    static constexpr GLenum kPixelUnpackBuffer = 0x88EC;
    static constexpr GLenum kPixelUnpackBufferBinding = 0x88EF;
    static constexpr GLenum kPixelPackBuffer = 0x88EB;
    static constexpr GLenum kPixelPackBufferBinding = 0x88ED;

    PFN_glGetIntegerv get_ = nullptr;
    PFN_glPixelStorei set_ = nullptr;
    PFN_glBindBuffer bind_ = nullptr;
    unsigned count_ = kCount;
    const GLenum* names_ = kNames;
    GLenum buffer_target_ = kPixelUnpackBuffer;
    GLenum buffer_binding_ = kPixelUnpackBufferBinding;
    GLint saved_[kCount] = {0};
    GLint buffer_ = 0;
    bool active_ = false;
};

// The application's sRGB-write state, switched off for the overlay's draw and
// put back (rule 12). With GL_FRAMEBUFFER_SRGB on, the hardware encodes what
// the shader writes, and ImGui's colours are already sRGB: encoded twice, the
// panel's dark slate turns light grey while white and saturated colours, being
// fixed points, still look right. The Vulkan half answers this in the shader;
// here it is a switch. ImGui's GL backend does not touch it.
//
// Desktop GL only: on OpenGL ES the enum arrives with EXT_sRGB_write_control,
// and asking for one that does not exist is a GL_INVALID_ENUM in the game's
// error queue.
class SrgbWriteGuard {
public:
    // `es` is 0 for desktop GL, or the OpenGL ES major version; `major` is the
    // desktop GL major version. GL_FRAMEBUFFER_SRGB is core from 3.0, and
    // asking a 2.1 context about it is GL_INVALID_ENUM, once per frame.
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

// Said by the atlas worker (vocem/atlas_owner.h) where the rasterisation
// happened, and not where the game's thread next looks: a context that dies
// mid-build joins the worker in release() and never reaches the draw path's
// join, and gl_context_cycle and gl_daemon_gone count this line.
void say_atlas_built(float pixels) {
    VOCEM_GLOG("font atlas built at %.0f px", static_cast<double>(pixels));
}

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
        // The two that make an upload safe in somebody else's renderer. A null
        // here means no GL could be reached at all (no symbol in scope, no
        // dispatcher known yet), and then there are no avatars rather than an
        // upload that reads wherever the game's state points. It does NOT mean
        // the context lacks them: glvnd's dispatchers hand out a stub for any
        // name at all, and a stub for a function the context lacks does nothing.
        pixel_store_ = gl_symbol<PFN_glPixelStorei>("glPixelStorei");
        bind_buffer_ = gl_symbol<PFN_glBindBuffer>("glBindBuffer");
        // Not part of `resolved_`: without them the overlay's colours are wrong
        // (SrgbWriteGuard), which is no reason to go without avatars.
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

    // For the frame capture's glReadPixels, which writes through GL_PACK_*.
    PixelStoreGuard pack_store_guard() const {
        return PixelStoreGuard(get_integer_, pixel_store_, bind_buffer_, es_,
                               PixelStoreGuard::Direction::Pack);
    }

    // The framebuffer glReadPixels reads, as draw_framebuffer_target() below
    // is the one the overlay draws into: its own target where the API has one,
    // GL_FRAMEBUFFER (both at once) on ES 2.
    GLenum read_framebuffer_target() const {
        return es_ == 2 ? GL_FRAMEBUFFER : 0x8CA8;  // GL_READ_FRAMEBUFFER
    }
    GLenum read_framebuffer_binding() const {
        return es_ == 2 ? GL_FRAMEBUFFER_BINDING : 0x8CAA;  // GL_READ_FRAMEBUFFER_BINDING
    }

    // For the overlay's own draw: the sRGB write state is the game's, and the
    // overlay's colours are already encoded.
    SrgbWriteGuard srgb_write_guard() const {
        return SrgbWriteGuard(is_enabled_, enable_, disable_, es_, gl_major_);
    }

    // The target the framebuffer retarget in draw() binds. GL_FRAMEBUFFER binds
    // BOTH the draw and the read framebuffer while GL_FRAMEBUFFER_BINDING reads
    // the draw one, so restoring "the binding" would leave the read side
    // pointing at what the overlay drew into -- clobbering a game that presents
    // by blitting from its own read framebuffer (rule 12). ES 2 has only the
    // one target, and nothing else to clobber.
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
        // A POD key, so the steady-state lookup below allocates nothing
        // (vocem/avatar_key.h says why).
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

        // At most one picture taken on per frame, the Vulkan cache's rule: a
        // channel filling up lands several faces at once, and a few frames of
        // grey discs are invisible where a hitch is not (entry 52). The check
        // sits above the stat as well as above the read, so a deferred face
        // costs this frame nothing at all.
        if (taken_on_this_frame_ >= kFacesPerFrame) {
            return 0;
        }

        // Uploading here is safe in a way the Vulkan path is not: a GL texture
        // upload does not need a queue submit or a fence, so it can happen inline
        // as long as we restore the binding the application had.
        char path[768];
        vocem::avatar_rgba_path(path, sizeof(path), user_id, avatar_hash);

        // Not there yet is not the same as broken: somebody who joins is drawn
        // while the daemon is still downloading their picture. Looked at again
        // twice a second rather than on every frame.
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
        // process, the journal's last line has to name it. Entry 46's crash was
        // an avatar upload -- the Vulkan cache's eighth face -- and nothing said so.
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

    // The texture names held, handed over to whoever will delete them -- the
    // backend moving away from this context (GlOverlay::move_away) -- and
    // forgotten here. The names alone: the context they live in is the
    // caller's to know.
    void take_texture_names(std::vector<GLuint>& names) {
        for (const auto& entry : textures_) {
            if (entry.second != 0) {
                names.push_back(static_cast<GLuint>(entry.second));
            }
        }
        textures_.clear();
    }

    // Every texture name we hold belongs to a GL context. Forget them.
    //
    // No `glDeleteTextures` here: a dying context deletes them first wherever
    // it can reach them (GlOverlay::release_dying -- in a share group the names
    // outlive the context), and switching the overlay off should not touch the
    // application's GL state on the way out. The pictures are re-uploaded from
    // the cache on the first frame after the overlay comes back.
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

// The context the backend's GL objects live in, and its display: plain
// globals so the teardown hooks can answer "not mine" -- in every browser and
// compositor, which get our eglDestroyContext hook -- without constructing
// the overlay. Written under g_gl_lock; read with the atomic builtins so the
// hooks can look before they lock.
void* g_owner_context = nullptr;
void* g_owner_display = nullptr;
int g_owner_egl = 0;
// How many backends the overlay has moved away from and not yet deleted
// (GlOverlay::move_away). Read by the teardown hooks before they lock, for the
// same reason as the three above: a destroyed context that is neither the
// owner nor one of these costs one atomic load.
int g_left_backends = 0;

class GlOverlay {
public:
    // Asks the windowing system how large the drawable is. Passed in rather
    // than asked up front: on GLX the two queries are X server round trips,
    // in every GL process of the session, so the question is asked only once a
    // frame has decided to draw (entry 45). The handle is an XID or an
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

        // What a backend left in a context it moved away from, deleted in the
        // first present where that context is current again -- the only moment
        // its objects can be reached (move_away() says why they were left).
        // First, before anything below can return: a switched-off overlay and
        // a stopped daemon hand everything back as well.
        if (!left_.empty()) {
            reclaim_left(egl);
        }

        // Before the first thing that derives a path. Inside a Flatpak game the
        // segment, config.ini and the avatar cache are all on the far side of
        // the sandbox, and this is what points the three lookups at the copies
        // the daemon puts where they can be reached (vocem/flatpak.h).
        session_.enter_flatpak_bridge_once();

        // One stat() every couple of seconds, not per frame.
        const vocem::Config& config = config_.current();

        // Recorded before any decision below, so an application appears in the
        // window's list whether or not the overlay may draw in it, and whether
        // or not there is anything to draw right now.
        vocem::record_application("opengl");

        // Whether the overlay belongs in this frame, asked **every** frame, so
        // the switch and the tray act on a running game: two string
        // comparisons, the lists walked only when one has changed. One
        // spelling with the Vulkan layer's, master switch included
        // (vocem/overlay_session.h). Turning it off releases the GL state
        // (release()); turning it back on rebuilds the backend in one frame.
        const bool want = session_.decide(config);
        if (want) {
            // The session's journal (vocem/journal.h): opened at the first
            // frame the overlay draws in this process, closed into history on
            // a clean exit by the destructor below -- and left behind, still
            // `.running`, by a crash, which is the detection.
            session_.journal_begin_once();
        }
        if (drawing_ >= 0 && want != (drawing_ == 1)) {
            if (!want && release_waits_for_build("switched off")) {
                return;  // drawing_ stays: the next present asks again
            }
            VOCEM_GLOG("%s in '%s'", want ? "switched on" : "switched off",
                       vocem::process_name().c_str());
            vocem::journal_note(want ? "switched on" : "switched off");
            if (!want) {
                // A present hook is the one place where one of the
                // application's contexts is guaranteed current, so this is the
                // moment to hand back what lives in it -- in it, and nowhere
                // else (give_back_here). Worded as the daemon-stopped case
                // below and the Vulkan layer word the same release.
                VOCEM_GLOG("switched off: releasing the backend and the font atlas");
                give_back_here(egl);
                // The atlas belongs to the fonts module and survives release()
                // on purpose, so that a game cycling its context pays nothing;
                // being switched off is when its glyphs go back.
                vocem::fonts_release();
            }
        }
        drawing_ = want ? 1 : 0;
        if (!want) {
            return;
        }

        // The Debug section's frame and draw counters, written every few
        // seconds: two file syscalls and a rename, throttled far below entry
        // 52's budget; the counting itself is two integers.
        session_.frame_seen();

        vocem::Snapshot* snapshot = poll_state();
        if (!snapshot) {
            // The daemon stopped -- the tray's Quit, or `systemctl --user stop`.
            // Not drawing is not enough: the backend, the atlas and a texture
            // per face are held on the daemon's behalf, and go back exactly as
            // when the switch is turned off (entry 146).
            if (state_poll_.daemon_left_pending() &&
                release_waits_for_build("the daemon stopped")) {
                return;  // still pending: the next present asks again
            }
            if (state_poll_.daemon_left()) {
                VOCEM_GLOG("the daemon stopped: releasing the backend and the font atlas");
                vocem::journal_note("daemon stopped: released");
                give_back_here(egl);
                vocem::fonts_release();
            }
            return;
        }
        // One clock for the whole frame: the animation step, the panel's motion
        // and the toast's age must agree about what time it is.
        const double now = vocem::monotonic_seconds();
        // Either feature is a reason to spend the frame (vocem/panel.h says
        // why there is one spelling of this).
        const bool panel_frame = vocem::panel_wanted(*snapshot, config);
        const bool toast_frame = vocem::notification_wanted(*snapshot, config, now);
        if (!panel_frame && !toast_frame) {
            session_.note_forget();
            return;
        }
        // The message's words, fetched only now that this process will draw
        // the toast, the one condition under which the note segment is opened
        // (vocem/note.h): three syscalls per message, none per frame. The
        // snapshot is this process's own copy, and note_forget() wipes it the
        // moment the toast is over. A message that arrived without its words
        // is said once, by the session.
        if (toast_frame) {
            std::snprintf(snapshot->notification.body, sizeof(snapshot->notification.body),
                          "%s", session_.note_words(snapshot->notification.serial));
        } else {
            session_.note_forget();
        }
        // The backend's objects are names in the context it was built in, and
        // mean something else, or nothing, in another unshared context: a
        // context that is not the owner is drawn nothing into, until the owner
        // has been silent for HandOver::kSeconds and the backend moves to it (a
        // game may keep its loading-screen context alive). The old context's
        // objects are deleted the next time it presents (move_away(),
        // reclaim_left()). A context the backend could not be made in holds the
        // overlay the same way (fail_in_this_context).
        if (hand_over_.held()) {
            switch (whose_present(egl, now)) {
                case Present::Owner:
                    break;
                case Present::Foreign:
                    return;
                case Present::Abandoned:
                    if (release_waits_for_build("the backend's context fell silent")) {
                        return;  // Abandoned again at the next present
                    }
                    VOCEM_GLOG("the backend's context has not presented for %.0f s: moving the "
                               "overlay to the one that does", vocem::HandOver::kSeconds);
                    move_away();
                    break;
            }
        }
        if (!ensure_backend(egl)) {
            return;
        }
        // Only now is the drawable's size worth two X round trips: every path
        // above this line returns without drawing.
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

        // The atlas at the size this drawable needs: the first one from the
        // worker, a rebuild for a new size or typeface here, and a new colour
        // emoji -- noted from this frame's text before the question is asked --
        // folded in and its squares copied, never a rebuild.
        //
        // One pixel-store guard over the whole of it -- a whole upload, the
        // folded squares, the draw -- because the backend's CreateFontsTexture
        // sets GL_UNPACK_ROW_LENGTH to 0 and never restores it.
        const PixelStoreGuard unpack = avatars_.pixel_store_guard();

        // The FIRST atlas of this process, or the first after fonts_release(),
        // goes to the worker (vocem/atlas_owner.h says why). A rebuild for a
        // new size or typeface stays here: it is rare, and the atlas it
        // replaces is the one this frame would otherwise draw from.
        bool atlas_from_worker = false;
        const float first_pixels = vocem::font_pixel_size(
            vocem::sizing_height(snapshot->display_height, height), config.scale,
            config.font_size);
        switch (vocem::atlas_worker().step(vocem::fonts().pixel_size == 0.0f, first_pixels,
                                           config.font_size, config.font_path,
                                           config.font_path_strong, &say_atlas_built)) {
            case vocem::AtlasWorker::Step::Started:
                VOCEM_GLOG("rasterising the font atlas at %.0f px off the game's thread",
                           static_cast<double>(first_pixels));
                return;
            case vocem::AtlasWorker::Step::Building:
                return;  // still rasterising: this frame goes out without the overlay
            case vocem::AtlasWorker::Step::Finished:
                atlas_from_worker = true;
                break;
            case vocem::AtlasWorker::Step::NoThread:
                // Built below, by the ensure_fonts() every frame asks.
                VOCEM_GLOG("no thread for the font atlas; rasterising it on the game's thread");
                break;
            case vocem::AtlasWorker::Step::Idle:
                break;
        }

        vocem::fonts_note_emoji_in(*snapshot);
        // Sized by the display, not by the window: sizing from the drawable
        // would make every resize rubber-band the whole panel, since every
        // distance is a multiple of ui_scale(). sizing_height() says when the
        // drawable wins instead (zero display, or a supersampled drawable
        // taller than the display and headed for a downscale).
        const float wanted_pixels = vocem::font_pixel_size(
            vocem::sizing_height(snapshot->display_height, height), config.scale,
            config.font_size);
        const uint32_t builds_before = vocem::fonts_build_count();
        if (vocem::ensure_fonts(wanted_pixels, config.font_size, config.font_path.c_str(),
                                config.font_path_strong.c_str())) {
            // The expensive thing this process does, said out loud:
            // tests/gl_context_cycle.cpp counts these lines, so it can assert a
            // count instead of a clock. A new emoji FOLDED into the atlas
            // answers true as well (the texture still has to go up again) and
            // is not a build, so it says so in its own words (entry 191).
            const bool rebuilt = vocem::fonts_build_count() != builds_before;
            if (rebuilt) {
                VOCEM_GLOG("font atlas built at %.0f px", static_cast<double>(wanted_pixels));
            } else {
                VOCEM_GLOG("colour emoji folded into the font atlas");
            }
            vocem::configure_style(config);
            // A fold changed a few 32-pixel squares of an atlas the texture
            // already holds, so only those go up (entry 192): the whole atlas
            // is a 43 MB glTexImage2D at 2160 lines. A build replaces the
            // texture whole, and so does a fold the regions cannot describe.
            if (rebuilt || atlas_from_worker || !upload_folded_regions()) {
                upload_atlas_whole();
                atlas_from_worker = false;
            }
        }
        if (atlas_from_worker) {
            // The worker's atlas, and nothing new folded into it since: the
            // backend's texture still holds the default bitmap it was created
            // with, so the atlas goes up whole now.
            vocem::configure_style(config);
            upload_atlas_whole();
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

        // Into the framebuffer about to be presented, not necessarily the one
        // the application left bound: ImGui's backend saves a dozen pieces of
        // state around its draw, but not the framebuffer binding, and an
        // application that swaps with its own framebuffer object bound would
        // get the overlay drawn into an object that is then discarded. The
        // DRAW target alone where the API has one (draw_framebuffer_target()),
        // restored immediately (rule 12).
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
            // Around the draw alone: the switch is per-draw state and does not
            // affect the uploads above (SrgbWriteGuard says why it is off).
            const SrgbWriteGuard srgb = avatars_.srgb_write_guard();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        if (retarget && previous_framebuffer != 0) {
            bind_framebuffer_(draw_target, static_cast<GLuint>(previous_framebuffer));
        }

        capture_if_asked(width, height);
        session_.frame_drawn();
    }

    // Give back everything that belongs to a GL context, and be ready to build
    // it again: the context our objects live in was destroyed, or the overlay
    // was switched off (a guest asked to leave keeps nothing). `gl_current`
    // says whether a GL context is current *right now* and safe to call into:
    // in a present hook it is; in a destroy hook only if the dying context was
    // made current for the purpose.
    void release(bool gl_current) {
        // A first atlas still being rasterised reaches the context through
        // ImGui::GetIO() and the atlas that fonts_release() is about to clear:
        // it finishes first (entry 192).
        vocem::atlas_worker().join();
        if (backend_ready() && gl_current) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        avatars_.forget();
        forget_owner();
        session_.reset_clock();
        // A backend that failed against one context deserves a fresh attempt
        // against the next: the failure was about that context, not about us.
        hand_over_.let_go();
        bind_framebuffer_ = nullptr;
        tex_sub_image_ = nullptr;
        bind_texture_ = nullptr;
        get_integer_ = nullptr;
        current_context_ = nullptr;
        capture_warmup_frames_ = 0;

        // Without a current context ImGui's backend cannot be shut down (that
        // deletes GL objects): its small heap block is leaked once per context
        // destruction, and the GL objects it named are gone with the context.
        // Destroying the ImGui context makes the next `Init` start from nothing
        // rather than from a half-torn-down backend.
        if (ImGui::GetCurrentContext()) {
            ImGui::DestroyContext();
        }
    }

    // The backend moves to another context and leaves this one's objects where
    // they are: they can only be deleted with this context current, and it is
    // not. The ImGui context that owns the backend is kept whole, with the GL
    // context it lives in and the face textures' names, and reclaim_left()
    // shuts it down the first time that context is current again; a context
    // that dies first is handed to forget_left() by the teardown hooks
    // (tests/gl_handover.cpp).
    void move_away() {
        vocem::atlas_worker().join();
        void* context = __atomic_load_n(&g_owner_context, __ATOMIC_ACQUIRE);
        ImGuiContext* imgui = ImGui::GetCurrentContext();
        if (backend_ready() && context && imgui) {
            LeftBackend left;
            left.context = context;
            left.display = __atomic_load_n(&g_owner_display, __ATOMIC_ACQUIRE);
            left.egl = __atomic_load_n(&g_owner_egl, __ATOMIC_ACQUIRE) == 1;
            left.imgui = imgui;
            avatars_.take_texture_names(left.textures);
            left_.push_back(std::move(left));
            __atomic_store_n(&g_left_backends, static_cast<int>(left_.size()), __ATOMIC_RELEASE);
            // Not current any more, so release() below neither shuts it down
            // (no context to delete in) nor destroys it (it is kept).
            ImGui::SetCurrentContext(nullptr);
        }
        release(false);
    }

    // The teardown hooks' half: `context` is dying (or, null, every context on
    // `display` with eglTerminate). A backend left in it is shut down properly
    // when that context is current on the calling thread, dropped without GL
    // otherwise. A running build is waited for first: the worker's allocations
    // count themselves through the ImGui context about to be destroyed
    // (ImGui::MemAlloc). Waited for, not joined: the live backend's next
    // present joins it and uploads what it built.
    void forget_left(void* display, void* context, bool egl) {
        vocem::atlas_worker().wait_until_built();
        for (size_t i = left_.size(); i-- > 0;) {
            LeftBackend& left = left_[i];
            if (left.egl != egl || (context ? left.context != context : left.display != display)) {
                continue;
            }
            tear_down_left(left, context && current_context_of(egl) == context);
            left_.erase(left_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        __atomic_store_n(&g_left_backends, static_cast<int>(left_.size()), __ATOMIC_RELEASE);
    }

    static bool left_anywhere() { return __atomic_load_n(&g_left_backends, __ATOMIC_ACQUIRE) > 0; }

    // Whether the backend has GL objects to delete (a context it could not be
    // made in holds none). Under g_gl_lock.
    bool backend_ready() const { return hand_over_.holding() == vocem::HandOver::Holding::Ready; }

    // Whether a backend was left in this very context (move_away). Under g_gl_lock.
    bool left_in(void* context, bool egl) const {
        for (const LeftBackend& left : left_) {
            if (left.egl == egl && left.context == context) {
                return true;
            }
        }
        return false;
    }

    // release() for the owner context's death. With `gl_current` the faces go
    // too, with glDeleteTextures: a context that dies shares its objects with
    // every context in its share group -- a game's loader context is the
    // common case -- and a name dropped without GL there is a texture kept
    // for the life of that group, not a name that "dies with the context".
    void release_dying(bool gl_current) {
        if (gl_current && backend_ready()) {
            std::vector<GLuint> faces;
            avatars_.take_texture_names(faces);
            if (!faces.empty()) {
                if (auto delete_textures = gl_symbol<PFN_glDeleteTextures>("glDeleteTextures")) {
                    delete_textures(static_cast<GLsizei>(faces.size()), faces.data());
                }
            }
        }
        release(gl_current);
    }

    // Whether the context presenting now is the one the backend lives in,
    // asked with the API it arrived through: one getter call per frame, only
    // once somebody holds the backend; a getter that could not be resolved
    // answers "the owner". The transition itself is vocem::HandOver's: a
    // foreign context is Abandoned once the owner has been silent for
    // HandOver::kSeconds, and the caller moves the backend to it.
    using Present = vocem::HandOver::Presenter;
    Present whose_present(bool egl, double now) {
        void* owner = __atomic_load_n(&g_owner_context, __ATOMIC_ACQUIRE);
        void* current = current_context_ ? current_context_() : owner;
        const Present who = hand_over_.present(present_from_owner(egl), now);
        if (who == Present::Foreign && hand_over_.first_word_with(current, nullptr)) {
            VOCEM_GLOG(hand_over_.holding() == vocem::HandOver::Holding::Failed
                           ? "not drawing in context %p: the overlay is held by context %p, "
                             "where it could not be made and which presented %.1f s ago"
                           : "not drawing in context %p: the backend does not live in it (it "
                             "belongs to context %p, which presented %.1f s ago)",
                       current, owner, now - hand_over_.seen());
        }
        return who;
    }

    // Whether the context current in this present is the one the backend lives
    // in, asked with the API the present arrived through; a getter that could
    // not be resolved answers yes.
    bool present_from_owner(bool egl) {
        void* owner = __atomic_load_n(&g_owner_context, __ATOMIC_ACQUIRE);
        void* current = current_context_ ? current_context_() : owner;
        return current == owner && __atomic_load_n(&g_owner_egl, __ATOMIC_ACQUIRE) == (egl ? 1 : 0);
    }

    // Whether a release noticed in this present -- the switch, the daemon,
    // the hand-over -- has to wait for the first atlas's build. Each of them
    // joins the worker (release(), move_away()), and joining here would hold
    // the game's swap for the rest of the build: 142-151 ms at 2160 lines
    // (tests/gl_release_mid_build.cpp). While it runs this frame goes out
    // without the overlay, which a build does anyway, and a later present
    // asks again. Said once per wait.
    bool release_waits_for_build(const char* what) {
        vocem::AtlasWorker* worker = vocem::atlas_worker_made();
        if (!worker || !worker->building()) {
            waiting_said_ = nullptr;
            return false;
        }
        if (waiting_said_ != what) {
            VOCEM_GLOG("%s while the first font atlas is being rasterised: releasing once it "
                       "is done", what);
            waiting_said_ = what;
        }
        return true;
    }

    // The switch or the daemon took the overlay away, noticed in this
    // present. The backend is shut down with GL calls only where it lives: in
    // another, unshared context its names are that context's own objects
    // (entry 237's rule). From there it is left in its own context, as the
    // hand-over leaves it, and deleted when that context next presents --
    // reclaim_left() runs before the switch is asked (tests/gl_switch_foreign.cpp).
    void give_back_here(bool egl) {
        if (backend_ready() && !present_from_owner(egl)) {
            move_away();
        } else {
            release(true);
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
    // written there as a PPM -- the one honest check of what the overlay looks
    // like, since screenshot tools on Wayland capture the wrong window or need a
    // portal prompt. Once per process, and nothing at all when the variable is
    // unset.
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
        // read from the default framebuffer.
        constexpr GLenum GL_RGBA_FORMAT = 0x1908;
        const size_t count = static_cast<size_t>(width) * height * 4;
        unsigned char* pixels = static_cast<unsigned char*>(std::malloc(count));
        if (!pixels) {
            return;
        }
        {
            // From the framebuffer the overlay drew into, through neutral pack
            // state, and everything put back: glReadPixels reads the READ
            // framebuffer and writes through GL_PACK_* and the pixel-pack
            // buffer, all of it the game's (entry 240). Without the entry
            // points to neutralise the state there is no capture at all.
            const PixelStoreGuard pack = avatars_.pack_store_guard();
            if (!pack.ok()) {
                std::free(pixels);
                return;
            }
            GLint previous_read = 0;
            const bool rebind = bind_framebuffer_ && get_integer_;
            if (rebind) {
                get_integer_(avatars_.read_framebuffer_binding(), &previous_read);
                if (previous_read != 0) {
                    bind_framebuffer_(avatars_.read_framebuffer_target(), 0);
                }
            }
            read_pixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                        GL_RGBA_FORMAT, GL_UNSIGNED_BYTE, pixels);
            if (rebind && previous_read != 0) {
                bind_framebuffer_(avatars_.read_framebuffer_target(),
                                  static_cast<GLuint>(previous_read));
            }
        }

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

    // One spelling with the Vulkan layer's, in vocem/state_poll.h.
    vocem::Snapshot* poll_state() { return state_poll_.poll(); }

    bool ensure_backend(bool egl) {
        switch (hand_over_.holding()) {
            case vocem::HandOver::Holding::Ready:
                return true;
            case vocem::HandOver::Holding::Failed:
                return false;
            case vocem::HandOver::Holding::Nobody:
                break;
        }

        if (!ImGui::GetCurrentContext()) {
            IMGUI_CHECKVERSION();
            // With the fonts module's atlas, not one of the context's own: the
            // atlas has to outlive this context, which dies and comes back
            // (vocem/fonts.h says what the alternatives cost).
            ImGui::CreateContext(vocem::fonts_atlas());
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.BackendPlatformName = "vocem-gl";
            vocem::configure_style(config_.current());
        }

        // The shader source has to match the context (entry 28): the backend
        // chooses its GLSL version at build time, "#version 130" for desktop GL,
        // and an OpenGL ES context refuses it -- the program does not link and
        // every draw call is rejected with GL_INVALID_OPERATION, silently, since
        // the backend's own checks are assertions and those are compiled out.
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
            return fail_in_this_context(egl);
        }
        // Asked again after Init where the first ask found nothing: in a game
        // that never resolved a dispatcher of its own, the third road has
        // nothing to forward to until ImGui's loader, in Init, has resolved one
        // through the dlsym door. Init took the desktop default GLSL, which an ES
        // context refuses (tests/gl_es_glsl.cpp), so an ES answer initialises
        // the backend again with its language; nothing is built yet.
        if (!version_known) {
            read_version();
            if (glsl_version) {
                VOCEM_GLOG("the context is OpenGL ES and the backend was initialised before it "
                           "could be asked: initialising it again with %s", glsl_version);
                ImGui_ImplOpenGL3_Shutdown();
                if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
                    VOCEM_GLOG("ImGui OpenGL3 backend failed to initialise");
                    return fail_in_this_context(egl);
                }
            }
        }
        // The backend's GL objects -- shader, buffers and the font texture --
        // built HERE, not left to its NewFrame: draw() replaces the font
        // texture once ensure_fonts() has the atlas at this output's size, and
        // a NewFrame building them after that would orphan a whole atlas
        // texture -- 43 MB at a 2160-line display -- per backend build
        // (tests/gl_draw_local.cpp counts the textures).
        avatars_.resolve(es_version, gl_major);
        {
            // Under the pixel-store guard, as draw() keeps the backend's font
            // upload: CreateFontsTexture zeroes GL_UNPACK_ROW_LENGTH and never
            // restores it.
            const PixelStoreGuard unpack = avatars_.pixel_store_guard();
            // CreateDeviceObjects uploads the atlas as it finds it: the RGBA
            // copy with the colour squares in it, handed back once it is up.
            unsigned char* pixels = nullptr;
            int atlas_width = 0;
            int atlas_height = 0;
            vocem::fonts_atlas_rgba(&pixels, &atlas_width, &atlas_height);
            const bool created = ImGui_ImplOpenGL3_CreateDeviceObjects();
            vocem::fonts_atlas_uploaded();
            if (!created) {
                VOCEM_GLOG("ImGui OpenGL3 backend could not create its GL objects");
                ImGui_ImplOpenGL3_Shutdown();
                return fail_in_this_context(egl);
            }
        }
        // CreateDeviceObjects answers true whether or not its program linked
        // (it prints the compiler's complaint to the game's stderr and goes
        // on), and a program that did not link turns every draw into an error
        // in the game's queue. Asked here, so "ready" below is a fact.
        if (!backend_program_linked()) {
            VOCEM_GLOG("not drawing in this context: the backend's shader program did not link "
                       "(ImGui's complaint is on stderr)");
            ImGui_ImplOpenGL3_Shutdown();
            return fail_in_this_context(egl);
        }
        // Resolved once, beside the rest. Null only when no GL can be reached
        // at all (see GlAvatarProvider::resolve): under glvnd a context older
        // than framebuffer objects still gets a pointer -- a stub that does
        // nothing -- and it is GL_FRAMEBUFFER_BINDING reading 0 there, not
        // this pointer, that leaves nothing to retarget.
        bind_framebuffer_ = gl_symbol<PFN_glBindFramebuffer>("glBindFramebuffer");
        // For uploading a folded emoji into the font texture in place. Core
        // since GL 1.1; without it a fold replaces the texture whole, as a
        // build does.
        tex_sub_image_ = gl_symbol<PFN_glTexSubImage2D>("glTexSubImage2D");
        bind_texture_ = gl_symbol<PFN_glBindTexture>("glBindTexture");
        get_integer_ = gl_symbol<PFN_glGetIntegerv>("glGetIntegerv");
        // Whose context this backend now lives in, so that the teardown hooks
        // can tell that context's death from any other's (see them below).
        remember_owner(egl, vocem::HandOver::Holding::Ready);
        VOCEM_GLOG("OpenGL backend ready");
        vocem::journal_note("OpenGL backend ready");
        return true;
    }

private:
    // The first fields of ImGui_ImplOpenGL3_Data (imgui_impl_opengl3.cpp),
    // which the backend keeps in io.BackendRendererUserData and does not
    // export: the program's name is the one thing needed from it, to ask GL
    // whether it linked. Mirrored for the vendored 1.91.9 and held to it: a
    // new ImGui fails to compile here until somebody has compared the prefix.
    struct BackendDataPrefix {
        GLuint gl_version;
        char glsl_version_string[32];
        bool profile_is_es2;
        bool profile_is_es3;
        bool profile_is_compat;
        GLint profile_mask;
        GLuint font_texture;
        GLuint shader_handle;
    };
    static_assert(IMGUI_VERSION_NUM == 19190,
                  "BackendDataPrefix mirrors ImGui_ImplOpenGL3_Data of ImGui 1.91.9: compare it "
                  "with the new backend's struct, then change this number");

    // Whether the backend's shader program linked. Unknown -- no
    // glGetProgramiv, no backend -- is answered yes.
    bool backend_program_linked() {
        using PFN_glGetProgramiv = void (*)(GLuint, GLenum, GLint*);
        const auto* data =
            static_cast<const BackendDataPrefix*>(ImGui::GetIO().BackendRendererUserData);
        auto get_program = gl_symbol<PFN_glGetProgramiv>("glGetProgramiv");
        if (!data || !get_program) {
            return true;
        }
        GLint linked = 0;
        get_program(data->shader_handle, 0x8B82 /*GL_LINK_STATUS*/, &linked);
        return data->shader_handle != 0 && linked != 0;
    }

    // The backend could not be made in the context current now. The failure
    // belongs to that context, and it is remembered WITH it: the context is
    // taken as the owner, so every way an owner is given up -- its teardown,
    // the hand-over once it has been silent for HandOver::kSeconds, the switch,
    // the daemon stopping -- clears the failure and lets the next context try,
    // rather than one splash context taking the overlay from the whole
    // process (tests/gl_failed_context.cpp). Asked every frame in the
    // meantime is one flag.
    bool fail_in_this_context(bool egl) {
        remember_owner(egl, vocem::HandOver::Holding::Failed);
        return false;
    }

    // Asked once, when the backend comes up or fails to, of the API the present
    // arrived through: which context is current right now is the one the
    // backend's objects were just created in, and it holds the backend as
    // `holding` from now on.
    void remember_owner(bool egl, vocem::HandOver::Holding holding) {
        using PFN_current = void* (*)();
        void* context = nullptr;
        void* display = nullptr;
        current_context_ = gl_symbol<PFN_current>(egl ? "eglGetCurrentContext"
                                                      : "glXGetCurrentContext");
        if (current_context_) {
            context = current_context_();
        }
        if (egl) {
            if (auto current = gl_symbol<PFN_current>("eglGetCurrentDisplay")) {
                display = current();
            }
        } else {
            if (auto current = gl_symbol<PFN_current>("glXGetCurrentDisplay")) {
                display = current();
            }
        }
        __atomic_store_n(&g_owner_display, display, __ATOMIC_RELEASE);
        __atomic_store_n(&g_owner_egl, egl ? 1 : 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_owner_context, context, __ATOMIC_RELEASE);
        hand_over_.take(holding, vocem::monotonic_seconds());
        VOCEM_GLOG(holding == vocem::HandOver::Holding::Failed
                       ? "the overlay stays with %s context %p, where it could not be made, "
                         "until that context is destroyed or falls silent"
                       : "backend belongs to %s context %p",
                   egl ? "EGL" : "GLX", context);
    }

    void forget_owner() {
        __atomic_store_n(&g_owner_context, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
        __atomic_store_n(&g_owner_display, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
    }

    // A backend the overlay moved away from (move_away()): the ImGui context
    // that owns it, the GL context its objects live in, and its faces.
    struct LeftBackend {
        void* context = nullptr;
        void* display = nullptr;
        bool egl = false;
        ImGuiContext* imgui = nullptr;
        std::vector<GLuint> textures;
    };
    std::vector<LeftBackend> left_;
    void* (*egl_current_)() = nullptr;
    void* (*glx_current_)() = nullptr;

    void* current_context_of(bool egl) {
        auto& getter = egl ? egl_current_ : glx_current_;
        if (!getter) {
            getter = gl_symbol<void* (*)()>(egl ? "eglGetCurrentContext" : "glXGetCurrentContext");
        }
        return getter ? getter() : nullptr;
    }

    // In a present: a backend left in the context current now goes, properly.
    // Not while the atlas worker is still rasterising (forget_left() says why),
    // and not by waiting for it either, inside the game's present: the reclaim
    // waits for a present after the build, and a context that dies first goes
    // through forget_left(), which does wait. A finished build is not joined
    // here: the live backend's present joins it and uploads what it built.
    void reclaim_left(bool egl) {
        void* current = current_context_of(egl);
        if (!current) {
            return;
        }
        if (vocem::atlas_worker().building()) {
            return;
        }
        for (size_t i = 0; i < left_.size(); ++i) {
            if (left_[i].egl == egl && left_[i].context == current) {
                tear_down_left(left_[i], true);
                left_.erase(left_.begin() + static_cast<std::ptrdiff_t>(i));
                __atomic_store_n(&g_left_backends, static_cast<int>(left_.size()),
                                 __ATOMIC_RELEASE);
                return;  // one entry per context: moving away from it twice reclaims first
            }
        }
    }

    // With `gl_current`, the left context is current here: the backend's own
    // Shutdown deletes its program, buffers and font texture, and the faces
    // go with glDeleteTextures. Without, only ImGui's side is freed. Either way
    // the live backend is untouched -- its ImGui context is put back, and so is
    // the shared atlas's texture name, which the old backend's Shutdown zeroes
    // (the atlas is one object across every ImGui context, vocem/fonts.h).
    void tear_down_left(LeftBackend& left, bool gl_current) {
        ImGuiContext* live = ImGui::GetCurrentContext();
        const ImTextureID atlas_texture = vocem::fonts_atlas()->TexID;
        ImGui::SetCurrentContext(left.imgui);
        if (gl_current) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        ImGui::DestroyContext(left.imgui);
        ImGui::SetCurrentContext(live);
        vocem::fonts_atlas()->SetTexID(atlas_texture);
        size_t faces = 0;
        if (gl_current && !left.textures.empty()) {
            if (auto delete_textures = gl_symbol<PFN_glDeleteTextures>("glDeleteTextures")) {
                delete_textures(static_cast<GLsizei>(left.textures.size()), left.textures.data());
                faces = left.textures.size();
            }
        }
        VOCEM_GLOG("%s the backend left in context %p when the overlay moved (%zu face "
                   "texture(s) deleted)",
                   gl_current ? "deleted" : "dropped, with its context,", left.context, faces);
        left.imgui = nullptr;
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
    // The release release_waits_for_build() last said it is waiting with, so
    // it is said once per wait; null when none waits.
    const char* waiting_said_ = nullptr;
    // Who holds the backend -- nobody, this context ready, or this context
    // where it could not be made -- and the clock and words of the hand-over
    // (vocem/atlas_owner.h). The context itself is in the owner globals above.
    vocem::HandOver hand_over_;
    bool captured_ = false;
    int capture_warmup_frames_ = 0;
    PFN_glBindFramebuffer bind_framebuffer_ = nullptr;
    PFN_glTexSubImage2D tex_sub_image_ = nullptr;
    PFN_glBindTexture bind_texture_ = nullptr;
    PFN_glGetIntegerv get_integer_ = nullptr;
    // The owner API's "get current context", resolved once when the backend
    // comes up (remember_owner) so the per-frame ask below is one call into
    // the dispatcher and no lookup.
    void* (*current_context_)() = nullptr;

    // The whole atlas into a new font texture: the fonts module's RGBA copy
    // (widened again, colour squares and all, if it was handed back after the
    // last one) goes up through the backend's CreateFontsTexture and is handed
    // back once it is up -- the texture holds it from there. Under draw()'s
    // PixelStoreGuard.
    void upload_atlas_whole() {
        vocem::fonts_take_folded(nullptr, 0);  // the whole atlas carries them
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        vocem::fonts_atlas_rgba(&pixels, &width, &height);
        ImGui_ImplOpenGL3_CreateFontsTexture();
        vocem::fonts_atlas_uploaded();
        VOCEM_GLOG("font texture uploaded whole");
    }

    // The squares a fold wrote, uploaded into the font texture the backend
    // already made, each from the fonts module's own packed copy of it (ES 2
    // has no GL_UNPACK_ROW_LENGTH to read one out of a wider image, and the
    // atlas's RGBA copy is gone once the whole atlas is up). Runs under
    // draw()'s PixelStoreGuard. The game's texture binding goes back
    // (rule 12). False when anything is missing: the caller then replaces the
    // texture whole, which is always correct.
    bool upload_folded_regions() {
        if (!tex_sub_image_ || !bind_texture_ || !get_integer_) {
            return false;
        }
        const ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        const GLuint texture = static_cast<GLuint>(atlas->TexID);  // an ImU64 in 1.91
        const int width = atlas->TexWidth;
        const int height = atlas->TexHeight;
        if (texture == 0 || width <= 0 || height <= 0) {
            return false;
        }
        vocem::AtlasRegion regions[vocem::kMaxFoldedRegions];
        const uint32_t count = vocem::fonts_take_folded(regions, vocem::kMaxFoldedRegions);
        for (uint32_t i = 0; i < count; ++i) {
            const vocem::AtlasRegion& region = regions[i];
            if (!region.pixels || region.width <= 0 || region.height <= 0 ||
                region.width > vocem::kMaxFoldedSide || region.height > vocem::kMaxFoldedSide ||
                region.x + region.width > width || region.y + region.height > height) {
                return false;
            }
        }
        GLint previous = 0;
        get_integer_(GL_TEXTURE_BINDING_2D, &previous);
        bind_texture_(GL_TEXTURE_2D, texture);
        for (uint32_t i = 0; i < count; ++i) {
            const vocem::AtlasRegion& region = regions[i];
            tex_sub_image_(GL_TEXTURE_2D, 0, region.x, region.y, region.width, region.height,
                           GL_RGBA, GL_UNSIGNED_BYTE, region.pixels);
        }
        bind_texture_(GL_TEXTURE_2D, static_cast<GLuint>(previous));
        // Said, so the arrivals scene can count it: a fold that went up whole
        // would pass every other check (entry 192).
        VOCEM_GLOG("font texture: %u folded square(s) copied in place", count);
        return true;
    }
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

// Only the query is needed here: the swap and dispatcher hooks are the shim's.
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

// The context the backend lives in, dying while it is not current here. Its
// objects can only be deleted with it current, and they do not always die
// with it: SDL and GLFW make NULL current and then destroy, and a loader
// context shares its objects with every window context
// (tests/gl_destroy_owner.cpp, `shared`).
//
// So it is made current for the teardown alone, on a 1x1 pbuffer of its own
// framebuffer configuration -- the game's drawable may not match (BadMatch)
// -- and what was current before is put back, read and draw drawables both.
// A GLX failure is an X error, and Xlib's default handler calls exit(1): so
// everything runs under a handler of ours that swallows the errors this
// thread's requests raise from the first serial on and forwards every other.
// Any error or missing function, and the teardown is the one without GL.
//
// Xlib is found where the game put it: the global scope, or among the loaded
// objects (SDL and GLFW dlopen libX11 RTLD_LOCAL), asked through its own
// handle. Nothing here opens a file.
struct XErrorEventLayout {  // XErrorEvent as <X11/Xlib.h> lays it out, unchanged since X11R4
    int type;
    void* display;
    unsigned long resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};
using XErrorHandlerFn = int (*)(void*, XErrorEventLayout*);

struct XErrorTrap {
    // Written by the thread holding g_gl_lock; read by the handler, which Xlib
    // may call on any thread.
    int active = 0;
    pthread_t thread{};
    void* display = nullptr;
    unsigned long first_serial = 0;
    int errors = 0;
    int last_code = 0;
    // The handler ours replaced, kept after ours is taken off: another
    // library that swapped handlers while ours was in (SDL does, around its
    // own GLX calls) may put ours back for good, and then it forwards.
    XErrorHandlerFn forward = nullptr;
};
XErrorTrap g_x_trap;

int x_error_trap(void* display, XErrorEventLayout* event) {
    if (__atomic_load_n(&g_x_trap.active, __ATOMIC_ACQUIRE) &&
        pthread_equal(pthread_self(), g_x_trap.thread) &&
        (display != g_x_trap.display ||
         static_cast<long>(event->serial - g_x_trap.first_serial) >= 0)) {
        ++g_x_trap.errors;
        g_x_trap.last_code = event->error_code;
        return 0;
    }
    XErrorHandlerFn next = __atomic_load_n(&g_x_trap.forward, __ATOMIC_ACQUIRE);
    return next ? next(display, event) : 0;
}

struct X11Found {
    void* handle = nullptr;
};

int find_loaded_x11(struct dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name ? std::strrchr(info->dlpi_name, '/') : nullptr;
    name = name ? name + 1 : info->dlpi_name;
    if (!name || std::strncmp(name, "libX11.so.6", 11) != 0) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            Dl_info where;
            void* map = nullptr;
            const void* address =
                reinterpret_cast<const void*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            // glibc's handle is the object's link_map: dlsym on it searches
            // the object and its own dependencies.
            if (dladdr1(address, &where, &map, RTLD_DL_LINKMAP) && map) {
                static_cast<X11Found*>(data)->handle = map;
            }
            return 1;
        }
    }
    return 0;
}

// The caller keeps what is found; a null is asked again next time.
template <typename Fn>
Fn x11_symbol(const char* name) {
    if (Fn found = next_symbol<Fn>(name)) {
        return found;
    }
    X11Found x11;
    dl_iterate_phdr(&find_loaded_x11, &x11);
    return x11.handle ? reinterpret_cast<Fn>(real_dlsym(x11.handle, name)) : nullptr;
}

class DyingGlxCurrent {
public:
    DyingGlxCurrent(void* display, void* context) : display_(display) {
        using PFN_current = void* (*)();
        using PFN_drawable = unsigned long (*)();
        using PFN_setHandler = XErrorHandlerFn (*)(XErrorHandlerFn);
        using PFN_sync = int (*)(void*, int);
        using PFN_nextRequest = unsigned long (*)(void*);
        using PFN_free = int (*)(void*);
        using PFN_queryContext = int (*)(void*, void*, int, int*);
        using PFN_chooseConfig = void** (*)(void*, int, const int*, int*);
        using PFN_configAttrib = int (*)(void*, void*, int, int*);
        using PFN_createPbuffer = unsigned long (*)(void*, void*, const int*);

        static PFN_setHandler set_handler = nullptr;
        static PFN_sync sync = nullptr;
        static PFN_nextRequest next_request = nullptr;
        static PFN_free x_free = nullptr;
        if (!set_handler) set_handler = x11_symbol<PFN_setHandler>("XSetErrorHandler");
        if (!sync) sync = x11_symbol<PFN_sync>("XSync");
        if (!next_request) next_request = x11_symbol<PFN_nextRequest>("XNextRequest");
        if (!x_free) x_free = x11_symbol<PFN_free>("XFree");
        // Each looked up until found, then remembered: a lookup walks the
        // loader's lists under its lock (entry 36), and a game may destroy
        // contexts often.
        static PFN_makeContextCurrent make_current = nullptr;
        static PFN_destroyPbuffer destroy_pbuffer = nullptr;
        static PFN_current get_context = nullptr;
        static PFN_current get_display = nullptr;
        static PFN_drawable get_draw = nullptr;
        static PFN_drawable get_read = nullptr;
        static PFN_queryContext query_context = nullptr;
        static PFN_chooseConfig choose_config = nullptr;
        static PFN_configAttrib config_attrib = nullptr;
        static PFN_createPbuffer create_pbuffer = nullptr;
        if (!make_current) make_current = gl_symbol<PFN_makeContextCurrent>("glXMakeContextCurrent");
        if (!destroy_pbuffer) destroy_pbuffer = gl_symbol<PFN_destroyPbuffer>("glXDestroyPbuffer");
        if (!get_context) get_context = gl_symbol<PFN_current>("glXGetCurrentContext");
        if (!get_display) get_display = gl_symbol<PFN_current>("glXGetCurrentDisplay");
        if (!get_draw) get_draw = gl_symbol<PFN_drawable>("glXGetCurrentDrawable");
        if (!get_read) get_read = gl_symbol<PFN_drawable>("glXGetCurrentReadDrawable");
        if (!query_context) query_context = gl_symbol<PFN_queryContext>("glXQueryContext");
        if (!choose_config) choose_config = gl_symbol<PFN_chooseConfig>("glXChooseFBConfig");
        if (!config_attrib) config_attrib = gl_symbol<PFN_configAttrib>("glXGetFBConfigAttrib");
        if (!create_pbuffer) create_pbuffer = gl_symbol<PFN_createPbuffer>("glXCreatePbuffer");
        make_current_ = make_current;
        destroy_pbuffer_ = destroy_pbuffer;
        if (!display || !context || !set_handler || !sync || !x_free || !make_current ||
            !destroy_pbuffer || !get_context || !get_display || !get_draw || !get_read ||
            !query_context || !choose_config || !config_attrib || !create_pbuffer) {
            VOCEM_GLOG("dying context %p: GLX or Xlib functions missing, so its objects "
                       "are dropped without GL",
                       context);
            return;
        }
        sync_ = sync;
        set_handler_ = set_handler;
        previous_context_ = get_context();
        previous_display_ = get_display();
        previous_draw_ = get_draw();
        previous_read_ = get_read();

        // Errors already on their way are the game's, delivered to its handler.
        sync(display, 0);
        g_x_trap.thread = pthread_self();
        g_x_trap.display = display;
        g_x_trap.first_serial = next_request ? next_request(display) : 0;
        g_x_trap.errors = 0;
        g_x_trap.last_code = 0;
        __atomic_store_n(&g_x_trap.active, 1, __ATOMIC_RELEASE);
        XErrorHandlerFn replaced = set_handler(&x_error_trap);
        if (replaced != &x_error_trap) {
            __atomic_store_n(&g_x_trap.forward, replaced, __ATOMIC_RELEASE);
        }
        replaced_ = replaced;
        trapped_ = true;

        constexpr int kGlxFbconfigId = 0x8013;
        constexpr int kGlxScreen = 0x800C;
        constexpr int kGlxDrawableType = 0x8010;
        constexpr int kGlxPbufferBit = 0x4;
        constexpr int kGlxPbufferWidth = 0x8041;
        constexpr int kGlxPbufferHeight = 0x8040;
        int config_id = 0;
        int screen = 0;
        if (query_context(display, context, kGlxFbconfigId, &config_id) != 0 ||
            query_context(display, context, kGlxScreen, &screen) != 0) {
            VOCEM_GLOG("dying context %p: its configuration could not be asked", context);
            return;
        }
        const int wanted[] = {kGlxFbconfigId, config_id, 0};
        int count = 0;
        void** configs = choose_config(display, screen, wanted, &count);
        if (configs && count > 0) {
            int drawable_types = 0;
            if (config_attrib(display, configs[0], kGlxDrawableType, &drawable_types) == 0 &&
                (drawable_types & kGlxPbufferBit)) {
                const int size[] = {kGlxPbufferWidth, 1, kGlxPbufferHeight, 1, 0};
                pbuffer_ = create_pbuffer(display, configs[0], size);
            }
        }
        if (configs) {
            x_free(configs);
        }
        // Without a pbuffer the context is asked with no drawable at all,
        // which GL 3.0 contexts accept; an older one refuses, trapped.
        sync(display, 0);
        attempted_ = g_x_trap.errors == 0;
        if (attempted_ && make_current_(display, pbuffer_, pbuffer_, context)) {
            sync(display, 0);
            current_ = g_x_trap.errors == 0 && get_context() == context;
        }
        VOCEM_GLOG("dying context %p %s", context,
                   current_ ? "made current on a pbuffer of its own configuration, to delete "
                              "the backend's objects in it"
                            : "could not be made current: the backend's objects are dropped "
                              "without GL");
    }

    ~DyingGlxCurrent() {
        if (!trapped_) {
            return;
        }
        // What was current goes back, whether or not the dying context was
        // made current: a failed attempt may have unbound it.
        if (!attempted_) {
        } else if (previous_context_) {
            make_current_(previous_display_ ? previous_display_ : display_, previous_draw_,
                          previous_read_, previous_context_);
        } else {
            make_current_(display_, 0, 0, nullptr);
        }
        if (pbuffer_) {
            destroy_pbuffer_(display_, pbuffer_);
        }
        sync_(display_, 0);
        if (previous_display_ && previous_display_ != display_) {
            sync_(previous_display_, 0);
        }
        __atomic_store_n(&g_x_trap.active, 0, __ATOMIC_RELEASE);
        // Ours comes off only if it is still the one installed; a handler put
        // in on top of ours in the meantime stays, and ours keeps forwarding.
        XErrorHandlerFn installed = set_handler_(replaced_);
        if (installed != &x_error_trap) {
            set_handler_(installed);
        }
        if (g_x_trap.errors > 0) {
            VOCEM_GLOG("dying context: %d X error(s) raised and kept from the game's handler "
                       "(the last, code %d)",
                       g_x_trap.errors, g_x_trap.last_code);
        }
    }

    DyingGlxCurrent(const DyingGlxCurrent&) = delete;
    DyingGlxCurrent& operator=(const DyingGlxCurrent&) = delete;

    bool current() const { return current_; }

private:
    using PFN_makeContextCurrent = int (*)(void*, unsigned long, unsigned long, void*);
    using PFN_destroyPbuffer = void (*)(void*, unsigned long);

    void* display_ = nullptr;
    bool trapped_ = false;
    bool attempted_ = false;
    bool current_ = false;
    unsigned long pbuffer_ = 0;
    void* previous_context_ = nullptr;
    void* previous_display_ = nullptr;
    unsigned long previous_draw_ = 0;
    unsigned long previous_read_ = 0;
    XErrorHandlerFn replaced_ = nullptr;
    PFN_makeContextCurrent make_current_ = nullptr;
    PFN_destroyPbuffer destroy_pbuffer_ = nullptr;
    int (*sync_)(void*, int) = nullptr;
    XErrorHandlerFn (*set_handler_)(XErrorHandlerFn) = nullptr;
};

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
// names below are exported: this library carries its own copy of ImGui, which
// preloaded directly would sit at the front of the global lookup order ahead
// of a game's own. It has no hooks: every path into a process
// preloads the shim, which dlopens this library RTLD_LOCAL and resolves
// exactly these vocem_gl_* names (tests/gl_entry_points.cmake).
#define VOCEM_EXPORT __attribute__((visibility("default")))

// One lock over everything this library exports.
//
// The four entry points below reach one process-wide overlay -- an ImGui
// context, two maps, GL objects -- and an application may call them from
// different threads: a worker destroying its context while the render thread
// is between NewFrame and RenderDrawData would run ImGui_ImplOpenGL3_Shutdown
// and DestroyContext underneath it. draw() already reads files, so the cost is
// a contended mutex on a path that is already making syscalls.
static std::mutex g_gl_lock;

extern "C" {

// Called by the shim, which is the only thing actually preloaded.
VOCEM_EXPORT void vocem_gl_present_glx(void* display, unsigned long drawable) {
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    // The size is not queried here: on GLX it is two X round trips, and draw()
    // asks for it only once the frame has decided to draw.
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

// A GL context is going away, and everything we built lives in one. Called
// from the shim before the real destroy. The backend's objects are deleted in
// the dying context: directly when it is current here, otherwise after making
// it current for the purpose (DyingGlxCurrent). Only when that fails is the
// state dropped without GL, the objects left to the context's share group.
// Without this hook the overlay would use names of a context that no longer
// exists.
VOCEM_EXPORT void vocem_gl_context_destroyed(void* display, void* context) {
    // Only the context the backend lives in, or one a backend was left in: any
    // other context a game destroys -- a loader's helper, a splash screen,
    // SDL's probe, every context of a browser -- must not cost a backend
    // rebuild. Asked before the lock and before the overlay is so much as
    // constructed: "not mine" costs one atomic load. tests/gl_draw_local.cpp
    // destroys a second context and counts the rebuilds.
    if (!GlOverlay::owns(display, context, false) && !GlOverlay::left_anywhere()) {
        return;
    }
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    // Asked again under the lock: a present on another thread may have moved
    // the backend (the hand-over) between the look above and the lock.
    const bool owner = GlOverlay::owns(display, context, false);
    const bool left = context && GlOverlay::left_anywhere() && overlay().left_in(context, false);
    if (!owner && !left) {
        return;
    }
    using PFN_glXGetCurrentContext = void* (*)();
    static PFN_glXGetCurrentContext current_context = nullptr;
    if (!current_context) {
        current_context = gl_symbol<PFN_glXGetCurrentContext>("glXGetCurrentContext");
    }
    bool gl_current = context && current_context && current_context() == context;
    // Not current here: made current for the teardown, on a pbuffer of its
    // own, and what was current put back when this scope ends (DyingGlxCurrent).
    std::optional<DyingGlxCurrent> made;
    if (context && !gl_current && ((owner && overlay().backend_ready()) || left)) {
        made.emplace(display, context);
        gl_current = made->current();
    }
    // A backend the overlay moved away from, left in this context (move_away).
    if (left) {
        overlay().forget_left(display, context, false);
    }
    if (owner) {
        vocem::journal_note("GLX context destroyed");
        overlay().release_dying(gl_current);
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
    if (!GlOverlay::owns(display, context, true) && !GlOverlay::left_anywhere()) {
        return;
    }
    const std::lock_guard<std::mutex> serialise(g_gl_lock);
    // A backend left in this context -- or, eglTerminate, on this display.
    if (GlOverlay::left_anywhere()) {
        overlay().forget_left(display, context, true);
    }
    // Asked again under the lock, as on GLX: the backend may have moved.
    if (!GlOverlay::owns(display, context, true)) {
        return;
    }
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

    // Nothing to delete -- the backend could not be made in this context --
    // needs no context made current.
    if (!display || !current_context || !current_surface || !make_current ||
        !overlay().backend_ready()) {
        overlay().release(false);
        return;
    }
    void* previous = current_context();
    if (!context) {
        // eglTerminate: GL calls reach the backend's objects only when the
        // context current here is the one it lives in; in another, unshared
        // context the backend's names are that context's own objects
        // (entry 237).
        overlay().release(previous != nullptr &&
                          previous == __atomic_load_n(&g_owner_context, __ATOMIC_ACQUIRE));
        return;
    }
    if (previous == context) {
        overlay().release(true);
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

// libstdc++'s own, defined in the copy this library carries (eh_alloc.cc).
namespace __gnu_cxx {
void __freeres() noexcept;
}

namespace {

// The clean end of the journal: a process that unwinds normally runs this and
// takes its crash marker with it; a crash does not, which is the mechanism.
__attribute__((destructor)) void vocem_gl_journal_close() {
    // A first atlas still being rasterised runs this library's code and reads
    // the atlas: it finishes before exit tears the process down (entry 192).
    // The shim never dlcloses this library, so exit is the one way this runs.
    // A process that never drew has no worker, and none is made here.
    if (vocem::AtlasWorker* worker = vocem::atlas_worker_made()) {
        worker->join();
    }
    vocem::journal_end();
    // Last, the exception emergency pool (about 73 KB) of the libstdc++ this
    // library carries inside it (-static-libstdc++, the top-level
    // CMakeLists.txt): its destructor never frees it, leaving that to
    // __gnu_cxx::__freeres(). In a game this runs once, at exit, where the
    // pool no longer matters: the shim never unloads this library. It is here
    // for what does unload it -- tests/injected_unload.cpp loads and unloads
    // both heavy libraries in a loop, as the Vulkan loader does the layer --
    // and so the two libraries end alike. It frees this library's own copy,
    // never the game's: the runtime inside is local to it.
    __gnu_cxx::__freeres();
}

}  // namespace
