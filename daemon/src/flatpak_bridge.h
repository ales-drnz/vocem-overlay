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
// Asking is not being given. `vocem/request` is written by whatever runs in the
// sandbox, and any Flatpak can write `drawing=1` into it: through 0.1.10 that
// line alone handed the voice channel, the faces and the words of every message
// to whichever application wrote it. What decides now is the host, by
// application id -- the directory's name, which a sandbox cannot choose: the
// id's exported desktop entry says Game, or the user listed the id in
// `flatpak_apps`. A sandbox that is neither is adopted all the same (it gets
// config.ini, so its overlay can decide what to say about itself) and is
// published a cleared state and nothing else.
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
#include <set>
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
        // What the overlay in that sandbox last said it was doing -- the
        // sandbox's own word, which an honest overlay tells straight and
        // anything else in there can write too. config.ini goes either way,
        // because it is what the overlay reads to decide.
        bool drawing = false;
        // The host's half: whether this application id may be given the voice
        // channel at all (consent_for() in the implementation), and the reason
        // either way, for the log.
        bool consented = false;
        std::string consent_why;
        // The refusal of a sandbox that asks to draw and has no consent, said
        // once per adoption and again only after consent came and went.
        bool refusal_said = false;
        // The voice state, the note, the faces and the emoji bank go where both
        // halves say yes, and nowhere else.
        bool voice() const { return drawing && consented; }
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
        // that a tick which learns nothing new writes nothing. The record is one
        // file per sandbox whatever the name (write_record_for), and a rename is
        // said once.
        std::string recorded;
        bool rename_said = false;
        // The publish failure's line, said once per sandbox: publish() runs on
        // every tick, so a mirror whose file cannot be written was one line a
        // second for as long as the sandbox existed.
        bool publish_refused = false;
    };

    static bool read_request(int directory, Request& request);
    void write_record_for(Mirror& mirror, const Request& request);
    void close(Mirror& mirror);
    bool state_is_ours(const Mirror& mirror) const;
    bool adopt(const char* id);
    // The host's decision for one mirror, and what changes when it moves.
    void decide(Mirror& mirror, bool announce);
    void say_refusal(Mirror& mirror);
    // flatpak_apps, reread when config.ini moves, and every mirror decided
    // again with it.
    void refresh_consent();
    void mirror_config(Mirror& mirror);
    void mirror_avatars(Mirror& mirror, const SharedState& state);
    void mirror_emoji_bank(Mirror& mirror);

    // How many sandboxes this daemon will serve at once. Far above any real
    // machine's count of running Flatpak games, and a bound on what a process
    // that can create directories under $XDG_RUNTIME_DIR/app can make this
    // daemon hold (rescan() says what each one costs).
    static constexpr size_t kMirrorCeiling = 32;

    // Whether a refusal about this directory NAME has already been said.
    // rescan() retries adopt() on every one-second tick, and two of its
    // refusals had no memory at all: `mkdir $XDG_RUNTIME_DIR/app/x` -- which
    // any process of this user's can do, a sandbox with the xdg-run/app grant
    // included, the grant entry 134 names as what hands a sandbox this power --
    // put one line a second into the journal for ever, from a directory name.
    // The retry itself is unchanged and must be: a directory that appears
    // before the game inside it has written its `request` is the ordinary case,
    // and only the SAYING is once. A name is forgotten again when it is
    // adopted, so a sandbox that comes back and fails differently speaks again.
    //
    // Bounded by the same ceiling as the mirrors, and for the same reason: the
    // set is fed by names somebody else chooses. Past the bound the refusals go
    // quiet with one line saying so, which is the honest end of a bounded
    // memory (entry 126's shape).
    bool say_refusal_once(const char* id);

    int applications_ = -1;  // $XDG_RUNTIME_DIR/app
    bool runtime_missing_said_ = false;  // the no-XDG_RUNTIME_DIR refusal, said once
    bool ceiling_said_ = false;          // the mirror ceiling's refusal, said once
    bool refusals_full_said_ = false;    // the refusal memory's own ceiling, said once
    std::set<std::string> refusals_said_;
    // The user's list of application ids (Config::flatpak_apps) and the
    // config.ini modification time it was read at; -1 until the first read.
    std::string flatpak_apps_;
    long long consent_config_mtime_ = -1;
    std::vector<Mirror> mirrors_;
};

}  // namespace vocem

#endif  // VOCEM_DAEMON_FLATPAK_BRIDGE_H
