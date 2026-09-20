#ifndef BLACKBOX_CALIBRATION_H
#define BLACKBOX_CALIBRATION_H

#include "blackbox_format.h"
#include "blackbox_common.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Calibration State & Anchors                                               */
/* ========================================================================= */

typedef struct {
    uint64_t qpc_freq;           /**< QueryPerformanceFrequency (ticks per second) */
    uint64_t qpc_start_ticks;    /**< Initial raw QPC counter at startup */
    uint64_t qpc_start_100ns;    /**< Normalized initial QPC in 100ns units */
    uint64_t tsc_start;          /**< Initial __rdtsc() cycle counter */
    uint64_t filetime_start;     /**< Windows FILETIME (UTC 100ns intervals since 1601) */
    uint64_t tsc_freq_hz;        /**< Calibrated TSC frequency (cycles per second) */
} calibration_state_t;

/**
 * @brief Initialize calibration subsystem, capturing initial anchors and measuring TSC frequency.
 *
 * @param state Calibration state struct to populate.
 * @param measure_ms Milliseconds to sample for TSC frequency measurement (e.g. 20..50 ms).
 * @return true on success, false on error.
 */
bool calibration_init(calibration_state_t *state, uint32_t measure_ms);

/**
 * @brief Populate a blackbox_header_t calibration fields from an active calibration state.
 */
void calibration_populate_header(const calibration_state_t *state,
                                 blackbox_header_t *hdr,
                                 uint64_t ring_bytes,
                                 uint32_t nominal_hz,
                                 uint32_t daemon_pid);

/**
 * @brief Capture a fresh calibration triple (for Type 2 records or resync).
 */
void calibration_capture_triple(const calibration_state_t *state,
                                uint64_t *out_tsc,
                                uint64_t *out_qpc_100ns,
                                uint64_t *out_filetime);

/**
 * @brief Convert raw QPC counter ticks to normalized 100-nanosecond units.
 */
uint64_t calibration_qpc_ticks_to_100ns(uint64_t ticks, uint64_t freq);

/**
 * @brief Convert record QPC 100ns timestamp to Windows FILETIME using calibration anchor.
 *
 * Formula: filetime = filetime_start + (qpc_100ns - qpc_start_100ns)
 */
uint64_t calibration_qpc_to_filetime(const blackbox_header_t *hdr, uint64_t qpc_100ns);

/**
 * @brief Convert record TSC cycle timestamp to Windows FILETIME using calibration anchor.
 */
uint64_t calibration_tsc_to_filetime(const blackbox_header_t *hdr, uint64_t tsc, uint64_t tsc_freq_hz);

/**
 * @brief Format a 64-bit FILETIME value into ISO 8601 UTC string ("YYYY-MM-DD HH:MM:SS.mmm UTC").
 */
bool calibration_format_filetime_utc(uint64_t filetime, char *buf, size_t buf_size);

/**
 * @brief Format a 64-bit FILETIME value into local time string ("YYYY-MM-DD HH:MM:SS.mmm").
 */
bool calibration_format_filetime_local(uint64_t filetime, char *buf, size_t buf_size);

/**
 * @brief Convert Windows FILETIME to Unix timestamp milliseconds.
 */
uint64_t calibration_filetime_to_unix_ms(uint64_t filetime);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_CALIBRATION_H */
