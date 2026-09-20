#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <windows.h>

#include "blackbox_format.h"
#include "blackbox_common.h"
#include "calibration.h"
#include "anomaly_engine.h"
#include "event_log.h"
#include "nvml_stub.h"
#include "ring_buffer.h"

/* CLI Options */
typedef struct {
    wchar_t ring_path[MAX_PATH];
    int     gap_index;      /* -1 if not specified, or >= 1 */
    bool    no_eventlog;
    bool    show_help;
} cli_options_t;

/* Dynamic Process Map */
typedef struct {
    uint32_t pid;
    char     name[32];
} pid_entry_t;

typedef struct {
    pid_entry_t *entries;
    size_t       count;
    size_t       capacity;
} pid_map_t;

static void pid_map_init(pid_map_t *map) {
    if (!map) return;
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void pid_map_free(pid_map_t *map) {
    if (!map) return;
    if (map->entries) {
        free(map->entries);
        map->entries = NULL;
    }
    map->count = 0;
    map->capacity = 0;
}

static void pid_map_put(pid_map_t *map, uint32_t pid, const char *name) {
    if (!map || !name) return;
    for (size_t i = 0; i < map->count; ++i) {
        if (map->entries[i].pid == pid) {
            strncpy(map->entries[i].name, name, sizeof(map->entries[i].name) - 1);
            map->entries[i].name[sizeof(map->entries[i].name) - 1] = '\0';
            return;
        }
    }

    if (map->count >= map->capacity) {
        size_t new_cap = (map->capacity == 0) ? 16 : (map->capacity * 2);
        pid_entry_t *new_entries = (pid_entry_t *)realloc(map->entries, new_cap * sizeof(pid_entry_t));
        if (!new_entries) return;
        map->entries = new_entries;
        map->capacity = new_cap;
    }

    pid_entry_t *entry = &map->entries[map->count++];
    entry->pid = pid;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
}

static const char *pid_map_lookup(const pid_map_t *map, uint32_t pid) {
    if (pid == 0) return "[None]";
    if (!map) return "[unknown]";
    for (size_t i = 0; i < map->count; ++i) {
        if (map->entries[i].pid == pid) {
            return map->entries[i].name;
        }
    }
    return "[unknown]";
}

/* CLI Parsing */
static void print_usage(const char *prog_name) {
    printf("Blackbox Post-Mortem Crash Analyzer\n");
    printf("Usage: %s [ring_path] [--ring=<path>] [--gap=N] [--no-eventlog] [-h|--help]\n\n", prog_name);
    printf("Arguments / Options:\n");
    printf("  ring_path, --ring=<path>  Path to ring file or directory to inspect (default: auto-discovery)\n");
    printf("  --gap=N                   Select 1-based gap index to analyze (default: latest gap)\n");
    printf("  --no-eventlog             Skip querying Windows System Event Log\n");
    printf("  -h, --help                Display this help message and exit\n");
}

static bool parse_cli_args(int argc, char *argv[], cli_options_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->gap_index = -1;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            opts->show_help = true;
            return true;
        } else if (strcmp(arg, "--no-eventlog") == 0) {
            opts->no_eventlog = true;
        } else if (strncmp(arg, "--gap=", 6) == 0) {
            opts->gap_index = atoi(arg + 6);
        } else if (strcmp(arg, "--gap") == 0) {
            if (i + 1 < argc) {
                opts->gap_index = atoi(argv[++i]);
            } else {
                fprintf(stderr, "Error: --gap requires an integer argument.\n");
                return false;
            }
        } else if (strncmp(arg, "--ring=", 7) == 0) {
            MultiByteToWideChar(CP_UTF8, 0, arg + 7, -1, opts->ring_path, MAX_PATH);
        } else if (strcmp(arg, "--ring") == 0) {
            if (i + 1 < argc) {
                MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, opts->ring_path, MAX_PATH);
            } else {
                fprintf(stderr, "Error: --ring requires a path argument.\n");
                return false;
            }
        } else if (strncmp(arg, "--ring-path=", 12) == 0) {
            MultiByteToWideChar(CP_UTF8, 0, arg + 12, -1, opts->ring_path, MAX_PATH);
        } else if (strncmp(arg, "--path=", 7) == 0) {
            MultiByteToWideChar(CP_UTF8, 0, arg + 7, -1, opts->ring_path, MAX_PATH);
        } else if (arg[0] != '-') {
            if (opts->ring_path[0] == L'\0') {
                MultiByteToWideChar(CP_UTF8, 0, arg, -1, opts->ring_path, MAX_PATH);
            }
        } else {
            fprintf(stderr, "Warning: Unrecognized option '%s'\n", arg);
        }
    }
    return true;
}

