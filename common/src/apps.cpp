// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The part of vocem/apps.h that reads and writes files: the desktop-entry
// search behind the verdict, the record writer and the record reader. None of
// it runs at frame rate. Compiled once per width into vocem_common, and
// dependency-free: the in-game side must not pull in a library to do this.

#include "vocem/apps.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <ctime>

#include "vocem/flatpak.h"
#include "vocem/paths.h"

namespace vocem {

namespace detail {

std::string read_first_line(const char* path) {
    std::FILE* file = std::fopen(path, "r");
    if (!file) {
        return {};
    }
    char buffer[512] = {};
    const char* read = std::fgets(buffer, sizeof(buffer), file);
    std::fclose(file);
    if (!read) {
        return {};
    }
    std::string value(buffer);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Process names come from whatever is running and are not to be trusted with a
// path. scripts/vocem-why.sh spells the same rule to find the record by name.
std::string sanitised(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char character : name) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '.' ||
                          character == '-' || character == '_';
        out.push_back(safe ? character : '_');
    }
    if (out.empty() || out == "." || out == "..") {
        out = "unknown";
    }
    return out;
}

// A dash inside a unit name is written `\x2d`, so an application id with a dash
// arrives from /proc/self/cgroup escaped and names no file. Only `\xNN` is
// undone; anything else, and a truncated escape at the end, is left as it
// stands.
std::string unescaped_unit(const std::string& name) {
    if (name.find("\\x") == std::string::npos) {
        return name;
    }
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        const int high = i + 3 < name.size() && name[i] == '\\' && name[i + 1] == 'x'
                             ? digit(name[i + 2])
                             : -1;
        const int low = high >= 0 ? digit(name[i + 3]) : -1;
        if (low >= 0) {
            out.push_back(static_cast<char>(high * 16 + low));
            i += 3;
        } else {
            out.push_back(name[i]);
        }
    }
    return out;
}

// /proc/self/comm keeps TASK_COMM_LEN - 1 = fifteen characters, so
// `minecraft-launcher` arrives as `minecraft-launc`. The cut form matches only a
// candidate long enough to have been cut, so no short name gains a prefix match.
bool same_name(const char* candidate, const std::string& name) {
    if (name == candidate) {
        return true;
    }
    return std::strlen(candidate) > kCommLength && name.size() == kCommLength &&
           name.compare(0, kCommLength, candidate, kCommLength) == 0;
}

// systemd spells the unit `app[-<launcher>]-<ApplicationID>[@<RANDOM>].service`
// or `app[-<launcher>]-<ApplicationID>-<RANDOM>.scope` (DESKTOP_ENVIRONMENTS.md).
// The launcher part is optional (GNOME's `app-gnome-<id>-<pid>.scope`, D-Bus
// activation's `app-dbus-:1.2-<id>` slice), so the right reading cannot be told
// from the string: the readings are returned best first and the caller tries
// each against the entries on disk. Widening is safe because an entry still has
// to name this executable before it is believed (game_verdict's guard).
std::vector<std::string> desktop_ids_from_cgroup(const std::string& cgroup) {
    std::vector<std::string> candidates;
    if (cgroup.find("/app-") == std::string::npos) {
        return candidates;
    }
    // The leaf unit, which is the one that actually ran: in the D-Bus shape the
    // `app-` piece is the slice around it.
    std::string unit = cgroup.substr(cgroup.find_last_of('/') + 1);
    bool scope = false;
    for (const char* suffix : {".scope", ".service", ".slice"}) {
        const size_t length = std::strlen(suffix);
        if (unit.size() > length && unit.compare(unit.size() - length, length, suffix) == 0) {
            scope = (suffix[1] == 's' && suffix[2] == 'c');
            unit.resize(unit.size() - length);
            break;
        }
    }
    // One base's readings: the `app-` prefix off, the escaping out, then the
    // same name with the optional launcher taken off -- `gnome-`, `flatpak-`,
    // and the two pieces D-Bus activation puts in front (`dbus-`, then the
    // connection's name). Deduplicated: the two bases below often agree.
    const auto offer = [&candidates](std::string base) {
        if (base.compare(0, 4, "app-") == 0) {
            base = base.substr(4);
        }
        base = unescaped_unit(base);
        if (base.empty()) {
            return;
        }
        const auto push = [&candidates](const std::string& value) {
            for (const std::string& existing : candidates) {
                if (existing == value) {
                    return;
                }
            }
            candidates.push_back(value);
        };
        push(base);
        for (int strip = 0; strip < 2; ++strip) {
            const size_t dash = base.find('-');
            if (dash == std::string::npos) {
                break;
            }
            base = base.substr(dash + 1);
            push(base);
        }
    };
    // The random part: `@<RANDOM>` on a service, `-<RANDOM>` on a scope. With an
    // `@` the cut is certain. Otherwise the last dash is cut, which is right for a
    // scope (its random part is mandatory) and wrong for a service or a slice
    // with none (`app-org.gnome.Evince.service`), so for those the unit as it
    // stands is offered as a second base.
    std::string stripped = unit;
    if (const size_t at = stripped.find('@'); at != std::string::npos) {
        stripped.resize(at);
        offer(stripped);
    } else {
        if (const size_t last = stripped.rfind('-'); last != std::string::npos) {
            stripped.resize(last);
        }
        offer(stripped);
        if (!scope) {
            offer(unit);
        }
    }
    return candidates;
}

