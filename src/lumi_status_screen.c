/* SPDX-License-Identifier: MIT
 * RYNOR ONE: four columns, three rows, live ZMK keymap captions.
 */
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>
#include <lvgl.h>
#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/bt.h>
#include <dt-bindings/zmk/outputs.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>
#include <zmk/keymap.h>
#include <zmk/pm.h>
#include <zmk/usb.h>
#include "lumi_panel.h"
#include "lumi_battery.h"
#include "lumi_now_playing.h"
#include "lumi_rgb.h"
#include "lumi_ui_config.h"
#include "lumi_diag.h"

#define KEY_COUNT 12
#define COLS 4
#define CELL_W 80
#define CELL_H 49
#define STATUS_H 25
#define PRESS_MIN_MS 100

static lv_obj_t *tiles[KEY_COUNT], *icons[KEY_COUNT], *captions[KEY_COUNT];
static lv_obj_t *layer_label, *output_label, *battery_label;
static atomic_t held_keys, tapped_keys;
static atomic_t popup_action;

static lv_obj_t *popup;
static lv_obj_t *popup_icon;
static lv_obj_t *popup_text;

static lv_obj_t *screensaver;
static lv_obj_t *saver_orb1;
static lv_obj_t *saver_orb2;
static lv_obj_t *saver_glass;
static lv_obj_t *saver_title;

#define SAVER_STRIPE_SRC_ROWS 16U
#define SAVER_STRIPE_DST_ROWS (SAVER_STRIPE_SRC_ROWS * 2U)
#define SAVER_MIN_FRAME_MS 40U /* 25 FPS maximum playback rate */

static const struct device *const saver_display =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static lv_color_t saver_stripe_buf[320U * SAVER_STRIPE_DST_ROWS];
static lv_color_t saver_rgb332_lut[256];
static bool saver_rgb332_lut_ready;

#define SAVER_FLASH_MAGIC 0x4C534156U /* "LSAV" */
#define SAVER_FLASH_VERSION 2U
#define SAVER_FORMAT_RGB332 0U
#define SAVER_FORMAT_RGB565_STATIC 1U
#define SAVER_FORMAT_RYQ1 2U
#define SAVER_PACKED_MODE_RGB565 0U
#define SAVER_PACKED_MODE_RGB332 1U
#define SAVER_PACKED_FLAGS 0x03U
#define SAVER_PACKED_HEADER_BYTES 26U
#define SAVER_PACKED_MAX_BYTES (336U * 1024U)
#define SAVER_FLASH_DATA_OFFSET 0x1000U
#define SAVER_FLASH_PAGE_SIZE 0x1000U
#define SAVER_FLASH_MAX_PAGES 86U
#define SAVER_FLASH_TIMING_OFFSET 0x40U
#define SAVER_FLASH_TIMING_MAGIC 0x4D495453U /* "STIM" */

struct saver_flash_header {
    uint32_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint32_t frame_bytes;
    uint8_t frame_count;
    uint8_t format;
    uint16_t interval_ms;
    uint32_t data_size;
};

struct saver_flash_timing {
    uint32_t magic;
    uint8_t frame_count;
    uint8_t reserved0;
    uint16_t reserved1;
    uint16_t interval_ms[LUMI_SAVER_MAX_FRAMES];
    uint16_t reserved2;
};

static const struct flash_area *saver_flash;
static bool saver_flash_checked;
static uint32_t saver_flash_erased_pages[3];
static uint8_t saver_media_frame_buffer[LUMI_SAVER_FRAME_BYTES];
static uint8_t saver_media_next_frame_buffer[LUMI_SAVER_FRAME_BYTES];
static uint8_t saver_image_row_buffer[LUMI_SAVER_IMAGE_W * 2U];
static uint8_t saver_prefetched_index;
static uint8_t saver_prefetched_next_index;
static bool saver_prefetch_valid;
static bool saver_prefetch_next_valid;
static uint8_t saver_media_frame_count;
static uint8_t saver_media_format = SAVER_FORMAT_RGB332;
static uint32_t saver_media_received_mask;
static uint16_t saver_media_received_bytes[LUMI_SAVER_MAX_FRAMES];
static uint32_t saver_image_received_bytes;
static uint32_t saver_packed_expected_bytes;
static uint32_t saver_packed_received_bytes;
static uint32_t saver_packed_data_size;
static uint16_t saver_packed_storage_width;
static uint16_t saver_packed_storage_height;
static uint16_t saver_packed_frame_count;
static uint16_t saver_packed_fps;
static uint32_t saver_packed_duration_ms;
static uint32_t saver_packed_frames_offset;
static uint32_t saver_packed_cursor;
static uint32_t saver_packed_next_frame_at;
static uint16_t saver_packed_frame_index;
static uint8_t saver_packed_color_mode;
static bool saver_packed_header_ready;
static bool saver_packed_playback_started;
static lv_color_t saver_packed_line_buf[320U];
static uint8_t saver_packed_read_cache[256U];
static uint16_t saver_packed_cache_pos;
static uint16_t saver_packed_cache_len;
static bool saver_static_drawn;
static uint16_t saver_media_interval_ms = 40;
static uint16_t saver_media_frame_intervals[LUMI_SAVER_MAX_FRAMES];
static uint32_t saver_media_loop_ms = 40U;
static uint8_t saver_media_index;
static uint32_t saver_media_epoch_ms;
static bool saver_media_valid;
static bool screensaver_visible;
static lv_obj_t *root_screen;
static lv_obj_t *sleep_overlay;
static lv_obj_t *boot_overlay;
static lv_obj_t *boot_progress;
static uint32_t boot_started_ms;
static bool boot_low_power_wake;
#define BOOT_BACKLIGHT_DELAY_MS 500U
#define BOOT_WAKE_BACKLIGHT_DELAY_MS 40U
#define BOOT_SPLASH_VISIBLE_MS 1500U

static lv_obj_t *pc_monitor_overlay;
static lv_obj_t *pc_cpu_title;
static lv_obj_t *pc_cpu_value;
static lv_obj_t *pc_cpu_meta;
static lv_obj_t *pc_gpu_title;
static lv_obj_t *pc_gpu_value;
static lv_obj_t *pc_gpu_meta;
static lv_obj_t *pc_ram_title;
static lv_obj_t *pc_ram_value;
static lv_obj_t *pc_ram_meta;
static lv_obj_t *pc_net_down;
static lv_obj_t *pc_net_up;
static lv_obj_t *pc_fps_value;
static lv_obj_t *pc_monitor_status;
static bool pc_monitor_selected;
static bool pc_monitor_saver_active;
static char pc_monitor_config_name[20] = "MY PC";
static uint8_t pc_monitor_metric_slots[6] = {0, 3, 6, 9, 10, 11};

struct pc_monitor_state {
    uint8_t cpu_load;
    int16_t cpu_temp_c;
    uint16_t cpu_clock_mhz;
    int16_t gpu_load;
    int16_t gpu_temp_c;
    uint16_t gpu_clock_mhz;
    uint8_t ram_load;
    uint32_t ram_used_mb;
    uint32_t ram_total_mb;
    uint32_t net_down_kbps;
    uint32_t net_up_kbps;
    int16_t fps;
    uint32_t updated_ms;
    bool valid;
};

static struct pc_monitor_state pc_monitor_state;

K_MUTEX_DEFINE(lumi_ui_config_lock);
static uint32_t ui_last_activity_ms;
static uint32_t rgb_last_activity_ms;
static uint32_t saver_delay_ms = 60000U;
static uint32_t sleep_delay_ms = 120000U;
static uint32_t deep_sleep_delay_ms = 900000U;
static uint32_t hibernate_delay_ms = 0U;
static uint32_t rgb_idle_delay_ms = 60000U;
static bool rgb_idle_suspended;
static bool deep_sleep_active;
static bool saver_enabled = true;
static uint8_t saver_style = LUMI_SAVER_OFF;
static uint32_t wallpaper_color_a = 0x000000;
static uint32_t wallpaper_color_b = 0x10151F;
static uint32_t saver_color_a = 0x4A7DFF;
static uint32_t saver_color_b = 0xA955FF;
static bool wallpaper_dirty = true;
static bool saver_style_dirty = true;
static bool media_active = false;
static bool soft_sleep = false;
static bool saver_force_show = false;
static bool saver_source_pc_monitor = false;
static lv_timer_t *pressed_lv_timer;
static lv_timer_t *popup_lv_timer;
static lv_timer_t *pc_monitor_lv_timer;
static lv_timer_t *screensaver_lv_timer;
static void pc_metric_format(
    uint8_t metric,
    const struct pc_monitor_state *state,
    bool stale,
    char *title,
    size_t title_len,
    char *value,
    size_t value_len,
    char *meta,
    size_t meta_len) {

    const char *fallback = "--";
    snprintf(meta, meta_len, " ");

    switch (metric) {
    case 0:
        snprintf(title, title_len, "CPU USE");
        if (stale) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%u%%", (unsigned int)state->cpu_load);
        }
        break;
    case 1:
        snprintf(title, title_len, "CPU TEMP");
        if (stale || state->cpu_temp_c < 0) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%dC", (int)state->cpu_temp_c);
        }
        break;
    case 2:
        snprintf(title, title_len, "CPU CLK");
        if (stale || state->cpu_clock_mhz == 0U) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%u", (unsigned int)state->cpu_clock_mhz);
            snprintf(meta, meta_len, "MHz");
        }
        break;
    case 3:
        snprintf(title, title_len, "GPU USE");
        if (stale || state->gpu_load < 0) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%d%%", (int)state->gpu_load);
        }
        break;
    case 4:
        snprintf(title, title_len, "GPU TEMP");
        if (stale || state->gpu_temp_c < 0) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%dC", (int)state->gpu_temp_c);
        }
        break;
    case 5:
        snprintf(title, title_len, "GPU CLK");
        if (stale || state->gpu_clock_mhz == 0U) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%u", (unsigned int)state->gpu_clock_mhz);
            snprintf(meta, meta_len, "MHz");
        }
        break;
    case 6:
        snprintf(title, title_len, "RAM USE");
        if (stale) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%u%%", (unsigned int)state->ram_load);
        }
        break;
    case 7: {
        snprintf(title, title_len, "RAM USED");
        if (stale) {
            snprintf(value, value_len, "--");
        } else {
            uint32_t tenths = (state->ram_used_mb * 10U) / 1024U;
            snprintf(value, value_len, "%u.%u",
                     (unsigned int)(tenths / 10U),
                     (unsigned int)(tenths % 10U));
            snprintf(meta, meta_len, "GB");
        }
        break;
    }
    case 8: {
        snprintf(title, title_len, "RAM TOTAL");
        if (stale) {
            snprintf(value, value_len, "--");
        } else {
            uint32_t tenths = (state->ram_total_mb * 10U) / 1024U;
            snprintf(value, value_len, "%u.%u",
                     (unsigned int)(tenths / 10U),
                     (unsigned int)(tenths % 10U));
            snprintf(meta, meta_len, "GB");
        }
        break;
    }
    case 9: {
        snprintf(title, title_len, "NET DOWN");
        if (stale) {
            snprintf(value, value_len, "--");
        } else if (state->net_down_kbps < 1000U) {
            snprintf(value, value_len, "%u",
                     (unsigned int)state->net_down_kbps);
            snprintf(meta, meta_len, "Kb/s");
        } else {
            uint32_t tenths = state->net_down_kbps / 100U;
            snprintf(value, value_len, "%u.%u",
                     (unsigned int)(tenths / 10U),
                     (unsigned int)(tenths % 10U));
            snprintf(meta, meta_len, "Mb/s");
        }
        break;
    }
    case 10: {
        snprintf(title, title_len, "NET UP");
        if (stale) {
            snprintf(value, value_len, "--");
        } else if (state->net_up_kbps < 1000U) {
            snprintf(value, value_len, "%u",
                     (unsigned int)state->net_up_kbps);
            snprintf(meta, meta_len, "Kb/s");
        } else {
            uint32_t tenths = state->net_up_kbps / 100U;
            snprintf(value, value_len, "%u.%u",
                     (unsigned int)(tenths / 10U),
                     (unsigned int)(tenths % 10U));
            snprintf(meta, meta_len, "Mb/s");
        }
        break;
    }
    case 11:
    default:
        snprintf(title, title_len, "FPS");
        if (stale || state->fps < 0) {
            snprintf(value, value_len, "--");
        } else {
            snprintf(value, value_len, "%d", (int)state->fps);
        }
        break;
    }
}

static void pc_render_card(
    lv_obj_t *title_label,
    lv_obj_t *value_label,
    lv_obj_t *meta_label,
    uint8_t metric,
    const struct pc_monitor_state *state,
    bool stale) {

    char title[16];
    char value[20];
    char meta[16];

    pc_metric_format(
        metric,
        state,
        stale,
        title,
        sizeof(title),
        value,
        sizeof(value),
        meta,
        sizeof(meta));

    lv_label_set_text(title_label, title);
    lv_label_set_text(value_label, value);
    lv_label_set_text(meta_label, meta);
}

static void pc_render_footer(
    lv_obj_t *label,
    uint8_t metric,
    const struct pc_monitor_state *state,
    bool stale) {

    char title[16];
    char value[20];
    char meta[16];
    char line[32];

    pc_metric_format(
        metric,
        state,
        stale,
        title,
        sizeof(title),
        value,
        sizeof(value),
        meta,
        sizeof(meta));

    switch (metric) {
    case 0:
    case 1:
        snprintf(line, sizeof(line), "CPU %s", value);
        break;
    case 2:
        snprintf(
            line,
            sizeof(line),
            stale ? "CPU --" : "CPU %uM",
            (unsigned int)state->cpu_clock_mhz);
        break;
    case 3:
    case 4:
        snprintf(line, sizeof(line), "GPU %s", value);
        break;
    case 5:
        snprintf(
            line,
            sizeof(line),
            (stale || state->gpu_clock_mhz == 0U)
                ? "GPU --"
                : "GPU %uM",
            (unsigned int)state->gpu_clock_mhz);
        break;
    case 6:
        snprintf(line, sizeof(line), "RAM %s", value);
        break;
    case 7:
    case 8:
        snprintf(
            line,
            sizeof(line),
            "%s %sG",
            metric == 7 ? "USED" : "TOTAL",
            value);
        break;
    case 9:
        if (stale) {
            snprintf(line, sizeof(line), "DOWN --");
        } else {
            snprintf(
                line,
                sizeof(line),
                "DOWN %s%s",
                value,
                strstr(meta, "Mb") ? "M" : "K");
        }
        break;
    case 10:
        if (stale) {
            snprintf(line, sizeof(line), "UP --");
        } else {
            snprintf(
                line,
                sizeof(line),
                "UP %s%s",
                value,
                strstr(meta, "Mb") ? "M" : "K");
        }
        break;
    case 11:
    default:
        snprintf(line, sizeof(line), "FPS %s", value);
        break;
    }

    lv_label_set_text(label, line);
}

static void refresh_pc_monitor_labels(void) {
    if (!pc_monitor_overlay ||
        !pc_cpu_title || !pc_cpu_value || !pc_cpu_meta ||
        !pc_gpu_title || !pc_gpu_value || !pc_gpu_meta ||
        !pc_ram_title || !pc_ram_value || !pc_ram_meta) {
        return;
    }

    struct pc_monitor_state state;
    uint8_t slots[6];

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    state = pc_monitor_state;
    memcpy(slots, pc_monitor_metric_slots, sizeof(slots));
    k_mutex_unlock(&lumi_ui_config_lock);

    bool stale =
        !state.valid ||
        (uint32_t)(k_uptime_get_32() - state.updated_ms) > 5000U;

    pc_render_card(
        pc_cpu_title,
        pc_cpu_value,
        pc_cpu_meta,
        slots[0],
        &state,
        stale);
    pc_render_card(
        pc_gpu_title,
        pc_gpu_value,
        pc_gpu_meta,
        slots[1],
        &state,
        stale);
    pc_render_card(
        pc_ram_title,
        pc_ram_value,
        pc_ram_meta,
        slots[2],
        &state,
        stale);

    pc_render_footer(
        pc_net_down,
        slots[3],
        &state,
        stale);
    pc_render_footer(
        pc_net_up,
        slots[4],
        &state,
        stale);
    pc_render_footer(
        pc_fps_value,
        slots[5],
        &state,
        stale);
}

static void pc_monitor_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    refresh_pc_monitor_labels();
}

