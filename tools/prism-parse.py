#!/usr/bin/env python3
"""Разбор двоичных файлов Prism: экспорт, импорт, секции - посимвольно.

Зачем свой разборщик, а не готовый. Нужен ровно один срез: ЧТО экспортирует их модуль против
того, что экспортирует наш. Это буквальный контракт, который от нас ждёт загрузчик Windows, и
сверка поимённо показывает, чего у нас нет, без догадок. Готовые средства дают либо больше, либо
не тот формат, а зависимость ставить незачем - формат PE здесь читается сотней строк.

Открытия, ради которых это писалось:
  * трансляторов ПЯТЬ: xtajit, xtajit64, xtajit64se, xtajitf, xtajitse;
  * игра грузит xtajit64se.dll, а мы реализуем xtajit64.dll;
  * есть ОТДЕЛЬНЫЕ AOT-компиляторы xtac.exe / xtac64.exe / xtac64se.exe, которых у нас нет
    как класса - у нас только JIT.

Запуск:  python3 tools/prism-parse.py reports/prism/файлы [--сравнить-с <наш.so>]
"""
import struct, sys, os, glob

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

MACHINE = {0x8664: 'x64', 0x14c: 'x86', 0xAA64: 'ARM64', 0x1c4: 'ARM', 0xA641: 'ARM64EC'}

class PE:
    def __init__(self, path):
        self.path = path
        self.b = open(path, 'rb').read()
        self.ok = False
        if self.b[:2] != b'MZ':
            self.err = 'не MZ'; return
        pe = u32(self.b, 0x3C)
        if self.b[pe:pe+4] != b'PE\0\0':
            self.err = 'не PE'; return
        self.machine = u16(self.b, pe + 4)
        nsec = u16(self.b, pe + 6)
        optsz = u16(self.b, pe + 20)
        opt = pe + 24
        self.magic = u16(self.b, opt)
        # PE32+ (0x20B) и PE32 (0x10B) отличаются раскладкой каталогов данных.
        ddoff = opt + (112 if self.magic == 0x20B else 96)
        self.dirs = [(u32(self.b, ddoff + i*8), u32(self.b, ddoff + i*8 + 4)) for i in range(16)]
        self.sections = []
        so = opt + optsz
        for i in range(nsec):
            o = so + i*40
            name = self.b[o:o+8].rstrip(b'\0').decode('ascii', 'replace')
            self.sections.append({
                'name': name, 'vsize': u32(self.b, o+8), 'vaddr': u32(self.b, o+12),
                'rsize': u32(self.b, o+16), 'raddr': u32(self.b, o+20),
                'chars': u32(self.b, o+36)})
        self.ok = True

    def rva2off(self, rva):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rsize']):
                return s['raddr'] + (rva - s['vaddr'])
        return None

    def cstr(self, off):
        if off is None or off >= len(self.b): return ''
        e = self.b.index(b'\0', off)
        return self.b[off:e].decode('ascii', 'replace')

    def exports(self):
        rva, size = self.dirs[0]
        if not rva: return []
        o = self.rva2off(rva)
        if o is None: return []
        nnames = u32(self.b, o + 24)
        anames = u32(self.b, o + 32)
        aords  = u32(self.b, o + 36)
        afuncs = u32(self.b, o + 28)
        base   = u32(self.b, o + 16)
        out = []
        no = self.rva2off(anames); oo = self.rva2off(aords); fo = self.rva2off(afuncs)
        if no is None: return []
        for i in range(nnames):
            nm = self.cstr(self.rva2off(u32(self.b, no + i*4)))
            ordi = u16(self.b, oo + i*2) if oo is not None else i
            faddr = u32(self.b, fo + ordi*4) if fo is not None else 0
            out.append((ordi + base, nm, faddr))
        return sorted(out, key=lambda x: x[1].lower())

    def imports(self):
        rva, size = self.dirs[1]
        if not rva: return {}
        o = self.rva2off(rva)
        if o is None: return {}
        res = {}
        while True:
            oft, _, _, nameRva, first = struct.unpack_from('<IIIII', self.b, o)
            if not nameRva: break
            dll = self.cstr(self.rva2off(nameRva))
            fns = []
            t = self.rva2off(oft or first)
            if t is not None:
                while True:
                    v = struct.unpack_from('<Q' if self.magic == 0x20B else '<I', self.b, t)[0]
                    if not v: break
                    top = (1 << 63) if self.magic == 0x20B else (1 << 31)
                    if v & top:
                        fns.append(f'#{v & 0xFFFF}')
                    else:
                        ho = self.rva2off(v & 0x7FFFFFFF)
                        fns.append(self.cstr(ho + 2) if ho else '?')
                    t += 8 if self.magic == 0x20B else 4
            res[dll] = fns
            o += 20
        return res

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'reports/prism/файлы'
    files = sorted(glob.glob(os.path.join(d, '*.dll')) + glob.glob(os.path.join(d, '*.exe')))
    if not files:
        print(f'В {d} нет .dll/.exe — сначала выгрузи их prism-extract.ps1'); return

    print(f'{"="*78}\nРАЗБОР {len(files)} файлов из {d}\n{"="*78}')
    allexp = {}
    for f in files:
        pe = PE(f)
        n = os.path.basename(f)
        if not pe.ok:
            print(f'\n-- {n}: {pe.err}'); continue
        exp = pe.exports(); imp = pe.imports()
        allexp[n] = exp
        print(f'\n{"-"*78}\n{n}  [{MACHINE.get(pe.machine, hex(pe.machine))}]  '
              f'{len(pe.b):,} байт  экспорт={len(exp)}  импорт из {len(imp)} модулей')
        print('  секции: ' + ', '.join(f"{s['name']}({s['vsize']:,})" for s in pe.sections))
        if exp:
            print(f'  ЭКСПОРТ ({len(exp)}):')
            for o, nm, fa in exp:
                print(f'    @{o:<5} 0x{fa:08X}  {nm}')
        if imp:
            print('  ИМПОРТ:')
            for dll, fns in sorted(imp.items()):
                print(f'    {dll} ({len(fns)}): ' + ', '.join(fns[:10]) +
                      (' …' if len(fns) > 10 else ''))

    # Сравнение вариантов транслятора между собой: что есть в se и чего нет в базовом.
    print(f'\n{"="*78}\nСРАВНЕНИЕ ВАРИАНТОВ ТРАНСЛЯТОРА\n{"="*78}')
    for a, b in (('xtajit64.dll', 'xtajit64se.dll'), ('xtajit.dll', 'xtajitse.dll'),
                 ('xtajit.dll', 'xtajitf.dll')):
        if a in allexp and b in allexp:
            sa = {n for _, n, _ in allexp[a]}
            sb = {n for _, n, _ in allexp[b]}
            print(f'\n{a} ({len(sa)}) против {b} ({len(sb)}):')
            only_b = sorted(sb - sa); only_a = sorted(sa - sb)
            print(f'  только в {b} ({len(only_b)}): ' + (', '.join(only_b) if only_b else '—'))
            print(f'  только в {a} ({len(only_a)}): ' + (', '.join(only_a) if only_a else '—'))
            print(f'  общих: {len(sa & sb)}')

if __name__ == '__main__':
    main()