static int compare_filenames_desc_w(const void *a, const void *b) {
    const wchar_t *fa = (const wchar_t *)a;
    const wchar_t *fb = (const wchar_t *)b;
    return wcscmp(fb, fa); /* Descending order (newest timestamp first) */
}

static inline uint32_t blackbox_header_get_clean_shutdown(const blackbox_header_t *hdr) {
    if (!hdr) return 0;
    uint32_t val = 0;
    memcpy(&val, &hdr->reserved_padding[0], sizeof(uint32_t));
    return val;
}

static bool auto_discover_ring_file(const wchar_t *dir_path, wchar_t *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;

    /* 1. Search for ring-crash-*.bin */
    wchar_t search_pattern[MAX_PATH];
    if (dir_path && wcslen(dir_path) > 0 && wcscmp(dir_path, L".") != 0) {
        _snwprintf(search_pattern, MAX_PATH - 1, L"%ls\\ring-crash-*.bin", dir_path);
    } else {
        wcsncpy(search_pattern, L"ring-crash-*.bin", MAX_PATH - 1);
    }
    search_pattern[MAX_PATH - 1] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    wchar_t crash_files[128][MAX_PATH];
    int crash_count = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (crash_count < 128) {
                    wcsncpy(crash_files[crash_count], fd.cFileName, MAX_PATH - 1);
                    crash_files[crash_count][MAX_PATH - 1] = L'\0';
                    crash_count++;
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (crash_count > 0) {
        /* Sort descending so newest timestamp comes first */
        qsort(crash_files, (size_t)crash_count, sizeof(crash_files[0]), compare_filenames_desc_w);

        for (int i = 0; i < crash_count; ++i) {
            wchar_t candidate[MAX_PATH];
            if (dir_path && wcslen(dir_path) > 0 && wcscmp(dir_path, L".") != 0) {
                _snwprintf(candidate, MAX_PATH - 1, L"%ls\\%ls", dir_path, crash_files[i]);
            } else {
                wcsncpy(candidate, crash_files[i], MAX_PATH - 1);
            }
            candidate[MAX_PATH - 1] = L'\0';

            /* Inspect crash ring */
            ring_buffer_reader_t reader;
            if (ring_buffer_reader_open_w(&reader, candidate)) {
                uint64_t min_seq = 0, max_seq = 0, valid_count = 0;
                uint32_t wrap_count = 0, newest_slot = 0;
                bool has_bounds = ring_buffer_scan_bounds(&reader, &min_seq, &max_seq, &valid_count, &wrap_count, &newest_slot);
                uint32_t clean_shutdown = blackbox_header_get_clean_shutdown(&reader.header);
                ring_buffer_reader_close(&reader);

                if (has_bounds && valid_count > 0) {
                    /* Tail ends with a gap / crash cutoff: clean_shutdown == 0 or internal gap */
                    bool ends_with_gap = (clean_shutdown == 0) || (valid_count < (max_seq - min_seq + 1));
                    if (ends_with_gap) {
                        wcsncpy(out_path, candidate, max_len - 1);
                        out_path[max_len - 1] = L'\0';
                        return true;
                    }
                }
            }
        }
    }

    /* 2. Fallback: check for ring.bin in dir_path */
    wchar_t ring_bin[MAX_PATH];
    if (dir_path && wcslen(dir_path) > 0 && wcscmp(dir_path, L".") != 0) {
        _snwprintf(ring_bin, MAX_PATH - 1, L"%ls\\ring.bin", dir_path);
    } else {
        wcsncpy(ring_bin, L"ring.bin", MAX_PATH - 1);
    }
    ring_bin[MAX_PATH - 1] = L'\0';

    if (GetFileAttributesW(ring_bin) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy(out_path, ring_bin, max_len - 1);
        out_path[max_len - 1] = L'\0';
        return true;
    }

    return false;
}

static bool resolve_ring_path(cli_options_t *opts) {
    if (opts->ring_path[0] != L'\0') {
        DWORD attr = GetFileAttributesW(opts->ring_path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t discovered[MAX_PATH];
            if (auto_discover_ring_file(opts->ring_path, discovered, MAX_PATH)) {
                wcsncpy(opts->ring_path, discovered, MAX_PATH - 1);
                opts->ring_path[MAX_PATH - 1] = L'\0';
                return true;
            }
            return false;
        }
        return true;
    }

    /* Auto-discovery without CLI path */
    wchar_t discovered[MAX_PATH];

    /* 1. Try current working directory */
    if (auto_discover_ring_file(L".", discovered, MAX_PATH)) {
        wcsncpy(opts->ring_path, discovered, MAX_PATH - 1);
        opts->ring_path[MAX_PATH - 1] = L'\0';
        return true;
    }

    /* 2. Try %ProgramData%\blackbox */
    wchar_t prog_data[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t pb_dir[MAX_PATH];
        _snwprintf(pb_dir, MAX_PATH - 1, L"%ls\\blackbox", prog_data);
        pb_dir[MAX_PATH - 1] = L'\0';
        if (auto_discover_ring_file(pb_dir, discovered, MAX_PATH)) {
            wcsncpy(opts->ring_path, discovered, MAX_PATH - 1);
            opts->ring_path[MAX_PATH - 1] = L'\0';
            return true;
        }
    }

    /* 3. Try directory of blackbox-analyze.exe */
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe_path, MAX_PATH) > 0) {
        wchar_t *last_slash = wcsrchr(exe_path, L'\\');
        if (!last_slash) last_slash = wcsrchr(exe_path, L'/');
        if (last_slash) {
            *last_slash = L'\0';
            if (auto_discover_ring_file(exe_path, discovered, MAX_PATH)) {
                wcsncpy(opts->ring_path, discovered, MAX_PATH - 1);
                opts->ring_path[MAX_PATH - 1] = L'\0';
                return true;
            }
        }
    }

    return false;
}

