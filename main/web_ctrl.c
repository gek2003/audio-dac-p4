/*
 * ============================================================================
 *  web_ctrl.c — мини-HTTP-сервер управления плеером (esp_http_server)
 *
 *  Реализация:
 *    - esp_http_server (компонент IDF) на порту 80; запросы обрабатывает таск
 *      httpd — вызовы в декодер идут из него (то же, что делают кнопки UI);
 *    - доступ к плееру через ui_hooks_t (см. main.c): громкость/mute/источник/
 *      next/prev выполняются ровно тем же кодом, что и кнопки экрана —
 *      громкость и фильтры сохраняются в NVS по тем же правилам;
 *    - JSON собирается вручную (короткие структуры, без cJSON — не тянем
 *      компонент json ради пары мелких ответов);
 *    - HTML-пульт — одна RODATA-строка, JS на fetch(): статус каждые 2 с,
 *      play/prev/next, громкость, mute, URL источника, файлы SD (по папкам),
 *      избранные станции, перемотка кликом по прогресс-полосе (если seekable).
 *
 *  Память: буферы ответов выделяются один раз при старте из PSRAM (фолбэк —
 *  обычный heap); стек httpd 6 КБ.
 *
 *  Потокобезопасность: мутирующие хуки мьютексированы в main.c (hook_lock —
 *  P4c-5/P2-10); смена источника асинхронна — decoder_request ставит команду
 *  в очередь декодера и не блокирует httpd-таск (P4c-5/P2-11).
 * ============================================================================
 */

#include "web_ctrl.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#include "audio_pipeline.h"
#include "playlist.h"
#include "stations.h"
#include "sd_card.h"
#include "usb_flash.h"
#include "phono_adc.h"
#include "dsd_native.h"
#include "ota_upd.h"
#include "dsd_dec.h"
#include "nvs.h"

static const char *TAG = "web_ctrl";

/* Хуки плеера (копия структуры, устанавливается web_ctrl_start). */
static ui_hooks_t s_hooks;
static bool s_started;
static httpd_handle_t s_server;

/* Буферы (один запрос за раз — httpd обрабатывает последовательно). */
#define WEB_BUF_STATUS   (8 * 1024)     /* /api/status, /api/stations */
#define WEB_BUF_FILES    (24 * 1024)    /* /api/files — до 200 записей */
static char *s_buf_a, *s_buf_b;         /* PSRAM */

#define WEB_SD_ROOT     "/sdcard"
#define WEB_FILES_MAX   200

/* Записи каталога для /api/files — максимум WEB_FILES_MAX, массив в PSRAM. */
struct web_entry {
    char name[128];
    bool is_dir;
};
static struct web_entry *s_entries;

/* ------------------------- Мелкие утилиты -------------------------------- */

/* JSON-экранирование (кавычки, слэш, управляющие байты; UTF-8 байты >0x7F
 * проходят как есть — LVGL/браузер справляются с кириллицей). */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p && o + 7 < cap; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* URL-decode (%XX и '+'). */
