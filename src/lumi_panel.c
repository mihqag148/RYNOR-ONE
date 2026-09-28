/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include "lumi_panel.h"
#include "lumi_diag.h"

LOG_MODULE_REGISTER(lumi_panel, CONFIG_ZMK_LOG_LEVEL);
#define PANEL DT_CHOSEN(zephyr_display)
static const struct spi_dt_spec bus =
    SPI_DT_SPEC_GET(PANEL, SPI_OP_MODE_MASTER | SPI_WORD_SET(8), 0);
static const struct gpio_dt_spec dc = GPIO_DT_SPEC_GET(PANEL, cmd_data_gpios);

/* ST7789 module BLK/backlight control on P0.08. */
#define LUMI_BL_PIN 8U
static const struct device *const bl_gpio =
    DEVICE_DT_GET(DT_NODELABEL(gpio0));
static bool bl_ready;
static bool panel_sleeping;

#define PANEL_X_OFFSET DT_PROP(PANEL, x_offset)
#define PANEL_Y_OFFSET DT_PROP(PANEL, y_offset)

#if defined(CONFIG_SPI_ASYNC)
K_SEM_DEFINE(lumi_panel_rgb444_done, 0, 1);
static struct spi_buf lumi_panel_rgb444_buf;
static struct spi_buf_set lumi_panel_rgb444_set;
static bool lumi_panel_rgb444_active;
static int lumi_panel_rgb444_result;

static void lumi_panel_rgb444_callback(
    const struct device *dev,
    int result,
    void *userdata) {
    ARG_UNUSED(dev);
    ARG_UNUSED(userdata);

    lumi_panel_rgb444_result = result;
    k_sem_give(&lumi_panel_rgb444_done);
}
#endif

static int lumi_panel_send_command(uint8_t command) {
    if (!spi_is_ready_dt(&bus) || !gpio_is_ready_dt(&dc)) {
        return -ENODEV;
    }

    struct spi_buf buffer = {
        .buf = &command,
        .len = sizeof(command),
    };
    const struct spi_buf_set buffers = {
        .buffers = &buffer,
        .count = 1,
    };

    int err = gpio_pin_set_dt(&dc, 1);
    if (err == 0) {
        err = spi_write_dt(&bus, &buffers);
    }

    return err;
}

static int lumi_panel_send_command_data(
    uint8_t command,
    const uint8_t *data,
    size_t len) {

    int err = lumi_panel_send_command(command);
    if (err != 0 || !data || len == 0U) {
        return err;
    }

    struct spi_buf buffer = {
        .buf = (void *)data,
        .len = len,
    };
    const struct spi_buf_set buffers = {
        .buffers = &buffer,
        .count = 1,
    };

    err = gpio_pin_set_dt(&dc, 0);
    if (err == 0) {
        err = spi_write_dt(&bus, &buffers);
    }

    return err;
}

int lumi_panel_rgb444_wait(void) {
#if defined(CONFIG_SPI_ASYNC)
    if (!lumi_panel_rgb444_active) {
        return 0;
    }

    int rc = k_sem_take(
        &lumi_panel_rgb444_done,
        K_FOREVER);
    if (rc != 0) {
        return rc;
    }

    lumi_panel_rgb444_active = false;
    return lumi_panel_rgb444_result;
#else
    return 0;
#endif
}

