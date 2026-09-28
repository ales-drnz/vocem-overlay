// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The settings file under hostile shapes: a property test of the one writer.
//
// Entries 224, 273, 276 and 277 are four defects of one kind, each found by
// hand after the previous fix: the window's save meeting a file it did not
// expect -- a link, an unreadable file, a dangling link, a line too long for
// the reader -- and destroying something the user had written. This test
// generates such files instead of waiting for the next one to be found: a
// seeded generator of config.ini contents (long lines past the reader's cap,
// bytes that are not UTF-8, CR/LF mixes, NUL, unknown keys and sections,
// duplicates, comments, a missing final newline, an empty file, a huge one)
// and of the thing at the path (a file, a link, a chain of links, a dangling
// link, a loop, a file nobody may read, a read-only file, a directory, a FIFO,
// a link to /dev/zero, a directory that may not be written).
//
// Every case loads the file as the window does, changes one setting through
// the path Apply takes (or clicks one of the three instant switches), and
// then holds the result to the policy:
//   * a file that could not be read completely is never written, and nothing
//     at the path or beside it changes;
//   * a FIFO or a device neither blocks the read nor gets written (each case
//     runs in a child with an alarm, so "it blocked" is a result);
//   * otherwise the write happens, the changed setting reads back, every
//     other setting reads back as it was, and every line the change did not
//     concern is still in the file, byte for byte and in order;
//   * a link stays the same link, and the file at its end is what changed;
//   * the file keeps its mode, and no temporary is left behind.
// A "line the change concerns" is decided here from the reader's rules, not
// from the writer's code: a line load() takes as a setting of the changed key.
//
// A second property: every number the window can hold is written as text that
// reads back as the same text (entry 15's writer and reader, and decimal()'s
// rounding, which is not printf's).
//
// Arguments: [cases] [seed]. Deterministic for a given pair; a failure prints
// its case number, which reruns alone with the same seed.
//
// XDG_CONFIG_HOME is a scratch directory; nothing here reaches the owner's
// settings. Root reads files of mode 000, so as root those cases cannot be
// built: skip.

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vocem/config.h"

