/*
 * ============================================================================
 *  dsd_native.h — Native DSD вывод на ES9039Q2M (без DoP-обёртки)
 *
 *  Даташит ES9039Q2M: native DSD принимает по линиям
 *      DATA1 = DSD CH1 (L), DATA2 = DSD CH2 (R), DATA_CLK = бит-клок DSD.
 *  BCK/WS в этом режиме чипом не используются. В SOFTWARE-режиме вход
 *  переключается es9039_set_input(ES9039_INPUT_DSD) (Reg1=0x82 + Reg57).
 *
 *  Требования железа (ПРОВЕРИТЬ ПО СХЕМЕ ПЛАТЫ):
 *      - GPIO_DSD_DATA1 (штатный DATA-пин платы, GPIO48) -> пин DATA1 чипа
 *        (обычно уже разведён как «DATA»);
 *      - GPIO_DSD_DATA2 (новый пин, по умолчанию GPIO49) -> пин DATA2 чипа
 *        (пин 14) — Скорее всего требует пайки;
 *      - GPIO_DSD_CLK (новый пин, по умолчанию GPIO50) -> пин DATA_CLK чипа
 *        (пин 12) — Скорее всего требует пайки.
 *  Если на плате нет дорожек DATA2/DATA_CLK — native DSD недоступен, остаётся
 *      DoP (двоичный/штатный тракт, до DSD256 при достаточной шине).
 *
 *  Схема: два I²S-порта P4:
 *      порт A — TX MASTER, слоты 8 бит, dout -> DATA1, BCLK = DSD bit rate;
 *      порт B — TX SLAVE,    слоты 8 бит, dout -> DATA2, клок/фаза — от порт A.
 *  Оба порта пишутся из одной задачи, скорость согласована (не теряется).
 * ============================================================================
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Запустить native-DSD вывод на заданной DSD-частоте (2822400 = DSD64).
 * Внутри: глушит ЦАП, переключает вход ES9039 на native DSD, поднимает
 * два I²S-порта (A master, B slave). */
esp_err_t dsd_native_start(uint32_t dsd_rate);

/* Остановить: закрыть порты, вернуть ES9039 на PCM-вход. */
void      dsd_native_stop(void);

bool      dsd_native_running(void);

/* Записать DSD-байты (интерлив L,R, MSB-first — как в DFF «DSD »-чанке).
 * bytes должен быть кратен 2 (пара байт L/R). Блокирует до полной записи
 * в оба порта. */
esp_err_t dsd_native_write(const uint8_t *inter, size_t bytes);

/* Синхронизация под DSD-поток (для предупреждений о недогрузке). */
uint32_t  dsd_native_rate(void);