static void url_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < cap; i++) {
        if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* Значение query-параметра (декодированное). >0 — длина, <0 — нет параметра. */
static int get_query(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[512];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return -1;
    char raw[512];
    if (httpd_query_key_value(q, key, raw, sizeof(raw)) != ESP_OK) return -1;
    url_decode(raw, out, cap);
    return (int)strlen(out);
}

static void send_json(httpd_req_t *req, const char *buf, int code)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (code != 200) {
        httpd_resp_set_status(req, code == 400 ? "400 Bad Request"
                                               : "500 Internal Server Error");
    }
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t opts_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------ /api/status ------------------------------ */

static const char *src_kind(const char *url)
{
    if (url == NULL || url[0] == '\0' || strcmp(url, "-") == 0) return "none";
    if (strncmp(url, "file://", 7) == 0) {
        const char *dot = strrchr(url, '.');
        if (dot && (strncasecmp(dot, ".dsf", 4) == 0 || strncasecmp(dot, ".dff", 4) == 0)) return "dsd";
        return "file";
    }
    return http_source_is_stream() ? "radio" : "http";
}

static void status_json(char *b, size_t cap)
{
    const char *src = (s_hooks.get_source ? s_hooks.get_source() : "");
    const char *state = "idle";
    if (decoder_running()) {
        state = decoder_paused() ? "paused" : "playing";
    }

    uint32_t pos_ms = 0, total_ms = 0;
    bool seekable = false;
    decoder_progress_ext(&pos_ms, &total_ms, &seekable);

    static const char *const ota_st[] = { "idle", "running", "ok", "fail" };
    char esc_ota[160];
    json_escape(ota_upd_msg(), esc_ota, sizeof(esc_ota));
    char esc_ver[80];
    json_escape(ota_upd_version(), esc_ver, sizeof(esc_ver));

    float db = (s_hooks.get_volume_db ? s_hooks.get_volume_db() : -127.5f);
    /* проценты: 0 дБ -> 100%, -127.5 дБ -> 0% (линейно, как слайдер UI) */
    int pct = (int)((-db / 127.5f) * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    char esc_src[1100];
    json_escape(src ? src : "", esc_src, sizeof(esc_src));

    char ip[20] = "";
    esp_netif_t *nf = esp_netif_get_default_netif();
    if (nf != NULL) {
        esp_netif_ip_info_t ipi;
        if (esp_netif_get_ip_info(nf, &ipi) == ESP_OK) {
            esp_ip4addr_ntoa(&ipi.ip, ip, sizeof(ip));
        }
    }

    char ssid[40] = "";
    if (s_hooks.get_wifi_ssid) {
        const char *s = s_hooks.get_wifi_ssid();
        snprintf(ssid, sizeof(ssid), "%s", s ? s : "");
    }

    snprintf(b, cap,
        "{\"state\":\"%s\",\"kind\":\"%s\",\"src\":\"%s\","
        "\"vol_db\":%.1f,\"vol_pct\":%d,\"mute\":%s,"
        "\"filter\":%d,\"fir\":%d,\"hpf\":%d,\"dsd\":%d,\"fs\":%lu,"
        "\"pos_ms\":%lu,\"total_ms\":%lu,\"seekable\":%s,"
        "\"pl_count\":%d,\"pl_index\":%d,\"stations\":%d,"
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"uptime_s\":%llu,"
        "\"sd\":%s,\"ver\":\"%s\","
        "\"ota\":{\"state\":\"%s\",\"pct\":%d,\"msg\":\"%s\"}}",
        state, src_kind(src), esc_src,
        db, pct, (s_hooks.get_mute && s_hooks.get_mute()) ? "true" : "false",
        s_hooks.get_filter ? s_hooks.get_filter() : -1,
        s_hooks.get_fir ? s_hooks.get_fir() : -1,
        (int)phono_hpf_get(),
        (int)dsd_play_mode_get(),
        (unsigned long)i2s_out_current_fs(),
        (unsigned long)pos_ms, (unsigned long)total_ms, seekable ? "true" : "false",
        playlist_count(), playlist_index(), stations_count(),
        ssid, ip,
        (unsigned long long)(esp_timer_get_time() / 1000000ULL),
        sd_card_ready() ? "true" : "false",
        esc_ver,
        ota_st[ota_upd_state()], ota_upd_progress(), esc_ota);
}

static esp_err_t api_status(httpd_req_t *req)
{
    status_json(s_buf_a, WEB_BUF_STATUS);
    send_json(req, s_buf_a, 200);
    return ESP_OK;
}

/* ------------------------- Действия плеера ------------------------------- */

static esp_err_t api_volume(httpd_req_t *req)
{
    char val[16];
    if (get_query(req, "v", val, sizeof(val)) <= 0) {
        send_json(req, "{\"err\":\"need v=0..100\"}", 400);
        return ESP_OK;
    }
    int v = atoi(val);
    if (v < 0 || v > 100) {
        send_json(req, "{\"err\":\"range 0..100\"}", 400);
        return ESP_OK;
    }
    float db = -v * 1.275f;
    if (s_hooks.set_volume_db) s_hooks.set_volume_db(db);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"vol_db\":%.1f,\"vol_pct\":%d}", db, v);
    send_json(req, buf, 200);
    return ESP_OK;
}

static esp_err_t api_mute(httpd_req_t *req)
{
    bool now;
    char val[8];
    if (get_query(req, "on", val, sizeof(val)) > 0) {
        now = (atoi(val) != 0);
    } else {
        now = !(s_hooks.get_mute && s_hooks.get_mute());
    }
    if (s_hooks.set_mute) s_hooks.set_mute(now);
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"mute\":%s}", now ? "true" : "false");
    send_json(req, buf, 200);
    return ESP_OK;
}

