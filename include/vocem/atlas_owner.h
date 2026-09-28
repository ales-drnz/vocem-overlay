// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Who holds the overlay's backend, and the thread that rasterises the first
// font atlas: one spelling for both injected paths.
//
// The backend (ImGui's renderer objects) lives in ONE place -- a GL context,
// or a Vulkan device and queue -- and means nothing, or something else, in any
// other. A present from anywhere else is passed through, said once; once the
// holder has been silent for kSeconds the backend moves to whoever presents
// (entries 210, 236). A place the backend could not be made in holds it the
// same way, so every way a holder is given up also clears the failure
// (entry 263). What "the same place" means is the path's: the current context
// on OpenGL, the device and queue on Vulkan.
//
// The atlas is the fonts module's (entry 144) and outlives every backend. Its
// first build runs on AtlasWorker, beside the game (entry 192); the three
// rules every path keeps with it:
//   * join() before the holder's backend, its ImGui context or the atlas is
//     torn down -- the worker reaches all three;
//   * wait_until_built() before ANOTHER ImGui context is destroyed -- the
//     worker's allocations count themselves through ImGui's global context
//     pointer (entry 262) -- without joining, so the path that draws still
//     learns the atlas came from the worker;
//   * never either inside a present, which can simply come back later:
//     building() answers without waiting. The OpenGL path notices the switch,
//     the daemon stopping and the hand-over inside a swap, and while a build
//     runs it leaves them to a later present (tests/gl_release_mid_build.cpp);
//     the Vulkan layer acts on them after the present has returned. The
//     teardown hooks -- a context or device destroyed, eglTerminate, the ELF
//     destructors -- have no later, and join there.
//
// The job is copied, and the thread created, only on the frame that starts a
// build -- inside that swap on OpenGL, after the present on Vulkan; no other
// frame allocates here. Hidden like the rest of vocem_common; each injected
// library carries its own copy, and its own worker.

#ifndef VOCEM_ATLAS_OWNER_H
#define VOCEM_ATLAS_OWNER_H

#include <pthread.h>

#include <atomic>
#include <mutex>
#include <string>

namespace vocem {

// ---------------------------------------------------------------------------
// The hand-over: an explicit state and the one transition a present makes.
// Not thread-safe on its own: each path asks it under the lock its present
// holds (g_gl_lock, the layer's g_lock).
// ---------------------------------------------------------------------------
class HandOver {
public:
    // How long the holder may be silent before the backend moves.
    static constexpr double kSeconds = 2.0;

    enum class Holding : unsigned char {
        Nobody,  // whoever presents next builds the backend
        Ready,   // built, in the holder's place
        Failed,  // could not be built in the holder's place; held all the same
    };
    enum class Presenter : unsigned char {
        Owner,      // the holder: draw (Ready) or keep holding (Failed)
        Foreign,    // somebody else while the holder is alive: pass through
        Abandoned,  // somebody else, the holder silent for kSeconds: move here
    };

    Holding holding() const { return holding_; }
    bool held() const { return holding_ != Holding::Nobody; }

    // The transition. `from_owner` is the path's answer to "is this present
    // from the holder's place". Asked only while held(): with nobody holding,
    // whoever presents builds, and there is nothing to compare with.
    Presenter present(bool from_owner, double now);

    // The presenter current now built the backend (Ready) or could not
    // (Failed). Its clock starts here, not at its next present: a second
    // presenter arriving meanwhile must not find it silent since the process
    // began.
    void take(Holding what, double now);

    // Every way a holder is given up ends here: its place destroyed, the
    // hand-over, the switch, the daemon stopping. A failure goes with it.
    void let_go();

    // True when a Foreign presenter -- up to two opaque words, a context or a
    // device and queue -- is not the last one told since let_go(): "said
    // once", not once per frame, and again when two alternate.
    bool first_word_with(const void* a, const void* b);

