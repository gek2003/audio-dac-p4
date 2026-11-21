/*
 * ============================================================================
 *  usb_flash.c — флешка как источник: USB-хост на OTG Full-Speed (GPIO24/25)
 *
 *  Стек (снизу вверх):
 *    usb/usb_host  (espressif/usb — hcd/usbh, периферия 1 = USB_DWC_FS)
 *      -> usb_host_msc (espressif/usb_host_msc — BOT/SCSI)
 *        -> FatFS BDL (IDF 6: esp_vfs_fat_bdl_mount)
 *          -> VFS "/usb" -> stdio -> http_source.c (file://)
 *
 *  Потоки:
 *    - задача MSC-драйвера (создаётся msc_host_install): клиентские события
 *      usb_host + BOT-транзакции;
 *    - наша задача lib_task: только usb_host_lib_handle_events — события
 *      уровня библиотеки (подключение/извлечение); без неё хост не увидит
 *      ничего;
 *    - наша задача worker: тяжёлые шаги (install_device, vfs_register),
 *      вынесены из колбэка MSC — тот вызывает нас из своей задачи, а
 *      перечисление устройства может тянуться сотни мс.
 *
 *  Конкуренция с TinyUSB MSC (SD как диск ПК): TinyUSB занимает OTG HS,
 *  мы — OTG FS; стеки независимы. Если у вас TinyUSB сконфигурирован на FS
 *  (нестандартно) — инициализация usb_host завершится ошибкой в занятой
 *  периферии, см. лог.
 * ============================================================================
 */

#include "usb_flash.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

static const char *TAG = "usb_flash";

#define USB_LIB_TASK_STACK   (4 * 1024)
#define USB_LIB_TASK_PRIO    4
#define USB_LIB_TASK_CORE    0            /* источник-задачи на ядрах не критичны */
#define USB_WORKER_STACK     (6 * 1024)
#define USB_WORKER_PRIO      5
#define USB_QUEUE_LEN        4

/* VFS-путь флешки — согласован с веб-пультом и README. */
#define USB_MOUNT_PATH       "/usb"

/* Состояние */
static volatile bool s_inited;
static volatile bool s_mounted;
static char          s_info[80] = "";        /* "SanDisk 64GB" */
static usb_flash_hooks_t s_hooks;

/* MSC-сущности (одна флешка за раз — обрабатываем последнюю) */
static msc_host_device_handle_t s_dev;
static msc_host_vfs_handle_t    s_vfs;

/* События MSC -> очередь worker'а */
typedef enum { EVT_CONNECT = 1, EVT_DISCONNECT } usb_evt_t;
typedef struct {
    usb_evt_t type;
    union {
        uint8_t addr;                    /* для CONNECT */
        msc_host_device_handle_t dev;    /* для DISCONNECT (хендл устройства) */
    } u;
} usb_msg_t;
static QueueHandle_t s_queue;

/* --------------------------- Утилиты -------------------------------------- */

static void wchar_to_ascii(const wchar_t *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != 0 && o + 1 < cap; i++) {
        wchar_t c = in[i];
        out[o++] = (c < 0x80) ? (char)c : '?';   /* не-ASCII заменяем */
    }
    out[o] = '\0';
    /* подрезать пробелы в конце */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == 0)) out[--o] = '\0';
}

/* --------------------------- Монтирование/размонтирование ------------------ */

static void do_mount(uint8_t addr)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "Флешка уже смонтирована — игнорирую второе устройство (addr %u)", addr);
        return;
    }

    esp_err_t err = msc_host_install_device(addr, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "msc_host_install_device(addr %u): %s", addr, esp_err_to_name(err));
        return;
    }

    msc_host_device_info_t info = { 0 };
    if (msc_host_get_device_info(s_dev, &info) == ESP_OK) {
        char prod[MSC_STR_DESC_SIZE] = "", man[MSC_STR_DESC_SIZE] = "";
        wchar_to_ascii(info.iProduct, prod, sizeof(prod));
        wchar_to_ascii(info.iManufacturer, man, sizeof(man));
        uint64_t mb = ((uint64_t)info.sector_count * info.sector_size) / (1024ULL * 1024ULL);
        snprintf(s_info, sizeof(s_info), "%s %llu МБ",
                 prod[0] ? prod : (man[0] ? man : "USB flash"),
                 (unsigned long long)mb);
        ESP_LOGI(TAG, "Флешка: %s (VID 0x%04X PID 0x%04X, %lu x %lu)",
                 s_info, info.idVendor, info.idProduct,
                 (unsigned long)info.sector_count, (unsigned long)info.sector_size);
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,     /* флешку пользователя не форматируем */
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    err = msc_host_vfs_register(s_dev, USB_MOUNT_PATH, &mount_cfg, &s_vfs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FatFS не смонтировалась (%s) — флешка в NTFS/EXT?", esp_err_to_name(err));
        msc_host_uninstall_device(s_dev);
        s_dev = NULL;
        return;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "Готово: %s -> %s", USB_MOUNT_PATH, s_info);
    if (s_hooks.on_mounted) s_hooks.on_mounted();
}