// Where entries live: the user's directory first, then the system ones, in the
// order the Base Directory Specification gives them.
std::vector<std::string> desktop_roots() {
    std::vector<std::string> roots;
    if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home) {
        roots.emplace_back(home);
    } else if (const char* base = std::getenv("HOME"); base && *base) {
        roots.emplace_back(std::string(base) + "/.local/share");
    }
    const char* dirs = std::getenv("XDG_DATA_DIRS");
    const std::string list = dirs && *dirs ? dirs : "/usr/local/share:/usr/share";
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(':', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        if (end > start) {
            roots.emplace_back(list.substr(start, end - start));
        }
        start = end + 1;
    }
    return roots;
}

// The entry with this id. An id is the path below applications/ with '/' turned
// into '-' (`foo/bar.desktop` is `foo-bar.desktop`), so going back is a guess:
// each dash is tried as a directory in turn, left to right. Wine installs its
// programs' entries two directories deep, under `applications/wine/Programs/`.
std::string find_desktop_entry(const std::string& reference) {
    if (!reference.empty() && reference.front() == '/') {
        return reference;
    }
    if (reference.empty()) {
        return {};
    }
    std::string file =
        reference.size() > 8 && reference.compare(reference.size() - 8, 8, ".desktop") == 0
            ? reference
            : reference + ".desktop";

    const std::vector<std::string> roots = desktop_roots();
    size_t dash = std::string::npos;
    for (;;) {
        for (const std::string& root : roots) {
            const std::string path = root + "/applications/" + file;
            struct stat info {};
            if (::stat(path.c_str(), &info) == 0) {
                return path;
            }
        }
        dash = file.find('-', dash == std::string::npos ? 0 : dash + 1);
        if (dash == std::string::npos) {
            return {};
        }
        file[dash] = '/';
    }
}

Entry read_entry(const std::string& path) {
    Entry entry;
    // Paths come from places that are not the project's own (data directories
    // the user can write to, GIO_LAUNCHED_DESKTOP_FILE -- which can name the
    // executable itself -- and a cgroup leaf whose escaping can spell a slash),
    // and this runs on a game's first frame under the layer's lock. So: no
    // following a link (a symlink to /dev/zero reads empty lines for ever), no
    // waiting on a FIFO, nothing read until fstat says it is a regular file of
    // a size a desktop entry can have (entry 135).
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        return entry;
    }
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size > kMaxEntryBytes) {
        ::close(descriptor);
        return entry;
    }
    std::FILE* file = ::fdopen(descriptor, "r");
    if (!file) {
        ::close(descriptor);
        return entry;
    }
    bool inside = false;
    char line[1024];
    long consumed = 0;
    while (std::fgets(line, sizeof(line), file)) {
        // The size was checked before the read, and the file could have
        // grown since; the bound is on what is read, not on what was there.
        consumed += static_cast<long>(std::strlen(line));
        if (consumed > kMaxEntryBytes) {
            break;
        }
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (!text.empty() && text.front() == '[') {
            if (inside) {
                break;  // the group ended, and everything wanted is behind us
            }
            inside = text == "[Desktop Entry]";
            entry.found = entry.found || inside;
            continue;
        }
        if (!inside) {
            continue;
        }
        for (const auto& field : {std::pair<const char*, std::string*>{"Exec", &entry.exec},
                                  {"TryExec", &entry.try_exec},
                                  {"Categories", &entry.categories},
                                  {"Icon", &entry.icon},
                                  {"StartupWMClass", &entry.wm_class}}) {
            const size_t length = std::strlen(field.first);
            if (text.size() > length && text.compare(0, length, field.first) == 0 &&
                text[length] == '=') {
                *field.second = text.substr(length + 1);
            }
        }
        // Only the plain keys: a localised Icon[xx] is a different key and not
        // one to be picked up by accident (the `=` right after the name above
        // is what keeps it out). NoDisplay and Hidden mean "not an application
        // to show", which the verdict does not care about and the window does.
        for (const char* key : {"NoDisplay=", "Hidden="}) {
            const size_t length = std::strlen(key);
            if (text.compare(0, length, key) == 0) {
                std::string value = text.substr(length);
                for (char& c : value) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                entry.no_display = entry.no_display || value == "true";
            }
        }
    }
    std::fclose(file);
    return entry;
}

