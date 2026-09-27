/* SPDX-License-Identifier: MIT */

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/storage/flash_map.h>

#include <lvgl.h>

#include "AnimatedGIF.h"
#include "lumi_diag.h"
#include "lumi_raw_gif.h"

namespace {

constexpr uint32_t kGifDataOffset = 0x1000U;
constexpr int kDisplayWidth = 320;
constexpr int kDisplayHeight = 172;
constexpr uint32_t kMinFrameDelayMs = 10U;

AnimatedGIF g_gif;
const struct flash_area *g_area = nullptr;
const struct device *const g_display =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

uint32_t g_data_size;
uint8_t g_scale_mode;
bool g_opened;
bool g_started;
uint32_t g_next_frame_at;
int g_source_width;
int g_source_height;
int g_content_width;
int g_content_height;
int g_offset_x;
int g_offset_y;

lv_color_t g_line[kDisplayWidth];
uint8_t g_mask[kDisplayWidth];

void close_area() {
    if (g_area) {
        flash_area_close(g_area);
        g_area = nullptr;
    }
}

void *gif_open(
    const char *filename,
    int32_t *file_size) {

    (void)filename;

    if (!file_size ||
        g_data_size == 0U ||
        flash_area_open(
            FIXED_PARTITION_ID(lumi_saver_partition),
            &g_area) != 0 ||
        !g_area ||
        !device_is_ready(g_area->fa_dev) ||
        (uint64_t)kGifDataOffset + g_data_size > g_area->fa_size) {
        close_area();
        return nullptr;
    }

    *file_size = static_cast<int32_t>(g_data_size);
    return const_cast<struct flash_area *>(g_area);
}

void gif_close(void *handle) {
    (void)handle;
    close_area();
}

int32_t gif_read(
    GIFFILE *file,
    uint8_t *buffer,
    int32_t length) {

    if (!file ||
        !buffer ||
        length <= 0 ||
        !g_area) {
        return 0;
    }

    int32_t remaining =
        file->iSize - file->iPos;

    int32_t count =
        std::min(length, remaining);

    if (count <= 0) {
        return 0;
    }

    int rc = flash_area_read(
        g_area,
        kGifDataOffset +
            static_cast<uint32_t>(file->iPos),
        buffer,
        static_cast<size_t>(count));

    if (rc != 0) {
        return 0;
    }

    file->iPos += count;
    return count;
}

int32_t gif_seek(
    GIFFILE *file,
    int32_t position) {

    if (!file) {
        return 0;
    }

    if (position < 0) {
        position = 0;
    }

    if (position > file->iSize) {
        position = file->iSize;
    }

    file->iPos = position;
    return position;
}

void compute_transform() {
    g_content_width = kDisplayWidth;
    g_content_height = kDisplayHeight;
    g_offset_x = 0;
    g_offset_y = 0;

    if (g_source_width <= 0 ||
        g_source_height <= 0) {
        return;
    }

    /* ScreensaverScaleMode:
     * 0 Fill, 1 Fit, 2 Stretch, 3 Tile, 4 Center, 5 Span.
     * Raw-GIF playback keeps the useful Windows-like semantics while avoiding
     * a framebuffer: Fill/Span crop, Stretch distorts, and Fit/Tile/Center
     * preserve aspect and letterbox.
     */
    if (g_scale_mode == 2U) {
        return;
    }

    bool fill =
        g_scale_mode == 0U ||
        g_scale_mode == 5U;

    int64_t source_vs_target =
        static_cast<int64_t>(g_source_width) *
        kDisplayHeight;

    int64_t target_vs_source =
        static_cast<int64_t>(g_source_height) *
        kDisplayWidth;

    if (fill) {
        if (source_vs_target >= target_vs_source) {
            g_content_height = kDisplayHeight;
            g_content_width =
                std::max(
                    1,
                    static_cast<int>(
                        (static_cast<int64_t>(g_source_width) *
                         kDisplayHeight) /
                        g_source_height));
        } else {
            g_content_width = kDisplayWidth;
            g_content_height =
                std::max(
                    1,
                    static_cast<int>(
                        (static_cast<int64_t>(g_source_height) *
                         kDisplayWidth) /
                        g_source_width));
        }
    } else {
        if (source_vs_target >= target_vs_source) {
            g_content_width = kDisplayWidth;
            g_content_height =
                std::max(
                    1,
                    static_cast<int>(
                        (static_cast<int64_t>(g_source_height) *
                         kDisplayWidth) /
                        g_source_width));
        } else {
            g_content_height = kDisplayHeight;
            g_content_width =
                std::max(
                    1,
                    static_cast<int>(
                        (static_cast<int64_t>(g_source_width) *
                         kDisplayHeight) /
                        g_source_height));
        }
    }

    g_offset_x =
        (kDisplayWidth - g_content_width) / 2;
    g_offset_y =
        (kDisplayHeight - g_content_height) / 2;
}

void clear_display() {
    if (!device_is_ready(g_display)) {
        return;
    }

    lv_color_t black =
        lv_color_make(0, 0, 0);

    for (int x = 0; x < kDisplayWidth; ++x) {
        g_line[x] = black;
    }

    struct display_buffer_descriptor desc = {};
    desc.buf_size =
        static_cast<size_t>(kDisplayWidth) *
        sizeof(lv_color_t);
    desc.width = kDisplayWidth;
    desc.height = 1U;
    desc.pitch = kDisplayWidth;

    for (int y = 0; y < kDisplayHeight; ++y) {
        (void)display_write(
            g_display,
            0,
            y,
            &desc,
            g_line);
    }
}

void gif_draw(GIFDRAW *draw) {
    if (!draw ||
        !draw->pPixels ||
        !draw->pPalette ||
        !device_is_ready(g_display) ||
        g_source_width <= 0 ||
        g_source_height <= 0) {
        return;
    }

    int source_y =
        draw->iY + draw->y;

    int dy0 =
        g_offset_y +
        static_cast<int>(
            (static_cast<int64_t>(source_y) *
             g_content_height) /
            g_source_height);

    int dy1 =
        g_offset_y +
        static_cast<int>(
            (static_cast<int64_t>(source_y + 1) *
             g_content_height) /
            g_source_height);

    dy0 = std::max(0, dy0);
    dy1 = std::min(kDisplayHeight, dy1);

    if (dy1 <= dy0) {
        return;
    }

    std::memset(g_mask, 0, sizeof(g_mask));

    const uint8_t *pixels =
        draw->pPixels;
    const uint16_t *palette =
        draw->pPalette;

    bool has_transparency =
        draw->ucHasTransparency != 0U;

    for (int local_x = 0;
         local_x < draw->iWidth;
         ++local_x) {

        int source_x =
            draw->iX + local_x;

        int dx0 =
            g_offset_x +
            static_cast<int>(
                (static_cast<int64_t>(source_x) *
                 g_content_width) /
                g_source_width);

        int dx1 =
            g_offset_x +
            static_cast<int>(
                (static_cast<int64_t>(source_x + 1) *
                 g_content_width) /
                g_source_width);

        dx0 = std::max(0, dx0);
        dx1 = std::min(kDisplayWidth, dx1);

        if (dx1 <= dx0) {
            continue;
        }

        uint8_t index =
            pixels[local_x];

        bool transparent =
            has_transparency &&
            index == draw->ucTransparent;

        if (transparent &&
            draw->ucDisposalMethod != 2U) {
            continue;
        }

        if (transparent) {
            index = draw->ucBackground;
        }

        lv_color_t color;
        color.full = palette[index];

        for (int x = dx0; x < dx1; ++x) {
            g_line[x] = color;
            g_mask[x] = 1U;
        }
    }

    for (int y = dy0; y < dy1; ++y) {
        int x = 0;

        while (x < kDisplayWidth) {
            while (x < kDisplayWidth &&
                   g_mask[x] == 0U) {
                ++x;
            }

            if (x >= kDisplayWidth) {
                break;
            }

            int start = x;

            while (x < kDisplayWidth &&
                   g_mask[x] != 0U) {
                ++x;
            }

            int width = x - start;

            struct display_buffer_descriptor desc = {};
            desc.buf_size =
                static_cast<size_t>(width) *
                sizeof(lv_color_t);
            desc.width =
                static_cast<uint16_t>(width);
            desc.height = 1U;
            desc.pitch =
                static_cast<uint16_t>(width);

            (void)display_write(
                g_display,
                static_cast<uint16_t>(start),
                static_cast<uint16_t>(y),
                &desc,
                &g_line[start]);
        }
    }
}

bool open_decoder() {
    if (g_data_size == 0U) {
        return false;
    }

    if (g_opened) {
        g_gif.close();
        g_opened = false;
    }

    g_gif.begin(GIF_PALETTE_RGB565_LE);

    int rc =
        g_gif.open(
            "R:gif",
            gif_open,
            gif_close,
            gif_read,
            gif_seek,
            gif_draw);

    if (rc == 0) {
        close_area();
        return false;
    }

    g_source_width =
        g_gif.getCanvasWidth();
    g_source_height =
        g_gif.getCanvasHeight();

    if (g_source_width < 1 ||
        g_source_height < 1 ||
        g_source_width > 2048 ||
        g_source_height > 2048) {
        g_gif.close();
        g_opened = false;
        return false;
    }

    compute_transform();
    g_opened = true;
    return true;
}

} // namespace

