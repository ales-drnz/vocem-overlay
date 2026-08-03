// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay's account of its life inside a process -- the structured log of
// every component, and the crash detector, in one mechanism.
//
// This grew out of the crash marker (its story: entry 38's swallowed stderr,
// entry 46's hand-symbolised core dump). Each process a component actually
// works in keeps a small journal file: opened at the first drawn frame (or at
// daemon startup), appended at the rare one-off events -- backend up, an
// avatar upload, a context teardown, a connection state change -- and, since
// the Debug section replaced the crash pop-up, RENAMED to `<pid>.done` on a
// clean unwind instead of being deleted: a finished journal is the session's
// log, and the settings window's Debug section reads it. A `.running` journal
// whose process is gone is still exactly what it always was: a process that
// ended without unwinding -- a crash, or a forced kill -- and its last line
// names what the component was doing at that moment.
//
// Beside the journal, a drawing process keeps `<pid>.stat`: its frame and
// draw counters, rewritten whole (temporary-and-rename, the apps.h rule: these
// are written from inside other people's processes) every few seconds. On the
// Vulkan side the write sits in the post-present phase with the uploads; the
// GL side has no such phase -- its hook runs before the real swap -- so there
// it rides the same bounded inline budget the avatar path already spends
// (entry 52), throttled far below it. What entry 36 forbids -- file syscalls
// in symbol-RESOLUTION paths -- is untouched either way. Removed with the journal's rename on a clean exit.
//
// Deliberately not a signal handler: the JVM owns SIGSEGV for its own null
// checks, and installing handlers inside somebody's game is a new way to
// crash it. The cost is honesty about the false positive -- SIGKILL and
// `_exit` also skip destructors, so the Debug section says "a crash, or a
// forced stop" rather than pretending to know.
//
// The writer lives in the injected code (heavy GL library and Vulkan layer --
// never the shim, which is not allowed the C runtime this uses) and in the
// daemon; the scanner runs in the configuration window. Both halves live in
// this one header so the format cannot drift between them (entry 33).

#ifndef VOCEM_JOURNAL_H
#define VOCEM_JOURNAL_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "vocem/paths.h"

namespace vocem {

inline std::string journal_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/vocem/journal";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.cache/vocem/journal";
}

// Where the crash marker used to write. Scanned for crashes only, so a game
// that was running across the upgrade still gets its report; nothing writes
// here any more.
inline std::string journal_legacy_dir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/vocem/crashes";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.cache/vocem/crashes";
}

// How many finished journals the history keeps. Enough to cover a day of
// sessions; the prune runs once per process, at begin().
inline constexpr int kJournalHistoryKeep = 20;

namespace detail {

inline FILE*& journal_file() {
    static FILE* file = nullptr;
    return file;
}

// The journal's own path, in a buffer that survives to the very end. The
// clean-exit rename runs from an ELF destructor, which glibc runs LAST --
// after every function-local static's destructor, including a std::string's.
// The first version kept the path in exactly such a string, and the rename
// read freed memory and silently did nothing: every clean session stayed
// `.running` and read as a crash (caught by the fresh-context attack pass,
// with the repo's own test calling journal_end() directly -- the wrong
// witness; tests/journal_exit.cpp walks the destructor's real door now).
// A char array has no destructor and is still itself when _dl_fini comes.
inline char* journal_path_buffer() {
    static char path[512] = {0};
    return path;
}

// Whose journal the statics describe. Games fork -- launchers, updaters,
// crash handlers -- and a forked child inherits the open FILE* and the
// destructor that closes the journal: without this check a child that exits
// cleanly would rename the PARENT's living journal into history and take its
// stat file with it. Everything below that writes or ends asks first.
inline int& journal_owner_pid() {
    static int pid = 0;
    return pid;
}

// Removes the oldest entries of one kind beyond a keep, by modification time.
inline void journal_prune_kind(const std::string& dir, const char* kind, size_t kind_length,
                               bool only_dead, int keep) {
    struct Old {
        std::string path;
        time_t when;
    };
    std::vector<Old> found;
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) {
        return;
    }
    while (struct dirent* entry = ::readdir(handle)) {
        const char* name = entry->d_name;
        const char* suffix = ::strstr(name, kind);
        if (!suffix || suffix[kind_length] != '\0') {
            continue;
        }
        if (only_dead) {
            // Liveness by pid alone here: reading each journal's header to
            // guard against a recycled pid would be a file read per entry in
            // a walk that runs inside somebody's game. A recycled pid keeps
            // one stale file alive until the pid frees up -- a bounded
            // imperfection in a mechanism that exists to provide a bound.
            char proc[64];
            std::snprintf(proc, sizeof(proc), "/proc/%d", ::atoi(name));
            struct stat alive {};
            if (::stat(proc, &alive) == 0) {
                continue;
            }
        }
        Old record;
        record.path = dir + "/" + name;
        struct stat info {};
        record.when = ::stat(record.path.c_str(), &info) == 0 ? info.st_mtime : 0;
        found.push_back(record);
    }
    ::closedir(handle);
    while (static_cast<int>(found.size()) >= keep) {
        size_t oldest = 0;
        for (size_t i = 1; i < found.size(); ++i) {
            if (found[i].when < found[oldest].when) {
                oldest = i;
            }
        }
        ::unlink(found[oldest].path.c_str());
        found.erase(found.begin() + static_cast<long>(oldest));
    }
}

