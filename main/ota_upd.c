/*
 * ============================================================================
 *  ota_upd.c — обновление прошивки без кабеля: файл с SD или по URL
 *
 *  Реализация на базовом esp_ota_* (компонент app_update), а не esp_https_ota:
 *  один и тот же цикл чтения обслуживает и SD (stdio), и HTTP(S)
 *  (esp_http_client — тот же стек, что и декодер; TLS через системный набор
 *  корневых сертификатов). esp_https_ota дал бы то же самое поверх TLS-обёртки,
 *  но с более жёсткими требованиями к конфигурации TLS.
 *
 *  Порядок записи:
 *    1) esp_ota_get_next_update_partition(NULL) — свободный ota_раздел;
 *    2) esp_ota_begin(..., OTA_SIZE_UNKNOWN, ...) — стирание раздела;
 *    3) цикл: чтение 16 КБ -> esp_ota_write; прогресс каждые 256 КБ;
 *    4) esp_ota_end() — валидация образа (magic, контрольные суммы);
 *    5) esp_ota_set_boot_partition() — новая прошивка активна со следующей
 *       загрузки; esp_restart() через 2 с (HTTP-ответ успевает уйти).
 *
 *  Потокобезопасность: единственная операция за раз (s_state RUNNING,
 *  повторный запрос вернёт ESP_ERR_INVALID_STATE). Декодер можно не
 *  останавливать — чтение с NAS не конфликтует по разделам flash.
 * ============================================================================
 */

#include "ota_upd.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include <stdarg.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sd_card.h"

static const char *TAG = "ota_upd";

#define OTA_BUF_SIZE   (16 * 1024)
#define OTA_TASK_STACK (16 * 1024)      /* esp_ota_write + esp_http_client */
#define OTA_TASK_PRIO  10
#define OTA_REBOOT_MS  2000

/* --- Состояние операции (volatile: читается из httpd-таска) --------------- */
static volatile ota_upd_state_t s_state = OTA_UPD_IDLE;
static volatile int  s_progress = -1;        /* %, -1 = неизвестно */
static char          s_msg[128] = "";

/* Установить человечечитаемое сообщение (для /api/status и лога). */
static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof(s_msg), fmt, ap);
    va_end(ap);
}

ota_upd_state_t ota_upd_state(void)     { return s_state; }

/* Подтверждение текущего образа (rollback): PENDING_VERIFY -> VALID. */
esp_err_t ota_upd_confirm_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;   /* otadata отсутствует/не OTA-раздел */
    }
    /* P4c-3/P1-6: диагностика отката. Флаг ставится на время загрузки; если
     * в следующем запуске образ уже VALID, а флаг остался — прошивка не
     * подтвердилась, загрузчик откатился на предыдущую. */
    nvs_handle_t nh = 0;
    bool rolled = false;
    if (nvs_open("ota_upd", NVS_READWRITE, &nh) == ESP_OK) {
        uint8_t fb = 0;
        if (nvs_get_u8(nh, "first_boot", &fb) == ESP_OK && fb == 1) {
            rolled = true;
        }
        nvs_set_u8(nh, "first_boot", 1);
        nvs_commit(nh);
        nvs_close(nh);
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        if (rolled) {
            ESP_LOGW(TAG, "OTA: обнаружен откат на предыдущий образ");
        }
        if (nvs_open("ota_upd", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_u8(nh, "first_boot", 0);
            nvs_commit(nh);
            nvs_close(nh);
        }
        return ESP_OK;                  /* уже подтверждён */
    }
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK) {
        if (nvs_open("ota_upd", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_u8(nh, "first_boot", 0);
            nvs_commit(nh);
            nvs_close(nh);
        }
        ESP_LOGI(TAG, "OTA: прошивка подтверждена, откат отменён");
    } else {
        ESP_LOGW(TAG, "OTA: confirm: 0x%x", e);
    }
    return e;
}
int  ota_upd_progress(void)             { return s_progress; }
const char *ota_upd_msg(void)           { return s_msg; }

/* --------------------------- Ядро записи ---------------------------------- */

/* Записать поток образа из произвольного источника. reader возвращает число
 * прочитанных байт (0 = конец, <0 = ошибка). */
typedef int (*ota_reader_t)(void *arg, uint8_t *buf, size_t cap);

