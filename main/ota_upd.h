/*
 * ============================================================================
 *  ota_upd.h — обновление прошивки без кабеля (OTA): файл с SD или по URL
 *
 *  Требования (см. partitions.csv в корне проекта):
 *    - таблица разделов с двумя app-разделами (ota_0/ota_1) + otadata;
 *    - первая прошивка этой таблицы — только по кабелю, дальше — OTA.
 *
 *  Поток:
 *    1) ota_upd_start_file("/firmware.bin") или ota_upd_start_url("http://...")
 *       — запускают фоновую задачу и СРАЗУ возвращают управление;
 *    2) статус: ota_upd_state() (IDLE/RUNNING/OK/FAIL), ota_upd_progress()
 *       (0..100 %), ota_upd_msg() — человекочитаемое сообщение (для /api/status
 *       и лога); esp_ota_write может тянуться до пары минут по Wi-Fi;
 *    3) по успеху через ~2 с (чтобы HTTP-ответ ушёл) плеер сам перезагружается
 *       на новый образ (esp_restart). Ошибка — без ребута, можно повторить.
 *
 *  Проверки перед записью: образ ESP-IDF (первый байт 0xE9), есть свободная
 *  OTA-партиция, карта готова (для SD). После esp_ota_end() — только
 *  esp_ota_set_boot_partition(); rollback не включён (осознанно, см. README).
 * ============================================================================
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_UPD_IDLE = 0,
    OTA_UPD_RUNNING,
    OTA_UPD_OK,
    OTA_UPD_FAIL,
} ota_upd_state_t;

/* Запустить обновление из файла на SD (путь относительно /sdcard или полный).
 * Фоновая задача; статус — ota_upd_state()/progress()/msg(). */
esp_err_t ota_upd_start_file(const char *path);

/* Запустить обновление по URL (http:// или https:// — TLS через системный
 * набор корневых сертификатов, как у декодера). Фоновая задача. */
esp_err_t ota_upd_start_url(const char *url);

/* Состояние/прогресс/сообщение последней (или текущей) операции. */
ota_upd_state_t ota_upd_state(void);
int             ota_upd_progress(void);      /* 0..100, -1 = неизвестно */
const char     *ota_upd_msg(void);           /* "" если нет */

/* Подтвердить текущую прошивку после успешного старта (если включён
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): PENDING_VERIFY -> VALID, откат
 * отменяется. Вызывать из app_main после успешной инициализации. Без
 * rollback-конфига — no-op. */
esp_err_t ota_upd_confirm_app(void);

#ifdef __cplusplus
}
#endif
const char *ota_upd_version(void);
