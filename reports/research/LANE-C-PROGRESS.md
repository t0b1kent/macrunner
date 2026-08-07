# LANE C — PROGRESS / HEARTBEAT (autoloop resume anchor)

**Mission:** `reports/research/LANE-C-LOADER-MISSION-pe32-machine-routing.md`
**Evidence:** `reports/research/PE32-NEEDS.md` (PE32's exact diagnosis — do not re-diagnose).
**Owned (edit ONLY):** `engine/wine/server/mapping.c`, `engine/wine/dlls/ntdll/unix/env.c`,
`engine/wine/dlls/ntdll/unix/loader.c`. NEVER macrunner_hb.c/signal_arm64.c (Lane A) / xtajit (PE32)
/ dxmt-graphics (Lane D).

**Merge policy:** agent does NOT commit. Coordinator commits these 3 disjoint files to branch
`lane-c/pe32-machine-routing` and merges to main AFTER the x64 regression smoke passes (clean by
construction — no other lane touches these files).

**Goal/gate:** i386 main image becomes process machine on ARM64 → `notepad++.exe machine=014c` →
wow64/xtajit load → `BTCpuProcessInit`/`BTCpuThreadInit`; x64 HK path unregressed.

Each turn: append ONE heartbeat line `TIME · action · result · next`, and end the turn with a status
line starting at column 0 (format in the prompt; GOAL only if gate met AND x64 unregressed).

---
2026-06-07 · activate · coordinator handed the free PE32-Codex the Lane C loader/server fix (its own
diagnosis from PE32-NEEDS.md). Main tree, gpt-5.5, no-commit. next: read mission + PE32-NEEDS, locate
the I386 reject in server/mapping.c, mirror the x86_64-on-ARM64 guest path.
23:24 · BURST C start · read-only cache/SMC inspection; no HK/Wine/process kill · gather code/log refs
23:25 · code search narrowed · found symbols tb_invalidate/block_cache_find/macrunner_hb_trace_smc; excluding noisy reports · inspect source refs
23:26 · runtime refs found · block cache pc-keyed/reset; persistent cache hashes guest block bytes/version/flags; checking env modes/invalidation · inspect aot/memory
23:27 · memory refs found · MRU invalidates on generation; reads/writes copy live bytes only; exec writes force special_write or bump generation · inspect wine special_io
23:29 · trusted logs scanned · no cache trace signatures by default; latest fault at UnityPlayer rva 0x2b54b2, SMC_TRACE_INSUFFICIENT · finalizing read-only recommendation
22:32 · AUDIT-PART1 graphics dispatch · начат поиск D3D COM/Vtable/Present-slot/bridge путей · продолжение PART1
22:34 · PART1 graphics audit next-target data collection · inspected macrunner_hb dxmt com/vtable slot logic and dxgi/d3d11 vtbl maps; next: synthesize PART1 matrix
07:45 · старт · начал сбор требований по iter-3 wrongview и проверку ветки/состояния рабочих файлов · далее патч + сборка
10:17 · phase4 offline opcode sweep prep · confirmed hk_codegen_pinpoint source constraints and enumerated game PE candidates · next: extract .text offsets and build scan harness
10:17 · module inventory complete · selected x64 .text targets for full-sweep (13 modules including UnityPlayer + mono/runtime/native plugins) · next: extract section bytes and run hk_codegen with filtered parse
10:32 · opcode sweep finalize · filtered real C-пайплайн failures from UnityPlayer + 12 PE, no Wine\n10:32 · report drafting · deduped opcode map, families + static marker check\n
10:35 · report finalized · wrote HK-COLD-TRANSLATION-OPCODE-SWEEP-RESULT.md with deduped unsupported opcode map and marker note\n
10:26 · HK-CODEGEN-V2 · подготовил новый harness сканирования с resync и skip pad-ранов, next: запустить полный проход по 13 x64 PE
10:39 · started · план: перезапуск HK cold offline скана с продолжением после отказа, фильтром паддинга и покрытием по всем модулям · next: locate hk_codegen_pinpoint и подготовить параметры
10:39 · discovered · найдено прошлое место запуска и список модулей из предыдущего отчета · next: найти существующие бинарники и скрипт запуска для правильного повторного скана
10:47 · located · в трассах обнаружены /tmp/hk_opcode_sweep_v2_scan.py и /tmp/hk_scan_one_debug.py из предыдущего прогона · next: проверить существование скриптов и переиспользовать/воспроизвести логику скана
10:50 · launch · запуск /tmp/hk_opcode_sweep_v2_scan.py через nice -n 10 для полного прохода по 13 PE · next: дождаться завершения и сверить выходные JSON/MD
10:55 · running · /tmp/hk_opcode_sweep_v2_scan.py активен ~4 мин, CPU ~3010:55 · running · /tmp/hk_opcode_sweep_v2_scan.py активен ~4 мин, CPU 30, прогресс ещё без вывода (ожидание завершения полного прохода) · next: дождаться финального отчёта и свести кандидатов
11:07 · Начало диагностики MOVNTDQ · подготовка офлайн-репродукции для hb_arm64_codegen_block_with_cfg · next: найти источник UNSUPPORTED_OPCODE
11:12 · Выделил целевой артефакт и проверю офлайн прогон офлайн-харнессов на  с тем же входом +  · next: подтвердить конкретный IR-op, который возвращает UNSUPPORTED
11:12 · Выделил целевой артефакт и проверю офлайн-харнесс на hb_arm64_codegen_block_with_cfg с тем же входом и тем же окном блока · next: подтвердить конкретный IR-op, который возвращает UNSUPPORTED
11:12 · relaunch · создан /tmp/hk_opcode_sweep_v2_scan_v2.py с whitelist из 13 модулей и chunk=65536; перезапуск по требованиям покрытия/benign
11:40 · пересборка скрипт-сканера завершена прерыванием на Galaxy64 в субпроцесе · нужен перезапуск с обработкой ошибок и прямым перебором целевых модулей · продолжить повторный полный проход ·
11:41 · обновлён сканер v2: прямой список целевых модулей + ловля OSError на spawn hk_codegen_pinpoint · подготовка к полному проходу без зависаний и с сохранением покрытия · запускать по всем целевым модулям ·
11:56 · phase4 audit gather · прочитаны текущие отчёты и hk_getenv_source_test; уточняю точные артефакты и file:line для отчёта
15:54 · HK verification table draft… part1-part3 synthesis prep, source excerpts for hb_runtime/hb_arm64_codegen and ntdll logs; next: write requested final report markdown
16:02 · HK report created: HK-VERIFICATION-EPISTEMICS-AND-OPCODE-TAG-SPEC.md… includes stage audit (5 checks), unsupported discriminator spec, and pre-run stale-proof gate
12:19 · HK artifact gate · implemented UNPROVEN/FAIL(order) behavioral-state handling in tools/artifact_identity_gate.py and updated HK-ARTIFACT-IDENTITY-GATE-RESULT.md · next: add deployed runtime probe binary path

$(date +12:55 · HK ledger synthesis · свёл подтверждённые факты по `UnityPlayer.dll+0xe10256` в `HK-INVESTIGATION-LEDGER-0xe10256.md` без сборок/правок/Wine запусков · следующий: держать реестр в статике и ждать следующего дифф-дизайна валидации
12:58 · hk ledger update · added 3 refutations + cold-sweep invalidity reason in HK-INVESTIGATION-LEDGER-0xe10256.md (read-only edits only) · next: no further edits, keep to evidence synthesis
13:25 · полный проход завершён (HK run5-cold) · 13 модулей, 53,963,857/59,877,376 байт (без паддинга) и full_scan=True, таблицы в HK-COLD-TRANSLATION-OPCODE-SWEEP-V2-RESULT.md · формировать финальный вывод по user ·
14:58 · GLib build-config trace · проверил tracked/игнорированные источники, 2.88.1 только в generated-артефактах и отчетах, next: добавить форс-переconfigure по pkg-config в scripts/build-wine-arm64ec-spike.sh · next fix
14:59 · HK GLib guard patch · scripts/build-wine-arm64ec-spike.sh patched: при старом Makefile принудительный reconfigure при расхождении pkg-config glib-2.0 include; next: создать отчёт HK-GLIB...md · next: validate diff
14:59 · HK GLib report · создал отчёт HK-GLIB-VERSION-TRACKED-FIX-RESULT.md с выводом по источнику и применённой правкой · next: подача статуса пользователю
15:04:36 · HK 0x49ef3b pinpoin t · извлечены и сверены runtime-fail данные из coherent run, поиск исторических run3/4/5, проверка flight/run-exit, формирование промежуточных гипотез · создать итоговый report.md
15:08 · HK-GLIB commit complete · committed ee9b83dc via ff-only merge hk-glib-version-agnostic-configure -> main · next verify status/HASH + report updated
15:08:57 · HK 0x49ef3b pinpoint · сверены RVA/file-offset и исторические прогоны, подготовка/создание отчёта HK-PINPOINT-0x49ef3b-MEMORY-FAULT.md · next: finalize hypotheses and status
15:09:05 · HK 0x49ef3b report saved · создан файл reports/phase4-hollow-knight/HK-PINPOINT-0x49ef3b-MEMORY-FAULT.md · waiting next request
16:45 · HK-DECISIVE-SCENE-CONFIG prepared · собран готовый HK-DECISIVE-SCENE-RUN-CONFIG.md (preflight SHA, profile-145129 env snapshot, ready command, phase0 checklist) · next: execute preflight checks, then run 4200s decisive scene bootstrap
21:11 · HK combined-PE config · собрал готовый reports/phase4-hollow-knight/HK-COMBINED-PE-RUN-CONFIG.md (profile-145129 + MACRUNNER_HB_BACKEND=jit/x64-loader=1, exact observer+ntdll SHA, run-contract READY gate, cold/preimage правила, watch-order и вердикты) · next: выполнить preflight и подготовить прогон 4200s
03:19 · HK run+log-read · ищу blend/d3d11/seq события в 230048 launch.stderr · следующая проверка IID и create-path
03:24 · HK blendstate lifecycle evidence · собрал офлайн-доказательства для 230048: IID, disasm OMSetBlendState/CreateBlendState, проверил отсутствие ранних Create/OMSet в launch.log · следующий шаг: формализовать гипотезу UAF в следующих трассах
03:54 · HK blendstate source triage · started context-mode evidence harvest for 230048 and UnityPlayer RVA slices · next: capture log matches + disasm evidence
2026-07-25 04:15:44 · HK blendstate source refinement · собрал launch.230048 (CreateSwapChain/MakeWindowAssociation only, runtime-fail на OMSetBlendState; дальше пишу HK-UNITY-BLENDSTATE-SOURCE-ANALYSIS.md
2026-07-25 04:15:57 · HK blendstate source analysis draft ready · записал reports/phase4-hollow-knight/HK-UNITY-BLENDSTATE-SOURCE-ANALYSIS.md (no fix, only offline evidence)
2026-07-25 04:26:59 · HK starvation discriminator config · собрал offline-конфиг HK-STARVATION-DISCRIMINATOR-CONFIG.md с одним delta и чек-листом Phase 0.
2026-07-25 04:27:23 · HK starvation discriminator config ready · created reports/phase4-hollow-knight/HK-STARVATION-DISCRIMINATOR-CONFIG.md with delta env+metrics table and phase-0 checklist.
