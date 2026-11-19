/*
 * ============================================================================
 *  stations.c — избранные радио-станции (NVS blob)
 * ============================================================================
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "nvs.h"
#include "esp_log.h"

#include "stations.h"

#define STATIONS_NVS_NS   "dac"
#define STATIONS_NVS_KEY  "stations"

static const char *TAG = "stations";

static station_t s_list[STATIONS_MAX];
static int s_count = 0;

void stations_load(void)
{
    nvs_handle_t h;
    if (nvs_open(STATIONS_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t need = 0;
    if (nvs_get_blob(h, STATIONS_NVS_KEY, NULL, &need) == ESP_OK && need > 0) {
        uint8_t *buf = malloc(need);
        if (buf != NULL) {
            size_t got = need;
            if (nvs_get_blob(h, STATIONS_NVS_KEY, buf, &got) == ESP_OK && got >= 1) {
                int n = buf[0];
                if (n > STATIONS_MAX) n = STATIONS_MAX;
                size_t entry = sizeof(station_t);
                if (n > 0 && got >= 1 + (size_t)n * entry) {
                    memcpy(s_list, buf + 1, (size_t)n * entry);
                    s_count = n;
                }
            }
            free(buf);
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Станций в NVS: %d", s_count);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(STATIONS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS не открылся — список не сохранён");
        return;
    }
    uint8_t buf[1 + STATIONS_MAX * sizeof(station_t)];
    buf[0] = (uint8_t)s_count;
    if (s_count > 0) memcpy(buf + 1, s_list, (size_t)s_count * sizeof(station_t));
    esp_err_t e = nvs_set_blob(h, STATIONS_NVS_KEY, buf, 1 + (size_t)s_count * sizeof(station_t));
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "Сохранение списка: %s", esp_err_to_name(e));
}

int stations_count(void)
{
    return s_count;
}

const station_t *stations_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_list[idx];
}

esp_err_t stations_add(const char *name, const char *url)
{
    if (url == NULL || url[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;      /* избранные — только HTTP-потоки */
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_list[i].url, url) == 0) return ESP_OK;   /* уже есть */
    }
    if (s_count >= STATIONS_MAX) {
        ESP_LOGW(TAG, "Список полон (%d)", STATIONS_MAX);
        return ESP_ERR_NO_MEM;
    }
    if (name == NULL || name[0] == '\0') {
        stations_name_from_url(url, s_list[s_count].name, STATION_NAME_MAX);
    } else {
        snprintf(s_list[s_count].name, STATION_NAME_MAX, "%s", name);
    }
    snprintf(s_list[s_count].url, STATION_URL_MAX, "%s", url);
    s_count++;
    save();
    ESP_LOGI(TAG, "Добавлена станция: %s", s_list[s_count - 1].name);
    return ESP_OK;
}

esp_err_t stations_remove(int idx)
{
    if (idx < 0 || idx >= s_count) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "Удалена станция: %s", s_list[idx].name);
    for (int i = idx; i + 1 < s_count; i++) {
        s_list[i] = s_list[i + 1];
    }
    s_count--;
    save();
    return ESP_OK;
}

void stations_name_from_url(const char *url, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (url == NULL) return;
    const char *p = strstr(url, "://");
    if (p == NULL) return;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0) return;
    snprintf(out, cap, "%.*s", (int)len, p);
}
