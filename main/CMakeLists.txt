# ============================================================================
#  main/CMakeLists.txt — ESP32-P4 + Arvin DAC (ES9039Q2M), ESP-IDF v6.1
#
#  В v6 компонент "driver" не тянет периферию по умолчанию —
#  esp_driver_i2c / esp_driver_gpio / esp_driver_i2s перечислены явно.
#
#  Wi-Fi: на P4 стандартный esp_wifi.h предоставляется esp_wifi_remote
#  (радио на ESP32-C6 через ESP-Hosted по SDIO). Сам esp_hosted тянется
#  как транзитивная зависимость esp_wifi_remote (см. idf_component.yml).
#
#  Декодеры: esp_audio_codec (MP3, FLAC, AAC, OGG, WAV) — managed-компонент
#  Espressif. FLAC требует PSRAM-буферы (P4 — ок, CONFIG_SPIRAM=y).
#
#  Сетевые источники (файлы с ПК/NAS и интернет-радио) — один и тот же стек:
#  esp_http_client (+ esp-tls для https) -> esp_audio_codec -> PCM -> I²S.
#
# FIR-модуль ES9039Q2M: es9039_fir.c + es9039_fir_coeff.c (коэффициенты
# генерируются tools/gen_fir_coeffs.py).
# UI: ui.c (LVGL, EN/RU) + ui_display.c (HX8394/MIPI-DSI) + ui_touch.c (GT911).
# ============================================================================

idf_component_register(
    SRCS
        "es9039q2m_esp32p4_driver.c"
        "es9039_fir.c"
        "es9039_fir_coeff.c"
        "decoder.c"
        "i2s_out.c"
        "http_source.c"
        "wifi_connect.c"
        "sd_card.c"
        "playlist.c"
        "meta.c"
        "stations.c"
        "dsd_dec.c"
        "usb_msc.c"
        "phono_adc.c"
        "phono_dsp_test.c"
        "dst_dec.c"
        "dsd_native.c"
        "web_ctrl.c"          # веб-пульт: HTTP-сервер порт 80
        "ota_upd.c"           # OTA-обновление прошивки (SD/URL)
        "usb_flash.c"         # флешка на OTG FS (GPIO24/25) как источник
        "ui.c"
        "ui_display.c"
        "ui_touch.c"
        "main.c"
        # Кириллические шрифты (Noto Sans, латиница+кириллица, 4bpp):
        "ui_font_ru_16.c"
        "ui_font_ru_28.c"
        "ui_font_ru_40.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # --- штатные компоненты IDF (короткие имена) ---
        esp_driver_i2c              # driver/i2c_master.h
        esp_driver_i2s              # driver/i2s_std.h
        esp_driver_gpio             # driver/gpio.h
        esp_driver_ledc             # driver/ledc.h (подсветка)
        esp_driver_jpeg             # аппаратный JPEG-декодер P4 (обложки альбомов)
        esp_hal_pmu                 # PMU/LDO (питание MIPI-D PHY, esp_ldo_channel.h)
        esp_ringbuf
        esp_timer
        esp_wifi                    # esp_wifi_remote (ESP-Hosted, ESP32-C6)
        esp_netif
        esp_event
        nvs_flash
        # --- источники и декодеры ---
        esp_http_client
        esp_http_server              # веб-пульт (web_ctrl.c)
        app_update                   # esp_ota_* (ota_upd.c)
        esp-tls
        # --- managed-компоненты (имена с префиксом, как в managed_components/) ---
        espressif__esp_audio_codec  # MP3/FLAC/WAV декодер
        lvgl__lvgl                  # LVGL 9.x
        espressif__esp_lvgl_port    # порт LVGL для esp_lcd
        esp_lcd                     # панель/IO (MIPI-DSI) — штатный IDF, без префикса
        espressif__esp_lcd_touch    # базовый esp_lcd_touch
        espressif__esp_lcd_touch_gt911  # GT911 (0x5D/0x14)
        waveshare__esp_lcd_hx8394   # дисплей HX8394, MIPI-DSI 2 lane
        esp_driver_sdmmc            # SDMMC-хост P4 (microSD слот платы)
        sdmmc                       # sdmmc_cmd (проба карты, print_info)
        fatfs                       # esp_vfs_fat (FatFS поверх SD)
        espressif__esp_tinyusb      # USB MSC: карта как USB-диск ПК
        espressif__usb_host_msc     # флешка как источник (usb_flash.c)
        espressif__usb              # usb_host стек (OTG FS)
)