    // When the holder last presented or took the backend, on the frame clock;
    // 0 while nobody holds it.
    double seen() const { return owner_seen_; }

private:
    Holding holding_ = Holding::Nobody;
    double owner_seen_ = 0.0;
    const void* said_a_ = nullptr;
    const void* said_b_ = nullptr;
};

// ---------------------------------------------------------------------------
// A font upload that failed, owed until one works. Vulkan's allocations can
// fail and say so (entry 194); nothing is drawn from an image that does not
// match the atlas, and the whole upload is tried again once a second.
//
// Only the Vulkan path has a failure to retry. On OpenGL the backend's
// CreateFontsTexture answers true whatever happened, and glTexImage2D reports
// running out of memory only through glGetError -- the game's own error queue,
// which asking would empty. A fold that cannot go up in place goes up whole,
// which is the one fallback that path can know it needs.
// ---------------------------------------------------------------------------
class UploadRetry {
public:
    static constexpr double kSeconds = 1.0;

    bool owed() const { return retry_at_ > 0.0; }
    bool due(double now) const { return retry_at_ > 0.0 && now >= retry_at_; }
    void failed(double now) { retry_at_ = now + kSeconds; }
    void succeeded() { retry_at_ = 0.0; }

private:
    double retry_at_ = 0.0;
};

// ---------------------------------------------------------------------------
// The first atlas's worker. A pthread, not a std::thread: the injected code
// has no exceptions and std::thread reports a refused clone by throwing,
// which would end the game; a refusal is a return code here, and the caller
// then builds on its own thread. One per library, leaked (atlas_worker()), so
// the job it reads is never destroyed under it at exit (entry 151's class);
// the ELF destructor joins it only if it was ever made (atlas_worker_made()).
// ---------------------------------------------------------------------------
class AtlasWorker {
public:
    struct Job {
        float pixels = 0.0f;
        float reference = 16.0f;
        std::string body;
        std::string strong;
        // Said by the worker once the atlas is rasterised, where it happened:
        // a present that never comes (the context died mid-build) would
        // never say it. Null says nothing.
        void (*built)(float pixels) = nullptr;
    };

    // What one look at the first atlas found (step()).
    enum class Step : unsigned char {
        Idle,      // no build started and none wanted
        Started,   // a thread took the build: this frame goes without the overlay
        NoThread,  // no thread to be had: the caller builds on its own thread
        Building,  // still rasterising: this frame goes without the overlay
        Finished,  // done and joined here: the atlas goes up whole
    };

    // The one question both paths ask on the frame that wants the first
    // atlas (`wanted`), and on every frame while one is being built. Copies
    // the job only when it starts one: no allocation on any other frame.
    Step step(bool wanted, float pixels, float reference, const std::string& body,
              const std::string& strong, void (*built)(float));

    // The job step() stored, on the caller's thread: the NoThread answer.
    void run_here();

    // A thread was started and not yet joined.
    bool started();
    // Started and not finished: a present asks this and never waits.
    bool building();
    // Waits for a started thread and joins it. Idempotent.
    void join();
    // Until a started build has finished, without joining it (entry 262):
    // after that the thread touches nothing a caller can tear down.
    void wait_until_built();

    // What the thread runs: ensure_fonts() and the RGBA32 widening by
    // default. A test replaces it to drive the lifecycle without fonts.
    using Rasterise = void (*)(const Job&);
    void set_rasterise(Rasterise rasterise) { rasterise_ = rasterise; }

private:
    static void* run(void* self);

    // Starting and joining only: the ELF destructor joins without the path's lock.
    std::mutex lock_;
    pthread_t thread_{};
    bool running_ = false;
    std::atomic<bool> done_{false};
    Job job_;
    Rasterise rasterise_ = nullptr;
};

// This library's worker, made at the first call. Never destroyed: see
// AtlasWorker.
AtlasWorker& atlas_worker();

// The worker if atlas_worker() ever made it, null otherwise: what a teardown
// that must construct nothing asks -- the ELF destructors, which run at every
// dlclose of a library that may never have drawn (tests/injected_unload.cpp).
AtlasWorker* atlas_worker_made();

}  // namespace vocem

#endif  // VOCEM_ATLAS_OWNER_H
