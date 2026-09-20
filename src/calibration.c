#include "calibration.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
typedef void (WINAPI *PFN_GetSystemTimePrecise)(LPFILETIME);

static void get_precise_filetime(LPFILETIME lpft) {
    static PFN_GetSystemTimePrecise pfn = NULL;
    static bool checked = false;
    if (!checked) {
        HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
        if (hKernel) {
            FARPROC fp = GetProcAddress(hKernel, "GetSystemTimePreciseAsFileTime");
            memcpy(&pfn, &fp, sizeof(pfn));
        }
        checked = true;
    }
    if (pfn) {
        pfn(lpft);
    } else {
        GetSystemTimeAsFileTime(lpft);
    }
}
#endif

uint64_t calibration_qpc_ticks_to_100ns(uint64_t ticks, uint64_t freq) {
    if (freq == 0) return 0;
    /* (ticks * 10,000,000) / freq */
    /* To prevent 64-bit overflow for large tick values, compute with integer + remainder or double */
    uint64_t seconds = ticks / freq;
    uint64_t remainder = ticks % freq;
    return (seconds * 10000000ULL) + ((remainder * 10000000ULL) / freq);
}

bool calibration_init(calibration_state_t *state, uint32_t measure_ms) {
    if (!state) return false;
    memset(state, 0, sizeof(*state));

#ifdef _WIN32
    LARGE_INTEGER qpc_freq;
    if (!QueryPerformanceFrequency(&qpc_freq) || qpc_freq.QuadPart <= 0) {
        return false;
    }
    state->qpc_freq = (uint64_t)qpc_freq.QuadPart;

    LARGE_INTEGER qpc0;
    QueryPerformanceCounter(&qpc0);
    uint64_t tsc0 = blackbox_rdtsc();

    FILETIME ft;
    get_precise_filetime(&ft);
    ULARGE_INTEGER uft;
    uft.LowPart = ft.dwLowDateTime;
    uft.HighPart = ft.dwHighDateTime;
    state->filetime_start = uft.QuadPart;

    state->qpc_start_ticks = (uint64_t)qpc0.QuadPart;
    state->qpc_start_100ns = calibration_qpc_ticks_to_100ns(state->qpc_start_ticks, state->qpc_freq);
    state->tsc_start = tsc0;

    /* Measure TSC frequency by observing cycles over short sleep duration */
    if (measure_ms == 0) measure_ms = 20;
    Sleep(measure_ms);

    LARGE_INTEGER qpc1;
    QueryPerformanceCounter(&qpc1);
    uint64_t tsc1 = blackbox_rdtsc();

    uint64_t qpc_delta = (uint64_t)(qpc1.QuadPart - qpc0.QuadPart);
    uint64_t tsc_delta = (tsc1 > tsc0) ? (tsc1 - tsc0) : 0;

    if (qpc_delta > 0 && tsc_delta > 0) {
        double elapsed_sec = (double)qpc_delta / (double)state->qpc_freq;
        state->tsc_freq_hz = (uint64_t)((double)tsc_delta / elapsed_sec);
    } else {
        state->tsc_freq_hz = 3000000000ULL; /* Default 3.0 GHz fallback */
    }

    return true;
#else
    /* Non-Windows fallback for host compilation / unit testing */
    state->qpc_freq = 10000000ULL; /* 10 MHz nominal */
    state->qpc_start_ticks = 1000000ULL;
    state->qpc_start_100ns = 1000000ULL;
    state->tsc_start = blackbox_rdtsc();
    state->filetime_start = 133700000000000000ULL;
    state->tsc_freq_hz = 3000000000ULL;
    return true;
#endif
}

void calibration_populate_header(const calibration_state_t *state,
                                 blackbox_header_t *hdr,
                                 uint64_t ring_bytes,
                                 uint32_t nominal_hz,
                                 uint32_t daemon_pid) {
    if (!state || !hdr) return;
    memset(hdr, 0, sizeof(*hdr));

    hdr->magic = BLACKBOX_MAGIC;
    hdr->version = BLACKBOX_VERSION;
    hdr->header_size = BLACKBOX_HEADER_SIZE;
    hdr->ring_bytes = ring_bytes;
    hdr->record_size = BLACKBOX_RECORD_SIZE;
    hdr->record_count = (uint32_t)((ring_bytes - BLACKBOX_HEADER_SIZE) / BLACKBOX_RECORD_SIZE);

    hdr->qpc_freq = state->qpc_freq;
    hdr->qpc_start_ticks = state->qpc_start_ticks;
    hdr->qpc_start_100ns = state->qpc_start_100ns;
    hdr->tsc_start = state->tsc_start;

    hdr->filetime_start = state->filetime_start;
    hdr->nominal_hz = (nominal_hz > 0) ? nominal_hz : 10;
    hdr->daemon_pid = daemon_pid;
    hdr->reserved_flags = 0;
}

