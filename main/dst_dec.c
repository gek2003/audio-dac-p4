/*
 * ============================================================================
 *  dst_dec.c — декодер DST (Direct Stream Transfer), ISO/IEC 14496-3 subpart 10
 *
 *  Перенос с libavcodec/dstdec.c (FFmpeg, LGPL-2.1+, (c) 2014 Peter Ross)
 *  на чистый C без ffmpeg-зависимостей (get_bits, arithmetic coder, golomb).
 *  Поддерживает до 2 каналов (плеер — стерео), DSD64/128/256 (кратно 44100).
 *  Декодер кадро-независимый: каждый DST-фрейм декодируется с нуля —
 *  это упрощает перемотку (seek по DSTF-чанкам).
 * ============================================================================
 */

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "dst_dec.h"

/* В esp_err.h нет ESP_ERR_INVALID_DATA (нет такого кода в IDF) —
 * аллеем на существующий INVALID_RESPONSE: семантика для вызывающего
 * кода идентична (повреждённые данные/фрейм). */
#ifndef ESP_ERR_INVALID_DATA
#define ESP_ERR_INVALID_DATA ESP_ERR_INVALID_RESPONSE
#endif

static const char *TAG = "dst_dec";

#define DST_MAX_CHANNELS 2
#define DST_MAX_ELEMENTS (2 * DST_MAX_CHANNELS)

/* LGPL-2.1+ код перенесён из FFmpeg (libavcodec/dstdec.c, (c) 2014 Peter Ross).
 * Файл обязан сохранять копию лицензии при распространении. */

typedef struct {
    uint32_t a;
    uint32_t c;
} arith_t;

typedef struct {
    unsigned elements;
    unsigned length[DST_MAX_ELEMENTS];
    int coeff[DST_MAX_ELEMENTS][128];
} table_t;

struct dst_ctx_s {
    uint32_t dsd_rate;
    uint8_t  channels;
    uint32_t samples_per_frame;     /* DSD-бит на канал */
    uint32_t frame_bytes;           /* байт интерлива на фрейм */

    /* поразрядный читатель (MSB-first) */
    const uint8_t *bit_buf;
    size_t  bit_len;                /* в битах */
    size_t  bit_pos;

    table_t fsets;
    table_t probs;
    arith_t ac;

    int16_t filter[DST_MAX_ELEMENTS][16][256];
    uint8_t status[DST_MAX_CHANNELS][16];
};

/* ------------------------- GetBitContext (упрощённый) ---------------------- */

static void gb_init(dst_handle_t s, const uint8_t *data, size_t len) {
    s->bit_buf = data;
    s->bit_len = len * 8;
    s->bit_pos = 0;
}

static int gb_get_bits(dst_handle_t s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s->bit_pos >= s->bit_len) return -1;    /* конец данных */
        v = (v << 1) | ((s->bit_buf[s->bit_pos >> 3] >> (7 - (s->bit_pos & 7))) & 1);
        s->bit_pos++;
    }
    return v;
}

static int gb_get_sbits(dst_handle_t s, int n) {
    int v = gb_get_bits(s, n);
    if (v < 0) return v;
    int sign = 1 << (n - 1);
    if (v & sign) v -= (1 << n);
    return v;
}

static int gb_get_bits_left(dst_handle_t s) {
    return (int)(s->bit_len - s->bit_pos);
}

/* get_ur_golomb_jpegls — сверено с ffmpeg libavcodec/golomb.h (LGPL-2.1+),
 * не-cached ветка: значение = (i << k) + bits, где i = число считанных нулей
 * (unary), bits — следующие k бит. limit = биты до конца потока (esc_len = 0,
 * как вызывает DST-декодер). Возврат -1 на ошибке/обрыве потока. Знаковый
 * бит читается отдельно в get_sr_golomb_dst (только при v != 0). P4c-2/P1-4:
 * сверено с ffmpeg libavcodec/dstdec.c бит-в-бит. */
static int get_ur_golomb_jpegls(dst_handle_t s, int k) {
    int i;
    int limit = gb_get_bits_left(s);
    if (limit <= 0) return -1;
    for (i = 0; i < limit && gb_get_bits(s, 1) == 0; i++) { }
    if (i >= limit - 1) return -1;          /* esc_len == 0: край = ошибка */
    int buf = 0;
    if (k > 0) {
        buf = gb_get_bits(s, k);
        if (buf < 0) return -1;
    }
    return (i << k) + buf;
}

