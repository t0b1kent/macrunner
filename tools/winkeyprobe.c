/* MacRunner 2026-07-28 (HK input lane) — the missing instrument.
 *
 * The winecfg probe cannot reproduce the HK defect: measured over three runs,
 * the only guest that reaches macdrv_init there is explorer.exe, and its four
 * shell windows carry styles 0x4c80000 / 0x8c000000 / 0x84000000 / 0x84000000 —
 * none has WS_VISIBLE, so window.c:2082 never calls show_window and no window is
 * ever ordered in.  `app_sendEvent_enter = 0` in that probe is therefore
 * trivially true (no window, no activation, no events) and says nothing about
 * Hollow Knight, which HAS a visible window and still gets nothing.
 *
 * This is the smallest program that exercises the real path: one WS_VISIBLE
 * top-level window, a message loop, and a count of the WM_* input messages that
 * actually arrive.  It drives
 *     macdrv_WindowPosChanged -> show_window -> macdrv_order_cocoa_window
 *       -> -[WineWindow orderBelow:orAbove:activate:]
 *         -> -[WineApplicationController transformProcessToForeground:]
 * which is the one path that lifts the process out of
 * NSApplicationActivationPolicyProhibited.  If input is broken, it is broken
 * here too — in ~20 seconds instead of 45 minutes.
 *
 * Build: engine/wine/dist-arm64ec-spike/bin/winegcc -m64 -o winkeyprobe.exe \
 *            tools/winkeyprobe.c -municode -lgdi32 -luser32
 *
 * It prints one line per second with the running counts, so a run is readable
 * without a trace channel, and it exits on its own after N seconds.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

static LONG g_keydown, g_keyup, g_char, g_mousemove, g_lbutton, g_setfocus, g_killfocus,
            g_activate, g_activateapp, g_ncactivate, g_paint;
static DWORD g_deadline_ms;

/* MacRunner 2026-07-29 (HK master lane iter 8) — the arm selector.
 *
 * The control arm below is proven to deliver keys to the guest (0 -> 5/5/5).  Hollow Knight's
 * window is NOT that shape: measured on two runs, its one visible top-level window is
 * style=0x94000000 (WS_POPUP|WS_VISIBLE|WS_CLIPSIBLINGS) with ex_style=0x0.  With ex_style==0 the
 * `excluded_by_cycle` term in get_cocoa_window_state (window.c:259-260) reduces EXACTLY to
 * "NtUserGetWindowRelative(hwnd, GW_OWNER) != NULL", and excluded_by_cycle drops
 * NSWindowCollectionBehaviorParticipatesInCycle (cocoa_window.m:1469), which makes
 * -isExcludedFromWindowsMenu YES (:2620), which makes -canBecomeKeyWindow return NO (:2556).
 * A window that can never become key cannot activate its app on a click, and an inactive app
 * receives no NSEvents from a HID tap — which is precisely the measured HK signature
 * (applicationDidBecomeActive=0, zero NSEvents, [NSApp run] healthy and idle in mach_msg).
 *
 * So the difference between the working probe and HK is one of: the window STYLE, or the presence
 * of an OWNER.  These arms separate those two in ~40 s each instead of a ~45 min title run.
 * Pre-registered predictions are in scripts/hk-winkeyprobe-style-ab.sh; do not read this file for
 * them, so that the expectation cannot be quietly edited after the numbers land.
 */
enum probe_arm { ARM_OVERLAPPED, ARM_HKSTYLE, ARM_HKSTYLE_OWNED };

static enum probe_arm g_arm = ARM_OVERLAPPED;
static const char *g_arm_name = "overlapped";

