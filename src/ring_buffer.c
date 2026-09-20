#include "ring_buffer.h"
#include <string.h>

/* Helper to ensure directory path exists before creating file */
static void ensure_parent_directory_w(const wchar_t *file_path) {
    if (!file_path) return;
    wchar_t dir_path[MAX_PATH];
    wcsncpy(dir_path, file_path, MAX_PATH - 1);
    dir_path[MAX_PATH - 1] = L'\0';

    wchar_t *last_slash = wcsrchr(dir_path, L'\\');
    if (!last_slash) {
        last_slash = wcsrchr(dir_path, L'/');
    }

    if (last_slash) {
        *last_slash = L'\0';
        /* Recursive directory creation if needed */
        for (wchar_t *p = dir_path; *p; ++p) {
            if (*p == L'\\' || *p == L'/') {
                wchar_t temp = *p;
                *p = L'\0';
                if (wcslen(dir_path) > 2 && dir_path[wcslen(dir_path) - 1] != L':') {
                    CreateDirectoryW(dir_path, NULL);
                }
                *p = temp;
            }
        }
        CreateDirectoryW(dir_path, NULL);
    }
}

bool ring_buffer_writer_init_w(ring_buffer_writer_t *writer,
                               const wchar_t *file_path,
                               uint64_t ring_bytes,
                               const blackbox_header_t *initial_header) {
    if (!writer || !file_path || ring_bytes < (BLACKBOX_HEADER_SIZE + BLACKBOX_RECORD_SIZE)) {
        return false;
    }

    writer->h_file = INVALID_HANDLE_VALUE;
    writer->ring_bytes = 0;
    writer->record_count = 0;
    writer->is_initialized = false;
    memset(&writer->header, 0, sizeof(writer->header));

    ensure_parent_directory_w(file_path);

    /* Open file with write-through flag: unbuffered synchronous persistence */
    HANDLE h_file = CreateFileW(
        file_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL
    );

    if (h_file == INVALID_HANDLE_VALUE) {
        return false;
    }

    /* Check existing physical size */
    LARGE_INTEGER current_size;
    if (!GetFileSizeEx(h_file, &current_size)) {
        CloseHandle(h_file);
        return false;
    }

    /* Pre-allocate file if size does not match requested ring_bytes */
    if ((uint64_t)current_size.QuadPart != ring_bytes) {
        LARGE_INTEGER target_size;
        target_size.QuadPart = (LONGLONG)ring_bytes;
        if (!SetFilePointerEx(h_file, target_size, NULL, FILE_BEGIN) || !SetEndOfFile(h_file)) {
            CloseHandle(h_file);
            return false;
        }
    }

    /* Write 4 KB initial header at byte offset 0 */
    LARGE_INTEGER zero_offset;
    zero_offset.QuadPart = 0;
    if (!SetFilePointerEx(h_file, zero_offset, NULL, FILE_BEGIN)) {
        CloseHandle(h_file);
        return false;
    }

    DWORD bytes_written = 0;
    blackbox_header_t hdr;
    if (initial_header) {
        memcpy(&hdr, initial_header, sizeof(hdr));
    } else {
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = BLACKBOX_MAGIC;
        hdr.version = BLACKBOX_VERSION;
        hdr.header_size = BLACKBOX_HEADER_SIZE;
        hdr.ring_bytes = ring_bytes;
        hdr.record_size = BLACKBOX_RECORD_SIZE;
        hdr.record_count = (uint32_t)((ring_bytes - BLACKBOX_HEADER_SIZE) / BLACKBOX_RECORD_SIZE);
        hdr.nominal_hz = 10;
    }

    /* Ensure geometry fields are correct */
    hdr.magic = BLACKBOX_MAGIC;
    hdr.version = BLACKBOX_VERSION;
    hdr.header_size = BLACKBOX_HEADER_SIZE;
    hdr.ring_bytes = ring_bytes;
    hdr.record_size = BLACKBOX_RECORD_SIZE;
    hdr.record_count = (uint32_t)((ring_bytes - BLACKBOX_HEADER_SIZE) / BLACKBOX_RECORD_SIZE);

    if (!WriteFile(h_file, &hdr, sizeof(hdr), &bytes_written, NULL) ||
        bytes_written != sizeof(hdr)) {
        CloseHandle(h_file);
        return false;
    }

    writer->h_file = h_file;
    writer->ring_bytes = ring_bytes;
    writer->record_count = hdr.record_count;
    memcpy(&writer->header, &hdr, sizeof(hdr));
    writer->is_initialized = true;

    return true;
}

