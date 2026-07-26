# HK Mono hash-chain run — log-gate invalid

Question: can the authorized direct `jit_code_hash` chain measurement start with
the required observable log contract?

Verdict: `INVALID_PRE_PRODUCT_ORCHESTRATION`.

The fresh owned-prefix launch sealed the expected child before Wine:

```text
final_env_count=116
final_winedll_entries=5
final_winedll_sha=cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432
```

It also deployed the expected x86_64 modules into its own prefix:

```text
system32/d3d11.dll     336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6
system32/winemetal.dll b4cf672eefd883e9e6d5fd09d8ef8c9e9d7c04ae7f20f25e1e6bc678b3135417
```

But neither mandatory runtime log was created:

```text
launch.stdout exists=false
launch.stderr exists=false
hash-chain.json exists=false
```

The launch was therefore scoped-stopped through its own `wineserver -k` before
the long-run gate and before any LLDB attach, chain read, or process-memory
write. The runner exited `0`; its prefix was removed and no HK/Wine/mr-run
process referencing the run remains.

No rendering, JIT, chain, or pixel conclusion is valid. A new owned run root
must redirect its foreground launcher stdout and stderr to the required
`launch.stdout`/`launch.stderr` pair from process start, then prove both grow
before the two-minute gate. This is an orchestration repair, not an experimental
retry or a product result.
