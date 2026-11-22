/*
 * ============================================================================
 *  main.c — точка входа: NVS -> I²C -> I²S -> ЦАП -> Wi-Fi -> декодер
 *  ESP-IDF 6.1
 *
 *  Порядок запуска:
 *    1) NVS — обязателен до Wi-Fi (креды хранятся в NVS);
 *    2) шина I²C + устройство ES9039Q2M (0x48); опционально — es9039_scan_bus()
 *       для подтверждения перепайки платы в SW-режим;
 *    3) i2s_out_init() — I²S-мастер стартует на 44,1 кГц. До или после
 *       es9039_init() — не принципиально: DAC в ASYNC slave ресемплирует на
 *       свой локальный MCLK, главное — запустить ДО decoder_start(), чтобы
 *       к началу звука BCLK/LRCK уже шли;
 *    4) es9039_init(s_dac_dev, GPIO_NUM_4) — GPIO1 платы -> automute;
 *    5) Wi-Fi (ESP-Hosted через ESP32-C6);
 *    6) decoder_start(...) — файл с NAS/ПК или интернет-радио.
 *
 *  Смена частоты потока (44.1/48/96 кГц) обрабатывается автоматически:
 *  decoder.c -> es9039_switch_frequency(..., fs) -> пересборка BCLK/LRCK
 *  (i2s_out_apply_fs) -> заливка FIR-набора (es9039_fir.c) -> SRC lock -> unmute.
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "es9039q2m_esp32p4_driver.h"
#include "es9039_fir.h"
#include "sd_card.h"
#include "usb_msc.h"
#include "playlist.h"
#include "ui.h"
#include "dsd_native.h"
#include "dsd_dec.h"
#include "ota_upd.h"

static const char *TAG = "main";

/* Пины I²C борта Waveshare ESP32-P4-WIFI6: SDA=GPIO7, SCL=GPIO8.
 * На этой же шине сидит тач дисплея GT911 (0x5D/0x14) — см. ui_touch_init(). */
#define MAIN_I2C_SDA_GPIO   GPIO_NUM_7
#define MAIN_I2C_SCL_GPIO   GPIO_NUM_8
#define MAIN_I2C_FREQ_HZ    100000   /* после отладки можно 400000 — FIR заливается быстрее */

/* Пин ESP32-P4, подключённый к GPIO1 платы DAC (Automute Status). */
#define MAIN_DAC_GPIO1_PIN  GPIO_NUM_4

static i2c_master_dev_handle_t s_dac_dev;

/* USB MSC: что играло, когда ПК забрал карту; возобновляем при возврате. */
static bool s_was_file_playing;   /* декодер играл file:// в момент передачи ПК */

/* --- Сохранённые настройки: ключи и дефолты NVS --------------------------- */
#define PERSIST_NVS_NAMESPACE   "dac"
#define PERSIST_SAVE_DELAY_US   (3ULL * 1000000ULL)
#define VOL_NVS_KEY             "vol_half_db"
#define FLT_NVS_KEY             "flt_shape"
#define FIR_NVS_KEY             "fir_sel"
#define DSD_MODE_NVS_KEY        "dsd_mode"
#define VOL_DEFAULT_DB          (-25.0f)
#define FLT_DEFAULT_SHAPE       7   /* как в es9039_init(): MP slow low disp */
#define FIR_DEFAULT_SEL         0   /* авто: набор под поток */
/* Вводимые из UI (вкладка «Сеть»), сохраняются сразу при «Применить»:
 * SSID/пароль Wi-Fi и URL источника. Пароль хранится в NVS открытым текстом —
 * это домашний плеер; если это неприемлемо — сотрите WIFI_PASS_NVS_KEY. */
#define WIFI_SSID_NVS_KEY       "wifi_ssid"
#define WIFI_PASS_NVS_KEY       "wifi_pass"
#define SRC_URL_NVS_KEY         "src_url"

/* --- Хуки для графической оболочки (ui.c) --- */
/* URL источника: дефолт-заглушка; при старте перезаписывается из NVS
 * (SRC_URL_NVS_KEY), из UI (вкладка «Сеть») меняется на лету. */
static char s_url[512] = "http://192.168.1.50/music/track.flac";

