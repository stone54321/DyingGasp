#ifndef BLACKBOX_EVENT_LOG_H
#define BLACKBOX_EVENT_LOG_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t event_id;
    char     provider_name[64];
    char     time_created_utc[32];
    char     level_str[16];
    char     message_snippet[256];
} event_log_entry_t;

typedef struct {
    event_log_entry_t *entries;
    size_t             count;
    size_t             capacity;
    bool               is_available;
} event_log_result_t;

/**
 * @brief Initialize event log result structure.
 */
void event_log_result_init(event_log_result_t *res);

/**
 * @brief Free resources allocated in event log result structure.
 */
void event_log_result_free(event_log_result_t *res);

/**
 * @brief Query Windows Event Log for crash and hardware error events.
 *
 * Targets:
 * - Microsoft-Windows-Kernel-Power (Event ID 41)
 * - Microsoft-Windows-WER-SystemErrorReporting / BugCheck (Event ID 1001)
 * - Microsoft-Windows-WHEA-Logger (Event IDs 17, 18, 19, 41)
 *
 * Sets res->is_available to false if wevtapi is missing, EvtQuery fails, or under Wine.
 */
void event_log_query(event_log_result_t *res);

/**
 * @brief Print correlated event log table or graceful degradation message.
 */
void event_log_print_report(const event_log_result_t *res);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_EVENT_LOG_H */
