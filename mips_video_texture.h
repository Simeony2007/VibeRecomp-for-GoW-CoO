#ifndef MIPS_VIDEO_TEXTURE_H
#define MIPS_VIDEO_TEXTURE_H

#include "mips_video_pixel.h"

static inline int mips_video_texture_byte_offset(uint32_t x, uint32_t y,
                                                 uint32_t buffer_width,
                                                 uint32_t bytes_per_pixel,
                                                 int swizzled,
                                                 size_t *offset_out) {
    if (offset_out == NULL || bytes_per_pixel == 0 || buffer_width == 0 ||
        x >= buffer_width) {
        return 0;
    }
    if (!swizzled) {
        if ((size_t)y > SIZE_MAX / buffer_width) return 0;
        size_t pixel_index = (size_t)y * buffer_width + x;
        if (pixel_index < x) return 0;
        if (pixel_index > SIZE_MAX / bytes_per_pixel) return 0;
        *offset_out = pixel_index * bytes_per_pixel;
        return 1;
    }

    if ((size_t)buffer_width > (SIZE_MAX - 15) / bytes_per_pixel) return 0;
    size_t row_blocks =
        ((size_t)buffer_width * bytes_per_pixel + 15) / 16;
    if ((size_t)(y / 8) > SIZE_MAX / row_blocks) return 0;
    size_t block_row = (size_t)(y / 8) * row_blocks;
    size_t block_column = ((size_t)x * bytes_per_pixel) / 16;
    size_t row_in_block = y & 7;
    size_t byte_in_block = ((size_t)x * bytes_per_pixel) & 15;
    if (block_row > SIZE_MAX - block_column) return 0;
    size_t block = block_row + block_column;
    if (block > SIZE_MAX / 128) return 0;
    size_t offset = block * 128;
    size_t within_block = row_in_block * 16 + byte_in_block;
    if (offset > SIZE_MAX - within_block) return 0;
    *offset_out = offset + within_block;
    return 1;
}

#endif
