/*
 * usb_msc.c — SD-карта как USB Mass Storage для ПК (esp_tinyusb v3.x).
 *
 * API сверено с include/tinyusb_msc.h компонента espressif/esp_tinyusb 3.0.1:
 *   tinyusb_msc_install_driver()  — MSC-драйвер (+ задача TinyUSB);
 *   tinyusb_msc_new_storage_sdmmc() — хранилище поверх sdmmc_card_t;
 *   конфиг: .fat_fs.base_path = "/sdcard", .mount_point = APP;
 *   события: TINYUSB_MSC_EVENT_MOUNT_START/COMPLETE c mount_point (USB/APP).
 * По умолчанию (auto_mount_off = 0) компонент сам переключает FatFS:
 * ПК смонтировал — у нас unmount; ПК отсоединил/Eject — mount обратно.
 * Производительность на P4 (по данным компонента, FIFO 8192):
 * чтение ~4.7 МБ/с, запись ~2.1 МБ/с — быстрее, чем нужно для FLAC/MP3.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "tinyusb_msc.h"

#include "sd_card.h"
#include "usb_msc.h"

static const char *TAG = "usb_msc";

static tinyusb_msc_storage_handle_t s_storage;
static usb_msc_owner_t s_owner = USB_MSC_OWNER_NONE;
static usb_msc_callbacks_t s_cb;

static void storage_event_cb(tinyusb_msc_storage_handle_t handle,
                             tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        /* Начало переключения владения. ПК забирает карту: до unmount FatFS
         * останавливаем чтение file:// — иначе fread упадёт на закрытом VFS. */
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
            s_owner = USB_MSC_OWNER_HOST;
            ESP_LOGW(TAG, "Карта передаётся ПК (USB MSC): FatFS будет размонтирована");
            if (s_cb.on_take_by_host) s_cb.on_take_by_host();
        } else {
            ESP_LOGI(TAG, "Карта возвращается приложению...");
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
            s_owner = USB_MSC_OWNER_HOST;
            ESP_LOGI(TAG, "Карта у ПК (диск смонтирован хостом)");
        } else {
            s_owner = USB_MSC_OWNER_APP;
            ESP_LOGI(TAG, "Карта у приложения: FatFS смонтирована в /sdcard");
            if (s_cb.on_return_to_app) s_cb.on_return_to_app();
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "Переключение владения картой не удалось (mount_point=%d)",
                 (int)event->mount_point);
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGW(TAG, "На карте нет FAT-ФС: отформатируйте её на ПК");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGE(TAG, "Форматирование карты не удалось");
        break;
    default:
        break;
    }
}

esp_err_t usb_msc_init(const usb_msc_callbacks_t *cb)
{
    if (s_owner != USB_MSC_OWNER_NONE) {
        return ESP_OK;              /* уже инициализировано */
    }
    if (!sd_card_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cb = *cb;   /* колбэки могут быть NULL — проверяем перед вызовом */

    tinyusb_msc_driver_config_t driver_cfg = { 0 };
    driver_cfg.callback = storage_event_cb;
    driver_cfg.callback_arg = NULL;
    /* auto_mount_off = 0: компонент сам переключает FatFS между ПК и нами */

    esp_err_t err = tinyusb_msc_install_driver(&driver_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver: %s", esp_err_to_name(err));
        return err;
    }

    const tinyusb_msc_storage_config_t cfg = {
        .medium.card = sd_card_get_card(),
        .fat_fs = {
            .base_path = "/sdcard",
            .config = {
                .format_if_mount_failed = false,
                .max_files = 5,
                .allocation_unit_size = 16 * 1024,
            },
            .do_not_format = true,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,   /* старт: карта у нас */
    };
    err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc: %s", esp_err_to_name(err));
        return err;
    }
    s_owner = USB_MSC_OWNER_APP;
    ESP_LOGI(TAG, "USB MSC готов: карта как USB-диск (Type-C «USB»)");
    return ESP_OK;
}

usb_msc_owner_t usb_msc_owner(void)
{
    return s_owner;
}

void usb_msc_status_text(char *out, size_t cap)
{
    switch (s_owner) {
    case USB_MSC_OWNER_APP:
        snprintf(out, cap, "USB: SD-карта у плеера");
        break;
    case USB_MSC_OWNER_HOST:
        snprintf(out, cap, "USB: карта у ПК");
        break;
    default:
        snprintf(out, cap, "USB: не инициализировано");
        break;
    }
}
