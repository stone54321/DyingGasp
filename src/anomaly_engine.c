#include "anomaly_engine.h"
#include "calibration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void anomaly_report_init(anomaly_report_t *report) {
    if (!report) return;
    report->entries = NULL;
    report->count = 0;
    report->capacity = 0;
    report->has_torn = false;
}

void anomaly_report_free(anomaly_report_t *report) {
    if (!report) return;
    if (report->entries) {
        free(report->entries);
        report->entries = NULL;
    }
    report->count = 0;
    report->capacity = 0;
    report->has_torn = false;
}

void anomaly_report_add(anomaly_report_t *report,
                        anomaly_type_t type,
                        uint64_t seq,
                        const char *tag,
                        const char *details) {
    if (!report) return;
    if (report->count >= report->capacity) {
        size_t new_cap = (report->capacity == 0) ? 16 : (report->capacity * 2);
        anomaly_entry_t *new_entries = (anomaly_entry_t *)realloc(report->entries, new_cap * sizeof(anomaly_entry_t));
        if (!new_entries) return;
        report->entries = new_entries;
        report->capacity = new_cap;
    }

    anomaly_entry_t *entry = &report->entries[report->count++];
    entry->type = type;
    entry->seq = seq;
    if (tag) {
        strncpy(entry->tag, tag, sizeof(entry->tag) - 1);
        entry->tag[sizeof(entry->tag) - 1] = '\0';
    } else {
        entry->tag[0] = '\0';
    }
    if (details) {
        strncpy(entry->details, details, sizeof(entry->details) - 1);
        entry->details[sizeof(entry->details) - 1] = '\0';
    } else {
        entry->details[0] = '\0';
    }
}

