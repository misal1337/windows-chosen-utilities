// ---------- Windows 11 style dialog (WinUI ContentDialog layout, net-merge acrylic base) ----------
// Borderless acrylic window: title, wrapped body, optional name/size rows, then a footer band with
// two equal buttons (primary on the left). Enter activates the focused button, Esc cancels.
typedef struct {
    HDC mem, scr; HBITMAP memBmp, scrBmp; void* bits; void* scrBits; int w, h;
} Surface;

typedef struct {
    const wchar_t* title; const wchar_t* body; const DlgRow* rows; int nrows;
    const wchar_t* primary; const wchar_t* secondary;
    int focus, hot, press, result;
    BOOL done, keyboard;
    double fade;                      // contents fade in on open and out on close
    RECT btn[2];
    Surface sf;
    const DlgExtra* x;
    int caret, selA;                  // text input
    wchar_t error[200];
    RECT field;
    int itemTop, itemsShown;          // list
    RECT list;
    RECT selAll, selNone;             // checklist only
    RECT bar;                         // the list's scrollbar, when it has more rows than it shows
    int drag;                         // 0 none, 1 dragging the scrollbar thumb, 2 dragging the list itself
    int dragY, dragTop;               // where the drag started, and the first row shown then
    BOOL dragMoved;                   // a press on a row that moved scrolls instead of ticking
} DlgState;

#define DLG_CHECK_ROWS 12             // a checklist shows this many rows and scrolls the rest

#define DLG_WIDTH   460
#define DLG_PAD     24
#define DLG_ROW     30
#define DLG_FOOTER  80
#define DLG_FIELD   36
#define DLG_ERROR   44

// the drawing helpers target the global backbuffer, so point them at this dialog while it paints
static void swapSurface(Surface* s) {
    Surface t = { g_memDC, g_scratchDC, g_memBmp, g_scratchBmp, g_bits, g_scratchBits, g_bw, g_bh };
    g_memDC = s->mem; g_scratchDC = s->scr; g_memBmp = s->memBmp; g_scratchBmp = s->scrBmp;
    g_bits = s->bits; g_scratchBits = s->scrBits; g_bw = s->w; g_bh = s->h;
    *s = t;
}

static int measureWrapped(WFont font, const wchar_t* s, int width) {
    if (!s || !*s) return 0;
    IDWriteTextLayout* l = textLayout(font, s, -1, (float)width, 100000.0f);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m; memset(&m, 0, sizeof(m));
    l->lpVtbl->GetMetrics(l, &m);
    comRelease(l);
    return (int)ceil(m.height);
}

// Room the text input or list takes, for a list showing all its items.
static int dlgExtraHeight(const DlgExtra* x) {
    if (!x) return 0;
    if (x->input) return dp(14) + dp(DLG_FIELD) + dp(DLG_ERROR);
    if (x->checks) return dp(14) + dp(DLG_ROW) + dp(DLG_ROW) * min(x->nItems, DLG_CHECK_ROWS) + dp(8);
    return x->nItems ? dp(14) + dp(DLG_ROW) * x->nItems + dp(8) : 0;
}

