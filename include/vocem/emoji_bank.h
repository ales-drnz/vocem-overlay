// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The colour emoji bank: the whole format in one file, both ends.
//
// Discord names are full of colour emoji and the atlas drew them as white
// monochrome glyphs -- the honest best available while decoding colour emoji
// at runtime meant FreeType and libpng inside somebody's game, which is the
// same fight the avatar cache already won by moving the parser out (the PNG
// bitmaps in Noto Color Emoji are exactly that: PNGs). So the decoding happens
// offline, in scripts/make-emoji-bank.py, and what ships is raw RGBA at one
// fixed size: records of u32 little-endian codepoint + 32*32*4 bytes,
// sorted by codepoint, binary-searched here with one pread per glyph. A file
// whose size is not a whole number of records is not a bank, end of story --
// the strictness of the reader is the format, as with vocem/avatar_rgba.h.
//
// The reader also depends on the records being *sorted*, which the size check
// cannot see, and the honest bound on that is worth writing down rather than
// leaving to be assumed: an unsorted or garbage file of the right length makes
// the search miss, never return the wrong glyph, because an index is returned
// only when the key read at it equals the codepoint asked for. A miss draws the
// monochrome fallback. That is why this file has no magic number: the failure
// mode it would guard against is already "no colour emoji", and unlike the
// avatar cache this file is not input from the internet -- it is installed by
// the package beside the libraries that read it.
//
// Two kinds of key. Below U+F0000 a key is the codepoint it draws. From
// U+F0000 up it is a SEQUENCE key: the glyph of a ZWJ sequence, a flag, a keycap
// or a tag sequence, which the font reaches only through a GSUB ligature that
// ImGui cannot shape -- so until 0.1.9 the lime (🍋 + ZWJ + 🟩) drew as a lemon
// beside a green square, each constituent its own colour glyph, the joiner a
// blank (entry 142). Which sequence each key stands for is in a second file
// beside the bank, `emoji_sequences.bin`, in fixed-size records of its own:
// u32 length, u32 key, kEmojiSequenceMaxLength x u32 codepoints zero-padded,
// sorted by the codepoint sequence. The table is read whole into static
// storage when the bank opens, so matching costs no syscall; the text is
// rewritten -- a known sequence into its key -- before it reaches the atlas
// (fonts.cpp), where the key is a codepoint like any other bank codepoint.
//
// The keys are assigned by the script and mean nothing outside the pair of
// files written together, which is why the table is a file beside the bank and
// not a table compiled into the libraries: the layer inside the Flatpak
// extension and the bank the host package installs are updated separately,
// and a compiled-in table meeting a bank of another version would draw the
// wrong picture, silently. A bank without its table still draws every single
// codepoint in colour and every sequence as its parts, and says so.

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
// keep looking. Deliberately the same policy, and the same two numbers, as
// vocem/avatar_file.h: inside a Flatpak the daemon copies the bank in on its
// own one-second tick, and the overlay's first frame happens before that. A
// single refused open remembered for the life of the process therefore left a
// sandboxed game monochrome for ever even though the file appeared a moment
// later -- measured, twice, with the host path hidden the way a sandbox hides
// it. "A picture that has not arrived yet is not a picture that failed" is the
// same sentence, one file along.
constexpr double kBankRetrySeconds = 0.5;
constexpr double kBankGiveUpSeconds = 30.0;

constexpr uint32_t kEmojiBankPixels = 32;
constexpr uint32_t kEmojiBankRgbaBytes = kEmojiBankPixels * kEmojiBankPixels * 4;
constexpr uint32_t kEmojiBankRecordBytes = 4 + kEmojiBankRgbaBytes;

// The sequence table's format. The longest sequence in the font is nine
// codepoints (a kiss with two skin tones, measured by the script); twelve is
// the record's room, and a record claiming more is a malformed table. Keys
// start at U+F0000 (plane 15, private use) because no text from Discord
// carries a codepoint there, so a key can never collide with a character a
// name spells out; the script leaves the font's own private-use cmap entries
// out of the bank for the same invariant.
constexpr uint32_t kEmojiSequenceMaxLength = 12;
constexpr uint32_t kEmojiSequenceRecordBytes = 4 * (2 + kEmojiSequenceMaxLength);
constexpr uint32_t kEmojiSequenceKeyFirst = 0xF0000;
// The table is 4166 records today; a file past this many is refused whole, so
// the storage below is the bound on what a process spends on it.
constexpr uint32_t kMaxEmojiSequences = 8192;

