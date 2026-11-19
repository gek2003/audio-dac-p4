/*
 * ============================================================================
 *  es9039q2m_esp32p4_driver.h — публичный API драйвера ES9039Q2M
 *  (SW-режим, I²C 0x48; MCLK от локального генератора 49,152 МГц)
 * ============================================================================
 */
#ifndef ES9039Q2M_ESP32P4_DRIVER_H
#define ES9039Q2M_ESP32P4_DRIVER_H

/* I²C-адрес ES9039Q2M: ADDR0=ADDR1=GND, Table 3 (стр. 13 даташита). */
#define ES9039_I2C_ADDR_7BIT    0x48

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Инициализация: soft reset -> тракт -> тактирование -> вход -> фильтр ->
 * громкость -> unmute -> проверка SRC lock. GPIO1 платы (automute) опционально. */
esp_err_t es9039_init(i2c_master_dev_handle_t i2c, int gpio1_pin);

/* Диагностический скан шины: 0x40..0x4F, подтвердить перепайку в SW-режим. */
esp_err_t es9039_scan_bus(i2c_master_bus_handle_t bus);

/* Громкость 0..-127.5 дБ, шаг 0.5 дБ (Reg 74/75). */
esp_err_t es9039_set_volume_db(float db);

/* Переключение встроенного пресета FIR (Reg 88): 8 значений, см. enum ниже. */
typedef enum {
    ES9039_FIR_MIN_PHASE          = 0,
    ES9039_FIR_LP_APODIZING_FAST  = 1,
    ES9039_FIR_LP_FAST            = 2,
    ES9039_FIR_LP_FAST_LOW_RIPPLE = 3,
    ES9039_FIR_LP_SLOW            = 4,
    ES9039_FIR_MP_FAST            = 5,
    ES9039_FIR_MP_SLOW            = 6,
    ES9039_FIR_MP_SLOW_LOW_DISP   = 7,
} es9039_fir_t;
esp_err_t es9039_set_filter(es9039_fir_t f);

esp_err_t es9039_mute(bool on);

typedef enum { ES9039_INPUT_PCM = 0, ES9039_INPUT_DSD = 1, ES9039_INPUT_DOP = 2 } es9039_input_t;
esp_err_t es9039_set_input(es9039_input_t in);

/* Automute: путь A — GPIO1 платы на пин ESP32; путь B — readback по I²C. */
bool      es9039_automute_status_pin(void);
esp_err_t es9039_automute_status_i2c(bool *muted_ch1, bool *muted_ch2);

/* SRC lock = Reg 239[7] IDAC_DIV_RATIO_VALID. */
esp_err_t es9039_src_locked(bool *locked);

/* Переключение частоты «на лету»: mute -> apply_cb (пересборка I²S) ->
 * заливка FIR-набора под fs_hz (es9039_fir.c) -> ожидание SRC lock -> unmute.
 * apply_cb вызывается в контексте вызывающей задачи. */
esp_err_t es9039_switch_frequency(void (*apply_cb)(void *), void *arg,
                                  uint32_t lock_timeout_ms, uint32_t fs_hz);

/* --- Снимок диагностики (readback-регистры) --- */
typedef struct {
    bool     ratio_valid;
    bool     spdif_valid;
    bool     tdm_data_valid;
    bool     dop_valid;
    uint8_t  input_select;
    bool     bck_ws_fail;
    bool     dop_valid_int;
    bool     automute_state;
    bool     vol_min_state;
    bool     automute_read;
    bool     soft_ramping;
    uint8_t  idac_div_auto;
    bool     idac_half;
    uint32_t fs_estimate_hz;
    uint8_t  chip_id;
} es9039_diag_t;
esp_err_t es9039_diag_read(es9039_diag_t *d, uint32_t mclk_hz);

esp_err_t es9039_set_dpll_bw(uint8_t bw);        /* 1..15, Reg 29[7:4] */
esp_err_t es9039_clear_interrupts(void);
esp_err_t es9039_set_thd_comp(uint8_t ch, int16_t c2, int16_t c3);  /* ch 1|2 */
esp_err_t es9039_thd_comp_clear(void);

/* --- Низкоуровневый доступ для FIR-модуля (es9039_fir.c) ---
 * Обёртки над статическими es9039_write/es9039_read; сильные символы
 * замещают слабые заглушки es9039_fir.c при линковке. */
esp_err_t es9039_write_reg(uint8_t reg, uint8_t val);
esp_err_t es9039_read_reg(uint8_t reg, uint8_t *val);

#endif /* ES9039Q2M_ESP32P4_DRIVER_H */