K_WORK_DEFINE(pc_monitor_work, pc_monitor_work_handler);

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
    int16_t fps) {

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    pc_monitor_state.cpu_load = MIN(cpu_load, 100U);
    pc_monitor_state.cpu_temp_c = cpu_temp_c;
    pc_monitor_state.cpu_clock_mhz = cpu_clock_mhz;
    pc_monitor_state.gpu_load =
        gpu_load < 0 ? -1 : MIN(gpu_load, 100);
    pc_monitor_state.gpu_temp_c = gpu_temp_c;
    pc_monitor_state.gpu_clock_mhz = gpu_clock_mhz;
    pc_monitor_state.ram_load = MIN(ram_load, 100U);
    pc_monitor_state.ram_used_mb = ram_used_mb;
    pc_monitor_state.ram_total_mb = ram_total_mb;
    pc_monitor_state.net_down_kbps = net_down_kbps;
    pc_monitor_state.net_up_kbps = net_up_kbps;
    pc_monitor_state.fps = fps;
    pc_monitor_state.updated_ms = k_uptime_get_32();
    pc_monitor_state.valid = true;
    k_mutex_unlock(&lumi_ui_config_lock);

    k_work_submit_to_queue(
        zmk_display_work_q(),
        &pc_monitor_work);
}

void lumi_ui_pc_monitor_clear(void) {
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    pc_monitor_state.valid = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    k_work_submit_to_queue(
        zmk_display_work_q(),
        &pc_monitor_work);
}

