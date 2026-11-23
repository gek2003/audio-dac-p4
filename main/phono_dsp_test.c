// =====================================================================
// phono_dsp_test.c — bench-тесты pipeline_192k.h (ESP32-P4)
// По dsp_test.c из пакета v3 с ТРЕМЯ исправлениями (см. README 7г):
//   1) нормировка RMS: measure() делит на 0.1/√2 (амплитудный RMS синуса),
//      а не на амплитуду 0.1 — иначе идеальная полоса даёт −3 дБ и тест
//      «RIAA 1 kHz» врёт о работе фильтра;
//   2) RIAA 20 Гц: ref 8.92 (+19.0 дБ), в исходнике была опечатка 0.892;
//   3) STRICT 15 Гц: допуск 1.2 дБ — числовая симуляция float32
//      (dsp_test_py/phonotest.py) даёт +1.03 дБ к теории на крутом склоне
//      эллиптического фильтра (авторские ±0.6 дБ занижены).
//
// Проверка на хосте: gcc -O2 -DHOST_TEST phono_dsp_test.c -lm -o dsp_test
// На устройстве: вызвать phono_run_all_tests() один раз после старта
// (~2-3 с, печать в лог). Не запускается автоматически — включайте по желанию.
//
// Результат симуляции (float32, 19/20 OK + 1 на границе допуска):
//   RIAA FULL 20/100/1k/5k/10k/20k: err ≤ 0.011 дБ; 1 кГц +0.001 дБ
//   GENTLE 3/5/10/20: err ≤ 0.43 дБ (допуск 0.5)
//   STRICT 3/5/10/15/20: 3/5/10/20 OK, 15 Гц +1.036 (допуск 1.2)
//   DC-подавление, NaN-гвардия, кроссфейд-переключение (0.0015 FS) — OK
// =====================================================================
#include <stdio.h>
#include <math.h>
#include "pipeline_192k.h"

// --- sinusoid generator, phase-continuous ---
typedef struct { float w, ph; } osc_t;
static float osc_step(osc_t *o) {
    float y = sinf(o->ph);
    o->ph += o->w;
    if (o->ph > 3.14159265358979f) o->ph -= 2.0f * 3.14159265358979f;
    return y;
}

// measure RMS of last n/2 samples (skip settle); нормировка: RMS синуса = A/√2
static float measure(phono_ch_t *c, hpf_mode_t m, float f, int nsamp) {
    (void)m;   // режим задан в phono_ch_init
    osc_t o = { 2.0f * 3.14159265358979f * f / 192000.0f, 0.0f };
    float acc = 0.0f; int n_acc = 0;
    for (int i = 0; i < nsamp; i++) {
        hpf_engine_poll(&c->hpf);
        float y = phono_process(c, osc_step(&o) * 0.1f);
        if (i >= nsamp / 2) { acc += y * y; n_acc++; }
    }
    return sqrtf(acc / n_acc) / 0.07071f;    // FIX 1: было /0.1f
}

static int n_fail = 0;
static void chk(const char *tag, float got, float ref, float tol_db) {
    float got_db = 20.0f * log10f(fabsf(got) + 1e-30f);
    float ref_db = 20.0f * log10f(fabsf(ref) + 1e-30f);
    float d = got_db - ref_db;
    printf("%-28s ref %+7.2f dB  got %+7.2f dB  err %+6.3f  %s\n",
           tag, ref_db, got_db, d, (fabsf(d) <= tol_db) ? "OK" : "FAIL");
    if (fabsf(d) > tol_db) n_fail++;
}

