# LANE HK-SHADER — progress journal

20:37 · recon prior reports · FS-output/VS/inputs already disproved for 5 captured shaders; plan=offline airconv CLI + full-population coverage · find/build airconv CLI
21:08 · magenta control PASS · hand-built DXBC mov o0,l(1,0,1,1) -> AIR returns const <1,0,1,1> rt0; instrument honest (my length-field bug caused earlier hang, not airconv) · full corpus translate
20:45 · airconv CLI verified · binary d2f0f1e7 built 18:02 from clean tree, runs; source Jul 22 < binary · known-good control next
20:52 · hang root cause · MY patcher wrote SM5 instr length at bits16-23 (0x00080036) not 24-30 (0x08000036); DXBCParser len=0 -> infinite loop; airconv innocent · fix + rerun
20:55 · magenta control PASS · const_magenta.dxbc a55c8f1b -> ret <1,0,1,1> rt0 metadata · corpus population next
21:05 · population found · 152 unique DXBC (128 resources.assets + 24 unity builtins): 82 PS / 46 VS / 24 CS · translate all
21:20 · corpus translated · 152/152 ok, 0 fail, 0 hang on current binary · semantic scan
21:30 · semantic scan clean · all 82 PS rt0 metadata; discard parity 28/28 exact; zero undef/null texture args; 2/82 const-black BY DESIGN (mov o0,l(0,0,0,0)); runtime-capture parity modulo unorm epilogue · verdict
21:35 · report written · SHADER-LANE-AIRCONV-OFFLINE-VERDICT-20260726.md · translation REFUTED as black-frame cause
21:17 · independent re-verify (fresh thread) · all SHAs match (airconv d2f0f1e7, results 6868f2d5, analysis 97e3c5d4, control a55c8f1b/d9b7e7a3); 152 ok/0 fail (summary line, not hidden fail); control .ll emits const <1,0,1,1>→rt0; 82/82 PS rt0_meta+n_ret; discard parity 28/28, 0 mismatches vs DXBC scan; 2 by-design black PS confirmed at DXBC level; 0 PS with o0_writes==0 · verdict stands, no new work needed
LOOP-STATUS: GOAL
