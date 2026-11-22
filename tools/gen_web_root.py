#!/usr/bin/env python3
"""Генератор main/web_root.h из tools/web_page.html.
Запуск из корня проекта: python tools/gen_web_root.py"""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, 'tools', 'web_page.html'), encoding='utf-8').read()
esc = (src.replace('\\', '\\\\').replace('"', '\\"')
          .replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r'))
units, k = [], 0
while k < len(esc):
    if esc[k] == '\\': units.append(esc[k:k+2]); k += 2
    else: units.append(esc[k]); k += 1
chunks, cur = [], ''
for u in units:
    if len(cur) + len(u) > 76 and cur: chunks.append(cur); cur = ''
    cur += u
if cur: chunks.append(cur)
hdr = ('/* Автогенерировано tools/gen_web_root.py — вручную не править.\n'
       '   Правки страницы: tools/web_page.html, затем из корня проекта:\n'
       '   python tools/gen_web_root.py */\n'
       '#define PAGE_HTML \\\n' + ' \\\n'.join('    "%s"' % c for c in chunks) + '\n')
open(os.path.join(root, 'main', 'web_root.h'), 'w', encoding='utf-8').write(hdr)
print('web_root.h: %d байт, %d чанков' % (len(hdr), len(chunks)))
