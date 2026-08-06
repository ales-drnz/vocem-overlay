// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocemd -- the only component that talks to Discord.
//
// It holds the RPC connection, tracks who is in the voice channel and who is
// speaking, and publishes that into shared memory for the in-game layer to read.
// Nothing here runs inside a game process, which is the whole point: the code
// that does I/O and parses JSON is kept as far away from the render loop as
// possible.
//
// Authorisation is its own: the daemon asks Discord for a token the first time it
// connects and stores it. See auth.h for how, and for what that costs us.

#include <time.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth.h"
#include "avatars.h"
#include "flatpak_bridge.h"
#include "log.h"
#include "vocem/clock.h"
#include "vocem/display.h"
#include "vocem/journal.h"
#include "vocem/live_config.h"
#include "vocem/note.h"
#include "vocem/shared_state.h"
#include "vocem/shm.h"
#include "websocket.h"

using json = nlohmann::json;

namespace {

constexpr const char* kOrigin = "http://localhost:3000";
// Discord's client takes the first free port in this range, "trying sequentially
// until it can bind to one", and tells clients to check the same way. Only 6463
// was ever tried: with anything else holding it, Discord ran on 6464 and the
// overlay said "Waiting for Discord" for ever, with nothing in the log.
constexpr uint16_t kRpcPortFirst = 6463;
constexpr uint16_t kRpcPortLast = 6472;

// How many participants one channel may make this process hold. Every id comes
// off the socket and nothing removes one until the peer says so.
constexpr size_t kParticipantCeiling = 4 * vocem::kMaxUsers;

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

// Discord sends explicit nulls for absent fields -- `evt` on command replies,
// `nick` and `global_name` on users. nlohmann's value() throws on a present-but-
// null field, so every read goes through these instead.
std::string str_field(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

bool bool_field(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

// Discord wraps every name it interpolates into a sentence in Unicode's
// bidirectional isolates, so that a right-to-left name cannot scramble the text
// around it: a notification title arrives as
// "⁨Lele⁩ (⁨Chilling⁩, ⁨Canali vocali⁩)". Read out
// of the owner's own journal, where the daemon had written it down.
//
// Those characters are invisible by definition, and no font in the overlay's
// atlas has a glyph for one -- measured: Inter Regular, Inter SemiBold and Noto
// Sans JP have none of them in their cmap, and ImGui draws its FallbackChar,
// which is '?'. Six isolates in that title are six question marks in the box, on
// the owner's screen, which is how this was found.
//
// They are removed here rather than given blank glyphs in the atlas, for two
// reasons. The atlas lives inside somebody's game and this does not; and the
// isolates exist to drive the Unicode bidirectional algorithm, which ImGui does
// not implement at all -- it draws left to right, always -- so in this overlay
// they can never do anything but take up a glyph.
//
// Only the bidirectional formatting characters, listed by hand rather than by
// category. "Every default-ignorable code point" would sweep up the zero-width
// joiner and the variation selectors, and entry 27 put those in the atlas on
// purpose: without them every emoji that carries one draws a question mark of
// its own.
std::string without_bidi_marks(const std::string& source) {
    // U+061C ARABIC LETTER MARK; U+200E..U+200F the two directional marks;
    // U+202A..U+202E the embeddings and overrides; U+2066..U+2069 the isolates.
    static const uint32_t kMarks[] = {0x061C, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C,
                                      0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069};
    std::string out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        const unsigned char lead = static_cast<unsigned char>(source[i]);
        size_t length = 1;
        uint32_t code = lead;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07u;
        }
        if (i + length > source.size()) {
            // A truncated sequence: copy the byte and move on rather than read
            // past the end. Nothing here is the place to repair broken UTF-8.
            out.push_back(source[i]);
            ++i;
            continue;
        }
        for (size_t k = 1; k < length; ++k) {
            code = (code << 6) | (static_cast<unsigned char>(source[i + k]) & 0x3Fu);
        }
        bool drop = false;
        for (uint32_t mark : kMarks) {
            if (code == mark) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            out.append(source, i, length);
        }
        i += length;
    }
    return out;
}

// The cut that backs off to a UTF-8 sequence boundary lives in
// vocem/shared_state.h, beside the capacities it cuts to, because the note
// segment needs the same one and had a byte-boundary snprintf instead.
using vocem::copy_string;

// ---------------------------------------------------------------------------
// Voice channel model
// ---------------------------------------------------------------------------

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
    Session(vocem::StateWriter& writer, vocem::AvatarCache& avatars)
        : writer_(writer), avatars_(avatars) {}

