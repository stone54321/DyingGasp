#include "timer_loop.h"
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

bool timer_loop_init(timer_loop_t *loop,
                     const timer_loop_config_t *config,
                     ring_buffer_writer_t *writer,
                     calibration_state_t *calibration,
                     nvml_context_t *nvml) {
    if (!loop || !config || !writer || !calibration || !nvml) {
        return false;
    }

    memset(loop, 0, sizeof(*loop));
    loop->config = *config;
    loop->writer = writer;
    loop->calibration = calibration;
    loop->nvml = nvml;

    /* Clamp frequency to valid range [1..100] */
    if (loop->config.hz < 1) {
        loop->config.hz = 1;
    } else if (loop->config.hz > 100) {
        loop->config.hz = 100;
    }

    loop->nominal_interval_100ns = 10000000ULL / (uint64_t)loop->config.hz;
    loop->prev_tick_qpc_100ns = 0;
    loop->seq = loop->config.start_seq;
    loop->last_calib_sec = 0;
    loop->last_heartbeat_sec = 0;

    telemetry_process_init(&loop->proc_monitor);

    /* Attempt to create high-resolution periodic waitable timer (Windows 10 1803+) */
    loop->h_timer = CreateWaitableTimerExW(
        NULL,
        NULL,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );

    /* Fallback to standard waitable timer if high-resolution flag is unsupported */
    if (!loop->h_timer) {
        loop->h_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }

    if (!loop->h_timer) {
        return false;
    }

    return true;
}