namespace {

// ---- the generator ------------------------------------------------------

struct Random {
    uint64_t state;
    uint64_t next() {
        // splitmix64: small, fast, the same on both widths.
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    // In [0, bound).
    uint32_t below(uint32_t bound) { return static_cast<uint32_t>(next() % bound); }
    bool chance(uint32_t percent) { return below(100) < percent; }
};

std::vector<std::string> known_keys() {
    std::vector<std::string> keys;
    for (const vocem::Config::Entry& entry : vocem::Config().entries()) {
        keys.push_back(entry.key);
    }
    return keys;
}

bool is_number_key(const std::string& key, double* low = nullptr, double* high = nullptr,
                   int* decimals = nullptr) {
    size_t count = 0;
    const vocem::Config::Number* table = vocem::Config::numbers(count);
    for (size_t i = 0; i < count; ++i) {
        if (key == table[i].key) {
            if (low) *low = table[i].low;
            if (high) *high = table[i].high;
            if (decimals) *decimals = table[i].decimals;
            return true;
        }
    }
    return false;
}

bool is_bool_key(const std::string& key) {
    static const char* const kBools[] = {
        "text_shadow",   "show_channel_name", "notifications_enabled", "only_speaking",
        "hide_self",     "show_muted_state",  "panel_enabled",         "enabled",
        "keep_running",  "start_at_login",    "tray_voice_icon"};
    for (const char* name : kBools) {
        if (key == name) return true;
    }
    return false;
}

bool is_colour_key(const std::string& key) {
    return key.find("colour") != std::string::npos;
}

// A value as a person, a script or a newer version might write it for `key`,
// valid or not.
std::string any_value(Random& random, const std::string& key) {
    double low = 0, high = 1;
    int decimals = 2;
    if (is_number_key(key, &low, &high, &decimals)) {
        switch (random.below(6)) {
            case 0: return "abc";
            case 1: return "-3";
            case 2: return "1e5";
            default: {
                const double value = low + (high - low) * (random.below(10001) / 10000.0);
                char text[64];
                std::snprintf(text, sizeof(text), "%.*f", static_cast<int>(random.below(6)),
                              value);
                std::string out = text;
                if (random.chance(20)) {  // the comma entry 15 still reads
                    for (char& c : out) {
                        if (c == '.') c = ',';
                    }
                }
                return out;
            }
        }
    }
    if (is_bool_key(key)) {
        static const char* const kSpellings[] = {"true", "false", "True", "yes", "0",
                                                 "on",   "off",   "",     "maybe"};
        return kSpellings[random.below(9)];
    }
    if (is_colour_key(key)) {
        static const char* const kColours[] = {"#a1b2c3", "A1B2C3", "auto", "#zzzzzz", "#fff", ""};
        return kColours[random.below(6)];
    }
    if (key == "panel_layout") return random.chance(50) ? "horizontal" : "sideways";
    if (key == "panel_box") return random.chance(50) ? "panel" : "2";
    if (key == "notification_corner") return std::to_string(random.below(6));
    // A string: application lists, font paths, display names.
    std::string out;
    const uint32_t length = random.below(40);
    for (uint32_t i = 0; i < length; ++i) {
        const uint32_t pick = random.below(20);
        if (pick == 0) {
            out += static_cast<char>(0x80 + random.below(0x80));  // not UTF-8 on its own
        } else if (pick == 1) {
            out += ',';
        } else if (pick == 2) {
            out += ' ';
        } else {
            out += static_cast<char>('a' + random.below(26));
        }
    }
    if (random.chance(5)) {
        out = "\"" + out + "\"";
    }
    return out;
}

// A value the window itself could hold and write: for the changed setting.
std::string window_value(Random& random, const std::string& key) {
    double low = 0, high = 1;
    int decimals = 2;
    if (is_number_key(key, &low, &high, &decimals)) {
        const double value = low + (high - low) * (random.below(100001) / 100000.0);
        char text[64];
        std::snprintf(text, sizeof(text), "%.6f", value);
        return text;
    }
    if (is_bool_key(key)) return random.chance(50) ? "true" : "false";
    if (is_colour_key(key)) {
        char text[16];
        std::snprintf(text, sizeof(text), "#%06x", random.below(0x1000000));
        return (key.find("text") != std::string::npos && random.chance(20)) ? "auto" : text;
    }
    if (key == "panel_layout") return random.chance(50) ? "horizontal" : "vertical";
    if (key == "panel_box") return random.chance(50) ? "panel" : "names";
    if (key == "notification_corner") return std::to_string(random.below(4));
    std::string out;
    const uint32_t length = 1 + random.below(30);
    for (uint32_t i = 0; i < length; ++i) {
        out += random.chance(5) ? static_cast<char>(0xc0 + random.below(0x30))
                                : static_cast<char>('a' + random.below(26));
    }
    // What the reader would trim: spaces at an end, a pair of quotes.
    switch (random.below(10)) {
        case 0: return " " + out;
        case 1: return out + "\t ";
        case 2: return "\"" + out + "\"";
        default: return out;
    }
}

std::string spaced_line(Random& random, const std::string& key, const std::string& value) {
    switch (random.below(5)) {
        case 0: return key + "=" + value;
        case 1: return "  " + key + "\t=  " + value + "  ";
        case 2: return key + " =" + value;
        default: return key + " = " + value;
    }
}

std::string random_line(Random& random, const std::vector<std::string>& keys) {
    const uint32_t kind = random.below(100);
    const std::string& key = keys[random.below(static_cast<uint32_t>(keys.size()))];
    if (kind < 35) return spaced_line(random, key, any_value(random, key));
    if (kind < 40) return "gl_blacklist = " + any_value(random, "hidden_apps");
    if (kind < 50) return "future_" + std::to_string(random.below(99)) + " = " + any_value(random, "x");
    if (kind < 58) {
        static const char* const kSections[] = {"[position]", "[appearance]", "[behaviour]",
                                                "[weird section]", "[]", "  [spacing]  ",
                                                "[voice"};
        return kSections[random.below(7)];
    }
    if (kind < 66) {
        static const char* const kComments[] = {"# a note", "; another", "  # indented enabled = false",
                                                "#enabled = false", "[x] enabled = false"};
        return kComments[random.below(5)];
    }
    if (kind < 74) return random.chance(50) ? "" : " \t ";
    if (kind < 82) {
        // Bytes of every kind but the newline and NUL.
        std::string out;
        const uint32_t length = 1 + random.below(60);
        for (uint32_t i = 0; i < length; ++i) {
            out += static_cast<char>(1 + random.below(255));
            if (out.back() == '\n') out.back() = '=';
        }
        return out;
    }
    if (kind < 88) {
        // A NUL: in a comment, in a key, in a value.
        std::string out = spaced_line(random, key, any_value(random, key));
        out.insert(random.below(static_cast<uint32_t>(out.size() + 1)), 1, '\0');
        return out;
    }
    if (kind < 94) return "\"" + key + "\" = " + any_value(random, key);  // a quoted key
    // A line with a carriage return where it does not belong.
    std::string out = spaced_line(random, key, any_value(random, key));
    out.insert(random.below(static_cast<uint32_t>(out.size() + 1)), 1, '\r');
    return out;
}

// A line past the reader's 64 KiB, for a known key or not.
std::string long_line(Random& random) {
    static const char* const kLongKeys[] = {"hidden_apps", "shown_apps", "font_path",
                                            "future_list"};
    std::string out = std::string(kLongKeys[random.below(4)]) + " = ";
    const size_t length = 64 * 1024 + random.below(4096) - 2048;  // either side of the cap
    while (out.size() < length) {
        out += "game" + std::to_string(out.size()) + ",";
    }
    return out;
}

std::string random_file(Random& random, const std::vector<std::string>& keys, bool huge) {
    std::string text;
    const uint32_t ending = random.below(3);  // LF, CRLF, mixed
    const uint32_t count = huge ? 60000 : random.below(32);
    for (uint32_t i = 0; i < count; ++i) {
        std::string line = random.chance(3) && !huge ? long_line(random) : random_line(random, keys);
        const bool crlf = ending == 1 || (ending == 2 && random.chance(50));
        text += line;
        text += crlf ? "\r\n" : "\n";
    }
    if (!text.empty() && random.chance(20)) {
        text.pop_back();  // no final newline (or a lone CR at the end)
    }
    return text;
}

// ---- the reader's rules, stated independently of config.h ----------------

// What load() makes of one line, from the rules it documents: a line longer
// than 64 KiB with its newline is dropped whole; the text ends at a NUL (it is
// a C string); a line opening with '#', '[' or ';' is not a setting; a setting
// is text before the first '=', trimmed of spaces and tabs (and of CR/LF at
// its end) and of one pair of surrounding quotes.
std::string setting_key(const std::string& raw, bool had_newline) {
    if (raw.size() + (had_newline ? 1 : 0) > 64 * 1024) {
        return {};
    }
    const std::string text = raw.substr(0, raw.find('\0'));
    if (text.empty() || text[0] == '#' || text[0] == '[' || text[0] == ';') {
        return {};
    }
    const size_t equals = text.find('=');
    if (equals == std::string::npos) {
        return {};
    }
    std::string key = text.substr(0, equals);
    size_t from = 0;
    while (from < key.size() && (key[from] == ' ' || key[from] == '\t')) ++from;
    size_t to = key.size();
    while (to > from && (key[to - 1] == ' ' || key[to - 1] == '\t' || key[to - 1] == '\r' ||
                         key[to - 1] == '\n'))
        --to;
    key = key.substr(from, to - from);
    if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
        key = key.substr(1, key.size() - 2);
    }
    return key;
}

struct Line {
    std::string text;
    bool newline;
};

std::vector<Line> split_lines(const std::string& text) {
    std::vector<Line> lines;
    for (size_t at = 0; at < text.size();) {
        const size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            lines.push_back({text.substr(at), false});
            break;
        }
        lines.push_back({text.substr(at, end - at), true});
        at = end + 1;
    }
    return lines;
}

