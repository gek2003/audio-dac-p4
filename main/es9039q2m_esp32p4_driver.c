/*
 * ============================================================================
 *  ES9039Q2M — драйвер управления по I²C для ESP32-P4 (ESP-IDF v6.1)
 *  Платформа: плата Arvin DAC ES9039Q2M + OPA1612 (HW/SW-режим, перепайка в SW)
 * ============================================================================
 *
 *  Целевая среда: ESP-IDF v6.1 (проверено по migration guide v6.0/v6.1):
 *    - новый I²C-драйвер (driver/i2c_master.h) — legacy driver/i2c.h в v6
 *      end-of-life и даёт warning при сборке;
 *    - legacy I²S-драйвер driver/i2s.h в v6.0 УДАЛЁН — мастер I²S на ESP32-P4
 *      должен быть собран на driver/i2s_std.h;
 *    - при NACK транзакция возвращает ESP_ERR_INVALID_RESPONSE — скан шины
 *      использует штатный i2c_master_probe(), различающий NOT_FOUND/TIMEOUT;
 *    - предупреждения = ошибки по умолчанию, код написан без них;
 *    - в main/CMakeLists.txt нужны REQUIRES: esp_driver_i2c esp_driver_gpio
 *      esp_driver_i2s (компонент driver больше не тянет их по умолчанию).
 *
 *  Все адреса/биты сверены с ES9039Q2M Product Datasheet v0.2.3 (ESS Technology).
 *  Ссылки на страницы даны в комментариях к каждому шагу.
 *
 *  Аппаратные условия:
 *    - Плата перепаяна в SW-режим: сняты R2/R3 (подтяжки SDA/HW0, SCL/HW1),
 *      перемычка HW/SW замкнута на ADDR1=ADDR0=GND, соединение HW/SW с
 *      обратной стороны разорвано (инструкция продавца платы).
 *    - MODE (pin 29) = 0  ->  интерфейс I²C  (Datasheet, стр. 13, Table 2)
 *    - ADDR0=ADDR1=GND    ->  I²C-адрес 0x90 (8-бит) = 0x48 (7-бит)
 *                            (Datasheet, стр. 13, Table 3)
 *  Дополнительные сведения:
 *    - Режим тактирования: ASYNC slave (Reg 1[6] SYNC_MODE = 0) — вариант Б:
 *      MCLK DAC'а берётся с локального генератора платы NDK NZ2520SDA
 *      (49,152 МГц), ESP32-P4 подаёт только BCLK/LRCK/DATA.
 *      Требование (HW-таблица, стр. 17, HW19): 130·FS ≤ MCLK ≤ 50 МГц —
 *      49,152 МГц покрывает все FS до 384 кГц.
 *    - Питание платы: 5 В цифровое, ±12 В (или ±15 В) аналоговое.
 *    - Кварц X1 на плате (генератор NDK NZ2520SDA, 49,152 МГц) — ИСТОЧНИК
 *      MCLK DAC'а (вариант Б). У ES9039Q2M нет ASRC — чистота MCLK
 *      критична.
 *
 *  Функции этого модуля:
 *    - es9039_init()              — полная последовательность инициализации
 *                                   (soft reset -> тракт -> тактирование ->
 *                                   вход -> фильтр -> громкость -> unmute)
 *    - es9039_set_volume_db()     — громкость 0..-127.5 дБ, шаг 0.5 дБ
 *                                   (Reg 74/75, стр. 82)
 *    - es9039_set_filter()        — 8 пресетов FIR (Reg 88, стр. 33 и 84)
 *    - es9039_mute()/unmute()     — программный mute (Reg 86, стр. 84)
 *    - es9039_set_input()         — PCM / DSD / DoP (Reg 1 + Reg 57, стр. 61, 78)
 *    - es9039_automute_status()   — чтение флага automute:
 *        путь A: пин ESP32 <- GPIO1 платы (запекается через ESP-IDF gpio)
 *        путь B: readback по I²C (Reg 45 + Reg 240, стр. 74 и карта рег.)
 *      GPIO1 по умолчанию выдаёт AND(automute CH1, CH2)
 *      (Reg 37 def 4'd13, Reg 46/47 bit1 GPIO_AND_VOL_MIN=1; стр. 68, 75)
    - es9039_src_locked()        — SRC lock: Reg 239[7] IDAC_DIV_RATIO_VALID
 *                                   (в HW-таблице дублируется GPIO2, стр. 14)
 *    - es9039_switch_frequency()  — переключение частоты «на лету» с
 *                                   mute-защитой и ожиданием SRC lock
 *    - FIR-интеграция (выполнена): публичные es9039_write_reg/read_reg для
 *      модуля es9039_fir.c; es9039_switch_frequency() заливает FIR-набор под
 *      новую частоту между пересборкой I²S и ожиданием SRC lock (выход в
 *      mute, поэтому заливка не слышна). Наборы: C-BRICK/C-SLOW × 44.1/48/96
 *      (es9039_fir_coeff.c); при отсутствии набора — встроенный пресет Reg 88.
 *
 *  Диагностика и компенсация гармоник (по мотивам практики интеграции
 *  ES9039 на DRIVE2, vladikb: https://www.drive2.ru/l/744522205194385067/):
 *    - es9039_diag_read()         — снимок состояния DAC: ratio_valid («SRC
 *                                   lock», Reg 239[7]), валидность потока
 *                                   (S/PDIF/TDM/DoP, Reg 245), активный вход,
 *                                   BCK/WS fail, automute, vol_min + оценка
 *                                   частоты дискретизации из IDAC_DIV_AUTO
 *    - es9039_set_dpll_bw()       — окно DPLL 1..15 (Reg 29[7:4], стр. 66):
 *                                   узкое окно = меньше джиттера, но выше
 *                                   требования к источнику; дефолт 4
 *    - es9039_clear_interrupts()  — сброс interrupt-флагов (Reg 20/21, стр. 66)
 *    - es9039_set_thd_comp()      — компенсация гармоник C2/C3 по каналам
 *                                   (Reg 91-94 / 107-110, стр. 86, 16-бит
 *                                   signed, output = x + c2·x² + c3·x³);
 *                                   подбирается по замеру на реальном тракте
 *
 *  Обязательные элементы инициализации (последовательность, стр. даташита):
 *    1. Reg 0 = 0x80              SOFT_RESET                  (стр. 60)
 *    2. пауза 10 мс
 *    3. Reg 0 = 0x02              DAC_MODE=1                  (стр. 60)
 *    4. Reg 1 = 0x80              ASYNC slave, только PCM      (стр. 61)
 *                                  (bit6 SYNC_MODE=0: MCLK от локального
 *                                   генератора, ресемплирование на DAC)
 *    5. Reg 3 = 0x80              AUTO_FS_DETECT=1            (стр. 62)
 *    6. Reg 5 = 0x04              AUTO_CLK_GEAR=1             (стр. 63)
 *    7. Reg 57 = 0x41             AUTO_INPUT_SEL, PCM slave   (стр. 78)
 *    8. Reg 88 = 0x07             фильтр №7 min-phase slow,   (стр. 33, 84)
 *                                 low dispersion
 *    9. Reg 74/75 = 0x50          громкость −40 дБ            (стр. 82)
 *   10. Reg 86 = 0x00             unmute                      (стр. 84)
 *   11. Reg 239 (0xEF) read       бит 7 = IDAC_DIV_RATIO_VALID —
 *                                 статус «SRC lock»         (стр. 92)
 *                                 NB: в карте v0.2.3 Reg 249 (0xF9) помечен
 *                                 как RESERVED — readback статуса именно
 *                                 здесь, а не в 0xF9.
 *
 *  Automute (Reg 123-129, стр. 87-88) оставлен с дефолтами:
 *    Reg 123 = 0x03 (включён по обоим каналам), порог −119/−117.5 дБ.
 * ============================================================================
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "es9039_fir.h"
#include "es9039q2m_esp32p4_driver.h"

static const char *TAG = "es9039q2m";

/* ------------------------- Адресация и пины ------------------------------ */


