/*
 * Heatline integration wrapper for the Common-FPS / SimpleFPS overlay engine.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Compiles the unmodified engine controller (commonfps_ps5_main.cpp) with its
 * entry renamed so it does not clash with Heatline's own main(), and exposes a
 * tiny C API the Heatline overlay toggle drives:
 *
 *   overlay_start()  - begin the engine's tracked worker on a detached thread.
 *                      Idempotent: starts at most once per process.
 *   overlay_stop()   - best effort. The engine worker is [[noreturn]] by design
 *                      (SimpleFPS has no off switch; a restart removes the
 *                      overlay), so this does not tear down an already-injected
 *                      renderer. Heatline persists the off state so the overlay
 *                      does not start on the next launch.
 */
#define main commonfps_overlay_worker
#include "commonfps_ps5_main.cpp"
#undef main

#include <pthread.h>

extern "C" int commonfps_overlay_worker();

namespace {
pthread_t g_thread{};
int       g_started = 0;

void* overlay_thread_main(void*) {
    commonfps_overlay_worker();   /* never returns */
    return nullptr;
}
} // namespace

extern "C" int
overlay_start(void) {
    if (g_started)
        return 0;
    if (pthread_create(&g_thread, nullptr, overlay_thread_main, nullptr) != 0)
        return -1;
    pthread_detach(g_thread);
    g_started = 1;
    return 0;
}

extern "C" void
overlay_stop(void) {
    /* No clean teardown of an injected renderer yet (engine worker is
     * [[noreturn]]); parity with SimpleFPS, where a restart removes it. */
}
