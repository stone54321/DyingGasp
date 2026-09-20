#ifndef BLACKBOX_FORMAT_H
#define BLACKBOX_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* File Format Identifiers & Constants                                       */
/* ========================================================================= */

/**
 * Magic identifier: ASCII "BLKBOX01" in Little-Endian byte order.
 * Bytes: 'B'=0x42, 'L'=0x4C, 'K'=0x4B, 'B'=0x42, 'O'=0x4F, 'X'=0x58, '0'=0x30, '1'=0x31
 */
#define BLACKBOX_MAGIC              0x3130584F424B4C42ULL

#define BLACKBOX_VERSION            1U
#define BLACKBOX_HEADER_SIZE        4096U
#define BLACKBOX_RECORD_SIZE        64U
#define BLACKBOX_DEFAULT_RING_BYTES (64ULL * 1024ULL * 1024ULL) /* 64 MB */

/* Canonical Win32 System Object Identifiers */
#define BLACKBOX_MUTEX_NAME_W       L"Global\\BlackboxLogger.SingleInstance"
#define BLACKBOX_MUTEX_NAME_A       "Global\\BlackboxLogger.SingleInstance"
#define BLACKBOX_TASK_NAME_W        L"BlackboxLogger"
#define BLACKBOX_TASK_NAME_A        "BlackboxLogger"

#ifdef UNICODE
#define BLACKBOX_MUTEX_NAME         BLACKBOX_MUTEX_NAME_W
#define BLACKBOX_TASK_NAME          BLACKBOX_TASK_NAME_W
#else
#define BLACKBOX_MUTEX_NAME         BLACKBOX_MUTEX_NAME_A
#define BLACKBOX_TASK_NAME          BLACKBOX_TASK_NAME_A
#endif

/* Sentinel Values for Missing / Unsupported Hardware Sensors */
#define BLACKBOX_SENTINEL_U8        0xFFU
#define BLACKBOX_SENTINEL_U16       0xFFFFU
#define BLACKBOX_SENTINEL_U32       0xFFFFFFFFU
#define BLACKBOX_SENTINEL_U64       0xFFFFFFFFFFFFFFFFULL

/* Record Types */
typedef enum {
    BLACKBOX_RECORD_TYPE_DATA        = 0, /**< Periodic hardware telemetry sample */
    BLACKBOX_RECORD_TYPE_PROCESS     = 1, /**< Foreground window process transition */
    BLACKBOX_RECORD_TYPE_CALIBRATION = 2, /**< Mid-stream clock calibration synchronization */
    BLACKBOX_RECORD_TYPE_HEARTBEAT   = 3  /**< Daemon health and liveness heartbeat */
} blackbox_record_type_t;

/* Telemetry Flags Bitmask */
typedef enum {
    BLACKBOX_FLAG_NVML_AVAILABLE     = (1 << 0), /**< bit 0: NVML loaded and GPU detected */
    BLACKBOX_FLAG_FG_QUERY_OK        = (1 << 1), /**< bit 1: Foreground process query succeeded */
    BLACKBOX_FLAG_TIMER_LATE         = (1 << 2)  /**< bit 2: Timer tick arrived > 2x nominal period late */
    /* bits 3..15: reserved zero */
} blackbox_flags_t;

/* Service Record Cadence */
#define BLACKBOX_CALIBRATION_INTERVAL_SEC  3600U /**< Type 2 emitted at startup + every 3600s */
#define BLACKBOX_HEARTBEAT_INTERVAL_SEC    10U   /**< Type 3 emitted every 10s */

/* Ring Buffer Offset Formula: 4096 + (seq % record_count) * 64 */
#define BLACKBOX_CALC_OFFSET(seq, record_count) \
    ((uint64_t)BLACKBOX_HEADER_SIZE + (((uint64_t)(seq) % (uint64_t)(record_count)) * (uint64_t)BLACKBOX_RECORD_SIZE))

/* ========================================================================= */
/* Binary Layout Structures (Packed, Little-Endian)                          */
/* ========================================================================= */

#pragma pack(push, 1)

/**
 * @brief 4 KB Little-Endian Ring Buffer File Header
 * Resides at offset 0 of ring.bin.
 * Total size: exactly 4096 bytes.
 */
