/*
 * ============================================================================
 *  stations.h — избранные радио-станции: NVS, список, добавление/удаление
 *
 *  Хранение: один NVS-blob в namespace "dac", ключ "stations" — массив
 *  записей { имя 64, url 256 }. Запись при add/remove — немедленная (редкие
 *  операции, debounce не нужен). Дедупликация по URL.
 * ============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define STATIONS_MAX      10
#define STATION_NAME_MAX  64
#define STATION_URL_MAX   256

typedef struct {
    char name[STATION_NAME_MAX];
    char url[STATION_URL_MAX];
} station_t;

/* Загрузить из NVS (idempotent; при отсутствии ключа — пустой список). */
void      stations_load(void);
int       stations_count(void);
const station_t *stations_get(int idx);      /* NULL вне диапазона */
/* Добавить (дедуп по URL). Если список полон — ESP_ERR_NO_MEM. */
esp_err_t stations_add(const char *name, const char *url);
/* Удалить по индексу; возврат ESP_OK при успехе. */
esp_err_t stations_remove(int idx);
/* Имя по умолчанию из URL (hostname[:port]) — для кнопки «Добавить». */
void      stations_name_from_url(const char *url, char *out, size_t cap);
