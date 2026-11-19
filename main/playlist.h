/*
 * ============================================================================
 *  playlist.h — очередь воспроизведения: папки SD и файлы .m3u
 *
 *  Два источника очереди:
 *    - playlist_load_dir(dir, start_file): все аудиофайлы папки,
 *      отсортированные по имени (включая DSD); start_file — текущий трек,
 *      индекс ставится на него. Позволяет next/prev по любой папке без
 *      плейлиста;
 *    - playlist_load_m3u(path): файл .m3u/.m3u8 (UTF-8, пути абсолютные
 *      /sdcard/... или относительные к папке плейлиста; строки http(s):// —
 *      тоже очередь, например подборка радиостанций). Комментарии # —
 *      игнорируются.
 *
 *  Автопереход и next/prev — в main.c (колбэк on_finish + кнопки UI).
 * ============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Загрузить очередь из папки dir_path; если start_file != NULL — индекс
 * ставится на этот файл (по имени, сравнение без регистра). Возвращает
 * число треков или ESP_ERR_* при ошибке (нет папки/пусто). */
esp_err_t playlist_load_dir(const char *dir_path, const char *start_file);

/* Загрузить очередь из .m3u/.m3u8. Возвращает число треков или ошибку.
 * Абсолютные пути (начинающиеся с '/') и http(s):// используются как есть;
 * прочие строки — разрешаются относительно папки плейлиста. */
esp_err_t playlist_load_m3u(const char *m3u_path);

int       playlist_count(void);
int       playlist_index(void);           /* -1 если очередь пуста/не активна */
esp_err_t playlist_set_index(int idx);
const char *playlist_current(void);       /* путь/URL текущего трека или NULL */
const char *playlist_get(int idx);
bool     playlist_next(void);             /* advance; false = конец очереди */
bool     playlist_prev(void);             /* назад; false = уже в начале */
void     playlist_clear(void);
bool     playlist_active(void);           /* очередь не пуста */