typedef struct {
    /* 0x0000 - 0x001F: Identification and Geometry (32 bytes) */
    uint64_t magic;              /**< Magic identifier: BLACKBOX_MAGIC ("BLKBOX01") */
    uint32_t version;            /**< Format version: BLACKBOX_VERSION (1) */
    uint32_t header_size;        /**< Size of this header in bytes (4096) */
    uint64_t ring_bytes;         /**< Total pre-allocated file size in bytes (e.g. 67,108,864 for 64 MB) */
    uint32_t record_size;        /**< Size of each record in bytes (64) */
    uint32_t record_count;       /**< Total record slots: (ring_bytes - header_size) / record_size */

    /* 0x0020 - 0x003F: Operational Calibration Triple (32 bytes) */
    uint64_t qpc_freq;           /**< QueryPerformanceFrequency (ticks per second) */
    uint64_t qpc_start_ticks;    /**< Raw QPC tick counter at daemon initialization */
    uint64_t qpc_start_100ns;    /**< Normalized QPC in 100ns units: (ticks * 10,000,000) / qpc_freq */
    uint64_t tsc_start;          /**< __rdtsc() cycle counter at daemon initialization */

    /* 0x0040 - 0x0057: Wall-Clock Anchor & Configuration (24 bytes) */
    uint64_t filetime_start;     /**< Windows FILETIME (UTC 100ns intervals since Jan 1, 1601) */
    uint32_t nominal_hz;         /**< Configured sampling frequency in Hz (1..100, default 10) */
    uint32_t daemon_pid;         /**< Process ID of the collector daemon instance */
    uint64_t reserved_flags;     /**< Reserved feature flags (initially 0) */

    /* 0x0058 - 0x0FFF: Reserved / Zero-padded to exactly 4096 bytes (4008 bytes) */
    uint8_t  reserved_padding[4008]; /**< Zero-padded reservation for future metadata */
} blackbox_header_t;

_Static_assert(sizeof(blackbox_header_t) == 4096, "blackbox_header_t must be exactly 4096 bytes");

/**
 * @brief Type 0: Periodic Hardware Telemetry Record (64 bytes)
 */
typedef struct {
    uint8_t  type;           /**< Record type: BLACKBOX_RECORD_TYPE_DATA (0) */
    uint8_t  pcie_gen;       /**< Current PCIe link gen (1..5) or 0xFF */
    uint8_t  pcie_width;     /**< Current PCIe link width (1, 2, 4, 8, 16) or 0xFF */
    uint8_t  cpu_pct;        /**< System CPU utilization percentage (0..100) or 0xFF */
    uint16_t gpu_temp_c;     /**< GPU core temperature in degrees Celsius or 0xFFFF */
    uint16_t flags;          /**< Bitmask of blackbox_flags_t (bits 0..2 active, 3..15 zero) */
    uint32_t gpu_power_mw;   /**< GPU power draw in milliwatts (mW) or 0xFFFFFFFF */
    uint32_t pcie_replay;    /**< PCIe replay counter or 0xFFFFFFFF */
    uint32_t sm_clock_mhz;   /**< GPU SM / Core clock in MHz or 0xFFFFFFFF */
    uint32_t mem_clock_mhz;  /**< GPU Memory clock in MHz or 0xFFFFFFFF */
    uint32_t fg_pid;         /**< Current foreground process ID (0 if none) */
    uint64_t seq;            /**< Monotonic 64-bit sequence counter (0, 1, 2, ...) */
    uint64_t tsc;            /**< CPU cycle timestamp via __rdtsc() */
    uint64_t qpc_100ns;      /**< Normalized QPC timestamp in 100ns units */
    uint8_t  reserved[12];   /**< Reserved padding, must be zeroed */
} blackbox_record_data_t;

_Static_assert(sizeof(blackbox_record_data_t) == 64, "blackbox_record_data_t must be exactly 64 bytes");

/**
 * @brief Type 1: Foreground Process Transition Record (64 bytes)
 * Emitted ONLY upon process transition. Name is stored exclusively here.
 */
typedef struct {
    uint8_t  type;           /**< Record type: BLACKBOX_RECORD_TYPE_PROCESS (1) */
    uint8_t  flags;          /**< Bitmask: bit0 nvml, bit1 fg_query_ok, bit2 timer_late */
    uint8_t  name_len;       /**< Length of executable basename (<= 31) */
    uint8_t  pad0;           /**< Reserved zero byte for alignment */
    uint64_t seq;            /**< Monotonic sequence counter */
    uint64_t tsc;            /**< CPU cycle timestamp via __rdtsc() */
    uint64_t qpc_100ns;      /**< Normalized QPC in 100ns units */
    uint32_t fg_pid;         /**< New foreground process ID */
    char     name[32];       /**< Basename of executable, truncated to 31 chars + NUL */
} blackbox_record_process_t;