    void set_status(vocem::DaemonStatus value) {
        status_ = value;
        publish();
    }

    void set_connected(bool value) {
        connected_ = value;
        status_ = value ? vocem::DaemonStatus::Connected
                        : vocem::DaemonStatus::WaitingForDiscord;
        if (!value) {
            participants_.clear();
            order_.clear();
            channel_name_.clear();
            in_channel_ = false;
        }
        publish();
    }

    void set_self_id(uint64_t id) { self_id_ = id; }
    uint64_t self_id() const { return self_id_; }
    bool in_channel() const { return in_channel_; }

    void leave_channel() {
        participants_.clear();
        order_.clear();
        channel_name_.clear();
        in_channel_ = false;
        publish();
    }

    void enter_channel(const std::string& name) {
        participants_.clear();
        order_.clear();
        channel_name_ = without_bidi_marks(name);
        in_channel_ = true;
        publish();
    }

    void upsert(uint64_t id, const std::string& raw_name, const std::string& avatar_hash,
                bool muted, bool deafened) {
        const std::string name = without_bidi_marks(raw_name);
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
            // churning and still a bound.
            if (order_.size() >= kParticipantCeiling) {
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

    void remove(uint64_t id) {
        participants_.erase(id);
        for (size_t i = 0; i < order_.size(); ++i) {
            if (order_[i] == id) {
                order_.erase(order_.begin() + static_cast<long>(i));
                break;
            }
        }
        publish();
    }

    void notify(uint64_t user_id, const std::string& title, const std::string& raw_body,
                const std::string& avatar_hash) {
        const std::string body = without_bidi_marks(raw_body);
        ++notification_serial_;
        notification_user_ = user_id;
        notification_title_ = without_bidi_marks(title);
        notification_avatar_ = avatar_hash;
        notification_received_ = vocem::monotonic_seconds();
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

    // The toast has outlived its seconds: the words are removed, from what is
    // mapped and from the name, so between messages there is nothing to read
    // anywhere. Called on the daemon's own tick, which is the only clock that
    // knows how long a message is meant to stay -- the drawing side decides
    // the same thing from the same timestamp, independently.
    void expire_note(double seconds) {
        if (notification_serial_ == 0 || note_cleared_) {
            return;
        }
        // A second of margin over the drawing side's own fade, so the words
        // never leave while the toast is still on somebody's screen.
        if (vocem::monotonic_seconds() - notification_received_ < seconds + 1.0) {
            return;
        }
        note_cleared_ = true;
        note_.clear();
    }

    // The display's mode height, for the overlay to size itself by. The daemon
    // is the one who reads /sys -- the injected code must not -- and a change
    // (monitor plugged, mode switched) republishes so running games follow.
    void set_display_height(uint32_t value) {
        if (display_height_ == value) {
            return;
        }
        display_height_ = value;
        publish();
    }

    void set_speaking(uint64_t id, bool speaking) {
        auto it = participants_.find(id);
        if (it == participants_.end() || it->second.speaking == speaking) {
            return;
        }
        it->second.speaking = speaking;
        publish();
    }

    size_t size() const { return participants_.size(); }

private:
    void publish() {
        writer_.publish([this](vocem::SharedState& state) {
            state.status = static_cast<uint32_t>(status_);
            state.connected = connected_ ? 1u : 0u;
            state.in_channel = in_channel_ ? 1u : 0u;
            copy_string(state.channel_name, vocem::kChannelCapacity, channel_name_);

            uint32_t count = 0;
            for (uint64_t id : order_) {
                if (count >= vocem::kMaxUsers) {
                    break;
                }
                auto it = participants_.find(id);
                if (it == participants_.end()) {
                    continue;
                }
                const Participant& participant = it->second;

                vocem::User& user = state.users[count];
                user.id = participant.id;
                user._reserved = 0;
                user.flags = 0;
                if (participant.speaking) user.flags |= vocem::kFlagSpeaking;
                if (participant.muted) user.flags |= vocem::kFlagMuted;
                if (participant.deafened) user.flags |= vocem::kFlagDeafened;
                if (participant.is_self) user.flags |= vocem::kFlagSelf;
                copy_string(user.name, vocem::kNameCapacity, participant.name);
                copy_string(user.avatar_hash, vocem::kAvatarHashCapacity, participant.avatar_hash);
                ++count;
            }
            state.user_count = count;
            state.display_height = display_height_;

            state.notification.serial = notification_serial_;
            state.notification.user_id = notification_user_;
            state.notification.received = notification_received_;
            copy_string(state.notification.title, vocem::kNotificationTitleCapacity,
                        notification_title_);
            // Deliberately always empty: the message's text has its own
            // segment now (vocem/note.h). The field stays because removing it
            // would move every offset after it and empty the overlay in every
            // running game for a change nobody asked to feel; the next bump
            // that happens for its own reasons takes it away.
            copy_string(state.notification.body, vocem::kNotificationBodyCapacity,
                        std::string());
            copy_string(state.notification.avatar_hash, vocem::kAvatarHashCapacity,
                        notification_avatar_);
        });
    }

    vocem::StateWriter& writer_;
    vocem::AvatarCache& avatars_;
    std::unordered_map<uint64_t, Participant> participants_;
    std::vector<uint64_t> order_;  // preserves Discord's ordering
    std::string channel_name_;
    uint64_t notification_serial_ = 0;
    uint64_t notification_user_ = 0;
    double notification_received_ = 0.0;
    std::string notification_title_;
    vocem::NoteWriter note_;

public:
    vocem::NoteWriter& note() { return note_; }

private:
    bool note_cleared_ = true;
    std::string notification_avatar_;
    uint64_t self_id_ = 0;
    vocem::DaemonStatus status_ = vocem::DaemonStatus::WaitingForDiscord;
    bool connected_ = false;
    bool in_channel_ = false;
    uint32_t display_height_ = 0;
};

// ---------------------------------------------------------------------------
// RPC client
// ---------------------------------------------------------------------------

class RpcClient {
public:
    RpcClient(vocem::WebSocket& socket, Session& session, std::string token)
        : socket_(socket), session_(session), token_(std::move(token)) {}

    void send(const json& message) { socket_.send_text(message.dump()); }

    void authenticate() {
        send({{"cmd", "AUTHENTICATE"}, {"args", {{"access_token", token_}}}, {"nonce", "auth"}});
    }

    // Asks the Discord client to authorise us; it shows the user a prompt and
    // replies with a short-lived code. "prompt": "none" lets Discord skip the
    // dialog when the user has already approved this application.
    void authorize() {
        LOG("requesting authorisation -- accept the prompt in Discord");
        session_.set_status(vocem::DaemonStatus::Authorising);
        send({{"cmd", "AUTHORIZE"},
              {"args",
               {{"client_id", vocem::kClientId},
                {"scopes", {"rpc", "messages.read", "rpc.notifications.read"}},
                {"prompt", "none"}}},
              {"nonce", "authorize"}});
    }

    void subscribe(const char* event, json args = json::object()) {
        send({{"cmd", "SUBSCRIBE"}, {"args", args}, {"evt", event}, {"nonce", event}});
    }

    void unsubscribe(const char* event, const json& args) {
        send({{"cmd", "UNSUBSCRIBE"}, {"args", args}, {"evt", event}, {"nonce", event}});
    }

    void query_selected_channel() {
        send({{"cmd", "GET_SELECTED_VOICE_CHANNEL"}, {"args", json::object()}, {"nonce", "chan"}});
    }

    // The channel the event named, rather than "whichever one is selected now".
    // VOICE_CHANNEL_SELECT carries the id it is about; asking a second, different
    // question means the answer can be the channel we were in a moment ago, and
    // then the daemon resubscribes to the one it was just told we had left.
    // GET_CHANNEL returns the same shape GET_SELECTED_VOICE_CHANNEL does -- name,
    // voice_states -- so adopt_channel handles either.
    void query_channel(const std::string& id) {
        send({{"cmd", "GET_CHANNEL"}, {"args", {{"channel_id", id}}}, {"nonce", "chan-id"}});
    }

    // Asked on a slow tick while connected, and the reason the overlay can no
    // longer be left describing a channel nobody is in. Every event that says
    // "you moved" is an optimisation on top of this: the daemon's idea of where
    // you are used to come from one event and was never checked again, so a move
    // Discord did not announce -- being dragged into another channel by somebody
    // else -- left the old channel's name and the old channel's people on screen
    // until the daemon was restarted. Silence is not success.
    //
    // Free when nothing changed: the reply adopts only when the id differs, so
    // the participants and their speaking flags are not rebuilt every few
    // seconds.
    void reconcile_channel() { query_selected_channel(); }

    void handle(const json& message) {
        const std::string command = str_field(message, "cmd");
        const std::string event = str_field(message, "evt");
        const json data = message.contains("data") ? message["data"] : json();

        if (event == "READY") {
            DBG("rpc ready");
            if (token_.empty()) {
                authorize();
            } else {
                authenticate();
            }
            return;
        }

        if (command == "AUTHORIZE") {
            if (event == "ERROR") {
                // Usually the user declined; retrying would prompt them in a loop.
                LOG("authorisation refused: %s", data.dump().c_str());
                session_.set_status(vocem::DaemonStatus::AuthorisationRefused);
                failed_ = true;
                return;
            }
            const std::string code = str_field(data, "code");
            if (code.empty()) {
                LOG("authorisation reply carried no code");
                failed_ = true;
                return;
            }
            // Blocking, once: there is nothing else to do until it finishes.
            token_ = vocem::exchange_code_for_token(code);
            if (token_.empty()) {
                session_.set_status(vocem::DaemonStatus::AuthorisationRefused);
                failed_ = true;
                return;
            }
            if (!vocem::save_token(token_)) {
                LOG("could not store the token; authorisation will be requested again");
            }
            LOG("authorised");
            authenticate();
            return;
        }

        if (command == "AUTHENTICATE") {
            if (event == "ERROR") {
                // A stored token can be revoked or expire. Discard it and ask once
                // more; only give up if authorisation fails too.
                if (!token_.empty() && !reauthorised_) {
                    LOG("stored token rejected, requesting authorisation again");
                    vocem::forget_token();
                    token_.clear();
                    reauthorised_ = true;
                    authorize();
                    return;
                }
                LOG("authentication rejected: %s", data.dump().c_str());
                failed_ = true;
                return;
            }
            if (data.contains("user")) {
                const std::string id = str_field(data["user"], "id");
                session_.set_self_id(parse_id(id));
                LOG("authenticated as %s", str_field(data["user"], "username").c_str());
            }
            session_.set_connected(true);
            authenticated_ = true;
            subscribe("VOICE_CHANNEL_SELECT");
            // "sent when the client's voice connection status changes", with no
            // subscription arguments -- the documented event for the case
            // VOICE_CHANNEL_SELECT does not obviously cover. Being moved into
            // another channel makes the client re-establish its voice
            // connection, so the status has to move through AWAITING_ENDPOINT
            // and VOICE_CONNECTING on the way to VOICE_CONNECTED whoever
            // started it.
            subscribe("VOICE_CONNECTION_STATUS");
            // Not tied to a channel: this is every direct message and mention.
            subscribe("NOTIFICATION_CREATE");
            query_selected_channel();
            return;
        }

        if (command == "GET_SELECTED_VOICE_CHANNEL" || command == "GET_CHANNEL") {
            if (event == "ERROR") {
                // A channel we were told about and cannot read: ask the plain
                // question instead of being left believing the old answer.
                DBG("channel query refused: %s", data.dump().c_str());
                if (command == "GET_CHANNEL") {
                    query_selected_channel();
                }
                return;
            }
            if (data.is_null() || data.empty()) {
                if (session_.in_channel()) {
                    DBG("not in a voice channel");
                    unsubscribe_voice();
                    session_.leave_channel();
                }
                return;
            }
            // The reconcile above asks this every few seconds. Adopting an answer
            // that names the channel we are already in would clear the
            // participants and their speaking flags on that same cadence, so the
            // only answer worth acting on is one that names a different channel.
            if (str_field(data, "id") == channel_id_ && session_.in_channel()) {
                return;
            }
            unsubscribe_voice();
            adopt_channel(data);
            return;
        }

        if (event == "VOICE_CHANNEL_SELECT") {
            const std::string channel = str_field(data, "channel_id");
            if (channel.empty()) {
                unsubscribe_voice();
                session_.leave_channel();
            } else if (channel != channel_id_) {
                // Ask about THAT channel, not about whichever one is selected by
                // the time the question arrives -- query_channel says why.
                query_channel(channel);
            }
            return;
        }

        if (event == "VOICE_STATE_CREATE" || event == "VOICE_STATE_UPDATE") {
            apply_voice_state(data);
            return;
        }

        if (event == "VOICE_STATE_DELETE") {
            if (data.contains("user")) {
                const uint64_t id = parse_id(str_field(data["user"], "id"));
                session_.remove(id);
                // Ourselves leaving the channel we are subscribed to is the one
                // event that says "you are not where this daemon thinks you are"
                // even when nothing announced a move. Being dragged into another
                // channel by somebody else looks exactly like this from here.
                if (id != 0 && id == session_.self_id()) {
                    DBG("we left the channel we were watching; asking where we are");
                    unsubscribe_voice();
                    query_selected_channel();
                }
            }
            return;
        }

        if (event == "NOTIFICATION_CREATE") {
            apply_notification(data);
            return;
        }

        // The voice connection changing state is the client telling us it has
        // gone somewhere, without saying where. Only a *change* is acted on: the
        // payload carries the last twenty pings and the running average, so this
        // event arrives while nothing is happening at all, and asking Discord a
        // question per ping would be rude. The reply is free when the answer is
        // the channel we already know.
        if (event == "VOICE_CONNECTION_STATUS") {
            const std::string state = str_field(data, "state");
            if (!state.empty() && state != voice_state_) {
                voice_state_ = state;
                DBG("voice connection is now %s; asking where we are", state.c_str());
                query_selected_channel();
            }
            return;
        }

        if (event == "SPEAKING_START" || event == "SPEAKING_STOP") {
            session_.set_speaking(parse_id(str_field(data, "user_id")), event == "SPEAKING_START");
            return;
        }
    }

    bool failed() const { return failed_; }
    bool authenticated() const { return authenticated_; }

private:
    static uint64_t parse_id(const std::string& text) {
        return text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 10);
    }

    // Discord sends the per-guild display name in `nick`, the account-wide one in
    // `global_name`, and the login name in `username`. That is the precedence the
    // client itself shows, so it is the precedence used here.
    static std::string display_name(const json& entry) {
        const std::string nick = str_field(entry, "nick");
        if (!nick.empty()) {
            return nick;
        }
        if (entry.contains("user") && entry["user"].is_object()) {
            const std::string global_name = str_field(entry["user"], "global_name");
            return global_name.empty() ? str_field(entry["user"], "username") : global_name;
        }
        return {};
    }

    void apply_voice_state(const json& entry) {
        if (!entry.contains("user")) {
            return;
        }
        const uint64_t id = parse_id(str_field(entry["user"], "id"));
        if (id == 0) {
            return;
        }

        bool muted = false;
        bool deafened = false;
        if (entry.contains("voice_state") && entry["voice_state"].is_object()) {
            const json& vs = entry["voice_state"];
            // Server mute, self mute and suppress all render as "cannot speak".
            muted = bool_field(vs, "mute") || bool_field(vs, "self_mute") ||
                    bool_field(vs, "suppress");
            deafened = bool_field(vs, "deaf") || bool_field(vs, "self_deaf");
        }
        const std::string avatar_hash = str_field(entry["user"], "avatar");
        session_.upsert(id, display_name(entry), avatar_hash, muted, deafened);
    }

    // The payload carries a ready-made title and body -- what the desktop
    // notification would have said -- plus the message it came from, which is where
    // the author and their avatar are.
    void apply_notification(const json& data) {
        std::string title = str_field(data, "title");
        std::string body = str_field(data, "body");
        uint64_t author_id = 0;
        std::string avatar;

        if (data.contains("message") && data["message"].is_object()) {
            const json& message = data["message"];
            if (message.contains("author") && message["author"].is_object()) {
                const json& author = message["author"];
                author_id = parse_id(str_field(author, "id"));
                avatar = str_field(author, "avatar");
                if (title.empty()) {
                    const std::string global_name = str_field(author, "global_name");
                    title = global_name.empty() ? str_field(author, "username") : global_name;
                }
            }
            if (body.empty()) {
                body = str_field(message, "content");
            }
        }

        if (title.empty() && body.empty()) {
            return;  // nothing worth showing
        }
        // The length, never the words: a toast that arrives with a name and a
        // face and no text is indistinguishable, from the outside, from one
        // whose text was lost on the way to the screen -- and this is the only
        // place that can tell the two apart. `body` is documented at the top
        // level of NOTIFICATION_CREATE, with the message's own `content` as the
        // fallback; if both are empty, Discord sent no text and nothing
        // downstream is at fault.
        DBG("notification from %s: %zu bytes of text (%s)", title.c_str(), body.size(),
            body.empty()               ? "none: Discord sent no body and no message content"
            : str_field(data, "body").empty() ? "from the message's content"
                                              : "from the notification's body");
        {
            // The sender, never the text: the journal is the Debug section's
            // log, and the body's privacy rule (publish()) applies to it too.
            char note[160];
            std::snprintf(note, sizeof(note), "notification from %.100s", title.c_str());
            vocem::journal_note(note);
        }
        session_.notify(author_id, title, body, avatar);
    }

    void adopt_channel(const json& data) {
        channel_id_ = str_field(data, "id");
        session_.enter_channel(str_field(data, "name"));

        if (data.contains("voice_states") && data["voice_states"].is_array()) {
            for (const json& entry : data["voice_states"]) {
                apply_voice_state(entry);
            }
        }
        LOG("in '%s' with %zu participant(s)", str_field(data, "name").c_str(), session_.size());

        for (const char* event : {"VOICE_STATE_CREATE", "VOICE_STATE_UPDATE", "VOICE_STATE_DELETE",
                                 "SPEAKING_START", "SPEAKING_STOP"}) {
            subscribe(event, {{"channel_id", channel_id_}});
        }
    }

    void unsubscribe_voice() {
        if (channel_id_.empty()) {
            return;
        }
        for (const char* event : {"VOICE_STATE_CREATE", "VOICE_STATE_UPDATE", "VOICE_STATE_DELETE",
                                 "SPEAKING_START", "SPEAKING_STOP"}) {
            unsubscribe(event, {{"channel_id", channel_id_}});
        }
        channel_id_.clear();
    }

    vocem::WebSocket& socket_;
    Session& session_;
    std::string token_;
    std::string channel_id_;
    std::string voice_state_;
    bool failed_ = false;
    bool authenticated_ = false;
    bool reauthorised_ = false;
};

}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    vocem::StateWriter writer;
    if (!writer.open()) {
        LOG("could not create the shared state segment");
        return 1;
    }

