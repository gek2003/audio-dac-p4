/*
 * ============================================================================
 *  wifi_connect.c — подключение ESP32-P4 к Wi-Fi через сопроцессор ESP32-C6
 *  ESP-IDF v6.1 (esp_wifi_remote + ESP-Hosted по SDIO)
 *
 *  Приложение использует стандартные esp_wifi / esp_netif / esp_event API —
 *  на ESP32-P4 их реализацию предоставляет компонент esp_wifi_remote.
 *
 *  Предварительные условия:
 *    - на плате установлен и прошит ESP32-C6 (прошивка ESP-Hosted);
 *    - в sdkconfig включён ESP-Hosted (CONFIG_ESP_HOSTED_ENABLED=y,
 *      CONFIG_ESP_WIFI_REMOTE_ENABLED=y, настройки SDIO — по разводке платы);
 *    - esp_wifi_remote работает только в диапазоне 2,4 ГГц.
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";
static esp_netif_t *s_sta_netif = NULL;

/* TODO: впишите свои данные или задайте через wifi_set_credentials() */
#define WIFI_DEFAULT_SSID   ""
#define WIFI_DEFAULT_PASS   ""

#define WIFI_RETRY_MAX          10
#define WIFI_DEFAULT_TIMEOUT_MS 30000

static EventGroupHandle_t s_wifi_events;
static int s_retry_count = 0;
static char s_ssid[33]   = WIFI_DEFAULT_SSID;
static char s_pass[65]   = WIFI_DEFAULT_PASS;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_STARTED_BIT   BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA запущен, подключаемся к \"%s\"", s_ssid);
                xEventGroupSetBits(s_wifi_events, WIFI_STARTED_BIT);
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                s_retry_count = 0;
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
                if (s_retry_count < WIFI_RETRY_MAX) {
                    s_retry_count++;
                    ESP_LOGW(TAG, "Соединение потеряно, повтор %d/%d", s_retry_count, WIFI_RETRY_MAX);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "Не удалось подключиться после %d попыток", WIFI_RETRY_MAX);
                }
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Получен IP: " IPSTR ", маска: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask));
    }
}

esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !pass || strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(pass) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));

    /* Стек уже стартовал — применяем конфиг сразу и переподключаемся
     * (вызов из UI, вкладка «Сеть»). */
    if (s_sta_netif != NULL) {
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
        /* Пустой пароль = открытая сеть */
        wc.sta.threshold.authmode = (strlen(s_pass) == 0) ? WIFI_AUTH_OPEN
                                                          : WIFI_AUTH_WPA2_PSK;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.pmf_cfg.required = false;
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (e != ESP_OK) return e;
        (void)esp_wifi_disconnect();   /* если подключены к прежней сети */
        s_retry_count = 0;
        return esp_wifi_connect();
    }
    return ESP_OK;   /* до старта — применится в wifi_connect_start() */
}

/* --- Спящий режим: Wi-Fi полностью выключаем (esp_wifi_stop) ---------------
 * Сопроцессор ESP32-C6 переводится в standby, ток падает до долей мА.
 * Пробуждение: wifi_on() — esp_wifi_start() даёт STA_START, обработчик
 * сам вызывает esp_wifi_connect() к сохранённой сети. */
static bool s_wifi_off = false;

esp_err_t wifi_off(void)
{
    if (s_wifi_off) return ESP_OK;
    if (s_sta_netif == NULL) return ESP_ERR_INVALID_STATE;
    (void)esp_wifi_disconnect();
    esp_err_t e = esp_wifi_stop();
    if (e == ESP_OK) {
        s_wifi_off = true;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi выключен (спящий режим)");
    }
    return e;
}

esp_err_t wifi_on(void)
{
    if (!s_wifi_off) return ESP_OK;
    s_wifi_off = false;
    xEventGroupClearBits(s_wifi_events, WIFI_STARTED_BIT);
    esp_err_t e = esp_wifi_start();    /* STA_START -> esp_wifi_connect() */
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi включается, переподключение к \"%s\"", s_ssid);
    }
    return e;
}

bool wifi_is_off(void)
{
    return s_wifi_off;
}

const char *wifi_get_ssid(void)
{
    return s_ssid;
}

