// Immediate-mode buttons and text boxes used by the Reg Networks, Windows AIO and Windows Repair pages.

// ---------- widgets (immediate mode: registered as they're drawn, hit-tested from the last frame) ----------
enum { A_NONE = 0, A_TAB0, A_TABLAST = A_TAB0 + AT_COUNT - 1,
       A_W_SELECT, A_W_DELETE, A_W_UP, A_W_DOWN, A_W_RESET, A_W_LIST, A_W_HBAR, A_W_NAME, A_W_DESC,
       A_W_ASWIM, A_W_ASESD, A_W_ASFOLDER, A_W_ASISO, A_W_MOPT0, A_W_MOPTLAST = A_W_MOPT0 + MEDIA_MAX - 1,
       A_E_SRC, A_E_SRCB, A_E_ALL, A_E_NONE, A_E_ROW0, A_E_ROWLAST = A_E_ROW0 + ESD_ROWS - 1, A_E_DIR, A_E_DIRB, A_E_NOURS, A_E_NABB, A_E_CH0, A_E_CHLAST = A_E_CH0 + 5, A_E_GO, A_E_CANCEL,
       A_F_SRC, A_F_SRCB, A_F_OUT, A_F_OUTB, A_F_LABEL, A_F_BUILD,
       A_I_SRC, A_I_SRCB, A_I_DST, A_I_DSTB, A_I_GO,
       A_U_SRC, A_U_SRCB, A_U_REFRESH, A_U_START, A_U_ROW0, A_U_ROWLAST = A_U_ROW0 + 7,
       A_H_FILE, A_H_FILEB, A_H_GO, A_H_V0, A_H_V3 = A_H_V0 + 3, A_H_C0, A_H_C3 = A_H_C0 + 3,
       A_STOP, A_REG_BOX, A_REG_OPEN, A_TI_FILE, A_TI_BROWSE, A_TI_RUN,
       A_R_SFC, A_R_SCAN, A_R_RESTORE, A_R_WIM, A_R_RS, A_R_WUC, A_R_COPY, A_R_TOGGLE, A_T_TERMINAL, A_T_EXPLORE, A_T_GALLERY, A_T_VBS, A_T_RDP, A_T_MAPDELAY, A_T_WUDRV, A_T_THEME, A_T_REGION, A_T_ANNOY,
       A_D_X64, A_D_ARM64, A_D_BOX, A_D_FIND, A_D_ED0, A_D_EDLAST = A_D_ED0 + 11,
       A_D_V0, A_D_VLAST = A_D_V0 + 3, A_D_LANG, A_D_APPS, A_D_CLEAN, A_D_GO, A_D_CANCEL, A_D_WORK, A_D_WORKDEF,
       A_P_FIREFOX, A_P_IDM, A_P_POT, A_P_CRD, A_P_NANAZIP, A_P_TAILSCALE, A_P_UPD0, A_P_UPDLAST = A_P_UPD0 + 7, A_P_NVIDIA, A_P_AMD,
       A_O_CUR, A_O_BETA, A_O_DEV, A_O_LANG, A_O_DOWNLOAD, A_O_CANCEL, A_O_SRC, A_O_ICUR, A_O_IBETA, A_O_IDEV, A_O_IAPPS, A_O_INSTALL, A_O_SW0, A_O_SW1, A_O_SW2,
       A_X_WINAPP, A_X_GAMING, A_X_ALL, A_X_NONE, A_X_LIST, A_X_INSTALL, A_X_UNINSTALL,
       A_COUNT };
// each widget fades in its own ANIM_AIO and ANIM_AIOF slot, so more ids than slots would share fades with other things
_Static_assert(A_COUNT <= ANIM_AIOF - ANIM_AIO && A_COUNT <= ANIM_MOPT - ANIM_AIOF, "too many widget ids for their fade slots");

typedef struct { int id; RECT rc; wchar_t* buf; int cch; BOOL ro; } AioHit;
static AioHit g_aioHits[96];
static int g_aioHitCount = 0;

static int g_focus = 0, g_caret = 0, g_selA = 0, g_fieldStart = 0;
static wchar_t* g_focusBuf = NULL; static int g_focusCch = 0; static BOOL g_focusRo = FALSE;

static wchar_t g_eSrc[MAX_PATH], g_eDir[MAX_PATH];
static wchar_t g_fSrc[MAX_PATH], g_fOut[MAX_PATH], g_fLabel[33] = L"WINDOWS";
static wchar_t g_iSrc[MAX_PATH], g_iDst[MAX_PATH];
static wchar_t g_uSrc[MAX_PATH];
static wchar_t g_hFile[MAX_PATH];
static wchar_t g_regBox[1024];
static wchar_t g_regStatus[1200];

