/*
 * ============================================================================
 *  phono_adc.h — АЦП-тракт WM8782 (винил/линия): I²S RX + DSP (pipeline_192k)
 *  ESP-IDF 6.1
 *
 *  Поток: I²S RX (192 кГц, 24 бит в 32-битных слотах) -> phono DSP
 *  (trim -> DC-block -> RIAA FULL -> HPF OFF/GENTLE/STRICT с кроссфейдом ->
 *  peak monitor) -> ringbuf -> decoder.c (passthrough adc://) -> i2s_out.
 *
 *  Тактирование (два варианта, см. README раздел 7г):
 *    - PHONO_CLK_MASTER (по умолчанию): P4 = мастер, подаёт MCLK (256·fs =
 *      49,152 МГц при 192 кГц, от внутреннего аудио-PLL — ПРОТЕСТИРОВАТЬ
 *      на бенче), BCLK/LRCK на модуль GPIO14/15/16 по схеме; модуль WM8782
 *      = slave (SLAVE-перемычка на модуле).
 *    - PHONO_CLK_SLAVE: модуль WM8782 = мастер (локальный кварц 24,576 МГц,
 *      FSAMPEN=NC -> 192 кГц), P4 = RX slave. Альтернатива по методике
 *      сравнения полов (README 1, п.4): сравнить idle-шум, оставить лучший.
 *  Частота фиксирована 192 кГц в обоих вариантах — DSP-константы
 *  pipeline_192k.h рассчитаны только на неё.
 *
 *  Пины (по схеме Input_Buffer WM8782S, проверить по своей плате!):
 *    BCLK GPIO15, LRCK GPIO16, DATA (DIN) GPIO17, MCLK GPIO14 (только master).
 *  Каналы WM8782 совпадают с основным трактом DAC (GPIO46/47/48) — не пересекаются.
 * ============================================================================
 */

#pragma once

#include "esp_err.h"
#include "freertos/ringbuf.h"
#include "pipeline_192k.h"

/* Режим тактирования тракта АЦП */
typedef enum {
    PHONO_CLK_SLAVE  = 0,    /* WM8782 = мастер (кварц 24,576), P4 = RX slave */
    PHONO_CLK_MASTER = 1,    /* P4 = мастер (MCLK 256*fs на GPIO14), WM8782 = slave */
} phono_clk_mode_t;

/* Запуск приёма и DSP-обработки. ring — кольцевой буфер декодера
 * (создаётся decoder_start() для adc://). Данные в ring: int32-фреймы
 * L,R (24 бита в старших разрядах), 192 000 Гц. */
esp_err_t phono_adc_start(RingbufHandle_t ring);

/* Остановка приёма (закрывает I²S RX, глушит задачу, ждёт её завершения).
 * Вызывается из decoder_stop() до удаления ringbuf. */
void      phono_adc_stop(void);

bool      phono_adc_running(void);

/* Частота входа (константа 192000) — decoder.c ставит её на DAC при старте. */
uint32_t  phono_adc_fs(void);

/* Сабсонический HPF: OFF / GENTLE / STRICT. Переключение — с 10 мс
 * кроссфейдом, применяется на границе блока. Оба канала сразу. */
void      phono_hpf_set(hpf_mode_t m);
hpf_mode_t phono_hpf_get(void);

/* Пик-статистика (для диагностики; peak — 0..1.0 FS). */
void      phono_adc_stats(float *peak, uint32_t *n_warn, uint32_t *n_clip);

/* Краткое описание режима для логов: "WM8782 master 192k" / "P4 master 192k". */
const char *phono_adc_info(void);
