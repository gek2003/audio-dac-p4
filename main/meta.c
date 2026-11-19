/*
 * ============================================================================
 *  meta.c — парсеры метаданных: ID3v2.2/2.3/2.4, ID3v1, FLAC vorbis comments
 *
 *  Кодировки текстовых фреймов ID3v2: 0 = ISO-8859-1, 1 = UTF-16 с BOM,
 *  2 = UTF-16BE, 3 = UTF-8 — всё приводится к UTF-8 для LVGL.
 *  Фреймы: TIT2/TT2 (title), TPE1/TP1 (artist), TALB/TAL (album).
 *  Обложки (APIC) игнорируются — парсер читает только текстовые фреймы.
 * ============================================================================
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "meta.h"

static const char *TAG = "meta";

#define META_FEED_MAX   (128 * 1024)   /* предел накопления для http-потока */

typedef struct {
    SemaphoreHandle_t lock;
    meta_tags_t tags;
    /* потоковый режим (meta_feed) */
    bool     active;
    bool     done;
    int      kind;               /* 0 = неизвестно, 1 = ID3v2, 2 = FLAC */
    uint8_t *acc;
    size_t   acc_len, acc_cap;
    /* обложка (APIC/PIC/FLAC PICTURE) */
    uint8_t *cover;              /* PSRAM-копия, живёт до meta_reset() */
    size_t   cover_len;
    int      cover_kind;         /* 0 = нет, 1 = JPEG, 2 = PNG */
    int      cover_serial;       /* инкремент при установке — UI декодирует только новое */
} meta_ctx_t;

static meta_ctx_t s_meta = { .active = false, .done = true };

static void lock_take(void)
{
    if (s_meta.lock == NULL) {
        s_meta.lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_meta.lock, portMAX_DELAY);
}

static void lock_give(void)
{
    xSemaphoreGive(s_meta.lock);
}

/* Сохранить данные обложки: копия в PSRAM, старая освобождается. Приоритет —
 * picture type 3 (front cover); первый встреченный тип — запасной вариант.
 * Вызывать ВНУТРИ lock. */
static void cover_store(const uint8_t *data, size_t len, int kind, bool is_front)
{
    if (data == NULL || len == 0 || len > META_COVER_MAX) return;
    if (s_meta.cover != NULL && s_meta.cover_kind != 0 && !is_front) {
        return;   /* уже есть — не передаваем */
    }
    if (s_meta.cover != NULL && s_meta.cover_kind == kind && is_front == false) {
        return;
    }
    uint8_t *nb = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (nb == NULL) nb = malloc(len);
    if (nb == NULL) return;
    memcpy(nb, data, len);
    if (s_meta.cover != NULL) free(s_meta.cover);
    s_meta.cover = nb;
    s_meta.cover_len = len;
    s_meta.cover_kind = kind;
    s_meta.cover_serial++;
    ESP_LOGI(TAG, "обложка: %zu байт (%s)%s", len,
             kind == 2 ? "PNG" : "JPEG", is_front ? " [front]" : "");
}

static void set_locked(const char *title, const char *artist, const char *album)
{
    if (title)  snprintf(s_meta.tags.title,  sizeof(s_meta.tags.title),  "%s", title);
    if (artist) snprintf(s_meta.tags.artist, sizeof(s_meta.tags.artist), "%s", artist);
    if (album)  snprintf(s_meta.tags.album,  sizeof(s_meta.tags.album),  "%s", album);
}

void meta_reset(void)
{
    lock_take();
    memset(&s_meta.tags, 0, sizeof(s_meta.tags));
    s_meta.active = false;
    s_meta.done = true;
    s_meta.kind = 0;
    s_meta.acc_len = 0;
    if (s_meta.cover != NULL) {
        free(s_meta.cover);
        s_meta.cover = NULL;
        s_meta.cover_len = 0;
        s_meta.cover_kind = 0;
    }
    lock_give();
}

void meta_set(const char *title, const char *artist, const char *album)
{
    lock_take();
    set_locked(title, artist, album);
    lock_give();
}

