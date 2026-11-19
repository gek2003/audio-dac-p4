/*
 * sd_card.h — microSD-карта платы Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
 *
 * Подключение по схеме (SDMMC2, 4-bit):
 *   CLK -> GPIO43, CMD -> GPIO44, D0 -> GPIO39, D1 -> GPIO40,
 *   D2 -> GPIO41, D3 -> GPIO42. Питание SD — от внутреннего LDO (канал 4).
 *
 * ВАЖНО: этот модуль только ИНИЦИАЛИЗИРУЕТ карту (SDMMC-хост + карта),
 * НЕ монтируя FatFS. Владение картой раздаёт модуль usb_msc.c (TinyUSB MSC):
 *   - монтирует FatFS в /sdcard, когда карта у приложения;
 *   - при подключении кабеля ПК передаёт карту USB-хосту (ПК видит её как
 *     обычный USB-диск, может читать и писать файлы);
 *   - при отсоединении/Eject — монтирует обратно.
 * Playback поддерживает URL "file:///sdcard/<путь>/<файл>" (http_source.c).
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "driver/sdmmc_host.h"

/* Инициализация SDMMC-хоста (4-bit, 20 МГц по умолчанию) и карты.
 * Идемпотентен. FatFS НЕ монтируется — это делает usb_msc_init(). */
esp_err_t sd_card_init(void);

/* Готова ли карта (инициализирована и отвечает). */
bool sd_card_ready(void);

/* Дескриптор карты для MSC-компонента (esp_tinyusb).
 * NULL до первого успешного sd_card_init(). */
sdmmc_card_t *sd_card_get_card(void);

/* Первый аудиофайл (.mp3/.flac/.wav) в дереве, начиная с root ("/sdcard").
 * Работает только пока FatFS смонтирована у приложения (см. usb_msc.c).
 * Поиск в глубину, максимум 3 уровня папок.
 * Возвращает true, если файл найден (out: полный путь без "file://"). */
bool sd_card_find_first_audio(const char *root, char *out, size_t cap);
