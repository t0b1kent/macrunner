#!/usr/bin/env python3
"""Разметка наших правок Wine по роду — сравнением с чистым деревом upstream.

Зачем: обновление Wine было лотереей (1248 меток в одном loader.c, дерево взято слепком и не
связано с upstream). Разметка отвечает, что при обновлении выбрасывается, что наше навсегда, и
что надо проверять каждый раз — вдруг там уже починили.

Запуск:  python3 tools/wine-upstream/classify-patches.py [путь-к-чистому-дереву]
По умолчанию берёт .tmp/wine-upstream/wine-11.14

Результат за 04.08: из 22 413 добавленных строк 74% — диагностика, и лишь 18% («прочее»)
требуют ревизии при обновлении.
"""
import subprocess, os, re, sys, collections

U = sys.argv[1] if len(sys.argv) > 1 else '.tmp/wine-upstream/wine-11.14'
OUR = 'engine/wine'
SUB = 'dlls/ntdll'          # расширить, когда появятся правки в других каталогах

DIAG = re.compile(r'TRACE\(|ERR\(|WARN\(|MESSAGE\(|fprintf|signal_writef|_trace_|_census|_meter|_probe\b|_log\b|трасс|печат|прибор|диагност', re.I)
PLAT = re.compile(r'\bx18\b|macOS|__APPLE__|Darwin|XNU|mach_|winemac|GA_ROOT|Rosetta|pthread_jit|MAP_JIT|sigaltstack|kern_return|Apple', re.I)
HB   = re.compile(r'macrunner_hb_|hb_jit|hb_memory|hb_runtime|транслятор|эмулятор|JIT')

def hunks(diff):
    """Непрерывные вставки. Классифицируем кусок ЦЕЛИКОМ: тело диагностической функции на сорок
    строк содержит две печати, и построчная разметка отправила бы 95% её в «прочее»."""
    cur, out = [], []
    for l in diff.splitlines():
        if l.startswith('> '): cur.append(l[2:])
        elif cur: out.append(cur); cur = []
    if cur: out.append(cur)
    return out

files = []
for root, _, fs in os.walk(os.path.join(OUR, SUB)):
    for f in fs:
        if not f.endswith(('.c', '.h')): continue
        p = os.path.join(root, f)
        rel = os.path.relpath(p, OUR)
        if not os.path.exists(os.path.join(U, rel)): continue
        try: t = open(p, errors='replace').read()
        except OSError: continue
        if 'macrunner' in t or 'MacRunner' in t:
            files.append((p, os.path.join(U, rel), rel))

tot, lines, per = collections.Counter(), collections.Counter(), {}
for ours, up, rel in files:
    d = subprocess.run(['diff', up, ours], capture_output=True, text=True).stdout
    c, lc = collections.Counter(), collections.Counter()
    for h in hunks(d):
        txt, n = '\n'.join(h), len(h)
        k = ('диагностика' if DIAG.search(txt) else
             'наша платформа' if PLAT.search(txt) else
             'связка с транслятором' if HB.search(txt) else 'прочее')
        c[k] += 1; lc[k] += n
    per[rel] = (sum(lc.values()), c, lc); tot.update(c); lines.update(lc)

print(f"=== РАЗМЕТКА ПРАВОК: наше дерево против {U} ===\n")
print(f"{'файл':<38} {'строк':>7} {'диагн':>7} {'платф':>7} {'трансл':>7} {'прочее':>7}")
print("-" * 82)
for rel, (n, c, lc) in sorted(per.items(), key=lambda x: -x[1][0]):
    if not n: continue
    print(f"{rel:<38} {n:>7} {lc['диагностика']:>7} {lc['наша платформа']:>7} "
          f"{lc['связка с транслятором']:>7} {lc['прочее']:>7}")
print("-" * 82)
s = sum(lines.values()) or 1
print(f"{'ИТОГО строк':<38} {s:>7} {lines['диагностика']:>7} {lines['наша платформа']:>7} "
      f"{lines['связка с транслятором']:>7} {lines['прочее']:>7}")
print(f"{'ИТОГО кусков':<38} {sum(tot.values()):>7}\n")
for k, v in lines.most_common():
    print(f"  {k:<24} {v:>7} строк  ({100*v/s:.0f}%)   кусков: {tot[k]}")
print("\nЧто с этим делать — reports/research/WINE-PATCH-CLASSIFICATION.md")
