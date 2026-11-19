/*
 * sd_card.c — инициализация microSD (SDMMC2 4-bit) без монтирования FatFS.
 * Пины и питание — по схеме платы Waveshare ESP32-P4-WIFI6-Touch-LCD-5,
 * сверено с examples/esp-idf/05_sdmmc и с test-app esp_tinyusb (msc_storage):
 * SDMMC_HOST_DEFAULT, 4 линии, GPIO-матрица, LDO канала 4, INTERNAL_PULLUP.
 * Монтирование FatFS делает TinyUSB MSC (usb_msc.c) — так карта может
 * «переезжать» между приложением и ПК без переподключения шины.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "sd_card.h"

static const char *TAG = "sd_card";

#define SD_SLOT_CLK           GPIO_NUM_43
#define SD_SLOT_CMD           GPIO_NUM_44
#define SD_SLOT_D0            GPIO_NUM_39
#define SD_SLOT_D1            GPIO_NUM_40
#define SD_SLOT_D2            GPIO_NUM_41
#define SD_SLOT_D3            GPIO_NUM_42
#define SD_PWR_LDO_CHANNEL    4        /* LDO_VO4 питает SD-слот */

static sdmmc_card_t *s_card;
static bool s_ready;
static sd_pwr_ctrl_handle_t s_pwr;

esp_err_t sd_card_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();   /* SDMMC_FREQ_DEFAULT = 20 МГц */

    /* SD-слот питается от внутреннего LDO P4 (канал 4) — по схеме платы. */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_PWR_LDO_CHANNEL };
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo: %s", esp_err_to_name(ret));
        return ret;
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    /* SOC_SDMMC_USE_GPIO_MATRIX: произвольные GPIO на шину SDMMC */
    slot.clk = SD_SLOT_CLK;
    slot.cmd = SD_SLOT_CMD;
    slot.d0  = SD_SLOT_D0;
    slot.d1  = SD_SLOT_D1;
    slot.d2  = SD_SLOT_D2;
    slot.d3  = SD_SLOT_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    s_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (s_card == NULL) {
        ESP_LOGE(TAG, "Нет памяти под sdmmc_card_t");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Инициализация SD-карты (4-bit, до 20 МГц)...");
    ret = sdmmc_host_init();
    if (ret == ESP_OK) ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret == ESP_OK) ret = sdmmc_card_init(&host, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD-карта не отвечает (%s): нет карты или слота",
                 esp_err_to_name(ret));
        sdmmc_host_deinit();
        free(s_card);
        s_card = NULL;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    s_ready = true;
    return ESP_OK;
}

bool sd_card_ready(void)
{
    return s_ready;
}

sdmmc_card_t *sd_card_get_card(void)
{
    return s_card;
}

/* --- Поиск первого аудиофайла (работает при смонтированной FatFS) -------- */

static bool is_audio_name(const char *name)
{
    static const char *const exts[] = { ".mp3", ".flac", ".wav", ".dsf", ".dff" };
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len >= el && strncasecmp(name + len - el, exts[i], el) == 0) return true;
    }
    return false;
}

static bool find_audio_dfs(const char *dir, int depth, char *out, size_t cap)
{
    if (depth > 3) return false;
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && !found) {
        if (ent->d_name[0] == '.') continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            found = find_audio_dfs(child, depth + 1, out, cap);
        } else if (is_audio_name(ent->d_name)) {
            snprintf(out, cap, "%s", child);
            found = true;
        }
    }
    closedir(d);
    return found;
}

bool sd_card_find_first_audio(const char *root, char *out, size_t cap)
{
    if (!s_ready || !root || !out) return false;
    return find_audio_dfs(root, 0, out, cap);
}
