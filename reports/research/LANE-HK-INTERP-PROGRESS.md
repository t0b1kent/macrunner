# LANE HK-INTERP — progress journal

22:50 · старт лайна · sample 8785 строк, 57 тредов, парсер self-time (leaf==thread totals, 0 потерь) · дальше: dispatch-код
23:10 · dispatch прочитан · JIT-диспатч активен (95.9% hot-треда под hb_jit_runtime_run, 0 codegen-фреймов); "интерпретатор" = helper-вызовы из JIT-кода (emit_interp_ir_helper ×14 + two_block_loop) · дальше: cost split
23:25 · cost split · hot-тред 27296671: signal-guard snapshot 16.2% (6KB memset+memcpy/блок, sizeof(ctx)=2616 замерено), interp-машинерия ~20%, scalar mem ~13%, vec mem ~10%, TLV 5.9%, flags 5.1%, generated code 2.7% · producer 27295721: mprotect-шторм 21.1% из macrunner_hb_sync_virtual_region
23:30 · Q3 закрыт · find_region_normalized уже O(log n) treap + hot-cache (фикс 2026-06-17, hb_memory.c:1589) — НЕ алгоритмическая проблема; 132 samples = producer-тред, downstream mprotect-шторма
23:40 · Q4 + отчёт · tiering есть (compile-on-first-touch + 7 pattern-фамилий промоции); не хватает heat-keyed re-promotion и direct-mem tier · отчёт reports/research/LANE-HK-INTERP-ANALYSIS.md · single biggest win: убрать per-block full-ctx snapshot в run_jit_block_with_signal_guard (hb_runtime.c:2623-2628) = 16.2% hot-треда
LOOP-STATUS: GOAL