static void aioResetHits(void) { g_aioHitCount = 0; }
static void regGo(void);
static void actionEsdLoad(void);
#if WCU_DOWNLOAD
static void dlFind(void);
#endif
static void aioRegister(int id, RECT rc, wchar_t* buf, int cch, BOOL ro) {
    if (g_aioHitCount >= 96) return;
    AioHit* h = &g_aioHits[g_aioHitCount++];
    h->id = id; h->rc = rc; h->buf = buf; h->cch = cch; h->ro = ro;
}
static int aioHitTest(int x, int y) {
    POINT p = { x, y };
    for (int i = g_aioHitCount - 1; i >= 0; i--) if (PtInRect(&g_aioHits[i].rc, p)) return g_aioHits[i].id;
    return A_NONE;
}
static BOOL aioOverField(int x, int y) {
    POINT p = { x, y };
    for (int i = 0; i < g_aioHitCount; i++) if (g_aioHits[i].buf && PtInRect(&g_aioHits[i].rc, p)) return TRUE;
    return FALSE;
}

static BOOL g_aioSmall = FALSE;      // a page whose buttons all use the smaller font, so they look even (Install Apps)
static void aioButton(int id, int l, int t, int r, int b, const wchar_t* text, BOOL primary, BOOL en, BOOL shield) {
    RECT rc = { l, t, r, b };
    if (en) aioRegister(id, rc, NULL, 0, FALSE);
    drawButtonFace(rc, dp(4), primary, anim(ANIM_AIO + id, g_hotBtn == BTN_WIDGET + id && en), en ? 1.0 : 0.4);
    COLORREF tc = primary ? RGB(255,255,255) : (en ? textRGB() : mutedRGB());
    // a label that wouldn't leave some room at both ends (such as one with the UAC shield) uses the smaller font
    int need = textWidth(g_fBody, text) + (shield ? dp(22) : 0) + dp(20);
    drawButtonLabel(rc, g_aioSmall || need > r - l ? g_fSmall : g_fBody, text, tc, shield, dp(16), dp(6));
}
static void aioField(int id, RECT rc, wchar_t* buf, int cch, BOOL ro, const wchar_t* hint) {
    aioRegister(id, rc, buf, cch, ro);
    BOOL focus = (g_focus == id);
    double v = anim(ANIM_AIO + id, focus || g_hotBtn == BTN_WIDGET + id);
    Col f = g_dark ? (Col){255,255,255,0.06} : (Col){255,255,255,0.70};
    fillRoundRect(rc.left, rc.top, rc.right, rc.bottom, dp(4), f.r, f.g, f.b, f.a + 0.03 * v);
    Col s = sepCol();
    strokeRoundRect(rc.left, rc.top, rc.right, rc.bottom, dp(4), dp(1), s.r, s.g, s.b, s.a * 0.8);
    double fv = anim(ANIM_AIOF + id, focus);
    if (fv > 0) fillAccentRect(rc.left + dp(2), rc.bottom - dp(2), rc.right - dp(2), rc.bottom, fv);
    RECT in = { rc.left + dp(10), rc.top, rc.right - dp(10), rc.bottom };
    int inner = in.right - in.left;
    if (!buf[0] && !focus) {
        if (hint) drawText(g_fSmall, hint, in, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    int start = 0;
    if (focus) {                      // keep the caret in view
        if (g_fieldStart > g_caret) g_fieldStart = g_caret;
        while (textWidthN(g_fSmall, buf + g_fieldStart, g_caret - g_fieldStart) > inner - dp(2)) g_fieldStart++;
        start = g_fieldStart;
    }
    if (focus && g_selA != g_caret) {
        int a = min(g_selA, g_caret), b = max(g_selA, g_caret);
        if (a < start) a = start;
        int xa = textWidthN(g_fSmall, buf + start, a - start), xb = textWidthN(g_fSmall, buf + start, b - start);
        if (xb > inner) xb = inner;
        fillAccentRect(in.left + xa, rc.top + dp(6), in.left + xb, rc.bottom - dp(6), 0.45);
    }
    g_trOff = TRUE;      // what's typed
    drawText(g_fSmall, buf + start, in, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
             (focus ? 0 : DT_PATH_ELLIPSIS));
    g_trOff = FALSE;
    if (focus && !ro) {
        int x = in.left + textWidthN(g_fSmall, buf + start, g_caret - start);
        Col t = textCol();
        fillRect(x, rc.top + dp(7), x + max(1, dp(1)), rc.bottom - dp(7), t.r, t.g, t.b, t.a);
    }
}
static int aioPathRow(int x1, int x2, int y, const wchar_t* label, int fid, wchar_t* buf, int bid) {
    RECT lr = { x1, y, x1 + dp(104), y + dp(32) };
    drawText(g_fSmall, label, lr, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT fr = { x1 + dp(108), y, x2 - dp(86), y + dp(32) };
    aioField(fid, fr, buf, MAX_PATH, FALSE, L"Type a path or choose Browse");
    aioButton(bid, x2 - dp(78), y, x2, y + dp(32), L"Browse", FALSE, g_busy == 0, FALSE);
    return y + dp(40);
}
// Status line and progress bar shared by every tool; Stop appears while its job runs.
static void aioStatus(int x1, int x2, int y) {
    if (g_aioRunning && g_aioJobTab == g_aioTab) {
        wchar_t line[128];
        const wchar_t* st = ST_TEXT[g_aioStage];
        if (g_aioPct >= 0) swprintf(line, 128, L"%s  %d%%", st, g_aioPct);
        else swprintf(line, 128, L"%s", st);
        int right = g_aioCanStop ? x2 - dp(86) : x2;
        RECT r = { x1, y, right, y + dp(20) };
        drawText(g_fSmall, line, r, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        drawProgressBar(x1, right, y + dp(24), g_aioPct);
        if (g_aioCanStop) aioButton(A_STOP, x2 - dp(78), y, x2, y + dp(30), L"Stop", FALSE, TRUE, FALSE);
    } else if (g_aioMsg[g_aioTab][0]) {
        RECT r = { x1, y, x2, y + dp(36) };
        drawText(g_fSmall, g_aioMsg[g_aioTab], r, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
}
static void aioNote(int x1, int x2, int y, int h, const wchar_t* text) {
    RECT r = { x1, y, x2, y + h };
    drawText(g_fSmall, text, r, mutedRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}

static void focusNone(void) { g_focus = 0; g_focusBuf = NULL; g_selA = g_caret = 0; g_fieldStart = 0; }

static void copyText(const wchar_t* s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) { memcpy(GlobalLock(g), s, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}

// ---------- input from WndProc ----------
static void wimHBarDragTo(int x) {
    int span = (g_rcWimHBar.right - g_rcWimHBar.left) - g_wimHThumbW;
    if (span <= 0 || !g_wimHMax) return;
    g_wimHS = (x - g_wimHDragOfs - g_rcWimHBar.left) * g_wimHMax / span;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void aioMouseDown(int x, int y) {
    POINT p = { x, y };
    if (g_page == PAGE_AIO && g_aioTab == AT_WIM && g_wimHMax && PtInRect(&g_rcWimHBar, p)) {
        int span = (g_rcWimHBar.right - g_rcWimHBar.left) - g_wimHThumbW;
        int tx = g_rcWimHBar.left + span * g_wimHS / g_wimHMax;
        g_wimHDragOfs = (x >= tx && x < tx + g_wimHThumbW) ? x - tx : g_wimHThumbW / 2;
        g_wimHDrag = TRUE;
        SetCapture(g_hwnd);
        wimHBarDragTo(x);
        return;
    }
    for (int i = 0; i < g_aioHitCount; i++) {
        AioHit* h = &g_aioHits[i];
        if (!h->buf || !PtInRect(&h->rc, p)) continue;
        if (g_focus != h->id) g_fieldStart = 0;
        g_focus = h->id; g_focusBuf = h->buf; g_focusCch = h->cch; g_focusRo = h->ro;
        int inner = x - (h->rc.left + dp(10)), n = (int)wcslen(h->buf), best = n;
        for (int k = g_fieldStart; k <= n; k++) {
            int w = textWidthN(g_fSmall, h->buf + g_fieldStart, k - g_fieldStart);
            if (w >= inner) { best = k; break; }
        }
        g_caret = g_selA = best;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    focusNone();
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void aioFieldChanged(void) {
    if ((g_focus == A_W_NAME || g_focus == A_W_DESC) && g_wimSel >= 0 && g_wimSel < g_wimCount) {
        WimEntry* e = &g_wim[g_wimSel];
        wcscpy(e->name, g_fName); wcscpy(e->desc, g_fDesc); e->dirty = TRUE;
    }
}
static void fieldDeleteSel(void) {
    int a = min(g_selA, g_caret), b = max(g_selA, g_caret);
    if (a == b) return;
    wmemmove(g_focusBuf + a, g_focusBuf + b, wcslen(g_focusBuf + b) + 1);
    g_caret = g_selA = a;
}
static void fieldInsert(const wchar_t* s) {
    if (g_focusRo) return;
    fieldDeleteSel();
    size_t len = wcslen(g_focusBuf), add = wcslen(s);
    if (len + add >= (size_t)g_focusCch) add = g_focusCch - 1 - len;
    if (!add) return;
    wmemmove(g_focusBuf + g_caret + add, g_focusBuf + g_caret, len - g_caret + 1);
    wmemcpy(g_focusBuf + g_caret, s, add);
    g_caret += (int)add; g_selA = g_caret;
    aioFieldChanged();
}
static BOOL aioChar(wchar_t c) {
    if (!g_focus || !g_focusBuf) return FALSE;
    int a = min(g_selA, g_caret), b = max(g_selA, g_caret);
    if (c == 1) { g_selA = 0; g_caret = (int)wcslen(g_focusBuf); }                       // Ctrl+A
    else if (c == 3 || c == 24) {                                                        // Ctrl+C / Ctrl+X
        if (a != b) {
            wchar_t t[1024]; int n = min(b - a, 1023);
            wmemcpy(t, g_focusBuf + a, n); t[n] = 0;
            copyText(t);
            if (c == 24 && !g_focusRo) { fieldDeleteSel(); aioFieldChanged(); }
        }
    } else if (c == 22 && !g_focusRo) {                                                  // Ctrl+V
        BOOL regBox = (g_focus == A_REG_BOX), pasted = FALSE;
        if (OpenClipboard(g_hwnd)) {
            HANDLE g = GetClipboardData(CF_UNICODETEXT);
            if (g) {
                const wchar_t* s = (const wchar_t*)GlobalLock(g);
                if (s) {
                    wchar_t t[1024]; int n = 0;
                    while (*s == L' ' || *s == L'\r' || *s == L'\n' || *s == L'\t') s++;
                    for (; s[n] && s[n] != L'\r' && s[n] != L'\n' && n < 1023; n++) t[n] = s[n];
                    t[n] = 0;
                    if (regBox) { g_selA = 0; g_caret = (int)wcslen(g_focusBuf); }   // a paste replaces the old key
                    fieldInsert(t);
                    pasted = (n > 0);
                    GlobalUnlock(g);
                }
            }
            CloseClipboard();
        }
        if (regBox && pasted) regGo();                   // pasting is the "open" action
    } else if (c == 13 && g_focus == A_REG_BOX) {                                        // Enter, for a typed key
        regGo();
    } else if (c == 13 && g_focus == A_E_SRC) {                                          // Enter lists a typed ESD's editions
        actionEsdLoad();
#if WCU_DOWNLOAD
    } else if (c == 13 && g_focus == A_D_BOX) {
        dlFind();
#endif
    } else if (c == 8 && !g_focusRo) {                                                   // Backspace
        if (a != b) fieldDeleteSel();
        else if (g_caret > 0) { g_selA = g_caret - 1; fieldDeleteSel(); }
        aioFieldChanged();
    } else if (c >= 32 && c != 127) {
        wchar_t t[2] = { c, 0 };
        fieldInsert(t);
    } else {
        return FALSE;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}
static BOOL aioKey(WPARAM vk) {
    if (!g_focus || !g_focusBuf) return FALSE;
    BOOL shift = GetKeyState(VK_SHIFT) < 0;
    int n = (int)wcslen(g_focusBuf);
    switch (vk) {
    case VK_LEFT:  if (g_caret > 0) g_caret--; break;
    case VK_RIGHT: if (g_caret < n) g_caret++; break;
    case VK_HOME:  g_caret = 0; break;
    case VK_END:   g_caret = n; break;
    case VK_DELETE:
        if (g_focusRo) return TRUE;
        if (g_selA == g_caret && g_caret < n) g_selA = g_caret + 1;
        fieldDeleteSel(); aioFieldChanged();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return TRUE;
    case VK_ESCAPE: focusNone(); InvalidateRect(g_hwnd, NULL, FALSE); return TRUE;
    default: return FALSE;
    }
    if (!shift) g_selA = g_caret;
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}
// Right-click in a text box pastes, as this box has no menu of its own
static void aioRightClick(int x, int y) {
    aioMouseDown(x, y);
    if (g_focus && g_focusBuf && !g_focusRo) aioChar(22);
}
static void aioWheel(int delta) {
    if (g_page == PAGE_AIO && g_aioTab == AT_ESD) {
        g_esdTop += (delta > 0) ? -1 : 1;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (g_page != PAGE_AIO || g_aioTab != AT_WIM) return;
    if (GetKeyState(VK_SHIFT) < 0) g_wimHS += (delta > 0) ? -dp(48) : dp(48);
    else g_wimTop += (delta > 0) ? -1 : 1;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void aioWheelH(int delta) {     // tilt wheel or touchpad
    if (g_page != PAGE_AIO || g_aioTab != AT_WIM) return;
    g_wimHS += delta * dp(48) / 120;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