static esp_err_t api_play(httpd_req_t *req)
{
    (void)req;
    if (s_hooks.play_pause) s_hooks.play_pause();
    const char *state = decoder_running()
        ? (decoder_paused() ? "paused" : "playing") : "idle";
    char buf[40];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\"}", state);
    send_json(req, buf, 200);
    return ESP_OK;
}

static esp_err_t api_next(httpd_req_t *req)
{
    (void)req;
    if (s_hooks.next_track) s_hooks.next_track();
    send_json(req, "{\"ok\":true}", 200);
    return ESP_OK;
}

static esp_err_t api_prev(httpd_req_t *req)
{
    (void)req;
    if (s_hooks.prev_track) s_hooks.prev_track();
    send_json(req, "{\"ok\":true}", 200);
    return ESP_OK;
}

static esp_err_t api_seek(httpd_req_t *req)
{
    char val[16];
    if (get_query(req, "ms", val, sizeof(val)) <= 0) {
        send_json(req, "{\"err\":\"need ms\"}", 400);
        return ESP_OK;
    }
    esp_err_t e = decoder_seek((uint32_t)strtoul(val, NULL, 10));
    const char *msg = (e == ESP_OK) ? "{\"ok\":true}"
                    : (e == ESP_ERR_NOT_SUPPORTED)
                        ? "{\"err\":\"not supported (MP3/DSD only)\"}"
                    : "{\"err\":\"no data or stopped\"}";
    send_json(req, msg, e == ESP_OK ? 200 : 400);
    return ESP_OK;
}

/* ------------------------- Phono (ADC) вход ------------------------------- */

static esp_err_t api_phono(httpd_req_t *req)
{
    (void)req;
    if (phono_adc_running()) {
        send_json(req, "{\"ok\":true,\"note\":\"already running\"}", 200);
        return ESP_OK;
    }
    if (s_hooks.set_source_url) {
        s_hooks.set_source_url("adc://");
    } else {
        /* P4c-5/P2-11: асинхронный запуск — httpd не блокируется; decoder_stop
         * не нужен: decoder_request сам гасит текущий источник. */
        decoder_request("adc://", DEC_TYPE_AUTO, false);
    }
    send_json(req, "{\"ok\":true}", 200);
    return ESP_OK;
}

static esp_err_t api_hpf(httpd_req_t *req)
{
    char val[16];
    if (get_query(req, "v", val, sizeof(val)) <= 0) {
        send_json(req, "{\"err\":\"need v=0|1|2\"}", 400);
        return ESP_OK;
    }
    int v = atoi(val);
    if (v < 0 || v > 2) {
        send_json(req, "{\"err\":\"range 0..2\"}", 400);
        return ESP_OK;
    }
    phono_hpf_set((hpf_mode_t)v);
    char buf[40];
    snprintf(buf, sizeof(buf), "{\"hpf\":%d}", v);
    send_json(req, buf, 200);
    return ESP_OK;
}

/* ------------------------- Режим DSD: DoP / Native ------------------------ */