// ---- the filesystem -----------------------------------------------------

bool read_file(const std::string& path, std::string& out) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return false;
    }
    char chunk[65536];
    ssize_t got = 0;
    while ((got = ::read(fd, chunk, sizeof(chunk))) > 0) out.append(chunk, static_cast<size_t>(got));
    ::close(fd);
    return got == 0;
}

bool write_file(const std::string& path, const std::string& text, mode_t mode) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool whole = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::fchmod(fd, mode);
    ::close(fd);
    return whole;
}

void wipe(const std::string& directory) {
    ::chmod(directory.c_str(), 0700);
    DIR* listing = ::opendir(directory.c_str());
    if (!listing) return;
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(listing)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(listing);
    for (const std::string& name : names) {
        const std::string path = directory + "/" + name;
        struct stat info{};
        if (::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            wipe(path);
            ::rmdir(path.c_str());
        } else {
            ::unlink(path.c_str());
        }
    }
}

// What is in a directory, as names with their type, inode, mode, size and
// modification time: two equal listings are two untouched directories.
std::string listing_of(const std::string& directory) {
    std::vector<std::string> rows;
    if (DIR* listing = ::opendir(directory.c_str())) {
        while (const dirent* entry = ::readdir(listing)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            struct stat info{};
            ::lstat((directory + "/" + name).c_str(), &info);
            char row[512];
            std::snprintf(row, sizeof(row), "%s %o %llu %lld %lld.%09ld", name.c_str(),
                          static_cast<unsigned>(info.st_mode),
                          static_cast<unsigned long long>(info.st_ino),
                          static_cast<long long>(info.st_size),
                          static_cast<long long>(info.st_mtim.tv_sec), info.st_mtim.tv_nsec);
            rows.push_back(row);
        }
        ::closedir(listing);
    }
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (const std::string& row : rows) out += row + "\n";
    return out;
}

