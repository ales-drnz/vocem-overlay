// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Publishing the same state a second time, on the far side of a Flatpak
// sandbox.
//
// A game that is itself a Flatpak cannot see the daemon's POSIX segment, its
// config.ini or its avatar cache: see vocem/flatpak.h for the measurements and
// for the one directory that does cross. This class is the daemon's half of
// that. It finds the sandboxes whose overlay asked to be served, keeps a
// MAP_SHARED mirror of the segment in each, and copies config.ini and the
// avatar files those sandboxes need.
//
// Everything it touches is on the other side of a trust boundary. The directory
// under $XDG_RUNTIME_DIR/app/<id> is writable by the sandboxed application, and
// this process is not sandboxed: it runs as the user, with the user's home
// reachable. So every open below the application's directory is O_NOFOLLOW and
// relative to a directory descriptor, and anything that is not the kind of file
// it should be is refused out loud rather than written through.

#ifndef VOCEM_DAEMON_FLATPAK_BRIDGE_H
#define VOCEM_DAEMON_FLATPAK_BRIDGE_H

#include <cstddef>
#include <string>
#include <vector>

#include "vocem/shared_state.h"

namespace vocem {

class FlatpakBridge {
public:
    FlatpakBridge() = default;
    ~FlatpakBridge();

    FlatpakBridge(const FlatpakBridge&) = delete;
    FlatpakBridge& operator=(const FlatpakBridge&) = delete;

    // Looks for $XDG_RUNTIME_DIR/app. False, once and quietly, on a machine with
    // no Flatpak at all -- which is not a failure and must not read like one.
    bool start();

    // Which sandboxes are asking, now. Cheap enough for a one-second tick: one
    // opendir and one open per application directory.
    void rescan();

    // Fan the segment out. Called after every publish, so what a sandboxed game
    // reads is never staler than what a host game reads.
    void publish(const SharedState& state);

    // The words of one message, or their removal when `body` is null. Written
    // only where the overlay says it is drawing, and taken away at the same
    // moment the daemon unlinks the segment on the host, so the words are in a
    // sandbox for exactly as long as its toast is on screen.
    void publish_note(uint64_t serial, const char* body);

    // config.ini when it moves, and the avatar files the current state names.
    // Off the publish path: these change on a human's timescale.
    void refresh_files(const SharedState& state);

    // The daemon is going. Takes every mirror's name away, which is what
    // StateWriter::unlink_segment() does for the segment and for the same
    // reason: unlinking is how a reader inside a game learns that what it holds
    // is history. Without it a Flatpak game would go on drawing the last channel
    // the daemon ever published, for as long as it ran -- the file stays where
    // it is and every check the reader makes keeps passing.
    void stop();

    size_t served() const { return mirrors_.size(); }

private:
    // What the overlay inside a sandbox says about itself, in the one file it can
    // write where this process can read it. Defined in the implementation: the
    // shape of that file is between the two halves of the bridge and nobody else.
    struct Request;

    struct Mirror {
        std::string id;
        int directory = -1;   // the sandbox's vocem/ directory
        int state_file = -1;
        // What the overlay in that sandbox last said it was doing. The voice
        // state and the faces only go to a sandbox that is actually drawing
        // them; config.ini goes either way, because it is what the overlay
        // reads to decide.
        bool drawing = false;
        // The seqlock's counter, kept here and not read back out of the file.
        // What is in the file is whatever the sandbox last left there.
        uint32_t sequence = 0;
        long long config_mtime = 0;
        // The avatars directory's refusal, said once per sandbox rather than
        // once per tick (mirror_avatars).
        bool avatars_refused = false;
        // Whether the colour emoji bank has been put in this sandbox. Six
        // megabytes that never change, so it is copied once and only into a
        // sandbox the overlay is actually drawing in.
        bool emoji_bank_copied = false;
        // The name of the record already written on the host for this sandbox, so
        // that a tick which learns nothing new writes nothing.
        std::string recorded;
    };

    static bool read_request(int directory, Request& request);
    void write_record_for(Mirror& mirror, const Request& request);
    void close(Mirror& mirror);
    bool state_is_ours(const Mirror& mirror) const;
    bool adopt(const char* id);
    void mirror_config(Mirror& mirror);
    void mirror_avatars(Mirror& mirror, const SharedState& state);
    void mirror_emoji_bank(Mirror& mirror);

    // How many sandboxes this daemon will serve at once. Far above any real
    // machine's count of running Flatpak games, and a bound on what a process
    // that can create directories under $XDG_RUNTIME_DIR/app can make this
    // daemon hold (rescan() says what each one costs).
    static constexpr size_t kMirrorCeiling = 32;

    int applications_ = -1;  // $XDG_RUNTIME_DIR/app
    bool runtime_missing_said_ = false;  // the no-XDG_RUNTIME_DIR refusal, said once
    bool ceiling_said_ = false;          // the mirror ceiling's refusal, said once
    std::vector<Mirror> mirrors_;
};

}  // namespace vocem

#endif  // VOCEM_DAEMON_FLATPAK_BRIDGE_H
