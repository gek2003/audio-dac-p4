/*
 * ============================================================================
 *  playlist.c — очередь воспроизведения (папка SD или файл .m3u)
 *
 *  Память: список путей в куче (максимум PLAYLIST_MAX путей по PLAYLIST_PATH_MAX
 *  символов); путём разрешаются относительные ссылки .m3u.
 *  Список папки сортируется по имени (strcmp) — детерминированный порядок
 *  треков, readdir порядок платформенно-зависим.
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "playlist.h"

static const char *TAG = "playlist";

#define PLAYLIST_MAX        512
#define PLAYLIST_PATH_MAX   512

typedef struct {
    char     *items[PLAYLIST_MAX];
    int      count;
    int      index;          /* -1 = не выбран */
    char     base_dir[PLAYLIST_PATH_MAX];   /* папка, из которой собрана очередь */
} playlist_ctx_t;

static playlist_ctx_t s_pl = { .count = 0, .index = -1 };

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

static void add_track(const char *path)
{
    if (s_pl.count >= PLAYLIST_MAX) return;
    size_t len = strlen(path);
    if (len == 0 || len >= PLAYLIST_PATH_MAX) return;
    char *dup = malloc(len + 1);
    if (dup == NULL) return;
    memcpy(dup, path, len + 1);
    s_pl.items[s_pl.count++] = dup;
}

static void reset(void)
{
    for (int i = 0; i < s_pl.count; i++) free(s_pl.items[i]);
    s_pl.count = 0;
    s_pl.index = -1;
    s_pl.base_dir[0] = '\0';
}

static int cmp_paths(const void *a, const void *b)
{
    const char *pa = *(const char *const *)a;
    const char *pb = *(const char *const *)b;
    return strcmp(pa, pb);
}

/* ---- кэш скана папки (P3-16) ------------------------------ */
/* Пути последнего удачного скана одним буфером через '\0'.
 * Повторный вход в ту же папку (совпадение пути + свежий mtime)
 * собирается из кэша без readdir/stat/qsort. Лимит буфера 64 КБ. */
#define CACHE_MAX 65536

static char     *s_cache_buf = NULL;
static int       s_cache_count = 0;
static char      s_cache_dir[PLAYLIST_PATH_MAX];
static time_t    s_cache_mtime = 0;
static bool      s_cache_valid = false;

static void cache_clear(void)
{
    if (s_cache_buf) { free(s_cache_buf); s_cache_buf = NULL; }
    s_cache_count = 0;
    s_cache_mtime = 0;
    s_cache_valid = false;
}

void playlist_cache_invalidate(void)
{
    cache_clear();
}

/* Общий выбор стартового трека для кэш-пути и свежего скана */
static void select_start(const char *start_file)
{
    s_pl.index = 0;
    if (start_file != NULL && start_file[0] != '\0') {
        for (int i = 0; i < s_pl.count; i++) {
            const char *base = strrchr(s_pl.items[i], '/');
            if (base && strcasecmp(base + 1, start_file) == 0) {
                s_pl.index = i;
                break;
            }
        }
    }
}