static esp_err_t api_dsd(httpd_req_t *req)
{
    char val[16];
    if (get_query(req, "v", val, sizeof(val)) <= 0) {
        send_json(req, "{\"dsd\":%d}", (int)dsd_play_mode_get());
        return ESP_OK;
    }
    int v = atoi(val);
    if (v < 0 || v > 1) {
        send_json(req, "{\"err\":\"range 0..1\"}", 400);
        return ESP_OK;
    }
    dsd_play_mode_set((dsd_play_mode_t)v);
    /* Namespace и ключ совпадают с persist в main.c: PERSIST_NVS_NAMESPACE
     * ("dac") и DSD_MODE_NVS_KEY ("dsd_mode") — при старте читается оттуда же.
     * P4c-4/P2-9: сверено — namespace "dac" единый по всему проекту. */
    nvs_handle_t nh = 0;
    if (nvs_open("dac", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "dsd_mode", (uint8_t)v);
        nvs_commit(nh);
        nvs_close(nh);
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "{\"dsd\":%d}", v);
    send_json(req, buf, 200);
    return ESP_OK;
}

static esp_err_t api_src(httpd_req_t *req)
{
    char url[600];
    if (get_query(req, "u", url, sizeof(url)) <= 0) {
        send_json(req, "{\"err\":\"need u=<url>\"}", 400);
        return ESP_OK;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0
        && strncmp(url, "file://", 7) != 0) {
        send_json(req, "{\"err\":\"only http(s):// or file://\"}", 400);
        return ESP_OK;
    }
    if (s_hooks.set_source_url) s_hooks.set_source_url(url);
    send_json(req, "{\"ok\":true}", 200);
    return ESP_OK;
}

/* --------------------------- Файлы SD ------------------------------------- */

/* Аудио-расширения (как в playlist.c); папки показываем всегда. */
static bool is_audio_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;
    static const char *const ext[] = { ".mp3", ".flac", ".wav", ".aac",
                                       ".dsf", ".dff", ".m3u", ".m3u8" };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        if (strncasecmp(dot, ext[i], strlen(ext[i])) == 0) return true;
    }
    return false;
}

/* Относительный путь -> абсолютный. root: "sd" (по умолчанию) -> /sdcard,
 * "usb" -> /usb (флешка). Без ".." и вылезания за корень носителя. */
static bool safe_media_path(const char *rel, const char *root, char *out, size_t cap)
{
    if (rel[0] == '/') rel++;
    if (strstr(rel, "..") != NULL) return false;
    if (rel[0] == '\0') {
        snprintf(out, cap, "%s", root);
    } else {
        snprintf(out, cap, "%s/%s", root, rel);
    }
    return true;
}

/* Определить корень запроса (root=sd|usb) и его абсолютный путь. */
static bool query_root(httpd_req_t *req, const char **root, char rootname[8])
{
    char r[8];
    if (get_query(req, "root", r, sizeof(r)) > 0 && strcasecmp(r, "usb") == 0) {
        if (!usb_flash_mounted()) return false;
        *root = "/usb";
        snprintf(rootname, 8, "usb");
    } else {
        *root = WEB_SD_ROOT;
        snprintf(rootname, 8, "sd");
    }
    return true;
}