std::string printable(const std::string& text, size_t limit = 80) {
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c < 0x7f) {
            out += static_cast<char>(c);
        } else {
            char code[8];
            std::snprintf(code, sizeof(code), "\\x%02x", c);
            out += code;
        }
    }
    if (text.size() > limit) out += "...(" + std::to_string(text.size()) + " bytes)";
    return out;
}

// ---- the write, as the window makes it ----------------------------------

// Apply: the window's copy with one setting changed, written through the one
// writer the window's Apply uses (ConfigBridge::apply).
bool window_apply(const vocem::Config& followed, const vocem::Config& mine) {
    return vocem::Config::write_edit(followed, mine);
}

// ---- one case -----------------------------------------------------------

enum class Shape {
    File,          // a regular file
    Empty,         // an empty regular file
    Absent,        // nothing there
    Link,          // an absolute link to a file in a dotfiles directory
    RelativeChain, // a relative link to a relative link to the file
    Dangling,      // a link whose target does not exist yet
    Loop,          // two links pointing at each other
    Unreadable,    // mode 000
    ReadOnly,      // mode 0400: may be read, must not be written
    Directory,     // a directory where the file should be
    Fifo,          // a FIFO where the file should be
    LinkToFifo,    // a link to a FIFO
    LinkToDevice,  // a link to /dev/zero, which never ends
    LinkToDirectory,
    LockedDirectory,  // the file is fine, its directory may not be written
    Count,
};