void gap_list_init(gap_list_t *list) {
    if (!list) return;
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

void gap_list_free(gap_list_t *list) {
    if (!list) return;
    if (list->entries) {
        free(list->entries);
        list->entries = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

void gap_list_add(gap_list_t *list, const gap_entry_t *gap) {
    if (!list || !gap) return;
    if (list->count >= list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 8 : (list->capacity * 2);
        gap_entry_t *new_entries = (gap_entry_t *)realloc(list->entries, new_cap * sizeof(gap_entry_t));
        if (!new_entries) return;
        list->entries = new_entries;
        list->capacity = new_cap;
    }

    list->entries[list->count++] = *gap;
}

void anomaly_engine_scan(const blackbox_record_t * const *records,
                         size_t count,
                         const blackbox_header_t *hdr,
                         anomaly_report_t *out_report,
                         gap_list_t *out_gaps) {
    if (!records || count < 2 || !hdr) return;

    uint32_t nominal_hz = (hdr->nominal_hz > 0) ? hdr->nominal_hz : 10;
    uint64_t nominal_100ns = 10000000ULL / (uint64_t)nominal_hz;

    for (size_t i = 1; i < count; ++i) {
        const blackbox_record_t *prev = records[i - 1];
        const blackbox_record_t *curr = records[i];

        uint64_t seq_prev = blackbox_record_get_seq(prev);
        uint64_t seq_curr = blackbox_record_get_seq(curr);
        uint64_t qpc_prev = blackbox_record_get_qpc_100ns(prev);
        uint64_t qpc_curr = blackbox_record_get_qpc_100ns(curr);

        /* 1. Gap Check (discontinuity in monotonic sequence) */
        if (seq_curr > seq_prev + 1) {
            uint64_t lost = seq_curr - seq_prev - 1;
            uint64_t dt_100ns = (qpc_curr >= qpc_prev) ? (qpc_curr - qpc_prev) : 0;
            double dur_sec = (double)dt_100ns / 10000000.0;
            uint64_t start_ft = calibration_qpc_to_filetime(hdr, qpc_prev);

            gap_entry_t gap;
            memset(&gap, 0, sizeof(gap));
            gap.gap_index = (out_gaps ? (uint32_t)(out_gaps->count + 1) : 1);
            gap.pre_gap_seq = seq_prev;
            gap.post_gap_seq = seq_curr;
            gap.pre_gap_record_idx = i - 1;
            gap.lost_records = lost;
            gap.duration_sec = dur_sec;
            gap.start_filetime = start_ft;
            calibration_format_filetime_utc(start_ft, gap.start_time_str, sizeof(gap.start_time_str));

            if (out_gaps) {
                gap_list_add(out_gaps, &gap);
            }
        }

        /* 2. Anomaly: HANG (> 3x nominal polling period) */
        if (qpc_curr > qpc_prev) {
            uint64_t dt_100ns = qpc_curr - qpc_prev;
            if (dt_100ns > 3 * nominal_100ns) {
                double dt_ms = (double)dt_100ns / 10000.0;
                double nom_ms = (double)nominal_100ns / 10000.0;
                char details[256];
                snprintf(details, sizeof(details),
                         "Interval %.1f ms exceeds 3x nominal (%.1f ms)",
                         dt_ms, nom_ms * 3.0);
                if (out_report) {
                    anomaly_report_add(out_report, ANOMALY_HANG, seq_curr, "HANG", details);
                }
            }
        }

        /* Telemetry comparison for adjacent Type 0 Data records */
        if (prev->common.type == BLACKBOX_RECORD_TYPE_DATA &&
            curr->common.type == BLACKBOX_RECORD_TYPE_DATA) {

            /* 3. Anomaly: POWER SPIKE (> 100 W = 100,000 mW delta) */
            uint32_t p0 = prev->data.gpu_power_mw;
            uint32_t p1 = curr->data.gpu_power_mw;
            if (p0 != BLACKBOX_SENTINEL_U32 && p1 != BLACKBOX_SENTINEL_U32) {
                long long delta = (long long)p1 - (long long)p0;
                if (llabs(delta) > 100000) {
                    char details[256];
                    snprintf(details, sizeof(details),
                             "Power jumped %+.1f W (%.1f W -> %.1f W)",
                             (double)delta / 1000.0,
                             (double)p0 / 1000.0,
                             (double)p1 / 1000.0);
                    if (out_report) {
                        anomaly_report_add(out_report, ANOMALY_POWER_SPIKE, seq_curr, "POWER SPIKE", details);
                    }
                }
            }

            /* 4. Anomaly: PCIE DROP (gen or width decreased) */
            uint8_t g0 = prev->data.pcie_gen;
            uint8_t w0 = prev->data.pcie_width;
            uint8_t g1 = curr->data.pcie_gen;
            uint8_t w1 = curr->data.pcie_width;
            if (g0 != BLACKBOX_SENTINEL_U8 && g1 != BLACKBOX_SENTINEL_U8 &&
                w0 != BLACKBOX_SENTINEL_U8 && w1 != BLACKBOX_SENTINEL_U8) {
                if (g1 < g0 || w1 < w0) {
                    char details[256];
                    snprintf(details, sizeof(details),
                             "PCIe link degraded from Gen%u x%u to Gen%u x%u",
                             (unsigned int)g0, (unsigned int)w0,
                             (unsigned int)g1, (unsigned int)w1);
                    if (out_report) {
                        anomaly_report_add(out_report, ANOMALY_PCIE_DROP, seq_curr, "PCIE DROP", details);
                    }
                }
            }

            /* 5. Anomaly: REPLAY JUMP (counter increased) */
            uint32_t r0 = prev->data.pcie_replay;
            uint32_t r1 = curr->data.pcie_replay;
            if (r0 != BLACKBOX_SENTINEL_U32 && r1 != BLACKBOX_SENTINEL_U32 && r1 > r0) {
                unsigned int delta = (unsigned int)(r1 - r0);
                char details[256];
                snprintf(details, sizeof(details),
                         "PCIe replay counter increased by +%u (%u -> %u)",
                         delta, (unsigned int)r0, (unsigned int)r1);
                if (out_report) {
                    anomaly_report_add(out_report, ANOMALY_REPLAY_JUMP, seq_curr, "REPLAY JUMP", details);
                }
            }
        }
    }
}

const anomaly_entry_t *anomaly_report_find_by_seq(const anomaly_report_t *report, uint64_t seq) {
    if (!report || !report->entries) return NULL;
    for (size_t i = 0; i < report->count; ++i) {
        if (report->entries[i].seq == seq) {
            return &report->entries[i];
        }
    }
    return NULL;
}

void anomaly_report_print(const anomaly_report_t *report) {
    printf("================================================================================\n");
    printf("                            AUTOMATED ANOMALY REPORT\n");
    printf("================================================================================\n");
    if (!report || (report->count == 0 && !report->has_torn)) {
        printf("No anomalies detected. Telemetry stream is continuous and nominal.\n");
    } else {
        for (size_t i = 0; i < report->count; ++i) {
            const anomaly_entry_t *entry = &report->entries[i];
            printf("[%s] Seq %llu: %s\n",
                   entry->tag,
                   (unsigned long long)entry->seq,
                   entry->details);
        }
        if (report->has_torn) {
            printf("[TORN RECORD] Corrupt or incomplete sequence/magic/type at ring boundary safely discarded\n");
        }
    }
    printf("================================================================================\n\n");
}

void gap_list_print(const gap_list_t *list) {
    if (!list || list->count == 0) return;

    printf("================================================================================\n");
    printf("                            DETECTED TIMELINE GAPS\n");
    printf("================================================================================\n");
    printf("Gap # | Start Wall Time (UTC)   | Pre-Gap Seq | Post-Gap Seq | Duration  | Lost Records\n");
    printf("------+-------------------------+-------------+--------------+-----------+-------------\n");
    for (size_t i = 0; i < list->count; ++i) {
        const gap_entry_t *g = &list->entries[i];
        printf("  %3u | %-23s | %11llu | %12llu | %8.2f s | %llu\n",
               g->gap_index,
               g->start_time_str,
               (unsigned long long)g->pre_gap_seq,
               (unsigned long long)g->post_gap_seq,
               g->duration_sec,
               (unsigned long long)g->lost_records);
    }
    printf("================================================================================\n\n");
}
