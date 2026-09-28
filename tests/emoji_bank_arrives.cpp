// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A bank that has not arrived yet is not a bank that is missing.
//
// Inside a Flatpak the host's /usr is not mounted, so the only reachable copy of
// the colour emoji bank is the one the daemon puts in the bridge directory --
// and the daemon does that on its own one-second tick, while the game's very
// first frame happens immediately. The bank's `open()` remembered a refusal for
// the life of the process, so a sandboxed game asked once, was told no, and drew
// every emoji in the monochrome font for ever, even though the file appeared
// half a second later. The Flatpak half of the candidate list therefore did not
// work at all, which is worth a test rather than a second reading of the code.
//
// This is `vocem/avatar_file.h`'s policy, one file along: a picture that has not
// arrived yet is not a picture that failed. Looked at again twice a second,
// given up on after thirty -- and only inside a sandbox, because on the host
// every candidate is a file that either exists or does not and one look settles
// it.
//
// The sandbox's shape is built rather than described: bwrap with a tmpfs over
// the compiled-in directory, so the host's bank is as unreachable as it is in
// there, and a copy left where the daemon's own copy would land. Skipped where
// bwrap is missing, and where the machine has no bank installed to stage.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "vocem/emoji_bank.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// Where the sandbox's copy would be staged from: bound in by the re-exec below,
// because the compiled-in directory is covered by a tmpfs inside.
const char* kStaged = "/tmp/vocem-bank-under-test.rgba";
// And the sequence table beside it, staged under its own unknown name: the
// bridge copies it into the sandbox BEFORE the bank, and the reader takes it
// at the moment the bank opens.
const char* kStagedTable = "/tmp/vocem-sequences-under-test.bin";

}  // namespace

int main(int argc, char** argv) {
    const std::string compiled = VOCEM_EMOJI_BANK_PATH;
    const std::string compiled_dir = compiled.substr(0, compiled.find_last_of('/'));

    if (argc < 2 || strcmp(argv[1], "--inside") != 0) {
        // The file to stage into the sandbox. Not the compiled-in path: in a
        // build tree that names an install prefix nothing was installed to, and
        // the point of this test is the timing rather than the path.
        const char* source = getenv("VOCEM_TEST_BANK_SOURCE");
        struct stat there {};
        if (!source || !source[0] || ::stat(source, &there) != 0) {
            printf("skip no bank to stage from (VOCEM_TEST_BANK_SOURCE)\n");
            return 77;
        }
        if (system("command -v bwrap >/dev/null 2>&1") != 0) {
            printf("skip bwrap is not installed, so the sandbox's shape cannot be built\n");
            return 77;
        }
        char self[4096];
        const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) {
            printf("FAIL cannot find my own binary\n");
            return 1;
        }
        self[n] = '\0';
        // The compiled-in directory is hidden only where it exists -- bwrap
        // builds its own mountpoints and cannot mkdir into a read-only /usr, and
        // in a build tree that directory is already absent, which is the same
        // state a sandbox is in. The bank goes in under a name the code does not
        // know, so nothing can find it except the bridge copy staged later.
        std::vector<const char*> args = {"bwrap", "--dev-bind", "/", "/"};
        if (::stat(compiled_dir.c_str(), &there) == 0) {
            args.push_back("--tmpfs");
            args.push_back(compiled_dir.c_str());
        }
        args.push_back("--ro-bind");
        args.push_back(source);
        args.push_back(kStaged);
        const std::string source_table =
            std::string(source).substr(0, std::string(source).find_last_of('/') + 1) +
            vocem::kBridgeEmojiSequencesName;
        if (::stat(source_table.c_str(), &there) == 0) {
            args.push_back("--ro-bind");
            args.push_back(source_table.c_str());
            args.push_back(kStagedTable);
        }
        args.push_back("--die-with-parent");
        args.push_back(self);
        args.push_back("--inside");
        args.push_back(nullptr);
        execvp("bwrap", const_cast<char* const*>(args.data()));
        printf("FAIL could not exec bwrap\n");
        return 1;
    }

    // A sandbox's runtime directory, with the bridge directory the daemon would
    // have created and nothing in it yet.
    char root[] = "/tmp/vocem-bank-arrives-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string app = std::string(root) + "/app/org.example.Game/vocem";
    std::string made;
    for (const char* part : {"/app", "/app/org.example.Game", "/app/org.example.Game/vocem"}) {
        made = std::string(root) + part;
        ::mkdir(made.c_str(), 0700);
    }
    setenv("XDG_RUNTIME_DIR", root, 1);
    setenv("FLATPAK_ID", "org.example.Game", 1);
    unsetenv("VOCEM_EMOJI_BANK");
    check(vocem::enter_flatpak_bridge(), "the sandbox's bridge directory is entered");

    vocem::EmojiBank bank;

    // The first frame, before the daemon's tick. Monochrome is right here --
    // what must NOT happen is that this answer becomes permanent.
    check(!bank.contains(0x1F525), "the first frame finds no bank, which is the truth then");
    check(bank.still_arriving(), "and the bank is still on its way, not written off");
    check(bank.reason() == nullptr,
          "with nothing reported yet: a reason during the wait may be untrue by the next look");

    // The daemon's tick lands: the table first, then the bank, in the bridge's
    // own order (flatpak_copies.cpp says why the order is that one).
    const std::string target = app + "/" + vocem::kBridgeEmojiBankName;
    const std::string table_target = app + "/" + vocem::kBridgeEmojiSequencesName;
    struct stat staged_table {};
    const bool table_staged = ::stat(kStagedTable, &staged_table) == 0;
    if (table_staged) {
        std::string copy_table = std::string("cp '") + kStagedTable + "' '" + table_target + "'";
        if (system(copy_table.c_str()) != 0) {
            printf("skip the sequence table could not be staged into the sandbox\n");
            return 77;
        }
    }
    std::string copy = std::string("cp '") + kStaged + "' '" + target + "'";
    if (system(copy.c_str()) != 0) {
        printf("skip the bank could not be staged into the sandbox\n");
        return 77;
    }
    // Past the retry interval, which is what a running game crosses in three
    // frames.
    usleep(700 * 1000);

    check(bank.contains(0x1F525), "and once it arrives the emoji are coloured after all");
    check(!bank.still_arriving(), "with the wait over");
    // The exact path, not a substring: the bridge copy and the host install
    // share the basename (kBridgeEmojiBankName is "emoji_bank.rgba"), so a
    // strstr on the name would be satisfied by the host path too and the check
    // would pass with the sandbox mechanism dead.
    check(strcmp(bank.path(), target.c_str()) == 0,
          "through the bridge's own copy, which is the only one a sandbox can reach");
    // The table beside it was read at that moment, from the same directory.
    if (table_staged) {
        printf("     %u sequences read beside the bridge's bank\n", bank.sequence_count());
        check(bank.sequence_count() > 0 && bank.sequences_reason() == nullptr,
              "and the sequence table that came across with it was read at the same moment");
        uint32_t used = 0;
        const uint32_t lime[] = {0x1F34B, 0x200D, 0x1F7E9};
        check(bank.sequence_key(lime, 3, &used) >= vocem::kEmojiSequenceKeyFirst && used == 3,
              "so the lime is one glyph inside the sandbox too");
    } else {
        check(bank.sequence_count() == 0 && bank.sequences_reason() != nullptr,
              "with no table staged, the bank says so rather than staying silent");
    }

    std::string cleanup = std::string("rm -rf ") + root;
    system(cleanup.c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