    // The daemon's own session journal (vocem/journal.h): the Debug section's
    // log of connection life, and -- exactly as for a game -- a `.running`
    // journal whose process is gone is a daemon that died without unwinding.
    vocem::journal_begin("daemon", "vocemd");

    // The second half of the same publication, for games that are themselves
    // Flatpaks and cannot see the segment above. Hung off the writer so there is
    // one publish path: every state the host sees, a served sandbox sees.
    vocem::FlatpakBridge bridge;
    bridge.start();
    bridge.rescan();
    writer.on_publish_context = &bridge;
    writer.on_publish = [](const vocem::SharedState& state, void* context) {
        static_cast<vocem::FlatpakBridge*>(context)->publish(state);
    };

    vocem::AvatarCache avatars;
    Session session(writer, avatars);
    // The message's words take the same road as the state, and are removed from
    // it at the same moment they are removed from the segment.
    session.note().on_publish_context = &bridge;
    session.note().on_publish = [](uint64_t serial, const char* body, void* context) {
        static_cast<vocem::FlatpakBridge*>(context)->publish_note(serial, body);
    };
    // The user's settings, reread on the same live mechanism the overlay uses --
    // one stat() every couple of seconds, a reparse only when the file moved. The
    // daemon consumes exactly one key: whether a message's text may be published
    // into the segment at all.
    vocem::LiveConfig live_config;
    session.set_display_height(vocem::display_height());
    session.set_connected(false);