// Every word of the line is considered, because the first is often not the
// program: a quoted path (`"/usr/lib/REAPER/reaper" %F`) or a wrapper
// (`/usr/bin/env ...`, `/bin/sh -c "..."`, `env "WINEPREFIX=..." wine ...`,
// `/usr/bin/mangohud /usr/bin/steam %U`). Left out: options, the
// specification's field codes (`%U`, `%f`) and the `VAR=value` assignments `env`
// takes. A quoted word is unquoted but never split: `sh -c "..."` holds a
// script, and guessing at it would widen the guard this serves.
std::vector<std::string> exec_program_names(const std::string& exec) {
    std::vector<std::string> names;
    size_t index = 0;
    while (index < exec.size()) {
        while (index < exec.size() && (exec[index] == ' ' || exec[index] == '\t')) {
            ++index;
        }
        std::string token;
        if (index < exec.size() && (exec[index] == '"' || exec[index] == '\'')) {
            const char quote = exec[index++];
            while (index < exec.size() && exec[index] != quote) {
                if (exec[index] == '\\' && index + 1 < exec.size()) {
                    ++index;
                }
                token.push_back(exec[index++]);
            }
            if (index < exec.size()) {
                ++index;
            }
        } else {
            while (index < exec.size() && exec[index] != ' ' && exec[index] != '\t') {
                token.push_back(exec[index++]);
            }
        }
        if (token.empty() || token.front() == '-' || token.front() == '%') {
            continue;
        }
        if (const size_t equals = token.find('='); equals != std::string::npos) {
            const size_t slash = token.find('/');
            if (slash == std::string::npos || slash > equals) {
                continue;  // an environment assignment, not a program
            }
        }
        const std::string name = basename_of(token);
        if (!name.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

bool exec_names(const std::string& exec, const std::string& binary, const std::string& comm) {
    for (const std::string& name : exec_program_names(exec)) {
        if ((!binary.empty() && name == binary) || same_name(name.c_str(), comm)) {
            return true;
        }
    }
    return false;
}

// The Desktop Entry Specification's Exec key: an argument containing a
// reserved character -- space, tab, newline, the quotes, the backslash and the
// shell's metacharacters -- is enclosed in double quotes, inside which a double
// quote, a backtick, a dollar sign and a backslash are escaped with a
// backslash. The string escape rule applies to the value first, so a literal
// backslash inside the quotes is written four times. A literal percent is `%%`
// everywhere, or it is read as a field code. Held by tests/apps.cpp.
std::string desktop_exec_quoted(const std::string& argument) {
    static const char* const reserved = " \t\n\"'\\><~|&;$*?#()`";
    const bool quote = argument.empty() || argument.find_first_of(reserved) != std::string::npos;
    std::string out;
    out.reserve(argument.size() + 8);
    if (quote) {
        out.push_back('"');
    }
    for (const char c : argument) {
        if (quote && (c == '"' || c == '`' || c == '$')) {
            out.push_back('\\');
            out.push_back(c);
        } else if (quote && c == '\\') {
            out.append("\\\\\\\\");
        } else if (c == '%') {
            out.append("%%");
        } else {
            out.push_back(c);
        }
    }
    if (quote) {
        out.push_back('"');
    }
    return out;
}

// Whole entries between the semicolons, never a substring. Every game
// subcategory in the menu specification's registry is used with the main
// category `Game`, so requiring it loses no real entry. `LauncherStore` and
// `GameTool`, the registry's words for a store front and a companion tool, are
// refused even beside `Game`; few entries use them, which is what is_launcher
// is for. `Games` is not in the registry: Discord writes it into the entries it
// generates for the games it detects, so it is read as `Game`.
bool categories_say_game(const std::string& categories) {
    bool game = false;
    bool refused = false;
    each_entry(categories, ';', [&](size_t from, size_t to) {
        const std::string entry = categories.substr(from, to - from);
        if (entry == "LauncherStore" || entry == "GameTool") {
            refused = true;
            return false;
        }
        if (entry == "Game" || entry == "Games") {
            game = true;
        }
        return true;
    });
    return game && !refused;
}

// The only signal nobody hands over: a game started from a terminal, a script
// or by hand has no launcher variable, no telling arguments and no scope of its
// own, but an installed entry may name it.
//
// Exactly one entry may name the program, and it must say Game: entries name
// interpreters and wrappers, and Steam's per-game entries all read
// `Exec=steam steam://rungameid/...` with `Game`, so "every entry naming it is a
// game" would hold for `steam` itself. The same id in two data directories is
// one entry (the first in $XDG_DATA_DIRS order is the one used).
//
// One pass over the installed entries, once per process, in the frame that
// already writes the record, and only for processes nothing else decided.
std::string entry_that_runs_this(const std::string& binary, const std::string& comm) {
    if (binary.empty() && comm.empty()) {
        return {};
    }
    // Entries live in subdirectories too, and an id spells those directories
    // with dashes, so the search is over the tree and the answer is the id.
    struct Directory {
        std::string path;
        std::string prefix;  // the id prefix everything inside carries
        int depth;           // how many directories below applications/
    };
    std::vector<Directory> pending;
    for (const std::string& root : desktop_roots()) {
        pending.push_back({root + "/applications", std::string(), 0});
    }
    std::vector<std::string> naming;  // the distinct ids that name this program
    std::string found;                // the id of the one that says Game
    for (size_t at = 0; at < pending.size() && naming.size() < 2; ++at) {
        DIR* handle = ::opendir(pending[at].path.c_str());
        if (!handle) {
            continue;
        }
        while (const dirent* item = ::readdir(handle)) {
            const std::string name = item->d_name;
            if (name.empty() || name.front() == '.') {
                continue;
            }
            const std::string path = pending[at].path + "/" + name;
            if (name.size() < 9 || name.compare(name.size() - 8, 8, ".desktop") != 0) {
                struct stat info {};
                // A directory, whose name becomes part of every id inside it.
                // Three levels: wine writes under
                // `applications/wine/Programs/<Folder>/`. lstat, so a link out
                // of the tree is not walked.
                if (::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
                    pending[at].depth < 3) {
                    pending.push_back({path, pending[at].prefix + name + "-", pending[at].depth + 1});
                }
                continue;
            }
            const Entry entry = read_entry(path);
            if (!entry.found || (!exec_names(entry.exec, binary, comm) &&
                                 !exec_names(entry.try_exec, binary, comm))) {
                continue;
            }
            const std::string id = pending[at].prefix + name;
            if (std::find(naming.begin(), naming.end(), id) != naming.end()) {
                continue;  // the same id in two data directories is one entry
            }
            naming.push_back(id);
            if (naming.size() > 1) {
                break;  // named by more than one: not specific enough to go on
            }
            if (categories_say_game(entry.categories)) {
                found = id;
            }
        }
        ::closedir(handle);
    }
    return naming.size() == 1 ? found : std::string();
}

// Minecraft, read off the game's own arguments, because there is nowhere else.
// Every Minecraft client is a JVM: comm is `java`, exe is the JVM, and only the
// command line says which program this is. Mojang's launcher, unlike Prism,
// puts nothing in the environment, but passes `--assetIndex` and `--gameDir`,
// which come from the version manifest and so survive Forge and Fabric (whose
// main class replaces `net.minecraft.client.main.Main`). Both are required:
// the list comes from the documented arguments, not from observed launches.
//
// `cmdline` is /proc/self/cmdline as it comes: arguments separated by NUL.
bool cmdline_says_minecraft(const std::string& cmdline) {
    bool asset_index = false;
    bool game_dir = false;
    size_t start = 0;
    while (start < cmdline.size()) {
        size_t end = cmdline.find('\0', start);
        if (end == std::string::npos) {
            end = cmdline.size();
        }
        const std::string argument = cmdline.substr(start, end - start);
        if (argument == "--assetIndex") {
            asset_index = true;
        } else if (argument == "--gameDir") {
            game_dir = true;
        }
        start = end + 1;
    }
    return asset_index && game_dir;
}

// The arguments this process was started with, read once and off the present
// path. To the end rather than a line: a Minecraft class path is thousands of
// characters.
std::string read_cmdline() {
    std::FILE* file = std::fopen("/proc/self/cmdline", "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, read);
    }
    std::fclose(file);
    return out;
}

}  // namespace detail

namespace {

std::string apps_directory() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/vocem/apps";
    }
    // No HOME either: a game started by a service. /tmp, where the avatar path
    // falls back too; never the game's working directory.
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.cache/vocem/apps";
}

// A field as it is written into a record: one line, no control characters, at
// most `cap` bytes (the reader keeps 1023 bytes of a line and takes the rest as
// a fresh line). Every field comes from the environment or /proc, and a newline
// would write a second key. The daemon's bridge
// (flatpak_bridge.cpp) applies the same rule to a sandbox's record.
std::string one_line(const std::string& value, size_t cap) {
    std::string out;
    out.reserve(value.size() < cap ? value.size() : cap);
    for (const char c : value) {
        if (out.size() >= cap) {
            break;
        }
        const unsigned char byte = static_cast<unsigned char>(c);
        out.push_back(byte < 0x20 || byte == 0x7F ? '?' : c);
    }
    return out;
}

}  // namespace

