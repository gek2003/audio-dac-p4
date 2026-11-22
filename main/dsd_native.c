/*
 * ============================================================================
 *  dsd_native.c — Native DSD: два I²S-порта P4 -> ES9039Q2M (DATA1/DATA2/CLK)
 * ============================================================================
 */

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

#include "dsd_native.h"
#include "es9039q2m_esp32p4_driver.h"
#include "i2s_out.h"

static const char *TAG = "dsd_nat";

/* Пины native-DSD тракта. ПРОВЕРИТЬ ПО СХЕМЕ ПЛАТЫ (см. dsd_native.h).
 * GPIO_DSD_DATA1 совпадает со штатным DATA-выходом PCM (GPIO48) — один и тот же
 * физический провод. GPIO_DSD_DATA2/GPIO_DSD_CLK — новые пины (пайка к чипу). */
#define DSD_NATIVE_DOUT1        GPIO_NUM_48   /* DATA1 ES9039 */
#define DSD_NATIVE_DOUT2        GPIO_NUM_49   /* DATA2 ES9039 — требуется провод */
#define DSD_NATIVE_BCLK         GPIO_NUM_46   /* DATA_CLK ES9039 — требуется провод */
#define DSD_NATIVE_WS_DUMMY     GPIO_NUM_47   /* WS: нужен порту B как опора; к чипу не обязателен */

#define DSD_NATIVE_BUF_BYTES    (4096)        /* байт интерлива на вызов write */

static i2s_chan_handle_t s_tx1 = NULL;
static i2s_chan_handle_t s_tx2 = NULL;
static volatile bool     s_running = false;
static volatile bool     s_paused = false;
static uint32_t          s_rate = 0;

/* Раскидка интерлива [L,R,L,R...] в [L,0,L,0...] и [R,0,R,0...]:
 * каждый слот 8 бит; за период WS два слота; ноль в «чужом» слоте держит
 * линию тише, чтобы ES9039 не брал мусор. */
static void deinterleave(const uint8_t *inter, size_t bytes,
                         uint8_t *a_buf, uint8_t *b_buf) {
    size_t frames = bytes / 2;
    for (size_t i = 0; i < frames; i++) {
        a_buf[2 * i]     = inter[2 * i];      /* L */
        a_buf[2 * i + 1] = 0;                 /* правый слот DATA1 — ноль */
        b_buf[2 * i]     = inter[2 * i + 1];  /* R */
        b_buf[2 * i + 1] = 0;                 /* правый слот DATA2 — ноль */
    }
}

esp_err_t dsd_native_start(uint32_t dsd_rate)
{
    if (dsd_rate < 2822400 || dsd_rate % 44100 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;   /* уже играет */
    }

    /* Штатный PCM-порт (i2s_out) молчит: его dout/WS — те же физические
     * пины GPIO48/GPIO47, что и у native-портов. Два DMA на один GPIO —
     * мусор в DATA1/WS. Пауза снимается в dsd_native_stop(). */
    i2s_out_set_paused(true);

    /* Переключение входа ЦАПа: PCM -> native DSD (Reg1=0x82, Reg57 с AUTO) */
    esp_err_t err = es9039_set_input(ES9039_INPUT_DSD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es9039_set_input(DSD): %s", esp_err_to_name(err));
        return err;
    }
    es9039_mute(true);

    uint32_t lrck = dsd_rate / 16;      /* 176.4к для DSD64, 352.8к для DSD128 */
    /* слот 8 бит × 2 слота × LRCK = 16×LRCK = dsd_rate — DSD bit clock */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    err = i2s_new_channel(&chan_cfg, &s_tx1, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(master): %s", esp_err_to_name(err));
        es9039_set_input(ES9039_INPUT_PCM);
        return err;
    }

    i2s_std_config_t cfg1 = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(lrck),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_8BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = DSD_NATIVE_BCLK,
            .ws   = DSD_NATIVE_WS_DUMMY,
            .dout = DSD_NATIVE_DOUT1,
            .din  = I2S_GPIO_UNUSED,
            .mclk = I2S_GPIO_UNUSED,       /* MCLK ЦАПа остаётся от локального генератора */
        },
    };
    err = i2s_channel_init_std_mode(s_tx1, &cfg1);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_tx1);
    if (err != ESP_OK) goto fail;

    /* Порт B: slave TX, клок от порт A (тот же GPIO BCLK/WS через GPIO-matrix). */
    i2s_chan_config_t chan_cfg2 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_SLAVE);
    chan_cfg2.dma_desc_num = 8;
    chan_cfg2.dma_frame_num = 256;
    err = i2s_new_channel(&chan_cfg2, &s_tx2, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel(slave): %s", esp_err_to_name(err));
        goto fail;
    }

    i2s_std_config_t cfg2 = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(lrck),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_8BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = DSD_NATIVE_BCLK,        /* вход: линия порт A */
            .ws   = DSD_NATIVE_WS_DUMMY,    /* вход: линия порт A */
            .dout = DSD_NATIVE_DOUT2,
            .din  = I2S_GPIO_UNUSED,
            .mclk = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(s_tx2, &cfg2);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(s_tx2);
    if (err != ESP_OK) goto fail;

    s_rate = dsd_rate;
    s_running = true;
    ESP_LOGI(TAG, "Native DSD: %lu бит/с, LRCK %lu кГц, слоты 8 бит; "
                  "DATA1=GPIO%u, DATA2=GPIO%u, DATA_CLK=GPIO%u",
             (unsigned long)dsd_rate, (unsigned long)(lrck / 1000),
             (unsigned)DSD_NATIVE_DOUT1, (unsigned)DSD_NATIVE_DOUT2, (unsigned)DSD_NATIVE_BCLK);
    return ESP_OK;

