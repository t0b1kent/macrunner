## Block C verifiers
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --bottle-dump manual-test-pe-8c2a2f73
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.31s)
{
  "bottle_id" : "manual-test-pe-8c2a2f73",
  "dll_overrides" : {
    "d3d11" : "native,builtin"
  },
  "env_vars" : {
    "WINEDEBUG" : "-all"
  },
  "graphics_backend" : "auto",
  "registry_tweaks" : {
    "HKCU\\Software\\Wine\\MacRunner" : "enabled"
  },
  "wine_version" : "bundled"
}
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --program-overrides manual-test-pe-8c2a2f73
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.17s)
[
  {
    "base_profile" : "game-generic-dx11",
    "executable_name" : "fixture.exe",
    "overrides" : {
      "WINEDEBUG" : "-all",
      "graphics_backend" : "dxvk"
    },
    "pe_hash" : "fb3765beb98bdf345d118b8a1ed5591a3ba45105d1287f74048a0686d72ce943"
  }
]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --activity-export json > /tmp/activity.json && cat /tmp/activity.json
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.17s)
{
  "cache" : {
    "hit_rate" : 0,
    "total_size_bytes" : 0
  },
  "database_path" : "\/Users\/timurtoby\/Library\/Application Support\/MacRunner\/activity.sqlite",
  "installed_count" : 2,
  "last_launched" : "2026-05-14T00:55:37Z",
  "programs" : [
    {
      "id" : "D7BB66A2-4446-46CB-84E9-BF0B6F272406",
      "launchCount" : 1,
      "name" : "QuickRunTest"
    },
    {
      "id" : "ED485670-CEE4-445B-928C-2A29C37DB5D0",
      "launchCount" : 1,
      "name" : "QuickRunTest"
    }
  ]
}
$ python3 - <<PY activity checks
2
0
/Users/timurtoby/Library/Application Support/MacRunner/activity.sqlite
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --cache-stats
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.24s)
{
  "by_program" : [
    {
      "block_count" : 1,
      "program_id" : "fixture",
      "size_bytes" : 19
    }
  ],
  "lru_evicted_count" : 0,
  "total_size_bytes" : 19
}
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --cache-export fixture /tmp/test.mrcache
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.17s)
wrote /tmp/test.mrcache
$ file /tmp/test.mrcache && head -3 /tmp/test.mrcache
/tmp/test.mrcache: ASCII text
MRCACHE1
signature=66a481e46c47bec198e604c6f31f3efe2413e81c628796de038a0e65fabf77f7
{