extern "C" bool lumi_raw_gif_validate(
    uint32_t data_size,
    uint8_t scale_mode,
    uint16_t *source_width,
    uint16_t *source_height) {

    lumi_raw_gif_stop();

    g_data_size = data_size;
    g_scale_mode = scale_mode;

    if (!open_decoder()) {
        return false;
    }

    if (source_width) {
        *source_width =
            static_cast<uint16_t>(g_source_width);
    }

    if (source_height) {
        *source_height =
            static_cast<uint16_t>(g_source_height);
    }

    g_gif.close();
    g_opened = false;
    g_started = false;
    return true;
}

extern "C" void lumi_raw_gif_configure(
    uint32_t data_size,
    uint8_t scale_mode) {

    lumi_raw_gif_stop();
    g_data_size = data_size;
    g_scale_mode = scale_mode;
}

extern "C" void lumi_raw_gif_reset_playback(void) {
    lumi_raw_gif_stop();
    g_next_frame_at = 0U;
}

extern "C" void lumi_raw_gif_stop(void) {
    if (g_opened) {
        g_gif.close();
    } else {
        close_area();
    }

    g_opened = false;
    g_started = false;
}

extern "C" bool lumi_raw_gif_render_due(
    uint32_t now_ms) {

    if (!g_opened) {
        if (!open_decoder()) {
            lumi_diag_report(
                'E',
                "Raw GIF open failed");
            return false;
        }

        clear_display();
        g_next_frame_at = now_ms;
        g_started = true;
    }

    if ((int32_t)(now_ms - g_next_frame_at) < 0) {
        return true;
    }

    int delay_ms = 0;

    int more =
        g_gif.playFrame(
            false,
            &delay_ms,
            nullptr);

    delay_ms =
        std::max(
            static_cast<int>(kMinFrameDelayMs),
            delay_ms);

    g_next_frame_at =
        now_ms +
        static_cast<uint32_t>(delay_ms);

    if (more == 0) {
        int error =
            g_gif.getLastError();

        if (error != GIF_SUCCESS &&
            error != GIF_EMPTY_FRAME) {
            lumi_diag_report(
                'E',
                "Raw GIF decode error=%d",
                error);
            lumi_raw_gif_stop();
            return false;
        }

        g_gif.reset();
    }

    return true;
}
