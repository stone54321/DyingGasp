#ifndef BLACKBOX_COMMON_H
#define BLACKBOX_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

/* Intrinsics header for __rdtsc */
#if defined(_MSC_VER)
  #include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
  #include <x86intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Compiler Intrinsics Wrapper for RDTSC */
static inline uint64_t blackbox_rdtsc(void) {
#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
    return (uint64_t)__rdtsc();
#else
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#endif
}

/* Common Utility Macros */
#ifndef BLACKBOX_ARRAY_SIZE
#define BLACKBOX_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef BLACKBOX_MIN
#define BLACKBOX_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef BLACKBOX_MAX
#define BLACKBOX_MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef BLACKBOX_UNUSED
#define BLACKBOX_UNUSED(var) ((void)(var))
#endif

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_COMMON_H */
