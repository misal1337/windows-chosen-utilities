// Drawing and the small helpers it needs: pixels on the backbuffer, text, fonts, colours, and theme.

// ---------- helpers ----------
static int dp(double dip) { return (int)lround(dip * g_dpi / 96.0); }

static void fmtSize(unsigned long long b, wchar_t* out, int cch) {
    const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) swprintf(out, cch, L"%llu %s", b, u[0]);
    else if (v < 10.0) swprintf(out, cch, L"%.1f %s", v, u[i]);
    else swprintf(out, cch, L"%.0f %s", v, u[i]);
}

static BOOL isElevated(void) {
    BOOL ret = FALSE; HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION el; DWORD cb = sizeof(el);
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb)) ret = el.TokenIsElevated;
        CloseHandle(tok);
    }
    return ret;
}

static BOOL readDarkTheme(void) {
    HKEY k; DWORD v = 1, cb = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"AppsUseLightTheme", NULL, NULL, (BYTE*)&v, &cb);
        RegCloseKey(k);
    }
    return v == 0;
}

static COLORREF readAccent(void) {
    // Prefer the user's accent from DWM; fall back to a Windows 11 default.
    DWORD argb = 0; BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
        COLORREF c = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
        if (c != RGB(0,0,0)) return c;
    }
    return g_dark ? RGB(76, 194, 255) : RGB(0, 95, 184);
}

// ---------- premultiplied BGRA blending on the backbuffer ----------
static inline void blendPx(int x, int y, int r, int g, int b, double a) {
    a *= g_alphaMul;
    if (a <= 0 || x < 0 || y < 0 || x >= g_bw || y >= g_bh) return;
    if (a > 1) a = 1;
    BYTE* p = (BYTE*)g_bits + ((size_t)y * g_bw + x) * 4;
    double ia = 1.0 - a;
    p[0] = (BYTE)(b * a + p[0] * ia);
    p[1] = (BYTE)(g * a + p[1] * ia);
    p[2] = (BYTE)(r * a + p[2] * ia);
    p[3] = (BYTE)(255 * a + p[3] * ia);
}

static void fillRect(int l, int t, int rr, int bb, int r, int g, int b, double a) {
    for (int y = t; y < bb; y++) for (int x = l; x < rr; x++) blendPx(x, y, r, g, b, a);
}

static void fillRoundRect(int l, int t, int rr, int bb, double radius, int r, int g, int b, double a) {
    double cx = (l + rr) / 2.0, cy = (t + bb) / 2.0;
    double inx = (rr - l) / 2.0 - radius, iny = (bb - t) / 2.0 - radius;
    if (inx < 0) inx = 0; if (iny < 0) iny = 0;
    for (int y = t; y < bb; y++) {
        if (y < 0 || y >= g_bh) continue;
        for (int x = l; x < rr; x++) {
            if (x < 0 || x >= g_bw) continue;
            double dx = fabs(x + 0.5 - cx) - inx; if (dx < 0) dx = 0;
            double dy = fabs(y + 0.5 - cy) - iny; if (dy < 0) dy = 0;
            double cov = 0.5 - (hypot(dx, dy) - radius);
            if (cov > 1) cov = 1; if (cov < 0) cov = 0;
            if (cov > 0) blendPx(x, y, r, g, b, a * cov);
        }
    }
}

// A border that lies wholly inside the same rectangle fillRoundRect would fill, following the same curve.
static void strokeRoundRect(int l, int t, int rr, int bb, double radius, double thick, int r, int g, int b, double a) {
    double cx = (l + rr) / 2.0, cy = (t + bb) / 2.0;
    double inx = (rr - l) / 2.0 - radius, iny = (bb - t) / 2.0 - radius;
    if (inx < 0) inx = 0; if (iny < 0) iny = 0;
    for (int y = t; y < bb; y++) {
        if (y < 0 || y >= g_bh) continue;
        for (int x = l; x < rr; x++) {
            if (x < 0 || x >= g_bw) continue;
            double dx = fabs(x + 0.5 - cx) - inx; if (dx < 0) dx = 0;
            double dy = fabs(y + 0.5 - cy) - iny; if (dy < 0) dy = 0;
            double d = hypot(dx, dy) - radius;              // signed distance to the outer edge
            double outer = 0.5 - d, inner = 0.5 - (d + thick);   // how far this pixel is inside each curve
            if (outer > 1) outer = 1; if (outer < 0) outer = 0;
            if (inner > 1) inner = 1; if (inner < 0) inner = 0;
            if (outer > inner) blendPx(x, y, r, g, b, a * (outer - inner));
        }
    }
}

