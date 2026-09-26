// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "rpc_client.h"

#include <cstdio>
#include <cstdlib>

#include "auth.h"
#include "log.h"
#include "text.h"
#include "vocem/journal.h"

using json = nlohmann::json;

namespace vocem {
namespace {

// Discord sends explicit nulls for absent fields -- `evt` on command replies,
// `nick` and `global_name` on users. nlohmann's value() throws on a present-but-
// null field, so every read goes through these instead. `find` on something
// that is not an object answers end(), so a top-level array or number is
// harmless too.
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

uint64_t parse_id(const std::string& text) {
    return text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 10);
}

// Discord sends the per-guild display name in `nick`, the account-wide one in
// `global_name`, and the login name in `username`. That is the precedence the
// client itself shows, so it is the precedence used here.
std::string display_name(const json& entry) {
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

// The five events a channel subscription is made of, in one place: the list
// was spelled twice, once to subscribe and once to unsubscribe.
const char* const kVoiceEvents[] = {"VOICE_STATE_CREATE", "VOICE_STATE_UPDATE",
                                    "VOICE_STATE_DELETE", "SPEAKING_START", "SPEAKING_STOP"};

}  // namespace

void RpcClient::send(const json& message) {
    if (broken_) {
        return;
    }
    // dump() escapes every control character and throws on a string that is
    // not valid UTF-8. Nothing here can carry one: what came off the wire went
    // through the parser, which refuses invalid UTF-8, and the token is
    // validated when it is loaded (auth.cpp) -- a token file with one 0xFF
    // byte in it used to end the daemon right here, on the first READY, and
    // again every ten seconds under Restart=on-failure.
    if (!socket_.send_text(message.dump())) {
        broken_ = true;
        LOG("the peer stopped taking what was sent to it: reconnecting");
    }
}

void RpcClient::authenticate() {
    send({{"cmd", "AUTHENTICATE"}, {"args", {{"access_token", token_}}}, {"nonce", "auth"}});
}

// Asks the Discord client to authorise us; it shows the user a prompt and
// replies with a short-lived code. "prompt": "none" lets Discord skip the
// dialog when the user has already approved this application.
void RpcClient::authorize() {
    LOG("requesting authorisation -- accept the prompt in Discord");
    session_.set_status(DaemonStatus::Authorising);
    send({{"cmd", "AUTHORIZE"},
          {"args",
           {{"client_id", kClientId},
            {"scopes", {"rpc", "messages.read", "rpc.notifications.read"}},
            {"prompt", "none"}}},
          {"nonce", "authorize"}});
}

void RpcClient::subscribe(const char* event, json args) {
    send({{"cmd", "SUBSCRIBE"}, {"args", args}, {"evt", event}, {"nonce", event}});
}

void RpcClient::unsubscribe(const char* event, const json& args) {
    send({{"cmd", "UNSUBSCRIBE"}, {"args", args}, {"evt", event}, {"nonce", event}});
}

void RpcClient::query_selected_channel() {
    send({{"cmd", "GET_SELECTED_VOICE_CHANNEL"}, {"args", json::object()}, {"nonce", "chan"}});
}

// The channel the event named, rather than "whichever one is selected now".
// VOICE_CHANNEL_SELECT carries the id it is about; asking a second, different
// question means the answer can be the channel we were in a moment ago, and
// then the daemon resubscribes to the one it was just told we had left.
// GET_CHANNEL returns the same shape GET_SELECTED_VOICE_CHANNEL does -- name,
// voice_states -- so adopt_channel handles either.
void RpcClient::query_channel(const std::string& id) {
    send({{"cmd", "GET_CHANNEL"}, {"args", {{"channel_id", id}}}, {"nonce", "chan-id"}});
}

void RpcClient::handle(const json& message) {
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
            session_.set_status(DaemonStatus::AuthorisationRefused);
            failed_ = true;
            return;
        }
        const std::string code = str_field(data, "code");
        if (code.empty()) {
            LOG("authorisation reply carried no code");
            failed_ = true;
            return;
        }
        // Blocking, once: there is nothing else to do until it finishes. Up to
        // forty seconds (auth.cpp's two timeouts) with no tick in them -- the
        // one wait in this daemon that is a call rather than a loop, and it
        // happens once per authorisation, before any message can have
        // arrived in the session, so what stalls is the bridge and the
        // display re-read. Written down rather than discovered, and kept: a
        // thread or a poll loop would buy a Flatpak game a few seconds of
        // earlier adoption during the one moment the user is looking at a
        // Discord prompt. What was NOT acceptable was a stop that could not
        // get in: the flag below ends the transfer within a second.
        token_ = exchange_code_for_token(code, stop_);
        if (token_.empty()) {
            session_.set_status(DaemonStatus::AuthorisationRefused);
            failed_ = true;
            return;
        }
        if (!save_token(token_)) {
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
                forget_token();
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
            LOG("authenticated as %s", sanitise_text(str_field(data["user"], "username")).c_str());
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
            DBG("voice connection is now %s; asking where we are",
                sanitise_text(state).c_str());
            query_selected_channel();
        }
        return;
    }

    if (event == "SPEAKING_START" || event == "SPEAKING_STOP") {
        session_.set_speaking(parse_id(str_field(data, "user_id")), event == "SPEAKING_START");
        return;
    }

    // Every reply this daemon asks for is handled above; an ERROR reaching
    // this point is a request Discord refused that nothing was waiting on.
    // A refused SUBSCRIBE is the sharp case: after one, the panel shows a
    // room where nobody ever talks, and a refusal that is not logged is
    // indistinguishable from a room that is simply quiet (entry 38's
    // silence, on the wire). The dump is the error object Discord sent --
    // a code and a message, never a user's content -- and dump() escapes
    // every control character. The command is the peer's own string too, and
    // went in raw until 0.1.11: a `cmd` with a newline in it wrote a line of
    // its choosing into the journal (tests/daemon_log_lines.cpp), so it goes
    // through sanitise_text like every other string from the other end.
    if (event == "ERROR") {
        LOG("rpc refused %s: %s", sanitise_text(command).c_str(), data.dump().c_str());
    }
}

