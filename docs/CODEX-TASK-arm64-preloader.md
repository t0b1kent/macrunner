# Task: Port wine_preloader to arm64 macOS

**Goal:** make Wine 11 secondary loader runnable on Apple Silicon macOS 26+,
unblocking `wine wineboot --init` and full Windows program execution.

**Time budget:** 2–6 weeks of focused C / Mach-O / Wine internals work.

---

## 1. Project context

`MacRunner` is an arm64-native CrossOver competitor. Wine 11 source from
CodeWeavers' LGPL release builds successfully (1011 aarch64 PE binaries,
1005 x86_64 PE binaries, all `.so` modules). Our 4 patches in
`wine-fork/patches/` are applied and clean. With SIP off + AMFI off the
top-level `bin/wine` runs and prints `wine-11.0`.

**The wall:** the secondary loader at
`engine/wine/dist/lib/wine/aarch64-unix/wine` either dies with SIGKILL
(when linked with `-pagezero_size,0x1000`) or exits with
`map_fixed_area out of memory for 0x7ffe0000` (when linked with default
PAGEZERO=0x100000000). Wine cannot map its required low-memory regions
(WINE_RESERVE 0x1000–0x200000000, shared_user_data 0x7ffe0000,
WINE_TOP_DOWN 0x7ff000000000) because:

* arm64 macOS forces PIE — `-no_pie` is **silently ignored**
  (`ld: warning: -no_pie ignored for arm64*`).
* `-image_base` is therefore ignored; binary always lands at random ASLR
  slide.
* With small `-pagezero_size,0x1000`, `__TEXT` collides with WINE_RESERVE
  zerofill section: `ld: custom segments overlap: __TEXT(0x1000-0x3000)
  WINE_RESERVE(0x1000-0x200000000)`.

The x86_64 zerofill+image_base trick (configure.ac line 956,
`USE_NO_HUGE`) is structurally impossible on arm64.

CrossOver, Apple GPTK, Whisky and Mythic all solve this with a separate
`wine-preloader` binary that reserves memory regions before exec'ing the
real loader. Wine HQ does not ship this for arm64. CodeWeavers'
preloader_mac.c only has `__i386__` and `__x86_64__` paths.

---

## 2. What needs to be built

### 2.1 Add aarch64 path to `engine/wine/loader/preloader_mac.c`

Currently the file gates everything on `#if defined(__APPLE__)` and
inside has only `__i386__` and `__x86_64__` reservations. Tasks:

* Add `#elif defined(__aarch64__)` arm. Reserve at minimum:
  - `0x7ffe0000`–`0x7ffe1000` (Windows shared user data, hard requirement
    for every PE binary).
  - `0x7f000000`–`0x80000000` (top-down virtual heap region used by
    ntdll/virtual.c).
  - Optionally a chunk at `0x10000000`–`0x40000000` for low-address PE
    image loads.
* Match the existing `wine_preload_info` schema:
  ```c
  static struct wine_preload_info preload_info[] = {
      { (void *)0x0000007ffe0000, 0x00010000 },
      { (void *)0x000000007f000000, 0x00fe0000 },
      { 0, 0 },
  };
  ```
* Replicate the dyld interpose / `__dyld_func_lookup` startup that the
  x86_64 path uses — without it the preloader does not get control
  before libSystem maps things.
* The preloader binary must end by exec'ing the real wine loader after
  marking reserved regions with `mmap(MAP_FIXED|MAP_NORESERVE|MAP_ANON,
  PROT_NONE)`.

### 2.2 Custom linker invocation for the preloader

Wine HQ's `loader/Makefile.in` builds `wine_preloader` with
`$(WINEPRELOADER_LDFLAGS)`, which `configure.ac` leaves empty for
`HOST_ARCH=aarch64`. Required flags (analogous to x86_64):

```
-Wl,-pagezero_size,0x1000          # leave low memory accessible
-Wl,-segalign,0x1000               # 4 KB segment alignment
-Wl,-sectcreate,__TEXT,__info_plist,loader/wine_info.plist
-static                            # no dyld dependence
-nostartfiles -nodefaultlibs       # custom _start
-Wl,-e,_start                      # entry point
```

