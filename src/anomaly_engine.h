#ifndef BLACKBOX_ANOMALY_ENGINE_H
#define BLACKBOX_ANOMALY_ENGINE_H

#include "blackbox_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANOMALY_NONE = 0,
    ANOMALY_HANG,
    ANOMALY_POWER_SPIKE,
    ANOMALY_PCIE_DROP,
    ANOMALY_REPLAY_JUMP,
    ANOMALY_TORN_RECORD
} anomaly_type_t;

typedef struct {
    anomaly_type_t type;
    uint64_t       seq;
    char           tag[32];      /* e.g. "HANG", "POWER SPIKE", "PCIE DROP", "REPLAY JUMP" */
    char           details[256]; /* Diagnostic message */
} anomaly_entry_t;

typedef struct {
    anomaly_entry_t *entries;
    size_t           count;
    size_t           capacity;
    bool             has_torn;
} anomaly_report_t;

typedef struct {
    uint32_t gap_index;          /* 1-based index */
    uint64_t pre_gap_seq;
    uint64_t post_gap_seq;
    size_t   pre_gap_record_idx; /* Index in sorted valid_records array */
    uint64_t lost_records;
    double   duration_sec;
    uint64_t start_filetime;
    char     start_time_str[32]; /* ISO 8601 UTC string */
} gap_entry_t;

typedef struct {
    gap_entry_t *entries;
    size_t       count;
    size_t       capacity;
} gap_list_t;

/**
 * @brief Initialize anomaly report container.
 */
void anomaly_report_init(anomaly_report_t *report);

/**
 * @brief Free memory associated with anomaly report.
 */
void anomaly_report_free(anomaly_report_t *report);

/**
 * @brief Add an anomaly entry to the report.
 */
void anomaly_report_add(anomaly_report_t *report,
                        anomaly_type_t type,
                        uint64_t seq,
                        const char *tag,
                        const char *details);

/**
 * @brief Initialize gap list container.
 */
void gap_list_init(gap_list_t *list);

/**
 * @brief Free memory associated with gap list.
 */
void gap_list_free(gap_list_t *list);

/**
 * @brief Add a gap entry to the gap list.
 */
void gap_list_add(gap_list_t *list, const gap_entry_t *gap);

/**
 * @brief Scan sorted records stream to catalog timeline gaps and anomalies.
 *
 * @param records Array of pointers to valid records, sorted chronologically by sequence.
 * @param count Number of valid records in array.
 * @param hdr Pointer to file header containing nominal_hz and calibration triple.
 * @param out_report Populated anomaly report.
 * @param out_gaps Populated gap list.
 */
void anomaly_engine_scan(const blackbox_record_t * const *records,
                         size_t count,
                         const blackbox_header_t *hdr,
                         anomaly_report_t *out_report,
                         gap_list_t *out_gaps);

/**
 * @brief Check if a record has any anomaly tag at the specified sequence.
 *
 * @return Anomaly entry pointer or NULL if none.
 */
const anomaly_entry_t *anomaly_report_find_by_seq(const anomaly_report_t *report, uint64_t seq);

/**
 * @brief Print formatted anomaly report to stdout.
 */
void anomaly_report_print(const anomaly_report_t *report);

/**
 * @brief Print formatted gap directory list to stdout.
 */
void gap_list_print(const gap_list_t *list);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_ANOMALY_ENGINE_H */