static int get_sr_golomb_dst(dst_handle_t s, unsigned int k) {
    int v = get_ur_golomb_jpegls(s, k);
    if (v && gb_get_bits(s, 1)) v = -v;
    return v;
}

/* ff_reverse[128] (как в ffmpeg libavutil/reverse.c) */
static const uint8_t ff_reverse128[128] = {
    0x00, 0x40, 0x20, 0x60, 0x10, 0x50, 0x30, 0x70, 0x08, 0x48, 0x28, 0x68, 0x18, 0x58, 0x38, 0x78,
    0x04, 0x44, 0x24, 0x64, 0x14, 0x54, 0x34, 0x74, 0x0C, 0x4C, 0x2C, 0x6C, 0x1C, 0x5C, 0x3C, 0x7C,
    0x02, 0x42, 0x22, 0x62, 0x12, 0x52, 0x32, 0x72, 0x0A, 0x4A, 0x2A, 0x6A, 0x1A, 0x5A, 0x3A, 0x7A,
    0x06, 0x46, 0x26, 0x66, 0x16, 0x56, 0x36, 0x76, 0x0E, 0x4E, 0x2E, 0x6E, 0x1E, 0x5E, 0x3E, 0x7E,
    0x01, 0x41, 0x21, 0x61, 0x11, 0x51, 0x31, 0x71, 0x09, 0x49, 0x29, 0x69, 0x19, 0x59, 0x39, 0x79,
    0x05, 0x45, 0x25, 0x65, 0x15, 0x55, 0x35, 0x75, 0x0D, 0x4D, 0x2D, 0x6D, 0x1D, 0x5D, 0x3D, 0x7D,
    0x03, 0x43, 0x23, 0x63, 0x13, 0x53, 0x33, 0x73, 0x0B, 0x4B, 0x2B, 0x6B, 0x1B, 0x5B, 0x3B, 0x7B,
    0x07, 0x47, 0x27, 0x67, 0x17, 0x57, 0x37, 0x77, 0x0F, 0x4F, 0x2F, 0x6F, 0x1F, 0x5F, 0x3F, 0x7F,
};

static uint8_t prob_dst_x_bit(int c) {
    return (uint8_t)((ff_reverse128[c & 127] >> 1) + 1);
}

static unsigned av_log2_(unsigned v) {
    unsigned r = 0;
    while (v >>= 1) r++;
    return r;
}

/* ------------------------------ DST-таблицы -------------------------------- */

static const int8_t fsets_code_pred_coeff[3][3] = {
    {  -8 },
    { -16,  8 },
    {  -9, -5, 6 },
};

static const int8_t probs_code_pred_coeff[3][3] = {
    {  -8 },
    { -16,  8 },
    { -24, 24, -8 },
};

static int read_map(dst_handle_t s, table_t *t, unsigned int map[], int channels) {
    int ch;
    t->elements = 1;
    map[0] = 0;
    if (!gb_get_bits(s, 1)) {
        for (ch = 1; ch < channels; ch++) {
            int bits = (int)av_log2_(t->elements) + 1;
            int v = gb_get_bits(s, bits);
            if (v < 0) return -1;
            map[ch] = (unsigned)v;
            if (map[ch] == t->elements) {
                t->elements++;
                if (t->elements >= DST_MAX_ELEMENTS) return -1;
            } else if (map[ch] > t->elements) {
                return -1;
            }
        }
    } else {
        memset(map, 0, sizeof(*map) * DST_MAX_CHANNELS);
    }
    return 0;
}

static void read_uncoded_coeff(dst_handle_t s, int *dst, unsigned elements,
                               int coeff_bits, int is_signed, int offset) {
    for (unsigned i = 0; i < elements; i++) {
        dst[i] = (is_signed ? gb_get_sbits(s, coeff_bits) : gb_get_bits(s, coeff_bits)) + offset;
    }
}

