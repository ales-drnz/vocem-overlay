// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The pieces of FlatpakBridge its three files share (flatpak_bridge.h is the
// class). flatpak_bridge.cpp: adopting, dropping and publishing into
// sandboxes. flatpak_policy.cpp: who is given what -- the host's consent, a
// process of the application running, the refusals and when each is said.
// flatpak_copies.cpp: how bytes cross -- opening under a directory the
// sandbox owns, copying with a temporary and a rename, the settings, the
// faces, the emoji bank once.

#ifndef VOCEM_DAEMON_FLATPAK_BRIDGE_PARTS_H
#define VOCEM_DAEMON_FLATPAK_BRIDGE_PARTS_H

#include <sys/types.h>

#include <cstddef>
#include <string>

namespace vocem {
namespace bridge {

// flatpak_copies.cpp
int open_regular(int directory, const char* name, int flags, mode_t mode = 0600);
bool write_at(int fd, const void* data, size_t length, off_t offset);
void remove_faces(int directory);

// flatpak_policy.cpp
bool looks_like_app_id(const char* id);
std::string printable_id(const char* id);

}  // namespace bridge
}  // namespace vocem

#endif  // VOCEM_DAEMON_FLATPAK_BRIDGE_PARTS_H