struct EmojiSequence {
    uint32_t length;
    uint32_t key;
    uint32_t codepoints[kEmojiSequenceMaxLength];
};
static_assert(sizeof(EmojiSequence) == kEmojiSequenceRecordBytes,
              "the sequence record is read straight into this struct");

// The table's storage: one per process, zero-initialised, never allocated.
// Every EmojiBank in a process shares it, and fonts.cpp holds exactly one bank;
// a test that opened two banks with different tables would see the last one's.
inline EmojiSequence g_emoji_sequence_storage[kMaxEmojiSequences];

// U+FE0E and U+FE0F, the presentation selectors. The font's cmap has neither:
// a shaper drops them before its ligatures apply, so the table never carries
// one and the matcher skips them in the text (❤️‍🔥 is 2764 FE0F 200D 1F525 in a
// name and 2764 200D 1F525 in the table).
inline bool is_variation_selector(uint32_t codepoint) {
    return codepoint == 0xFE0E || codepoint == 0xFE0F;
}

#ifndef VOCEM_EMOJI_BANK_PATH
#define VOCEM_EMOJI_BANK_PATH "/usr/share/vocem/emoji_bank.rgba"
#endif

// The bank on disk, opened lazily and kept open: one fd per process that draws
// colour emoji. The open happens on the first codepoint the session's text shows
// that could be in it -- so once per process, never per frame, but *not* on the
// rebuild path as an earlier version of this comment claimed.
//
// A bank that fails to open is remembered as failed (`attempted_`), so a missing
// bank costs one refused open for the life of the process. `reason()` is how the
// caller can say why colour emoji are absent instead of leaving it to be
// guessed: the honesty rule wants a component that declines to act to say so,
// and this class is not the one holding a log.
//
// **The bank is looked for in more than one place, and it has to be.** For a
// long time this was one hardcoded absolute path, in a project where every other
// thing the injected code reads has a list of candidates -- the shim tries the
// soname, then `/run/host` + VOCEM_LIBDIR, then VOCEM_LIBDIR (entry 31); the
// state, the settings, the avatars and the note each have a POSIX name and a
// Flatpak mirror. The bank had one, and the one was wrong wherever `/usr` is not
// the host's. Measured inside the Steam Linux Runtime with no game launched:
// `/usr/share/vocem/emoji_bank.rgba` is ABSENT and
// `/run/host/usr/share/vocem/emoji_bank.rgba` is PRESENT -- so every Steam title
// under pressure-vessel drew the overlay with monochrome emoji while the same
// game outside it drew them in colour. From the outside that is "sometimes
// coloured, sometimes not", which is exactly how it was reported.
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
        // Not there *yet* is not the same as not there. On the host every
        // candidate is a file that either exists or does not, and one look
        // settles it; inside a Flatpak the only reachable candidate is the copy
        // the daemon makes on its own tick, and the game's first frame beats it.
        // So a sandbox gets the avatar cache's policy -- looked at again twice a
        // second, given up on after thirty -- and the host gets exactly the one
        // attempt it always had.
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

        // Named explicitly: that answer is the whole answer. An override that
        // points at nothing must NOT fall through to the installed bank -- the
        // differential test drives its no-bank leg exactly this way, and a
        // fallback here would give it a bank and make both its frames identical.
        if (const char* named = std::getenv("VOCEM_EMOJI_BANK"); named && named[0]) {
            std::snprintf(path_, sizeof(path_), "%s", named);
            if (try_open()) {
                return true;
            }
            reason_ = "no colour emoji bank where VOCEM_EMOJI_BANK points";
            return false;
        }

        // Inside a Flatpak game the host's /usr is not mounted at all, so the
        // copy the daemon puts in the bridge directory is the only reachable
        // one (vocem/flatpak.h). Asked before the installed path, because in a
        // sandbox that path may exist and belong to the runtime.
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
        // And inside a container that mounts the host there -- pressure-vessel
        // does, which is where every Steam title runs. The same fallback, and
        // for the same reason, as the shim's second dlopen candidate.
        std::snprintf(path_, sizeof(path_), "/run/host%s", VOCEM_EMOJI_BANK_PATH);
        if (try_open()) {
            return true;
        }
        std::snprintf(path_, sizeof(path_), "%s", VOCEM_EMOJI_BANK_PATH);
        // While a sandbox is still being waited on there is nothing to report:
        // saying "no bank on disk" during the wait would put a reason in the log
        // that the next half-second may make untrue, and this project's rule is
        // that a component which declines to act says why -- not that it guesses
        // early.
        if (!reason_ && !still_arriving()) {
            reason_ = "no colour emoji bank on disk";
        }
        return false;
    }

    // Why there are no colour emoji, or nullptr while there is nothing to say
    // (working, or never asked). The string is a literal, so a caller can log it
    // once by comparing the pointer.
    const char* reason() const { return reason_; }
    // Whether the bank may still turn up. True only inside a Flatpak, only
    // while nothing has opened and the thirty seconds have not run out: the
    // caller must not write down "this codepoint has no colour glyph" during
    // that window, because the answer is not in yet.
    bool still_arriving() const {
        return fd_ < 0 && !attempted_ && first_asked_ != 0.0;
    }
    // The path the answer above is about: the candidate that opened, or the
    // installed one when none did.
    const char* path() const { return path_; }

    // Whether the bank carries this codepoint. Binary search over the sorted
    // records, reading only the 4-byte keys.
    bool contains(uint32_t codepoint) {
        return open() && index_of(codepoint) >= 0;
    }

    // How many sequences the table beside the bank has, and why it has none
    // when it has none (a literal, or nullptr while the bank is not open or the
    // table is fine). Zero with the bank open means every sequence draws as its
    // parts, which is the pre-0.1.9 picture and is said out loud.
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
    // One candidate, already in path_. Opens it, checks it is a whole number of
    // records, and takes it. A candidate that opens and is malformed stops the
    // search rather than falling through: a bank of the wrong length is a fault
    // worth naming, and the next candidate would hide it.
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

    // The table beside the bank that just opened: the same directory, the one
    // name (kBridgeEmojiSequencesName). Read whole, once, into the static
    // storage, and checked before it is believed -- a record's length, its key
    // being a sequence key, no zero or selector among its codepoints, and the
    // order the search depends on, strictly ascending with no duplicates. A
    // table that fails any of it is refused whole, with the reason said; the
    // bank is kept, and sequences draw as their parts, which is what they did
    // before there was a table. Missing is a reason too, not a silence.
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
        // The struct is read from little-endian bytes; the machines this runs
        // on are little-endian at both widths, and the bank's own keys are read
        // the same way in index_of.
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
    // The Flatpak retry window: when the first look happened and when the next
    // one is due. Zero until the first look, which is what tells still_arriving()
    // that a sandbox is being waited on at all.
    double first_asked_ = 0.0;
    double next_attempt_ = 0.0;
    // The candidate being tried, and afterwards the one that answered. A buffer
    // rather than a pointer because two of the four candidates are composed
    // rather than named, and this is read back by the log.
    char path_[512] = VOCEM_EMOJI_BANK_PATH;
    const char* reason_ = nullptr;
};

