// Disk Clean-up page: scanning and deleting the fixed list of locations, and System Restore.

static const wchar_t* EXPLORER_PROMPT =
    L"Windows Explorer keeps the thumbnail and icon cache files open, so clearing them only "
    L"takes effect once it restarts. Until then, blank or wrong icons may linger.\n\n"
    L"Your taskbar and open File Explorer windows will briefly close and reopen.";

static unsigned long long patternSize(const Category* c) {
    unsigned long long total = 0;
    for (int i = 0; c->patterns[i]; i++) {
        wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%s\\%s", c->path, c->patterns[i]);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            total += fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return total;
}

static unsigned long long patternDelete(const Category* c) {
    unsigned long long freed = 0;
    for (int i = 0; c->patterns[i]; i++) {
        wchar_t pat[MAX_PATH]; swprintf(pat, MAX_PATH, L"%s\\%s", c->path, c->patterns[i]);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t full[MAX_PATH]; swprintf(full, MAX_PATH, L"%s\\%s", c->path, fd.cFileName);
            unsigned long long sz = fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(full)) freed += sz;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return freed;
}

static unsigned long long recycleSize(void) {
    SHQUERYRBINFO info = { sizeof(info) };
    if (SUCCEEDED(SHQueryRecycleBinW(NULL, &info))) return (unsigned long long)info.i64Size;
    return 0;
}

// ---------- System Restore ----------
// Restore points are listed and switched on/off through Windows' SystemRestore WMI class, and removed
// one by one with SRRemoveRestorePoint. Other shadow copies (backup software, for example) are left alone.
static BOOL g_srOff = FALSE;            // System Restore is turned off
static BOOL g_srWantOff = FALSE;        // what the running switch is asking for
static const wchar_t* g_scanNote = NULL; // shown ahead of the next scan's "Ready" line

static BOOL isRestoreKind(const Category* c) { return c->kind == KIND_RESTORE_OLD || c->kind == KIND_RESTORE_ALL; }

// Windows sets this to 0 when System Restore is off (documented on the SystemRestoreConfig class)
static BOOL restoreIsOff(void) {
    HKEY k; DWORD v = 0, cb = sizeof(v); BOOL off = TRUE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"RPSessionInterval", NULL, NULL, (BYTE*)&v, &cb) == ERROR_SUCCESS) off = (v == 0);
        RegCloseKey(k);
    }
    return off;
}