/* Record sorting comparator */
static int compare_records(const void *a, const void *b) {
    const blackbox_record_t *ra = *(const blackbox_record_t * const *)a;
    const blackbox_record_t *rb = *(const blackbox_record_t * const *)b;
    uint64_t sa = blackbox_record_get_seq(ra);
    uint64_t sb = blackbox_record_get_seq(rb);
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    return 0;
}

/* Live NVML Hardware Delta Query */
static void query_live_nvml_replay(uint32_t last_recorded_replay) {
    printf("================================================================================\n");
    printf("                          LIVE HARDWARE RECOVERY DELTA\n");
    printf("================================================================================\n");

    HMODULE hNvml = LoadLibraryW(L"nvml.dll");
    if (!hNvml) {
        printf("Live Replay Counter: N/A (NVML unavailable)\n");
        printf("================================================================================\n\n");
        return;
    }

    pfn_nvmlInit_v2 pInit = (pfn_nvmlInit_v2)(void *)GetProcAddress(hNvml, "nvmlInit_v2");
    if (!pInit) pInit = (pfn_nvmlInit_v2)(void *)GetProcAddress(hNvml, "nvmlInit");
    pfn_nvmlShutdown pShutdown = (pfn_nvmlShutdown)(void *)GetProcAddress(hNvml, "nvmlShutdown");
    pfn_nvmlDeviceGetHandleByIndex_v2 pGetHandle = (pfn_nvmlDeviceGetHandleByIndex_v2)(void *)GetProcAddress(hNvml, "nvmlDeviceGetHandleByIndex_v2");
    if (!pGetHandle) pGetHandle = (pfn_nvmlDeviceGetHandleByIndex_v2)(void *)GetProcAddress(hNvml, "nvmlDeviceGetHandleByIndex");
    pfn_nvmlDeviceGetPcieReplayCounter pGetReplay = (pfn_nvmlDeviceGetPcieReplayCounter)(void *)GetProcAddress(hNvml, "nvmlDeviceGetPcieReplayCounter");

    if (!pInit || !pShutdown || !pGetHandle || !pGetReplay) {
        FreeLibrary(hNvml);
        printf("Live Replay Counter: N/A (NVML unavailable)\n");
        printf("================================================================================\n\n");
        return;
    }

    if (pInit() != NVML_SUCCESS) {
        FreeLibrary(hNvml);
        printf("Live Replay Counter: N/A (NVML unavailable)\n");
        printf("================================================================================\n\n");
        return;
    }

    nvmlDevice_t dev = NULL;
    unsigned int live_replay = 0;
    if (pGetHandle(0, &dev) == NVML_SUCCESS && pGetReplay(dev, &live_replay) == NVML_SUCCESS) {
        if (last_recorded_replay == BLACKBOX_SENTINEL_U32) {
            printf("Ring Last Recorded Replay Counter : N/A\n");
            printf("Current Live Hardware Replay Counter: %u\n", live_replay);
        } else {
            long long delta = (long long)live_replay - (long long)last_recorded_replay;
            printf("Ring Last Recorded Replay Counter : %u\n", last_recorded_replay);
            printf("Current Live Hardware Replay Counter: %u (Delta: %+lld)\n", live_replay, delta);
        }
    } else {
        printf("Live Replay Counter: N/A (NVML query failed)\n");
    }

    pShutdown();
    FreeLibrary(hNvml);
    printf("================================================================================\n\n");
}

