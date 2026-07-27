/* hk_registry_probe — minimal x86-64 probe for the Hollow Knight PlayerPrefs
 * registry read path.
 *
 * What it does, in order:
 *  1. CONTROL: creates HKCU\Software\MacRunnerProbe, writes a DWORD and a
 *     REG_BINARY("EN\0"), reads both back, prints PASS/FAIL. A probe that
 *     cannot read a value it planted itself proves nothing.
 *  2. Prints IsWow64Process() for this process (redirection hypothesis).
 *  3. Opens HKCU\Software\Team Cherry\Hollow Knight exactly like Unity
 *     PlayerPrefs does (RegOpenKeyExW, KEY_QUERY_VALUE) and queries
 *     GameLangSet_h1172976845 and M2H_lastLanguage_h3859156181, printing
 *     LSTATUS, type, size, and raw bytes for each.
 *  4. Variants: same open with KEY_WOW64_64KEY and with KEY_WOW64_32KEY, to
 *     expose redirection behavior if any.
 *  5. On failure, enumerates HKCU\Software subkeys so we can see what the
 *     running wineserver actually has loaded.
 *
 * Exit code: 0 if both HK values read OK, 2 if the control failed,
 * 3 if control passed but HK values failed.
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

/* direct-ntdll bisector: compare kernelbase-level handles with raw ntdll handles */
extern NTSTATUS WINAPI RtlOpenCurrentUser(ACCESS_MASK, PHANDLE);
extern NTSTATUS WINAPI NtOpenKeyEx(PHANDLE, ACCESS_MASK, const OBJECT_ATTRIBUTES *, ULONG);
extern NTSTATUS WINAPI NtQueryValueKey(HANDLE, const UNICODE_STRING *, int, void *, ULONG, ULONG *);
#define KeyValuePartialInformation 2
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x40
#endif

static void hex_handle(const char *label, HANDLE h)
{
    printf("%s = %p\n", label, h);
    fflush(stdout);
}

static void ntdll_direct_test(void)
{
    HANDLE hkcu = NULL, hsoft = NULL, htc = NULL;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING us;
    NTSTATUS st;
    WCHAR sw[] = L"Software";
    WCHAR tc[] = L"Software\\Team Cherry\\Hollow Knight";

    st = RtlOpenCurrentUser(MAXIMUM_ALLOWED, &hkcu);
    printf("NTDLL RtlOpenCurrentUser -> 0x%lx ", (unsigned long)st);
    hex_handle("NTDLL hkcu", hkcu);

    InitializeObjectAttributes(&attr, &us, OBJ_CASE_INSENSITIVE, hkcu, NULL);
    RtlInitUnicodeString(&us, sw);
    st = NtOpenKeyEx(&hsoft, KEY_READ, &attr, 0);
    printf("NTDLL NtOpenKeyEx(HKCU, Software) -> 0x%lx ", (unsigned long)st);
    hex_handle("NTDLL hsoft", hsoft);

    RtlInitUnicodeString(&us, tc);
    st = NtOpenKeyEx(&htc, KEY_READ, &attr, 0);
    printf("NTDLL NtOpenKeyEx(HKCU, Software\\Team Cherry\\Hollow Knight) -> 0x%lx ", (unsigned long)st);
    hex_handle("NTDLL htc", htc);

    if (!st && htc)
    {
        WCHAR vname[] = L"GameLangSet_h1172976845";
        UNICODE_STRING vn;
        struct { ULONG TitleIndex; ULONG Type; ULONG DataLength; UCHAR Data[16]; } info;
        ULONG reslen = 0;
        memset(&info, 0, sizeof(info));
        RtlInitUnicodeString(&vn, vname);
        st = NtQueryValueKey(htc, &vn, KeyValuePartialInformation, &info, sizeof(info), &reslen);
        printf("NTDLL NtQueryValueKey(GameLangSet_h1172976845) -> 0x%lx type=%lu datalen=%lu reslen=%lu",
               (unsigned long)st, (unsigned long)info.Type, (unsigned long)info.DataLength,
               (unsigned long)reslen);
        if (!st && info.DataLength >= 4)
            printf(" dword=%lu", (unsigned long)*(DWORD *)info.Data);
        printf("\n");
        fflush(stdout);
    }
    if (htc) NtClose(htc);
    if (hsoft) NtClose(hsoft);
    if (hkcu) NtClose(hkcu);
}

