/*
 * shellui_attach_probe.hpp - minimal, guaranteed-resume attach/detach probe.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The upstream Common-FPS release references this header only under the
 * COMMON_FPS_TEST26_ATTACH_DETACH_ONLY diagnostic build, but does not ship the
 * author's private test harness that defined it. Heatline recreates the
 * minimal probe here, using the SAME primitives the real injector uses
 * (kernel_{get,set}_ucred_authid + pt_attach/pt_detach), so the attach
 * primitive can be tested in isolation on the console.
 *
 * What it does: raise this process's auth to the ptrace auth id, ptrace-attach
 * SceShellUI (which STOPS it), then immediately detach (which RESUMES it),
 * also send SIGCONT as a second resume, and restore the original auth. It
 * NEVER loads code into ShellUI, writes its memory, or creates a thread there.
 *
 * Safety: whenever the attach succeeds, the detach is always attempted, and a
 * SIGCONT is sent regardless, so this probe does not leave ShellUI stopped.
 */
#pragma once

#include <sys/types.h>
#include <csignal>
#include <unistd.h>

extern "C" {
#include "pt.h"
}
#include <ps5/kernel.h>

namespace common_fps::ps5 {

struct ShellUiAttachProbeResult {
    bool auth_changed  = false;  // raised own auth to the ptrace auth id
    bool auth_restored = false;  // restored original auth afterwards
    bool attached      = false;  // pt_attach succeeded (ShellUI stopped)
    bool detached      = false;  // pt_detach succeeded (ShellUI resumed)
    bool sigcont_sent  = false;  // SIGCONT delivered as a second resume
};

inline ShellUiAttachProbeResult
probe_shellui_attach_detach(pid_t pid) {
    constexpr unsigned long long kPtraceAuthId = 0x4800000000010003ULL;

    ShellUiAttachProbeResult r{};
    const pid_t self = getpid();

    const unsigned long long original_auth = kernel_get_ucred_authid(self);
    if (original_auth == 0)
        return r;                       // cannot read own auth; do nothing

    if (kernel_set_ucred_authid(self, kPtraceAuthId) != 0)
        return r;                       // could not raise auth; nothing attached
    r.auth_changed = true;

    if (pt_attach(pid) == 0) {
        r.attached = true;
        r.detached     = (pt_detach(pid, 0) == 0);   // always resume
        r.sigcont_sent = (kill(pid, SIGCONT) == 0);  // belt-and-suspenders
    }

    if (kernel_set_ucred_authid(self, original_auth) == 0 &&
        kernel_get_ucred_authid(self) == original_auth) {
        r.auth_restored = true;
    }
    return r;
}

} // namespace common_fps::ps5