int lumi_panel_rgb444_begin_rect(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height) {

    if (width == 0U ||
        height == 0U ||
        x >= 320U ||
        y >= 172U ||
        (uint32_t)x + width > 320U ||
        (uint32_t)y + height > 172U) {
        return -EINVAL;
    }

    if (!spi_is_ready_dt(&bus) ||
        !gpio_is_ready_dt(&dc)) {
        return -ENODEV;
    }

    int rc = lumi_panel_rgb444_wait();
    if (rc != 0) {
        return rc;
    }

    const uint8_t colmod = 0x03U;
    rc = lumi_panel_send_command_data(
        0x3AU,
        &colmod,
        sizeof(colmod));
    if (rc != 0) {
        return rc;
    }

    const uint16_t x0 =
        (uint16_t)(PANEL_X_OFFSET + x);
    const uint16_t y0 =
        (uint16_t)(PANEL_Y_OFFSET + y);
    const uint16_t x1 =
        (uint16_t)(x0 + width - 1U);
    const uint16_t y1 =
        (uint16_t)(y0 + height - 1U);

    const uint8_t columns[4] = {
        (uint8_t)(x0 >> 8),
        (uint8_t)x0,
        (uint8_t)(x1 >> 8),
        (uint8_t)x1,
    };
    const uint8_t rows[4] = {
        (uint8_t)(y0 >> 8),
        (uint8_t)y0,
        (uint8_t)(y1 >> 8),
        (uint8_t)y1,
    };

    rc = lumi_panel_send_command_data(
        0x2AU,
        columns,
        sizeof(columns));
    if (rc != 0) {
        return rc;
    }

    rc = lumi_panel_send_command_data(
        0x2BU,
        rows,
        sizeof(rows));
    if (rc != 0) {
        return rc;
    }

    return lumi_panel_send_command(0x2CU);
}

int lumi_panel_rgb444_begin_frame(void) {
    return lumi_panel_rgb444_begin_rect(
        0U,
        0U,
        320U,
        172U);
}

int lumi_panel_rgb444_write_async(
    const uint8_t *data,
    size_t len) {

    if (!data || len == 0U) {
        return -EINVAL;
    }

    if (!spi_is_ready_dt(&bus) ||
        !gpio_is_ready_dt(&dc)) {
        return -ENODEV;
    }

    int rc = lumi_panel_rgb444_wait();
    if (rc != 0) {
        return rc;
    }

    rc = gpio_pin_set_dt(&dc, 0);
    if (rc != 0) {
        return rc;
    }

#if defined(CONFIG_SPI_ASYNC)
    k_sem_reset(&lumi_panel_rgb444_done);

    lumi_panel_rgb444_buf.buf =
        (void *)data;
    lumi_panel_rgb444_buf.len = len;
    lumi_panel_rgb444_set.buffers =
        &lumi_panel_rgb444_buf;
    lumi_panel_rgb444_set.count = 1U;
    lumi_panel_rgb444_result = 0;

    rc = spi_transceive_cb(
        bus.bus,
        &bus.config,
        &lumi_panel_rgb444_set,
        NULL,
        lumi_panel_rgb444_callback,
        NULL);

    if (rc == 0) {
        lumi_panel_rgb444_active = true;
    }

    return rc;
#else
    struct spi_buf buffer = {
        .buf = (void *)data,
        .len = len,
    };
    const struct spi_buf_set buffers = {
        .buffers = &buffer,
        .count = 1U,
    };
    return spi_write_dt(&bus, &buffers);
#endif
}

int lumi_panel_rgb444_end_frame(void) {
    int rc = lumi_panel_rgb444_wait();

    const uint8_t colmod = 0x05U;
    int restore_rc = lumi_panel_send_command_data(
        0x3AU,
        &colmod,
        sizeof(colmod));

    return rc != 0 ? rc : restore_rc;
}

static int lumi_panel_backlight_init(void) {
    if (!device_is_ready(bl_gpio)) {
        lumi_diag_report('E', "Backlight GPIO not ready");
        return -ENODEV;
    }

    int err = gpio_pin_configure(
        bl_gpio,
        LUMI_BL_PIN,
        GPIO_OUTPUT_LOW);
    if (err) {
        lumi_diag_report('E', "Backlight GPIO init rc=%d", err);
        return err;
    }

    bl_ready = true;
    return 0;
}

