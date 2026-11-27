// Windows Chosen Utilities - Disk Clean-up
// A native Win32 app: Mica backdrop, dark/light following the system, rounded corners,
// and content styled with net-merge's metrics/colours (row height, radii, hover tints, acrylic base).
//
// It scans a fixed whitelist of clearly-safe locations (temp, recycle bin, update cache,
// thumbnail cache, crash dumps, error reports) plus restore points if ticked, shows sizes, and deletes
// only after one prompt. It never touches the component store or anything outside the list.


#include "core.h"
#include "gfx.c"
#include "sysutil.c"
#include "dialog.c"
#include "note.c"
#include "cleanup.c"
#include "uupapi.c"
#include "dism.c"
#include "aio_engine.c"
#include "widgets.c"
#include "regnet.c"
#include "aio.c"
#include "repair.c"
#include "esdiso.c"
#include "region.c"
#include "tweaks.c"
#include "runti.c"
#include "usbboot.c"
#if WCU_DOWNLOAD
#include "download.c"
#endif
#include "assoc.c"
#include "apps.c"
#include "office.c"
#include "remove.c"
#include "update.c"

// ---------- left navigation rail ----------
static const wchar_t* NAV_LABEL[NAV_COUNT] = { L"Disk Clean-up", L"Reg Networks", L"Run as TI", L"Manual Updates", L"Windows AIO", L"Windows Repair", L"Misc Tweaks",
#if WCU_DOWNLOAD
    L"Download Windows",
#endif
    L"Install Apps", L"MS Office 365", L"Remove Apps",
};
static const unsigned int NAV_GLYPH[NAV_COUNT] = { 0xEDA2, 0xE968, 0xE72E, 0xE90F, 0xE958, 0xE95E, 0xE713,
#if WCU_DOWNLOAD
    0xE896,
#endif
    0xE71D, 0xE8A5, 0xE74D,
};
// The order the rail shows the pages in, grouped by job: keeping Windows healthy (clean, repair, update), changing how
// it works (tweaks, network profiles, TrustedInstaller), getting and building Windows media, then apps. NAV_LABEL and
// NAV_GLYPH stay indexed by page; g_rcNav, g_hotNav and the rail's fades go by place in this list.
static const int NAV_ORDER[NAV_COUNT] = { PAGE_CLEANUP, PAGE_REPAIR, PAGE_DISM, PAGE_TWEAKS, PAGE_REGNET, PAGE_RUNTI,
#if WCU_DOWNLOAD
    PAGE_DOWNLOAD,
#endif
    PAGE_AIO, PAGE_APPS, PAGE_OFFICE, PAGE_REMOVE,
};

