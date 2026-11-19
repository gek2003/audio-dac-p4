/*
 * ============================================================================
 *  dsd_dec.c — потоковый DSD-парсер (DSF/DFF) и упаковка в DoP
 *
 *  Подробности форматов и маркера — см. dsd_dec.h.
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "esp_log.h"

#include "dsd_dec.h"

static const char *TAG = "dsd_dec";

#define DSD_BLOCK_SIZE_DSF   4096   /* блок данных на канал в DSF */
#define DSD_MAX_CHANNELS     2      /* DoP — стерео (mono дублируется) */
#define DSD_CHUNK_DFF        16384  /* чанк чтения DFF */

typedef enum { DSD_FMT_DSF = 0, DSD_FMT_DFF = 1 } dsd_format_t;

struct dsd_ctx_s {
    FILE     *f;
    dsd_format_t fmt;
    uint32_t dsd_rate;             /* Гц DSD-сэмплов */
    uint8_t  channels;
    bool     msb_first;            /* порядок DSD-бит внутри байта */
    uint8_t  bits_per_sample;      /* DSF: 1 или 8 */
    uint64_t bytes_left_per_ch;    /* сколько валидных байтов данных осталось (на канал) */
    uint64_t bytes_total_per_ch;   /* всего валидных байтов данных (на канал) */
    uint64_t samples_per_ch;       /* DSD-сэмплов на канал (для длительности) */
    uint64_t data_off;             /* офсет данных в файле (DSF и DFF) */
    uint64_t id3_off;              /* офсет ID3-метаданных (DSF: metadata ptr; DFF: 0) */

    /* DSF: channel-blocked данные (супергруппа = block_size байт на канал) */
    uint8_t *super;                /* channels * block_size */
    uint32_t block_size;
    uint32_t super_pos;            /* индекс байта внутри супергруппы */
    uint32_t super_valid;          /* валидных байтов в супергруппе (min по каналам) */

    /* DFF: потоковые данные (интерливированы по байтам) */
    uint8_t *stream_buf;
    size_t   stream_len, stream_pos;
    uint64_t stream_left;          /* байт данных, оставшихся в "DSD " chunk */

    uint8_t  rev8[256];            /* таблица bit-reverse (для LSB-first DSF) */

    /* DoP-состояние */
    uint32_t frame_cnt;            /* стерео-фреймов с начала потока (для маркера) */
};

typedef struct dsd_ctx_s dsd_ctx_t;

/* --------------------------- DSF ------------------------------------------ */

/* Все поля DSF — little-endian */
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }

