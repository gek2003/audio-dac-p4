/*
 * ============================================================================
 *  decoder.c — конвейер воспроизведения: HTTP-поток -> декодер -> I²S
 *  ESP-IDF v6.1, компонент espressif/esp_audio_codec (v2.6.x)
 *
 *  Пайплайн:
 *    http_source (task) --ringbuf 256 КБ (PSRAM)--> decoder (task)
 *      --esp_audio_simple_dec_process--> PCM --конверсия в 32 бит-->
 *      i2s_out_write --> I²S-мастер --> ES9039Q2M (I²S-slave)
 *
 *  Формат источника определяется по расширению URL: .mp3 -> MP3,
 *  .flac -> FLAC, .wav -> WAV; если расширения нет (интернет-радио) —
 *  по умолчанию MP3 (типичный формат Icecast-потоков). Тип можно задать
 *  вручную через decoder_set_type().
 *
 *  DSD-файлы (.dsf/.dff с SD) идут своим пайплайном: dsd_dec.c читает их
 *  напрямую (fopen/fread, без ringbuf) и упаковывает в DoP (маркер 0x05/0xFA
 *  в старших битах, см. dsd_dec.h); DAC переводится на вход DoP через
 *  es9039_set_input(ES9039_INPUT_DOP), по завершении — обратно на PCM.
 *
 *  Связка с DAC (es9039q2m_esp32p4_driver.c):
 *    DAC работает в ASYNC slave — MCLK идёт от локального генератора
 *    NDK NZ2520SDA 49,152 МГц на плате, P4 подаёт только BCLK/LRCK/DATA.
 *    При первом декодированном кадре читаются sample_rate/bits_per_sample/
 *    channel, при смене частоты вызывается es9039_switch_frequency() с
 *    колбэком i2s_out_apply_fs(): DAC сначала mute, затем пересборка
 *    BCLK/LRCK (MCLK не меняется), ожидание SRC lock, затем unmute — без щелчков.
 *
 *  Данные декодера всегда приводятся к 32-битным сэмплам (старшие разряды
 *  впереди, стандарт I²S): 16 бит <<16, 24 бит <<8, 32 бит без изменений.
 *  Mono-каналы дублируются в оба слота.
 *
 *  Память (README esp_audio_codec): MP3 ~28 КБ heap, FLAC ~89 КБ heap,
 *  стек задачи декодера ~20 КБ.
 * ============================================================================
 */

#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "audio_pipeline.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "dsd_dec.h"
#include "dsd_native.h"
#include "phono_adc.h"
#include "meta.h"
#include "es9039q2m_esp32p4_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

/* Прототип из es9039q2m_esp32p4_driver.c (файл без отдельного .h) */
extern esp_err_t es9039_switch_frequency(void (*apply_cb)(void *), void *arg,
                                          uint32_t lock_timeout_ms, uint32_t fs_hz);

/* Прототипы из i2s_out.c включены в audio_pipeline.h */

static const char *TAG = "decoder";

#define DEC_RING_SIZE        (256 * 1024)   /* буфер между HTTP-читателем и декодером */
#define DEC_READ_CHUNK       8192           /* чанк чтения из ringbuf */
#define DEC_OUT_BUF_MIN      (64 * 1024)    /* стартовый выходной буфер PCM */
#define DEC_TASK_STACK       20480          /* ~20 КБ по рекомендации README компонента */
#define DEC_TASK_PRIO        11
#define DEC_STOP_DRAIN_MS    2000

/* Прототипы из i2s_out.c */
extern esp_err_t i2s_out_init(void);
extern esp_err_t i2s_out_apply_fs(uint32_t fs);
extern esp_err_t i2s_out_write(const void *pcm, size_t bytes);
extern uint32_t  i2s_out_current_fs(void);
extern void      i2s_out_set_paused(bool paused);
extern bool      i2s_out_paused(void);