static void fillAccent(int l, int t, int rr, int bb, double radius, double a) {
    fillRoundRect(l, t, rr, bb, radius, GetRValue(g_accent), GetGValue(g_accent), GetBValue(g_accent), a);
}
static void fillAccentRect(int l, int t, int rr, int bb, double a) {
    fillRect(l, t, rr, bb, GetRValue(g_accent), GetGValue(g_accent), GetBValue(g_accent), a);
}
static void comRelease(void* p) { ((IUnknown*)p)->lpVtbl->Release((IUnknown*)p); }

// ---------- text/icons on glass via an alpha mask ----------
// DirectWrite shapes and positions the text and Direct2D rasterises it, as in native Windows 11 apps:
// natural-symmetric subpixel positioning with gamma correction. It renders into a transparent scratch
// buffer that is then blended over the backbuffer, so text works on glass and honours the fades.
// Text is grayscale because colour ClearType needs an opaque background; Windows 11's Mica apps do the same.
static BOOL textCreateTarget(void) {
    if (g_rt) return TRUE;
    D2D1_RENDER_TARGET_PROPERTIES p;
    memset(&p, 0, sizeof(p));
    p.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    p.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    p.dpiX = 96.0f; p.dpiY = 96.0f;                 // coordinates and font sizes are device pixels
    if (FAILED(g_d2d->lpVtbl->CreateDCRenderTarget(g_d2d, &p, &g_rt))) return FALSE;
    ID2D1RenderTarget* rt = (ID2D1RenderTarget*)g_rt;
    D2D1_COLOR_F white = { 1, 1, 1, 1 };
    if (FAILED(rt->lpVtbl->CreateSolidColorBrush(rt, &white, NULL, &g_textBrush))) return FALSE;
    rt->lpVtbl->SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (g_textParams) rt->lpVtbl->SetTextRenderingParams(rt, g_textParams);
    return TRUE;
}
static void textReleaseTarget(void) {
    if (g_textBrush) { comRelease(g_textBrush); g_textBrush = NULL; }
    if (g_rt) { comRelease(g_rt); g_rt = NULL; }
}
static BOOL textInit(void) {
    static const GUID IID_D2DFactory = { 0x06152247, 0x6f50, 0x465a, { 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07 } };
    static const GUID IID_DWFactory  = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
    if (g_dw) return TRUE;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_DWFactory, (IUnknown**)&g_dw))) return FALSE;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_D2DFactory, NULL, (void**)&g_d2d))) return FALSE;
    IDWriteRenderingParams* def = NULL;
    if (SUCCEEDED(g_dw->lpVtbl->CreateRenderingParams(g_dw, &def))) {
        g_dw->lpVtbl->CreateCustomRenderingParams(g_dw, def->lpVtbl->GetGamma(def), def->lpVtbl->GetEnhancedContrast(def),
            0.0f /* no colour fringing */, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &g_textParams);
        comRelease(def);
    }
    return TRUE;
}

static IDWriteTextLayout* textLayout(WFont font, const wchar_t* s, int len, float w, float h) {
    IDWriteTextLayout* l = NULL;
    if (!g_dw || !font) return NULL;
    if (len < 0) len = (int)wcslen(s);
    if (FAILED(g_dw->lpVtbl->CreateTextLayout(g_dw, s, (UINT32)len, font, w, h, &l))) return NULL;
    return l;
}

