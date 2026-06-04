# Lane A HK 0x51589d disassembly checkpoint - 2026-06-04

## Litmus verdict

Evidence: `reports/phase4-hollow-knight/run-20260604-113246-tso-litmus-final-matrix-greencheck/`
and repeated post-fuzz matrix `reports/phase4-hollow-knight/run-20260604-114143-tso-litmus-post-fuzz-final-matrix/`.

- `mp`: PASS
- `spin`: PASS
- `cas`: PASS
- `xadd`: PASS
- `split`: PASS (`PASS split-lock xadd`)

Later fresh single-mode reruns with the newly rebuilt clang litmus binary hit pre-`ProcessInit`
bootstrap exits/timeouts; those are harness/bootstrap misses, not semantic FAIL verdicts.

## HK heartbeat verdict

Valid heartbeat run: `reports/phase4-hollow-knight/run-20260604-heartbeat-sampler-game60/`.

- Final heartbeat: `blocks=0x1a876a`, `steps=0xa6a872`,
  `block_pc=0x87ef188589d`, `rva=0x51589d`.
- Graphics counters: `D3D11CreateDevice=0`, `GfxDevice=0`, `CreateSwapChain=0`, `Present=0`.
- The `0x51589d` heartbeat is immediately followed by Mono
  `System.RuntimeType has invalid vtable method slot 16` assertions.

Runtime module base is `0x87ef1370000`; `0x87ef188589d - 0x87ef1370000 = 0x51589d`.
That base maps to:

`/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll`

The module is Mono, not `Hollow Knight.exe` and not `UnityPlayer.dll`.

## Disassembly

File VA: `0x18051589d` in `mono-2.0-bdwgc.dll`.

```asm
18051585d: movslq %edx, %r10
180515860: movq   %rcx, %rdi
180515866: movl   %r9d, %ebp
180515878: addq   %r8, %rbp        ; end = src + len
18051587b: movq   %r8, %rsi        ; src cursor
180515886: movq   0x28(%rax,%rdx,8), %r14
18051588d: movq   %rax, (%rdi)
180515890: movl   %eax, 0x8(%rdi)
180515893: cmpq   %rbp, %r8
180515896: jae    0x180515907
180515898: leaq   0x40(%rsp), %rbx ; stack output buffer
18051589d: cmpq   %rbp, %rsi       ; loop bound check
1805158a0: jae    0x1805158c6
1805158a2: movb   (%rsi), %al      ; load next source byte
1805158a4: incq   %rsi
1805158a7: cmpb   $0xa, %al        ; newline?
1805158a9: jne    0x1805158b4
1805158ab: incl   0x8(%rdi)
1805158ae: movb   $0xd, (%rbx)     ; insert CR before LF
1805158b1: incq   %rbx
1805158b4: movb   %al, (%rbx)
1805158b6: incq   %rbx
1805158b9: leaq   0x143f(%rsp), %rax
1805158c1: cmpq   %rax, %rbx       ; output buffer full?
1805158c4: jb     0x18051589d
```

## Live byte trace

Diagnostic build/run:
`reports/phase4-hollow-knight/run-20260604-mono515-byte-probe180b/`.

At entry to the loop:

- `r8=0x11d2619c0`: source buffer start
- `r9=0x1b6`: source length (`438` bytes)
- `rbp=0x11d261b76`: source end
- `rsi`: cursor, advanced by one byte on every loop iteration
- loaded bytes start:
  - `0x2a` (`*`)
  - `0x20` (space)
  - `0x41` (`A`)
  - `0x73` (`s`)
  - `0x73` (`s`)
  - `0x65` (`e`)
  - `0x72` (`r`)
  - `0x74` (`t`)
  - `0x69` (`i`)
  - `0x6f` (`o`)
  - `0x6e` (`n`)

That byte stream is the assertion text beginning `* Assertion ...`.

## Classification

`0x51589d` is not a cross-thread flag spin, not a QPC/RDTSC/timer deadline, and not a stable
poll address. It is a local Mono assertion/log formatting loop:

- each iteration compares cursor `rsi` against end `rbp`;
- each iteration loads the next byte from the assertion source buffer at `rsi`;
- the source pointer advances monotonically;
- the only producer of the bytes is Mono's assertion/error formatting path.

The gate is therefore earlier than `0x51589d`: Mono has already produced
`System.RuntimeType has invalid vtable method slot 16`. The `0x51589d` hot heartbeat is just the
formatter copying that assertion text to its output path.

The next real CPU-side target is the invalid-vtable producer path, not TSO/no-hoist.