static void do_unmount(msc_host_device_handle_t dev)
{
    if (dev != s_dev) return;                /* не наша — не наша забота */
    if (s_mounted) {
        msc_host_vfs_unregister(s_vfs);
        s_vfs = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "Флешка извлечена (%s)", s_info);
        if (s_hooks.on_unmounted) s_hooks.on_unmounted();
    }
    if (s_dev) {
        msc_host_uninstall_device(s_dev);
        s_dev = NULL;
    }
    s_info[0] = '\0';
}

/* --------------------------- Worker MSC-событий ---------------------------- */

static void usb_worker(void *arg)
{
    usb_msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.type == EVT_CONNECT) {
            do_mount(m.u.addr);
        } else if (m.type == EVT_DISCONNECT) {
            do_unmount(m.u.dev);
        }
    }
}

/* --------------------------- Колбэк MSC-драйвера --------------------------- */

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    usb_msg_t m = { 0 };
    if (event->event == MSC_DEVICE_CONNECTED) {
        m.type = EVT_CONNECT;
        m.u.addr = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        m.type = EVT_DISCONNECT;
        m.u.dev = event->device.handle;
    } else {
        return;
    }
    xQueueSend(s_queue, &m, 0);
}

/* --------------------------- Задача событий usb_host -----------------------
 * usb_host_lib_handle_events обязана вызываться постоянно — это события
 * БИБЛИОТЕКИ (hub/enumeration), MSC-драйвер не вызывает её сам. */

static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t event_flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
        (void)usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
        // все устройства освобождены — можно продолжать ждать подключения
        }
    }
}

/* --------------------------- Публичный API --------------------------------- */

bool usb_flash_mounted(void)            { return s_mounted; }
const char *usb_flash_info(void)        { return s_info; }

void usb_flash_set_hooks(const usb_flash_hooks_t *hooks)
{
    s_hooks = *hooks;
}

esp_err_t usb_flash_init(void)
{
    if (s_inited) return ESP_OK;

    /* 1) USB-хост на периферии 1 (USB_DWC_FS -> GPIO24/25, разъём USB FS).
     *    HS (периферия 0) остаётся за TinyUSB MSC (SD как диск ПК). */
    usb_host_config_t hc = {
        .skip_phy_setup = false,
        .peripheral_map = 1u << 1,       /* BIT(1): на P4 это USB_DWC_FS */
    };
    esp_err_t err = usb_host_install(&hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return err;
    }

    /* 2) Задача событий библиотеки */
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", USB_LIB_TASK_STACK,
                                NULL, USB_LIB_TASK_PRIO, NULL, USB_LIB_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "не создать задачу usb_lib");
        return ESP_ERR_NO_MEM;
    }

    /* 3) Очередь + worker */
    s_queue = xQueueCreate(USB_QUEUE_LEN, sizeof(usb_msg_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(usb_worker, "usb_wrk", USB_WORKER_STACK,
                                NULL, USB_WORKER_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "не создать задачу usb_worker");
        return ESP_ERR_NO_MEM;
    }

    /* 4) MSC-драйвер со своей фоновой задачей */
    msc_host_driver_config_t mc = {
        .create_backround_task = true,
        .task_priority = USB_WORKER_PRIO,
        .stack_size = 8 * 1024,
        .core_id = tskNO_AFFINITY,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    err = msc_host_install(&mc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "USB-хост готов (OTG FS, GPIO24/25) — ждём флешку");
    return ESP_OK;
}
