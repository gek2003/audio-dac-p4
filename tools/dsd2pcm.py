#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dsd2pcm.py — эталонная DSD->PCM конверсия (референс для tools/dst_bench.c).

Дословный порт libavcodec/dsd.c (DSF/DSDIFF) от FFmpeg 8.0,
основан на BSD-лицензированном dsd2pcm Себастьяна Геземанна:
  ff_init_dsd_data()      — построение ctables (HTAPS=48 -> CTABLES=6)
  ff_dsd2pcm_translate()  — конверсия: двойной точности, FIFO 16 байт,
                            контекст = симметричный 96-tap фильтр (2я половина)

Использование:
  python3 dsd2pcm.py dense_dsd.bin -o out.pcm --channels 2 --lsbf 0
    --s16 out_s16.raw   (int16 LE: clip(round(x*32768)), double, round-half-up)
  --s16f32 out.raw    (int16 LE: путь ffmpeg/swr — double->float32 (fltp),
                       x*2^15 точно, llrint = round-half-even, clip; P5-7)
"""
import sys, math, struct

HTAPS = [
 0.09950731974056658, 0.09562845727714668, 0.08819647126516944,
 0.07782552527068175, 0.06534876523171299, 0.05172629311427257,
 0.0379429484910187, 0.02490921351762261, 0.0133774746265897,
 0.003883043418804416, -0.003284703416210726, -0.008080250212687497,
 -0.01067241812471033, -0.01139427235000863, -0.0106813877974587,
 -0.009007905078766049, -0.006828859761015335, -0.004535184322001496,
 -0.002425035959059578, -0.0006922187080790708, 0.0005700762133516592,
 0.001353838005269448, 0.001713709169690937, 0.001742046839472948,
 0.001545601648013235, 0.001226696225277855, 0.0008704322683580222,
 0.0005381636200535649, 0.000266446345425276, 7.002968738383528e-05,
 -5.279407053811266e-05, -0.0001140625650874684, -0.0001304796361231895,
 -0.0001189970287491285, -9.396247155265073e-05, -6.577634378272832e-05,
 -4.07492895872535e-05, -2.17407957554587e-05, -9.163058931391722e-06,
 -2.017460145032201e-06, 1.249721855219005e-06, 2.166655190537392e-06,
 1.930520892991082e-06, 1.319400334374195e-06, 7.410039764949091e-07,
 3.423230509967409e-07, 1.244182214744588e-07, 3.130441005359396e-08]
HTAPS_N = len(HTAPS); assert HTAPS_N == 48
CTABLES = (HTAPS_N + 7) // 8          # 6
FIFOSIZE = 16; FIFOMASK = 15
REVERSE = [int(format(i, '08b')[::-1], 2) for i in range(256)]

def _tableinit():
    msbf = [[0.0]*256 for _ in range(CTABLES)]
    lsbf = [[0.0]*256 for _ in range(CTABLES)]
    for e in range(256):
        acc = [0.0]*CTABLES
        for m in range(8):
            sign = ((e >> (7-m)) & 1) * 2 - 1
            for t in range(CTABLES):
                acc[t] += sign * HTAPS[t*8 + m]
        for t in range(CTABLES):
            msbf[CTABLES-1-t][e] = acc[t]
            lsbf[CTABLES-1-t][REVERSE[e]] = acc[t]
    return msbf, lsbf

_MS, _LS = _tableinit()

class Dsd2Pcm:
    """Состояние одного канала (аналог DSDContext)."""
    def __init__(self, lsbf=0):
        self.lsbf = lsbf
        self.buf = [0]*FIFOSIZE      # FIFO уходаящих сэмплов
        self.pos = 0
        self.ctables = _LS if lsbf else _MS

    def translate(self, src, samples, stride=1, src_off=0):
        """src — bytes (плотный/интерлив), возвращает list[float] длиной samples."""
        ct = self.ctables; buf = self.buf; pos = self.pos
        rev = REVERSE; out = [0.0]*samples
        for i in range(samples):
            byte = src[src_off + i*stride]
            buf[pos] = rev[byte] if self.lsbf else byte
            p = buf[(pos - CTABLES) & FIFOMASK]
            buf[(pos - CTABLES) & FIFOMASK] = rev[p]   # реверс уходящего байта
            s = 0.0
            for j in range(CTABLES):
                a = buf[(pos - j) & FIFOMASK]
                b = buf[(pos - (CTABLES*2 - 1) + j) & FIFOMASK]
                s += ct[j][a] + ct[j][b]
            out[i] = s
            pos = (pos + 1) & FIFOMASK
        self.pos = pos
        return out

def convert_dense(dsd_bytes, channels, lsbf=0):
    """Плотный интерлив-DSD -> интерлив float. Возвращает [канал_0, канал_1...] и общий интерлив."""
    n = len(dsd_bytes)//channels
    chans = []
    inter = [0.0]*(n*channels)
    for ch in range(channels):
        d = Dsd2Pcm(lsbf)
        vals = d.translate(dsd_bytes, n, channels, ch)
        chans.append(vals)
        for i, v in enumerate(vals):
            inter[i*channels + ch] = v
    return chans, inter

def s16(x):
    v = math.floor(x*32768 + 0.5)   # round-half-up, как numpy round в FATE-режиме
    if v > 32767: v = 32767
    if v < -32768: v = -32768
    return v

def f32(x):
    """Квантование в float32 (как AV_SAMPLE_FMT_FLTP в ffmpeg)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]

def s16f32(x):
    """Путь эталона ffmpeg/swr (P5-7): float32-квантование значения, затем
    x*2^15 (умножение на степень двойки — точное), llrint = round-half-even
    (к чётному соседу на границе k+0.5), затем clip в int16."""
    t = f32(x) * 32768.0
    fr = t - math.floor(t)
    if fr == 0.5:                    # точная граница полшага: ties-even
        f0 = math.floor(t)
        v = f0 if (f0 % 2) == 0 else f0 + 1
    else:
        v = math.floor(t + 0.5)
    if v > 32767: v = 32767
    if v < -32768: v = -32768
    return v

def main(argv):
    inp=None; out=None; s16out=None; s16f32out=None; ch=2; lsbf=0
    i=1
    while i < len(argv):
        a=argv[i]
        if a=='-o' and i+1<len(argv): out=argv[i+1]; i+=2
        elif a=='--s16' and i+1<len(argv): s16out=argv[i+1]; i+=2
        elif a=='--s16f32' and i+1<len(argv): s16f32out=argv[i+1]; i+=2
        elif a=='--channels' and i+1<len(argv): ch=int(argv[i+1]); i+=2
        elif a=='--lsbf' and i+1<len(argv): lsbf=int(argv[i+1]); i+=2
        elif inp is None: inp=argv[i]; i+=1
        else: print(__doc__); return 2
    if not inp: print(__doc__); return 2
    data=open(inp,'rb').read()
    chans,inter = convert_dense(data, ch, lsbf)
    if out:
        import struct
        with open(out,'wb') as f:
            f.write(struct.pack('<%df'%len(inter), *inter))
    if s16out:
        import struct
        with open(s16out,'wb') as f:
            f.write(struct.pack('<%dh'%len(inter), *[s16(x) for x in inter]))
    if s16f32out:
        import struct
        with open(s16f32out,'wb') as f:
            f.write(struct.pack('<%dh'%len(inter), *[s16f32(x) for x in inter]))
    print('samples/channel=%d channels=%d' % (len(data)//ch, ch))
    return 0

if __name__=='__main__':
    sys.exit(main(sys.argv))