static esp_err_t ota_write_stream(ota_reader_t reader, void *arg, int64_t total_hint)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        set_msg("нет OTA-партиции (см. partitions.csv)");
        ESP_LOGE(TAG, "%s", s_msg);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *buf = heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(OTA_BUF_SIZE);
    }
    if (buf == NULL) {
        set_msg("нет памяти под буфер записи");
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        set_msg("esp_ota_begin: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_msg);
        free(buf);
        return err;
    }

    ESP_LOGI(TAG, "Пишу образ в %s (размер ~%lld)", update->label, (long long)total_hint);

    int64_t written = 0;
    int64_t next_report = 256 * 1024;
    bool io_failed = false;

    for (;;) {
        int n = reader(arg, buf, OTA_BUF_SIZE);
        if (n == 0) break;
        if (n < 0) {
            io_failed = true;
            break;
        }

        err = esp_ota_write(ota, buf, n);
        if (err != ESP_OK) {
            set_msg("esp_ota_write: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", s_msg);
            esp_ota_abort(ota);
            free(buf);
            return err;
        }
        written += n;
        if (total_hint > 0 && written >= next_report) {
            s_progress = (int)((written * 100) / total_hint);
            if (s_progress > 99) s_progress = 99;
            next_report += 256 * 1024;
        }
    }
    free(buf);

    if (io_failed || written < 32) {
        set_msg(io_failed ? "обрыв чтения образа" : "образ пуст");
        ESP_LOGE(TAG, "%s (записано %lld)", s_msg, (long long)written);
        esp_ota_abort(ota);
        return ESP_FAIL;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        set_msg("esp_ota_end: %s (не образ ESP-IDF?)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_msg);
        return err;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        set_msg("esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_msg);
        return err;
    }

    s_progress = 100;
    ESP_LOGI(TAG, "Образ записан (%lld байт), загрузка из %s после перезагрузки",
             (long long)written, update->label);
    return ESP_OK;
}

/* --------------------------- Источники ------------------------------------ */

typedef struct {
    FILE *f;
    int64_t total;
} file_reader_t;

static int file_read(void *arg, uint8_t *buf, size_t cap)
{
    file_reader_t *fr = arg;
    return (int)fread(buf, 1, cap, fr->f);
}

typedef struct {
    esp_http_client_handle_t cl;
} http_reader_t;

static int http_read(void *arg, uint8_t *buf, size_t cap)
{
    http_reader_t *hr = arg;
    return esp_http_client_read(hr->cl, (char *)buf, cap);  /* 0 = конец, <0 = ошибка */
}

/* --------------------------- Фоновая задача ------------------------------- */

typedef struct {
    bool is_file;
    char source[512];       /* SD: полный путь ("/sdcard/firmware.bin"); URL: как есть */
} ota_job_t;

static void ota_task(void *arg)
{
    ota_job_t job;
    memcpy(&job, arg, sizeof(job));
    free(arg);
    arg = NULL;

    esp_err_t err = ESP_FAIL;

    if (job.is_file) {
        file_reader_t fr = { 0 };
        fr.f = fopen(job.source, "rb");
        if (fr.f == NULL) {
            set_msg("не открыть файл: %s", job.source);
            ESP_LOGE(TAG, "%s", s_msg);
            s_state = OTA_UPD_FAIL;
            vTaskDelete(NULL);
            return;
        }
        fseek(fr.f, 0, SEEK_END);
        fr.total = (int64_t)ftell(fr.f);
        fseek(fr.f, 0, SEEK_SET);

        /* Проверка образа заранее — не стираем раздел ради случайного файла. */
        uint8_t magic = 0;
        if (fr.total < 32 || fread(&magic, 1, 1, fr.f) != 1 || magic != 0xE9) {
            set_msg("это не образ прошивки (нет 0xE9)");
            ESP_LOGE(TAG, "%s", s_msg);
            fclose(fr.f);
            s_state = OTA_UPD_FAIL;
            vTaskDelete(NULL);
            return;
        }
        fseek(fr.f, 0, SEEK_SET);

        s_progress = 0;
        err = ota_write_stream(file_read, &fr, fr.total);
        fclose(fr.f);
    } else {
        const esp_http_client_config_t cfg = {
            .url = job.source,
            .buffer_size = OTA_BUF_SIZE,
            .buffer_size_tx = 2048,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,   /* https: корневые сертификаты */
        };
        esp_http_client_handle_t cl = esp_http_client_init(&cfg);
        if (cl == NULL) {
            set_msg("не создать HTTP-клиент");
            ESP_LOGE(TAG, "%s", s_msg);
            s_state = OTA_UPD_FAIL;
            vTaskDelete(NULL);
            return;
        }
        esp_err_t oerr = esp_http_client_open(cl, -1);
        if (oerr != ESP_OK) {
            set_msg("не открыть URL: %s", esp_err_to_name(oerr));
            ESP_LOGE(TAG, "%s", s_msg);
            esp_http_client_cleanup(cl);
            s_state = OTA_UPD_FAIL;
            vTaskDelete(NULL);
            return;
        }
        (void)esp_http_client_fetch_headers(cl);          /* Content-Length */
        int status = esp_http_client_get_status_code(cl);
        if (status < 200 || status >= 300) {
            set_msg("сервер ответил %d", status);
            ESP_LOGE(TAG, "%s", s_msg);
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
            s_state = OTA_UPD_FAIL;
            vTaskDelete(NULL);
            return;
        }

        http_reader_t hr = { .cl = cl };
        s_progress = 0;
        err = ota_write_stream(http_read, &hr,
                               esp_http_client_get_content_length(cl));
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
    }

    if (err == ESP_OK) {
        s_state = OTA_UPD_OK;
        set_msg("готово: перезагрузка...");
        vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_MS));
        esp_restart();      /* на новый образ — сюда не возвращаемся */
    }

    s_state = OTA_UPD_FAIL;
    ESP_LOGW(TAG, "OTA не удался: %s", s_msg);
    vTaskDelete(NULL);
}