const char* shape_name(Shape shape) {
    static const char* const kNames[] = {"file", "empty", "absent", "link", "relative-chain",
                                         "dangling-link", "loop", "unreadable", "read-only",
                                         "directory", "fifo", "link-to-fifo", "link-to-device",
                                         "link-to-directory", "locked-directory"};
    return kNames[static_cast<int>(shape)];
}

Shape pick_shape(Random& random) {
    const uint32_t roll = random.below(100);
    if (roll < 50) return Shape::File;
    if (roll < 53) return Shape::Empty;
    if (roll < 55) return Shape::Absent;
    if (roll < 62) return Shape::Link;
    if (roll < 66) return Shape::RelativeChain;
    if (roll < 71) return Shape::Dangling;
    if (roll < 73) return Shape::Loop;
    if (roll < 77) return Shape::Unreadable;
    if (roll < 81) return Shape::ReadOnly;
    if (roll < 84) return Shape::Directory;
    if (roll < 87) return Shape::Fifo;
    if (roll < 89) return Shape::LinkToFifo;
    if (roll < 91) return Shape::LinkToDevice;
    if (roll < 93) return Shape::LinkToDirectory;
    return Shape::LockedDirectory;
}

struct Paths {
    std::string root, vocem_dir, dot_dir, config, target;
};

// Whether the writer may write at all: the path ends, after its links, at a
// regular file it can read whole and may write, or at nothing inside a
// directory it may write.
bool writable_shape(Shape shape) {
    switch (shape) {
        case Shape::File:
        case Shape::Empty:
        case Shape::Absent:
        case Shape::Link:
        case Shape::RelativeChain:
        case Shape::Dangling:
            return true;
        default:
            return false;
    }
}

int failures_in_case = 0;
void check(bool condition, int number, Shape shape, const std::string& what) {
    if (!condition) {
        ++failures_in_case;
        std::printf("FAIL case %d (%s): %s\n", number, shape_name(shape), what.c_str());
    }
}

// Builds the case on disk; returns the text the file holds (for the shapes
// that hold one).
std::string build(const Paths& p, Shape shape, const std::string& text) {
    switch (shape) {
        case Shape::File:
            write_file(p.config, text, 0600);
            break;
        case Shape::Empty:
            write_file(p.config, "", 0640);
            return "";
        case Shape::Absent:
            return "";
        case Shape::Link:
            write_file(p.target, text, 0600);
            ::symlink(p.target.c_str(), p.config.c_str());
            break;
        case Shape::RelativeChain:
            write_file(p.target, text, 0644);
            ::symlink("vocem.ini", (p.dot_dir + "/current.ini").c_str());
            ::symlink("../../dot/current.ini", p.config.c_str());
            break;
        case Shape::Dangling:
            ::symlink("../../dot/vocem.ini", p.config.c_str());
            return "";
        case Shape::Loop:
            ::symlink((p.dot_dir + "/b").c_str(), (p.dot_dir + "/a").c_str());
            ::symlink((p.dot_dir + "/a").c_str(), (p.dot_dir + "/b").c_str());
            ::symlink((p.dot_dir + "/a").c_str(), p.config.c_str());
            return "";
        case Shape::Unreadable:
            write_file(p.config, text, 0000);
            break;
        case Shape::ReadOnly:
            write_file(p.config, text, 0400);
            break;
        case Shape::Directory:
            ::mkdir(p.config.c_str(), 0700);
            return "";
        case Shape::Fifo:
            ::mkfifo(p.config.c_str(), 0600);
            return "";
        case Shape::LinkToFifo:
            ::mkfifo(p.target.c_str(), 0600);
            ::symlink(p.target.c_str(), p.config.c_str());
            return "";
        case Shape::LinkToDevice:
            ::symlink("/dev/zero", p.config.c_str());
            return "";
        case Shape::LinkToDirectory:
            ::mkdir(p.target.c_str(), 0700);
            ::symlink(p.target.c_str(), p.config.c_str());
            return "";
        case Shape::LockedDirectory:
            write_file(p.config, text, 0600);
            ::chmod(p.vocem_dir.c_str(), 0500);
            break;
        case Shape::Count:
            break;
    }
    return text;
}

