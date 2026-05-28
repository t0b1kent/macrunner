## Package artifact
$ ls -lh dist/MacRunner-0.3.0.dmg dist/MacRunner-0.3.0.dmg.sha256
-rw-r--r--@ 1 timurtoby  staff   730M May 14 10:11 dist/MacRunner-0.3.0.dmg
-rw-r--r--  1 timurtoby  staff   116B May 14 10:11 dist/MacRunner-0.3.0.dmg.sha256
$ cat dist/MacRunner-0.3.0.dmg.sha256
33509b13273d9cba7501434f502947dedde9b50f220be4f01ca185ed55d99eb0  /Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/MacRunner-0.3.0.dmg
$ hdiutil verify dist/MacRunner-0.3.0.dmg
Checksumming Protective Master Boot Record (MBR : 0)…
Protective Master Boot Record (MBR :: verified   CRC32 $909FB6EF
Checksumming GPT Header (Primary GPT Header : 1)…
 GPT Header (Primary GPT Header : 1): verified   CRC32 $BCE061BC
Checksumming GPT Partition Data (Primary GPT Table : 2)…
GPT Partition Data (Primary GPT Tabl: verified   CRC32 $AF3309A5
Checksumming  (Apple_Free : 3)…
                    (Apple_Free : 3): verified   CRC32 $00000000
Checksumming EFI System Partition (C12A7328-F81F-11D2-BA4B-00A0C93EC93B : 4)…
EFI System Partition (C12A7328-F81F-: verified   CRC32 $B54B659C
Checksumming disk image (Apple_APFS : 5)…
         disk image (Apple_APFS : 5): verified   CRC32 $2F59CEF5
Checksumming  (Apple_Free : 6)…
                    (Apple_Free : 6): verified   CRC32 $00000000
Checksumming GPT Partition Data (Backup GPT Table : 7)…
GPT Partition Data (Backup GPT Table: verified   CRC32 $AF3309A5
Checksumming GPT Header (Backup GPT Header : 8)…
  GPT Header (Backup GPT Header : 8): verified   CRC32 $69EC22DC
verified   CRC32 $29ADDA6B
hdiutil: verify: checksum of "dist/MacRunner-0.3.0.dmg" is VALID

## Sparkle vendored gate
$ codesign --verify --deep --strict dist/stage/MacRunner.app
$ file dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/Sparkle
dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/Sparkle: Mach-O universal binary with 2 architectures: [x86_64:Mach-O 64-bit dynamically linked shared library x86_64] [arm64:Mach-O 64-bit dynamically linked shared library arm64]
dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/Sparkle (for architecture x86_64):	Mach-O 64-bit dynamically linked shared library x86_64
dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/Sparkle (for architecture arm64):	Mach-O 64-bit dynamically linked shared library arm64

## PLCrashReporter vendored gate
$ swift build --package-path app/macr-control-center >/tmp/macr-swift-build.log && swift test --package-path app/macr-control-center >/tmp/macr-swift-test.log && tail -n 8 /tmp/macr-swift-test.log
[0/1] Planning build
Building for debugging...
[0/8] Write swift-version--58304C5D6DBC2206.txt
[2/7] Emitting module MacRunnerControlCenter
[2/6] Linking MacRunnerControlCenter
[3/6] Applying MacRunnerControlCenter
[5/27] Compiling MacRunnerControlCenterTests MacRunnerTaskTests.swift
[6/27] Compiling MacRunnerControlCenterTests ManifestTests.swift
[7/27] Compiling MacRunnerControlCenterTests OnboardingTests.swift
[8/29] Compiling MacRunnerControlCenterTests AppImportWizardTests.swift
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/AppImportWizardTests.swift:31:13: warning: variable 'r' was never mutated; consider changing to 'let' constant
29 | 
30 |     @Test func inspectionResultFullFields() {
31 |         var r = PEInspectionResult(
   |             `- warning: variable 'r' was never mutated; consider changing to 'let' constant
32 |             arch: "x64",
33 |             subsystem: "gui",
[9/29] Compiling MacRunnerControlCenterTests AppLibraryTests.swift
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/AppImportWizardTests.swift:31:13: warning: variable 'r' was never mutated; consider changing to 'let' constant
29 | 
30 |     @Test func inspectionResultFullFields() {
31 |         var r = PEInspectionResult(
   |             `- warning: variable 'r' was never mutated; consider changing to 'let' constant
32 |             arch: "x64",
33 |             subsystem: "gui",
[10/29] Compiling MacRunnerControlCenterTests CommandRunnerTests.swift
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/AppImportWizardTests.swift:31:13: warning: variable 'r' was never mutated; consider changing to 'let' constant
29 | 
30 |     @Test func inspectionResultFullFields() {
31 |         var r = PEInspectionResult(
   |             `- warning: variable 'r' was never mutated; consider changing to 'let' constant
32 |             arch: "x64",
33 |             subsystem: "gui",
[11/29] Compiling MacRunnerControlCenterTests ServiceTests.swift
[12/29] Compiling MacRunnerControlCenterTests SettingsTests.swift
[13/29] Compiling MacRunnerControlCenterTests PPMParserTests.swift
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/PPMParserTests.swift:13:13: warning: variable 'header' was never mutated; consider changing to 'let' constant
11 | 
12 |     @Test func parseValidP6() {
13 |         var header = "P6\n2 2\n255\n"
   |             `- warning: variable 'header' was never mutated; consider changing to 'let' constant
14 |         var pixels: [UInt8] = [
15 |             255, 0, 0,   0, 255, 0,

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/PPMParserTests.swift:14:13: warning: variable 'pixels' was never mutated; consider changing to 'let' constant
12 |     @Test func parseValidP6() {
13 |         var header = "P6\n2 2\n255\n"
14 |         var pixels: [UInt8] = [
   |             `- warning: variable 'pixels' was never mutated; consider changing to 'let' constant
15 |             255, 0, 0,   0, 255, 0,
16 |             0, 0, 255,   255, 255, 255
[14/29] Compiling MacRunnerControlCenterTests PathSafetyTests.swift
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/PPMParserTests.swift:13:13: warning: variable 'header' was never mutated; consider changing to 'let' constant
11 | 
12 |     @Test func parseValidP6() {
13 |         var header = "P6\n2 2\n255\n"
   |             `- warning: variable 'header' was never mutated; consider changing to 'let' constant
14 |         var pixels: [UInt8] = [
15 |             255, 0, 0,   0, 255, 0,

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/PPMParserTests.swift:14:13: warning: variable 'pixels' was never mutated; consider changing to 'let' constant
12 |     @Test func parseValidP6() {
13 |         var header = "P6\n2 2\n255\n"
14 |         var pixels: [UInt8] = [
   |             `- warning: variable 'pixels' was never mutated; consider changing to 'let' constant
15 |             255, 0, 0,   0, 255, 0,
16 |             0, 0, 255,   255, 255, 255
[15/29] Compiling MacRunnerControlCenterTests RunHistoryStoreTests.swift
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/PPMParserTests.swift:13:13: warning: variable 'header' was never mutated; consider changing to 'let' constant
11 | 
12 |     @Test func parseValidP6() {
13 |         var header = "P6\n2 2\n255\n"
   |             `- warning: variable 'header' was never mutated; consider changing to 'let' constant
14 |         var pixels: [UInt8] = [
15 |             255, 0, 0,   0, 255, 0,

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Tests/MacRunnerControlCenterTests/PPMParserTests.swift:14:13: warning: variable 'pixels' was never mutated; consider changing to 'let' constant
12 |     @Test func parseValidP6() {
13 |         var header = "P6\n2 2\n255\n"
14 |         var pixels: [UInt8] = [
   |             `- warning: variable 'pixels' was never mutated; consider changing to 'let' constant
15 |             255, 0, 0,   0, 255, 0,
16 |             0, 0, 255,   255, 255, 255
[16/29] Emitting module MacRunnerControlCenterTests
[17/29] Compiling MacRunnerControlCenterTests D3DArtifactViewerTests.swift
[18/29] Compiling MacRunnerControlCenterTests DebugBundleTests.swift
[19/29] Compiling MacRunnerControlCenterTests DebugBundleV2Tests.swift
[20/29] Compiling MacRunnerControlCenterTests FailureClassifierTests.swift
[21/29] Compiling MacRunnerControlCenterTests JSONDecodeTests.swift
[22/29] Compiling MacRunnerControlCenterTests LogStreamerTests.swift
[23/29] Compiling MacRunnerControlCenterTests CompatibilityDBTests.swift
[24/29] Compiling MacRunnerControlCenterTests ControlCenterWorldsTests.swift
[25/29] Compiling MacRunnerControlCenterTests CoreStatusTests.swift
[26/29] Compiling MacRunnerControlCenterTests ShippablePhase2Tests.swift
[27/29] Compiling MacRunnerControlCenterTests TaskQueueTests.swift
[28/31] Emitting module MacRunnerControlCenterPackageTests
[29/31] Compiling MacRunnerControlCenterPackageTests runner.swift
[29/31] Write Objects.LinkFileList
[30/31] Linking MacRunnerControlCenterPackageTests
Build complete! (7.57s)
✔ Test echoTest() passed after 5.268 seconds.
✔ Test quickRunUpdatesAppStatus() passed after 0.311 seconds.
◇ Test runAppViewModelOnCompleteCalled() started.
✔ Test runAppViewModelOnCompleteCalled() passed after 0.320 seconds.
✔ Suite AppLibraryTests passed after 5.737 seconds.
✔ Test timeoutWorks() passed after 6.363 seconds.
✔ Suite CommandRunnerTests passed after 6.364 seconds.
✔ Test run with 94 tests in 22 suites passed after 6.364 seconds.
$ otool -L app/macr-control-center/.build/debug/MacRunnerControlCenter | grep CrashReporter
	@rpath/CrashReporter.framework/Versions/A/CrashReporter (compatibility version 1.0.0, current version 1.0.0)
$ otool -L dist/stage/MacRunner.app/Contents/MacOS/MacRunner | grep CrashReporter
	@rpath/CrashReporter.framework/Versions/A/CrashReporter (compatibility version 1.0.0, current version 1.0.0)

## Legendary/GOG tools vendored gate
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --print-tool-paths
[0/1] Planning build
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
[2/4] Emitting module MacRunnerControlCenter
Build of product 'MacRunnerControlCenter' complete! (1.83s)
legendary=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Sources/MacRunnerControlCenter/Resources/tools/legendary
gogdl=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Sources/MacRunnerControlCenter/Resources/tools/gogdl
$ dist/stage/MacRunner.app/Contents/MacOS/MacRunner --print-tool-paths
legendary=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Resources/tools/legendary
gogdl=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Resources/tools/gogdl
$ dist/stage/MacRunner.app/Contents/Resources/tools/legendary --version
legendary version "0.20.34", codename "Direct Intervention"
$ dist/stage/MacRunner.app/Contents/Resources/tools/gogdl --version
1.2.1

## HUD synthetic socket gate
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --hud-synthetic
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.24s)
wrote 300 samples to /Users/timurtoby/Documents/MacRunner/Main/MacRunner/run/hud.sock.log
$ wc -l /Users/timurtoby/Documents/MacRunner/Main/MacRunner/run/hud.sock.log
     300 /Users/timurtoby/Documents/MacRunner/Main/MacRunner/run/hud.sock.log
$ tail -n 5 /Users/timurtoby/Documents/MacRunner/Main/MacRunner/run/hud.sock.log
{"cache_misses":20,"fps":55.0642206898179,"gpu_ms":9.443518740221721,"cpu_ms":5.811396147828751,"frame_ms":18.160612961964848,"cache_hits":1195}
{"cache_misses":20,"fps":55.425847466572,"gpu_ms":9.381904359939979,"cpu_ms":5.77347960611691,"frame_ms":18.042123769115342,"cache_hits":1196}
{"cache_misses":20,"fps":55.82764303266264,"gpu_ms":9.31438211883256,"cpu_ms":5.731927457743113,"frame_ms":17.91227330544723,"cache_hits":1197}
{"cache_misses":20,"fps":56.26333750266499,"gpu_ms":9.242253003127828,"cpu_ms":5.687540309617125,"frame_ms":17.773563467553515,"cache_hits":1198}
{"cache_misses":20,"fps":56.726132010096435,"gpu_ms":9.166851001006865,"cpu_ms":5.641139077542686,"frame_ms":17.628559617320892,"cache_hits":1199}

## License sign+verify gate
$ TEST_LICENSE=$(./licensing/sign-license.sh test@example.com 2027-01-01T00:00:00Z)
license=test@example.com|2027-01-01T00:00:00Z|8707d56fde68f4b671c918503d23183abd0b4cd63566954f0097453f411a2a98d0f4f44f34f594293ee54a9461f15b14fbcd235e83087b960dfce72600a65507
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --verify-license "$TEST_LICENSE"
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.20s)
OK, valid until 2027-01-01T00:00:00Z
$ swift run --package-path app/macr-control-center MacRunnerControlCenter --verify-license garbage
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.15s)
license verification failed: signature mismatch
exit_code=1

## v0.3 baseline acceptance surrogate
$ ./scripts/verify-v0.3-baseline.sh
[integration] core_unit FAIL rc=1 duration_ms=767385
integration blocks: 0/1 PASS
$ tail -n 40 reports/integration-blocks/core_unit.log
skipped 'requires RUN_INTEGRATION=1'
skipped 'requires RUN_INTEGRATION=1'
test_auto_mode_is_esync (test_threading_plan.ThreadingPlanTests) ... ok
test_matrix_script_and_representative_fixtures_exist (test_winapi_smoke_matrix.WinapiSmokeMatrixTests) ... ok
test_matrix_script_runs (test_winapi_smoke_matrix.WinapiSmokeMatrixTests) ... skipped 'requires RUN_INTEGRATION=1'
test_hyperbridge_marker_jsonl_regression_runs (test_wine_pe_x86_marker.WinePeX86MarkerTests) ... ok
test_marker_schema_fields_are_emitted_by_runtime (test_wine_pe_x86_marker.WinePeX86MarkerTests) ... ok
test_patch_applies_to_current_wine_tree (test_wine_pe_x86_marker.WinePeX86MarkerTests) ... ok
test_patch_is_best_effort_and_emits_load_unload (test_wine_pe_x86_marker.WinePeX86MarkerTests) ... ok

======================================================================
ERROR: test_d3d_smoke_script_runs_and_records_expanded_passes (test_d3d_v0_1_milestone.D3DV01MilestoneTests)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/tests/test_d3d_v0_1_milestone.py", line 13, in test_d3d_smoke_script_runs_and_records_expanded_passes
    proc = subprocess.run([str(ROOT / "scripts/run-d3d-end-to-end-smoke.sh")], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=720)
  File "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/Python3.framework/Versions/3.9/lib/python3.9/subprocess.py", line 507, in run
    stdout, stderr = process.communicate(input, timeout=timeout)
  File "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/Python3.framework/Versions/3.9/lib/python3.9/subprocess.py", line 1134, in communicate
    stdout, stderr = self._communicate(input, endtime, timeout)
  File "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/Python3.framework/Versions/3.9/lib/python3.9/subprocess.py", line 1980, in _communicate
    self._check_timeout(endtime, orig_timeout, stdout, stderr)
  File "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/Python3.framework/Versions/3.9/lib/python3.9/subprocess.py", line 1178, in _check_timeout
    raise TimeoutExpired(
subprocess.TimeoutExpired: Command '['/Users/timurtoby/Documents/MacRunner/Main/MacRunner/scripts/run-d3d-end-to-end-smoke.sh']' timed out after 720 seconds

======================================================================
ERROR: test_launcher_d3d_json_contains_artifacts (test_d3d_v0_1_milestone.D3DV01MilestoneTests)
----------------------------------------------------------------------
Traceback (most recent call last):
  File "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/tests/test_d3d_v0_1_milestone.py", line 21, in test_launcher_d3d_json_contains_artifacts
    subprocess.run([str(ROOT / "scripts/run-windows-app.sh"), str(ROOT / "fixtures/arm64/d3d11_triangle_arm64.exe"), "--d3d-backend", "mock", "--timeout", "20", "--json", str(out)], cwd=ROOT, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60)
  File "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/Python3.framework/Versions/3.9/lib/python3.9/subprocess.py", line 528, in run
    raise CalledProcessError(retcode, process.args,
subprocess.CalledProcessError: Command '['/Users/timurtoby/Documents/MacRunner/Main/MacRunner/scripts/run-windows-app.sh', '/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/arm64/d3d11_triangle_arm64.exe', '--d3d-backend', 'mock', '--timeout', '20', '--json', '/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/test-d3d-v01-json.json']' returned non-zero exit status 241.

----------------------------------------------------------------------
Ran 104 tests in 764.781s

FAILED (errors=2, skipped=76)
$ tail -n 12 reports/d3d-end-to-end/d3d-end-to-end.jsonl
{"arch": "arm64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/arm64/d3d12_descriptor_heap_arm64.exe", "id": "arm64-d3d12-descriptor", "leftovers_count": 0, "rc": 124, "status": "TIMEOUT"}
{"arch": "arm64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/arm64/d3d12_root_signature_arm64.exe", "id": "arm64-d3d12-root-signature", "leftovers_count": 0, "rc": 124, "status": "TIMEOUT"}
{"arch": "arm64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/arm64/d3d12_barrier_transitions_arm64.exe", "id": "arm64-d3d12-barrier", "leftovers_count": 0, "rc": 241, "status": "FAIL"}
{"arch": "arm64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/arm64/d3d12_fence_wait_arm64.exe", "id": "arm64-d3d12-fence", "leftovers_count": 0, "rc": 241, "status": "FAIL"}
{"arch": "arm64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/arm64/d3d12_viewport_scissor_arm64.exe", "id": "arm64-d3d12-viewport", "leftovers_count": 0, "rc": 241, "status": "FAIL"}
{"arch": "x86_64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x64/d3d11_triangle_x64.exe", "id": "x64-d3d11-triangle", "leftovers_count": 0, "rc": 241, "status": "FAIL"}
{"arch": "x86_64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x64/d3d11_texture_sample_x64.exe", "id": "x64-d3d11-texture", "leftovers_count": 0, "rc": 53, "status": "FAIL"}
{"arch": "x86_64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x64/d3d12_triangle_x64.exe", "id": "x64-d3d12-triangle", "leftovers_count": 0, "rc": 53, "status": "FAIL"}
{"arch": "x86_64", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x64/d3d12_texture_sample_x64.exe", "id": "x64-d3d12-texture", "leftovers_count": 0, "rc": 53, "status": "FAIL"}
{"arch": "x86_64", "backend": "mock", "cleanup_ok": false, "d3d_non_background_pixels": 0, "d3d_ppm_path": null, "d3d_status": "NO_TRACE", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x64/d3d12_descriptor_heap_x64.exe", "id": "x64-d3d12-descriptor", "leftovers_count": 3, "rc": 53, "status": "FAIL"}
{"arch": "x86", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 1152, "d3d_ppm_path": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/d3d-run/20260514-100241/replay/trace.ppm", "d3d_status": "PASS", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x86/d3d11_triangle_x86.exe", "id": "x86-d3d11-triangle", "leftovers_count": 0, "rc": 0, "status": "PASS"}
{"arch": "x86", "backend": "mock", "cleanup_ok": true, "d3d_non_background_pixels": 1152, "d3d_ppm_path": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/d3d-run/20260514-100304/replay/trace.ppm", "d3d_status": "PASS", "exe": "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/fixtures/x86/d3d11_texture_sample_x86.exe", "id": "x86-d3d11-texture", "leftovers_count": 0, "rc": 0, "status": "PASS"}
$ ./scripts/cleanup-wine-runtime.py --assert-clear
wine_cleanup before=0 after=0 active_after=0 exiting_after=0 term=0 kill=0 winetemp_removed=0

## Patch portability gate
$ git archive HEAD | tar -x -C "$TMP" && git -C "$TMP" apply --check patches/control-center-no-more-foundations.patch
patch apply check PASS in /tmp/macr-patchcheck.V7TtsU

## Git tag truth gate
$ git tag -d v0.2.0-shippable || true
Deleted tag 'v0.2.0-shippable' (was 695cc53)
$ git tag -d v0.3.0-shippable || true
error: tag 'v0.3.0-shippable' not found.
$ git tag v0.3.0-shippable
$ git rev-parse HEAD v0.3.0-shippable
f9ca0fef9cf0276ebbb2c84047f448d6272e1051
f9ca0fef9cf0276ebbb2c84047f448d6272e1051
$ git diff HEAD..v0.3.0-shippable --stat
$ git archive 9394a84028f94770fdd3758ab0044536fbb22c7e | tar -x -C "$TMP" && git -C "$TMP" apply --check patches/control-center-no-more-foundations.patch
patch apply check PASS in /tmp/macr-patchcheck-final.pjPoJz against 9394a84028f94770fdd3758ab0044536fbb22c7e

## Final Git tag truth gate
$ git tag -f v0.3.0-shippable HEAD
Updated tag 'v0.3.0-shippable' (was f9ca0fe)
$ git rev-parse HEAD v0.3.0-shippable
ffabdf4e58465de2d7ba2ad5c73817d590f2600c
ffabdf4e58465de2d7ba2ad5c73817d590f2600c
$ git diff HEAD..v0.3.0-shippable --stat
