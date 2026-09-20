#include "event_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dynamic prototypes for wevtapi.dll */
typedef HANDLE EVT_HANDLE;

typedef EVT_HANDLE (WINAPI *pfn_EvtQuery)(
    EVT_HANDLE Session,
    LPCWSTR    Path,
    LPCWSTR    Query,
    DWORD      Flags
);

typedef BOOL (WINAPI *pfn_EvtNext)(
    EVT_HANDLE  ResultSet,
    DWORD       EventsSize,
    EVT_HANDLE *Events,
    DWORD       Timeout,
    DWORD       Flags,
    DWORD      *Returned
);

typedef BOOL (WINAPI *pfn_EvtRender)(
    EVT_HANDLE Context,
    EVT_HANDLE Fragment,
    DWORD      Flags,
    DWORD      BufferSize,
    PVOID      Buffer,
    DWORD     *BufferUsed,
    DWORD     *PropertyCount
);

typedef BOOL (WINAPI *pfn_EvtClose)(EVT_HANDLE Object);

#define EVT_QUERY_CHANNEL_PATH       0x1
#define EVT_QUERY_REVERSE_DIRECTION  0x200
#define EVT_RENDER_EVENT_XML         1

void event_log_result_init(event_log_result_t *res) {
    if (!res) return;
    res->entries = NULL;
    res->count = 0;
    res->capacity = 0;
    res->is_available = false;
}

void event_log_result_free(event_log_result_t *res) {
    if (!res) return;
    if (res->entries) {
        free(res->entries);
        res->entries = NULL;
    }
    res->count = 0;
    res->capacity = 0;
    res->is_available = false;
}

static void parse_xml_event(const wchar_t *xml_w, event_log_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));

    /* Extract EventID */
    const wchar_t *p = wcsstr(xml_w, L"<EventID");
    if (p) {
        const wchar_t *p_close = wcschr(p, L'>');
        if (p_close) {
            entry->event_id = (uint32_t)_wtoi(p_close + 1);
        }
    }

    /* Extract Provider Name */
    p = wcsstr(xml_w, L"<Provider");
    if (p) {
        const wchar_t *p_name = wcsstr(p, L"Name='");
        wchar_t quote_char = L'\'';
        if (!p_name) {
            p_name = wcsstr(p, L"Name=\"");
            quote_char = L'\"';
        }
        if (p_name) {
            p_name += 6;
            const wchar_t *p_end = wcschr(p_name, quote_char);
            if (p_end) {
                int len = (int)(p_end - p_name);
                if (len > 63) len = 63;
                WideCharToMultiByte(CP_UTF8, 0, p_name, len, entry->provider_name, sizeof(entry->provider_name) - 1, NULL, NULL);
            }
        }
    }

    /* Extract TimeCreated SystemTime */
    p = wcsstr(xml_w, L"<TimeCreated");
    if (p) {
        const wchar_t *p_time = wcsstr(p, L"SystemTime='");
        wchar_t quote_char = L'\'';
        if (!p_time) {
            p_time = wcsstr(p, L"SystemTime=\"");
            quote_char = L'\"';
        }
        if (p_time) {
            p_time += 12;
            const wchar_t *p_end = wcschr(p_time, quote_char);
            if (p_end) {
                int len = (int)(p_end - p_time);
                if (len > 31) len = 31;
                WideCharToMultiByte(CP_UTF8, 0, p_time, len, entry->time_created_utc, sizeof(entry->time_created_utc) - 1, NULL, NULL);
            }
        }
    }

    /* Populate description / diagnostic summary based on event type */
    if (entry->event_id == 41 && strstr(entry->provider_name, "Kernel-Power")) {
        snprintf(entry->message_snippet, sizeof(entry->message_snippet),
                 "Kernel-Power: System rebooted without cleanly shutting down first");
    } else if (entry->event_id == 1001) {
        snprintf(entry->message_snippet, sizeof(entry->message_snippet),
                 "BugCheck / System Error: Stop code crash dump logged");
    } else if (strstr(entry->provider_name, "WHEA-Logger")) {
        snprintf(entry->message_snippet, sizeof(entry->message_snippet),
                 "WHEA-Logger Hardware Error (Event ID %u)", (unsigned int)entry->event_id);
    } else {
        snprintf(entry->message_snippet, sizeof(entry->message_snippet),
                 "System Event ID %u from %s", (unsigned int)entry->event_id, entry->provider_name);
    }
}