static void drawRail(int L, int R, int top, int bottom) {
    Col sf = surfaceCol();
    fillRoundRect(L, top, R, bottom, dp(8), sf.r, sf.g, sf.b, sf.a);

    RECT bt = { L + dp(16), top + dp(16), R - dp(66), top + dp(16) + dp(24) };
    drawText(g_fBodyBold, L"Chosen Utilities", bt, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // small dark/light switch: shows the mode you'd switch to (moon in light mode, sun in dark mode)
    RECT tb = { R - dp(36), top + dp(14), R - dp(8), top + dp(14) + dp(28) };
    g_rcTheme = tb;
    double tv = anim(ANIM_BTN + BTN_THEME, g_hotBtn == BTN_THEME);
    fillHover(tb.left, tb.top, tb.right, tb.bottom, dp(4), 2 * tv);
    drawGlyph(g_fIcon, g_dark ? 0xE706 : 0xE708, (tb.left + tb.right) / 2, (tb.top + tb.bottom) / 2, textRGB());

    // the language button: the theme switch's size and shape, to its left
    RECT lb = tb; OffsetRect(&lb, -dp(28), 0);
    g_rcLang = lb;
    fillHover(lb.left, lb.top, lb.right, lb.bottom, dp(4), 2 * anim(ANIM_LANG, g_hotBtn == BTN_LANG));
    drawGlyph(g_fIcon, 0xE774, (lb.left + lb.right) / 2, (lb.top + lb.bottom) / 2, textRGB());

    // The three buttons at the bottom take 128; on a short window the rail's items get a little shorter to leave them room
    int y = top + dp(58), itemH = dp(38), gap = dp(4);
    int fit = (bottom - dp(8) - dp(36 * 3 + 6 * 2) - dp(8) - y + gap) / NAV_COUNT;
    if (fit < itemH + gap) itemH = max(dp(30), fit - gap);
    for (int i = 0; i < NAV_COUNT; i++) {
        RECT it = { L + dp(8), y, R - dp(8), y + itemH };
        g_rcNav[i] = it;
        int pg = NAV_ORDER[i];
        BOOL sel = ((g_fadeAct == FADE_PAGE ? g_fadeArg : g_page) == pg);
        double sv = anim(ANIM_NAVSEL + i, sel), nv = anim(ANIM_NAV + i, i == g_hotNav && !sel);
        fillHover(it.left, it.top, it.right, it.bottom, dp(4), nv);
        if (sv > 0) {
            Col h = hoverCol();
            fillRoundRect(it.left, it.top, it.right, it.bottom, dp(4), h.r, h.g, h.b, h.a * 2.4 * sv);
            int pillH = dp(18), py = y + (itemH - pillH) / 2;
            fillAccent(it.left + dp(2), py, it.left + dp(5), py + pillH, dp(2), sv);
        }
        drawGlyph(g_fIcon, NAV_GLYPH[pg], it.left + dp(24), y + itemH / 2, sel ? g_accent : textRGB());
        if (pg == g_noteDot && !sel) {     // a finished job to look at, on the icon's corner
            COLORREF c = noteGreen();
            fillRoundRect(it.left + dp(31), y + dp(7), it.left + dp(39), y + dp(15), dp(4), GetRValue(c), GetGValue(c), GetBValue(c), 1.0);
        }
        RECT lt = { it.left + dp(44), y, it.right - dp(8), y + itemH };
        drawText(sel ? g_fBodyBold : g_fBody, NAV_LABEL[pg], lt, textRGB(),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += itemH + gap;
    }

    // "Boot to USB" and "Boot to UEFI BIOS" actions, pinned to the bottom of the rail so it's reachable from every page
    RECT b = { L + dp(8), bottom - dp(8) - dp(36), R - dp(8), bottom - dp(8) };
    g_rcUefi = b;
    BOOL en = (g_busy == 0);
    drawButtonFace(b, dp(6), FALSE, anim(ANIM_BTN + BTN_UEFI, g_hotBtn == BTN_UEFI && en), en ? 1.0 : 0.4);
    drawButtonLabel(b, g_fBody, L"Boot to UEFI BIOS", en ? textRGB() : mutedRGB(), !g_elevated, dp(16), dp(6));
    OffsetRect(&b, 0, -dp(36 + 6));
    g_rcUsbBoot = b;
    drawButtonFace(b, dp(6), FALSE, anim(ANIM_BTN + BTN_USBBOOT, g_hotBtn == BTN_USBBOOT && en), en ? 1.0 : 0.4);
    drawButtonLabel(b, g_fBody, L"Boot to USB", en ? textRGB() : mutedRGB(), !g_elevated, dp(16), dp(6));
    OffsetRect(&b, 0, -dp(36 + 6));
    suDrawButton(b);                  // on only when GitHub has a newer release
}

static void render(void) {
    memset(g_bits, 0, (size_t)g_bw * g_bh * 4);   // transparent -> Mica shows
    stepFade();
    aioResetHits();
    int W = g_bw;
    int pad = dp(16), railW = dp(196);
    int railL = pad, railR = pad + railW;
    int contentL = railR + dp(12), contentR = W - pad;

    // a tool change fades only the AIO pane; page and theme changes fade the content area
    g_tabMul = (g_fadeAct == FADE_TAB) ? g_fadeV : 1.0;
    g_alphaMul = (g_fadeAct == FADE_THEME) ? g_fadeV : 1.0;
    drawRail(railL, railR, pad, g_bh - pad);
    g_alphaMul = (g_fadeAct == FADE_PAGE || g_fadeAct == FADE_THEME) ? g_fadeV : 1.0;
    if (g_page == PAGE_CLEANUP)     drawCleanup(contentL, contentR);
    else if (g_page == PAGE_REGNET) drawRegNet(contentL, contentR);
    else if (g_page == PAGE_RUNTI)  drawRunTI(contentL, contentR);
    else if (g_page == PAGE_DISM)   drawDism(contentL, contentR);
    else if (g_page == PAGE_REPAIR) drawRepair(contentL, contentR);
    else if (g_page == PAGE_TWEAKS) drawTweaks(contentL, contentR);
#if WCU_DOWNLOAD
    else if (g_page == PAGE_DOWNLOAD) drawDownload(contentL, contentR);
#endif
    else if (g_page == PAGE_APPS)   drawApps(contentL, contentR);
    else if (g_page == PAGE_OFFICE) drawOffice(contentL, contentR);
    else if (g_page == PAGE_REMOVE) drawRemove(contentL, contentR);
    else                            drawAio(contentL, contentR);
    g_alphaMul = 1.0;
    // the version, small, in the margin under the pages' bottom-right corner
    RECT vr = { contentR - dp(160), g_bh - pad, contentR - dp(4), g_bh - dp(1) };
    drawText(g_fTiny, WCU_VERSION, vr, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    double dim = anim(ANIM_DIM, g_dlgDepth > 0);                     // dim behind a modal dialog
    if (dim > 0) fillRect(0, 0, g_bw, g_bh, 0, 0, 0, 0.30 * dim);
}

static void paint(HDC hdc) {
    g_needFrame = FALSE;
    render();
    BitBlt(hdc, 0, 0, g_bw, g_bh, g_memDC, 0, 0, SRCCOPY);
    // redraw only while something is still fading
    static BOOL timerOn = FALSE;
    if (g_needFrame && !timerOn) { SetTimer(g_hwnd, 1, 16, NULL); timerOn = TRUE; }
    else if (!g_needFrame && timerOn) { KillTimer(g_hwnd, 1); timerOn = FALSE; }
}

static const wchar_t* THEME_KEY = L"Software\\WindowsChosenUtilities";
// The app follows Windows. The switch in the rail overrides that until Windows' own theme next changes, so
// the override is saved along with the Windows theme it was chosen against (1 = dark, 2 = light).
static void clearThemeOverride(void) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, THEME_KEY, L"Theme");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, THEME_KEY, L"ThemeBase");
}
static void loadTheme(void) {
    g_winDark = readDarkTheme();
    g_dark = g_winDark;
    DWORD theme = 0, base = 0, cb = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, L"Theme", RRF_RT_REG_DWORD, NULL, &theme, &cb) != ERROR_SUCCESS) return;
    cb = sizeof(DWORD);
    RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, L"ThemeBase", RRF_RT_REG_DWORD, NULL, &base, &cb);
    if (base == (g_winDark ? 1u : 2u)) g_dark = (theme == 1);
    else clearThemeOverride();              // Windows has changed since, or it's from an older version
}
static void toggleTheme(void) {
    g_dark = !g_dark;
    if (g_dark == g_winDark) clearThemeOverride();     // back to matching Windows, so back to following it
    else {
        HKEY k; DWORD v = g_dark ? 1 : 2, base = g_winDark ? 1 : 2;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, THEME_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"Theme", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
            RegSetValueExW(k, L"ThemeBase", 0, REG_DWORD, (const BYTE*)&base, sizeof(base));
            RegCloseKey(k);
        }
    }
    g_accent = readAccent();
    applyWindowTheme(g_hwnd);
}
static void applyFade(void) {
    if (g_fadeAct == FADE_PAGE) {
        g_page = g_fadeArg;
        if (g_page == g_noteDot) g_noteDot = -1;
        focusNone();
        if (g_page == PAGE_REGNET) regFocusBox();
        if (g_page == PAGE_TWEAKS) tweaksRefresh();
        if (g_page == PAGE_APPS && g_appsRun < 0) { appsRefresh(); appsCheckLatest(FALSE); if (g_gpuRun < 0) { gpuRefresh(); gpuCheckLatest(); } }
        if (g_page == PAGE_OFFICE) offRefresh();
        if (g_page == PAGE_REMOVE) rmRefresh();
    }
    else if (g_fadeAct == FADE_TAB) g_aioTab = g_fadeArg;
    else if (g_fadeAct == FADE_THEME) toggleTheme();
}

