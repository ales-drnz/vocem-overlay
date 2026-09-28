// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Discord's local RPC, as this daemon speaks it: the authorisation dance, the
// subscriptions, and what each event does to the Session. main.cpp keeps the
// loops.

#ifndef VOCEM_DAEMON_RPC_CLIENT_H
#define VOCEM_DAEMON_RPC_CLIENT_H

#include <cstdint>
#include <csignal>
#include <string>

#include <nlohmann/json.hpp>

#include "session.h"
#include "websocket.h"

namespace vocem {

class RpcClient {
public:
    // `stop` is the daemon's stop flag, handed to the one blocking transfer
    // this class makes (the token exchange) so a SIGTERM can end it.
    RpcClient(WebSocket& socket, Session& session, std::string token,
              const volatile std::sig_atomic_t* stop)
        : socket_(socket), session_(session), token_(std::move(token)), stop_(stop) {}

    void handle(const nlohmann::json& message);

    // Whether this connection is over. `failed()` is an authorisation that did
    // not succeed, which the interface has to be told about and retry;
    // `broken()` is a peer that stopped taking what was sent to it, which is
    // a reconnect -- otherwise a peer that kept the socket open and read
    // nothing would leave the daemon "connected" to a room where nobody talks.
    bool failed() const { return failed_; }
    bool broken() const { return broken_; }
    bool authenticated() const { return authenticated_; }

    // Asked on a slow tick while connected, so the overlay cannot be left
    // describing a channel nobody is in. Every event that says "you moved" is
    // an optimisation on top of this: a move Discord does not announce (being
    // dragged into another channel by somebody else) is caught here.
    //
    // Free when nothing changed: the reply adopts only when the id differs, so
    // the participants and their speaking flags are not rebuilt every few
    // seconds.
    void reconcile_channel() { query_selected_channel(); }

private:
    void send(const nlohmann::json& message);
    void authenticate();
    void authorize();
    void subscribe(const char* event, nlohmann::json args = nlohmann::json::object());
    void unsubscribe(const char* event, const nlohmann::json& args);
    void query_selected_channel();
    void query_channel(const std::string& id);
    void apply_voice_state(const nlohmann::json& entry);
    void apply_notification(const nlohmann::json& data);
    void adopt_channel(const nlohmann::json& data);
    void unsubscribe_voice();

    WebSocket& socket_;
    Session& session_;
    std::string token_;
    const volatile std::sig_atomic_t* stop_;
    std::string channel_id_;
    std::string voice_state_;
    bool failed_ = false;
    bool broken_ = false;
    bool authenticated_ = false;
    bool reauthorised_ = false;
};

}  // namespace vocem

#endif  // VOCEM_DAEMON_RPC_CLIENT_H
