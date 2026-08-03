// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Discord authorisation.
//
// The RPC protocol wants an access token. Getting one takes two steps: ask the
// running Discord client to authorise us (it shows the user a prompt and answers
// with a short-lived code), then exchange that code for a token.
//
// The exchange endpoint belongs to Streamkit, Discord's own overlay application,
// and so does the client id. That is deliberate: the exchange requires a client
// secret, and an open-source client cannot ship a secret. The alternative is
// registering our own application and running a server to hold the secret, which is
// a permanent operational burden for an overlay. The cost of this choice is a
// dependency on an endpoint we do not control -- if Discord changes it, this stops
// working and there is no local fix.
//
// The token is a credential. It currently lives in a file with mode 0600; the
// correct home is the session keyring through libsecret, and that is a deliberate
// TODO rather than an oversight.

#ifndef VOCEM_AUTH_H
#define VOCEM_AUTH_H

#include <string>

namespace vocem {

// Streamkit's public client id.
constexpr const char* kClientId = "207646673902501888";

// Reads our own stored token. Empty when there is none.
std::string load_token();

// Stores the token with 0600. Returns false if it could not be written, in which
// case the daemon still works for this session but will ask again next time.
bool save_token(const std::string& token);

void forget_token();

// Exchanges the code from the AUTHORIZE reply for an access token. Blocking, by
// design: nothing else can proceed until authorisation completes, and it happens
// once.
std::string exchange_code_for_token(const std::string& code);

}  // namespace vocem

#endif  // VOCEM_AUTH_H