// Select all and Select none, above a checklist
static void dlgPaintSelectRow(DlgState* d, int left, int y) {
    int n = d->x->nItems, on = 0;
    for (int i = 0; i < n; i++) if (d->x->checks[i]) on++;
    int w1 = textWidth(g_fSmall, L"Select all") + dp(16), w2 = textWidth(g_fSmall, L"Select none") + dp(16);
    d->selAll = (RECT){ left, y, left + w1, y + dp(DLG_ROW) - dp(4) };
    d->selNone = (RECT){ left + w1 + dp(6), y, left + w1 + dp(6) + w2, y + dp(DLG_ROW) - dp(4) };
    Col f = buttonFillCol(), sep = sepCol();
    RECT* rs[2] = { &d->selAll, &d->selNone };
    const wchar_t* ts[2] = { L"Select all", L"Select none" };
    for (int i = 0; i < 2; i++) {
        RECT b = *rs[i];
        fillRoundRect(b.left, b.top, b.right, b.bottom, dp(4), f.r, f.g, f.b, f.a);
        strokeRoundRect(b.left, b.top, b.right, b.bottom, dp(4), dp(1), sep.r, sep.g, sep.b, sep.a * (g_dark ? 1.0 : 1.6));
        drawText(g_fSmall, ts[i], b, textRGB(), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    wchar_t count[48]; swprintf(count, 48, L"%d of %d", on, n);
    RECT c = { d->selNone.right + dp(8), y, d->selNone.right + dp(200), y + dp(DLG_ROW) - dp(4) };
    drawText(g_fSmall, count, c, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void dlgPaintInput(DlgState* d, int left, int right, int y) {
    RECT rc = { left, y, right, y + dp(DLG_FIELD) };
    d->field = rc;
    Col f = g_dark ? (Col){ 255,255,255,0.08 } : (Col){ 255,255,255,0.85 }, s = sepCol();
    fillRoundRect(rc.left, rc.top, rc.right, rc.bottom, dp(4), f.r, f.g, f.b, f.a);
    strokeRoundRect(rc.left, rc.top, rc.right, rc.bottom, dp(4), dp(1), s.r, s.g, s.b, s.a * (g_dark ? 1.0 : 1.6));
    fillAccentRect(rc.left + dp(2), rc.bottom - dp(2), rc.right - dp(2), rc.bottom, 1.0);

    RECT in = { rc.left + dp(10), rc.top, rc.right - dp(10), rc.bottom };
    const wchar_t* t = d->x->input;
    if (d->selA != d->caret) {
        int a = min(d->selA, d->caret), b = max(d->selA, d->caret);
        fillAccentRect(in.left + textWidthN(g_fBody, t, a), rc.top + dp(7), in.left + textWidthN(g_fBody, t, b), rc.bottom - dp(7), 0.45);
    }
    drawText(g_fBody, t, in, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    int cx = in.left + textWidthN(g_fBody, t, d->caret);
    Col c = textCol();
    fillRect(cx, rc.top + dp(8), cx + max(1, dp(1)), rc.bottom - dp(8), c.r, c.g, c.b, c.a);

    if (d->error[0]) {
        RECT re = { left, rc.bottom + dp(6), right, rc.bottom + dp(DLG_ERROR) };
        drawText(g_fSmall, d->error, re, g_dark ? RGB(255, 153, 164) : RGB(196, 43, 28), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
}

static void dlgPaintList(DlgState* d, int left, int right, int y, int bottom) {
    int rowH = dp(DLG_ROW), n = d->x->nItems;
    d->itemsShown = (bottom - y - dp(8)) / rowH;
    if (d->itemsShown < 1) d->itemsShown = 1;
    if (d->itemsShown > n) d->itemsShown = n;
    if (d->itemTop > n - d->itemsShown) d->itemTop = n - d->itemsShown;
    if (d->itemTop < 0) d->itemTop = 0;
    int pick = *d->x->pick;
    RECT rc = { left, y, right, y + rowH * d->itemsShown + dp(8) };
    d->list = rc;
    Col s = g_dark ? (Col){ 255,255,255, 0.05 } : (Col){ 255,255,255, 0.60 };
    fillRoundRect(rc.left, rc.top, rc.right, rc.bottom, dp(4), s.r, s.g, s.b, s.a);
    for (int i = 0; i < d->itemsShown; i++) {
        int idx = d->itemTop + i, ry = rc.top + dp(4) + i * rowH;
        if (idx == pick) {
            Col h = hoverCol();
            fillRoundRect(rc.left + dp(4), ry + dp(1), rc.right - dp(4), ry + rowH - dp(1), dp(4), h.r, h.g, h.b, h.a * 2.4);
            fillAccent(rc.left + dp(4), ry + dp(7), rc.left + dp(7), ry + rowH - dp(7), dp(1.5), 1.0);
        }
        int tl = rc.left + dp(14);
        if (d->x->checks) {
            drawCheckbox(rc.left + dp(24), ry + rowH / 2, dp(16), d->x->checks[idx] ? 1.0 : 0.0, TRUE);
            tl = rc.left + dp(42);
        }
        RECT t = { tl, ry, rc.right - dp(12), ry + rowH };
        drawText(g_fSmall, d->x->items[idx], t, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SetRectEmpty(&d->bar);
    if (d->itemsShown < n) {               // where the visible part is in the whole list; it can be dragged
        int track = rc.bottom - rc.top - dp(8), th = max(dp(24), track * d->itemsShown / n);
        int ty = rc.top + dp(4) + (track - th) * d->itemTop / (n - d->itemsShown);
        d->bar = (RECT){ rc.right - dp(14), rc.top, rc.right, rc.bottom };
        Col c = textCol();
        BOOL wide = d->drag == 1;
        fillRoundRect(rc.right - dp(wide ? 8 : 6), ty, rc.right - dp(2), ty + th, dp(2), c.r, c.g, c.b, wide ? 0.6 : 0.4);
    }
}

// The first row shown for a scrollbar thumb whose top is at y
static int dlgTopForBar(DlgState* d, int y) {
    int n = d->x->nItems, shown = d->itemsShown;
    if (shown >= n) return 0;
    int track = d->list.bottom - d->list.top - dp(8), th = max(dp(24), track * shown / n);
    int span = track - th;
    if (span <= 0) return 0;
    int top = (int)((long long)(y - th / 2 - d->list.top - dp(4)) * (n - shown) / span + 0.5);   // the thumb centred on y
    return max(0, min(n - shown, top));
}

static void dlgPaint(HWND h, DlgState* d, HDC hdc) {
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom, pad = dp(DLG_PAD);
    swapSurface(&d->sf);
    resizeBackbuffer(W, H);
    memset(g_bits, 0, (size_t)g_bw * g_bh * 4);
    g_needFrame = FALSE;
    g_alphaMul = d->fade;

    if (g_dark) fillRect(0, 0, W, H, 32, 32, 32, 0.745);
    else        fillRect(0, 0, W, H, 243, 243, 243, 0.825);

    int footTop = H - dp(DLG_FOOTER);
    int y = pad;
    RECT rt = { pad, y, W - pad, y + dp(28) };
    drawText(g_fTitle, d->title, rt, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    y += dp(28) + dp(12);

    int inner = W - pad * 2;
    int bodyH = measureWrapped(g_fBody, d->body, inner);
    int rowsH = d->nrows ? dp(DLG_ROW) * d->nrows + dp(8) : 0;
    int maxBody = footTop - y - (rowsH ? rowsH + dp(16) : 0) - dp(16);
    if (d->x && d->x->input) maxBody -= dlgExtraHeight(d->x);
    else if (d->x && d->x->nItems) maxBody -= dp(14) + dp(DLG_ROW) * (d->x->checks ? 2 : 1) + dp(8);
    if (bodyH > maxBody) bodyH = maxBody;
    RECT rb = { pad, y, W - pad, y + bodyH };
    drawText(g_fBody, d->body, rb, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    y += bodyH;

    if (d->nrows) {
        y += dp(16);
        Col s = g_dark ? (Col){ 255,255,255, 0.05 } : (Col){ 255,255,255, 0.60 };
        fillRoundRect(pad, y, W - pad, y + rowsH, dp(4), s.r, s.g, s.b, s.a);
        int ry = y + dp(4);
        for (int i = 0; i < d->nrows; i++, ry += dp(DLG_ROW)) {
            RECT l = { pad + dp(12), ry, W - pad - dp(112), ry + dp(DLG_ROW) };
            RECT r = { W - pad - dp(110), ry, W - pad - dp(12), ry + dp(DLG_ROW) };
            drawText(g_fBody, d->rows[i].left, l, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            drawText(g_fBody, d->rows[i].right, r, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    if (d->x && d->x->input) dlgPaintInput(d, pad, W - pad, y + dp(14));
    else if (d->x && d->x->checks) {
        dlgPaintSelectRow(d, pad, y + dp(14));
        dlgPaintList(d, pad, W - pad, y + dp(14) + dp(DLG_ROW), footTop - dp(8));
    }
    else if (d->x && d->x->nItems) dlgPaintList(d, pad, W - pad, y + dp(14), footTop - dp(8));

    // footer band and buttons
    if (g_dark) fillRect(0, footTop, W, H, 0, 0, 0, 0.14);
    else        fillRect(0, footTop, W, H, 0, 0, 0, 0.03);
    Col sep = sepCol();
    fillRect(0, footTop, W, footTop + 1, sep.r, sep.g, sep.b, sep.a);

    int gap = dp(8), bh = dp(32), bw = (inner - gap) / 2, by = footTop + (dp(DLG_FOOTER) - bh) / 2;
    if (d->secondary) {
        d->btn[0] = (RECT){ pad, by, pad + bw, by + bh };
        d->btn[1] = (RECT){ W - pad - bw, by, W - pad, by + bh };
    } else {
        d->btn[0] = (RECT){ W - pad - bw, by, W - pad, by + bh };
        SetRectEmpty(&d->btn[1]);
    }
    for (int i = 0; i < (d->secondary ? 2 : 1); i++) {
        RECT b = d->btn[i];
        BOOL pressed = (d->press == i && d->hot == i);
        double bv = anim(ANIM_DLG + i, d->hot == i);
        if (i == 0) {
            fillAccent(b.left, b.top, b.right, b.bottom, dp(4), 1.0);
            if (bv > 0) fillRoundRect(b.left, b.top, b.right, b.bottom, dp(4), 255, 255, 255, (pressed ? 0.02 : 0.10) * bv);
        } else {
            Col f = buttonFillCol();
            fillRoundRect(b.left, b.top, b.right, b.bottom, dp(4), f.r, f.g, f.b, f.a);
            fillHover(b.left, b.top, b.right, b.bottom, dp(4), (pressed ? 0.8 : 2) * bv);
            strokeRoundRect(b.left, b.top, b.right, b.bottom, dp(4), dp(1), sep.r, sep.g, sep.b, sep.a * (g_dark ? 1.0 : 1.6));
        }
        if (d->keyboard && d->focus == i) {
            Col t = textCol();
            strokeRoundRect(b.left - dp(2), b.top - dp(2), b.right + dp(2), b.bottom + dp(2), dp(6), dp(2), t.r, t.g, t.b, t.a);
        }
        drawText(g_fBody, i == 0 ? d->primary : d->secondary, b, i == 0 ? RGB(255,255,255) : textRGB(),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    g_alphaMul = 1.0;
    BitBlt(hdc, 0, 0, W, H, g_memDC, 0, 0, SRCCOPY);
    swapSurface(&d->sf);
    if (g_needFrame) SetTimer(h, 1, 16, NULL); else KillTimer(h, 1);
}

static int dlgHitButton(DlgState* d, int x, int y) {
    POINT p = { x, y };
    if (PtInRect(&d->btn[0], p)) return 0;
    if (d->secondary && PtInRect(&d->btn[1], p)) return 1;
    return -1;
}

static void dlgFinish(HWND h, DlgState* d, int result) {
    if (result && d->x && d->x->check && !d->x->check(d->x->input, d->error, 200)) {
        d->selA = 0; d->caret = (int)wcslen(d->x->input);      // ready to be typed over
        InvalidateRect(h, NULL, FALSE);
        return;
    }
    d->result = result; d->done = TRUE;
}

// Replaces the selection with n characters of s.
static void dlgInsert(HWND h, DlgState* d, const wchar_t* s, int n) {
    wchar_t* t = d->x->input;
    int a = min(d->selA, d->caret), b = max(d->selA, d->caret), len = (int)wcslen(t);
    if (n > d->x->inputCch - 1 - (len - (b - a))) n = d->x->inputCch - 1 - (len - (b - a));
    if (n < 0) n = 0;
    memmove(t + a + n, t + b, (len - b + 1) * sizeof(wchar_t));
    memcpy(t + a, s, n * sizeof(wchar_t));
    d->caret = d->selA = a + n;
    d->error[0] = 0;
    InvalidateRect(h, NULL, FALSE);
}

static void dlgPaste(HWND h, DlgState* d) {
    if (!OpenClipboard(h)) return;
    HANDLE c = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* p = c ? (const wchar_t*)GlobalLock(c) : NULL;
    if (p) {
        int n = 0;
        while (p[n] && p[n] != L'\r' && p[n] != L'\n') n++;     // one line
        dlgInsert(h, d, p, n);
        GlobalUnlock(c);
    }
    CloseClipboard();
}

// TRUE if the text input used this key.
static BOOL dlgEditKey(HWND h, DlgState* d, WPARAM key) {
    int len = (int)wcslen(d->x->input);
    BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    int to = d->caret;
    switch (key) {
    case VK_LEFT:  to = d->selA != d->caret && !shift ? min(d->selA, d->caret) : max(0, d->caret - 1); break;
    case VK_RIGHT: to = d->selA != d->caret && !shift ? max(d->selA, d->caret) : min(len, d->caret + 1); break;
    case VK_HOME:  to = 0; break;
    case VK_END:   to = len; break;
    case VK_BACK:
    case VK_DELETE:
        if (d->selA == d->caret) {
            if (key == VK_BACK && d->caret > 0) d->selA = d->caret - 1;
            else if (key == VK_DELETE && d->caret < len) d->selA = d->caret + 1;
            else return TRUE;
        }
        dlgInsert(h, d, L"", 0);
        return TRUE;
    case 'A': if (!ctrl) return FALSE; d->selA = 0; d->caret = len; InvalidateRect(h, NULL, FALSE); return TRUE;
    case 'V': if (!ctrl) return FALSE; dlgPaste(h, d); return TRUE;
    default: return FALSE;
    }
    d->caret = to;
    if (!shift) d->selA = to;
    InvalidateRect(h, NULL, FALSE);
    return TRUE;
}

static int dlgItemAt(DlgState* d, int x, int y) {
    POINT p = { x, y };
    if (!d->x || !d->x->nItems || !PtInRect(&d->list, p)) return -1;
    int i = (y - d->list.top - dp(4)) / dp(DLG_ROW);
    if (i < 0 || i >= d->itemsShown) return -1;
    return d->itemTop + i;
}

static void dlgMovePick(HWND h, DlgState* d, int to) {
    if (to < 0) to = 0;
    if (to >= d->x->nItems) to = d->x->nItems - 1;
    *d->x->pick = to;
    if (to < d->itemTop) d->itemTop = to;
    else if (d->itemsShown && to >= d->itemTop + d->itemsShown) d->itemTop = to - d->itemsShown + 1;
    InvalidateRect(h, NULL, FALSE);
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    DlgState* d = (DlgState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        break;
    case WM_NCCALCSIZE:
        if (w) return 0;              // no title bar: the client area fills the rounded frame
        break;
    case WM_NCHITTEST: {              // drag the dialog anywhere except on its buttons
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &p);
        if (d && (PtInRect(&d->field, p) || PtInRect(&d->list, p) || PtInRect(&d->selAll, p) || PtInRect(&d->selNone, p))) return HTCLIENT;
        return (d && dlgHitButton(d, p.x, p.y) != -1) ? HTCLIENT : HTCAPTION;
    }
    case WM_SETCURSOR:
        if (d && d->x && d->x->input && LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (PtInRect(&d->field, p)) { SetCursor(LoadCursor(NULL, IDC_IBEAM)); return TRUE; }
        }
        break;
    case WM_CHAR:
        if (d && d->x && d->x->input && w >= 32 && w != 127) {
            wchar_t c = (wchar_t)w;
            dlgInsert(h, d, &c, 1);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (d && d->x && d->x->nItems) {
            d->itemTop += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;       // the painter keeps it in range
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        dlgPaint(h, d, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (d->drag) {
            int y = GET_Y_LPARAM(l), top = d->itemTop;
            if (d->drag == 1) top = dlgTopForBar(d, y);
            else {
                int dy = y - d->dragY;
                if (abs(dy) > dp(4)) d->dragMoved = TRUE;
                if (d->dragMoved) top = d->dragTop - dy / dp(DLG_ROW);
            }
            if (top != d->itemTop) { d->itemTop = top; InvalidateRect(h, NULL, FALSE); }    // the painter keeps it in range
            return 0;
        }
        int hot = dlgHitButton(d, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hot != d->hot) { d->hot = hot; InvalidateRect(h, NULL, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        d->hot = -1; InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        if (d && d->drag) { d->drag = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (d->x && d->x->input && PtInRect(&d->field, p)) {     // put the caret at the nearest character
            const wchar_t* t = d->x->input;
            int at = 0, len = (int)wcslen(t), px = p.x - d->field.left - dp(10);
            while (at < len && textWidthN(g_fBody, t, at + 1) <= px + dp(3)) at++;
            d->caret = d->selA = at;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (d->x && d->x->checks && (PtInRect(&d->selAll, p) || PtInRect(&d->selNone, p))) {
            BOOL on = PtInRect(&d->selAll, p);
            for (int i = 0; i < d->x->nItems; i++) d->x->checks[i] = on;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (d->x && d->x->nItems && PtInRect(&d->bar, p)) {      // the scrollbar: jump there, then follow the mouse
            d->drag = 1;
            d->itemTop = dlgTopForBar(d, p.y);
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        int item = dlgItemAt(d, p.x, p.y);
        if (item >= 0) {                  // a row: dragging scrolls the list; a click without moving picks or ticks it
            d->drag = 2; d->dragY = p.y; d->dragTop = d->itemTop; d->dragMoved = FALSE;
            SetCapture(h);
            if (!d->x->checks) dlgMovePick(h, d, item);
            return 0;
        }
        d->press = dlgHitButton(d, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (d->drag) {
            if (d->drag == 2 && !d->dragMoved && d->x->checks) {
                int item = dlgItemAt(d, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                if (item >= 0) { d->x->checks[item] = !d->x->checks[item]; dlgMovePick(h, d, item); }
            }
            d->drag = 0;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        int b = dlgHitButton(d, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        ReleaseCapture();
        if (b != -1 && b == d->press) dlgFinish(h, d, b == 0);
        d->press = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (d->x && d->x->input && dlgEditKey(h, d, w)) return 0;
        if (d->x && d->x->nItems && (w == VK_UP || w == VK_DOWN)) {
            dlgMovePick(h, d, *d->x->pick + (w == VK_UP ? -1 : 1));
            return 0;
        }
        if (d->x && d->x->checks && w == VK_SPACE) {          // Space ticks the row, Enter still confirms
            d->x->checks[*d->x->pick] = !d->x->checks[*d->x->pick];
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (w == VK_RETURN || (w == VK_SPACE && !(d->x && d->x->input))) dlgFinish(h, d, d->focus == 0);
        else if (w == VK_ESCAPE) { d->result = 0; d->done = TRUE; }
        else if ((w == VK_TAB || w == VK_LEFT || w == VK_RIGHT) && d->secondary) {
            d->focus = !d->focus; d->keyboard = TRUE; InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_SYSKEYDOWN:               // no system menu (it would draw a caption button), so handle Alt+F4
        if (w == VK_F4) { d->result = 0; d->done = TRUE; return 0; }
        break;
    case WM_CLOSE:
        d->result = 0; d->done = TRUE;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int showDialogEx(HWND owner, const wchar_t* title, const wchar_t* body,
                        const DlgRow* rows, int nrows,
                        const wchar_t* primary, const wchar_t* secondary, int focusBtn, const DlgExtra* extra) {
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"WCU_Dialog";
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = TRUE;
    }
    DlgState d = {0};
    d.title = title; d.body = body; d.rows = rows; d.nrows = nrows;
    d.primary = primary; d.secondary = secondary;
    d.focus = secondary ? focusBtn : 0; d.hot = -1; d.press = -1;
    d.x = extra;
    if (extra && extra->input) d.caret = (int)wcslen(extra->input);    // starts with it all selected
    if (extra && extra->nItems) d.itemTop = *extra->pick;              // the painter pulls this back into range

    // size to the content, clamped to the screen (the footer stays reachable, long bodies clip)
    int W = dp(DLG_WIDTH), pad = dp(DLG_PAD);
    int H = pad + dp(28) + dp(12) + measureWrapped(g_fBody, body, W - pad * 2)
          + (nrows ? dp(16) + dp(DLG_ROW) * nrows + dp(8) : 0) + dlgExtraHeight(extra) + dp(24) + dp(DLG_FOOTER);
    // A minimized window is at -32000,-32000: the dialog would open off the screen with the window disabled behind
    // it, so the app looks hung.
    if (owner && IsIconic(owner)) ShowWindow(owner, SW_RESTORE);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    MONITORINFO mi = { sizeof(mi) };
    if (owner && GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
    RECT cr = wa;
    if (owner && !IsIconic(owner)) GetWindowRect(owner, &cr);
    if (H > (wa.bottom - wa.top) - dp(48)) H = (wa.bottom - wa.top) - dp(48);
    int x = cr.left + ((cr.right - cr.left) - W) / 2;
    int y = cr.top + ((cr.bottom - cr.top) - H) / 2;
    x = max(wa.left, min(x, wa.right - W));      // always wholly on the window's monitor
    y = max(wa.top, min(y, wa.bottom - H));

    g_dlgDepth++;
    if (owner) { InvalidateRect(owner, NULL, FALSE); EnableWindow(owner, FALSE); }
    HWND h = CreateWindowExW(owner ? 0 : WS_EX_APPWINDOW, L"WCU_Dialog", title,
                             WS_POPUP | WS_CAPTION, x, y, W, H, owner, NULL, g_inst, &d);
    BOOL dark = g_dark;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int backdrop = 3;                 // DWMSBT_TRANSIENTWINDOW: acrylic, as Windows 11 flyouts use
    DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    int corner = WCU_DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    MARGINS mg = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(h, &mg);
    SetWindowPos(h, NULL, x, y, W, H, SWP_NOZORDER | SWP_FRAMECHANGED);
    d.fade = 0;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);

    // fade the contents in (120 ms) while staying responsive
    ULONGLONG fadeStart = GetTickCount64();
    SetTimer(h, 2, 16, NULL);

    MSG msg;
    while (!d.done) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)msg.wParam); break; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
        if (d.fade < 1) {
            d.fade = (double)(GetTickCount64() - fadeStart) / 120.0;
            if (d.fade >= 1) { d.fade = 1; KillTimer(h, 2); }
            InvalidateRect(h, NULL, FALSE);
        }
    }
    KillTimer(h, 2);

    // ...and out (90 ms) before it closes
    double fromFade = d.fade;
    fadeStart = GetTickCount64();
    for (;;) {
        double t = (double)(GetTickCount64() - fadeStart) / 90.0;
        d.fade = fromFade * (1 - t);
        if (t >= 1) break;
        InvalidateRect(h, NULL, FALSE);
        UpdateWindow(h);
        Sleep(10);
    }

    g_dlgDepth--;
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); InvalidateRect(owner, NULL, FALSE); }
    DestroyWindow(h);
    if (d.sf.mem) { DeleteObject(d.sf.memBmp); DeleteDC(d.sf.mem); }
    if (d.sf.scr) { DeleteObject(d.sf.scrBmp); DeleteDC(d.sf.scr); }
    return d.result;
}

static int showDialog(HWND owner, const wchar_t* title, const wchar_t* body,
                      const DlgRow* rows, int nrows,
                      const wchar_t* primary, const wchar_t* secondary, int focusBtn) {
    return showDialogEx(owner, title, body, rows, nrows, primary, secondary, focusBtn, NULL);
}

static void okDialog(const wchar_t* title, const wchar_t* body) { showDialog(g_hwnd, title, body, NULL, 0, L"OK", NULL, 0); }