bool ring_buffer_writer_init_a(ring_buffer_writer_t *writer,
                               const char *file_path,
                               uint64_t ring_bytes,
                               const blackbox_header_t *initial_header) {
    if (!file_path) return false;
    wchar_t path_w[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, file_path, -1, path_w, MAX_PATH) == 0) {
        return false;
    }
    return ring_buffer_writer_init_w(writer, path_w, ring_bytes, initial_header);
}

bool ring_buffer_writer_write_record(ring_buffer_writer_t *writer,
                                     const blackbox_record_t *record) {
    /* Hot-path guarantees: strictly zero malloc/free/HeapAlloc, zero locks */
    if (!writer || !writer->is_initialized || !record || writer->record_count == 0) {
        return false;
    }

    uint64_t seq = blackbox_record_get_seq(record);
    uint64_t byte_offset = BLACKBOX_CALC_OFFSET(seq, writer->record_count);

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)byte_offset;

    if (!SetFilePointerEx(writer->h_file, li, NULL, FILE_BEGIN)) {
        return false;
    }

    DWORD bytes_written = 0;
    if (!WriteFile(writer->h_file, record, sizeof(blackbox_record_t), &bytes_written, NULL)) {
        return false;
    }

    return (bytes_written == sizeof(blackbox_record_t));
}

bool ring_buffer_writer_update_header(ring_buffer_writer_t *writer,
                                      const blackbox_header_t *header) {
    if (!writer || !writer->is_initialized || !header) {
        return false;
    }

    LARGE_INTEGER zero_offset;
    zero_offset.QuadPart = 0;
    if (!SetFilePointerEx(writer->h_file, zero_offset, NULL, FILE_BEGIN)) {
        return false;
    }

    DWORD bytes_written = 0;
    if (!WriteFile(writer->h_file, header, sizeof(blackbox_header_t), &bytes_written, NULL) ||
        bytes_written != sizeof(blackbox_header_t)) {
        return false;
    }

    memcpy(&writer->header, header, sizeof(blackbox_header_t));
    return true;
}

void ring_buffer_writer_close(ring_buffer_writer_t *writer) {
    if (writer && writer->h_file != INVALID_HANDLE_VALUE) {
        CloseHandle(writer->h_file);
        writer->h_file = INVALID_HANDLE_VALUE;
        writer->is_initialized = false;
    }
}


/* ========================================================================= */
/* Ring Buffer Reader & Traversal                                            */
/* ========================================================================= */

bool ring_buffer_reader_open_w(ring_buffer_reader_t *reader, const wchar_t *file_path) {
    if (!reader || !file_path) return false;

    memset(reader, 0, sizeof(*reader));
    reader->h_file = INVALID_HANDLE_VALUE;

    HANDLE h_file = CreateFileW(
        file_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (h_file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(h_file, &file_size)) {
        CloseHandle(h_file);
        return false;
    }

    reader->file_size = (uint64_t)file_size.QuadPart;
    if (reader->file_size < BLACKBOX_HEADER_SIZE) {
        CloseHandle(h_file);
        return false;
    }

    /* Read header */
    LARGE_INTEGER zero_offset;
    zero_offset.QuadPart = 0;
    if (!SetFilePointerEx(h_file, zero_offset, NULL, FILE_BEGIN)) {
        CloseHandle(h_file);
        return false;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(h_file, &reader->header, sizeof(reader->header), &bytes_read, NULL) ||
        bytes_read != sizeof(reader->header)) {
        CloseHandle(h_file);
        return false;
    }

    /* Validate header invariants */
    if (reader->header.magic != BLACKBOX_MAGIC ||
        reader->header.version != BLACKBOX_VERSION ||
        reader->header.header_size != BLACKBOX_HEADER_SIZE ||
        reader->header.record_size != BLACKBOX_RECORD_SIZE) {
        CloseHandle(h_file);
        return false;
    }

    reader->h_file = h_file;
    reader->record_count = (uint32_t)((reader->file_size - BLACKBOX_HEADER_SIZE) / BLACKBOX_RECORD_SIZE);
    reader->is_valid_header = true;

    return true;
}

bool ring_buffer_reader_open_a(ring_buffer_reader_t *reader, const char *file_path) {
    if (!file_path) return false;
    wchar_t path_w[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, file_path, -1, path_w, MAX_PATH) == 0) {
        return false;
    }
    return ring_buffer_reader_open_w(reader, path_w);
}

