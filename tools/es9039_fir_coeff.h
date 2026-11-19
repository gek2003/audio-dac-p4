/*
 * es9039_fir_coeff.h — коэффициенты пользовательских FIR-фильтров ES9039Q2M.
 *
 * Сгенерировано tools/gen_fir_coeffs.py (Parks-McClellan / окно Кайзера).
 * Формат: 24-битные знаковые коэффициенты (Q1.23), полный масштаб = 2^23-1.
 * Нормировка: сумма коэффициентов ступени = R * (2^23-1), где R — коэффициент
 * интерполяции ступени (сумма(2x) = 2*FULL, сумма(4x) = 4*FULL).
 *
 * TODO(масштаб): внутренняя нормализация RAM чипа не документирована (app note
 * только у дистрибьютора). После первой загрузки сравнить уровень на 1 кГц с
 * встроенным фильтром; расхождение скорректировать в es9039_fir.c
 * (константа ES9039_FIR_GAIN_SHIFT, процедура калибровки в docs/FIR_CUSTOM_INTEGRATION.md).
 */
#ifndef ES9039_FIR_COEFF_H
#define ES9039_FIR_COEFF_H

#include <stdint.h>
#include <stddef.h>

#define ES9039_FIR_N2X 128   /* ступень x2: RAM-адреса 0x00..0x7F (Reg137 stage=0) */
#define ES9039_FIR_N4X 32    /* ступень x4: RAM-адреса 0x00..0x1F (Reg137 stage=1) */

typedef struct {
    const char    *name;       /* человекочитаемое имя */
    uint32_t       fs_hz;      /* частота дискретизации ВХОДНОГО потока */
    const int32_t *taps2x;     /* 128 коэфф, ступень x2 (главный брикволл) */
    const int32_t *taps4x;     /* 32 коэфф, ступень x4 (чистовая) */
    float          img_fs_db;  /* оценка подавления образа fs, дБ */
    float          img_2fs_db; /* оценка подавления образов 2fs и выше, дБ */
    float          ripple_db;  /* неравномерность 0..20 кГц, дБ */
} es9039_fir_set_t;

extern const es9039_fir_set_t es9039_fir_sets[];
extern const int              es9039_fir_sets_count;

#endif /* ES9039_FIR_COEFF_H */
