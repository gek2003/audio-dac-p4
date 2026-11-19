/*
 * ============================================================================
 *  i2s_out.c — ESP32-P4 как мастер BCLK/LRCK для ES9039Q2M
 *  ESP-IDF v6.1, новый драйвер I²S (driver/i2s_std.h)
 *
 *  НОТАЦИЯ: legacy-драйвер driver/i2s.h в ESP-IDF v6.0 удалён полностью —
 *  используется только driver/i2s_std.h.
 *
 *  MCLK DAC'а НЕ подаётся с ESP32-P4.
 *
 *    ES9039Q2M тактируется локальным генератором платы NDK NZ2520SDA
 *    49,152 МГц (пин MCLK платы), а P4 подаёт только BCLK, LRCK и DATA —
 *    DAC работает в ASYNC slave (HW19-эквивалент, HW-таблица стр. 17;
 *    в SW-режиме: Reg 1[6] SYNC_MODE = 0).
 *
 *    Требование даташита: 130*FS <= MCLK <= 50 МГц. При MCLK = 49,152 МГц
 *    покрывается весь диапазон FS до 384 кГц — при смене частоты на лету
 *    пересобираются только BCLK/LRCK на P4, DAC ресемплирует на свой
 *    чистый MCLK.
 *
 *    Преимущества: чистота тактирования DAC не зависит от P4-PLL.
 *    Цена: локальный генератор не кратен семейству 44,1 кГц — для
 *    максимального качества 44,1/88,2/176,4 кГц нужен второй генератор
 *    45,1584 МГц (см. обсуждение). В ASYNC DAC ресемплирует сам.
 *
 *  Пины I²S подтверждены схемой: BCK=GPIO46, WS=GPIO47, DATA=GPIO48.
 * ============================================================================
 */

#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_out";

/* MCLK на P4 НЕ подключается: DAC получает MCLK от генератора
 * NDK NZ2520SDA 49,152 МГц на самой DAC-плате.
 * Если вернётесь к варианту А (MCLK от P4) — раскомментируйте строку ниже
 * и верните .mclk в gpio_cfg. */
/* Разводка платы подтверждена схемой пользователя (19.11.2026):
 * BCK = GPIO46 (#35), LRCK/WS = GPIO47 (#37), DATA/SD0 = GPIO48 (#39).
 * MCLK на P4 не подключается — DAC тактируется локальным генератором. */
/* #define I2S_OUT_MCLK_PIN    GPIO_NUM_10 */
#define I2S_OUT_BCLK_PIN    GPIO_NUM_46
#define I2S_OUT_WS_PIN      GPIO_NUM_47
#define I2S_OUT_DOUT_PIN    GPIO_NUM_48

#define I2S_OUT_DEFAULT_FS  44100

typedef struct {
    i2s_chan_handle_t tx;
    uint32_t fs;
    bool running;
    volatile bool paused;     /* пауза: i2s_out_write ждёт, DMA выдет тишину */
} i2s_out_ctx_t;

static i2s_out_ctx_t s_i2s = { .tx = NULL, .fs = 0, .running = false, .paused = false };

esp_err_t i2s_out_init(void)
{
    if (s_i2s.tx != NULL) {
        return ESP_OK;   /* уже инициализирован */
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;             /* тишина при недогрузке DMA вместо шума */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s.tx, NULL), TAG, "new channel");

    /* Слот 32 бит: подаём данные MSB-first — DAC принимает 16/24/32-битные
     * сэмплы в старших разрядах 32-битного слота (стандарт I²S).
     * DoP-поток (dsd_dec.c) идёт тем же форматом: 24-бит слово
     * [marker 0x05/0xFA | DSD_L/R | 0] сдвинуто на 8 влево — маркер
     * оказывается в битах 31..24, как того ждёт DoP-детектор ES9039. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_OUT_DEFAULT_FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            /* mclk = I2S_GPIO_UNUSED: в варианте Б MCLK идёт от локального
             * генератора NDK NZ2520SDA 49,152 МГц, а не от P4 */
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_OUT_BCLK_PIN,
            .ws   = I2S_OUT_WS_PIN,
            .dout = I2S_OUT_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* 64*FS — для 32-битных слотов. В ASYNC-режиме MCLK DAC'а локальный,
     * кратность MCLK/FS не требуется — ресемплирование внутри DAC */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s.tx, &std_cfg), TAG, "init std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s.tx), TAG, "enable");
    s_i2s.fs = I2S_OUT_DEFAULT_FS;
    s_i2s.running = true;
    ESP_LOGI(TAG, "I²S мастер запущен: FS=%lu, слоты 32 бит stereo, BCLK=64*FS "
             "(MCLK — от локального генератора 49,152 МГц)", (unsigned long)s_i2s.fs);
    return ESP_OK;
}

/* Пересборка BCLK/LRCK без пересоздания канала. MCLK не меняется — он всегда
 * 49,152 МГц от локального генератора; DAC ресемплирует сам (ASYNC).
 * Вызывается из decoder.c внутри колбэка es9039_switch_frequency() —
 * DAC в этот момент в mute. */
esp_err_t i2s_out_apply_fs(uint32_t fs)
{
    if (s_i2s.tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fs == s_i2s.fs) {
        return ESP_OK;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(fs);
    /* MCLK-вывод не используется, mclk_multiple не важен */
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_i2s.tx), TAG, "disable");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s.tx, &clk_cfg), TAG, "reconfig clock");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s.tx), TAG, "enable");
    s_i2s.fs = fs;
    ESP_LOGI(TAG, "Частота I²S переключена: FS=%lu (MCLK DAC — локальный 49,152 МГц)",
             (unsigned long)fs);
    return ESP_OK;
}

/* Блокирующая запись PCM в DMA. Если декодер даёт данные быстрее, чем
 * воспроизводятся, — функция сама ждёт свободного места.
 * При паузе (i2s_out_set_paused(true)) запись не идёт: DMA досиграет свои
 * буферы и auto_clear даст тишину; декодер, стоящий в этой точке, ждёт —
 * для HTTP-потоков пауза реализуется через mute DAC, а не здесь. */
esp_err_t i2s_out_write(const void *pcm, size_t bytes)
{
    if (s_i2s.tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    while (s_i2s.paused) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    size_t written = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_write(s_i2s.tx, pcm, bytes, &written, -1), TAG, "write");
    return (written == bytes) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void i2s_out_set_paused(bool paused)
{
    s_i2s.paused = paused;
}

bool i2s_out_paused(void)
{
    return s_i2s.paused;
}

esp_err_t i2s_out_deinit(void)
{
    if (s_i2s.tx == NULL) {
        return ESP_OK;
    }
    if (s_i2s.running) {
        i2s_channel_disable(s_i2s.tx);
        s_i2s.running = false;
    }
    esp_err_t err = i2s_del_channel(s_i2s.tx);
    s_i2s.tx = NULL;
    s_i2s.fs = 0;
    return err;
}

uint32_t i2s_out_current_fs(void)
{
    return s_i2s.fs;
}
