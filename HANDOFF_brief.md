# БРИФ ДЛЯ ПЕРЕДАЧИ ПРОЕКТА (handoff) — дата 24.11.2026

## Проект
audio-dac-p4-3.10-p4d-rc1 — аудиостанция на ESP32-P4 + ЦАП ES9039Q2M.
Функции: аудио-конвейер (FLAC/WAV/DSD/AAC…), радио, Wi-Fi через сопроцессор
ESP32-C6, web-пульт, OTA, USB-хост флешки, SD как USB-диск для ПК, LVGL 9 UI,
кастомные FIR для ES9039Q2M, АЦП-тракт WM8782 (винил, 192 кГц),
native DSD (без DoP) и собственный DST-декодер для DST-сжатых SACD-DFF.

Ключевой модуль: `main/dst_dec.c` + `main/dst_dec.h` — порт DST-декодера
из libavcodec (ffmpeg 8.0/9.1, файлы dst.c/dstdec.c/golomb). Архитектура:
арифм. декодер (get_sr_golomb_dst, 11/15-битные fsets, probs) + ПХ-фильтр
(8 наборов, 128-коэфф. P1/P2, ctables) + предсказание-4/2 + J(t) данных.

## Статус верификации (хост, всё зелёное)
| Проверка | Результат |
|---|---|
| dst_bench --selftest | PASS 3/3, dsd_bytes 28 224/28 224 |
| dst_bench synth.dff --dsd synth_out.bin | DSD MATCH 9408/9408 |
| Эталонный SACD-DFF vs ffmpeg 9.1 | DSD MATCH 94 080/94 080 (биты побайтно) |
| ffmpeg 8.0 vs 9.1 DSD | 94 080/94 080 — версии идентичны |
| PCM vs эталон | 188 160/188 160 в допуске ±1 LSB (14 позиций, канал L) |

P5-7 закрыт 24.11.2026: расхождение ±1 LSB объяснено (эталон шёл через
float32/ties-even, порт — double/round-half-up); в tools/dsd2pcm.py добавлен
режим --s16f32 (путь эталона бит-в-бит). Критерий корректности декодера —
DSD-биты, а не PCM.

## НЕ проверено
- Компиляция под ESP-IDF и работа на железе (весь проект).
- DST на реальных SACD-DFF (сжатые фреймы: fsets/probs/сегментация) —
  верифицированы только несжатые ветки. Первый реальный файл может не
  сойтись — это ожидаемый сценарий, не регресс.

## Как проверять на хосте
    gcc -O2 -o tools/dst_bench tools/dst_bench.c main/dst_dec.c \
        -Imain -Itools/bench_stubs -lm
    ./tools/dst_bench --selftest                        # ожидание: PASS 3/3
    ./tools/dst_bench synth.dff --dsd synth_out.bin     # ожидание: DSD MATCH 9408/9408
    python3 tools/dsd2pcm.py synth_out.bin --s16f32 -o ref.pcm --channels 2
Заглушки ESP-IDF API для хост-сборки: tools/bench_stubs/.

## Документация проекта
- README.md — полное описание (§25 — верификация, «Сборка» — ESP-IDF,
  «Журнал патчей» — история до 3.10-p4d-rc2).
- docs/code_review_plan.md — ревью всех патчей (P1-1…P4-24, статусы в G),
  блок H — план P5.
- docs/ffmpeg_dst_analysis.pdf — отчёт по DST-порту, 6 стр., 17 разделов
  (алгоритм, все коэффициенты, методика, результаты, P5).

## Открытые задачи (блок H плана)
- P5-1 (высокий): прогон 3–5 реальных SACD-DFF с DST-сжатием через
  dst_bench vs эталон ffmpeg 9.1. При несовпадении: локализация до фрейма
  (счётчик в бенче есть, лог каждые 16-й), сверка fsets/probs и
  get_sr_golomb_dst с ffmpeg 9.1. Фикс -> статус P1 -> полный повторный цикл.
- P5-2 (высокий): idf.py set-target esp32p4 && idf.py reconfigure && idf.py build;
  затем flash monitor. dependencies.lock НЕ править. Smoke: USB-флешка,
  web-пульт, native-DSD старт/пауза, счётчик DST-повторов молчит.
- P5-3: native DSD, TX-slave на P4, фазировка двух I2S-портов (осциллограф).
- P5-4: сверка GPIO14–17 и DATA2/DATA_CLK со схемой платы.
- P5-5: OTA-откат (не подтверждать образ, ребут, ждать warning в логе).
- P5-6: GT911 INT, предел BCLK при DSD256, FIR на бенче.

## Правила
- dependencies.lock не править.
- P4-23 (multi-channel DSF) — осознанное решение автора не менять, в P5 не входит.
- Критерий декодера — побайтовое совпадение DSD; PCM сверять с допуском ±1 LSB.
- sdkconfig.defaults применяется только на чистой сборке (REv3, SPIRAM 250 МГц,
  LVGL 9.5, TinyUSB MSC и пр. — уже настроено).

## Первые шаги новой модели
1. Прочитать README.md §25 и docs/code_review_plan.md (блоки G и H).
2. Собрать бенч и прогнать два контрольных теста из раздела выше.
3. Если всё зелёное — состояние подтверждено, работать по P5.