/* Пин ESP32-P4, подключённый к GPIO1 платы (Automute Status).
 * Если GPIO1 платы никуда не выведен — используйте только путь B (readback). */
#define ES9039_GPIO1_AUTOMUTE_PIN   GPIO_NUM_4

/* ------------------------------ Регистры ---------------------------------- */

/* System registers (стр. 60-63) */
#define R00_SYSTEM_CONFIG        0x00  /* [7]=SOFT_RESET [6]=EN_64FS [1]=DAC_MODE */
#define R01_SYS_MODE_CONFIG      0x01  /* [7]=EN_DAC_CLK [6]=SYNC_MODE [3]=SPDIF [2]=DoP [1]=DSD [0]=TDM */
#define R03_DAC_CLOCK_CONFIG     0x03  /* [7]=AUTO_FS_DETECT [5:0]=SELECT_IDAC_NUM */
#define R05_CLK_GEAR_SELECT      0x05  /* [2]=AUTO_CLK_GEAR */
#define R29_DPLL_BW              0x1D  /* [7:4]=DPLL_BW (S/PDIF ASYNC; в sync-slave не трогаем) */

/* Input selection (стр. 78-79) */
#define R57_INPUT_SELECTION      0x57  /* [7]=AUTO_CH_DETECT [4]=PCM_MASTER [2:1]=INPUT_SEL [0]=AUTO_INPUT_SEL */
#    define INPUT_SEL_PCM        0x0
#    define INPUT_SEL_DSD        0x1
#    define INPUT_SEL_DOP        0x2
#    define INPUT_SEL_SPDIF      0x3

/* Volume (стр. 82-83) */
#define R74_VOLUME_CH1           0x4A  /* 0x00=0 dB ... 0xFF=-127.5 dB, шаг 0.5 дБ */
#define R75_VOLUME_CH2           0x4B
#define R82_DAC_VOL_UP_RATE      0x52  /* дефолт 0x04 */
#define R83_DAC_VOL_DOWN_RATE    0x53  /* дефолт 0x04 */