// One directory walk per process lifetime, in the same phase that creates the
// directory -- never per frame. Two bounds: finished journals beyond the keep,
// and -- the same bound again -- `.running` journals of processes that are
// gone. Those are crash reports, so the newest are kept for the Debug section
// and only the oldest overflow goes; without this a crash-looping process
// (a daemon under Restart=on-failure above all) grew the directory without
// limit, and this walk with it.
inline void journal_prune(const std::string& dir) {
    journal_prune_kind(dir, ".done", 5, false, kJournalHistoryKeep);
    journal_prune_kind(dir, ".running", 8, true, kJournalHistoryKeep);
}

}  // namespace detail

// Opens this process's journal. Called once, at the first frame the overlay
// actually draws (a process it declines needs no journal) or at the daemon's
// startup. `component` is "vulkan", "opengl" or "daemon"; `process` is the
// process's own name.
inline void journal_begin(const char* component, const char* process) {
    if (detail::journal_file()) {
        return;
    }
    const std::string dir = journal_dir();
    make_directories(dir);
    detail::journal_prune(dir);
    std::snprintf(detail::journal_path_buffer(), 512, "%s/%d.running", dir.c_str(),
                  static_cast<int>(::getpid()));
    FILE* file = ::fopen(detail::journal_path_buffer(), "w");
    if (!file) {
        return;
    }
    ::setvbuf(file, nullptr, _IOLBF, 0);
    const time_t now = ::time(nullptr);
    char stamp[32] = "?";
    if (struct tm parts{}; ::localtime_r(&now, &parts)) {
        ::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &parts);
    }
    std::fprintf(file, "process = %s\npid = %d\napi = %s\nstarted = %s\n--\n", process,
                 static_cast<int>(::getpid()), component, stamp);
    detail::journal_file() = file;
    detail::journal_owner_pid() = static_cast<int>(::getpid());
}

// One line in the journal, timestamped. Only for events that happen a handful
// of times per session -- never from a per-frame path.
inline void journal_note(const char* line) {
    FILE* file = detail::journal_file();
    if (!file || detail::journal_owner_pid() != static_cast<int>(::getpid())) {
        return;  // a forked child holds the parent's journal; not its to write
    }
    const time_t now = ::time(nullptr);
    char stamp[16] = "?";
    if (struct tm parts{}; ::localtime_r(&now, &parts)) {
        ::strftime(stamp, sizeof(stamp), "%H:%M:%S", &parts);
    }
    std::fprintf(file, "%s %s\n", stamp, line);
    std::fflush(file);
}

