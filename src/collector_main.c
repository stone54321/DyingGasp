#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "blackbox_format.h"
#include "blackbox_common.h"
#include "ring_buffer.h"
#include "calibration.h"
#include "telemetry_nvml.h"
#include "telemetry_process.h"
#include "timer_loop.h"

/* Global stop event for console control handler */
static HANDLE g_hStopEvent = NULL;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_hStopEvent != NULL) {
                SetEvent(g_hStopEvent);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

static void print_usage(const wchar_t *prog_name) {
    wprintf(L"Usage: %ls [OPTIONS] [ring_path]\n\n", prog_name ? prog_name : L"blackbox.exe");
    wprintf(L"Options:\n");
    wprintf(L"  --ring-path=<path>, --path=<path>    Path to ring buffer file (default: %%ProgramData%%\\blackbox\\ring.bin)\n");
    wprintf(L"  --ring-size-mb=<N>, --size-mb=<N>    Total ring size in Megabytes (default: 64)\n");
    wprintf(L"  --ring-kb=<N>, --kb=<N>              Total ring size in Kilobytes\n");
    wprintf(L"  --hz=<1..100>                        Sampling frequency in Hz (default: 10)\n");
    wprintf(L"  -h, --help                           Display this help message and exit\n");
}

static int compare_filenames_w(const void *a, const void *b) {
    const wchar_t *sa = (const wchar_t *)a;
    const wchar_t *sb = (const wchar_t *)b;
    return wcscmp(sa, sb);
}

static void extract_directory(const wchar_t *file_path, wchar_t *out_dir, size_t max_len) {
    if (!file_path || !out_dir || max_len == 0) return;
    wcsncpy(out_dir, file_path, max_len - 1);
    out_dir[max_len - 1] = L'\0';
    wchar_t *last_slash = wcsrchr(out_dir, L'\\');
    if (!last_slash) {
        last_slash = wcsrchr(out_dir, L'/');
    }
    if (last_slash) {
        *last_slash = L'\0';
    } else {
        out_dir[0] = L'.';
        out_dir[1] = L'\0';
    }
}

static void prune_old_crash_files(const wchar_t *dir_path) {
    wchar_t search_pattern[MAX_PATH];
    if (dir_path && wcslen(dir_path) > 0 && wcscmp(dir_path, L".") != 0) {
        _snwprintf(search_pattern, MAX_PATH - 1, L"%ls\\ring-crash-*.bin", dir_path);
    } else {
        wcsncpy(search_pattern, L"ring-crash-*.bin", MAX_PATH - 1);
    }
    search_pattern[MAX_PATH - 1] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t files[128][MAX_PATH];
    int count = 0;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (count < 128) {
                wcsncpy(files[count], fd.cFileName, MAX_PATH - 1);
                files[count][MAX_PATH - 1] = L'\0';
                count++;
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (count <= 3) {
        return;
    }

    /* Sort ascending lexicographically (ring-crash-YYYYMMDD-HHMMSS.bin sorts chronologically) */
    qsort(files, (size_t)count, sizeof(files[0]), compare_filenames_w);

    /* Delete oldest files, keeping at most 3 newest */
    int to_delete = count - 3;
    for (int i = 0; i < to_delete; ++i) {
        wchar_t delete_path[MAX_PATH];
        if (dir_path && wcslen(dir_path) > 0 && wcscmp(dir_path, L".") != 0) {
            _snwprintf(delete_path, MAX_PATH - 1, L"%ls\\%ls", dir_path, files[i]);
        } else {
            wcsncpy(delete_path, files[i], MAX_PATH - 1);
        }
        delete_path[MAX_PATH - 1] = L'\0';
        DeleteFileW(delete_path);
    }
}

int main(int argc, char *argv[]) {
    BLACKBOX_UNUSED(argc);
    BLACKBOX_UNUSED(argv);

    /* -------------------------------------------------------------------------
     * 1. Mutex Enforcement: Single Instance Check
     * ------------------------------------------------------------------------- */
    HANDLE hMutex = CreateMutexW(NULL, FALSE, BLACKBOX_MUTEX_NAME_W);
    if (hMutex == NULL && GetLastError() == ERROR_ACCESS_DENIED) {
        /* Fallback to local session mutex if Global\ namespace is restricted */
        hMutex = CreateMutexW(NULL, FALSE, L"BlackboxLogger.SingleInstance");
    }

    if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "Error: blackbox daemon is already running.\n");
        if (hMutex != NULL) {
            CloseHandle(hMutex);
        }
        return 1;
    }

    /* -------------------------------------------------------------------------
     * 2. Parse CLI Arguments (using CommandLineToArgvW for full Unicode support)
     * ------------------------------------------------------------------------- */
    wchar_t ring_path[MAX_PATH] = {0};
    uint64_t ring_bytes = BLACKBOX_DEFAULT_RING_BYTES; /* 64 MB */
    uint32_t hz = 10;                                  /* 10 Hz */
    bool has_custom_path = false;

    int argc_w = 0;
    LPWSTR *argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (argv_w != NULL) {
        for (int i = 1; i < argc_w; ++i) {
            const wchar_t *arg = argv_w[i];

            if (wcsncmp(arg, L"--ring-path=", 12) == 0) {
                wcsncpy(ring_path, arg + 12, MAX_PATH - 1);
                ring_path[MAX_PATH - 1] = L'\0';
                has_custom_path = true;
            } else if (wcsncmp(arg, L"--path=", 7) == 0) {
                wcsncpy(ring_path, arg + 7, MAX_PATH - 1);
                ring_path[MAX_PATH - 1] = L'\0';
                has_custom_path = true;
            } else if (wcsncmp(arg, L"--ring-size-mb=", 15) == 0) {
                unsigned long mb = wcstoul(arg + 15, NULL, 10);
                if (mb > 0) {
                    ring_bytes = (uint64_t)mb * 1024ULL * 1024ULL;
                }
            } else if (wcsncmp(arg, L"--size-mb=", 10) == 0) {
                unsigned long mb = wcstoul(arg + 10, NULL, 10);
                if (mb > 0) {
                    ring_bytes = (uint64_t)mb * 1024ULL * 1024ULL;
                }
            } else if (wcsncmp(arg, L"--ring-kb=", 10) == 0) {
                unsigned long kb = wcstoul(arg + 10, NULL, 10);
                if (kb > 0) {
                    ring_bytes = (uint64_t)kb * 1024ULL;
                }
            } else if (wcsncmp(arg, L"--kb=", 5) == 0) {
                unsigned long kb = wcstoul(arg + 5, NULL, 10);
                if (kb > 0) {
                    ring_bytes = (uint64_t)kb * 1024ULL;
                }
            } else if (wcsncmp(arg, L"--hz=", 5) == 0) {
                unsigned long parsed_hz = wcstoul(arg + 5, NULL, 10);
                if (parsed_hz >= 1 && parsed_hz <= 100) {
                    hz = (uint32_t)parsed_hz;
                }
            } else if (wcscmp(arg, L"--help") == 0 || wcscmp(arg, L"-h") == 0) {
                print_usage(argv_w[0]);
                LocalFree(argv_w);
                CloseHandle(hMutex);
                return 0;
            } else if (arg[0] != L'-' && !has_custom_path) {
                wcsncpy(ring_path, arg, MAX_PATH - 1);
                ring_path[MAX_PATH - 1] = L'\0';
                has_custom_path = true;
            }
        }
        LocalFree(argv_w);
    }

    /* Clamp minimum ring size to header + 2 records */
    if (ring_bytes < (BLACKBOX_HEADER_SIZE + (2 * BLACKBOX_RECORD_SIZE))) {
        ring_bytes = BLACKBOX_HEADER_SIZE + (2 * BLACKBOX_RECORD_SIZE);
    }

    /* -------------------------------------------------------------------------
     * 3. Default Path Resolution & Directory Fallback
     * ------------------------------------------------------------------------- */
    if (!has_custom_path) {
        wchar_t prog_data[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH);
        if (len > 0 && len < (MAX_PATH - 32)) {
            _snwprintf(ring_path, MAX_PATH - 1, L"%ls\\blackbox\\ring.bin", prog_data);
            ring_path[MAX_PATH - 1] = L'\0';
        } else {
            /* Fallback to directory containing blackbox.exe */
            GetModuleFileNameW(NULL, ring_path, MAX_PATH);
            wchar_t *p = wcsrchr(ring_path, L'\\');
            if (!p) p = wcsrchr(ring_path, L'/');
            if (p) *(p + 1) = L'\0';
            wcsncat(ring_path, L"ring.bin", MAX_PATH - wcslen(ring_path) - 1);
        }
    }

    /* -------------------------------------------------------------------------
     * 4. Crash Detection & Atomic Ring Rotation
     * ------------------------------------------------------------------------- */
    bool is_resume = false;
    uint64_t start_seq = 0;
    uint32_t generation = 0;

    DWORD file_attr = GetFileAttributesW(ring_path);
    if (file_attr != INVALID_FILE_ATTRIBUTES && !(file_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ring_buffer_reader_t reader;
        if (ring_buffer_reader_open_w(&reader, ring_path) && reader.is_valid_header) {
            uint32_t prev_clean = blackbox_header_get_clean_shutdown(&reader.header);
            uint32_t prev_gen = blackbox_header_get_generation(&reader.header);

            if (prev_clean == 0) {
                /*
                 * Abrupt termination / crash / kill detected from previous run.
                 * Atomically rotate ring.bin to ring-crash-<YYYYMMDD-HHMMSS>.bin
                 */
                ring_buffer_reader_close(&reader);

                SYSTEMTIME st;
                GetSystemTime(&st);
                wchar_t dir[MAX_PATH];
                extract_directory(ring_path, dir, MAX_PATH);

                wchar_t crash_path[MAX_PATH];
                if (wcscmp(dir, L".") == 0) {
                    _snwprintf(crash_path, MAX_PATH - 1,
                               L"ring-crash-%04u%02u%02u-%02u%02u%02u.bin",
                               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                } else {
                    _snwprintf(crash_path, MAX_PATH - 1,
                               L"%ls\\ring-crash-%04u%02u%02u-%02u%02u%02u.bin",
                               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                }
                crash_path[MAX_PATH - 1] = L'\0';

                bool moved = MoveFileW(ring_path, crash_path);
                if (!moved) {
                    moved = MoveFileExW(ring_path, crash_path, MOVEFILE_REPLACE_EXISTING);
                }

                if (moved) {
                    prune_old_crash_files(dir);
                    start_seq = 0;
                    generation = 0;
                    is_resume = false;
                } else {
                    /*
                     * Fallback: If rename fails (locked/access), do NOT overwrite.
                     * Bump generation, continue seq from (max seq in ring) + 1.
                     */
                    generation = prev_gen + 1;
                    if (ring_buffer_reader_open_w(&reader, ring_path)) {
                        uint64_t min_s = 0, max_s = 0, valid_cnt = 0;
                        uint32_t wrp = 0, new_s = 0;
                        ring_buffer_scan_bounds(&reader, &min_s, &max_s, &valid_cnt, &wrp, &new_s);
                        ring_buffer_reader_close(&reader);
                        start_seq = (valid_cnt > 0) ? (max_s + 1) : 0;
                    }
                    is_resume = true;
                }
            } else {
                /*
                 * Clean previous shutdown: Do NOT rotate to crash file.
                 * Continue writing to existing ring with generation bump and seq = (max seq) + 1.
                 */
                generation = prev_gen + 1;
                uint64_t min_s = 0, max_s = 0, valid_cnt = 0;
                uint32_t wrp = 0, new_s = 0;
                ring_buffer_scan_bounds(&reader, &min_s, &max_s, &valid_cnt, &wrp, &new_s);
                ring_buffer_reader_close(&reader);
                start_seq = (valid_cnt > 0) ? (max_s + 1) : 0;
                is_resume = true;
            }
        } else {
            if (reader.h_file != INVALID_HANDLE_VALUE) {
                ring_buffer_reader_close(&reader);
            }
            start_seq = 0;
            generation = 0;
            is_resume = false;
        }
    }

    /* -------------------------------------------------------------------------
     * 5. Calibration Anchor Initialization
     * ------------------------------------------------------------------------- */
    calibration_state_t cal;
    if (!calibration_init(&cal, 20)) {
        fprintf(stderr, "Error: Failed to initialize system calibration anchors.\n");
        CloseHandle(hMutex);
        return 1;
    }

    blackbox_header_t header;
    calibration_populate_header(&cal, &header, ring_bytes, hz, GetCurrentProcessId());
    blackbox_header_set_clean_shutdown(&header, 0); /* 0 while session is active */
    blackbox_header_set_generation(&header, generation);

    /* -------------------------------------------------------------------------
     * 6. Pre-allocate Ring Buffer File
     * ------------------------------------------------------------------------- */
    ring_buffer_writer_t writer;
    bool writer_ok = ring_buffer_writer_init_w(&writer, ring_path, ring_bytes, &header);

    if (!writer_ok && !has_custom_path) {
        /* If default %ProgramData% path failed (unprivileged user), fallback to exe directory */
        wchar_t fallback_path[MAX_PATH];
        GetModuleFileNameW(NULL, fallback_path, MAX_PATH);
        wchar_t *p = wcsrchr(fallback_path, L'\\');
        if (!p) p = wcsrchr(fallback_path, L'/');
        if (p) *(p + 1) = L'\0';
        wcsncat(fallback_path, L"ring.bin", MAX_PATH - wcslen(fallback_path) - 1);

        wcsncpy(ring_path, fallback_path, MAX_PATH - 1);
        ring_path[MAX_PATH - 1] = L'\0';

        writer_ok = ring_buffer_writer_init_w(&writer, ring_path, ring_bytes, &header);
    }

    if (!writer_ok) {
        fprintf(stderr, "Error: Failed to pre-allocate ring buffer file.\n");
        CloseHandle(hMutex);
        return 1;
    }

    /* -------------------------------------------------------------------------
     * 7. Console Control Handler Setup
     * ------------------------------------------------------------------------- */
    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_hStopEvent == NULL) {
        fprintf(stderr, "Error: Failed to create shutdown signaling event.\n");
        ring_buffer_writer_close(&writer);
        CloseHandle(hMutex);
        return 1;
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    /* -------------------------------------------------------------------------
     * 8. Dynamic NVML Initialization (Graceful Fallback)
     * ------------------------------------------------------------------------- */
    nvml_context_t nvml;
    telemetry_nvml_init(&nvml);

    /* -------------------------------------------------------------------------
     * 9. Timer Loop Initialization & Execution
     * ------------------------------------------------------------------------- */
    timer_loop_config_t loop_cfg;
    loop_cfg.hz = hz;
    loop_cfg.h_stop_event = g_hStopEvent;
    loop_cfg.start_seq = start_seq;
    loop_cfg.generation = generation;
    loop_cfg.is_resume = is_resume;

    timer_loop_t loop;
    if (timer_loop_init(&loop, &loop_cfg, &writer, &cal, &nvml)) {
        timer_loop_run(&loop);
        timer_loop_cleanup(&loop);
    } else {
        fprintf(stderr, "Error: Failed to initialize waitable timer loop.\n");
    }

    /* -------------------------------------------------------------------------
     * 10. Clean Shutdown & Resource Release
     * ------------------------------------------------------------------------- */
    /* Mark clean shutdown in 4KB header at offset 0x0058 before closing */
    blackbox_header_set_clean_shutdown(&writer.header, 1);
    ring_buffer_writer_update_header(&writer, &writer.header);

    telemetry_nvml_shutdown(&nvml);
    ring_buffer_writer_close(&writer);

    if (g_hStopEvent != NULL) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
    }

    if (hMutex != NULL) {
        CloseHandle(hMutex);
        hMutex = NULL;
    }

    return 0;
}
