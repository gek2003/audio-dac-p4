/*
 * ============================================================================
 *  ui.c — графическая оболочка (LVGL 9)
 *  Языки EN/RU с переключением в Настройках (полная пересборка экранов).
 *
 *  КИРИЛЛИЦА: в проекте уже есть кириллические шрифты ui_font_ru_16/28/40.c
 *  (Noto Sans, сгенерированы через lv_font_conv; при необходимости пересоздать
 *  — bash tools/generate_fonts.sh). Диапазон: латиница 0x20-0x7F,
 *  полная кириллица 0x400-0x4FF, тире/минус, «», №.
 *
 *  Объявлены weak: если файлы шрифтов удалить из сборки, линкер даст NULL
 *  и UI автоматически перейдёт на montserrat (fallback включён в sdkconfig).
 * ============================================================================
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_check.h"

#include "ui_display.h"
#include "ui_touch.h"
#include "sd_card.h"
#include "usb_msc.h"
#include "audio_pipeline.h"
#include "playlist.h"
#include "meta.h"
#include "stations.h"
#include "nvs.h"
#include "driver/jpeg_decode.h"        /* аппаратный JPEG-декодер P4 (esp_driver_jpeg) */

#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

#ifndef LV_SYMBOL_LOCK
#define LV_SYMBOL_LOCK LV_SYMBOL_EYE_CLOSE
#endif

static const char *TAG = "ui";

/* рабочая область после поворота */
#define UI_W  1280
#define UI_H  720

static void ui_rebuild(void);

/* --- i18n ---------------------------------------------------------------- */
enum {
    STR_HOME, STR_VOLUME, STR_MUTE, STR_FILTERS, STR_DIAG, STR_SETTINGS,
    STR_LANGUAGE, STR_SOURCE, STR_BRIGHTNESS, STR_REG88, STR_CUSTOM_FIR,
    STR_NONE, STR_NETWORK, STR_WIFI_SSID, STR_PASSWORD, STR_CONNECT,
    STR_SRC_URL, STR_APPLY, STR_FILES, STR_NO_CARD, STR_EMPTY_DIR,
    STR_PLAYING, STR_USB_HOST, STR_UP, STR_PLAYLIST_OPEN, STR_TRACK,
    STR_RADIO, STR_ST_NAME, STR_ST_ADD, STR_ST_EMPTY, STR_ST_NOTHTTP,
    STR_SCAN, STR_SCANNING, STR_HIDDEN, STR_BL_TIMEOUT,
    STR_STANDBY, STR_STANDBY_PLAYING, STR_COUNT
};

static const char *const S[STR_COUNT][2] = {
    [STR_HOME]      = { "Home",        "Главный" },
    [STR_VOLUME]    = { "Volume",      "Громкость" },
    [STR_MUTE]      = { "Mute",        "Без звука" },
    [STR_FILTERS]   = { "Filters",     "Фильтры" },
    [STR_DIAG]      = { "Diagnostics", "Диагностика" },
    [STR_SETTINGS]  = { "Settings",    "Настройки" },
    [STR_LANGUAGE]  = { "Language",    "Язык" },
    [STR_SOURCE]    = { "Source",      "Источник" },
    [STR_BRIGHTNESS]= { "Brightness",  "Подсветка" },
    [STR_REG88]     = { "Reg 88 preset", "Пресет Reg 88" },
    [STR_CUSTOM_FIR]= { "Custom FIR",  "Кастомный FIR" },
    [STR_NONE]      = { "None",        "—" },
    [STR_NETWORK]   = { "Network",     "Сеть" },
    [STR_WIFI_SSID] = { "Wi-Fi SSID",  "Сеть Wi-Fi (SSID)" },
    [STR_PASSWORD]  = { "Password",    "Пароль" },
    [STR_CONNECT]   = { "Connect",     "Подключить" },
    [STR_SRC_URL]   = { "Source URL",  "URL источника" },
    [STR_APPLY]     = { "Apply",       "Применить" },
    [STR_FILES]     = { "SD Files",    "Файлы SD" },
    [STR_NO_CARD]   = { "No SD card",  "SD-карта не найдена" },
    [STR_EMPTY_DIR] = { "(empty)",     "(пусто)" },
    [STR_PLAYING]   = { "Playing",     "Играет" },
    [STR_USB_HOST]  = { "Card is on PC (USB)", "Карта у ПК (USB)" },
    [STR_UP]        = { "Up",          "Вверх" },
    [STR_PLAYLIST_OPEN] = { "Playlist: %d tracks", "Плейлист: %d трек(ов)" },
    [STR_TRACK]     = { "Track",       "Трек" },
    [STR_RADIO]     = { "Radio",       "Радио" },
    [STR_ST_NAME]   = { "Station name", "Имя станции" },
    [STR_ST_ADD]    = { "Add current source", "Добавить текущий источник" },
    [STR_ST_EMPTY]  = { "No favorite stations yet. Open a radio URL in Network tab, then Add here.\n"
                         "Tap a station to play, long press to delete.",
                         "Избранных станций нет. Откройте радио-URL на вкладке «Сеть», затем добавьте здесь.\n"
                         "Тап — слушать, долгий тап — удалить." },
    [STR_ST_NOTHTTP] = { "Current source is not a radio URL", "Текущий источник — не HTTP-URL" },
    [STR_SCAN]      = { "Scan networks",  "Сканировать сети" },
    [STR_SCANNING]  = { "Scanning...",    "Сканирую..." },
    [STR_HIDDEN]    = { "(hidden network)", "(скрытая сеть)" },
    [STR_BL_TIMEOUT] = { "Backlight timeout", "Гашение подсветки" },
    [STR_STANDBY]   = { "Standby (screen + Wi-Fi)", "Спящий режим (экран + Wi-Fi)" },
    [STR_STANDBY_PLAYING] = { "Wi-Fi stays on while radio is playing",
                              "Радио играет — Wi-Fi не выключаем" },
};

static ui_lang_t s_lang = UI_LANG_RU;

/* шрифты с кириллицей (weak — см. шапку файла) */
extern const lv_font_t ui_font_ru_40 __attribute__((weak));
extern const lv_font_t ui_font_ru_28 __attribute__((weak));
extern const lv_font_t ui_font_ru_16 __attribute__((weak));

static const lv_font_t *f_huge(void)
{
    return &ui_font_ru_40 ? &ui_font_ru_40 : &lv_font_montserrat_40;
}
static const lv_font_t *f_big(void)
{
    return &ui_font_ru_28 ? &ui_font_ru_28 : &lv_font_montserrat_28;
}
static const lv_font_t *f_norm(void)
{
    return &ui_font_ru_16 ? &ui_font_ru_16 : &lv_font_montserrat_16;
}

static ui_hooks_t s_hooks;   /* любое поле может быть NULL — UI это учитывает */
static i2c_master_bus_handle_t s_i2c_bus;   /* общая шина: ЦАП + GT911 + подсветка 0x45 */

/* элементы состояния */
static lv_obj_t *s_vol_lbl;
static lv_obj_t *s_vol_slider;    /* зеркало громкости (P2-12) */
static lv_obj_t *s_mute_sw;
static lv_obj_t *s_src_lbl;
static lv_obj_t *s_diag_lbl;
static lv_obj_t *s_reg_dd;
static lv_obj_t *s_fir_dd;
static lv_obj_t *s_lang_dd;
static bool s_diag_timer_done;

/* Главная: метаданные трека */
static lv_obj_t *s_title_lbl;     /* название трека / StreamTitle */
static lv_obj_t *s_artist_lbl;    /* исполнитель / радиостанция */

/* Главная: управление воспроизведением и прогресс */
static lv_obj_t *s_pp_btn;        /* play/pause — иконка меняется */
static lv_obj_t *s_pp_icon;
static lv_obj_t *s_prev_btn, *s_prev_icon, *s_next_btn;
static lv_obj_t *s_prog_bar;      /* прогресс файла (скрыт для радио) */
static lv_obj_t *s_prog_lbl;      /* 0:12 / 3:45 */
static bool s_prog_timer_done;
static bool s_scrubbing;          /* пользователь держит прогресс-слайдер */
static bool s_seek_in_flight;     /* seek выполнен — ждём первые данные декодера */