void timer_loop_run(timer_loop_t *loop) {
    if (!loop || !loop->writer || !loop->writer->is_initialized || !loop->h_timer) {
        return;
    }

    /* -------------------------------------------------------------------------
     * Startup Cadence: Pre-Sampling & Initial Anchor Records
     * ------------------------------------------------------------------------- */
    uint64_t start_tsc = 0, start_qpc_100ns = 0, start_ft = 0;
    calibration_capture_triple(loop->calibration, &start_tsc, &start_qpc_100ns, &start_ft);
    loop->prev_tick_qpc_100ns = start_qpc_100ns;

    /* Initialize CPU baseline measurement */
    FILETIME ft_idle, ft_kernel, ft_user;
    if (GetSystemTimes(&ft_idle, &ft_kernel, &ft_user)) {
        loop->prev_idle.LowPart = ft_idle.dwLowDateTime;
        loop->prev_idle.HighPart = ft_idle.dwHighDateTime;
        loop->prev_kernel.LowPart = ft_kernel.dwLowDateTime;
        loop->prev_kernel.HighPart = ft_kernel.dwHighDateTime;
        loop->prev_user.LowPart = ft_user.dwLowDateTime;
        loop->prev_user.HighPart = ft_user.dwHighDateTime;
        loop->has_prev_cpu = true;
    }

    /* Initial foreground process query */
    DWORD cur_fg_pid = 0;
    char cur_fg_name[32];
    uint8_t cur_fg_name_len = 0;
    bool cur_fg_ok = false;
    telemetry_process_sample(&loop->proc_monitor,
                             &cur_fg_pid,
                             cur_fg_name,
                             &cur_fg_name_len,
                             &cur_fg_ok);

    /* Initial NVML query */
    nvml_telemetry_t nvml_sample;
    uint16_t flags = 0;
    telemetry_nvml_sample(loop->nvml, &nvml_sample, &flags);
    if (cur_fg_ok) {
        flags |= BLACKBOX_FLAG_FG_OK;
    }

    if (loop->config.is_resume) {
        /*
         * Resuming existing ring session.
         * Write Type 2 Calibration record at seq = (max_seq + 1) to anchor the resumed run.
         */
        blackbox_record_t resume_cal;
        memset(&resume_cal, 0, sizeof(resume_cal));
        resume_cal.cal.type      = BLACKBOX_RECORD_TYPE_CALIBRATION;
        resume_cal.cal.seq       = loop->seq++;
        resume_cal.cal.tsc       = start_tsc;
        resume_cal.cal.qpc_100ns = start_qpc_100ns;
        resume_cal.cal.filetime  = start_ft;
        ring_buffer_writer_write_record(loop->writer, &resume_cal);
    } else {
        /*
         * Fresh ring session:
         * Record 0 (seq 0): Type 0 Telemetry Data Record.
         * Ensures slot 0 contains valid telemetry metrics and sentinels for immediate inspection.
         */
        blackbox_record_t initial_data;
        memset(&initial_data, 0, sizeof(initial_data));
        initial_data.data.type          = BLACKBOX_RECORD_TYPE_DATA;
        initial_data.data.pcie_gen      = nvml_sample.pcie_gen;
        initial_data.data.pcie_width    = nvml_sample.pcie_width;
        initial_data.data.cpu_pct       = 0;
        initial_data.data.gpu_temp_c    = nvml_sample.gpu_temp_c;
        initial_data.data.flags         = flags;
        initial_data.data.gpu_power_mw  = nvml_sample.gpu_power_mw;
        initial_data.data.pcie_replay   = nvml_sample.pcie_replay;
        initial_data.data.sm_clock_mhz  = nvml_sample.sm_clock_mhz;
        initial_data.data.mem_clock_mhz = nvml_sample.mem_clock_mhz;
        initial_data.data.fg_pid        = (uint32_t)cur_fg_pid;
        initial_data.data.seq           = loop->seq++;
        initial_data.data.tsc           = start_tsc;
        initial_data.data.qpc_100ns     = start_qpc_100ns;
        ring_buffer_writer_write_record(loop->writer, &initial_data);

        /*
         * Record 1 (seq 1): Type 2 Calibration Service Record.
         * Emitted at daemon startup.
         */
        blackbox_record_t startup_cal;
        memset(&startup_cal, 0, sizeof(startup_cal));
        startup_cal.cal.type      = BLACKBOX_RECORD_TYPE_CALIBRATION;
        startup_cal.cal.seq       = loop->seq++;
        startup_cal.cal.tsc       = start_tsc;
        startup_cal.cal.qpc_100ns = start_qpc_100ns;
        startup_cal.cal.filetime  = start_ft;
        ring_buffer_writer_write_record(loop->writer, &startup_cal);

        /*
         * Record 2 (seq 2): Type 1 Process Transition Record.
         * Emitted at startup if an active foreground process exists to seed pid -> name mapping.
         */
        if (cur_fg_pid != 0) {
            blackbox_record_t startup_proc;
            memset(&startup_proc, 0, sizeof(startup_proc));
            startup_proc.process.type      = BLACKBOX_RECORD_TYPE_PROCESS;
            startup_proc.process.flags     = (uint8_t)flags;
            startup_proc.process.name_len  = cur_fg_name_len;
            startup_proc.process.pad0      = 0;
            startup_proc.process.seq       = loop->seq++;
            startup_proc.process.tsc       = start_tsc;
            startup_proc.process.qpc_100ns = start_qpc_100ns;
            startup_proc.process.fg_pid    = (uint32_t)cur_fg_pid;
            memcpy(startup_proc.process.name, cur_fg_name, 32);
            ring_buffer_writer_write_record(loop->writer, &startup_proc);
        }
    }

    /* -------------------------------------------------------------------------
     * Arm Periodic Waitable Timer
     * ------------------------------------------------------------------------- */
    LARGE_INTEGER due_time;
    due_time.QuadPart = -10000LL; /* Relative delay of 1 ms to start first tick */
    LONG period_ms = (LONG)(1000 / loop->config.hz);
    if (period_ms < 1) period_ms = 1;

    SetWaitableTimer(loop->h_timer, &due_time, period_ms, NULL, NULL, FALSE);

    HANDLE wait_handles[2];
    wait_handles[0] = loop->config.h_stop_event;
    wait_handles[1] = loop->h_timer;
    DWORD num_handles = (loop->config.h_stop_event != NULL) ? 2 : 1;
    if (num_handles == 1) {
        wait_handles[0] = loop->h_timer;
    }

    /* -------------------------------------------------------------------------
     * Hot Loop: Invariants Strictly Maintained
     * Zero malloc/HeapAlloc, Zero printf, Zero locks, Pure Win32, Single Thread
     * ------------------------------------------------------------------------- */
    while (true) {
        DWORD wait_res = WaitForMultipleObjects(num_handles, wait_handles, FALSE, INFINITE);

        /* Handle clean termination event */
        if (num_handles == 2 && wait_res == WAIT_OBJECT_0) {
            break;
        }

        /* Check for timer signal */
        DWORD timer_index = (num_handles == 2) ? (WAIT_OBJECT_0 + 1) : WAIT_OBJECT_0;
        if (wait_res != timer_index) {
            /* Unexpected error or abandoned wait */
            break;
        }

        /* 1. Time & Lateness Sampling */
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        uint64_t current_qpc_100ns = calibration_qpc_ticks_to_100ns((uint64_t)qpc_now.QuadPart,
                                                                    loop->calibration->qpc_freq);
        uint64_t current_tsc = blackbox_rdtsc();

        bool is_late = false;
        if (loop->prev_tick_qpc_100ns != 0) {
            uint64_t dt_100ns = current_qpc_100ns - loop->prev_tick_qpc_100ns;
            if (dt_100ns > (2ULL * loop->nominal_interval_100ns)) {
                is_late = true;
            }
        }
        loop->prev_tick_qpc_100ns = current_qpc_100ns;

        /* 2. System CPU Percentage Calculation */
        uint8_t cpu_pct = 0;
        if (GetSystemTimes(&ft_idle, &ft_kernel, &ft_user)) {
            ULARGE_INTEGER cur_idle, cur_kernel, cur_user;
            cur_idle.LowPart = ft_idle.dwLowDateTime;     cur_idle.HighPart = ft_idle.dwHighDateTime;
            cur_kernel.LowPart = ft_kernel.dwLowDateTime; cur_kernel.HighPart = ft_kernel.dwHighDateTime;
            cur_user.LowPart = ft_user.dwLowDateTime;     cur_user.HighPart = ft_user.dwHighDateTime;

            if (loop->has_prev_cpu) {
                uint64_t idle_delta   = cur_idle.QuadPart   - loop->prev_idle.QuadPart;
                uint64_t kernel_delta = cur_kernel.QuadPart - loop->prev_kernel.QuadPart;
                uint64_t user_delta   = cur_user.QuadPart   - loop->prev_user.QuadPart;
                uint64_t total_delta  = kernel_delta + user_delta;

                if (total_delta > 0 && idle_delta < total_delta) {
                    uint64_t busy_delta = total_delta - idle_delta;
                    uint64_t pct = (busy_delta * 100ULL) / total_delta;
                    cpu_pct = (pct > 100) ? 100 : (uint8_t)pct;
                }
            }
            loop->prev_idle = cur_idle;
            loop->prev_kernel = cur_kernel;
            loop->prev_user = cur_user;
            loop->has_prev_cpu = true;
        }

        /* 3. Dynamic NVML Telemetry Sampling */
        telemetry_nvml_sample(loop->nvml, &nvml_sample, &flags);
        if (is_late) {
            flags |= BLACKBOX_FLAG_TIMER_LATE;
        }

        /* 4. Foreground Process Transition Sampling */
        DWORD fg_pid = 0;
        char fg_name[32];
        uint8_t fg_name_len = 0;
        bool fg_ok = false;
        bool is_transition = telemetry_process_sample(&loop->proc_monitor,
                                                      &fg_pid,
                                                      fg_name,
                                                      &fg_name_len,
                                                      &fg_ok);
        if (fg_ok) {
            flags |= BLACKBOX_FLAG_FG_OK;
        }

        /* 5. Emit Type 1 (Process Transition) Record if foreground process changed */
        if (is_transition && fg_pid != 0) {
            blackbox_record_t proc_rec;
            memset(&proc_rec, 0, sizeof(proc_rec));
            proc_rec.process.type      = BLACKBOX_RECORD_TYPE_PROCESS;
            proc_rec.process.flags     = (uint8_t)flags;
            proc_rec.process.name_len  = fg_name_len;
            proc_rec.process.pad0      = 0;
            proc_rec.process.seq       = loop->seq++;
            proc_rec.process.tsc       = current_tsc;
            proc_rec.process.qpc_100ns = current_qpc_100ns;
            proc_rec.process.fg_pid    = (uint32_t)fg_pid;
            memcpy(proc_rec.process.name, fg_name, 32);
            ring_buffer_writer_write_record(loop->writer, &proc_rec);
        }

        /* 6. Emit Type 0 (Periodic Telemetry Data) Record */
        blackbox_record_t data_rec;
        memset(&data_rec, 0, sizeof(data_rec));
        data_rec.data.type          = BLACKBOX_RECORD_TYPE_DATA;
        data_rec.data.pcie_gen      = nvml_sample.pcie_gen;
        data_rec.data.pcie_width    = nvml_sample.pcie_width;
        data_rec.data.cpu_pct       = cpu_pct;
        data_rec.data.gpu_temp_c    = nvml_sample.gpu_temp_c;
        data_rec.data.flags         = flags;
        data_rec.data.gpu_power_mw  = nvml_sample.gpu_power_mw;
        data_rec.data.pcie_replay   = nvml_sample.pcie_replay;
        data_rec.data.sm_clock_mhz  = nvml_sample.sm_clock_mhz;
        data_rec.data.mem_clock_mhz = nvml_sample.mem_clock_mhz;
        data_rec.data.fg_pid        = (uint32_t)fg_pid;
        data_rec.data.seq           = loop->seq++;
        data_rec.data.tsc           = current_tsc;
        data_rec.data.qpc_100ns     = current_qpc_100ns;
        ring_buffer_writer_write_record(loop->writer, &data_rec);

        /* 7. Cadence Check: Type 3 (Heartbeat) Record (every 10 seconds) */
        uint32_t uptime_sec = (uint32_t)((current_qpc_100ns - loop->calibration->qpc_start_100ns) / 10000000ULL);
        if (uptime_sec >= (loop->last_heartbeat_sec + BLACKBOX_HEARTBEAT_INTERVAL_SEC)) {
            blackbox_record_t hb_rec;
            memset(&hb_rec, 0, sizeof(hb_rec));
            hb_rec.heartbeat.type       = BLACKBOX_RECORD_TYPE_HEARTBEAT;
            hb_rec.heartbeat.cpu_pct    = cpu_pct;
            hb_rec.heartbeat.flags      = flags;
            hb_rec.heartbeat.fg_pid     = (uint32_t)fg_pid;
            hb_rec.heartbeat.seq        = loop->seq++;
            hb_rec.heartbeat.tsc        = current_tsc;
            hb_rec.heartbeat.qpc_100ns  = current_qpc_100ns;
            hb_rec.heartbeat.uptime_sec = uptime_sec;
            ring_buffer_writer_write_record(loop->writer, &hb_rec);
            loop->last_heartbeat_sec = uptime_sec;
        }

        /* 8. Cadence Check: Type 2 (Calibration Resync) Record (every 3600 seconds) */
        if (uptime_sec >= (loop->last_calib_sec + BLACKBOX_CALIBRATION_INTERVAL_SEC)) {
            uint64_t cal_tsc = 0, cal_qpc = 0, cal_ft = 0;
            calibration_capture_triple(loop->calibration, &cal_tsc, &cal_qpc, &cal_ft);

            blackbox_record_t cal_rec;
            memset(&cal_rec, 0, sizeof(cal_rec));
            cal_rec.cal.type      = BLACKBOX_RECORD_TYPE_CALIBRATION;
            cal_rec.cal.seq       = loop->seq++;
            cal_rec.cal.tsc       = cal_tsc;
            cal_rec.cal.qpc_100ns = cal_qpc;
            cal_rec.cal.filetime  = cal_ft;
            ring_buffer_writer_write_record(loop->writer, &cal_rec);
            loop->last_calib_sec = uptime_sec;
        }
    }

    CancelWaitableTimer(loop->h_timer);
}

void timer_loop_cleanup(timer_loop_t *loop) {
    if (!loop) return;

    if (loop->h_timer) {
        CancelWaitableTimer(loop->h_timer);
        CloseHandle(loop->h_timer);
        loop->h_timer = NULL;
    }
}
