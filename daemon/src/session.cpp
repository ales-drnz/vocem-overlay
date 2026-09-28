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
// segment needs the same one.

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
        // peer says so: without it a peer emitting a fresh id per message
        // grows both containers without limit and makes publish(), which
        // walks `order_` on every event, quadratic in the number of ids ever
        // seen. The segment carries kMaxUsers; a few times that is room for a
        // channel churning and still a bound. Said in the log once per
        // channel: a person the panel silently stops showing is otherwise a
        // report nobody can investigate.
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
    // And the sender with the words. `vocem/note.h` promises that "between
    // messages there is nothing to read anywhere", and that covers everything
    // around the body too: the serial, the user id, the timestamp, the avatar
    // hash and the title (the sender's display name plus the guild and the
    // channel) would otherwise stand in SharedState, which every GL and Vulkan
    // process of the session maps, until the next message.
    //
    // Safe at exactly this moment and no earlier, which is why it is here
    // rather than on a clock of its own: expire_note() has already waited the
    // toast's seconds plus a second of margin, so no reader is still drawing
    // it, and publish() below is the same write that retires it everywhere.
    // The counter is NOT reset -- `notification_serial_` goes on increasing, or
    // a later message would reuse a serial a reader has already seen and been
    // latched on (note.h's `have_`).
    // messages there is nothing to read anywhere", and that was true of the
    // body and false of everything around it: the serial, the user id, the
    // timestamp, the avatar hash and the TITLE -- Discord's composed string,
    // which is the sender's display name plus the guild and the channel -- went
    // on standing in SharedState, which every GL and Vulkan process of the
    // session maps as a matter of course, until the next message replaced them
    // or the daemon stopped. Nothing zeroed the slot: not on expiry, not on
    // set_connected(false).
    //
    // Safe at exactly this moment and no earlier, which is why it is here
    // rather than on a clock of its own: expire_note() has already waited the
    // toast's seconds plus a second of margin, so no reader is still drawing
    // it, and publish() below is the same write that retires it everywhere.
    // The counter is NOT reset -- `notification_serial_` goes on increasing, or
    // a later message would reuse a serial a reader has already seen and been
    // latched on (note.h's `have_`).
    publish();
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
// kAvatarHashCapacity - 1 characters on the way in, so an over-long hash (which
// only a hostile peer sends) could fail the daemon's check while its truncated
// prefix passed the readers'. One rule on both sides: what the daemon publishes
// is what it judged sane, whole, or empty (entry 133).
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

        // A retired message leaves nothing behind: an expired note means the
        // whole slot goes, not only the words (expire_note says why here and
        // not sooner). A reader takes serial 0 as "no toast", which is the
        // question notification_wanted() asks first.
        const bool retired = note_cleared_;
        state.notification.serial = retired ? 0 : notification_serial_;
        state.notification.user_id = retired ? 0 : notification_user_;
        state.notification.received = retired ? 0.0 : notification_received_;
        copy_string(state.notification.title, kNotificationTitleCapacity,
                    retired ? std::string() : notification_title_);
        // Deliberately always empty: the message's text has its own segment
        // (vocem/note.h). The field stays because removing it would move
        // every offset after it and empty the overlay in every running game;
        // the next ABI bump that happens for its own reasons takes it away.
        // segment now (vocem/note.h). The field stays because removing it
        // would move every offset after it and empty the overlay in every
        // running game for a change nobody asked to feel; the next bump
        // that happens for its own reasons takes it away.
        copy_string(state.notification.body, kNotificationBodyCapacity, std::string());
        copy_string(state.notification.avatar_hash, kAvatarHashCapacity,
                    retired ? std::string() : publishable_hash(notification_avatar_));
    });
}

}  // namespace vocem
