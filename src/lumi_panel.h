#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int lumi_panel_init(void);
int lumi_panel_set_sleep(bool sleeping);

/* Prepare ST7789 for external VCC removal before nRF52840 System OFF. */
int lumi_panel_enter_deep_sleep(void);

int lumi_panel_set_backlight(bool enabled);

/* Direct ST7789 RGB444 streaming used by RYNOR's preprocessed P16 GIF path.
 * begin_frame() selects 12-bit color and opens one full 320x172 GRAM window.
 * write_async() uses SPIM3 EasyDMA; the caller may prepare/read the next
 * stripe while the current one is on the wire.
 */
int lumi_panel_rgb444_begin_frame(void);
int lumi_panel_rgb444_begin_rect(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height);
int lumi_panel_rgb444_write_async(const uint8_t *data, size_t len);
int lumi_panel_rgb444_wait(void);
int lumi_panel_rgb444_end_frame(void);
