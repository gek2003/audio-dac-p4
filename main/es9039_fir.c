/*
 * es9039_fir.c — загрузка пользовательских FIR-фильтров в ES9039Q2M.
 *
 * Последовательность записи одного коэффициента (даташит v0.1.3, стр. 30):
 *   Write Reg137 addr        // RAM-адрес (и выбор ступени битом 7)
 *   Write Reg138 data[7:0]
 *   Write Reg139 data[15:8]
 *   Write Reg140 data[23:16]
 *   Write Reg135 0x02        // защёлкнуть 24 бита в RAM (WE=1, EN=0)
 *   Write Reg135 0x00        // снять WE
 * Повторить на все адреса обеих ступеней, затем Reg135 = 0x01 (EN=1).
 *
 * Коэффициенты в es9039_fir_coeff.c хранятся как ПРОТОТИП h[0..N-1] ступени.
 * Здесь прототип раскладывается на полифазные ветви g_p[k] = h[p + k*R]
 * (R — коэффициент интерполяции ступени) и раскладывается по RAM согласно
 * выбранной гипотезе маппинга.
 *
 * Неизвестные (не документированы, app note только у дистрибьютора):
 *   TODO(структура): порядок ступеней в тракте (2x->4x или 4x->2x),
 *   маппинг полифазных фаз на адреса RAM, внутренняя нормировка.
 *   Диагностика — docs/FIR_CUSTOM_INTEGRATION.md.
 */
#include <string.h>
#include "esp_log.h"
#include "es9039_fir.h"

static const char *TAG = "es9039_fir";

/* --- слабые реализации низкоуровневого доступа (переопределяются драйвером) --- */
__attribute__((weak)) esp_err_t es9039_write_reg(uint8_t reg, uint8_t val)
{
    ESP_LOGE(TAG, "es9039_write_reg() не связана с драйвером: добавьте реализацию в es9039q2m_esp32p4_driver.c");
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) esp_err_t es9039_read_reg(uint8_t reg, uint8_t *val)
{
    ESP_LOGE(TAG, "es9039_read_reg() не связана с драйвером");
    return ESP_ERR_NOT_SUPPORTED;
}

/* --- конфигурация маппинга (уточняется по диагностике, см. MD) -------------- */
static es9039_fir_phase_map_t s_phase_map = FIR_PHASE_BLOCK;

/* Активный набор (последний применённый es9039_fir_apply) — для предупреждений,
 * когда ручной выбор рассчитан под другую частоту потока. */
static const es9039_fir_set_t *s_active_set = NULL;

/* Ручной выбор приоритетнее авто: пока true, es9039_fir_apply_for_fs()
 * не меняет активный набор (см. es9039_fir.h). Сбрасывается из UI «Auto». */
static bool s_manual_mode = false;

void es9039_fir_set_manual_mode(bool enable)
{
    if (s_manual_mode == enable) return;
    s_manual_mode = enable;
    ESP_LOGI(TAG, "ручной FIR-выбор %s", enable ? "ВКЛЮЧЁН — авто-подмена пропускается"
                                                : "выключен — набор под поток (авто)");
}

bool es9039_fir_get_manual_mode(void)
{
    return s_manual_mode;
}

void es9039_fir_set_phase_map(es9039_fir_phase_map_t map)
{
    s_phase_map = map;
    ESP_LOGI(TAG, "phase map = %s", map == FIR_PHASE_INTERLEAVED ? "interleaved" : "block");
}

/* Роли ступеней (рабочая модель): ступень 2x — главный фильтр (подавляет образ
 * fs), ступень 4x — чистовая (подавляет образы 2fs и выше).
 * TODO(структура): если диагностика покажет обратный порядок ступеней в тракте,
 * поменять местами роли при генерации (tools/gen_fir_coeffs.py, спеки BRICK/SLOW). */

static uint8_t coeff_addr(uint8_t stage, size_t phase, size_t tap)
{
    const size_t R = (stage == 0) ? 2 : 4;
    if (s_phase_map == FIR_PHASE_INTERLEAVED) {
        return (uint8_t)(tap * R + phase);          /* чередование фаз */
    }
    const size_t M = (stage == 0) ? ES9039_FIR_N2X / 2 : ES9039_FIR_N4X / 4;
    return (uint8_t)(phase * M + tap);              /* фазы блоками */
}

