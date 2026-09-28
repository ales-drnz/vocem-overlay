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
// mirror of the segment in a file in each, and copies config.ini and the
// avatar files those sandboxes need.
//
// Asking is not being given. `vocem/request` is written by whatever runs in the
// sandbox, and any Flatpak can write `drawing=1` into it, so that line alone
// never hands over the voice channel, the faces or the words of a message.
// What decides is the host, by application id: the id's exported desktop entry
// says Game, or the user listed the id in `flatpak_apps` -- AND a process of
// the user's is running in a sandbox of that id (flatpak_process.h). The id
// is the directory's name, and a name is not evidence: Flatpak makes the
// directory, but any sandbox holding the xdg-run/app grant can make one too,
// under any name (entry 164). A sandbox that fails either half is adopted all
// the same (it gets config.ini, so its overlay can decide what to say about
// itself) and is published a cleared state and nothing else.
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

    // The daemon is going. Takes every mirror's name away, as
    // StateWriter::unlink_segment() does for the segment: unlinking is how a
    // reader inside a game learns that what it holds is stale, since the file
    // itself keeps passing every check. The faces it copied go too; the emoji
    // bank, which is the package's and nobody's data, stays.
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
        // Whether a process of this application is running now, which is
        // what makes the directory the application its name says
        // (check_running() in the implementation). Asked once per sweep, and
        // only of a mirror both other halves would serve.
        bool running = false;
        // Its absence said, once per stretch of absence.
        bool absence_said = false;
        // The refusal of a sandbox that asks to draw and has no consent, said
        // once per adoption and again only after consent came and went.
        bool refusal_said = false;
        // The voice state, the note, the faces and the emoji bank go where both
        // halves say yes, and nowhere else.
        bool voice() const { return drawing && consented && running; }
        // The seqlock's counter, kept here and not read back out of the file.
        // What is in the file is whatever the sandbox last left there.
        uint32_t sequence = 0;
        // The config.ini version (its mtime) that has arrived in the sandbox,
        // and the one whose failure to arrive has been said: two memories,
        // so that a failed copy is retried and not only said once.
        long long config_mtime = -1;  // none yet: the first sweep decides
        long long config_failure_said = 0;
        // The avatars directory's refusal, said once per sandbox rather than
        // once per tick (mirror_avatars).
        bool avatars_refused = false;
        // Whether faces may be in its avatars directory: set by
        // mirror_avatars(), and at adoption for whatever a daemon before this
        // one left. Cleared by take_faces_back(), which runs whenever the
        // voice channel stops going there.
        bool faces_given = false;
        // Whether this mirror has settled the colour emoji bank: copied, found
        // already there (mirror_emoji_bank), or found missing on the host.
        bool emoji_bank_copied = false;
        // The name of the record already written on the host for this sandbox, so
        // that a tick which learns nothing new writes nothing. The record is one
        // file per sandbox whatever the name (write_record_for), and a rename is
        // said once.
        std::string recorded;
        bool rename_said = false;
        // The publish failure's line, said once per sandbox: publish() runs on
        // every tick.
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
    void check_running(Mirror& mirror);
    // The faces follow the channel: where it no longer goes, they are removed.
    void take_faces_back(Mirror& mirror);
    // The ids of the Flatpak sandboxes with a process running, scanned at most
    // once per rescan() and only when a mirror asks.
    const std::set<std::string>& running_ids();
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
    // rescan() retries adopt() on every one-second tick, and any process of
    // this user's (a sandbox with the xdg-run/app grant included) can
    // `mkdir $XDG_RUNTIME_DIR/app/x`; without this memory one name would put a
    // line a second into the journal for ever. The retry itself must stay: a
    // directory that appears before the game inside it has written its
    // `request` is the ordinary case, and only the SAYING is once. A name is
    // forgotten again when it is adopted, so a sandbox that comes back and
    // fails differently speaks again.
    //
    // Bounded by the same ceiling as the mirrors, and for the same reason: the
    // set is fed by names somebody else chooses. Past the bound the refusals go
    // quiet with one line saying so, which is the honest end of a bounded
    // memory.
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
    std::set<std::string> running_ids_;
    bool running_scanned_ = false;  // this sweep's scan is in running_ids_
    bool proc_refused_said_ = false;
    std::vector<Mirror> mirrors_;
};

}  // namespace vocem

#endif  // VOCEM_DAEMON_FLATPAK_BRIDGE_H