static void select_arm(void)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA("MACRUNNER_WINKEYPROBE_ARM", buf, sizeof(buf));

    if (!n || n >= sizeof(buf)) return;          /* unset -> the control arm, unchanged */
    if (!strcmp(buf, "hkstyle"))       { g_arm = ARM_HKSTYLE;       g_arm_name = "hkstyle"; }
    else if (!strcmp(buf, "hkstyle_owned")) { g_arm = ARM_HKSTYLE_OWNED; g_arm_name = "hkstyle_owned"; }
    else if (!strcmp(buf, "overlapped")) { /* explicit control */ }
    else fprintf(stderr, "winkeyprobe: WARNING unknown arm '%s' — falling back to overlapped\n", buf);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:    case WM_SYSKEYDOWN: g_keydown++;   break;
    case WM_KEYUP:      case WM_SYSKEYUP:   g_keyup++;     break;
    case WM_CHAR:                           g_char++;      break;
    case WM_MOUSEMOVE:                      g_mousemove++; break;
    case WM_LBUTTONDOWN:                    g_lbutton++;   break;
    case WM_SETFOCUS:                       g_setfocus++;  break;
    case WM_KILLFOCUS:                      g_killfocus++; break;
    case WM_ACTIVATE:                       g_activate++;  break;
    case WM_ACTIVATEAPP:                    g_activateapp++;  break;
    case WM_NCACTIVATE:                     g_ncactivate++;   break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        HBRUSH br = CreateSolidBrush(RGB(20, 90, 160));
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        g_paint++;
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void report(const char *tag, HWND hwnd)
{
    fprintf(stderr, "winkeyprobe: [%s] %s keydown=%ld keyup=%ld char=%ld mousemove=%ld lbutton=%ld "
            "setfocus=%ld killfocus=%ld activate=%ld activateapp=%ld ncactivate=%ld paint=%ld "
            "visible=%d foreground=%d(self=%d) focus=%d active=%d style=0x%lx ex_style=0x%lx "
            "owner=%p excluded_by_cycle=%d\n",
            g_arm_name, tag, g_keydown, g_keyup, g_char, g_mousemove, g_lbutton, g_setfocus, g_killfocus,
            g_activate, g_activateapp, g_ncactivate, g_paint,
            (int)IsWindowVisible(hwnd),
            (int)(GetForegroundWindow() != NULL), (int)(GetForegroundWindow() == hwnd),
            (int)(GetFocus() == hwnd), (int)(GetActiveWindow() == hwnd),
            GetWindowLongW(hwnd, GWL_STYLE), GetWindowLongW(hwnd, GWL_EXSTYLE),
            GetWindow(hwnd, GW_OWNER),
            /* the exact expression from window.c:259-260, evaluated guest-side so the probe
             * reports the same predicate the driver will compute for this window */
            (int)(!(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW) &&
                  (GetWindow(hwnd, GW_OWNER) != NULL ||
                   (GetWindowLongW(hwnd, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))));
    fflush(stderr);
}

int wmain(int argc, WCHAR **argv)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    HWND hwnd;
    MSG msg;
    DWORD secs = 25, last_report = 0;

    if (argc > 1) secs = (DWORD)_wtoi(argv[1]);

    select_arm();
    fprintf(stderr, "winkeyprobe: ARM=%s (MACRUNNER_WINKEYPROBE_ARM)\n", g_arm_name);
    fflush(stderr);

    /* Which driver is actually serving THIS process?  The winecfg/winkeyprobe
     * runs showed only two processes reaching macdrv_init while a WS_VISIBLE
     * top-level window existed and reported foreground/focus/active — so the
     * app process may be running on win32u's null driver, where every USER call
     * succeeds and no NSWindow is ever created.  win32u installs the loud
     * nodrv_CreateWindow only when the window station has WSF_VISIBLE
     * (driver.c:1034); without it the null-driver fallback is SILENT, which is
     * exactly the signature to rule in or out here. */
    {
        HMODULE macdrv = GetModuleHandleW(L"winemac.drv");
        HWINSTA winsta = GetProcessWindowStation();
        USEROBJECTFLAGS flags = { 0 };
        DWORD need = 0;
        WCHAR staname[128] = { 0 };
        BOOL gotflags = GetUserObjectInformationW(winsta, UOI_FLAGS, &flags, sizeof(flags), &need);

        GetUserObjectInformationW(winsta, UOI_NAME, staname, sizeof(staname), &need);
        fprintf(stderr, "winkeyprobe: winepid=%04lx winemac_drv=%p winsta=%p name=%ls "
                "gotflags=%d dwFlags=0x%lx WSF_VISIBLE=%d cxscreen=%d cyscreen=%d\n",
                GetCurrentProcessId(), macdrv, winsta, staname, (int)gotflags,
                flags.dwFlags, (int)((flags.dwFlags & WSF_VISIBLE) != 0),
                GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        fflush(stderr);
    }

    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"MacRunnerWinKeyProbe";
    RegisterClassExW(&wc);

    if (g_arm == ARM_OVERLAPPED)
    {
        /* WS_OVERLAPPEDWINDOW | WS_VISIBLE — deliberately NOT WS_POPUP.  The
         * on-demand realization path (window.c:826) derives its activate flag from
         * WS_POPUP alone, so an ordinary overlapped window is the case that gate
         * gets wrong.  This is the arm that measured 0 -> 5/5/5; it is left
         * byte-for-byte as it was so the new arms are comparable against it. */
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"MacRunner winkeyprobe",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               120, 120, 520, 360, NULL, NULL, wc.hInstance, NULL);
    }
    else
    {
        /* Hollow Knight's measured shape: style=0x94000000, ex_style=0x0, full screen at 0,0. */
        HWND owner = NULL;
        int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);

        if (g_arm == ARM_HKSTYLE_OWNED)
        {
            /* An OWNER, and nothing else, is what flips excluded_by_cycle when ex_style==0.
             * It must itself be a top-level window; it is deliberately left invisible so the
             * only difference from the hkstyle arm on screen is nil. */
            owner = CreateWindowExW(0, wc.lpszClassName, L"MacRunner winkeyprobe owner",
                                    WS_OVERLAPPED, 0, 0, 64, 64,
                                    NULL, NULL, wc.hInstance, NULL);
            if (!owner)
                fprintf(stderr, "winkeyprobe: WARNING owner CreateWindowExW FAILED err=%lu — "
                        "this arm degenerates into hkstyle\n", GetLastError());
        }

        hwnd = CreateWindowExW(0, wc.lpszClassName, L"MacRunner winkeyprobe",
                               WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, cx, cy, owner, NULL, wc.hInstance, NULL);
    }
    if (!hwnd)
    {
        fprintf(stderr, "winkeyprobe: CreateWindowExW FAILED arm=%s err=%lu\n",
                g_arm_name, GetLastError());
        return 2;
    }

    fprintf(stderr, "winkeyprobe: arm=%s hwnd=%p style=0x%lx ex_style=0x%lx owner=%p visible=%d — "
            "window created, entering loop for %lus\n",
            g_arm_name, hwnd, GetWindowLongW(hwnd, GWL_STYLE), GetWindowLongW(hwnd, GWL_EXSTYLE),
            GetWindow(hwnd, GW_OWNER), (int)IsWindowVisible(hwnd), secs);
    fflush(stderr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    /* Decide between the two remaining causes of "no macdrv_WindowPosChanging
     * in this process":
     *   (a) GA_PARENT == 0 — then win32u's nodrv_CreateWindow returns TRUE
     *       WITHOUT its ERR (driver.c: "if (!parent ...) return TRUE"), so a
     *       null-driver process creates this window silently, exactly as
     *       observed; and macdrv_create_win_data would have bailed at its own
     *       create_NULL_desktop branch anyway.
     *   (b) GA_PARENT == desktop — then the window is well-formed and the
     *       driver install itself was lost after macdrv's init_user_driver().
     * The measured hwnd ancestry separates them with no rebuild of wine. */
    fprintf(stderr, "winkeyprobe: post-show winemac_drv=%p ga_parent=%p ga_root=%p ga_rootowner=%p "
            "getparent=%p desktop=%p iswindow=%d parent_is_desktop=%d\n",
            GetModuleHandleW(L"winemac.drv"),
            GetAncestor(hwnd, GA_PARENT), GetAncestor(hwnd, GA_ROOT),
            GetAncestor(hwnd, GA_ROOTOWNER), GetParent(hwnd), GetDesktopWindow(),
            (int)IsWindow(hwnd),
            (int)(GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow()));
    fflush(stderr);
    report("after-show", hwnd);

    g_deadline_ms = GetTickCount() + secs * 1000;

    for (;;)
    {
        DWORD now = GetTickCount();

        if ((LONG)(now - g_deadline_ms) >= 0) break;
        if (now - last_report >= 1000)
        {
            last_report = now;
            report("tick", hwnd);
        }

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        /* MacRunner 2026-07-29 (HK master lane iter 8) — this MUST be a message-pump WAIT, not
         * Sleep().  macdrv's unix-side self-init is the only thing that installs the mac driver in
         * a process whose winemac.drv PE DllMain never ran, and it is driven from
         * nulldrv_ProcessEvents() — "called from every message-pump wait of every placeholder
         * process" (macdrv_main.c:649-652).  A PeekMessage + Sleep(10) loop never performs such a
         * wait, so self-init never fired and this probe ran its whole life on win32u's NULL driver:
         * measured 2026-07-29 21:09, macdrv_selfinit_entry=0, run_cocoa_app_entry=0,
         * macdrv_init_user_driver_set=0, macrunner-winshow=0, and ZERO wine-owned windows in the
         * on-screen CGWindow list (28 candidates, none wine) — while the guest cheerfully reported
         * visible=1 focus=1 active=1 foreground=1(self=1) paint=1, because on the null driver every
         * USER call succeeds and no NSWindow is ever created.  That is a SILENT invalid vehicle:
         * it produces confident guest-side numbers about a window macOS never saw. */
        MsgWaitForMultipleObjectsEx(0, NULL, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
out:
    report("FINAL", hwnd);
    return 0;
}
