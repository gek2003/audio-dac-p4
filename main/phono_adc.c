/*
 * ============================================================================
 *  phono_adc.c — АЦП-тракт WM8782: приём винила/линии
 *  ESP-IDF 6.1
 *
 *  I²S RX (slave, 192 кГц, 32-битные слот, 24 бита в старших разрядах)
 *  -> конверсия в float32 -> DSP-канал (pipeline_192k.h):
 *     trim -> DC-block 5 Гц -> RIAA FULL -> HPF (OFF/GENTLE/STRICT,
 *     10 мс кроссфейд) -> peak monitor -> конверсия в int32 -> ringbuf.
 *
 *  Обработка в задаче phono_rx (приоритет 10, стек 8 КБ). Блок — 10 мс
 *  (1920 стерео-фреймов), как в пакете v3.
 *
 *  Валидация DSP: константы и рекурсии проверены числовой симуляцией
 *  float32 (dsp_test_py/phonotest.py, повторяет dsp_test.c с двумя
 *  исправлениями — см. README 7г): RIAA FULL 0.002..0.011 дБ от теории,
 *  HPF-переключение без щелчка (max jump 0.0015 FS), DC/NaN — OK.
 * ============================================================================
 */

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"

#include "phono_adc.h"

static const char *TAG = "phono";

/* ---------------------------- Пины и режим -------------------------------- */
/* Пины (по схеме входного буфера Input_Buffer WM8782S, проверить по своей плате!):
 * P4-мастер: MCLK GPIO14 -> модуль, BCLK GPIO15 -> модуль, LRCK GPIO16 -> модуль,
 * DATA GPIO17 <- модуль. В slave-режиме (модуль = мастер) MCLK/BCLK/LRCK — входы,
 * GPIO14 свободен. */
#define PHONO_BCLK_PIN      GPIO_NUM_15     /* <-> модуль BICK  */
#define PHONO_LRCK_PIN      GPIO_NUM_16     /* <-> модуль LRCK  */
#define PHONO_DIN_PIN       GPIO_NUM_17     /* <-  модуль DATA  */
#define PHONO_MCLK_PIN      GPIO_NUM_14     /* ->  модуль MCLK (только MASTER) */

/* Тактирование по умолчанию: P4 = мастер (по схеме: MCLK/BCLK/LRCK от P4,
 * модуль WM8782 = slave). На модуле включить SLAVE-перемычку (чек-лист README
 * пакета п.4: Strap Slave) — иначе модуль-мастер конфликтует по BCLK/LRCK.
 * Для сравнения полов master/slave (README 1, п.4 методики) поставить
 * PHONO_CLK_SLAVE и сравнить idle-шум; оставить лучший. */
#define PHONO_DEFAULT_CLK   PHONO_CLK_MASTER

#define PHONO_FS            192000
#define PHONO_BLOCK_FRAMES  1920            /* 10 мс стерео при 192 кГц */
#define PHONO_TASK_STACK    8192
#define PHONO_TASK_PRIO     10
#define PHONO_STATS_PERIOD  600             /* блоков между логами статуса (6 с) */

/* --------------------------- Состояние модуля ----------------------------- */
static i2s_chan_handle_t s_rx = NULL;
static TaskHandle_t      s_task = NULL;
static RingbufHandle_t   s_ring = NULL;
static volatile bool     s_stop_req = false;
static volatile bool     s_running = false;
static hpf_mode_t        s_hpf_req = HPF_GENTLE;    /* запрос от веб-пульта */
static phono_clk_mode_t  s_clk = PHONO_DEFAULT_CLK;

/* DSP-каналы L/R (pipeline_192k.h) */
static phono_ch_t s_chL;
static phono_ch_t s_chR;

/* Пик-статистика с момента старта */
static uint32_t s_warn = 0;
static uint32_t s_clip = 0;
static float    s_peak = 0.0f;

/* ------------------------------- Задача RX -------------------------------- */

/* int32 (24 бита в старших разрядах 32-битного слота) -> float [-1, 1) */
static inline float s24_to_float(int32_t v) {
    return (float)(v >> 8) * (1.0f / 8388608.0f);
}

/* float [-1, 1) -> int32, 24 бита в старших разрядах (формат i2s_out/DAC) */
static inline int32_t float_to_s24(float x) {
    if (x >= 0.99999f)  return 0x7FFFFF00;      /* +2^23-1 << 8 */
    if (x <= -1.0f)     return (int32_t)0x80000000;
    return ((int32_t)(x * 8388608.0f)) << 8;
}

