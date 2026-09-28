// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "vocem/atlas_owner.h"

#include <time.h>

#include "imgui.h"
#include "vocem/fonts.h"

namespace vocem {

HandOver::Presenter HandOver::present(bool from_owner, double now) {
    if (from_owner) {
        owner_seen_ = now;
        return Presenter::Owner;
    }
    if (owner_seen_ > 0.0 && now - owner_seen_ >= kSeconds) {
        return Presenter::Abandoned;
    }
    return Presenter::Foreign;
}

void HandOver::take(Holding what, double now) {
    holding_ = what;
    owner_seen_ = now;
}

void HandOver::let_go() {
    holding_ = Holding::Nobody;
    owner_seen_ = 0.0;
    said_a_ = nullptr;
    said_b_ = nullptr;
}

bool HandOver::first_word_with(const void* a, const void* b) {
    if (a == said_a_ && b == said_b_) {
        return false;
    }
    said_a_ = a;
    said_b_ = b;
    return true;
}

namespace {

// The build itself: the atlas at the job's size, then its RGBA32 form, which
// is the other tenth of a second the game would otherwise wait for.
void rasterise_fonts(const AtlasWorker::Job& job) {
    ensure_fonts(job.pixels, job.reference, job.body.c_str(), job.strong.c_str());
    unsigned char* rgba = nullptr;
    int width = 0;
    int height = 0;
    fonts_atlas()->GetTexDataAsRGBA32(&rgba, &width, &height);
}

}  // namespace

void* AtlasWorker::run(void* self) {
    auto* worker = static_cast<AtlasWorker*>(self);
    const Job& job = worker->job_;
    (worker->rasterise_ ? worker->rasterise_ : &rasterise_fonts)(job);
    if (job.built) {
        job.built(job.pixels);
    }
    worker->done_.store(true, std::memory_order_release);
    return nullptr;
}

AtlasWorker::Step AtlasWorker::step(bool wanted, float pixels, float reference,
                                    const std::string& body, const std::string& strong,
                                    void (*built)(float)) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (running_) {
            if (!done_.load(std::memory_order_acquire)) {
                return Step::Building;
            }
            pthread_join(thread_, nullptr);
            running_ = false;
            return Step::Finished;
        }
        if (!wanted) {
            return Step::Idle;
        }
        job_.pixels = pixels;
        job_.reference = reference;
        job_.body = body;
        job_.strong = strong;
        job_.built = built;
        done_.store(false, std::memory_order_relaxed);
        running_ = pthread_create(&thread_, nullptr, &AtlasWorker::run, this) == 0;
        if (running_) {
            // Named, so a stack in a game's crash report says whose it is.
            pthread_setname_np(thread_, "vocem-atlas");
            return Step::Started;
        }
    }
    return Step::NoThread;
}

void AtlasWorker::run_here() { run(this); }

bool AtlasWorker::started() {
    std::lock_guard<std::mutex> guard(lock_);
    return running_;
}

bool AtlasWorker::building() {
    std::lock_guard<std::mutex> guard(lock_);
    return running_ && !done_.load(std::memory_order_acquire);
}

void AtlasWorker::join() {
    std::lock_guard<std::mutex> guard(lock_);
    if (running_) {
        pthread_join(thread_, nullptr);
        running_ = false;
    }
}

void AtlasWorker::wait_until_built() {
    while (building()) {
        const timespec millisecond{0, 1000000};
        nanosleep(&millisecond, nullptr);
    }
}

AtlasWorker& atlas_worker() {
    // Never destroyed (the class says why); a pointer, so no destructor is
    // registered at exit (entry 151).
    static AtlasWorker* worker = new AtlasWorker;
    return *worker;
}

}  // namespace vocem
