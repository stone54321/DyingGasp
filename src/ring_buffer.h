#ifndef BLACKBOX_RING_BUFFER_H
#define BLACKBOX_RING_BUFFER_H

#include "blackbox_format.h"
#include "blackbox_common.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Ring Buffer Writer (Collector Hot Path)                                   */
/* ========================================================================= */

typedef struct {
    HANDLE            h_file;          /**< Win32 file handle opened with FILE_FLAG_WRITE_THROUGH */
    uint64_t          ring_bytes;      /**< Total pre-allocated physical file size */
    uint32_t          record_count;    /**< Total slots: (ring_bytes - 4096) / 64 */
    blackbox_header_t header;          /**< Active file header */
    bool              is_initialized;  /**< Initialization flag */
} ring_buffer_writer_t;

/**
 * @brief Initialize ring buffer writer with a wide-character file path.
 *
 * Pre-allocates the file to exactly `ring_bytes` using CreateFileW,
 * SetFilePointerEx, and SetEndOfFile. Writes the 4096-byte header at offset 0.
 *
 * @param writer Pointer to uninitialized writer struct.
 * @param file_path Wide-character null-terminated path (e.g. L"C:\\ProgramData\\blackbox\\ring.bin").
 * @param ring_bytes Total file size in bytes (e.g. 64 MB = 67,108,864).
 * @param initial_header Pointer to header data to commit at offset 0.
 * @return true on success, false on error.
 */
bool ring_buffer_writer_init_w(ring_buffer_writer_t *writer,
                               const wchar_t *file_path,
                               uint64_t ring_bytes,
                               const blackbox_header_t *initial_header);

/**
 * @brief Initialize ring buffer writer with a UTF-8 / ANSI file path.
 */
bool ring_buffer_writer_init_a(ring_buffer_writer_t *writer,
                               const char *file_path,
                               uint64_t ring_bytes,
                               const blackbox_header_t *initial_header);

/**
 * @brief Hot-path write record to pre-allocated ring slot.
 *
 * Strictly zero dynamic allocations, zero heap calls, zero locks.
 * Computes slot offset via 4096 + (seq % record_count) * 64,
 * seeks via SetFilePointerEx, and commits via WriteFile.
 *
 * @param writer Initialized writer instance.
 * @param record Pointer to 64-byte record to persist.
 * @return true if write succeeded and exactly 64 bytes were written, false otherwise.
 */
bool ring_buffer_writer_write_record(ring_buffer_writer_t *writer,
                                     const blackbox_record_t *record);

/**
 * @brief Update the 4096-byte header at offset 0 (e.g. on clean shutdown or calibration resync).
 */
bool ring_buffer_writer_update_header(ring_buffer_writer_t *writer,
                                      const blackbox_header_t *header);

/**
 * @brief Close writer file handle and release resources.
 */
void ring_buffer_writer_close(ring_buffer_writer_t *writer);


/* ========================================================================= */
/* Ring Buffer Reader & Traversal (Analyzer Engine)                         */
/* ========================================================================= */

typedef struct {
    HANDLE            h_file;          /**< Win32 file handle opened for shared read */
    uint64_t          file_size;       /**< Actual file size on disk */
    blackbox_header_t header;          /**< Header loaded from disk */
    uint32_t          record_count;    /**< Total slots available */
    bool              is_valid_header; /**< Magic and version validated */
} ring_buffer_reader_t;

/**
 * @brief Open ring buffer for read-only inspection (wide path).
 */
bool ring_buffer_reader_open_w(ring_buffer_reader_t *reader, const wchar_t *file_path);

/**
 * @brief Open ring buffer for read-only inspection (narrow path).
 */
bool ring_buffer_reader_open_a(ring_buffer_reader_t *reader, const char *file_path);

/**
 * @brief Read a single 64-byte record from a designated slot index (0 .. record_count - 1).
 */
bool ring_buffer_reader_read_slot(const ring_buffer_reader_t *reader,
                                  uint32_t slot_index,
                                  blackbox_record_t *out_record);

/**
 * @brief Validate whether a record is structurally sound and belongs to the given slot.
 *
 * Checks record type bounds (0..3) and verifies slot sequence invariant:
 * (record->seq % record_count) == slot_index.
 */
bool ring_buffer_is_record_valid(const blackbox_record_t *record,
                                 uint32_t slot_index,
                                 uint32_t record_count);

/**
 * @brief Check if a 64-byte record is entirely unwritten / zeroed.
 */
bool ring_buffer_is_record_empty(const blackbox_record_t *record);

/**
 * @brief Scan the ring buffer and determine sequence boundaries.
 *
 * Populates min_seq, max_seq, total_valid_records, wrap_count, and newest_slot.
 */
bool ring_buffer_scan_bounds(const ring_buffer_reader_t *reader,
                             uint64_t *out_min_seq,
                             uint64_t *out_max_seq,
                             uint64_t *out_valid_count,
                             uint32_t *out_wrap_count,
                             uint32_t *out_newest_slot);

/**
 * @brief Close reader file handle.
 */
void ring_buffer_reader_close(ring_buffer_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif /* BLACKBOX_RING_BUFFER_H */