static void print_bytes(const char *label, const BYTE *data, DWORD len)
{
    DWORD i;
    printf("%s len=%lu hex:", label, (unsigned long)len);
    for (i = 0; i < len && i < 64; i++) printf(" %02x", data[i]);
    if (len > 64) printf(" ...");
    printf("  ascii: ");
    for (i = 0; i < len && i < 64; i++) printf("%c", (data[i] >= 32 && data[i] < 127) ? data[i] : '.');
    printf("\n");
    fflush(stdout);
}

static int control_test(void)
{
    HKEY k = NULL;
    LONG st;
    DWORD dword_in = 0x11223344, dword_out = 0, dword_out_sz = sizeof(dword_out), type = 0;
    static const BYTE bin_in[3] = { 'E', 'N', 0 };
    BYTE bin_out[16];
    DWORD bin_out_sz = sizeof(bin_out);
    int ok = 1;

    st = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MacRunnerProbe", 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &k, NULL);
    printf("CONTROL RegCreateKeyExA -> %ld (hkey=%p)\n", (long)st, k);
    if (st != ERROR_SUCCESS) return 0;

    st = RegSetValueExA(k, "ProbeDword", 0, REG_DWORD, (const BYTE *)&dword_in, sizeof(dword_in));
    printf("CONTROL RegSetValueExA dword -> %ld\n", (long)st);
    if (st != ERROR_SUCCESS) ok = 0;
    st = RegSetValueExA(k, "ProbeBin", 0, REG_BINARY, bin_in, sizeof(bin_in));
    printf("CONTROL RegSetValueExA binary -> %ld\n", (long)st);
    if (st != ERROR_SUCCESS) ok = 0;

    st = RegQueryValueExA(k, "ProbeDword", NULL, &type, (BYTE *)&dword_out, &dword_out_sz);
    printf("CONTROL RegQueryValueExA dword -> %ld type=%lu val=0x%08lx %s\n",
           (long)st, (unsigned long)type, (unsigned long)dword_out,
           (st == ERROR_SUCCESS && type == REG_DWORD && dword_out == dword_in) ? "MATCH" : "MISMATCH");
    if (st != ERROR_SUCCESS || dword_out != dword_in) ok = 0;

    st = RegQueryValueExA(k, "ProbeBin", NULL, &type, bin_out, &bin_out_sz);
    printf("CONTROL RegQueryValueExA binary -> %ld type=%lu\n", (long)st, (unsigned long)type);
    if (st == ERROR_SUCCESS) print_bytes("CONTROL bin", bin_out, bin_out_sz);
    if (st != ERROR_SUCCESS || bin_out_sz != sizeof(bin_in) || memcmp(bin_out, bin_in, sizeof(bin_in))) ok = 0;

    RegCloseKey(k);
    printf("CONTROL overall: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static LONG query_one(HKEY k, const WCHAR *name)
{
    BYTE buf[256];
    DWORD sz = sizeof(buf), type = 0;
    LONG st = RegQueryValueExW(k, name, NULL, &type, buf, &sz);
    char name_a[128];
    WideCharToMultiByte(CP_ACP, 0, name, -1, name_a, sizeof(name_a), NULL, NULL);
    printf("  RegQueryValueExW \"%s\" -> %ld type=%lu size=%lu\n",
           name_a, (long)st, (unsigned long)type, (unsigned long)sz);
    if (st == ERROR_SUCCESS)
    {
        print_bytes("  data", buf, sz);
        if (type == REG_DWORD && sz == 4)
            printf("  as dword: %lu\n", (unsigned long)*(DWORD *)buf);
    }

    /* UnityPlayer's PlayerPrefs queries values through the ANSI variant
     * (RegQueryValueExA, see UnityPlayer.dll IAT + call sites in the
     * PlayerPrefs region) — test that exact path too. */
    {
        BYTE buf2[256];
        DWORD sz2 = sizeof(buf2), type2 = 0;
        LONG st2 = RegQueryValueExA(k, name_a, NULL, &type2, buf2, &sz2);
        printf("  RegQueryValueExA \"%s\" -> %ld type=%lu size=%lu\n",
               name_a, (long)st2, (unsigned long)type2, (unsigned long)sz2);
        if (st2 == ERROR_SUCCESS)
        {
            print_bytes("  dataA", buf2, sz2);
            if (type2 == REG_DWORD && sz2 == 4)
                printf("  as dword: %lu\n", (unsigned long)*(DWORD *)buf2);
        }
        /* HasKey-style probe: data=NULL count=NULL (existence check only) */
        {
            LONG st3 = RegQueryValueExA(k, name_a, NULL, NULL, NULL, NULL);
            printf("  RegQueryValueExA existence(data=NULL) -> %ld\n", (long)st3);
        }
    }
    fflush(stdout);
    return st;
}

/* Bisector: list what the server actually holds in the opened key. */
static void enum_values(HKEY k, const char *tag)
{
    DWORD i;
    printf("ENUM-VALUES[%s]:\n", tag);
    for (i = 0; i < 100; i++)
    {
        char name[256];
        DWORD nlen = sizeof(name), type = 0, dsz = 0;
        LONG st = RegEnumValueA(k, i, name, &nlen, NULL, &type, NULL, &dsz);
        if (st != ERROR_SUCCESS) { printf("  enum stop at %lu (st=%ld)\n", (unsigned long)i, (long)st); break; }
        printf("  [%lu] \"%s\" type=%lu datasz=%lu\n", (unsigned long)i, name, (unsigned long)type, (unsigned long)dsz);
    }
    fflush(stdout);
}

/* Bisector: write a fresh value into the Team Cherry key through this handle,
 * read it back. Distinguishes "handle/key fine, loaded values missing" from
 * "handle/key broken". */
static void write_read_into_tc(HKEY k)
{
    DWORD v = 0xAABBCCDD, out = 0, osz = sizeof(out), type = 0;
    LONG st = RegSetValueExA(k, "ProbeTCDword", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    printf("TC-WRITE RegSetValueExA ProbeTCDword -> %ld\n", (long)st);
    st = RegQueryValueExA(k, "ProbeTCDword", NULL, &type, (BYTE *)&out, &osz);
    printf("TC-WRITE RegQueryValueExA ProbeTCDword -> %ld type=%lu val=0x%08lx %s\n",
           (long)st, (unsigned long)type, (unsigned long)out,
           (st == ERROR_SUCCESS && out == v) ? "MATCH" : "MISMATCH");
    RegDeleteValueA(k, "ProbeTCDword");
    fflush(stdout);
}

static void try_open(REGSAM access, const char *tag, LONG *open_st_out, int *read_ok)
{
    HKEY k = NULL;
    /* UnityPlayer.dll: RegOpenKeyExW(HKEY_CURRENT_USER(0x80000001),
     * "Software\<company>\<product>", 0, KEY_READ(0x20019), &hkey) — no
     * KEY_WOW64_* flags (verified by disassembly of the PlayerPrefs open
     * site at UnityPlayer.dll+0x7cb4d4). */
    LONG st = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Team Cherry\\Hollow Knight",
                            0, access, &k);
    printf("OPEN[%s] RegOpenKeyExW(HKCU, Software\\Team Cherry\\Hollow Knight, 0, 0x%lx) -> %ld\n",
           tag, (unsigned long)access, (long)st);
    *open_st_out = st;
    if (st == ERROR_SUCCESS)
    {
        LONG s1 = query_one(k, L"GameLangSet_h1172976845");
        LONG s2 = query_one(k, L"M2H_lastLanguage_h3859156181");
        if (s1 == ERROR_SUCCESS && s2 == ERROR_SUCCESS) *read_ok = 1;
        RegCloseKey(k);
    }
}

static void enumerate_software(void)
{
    HKEY k = NULL;
    LONG st = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_ENUMERATE_SUB_KEYS, &k);
    DWORD i;
    printf("ENUM HKCU\\Software open -> %ld\n", (long)st);
    if (st != ERROR_SUCCESS) return;
    for (i = 0; i < 200; i++)
    {
        WCHAR name[256];
        DWORD nlen = 256;
        st = RegEnumKeyExW(k, i, name, &nlen, NULL, NULL, NULL, NULL);
        if (st != ERROR_SUCCESS) { printf("ENUM stop at %lu (st=%ld)\n", (unsigned long)i, (long)st); break; }
        printf("ENUM [%lu] %ls\n", (unsigned long)i, name);
    }
    RegCloseKey(k);
}

int main(void)
{
    BOOL wow64 = FALSE;
    HKEY k = NULL, kw = NULL;
    LONG open_st, s1, s2;
    int read_ok = 0;

    printf("=== hk_registry_probe ===\n");
    fflush(stdout);

    IsWow64Process(GetCurrentProcess(), &wow64);
    printf("IsWow64Process -> %d\n", (int)wow64);
    printf("sizeof(void*) = %u\n", (unsigned)sizeof(void *));
    fflush(stdout);

    if (!control_test())
    {
        printf("RESULT: CONTROL FAILED — probe cannot be trusted\n");
        fflush(stdout);
        return 2;
    }

    /* Game-exact open: RegOpenKeyExW(HKCU, path, 0, KEY_READ) — UnityPlayer
     * call site uses sam=0x20019 (KEY_READ), no KEY_WOW64_* flags. */
    open_st = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Team Cherry\\Hollow Knight",
                            0, KEY_READ, &k);
    printf("OPEN RegOpenKeyExW(HKCU, Software\\Team Cherry\\Hollow Knight, 0, KEY_READ) -> %ld (hkey=%p)\n",
           (long)open_st, k);
    fflush(stdout);

    /* ANSI open for comparison (the control's create path was ANSI and worked) */
    {
        HKEY ka = NULL;
        LONG sta = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Team Cherry\\Hollow Knight",
                                 0, KEY_READ, &ka);
        printf("OPEN RegOpenKeyExA(HKCU, same path, 0, KEY_READ) -> %ld (hkey=%p)\n", (long)sta, ka);
        fflush(stdout);
        if (sta == ERROR_SUCCESS)
        {
            DWORD out = 0, osz = sizeof(out), type = 0;
            LONG sq = RegQueryValueExA(ka, "GameLangSet_h1172976845", NULL, &type, (BYTE *)&out, &osz);
            printf("  via-A-open RegQueryValueExA GameLangSet -> %ld type=%lu val=%lu %s\n",
                   (long)sq, (unsigned long)type, (unsigned long)out,
                   (sq == ERROR_SUCCESS && out == 1) ? "MATCH" : "MISMATCH");
            {
                BYTE bbuf[64]; DWORD bsz = sizeof(bbuf), btype = 0;
                LONG sq2 = RegQueryValueExA(ka, "M2H_lastLanguage_h3859156181", NULL, &btype, bbuf, &bsz);
                printf("  via-A-open RegQueryValueExA M2H_lastLanguage -> %ld type=%lu\n", (long)sq2, (unsigned long)btype);
                if (sq2 == ERROR_SUCCESS) print_bytes("  via-A-open M2H data", bbuf, bsz);
                printf("  via-A-open M2H %s\n", (sq2 == ERROR_SUCCESS && bsz >= 2 && bbuf[0] == 'E' && bbuf[1] == 'N') ? "MATCH(EN)" : "MISMATCH");
            }
            fflush(stdout);
            RegCloseKey(ka);
        }
    }

    ntdll_direct_test();
    if (open_st == ERROR_SUCCESS)
    {
        s1 = query_one(k, L"GameLangSet_h1172976845");
        s2 = query_one(k, L"M2H_lastLanguage_h3859156181");
        if (s1 == ERROR_SUCCESS && s2 == ERROR_SUCCESS) read_ok = 1;
        enum_values(k, "Team Cherry key");
        RegCloseKey(k);
    }
    else
    {
        enumerate_software();
    }

    /* Write/read probe through a writable handle on the same key */
    {
        LONG st = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Team Cherry\\Hollow Knight",
                                0, KEY_ALL_ACCESS, &kw);
        printf("OPEN RegOpenKeyExW(..., KEY_ALL_ACCESS) -> %ld (hkey=%p)\n", (long)st, kw);
        fflush(stdout);
        if (st == ERROR_SUCCESS)
        {
            write_read_into_tc(kw);
            RegCloseKey(kw);
        }
    }

    printf("RESULT: %s\n", read_ok ? "HK VALUES READ OK" : "HK VALUES NOT READ");
    fflush(stdout);
    return read_ok ? 0 : 3;
}
