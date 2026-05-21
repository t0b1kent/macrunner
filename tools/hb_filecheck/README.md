# hb_filecheck

Минимальный FileCheck-style валидатор для HyperBridge trace/log.

## Директивы

- `CHECK: <substring>` — строка должна встретиться.
- `CHECK-NOT: <substring>` — строка не должна встретиться.
- `CHECK-ORDER: a >> b >> c` — `a` перед `b`, `b` перед `c`.
- `CHECK-COUNT: <N> :: <substring>` — подстрока встречается ровно `N` раз.
- `CHECK-REGEX: <python-regex>` — хотя бы одно regex-совпадение.

## Запуск

```bash
python3 tools/hb_filecheck/hb_filecheck.py \
  --input path/to/trace.log \
  --check tools/hb_filecheck/examples/rep_movs_forward.check
```

Код возврата: `0` PASS, `1` FAIL.
