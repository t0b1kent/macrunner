# ARM64X native-twin DllMain-.data hazard audit (Step 4)

Date: 2026-06-15 (Lane D). Source: HK run `laneA-verify-attach-try1-224104` (loaddll + bootstrap
trace) + `needs_native_entry` allowlist (loader.c:5258-5260). Companion to the confirmed kernelbase
crash: [[project_hk_dxmt_887a0004_root_cause]].

## The hazard class
A native ARM64 twin (aarch64-windows, ARM64X) is loaded as the **EC/AMD64 view** (machine=8664). Its
writable `.data` is never made coherent for the native view (loader applies NO ARM64X DVRT/.data
view-relocations; `macrunner_hb_update_arm64x_native_dispatch_metadata` active def @loader.c:2693 = no-op).
So globals a DllMain initializes are NOT what native exported code reads. Two tiers:
- **Tier 1 — native DllMain IS called** (allowlist: kernelbase/user32/win32u): DllMain runs but its
  writes land in the wrong view → native reads see NULL. **kernelbase = CONFIRMED crash** (sort.ctype_idx
  @ base+0x183d28 NULL after DllMain ret=1; AV in GetStringTypeW+0x45894).
- **Tier 2 — native DllMain NOT called** (all other 34 twins): native `.data` is entirely
  uninitialized; only bites when native code calls that twin's native export which reads DllMain-init
  `.data`.

A proper view-coherence fix (native `.data` aliases/syncs the EC `.data` that the always-run EC-view
DllMain initializes) would close BOTH tiers at once.

## 37 native twins loaded this run
advapi32 bcrypt combase coml2 crypt32 cryptbase **d3d11** **dxgi** dnsapi dwmapi gdi32 hid imm32 iphlpapi
jsproxy kernel32 kernelbase msacm32 msvcrt nsi ole32 oleaut32 opengl32 rpcrt4 sechost setupapi shcore
shell32 shlwapi ucrtbase user32 version win32u **winemetal** winhttp winmm ws2_32

## Risk table  (twin | native DllMain called? | .data hazard | read-by / note)
| twin | native-entry? | DllMain-.data hazard | read-by / risk |
|---|---|---|---|
| **kernelbase** | YES (allowlist) | **HIGH** locale (sort.ctype_idx/keys/casemap/guids), startup_info, console | GetStringType*/LCMapString/CompareString — **CONFIRMED crash** |
| **ucrtbase**  | no | **HIGH** CRT locale/multibyte/TLS/atexit/_initterm (libucrtbase.a) | DXMT C++ CRT; the kernelbase crash is reached VIA CRT locale init |
| **msvcrt**    | no | **HIGH** msvcrt_tls_index, locale, stdio iob (locale.c/data.c/iob.c) | native CRT calls |
| **combase**   | no | MED-HIGH apartment globals, com tlsdata, RPC channel hooks, crit-sect | Co*/apartment if native COM (DXGI is COM) |
| user32   | YES (allowlist) | MED KernelCallbackTable→Peb (shared, safe) + atom/class `.data` | window class/atom; keystone made window path work |
| win32u   | YES (allowlist) | MED `__wine_syscall_dispatcher` (ALREADY synced by keystone) + gdi globals | NtGdi/NtUser; dispatcher handled, residual gdi `.data` |
| ole32    | no | MED GIT/clipboard/std-proxy/marshal | legacy COM marshal |
| ws2_32   | no | MED unixlib handle + per-thread data | **KNOWN past victim** (dispatch crash, fixed by dispatch-slot fill); winsock |
| rpcrt4   | no | MED RPC crit-sect/protseqs/thread data | native RPC |
| bcrypt   | no | MED `__wine_unixlib_handle` (NULL→crash) | native bcrypt |
| crypt32  | no | LOW-MED cert stores/OID tables/crit-sect/unixlib | native crypto (unlikely) |
| oleaut32 | no | LOW-MED BSTR cache/typelib | native OLE automation (unlikely) |
| winmm    | no | LOW-MED multimedia device `.data` | audio/joystick (separate path) |
| shlwapi  | no | LOW-MED ntdll fn-ptrs (vsnprintf) | native string funcs |
| shcore/imm32 | no | LOW TLS / IME `.data` | |
| kernel32 | no | LOW (forwards to kernelbase; minimal own `.data`) | |
| gdi32/advapi32/sechost/version/msacm32/dnsapi/winhttp/iphlpapi/nsi/hid/dwmapi/setupapi/shell32/opengl32/coml2/cryptbase/jsproxy | no | LOW (forwarders / trivial DllMain / not in native DXMT path) | |
| d3d11/dxgi/winemetal | (DXMT) | DXMT-owned; same ARM64X class if their `.data`-init diverges | separate from this wine-twin audit |

NB: native exports actually wired+called THIS run (sampled, capped at 32): ntdll(13), kernel32(7),
kernelbase(4). ntdll runs natively (it IS the loader) → not a twin hazard.

## Acid tests for the view-coherence fix (regression checklist)
After the fix, a native read of a DllMain-init global must be non-NULL/coherent for each Tier-1 + HIGH
Tier-2 twin:
1. **kernelbase**: `sort.ctype_idx` (base+0x183d28) != NULL before first native GetStringTypeW (primary).
2. **ucrtbase/msvcrt**: CRT locale tables + TLS index initialized for native CRT calls.
3. **combase**: apartment / com-tls globals initialized before native Co* calls.
4. user32 atom/class `.data`, win32u gdi `.data` coherent (beyond the already-synced callback table /
   syscall dispatcher).