bool ring_buffer_reader_read_slot(const ring_buffer_reader_t *reader,
                                  uint32_t slot_index,
                                  blackbox_record_t *out_record) {
    if (!reader || reader->h_file == INVALID_HANDLE_VALUE || !out_record ||
        slot_index >= reader->record_count) {
        return false;
    }

    uint64_t byte_offset = (uint64_t)BLACKBOX_HEADER_SIZE + ((uint64_t)slot_index * (uint64_t)BLACKBOX_RECORD_SIZE);
    if (byte_offset + sizeof(blackbox_record_t) > reader->file_size) {
        return false;
    }

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)byte_offset;
    if (!SetFilePointerEx(reader->h_file, li, NULL, FILE_BEGIN)) {
        return false;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(reader->h_file, out_record, sizeof(blackbox_record_t), &bytes_read, NULL)) {
        return false;
    }

    return (bytes_read == sizeof(blackbox_record_t));
}

bool ring_buffer_is_record_empty(const blackbox_record_t *record) {
    if (!record) return true;
    const uint64_t *p64 = (const uint64_t *)record->raw;
    for (size_t i = 0; i < sizeof(blackbox_record_t) / sizeof(uint64_t); ++i) {
        if (p64[i] != 0) return false;
    }
    return true;
}

bool ring_buffer_is_record_valid(const blackbox_record_t *record,
                                 uint32_t slot_index,
                                 uint32_t record_count) {
    if (!record || record_count == 0) return false;
    if (ring_buffer_is_record_empty(record)) return false;

    /* Check type bounds (0..3) */
    if (record->common.type > BLACKBOX_RECORD_TYPE_HEARTBEAT) {
        return false;
    }

    /* Verify slot sequence invariant: (seq % record_count) == slot_index */
    uint64_t seq = blackbox_record_get_seq(record);
    if ((seq % (uint64_t)record_count) != (uint64_t)slot_index) {
        return false;
    }

    return true;
}

bool ring_buffer_scan_bounds(const ring_buffer_reader_t *reader,
                             uint64_t *out_min_seq,
                             uint64_t *out_max_seq,
                             uint64_t *out_valid_count,
                             uint32_t *out_wrap_count,
                             uint32_t *out_newest_slot) {
    if (!reader || !reader->is_valid_header || reader->record_count == 0) {
        return false;
    }

    uint64_t min_seq = UINT64_MAX;
    uint64_t max_seq = 0;
    uint64_t valid_count = 0;
    uint32_t newest_slot = 0;
    bool found_any = false;

    blackbox_record_t rec;
    for (uint32_t slot = 0; slot < reader->record_count; ++slot) {
        if (!ring_buffer_reader_read_slot(reader, slot, &rec)) {
            continue;
        }

        if (ring_buffer_is_record_valid(&rec, slot, reader->record_count)) {
            uint64_t seq = blackbox_record_get_seq(&rec);
            if (!found_any || seq < min_seq) {
                min_seq = seq;
            }
            if (!found_any || seq >= max_seq) {
                max_seq = seq;
                newest_slot = slot;
            }
            valid_count++;
            found_any = true;
        }
    }

    if (!found_any) {
        if (out_min_seq) *out_min_seq = 0;
        if (out_max_seq) *out_max_seq = 0;
        if (out_valid_count) *out_valid_count = 0;
        if (out_wrap_count) *out_wrap_count = 0;
        if (out_newest_slot) *out_newest_slot = 0;
        return false;
    }

    if (out_min_seq) *out_min_seq = min_seq;
    if (out_max_seq) *out_max_seq = max_seq;
    if (out_valid_count) *out_valid_count = valid_count;
    if (out_wrap_count) {
        *out_wrap_count = (uint32_t)(max_seq / (uint64_t)reader->record_count);
    }
    if (out_newest_slot) *out_newest_slot = newest_slot;

    return true;
}

void ring_buffer_reader_close(ring_buffer_reader_t *reader) {
    if (reader && reader->h_file != INVALID_HANDLE_VALUE) {
        CloseHandle(reader->h_file);
        reader->h_file = INVALID_HANDLE_VALUE;
        reader->is_valid_header = false;
    }
}
