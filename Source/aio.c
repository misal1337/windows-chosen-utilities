// Windows AIO page: the panes for each tool, what their buttons do, and handling a finished job.

// ---------- the panes ----------
// The list of images, with a line above each WIM's images, and its scroll bars
static void wimDrawList(int x1, int x2, int y, int listB) {
    int W = x2 - x1;
    LONG count = g_wimCount;
    RECT list = { x1, y, x2, listB };
    Col s = sepCol();
    Col lf = g_dark ? (Col){255,255,255,0.03} : (Col){255,255,255,0.45};
    fillRoundRect(list.left, list.top, list.right, list.bottom, dp(4), lf.r, lf.g, lf.b, lf.a);
    strokeRoundRect(list.left, list.top, list.right, list.bottom, dp(4), dp(1), s.r, s.g, s.b, s.a * 0.7);

    // Index, Name, Description, Edition ID, Arch, Build. Name and Description fit "Windows 11 Education"
    // (longer names shorten with an ellipsis), and Edition ID takes what's left. They only scroll
    // sideways if the window is made too narrow to hold them all.
    int viewW = W - dp(8), cw[6];
    cw[0] = textWidth(g_fSmall, L"Index") + dp(14);
    cw[1] = cw[2] = textWidth(g_fSmall, L"Windows 11 Education") + dp(14);
    cw[4] = textWidth(g_fSmall, L"ARM64") + dp(14);
    cw[5] = textWidth(g_fSmall, L"26100.1742") + dp(10);
    int total = dp(10) + cw[0] + cw[1] + cw[2] + cw[4] + cw[5];
    cw[3] = max(dp(90), viewW - total);
    total += cw[3];
    g_wimHMax = max(0, total - viewW);
    if (g_wimHS > g_wimHMax) g_wimHS = g_wimHMax;
    if (g_wimHS < 0) g_wimHS = 0;
    int cx[6];
    cx[0] = x1 + dp(10) - g_wimHS;
    for (int c = 1; c < 6; c++) cx[c] = cx[c - 1] + cw[c - 1];

    int barH = g_wimHMax ? dp(10) : 0;
    RECT clip = { x1 + dp(2), y, x2 - dp(8), listB - barH };
    g_textClip = clip; g_textClipOn = TRUE;
    static const wchar_t* hdr[6] = { L"Index", L"Name", L"Description", L"Edition ID", L"Arch", L"Build" };
    for (int c = 0; c < 6; c++) {
        RECT r = { cx[c], y, cx[c] + cw[c] - dp(6), y + dp(28) };
        drawText(g_fSmall, hdr[c], r, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    fillRect(x1 + dp(1), y + dp(28), x2 - dp(1), y + dp(28) + 1, s.r, s.g, s.b, s.a);

    // on-screen rows: a separator wherever the next image comes from a different WIM
    int nv = 0;
    for (LONG i = 0; i < count && nv < WIM_MAX * 2 - 1; i++) {
        if (i == 0 || _wcsicmp(g_wim[i].path, g_wim[i - 1].path)) g_wimRows[nv++] = (short)(-1 - i);
        g_wimRows[nv++] = (short)i;
    }
    g_wimRowsCount = nv;
    int ry = y + dp(29), rowH = dp(28), bodyB = listB - barH - dp(2);
    int visible = (bodyB - ry) / rowH;
    if (visible < 1) visible = 1;
    if (g_wimShowSel) {               // keep a moved image on screen
        g_wimShowSel = FALSE;
        for (int v = 0; v < nv; v++) if (g_wimRows[v] == g_wimSel) {
            if (v < g_wimTop) g_wimTop = v > 0 && g_wimRows[v - 1] < 0 ? v - 1 : v;
            if (v >= g_wimTop + visible) g_wimTop = v - visible + 1;
        }
    }
    if (g_wimTop > nv - visible) g_wimTop = nv - visible;
    if (g_wimTop < 0) g_wimTop = 0;
    g_wimRowsTop = ry; g_wimRowH = rowH;
    RECT lhit = { x1, ry, x2, bodyB };
    aioRegister(A_W_LIST, lhit, NULL, 0, FALSE);
    if (count == 0) {
        RECT r = { x1, ry, x2, bodyB };
        drawText(g_fSmall, L"Select WIMs to list their images. Each WIM you add appears under its own line.", r, mutedRGB(),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (int v = g_wimTop; v < nv && v < g_wimTop + visible; v++) {
        int t = ry + (v - g_wimTop) * rowH;
        if (g_wimRows[v] < 0) {       // separator: the WIM's path, then a line (these don't scroll sideways)
            const wchar_t* p = g_wim[-1 - g_wimRows[v]].path;
            wchar_t label[MAX_PATH + 16];
            swprintf(label, MAX_PATH + 16, L"%s", p);
            int mid = t + rowH / 2, tw = min(textWidth(g_fSmall, label), clip.right - clip.left - dp(40));
            RECT r = { x1 + dp(10), t, x1 + dp(10) + tw, t + rowH };
            drawText(g_fSmall, label, r, g_accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            fillAccentRect(r.right + dp(8), mid, x2 - dp(10), mid + 1, 0.45);
            continue;
        }
        int i = g_wimRows[v];
        WimEntry* e = &g_wim[i];
        double sv = anim(ANIM_WROW + (i % 16), i == g_wimSel);
        if (sv > 0) fillAccent(x1 + dp(3), t + dp(1), x2 - dp(9), t + rowH - dp(1), dp(3), 0.20 * sv);
        wchar_t idx[16]; swprintf(idx, 16, L"%d%s", e->index, e->dirty ? L" *" : L"");
        const wchar_t* val[6] = { idx, e->name, e->desc, e->edition, e->arch, e->build };
        for (int c = 0; c < 6; c++) {
            RECT r = { cx[c], t, cx[c] + cw[c] - dp(6), t + rowH };
            drawText(g_fSmall, val[c], r, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    g_textClipOn = FALSE;
    Col barCol = textCol();
    if (nv > visible) {               // vertical scroll indicator
        int trackT = ry + dp(2), trackH = bodyB - ry - dp(4);
        int th = max(dp(16), trackH * visible / nv);
        int ty = trackT + (trackH - th) * g_wimTop / max(1, nv - visible);
        fillRoundRect(x2 - dp(6), ty, x2 - dp(3), ty + th, dp(1), barCol.r, barCol.g, barCol.b, 0.35);
    }
    SetRectEmpty(&g_rcWimHBar);
    if (g_wimHMax) {                  // horizontal scroll bar: drag it, or use Shift+wheel
        RECT bar = { x1 + dp(4), listB - dp(10), x2 - dp(8), listB - dp(1) };
        g_rcWimHBar = bar;
        aioRegister(A_W_HBAR, bar, NULL, 0, FALSE);
        int barW = bar.right - bar.left;
        g_wimHThumbW = max(dp(24), barW * viewW / total);
        int tx = bar.left + (barW - g_wimHThumbW) * g_wimHS / g_wimHMax;
        double hv = anim(ANIM_AIO + A_W_HBAR, g_hotBtn == BTN_WIDGET + A_W_HBAR || g_wimHDrag);
        fillRoundRect(tx, bar.top + dp(3), tx + g_wimHThumbW, bar.bottom - dp(2), dp(2), barCol.r, barCol.g, barCol.b, 0.30 + 0.25 * hv);
    }
}

// Which build's setup files a saved folder or ISO uses: only one can be ticked
static void wimDrawBuildChoice(int x1, int x2, int y, BOOL idle, BOOL any) {
    if (g_mediaDirty) wimRefreshMedia();
    if (any) {
        RECT ml = { x1, y, x1 + dp(100), y + dp(28) };
        drawText(g_fSmall, L"Setup files from", ml, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        int mx = x1 + dp(104), mid = y + dp(14);
        if (g_mediaCount == 0) {
            RECT mr = { mx, y, x2, y + dp(28) };
            drawText(g_fSmall, L"These WIMs aren't in a setup folder. Save as folder or ISO will ask for one.",
                     mr, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        for (int m = 0; m < g_mediaCount; m++) {
            int tw = textWidth(g_fSmall, g_mediaOpts[m].build);
            RECT hit = { mx, y, mx + dp(26) + tw + dp(8), y + dp(28) };
            if (idle) aioRegister(A_W_MOPT0 + m, hit, NULL, 0, FALSE);
            double hv = anim(ANIM_AIO + A_W_MOPT0 + m, g_hotBtn == BTN_WIDGET + A_W_MOPT0 + m && idle);
            fillHover(hit.left - dp(3), hit.top + dp(2), hit.right, hit.bottom - dp(2), dp(4), hv);
            drawCheckbox(mx + dp(9), mid, dp(16), anim(ANIM_MOPT + m, m == g_mediaSel), idle);
            RECT tr = { mx + dp(24), y, hit.right, y + dp(28) };
            drawText(g_fSmall, g_mediaOpts[m].build, tr, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            mx = hit.right + dp(10);
        }
    }
}

static void paneWim(int x1, int x2, int top, int bottom) {
    BOOL idle = (g_busy == 0);
    int y = top, bh = dp(32), W = x2 - x1, gap = dp(6);
    BOOL any = g_wimCount > 0;
    LONG count = g_wimCount;
    BOOL hasSelNow = g_wimSel >= 0 && g_wimSel < count;
    // five buttons on one row
    int bw5 = (W - gap * 4) / 5;
    aioButton(A_W_SELECT, x1, y, x1 + bw5, y + bh, L"Select WIMs", FALSE, idle, FALSE);
    aioButton(A_W_DELETE, x1 + (bw5 + gap), y, x1 + (bw5 + gap) + bw5, y + bh, L"Delete", FALSE, idle && hasSelNow, FALSE);
    aioButton(A_W_UP, x1 + (bw5 + gap) * 2, y, x1 + (bw5 + gap) * 2 + bw5, y + bh, L"Move up", FALSE, idle && hasSelNow && g_wimSel > 0, FALSE);
    aioButton(A_W_DOWN, x1 + (bw5 + gap) * 3, y, x1 + (bw5 + gap) * 3 + bw5, y + bh, L"Move down", FALSE, idle && hasSelNow && g_wimSel < count - 1, FALSE);
    aioButton(A_W_RESET, x1 + (bw5 + gap) * 4, y, x2, y + bh, L"Reset", FALSE, idle && (any || g_wimDelCount), FALSE);
    y += bh + dp(10);

    int listB = bottom - dp(182);
    wimDrawList(x1, x2, y, listB);

    y = listB + dp(10);
    BOOL hasSel = g_wimSel >= 0 && g_wimSel < count;
    RECT nl = { x1, y, x1 + dp(78), y + dp(30) };
    drawText(g_fSmall, L"Name", nl, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT nf = { x1 + dp(82), y, x2, y + dp(30) };
    aioField(A_W_NAME, nf, g_fName, 128, !hasSel || !idle, L"Select an image to rename it");
    y += dp(36);
    RECT dl = { x1, y, x1 + dp(78), y + dp(30) };
    drawText(g_fSmall, L"Description", dl, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT df = { x1 + dp(82), y, x2, y + dp(30) };
    aioField(A_W_DESC, df, g_fDesc, 256, !hasSel || !idle, NULL);
    y += dp(36);

    wimDrawBuildChoice(x1, x2, y, idle, any);
    y += dp(32);
    aioStatus(x1, x2, y);

    // the four ways to save, along the bottom
    int sy = bottom - dp(32), sw = (W - gap * 3) / 4;
    aioButton(A_W_ASWIM, x1, sy, x1 + sw, bottom, L"Save as WIM", TRUE, idle && any, FALSE);
    aioButton(A_W_ASESD, x1 + (sw + gap), sy, x1 + (sw + gap) + sw, bottom, L"Save as ESD", FALSE, idle && any, !g_elevated);
    aioButton(A_W_ASFOLDER, x1 + (sw + gap) * 2, sy, x1 + (sw + gap) * 2 + sw, bottom, L"Save as folder", FALSE, idle && any, FALSE);
    aioButton(A_W_ASISO, x1 + (sw + gap) * 3, sy, x2, bottom, L"Save as ISO", FALSE, idle && any, FALSE);
}
static void paneEsd(int x1, int x2, int top, int bottom);   // esdiso.c
static void paneF2I(int x1, int x2, int top) {
    int y = top;
    y = aioPathRow(x1, x2, y, L"Source folder", A_F_SRC, g_fSrc, A_F_SRCB);
    y = aioPathRow(x1, x2, y, L"ISO to save", A_F_OUT, g_fOut, A_F_OUTB);
    RECT lr = { x1, y, x1 + dp(104), y + dp(32) };
    drawText(g_fSmall, L"ISO label", lr, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT fr = { x1 + dp(108), y, x1 + dp(340), y + dp(32) };
    aioField(A_F_LABEL, fr, g_fLabel, 33, FALSE, NULL);
    y += dp(42);
    aioNote(x1, x2, y, dp(36), L"The ISO is bootable when the folder holds Windows boot files "
                               L"(boot\\etfsboot.com for BIOS, efi\\microsoft\\boot\\efisys.bin for UEFI).");
    y += dp(46);
    aioButton(A_F_BUILD, x2 - dp(140), y, x2, y + dp(34), L"Build", TRUE, g_busy == 0 && g_fSrc[0] && g_fOut[0], FALSE);
    aioStatus(x1, x2 - dp(152), y);
}
static void paneI2F(int x1, int x2, int top) {
    int y = top;
    y = aioPathRow(x1, x2, y, L"Source ISO", A_I_SRC, g_iSrc, A_I_SRCB);
    y = aioPathRow(x1, x2, y, L"Extract to", A_I_DST, g_iDst, A_I_DSTB);
    y += dp(8);
    aioButton(A_I_GO, x2 - dp(140), y, x2, y + dp(34), L"Extract", TRUE, g_busy == 0 && g_iSrc[0] && g_iDst[0], FALSE);
    aioStatus(x1, x2 - dp(152), y);
}
static void aioListRow(int id, int x1, int x2, int y, const wchar_t* text, BOOL sel, int animIdx) {
    RECT rc = { x1, y, x2, y + dp(32) };
    aioRegister(id, rc, NULL, 0, FALSE);
    double hv = anim(ANIM_AIO + id, g_hotBtn == BTN_WIDGET + id);
    double sv = anim(ANIM_DROW + animIdx, sel);
    fillHover(rc.left, rc.top, rc.right, rc.bottom, dp(4), hv);
    if (sv > 0) fillAccent(rc.left, rc.top, rc.right, rc.bottom, dp(4), 0.20 * sv);
    RECT t = { x1 + dp(12), y, x2 - dp(8), y + dp(32) };
    drawText(g_fBody, text, t, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}
static void paneUsb(int x1, int x2, int top) {
    int y = top;
    y = aioPathRow(x1, x2, y, L"Setup folder", A_U_SRC, g_uSrc, A_U_SRCB);
    y += dp(4);
    RECT lr = { x1, y, x2 - dp(100), y + dp(32) };
    drawText(g_fBodyBold, L"USB drives", lr, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    aioButton(A_U_REFRESH, x2 - dp(88), y, x2, y + dp(32), L"Refresh", FALSE, g_busy == 0, FALSE);
    y += dp(38);
    if (g_usbCount == 0) {
        RECT r = { x1, y, x2, y + dp(32) };
        drawText(g_fSmall, L"Compatible devices not found. Plug in a USB drive and choose Refresh.", r, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += dp(36);
    } else for (int i = 0; i < g_usbCount; i++) {
        aioListRow(A_U_ROW0 + i, x1, x2, y, g_usb[i].label, i == g_usbSel, i);
        y += dp(34);
    }
    y += dp(6);
    aioNote(x1, x2, y, dp(36), L"Everything on the chosen USB drive is erased. The drive starts on both BIOS and UEFI PCs, "
                               L"and only 32 GB of it is used. Only USB drives that don't hold Windows are listed.");
    y += dp(46);
    aioButton(A_U_START, x2 - dp(140), y, x2, y + dp(34), L"Start", TRUE, g_busy == 0 && g_uSrc[0] && g_usbSel >= 0, !g_elevated);
    aioStatus(x1, x2 - dp(152), y);
}
static void paneHash(int x1, int x2, int top) {
    static const wchar_t* names[4] = { L"MD5", L"SHA-1", L"SHA-256", L"CRC-32" };
    int y = top;
    y = aioPathRow(x1, x2, y, L"File", A_H_FILE, g_hFile, A_H_FILEB);
    y += dp(4);
    for (int i = 0; i < 4; i++) {
        RECT lr = { x1, y, x1 + dp(104), y + dp(32) };
        drawText(g_fSmall, names[i], lr, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT fr = { x1 + dp(108), y, x2 - dp(86), y + dp(32) };
        aioField(A_H_V0 + i, fr, g_hashOut[i], 132, TRUE, NULL);
        aioButton(A_H_C0 + i, x2 - dp(78), y, x2, y + dp(32), L"Copy", FALSE, g_hashOut[i][0] != 0, FALSE);
        y += dp(38);
    }
    y += dp(8);
    aioButton(A_H_GO, x2 - dp(170), y, x2, y + dp(34), L"Get hash from file", TRUE, g_busy == 0 && g_hFile[0], FALSE);
    aioStatus(x1, x2 - dp(182), y);
}

static void drawAio(int cL, int cR) {
    int pad = dp(16), top = pad, bottom = g_bh - pad;
    Col sf = surfaceCol();

    // tool tabs
    int tabH = dp(36), n = AT_COUNT, W = cR - cL;
    fillRoundRect(cL, top, cR, top + tabH, dp(8), sf.r, sf.g, sf.b, sf.a);
    int target = (g_fadeAct == FADE_TAB) ? g_fadeArg : g_aioTab;
    for (int i = 0; i < n; i++) {
        int l = cL + W * i / n, r = cL + W * (i + 1) / n;
        RECT rc = { l, top, r, top + tabH };
        aioRegister(A_TAB0 + i, rc, NULL, 0, FALSE);
        double sv = anim(ANIM_TAB + i, i == target);
        double hv = anim(ANIM_AIO + A_TAB0 + i, g_hotBtn == BTN_WIDGET + A_TAB0 + i && i != target);
        fillHover(l + dp(3), top + dp(3), r - dp(3), top + tabH - dp(3), dp(4), hv);
        if (sv > 0) {
            Col h = hoverCol();
            fillRoundRect(l + dp(3), top + dp(3), r - dp(3), top + tabH - dp(3), dp(4), h.r, h.g, h.b, h.a * 2.4 * sv);
            int bw = (r - l) / 3, bx = (l + r - bw) / 2;
            fillAccent(bx, top + tabH - dp(5), bx + bw, top + tabH - dp(2), dp(1), sv);
        }
        drawText(g_fBody, AT_LABEL[i], rc, textRGB(), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // the pane fades when switching tools; the tabs themselves stay put
    int cardT = top + tabH + dp(8);
    fillRoundRect(cL, cardT, cR, bottom, dp(8), sf.r, sf.g, sf.b, sf.a);
    int x1 = cL + dp(16), x2 = cR - dp(16), y = cardT + dp(14);
    double save = g_alphaMul;
    g_alphaMul *= g_tabMul;
    switch (g_aioTab) {
    case AT_WIM:  paneWim(x1, x2, y, bottom - dp(14)); break;
    case AT_ESD:  paneEsd(x1, x2, y, bottom - dp(14)); break;
    case AT_F2I:  paneF2I(x1, x2, y); break;
    case AT_I2F:  paneI2F(x1, x2, y); break;
    case AT_USB:  paneUsb(x1, x2, y); break;
    case AT_HASH: paneHash(x1, x2, y); break;
    }
    g_alphaMul = save;
}


static const wchar_t* ISO_FILTER = L"ISO images (*.iso)\0*.iso\0";

// ---------- actions ----------
static void wimSelect(int i) {
    g_wimSel = i;
    if (g_focus == A_W_NAME || g_focus == A_W_DESC) focusNone();
    if (i >= 0 && i < g_wimCount) { wcscpy(g_fName, g_wim[i].name); wcscpy(g_fDesc, g_wim[i].desc); }
    else { g_fName[0] = 0; g_fDesc[0] = 0; }
}
static BOOL wimHasOtherFiles(void) {
    for (LONG i = 0; i < g_wimCount; i++) if (_wcsicmp(g_wim[i].path, g_wimFirst)) return TRUE;
    return FALSE;
}
static BOOL wimHasEdits(void) {
    if (g_wimDelCount || g_wimOrderChanged || wimHasOtherFiles()) return TRUE;
    for (LONG i = 0; i < g_wimCount; i++) if (g_wim[i].dirty) return TRUE;
    return FALSE;
}
static void wimClear(void) {
    g_wimCount = 0; g_wimDelCount = 0; g_wimTop = 0; g_wimHS = 0;
    g_wimOrderChanged = FALSE; g_wimFirst[0] = 0;
    g_mediaDirty = TRUE;
    wimSelect(-1);
}

// Adds to the list: each call appends, and the first file ever added is where Save writes.
static void actionWimSelect(void) {
    static wchar_t buf[8192]; buf[0] = 0; buf[1] = 0;
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = L"Windows images (*.wim, *.esd)\0*.wim;*.esd\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = 8192; ofn.lpstrTitle = L"Select WIMs";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    // single file: "path\0\0"; several: "dir\0a\0b\0\0". Normalised to a list of full paths.
    wchar_t* w = g_wimLoadList; size_t room = 8192;
    const wchar_t* first = buf; const wchar_t* next = buf + wcslen(buf) + 1;
    if (!*next) { wcscpy(w, first); w += wcslen(w) + 1; }
    else for (; *next; next += wcslen(next) + 1) {
        int n = swprintf(w, room, L"%s\\%s", first, next);
        if (n < 0) break;
        w += n + 1; room -= n + 1;
    }
    *w = 0;
    aioStartJob(JOB_WIMLOAD, NULL, FALSE);
}
static BOOL folderHasFiles(const wchar_t* dir);
// Save as WIM: choose where, starting at the first WIM. A new location leaves every source file as
// it was; an existing file gets Windows' own "replace?" prompt. If only names changed and the
// target is that WIM, they're written straight in, which is much faster.
static void actionSaveWim(void) {
    BOOL isEsd = _wcsicmp(PathFindExtensionW(g_wimFirst), L".wim") != 0;
    BOOL rebuild = isEsd || g_wimDelCount || g_wimOrderChanged || wimHasOtherFiles();
    lstrcpynW(g_jA, g_wimFirst, MAX_PATH);
    if (isEsd) PathRenameExtensionW(g_jA, L".wim");
    if (!pickSave(g_jA, L"Windows image (*.wim)\0*.wim\0", L"wim", L"Save as WIM")) { g_jA[0] = 0; return; }
    if (_wcsicmp(PathFindExtensionW(g_jA), L".wim")) PathRenameExtensionW(g_jA, L".wim");
    if (!rebuild && !_wcsicmp(g_jA, g_wimFirst)) g_jA[0] = 0;
    aioStartJob(JOB_WIMSAVE, NULL, g_jA[0] != 0);
}
static void actionSaveEsd(void) {
    lstrcpynW(g_jA, g_wimFirst, MAX_PATH);
    PathRenameExtensionW(g_jA, L".esd");
    if (!pickSave(g_jA, L"Compressed Windows image (*.esd)\0*.esd\0", L"esd", L"Save as ESD")) return;
    if (_wcsicmp(PathFindExtensionW(g_jA), L".esd")) PathRenameExtensionW(g_jA, L".esd");
    swprintf(g_jC, MAX_PATH, L"%s.build.wim", g_jA);
    aioStartJob(JOB_ESD_BUILD, NULL, TRUE);
}
// The ticked build's setup folder. Asks for a folder only if no listed WIM is in one.
static wchar_t g_mediaNote[64];
static BOOL pickMediaRoot(wchar_t* root) {
    if (g_mediaDirty) wimRefreshMedia();
    if (g_mediaSel < g_mediaCount) {
        wcscpy(root, g_mediaOpts[g_mediaSel].root);
        swprintf(g_mediaNote, 64, L"Setup files are from build %s.", g_mediaOpts[g_mediaSel].build);
        return TRUE;
    }
    if (!showDialog(g_hwnd, L"Choose a Windows setup folder",
                    L"None of these WIMs is inside a Windows setup folder (…\\sources\\install.wim), so there are no "
                    L"setup files to use. Choose an extracted setup folder. ISO2Folder can make one from an ISO.",
                    NULL, 0, L"Choose folder", L"Cancel", 0)) return FALSE;
    if (!pickFolder(root)) return FALSE;
    trimSlash(root);
    if (!mediaValid(root)) {
        okDialog(L"Not a setup folder", L"That folder doesn't have sources\\boot.wim and Windows boot files.");
        return FALSE;
    }
    wcscpy(g_mediaNote, L"Setup files are from the folder you chose.");
    return TRUE;
}
static void actionSaveFolder(void) {
    wchar_t root[MAX_PATH];
    if (!pickMediaRoot(root)) return;
    g_jA[0] = 0;
    if (!pickFolder(g_jA)) return;
    trimSlash(g_jA);
    if (folderHasFiles(g_jA)) {
        okDialog(L"Choose an empty folder", L"The setup files need an empty folder. In the folder picker you can create a new one.");
        return;
    }
    size_t rl = wcslen(root);
    if (!_wcsnicmp(g_jA, root, rl) && (g_jA[rl] == 0 || g_jA[rl] == L'\\')) {
        okDialog(L"Choose another folder", L"The new folder can't be inside the setup folder it copies from.");
        return;
    }
    wcscpy(g_jB, root);
    aioStartJob(JOB_FOLDER, NULL, TRUE);
}
static void actionSaveIso(void) {
    wchar_t root[MAX_PATH];
    if (!pickMediaRoot(root)) return;
    if (!mediaFileExists(root, L"efi\\microsoft\\boot\\efisys.bin")) {
        wchar_t msg[MAX_PATH + 96];
        swprintf(msg, MAX_PATH + 96, L"%s has no efi\\microsoft\\boot\\efisys.bin, so an ISO made from it couldn't start on a UEFI PC.", root);
        okDialog(L"No UEFI boot file", msg);
        return;
    }
    wcscpy(g_jA, L"Windows.iso");
    if (!pickSave(g_jA, ISO_FILTER, L"iso", L"Save as ISO")) return;
    wcscpy(g_jB, root);
    aioStartJob(JOB_ISO, NULL, TRUE);
}
static void actionWimMove(int dir) {
    int a = g_wimSel, b = a + dir;
    if (a < 0 || b < 0 || b >= g_wimCount) return;
    WimEntry t = g_wim[a]; g_wim[a] = g_wim[b]; g_wim[b] = t;
    g_wimSel = b;
    g_wimOrderChanged = TRUE;
    g_wimShowSel = TRUE;
    swprintf(g_aioMsg[AT_WIM], MAX_PATH + 96, L"Order changed. Save to apply it.");
}
static void actionWimDelete(void) {
    if (g_wimSel < 0 || g_wimSel >= g_wimCount) return;
    if (g_wimCount == 1) {
        okDialog(L"Can't delete the last image", L"A WIM has to keep at least one image.");
        return;
    }
    WimEntry* e = &g_wim[g_wimSel];
    if (g_wimDelCount < WIM_MAX) {
        wcscpy(g_wimDel[g_wimDelCount].path, e->path);
        g_wimDel[g_wimDelCount].index = e->index;
        g_wimDelCount++;
    }
    int at = g_wimSel;
    for (LONG i = at; i < g_wimCount - 1; i++) g_wim[i] = g_wim[i + 1];
    g_wimCount--;
    g_mediaDirty = TRUE;
    wimSelect(at < g_wimCount ? at : (int)g_wimCount - 1);
    swprintf(g_aioMsg[AT_WIM], MAX_PATH + 96, L"Image removed from the list. Save to apply it.");
}
static void actionWimReset(void) {
    if (wimHasEdits() && !showDialog(g_hwnd, L"Discard unsaved changes?", L"The list is cleared and your edits are lost. Files on disk aren't changed.", NULL, 0, L"Discard", L"Cancel", 1)) return;
    wimClear();
    g_aioMsg[AT_WIM][0] = 0;
}
static BOOL folderHasFiles(const wchar_t* dir) {
    wchar_t pat[MAX_PATH + 4]; swprintf(pat, MAX_PATH + 4, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL any = FALSE;
    do { if (!isDotName(fd.cFileName)) { any = TRUE; break; } } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

static void actionF2I(void) {
    if (!isFolder(g_fSrc)) { okDialog(L"Choose a source folder", L"The source folder doesn't exist."); return; }
    wcscpy(g_jA, g_fSrc); wcscpy(g_jB, g_fOut); wcscpy(g_jC, g_fLabel);
    aioStartJob(JOB_F2I, NULL, TRUE);
}
static void actionI2F(void) {
    if (!isFile(g_iSrc)) { okDialog(L"Choose an ISO", L"The ISO wasn't found."); return; }
    if (folderHasFiles(g_iDst) && !showDialog(g_hwnd, L"Folder isn't empty", L"Files with the same names will be replaced.", NULL, 0, L"Extract", L"Cancel", 1)) return;
    wcscpy(g_jA, g_iSrc); wcscpy(g_jB, g_iDst);
    aioStartJob(JOB_I2F, NULL, TRUE);
}
static void actionUsb(void) {
    if (!isFolder(g_uSrc)) { okDialog(L"Choose a setup folder", L"The setup folder doesn't exist."); return; }
    if (g_usbSel < 0 || g_usbSel >= g_usbCount) return;
    if (wcschr(g_uSrc, L'"')) return;
    UsbDisk* u = &g_usb[g_usbSel];
    if (!usbDiskSafe(u->number, NULL)) { usbRefresh(); okDialog(L"Drive not available", L"That drive isn't available any more. Check the list and try again."); return; }
    // UAC can't name the drive, and this can't be undone, so this confirmation stays even with UAC
    wchar_t msg[256];
    swprintf(msg, 256, L"Everything on %s will be permanently erased.", u->label);
    if (!showDialog(g_hwnd, L"Erase this USB drive?", msg, NULL, 0, L"Erase and continue", L"Cancel", 1)) return;
    wchar_t args[MAX_PATH + 64];
    swprintf(args, MAX_PATH + 64, L"--usb-write=\"%s\" --disk=%d", g_uSrc, u->number);
    if (g_elevated) {
        // same helper either way, so the worker has one path and the safety check always repeats
        HANDLE direct = startHelperDirect(args);
        if (!direct) { okDialog(L"Couldn't start", L"The USB helper didn't start, and the drive wasn't changed."); return; }
        aioStartJob(JOB_USB, direct, FALSE);
        return;
    }
    HANDLE proc = NULL; BOOL cancelled = FALSE;
    if (startElevatedHelper(args, &proc, &cancelled)) aioStartJob(JOB_USB, proc, FALSE);
    else {
        wcscpy(g_aioMsg[AT_USB], cancelled ? L"Cancelled \u2014 the drive wasn't changed." : L"Couldn't start the administrator helper \u2014 the drive wasn't changed.");
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}
static void actionHash(void) {
    if (!isFile(g_hFile)) { okDialog(L"Choose a file", L"The file wasn't found."); return; }
    for (int i = 0; i < 4; i++) g_hashOut[i][0] = 0;
    wcscpy(g_jA, g_hFile);
    aioStartJob(JOB_HASH, NULL, TRUE);
}
static void aioOnProgress(int pct, int stage) {
    g_aioPct = (pct > 100) ? 100 : pct;
    if (stage >= 0 && stage < (int)(sizeof(ST_TEXT) / sizeof(ST_TEXT[0]))) g_aioStage = stage;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void aioOnDone(DWORD code) {
    int job = g_aioJob, tab = g_aioJobTab;
    InterlockedExchange(&g_busy, 0);
    g_aioRunning = FALSE; g_aioJob = JOB_NONE;
    wchar_t* m = g_aioMsg[tab]; int cch = MAX_PATH + 96;
    if (job == JOB_ESD_DISM) DeleteFileW(g_jC);       // the intermediate .wim, finished with either way
    if (code == ERROR_CANCELLED) {
        swprintf(m, cch, L"Stopped. Nothing further was written.");
    } else if (code) {
        wchar_t err[512]; aioErrorText(code, err, 512);
        swprintf(m, cch, L"That didn't finish (error 0x%08lX).", code);
        InvalidateRect(g_hwnd, NULL, FALSE);
        okDialog(L"That didn't finish", err);
    } else switch (job) {
    case JOB_WIMLOAD:
        if (g_wimJustSaved) { swprintf(m, cch, L"Saved %ld images to the file listed above.", (long)g_wimCount); g_wimJustSaved = FALSE; }
        else swprintf(m, cch, L"%ld image%s listed.", (long)g_wimCount, g_wimCount == 1 ? L"" : L"s");
        if (g_wimCount && g_wimSel < 0) wimSelect(0);
        g_mediaDirty = TRUE;
        break;
    case JOB_ESD_BUILD: {                 // the images are ready: DISM compresses them, with administrator rights
        wchar_t args[MAX_PATH * 2 + 64];
        swprintf(args, MAX_PATH * 2 + 64, L"--esd-src=\"%s\" --esd-dst=\"%s\" --count=%ld", g_jC, g_jA, (long)g_wimCount);
        HANDLE proc = NULL; BOOL cancelled = FALSE, started = FALSE;
        if (g_elevated) {
            proc = startHelperDirect(args);
            started = (proc != NULL);
        } else {
            started = startElevatedHelper(args, &proc, &cancelled);
        }
        if (!started) {
            DeleteFileW(g_jC);
            wcscpy(m, cancelled ? L"Cancelled — no ESD was made." : L"Couldn't start the administrator helper — no ESD was made.");
            break;
        }
        aioStartJob(JOB_ESD_DISM, proc, FALSE);
        return;
    }
    case JOB_FOLDER: swprintf(m, cch, L"Created the setup folder. %s", g_mediaNote); break;
    case JOB_ISO:    swprintf(m, cch, L"Created %s. %s", PathFindFileNameW(g_jA), g_mediaNote); break;
    case JOB_ESD_DISM:
    case JOB_WIMSAVE: {
        // show what the file really holds now
        const wchar_t* target = g_jA[0] ? g_jA : g_wimFirst;
        wcscpy(g_wimLoadList, target); g_wimLoadList[wcslen(target) + 1] = 0;
        wimClear();
        g_wimJustSaved = TRUE;
        aioStartJob(JOB_WIMLOAD, NULL, FALSE);
        return;
    }
    case JOB_ESDLOAD:
        if (g_esdSkipped)
            swprintf(m, cch, L"%ld edition%s in this ESD. Untick any you don't want in the ISO. Its %d setup image%s (boot files, not editions) %s left out.",
                     (long)g_esdN, g_esdN == 1 ? L"" : L"s", g_esdSkipped, g_esdSkipped == 1 ? L"" : L"s", g_esdSkipped == 1 ? L"is" : L"are");
        else swprintf(m, cch, L"%ld edition%s in this ESD. Untick any you don't want in the ISO.", (long)g_esdN, g_esdN == 1 ? L"" : L"s");
        break;
    case JOB_F2I:  swprintf(m, cch, L"Created %s.", g_jB); break;
    case JOB_I2F:  swprintf(m, cch, L"Extracted to %s.", g_jB); break;
    case JOB_USB:  swprintf(m, cch, L"The USB drive is ready."); break;
    case JOB_HASH:
        for (int i = 0; i < 4; i++) wcscpy(g_hashOut[i], g_jobHash[i]);
        swprintf(m, cch, L"Hashes calculated for %s.", PathFindFileNameW(g_jA));
        break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}