/* Выбранный пункт дропдауна FIR: 0=авто (под поток), 1=built-in, 2..7 кастомные.
 * Выбранный штатный пресет фильтра (Reg 88): 0..7.
 * Оба хранятся для восстановления после пересборки UI (смена языка)
 * и загружаются из NVS при старте (см. persist_load_u8). */
static int s_fir_sel = 0;
static int s_filter_sel = FLT_DEFAULT_SHAPE;

/* Применение выбора фильтра/FIR без планирования записи в NVS
 * (используется и при восстановлении сохранённого выбора при старте). */
static void flt_apply_by_index(int preset)
{
    (void)es9039_set_filter((es9039_fir_t)preset);
}

static void fir_apply_by_index(int fir)
{
    if (fir == 0) {
        es9039_fir_set_manual_mode(false);
    } else {
        es9039_fir_set_manual_mode(true);
        if (fir == 1) {
            (void)es9039_fir_use_builtin();
        } else {
            const es9039_fir_set_t *set = &es9039_fir_sets[fir - 2];
            esp_err_t e = es9039_fir_apply(set, true);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "FIR '%s' не загружен: %s", set->name, esp_err_to_name(e));
            }
        }
    }
}

/* Текущая громкость, дБ. Стартовое значение соответствует тому, что
 * es9039_init() записывает в Reg 74/75 (0x50 = -40 дБ, безопасный старт);
 * дальше vol_load_db() подменяет его сохранённым в NVS (см. ниже). */
static float s_vol_db = -40.0f;

/* --- Сохранение настроек в NVS (громкость, фильтр, FIR) --------------------
 * Слайдеры и дропдауны генерируют десятки событий за одно движение — писать в
 * NVS на каждое нельзя (износ флеш-страниц). Пишем изменённое один раз через
 * 3 с после последнего изменения (отложенная запись через esp_timer).
 * Громкость — в шагах 0.5 дБ (0..255, то же кодирование, что Reg 74/75);
 * фильтр — 0..7 (Reg 88 FILTER_SHAPE); FIR — 0..7 (пункт дропдауна). */
static esp_timer_handle_t s_persist_timer;
static bool s_vol_dirty, s_flt_dirty, s_fir_dirty;

