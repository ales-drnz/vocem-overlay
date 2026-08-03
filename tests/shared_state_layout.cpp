// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The segment's layout is the same at both widths -- which it was not.
//
// The daemon is 64-bit and both widths of the injected code read its segment, so
// the struct's layout *is* the ABI. It had never been checked, and it did not
// hold: `User` begins with a `uint64_t`, whose alignment is eight bytes on x86-64
// and four on i386, so the compiler padded after `channel_name` at 64 bits and
// not at 32. Every 32-bit game read every user record four bytes early.
//
// Measured against the running daemon's own segment before the fix -- the same
// 2728 bytes, two readers:
//
//     64-bit: id=1018972252676554842 flags=10 name='Fazen' hash='d907b387...'
//     32-bit: id=8413850390381985792 flags=237247965 name='' hash=''
//
// A name that begins with four NUL bytes reads as empty, and a shifted hash reads
// as "no custom avatar", which sends the overlay to a default face whose index is
// computed from the broken id -- so The Binding of Isaac (32-bit GL under Proton)
// drew nameless grey discs and logged `gave up waiting for .../default_0.rgba`,
// a file the daemon had no reason to write. The channel name sits *before* the
// misalignment, which is why the panel looked almost right.
//
// This test is the cross-width check the earlier 32-bit probes could not be: they
// published with a writer of their own width, so writer and reader agreed and the
// defect was invisible. Here one width writes a segment to a file and the *other*
// reads it, and both directions are run by ctest.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vocem/shared_state.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// One id that needs all sixty-four bits, and one hash: the two fields whose
// misreading produced a grey disc.
const uint64_t kId = 1018972252676554842ull;
const char* const kName = "Fazen";
const char* const kHash = "d907b387c7c707262918a6a6709980f7";

void fill(vocem::SharedState& state) {
    memset(&state, 0, sizeof(state));
    state.abi_version = vocem::kAbiVersion;
    state.connected = 1;
    state.in_channel = 1;
    state.user_count = 1;
    state.status = 2;
    state.display_height = 2160;
    snprintf(state.channel_name, sizeof(state.channel_name), "a channel");
    state.users[0].id = kId;
    state.users[0].flags = 10;
    snprintf(state.users[0].name, sizeof(state.users[0].name), "%s", kName);
    snprintf(state.users[0].avatar_hash, sizeof(state.users[0].avatar_hash), "%s", kHash);
    state.notification.serial = 7;
    state.notification.user_id = kId;
}

}  // namespace

int main(int argc, char** argv) {
    printf("     this build is %zu-bit\n", sizeof(void*) * 8);

    // The numbers, asserted here as well as in the header: a static_assert stops a
    // build, and this says which number moved when somebody reads the failure.
    printf("     sizeof=%zu users=%zu notification=%zu\n", sizeof(vocem::SharedState),
           offsetof(vocem::SharedState, users), offsetof(vocem::SharedState, notification));
    check(sizeof(vocem::SharedState) == 2728, "the segment is 2728 bytes at this width");
    check(offsetof(vocem::SharedState, users) == 96, "users begins at 96 at this width");
    check(offsetof(vocem::SharedState, notification) == 2400,
          "the notification begins at 2400 at this width");
    check(sizeof(vocem::User) == 96 && offsetof(vocem::User, name) == 16 &&
              offsetof(vocem::User, avatar_hash) == 56,
          "a user record is 96 bytes with its name at 16 and its hash at 56");

    // `write <path>` lays a segment down; `read <path>` reads one somebody else
    // wrote. ctest runs one of each at each width, crossing them, because a writer
    // and a reader of the same width agree even when both are wrong.
    if (argc == 3 && !strcmp(argv[1], "write")) {
        static vocem::SharedState state;
        fill(state);
        FILE* out = fopen(argv[2], "wb");
        if (!out) {
            printf("FAIL cannot write %s\n", argv[2]);
            return 1;
        }
        fwrite(&state, 1, sizeof(state), out);
        fclose(out);
        printf("ok   wrote a %zu-byte segment to %s\n", sizeof(state), argv[2]);
        return failures == 0 ? 0 : 1;
    }

    if (argc == 3 && !strcmp(argv[1], "read")) {
        static vocem::SharedState state;
        FILE* in = fopen(argv[2], "rb");
        if (!in) {
            printf("skip %s does not exist, so the other width did not write it\n", argv[2]);
            return 77;
        }
        const size_t got = fread(&state, 1, sizeof(state), in);
        fclose(in);
        check(got == sizeof(state), "the file is exactly the size this width expects");
        check(state.abi_version == vocem::kAbiVersion, "and it declares this ABI");
        printf("     read back: id=%llu flags=%u name='%s' hash='%s'\n",
               (unsigned long long)state.users[0].id, state.users[0].flags, state.users[0].name,
               state.users[0].avatar_hash);
        // These three are the whole defect. Against the unpadded struct the id is
        // a mixture of padding and half the real one, the name is empty and the
        // hash is gone.
        check(state.users[0].id == kId, "the id survives the crossing whole");
        check(!strcmp(state.users[0].name, kName), "and the name is not empty");
        check(!strcmp(state.users[0].avatar_hash, kHash), "and the hash is the hash");
        check(state.notification.user_id == kId, "the notification's author survives too");
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