typedef struct {
    char                       url[512];
    bool                       is_stream;
    bool                       stop_requested;
    bool                       registered;
    bool                       paused;
    TaskHandle_t               task;
    RingbufHandle_t            ring;
    esp_audio_simple_dec_handle_t dec;
    decoder_type_t             type_override;
    uint8_t                   *read_buf;
    uint8_t                   *out_buf;
    uint32_t                   out_buf_size;
    uint32_t                   current_fs;
    uint8_t                    current_bits;
    uint8_t                    current_channel;
    dsd_handle_t               dsd;          /* активный DSD-файл (для прогресса) */
    uint64_t                   samples;      /* декодированных сэмплов на канал (позиция) */
    uint64_t                   consumed;     /* сжатых байт потреблено из источника */
    uint32_t                   info_bitrate; /* битрейт от декодера, бит/с (0 = нет) */
} decoder_ctx_t;

/* Колбэк конца трека — вне ctx, чтобы decoder_start (memset) его не затирал */
static void (*s_on_finish_cb)(void) = NULL;

static decoder_ctx_t s_dec = { .stop_requested = true };

/* Дефолт типа декодера, задаваемый decoder_set_type(); не сбрасывается в decoder_start */
static decoder_type_t s_type_default = DEC_TYPE_AUTO;

/* --------------------- Регистрация декодеров ----------------------------- */

static esp_err_t register_decoders_once(void)
{
    if (s_dec.registered) {
        return ESP_OK;
    }
    esp_audio_err_t err1 = esp_audio_dec_register_default();
    esp_audio_err_t err2 = esp_audio_simple_dec_register_default();
    if (err1 != ESP_AUDIO_ERR_OK || err2 != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Регистрация декодеров: frame=0x%x simple=0x%x", err1, err2);
        return ESP_FAIL;
    }
    s_dec.registered = true;
    return ESP_OK;
}

