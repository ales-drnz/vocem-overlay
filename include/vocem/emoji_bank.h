// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The colour emoji bank: the whole format in one file, both ends.
//
// Decoding colour emoji at runtime would mean FreeType and libpng inside
// somebody's game, so the decoding happens offline (scripts/make-emoji-bank.py)
// and what ships is raw RGBA at one fixed size: records of u32 little-endian
// codepoint + 32*32*4 bytes, sorted by codepoint, binary-searched here with one
// pread per glyph. A file whose size is not a whole number of records is not a
// bank: the strictness of the reader is the format, as in vocem/avatar_rgba.h.
//
// An unsorted or garbage file of the right length makes the search miss, never
// return the wrong glyph, because an index is returned only when the key read
// at it equals the codepoint asked for; a miss draws the monochrome fallback.
// That is why there is no magic number: the file is installed by the package,
// not input from the internet.
//
// Two kinds of key. Below U+F0000 a key is the codepoint it draws. From U+F0000
// up it is a SEQUENCE key: the glyph of a ZWJ sequence, a flag, a keycap or a tag
// sequence, which the font reaches only through a GSUB ligature ImGui cannot
// shape. Which sequence each key stands for is in a second file beside the
// bank, `emoji_sequences.bin`: fixed-size records of u32 length,
// u32 key, kEmojiSequenceMaxLength x u32 codepoints zero-padded, sorted by the
// codepoint sequence. The table is read whole into static storage when the bank
// opens, so matching costs no syscall; fonts.cpp rewrites a known sequence into
// its key before the text reaches the atlas.
//
// The keys are assigned by the script and mean nothing outside the pair of files
// written together, so the table is never compiled into the libraries: the
// Flatpak extension's layer and the host's bank are updated separately. A bank
// without its table still draws every single codepoint in colour and every
// sequence as its parts, and says so.

#ifndef VOCEM_EMOJI_BANK_H
#define VOCEM_EMOJI_BANK_H

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vocem/clock.h"
#include "vocem/flatpak.h"

