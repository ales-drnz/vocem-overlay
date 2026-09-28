// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The hand-over and the atlas worker (vocem/atlas_owner.h), driven without a
// GPU over EVERY sequence of events up to a bound.
//
// Thirty-eight entries are about who owns the atlas, the backend and the
// context, and fifteen of them were made by the fix before (37 -> 132 -> 144,
// 144 -> 211 -> 256, 210 -> 236 -> 262, 233 -> 260...). Each was found by a
// scene somebody thought of. This enumerates instead: two contexts, and the
// events that moved ownership in those entries --
//
//   P0 P1  a present from context 0 or 1 (0.1 s of frame clock)
//   T      2.5 s with no present at all
//   D0 D1  context 0 or 1 destroyed; a new one takes its number
//   W      the worker's build finishes
//   F      the next backend build fails in the context it is tried in
//   U      the next font upload fails
//   O      the switch goes off (or the daemon stops), noticed by the next
//          present, which switches it on again after
//
// -- every sequence of them up to six long (the argument, when given), each
// followed by an epilogue that must end in a drawn frame (a failure or a
// hand-over never keeps the overlay away for good) and by the death of both
// contexts (nothing left behind).
//
// What runs is the REAL HandOver, UploadRetry and AtlasWorker -- a real
// pthread per build, whose rasterisation is a stub the test lets finish -- and
// a model of a path around them that keeps the protocol both paths keep: the
// OpenGL one in full (left backends, a dying context made current, the
// reclaim), the Vulkan one being the same with no left backends. The model is
// a third spelling of that protocol, and what it proves is the module's and
// the protocol's, not the paths': the paths are held by the scenes in
// gl_handover, gl_destroy_owner, gl_failed_context and vk_present_draw.
//
// The invariants, checked at every step:
//   I1 at most one live backend, and it is the holder's, in the holder's place
//   I2 a backend's objects are deleted once, and only with its own context
//      current; any other end is its context's death
//   I3 no ImGui context destroyed and no atlas handed back while the worker
//      is inside the build (entries 192, 262)
//   I4 no backend built -- which uploads the atlas as it stands -- while the
//      worker writes it
//   I5 a frame is drawn only with a live backend in the current context, a
//      live ImGui context and a font texture that holds the atlas
//   I6 the backend moves only after its holder was silent kSeconds (210)
//   I7 the epilogue draws: a failed build or upload is tried again (263, 194)
//   I8 at the end every backend was deleted or went with its context

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "vocem/atlas_owner.h"

namespace {

using vocem::AtlasWorker;
using vocem::HandOver;
using vocem::UploadRetry;
using Holding = HandOver::Holding;
using Presenter = HandOver::Presenter;

// ---- the stub build ----------------------------------------------------------

std::mutex g_stub_lock;
std::condition_variable g_stub_wake;
bool g_go = false;
std::atomic<bool> g_inside{false};
// What fonts().pixel_size is to the paths: nonzero once an atlas exists.
std::atomic<int> g_atlas{0};
int g_atlas_serial = 0;

void stub_rasterise(const AtlasWorker::Job&) {
    g_inside.store(true, std::memory_order_seq_cst);
    {
        std::unique_lock<std::mutex> guard(g_stub_lock);
        g_stub_wake.wait(guard, [] { return g_go; });
    }
    g_atlas.store(++g_atlas_serial, std::memory_order_seq_cst);
    g_inside.store(false, std::memory_order_seq_cst);
}

void let_build_finish() {
    std::lock_guard<std::mutex> guard(g_stub_lock);
    g_go = true;
    g_stub_wake.notify_all();
}

void hold_next_build() {
    std::lock_guard<std::mutex> guard(g_stub_lock);
    g_go = false;
}

// ---- the model ---------------------------------------------------------------

enum Event { P0, P1, T, D0, D1, W, F, U, O, kEvents };
const char* const kNames[kEvents] = {"P0", "P1", "T", "D0", "D1", "W", "F", "U", "O"};

struct Place {
    int k = -1;
    int gen = 0;
    bool operator==(const Place& o) const { return k == o.k && gen == o.gen; }
    bool operator!=(const Place& o) const { return !(*this == o); }
};

struct Backend {
    Place place;
    int imgui = -1;
    int texture = 0;     // the atlas serial its font texture holds
    bool deleted = false;
    bool dropped = false;  // its objects went with its context's death
    bool left = false;
};

struct Model {
    double now = 100.0;
    int gen[2] = {0, 0};
    HandOver hand_over;
    UploadRetry retry;
    Place owner;
    double owner_last = 0.0;  // kept by the model itself, for I6
    std::vector<Backend> backends;
    int live = -1;
    std::vector<bool> imgui_alive;
    int live_imgui = -1;
    bool fail_build = false;
    bool fail_upload = false;
    bool off_pending = false;
    int drawn = 0;
    bool last_drew = false;

