/*
 * ============================================================================
 *  dst_dec.h — декодер DST (Direct Stream Transfer) для ESP32-P4
 * ============================================================================
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct dst_ctx_s *dst_handle_t;

/* Метаданные DST-потока */
typedef struct {
    uint32_t dsd_rate;        /* Гц DSD-бит (2822400 = DSD64) */
    uint8_t  channels;        /* 1..2 (плеер поддерживает стерео) */
    uint32_t samples_per_frame;   /* DSD-бит на канал в одном фрейме (fs/75) */
    uint32_t frame_bytes;     /* декодированных интерлив-байт на фрейм */
} dst_info_t;

/* Отдельный фрейм: без сохранения состояния между фреймами (каждый самодостаточен) */
dst_handle_t dst_open(uint32_t dsd_rate, uint8_t channels);
void         dst_close(dst_handle_t h);

/* Декодировать один DST-фрейм.
 * data/len — DST-данные фрейма (содержимое DSTF-чанка, включая заголовок фрейма).
 * out — буфер размером frame_bytes (интерлив L,R, MSB-first, как в DFF).
 * Возврат ESP_OK или ошибка фрейма (фрейм пропускается вызывающим кодом). */
esp_err_t dst_decode_frame(dst_handle_t h, const uint8_t *data, size_t len, uint8_t *out);
