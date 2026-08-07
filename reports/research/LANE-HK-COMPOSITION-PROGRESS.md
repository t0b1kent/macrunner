# LANE HK-COMPOSITION — progress journal

22:10 · locate swaptrace data · no file matches prompt's 21204-record table; real dataset = sidechannel-grid (totals drawindexed=41807/present=857) + ring-ledger v2/v4; swaptrace cap 4096, drawtrace cap 64 found in source (d3d11_context_impl.cpp) · build analyzer
22:25 · analyzer tools/hk_composition/analyze_swaptrace.py on 3 runs · identity chain CLOSED ×3 (run A: bb 0xee23331d0→RTV 0xee2333330/0xee2333460); census identical ×3: 937 backbuffer binds / 3158 offscreen / 1 true unbind / 0 nullbinds — prompt's arg1=0x0×3296 NOT reproduced (aggregation artifact) · per-frame analysis
22:40 · level-start lands after present#48; OMSet coverage ends frame 258 (budget) → 209 post-level frames fully covered: EVERY one binds backbuffer RTV exactly 4×, last-bound-before-Present1 = BACKBUFFER 210/210 · draws: 128/128 sampled (pre-level, cap) → backbuffer RTV; clears mostly offscreen → asymmetry INVERTED vs hypothesis
22:58 · verdict written reports/phase4-hollow-knight/HK-COMPOSITION-GRAPH-OFFLINE-VERDICT-20260726.md · composition candidate REFUTED structurally; residual = post-level per-draw sampling (item-5 spec: boundrtv field in dxmt_hk_draw_trace_call + MACRUNNER_DXMT_DRAW_TRACE_MAX raise, handed to run lane) · done
LOOP-STATUS: GOAL
