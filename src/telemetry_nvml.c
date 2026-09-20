#include "telemetry_nvml.h"
#include <string.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

bool telemetry_nvml_init(nvml_context_t *ctx) {
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));

    /* Dynamically load nvml.dll */
    ctx->h_module = LoadLibraryW(L"nvml.dll");
    if (!ctx->h_module) {
        ctx->h_module = LoadLibraryA("nvml.dll");
    }

    if (!ctx->h_module) {
        ctx->is_available = false;
        ctx->device_ready = false;
        return false;
    }

    /* Resolve function pointers via GetProcAddress with fallbacks */
    ctx->Init_v2 = (PFN_nvmlInit_v2)(void*)GetProcAddress(ctx->h_module, "nvmlInit_v2");
    if (!ctx->Init_v2) {
        ctx->Init_v2 = (PFN_nvmlInit_v2)(void*)GetProcAddress(ctx->h_module, "nvmlInit");
    }

    ctx->Shutdown = (PFN_nvmlShutdown)(void*)GetProcAddress(ctx->h_module, "nvmlShutdown");
    ctx->ErrorString = (PFN_nvmlErrorString)(void*)GetProcAddress(ctx->h_module, "nvmlErrorString");

    ctx->DeviceGetCount_v2 = (PFN_nvmlDeviceGetCount_v2)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetCount_v2");
    if (!ctx->DeviceGetCount_v2) {
        ctx->DeviceGetCount_v2 = (PFN_nvmlDeviceGetCount_v2)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetCount");
    }

    ctx->DeviceGetHandleByIndex_v2 = (PFN_nvmlDeviceGetHandleByIndex_v2)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetHandleByIndex_v2");
    if (!ctx->DeviceGetHandleByIndex_v2) {
        ctx->DeviceGetHandleByIndex_v2 = (PFN_nvmlDeviceGetHandleByIndex_v2)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetHandleByIndex");
    }

    ctx->DeviceGetName = (PFN_nvmlDeviceGetName)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetName");
    ctx->DeviceGetTemperature = (PFN_nvmlDeviceGetTemperature)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetTemperature");
    ctx->DeviceGetPowerUsage = (PFN_nvmlDeviceGetPowerUsage)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetPowerUsage");
    ctx->DeviceGetClockInfo = (PFN_nvmlDeviceGetClockInfo)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetClockInfo");

    ctx->CurrPcieLinkGen = (PFN_nvmlDeviceGetCurrPcieLinkGen)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetCurrPcieLinkGeneration");
    if (!ctx->CurrPcieLinkGen) {
        ctx->CurrPcieLinkGen = (PFN_nvmlDeviceGetCurrPcieLinkGen)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetCurrPcieLinkGen");
    }

    ctx->CurrPcieLinkWidth = (PFN_nvmlDeviceGetCurrPcieLinkWidth)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetCurrPcieLinkWidth");
    ctx->PcieReplayCounter = (PFN_nvmlDeviceGetPcieReplayCounter)(void*)GetProcAddress(ctx->h_module, "nvmlDeviceGetPcieReplayCounter");

    /* Validate essential entry points */
    if (!ctx->Init_v2 || !ctx->DeviceGetHandleByIndex_v2) {
        FreeLibrary(ctx->h_module);
        ctx->h_module = NULL;
        ctx->is_available = false;
        ctx->device_ready = false;
        return false;
    }

    /* Initialize NVML driver subsystem */
    nvmlReturn_t status = ctx->Init_v2();
    if (status != NVML_SUCCESS) {
        FreeLibrary(ctx->h_module);
        ctx->h_module = NULL;
        ctx->is_available = false;
        ctx->device_ready = false;
        return false;
    }

    ctx->is_available = true;

    /* Query device count and obtain primary GPU handle */
    unsigned int device_count = 0;
    if (ctx->DeviceGetCount_v2 && ctx->DeviceGetCount_v2(&device_count) == NVML_SUCCESS && device_count > 0) {
        if (ctx->DeviceGetHandleByIndex_v2(0, &ctx->device) == NVML_SUCCESS && ctx->device != NULL) {
            ctx->device_ready = true;
            return true;
        }
    }

    /* NVML initialized successfully, but no active device was detected */
    ctx->device_ready = false;
    return true;
}

