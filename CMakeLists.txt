# ============================================================================
#  ESP32-P4 + Arvin DAC (ES9039Q2M) — сетевой аудиоплеер
#  ESP-IDF v6.1
#
#  Архитектура Wi-Fi на ESP32-P4:
#    У P4 нет собственного радиомодуля. Wi-Fi реализуется сопроцессором
#    ESP32-C6, подключённым по SDIO (ESP-Hosted). Приложение использует
#    стандартные API esp_wifi.h — их на P4 предоставляет esp_wifi_remote.
#    - Сопроцессор ESP32-C6 должен быть прошит прошивкой ESP-Hosted
#      (esp_hosted/esp_hosted_ng/esp/esp_driver/network_adapter).
#    - Настройки шины SDIO и буферов — в sdkconfig.defaults (см. комментарии
#      к файлу main/idf_component.yml и в ответе ассистента).
#
#  Состав проекта:
#    main/es9039q2m_esp32p4_driver.c — драйвер DAC (I²C: init/volume/filter/
#                                      automute/частота на лету)
#    main/main.c                     — точка входа, wi-fi, источники
#    main/idf_component.yml          — managed-зависимости (esp_hosted,
#                                      esp_wifi_remote, esp_audio_codec)
#
#  Источники звука (добавляются по мере готовности модулей):
#    - файлы с ПК/NAS: HTTP(S)-стриминг mp3/flac через esp_http_client
#      (на ПК достаточно любого HTTP-сервера над папкой с музыкой, либо
#      NAS с web-доступом); далее esp_audio_codec -> PCM -> I²S;
#    - интернет-радио: те же esp_http_client + esp_audio_codec (Icecast/
#      Shoutcast-потоки — это тоже HTTP).
#
#  Для буферов декодера нужен PSRAM (у P4 он есть): CONFIG_SPIRAM=y.
# ============================================================================

cmake_minimum_required(VERSION 3.16)

# Подключение базовой логики сборки ESP-IDF
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(es9039q2m_audio_player)