void record_application(const char* api) {
    // Once. Atomic: the Vulkan layer calls this outside its lock, and a game
    // presenting two swapchains from two threads would otherwise run two
    // writers onto one temporary name.
    static int done = 0;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) {
        return;
    }

    const std::string& name = process_name();
    if (name.empty() || is_own_process(name) ||
        is_own_process(detail::basename_of(process_executable()))) {
        return;
    }

    write_application_record(
        {name, process_executable(), api, launched_from_desktop_entry(), steam_app_id(), 0,
         game_verdict().game, game_verdict().reason});

    // Inside a Flatpak the file just written is in the sandbox's own cache,
    // which the window cannot reach: the daemon writes the same record on the
    // host's side, across the bridge. A no-op everywhere else.
    flatpak_bridge_record(name.c_str(), process_executable().c_str(), api,
                          game_verdict().game, game_verdict().reason.c_str());
}

void write_application_record(const Application& application) {
    write_application_record(application, detail::sanitised(application.key));
}

void write_application_record(const Application& application, const std::string& file_name) {
    if (file_name.empty() || file_name == "." || file_name == ".." ||
        file_name.find('/') != std::string::npos) {
        return;
    }
    const std::string directory = apps_directory();
    // One mkdir -p for the whole project (vocem/paths.h).
    make_directories(directory);

    const std::string path = directory + "/" + file_name;
    // The temporary carries the pid and is created exclusively: two writers of
    // one name are ordinary (every Minecraft is `java`), entry 98.
    // O_EXCL|O_NOFOLLOW rather than fopen: anything in the session can write
    // this directory, and a symlink would redirect the write while a FIFO would
    // block open() inside somebody else's first frame.
    const std::string temporary = path + ".tmp." + std::to_string(::getpid());
    int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            0600);
    if (descriptor < 0) {
        // Ours, from a process with this pid that died before the rename.
        ::unlink(temporary.c_str());
        descriptor = ::open(temporary.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    if (descriptor < 0) {
        return;
    }
    std::FILE* file = ::fdopen(descriptor, "w");
    if (!file) {
        ::close(descriptor);
        ::unlink(temporary.c_str());
        return;
    }
    // The same bounds the daemon's bridge writer applies to a sandbox's record
    // (flatpak_bridge.cpp: a name is at most a comm, an executable 512, a
    // reason 256), one line each.
    std::fprintf(file, "name = %s\n", one_line(application.key, kCommLength).c_str());
    std::fprintf(file, "executable = %s\n", one_line(application.executable, 512).c_str());
    std::fprintf(file, "api = %s\n", one_line(application.api, 16).c_str());
    std::fprintf(file, "game = %s\n", application.looks_like_game ? "true" : "false");
    // Beside the verdict, the evidence for it, so a game the overlay stayed out
    // of can be read rather than worked around.
    std::fprintf(file, "why = %s\n", one_line(application.reason, 256).c_str());
    if (!application.desktop.empty()) {
        std::fprintf(file, "desktop = %s\n", one_line(application.desktop, 512).c_str());
    }
    if (!application.steam_app_id.empty()) {
        std::fprintf(file, "steam = %s\n", one_line(application.steam_app_id, 32).c_str());
    }
    std::fprintf(file, "seen = %ld\n",
                 application.seen > 0 ? application.seen : static_cast<long>(std::time(nullptr)));
    std::fclose(file);
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
    }
}