/* вкладка «Сеть»: поля ввода + экранная клавиатура */
static lv_obj_t *s_ssid_ta, *s_pass_ta, *s_url_ta, *s_net_kb;

/* обложка альбома: widget + RGB565-буфер из аппаратного декодера JPEG */
static lv_obj_t *s_cover_img;
static lv_image_dsc_t s_cover_dsc;
static uint8_t *s_cover_rgb;
static uint32_t s_cover_w, s_cover_h;
static int      s_cover_serial = -1;
static jpeg_decoder_handle_t s_jpeg_dec;

/* вкладка «Радио»: избранные станции */
static lv_obj_t *s_radio_list;
static lv_obj_t *s_radio_name_ta;
static bool s_stations_loaded;

/* вкладка «Сеть»: скан Wi-Fi */
static lv_obj_t *s_wifi_scan_btn, *s_wifi_scan_lbl, *s_wifi_list;

/* Автотушение подсветки: таймер неактивности + хранение в NVS */
static bool s_touch_ok;                 /* тач поднялся — иначе автотушение не включаем */
static uint32_t s_idle_timeout_ms;      /* 0 = никогда */
static bool s_screen_off;
static lv_obj_t *s_bl_to_dd;

/* Спящий режим: гашение панели + выключение Wi-Fi по таймеру */
static uint32_t s_standby_timeout_ms;   /* 0 = никогда */
static bool s_standby;
static lv_obj_t *s_standby_dd;

/* вкладка «Файлы»: браузер SD-карты */
static lv_obj_t *s_files_list, *s_files_title, *s_files_play_lbl;
static char      s_files_path[512] = "/sdcard";
static bool      s_sd_ok;

/* Громкость: слайдер 0..255 => 0..-127.5 dB (шаг 0.5 dB) */
static int db_to_slider(float db)
{
    int v = (int)(-db * 2.0f + 0.5f);
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
static float slider_to_db(int v)
{
    return -(float)v / 2.0f;
}

/* --- обложка альбома: аппаратный JPEG -> RGB565 ------------------------------
 * Декодирование по требованию (смена serial), в LVGL-таске: ~50-150 мс на
 * обложку 500x500 — приемлемо при смене трека. Оборудование P4: esp_driver_jpeg.
 * Выход RGB565 с паддингом строки до 16 px (YUV420/422) — stride в dsc. */
#define COVER_WIDGET_SIZE 190
#define COVER_MAX_DIM     2048

static void cover_show_none(void)
{
    if (s_cover_img) {
        lv_obj_set_hidden(s_cover_img, true);
    }
}

static void decode_cover(const meta_cover_t *cv)
{
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(cv->data, (uint32_t)cv->len, &info) != ESP_OK) {
        ESP_LOGW(TAG, "обложка: JPEG-заголовок не разобран");
        s_cover_serial = cv->serial;
        cover_show_none();
        return;
    }
    if (info.width == 0 || info.height == 0 ||
        info.width > COVER_MAX_DIM || info.height > COVER_MAX_DIM) {
        ESP_LOGW(TAG, "обложка %lux%lu — вне пределов, пропускаем",
                 (unsigned long)info.width, (unsigned long)info.height);
        s_cover_serial = cv->serial;
        cover_show_none();
        return;
    }
    /* Строки YUV420/422 паддятся до 16 px (заметка из jpeg_decode.h). */
    uint32_t padded_w = (info.width + 15) & ~15u;
    size_t need = (size_t)padded_w * info.height * 2;

    if (s_cover_rgb != NULL && (s_cover_w != padded_w || s_cover_h != info.height)) {
        heap_caps_free(s_cover_rgb);
        s_cover_rgb = NULL;
    }
    if (s_cover_rgb == NULL) {
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t got = 0;
        s_cover_rgb = jpeg_alloc_decoder_mem(need, &mem_cfg, &got);
        if (s_cover_rgb == NULL) {
            ESP_LOGW(TAG, "нет памяти под обложку (%zu байт)", need);
            s_cover_serial = cv->serial;
            cover_show_none();
            return;
        }
    }

    if (s_jpeg_dec == NULL) {
        jpeg_decode_engine_cfg_t eng = {
            .intr_priority = 0,
            .timeout_ms = -1,
        };
        if (jpeg_new_decoder_engine(&eng, &s_jpeg_dec) != ESP_OK) {
            s_jpeg_dec = NULL;
            ESP_LOGW(TAG, "аппаратный JPEG-декодер не открыт");
            s_cover_serial = cv->serial;
            cover_show_none();
            return;
        }
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* R/B на железе проверить */
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t e = jpeg_decoder_process(s_jpeg_dec, &dec_cfg,
                                       cv->data, (uint32_t)cv->len,
                                       s_cover_rgb, (uint32_t)need, &out_size);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "декодирование обложки: %s", esp_err_to_name(e));
        s_cover_serial = cv->serial;
        cover_show_none();
        return;
    }

    s_cover_w = padded_w;
    s_cover_h = info.height;
    memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
    s_cover_dsc.header.w = info.width;            /* видимые размеры */
    s_cover_dsc.header.h = info.height;
    s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cover_dsc.header.stride = padded_w * 2;     /* паддинг аппаратного декодера */
    s_cover_dsc.data = s_cover_rgb;
    s_cover_dsc.data_size = need;

    lv_image_set_src(s_cover_img, &s_cover_dsc);
    lv_obj_set_hidden(s_cover_img, false);
    s_cover_serial = cv->serial;
    ESP_LOGI(TAG, "обложка показана: %lux%lu (stride %lu)",
             (unsigned long)info.width, (unsigned long)info.height,
             (unsigned long)s_cover_dsc.header.stride);
}

static void cover_tick(void)
{
    if (s_cover_img == NULL) return;
    meta_cover_t cv;
    if (!meta_cover_get(&cv) || cv.serial == s_cover_serial) return;
    if (cv.kind == 1) {
        decode_cover(&cv);            /* JPEG — аппаратный декодер */
    } else {
        /* PNG: аппаратного декодера нет; софтверный — отдельный компонент,
         * пока пропускаем (обложки ID3 почти всегда JPEG). */
        ESP_LOGI(TAG, "обложка PNG — не декодируем (MVP: JPEG)");
        s_cover_serial = cv.serial;
        cover_show_none();
    }
}

/* --- автотушение подсветки: NVS + логика ----------------------------------- */
static const uint32_t s_bl_options_ms[4] = { 0, 60 * 1000, 5 * 60 * 1000, 15 * 60 * 1000 };

static void bl_timeout_load(void)
{
    nvs_handle_t h;
    if (nvs_open("dac", NVS_READONLY, &h) == ESP_OK) {
        uint8_t idx = 0;
        if (nvs_get_u8(h, "bl_timeout", &idx) == ESP_OK &&
            idx < sizeof(s_bl_options_ms) / sizeof(s_bl_options_ms[0])) {
            s_idle_timeout_ms = s_bl_options_ms[idx];
        }
        nvs_close(h);
    }
}

static void bl_timeout_save(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(s_bl_options_ms) / sizeof(s_bl_options_ms[0]))) return;
    nvs_handle_t h;
    if (nvs_open("dac", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bl_timeout", (uint8_t)idx);
        nvs_commit(h);
        nvs_close(h);
        s_idle_timeout_ms = s_bl_options_ms[idx];
    }
}

/* Проверка неактивности: вызывается из prog_tick (каждые 500 мс).
 * Неактивность берём у LVGL (lv_display_get_inactive_time): сбрасывается
 * любым вводом (тап/движение). Первый тап и будит, и действует — экран
 * включится и событие обработается (двухфазное пробуждение не делаем). */
