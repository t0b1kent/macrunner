## Block D verifiers
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --lint-localization
[0/1] Planning build
Building for debugging...
[0/4] Write swift-version--58304C5D6DBC2206.txt
[1/3] Linking MacRunnerControlCenter
[2/3] Applying MacRunnerControlCenter
Build of product 'MacRunnerControlCenter' complete! (2.17s)
{
  "locales" : [
    "en",
    "ru"
  ],
  "missing" : {

  },
  "status" : "PASS"
}
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --help-search "1С"
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.30s)
[

]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --help-list
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.22s)
[
  {
    "id" : "getting-started",
    "title" : "Getting Started"
  }
]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --bug-report-dry-run
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.24s)
{
  "sha256" : "003f21292979fdc4575f6a833304c3628b28930179c5438f82844fa3ef2b715c",
  "zip" : "\/var\/folders\/sp\/jbp4ynb53310q0w1db69lrlm0000gn\/T\/bug-report-2026-05-14T00-56-32Z.zip"
}
$ python3 - <<PY unzip generated zip
Archive:  /var/folders/sp/jbp4ynb53310q0w1db69lrlm0000gn/T/bug-report-2026-05-14T00-56-32Z.zip
  Length      Date    Time    Name
---------  ---------- -----   ----
        0  05-14-2026 10:56   macrunner-bug-report-2026-05-14T00-56-32Z/
      111  05-14-2026 10:56   macrunner-bug-report-2026-05-14T00-56-32Z/diagnostic.md
        0  05-14-2026 10:56   __MACOSX/
        0  05-14-2026 10:56   __MACOSX/macrunner-bug-report-2026-05-14T00-56-32Z/
      171  05-14-2026 10:56   __MACOSX/macrunner-bug-report-2026-05-14T00-56-32Z/._diagnostic.md
       41  05-14-2026 10:56   macrunner-bug-report-2026-05-14T00-56-32Z/sanitised-wine-log.txt
      171  05-14-2026 10:56   __MACOSX/macrunner-bug-report-2026-05-14T00-56-32Z/._sanitised-wine-log.txt
      195  05-14-2026 10:56   macrunner-bug-report-2026-05-14T00-56-32Z/manifest.json
---------                     -------
      689                     8 files
/var/folders/sp/jbp4ynb53310q0w1db69lrlm0000gn/T/bug-report-2026-05-14T00-56-32Z.zip

## Block D help rerun after source-tree fallback
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --help-search "1С"
Building for debugging...
[0/4] Write sources
[1/4] Write swift-version--58304C5D6DBC2206.txt
[3/6] Compiling MacRunnerControlCenter HelpCenterService.swift
[4/6] Emitting module MacRunnerControlCenter
[4/7] Write Objects.LinkFileList
[5/7] Linking MacRunnerControlCenter
[6/7] Applying MacRunnerControlCenter
Build of product 'MacRunnerControlCenter' complete! (4.28s)
[
  {
    "id" : "one-c-guide",
    "title" : "1С guide"
  }
]
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --help-list
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.22s)
[
  {
    "id" : "one-c-guide",
    "title" : "1С guide"
  },
  {
    "id" : "bottles",
    "title" : "Bottles"
  },
  {
    "id" : "getting-started",
    "title" : "Getting Started"
  },
  {
    "id" : "performance-hud",
    "title" : "Performance HUD"
  },
  {
    "id" : "profiles",
    "title" : "Profiles"
  },
  {
    "id" : "release-notes",
    "title" : "Release Notes"
  },
  {
    "id" : "store-integrations",
    "title" : "Store Integrations"
  },
  {
    "id" : "troubleshooting",
    "title" : "Troubleshooting"
  }
]
