// =====================================================================
// pipeline_192k.h — Phono DSP core for ESP32-P4, fs = 192000 Hz
// ADC: WM8782 module in MASTER mode (onboard 24.576 MHz xtal, 128fs)
// MCU: I2S RX slave. Channel chain: trim -> DC-block -> RIAA FULL ->
//      HPF (OFF/GENTLE/STRICT, runtime switch with 10 ms crossfade)
//      -> peak monitor
// All arithmetic: float32. Normalization: 0 dB @ 1 kHz.
//
// Version 3 (2026-10-30). Verification (float64 vs IEC 60098 analog ref):
//   RIAA FULL max |err| = 0.0003 dB over 10 Hz..20 kHz
//   Constants reproduced by fresh scipy design: GENTLE 0.002 dB, STRICT 0.0006 dB
//   Unfolded biquad form in float32: 0.0003 dB (standard DF1 form: up to 46 dB error!)
//   (перепроверено числовой симуляцией float32, см. README раздел 7г)
//
// FIXED SOLUTIONS (do not re-tune):
//   - RIAA = FULL curve only (IEC 60098 incl. Neumann 3.18 us pole). STD removed.
//   - HPF: runtime selectable OFF / GENTLE / STRICT, default GENTLE.
// =====================================================================
#ifndef PIPELINE_192K_H_
#define PIPELINE_192K_H_

#include <stdint.h>
#include <math.h>

// ================= 1. RIAA FULL (IEC 60098, with Neumann 3.18 us) =================
// Two DF1 sections. sec1: bass pole/zero (3180/318 us). sec2: HF pole (75 us)
// + Neumann zero (3.18 us). max |err| = 0.0003 dB vs analog reference.
#define RIAA_S1_B0  0.996894032014f
#define RIAA_S1_B1 -0.980698760448f
#define RIAA_S1_A1 -0.998363499127f
#define RIAA_S2_B0  0.082735343194f
#define RIAA_S2_B1 -0.015640689213f
#define RIAA_S2_A1 -0.932905346018f

typedef struct { float x1, y1; } riaa1_t;   // first-order DF1 state

static inline float riaa1_step(riaa1_t *s, float b0, float b1, float a1, float x) {
    float y = b0 * x + b1 * s->x1 - a1 * s->y1;
    s->x1 = x; s->y1 = y;
    return y;
}

// ================= 2. Subsonic HPF section (unfolded float32-safe form) =================
// Denominator stored as: A(z) = (1 - 2z^-1 + z^-2) + e1*z^-1 - e2*z^-2
// Numerator:             B(z) = g*(1 - 2z^-1 + z^-2) + cb*z^-1
//   - e1, e2 are SMALL (~1e-3): float32 keeps them to ~1e-11 -> pole frequency stable
//   - cb (elliptic notch zero offset from DC) is ~1e-7..1e-8: exact at float32
//   - factor 2 is exact in float32
// Recursion (VERIFIED, v3):
//   num = g*(x - 2*x1 + x2) + cb*x1;
//   y   = num + 2*y1 - y2 - e1*y1 + e2*y2;
typedef struct { float g, cb, e1, e2; float x1, x2, y1, y2; } hpf_sec_t;