static void phono_rx_task(void *arg)
{
    (void)arg;

    /* блок: 1920 стерео-фреймов int32 (10 мс) — статически, не в стеке */
    static int32_t in_buf[2 * PHONO_BLOCK_FRAMES];
    static int32_t out_buf[2 * PHONO_BLOCK_FRAMES];

    phono_ch_init(&s_chL, 1.0f, s_hpf_req);
    phono_ch_init(&s_chR, 1.0f, s_hpf_req);

    s_running = true;
    ESP_LOGI(TAG, "АЦП-тракт запущен: %s, %lu Гц, 24 бит/32-слот",
             s_clk == PHONO_CLK_MASTER ? "P4=master (MCLK 49,152 МГц)" : "WM8782=master, P4=slave",
             (unsigned long)PHONO_FS);

    uint32_t stat_t = 0;
    while (!s_stop_req) {
        size_t bytes = 0;
        /* таймаут 200 мс: задача регулярно просыпается и проверяет stop_req */
        if (i2s_channel_read(s_rx, in_buf, sizeof(in_buf), &bytes,
                             pdMS_TO_TICKS(200)) != ESP_OK || bytes == 0) {
            continue;
        }
        int frames = (int)(bytes / (2 * sizeof(int32_t)));
        if (frames <= 0) continue;

        hpf_engine_poll(&s_chL.hpf);
        hpf_engine_poll(&s_chR.hpf);

        for (int i = 0; i < frames; i++) {
            float l = s24_to_float(in_buf[2 * i]);
            float r = s24_to_float(in_buf[2 * i + 1]);
            l = phono_process(&s_chL, l);
            r = phono_process(&s_chR, r);
            out_buf[2 * i]     = float_to_s24(l);
            out_buf[2 * i + 1] = float_to_s24(r);
        }

        /* Пик-статистика: обновляем максимум/счётчики с момента старта */
        s_warn = s_chL.peak.n_warn + s_chR.peak.n_warn;
        s_clip = s_chL.peak.n_clip + s_chR.peak.n_clip;
        float pk = (s_chL.peak.peak > s_chR.peak.peak) ? s_chL.peak.peak : s_chR.peak.peak;
        if (pk > s_peak) s_peak = pk;
        if (++stat_t >= PHONO_STATS_PERIOD && s_clip > 0) {
            ESP_LOGW(TAG, "пик-монитор: предупреждений %lu, клипов %lu, peak %.4f FS",
                     (unsigned long)s_warn, (unsigned long)s_clip, (double)s_peak);
            stat_t = 0;
        }

        /* Живой поток: decoder подтягивает. Если decoder на паузе — ringbuf
         * полон: блокируемся до 1000 мс (на выходе тишина; DMA RX переполнится,
         * WM8782-мастер продолжит — после снятия паузы звук вернётся без рывка,
         * только с пропуском недослушанного) */
        size_t wb = 2 * (size_t)frames * sizeof(int32_t);
        if (xRingbufferSend(s_ring, out_buf, wb, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "ringbuf занят — блок потерян (%u байт)", (unsigned)wb);
        }
    }

    s_running = false;
    s_task = NULL;
    ESP_LOGI(TAG, "АЦП-тракт остановлен");
    vTaskDelete(NULL);
}

/* ------------------------------ I²S RX ------------------------------------ */

static esp_err_t phono_i2s_init(void)
{
    if (s_rx != NULL) {
        return ESP_OK;
    }

    /* Отдельный порт (AUTO — свободный; у i2s_out занят свой). */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,
        (s_clk == PHONO_CLK_SLAVE) ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = PHONO_BLOCK_FRAMES;   /* фрейм = 1 стерео-сэмпл */
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(PHONO_FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = PHONO_BCLK_PIN,                 /* slave: вход; master: выход */
            .ws   = PHONO_LRCK_PIN,                 /* slave: вход; master: выход */
            .dout = I2S_GPIO_UNUSED,
            .din  = PHONO_DIN_PIN,
            /* MCLK нужен только в P4-master: GPIO14 -> модуль, 256*fs = 49,152 МГц
             * от аудио-PLL (README 1: «методика сравнения полов» — мерить на бенче) */
            .mclk = (s_clk == PHONO_CLK_MASTER) ? PHONO_MCLK_PIN : I2S_GPIO_UNUSED,
        },
    };
    /* master: MCLK 256*fs (clk_cfg.mclk_multiple уже 256 по дефолтному макросу) */
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    return ESP_OK;
}

static void phono_i2s_deinit(void)
{
    if (s_rx != NULL) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}

/* ------------------------------ Публичный API ----------------------------- */

esp_err_t phono_adc_start(RingbufHandle_t ring)
{
    if (ring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;   /* уже играет — сначала phono_adc_stop() */
    }
    s_ring = ring;
    s_stop_req = false;
    s_warn = 0; s_clip = 0; s_peak = 0.0f;

    esp_err_t err = phono_i2s_init();
    if (err != ESP_OK) {
        s_ring = NULL;
        return err;
    }

    BaseType_t ok = xTaskCreate(phono_rx_task, "phono_rx", PHONO_TASK_STACK,
                                NULL, PHONO_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        phono_i2s_deinit();
        s_ring = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void phono_adc_stop(void)
{
    if (!s_running && s_task == NULL) {
        phono_i2s_deinit();
        s_ring = NULL;
        return;
    }
    s_stop_req = true;
    int waited = 0;
    while (s_task != NULL && waited < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "задача RX не завершилась за 3 с (ringbuf заблокирован?)");
    }
    phono_i2s_deinit();
    s_ring = NULL;
}

bool phono_adc_running(void)
{
    return s_running;
}

uint32_t phono_adc_fs(void)
{
    return PHONO_FS;
}

void phono_hpf_set(hpf_mode_t m)
{
    if (m > HPF_STRICT) m = HPF_STRICT;
    s_hpf_req = m;
    phono_request_hpf(&s_chL.hpf, m);
    phono_request_hpf(&s_chR.hpf, m);
}

hpf_mode_t phono_hpf_get(void)
{
    return s_hpf_req;
}

void phono_adc_stats(float *peak, uint32_t *n_warn, uint32_t *n_clip)
{
    if (peak)   *peak = s_peak;
    if (n_warn) *n_warn = s_warn;
    if (n_clip) *n_clip = s_clip;
}

const char *phono_adc_info(void)
{
    return (s_clk == PHONO_CLK_MASTER) ? "P4 master 192k" : "WM8782 master 192k";
}
