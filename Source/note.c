// ---------- in-app notification ----------
// A bar under the pages, for a job that ends long after it was started. It never takes the
// focus or disables the window, and stays until it's closed or the next job with an output box starts. If the app
// isn't in front, its taskbar button flashes instead.
static BOOL g_noteOn = FALSE;
static wchar_t g_noteTitle[96], g_noteText[MAX_PATH * 3 + 200];
static RECT g_rcNote, g_rcNoteX;

static void noteHide(void) {
    if (!g_noteOn) return;
    g_noteOn = FALSE;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void noteShow(const wchar_t* title, const wchar_t* text) {
    lstrcpynW(g_noteTitle, title, 96);
    lstrcpynW(g_noteText, text, (int)(sizeof(g_noteText) / sizeof(g_noteText[0])));
    g_noteOn = TRUE;
    InvalidateRect(g_hwnd, NULL, FALSE);
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        FLASHWINFO f = { sizeof(f), g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&f);
    }
}

// How much of the window's height the bar takes this frame, its gap included: 0 once it has faded out
static int noteReserve(int L, int R, double* v) {
    *v = anim(ANIM_NOTE, g_noteOn);
    // these two pages fill a small window to its bottom edge, so the bar would cover their last controls
    BOOL noRoom = (g_page == PAGE_CLEANUP || g_page == PAGE_TWEAKS) && g_bh < dp(770);
    if (*v <= 0 || noRoom) { SetRectEmpty(&g_rcNote); SetRectEmpty(&g_rcNoteX); return 0; }
    int h = dp(12) + dp(22) + measureWrapped(g_fSmall, g_noteText, R - L - dp(52) - dp(52)) + dp(12);
    if (h > dp(130)) h = dp(130);
    return h + dp(12);
}

static void noteDraw(int L, int R, int reserve, double v) {
    if (reserve <= 0) return;
    int B = g_bh - dp(16), T = B - (reserve - dp(12));
    g_rcNote = (RECT){ L, T, R, B };
    g_alphaMul = v;
    Col s = sepCol();
    fillRoundRect(L, T, R, B, dp(8), g_dark ? 50 : 251, g_dark ? 50 : 251, g_dark ? 50 : 251, 1.0);
    strokeRoundRect(L, T, R, B, dp(8), dp(1), s.r, s.g, s.b, s.a * (g_dark ? 1.0 : 1.6));
    COLORREF green = g_dark ? RGB(108, 203, 95) : RGB(15, 123, 15);
    drawGlyph(g_fIconLg, 0xE73E, L + dp(27), T + dp(24), green);
    RECT t1 = { L + dp(52), T + dp(12), R - dp(52), T + dp(34) };
    drawText(g_fBodyBold, g_noteTitle, t1, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT t2 = { L + dp(52), T + dp(34), R - dp(52), B - dp(10) };
    drawText(g_fSmall, g_noteText, t2, mutedRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    g_rcNoteX = (RECT){ R - dp(40), T + dp(8), R - dp(8), T + dp(40) };
    fillHover(g_rcNoteX.left, g_rcNoteX.top, g_rcNoteX.right, g_rcNoteX.bottom, dp(4), 2 * anim(ANIM_NOTEX, g_hotBtn == BTN_NOTEX));
    drawGlyph(g_fIcon, 0xE711, (g_rcNoteX.left + g_rcNoteX.right) / 2, (g_rcNoteX.top + g_rcNoteX.bottom) / 2, textRGB());
    g_alphaMul = 1.0;
}