static void persist_save_cb(void *arg)
{
    nvs_handle_t h;
    if (nvs_open(PERSIST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_vol_dirty) {
        int16_t steps = (int16_t)(-s_vol_db / 0.5f + 0.5f);
        if (steps < 0) steps = 0;
        if (steps > 255) steps = 255;
        if (nvs_set_i16(h, VOL_NVS_KEY, steps) == ESP_OK) s_vol_dirty = false;
    }
    if (s_flt_dirty) {
        if (nvs_set_u8(h, FLT_NVS_KEY, (uint8_t)s_filter_sel) == ESP_OK) s_flt_dirty = false;
    }
    if (s_fir_dirty) {
        if (nvs_set_u8(h, FIR_NVS_KEY, (uint8_t)s_fir_sel) == ESP_OK) s_fir_dirty = false;
    }
    nvs_commit(h);
    nvs_close(h);
}

static void persist_schedule(void)
{
    if (s_persist_timer == NULL) {
        const esp_timer_create_args_t tcfg = {
            .callback = persist_save_cb,
            .name = "persist_save",
        };
        if (esp_timer_create(&tcfg, &s_persist_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_persist_timer);
    esp_timer_start_once(s_persist_timer, PERSIST_SAVE_DELAY_US);
}

/* Строковый ключ: запись сразу (ввод завершён кнопкой — не слайдер, износ
 * минимален) и чтение. Вызывать после init_nvs(). */
static void persist_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(PERSIST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, key, val ? val : "") == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool persist_load_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(PERSIST_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = cap;
        if (nvs_get_str(h, key, out, &len) == ESP_OK) ok = true;
        nvs_close(h);
    }
    return ok;
}

/* Чтение u8-ключа с проверкой диапазона: нет ключа или вне диапазона — дефолт.
 * Вызывать после init_nvs(). */
static uint8_t persist_load_u8(const char *key, uint8_t def, uint8_t lo, uint8_t hi)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open(PERSIST_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return (v < lo || v > hi) ? def : v;
}

/* Чтение сохранённой громкости (вызывать после init_nvs()). Нет ключа или
 * некорректное значение — дефолт -25 дБ. */
static float vol_load_db(void)
{
    nvs_handle_t h;
    int16_t steps = INT16_MIN;   /* маркер «нет ключа» */
    if (nvs_open(PERSIST_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i16(h, VOL_NVS_KEY, &steps);
        nvs_close(h);
    }
    if (steps < 0 || steps > 255) return VOL_DEFAULT_DB;
    return -(float)steps * 0.5f;
}

static void vol_apply(float db)
{
    s_vol_db = db;
    (void)es9039_set_volume_db(db);
}

/* decoder_request объявлен в audio_pipeline.h (P4b: перенесён из extern) */

/* --- Мьютекс хуков UI/web: хуки зовутся из LVGL-задачи и из httpd ----------
 * Гонка декодера (двойной start/переключение) закрыта мьютексом в decoder.c;
 * здесь защищаем хуки, меняющие состояние ЦАП/NVS: громкость, mute, фильтры,
 * Wi-Fi. Долгих операций под мьютексом нет (persist — отложенный таймер). */
static SemaphoreHandle_t s_hooks_mutex = NULL;
static bool hook_lock(void)
{
    if (s_hooks_mutex == NULL) s_hooks_mutex = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_hooks_mutex, pdMS_TO_TICKS(200)) == pdTRUE;
}
static void hook_unlock(void)
{
    if (s_hooks_mutex != NULL) xSemaphoreGive(s_hooks_mutex);
}

static void ui_h_set_volume(float db)
{
    if (!hook_lock()) return;
    vol_apply(db);
    s_vol_dirty = true;
    persist_schedule();   /* отложенная запись — см. комментарий выше */
    hook_unlock();
}
/* Mute не сохраняется в NVS (специально): случайный mute не должен «прилипать»
 * после перезапуска. Но состояние держим в ОЗУ — чтобы UI после пересборки
 * (смена языка) показывал актуальное. */
static bool s_mute_state = false;

static void ui_h_set_mute(bool on)         { if (hook_lock()) { s_mute_state = on; (void)es9039_mute(on); hook_unlock(); } }

/* Громкость/mute пользователя для fade декодера (P4b/P4-20): чтение под
 * мьютексом хуков; при занятом мьютексе — чтение без блокировки (гонка на
 * float безвредна: vol_apply меняет значение одной записью). */
float audio_user_volume_db(void)
{
    if (hook_lock()) {
        float db = s_vol_db;
        hook_unlock();
        return db;
    }
    return s_vol_db;   /* фолбэк без блокировки */
}

bool audio_user_muted(void)
{
    if (hook_lock()) {
        bool on = s_mute_state;
        hook_unlock();
        return on;
    }
    return s_mute_state;   /* фолбэк без блокировки */
}
static void ui_h_set_filter(int preset)
{
    if (!hook_lock()) return;
    s_filter_sel = preset;
    flt_apply_by_index(preset);
    s_flt_dirty = true;
    persist_schedule();
    hook_unlock();
}
/* Wi-Fi: сохранить креды в NVS и применить сразу (wifi_set_credentials
 * при уже поднятом стеке сам переподключается). */
static void ui_h_set_wifi(const char *ssid, const char *pass)
{
    if (!hook_lock()) return;
    persist_save_str(WIFI_SSID_NVS_KEY, ssid ? ssid : "");
    persist_save_str(WIFI_PASS_NVS_KEY, pass ? pass : "");
    esp_err_t e = wifi_set_credentials(ssid, pass);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "wifi_set_credentials(%s): %s", ssid ? ssid : "?", esp_err_to_name(e));
    }
    hook_unlock();
}

static const char *ui_h_get_wifi_ssid(void) { return wifi_get_ssid(); }

/* URL источника: сохранить в NVS и перезапустить декодер на лету.
 * decoder_request() асинхронен: не ждёт выхода задачи декодера и не блокирует
 * httpd/UI (коннект HTTP нового источника поднимается в задаче декодера). */
static void ui_h_set_source_url(const char *url)
{
    if (!url || strlen(url) == 0) return;
    snprintf(s_url, sizeof(s_url), "%s", url);
    persist_save_str(SRC_URL_NVS_KEY, s_url);
    /* Радио/HTTP-файл вне очереди: без сброса авто-переход мог бы утащить
     * воспроизведение обратно в плейлист после обрыва потока. */
    if (strncmp(url, "file://", 7) != 0) {
        playlist_clear();
        if (wifi_is_off()) {
            wifi_on();    /* спящий режим: радио включило Wi-Fi */
        }
    }
    esp_err_t e = decoder_request(s_url, DEC_TYPE_AUTO, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "decoder_request(%s): %s", s_url, esp_err_to_name(e));
    }
}