static esp_err_t api_files(httpd_req_t *req)
{
    char rel[256], rootname[8] = "sd";
    if (get_query(req, "path", rel, sizeof(rel)) < 0) {
        rel[0] = '\0';
    }
    const char *root = WEB_SD_ROOT;
    if (!query_root(req, &root, rootname)) {
        send_json(req, "{\"err\":\"USB not mounted\"}", 400);
        return ESP_OK;
    }
    char full[300];
    if (!safe_media_path(rel, root, full, sizeof(full))) {
        send_json(req, "{\"err\":\"bad path\"}", 400);
        return ESP_OK;
    }
    if (rootname[0] == 's' && !sd_card_ready()) {
        send_json(req, "{\"err\":\"SD not ready\"}", 500);
        return ESP_OK;
    }

    DIR *d = opendir(full);
    if (d == NULL) {
        send_json(req, "{\"err\":\"open failed\"}", 400);
        return ESP_OK;
    }

    /* Записи: имя + флаги, максимум WEB_FILES_MAX. Статический массив из PSRAM
     * (26 КБ на стек не влезет, а heap трогать на каждый запрос незачем). */
    struct entry { char name[128]; bool is_dir; };
    static struct entry *ents;          /* выделяется при web_ctrl_start */
    if (ents == NULL) {
        closedir(d);
        send_json(req, "{\"err\":\"internal\"}", 500);
        return ESP_OK;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < WEB_FILES_MAX) {
        if (de->d_name[0] == '.') continue;
        char child[320];
        snprintf(child, sizeof(child), "%s/%s", full, de->d_name);
        struct stat st;
        bool is_dir = (stat(child, &st) == 0 && S_ISDIR(st.st_mode));
        if (!is_dir && !is_audio_name(de->d_name)) continue;
        snprintf(ents[n].name, sizeof(ents[n].name), "%s", de->d_name);
        ents[n].is_dir = is_dir;
        n++;
    }
    closedir(d);

    /* сортировка: папки вперёд, потом по имени без регистра */
    for (int i = 0; i + 1 < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            bool j_before;
            if (ents[j].is_dir != ents[best].is_dir) j_before = ents[j].is_dir;
            else j_before = (strcasecmp(ents[j].name, ents[best].name) < 0);
            if (j_before) best = j;
        }
        if (best != i) {
            struct entry tmp = ents[i]; ents[i] = ents[best]; ents[best] = tmp;
        }
    }

    /* Пагинация: off/lim из query (P4d/P3-15: сверка — работает с 3.6.3). Без параметров поведение прежнее
     * (все записи, лимит WEB_FILES_MAX). lim срезает ответ, чтобы большой
     * каталог не упирался в буфер httpd. */
    char off_s[16] = "", lim_s[16] = "";
    (void)get_query(req, "off", off_s, sizeof(off_s));
    (void)get_query(req, "lim", lim_s, sizeof(lim_s));
    int off = (off_s[0] ? atoi(off_s) : 0);
    int lim = (lim_s[0] ? atoi(lim_s) : WEB_FILES_MAX);
    if (off < 0) off = 0;
    if (off > n) off = n;
    if (lim <= 0 || lim > WEB_FILES_MAX) lim = WEB_FILES_MAX;

    size_t o = 0;
    char *buf = s_buf_b;
    o += (size_t)snprintf(buf + o, WEB_BUF_FILES - o,
                          "{\"path\":\"%s\",\"count\":%d,\"off\":%d,\"items\":[", rel, n, off);
    for (int i = off; i < n && i < off + lim && o < WEB_BUF_FILES - 300; i++) {
        char esc[280];
        json_escape(ents[i].name, esc, sizeof(esc));
        o += (size_t)snprintf(buf + o, WEB_BUF_FILES - o, "%s{\"name\":\"%s\",\"dir\":%s}",
                              (i > off) ? "," : "", esc, ents[i].is_dir ? "true" : "false");
    }
    snprintf(buf + o, WEB_BUF_FILES - o, "]}");
    send_json(req, buf, 200);
    return ESP_OK;
}

static esp_err_t api_playfile(httpd_req_t *req)
{
    char rel[256], rootname[8] = "sd";
    if (get_query(req, "p", rel, sizeof(rel)) <= 0) {
        send_json(req, "{\"err\":\"need p=<path>\"}", 400);
        return ESP_OK;
    }
    const char *root = WEB_SD_ROOT;
    if (!query_root(req, &root, rootname)) {
        send_json(req, "{\"err\":\"USB not mounted\"}", 400);
        return ESP_OK;
    }
    char full[300];
    if (!safe_media_path(rel, root, full, sizeof(full))) {
        send_json(req, "{\"err\":\"bad path\"}", 400);
        return ESP_OK;
    }
    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
        send_json(req, "{\"err\":\"no such file\"}", 400);
        return ESP_OK;
    }
    char url[320];
    snprintf(url, sizeof(url), "file://%s", full);
    if (s_hooks.set_source_url) s_hooks.set_source_url(url);
    send_json(req, "{\"ok\":true}", 200);
    return ESP_OK;
}