esp_err_t playlist_load_dir(const char *dir_path, const char *start_file)
{
    if (dir_path == NULL || dir_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    reset();

    struct stat dst;
    bool have_dst = (stat(dir_path, &dst) == 0);

    /* Кэш: та же папка, mtime не менялся — без readdir/stat/qsort */
    if (have_dst && s_cache_valid &&
        strcmp(dir_path, s_cache_dir) == 0 && dst.st_mtime == s_cache_mtime) {
        const char *cp = s_cache_buf;
        for (int i = 0; i < s_cache_count; i++, cp += strlen(cp) + 1) add_track(cp);
        if (s_pl.count == s_cache_count) {
            snprintf(s_pl.base_dir, sizeof(s_pl.base_dir), "%s", dir_path);
            select_start(start_file);
            ESP_LOGI(TAG, "Плейлист папки (из кэша): %d трек(ов), старт №%d",
                     s_pl.count, s_pl.index + 1);
            return ESP_OK;
        }
        reset();                       /* кэш рассыпался (OOM) — обычный скан */
    }

    DIR *d = opendir(dir_path);
    if (d == NULL) {
        ESP_LOGW(TAG, "Папка не открылась: %s", dir_path);
        cache_clear();
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!name_is_audio(ent->d_name)) continue;
        char full[PLAYLIST_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        add_track(full);
    }
    closedir(d);

    if (s_pl.count == 0) {
        ESP_LOGW(TAG, "В папке нет аудиофайлов: %s", dir_path);
        cache_clear();
        return ESP_ERR_NOT_FOUND;
    }
    qsort(s_pl.items, s_pl.count, sizeof(char *), cmp_paths);
    snprintf(s_pl.base_dir, sizeof(s_pl.base_dir), "%s", dir_path);

    /* Сохранить кэш скана (если весь список влезает в лимит) */
    {
        size_t total = 0;
        for (int i = 0; i < s_pl.count; i++) total += strlen(s_pl.items[i]) + 1;
        cache_clear();
        if (have_dst && total <= CACHE_MAX) {
            s_cache_buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
            if (s_cache_buf == NULL) s_cache_buf = malloc(total);
            if (s_cache_buf != NULL) {
                char *w = s_cache_buf;
                for (int i = 0; i < s_pl.count; i++) {
                    size_t l = strlen(s_pl.items[i]) + 1;
                    memcpy(w, s_pl.items[i], l);
                    w += l;
                }
                s_cache_count = s_pl.count;
                snprintf(s_cache_dir, sizeof(s_cache_dir), "%s", dir_path);
                s_cache_mtime = dst.st_mtime;
                s_cache_valid = true;
            }
        }
    }

    select_start(start_file);
    ESP_LOGI(TAG, "Плейлист папки: %d трек(ов), старт №%d", s_pl.count, s_pl.index + 1);
    return ESP_OK;
}

esp_err_t playlist_load_m3u(const char *m3u_path)
{
    if (m3u_path == NULL || m3u_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    reset();
    cache_clear();

    FILE *f = fopen(m3u_path, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "Плейлист не открылся: %s", m3u_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* папка плейлиста — для относительных путей */
    char dir[PLAYLIST_PATH_MAX] = "/sdcard";
    {
        const char *file_pos = strrchr(m3u_path, '/');
        size_t dlen = file_pos ? (size_t)(file_pos - m3u_path) : 0;
        if (dlen > 0 && dlen < sizeof(dir)) {
            memcpy(dir, m3u_path, dlen);
            dir[dlen] = '\0';
        }
    }
    snprintf(s_pl.base_dir, sizeof(s_pl.base_dir), "%s", dir);

    char line[640];
    int n_tracks = 0;
    while (fgets(line, sizeof(line), f) != NULL && s_pl.count < PLAYLIST_MAX) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;          /* комментарии и #EXTINF */

        char full[PLAYLIST_PATH_MAX];
        if (line[0] == '/' || strncmp(line, "http://", 7) == 0 ||
            strncmp(line, "https://", 8) == 0 || strncmp(line, "file://", 7) == 0) {
            strlcpy(full, line, sizeof(full));
        } else {
            strlcpy(full, dir, sizeof(full));
            strlcat(full, "/", sizeof(full));
            strlcat(full, line, sizeof(full));
        }
        add_track(full);
        n_tracks++;
    }
    fclose(f);

    if (s_pl.count == 0) {
        ESP_LOGW(TAG, "В плейлисте нет треков: %s", m3u_path);
        return ESP_ERR_NOT_FOUND;
    }
    s_pl.index = 0;
    ESP_LOGI(TAG, "Плейлист m3u: %d строк, папка %s", s_pl.count, dir);
    return ESP_OK;
}

int playlist_count(void) { return s_pl.count; }

int playlist_index(void)
{
    return s_pl.count ? s_pl.index : -1;
}

esp_err_t playlist_set_index(int idx)
{
    if (idx < 0 || idx >= s_pl.count) return ESP_ERR_INVALID_ARG;
    s_pl.index = idx;
    return ESP_OK;
}

const char *playlist_current(void)
{
    if (s_pl.count == 0 || s_pl.index < 0 || s_pl.index >= s_pl.count) return NULL;
    return s_pl.items[s_pl.index];
}

const char *playlist_get(int idx)
{
    if (idx < 0 || idx >= s_pl.count) return NULL;
    return s_pl.items[idx];
}

bool playlist_next(void)
{
    if (s_pl.count == 0) return false;
    if (s_pl.index + 1 >= s_pl.count) return false;
    s_pl.index++;
    return true;
}

bool playlist_prev(void)
{
    if (s_pl.count == 0) return false;
    if (s_pl.index <= 0) return false;
    s_pl.index--;
    return true;
}

void playlist_clear(void)
{
    reset();
}

bool playlist_active(void)
{
    return s_pl.count > 0;
}