static void ui_h_set_fir(int fir)
{
    /* fir: 0 — авто: набор под частоту потока, заливается при каждой смене fs;
     * 1 — встроенный пресет (Reg 88), закреплён вручную;
     * 2..7 — кастомные наборы из es9039_fir_sets[] (порядок совпадает с
     * дропдауном: 2..4 C-BRICK, 5..7 C-SLOW).
     * Ручной выбор (1..7) приоритетнее авто: es9039_fir_apply_for_fs() при
     * смене частоты потока его не перекрывает (см. es9039_fir.h).
     * verify=true: контрольная перечитка RAM при ручном переключении; заливка
     * без mute — если играет звук, возможен короткий глитч. */
    if (!hook_lock()) return;
    s_fir_sel = fir;
    fir_apply_by_index(fir);
    s_fir_dirty = true;
    persist_schedule();
    hook_unlock();
}

/* --- Управление воспроизведением: плейлист, next/prev, авто-переход ------- */

/* Запустить трек из очереди (путь в очереди — без префикса file://). */
static void play_current_track(void)
{
    const char *path = playlist_current();
    if (path == NULL) return;
    snprintf(s_url, sizeof(s_url), "file://%.504s", path);
    persist_save_str(SRC_URL_NVS_KEY, s_url);
    esp_err_t e = decoder_request(s_url, DEC_TYPE_AUTO, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "decoder_request(%s): %s", s_url, esp_err_to_name(e));
    }
}

/* Ленивое построение очереди: папка текущего file:// трека, индекс — на него.
 * Позволяет next/prev по любой папке без явного .m3u. */
static void ensure_playlist_for_current(void)
{
    if (playlist_active()) return;
    if (strncmp(s_url, "file://", 7) != 0) return;
    char path[512];
    snprintf(path, sizeof(path), "%s", s_url + 7);
    char *slash = strrchr(path, '/');
    if (slash == NULL) return;
    *slash = '\0';
    const char *fname = slash + 1;
    if (fname[0] == '\0') return;
    (void)playlist_load_dir(path, fname);
}

/* Конец трека: авто-переход к следующему в очереди (плейлист или папка).
 * Для одиночного файла без очереди очередь строится лениво из папки. */
static void on_decoder_finished(void)
{
    if (!playlist_active()) {
        ensure_playlist_for_current();
        if (!playlist_active()) {
            ESP_LOGI(TAG, "Трек закончился, очередь пуста — остановка");
            return;
        }
        /* индекс указывает на только что отыгранный трек */
        if (!playlist_next()) {
            playlist_clear();
            return;
        }
    }
    if (playlist_next()) {
        play_current_track();
    } else {
        playlist_clear();
        ESP_LOGI(TAG, "Очередь закончилась");
    }
}

static void ui_h_play_pause(void)
{
    if (decoder_running()) {
        if (decoder_paused()) {
            decoder_resume();
        } else {
            decoder_pause();
        }
        return;
    }
    if (s_url[0] == '\0') return;
    esp_err_t e = decoder_start(s_url, DEC_TYPE_AUTO, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "decoder_start(%s): %s", s_url, esp_err_to_name(e));
    }
}

static void ui_h_next_track(void)
{
    if (strncmp(s_url, "file://", 7) != 0) return;   /* для радио не применяется */
    ensure_playlist_for_current();
    if (playlist_next()) {
        play_current_track();
    } else {
        ESP_LOGI(TAG, "Дальше в очереди нет треков");
    }
}

static void ui_h_prev_track(void)
{
    if (strncmp(s_url, "file://", 7) != 0) return;
    ensure_playlist_for_current();
    if (playlist_prev()) {
        play_current_track();
    } else if (playlist_active()) {
        /* начало очереди — перезапуск текущего трека (стандартное поведение) */
        decoder_request(s_url, DEC_TYPE_AUTO, false);
    }
}