static void bl_idle_check(void)
{
    if (!s_touch_ok || s_idle_timeout_ms == 0 || s_standby) return;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s_screen_off && idle >= s_idle_timeout_ms) {
        ui_display_brightness_set(0);
        s_screen_off = true;
        ESP_LOGI(TAG, "Подсветка погашена (неактивность %lu мс)", (unsigned long)idle);
    } else if (s_screen_off && idle < s_idle_timeout_ms) {
        ui_display_brightness_set(ui_display_brightness_get());
        s_screen_off = false;
        ESP_LOGI(TAG, "Подсветка включена по касанию");
    }
}

/* Спящий режим: после гашения подсветки, по отдельному таймеру — панель
 * Sleep In (команда 0x28 через DBI) + Wi-Fi off (если не играет радио/HTTP).
 * Пробуждение: любое касание — Sleep Out, подсветка, Wi-Fi НЕ включается
 * автоматически (только выбор радио/скан — так экономим ток без сюрпризов). */
static const uint32_t s_standby_options_ms[4] = { 0, 5*60*1000, 15*60*1000, 30*60*1000 };

static void standby_load(void)
{
    nvs_handle_t h;
    if (nvs_open("dac", NVS_READONLY, &h) == ESP_OK) {
        uint8_t idx = 0;
        if (nvs_get_u8(h, "standby_min", &idx) == ESP_OK &&
            idx < sizeof(s_standby_options_ms) / sizeof(s_standby_options_ms[0])) {
            s_standby_timeout_ms = s_standby_options_ms[idx];
        }
        nvs_close(h);
    }
}

static void standby_save(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(s_standby_options_ms) / sizeof(s_standby_options_ms[0]))) return;
    nvs_handle_t h;
    if (nvs_open("dac", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "standby_min", (uint8_t)idx);
        nvs_commit(h);
        nvs_close(h);
        s_standby_timeout_ms = s_standby_options_ms[idx];
        /* Ручное включение режима «Никогда» из сна — выходим */
        if (idx == 0 && s_standby) {
            ui_display_sleep(false);
            s_standby = false;
            s_screen_off = false;
        }
    }
}

static void standby_check(void)
{
    if (!s_touch_ok || s_standby_timeout_ms == 0) return;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s_standby) {
        if (idle >= s_standby_timeout_ms) {
            s_standby = true;
            s_screen_off = true;
            ui_display_sleep(true);
            const char *src = s_hooks.get_source ? s_hooks.get_source() : "";
            if (strncmp(src, "http", 4) == 0) {
                ESP_LOGI(TAG, "%s", S[STR_STANDBY_PLAYING][s_lang]);
            } else {
                wifi_off();     /* SD или тишина — Wi-Fi не нужен */
            }
            ESP_LOGI(TAG, "Спящий режим (неактивность %lu мс)", (unsigned long)idle);
        }
    } else if (idle < s_standby_timeout_ms) {
        /* тап по спящему: подсветку вернёт ui_display_sleep(false) */
        ui_display_sleep(false);
        s_standby = false;
        s_screen_off = false;
    }
}

/* --- события ------------------------------------------------------------- */
static void on_vol_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    float db = slider_to_db(lv_slider_get_value(sl));
    if (s_hooks.set_volume_db) s_hooks.set_volume_db(db);
    if (s_vol_lbl) lv_label_set_text_fmt(s_vol_lbl, "%.1f dB", db);
}