// Box-resample a bank glyph to `size` pixels (straight RGBA in and out). The
// bank is 32 and the atlas asks for the text size capped at 32, so this only
// ever scales down -- the same integer accumulation avatar_rgba_write uses,
// for the same reason: exact, dependency-free, and cheap at these sizes.
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

// Walks a UTF-8 string and calls `visit(codepoint)` for every decoded scalar.
// Twenty lines instead of a library, because the two callers run inside other
// people's games; malformed bytes are skipped a byte at a time rather than
// trusted.
//
// What "malformed" covers is spelled out, because the first version decoded
// three families of ill-formed sequence into perfectly ordinary codepoints:
// overlong forms (`C0 80` came out as U+0000, and a four-byte spelling of a
// three-byte character came out as that character), the surrogate range, and
// values past U+10FFFF. None of them could reach a wrong *glyph* -- the bank
// carries none of those codepoints -- but a decoder that reports characters a
// string does not contain is a decoder whose callers cannot reason about it,
// and one of those callers hands the value to ImGui as an `ImWchar`. A
// truncated sequence stops at the NUL, which is read and refused like any other
// non-continuation byte: the walk never steps past the terminator.
//
// utf8_each_span is the same walk telling the visitor WHERE each scalar sits --
// `visit(codepoint, begin, end)` as byte offsets into `text` -- which is what a
// caller that rewrites the string in place needs (fonts.cpp collapses a known
// emoji sequence into its key and copies every other byte, malformed ones
// included, exactly as it found them). utf8_each is that walk without the
// offsets, so there is one decoder here and not two.
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