static int read_table(dst_handle_t s, table_t *t, const int8_t code_pred_coeff[3][3],
                      int length_bits, int coeff_bits, int is_signed, int offset) {
    for (unsigned i = 0; i < t->elements; i++) {
        int l = gb_get_bits(s, length_bits);
        if (l < 0) return -1;
        t->length[i] = (unsigned)l + 1;
        if (!gb_get_bits(s, 1)) {
            read_uncoded_coeff(s, t->coeff[i], t->length[i], coeff_bits, is_signed, offset);
        } else {
            int method = gb_get_bits(s, 2);
            if (method == 3) return -1;
            read_uncoded_coeff(s, t->coeff[i], (unsigned)method + 1, coeff_bits, is_signed, offset);
            int lsb_size = gb_get_bits(s, 3);
            for (unsigned j = method + 1; j < t->length[i]; j++) {
                int c, x = 0;
                for (int k = 0; k < method + 1; k++)
                    x += code_pred_coeff[method][k] * t->coeff[i][j - k - 1];
                c = get_sr_golomb_dst(s, lsb_size);
                if (x >= 0) c -= (x + 4) / 8;
                else        c += (-x + 3) / 8;
                if (!is_signed) {
                    if (c < offset || c >= offset + (1 << coeff_bits)) return -1;
                }
                t->coeff[i][j] = c;
            }
        }
    }
    return 0;
}

static void ac_init(dst_handle_t s) {
    s->ac.a = 4095;
    s->ac.c = (uint32_t)gb_get_bits(s, 12);
}

static inline void ac_get(dst_handle_t s, int p, int *e) {
    unsigned int k = (s->ac.a >> 8) | ((s->ac.a >> 7) & 1);
    unsigned int q = k * (unsigned)p;
    unsigned int a_q = s->ac.a - q;

    *e = (s->ac.c < a_q);
    if (*e) {
        s->ac.a = a_q;
    } else {
        s->ac.a = q;
        s->ac.c -= a_q;
    }
    if (s->ac.a < 2048) {
        int n = 11 - (int)av_log2_(s->ac.a);
        s->ac.a <<= n;
        int nb = gb_get_bits(s, n);
        if (nb < 0) nb = 0;   /* P4c-2/P1-5: EOF — добор нулями за концом потока, как в ffmpeg */
        uint32_t new_bits = (uint32_t)nb;
        s->ac.c = (s->ac.c << n) | new_bits;
    }
}

static int build_filter(dst_handle_t s) {
    for (unsigned i = 0; i < s->fsets.elements; i++) {
        int length = (int)s->fsets.length[i];
        for (int j = 0; j < 16; j++) {
            int total = length - j * 8;
            if (total > 8) total = 8;
            if (total < 0) total = 0;
            for (int k = 0; k < 256; k++) {
                int64_t v = 0;
                for (int l = 0; l < total; l++)
                    v += (((k >> l) & 1) * 2 - 1) * s->fsets.coeff[i][j * 8 + l];
                if ((int16_t)v != v) return -1;
                s->filter[i][j][k] = (int16_t)v;
            }
        }
    }
    return 0;
}

/* ------------------------------ Открытие ----------------------------------- */

dst_handle_t dst_open(uint32_t dsd_rate, uint8_t channels) {
    if (channels == 0 || channels > 2) return NULL;
    /* ISO/IEC 14496-3 допускает 64/128/256 × 44100; терпим кратность */
    if (dsd_rate < 2822400 || dsd_rate > 512 * 44100) return NULL;
    if ((588ULL * ((uint64_t)dsd_rate / 44100)) & 7) return NULL;

    dst_handle_t s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    s->dsd_rate = dsd_rate;
    s->channels = channels;
    s->samples_per_frame = 588u * (dsd_rate / 44100);
    s->frame_bytes = s->samples_per_frame / 8 * channels;
    return s;
}

void dst_close(dst_handle_t h) {
    if (h) free(h);
}

/* --------------------------- Декодирование фрейма -------------------------- */