static void on_mute_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    if (s_hooks.set_mute) s_hooks.set_mute(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void on_reg_dd(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    if (s_hooks.set_filter) s_hooks.set_filter((int)lv_dropdown_get_selected(dd));
}

static void on_fir_dd(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    if (s_hooks.set_fir) s_hooks.set_fir((int)lv_dropdown_get_selected(dd));
}

static void kb_apply_lang(void);

static void on_lang_dd(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    ui_lang_t lang = (ui_lang_t)lv_dropdown_get_selected(dd);
    if (lang != s_lang) {
        s_lang = lang;
        ui_rebuild();          /* пересобираем всё с новым языком */
        kb_apply_lang();       /* клавиатура живёт на layer_top и пересоздаётся не всегда */
    }
}

static void on_br_slider(lv_event_t *e)
{
    int pct = (int)lv_slider_get_value(lv_event_get_target(e));
    ui_display_brightness_set(pct);
    nvs_handle_t h;
    if (nvs_open("dac", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bl_pct", (uint8_t)pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void diag_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_diag_lbl || !s_hooks.get_diag) return;
    char buf[512];
    s_hooks.get_diag(buf, sizeof(buf));
    lv_label_set_text(s_diag_lbl, buf);
}

/* --- ввод: клавиатура поверх интерфейса ----------------------------------
 * Одна клавиатура на все поля (lv_layer_top). Появляется при фокусе на поле,
 * закрывается кнопками Enter (LV_EVENT_READY) и Cancel (LV_EVENT_CANCEL)
 * клавиатуры LVGL. */
/* Кириллическая клавиатура: назначается на режим SPECIAL (lv_keyboard).
 * Стандартные карты клавиатуры LVGL остаются: TEXT = латиница (SSID, URL),
 * NUMBER = цифры; кнопка переключения циклит TEXT -> SPECIAL -> NUMBER,
 * т.е. «спец»-кнопка = переключение RU/EN. Кнопки управления
 * (LV_SYMBOL_*) обрабатываются клавиатурой по содержимому — работают. */
static const char *s_kb_ru_lower[] = {
    "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "\n",
    "ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "\n",
    "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", "ё", LV_SYMBOL_LEFT, "\n",
    LV_SYMBOL_CLOSE, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
[[maybe_unused]] static const char *s_kb_ru_upper[] = {
    "Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З", "Х", "\n",
    "Ф", "Ы", "В", "А", "П", "Р", "О", "Л", "Д", "Ж", "Э", "\n",
    "Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", "Ё", LV_SYMBOL_LEFT, "\n",
    LV_SYMBOL_CLOSE, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static void kb_apply_lang(void)
{
    if (s_net_kb != NULL) {
        /* «спец»-кнопка = русская раскладка; TEXT-режимы — штатная латиница LVGL */
        lv_keyboard_set_map(s_net_kb, LV_KEYBOARD_MODE_SPECIAL,
                            s_kb_ru_lower, NULL);
    }
}

static void kb_hide(void)
{
    if (s_net_kb) lv_obj_set_hidden(s_net_kb, true);
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        if (s_net_kb == NULL) {
            s_net_kb = lv_keyboard_create(lv_layer_top());
            lv_obj_set_size(s_net_kb, UI_W, 320);
            lv_obj_align(s_net_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
        kb_apply_lang();
        lv_keyboard_set_textarea(s_net_kb, ta);
        lv_obj_set_hidden(s_net_kb, false);
        lv_obj_move_foreground(s_net_kb);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        kb_hide();
    }
}

static void on_wifi_apply(lv_event_t *e)
{
    (void)e;
    const char *ssid = s_ssid_ta ? lv_textarea_get_text(s_ssid_ta) : "";
    const char *pass = s_pass_ta ? lv_textarea_get_text(s_pass_ta) : "";
    kb_hide();
    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "SSID пуст — Wi-Fi не применяется");
        return;
    }
    if (s_hooks.set_wifi) s_hooks.set_wifi(ssid, pass);
}

static void on_url_apply(lv_event_t *e)
{
    (void)e;
    const char *url = s_url_ta ? lv_textarea_get_text(s_url_ta) : "";
    kb_hide();
    if (url[0] == '\0') {
        ESP_LOGW(TAG, "URL пуст — источник не меняется");
        return;
    }
    if (s_hooks.set_source_url) s_hooks.set_source_url(url);
    if (s_src_lbl) lv_label_set_text(s_src_lbl, url);
}

static lv_obj_t *make_textarea(lv_obj_t *c, lv_coord_t x, lv_coord_t y,
                               lv_coord_t w, bool password)
{
    lv_obj_t *ta = lv_textarea_create(c);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_max_length(ta, 128);
    lv_obj_set_size(ta, w, 44);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_add_event_cb(ta, ta_cb, LV_EVENT_ALL, NULL);
    return ta;
}

static lv_obj_t *make_button(lv_obj_t *c, const char *txt, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(c);
    lv_obj_set_size(btn, w, 44);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, f_norm(), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* --- браузер SD-карты (вкладка «Файлы») -----------------------------------
 * Список файлов/папок текущего каталога. Папка — вход, «..» — вверх,
 * файл .mp3/.flac/.wav — воспроизведение через set_source_url("file://...").
 * Чтение директории — из LVGL-задачи: opendir/stat по SDMMC короткие,
 * FatFS секционирует одновременный доступ с декодером. */
/* --- замена deprecated lv_list: flex-колонка + строки-кнопки ---------------
 * Структура детей строки повторяет lv_list_add_button: [0] — иконка,
 * [1] — метка (обращения lv_obj_get_child(btn, 0/1) остаются валидными).
 * RSSI в Wi-Fi добавляется третьим ребёнком и прижимается вправо grow-меткой. */
static lv_obj_t *ui_list_create(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

static lv_obj_t *ui_list_add_button(lv_obj_t *parent, const char *icon, const char *txt)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a4148), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_pad_ver(btn, 6, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_pad_right(ic, 8, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_flex_grow(lbl, 1);
    return btn;
}

static void files_refresh(void);

static bool name_is_audio(const char *n)
{
    static const char *const exts[] = { ".mp3", ".flac", ".wav", ".dsf", ".dff" };
    size_t len = strlen(n);
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len >= el && strncasecmp(n + len - el, exts[i], el) == 0) return true;
    }
    return false;
}

static bool name_is_playlist(const char *n)
{
    size_t len = strlen(n);
    if (len < 4) return false;
    const char *e = n + len - 4;
    return strncasecmp(e, ".m3u", 4) == 0 || strncasecmp(e, ".m3u8", 5) == 0;
}

static void on_file_open(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    char url[600];
    snprintf(url, sizeof(url), "file://%s/%s", s_files_path, name);
    if (s_hooks.set_source_url) s_hooks.set_source_url(url);
    if (s_src_lbl) lv_label_set_text(s_src_lbl, url);
    if (s_files_play_lbl) {
        lv_label_set_text_fmt(s_files_play_lbl, "%s: %s", S[STR_PLAYING][s_lang], name);
    }
}

/* .m3u/.m3u8 — очередь из плейлиста: main.c разберёт и запустит первый трек */
static void on_playlist_open(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    char url[600];
    snprintf(url, sizeof(url), "file://%s/%s", s_files_path, name);
    if (s_hooks.set_source_url) s_hooks.set_source_url(url);
    if (s_src_lbl) lv_label_set_text(s_src_lbl, url);
    if (s_files_play_lbl) {
        int n = 0;
        char tmp[600];
        snprintf(tmp, sizeof(tmp), "%s/%s", s_files_path, name);
        if (playlist_load_m3u(tmp) == ESP_OK) n = playlist_count();
        playlist_clear();   /* main.c перезагрузит список при старте */
        lv_label_set_text_fmt(s_files_play_lbl, S[STR_PLAYLIST_OPEN][s_lang], n);
    }
}

static void on_dir_enter(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    char name_copy[256];
    strlcpy(name_copy, name ? name : "", sizeof(name_copy));
    size_t plen = strlen(s_files_path);
    size_t nlen = strlen(name_copy);
    if (plen + nlen + 2 >= sizeof(s_files_path)) return;   /* путь не влезает */
    s_files_path[plen] = '/';
    memcpy(s_files_path + plen + 1, name_copy, nlen + 1);
    files_refresh();   /* список пересоздан, аргументы кнопок освободятся ниже */
}

/* Освобождение динамического аргумента при удалении кнопки (LV_EVENT_DELETE). */
static void arg_del_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void add_btn_with_arg(lv_obj_t *btn, const char *name, lv_event_cb_t cb)
{
    char *arg = lv_malloc(strlen(name) + 1);
    if (arg == NULL) return;
    strcpy(arg, name);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);
    lv_obj_add_event_cb(btn, arg_del_cb, LV_EVENT_DELETE, arg);
}

static void on_dir_up(lv_event_t *e)
{
    (void)e;
    char *slash = strrchr(s_files_path, '/');
    if (slash && strcmp(s_files_path, "/sdcard") != 0) {
        *slash = '\0';
        if (s_files_path[0] == '\0') snprintf(s_files_path, sizeof(s_files_path), "/sdcard");
        files_refresh();
    }
}

static void files_refresh(void)
{
    if (s_files_list) {
        lv_obj_t *old_list = s_files_list;
        s_files_list = NULL;
        lv_obj_del(old_list);
    }
    if (!s_files_title || !s_sd_ok) return;

    lv_label_set_text(s_files_title, s_files_path);

    s_files_list = ui_list_create(lv_screen_active());
    lv_obj_set_size(s_files_list, UI_W - 80, UI_H - 190);
    lv_obj_align(s_files_list, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* «..» — на уровень выше (кроме корня), всегда первой строкой */
    if (strcmp(s_files_path, "/sdcard") != 0) {
        lv_obj_t *btn = ui_list_add_button(s_files_list, LV_SYMBOL_LEFT, S[STR_UP][s_lang]);
        lv_obj_add_event_cb(btn, on_dir_up, LV_EVENT_CLICKED, NULL);
    }

    DIR *d = opendir(s_files_path);
    if (d == NULL) {
        lv_obj_t *btn = ui_list_add_button(s_files_list, LV_SYMBOL_WARNING, S[STR_EMPTY_DIR][s_lang]);
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char child[600];
        {
            size_t plen = strlen(s_files_path);
            size_t nlen = strlen(ent->d_name);
            if (plen + nlen + 2 >= sizeof(child)) continue;   /* слишком длинный путь */
            memcpy(child, s_files_path, plen);
            child[plen] = '/';
            memcpy(child + plen + 1, ent->d_name, nlen + 1);
        }
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            lv_obj_t *btn = ui_list_add_button(s_files_list, LV_SYMBOL_DIRECTORY, ent->d_name);
            add_btn_with_arg(btn, ent->d_name, on_dir_enter);
        } else if (name_is_playlist(ent->d_name)) {
            lv_obj_t *btn = ui_list_add_button(s_files_list, LV_SYMBOL_LIST, ent->d_name);
            add_btn_with_arg(btn, ent->d_name, on_playlist_open);
        } else if (name_is_audio(ent->d_name)) {
            lv_obj_t *btn = ui_list_add_button(s_files_list, LV_SYMBOL_PLAY, ent->d_name);
            add_btn_with_arg(btn, ent->d_name, on_file_open);
        }
        /* прочие файлы не показываем — в списке только музыка и папки */
    }
    closedir(d);
}

static void build_files(lv_obj_t *c)
{
    if (!sd_card_ready()) {
        s_sd_ok = false;
        lv_obj_t *lbl = lv_label_create(c);
        lv_obj_set_style_text_font(lbl, f_big(), 0);
        lv_label_set_text(lbl, S[STR_NO_CARD][s_lang]);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 60);
        s_files_title = NULL;
        return;
    }
    if (usb_msc_owner() == USB_MSC_OWNER_HOST) {
        /* Карта сейчас у ПК (USB MSC) — список недоступен до возврата. */
        s_sd_ok = false;
        lv_obj_t *lbl = lv_label_create(c);
        lv_obj_set_style_text_font(lbl, f_big(), 0);
        lv_label_set_text(lbl, S[STR_USB_HOST][s_lang]);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 60);
        s_files_title = NULL;
        return;
    }
    s_sd_ok = true;

    s_files_title = lv_label_create(c);
    lv_obj_set_style_text_font(s_files_title, f_norm(), 0);
    lv_obj_set_style_text_opa(s_files_title, LV_OPA_60, 0);
    lv_obj_align(s_files_title, LV_ALIGN_TOP_LEFT, 40, 24);

    s_files_play_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_files_play_lbl, f_norm(), 0);
    lv_obj_align(s_files_play_lbl, LV_ALIGN_TOP_RIGHT, -40, 24);

    files_refresh();
}