esp_err_t wifi_connect_start(void)
{
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

    /* NVS нужен и стеку Wi-Fi, и драйверу DAC (сохранение настроек) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "default sta");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL), TAG, "reg wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &wifi_event_handler, NULL), TAG, "reg ip");

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid,     s_ssid, sizeof(wifi_config.sta.ssid));
	strlcpy((char *)wifi_config.sta.password, s_pass, sizeof(wifi_config.sta.password));
    /* Пустой пароль = открытая сеть */
    wifi_config.sta.threshold.authmode = (strlen(s_pass) == 0) ? WIFI_AUTH_OPEN
                                                               : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config");
    /* Для стабильного стриминга отключаем энергосбережение Wi-Fi */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set ps");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    return ESP_OK;
}

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Не дождались IP за %lu мс", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    if (s_wifi_events == NULL || s_wifi_off) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_reconnect(void)
{
    if (s_wifi_off) {
        return wifi_on();          /* остановленный стек — стартуем заново */
    }
    if (s_wifi_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_retry_count = 0;
    return esp_wifi_connect();
}

/* --------------------------- Скан сетей -------------------------------------
 * Асинхронный: wifi_scan_start() поднимает короткоживущую задачу с
 * блокирующим esp_wifi_scan_start(); UI опрашивает wifi_scan_state()
 * (0 = idle, 1 = идёт скан, 2 = готово) и берёт записи wifi_scan_get().
 * Скан не рвёт текущее соединение (STA совмещённый скан). */
#include "esp_wifi_types.h"

#define WIFI_SCAN_MAX   20

typedef struct {
    volatile int  state;                 /* 0 idle, 1 running, 2 done */
    int           count;
    wifi_ap_record_t ap[WIFI_SCAN_MAX];
} wifi_scan_ctx_t;

static wifi_scan_ctx_t s_scan = { .state = 0, .count = 0 };

static void wifi_scan_task(void *arg)
{
    (void)arg;
    if (s_wifi_off) {
        /* Скан включён из UI при спящем Wi-Fi: стартуем стек в этой задаче
         * (не в LVGL) и ждём STA_START, иначе esp_wifi_scan_start вернёт
         * ошибку режима. */
        wifi_on();
        if (s_wifi_events != NULL &&
            (xEventGroupWaitBits(s_wifi_events, WIFI_STARTED_BIT, pdFALSE,
                                 pdTRUE, pdMS_TO_TICKS(3000)) & WIFI_STARTED_BIT) == 0) {
            ESP_LOGW(TAG, "скан: стек не стартовал за 3 с");
            s_scan.state = 2;
            vTaskDelete(NULL);
            return;
        }
    }
    wifi_scan_config_t cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,     /* мс на канал — компромисс скорость/полнота */
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);   /* блокирующий в этой задаче */
    s_scan.count = 0;
    if (err == ESP_OK) {
        uint16_t n = WIFI_SCAN_MAX;
        esp_wifi_scan_get_ap_records(&n, s_scan.ap);
        s_scan.count = n;
        /* сортировка по RSSI (сильные сверху) */
        for (int i = 0; i + 1 < n; i++) {
            for (int j = i + 1; j < n; j++) {
                if (s_scan.ap[j].rssi > s_scan.ap[i].rssi) {
                    wifi_ap_record_t tmp = s_scan.ap[i];
                    s_scan.ap[i] = s_scan.ap[j];
                    s_scan.ap[j] = tmp;
                }
            }
        }
        ESP_LOGI(TAG, "Скан: %d сетей", n);
    } else {
        ESP_LOGW(TAG, "Скан не удался: %s", esp_err_to_name(err));
    }
    s_scan.state = 2;
    vTaskDelete(NULL);
}

int wifi_scan_start(void)
{
    if (s_scan.state == 1) return 1;               /* уже идёт */
    if (s_sta_netif == NULL) return -1;            /* стек Wi-Fi не поднят */
    s_scan.state = 1;
    s_scan.count = 0;
    BaseType_t ok = xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL,
                                8, NULL);
    if (ok != pdPASS) {
        s_scan.state = 0;
        return -1;
    }
    return 1;
}

int wifi_scan_state(void)
{
    return s_scan.state;                            /* 0 idle / 1 running / 2 done */
}

int wifi_scan_count(void)
{
    return (s_scan.state == 2) ? s_scan.count : 0;
}

const wifi_ap_record_t *wifi_scan_get(int idx)
{
    if (s_scan.state != 2 || idx < 0 || idx >= s_scan.count) return NULL;
    return &s_scan.ap[idx];
}

void wifi_scan_ack(void)
{
    s_scan.state = 0;                               /* UI забрал результат */
}
