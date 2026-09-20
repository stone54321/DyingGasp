#include "telemetry_process.h"
#include <stdio.h>
#include <string.h>

void telemetry_process_init(process_monitor_t *mon) {
    if (!mon) return;
    memset(mon, 0, sizeof(*mon));
}

bool telemetry_process_sample(process_monitor_t *mon,
                              DWORD *out_pid,
                              char out_name[32],
                              uint8_t *out_name_len,
                              bool *out_query_ok) {
    if (!mon) return false;

    HWND hwnd = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (hwnd != NULL) {
        GetWindowThreadProcessId(hwnd, &fg_pid);
    }

    bool is_transition = (!mon->is_initialized || fg_pid != mon->last_fg_pid);

    if (is_transition) {
        mon->is_initialized = true;
        mon->last_fg_pid = fg_pid;

        if (fg_pid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fg_pid);
            bool query_ok = false;
            mon->current_name[0] = '\0';

            if (hProc != NULL) {
                WCHAR path_w[MAX_PATH];
                DWORD path_len = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, path_w, &path_len) && path_len > 0) {
                    const WCHAR *base = wcsrchr(path_w, L'\\');
                    if (!base) {
                        base = wcsrchr(path_w, L'/');
                    }
                    base = base ? (base + 1) : path_w;

                    int res = WideCharToMultiByte(CP_UTF8, 0, base, -1,
                                                 mon->current_name,
                                                 (int)sizeof(mon->current_name),
                                                 NULL, NULL);
                    if (res > 0) {
                        mon->current_name[31] = '\0';
                        mon->current_name_len = (uint8_t)strlen(mon->current_name);
                        query_ok = true;
                    }
                }
                CloseHandle(hProc);
            }

            if (!query_ok) {
                /* Fallback name for denied or inaccessible process: "pid_<N>" */
                snprintf(mon->current_name, sizeof(mon->current_name), "pid_%lu", (unsigned long)fg_pid);
                mon->current_name[31] = '\0';
                mon->current_name_len = (uint8_t)strlen(mon->current_name);
                mon->last_query_ok = false;
            } else {
                mon->last_query_ok = true;
            }
        } else {
            /* fg_pid == 0 (Desktop, Lock Screen, or no window focused) */
            mon->current_name[0] = '\0';
            mon->current_name_len = 0;
            mon->last_query_ok = false;
        }
    }

    if (out_pid) {
        *out_pid = mon->last_fg_pid;
    }
    if (out_name) {
        memcpy(out_name, mon->current_name, 32);
        out_name[31] = '\0';
    }
    if (out_name_len) {
        *out_name_len = mon->current_name_len;
    }
    if (out_query_ok) {
        *out_query_ok = mon->last_query_ok;
    }

    return is_transition;
}