/* DAC output (стр. 84-86) */
#define R86_DAC_MUTE             0x56  /* [1]=MUTE_CH2 [0]=MUTE_CH1 */
#define R87_DAC_INVERT           0x57
#define R88_FILTER_SHAPE         0x58  /* [2:0] — 8 пресетов FIR */

/* Automute (стр. 87-88) */
#define R123_AUTOMUTE_ENABLE     0x7B  /* [1]=EN_CH2 [0]=EN_CH1, дефолт 0x03 */
#define R124_AUTOMUTE_TIME       0x7C  /* [11]=MUTE_RAMP_TO_GROUND, [10:0]=TIME */
#define R126_AUTOMUTE_LEVEL      0x7E  /* 16'h0008 = -119 dB (дефолт) */
#define R128_AUTOMUTE_OFF_LEVEL  0x80  /* 16'h000A = -117.5 dB (дефолт) */

/* GPIO (стр. 68-75) */
#define R37_GPIO12_CONFIG        0x25  /* [7:4]=GPIO2_CFG [3:0]=GPIO1_CFG; def 0x7D */
#    define GPIO_CFG_CLOCKS             3   /* 4'd3  */
#    define GPIO_CFG_MUTE_INPUT         5   /* 4'd5  */
#    define GPIO_CFG_SYSTEM_MODE        6   /* 4'd6  */
#    define GPIO_CFG_SRC_LOCK           7   /* 4'd7  */
#    define GPIO_CFG_VOL_MIN            12  /* 4'd12 */
#    define GPIO_CFG_AUTOMUTE_STATUS    13  /* 4'd13: GPIO1 дефолт, AND обоих каналов */
#    define GPIO_CFG_SOFT_RAMP_DONE     14  /* 4'd14 */
#define R41_GPIO_OUTPUT_ENABLE   0x29  /* [1]=GPIO2_OE [0]=GPIO1_OE; дефолт 0x03 */
#define R45_GPIO_READ            0x2D  /* [1]=GPIO2_READ [0]=GPIO1_READ: разрешить readback */
#define R240_GPIO_READBACK       0xF0  /* [1]=GPIO2_I [0]=GPIO1_I после Reg 45 */

/* Readback и диагностика (карта регистров, стр. 59; описания, стр. 92-94)
 * NB: в карте v0.2.3 Reg 249 (0xF9) помечен как RESERVED — статус «SRC lock»
 * читается из Reg 239 (AUTO FS READ), а не из 0xF9. */
#define R00_CHIP_ID              0xE1  /* [7:0]=CHIP_ID (Reg 225) */
#define R229_INT_STATES          0xE5  /* [7]=BCK_WS_FAIL [6]=DOP_VALID [5:4]=SS_FULL_RAMP [3:2]=AUTOMUTE [1:0]=VOL_MIN */
#define R230_INT_STATES2         0xE6  /* [7:6]=INPUT_SELECT_OVERRIDE [5]=TDM_DATA_VALID */
#define R239_AUTO_FS_READ        0xEF  /* [7]=IDAC_DIV_RATIO_VALID [6]=IDAC_HALF_DIV_AUTO [5:0]=IDAC_DIV_AUTO */
#define R241_VOL_MIN_READ        0xF1  /* [1]=CH2 [0]=CH1 */
#define R242_AUTOMUTE_READ       0xF2  /* [1]=CH2 [0]=CH1 */
#define R243_SOFT_RAMP_UP_READ   0xF3  /* [1]=CH2 [0]=CH1 */
#define R245_INPUT_STREAM_RB     0xF5  /* [4]=SPDIF_VALID [3]=TDM_DATA_VALID [2]=DOP_VALID [1:0]=INPUT_SELECT_OVERRIDE */
#define R020_INTERRUPT_CLEAR     0x14  /* сброс флагов: toggle high-low (стр. 66) */
#define R021_INTERRUPT_CLEAR2    0x15  /* INPUT_SELECT_OVERRIDE / TDM_VALID_EDGE */

/* Компенсация гармоник THD (стр. 86): 16-бит signed на канал,
 * output = x + c2·x² + c3·x³. Порядок байтов (Register 94-91:
 * [15:0]=CH1, [31:16]=CH2): Reg 91 = CH1[7:0] (LSB), Reg 92 = CH1[15:8] (MSB).
 * Если по замеру коэффициент не вступает в силу — попробовать обратный порядок. */
#define R91_THD_C2_CH1_LSB       0x5B
#define R92_THD_C2_CH1_MSB       0x5C
#define R93_THD_C2_CH2_LSB       0x5D
#define R94_THD_C2_CH2_MSB       0x5E
#define R107_THD_C3_CH1_LSB      0x6B
#define R108_THD_C3_CH1_MSB      0x6C
#define R109_THD_C3_CH2_LSB      0x6D
#define R110_THD_C3_CH2_MSB      0x6E

/* ---------------------- Фильтры (стр. 33, Table 17) ---------------------- */

/* 8 пресетов FIR: enum es9039_fir_t перенесён в es9039q2m_esp32p4_driver.h */

/* ------------------------------- Вход ------------------------------------- */