void event_log_query(event_log_result_t *res) {
    if (!res) return;
    event_log_result_init(res);

    HMODULE hWevt = LoadLibraryW(L"wevtapi.dll");
    if (!hWevt) {
        res->is_available = false;
        return;
    }

    pfn_EvtQuery  pEvtQuery  = (pfn_EvtQuery)(void *)GetProcAddress(hWevt, "EvtQuery");
    pfn_EvtNext   pEvtNext   = (pfn_EvtNext)(void *)GetProcAddress(hWevt, "EvtNext");
    pfn_EvtRender pEvtRender = (pfn_EvtRender)(void *)GetProcAddress(hWevt, "EvtRender");
    pfn_EvtClose  pEvtClose  = (pfn_EvtClose)(void *)GetProcAddress(hWevt, "EvtClose");

    if (!pEvtQuery || !pEvtNext || !pEvtRender || !pEvtClose) {
        FreeLibrary(hWevt);
        res->is_available = false;
        return;
    }

    static const wchar_t *QUERY_XPATH =
        L"*[System[("
        L"(Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41) or "
        L"(Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] and EventID=1001) or "
        L"(Provider[@Name='Microsoft-Windows-WHEA-Logger'] and (EventID=17 or EventID=18 or EventID=19 or EventID=41)) or "
        L"EventID=1001"
        L")]]";

    EVT_HANDLE hQuery = pEvtQuery(
        NULL,
        L"System",
        QUERY_XPATH,
        EVT_QUERY_CHANNEL_PATH | EVT_QUERY_REVERSE_DIRECTION
    );

    if (!hQuery) {
        FreeLibrary(hWevt);
        res->is_available = false;
        return;
    }

    res->is_available = true;

    EVT_HANDLE hEvents[50];
    DWORD returned = 0;
    if (pEvtNext(hQuery, 50, hEvents, 1000, 0, &returned) && returned > 0) {
        res->entries = (event_log_entry_t *)calloc(returned, sizeof(event_log_entry_t));
        if (res->entries) {
            res->capacity = returned;
            DWORD buf_size = 8192;
            wchar_t *xml_buf = (wchar_t *)malloc(buf_size * sizeof(wchar_t));

            for (DWORD i = 0; i < returned; ++i) {
                DWORD buf_used = 0;
                DWORD prop_cnt = 0;
                if (xml_buf && pEvtRender(NULL, hEvents[i], EVT_RENDER_EVENT_XML,
                                          buf_size * sizeof(wchar_t), xml_buf,
                                          &buf_used, &prop_cnt)) {
                    parse_xml_event(xml_buf, &res->entries[res->count++]);
                } else if (xml_buf && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    wchar_t *larger_buf = (wchar_t *)realloc(xml_buf, buf_used);
                    if (larger_buf) {
                        xml_buf = larger_buf;
                        buf_size = buf_used / sizeof(wchar_t);
                        if (pEvtRender(NULL, hEvents[i], EVT_RENDER_EVENT_XML,
                                       buf_size * sizeof(wchar_t), xml_buf,
                                       &buf_used, &prop_cnt)) {
                            parse_xml_event(xml_buf, &res->entries[res->count++]);
                        }
                    }
                }
                pEvtClose(hEvents[i]);
            }

            if (xml_buf) free(xml_buf);
        } else {
            for (DWORD i = 0; i < returned; ++i) {
                pEvtClose(hEvents[i]);
            }
        }
    }

    pEvtClose(hQuery);
    FreeLibrary(hWevt);
}

void event_log_print_report(const event_log_result_t *res) {
    printf("================================================================================\n");
    printf("                            WINDOWS SYSTEM EVENT LOG\n");
    printf("================================================================================\n");
    if (!res || !res->is_available) {
        printf("Event Log: unavailable\n");
    } else if (res->count == 0) {
        printf("No matching crash / hardware error events found in System event log.\n");
    } else {
        printf("%-23s | %-32s | %-8s | %s\n", "Time (UTC)", "Provider", "EventID", "Description");
        printf("------------------------+----------------------------------+----------+-----------------------------------------\n");
        for (size_t i = 0; i < res->count; ++i) {
            const event_log_entry_t *e = &res->entries[i];
            printf("%-23s | %-32s | %-8u | %s\n",
                   e->time_created_utc[0] ? e->time_created_utc : "N/A",
                   e->provider_name[0] ? e->provider_name : "System",
                   (unsigned int)e->event_id,
                   e->message_snippet);
        }
    }
    printf("================================================================================\n\n");
}