/* --------------------------- Станции -------------------------------------- */

static esp_err_t api_stations(httpd_req_t *req)
{
    int n = stations_count();
    size_t o = 0;
    o += (size_t)snprintf(s_buf_a + o, WEB_BUF_STATUS - o,
                          "{\"count\":%d,\"items\":[", n);
    for (int i = 0; i < n && o < WEB_BUF_STATUS - 500; i++) {
        const station_t *st = stations_get(i);
        char esc_n[140], esc_u[320];
        json_escape(st ? st->name : "", esc_n, sizeof(esc_n));
        json_escape(st ? st->url : "", esc_u, sizeof(esc_u));
        o += (size_t)snprintf(s_buf_a + o, WEB_BUF_STATUS - o,
                              "%s{\"name\":\"%s\",\"url\":\"%s\"}",
                              i ? "," : "", esc_n, esc_u);
    }
    snprintf(s_buf_a + o, WEB_BUF_STATUS - o, "]}");
    send_json(req, s_buf_a, 200);
    return ESP_OK;
}

static esp_err_t api_station(httpd_req_t *req)
{
    char val[8];
    if (get_query(req, "i", val, sizeof(val)) <= 0) {
        send_json(req, "{\"err\":\"need i=<index>\"}", 400);
        return ESP_OK;
    }
    int i = atoi(val);
    const station_t *st = stations_get(i);
    if (st == NULL) {
        send_json(req, "{\"err\":\"no such station\"}", 400);
        return ESP_OK;
    }
    if (s_hooks.set_source_url) s_hooks.set_source_url(st->url);
    send_json(req, "{\"ok\":true}", 200);
    return ESP_OK;
}

static esp_err_t api_ota(httpd_req_t *req)
{
    char file[256], url[600];
    esp_err_t e = ESP_FAIL;
    bool have_file = (get_query(req, "file", file, sizeof(file)) > 0);
    bool have_url  = !have_file && (get_query(req, "u", url, sizeof(url)) > 0);

    if (!have_file && !have_url) {
        /* без параметров — только статус */
        char buf[192], m[128];
        json_escape(ota_upd_msg(), m, sizeof(m));
        snprintf(buf, sizeof(buf), "{\"ota\":{\"state\":\"%s\",\"pct\":%d,\"msg\":\"%s\"}}",
                 (ota_upd_state() == OTA_UPD_IDLE) ? "idle"
                 : (ota_upd_state() == OTA_UPD_RUNNING) ? "running"
                 : (ota_upd_state() == OTA_UPD_OK) ? "ok" : "fail",
                 ota_upd_progress(), m);
        send_json(req, buf, 200);
        return ESP_OK;
    }

    if (have_file) {
        e = ota_upd_start_file(file);
    } else {
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            send_json(req, "{\"err\":\"only http(s)://\"}", 400);
            return ESP_OK;
        }
        e = ota_upd_start_url(url);
    }

    if (e == ESP_OK) {
        send_json(req, "{\"ok\":true,\"reboot\":\"auto after write\"}", 200);
        return ESP_OK;
    }
    const char *err = (e == ESP_ERR_INVALID_STATE) ? "already running"
                    : (e == ESP_ERR_NOT_SUPPORTED) ? "no ota partition"
                    : (e == ESP_ERR_NOT_FOUND) ? "file not found"
                    : (e == ESP_ERR_INVALID_SIZE) ? "bad size"
                    : (e == ESP_ERR_INVALID_ARG) ? "bad arg" : "error";
    char m[128], buf[256];
    json_escape(ota_upd_msg(), m, sizeof(m));
    snprintf(buf, sizeof(buf), "{\"err\":\"%s\",\"msg\":\"%s\"}", err, m);
    send_json(req, buf, 400);
    return ESP_OK;
}

/* --------------------------- Диагностика ---------------------------------- */

