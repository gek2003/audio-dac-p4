/*
 * ============================================================================
 *  dsd_dec.h — потоковый DSD-парсер (DSF / DFF) с упаковкой в DoP
 *
 *  Задача: читать DSD-файлы (DSF — Sony, DFF — DSDIFF) с SD-карты и выдавать
 *  их в виде DoP-потока (DSD over PCM) для ES9039Q2M:
 *
 *    DSD-файл ──► dsd_read_dop() ──► int32 сэмплы (MSB-aligned 24 бит) ──►
 *      i2s_out_write (32-бит слоты stereo) ──► ES9039Q2M (вход DoP,
 *      es9039_set_input(ES9039_INPUT_DOP)) ──► DSD-тракт чипа
 *
 *  DoP-упаковка (стандарт DoP open standard, 24-бит, маркер 0x05/0xFA):
 *    левый слот:  [marker:8][DSD_L:8][0:8]
 *    правый слот: [marker:8][DSD_R:8][0:8]
 *    Маркер чередуется 0x05 <-> 0xFA каждые 256 стерео-фреймов (старт 0x05).
 *    Частота LRCK = DSD_rate/16: DSD64 (2.8224М) -> 176.4к, DSD128 -> 352.8к,
 *    DSD256 -> 705.6к, DSD64-48k (3.072М) -> 192к.
 *
 *  Поддержка:
 *    - DSF (little-endian, channel-blocked по 4096 байт, bits_order 0=MSB /
 *      1=LSB first); bits_per_sample=1 — штатно, 8 — трактуется как 1 (см.
 *      ограничение в README).
 *    - DFF/DSDIFF (big-endian, FRM8, данные chunk "DSD " — интерливированы
 *      по байтам, MSB first); chunk "DST " (сжатый DSD) не поддерживается.
 *  Читается потоково через stdio (fopen/fread), без ringbuf.
 * ============================================================================
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

typedef struct dsd_ctx_s *dsd_handle_t;

typedef struct {
    uint32_t    dsd_rate;        /* Гц DSD-сэмплов: 2822400 = DSD64 и т.д. */
    uint8_t     channels;        /* число каналов (1..2 поддерживаются) */
    uint32_t    dop_fs;          /* частота шины для DoP: dsd_rate/16 */
    const char *fmt_name;        /* "DSF" или "DFF" */
    uint64_t    samples_per_ch;  /* DSD-сэмплов на канал */
    uint32_t    seconds;         /* оценка длительности */
} dsd_info_t;

/* Открыть файл, распарсить заголовок (DSF или DFF по сигнатуре).
 * out — метаданные (частота DSD, каналы, DoP-частота), h — контекст. */
esp_err_t dsd_open(const char *path, dsd_info_t *out, dsd_handle_t *h);

/* Прочитать и упаковать DoP-фреймы. out — массив int32 (пары L/R,
 * MSB-aligned 24 бита, маркер в битах 31..24). Возврат: число записанных
 * int32-сэмплов (кратно 2). eof=true — данные кончились. */
size_t dsd_read_dop(dsd_handle_t h, int32_t *out, size_t max_samples, bool *eof);

/* Прогресс файла (для UI): прочитано/всего байтов данных на канал. */
void dsd_get_progress(dsd_handle_t h, uint64_t *pos, uint64_t *total);

/* Перемотка DSD-файла на миллисекунды (seek по фиксированным кадрам). */
esp_err_t dsd_seek_ms(dsd_handle_t h, uint32_t ms);

/* Частота DSD-сэмплов файла (0 если не открыт). */
uint32_t dsd_get_rate(dsd_handle_t h);

/* ID3-метаданные в конце DSF (metadata pointer из заголовка). */
uint64_t dsd_get_id3_offset(dsd_handle_t h);
size_t   dsd_read_id3(dsd_handle_t h, uint8_t *buf, size_t cap);

void dsd_close(dsd_handle_t h);

/* true, если это file://-URL с расширением .dsf/.dff */
bool dsd_is_dsd_path(const char *url);