static const char *ui_h_get_source(void)   { return s_url; }
static float ui_h_get_volume(void)         { return s_vol_db; }
static bool ui_h_get_mute(void)            { return s_mute_state; }
static int ui_h_get_filter(void)           { return s_filter_sel; }
static int ui_h_get_fir(void)              { return s_fir_sel; }
/* Частота генератора NDK NZ2520SDA на плате DAC — источник SYS_CLK ЦАПа.
 * Нужна es9039_diag_read() для расчёта fs из Reg 239 (AUTO FS READ). */
#define DAC_MCLK_HZ  49152000

/* Диагностическая строка для вкладки «Диагностика»: всё из readback-регистров
 * ЦАПа (es9039_diag_read), язык — по текущему выбору в UI. */
static void ui_h_get_diag(char *buf, size_t len)
{
    bool ru = (ui_get_lang() == UI_LANG_RU);

    es9039_diag_t d;
    esp_err_t e = es9039_diag_read(&d, DAC_MCLK_HZ);
    if (e != ESP_OK) {
        snprintf(buf, len, ru ? "ES9039Q2M: ошибка I2C (%s)"
                              : "ES9039Q2M: I2C error (%s)",
                 esp_err_to_name(e));
        return;
    }

    /* Дополнительно: режим FIR (Reg 135 bit0 PROG_COEFF_EN) и активный
     * штатный пресет (Reg 88 [2:0]) — видно сразу, применился ли кастомный
     * набор и какой пресет остаётся при его отсутствии. */
    uint8_t fir_ctrl = 0, shape = 0;
    (void)es9039_read_reg(0x87, &fir_ctrl);   /* Reg 135: bit0 PROG_COEFF_EN */
    (void)es9039_read_reg(0x58, &shape);      /* Reg 88:  [2:0] FILTER_SHAPE */

    static const char *const in_n[4][2] = {
        { "PCM", "PCM" }, { "DSD", "DSD" }, { "DoP", "DoP" }, { "S/PDIF", "S/PDIF" }
    };

    /* fs из Reg 239: IDAC_DIV_AUTO; без SRC lock — оценить нельзя */
    char fs[20];
    if (d.ratio_valid && d.fs_estimate_hz) {
        snprintf(fs, sizeof(fs), "%lu.%02lu %s",
                 (unsigned long)(d.fs_estimate_hz / 1000),
                 (unsigned long)((d.fs_estimate_hz % 1000) / 10),
                 ru ? "кГц" : "kHz");
    } else {
        snprintf(fs, sizeof(fs), "%s", ru ? "н/д" : "n/a");
    }

    snprintf(buf, len,
             ru ? "ES9039Q2M: чип 0x%02X | SRC lock: %s | fs: %s\n"
                  "Вход: %s | BCK/WS: %s | DoP: %s | S/PDIF: %s\n"
                  "Auto-mute: %s | Vol-min: %s | Ramp: %s\n"
                  "FIR: %s | Shape: %u"
                : "ES9039Q2M: chip 0x%02X | SRC lock: %s | fs: %s\n"
                  "Input: %s | BCK/WS: %s | DoP: %s | S/PDIF: %s\n"
                  "Auto-mute: %s | Vol-min: %s | Ramp: %s\n"
                  "FIR: %s | Shape: %u",
             d.chip_id,
             d.ratio_valid ? (ru ? "да" : "yes") : (ru ? "нет" : "no"),
             fs,
             in_n[d.input_select & 3][ru],
             d.bck_ws_fail ? (ru ? "сбой" : "fail") : (ru ? "ок" : "ok"),
             d.dop_valid ? (ru ? "да" : "yes") : "—",
             d.spdif_valid ? (ru ? "да" : "yes") : "—",
             d.automute_read ? (ru ? "да" : "yes") : (ru ? "нет" : "no"),
             d.vol_min_state ? (ru ? "да" : "yes") : (ru ? "нет" : "no"),
             d.soft_ramping ? (ru ? "идёт" : "ramping") : (ru ? "нет" : "off"),
             (fir_ctrl & 0x01) ? (ru ? "кастом" : "custom")
                               : (ru ? "встроенный" : "built-in"),
             (unsigned)shape & 7);

    /* Статус USB MSC (карта у плеера или у ПК) — дописываем в конец. */
    char usb_line[48];
    usb_msc_status_text(usb_line, sizeof(usb_line));
    size_t used = strnlen(buf, len - 1);
    snprintf(buf + used, len - used, "\n%s", usb_line);
}

