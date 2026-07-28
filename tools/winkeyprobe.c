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

static LONG g_keydown, g_keyup, g_char, g_mousemove, g_lbutton, g_setfocus, g_killfocus,
            g_activate, g_activateapp, g_ncactivate, g_paint;
static DWORD g_deadline_ms;

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
    fprintf(stderr, "winkeyprobe: %s keydown=%ld keyup=%ld char=%ld mousemove=%ld lbutton=%ld "
            "setfocus=%ld killfocus=%ld activate=%ld activateapp=%ld ncactivate=%ld paint=%ld "
            "visible=%d foreground=%d(self=%d) focus=%d active=%d\n",
            tag, g_keydown, g_keyup, g_char, g_mousemove, g_lbutton, g_setfocus, g_killfocus,
            g_activate, g_activateapp, g_ncactivate, g_paint,
            (int)IsWindowVisible(hwnd),
            (int)(GetForegroundWindow() != NULL), (int)(GetForegroundWindow() == hwnd),
            (int)(GetFocus() == hwnd), (int)(GetActiveWindow() == hwnd));
    fflush(stderr);
}

int wmain(int argc, WCHAR **argv)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    HWND hwnd;
    MSG msg;
    DWORD secs = 25, last_report = 0;

    if (argc > 1) secs = (DWORD)_wtoi(argv[1]);

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

    /* WS_OVERLAPPEDWINDOW | WS_VISIBLE — deliberately NOT WS_POPUP.  The
     * on-demand realization path (window.c:826) derives its activate flag from
     * WS_POPUP alone, so an ordinary overlapped window is the case that gate
     * gets wrong. */
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"MacRunner winkeyprobe",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           120, 120, 520, 360, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
        fprintf(stderr, "winkeyprobe: CreateWindowExW FAILED err=%lu\n", GetLastError());
        return 2;
    }

    fprintf(stderr, "winkeyprobe: hwnd=%p style=0x%lx visible=%d — window created, entering loop for %lus\n",
            hwnd, GetWindowLongW(hwnd, GWL_STYLE), (int)IsWindowVisible(hwnd), secs);
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
        Sleep(10);
    }
out:
    report("FINAL", hwnd);
    return 0;
}