static esp_audio_simple_dec_type_t detect_type(const char *url, decoder_type_t override)
{
    if (override == DEC_TYPE_MP3)  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (override == DEC_TYPE_FLAC) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (override == DEC_TYPE_WAV)  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;

    /* AUTO: по расширению URL */
    const char *path_end = strstr(url, "?");
    const char *dot = strrchr(url, '.');
    if (dot != NULL && (path_end == NULL || dot < path_end)) {
        if (strncasecmp(dot, ".mp3", 4) == 0)  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        if (strncasecmp(dot, ".flac", 5) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        if (strncasecmp(dot, ".wav", 4) == 0)  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        if (strncasecmp(dot, ".aac", 4) == 0)  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    /* Радиопоток без расширения: типичный Icecast — MP3 */
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

/* ---------------------- Переключение частоты ------------------------------ */

static uint32_t s_pending_fs;

static void apply_rate_cb(void *arg)
{
    (void)arg;
    i2s_out_apply_fs(s_pending_fs);
}

static void apply_rate_with_dac(uint32_t fs)
{
    if (fs == s_dec.current_fs) {
        return;
    }
    s_pending_fs = fs;
    esp_err_t err = es9039_switch_frequency(apply_rate_cb, NULL, 500, fs);
    if (err == ESP_OK) {
        s_dec.current_fs = fs;
        ESP_LOGI(TAG, "Частота воспроизведения: %lu Гц", (unsigned long)fs);
    } else {
        ESP_LOGW(TAG, "SRC lock не получен для %lu Гц — продолжаем на прежней частоте",
                 (unsigned long)fs);
    }
}

/* ------------------- Конверсия PCM к 32-битным слотам --------------------- */

/* buf: PCM на выходе декодера (16/24/32 бит), out: массив int32 (L,R) */
static size_t convert_to_32bit(const uint8_t *src, size_t src_len, uint8_t bits,
                               uint8_t channel, uint8_t *dst, size_t dst_cap)
{
    size_t out_used = 0;
    int32_t *d = (int32_t *)dst;

    if (bits == 16) {
        size_t n_samples = src_len / 2;                 /* int16 сэмплы */
        for (size_t i = 0; i < n_samples; i++) {
            if (out_used + sizeof(int32_t) > dst_cap) break;
            int16_t s = (int16_t)(src[2*i] | (src[2*i+1] << 8));
            if (channel == 1) {
                d[out_used/4] = (int32_t)s << 16;       /* mono: левый слот */
                d[out_used/4 + 1] = (int32_t)s << 16;   /* дублируем в правый */
                out_used += 2 * sizeof(int32_t);
            } else {
                d[out_used/4] = (int32_t)s << 16;
                out_used += sizeof(int32_t);
            }
        }
    } else if (bits == 24) {
        size_t n_samples = src_len / 3;                 /* packed 3 байта */
        for (size_t i = 0; i < n_samples; i++) {
            if (out_used + sizeof(int32_t) > dst_cap) break;
            int32_t v = (int32_t)(src[3*i] | (src[3*i+1] << 8) | (src[3*i+2] << 16));
            /* sign-extend до 32 бит, затем выравнивание в старшие разряды слота */
            if (v & 0x800000) v -= 0x1000000;
            v <<= 8;
            if (channel == 1) {
                d[out_used/4] = v;
                d[out_used/4 + 1] = v;
                out_used += 2 * sizeof(int32_t);
            } else {
                d[out_used/4] = v;
                out_used += sizeof(int32_t);
            }
        }
    } else { /* bits == 32 */
        size_t n_samples = src_len / 4;                 /* int32 сэмплы */
        for (size_t i = 0; i < n_samples; i++) {
            if (out_used + sizeof(int32_t) > dst_cap) break;
            int32_t v = (int32_t)((uint32_t)src[4*i] | ((uint32_t)src[4*i+1] << 8) |
                                  ((uint32_t)src[4*i+2] << 16) | ((uint32_t)src[4*i+3] << 24));
            if (channel == 1) {
                d[out_used/4] = v;
                d[out_used/4 + 1] = v;
                out_used += 2 * sizeof(int32_t);
            } else {
                d[out_used/4] = v;
                out_used += sizeof(int32_t);
            }
        }
    }
    return out_used;
}

/* ------------------- ADC passthrough (adc://) ------------------------------
 * Живой вход: phono_adc.c пишет в ringbuf готовые 32-битные стерео-фреймы
 * (24 бита в старших разрядах, 192 кГц), decoder льёт их без изменений в
 * i2s_out_write. Декодирование/метаданные/перемотка не применяются.
 * Остановка: только decoder_stop() (s_dec.stop_requested) либо авария
 * phono_adc — тогда «конец источника» даст авто-восстановление из main.c. */
static void adc_playback_loop(void)
{
    apply_rate_with_dac(192000);   /* BCLK/LRCK 192к на DAC (mute->unmute внутри) */

    while (!s_dec.stop_requested) {
        size_t chunk = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_dec.ring, &chunk,
                                                          pdMS_TO_TICKS(500),
                                                          DEC_READ_CHUNK);
        if (data == NULL) {
            if (!phono_adc_running()) {
                ESP_LOGW(TAG, "ADC-источник остановился");
                break;
            }
            continue;
        }
        if (chunk > 0) {
            i2s_out_write(data, chunk);
            s_dec.samples += chunk / (2 * sizeof(int32_t));
        }
        vRingbufferReturnItem(s_dec.ring, (void *)data);
    }
}

/* ------------------------- Задача декодера -------------------------------- */

/* ------------------------- DSD-файлы (DoP) -------------------------------- */

/* .dsf/.dff — свой пайплайн: файл читается напрямую (без ringbuf/http),
 * упаковка в DoP идёт в dsd_read_dop(); DAC переводится на DoP-вход. */
static void dsd_playback_loop(void)
{
    const char *path = s_dec.url + strlen("file://");
    dsd_info_t info;
    dsd_handle_t dsd = NULL;
    if (dsd_open(path, &info, &dsd) != ESP_OK) {
        ESP_LOGE(TAG, "DSD-файл не открыт: %s", path);
        return;
    }
    s_dec.dsd = dsd;

    /* ID3-теги DSF лежат в конце файла (metadata pointer из заголовка) */
    {
        uint8_t id3buf[8192];
        size_t got = dsd_read_id3(dsd, id3buf, sizeof(id3buf));
        if (got > 0) {
            meta_reset();
            meta_parse_id3_buffer(id3buf, got);
        }
    }

    bool native = (dsd_play_mode_get() == DSD_MODE_NATIVE);

    if (native) {
        esp_err_t nerr = dsd_native_start(info.dsd_rate);
        if (nerr != ESP_OK) {
            ESP_LOGW(TAG, "Native DSD недоступен (%s) — откат на DoP",
                     esp_err_to_name(nerr));
            dsd_play_mode_set(DSD_MODE_DOP);
            native = false;
        }
    }

    if (native) {
        ESP_LOGI(TAG, "DSD: %s, %lu Гц (%d кан.) -> Native DSD (DSD%lu)",
                 info.fmt_name, (unsigned long)info.dsd_rate, info.channels,
                 (unsigned long)(info.dsd_rate / 2822400));
        while (!s_dec.stop_requested) {
            bool eof = false;
            size_t n = dsd_read_raw(dsd, s_dec.out_buf, s_dec.out_buf_size, &eof);
            if (n == 0) break;
            if (dsd_native_write((const uint8_t *)s_dec.out_buf, n) != ESP_OK) break;
            if (eof) break;
        }
        dsd_close(dsd);
        s_dec.dsd = NULL;
        dsd_native_stop();
        ESP_LOGI(TAG, "Native DSD завершён, DAC вернулся в PCM");
        return;
    }

    /* вход DAC -> DoP: mute, смена Reg 1/57, ожидание lock, unmute.
     * Lock при DoP может не подняться так же, как при PCM (детекция иная),
     * поэтому после переключения частоты принудительно снимаем mute —
     * иначе поток замучен навсегда. */
    es9039_set_input(ES9039_INPUT_DOP);
    apply_rate_with_dac(info.dop_fs);
    es9039_mute(false);
    ESP_LOGI(TAG, "DSD: %s, %lu Гц (%d кан.) -> DoP %lu Гц",
             info.fmt_name, (unsigned long)info.dsd_rate, info.channels,
             (unsigned long)info.dop_fs);

    while (!s_dec.stop_requested) {
        bool eof = false;
        size_t n = dsd_read_dop(dsd, (int32_t *)s_dec.out_buf,
                                s_dec.out_buf_size / 4, &eof);
        if (n == 0) break;
        i2s_out_write(s_dec.out_buf, n * sizeof(int32_t));
        if (eof) break;
    }
    dsd_close(dsd);
    s_dec.dsd = NULL;

    /* вернём DAC на PCM-вход — иначе следующий PCM-файл не воспроизведётся */
    es9039_set_input(ES9039_INPUT_PCM);
    ESP_LOGI(TAG, "DSD-воспроизведение завершено, DAC вернулся в PCM");
}

static void decoder_task(void *arg)
{
    (void)arg;

    /* ADC-вход (adc://): passthrough без esp_audio_codec и метаданных */
    if (strncmp(s_dec.url, "adc://", 6) == 0) {
        adc_playback_loop();
        goto cleanup;
    }

    /* DSD-файлы (.dsf/.dff): свой пайплайн, без esp_audio_codec и без ringbuf */
    if (strncmp(s_dec.url, "file://", 7) == 0 && dsd_is_dsd_path(s_dec.url)) {
        if (s_dec.out_buf == NULL) {
            s_dec.out_buf_size = DEC_OUT_BUF_MIN;
            s_dec.out_buf = heap_caps_malloc(s_dec.out_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_dec.out_buf == NULL) {
                s_dec.out_buf = malloc(s_dec.out_buf_size);
            }
        }
        if (s_dec.out_buf != NULL) {
            dsd_playback_loop();
        } else {
            ESP_LOGE(TAG, "Нет памяти под буфер DoP");
        }
        goto cleanup;
    }

    esp_err_t err = register_decoders_once();
    if (err != ESP_OK) {
        goto cleanup;
    }

    /* Метаданные: для файлов с SD — прямое чтение заголовка; для HTTP —
     * потоковое накопление в цикле ниже (meta_feed перед подачей чанка). */
    meta_reset();
    if (strncmp(s_dec.url, "file://", 7) == 0) {
        meta_probe_file(s_dec.url + 7);
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = detect_type(s_dec.url, s_dec.type_override),
        .dec_cfg  = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,     /* ввод произвольной длины: парсер собирает фреймы */
    };
    esp_audio_err_t derr = esp_audio_simple_dec_open(&cfg, &s_dec.dec);
    if (derr != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_simple_dec_open: 0x%x", derr);
        goto cleanup;
    }
    ESP_LOGI(TAG, "Декодер открыт, тип = %s", esp_audio_simple_dec_get_name(cfg.dec_type));

    bool info_known = false;
    bool input_ended = false;

    while (!s_dec.stop_requested) {
        if (s_dec.out_buf == NULL) {
            s_dec.out_buf_size = DEC_OUT_BUF_MIN;
            s_dec.out_buf = heap_caps_malloc(s_dec.out_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_dec.out_buf == NULL) {
                s_dec.out_buf_size = DEC_OUT_BUF_MIN;
                s_dec.out_buf = malloc(s_dec.out_buf_size);
            }
            if (s_dec.out_buf == NULL) {
                ESP_LOGE(TAG, "Нет памяти под выходной буфер PCM");
                break;
            }
        }

        /* читаем чанк из ringbuf */
        size_t chunk = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_dec.ring, &chunk,
                                                          pdMS_TO_TICKS(500), DEC_READ_CHUNK);
        if (data == NULL) {
            if (input_ended) {
                ESP_LOGI(TAG, "Входной поток завершён");
                break;
            }
            continue;   /* ждём новые данные от http_source */
        }

        /* HTTP: теги MP3/FLAC извлекаем из потока до передачи декодеру;
         * радио-потоки без тегов meta_feed завершит сам (MPEG-sync/лимит) */
        if (strncmp(s_dec.url, "file://", 7) != 0 && !meta_done()) {
            meta_feed(data, chunk);
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = data,
            .len = (uint32_t)chunk,
            .eos = false,
        };
        bool consumed_all = false;
        while (!consumed_all && !s_dec.stop_requested) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = s_dec.out_buf,
                .len = s_dec.out_buf_size,
            };
            esp_audio_err_t r = esp_audio_simple_dec_process(s_dec.dec, &raw, &frame);
            if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                /* расширяем выходной буфер и повторяем */
                uint8_t *bigger = heap_caps_realloc(s_dec.out_buf, frame.needed_size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (bigger == NULL) {
                    bigger = realloc(s_dec.out_buf, frame.needed_size);
                }
                if (bigger != NULL) {
                    s_dec.out_buf = bigger;
                    s_dec.out_buf_size = frame.needed_size;
                    continue;
                }
                ESP_LOGE(TAG, "Не удалось расширить выходной буфер до %lu байт",
                         (unsigned long)frame.needed_size);
                break;
            }
            if (r != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "Декодирование вернуло 0x%x, пропускаем чанк", r);
                break;
            }
            s_dec.consumed += raw.consumed;   /* сжатые байты, съеденные парсером */
            if (frame.decoded_size == 0) {
                break;   /* парсер ещё собирает фрейм */
            }

            if (!info_known) {
                esp_audio_simple_dec_info_t info = { 0 };
                if (esp_audio_simple_dec_get_info(s_dec.dec, &info) == ESP_AUDIO_ERR_OK
                    && info.sample_rate > 0) {
                    ESP_LOGI(TAG, "Аудио: %lu Гц, %d бит, %d кан., %lu кбит/с",
                             (unsigned long)info.sample_rate,
                             info.bits_per_sample, info.channel,
                             (unsigned long)(info.bitrate / 1000));
                    apply_rate_with_dac(info.sample_rate);
                    s_dec.current_bits = info.bits_per_sample;
                    s_dec.current_channel = info.channel;
                    s_dec.info_bitrate = info.bitrate;
                    info_known = true;
                }
            }

            /* точный прогресс: сэмплы до конверсии (байт PCM / (кан * ширина)) */
            uint32_t ch = s_dec.current_channel ? s_dec.current_channel : 2;
            uint32_t by = (s_dec.current_bits ? s_dec.current_bits : 16) / 8;
            if (by > 0) s_dec.samples += frame.decoded_size / (ch * by);

            size_t out_len = convert_to_32bit(frame.buffer, frame.decoded_size,
                                              s_dec.current_bits ? s_dec.current_bits : 16,
                                              s_dec.current_channel ? s_dec.current_channel : 2,
                                              s_dec.out_buf, s_dec.out_buf_size);
            if (out_len > 0) {
                i2s_out_write(s_dec.out_buf, out_len);
            }

            if (raw.len == 0) {
                consumed_all = true;
            }
        }

        vRingbufferReturnItem(s_dec.ring, (void *)data);
    }

