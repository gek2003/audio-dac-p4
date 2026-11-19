/*
 * ============================================================================
 *  ui_display.h — дисплей Waveshare ESP32-P4-WIFI6-Touch-LCD-5
 *  (IPS 720x1280, контроллер HX8394, MIPI-DSI, 2 lane).
 *
 *  Ориентация по умолчанию — ГОРИЗОНТАЛЬНАЯ: нативные 720x1280 поворачиваются
 *  через rotation (swap_xy) порта LVGL -> рабочая область 1280x720.
 * ============================================================================
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Полная инициализация: LDO PHY -> DSI-шина -> HX8394 -> LVGL (rotation 90).
 * I²C-шина (тач/ЦАП) инициализируется отдельно в main.c — сюда не нужна. */
esp_err_t ui_display_init(void);

lv_display_t *ui_display_get(void);

/* Подсветка, 0..100%. По схеме ESP32-P4-WIFI6-Touch-LCD-5 подсветка — чисто
 * PWM: BL_EN GPIO33 (EN контроллера AP3032KTR-G1) + BL_PWM GPIO26; I²C не
 * используется. */
void ui_display_brightness_set(int percent);
int  ui_display_brightness_get(void);      /* уровень, заданный последним set */

/* Сон/пробуждение панели: Sleep In/Out через DBI + подсветка. on=true —
 * погасить и усыпить; on=false — разбудить (блокирует ~130 мс) и вернуть
 * яркость. Если DBI недоступен — ESP_ERR_NOT_SUPPORTED (только подсветка). */
esp_err_t ui_display_sleep(bool on);
