# LANE HK-SHIMAUDIT — progress journal

03:54 · inventory complete: 39 shim handlers found in macrunner_hb.c (registry shim already gone), dispatch map next · spawning parallel audit chunks
LOOP-STATUS: GOAL
18:20 · full audit done: 39/39 handlers classified with quoted return paths, ranked by blast radius · report reports/phase4-hollow-knight/SHIM-FAKE-SUCCESS-AUDIT.md (+ raw evidence shim-audit-raw-swarm-output.txt)
18:20 · top fakes: vectored_exception (minted VEH handles, real faults never delivered), msvcrt_exit (onexit/atexit family reports success, nothing runs), MapViewOfFile write path (no writeback), environment+crt_locale (fake codepage/locale, non-ASCII→'?' with success), user32 pseudo-handles (0x00006f50… namespace), system_info hardcoded 1-CPU
18:20 · security_token flag DISPROVEN by evidence: real NtOpenProcessToken/NtOpenThreadToken handles (L30820-30841) — do not touch