/* --- скан Wi-Fi на вкладке «Сеть» ------------------------------------------
 * Кнопка запускает асинхронный скан (wifi_connect.c); список обновляется
 * таймером; тап по сети — подставить SSID в поле, фокус на пароль. */
static void wifi_list_refresh(void);

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    wifi_scan_start();
}

static void on_wifi_ap_pick(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (ssid == NULL || s_ssid_ta == NULL) return;
    lv_textarea_set_text(s_ssid_ta, ssid);
    kb_hide();
}

static void wifi_arg_del_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void wifi_list_refresh(void)
{
    if (s_wifi_list) {
        lv_obj_t *old = s_wifi_list;
        s_wifi_list = NULL;
        lv_obj_del(old);
    }
    if (!s_wifi_scan_lbl) return;

    int state = wifi_scan_state();
    if (state == 1) {
        lv_label_set_text(s_wifi_scan_lbl, S[STR_SCANNING][s_lang]);
    } else if (state == 2) {
        int n = wifi_scan_count();
        lv_label_set_text_fmt(s_wifi_scan_lbl, "%s: %d", S[STR_SCAN][s_lang], n);
        wifi_scan_ack();
        if (n == 0) return;
        s_wifi_list = ui_list_create(lv_screen_active());
        lv_obj_set_size(s_wifi_list, 420, 300);
        lv_obj_align(s_wifi_list, LV_ALIGN_TOP_LEFT, 40, 260);
        for (int i = 0; i < n; i++) {
            const wifi_ap_record_t *ap = wifi_scan_get(i);
            if (ap == NULL) continue;
            char ssid[33] = "(?)";
            if (ap->ssid[0] != '\0') snprintf(ssid, sizeof(ssid), "%s", (const char *)ap->ssid);
            else snprintf(ssid, sizeof(ssid), "%s", S[STR_HIDDEN][s_lang]);
            lv_obj_t *btn = ui_list_add_button(s_wifi_list,
                (ap->authmode == WIFI_AUTH_OPEN) ? LV_SYMBOL_WIFI : LV_SYMBOL_LOCK,
                ssid);
            lv_obj_t *lbl = lv_obj_get_child(btn, 1);
            if (lbl != NULL) {
                lv_obj_set_style_text_font(lbl, f_norm(), 0);
            }
            /* RSSI в конце строки */
            lv_obj_t *rssi = lv_label_create(btn);
            lv_obj_set_style_text_font(rssi, f_norm(), 0);
            lv_label_set_text_fmt(rssi, "%d dBm", ap->rssi);
            lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, -8, 0);
            char *copy = lv_malloc(strlen(ssid) + 1);
            if (copy != NULL) {
                strcpy(copy, ssid);
                lv_obj_add_event_cb(btn, on_wifi_ap_pick, LV_EVENT_CLICKED, copy);
                lv_obj_add_event_cb(btn, wifi_arg_del_cb, LV_EVENT_DELETE, copy);
            }
        }
    } else {
        lv_label_set_text(s_wifi_scan_lbl, "");
    }
}

static void wifi_scan_tick(lv_timer_t *t)
{
    (void)t;
    static int s_last_state = 0;
    int st = wifi_scan_state();
    if (st != s_last_state) {
        s_last_state = st;
        wifi_list_refresh();
    }
}

static void build_network(lv_obj_t *c)
{
    /* --- Wi-Fi (слева) --- */
    lv_obj_t *l1 = lv_label_create(c);
    lv_obj_set_style_text_font(l1, f_norm(), 0);
    lv_label_set_text(l1, S[STR_WIFI_SSID][s_lang]);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 40, 40);

    s_ssid_ta = make_textarea(c, 40, 76, 420, false);
    if (s_hooks.get_wifi_ssid) {
        lv_textarea_set_text(s_ssid_ta, s_hooks.get_wifi_ssid());
    }

    lv_obj_t *l2 = lv_label_create(c);
    lv_obj_set_style_text_font(l2, f_norm(), 0);
    lv_label_set_text(l2, S[STR_PASSWORD][s_lang]);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 40, 140);

    s_pass_ta = make_textarea(c, 40, 176, 420, true);
    lv_textarea_set_placeholder_text(s_pass_ta,
        s_lang == UI_LANG_RU ? "(пусто = открытая сеть)" : "(empty = open network)");

    make_button(c, S[STR_CONNECT][s_lang], 40, 240, 220, on_wifi_apply);

    /* --- скан сетей: кнопка, подпись-статус, список результатов --- */
    s_wifi_scan_btn = make_button(c, S[STR_SCAN][s_lang], 280, 240, 180, on_wifi_scan);
    s_wifi_scan_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_wifi_scan_lbl, f_norm(), 0);
    lv_obj_set_style_text_opa(s_wifi_scan_lbl, LV_OPA_60, 0);
    lv_obj_align(s_wifi_scan_lbl, LV_ALIGN_TOP_LEFT, 480, 252);

    /* --- URL источника (справа) --- */
    lv_obj_t *l3 = lv_label_create(c);
    lv_obj_set_style_text_font(l3, f_norm(), 0);
    lv_label_set_text(l3, S[STR_SRC_URL][s_lang]);
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 520, 40);

    s_url_ta = make_textarea(c, 520, 76, 700, false);
    if (s_hooks.get_source) lv_textarea_set_text(s_url_ta, s_hooks.get_source());

    make_button(c, S[STR_APPLY][s_lang], 520, 140, 220, on_url_apply);

    lv_obj_t *hint = lv_label_create(c);
    lv_obj_set_style_text_font(hint, f_norm(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
    lv_label_set_text(hint,
        "SSID + пароль сохраняются в NVS, подключаются сразу.\n"
        "URL: файл с NAS/ПК или поток радио; применить — декодер перезапустится.");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 40, -24);
}

/* --- воспроизведение: кнопки и прогресс ----------------------------------- */

