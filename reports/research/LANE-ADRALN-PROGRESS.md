14:35 · старт ADRALN-лайна · grep нашёл codegen/signal точки · читаю hb_arm64_codegen.c TSO-эмиттеры
15:15 · TSO direct-mem load+store имеют alignment-check с helper-fallback · ищу другие LDAR/STLR без проверки
15:46 · счётчики: adraln=сырой si_code (ненадёжен), pages_distinct=43 насыщается → re-fault одних страниц, bus_handler НЕ эмулирует (setup_exception→SEH) · ищу адреса фолтов
16:26 · direct_mem128 load вокруг xmm без align-check; ldar_to_reg(buf,23,21,size) на cmp пути без check · смотрю конте�ст around 1655/2292
16:35 · emit_stlr_from_reg без align-check на строках 1446 (store_zero) и 1900 (push_x20); push/pop-путь НЕ имеет alignment-check · проверяю accerr-sample fault-адреса
16:46 · accerr=SEGV_ACCERR (permission) отдельный класс с fault=0x60/0x68/0x3004 (NULL+offset guard); bus=3.6M adraln-лейбл — отдельный класс · изучаю bus-адреса и handler cost
16:48 · LOAD path: xmm direct 128-bit (no align check), GPR via _tso (checked), else helper · STORE path: читаю case HB_IR_STORE
17:11 · timeline: Begin MonoManager +44s, Restored language +427s; bus-fault sampling выключено (bfc>=24) · фолты адресов нет для bus в поздней фазе; счётчики подтверждены
18:08 · direct_mem=0 → _tso и scan-loop выключены; активны: stack push STLR (ст.1900, unguarded), pop/ret LDAR (превращены в LDR), XMM (LDR без риска) · RSP выравнивание на +640 с неустойчиво → STLR push с SIGBUS · пишу отчёт
18:17 · ADRALN-ROOT-MAP.md написан · 4 раздела: механизм/43 страницы/цена handler/варианты с file:line · LOOP-STATUS: GOAL
18:26 · LANE-ADRALN start · target: reports/research/ADRALN-ROOT-MAP.md (4 sections) · step 1: locate LDAR/STLR emission sites
18:35 · ADRALN root-map exists but stale line-numbers (push STLR real line 1959 not 1900; LDAR pat\/pop\/ret offsets shifted) · all core claims verified against src+log · need: fix line refs + §1b table (only 3 emit_stlr call sites: 1469,1505,1959 — map lists 5) · next: patch ADRALN-ROOT-MAP.md with corrected lines
18:44 · ADRALN map verified+corrected · all 4 sections present, line refs updated to current src (push STLR=1959), §1b table fixed to 3 real emit sites, cross-verification appendix added · loop complete
LOOP-STATUS: GOAL
