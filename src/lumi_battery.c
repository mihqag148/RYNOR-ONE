/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/usb.h>

#include "lumi_battery.h"
#include "lumi_diag.h"
#include "lumi_ui_config.h"

#define LUMI_BATTERY_SETTINGS_VERSION 1U
#define LUMI_BATTERY_BOOT_DELAY_MS 5000U
#define LUMI_BATTERY_AWAKE_PERIOD_MS 60000U
#define LUMI_BATTERY_SLEEP_PERIOD_MS 300000U
#define LUMI_BATTERY_SAMPLE_COUNT 9U
#define LUMI_BATTERY_SAMPLE_GAP_MS 35U
#define LUMI_BATTERY_SAVE_MIN_DELTA 2U
#define LUMI_BATTERY_CHARGE_STEP_MS 120000U
#define LUMI_BATTERY_DISCHARGE_STEP_MS 300000U

static const struct device *const battery_sensor =
    DEVICE_DT_GET(DT_CHOSEN(zmk_battery));

struct lumi_battery_saved_state {
    uint8_t version;
    uint8_t percent;
    uint16_t millivolts;
};

struct lumi_battery_curve_point {
    uint16_t millivolts;
    uint8_t percent;
};

/* Approximate resting-voltage curve for a single-cell Li-Po.
 * Capacity (1200 mAh) does not change voltage-to-SOC mapping; it is only used
 * indirectly to keep the charging ramp realistic for the configured 300 mA
 * charge rate (~25%/hour before taper).
 */
static const struct lumi_battery_curve_point battery_curve[] = {
    {3450,   0},
    {3610,   5},
    {3690,  10},
    {3710,  15},
    {3730,  20},
    {3750,  25},
    {3770,  30},
    {3790,  35},
    {3800,  40},
    {3820,  45},
    {3840,  50},
    {3850,  55},
    {3870,  60},
    {3910,  65},
    {3950,  70},
    {3980,  75},
    {4020,  80},
    {4080,  85},
    {4110,  90},
    {4150,  95},
    {4200, 100},
};

K_MUTEX_DEFINE(lumi_battery_lock);

static uint8_t stable_percent;
static uint16_t filtered_mv;
static bool stable_ready;
static bool loaded_from_settings;
static uint8_t last_saved_percent = 0xFFU;
static uint32_t last_percent_change_ms;

static void lumi_battery_measure_work_handler(struct k_work *work);
static void lumi_battery_save_work_handler(struct k_work *work);
static void lumi_battery_bas_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(
    lumi_battery_measure_work,
    lumi_battery_measure_work_handler);
K_WORK_DELAYABLE_DEFINE(
    lumi_battery_save_work,
    lumi_battery_save_work_handler);
K_WORK_DELAYABLE_DEFINE(
    lumi_battery_bas_work,
    lumi_battery_bas_work_handler);

static uint8_t voltage_to_percent(uint16_t millivolts) {
    if (millivolts <= battery_curve[0].millivolts) {
        return battery_curve[0].percent;
    }

    for (size_t i = 1U; i < ARRAY_SIZE(battery_curve); i++) {
        const struct lumi_battery_curve_point *lo = &battery_curve[i - 1U];
        const struct lumi_battery_curve_point *hi = &battery_curve[i];

        if (millivolts <= hi->millivolts) {
            uint32_t mv_span = (uint32_t)hi->millivolts - lo->millivolts;
            uint32_t pct_span = (uint32_t)hi->percent - lo->percent;
            uint32_t mv_into = (uint32_t)millivolts - lo->millivolts;

            return (uint8_t)(
                lo->percent +
                ((mv_into * pct_span + (mv_span / 2U)) / mv_span));
        }
    }

    return 100U;
}

static int read_battery_mv_once(uint16_t *millivolts) {
    if (!millivolts || !device_is_ready(battery_sensor)) {
        return -ENODEV;
    }

    int rc = sensor_sample_fetch_chan(
        battery_sensor,
        SENSOR_CHAN_GAUGE_VOLTAGE);
    if (rc != 0) {
        return rc;
    }

    struct sensor_value voltage;
    rc = sensor_channel_get(
        battery_sensor,
        SENSOR_CHAN_GAUGE_VOLTAGE,
        &voltage);
    if (rc != 0) {
        return rc;
    }

    int32_t mv =
        (voltage.val1 * 1000) +
        (voltage.val2 / 1000);

    if (mv < 2500 || mv > 5000) {
        return -ERANGE;
    }

    *millivolts = (uint16_t)mv;
    return 0;
}

static void sort_u16(uint16_t *values, size_t count) {
    for (size_t i = 1U; i < count; i++) {
        uint16_t value = values[i];
        size_t j = i;

        while (j > 0U && values[j - 1U] > value) {
            values[j] = values[j - 1U];
            j--;
        }

        values[j] = value;
    }
}