static void update_play_icons(void)
{
    if (s_pp_icon) {
        bool paused = decoder_paused();
        lv_label_set_text(s_pp_icon, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
    if (s_prev_btn) {
        bool file_track = playlist_active() &&
                          (strncmp(s_hooks.get_source ? s_hooks.get_source() : "", "file://", 7) == 0);
        if (file_track) lv_obj_clear_state(s_prev_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_prev_btn, LV_STATE_DISABLED);
    }
    if (s_next_btn) {
        /* доступен, если в очереди есть следующий трек (или очередь ещё не
         * построена для файла — main.c построит её лениво) */
        const char *src = s_hooks.get_source ? s_hooks.get_source() : "";
        bool file_track = strncmp(src, "file://", 7) == 0;
        int idx = playlist_index();
        bool has_next = file_track && (!playlist_active() || idx + 1 < playlist_count());
        if (has_next) lv_obj_clear_state(s_next_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_next_btn, LV_STATE_DISABLED);
    }
}

static void on_play_pause(lv_event_t *e)
{
    (void)e;
    if (s_hooks.play_pause) s_hooks.play_pause();
    update_play_icons();
}

static void on_next(lv_event_t *e)
{
    (void)e;
    if (s_hooks.next_track) s_hooks.next_track();
    update_play_icons();
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    if (s_hooks.prev_track) s_hooks.prev_track();
    update_play_icons();
}

/* обновление прогресса раз в 500 мс */
static void meta_tick(void)
{
    if (!s_title_lbl || !s_artist_lbl) return;
    meta_tags_t mt;
    meta_get(&mt);
    const char *title = mt.title[0] ? mt.title : "—";
    const char *artist = mt.artist[0] ? mt.artist : "";
    /* если в заголовке радио "Artist - Title" и artist пуст — разобрать */
    if (mt.artist[0] == '\0' && title[0] != '\0') {
        const char *dash = strstr(title, " - ");
        if (dash != NULL) {   /* одноразовый разбор — копия, не меняем мету */
            static char t_copy[META_TITLE_MAX], a_copy[META_ARTIST_MAX];
            strlcpy(t_copy, dash + 3, sizeof(t_copy));
            {
                size_t alen = (size_t)(dash - title);
                if (alen >= sizeof(a_copy)) alen = sizeof(a_copy) - 1;
                memcpy(a_copy, title, alen);
                a_copy[alen] = '\0';
            }
            title = t_copy;
            artist = a_copy;
        }
    }
    lv_label_set_text(s_title_lbl, title);
    lv_label_set_text(s_artist_lbl, artist);
}

/* Скраб: пока пользователь держит слайдер — показываем время в точке курсора
 * и НЕ перезаписываем позицию из декодера. Отпустил — decoder_seek(ms).
 * Рестарт конвейера занимает ~0.3-1 с: флаг s_seek_in_flight держит слайдер
 * на месте, пока декодер не выдаст первые данные. */
static void on_prog_slider(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *sl = lv_event_get_target(e);
    if (code == LV_EVENT_PRESSING) {
        s_scrubbing = true;
        uint32_t pos_ms = 0, total_ms = 0;
        bool seekable = false;
        decoder_progress_ext(&pos_ms, &total_ms, &seekable);
        if (total_ms > 0) {
            uint32_t want = (uint32_t)((uint64_t)lv_slider_get_value(sl) * total_ms / 1000);
            if (s_prog_lbl) lv_label_set_text_fmt(s_prog_lbl, "%s -> %u:%02u",
                                                 S[STR_TRACK][s_lang], (unsigned)(want / 60000), (unsigned)((want / 1000) % 60));
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_scrubbing = false;
        uint32_t pos_ms = 0, total_ms = 0;
        bool seekable = false;
        decoder_progress_ext(&pos_ms, &total_ms, &seekable);
        if (total_ms > 0) {
            uint32_t want = (uint32_t)((uint64_t)lv_slider_get_value(sl) * total_ms / 1000);
            ESP_LOGI(TAG, "перемотка: %u мс (из %u)", want, total_ms);
            s_seek_in_flight = decoder_seek(want) == ESP_OK;
        }
    }
}

/* мм:сс (или ч:мм:сс) */
static void fmt_time(char *buf, size_t cap, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(buf, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    } else {
        snprintf(buf, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

static void prog_tick(lv_timer_t *t)
{
    (void)t;
    update_play_icons();
    meta_tick();
    cover_tick();
    bl_idle_check();
    standby_check();

    /* P2-12: зеркало громкости — слайдер и подпись догоняют громкость,
     * изменённую с телефона/веб-пульта. Вставлено до раннего выхода для
     * радио (иначе слайдер не обновлялся бы, пока играет поток). Пока
     * слайдер держат пальцем (LV_STATE_PRESSED) — не трогаем. */
    if (s_vol_slider != NULL
        && (lv_obj_get_state(s_vol_slider) & LV_STATE_PRESSED) == 0) {
        float cur_db = s_hooks.get_volume_db ? s_hooks.get_volume_db() : -25.0f;
        if (slider_to_db(lv_slider_get_value(s_vol_slider)) != cur_db) {
            lv_slider_set_value(s_vol_slider, db_to_slider(cur_db), LV_ANIM_OFF);
            if (s_vol_lbl != NULL) {
                lv_label_set_text_fmt(s_vol_lbl, "%.1f dB", cur_db);
            }
        }
    }
    if (!s_prog_bar || !s_prog_lbl) return;

    const char *src = s_hooks.get_source ? s_hooks.get_source() : "";
    (void)src;
    bool is_stream = http_source_is_stream();
    if (is_stream) {
        lv_obj_set_hidden(s_prog_bar, true);
        lv_obj_set_hidden(s_prog_lbl, true);
        return;
    }
    uint32_t pos_ms = 0, total_ms = 0;
    bool seekable = false;
    decoder_progress_ext(&pos_ms, &total_ms, &seekable);

    if (total_ms == 0) {
        lv_obj_set_hidden(s_prog_bar, true);
        lv_obj_set_hidden(s_prog_lbl, true);
        return;
    }
    lv_obj_set_hidden(s_prog_bar, false);
    lv_obj_set_hidden(s_prog_lbl, false);

    if (s_scrubbing) {
        return;   /* пользователь держит слайдер — позицию не трогаем */
    }
    if (s_seek_in_flight) {
        if (!decoder_running()) return;      /* рестарт идёт — слайдер на месте */
        if (pos_ms > 0) s_seek_in_flight = false;   /* данные пошли */
        else return;
    }

    lv_slider_set_value(s_prog_bar, (int32_t)(((uint64_t)pos_ms * 1000) / total_ms), LV_ANIM_OFF);
    char pbuf[12], tbuf[12];
    fmt_time(pbuf, sizeof(pbuf), pos_ms);
    fmt_time(tbuf, sizeof(tbuf), total_ms);
    lv_label_set_text_fmt(s_prog_lbl, "%s / %s%s", pbuf, tbuf, seekable ? "" : "  (без перемотки)");
}

/* --- вкладка «Радио»: избранные станции ------------------------------------
 * Тап по станции — играть (set_source_url); долгий тап — удалить станцию.
 * Добавление: имя в поле (пусто = hostname из URL) + текущий источник. */
static void radio_refresh(void);

static void on_radio_station_delete(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (stations_remove(idx) == ESP_OK) {
        radio_refresh();
    }
}

static void on_radio_station_play(lv_event_t *e)
{
    const char *url = (const char *)lv_event_get_user_data(e);
    if (url == NULL) return;
    playlist_clear();                    /* радио вне очереди */
    if (s_hooks.set_source_url) s_hooks.set_source_url(url);
    if (s_src_lbl) lv_label_set_text(s_src_lbl, url);
}

static void on_radio_add(lv_event_t *e)
{
    (void)e;
    const char *src = s_hooks.get_source ? s_hooks.get_source() : "";
    if (src[0] == '\0' || (strncmp(src, "http://", 7) != 0 && strncmp(src, "https://", 8) != 0)) {
        ESP_LOGW(TAG, "%s", S[STR_ST_NOTHTTP][s_lang]);
        return;
    }
    const char *name = s_radio_name_ta ? lv_textarea_get_text(s_radio_name_ta) : "";
    kb_hide();
    stations_add(name[0] ? name : NULL, src);
    radio_refresh();
}

static void radio_arg_del_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void radio_refresh(void)
{
    if (!s_radio_list) return;
    lv_obj_t *old = s_radio_list;
    s_radio_list = NULL;
    lv_obj_del(old);

    s_radio_list = ui_list_create(lv_screen_active());
    lv_obj_set_size(s_radio_list, UI_W - 80, UI_H - 420);
    lv_obj_align(s_radio_list, LV_ALIGN_TOP_MID, 0, 200);

    int n = stations_count();
    if (n == 0) {
        lv_obj_t *btn = ui_list_add_button(s_radio_list, LV_SYMBOL_BELL, S[STR_ST_EMPTY][s_lang]);
        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), f_norm(), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0x808890), 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        const station_t *st = stations_get(i);
        if (st == NULL) continue;
        lv_obj_t *btn = ui_list_add_button(s_radio_list, LV_SYMBOL_BELL, st->name);
        lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), f_norm(), 0);
        char *url = lv_malloc(strlen(st->url) + 1);
        if (url != NULL) {
            strcpy(url, st->url);
            lv_obj_add_event_cb(btn, on_radio_station_play, LV_EVENT_CLICKED, url);
            lv_obj_add_event_cb(btn, radio_arg_del_cb, LV_EVENT_DELETE, url);
        }
        int *idx = lv_malloc(sizeof(int));
        if (idx != NULL) {
            *idx = i;
            lv_obj_add_event_cb(btn, on_radio_station_delete, LV_EVENT_LONG_PRESSED, idx);
            lv_obj_add_event_cb(btn, radio_arg_del_cb, LV_EVENT_DELETE, idx);
        }
    }
}

static void build_radio(lv_obj_t *c)
{
    if (!s_stations_loaded) {
        stations_load();
        s_stations_loaded = true;
    }

    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_text_font(t, f_big(), 0);
    lv_label_set_text(t, S[STR_RADIO][s_lang]);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *nt = lv_label_create(c);
    lv_obj_set_style_text_font(nt, f_norm(), 0);
    lv_label_set_text(nt, S[STR_ST_NAME][s_lang]);
    lv_obj_align(nt, LV_ALIGN_TOP_LEFT, 40, 90);

    s_radio_name_ta = make_textarea(c, 40, 126, 420, false);

    make_button(c, S[STR_ST_ADD][s_lang], 40, 186, 300, on_radio_add);

    radio_refresh();
}

/* --- построение экранов -------------------------------------------------- */
static void on_prog_slider(lv_event_t *e);   /* перемотка прогресс-слайдером */

static void build_home(lv_obj_t *c)
{
    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_text_font(t, f_big(), 0);
    lv_label_set_text(t, S[STR_VOLUME][s_lang]);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);

    s_vol_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_vol_lbl, f_huge(), 0);
    float db = s_hooks.get_volume_db ? s_hooks.get_volume_db() : -25.0f;
    lv_label_set_text_fmt(s_vol_lbl, "%.1f dB", db);
    lv_obj_align(s_vol_lbl, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *sl = lv_slider_create(c);
    lv_obj_set_size(sl, 700, 24);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 130);
    lv_slider_set_range(sl, 0, 255);
    lv_slider_set_value(sl, db_to_slider(db), LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, on_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);
    s_vol_slider = sl;   /* P2-12: зеркало громкости в prog_tick */

    lv_obj_t *mt = lv_label_create(c);
    lv_obj_set_style_text_font(mt, f_norm(), 0);
    lv_label_set_text(mt, S[STR_MUTE][s_lang]);
    lv_obj_align(mt, LV_ALIGN_TOP_MID, -100, 190);

    s_mute_sw = lv_switch_create(c);
    lv_obj_align(s_mute_sw, LV_ALIGN_TOP_MID, 60, 190);
    if (s_hooks.get_mute && s_hooks.get_mute()) {
        lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_mute_sw, on_mute_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- обложка альбома: слева от метаданных (появляется по мере разбора) --- */
    s_cover_img = lv_image_create(c);
    lv_obj_set_size(s_cover_img, COVER_WIDGET_SIZE, COVER_WIDGET_SIZE);
    lv_obj_align(s_cover_img, LV_ALIGN_TOP_LEFT, 40, 250);
    lv_image_set_inner_align(s_cover_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_hidden(s_cover_img, true);

    /* --- метаданные трека: название (крупно), исполнитель, источник ---
     * Правее обложки (левая граница 240, обложка до x=230). */
    s_title_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_title_lbl, f_big(), 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_lbl, UI_W - 480);
    lv_label_set_text(s_title_lbl, "—");
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, 244);

    s_artist_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_artist_lbl, f_norm(), 0);
    lv_obj_set_style_text_opa(s_artist_lbl, LV_OPA_80, 0);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_artist_lbl, UI_W - 480);
    lv_label_set_text(s_artist_lbl, "");
    lv_obj_align(s_artist_lbl, LV_ALIGN_TOP_MID, 0, 284);

    lv_obj_t *src_t = lv_label_create(c);
    lv_obj_set_style_text_font(src_t, f_norm(), 0);
    lv_obj_set_style_text_opa(src_t, LV_OPA_60, 0);
    lv_label_set_text(src_t, S[STR_SOURCE][s_lang]);
    lv_obj_align(src_t, LV_ALIGN_TOP_MID, 0, 314);

    s_src_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_src_lbl, f_norm(), 0);
    lv_obj_set_style_text_opa(s_src_lbl, LV_OPA_60, 0);
    lv_label_set_long_mode(s_src_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_src_lbl, UI_W - 480);
    lv_label_set_text(s_src_lbl, s_hooks.get_source ? s_hooks.get_source() : S[STR_NONE][s_lang]);
    lv_obj_align(s_src_lbl, LV_ALIGN_TOP_MID, 0, 340);

    /* --- управление воспроизведением: prev / play-pause / next --- */
    s_prev_btn = lv_btn_create(c);
    lv_obj_set_size(s_prev_btn, 88, 64);
    lv_obj_align(s_prev_btn, LV_ALIGN_TOP_MID, -160, 380);
    s_prev_icon = lv_label_create(s_prev_btn);
    lv_obj_set_style_text_font(s_prev_icon, f_huge(), 0);
    lv_label_set_text(s_prev_icon, LV_SYMBOL_PREV);
    lv_obj_center(s_prev_icon);
    lv_obj_add_event_cb(s_prev_btn, on_prev, LV_EVENT_CLICKED, NULL);

    s_pp_btn = lv_btn_create(c);
    lv_obj_set_size(s_pp_btn, 120, 64);
    lv_obj_align(s_pp_btn, LV_ALIGN_TOP_MID, 0, 380);
    s_pp_icon = lv_label_create(s_pp_btn);
    lv_obj_set_style_text_font(s_pp_icon, f_huge(), 0);
    lv_label_set_text(s_pp_icon, LV_SYMBOL_PLAY);
    lv_obj_center(s_pp_icon);
    lv_obj_add_event_cb(s_pp_btn, on_play_pause, LV_EVENT_CLICKED, NULL);

    s_next_btn = lv_btn_create(c);
    lv_obj_set_size(s_next_btn, 88, 64);
    lv_obj_align(s_next_btn, LV_ALIGN_TOP_MID, 160, 380);
    lv_obj_t *nxt_icon = lv_label_create(s_next_btn);
    lv_obj_set_style_text_font(nxt_icon, f_huge(), 0);
    lv_label_set_text(nxt_icon, LV_SYMBOL_NEXT);
    lv_obj_center(nxt_icon);
    lv_obj_add_event_cb(s_next_btn, on_next, LV_EVENT_CLICKED, NULL);

    /* прогресс файла: слайдер — тап/драг = перемотка (MP3/DSD) */
    s_prog_bar = lv_slider_create(c);
    lv_obj_set_size(s_prog_bar, 700, 26);
    lv_obj_align(s_prog_bar, LV_ALIGN_TOP_MID, 0, 494);
    lv_slider_set_range(s_prog_bar, 0, 1000);
    lv_obj_add_event_cb(s_prog_bar, on_prog_slider, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_prog_bar, on_prog_slider, LV_EVENT_RELEASED, NULL);
    s_prog_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_prog_lbl, f_norm(), 0);
    lv_obj_set_style_text_opa(s_prog_lbl, LV_OPA_60, 0);
    lv_label_set_text(s_prog_lbl, "");
    lv_obj_align(s_prog_lbl, LV_ALIGN_TOP_MID, 0, 524);
}

