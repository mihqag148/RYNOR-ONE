/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool lumi_raw_gif_validate(
    uint32_t data_size,
    uint8_t scale_mode,
    uint16_t *source_width,
    uint16_t *source_height);

void lumi_raw_gif_configure(
    uint32_t data_size,
    uint8_t scale_mode);

void lumi_raw_gif_reset_playback(void);
void lumi_raw_gif_stop(void);
bool lumi_raw_gif_render_due(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