    // The bridge's own tick, on the slow clock the sandboxes' needs run on: a
    // game starting is when a new one appears, and config.ini and the avatar
    // files move on a human's timescale. The state itself does not wait for
    // this -- it goes out with every publish. The republish is here so a sandbox
    // that has just been adopted does not sit on an empty state until the next
    // thing Discord says.
    double bridge_checked = 0.0;
    const auto service_bridge = [&] {
        const double now = vocem::monotonic_seconds();
        if (now - bridge_checked < 1.0) {
            return;
        }
        bridge_checked = now;
        bridge.rescan();
        if (const vocem::SharedState* state = writer.state()) {
            bridge.publish(*state);
            bridge.refresh_files(*state);
        }
    };

    const std::string path = std::string("/?v=1&client_id=") + vocem::kClientId;
    int backoff_seconds = 1;
    bool warned_about_token = false;

    // Escape hatch for exercising the authorisation flow when a token is already
    // stored, which is otherwise unreachable once authorisation succeeded.
    const bool force_authorise = [] {
        const char* env = std::getenv("VOCEM_FORCE_AUTHORISE");
        return env && env[0] == '1';
    }();

    while (!g_stop) {
        // No token is not an error: the daemon asks Discord for authorisation as
        // soon as it is reachable, and until then it waits.
        const std::string token = force_authorise ? std::string() : vocem::load_token();
        if (token.empty() && !warned_about_token) {
            warned_about_token = true;
            LOG("not authorised yet -- will ask Discord once it is reachable");
        }

        vocem::WebSocket socket;
        uint16_t reached_on = 0;
        for (uint16_t port = kRpcPortFirst; port <= kRpcPortLast && !g_stop; ++port) {
            if (!socket.connect("127.0.0.1", port, path, kOrigin)) {
                continue;
            }
            // Who is on the other end, before a token with messages.read scope is
            // handed to it. Anything that binds the port before Discord does gets
            // this connection; a peer owned by another user, or by a sandbox that
            // can bind loopback but cannot read our state directory, is not
            // Discord and is told so out loud rather than trusted quietly.
            const vocem::PeerIdentity owner = socket.peer_owner();
            if (owner.outcome == vocem::PeerOwner::Found &&
                static_cast<uid_t>(owner.uid) != getuid()) {
                LOG("refusing port %u: the process listening there belongs to uid %ld, not to "
                    "you -- not sending it the Discord token",
                    port, owner.uid);
                socket.close();
                continue;
            }
            if (owner.outcome != vocem::PeerOwner::Found) {
                // Not a refusal: a container with /proc restricted cannot answer
                // the question, and a daemon that will not work there is worse
                // than one that cannot check. Said out loud, every time, because
                // a check that quietly does not happen is worth nothing.
                LOG("could not establish who owns the listener on port %u; continuing without "
                    "that check", port);
            }
            reached_on = port;
            break;
        }
        if (reached_on == 0) {
            DBG("Discord not reachable on ports %u-%u, retrying in %ds", kRpcPortFirst,
                kRpcPortLast, backoff_seconds);
            for (int i = 0; i < backoff_seconds && !g_stop; ++i) {
                sleep(1);
                service_bridge();
            }
            backoff_seconds = backoff_seconds < 30 ? backoff_seconds * 2 : 30;
            continue;
        }

        LOG("connected to Discord RPC on port %u", reached_on);
        vocem::journal_note("connected to Discord RPC");
        RpcClient client(socket, session, token);

        while (!g_stop && !client.failed()) {
            std::string raw;
            const auto result = socket.recv(raw, 1000);
            // The recv timeout doubles as the tick that retires a message's
            // words: the note segment lives only as long as the toast that is
            // drawn from it, and how long that is comes from the same setting
            // the drawing side reads.
            session.expire_note(live_config.current().notification_seconds);
            service_bridge();
            // The display, on a slower clock: a mode switch or a plugged monitor
            // is rare, and four sysfs opens a minute cost nothing.
            static double display_checked = 0.0;
            const double tick_now = vocem::monotonic_seconds();
            if (tick_now - display_checked >= 60.0) {
                display_checked = tick_now;
                session.set_display_height(vocem::display_height());
            }
            // Which channel we are actually in, asked rather than remembered.
            // Discord announces a move it performs for you; being moved by
            // somebody else is not the client joining anything, and
            // VOICE_CHANNEL_SELECT is documented as "dispatched when the client
            // joins a voice channel". The panel is meant to describe what is
            // around you right now, so where you are is reconciled on a tick and
            // the events only make it instant. The reply costs nothing when the
            // answer is the channel we already know.
            static double channel_checked = 0.0;
            if (client.authenticated() && tick_now - channel_checked >= 5.0) {
                channel_checked = tick_now;
                client.reconcile_channel();
            }
            if (result == vocem::WebSocket::Result::Closed) {
                break;
            }
            if (result == vocem::WebSocket::Result::Timeout) {
                continue;
            }
            json message = json::parse(raw, nullptr, false);
            if (message.is_discarded()) {
                continue;
            }
            client.handle(message);
        }

        const bool authorisation_failed = client.failed();
        session.set_connected(false);
        if (authorisation_failed) {
            // Keep the segment alive with the reason in it: the interface has to be
            // able to explain why nothing is happening, and offer to try again.
            session.set_status(vocem::DaemonStatus::AuthorisationRefused);
            LOG("authorisation did not succeed -- waiting for the interface to retry");
            vocem::journal_note("authorisation refused; waiting for a retry");
            while (!g_stop) {
                sleep(1);
                service_bridge();
            }
            break;
        }
        // A connection that got as far as authenticating is evidence Discord is
        // really there, and the next attempt should be immediate. One that did
        // not is a peer that took the connection and dropped it -- Discord
        // refusing an origin or a client_id, a client still starting, or
        // something else on the port entirely -- and reconnecting at once turns
        // that into a busy loop. The backoff used to be reset the moment the
        // socket connected, which made "reachable" and "willing to talk" the
        // same question: measured against the packaged 0.1.4-1 daemon, 1914
        // connections in five seconds against a peer that answered the handshake
        // and hung up, where this makes 3 -- and the stub, not the daemon, was
        // what set that ceiling (tests/daemon_reconnect.cpp).
        if (client.authenticated()) {
            backoff_seconds = 1;
        }
        if (!g_stop) {
            LOG("connection lost, reconnecting%s",
                client.authenticated() ? "" : " after a pause: the session never started");
            vocem::journal_note("connection lost, reconnecting");
        }
        if (!client.authenticated()) {
            for (int i = 0; i < backoff_seconds && !g_stop; ++i) {
                sleep(1);
                service_bridge();
            }
            backoff_seconds = backoff_seconds < 30 ? backoff_seconds * 2 : 30;
        }
    }

    LOG("shutting down");
    avatars.stop();
    // Before the segment's own name goes, and for the same reason: a mirror left
    // behind is a Flatpak game drawing a channel nobody is in any more.
    bridge.stop();
    writer.close();
    vocem::StateWriter::unlink_segment();
    vocem::journal_end();
    return 0;
}