namespace vocem {

// How often to look again for a bank that is not there *yet*, and how long to
// keep looking: the policy and numbers of vocem/avatar_file.h. Inside a Flatpak
// the daemon copies the bank in on its one-second tick, after the overlay's
// first frame, so a refusal must not be remembered for the life of the process.
constexpr double kBankRetrySeconds = 0.5;
constexpr double kBankGiveUpSeconds = 30.0;

constexpr uint32_t kEmojiBankPixels = 32;
constexpr uint32_t kEmojiBankRgbaBytes = kEmojiBankPixels * kEmojiBankPixels * 4;
constexpr uint32_t kEmojiBankRecordBytes = 4 + kEmojiBankRgbaBytes;

// The sequence table's format. The font's longest sequence is nine codepoints;
// twelve is the record's room, and a record claiming more is malformed. Keys
// start at U+F0000 (plane 15, private use) so a key can never collide with a
// character a name spells out; the script leaves the font's own private-use
// cmap entries out of the bank for the same invariant.
constexpr uint32_t kEmojiSequenceMaxLength = 12;
constexpr uint32_t kEmojiSequenceRecordBytes = 4 * (2 + kEmojiSequenceMaxLength);
constexpr uint32_t kEmojiSequenceKeyFirst = 0xF0000;
// A table past this many records (it holds a few thousand) is refused whole, so
// the storage below bounds what a process spends on it.
constexpr uint32_t kMaxEmojiSequences = 8192;

struct EmojiSequence {
    uint32_t length;
    uint32_t key;
    uint32_t codepoints[kEmojiSequenceMaxLength];
};
static_assert(sizeof(EmojiSequence) == kEmojiSequenceRecordBytes,
              "the sequence record is read straight into this struct");

// The table's storage: one per process, zero-initialised, never allocated,
// shared by every EmojiBank (fonts.cpp holds exactly one; two banks with
// different tables would see the last one's).
inline EmojiSequence g_emoji_sequence_storage[kMaxEmojiSequences];

// U+FE0E and U+FE0F, the presentation selectors. A shaper drops them before
// its ligatures apply, so the table never carries one and the matcher skips
// them in the text (❤️‍🔥 is 2764 FE0F 200D 1F525 in a name, 2764 200D 1F525 in
// the table).
inline bool is_variation_selector(uint32_t codepoint) {
    return codepoint == 0xFE0E || codepoint == 0xFE0F;
}

#ifndef VOCEM_EMOJI_BANK_PATH
#define VOCEM_EMOJI_BANK_PATH "/usr/share/vocem/emoji_bank.rgba"
#endif

// The bank on disk, opened lazily on the first codepoint the session's text
// shows that could be in it, and kept open: one fd per process, never per frame.
// A failed open is remembered (`attempted_`), so a missing bank costs one
// refused open per process; `reason()` lets the caller say why colour emoji
// are absent, since this class holds no log.
//
// Candidates, in order: VOCEM_EMOJI_BANK, the Flatpak bridge copy, the installed
// path, then /run/host + the installed path -- inside pressure-vessel, where
// every Steam title runs, `/usr` is not the host's and only /run/host has it.
class EmojiBank {
public:
    // True when the bank is present and well-formed; the monochrome fallback
    // still draws otherwise, which is rule 7's shape.
    bool open() {
        if (fd_ >= 0) {
            return true;
        }
        if (attempted_) {
            return false;
        }
        // Not there *yet* is not the same as not there. On the host one look
        // settles it; inside a Flatpak the daemon copies the bank in on its own
        // tick, after the game's first frame, so a sandbox looks again every
        // kBankRetrySeconds until kBankGiveUpSeconds.
        if (bridge_in_use()) {
            const double now = monotonic_seconds();
            if (first_asked_ == 0.0) {
                first_asked_ = now;
            } else if (now < next_attempt_) {
                return false;
            } else if (now - first_asked_ >= kBankGiveUpSeconds) {
                attempted_ = true;
                reason_ = "no colour emoji bank came across the Flatpak bridge";
                return false;
            }
            next_attempt_ = now + kBankRetrySeconds;
        } else {
            attempted_ = true;
        }

        // Named explicitly: that answer is the whole answer. An override pointing
        // at nothing must NOT fall through to the installed bank (the
        // differential test's no-bank leg relies on it).
        if (const char* named = std::getenv("VOCEM_EMOJI_BANK"); named && named[0]) {
            std::snprintf(path_, sizeof(path_), "%s", named);
            if (try_open()) {
                return true;
            }
            reason_ = "no colour emoji bank where VOCEM_EMOJI_BANK points";
            return false;
        }

        // Inside a Flatpak the daemon's copy in the bridge directory is the only
        // reachable one; asked before the installed path, which in a sandbox may
        // exist and belong to the runtime.
        if (bridge_in_use() && bridge_path(path_, sizeof(path_), kBridgeEmojiBankName)) {
            if (try_open()) {
                return true;
            }
            if (reason_) {
                return false;  // it was there and it was malformed: say that
            }
        }
        // Where the package put it, which is right on the host.
        std::snprintf(path_, sizeof(path_), "%s", VOCEM_EMOJI_BANK_PATH);
        if (try_open()) {
            return true;
        }
        if (reason_) {
            return false;
        }
        // Inside a container that mounts the host at /run/host, as
        // pressure-vessel does for every Steam title (same as the shim's second
        // dlopen candidate).
        std::snprintf(path_, sizeof(path_), "/run/host%s", VOCEM_EMOJI_BANK_PATH);
        if (try_open()) {
            return true;
        }
        std::snprintf(path_, sizeof(path_), "%s", VOCEM_EMOJI_BANK_PATH);
        // While a sandbox is still being waited on there is no reason yet: the
        // next half-second may make "no bank on disk" untrue.
        if (!reason_ && !still_arriving()) {
            reason_ = "no colour emoji bank on disk";
        }
        return false;
    }

    // Why there are no colour emoji, or nullptr while there is nothing to say
    // (working, or never asked). The string is a literal, so a caller can log it
    // once by comparing the pointer.
    const char* reason() const { return reason_; }
    // Whether the bank may still turn up: only inside a Flatpak, while nothing
    // has opened and the thirty seconds have not run out. The caller must not
    // record "this codepoint has no colour glyph" during that window.
    bool still_arriving() const {
        return fd_ < 0 && !attempted_ && first_asked_ != 0.0;
    }
    // Whether open() has ever been called: opened, given up on, or being waited
    // for in a sandbox. fonts.cpp opens the bank after a present and notes
    // nothing it would have to take back until then (fonts_look_up_noted).
    bool asked() const { return fd_ >= 0 || attempted_ || first_asked_ != 0.0; }
    // The path the answer above is about: the candidate that opened, or the
    // installed one when none did.
    const char* path() const { return path_; }

