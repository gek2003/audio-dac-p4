/*
 * ============================================================================
 *  ui_display.c — инициализация MIPI-DSI (2 lane) + HX8394 + LVGL
 *  Плата Waveshare ESP32-P4-WIFI6-Touch-LCD-5 (IPS 720x1280)
 *
 *  Подсветка — по схеме платы (проверено по schematic, 11.2026):
 *    контроллер подсветки AP3032KTR-G1, LED-цепочка 19.8 В.
 *    BL_EN  -> GPIO33 (вход EN контроллера; подтянут к 5 В через 100 кОм);
 *    BL_PWM -> GPIO26 (ШИМ яркости через транзистор Q9 на вход EN).
 *    I²C-контура в подсветке на этой плате НЕТ (0x45 — у других панелей
 *    Waveshare, напр. 10.1"). Яркость задаётся только скважностью PWM.
 * ============================================================================
 */
#include "ui_display.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"  /* esp_hw_support: LDO API в v6.1 переехал сюда */
#include "esp_lvgl_port.h"
#include "esp_lcd_hx8394.h"             /* waveshare/esp_lcd_hx8394 (managed-компонент) */

static const char *TAG = "ui_disp";

#define PANEL_W        720
#define PANEL_H        1280

/* Подсветка — пины из схемы платы ESP32-P4-WIFI6-Touch-LCD-5. */
#define BL_PWM_GPIO    26   /* LCD BL PWM — ШИМ яркости */
#define BL_EN_GPIO     33   /* BL EN — разрешение контроллера AP3032KTR-G1 */
#define BL_PWM_FREQ    20000        /* 20 кГц — выше слышимого диапазона */

static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_handle_t   s_panel;
static esp_lcd_panel_io_handle_t s_dbi_io;        /* DBI-канал: Sleep In/Out */
static lv_display_t            *s_disp;

static void backlight_init(void)
{
    /* BL_EN: выход, высокий — разрешение. По схеме подтянут к 5 В через 100 кОм,
     * т.е. разрешён и до настройки — настраиваем явно, чтобы не зависеть от
     * подводного состояния пина (GPIO33 может быть в любом дефолтном режиме). */
    gpio_config_t en_cfg = {
        .pin_bit_mask = (1ULL << BL_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&en_cfg));
    gpio_set_level(BL_EN_GPIO, 1);

    /* BL_PWM: LEDC, 20 кГц, 10 бит; стартуем с duty 0 — яркость задаёт
     * ui_display_brightness_set(). */
    ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz         = BL_PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_chan = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BL_PWM_GPIO,
        .duty       = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_chan));
    ESP_LOGI(TAG, "Подсветка: BL_EN=GPIO%d (вкл), BL_PWM=GPIO%d 20 кГц",
             BL_EN_GPIO, BL_PWM_GPIO);
}

esp_err_t ui_display_init(void)
{
    /* I²C-шина инициализируется отдельно (main.c) — для тача GT911 и ЦАПа;
     * подсветка этой платы чисто PWM (BL_PWM GPIO26 / BL_EN GPIO33). */

    backlight_init();

    /* --- Питание MIPI-D PHY через LDO (канал/напряжение — из примера драйвера) --- */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,       /* LDO_VO3 подключён к VDD_MIPI_DPHY */
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));
    ESP_LOGI(TAG, "MIPI-D PHY LDO OK");

    /* --- Шина DSI: 2 data lane --- */
    esp_lcd_dsi_bus_config_t bus_cfg = HX8394_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus));
    ESP_LOGI(TAG, "DSI bus OK");

    /* --- DBI-канал для управляющих команд панели --- */
    esp_lcd_dbi_io_config_t dbi_cfg = HX8394_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi_io));

    /* --- DPI-тайминги 720x1280: макрос заголовка HX8394 (IDF6: .in_color_format).
     *     Если на экране мусор/полосы — править тайминги в первую очередь. --- */
    esp_lcd_dpi_panel_config_t dpi_cfg =
        HX8394_720_1280_PANEL_30HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.num_fbs = 2;    /* двойная буферизация DPI: против тиринга */

    hx8394_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = 2,           /* 2 data lane */
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_hx8394(s_dbi_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "HX8394 panel OK (720x1280)");

    /* --- Стартовая яркость 80% --- */
    ui_display_brightness_set(80);

    /* --- LVGL (порт v2, LVGL9) --- */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_dbi_io,
        .panel_handle = s_panel,
        .buffer_size = PANEL_W * PANEL_H,      /* пикселей на буфер (полный кадр) */
        .double_buffer = true,
        .hres = PANEL_W,
        .vres = PANEL_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            /* горизонтальная ориентация: 1280x720.
             * Если картинка «повёрнута не туда» — поменяйте mirror_x/mirror_y. */
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,        /* LVGL-буферы — в PSRAM (см. buff_spiram) */
            .buff_spiram = true,
            .sw_rotate = true,        /* swap_xy/mirror делает софтверно (HW нет) */
            .swap_bytes = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_disp_cfg = { 0 };   /* avoid_tearing — при проблемах включить */
    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL display OK (1280x720, rot 90)");
    return ESP_OK;
}

lv_display_t *ui_display_get(void)
{
    return s_disp;
}

static int s_brightness_pct = 0;

void ui_display_brightness_set(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_brightness_pct = percent;
    uint32_t duty = (percent * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* Текущий заданный уровень (не читает LEDC) — для восстановления после
 * автотушения и для слайдера «Настройки». */
int ui_display_brightness_get(void)
{
    return s_brightness_pct;
}

/* --- Сон панели -------------------------------------------------------------
 * Драйвер HX8394 не реализует disp_on_off, поэтому стандартные команды MIPI-DSI
 * шлём напрямую через DBI-канал: Sleep In (0x28) / Sleep Out (0x11).
 * Подсветка — вне: гасим/включаем здесь же, чтобы «спящий» и «погашенный»
 * экраны не расходились. DPI-поток LVGL продолжает идти — панель его
 * игнорирует в sleep (это норма для MIPI-DSI).
 * Возврат: ESP_OK при успехе; если DBI не доступен — ESP_ERR_NOT_SUPPORTED
 * (экономия останется только на подсветке). */
esp_err_t ui_display_sleep(bool on)
{
    if (s_dbi_io == NULL) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t e;
    if (on) {
        ui_display_brightness_set(0);
        e = esp_lcd_panel_io_tx_param(s_dbi_io, 0x28, NULL, 0);   /* Sleep In */
    } else {
        e = esp_lcd_panel_io_tx_param(s_dbi_io, 0x11, NULL, 0);   /* Sleep Out */
        if (e == ESP_OK) {
            /* Спецификация MIPI-DSI: не менее 120 мс после Sleep Out до
             * стабильного изображения. Редкий вызов — допустимо блокировать. */
            vTaskDelay(pdMS_TO_TICKS(130));
            ui_display_brightness_set(ui_display_brightness_get());
        }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "панель sleep %s: %s (экономия только подсветкой)",
                 on ? "in" : "out", esp_err_to_name(e));
    }
    return e;
}
