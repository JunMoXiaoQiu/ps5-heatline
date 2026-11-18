/*
 * Common FPS for PS5
 * Copyright (C) 2026 porhe911
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/config.hpp"
#include "common_fps/fps_sampler.hpp"
#include "common_fps/layout.hpp"
#include "common_fps/overlay_cells.hpp"
#include "common_fps/overlay_rows.hpp"
#include "common_fps/wire.hpp"
#include "overlay_layout_state.hpp"
#include "ps5_platform.hpp"
#include "dce_fps_sampler.hpp"
#include "shellui_injector.hpp"
#include "state_sender.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <optional>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

/* Heatline's own sampler (fps.c, linked into the same payload). The overlay
 * displays these values so it and the dashboard share one metric source. */
extern "C" int fps_overlay_metrics(int* fps, int* avg, int* p1, int* p02);
/* Heatline's collector (see osd_overlay.h). Weak: a standalone engine build
 * has none, and then only the FPS-engine readings exist. */
extern "C" int overlay_collect_metrics(
    double* value, unsigned char* have, int count) __attribute__((weak));

namespace {

constexpr const char* kControllerLog =
    "/data/CommonFPS_v1_2_1.log";

/* SimpleFPS settings, read once a second so an edit shows while playing. */
constexpr const char* kSettingsPath = "/data/simplefps.ini";
constexpr std::size_t kSettingsMaxSize = 4096;

/*
 * The newest launch owns the overlay. Each controller writes its pid here when
 * it starts; one that later reads another pid stops for good. Without this a
 * second launch leaves two controllers sending the renderer different numbers.
 */
constexpr const char* kOwnerPath = "/system_tmp/simplefps_owner.pid";

constexpr std::uint64_t kDceVideoOutStandbyUs = 60'000'000ULL;
constexpr unsigned kDceMissesBeforeVideoOutWake = 3;

/*
 * These FW 9.60 kinfo_proc offsets were established by the hardware-proven
 * PARITY TEST2/TEST4 sysctl_tdname discovery path.
 */
constexpr long kSysctlSyscall = 202;
constexpr int kCtlKern = 1;
constexpr int kKernProc = 14;
constexpr int kKernProcProc = 8;
constexpr std::size_t kPidOffset = 72U;
constexpr std::size_t kTdnameOffset = 447U;
constexpr char kShellUiName[] = "SceShellUI";

struct ShellUiObservation {
    pid_t pid = -1;
    int size_query_rc = -1;
    int data_query_rc = -1;
    int saved_errno = 0;
    std::size_t bytes = 0;
    unsigned records = 0;
    unsigned malformed_records = 0;
};

void write_all(int fd, const char* data, std::size_t size) noexcept {
    while (size != 0) {
        const ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }
}

bool record_is_shellui(
    const std::uint8_t* record,
    std::size_t record_size) noexcept {

    if (!record || record_size < kTdnameOffset + sizeof(kShellUiName))
        return false;

    return std::memcmp(
        record + kTdnameOffset,
        kShellUiName,
        sizeof(kShellUiName)) == 0;
}

ShellUiObservation observe_shellui_once() noexcept {
    ShellUiObservation result{};
    int mib[4] = {
        kCtlKern,
        kKernProc,
        kKernProcProc,
        0,
    };

    std::size_t required = 0;
    result.size_query_rc = static_cast<int>(syscall(
        kSysctlSyscall,
        mib,
        4,
        nullptr,
        &required,
        nullptr,
        0));

    if (result.size_query_rc != 0 || required == 0) {
        result.saved_errno = errno;
        return result;
    }

    /* Allow the process list to grow between the two read-only queries. */
    const std::size_t capacity = required + required / 4U + 4096U;
    auto* buffer = static_cast<std::uint8_t*>(std::malloc(capacity));
    if (!buffer) {
        result.saved_errno = ENOMEM;
        return result;
    }

    result.bytes = capacity;
    result.data_query_rc = static_cast<int>(syscall(
        kSysctlSyscall,
        mib,
        4,
        buffer,
        &result.bytes,
        nullptr,
        0));

    if (result.data_query_rc != 0 || result.bytes > capacity) {
        result.saved_errno = result.bytes > capacity ? EOVERFLOW : errno;
        std::free(buffer);
        return result;
    }

    const pid_t self = getpid();
    const std::uint8_t* cursor = buffer;
    const std::uint8_t* const end = buffer + result.bytes;

    while (cursor < end) {
        const std::size_t remaining =
            static_cast<std::size_t>(end - cursor);
        if (remaining < sizeof(int)) {
            ++result.malformed_records;
            break;
        }

        int record_size_signed = 0;
        std::memcpy(
            &record_size_signed,
            cursor,
            sizeof(record_size_signed));

        if (record_size_signed <= 0 ||
            static_cast<std::size_t>(record_size_signed) > remaining) {
            ++result.malformed_records;
            break;
        }

        ++result.records;
        const std::size_t record_size =
            static_cast<std::size_t>(record_size_signed);

        if (record_size >= kPidOffset + sizeof(pid_t) &&
            record_is_shellui(cursor, record_size)) {
            pid_t candidate = -1;
            std::memcpy(
                &candidate,
                cursor + kPidOffset,
                sizeof(candidate));
            if (candidate > 0 && candidate != self) {
                result.pid = candidate;
                break;
            }
        }

        cursor += record_size;
    }

    std::free(buffer);
    return result;
}

void write_worker_ready_record(
    const ShellUiObservation& first) noexcept {

    const int fd = open(
        kControllerLog,
        O_WRONLY | O_CREAT | O_TRUNC | O_SYNC,
        0644);
    if (fd < 0)
        return;

    char record[1024]{};
    const int record_size = std::snprintf(
        record,
        sizeof(record),
        "Common FPS for PS5 v1.2.1\n"
        "Mode=loader-tracked internal_fork=absent spawned_pid=resident "
        "shellui_observation=sysctl_tdname_1s stability_gate=10 "
        "game_gate=process_present_stable_3s "
        "renderer_injection=deferred_until_game "
        "renderer=shared_elf_stopped_chain_hook method_writes=controller_only "
        "injection=target_stack_pthread ipc=udp_loopback_1s "
        "mono_gc=pinned sampler=videoout_preferred+dce_auth_adaptive standby=60s_reprobe "
        "read=mdbg+dce_ioctl highfw_guard=ptrace_io_1byte "
        "shutdown_trace=disabled "
        "shutdown_writes=disabled signal_handlers=default "
        "stop_path=disabled\n"
        "Worker ready pid=%d ppid=%d first_shellui_pid=%d "
        "size_rc=%d data_rc=%d errno=%d bytes=%zu records=%u "
        "malformed=%u log_fd=closing periodic_log=disabled "
        "platform_log=enabled\n",
        getpid(),
        getppid(),
        first.pid,
        first.size_query_rc,
        first.data_query_rc,
        first.saved_errno,
        first.bytes,
        first.records,
        first.malformed_records);

    if (record_size > 0) {
        const std::size_t safe_size =
            static_cast<std::size_t>(record_size) < sizeof(record)
                ? static_cast<std::size_t>(record_size)
                : sizeof(record) - 1;
        write_all(fd, record, safe_size);
    }

    (void)fsync(fd);
    (void)close(fd);
}

void append_sampler_policy_record(
    pid_t game_pid,
    const char* state,
    const char* backend,
    std::uint64_t retry_after_us) noexcept {

    const int fd = open(
        kControllerLog,
        O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
        0644);
    if (fd < 0)
        return;

    char record[320]{};
    const unsigned long long retry_ms =
        static_cast<unsigned long long>(retry_after_us / 1000ULL);
    const int record_size = std::snprintf(
        record,
        sizeof(record),
        "Sampler policy pid=%d state=%s backend=%s "
        "videoout_retry_after_ms=%llu\n",
        game_pid,
        state ? state : "unknown",
        backend ? backend : "unknown",
        retry_ms);

    if (record_size > 0) {
        const std::size_t safe_size =
            static_cast<std::size_t>(record_size) < sizeof(record)
                ? static_cast<std::size_t>(record_size)
                : sizeof(record) - 1;
        write_all(fd, record, safe_size);
    }

    (void)fsync(fd);
    (void)close(fd);
}

void append_first_fps_record(
    pid_t game_pid,
    std::uintptr_t counter_address,
    int fps,
    const char* backend) noexcept {

    const int fd = open(
        kControllerLog,
        O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
        0644);
    if (fd < 0)
        return;

    char record[256]{};
    const int record_size = std::snprintf(
        record,
        sizeof(record),
        "Sampler online pid=%d backend=%s counter=0x%llx first_fps=%d "
        "event_log=once_per_backend_per_game_pid log_fd=closing\n",
        game_pid,
        backend ? backend : "unknown",
        static_cast<unsigned long long>(counter_address),
        fps);

    if (record_size > 0) {
        const std::size_t safe_size =
            static_cast<std::size_t>(record_size) < sizeof(record)
                ? static_cast<std::size_t>(record_size)
                : sizeof(record) - 1;
        write_all(fd, record, safe_size);
    }

    (void)fsync(fd);
    (void)close(fd);
}

void append_settings_record(
    const char* source,
    const common_fps::OverlayConfig& cfg) noexcept {

    const int fd = open(
        kControllerLog,
        O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
        0644);
    if (fd < 0)
        return;

    char show[96]{};
    std::size_t used = 0;
    for (std::size_t i = 0;
         i < cfg.item_count && used < sizeof(show);
         ++i) {
        const int n = std::snprintf(
            show + used,
            sizeof(show) - used,
            "%s%s",
            i == 0 ? "" : ",",
            common_fps::item_name(cfg.items[i]));
        if (n <= 0)
            break;
        used += static_cast<std::size_t>(n);
    }

    char record[320]{};
    const int record_size = std::snprintf(
        record,
        sizeof(record),
        "Settings source=%s path=%s corner=%s show=%s labels=%d color=%d "
        "fps_good=%d fps_ok=%d font_size=%d margin_x=%d margin_y=%d "
        "visible=%d opacity=%d rotate=%d\n",
        source,
        kSettingsPath,
        common_fps::corner_name(cfg.corner),
        show,
        static_cast<int>(cfg.labels),
        cfg.color ? 1 : 0,
        cfg.fps_good,
        cfg.fps_ok,
        cfg.font_size,
        static_cast<int>(cfg.margin_x),
        static_cast<int>(cfg.margin_y),
        cfg.visible ? 1 : 0,
        cfg.opacity,
        cfg.rotate_minutes);

    if (record_size > 0) {
        const std::size_t safe_size =
            static_cast<std::size_t>(record_size) < sizeof(record)
                ? static_cast<std::size_t>(record_size)
                : sizeof(record) - 1;
        write_all(fd, record, safe_size);
    }

    (void)fsync(fd);
    (void)close(fd);
}

void append_rotate_record(common_fps::Corner corner) noexcept {
    const int fd = open(
        kControllerLog,
        O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
        0644);
    if (fd < 0)
        return;

    char record[64]{};
    const int record_size = std::snprintf(
        record,
        sizeof(record),
        "Rotate corner=%s\n",
        common_fps::corner_name(corner));
    if (record_size > 0 &&
        static_cast<std::size_t>(record_size) < sizeof(record))
        write_all(fd, record, static_cast<std::size_t>(record_size));

    (void)fsync(fd);
    (void)close(fd);
}

/*
 * The settings file as last seen. It is small, so it is read whole every
 * second and compared: that needs no file times, and catches an editor that
 * saves by writing a new file and renaming it over the old one.
 */
struct SettingsFile {
    char text[kSettingsMaxSize]{};
    std::size_t size = 0;
    bool loaded = false;
    bool defaults_logged = false;
    bool create_failed = false;
};

enum class SettingsRead {
    Ok,
    Missing,
    Failed,
};

SettingsRead read_settings_file(char* out, std::size_t& size) noexcept {
    const int fd = open(kSettingsPath, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? SettingsRead::Missing : SettingsRead::Failed;

    size = 0;
    bool ok = true;
    while (size < kSettingsMaxSize) {
        const ssize_t n = read(fd, out + size, kSettingsMaxSize - size);
        if (n > 0) {
            size += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        ok = n == 0;
        break;
    }

    (void)close(fd);
    return ok ? SettingsRead::Ok : SettingsRead::Failed;
}

/* Never replaces a file that is there: O_EXCL fails instead. */
bool write_default_settings_file() noexcept {
    const int fd = open(
        kSettingsPath,
        O_WRONLY | O_CREAT | O_EXCL,
        0666);
    if (fd < 0)
        return false;

    const char* text = common_fps::default_config_text();
    write_all(fd, text, std::strlen(text));
    (void)fsync(fd);
    (void)close(fd);
    return true;
}

/* True when cfg was replaced: the file changed, appeared or went missing. */
bool refresh_settings(
    SettingsFile& file,
    common_fps::OverlayConfig& cfg) noexcept {

    char text[kSettingsMaxSize];
    std::size_t size = 0;
    const SettingsRead result = read_settings_file(text, size);

    if (result == SettingsRead::Failed) {
        /* A read that failed half way says nothing: keep the settings in use. */
        return false;
    }

    if (result == SettingsRead::Missing) {
        const bool replaced = file.loaded || !file.defaults_logged;
        if (!file.create_failed)
            file.create_failed = !write_default_settings_file();
        if (replaced) {
            cfg = common_fps::default_config();
            append_settings_record("defaults", cfg);
        }
        file.loaded = false;
        file.defaults_logged = true;
        return replaced;
    }

    /* Compared by hand: the controller imports nothing Common FPS did not. */
    bool unchanged = file.loaded && size == file.size;
    for (std::size_t i = 0; unchanged && i < size; ++i)
        unchanged = text[i] == file.text[i];
    if (unchanged)
        return false;

    std::memcpy(file.text, text, size);
    file.size = size;
    file.loaded = true;
    cfg = common_fps::parse_config_text(std::string_view(text, size));
    append_settings_record("file", cfg);
    return true;
}

void claim_overlay() noexcept {
    const int fd = open(
        kOwnerPath,
        O_WRONLY | O_CREAT | O_TRUNC,
        0644);
    if (fd < 0)
        return;

    char text[16]{};
    const int size = std::snprintf(text, sizeof(text), "%d\n", getpid());
    if (size > 0 && static_cast<std::size_t>(size) < sizeof(text))
        write_all(fd, text, static_cast<std::size_t>(size));
    (void)fsync(fd);
    (void)close(fd);
}

/*
 * True once a later launch has written its pid. A file that is missing,
 * unreadable or half written (no line end yet) changes nothing.
 */
bool overlay_taken_over() noexcept {
    const int fd = open(kOwnerPath, O_RDONLY);
    if (fd < 0)
        return false;

    char text[16]{};
    const ssize_t size = read(fd, text, sizeof(text) - 1);
    (void)close(fd);
    if (size <= 0)
        return false;

    long owner = 0;
    ssize_t digits = 0;
    while (digits < size && text[digits] >= '0' && text[digits] <= '9') {
        owner = owner * 10 + (text[digits] - '0');
        ++digits;
    }
    if (digits == 0 || digits == size || text[digits] != '\n')
        return false;
    return owner != static_cast<long>(getpid());
}

void append_yield_record() noexcept {
    const int fd = open(
        kControllerLog,
        O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
        0644);
    if (fd < 0)
        return;

    char record[160]{};
    const int record_size = std::snprintf(
        record,
        sizeof(record),
        "Controller pid=%d stopped: a newer launch owns the overlay "
        "sends=stopped injection=stopped process=resident\n",
        getpid());

    if (record_size > 0) {
        const std::size_t safe_size =
            static_cast<std::size_t>(record_size) < sizeof(record)
                ? static_cast<std::size_t>(record_size)
                : sizeof(record) - 1;
        write_all(fd, record, safe_size);
    }

    (void)fsync(fd);
    (void)close(fd);
}

[[noreturn]] void run_tracked_worker() noexcept {
    const ShellUiObservation first = observe_shellui_once();
    write_worker_ready_record(first);
    claim_overlay();

    common_fps::ps5::Ps5Platform platform;
    common_fps::FpsSampler sampler(platform);
    common_fps::ps5::DceFpsSampler dce_sampler(platform);
    common_fps::ps5::StateSender sender;
    pid_t reported_pid = -1;
    pid_t dce_reported_pid = -1;
    pid_t stable_shellui_pid = -1;
    unsigned stable_shellui_observations = 0;
    pid_t stable_game_pid = -1;
    unsigned stable_game_observations = 0;
    bool dce_standby_active = false;
    pid_t dce_standby_pid = -1;
    unsigned dce_miss_streak = 0;
    std::uint64_t next_videoout_probe_us = 0;
    bool renderer_online = false;
    bool have_fps = false;
    int latest_fps = 0;
    int hl_avg = -1, hl_p1 = -1, hl_p02 = -1;   /* Heatline avg / 1% low / 0.2% low */
    std::uint64_t sequence = 1;
    common_fps::OverlayConfig overlay_config{};
    SettingsFile settings_file;
    common_fps::FpsStats session_stats;
    std::uint64_t session_start_us = 0;
    /* Corner rotation: how many corners on from the configured one, since when. */
    unsigned rotate_steps = 0;
    std::uint64_t rotate_since_us = platform.monotonic_us();

    /*
     * The controller remains in etaHEN's tracked process: no second fork,
     * signal handler, shutdown write or stop path is introduced.  Stage 8
     * requires ten identical one-second ShellUI observations before the first
     * injection.  A changed PID after rest mode restarts the same gate.
     */
    for (;;) {
        if (overlay_taken_over()) {
            /*
             * Like the rest of the controller, no exit path: stay resident,
             * but never sample, send or touch the home screen again.
             */
            append_yield_record();
            for (;;)
                platform.sleep_ms(60000);
        }

        if (refresh_settings(settings_file, overlay_config)) {
            rotate_steps = 0;
            rotate_since_us = platform.monotonic_us();
        }
        /*
         * Heatline fixed overlay layout: FPS, AVG, 1% low, 0.2% low, labelled
         * and colour-coded. Enforced every tick so a stale /data/simplefps.ini
         * (e.g. left by SimpleFPS) cannot change which metrics are shown. The
         * ini can still influence corner/font/opacity.
         */
        overlay_config.items[0] = common_fps::OverlayItem::Fps;
        overlay_config.items[1] = common_fps::OverlayItem::Avg;
        overlay_config.items[2] = common_fps::OverlayItem::P1;
        overlay_config.items[3] = common_fps::OverlayItem::P02;
        overlay_config.item_count = 4;
        overlay_config.labels = common_fps::LabelMode::On;
        overlay_config.color = true;
        /* Position and size come from Heatline's web page, not the ini. */
        {
            const auto style = common_fps::ps5::current_overlay_style();
            overlay_config.corner = style.corner;
            overlay_config.margin_x = static_cast<float>(style.margin_x);
            overlay_config.margin_y = static_cast<float>(style.margin_y);
            overlay_config.font_size = style.font_size;
            overlay_config.opacity = style.opacity;
            overlay_config.rotate_minutes = 0;   // no automatic corner rotation
        }
        if (overlay_config.rotate_minutes > 0) {
            const std::uint64_t now_us = platform.monotonic_us();
            const std::uint64_t period_us =
                static_cast<std::uint64_t>(overlay_config.rotate_minutes) *
                60'000'000ULL;
            if (now_us - rotate_since_us >= period_us) {
                rotate_steps = (rotate_steps + 1) % 4;
                rotate_since_us = now_us;
                append_rotate_record(common_fps::rotated_corner(
                    overlay_config.corner, rotate_steps));
            }
        } else {
            rotate_steps = 0;
        }

        const ShellUiObservation shellui = observe_shellui_once();
        if (shellui.pid > 0) {
            if (shellui.pid == stable_shellui_pid) {
                if (stable_shellui_observations < 10)
                    ++stable_shellui_observations;
            } else {
                stable_shellui_pid = shellui.pid;
                stable_shellui_observations = 1;
                renderer_online = false;
            }
        } else {
            stable_shellui_pid = -1;
            stable_shellui_observations = 0;
            renderer_online = false;
        }

        /*
         * Do not inject the ShellUI renderer on the home screen.  Earlier
         * hardware tests proved that touching Application.Update before a
         * game owns the "Game" container can make ShellUI restart.  The
         * controller remains loaded, but the renderer is started only after
         * the process sampler has observed the same real game process for
         * three consecutive one-second polls.  This also keeps autoload
         * completely inert between games and across reboot.
         */
        const auto observed_game_pid = platform.find_game_process();
        if (observed_game_pid) {
            if (*observed_game_pid == stable_game_pid) {
                if (stable_game_observations < 3)
                    ++stable_game_observations;
            } else {
                stable_game_pid = *observed_game_pid;
                stable_game_observations = 1;
                session_stats.reset();
                session_start_us = platform.monotonic_us();

                sampler.reset();
                reported_pid = -1;
                dce_sampler.reset();
                dce_reported_pid = -1;

                dce_standby_active = false;
                dce_standby_pid = -1;
                dce_miss_streak = 0;
                next_videoout_probe_us = 0;
            }
        } else {
            stable_game_pid = -1;
            stable_game_observations = 0;
            session_stats.reset();
            session_start_us = 0;

            sampler.reset();
            reported_pid = -1;
            dce_sampler.reset();
            dce_reported_pid = -1;

            dce_standby_active = false;
            dce_standby_pid = -1;
            dce_miss_streak = 0;
            next_videoout_probe_us = 0;
        }
        const bool game_process_ready =
            stable_game_pid > 0 && stable_game_observations >= 3;

        const std::uint64_t now_us = platform.monotonic_us();
        const bool videoout_probe_due =
            !dce_standby_active ||
            now_us >= next_videoout_probe_us;

        std::optional<int> process_fps;
#ifndef HEATLINE_NO_GAME_ATTACH
        if (sampler.attached()) {
            process_fps = sampler.sample();
        } else if (observed_game_pid && videoout_probe_due) {
            const bool attached = sampler.attach(*observed_game_pid);
            if (!attached && dce_standby_active) {
                next_videoout_probe_us =
                    now_us + kDceVideoOutStandbyUs;
            }
        }
#else
        /*
         * Heatline integration: NEVER ptrace-attach the running game.
         * The VideoOut sampler attaches the game process, which stops and
         * resumes it each poll and destabilises frametimes. With this define
         * the controller leaves process_fps empty and relies solely on the
         * DCE sampler below, which reads /dev/dce (the same source Heatline's
         * dashboard uses) without ever touching the game.
         */
        (void)process_fps;
        (void)videoout_probe_due;
#endif

        /*
         * v1.2.0 keeps VideoOut as the preferred source, but once the DCE
         * fallback has produced a valid FPS sample it suppresses expensive
         * read-only VideoOut discovery for 60 seconds.  A lightweight
         * periodic re-probe preserves the preferred path without rescanning
         * thousands of candidates every 10 seconds.
         */
        /*
         * Single metric source: Heatline's own sampler (fps.c), not a second
         * DCE reader in the engine. It gives the current FPS plus the CapFrameX
         * average, 1% low and 0.2% low that the dashboard also shows. The
         * engine's dce_sampler is left in place only so the standby bookkeeping
         * below keeps its shape; sample() is no longer called.
         */
        int hl_fps = 0;
        hl_avg = -1;
        hl_p1 = -1;
        hl_p02 = -1;
        const bool hl_have =
            game_process_ready &&
            fps_overlay_metrics(&hl_fps, &hl_avg, &hl_p1, &hl_p02) != 0;
        const auto dce_fps =
            hl_have ? std::optional<int>(hl_fps) : std::optional<int>{};

        if (process_fps) {
            have_fps = true;
            latest_fps = *process_fps;

            if (dce_standby_active) {
                append_sampler_policy_record(
                    sampler.pid(),
                    "videoout-resumed",
                    "videoout-process",
                    0);
            }
            dce_standby_active = false;
            dce_standby_pid = -1;
            dce_miss_streak = 0;
            next_videoout_probe_us = 0;

            if (sampler.pid() != reported_pid) {
                reported_pid = sampler.pid();
                append_first_fps_record(
                    reported_pid,
                    sampler.counter_address(),
                    *process_fps,
                    "videoout-process");
            }
        } else if (dce_fps && observed_game_pid) {
            have_fps = true;
            latest_fps = *dce_fps;
            dce_miss_streak = 0;

            if (!sampler.attached()) {
                const bool entering_standby =
                    !dce_standby_active ||
                    dce_standby_pid != *observed_game_pid;

                dce_standby_active = true;
                dce_standby_pid = *observed_game_pid;

                if (entering_standby) {
                    next_videoout_probe_us =
                        now_us + kDceVideoOutStandbyUs;
                    append_sampler_policy_record(
                        *observed_game_pid,
                        "dce-standby",
                        dce_sampler.backend_name(),
                        kDceVideoOutStandbyUs);
                }
            }

            if (*observed_game_pid != dce_reported_pid) {
                dce_reported_pid = *observed_game_pid;
                append_first_fps_record(
                    dce_reported_pid,
                    0,
                    *dce_fps,
                    dce_sampler.backend_name());
            }
        } else {
            have_fps = false;

            if (dce_standby_active && observed_game_pid) {
                ++dce_miss_streak;
                if (dce_miss_streak >=
                    kDceMissesBeforeVideoOutWake) {
                    append_sampler_policy_record(
                        *observed_game_pid,
                        "dce-miss-wake",
                        dce_sampler.backend_name(),
                        0);
                    dce_standby_active = false;
                    dce_standby_pid = -1;
                    dce_miss_streak = 0;
                    next_videoout_probe_us = 0;
                }
            }
        }

#ifndef HEATLINE_NO_INJECT
        if (!renderer_online && game_process_ready &&
            stable_shellui_observations >= 10) {
            renderer_online =
                common_fps::ps5::ensure_shellui_renderer();
        }
#else
        /*
         * Heatline isolation build: the ShellUI injection is disabled so the
         * sampler change can be tested on its own. The engine finds the game,
         * samples via DCE (no attach) and would send wire packets, but never
         * injects the renderer - so nothing draws on the TV and the home
         * screen is never touched. Toggling the overlay on in this build is a
         * pure "did the stutter go away?" test with no crash risk.
         */
        (void)stable_shellui_observations;
#endif

        if (have_fps)
            session_stats.add(latest_fps);

        if (renderer_online) {
            common_fps::OverlayFrame frame{};
            frame.visible = true;
            frame.loading = !have_fps;
            frame.fps = latest_fps;
            frame.config = overlay_config;
            frame.config.corner = common_fps::rotated_corner(
                overlay_config.corner, rotate_steps);
            frame.anchor = common_fps::compute_anchor(frame.config);

            common_fps::OverlayReadings readings{};
            readings.have_fps = have_fps;
            readings.fps = latest_fps;
            readings.avg = hl_avg;
            readings.p1 = hl_p1;
            readings.p02 = hl_p02;
            readings.stats = session_stats;
            readings.session_seconds = session_start_us != 0
                ? (platform.monotonic_us() - session_start_us) / 1'000'000ULL
                : 0;
            common_fps::MetricValues metric_values =
                common_fps::metric_values_from_readings(readings);
            if (overlay_collect_metrics) {
                /* Heatline fills in every metric it can read right now (FPS,
                 * temperatures, power, memory, network, ...). */
                double value[common_fps::kMetricCount]{};
                unsigned char have[common_fps::kMetricCount]{};
                if (overlay_collect_metrics(
                        value, have,
                        static_cast<int>(common_fps::kMetricCount)) == 0) {
                    for (std::size_t m = 0; m < common_fps::kMetricCount; ++m) {
                        metric_values.have[m] = have[m] != 0;
                        metric_values.value[m] = value[m];
                    }
                }
            }
            common_fps::build_overlay_from_spec(
                frame,
                common_fps::ps5::current_layout_spec(),
                metric_values);

            (void)sender.send(common_fps::make_wire_packet(frame, sequence++));
        }

        platform.sleep_ms(1000);
    }
}

} // namespace

/* Renderer log relay: the renderer (inside ShellUI) cannot write /data, so it
 * sends its log lines to this loopback UDP port and we append them here. */
static void* log_relay_thread(void*) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return nullptr;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(39029);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return nullptr;
    }
    char buf[300];
    for (;;) {
        const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        FILE* fp = std::fopen(kControllerLog, "a");
        if (fp) {
            std::fprintf(fp, "R %s\n", buf);
            std::fclose(fp);
        }
    }
}

extern "C" int main() {
    pthread_t relay;
    if (pthread_create(&relay, nullptr, log_relay_thread, nullptr) == 0)
        pthread_detach(relay);
    run_tracked_worker();
}
