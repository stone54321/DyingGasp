#ifndef BLACKBOX_TIMER_LOOP_H
#define BLACKBOX_TIMER_LOOP_H

#include "blackbox_format.h"
#include "nvml_stub.h"
#include "ring_buffer.h"
#include "calibration.h"
#include "telemetry_nvml.h"
#include "telemetry_process.h"
#include <windows.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Offset in blackbox_header_t reserved_padding: 0x0058 */
#define BLACKBOX_HEADER_CLEAN_SHUTDOWN_OFFSET 0x0058

static inline uint32_t blackbox_header_get_clean_shutdown(const blackbox_header_t *hdr) {
    if (!hdr) return 0;
    uint32_t val = 0;
    memcpy(&val, &hdr->reserved_padding[0], sizeof(uint32_t));
    return val;
}

static inline void blackbox_header_set_clean_shutdown(blackbox_header_t *hdr, uint32_t val) {
    if (!hdr) return;
    memcpy(&hdr->reserved_padding[0], &val, sizeof(uint32_t));
}

static inline uint32_t blackbox_header_get_generation(const blackbox_header_t *hdr) {
    if (!hdr) return 0;
    uint32_t val = 0;
    memcpy(&val, &hdr->reserved_padding[4], sizeof(uint32_t));
    return val;
}

static inline void blackbox_header_set_generation(blackbox_header_t *hdr, uint32_t val) {
    if (!hdr) return;
    memcpy(&hdr->reserved_padding[4], &val, sizeof(uint32_t));
}

typedef struct {
    uint32_t hz;                         /**< Polling rate: 1..100 Hz (default 10) */
    HANDLE   h_stop_event;               /**< Event signaled when clean shutdown requested */
    uint64_t start_seq;                  /**< Starting sequence counter (0 or resumed max_seq + 1) */
    uint32_t generation;                 /**< Ring session generation counter */
    bool     is_resume;                  /**< True if resuming existing ring without Type 0 at start */
} timer_loop_config_t;

typedef struct {
    timer_loop_config_t  config;
    ring_buffer_writer_t *writer;
    calibration_state_t  *calibration;
    nvml_context_t       *nvml;
    process_monitor_t    proc_monitor;

    /* Timing */
    HANDLE               h_timer;
    uint64_t             nominal_interval_100ns;
    uint64_t             prev_tick_qpc_100ns;

    /* Sequence */
    uint64_t             seq;

    /* Cadence tracking */
    uint32_t             last_calib_sec;
    uint32_t             last_heartbeat_sec;

    /* CPU calculation state */
    ULARGE_INTEGER       prev_idle;
    ULARGE_INTEGER       prev_kernel;
    ULARGE_INTEGER       prev_user;
    bool                 has_prev_cpu;
} timer_loop_t;

/**
 * @brief Initialize timer loop resources and state.
 *
 * @param loop Timer loop instance to initialize.
 * @param config Configuration parameters (hz, stop event, start_seq, is_resume).
 * @param writer Pre-allocated ring buffer writer.
 * @param calibration Initialized calibration subsystem.
 * @param nvml Initialized NVML context.
 * @return true on success, false on error.
 */
bool timer_loop_init(timer_loop_t *loop,
                     const timer_loop_config_t *config,
                     ring_buffer_writer_t *writer,
                     calibration_state_t *calibration,
                     nvml_context_t *nvml);

/**
 * @brief Run the high-resolution collector timer loop until stop event is signaled.
 *
 * Invariants:
 * - Pure Win32 API (zero POSIX).
 * - Zero dynamic memory allocations (zero malloc/HeapAlloc).
 * - Zero printf / console I/O in the hot path.
 * - Zero locks / synchronization primitives in the hot path.
 * - Single thread.
 * - CPU < 0.5% of a single core at nominal frequency.
 * - All record buffers stack-allocated.
 *
 * Cadence:
 * - Type 2 (calibration): at startup and every 3600 seconds.
 * - Type 3 (heartbeat): every 10 seconds.
 * - Type 1 (process transition): whenever foreground process PID changes.
 * - Type 0 (telemetry data): all normal ticks.
 *
 * @param loop Initialized timer loop instance.
 */
void timer_loop_run(timer_loop_t *loop);

/**
 * @brief Clean up timer handles and resources.
 *
 * @param loop Timer loop instance to clean up.
 */
void timer_loop_cleanup(timer_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_TIMER_LOOP_H */