/* enum es9039_input_t перенесён в es9039q2m_esp32p4_driver.h */

/* --------------------------- Контекст драйвера ---------------------------- */

typedef struct {
    i2c_master_dev_handle_t i2c;      /* уже настроенный дескриптор устройства */
    uint8_t                 addr;     /* 0x48 */
    int                     gpio1_pin;/* пин ESP32 на GPIO1 платы; -1 = не подключён */
    bool                    mute_on;  /* текущее состояние программного mute */
} es9039_ctx_t;

static es9039_ctx_t s_es = { .addr = ES9039_I2C_ADDR_7BIT, .gpio1_pin = -1, .mute_on = false };

/* ----------------------------- Низкий уровень ----------------------------- */

static esp_err_t es9039_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_es.i2c, buf, sizeof(buf), -1);
}

static esp_err_t es9039_read(uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_master_transmit(s_es.i2c, &reg, 1, -1);
    if (err != ESP_OK) return err;
    return i2c_master_receive(s_es.i2c, val, 1, -1);
}

/* ---- Публичные обёртки для FIR-модуля (es9039_fir.c) --------------------
 * es9039_fir.c адресует чип через эти имена (макросы в es9039_fir.h);
 * его слабые заглушки замещаются этими сильными символами при линковке. */
esp_err_t es9039_write_reg(uint8_t reg, uint8_t val) { return es9039_write(reg, val); }
esp_err_t es9039_read_reg(uint8_t reg, uint8_t *val)  { return es9039_read(reg, val); }

/* Полезная утилита при первом запуске: перебрать адреса 0x40..0x4F и убедиться,
 * что плата действительно перепаяна в SW-режим.
 * v6: при NACK i2c_master_transmit возвращает ESP_ERR_INVALID_RESPONSE, поэтому
 * для перебора используем штатный i2c_master_probe() — он различает
 * ESP_ERR_NOT_FOUND (NACK/нет устройства) и ESP_ERR_TIMEOUT (шина зависла). */
esp_err_t es9039_scan_bus(i2c_master_bus_handle_t bus)
{
    for (uint8_t a = 0x40; a <= 0x4F; a++) {
        esp_err_t probe = i2c_master_probe(bus, a, 50);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "I²C отвечает на 0x%02X (7-bit) — ожидаемый адрес 0x%02X", a, s_es.addr);
            if (a == s_es.addr) return ESP_OK;
        } else if (probe == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "0x%02X: таймаут шины (проверьте подтяжки SDA/SCL)", a);
        }
    }
    ESP_LOGE(TAG, "Адрес 0x%02X не отвечает — проверьте перепайку HW/SW", s_es.addr);
    return ESP_ERR_NOT_FOUND;
}

/* --------------------------- Инициализация -------------------------------- */

esp_err_t es9039_init(i2c_master_dev_handle_t i2c, int gpio1_pin)
{
    s_es.i2c = i2c;
    s_es.gpio1_pin = gpio1_pin;
    s_es.mute_on = false;
    vTaskDelay(pdMS_TO_TICKS(100));          /* стабилизация питания после CHIP_EN */

    /* 1-2. SOFT_RESET + пауза */
    ESP_RETURN_ON_ERROR(es9039_write(R00_SYSTEM_CONFIG, 0x80), TAG, "soft reset");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 3. DAC_MODE=1, 64FS mode выключен */
    ESP_RETURN_ON_ERROR(es9039_write(R00_SYSTEM_CONFIG, 0x02), TAG, "dac mode");

    /* 4. ASYNC slave, только PCM (SYNC_MODE=0 — тактирование от локального
     *    генератора 49,152 МГц; SPDIF/DoP/DSD/TDM-декоды выключены) */
    ESP_RETURN_ON_ERROR(es9039_write(R01_SYS_MODE_CONFIG, 0x80), TAG, "sys mode");

    /* 5-6. AUTO_FS_DETECT + AUTO_CLK_GEAR */
    ESP_RETURN_ON_ERROR(es9039_write(R03_DAC_CLOCK_CONFIG, 0x80), TAG, "auto fs");
    ESP_RETURN_ON_ERROR(es9039_write(R05_CLK_GEAR_SELECT, 0x04), TAG, "auto gear");

    /* 7. AUTO_INPUT_SEL=1, PCM slave; DSD fault detection остаётся (дефолт bit6) */
    ESP_RETURN_ON_ERROR(es9039_write(R57_INPUT_SELECTION, 0x41), TAG, "input sel");

    /* 8. Фильтр №7 (min-phase slow roll-off, low dispersion) */
    ESP_RETURN_ON_ERROR(es9039_write(R88_FILTER_SHAPE, ES9039_FIR_MP_SLOW_LOW_DISP), TAG, "fir");

    /* 9. Громкость −40 дБ (безопасный старт) */
    ESP_RETURN_ON_ERROR(es9039_write(R74_VOLUME_CH1, 0x50), TAG, "vol ch1");
    ESP_RETURN_ON_ERROR(es9039_write(R75_VOLUME_CH2, 0x50), TAG, "vol ch2");

    /* 10. Unmute */
    ESP_RETURN_ON_ERROR(es9039_write(R86_DAC_MUTE, 0x00), TAG, "unmute");

    /* 11. Проверка SRC lock — Reg 239[7] IDAC_DIV_RATIO_VALID (стр. 92) */
    uint8_t st = 0;
    if (es9039_read(R239_AUTO_FS_READ, &st) == ESP_OK) {
        ESP_LOGI(TAG, "init ok, Reg239=0x%02X (ratio_valid=%d)", st, (st >> 7) & 1);
    } else {
        ESP_LOGW(TAG, "init ok, но readback Reg239 не отвечает — проверьте шину");
    }

    /* 12. Путь A: настроить GPIO1 платы как вход ESP32 с прерыванием.
     *     GPIO1_CFG уже 4'd13 (Automute status) по дефолту, GPIO1_OE=1 —
     *     ничего дописывать в DAC не требуется. */
    if (gpio1_pin >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << gpio1_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* флаг в 1 = automute активен; полярность уточнить по осциллографу */
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
    }

    /* 13. Путь B: разрешить readback GPIO1/GPIO2 через I²C (Reg 45) */
    ESP_RETURN_ON_ERROR(es9039_write(R45_GPIO_READ, 0x03), TAG, "gpio readback");

    return ESP_OK;
}

