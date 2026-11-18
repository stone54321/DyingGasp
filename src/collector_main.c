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
#include "cli.h"

/* Global stop event for console control handler */
static HANDLE g_hStopEvent = NULL;
static HANDLE g_hStoppedEvent = NULL;
static HANDLE g_hWindowReady = NULL;
static HWND g_shutdownWindow = NULL;

/* Loading user32 disables console logoff/shutdown notifications. An invisible
 * top-level window receives WM_ENDSESSION without changing the sampling loop. */
static LRESULT CALLBACK shutdown_window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_QUERYENDSESSION) return TRUE;
    if (message == WM_ENDSESSION && wp) {
        SetEvent(g_hStopEvent);
        WaitForSingleObject(g_hStoppedEvent, 4000);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static DWORD WINAPI shutdown_window_thread(LPVOID unused) {
    BLACKBOX_UNUSED(unused);
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = shutdown_window_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"BlackboxLogger.ShutdownListener";
    if (RegisterClassW(&cls)) {
        g_shutdownWindow = CreateWindowExW(0, cls.lpszClassName, L"Blackbox shutdown listener",
                                           0, 0, 0, 0, 0, NULL, NULL, cls.hInstance, NULL);
    }
    SetEvent(g_hWindowReady);
    if (g_shutdownWindow) {
        MSG message;
        while (GetMessageW(&message, NULL, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DestroyWindow(g_shutdownWindow);
    }
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    return 0;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_hStopEvent != NULL) {
                SetEvent(g_hStopEvent);
                if (ctrl_type != CTRL_C_EVENT && ctrl_type != CTRL_BREAK_EVENT && g_hStoppedEvent)
                    WaitForSingleObject(g_hStoppedEvent, 4000);
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
    bool args_ok = argv_w != NULL;
    for (int i = 1; args_ok && i < argc_w; ++i) {
        const wchar_t *arg = argv_w[i];
        const wchar_t *path = NULL;
        const wchar_t *number = NULL;
        uint64_t multiplier = 1;
        bool frequency = false;
        if (wcsncmp(arg, L"--ring-path=", 12) == 0) path = arg + 12;
        else if (wcsncmp(arg, L"--path=", 7) == 0) path = arg + 7;
        else if (wcsncmp(arg, L"--ring-size-mb=", 15) == 0) { number = arg + 15; multiplier = 1048576; }
        else if (wcsncmp(arg, L"--size-mb=", 10) == 0) { number = arg + 10; multiplier = 1048576; }
        else if (wcsncmp(arg, L"--ring-kb=", 10) == 0) { number = arg + 10; multiplier = 1024; }
        else if (wcsncmp(arg, L"--kb=", 5) == 0) { number = arg + 5; multiplier = 1024; }
        else if (wcsncmp(arg, L"--hz=", 5) == 0) { number = arg + 5; frequency = true; }
        else if (wcscmp(arg, L"--help") == 0 || wcscmp(arg, L"-h") == 0) {
            print_usage(argv_w[0]);
            LocalFree(argv_w);
            CloseHandle(hMutex);
            return 0;
        } else if (arg[0] != L'-') path = arg;
        else args_ok = false;

        if (path) {
            args_ok = !has_custom_path && cli_path(ring_path, MAX_PATH, path);
            has_custom_path = true;
        }
        if (number) {
            uint64_t value = 0;
            uint64_t maximum = frequency ? 100 :
                (BLACKBOX_HEADER_SIZE + (uint64_t)UINT32_MAX * BLACKBOX_RECORD_SIZE) / multiplier;
            args_ok = cli_uint(number, 1, maximum, &value);
            if (args_ok) {
                if (frequency) hz = (uint32_t)value;
                else ring_bytes = value * multiplier;
            }
        }
    }
    if (argv_w) LocalFree(argv_w);
    if (!args_ok || ring_bytes < BLACKBOX_HEADER_SIZE + 2 * BLACKBOX_RECORD_SIZE) {
        fprintf(stderr, "Error: Invalid arguments, path or ring size. Use --help.\n");
        CloseHandle(hMutex);
        return 1;
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
    blackbox_header_t previous_header;
    memset(&previous_header, 0, sizeof(previous_header));

    DWORD file_attr = GetFileAttributesW(ring_path);
    if (file_attr != INVALID_FILE_ATTRIBUTES && !(file_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ring_buffer_reader_t reader;
        if (ring_buffer_reader_open_w(&reader, ring_path) && reader.is_valid_header) {
            previous_header = reader.header;
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
                bool moved = false;
                /* Never replace an existing crash, even during rapid restarts. */
                for (unsigned attempt = 0; attempt < 1000; ++attempt) {
                    int length = _snwprintf(crash_path, MAX_PATH,
                        L"%ls\\ring-crash-%04u%02u%02u-%02u%02u%02u-%03u-%03u.bin",
                        dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                        st.wSecond, st.wMilliseconds, attempt);
                    if (length < 0 || length >= MAX_PATH) break;
                    if (MoveFileW(ring_path, crash_path)) { moved = true; break; }
                    DWORD error = GetLastError();
                    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) break;
                }

                if (moved) {
                    prune_old_crash_files(dir);
                    start_seq = 0;
                    generation = 0;
                    is_resume = false;
                } else {
                    fprintf(stderr, "Error: Cannot preserve crash ring; refusing to overwrite it.\n");
                    CloseHandle(hMutex);
                    return 1;
                }
            } else {
                /*
                 * Clean previous shutdown: Do NOT rotate to crash file.
                 * Continue writing to existing ring with generation bump and seq = (max seq) + 1.
                 */
                if (reader.file_size != ring_bytes || reader.header.ring_bytes != ring_bytes ||
                    reader.header.nominal_hz != hz) {
                    fprintf(stderr, "Error: Existing clean ring has different size/rate or is truncated. Use a new --path.\n");
                    ring_buffer_reader_close(&reader);
                    CloseHandle(hMutex);
                    return 1;
                }
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
            fprintf(stderr, "Error: Existing ring header is invalid; refusing to overwrite it.\n");
            CloseHandle(hMutex);
            return 1;
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
    if (is_resume) {
        header.qpc_freq = previous_header.qpc_freq;
        header.qpc_start_ticks = previous_header.qpc_start_ticks;
        header.qpc_start_100ns = previous_header.qpc_start_100ns;
        header.tsc_start = previous_header.tsc_start;
        header.filetime_start = previous_header.filetime_start;
    }
    blackbox_header_set_clean_shutdown(&header, 0); /* 0 while session is active */
    blackbox_header_set_generation(&header, generation);

    /* -------------------------------------------------------------------------
     * 6. Pre-allocate Ring Buffer File
     * ------------------------------------------------------------------------- */
    ring_buffer_writer_t writer;
    bool writer_ok = ring_buffer_writer_init_w(&writer, ring_path, ring_bytes, &header);

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

    g_hStoppedEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hWindowReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD window_thread_id = 0;
    HANDLE window_thread = NULL;
    if (g_hStoppedEvent && g_hWindowReady)
        window_thread = CreateThread(NULL, 0, shutdown_window_thread, NULL, 0, &window_thread_id);
    if (window_thread) WaitForSingleObject(g_hWindowReady, INFINITE);
    if (!window_thread || !g_shutdownWindow || !SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        fprintf(stderr, "Error: Failed to initialize shutdown notifications.\n");
        if (window_thread) {
            PostThreadMessageW(window_thread_id, WM_QUIT, 0, 0);
            WaitForSingleObject(window_thread, INFINITE);
            CloseHandle(window_thread);
        }
        if (g_hStoppedEvent) CloseHandle(g_hStoppedEvent);
        if (g_hWindowReady) CloseHandle(g_hWindowReady);
        CloseHandle(g_hStopEvent);
        ring_buffer_writer_close(&writer);
        CloseHandle(hMutex);
        return 1;
    }

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
    bool clean_exit = false;
    if (timer_loop_init(&loop, &loop_cfg, &writer, &cal, &nvml)) {
        clean_exit = timer_loop_run(&loop);
        if (!clean_exit) fprintf(stderr, "Error: Collector timer or disk write failed.\n");
        timer_loop_cleanup(&loop);
    } else {
        fprintf(stderr, "Error: Failed to initialize waitable timer loop.\n");
    }

    /* -------------------------------------------------------------------------
     * 10. Clean Shutdown & Resource Release
     * ------------------------------------------------------------------------- */
    /* Mark clean shutdown in 4KB header at offset 0x0058 before closing */
    if (clean_exit) {
        blackbox_header_set_clean_shutdown(&writer.header, 1);
        clean_exit = ring_buffer_writer_update_header(&writer, &writer.header);
        if (!clean_exit) fprintf(stderr, "Error: Failed to persist clean shutdown.\n");
    }

    telemetry_nvml_shutdown(&nvml);
    ring_buffer_writer_close(&writer);
    SetEvent(g_hStoppedEvent);
    PostThreadMessageW(window_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(window_thread, INFINITE);
    CloseHandle(window_thread);
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    CloseHandle(g_hWindowReady);
    /* The process owns g_hStoppedEvent until exit; an in-flight console handler
     * may still be waiting on it. */

    if (g_hStopEvent != NULL) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
    }

    if (hMutex != NULL) {
        CloseHandle(hMutex);
        hMutex = NULL;
    }

    return clean_exit ? 0 : 1;
}