static int read_trimmed_battery_mv(uint16_t *millivolts) {
    uint16_t samples[LUMI_BATTERY_SAMPLE_COUNT];
    size_t valid = 0U;

    for (size_t i = 0U; i < LUMI_BATTERY_SAMPLE_COUNT; i++) {
        uint16_t mv = 0U;
        if (read_battery_mv_once(&mv) == 0) {
            samples[valid++] = mv;
        }

        if (i + 1U < LUMI_BATTERY_SAMPLE_COUNT) {
            k_msleep(LUMI_BATTERY_SAMPLE_GAP_MS);
        }
    }

    if (valid < 5U) {
        return -EIO;
    }

    sort_u16(samples, valid);

    /* Drop one high and one low sample, then average the middle values. */
    uint32_t sum = 0U;
    for (size_t i = 1U; i + 1U < valid; i++) {
        sum += samples[i];
    }

    size_t averaged = valid - 2U;
    *millivolts =
        (uint16_t)((sum + (averaged / 2U)) / averaged);
    return 0;
}

static void publish_bas_level(uint8_t percent) {
#if IS_ENABLED(CONFIG_BT_BAS)
    (void)bt_bas_set_battery_level(percent);
#else
    ARG_UNUSED(percent);
#endif
}

static void schedule_persist_if_needed(uint8_t percent) {
#if IS_ENABLED(CONFIG_SETTINGS)
    if (last_saved_percent == 0xFFU ||
        ABS((int)percent - (int)last_saved_percent) >=
            LUMI_BATTERY_SAVE_MIN_DELTA) {
        (void)k_work_reschedule(
            &lumi_battery_save_work,
            K_SECONDS(30));
    }
#else
    ARG_UNUSED(percent);
#endif
}

static void update_stable_estimate(uint16_t measured_mv) {
    uint8_t old_percent;
    uint8_t new_percent;
    uint16_t new_filtered_mv;
    bool charging = zmk_usb_is_powered();
    uint32_t now = k_uptime_get_32();

    k_mutex_lock(&lumi_battery_lock, K_FOREVER);

    old_percent = stable_percent;

    if (!stable_ready) {
        filtered_mv = measured_mv;
        stable_percent = voltage_to_percent(measured_mv);
        stable_ready = true;
        last_percent_change_ms = now;
    } else {
        /* Voltage EMA removes load spikes without hiding a genuine trend. */
        if (filtered_mv == 0U) {
            filtered_mv = measured_mv;
        } else {
            filtered_mv =
                (uint16_t)(
                    (((uint32_t)filtered_mv * 7U) +
                     measured_mv + 4U) /
                    8U);
        }

        uint8_t target = voltage_to_percent(filtered_mv);
        uint32_t since_change =
            (uint32_t)(now - last_percent_change_ms);

        if (filtered_mv <= 3500U) {
            stable_percent = MIN(stable_percent, 3U);
            last_percent_change_ms = now;
        } else if (charging) {
            /* 300 mA into 1200 mAh is about 25%/hour before taper. Limit the
             * visible rise to 1% every two minutes so charge voltage does not
             * make the gauge race to 100%.
             */
            if (target > stable_percent &&
                since_change >= LUMI_BATTERY_CHARGE_STEP_MS) {
                stable_percent++;
                last_percent_change_ms = now;
            }
        } else {
            /* A battery that is not charging must not gain charge just because
             * voltage rebounds after reboot or after a heavy display/RGB load.
             * Let the gauge fall at most 1% every five minutes.
             */
            if (target < stable_percent &&
                since_change >= LUMI_BATTERY_DISCHARGE_STEP_MS) {
                stable_percent--;
                last_percent_change_ms = now;
            }
        }
    }

    new_percent = stable_percent;
    new_filtered_mv = filtered_mv;

    k_mutex_unlock(&lumi_battery_lock);

    publish_bas_level(new_percent);

    if (new_percent != old_percent) {
        schedule_persist_if_needed(new_percent);
        lumi_diag_report(
            'I',
            "Battery stable=%u%% filtered=%umV measured=%umV",
            (unsigned int)new_percent,
            (unsigned int)new_filtered_mv,
            (unsigned int)measured_mv);
    }
}

static void lumi_battery_measure_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (lumi_ui_is_soft_sleeping()) {
        (void)k_work_reschedule(
            &lumi_battery_measure_work,
            K_MSEC(LUMI_BATTERY_SLEEP_PERIOD_MS));
        return;
    }

    uint16_t measured_mv = 0U;
    int rc = read_trimmed_battery_mv(&measured_mv);

    if (rc == 0) {
        update_stable_estimate(measured_mv);
    } else {
        lumi_diag_report(
            'W',
            "Battery filtered sample failed rc=%d",
            rc);
    }

    (void)k_work_reschedule(
        &lumi_battery_measure_work,
        K_MSEC(LUMI_BATTERY_AWAKE_PERIOD_MS));
}

