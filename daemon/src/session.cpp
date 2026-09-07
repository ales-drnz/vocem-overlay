// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "session.h"

#include "log.h"
#include "text.h"
#include "vocem/clock.h"

namespace vocem {

// The cut that backs off to a UTF-8 sequence boundary lives in
// vocem/shared_state.h, beside the capacities it cuts to, because the note
// segment needs the same one and had a byte-boundary snprintf instead.

void Session::set_status(DaemonStatus value) {
    status_ = value;
    publish();
}

void Session::set_connected(bool value) {
    connected_ = value;
    status_ = value ? DaemonStatus::Connected : DaemonStatus::WaitingForDiscord;
    if (!value) {
        participants_.clear();
        order_.clear();
        channel_name_.clear();
        in_channel_ = false;
        ceiling_said_ = false;
    }
    publish();
}

void Session::leave_channel() {
    participants_.clear();
    order_.clear();
    channel_name_.clear();
    in_channel_ = false;
    ceiling_said_ = false;
    publish();
}

void Session::enter_channel(const std::string& name) {
    participants_.clear();
    order_.clear();
    channel_name_ = sanitise_text(name);
    in_channel_ = true;
    ceiling_said_ = false;
    publish();
}

void Session::upsert(uint64_t id, const std::string& raw_name, const std::string& avatar_hash,
                     bool muted, bool deafened) {
    const std::string name = sanitise_text(raw_name);
    auto it = participants_.find(id);
    if (it == participants_.end()) {
        Participant participant;
        participant.id = id;
        participant.name = name;
        participant.avatar_hash = avatar_hash;
        participant.muted = muted;
        participant.deafened = deafened;
        participant.is_self = (id == self_id_);
        // A ceiling on what one channel can make this process hold. Every
        // id here came off the socket, and nothing removes one until the
        // peer says so: a peer emitting a fresh id per message grew both
        // containers without limit and made publish(), which walks `order_`
        // on every event, quadratic in the number of ids ever seen. The
        // segment carries kMaxUsers; a few times that is room for a channel
        // churning and still a bound. Said in the log once per channel: a
        // person the panel silently stops showing is otherwise a report
        // nobody can investigate.
        if (order_.size() >= kParticipantCeiling) {
            if (!ceiling_said_) {
                ceiling_said_ = true;
                LOG("participant ceiling reached (%zu ids in '%s'): further joiners are not "
                    "shown",
                    order_.size(), channel_name_.c_str());
            }
            return;
        }
        participants_.emplace(id, participant);
        order_.push_back(id);
        avatars_.request(id, avatar_hash);
    } else {
        if (!name.empty()) {
            it->second.name = name;
        }
        if (it->second.avatar_hash != avatar_hash) {
            it->second.avatar_hash = avatar_hash;
            avatars_.request(id, avatar_hash);
        }
        it->second.muted = muted;
        it->second.deafened = deafened;
    }
    publish();
}

void Session::remove(uint64_t id) {
    participants_.erase(id);
    for (size_t i = 0; i < order_.size(); ++i) {
        if (order_[i] == id) {
            order_.erase(order_.begin() + static_cast<long>(i));
            break;
        }
    }
    publish();
}

void Session::notify(uint64_t user_id, const std::string& title, const std::string& raw_body,
                     const std::string& avatar_hash) {
    const std::string body = sanitise_text(raw_body);
    ++notification_serial_;
    notification_user_ = user_id;
    notification_title_ = sanitise_text(title);
    notification_avatar_ = avatar_hash;
    notification_received_ = monotonic_seconds();
    // The words go to the note segment, never to the state segment: the
    // state segment is mapped by every graphical process on the machine,
    // and the note is opened only by a process that is about to draw this
    // very toast (vocem/note.h says the whole of it).
    note_.publish(notification_serial_, body.c_str());
    note_cleared_ = false;
    // The author's picture may be someone not in the voice channel, so it has to
    // be requested here rather than relying on the participant list.
    avatars_.request(user_id, avatar_hash);
    publish();
}

void Session::expire_note(double seconds) {
    if (notification_serial_ == 0 || note_cleared_) {
        return;
    }
    // A second of margin over the drawing side's own fade, so the words
    // never leave while the toast is still on somebody's screen.
    if (monotonic_seconds() - notification_received_ < seconds + 1.0) {
        return;
    }
    note_cleared_ = true;
    note_.clear();
}

void Session::set_display_height(uint32_t value) {
    if (display_height_ == value) {
        return;
    }
    display_height_ = value;
    publish();
}

void Session::set_speaking(uint64_t id, bool speaking) {
    auto it = participants_.find(id);
    if (it == participants_.end() || it->second.speaking == speaking) {
        return;
    }
    it->second.speaking = speaking;
    publish();
}

namespace {

// A hash the readers will accept, or nothing. The readers judge a hash by
// avatar_hash_is_sane() on the field as they find it, and the field is cut to
// kAvatarHashCapacity - 1 characters on the way in: a hash of forty or more
// hex characters -- which Discord does not issue, so this needs a hostile
// peer -- failed the daemon's own sanity check and was fetched as the default
// face, while its first thirty-nine characters passed the readers' and sent
// them looking for a file under a name that was never written. One rule on
// both sides now: what the daemon publishes is what it judged sane, whole, or
// empty (entry 133).
const std::string& publishable_hash(const std::string& hash) {
    static const std::string none;
    return avatar_hash_is_sane(hash.c_str()) ? hash : none;
}

}  // namespace

void Session::publish() {
    writer_.publish([this](SharedState& state) {
        state.status = static_cast<uint32_t>(status_);
        state.connected = connected_ ? 1u : 0u;
        state.in_channel = in_channel_ ? 1u : 0u;
        copy_string(state.channel_name, kChannelCapacity, channel_name_);

        uint32_t count = 0;
        for (uint64_t id : order_) {
            if (count >= kMaxUsers) {
                break;
            }
            auto it = participants_.find(id);
            if (it == participants_.end()) {
                continue;
            }
            const Participant& participant = it->second;

            User& user = state.users[count];
            user.id = participant.id;
            user._reserved = 0;
            user.flags = 0;
            if (participant.speaking) user.flags |= kFlagSpeaking;
            if (participant.muted) user.flags |= kFlagMuted;
            if (participant.deafened) user.flags |= kFlagDeafened;
            if (participant.is_self) user.flags |= kFlagSelf;
            copy_string(user.name, kNameCapacity, participant.name);
            copy_string(user.avatar_hash, kAvatarHashCapacity,
                        publishable_hash(participant.avatar_hash));
            ++count;
        }
        state.user_count = count;
        state.display_height = display_height_;

        state.notification.serial = notification_serial_;
        state.notification.user_id = notification_user_;
        state.notification.received = notification_received_;
        copy_string(state.notification.title, kNotificationTitleCapacity, notification_title_);
        // Deliberately always empty: the message's text has its own
        // segment now (vocem/note.h). The field stays because removing it
        // would move every offset after it and empty the overlay in every
        // running game for a change nobody asked to feel; the next bump
        // that happens for its own reasons takes it away.
        copy_string(state.notification.body, kNotificationBodyCapacity, std::string());
        copy_string(state.notification.avatar_hash, kAvatarHashCapacity,
                    publishable_hash(notification_avatar_));
    });
}

}  // namespace vocem