static IWbemServices* wmiOpen(const wchar_t* ns) {
    static const GUID clsidLocator = { 0x4590F811, 0x1D3A, 0x11D0, { 0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
    static const GUID iidLocator   = { 0xDC12A687, 0x737F, 0x11CF, { 0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24 } };
    IWbemLocator* loc = NULL; IWbemServices* svc = NULL;
    if (FAILED(CoCreateInstance(&clsidLocator, NULL, CLSCTX_INPROC_SERVER, &iidLocator, (void**)&loc))) return NULL;
    BSTR name = SysAllocString(ns);
    HRESULT hr = loc->lpVtbl->ConnectServer(loc, name, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(name);
    loc->lpVtbl->Release(loc);
    if (FAILED(hr)) return NULL;
    CoSetProxyBlanket((IUnknown*)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return svc;
}

static IEnumWbemClassObject* wmiQuery(IWbemServices* svc, const wchar_t* query) {
    IEnumWbemClassObject* en = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = svc->lpVtbl->ExecQuery(svc, lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(lang); SysFreeString(q);
    return FAILED(hr) ? NULL : en;
}

// Restore point sequence numbers; -1 if Windows can't list them (this needs administrator rights)
static int restoreList(DWORD* seq, int max) {
    int n = -1;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWbemServices* svc = wmiOpen(L"ROOT\\DEFAULT");
    if (svc) {
        IEnumWbemClassObject* en = wmiQuery(svc, L"SELECT SequenceNumber FROM SystemRestore");
        if (en) {
            IWbemClassObject* o = NULL; ULONG got = 0;
            n = 0;
            while (n < max && en->lpVtbl->Next(en, WBEM_INFINITE, 1, &o, &got) == S_OK && got) {
                VARIANT v; VariantInit(&v);
                if (SUCCEEDED(o->lpVtbl->Get(o, L"SequenceNumber", 0, &v, NULL, NULL)) && (v.vt == VT_I4 || v.vt == VT_UI4))
                    seq[n++] = (DWORD)v.lVal;
                VariantClear(&v);
                o->lpVtbl->Release(o);
            }
            en->lpVtbl->Release(en);
        }
        svc->lpVtbl->Release(svc);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return n;
}

// Which points to remove. With keepNewest, the highest sequence number stays.
static int restorePickRemovals(const DWORD* seq, int n, BOOL keepNewest, DWORD* out) {
    DWORD newest = 0; int m = 0;
    for (int i = 0; i < n; i++) if (seq[i] > newest) newest = seq[i];
    for (int i = 0; i < n; i++) if (!keepNewest || seq[i] != newest) out[m++] = seq[i];
    return m;
}

static void restoreRemove(BOOL keepNewest) {
    DWORD seq[256], rm[256];
    int n = restoreList(seq, 256);
    if (n <= 0) return;
    HMODULE dll = LoadLibraryW(L"srclient.dll");
    if (!dll) return;
    typedef DWORD (WINAPI *RemoveFn)(DWORD);
    RemoveFn remove = (RemoveFn)(void*)GetProcAddress(dll, "SRRemoveRestorePoint");
    if (remove) {
        int m = restorePickRemovals(seq, n, keepNewest, rm);
        for (int i = 0; i < m; i++) remove(rm[i]);
    }
    FreeLibrary(dll);
}

// Space used by shadow copies on all drives. Windows doesn't report it per restore point, and other
// shadow copies share it, so this is the most that removing every restore point could free.
static unsigned long long shadowStorageUsed(void) {
    unsigned long long total = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWbemServices* svc = wmiOpen(L"ROOT\\CIMV2");
    if (svc) {
        IEnumWbemClassObject* en = wmiQuery(svc, L"SELECT UsedSpace FROM Win32_ShadowStorage");
        if (en) {
            IWbemClassObject* o = NULL; ULONG got = 0;
            while (en->lpVtbl->Next(en, WBEM_INFINITE, 1, &o, &got) == S_OK && got) {
                VARIANT v; VariantInit(&v);
                if (SUCCEEDED(o->lpVtbl->Get(o, L"UsedSpace", 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal)
                    total += _wcstoui64(v.bstrVal, NULL, 10);
                VariantClear(&v);
                o->lpVtbl->Release(o);
            }
            en->lpVtbl->Release(en);
        }
        svc->lpVtbl->Release(svc);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return total;
}

// For restore rows, bytes holds the number of older points, or the most that removing them all could free.
static unsigned long long restoreMeasure(const Category* c) {
    DWORD seq[256];
    int n = restoreList(seq, 256);
    if (n <= 0) return 0;
    if (c->kind == KIND_RESTORE_OLD) return (unsigned long long)(n - 1);
    return shadowStorageUsed();
}

static void fmtRestore(const Category* c, wchar_t* out, int cch) {
    if (c->kind == KIND_RESTORE_OLD) {
        if (c->bytes == 0) wcscpy(out, L"No older points");
        else swprintf(out, cch, L"%llu older point%s", c->bytes, c->bytes == 1 ? L"" : L"s");
    } else if (c->bytes == 0) {
        wcscpy(out, L"None");
    } else {
        wchar_t s[32]; fmtSize(c->bytes, s, 32);
        swprintf(out, cch, L"Up to %s", s);
    }
}

// Turn monitoring on or off through SystemRestore.Enable/Disable. An empty drive turns it off on all
// drives; turning it on names the system drive, which must come first.
static BOOL restoreSetOff(BOOL off) {
    BOOL ok = FALSE;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWbemServices* svc = wmiOpen(L"ROOT\\DEFAULT");
    if (svc) {
        BSTR cls = SysAllocString(L"SystemRestore");
        BSTR method = SysAllocString(off ? L"Disable" : L"Enable");
        wchar_t drive[8] = L"";
        if (!off) { wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH); swprintf(drive, 8, L"%c:\\", win[0]); }
        IWbemClassObject* def = NULL; IWbemClassObject* inDef = NULL; IWbemClassObject* in = NULL; IWbemClassObject* outp = NULL;
        if (SUCCEEDED(svc->lpVtbl->GetObject(svc, cls, 0, NULL, &def, NULL)) &&
            SUCCEEDED(def->lpVtbl->GetMethod(def, method, 0, &inDef, NULL)) &&
            SUCCEEDED(inDef->lpVtbl->SpawnInstance(inDef, 0, &in))) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_BSTR; v.bstrVal = SysAllocString(drive);
            in->lpVtbl->Put(in, L"Drive", 0, &v, 0);
            VariantClear(&v);
            if (SUCCEEDED(svc->lpVtbl->ExecMethod(svc, cls, method, 0, NULL, in, &outp, NULL)) && outp) {
                VARIANT r; VariantInit(&r);
                if (SUCCEEDED(outp->lpVtbl->Get(outp, L"ReturnValue", 0, &r, NULL, NULL)))
                    ok = ((r.vt == VT_I4 || r.vt == VT_UI4) && r.lVal == 0);
                VariantClear(&r);
                outp->lpVtbl->Release(outp);
            }
        }
        if (in) in->lpVtbl->Release(in);
        if (inDef) inDef->lpVtbl->Release(inDef);
        if (def) def->lpVtbl->Release(def);
        SysFreeString(method); SysFreeString(cls);
        svc->lpVtbl->Release(svc);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// ---------- workers ----------
static DWORD WINAPI scanProc(LPVOID arg) {
    (void)arg;
    for (int i = 0; i < NCAT; i++) {
        Category* c = &g_cat[i];
        unsigned long long sz = 0;
        BOOL blocked = FALSE;
        if (!c->available) { /* leave 0 */ }
        else if (c->kind == KIND_RECYCLEBIN) sz = recycleSize();
        else if (c->kind == KIND_PATTERN)    sz = patternSize(c);
        else if (isRestoreKind(c)) {                  // Windows only lists restore points for administrators
            if (g_elevated) sz = restoreMeasure(c);
            else blocked = TRUE;
        }
        else if (!canListDir(c->path))       blocked = TRUE;
        else sz = folderBytes(c->path);
        EnterCriticalSection(&g_lock);
        if (blocked && c->adminKnown) sz = c->adminBytes;   // last size the elevated helper reported
        c->adminBlocked = blocked;
        c->bytes = sz; c->scanned = TRUE; c->status = 0;
        LeaveCriticalSection(&g_lock);
        PostMessageW(g_hwnd, WM_APP_CATDONE, i, 0);
    }
    PostMessageW(g_hwnd, WM_APP_SCANDONE, 0, 0);
    return 0;
}

static void cleanOneCategory(Category* c) {
    if (c->kind == KIND_RECYCLEBIN)
        SHEmptyRecycleBinW(NULL, NULL, SHERB_NOCONFIRMATION | SHERB_NOSOUND | SHERB_NOPROGRESSUI);
    else if (c->kind == KIND_PATTERN)
        patternDelete(c);
    else if (isRestoreKind(c))
        restoreRemove(c->kind == KIND_RESTORE_OLD);
    else { wchar_t* lp = longPath(c->path); if (lp) { deleteContents(lp); free(lp); } }
}

// Elevated helper: only ever cleans admin categories, so user folders (temp, Recycle Bin...) are
// always handled by the logged-in user's own process, even if another admin account approved UAC.
static void runAdminClean(int mask) {
    for (int i = 0; i < NCAT; i++)
        if (((mask >> i) & 1) && g_cat[i].needsAdmin && g_cat[i].available) cleanOneCategory(&g_cat[i]);
}

// Elevated helper: report admin-folder sizes back to the non-elevated window that started it.
static void postAdminSizes(HWND notify) {
    for (int i = 0; i < NCAT; i++) {
        Category* c = &g_cat[i];
        if (!c->needsAdmin || !c->available) continue;
        unsigned long long sz = 0;
        if (isRestoreKind(c)) sz = restoreMeasure(c);
        else sz = folderBytes(c->path);
        if (notify) PostMessageW(notify, WM_APP_ADMINSIZE, (WPARAM)i, (LPARAM)sz);
    }
}

// arg = the elevated helper's process handle, when it is cleaning the admin categories
static DWORD WINAPI cleanProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    if (helper) {
        for (int i = 0; i < NCAT; i++)
            if (g_cat[i].checked && g_cat[i].available && g_cat[i].needsAdmin) g_cat[i].status = 2;
        PostMessageW(g_hwnd, WM_APP_CLEANPROG, 0, 0);
        WaitForSingleObject(helper, INFINITE);
        CloseHandle(helper);
    }
    for (int i = 0; i < NCAT; i++) {
        Category* c = &g_cat[i];
        if (!c->checked || !c->available) continue;
        if (helper && c->needsAdmin) {                // already cleaned by the helper
            EnterCriticalSection(&g_lock); c->status = 3; c->scanned = FALSE; LeaveCriticalSection(&g_lock);
            continue;
        }
        EnterCriticalSection(&g_lock); c->status = 2; LeaveCriticalSection(&g_lock);
        PostMessageW(g_hwnd, WM_APP_CLEANPROG, i, 0);
        cleanOneCategory(c);
        EnterCriticalSection(&g_lock); c->status = 3; c->scanned = FALSE; LeaveCriticalSection(&g_lock);
    }
    PostMessageW(g_hwnd, WM_APP_CLEANDONE, 0, 0);
    return 0;
}

// the one pattern category is the thumbnail/icon cache
static Category* thumbCategory(void) {
    for (int i = 0; i < NCAT; i++) if (g_cat[i].kind == KIND_PATTERN) return &g_cat[i];
    return NULL;
}

// Ends this session's Explorer and waits until it is really gone, which is when its files are released. Exit code 0:
// Windows then leaves starting it again to us, so there are never two.
static void terminateExplorer(void) {
    HANDLE procs[32]; int n = 0;
    DWORD mine = 0, sess = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n < 32 && _wcsicmp(pe.szExeFile, L"explorer.exe") == 0 && ProcessIdToSessionId(pe.th32ProcessID, &sess) && sess == mine) {
                HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (p) procs[n++] = p;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    for (int i = 0; i < n; i++) TerminateProcess(procs[i], 0);
    if (n) WaitForMultipleObjects((DWORD)n, procs, TRUE, 3000);
    for (int i = 0; i < n; i++) CloseHandle(procs[i]);
}

// The rights Explorer runs with, taken while it is still running: an app running as administrator must not start the
// taskbar and desktop as administrator.
static HANDLE explorerToken(void) {
    DWORD pid = 0; HANDLE p = NULL, tok = NULL, dup = NULL;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", NULL), &pid);
    if (pid) p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p && OpenProcessToken(p, TOKEN_DUPLICATE | TOKEN_QUERY, &tok))
        if (!DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &dup)) dup = NULL;
    if (tok) CloseHandle(tok);
    if (p) CloseHandle(p);
    return dup;
}

// Stop Explorer (releasing its lock on the cache files), delete the now-unlocked thumbnail/icon cache, then start
// Explorer again straight away. Windows can't be relied on to do that: on some PCs it never does, and waiting for it
// took over 6 seconds.
static DWORD WINAPI restartExplorerProc(LPVOID arg) {
    (void)arg;
    HANDLE tok = g_elevated ? explorerToken() : NULL;
    terminateExplorer();
    Category* t = thumbCategory();
    if (t) patternDelete(t);                      // now succeeds -> cache really cleared
    wchar_t exe[MAX_PATH]; GetWindowsDirectoryW(exe, MAX_PATH - 16); wcscat(exe, L"\\explorer.exe");
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {0};
    if (!FindWindowW(L"Shell_TrayWnd", NULL)) {
        BOOL ok = tok ? CreateProcessWithTokenW(tok, 0, exe, NULL, 0, NULL, NULL, &si, &pi)
                      : !g_elevated && CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    if (tok) CloseHandle(tok);
    BOOL up = FALSE;
    for (int i = 0; i < 120 && !up; i++) {        // the taskbar is normally there in well under a second
        up = FindWindowW(L"Shell_TrayWnd", NULL) != NULL;
        if (!up) Sleep(50);
    }
    if (!up) ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL);
    PostMessageW(g_hwnd, WM_APP_EXPLORERDONE, 0, 0);
    return 0;
}
static void startScan(void) {
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    for (int i = 0; i < NCAT; i++) { g_cat[i].scanned = FALSE; g_cat[i].status = 1; }
    wcscpy(g_status, L"Scanning\u2026");
    startWorker(scanProc, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void startClean(HANDLE helper) {
    if (!jobBegin()) { if (helper) CloseHandle(helper); return; }
    Category* t = thumbCategory();
    g_askExplorer = (t && t->checked && t->available && t->bytes > 0);
    wcscpy(g_status, L"Cleaning\u2026");
    startWorker(cleanProc, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// worker for "Show size": wait for the helper, whose WM_APP_ADMINSIZE messages carry the sizes
static DWORD WINAPI measureProc(LPVOID arg) {
    WaitForSingleObject((HANDLE)arg, INFINITE);
    CloseHandle((HANDLE)arg);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < NCAT; i++) g_cat[i].scanned = TRUE;
    LeaveCriticalSection(&g_lock);
    PostMessageW(g_hwnd, WM_APP_SCANDONE, 0, 0);
    return 0;
}

static void startElevatedMeasure(void) {
    HANDLE proc = NULL; BOOL cancelled = FALSE;
    if (!startElevatedHelper(L"--measure-admin", &proc, &cancelled)) return;   // declined: nothing changes
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) { CloseHandle(proc); return; }
    for (int i = 0; i < NCAT; i++) if (g_cat[i].adminBlocked) g_cat[i].scanned = FALSE;
    wcscpy(g_status, L"Checking size\u2026");
    startWorker(measureProc, proc);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Restore rows stay out of the totals: they hold a count or an upper limit, not bytes that will be freed.
static unsigned long long selectedTotal(void) {
    unsigned long long t = 0;
    for (int i = 0; i < NCAT; i++)
        if (g_cat[i].checked && g_cat[i].scanned && !isRestoreKind(&g_cat[i])) t += g_cat[i].bytes;
    return t;
}
static unsigned long long scannedTotal(void) {
    unsigned long long t = 0;
    for (int i = 0; i < NCAT; i++)
        if (g_cat[i].scanned && !isRestoreKind(&g_cat[i])) t += g_cat[i].bytes;
    return t;
}
static BOOL anyChecked(void) {
    for (int i = 0; i < NCAT; i++) if (g_cat[i].checked && g_cat[i].available) return TRUE;
    return FALSE;
}
static BOOL anyAdminSelected(void) {
    for (int i = 0; i < NCAT; i++)
        if (g_cat[i].checked && g_cat[i].available && g_cat[i].needsAdmin) return TRUE;
    return FALSE;
}
static int selectionMask(void) {
    int m = 0;
    for (int i = 0; i < NCAT; i++) if (g_cat[i].checked && g_cat[i].available) m |= (1 << i);
    return m;
}
static int adminMask(void) {
    int m = 0;
    for (int i = 0; i < NCAT; i++) if (g_cat[i].needsAdmin) m |= (1 << i);
    return m;
}

// desired client height for the fixed content, at the current DPI
#define CLEANUP_SR_ROW_H 34
static int cleanupFixedH(void) {          // everything on the page except the category rows
    int pad = dp(16), headH = dp(96), footerH = dp(44);
    return pad + headH + pad + dp(8) + dp(CLEANUP_SR_ROW_H) + pad + footerH + pad;
}
static int contentClientH(void) { return cleanupFixedH() + dp(48) * NCAT; }
// rows give up some height on short screens, so the buttons below them stay in view
static int cleanupRowH(void) {
    int h = (g_bh - cleanupFixedH()) / NCAT;
    if (h > dp(48)) h = dp(48);
    if (h < dp(40)) h = dp(40);
    return h;
}
static void sizeToContent(HWND h) {
    int cw = dp(860), ch = contentClientH();   // left rail + content (860 keeps the AIO tools on one row)
    RECT r = { 0, 0, cw, ch };
    DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g_dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (wh > wa.bottom - wa.top) wh = wa.bottom - wa.top;
    int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    SetWindowPos(h, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void drawCleanupHeader(int cardL, int cardR, int headTop, int headH) {
    Col sf = surfaceCol();
    fillRoundRect(cardL, headTop, cardR, headTop + headH, dp(8), sf.r, sf.g, sf.b, sf.a);

    // big disk glyph
    drawGlyph(g_fHeadIcon, 0xEDA2, cardL + dp(44), headTop + headH/2, g_accent);

    // title + subtitle
    RECT rt = { cardL + dp(84), headTop + dp(16), cardR - dp(200), headTop + dp(16) + dp(34) };
    drawText(g_fH1, L"Disk Clean-up", rt, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t sub[128];
    {
        ULARGE_INTEGER freeB = {0}, totB = {0};
        GetDiskFreeSpaceExW(L"C:\\", &freeB, &totB, NULL);
        wchar_t f[32], tt[32]; fmtSize(freeB.QuadPart, f, 32); fmtSize(totB.QuadPart, tt, 32);
        swprintf(sub, 128, L"Drive C:  \u2022  %s free of %s", f, tt);
    }
    RECT rs = { cardL + dp(84), headTop + dp(52), cardR - dp(200), headTop + dp(52) + dp(22) };
    drawText(g_fBody, sub, rs, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // right column: an "Administrator" badge when elevated, then the reclaimable total
    int rcolR = cardR - dp(16);
    int ry0 = headTop + dp(14);
    if (g_elevated) {
        int sh = dp(14);
        int aw = textWidth(g_fSmall, L"Administrator");
        int chipW = dp(8) + sh + dp(6) + aw + dp(10);
        int cx = rcolR - chipW, chipH = dp(22);
        fillAccent(cx, ry0, rcolR, ry0 + chipH, dp(11), 0.16);
        drawGlyph(g_fIcon, 0xEA18, cx + dp(8) + sh/2, ry0 + chipH/2, g_accent);
        RECT at = { cx + dp(8) + sh + dp(4), ry0, rcolR, ry0 + chipH };
        drawText(g_fSmall, L"Administrator", at, g_accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        ry0 += dp(30);
    }
    wchar_t big[48];
    unsigned long long sel = selectedTotal();
    int anyScanned = 0; for (int i=0;i<NCAT;i++) if (g_cat[i].scanned) anyScanned=1;
    RECT rb1 = { cardR - dp(220), ry0, rcolR, ry0 + dp(30) };
    RECT rb2 = { cardR - dp(220), ry0 + dp(30), rcolR, ry0 + dp(30) + dp(20) };
    if (g_busy == 1) {
        drawText(g_fBody, L"Scanning\u2026", rb1, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (anyScanned) {
        fmtSize(sel, big, 48);
        drawText(g_fH1, big, rb1, g_accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        drawText(g_fSmall, L"selected to remove", rb2, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void drawCleanupRow(int i, int cardL, int cardR, int listTop, int rowH) {
    Category* c = &g_cat[i];
    int ry = listTop + dp(4) + i * rowH;
    RECT row = { cardL + dp(6), ry, cardR - dp(6), ry + rowH - dp(0) };
    g_rcRow[i] = row;
    BOOL enabled = c->available;
    g_rowsEnabled[i] = enabled;

    double rowV = anim(ANIM_ROW + i, i == g_hotRow && enabled);
    fillHover(row.left, ry + dp(2), row.right, ry + rowH - dp(2), dp(4), rowV);
    {
        Col s = sepCol();
        for (int x = row.left + dp(48); x < row.right - dp(8); x++) blendPx(x, ry + rowH - 1, s.r, s.g, s.b, s.a);
    }

    int midY = ry + rowH/2;
    // checkbox (show unchecked on rows that can't be acted on)
    drawCheckbox(row.left + dp(22), midY, dp(18), anim(ANIM_CHK + i, c->checked && enabled), enabled);
    // icon
    drawGlyph(g_fIconLg, c->glyph, row.left + dp(60), midY, enabled ? g_accent : mutedRGB());

    // name + desc
    COLORREF tcol = enabled ? textRGB() : mutedRGB();
    int textTop = ry + (rowH - dp(39)) / 2;
    int textR = cardR - dp(234);
    RECT rn = { row.left + dp(88), textTop, textR, textTop + dp(22) };
    drawText(g_fBodyBold, c->name, rn, tcol, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT rd = { row.left + dp(88), textTop + dp(21), textR, textTop + dp(39) };
    drawText(g_fSmall, c->desc, rd, mutedRGB(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // small View button: opens the row's files in Explorer, or the restore points in Windows' own list
    {
        const wchar_t* vl = isRestoreKind(c) ? L"View points" : L"View files";
        BOOL vshield = isRestoreKind(c) && !g_elevated;
        RECT vb = { cardR - dp(226), midY - dp(13), cardR - dp(132), midY + dp(13) };
        g_rcView[i] = enabled ? vb : (RECT){ 0, 0, 0, 0 };
        drawButtonFace(vb, dp(4), FALSE, anim(ANIM_VIEW + i, g_hotBtn == BTN_VIEW + i && enabled), enabled ? 1.0 : 0.4);
        drawButtonLabel(vb, g_fSmall, vl, (vshield || enabled) ? textRGB() : mutedRGB(), vshield, dp(12), dp(3));
    }
    // size / status on the right
    RECT rz = { cardR - dp(128), ry, cardR - dp(18), ry + rowH };
    wchar_t sz[48];
    SetRectEmpty(&g_rcSizeLink[i]);
    if (!c->available) {
        drawText(g_fSmall, L"Not present", rz, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (c->status == 2) {
        drawText(g_fSmall, L"Cleaning\u2026", rz, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (c->status == 3 && !c->scanned) {
        drawText(g_fSmall, L"Cleaned", rz, g_accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (c->scanned && c->adminBlocked && !c->adminKnown) {
        // Windows won't let a normal process list this folder: one UAC approval measures it
        int tw = textWidth(g_fSmall, L"Show size");
        int sh = dp(16), left = rz.right - tw - dp(4) - sh;
        drawGlyph(g_fIcon, 0xEA18, left + sh/2, midY, g_accent);
        drawText(g_fSmall, L"Show size", rz, g_accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        g_rcSizeLink[i] = (RECT){ left - dp(6), midY - dp(14), rz.right + dp(6), midY + dp(14) };
    } else if (c->scanned && isRestoreKind(c)) {
        fmtRestore(c, sz, 48);
        drawText(g_fSmall, sz, rz, tcol, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (c->scanned) {
        fmtSize(c->bytes, sz, 48);
        drawText(g_fBody, sz, rz, tcol, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else {
        drawText(g_fBody, L"\u2026", rz, mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

// small "Turn off System Restore" checkbox, closing the list. It acts at once, not on Clean up.
static void drawSrSwitch(int cardL, int cardR, int srTop, int srH) {
    int srMid = srTop + srH / 2;
    BOOL en = g_cat[NCAT - 1].available && g_busy == 0;
    const wchar_t* lbl = L"Turn off System Restore";
    int box = dp(14), boxL = cardL + dp(94), lblL = boxL + box + dp(8), lblW = textWidth(g_fSmall, lbl);
    int hitR = lblL + lblW + (g_elevated ? 0 : dp(18));
    g_rcSrOff = g_cat[NCAT - 1].available ? (RECT){ boxL - dp(6), srMid - dp(13), hitR + dp(6), srMid + dp(13) } : (RECT){ 0, 0, 0, 0 };
    double hv = anim(ANIM_SROFFH, g_hotBtn == BTN_SROFF && en);
    fillHover(g_rcSrOff.left, g_rcSrOff.top, g_rcSrOff.right, g_rcSrOff.bottom, dp(4), hv);
    drawCheckbox(boxL + box / 2, srMid, box, anim(ANIM_SROFF, g_srOff), en);
    RECT lr = { lblL, srTop, lblL + lblW + dp(2), srTop + srH };
    drawText(g_fSmall, lbl, lr, en ? textRGB() : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!g_elevated) drawShield(lblL + lblW + dp(4), srMid - dp(7), dp(14));
    RECT nr = { g_rcSrOff.right + dp(8), srTop, cardR - dp(18), srTop + srH };
    drawText(g_fSmall, L"No new restore points. Windows may delete existing ones.", nr, mutedRGB(),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void drawCleanupFooter(int cardL, int cardR, int footTop) {
    // status text (left)
    RECT rstat = { cardL + dp(4), footTop, cardR - dp(262), footTop + dp(40) };   // up to the buttons
    drawText(g_fSmall, g_status, rstat, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // buttons (right)
    int btnH = dp(36), btnW = dp(120), gap = dp(10);
    int bx = cardR, by = footTop + dp(2);
    g_rcBtnClean  = (RECT){ bx - btnW, by, bx, by + btnH };
    g_rcBtnRescan = (RECT){ bx - btnW*2 - gap, by, bx - btnW - gap, by + btnH };

    // Rescan (secondary)
    BOOL scanEn = (g_busy == 0);
    drawButtonFace(g_rcBtnRescan, dp(6), FALSE, anim(ANIM_BTN + BTN_RESCAN, g_hotBtn == BTN_RESCAN && scanEn), scanEn ? 1.0 : 0.4);
    drawButtonLabel(g_rcBtnRescan, g_fBody, L"Rescan", scanEn ? textRGB() : mutedRGB(), FALSE, 0, 0);
    // Clean up (primary accent); shows the UAC shield when elevation will be requested
    BOOL cleanEn = (g_busy == 0 && anyChecked());
    drawButtonFace(g_rcBtnClean, dp(6), TRUE, anim(ANIM_BTN + BTN_CLEAN, g_hotBtn == BTN_CLEAN && cleanEn), cleanEn ? 1.0 : 0.4);
    drawButtonLabel(g_rcBtnClean, g_fBodyBold, L"Clean up", RGB(255,255,255), !g_elevated && anyAdminSelected(), dp(16), dp(6));
}

static void drawCleanup(int cardL, int cardR) {
    int pad = dp(16), headTop = pad, headH = dp(96);
    drawCleanupHeader(cardL, cardR, headTop, headH);

    int listTop = headTop + headH + pad;
    int rowH = cleanupRowH();
    int srH = dp(CLEANUP_SR_ROW_H);
    int listBottom = listTop + rowH * NCAT + dp(8) + srH;
    Col sf = surfaceCol();
    fillRoundRect(cardL, listTop, cardR, listBottom, dp(8), sf.r, sf.g, sf.b, sf.a);
    for (int i = 0; i < NCAT; i++) drawCleanupRow(i, cardL, cardR, listTop, rowH);
    drawSrSwitch(cardL, cardR, listTop + dp(4) + NCAT * rowH, srH);

    drawCleanupFooter(cardL, cardR, listBottom + pad);
}

// ---------- category setup ----------
static void initCategories(void) {
    wchar_t tmp[MAX_PATH], win[MAX_PATH], local[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    if (wcslen(tmp) && tmp[wcslen(tmp)-1] == L'\\') tmp[wcslen(tmp)-1] = 0;
    GetWindowsDirectoryW(win, MAX_PATH);
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local);

    int i = 0;
    // 1 user temp
    g_cat[i] = (Category){ L"Temporary files", L"Leftover files in your user temp folder", KIND_FOLDER };
    wcscpy(g_cat[i].path, tmp); g_cat[i].glyph = 0xE8B7; g_cat[i].checked = TRUE; i++;
    // 2 windows temp
    g_cat[i] = (Category){ L"Windows temporary files", L"System-wide temp folder (C:\\Windows\\Temp)", KIND_FOLDER };
    swprintf(g_cat[i].path, MAX_PATH, L"%s\\Temp", win); g_cat[i].glyph = 0xE8B7; g_cat[i].needsAdmin = TRUE; g_cat[i].checked = TRUE; i++;
    // 3 recycle bin
    g_cat[i] = (Category){ L"Recycle Bin", L"Files you have deleted, on all drives", KIND_RECYCLEBIN };
    g_cat[i].glyph = 0xE74D; g_cat[i].checked = TRUE; i++;
    // 4 windows update cache
    g_cat[i] = (Category){ L"Windows Update cache", L"Downloaded update files (safely re-downloaded)", KIND_FOLDER };
    swprintf(g_cat[i].path, MAX_PATH, L"%s\\SoftwareDistribution\\Download", win); g_cat[i].glyph = 0xE895; g_cat[i].needsAdmin = TRUE; g_cat[i].checked = TRUE; i++;
    // 5 thumbnail cache
    g_cat[i] = (Category){ L"Thumbnail cache", L"Explorer thumbnail and icon caches", KIND_PATTERN };
    swprintf(g_cat[i].path, MAX_PATH, L"%s\\Microsoft\\Windows\\Explorer", local);
    g_cat[i].patterns[0]=L"thumbcache_*.db"; g_cat[i].patterns[1]=L"iconcache_*.db"; g_cat[i].patterns[2]=NULL;
    g_cat[i].glyph = 0xEB9F; g_cat[i].checked = FALSE; i++;
    // 6 crash dumps
    g_cat[i] = (Category){ L"Crash dumps", L"Application crash dump files", KIND_FOLDER };
    swprintf(g_cat[i].path, MAX_PATH, L"%s\\CrashDumps", local); g_cat[i].glyph = 0xE7BA; g_cat[i].checked = TRUE; i++;
    // 7 WER
    g_cat[i] = (Category){ L"Windows error reports", L"Queued problem-report data", KIND_FOLDER };
    swprintf(g_cat[i].path, MAX_PATH, L"%s\\Microsoft\\Windows\\WER", local); g_cat[i].glyph = 0xE7BA; g_cat[i].checked = TRUE; i++;
    // 8, 9 restore points: never ticked by default, because they can't be recreated
    g_cat[i] = (Category){ L"Old restore points", L"All System Restore points except the newest", KIND_RESTORE_OLD };
    g_cat[i].glyph = 0xE81C; g_cat[i].needsAdmin = TRUE; g_cat[i].checked = FALSE; i++;
    g_cat[i] = (Category){ L"All restore points", L"Every System Restore point, including the newest", KIND_RESTORE_ALL };
    g_cat[i].glyph = 0xE777; g_cat[i].needsAdmin = TRUE; g_cat[i].checked = FALSE; i++;

    wchar_t srDll[MAX_PATH];
    swprintf(srDll, MAX_PATH, L"%s\\System32\\srclient.dll", win);
    BOOL srPresent = GetFileAttributesW(srDll) != INVALID_FILE_ATTRIBUTES;

    for (int k = 0; k < NCAT; k++) {
        Category* c = &g_cat[k];
        if (c->kind == KIND_RECYCLEBIN) { c->available = TRUE; }
        else if (isRestoreKind(c)) { c->available = srPresent; }
        else {
            c->available = isFolder(c->path);
        }
        // admin-required categories stay enabled & checked; elevation is requested at clean time
    }
}

static BOOL confirmClean(void) {
    static wchar_t sizes[NCAT + 1][48];
    DlgRow rows[NCAT + 1];
    int n = 0;
    for (int i = 0; i < NCAT; i++) {
        Category* c = &g_cat[i];
        if (!c->checked || !c->available) continue;
        if (c->adminBlocked && !c->adminKnown) wcscpy(sizes[n], L"Size unknown");
        else if (isRestoreKind(c)) fmtRestore(c, sizes[n], 48);
        else fmtSize(c->bytes, sizes[n], 48);
        rows[n].left = c->name; rows[n].right = sizes[n]; n++;
    }
    if (n > 1) {
        BOOL restore = FALSE;
        for (int i = 0; i < NCAT; i++) if (g_cat[i].checked && g_cat[i].available && isRestoreKind(&g_cat[i])) restore = TRUE;
        fmtSize(selectedTotal(), sizes[n], 48);
        rows[n].left = restore ? L"Total, without restore points" : L"Total"; rows[n].right = sizes[n]; n++;
    }
    return showDialog(g_hwnd, L"Delete selected items?",
                      L"These items will be permanently deleted. This can't be undone.",
                      rows, n, L"Delete", L"Cancel", 1) == 1;
}

// One prompt only. If elevation is needed, the UAC prompt is the confirmation (declining it
// deletes nothing); the in-app dialog appears when UAC won't actually prompt.
static void doClean(void) {
    BOOL needElev = (!g_elevated && anyAdminSelected());
    if ((!needElev || !uacWillPrompt()) && !confirmClean()) return;

    if (!needElev) { startClean(NULL); return; }

    wchar_t args[48]; swprintf(args, 48, L"--clean-admin=%d", selectionMask() & adminMask());
    HANDLE proc = NULL; BOOL cancelled = FALSE;
    if (startElevatedHelper(args, &proc, &cancelled)) {
        startClean(proc);
    } else {
        wcscpy(g_status, cancelled ? L"Cancelled \u2014 nothing was deleted"
                                   : L"Couldn't start the administrator helper \u2014 nothing was deleted");
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

// View a row's contents in what Windows provides. Restore points have no files you can open, so those
// rows show Windows' own restore point list, which asks for administrator permission itself.
static void viewCategory(const Category* c) {
    const wchar_t* target = c->path;
    if (c->kind == KIND_RECYCLEBIN) target = L"shell:RecycleBinFolder";
    else if (isRestoreKind(c)) target = L"rstrui.exe";
    ShellExecuteW(g_hwnd, L"open", target, NULL, NULL, SW_SHOWNORMAL);
}

// arg = the elevated helper's process handle, or NULL when this process already has the rights
static DWORD WINAPI srSwitchProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    BOOL ok;
    ok = helper ? (waitExitCode(helper) == 0) : restoreSetOff(g_srWantOff);
    PostMessageW(g_hwnd, WM_APP_SRDONE, (WPARAM)ok, 0);
    return 0;
}

// One prompt: UAC is the confirmation, unless we're elevated or UAC approves silently
static void doSrSwitch(void) {
    BOOL off = !g_srOff, needElev = !g_elevated;
    if (!needElev || !uacWillPrompt()) {
        BOOL yes = off
            ? showDialog(g_hwnd, L"Turn off System Restore?",
                         L"Windows will stop creating restore points, and may delete the ones you have.",
                         NULL, 0, L"Turn off", L"Cancel", 1)
            : showDialog(g_hwnd, L"Turn on System Restore?",
                         L"Windows will create restore points again.", NULL, 0, L"Turn on", L"Cancel", 1);
        if (!yes) return;
    }
    HANDLE proc = NULL; BOOL cancelled = FALSE;
    if (needElev && !startElevatedHelper(off ? L"--sr-off" : L"--sr-on", &proc, &cancelled)) {
        wcscpy(g_status, cancelled ? L"Cancelled — System Restore wasn't changed"
                                   : L"Couldn't start the administrator helper — System Restore wasn't changed");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (!jobBegin()) { if (proc) CloseHandle(proc); return; }
    g_srWantOff = off;
    wcscpy(g_status, off ? L"Turning off System Restore…" : L"Turning on System Restore…");
    startWorker(srSwitchProc, proc);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void cleanupOnScanDone(void) {
    InterlockedExchange(&g_busy, 0);
    unsigned long long t = scannedTotal();
    int locations = 0;
    for (int i = 0; i < NCAT; i++) if (g_cat[i].scanned && g_cat[i].bytes > 0 && !isRestoreKind(&g_cat[i])) locations++;
    wchar_t ts[48]; fmtSize(t, ts, 48);
    swprintf(g_status, 256, L"%sReady \u2014 %s found in %d location%s",
             g_scanNote ? g_scanNote : g_justCleaned ? L"Clean-up complete. " : L"", ts, locations, locations == 1 ? L"" : L"s");
    g_justCleaned = FALSE;
    g_scanNote = NULL;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void cleanupOnCleanDone(void) {
    g_justCleaned = TRUE;
    if (g_askExplorer) {
        g_askExplorer = FALSE;
        if (showDialog(g_hwnd, L"Restart Explorer?", EXPLORER_PROMPT, NULL, 0,
                       L"Restart Explorer", L"Not now", 0)) {
            wcscpy(g_status, L"Restarting Explorer\u2026");
            startWorker(restartExplorerProc, NULL);
            InvalidateRect(g_hwnd, NULL, FALSE);
            return;
        }
    }
    InterlockedExchange(&g_busy, 0);
    startScan();
}
