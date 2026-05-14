## Block A verifiers
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --dump-steam-library
[0/1] Planning build
Building for debugging...
[0/4] Write swift-version--58304C5D6DBC2206.txt
[1/3] Linking MacRunnerControlCenter
[2/3] Applying MacRunnerControlCenter
Build of product 'MacRunnerControlCenter' complete! (2.61s)
[
  {
    "appid" : "620",
    "cover_path" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers\/steam\/620.png",
    "install_dir" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Tests\/Fixtures\/steam-account\/steamapps\/common\/Portal 2",
    "installed" : true,
    "last_played" : "2024-03-09T16:00:00Z",
    "name" : "Portal 2 Fixture"
  }
]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --epic-status
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.17s)
{
  "logged_in" : false,
  "provider" : "epic",
  "tool_path" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Sources\/MacRunnerControlCenter\/Resources\/tools\/legendary"
}
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --dump-epic-library
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.32s)
[

]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --gog-status
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.16s)
{
  "logged_in" : false,
  "provider" : "gog",
  "tool_path" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Sources\/MacRunnerControlCenter\/Resources\/tools\/gogdl"
}
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --dump-gog-library
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.16s)
[

]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --dump-bnet-library
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.16s)
[

]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --import-exe app/macr-control-center/Tests/Fixtures/test-pe.exe
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.18s)
{
  "arch" : "x86_64",
  "bottle_id" : "manual-test-pe-8c2a2f73",
  "executable_path" : "app\/macr-control-center\/Tests\/Fixtures\/test-pe.exe",
  "pe_hash" : "8c2a2f7328e85fc0868e174dd80057bbb9e136b5229d8d6f3ed316c67f37cd3b",
  "profile_id_or_null" : "generic-x86_64-rosetta",
  "suggested_name" : "test-pe"
}
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --cover-cache-report
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.16s)
{
  "entries" : [
    {
      "id" : "620",
      "path" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers\/steam\/620.png",
      "provider" : "steam",
      "sizeBytes" : 138051,
      "updatedAt" : "2026-05-14T00:54:09Z"
    }
  ],
  "limitBytes" : 524288000,
  "root" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers",
  "totalSizeBytes" : 138051
}
$ du -sh ~/Library/Caches/MacRunner/covers/
136K	/Users/timurtoby/Library/Caches/MacRunner/covers/