    const char* failure = nullptr;
    void require(bool condition, const char* what) {
        if (!condition && !failure) {
            failure = what;
        }
    }

    Place place(int k) const { return Place{k, gen[k]}; }

    void destroy_imgui(int id) {
        require(!g_inside.load(),
                "I3 an ImGui context destroyed while the worker is inside the build");
        imgui_alive[static_cast<size_t>(id)] = false;
    }

    // A backend's objects deleted with `current` current.
    void delete_backend(Backend& b, const Place& current) {
        require(!b.deleted, "I2 a backend deleted twice");
        require(b.place == current, "I2 a backend's objects deleted in another context");
        b.deleted = true;
    }

    // Every way the holder is given up (the paths' release()): the worker
    // joined first, then the backend in its context if that is current here.
    void release(bool api_current, const Place& current) {
        let_build_finish();
        vocem::atlas_worker().join();
        require(!g_inside.load(), "I3 released while the worker is inside the build");
        if (hand_over.holding() == Holding::Ready && live >= 0) {
            Backend& b = backends[static_cast<size_t>(live)];
            // In the model a dying context is always made current, so a live
            // backend released without GL is one left in a living context.
            require(api_current, "I2 a live backend dropped in a context that lives on");
            if (api_current) {
                delete_backend(b, current);
            }
        }
        live = -1;
        hand_over.let_go();
        owner = Place{};
        retry = UploadRetry();
        if (live_imgui >= 0) {
            destroy_imgui(live_imgui);
            live_imgui = -1;
        }
        hold_next_build();
    }

    // The hand-over (GL's move_away): the backend and its ImGui context stay
    // in their context, to be deleted there later; everything else goes.
    void move_away(const Place& current) {
        let_build_finish();
        vocem::atlas_worker().join();
        if (hand_over.holding() == Holding::Ready && live >= 0) {
            backends[static_cast<size_t>(live)].left = true;
            live = -1;
            live_imgui = -1;  // kept by the left backend, not destroyed
        }
        release(false, current);
    }

    void tear_down_left(Backend& b, const Place& current) {
        delete_backend(b, current);
        destroy_imgui(b.imgui);
        b.left = false;
    }