esp_err_t es9039_fir_load_stage(uint8_t stage, const int32_t *coeffs, size_t n)
{
    if (!coeffs || (stage == 0 && n != ES9039_FIR_N2X) || (stage == 1 && n != ES9039_FIR_N4X)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t R = (stage == 0) ? 2u : 4u;
    const size_t M = n / R;              /* тапов на фазу: 64 или 8 */
    const uint8_t stage_bit = stage ? 0x80 : 0x00;

    /* На время программирования EN (бит0 Reg135) держим 0 — иначе чип может
     * подхватить наполовину залитый фильтр. */
    esp_err_t e;
    for (size_t p = 0; p < R; p++) {
        for (size_t k = 0; k < M; k++) {
            const int32_t c = coeffs[p + k * R];      /* полифазная ветвь g_p[k] */
            if (c > 8388607 || c < -8388608) {
                ESP_LOGE(TAG, "коэффициент вне диапазона int24: %ld (stage%u p%u k%u)",
                         (long)c, stage, (unsigned)p, (unsigned)k);
                return ESP_ERR_INVALID_ARG;
            }
            const uint8_t addr_reg = stage_bit | (coeff_addr(stage, p, k) & 0x7F);
            const uint8_t d0 = (uint8_t)(c & 0xFF);
            const uint8_t d1 = (uint8_t)((c >> 8) & 0xFF);
            const uint8_t d2 = (uint8_t)((c >> 16) & 0xFF);
            if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_ADDR, addr_reg)) != ESP_OK) return e;
            if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_DATA0, d0)) != ESP_OK) return e;
            if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_DATA1, d1)) != ESP_OK) return e;
            if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_DATA2, d2)) != ESP_OK) return e;
            if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_CTRL, 0x02)) != ESP_OK) return e;  /* WE=1 */
            if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_CTRL, 0x00)) != ESP_OK) return e;  /* WE=0 */
        }
    }
    ESP_LOGI(TAG, "ступень %s: записано %u коэффициентов (map=%s)",
             stage ? "4x" : "2x", (unsigned)n,
             s_phase_map == FIR_PHASE_INTERLEAVED ? "interleaved" : "block");
    return ESP_OK;
}

esp_err_t es9039_fir_readback(uint8_t stage, uint8_t addr, int32_t *out)
{
    if (!out || addr > 0x7F) return ESP_ERR_INVALID_ARG;
    esp_err_t e;
    if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_ADDR, (stage ? 0x80 : 0x00) | addr)) != ESP_OK) return e;
    uint8_t b0, b1, b2;
    if ((e = FIR_READ_REG(ES9039_REG_PROG_RAM_OUT0, &b0)) != ESP_OK) return e;
    if ((e = FIR_READ_REG(ES9039_REG_PROG_RAM_OUT1, &b1)) != ESP_OK) return e;
    if ((e = FIR_READ_REG(ES9039_REG_PROG_RAM_OUT2, &b2)) != ESP_OK) return e;
    /* TODO(verify): подтвердить, что OUT-регистры возвращают коэффициент,
     * установленный последней записью ADDR (а не автоинкрементом). */
    int32_t v = (int32_t)((uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16));
    if (v & 0x00800000) v -= 0x01000000;   /* расширение знака 24->32 */
    *out = v;
    return ESP_OK;
}

esp_err_t es9039_fir_apply(const es9039_fir_set_t *set, bool verify)
{
    if (!set) return ESP_ERR_INVALID_ARG;
    s_active_set = set;   /* запоминаем для предупреждений в apply_for_fs */
    /* 1) гарантированно выключить EN перед заливкой */
    esp_err_t e;
    if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_CTRL, 0x00)) != ESP_OK) return e;
    /* 2) ступень 2x — 128 коэфф (главный брикволл) */
    if ((e = es9039_fir_load_stage(0, set->taps2x, ES9039_FIR_N2X)) != ESP_OK) return e;
    /* 3) ступень 4x — 32 коэфф (чистовая) */
    if ((e = es9039_fir_load_stage(1, set->taps4x, ES9039_FIR_N4X)) != ESP_OK) return e;
    /* 4) выборочная проверка чтением: сравниваем содержимое RAM по тем же
     * формулам адресации, что использовались при записи */
    if (verify) {
        const size_t probe2[] = {0, 1, 63, 64, 127};   /* индексы прототипа 2x */
        const size_t probe4[] = {0, 1, 15, 16, 31};    /* индексы прототипа 4x */
        for (unsigned i = 0; i < sizeof(probe2)/sizeof(probe2[0]); i++) {
            const int32_t c = set->taps2x[probe2[i]];
            const uint8_t addr = coeff_addr(0, probe2[i] % 2, probe2[i] / 2);
            int32_t v;
            if ((e = es9039_fir_readback(0, addr, &v)) != ESP_OK) return e;
            if (v != c) {
                ESP_LOGE(TAG, "readback 2x p%u k%u (addr %u): %ld != %ld — маппинг/порядок не тот",
                         (unsigned)(probe2[i] % 2), (unsigned)(probe2[i] / 2), addr,
                         (long)v, (long)c);
                return ESP_ERR_INVALID_STATE;
            }
        }
        for (unsigned i = 0; i < sizeof(probe4)/sizeof(probe4[0]); i++) {
            const int32_t c = set->taps4x[probe4[i]];
            const uint8_t addr = coeff_addr(1, probe4[i] % 4, probe4[i] / 4);
            int32_t v;
            if ((e = es9039_fir_readback(1, addr, &v)) != ESP_OK) return e;
            if (v != c) {
                ESP_LOGE(TAG, "readback 4x p%u k%u (addr %u): %ld != %ld — маппинг/порядок не тот",
                         (unsigned)(probe4[i] % 4), (unsigned)(probe4[i] / 4), addr,
                         (long)v, (long)c);
                return ESP_ERR_INVALID_STATE;
            }
        }
        ESP_LOGI(TAG, "readback-проверка пройдена");
    }
    /* 5) включить кастомный фильтр */
    if ((e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_CTRL, 0x01)) != ESP_OK) return e;
    ESP_LOGI(TAG, "загружен фильтр «%s» (img@fs %.1f дБ, img@2fs+ %.1f дБ, ripple %.3f дБ)",
             set->name, set->img_fs_db, set->img_2fs_db, set->ripple_db);
    return ESP_OK;
}

