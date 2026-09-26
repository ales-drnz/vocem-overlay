// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The file calls the Vulkan layer makes on the colour emoji bank, stamped.
//
// Linked into vk_present_draw and exported from it (--export-dynamic-symbol),
// so the layer's own calls to open and pread -- which go through its PLT to
// whatever the process's global scope answers first, and the executable is
// first -- land here before libc. Only the bank and its sequence table are
// counted: a path naming either file, and every pread on a descriptor such a
// path was opened as. The state poll's shm_open and the settings' stat are
// file work the design accepts on the present path by name and cadence
// (rule 8 as entry 127 states it); the bank is not among them, and
// overlay_renderer.cpp said the layer "takes no file work there" while a new
// codepoint opened the bank and read its table from inside the present
// (39-139 us the first time, 6-8 us for each later new codepoint, measured by
// the 0.1.10 review).
//
// The stamps are CLOCK_MONOTONIC, the probe's own clock, so they can be read
// against the witness layer's hand-down stamps: a call before the hand-down is
// on the present path, a call after it is the post-present phase, which is
// where the design sends this work.
//
// Its own translation unit, with _FORTIFY_SOURCE off, because glibc's
// fortified headers define open and pread as inline wrappers, and a second
// definition beside them does not compile.

#undef _FORTIFY_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

namespace {

constexpr int kMaxStamps = 4096;
long long g_stamps[kMaxStamps];
std::atomic<int> g_stamp_count{0};

constexpr int kMaxTracked = 32;
std::atomic<int> g_tracked[kMaxTracked];
std::atomic<bool> g_tracked_ready{false};

void ensure_tracked() {
    if (!g_tracked_ready.load(std::memory_order_acquire)) {
        for (auto& slot : g_tracked) {
            slot.store(-1, std::memory_order_relaxed);
        }
        g_tracked_ready.store(true, std::memory_order_release);
    }
}

// Only the game's thread, which is the one the probe presents from: the atlas
// worker (entry 192) opens the bank and reads its table during the first
// build, on its own thread, while the game goes on presenting, and a stamp
// cannot say which thread made it. Measured with every thread counted: those
// two calls fell after a hand-down in 3 runs of 3, so this filter changes no
// number today; it keeps a slower machine's timing from putting another
// thread's call on this thread's present path.
void stamp() {
    if (syscall(SYS_gettid) != getpid()) {
        return;
    }
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int at = g_stamp_count.fetch_add(1, std::memory_order_relaxed);
    if (at < kMaxStamps) {
        g_stamps[at] = static_cast<long long>(now.tv_sec) * 1000000000LL + now.tv_nsec;
    }
}

// The bank and its table by their file names, and nothing wider: "emoji"
// alone matched /usr/share/applications/org.kde.plasma.emojier.desktop, which
// the application detection opens on the first present (measured: the one
// call left on the present path, 2.3 ms into the first present).
bool is_bank_path(const char* path) {
    return path && (strstr(path, "emoji_bank") != nullptr ||
                    strstr(path, "emoji_sequences") != nullptr);
}

void track(int fd, const char* path) {
    if (fd < 0 || !is_bank_path(path)) {
        return;
    }
    ensure_tracked();
    stamp();
    for (auto& slot : g_tracked) {
        int expected = -1;
        if (slot.compare_exchange_strong(expected, fd)) {
            return;
        }
    }
}

void touched(int fd) {
    ensure_tracked();
    for (auto& slot : g_tracked) {
        if (slot.load(std::memory_order_relaxed) == fd) {
            stamp();
            return;
        }
    }
}

void untrack(int fd) {
    ensure_tracked();
    for (auto& slot : g_tracked) {
        int expected = fd;
        slot.compare_exchange_strong(expected, -1);
    }
}

template <typename Fn>
Fn next(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

mode_t mode_of(int flags, va_list args) {
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
        return static_cast<mode_t>(va_arg(args, unsigned int));
    }
    return 0;
}

}  // namespace

extern "C" {

// The count, and the stamps, for the probe.
int vocem_file_witness_stamps(const long long** stamps) {
    *stamps = g_stamps;
    const int count = g_stamp_count.load(std::memory_order_acquire);
    return count < kMaxStamps ? count : kMaxStamps;
}

__attribute__((visibility("default"))) int open(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const mode_t mode = mode_of(flags, args);
    va_end(args);
    static auto real = next<int (*)(const char*, int, ...)>("open");
    const int fd = real(path, flags, mode);
    track(fd, path);
    return fd;
}

__attribute__((visibility("default"))) int open64(const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const mode_t mode = mode_of(flags, args);
    va_end(args);
    static auto real = next<int (*)(const char*, int, ...)>("open64");
    const int fd = real(path, flags, mode);
    track(fd, path);
    return fd;
}

__attribute__((visibility("default"))) int openat(int dir, const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const mode_t mode = mode_of(flags, args);
    va_end(args);
    static auto real = next<int (*)(int, const char*, int, ...)>("openat");
    const int fd = real(dir, path, flags, mode);
    track(fd, path);
    return fd;
}

__attribute__((visibility("default"))) int openat64(int dir, const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const mode_t mode = mode_of(flags, args);
    va_end(args);
    static auto real = next<int (*)(int, const char*, int, ...)>("openat64");
    const int fd = real(dir, path, flags, mode);
    track(fd, path);
    return fd;
}

__attribute__((visibility("default"))) ssize_t pread(int fd, void* buffer, size_t bytes,
                                                     off_t offset) {
    static auto real = next<ssize_t (*)(int, void*, size_t, off_t)>("pread");
    touched(fd);
    return real(fd, buffer, bytes, offset);
}

__attribute__((visibility("default"))) ssize_t pread64(int fd, void* buffer, size_t bytes,
                                                       off64_t offset) {
    static auto real = next<ssize_t (*)(int, void*, size_t, off64_t)>("pread64");
    touched(fd);
    return real(fd, buffer, bytes, offset);
}

__attribute__((visibility("default"))) int close(int fd) {
    static auto real = next<int (*)(int)>("close");
    untrack(fd);
    return real(fd);
}

}  // extern "C"