void RpcClient::apply_voice_state(const json& entry) {
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
void RpcClient::apply_notification(const json& data) {
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
    const std::string sender = sanitise_text(title);
    DBG("notification from %s: %zu bytes of text (%s)", sender.c_str(), body.size(),
        body.empty()                       ? "none: Discord sent no body and no message content"
        : str_field(data, "body").empty() ? "from the message's content"
                                          : "from the notification's body");
    {
        // That a message arrived and how long it was, and nothing about who
        // sent it or where: the journal is a file that outlives the session
        // (twenty are kept for the Debug section), and the title is the
        // sender's name with the guild and the channel -- exactly what entry
        // 163 retired from the segment the moment the toast is over. This line
        // used to carry it, for twenty sessions (entry 197).
        char note[96];
        std::snprintf(note, sizeof(note), "notification received (%zu bytes of text)", body.size());
        journal_note(note);
    }
    session_.notify(author_id, title, body, avatar);
}

void RpcClient::adopt_channel(const json& data) {
    channel_id_ = str_field(data, "id");
    session_.enter_channel(str_field(data, "name"));

    if (data.contains("voice_states") && data["voice_states"].is_array()) {
        for (const json& entry : data["voice_states"]) {
            apply_voice_state(entry);
        }
    }
    LOG("in '%s' with %zu participant(s)", sanitise_text(str_field(data, "name")).c_str(),
        session_.size());

    for (const char* event : kVoiceEvents) {
        subscribe(event, {{"channel_id", channel_id_}});
    }
}

void RpcClient::unsubscribe_voice() {
    if (channel_id_.empty()) {
        return;
    }
    for (const char* event : kVoiceEvents) {
        unsubscribe(event, {{"channel_id", channel_id_}});
    }
    channel_id_.clear();
}

}  // namespace vocem
