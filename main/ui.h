/*
 * ============================================================================
 *  ui.h — графическая оболочка: LVGL 9, EN/RU, горизонтально (1280x720)
 *
 *  Экраны (tabview): Главная (громкость, mute, play/pause/next/prev,
 *  прогресс трека, источник) / Фильтры (штатный пресет Reg 88 + кастомный
 *  FIR) / Диагностика / Настройки (язык, подсветка).
 *
 *  Доступ к системе — через hooks (функции-обёртки над драйвером ЦАПа
 *  в main.c). Любое поле hooks может быть NULL — соответствующий элемент
 *  UI просто не активен.
 * ============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef enum {
    UI_LANG_EN = 0,
    UI_LANG_RU = 1,
} ui_lang_t;

typedef struct {
    /* --- Действия --- */
    void (*set_volume_db)(float db);          /* 0.0 .. -127.5 */
    void (*set_mute)(bool on);
    void (*set_filter)(int preset);           /* штатный пресет Reg 88, 0..7 */
    /* Wi-Fi и источник (вкладка «Сеть»): set_wifi применяет креды сразу
     * (wifi_set_credentials переподключается, если стек уже стартовал),
     * set_source_url перезапускает декодер с новым URL. */
    void (*set_wifi)(const char *ssid, const char *pass);
    void (*set_source_url)(const char *url);
    /* Управление воспроизведением (Главная): play/pause — пауза/возобновление
     * текущего источника или перезапуск последнего; next/prev — соседний
     * трек очереди (плейлист или папка). NULL — кнопки не активны. */
    void (*play_pause)(void);
    void (*next_track)(void);
    void (*prev_track)(void);
    void (*set_fir)(int fir);                 /* 0 = авто (набор под поток),
                                                 1 = встроенный (Reg 88), закреплён,
                                                 2..4 C-BRICK 44.1/48/96k,
                                                 5..7 C-SLOW 44.1/48/96k.
                                                 Ручной выбор (1..7) приоритетнее
                                                 авто-подмены при смене fs */
    /* --- Состояние --- */
    const char *(*get_source)(void);          /* URL потока или "-" */
    const char *(*get_wifi_ssid)(void);       /* текущий SSID ("" — не задан) */
    void (*get_diag)(char *buf, size_t len);  /* строка диагностики */
    float (*get_volume_db)(void);
    bool (*get_mute)(void);
    int (*get_filter)(void);
    int (*get_fir)(void);
} ui_hooks_t;

/* bus — та же I2C-шина, что и для ЦАПа (GT911 сидит на ней же, 0x5D/0x14). */
esp_err_t ui_init(i2c_master_bus_handle_t bus, const ui_hooks_t *hooks);

void ui_set_lang(ui_lang_t lang);
ui_lang_t ui_get_lang(void);
