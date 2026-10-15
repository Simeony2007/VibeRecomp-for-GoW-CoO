#include "mips_prx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_PRX_HEADER_SIZE 0x150U
#define SCE_HEADER_MIN_SIZE 8U

static uint16_t read_u16_le(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

void mips_prx_file_release(MIPS_PRX_File *file) {
    if (file == NULL) return;
    free(file->file_data);
    memset(file, 0, sizeof(*file));
}

MIPS_PRX_ReadStatus mips_prx_read_file(const char *path, MIPS_PRX_File *out) {
    FILE *source;
    long file_length;
    size_t read_size;
    size_t payload_offset = 0;
    uint8_t *payload;

    if (out == NULL) return MIPS_PRX_READ_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (path == NULL || path[0] == '\0') return MIPS_PRX_READ_INVALID_ARGUMENT;

    source = fopen(path, "rb");
    if (source == NULL) return MIPS_PRX_READ_OPEN_FAILED;
    if (fseek(source, 0, SEEK_END) != 0 ||
        (file_length = ftell(source)) < 0 ||
        fseek(source, 0, SEEK_SET) != 0) {
        fclose(source);
        return MIPS_PRX_READ_IO_ERROR;
    }
    if (file_length == 0) {
        fclose(source);
        return MIPS_PRX_READ_EMPTY;
    }
    if ((uint64_t)file_length > MIPS_PRX_MAX_FILE_SIZE) {
        fclose(source);
        return MIPS_PRX_READ_TOO_LARGE;
    }

    out->file_data = (uint8_t *)malloc((size_t)file_length);
    if (out->file_data == NULL) {
        fclose(source);
        return MIPS_PRX_READ_IO_ERROR;
    }
    read_size = fread(out->file_data, 1, (size_t)file_length, source);
    if (ferror(source) || read_size != (size_t)file_length) {
        fclose(source);
        mips_prx_file_release(out);
        return MIPS_PRX_READ_IO_ERROR;
    }
    fclose(source);
    out->file_size = read_size;

    if (out->file_size >= 4 && memcmp(out->file_data, "~SCE", 4) == 0) {
        uint32_t header_size;
        if (out->file_size < SCE_HEADER_MIN_SIZE) {
            mips_prx_file_release(out);
            return MIPS_PRX_READ_BAD_SCE_HEADER;
        }
        header_size = read_u32_le(out->file_data + 4);
        if (header_size < SCE_HEADER_MIN_SIZE || header_size >= out->file_size) {
            mips_prx_file_release(out);
            return MIPS_PRX_READ_BAD_SCE_HEADER;
        }
        payload_offset = header_size;
        out->container = MIPS_PRX_CONTAINER_SCE;
    } else {
        out->container = MIPS_PRX_CONTAINER_PSP;
    }

    payload = out->file_data + payload_offset;
    out->payload_offset = payload_offset;
    out->payload_size = out->file_size - payload_offset;

    if (out->payload_size >= 4 && memcmp(payload, "\x7f" "ELF", 4) == 0) {
        out->container = MIPS_PRX_CONTAINER_ELF;
        return MIPS_PRX_READ_OK;
    }
    if (out->payload_size < 4 || memcmp(payload, "~PSP", 4) != 0) {
        mips_prx_file_release(out);
        return MIPS_PRX_READ_UNSUPPORTED_FORMAT;
    }
    if (out->payload_size < PSP_PRX_HEADER_SIZE) {
        mips_prx_file_release(out);
        return MIPS_PRX_READ_TRUNCATED_PSP;
    }

    out->psp_size = read_u32_le(payload + 0x2C);
    out->elf_size = read_u32_le(payload + 0x28);
    out->compression_attributes = read_u16_le(payload + 0x06);
    out->decrypt_mode = payload[0x7C];
    out->tag = read_u32_le(payload + 0xD0);
    out->has_psp_header = 1;
    if (out->psp_size < PSP_PRX_HEADER_SIZE ||
        out->psp_size > out->payload_size) {
        mips_prx_file_release(out);
        return MIPS_PRX_READ_BAD_PSP_SIZE;
    }
    out->payload_size = out->psp_size;
    return MIPS_PRX_READ_OK;
}

const char *mips_prx_read_status_name(MIPS_PRX_ReadStatus status) {
    switch (status) {
        case MIPS_PRX_READ_OK: return "ok";
        case MIPS_PRX_READ_INVALID_ARGUMENT: return "invalid_argument";
        case MIPS_PRX_READ_OPEN_FAILED: return "open_failed";
        case MIPS_PRX_READ_IO_ERROR: return "io_error";
        case MIPS_PRX_READ_EMPTY: return "empty";
        case MIPS_PRX_READ_TOO_LARGE: return "too_large";
        case MIPS_PRX_READ_BAD_SCE_HEADER: return "bad_sce_header";
        case MIPS_PRX_READ_UNSUPPORTED_FORMAT: return "unsupported_format";
        case MIPS_PRX_READ_TRUNCATED_PSP: return "truncated_psp";
        case MIPS_PRX_READ_BAD_PSP_SIZE: return "bad_psp_size";
    }
    return "unknown_status";
}

const char *mips_prx_container_name(MIPS_PRX_Container container) {
    switch (container) {
        case MIPS_PRX_CONTAINER_ELF: return "ELF";
        case MIPS_PRX_CONTAINER_PSP: return "~PSP";
        case MIPS_PRX_CONTAINER_SCE: return "~SCE";
    }
    return "unknown";
}
