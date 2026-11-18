#ifndef BLACKBOX_CLI_H
#define BLACKBOX_CLI_H

#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>

/* Decimal digits only: reject signs, trailing text and overflow. */
static inline bool cli_uint(const wchar_t *text, uint64_t min_value,
                            uint64_t max_value, uint64_t *out) {
    uint64_t value = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9') return false;
        unsigned digit = (unsigned)(*text - L'0');
        if (digit > max_value || value > (max_value - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value < min_value) return false;
    *out = value;
    return true;
}

static inline bool cli_path(wchar_t *dest, size_t capacity, const wchar_t *src) {
    size_t length = wcslen(src);
    if (!length || length >= capacity) return false;
    wmemcpy(dest, src, length + 1);
    return true;
}
#endif
