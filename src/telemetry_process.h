#ifndef BLACKBOX_TELEMETRY_PROCESS_H
#define BLACKBOX_TELEMETRY_PROCESS_H

#include "blackbox_format.h"
#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility alias for flags */
#ifndef BLACKBOX_FLAG_FG_OK
#define BLACKBOX_FLAG_FG_OK BLACKBOX_FLAG_FG_QUERY_OK
#endif

typedef struct {
    DWORD   last_fg_pid;
    bool    is_initialized;
    char    current_name[32];
    uint8_t current_name_len;
    bool    last_query_ok;
} process_monitor_t;

/**
 * @brief Initialize foreground process monitoring state.
 *
 * @param mon Monitor structure to initialize.
 */
void telemetry_process_init(process_monitor_t *mon);

/**
 * @brief Sample the current foreground window and process.
 *
 * Checks GetForegroundWindow -> GetWindowThreadProcessId.
 * If PID changed since last sample (process transition):
 *   - Calls OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) and QueryFullProcessImageNameW.
 *   - Extracts basename, truncating to 31 chars + NUL.
 *   - If query fails/denied, formats fallback "pid_<N>" and clears query_ok.
 *
 * @param mon Initialized process monitor state.
 * @param out_pid Receives active foreground PID (0 if none / desktop).
 * @param out_name Buffer receiving process basename (minimum 32 bytes).
 * @param out_name_len Receives string length of basename (<= 31).
 * @param out_query_ok Receives true if process name query succeeded, false if fallback/denied.
 * @return true if a process transition occurred (PID changed), false if unchanged.
 */
bool telemetry_process_sample(process_monitor_t *mon,
                              DWORD *out_pid,
                              char out_name[32],
                              uint8_t *out_name_len,
                              bool *out_query_ok);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_TELEMETRY_PROCESS_H */
