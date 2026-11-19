/*
 * ============================================================================
 *  http_source.c — потоковый читатель HTTP-источников
 *  ESP-IDF v6.1, esp_http_client
 *
 *  Обслуживает оба типа источников одинаково:
 *    - файлы с ПК/NAS: http://<ip-адрес-сервера>/<путь>/файл.mp3|flac
 *      (на ПК достаточно любого HTTP-сервера над папкой с музыкой);
 *    - интернет-радио: Icecast/Shoutcast-потоки (это тоже HTTP, обычно
 *      без Content-Length — поток неограниченной длины).
 *
 *  Данные наливаются в ring buffer, из которого их забирает decoder.c.
 *
 *  Поведение:
 *    - если сервер отдал Content-Length — считаем источник конечным файлом:
 *      по исчерпанию длины вызывается HTTP_SRC_EOF;
 *    - если Content-Length нет (радиопоток) — источник считается
 *      бесконечным: при обрыве автоматически переподключаемся;
 *    - Icy-MetaData: 0 — метаданные радио не вставляются в поток,
 *      декодер получает только аудиофреймы.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "meta.h"

static const char *TAG = "http_source";

#define HTTP_READ_BUF_SIZE   8192        /* размер буфера чтения сети */
#define HTTP_RING_WAIT_MS    2000        /* ожидание свободного места в ringbuf */
#define HTTP_RETRY_MAX       10          /* число переподключений для потока */
#define HTTP_RETRY_BASE_MS   1000        /* базовая пауза переподключения */
#define HTTP_TASK_STACK      8192
#define HTTP_TASK_PRIO       10

typedef enum {
    HTTP_SRC_STARTED = 0,   /* соединение установлено, заголовки получены */
    HTTP_SRC_EOF,           /* конечный файл дочитан до конца */
    HTTP_SRC_ERROR,         /* поток оборвался (для файла — тоже конец) */
} http_source_evt_t;

typedef void (*http_source_cb_t)(http_source_evt_t evt, void *user);

typedef struct {
    char             url[512];
    RingbufHandle_t  ring;
    bool             is_stream;         /* true = радиопоток (без Content-Length) */
    bool             stop_requested;
    TaskHandle_t     task;
    http_source_cb_t cb;
    void            *user;
    bool             is_file;           /* url начинается с file:// — источник с SD */
    volatile uint64_t file_pos;         /* прочитано байт файла (для прогресса) */
    volatile uint64_t file_total;       /* размер файла (0 = неизвестен/поток) */
    volatile uint64_t start_offset;     /* офсет старта (перемотка): file:// — fseek,
                                           http:// — Range: bytes=N- */
    int      icy_metaint;               /* интервал Icy-метаданных (0 = нет) */
    int      icy_left;                  /* аудио-байтов до следующего метаблока */
    int      icy_state;                 /* 0 = аудио, 1 = байт длины, 2 = данные */
    int      icy_meta_len;              /* длина текущего метаблока (байт*16) */
    int      icy_meta_got;
    uint8_t *icy_meta_buf;              /* сборка метаблока (до 4 КБ) */
} http_source_ctx_t;

static http_source_ctx_t s_ctx = { .stop_requested = true };

static void notify(http_source_evt_t evt)
{
    if (s_ctx.cb) {
        s_ctx.cb(evt, s_ctx.user);
    }
}