// The frame and draw counters, rewritten whole. On Vulkan this runs in the
// post-present phase; the GL side has no such phase and spends it from the
// same bounded inline budget as its avatar path (entry 52), throttled to once
// per few seconds. Never from any symbol-resolution path (entry 36).
inline void journal_stat(long frames, long drawn) {
    if (!detail::journal_file() ||
        detail::journal_owner_pid() != static_cast<int>(::getpid())) {
        return;  // no journal (a declining process), or a forked child's copy
    }
    const std::string dir = journal_dir();
    char name[160];
    std::snprintf(name, sizeof(name), "%s/%d.stat.part", dir.c_str(),
                  static_cast<int>(::getpid()));
    FILE* file = ::fopen(name, "w");
    if (!file) {
        return;
    }
    std::fprintf(file, "frames = %ld\ndrawn = %ld\n", frames, drawn);
    ::fclose(file);
    char final_name[160];
    std::snprintf(final_name, sizeof(final_name), "%s/%d.stat", dir.c_str(),
                  static_cast<int>(::getpid()));
    ::rename(name, final_name);
}

// The clean end: the journal becomes history rather than evidence. A crash
// never gets here, which is the entire detection mechanism. This runs from an
// ELF destructor, after every function-local static with a destructor is
// gone, so everything it touches is a char buffer or a fresh local.
inline void journal_end() {
    FILE* file = detail::journal_file();
    if (!file || detail::journal_owner_pid() != static_cast<int>(::getpid())) {
        return;  // a forked child unwinding must not archive the parent's life
    }
    journal_note("clean exit");
    ::fclose(file);
    detail::journal_file() = nullptr;
    const char* running = detail::journal_path_buffer();
    char done[520];
    std::snprintf(done, sizeof(done), "%s", running);
    if (char* suffix = ::strstr(done, ".running")) {
        std::snprintf(suffix, sizeof(done) - static_cast<size_t>(suffix - done), ".done");
        ::rename(running, done);
    } else {
        ::unlink(running);
    }
    // The stat file's path is rebuilt rather than derived from the journal's:
    // this half already survived the exit path for exactly that reason.
    char stat_name[520];
    std::snprintf(stat_name, sizeof(stat_name), "%s", running);
    if (char* suffix = ::strstr(stat_name, ".running")) {
        std::snprintf(suffix, sizeof(stat_name) - static_cast<size_t>(suffix - stat_name),
                      ".stat");
        ::unlink(stat_name);
    }
}

// ---------------------------------------------------------------------------
// The scanner's half: what the configuration window's Debug section shows.
// ---------------------------------------------------------------------------

struct JournalEntry {
    std::string path;     // the journal file itself
    std::string process;  // from its header
    std::string api;      // "vulkan", "opengl" or "daemon", from its header
    int pid = 0;
    time_t when = 0;  // the file's own modification time
};

// A journal whose process is gone is a crash candidate. The pid is checked
// against /proc, and against the recorded name when the pid is alive -- a
// recycled pid must not hide a report, and a live game must not produce one.
inline bool journal_process_alive(int pid, const std::string& process) {
    char comm_path[64];
    std::snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
    FILE* comm = ::fopen(comm_path, "r");
    if (!comm) {
        return false;
    }
    char name[64] = {0};
    const bool read = ::fgets(name, sizeof(name), comm) != nullptr;
    ::fclose(comm);
    if (!read) {
        return false;
    }
    if (char* newline = ::strchr(name, '\n')) {
        *newline = '\0';
    }
    // comm is truncated to 15 characters by the kernel; compare accordingly.
    return ::strncmp(name, process.c_str(), 15) == 0;
}

#ifdef VOCEM_JOURNAL_SCANNER

