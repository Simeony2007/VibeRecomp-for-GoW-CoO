#include "mips_prx.h"

#include "Core/ELF/PrxDecrypter.h"
#include "Common/Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <zlib.h>

bool logEnabled = false;
bool *g_bLogEnabledSetting = &logEnabled;
LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS]{};
void GenericLog(Log, LogLevel, const char *, int, const char *, ...) {}

static uint32_t read_u32_le(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool valid_mips_elf(const uint8_t *data, size_t size) {
    if (size < 52 || std::memcmp(data, "\x7f" "ELF", 4) != 0 ||
        data[4] != 1 || data[5] != 1 || read_u32_le(data + 20) != 1 ||
        data[18] != 8 || data[19] != 0) {
        return false;
    }

    const uint32_t phoff = read_u32_le(data + 28);
    const uint16_t phentsize = (uint16_t)data[42] | ((uint16_t)data[43] << 8);
    const uint16_t phnum = (uint16_t)data[44] | ((uint16_t)data[45] << 8);
    return phentsize >= 32 && phoff <= size &&
           phnum <= (size - phoff) / phentsize;
}

extern "C" MIPS_PRX_PrepareStatus mips_prx_prepare_elf(
    const MIPS_PRX_File *file, uint8_t **elf_data, size_t *elf_size) {
    if (file == nullptr || elf_data == nullptr || elf_size == nullptr ||
        file->file_data == nullptr || file->payload_offset > file->file_size ||
        file->payload_size > file->file_size - file->payload_offset) {
        return MIPS_PRX_PREPARE_INVALID_ARGUMENT;
    }
    *elf_data = nullptr;
    *elf_size = 0;

    const uint8_t *payload = file->file_data + file->payload_offset;
    if (file->container == MIPS_PRX_CONTAINER_ELF) {
        uint8_t *copy = (uint8_t *)std::malloc(file->payload_size);
        if (copy == nullptr) return MIPS_PRX_PREPARE_OUT_OF_MEMORY;
        std::memcpy(copy, payload, file->payload_size);
        if (!valid_mips_elf(copy, file->payload_size)) {
            std::free(copy);
            return MIPS_PRX_PREPARE_INVALID_ELF;
        }
        *elf_data = copy;
        *elf_size = file->payload_size;
        return MIPS_PRX_PREPARE_OK;
    }

    if (!file->has_psp_header || file->psp_size < 0x150 ||
        file->psp_size > file->payload_size ||
        file->elf_size == 0 || file->elf_size > MIPS_PRX_MAX_FILE_SIZE) {
        return MIPS_PRX_PREPARE_INVALID_ARGUMENT;
    }

    // PPSSPP supports multiple compression codecs; these game PRXs use gzip.
    if ((file->compression_attributes & 0x0001) == 0) {
        return MIPS_PRX_PREPARE_UNSUPPORTED_COMPRESSION;
    }

    std::vector<uint8_t> decrypted(
        std::max((size_t)file->psp_size, (size_t)file->elf_size));
    const int decrypted_size =
        pspDecryptPRX(payload, decrypted.data(), file->psp_size, nullptr);
    if (decrypted_size <= 0 || (size_t)decrypted_size > decrypted.size()) {
        return MIPS_PRX_PREPARE_DECRYPT_FAILED;
    }

    z_stream stream{};
    stream.next_in = decrypted.data();
    stream.avail_in = (uInt)decrypted_size;
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        return MIPS_PRX_PREPARE_DECOMPRESSION_FAILED;
    }

    std::vector<uint8_t> output;
    output.reserve(file->elf_size);
    uint8_t chunk[16384];
    int zstatus = Z_OK;
    while (zstatus == Z_OK) {
        stream.next_out = chunk;
        stream.avail_out = sizeof(chunk);
        zstatus = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = sizeof(chunk) - stream.avail_out;
        if (produced > MIPS_PRX_MAX_FILE_SIZE - output.size()) {
            inflateEnd(&stream);
            return MIPS_PRX_PREPARE_DECOMPRESSION_FAILED;
        }
        output.insert(output.end(), chunk, chunk + produced);
    }
    inflateEnd(&stream);
    if (zstatus != Z_STREAM_END || !valid_mips_elf(output.data(), output.size())) {
        return zstatus == Z_STREAM_END
            ? MIPS_PRX_PREPARE_INVALID_ELF
            : MIPS_PRX_PREPARE_DECOMPRESSION_FAILED;
    }

    uint8_t *copy = (uint8_t *)std::malloc(output.size());
    if (copy == nullptr) return MIPS_PRX_PREPARE_OUT_OF_MEMORY;
    std::memcpy(copy, output.data(), output.size());
    *elf_data = copy;
    *elf_size = output.size();
    return MIPS_PRX_PREPARE_OK;
}

extern "C" const char *mips_prx_prepare_status_name(
    MIPS_PRX_PrepareStatus status) {
    switch (status) {
        case MIPS_PRX_PREPARE_OK: return "ok";
        case MIPS_PRX_PREPARE_INVALID_ARGUMENT: return "invalid_argument";
        case MIPS_PRX_PREPARE_DECRYPT_FAILED: return "decrypt_failed";
        case MIPS_PRX_PREPARE_UNSUPPORTED_COMPRESSION: return "unsupported_compression";
        case MIPS_PRX_PREPARE_DECOMPRESSION_FAILED: return "decompression_failed";
        case MIPS_PRX_PREPARE_INVALID_ELF: return "invalid_elf";
        case MIPS_PRX_PREPARE_OUT_OF_MEMORY: return "out_of_memory";
    }
    return "unknown_status";
}