void meta_get(meta_tags_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    lock_take();
    memcpy(out, &s_meta.tags, sizeof(meta_tags_t));
    lock_give();
}

/* ---------------------- конверсии текста в UTF-8 --------------------------- */

static void latin1_to_utf8(const uint8_t *src, size_t len, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 3 < cap; i++) {
        uint8_t c = src[i];
        if (c < 0x80) dst[o++] = (char)c;
        else {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

static void utf16_to_utf8(const uint8_t *src, size_t len, bool be, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < len && o + 5 < cap; i += 2) {
        uint32_t u = be ? ((src[i] << 8) | src[i + 1]) : ((src[i + 1] << 8) | src[i]);
        if (u == 0) break;   /* терминатор UTF-16 */
        if (u >= 0xD800 && u <= 0xDFFF) { dst[o++] = '?'; continue; }  /* суррогаты — упрощённо */
        if (u < 0x80) dst[o++] = (char)u;
        else if (u < 0x800) {
            dst[o++] = (char)(0xC0 | (u >> 6));
            dst[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | (u >> 12));
            dst[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    dst[o] = '\0';
}

static void id3_text_to_utf8(const uint8_t *enc, size_t len, char *dst, size_t cap)
{
    if (len == 0) { dst[0] = '\0'; return; }
    uint8_t e = enc[0];
    const uint8_t *t = enc + 1;
    size_t tl = len - 1;
    switch (e) {
        case 0:  /* ISO-8859-1 */
            latin1_to_utf8(t, tl, dst, cap);
            break;
        case 1: { /* UTF-16 c BOM: FF FE = LE, FE FF = BE */
            bool be = (tl >= 2 && t[0] == 0xFE && t[1] == 0xFF);
            size_t off = (tl >= 2 && ((t[0] == 0xFE && t[1] == 0xFF) ||
                                      (t[0] == 0xFF && t[1] == 0xFE))) ? 2 : 0;
            utf16_to_utf8(t + off, tl - off, be, dst, cap);
            break;
        }
        case 2:  /* UTF-16BE без BOM */
            utf16_to_utf8(t, tl, true, dst, cap);
            break;
        case 3:  /* UTF-8 */
        default: {
            size_t n = tl < cap - 1 ? tl : cap - 1;
            memcpy(dst, t, n);
            dst[n] = '\0';
            break;
        }
    }
}

/* ---------------------------- ID3v2 ---------------------------------------- */

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  | (p[3] & 0x7F);
}

/* --- обложки: ID3 APIC/PIC (v2.2/2.3/2.4) ----------------------------------
 * Тело APIC (v2.3/2.4): [encoding 1][mime null-term latin1][type 1]
 *                        [description null-term в encoding][данные].
 * v2.2 PIC:             [encoding 1][format 3 ASCII ("JPG"/"PNG")][type 1]
 *                        [description null-term][данные].
 * Кодировка description: 0/3 = 1 байт до нуля; 1/2 = UTF-16 — до двойного нуля. */
static const uint8_t *skip_desc_utf8_or_utf16(const uint8_t *p, const uint8_t *end)
{
    bool utf16 = (p < end && (p[0] == 1 || p[0] == 2));
    if (utf16) {
        while (p + 1 < end) {
            if (p[0] == 0x00 && p[1] == 0x00) return p + 2;
            p += 2;
        }
        return end;
    }
    while (p < end && p[0] != 0x00) p++;
    return (p < end) ? p + 1 : end;
}

static int cover_kind_from_mime(const char *mime, size_t mlen)
{
    if (mlen >= 9 && strncasecmp(mime, "image/png", 9) == 0) return 2;
    if (mlen >= 10 && strncasecmp(mime, "image/jpeg", 10) == 0) return 1;
    return 1;   /* по умолчанию считаем JPEG (большинство APIC) */
}

static void cover_from_apic(const uint8_t *body, size_t fsize, uint8_t ver)
{
    if (fsize < 6) return;
    const uint8_t *p = body, *end = body + fsize;
    int kind = 1;
    if (ver >= 3) {
        uint8_t enc = p[0]; (void)enc;
        p++;
        const uint8_t *mime = p;
        while (p < end && p[0] != 0x00) p++;
        if (p >= end) return;
        size_t mlen = (size_t)(p - mime);
        kind = cover_kind_from_mime((const char *)mime, mlen);
        p++;                                    /* терминатор mime */
        if (p >= end) return;
        uint8_t ptype = p[0]; p++;
        p = skip_desc_utf8_or_utf16(p, end);
        if (p >= end) return;
        cover_store(p, (size_t)(end - p), kind, ptype == 3);
    } else {
        uint8_t enc = p[0]; (void)enc;
        p++;
        if (p + 3 > end) return;
        char fmt[4] = { (char)p[0], (char)p[1], (char)p[2], 0 };
        p += 3;
        kind = (memcmp(fmt, "PNG", 3) == 0) ? 2 : 1;
        if (p >= end) return;
        uint8_t ptype = p[0]; p++;
        p = skip_desc_utf8_or_utf16(p, end);
        if (p >= end) return;
        cover_store(p, (size_t)(end - p), kind, ptype == 3);
    }
}

/* Разобрать блок памяти с ID3v2 (buf[0..len)); buf начинается с "ID3". */
static void parse_id3(const uint8_t *buf, size_t len)
{
    if (len < 10 || memcmp(buf, "ID3", 3) != 0) return;
    uint8_t ver = buf[3];
    uint8_t flags = buf[5];
    uint32_t tag_size = syncsafe32(buf + 6);      /* тело после 10-байт заголовка */
    if (tag_size == 0) tag_size = (uint32_t)len;  /* кривой заголовок — идём по факту */
    if ((size_t)tag_size + 10 > len) tag_size = (uint32_t)(len > 10 ? len - 10 : 0);

    size_t off = 10;
    if (flags & 0x40) {                            /* extended header — пропускаем */
        if (ver >= 4 && off + 4 <= len) {
            off += syncsafe32(buf + off);
        } else if (off + 4 <= len) {
            uint32_t esz = ((uint32_t)buf[off] << 24) | (buf[off+1] << 16) |
                           (buf[off+2] << 8) | buf[off+3];
            off += esz + 4;
        }
    }

    char title[META_TITLE_MAX] = {0}, artist[META_ARTIST_MAX] = {0}, album[META_ALBUM_MAX] = {0};

    /* Цикл идёт по ВСЕМ фреймам до мусора (не только текстовым): APIC/PIC
     * обычно стоят после TXXX и должны быть обработаны. */
    while (off < len) {
        size_t id_len = (ver <= 2) ? 3 : 4;
        if (off + id_len + (ver <= 2 ? 3 : 7) > len) break;
        bool valid_id = true;
        for (size_t i = 0; i < id_len; i++) {
            uint8_t ch = buf[off + i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'))) { valid_id = false; break; }
        }
        if (!valid_id) break;                      /* конец тега */

        size_t hsize = (ver <= 2) ? 6 : 10;
        size_t fsize;
        if (ver <= 2) {
            fsize = ((size_t)buf[off+3] << 16) | ((size_t)buf[off+4] << 8) | buf[off+5];
        } else if (ver >= 4) {
            fsize = syncsafe32(buf + off + 4);
        } else {
            fsize = ((size_t)buf[off+4] << 24) | ((size_t)buf[off+5] << 16) |
                    ((size_t)buf[off+6] << 8) | buf[off+7];
        }
        size_t body = off + hsize;
        if (fsize == 0 || body + fsize > len) break;

        const char *field = NULL;
        if (ver >= 3 && memcmp(buf + off, "APIC", 4) == 0) {
            cover_from_apic(buf + body, fsize, ver);
        } else if (ver <= 2 && memcmp(buf + off, "PIC", 3) == 0) {
            cover_from_apic(buf + body, fsize, ver);
        } else {
            if (ver <= 2) {
                if (memcmp(buf + off, "TT2", 3) == 0)      field = "title";
                else if (memcmp(buf + off, "TP1", 3) == 0) field = "artist";
                else if (memcmp(buf + off, "TAL", 3) == 0) field = "album";
            } else {
                if (memcmp(buf + off, "TIT2", 4) == 0)      field = "title";
                else if (memcmp(buf + off, "TPE1", 4) == 0) field = "artist";
                else if (memcmp(buf + off, "TALB", 4) == 0) field = "album";
            }
            if (field != NULL && buf[body] <= 3) {
                char tmp[320];
                id3_text_to_utf8(buf + body, fsize, tmp, sizeof(tmp));
                if (strcmp(field, "title") == 0) {
                    strlcpy(title,  tmp, sizeof(title));
                } else if (strcmp(field, "artist") == 0) {
                    strlcpy(artist, tmp, sizeof(artist));
                } else {
                    strlcpy(album,  tmp, sizeof(album));
                }
            }
        }
        off = body + fsize;
    }

    if (title[0] || artist[0] || album[0]) {
        meta_set(title[0] ? title : NULL, artist[0] ? artist : NULL, album[0] ? album : NULL);
        ESP_LOGI(TAG, "ID3v2: «%s» / «%s»", title, artist);
    }
}

void meta_parse_id3_buffer(const uint8_t *buf, size_t len)
{
    parse_id3(buf, len);
}

/* ----------------------------- FLAC ---------------------------------------- */

/* buf начинается с "fLaC"; разбираем блоки до vorbis comment (type 4). */
static void parse_flac(const uint8_t *buf, size_t len)
{
    if (len < 8 || memcmp(buf, "fLaC", 4) != 0) return;
    size_t off = 4;
    char title[META_TITLE_MAX] = {0}, artist[META_ARTIST_MAX] = {0}, album[META_ALBUM_MAX] = {0};
    bool found = false;

    while (off + 4 <= len) {
        uint8_t hdr = buf[off];
        bool last = (hdr & 0x80) != 0;
        uint8_t type = hdr & 0x7F;
        uint32_t blen = ((uint32_t)buf[off+1] << 16) | ((uint32_t)buf[off+2] << 8) | buf[off+3];
        off += 4;
        if (type == 6 && off + blen <= len) {          /* PICTURE (front cover) */
            size_t p = off;
            if (p + 8 > off + blen) { off += blen; continue; }
            uint32_t ptype = ((uint32_t)buf[p] << 24) | (buf[p+1] << 16) |
                             (buf[p+2] << 8) | buf[p+3];
            p += 4;
            uint32_t mlen = ((uint32_t)buf[p] << 24) | (buf[p+1] << 16) |
                            (buf[p+2] << 8) | buf[p+3];
            p += 4;
            if (p + mlen + 4 > off + blen) { off += blen; continue; }
            int kind = cover_kind_from_mime((const char *)&buf[p], mlen);
            p += mlen;
            uint32_t dlen = ((uint32_t)buf[p] << 24) | (buf[p+1] << 16) |
                            (buf[p+2] << 8) | buf[p+3];
            p += 4 + dlen;                              /* описание пропускаем */
            if (p + 16 + 4 > off + blen) { off += blen; continue; }
            p += 16;                                    /* w/h/цвета/глубина */
            uint32_t datalen = ((uint32_t)buf[p] << 24) | (buf[p+1] << 16) |
                               (buf[p+2] << 8) | buf[p+3];
            p += 4;
            if (p + datalen > off + blen) { off += blen; continue; }
            cover_store(&buf[p], datalen, kind, ptype == 3);
        }
        if (type == 4 && off + blen <= len) {          /* VORBIS_COMMENT */
            size_t p = off;
            uint32_t vlen = (uint32_t)(buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                                       ((uint32_t)buf[p+3] << 24));
            p += 4 + vlen;
            if (p + 4 > off + blen) break;
            uint32_t count = (uint32_t)(buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                                        ((uint32_t)buf[p+3] << 24));
            p += 4;
            for (uint32_t i = 0; i < count && p + 4 <= off + blen; i++) {
                uint32_t clen = (uint32_t)(buf[p] | (buf[p+1] << 8) | (buf[p+2] << 16) |
                                           ((uint32_t)buf[p+3] << 24));
                p += 4;
                if (p + clen > off + blen) break;
                const char *c = (const char *)&buf[p];
                if (strncasecmp(c, "TITLE=", 6) == 0 && clen > 6) {
                    snprintf(title, sizeof(title), "%.*s", (int)(clen - 6), c + 6);
                    found = true;
                } else if (strncasecmp(c, "ARTIST=", 7) == 0 && clen > 7) {
                    snprintf(artist, sizeof(artist), "%.*s", (int)(clen - 7), c + 7);
                    found = true;
                } else if (strncasecmp(c, "ALBUM=", 6) == 0 && clen > 6) {
                    snprintf(album, sizeof(album), "%.*s", (int)(clen - 6), c + 6);
                    found = true;
                }
                p += clen;
            }
            break;                                     /* comment дальше не ищем */
        }
        off += blen;
        if (last) break;
    }

    if (found) {
        meta_set(title[0] ? title : NULL, artist[0] ? artist : NULL, album[0] ? album : NULL);
        ESP_LOGI(TAG, "FLAC: «%s» / «%s»", title, artist);
    }
}

/* ----------------------------- ID3v1 --------------------------------------- */

static void probe_id3v1(FILE *f)
{
    if (fseek(f, -128, SEEK_END) != 0) return;
    uint8_t blk[128];
    if (fread(blk, 1, 128, f) != 128 || memcmp(blk, "TAG", 3) != 0) return;
    char title[31] = {0}, artist[31] = {0};
    latin1_to_utf8(blk + 3, 30, title, sizeof(title));
    latin1_to_utf8(blk + 33, 30, artist, sizeof(artist));
    /* обрезаем пробельный хвост формата */
    for (int i = 29; i >= 0; i--) {
        if (title[i] == ' ' || title[i] == '\0') title[i] = '\0'; else break;
        if (i == 0) break;
    }
    for (int i = 29; i >= 0; i--) {
        if (artist[i] == ' ' || artist[i] == '\0') artist[i] = '\0'; else break;
        if (i == 0) break;
    }
    if (title[0] || artist[0]) {
        meta_set(title[0] ? title : NULL, artist[0] ? artist : NULL, NULL);
        ESP_LOGI(TAG, "ID3v1: «%s» / «%s»", title, artist);
    }
}

/* --------------------------- file:// ---------------------------------------- */

void meta_probe_file(const char *path)
{
    if (path == NULL) return;
    meta_reset();

    uint8_t head[2048];
    FILE *f = fopen(path, "rb");
    if (f == NULL) return;
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (n == 0) return;

    if (n >= 10 && memcmp(head, "ID3", 3) == 0) {
        /* ID3v2 с обложкой легко переваливает 256 КБ — читаем до 1.5 МБ
         * (META_COVER_MAX); SDMMC2 тянет это за десятки мс. */
        size_t full = META_COVER_MAX;
        uint8_t *buf = heap_caps_malloc(full, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) buf = malloc(full);
        if (buf != NULL) {
            FILE *f2 = fopen(path, "rb");
            if (f2 != NULL) {
                size_t got = fread(buf, 1, full, f2);
                fclose(f2);
                parse_id3(buf, got);
            }
            free(buf);
        }
        meta_tags_t chk;
        meta_get(&chk);
        if (chk.title[0] == '\0' && chk.artist[0] == '\0') {
            /* ID3v2 не дал тегов — пробуем ID3v1 в конце файла */
            FILE *f2 = fopen(path, "rb");
            if (f2 != NULL) { probe_id3v1(f2); fclose(f2); }
        }
        return;
    }
    if (n >= 4 && memcmp(head, "fLaC", 4) == 0) {
        size_t full = META_COVER_MAX;   /* PICTURE может быть в поздних блоках */
        FILE *f2 = fopen(path, "rb");
        if (f2 == NULL) return;
        uint8_t *buf = heap_caps_malloc(full, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) buf = malloc(full);
        if (buf == NULL) { fclose(f2); return; }
        size_t got = fread(buf, 1, full, f2);
        fclose(f2);
        parse_flac(buf, got);
        free(buf);
        return;
    }
    if (n >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) {
        /* MPEG-кадры без ID3v2 — пробуем ID3v1 в конце файла */
        FILE *f2 = fopen(path, "rb");
        if (f2 != NULL) { probe_id3v1(f2); fclose(f2); }
        return;
    }
    /* прочие (WAV): тоже ID3v1 в конце */
    FILE *f2 = fopen(path, "rb");
    if (f2 != NULL) { probe_id3v1(f2); fclose(f2); }
}

/* ----------------------- потоковый режим (http) ---------------------------- */

void meta_feed(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) return;
    lock_take();
    if (s_meta.done) { lock_give(); return; }
    if (!s_meta.active) {
        s_meta.active = true;
        s_meta.acc_cap = 16 * 1024;
        s_meta.acc = heap_caps_malloc(s_meta.acc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_meta.acc == NULL) s_meta.acc = malloc(s_meta.acc_cap);
        if (s_meta.acc == NULL) {
            s_meta.done = true;
            lock_give();
            return;
        }
        s_meta.acc_len = 0;
    }
    bool flush = false;
    while (len > 0 && !flush) {
        size_t free_space = s_meta.acc_cap - s_meta.acc_len;
        if (free_space == 0) {
            /* расширяем буфер до предела; без места — завершаем */
            if (s_meta.acc_cap < META_FEED_MAX) {
                size_t nc = s_meta.acc_cap * 2;
                uint8_t *b = heap_caps_realloc(s_meta.acc, nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (b == NULL) b = realloc(s_meta.acc, nc);
                if (b != NULL) {
                    s_meta.acc = b;
                    s_meta.acc_cap = nc;
                    continue;
                }
            }
            flush = true;
            break;
        }
        size_t take = len < free_space ? len : free_space;
        memcpy(s_meta.acc + s_meta.acc_len, data, take);
        s_meta.acc_len += take;
        data += take;
        len -= take;

        if (s_meta.kind == 0 && s_meta.acc_len >= 4) {
            if (memcmp(s_meta.acc, "ID3", 3) == 0) {
                s_meta.kind = 1;
            } else if (memcmp(s_meta.acc, "fLaC", 4) == 0) {
                s_meta.kind = 2;
            } else {
                /* нет тегов: MPEG-кадры или радио-поток — конец */
                s_meta.done = true;
            }
        }
        if (s_meta.kind == 1 && s_meta.acc_len >= 10) {
            uint32_t tag_size = syncsafe32(s_meta.acc + 6);
            if (s_meta.acc_len >= 10 + tag_size) {
                parse_id3(s_meta.acc, s_meta.acc_len);
                s_meta.done = true;
            }
        }
        if (s_meta.kind == 2) {
            /* блоки FLAC разбираем по мере накопления: vorbis comment
             * обычно в первых КБ; parse_flac сам завершится на найденном */
            parse_flac(s_meta.acc, s_meta.acc_len);
            meta_tags_t chk;
            memcpy(&chk, &s_meta.tags, sizeof(chk));
            if (chk.title[0] || chk.artist[0] || chk.album[0]) {
                s_meta.done = true;
            }
        }
        if (s_meta.done) break;
        if (s_meta.acc_cap >= META_FEED_MAX) {
            s_meta.done = true;   /* лимит — сдаёмся */
        }
    }
    lock_give();
}

bool meta_done(void)
{
    return s_meta.done;
}

bool meta_cover_get(meta_cover_t *out)
{
    if (out == NULL) return false;
    out->data = NULL; out->len = 0; out->kind = 0; out->serial = -1;
    lock_take();
    if (s_meta.cover != NULL && s_meta.cover_kind != 0) {
        out->data = s_meta.cover;
        out->len = s_meta.cover_len;
        out->kind = s_meta.cover_kind;
        out->serial = s_meta.cover_serial;
    }
    lock_give();
    return out->data != NULL;
}