static RECT g_textClip;                 // while on, text is cut off at this rectangle (scrolling columns)
static BOOL g_textClipOn = FALSE;
static void drawText(WFont font, const wchar_t* s, RECT rc, COLORREF col, UINT fmt) {
    if (!s || !*s || !g_scratchDC) return;
    if (!textCreateTarget()) return;
    RECT b = rc;
    if (b.left < 0) b.left = 0; if (b.top < 0) b.top = 0;
    if (b.right > g_bw) b.right = g_bw; if (b.bottom > g_bh) b.bottom = g_bh;
    if (g_textClipOn) {
        if (b.left < g_textClip.left) b.left = g_textClip.left;
        if (b.top < g_textClip.top) b.top = g_textClip.top;
        if (b.right > g_textClip.right) b.right = g_textClip.right;
        if (b.bottom > g_textClip.bottom) b.bottom = g_textClip.bottom;
    }
    if (b.right <= b.left || b.bottom <= b.top) return;

    IDWriteTextLayout* l = textLayout(font, s, -1, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    if (!l) return;
    l->lpVtbl->SetTextAlignment(l, (fmt & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER :
                                   (fmt & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    l->lpVtbl->SetParagraphAlignment(l, (fmt & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    l->lpVtbl->SetWordWrapping(l, (fmt & DT_WORDBREAK) ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (fmt & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS)) {
        DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        if (fmt & DT_PATH_ELLIPSIS) { t.delimiter = L'\\'; t.delimiterCount = 1; }   // keep the file name in view
        IDWriteInlineObject* sign = NULL;
        g_dw->lpVtbl->CreateEllipsisTrimmingSign(g_dw, font, &sign);
        l->lpVtbl->SetTrimming(l, &t, sign);
        if (sign) comRelease(sign);
    }

    ID2D1RenderTarget* rt = (ID2D1RenderTarget*)g_rt;
    HRESULT hr = g_rt->lpVtbl->BindDC(g_rt, g_scratchDC, &b);
    if (SUCCEEDED(hr)) {
        D2D1_COLOR_F clear = { 0, 0, 0, 0 };
        D2D1_COLOR_F c = { GetRValue(col) / 255.0f, GetGValue(col) / 255.0f, GetBValue(col) / 255.0f, 1.0f };
        D2D1_POINT_2F at = { (float)(rc.left - b.left), (float)(rc.top - b.top) };
        rt->lpVtbl->BeginDraw(rt);
        rt->lpVtbl->Clear(rt, &clear);
        g_textBrush->lpVtbl->SetColor(g_textBrush, &c);
        rt->lpVtbl->DrawTextLayout(rt, at, l, (ID2D1Brush*)g_textBrush,
                                   (fmt & DT_NOCLIP) ? D2D1_DRAW_TEXT_OPTIONS_NONE : D2D1_DRAW_TEXT_OPTIONS_CLIP);
        hr = rt->lpVtbl->EndDraw(rt, NULL, NULL);
    }
    comRelease(l);
    if (FAILED(hr)) { textReleaseTarget(); return; }   // recreated on the next call
    GdiFlush();

    // premultiplied source-over, scaled by the current fade
    double mul = g_alphaMul;
    for (int y = b.top; y < b.bottom; y++) {
        const BYTE* sp = (const BYTE*)g_scratchBits + ((size_t)y * g_bw + b.left) * 4;
        BYTE* dp2 = (BYTE*)g_bits + ((size_t)y * g_bw + b.left) * 4;
        for (int x = b.left; x < b.right; x++, sp += 4, dp2 += 4) {
            if (!sp[3]) continue;
            double a = sp[3] / 255.0 * mul, ia = 1.0 - a;
            dp2[0] = (BYTE)(sp[0] * mul + dp2[0] * ia + 0.5);
            dp2[1] = (BYTE)(sp[1] * mul + dp2[1] * ia + 0.5);
            dp2[2] = (BYTE)(sp[2] * mul + dp2[2] * ia + 0.5);
            dp2[3] = (BYTE)(sp[3] * mul + dp2[3] * ia + 0.5);
        }
    }
}

static int textWidthN(WFont font, const wchar_t* s, int n) {
    if (n <= 0) return 0;
    IDWriteTextLayout* l = textLayout(font, s, n, 100000.0f, 1000.0f);
    if (!l) return 0;
    l->lpVtbl->SetWordWrapping(l, DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m; memset(&m, 0, sizeof(m));
    l->lpVtbl->GetMetrics(l, &m);
    comRelease(l);
    return (int)ceil(m.widthIncludingTrailingWhitespace);
}
static int textWidth(WFont font, const wchar_t* s) { return textWidthN(font, s, (int)wcslen(s)); }

// ---------- fonts / theme ----------
static const wchar_t* pickFamily(const wchar_t* wanted, const wchar_t* fallback) {
    IDWriteFontCollection* c = NULL; UINT32 idx = 0; BOOL found = FALSE;
    if (g_dw && SUCCEEDED(g_dw->lpVtbl->GetSystemFontCollection(g_dw, &c, FALSE)) && c) {
        c->lpVtbl->FindFamilyName(c, wanted, &idx, &found);
        comRelease(c);
    }
    return found ? wanted : fallback;
}
static WFont mkFormat(const wchar_t* family, double dip, int weight) {
    WFont f = NULL;
    if (g_dw) g_dw->lpVtbl->CreateTextFormat(g_dw, family, NULL, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, (float)(dip * g_dpi / 96.0), L"en-us", &f);
    return f;
}
static WFont mkFont(double dip, int weight) {
    return mkFormat(pickFamily(L"Segoe UI Variable Text", L"Segoe UI"), dip, weight);
}
static WFont mkIconFont(double dip) {
    return mkFormat(pickFamily(L"Segoe Fluent Icons", L"Segoe MDL2 Assets"), dip, DWRITE_FONT_WEIGHT_NORMAL);
}
static void freeFont(WFont f) { if (f) comRelease(f); }
static void makeFonts(void) {
    textInit();
    if (g_fBody) { freeFont(g_fBody); freeFont(g_fBodyBold); freeFont(g_fSmall); freeFont(g_fH1);
                   freeFont(g_fTitle); freeFont(g_fIcon); freeFont(g_fIconLg); freeFont(g_fHeadIcon); freeFont(g_fMono);
                   freeFont(g_fTiny); }
    g_fTiny     = mkFont(10, FW_NORMAL);
    g_fBody     = mkFont(14, FW_NORMAL);
    g_fBodyBold = mkFont(14, FW_SEMIBOLD);
    g_fSmall    = mkFont(12, FW_NORMAL);
    g_fH1       = mkFont(22, FW_SEMIBOLD);
    g_fTitle    = mkFont(20, FW_SEMIBOLD);
    g_fIcon     = mkIconFont(16);
    g_fIconLg   = mkIconFont(22);
    g_fHeadIcon = mkIconFont(34);
    g_fMono     = mkFormat(pickFamily(L"Cascadia Mono", L"Consolas"), 12, DWRITE_FONT_WEIGHT_NORMAL);
}

static void resizeBackbuffer(int w, int h) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w == g_bw && h == g_bh && g_memDC) return;
    if (g_memDC) { DeleteObject(g_memBmp); DeleteDC(g_memDC); }
    if (g_scratchDC) { DeleteObject(g_scratchBmp); DeleteDC(g_scratchDC); }
    g_bw = w; g_bh = h;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL);
    g_memDC = CreateCompatibleDC(screen);
    g_memBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    SelectObject(g_memDC, g_memBmp);
    g_scratchDC = CreateCompatibleDC(screen);
    g_scratchBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_scratchBits, NULL, 0);
    SelectObject(g_scratchDC, g_scratchBmp);
    ReleaseDC(NULL, screen);
}

// ---------- rendering ----------
typedef struct { int r, g, b; double a; } Col;

static Col surfaceCol(void)   { return g_dark ? (Col){ 44, 44, 44, 0.55 } : (Col){ 255, 255, 255, 0.65 }; }
static Col textCol(void)      { return g_dark ? (Col){ 255,255,255, 0.95 } : (Col){ 0,0,0, 0.92 }; }
static COLORREF textRGB(void) { return g_dark ? RGB(255,255,255) : RGB(20,20,20); }
static COLORREF mutedRGB(void){ return g_dark ? RGB(170,170,170) : RGB(100,100,100); }
static Col hoverCol(void)     { return g_dark ? (Col){ 255,255,255, 0.0605 } : (Col){ 0,0,0, 0.0373 }; }
static Col sepCol(void)       { return g_dark ? (Col){ 255,255,255, 0.0837 } : (Col){ 0,0,0, 0.0803 }; }

// the hover tint, at a strength the caller has worked out
static void fillHover(int l, int t, int rr, int bb, double radius, double strength) {
    if (strength <= 0) return;
    Col h = hoverCol();
    fillRoundRect(l, t, rr, bb, radius, h.r, h.g, h.b, h.a * strength);
}

static void drawGlyph(WFont font, unsigned int cp, int cx, int cy, COLORREF col) {
    wchar_t s[3]; int n = 0;
    if (cp > 0xFFFF) { cp -= 0x10000; s[n++] = (wchar_t)(0xD800 + (cp >> 10)); s[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF)); }
    else s[n++] = (wchar_t)cp;
    s[n] = 0;
    RECT rc = { cx - dp(40), cy - dp(40), cx + dp(40), cy + dp(40) };
    drawText(font, s, rc, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
}

static void drawCheckStroke(int x0, int y0, int x1, int y1, double step, double a) {
    for (double u = 0; u <= 1; u += step) {
        int x = (int)(x0 + (x1 - x0) * u), y = (int)(y0 + (y1 - y0) * u);
        blendPx(x, y, 255, 255, 255, a);
        for (int o = -1; o <= 1; o++) {
            blendPx(x + o, y, 255, 255, 255, a * 0.6);
            blendPx(x, y + o, 255, 255, 255, a * 0.6);
        }
    }
}

static void drawCheckbox(int cx, int cy, int size, double checkedV, BOOL enabled) {
    int l = cx - size/2, t = cy - size/2;
    Col tc = textCol();
    double a = (enabled ? 1.0 : 0.4);
    if (checkedV < 1) strokeRoundRect(l, t, l+size, t+size, dp(4), dp(1), tc.r, tc.g, tc.b, a * 0.55 * (1 - checkedV));
    if (checkedV > 0) {
        a *= checkedV;
        fillAccent(l, t, l+size, t+size, dp(4), a);
        // check mark
        int x0 = l + size*0.24, y0 = t + size*0.52;
        int x1 = l + size*0.42, y1 = t + size*0.70;
        int x2 = l + size*0.78, y2 = t + size*0.30;
        drawCheckStroke(x0, y0, x1, y1, 0.03, a);
        drawCheckStroke(x1, y1, x2, y2, 0.02, a);
    }
}

// the real Windows UAC shield (stock icon), composited onto an opaque button area
static void drawShield(int x, int y, int size) {
    SHSTOCKICONINFO sii = { sizeof(sii) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_LARGEICON, &sii))) {
        DrawIconEx(g_memDC, x, y, sii.hIcon, size, size, 0, NULL, DI_NORMAL);
        DestroyIcon(sii.hIcon);
    }
}

// ---------- buttons, page headers, progress bars ----------
static Col buttonFillCol(void) { return g_dark ? (Col){ 255,255,255, 0.08 } : (Col){ 255,255,255, 0.75 }; }

// Accent-filled or outlined. hover is its 0..1 fade; al is its opacity, lower when disabled.
static void drawButtonFace(RECT b, int radius, BOOL primary, double hover, double al) {
    if (primary) {
        fillAccent(b.left, b.top, b.right, b.bottom, radius, al);
        if (hover > 0) fillRoundRect(b.left, b.top, b.right, b.bottom, radius, 255, 255, 255, 0.10 * hover);
        return;
    }
    Col f = buttonFillCol(), s = sepCol();
    fillRoundRect(b.left, b.top, b.right, b.bottom, radius, f.r, f.g, f.b, f.a * al);
    fillHover(b.left, b.top, b.right, b.bottom, radius, 2 * hover);
    strokeRoundRect(b.left, b.top, b.right, b.bottom, radius, dp(1), s.r, s.g, s.b, s.a * (g_dark ? 1.0 : 1.6) * al);   // white on white needs more
}

// Centred text, with the UAC shield ahead of it when the click will ask for administrator rights
static void drawButtonLabel(RECT b, WFont font, const wchar_t* text, COLORREF col, BOOL shield, int shieldSize, int gap) {
    if (!shield) {
        drawText(font, text, b, col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int sx = b.left + ((b.right - b.left) - (shieldSize + gap + textWidth(font, text))) / 2;
    drawShield(sx, b.top + ((b.bottom - b.top) - shieldSize) / 2, shieldSize);
    RECT t = { sx + shieldSize + gap, b.top, b.right, b.bottom };
    drawText(font, text, t, col, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void drawPageHeader(unsigned int glyph, int cL, int cR, int top, const wchar_t* title, const wchar_t* sub) {
    drawGlyph(g_fHeadIcon, glyph, cL + dp(44), top + dp(50), g_accent);
    RECT t1 = { cL + dp(84), top + dp(24), cR - dp(16), top + dp(24) + dp(32) };
    drawText(g_fH1, title, t1, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t2 = { cL + dp(84), top + dp(58), cR - dp(16), top + dp(58) + dp(22) };
    drawText(g_fBody, sub, t2, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// pct below 1 shows the empty track
static void drawProgressBar(int l, int r, int top, int pct) {
    Col s = sepCol();
    fillRoundRect(l, top, r, top + dp(4), dp(2), s.r, s.g, s.b, s.a * 2);
    if (pct > 0) fillAccent(l, top, l + (r - l) * pct / 100, top + dp(4), dp(2), 1.0);
}

static void applyWindowTheme(HWND h) {
    BOOL dark = g_dark;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int backdrop = WCU_DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    int corner = WCU_DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(h, &m);
}

