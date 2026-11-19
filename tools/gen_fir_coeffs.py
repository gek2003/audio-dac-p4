#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_fir_coeffs.py — генератор стартовых FIR-фильтров для ES9039Q2M
(аналоги авторских C-BRICK и C-SLOW с проекта «Madbit + ES9039SPRO», Drive2).

Модель каскада (рабочая гипотеза, см. docs/FIR_CUSTOM_INTEGRATION.md):
    вход fs -> ступень x2 (128 коэфф на 2fs, главный брикволл)
             -> ступень x4 (32 коэфф на 8fs, чистовая) -> выход 8fs.
Модель подтверждается совпадением групповой задержки штатного «linear phase
fast roll-off» ESS (33.8/fs) с полной длиной каскада (540 тапов на 8fs).

Спецификации:
  C-BRICK: полоса 0..20 кГц ровная, стоп-полоса от 0.55*fs (ступень 2x, как
           штатный fast roll-off ESS) и от 1.5*fs (ступень 4x, зона зеркала
           сигнала вокруг 2fs). Для 96 кГц стоп 2x-ступени берётся с 24 кГц —
           «прибить всё после 20 кГц» как у автора (обеспечивает ~-47 дБ
           подавления образа fs; RAM 128 тапов не даёт больше).
  C-SLOW:  мягкий спад, короткий импульс: 2x-ступень — 40 тапов (центрировано
           в RAM 128), переход 20 кГц..0.72*fs; 4x-ступень — 32 тапа, стоп от
           1.44*fs. Групповая задержка ~14/fs (против 33.8/fs у brick).

Выход:
  - es9039_fir_coeff.h / es9039_fir_coeff.c (C-массивы, Q1.23)
  - fir_<имя>_response.png (АЧХ каждого фильтра)