static void lumi_battery_save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_SETTINGS)
    struct lumi_battery_saved_state state;

    k_mutex_lock(&lumi_battery_lock, K_FOREVER);
    if (!stable_ready) {
        k_mutex_unlock(&lumi_battery_lock);
        return;
    }

    state.version = LUMI_BATTERY_SETTINGS_VERSION;
    state.percent = stable_percent;
    state.millivolts = filtered_mv;
    k_mutex_unlock(&lumi_battery_lock);

    int rc = settings_save_one(
        "lumi_batt/state",
        &state,
        sizeof(state));

    if (rc == 0) {
        last_saved_percent = state.percent;
    } else {
        lumi_diag_report(
            'W',
            "Battery NVS save failed rc=%d",
            rc);
    }
#endif
}

static void lumi_battery_bas_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    publish_bas_level(lumi_battery_percent());
}

static int lumi_battery_raw_event_listener(const zmk_event_t *eh) {
    if (!as_zmk_battery_state_changed(eh)) {
        return -ENOTSUP;
    }

    /* ZMK writes its raw single-sample value to BAS after raising the battery
     * event. Re-apply the filtered RYNOR value just after that update so the
     * Windows battery indicator matches the device screen and LumiPad.
     */
    (void)k_work_reschedule(
        &lumi_battery_bas_work,
        K_MSEC(100));

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(
    lumi_battery_raw,
    lumi_battery_raw_event_listener);
ZMK_SUBSCRIPTION(
    lumi_battery_raw,
    zmk_battery_state_changed);

#if IS_ENABLED(CONFIG_SETTINGS)
static int lumi_battery_settings_set(
    const char *name,
    size_t len,
    settings_read_cb read_cb,
    void *cb_arg) {

    const char *next;
    if (!settings_name_steq(name, "state", &next) || next) {
        return -ENOENT;
    }

    if (len != sizeof(struct lumi_battery_saved_state)) {
        return -EINVAL;
    }

    struct lumi_battery_saved_state state;
    int rc = read_cb(cb_arg, &state, sizeof(state));
    if (rc < 0) {
        return rc;
    }

    if (state.version != LUMI_BATTERY_SETTINGS_VERSION ||
        state.percent > 100U ||
        state.millivolts < 3000U ||
        state.millivolts > 4500U) {
        return -EINVAL;
    }

    k_mutex_lock(&lumi_battery_lock, K_FOREVER);
    stable_percent = state.percent;
    filtered_mv = state.millivolts;
    stable_ready = true;
    loaded_from_settings = true;
    last_saved_percent = state.percent;
    last_percent_change_ms = k_uptime_get_32();
    k_mutex_unlock(&lumi_battery_lock);

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(
    lumi_battery,
    "lumi_batt",
    NULL,
    lumi_battery_settings_set,
    NULL,
    NULL);
#endif

uint8_t lumi_battery_percent(void) {
    uint8_t percent;
    bool ready;

    k_mutex_lock(&lumi_battery_lock, K_FOREVER);
    percent = stable_percent;
    ready = stable_ready;
    k_mutex_unlock(&lumi_battery_lock);

    return ready
        ? MIN(percent, 100U)
        : MIN(zmk_battery_state_of_charge(), 100U);
}

uint16_t lumi_battery_millivolts(void) {
    uint16_t mv;

    k_mutex_lock(&lumi_battery_lock, K_FOREVER);
    mv = filtered_mv;
    k_mutex_unlock(&lumi_battery_lock);

    return mv;
}

bool lumi_battery_ready(void) {
    bool ready;

    k_mutex_lock(&lumi_battery_lock, K_FOREVER);
    ready = stable_ready;
    k_mutex_unlock(&lumi_battery_lock);

    return ready;
}

static int lumi_battery_init(void) {
    if (!device_is_ready(battery_sensor)) {
        return -ENODEV;
    }

    (void)k_work_schedule(
        &lumi_battery_measure_work,
        K_MSEC(LUMI_BATTERY_BOOT_DELAY_MS));

    /* If NVS restores a value before the first fresh measurement, publish it
     * quickly so Windows, the device UI, and LumiPad all begin from the same
     * stable percentage after reboot.
     */
    (void)k_work_schedule(
        &lumi_battery_bas_work,
        K_SECONDS(2));

    ARG_UNUSED(loaded_from_settings);
    return 0;
}

SYS_INIT(
    lumi_battery_init,
    APPLICATION,
    CONFIG_APPLICATION_INIT_PRIORITY + 1);
