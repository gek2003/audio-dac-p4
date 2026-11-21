/*
 * ============================================================================
 *  usb_flash.h — флешка (USB MSC device) как источник аудио, USB OTG FS P4
 *
 *  Плата Waveshare ESP32-P4-WIFI6-Touch-LCD-5 имеет ДВА USB-порта:
 *    - OTG High-Speed (USBD_N/P, чиповые пины 49/50) — занят TinyUSB MSC:
 *      SD-карта видна ПК как диск (usb_msc.c) — НЕ трогаем;
 *    - OTG Full-Speed (USB1P1_N/P -> GPIO24/25) — наш новый HOST: сюда
 *      втыкается флешка (разъём платы, обычно USB-A или второй USB-C через
 *      переходник; на плате он подписан как USB FS).
 *
 *  Как это работает:
 *    usb_flash_init():
 *      1) usb_host_install(peripheral_map = BIT(1)) — хост-стек на OTG FS
 *         (USB_DWC_LL_GET_HW(1) == USB_DWC_FS, проверено по HAL esp32-p4);
 *      2) msc_host_install() — MSC/BOT/SCSI драйвер (espressif/usb_host_msc)
 *         со своей фоновой задачей;
 *      3) собственная задача, которая постоянно дёргает
 *         usb_host_lib_handle_events() — без неё хост-стек не увидит
 *         подключение/извлечение (это события библиотеки, не MSC-драйвера);
 *      4) событие MSC_DEVICE_CONNECTED -> msc_host_install_device() ->
 *         msc_host_vfs_register() — флешка монтируется в "/usb";
 *      5) MSC_DEVICE_DISCONNECTED -> vfs_unregister, остановка воспроиз-
 *         ведения с /usb через хук on_unmounted (main.c).
 *
 *  Источник для плеера — обычный file:///usb/<путь> (http_source.c читает
 *  через stdio, ничего дополнительно не нужно). Браузер файлов веб-пульта
 *  умеет root=usb (см. web_ctrl.c).
 *
 *  Питание: флешку лучше подключать к порту платы напрямую (VBUS от LDO);
 *  прожорливые диски/хабы — через хаб с внешним питанием.
 * ============================================================================
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_mounted)(void);       /* флешка смонтирована в /usb (может быть NULL) */
    void (*on_unmounted)(void);     /* флешка извлечена (может быть NULL) */
} usb_flash_hooks_t;

/* Запустить хост на OTG FS. Идемпотентен. Блокирует до готовности стека
 * (usb_host_install + msc_host_install), НЕ до подключения флешки. */
esp_err_t usb_flash_init(void);

/* Смонтирована ли флешка в /usb. */
bool usb_flash_mounted(void);

/* Человекочитаемое описание устройства ("SanDisk 64GB") или "". */
const char *usb_flash_info(void);

/* Установить хуки жизненного цикла (до или после init — как удобнее). */
void usb_flash_set_hooks(const usb_flash_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
