# LANE HK-REGISTRY — progress journal

19:51 · lane start · no HK run live · reading registry read path offline (advapi32/ntdll/server) before building probe
20:05 · probe built (x86_64 PE32+) · run REFUSED: mr-run.sh live (10800s slot) · falling back to offline: tracing seed-vs-wineserver-start order in mr-run.sh
20:36 · PROBE REPRODUCED DEFECT: open OK, GameLangSet W-query=2(not found), A-query=6 · control(runtime-created values)=PASS · crash at KEY_WOW64_32KEY section · extending probe with RegEnumValue bisector
20:45 · BISECTED: RegOpenKeyExW(spike kernelbase) returns synthetic handle 0x6f00f0000000(+1/open), all queries on it fail · A-open, NtOpenKeyEx, create paths all read GameLangSet=1 fine · hunting source of synthetic handle
20:49 · root cause NAMED + PROVEN: macrunner_hb_try_registry_semantic (macrunner_hb.c:30794, chain :32604) fakes all advapi32 W-registry imports · RegOpenKeyExW→fake handle 0x6f00f0000000, queries miss · real path reads GameLangSet=1 + M2H=EN (probe, 30s, spike dist) · WOW64/type/timing/path hypotheses disproven
20:56 · report + ready patch (git apply --check clean) + swarm inbox note written · fix = delete 49 lines in macrunner_hb.c = FORBIDDEN territory (engine lane live) · next: operator/engine lane applies patch, probe re-verifies
LOOP-STATUS: BLOCKED
03:50 · COORDINATOR AUTOMATION · registry-fix finisher COMPLETED unattended. Slot waited 0 min. dist ntdll.so 30ae36659ba83146 -> 0b4dd887bbd06323 (artifact verified changed by SHA, not by exit code). Probe rc=4; extracted: . Full probe output: /tmp/mr-agents/registry-probe-after-fix.txt; build log: /tmp/mr-agents/registry-fix-finish.log. If GameLangSet now reads through the W path, the language gate disappears entirely and the confirmedLanguage poll is never entered.
04:00 · COORDINATOR AUTOMATION · registry-fix finisher: STOPPED. dist ntdll.so SHA is unchanged (0b4dd887bbd06323) after a successful build, i.e. the install did not land — the exact stale-artifact shape the makedep.c bug produced. Not running the probe against a stale binary.
