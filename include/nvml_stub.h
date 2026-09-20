#ifndef BLACKBOX_NVML_STUB_H
#define BLACKBOX_NVML_STUB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #define NVML_API __cdecl
#else
  #define NVML_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* NVML Return Codes & Enums                                                 */
/* ========================================================================= */

typedef enum nvmlReturn_enum {
    NVML_SUCCESS                   = 0,
    NVML_ERROR_UNINITIALIZED       = 1,
    NVML_ERROR_INVALID_ARGUMENT    = 2,
    NVML_ERROR_NOT_SUPPORTED       = 3,
    NVML_ERROR_NO_PERMISSION       = 4,
    NVML_ERROR_ALREADY_INITIALIZED = 5,
    NVML_ERROR_NOT_FOUND           = 6,
    NVML_ERROR_INSUFFICIENT_SIZE   = 7,
    NVML_ERROR_INSUFFICIENT_POWER  = 8,
    NVML_ERROR_DRIVER_NOT_LOADED   = 9,
    NVML_ERROR_TIMEOUT             = 10,
    NVML_ERROR_IRQ_ISSUE           = 11,
    NVML_ERROR_LIBRARY_NOT_FOUND   = 12,
    NVML_ERROR_FUNCTION_NOT_FOUND  = 13,
    NVML_ERROR_CORRUPTED_INFOROM   = 14,
    NVML_ERROR_GPU_IS_LOST         = 15,
    NVML_ERROR_RESET_REQUIRED      = 16,
    NVML_ERROR_OPERATING_SYSTEM    = 17,
    NVML_ERROR_UNKNOWN             = 999
} nvmlReturn_t;

typedef enum nvmlTemperatureSensors_enum {
    NVML_TEMPERATURE_GPU = 0
} nvmlTemperatureSensors_t;

typedef enum nvmlClockType_enum {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM       = 1,
    NVML_CLOCK_MEM      = 2,
    NVML_CLOCK_VIDEO    = 3
} nvmlClockType_t;

/* Opaque device handle */
struct nvmlDevice_st;
typedef struct nvmlDevice_st* nvmlDevice_t;

/* ========================================================================= */
/* Telemetry Sentinels (Used when GPU is non-NVIDIA, missing, or errored)    */
/* ========================================================================= */

#define NVML_SENTINEL_TEMP_U16      ((uint16_t)0xFFFF)
#define NVML_SENTINEL_POWER_U32     ((uint32_t)0xFFFFFFFF)
#define NVML_SENTINEL_CLOCK_U32     ((uint32_t)0xFFFFFFFF)
#define NVML_SENTINEL_PCIE_GEN_U8   ((uint8_t)0xFF)
#define NVML_SENTINEL_PCIE_WIDTH_U8 ((uint8_t)0xFF)
#define NVML_SENTINEL_REPLAY_U32    ((uint32_t)0xFFFFFFFF)

/* ========================================================================= */
/* Function Pointer Types                                                    */
/* ========================================================================= */

typedef nvmlReturn_t (NVML_API *PFN_nvmlInit_v2)(void);
typedef nvmlReturn_t (NVML_API *PFN_nvmlShutdown)(void);
typedef const char*  (NVML_API *PFN_nvmlErrorString)(nvmlReturn_t result);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetCount_v2)(unsigned int *deviceCount);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetHandleByIndex_v2)(unsigned int index, nvmlDevice_t *device);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetName)(nvmlDevice_t device, char *name, unsigned int length);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetTemperature)(nvmlDevice_t device, nvmlTemperatureSensors_t sensorType, unsigned int *temp);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetPowerUsage)(nvmlDevice_t device, unsigned int *power);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetClockInfo)(nvmlDevice_t device, nvmlClockType_t type, unsigned int *clock);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetCurrPcieLinkGen)(nvmlDevice_t device, unsigned int *currLinkGen);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetCurrPcieLinkWidth)(nvmlDevice_t device, unsigned int *currLinkWidth);
typedef nvmlReturn_t (NVML_API *PFN_nvmlDeviceGetPcieReplayCounter)(nvmlDevice_t device, unsigned int *value);

/* Lowercase aliases */
typedef PFN_nvmlInit_v2                     pfn_nvmlInit_v2;
typedef PFN_nvmlShutdown                   pfn_nvmlShutdown;
typedef PFN_nvmlErrorString                pfn_nvmlErrorString;
typedef PFN_nvmlDeviceGetCount_v2          pfn_nvmlDeviceGetCount_v2;
typedef PFN_nvmlDeviceGetHandleByIndex_v2  pfn_nvmlDeviceGetHandleByIndex_v2;
typedef PFN_nvmlDeviceGetName              pfn_nvmlDeviceGetName;
typedef PFN_nvmlDeviceGetTemperature       pfn_nvmlDeviceGetTemperature;
typedef PFN_nvmlDeviceGetPowerUsage        pfn_nvmlDeviceGetPowerUsage;
typedef PFN_nvmlDeviceGetClockInfo         pfn_nvmlDeviceGetClockInfo;
typedef PFN_nvmlDeviceGetCurrPcieLinkGen   pfn_nvmlDeviceGetCurrPcieLinkGen;
typedef PFN_nvmlDeviceGetCurrPcieLinkWidth pfn_nvmlDeviceGetCurrPcieLinkWidth;
typedef PFN_nvmlDeviceGetPcieReplayCounter pfn_nvmlDeviceGetPcieReplayCounter;

/* ========================================================================= */
/* NVML Dynamic Context & Dispatch Table                                     */
/* ========================================================================= */

typedef struct nvml_context_s {
#ifdef _WIN32
    HMODULE                       h_module;
#else
    void*                         h_module;
#endif
    nvmlDevice_t                  device;
    bool                          is_available;
    bool                          device_ready;

    /* Function Pointers */
    PFN_nvmlInit_v2               Init_v2;
    PFN_nvmlShutdown             Shutdown;
    PFN_nvmlErrorString          ErrorString;
    PFN_nvmlDeviceGetCount_v2    DeviceGetCount_v2;
    PFN_nvmlDeviceGetHandleByIndex_v2 DeviceGetHandleByIndex_v2;
    PFN_nvmlDeviceGetName        DeviceGetName;
    PFN_nvmlDeviceGetTemperature DeviceGetTemperature;
    PFN_nvmlDeviceGetPowerUsage  DeviceGetPowerUsage;
    PFN_nvmlDeviceGetClockInfo   DeviceGetClockInfo;
    PFN_nvmlDeviceGetCurrPcieLinkGen CurrPcieLinkGen;
    PFN_nvmlDeviceGetCurrPcieLinkWidth CurrPcieLinkWidth;
    PFN_nvmlDeviceGetPcieReplayCounter PcieReplayCounter;
} nvml_context_t;

/* Telemetry Sample Output */
typedef struct nvml_telemetry_s {
    uint16_t gpu_temp_c;
    uint32_t gpu_power_mw;
    uint32_t sm_clock_mhz;
    uint32_t mem_clock_mhz;
    uint8_t  pcie_gen;
    uint8_t  pcie_width;
    uint32_t pcie_replay;
} nvml_telemetry_t;

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_NVML_STUB_H */
