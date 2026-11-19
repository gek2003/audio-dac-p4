/*
 * ============================================================================
 *  ui_touch.c — инициализация тача GT911 (Waveshare 5" DSI 720x1280, HX8394)
 *
 *  Два слоя:
 *    1) esp_lcd panel IO поверх I2C — GT911 говорит на I2C c 16-битными
 *       командами (lcd_cmd_bits=16, control phase отключена);
 *    2) esp_lcd_touch — общий интерфейс тач-драйверов Espressif поверх IO.
 *
 *  Сам GT911 конфигурацию сенсора хранит в своей NVRAM и начинает сканировать
 *  сразу после питания — со стороны ESP-IDF ничего «прошивать» не нужно.
 * ============================================================================
 */
#include "ui_touch.h"

#include <string.h>

#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "ui_touch";

/* INT при reset в 0 / висит — основной адрес; в 1 — резервный.
 * С запаянной перемычкой (UI_TOUCH_INT_BRIDGED=1) адрес по-прежнему задаёт
 * подтяжка модуля: драйвер проходит ветку "address initialization skipped"
 * (driver_data не передаётся — INT как выход не трогается), поэтому адрес
 * не меняется; если в вашей конфигурации он сместился — автодетект ниже
 * найдёт контроллер на втором адресе. */
#define GT911_ADDR_PRIMARY   0x5D
#define GT911_ADDR_BACKUP    0x14

static esp_err_t try_addr(i2c_master_bus_handle_t bus, uint8_t addr,
                          esp_lcd_touch_handle_t *out_touch)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_touch_handle_t touch = NULL;

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .control_phase_bytes = 1,           /* GT911: адрес/команда в 2 байтах */
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,                 /* 16-битные регистровые адреса */
        .lcd_param_bits = 0,
        .scl_speed_hz = 400000,             /* тач нечасто; 400 кГц ок. Библиотека берёт скорость отсюда */
        .flags = {
            .disable_control_phase = 1,     /* data phase без бита DC — как у GT911 */
        },
    };

        /* под новый esp_driver_i2c (i2c_master_bus_handle_t) */
        esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io GT911 (0x%02X): %s", addr, esp_err_to_name(err));
        return err;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = UI_TOUCH_X_MAX,
        .y_max = UI_TOUCH_Y_MAX,
        .rst_gpio_num = UI_TOUCH_RST_GPIO,
        /* INT: GPIO_NUM_NC при UI_TOUCH_INT_BRIDGED=0 (поллинг); при 1 — GPIO2.
         * levels.interrupt=0 => esp_lcd_touch_gt911 ставит GPIO_INTR_NEGEDGE:
         * прерывание по СПАДУ — совпадает с полярностью GT911 по умолчанию
         * (INT активен низким уровнем, регистр 0x8145 bit1 = 0). */
        .int_gpio_num = UI_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,                     /* активный уровень RST — сверьте со схемой */
            .interrupt = 0,                 /* 0 = falling edge (штатная полярность GT911) */
        },
        .flags = {
            /* Подберите по факту первого запуска (сильнее всего зависит от
             * монтажа панели и стартовой ротации LVGL): */
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    err = esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 @0x%02X не ответил (%s)", addr, esp_err_to_name(err));
        (void)esp_lcd_panel_io_del(io);
        return err;
    }

    *out_touch = touch;
    ESP_LOGI(TAG, "GT911 готов на 0x%02X (%dx%d)", addr, UI_TOUCH_X_MAX, UI_TOUCH_Y_MAX);
    return ESP_OK;
}

esp_err_t ui_touch_init(i2c_master_bus_handle_t bus, esp_lcd_touch_handle_t *out_touch)
{
    if (bus == NULL || out_touch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = try_addr(bus, GT911_ADDR_PRIMARY, out_touch);
    if (err != ESP_OK) {
        err = try_addr(bus, GT911_ADDR_BACKUP, out_touch);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 не найден ни на 0x%02X, ни на 0x%02X — "
                      "проверьте шлейф дисплея и пины INT/RST", GT911_ADDR_PRIMARY, GT911_ADDR_BACKUP);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