    // Whether the bank carries this codepoint. Binary search over the sorted
    // records, reading only the 4-byte keys.
    bool contains(uint32_t codepoint) {
        return open() && index_of(codepoint) >= 0;
    }

    // How many sequences the table beside the bank has, and why it has none
    // (a literal, or nullptr while the bank is not open or the table is fine).
    // Zero with the bank open means every sequence draws as its parts.
    uint32_t sequence_count() const { return sequence_count_; }
    const char* sequences_reason() const { return sequences_reason_; }

    // The longest sequence the table knows at the front of `codepoints`, as
    // its key, with `*consumed` the number of codepoints it covers -- or 0 and
    // 0 when none starts here. Presentation selectors inside or right after a
    // match are consumed with it (the table has none, a name may). No syscall:
    // a binary search on the first codepoint over the loaded table, then the
    // handful of sequences that start with it. A sequence needs at least two
    // codepoints, so a lone codepoint never matches, whatever the table says.
    uint32_t sequence_key(const uint32_t* codepoints, uint32_t count, uint32_t* consumed) const {
        *consumed = 0;
        if (sequence_count_ == 0 || count < 2 || is_variation_selector(codepoints[0])) {
            return 0;
        }
        const EmojiSequence* table = g_emoji_sequence_storage;
        uint32_t low = 0;
        uint32_t high = sequence_count_;
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2;
            if (table[middle].codepoints[0] < codepoints[0]) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        uint32_t best_key = 0;
        uint32_t best_used = 0;
        for (uint32_t i = low; i < sequence_count_ && table[i].codepoints[0] == codepoints[0]; ++i) {
            const EmojiSequence& sequence = table[i];
            uint32_t used = 0;
            uint32_t matched = 0;
            while (matched < sequence.length && used < count) {
                if (matched > 0 && is_variation_selector(codepoints[used])) {
                    ++used;
                    continue;
                }
                if (codepoints[used] != sequence.codepoints[matched]) {
                    break;
                }
                ++used;
                ++matched;
            }
            if (matched == sequence.length && used > best_used) {
                best_used = used;
                best_key = sequence.key;
            }
        }
        if (best_key == 0) {
            return 0;
        }
        while (best_used < count && is_variation_selector(codepoints[best_used])) {
            ++best_used;
        }
        *consumed = best_used;
        return best_key;
    }