static esp_err_t open_stream(esp_http_client_handle_t *client, int64_t *content_len)
{
    esp_http_client_config_t cfg = {
        .url = s_ctx.url,
        .method = HTTP_METHOD_GET,
        .buffer_size = HTTP_READ_BUF_SIZE,
        .buffer_size_tx = 2048,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https: системный набор корневых сертификатов */
    };
    *client = esp_http_client_init(&cfg);
    if (*client == NULL) {
        ESP_LOGE(TAG, "Не удалось создать клиент: %s", s_ctx.url);
        return ESP_FAIL;
    }
    esp_http_client_set_header(*client, "Icy-MetaData", "1");   /* радио: StreamTitle */
    esp_http_client_set_header(*client, "User-Agent", "esp32p4-player/1.0");
    esp_http_client_set_header(*client, "Accept", "*/*");
    if (s_ctx.start_offset > 0) {
        /* перемотка HTTP-файла: серверы NAS/ПК почти всегда поддерживают
         * Range; если нет — ответ придёт 200, seek не состоится (см. ниже) */
        char range[48];
        snprintf(range, sizeof(range), "bytes=%llu-", (unsigned long long)s_ctx.start_offset);
        esp_http_client_set_header(*client, "Range", range);
    }

    esp_err_t err = esp_http_client_open(*client, -1);   /* -1 = GET без записи тела */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось открыть соединение: %s", esp_err_to_name(err));
        esp_http_client_cleanup(*client);
        *client = NULL;
        return err;
    }
    *content_len = esp_http_client_fetch_headers(*client);
    int status = esp_http_client_get_status_code(*client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Сервер ответил %d: %s", status, s_ctx.url);
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
        return ESP_FAIL;
    }
    /* Range-ответ: 206 Partial Content — начало и общая длина из Content-Range;
     * 200 — сервер игнорировал Range: стартуем с нуля и сбрасываем офсет
     * (иначе прогресс пойдёт со сдвинутых цифр). */
    if (s_ctx.start_offset > 0) {
        if (status == 206) {
            char *cr = NULL;
            s_ctx.file_pos = s_ctx.start_offset;
            if (esp_http_client_get_header(*client, "Content-Range", &cr) == ESP_OK &&
                cr != NULL) {
                /* формат: "bytes N-M/K", звёздочка означает до конца файла */
                const char *slash = strrchr(cr, '/');
                if (slash != NULL && slash[1] != '*') {
                    s_ctx.file_total = strtoull(slash + 1, NULL, 10);
                }
            }
            ESP_LOGI(TAG, "Range: старт с %llu, всего %llu",
                     (unsigned long long)s_ctx.file_pos, (unsigned long long)s_ctx.file_total);
        } else {
            ESP_LOGW(TAG, "сервер не поддержал Range (%d) — воспроизведение с начала", status);
            s_ctx.start_offset = 0;
            s_ctx.file_pos = 0;
        }
    }

    /* Icy-MetaInt: интервал аудио-байтов между метаблоками StreamTitle */
    char *mi = NULL;
    s_ctx.icy_metaint = 0;
    if (esp_http_client_get_header(*client, "icy-metaint", &mi) == ESP_OK && mi != NULL) {
        int v = atoi(mi);
        if (v > 0 && v < 10 * 1024 * 1024) {
            s_ctx.icy_metaint = v;
            s_ctx.icy_left = v;
            s_ctx.icy_state = 0;
            ESP_LOGI(TAG, "Icy-MetaInt: %d — радио с метаданными", v);
        }
    }
    ESP_LOGI(TAG, "Открыто: %s, Content-Length: %lld", s_ctx.url, (long long)*content_len);
    return ESP_OK;
}

/* --- Icy-метаданные: разделение аудио/мета в потоке -------------------------
 * Поток: metaint байт аудио, 1 байт длины (len*16), len*16 байт мета,
 * снова metaint аудио... Мета не идёт в ringbuf — только парсится. */