cleanup:
    if (s_dec.dec != NULL) {
        esp_audio_simple_dec_close(s_dec.dec);
        s_dec.dec = NULL;
    }
    if (s_dec.out_buf != NULL) {
        free(s_dec.out_buf);
        s_dec.out_buf = NULL;
        s_dec.out_buf_size = 0;
    }
    if (s_dec.read_buf != NULL) {
        free(s_dec.read_buf);
        s_dec.read_buf = NULL;
    }
    if (s_dec.ring != NULL) {
        vRingbufferDelete(s_dec.ring);
        s_dec.ring = NULL;
    }
    s_dec.task = NULL;
    ESP_LOGI(TAG, "Задача декодера завершена");
    /* конец трека: авто-переход (колбэк ставит main.c до старта декодера) */
    if (s_on_finish_cb != NULL && !s_dec.stop_requested
        && strncmp(s_dec.url, "adc://", 6) != 0) {
        s_on_finish_cb();
    }
    vTaskDelete(NULL);
}

/* ------------------------------ Публичный API ----------------------------- */

esp_err_t decoder_start(const char *url, decoder_type_t type, bool is_stream_hint)
{
    if (url == NULL || strlen(url) >= sizeof(s_dec.url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dec.task != NULL) {
        return ESP_ERR_INVALID_STATE;   /* уже играет — сначала decoder_stop() */
    }
    decoder_type_t eff_type = (type != DEC_TYPE_AUTO) ? type : s_type_default;
    memset(&s_dec, 0, sizeof(s_dec));
    snprintf(s_dec.url, sizeof(s_dec.url), "%s", url);
    s_dec.type_override = eff_type;
    s_dec.stop_requested = false;
    s_dec.current_fs = 0;
    i2s_out_set_paused(false);

    /* DSD-файлы (.dsf/.dff): источник не нужен — dsd_dec.c читает файл сам */
    bool is_dsd_file = (strncmp(url, "file://", 7) == 0) && dsd_is_dsd_path(url);
    /* ADC-вход (adc://): источник — задача phono_adc.c (I²S RX + DSP) */
    bool is_adc = (strncmp(url, "adc://", 6) == 0);

    if (!is_dsd_file) {
        s_dec.ring = xRingbufferCreateWithCaps(DEC_RING_SIZE, RINGBUF_TYPE_BYTEBUF,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_dec.ring == NULL) {
            s_dec.ring = xRingbufferCreate(DEC_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
        }
        if (s_dec.ring == NULL) {
            return ESP_ERR_NO_MEM;
        }

        if (is_adc) {
            /* запускаем приём с АЦП в этот же ringbuf */
            esp_err_t aerr = phono_adc_start(s_dec.ring);
            if (aerr != ESP_OK) {
                vRingbufferDelete(s_dec.ring);
                s_dec.ring = NULL;
                return aerr;
            }
        } else {
            /* запускаем HTTP-читатель в этот же ringbuf */
            esp_err_t err = http_source_start(url, s_dec.ring, NULL, NULL);
            if (err != ESP_OK) {
                vRingbufferDelete(s_dec.ring);
                s_dec.ring = NULL;
                return err;
            }
        }
    }

    BaseType_t ok = xTaskCreate(decoder_task, "decoder", DEC_TASK_STACK, NULL,
                                DEC_TASK_PRIO, &s_dec.task);
    if (ok != pdPASS) {
        if (s_dec.ring != NULL) {
            http_source_stop();
            vRingbufferDelete(s_dec.ring);
            s_dec.ring = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Воспроизведение: %s (%s%s)", url, is_stream_hint ? "поток" : "файл",
             is_dsd_file ? ", DoP" : (is_adc ? ", ADC" : ""));
    return ESP_OK;
}

void decoder_stop(void)
{
    s_dec.stop_requested = true;
    s_dec.paused = false;
    i2s_out_set_paused(false);
    phono_adc_stop();      /* ADC-источник: глушим задачу ДО удаления ringbuf */
    http_source_stop();
    int waited = 0;
    while (s_dec.task != NULL && waited < DEC_STOP_DRAIN_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
}

bool decoder_running(void)
{
    return s_dec.task != NULL && !s_dec.stop_requested;
}

void decoder_set_type(decoder_type_t type)
{
    s_type_default = type;
}

/* ------------------------------ Пауза -------------------------------------
 * Файлы: i2s_out_write перестаёт подавать данные — DMA досиграет буферы,
 * auto_clear даёт тишину, декодер стоит в месте записи. Для радиопотока
 * пауза реализуется mute DAC: декодер продолжает работать (буфер не
 * переполняется, при снятии паузы звук возвращается без рывка). */
void decoder_pause(void)
{
    if (!decoder_running() || s_dec.paused) return;
    s_dec.paused = true;
    i2s_out_set_paused(true);
    if (http_source_is_stream()) {
        es9039_mute(true);
    }
}

void decoder_resume(void)
{
    if (!s_dec.paused) return;
    s_dec.paused = false;
    i2s_out_set_paused(false);
    if (http_source_is_stream()) {
        es9039_mute(false);
    }
}

bool decoder_paused(void)
{
    return s_dec.task != NULL && s_dec.paused;
}

void decoder_set_on_finish(void (*cb)(void))
{
    s_on_finish_cb = cb;
}

/* Прогресс файла: pos/total — прочитанное из файла (приближение позиции
 * воспроизведения: за вычетом буфера ringbuf ~0.3-1 с аудио). Для радио
 * total = 0. */
void decoder_progress(uint64_t *pos, uint64_t *total)
{
    if (pos)   *pos = 0;
    if (total) *total = 0;
    if (!decoder_running()) return;

    if (s_dec.dsd != NULL) {
        dsd_get_progress(s_dec.dsd, pos, total);
        return;
    }
    http_source_progress(pos, total);
}

/* Точный прогресс: миллисекунды позиции и длительности.
 *   DSD:  байты на канал -> мс (1 байт/канал = 8 DSD-сэмплов): точные цифры
 *         из заголовка файла.
 *   PCM:  позиция = декодированные сэмплы / fs (реальная позиция, не позиция
 *         чтения файла). Длительность: file_total*8000/bitrate (битрейт от
 *         декодера: MP3 — из первого кадра, FLAC/WAV — может быть 0) либо
 *         эмпирика consumed/played (работает и для VBR, но как среднее —
 *         погрешность до секунд в начале VBR-файла).
 * seekable: файл (не радио) с известным размером и длительностью. */
void decoder_progress_ext(uint32_t *pos_ms, uint32_t *total_ms, bool *seekable)
{
    if (pos_ms)   *pos_ms = 0;
    if (total_ms) *total_ms = 0;
    if (seekable) *seekable = false;
    if (!decoder_running()) return;

    /* ADC: живой поток — позиция по сэмплам, длительности нет, не перематывается */
    if (strncmp(s_dec.url, "adc://", 6) == 0) {
        if (s_dec.current_fs > 0 && s_dec.samples > 0) {
            if (pos_ms) *pos_ms = (uint32_t)((s_dec.samples * 1000ULL) / s_dec.current_fs);
        }
        if (seekable) *seekable = false;
        return;
    }

    if (s_dec.dsd != NULL) {
        uint64_t pos = 0, total = 0;
        dsd_get_progress(s_dec.dsd, &pos, &total);
        uint32_t rate = dsd_get_rate(s_dec.dsd);
        if (rate > 0) {
            if (pos_ms)   *pos_ms   = (uint32_t)((pos * 8000ULL) / rate);
            if (total_ms) *total_ms = (uint32_t)((total * 8000ULL) / rate);
        }
        if (seekable) *seekable = (total > 0);
        return;
    }

    /* PCM */
    uint64_t fpos = 0, ftotal = 0;
    http_source_progress(&fpos, &ftotal);
    if (s_dec.current_fs > 0 && s_dec.samples > 0) {
        if (pos_ms) *pos_ms = (uint32_t)((s_dec.samples * 1000ULL) / s_dec.current_fs);
    }
    uint32_t tm = 0;
    if (ftotal > 0) {
        if (s_dec.info_bitrate > 0) {
            tm = (uint32_t)((ftotal * 8000ULL) / s_dec.info_bitrate);
        } else if (s_dec.consumed > 0) {
            uint32_t pm = (s_dec.current_fs > 0)
                ? (uint32_t)((s_dec.samples * 1000ULL) / s_dec.current_fs) : 0;
            if (pm > 2000) {
                tm = (uint32_t)((ftotal * (uint64_t)pm) / s_dec.consumed);
            }
        }
    }
    if (total_ms) *total_ms = tm;
    if (seekable) *seekable = (tm > 0) && !http_source_is_stream();
}

/* --------------------------- Перемотка ------------------------------------
 * Стратегия: рестарт конвейера с байтового офсета. Источник (file:// — fseek,
 * http:// — Range) начинает чтение с офсета, декодер пересинхронизируется
 * (MP3 — MPEG sync; FLAC/WAV — требуют заголовок, не поддерживаются).
 * Так пайплайн не остаётся с полубуфером старого контента, а длительность
 * почти не заметна (0.3-1 с на рестарт + переключение частоты DAC).
 *
 * Точность:
 *   - DSD: seek точный (кадры фиксированного размера, из заголовка);
 *   - MP3 CBR: ~1-2 кадра; VBR: по эмпирике bytes/sec глобально — погрешность
 *     до секунд в переменных файлах (TODO: Xing/Info header, FLAK seektable).
 *
 * Пауза сохраняется: если позицию меняли на паузе — вернёмся в паузу. */
esp_err_t decoder_seek(uint32_t ms)
{
    if (!decoder_running()) return ESP_ERR_INVALID_STATE;
    if (strncmp(s_dec.url, "adc://", 6) == 0) {
        ESP_LOGW(TAG, "перемотка ADC-входа невозможна (живой поток)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_dec.dsd != NULL) {
        return dsd_seek_ms(s_dec.dsd, ms);      /* точный seek в живом декодере */
    }
    if (http_source_is_stream()) {
        ESP_LOGW(TAG, "перемотка радио невозможна");
        return ESP_ERR_NOT_SUPPORTED;
    }
    decoder_type_t eff = detect_type(s_dec.url, s_dec.type_override);
    if ((int)eff != (int)ESP_AUDIO_SIMPLE_DEC_TYPE_MP3) {
        /* FLAC/WAV: рестарт с середины требует заголовок — пока не поддержано
         * (TODO: проверить mid-stream sync на железе; для FLAC добавить seektable). */
        ESP_LOGW(TAG, "перемотка для %s пока не поддерживается",
                 esp_audio_simple_dec_get_name(eff));
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t fpos = 0, ftotal = 0;
    http_source_progress(&fpos, &ftotal);
    uint64_t offset;
    uint32_t played_ms = (s_dec.current_fs > 0)
        ? (uint32_t)((s_dec.samples * 1000ULL) / s_dec.current_fs) : 0;

    if (played_ms > 2000 && s_dec.consumed > 0) {
        /* эмпирика: сжатые байты на секунду по факту (VBR-устойчиво) */
        offset = ((uint64_t)s_dec.consumed * ms) / played_ms;
    } else if (s_dec.info_bitrate > 0) {
        offset = ((uint64_t)s_dec.info_bitrate / 8) * (ms / 1000);
    } else {
        ESP_LOGW(TAG, "нет данных для расчёта офсета — seek отклонён");
        return ESP_ERR_INVALID_STATE;
    }
    if (ftotal > 0 && offset >= ftotal) {
        offset = (ftotal > 1024) ? ftotal - 1024 : 0;
    }

    bool was_paused = s_dec.paused;
    char url[512];
    snprintf(url, sizeof(url), "%s", s_dec.url);
    decoder_type_t type = s_dec.type_override;

    ESP_LOGI(TAG, "Seek: %lu мс -> офсет %llu (%s)", (unsigned long)ms,
             (unsigned long long)offset, url);
    http_source_set_start_offset(offset);
    decoder_stop();
    esp_err_t e = decoder_start(url, type, false);
    if (e == ESP_OK && was_paused) {
        decoder_pause();
    }
    return e;
}
