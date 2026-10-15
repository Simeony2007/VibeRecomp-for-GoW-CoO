#ifndef MIPS_VIDEO_PIXEL_H
#define MIPS_VIDEO_PIXEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int mips_video_decode_display_pixel(const uint8_t *src,
                                                  uint32_t format,
                                                  uint8_t rgba[4]) {
    if (src == NULL || rgba == NULL) return 0;
    if (format == 3) {
        memcpy(rgba, src, 4);
        return 1;
    }
    if (format > 2) return 0;

    uint16_t pixel;
    memcpy(&pixel, src, sizeof(pixel));
    if (format == 0) {
        rgba[0] = (uint8_t)(((pixel & 0x1F) * 255) / 31);
        rgba[1] = (uint8_t)((((pixel >> 5) & 0x3F) * 255) / 63);
        rgba[2] = (uint8_t)((((pixel >> 11) & 0x1F) * 255) / 31);
        rgba[3] = 255;
    } else if (format == 1) {
        rgba[0] = (uint8_t)(((pixel & 0x1F) * 255) / 31);
        rgba[1] = (uint8_t)((((pixel >> 5) & 0x1F) * 255) / 31);
        rgba[2] = (uint8_t)((((pixel >> 10) & 0x1F) * 255) / 31);
        rgba[3] = (pixel & 0x8000) ? 255 : 0;
    } else {
        rgba[0] = (uint8_t)((pixel & 0xF) * 17);
        rgba[1] = (uint8_t)(((pixel >> 4) & 0xF) * 17);
        rgba[2] = (uint8_t)(((pixel >> 8) & 0xF) * 17);
        rgba[3] = (uint8_t)(((pixel >> 12) & 0xF) * 17);
    }
    return 1;
}

#endif