static void icy_stream(const uint8_t *data, size_t len)
{
    if (s_ctx.icy_metaint <= 0 || s_ctx.ring == NULL) {
        /* обычный поток без меты — весь чанк в ringbuf */
        size_t offset = 0;
        while (offset < len && !s_ctx.stop_requested) {
            if (xRingbufferSend(s_ctx.ring, data + offset, len - offset,
                                pdMS_TO_TICKS(HTTP_RING_WAIT_MS)) == pdTRUE) {
                offset = len;
            }
        }
        return;
    }
    size_t off = 0;
    while (off < len && !s_ctx.stop_requested) {
        if (s_ctx.icy_state == 0) {
            /* аудио: отдаём до границы метаблока */
            size_t audio = (size_t)s_ctx.icy_left;
            size_t avail = len - off;
            size_t take = audio < avail ? audio : avail;
            if (take > 0) {
                size_t offset = off;
                size_t sent = 0;
                while (sent < take && !s_ctx.stop_requested) {
                    if (xRingbufferSend(s_ctx.ring, data + offset + sent, take - sent,
                                        pdMS_TO_TICKS(HTTP_RING_WAIT_MS)) == pdTRUE) {
                        sent = take;
                    }
                }
                off += take;
                s_ctx.icy_left -= (int)take;
                if (s_ctx.icy_left > 0) return;   /* чанк закончился внутри аудио */
            }
            s_ctx.icy_state = 1;                  /* следующий байт — длина меты */
        } else if (s_ctx.icy_state == 1) {
            s_ctx.icy_meta_len = (int)data[off] * 16;
            off++;
            s_ctx.icy_state = (s_ctx.icy_meta_len > 0) ? 2 : 0;
            if (s_ctx.icy_state == 0) {
                s_ctx.icy_left = s_ctx.icy_metaint;
                /* нулевая мета — часто означает "не изменилась"; в реальном
                 * протоколе это просто пустой блок */
            }
            if (s_ctx.icy_state == 2) {
                s_ctx.icy_meta_got = 0;
            }
        } else { /* state 2: собираем метаблок */
            size_t want = (size_t)(s_ctx.icy_meta_len - s_ctx.icy_meta_got);
            size_t avail = len - off;
            size_t take = want < avail ? want : avail;
            if (s_ctx.icy_meta_buf != NULL && s_ctx.icy_meta_got + take <= 4096) {
                memcpy(s_ctx.icy_meta_buf + s_ctx.icy_meta_got, data + off, take);
            }
            s_ctx.icy_meta_got += (int)take;
            off += take;
            if (s_ctx.icy_meta_got >= s_ctx.icy_meta_len) {
                /* парсим StreamTitle='...' */
                if (s_ctx.icy_meta_buf != NULL) {
                    s_ctx.icy_meta_buf[s_ctx.icy_meta_len] = '\0';
                    const char *p = strstr((const char *)s_ctx.icy_meta_buf, "StreamTitle='");
                    if (p != NULL) {
                        p += 13;
                        char title[META_TITLE_MAX];
                        const char *q = strchr(p, '\'');
                        size_t tl = (q && (size_t)(q - p) < sizeof(title)) ? (size_t)(q - p)
                                                                          : sizeof(title) - 1;
                        snprintf(title, sizeof(title), "%.*s", (int)tl, p);
                        if (title[0] != '\0') {
                            meta_set(title, NULL, NULL);
                        }
                    }
                }
                s_ctx.icy_state = 0;
                s_ctx.icy_left = s_ctx.icy_metaint;
            }
        }
    }
}

/* --- Режим file:// — источник с SD-карты (/sdcard) ------------------------
 * Читает файл обычным fopen/fread и наливает в тот же ringbuf, что и HTTP.
 * EOF — HTTP_SRC_EOF: для файла это нормальный конец, декодер завершится. */
static void file_source_task(void *arg)
{
    (void)arg;
    const char *path = s_ctx.url + 7;    /* "file:///sdcard/..." -> "/sdcard/..." */
    uint8_t *buf = heap_caps_malloc(HTTP_READ_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Нет памяти под буфер чтения");
        notify(HTTP_SRC_ERROR);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Файл: %s", path);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Не открыть файл: %s (карта смонтирована?)", path);
        free(buf);
        notify(HTTP_SRC_ERROR);
        s_ctx.task = NULL;
        vTaskDelete(NULL);
        return;
    }
    notify(HTTP_SRC_STARTED);
    {
        /* размер файла для прогресс-бара */
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz > 0) s_ctx.file_total = (uint64_t)sz;
            fseek(f, 0, SEEK_SET);
        }
        /* перемотка: старт с байтового офсета (seek декодера) */
        if (s_ctx.start_offset > 0) {
            uint64_t off = s_ctx.start_offset;
            if (off >= s_ctx.file_total) off = (s_ctx.file_total > 0) ? s_ctx.file_total - 1 : 0;
            if (fseek(f, (long)off, SEEK_SET) == 0) {
                s_ctx.file_pos = off;
                ESP_LOGI(TAG, "файл: старт с офсета %llu", (unsigned long long)off);
            }
        }
    }
    while (!s_ctx.stop_requested) {
        size_t n = fread(buf, 1, HTTP_READ_BUF_SIZE, f);
        s_ctx.file_pos += n;
        if (n == 0) break;              /* EOF или ошибка — трактуем как конец */
        size_t offset = 0;
        while (offset < n && !s_ctx.stop_requested) {
            if (xRingbufferSend(s_ctx.ring, buf + offset, n - offset,
                                pdMS_TO_TICKS(HTTP_RING_WAIT_MS)) == pdTRUE) {
                offset = n;
            }
        }
    }
    fclose(f);
    free(buf);
    ESP_LOGI(TAG, "Файл дочитан");
    s_ctx.task = NULL;
    if (!s_ctx.stop_requested) notify(HTTP_SRC_EOF);
    vTaskDelete(NULL);
}

static bool url_is_file(const char *url)
{
    return strncmp(url, "file://", 7) == 0;
}