static esp_err_t api_diag(httpd_req_t *req)
{
    char buf[512] = "";
    if (s_hooks.get_diag) s_hooks.get_diag(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* --------------------------- HTML-пульт ------------------------------------
 * Компактная страница на тёмной палитре устройства (0x14181c / 0x3a4148):
 * статус, прогресс (клик = seek при seekable), громкость +/- и слайдер,
 * mute, prev/play/next, URL источника, файлы SD (навигация по папкам),
 * станции. Статус обновляется каждые 2 с, кнопка play меняет иконку. */

#include "web_root.h"

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* --------------------------- Старт/стоп ----------------------------------- */

esp_err_t web_ctrl_start(const ui_hooks_t *hooks)
{
    if (s_started) return ESP_OK;
    if (hooks == NULL) return ESP_ERR_INVALID_ARG;
    s_hooks = *hooks;

    /* Буферы — из PSRAM (фолбэк на обычный heap). */
    if (s_buf_a == NULL) {
        s_buf_a = heap_caps_malloc(WEB_BUF_STATUS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buf_a == NULL) s_buf_a = malloc(WEB_BUF_STATUS);
    }
    if (s_buf_b == NULL) {
        s_buf_b = heap_caps_malloc(WEB_BUF_FILES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buf_b == NULL) s_buf_b = malloc(WEB_BUF_FILES);
    }
    /* массив записей каталога: 200 x 132 байта */
    if (s_entries == NULL) {
        s_entries = heap_caps_malloc(sizeof(struct web_entry) * WEB_FILES_MAX,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_entries == NULL) {
            s_entries = malloc(sizeof(struct web_entry) * WEB_FILES_MAX);
        }
    }
    if (s_buf_a == NULL || s_buf_b == NULL || s_entries == NULL) {
        ESP_LOGE(TAG, "Нет памяти под буферы веб-сервера");
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 6144;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,     .handler = page_handler  },
        { .uri = "/api/status",   .method = HTTP_GET,     .handler = api_status    },
        { .uri = "/api/volume",   .method = HTTP_POST,    .handler = api_volume    },
        { .uri = "/api/mute",     .method = HTTP_POST,    .handler = api_mute      },
        { .uri = "/api/play",     .method = HTTP_POST,    .handler = api_play      },
        { .uri = "/api/next",     .method = HTTP_POST,    .handler = api_next      },
        { .uri = "/api/prev",     .method = HTTP_POST,    .handler = api_prev      },
        { .uri = "/api/seek",     .method = HTTP_POST,    .handler = api_seek      },
        { .uri = "/api/phono",    .method = HTTP_POST,    .handler = api_phono     },
        { .uri = "/api/hpf",      .method = HTTP_POST,    .handler = api_hpf       },
        { .uri = "/api/dsd",      .method = HTTP_POST,    .handler = api_dsd       },
        { .uri = "/api/src",      .method = HTTP_POST,    .handler = api_src       },
        { .uri = "/api/files",    .method = HTTP_GET,     .handler = api_files     },
        { .uri = "/api/playfile", .method = HTTP_POST,    .handler = api_playfile  },
        { .uri = "/api/stations", .method = HTTP_GET,     .handler = api_stations  },
        { .uri = "/api/station",  .method = HTTP_POST,    .handler = api_station   },
        { .uri = "/api/diag",     .method = HTTP_GET,     .handler = api_diag      },
        { .uri = "/api/ota",      .method = HTTP_POST,    .handler = api_ota       },
        { .uri = "*",             .method = HTTP_OPTIONS, .handler = opts_handler  },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGW(TAG, "handler %s не зарегистрирован", uris[i].uri);
        }
    }

    s_started = true;
    ESP_LOGI(TAG, "HTTP-пульт запущен: порт 80, %u обработчиков",
             (unsigned)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}

esp_err_t web_ctrl_stop(void)
{
    if (!s_started || s_server == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t e = httpd_stop(s_server);
    s_server = NULL;
    s_started = false;
    return e;
}
