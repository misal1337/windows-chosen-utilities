// Reg Networks page: network profiles, and the box that opens a pasted key in Registry Editor.

static const wchar_t* REGNET_KEY =
    L"Computer\\HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\NetworkList\\Profiles";
// same key for RegOpenKeyEx (no "Computer\" / root prefix), relative to HKLM:
static const wchar_t* PROFILES_SUBPATH =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\NetworkList\\Profiles";

// Open Registry Editor navigated to a key: regedit restores its last-used key on launch,
// so we write that value first, then start regedit (which prompts for elevation itself).
// A running regedit ignores new locations, so in that case a second window is opened (-m).
static BOOL openRegeditAt(const wchar_t* key) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
        0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"LastKey", 0, REG_SZ, (const BYTE*)key,
                       (DWORD)((wcslen(key) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    const wchar_t* args = FindWindowW(L"RegEdit_RegEdit", NULL) ? L"-m" : NULL;
    return (INT_PTR)ShellExecuteW(NULL, L"open", L"regedit.exe", args, NULL, SW_SHOWNORMAL) > 32;
}

// case-insensitive "does s start with pfx"
static int ciStartsWith(const wchar_t* s, const wchar_t* pfx) {
    for (; *pfx; s++, pfx++) {
        wchar_t a = *s, b = *pfx;
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

// A profile name is "generic" only if it is exactly  Network <n>  with n in 1..100.
// "Network" (no number), "Network 101", "My Network 2", "Tailscale", etc. are NOT generic.
static BOOL isGenericNetworkName(const wchar_t* name) {
    if (!name || !ciStartsWith(name, L"Network ")) return FALSE;
    const wchar_t* p = name + 8;              // after "Network "
    if (*p < L'0' || *p > L'9') return FALSE;
    int val = 0;
    for (; *p; p++) {
        if (*p < L'0' || *p > L'9') return FALSE;
        val = val * 10 + (*p - L'0');
        if (val > 100) return FALSE;
    }
    return val >= 1 && val <= 100;
}

// Enumerate root\subpath; a subkey matches if its ProfileName is a generic "Network <1..100>".
// Appends each matching name to `report`. If doDelete, deletes the whole matching subkey (tree).
// Returns match count, or -1 if the key could not be opened. *deleted = successful deletions.
static int purgeNetworks(HKEY root, const wchar_t* subpath, BOOL doDelete,
                         int* deleted, BOOL* denied, wchar_t* report, int reportCch) {
    if (report && reportCch) report[0] = 0;
    if (deleted) *deleted = 0;
    if (denied) *denied = FALSE;
    REGSAM sam = KEY_READ | KEY_ENUMERATE_SUB_KEYS;
    if (doDelete) sam |= KEY_WRITE | DELETE;
    HKEY h;
    LONG r = RegOpenKeyExW(root, subpath, 0, sam, &h);
    if (r != ERROR_SUCCESS) { if (r == ERROR_ACCESS_DENIED && denied) *denied = TRUE; return -1; }

    static wchar_t names[512][80];
    int count = 0; DWORD idx = 0;
    for (;;) {
        wchar_t nm[80]; DWORD nl = 80;
        if (RegEnumKeyExW(h, idx++, nm, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (count < 512) wcscpy(names[count++], nm);
    }

    int matched = 0, del = 0, pos = 0;
    for (int i = 0; i < count; i++) {
        HKEY sk;
        if (RegOpenKeyExW(h, names[i], 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;
        wchar_t pn[256]; DWORD cb = sizeof(pn), type = 0;
        LONG rv = RegQueryValueExW(sk, L"ProfileName", NULL, &type, (BYTE*)pn, &cb);
        RegCloseKey(sk);
        if (rv != ERROR_SUCCESS || type != REG_SZ) continue;
        pn[255] = 0;
        if (!isGenericNetworkName(pn)) continue;
        matched++;
        if (report && pos < reportCch - 80)
            pos += swprintf(report + pos, reportCch - pos, L"   •  %s\n", pn);
        if (doDelete) {
            LONG dr = RegDeleteTreeW(h, names[i]);
            if (dr == ERROR_SUCCESS) del++;
            else if (dr == ERROR_ACCESS_DENIED && denied) *denied = TRUE;
        }
    }
    RegCloseKey(h);
    if (deleted) *deleted = del;
    return matched;
}

static void relaunchElevatedPurge(void) {
    wchar_t exe[MAX_PATH], args[64]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(args, 64, L"--purge-networks --notify=%llu", (unsigned long long)(ULONG_PTR)g_hwnd);
    // if it starts, its result dialog comes first, and it gives the foreground back when that closes
    if ((INT_PTR)ShellExecuteW(g_hwnd, L"runas", exe, args, NULL, SW_SHOWNORMAL) <= 32) raiseStart(g_hwnd);
}

static void doPurgeNetworks(void) {
    wchar_t list[4096]; int del = 0; BOOL den = FALSE;
    int matched = purgeNetworks(HKEY_LOCAL_MACHINE, PROFILES_SUBPATH, FALSE, &del, &den, list, 4096);
    if (matched == 0) {
        okDialog(L"Nothing to remove", L"No generic “Network N” profiles were found.");
        return;
    }
    wchar_t msg[4608];
    if (matched < 0) {   // couldn't read (needs admin) — confirm generically, then elevate
        wcscpy(msg, L"This permanently removes every unidentified “Network 1”…“Network 100” "
                    L"profile from the registry.\n\n“Network” (no number), Tailscale, VPNs and named "
                    L"networks are kept.\n\nThis can't be undone.");
    } else {
        swprintf(msg, 4608,
            L"This permanently removes these %d network profile(s):\n\n%s\n"
            L"“Network” (no number), Tailscale, VPNs and named networks are kept.\n\n"
            L"This can't be undone.", matched, list);
    }
    if (!showDialog(g_hwnd, L"Remove network profiles?", msg, NULL, 0, L"Remove", L"Cancel", 1)) return;

    if (g_elevated) {
        wchar_t rep[4096]; int d = 0; BOOL dn = FALSE;
        int m = purgeNetworks(HKEY_LOCAL_MACHINE, PROFILES_SUBPATH, TRUE, &d, &dn, rep, 4096);
        wchar_t done[256];
        if (m <= 0) wcscpy(done, L"No profiles were removed.");
        else if (dn) swprintf(done, 256, L"Removed %d of %d profile(s); some were protected.", d, m);
        else swprintf(done, 256, L"Removed %d network profile(s).", d);
        okDialog(L"Network profiles", done);
    } else {
        relaunchElevatedPurge();   // one UAC prompt; the elevated copy does it and reports
    }
}

// ---------- Reg Networks: go to a pasted key ----------
// Accepts what people actually copy: Computer\..., HKLM\..., HKLM:\... (PowerShell),
// Registry::HKLM\..., [HKEY_...] from a .reg file, quotes, doubled backslashes, trailing slashes.
// Writes "HKEY_NAME\sub\key" to out. Returns FALSE if it doesn't start with a registry root.
static BOOL regNormalize(const wchar_t* in, wchar_t* out, int cch, HKEY* rootOut, const wchar_t** subOut) {
    static const struct { const wchar_t* name; const wchar_t* full; HKEY key; } roots[] = {
        { L"HKLM", L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE }, { L"HKEY_LOCAL_MACHINE", L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE },
        { L"HKCU", L"HKEY_CURRENT_USER", HKEY_CURRENT_USER },   { L"HKEY_CURRENT_USER", L"HKEY_CURRENT_USER", HKEY_CURRENT_USER },
        { L"HKCR", L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT },   { L"HKEY_CLASSES_ROOT", L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT },
        { L"HKU",  L"HKEY_USERS", HKEY_USERS },                 { L"HKEY_USERS", L"HKEY_USERS", HKEY_USERS },
        { L"HKCC", L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG }, { L"HKEY_CURRENT_CONFIG", L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG },
    };
    static const wchar_t* prefixes[] = { L"Microsoft.PowerShell.Core\\Registry::", L"Registry::", L"My Computer\\", L"Computer\\" };

    wchar_t buf[1024]; lstrcpynW(buf, in, 1024);
    wchar_t* s = buf;
    #define REG_JUNK(c) ((c) == L' ' || (c) == L'\t' || (c) == L'\r' || (c) == L'\n' || (c) == L'"' || (c) == L'\'' || (c) == L'[' || (c) == L']')
    while (REG_JUNK(*s)) s++;
    if (*s == L'-') s++;                                  // [-HKEY_...] is a .reg file's "delete this key"
    wchar_t* e = s + wcslen(s);
    while (e > s && REG_JUNK(e[-1])) *--e = 0;
    if (!wcschr(s, L'\\')) for (wchar_t* p = s; *p; p++) if (*p == L'/') *p = L'\\';   // HKLM/SOFTWARE
    while (*s == L'\\') s++;
    for (int i = 0; i < 4; i++) {
        if (ciStartsWith(s, prefixes[i])) { s += wcslen(prefixes[i]); while (*s == L'\\') s++; }
    }
    size_t tok = wcscspn(s, L"\\:");
    for (int i = 0; i < 10; i++) {
        if (wcslen(roots[i].name) != tok || !ciStartsWith(s, roots[i].name)) continue;
        const wchar_t* rest = s + tok;
        if (*rest == L':') rest++;
        int n = swprintf(out, cch, L"%s", roots[i].full);
        int rootLen = n;
        for (; *rest && n < cch - 2; rest++) {            // one backslash at a time, none at the end
            if (*rest == L'\\') {
                while (rest[1] == L'\\') rest++;
                if (!rest[1]) break;
            }
            out[n++] = *rest;
        }
        out[n] = 0;
        *rootOut = roots[i].key;
        *subOut = (out[rootLen] == L'\\') ? out + rootLen + 1 : out + rootLen;
        return TRUE;
    }
    return FALSE;
}
static BOOL regKeyExists(HKEY root, const wchar_t* sub) {
    if (!*sub) return TRUE;
    HKEY k;
    LONG r = RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k);
    if (r == ERROR_SUCCESS) { RegCloseKey(k); return TRUE; }
    return r != ERROR_FILE_NOT_FOUND && r != ERROR_PATH_NOT_FOUND;     // access denied still means it exists
}
static void regGo(void) {
    wchar_t full[1024]; HKEY root; const wchar_t* sub;
    if (!g_regBox[0]) return;
    if (!regNormalize(g_regBox, full, 1024, &root, &sub)) {
        swprintf(g_regStatus, 1200, L"That isn't a registry key. It should start with HKLM, HKCU, HKCR, HKU, HKCC or HKEY_…");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    wcscpy(g_regBox, full);
    // stay ready for the next key: it's selected, so a paste or typing replaces it
    g_focus = A_REG_BOX; g_focusBuf = g_regBox; g_focusCch = 1024; g_focusRo = FALSE;
    g_selA = 0; g_caret = (int)wcslen(g_regBox); g_fieldStart = 0;

    // Registry Editor quietly lands elsewhere for a missing key, so find the closest real parent ourselves
    wchar_t target[1024]; wcscpy(target, full);
    size_t subAt = sub - full;
    BOOL exact = TRUE;
    while (!regKeyExists(root, target + subAt)) {
        wchar_t* cut = wcsrchr(target + subAt, L'\\');
        if (cut) *cut = 0; else target[subAt] = 0;
        exact = FALSE;
    }
    if (target[subAt] == 0 && subAt > 0) target[subAt - 1] = 0;   // only the root is left

    wchar_t withComputer[1040]; swprintf(withComputer, 1040, L"Computer\\%s", target);
    if (!openRegeditAt(withComputer))
        swprintf(g_regStatus, 1200, L"Registry Editor didn't open (cancelled or blocked).");
    else if (exact)
        swprintf(g_regStatus, 1200, L"Opened in Registry Editor.");
    else
        swprintf(g_regStatus, 1200, L"That key doesn't exist. Opened the closest one that does: %s", target);
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void drawRegPaste(int cL, int cR, int top, int bottom) {
    Col sf = surfaceCol();
    fillRoundRect(cL, top, cR, bottom, dp(8), sf.r, sf.g, sf.b, sf.a);
    int x1 = cL + dp(20), x2 = cR - dp(20), y = top + dp(16);
    RECT t = { x1, y, x2, y + dp(22) };
    drawText(g_fBodyBold, L"Go to a registry key", t, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += dp(24);
    RECT n = { x1, y, x2, y + dp(36) };
    drawText(g_fSmall, L"Paste any key and it opens in Registry Editor. Computer\\ and short names "
                       L"like HKCU and HKLM are understood. To type one instead, press Enter.",
             n, mutedRGB(), DT_LEFT | DT_WORDBREAK);
    y += dp(42);
    RECT fr = { x1, y, x2 - dp(92), y + dp(34) };
    aioField(A_REG_BOX, fr, g_regBox, 1024, FALSE, L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    aioButton(A_REG_OPEN, x2 - dp(84), y, x2, y + dp(34), L"Open", TRUE, g_regBox[0] != 0, !g_elevated);
    y += dp(40);
    if (g_regStatus[0]) {
        RECT r = { x1, y, x2, bottom - dp(6) };
        drawText(g_fSmall, g_regStatus, r, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
}
static void regFocusBox(void) {        // Ctrl+V works as soon as the page opens
    g_focus = A_REG_BOX; g_focusBuf = g_regBox; g_focusCch = 1024; g_focusRo = FALSE;
    g_caret = g_selA = (int)wcslen(g_regBox);
    g_fieldStart = 0;
}

// ---------- Reg Networks page ----------
static void drawRegNet(int cL, int cR) {
    int pad = dp(16);
    Col sf = surfaceCol();
    int top = pad;
    int cardB = top + dp(286);
    fillRoundRect(cL, top, cR, cardB, dp(8), sf.r, sf.g, sf.b, sf.a);

    drawPageHeader(0xE968, cL, cR, top, L"Network profiles", L"Saved Wi‑Fi and Ethernet networks, in the Windows registry.");
    // key path box
    RECT kb = { cL + dp(20), top + dp(96), cR - dp(20), top + dp(96) + dp(40) };
    Col kf = g_dark ? (Col){ 255,255,255, 0.06 } : (Col){ 0,0,0, 0.045 };
    fillRoundRect(kb.left, kb.top, kb.right, kb.bottom, dp(6), kf.r, kf.g, kf.b, kf.a);
    RECT kt = { kb.left + dp(12), kb.top, kb.right - dp(12), kb.bottom };
    drawText(g_fSmall,
        L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\NetworkList\\Profiles",
        kt, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    // two buttons, side by side
    int bh = dp(36), by = top + dp(156), gap = dp(12);
    int bw1 = dp(190), bw2 = dp(228);
    RECT b1 = { cL + dp(20), by, cL + dp(20) + bw1, by + bh };
    RECT b2 = { b1.right + gap, by, b1.right + gap + bw2, by + bh };
    g_rcRegOpen = b1; g_rcRegPurge = b2;

    // 1) Open in Registry Editor (accent)
    drawButtonFace(b1, dp(6), TRUE, anim(ANIM_BTN + BTN_REGOPEN, g_hotBtn == BTN_REGOPEN), 1.0);
    drawButtonLabel(b1, g_fBodyBold, L"Open in Registry Editor", RGB(255,255,255), FALSE, 0, 0);

    // 2) Remove generic profiles (danger red)
    fillRoundRect(b2.left, b2.top, b2.right, b2.bottom, dp(6), 196, 43, 28, 1.0);
    double purgeHover = anim(ANIM_BTN + BTN_REGPURGE, g_hotBtn == BTN_REGPURGE);
    if (purgeHover > 0) fillRoundRect(b2.left, b2.top, b2.right, b2.bottom, dp(6), 255, 255, 255, 0.10 * purgeHover);
    drawButtonLabel(b2, g_fBodyBold, L"Remove “Network N” profiles", RGB(255,255,255), FALSE, 0, 0);
    // explanations
    RECT e1 = { cL + dp(20), by + bh + dp(14), cR - dp(20), by + bh + dp(14) + dp(20) };
    drawText(g_fSmall, L"Open  —  view the key in Registry Editor (asks for administrator).",
             e1, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT e2 = { cL + dp(20), by + bh + dp(38), cR - dp(20), by + bh + dp(38) + dp(44) };
    drawText(g_fSmall,
        L"Remove  —  deletes unidentified “Network 1”…“Network 100” profiles. "
        L"Keeps “Network”, Tailscale, VPNs and named networks. Needs administrator.",
        e2, mutedRGB(), DT_LEFT | DT_WORDBREAK);

    drawRegPaste(cL, cR, cardB + dp(12), g_bh - pad);
}