static esp_err_t parse_dsf(dsd_ctx_t *ctx)
{
    /* Заголовок DSF до данных всегда = "DSD " (28) + "fmt " (52) + "data " (24)
     * — читаем буфером с запасом (ID3-метаданные в DSF лежат В КОНЦЕ файла). */
    uint8_t hdr[512];
    size_t n = fread(hdr, 1, sizeof(hdr), ctx->f);
    if (n < 128 || memcmp(hdr, "DSD ", 4) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* DSD chunk: id(4) + size(8) + total_file_size(8) + metadata_ptr(8 LE) */
    uint64_t id3_ptr = le64(hdr + 16);

    /* fmt chunk: ищем по маркеру "fmt " (не полагаемся на точные офсеты) */
    int fmt_off = -1, data_off = -1;
    for (size_t i = 12; i + 8 <= n; i++) {
        if (fmt_off < 0 && memcmp(&hdr[i], "fmt ", 4) == 0) fmt_off = (int)i;
        if (data_off < 0 && memcmp(&hdr[i], "data", 4) == 0) data_off = (int)i;
        if (fmt_off >= 0 && data_off >= 0) break;
    }
    if (fmt_off < 0 || data_off < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint8_t *fm = &hdr[fmt_off];
    /* Структура fmt DSF (сверено с ffmpeg libavformat/dsfdec.c, chunk size = 52):
     *  0..3 "fmt ", 4..11 size, 12..15 version (=1), 16..19 format id (=0),
     *  20..23 channel type (u32), 24..27 channel num (u32),
     *  28..31 sampling frequency (u32, Гц DSD-бит), 32..35 bits per sample
     *  (u32: 1 = LSB first, 8 = MSB first — это И порядок, и ширина),
     *  36..43 sample count (u64, сэмплов/бит на канал), 44..47 block size (u32),
     *  48..51 reserved. Отдельного поля bits order в DSF нет. */
    ctx->channels        = (uint8_t)le32(fm + 24);
    ctx->dsd_rate        = le32(fm + 28);
    uint32_t bits_field  = le32(fm + 32);
    uint64_t sample_cnt  = le64(fm + 36);
    ctx->block_size      = le32(fm + 44);

    /* bits=1: сэмпл = 1 бит, LSB first (практически все DSF-файлы);
     * bits=8: сэмпл = 1 байт, MSB first (редкий формат, файл в 8 раз больше). */
    ctx->bits_per_sample = (bits_field == 8) ? 8 : 1;
    ctx->msb_first       = (bits_field == 8);

    if (ctx->channels == 0 || ctx->channels > DSD_MAX_CHANNELS) {
        ESP_LOGE(TAG, "DSF: каналов=%u — поддерживается 1..2", ctx->channels);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ctx->block_size == 0 || ctx->block_size > DSD_BLOCK_SIZE_DSF) {
        ESP_LOGW(TAG, "DSF: block_size=%u — берём %u", (unsigned)ctx->block_size, DSD_BLOCK_SIZE_DSF);
        ctx->block_size = DSD_BLOCK_SIZE_DSF;
    }
    if (ctx->dsd_rate < 2822400 || ctx->dsd_rate % 16 != 0) {
        ESP_LOGE(TAG, "DSF: частота %lu Гц не поддерживается (кратность 16 обязательна)",
                 (unsigned long)ctx->dsd_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* data chunk: id(4) + size(8) + reserved(12) -> данные */
    ctx->data_off = (uint64_t)data_off + 24;
    ctx->id3_off = id3_ptr;
    uint64_t data_size = le64(hdr + data_off + 4);

    /* валидные байты на канал: при bits=1 сэмпл=бит (байтов = ceil(n/8)),
     * при bits=8 сэмпл=байт (байтов = n) */
    uint64_t bytes_per_ch = (ctx->bits_per_sample == 1) ? ((sample_cnt + 7) / 8) : sample_cnt;
    ctx->bytes_left_per_ch = bytes_per_ch;
    ctx->bytes_total_per_ch = ctx->bytes_left_per_ch;
    ctx->samples_per_ch = sample_cnt;

    /* ограничение данных размером chunk'а (защита от кривых заголовков) */
    if (data_size / ctx->channels < bytes_per_ch) {
        ctx->bytes_left_per_ch = data_size / ctx->channels;
    }

    ctx->super = malloc((size_t)ctx->channels * ctx->block_size);
    if (ctx->super == NULL) return ESP_ERR_NO_MEM;
    ctx->super_pos = 0;
    ctx->super_valid = 0;

    if (fseek(ctx->f, (long)ctx->data_off, SEEK_SET) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* Прочитать следующую супергруппу DSF: channels * block_size байт.
 * Возврат: валидных байтов на канал (0 = EOF). */
static uint32_t dsf_read_super(dsd_ctx_t *ctx)
{
    if (ctx->bytes_left_per_ch == 0) return 0;
    uint32_t want = (ctx->bytes_left_per_ch < ctx->block_size)
                        ? (uint32_t)ctx->bytes_left_per_ch
                        : ctx->block_size;
    size_t got = fread(ctx->super, 1, (size_t)ctx->channels * ctx->block_size, ctx->f);
    uint32_t per_ch = (uint32_t)(got / ctx->channels);
    if (per_ch > ctx->block_size) per_ch = ctx->block_size;
    uint32_t valid = (per_ch < want) ? per_ch : want;   /* паддинг не читаем как данные */
    ctx->bytes_left_per_ch -= valid;
    ctx->super_pos = 0;
    ctx->super_valid = valid;
    return valid;
}

/* --------------------------- DFF ------------------------------------------ */

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static esp_err_t parse_dff(dsd_ctx_t *ctx)
{
    /* FRM8: id(4) + size(8 BE) + form type(4) — далее чанки id(4)+size(8 BE) */
    uint8_t head[16];
    if (fread(head, 1, 16, ctx->f) != 16) return ESP_ERR_NOT_SUPPORTED;
    if (memcmp(head, "FRM8", 4) != 0) return ESP_ERR_NOT_SUPPORTED;

    bool rate_ok = false, chnl_ok = false, cmpr_ok = false;
    bool have_dsd = false;
    uint64_t dsd_size = 0;

    while (!have_dsd) {
        uint8_t ch[12];
        if (fread(ch, 1, 12, ctx->f) != 12) break;
        uint64_t sz = be64(ch + 4);
        if (memcmp(ch, "FS ", 3) == 0 && sz >= 4) {
            uint8_t b[4];
            if (fread(b, 1, 4, ctx->f) != 4) break;
            ctx->dsd_rate = be32(b);
            rate_ok = true;
            if (sz > 4 && fseek(ctx->f, (long)(sz - 4), SEEK_CUR) != 0) break;
        } else if (memcmp(ch, "CHNL", 4) == 0 && sz >= 4) {
            uint8_t b[4];
            if (fread(b, 1, 4, ctx->f) != 4) break;
            ctx->channels = (uint8_t)be32(b);
            chnl_ok = true;
            if (sz > 4 && fseek(ctx->f, (long)(sz - 4), SEEK_CUR) != 0) break;
        } else if (memcmp(ch, "CMPR", 4) == 0 && sz >= 4) {
            uint8_t b[4];
            if (fread(b, 1, 4, ctx->f) != 4) break;
            if (memcmp(b, "DSD ", 4) != 0) {
                ESP_LOGE(TAG, "DFF: сжатие DST не поддерживается");
                return ESP_ERR_NOT_SUPPORTED;
            }
            cmpr_ok = true;
            if (sz > 4 && fseek(ctx->f, (long)(sz - 4), SEEK_CUR) != 0) break;
        } else if (memcmp(ch, "DSD ", 4) == 0) {
            dsd_size = sz;      /* данные начинаются сразу после заголовка чанка */
            ctx->data_off = (uint64_t)ftell(ctx->f);   /* начало данных (для seek) */
            have_dsd = true;
        } else if (sz == 0) {
            /* чанк без тела при нулевом размере: иначе цикл зациклится */
            break;
        } else {
            /* прочие чанки (ABSS, COMT, ...) — пропускаем */
            if (fseek(ctx->f, (long)sz, SEEK_CUR) != 0) break;
        }
        if (sz > (1ULL << 40)) return ESP_ERR_NOT_SUPPORTED;   /* защита от кривого файла */
    }

    if (!have_dsd) return ESP_ERR_NOT_SUPPORTED;
    if (!chnl_ok || ctx->channels == 0 || ctx->channels > DSD_MAX_CHANNELS) {
        ESP_LOGE(TAG, "DFF: каналов=%u — поддерживается 1..2", ctx->channels);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!rate_ok || ctx->dsd_rate < 2822400 || ctx->dsd_rate % 16 != 0) {
        ESP_LOGE(TAG, "DFF: частота %lu Гц не поддерживается", (unsigned long)ctx->dsd_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    (void)cmpr_ok;

    ctx->msb_first = true;    /* DSDIFF — MSB first */
    ctx->samples_per_ch = dsd_size * 8 / ctx->channels;
    ctx->stream_buf = malloc(DSD_CHUNK_DFF);
    if (ctx->stream_buf == NULL) return ESP_ERR_NO_MEM;
    ctx->stream_len = 0;
    ctx->stream_pos = 0;
    ctx->stream_left = dsd_size;
    ctx->bytes_total_per_ch = dsd_size / ctx->channels;
    return ESP_OK;
}

/* --------------------------- Открытие -------------------------------------- */

esp_err_t dsd_open(const char *path, dsd_info_t *out, dsd_handle_t *h)
{
    if (path == NULL || out == NULL || h == NULL) return ESP_ERR_INVALID_ARG;
    *h = NULL;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Не открыть файл: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    dsd_ctx_t *ctx = calloc(1, sizeof(dsd_ctx_t));
    if (ctx == NULL) { fclose(f); return ESP_ERR_NO_MEM; }
    ctx->f = f;

    esp_err_t err;
    uint8_t sig[4];
    if (fread(sig, 1, 4, f) != 4) { err = ESP_ERR_NOT_SUPPORTED; goto fail; }
    if (fseek(f, 0, SEEK_SET) != 0) { err = ESP_ERR_INVALID_STATE; goto fail; }

    if (memcmp(sig, "DSD ", 4) == 0) {
        ctx->fmt = DSD_FMT_DSF;
        err = parse_dsf(ctx);
    } else if (memcmp(sig, "FRM8", 4) == 0) {
        ctx->fmt = DSD_FMT_DFF;
        err = parse_dff(ctx);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Заголовок DSD не распознан (%s)", path);
        goto fail;
    }

    /* bit-reverse таблица для LSB-first DSF */
    for (int i = 0; i < 256; i++) {
        uint8_t r = 0, v = (uint8_t)i;
        for (int b = 0; b < 8; b++) r = (uint8_t)((r << 1) | (v & 1)), v >>= 1;
        ctx->rev8[i] = r;
    }

    out->dsd_rate = ctx->dsd_rate;
    out->channels = ctx->channels;
    out->dop_fs = ctx->dsd_rate / 16;
    out->fmt_name = (ctx->fmt == DSD_FMT_DSF) ? "DSF" : "DFF";
    out->samples_per_ch = ctx->samples_per_ch;
    out->seconds = (uint32_t)(ctx->samples_per_ch / (ctx->dsd_rate ? ctx->dsd_rate : 1));

    ESP_LOGI(TAG, "%s: DSD %lu Гц, %d кан., ~%lu с -> DoP %lu Гц",
             out->fmt_name, (unsigned long)ctx->dsd_rate, ctx->channels,
             (unsigned long)out->seconds, (unsigned long)out->dop_fs);
    *h = ctx;
    return ESP_OK;

fail:
    fclose(f);
    free(ctx->super);
    free(ctx->stream_buf);
    free(ctx);
    return err;
}

void dsd_close(dsd_handle_t h)
{
    if (h == NULL) return;
    if (h->f) fclose(h->f);
    free(h->super);
    free(h->stream_buf);
    free(h);
}

/* --------------------------- Чтение DoP ------------------------------------ */

size_t dsd_read_dop(dsd_handle_t h, int32_t *out, size_t max_samples, bool *eof)
{
    if (h == NULL || out == NULL || max_samples < 2) {
        if (eof) *eof = true;
        return 0;
    }
    size_t max_frames = max_samples / 2;
    size_t produced = 0;

    while (produced < max_frames) {
        uint8_t dl, dr;
        bool data_ok = false;

        if (h->fmt == DSD_FMT_DSF) {
            if (h->super_pos >= h->super_valid) {
                if (dsf_read_super(h) == 0) break;      /* EOF */
            }
            uint8_t bl = h->super[h->super_pos];
            uint8_t br = (h->channels >= 2) ? h->super[h->block_size + h->super_pos] : bl;
            dl = h->msb_first ? bl : h->rev8[bl];
            dr = h->msb_first ? br : h->rev8[br];
            h->super_pos++;
            data_ok = true;
        } else { /* DFF: потоковый интерливинг */
            if (h->stream_pos >= h->stream_len) {
                if (h->stream_left == 0) break;         /* EOF */
                size_t want = DSD_CHUNK_DFF;
                if (h->stream_left < want) want = (size_t)h->stream_left;
                size_t got = fread(h->stream_buf, 1, want, h->f);
                if (got == 0) break;
                h->stream_len = got;
                h->stream_pos = 0;
                h->stream_left -= got;
            }
            uint8_t bl = h->stream_buf[h->stream_pos];
            uint8_t br = (h->channels >= 2) ? h->stream_buf[h->stream_pos + 1] : bl;
            h->stream_pos += h->channels;
            dl = bl; dr = br;    /* DFF уже MSB-first */
            data_ok = true;
        }

        if (!data_ok) break;

        uint8_t marker = ((h->frame_cnt / 256) & 1) ? 0xFA : 0x05;
        h->frame_cnt++;
        out[produced * 2]     = ((int32_t)marker << 24) | ((int32_t)dl << 16);
        out[produced * 2 + 1] = ((int32_t)marker << 24) | ((int32_t)dr << 16);
        produced++;
    }

    if (eof) *eof = (produced < max_frames);   /* досрочно — значит данные кончились */
    return produced * 2;
}

/* --------------------------- Помощник -------------------------------------- */

bool dsd_is_dsd_path(const char *url)
{
    if (url == NULL) return false;
    const char *dot = strrchr(url, '.');
    if (dot == NULL) return false;
    return strncasecmp(dot, ".dsf", 4) == 0 || strncasecmp(dot, ".dff", 4) == 0;
}

void dsd_get_progress(dsd_handle_t h, uint64_t *pos, uint64_t *total)
{
    if (pos)   *pos = 0;
    if (total) *total = 0;
    if (h == NULL) return;
    uint64_t t = h->bytes_total_per_ch;
    uint64_t left = (h->fmt == DSD_FMT_DSF) ? h->bytes_left_per_ch
                                            : (h->stream_left / h->channels);
    if (t < left) left = t;   /* защита от кривых заголовков */
    if (pos)   *pos = t - left;
    if (total) *total = t;
}

/* Частота DSD-сэмплов файла (для перевода байтов прогресса в мс). */
uint32_t dsd_get_rate(dsd_handle_t h)
{
    return (h == NULL) ? 0 : h->dsd_rate;
}

/* Перемотка: миллисекунды -> байтовый офсет данных на канал.
 * DSF: кратен block_size (супергруппа читается блоками по каналам);
 * DFF: интерливированные байты — офсет умножается на каналы.
 * DoP-маркер сбрасывается на 0x05 (фаза начинается заново). */
esp_err_t dsd_seek_ms(dsd_handle_t h, uint32_t ms)
{
    if (h == NULL || h->f == NULL) return ESP_ERR_INVALID_STATE;
    if (h->bytes_total_per_ch == 0) return ESP_ERR_NOT_SUPPORTED;

    uint64_t bytes_per_sec = (uint64_t)h->dsd_rate / 8;   /* 1 байт на канал = 8 DSD-сэмплов */
    uint64_t want = (bytes_per_sec * (uint64_t)ms) / 1000;
    if (want >= h->bytes_total_per_ch) {
        want = (h->bytes_total_per_ch > 0) ? h->bytes_total_per_ch - 1 : 0;
    }

    uint64_t file_off;
    if (h->fmt == DSD_FMT_DSF) {
        want -= want % h->block_size;                 /* начало супергруппы */
        file_off = h->data_off + want;
        h->bytes_left_per_ch = h->bytes_total_per_ch - want;
        h->super_pos = 0;
        h->super_valid = 0;
    } else {
        uint64_t want_int = want * h->channels;       /* интерливированные байты */
        file_off = h->data_off + want_int;
        h->stream_left = (h->bytes_total_per_ch * h->channels) - want_int;
        h->stream_pos = h->stream_len = 0;
    }
    if (fseek(h->f, (long)file_off, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    h->frame_cnt = 0;                                  /* маркер DoP с начала фазы */
    ESP_LOGI("dsd", "seek: %lu мс -> офсет %llu (на канал)",
             (unsigned long)ms, (unsigned long long)(file_off - h->data_off));
    return ESP_OK;
}

/* Офсет ID3-метаданных (0 = нет; DFF пока без тегов — см. README). */
uint64_t dsd_get_id3_offset(dsd_handle_t h)
{
    return (h == NULL) ? 0 : h->id3_off;
}

/* Прочитать ID3-блок в buf (до cap байт). Возврат: прочитано байт (0 = нет). */
size_t dsd_read_id3(dsd_handle_t h, uint8_t *buf, size_t cap)
{
    if (h == NULL || buf == NULL || cap == 0 || h->id3_off == 0 || h->f == NULL) return 0;
    long cur = ftell(h->f);
    if (fseek(h->f, (long)h->id3_off, SEEK_SET) != 0) return 0;
    size_t got = fread(buf, 1, cap, h->f);
    if (cur >= 0) fseek(h->f, cur, SEEK_SET);
    return got;
}