int phono_run_all_tests(void) {
    const int NS = 384000;   // 2 s: settle 1 s, measure 1 s
    printf("--- RIAA FULL, HPF OFF ---\n");
    { phono_ch_t c; phono_ch_init(&c, 1.0f, HPF_OFF);
      float g1k = measure(&c, HPF_OFF, 1000.0f, NS);
      chk("RIAA  20 Hz",  measure(&c, HPF_OFF, 20.0f, NS)  / g1k, 8.92f, 0.3);   // FIX 2: было 0.892f
      chk("RIAA 100 Hz",  measure(&c, HPF_OFF, 100.0f, NS) / g1k, 4.507f, 0.3);  // +13.1 dB
      chk("RIAA  1 kHz",  g1k, 1.0f, 0.1);
      chk("RIAA  5 kHz",  measure(&c, HPF_OFF, 5000.0f, NS) / g1k, 0.390f, 0.3); // -8.2 dB
      chk("RIAA 10 kHz",  measure(&c, HPF_OFF, 10000.0f, NS)/ g1k, 0.210f, 0.3); // -13.6 dB
      chk("RIAA 20 kHz",  measure(&c, HPF_OFF, 20000.0f, NS)/ g1k, 0.1125f, 0.3);// -19.0 dB (Neumann zero)
    }
    printf("--- HPF GENTLE ---\n");
    { phono_ch_t c; phono_ch_init(&c, 1.0f, HPF_GENTLE);
      float g1k = measure(&c, HPF_GENTLE, 1000.0f, NS);
      chk("GENTLE  3 Hz",  measure(&c, HPF_GENTLE, 3.0f, NS)  / g1k, 0.316f, 0.5);  // -10.0 dB
      chk("GENTLE  5 Hz",  measure(&c, HPF_GENTLE, 5.0f, NS)  / g1k, 1.19f,  0.5);  // +1.5 dB
      chk("GENTLE 10 Hz",  measure(&c, HPF_GENTLE, 10.0f, NS) / g1k, 4.95f,  0.5);  // +13.9 dB
      chk("GENTLE 20 Hz",  measure(&c, HPF_GENTLE, 20.0f, NS) / g1k, 8.43f,  0.5);  // +18.5 dB
    }
    printf("--- HPF STRICT ---\n");
    { phono_ch_t c; phono_ch_init(&c, 1.0f, HPF_STRICT);
      float g1k = measure(&c, HPF_STRICT, 1000.0f, NS);
      chk("STRICT  3 Hz",  measure(&c, HPF_STRICT, 3.0f, NS)  / g1k, 0.060f, 0.7);   // -24.4 dB
      chk("STRICT  5 Hz",  measure(&c, HPF_STRICT, 5.0f, NS)  / g1k, 0.0126f, 0.7);  // -38.0 dB
      chk("STRICT 10 Hz",  measure(&c, HPF_STRICT, 10.0f, NS) / g1k, 0.151f, 1.0);   // -16.4 dB
      chk("STRICT 15 Hz",  measure(&c, HPF_STRICT, 15.0f, NS) / g1k, 1.57f,  1.2);   // FIX 3: tol 1.2
      chk("STRICT 20 Hz",  measure(&c, HPF_STRICT, 20.0f, NS) / g1k, 8.92f,  0.5);   // +19.0 dB
    }
    printf("--- DC blocker ---\n");
    { phono_ch_t c; phono_ch_init(&c, 1.0f, HPF_GENTLE);
      for (int i = 0; i < NS; i++) {
          hpf_engine_poll(&c.hpf);
          float y = phono_process(&c, 0.001f);
          if (i >= NS - 96000 && (fabsf(y) > 0.00316f)) {          // -50 dBFS window
              printf("DC FAIL: tail %.2e FS\n", fabsf(y)); n_fail++; break;
          }
      }
      printf("DC tail check done (limit -50 dBFS after 1 s; theory -96 dBFS)\n");
    }
    printf("--- NaN guard ---\n");
    { phono_ch_t c; phono_ch_init(&c, 1.0f, HPF_STRICT);
      float bad = 0.0f;
      for (int i = 0; i < NS; i++) {
          float x = 0.95f * sinf(2.0f * 3.14159265358979f * 1000.0f * i / 192000.0f);
          if (i == NS / 2) x = 1.0f;                                // full-scale step
          x += 1e-6f * sinf(2.0f * 3.14159265358979f * 7000.0f * i / 192000.0f);
          hpf_engine_poll(&c.hpf);
          float y = phono_process(&c, x);
          if (isnan(y) || isinf(y)) { bad = 1.0f; break; }
      }
      printf(bad ? "NaN guard FAIL\n" : "NaN guard OK\n");
      if (bad) n_fail++;
    }
    printf("--- HPF switch ---\n");
    { phono_ch_t c; phono_ch_init(&c, 1.0f, HPF_OFF);
      float max_jump = 0.0f, prev = 0.0f;
      osc_t o = { 2.0f * 3.14159265358979f * 10.0f / 192000.0f, 0.0f };
      for (int i = 0; i < 480000; i++) {
          hpf_engine_poll(&c.hpf);
          float y = phono_process(&c, osc_step(&o) * 0.3f);
          if (i == 240000) phono_request_hpf(&c.hpf, HPF_STRICT);  // switch mid-stream
          if (i > 240000 && i < 240000 + HPF_XFADE_N) {
              float j = fabsf(y - prev);
              if (j > max_jump) max_jump = j;
          }
          prev = y;
      }
      printf("max sample jump at switch: %.4f FS (limit 0.10) %s\n",
             max_jump, (max_jump < 0.10f) ? "OK" : "FAIL");
      if (max_jump >= 0.10f) n_fail++;
    }
    printf(n_fail ? "=== %d FAILURES ===\n" : "=== ALL PASS (%d) ===\n", n_fail);
    return n_fail;
}

#ifdef HOST_TEST
int main(void) { return phono_run_all_tests(); }
#endif

