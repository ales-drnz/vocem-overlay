// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// config.h's numeric settings as compiled: one line per row of
// Config::numbers(), "<key> <low> <high> <decimals>", for the script tests
// that hold the window's controls to the bounds the file is read with
// (slider_bounds.cmake). Compiled rather than read out of the header with a
// regex, so a table the regex stopped matching cannot pass as agreement.

#include <cstdio>

#include "vocem/config.h"

int main() {
    size_t count = 0;
    const vocem::Config::Number* table = vocem::Config::numbers(count);
    for (size_t i = 0; i < count; ++i) {
        std::printf("%s %.6f %.6f %d\n", table[i].key, table[i].low, table[i].high,
                    table[i].decimals);
    }
    return count > 0 ? 0 : 1;
}