namespace detail {

// A journal is a regular file the overlay wrote. Anything else in that
// directory -- a symlink above all -- is not ours and is not opened: the
// scanner followed one to /etc/passwd and put its contents in the window,
// unattended, on the four-second tick. Checked with lstat, which does not
// follow, before anything opens anything.
inline bool journal_is_regular_file(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

inline JournalEntry journal_entry_from(const std::string& dir, const char* name) {
    JournalEntry entry;
    entry.pid = ::atoi(name);
    entry.path = dir + "/" + name;
    struct stat info {};
    if (::stat(entry.path.c_str(), &info) == 0) {
        entry.when = info.st_mtime;
    }
    // The header's few fields, read until its "--" line. The crash marker
    // spelled the component field "path ="; journals from before the upgrade
    // still carry that, so both spellings are read.
    if (FILE* file = ::fopen(entry.path.c_str(), "r")) {
        char line[256] = {0};
        for (int i = 0; i < 5 && ::fgets(line, sizeof(line), file); ++i) {
            if (char* newline = ::strchr(line, '\n')) {
                *newline = '\0';
            }
            if (::strncmp(line, "--", 2) == 0) {
                break;
            }
            if (::strncmp(line, "process = ", 10) == 0) {
                entry.process = line + 10;
            } else if (::strncmp(line, "api = ", 6) == 0) {
                entry.api = line + 6;
            } else if (::strncmp(line, "path = ", 7) == 0 && entry.api.empty()) {
                entry.api = line + 7;
            }
        }
        ::fclose(file);
    }
    return entry;
}

// Every `.running` journal in one directory, kept or dropped by whether its
// process is still there.
inline void journal_walk_running(const std::string& dir, bool want_alive,
                                 std::vector<JournalEntry>& out) {
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) {
        return;
    }
    while (struct dirent* entry = ::readdir(handle)) {
        const char* name = entry->d_name;
        const char* suffix = ::strstr(name, ".running");
        if (!suffix || suffix[8] != '\0' || !journal_is_regular_file(dir + "/" + name)) {
            continue;
        }
        JournalEntry record = journal_entry_from(dir, name);
        if (record.pid > 0 &&
            journal_process_alive(record.pid, record.process) == want_alive) {
            out.push_back(record);
        }
    }
    ::closedir(handle);
}

}  // namespace detail

// The journals left by processes that are gone: crash candidates. The legacy
// crash-marker directory is walked as well, so a process that was running
// across the upgrade still gets its report.
inline std::vector<JournalEntry> journal_crashes() {
    std::vector<JournalEntry> reports;
    detail::journal_walk_running(journal_dir(), false, reports);
    detail::journal_walk_running(journal_legacy_dir(), false, reports);
    return reports;
}

// The journals of processes still running: the overlay's live instances (and
// the daemon's own journal, which callers may filter by process name).
inline std::vector<JournalEntry> journal_live() {
    std::vector<JournalEntry> live;
    detail::journal_walk_running(journal_dir(), true, live);
    return live;
}

// Finished sessions, newest first. What the Debug section lists as history.
inline std::vector<JournalEntry> journal_history() {
    std::vector<JournalEntry> done;
    const std::string dir = journal_dir();
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) {
        return done;
    }
    while (struct dirent* entry = ::readdir(handle)) {
        const char* name = entry->d_name;
        const char* suffix = ::strstr(name, ".done");
        if (!suffix || suffix[5] != '\0' ||
            !detail::journal_is_regular_file(dir + "/" + name)) {
            continue;
        }
        done.push_back(detail::journal_entry_from(dir, name));
    }
    ::closedir(handle);
    for (size_t i = 1; i < done.size(); ++i) {
        JournalEntry lifted = done[i];
        size_t j = i;
        while (j > 0 && done[j - 1].when < lifted.when) {
            done[j] = done[j - 1];
            --j;
        }
        done[j] = lifted;
    }
    return done;
}

// The counters a drawing process keeps beside its journal. False when the
// process has not written any (an old library, or the first seconds of one).
inline bool journal_read_stat(int pid, long& frames, long& drawn) {
    char name[160];
    std::snprintf(name, sizeof(name), "%s/%d.stat", journal_dir().c_str(), pid);
    FILE* file = ::fopen(name, "r");
    if (!file) {
        return false;
    }
    frames = 0;
    drawn = 0;
    const bool read = std::fscanf(file, "frames = %ld\ndrawn = %ld", &frames, &drawn) == 2;
    ::fclose(file);
    return read;
}

#endif  // VOCEM_JOURNAL_SCANNER

}  // namespace vocem

#endif  // VOCEM_JOURNAL_H
