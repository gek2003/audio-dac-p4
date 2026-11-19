/*
 * ============================================================================
 *  audio_pipeline.h — публичные прототипы конвейера воспроизведения
 *  wifi_connect.c / http_source.c / decoder.c / i2s_out.c
 *  ESP-IDF v6.1
 * ============================================================================
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/ringbuf.h"
#include "esp_wifi_types.h"

/* ----------------------------- wifi_connect.c ----------------------------- */

esp_err_t wifi_connect_start(void);
esp_err_t wifi_wait_connected(uint32_t timeout_ms);
bool      wifi_is_connected(void);
const char *wifi_get_ssid(void);          /* текущий SSID ("" — не задан) */
esp_err_t wifi_reconnect(void);
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);

/* Спящий режим: wifi_off() останавливает стек ESP32-C6 (esp_wifi_stop),
 * wifi_on() — стартует и переподключается к сохранённой сети (STA_START).
 * wifi_is_off() — проверка; wifi_on при выборе радио-URL и из скана. */
esp_err_t wifi_off(void);
esp_err_t wifi_on(void);
bool      wifi_is_off(void);

/* Скан сетей (асинхронный): wifi_scan_start() запускает задачу,
 * wifi_scan_state() — 0 idle / 1 идёт / 2 готово (затем wifi_scan_ack()).
 * wifi_scan_count/get — результат после state==2 (отсортирован по RSSI). */
int wifi_scan_start(void);
int wifi_scan_state(void);
int wifi_scan_count(void);
const wifi_ap_record_t *wifi_scan_get(int idx);
void wifi_scan_ack(void);

/* ------------------------------ http_source.c ----------------------------- */

typedef enum {
    HTTP_SRC_STARTED = 0,
    HTTP_SRC_EOF,
    HTTP_SRC_ERROR,
} http_source_evt_t;

typedef void (*http_source_cb_t)(http_source_evt_t evt, void *user);

esp_err_t http_source_start(const char *url, RingbufHandle_t ring,
                            http_source_cb_t cb, void *user);
void      http_source_stop(void);
bool      http_source_running(void);
bool      http_source_is_stream(void);      /* радиопоток (без Content-Length) */
void      http_source_progress(uint64_t *pos, uint64_t *total);  /* файл: позиция/размер */

/* Перемотка HTTP-файлов: офсет старта для СЛЕДУЮЩЕГО http_source_start
 * (file:// — fseek, http:// — Range: bytes=N-). Вызывается decoder_seek(). */
void      http_source_set_start_offset(uint64_t offset);

/* -------------------------------- decoder.c ------------------------------- */

typedef enum {
    DEC_TYPE_AUTO = 0,   /* по расширению URL; без расширения — MP3 */
    DEC_TYPE_MP3,
    DEC_TYPE_FLAC,
    DEC_TYPE_WAV,
} decoder_type_t;

esp_err_t decoder_start(const char *url, decoder_type_t type, bool is_stream_hint);
void      decoder_stop(void);
bool      decoder_running(void);
void      decoder_set_type(decoder_type_t type);

/* Пауза и прогресс воспроизведения */
void      decoder_pause(void);
void      decoder_resume(void);
bool      decoder_paused(void);
void      decoder_set_on_finish(void (*cb)(void));  /* конец файла (авто-переход) */
void      decoder_progress(uint64_t *pos, uint64_t *total);

/* Точный прогресс: позиция и длительность в мс (+ флаг «можно ли перематывать»).
 * Позиция — по декодированным сэмплам (не по позиции чтения файла). */
void      decoder_progress_ext(uint32_t *pos_ms, uint32_t *total_ms, bool *seekable);

/* Перемотка на миллисекунды (MP3-файлы, DSD; радио — ESP_ERR_NOT_SUPPORTED).
 * Рестарт конвейера с байтового офсета (~0.3-1 с). */
esp_err_t decoder_seek(uint32_t ms);

/* -------------------------------- i2s_out.c ------------------------------- */

esp_err_t i2s_out_init(void);
esp_err_t i2s_out_apply_fs(uint32_t fs);
esp_err_t i2s_out_write(const void *pcm, size_t bytes);
esp_err_t i2s_out_deinit(void);
uint32_t  i2s_out_current_fs(void);