static inline float hpf_sec_step(hpf_sec_t *s, float x) {
    float num = s->g * (x - 2.0f * s->x1 + s->x2) + s->cb * s->x1;
    float y = num + 2.0f * s->y1 - s->y2 - s->e1 * s->y1 + s->e2 * s->y2;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

static inline void hpf_sec_reset(hpf_sec_t *s) {
    s->x1 = s->x2 = s->y1 = s->y2 = 0.0f;
}

// ---- GENTLE: butter2 @ 12 Hz. Response: 3Hz -24.1 / 5Hz -15.3 / 10Hz -4.9 / 20Hz -0.5 dB
#define HPF_GENTLE_G   0.999722358366f
#define HPF_GENTLE_CB  0.0f
#define HPF_GENTLE_E1  5.553603530017e-04f
#define HPF_GENTLE_E2  5.552061832448e-04f

// ---- STRICT: ellip4, Rp 0.2 dB, Rs 35 dB, passband edge 20 Hz ----
// Response: 3Hz -38.7 / 5Hz -55.1 / 8Hz -36.7 / 10Hz -35.4 / 12Hz -46.7 /
//           15Hz -15.4 / 17Hz -6.1 / 20Hz -0.2 dB. Tonearm resonance (8-12 Hz) killed 35-47 dB.
#define HPF_STRICT_S1_G   0.976560813826f
#define HPF_STRICT_S1_CB  2.945223487316e-08f
#define HPF_STRICT_S1_E1  1.200204908426e-03f
#define HPF_STRICT_S1_E2  1.199534842155e-03f
#define HPF_STRICT_S2_G   1.0f
#define HPF_STRICT_S2_CB  1.482404068387e-07f
#define HPF_STRICT_S2_E1  1.848386015479e-04f
#define HPF_STRICT_S2_E2  1.844721917307e-04f

// ================= 3. DC blocker (one-pole 5 Hz, always on) =================
// RIAA has +19.9 dB DC gain: ADC offset must be removed BEFORE RIAA.
// y = x - x1 + (1 - e)*y1
#define DC_E 1.636112315968e-04f
typedef struct { float x1, y1; } dcb_t;
static inline float dcb_step(dcb_t *s, float x) {
    float y = x - s->x1 + (1.0f - DC_E) * s->y1;
    s->x1 = x; s->y1 = y;
    return y;
}

// ================= 4. HPF engine: two banks + 10 ms crossfade =================
// Runtime switching without clicks: bank A holds the old mode, bank B starts
// the new one; output = (1-a)*old + a*new, a ramps linearly over 1920 samples.
typedef enum { HPF_OFF = 0, HPF_GENTLE = 1, HPF_STRICT = 2 } hpf_mode_t;
#define N_HPF_BANKS 2
#define HPF_XFADE_N 1920          // 10 ms at 192 kHz

typedef struct {
    hpf_sec_t sec[2];             // STRICT uses both; GENTLE uses sec[0]
    hpf_mode_t mode;
} hpf_bank_t;

typedef struct {
    hpf_bank_t bank[N_HPF_BANKS];
    int      cur;                 // active bank index
    hpf_mode_t active;            // mode of active bank
    hpf_mode_t pending;           // requested mode (atomic write from GUI/UART)
    int      xf;                  // crossfade counter, 0 = none; 1..N while fading
} hpf_engine_t;

static inline void hpf_bank_load(hpf_bank_t *bk, hpf_mode_t m) {
    hpf_sec_reset(&bk->sec[0]); hpf_sec_reset(&bk->sec[1]);
    bk->mode = m;
}

static inline void hpf_engine_init(hpf_engine_t *e, hpf_mode_t def) {
    hpf_bank_load(&e->bank[0], def);
    e->cur = 0; e->active = def; e->pending = def; e->xf = 0;
}

static inline float hpf_bank_step(hpf_bank_t *bk, float x) {
    if (bk->mode == HPF_OFF) return x;
    if (bk->mode == HPF_GENTLE) {
        bk->sec[0].g = HPF_GENTLE_G; bk->sec[0].cb = HPF_GENTLE_CB;
        bk->sec[0].e1 = HPF_GENTLE_E1; bk->sec[0].e2 = HPF_GENTLE_E2;
        return hpf_sec_step(&bk->sec[0], x);
    }
    bk->sec[0].g = HPF_STRICT_S1_G; bk->sec[0].cb = HPF_STRICT_S1_CB;
    bk->sec[0].e1 = HPF_STRICT_S1_E1; bk->sec[0].e2 = HPF_STRICT_S1_E2;
    bk->sec[1].g = HPF_STRICT_S2_G; bk->sec[1].cb = HPF_STRICT_S2_CB;
    bk->sec[1].e1 = HPF_STRICT_S2_E1; bk->sec[1].e2 = HPF_STRICT_S2_E2;
    return hpf_sec_step(&bk->sec[1], hpf_sec_step(&bk->sec[0], x));
}

// Call once per block boundary (see audio_task loop in README)
static inline void hpf_engine_poll(hpf_engine_t *e) {
    if (e->pending == e->active) return;
    int nxt = 1 - e->cur;
    hpf_bank_load(&e->bank[nxt], e->pending);
    e->cur = nxt;
    e->active = e->pending;
    e->xf = HPF_XFADE_N;          // both banks live during ramp
}

static inline float hpf_engine_step(hpf_engine_t *e, float x) {
    float y = hpf_bank_step(&e->bank[e->cur], x);
    if (e->xf > 0) {
        // old bank still runs; fade coefficient
        float a = (float)(HPF_XFADE_N - e->xf) / (float)HPF_XFADE_N;
        float yo = hpf_bank_step(&e->bank[1 - e->cur], x);
        y = a * y + (1.0f - a) * yo;
        e->xf--;
        if (e->xf == 0) hpf_sec_reset(&e->bank[1 - e->cur].sec[0]),
                        hpf_sec_reset(&e->bank[1 - e->cur].sec[1]);
    }
    return y;
}

// GUI/UART entry point: request new mode, applied at next block boundary
static inline void phono_request_hpf(hpf_engine_t *e, hpf_mode_t m) {
    e->pending = m;
}

// ================= 5. Peak monitor =================
// Archive mode: monitor only, signal untouched. Limiter OFF by default.
// clip: |x| >= 0.9440 (-0.5 dBFS, calc. max click) ; warn: |x| >= 0.8913 (-1.0 dBFS)
typedef struct { float peak; uint32_t n_warn, n_clip; } peakmon_t;
static inline float peakmon_step(peakmon_t *p, float x) {
    float ax = fabsf(x);
    if (ax > p->peak) p->peak = ax;
    if (ax >= 0.9440f) p->n_clip++;
    else if (ax >= 0.8913f) p->n_warn++;
    return x;
}

// ================= 6. Channel =================
typedef struct {
    dcb_t        dcb;
    riaa1_t      rs1, rs2;
    hpf_engine_t hpf;
    peakmon_t    peak;
    float        trim;            // input trim (linear), set from calibration
} phono_ch_t;

static inline void phono_ch_init(phono_ch_t *c, float trim, hpf_mode_t def) {
    c->dcb.x1 = c->dcb.y1 = 0.0f;
    c->rs1.x1 = c->rs1.y1 = 0.0f;
    c->rs2.x1 = c->rs2.y1 = 0.0f;
    c->trim = trim;
    c->peak.peak = 0.0f; c->peak.n_warn = 0; c->peak.n_clip = 0;
    hpf_engine_init(&c->hpf, def);
}

static inline float phono_process(phono_ch_t *c, float x) {
    x *= c->trim;
    x = dcb_step(&c->dcb, x);
    x = riaa1_step(&c->rs1, RIAA_S1_B0, RIAA_S1_B1, RIAA_S1_A1, x);
    x = riaa1_step(&c->rs2, RIAA_S2_B0, RIAA_S2_B1, RIAA_S2_A1, x);
    x = hpf_engine_step(&c->hpf, x);
    return peakmon_step(&c->peak, x);
}

#endif // PIPELINE_192K_H_
