#ifndef MIPS_PRX_H
#define MIPS_PRX_H

#include <stddef.h>
#include <stdint.h>

#define MIPS_PRX_MAX_FILE_SIZE (64U * 1024U * 1024U)

typedef enum {
    MIPS_PRX_READ_OK = 0,
    MIPS_PRX_READ_INVALID_ARGUMENT,
    MIPS_PRX_READ_OPEN_FAILED,
    MIPS_PRX_READ_IO_ERROR,
    MIPS_PRX_READ_EMPTY,
    MIPS_PRX_READ_TOO_LARGE,
    MIPS_PRX_READ_BAD_SCE_HEADER,
    MIPS_PRX_READ_UNSUPPORTED_FORMAT,
    MIPS_PRX_READ_TRUNCATED_PSP,
    MIPS_PRX_READ_BAD_PSP_SIZE
} MIPS_PRX_ReadStatus;

typedef enum {
    MIPS_PRX_CONTAINER_ELF,
    MIPS_PRX_CONTAINER_PSP,
    MIPS_PRX_CONTAINER_SCE
} MIPS_PRX_Container;

typedef struct {
    uint8_t *file_data;
    size_t file_size;
    size_t payload_offset;
    size_t payload_size;
    MIPS_PRX_Container container;
    uint32_t psp_size;
    uint32_t elf_size;
    uint32_t tag;
    uint16_t compression_attributes;
    uint8_t decrypt_mode;
    int has_psp_header;
} MIPS_PRX_File;

typedef enum {
    MIPS_PRX_PREPARE_OK = 0,
    MIPS_PRX_PREPARE_INVALID_ARGUMENT,
    MIPS_PRX_PREPARE_DECRYPT_FAILED,
    MIPS_PRX_PREPARE_UNSUPPORTED_COMPRESSION,
    MIPS_PRX_PREPARE_DECOMPRESSION_FAILED,
    MIPS_PRX_PREPARE_INVALID_ELF,
    MIPS_PRX_PREPARE_OUT_OF_MEMORY
} MIPS_PRX_PrepareStatus;

#ifdef __cplusplus
extern "C" {
#endif

MIPS_PRX_ReadStatus mips_prx_read_file(const char *path, MIPS_PRX_File *out);
void mips_prx_file_release(MIPS_PRX_File *file);
const char *mips_prx_read_status_name(MIPS_PRX_ReadStatus status);
const char *mips_prx_container_name(MIPS_PRX_Container container);
MIPS_PRX_PrepareStatus mips_prx_prepare_elf(const MIPS_PRX_File *file,
                                            uint8_t **elf_data,
                                            size_t *elf_size);
const char *mips_prx_prepare_status_name(MIPS_PRX_PrepareStatus status);

#ifdef __cplusplus
}
#endif

#endif
