/*
 * es9039_fir.h — загрузка пользовательских FIR-фильтров в ES9039Q2M (SW-режим, I2C).
 *
 * Механика по даташиту ES9039Q2M v0.1.3, стр. 30 и 80–81:
 *   8x-интерполятор = 2 фильтра: ступень x2 (RAM 128 коэфф, адреса 0x00..0x7F)
 *   и ступень x4 (RAM 32 коэфф, адреса 0x80..0x9F в терминах даташита; при
 *   PROG_COEFF_STAGE=1 адресация 0x00..0x1F).
 *   Регистры:
 *     Reg 135: бит1 PROG_COEFF_WE — строб записи в RAM;
 *              бит0 PROG_COEFF_EN — 1 = использовать пользовательские коэффициенты,
 *                                   0 = встроенный пресет из Reg 88 (FILTER_SHAPE).
 *     Reg 137: бит7 PROG_COEFF_STAGE (0 = ступень 2x, 1 = ступень 4x),
 *              биты [6:0] — адрес коэффициента.
 *     Reg 138..140: 24-битный знаковый коэффициент ([7:0], [15:8], [23:16]).
 *     Reg 246..248: PROG_COEFF_OUT — обратное чтение записанного коэффициента.
 *
 * Интеграция: модуль вызывает низкоуровневые es9039_write_reg()/es9039_read_reg().
 * Если в вашем драйвере функции называются иначе — поправьте макросы ниже.
 *
 * Поток: es9039_fir_apply_for_fs() вызывается из es9039_switch_frequency()
 * (es9039q2m_esp32p4_driver.c) при каждой смене частоты потока — RAM чипа
 * не переживает смену FS «бесплатно», набор перезаливается. Ручной выбор
 * из UI (es9039_fir_set_manual_mode) приоритетнее авто-подмены.
 */
#ifndef ES9039_FIR_H
#define ES9039_FIR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "es9039_fir_coeff.h"

/* --- адаптация к низкоуровневому доступу драйвера ------------------------- */
/* Если имена отличаются — замените здесь. Реализации объявлены слабыми. */
#define FIR_WRITE_REG(reg, val) es9039_write_reg((uint8_t)(reg), (uint8_t)(val))
#define FIR_READ_REG(reg, pval) es9039_read_reg((uint8_t)(reg), (pval))
esp_err_t es9039_write_reg(uint8_t reg, uint8_t val);
esp_err_t es9039_read_reg(uint8_t reg, uint8_t *val);

/* --- регистры PROGRAM RAM (даташит, стр. 80–81) --------------------------- */
#define ES9039_REG_PROG_RAM_CTRL   135u  /* [1] WE, [0] EN        */
#define ES9039_REG_PROG_RAM_ADDR   137u  /* [7] stage, [6:0] addr */
#define ES9039_REG_PROG_RAM_DATA0  138u  /* [7:0]   коэффициента  */
#define ES9039_REG_PROG_RAM_DATA1  139u  /* [15:8]                */
#define ES9039_REG_PROG_RAM_DATA2  140u  /* [23:16]               */
#define ES9039_REG_PROG_RAM_OUT0   246u  /* readback [7:0]        */
#define ES9039_REG_PROG_RAM_OUT1   247u  /* readback [15:8]       */
#define ES9039_REG_PROG_RAM_OUT2   248u  /* readback [23:16]      */

/* Гипотезы маппинга полифазных фаз на адреса RAM (см. MD, раздел «Диагностика»). */
typedef enum {
    FIR_PHASE_BLOCK       = 0,  /* addr = phase * M + tap     (фазы блоками)  */
    FIR_PHASE_INTERLEAVED = 1,  /* addr = tap * R + phase     (чередование)   */
} es9039_fir_phase_map_t;

/* Сдвиг общего гейна в шагах 2^-23: подлежит калибровке по замеру (TODO). */
#define ES9039_FIR_GAIN_SHIFT 0

/**
 * Записать один набор коэффициентов в RAM одной ступени.
 * stage: 0 = ступень 2x (128 коэфф), 1 = ступень 4x (32 коэфф).
 */
esp_err_t es9039_fir_load_stage(uint8_t stage, const int32_t *coeffs, size_t n);

/**
 * Полная загрузка набора: обе ступени + включение PROG_COEFF_EN.
 * Если verify=true — проверяет чтением несколько контрольных адресов.
 */
esp_err_t es9039_fir_apply(const es9039_fir_set_t *set, bool verify);

/** Выбор набора под частоту входного потока (44100/48000/96000).
 *  Если точного совпадения нет — ВЕРНУТЬСЯ к встроенному пресету (EN=0)
 *  и вернуть ESP_ERR_NOT_FOUND. */
esp_err_t es9039_fir_apply_for_fs(uint32_t fs_hz, bool verify);

/** Вернуться к встроенному пресету (PROG_COEFF_EN = 0), RAM не трогаем. */
esp_err_t es9039_fir_use_builtin(void);

/** Прочитать коэффициент из RAM для диагностики. */
esp_err_t es9039_fir_readback(uint8_t stage, uint8_t addr, int32_t *out);

/** Выбор гипотезы маппинга полифазных фаз (диагностика, см. MD).
 *  Вызывать ДО первой загрузки набора. */
void es9039_fir_set_phase_map(es9039_fir_phase_map_t map);

/** Ручной выбор приоритетнее авто. При enable=true вызовы
 *  es9039_fir_apply_for_fs() не меняют активный набор: авто-подмена при смене
 *  частоты потока пропускается (с предупреждением в лог, если ручной набор
 *  рассчитан под другую fs). Вернуться к авто: set_manual_mode(false) —
 *  набор под текущую fs зальётся при ближайшей смене частоты. */
void es9039_fir_set_manual_mode(bool enable);
bool es9039_fir_get_manual_mode(void);

/** Загрузить диагностическую дельту (прозрачный тракт) в обе ступени. */
esp_err_t es9039_fir_load_delta(void);

#endif /* ES9039_FIR_H */