/* ------------------------------ Громкость --------------------------------- */

/* dB = отрицательное число (например -40.0), ограничение 0..-127.5 */
esp_err_t es9039_set_volume_db(float db)
{
    if (db > 0.0f) db = 0.0f;
    if (db < -127.5f) db = -127.5f;
    uint8_t val = (uint8_t)(-db / 0.5f + 0.5f);   /* 0x00 = 0 дБ */
    ESP_RETURN_ON_ERROR(es9039_write(R74_VOLUME_CH1, val), TAG, "vol1");
    ESP_RETURN_ON_ERROR(es9039_write(R75_VOLUME_CH2, val), TAG, "vol2");
    return ESP_OK;
}

/* ------------------------------ Фильтры ----------------------------------- */

esp_err_t es9039_set_filter(es9039_fir_t f)
{
    return es9039_write(R88_FILTER_SHAPE, (uint8_t)(f & 0x07));
}

/* ------------------------------ Mute --------------------------------------- */

esp_err_t es9039_mute(bool on)
{
    s_es.mute_on = on;
    return es9039_write(R86_DAC_MUTE, on ? 0x03 : 0x00);
}

/* ------------------------------ Вход --------------------------------------- */

/* PCM: reg1=0x80 (ASYNC slave: SYNC_MODE=0, MCLK от генератора платы),
 * reg57=0x41 (AUTO_INPUT_SEL + PCM slave)
 * DSD: reg1: SYNC_MODE=0 (async), ENABLE_DSD_DECODE=1 -> 0x82
 *      reg57: DSD slave (def), AUTO_INPUT_SEL=1, ENABLE_DSD_FAULT_DET=1 -> 0x41
 * DoP: reg1=0x84 (ENABLE_DOP_DECODE), reg57=0x41
 * Для DSD/DoP на плате нужна единая шина DATA1/DATA2 (AUTO_INPUT_SEL требует
 * DSD-линии на DATA1/DATA2, стр. 78, Note). */
esp_err_t es9039_set_input(es9039_input_t in)
{
    uint8_t r1, r57;
    switch (in) {
        case ES9039_INPUT_PCM: r1 = 0x80; r57 = 0x41; break;
        case ES9039_INPUT_DSD: r1 = 0x82; r57 = 0x41; break;
        case ES9039_INPUT_DOP: r1 = 0x84; r57 = 0x41; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    /* При смене формата глушим выход, переконфигурируем, ждём SRC lock, открываем */
    es9039_mute(true);
    ESP_RETURN_ON_ERROR(es9039_write(R01_SYS_MODE_CONFIG, r1), TAG, "sys mode");
    ESP_RETURN_ON_ERROR(es9039_write(R57_INPUT_SELECTION, r57), TAG, "input sel");
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t st = 0;
    if (es9039_read(R239_AUTO_FS_READ, &st) == ESP_OK && (st & 0x80) == 0) {
        ESP_LOGW(TAG, "SRC не залочился после смены входа (Reg239=0x%02X)", st);
    }
    es9039_mute(false);
    return ESP_OK;
}

/* --------------------- Automute status через GPIO1 ------------------------ */

/* Путь A: пин ESP32, подключённый к GPIO1 платы (запекается через ESP-IDF gpio) */
bool es9039_automute_status_pin(void)
{
    if (s_es.gpio1_pin < 0) return false;
    /* GPIO1_CFG = 4'd13 (Automute status, AND обоих каналов по умолчанию через
     * GPIO_AND_VOL_MIN reg46/47[1]). Возврат: true = DAC находится в automute. */
    return gpio_get_level(s_es.gpio1_pin) != 0;
}

/* Путь B: readback через I²C (не требует физического доступа к GPIO1).
 * Включён в es9039_init() записью Reg45 = 0x03. */
esp_err_t es9039_automute_status_i2c(bool *muted_ch1, bool *muted_ch2)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(es9039_read(R240_GPIO_READBACK, &v), TAG, "gpio rb");
    if (muted_ch1) *muted_ch1 = (v & 0x01) != 0;
    if (muted_ch2) *muted_ch2 = (v & 0x02) != 0;
    return ESP_OK;
}