std::vector<Application> known_applications() {
    std::vector<Application> applications;
    const std::string directory = apps_directory();
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) {
        return applications;
    }
    while (const dirent* entry = ::readdir(handle)) {
        const std::string file_name = entry->d_name;
        if (file_name == "." || file_name == "..") {
            continue;
        }
        // A record being written right now (`<name>.tmp.<pid>`), or a `<name>.tmp`
        // that older writers could leave behind.
        if (file_name.find(".tmp.") != std::string::npos ||
            (file_name.size() > 4 && file_name.compare(file_name.size() - 4, 4, ".tmp") == 0)) {
            continue;
        }

        // A record is a regular file the overlay wrote; anything else here, a
        // symlink above all, is not opened.
        const std::string path = directory + "/" + file_name;
        struct stat info {};
        if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }

        std::FILE* file = std::fopen(path.c_str(), "r");
        if (!file) {
            continue;
        }
        Application application;
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            char* equals = std::strchr(line, '=');
            if (!equals) {
                continue;
            }
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            while (*key == ' ' || *key == '\t') ++key;
            for (char* end = key + std::strlen(key); end > key && (end[-1] == ' ' || end[-1] == '\t');
                 --end) {
                end[-1] = '\0';
            }
            while (*value == ' ' || *value == '\t') ++value;
            for (char* end = value + std::strlen(value);
                 end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ');
                 --end) {
                end[-1] = '\0';
            }
            if (std::strcmp(key, "name") == 0) {
                application.key = value;
            } else if (std::strcmp(key, "executable") == 0) {
                application.executable = value;
            } else if (std::strcmp(key, "api") == 0) {
                application.api = value;
            } else if (std::strcmp(key, "desktop") == 0) {
                application.desktop = value;
            } else if (std::strcmp(key, "steam") == 0) {
                application.steam_app_id = value;
            } else if (std::strcmp(key, "game") == 0) {
                application.looks_like_game = std::strcmp(value, "true") == 0;
            } else if (std::strcmp(key, "why") == 0) {
                application.reason = value;
            } else if (std::strcmp(key, "seen") == 0) {
                application.seen = std::atol(value);
            }
        }
        std::fclose(file);
        // Our own processes are dropped (older records predate their exclusion),
        // and so is a key longer than comm's fifteen characters, which no record
        // of ours has: the key is what a flipped switch writes into the settings
        // file.
        if (!application.key.empty() && application.key.size() <= kCommLength &&
            !is_own_process(application.key)) {
            applications.push_back(application);
        }
    }
    ::closedir(handle);
    return applications;
}

void forget_applications() {
    const std::string directory = apps_directory();
    DIR* handle = ::opendir(directory.c_str());
    if (!handle) {
        return;
    }
    while (const dirent* entry = ::readdir(handle)) {
        const std::string file_name = entry->d_name;
        if (file_name == "." || file_name == "..") {
            continue;
        }
        std::remove((directory + "/" + file_name).c_str());
    }
    ::closedir(handle);
}

}  // namespace vocem