Зависимости: numpy, scipy, matplotlib.
"""
import numpy as np
from scipy.signal import remez, firwin, freqz
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

FULL = 2**23 - 1          # полный масштаб Q1.23

# ---------------------------------------------------------------- спецификации
# (fs, fstop2 [край стопа 2x-ступени], fstop4 [край стопа 4x-ступени])
BRICK = {44100: (0.55*44100, 1.5*44100),
         48000: (0.55*48000, 1.5*48000),
         88200: (0.55*88200, 1.5*88200),
         96000: (24000,      1.5*96000),
         176400: (0.55*176400, 1.5*176400),
         192000: (0.55*192000, 1.5*192000),
         352800: (0.55*352800, 1.5*352800),
         384000: (0.55*384000, 1.5*384000)}
# C-SLOW: (fs, fstop2, fstop4, эффективные тапы 2x/4x)
SLOW  = {44100: dict(fstop2=0.72*44100, fstop4=1.44*44100, neff2=40, neff4=32),
         48000: dict(fstop2=0.72*48000, fstop4=1.44*48000, neff2=40, neff4=32),
         88200: dict(fstop2=0.72*88200, fstop4=1.44*88200, neff2=40, neff4=32),
         96000: dict(fstop2=0.72*96000, fstop4=1.44*96000, neff2=40, neff4=32),
         176400: dict(fstop2=0.72*176400, fstop4=1.44*176400, neff2=40, neff4=32),
         192000: dict(fstop2=0.72*192000, fstop4=1.44*192000, neff2=40, neff4=32),
         352800: dict(fstop2=0.72*352800, fstop4=1.44*352800, neff2=40, neff4=32),
         384000: dict(fstop2=0.72*384000, fstop4=1.44*384000, neff2=40, neff4=32)}

# ---------------------------------------------------------------- проектирование
def kaiser_beta_from_A(A):
    if A > 50:  return 0.1102*(A-8.7)
    if A >= 21: return 0.5842*(A-21)**0.4 + 0.07886*(A-21)
    return 0.0

def design_stage(N, fpass, fstop, fs_stage, astop=110.0):
    """Линейно-фазовый ФНЧ: пробуем Parks-McClellan с несколькими целями
    подавления, фолбэк — окно Кайзера (beta из достижимой переходной зоны).
    Возвращает лучший вариант по фактическому подавлению в стоп-полосе."""
    cands = []
    for A in [astop, 100, 90, 80, 70, 60]:
        for ripple in [0.5, 1.0, 2.0]:
            try:
                dp = 10**(-ripple/40); ds = 10**(-A/20)
                h = remez(N, [0, fpass, fstop, fs_stage/2], [1, 0],
                          fs=fs_stage, weight=[dp/ds, 1.0], maxiter=250)
                cands.append(h)
            except Exception:
                pass
    dw = 2*np.pi*(fstop-fpass)/fs_stage
    A_f = 2.285*dw*(N-1)+7.95
    cands.append(firwin(N, (fpass+fstop)/2,
                        window=('kaiser', kaiser_beta_from_A(A_f)), fs=fs_stage))
    f = np.linspace(0, fs_stage/2, 32768)
    stop = f >= fstop*1.01
    best, bestA = None, 1e9
    for h in cands:
        _, H = freqz(h, worN=32768, fs=fs_stage)
        a = 20*np.log10(np.maximum(np.abs(H[stop]), 1e-15)).max()
        if a < bestA: bestA, best = a, h
    return best, bestA

def pad_center(h, N):
    out = np.zeros(N); s = (N-len(h))//2
    out[s:s+len(h)] = h
    return out

def quant(h, gain):
    """Нормировка: сумма = gain (гейн интерполяции), квантование в Q1.23.

    Пик узкополосного ФНЧ может выйти за 1.0 в Q1.23 (центральный тап
    brick-фильтра ~1.02 при сумме 2.0). Ограничение просто обрезкой сдвигает
    DC-гейн; вместо этого: пик фиксируется на FULL, излишек перераспределяется
    по остальным тапам пропорционально их модулю — сумма ступени сохраняется
    точно, форма меняется на ~0.1 дБ в стоп-полосе."""
    hn = h/h.sum()*gain
    peak = np.abs(hn).max()
    if peak > 1.0:
        # Пик узкополосного ФНЧ > 1.0 в Q1.23 (свойство: сумма = gain, узкий
        # импульс). Перенормировка по пику сохраняет ФОРМУ и подавление,
        # снижая общий гейн на ~0.2 дБ (компенсируется уровнем трека/кгейном
        # тракта; сумма ступени тогда < R*FULL — учтено при калибровке).
        hn = hn / peak
    return np.round(hn*FULL).astype(np.int64)

def sim_cascade(fs, q2, q4):
    """Честная симуляция каскада с ЦЕЛОЧИСЛЕННЫМИ коэффициентами:
    дельта -> zero-stuff x2 + conv q2 -> zero-stuff x4 + conv q4 -> АЧХ на 8fs."""
    x = np.zeros(4096); x[0] = 1.0
    u2 = np.zeros(len(x)*2); u2[::2] = x
    y2 = np.convolve(u2, q2)[:len(u2)]
    u4 = np.zeros(len(y2)*4); u4[::4] = y2
    y8 = np.convolve(u4, q4)[:len(u4)]
    spec = np.fft.rfft(y8, n=16384)
    f = np.fft.rfftfreq(16384, d=1.0/(8*fs))
    mag = 20*np.log10(np.maximum(np.abs(spec), 1e-15) / abs(spec[0]))
    return f, mag

def zm(f, mag, lo, hi):
    m = (f >= lo) & (f <= hi)
    return mag[m].max() if m.sum() else -999.0

# ---------------------------------------------------------------- генерация
def build_sets():
    data, meta = {}, {}
    for fs, (fst2, fst4) in BRICK.items():
        h2, a2 = design_stage(128, 20000, fst2, 2*fs)
        h4, a4 = design_stage(32, 20000, fst4, 8*fs)
        q2, q4 = quant(h2, 2), quant(h4, 4)
        f, mag = sim_cascade(fs, q2, q4)
        band = f <= 20000
        data[("brick", fs)] = (q2, q4)
        meta[("brick", fs)] = dict(ripple=mag[band].max()-mag[band].min(),
                                   img_fs=zm(f, mag, fst2, fs+fs/2),
                                   img_2fs=zm(f, mag, 1.5*fs, 2.5*fs))
    for fs, sp in SLOW.items():
        h2e, a2 = design_stage(sp['neff2'], 20000, sp['fstop2'], 2*fs)
        h4e, a4 = design_stage(sp['neff4'], 20000, sp['fstop4'], 8*fs)
        q2 = quant(pad_center(h2e, 128), 2)
        q4 = quant(pad_center(h4e, 32), 4)
        f, mag = sim_cascade(fs, q2, q4)
        band = f <= 20000
        data[("slow", fs)] = (q2, q4)
        meta[("slow", fs)] = dict(ripple=mag[band].max()-mag[band].min(),
                                  img_fs=zm(f, mag, sp['fstop2'], fs+fs/2),
                                  img_2fs=zm(f, mag, 1.5*fs, 2.5*fs))
    return data, meta

def fmt_int32(arr, per_line=8):
    items = [str(int(v)) for v in arr]
    return "\n".join("    " + ", ".join(items[i:i+per_line]) +
                     ("," if i+per_line < len(items) else "")
                     for i in range(0, len(items), per_line))

HDR = """/*
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
"""

def write_c(data, meta):
    decls, rows = [], []
    for key in sorted(data.keys()):
        q2, q4 = data[key]
        kind, fs = key
        tag = f"{kind}_{fs//1000}k"
        arr2, arr4 = f"es9039_fir_{tag}_2x", f"es9039_fir_{tag}_4x"
        m = meta[(kind, fs)]
        label = ("C-BRICK" if kind == "brick" else "C-SLOW") + f" {fs//1000}k"
        decls.append(f"static const int32_t {arr2}[ES9039_FIR_N2X] = {{\n{fmt_int32(q2)}\n}};\n")
        decls.append(f"static const int32_t {arr4}[ES9039_FIR_N4X] = {{\n{fmt_int32(q4)}\n}};\n")
        rows.append(
            f'    {{ .name = "{label}", .fs_hz = {fs}, .taps2x = {arr2}, .taps4x = {arr4},\n'
            f'      .img_fs_db = {m["img_fs"]:.1f}f, .img_2fs_db = {m["img_2fs"]:.1f}f, '
            f'.ripple_db = {m["ripple"]:.3f}f }},')
    body = f"""/*
 * es9039_fir_coeff.c — данные коэффициентов (сгенерировано tools/gen_fir_coeffs.py).
 * Модель каскада: вход fs -> ступень x2 (128 коэфф на 2fs) -> ступень x4 (32 коэфф на 8fs).
 * TODO(структура): полифазный маппинг RAM (interleaved/block) и порядок ступеней
 * не документированы ESS; уточнить по процедуре в docs/FIR_CUSTOM_INTEGRATION.md.
 */