/* SRC lock = Reg 239[7] IDAC_DIV_RATIO_VALID (стр. 92; дублируется GPIO2, стр. 14).
 * NB: в карте v0.2.3 Reg 249 (0xF9) — RESERVED, readback именно здесь. */
esp_err_t es9039_src_locked(bool *locked)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(es9039_read(R239_AUTO_FS_READ, &v), TAG, "auto fs read");
    *locked = (v & 0x80) != 0;
    return ESP_OK;
}

/* ------------------ Переключение частоты «на лету» ------------------------ */

/* В sync-slave конфигурации с AUTO_FS_DETECT (Reg3[7]) и AUTO_CLK_GEAR (Reg5[2])
 * чип сам подстраивает CLK_IDAC и SYS_CLK при изменении BCLK/LRCK/MCLK —
 * регистры DAC трогать не надо. Вся забота сводится к:
 *   1) mute (Reg 86) — чтобы на выход не пролезли переходные щелчки;
 *   2) переключить конфигурацию I2S на ESP32 (новая fs/mclk/bclk);
 *   3) подождать SRC lock (Reg249[2] или GPIO2);
 *   4) unmute.
 * При смене семейства (44.1 к <-> 48 к) MCLK меняется — это нормально для
 * AUTO_FS_DETECT: чип отслеживает соотношение SYS_CLK/CLK_IDAC (стр. 62).
 * Параметр apply_cb вызывается между mute и ожиданием lock — там вы меняете
 * конфигурацию I²S-мастера на ESP32-P4. */
esp_err_t es9039_switch_frequency(void (*apply_cb)(void *), void *arg,
                                  uint32_t lock_timeout_ms, uint32_t fs_hz)
{
    if (!apply_cb) return ESP_ERR_INVALID_ARG;
    es9039_mute(true);
    vTaskDelay(pdMS_TO_TICKS(5));
    apply_cb(arg);

    /* 2.5) Пользовательский FIR под новую частоту (интеграция FIR):
     * заливка RAM идёт, пока выход в mute — на звук не влияет. RAM чипа
     * не переживает смену FS «бесплатно»: набор рассчитан под конкретную
     * входную частоту, поэтому перезаливаем здесь. При ошибке/отсутствии
     * набора чип остаётся на встроенном пресете (Reg 88 FILTER_SHAPE). */
    esp_err_t fe = es9039_fir_apply_for_fs(fs_hz, false);
    if (fe == ESP_OK) {
        ESP_LOGI(TAG, "FIR-набор залит под fs=%lu Гц", (unsigned long)fs_hz);
    } else {
        ESP_LOGW(TAG, "FIR-набор не применён (%s) — активен встроенный пресет",
                 esp_err_to_name(fe));
    }

    uint32_t waited = 0;
    bool locked = false;
    while (waited < lock_timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (es9039_src_locked(&locked) == ESP_OK && locked) break;
    }
    if (!locked) {
        ESP_LOGW(TAG, "SRC lock не получен за %lu мс после смены частоты", (unsigned long)lock_timeout_ms);
        /* Оставляем mute, пока тактирование не стабилизируется. */
        return ESP_ERR_TIMEOUT;
    }
    es9039_mute(false);
    return ESP_OK;
}

/* ------------------------- Пример использования --------------------------- */

#if 0
 /* Пример в app_main(). I²S-мастер — только НОВЫЙ драйвер (driver/i2s_std.h):
  * legacy driver/i2s.h в ESP-IDF v6.0 удалён полностью. */
