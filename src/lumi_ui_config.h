/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum lumi_screensaver_style {
    LUMI_SAVER_TAHOE = 0,
    LUMI_SAVER_MINIMAL = 1,
    LUMI_SAVER_OFF = 2,
};

#define LUMI_SAVER_FRAME_W 160
#define LUMI_SAVER_FRAME_H 86
#define LUMI_SAVER_FRAME_BYTES (LUMI_SAVER_FRAME_W * LUMI_SAVER_FRAME_H)
#define LUMI_SAVER_MAX_FRAMES 25

#define LUMI_SAVER_IMAGE_W 320
#define LUMI_SAVER_IMAGE_H 172
#define LUMI_SAVER_IMAGE_BYTES (LUMI_SAVER_IMAGE_W * LUMI_SAVER_IMAGE_H * 2U)

void lumi_ui_set_wallpaper(uint8_t r1, uint8_t g1, uint8_t b1,
                           uint8_t r2, uint8_t g2, uint8_t b2);
void lumi_ui_set_screensaver(bool enabled, uint8_t style,
                             uint32_t delay_seconds,
                             uint8_t r1, uint8_t g1, uint8_t b1,
                             uint8_t r2, uint8_t g2, uint8_t b2);
void lumi_ui_set_sleep_timeout(uint32_t seconds);
void lumi_ui_set_deep_sleep_timeout(uint32_t seconds);
void lumi_ui_set_hibernate_timeout(uint32_t seconds);
bool lumi_ui_is_soft_sleeping(void);
bool lumi_ui_is_deep_sleeping(void);
void lumi_ui_set_rgb_idle_timeout(uint32_t seconds);
void lumi_ui_sleep_now(void);
void lumi_ui_wake_now(void);
void lumi_ui_set_screensaver_delay(uint32_t seconds);
void lumi_ui_set_screensaver_source(bool pc_monitor);
void lumi_ui_show_screensaver_now(void);
void lumi_ui_note_activity(void);
void lumi_ui_note_key_activity(void);
void lumi_ui_set_media_active(bool active);

void lumi_ui_pc_monitor_update(
    uint8_t cpu_load,
    int16_t cpu_temp_c,
    uint16_t cpu_clock_mhz,
    int16_t gpu_load,
    int16_t gpu_temp_c,
    uint16_t gpu_clock_mhz,
    uint8_t ram_load,
    uint32_t ram_used_mb,
    uint32_t ram_total_mb,
    uint32_t net_down_kbps,
    uint32_t net_up_kbps,
    int16_t fps);
void lumi_ui_pc_monitor_clear(void);
void lumi_ui_pc_monitor_set_config_name(const char *name);
void lumi_ui_pc_monitor_set_layout(const uint8_t slots[6]);

bool lumi_ui_saver_anim_begin(uint8_t frame_count, uint16_t frame_interval_ms);
bool lumi_ui_saver_anim_set_frame_interval(uint8_t index, uint16_t interval_ms);
void lumi_ui_saver_anim_frame(uint8_t index, const uint8_t *data, size_t len);
bool lumi_ui_saver_anim_chunk(uint8_t index, uint16_t offset,
                              const uint8_t *data, size_t len);
bool lumi_ui_saver_anim_end(void);

bool lumi_ui_saver_packed_begin(size_t total_bytes);
bool lumi_ui_saver_packed_chunk(uint32_t offset,
                                 const uint8_t *data, size_t len);
bool lumi_ui_saver_packed_end(void);

bool lumi_ui_saver_image_begin(size_t total_bytes);
bool lumi_ui_saver_image_chunk(uint32_t offset,
                               const uint8_t *data, size_t len);
bool lumi_ui_saver_image_end(void);

bool lumi_ui_saver_anim_is_valid(void);
void lumi_ui_saver_anim_clear(void);