void lumi_ui_pc_monitor_set_config_name(const char *name) {
    if (!name) {
        name = "MY PC";
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    snprintf(
        pc_monitor_config_name,
        sizeof(pc_monitor_config_name),
        "%.18s",
        name);
    k_mutex_unlock(&lumi_ui_config_lock);

    k_work_submit_to_queue(
        zmk_display_work_q(),
        &pc_monitor_work);
}

void lumi_ui_pc_monitor_set_layout(const uint8_t slots[6]) {
    if (!slots) {
        return;
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    for (uint8_t i = 0U; i < 6U; i++) {
        pc_monitor_metric_slots[i] = MIN(slots[i], 11U);
    }
    k_mutex_unlock(&lumi_ui_config_lock);

    k_work_submit_to_queue(
        zmk_display_work_q(),
        &pc_monitor_work);
}

static void update_pc_monitor_visibility(void) {
    if (!pc_monitor_overlay) {
        return;
    }

    if (pc_monitor_selected || pc_monitor_saver_active) {
        refresh_pc_monitor_labels();
        lv_obj_clear_flag(
            pc_monitor_overlay,
            LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(pc_monitor_overlay);
    } else {
        lv_obj_add_flag(
            pc_monitor_overlay,
            LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_pc_monitor_selected(bool selected) {
    pc_monitor_selected = selected;
    update_pc_monitor_visibility();
}


static uint32_t popup_until;
static bool popup_visible = false;

enum {
    POPUP_NONE = 0,
    POPUP_VOL_UP,
    POPUP_VOL_DOWN,
    POPUP_NEXT,
    POPUP_PREVIOUS,
    POPUP_PAGE_UP,
    POPUP_PAGE_DOWN,
};
static uint32_t press_started[KEY_COUNT];
static bool highlighted[KEY_COUNT];
static uint32_t icon_colors[KEY_COUNT];
static lv_color_t accent;

/* Slots are ZMK positions 0..11 in order. Position 12 is the encoder.
 * The wired 4x4 matrix and its transform remain unchanged.
 */
struct key_caption {
    char text[16];
    const char *icon;
    uint32_t color;
};
struct page_state {
    zmk_keymap_layer_id_t id;
    char name[32];
    struct key_caption keys[KEY_COUNT];
    uint8_t battery_percent;
    bool battery_ready;
};

/* Dedicated navigation icons. Use only LVGL built-in symbols so these render
 * with the existing Montserrat/symbol font set on the RYNOR ONE display.
 */
#define LUMI_ICON_HOME       LV_SYMBOL_HOME
#define LUMI_ICON_END        LV_SYMBOL_STOP
#define LUMI_ICON_PAGE_UP    LV_SYMBOL_LIST LV_SYMBOL_UP
#define LUMI_ICON_PAGE_DOWN  LV_SYMBOL_LIST LV_SYMBOL_DOWN

/*
 * ZMK Studio can restore a &kp parameter from settings in either the full
 * ZMK HID encoding (usage page + usage ID) or an equivalent raw keyboard
 * usage value. Compare navigation keys by normalized HID usage instead of by
 * the complete 32-bit macro value so HOME/END/PAGE UP/PAGE DOWN never fall
 * through to the generic keyboard icon after a Studio remap/reboot.
 */
static bool describe_navigation_key(uint32_t code, struct key_caption *out) {
    uint32_t stripped = STRIP_MODS(code);
    uint16_t page = ZMK_HID_USAGE_PAGE(stripped);

    if (page == 0U) {
        page = HID_USAGE_KEY;
    }
    if (page != HID_USAGE_KEY) {
        return false;
    }

    switch (ZMK_HID_USAGE_ID(stripped)) {
    case HID_USAGE_KEY_KEYBOARD_HOME:
        snprintf(out->text, sizeof(out->text), "HOME");
        out->icon = LUMI_ICON_HOME;
        out->color = 0xFF9F0A;
        return true;
    case HID_USAGE_KEY_KEYBOARD_END:
        snprintf(out->text, sizeof(out->text), "END");
        out->icon = LUMI_ICON_END;
        out->color = 0xFF9F0A;
        return true;
    case HID_USAGE_KEY_KEYBOARD_PAGEUP:
        snprintf(out->text, sizeof(out->text), "PAGE UP");
        out->icon = LUMI_ICON_PAGE_UP;
        out->color = 0xBF5AF2;
        return true;
    case HID_USAGE_KEY_KEYBOARD_PAGEDOWN:
        snprintf(out->text, sizeof(out->text), "PAGE DOWN");
        out->icon = LUMI_ICON_PAGE_DOWN;
        out->color = 0xBF5AF2;
        return true;
    default:
        return false;
    }
}

static bool is_key_press_behavior(const struct zmk_behavior_binding *binding) {
    if (!binding || !binding->behavior_dev) {
        return false;
    }

    const char *kp_name = DEVICE_DT_NAME(DT_NODELABEL(kp));
    return strcmp(binding->behavior_dev, kp_name) == 0 ||
           strstr(binding->behavior_dev, "key_press") != NULL;
}

static void describe_key(uint32_t code, struct key_caption *out) {
    const char *text = NULL;
    out->icon = LV_SYMBOL_KEYBOARD;
    out->color = 0xFFFFFF;

    if (describe_navigation_key(code, out)) {
        return;
    }

    switch (code) {
    case LC(C): text = "COPY"; out->icon = LV_SYMBOL_COPY; out->color = 0x64D2FF; break;
    case LC(V): text = "PASTE"; out->icon = LV_SYMBOL_PASTE; out->color = 0x30D158; break;
    case LC(X): text = "CUT"; out->icon = LV_SYMBOL_CUT; out->color = 0xFF453A; break;
    case LC(Z): text = "UNDO"; out->icon = LV_SYMBOL_LEFT; out->color = 0x0A84FF; break;
    case LC(Y): text = "REDO"; out->icon = LV_SYMBOL_RIGHT; out->color = 0xBF5AF2; break;
    case LC(A): text = "SELECT ALL"; out->icon = LV_SYMBOL_LIST; out->color = 0xFFD60A; break;
    case LC(S): text = "SAVE"; out->icon = LV_SYMBOL_SAVE; out->color = 0xFF9F0A; break;
    case LC(F): text = "FIND"; out->icon = LV_SYMBOL_EYE_OPEN; out->color = 0x5AC8FA; break;
    case ENTER: text = "ENTER"; out->icon = LV_SYMBOL_OK; out->color = 0x30D158; break;
    case BACKSPACE: text = "BACKSPACE"; out->icon = LV_SYMBOL_BACKSPACE; out->color = 0xFF453A; break;
    case TAB: text = "TAB"; out->icon = LV_SYMBOL_RIGHT; out->color = 0x64D2FF; break;
    case ESC: text = "ESC"; out->icon = LV_SYMBOL_CLOSE; out->color = 0xFF9F0A; break;
    case DELETE: text = "DELETE"; out->icon = LV_SYMBOL_TRASH; out->color = 0xFF453A; break;
    case C_PLAY_PAUSE: text = "PLAY/PAUSE"; out->icon = LV_SYMBOL_PLAY; out->color = 0x30D158; break;
    case C_PREVIOUS: text = "PREVIOUS"; out->icon = LV_SYMBOL_PREV; out->color = 0x64D2FF; break;
    case C_NEXT: text = "NEXT"; out->icon = LV_SYMBOL_NEXT; out->color = 0x64D2FF; break;
    case C_STOP: text = "STOP"; out->icon = LV_SYMBOL_STOP; out->color = 0xFF453A; break;
    case C_MUTE: text = "MUTE"; out->icon = LV_SYMBOL_MUTE; out->color = 0xFF453A; break;
    case C_VOL_DN: text = "VOL -"; out->icon = LV_SYMBOL_VOLUME_MID; out->color = 0x0A84FF; break;
    case C_VOL_UP: text = "VOL +"; out->icon = LV_SYMBOL_VOLUME_MAX; out->color = 0x0A84FF; break;
    case C_AC_BACK: text = "BACK"; out->icon = LV_SYMBOL_LEFT; out->color = 0x5AC8FA; break;
    case C_AC_FORWARD: text = "FORWARD"; out->icon = LV_SYMBOL_RIGHT; out->color = 0x5AC8FA; break;
    case HOME: text = "HOME"; out->icon = LUMI_ICON_HOME; out->color = 0xFF9F0A; break;
    case END: text = "END"; out->icon = LUMI_ICON_END; out->color = 0xFF9F0A; break;
    case PG_UP: text = "PAGE UP"; out->icon = LUMI_ICON_PAGE_UP; out->color = 0xBF5AF2; break;
    case PG_DN: text = "PAGE DOWN"; out->icon = LUMI_ICON_PAGE_DOWN; out->color = 0xBF5AF2; break;
    default: break;
    }

    if (text) {
        snprintf(out->text, sizeof(out->text), "%s", text);
    } else if (STRIP_MODS(code) >= A && STRIP_MODS(code) <= Z) {
        out->color = 0xD8F8FF;
        snprintf(out->text, sizeof(out->text), "%s%s%s%s%c",
                 SELECT_MODS(code) & (MOD_LCTL | MOD_RCTL) ? "C+" : "",
                 SELECT_MODS(code) & (MOD_LALT | MOD_RALT) ? "A+" : "",
                 SELECT_MODS(code) & (MOD_LSFT | MOD_RSFT) ? "S+" : "",
                 SELECT_MODS(code) & (MOD_LGUI | MOD_RGUI) ? "W+" : "",
                 (char)('A' + STRIP_MODS(code) - A));
    } else {
        out->color = 0xFFFFFF;
        snprintf(out->text, sizeof(out->text), "0x%08X", (unsigned int)code);
    }
}

static void set_profile_key_visual(
    struct key_caption *out,
    const char *text,
    const char *icon,
    uint32_t color
) {
    snprintf(out->text, sizeof(out->text), "%s", text);
    out->icon = icon;
    out->color = color;
}

/* Give the built-in application/game profiles semantic icons instead of
 * showing the same keyboard glyph for every single-letter shortcut. Each
 * override verifies the expected keycode first, so a Studio remap falls back
 * to describe_key() and never leaves a stale/misleading icon on screen.
 */
static void describe_profile_key(
    zmk_keymap_layer_index_t layer,
    uint8_t position,
    uint32_t code,
    struct key_caption *out
) {
#define PROFILE_ICON(pos_, code_, text_, icon_, color_) \
    case (pos_): \
        if (code == (code_)) { \
            set_profile_key_visual(out, (text_), (icon_), (color_)); \
        } \
        break

    switch (layer) {
    case 0: /* P1 MAIN / OFFICE - live ZMK bindings */
        switch (position) {
        PROFILE_ICON(1, LG(LS(S)), "SCREENSHOT", LV_SYMBOL_IMAGE, 0x5AC8FA);
        PROFILE_ICON(2, HOME, "HOME", LUMI_ICON_HOME, 0xFF9F0A);
        PROFILE_ICON(3, PG_UP, "PAGE UP", LUMI_ICON_PAGE_UP, 0xBF5AF2);
        PROFILE_ICON(4, LC(LG(LEFT)), "DESKTOP LEFT", LV_SYMBOL_LEFT, 0x64D2FF);
        PROFILE_ICON(5, LC(LG(RIGHT)), "DESKTOP RIGHT", LV_SYMBOL_RIGHT, 0x64D2FF);
        PROFILE_ICON(6, END, "END", LUMI_ICON_END, 0xFF9F0A);
        PROFILE_ICON(7, PG_DN, "PAGE DOWN", LUMI_ICON_PAGE_DOWN, 0xBF5AF2);
        PROFILE_ICON(8, LC(LS(Z)), "REDO", LV_SYMBOL_RIGHT, 0xBF5AF2);
        PROFILE_ICON(9, LC(Z), "UNDO", LV_SYMBOL_LEFT, 0x0A84FF);
        PROFILE_ICON(10, LC(C), "COPY", LV_SYMBOL_COPY, 0x64D2FF);
        PROFILE_ICON(11, LC(V), "PASTE", LV_SYMBOL_PASTE, 0x30D158);
        }
        break;

    case 1: /* P2 MEDIA */
        switch (position) {
        PROFILE_ICON(0, C_PLAY_PAUSE, "PLAY/PAUSE", LV_SYMBOL_PLAY, 0x30D158);
        PROFILE_ICON(1, C_PREVIOUS, "PREVIOUS", LV_SYMBOL_PREV, 0x64D2FF);
        PROFILE_ICON(2, C_NEXT, "NEXT", LV_SYMBOL_NEXT, 0x64D2FF);
        PROFILE_ICON(3, C_MUTE, "MUTE", LV_SYMBOL_MUTE, 0xFF453A);
        PROFILE_ICON(4, C_REWIND, "REWIND", LV_SYMBOL_LEFT, 0x5AC8FA);
        PROFILE_ICON(5, C_FAST_FORWARD, "FORWARD", LV_SYMBOL_RIGHT, 0x5AC8FA);
        PROFILE_ICON(6, C_STOP, "STOP", LV_SYMBOL_STOP, 0xFF453A);
        PROFILE_ICON(7, F11, "FULLSCREEN", LV_SYMBOL_IMAGE, 0xBF5AF2);
        PROFILE_ICON(8, C_AC_BACK, "BACK", LV_SYMBOL_LEFT, 0x64D2FF);
        PROFILE_ICON(9, C_AC_FORWARD, "FORWARD", LV_SYMBOL_RIGHT, 0x64D2FF);
        PROFILE_ICON(10, HOME, "HOME", LUMI_ICON_HOME, 0xFF9F0A);
        PROFILE_ICON(11, END, "END", LUMI_ICON_END, 0xFF9F0A);
        }
        break;

    case 2: /* P3 BAMBU STUDIO */
        switch (position) {
        PROFILE_ICON(0, LC(R), "SLICE", LV_SYMBOL_CUT, 0xFF9F0A);
        PROFILE_ICON(1, LC(LS(G)), "PRINT", LV_SYMBOL_UPLOAD, 0x30D158);
        PROFILE_ICON(2, LC(G), "EXPORT", LV_SYMBOL_SAVE, 0x64D2FF);
        PROFILE_ICON(3, LC(I), "IMPORT", LV_SYMBOL_DOWNLOAD, 0x64D2FF);
        PROFILE_ICON(4, M, "MOVE", LV_SYMBOL_GPS, 0x5AC8FA);
        PROFILE_ICON(5, R, "ROTATE", LV_SYMBOL_REFRESH, 0xBF5AF2);
        PROFILE_ICON(6, S, "SCALE", LV_SYMBOL_PLUS, 0xFFD60A);
        PROFILE_ICON(7, F, "LAY FACE", LV_SYMBOL_DOWN, 0x30D158);
        PROFILE_ICON(8, C, "CUT", LV_SYMBOL_CUT, 0xFF453A);
        PROFILE_ICON(9, A, "ARRANGE", LV_SYMBOL_LIST, 0x64D2FF);
        PROFILE_ICON(10, LS(R), "AUTO ORIENT", LV_SYMBOL_REFRESH, 0x30D158);
        PROFILE_ICON(11, LC(S), "SAVE", LV_SYMBOL_SAVE, 0xFF9F0A);
        }
        break;

    case 3: /* P4 FUSION 360 */
        switch (position) {
        PROFILE_ICON(0, E, "EXTRUDE", LV_SYMBOL_UP, 0x30D158);
        PROFILE_ICON(1, Q, "PRESS PULL", LV_SYMBOL_PLUS, 0x5AC8FA);
        PROFILE_ICON(2, F, "FILLET", LV_SYMBOL_EDIT, 0xBF5AF2);
        PROFILE_ICON(3, H, "HOLE", LV_SYMBOL_MINUS, 0xFF453A);
        PROFILE_ICON(4, M, "MOVE", LV_SYMBOL_GPS, 0x64D2FF);
        PROFILE_ICON(5, I, "MEASURE", LV_SYMBOL_EYE_OPEN, 0xFFD60A);
        PROFILE_ICON(6, P, "PROJECT", LV_SYMBOL_IMAGE, 0x5AC8FA);
        PROFILE_ICON(7, D, "DIMENSION", LV_SYMBOL_EDIT, 0xFF9F0A);
        PROFILE_ICON(8, L, "LINE", LV_SYMBOL_MINUS, 0x64D2FF);
        PROFILE_ICON(9, R, "RECTANGLE", LV_SYMBOL_LIST, 0x30D158);
        PROFILE_ICON(10, C, "CIRCLE", LV_SYMBOL_LOOP, 0xBF5AF2);
        PROFILE_ICON(11, J, "JOINT", LV_SYMBOL_SETTINGS, 0xFF9F0A);
        }
        break;

    case 4: /* P5 CAPCUT */
        switch (position) {
        PROFILE_ICON(0, LC(B), "SPLIT", LV_SYMBOL_CUT, 0xFF453A);
        PROFILE_ICON(1, Q, "TRIM LEFT", LV_SYMBOL_LEFT, 0x64D2FF);
        PROFILE_ICON(2, W, "TRIM RIGHT", LV_SYMBOL_RIGHT, 0x64D2FF);
        PROFILE_ICON(3, DELETE, "DELETE", LV_SYMBOL_TRASH, 0xFF453A);
        PROFILE_ICON(4, LC(Z), "UNDO", LV_SYMBOL_LEFT, 0x0A84FF);
        PROFILE_ICON(5, LC(LS(Z)), "REDO", LV_SYMBOL_RIGHT, 0xBF5AF2);
        PROFILE_ICON(6, LC(D), "DUPLICATE", LV_SYMBOL_COPY, 0x5AC8FA);
        PROFILE_ICON(7, SPACE, "PLAY/PAUSE", LV_SYMBOL_PLAY, 0x30D158);
        PROFILE_ICON(8, LEFT, "PREV FRAME", LV_SYMBOL_PREV, 0x64D2FF);
        PROFILE_ICON(9, RIGHT, "NEXT FRAME", LV_SYMBOL_NEXT, 0x64D2FF);
        PROFILE_ICON(10, LC(E), "EXPORT", LV_SYMBOL_UPLOAD, 0xFF9F0A);
        PROFILE_ICON(11, LC(S), "SAVE", LV_SYMBOL_SAVE, 0x30D158);
        }
        break;

    case 5: /* P6 DELTA FORCE */
        switch (position) {
        PROFILE_ICON(0, Y, "TACTICAL", LV_SYMBOL_SETTINGS, 0xFF9F0A);
        PROFILE_ICON(1, X, "TACT GEAR", LV_SYMBOL_SETTINGS, 0xBF5AF2);
        PROFILE_ICON(2, V, "GADGET 1", LV_SYMBOL_PLUS, 0x30D158);
        PROFILE_ICON(3, G, "GADGET 2", LV_SYMBOL_PLUS, 0x5AC8FA);
        PROFILE_ICON(4, N, "OPTIC", LV_SYMBOL_EYE_OPEN, 0xFFD60A);
        PROFILE_ICON(5, J, "BIPOD", LV_SYMBOL_DOWN, 0x64D2FF);
        PROFILE_ICON(6, K, "RANGE", LV_SYMBOL_GPS, 0x5AC8FA);
        PROFILE_ICON(7, EQUAL, "AUTO RUN", LV_SYMBOL_RIGHT, 0x30D158);
        PROFILE_ICON(8, M, "MAP", LV_SYMBOL_GPS, 0xFFD60A);
        PROFILE_ICON(9, LC(M), "MUTE SQUAD", LV_SYMBOL_MUTE, 0xFF453A);
        PROFILE_ICON(10, U, "FLASHLIGHT", LV_SYMBOL_EYE_OPEN, 0xFF9F0A);
        PROFILE_ICON(11, H, "INTERACT", LV_SYMBOL_OK, 0x30D158);
        }
        break;

    case 6: /* P7 WUWA */
        switch (position) {
        PROFILE_ICON(0, M, "MAP", LV_SYMBOL_GPS, 0xFFD60A);
        PROFILE_ICON(1, J, "QUEST", LV_SYMBOL_LIST, 0x64D2FF);
        PROFILE_ICON(2, B, "BACKPACK", LV_SYMBOL_DIRECTORY, 0xFF9F0A);
        PROFILE_ICON(3, C, "RESONATOR", LV_SYMBOL_AUDIO, 0xBF5AF2);
        PROFILE_ICON(4, L, "TEAM", LV_SYMBOL_LIST, 0x30D158);
        PROFILE_ICON(5, O, "UTILITIES", LV_SYMBOL_SETTINGS, 0x5AC8FA);
        PROFILE_ICON(6, F1, "EVENT", LV_SYMBOL_BELL, 0xFF9F0A);
        PROFILE_ICON(7, F2, "GUIDEBOOK", LV_SYMBOL_FILE, 0x64D2FF);
        PROFILE_ICON(8, F3, "CONVENE", LV_SYMBOL_SHUFFLE, 0xBF5AF2);
        PROFILE_ICON(9, F4, "PODCAST", LV_SYMBOL_AUDIO, 0x5AC8FA);
        PROFILE_ICON(10, U, "CO-OP", LV_SYMBOL_WIFI, 0x30D158);
        PROFILE_ICON(11, T, "UTILITY", LV_SYMBOL_SETTINGS, 0xFFD60A);
        }
        break;

    default:
        break;
    }

#undef PROFILE_ICON
}

static bool describe_profile_behavior(
    zmk_keymap_layer_index_t layer,
    uint8_t position,
    const struct zmk_behavior_binding *binding,
    struct key_caption *out
) {
    if (!binding || !binding->behavior_dev) {
        return false;
    }

    /* ZMK's built-in &sl node is named sticky_layer. Match the live behavior
     * instead of hard-coding K1 visually so a future Studio remap cannot leave
     * a misleading layer icon behind.
     */
    if (layer == 0U && position == 0U &&
        strstr(binding->behavior_dev, "sticky_layer") != NULL) {
        set_profile_key_visual(
            out,
            "STICKY LAYER",
            LV_SYMBOL_LOOP,
            0xBF5AF2);
        return true;
    }

    /* Profile 10 is the connection toolbox. Give ZMK's Bluetooth/output
     * behaviors readable labels instead of exposing internal device names.
     */
    if (strstr(binding->behavior_dev, "outputs") != NULL) {
        if (binding->param1 == OUT_BLE) {
            set_profile_key_visual(
                out,
                "BLE OUTPUT",
                LV_SYMBOL_WIFI,
                0x64D2FF);
            return true;
        }

        if (binding->param1 == OUT_USB) {
            set_profile_key_visual(
                out,
                "USB OUTPUT",
                LV_SYMBOL_USB,
                0x30D158);
            return true;
        }
    }

    if (strstr(binding->behavior_dev, "bluetooth") != NULL) {
        switch (binding->param1) {
        case BT_SEL_CMD:
            snprintf(
                out->text,
                sizeof(out->text),
                "BLE %u",
                (unsigned int)binding->param2 + 1U);
            out->icon = LV_SYMBOL_WIFI;
            out->color = 0x64D2FF;
            return true;
        case BT_CLR_CMD:
            set_profile_key_visual(
                out,
                "RESET BLE",
                LV_SYMBOL_TRASH,
                0xFF9F0A);
            return true;
        case BT_CLR_ALL_CMD:
            set_profile_key_visual(
                out,
                "RESET ALL BLE",
                LV_SYMBOL_TRASH,
                0xFF453A);
            return true;
        case BT_NXT_CMD:
            set_profile_key_visual(
                out,
                "BLE NEXT",
                LV_SYMBOL_NEXT,
                0x5AC8FA);
            return true;
        case BT_PRV_CMD:
            set_profile_key_visual(
                out,
                "BLE PREV",
                LV_SYMBOL_PREV,
                0x5AC8FA);
            return true;
        default:
            break;
        }
    }

    return false;
}

static struct page_state read_page(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    struct page_state state = {0};
    state.battery_percent = lumi_battery_percent();
    state.battery_ready = lumi_battery_ready();

    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    state.id = zmk_keymap_layer_index_to_id(index);
    const char *name = zmk_keymap_layer_name(state.id);
    if (name && name[0]) {
        snprintf(state.name, sizeof(state.name), "%s", name);
    } else {
        snprintf(state.name, sizeof(state.name), "LAYER %u", index + 1);
    }
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(state.id, i);
        struct key_caption *key = &state.keys[i];
        key->icon = LV_SYMBOL_KEYBOARD;
        key->color = 0xFFFFFF;
        if (!binding || !binding->behavior_dev) {
            snprintf(key->text, sizeof(key->text), "--");
        } else if (is_key_press_behavior(binding)) {
            describe_key(binding->param1, key);
            describe_profile_key(index, i, binding->param1, key);
        } else if (describe_navigation_key(binding->param1, key)) {
            /*
             * Last-resort compatibility for key-press bindings restored from
             * Studio with a behavior name that differs from the compiled DT
             * device name. Navigation HID usages are unambiguous here and
             * should never render as the generic keyboard glyph.
             */
        } else if (!describe_profile_behavior(index, i, binding, key)) {
            /* Never keep a stale semantic label for a remapped behavior. */
            snprintf(key->text, sizeof(key->text), "%.8s %u",
                     binding->behavior_dev, (unsigned int)binding->param1);
        }
    }
    return state;
}

static void set_tile_pressed(uint8_t i, bool pressed) {
    lv_obj_set_style_bg_color(
        tiles[i],
        pressed ? lv_color_hex(0x18333D) : lv_color_hex(0x000000),
        0
    );

    lv_obj_set_style_bg_opa(
        tiles[i],
        pressed ? 205 : 78,
        0
    );

    lv_obj_set_style_border_color(
        tiles[i],
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_set_style_text_color(
        icons[i],
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_set_style_text_color(
        captions[i],
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_align(
        icons[i],
        LV_ALIGN_CENTER,
        0,
        pressed ? 3 : 0
    );
}

static void profile_anim_y_cb(void *obj, int32_t value) {
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void animate_profile_from_below(void) {
    lv_anim_t a;

    if (layer_label) {
        lv_anim_del(layer_label, profile_anim_y_cb);
        lv_obj_set_y(layer_label, 22);

        lv_anim_init(&a);
        lv_anim_set_var(&a, layer_label);
        lv_anim_set_exec_cb(&a, profile_anim_y_cb);
        lv_anim_set_values(&a, 22, 6);
        lv_anim_set_time(&a, 230);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        if (!tiles[i]) {
            continue;
        }

        uint8_t row = i / COLS;
        int32_t target_y = STATUS_H + row * CELL_H;

        lv_anim_del(tiles[i], profile_anim_y_cb);
        lv_obj_set_y(tiles[i], target_y + 18);

        lv_anim_init(&a);
        lv_anim_set_var(&a, tiles[i]);
        lv_anim_set_exec_cb(&a, profile_anim_y_cb);
        lv_anim_set_values(&a, target_y + 18, target_y);
        lv_anim_set_time(&a, 230);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static void update_page(struct page_state state) {
    static struct page_state previous;
    static bool have_previous;
    bool same = have_previous && previous.id == state.id &&
                strcmp(previous.name, state.name) == 0 &&
                previous.battery_percent == state.battery_percent &&
                previous.battery_ready == state.battery_ready;
    for (uint8_t i = 0; same && i < KEY_COUNT; i++) {
        same = previous.keys[i].icon == state.keys[i].icon &&
               previous.keys[i].color == state.keys[i].color &&
               strcmp(previous.keys[i].text, state.keys[i].text) == 0;
    }
    if (!layer_label || same) {
        return;
    }

    set_pc_monitor_selected(state.id == 7);

    bool animate_change = have_previous && previous.id != state.id;
    previous = state;
    have_previous = true;
    const uint32_t colors[] = {0x5de0c3, 0xffc765, 0x7ebcff, 0xBF5AF2, 0xFF9F0A};
    accent = lv_color_hex(colors[state.id % ARRAY_SIZE(colors)]);
    lv_label_set_text(layer_label, state.name);
    lv_obj_set_style_text_color(layer_label, accent, 0);

    if (battery_label) {
        char battery_text[12];
        if (state.battery_ready) {
            snprintf(
                battery_text,
                sizeof(battery_text),
                "%u%%",
                (unsigned int)state.battery_percent);
        } else {
            snprintf(battery_text, sizeof(battery_text), "--%%");
        }
        lv_label_set_text(battery_label, battery_text);
    }

    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        lv_label_set_text(captions[i], state.keys[i].text);
        lv_label_set_text(icons[i], state.keys[i].icon);
        icon_colors[i] = state.keys[i].color;
        set_tile_pressed(i, highlighted[i]);
    }

    if (animate_change) {
        animate_profile_from_below();
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(lumi_page, struct page_state, update_page, read_page)
ZMK_SUBSCRIPTION(lumi_page, zmk_layer_state_changed);

/* Studio edits do not all emit layer_state_changed in v0.3. Poll on the
 * system queue, copy state, and perform every LVGL call on the display queue.
 */
static void poll_page(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(page_poll_work, poll_page);

static void poll_page(struct k_work *work) {
    ARG_UNUSED(work);

    bool sleeping;
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    sleeping = soft_sleep;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (sleeping) {
        /* Stop this polling work completely during soft sleep. The wake path
         * restarts it after the panel is ready, avoiding a 2-second CPU wakeup
         * for a screen that is intentionally off.
         */
        return;
    }

    lumi_page_refresh_state(NULL);
    k_work_submit_to_queue(zmk_display_work_q(), &lumi_page_work);
    k_work_schedule(&page_poll_work, K_MSEC(500));
}

struct output_state {
    struct zmk_endpoint_instance endpoint;
    bool connected;
    bool open;
};
static struct output_state read_output(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return (struct output_state){
        .endpoint = zmk_endpoints_selected(),
        .connected = zmk_ble_active_profile_is_connected(),
        .open = zmk_ble_active_profile_is_open(),
    };
}
static void update_output(struct output_state state) {
    if (!output_label) {
        return;
    }
    if (state.endpoint.transport == ZMK_TRANSPORT_USB) {
        lv_label_set_text(output_label, LV_SYMBOL_USB " USB");
    } else {
        char text[24];
        snprintf(text, sizeof(text), "BLE%u %s", state.endpoint.ble.profile_index + 1,
                 state.connected ? LV_SYMBOL_OK :
                                   (state.open ? LV_SYMBOL_SETTINGS : LV_SYMBOL_CLOSE));
        lv_label_set_text(output_label, text);
    }
}
ZMK_DISPLAY_WIDGET_LISTENER(lumi_output, struct output_state, update_output, read_output)
ZMK_SUBSCRIPTION(lumi_output, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(lumi_output, zmk_ble_active_profile_changed);

static int position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);

    if (event && event->state) {
        lumi_ui_note_key_activity();

        /* Any physical button press returns from Now Playing to the main
         * key grid immediately. Encoder rotation is a sensor event rather
         * than a position event, so volume rotation can keep Media visible.
         */
        lumi_now_playing_user_activity();
    }

    if (event && event->position < KEY_COUNT) {
        if (event->state) {
            atomic_set_bit(&held_keys, event->position);
            atomic_set_bit(&tapped_keys, event->position);
        } else {
            atomic_clear_bit(&held_keys, event->position);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(lumi_keys, position_listener);
ZMK_SUBSCRIPTION(lumi_keys, zmk_position_state_changed);

static bool keycode_matches(
    const struct zmk_keycode_state_changed *event,
    uint32_t encoded
) {
    uint16_t page = ZMK_HID_USAGE_PAGE(encoded);

    if (page == 0) {
        page = HID_USAGE_KEY;
    }

    return event->usage_page == page &&
           event->keycode == ZMK_HID_USAGE_ID(encoded);
}

static int popup_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *event =
        as_zmk_keycode_state_changed(eh);

    if (!event || !event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    lumi_ui_note_key_activity();
    bool keep_music_visible = false;

    if (keycode_matches(event, C_VOL_UP)) {
        atomic_set(&popup_action, POPUP_VOL_UP);
        keep_music_visible = true;

    } else if (keycode_matches(event, C_VOL_DN)) {
        atomic_set(&popup_action, POPUP_VOL_DOWN);
        keep_music_visible = true;

    } else if (keycode_matches(event, C_NEXT)) {
        atomic_set(&popup_action, POPUP_NEXT);
        keep_music_visible = true;

    } else if (keycode_matches(event, C_PREVIOUS)) {
        atomic_set(&popup_action, POPUP_PREVIOUS);
        keep_music_visible = true;

    } else if (keycode_matches(event, C_PLAY_PAUSE) ||
               keycode_matches(event, C_MUTE)) {
        keep_music_visible = true;

    } else if (keycode_matches(event, PG_UP)) {
        atomic_set(&popup_action, POPUP_PAGE_UP);

    } else if (keycode_matches(event, PG_DN)) {
        atomic_set(&popup_action, POPUP_PAGE_DOWN);
    }

    if (!keep_music_visible) {
        lumi_now_playing_user_activity();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(lumi_popup, popup_keycode_listener);
ZMK_SUBSCRIPTION(lumi_popup, zmk_keycode_state_changed);
static void refresh_pressed(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    uint32_t now = lv_tick_get();
    /* Atomic exchange retains taps that finish between display ticks. */
    atomic_val_t taps = atomic_set(&tapped_keys, 0);
    atomic_val_t held = atomic_get(&held_keys);
    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        if (taps & BIT(i)) {
            press_started[i] = now;
        }
        bool pressed = (held & BIT(i)) || (taps & BIT(i)) ||
                       (highlighted[i] && (uint32_t)(now - press_started[i]) < PRESS_MIN_MS);
        if (pressed != highlighted[i]) {
            highlighted[i] = pressed;
            set_tile_pressed(i, pressed);
        }
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}
static void popup_anim_y_cb(void *obj, int32_t value) {
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void popup_anim_opa_cb(void *obj, int32_t value) {
    lv_obj_set_style_opa((lv_obj_t *)obj, value, 0);
}

static void popup_animate(int32_t y_from,
                          int32_t y_to,
                          int32_t opa_from,
                          int32_t opa_to,
                          uint32_t time_ms,
                          bool showing) {

    lv_anim_del(popup, popup_anim_y_cb);
    lv_anim_del(popup, popup_anim_opa_cb);

    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, popup);
    lv_anim_set_exec_cb(&a, popup_anim_y_cb);
    lv_anim_set_values(&a, y_from, y_to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(
        &a,
        showing ? lv_anim_path_ease_out : lv_anim_path_ease_in
    );
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, popup);
    lv_anim_set_exec_cb(&a, popup_anim_opa_cb);
    lv_anim_set_values(&a, opa_from, opa_to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(
        &a,
        showing ? lv_anim_path_ease_out : lv_anim_path_ease_in
    );
    lv_anim_start(&a);
}
static void refresh_popup(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    int action = atomic_set(&popup_action, POPUP_NONE);
    uint32_t now = lv_tick_get();

    if (action != POPUP_NONE) {
        const char *icon = LV_SYMBOL_SETTINGS;
        const char *text = "";

        switch (action) {

        case POPUP_VOL_UP:
            icon = LV_SYMBOL_VOLUME_MAX;
            text = "VOLUME +";
            break;

        case POPUP_VOL_DOWN:
            icon = LV_SYMBOL_VOLUME_MID;
            text = "VOLUME -";
            break;

        case POPUP_NEXT:
            icon = LV_SYMBOL_NEXT;
            text = "NEXT";
            break;

        case POPUP_PREVIOUS:
            icon = LV_SYMBOL_PREV;
            text = "PREVIOUS";
            break;

        case POPUP_PAGE_UP:
            icon = LV_SYMBOL_UP;
            text = "PAGE UP";
            break;

        case POPUP_PAGE_DOWN:
            icon = LV_SYMBOL_DOWN;
            text = "PAGE DOWN";
            break;
        }

        lv_label_set_text(popup_icon, icon);
        lv_label_set_text(popup_text, text);

        /* Nếu popup chưa hiện thì trượt từ dưới lên */
        if (!popup_visible) {

            lv_obj_clear_flag(popup, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(popup);

            lv_obj_set_y(popup, 172);
            lv_obj_set_style_opa(popup, 0, 0);

            popup_animate(
                172,
                116,
                0,
                255,
                180,
                true
            );

            popup_visible = true;
        }

        /* Xoay tiếp thì chỉ kéo dài thời gian hiện,
         * không chạy lại animation liên tục.
         */
        popup_until = now + 650;
    }

    /* Hết thời gian thì trượt xuống */
    if (popup_visible) {
        lv_obj_move_foreground(popup);
    }

    if (popup_visible &&
        (int32_t)(now - popup_until) >= 0) {

        popup_animate(
            lv_obj_get_y(popup),
            172,
            255,
            0,
            160,
            false
        );

        popup_visible = false;
    }
}

static int32_t saver_wave(uint32_t now,
                           uint32_t period,
                           int32_t min_value,
                           int32_t max_value) {
    uint32_t phase = now % period;
    uint32_t half = period / 2U;
    uint32_t pos = phase <= half ? phase : period - phase;

    return min_value +
           (int32_t)(((int64_t)(max_value - min_value) * pos) / half);
}

static void init_screensaver(lv_obj_t *screen) {
    screensaver = lv_obj_create(screen);
    lv_obj_remove_style_all(screensaver);
    lv_obj_set_pos(screensaver, 0, 0);
    lv_obj_set_size(screensaver, 320, 172);
    lv_obj_set_style_bg_color(screensaver, lv_color_hex(0x030407), 0);
    lv_obj_set_style_bg_opa(screensaver, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screensaver, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver, LV_OBJ_FLAG_HIDDEN);

    saver_orb1 = lv_obj_create(screensaver);
    lv_obj_remove_style_all(saver_orb1);
    lv_obj_set_size(saver_orb1, 104, 104);
    lv_obj_set_style_radius(saver_orb1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(saver_orb1, lv_color_hex(0x4A7DFF), 0);
    lv_obj_set_style_bg_opa(saver_orb1, 72, 0);

    saver_orb2 = lv_obj_create(screensaver);
    lv_obj_remove_style_all(saver_orb2);
    lv_obj_set_size(saver_orb2, 92, 92);
    lv_obj_set_style_radius(saver_orb2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(saver_orb2, lv_color_hex(0xA955FF), 0);
    lv_obj_set_style_bg_opa(saver_orb2, 62, 0);

    saver_glass = lv_obj_create(screensaver);
    lv_obj_remove_style_all(saver_glass);
    lv_obj_set_size(saver_glass, 176, 58);
    lv_obj_align(saver_glass, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(saver_glass, lv_color_hex(0x141720), 0);
    lv_obj_set_style_bg_opa(saver_glass, 210, 0);
    lv_obj_set_style_radius(saver_glass, 29, 0);
    lv_obj_set_style_border_width(saver_glass, 1, 0);
    lv_obj_set_style_border_color(saver_glass, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(saver_glass, 52, 0);

    saver_title = make_label(saver_glass, &lv_font_montserrat_20);
    lv_label_set_text(saver_title, "LumiPad");
    lv_obj_set_style_text_color(saver_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(saver_title);

}

static void saver_timing_recalculate(void) {
    uint32_t total = 0U;

    for (uint8_t i = 0U; i < saver_media_frame_count; i++) {
        uint16_t interval =
            CLAMP(saver_media_frame_intervals[i],
                  (uint16_t)SAVER_MIN_FRAME_MS,
                  (uint16_t)5000U);
        saver_media_frame_intervals[i] = interval;
        total += interval;
    }

    saver_media_loop_ms = MAX(total, 1U);

    if (saver_media_frame_count > 0U) {
        saver_media_interval_ms =
            (uint16_t)CLAMP(
                saver_media_loop_ms / saver_media_frame_count,
                SAVER_MIN_FRAME_MS,
                5000U);
    }
}

static void saver_timing_set_uniform(
    uint8_t frame_count,
    uint16_t interval_ms) {

    interval_ms =
        CLAMP(interval_ms,
              (uint16_t)SAVER_MIN_FRAME_MS,
              (uint16_t)5000U);

    memset(
        saver_media_frame_intervals,
        0,
        sizeof(saver_media_frame_intervals));

    for (uint8_t i = 0U;
         i < frame_count && i < LUMI_SAVER_MAX_FRAMES;
         i++) {
        saver_media_frame_intervals[i] = interval_ms;
    }

    saver_media_loop_ms =
        MAX((uint32_t)frame_count * interval_ms, 1U);
    saver_media_interval_ms = interval_ms;
}

static int saver_flash_open_once(void) {
    if (saver_flash) {
        return 0;
    }

    int rc = flash_area_open(
        FIXED_PARTITION_ID(lumi_saver_partition),
        &saver_flash);

    if (rc != 0 || !saver_flash) {
        saver_flash = NULL;
        lumi_diag_report('E', "Saver flash open rc=%d", rc);
        return rc != 0 ? rc : -ENODEV;
    }

    size_t required =
        SAVER_FLASH_DATA_OFFSET +
        MAX(
            MAX(
                (size_t)LUMI_SAVER_MAX_FRAMES * LUMI_SAVER_FRAME_BYTES,
                (size_t)LUMI_SAVER_IMAGE_BYTES),
            (size_t)SAVER_PACKED_MAX_BYTES);

    if (saver_flash->fa_size < required) {
        lumi_diag_report('E', "Saver flash too small have=%u need=%u",
                         (unsigned int)saver_flash->fa_size,
                         (unsigned int)required);
        return -ENOSPC;
    }

    return 0;
}

static uint16_t saver_packed_read_le16(const uint8_t *data) {
    return (uint16_t)data[0] |
           ((uint16_t)data[1] << 8);
}

static uint32_t saver_packed_read_le32(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void saver_packed_reset_state(void) {
    saver_packed_expected_bytes = 0U;
    saver_packed_received_bytes = 0U;
    saver_packed_data_size = 0U;
    saver_packed_storage_width = 0U;
    saver_packed_storage_height = 0U;
    saver_packed_frame_count = 0U;
    saver_packed_fps = 0U;
    saver_packed_duration_ms = 0U;
    saver_packed_frames_offset = 0U;
    saver_packed_cursor = 0U;
    saver_packed_next_frame_at = 0U;
    saver_packed_frame_index = 0U;
    saver_packed_color_mode = SAVER_PACKED_MODE_RGB332;
    saver_packed_header_ready = false;
    saver_packed_playback_started = false;
    saver_packed_cache_pos = 0U;
    saver_packed_cache_len = 0U;
}

static bool saver_packed_load_header(
    uint32_t data_size,
    bool apply) {

    if (!saver_flash ||
        data_size < SAVER_PACKED_HEADER_BYTES ||
        data_size > SAVER_PACKED_MAX_BYTES) {
        return false;
    }

    uint8_t header[SAVER_PACKED_HEADER_BYTES] = {0};
    int rc = flash_area_read(
        saver_flash,
        SAVER_FLASH_DATA_OFFSET,
        header,
        sizeof(header));

    if (rc != 0 ||
        header[0] != 'R' ||
        header[1] != 'Y' ||
        header[2] != 'Q' ||
        header[3] != '1') {
        return false;
    }

    uint8_t color_mode = header[4];
    uint8_t flags = header[5];
    uint16_t storage_width =
        saver_packed_read_le16(&header[6]);
    uint16_t storage_height =
        saver_packed_read_le16(&header[8]);
    uint16_t display_width =
        saver_packed_read_le16(&header[10]);
    uint16_t display_height =
        saver_packed_read_le16(&header[12]);
    uint16_t frame_count =
        saver_packed_read_le16(&header[14]);
    uint16_t fps =
        saver_packed_read_le16(&header[16]);
    uint32_t duration_ms =
        saver_packed_read_le32(&header[18]);
    uint16_t palette_count =
        saver_packed_read_le16(&header[22]);

    bool storage_ok =
        (storage_width == 320U && storage_height == 172U) ||
        (storage_width == 240U && storage_height == 129U) ||
        (storage_width == 160U && storage_height == 86U);

    if ((color_mode != SAVER_PACKED_MODE_RGB565 &&
         color_mode != SAVER_PACKED_MODE_RGB332) ||
        flags != SAVER_PACKED_FLAGS ||
        !storage_ok ||
        display_width != 320U ||
        display_height != 172U ||
        frame_count < 1U ||
        frame_count > 250U ||
        fps < 15U ||
        fps > 25U ||
        duration_ms == 0U ||
        palette_count != 0U) {
        return false;
    }

    if (apply) {
        saver_packed_data_size = data_size;
        saver_packed_storage_width = storage_width;
        saver_packed_storage_height = storage_height;
        saver_packed_frame_count = frame_count;
        saver_packed_fps = fps;
        saver_packed_duration_ms = duration_ms;
        saver_packed_frames_offset = SAVER_PACKED_HEADER_BYTES;
        saver_packed_cursor = SAVER_PACKED_HEADER_BYTES;
        saver_packed_next_frame_at = 0U;
        saver_packed_frame_index = 0U;
        saver_packed_color_mode = color_mode;
        saver_packed_header_ready = true;
        saver_packed_playback_started = false;
        saver_media_frame_count = (uint8_t)frame_count;
        saver_media_interval_ms =
            (uint16_t)MAX(
                (uint32_t)SAVER_MIN_FRAME_MS,
                (1000U + fps - 1U) / fps);
    }

    return true;
}

static bool saver_flash_load_metadata(void) {
    if (saver_flash_checked) {
        return saver_media_valid;
    }

    saver_flash_checked = true;
    saver_media_valid = false;

    if (saver_flash_open_once() != 0) {
        return false;
    }

    struct saver_flash_header header = {0};
    int read_rc = flash_area_read(saver_flash, 0, &header, sizeof(header));
    if (read_rc != 0) {
        lumi_diag_report('E', "Saver header read rc=%d", read_rc);
        return false;
    }

    bool gif_ok =
        header.format == SAVER_FORMAT_RGB332 &&
        header.width == LUMI_SAVER_FRAME_W &&
        header.height == LUMI_SAVER_FRAME_H &&
        header.frame_bytes == LUMI_SAVER_FRAME_BYTES &&
        header.frame_count >= 1U &&
        header.frame_count <= LUMI_SAVER_MAX_FRAMES &&
        header.interval_ms >= SAVER_MIN_FRAME_MS &&
        header.data_size ==
            (uint32_t)header.frame_count * LUMI_SAVER_FRAME_BYTES;

    bool static_ok =
        header.format == SAVER_FORMAT_RGB565_STATIC &&
        header.width == LUMI_SAVER_IMAGE_W &&
        header.height == LUMI_SAVER_IMAGE_H &&
        header.frame_bytes == LUMI_SAVER_IMAGE_BYTES &&
        header.frame_count == 1U &&
        header.data_size == LUMI_SAVER_IMAGE_BYTES;

    bool packed_ok =
        header.format == SAVER_FORMAT_RYQ1 &&
        header.frame_bytes == 0U &&
        header.frame_count >= 1U &&
        header.data_size >= SAVER_PACKED_HEADER_BYTES &&
        header.data_size <= SAVER_PACKED_MAX_BYTES;

    if (header.magic != SAVER_FLASH_MAGIC ||
        header.version != SAVER_FLASH_VERSION ||
        (!gif_ok && !static_ok && !packed_ok)) {
        lumi_diag_report(
            'W',
            "Saver metadata invalid magic=%08x fmt=%u frames=%u",
            (unsigned int)header.magic,
            (unsigned int)header.format,
            (unsigned int)header.frame_count);
        return false;
    }

    saver_media_format = header.format;
    saver_packed_reset_state();

    if (packed_ok) {
        if (!saver_packed_load_header(header.data_size, true) ||
            saver_packed_frame_count != header.frame_count ||
            saver_packed_storage_width != header.width ||
            saver_packed_storage_height != header.height) {
            lumi_diag_report('W', "RYQ1 header mismatch");
            return false;
        }
    } else {
        saver_media_frame_count = header.frame_count;
        saver_timing_set_uniform(
            saver_media_frame_count,
            static_ok ? 1000U : header.interval_ms);

        struct saver_flash_timing timing = {0};
        int timing_rc = flash_area_read(
            saver_flash,
            SAVER_FLASH_TIMING_OFFSET,
            &timing,
            sizeof(timing));

        if (saver_media_format == SAVER_FORMAT_RGB332 &&
            timing_rc == 0 &&
            timing.magic == SAVER_FLASH_TIMING_MAGIC &&
            timing.frame_count == saver_media_frame_count) {

            for (uint8_t i = 0U;
                 i < saver_media_frame_count;
                 i++) {
                saver_media_frame_intervals[i] =
                    timing.interval_ms[i];
            }

            saver_timing_recalculate();
        }
    }

    saver_media_index = 0U;
    saver_media_epoch_ms = 0U;
    saver_prefetch_valid = false;
    saver_prefetch_next_valid = false;
    saver_static_drawn = false;
    saver_media_valid = true;
    lumi_diag_report(
        'I',
        "Saver metadata OK fmt=%u frames=%u interval=%ums",
        (unsigned int)saver_media_format,
        (unsigned int)saver_media_frame_count,
        (unsigned int)saver_media_interval_ms);
    return true;
}

static void saver_flash_reset_erase_tracking(void) {
    memset(saver_flash_erased_pages, 0, sizeof(saver_flash_erased_pages));
}

static bool saver_flash_page_is_erased(uint32_t page) {
    if (page >= SAVER_FLASH_MAX_PAGES) {
        return false;
    }

    return (saver_flash_erased_pages[page / 32U] & BIT(page % 32U)) != 0U;
}

static void saver_flash_mark_page_erased(uint32_t page) {
    if (page < SAVER_FLASH_MAX_PAGES) {
        saver_flash_erased_pages[page / 32U] |= BIT(page % 32U);
    }
}

static int saver_flash_erase_page(uint32_t page) {
    if (!saver_flash || page >= SAVER_FLASH_MAX_PAGES) {
        return -EINVAL;
    }

    if (saver_flash_page_is_erased(page)) {
        return 0;
    }

    uint32_t offset = page * SAVER_FLASH_PAGE_SIZE;
    if (offset + SAVER_FLASH_PAGE_SIZE > saver_flash->fa_size) {
        return -ENOSPC;
    }

    int rc = flash_area_erase(
        saver_flash,
        offset,
        SAVER_FLASH_PAGE_SIZE);

    if (rc == 0) {
        saver_flash_mark_page_erased(page);
    }

    return rc;
}

static int saver_flash_prepare_upload(void) {
    int rc = saver_flash_open_once();
    if (rc != 0) {
        return rc;
    }

    saver_flash_reset_erase_tracking();

    /* Erase the metadata page first. Until a new valid header is written at
     * SAVEND, an interrupted upload is intentionally treated as invalid.
     */
    rc = saver_flash_erase_page(0U);
    if (rc != 0) {
        return rc;
    }

    saver_flash_checked = true;
    saver_media_valid = false;
    return 0;
}

static int saver_flash_write_bytes(uint32_t data_offset,
                                   const uint8_t *data,
                                   size_t len) {
    if (!saver_flash || !data || len == 0U) {
        return -EINVAL;
    }

    uint32_t absolute = SAVER_FLASH_DATA_OFFSET + data_offset;

    if ((size_t)absolute + len > saver_flash->fa_size) {
        return -ENOSPC;
    }

    uint32_t first_page = absolute / SAVER_FLASH_PAGE_SIZE;
    uint32_t last_page =
        (uint32_t)(absolute + len - 1U) / SAVER_FLASH_PAGE_SIZE;

    for (uint32_t page = first_page; page <= last_page; page++) {
        int rc = saver_flash_erase_page(page);
        if (rc != 0) {
            return rc;
        }
    }

    return flash_area_write(saver_flash, absolute, data, len);
}

static int saver_flash_write_chunk(uint8_t index,
                                   uint16_t offset,
                                   const uint8_t *data,
                                   size_t len) {
    uint32_t data_offset =
        (uint32_t)index * LUMI_SAVER_FRAME_BYTES + offset;
    return saver_flash_write_bytes(data_offset, data, len);
}

static int saver_flash_read_frame(uint8_t index) {
    if (!saver_media_valid ||
        index >= saver_media_frame_count ||
        saver_flash_open_once() != 0) {
        return -EINVAL;
    }

    uint32_t offset =
        SAVER_FLASH_DATA_OFFSET +
        (uint32_t)index * LUMI_SAVER_FRAME_BYTES;

    return flash_area_read(
        saver_flash,
        offset,
        saver_media_frame_buffer,
        sizeof(saver_media_frame_buffer));
}

static int saver_flash_read_frame_into(
    uint8_t index,
    uint8_t *destination) {

    if (!destination ||
        saver_media_format != SAVER_FORMAT_RGB332 ||
        !saver_flash_load_metadata() ||
        index >= saver_media_frame_count ||
        saver_flash_open_once() != 0) {
        return -EINVAL;
    }

    uint32_t offset =
        SAVER_FLASH_DATA_OFFSET +
        (uint32_t)index * LUMI_SAVER_FRAME_BYTES;

    return flash_area_read(
        saver_flash,
        offset,
        destination,
        LUMI_SAVER_FRAME_BYTES);
}

static int saver_flash_prefetch_frame(uint8_t index) {
    if (!saver_flash_load_metadata() ||
        index >= saver_media_frame_count ||
        saver_flash_open_once() != 0) {
        saver_prefetch_valid = false;
        return -EINVAL;
    }

    uint32_t offset =
        SAVER_FLASH_DATA_OFFSET +
        (uint32_t)index * LUMI_SAVER_FRAME_BYTES;

    int rc = flash_area_read(
        saver_flash,
        offset,
        saver_media_frame_buffer,
        sizeof(saver_media_frame_buffer));

    if (rc == 0) {
        saver_prefetched_index = index;
        saver_prefetch_valid = true;
    } else {
        saver_prefetch_valid = false;
    }

    return rc;
}

static int saver_flash_commit_header(void) {
    if (!saver_flash) {
        return -ENODEV;
    }

    bool static_image =
        saver_media_format == SAVER_FORMAT_RGB565_STATIC;
    bool packed_animation =
        saver_media_format == SAVER_FORMAT_RYQ1;

    struct saver_flash_header header = {
        .magic = SAVER_FLASH_MAGIC,
        .version = SAVER_FLASH_VERSION,
        .width = packed_animation
            ? saver_packed_storage_width
            : (static_image
                ? LUMI_SAVER_IMAGE_W
                : LUMI_SAVER_FRAME_W),
        .height = packed_animation
            ? saver_packed_storage_height
            : (static_image
                ? LUMI_SAVER_IMAGE_H
                : LUMI_SAVER_FRAME_H),
        .frame_bytes = packed_animation
            ? 0U
            : (static_image
                ? LUMI_SAVER_IMAGE_BYTES
                : LUMI_SAVER_FRAME_BYTES),
        .frame_count = saver_media_frame_count,
        .format = saver_media_format,
        .interval_ms = static_image
            ? 1000U
            : saver_media_interval_ms,
        .data_size = packed_animation
            ? saver_packed_data_size
            : (static_image
                ? LUMI_SAVER_IMAGE_BYTES
                : (uint32_t)saver_media_frame_count *
                  LUMI_SAVER_FRAME_BYTES),
    };

    int rc = flash_area_write(
        saver_flash,
        0,
        &header,
        sizeof(header));

    if (rc != 0) {
        return rc;
    }

    if (static_image || packed_animation) {
        return 0;
    }

    struct saver_flash_timing timing = {
        .magic = SAVER_FLASH_TIMING_MAGIC,
        .frame_count = saver_media_frame_count,
        .reserved0 = 0U,
        .reserved1 = 0U,
        .reserved2 = 0U,
    };

    for (uint8_t i = 0U;
         i < saver_media_frame_count;
         i++) {
        timing.interval_ms[i] =
            saver_media_frame_intervals[i];
    }

    return flash_area_write(
        saver_flash,
        SAVER_FLASH_TIMING_OFFSET,
        &timing,
        sizeof(timing));
}

static void saver_flash_invalidate(void) {
    if (saver_flash_open_once() == 0) {
        saver_flash_reset_erase_tracking();
        (void)saver_flash_erase_page(0U);
    }

    saver_flash_checked = true;
    saver_media_valid = false;
}

static void draw_static_saver_image(void) {
    if (saver_static_drawn ||
        saver_media_format != SAVER_FORMAT_RGB565_STATIC ||
        !device_is_ready(saver_display) ||
        saver_flash_open_once() != 0) {
        return;
    }

    for (uint16_t y = 0U; y < LUMI_SAVER_IMAGE_H;
         y += SAVER_STRIPE_DST_ROWS) {

        uint16_t rows =
            MIN((uint16_t)SAVER_STRIPE_DST_ROWS,
                (uint16_t)(LUMI_SAVER_IMAGE_H - y));

        for (uint16_t row = 0U; row < rows; row++) {
            uint32_t flash_offset =
                SAVER_FLASH_DATA_OFFSET +
                ((uint32_t)(y + row) * LUMI_SAVER_IMAGE_W * 2U);

            int rc = flash_area_read(
                saver_flash,
                flash_offset,
                saver_image_row_buffer,
                sizeof(saver_image_row_buffer));

            if (rc != 0) {
                lumi_diag_report(
                    'E',
                    "Static saver flash read rc=%d y=%u",
                    rc,
                    (unsigned int)(y + row));
                return;
            }

            lv_color_t *dst =
                &saver_stripe_buf[(size_t)row * LUMI_SAVER_IMAGE_W];

            for (uint16_t x = 0U; x < LUMI_SAVER_IMAGE_W; x++) {
                size_t o = (size_t)x * 2U;
                uint16_t v =
                    (uint16_t)saver_image_row_buffer[o] |
                    ((uint16_t)saver_image_row_buffer[o + 1U] << 8);

                uint8_t r =
                    (uint8_t)((((v >> 11) & 0x1FU) * 255U) / 31U);
                uint8_t g =
                    (uint8_t)((((v >> 5) & 0x3FU) * 255U) / 63U);
                uint8_t b =
                    (uint8_t)(((v & 0x1FU) * 255U) / 31U);

                dst[x] = lv_color_make(r, g, b);
            }
        }

        struct display_buffer_descriptor desc = {
            .buf_size =
                (size_t)LUMI_SAVER_IMAGE_W * rows * sizeof(lv_color_t),
            .width = LUMI_SAVER_IMAGE_W,
            .height = rows,
            .pitch = LUMI_SAVER_IMAGE_W,
        };

        int rc = display_write(
            saver_display,
            0U,
            y,
            &desc,
            saver_stripe_buf);

        if (rc != 0) {
            lumi_diag_report(
                'E',
                "Static saver display_write rc=%d y=%u",
                rc,
                (unsigned int)y);
            return;
        }
    }

    saver_static_drawn = true;
}

static void saver_packed_stream_reset(uint32_t offset) {
    saver_packed_cursor = offset;
    saver_packed_cache_pos = 0U;
    saver_packed_cache_len = 0U;
}

static bool saver_packed_read_u8(uint8_t *value) {
    if (!value ||
        !saver_packed_header_ready ||
        saver_packed_cursor >= saver_packed_data_size) {
        return false;
    }

    if (saver_packed_cache_pos >= saver_packed_cache_len) {
        uint32_t remaining =
            saver_packed_data_size - saver_packed_cursor;
        uint16_t read_len =
            (uint16_t)MIN(
                (uint32_t)sizeof(saver_packed_read_cache),
                remaining);

        if (read_len == 0U ||
            flash_area_read(
                saver_flash,
                SAVER_FLASH_DATA_OFFSET + saver_packed_cursor,
                saver_packed_read_cache,
                read_len) != 0) {
            return false;
        }

        saver_packed_cache_pos = 0U;
        saver_packed_cache_len = read_len;
    }

    *value = saver_packed_read_cache[saver_packed_cache_pos++];
    saver_packed_cursor++;
    return true;
}

static bool saver_packed_read_u16(uint16_t *value) {
    uint8_t lo = 0U;
    uint8_t hi = 0U;

    if (!value ||
        !saver_packed_read_u8(&lo) ||
        !saver_packed_read_u8(&hi)) {
        return false;
    }

    *value = (uint16_t)lo | ((uint16_t)hi << 8);
    return true;
}

static bool saver_packed_read_color(lv_color_t *color) {
    if (!color) {
        return false;
    }

    if (saver_packed_color_mode == SAVER_PACKED_MODE_RGB565) {
        uint16_t value = 0U;
        if (!saver_packed_read_u16(&value)) {
            return false;
        }

        uint8_t r =
            (uint8_t)((((value >> 11) & 0x1FU) * 255U) / 31U);
        uint8_t g =
            (uint8_t)((((value >> 5) & 0x3FU) * 255U) / 63U);
        uint8_t b =
            (uint8_t)(((value & 0x1FU) * 255U) / 31U);
        *color = lv_color_make(r, g, b);
        return true;
    }

    uint8_t value = 0U;
    if (!saver_packed_read_u8(&value)) {
        return false;
    }

    uint8_t r =
        (uint8_t)((((value >> 5) & 0x07U) * 255U) / 7U);
    uint8_t g =
        (uint8_t)((((value >> 2) & 0x07U) * 255U) / 7U);
    uint8_t b =
        (uint8_t)(((value & 0x03U) * 255U) / 3U);
    *color = lv_color_make(r, g, b);
    return true;
}

static void saver_packed_set_scaled_pixel(
    uint16_t source_x,
    lv_color_t color) {

    uint16_t dx0 =
        (uint16_t)(((uint32_t)source_x * 320U) /
                   saver_packed_storage_width);
    uint16_t dx1 =
        (uint16_t)(((uint32_t)(source_x + 1U) * 320U) /
                   saver_packed_storage_width);

    if (dx1 <= dx0) {
        dx1 = MIN((uint16_t)(dx0 + 1U), (uint16_t)320U);
    }

    for (uint16_t x = dx0; x < dx1 && x < 320U; x++) {
        saver_packed_line_buf[x] = color;
    }
}

static bool saver_packed_decode_span(
    uint16_t source_y,
    uint16_t source_x,
    uint16_t source_count) {

    if (source_y >= saver_packed_storage_height ||
        source_x >= saver_packed_storage_width ||
        source_count == 0U ||
        (uint32_t)source_x + source_count > saver_packed_storage_width) {
        return false;
    }

    uint16_t produced = 0U;

    while (produced < source_count) {
        uint8_t control = 0U;
        if (!saver_packed_read_u8(&control)) {
            return false;
        }

        uint16_t packet_count =
            (uint16_t)((control & 0x7FU) + 1U);

        if ((uint32_t)produced + packet_count > source_count) {
            return false;
        }

        bool repeat = (control & 0x80U) != 0U;

        if (repeat) {
            lv_color_t color;
            if (!saver_packed_read_color(&color)) {
                return false;
            }

            for (uint16_t i = 0U; i < packet_count; i++) {
                saver_packed_set_scaled_pixel(
                    (uint16_t)(source_x + produced + i),
                    color);
            }
        } else {
            for (uint16_t i = 0U; i < packet_count; i++) {
                lv_color_t color;
                if (!saver_packed_read_color(&color)) {
                    return false;
                }

                saver_packed_set_scaled_pixel(
                    (uint16_t)(source_x + produced + i),
                    color);
            }
        }

        produced = (uint16_t)(produced + packet_count);
    }

    uint16_t dx0 =
        (uint16_t)(((uint32_t)source_x * 320U) /
                   saver_packed_storage_width);
    uint16_t dx1 =
        (uint16_t)(((uint32_t)(source_x + source_count) * 320U) /
                   saver_packed_storage_width);
    uint16_t dy0 =
        (uint16_t)(((uint32_t)source_y * 172U) /
                   saver_packed_storage_height);
    uint16_t dy1 =
        (uint16_t)(((uint32_t)(source_y + 1U) * 172U) /
                   saver_packed_storage_height);

    if (dx1 <= dx0 || dy1 <= dy0 || dx1 > 320U || dy1 > 172U) {
        return false;
    }

    struct display_buffer_descriptor desc = {
        .buf_size = (size_t)(dx1 - dx0) * sizeof(lv_color_t),
        .width = (uint16_t)(dx1 - dx0),
        .height = 1U,
        .pitch = (uint16_t)(dx1 - dx0),
    };

    for (uint16_t y = dy0; y < dy1; y++) {
        int rc = display_write(
            saver_display,
            dx0,
            y,
            &desc,
            &saver_packed_line_buf[dx0]);

        if (rc != 0) {
            lumi_diag_report(
                'E',
                "RYQ1 display_write rc=%d y=%u",
                rc,
                (unsigned int)y);
            return false;
        }
    }

    return true;
}

static void saver_packed_restart_playback(void) {
    saver_packed_stream_reset(saver_packed_frames_offset);
    saver_packed_frame_index = 0U;
    saver_packed_next_frame_at = lv_tick_get();
    saver_packed_playback_started = true;
}

static bool saver_packed_decode_next_frame(uint32_t now_ms) {
    if (saver_media_format != SAVER_FORMAT_RYQ1 ||
        !saver_packed_header_ready ||
        !saver_media_valid ||
        !device_is_ready(saver_display)) {
        return false;
    }

    if (!saver_packed_playback_started) {
        saver_packed_restart_playback();
    }

    if (saver_packed_frame_index >= saver_packed_frame_count) {
        saver_packed_stream_reset(saver_packed_frames_offset);
        saver_packed_frame_index = 0U;
    }

    uint16_t duration_ms = 0U;
    uint16_t span_count = 0U;

    if (!saver_packed_read_u16(&duration_ms) ||
        !saver_packed_read_u16(&span_count)) {
        return false;
    }

    for (uint16_t span = 0U; span < span_count; span++) {
        uint16_t y = 0U;
        uint16_t x = 0U;
        uint16_t count = 0U;

        if (!saver_packed_read_u16(&y) ||
            !saver_packed_read_u16(&x) ||
            !saver_packed_read_u16(&count) ||
            !saver_packed_decode_span(y, x, count)) {
            lumi_diag_report(
                'E',
                "RYQ1 decode failed frame=%u span=%u",
                (unsigned int)saver_packed_frame_index,
                (unsigned int)span);
            saver_packed_playback_started = false;
            return false;
        }
    }

    saver_packed_frame_index++;
    saver_packed_next_frame_at =
        now_ms + MAX((uint32_t)duration_ms, 1U);
    return true;
}

static bool saver_prepare_blend_frames(uint8_t frame_index) {
    if (saver_media_format != SAVER_FORMAT_RGB332 ||
        frame_index >= saver_media_frame_count) {
        return false;
    }

    uint8_t next_index =
        (uint8_t)((frame_index + 1U) % saver_media_frame_count);

    if (!saver_prefetch_valid ||
        saver_prefetched_index != frame_index) {
        if (saver_flash_read_frame_into(
                frame_index,
                saver_media_frame_buffer) != 0) {
            saver_prefetch_valid = false;
            return false;
        }

        saver_prefetched_index = frame_index;
        saver_prefetch_valid = true;
    }

    if (!saver_prefetch_next_valid ||
        saver_prefetched_next_index != next_index) {
        if (saver_flash_read_frame_into(
                next_index,
                saver_media_next_frame_buffer) != 0) {
            saver_prefetch_next_valid = false;
            return false;
        }

        saver_prefetched_next_index = next_index;
        saver_prefetch_next_valid = true;
    }

    return true;
}

static void draw_custom_saver_frame(
    uint8_t frame_index,
    uint8_t blend) {

    if (saver_media_format == SAVER_FORMAT_RGB565_STATIC) {
        draw_static_saver_image();
        return;
    }

    if (!device_is_ready(saver_display) ||
        frame_index >= saver_media_frame_count ||
        !saver_prepare_blend_frames(frame_index)) {
        return;
    }

    for (uint16_t src_y = 0U;
         src_y < LUMI_SAVER_FRAME_H;
         src_y += SAVER_STRIPE_SRC_ROWS) {

        uint16_t src_rows =
            MIN((uint16_t)SAVER_STRIPE_SRC_ROWS,
                (uint16_t)(LUMI_SAVER_FRAME_H - src_y));
        uint16_t dst_rows = (uint16_t)(src_rows * 2U);

        for (uint16_t row = 0U; row < src_rows; row++) {
            const uint8_t *src_a =
                &saver_media_frame_buffer[
                    (size_t)(src_y + row) * LUMI_SAVER_FRAME_W];
            const uint8_t *src_b =
                &saver_media_next_frame_buffer[
                    (size_t)(src_y + row) * LUMI_SAVER_FRAME_W];

            lv_color_t *dst0 =
                &saver_stripe_buf[(size_t)(row * 2U) * 320U];
            lv_color_t *dst1 = dst0 + 320U;

            for (uint16_t x = 0U; x < LUMI_SAVER_FRAME_W; x++) {
                uint8_t a = src_a[x];
                uint8_t b = src_b[x];

                uint16_t ar =
                    (uint16_t)((((a >> 5) & 0x07U) * 255U) / 7U);
                uint16_t ag =
                    (uint16_t)((((a >> 2) & 0x07U) * 255U) / 7U);
                uint16_t ab =
                    (uint16_t)(((a & 0x03U) * 255U) / 3U);

                uint16_t br =
                    (uint16_t)((((b >> 5) & 0x07U) * 255U) / 7U);
                uint16_t bg =
                    (uint16_t)((((b >> 2) & 0x07U) * 255U) / 7U);
                uint16_t bb =
                    (uint16_t)(((b & 0x03U) * 255U) / 3U);

                uint8_t r = (uint8_t)(
                    ar + (((int32_t)br - ar) * blend) / 255);
                uint8_t g = (uint8_t)(
                    ag + (((int32_t)bg - ag) * blend) / 255);
                uint8_t blue = (uint8_t)(
                    ab + (((int32_t)bb - ab) * blend) / 255);

                lv_color_t color = lv_color_make(r, g, blue);
                dst0[x * 2U] = color;
                dst0[x * 2U + 1U] = color;
                dst1[x * 2U] = color;
                dst1[x * 2U + 1U] = color;
            }
        }

        struct display_buffer_descriptor desc = {
            .buf_size = (size_t)320U * dst_rows * sizeof(lv_color_t),
            .width = 320U,
            .height = dst_rows,
            .pitch = 320U,
        };

        int rc = display_write(
            saver_display,
            0U,
            (uint16_t)(src_y * 2U),
            &desc,
            saver_stripe_buf);

        if (rc != 0) {
            lumi_diag_report(
                'E',
                "Saver display_write rc=%d y=%u",
                rc,
                (unsigned int)(src_y * 2U));
            break;
        }
    }
}

static void refresh_screensaver(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    uint32_t now_uptime = k_uptime_get_32();
    uint32_t delay;
    bool enabled;
    uint8_t style;
    uint32_t color_a;
    uint32_t color_b;
    bool update_wallpaper;
    bool update_saver;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    delay = saver_delay_ms;
    enabled = saver_enabled;
    style = saver_style;
    color_a = saver_color_a;
    color_b = saver_color_b;
    update_wallpaper = wallpaper_dirty;
    update_saver = saver_style_dirty;
    wallpaper_dirty = false;
    saver_style_dirty = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (update_wallpaper && root_screen) {
        uint32_t wa;
        uint32_t wb;
        k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
        wa = wallpaper_color_a;
        wb = wallpaper_color_b;
        k_mutex_unlock(&lumi_ui_config_lock);

        lv_obj_set_style_bg_color(root_screen, lv_color_hex(wa), 0);
        lv_obj_set_style_bg_grad_color(root_screen, lv_color_hex(wb), 0);
        lv_obj_set_style_bg_grad_dir(root_screen, LV_GRAD_DIR_VER, 0);
    }

    if (update_saver) {
        lv_obj_set_style_bg_color(saver_orb1, lv_color_hex(color_a), 0);
        lv_obj_set_style_bg_color(saver_orb2, lv_color_hex(color_b), 0);

        if (style == LUMI_SAVER_MINIMAL) {
            lv_obj_add_flag(saver_orb1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(saver_orb2, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(saver_glass, lv_color_hex(0x0F1117), 0);
        } else {
            lv_obj_clear_flag(saver_orb1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(saver_orb2, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(saver_glass, lv_color_hex(0x141720), 0);
        }
    }

    bool current_media_active;
    bool current_soft_sleep;
    bool use_pc_monitor;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    current_media_active = media_active;
    current_soft_sleep = soft_sleep;
    use_pc_monitor = saver_source_pc_monitor;
    k_mutex_unlock(&lumi_ui_config_lock);

    bool force_show;
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    force_show = saver_force_show;
    k_mutex_unlock(&lumi_ui_config_lock);

    bool idle_trigger =
        !current_soft_sleep &&
        (force_show ||
         (enabled &&
          !current_media_active &&
          !pc_monitor_selected &&
          delay > 0U &&
          (uint32_t)(now_uptime - ui_last_activity_ms) >= delay));

    bool should_show_pc =
        idle_trigger && use_pc_monitor;

    bool should_show =
        idle_trigger &&
        !use_pc_monitor &&
        saver_media_valid;

    if (pc_monitor_saver_active != should_show_pc) {
        pc_monitor_saver_active = should_show_pc;
        update_pc_monitor_visibility();
    }

    if (should_show_pc && screensaver_visible) {
        screensaver_visible = false;
        lv_disp_enable_invalidation(NULL, true);
        if (root_screen) {
            lv_obj_invalidate(root_screen);
        }
    }

    if (should_show && !screensaver_visible) {
        screensaver_visible = true;

        /* The GIF path writes directly to the ST7789. While it is active,
         * stop LVGL from scheduling unrelated UI flushes on the same SPI bus.
         * Without this, a label/popup refresh can cut into a fast GIF frame
         * and create an extra tear line even though the GIF pacing is correct.
         */
        lv_disp_enable_invalidation(NULL, false);

        saver_media_index = 0U;
        saver_prefetch_valid = false;
        saver_prefetch_next_valid = false;
        saver_static_drawn = false;
        saver_media_epoch_ms = lv_tick_get();

        if (saver_media_format == SAVER_FORMAT_RYQ1) {
            saver_packed_restart_playback();
            (void)saver_packed_decode_next_frame(
                saver_media_epoch_ms);
        } else {
            draw_custom_saver_frame(0U, 0U);
        }
    } else if (!should_show && screensaver_visible) {
        screensaver_visible = false;
        saver_packed_playback_started = false;

        /* Hand display ownership back to LVGL and force one clean redraw of
         * the normal UI after direct GIF rendering stops.
         */
        lv_disp_enable_invalidation(NULL, true);

        if (root_screen) {
            lv_obj_invalidate(root_screen);
        }
    }

    if (sleep_overlay) {
        if (current_soft_sleep) {
            lv_obj_clear_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(sleep_overlay);
        } else {
            lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (should_show_pc) {
        return;
    }

    if (!screensaver_visible) {
        return;
    }

    if (saver_media_format == SAVER_FORMAT_RGB565_STATIC) {
        draw_custom_saver_frame(0U, 0U);
        return;
    }

    uint32_t lv_now = lv_tick_get();

    if (saver_media_format == SAVER_FORMAT_RYQ1) {
        if (!saver_packed_playback_started) {
            saver_packed_restart_playback();
            (void)saver_packed_decode_next_frame(lv_now);
            return;
        }

        if ((int32_t)(lv_now - saver_packed_next_frame_at) >= 0) {
            (void)saver_packed_decode_next_frame(lv_now);
        }
        return;
    }
    uint32_t elapsed = (uint32_t)(lv_now - saver_media_epoch_ms);
    uint32_t loop_ms = MAX(saver_media_loop_ms, 1U);
    uint32_t loop_pos = elapsed % loop_ms;

    /* Output is fixed at 25 Hz, while the stored source timeline keeps the
     * original loop duration. Blend toward the next stored frame on every
     * 40 ms tick so a long GIF is not reduced to visibly choppy 11 FPS motion.
     */
    uint8_t desired_index = 0U;
    uint32_t boundary = 0U;
    uint32_t frame_start = 0U;
    uint32_t frame_duration = SAVER_MIN_FRAME_MS;

    for (uint8_t i = 0U; i < saver_media_frame_count; i++) {
        frame_start = boundary;
        frame_duration = MAX(
            (uint32_t)saver_media_frame_intervals[i],
            (uint32_t)SAVER_MIN_FRAME_MS);
        boundary += frame_duration;
        desired_index = i;

        if (loop_pos < boundary) {
            break;
        }
    }

    uint32_t local_time =
        loop_pos >= frame_start
            ? loop_pos - frame_start
            : 0U;
    uint8_t blend = (uint8_t)MIN(
        255U,
        (local_time * 255U) / MAX(frame_duration, 1U));

    saver_media_index = desired_index;
    draw_custom_saver_frame(saver_media_index, blend);
}

bool lumi_ui_saver_anim_begin(uint8_t frame_count, uint16_t frame_interval_ms) {
    if (frame_count < 1U || frame_count > LUMI_SAVER_MAX_FRAMES) {
        return false;
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_media_valid = false;
    saver_media_format = SAVER_FORMAT_RGB332;
    saver_packed_reset_state();
    saver_media_frame_count = frame_count;
    saver_media_received_mask = 0U;
    saver_image_received_bytes = 0U;
    memset(saver_media_received_bytes, 0, sizeof(saver_media_received_bytes));
    saver_timing_set_uniform(
        frame_count,
        frame_interval_ms);
    saver_media_index = 0U;
    saver_media_epoch_ms = 0U;
    saver_prefetch_valid = false;
    saver_prefetch_next_valid = false;
    saver_static_drawn = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (saver_flash_prepare_upload() != 0) {
        saver_media_frame_count = 0U;
        return false;
    }

    return true;
}

bool lumi_ui_saver_anim_set_frame_interval(
    uint8_t index,
    uint16_t interval_ms) {

    if (index >= saver_media_frame_count ||
        index >= LUMI_SAVER_MAX_FRAMES) {
        return false;
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_media_frame_intervals[index] =
        CLAMP(interval_ms,
              (uint16_t)SAVER_MIN_FRAME_MS,
              (uint16_t)5000U);
    saver_timing_recalculate();
    k_mutex_unlock(&lumi_ui_config_lock);

    return true;
}

void lumi_ui_saver_anim_frame(uint8_t index, const uint8_t *data, size_t len) {
    if (!data ||
        index >= saver_media_frame_count ||
        index >= LUMI_SAVER_MAX_FRAMES ||
        len != LUMI_SAVER_FRAME_BYTES) {
        return;
    }

    if (saver_flash_write_chunk(index, 0U, data, len) != 0) {
        return;
    }

    saver_media_received_bytes[index] = LUMI_SAVER_FRAME_BYTES;
    saver_media_received_mask |= BIT(index);
}

bool lumi_ui_saver_anim_chunk(uint8_t index, uint16_t offset,
                              const uint8_t *data, size_t len) {
    if (!data ||
        index >= saver_media_frame_count ||
        index >= LUMI_SAVER_MAX_FRAMES ||
        offset >= LUMI_SAVER_FRAME_BYTES ||
        len == 0U ||
        (size_t)offset + len > LUMI_SAVER_FRAME_BYTES) {
        return false;
    }

    if (saver_flash_write_chunk(index, offset, data, len) != 0) {
        return false;
    }

    uint16_t end = (uint16_t)(offset + len);
    if (end > saver_media_received_bytes[index]) {
        saver_media_received_bytes[index] = end;
    }

    if (saver_media_received_bytes[index] == LUMI_SAVER_FRAME_BYTES) {
        saver_media_received_mask |= BIT(index);
    }

    return true;
}

bool lumi_ui_saver_anim_end(void) {
    uint32_t expected =
        saver_media_frame_count >= 32U
            ? UINT32_MAX
            : ((1U << saver_media_frame_count) - 1U);

    bool ok =
        saver_media_frame_count > 0U &&
        saver_media_received_mask == expected &&
        saver_flash_commit_header() == 0;

    if (ok) {
        saver_media_valid = true;
        saver_media_index = 0U;
        saver_media_epoch_ms = 0U;
        saver_prefetch_valid = false;
        saver_prefetch_next_valid = false;
        saver_static_drawn = false;
    } else {
        saver_media_valid = false;
    }

    return ok;
}

bool lumi_ui_saver_packed_begin(size_t total_bytes) {
    if (total_bytes < SAVER_PACKED_HEADER_BYTES ||
        total_bytes > SAVER_PACKED_MAX_BYTES) {
        return false;
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_media_valid = false;
    saver_media_format = SAVER_FORMAT_RYQ1;
    saver_media_frame_count = 0U;
    saver_media_received_mask = 0U;
    saver_image_received_bytes = 0U;
    saver_packed_reset_state();
    saver_packed_expected_bytes = (uint32_t)total_bytes;
    saver_packed_received_bytes = 0U;
    saver_static_drawn = false;
    saver_prefetch_valid = false;
    saver_prefetch_next_valid = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (saver_flash_prepare_upload() != 0) {
        saver_packed_reset_state();
        return false;
    }

    return true;
}

bool lumi_ui_saver_packed_chunk(
    uint32_t offset,
    const uint8_t *data,
    size_t len) {

    if (!data ||
        saver_media_format != SAVER_FORMAT_RYQ1 ||
        saver_packed_expected_bytes < SAVER_PACKED_HEADER_BYTES ||
        len == 0U ||
        offset != saver_packed_received_bytes ||
        (uint64_t)offset + len > saver_packed_expected_bytes) {
        return false;
    }

    if (saver_flash_write_bytes(offset, data, len) != 0) {
        return false;
    }

    saver_packed_received_bytes += (uint32_t)len;
    return true;
}

bool lumi_ui_saver_packed_end(void) {
    bool ok =
        saver_media_format == SAVER_FORMAT_RYQ1 &&
        saver_packed_expected_bytes >= SAVER_PACKED_HEADER_BYTES &&
        saver_packed_received_bytes == saver_packed_expected_bytes;

    if (ok) {
        saver_packed_data_size = saver_packed_expected_bytes;
        ok = saver_packed_load_header(
                 saver_packed_data_size,
                 true) &&
             saver_flash_commit_header() == 0;
    }

    if (ok) {
        saver_media_valid = true;
        saver_media_index = 0U;
        saver_media_epoch_ms = 0U;
        saver_prefetch_valid = false;
        saver_prefetch_next_valid = false;
        saver_static_drawn = false;
        saver_packed_cursor = saver_packed_frames_offset;
        saver_packed_frame_index = 0U;
        saver_packed_next_frame_at = 0U;
        saver_packed_playback_started = false;
    } else {
        saver_media_valid = false;
    }

    return ok;
}

bool lumi_ui_saver_image_begin(size_t total_bytes) {
    if (total_bytes != LUMI_SAVER_IMAGE_BYTES) {
        return false;
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_media_valid = false;
    saver_media_format = SAVER_FORMAT_RGB565_STATIC;
    saver_packed_reset_state();
    saver_media_frame_count = 1U;
    saver_media_received_mask = 0U;
    saver_image_received_bytes = 0U;
    saver_timing_set_uniform(1U, 1000U);
    saver_media_index = 0U;
    saver_media_epoch_ms = 0U;
    saver_prefetch_valid = false;
    saver_prefetch_next_valid = false;
    saver_static_drawn = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    return saver_flash_prepare_upload() == 0;
}

bool lumi_ui_saver_image_chunk(
    uint32_t offset,
    const uint8_t *data,
    size_t len) {

    if (!data ||
        saver_media_format != SAVER_FORMAT_RGB565_STATIC ||
        len == 0U ||
        offset != saver_image_received_bytes ||
        offset >= LUMI_SAVER_IMAGE_BYTES ||
        (uint64_t)offset + len > LUMI_SAVER_IMAGE_BYTES) {
        return false;
    }

    if (saver_flash_write_bytes(offset, data, len) != 0) {
        return false;
    }

    uint32_t end = offset + (uint32_t)len;
    if (end > saver_image_received_bytes) {
        saver_image_received_bytes = end;
    }

    return true;
}

bool lumi_ui_saver_image_end(void) {
    bool ok =
        saver_media_format == SAVER_FORMAT_RGB565_STATIC &&
        saver_image_received_bytes == LUMI_SAVER_IMAGE_BYTES &&
        saver_flash_commit_header() == 0;

    if (ok) {
        saver_media_valid = true;
        saver_media_frame_count = 1U;
        saver_media_index = 0U;
        saver_media_epoch_ms = 0U;
        saver_prefetch_valid = false;
        saver_prefetch_next_valid = false;
        saver_static_drawn = false;
    } else {
        saver_media_valid = false;
    }

    return ok;
}

bool lumi_ui_saver_anim_is_valid(void) {
    return saver_flash_load_metadata();
}

void lumi_ui_saver_anim_clear(void) {
    saver_flash_invalidate();
    saver_media_frame_count = 0U;
    saver_media_received_mask = 0U;
    saver_image_received_bytes = 0U;
    saver_media_format = SAVER_FORMAT_RGB332;
    saver_packed_reset_state();
    memset(saver_media_received_bytes, 0, sizeof(saver_media_received_bytes));
    saver_media_index = 0U;
    saver_prefetch_valid = false;
    saver_prefetch_next_valid = false;
    saver_static_drawn = false;
}

static void lumi_panel_refresh_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (sleep_overlay) {
        lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (root_screen) {
        lv_obj_invalidate(root_screen);
    }
}

K_WORK_DEFINE(lumi_panel_refresh_work, lumi_panel_refresh_work_handler);

static void lumi_ui_set_eco_timers(bool sleeping) {
    lv_timer_t *timers[] = {
        pressed_lv_timer,
        popup_lv_timer,
        pc_monitor_lv_timer,
        screensaver_lv_timer,
    };

    for (size_t i = 0U; i < ARRAY_SIZE(timers); i++) {
        if (!timers[i]) {
            continue;
        }

        if (sleeping) {
            lv_timer_pause(timers[i]);
        } else {
            lv_timer_resume(timers[i]);
        }
    }
}

static void lumi_panel_backlight_on_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    /* On cold boot the splash has already been drawn into ST7789 RAM while
     * BLK is LOW. Start the visible 1.5 s loading window exactly when BLK
     * turns on, so the user never sees the panel's random power-up RAM.
     */
    if (boot_overlay && boot_started_ms == 0U) {
        /* The visible splash timer begins when BLK actually turns on. */
    boot_started_ms = 0U;
    }

    (void)lumi_panel_set_backlight(true);
}

K_WORK_DELAYABLE_DEFINE(
    lumi_panel_backlight_on_work,
    lumi_panel_backlight_on_work_handler);

static void lumi_panel_sleep_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lumi_ui_set_eco_timers(true);
    (void)k_work_cancel_delayable(&lumi_panel_backlight_on_work);
    (void)lumi_panel_set_sleep(true);
}

K_WORK_DEFINE(lumi_panel_sleep_work, lumi_panel_sleep_work_handler);

static int lumi_panel_deep_sleep_rc;

static void lumi_panel_deep_sleep_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lumi_ui_set_eco_timers(true);
    (void)k_work_cancel_delayable(&lumi_panel_backlight_on_work);
    lumi_panel_deep_sleep_rc = lumi_panel_enter_deep_sleep();
}

K_WORK_DEFINE(lumi_panel_deep_sleep_work, lumi_panel_deep_sleep_work_handler);

static void lumi_panel_wake_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (lumi_panel_set_sleep(false) == 0) {
        lumi_ui_set_eco_timers(false);
        lumi_panel_refresh_work_handler(NULL);
        (void)k_work_reschedule(
            &page_poll_work,
            K_MSEC(250));
        (void)k_work_schedule(
            &lumi_panel_backlight_on_work,
            K_MSEC(80));
    }
}

K_WORK_DEFINE(lumi_panel_wake_work, lumi_panel_wake_work_handler);

static void lumi_ui_note_activity_internal(bool physical_key) {
    bool was_sleeping;
    bool resume_rgb;
    uint32_t now = k_uptime_get_32();

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    ui_last_activity_ms = now;
    deep_sleep_active = false;

    /* RGB uses its own idle timeout, but it shares the same explicit wake
     * sources as the display: physical input, Auto Profile changes, and
     * manual wake. Media metadata/playback updates never wake the device.
     */
    rgb_last_activity_ms = now;
    rgb_idle_suspended = false;

    was_sleeping = soft_sleep;
    soft_sleep = false;
    saver_force_show = false;
    resume_rgb = true;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (resume_rgb) {
        lumi_rgb_set_suspended(false);
    }

    if (was_sleeping) {
        lumi_diag_report(
            'I',
            physical_key
                ? "Wake requested by key/encoder"
                : "Wake requested by meaningful activity");
        k_work_submit_to_queue(zmk_display_work_q(), &lumi_panel_wake_work);
    }
}

void lumi_ui_note_activity(void) {
    lumi_ui_note_activity_internal(false);
}

void lumi_ui_note_key_activity(void) {
    lumi_ui_note_activity_internal(true);
}

void lumi_ui_show_screensaver_now(void) {
    bool use_pc_monitor;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    use_pc_monitor = saver_source_pc_monitor;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (!use_pc_monitor && !saver_flash_load_metadata()) {
        return;
    }

    bool was_sleeping;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    was_sleeping = soft_sleep;
    soft_sleep = false;
    saver_force_show = true;
    rgb_last_activity_ms = k_uptime_get_32();
    rgb_idle_suspended = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    lumi_rgb_set_suspended(false);

    if (was_sleeping) {
        k_work_submit(&lumi_panel_wake_work);
    }
}

void lumi_ui_set_media_active(bool active) {
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    media_active = active;
    k_mutex_unlock(&lumi_ui_config_lock);

    /* Media is display content, not user activity. Track changes, playback
     * transitions, artwork and metadata must never reset the sleep timer or
     * wake a device that has already entered soft sleep.
     */
}

void lumi_ui_set_wallpaper(uint8_t r1, uint8_t g1, uint8_t b1,
                           uint8_t r2, uint8_t g2, uint8_t b2) {
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    wallpaper_color_a = ((uint32_t)r1 << 16) | ((uint32_t)g1 << 8) | b1;
    wallpaper_color_b = ((uint32_t)r2 << 16) | ((uint32_t)g2 << 8) | b2;
    wallpaper_dirty = true;
    k_mutex_unlock(&lumi_ui_config_lock);
}

void lumi_ui_set_screensaver(bool enabled, uint8_t style,
                             uint32_t delay_seconds,
                             uint8_t r1, uint8_t g1, uint8_t b1,
                             uint8_t r2, uint8_t g2, uint8_t b2) {
    if (style > LUMI_SAVER_OFF) {
        style = LUMI_SAVER_TAHOE;
    }

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_enabled = enabled;
    saver_style = style;
    saver_delay_ms = delay_seconds * 1000U;
    saver_color_a = ((uint32_t)r1 << 16) | ((uint32_t)g1 << 8) | b1;
    saver_color_b = ((uint32_t)r2 << 16) | ((uint32_t)g2 << 8) | b2;
    saver_style_dirty = true;
    k_mutex_unlock(&lumi_ui_config_lock);
}

void lumi_ui_set_screensaver_delay(uint32_t seconds) {
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_delay_ms = seconds * 1000U;
    saver_enabled = seconds > 0U;
    k_mutex_unlock(&lumi_ui_config_lock);
}

void lumi_ui_set_screensaver_source(bool pc_monitor) {
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    saver_source_pc_monitor = pc_monitor;
    saver_force_show = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    pc_monitor_saver_active = false;
    update_pc_monitor_visibility();
}

void lumi_ui_set_sleep_timeout(uint32_t seconds) {
    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    sleep_delay_ms = seconds * 1000U;
    k_mutex_unlock(&lumi_ui_config_lock);
}

void lumi_ui_set_deep_sleep_timeout(uint32_t seconds) {
    /* Deep sleep stays in nRF52840 System ON so BLE remains connected.
     * Zero means Never. Limit the configurable range to seven days.
     */
    uint32_t clamped = MIN(seconds, 604800U);

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    deep_sleep_delay_ms = clamped * 1000U;

    /* If the threshold is moved into the future while already deep sleeping,
     * return to normal soft-sleep state without waking the display.
     */
    if (clamped == 0U ||
        (uint32_t)(k_uptime_get_32() - ui_last_activity_ms) <
            deep_sleep_delay_ms) {
        deep_sleep_active = false;
    }
    k_mutex_unlock(&lumi_ui_config_lock);
}

void lumi_ui_set_hibernate_timeout(uint32_t seconds) {
    /* Hibernate is the old System OFF path: lowest current, BLE disconnects,
     * and wake is a cold boot. Keep it disabled by default.
     */
    uint32_t clamped = MIN(seconds, 604800U);

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    hibernate_delay_ms = clamped * 1000U;
    k_mutex_unlock(&lumi_ui_config_lock);
}

bool lumi_ui_is_soft_sleeping(void) {
    bool sleeping;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    sleeping = soft_sleep;
    k_mutex_unlock(&lumi_ui_config_lock);

    return sleeping;
}

bool lumi_ui_is_deep_sleeping(void) {
    bool sleeping;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    sleeping = deep_sleep_active;
    k_mutex_unlock(&lumi_ui_config_lock);

    return sleeping;
}

void lumi_ui_set_rgb_idle_timeout(uint32_t seconds) {
    bool sleeping;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    rgb_idle_delay_ms = seconds * 1000U;
    sleeping = soft_sleep;

    if (seconds == 0U) {
        rgb_idle_suspended = false;
    }
    k_mutex_unlock(&lumi_ui_config_lock);

    if (seconds == 0U && !sleeping) {
        lumi_rgb_set_suspended(false);
    }
}

void lumi_ui_sleep_now(void) {
    bool already_sleeping;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    already_sleeping = soft_sleep;
    soft_sleep = true;
    saver_force_show = false;
    k_mutex_unlock(&lumi_ui_config_lock);

    if (already_sleeping) {
        return;
    }

    lumi_diag_report('I', "Manual soft sleep requested");
    lumi_rgb_set_suspended(true);
    k_work_submit_to_queue(zmk_display_work_q(), &lumi_panel_sleep_work);
}

void lumi_ui_wake_now(void) {
    /* Reuse the normal activity wake path so panel, RGB, idle timers and
     * screensaver state are restored exactly like a physical key wake.
     */
    lumi_diag_report('I', "Manual wake requested");
    lumi_ui_note_activity();
}

static void lumi_sleep_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(lumi_sleep_work, lumi_sleep_work_handler);

static bool lumi_usb_power_present(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void lumi_sleep_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint32_t soft_timeout;
    uint32_t deep_timeout;
    uint32_t hibernate_timeout;
    uint32_t rgb_timeout;
    uint32_t last_activity;
    uint32_t last_rgb_activity;
    bool already_sleeping;
    bool already_deep;
    bool rgb_timed_out;

    k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
    soft_timeout = sleep_delay_ms;
    deep_timeout = deep_sleep_delay_ms;
    hibernate_timeout = hibernate_delay_ms;
    rgb_timeout = rgb_idle_delay_ms;
    last_activity = ui_last_activity_ms;
    last_rgb_activity = rgb_last_activity_ms;
    already_sleeping = soft_sleep;
    already_deep = deep_sleep_active;
    rgb_timed_out = rgb_idle_suspended;
    k_mutex_unlock(&lumi_ui_config_lock);

    uint32_t now = k_uptime_get_32();
    uint32_t idle_ms = (uint32_t)(now - last_activity);

    if (rgb_timeout > 0U &&
        !rgb_timed_out &&
        (uint32_t)(now - last_rgb_activity) >= rgb_timeout) {

        k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
        rgb_idle_suspended = true;
        k_mutex_unlock(&lumi_ui_config_lock);

        lumi_diag_report(
            'I',
            "RGB idle timeout=%us",
            (unsigned int)(rgb_timeout / 1000U));
        lumi_rgb_set_suspended(true);
        rgb_timed_out = true;
    }

    /* Stage 1: soft sleep. BLE HID remains connected, while panel/RGB and
     * display work are stopped.
     */
    if (soft_timeout > 0U &&
        !already_sleeping &&
        idle_ms >= soft_timeout) {

        k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
        soft_sleep = true;
        k_mutex_unlock(&lumi_ui_config_lock);

        lumi_diag_report(
            'I',
            "Entering BLE eco sleep timeout=%us",
            (unsigned int)(soft_timeout / 1000U));
        lumi_rgb_set_suspended(true);
        k_work_submit_to_queue(zmk_display_work_q(), &lumi_panel_sleep_work);
        already_sleeping = true;
    }

    /* Stage 2: connected deep sleep. Do NOT call zmk_pm_soft_off(). Zephyr
     * naturally idles the nRF52840 in System ON between BLE/GPIO events, so
     * the BLE connection survives and a key/encoder event resumes immediately.
     *
     * Panel/RGB are already in their lowest practical connected state from
     * soft sleep. Marking DEEP lets LumiPad reduce GATT polling even further.
     */
    if (deep_timeout > 0U &&
        !already_deep &&
        idle_ms >= deep_timeout) {

        k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
        soft_sleep = true;
        deep_sleep_active = true;
        k_mutex_unlock(&lumi_ui_config_lock);

        lumi_diag_report(
            'I',
            "Entering connected deep sleep timeout=%us",
            (unsigned int)(deep_timeout / 1000U));

        lumi_rgb_set_suspended(true);

        if (!already_sleeping) {
            k_work_submit_to_queue(
                zmk_display_work_q(),
                &lumi_panel_sleep_work);
            already_sleeping = true;
        }

        already_deep = true;
    }

    /* Stage 3: optional Hibernate. This is the previous deep-sleep behavior:
     * devices are suspended and the nRF52840 enters System OFF. Bluetooth is
     * intentionally lost and wake performs a cold boot. Default is Never.
     */
    if (hibernate_timeout > 0U &&
        !lumi_usb_power_present() &&
        idle_ms >= hibernate_timeout) {

        k_mutex_lock(&lumi_ui_config_lock, K_FOREVER);
        soft_sleep = true;
        deep_sleep_active = true;
        k_mutex_unlock(&lumi_ui_config_lock);

        lumi_diag_report(
            'I',
            "Entering hibernate timeout=%us",
            (unsigned int)(hibernate_timeout / 1000U));

        lumi_rgb_set_suspended(true);

        struct k_work_sync deep_panel_sync;
        lumi_panel_deep_sleep_rc = 0;
        k_work_submit_to_queue(
            zmk_display_work_q(),
            &lumi_panel_deep_sleep_work);
        (void)k_work_flush(
            &lumi_panel_deep_sleep_work,
            &deep_panel_sync);

        if (lumi_panel_deep_sleep_rc < 0) {
            lumi_diag_report(
                'E',
                "Hibernate panel shutdown failed rc=%d",
                lumi_panel_deep_sleep_rc);
        }

        int rc = zmk_pm_soft_off();
        if (rc < 0) {
            lumi_diag_report(
                'E',
                "Hibernate soft-off failed rc=%d",
                rc);
        }
    }

    /* Awake checks stay responsive. Soft sleep checks every 30 s. Connected
     * deep sleep wakes this maintenance work only every 5 min unless a pending
     * Hibernate deadline is closer. Physical GPIO/encoder wake does not wait
     * for this timer; its ZMK event calls lumi_ui_note_key_activity directly.
     */
    uint32_t next_ms =
        already_deep
            ? 300000U
            : (already_sleeping ? 30000U : 1000U);

    if (deep_timeout > 0U &&
        !already_deep &&
        idle_ms < deep_timeout) {
        uint32_t remaining = deep_timeout - idle_ms;
        if (remaining < next_ms) {
            next_ms = MAX(remaining, 250U);
        }
    }

    if (hibernate_timeout > 0U &&
        !lumi_usb_power_present() &&
        idle_ms < hibernate_timeout) {
        uint32_t remaining = hibernate_timeout - idle_ms;
        if (remaining < next_ms) {
            next_ms = MAX(remaining, 250U);
        }
    }

    k_work_reschedule(&lumi_sleep_work, K_MSEC(next_ms));
}

static lv_obj_t *pc_monitor_label(
    lv_obj_t *parent,
    const char *text,
    const lv_font_t *font,
    uint32_t color) {

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0);
    return label;
}

static void pc_monitor_card(
    lv_obj_t *parent,
    int32_t x,
    const char *title,
    uint32_t accent_color,
    lv_obj_t **title_out,
    lv_obj_t **value_out,
    lv_obj_t **meta_out) {

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, 8);
    lv_obj_set_size(card, 96, 82);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111317), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2D3138), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    *title_out =
        pc_monitor_label(
            card,
            title,
            &lv_font_montserrat_12,
            accent_color);
    lv_obj_set_pos(*title_out, 9, 6);

    *value_out =
        pc_monitor_label(
            card,
            "--%",
            &lv_font_montserrat_20,
            0xFFFFFF);
    lv_obj_set_pos(*value_out, 9, 26);

    *meta_out =
        pc_monitor_label(
            card,
            "--",
            &lv_font_montserrat_12,
            0x8E8E93);
    lv_obj_set_pos(*meta_out, 9, 57);
    lv_obj_set_width(*meta_out, 80);
    lv_label_set_long_mode(
        *meta_out,
        LV_LABEL_LONG_DOT);
}

static void init_pc_monitor(lv_obj_t *screen) {
    pc_monitor_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(pc_monitor_overlay);
    lv_obj_set_pos(pc_monitor_overlay, 0, STATUS_H);
    lv_obj_set_size(pc_monitor_overlay, 320, 172 - STATUS_H);
    lv_obj_set_style_bg_color(
        pc_monitor_overlay,
        lv_color_hex(0x050608),
        0);
    lv_obj_set_style_bg_opa(
        pc_monitor_overlay,
        LV_OPA_COVER,
        0);
    lv_obj_clear_flag(
        pc_monitor_overlay,
        LV_OBJ_FLAG_SCROLLABLE);

    pc_monitor_card(
        pc_monitor_overlay,
        8,
        "CPU USE",
        0x64D2FF,
        &pc_cpu_title,
        &pc_cpu_value,
        &pc_cpu_meta);
    pc_monitor_card(
        pc_monitor_overlay,
        112,
        "GPU USE",
        0xBF5AF2,
        &pc_gpu_title,
        &pc_gpu_value,
        &pc_gpu_meta);
    pc_monitor_card(
        pc_monitor_overlay,
        216,
        "RAM USE",
        0x30D158,
        &pc_ram_title,
        &pc_ram_value,
        &pc_ram_meta);

    lv_obj_t *network = lv_obj_create(pc_monitor_overlay);
    lv_obj_remove_style_all(network);
    lv_obj_set_pos(network, 8, 98);
    lv_obj_set_size(network, 304, 41);
    lv_obj_set_style_bg_color(network, lv_color_hex(0x111317), 0);
    lv_obj_set_style_bg_opa(network, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(network, 1, 0);
    lv_obj_set_style_border_color(network, lv_color_hex(0x2D3138), 0);
    lv_obj_set_style_radius(network, 10, 0);
    lv_obj_clear_flag(network, LV_OBJ_FLAG_SCROLLABLE);

    pc_net_down =
        pc_monitor_label(
            network,
            "DOWN --",
            &lv_font_montserrat_16,
            0x64D2FF);
    lv_obj_set_pos(pc_net_down, 0, 10);
    lv_obj_set_width(pc_net_down, 101);
    lv_obj_set_style_text_align(
        pc_net_down,
        LV_TEXT_ALIGN_CENTER,
        0);

    pc_net_up =
        pc_monitor_label(
            network,
            "UP --",
            &lv_font_montserrat_16,
            0xBF5AF2);
    lv_obj_set_pos(pc_net_up, 101, 10);
    lv_obj_set_width(pc_net_up, 101);
    lv_obj_set_style_text_align(
        pc_net_up,
        LV_TEXT_ALIGN_CENTER,
        0);

    pc_fps_value =
        pc_monitor_label(
            network,
            "FPS --",
            &lv_font_montserrat_16,
            0x30D158);
    lv_obj_set_pos(pc_fps_value, 202, 10);
    lv_obj_set_width(pc_fps_value, 101);
    lv_obj_set_style_text_align(
        pc_fps_value,
        LV_TEXT_ALIGN_CENTER,
        0);

    pc_monitor_status = NULL;

    lv_obj_add_flag(
        pc_monitor_overlay,
        LV_OBJ_FLAG_HIDDEN);
}

static void refresh_pc_monitor_timer(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    if (pc_monitor_selected || pc_monitor_saver_active) {
        refresh_pc_monitor_labels();
    }
}

static bool detect_low_power_wake(void) {
    uint32_t cause = 0U;

    if (hwinfo_get_reset_cause(&cause) != 0) {
        return false;
    }

    bool woke_from_system_off =
        (cause & RESET_LOW_POWER_WAKE) != 0U;

    /* Clear the sticky OFF reason so a later software/reset-button reboot
     * returns to the normal boot splash instead of being mistaken for another
     * deep-sleep wake.
     */
    (void)hwinfo_clear_reset_cause();

    return woke_from_system_off;
}

static void refresh_boot_splash(lv_timer_t *timer) {
    if (!boot_overlay || !boot_progress) {
        lv_timer_del(timer);
        return;
    }

    if (boot_started_ms == 0U) {
        lv_bar_set_value(
            boot_progress,
            0,
            LV_ANIM_OFF);
        return;
    }

    uint32_t elapsed =
        (uint32_t)(lv_tick_get() - boot_started_ms);
    uint32_t progress =
        MIN(
            100U,
            (elapsed * 100U) /
                BOOT_SPLASH_VISIBLE_MS);

    lv_bar_set_value(
        boot_progress,
        (int32_t)progress,
        LV_ANIM_OFF);

    if (elapsed < BOOT_SPLASH_VISIBLE_MS) {
        return;
    }

    lv_obj_del(boot_overlay);
    boot_overlay = NULL;
    boot_progress = NULL;
    lv_timer_del(timer);
}

static void init_boot_splash(lv_obj_t *screen) {
    ARG_UNUSED(screen);
    boot_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(boot_overlay);
    lv_obj_set_pos(boot_overlay, 0, 0);
    lv_obj_set_size(boot_overlay, 320, 172);
    lv_obj_set_style_bg_color(
        boot_overlay,
        lv_color_hex(0x000000),
        0);
    lv_obj_set_style_bg_opa(
        boot_overlay,
        LV_OPA_COVER,
        0);
    lv_obj_clear_flag(
        boot_overlay,
        LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *product =
        make_label(
            boot_overlay,
            &lv_font_montserrat_20);
    lv_label_set_text(product, "RYNOR ONE");
    lv_obj_set_style_text_color(
        product,
        lv_color_hex(0xFFFFFF),
        0);
    lv_obj_set_style_text_letter_space(
        product,
        2,
        0);
    lv_obj_align(
        product,
        LV_ALIGN_CENTER,
        0,
        -30);

    lv_obj_t *designed =
        make_label(
            boot_overlay,
            &lv_font_montserrat_12);
    lv_label_set_text(designed, "Designed by Lumi3D");
    lv_obj_set_style_text_color(
        designed,
        lv_color_hex(0xA8A8AD),
        0);
    lv_obj_align(
        designed,
        LV_ALIGN_CENTER,
        0,
        -4);

    boot_progress = lv_bar_create(boot_overlay);
    lv_obj_set_size(
        boot_progress,
        116,
        4);
    lv_obj_align(
        boot_progress,
        LV_ALIGN_CENTER,
        0,
        33);
    lv_bar_set_range(
        boot_progress,
        0,
        100);
    lv_bar_set_value(
        boot_progress,
        0,
        LV_ANIM_OFF);

    lv_obj_set_style_bg_color(
        boot_progress,
        lv_color_hex(0x303034),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        boot_progress,
        LV_OPA_COVER,
        LV_PART_MAIN);
    lv_obj_set_style_radius(
        boot_progress,
        2,
        LV_PART_MAIN);

    lv_obj_set_style_bg_color(
        boot_progress,
        lv_color_hex(0xF5F5F7),
        LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(
        boot_progress,
        LV_OPA_COVER,
        LV_PART_INDICATOR);
    lv_obj_set_style_radius(
        boot_progress,
        2,
        LV_PART_INDICATOR);

    boot_started_ms = lv_tick_get();
    lv_timer_create(
        refresh_boot_splash,
        40,
        NULL);
}

lv_obj_t *zmk_display_status_screen(void) {
    boot_low_power_wake = detect_low_power_wake();

    (void)lumi_panel_init();
    (void)saver_flash_load_metadata();
    lv_obj_t *screen = lv_obj_create(NULL);
    root_screen = screen;
    ui_last_activity_ms = k_uptime_get_32();
    rgb_last_activity_ms = ui_last_activity_ms;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, 320, 172);
    lv_obj_set_style_bg_color(screen, lv_color_hex(wallpaper_color_a), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(wallpaper_color_b), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Cold power-on keeps the normal loading splash. A GPIO wake from
     * nRF52840 System OFF skips it completely so the very first wake event
     * produces a visible UI as soon as the display is ready.
     */
    if (!boot_low_power_wake) {
        init_boot_splash(screen);
    }

    for (uint8_t i = 0; i < KEY_COUNT; i++) {
        tiles[i] = lv_obj_create(screen);
        lv_obj_remove_style_all(tiles[i]);
        uint8_t col = i % COLS;
uint8_t row = i / COLS;

lv_obj_set_pos(
    tiles[i],
    col * CELL_W,
    STATUS_H + row * CELL_H
);
        lv_obj_set_size(tiles[i], CELL_W, CELL_H);
        lv_obj_set_style_bg_opa(tiles[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tiles[i], 1, 0);
lv_obj_set_style_border_color(
    tiles[i],
    lv_color_hex(0xD8F8FF),
    0
);

/* Chỉ kẻ đường chia bên trong.
 * Không kẻ viền ngoài màn hình.
 */
lv_border_side_t side = LV_BORDER_SIDE_NONE;

if (col < COLS - 1) {
    side |= LV_BORDER_SIDE_RIGHT;
}

if (row < 2) {
    side |= LV_BORDER_SIDE_BOTTOM;
}

lv_obj_set_style_border_side(
    tiles[i],
    side,
    0
);
        lv_obj_clear_flag(tiles[i], LV_OBJ_FLAG_SCROLLABLE);
        icon_colors[i] = 0xFFFFFF;
        icons[i] = make_label(tiles[i], &lv_font_montserrat_20);
        captions[i] = make_label(tiles[i], &lv_font_montserrat_12);
        lv_obj_add_flag(
    captions[i],
    LV_OBJ_FLAG_HIDDEN
);
        lv_obj_set_width(captions[i], CELL_W - 4);
        lv_label_set_long_mode(captions[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(captions[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(captions[i], LV_ALIGN_BOTTOM_MID, 0, -3);
    }
    /* Dedicated 28px footer, outside the 144px grid. */
    /* Top status bar */
/* Top status bar */

layer_label = make_label(
    screen,
    &lv_font_montserrat_14
);

lv_obj_set_style_text_letter_space(
    layer_label,
    -1,
    0
);

lv_obj_set_pos(
    layer_label,
    10,
    6
);

lv_obj_set_width(
    layer_label,
    125
);


/* Output */

output_label = make_label(
    screen,
    &lv_font_montserrat_14
);

lv_obj_set_style_text_letter_space(
    output_label,
    -1,
    0
);

lv_obj_set_pos(
    output_label,
    135,
    6
);

lv_obj_set_width(
    output_label,
    100
);

lumi_output_init();


/* Battery: use RYNOR's filtered/persisted estimator instead of the
 * raw ZMK single-sample VDDH percentage so reboot/load transients never make
 * the status bar jump by large amounts.
 */

battery_label = lv_label_create(screen);
lv_label_set_text(
    battery_label,
    lumi_battery_ready() ? "0%" : "--%"
);

if (lumi_battery_ready()) {
    char battery_text[12];
    snprintf(
        battery_text,
        sizeof(battery_text),
        "%u%%",
        (unsigned int)lumi_battery_percent());
    lv_label_set_text(battery_label, battery_text);
}

lv_obj_set_style_text_font(
    battery_label,
    &lv_font_montserrat_14,
    0
);

lv_obj_set_style_text_letter_space(
    battery_label,
    1,
    0
);

lv_obj_set_style_text_color(
    battery_label,
    lv_color_white(),
    0
);

lv_obj_set_width(
    battery_label,
    82
);

lv_obj_set_style_text_align(
    battery_label,
    LV_TEXT_ALIGN_RIGHT,
    0
);

lv_obj_set_pos(
    battery_label,
    226,
    6
);

popup = lv_obj_create(screen);
lv_obj_remove_style_all(popup);

/* Pill nhỏ nằm sát phía dưới */
lv_obj_set_pos(popup, 70, 172);
lv_obj_set_size(popup, 180, 44);

/* Nền tối hơi trong */
lv_obj_set_style_bg_color(
    popup,
    lv_color_hex(0x151719),
    0
);

lv_obj_set_style_bg_opa(
    popup,
    235,
    0
);

/* Bo tròn kiểu capsule */
lv_obj_set_style_radius(
    popup,
    22,
    0
);

/* Viền rất nhẹ */
lv_obj_set_style_border_width(
    popup,
    1,
    0
);

lv_obj_set_style_border_color(
    popup,
    lv_color_hex(0xFFFFFF),
    0
);

lv_obj_set_style_border_opa(
    popup,
    55,
    0
);

/* Không cho object bắt touch/click */
lv_obj_clear_flag(
    popup,
    LV_OBJ_FLAG_CLICKABLE
);

/* ICON */
popup_icon = make_label(
    popup,
    &lv_font_montserrat_20
);

lv_obj_set_style_text_color(
    popup_icon,
    lv_color_hex(0xFFFFFF),
    0
);

lv_obj_align(
    popup_icon,
    LV_ALIGN_LEFT_MID,
    14,
    0
);

/* TEXT */
popup_text = make_label(
    popup,
    &lv_font_montserrat_16
);

lv_obj_set_style_text_color(
    popup_text,
    lv_color_hex(0xFFFFFF),
    0
);

lv_obj_set_style_text_letter_space(
    popup_text,
    -1,
    0
);

lv_obj_align(
    popup_text,
    LV_ALIGN_LEFT_MID,
    48,
    0
);

lv_obj_add_flag(
    popup,
    LV_OBJ_FLAG_HIDDEN
);

    init_pc_monitor(screen);
    lumi_page_init();
    lumi_now_playing_init(screen);
    init_screensaver(screen);

    sleep_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(sleep_overlay);
    lv_obj_set_pos(sleep_overlay, 0, 0);
    lv_obj_set_size(sleep_overlay, 320, 172);
    lv_obj_set_style_bg_color(sleep_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);

k_work_schedule(&page_poll_work, K_MSEC(500));

pressed_lv_timer = lv_timer_create(refresh_pressed, 20, NULL);
popup_lv_timer = lv_timer_create(refresh_popup, 20, NULL);
pc_monitor_lv_timer = lv_timer_create(refresh_pc_monitor_timer, 500, NULL);
screensaver_lv_timer = lv_timer_create(
    refresh_screensaver,
    SAVER_MIN_FRAME_MS,
    NULL);

k_work_schedule(&lumi_sleep_work, K_SECONDS(1));

/* Cold boot keeps the existing splash timing. Deep-sleep GPIO wake skips the
 * splash and turns BLK on almost immediately after the main UI is built.
 */
(void)k_work_schedule(
    &lumi_panel_backlight_on_work,
    K_MSEC(
        boot_low_power_wake
            ? BOOT_WAKE_BACKLIGHT_DELAY_MS
            : BOOT_BACKLIGHT_DELAY_MS));

return screen;
}
