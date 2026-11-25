// ---------- a finished ISO ----------
// Said without taking any room: the page's own line carries the text, a green dot marks the page in the rail until
// it's opened, and the taskbar button flashes if the app isn't in front. No dialog, no focus change.
static int g_noteDot = -1;                       // the page the dot is on, or -1
static wchar_t g_dlReady[MAX_PATH * 2 + 64];     // Download Windows: the line under its title, until the next job

static void noteHide(void) { g_noteDot = -1; g_dlReady[0] = 0; InvalidateRect(g_hwnd, NULL, FALSE); }

static void noteShow(int page) {
    g_noteDot = (g_fadeAct == FADE_PAGE ? g_fadeArg : g_page) == page ? -1 : page;
    InvalidateRect(g_hwnd, NULL, FALSE);
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        FLASHWINFO f = { sizeof(f), g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&f);
    }
}
static COLORREF noteGreen(void) { return g_dark ? RGB(108, 203, 95) : RGB(15, 123, 15); }