_Static_assert(sizeof(blackbox_record_process_t) == 64, "blackbox_record_process_t must be exactly 64 bytes");

/**
 * @brief Type 2: Clock Calibration Resync Record (64 bytes)
 * Emitted at daemon start and every 3600 seconds.
 */
typedef struct {
    uint8_t  type;           /**< Record type: BLACKBOX_RECORD_TYPE_CALIBRATION (2) */
    uint8_t  reserved[7];    /**< Reserved padding for 8-byte alignment */
    uint64_t seq;            /**< Monotonic sequence counter */
    uint64_t tsc;            /**< Cycle timestamp via __rdtsc() */
    uint64_t qpc_100ns;      /**< Normalized QPC timestamp in 100ns units */
    uint64_t filetime;       /**< Windows FILETIME in 100ns units */
    uint8_t  reserved2[24];  /**< Reserved padding, zeroed */
} blackbox_record_cal_t;

_Static_assert(sizeof(blackbox_record_cal_t) == 64, "blackbox_record_cal_t must be exactly 64 bytes");

/**
 * @brief Type 3: Daemon Heartbeat Record (64 bytes)
 * Emitted every 10 seconds.
 */
typedef struct {
    uint8_t  type;           /**< Record type: BLACKBOX_RECORD_TYPE_HEARTBEAT (3) */
    uint8_t  cpu_pct;        /**< System CPU utilization percentage */
    uint16_t flags;          /**< Bitmask of blackbox_flags_t */
    uint32_t fg_pid;         /**< Active foreground PID */
    uint64_t seq;            /**< Monotonic sequence counter */
    uint64_t tsc;            /**< Cycle timestamp via __rdtsc() */
    uint64_t qpc_100ns;      /**< Normalized QPC in 100ns units */
    uint32_t uptime_sec;     /**< Daemon uptime in seconds */
    uint8_t  reserved[28];   /**< Reserved padding, zeroed */
} blackbox_record_heartbeat_t;

_Static_assert(sizeof(blackbox_record_heartbeat_t) == 64, "blackbox_record_heartbeat_t must be exactly 64 bytes");

/**
 * @brief Unified 64-Byte Record Union
 */
typedef union {
    uint8_t                     raw[64];
    struct {
        uint8_t type;
    } common;
    blackbox_record_data_t      data;
    blackbox_record_process_t   process;
    blackbox_record_cal_t       cal;
    blackbox_record_heartbeat_t heartbeat;
} blackbox_record_t;

_Static_assert(sizeof(blackbox_record_t) == 64, "blackbox_record_t must be exactly 64 bytes");

#pragma pack(pop)

/* Helper Accessors for Polymorphic Record Fields */
static inline uint64_t blackbox_record_get_seq(const blackbox_record_t *rec) {
    if (!rec) return 0;
    switch (rec->common.type) {
        case BLACKBOX_RECORD_TYPE_DATA:        return rec->data.seq;
        case BLACKBOX_RECORD_TYPE_PROCESS:     return rec->process.seq;
        case BLACKBOX_RECORD_TYPE_CALIBRATION: return rec->cal.seq;
        case BLACKBOX_RECORD_TYPE_HEARTBEAT:   return rec->heartbeat.seq;
        default:                               return rec->data.seq;
    }
}

static inline uint64_t blackbox_record_get_tsc(const blackbox_record_t *rec) {
    if (!rec) return 0;
    switch (rec->common.type) {
        case BLACKBOX_RECORD_TYPE_DATA:        return rec->data.tsc;
        case BLACKBOX_RECORD_TYPE_PROCESS:     return rec->process.tsc;
        case BLACKBOX_RECORD_TYPE_CALIBRATION: return rec->cal.tsc;
        case BLACKBOX_RECORD_TYPE_HEARTBEAT:   return rec->heartbeat.tsc;
        default:                               return rec->data.tsc;
    }
}

static inline uint64_t blackbox_record_get_qpc_100ns(const blackbox_record_t *rec) {
    if (!rec) return 0;
    switch (rec->common.type) {
        case BLACKBOX_RECORD_TYPE_DATA:        return rec->data.qpc_100ns;
        case BLACKBOX_RECORD_TYPE_PROCESS:     return rec->process.qpc_100ns;
        case BLACKBOX_RECORD_TYPE_CALIBRATION: return rec->cal.qpc_100ns;
        case BLACKBOX_RECORD_TYPE_HEARTBEAT:   return rec->heartbeat.qpc_100ns;
        default:                               return rec->data.qpc_100ns;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_FORMAT_H */