    void present(int k) {
        now += 0.1;
        const Place current = place(k);

        // A backend left here goes, properly -- never while the worker builds,
        // and never by waiting for it inside a present (entry 262).
        if (!vocem::atlas_worker().building()) {
            for (Backend& b : backends) {
                if (b.left && b.place == current) {
                    tear_down_left(b, current);
                }
            }
        }

        if (off_pending) {
            // Switched off, in whichever context presents: the holder's
            // objects are deleted only where they live (entry 237's rule).
            off_pending = false;
            if (hand_over.holding() == Holding::Ready && owner != current) {
                move_away(current);
            } else {
                release(owner == current, current);
            }
            require(!g_inside.load(),
                    "I3 the atlas handed back while the worker is inside the build");
            g_atlas.store(0);
            last_drew = false;
            return;
        }

        if (hand_over.held()) {
            const Presenter who = hand_over.present(owner == current, now);
            if (who == Presenter::Owner) {
                owner_last = now;
            }
            if (who == Presenter::Abandoned) {
                require(now - owner_last >= HandOver::kSeconds - 1e-9,
                        "I6 the backend moved while its holder was presenting");
            }
            if (who == Presenter::Foreign) {
                last_drew = false;
                return;
            }
            if (who == Presenter::Abandoned) {
                move_away(current);
            }
        }

        if (!hand_over.held()) {
            require(!g_inside.load(), "I4 a backend built while the worker writes the atlas");
            if (live_imgui < 0) {
                imgui_alive.push_back(true);
                live_imgui = static_cast<int>(imgui_alive.size()) - 1;
            }
            owner = current;
            owner_last = now;
            if (fail_build) {
                fail_build = false;
                hand_over.take(Holding::Failed, now);
                last_drew = false;
                return;
            }
            for (const Backend& b : backends) {
                require(b.deleted || b.dropped || b.left,
                        "I1 a second live backend");
            }
            Backend b;
            b.place = current;
            b.imgui = live_imgui;
            b.texture = g_atlas.load();
            backends.push_back(b);
            live = static_cast<int>(backends.size()) - 1;
            hand_over.take(Holding::Ready, now);
        }
        if (hand_over.holding() == Holding::Failed) {
            last_drew = false;
            return;
        }

        // The first atlas: the worker's, joined here, uploaded whole.
        static const std::string kNoFile;
        bool whole = false;
        switch (vocem::atlas_worker().step(g_atlas.load() == 0, 16.0f, 16.0f, kNoFile, kNoFile,
                                           nullptr)) {
            case AtlasWorker::Step::Started:
                // Until the build is really underway, so I3 can see it.
                while (!g_inside.load() && vocem::atlas_worker().building()) {
                    sched_yield();
                }
                last_drew = false;
                return;
            case AtlasWorker::Step::Building:
                last_drew = false;
                return;
            case AtlasWorker::Step::NoThread:
                require(false, "a thread is always to be had here");
                return;
            case AtlasWorker::Step::Finished:
                require(g_atlas.load() != 0, "the worker finished with no atlas");
                hold_next_build();
                whole = true;
                break;
            case AtlasWorker::Step::Idle:
                break;
        }
        Backend& b = backends[static_cast<size_t>(live)];
        if (whole || retry.due(now)) {
            if (fail_upload) {
                fail_upload = false;
                retry.failed(now);
            } else {
                b.texture = g_atlas.load();
                retry.succeeded();
            }
        }
        if (retry.owed()) {
            last_drew = false;
            return;
        }

        require(!b.deleted && !b.dropped && !b.left, "I5 drawn with a backend that is gone");
        require(b.place == current, "I5 drawn with a backend of another context");
        require(b.imgui >= 0 && imgui_alive[static_cast<size_t>(b.imgui)],
                "I5 drawn with a destroyed ImGui context");
        require(b.texture != 0 && b.texture == g_atlas.load(),
                "I5 drawn with a font texture that does not hold the atlas");
        require(!g_inside.load(), "I5 drawn while the worker writes the atlas");
        ++drawn;
        last_drew = true;
    }

    // The context dies; the teardown makes it current for the purpose (the
    // GLX pbuffer, eglMakeCurrent) and a new context takes its number.
    void destroy(int k) {
        const Place dying = place(k);
        bool any_left = false;
        for (const Backend& b : backends) {
            any_left = any_left || (b.left && b.place == dying);
        }
        if (any_left) {
            // Waited for, not joined: the live backend's next present joins
            // it and uploads what it built (entry 262).
            let_build_finish();
            vocem::atlas_worker().wait_until_built();
            for (Backend& b : backends) {
                if (b.left && b.place == dying) {
                    tear_down_left(b, dying);
                }
            }
        }
        if (hand_over.held() && owner == dying) {
            release(true, dying);
        }
        for (Backend& b : backends) {
            if (b.place == dying && !b.deleted) {
                require(!b.left && static_cast<int>(&b - backends.data()) != live,
                        "I8 a context died holding a backend nobody deleted");
                b.dropped = true;
            }
        }
        ++gen[k];
    }

    void apply(int event) {
        switch (event) {
            case P0: present(0); break;
            case P1: present(1); break;
            case T: now += 2.5; break;
            case D0: destroy(0); break;
            case D1: destroy(1); break;
            case W:
                let_build_finish();
                while (vocem::atlas_worker().building()) {
                    sched_yield();
                }
                hold_next_build();
                break;
            case F: fail_build = true; break;
            case U: fail_upload = true; break;
            case O: off_pending = true; break;
        }
    }