// The file the writer is expected to have written.
std::string written_path(const Paths& p, Shape shape) {
    switch (shape) {
        case Shape::Link:
        case Shape::RelativeChain:
        case Shape::Dangling:
            return p.target;
        default:
            return p.config;
    }
}

// Runs in the child. Returns the number of failed checks.
int run_case(const Paths& p, int number, Random& random, const std::vector<std::string>& keys) {
    failures_in_case = 0;
    wipe(p.vocem_dir);
    wipe(p.dot_dir);
    const Shape shape = pick_shape(random);
    const bool huge = number % 997 == 3;  // a few, because each is 2 MB
    const std::string original = build(p, shape, random_file(random, keys, huge));

    struct stat link_before{};
    const bool was_link = ::lstat(p.config.c_str(), &link_before) == 0 && S_ISLNK(link_before.st_mode);
    char link_text_before[4096] = {0};
    if (was_link) ::readlink(p.config.c_str(), link_text_before, sizeof(link_text_before) - 1);
    const std::string vocem_before = listing_of(p.vocem_dir);
    const std::string dot_before = listing_of(p.dot_dir);
    struct stat target_before{};
    const bool target_existed = ::stat(written_path(p, shape).c_str(), &target_before) == 0;

    // What the window holds: its copy, as loaded at startup (saved_ and
    // config_ in ConfigBridge), and the file as the reader sees it.
    vocem::Config followed;
    followed.load();

    // The change: one setting through Apply, or one instant switch.
    const bool through_switch = random.chance(30);
    vocem::Config expected = followed;
    std::string changed;
    bool saved = false;
    if (through_switch) {
        static bool vocem::Config::*const kSwitches[] = {&vocem::Config::enabled,
                                                          &vocem::Config::panel_enabled,
                                                          &vocem::Config::notifications_enabled};
        static const char* const kNames[] = {"enabled", "panel_enabled", "notifications_enabled"};
        const uint32_t which = random.below(3);
        changed = kNames[which];
        expected.*kSwitches[which] = !(followed.*kSwitches[which]);
        saved = vocem::Config::write_switch(kSwitches[which], expected.*kSwitches[which]);
    } else {
        const std::vector<vocem::Config::Entry> before = followed.entries();
        for (int attempt = 0; attempt < 20; ++attempt) {
            const size_t k = random.below(static_cast<uint32_t>(keys.size()));
            vocem::Config mine = followed;
            mine.assign(keys[k].c_str(), window_value(random, keys[k]).c_str());
            if (mine.entries()[k].value != before[k].value) {
                changed = keys[k];
                expected = mine;
                break;
            }
        }
        saved = window_apply(followed, expected);
    }
    const std::string what = "changing " + changed + (through_switch ? " (switch)" : " (apply)");

    // A link is the same link, whatever happened.
    if (was_link) {
        struct stat link_after{};
        char link_text_after[4096] = {0};
        const bool still = ::lstat(p.config.c_str(), &link_after) == 0 && S_ISLNK(link_after.st_mode);
        if (still) ::readlink(p.config.c_str(), link_text_after, sizeof(link_text_after) - 1);
        check(still && std::strcmp(link_text_before, link_text_after) == 0 &&
                  link_after.st_ino == link_before.st_ino,
              number, shape, what + ": the link is still the same link");
    }

    if (!writable_shape(shape)) {
        check(!saved, number, shape, what + ": the write is refused");
        ::chmod(p.vocem_dir.c_str(), 0700);
        check(listing_of(p.vocem_dir) == vocem_before, number, shape,
              what + ": nothing at the path or beside it changed");
        check(listing_of(p.dot_dir) == dot_before, number, shape,
              what + ": nothing beside a link's target changed");
        if (shape == Shape::ReadOnly || shape == Shape::LockedDirectory) {
            std::string now;
            read_file(p.config, now);
            check(now == original, number, shape, what + ": the file holds what it held");
        }
        return failures_in_case;
    }

    check(saved, number, shape, what + ": the write succeeds");
    const std::string file = written_path(p, shape);
    std::string now;
    check(read_file(file, now), number, shape, what + ": the written file is a regular file");
    struct stat target_after{};
    ::stat(file.c_str(), &target_after);
    if (target_existed) {
        check((target_after.st_mode & 07777) == (target_before.st_mode & 07777), number, shape,
              what + ": the file keeps its mode");
    }
    // No temporary beside either end: only the names that were there, plus
    // the file when it was created.
    const auto names = [](const std::string& listing) {
        std::string out;
        for (size_t at = 0; at < listing.size();) {
            const size_t end = listing.find('\n', at);
            out += listing.substr(at, listing.find(' ', at) - at) + " ";
            at = end + 1;
        }
        return out;
    };
    const std::string vocem_names = names(listing_of(p.vocem_dir));
    const std::string dot_names = names(listing_of(p.dot_dir));
    check(vocem_names == names(vocem_before) || (!target_existed && vocem_names == "config.ini "),
          number, shape, what + ": no temporary beside the path (" + vocem_names + ")");
    check(dot_names == names(dot_before) ||
              (!target_existed && shape == Shape::Dangling && dot_names == "vocem.ini "),
          number, shape, what + ": no temporary beside the target (" + dot_names + ")");

    // Every setting reads back: the changed one as changed, every other as it was.
    vocem::Config back;
    back.load();
    const std::vector<vocem::Config::Entry> want = expected.entries();
    const std::vector<vocem::Config::Entry> got = back.entries();
    for (size_t i = 0; i < want.size(); ++i) {
        check(want[i].value == got[i].value, number, shape,
              what + ": " + want[i].key + " reads back as " + printable(want[i].value) +
                  ", not " + printable(got[i].value));
    }

    // Every line the change did not concern is still there, in order, byte
    // for byte. A line concerns the change when the reader takes it as a
    // setting of the changed key (hidden_apps also answers to gl_blacklist).
    const std::vector<Line> old_lines = split_lines(original);
    const std::vector<Line> new_lines = split_lines(now);
    size_t cursor = 0;
    for (size_t i = 0; i < old_lines.size(); ++i) {
        const std::string key = setting_key(old_lines[i].text, old_lines[i].newline);
        if (key == changed || (changed == "hidden_apps" && key == "gl_blacklist")) {
            continue;
        }
        size_t found = cursor;
        while (found < new_lines.size() && new_lines[found].text != old_lines[i].text) ++found;
        if (found == new_lines.size()) {
            check(false, number, shape,
                  what + ": line " + std::to_string(i + 1) + " is kept byte for byte: \"" +
                      printable(old_lines[i].text) + "\"");
            break;  // one is enough to name the case
        }
        cursor = found + 1;
    }
    return failures_in_case;
}

