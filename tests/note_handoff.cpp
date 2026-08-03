// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The Vulkan path's two-step handoff of a message's words.
//
// The GL side reads the note segment inline and `gl_toast_alone` measures the
// result in a real framebuffer. The Vulkan side cannot: its draw() runs inside
// vkQueuePresentKHR, where the layer takes no file work (rule 8), so it records
// which toast it wants and process_uploads() -- the post-present phase -- fetches
// the words for the NEXT frame to draw. That order is easy to get wrong in ways
// no compiler notices: fetching for a serial nobody asked about, holding a
// message's words after its toast ended, or handing draw() a buffer that is
// still empty on every frame.
//
// This walks exactly that sequence against a real note segment, with the same
// NoteReader the layer holds, in a private /dev/shm. It is NOT a frame-level
// witness -- the project has none for Vulkan, and this file says so rather than
// letting the suite imply otherwise -- but it holds the half that is ours: the
// handoff, and the forgetting.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "private_shm.h"
#include "vocem/note.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The layer's two halves, in the order the layer runs them: draw() records the
// serial and copies whatever the previous post-present fetched; process_uploads()
// fetches for the recorded serial, or forgets when nothing wants one.
struct LayerSequence {
    vocem::NoteReader note;
    char note_body[vocem::kNotificationBodyCapacity] = {0};
    uint64_t wanted = 0;

    // Returns what this frame would draw.
    const char* draw(uint64_t serial, bool toast_wanted) {
        if (toast_wanted) {
            wanted = serial;
        } else {
            wanted = 0;
        }
        return note_body;
    }

    void post_present() {
        if (wanted != 0) {
            std::snprintf(note_body, sizeof(note_body), "%s", note.body_for(wanted));
        } else if (note_body[0] != '\0') {
            note.forget();
            std::memset(note_body, 0, sizeof(note_body));
        }
    }
};

}  // namespace

int main() {
    char shm_holds[256] = {0};
    if (!vocem_test::shm_is_private(shm_holds, sizeof(shm_holds))) {
        if (getenv("VOCEM_SANDBOXED")) {
            vocem_test::shm_explain_refusal(shm_holds);
            return 1;
        }
        if (system("command -v bwrap >/dev/null 2>&1") != 0) {
            printf("skip bwrap is not installed, so the private /dev/shm cannot be built\n");
            return 77;
        }
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) {
            printf("FAIL cannot find my own binary\n");
            return 1;
        }
        self[n] = '\0';
        setenv("VOCEM_SANDBOXED", "1", 1);
        execlp("bwrap", "bwrap", "--dev-bind", "/", "/", "--tmpfs", "/dev/shm",
               "--die-with-parent", self, nullptr);
        printf("FAIL could not exec bwrap\n");
        return 1;
    }

    vocem::NoteWriter writer;
    writer.publish(7, "wrote to you");

    LayerSequence layer;

    // Frame 1: the toast is wanted, nothing has been fetched yet. The layer
    // draws the box and the sender; the words are one frame away, which is the
    // accepted cost of keeping the present path free of file work.
    check(layer.draw(7, true)[0] == '\0', "the first frame of a toast draws no words yet");
    layer.post_present();

    // Frame 2 onwards: the words are there, and a repeated frame costs nothing
    // -- body_for returns its cached copy for the same serial.
    check(strcmp(layer.draw(7, true), "wrote to you") == 0,
          "the next frame draws the message");
    layer.post_present();
    check(strcmp(layer.draw(7, true), "wrote to you") == 0, "and every frame after it");
    layer.post_present();

    // The toast ends: the words leave this process's memory, without waiting
    // for another message to displace them.
    layer.draw(7, false);
    layer.post_present();
    check(layer.draw(7, false)[0] == '\0', "when the toast ends the words are forgotten");

    // A second message, and the daemon has already taken the first one away:
    // asking for a serial the segment does not hold must draw nothing rather
    // than the previous message's words.
    writer.clear();
    writer.publish(8, "a second message");
    layer.draw(8, true);
    layer.post_present();
    check(strcmp(layer.draw(8, true), "a second message") == 0, "a new message replaces it");

    writer.clear();
    LayerSequence stale;
    stale.draw(8, true);
    stale.post_present();
    check(stale.draw(8, true)[0] == '\0',
          "and once the daemon has retired a message, a fresh reader gets nothing");

    // A message longer than the field is cut, and the cut must land on a
    // character. The names went through the daemon's UTF-8-aware copy; the body
    // moved into this segment and got a plain snprintf, which cuts on the byte
    // -- so an accent or an emoji straddling byte 191 left half a sequence, and
    // the overlay draws half a sequence as a question mark. Filled so that the
    // last character to fit whole is a two-byte one landing exactly on the edge.
    {
        std::string overlong(vocem::kNotificationBodyCapacity - 2, 'a');
        overlong += "è";  // two bytes: the first fits, the second does not
        overlong += "tail that will not fit";
        writer.clear();
        writer.publish(9, overlong.c_str());

        LayerSequence cut;
        cut.draw(9, true);
        cut.post_present();
        const char* drawn = cut.draw(9, true);
        const size_t length = strlen(drawn);
        check(length > 0, "an over-long message still draws");
        // No truncated sequence: the last byte may not be a continuation byte,
        // and may not be a lead byte whose continuations were cut away.
        bool whole = true;
        if (length > 0) {
            const unsigned char last = static_cast<unsigned char>(drawn[length - 1]);
            if ((last & 0xc0) == 0x80 || (last & 0xc0) == 0xc0) {
                whole = false;
            }
        }
        check(whole, "the cut lands on a character boundary, not inside one");
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