/* --------------------------- Старт операции ------------------------------- */

static esp_err_t start_job(bool is_file, const char *source)
{
    if (source == NULL || source[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == OTA_UPD_RUNNING) {
        set_msg("обновление уже идёт");
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_ota_get_next_update_partition(NULL) == NULL) {
        set_msg("нет OTA-партиции (см. partitions.csv)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ota_job_t *job = malloc(sizeof(ota_job_t));
    if (job == NULL) return ESP_ERR_NO_MEM;
    job->is_file = is_file;

    if (is_file) {
        if (strstr(source, "..") != NULL) {
            free(job);
            return ESP_ERR_INVALID_ARG;
        }
        if (!sd_card_ready()) {
            free(job);
            set_msg("SD-карта не готова");
            return ESP_ERR_INVALID_STATE;
        }
        if (source[0] == '/') {
            /* путь уже полный (например "/sdcard/firmware.bin") */
            snprintf(job->source, sizeof(job->source), "%s", source);
        } else {
            snprintf(job->source, sizeof(job->source), "/sdcard/%s", source);
        }
        ESP_LOGI(TAG, "Проверяю файл: %s", job->source);
        FILE *f = fopen(job->source, "rb");
        if (f == NULL) {
            set_msg("не открыть файл: %s", job->source);
            free(job);
            return ESP_ERR_NOT_FOUND;
        }
        fseek(f, 0, SEEK_END);
        int64_t sz = (int64_t)ftell(f);
        fclose(f);
        if (sz < 32) {
            free(job);
            set_msg("файл подозрительно мал (%lld)", (long long)sz);
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        snprintf(job->source, sizeof(job->source), "%s", source);
    }

    s_state = OTA_UPD_RUNNING;
    s_progress = 0;
    set_msg("");

    BaseType_t ok = xTaskCreate(ota_task, "ota_upd", OTA_TASK_STACK, job,
                                OTA_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        free(job);
        s_state = OTA_UPD_IDLE;
        set_msg("не создать задачу OTA");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "OTA запущен: %s", job->source);
    return ESP_OK;
}

esp_err_t ota_upd_start_file(const char *path)
{
    return start_job(true, path);
}

esp_err_t ota_upd_start_url(const char *url)
{
    if (url == NULL || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return start_job(false, url);
}

/* Версия текущей прошивки — показываем в /api/status до и после OTA. */
const char *ota_upd_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "";
}