static BOOL pageHasFields(void) { return g_page == PAGE_AIO || g_page == PAGE_REGNET || g_page == PAGE_RUNTI || g_page == PAGE_DOWNLOAD; }

static int hitSizeLink(int x, int y) {
    POINT p = { x, y };
    if (g_page != PAGE_CLEANUP) return -1;
    for (int i = 0; i < NCAT; i++) if (PtInRect(&g_rcSizeLink[i], p)) return i;
    return -1;
}

static int hitButton(int x, int y) {
    POINT p = { x, y };
    if (PtInRect(&g_rcUefi, p)) return BTN_UEFI;
    if (PtInRect(&g_rcUsbBoot, p)) return BTN_USBBOOT;
    if (PtInRect(&g_rcSelfUpd, p)) return BTN_SELFUPD;
    if (PtInRect(&g_rcTheme, p)) return BTN_THEME;
    if (PtInRect(&g_rcLang, p)) return BTN_LANG;
    if (g_page == PAGE_AIO || g_page == PAGE_REPAIR || g_page == PAGE_TWEAKS || g_page == PAGE_RUNTI || g_page == PAGE_DOWNLOAD || g_page == PAGE_APPS || g_page == PAGE_OFFICE || g_page == PAGE_REMOVE) {
        int id = aioHitTest(x, y);
        return id ? BTN_WIDGET + id : -1;
    }
    if (g_page == PAGE_REGNET) {
        int id = aioHitTest(x, y);
        if (id) return BTN_WIDGET + id;
    }
    if (g_page == PAGE_CLEANUP) {
        if (PtInRect(&g_rcBtnRescan, p)) return BTN_RESCAN;
        if (PtInRect(&g_rcBtnClean, p)) return BTN_CLEAN;
        for (int i = 0; i < NCAT; i++) if (PtInRect(&g_rcView[i], p)) return BTN_VIEW + i;
        if (PtInRect(&g_rcSrOff, p)) return BTN_SROFF;
    } else if (g_page == PAGE_REGNET) {
        if (PtInRect(&g_rcRegOpen, p)) return BTN_REGOPEN;
        if (PtInRect(&g_rcRegPurge, p)) return BTN_REGPURGE;
    } else if (g_page == PAGE_DISM) {
        if (PtInRect(&g_rcDismInstall, p)) return BTN_DISM;
        if (PtInRect(&g_rcUpdGet, p)) return BTN_UPDGET;
        if (PtInRect(&g_rcUpdNdp, p)) return BTN_UPDNDP;
    }
    return -1;
}
static int hitRow(int x, int y) {
    POINT p = { x, y };
    if (g_page != PAGE_CLEANUP) return -1;
    for (int i = 0; i < NCAT; i++) if (PtInRect(&g_rcRow[i], p)) return i;
    return -1;
}
static int hitNav(int x, int y) {
    POINT p = { x, y };
    for (int i = 0; i < NAV_COUNT; i++) if (PtInRect(&g_rcNav[i], p)) return i;
    return -1;
}

// Restart into the motherboard's UEFI setup. "Boot to firmware UI" is part of the UEFI standard, so
// this is the same on every vendor's board. /t 0 is a normal restart, not a forced one.
static void bootToFirmware(void) {
    FIRMWARE_TYPE ft = FirmwareTypeUnknown;
    if (!GetFirmwareType(&ft) || ft != FirmwareTypeUefi) {
        okDialog(L"Boot to UEFI BIOS isn't available",
                   L"Windows started in Legacy BIOS mode on this PC, so it can't restart into firmware "
                   L"setup. Press Del during startup instead.");
        return;
    }
    if (askInApp() &&
        !showDialog(g_hwnd, L"Restart to UEFI settings?",
                    L"Your PC will restart now and open the motherboard's UEFI settings. "
                    L"Save your work first.", NULL, 0, L"Restart", L"Cancel", 1)) return;

    wchar_t exe[MAX_PATH];
    systemPath(exe, L"shutdown.exe");
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_hwnd;
    sei.lpVerb = g_elevated ? L"open" : L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"/r /fw /t 0";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        raiseStart(g_hwnd);
        if (err != ERROR_CANCELLED)
            okDialog(L"Couldn't restart", L"Windows couldn't start the restart command.");
        return;                       // declining UAC changes nothing
    }
    DWORD code = 0;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 10000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    if (code != 0 && code != STILL_ACTIVE) {
        wchar_t msg[256];
        swprintf(msg, 256, L"Windows refused to restart into UEFI settings (error %lu). "
                           L"Press Del during startup instead.", code);
        okDialog(L"Couldn't restart", msg);
    }
}

