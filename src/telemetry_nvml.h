#ifndef BLACKBOX_TELEMETRY_NVML_H
#define BLACKBOX_TELEMETRY_NVML_H

#include "nvml_stub.h"
#include "blackbox_format.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility alias for flags */
#ifndef BLACKBOX_FLAG_NVML_AVAIL
#define BLACKBOX_FLAG_NVML_AVAIL BLACKBOX_FLAG_NVML_AVAILABLE
#endif

/**
 * @brief Initialize NVML client by dynamically loading nvml.dll.
 *
 * Resolves all function pointers. Gracefully sets is_available=false and
 * device_ready=false if nvml.dll is missing or GPU is non-NVIDIA.
 * Never crashes or throws exceptions.
 *
 * @param ctx NVML context struct to initialize.
 * @return true if NVML is loaded and at least one device handle is ready, false otherwise.
 */
bool telemetry_nvml_init(nvml_context_t *ctx);

/**
 * @brief Sample NVML telemetry metrics into an nvml_telemetry_t struct.
 *
 * If NVML is not available or query calls fail, populates sentinel values
 * (BLACKBOX_SENTINEL_U16, BLACKBOX_SENTINEL_U32, BLACKBOX_SENTINEL_U8),
 * clears BLACKBOX_FLAG_NVML_AVAILABLE in out_flags, and never crashes.
 *
 * @param ctx Initialized NVML context.
 * @param out_sample Receives sampled metrics or sentinels.
 * @param out_flags Receives updated flag bitmask (sets or clears BLACKBOX_FLAG_NVML_AVAILABLE).
 */
void telemetry_nvml_sample(nvml_context_t *ctx,
                           nvml_telemetry_t *out_sample,
                           uint16_t *out_flags);

/**
 * @brief Populate a blackbox_record_data_t struct directly with NVML readings or sentinels.
 *
 * @param ctx Initialized NVML context.
 * @param rec Record to update with GPU telemetry and flag bit.
 */
void telemetry_nvml_sample_record(nvml_context_t *ctx, blackbox_record_data_t *rec);

/**
 * @brief Query live PCIe replay counter from hardware via NVML.
 *
 * Used by post-mortem analyzer to compute hardware replay counter delta.
 *
 * @param ctx Initialized NVML context.
 * @param out_replay Receives live replay counter value.
 * @return true if query succeeded, false if NVML unavailable or unsupported.
 */
bool telemetry_nvml_get_live_replay(nvml_context_t *ctx, uint32_t *out_replay);

/**
 * @brief Shut down NVML and release loaded DLL handle.
 *
 * @param ctx Context to clean up.
 */
void telemetry_nvml_shutdown(nvml_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_TELEMETRY_NVML_H */