#include "es9039_fir_coeff.h"

{chr(10).join(decls)}
const es9039_fir_set_t es9039_fir_sets[] = {{
{chr(10).join(rows)}
}};

const int es9039_fir_sets_count = sizeof(es9039_fir_sets) / sizeof(es9039_fir_sets[0]);
"""
    return body

def plot(data, meta):
    colors = {44100: 'tab:blue', 48000: 'tab:orange', 96000: 'tab:green'}
    for label, title in [("brick", "C-BRICK"), ("slow", "C-SLOW")]:
        fig, axes = plt.subplots(1, 2, figsize=(14, 5))
        for fs in [44100, 48000, 96000]:
            q2, q4 = data[(label, fs)]
            f, mag = sim_cascade(fs, q2, q4)
            name = f"{title} {fs//1000}k"
            m = f <= 4*fs
            axes[0].plot(f[m]/1000, mag[m], color=colors[fs], lw=1.0, label=name)
            m2 = (f >= 100) & (f <= 25000)
            axes[1].plot(f[m2]/1000, mag[m2], color=colors[fs], lw=1.0, label=name)
        axes[0].set_ylim(-140, 5); axes[0].set_xlim(0, 384)
        axes[0].set_title(f"{title}: полный отклик каскада (отн. DC)")
        axes[0].set_xlabel("кГц"); axes[0].set_ylabel("дБ")
        axes[0].grid(alpha=0.3); axes[0].legend(fontsize=8)
        axes[1].set_ylim(-1.5, 1.0); axes[1].set_xlim(0, 25)
        axes[1].set_title("зум полосы 0..25 кГц")
        axes[1].set_xlabel("кГц"); axes[1].grid(alpha=0.3); axes[1].legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(f"fir_{label}_response.png", dpi=130)
        plt.close(fig)

if __name__ == "__main__":
    data, meta = build_sets()
    body = write_c(data, meta)
    with open("es9039_fir_coeff.h", "w") as fh: fh.write(HDR)
    with open("es9039_fir_coeff.c", "w") as fh: fh.write(body)
    plot(data, meta)
    print("Итоговые показатели (симуляция с целочисленными коэффициентами):")
    for key in sorted(data.keys()):
        m = meta[key]
        kind, fs = key
        print(f"  {kind.upper()} {fs/1000:g}k: ripple {m['ripple']:.3f} дБ, "
              f"образ fs {m['img_fs']:.1f} дБ, образы 2fs+ {m['img_2fs']:.1f} дБ")
    print("Записаны: es9039_fir_coeff.h, es9039_fir_coeff.c, fir_brick_response.png, fir_slow_response.png")