// The language list: Follow Windows, then each language under its own name
static void langPick(void) {
    const wchar_t* items[LANG_N + 1] = { L"Follow Windows" };
    for (int i = 0; i < LANG_N; i++) items[i + 1] = LANG_LIST[i].name;
    int pick = g_langSet + 1;
    DlgExtra list; memset(&list, 0, sizeof(list));
    list.items = items; list.nItems = LANG_N + 1; list.pick = &pick;
    if (!showDialogEx(g_hwnd, L"Language", L"Choose the app's language. Logs and output boxes stay in English.",
                      NULL, 0, L"Choose", L"Cancel", 0, &list)) return;
    langChoose(pick - 1);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void aioClick(int id, int x, int y) {
    (void)x;
    if (id >= A_TAB0 && id <= A_TABLAST) {
        int t = id - A_TAB0;
        if (t != g_aioTab) { focusNone(); startFade(FADE_TAB, t); }
        return;
    }
    if (id >= A_U_ROW0 && id <= A_U_ROWLAST) { g_usbSel = id - A_U_ROW0; InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (id == A_E_CANCEL) { actionEsdCancel(); return; }
    if (id >= A_E_CH0 && id <= A_E_CHLAST) {           // the channel in our naming: one always chosen
        if (g_busy == 0) g_eiChannel = id - A_E_CH0;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (id >= A_E_ROW0 && id <= A_E_ROWLAST) {         // tick or untick an edition
        int i = g_esdTop + id - A_E_ROW0;
        if (g_busy == 0 && i < g_esdN) g_esdTick[i] = !g_esdTick[i];
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (id == A_REG_OPEN) { regGo(); return; }
    if (id == A_R_COPY) { repairCopy(); return; }
    if (id >= A_O_CUR && id <= A_O_SW2) { offClick(id); return; }
    if (id >= A_X_WINAPP && id <= A_X_UNINSTALL) { rmClick(id, y); return; }
#if WCU_DOWNLOAD
    if (id >= A_D_X64 && id <= A_D_WORKDEF) { dlClick(id); return; }
#endif
    if (id == A_T_TERMINAL) { tweakTerminalRestore(); InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (id == A_R_TOGGLE) { g_repShow = !g_repShow; InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (id >= A_H_C0 && id <= A_H_C3) { copyText(g_hashOut[id - A_H_C0]); return; }
    if (id == A_STOP) { if (g_aioRunning && g_aioCanStop) InterlockedExchange(&g_aioStop, 1); return; }
    if (id == A_W_HBAR) return;
    if (id >= A_W_MOPT0 && id <= A_W_MOPTLAST) {       // ticking one build unticks the others
        int m = id - A_W_MOPT0;
        if (m < g_mediaCount && g_busy == 0) { g_mediaSel = m; wcscpy(g_mediaSelBuild, g_mediaOpts[m].build); }
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (id == A_W_LIST) {
        int v = g_wimTop + (y - g_wimRowsTop) / max(1, g_wimRowH);
        if (v >= 0 && v < g_wimRowsCount && g_wimRows[v] >= 0 && g_busy == 0) wimSelect(g_wimRows[v]);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (g_busy != 0) return;
    switch (id) {
    case A_W_SELECT: actionWimSelect(); break;
    case A_W_ASWIM:  actionSaveWim(); break;
    case A_W_ASESD:  actionSaveEsd(); break;
    case A_W_ASFOLDER: actionSaveFolder(); break;
    case A_W_ASISO:  actionSaveIso(); break;
    case A_W_DELETE: actionWimDelete(); break;
    case A_W_UP:     actionWimMove(-1); break;
    case A_W_DOWN:   actionWimMove(1); break;
    case A_W_RESET:  actionWimReset(); break;
    case A_E_SRCB:   actionEsdBrowse(); break;
    case A_E_ALL:    actionEsdTickAll(TRUE); break;
    case A_E_NONE:   actionEsdTickAll(FALSE); break;
    case A_E_DIRB:   actionEsdDir(); break;
    case A_E_NOURS:  g_eiNameMode = 0; break;
    case A_E_NABB:   g_eiNameMode = 1; break;
    case A_E_GO:     actionEsdIso(); break;
    case A_F_SRCB:   pickFolder(g_fSrc); break;
    case A_F_OUTB:   pickSave(g_fOut, ISO_FILTER, L"iso", L"ISO to save"); break;
    case A_F_BUILD:  actionF2I(); break;
    case A_I_SRCB:   pickOpen(g_iSrc, ISO_FILTER, L"Source ISO"); break;
    case A_I_DSTB:   pickFolder(g_iDst); break;
    case A_I_GO:     actionI2F(); break;
    case A_U_SRCB:   pickFolder(g_uSrc); break;
    case A_U_REFRESH: usbRefresh(); break;
    case A_U_START:  actionUsb(); break;
    case A_H_FILEB:  pickOpen(g_hFile, L"All files (*.*)\0*.*\0", L"Choose a file"); break;
    case A_H_GO:     actionHash(); break;
    case A_R_SFC: case A_R_SCAN: case A_R_RESTORE: case A_R_WIM: case A_R_RS: case A_R_WUC: repairAction(id); break;
    case A_TI_BROWSE: case A_TI_RUN: tiClick(id); break;
    case A_P_FIREFOX: appsClick(APP_FIREFOX); break;
    case A_P_IDM:     appsClick(APP_IDM); break;
    case A_P_POT:     appsClick(APP_POT); break;
    case A_P_CRD:     appsClick(APP_CRD); break;
    case A_P_NANAZIP: appsClick(APP_NANAZIP); break;
    case A_P_TAILSCALE: appsClick(APP_TAILSCALE); break;
    case A_P_NVIDIA:  gpuClick(GPU_NV); break;
    case A_P_AMD:     gpuClick(GPU_AMD); break;
    case A_P_UPD0: case A_P_UPD0 + 1: case A_P_UPD0 + 2: case A_P_UPD0 + 3: case A_P_UPD0 + 4: case A_P_UPD0 + 5:
        appsUpdateClick(id - A_P_UPD0); break;
    case A_T_EXPLORE: case A_T_GALLERY: case A_T_VBS: case A_T_RDP: case A_T_MAPDELAY: case A_T_WUDRV: case A_T_THEME: case A_T_REGION: case A_T_ANNOY:
        tweakButtonClicked(id); break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void onLeftButtonUp(int x, int y) {
    if (g_wimHDrag) { g_wimHDrag = FALSE; ReleaseCapture(); g_pressBtn = -1; InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (g_repDrag) { g_repDrag = FALSE; ReleaseCapture(); g_pressBtn = -1; InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (g_logDrag) { g_logDrag = FALSE; ReleaseCapture(); g_pressBtn = -1; InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (g_rmDrag) { g_rmDrag = FALSE; ReleaseCapture(); g_pressBtn = -1; InvalidateRect(g_hwnd, NULL, FALSE); return; }
    int nav = hitNav(x, y);
    if (nav != -1) nav = NAV_ORDER[nav];   // a place in the rail -> its page
    if (nav != -1) {                       // switching pages never prompts for anything
        if (g_page != nav && !(g_fadeAct == FADE_PAGE && g_fadeArg == nav)) startFade(FADE_PAGE, nav);
        g_pressBtn = -1;
        return;
    }
    int b = hitButton(x, y);
    if (b != -1 && b == g_pressBtn) {
        if (b == BTN_RESCAN && g_busy == 0) startScan();
        else if (b == BTN_CLEAN && g_busy == 0 && anyChecked()) doClean();
        else if (b == BTN_REGOPEN) openRegeditAt(REGNET_KEY);
        else if (b == BTN_REGPURGE) doPurgeNetworks();
        else if (b == BTN_UEFI && g_busy == 0) bootToFirmware();
        else if (b == BTN_USBBOOT && g_busy == 0) bootToUsb();
        else if (b == BTN_SELFUPD) suClick();
        else if (b == BTN_DISM && g_busy == 0) doDismInstall();
        else if (b == BTN_UPDGET) updClicked(UPD_MSU);
        else if (b == BTN_UPDNDP) updClicked(UPD_NDP);
        else if (b == BTN_THEME) startFade(FADE_THEME, 0);
        else if (b == BTN_LANG) langPick();
        else if (b >= BTN_VIEW && b < BTN_VIEW + NCAT) viewCategory(&g_cat[b - BTN_VIEW]);
        else if (b == BTN_SROFF && g_busy == 0) doSrSwitch();
        else if (b >= BTN_WIDGET) aioClick(b - BTN_WIDGET, x, y);
    } else if (hitSizeLink(x, y) != -1) {
        if (g_busy == 0) startElevatedMeasure();
    } else if (b == -1) {
        int r = hitRow(x, y);
        if (r != -1 && g_rowsEnabled[r] && g_busy == 0) {
            g_cat[r].checked = !g_cat[r].checked;
            // the two restore rows overlap, so only one can be ticked
            if (g_cat[r].checked && isRestoreKind(&g_cat[r]))
                for (int i = 0; i < NCAT; i++)
                    if (i != r && isRestoreKind(&g_cat[i])) g_cat[i].checked = FALSE;
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
    g_pressBtn = -1;
}

// The job that closing the app (or updating it, which closes it) would lose track of: its dialog title, and in *body
// what to do. NULL when nothing is running.
static const wchar_t* jobRunningTitle(const wchar_t** body) {
    if (g_suState == SU_WORKING) {
        *body = L"Wait for the update to finish. The app restarts by itself.";
        return L"The app is updating";
    }
    if (g_gpuRun >= 0) {
        *body = g_gpuStep == 2 ? L"Press Cancel download on the Install Apps page first, or wait for it to finish."
                               : L"Wait for the driver lookup on the Install Apps page to finish.";
        return L"Driver still downloading";
    }
    if (g_dismRunning) {
        *body = L"Wait for the update to finish before closing the app.";
        return L"Update still installing";
    }
    if (g_updState != UPD_IDLE) {
        *body = L"Press Cancel download on the Manual Updates page first, or wait for it to finish.";
        return L"Update still downloading";
    }
    if (g_offJob != OFFJ_NONE) {
        *body = g_offJob == OFFJ_DOWNLOAD ? L"Press Cancel on the MS Office 365 page first, or wait for the download to finish."
                                          : L"Wait for the install on the MS Office 365 page to finish before closing the app.";
        return g_offJob == OFFJ_DOWNLOAD ? L"Office is still downloading" : L"Office is still installing";
    }
    if (g_rmJob) {
        *body = L"Wait for the job on the Remove Apps page to finish before closing the app.";
        return g_rmRemoving ? L"Still removing apps" : L"Still installing apps";
    }
    if (g_appsRun >= 0) {
        *body = L"Wait for the install on the Install Apps page to finish before closing the app.";
        return L"Still installing";
    }
    if (g_tiRunning) {
        *body = L"Wait for the program or import on the Run as TI page to finish before closing the app.";
        return L"Still running as TrustedInstaller";
    }
#if WCU_DOWNLOAD
    if (g_repRunning && g_repJob == REP_UUP) {
        *body = L"Press Cancel on the Download Windows page first, or wait for the ISO to finish.";
        return L"Windows is still downloading";
    }
#endif
    if (g_repRunning && g_repJob == REP_ESDISO) {
        *body = L"Press Cancel on the ESD to ISO tab of Windows AIO first, or wait for the ISO to finish.";
        return L"Still making the ISO";
    }
    if (g_repRunning) {
        *body = L"Wait for the repair to finish before closing the app. It can't be cancelled once it starts.";
        return L"Repair still running";
    }
    if (g_aioRunning) {
        *body = L"Wait for the current Windows AIO task to finish, or stop it first.";
        return L"Still working";
    }
    return NULL;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_dpi = GetDpiForWindow(h);
        makeFonts();
        sizeToContent(h);
        startScan();
        return 0;

    case WM_ERASEBKGND:
        return 1;  // we paint everything; prevents flashing

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        resizeBackbuffer(rc.right, rc.bottom);
        paint(hdc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        makeFonts();
        RECT* nr = (RECT*)l;
        SetWindowPos(h, NULL, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (g_wimHDrag) { wimHBarDragTo(x); return 0; }
        if (g_repDrag) { repairDragTo(y); return 0; }
        if (g_logDrag) { appsLogDragTo(y); return 0; }
        if (g_rmDrag) { rmDragTo(y); return 0; }
        if (g_page == PAGE_REMOVE) rmMouseMove(x, y);
        int nr = hitRow(x, y), nb = hitButton(x, y), nn = hitNav(x, y);
        if (nr != g_hotRow || nb != g_hotBtn || nn != g_hotNav) {
            g_hotRow = nr; g_hotBtn = nb; g_hotNav = nn; InvalidateRect(h, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hotRow = -1; g_hotBtn = -1; g_hotNav = -1; InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_TIMER:
        if (w == TIMER_RAISE) { raiseTick(h); return 0; }
        if (w == TIMER_SUCHECK) { suCheck(); return 0; }
        if (w == TIMER_LOGCOPIED) { KillTimer(h, w); InvalidateRect(h, NULL, FALSE); return 0; }
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_APP_RAISE:
        raiseStart(h);
        return 0;

    case WM_LBUTTONDOWN:
        g_pressBtn = hitButton(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (pageHasFields()) aioMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_page == PAGE_REPAIR || g_page == PAGE_DOWNLOAD) repairMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_page == PAGE_APPS || g_page == PAGE_OFFICE) appsLogMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_page == PAGE_REMOVE && !rmMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l))) appsLogMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;

    case WM_RBUTTONUP:
        if (pageHasFields() && aioOverField(GET_X_LPARAM(l), GET_Y_LPARAM(l)))
            aioRightClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_CHAR:
        if (pageHasFields() && aioChar((wchar_t)w)) return 0;
        break;
    case WM_KEYDOWN:
        if (pageHasFields() && aioKey(w)) return 0;
        break;
    case WM_MOUSEWHEEL:
        if (g_page == PAGE_AIO) aioWheel(GET_WHEEL_DELTA_WPARAM(w));
        else if (g_page == PAGE_REPAIR || g_page == PAGE_DOWNLOAD) repairWheel(GET_WHEEL_DELTA_WPARAM(w));
        else if (g_page == PAGE_APPS || g_page == PAGE_OFFICE) {
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };   // the wheel gives screen coordinates
            ScreenToClient(h, &p);
            appsLogWheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(w));
        } else if (g_page == PAGE_REMOVE) {
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &p);
            if (!rmWheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(w))) appsLogWheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(w));
        }
        return 0;
    case WM_MOUSEHWHEEL:
        if (g_page == PAGE_AIO) aioWheelH(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && pageHasFields()) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (aioOverField(p.x, p.y)) { SetCursor(LoadCursor(NULL, IDC_IBEAM)); return TRUE; }
        }
        if (LOWORD(l) == HTCLIENT && (g_page == PAGE_APPS || g_page == PAGE_OFFICE || g_page == PAGE_REMOVE)) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (appsLogLinkAt(p.x, p.y)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }   // a link in the log
        }
        break;

    case WM_LBUTTONUP:
        onLeftButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;

    case WM_SETTINGCHANGE:
        if (l && !wcscmp((wchar_t*)l, L"ImmersiveColorSet")) {
            // this also arrives for accent changes, so only a real theme change ends an override
            BOOL winDark = readDarkTheme();
            if (winDark != g_winDark) {
                g_winDark = g_dark = winDark;
                clearThemeOverride();
            }
            g_accent = readAccent();
            applyWindowTheme(h);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_APP_CATDONE:
    case WM_APP_CLEANPROG:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_APP_DISMPROG:
        g_dismPct = (w > 100) ? 100 : (int)w;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_APP_DISMDONE:
        dismOnDone((DWORD)w);
        return 0;
    case WM_APP_UPDFOUND:
        updOnFound((LONG)w);
        return 0;
    case WM_APP_UPDDONE:
        updOnDone((DWORD)w);
        return 0;
    case WM_APP_RUNTI_DONE:
        tiOnDone((DWORD)w);
        return 0;
    case WM_APP_APPSPROG:
        appsOnProgress((int)w, (int)l);
        return 0;
    case WM_APP_APPSDONE:
        appsOnDone((DWORD)w);
        return 0;
    case WM_APP_APPSLATEST:
        appsOnLatest((wchar_t*)l);
        return 0;
    case WM_APP_OFFLATEST:
        offOnLatest((wchar_t*)l);
        return 0;
    case WM_APP_OFFDONE:
        offOnDone((DWORD)w, (wchar_t*)l);
        return 0;
    case WM_APP_OFFVER:
        offOnVersion((wchar_t*)l);
        return 0;
    case WM_APP_RMDONE:
        rmOnDone(w != 0, (RmList*)l);
        return 0;
    case WM_APP_GPUFOUND:
        gpuOnFound((GpuOffer*)l);
        return 0;
    case WM_APP_GPULATEST:
        gpuOnLatest((wchar_t*)l);
        return 0;
    case WM_APP_GPUDONE:
        gpuOnDone((DWORD)w, (DWORD)l);
        return 0;
    case WM_APP_SUFOUND:
        suOnFound((SuInfo*)l);
        return 0;
    case WM_APP_SUPROG:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_APP_SUDONE:
        suOnDone((DWORD)w);
        return 0;
    case WM_APP_SUTRAY:
        suOnTray(l);
        return 0;
    case WM_COPYDATA:                    // the Install Apps and Office helpers' progress lines
        if (appsOnCopyData((const COPYDATASTRUCT*)l)) return TRUE;
        break;

    case WM_APP_AIOPROG:
        aioOnProgress((int)w, (int)l);
        return 0;
    case WM_APP_AIODONE:
        aioOnDone((DWORD)w);
        return 0;
    case WM_APP_REPAIROUT:
        InterlockedExchange(&g_repPosted, 0);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_APP_REPAIRDONE:
        repairOnDone((DWORD)w);
        return 0;
    case WM_APP_TWEAKDONE:
        tweakOnDone((DWORD)w);
        return 0;
#if WCU_DOWNLOAD
    case WM_APP_DLRESULT:
        dlOnResult((DlResult*)l);
        return 0;
#endif
    case WM_ACTIVATEAPP:                 // the setting may have been changed elsewhere, such as in Windows Security
        if (w && g_page == PAGE_TWEAKS && g_twAct < 0) tweaksRefresh();
        return 0;

    case WM_CLOSE: {                     // closing now would lose track of the running job
        const wchar_t* body = NULL;
        const wchar_t* title = jobRunningTitle(&body);
        if (title) { okDialog(title, body); return 0; }
        break;
    }

    case WM_APP_ADMINSIZE: {
        if (w >= NCAT || !g_cat[w].needsAdmin) return 0;
        Category* c = &g_cat[w];
        EnterCriticalSection(&g_lock);
        c->adminBytes = (unsigned long long)l; c->adminKnown = TRUE;
        c->bytes = c->adminBytes; c->scanned = TRUE;
        LeaveCriticalSection(&g_lock);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_APP_SCANDONE:
        cleanupOnScanDone();
        return 0;
    case WM_APP_CLEANDONE:
        cleanupOnCleanDone();
        return 0;
    case WM_APP_EXPLORERDONE: {
        InterlockedExchange(&g_busy, 0);
        startScan();
        return 0;
    }
    case WM_APP_SRDONE: {
        if (w) g_srOff = g_srWantOff;
        g_scanNote = !w ? L"Couldn't change System Restore. "
                        : g_srOff ? L"System Restore is off. " : L"System Restore is on. ";
        InterlockedExchange(&g_busy, 0);
        startScan();                     // turning it off may have removed restore points
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        // Width: never narrower than the 860 the pages are laid out for (and that the window opens at). Narrower,
        // buttons overlap or clip on Download Windows, Reg Networks, Windows AIO and Windows Repair.
        RECT r = { 0, 0, dp(860), 0 };
        AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE), g_dpi);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = dp(700);   // keeps every Misc Tweaks row, and so every other page, in view
        return 0;
    }

    case WM_DESTROY:
        suTrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

#ifndef WCU_TEST
// The one-shot modes of this exe, mostly elevated copies doing one job for the window that started them.
// Returns TRUE and sets *exitCode if the command line asked for one.
static BOOL runHelperCommand(const wchar_t* cmd, int* exitCode) {
    *exitCode = 0;

    // restart once into a USB drive, or remove that one-time entry again at the next start of Windows
    if (!wcsncmp(cmd, L"--usbboot", 9)) { *exitCode = ubHelperMain(cmd); return TRUE; }

    // put a downloaded update in place of this exe, for a folder that needs administrator rights. Checked ahead of
    // the flag matches below because the command line holds a file path.
    if (wcsstr(cmd, L"--self-update=\"")) { *exitCode = suHelperMain(cmd); return TRUE; }

    // run a Windows Repair job, sending its output back to the window
    if (!wcsncmp(cmd, L"--repair=", 9)) { *exitCode = repairHelperMain(cmd); return TRUE; }

    // make one Misc Tweaks change
    if (!wcsncmp(cmd, L"--tweak=", 8)) { *exitCode = tweakHelperMain(cmd); return TRUE; }

    // download and install one of the Install Apps page's apps (a number, never a file or a link)
    if (!wcsncmp(cmd, L"--apps-install=", 15)) { *exitCode = appsHelperMain(cmd); return TRUE; }

    // Install one update and return DISM's exit code. This is checked ahead of the flags below because
    // the command line holds a file path, which must not be mistaken for them.
    const wchar_t* arg = wcsstr(cmd, L"--dism-install=\"");
    if (arg) {
        wchar_t path[MAX_PATH];
        cmdQuoted(arg + 16, path, MAX_PATH);
        *exitCode = (int)runDismInstall(path, cmdNotify(cmd));
        return TRUE;
    }

    // Install Office from a source folder. Checked here, ahead of the flag matches below, because it holds a path.
    if (wcsstr(cmd, L"--office-install=\"")) { *exitCode = offHelperMain(cmd); return TRUE; }
    if (!wcsncmp(cmd, L"--office-channel=", 17)) { *exitCode = offChannelHelperMain(cmd); return TRUE; }

    // Run a user-chosen program or registry file as TrustedInstaller. Checked here, ahead of the flag matches
    // below, because the command line holds a file path.
    arg = wcsstr(cmd, L"--runti-reg=\"");
    if (arg) {
        wchar_t path[MAX_PATH];
        cmdQuoted(arg + 13, path, MAX_PATH);
        *exitCode = (int)runTiReg(path);
        return TRUE;
    }
    arg = wcsstr(cmd, L"--runti-exe=\"");
    if (arg) {
        wchar_t path[MAX_PATH];
        cmdQuoted(arg + 13, path, MAX_PATH);
        *exitCode = (int)runTiExe(path);
        return TRUE;
    }

    // ESD compressor: DISM's recovery compression needs administrator rights
    arg = wcsstr(cmd, L"--esd-src=\"");
    if (arg) {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        const wchar_t* p = cmdQuoted(arg + 11, src, MAX_PATH);
        const wchar_t* d = *p ? wcsstr(p, L"--esd-dst=\"") : NULL;
        if (!d) { *exitCode = ERROR_INVALID_PARAMETER; return TRUE; }
        p = cmdQuoted(d + 11, dst, MAX_PATH);
        const wchar_t* c = *p ? wcsstr(p, L"--count=") : NULL;
        g_aioNotify = *p ? cmdNotify(p) : NULL;
        *exitCode = (int)runEsdExport(src, dst, c ? _wtoi(c + 8) : 1);
        return TRUE;
    }

    // USB writer. It repeats the safety check itself before erasing anything.
    arg = wcsstr(cmd, L"--usb-write=\"");
    if (arg) {
        wchar_t path[MAX_PATH];
        const wchar_t* p = cmdQuoted(arg + 13, path, MAX_PATH);
        const wchar_t* disk = *p ? wcsstr(p, L"--disk=") : NULL;
        if (!disk) { *exitCode = ERROR_INVALID_PARAMETER; return TRUE; }
        g_aioNotify = cmdNotify(p);
        *exitCode = (int)usbWrite(path, _wtoi(disk + 7));
        return TRUE;
    }

    // do the network profile purge, report, and exit
    if (wcsstr(cmd, L"--purge-networks")) {
        wchar_t rep[4096]; int del = 0; BOOL den = FALSE;
        int m = purgeNetworks(HKEY_LOCAL_MACHINE, PROFILES_SUBPATH, TRUE, &del, &den, rep, 4096);
        wchar_t done[256];
        if (m < 0) wcscpy(done, L"Could not open the network profiles key.");
        else if (m == 0) wcscpy(done, L"No generic “Network N” profiles were found.");
        else if (den) swprintf(done, 256, L"Removed %d of %d profile(s); some were protected.", del, m);
        else swprintf(done, 256, L"Removed %d network profile(s).", del);
        showDialog(NULL, L"Network profiles", done, NULL, 0, L"OK", NULL, 0);
        SetForegroundWindow(cmdNotify(cmd));    // the click on OK lets this process hand the foreground back
        return TRUE;
    }

    // switch System Restore, refresh the sizes, and report through the exit code
    if (wcsstr(cmd, L"--sr-off") || wcsstr(cmd, L"--sr-on")) {
        BOOL ok = restoreSetOff(wcsstr(cmd, L"--sr-off") != NULL);
        initCategories();
        postAdminSizes(cmdNotify(cmd));
        *exitCode = ok ? 0 : 1;
        return TRUE;
    }

    // Clean and/or measure the admin folders, report the sizes to the window that started us, and exit.
    // That window waits for us, then rescans.
    if (wcsstr(cmd, L"--clean-admin=") || wcsstr(cmd, L"--measure-admin")) {
        initCategories();
        const wchar_t* c = wcsstr(cmd, L"--clean-admin=");
        if (c) runAdminClean((int)wcstol(c + 14, NULL, 10));
        postAdminSizes(cmdNotify(cmd));
        return TRUE;
    }

    // preview the Explorer-restart prompt without doing anything (appearance check only)
    if (wcsstr(cmd, L"--demo-restart-prompt")) {
        showDialog(NULL, L"Restart Explorer?", EXPLORER_PROMPT, NULL, 0, L"Restart Explorer", L"Not now", 0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show) {
    (void)hp;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitializeCriticalSection(&g_lock);
    g_inst = hi;
    g_elevated = isElevated();

    g_dpi = GetDpiForSystem();
    loadTheme();
    langLoad();
    if (cmd && wcsstr(cmd, L"--light")) g_dark = FALSE;
    if (cmd && wcsstr(cmd, L"--dark"))  g_dark = TRUE;
    g_accent = readAccent();
    makeFonts();

    // An elevated helper is allowed the foreground just after UAC, so it hands it to the window that started it.
    if (cmd && cmdNotify(cmd)) SetForegroundWindow(cmdNotify(cmd));

    int helperExit;
    if (cmd && runHelperCommand(cmd, &helperExit)) return helperExit;
    wcscpy(g_status, L"Starting\u2026");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   // the folder picker is a COM dialog
    usbRefresh();

    // resolve the fixed, safe category paths before any scanning
    initCategories();
    g_srOff = restoreIsOff();
    appsRefresh();
    gpuRefresh();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"WindowsChosenUtilities";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    RegisterClassW(&wc);

    int w = 760, h = 640;
    g_hwnd = CreateWindowExW(0, L"WindowsChosenUtilities", L"Windows Chosen Utilities",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL, hi, NULL);

    applyWindowTheme(g_hwnd);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    // ask GitHub whether there's a newer release, now and every 6 hours; after an update, remove the old exe
    suCheck();
    SetTimer(g_hwnd, TIMER_SUCHECK, SU_RECHECK_MS, NULL);
    HANDLE cleanup = CreateThread(NULL, 0, suCleanupProc, NULL, 0, NULL);
    if (cleanup) CloseHandle(cleanup);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
#else
#include "tests.c"
#endif
