/*
 * Copyright (c) 2025 Mariano Uvalle
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT azoteq_iqs5xx

#include <stdlib.h>
#include <string.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "iqs5xx.h"
#include "iqs5xx_gr_trackpad65_fw.h"

LOG_MODULE_REGISTER(iqs5xx, CONFIG_INPUT_LOG_LEVEL);

static int iqs5xx_write_reg16(const struct device *dev, uint16_t reg, uint16_t val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_read_reg8(const struct device *dev, uint16_t reg, uint8_t *val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};

    return i2c_write_read_dt(&config->i2c, reg_buf, sizeof(reg_buf), val, 1);
}

static int iqs5xx_read_block(const struct device *dev, uint16_t reg, uint8_t *data, size_t len) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};

    return i2c_write_read_dt(&config->i2c, reg_buf, sizeof(reg_buf), data, len);
}

static int iqs5xx_write_reg8(const struct device *dev, uint16_t reg, uint8_t val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[3] = {reg >> 8, reg & 0xFF, val};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_end_comm_window(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[3] = {IQS5XX_END_COMM_WINDOW >> 8, IQS5XX_END_COMM_WINDOW & 0xFF, 0x00};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_i2c_write16_addr(const struct iqs5xx_config *config, uint16_t addr,
                                   uint16_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[2 + IQS5XX_BL_PROGRAM_BLOCK_SIZE];

    if (len > IQS5XX_BL_PROGRAM_BLOCK_SIZE) {
        return -EINVAL;
    }

    buf[0] = reg >> 8;
    buf[1] = reg & 0xFF;
    memcpy(&buf[2], data, len);

    return i2c_write(config->i2c.bus, buf, len + 2, addr);
}

static int iqs5xx_bl_read_cmd(const struct iqs5xx_config *config, uint16_t addr, uint8_t cmd,
                              uint8_t *data, size_t len) {
    return i2c_write_read(config->i2c.bus, addr, &cmd, 1, data, len);
}

static int iqs5xx_bl_write_cmd(const struct iqs5xx_config *config, uint16_t addr, uint8_t cmd,
                               uint8_t data) {
    uint8_t buf[2] = {cmd, data};

    return i2c_write(config->i2c.bus, buf, sizeof(buf), addr);
}

static int iqs5xx_bl_read_block(const struct iqs5xx_config *config, uint16_t addr, uint16_t reg,
                                uint8_t *data, size_t len) {
    uint8_t cmd[3] = {IQS5XX_BL_CMD_READ, reg >> 8, reg & 0xFF};

    return i2c_write_read(config->i2c.bus, addr, cmd, sizeof(cmd), data, len);
}

static int iqs5xx_wait_for_bootloader(const struct device *dev, uint8_t *version, uint16_t timeout_ms) {
    const struct iqs5xx_config *config = dev->config;
    uint16_t bl_addr = config->i2c.addr ^ IQS5XX_BL_ADDR_XOR;
    int ret = -ETIMEDOUT;
    uint16_t interval_ms = config->bootloader_poll_interval_ms;

    if (interval_ms == 0) {
        interval_ms = 100;
    }

    for (uint16_t elapsed = 0; elapsed < timeout_ms; elapsed += interval_ms) {
        ret = iqs5xx_bl_read_cmd(config, bl_addr, IQS5XX_BL_CMD_VERSION, version, 2);
        if (ret == 0) {
            return 0;
        }

        if (elapsed == 0 || (elapsed % 1000) == 0) {
            LOG_WRN("IQS5xx bootloader poll failed at 0x%02x: %d", bl_addr, ret);
        }

        k_msleep(interval_ms);
    }

    return ret < 0 ? ret : -ETIMEDOUT;
}

static int iqs5xx_enter_bootloader(const struct device *dev, uint8_t *version) {
    const struct iqs5xx_config *config = dev->config;
    int ret;

    if (config->reset_gpio.port) {
        gpio_pin_set_dt(&config->reset_gpio, 1);
        k_msleep(1);
        gpio_pin_set_dt(&config->reset_gpio, 0);
        ret = iqs5xx_wait_for_bootloader(dev, version, 1000);
        if (ret == 0) {
            return 0;
        }
    } else {
        LOG_INF("No IQS5xx reset GPIO; polling bootloader for %u ms",
                config->bootloader_poll_timeout_ms);
        ret = iqs5xx_wait_for_bootloader(dev, version, config->bootloader_poll_timeout_ms);
        if (ret == 0) {
            return 0;
        }

        LOG_INF("Bootloader not seen; trying IQS5xx software reset through app address 0x%02x",
                config->i2c.addr);
        ret = iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONTROL_1, IQS5XX_RESET);
        if (ret == 0) {
            (void)iqs5xx_end_comm_window(dev);
        } else {
            LOG_WRN("IQS5xx software reset command failed: %d", ret);
        }

        ret = iqs5xx_wait_for_bootloader(dev, version, config->bootloader_poll_timeout_ms);
        if (ret == 0) {
            return 0;
        }
    }

    return ret;
}

static int iqs5xx_gr_trackpad65_nv_matches(const struct iqs5xx_config *config, uint16_t bl_addr) {
    uint8_t buf[IQS5XX_BL_PROGRAM_BLOCK_SIZE];

    for (uint16_t offset = 0; offset < IQS5XX_GR_TRACKPAD65_NV_SIZE;
         offset += IQS5XX_BL_PROGRAM_BLOCK_SIZE) {
        int ret = iqs5xx_bl_read_block(config, bl_addr, IQS5XX_GR_TRACKPAD65_NV_START + offset, buf,
                                       sizeof(buf));
        if (ret < 0) {
            return ret;
        }

        if (memcmp(buf, &iqs5xx_gr_trackpad65_fw[IQS5XX_GR_TRACKPAD65_NV_START -
                                                IQS5XX_GR_TRACKPAD65_FW_START + offset],
                   sizeof(buf)) != 0) {
            return 0;
        }
    }

    return 1;
}

static int iqs5xx_program_gr_trackpad65_firmware(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    uint16_t bl_addr = config->i2c.addr ^ IQS5XX_BL_ADDR_XOR;
    uint8_t version[2];
    uint8_t status;
    int ret;

    LOG_INF("Entering IQS5xx bootloader for GR-Trackpad65 firmware programming");

    ret = iqs5xx_enter_bootloader(dev, version);
    if (ret < 0) {
        LOG_ERR("Failed to enter IQS5xx bootloader: %d", ret);
        return ret;
    }

    if (version[0] != IQS5XX_BL_VERSION_MAJOR || version[1] != IQS5XX_BL_VERSION_MINOR) {
        LOG_ERR("Unsupported IQS5xx bootloader version %u.%u", version[0], version[1]);
        return -ENOTSUP;
    }

    if (!config->force_firmware_update) {
        ret = iqs5xx_gr_trackpad65_nv_matches(config, bl_addr);
        if (ret < 0) {
            LOG_WRN("Could not verify existing GR-Trackpad65 NV config: %d", ret);
        } else if (ret == 1) {
            LOG_INF("GR-Trackpad65 non-volatile config already matches bundled image");
            (void)iqs5xx_bl_write_cmd(config, bl_addr, IQS5XX_BL_CMD_EXIT, 0);
            k_msleep(100);
            return 0;
        }
    }

    LOG_INF("Programming GR-Trackpad65 IQS550 image (%u bytes)",
            (unsigned int)IQS5XX_GR_TRACKPAD65_FW_SIZE);

    for (uint16_t offset = 0; offset < IQS5XX_GR_TRACKPAD65_FW_SIZE;
         offset += IQS5XX_BL_PROGRAM_BLOCK_SIZE) {
        ret = iqs5xx_i2c_write16_addr(config, bl_addr, IQS5XX_GR_TRACKPAD65_FW_START + offset,
                                      &iqs5xx_gr_trackpad65_fw[offset],
                                      IQS5XX_BL_PROGRAM_BLOCK_SIZE);
        if (ret < 0) {
            LOG_ERR("Failed to write IQS5xx firmware block 0x%04x: %d",
                    (unsigned int)(IQS5XX_GR_TRACKPAD65_FW_START + offset), ret);
            return ret;
        }

        k_msleep(7);
    }

    ret = iqs5xx_i2c_write16_addr(config, bl_addr, IQS5XX_GR_TRACKPAD65_CRC_ADDR,
                                  iqs5xx_gr_trackpad65_crc,
                                  IQS5XX_GR_TRACKPAD65_CRC_SIZE);
    if (ret < 0) {
        LOG_ERR("Failed to write IQS5xx CRC descriptor: %d", ret);
        return ret;
    }
    k_msleep(7);

    ret = iqs5xx_bl_read_cmd(config, bl_addr, IQS5XX_BL_CMD_CRC_CHECK, &status, 1);
    if (ret < 0) {
        LOG_ERR("Failed to run IQS5xx CRC check: %d", ret);
        return ret;
    }
    if (status != 0) {
        LOG_ERR("IQS5xx CRC check failed with status 0x%02x", status);
        return -EIO;
    }

    ret = iqs5xx_gr_trackpad65_nv_matches(config, bl_addr);
    if (ret != 1) {
        LOG_ERR("IQS5xx non-volatile readback verification failed: %d", ret);
        return ret < 0 ? ret : -EIO;
    }

    LOG_INF("GR-Trackpad65 IQS550 firmware programmed successfully");

    ret = iqs5xx_bl_write_cmd(config, bl_addr, IQS5XX_BL_CMD_EXIT, 0);
    if (ret < 0) {
        LOG_ERR("Failed to exit IQS5xx bootloader: %d", ret);
        return ret;
    }

    k_msleep(100);
    return 0;
}

static void iqs5xx_firmware_program_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, firmware_program_work);
    int ret;

    LOG_INF("Starting delayed GR-Trackpad65 IQS550 firmware writer");

    ret = iqs5xx_program_gr_trackpad65_firmware(data->dev);
    if (ret < 0) {
        LOG_ERR("GR-Trackpad65 IQS550 firmware writer failed: %d", ret);
        LOG_ERR("Check FFC orientation, VCC/GND, SDA/SCL continuity, and IQS550 NRST/RDY access");
        return;
    }

    LOG_INF("GR-Trackpad65 IQS550 firmware writer finished; flash normal futaba firmware now");
}

static void iqs5xx_button_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, button_release_work);

    for (int i = 0; i < 3; i++) {
        if (data->buttons_pressed & BIT(i)) {
            input_report_key(data->dev, INPUT_BTN_0 + i, 0, true, K_FOREVER);
            data->buttons_pressed &= ~BIT(i);
        }
    }
}

static int16_t iqs5xx_be16s(const uint8_t *data) {
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int16_t iqs5xx_apply_divisor(int16_t value, uint16_t divisor) {
    if (divisor <= 1) {
        return value;
    }

    return value / divisor;
}

static void iqs5xx_report_click(struct iqs5xx_data *data, uint16_t button_code) {
    k_work_cancel_delayable(&data->button_release_work);

    input_report_key(data->dev, button_code, 1, true, K_FOREVER);
    data->buttons_pressed |= BIT(button_code - INPUT_BTN_0);

    k_work_schedule(&data->button_release_work, K_MSEC(100));
}

static bool iqs5xx_report_scroll(const struct iqs5xx_config *config, struct iqs5xx_data *data,
                                 int16_t rel_x, int16_t rel_y) {
    const uint16_t scroll_div = config->scroll_divisor == 0 ? 24 : config->scroll_divisor;

    if (rel_x != 0) {
        if (!config->natural_scroll_x) {
            rel_x *= -1;
        }

        data->scroll_x_acc += rel_x;
        if (abs(data->scroll_x_acc) >= scroll_div) {
            input_report_rel(data->dev, INPUT_REL_HWHEEL, data->scroll_x_acc / scroll_div, true,
                             K_FOREVER);
            data->scroll_x_acc %= scroll_div;
            return true;
        }
    }

    if (rel_y != 0) {
        if (config->natural_scroll_y) {
            rel_y *= -1;
        }

        data->scroll_y_acc += rel_y;
        if (abs(data->scroll_y_acc) >= scroll_div) {
            input_report_rel(data->dev, INPUT_REL_WHEEL, data->scroll_y_acc / scroll_div, true,
                             K_FOREVER);
            data->scroll_y_acc %= scroll_div;
            return true;
        }
    }

    return false;
}

static void iqs5xx_work_handler(struct k_work *work) {
    struct iqs5xx_data *data = CONTAINER_OF(work, struct iqs5xx_data, work);
    const struct device *dev = data->dev;
    const struct iqs5xx_config *config = dev->config;
    uint8_t touch_data[IQS5XX_TOUCH_DATA_SIZE];
    int ret;

    ret = iqs5xx_read_block(dev, IQS5XX_TOUCH_DATA_START, touch_data, sizeof(touch_data));
    if (ret < 0) {
        LOG_ERR("Failed to read IQS5xx touch data: %d", ret);
        goto end_comm;
    }

    const uint8_t gesture_events_0 = touch_data[0];
    const uint8_t gesture_events_1 = touch_data[1];
    const uint8_t sys_info_0 = touch_data[2];
    const uint8_t sys_info_1 = touch_data[3];
    const uint8_t num_fingers = touch_data[4];
    int16_t rel_x = iqs5xx_be16s(&touch_data[5]);
    int16_t rel_y = iqs5xx_be16s(&touch_data[7]);

    // Handle reset indication.
    if (sys_info_0 & IQS5XX_SHOW_RESET) {
        LOG_INF("Device reset detected");
        iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONTROL_0, IQS5XX_ACK_RESET);
        goto end_comm;
    }

    bool tp_movement = (sys_info_1 & IQS5XX_TP_MOVEMENT) != 0;
    bool scroll = config->scroll && (gesture_events_1 & IQS5XX_SCROLL) != 0;
    if (!scroll) {
        data->scroll_x_acc = 0;
        data->scroll_y_acc = 0;
    }

    bool hold_became_active = (gesture_events_0 & IQS5XX_PRESS_AND_HOLD) && !data->active_hold;
    bool hold_released = !(gesture_events_0 & IQS5XX_PRESS_AND_HOLD) && data->active_hold;

    if (hold_became_active) {
        LOG_INF("Hold became active");
        input_report_key(dev, LEFT_BUTTON_CODE, 1, true, K_FOREVER);
        data->active_hold = true;
    } else if (hold_released) {
        LOG_INF("Hold became inactive");
        input_report_key(dev, LEFT_BUTTON_CODE, 0, true, K_FOREVER);
        data->active_hold = false;
    } else if (config->one_finger_tap && (gesture_events_0 & IQS5XX_SINGLE_TAP)) {
        iqs5xx_report_click(data, LEFT_BUTTON_CODE);
    } else if (config->two_finger_tap && (gesture_events_1 & IQS5XX_TWO_FINGER_TAP)) {
        iqs5xx_report_click(data, RIGHT_BUTTON_CODE);
    } else if (scroll) {
        (void)iqs5xx_report_scroll(config, data, rel_x, rel_y);
    } else if (tp_movement && num_fingers == 1) {
        rel_x = iqs5xx_apply_divisor(rel_x, config->movement_divisor);
        rel_y = iqs5xx_apply_divisor(rel_y, config->movement_divisor);
        if (rel_x != 0 || rel_y != 0) {
            input_report_rel(dev, INPUT_REL_X, rel_x, false, K_FOREVER);
            input_report_rel(dev, INPUT_REL_Y, rel_y, true, K_FOREVER);
        }
    }

end_comm:
    // End communication window.
    iqs5xx_end_comm_window(dev);
}

static void iqs5xx_poll_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, poll_work);
    const struct iqs5xx_config *config = data->dev->config;

    k_work_submit(&data->work);
    k_work_reschedule(&data->poll_work, K_MSEC(config->poll_interval_ms));
}

static void iqs5xx_rdy_handler(const struct device *port, struct gpio_callback *cb,
                               gpio_port_pins_t pins) {
    ARG_UNUSED(port);
    ARG_UNUSED(pins);

    struct iqs5xx_data *data = CONTAINER_OF(cb, struct iqs5xx_data, rdy_cb);

    k_work_submit(&data->work);
}

static int iqs5xx_setup_device(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    int ret;

    // Enable event mode and trackpad events.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONFIG_1,
                            IQS5XX_EVENT_MODE | IQS5XX_TP_EVENT | IQS5XX_GESTURE_EVENT);
    if (ret < 0) {
        LOG_ERR("Failed to configure event mode: %d", ret);
        return ret;
    }

    ret = iqs5xx_write_reg8(dev, IQS5XX_BOTTOM_BETA, config->bottom_beta);
    if (ret < 0) {
        LOG_ERR("Failed to set bottom beta: %d", ret);
        return ret;
    }

    ret = iqs5xx_write_reg8(dev, IQS5XX_STATIONARY_THRESH, config->stationary_threshold);
    if (ret < 0) {
        LOG_ERR("Failed to set bottom stationary threshold: %d", ret);
        return ret;
    }

    // TODO: Expose these through dts bindings.
    // Set filter settings with:
    // - IIR filter enabled
    // - MAV filter enabled
    // - IIR select disabled (dynamic IIR)
    // - ALP count filter enabled
    ret = iqs5xx_write_reg8(dev, IQS5XX_FILTER_SETTINGS,
                            IQS5XX_IIR_FILTER | IQS5XX_MAV_FILTER | IQS5XX_ALP_COUNT_FILTER);
    if (ret < 0) {
        LOG_ERR("Failed to configure filter settings: %d", ret);
        return ret;
    }

    uint8_t single_finger_gestures = 0;
    single_finger_gestures |= config->one_finger_tap ? IQS5XX_SINGLE_TAP : 0;
    single_finger_gestures |= config->press_and_hold ? IQS5XX_PRESS_AND_HOLD : 0;
    // Configure single finger gestures.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SINGLE_FINGER_GESTURES_CONF, single_finger_gestures);
    if (ret < 0) {
        LOG_ERR("Failed to configure single finger gestures: %d", ret);
        return ret;
    }

    // Configure the hold time for the press and hold gesture.
    ret = iqs5xx_write_reg16(dev, IQS5XX_HOLD_TIME, config->press_and_hold_time);
    if (ret < 0) {
        LOG_ERR("Failed to configure the hold time: %d", ret);
        return ret;
    }

    uint8_t two_finger_gestures = 0;
    two_finger_gestures |= config->two_finger_tap ? IQS5XX_TWO_FINGER_TAP : 0;
    two_finger_gestures |= config->scroll ? IQS5XX_SCROLL : 0;
    // Configure multi finger gestures.
    ret = iqs5xx_write_reg8(dev, IQS5XX_MULTI_FINGER_GESTURES_CONF, two_finger_gestures);
    if (ret < 0) {
        LOG_ERR("Failed to configure multi finger gestures: %d", ret);
        return ret;
    }

    // Configure axes.
    uint8_t xy_config = 0;
    xy_config |= config->flip_x ? IQS5XX_FLIP_X : 0;
    xy_config |= config->flip_y ? IQS5XX_FLIP_Y : 0;
    xy_config |= config->switch_xy ? IQS5XX_SWITCH_XY_AXIS : 0;
    ret = iqs5xx_write_reg8(dev, IQS5XX_XY_CONFIG_0, xy_config);
    if (ret < 0) {
        LOG_ERR("Failed to configure axes: %d", ret);
        return ret;
    }

    // Configure system settings.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONFIG_0, IQS5XX_SETUP_COMPLETE | IQS5XX_WDT);
    if (ret < 0) {
        LOG_ERR("Failed to configure system: %d", ret);
        return ret;
    }

    // End communication window.
    ret = iqs5xx_end_comm_window(dev);
    if (ret < 0) {
        LOG_ERR("Failed to end comm window during initialization: %d", ret);
        return ret;
    }

    return 0;
}

static int iqs5xx_init(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    struct iqs5xx_data *data = dev->data;
    int ret;

    if (!i2c_is_ready_dt(&config->i2c)) {
        LOG_ERR("I2C device not ready");
        return -ENODEV;
    }

    data->dev = dev;
    k_work_init(&data->work, iqs5xx_work_handler);
    k_work_init_delayable(&data->poll_work, iqs5xx_poll_work_handler);
    k_work_init_delayable(&data->button_release_work, iqs5xx_button_release_work_handler);
    k_work_init_delayable(&data->firmware_program_work, iqs5xx_firmware_program_work_handler);

    // Configure reset GPIO if available.
    if (config->reset_gpio.port) {
        if (!gpio_is_ready_dt(&config->reset_gpio)) {
            LOG_ERR("Reset GPIO not ready");
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure reset GPIO: %d", ret);
            return ret;
        }

        // Reset the device.
        gpio_pin_set_dt(&config->reset_gpio, 1);
        k_msleep(1);
        gpio_pin_set_dt(&config->reset_gpio, 0);
        k_msleep(10);
    }

    if (config->program_firmware) {
        LOG_INF("GR-Trackpad65 writer enabled; programming starts in %u ms",
                config->firmware_program_delay_ms);
        k_work_schedule(&data->firmware_program_work, K_MSEC(config->firmware_program_delay_ms));
        return 0;
    }

    if (config->rdy_gpio.port) {
        if (!gpio_is_ready_dt(&config->rdy_gpio)) {
            LOG_ERR("RDY GPIO not ready");
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(&config->rdy_gpio, GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("Failed to configure RDY GPIO: %d", ret);
            return ret;
        }

        gpio_init_callback(&data->rdy_cb, iqs5xx_rdy_handler, BIT(config->rdy_gpio.pin));
        ret = gpio_add_callback(config->rdy_gpio.port, &data->rdy_cb);
        if (ret < 0) {
            LOG_ERR("Failed to add RDY callback: %d", ret);
            return ret;
        }

        ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_EDGE_RISING);
        if (ret < 0) {
            LOG_ERR("Failed to configure RDY interrupt: %d", ret);
            return ret;
        }
    } else {
        if (config->poll_interval_ms == 0) {
            LOG_ERR("Polling mode requires a non-zero poll interval");
            return -EINVAL;
        }

        LOG_INF("RDY GPIO not present, using polling mode (%u ms)", config->poll_interval_ms);
    }

    // Wait for device to be ready.
    k_msleep(100);

    // Setup device configuration.
    ret = iqs5xx_setup_device(dev);
    if (ret < 0) {
        LOG_ERR("Failed to setup device: %d", ret);
        return ret;
    }

    data->initialized = true;

    if (!config->rdy_gpio.port) {
        k_work_schedule(&data->poll_work, K_MSEC(config->poll_interval_ms));
    }

    LOG_INF("IQS5xx trackpad initialized");

    return 0;
}

#define IQS5XX_INIT(n)                                                                                \
    static struct iqs5xx_data iqs5xx_data_##n;                                                        \
    static const struct iqs5xx_config iqs5xx_config_##n = {                                           \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                               \
        .rdy_gpio = GPIO_DT_SPEC_INST_GET_OR(n, rdy_gpios, {0}),                                     \
        .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),                                 \
        .one_finger_tap = DT_INST_PROP(n, one_finger_tap),                                            \
        .press_and_hold = DT_INST_PROP(n, press_and_hold),                                            \
        .two_finger_tap = DT_INST_PROP(n, two_finger_tap),                                            \
        .scroll = DT_INST_PROP(n, scroll),                                                            \
        .natural_scroll_x = DT_INST_PROP(n, natural_scroll_x),                                        \
        .natural_scroll_y = DT_INST_PROP(n, natural_scroll_y),                                        \
        .press_and_hold_time = DT_INST_PROP_OR(n, press_and_hold_time, 250),                         \
        .switch_xy = DT_INST_PROP(n, switch_xy),                                                      \
        .flip_x = DT_INST_PROP(n, flip_x),                                                            \
        .flip_y = DT_INST_PROP(n, flip_y),                                                            \
        .bottom_beta = DT_INST_PROP_OR(n, bottom_beta, 5),                                            \
        .stationary_threshold = DT_INST_PROP_OR(n, stationary_threshold, 5),                         \
        .poll_interval_ms = DT_INST_PROP_OR(n, poll_interval_ms, 12),                                \
        .scroll_divisor = DT_INST_PROP_OR(n, scroll_divisor, 24),                                    \
        .movement_divisor = DT_INST_PROP_OR(n, movement_divisor, 1),                                 \
        .program_firmware = DT_INST_PROP(n, program_firmware),                                       \
        .force_firmware_update = DT_INST_PROP(n, force_firmware_update),                             \
        .firmware_program_delay_ms = DT_INST_PROP_OR(n, firmware_program_delay_ms, 5000),             \
        .bootloader_poll_timeout_ms = DT_INST_PROP_OR(n, bootloader_poll_timeout_ms, 1000),           \
        .bootloader_poll_interval_ms = DT_INST_PROP_OR(n, bootloader_poll_interval_ms, 100),          \
    };                                                                                                \
    DEVICE_DT_INST_DEFINE(n, iqs5xx_init, NULL, &iqs5xx_data_##n, &iqs5xx_config_##n, POST_KERNEL,  \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(IQS5XX_INIT)