/* --- USB MSC: карта переезжает между плеером и ПК -------------------------
 * on_take_by_host вызывается в контексте задачи TinyUSB: decoder_stop ждёт до
 * 2 с — переключение чуть замедлится, это норма. Возобновление — по факту
 * возврата карты ( FatFS снова смонтирована). */
static void msc_take_by_host(void)
{
    if (strncmp(s_url, "file://", 7) != 0) return;
    if (decoder_running()) {
        s_was_file_playing = true;
        decoder_stop();
        ESP_LOGW(TAG, "Воспроизведение остановлено: карта передана ПК");
    }
}

static void msc_return_to_app(void)
{
    if (s_was_file_playing && strncmp(s_url, "file://", 7) == 0
        && !decoder_running()) {
        ESP_LOGI(TAG, "Возобновляю воспроизведение: %s", s_url);
        esp_err_t e = decoder_start(s_url, DEC_TYPE_AUTO, false);
        s_was_file_playing = (e == ESP_OK);
    }
}

static const ui_hooks_t s_ui_hooks = {
    .set_volume_db = ui_h_set_volume,
    .set_mute      = ui_h_set_mute,
    .set_filter    = ui_h_set_filter,
    .set_fir       = ui_h_set_fir,
    .set_wifi      = ui_h_set_wifi,
    .set_source_url= ui_h_set_source_url,
    .get_source    = ui_h_get_source,
    .get_wifi_ssid = ui_h_get_wifi_ssid,
    .get_diag      = ui_h_get_diag,
    .get_volume_db = ui_h_get_volume,
    .get_mute      = ui_h_get_mute,
    .get_filter    = ui_h_get_filter,
    .get_fir       = ui_h_get_fir,
    .play_pause    = ui_h_play_pause,
    .next_track    = ui_h_next_track,
    .prev_track    = ui_h_prev_track,
};

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

