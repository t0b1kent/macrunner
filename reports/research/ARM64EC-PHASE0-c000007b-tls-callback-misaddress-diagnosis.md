# Phase 0 diagnosis — c000007b on x64-signal-callback @ 0x14000150f (TLS-callback mis-address)

**Date:** 2026-05-30
**For:** Codex MEGA-PROGRAM Phase 0 (close out the residual fault before Phase 1).
**Source:** `reports/arm64ec-finish-run-final-20260530-104000.log` + `llvm-objdump` of
`tests/native-fixtures/build/hello_x64.exe`. Analysis operator-side via context-mode.

## The fault (from the log)
```
macrunner-hb-runtime-fail: label=x64-signal-callback block_pc=0x14000150f next_pc=0x14000150f
  ret=OK out=MEMORY_FAULT reason=memory or internal fault blocks=1 steps=1
  rax=0x0 rcx=0x140000000 rdx=0x1 rsi=0x0 rdi=0x0 rsp=0x107b9efb8 r9=0x1203
macrunner-hb-run-exit: label=x64-signal-callback status=c000007b reason=runtime pc=0x14000150f
```
Non-fatal today (main thread enters BeginSimulation at the real entry 0x140013D0 and the process
exits 0), but it WILL bite under real games (heavy TLS/exception use). Close it now.

## Root cause: the callback target address is wrong (lands mid-NOP-padding)
`hello_x64.exe` ImageBase=0x140000000, AddressOfEntryPoint=0x13D0, and it HAS a `.tls` directory
(Entry 9 Thread Storage Directory, `.tls` @ RVA 0x6000) with an AddressOfCallBacks array.

Disasm around the fault RVA:
```
140001500: 31 c0 ; xorl %eax,%eax
140001502: c3    ; retq                         <- tiny stub returns 0
140001503: 66 66 66 66 2e 0f 1f 84 00 00 00 00 00  ; nopw %cs:(%rax,%rax)  (13-byte NOP, 0x1503..0x150f)
140001510: 83 fa 03 ; cmpl $0x3,%edx            ; if reason==DLL_THREAD_DETACH ...
140001513: 0f 84 ...; je 0x140002110
140001519: 85 d2    ; testl %edx,%edx           ; if reason==DLL_PROCESS_DETACH ...
14000151b: 0f 84 ...; je 0x140002110
140001521: c3       ; retq
...
140001530: 48 83 ec 28 ; subq $0x28,%rsp        ; walks list @0x140004008, calls *0x140003578
```
**`0x14000150f` is the LAST byte of the 13-byte NOP padding (0x1503–0x150f)** — NOT an instruction
boundary and NOT a function entry. Decoding from there = garbage → MEMORY_FAULT → c000007b.

The args make the intent clear: `rcx=ImageBase`, `rdx=1` = the Win64 TLS-callback signature
`VOID cb(PVOID DllHandle=ImageBase, DWORD Reason=DLL_PROCESS_ATTACH(1), PVOID Reserved)`.
So this is the **TLS-callback dispatch**, and it is entering the callback at the WRONG address
(0x150f, mid-padding) instead of a real callback function pointer read from the TLS directory's
AddressOfCallBacks (the real callback bodies are the functions at 0x1500/0x1510/0x1530).

## Fix direction (Codex)
The `x64-signal-callback` / TLS-callback dispatch path (the `macrunner_hb` callback dispatch in
`dlls/ntdll/unix/macrunner_hb.c` / `loader.c`, the area just edited) is computing/passing the
callback target wrong — off into NOP padding. It must:
1. Read the real callback function pointers from the image's TLS directory `AddressOfCallBacks`
   array (each is an absolute VA = ImageBase + RVA), and
2. Enter each at its true start (a valid instruction boundary), with `(ImageBase, reason, NULL)`.
Compare with how the 32-bit path dispatches TLS/init callbacks (working reference). Likely an
off-by / wrong-pointer-source in reading the callback entry (the target ended 0x150f instead of a
real function like 0x1500/0x1530).

## Gate for Phase 0 done
`hello_x64` + `stdout_stderr_x64` run with ZERO `runtime-fail`/`MEMORY_FAULT`/`c000007b` lines,
exit 0. Paste the clean log.
