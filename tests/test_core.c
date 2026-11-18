#include "timer_loop.h"
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void) {
    /* The caller must see persistence failure, including startup writes. */
    calibration_state_t cal;
    CHECK(calibration_init(&cal, 1));
    nvml_context_t nvml = {0};
    ring_buffer_writer_t writer = {0};
    writer.h_file = INVALID_HANDLE_VALUE;
    writer.is_initialized = true;
    writer.record_count = 16;
    timer_loop_config_t config = {0};
    config.hz = 10;
    config.h_stop_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    CHECK(config.h_stop_event != NULL);
    timer_loop_t loop;
    CHECK(timer_loop_init(&loop, &config, &writer, &cal, &nvml));
    CHECK(!timer_loop_run(&loop));
    timer_loop_cleanup(&loop);

    wchar_t temp_dir[MAX_PATH], path[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp_dir) > 0);
    CHECK(GetTempFileNameW(temp_dir, L"bbr", 0, path) != 0);
    CHECK(ring_buffer_writer_init_w(&writer, path, 5120, NULL));
    CHECK(timer_loop_init(&loop, &config, &writer, &cal, &nvml));
    CHECK(timer_loop_run(&loop));
    timer_loop_cleanup(&loop);

    ring_buffer_reader_t reader;
    CHECK(ring_buffer_reader_open_w(&reader, path));
    blackbox_record_t rec;
    CHECK(ring_buffer_reader_read_slot(&reader, 0, &rec));
    CHECK(rec.common.type == BLACKBOX_RECORD_TYPE_DATA);
    CHECK(rec.data.gpu_temp_c == BLACKBOX_SENTINEL_U16);
    CHECK(rec.data.gpu_power_mw == BLACKBOX_SENTINEL_U32);
    CHECK(rec.data.pcie_replay == BLACKBOX_SENTINEL_U32);
    CHECK(rec.data.sm_clock_mhz == BLACKBOX_SENTINEL_U32);
    CHECK(rec.data.mem_clock_mhz == BLACKBOX_SENTINEL_U32);
    CHECK(rec.data.pcie_gen == BLACKBOX_SENTINEL_U8);
    CHECK(rec.data.pcie_width == BLACKBOX_SENTINEL_U8);
    CHECK(!(rec.data.flags & BLACKBOX_FLAG_NVML_AVAILABLE));
    ring_buffer_reader_close(&reader);

    /* A failed timer arm must not block forever or report a clean stop. */
    CHECK(timer_loop_init(&loop, &config, &writer, &cal, &nvml));
    CloseHandle(loop.h_timer);
    loop.h_timer = INVALID_HANDLE_VALUE;
    CHECK(!timer_loop_run(&loop));
    loop.h_timer = NULL;
    ring_buffer_writer_close(&writer);
    CloseHandle(config.h_stop_event);
    CHECK(DeleteFileW(path));
    puts("PASS: write failures, timer-arm failures, clean stop, no-GPU sentinels");
    return 0;
}