esp_err_t dst_decode_frame(dst_handle_t s, const uint8_t *data, size_t len, uint8_t *out) {
    if (s == NULL || data == NULL || out == NULL || len <= 1) return ESP_ERR_INVALID_ARG;
    unsigned map_fe[DST_MAX_CHANNELS], map_pe[DST_MAX_CHANNELS];
    unsigned i, ch, dst_x_bit;
    unsigned half_prob[DST_MAX_CHANNELS];
    const int channels = s->channels;

    gb_init(s, data, len);

    if (!gb_get_bits(s, 1)) {
        /* Несжатый (короткий) фрейм: сырые DSD-байты, паддинг 0x69 */
        gb_get_bits(s, 1);
        if (gb_get_bits(s, 6) != 0) return ESP_ERR_INVALID_DATA;
        unsigned total = s->frame_bytes;
        unsigned n = (unsigned)(len - 1) < total ? (unsigned)(len - 1) : total;
        memset(out, 0, total);
        memcpy(out, data + 1, n);
        memset(out + n, 0x69, total - n);
        return ESP_OK;
    }

    /* Сегментация (10.4–10.6): только «same segmentation» поддержана ffmpeg-декодером */
    if (!gb_get_bits(s, 1)) return ESP_ERR_NOT_SUPPORTED;   /* not same segmentation */
    if (!gb_get_bits(s, 1)) return ESP_ERR_NOT_SUPPORTED;   /* not same seg for all channels */
    if (!gb_get_bits(s, 1)) return ESP_ERR_NOT_SUPPORTED;   /* not end of channel segmentation */

    unsigned same_map = (unsigned)gb_get_bits(s, 1);

    if (read_map(s, &s->fsets, map_fe, channels) < 0) return ESP_ERR_INVALID_DATA;
    if (same_map) {
        s->probs.elements = s->fsets.elements;
        memcpy(map_pe, map_fe, sizeof(map_fe));
    } else {
        if (read_map(s, &s->probs, map_pe, channels) < 0) return ESP_ERR_INVALID_DATA;
    }

    for (ch = 0; ch < (unsigned)channels; ch++)
        half_prob[ch] = (unsigned)gb_get_bits(s, 1);

    if (read_table(s, &s->fsets, fsets_code_pred_coeff, 7, 9, 1, 0) < 0) return ESP_ERR_INVALID_DATA;
    if (read_table(s, &s->probs, probs_code_pred_coeff, 6, 7, 0, 1) < 0) return ESP_ERR_INVALID_DATA;

    if (gb_get_bits(s, 1)) return ESP_ERR_INVALID_DATA;
    ac_init(s);

    if (build_filter(s) < 0) return ESP_ERR_INVALID_DATA;

    memset(s->status, 0xAA, sizeof(s->status));
    memset(out, 0, s->frame_bytes);

    ac_get(s, prob_dst_x_bit(s->fsets.coeff[0][0]), (int *)&dst_x_bit);

    for (i = 0; i < s->samples_per_frame; i++) {
        for (ch = 0; ch < (unsigned)channels; ch++) {
            const unsigned felem = map_fe[ch];
            int16_t (*filter)[256] = s->filter[felem];
            uint8_t *status = s->status[ch];
            int prob, residual, v;

            const int16_t predict = filter[0][status[0]] + filter[1][status[1]] +
                                    filter[2][status[2]] + filter[3][status[3]] +
                                    filter[4][status[4]] + filter[5][status[5]] +
                                    filter[6][status[6]] + filter[7][status[7]] +
                                    filter[8][status[8]] + filter[9][status[9]] +
                                    filter[10][status[10]] + filter[11][status[11]] +
                                    filter[12][status[12]] + filter[13][status[13]] +
                                    filter[14][status[14]] + filter[15][status[15]];

            if (!half_prob[ch] || i >= s->fsets.length[felem]) {
                unsigned pelem = map_pe[ch];
                unsigned index = (unsigned)(predict < 0 ? -predict : predict) >> 3;
                unsigned pl = s->probs.length[pelem];
                prob = s->probs.coeff[pelem][(index < pl) ? index : pl - 1];
            } else {
                prob = 128;
            }

            ac_get(s, prob, &residual);
            v = ((predict >> 15) ^ residual) & 1;
            out[(i >> 3) * (unsigned)channels + ch] |= (uint8_t)(v << (7 - (i & 0x7)));

            /* сдвиг кольцевого статуса (16 байт little-endian, как в ffmpeg) */
            uint64_t st_hi = 0, st_lo = 0;
            for (int b = 7; b >= 0; b--) st_hi = (st_hi << 8) | status[8 + b];
            for (int b = 7; b >= 0; b--) st_lo = (st_lo << 8) | status[b];
            uint64_t new_hi = (st_hi << 1) | (st_lo >> 63);
            uint64_t new_lo = (st_lo << 1) | (uint64_t)v;
            for (int b = 7; b >= 0; b--) status[8 + b] = (uint8_t)(new_hi >> (8 * b));
            for (int b = 7; b >= 0; b--) status[b]     = (uint8_t)(new_lo >> (8 * b));
        }
    }
    return ESP_OK;
}
