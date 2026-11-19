/*
 * ============================================================================
 *  meta.h — метаданные трека для UI: ID3v2/ID3v1 (MP3), vorbis comments
 *  (FLAC), ID3 из DSF-файла, Icy StreamTitle (радио).
 *
 *  Хранилище — один набор строк (title/artist/album) с мьютексом; UI читает
 *  meta_get(), декодер/http_source пишут meta_set()/meta_feed().
 *
 *  Источники заполнения:
 *    - file:// MP3/WAV: meta_probe_file() — ID3v2 в начале файла, иначе
 *      ID3v1 в конце (128 байт);
 *    - file:// FLAC: meta_probe_file() — vorbis comment из первых блоков;
 *    - file:// DSF: ID3 в конце файла (metadata pointer из заголовка) —
 *      dsd_dec.c отдаёт офсет, decoder.c читает блок и вызывает
 *      meta_parse_id3_buffer();
 *    - http:// файл (MP3/FLAC): meta_feed() — потоковое накопление первых
 *      КБ до декодера (файл уже идёт в ringbuf, парсер только читает);
 *    - радио: http_source.c при Icy-MetaData парсит StreamTitle и вызывает
 *      meta_set() в реальном времени.
 * ============================================================================
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define META_TITLE_MAX  192
#define META_ARTIST_MAX 128
#define META_ALBUM_MAX  128

typedef struct {
    char title[META_TITLE_MAX];
    char artist[META_ARTIST_MAX];
    char album[META_ALBUM_MAX];
} meta_tags_t;

/* Сбросить мету (при старте нового источника). */
void meta_reset(void);

/* Записать теги (потокобезопасно). Любой из аргументов может быть NULL. */
void meta_set(const char *title, const char *artist, const char *album);

/* Снимок текущих тегов в out (безопасен для UI-потока). */
void meta_get(meta_tags_t *out);

/* file://-файл: вытащить ID3v2/ID3v1/vorbis comments. path — без "file://". */
void meta_probe_file(const char *path);

/* Разобрать готовый ID3-блок из памяти (DSD-файлы). */
void meta_parse_id3_buffer(const uint8_t *buf, size_t len);

/* Потоковое накопление для HTTP-источников: кормить чанками ДО передачи их
 * декодеру; парсер сам понимает ID3v2 ("ID3"), FLAC ("fLaC") и MPEG-sync
 * (нет тегов — быстро завершается). */
void meta_feed(const uint8_t *data, size_t len);

/* true, когда потоковые теги больше не ожидаются (парсер закончил). */
bool meta_done(void);

/* ------------------------------ обложка ------------------------------------
 * Извлекается из ID3v2 APIC/PIC и FLAC PICTURE (тип 3 — front cover) при
 * meta_probe_file / meta_parse_id3_buffer. Данные держатся в PSRAM до
 * meta_reset() (новый источник). kind: 1 = JPEG (аппаратный декодер P4),
 * 2 = PNG (аппаратного декодера нет — не показываем). serial инкрементится
 * при каждой установке — UI декодирует только при смене. */
#define META_COVER_MAX (1536 * 1024)

typedef struct {
    const uint8_t *data;   /* ссылка на внутренний буфер meta (без копии) */
    size_t          len;
    int             kind;   /* 1 = JPEG, 2 = PNG */
    int             serial; /* номер установки (для «декодировать только новое») */
} meta_cover_t;

/* Текущая обложка (безопасно из UI-потока). false — обложки нет. */
bool meta_cover_get(meta_cover_t *out);