// ---- the numbers --------------------------------------------------------

int number_round_trips(Random& random) {
    int failures = 0;
    size_t count = 0;
    const vocem::Config::Number* table = vocem::Config::numbers(count);
    for (int sample = 0; sample < 20000; ++sample) {
        const vocem::Config::Number& number = table[random.below(static_cast<uint32_t>(count))];
        vocem::Config config;
        // Any float in bounds, and the exact ties decimal() has to decide.
        const double span = number.high - number.low;
        double value = number.low + span * (random.below(1u << 24) / double(1u << 24));
        if (sample % 4 == 0) {
            double step = 1.0;
            for (int i = 0; i < number.decimals; ++i) step /= 10.0;
            value = number.low + step * (random.below(static_cast<uint32_t>(span / step)) + 0.5);
        }
        config.*(number.member) = static_cast<float>(value);
        const std::string first = [&] {
            for (const vocem::Config::Entry& entry : config.entries()) {
                if (std::strcmp(entry.key, number.key) == 0) return entry.value;
            }
            return std::string();
        }();
        vocem::Config back;
        back.assign(number.key, first.c_str());
        std::string second;
        for (const vocem::Config::Entry& entry : back.entries()) {
            if (std::strcmp(entry.key, number.key) == 0) second = entry.value;
        }
        if (first != second && failures < 10) {
            std::printf("FAIL %s = %.9g is written %s and reads back as %s\n", number.key, value,
                        first.c_str(), second.c_str());
        }
        failures += first != second;
    }
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (::geteuid() == 0) {
        std::printf("-- skip: root reads a file of mode 000, so the unreadable cases cannot be made\n");
        return 77;
    }
    const int cases = argc > 1 ? std::atoi(argv[1]) : 3000;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0x7ea5ULL;
    const int only = argc > 3 ? std::atoi(argv[3]) : -1;

    char scratch_template[] = "/tmp/vocem-config-hostile-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    Paths p;
    p.root = scratch;
    ::mkdir((p.root + "/config").c_str(), 0700);
    p.vocem_dir = p.root + "/config/vocem";
    p.dot_dir = p.root + "/dot";
    ::mkdir(p.vocem_dir.c_str(), 0700);
    ::mkdir(p.dot_dir.c_str(), 0700);
    p.config = p.vocem_dir + "/config.ini";
    p.target = p.dot_dir + "/vocem.ini";
    setenv("XDG_CONFIG_HOME", (p.root + "/config").c_str(), 1);
    unsetenv("FLATPAK_ID");

    const std::vector<std::string> keys = known_keys();
    int failed_cases = 0;
    int blocked = 0;
    int shapes_seen[static_cast<int>(Shape::Count)] = {0};
    for (int number = 0; number < cases; ++number) {
        if (only >= 0 && number != only) continue;
        Random random{seed * 1000003ULL + static_cast<uint64_t>(number)};
        std::fflush(stdout);
        const pid_t child = ::fork();
        if (child == 0) {
            // "It blocked" is a result: a read that waits on a FIFO or reads
            // /dev/zero for ever dies here and the parent says which case.
            ::alarm(10);
            Random own = random;
            const int failed = run_case(p, number, own, keys);
            std::fflush(stdout);
            ::_exit(failed > 100 ? 100 : failed);
        }
        int status = 0;
        ::waitpid(child, &status, 0);
        Random again = random;  // run_case's first draw is the shape
        const Shape shape = pick_shape(again);
        ++shapes_seen[static_cast<int>(shape)];
        if (WIFSIGNALED(status)) {
            ++blocked;
            ++failed_cases;
            std::printf("FAIL case %d (%s): the child died of signal %d%s\n", number,
                        shape_name(shape), WTERMSIG(status),
                        WTERMSIG(status) == SIGALRM ? " -- it blocked" : "");
            // A child killed while blocked left the directory as it was.
            ::chmod(p.vocem_dir.c_str(), 0700);
        } else if (WEXITSTATUS(status) != 0) {
            ++failed_cases;
        }
    }
    Random numbers{seed ^ 0x5eedULL};
    const int number_failures = only >= 0 ? 0 : number_round_trips(numbers);

    wipe(p.root);
    ::rmdir(p.root.c_str());
    std::printf("-- %d cases, seed 0x%llx:", cases, static_cast<unsigned long long>(seed));
    for (int s = 0; s < static_cast<int>(Shape::Count); ++s) {
        std::printf(" %s %d", shape_name(static_cast<Shape>(s)), shapes_seen[s]);
    }
    std::printf("\n-- %d case(s) failed, %d blocked; %d number(s) did not read back as written\n",
                failed_cases, blocked, number_failures);
    const bool passed = failed_cases == 0 && number_failures == 0;
    std::printf("%s\n", passed ? "all checks passed" : "FAILURES");
    return passed ? 0 : 1;
}