void telemetry_nvml_sample(nvml_context_t *ctx,
                           nvml_telemetry_t *out_sample,
                           uint16_t *out_flags) {
    if (!out_sample) return;

    /* Graceful fallback if NVML is absent, non-NVIDIA, or device handle unavailable */
    if (!ctx || !ctx->is_available || !ctx->device_ready || !ctx->device) {
        out_sample->gpu_temp_c   = BLACKBOX_SENTINEL_U16;
        out_sample->gpu_power_mw = BLACKBOX_SENTINEL_U32;
        out_sample->sm_clock_mhz = BLACKBOX_SENTINEL_U32;
        out_sample->mem_clock_mhz= BLACKBOX_SENTINEL_U32;
        out_sample->pcie_gen     = BLACKBOX_SENTINEL_U8;
        out_sample->pcie_width   = BLACKBOX_SENTINEL_U8;
        out_sample->pcie_replay  = BLACKBOX_SENTINEL_U32;

        if (out_flags) {
            *out_flags &= (uint16_t)~BLACKBOX_FLAG_NVML_AVAILABLE;
        }
        return;
    }

    if (out_flags) {
        *out_flags |= BLACKBOX_FLAG_NVML_AVAILABLE;
    }

    /* 1. GPU Core Temperature */
    if (ctx->DeviceGetTemperature) {
        unsigned int temp = 0;
        nvmlReturn_t rc = ctx->DeviceGetTemperature(ctx->device, NVML_TEMPERATURE_GPU, &temp);
        out_sample->gpu_temp_c = (rc == NVML_SUCCESS) ? (uint16_t)temp : BLACKBOX_SENTINEL_U16;
    } else {
        out_sample->gpu_temp_c = BLACKBOX_SENTINEL_U16;
    }

    /* 2. GPU Power Usage in mW */
    if (ctx->DeviceGetPowerUsage) {
        unsigned int power = 0;
        nvmlReturn_t rc = ctx->DeviceGetPowerUsage(ctx->device, &power);
        out_sample->gpu_power_mw = (rc == NVML_SUCCESS) ? (uint32_t)power : BLACKBOX_SENTINEL_U32;
    } else {
        out_sample->gpu_power_mw = BLACKBOX_SENTINEL_U32;
    }

    /* 3. GPU Clocks (SM and Memory) */
    if (ctx->DeviceGetClockInfo) {
        unsigned int sm_clock = 0;
        nvmlReturn_t rc_sm = ctx->DeviceGetClockInfo(ctx->device, NVML_CLOCK_SM, &sm_clock);
        out_sample->sm_clock_mhz = (rc_sm == NVML_SUCCESS) ? (uint32_t)sm_clock : BLACKBOX_SENTINEL_U32;

        unsigned int mem_clock = 0;
        nvmlReturn_t rc_mem = ctx->DeviceGetClockInfo(ctx->device, NVML_CLOCK_MEM, &mem_clock);
        out_sample->mem_clock_mhz = (rc_mem == NVML_SUCCESS) ? (uint32_t)mem_clock : BLACKBOX_SENTINEL_U32;
    } else {
        out_sample->sm_clock_mhz = BLACKBOX_SENTINEL_U32;
        out_sample->mem_clock_mhz = BLACKBOX_SENTINEL_U32;
    }

    /* 4. Current PCIe Link Generation */
    if (ctx->CurrPcieLinkGen) {
        unsigned int gen = 0;
        nvmlReturn_t rc = ctx->CurrPcieLinkGen(ctx->device, &gen);
        out_sample->pcie_gen = (rc == NVML_SUCCESS && gen > 0 && gen <= 255) ? (uint8_t)gen : BLACKBOX_SENTINEL_U8;
    } else {
        out_sample->pcie_gen = BLACKBOX_SENTINEL_U8;
    }

    /* 5. Current PCIe Link Width */
    if (ctx->CurrPcieLinkWidth) {
        unsigned int width = 0;
        nvmlReturn_t rc = ctx->CurrPcieLinkWidth(ctx->device, &width);
        out_sample->pcie_width = (rc == NVML_SUCCESS && width > 0 && width <= 255) ? (uint8_t)width : BLACKBOX_SENTINEL_U8;
    } else {
        out_sample->pcie_width = BLACKBOX_SENTINEL_U8;
    }

    /* 6. PCIe Replay Counter */
    if (ctx->PcieReplayCounter) {
        unsigned int replay = 0;
        nvmlReturn_t rc = ctx->PcieReplayCounter(ctx->device, &replay);
        out_sample->pcie_replay = (rc == NVML_SUCCESS) ? (uint32_t)replay : BLACKBOX_SENTINEL_U32;
    } else {
        out_sample->pcie_replay = BLACKBOX_SENTINEL_U32;
    }
}

void telemetry_nvml_sample_record(nvml_context_t *ctx, blackbox_record_data_t *rec) {
    if (!rec) return;

    nvml_telemetry_t sample;
    uint16_t flags = rec->flags;
    telemetry_nvml_sample(ctx, &sample, &flags);

    rec->gpu_temp_c   = sample.gpu_temp_c;
    rec->gpu_power_mw = sample.gpu_power_mw;
    rec->sm_clock_mhz = sample.sm_clock_mhz;
    rec->mem_clock_mhz= sample.mem_clock_mhz;
    rec->pcie_gen     = sample.pcie_gen;
    rec->pcie_width   = sample.pcie_width;
    rec->pcie_replay  = sample.pcie_replay;
    rec->flags        = flags;
}

bool telemetry_nvml_get_live_replay(nvml_context_t *ctx, uint32_t *out_replay) {
    if (!ctx || !ctx->is_available || !ctx->device_ready || !ctx->device || !ctx->PcieReplayCounter) {
        return false;
    }

    unsigned int val = 0;
    nvmlReturn_t rc = ctx->PcieReplayCounter(ctx->device, &val);
    if (rc == NVML_SUCCESS) {
        if (out_replay) {
            *out_replay = (uint32_t)val;
        }
        return true;
    }
    return false;
}

void telemetry_nvml_shutdown(nvml_context_t *ctx) {
    if (!ctx) return;

    if (ctx->Shutdown && ctx->is_available) {
        ctx->Shutdown();
    }

    if (ctx->h_module) {
        FreeLibrary(ctx->h_module);
        ctx->h_module = NULL;
    }

    memset(ctx, 0, sizeof(*ctx));
}