fail:
    if (s_tx1 != NULL) { i2s_channel_disable(s_tx1); i2s_del_channel(s_tx1); s_tx1 = NULL; }
    if (s_tx2 != NULL) { i2s_channel_disable(s_tx2); i2s_del_channel(s_tx2); s_tx2 = NULL; }
    es9039_set_input(ES9039_INPUT_PCM);
    es9039_mute(false);
    i2s_out_set_paused(false);            /* откат: штатный PCM-порт снова жив */
    return err;
}

void dsd_native_stop(void)
{
    if (!s_running && s_tx1 == NULL && s_tx2 == NULL) return;
    if (s_tx1 != NULL) { i2s_channel_disable(s_tx1); i2s_del_channel(s_tx1); s_tx1 = NULL; }
    if (s_tx2 != NULL) { i2s_channel_disable(s_tx2); i2s_del_channel(s_tx2); s_tx2 = NULL; }
    es9039_set_input(ES9039_INPUT_PCM);   /* вернуть PCM-вход */
    es9039_mute(false);
    i2s_out_set_paused(false);            /* вернуть штатный PCM-порт */
    s_running = false;
    s_paused = false;
    s_rate = 0;
    ESP_LOGI(TAG, "Native DSD остановлен");
}

bool dsd_native_running(void) { return s_running; }
bool dsd_native_paused(void) { return s_paused; }
uint32_t dsd_native_rate(void) { return s_rate; }

/* Пауза native-DSD: останавливаем клок (DATA_CLK падает) — ES9039 по
 * fault-detect (Reg57[6]) сам ставит DSD mute pattern 0x96. При снятии —
 * клок возвращается, Reg1/57 переписываются (гарантированный вход). */
void dsd_native_pause(bool p)
{
    if (!s_running) return;
    if (p == s_paused) return;
    if (p) {
        if (s_tx1) i2s_channel_disable(s_tx1);
        if (s_tx2) i2s_channel_disable(s_tx2);
    } else {
        if (s_tx1) i2s_channel_enable(s_tx1);
        if (s_tx2) i2s_channel_enable(s_tx2);
        es9039_set_input(ES9039_INPUT_DSD);
    }
    s_paused = p;
}

esp_err_t dsd_native_write(const uint8_t *inter, size_t bytes)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if ((bytes & 1) || bytes == 0) return ESP_ERR_INVALID_ARG;

    /* статические буферы: 2 × DSD_NATIVE_BUF_BYTES (внутренняя RAM) */
    static uint8_t a_buf[DSD_NATIVE_BUF_BYTES];
    static uint8_t b_buf[DSD_NATIVE_BUF_BYTES];

    size_t off = 0;
    while (off < bytes) {
        size_t chunk = bytes - off;
        if (chunk > DSD_NATIVE_BUF_BYTES / 2) chunk = DSD_NATIVE_BUF_BYTES / 2;
        deinterleave(inter + off, chunk, a_buf, b_buf);
        esp_err_t e1 = i2s_channel_write(s_tx1, a_buf, chunk, NULL, portMAX_DELAY);
        esp_err_t e2 = i2s_channel_write(s_tx2, b_buf, chunk, NULL, portMAX_DELAY);
        if (e1 != ESP_OK || e2 != ESP_OK) {
            ESP_LOGW(TAG, "write: %s / %s", esp_err_to_name(e1), esp_err_to_name(e2));
            return (e1 != ESP_OK) ? e1 : e2;
        }
        off += chunk;
    }
    return ESP_OK;
}