esp_err_t es9039_fir_apply_for_fs(uint32_t fs_hz, bool verify)
{
    if (s_manual_mode) {
        /* Ручной выбор приоритетнее авто: активный набор не меняем. Если
         * набор рассчитан под другую fs — предупредить: брикволл сдвинется
         * (срез верхов выше переходной полосы набора). */
        if (s_active_set && s_active_set->fs_hz != 0 && s_active_set->fs_hz != fs_hz) {
            ESP_LOGW(TAG, "ручной FIR «%s» рассчитан под fs=%lu Гц, поток %lu Гц — "
                          "частота среза сдвинута", s_active_set->name,
                     (unsigned long)s_active_set->fs_hz, (unsigned long)fs_hz);
        }
        return ESP_OK;
    }

    const es9039_fir_set_t *set = NULL;
    for (int i = 0; i < es9039_fir_sets_count; i++) {
        if (es9039_fir_sets[i].fs_hz == fs_hz) { set = &es9039_fir_sets[i]; break; }
    }
    if (!set) {
        /* Наборы есть для 44.1/48/88.2/96/176.4/192/352.8/384 кГц; сюда попадают
         * прочие частоты (например DoP256 = 705.6 кГц) — оставляем встроенный
         * пресет: RAM чипа перезаливать наугад опаснее, чем пресет из Reg 88. */
        ESP_LOGW(TAG, "нет набора FIR для fs=%lu Гц — возвращаюсь к встроенному пресету",
                 (unsigned long)fs_hz);
        esp_err_t e = es9039_fir_use_builtin();
        return (e == ESP_OK) ? ESP_ERR_NOT_FOUND : e;
    }
    return es9039_fir_apply(set, verify);
}

esp_err_t es9039_fir_use_builtin(void)
{
    esp_err_t e = FIR_WRITE_REG(ES9039_REG_PROG_RAM_CTRL, 0x00);
    if (e == ESP_OK) {
        s_active_set = NULL;
        ESP_LOGI(TAG, "встроенный пресет (Reg 88 FILTER_SHAPE)");
    }
    return e;
}

esp_err_t es9039_fir_load_delta(void)
{
    /* Диагностическая «прозрачная» загрузка: единичный коэффициент в фазе 0,
     * тапе 0 обеих ступеней (Q1.23: FULL = 1.0). Суммарный гейн тракта = 1
     * вместо 8 (на −18 дБ) — для диагностики формы АЧХ и структуры это учтено:
     * сравнивать форму, а не абсолютный уровень. */
    static int32_t d2[ES9039_FIR_N2X], d4[ES9039_FIR_N4X];
    memset(d2, 0, sizeof(d2)); memset(d4, 0, sizeof(d4));
    d2[0] = 8388607;   /* 1.0 в Q1.23 */
    d4[0] = 8388607;
    const es9039_fir_set_t delta = {
        .name = "DELTA (diagnostic)", .fs_hz = 0,
        .taps2x = d2, .taps4x = d4,
        .img_fs_db = 0.0f, .img_2fs_db = 0.0f, .ripple_db = 0.0f,
    };
    return es9039_fir_apply(&delta, false);
}
