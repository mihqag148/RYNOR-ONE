/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_SPI)
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#endif
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include "AnimatedGIF.h"
#include "lumi_diag.h"
#include "lumi_raw_gif.h"

namespace {

constexpr uint32_t kGifDataOffset = 0x1000U;
constexpr int kDisplayWidth = 320;
constexpr int kDisplayHeight = 172;
constexpr int kStripeRows = 8;
constexpr int kRgb444StripeRows = 16;
constexpr uint32_t kMinFrameDelayMs = 10U;
/*
 * RGB444 full-screen transfer at 32 MHz is ~20.6 ms before decode overhead.
 * Cap presentation near 30 FPS instead of forcing the LCD itself to 25 Hz.
 */
constexpr uint32_t kPresentIntervalMs = 33U;
constexpr size_t kRgb444BytesPerRow =
    static_cast<size_t>(kDisplayWidth) * 3U / 2U;

/* ST7789 command subset used only by the raw-GIF fast path. */
constexpr uint8_t kCmdCaseT = 0x2AU;
constexpr uint8_t kCmdRaseT = 0x2BU;
constexpr uint8_t kCmdRamWr = 0x2CU;
constexpr uint8_t kCmdColMod = 0x3AU;
constexpr uint8_t kRgb444ColMod = 0x03U;
constexpr uint8_t kRgb565ColMod = 0x05U;

/* A requested slave frequency does not override an SPIM instance's limit. */
BUILD_ASSERT(DT_PROP(DT_BUS(DT_CHOSEN(zephyr_display)), max_frequency) >=
             DT_PROP(DT_CHOSEN(zephyr_display), spi_max_frequency),
             "RYNOR TFT bus cannot supply the requested SPI clock");

AnimatedGIF g_gif;
const struct flash_area *g_area = nullptr;
const struct device *const g_display =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

uint32_t g_data_size;
uint8_t g_scale_mode;
bool g_opened;
bool g_started;
uint32_t g_next_frame_at;
uint32_t g_next_present_at;
bool g_suppress_opaque_output;
bool g_frame_forced_present;
bool g_fast_panel_mode;
bool g_fast_path_allowed;
bool g_rgb444_stream_active;
bool g_rgb444_frame_sequential;
int g_rgb444_last_input_y;
int g_source_width;
int g_source_height;
int g_content_width;
int g_content_height;
int g_offset_x;
int g_offset_y;

lv_color_t g_line[kDisplayWidth];
uint8_t g_mask[kDisplayWidth];

/* AnimatedGIF reads GIF sub-block lengths one byte at a time. Without this
 * cache, each length/data read wakes and sleeps SPI NOR and stalls the next
 * LCD stripe. Cache source bytes, not decoded colors: palettes, transparency
 * and source resolution remain unchanged. Never read past the uploaded GIF.
 */
uint8_t g_read_cache[2048];
int32_t g_cache_start;
int32_t g_cache_size;

/*
 * Native GIF fast path.
 *
 * The old renderer called display_write() for almost every LCD row. A
 * 320x172 full-frame GIF therefore needed ~172 ST7789 transactions per frame,
 * making transfers slow. Stripes do NOT synchronize to the panel scan. Keep an
 * 8-row stripe (5 KiB) in RAM and flush full-frame opaque GIFs in blocks.
 * This avoids a 110 KiB framebuffer, which the nRF52840 cannot afford.
 */
lv_color_t g_stripe[kDisplayWidth * kStripeRows];
int g_stripe_start_y = -1;
int g_stripe_rows = 0;

uint8_t g_rgb444_stripes[2][kRgb444BytesPerRow * kRgb444StripeRows];
int g_rgb444_fill_index = 0;
int g_rgb444_start_y = -1;
int g_rgb444_rows = 0;
bool g_rgb444_tx_active = false;
int g_rgb444_tx_result = 0;
bool g_rgb444_tx_sync_ready = false;
struct k_sem g_rgb444_tx_done;

#if defined(CONFIG_SPI_ASYNC)
struct spi_buf g_rgb444_async_buf = {};
struct spi_buf_set g_rgb444_async_set = {};
#endif

void flush_stripe();

#if defined(CONFIG_SPI)
const struct spi_dt_spec g_panel_spi =
    SPI_DT_SPEC_GET(
        DT_CHOSEN(zephyr_display),
        SPI_OP_MODE_MASTER | SPI_WORD_SET(8),
        0);
const struct gpio_dt_spec g_panel_dc =
    GPIO_DT_SPEC_GET(
        DT_CHOSEN(zephyr_display),
        cmd_data_gpios);

constexpr uint16_t kPanelXOffset =
    DT_PROP(DT_CHOSEN(zephyr_display), x_offset);
constexpr uint16_t kPanelYOffset =
    DT_PROP(DT_CHOSEN(zephyr_display), y_offset);

bool panel_command(
    uint8_t command,
    const uint8_t *data = nullptr,
    size_t length = 0U) {

    if (!spi_is_ready_dt(&g_panel_spi) ||
        !gpio_is_ready_dt(&g_panel_dc)) {
        return false;
    }

    struct spi_buf command_buffer = {};
    command_buffer.buf = &command;
    command_buffer.len = 1U;

    struct spi_buf_set command_set = {};
    command_set.buffers = &command_buffer;
    command_set.count = 1U;

    /* cmd-data-gpios is active-low: logical 1 drives D/C low (command). */
    if (gpio_pin_set_dt(&g_panel_dc, 1) != 0 ||
        spi_write_dt(&g_panel_spi, &command_set) != 0) {
        return false;
    }

    if (!data || length == 0U) {
        return true;
    }

    struct spi_buf data_buffer = {};
    data_buffer.buf =
        const_cast<uint8_t *>(data);
    data_buffer.len = length;

    struct spi_buf_set data_set = {};
    data_set.buffers = &data_buffer;
    data_set.count = 1U;

    /* logical 0 drives D/C high (pixel/parameter data). */
    return gpio_pin_set_dt(&g_panel_dc, 0) == 0 &&
           spi_write_dt(&g_panel_spi, &data_set) == 0;
}

bool panel_data(
    const uint8_t *data,
    size_t length);

void ensure_rgb444_tx_sync() {
    if (!g_rgb444_tx_sync_ready) {
        k_sem_init(
            &g_rgb444_tx_done,
            0,
            1);
        g_rgb444_tx_sync_ready = true;
    }
}

#if defined(CONFIG_SPI_ASYNC)
void panel_async_done(
    const struct device *dev,
    int result,
    void *userdata) {

    (void)dev;
    (void)userdata;

    g_rgb444_tx_result = result;
    k_sem_give(&g_rgb444_tx_done);
}
#endif

bool wait_rgb444_tx() {
    if (!g_rgb444_tx_active) {
        return true;
    }

    ensure_rgb444_tx_sync();

    if (k_sem_take(
            &g_rgb444_tx_done,
            K_FOREVER) != 0) {
        return false;
    }

    g_rgb444_tx_active = false;
    return g_rgb444_tx_result == 0;
}

bool panel_data_async(
    const uint8_t *data,
    size_t length) {

#if defined(CONFIG_SPI_ASYNC)
    if (!data ||
        length == 0U ||
        !spi_is_ready_dt(&g_panel_spi) ||
        !gpio_is_ready_dt(&g_panel_dc)) {
        return false;
    }

    if (!wait_rgb444_tx()) {
        return false;
    }

    ensure_rgb444_tx_sync();
    k_sem_reset(&g_rgb444_tx_done);

    if (gpio_pin_set_dt(&g_panel_dc, 0) != 0) {
        return false;
    }

    g_rgb444_async_buf.buf =
        const_cast<uint8_t *>(data);
    g_rgb444_async_buf.len = length;
    g_rgb444_async_set.buffers =
        &g_rgb444_async_buf;
    g_rgb444_async_set.count = 1U;

    g_rgb444_tx_result = 0;

    int rc =
        spi_transceive_cb(
            g_panel_spi.bus,
            &g_panel_spi.config,
            &g_rgb444_async_set,
            nullptr,
            panel_async_done,
            nullptr);

    if (rc != 0) {
        return false;
    }

    g_rgb444_tx_active = true;
    return true;
#else
    return panel_data(data, length);
#endif
}

bool panel_data(
    const uint8_t *data,
    size_t length) {

    if (!data ||
        length == 0U ||
        !spi_is_ready_dt(&g_panel_spi) ||
        !gpio_is_ready_dt(&g_panel_dc)) {
        return false;
    }

    struct spi_buf data_buffer = {};
    data_buffer.buf =
        const_cast<uint8_t *>(data);
    data_buffer.len = length;

    struct spi_buf_set data_set = {};
    data_set.buffers = &data_buffer;
    data_set.count = 1U;

    /* logical 0 drives D/C high for pixel data. */
    return gpio_pin_set_dt(&g_panel_dc, 0) == 0 &&
           spi_write_dt(&g_panel_spi, &data_set) == 0;
}

bool panel_set_window(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height) {

    if (width == 0U || height == 0U) {
        return false;
    }

    uint16_t x0 = x + kPanelXOffset;
    uint16_t y0 = y + kPanelYOffset;
    uint16_t x1 = x0 + width - 1U;
    uint16_t y1 = y0 + height - 1U;

    uint8_t columns[4] = {
        static_cast<uint8_t>(x0 >> 8),
        static_cast<uint8_t>(x0),
        static_cast<uint8_t>(x1 >> 8),
        static_cast<uint8_t>(x1),
    };
    uint8_t rows[4] = {
        static_cast<uint8_t>(y0 >> 8),
        static_cast<uint8_t>(y0),
        static_cast<uint8_t>(y1 >> 8),
        static_cast<uint8_t>(y1),
    };

    return panel_command(kCmdCaseT, columns, sizeof(columns)) &&
           panel_command(kCmdRaseT, rows, sizeof(rows));
}

bool panel_fast_path_available() {
    return spi_is_ready_dt(&g_panel_spi) &&
           gpio_is_ready_dt(&g_panel_dc);
}
#else
bool wait_rgb444_tx() {
    return true;
}

bool panel_fast_path_available() {
    return false;
}
#endif

void close_area() {
    g_cache_size = 0;
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
        MIN(length, remaining);

    if (count <= 0) {
        return 0;
    }

    int32_t copied = 0;
    while (copied < count) {
        int32_t offset = file->iPos - g_cache_start;
        if (g_cache_size == 0 || offset < 0 || offset >= g_cache_size) {
            g_cache_size = 0;
            g_cache_start = file->iPos;
            int32_t refill = MIN(static_cast<int32_t>(sizeof(g_read_cache)),
                                 file->iSize - file->iPos);
            if (flash_area_read(g_area,
                    kGifDataOffset + static_cast<uint32_t>(file->iPos),
                    g_read_cache, static_cast<size_t>(refill)) != 0) {
                return copied;
            }
            g_cache_size = refill;
            offset = 0;
        }
        int32_t chunk = MIN(count - copied, g_cache_size - offset);
        memcpy(buffer + copied, g_read_cache + offset,
               static_cast<size_t>(chunk));
        file->iPos += chunk;
        copied += chunk;
    }
    return copied;
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
                MAX(
                    1,
                    static_cast<int>(
                        (static_cast<int64_t>(g_source_width) *
                         kDisplayHeight) /
                        g_source_height));
        } else {
            g_content_width = kDisplayWidth;
            g_content_height =
                MAX(
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
                MAX(
                    1,
                    static_cast<int>(
                        (static_cast<int64_t>(g_source_height) *
                         kDisplayWidth) /
                        g_source_width));
        } else {
            g_content_height = kDisplayHeight;
            g_content_width =
                MAX(
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


uint16_t palette_be_to_rgb565(
    uint16_t value) {
    return static_cast<uint16_t>(
        (value >> 8) |
        (value << 8));
}

void flush_rgb444_stripe() {
    if (g_rgb444_rows <= 0 ||
        g_rgb444_start_y < 0) {
        g_rgb444_start_y = -1;
        g_rgb444_rows = 0;
        return;
    }

#if defined(CONFIG_SPI)
    bool ok = false;

    if (g_fast_panel_mode &&
        g_rgb444_frame_sequential) {

        if (!g_rgb444_stream_active) {
            /*
             * Program the full remaining frame once, then keep feeding pixel
             * data. ST7789 GRAM continues across CS toggles, so subsequent
             * stripes avoid CASET/RASET/RAMWR command overhead.
             */
            ok =
                panel_set_window(
                    0U,
                    static_cast<uint16_t>(g_rgb444_start_y),
                    kDisplayWidth,
                    static_cast<uint16_t>(
                        kDisplayHeight -
                        g_rgb444_start_y)) &&
                panel_command(
                    kCmdRamWr,
                    nullptr,
                    0U);

            g_rgb444_stream_active = ok;
        } else {
            ok = true;
        }

        if (ok) {
            ok = panel_data_async(
                g_rgb444_stripes[g_rgb444_fill_index],
                kRgb444BytesPerRow *
                    static_cast<size_t>(g_rgb444_rows));
        }
    } else if (g_fast_panel_mode) {
        ok =
            panel_set_window(
                0U,
                static_cast<uint16_t>(g_rgb444_start_y),
                kDisplayWidth,
                static_cast<uint16_t>(g_rgb444_rows)) &&
            panel_command(
                kCmdRamWr,
                g_rgb444_stripes[g_rgb444_fill_index],
                kRgb444BytesPerRow *
                    static_cast<size_t>(g_rgb444_rows));
    }

    if (g_fast_panel_mode && !ok) {
        lumi_diag_report(
            'E',
            "Raw GIF RGB444 SPI write failed");
    }
#endif

    if (g_rgb444_tx_active) {
        g_rgb444_fill_index ^= 1;
    }

    g_rgb444_start_y = -1;
    g_rgb444_rows = 0;
}

void leave_fast_panel_mode() {
    flush_rgb444_stripe();
    (void)wait_rgb444_tx();

    if (!g_fast_panel_mode) {
        return;
    }

#if defined(CONFIG_SPI)
    /*
     * Pixel format changes are hidden once when leaving the screensaver.
     * The LCD refresh rate/porches are never modified by GIF playback.
     */
    (void)display_blanking_on(g_display);
    (void)panel_command(
        kCmdColMod,
        &kRgb565ColMod,
        1U);
    (void)display_blanking_off(g_display);
#endif

    g_fast_panel_mode = false;
    g_rgb444_stream_active = false;
}

bool enter_fast_panel_mode() {
    if (g_fast_panel_mode) {
        return true;
    }

    if (!g_fast_path_allowed ||
        !panel_fast_path_available()) {
        return false;
    }

    flush_stripe();

#if defined(CONFIG_SPI)
    /*
     * Keep the ST7789 at its normal refresh timing. v1.14.40 attempted to
     * retime the panel itself and made the visible top-to-bottom transition
     * much slower on the real 1.47-inch module.
     */
    (void)display_blanking_on(g_display);
    bool ok =
        panel_command(
            kCmdColMod,
            &kRgb444ColMod,
            1U);
    (void)display_blanking_off(g_display);

    if (!ok) {
        g_fast_path_allowed = false;
        return false;
    }

    g_fast_panel_mode = true;
    g_rgb444_stream_active = false;
    return true;
#else
    return false;
#endif
}

void append_rgb444_row(
    int y,
    const uint8_t *pixels,
    const uint16_t *palette) {

    if (!pixels ||
        !palette ||
        y < 0 ||
        y >= kDisplayHeight ||
        !g_fast_panel_mode) {
        return;
    }

    if (g_rgb444_last_input_y >= 0 &&
        y != g_rgb444_last_input_y + 1) {
        g_rgb444_frame_sequential = false;
        flush_rgb444_stripe();
        (void)wait_rgb444_tx();
        g_rgb444_stream_active = false;
    }

    g_rgb444_last_input_y = y;

    if (g_rgb444_rows <= 0) {
        g_rgb444_start_y = y;
    }

    int expected_y =
        g_rgb444_start_y +
        g_rgb444_rows;

    if (y != expected_y ||
        g_rgb444_rows >= kRgb444StripeRows) {
        flush_rgb444_stripe();
        g_rgb444_start_y = y;
    }

    uint8_t *out =
        &g_rgb444_stripes[g_rgb444_fill_index][
            static_cast<size_t>(g_rgb444_rows) *
            kRgb444BytesPerRow];

    for (int x = 0;
         x < kDisplayWidth;
         x += 2) {

        uint16_t c0 =
            palette_be_to_rgb565(
                palette[pixels[x]]);
        uint16_t c1 =
            palette_be_to_rgb565(
                palette[pixels[x + 1]]);

        uint8_t r0 =
            static_cast<uint8_t>(
                (c0 >> 12) & 0x0FU);
        uint8_t g0 =
            static_cast<uint8_t>(
                (c0 >> 7) & 0x0FU);
        uint8_t b0 =
            static_cast<uint8_t>(
                (c0 >> 1) & 0x0FU);

        uint8_t r1 =
            static_cast<uint8_t>(
                (c1 >> 12) & 0x0FU);
        uint8_t g1 =
            static_cast<uint8_t>(
                (c1 >> 7) & 0x0FU);
        uint8_t b1 =
            static_cast<uint8_t>(
                (c1 >> 1) & 0x0FU);

        size_t offset =
            static_cast<size_t>(x / 2) *
            3U;

        out[offset] =
            static_cast<uint8_t>(
                (r0 << 4) |
                g0);
        out[offset + 1U] =
            static_cast<uint8_t>(
                (b0 << 4) |
                r1);
        out[offset + 2U] =
            static_cast<uint8_t>(
                (g1 << 4) |
                b1);
    }

    g_rgb444_rows++;

    if (g_rgb444_rows >= kRgb444StripeRows) {
        flush_rgb444_stripe();
    }
}

void flush_stripe() {
    if (g_stripe_rows <= 0 ||
        g_stripe_start_y < 0 ||
        !device_is_ready(g_display)) {
        g_stripe_start_y = -1;
        g_stripe_rows = 0;
        return;
    }

    struct display_buffer_descriptor desc = {};
    desc.buf_size =
        static_cast<size_t>(kDisplayWidth) *
        static_cast<size_t>(g_stripe_rows) *
        sizeof(lv_color_t);
    desc.width = kDisplayWidth;
    desc.height =
        static_cast<uint16_t>(g_stripe_rows);
    desc.pitch = kDisplayWidth;

    (void)display_write(
        g_display,
        0,
        static_cast<uint16_t>(g_stripe_start_y),
        &desc,
        g_stripe);

    g_stripe_start_y = -1;
    g_stripe_rows = 0;
}

void append_full_width_row(
    int y,
    const lv_color_t *row) {

    if (!row ||
        y < 0 ||
        y >= kDisplayHeight) {
        return;
    }

    if (g_stripe_rows <= 0) {
        g_stripe_start_y = y;
    }

    int expected_y =
        g_stripe_start_y +
        g_stripe_rows;

    if (y != expected_y ||
        g_stripe_rows >= kStripeRows) {
        flush_stripe();
        g_stripe_start_y = y;
    }

    memcpy(
        &g_stripe[
            g_stripe_rows *
            kDisplayWidth],
        row,
        sizeof(lv_color_t) *
            kDisplayWidth);

    g_stripe_rows++;

    if (g_stripe_rows >= kStripeRows) {
        flush_stripe();
    }
}

void clear_display() {
    if (!device_is_ready(g_display)) {
        return;
    }

    flush_stripe();

    lv_color_t black =
        lv_color_make(0, 0, 0);

    for (int i = 0;
         i < kDisplayWidth * kStripeRows;
         ++i) {
        g_stripe[i] = black;
    }

    for (int y = 0;
         y < kDisplayHeight;
         y += kStripeRows) {

        int rows =
            MIN(
                kStripeRows,
                kDisplayHeight - y);

        struct display_buffer_descriptor desc = {};
        desc.buf_size =
            static_cast<size_t>(kDisplayWidth) *
            static_cast<size_t>(rows) *
            sizeof(lv_color_t);
        desc.width = kDisplayWidth;
        desc.height =
            static_cast<uint16_t>(rows);
        desc.pitch = kDisplayWidth;

        (void)display_write(
            g_display,
            0,
            static_cast<uint16_t>(y),
            &desc,
            g_stripe);
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

    dy0 = MAX(0, dy0);
    dy1 = MIN(kDisplayHeight, dy1);

    if (dy1 <= dy0) {
        return;
    }

    memset(g_mask, 0, sizeof(g_mask));

    const uint8_t *pixels =
        draw->pPixels;
    const uint16_t *palette =
        draw->pPalette;

    bool has_transparency =
        draw->ucHasTransparency != 0U;

    bool full_frame_opaque =
        !has_transparency &&
        draw->iX == 0 &&
        draw->iY == 0 &&
        draw->iWidth == g_source_width &&
        draw->iHeight == g_source_height;

    /*
     * Full-frame video GIFs are safe to frame-drop: every frame completely
     * replaces the previous one. Decode them on their original timeline but
     * do not push more than the ST7789/SPIM3 link can physically sustain.
     * Partial/translucent frames are never dropped because they depend on
     * previous LCD contents for composition.
     */
    if (full_frame_opaque &&
        g_suppress_opaque_output) {
        return;
    }

    if (!full_frame_opaque) {
        g_frame_forced_present = true;
    }

    /* Native-size opaque video needs only a palette lookup. The generic
     * scaler did two 64-bit divisions per pixel even for this 1:1 case,
     * inserting CPU work between LCD stripes on a 64 MHz Cortex-M4.
     * Interlaced rows still go through append_full_width_row(), which flushes
     * on a discontinuity; partial/transparent/scaled GIFs retain their path.
     */
    if (full_frame_opaque &&
        g_source_width == kDisplayWidth &&
        g_source_height == kDisplayHeight &&
        g_content_width == kDisplayWidth &&
        g_content_height == kDisplayHeight &&
        enter_fast_panel_mode()) {

        append_rgb444_row(
            source_y,
            pixels,
            palette);
        return;
    }

    /*
     * Any scaled, transparent or partial-frame GIF needs RGB565 run writes
     * so unchanged pixels remain intact. Once an asset needs this path, keep
     * it here for the rest of the playback loop instead of repeatedly
     * switching panel pixel formats.
     */
    if (!full_frame_opaque ||
        g_source_width != kDisplayWidth ||
        g_source_height != kDisplayHeight ||
        g_content_width != kDisplayWidth ||
        g_content_height != kDisplayHeight) {
        g_fast_path_allowed = false;
        leave_fast_panel_mode();
    }

    if (full_frame_opaque) {
        lv_color_t black =
            lv_color_make(0, 0, 0);

        for (int x = 0;
             x < kDisplayWidth;
             ++x) {
            g_line[x] = black;
        }
    } else {
        /*
         * A partial/translucent GIF frame depends on pixels already present
         * on the LCD. Flush any pending opaque stripe before falling back to
         * the precise run writer below.
         */
        flush_stripe();
    }

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

        dx0 = MAX(0, dx0);
        dx1 = MIN(kDisplayWidth, dx1);

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

    if (full_frame_opaque) {
        for (int y = dy0; y < dy1; ++y) {
            append_full_width_row(
                y,
                g_line);
        }

        if (draw->y >=
            draw->iHeight - 1) {
            flush_stripe();
        }
        return;
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

    /*
     * RYNOR ONE builds LVGL with CONFIG_LV_COLOR_16_SWAP=y. display_write()
     * therefore expects the same byte-swapped RGB565 representation used by
     * the rest of the LVGL buffers. AnimatedGIF's BE palette produces exactly
     * that representation on the little-endian nRF52840.
     */
    g_gif.begin(GIF_PALETTE_RGB565_BE);

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
    g_fast_path_allowed = true;
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
    g_next_present_at = 0U;
    g_suppress_opaque_output = false;
    g_frame_forced_present = false;
    g_rgb444_stream_active = false;
    g_rgb444_frame_sequential = true;
    g_rgb444_last_input_y = -1;
}

extern "C" void lumi_raw_gif_stop(void) {
    flush_stripe();
    flush_rgb444_stripe();
    leave_fast_panel_mode();

    if (g_opened) {
        g_gif.close();
    } else {
        close_area();
    }

    g_opened = false;
    g_started = false;
    g_rgb444_stream_active = false;
    g_rgb444_fill_index = 0;
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
        g_next_present_at = now_ms;
        g_suppress_opaque_output = false;
        g_frame_forced_present = false;
        g_started = true;
    }

    if ((int32_t)(now_ms - g_next_frame_at) < 0) {
        return true;
    }

    int delay_ms = 0;

    g_rgb444_stream_active = false;
    g_rgb444_frame_sequential = true;
    g_rgb444_last_input_y = -1;
    g_suppress_opaque_output =
        (int32_t)(now_ms - g_next_present_at) < 0;
    g_frame_forced_present = false;

    int more =
        g_gif.playFrame(
            false,
            &delay_ms,
            nullptr);

    if (more < 0) {
        lumi_diag_report('E', "Raw GIF decode error=%d", g_gif.getLastError());
        g_stripe_rows = 0;
        g_stripe_start_y = -1;
        g_rgb444_rows = 0;
        g_rgb444_start_y = -1;
        (void)wait_rgb444_tx();
        lumi_raw_gif_stop();
        return false;
    }

    /* A frame may end with fewer than eight rows (including interlaced
     * order). Commit that tail before returning to LVGL or resetting GIF.
     */
    flush_stripe();
    flush_rgb444_stripe();
    (void)wait_rgb444_tx();

    if (!g_suppress_opaque_output ||
        g_frame_forced_present) {
        g_next_present_at =
            now_ms +
            kPresentIntervalMs;
    }

    delay_ms =
        MAX(
            static_cast<int>(kMinFrameDelayMs),
            delay_ms);

    /*
     * Advance from the previous presentation deadline, not from the time the
     * decode finished. Otherwise decode/SPI time is added to every GIF delay
     * and a nominal 25/50/100 FPS asset progressively plays much slower.
     */
    g_next_frame_at +=
        static_cast<uint32_t>(delay_ms);

    if ((int32_t)(now_ms - g_next_frame_at) >= 0) {
        g_next_frame_at = now_ms;
    }

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