static void build_filters(lv_obj_t *c)
{
    lv_obj_t *t1 = lv_label_create(c);
    lv_obj_set_style_text_font(t1, f_norm(), 0);
    lv_label_set_text(t1, S[STR_REG88][s_lang]);
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 40, 40);

    s_reg_dd = lv_dropdown_create(c);
    /* Соответствие значений Reg 88 [2:0] — см. даташит ES9039Q2M (Filter Shape). */
    lv_dropdown_set_options(s_reg_dd,
        "Preset 0\nPreset 1\nPreset 2\nPreset 3\n"
        "Preset 4\nPreset 5\nPreset 6\nPreset 7");
    lv_dropdown_set_selected(s_reg_dd, s_hooks.get_filter ? s_hooks.get_filter() : 0);
    lv_obj_set_size(s_reg_dd, 280, 44);
    lv_obj_align(s_reg_dd, LV_ALIGN_TOP_LEFT, 40, 76);
    lv_obj_add_event_cb(s_reg_dd, on_reg_dd, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *t2 = lv_label_create(c);
    lv_obj_set_style_text_font(t2, f_norm(), 0);
    lv_label_set_text(t2, S[STR_CUSTOM_FIR][s_lang]);
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 40, 150);

    s_fir_dd = lv_dropdown_create(c);
    lv_dropdown_set_options(s_fir_dd,
        "Auto (per stream)\nBuilt-in (Reg 88)\nC-BRICK 44.1k\nC-BRICK 48k\nC-BRICK 96k\n"
        "C-SLOW 44.1k\nC-SLOW 48k\nC-SLOW 96k");
    lv_dropdown_set_selected(s_fir_dd, s_hooks.get_fir ? s_hooks.get_fir() : 0);
    lv_obj_set_size(s_fir_dd, 280, 44);
    lv_obj_align(s_fir_dd, LV_ALIGN_TOP_LEFT, 40, 186);
    lv_obj_add_event_cb(s_fir_dd, on_fir_dd, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint = lv_label_create(c);
    lv_obj_set_style_text_font(hint, f_norm(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
    lv_label_set_text(hint,
        "C-BRICK: brickwall, transition 20-24 kHz (pre+post ringing)\n"
        "C-SLOW:  short impulse, soft roll-off");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 40, -24);
}

static void build_diag(lv_obj_t *c)
{
    s_diag_lbl = lv_label_create(c);
    lv_obj_set_style_text_font(s_diag_lbl, f_norm(), 0);
    lv_label_set_long_mode(s_diag_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_diag_lbl, UI_W - 80);
    lv_obj_align(s_diag_lbl, LV_ALIGN_TOP_LEFT, 40, 40);
    diag_tick(NULL);
}

static void on_standby_dd(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    standby_save((int)lv_dropdown_get_selected(dd));
}

static void on_bl_timeout_dd(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    bl_timeout_save((int)lv_dropdown_get_selected(dd));
}

static void build_settings(lv_obj_t *c)
{
    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_text_font(t, f_norm(), 0);
    lv_label_set_text(t, S[STR_LANGUAGE][s_lang]);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 40, 40);

    s_lang_dd = lv_dropdown_create(c);
    lv_dropdown_set_options(s_lang_dd, "English\nРусский");
    lv_dropdown_set_selected(s_lang_dd, (int)s_lang);
    lv_obj_set_size(s_lang_dd, 220, 44);
    lv_obj_align(s_lang_dd, LV_ALIGN_TOP_LEFT, 40, 76);
    lv_obj_add_event_cb(s_lang_dd, on_lang_dd, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *b = lv_label_create(c);
    lv_obj_set_style_text_font(b, f_norm(), 0);
    lv_label_set_text(b, S[STR_BRIGHTNESS][s_lang]);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 40, 150);

    lv_obj_t *sl = lv_slider_create(c);
    lv_obj_set_size(sl, 400, 24);
    lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 40, 186);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, ui_display_brightness_get(), LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, on_br_slider, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- автотушение подсветки: таймаут неактивности --- */
    lv_obj_t *t2 = lv_label_create(c);
    lv_obj_set_style_text_font(t2, f_norm(), 0);
    lv_label_set_text(t2, S[STR_BL_TIMEOUT][s_lang]);
    lv_obj_align(t2, LV_ALIGN_TOP_LEFT, 40, 250);

    s_bl_to_dd = lv_dropdown_create(c);
    lv_dropdown_set_options(s_bl_to_dd,
        "Never\n1 minute\n5 minutes\n15 minutes");
    int idx = 0;
    for (int i = 0; i < (int)(sizeof(s_bl_options_ms) / sizeof(s_bl_options_ms[0])); i++) {
        if (s_bl_options_ms[i] == s_idle_timeout_ms) idx = i;
    }
    lv_dropdown_set_selected(s_bl_to_dd, idx);
    lv_obj_set_size(s_bl_to_dd, 220, 44);
    lv_obj_align(s_bl_to_dd, LV_ALIGN_TOP_LEFT, 40, 286);
    lv_obj_add_event_cb(s_bl_to_dd, on_bl_timeout_dd, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- спящий режим: панель Sleep In + Wi-Fi off --- */
    lv_obj_t *st = lv_label_create(c);
    lv_obj_set_style_text_font(st, f_norm(), 0);
    lv_label_set_text(st, S[STR_STANDBY][s_lang]);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 40, 336);

    s_standby_dd = lv_dropdown_create(c);
    lv_dropdown_set_options(s_standby_dd, "Never\n5 minutes\n15 minutes\n30 minutes");
    int sidx = 0;
    for (int i = 0; i < (int)(sizeof(s_standby_options_ms) / sizeof(s_standby_options_ms[0])); i++) {
        if (s_standby_options_ms[i] == s_standby_timeout_ms) sidx = i;
    }
    lv_dropdown_set_selected(s_standby_dd, sidx);
    lv_obj_set_size(s_standby_dd, 220, 44);
    lv_obj_align(s_standby_dd, LV_ALIGN_TOP_LEFT, 40, 372);
    lv_obj_add_event_cb(s_standby_dd, on_standby_dd, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint = lv_label_create(c);
    lv_obj_set_style_text_font(hint, f_norm(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
    lv_label_set_text(hint,
        "0% выкл, 100% макс. Тайм-аут — гашение без касаний (tap будит).");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 40, -24);
}

static void ui_rebuild(void)
{
    lv_obj_t *root = lv_screen_active();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *tv = lv_tabview_create(root);
    lv_obj_set_size(tv, UI_W, UI_H);
    lv_tabview_set_tab_bar_size(tv, 56);

    build_home(lv_tabview_add_tab(tv, S[STR_HOME][s_lang]));
    build_radio(lv_tabview_add_tab(tv, S[STR_RADIO][s_lang]));
    build_filters(lv_tabview_add_tab(tv, S[STR_FILTERS][s_lang]));
    build_diag(lv_tabview_add_tab(tv, S[STR_DIAG][s_lang]));
    build_settings(lv_tabview_add_tab(tv, S[STR_SETTINGS][s_lang]));
    build_network(lv_tabview_add_tab(tv, S[STR_NETWORK][s_lang]));
    build_files(lv_tabview_add_tab(tv, S[STR_FILES][s_lang]));
}

/* --- публичный API ------------------------------------------------------- */
void ui_set_lang(ui_lang_t lang)
{
    if (lang != s_lang) {
        s_lang = lang;
        lvgl_port_lock(0);
        ui_rebuild();
        lvgl_port_unlock();
    }
}

ui_lang_t ui_get_lang(void)
{
    return s_lang;
}

esp_err_t ui_init(i2c_master_bus_handle_t bus, const ui_hooks_t *hooks)
{
    if (hooks) s_hooks = *hooks;
    s_i2c_bus = bus;

    bl_timeout_load();
    standby_load();
    ESP_RETURN_ON_ERROR(ui_display_init(), TAG, "display init failed");
    /* стартовый уровень: сохранённый, иначе 80% */
    int bl_pct = 80;
    {
        nvs_handle_t h;
        if (nvs_open("dac", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(h, "bl_pct", &v) == ESP_OK) bl_pct = v;
            nvs_close(h);
        }
    }
    ui_display_brightness_set(bl_pct);
    /* сразу после старта автотушение не должно погасить экран: последнее
     * «действие» пользователя = инициализация; LVGL сбрасывает неактивность
     * только по вводу — тайм-аут стартует от загрузки, это ок. */

    /* Тач на ту же I2C-шину */
    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t terr = ui_touch_init(bus, &tp);
    if (terr != ESP_OK) {
        ESP_LOGW(TAG, "тач не поднялся (%s) — UI будет работать без касаний",
                 esp_err_to_name(terr));
    } else {
        s_touch_ok = true;
        const lvgl_port_touch_cfg_t tcfg = {
            .disp = ui_display_get(),
            .handle = tp,
        };
        if (lvgl_port_add_touch(&tcfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed");
        }
    }

    lvgl_port_lock(0);
    ui_rebuild();
    if (!s_diag_timer_done) {
        lv_timer_create(diag_tick, 1000, NULL);
        s_diag_timer_done = true;
    }
    if (!s_prog_timer_done) {
        lv_timer_create(prog_tick, 500, NULL);
        s_prog_timer_done = true;
    }
    lv_timer_create(wifi_scan_tick, 500, NULL);
    lvgl_port_unlock();
    return ESP_OK;
}