int main(int argc, char *argv[]) {
    cli_options_t opts;
    if (!parse_cli_args(argc, argv, &opts)) {
        return 1;
    }

    if (opts.show_help) {
        print_usage(argv[0] ? argv[0] : "blackbox-analyze.exe");
        return 0;
    }

    if (!resolve_ring_path(&opts)) {
        fprintf(stderr, "Error: Ring file not found at default locations (%%ProgramData%%\\blackbox\\ring.bin, .\\ring.bin).\n");
        return 1;
    }

    char path_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, opts.ring_path, -1, path_utf8, sizeof(path_utf8), NULL, NULL);

    /* Open ring buffer file */
    HANDLE hFile = CreateFileW(
        opts.ring_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: Unable to open ring file '%s' (Win32 Error: %lu).\n",
                path_utf8, (unsigned long)GetLastError());
        return 1;
    }

    LARGE_INTEGER file_size_li;
    if (!GetFileSizeEx(hFile, &file_size_li)) {
        fprintf(stderr, "Error: Unable to determine file size.\n");
        CloseHandle(hFile);
        return 1;
    }

    uint64_t file_size = (uint64_t)file_size_li.QuadPart;
    if (file_size < BLACKBOX_HEADER_SIZE) {
        fprintf(stderr, "Error: File size (%llu bytes) smaller than 4 KB header.\n",
                (unsigned long long)file_size);
        CloseHandle(hFile);
        return 1;
    }

    /* Read and validate 4 KB header */
    blackbox_header_t hdr;
    DWORD bytes_read = 0;
    if (!ReadFile(hFile, &hdr, sizeof(hdr), &bytes_read, NULL) || bytes_read != sizeof(hdr)) {
        fprintf(stderr, "Error: Failed to read 4 KB header.\n");
        CloseHandle(hFile);
        return 1;
    }

    if (hdr.magic != BLACKBOX_MAGIC ||
        hdr.version != BLACKBOX_VERSION ||
        hdr.header_size != BLACKBOX_HEADER_SIZE ||
        hdr.record_size != BLACKBOX_RECORD_SIZE) {
        fprintf(stderr, "Error: Invalid blackbox ring buffer header (magic/version/geometry mismatch).\n");
        CloseHandle(hFile);
        return 1;
    }

    uint32_t record_count = hdr.record_count;
    if (record_count == 0) {
        fprintf(stderr, "Error: Header specifies 0 record slots.\n");
        CloseHandle(hFile);
        return 1;
    }

    /* Allocate slot storage and pointer array */
    blackbox_record_t *all_records = (blackbox_record_t *)calloc(record_count, sizeof(blackbox_record_t));
    const blackbox_record_t **valid_records = (const blackbox_record_t **)calloc(record_count, sizeof(blackbox_record_t *));
    if (!all_records || !valid_records) {
        fprintf(stderr, "Error: Failed to allocate memory for %u record slots.\n", record_count);
        if (all_records) free(all_records);
        if (valid_records) free(valid_records);
        CloseHandle(hFile);
        return 1;
    }

    pid_map_t pid_map;
    pid_map_init(&pid_map);

    bool has_torn = false;
    size_t valid_count = 0;

    /* Scan each physical slot */
    for (uint32_t slot = 0; slot < record_count; ++slot) {
        uint64_t offset = (uint64_t)BLACKBOX_HEADER_SIZE + ((uint64_t)slot * (uint64_t)BLACKBOX_RECORD_SIZE);
        if (offset + sizeof(blackbox_record_t) > file_size) {
            has_torn = true;
            break;
        }

        LARGE_INTEGER seek_pos;
        seek_pos.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(hFile, seek_pos, NULL, FILE_BEGIN)) {
            has_torn = true;
            break;
        }

        DWORD rec_read = 0;
        if (!ReadFile(hFile, &all_records[slot], sizeof(blackbox_record_t), &rec_read, NULL) ||
            rec_read != sizeof(blackbox_record_t)) {
            has_torn = true;
            break;
        }

        /* Check for empty / unwritten slot (all zeroes) */
        const uint64_t *p64 = (const uint64_t *)all_records[slot].raw;
        bool is_empty = true;
        for (size_t k = 0; k < sizeof(blackbox_record_t) / sizeof(uint64_t); ++k) {
            if (p64[k] != 0) {
                is_empty = false;
                break;
            }
        }
        if (is_empty) {
            continue;
        }

        /* Type bounds check */
        uint8_t rtype = all_records[slot].common.type;
        if (rtype > BLACKBOX_RECORD_TYPE_HEARTBEAT) {
            has_torn = true;
            continue;
        }

        /* Slot sequence alignment check */
        uint64_t seq = blackbox_record_get_seq(&all_records[slot]);
        if ((seq % (uint64_t)record_count) != (uint64_t)slot) {
            has_torn = true;
            continue;
        }

        /* Valid record */
        valid_records[valid_count++] = &all_records[slot];

        /* Update process map on Type 1 transition */
        if (rtype == BLACKBOX_RECORD_TYPE_PROCESS) {
            char safe_name[33];
            memcpy(safe_name, all_records[slot].process.name, 32);
            safe_name[32] = '\0';
            pid_map_put(&pid_map, all_records[slot].process.fg_pid, safe_name);
        }
    }

    CloseHandle(hFile);

    /* Sort valid records chronologically by monotonic sequence */
    if (valid_count > 1) {
        qsort(valid_records, valid_count, sizeof(blackbox_record_t *), compare_records);
    }

    /* Wrap count & time metrics */
    uint32_t wrap_count = 0;
    char first_time_str[32] = "N/A";
    char last_time_str[32] = "N/A";
    uint64_t min_seq = 0;
    uint64_t max_seq = 0;
    double actual_hz = (double)hdr.nominal_hz;

    if (valid_count > 0) {
        min_seq = blackbox_record_get_seq(valid_records[0]);
        max_seq = blackbox_record_get_seq(valid_records[valid_count - 1]);
        wrap_count = (uint32_t)(max_seq / (uint64_t)record_count);

        uint64_t first_qpc = blackbox_record_get_qpc_100ns(valid_records[0]);
        uint64_t last_qpc = blackbox_record_get_qpc_100ns(valid_records[valid_count - 1]);
        uint64_t first_ft = calibration_qpc_to_filetime(&hdr, first_qpc);
        uint64_t last_ft = calibration_qpc_to_filetime(&hdr, last_qpc);
        calibration_format_filetime_utc(first_ft, first_time_str, sizeof(first_time_str));
        calibration_format_filetime_utc(last_ft, last_time_str, sizeof(last_time_str));

        if (last_qpc > first_qpc && valid_count > 1) {
            double dt_sec = (double)(last_qpc - first_qpc) / 10000000.0;
            if (dt_sec > 0.0) {
                actual_hz = (double)valid_count / dt_sec;
            }
        }
    }

    /* Print Summary Line & Ring Health Summary */
    printf("Selected ring file for analysis: %s\n\n", path_utf8);

    printf("================================================================================\n");
    printf("                             RING HEALTH SUMMARY\n");
    printf("================================================================================\n");
    printf("Ring File Path        : %s\n", path_utf8);
    printf("Header Magic / Version: BLKBOX01 / Version %u\n", (unsigned int)hdr.version);
    printf("Total Ring Capacity   : %llu bytes (%u slots @ %u bytes/record)\n",
           (unsigned long long)hdr.ring_bytes,
           (unsigned int)record_count,
           (unsigned int)hdr.record_size);
    printf("Total Valid Records   : %llu\n", (unsigned long long)valid_count);
    printf("Buffer Wrap Count     : %u wrap(s)\n", (unsigned int)wrap_count);
    printf("First Record Time     : %s (Seq %llu)\n", first_time_str, (unsigned long long)min_seq);
    printf("Last Record Time      : %s (Seq %llu)\n", last_time_str, (unsigned long long)max_seq);
    printf("Nominal Sampling Rate : %.2f Hz\n", (double)hdr.nominal_hz);
    printf("Actual Sampling Rate  : %.2f Hz\n", actual_hz);
    printf("Active Process Map    : %zu distinct executables mapped\n", pid_map.count);
    printf("================================================================================\n\n");

    /* Scan for anomalies & gaps */
    anomaly_report_t anomalies;
    anomaly_report_init(&anomalies);
    anomalies.has_torn = has_torn;

    gap_list_t gaps;
    gap_list_init(&gaps);

    if (valid_count > 0) {
        anomaly_engine_scan(valid_records, valid_count, &hdr, &anomalies, &gaps);
    }

    /* Print Anomaly Report */
    anomaly_report_print(&anomalies);

    /* Determine inspection window */
    size_t start_idx = 0;
    size_t end_idx = 0;
    bool is_clean_tail = false;

    if (gaps.count == 0) {
        is_clean_tail = true;
        if (valid_count > 0) {
            end_idx = valid_count - 1;
            start_idx = (valid_count > 200) ? (valid_count - 200) : 0;
        }
    } else {
        gap_list_print(&gaps);

        size_t chosen_gap_idx = gaps.count - 1; /* Default to latest */
        if (opts.gap_index > 0) {
            if ((size_t)opts.gap_index <= gaps.count) {
                chosen_gap_idx = (size_t)opts.gap_index - 1;
                printf("Analyzing Gap #%d (selected via --gap=%d)...\n\n", opts.gap_index, opts.gap_index);
            } else {
                printf("Notice: Requested Gap #%d not found (total gaps: %zu). Analyzing latest Gap #%zu.\n\n",
                       opts.gap_index, gaps.count, gaps.count);
                chosen_gap_idx = gaps.count - 1;
            }
        } else {
            printf("Analyzing Gap #%zu (LATEST GAP) by default. Use --gap=N to inspect earlier gaps.\n\n", gaps.count);
        }

        const gap_entry_t *target_gap = &gaps.entries[chosen_gap_idx];
        end_idx = target_gap->pre_gap_record_idx;
        start_idx = (end_idx >= 199) ? (end_idx - 199) : 0;
    }

    /* Print Tabular Telemetry View */
    if (valid_count > 0) {
        printf("========================================================================================================================\n");
        printf("                                              TELEMETRY INSPECTION WINDOW\n");
        printf("========================================================================================================================\n");
        printf("+-------+-------------------------+---------+--------+----------+--------+---------+---------+------+----------------------+\n");
        printf("| SEQ   | WALL CLOCK (UTC)        | GPU PWR | TEMP   | PCIE     | REPLAY | SM CLK  | MEM CLK | CPU%% | FG PROCESS (PID)     |\n");
        printf("+-------+-------------------------+---------+--------+----------+--------+---------+---------+------+----------------------+\n");

        for (size_t idx = start_idx; idx <= end_idx; ++idx) {
            const blackbox_record_t *rec = valid_records[idx];
            uint64_t seq = blackbox_record_get_seq(rec);
            uint64_t qpc = blackbox_record_get_qpc_100ns(rec);
            uint64_t ft = calibration_qpc_to_filetime(&hdr, qpc);
            char time_str[32];
            calibration_format_filetime_utc(ft, time_str, sizeof(time_str));

            if (rec->common.type == BLACKBOX_RECORD_TYPE_DATA) {
                char pwr_str[32];
                if (rec->data.gpu_power_mw != BLACKBOX_SENTINEL_U32) {
                    snprintf(pwr_str, sizeof(pwr_str), "%5.1f W", (double)rec->data.gpu_power_mw / 1000.0);
                } else {
                    snprintf(pwr_str, sizeof(pwr_str), "  N/A  ");
                }

                char temp_str[32];
                if (rec->data.gpu_temp_c != BLACKBOX_SENTINEL_U16) {
                    snprintf(temp_str, sizeof(temp_str), "%3u C ", (unsigned int)rec->data.gpu_temp_c);
                } else {
                    snprintf(temp_str, sizeof(temp_str), " N/A  ");
                }

                char pcie_str[32];
                if (rec->data.pcie_gen != BLACKBOX_SENTINEL_U8 && rec->data.pcie_width != BLACKBOX_SENTINEL_U8) {
                    snprintf(pcie_str, sizeof(pcie_str), "Gen%u x%-2u", (unsigned int)rec->data.pcie_gen, (unsigned int)rec->data.pcie_width);
                } else {
                    snprintf(pcie_str, sizeof(pcie_str), "  N/A   ");
                }

                char rply_str[32];
                if (rec->data.pcie_replay != BLACKBOX_SENTINEL_U32) {
                    snprintf(rply_str, sizeof(rply_str), "%6u", (unsigned int)rec->data.pcie_replay);
                } else {
                    snprintf(rply_str, sizeof(rply_str), "   N/A");
                }

                char sm_str[32];
                if (rec->data.sm_clock_mhz != BLACKBOX_SENTINEL_U32) {
                    snprintf(sm_str, sizeof(sm_str), "%5uMHz", (unsigned int)rec->data.sm_clock_mhz);
                } else {
                    snprintf(sm_str, sizeof(sm_str), "   N/A  ");
                }

                char mem_str[32];
                if (rec->data.mem_clock_mhz != BLACKBOX_SENTINEL_U32) {
                    snprintf(mem_str, sizeof(mem_str), "%5uMHz", (unsigned int)rec->data.mem_clock_mhz);
                } else {
                    snprintf(mem_str, sizeof(mem_str), "   N/A  ");
                }

                char cpu_str[32];
                if (rec->data.cpu_pct != BLACKBOX_SENTINEL_U8) {
                    snprintf(cpu_str, sizeof(cpu_str), "%3u%%", (unsigned int)rec->data.cpu_pct);
                } else {
                    snprintf(cpu_str, sizeof(cpu_str), " N/A");
                }

                const char *pname = pid_map_lookup(&pid_map, rec->data.fg_pid);
                char fg_proc_str[64];
                if (rec->data.fg_pid != 0) {
                    snprintf(fg_proc_str, sizeof(fg_proc_str), "%.13s (%u)", pname, (unsigned int)rec->data.fg_pid);
                } else {
                    snprintf(fg_proc_str, sizeof(fg_proc_str), "[None] (0)");
                }

                printf("| %5llu | %-23s | %-7s | %-6s | %-8s | %-6s | %-7s | %-7s | %-4s | %-20s |",
                       (unsigned long long)seq,
                       time_str,
                       pwr_str,
                       temp_str,
                       pcie_str,
                       rply_str,
                       sm_str,
                       mem_str,
                       cpu_str,
                       fg_proc_str);
            } else if (rec->common.type == BLACKBOX_RECORD_TYPE_PROCESS) {
                char pname[33];
                memcpy(pname, rec->process.name, 32);
                pname[32] = '\0';
                printf("| %5llu | %-23s | [PROCESS TRANSITION] PID: %-5u -> %-43s |",
                       (unsigned long long)seq,
                       time_str,
                       (unsigned int)rec->process.fg_pid,
                       pname);
            } else if (rec->common.type == BLACKBOX_RECORD_TYPE_CALIBRATION) {
                printf("| %5llu | %-23s | [CLOCK CALIBRATION RESYNC] Mid-stream timer synchronization     |",
                       (unsigned long long)seq,
                       time_str);
            } else if (rec->common.type == BLACKBOX_RECORD_TYPE_HEARTBEAT) {
                printf("| %5llu | %-23s | [HEARTBEAT] Daemon Uptime: %-6u s, System CPU: %-3u%%              |",
                       (unsigned long long)seq,
                       time_str,
                       (unsigned int)rec->heartbeat.uptime_sec,
                       (unsigned int)rec->heartbeat.cpu_pct);
            }

            /* Check for anomaly annotation */
            const anomaly_entry_t *anom = anomaly_report_find_by_seq(&anomalies, seq);
            if (anom) {
                printf(" <-- [%s] %s", anom->tag, anom->details);
            }
            printf("\n");
        }

        printf("+-------+-------------------------+---------+--------+----------+--------+---------+---------+------+----------------------+\n");
    }

    if (is_clean_tail) {
        printf("\nNO GAP DETECTED (clean tail)\n\n");
    } else {
        printf("\nPre-gap telemetry window displayed (%zu records).\n\n", (end_idx >= start_idx) ? (end_idx - start_idx + 1) : 0);
    }

    /* Query live NVML hardware delta */
    uint32_t last_replay = BLACKBOX_SENTINEL_U32;
    if (valid_count > 0) {
        for (size_t k = valid_count; k > 0; --k) {
            if (valid_records[k - 1]->common.type == BLACKBOX_RECORD_TYPE_DATA) {
                last_replay = valid_records[k - 1]->data.pcie_replay;
                break;
            }
        }
    }
    query_live_nvml_replay(last_replay);

    /* Correlate Windows System Event Log */
    if (!opts.no_eventlog) {
        event_log_result_t ev_res;
        event_log_result_init(&ev_res);
        event_log_query(&ev_res);
        event_log_print_report(&ev_res);
        event_log_result_free(&ev_res);
    } else {
        printf("================================================================================\n");
        printf("                            WINDOWS SYSTEM EVENT LOG\n");
        printf("================================================================================\n");
        printf("Event Log: unavailable\n");
        printf("================================================================================\n\n");
    }

    /* Cleanup */
    anomaly_report_free(&anomalies);
    gap_list_free(&gaps);
    pid_map_free(&pid_map);
    free(all_records);
    free(valid_records);

    return 0;
}