Because arm64 forces PIE, classic `-image_base` does not work. Two
candidate strategies:

1. **PIE preloader + runtime mmap**: link normally PIE, place __TEXT
   wherever ASLR puts it. Use `mmap(MAP_FIXED)` from `_start` to claim
   low regions before any other code runs. Investigate whether macOS 26
   honors `MAP_FIXED` on completely unmapped low addresses for processes
   with `__PAGEZERO=0x1000`.
2. **PIE preloader with custom segment placement**: declare WINE_RESERVE
   as a `__zerofill` section and rely on `-segaddr,WINE_RESERVE,0x1000`.
   On arm64 segaddr inside a PIE binary may slide together with __TEXT;
   needs verification with `otool -l` after each build.

Strategy 1 is the cleanest. Strategy 2 is what x86_64 does pre-Xcode
15.3.

### 2.3 Configure.ac patch

In `engine/wine/configure.ac` around line 951:

```diff
   case $HOST_ARCH in
     i386) wine_use_preloader=yes ;;
     x86_64)
       WINE_TRY_LDFLAGS([-Wl,-no_huge], ...)
       ;;
+    aarch64) wine_use_preloader=yes ;;
     *)    wine_use_preloader=no ;;
   esac
```

Then around line 962, add an `aarch64` branch in the `WINEPRELOADER_LDFLAGS`
case.

After the patch, regenerate `configure` (`autoconf` in `engine/wine/`),
delete `engine/wine/build/Makefile`, re-run `./scripts/build-wine.sh`.
`HAVE_WINE_PRELOADER` should become defined and `wine-preloader` should
be installed under `engine/wine/dist/bin/`.

### 2.4 Sign with our cert + entitlements

`scripts/sign-engine.sh` already handles the bin tree. Ensure the new
`wine-preloader` binary is included.

---

## 3. Acceptance criteria

```bash
cd ~/Developer/MacRunner
./scripts/sign-engine.sh
./engine/wine/dist/bin/wine wineboot --init
ls "$WINEPREFIX/drive_c"        # must list windows/, users/, ...
./engine/wine/dist/bin/wine notepad.exe   # GUI window opens
```

Stretch:
```bash
./engine/wine/dist/bin/wine /path/to/1cv8.exe   # 1С launches
```

---

## 4. Existing assets the agent inherits

* `engine/wine/` — Wine 11.0 source from `crossover-sources-26.1.0.tar.gz`.
* `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/` —
  `aarch64-w64-mingw32-clang` for PE compilation.
* `wine-fork/patches/0001..0004.patch` — applied patches (CAMetalLayer,
  Vulkan SONAME, CLIENT_SURFACE_PRESENTED guard, toolchain).
* `wine-fork/wine.entitlements` — hardened-runtime entitlements.
* `scripts/build-wine.sh`, `scripts/sign-engine.sh` — full build pipeline.
* `engine/wine/build/Makefile` — last successful configure with
  `--enable-archs=aarch64,x86_64,i386`.

System: macOS 26.4 (Tahoe), Apple Silicon. SIP disabled, AMFI disabled
via boot-args (`amfi_get_out_of_my_way=0x1`). Free Apple Development
cert in keychain (Team `27GN9XE9CP`, Subject CN
`tobikent@mail.ru (86SLY5MXLN)`).

---

## 5. References for the agent

* `engine/wine/loader/preloader_mac.c` — current preloader implementation.
* `engine/wine/loader/main.c` lines 52–95 — host loader memory setup.
* `engine/wine/configure.ac` lines 910, 945–975 — preloader detection.
* `engine/wine/dlls/ntdll/unix/loader.c` line 2529 (`pre_exec`) — where
  ntdll relies on `wine_main_preload_info` symbol.
* Wine HQ ML thread "Wine on Apple Silicon" — community work in progress.
* CodeWeavers blog posts on arm64 — high-level only, no code.
* mstorsjo `llvm-mingw` docs — for cross-compile gotchas.

---

## 6. Out of scope for this task

* DXMT / DXVK integration.
* SwiftUI app.
* AI configurator.
* Distribution / notarization.

The single deliverable is: **`wine wineboot --init` produces a working
drive_c on Apple Silicon macOS 26.4 with the artifacts in this repo**.