static void http_source_task(void *arg)
{
    (void)arg;
    uint8_t *read_buf = heap_caps_malloc(HTTP_READ_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (read_buf == NULL) {
        ESP_LOGE(TAG, "Нет памяти под буфер чтения");
        notify(HTTP_SRC_ERROR);
        vTaskDelete(NULL);
        return;
    }

    while (!s_ctx.stop_requested) {
        esp_http_client_handle_t client = NULL;
        int64_t content_len = 0;
        int retries = 0;

        esp_err_t err = open_stream(&client, &content_len);
        if (err != ESP_OK) {
            if (s_ctx.stop_requested) break;
            retries++;
            if (retries > HTTP_RETRY_MAX) {
                notify(HTTP_SRC_ERROR);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(HTTP_RETRY_BASE_MS * retries));
            continue;
        }
        s_ctx.is_stream = (content_len <= 0);
        if (s_ctx.icy_metaint > 0 && s_ctx.icy_meta_buf == NULL) {
            s_ctx.icy_meta_buf = heap_caps_malloc(4097, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (retries == 0) {
            notify(HTTP_SRC_STARTED);
        }

        bool eof = false;
        while (!s_ctx.stop_requested && !eof) {
            int len = esp_http_client_read(client, (char *)read_buf, HTTP_READ_BUF_SIZE);
            if (len < 0) {
                /* обрыв соединения */
                eof = true;
            } else if (len == 0) {
                /* конец потока: для файла — нормальный EOF, для радио — обрыв */
                eof = true;
            } else if (len > 0) {
                /* чанк: аудио в ringbuf, мета (если есть) — в парсер StreamTitle */
                icy_stream(read_buf, (size_t)len);
            }
        }

        if (client != NULL) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }

        if (s_ctx.stop_requested) {
            break;
        }
        if (s_ctx.is_stream) {
            /* радиопоток оборвался — переподключаемся с растущей паузой */
            retries++;
            if (retries > HTTP_RETRY_MAX) {
                notify(HTTP_SRC_ERROR);
                break;
            }
            ESP_LOGW(TAG, "Поток оборвался, переподключение %d/%d", retries, HTTP_RETRY_MAX);
            vTaskDelay(pdMS_TO_TICKS(HTTP_RETRY_BASE_MS * retries));
            continue;
        }
        /* конечный файл дочитан */
        notify(HTTP_SRC_EOF);
        break;
    }

    free(read_buf);
    free(s_ctx.icy_meta_buf);
    s_ctx.icy_meta_buf = NULL;
    ESP_LOGI(TAG, "Задача чтения завершена");
    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t http_source_start(const char *url, RingbufHandle_t ring, http_source_cb_t cb, void *user)
{
    if (url == NULL || ring == NULL || strlen(url) >= sizeof(s_ctx.url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.task != NULL) {
        return ESP_ERR_INVALID_STATE;   /* уже читаем другой источник — сначала http_source_stop() */
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    snprintf(s_ctx.url, sizeof(s_ctx.url), "%s", url);
    s_ctx.ring = ring;
    s_ctx.cb = cb;
    s_ctx.user = user;
    s_ctx.stop_requested = false;
    s_ctx.is_file = url_is_file(url);

    /* file:// — читатель SD-карты, иначе HTTP (NAS/радио) */
    const char *task_name = s_ctx.is_file ? "file_src" : "http_src";
    void (*entry)(void *) = s_ctx.is_file ? file_source_task : http_source_task;
    BaseType_t ok = xTaskCreate(entry, task_name, HTTP_TASK_STACK, NULL,
                                HTTP_TASK_PRIO, &s_ctx.task);
    if (ok != pdPASS) {
        s_ctx.stop_requested = true;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void http_source_stop(void)
{
    s_ctx.stop_requested = true;
    /* даём задаче время закрыть соединение */
    int waited = 0;
    while (s_ctx.task != NULL && waited < 5000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
}

bool http_source_running(void)
{
    return s_ctx.task != NULL && !s_ctx.stop_requested;
}

bool http_source_is_stream(void)
{
    return s_ctx.task != NULL && s_ctx.is_stream;
}

void http_source_progress(uint64_t *pos, uint64_t *total)
{
    if (pos)   *pos = s_ctx.file_pos;
    if (total) *total = s_ctx.file_total;
}

void http_source_set_start_offset(uint64_t offset)
{
    s_ctx.start_offset = offset;
}
