// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The systemd-run command the `_unit` tests re-execute themselves with
// (tests/unit_confinement.h), and what it shows of the test's environment.
//
// The unit gets the test's whole environment. It was passed as
// --setenv=NAME=VALUE, one argument per variable, and a process's arguments
// are readable by every local user through /proc/<pid>/cmdline -- ps shows
// them -- for as long as systemd-run runs, which is the whole test: the
// 2026-09-28 refutation pass read the session's tokens there. --setenv=NAME
// takes the value from systemd-run's own environment and keeps it off the
// command line. No systemd is needed to check the command: it is built, not
// run.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unit_confinement.h"
#include "vocem_check.h"

using vocem_test::check;

int main() {
    setenv("VOCEM_TEST_SECRET", "not-for-ps-eyes", 1);
    const std::vector<std::string> argv = vocem_test::unit_command(
        "/scratch/vocem_daemon_probe", "/scratch/vocemd.service", "/run/user/1000/app",
        {"MemoryMax=512M"}, {"PrivateNetwork=yes"});
    long with_value = 0;
    long secret_seen = 0;
    bool secret_by_name = false;
    bool unit_file_by_name = false;
    for (const std::string& arg : argv) {
        if (arg.rfind("--setenv=", 0) == 0 && arg.find('=', 9) != std::string::npos) {
            if (with_value < 3) {
                // The name only: the value is what must not be shown.
                printf("     on the command line with its value: %s\n",
                       arg.substr(9, arg.find('=', 9) - 9).c_str());
            }
            ++with_value;
        }
        secret_seen += arg.find("not-for-ps-eyes") != std::string::npos ? 1 : 0;
        secret_by_name = secret_by_name || arg == "--setenv=VOCEM_TEST_SECRET";
        unit_file_by_name = unit_file_by_name || arg == "--setenv=VOCEM_UNIT_FILE";
    }
    printf("     %zu arguments, %ld of them --setenv with a value\n", argv.size(), with_value);
    check(with_value == 0, "no --setenv carries a value: every value stays off the command line");
    check(secret_seen == 0, "the test's variable's value appears in no argument");
    check(secret_by_name, "and the variable still reaches the unit, by name");
    const char* unit_file = getenv("VOCEM_UNIT_FILE");
    const char* confined = getenv("VOCEM_UNIT_CONFINED");
    const char* live_app = getenv("VOCEM_UNIT_LIVE_APP");
    check(unit_file_by_name && unit_file && strcmp(unit_file, "/scratch/vocemd.service") == 0 &&
              confined && strcmp(confined, "1") == 0 && live_app &&
              strcmp(live_app, "/run/user/1000/app") == 0,
          "the unit's own three variables are in systemd-run's environment, named on its line");
    check(!argv.empty() && argv.back() == "/scratch/vocem_daemon_probe",
          "and the command ends with the binary it re-executes");
    return vocem_test::finish();
}