void calibration_capture_triple(const calibration_state_t *state,
                                uint64_t *out_tsc,
                                uint64_t *out_qpc_100ns,
                                uint64_t *out_filetime) {
#ifdef _WIN32
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    uint64_t tsc = blackbox_rdtsc();

    FILETIME ft;
    get_precise_filetime(&ft);
    ULARGE_INTEGER uft;
    uft.LowPart = ft.dwLowDateTime;
    uft.HighPart = ft.dwHighDateTime;

    if (out_tsc) *out_tsc = tsc;
    if (out_qpc_100ns) {
        uint64_t freq = (state && state->qpc_freq > 0) ? state->qpc_freq : 10000000ULL;
        *out_qpc_100ns = calibration_qpc_ticks_to_100ns((uint64_t)qpc.QuadPart, freq);
    }
    if (out_filetime) *out_filetime = uft.QuadPart;
#else
    if (out_tsc) *out_tsc = blackbox_rdtsc();
    if (out_qpc_100ns) *out_qpc_100ns = 0;
    if (out_filetime) *out_filetime = 0;
#endif
}

uint64_t calibration_qpc_to_filetime(const blackbox_header_t *hdr, uint64_t qpc_100ns) {
    if (!hdr) return 0;
    if (qpc_100ns >= hdr->qpc_start_100ns) {
        return hdr->filetime_start + (qpc_100ns - hdr->qpc_start_100ns);
    } else {
        uint64_t diff = hdr->qpc_start_100ns - qpc_100ns;
        return (hdr->filetime_start > diff) ? (hdr->filetime_start - diff) : 0;
    }
}

uint64_t calibration_tsc_to_filetime(const blackbox_header_t *hdr, uint64_t tsc, uint64_t tsc_freq_hz) {
    if (!hdr) return 0;
    if (tsc_freq_hz == 0) tsc_freq_hz = 3000000000ULL;
    if (tsc >= hdr->tsc_start) {
        uint64_t delta_cycles = tsc - hdr->tsc_start;
        uint64_t delta_100ns = (uint64_t)(((double)delta_cycles * 10000000.0) / (double)tsc_freq_hz);
        return hdr->filetime_start + delta_100ns;
    } else {
        uint64_t delta_cycles = hdr->tsc_start - tsc;
        uint64_t delta_100ns = (uint64_t)(((double)delta_cycles * 10000000.0) / (double)tsc_freq_hz);
        return (hdr->filetime_start > delta_100ns) ? (hdr->filetime_start - delta_100ns) : 0;
    }
}

bool calibration_format_filetime_utc(uint64_t filetime, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return false;

#ifdef _WIN32
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(filetime & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(filetime >> 32);

    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st)) {
        snprintf(buf, buf_size, "0x%016llX", (unsigned long long)filetime);
        return false;
    }

    snprintf(buf, buf_size, "%04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return true;
#else
    const uint64_t epoch_diff = 116444736000000000ULL;
    if (filetime < epoch_diff) {
        snprintf(buf, buf_size, "0x%016llX", (unsigned long long)filetime);
        return false;
    }
    time_t sec = (time_t)((filetime - epoch_diff) / 10000000ULL);
    uint32_t ms = (uint32_t)(((filetime - epoch_diff) % 10000000ULL) / 10000ULL);
    struct tm tm_utc;
    gmtime_r(&sec, &tm_utc);
    snprintf(buf, buf_size, "%04d-%02d-%02d %02d:%02d:%02d.%03u UTC",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ms);
    return true;
#endif
}

bool calibration_format_filetime_local(uint64_t filetime, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return false;

#ifdef _WIN32
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(filetime & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(filetime >> 32);

    SYSTEMTIME st_utc, st_local;
    if (!FileTimeToSystemTime(&ft, &st_utc) ||
        !SystemTimeToTzSpecificLocalTime(NULL, &st_utc, &st_local)) {
        return calibration_format_filetime_utc(filetime, buf, buf_size);
    }

    snprintf(buf, buf_size, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
             st_local.wYear, st_local.wMonth, st_local.wDay,
             st_local.wHour, st_local.wMinute, st_local.wSecond, st_local.wMilliseconds);
    return true;
#else
    return calibration_format_filetime_utc(filetime, buf, buf_size);
#endif
}

uint64_t calibration_filetime_to_unix_ms(uint64_t filetime) {
    const uint64_t epoch_diff = 116444736000000000ULL;
    if (filetime <= epoch_diff) return 0;
    return (filetime - epoch_diff) / 10000ULL;
}