    // The overlay must come back by itself: a failure or a silent holder
    // never keeps it away. Presented from the context a failed holder is not.
    void epilogue() {
        fail_build = false;
        fail_upload = false;
        off_pending = false;
        const int k =
            hand_over.holding() == Holding::Failed && owner.k >= 0 ? 1 - owner.k : 0;
        const int steps[] = {T, P0 + k, W, P0 + k, T, P0 + k, P0 + k};
        for (int e : steps) {
            apply(e);
        }
        require(last_drew, "I7 the overlay did not come back");
        apply(D0);
        apply(D1);
        let_build_finish();
        vocem::atlas_worker().join();
        hold_next_build();
        require(!hand_over.held(), "I8 somebody still holds the backend with both contexts gone");
        for (const Backend& b : backends) {
            require(b.deleted || b.dropped, "I8 a backend nobody deleted");
        }
        for (size_t i = 0; i < imgui_alive.size(); ++i) {
            require(!imgui_alive[i], "I8 an ImGui context nobody destroyed");
        }
    }
};

// Each sequence starts from a process with no atlas and no worker running.
const char* run(const int* events, int length, int* drawn) {
    g_atlas.store(0);
    hold_next_build();
    Model model;
    for (int i = 0; i < length && !model.failure; ++i) {
        model.apply(events[i]);
    }
    if (!model.failure) {
        model.epilogue();
    }
    let_build_finish();
    vocem::atlas_worker().join();
    *drawn += model.drawn;
    return model.failure;
}

// The pure half, asked directly: the transition table and the retry policy.
int check_units() {
    int failures = 0;
    auto check = [&](bool condition, const char* what) {
        printf("%s %s\n", condition ? "ok  " : "FAIL", what);
        failures += condition ? 0 : 1;
    };
    HandOver h;
    check(!h.held() && h.seen() == 0.0, "nobody holds the backend at first");
    h.take(Holding::Ready, 10.0);
    check(h.present(true, 10.5) == Presenter::Owner && h.seen() == 10.5,
          "the holder's present is the owner's and restarts its clock");
    check(h.present(false, 12.4) == Presenter::Foreign,
          "somebody else 1.9 s after the holder is foreign");
    check(h.present(false, 12.5) == Presenter::Abandoned,
          "somebody else 2.0 s after the holder finds it abandoned");
    check(h.first_word_with(&h, nullptr) && !h.first_word_with(&h, nullptr),
          "a foreign presenter is told once");
    check(h.first_word_with(&check, nullptr) && h.first_word_with(&h, nullptr),
          "and told again when two alternate");
    h.let_go();
    check(!h.held() && h.seen() == 0.0 && h.first_word_with(&h, nullptr),
          "letting go clears the holder, its clock and who was told");
    h.take(Holding::Failed, 20.0);
    check(h.held() && h.present(false, 21.0) == Presenter::Foreign,
          "a failure holds the backend like a success (entry 263)");
    check(h.present(false, 22.0) == Presenter::Abandoned,
          "and gives it up when its context falls silent");
    UploadRetry r;
    check(!r.owed() && !r.due(5.0), "no upload is owed at first");
    r.failed(5.0);
    check(r.owed() && !r.due(5.9) && r.due(6.0), "a failed upload is due a second later");
    r.succeeded();
    check(!r.owed(), "and owed no more once one works");
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const int unit_failures = check_units();
    int failures = 0;
    vocem::atlas_worker().set_rasterise(&stub_rasterise);

    // Every sequence of length 0..kLength over the nine events. The bound is
    // the argument when given (a mutation hunt wants it short).
    int length = argc > 1 ? atoi(argv[1]) : 6;
    if (length < 0 || length > 8) {
        length = 6;
    }
    long sequences = 0;
    int drawn = 0;
    int events[8] = {0};
    for (int n = 0; n <= length && failures == 0; ++n) {
        long total = 1;
        for (int i = 0; i < n; ++i) {
            total *= kEvents;
        }
        for (long index = 0; index < total; ++index) {
            long rest = index;
            for (int i = 0; i < n; ++i) {
                events[i] = static_cast<int>(rest % kEvents);
                rest /= kEvents;
            }
            ++sequences;
            if (const char* failure = run(events, n, &drawn)) {
                printf("FAIL %s, after:", failure);
                for (int i = 0; i < n; ++i) {
                    printf(" %s", kNames[events[i]]);
                }
                printf(" (then the epilogue)\n");
                ++failures;
                break;
            }
        }
    }
    if (failures == 0) {
        printf("ok   %ld sequences of up to %d events, %d frames drawn, every invariant held\n",
               sequences, length, drawn);
    }
    return failures + unit_failures == 0 ? 0 : 1;
}