static i2c_master_dev_handle_t s_dac_dev;

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_AUTO,   /* v6: авто-выбор порта (I2C_NUM_0 тоже валиден) */
        .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_9,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    es9039_scan_bus(bus);   /* опционально: убедиться, что 0x48 отвечает */

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES9039_I2C_ADDR_7BIT,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dac_dev));

    ESP_ERROR_CHECK(es9039_init(s_dac_dev, ES9039_GPIO1_AUTOMUTE_PIN));

    /* Плей: смена 44.1 кГц на 48 кГц «на лету» — ESP32-мастер подменяет BCLK/LRCK/MCLK */
    extern void i2s_apply_44k1(void); extern void i2s_apply_48k(void);
    es9039_switch_frequency(i2s_apply_44k1, NULL, 200, 44100);
    vTaskDelay(pdMS_TO_TICKS(2000));
    es9039_switch_frequency(i2s_apply_48k, NULL, 200, 48000);

    es9039_set_volume_db(-25.0f);
    es9039_set_filter(ES9039_FIR_MP_SLOW_LOW_DISP);

    /* Опрос automute-флага (путь B, без пина на плате) */
    bool m1, m2;
    if (es9039_automute_status_i2c(&m1, &m2) == ESP_OK) {
        ESP_LOGI(TAG, "automute CH1=%d CH2=%d", m1, m2);
    }
    /* Путь A (если GPIO1 платы выведен на пин ESP32): */
    // if (es9039_automute_status_pin()) { /* DAC сейчас в automute */ }

    /* Диагностика «как в EVB-гиде»: Fs, статус потока, BCK/WS, automute.
     * mclk_hz = 49152000 — частота генератора платы (вариант Б). */
    es9039_diag_t diag;
    if (es9039_diag_read(&diag, 49152000) == ESP_OK) {
        ESP_LOGI(TAG, "fs≈%lu Hz, вход=%u, ratio_valid=%d, bck_ws_fail=%d, am=%d",
                 (unsigned long)diag.fs_estimate_hz, diag.input_select,
                 diag.ratio_valid, diag.bck_ws_fail, diag.automute_read);
    }

    /* Окно DPLL: 8 — середина диапазона; сузить (меньше джиттера) — 1..3 */
    es9039_set_dpll_bw(8);

    /* Компенсация гармоник: стартовые малые значения для канала 1.
     * Реальные коэффициенты подбираются по замеру THD до/после (см. док).
     * Сброс на дефолт: es9039_thd_comp_clear(); */
    es9039_set_thd_comp(1, -0x0040, 0x0020);
}
#endif

/* ============================ Диагностика =================================
 * Снимок состояния DAC «как в EVB-гиде» — всё из readback-регистров, без
 * обращения к undocumented областям. Мотивация: при интеграции ES9039
 * (DRIVE2, vladikb) отдельным этапом шли определение Fs, состояние TDM
 * и восстановление после потери потока — мониторинг это делает видимым.
 */

/* struct es9039_diag_t перенесён в es9039q2m_esp32p4_driver.h */

/* Оценка частоты дискретизации по формуле даташита (Reg 239, стр. 92):
 *   FS = Y · SYS_CLK / ((X+1) · 128)     (ENABLE_64FS_MODE = 0 в нашей конфиг.)
 * SYS_CLK = MCLK / 2^gear (Reg 5). При AUTO_CLK_GEAR=1 чип выбирает gear сам,
 * поэтому перебираем gear 0..3 и берём вариант, при котором SYS_CLK/FS
 * попадает в штатное окно 128FS <= SYS_CLK < 256FS. */
esp_err_t es9039_diag_read(es9039_diag_t *d, uint32_t mclk_hz)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    uint8_t st_fs = 0, st = 0, st2 = 0, am = 0, up = 0, down = 0, stream = 0, chip = 0;
    ESP_RETURN_ON_ERROR(es9039_read(R239_AUTO_FS_READ, &st_fs), TAG, "diag fs");
    ESP_RETURN_ON_ERROR(es9039_read(R229_INT_STATES, &st), TAG, "diag st1");
    ESP_RETURN_ON_ERROR(es9039_read(R230_INT_STATES2, &st2), TAG, "diag st2");
    ESP_RETURN_ON_ERROR(es9039_read(R242_AUTOMUTE_READ, &am), TAG, "diag am");
    ESP_RETURN_ON_ERROR(es9039_read(R243_SOFT_RAMP_UP_READ, &up), TAG, "diag up");
    ESP_RETURN_ON_ERROR(es9039_read(R245_INPUT_STREAM_RB, &stream), TAG, "diag stream");
    /* SOFT RAMP DOWN READ — читаем отдельным чтением (Reg 244), не критично:
     * если шина занята — просто не учтём ramp_down в поле soft_ramping */
    (void)es9039_read(0xF4, &down);   /* R244_SOFT_RAMP_DOWN_READ */
    (void)es9039_read(R00_CHIP_ID, &chip);

    d->ratio_valid    = (st_fs & 0x80) != 0;
    d->idac_half      = (st_fs & 0x40) != 0;
    d->idac_div_auto  = (uint8_t)(st_fs & 0x3F);

    /* Reg 229 (INTERRUPT STATES): [7]=BCK_WS_FAIL [6]=DOP_VALID [5:4]=SS_RAMP [3:2]=AUTOMUTE [1:0]=VOL_MIN */
    d->bck_ws_fail    = (st & 0x80) != 0;
    d->dop_valid_int  = (st & 0x40) != 0;
    d->automute_state = (st & 0x0C) != 0;
    d->vol_min_state  = (st & 0x03) != 0;

    d->automute_read  = (am & 0x03) != 0;
    d->soft_ramping   = ((up & 0x03) != 0) || ((down & 0x03) != 0);

    d->spdif_valid    = (stream & 0x10) != 0;
    d->tdm_data_valid = (stream & 0x08) != 0;
    d->dop_valid      = (stream & 0x04) != 0;
    d->input_select   = stream & 0x03;

    d->chip_id = chip;

    /* Оценка Fs: перебор gear 0..3 с проверкой окна SYS_CLK/FS из Reg 5 */
    d->fs_estimate_hz = 0;
    if (d->ratio_valid && mclk_hz > 0) {
        uint32_t y = d->idac_half ? 2 : 1;
        for (uint8_t g = 0; g <= 3; g++) {
            uint64_t sys = (uint64_t)mclk_hz >> g;
            uint64_t fs64 = sys * y / ((uint64_t)(d->idac_div_auto + 1) * 128u);
            if (fs64 == 0) continue;
            uint64_t ratio = sys / fs64;
            if (ratio >= 128 && ratio < 256) {
                d->fs_estimate_hz = (uint32_t)fs64;
                break;
            }
        }
    }
    return ESP_OK;
}