int lumi_panel_set_backlight(bool enabled) {
    if (!bl_ready) {
        int err = lumi_panel_backlight_init();
        if (err) {
            return err;
        }
    }

    int err = gpio_pin_set(
        bl_gpio,
        LUMI_BL_PIN,
        enabled ? 1 : 0);
    if (err) {
        lumi_diag_report(
            'E',
            "Backlight %s rc=%d",
            enabled ? "ON" : "OFF",
            err);
        return err;
    }

    lumi_diag_report(
        'I',
        "Backlight %s",
        enabled ? "ON" : "OFF");
    return 0;
}

int lumi_panel_init(void) {
    /* Keep the backlight dark until the complete LVGL screen is built.
     * This hides the ST7789's undefined RAM contents during power-up.
     */
    (void)lumi_panel_backlight_init();
    (void)lumi_panel_set_backlight(false);

    /* Keep the known-good ST7789 baseline timing. This panel has no TE
     * feedback wired, so forcing FRCTRL2/porch cannot synchronize RAMWR.
     */
    if (!spi_is_ready_dt(&bus) || !gpio_is_ready_dt(&dc)) {
        lumi_diag_report('E', "Panel init: SPI/DC not ready");
        return -ENODEV;
    }

    uint8_t command = 0x21; /* INVON */
    struct spi_buf buffer = {.buf = &command, .len = sizeof(command)};
    const struct spi_buf_set buffers = {.buffers = &buffer, .count = 1};

    int err = gpio_pin_set_dt(&dc, 1);
    if (err == 0) {
        err = spi_write_dt(&bus, &buffers);
    }
    if (err) {
        LOG_ERR("Panel inversion setup failed: %d", err);
        lumi_diag_report('E', "Panel INVON failed rc=%d", err);
    } else {
        lumi_diag_report('I', "Panel init OK");
    }
    return err;
}

int lumi_panel_set_sleep(bool sleeping) {
    if (!spi_is_ready_dt(&bus) || !gpio_is_ready_dt(&dc)) {
        lumi_diag_report('E', "Panel %s: SPI/DC not ready",
                         sleeping ? "sleep" : "wake");
        return -ENODEV;
    }

    if (sleeping == panel_sleeping) {
        if (sleeping) {
            (void)lumi_panel_set_backlight(false);
        }
        return 0;
    }

    /* Backlight is always dark while changing ST7789 power state. */
    (void)lumi_panel_set_backlight(false);

    int err;

    if (sleeping) {
        /* DISPOFF + SLPIN drops the ST7789 controller into its low-current
         * sleep state. The old DISPOFF-only path left the controller fully
         * powered and was a measurable source of soft-sleep drain.
         */
        err = lumi_panel_send_command(0x28); /* DISPOFF */
        if (err == 0) {
            k_msleep(5);
            err = lumi_panel_send_command(0x10); /* SLPIN */
        }
        if (err == 0) {
            /* ST7789 requires >=120 ms after SLPIN before power removal. */
            k_msleep(120);
        }
    } else {
        /* Wake is deliberately serialized and waits for SLPOUT before DISPON.
         * This fixes the first-redraw loss that originally motivated the
         * higher-current DISPOFF-only soft sleep.
         */
        err = lumi_panel_send_command(0x11); /* SLPOUT */
        if (err == 0) {
            k_msleep(120);
            err = lumi_panel_send_command(0x29); /* DISPON */
        }
        if (err == 0) {
            k_msleep(20);
        }
    }

    if (err) {
        LOG_ERR("Panel %s failed: %d", sleeping ? "sleep" : "wake", err);
        lumi_diag_report('E', "Panel %s failed rc=%d",
                         sleeping ? "sleep" : "wake", err);
        return err;
    }

    panel_sleeping = sleeping;
    lumi_diag_report('I', "Panel %s", sleeping ? "SLPIN" : "SLPOUT");
    return 0;
}

int lumi_panel_enter_deep_sleep(void) {
    /* Deep sleep is a cold-boot wake path. Ensure the controller is already
     * in SLPIN before ZMK suspends devices and disables the external VCC rail.
     */
    int err = lumi_panel_set_sleep(true);
    if (err == 0) {
        lumi_diag_report('I', "Panel ready for VCC power-off");
    }
    return err;
}