void app_main(void)
{
    init_nvs();
    ESP_LOGI(TAG, "audio_dac_p4: старт");

    /* --- I²C-шина + устройство ЦАПа --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,   /* автоподбор порта */
        .sda_io_num = MAIN_I2C_SDA_GPIO,
        .scl_io_num = MAIN_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* при длинных линиях — внешние подтяжки */
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* Опционально, но полезно при первом запуске: 0x48 должен отвечать.
     * Если нет — плата не перепаяна в SW-режим или пины не те. */
    (void)es9039_scan_bus(bus);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES9039_I2C_ADDR_7BIT,
        .scl_speed_hz = MAIN_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dac_dev));

    /* --- I²S-мастер (до или после es9039_init — не критично; главное до decoder_start) --- */
    ESP_ERROR_CHECK(i2s_out_init());

    /* --- ЦАП: GPIO1 платы — automute, Reg1=0x80 ASYNC slave --- */
    ESP_ERROR_CHECK(es9039_init(s_dac_dev, MAIN_DAC_GPIO1_PIN));

    /* --- Wi-Fi через сопроцессор ESP32-C6 ---
     * Креды из NVS (введены в UI и сохранены) применяются до старта стека. */
    char wifi_ssid[33] = {0}, wifi_pass[65] = {0};
    if (persist_load_str(WIFI_SSID_NVS_KEY, wifi_ssid, sizeof(wifi_ssid))
        && wifi_ssid[0] != '\0') {
        persist_load_str(WIFI_PASS_NVS_KEY, wifi_pass, sizeof(wifi_pass));
        (void)wifi_set_credentials(wifi_ssid, wifi_pass);
        ESP_LOGI(TAG, "Wi-Fi из NVS: \"%s\"", wifi_ssid);
    }
    ESP_ERROR_CHECK(wifi_connect_start());
    esp_err_t werr = wifi_wait_connected(30000);
    if (werr != ESP_OK) {
        /* Не фаталим: decoder_start вернёт ошибку HTTP, но диагностика останется живой */
        ESP_LOGW(TAG, "Wi-Fi не подключился за 30 с (%s) — сеть недоступна",
                 esp_err_to_name(werr));
    }

    /* --- SD-карта: инициализация + USB MSC (карта как USB-диск ПК) --- */
    esp_err_t sderr = sd_card_init();
    if (sderr == ESP_OK) {
        esp_err_t uerr = usb_msc_init(&(usb_msc_callbacks_t){
            .on_take_by_host  = msc_take_by_host,
            .on_return_to_app = msc_return_to_app,
        });
        if (uerr != ESP_OK) {
            /* Без MSC FatFS не смонтируется — SD-источник недоступен, но
             * HTTP-воспроизведение и UI продолжают работать. */
            ESP_LOGW(TAG, "USB MSC не поднялся (%s) — SD как источник недоступен",
                     esp_err_to_name(uerr));
        }
    }

    /* --- Воспроизведение --- */
    /* SD-карта — источник по умолчанию: если готова и на ней есть аудиофайл,
     * играем его; иначе — сохранённый/дефолтный HTTP-URL. */
    persist_load_str(SRC_URL_NVS_KEY, s_url, sizeof(s_url));
    if (usb_msc_owner() == USB_MSC_OWNER_APP) {
        char first_file[512];
        if (sd_card_find_first_audio("/sdcard", first_file, sizeof(first_file))) {
            snprintf(s_url, sizeof(s_url), "file://%.504s", first_file);
            ESP_LOGI(TAG, "Источник по умолчанию: SD-карта: %s", first_file);
        } else {
            ESP_LOGW(TAG, "SD смонтирована, аудиофайлов не найдено — играем %s", s_url);
        }
    }
    /* Колбэк конца трека — до старта декодера (авто-переход) */
    decoder_set_on_finish(on_decoder_finished);

    /* Файл с NAS/ПК или файл с SD (file://): */
    esp_err_t derr = decoder_start(s_url, DEC_TYPE_AUTO, false);
    /* Интернет-радио (Icecast, без расширения — детектируется как MP3): */
    // esp_err_t derr = decoder_start("http://icecast.example.com/stream", DEC_TYPE_AUTO, true);
    if (derr != ESP_OK) {
        ESP_LOGE(TAG, "decoder_start: %s", esp_err_to_name(derr));
    }

    /* Громкость, фильтр и FIR — из NVS (переживают перезапуск питания).
     * Фильтр применяем всегда: перекрывает init-значение Reg 88.
     * FIR — только если сохранён ручной выбор: авто (0) зальёт набор сам при
     * первой смене частоты. Без отложенной записи: только прочитали — писать
     * обратно незачем. Можно ставить и до старта декодера. */
    vol_apply(vol_load_db());
    /* Режим DSD: 0 = DoP (штатно), 1 = Native (требует проводки DATA2/DATA_CLK) */
    dsd_play_mode_set((dsd_play_mode_t)persist_load_u8(DSD_MODE_NVS_KEY, 0, 0, 1));
    s_filter_sel = persist_load_u8(FLT_NVS_KEY, FLT_DEFAULT_SHAPE, 0, 7);
    flt_apply_by_index(s_filter_sel);
    s_fir_sel = persist_load_u8(FIR_NVS_KEY, FIR_DEFAULT_SEL, 0, 7);
    if (s_fir_sel != 0) {
        fir_apply_by_index(s_fir_sel);
    }

    /* --- Графическая оболочка (дисплей HX8394 + тач GT911 на шине 'bus') ---
     * Горизонтальная ориентация 1280x720, языки EN/RU (переключение в Настройках).
     * Вкладка «Диагностика» раз в секунду опрашивает readback-регистры ЦАПа
     * (es9039_diag_read) — путь B, без дополнительного пина. */
    esp_err_t uerr = ui_init(bus, &s_ui_hooks);
    if (uerr != ESP_OK) {
        /* Не фаталим: аудио-тракт должен работать и без экрана */
        ESP_LOGW(TAG, "UI не поднялся: %s", esp_err_to_name(uerr));
    }

    /* OTA rollback: если запустилась новая прошивка (pending verify) и всё
     * поднялось до сюда — подтверждаем её; иначе после ребута загрузчик
     * откатит на предыдущий образ (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). */
    ota_upd_confirm_app();

    /* При желании: сменить стартовый FIR-пресет до подключения источника:
     * es9039_set_filter(ES9039_FIR_LP_FAST);  // штатные пресеты Reg 88
     * Кастомные FIR-наборы подключаются автоматически: decoder при смене частоты
     * вызывает es9039_switch_frequency(..., fs), а тот заливает набор из
     * es9039_fir_coeff.c (C-BRICK/C-SLOW × 44.1/48/96 кГц). */
}