/* Окно DPLL (Reg 29[7:4], стр. 66): 1 = самое узкое (минимум джиттера,
 * но выше требования к стабильности источника), 15 = самое широкое.
 * Дефолт чипа — 4. В нашем ASYNC-варианте (SYNC_MODE=0) DPLL активен и
 * преследует BCK/LRCK, поэтому окно влияет на джиттероустойчивость тракта. */
esp_err_t es9039_set_dpll_bw(uint8_t bw)
{
    if (bw < 1 || bw > 15) return ESP_ERR_INVALID_ARG;
    return es9039_write(R29_DPLL_BW, (uint8_t)(bw << 4));
}

/* Сброс interrupt-флагов: поля CLEAR «toggle high-low» (стр. 66).
 * Reg 20: [7]=BCK_WS_FAIL [6]=DOP_VALID [5:4]=SS_RAMP [3:2]=AUTOMUTE [1:0]=VOL_MIN
 * Reg 21: [7:6]=INPUT_SELECT_OVERRIDE [5]=TDM_VALID_EDGE (маска 0xE0) */
esp_err_t es9039_clear_interrupts(void)
{
    ESP_RETURN_ON_ERROR(es9039_write(R020_INTERRUPT_CLEAR, 0xFF), TAG, "int clr hi");
    ESP_RETURN_ON_ERROR(es9039_write(R021_INTERRUPT_CLEAR2, 0xE0), TAG, "int clr2 hi");
    ESP_RETURN_ON_ERROR(es9039_write(R020_INTERRUPT_CLEAR, 0x00), TAG, "int clr lo");
    ESP_RETURN_ON_ERROR(es9039_write(R021_INTERRUPT_CLEAR2, 0x00), TAG, "int clr2 lo");
    return ESP_OK;
}

/* ==================== Компенсация гармоник (THD C2/C3) ====================
 * Рег 91-94 (THD C2) и 107-110 (THD C3), 16-бит signed на канал:
 *   output = x + c2·x² + c3·x³                       (стр. 86)
 * По даташиту ES9039MSPRO/SPRO v0.4 коэффициенты следует подбирать in-situ
 * на каждом устройстве. Q-формат коэффициентов ESS не публикует: значения
 * подбираются по замеру (тон 1 кГц @ -1 dBFS, слежение за 2-й/3-й гармоникой
 * в спектре), начиная с малых значений (|c| < 0x1000).
 */

/* ch = 1 или 2; коэффициенты — сырые int16 (формат подбора см. выше).
 * Порядок байтов: младший байт в первом регистре (Reg 91 = CH1[7:0]). */
esp_err_t es9039_set_thd_comp(uint8_t ch, int16_t c2, int16_t c3)
{
    if (ch != 1 && ch != 2) return ESP_ERR_INVALID_ARG;
    uint8_t c2_lo = (uint8_t)((uint16_t)c2 & 0xFF);
    uint8_t c2_hi = (uint8_t)(((uint16_t)c2) >> 8);
    uint8_t c3_lo = (uint8_t)((uint16_t)c3 & 0xFF);
    uint8_t c3_hi = (uint8_t)(((uint16_t)c3) >> 8);
    if (ch == 1) {
        ESP_RETURN_ON_ERROR(es9039_write(R91_THD_C2_CH1_LSB, c2_lo), TAG, "c2 ch1 l");
        ESP_RETURN_ON_ERROR(es9039_write(R92_THD_C2_CH1_MSB, c2_hi), TAG, "c2 ch1 h");
        ESP_RETURN_ON_ERROR(es9039_write(R107_THD_C3_CH1_LSB, c3_lo), TAG, "c3 ch1 l");
        return es9039_write(R108_THD_C3_CH1_MSB, c3_hi);
    }
    ESP_RETURN_ON_ERROR(es9039_write(R93_THD_C2_CH2_LSB, c2_lo), TAG, "c2 ch2 l");
    ESP_RETURN_ON_ERROR(es9039_write(R94_THD_C2_CH2_MSB, c2_hi), TAG, "c2 ch2 h");
    ESP_RETURN_ON_ERROR(es9039_write(R109_THD_C3_CH2_LSB, c3_lo), TAG, "c3 ch2 l");
    return es9039_write(R110_THD_C3_CH2_MSB, c3_hi);
}

/* Сброс компенсации: коэффициенты 0 = коррекция выключена (дефолт чипа) */
esp_err_t es9039_thd_comp_clear(void)
{
    return es9039_set_thd_comp(1, 0, 0) == ESP_OK ? es9039_set_thd_comp(2, 0, 0) : ESP_FAIL;
}

/* ============================ конец файла ================================= */