    // Copies the glyph's straight RGBA into `out` (kEmojiBankRgbaBytes).
    bool load(uint32_t codepoint, unsigned char* out) {
        if (!open()) {
            return false;
        }
        const long index = index_of(codepoint);
        if (index < 0) {
            return false;
        }
        const off_t offset =
            static_cast<off_t>(index) * kEmojiBankRecordBytes + 4;
        return ::pread(fd_, out, kEmojiBankRgbaBytes, offset) ==
               static_cast<ssize_t>(kEmojiBankRgbaBytes);
    }

private:
    // One candidate, already in path_: opened and checked to be a whole number
    // of records. A malformed candidate stops the search rather than falling
    // through, so the fault is named instead of hidden by the next candidate.
    bool try_open() {
        const int fd = ::open(path_, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        struct stat info{};
        if (::fstat(fd, &info) != 0 || info.st_size <= 0 ||
            info.st_size % kEmojiBankRecordBytes != 0) {
            ::close(fd);
            reason_ = "the colour emoji bank is not a whole number of records";
            return false;
        }
        fd_ = fd;
        count_ = static_cast<uint32_t>(info.st_size / kEmojiBankRecordBytes);
        load_sequences();
        return true;
    }

    // The table beside the bank that just opened (same directory,
    // kBridgeEmojiSequencesName), read whole once into the static storage and
    // checked before it is believed: record length, a sequence key, no zero or
    // selector among the codepoints, strictly ascending order. A table failing
    // any of it is refused whole with the reason said; the bank is kept and
    // sequences draw as their parts. Missing is a reason too.
    void load_sequences() {
        sequence_count_ = 0;
        sequences_reason_ = nullptr;
        char companion[sizeof(path_) + 32];
        const char* slash = std::strrchr(path_, '/');
        const int directory = slash ? static_cast<int>(slash - path_ + 1) : 0;
        std::snprintf(companion, sizeof(companion), "%.*s%s", directory, path_,
                      kBridgeEmojiSequencesName);
        const int fd = ::open(companion, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            sequences_reason_ =
                "no sequence table beside the bank, so emoji sequences draw as their parts";
            return;
        }
        struct stat info{};
        if (::fstat(fd, &info) != 0 || info.st_size <= 0 ||
            info.st_size % kEmojiSequenceRecordBytes != 0 ||
            info.st_size / kEmojiSequenceRecordBytes > kMaxEmojiSequences) {
            ::close(fd);
            sequences_reason_ = "the emoji sequence table is not a whole number of records";
            return;
        }
        const uint32_t count = static_cast<uint32_t>(info.st_size / kEmojiSequenceRecordBytes);
        unsigned char* bytes = reinterpret_cast<unsigned char*>(g_emoji_sequence_storage);
        size_t have = 0;
        const size_t want = static_cast<size_t>(info.st_size);
        while (have < want) {
            const ssize_t got = ::pread(fd, bytes + have, want - have, static_cast<off_t>(have));
            if (got <= 0) {
                break;
            }
            have += static_cast<size_t>(got);
        }
        ::close(fd);
        if (have != want) {
            sequences_reason_ = "the emoji sequence table could not be read whole";
            return;
        }
        // Read straight from little-endian bytes: every target is little-endian
        // at both widths, as index_of also assumes.
        for (uint32_t i = 0; i < count; ++i) {
            const EmojiSequence& sequence = g_emoji_sequence_storage[i];
            if (sequence.length < 2 || sequence.length > kEmojiSequenceMaxLength ||
                sequence.key < kEmojiSequenceKeyFirst || sequence.key > 0x10FFFF) {
                sequences_reason_ = "the emoji sequence table is malformed";
                return;
            }
            for (uint32_t j = 0; j < sequence.length; ++j) {
                const uint32_t codepoint = sequence.codepoints[j];
                if (codepoint == 0 || codepoint > 0x10FFFF || is_variation_selector(codepoint)) {
                    sequences_reason_ = "the emoji sequence table is malformed";
                    return;
                }
            }
            if (i > 0 && !sequence_before(g_emoji_sequence_storage[i - 1], sequence)) {
                sequences_reason_ = "the emoji sequence table is not sorted";
                return;
            }
        }
        sequence_count_ = count;
    }

    // Strictly before, in the order the file is sorted in: codepoint by
    // codepoint, a prefix before what it prefixes, equals never.
    static bool sequence_before(const EmojiSequence& a, const EmojiSequence& b) {
        const uint32_t shorter = a.length < b.length ? a.length : b.length;
        for (uint32_t i = 0; i < shorter; ++i) {
            if (a.codepoints[i] != b.codepoints[i]) {
                return a.codepoints[i] < b.codepoints[i];
            }
        }
        return a.length < b.length;
    }

    long index_of(uint32_t codepoint) {
        long low = 0;
        long high = static_cast<long>(count_) - 1;
        while (low <= high) {
            const long middle = low + (high - low) / 2;
            unsigned char key[4];
            if (::pread(fd_, key, 4, static_cast<off_t>(middle) * kEmojiBankRecordBytes) != 4) {
                return -1;
            }
            const uint32_t value = static_cast<uint32_t>(key[0]) |
                                   (static_cast<uint32_t>(key[1]) << 8) |
                                   (static_cast<uint32_t>(key[2]) << 16) |
                                   (static_cast<uint32_t>(key[3]) << 24);
            if (value == codepoint) {
                return middle;
            }
            if (value < codepoint) {
                low = middle + 1;
            } else {
                high = middle - 1;
            }
        }
        return -1;
    }

    int fd_ = -1;
    uint32_t count_ = 0;
    uint32_t sequence_count_ = 0;
    const char* sequences_reason_ = nullptr;
    bool attempted_ = false;
    // The Flatpak retry window. first_asked_ is zero until the first look, which
    // is how still_arriving() knows a sandbox is being waited on.
    double first_asked_ = 0.0;
    double next_attempt_ = 0.0;
    // The candidate being tried, and afterwards the one that answered; a buffer
    // because some candidates are composed, and the log reads it back.
    char path_[512] = VOCEM_EMOJI_BANK_PATH;
    const char* reason_ = nullptr;
};

// Box-resample a bank glyph to `size` pixels (straight RGBA in and out). The
// atlas asks for at most 32, so this only scales down; integer accumulation as
// in avatar_rgba_write: exact, dependency-free, cheap at these sizes.
inline void emoji_bank_resample(const unsigned char* rgba32, unsigned char* out, uint32_t size) {
    if (size == kEmojiBankPixels) {
        std::memcpy(out, rgba32, kEmojiBankRgbaBytes);
        return;
    }
    for (uint32_t y = 0; y < size; ++y) {
        uint32_t y0 = y * kEmojiBankPixels / size;
        uint32_t y1 = (y + 1) * kEmojiBankPixels / size;
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < size; ++x) {
            uint32_t x0 = x * kEmojiBankPixels / size;
            uint32_t x1 = (x + 1) * kEmojiBankPixels / size;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const unsigned char* p = rgba32 + (sy * kEmojiBankPixels + sx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            unsigned char* q = out + (y * size + x) * 4;
            q[0] = static_cast<unsigned char>(sum[0] / n);
            q[1] = static_cast<unsigned char>(sum[1] / n);
            q[2] = static_cast<unsigned char>(sum[2] / n);
            q[3] = static_cast<unsigned char>(sum[3] / n);
        }
    }
}

// Walks a UTF-8 string and calls `visit(codepoint)` for every decoded scalar;
// no library, because the callers run inside other people's games. Ill-formed
// input is skipped a byte at a time: overlong forms, surrogates and values past
// U+10FFFF are refused, never decoded into characters the string does not
// contain (one caller hands the value to ImGui as an `ImWchar`). A truncated
// sequence stops at the NUL, which is refused like any non-continuation byte.
//
// utf8_each_span also tells the visitor where each scalar sits,
// `visit(codepoint, begin, end)` as byte offsets into `text`, for a caller that
// rewrites the string in place (fonts.cpp); utf8_each is the same walk without
// the offsets, so there is one decoder.
template <typename Visit>
inline void utf8_each_span(const char* text, Visit visit) {
    const unsigned char* start = reinterpret_cast<const unsigned char*>(text);
    const unsigned char* s = start;
    while (*s) {
        uint32_t cp = 0;
        int extra = 0;
        uint32_t least = 0;  // the smallest value this length may encode
        if (*s < 0x80) {
            cp = *s;
        } else if ((*s & 0xe0) == 0xc0) {
            cp = *s & 0x1f;
            extra = 1;
            least = 0x80;
        } else if ((*s & 0xf0) == 0xe0) {
            cp = *s & 0x0f;
            extra = 2;
            least = 0x800;
        } else if ((*s & 0xf8) == 0xf0) {
            cp = *s & 0x07;
            extra = 3;
            least = 0x10000;
        } else {
            ++s;
            continue;
        }
        const unsigned char* p = s + 1;
        bool valid = true;
        for (int i = 0; i < extra; ++i, ++p) {
            if ((*p & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (*p & 0x3f);
        }
        if (!valid || cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            ++s;
            continue;
        }
        visit(cp, static_cast<size_t>(s - start), static_cast<size_t>(p - start));
        s = p;
    }
}

template <typename Visit>
inline void utf8_each(const char* text, Visit visit) {
    utf8_each_span(text, [&](uint32_t cp, size_t, size_t) { visit(cp); });
}

// The UTF-8 spelling of one scalar into `out` (at least 4 bytes); how many
// bytes it took. The one encoder, for the one caller that writes a key back
// into a name.
inline size_t utf8_put(uint32_t cp, char* out) {
    unsigned char* o = reinterpret_cast<unsigned char*>(out);
    if (cp < 0x80) {
        o[0] = static_cast<unsigned char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        o[0] = static_cast<unsigned char>(0xC0 | (cp >> 6));
        o[1] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = static_cast<unsigned char>(0xE0 | (cp >> 12));
        o[1] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
        o[2] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = static_cast<unsigned char>(0xF0 | (cp >> 18));
    o[1] = static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F));
    o[2] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
    o[3] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
    return 4;
}

}  // namespace vocem

#endif  // VOCEM_EMOJI_BANK_H
