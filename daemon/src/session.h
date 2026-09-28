// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The voice channel as the daemon knows it, and its publication into the
// segment: who is in the channel, what each of them is doing, which message
// arrived last, and one publish() that writes all of it under the seqlock.
// main.cpp is the loops and nothing else.

#ifndef VOCEM_DAEMON_SESSION_H
#define VOCEM_DAEMON_SESSION_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "avatars.h"
#include "vocem/note.h"
#include "vocem/shared_state.h"
#include "vocem/shm.h"

namespace vocem {

// How many participants one channel may make this process hold. Every id
// comes off the socket and nothing removes one until the peer says so.
constexpr size_t kParticipantCeiling = 4 * kMaxUsers;

struct Participant {
    uint64_t id = 0;
    std::string name;
    std::string avatar_hash;
    bool speaking = false;
    bool muted = false;
    bool deafened = false;
    bool is_self = false;
};

class Session {
public:
    Session(StateWriter& writer, AvatarCache& avatars) : writer_(writer), avatars_(avatars) {}

    void set_status(DaemonStatus value);
    void set_connected(bool value);

    void set_self_id(uint64_t id) { self_id_ = id; }
    uint64_t self_id() const { return self_id_; }
    bool in_channel() const { return in_channel_; }

    void leave_channel();
    void enter_channel(const std::string& name);

    void upsert(uint64_t id, const std::string& raw_name, const std::string& avatar_hash,
                bool muted, bool deafened);
    void remove(uint64_t id);

    void notify(uint64_t user_id, const std::string& title, const std::string& raw_body,
                const std::string& avatar_hash);

    // The toast has outlived its seconds: the words are removed, from what is
    // mapped and from the name, so between messages there is nothing to read
    // anywhere. Called on the daemon's own tick, which is the only clock that
    // knows how long a message is meant to stay -- the drawing side decides
    // the same thing from the same timestamp, independently.
    void expire_note(double seconds);

    // The display's mode height, for the overlay to size itself by. The daemon
    // is the one who reads /sys -- the injected code must not -- and a change
    // (monitor plugged, mode switched) republishes so running games follow.
    void set_display_height(uint32_t value);

    void set_speaking(uint64_t id, bool speaking);

    size_t size() const { return participants_.size(); }

    // The bridge and main() hand the note writer around by reference (its
    // on_publish hook is wired there).
    NoteWriter& note() { return note_; }

private:
    void publish();

    StateWriter& writer_;
    AvatarCache& avatars_;
    std::unordered_map<uint64_t, Participant> participants_;
    std::vector<uint64_t> order_;  // preserves Discord's ordering
    bool ceiling_said_ = false;    // the participant ceiling's once-per-channel log
    std::string channel_name_;
    uint64_t notification_serial_ = 0;
    uint64_t notification_user_ = 0;
    double notification_received_ = 0.0;
    std::string notification_title_;
    NoteWriter note_;
    bool note_cleared_ = true;
    std::string notification_avatar_;
    uint64_t self_id_ = 0;
    DaemonStatus status_ = DaemonStatus::WaitingForDiscord;
    bool connected_ = false;
    bool in_channel_ = false;
    uint32_t display_height_ = 0;
};

}  // namespace vocem

#endif  // VOCEM_DAEMON_SESSION_H
