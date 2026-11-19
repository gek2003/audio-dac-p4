/*
 * ============================================================================
 *  ui_touch.h — ёмкостный тач GT911 (Waveshare ESP32-P4-WIFI6-Touch-LCD-5,
 *  IPS 720x1280, контроллер матрицы HX8394 / MIPI-DSI 2 lane).
 *
 *  GT911 — второе I2C-устройство на общей шине борта (SDA=GPIO7, SCL=GPIO8)
 *  рядом с ES9039Q2M (0x48). Адрес тача: 0x5D основной, 0x14 резервный
 *  (какой активен — задаётся состоянием линии INT при сбросе; пробуем оба).
 *
 *  INT/RST — сверить со схемой дисплея (у разных Waveshare-DSI разные пины;
 *  INT опционален: без него драйвер поллит esp_lcd_touch_read_data()).
 *
 *  Подключение к LVGL (espressif/esp_lvgl_adapter), после lvgl_port_init():
 *
 *      const lvgl_port_touch_cfg_t touch_cfg = {
 *          .disp_drv = disp_cfg,          // lvgl_port_display_cfg_t дисплея
 *          .handle   = touch,             // handle из ui_touch_init()
 *      };
 *      lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
 *
 *  Либо собственный indev: lv_indev_drv_t.read_cb -> esp_lcd_touch_read_data().
 * ============================================================================
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Схема ESP32-P4-WIFI6-Touch-LCD-5: TP_RST -> GPIO23 (R37 0R, распаян);
 * сеть TP_INT (выход GT911 «данные готовы») через ОПЦИОНАЛЬНЫЙ резистор
 * R108 «NC/0R» идёт к GPIO2. В стандартной сборке R108 не распаян, поэтому
 * INT до MCU не доходит — драйвер работает поллингом (~20 Гц, для UI
 * достаточно). Вместо R108 можно запаять перемычку TP2 -> GPIO2 (TP2 —
 * тестовая площадка той же сети INT на плате).
 *
 * UI_TOUCH_INT_BRIDGED: 0 — перемычки нет (заводское состояние, по умолчанию);
 *                       1 — перемычка запаяна: INT подключён к GPIO2,
 *                           драйвер настраивает прерывание по СПАДУ
 *                           (falling edge — GT911 по умолчанию активен
 *                           низким уровнем, esp_lcd_touch_gt911 переводит
 *                           levels.interrupt=0 в GPIO_INTR_NEGEDGE).
 *
 * Внимание: GT911 выбирает I2C-адрес (0x5D/0x14) по уровню INT в момент
 * сброса. Перемычка сама ничего не меняет (драйвер не трогает INT как выход,
 * если driver_data не передан) — но не ставьте подтяжку вверх на GPIO2 до
 * сброса контроллера, иначе тач окажется на 0x14 (автодетект его найдёт,
 * адрес в логе). */
#define UI_TOUCH_INT_BRIDGED 0     /* 1 = перемычка TP2 -> GPIO2 запаяна (R108) */

#if UI_TOUCH_INT_BRIDGED
#define UI_TOUCH_INT_GPIO   2      /* INT GT911 -> GPIO2 (через перемычку/R108) */
#else
#define UI_TOUCH_INT_GPIO   (-1)   /* INT не подключён — поллинг */
#endif

#define UI_TOUCH_RST_GPIO   23

/* Разрешение тача = разрешение панели. */
#define UI_TOUCH_X_MAX      720
#define UI_TOUCH_Y_MAX      1280

/*
 * Инициализация GT911 на уже созданной I2C-шине (та, что в main.c).
 *
 * out_touch — handle для lvgl_port_add_touch() или esp_lcd_touch_read_data().
 * Возвращает ESP_ERR_NOT_FOUND, если контроллер не ответил ни на 0x5D, ни на 0x14.
 */
esp_err_t ui_touch_init(i2c_master_bus_handle_t bus, esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif
