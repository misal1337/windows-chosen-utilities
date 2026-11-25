// Windows AIO work, with no drawing: WIM editing, ISO building and extraction, setup media,
// hashes and USB drives. Everything uses APIs that ship with Windows: wimgapi.dll, IMAPI2 (late bound),
// the Virtual Disk API, CryptoAPI, diskpart and dism.

enum { AT_WIM, AT_ESD, AT_F2I, AT_I2F, AT_USB, AT_HASH, AT_COUNT };
static const wchar_t* AT_LABEL[AT_COUNT] = {
    L"WIM Editor", L"ESD to ISO", L"Folder2ISO", L"ISO2Folder", L"WinToUSB", L"Hash" };

// progress stages (a number, so the elevated helper can report them across processes)
enum { ST_NONE, ST_LOADING, ST_SAVING, ST_MOUNT, ST_COPY, ST_EXPORT, ST_ISO_PREP, ST_ISO_WRITE,
       ST_HASH, ST_PARTITION, ST_SPLIT, ST_BOOTSECT, ST_COMPRESS };
static const wchar_t* ST_TEXT[] = {
    L"Working\u2026", L"Reading images\u2026", L"Saving images\u2026", L"Opening ISO\u2026", L"Copying files\u2026",
    L"Merging images\u2026", L"Preparing ISO\u2026", L"Writing ISO\u2026", L"Calculating\u2026",
    L"Preparing USB drive\u2026", L"Splitting install image\u2026", L"Making USB bootable\u2026",
    L"Compressing to ESD. This can take a long time…" };

// custom result codes (the WinToUSB helper returns them as its exit code)
#define E_USB_NOTSAFE  0xA001
#define E_USB_DISKPART 0xA002
#define E_USB_NOLETTER 0xA003
#define E_USB_TOOBIG   0xA004
#define E_USB_SPLIT    0xA005
#define E_ISO_NOLETTER 0xA006
#define E_WIMGAPI      0xA008
#define E_ESD_COUNT    0xA00C

// ---------- job state ----------
enum { JOB_NONE, JOB_WIMLOAD, JOB_WIMSAVE, JOB_ESD_BUILD, JOB_ESD_DISM, JOB_FOLDER, JOB_ISO, JOB_ESDLOAD, JOB_F2I, JOB_I2F, JOB_USB, JOB_HASH };
static int  g_aioTab = AT_WIM;
static int  g_aioJob = JOB_NONE, g_aioJobTab = 0;
static BOOL g_aioRunning = FALSE;
static int  g_aioPct = -1, g_aioStage = ST_NONE;
static volatile LONG g_aioStop = 0;
static HWND g_aioNotify = NULL;
static BOOL g_aioCanStop = TRUE;
static wchar_t g_aioMsg[AT_COUNT][MAX_PATH + 96];

static void aioProg(int pct, int stage) {
    if (g_aioNotify) PostMessageW(g_aioNotify, WM_APP_AIOPROG, (WPARAM)pct, (LPARAM)stage);
}

// ---------- wimgapi (loaded at runtime) ----------
#define WIM_CREATE_ALWAYS 2
#define WIM_OPEN_EXISTING 3
#define WIM_COMPRESS_LZX  2
#define WIM_EXPORT_ALLOW_DUPLICATES 1
#define WCU_WIM_MSG_PROGRESS (WM_APP + 0x1476 + 2)
#define WCU_WIM_MSG_ABORT ((DWORD)-1)

static HANDLE (WINAPI *pWIMCreateFile)(PCWSTR, DWORD, DWORD, DWORD, DWORD, PDWORD);
static BOOL   (WINAPI *pWIMCloseHandle)(HANDLE);
static BOOL   (WINAPI *pWIMSetTemporaryPath)(HANDLE, PCWSTR);
static DWORD  (WINAPI *pWIMGetImageCount)(HANDLE);
static HANDLE (WINAPI *pWIMLoadImage)(HANDLE, DWORD);
static BOOL   (WINAPI *pWIMGetImageInformation)(HANDLE, PVOID*, PDWORD);
static BOOL   (WINAPI *pWIMSetImageInformation)(HANDLE, PVOID, DWORD);
static BOOL   (WINAPI *pWIMDeleteImage)(HANDLE, DWORD);
static BOOL   (WINAPI *pWIMExportImage)(HANDLE, HANDLE, DWORD);
static DWORD  (WINAPI *pWIMRegisterMessageCallback)(HANDLE, FARPROC, PVOID);
static BOOL   (WINAPI *pWIMUnregisterMessageCallback)(HANDLE, FARPROC);

static BOOL wimLoadApi(void) {
    static HMODULE m = NULL;
    if (m) return TRUE;
    wchar_t sys[MAX_PATH]; systemPath(sys, L"wimgapi.dll");
    HMODULE h = LoadLibraryW(sys);
    if (!h) return FALSE;
#define WIMF(n) *(FARPROC*)&p##n = GetProcAddress(h, #n); if (!p##n) return FALSE;
    WIMF(WIMCreateFile) WIMF(WIMCloseHandle) WIMF(WIMSetTemporaryPath) WIMF(WIMGetImageCount)
    WIMF(WIMLoadImage) WIMF(WIMGetImageInformation) WIMF(WIMSetImageInformation) WIMF(WIMDeleteImage)
    WIMF(WIMExportImage) WIMF(WIMRegisterMessageCallback) WIMF(WIMUnregisterMessageCallback)
#undef WIMF
    m = h;
    return TRUE;
}

// ---------- image metadata XML ----------
#define WIM_MAX 128
typedef struct {
    wchar_t path[MAX_PATH];
    int index;
    wchar_t name[128], desc[256], edition[64], arch[8], build[32];
    BOOL dirty;
} WimEntry;
typedef struct { wchar_t path[MAX_PATH]; int index; } WimDeleted;

static WimEntry   g_wim[WIM_MAX];
static volatile LONG g_wimCount = 0;
static WimDeleted g_wimDel[WIM_MAX];
static int  g_wimDelCount = 0;
static int  g_wimSel = -1, g_wimTop = 0;
static int  g_wimHS = 0, g_wimHMax = 0;          // horizontal scroll of the columns, in device pixels
static BOOL g_wimHDrag = FALSE;
static RECT g_rcWimHBar;
static int  g_wimHThumbW = 0, g_wimHDragOfs = 0;
static BOOL g_wimOrderChanged = FALSE, g_wimShowSel = FALSE, g_wimJustSaved = FALSE;
static BOOL g_mediaDirty = TRUE;                  // the "Setup files from" choices need rebuilding
static wchar_t g_wimFirst[MAX_PATH];              // the first WIM added: Save writes everything into it
static short g_wimRows[WIM_MAX * 2];              // on-screen rows: an entry number, or -1 for a separator
static int  g_wimRowsCount = 0, g_wimRowsTop = 0, g_wimRowH = 1;
static wchar_t g_fName[128], g_fDesc[256];

static void xmlUnescape(wchar_t* s) {
    static const struct { const wchar_t* e; wchar_t c; } t[] = {
        { L"&amp;", L'&' }, { L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&quot;", L'"' }, { L"&apos;", L'\'' } };
    wchar_t* w = s;
    for (wchar_t* r = s; *r; ) {
        BOOL hit = FALSE;
        if (*r == L'&') for (int i = 0; i < 5; i++) {
            size_t n = wcslen(t[i].e);
            if (!wcsncmp(r, t[i].e, n)) { *w++ = t[i].c; r += n; hit = TRUE; break; }
        }
        if (!hit) *w++ = *r++;
    }
    *w = 0;
}
static int xmlEscape(const wchar_t* s, wchar_t* out, int cch) {
    int n = 0;
    for (; *s && n < cch - 8; s++) {
        const wchar_t* e = NULL;
        if (*s == L'&') e = L"&amp;"; else if (*s == L'<') e = L"&lt;"; else if (*s == L'>') e = L"&gt;";
        if (e) { wcscpy(out + n, e); n += (int)wcslen(e); } else out[n++] = *s;
    }
    out[n] = 0;
    return n;
}
static BOOL xmlGet(const wchar_t* xml, const wchar_t* tag, wchar_t* out, int cch) {
    wchar_t o[48], c[48];
    swprintf(o, 48, L"<%s>", tag); swprintf(c, 48, L"</%s>", tag);
    out[0] = 0;
    const wchar_t* a = wcsstr(xml, o);
    if (!a) return FALSE;
    a += wcslen(o);
    const wchar_t* b = wcsstr(a, c);
    if (!b) return FALSE;
    int n = (int)(b - a); if (n >= cch) n = cch - 1;
    wmemcpy(out, a, n); out[n] = 0;
    xmlUnescape(out);
    return TRUE;
}
// Returns a malloc'd copy of xml with <tag> holding value. A missing tag is added only if `add`.
static wchar_t* xmlSet(const wchar_t* xml, const wchar_t* tag, const wchar_t* value, BOOL add) {
    wchar_t o[48], c[48], esc[1024];
    swprintf(o, 48, L"<%s>", tag); swprintf(c, 48, L"</%s>", tag);
    xmlEscape(value, esc, 1024);
    size_t total = wcslen(xml) + wcslen(esc) + 128;
    wchar_t* out = (wchar_t*)malloc(total * sizeof(wchar_t));
    if (!out) return NULL;
    const wchar_t* a = wcsstr(xml, o);
    const wchar_t* b = a ? wcsstr(a, c) : NULL;
    if (a && b) {
        size_t head = (a - xml) + wcslen(o);
        wmemcpy(out, xml, head); out[head] = 0;
        wcscat(out, esc);
        wcscat(out, b);
    } else if (add && (b = wcsstr(xml, L"</IMAGE>")) != NULL) {
        size_t head = b - xml;
        wmemcpy(out, xml, head); out[head] = 0;
        wcscat(out, o); wcscat(out, esc); wcscat(out, c);
        wcscat(out, b);
    } else {
        wcscpy(out, xml);
    }
    return out;
}
static void wimParseInfo(const wchar_t* xml, WimEntry* e) {
    wchar_t a[16];
    xmlGet(xml, L"NAME", e->name, 128);
    xmlGet(xml, L"DESCRIPTION", e->desc, 256);
    xmlGet(xml, L"EDITIONID", e->edition, 64);
    xmlGet(xml, L"ARCH", a, 16);
    int v = a[0] ? _wtoi(a) : -1;
    wcscpy(e->arch, v == 0 ? L"x86" : v == 9 ? L"x64" : v == 12 ? L"ARM64" : v == 5 ? L"ARM" : L"-");
    wchar_t b[16], sp[16];                // <VERSION><BUILD>26100</BUILD><SPBUILD>1</SPBUILD>, shown as 26100.1
    xmlGet(xml, L"BUILD", b, 16);
    xmlGet(xml, L"SPBUILD", sp, 16);
    if (b[0]) swprintf(e->build, 32, L"%s.%s", b, sp[0] ? sp : L"0");
    else wcscpy(e->build, L"-");
}
// Writes name/description into an open image. Windows Setup prefers the DISPLAY* tags, so
// those are kept in step if they exist.
static BOOL wimApplyInfo(HANDLE img, const WimEntry* e) {
    PVOID raw = NULL; DWORD cb = 0;
    if (!pWIMGetImageInformation(img, &raw, &cb)) return FALSE;
    const wchar_t* src = (const wchar_t*)raw;
    size_t chars = cb / sizeof(wchar_t);
    wchar_t* base = (wchar_t*)malloc((chars + 1) * sizeof(wchar_t));
    if (!base) { LocalFree(raw); return FALSE; }
    wmemcpy(base, src, chars); base[chars] = 0;
    LocalFree(raw);
    const wchar_t* body = (base[0] == 0xFEFF) ? base + 1 : base;

    wchar_t* x1 = xmlSet(body, L"NAME", e->name, TRUE);
    wchar_t* x2 = x1 ? xmlSet(x1, L"DESCRIPTION", e->desc, TRUE) : NULL;
    wchar_t* x3 = x2 ? xmlSet(x2, L"DISPLAYNAME", e->name, FALSE) : NULL;
    wchar_t* x4 = x3 ? xmlSet(x3, L"DISPLAYDESCRIPTION", e->desc, FALSE) : NULL;
    BOOL ok = FALSE;
    if (x4) {
        size_t n = wcslen(x4);
        wchar_t* outb = (wchar_t*)malloc((n + 2) * sizeof(wchar_t));
        if (outb) {
            outb[0] = 0xFEFF;
            wmemcpy(outb + 1, x4, n);
            ok = pWIMSetImageInformation(img, outb, (DWORD)((n + 1) * sizeof(wchar_t)));
            free(outb);
        }
    }
    free(x1); free(x2); free(x3); free(x4); free(base);
    return ok;
}

static void wimTempDir(wchar_t* out) {
    GetTempPathW(MAX_PATH, out);
}

// Fills e from an open image. Returns FALSE, with e untouched, if Windows can't provide its details.
static BOOL wimReadImageInfo(HANDLE img, const wchar_t* path, DWORD index, WimEntry* e) {
    PVOID raw = NULL; DWORD cb = 0;
    if (!pWIMGetImageInformation(img, &raw, &cb)) return FALSE;
    size_t chars = cb / sizeof(wchar_t);
    wchar_t* xml = (wchar_t*)malloc((chars + 1) * sizeof(wchar_t));
    if (xml) {
        wmemcpy(xml, (wchar_t*)raw, chars); xml[chars] = 0;
        memset(e, 0, sizeof(*e));
        wcscpy(e->path, path);
        e->index = (int)index;
        wimParseInfo(xml[0] == 0xFEFF ? xml + 1 : xml, e);
        free(xml);
    }
    LocalFree(raw);
    return xml != NULL;
}

static DWORD wimReadFile(const wchar_t* path) {
    if (!wimLoadApi()) return E_WIMGAPI;
    for (LONG i = 0; i < g_wimCount; i++) if (!_wcsicmp(g_wim[i].path, path)) return 0;   // already listed
    if (!g_wimFirst[0]) lstrcpynW(g_wimFirst, path, MAX_PATH);
    g_mediaDirty = TRUE;
    wchar_t tmp[MAX_PATH]; wimTempDir(tmp);
    DWORD created;
    HANDLE h = pWIMCreateFile(path, GENERIC_READ, WIM_OPEN_EXISTING, 0, 0, &created);
    if (!h) return GetLastError();
    pWIMSetTemporaryPath(h, tmp);
    DWORD n = pWIMGetImageCount(h), err = 0;
    for (DWORD i = 1; i <= n && g_wimCount < WIM_MAX; i++) {
        HANDLE img = pWIMLoadImage(h, i);
        if (!img) { err = GetLastError(); break; }
        if (wimReadImageInfo(img, path, i, &g_wim[g_wimCount])) InterlockedIncrement(&g_wimCount);
        pWIMCloseHandle(img);
    }
    pWIMCloseHandle(h);
    return err;
}

// export progress is scaled into the part of the overall job that this image represents
static double g_wimPctBase = 0, g_wimPctSpan = 100;
static int g_wimStage = ST_EXPORT;
static DWORD WINAPI wimCallback(DWORD msg, WPARAM w, LPARAM l, PVOID u) {
    (void)l; (void)u;
    if (g_aioStop) return WCU_WIM_MSG_ABORT;
    if (msg == WCU_WIM_MSG_PROGRESS) aioProg((int)(g_wimPctBase + g_wimPctSpan * (double)w / 100.0), g_wimStage);
    return 0;
}

// Exports one image into an already open destination; if e is non-NULL its edits are applied.
static DWORD wimExportOne(const wchar_t* srcPath, DWORD index, HANDLE dst, const WimEntry* e) {
    wchar_t tmp[MAX_PATH]; wimTempDir(tmp);
    DWORD created, err = 0;
    HANDLE src = pWIMCreateFile(srcPath, GENERIC_READ, WIM_OPEN_EXISTING, 0, 0, &created);
    if (!src) return GetLastError();
    pWIMSetTemporaryPath(src, tmp);
    pWIMRegisterMessageCallback(src, (FARPROC)wimCallback, NULL);
    pWIMRegisterMessageCallback(dst, (FARPROC)wimCallback, NULL);
    HANDLE img = pWIMLoadImage(src, index);
    if (!img) err = GetLastError();
    else {
        if (!pWIMExportImage(img, dst, WIM_EXPORT_ALLOW_DUPLICATES)) err = GetLastError();
        pWIMCloseHandle(img);
    }
    pWIMUnregisterMessageCallback(dst, (FARPROC)wimCallback);
    pWIMUnregisterMessageCallback(src, (FARPROC)wimCallback);
    pWIMCloseHandle(src);
    if (!err && e && e->dirty) {
        HANDLE out = pWIMLoadImage(dst, pWIMGetImageCount(dst));
        if (!out) return GetLastError();
        if (!wimApplyInfo(out, e)) err = GetLastError();
        pWIMCloseHandle(out);
    }
    if (g_aioStop) return ERROR_CANCELLED;
    return err;
}

// In place: rename, then delete from the highest index down so lower indexes stay valid.
static DWORD wimSaveInPlace(void) {
    wchar_t tmp[MAX_PATH]; wimTempDir(tmp);
    DWORD created, err = 0;
    HANDLE h = pWIMCreateFile(g_wim[0].path, GENERIC_READ | GENERIC_WRITE, WIM_OPEN_EXISTING, 0, 0, &created);
    if (!h) return GetLastError();
    pWIMSetTemporaryPath(h, tmp);
    for (LONG i = 0; i < g_wimCount && !err; i++) {
        if (!g_wim[i].dirty) continue;
        HANDLE img = pWIMLoadImage(h, g_wim[i].index);
        if (!img) { err = GetLastError(); break; }
        if (!wimApplyInfo(img, &g_wim[i])) err = GetLastError();
        pWIMCloseHandle(img);
    }
    for (int pass = 0; pass < g_wimDelCount && !err; pass++) {
        int best = -1;
        for (int i = 0; i < g_wimDelCount; i++)
            if (g_wimDel[i].index > 0 && (best < 0 || g_wimDel[i].index > g_wimDel[best].index)) best = i;
        if (best < 0) break;
        if (!pWIMDeleteImage(h, g_wimDel[best].index)) err = GetLastError();
        g_wimDel[best].index = 0;
    }
    pWIMCloseHandle(h);
    return err;
}

// To a new file: built under a temporary name, so a failure can't leave a broken output behind.
static DWORD wimSaveAsRange(const wchar_t* out, double base, double span) {
    wchar_t tmp[MAX_PATH], work[MAX_PATH + 8]; wimTempDir(tmp);
    swprintf(work, MAX_PATH + 8, L"%s.tmp", out);
    DWORD created, err = 0;
    HANDLE dst = pWIMCreateFile(work, GENERIC_READ | GENERIC_WRITE, WIM_CREATE_ALWAYS, 0, WIM_COMPRESS_LZX, &created);
    if (!dst) return GetLastError();
    pWIMSetTemporaryPath(dst, tmp);
    g_wimStage = ST_SAVING;
    LONG n = g_wimCount;
    for (LONG i = 0; i < n && !err; i++) {
        g_wimPctBase = base + span * i / n; g_wimPctSpan = span / n;
        err = wimExportOne(g_wim[i].path, g_wim[i].index, dst, &g_wim[i]);
    }
    pWIMCloseHandle(dst);
    if (!err) SetFileAttributesW(out, FILE_ATTRIBUTE_NORMAL);
    if (!err && !MoveFileExW(work, out, MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
    if (err) DeleteFileW(work);
    return err;
}
static DWORD wimSaveAs(const wchar_t* out) { return wimSaveAsRange(out, 0, 100); }

// ---------- ISO: mount, copy ----------
static DWORD isoMount(const wchar_t* iso, HANDLE* vd, wchar_t* root) {
    static const GUID VENDOR_MS = { 0xEC984AEC, 0xA0F9, 0x47E9, { 0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B } };
    VIRTUAL_STORAGE_TYPE vst = { 1 /* ISO */, VENDOR_MS };
    HANDLE h = NULL;
    DWORD r = OpenVirtualDisk(&vst, iso, VIRTUAL_DISK_ACCESS_READ, OPEN_VIRTUAL_DISK_FLAG_NONE, NULL, &h);
    if (r) return r;
    r = AttachVirtualDisk(h, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY, 0, NULL, NULL);
    if (r) { CloseHandle(h); return r; }
    wchar_t phys[MAX_PATH]; ULONG cb = sizeof(phys);
    r = GetVirtualDiskPhysicalPath(h, &cb, phys);
    if (r) { CloseHandle(h); return r; }
    // \\.\CDROM2 -> \Device\CdRom2 -> whichever drive letter maps to it
    const wchar_t* num = phys + wcslen(phys);
    while (num > phys && iswdigit(num[-1])) num--;
    wchar_t want[64]; swprintf(want, 64, L"\\Device\\CdRom%s", num);
    for (int tries = 0; tries < 100; tries++) {
        for (wchar_t d = L'A'; d <= L'Z'; d++) {
            wchar_t dev[4] = { d, L':', 0 }, target[128];
            if (QueryDosDeviceW(dev, target, 128) && !_wcsicmp(target, want)) {
                swprintf(root, 4, L"%c:\\", d);
                DWORD a = GetFileAttributesW(root);
                if (a != INVALID_FILE_ATTRIBUTES) { *vd = h; return 0; }
            }
        }
        Sleep(100);
    }
    CloseHandle(h);
    return E_ISO_NOLETTER;
}
static void isoUnmount(HANDLE vd) {
    if (!vd) return;
    DetachVirtualDisk(vd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    CloseHandle(vd);
}

typedef struct { unsigned long long total, done; double base, span; int stage; } CopyCtx;
static BOOL g_copySkipImage = FALSE;     // leave out sources\install.wim and .esd when copying setup files

static void treeSize(const wchar_t* dir, unsigned long long* bytes) {
    wchar_t pat[1024]; swprintf(pat, 1024, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (isDotName(fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub[1024]; swprintf(sub, 1024, L"%s\\%s", dir, fd.cFileName);
            treeSize(sub, bytes);
        } else {
            *bytes += fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static DWORD CALLBACK copyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER s, LARGE_INTEGER sd,
                                   DWORD n, DWORD reason, HANDLE a, HANDLE b, LPVOID ctx) {
    (void)total; (void)s; (void)sd; (void)n; (void)reason; (void)a; (void)b;
    CopyCtx* c = (CopyCtx*)ctx;
    if (g_aioStop) return PROGRESS_CANCEL;
    if (c->total) {
        unsigned long long cur = c->done + (unsigned long long)done.QuadPart;
        aioProg((int)(c->base + c->span * (double)cur / (double)c->total), c->stage);
    }
    return PROGRESS_CONTINUE;
}
// Copies a tree, clearing the read-only flag that everything on a disc image carries.
// skipFile (relative, optional) is left out.
static DWORD copyTree(const wchar_t* src, const wchar_t* dst, CopyCtx* c, const wchar_t* skip, const wchar_t* rel) {
    CreateDirectoryW(dst, NULL);
    wchar_t pat[1024]; swprintf(pat, 1024, L"%s\\*", src);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD err = 0;
    do {
        if (isDotName(fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wchar_t s[1024], d[1024], r[768];
        swprintf(s, 1024, L"%s\\%s", src, fd.cFileName);
        swprintf(d, 1024, L"%s\\%s", dst, fd.cFileName);
        swprintf(r, 768, L"%s%s%s", rel, rel[0] ? L"\\" : L"", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            err = copyTree(s, d, c, skip, r);
        } else if (!(skip && !_wcsicmp(r, skip)) &&
                   !(g_copySkipImage && (!_wcsicmp(r, L"sources\\install.wim") || !_wcsicmp(r, L"sources\\install.esd")))) {
            SetFileAttributesW(d, FILE_ATTRIBUTE_NORMAL);
            if (!CopyFileExW(s, d, copyProgress, c, NULL, 0)) err = GetLastError();
            else {
                c->done += fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow);
                SetFileAttributesW(d, FILE_ATTRIBUTE_NORMAL);
            }
        }
    } while (!err && FindNextFileW(h, &fd));
    FindClose(h);
    return g_aioStop ? ERROR_CANCELLED : err;
}

// "\\?\" form of a full path, so copies aren't limited to 260 characters. Also creates missing parents.
static void extendedPath(const wchar_t* in, wchar_t* out, int cch) {
    wchar_t full[MAX_PATH * 2];
    if (!GetFullPathNameW(in, MAX_PATH * 2, full, NULL)) wcscpy(full, in);
    swprintf(out, cch, L"\\\\?\\%s", full);
}
static void createParents(const wchar_t* path) {
    wchar_t p[MAX_PATH * 2];
    lstrcpynW(p, path, MAX_PATH * 2);
    for (wchar_t* s = wcschr(p + 3, L'\\'); s; s = wcschr(s + 1, L'\\')) {
        *s = 0; CreateDirectoryW(p, NULL); *s = L'\\';
    }
}

static DWORD isoExtract(const wchar_t* iso, const wchar_t* dst, double base, double span) {
    HANDLE vd = NULL; wchar_t root[4];
    aioProg((int)base, ST_MOUNT);
    DWORD r = isoMount(iso, &vd, root);
    if (r) return r;
    wchar_t src[8]; swprintf(src, 8, L"%c:", root[0]);   // "X:" so paths join as X:\file
    wchar_t xsrc[16], xdst[MAX_PATH * 2 + 8];
    swprintf(xsrc, 16, L"\\\\?\\%c:", root[0]);
    createParents(dst);
    extendedPath(dst, xdst, MAX_PATH * 2 + 8);
    CopyCtx c = {0};
    treeSize(src, &c.total);
    c.base = base; c.span = span; c.stage = ST_COPY;
    r = copyTree(xsrc, xdst, &c, NULL, L"");
    isoUnmount(vd);
    return r;
}

// ---------- ISO: build with IMAPI2 through IDispatch ----------
static wchar_t g_dispErr[512];
static HRESULT dispCreate(const wchar_t* progId, IDispatch** out) {
    CLSID id; HRESULT hr = CLSIDFromProgID(progId, &id);
    if (FAILED(hr)) return hr;
    return CoCreateInstance(&id, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void**)out);
}
static HRESULT dispInvoke(IDispatch* d, const wchar_t* name, WORD flags, VARIANT* args, int nargs, VARIANT* result) {
    DISPID id; LPOLESTR nm = (LPOLESTR)name;
    HRESULT hr = d->lpVtbl->GetIDsOfNames(d, &IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    DISPPARAMS p = { args, NULL, (UINT)nargs, 0 };
    DISPID put = DISPID_PROPERTYPUT;
    if (flags & DISPATCH_PROPERTYPUT) { p.rgdispidNamedArgs = &put; p.cNamedArgs = 1; }
    EXCEPINFO ex; memset(&ex, 0, sizeof(ex));
    hr = d->lpVtbl->Invoke(d, id, &IID_NULL, LOCALE_USER_DEFAULT, flags, &p, result, &ex, NULL);
    if (hr == DISP_E_EXCEPTION) {      // the real error is inside the exception: keep it, and its text
        hr = ex.scode ? ex.scode : hr;
        swprintf(g_dispErr, 512, L"%s failed: %s", name, ex.bstrDescription ? ex.bstrDescription : L"(no description)");
        SysFreeString(ex.bstrDescription); SysFreeString(ex.bstrSource); SysFreeString(ex.bstrHelpFile);
    }
    return hr;
}
static HRESULT dispPutI4(IDispatch* d, const wchar_t* name, LONG v) {
    VARIANT a; VariantInit(&a); a.vt = VT_I4; a.lVal = v;
    return dispInvoke(d, name, DISPATCH_PROPERTYPUT, &a, 1, NULL);
}
static HRESULT dispPutStr(IDispatch* d, const wchar_t* name, const wchar_t* v) {
    VARIANT a; VariantInit(&a); a.vt = VT_BSTR; a.bstrVal = SysAllocString(v);
    HRESULT hr = dispInvoke(d, name, DISPATCH_PROPERTYPUT, &a, 1, NULL);
    VariantClear(&a);
    return hr;
}

// One El Torito entry. Windows media keeps BIOS and UEFI boot files at standard paths.
static HRESULT isoBootEntry(const wchar_t* file, LONG platform, IDispatch** out) {
    IStream* s = NULL; IDispatch* b = NULL;
    HRESULT hr = SHCreateStreamOnFileEx(file, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, NULL, &s);
    if (FAILED(hr)) return hr;
    hr = dispCreate(L"IMAPI2FS.BootOptions", &b);
    if (SUCCEEDED(hr)) {
        VARIANT a; VariantInit(&a); a.vt = VT_UNKNOWN; a.punkVal = (IUnknown*)s;
        hr = dispInvoke(b, L"AssignBootImage", DISPATCH_METHOD, &a, 1, NULL);
    }
    if (SUCCEEDED(hr)) dispPutI4(b, L"PlatformId", platform);
    if (SUCCEEDED(hr)) dispPutI4(b, L"Emulation", 0);
    if (SUCCEEDED(hr)) dispPutStr(b, L"Manufacturer", L"Microsoft");
    s->lpVtbl->Release(s);
    if (FAILED(hr)) { if (b) b->lpVtbl->Release(b); return hr; }
    *out = b;
    return S_OK;
}

// Build out from folder (bootable when the standard boot files are present).
static DWORD isoBuild(const wchar_t* fullFolder, const wchar_t* label, const wchar_t* out, double base, double span) {
    // IMAPI can't read paths over 260 characters, and setup media has deep ones. A long folder is
    // reached through a temporary drive letter for the duration of the build, which shortens them all.
    // The letter maps the parent, because IMAPI rejects a bare drive root like Z:\ as its folder.
    wchar_t alias[3] = L"", aliasFolder[MAX_PATH + 4] = L"", parent[MAX_PATH];
    const wchar_t* folder = fullFolder;
    lstrcpynW(parent, fullFolder, MAX_PATH);
    PathRemoveBackslashW(parent);
    const wchar_t* leaf = PathFindFileNameW(fullFolder);
    wchar_t leafCopy[MAX_PATH]; lstrcpynW(leafCopy, leaf, MAX_PATH);
    if (wcslen(fullFolder) > 40 && PathRemoveFileSpecW(parent) && !PathIsRootW(parent)) {
        DWORD used = GetLogicalDrives();
        for (wchar_t d = L'Z'; d >= L'H' && !alias[0]; d--) {
            if ((used >> (d - L'A')) & 1) continue;
            wchar_t a[3] = { d, L':', 0 };
            if (DefineDosDeviceW(0, a, parent)) {
                wcscpy(alias, a);
                swprintf(aliasFolder, MAX_PATH + 4, L"%s\\%s", a, leafCopy);
                folder = aliasFolder;
            }
        }
    }
    IDispatch* fsi = NULL; IDispatch* rootDir = NULL; IDispatch* result = NULL;
    IDispatch* boots[2] = { NULL, NULL }; IStream* image = NULL; int nb = 0;
    wchar_t work[MAX_PATH + 8]; swprintf(work, MAX_PATH + 8, L"%s.tmp", out);
    VARIANT v; VariantInit(&v);
    HANDLE hf = INVALID_HANDLE_VALUE;
    DWORD ret = 0;

    aioProg((int)base, ST_ISO_PREP);
    HRESULT hr = dispCreate(L"IMAPI2FS.MsftFileSystemImage", &fsi);
    if (FAILED(hr)) goto done;
    hr = dispPutI4(fsi, L"FileSystemsToCreate", 4 /* UDF */);
    if (SUCCEEDED(hr)) hr = dispPutI4(fsi, L"UDFRevision", 0x102);
    if (SUCCEEDED(hr)) hr = dispPutI4(fsi, L"FreeMediaBlocks", 0);
    if (SUCCEEDED(hr)) hr = dispPutStr(fsi, L"VolumeName", label && *label ? label : L"WINDOWS");
    if (FAILED(hr)) goto done;

    hr = dispInvoke(fsi, L"Root", DISPATCH_PROPERTYGET, NULL, 0, &v);
    if (FAILED(hr)) goto done;
    rootDir = v.pdispVal; VariantInit(&v);
    {
        VARIANT a[2]; VariantInit(&a[0]); VariantInit(&a[1]);        // arguments run last to first
        a[1].vt = VT_BSTR; a[1].bstrVal = SysAllocString(folder);
        a[0].vt = VT_BOOL; a[0].boolVal = VARIANT_FALSE;
        hr = dispInvoke(rootDir, L"AddTree", DISPATCH_METHOD, a, 2, NULL);
        VariantClear(&a[1]);
    }
    if (FAILED(hr)) goto done;
    if (g_aioStop) { ret = ERROR_CANCELLED; goto cleanup; }

    {
        wchar_t bios[MAX_PATH * 2], uefi[MAX_PATH * 2];
        swprintf(bios, MAX_PATH * 2, L"%s\\boot\\etfsboot.com", folder);
        swprintf(uefi, MAX_PATH * 2, L"%s\\efi\\microsoft\\boot\\efisys.bin", folder);
        if (GetFileAttributesW(bios) != INVALID_FILE_ATTRIBUTES &&
            SUCCEEDED(isoBootEntry(bios, 0 /* x86 BIOS */, &boots[nb]))) nb++;
        if (GetFileAttributesW(uefi) != INVALID_FILE_ATTRIBUTES &&
            SUCCEEDED(isoBootEntry(uefi, 0xEF /* EFI */, &boots[nb]))) nb++;
    }
    if (nb > 0) {
        SAFEARRAY* sa = SafeArrayCreateVector(VT_VARIANT, 0, nb);
        for (LONG i = 0; i < nb; i++) {
            VARIANT e; VariantInit(&e); e.vt = VT_DISPATCH; e.pdispVal = boots[i];
            SafeArrayPutElement(sa, &i, &e);
        }
        VARIANT a; VariantInit(&a); a.vt = VT_ARRAY | VT_VARIANT; a.parray = sa;
        hr = dispInvoke(fsi, L"BootImageOptionsArray", DISPATCH_PROPERTYPUT, &a, 1, NULL);
        SafeArrayDestroy(sa);
        if (FAILED(hr)) goto done;
    }

    hr = dispInvoke(fsi, L"CreateResultImage", DISPATCH_METHOD, NULL, 0, &v);
    if (FAILED(hr)) goto done;
    result = v.pdispVal; VariantInit(&v);
    hr = dispInvoke(result, L"ImageStream", DISPATCH_PROPERTYGET, NULL, 0, &v);
    if (FAILED(hr)) goto done;
    hr = v.punkVal->lpVtbl->QueryInterface(v.punkVal, &IID_IStream, (void**)&image);
    VariantClear(&v);
    if (FAILED(hr)) goto done;

    {
        STATSTG st; image->lpVtbl->Stat(image, &st, STATFLAG_NONAME);
        unsigned long long total = st.cbSize.QuadPart, written = 0;
        hf = CreateFileW(work, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) { ret = GetLastError(); goto cleanup; }
        static BYTE buf[1 << 20];
        for (;;) {
            ULONG got = 0; DWORD wr;
            hr = image->lpVtbl->Read(image, buf, sizeof(buf), &got);
            if (FAILED(hr)) goto done;
            if (!got) break;
            if (!WriteFile(hf, buf, got, &wr, NULL) || wr != got) { ret = GetLastError(); goto cleanup; }
            written += got;
            if (g_aioStop) { ret = ERROR_CANCELLED; goto cleanup; }
            if (total) aioProg((int)(base + span * (double)written / (double)total), ST_ISO_WRITE);
        }
    }
    hr = S_OK;
done:
    if (FAILED(hr)) ret = (DWORD)hr;
cleanup:
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    if (image) image->lpVtbl->Release(image);
    if (result) result->lpVtbl->Release(result);
    if (rootDir) rootDir->lpVtbl->Release(rootDir);
    for (int i = 0; i < 2; i++) if (boots[i]) boots[i]->lpVtbl->Release(boots[i]);
    if (fsi) fsi->lpVtbl->Release(fsi);
    if (alias[0]) DefineDosDeviceW(DDD_REMOVE_DEFINITION, alias, NULL);
    if (!ret && !MoveFileExW(work, out, MOVEFILE_REPLACE_EXISTING)) ret = GetLastError();
    if (ret) DeleteFileW(work);
    return ret;
}

// ---------- saving as setup media ----------
static BOOL mediaFileExists(const wchar_t* root, const wchar_t* rel) {
    wchar_t p[MAX_PATH * 2]; swprintf(p, MAX_PATH * 2, L"%s\\%s", root, rel);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
// A Windows setup folder: boot.wim plus BIOS or UEFI boot files
static BOOL mediaValid(const wchar_t* root) {
    return mediaFileExists(root, L"sources\\boot.wim") &&
           (mediaFileExists(root, L"efi\\microsoft\\boot\\efisys.bin") || mediaFileExists(root, L"boot\\etfsboot.com"));
}
static void trimSlash(wchar_t* p) { size_t n = wcslen(p); while (n > 0 && p[n - 1] == L'\\') p[--n] = 0; }
// The setup folder an image sits in: <root>\sources\install.wim gives <root>
static BOOL mediaRootOf(const wchar_t* image, wchar_t* root) {
    lstrcpynW(root, image, MAX_PATH);
    if (!PathRemoveFileSpecW(root)) return FALSE;
    trimSlash(root);
    if (_wcsicmp(PathFindFileNameW(root), L"sources")) return FALSE;
    if (!PathRemoveFileSpecW(root)) return FALSE;
    trimSlash(root);
    return mediaValid(root);
}

// The "Setup files from" row: one choice per build number among listed WIMs in setup folders,
// lowest first. Exactly one is selected; the lowest is the default.
#define MEDIA_MAX 6
typedef struct { wchar_t root[MAX_PATH]; wchar_t build[32]; int b, s; } MediaOpt;
static MediaOpt g_mediaOpts[MEDIA_MAX];
static int g_mediaCount = 0, g_mediaSel = 0;
static wchar_t g_mediaSelBuild[32];              // what you ticked, kept while the list changes

static void wimRefreshMedia(void) {
    g_mediaDirty = FALSE;
    g_mediaCount = 0;
    for (LONG i = 0; i < g_wimCount; i++) {
        BOOL have = FALSE;
        for (int m = 0; m < g_mediaCount; m++) if (!wcscmp(g_mediaOpts[m].build, g_wim[i].build)) have = TRUE;
        if (have || g_mediaCount >= MEDIA_MAX) continue;
        wchar_t r[MAX_PATH];
        if (!mediaRootOf(g_wim[i].path, r)) continue;
        MediaOpt o; memset(&o, 0, sizeof(o));
        wcscpy(o.root, r); wcscpy(o.build, g_wim[i].build);
        o.b = _wtoi(o.build);
        const wchar_t* dot = wcschr(o.build, L'.');
        o.s = dot ? _wtoi(dot + 1) : 0;
        int at = g_mediaCount++;
        while (at > 0 && (g_mediaOpts[at - 1].b > o.b || (g_mediaOpts[at - 1].b == o.b && g_mediaOpts[at - 1].s > o.s))) {
            g_mediaOpts[at] = g_mediaOpts[at - 1]; at--;
        }
        g_mediaOpts[at] = o;
    }
    g_mediaSel = 0;
    for (int m = 0; m < g_mediaCount; m++) if (!wcscmp(g_mediaOpts[m].build, g_mediaSelBuild)) g_mediaSel = m;
}

// These files lock Setup to one edition; removing them is what lets it offer all of them.
static void removeEditionLock(const wchar_t* media) {
    wchar_t f[MAX_PATH * 2 + 32];
    swprintf(f, MAX_PATH * 2 + 32, L"%s\\sources\\ei.cfg", media); DeleteFileW(f);
    swprintf(f, MAX_PATH * 2 + 32, L"%s\\sources\\pid.txt", media); DeleteFileW(f);
}

// Setup files from root, plus the listed images as sources\install.wim. Edition-lock files are left
// out so Setup offers every image. Partial output is removed by the caller.
static DWORD mediaBuild(const wchar_t* root, const wchar_t* dst, double base, double span) {
    if (!wimLoadApi()) return E_WIMGAPI;
    wchar_t r[MAX_PATH], full[MAX_PATH * 2], xsrc[MAX_PATH + 8], xdst[MAX_PATH * 2 + 8];
    lstrcpynW(r, root, MAX_PATH); trimSlash(r);
    if (!GetFullPathNameW(dst, MAX_PATH * 2, full, NULL)) wcscpy(full, dst);
    trimSlash(full);
    swprintf(xsrc, MAX_PATH + 8, L"\\\\?\\%s", r);
    swprintf(xdst, MAX_PATH * 2 + 8, L"\\\\?\\%s", full);
    createParents(full);
    CopyCtx c = {0};
    treeSize(r, &c.total);
    for (int i = 0; i < 2; i++) {         // the old install image isn't copied, so it isn't counted
        wchar_t im[MAX_PATH * 2]; WIN32_FILE_ATTRIBUTE_DATA fa;
        swprintf(im, MAX_PATH * 2, L"%s\\sources\\install.%s", r, i ? L"esd" : L"wim");
        if (GetFileAttributesExW(im, GetFileExInfoStandard, &fa)) {
            unsigned long long sz = fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow);
            c.total = c.total > sz ? c.total - sz : 0;
        }
    }
    c.base = base; c.span = span * 0.3; c.stage = ST_COPY;
    g_copySkipImage = TRUE;
    DWORD e = copyTree(xsrc, xdst, &c, NULL, L"");
    g_copySkipImage = FALSE;
    if (e) return e;
    removeEditionLock(full);
    wchar_t f[MAX_PATH * 2 + 32];
    swprintf(f, MAX_PATH * 2 + 32, L"%s\\sources\\install.wim", full);
    g_wimStage = ST_SAVING;
    return wimSaveAsRange(f, base + span * 0.3, span * 0.7);
}
static void removeTreeOwn(const wchar_t* dir) {   // only ever called on a folder this app created
    wchar_t* lp = longPath(dir);
    if (lp) { deleteContents(lp); RemoveDirectoryW(lp); free(lp); }
}

// ---------- ESD, in the elevated helper ----------
// DISM's "recovery" compression is what makes a real .esd, and it needs administrator rights.
static int g_esdIndex = 0, g_esdCount = 1;
static void esdProgress(int pct) {
    aioProg((int)(30 + 70.0 * (g_esdIndex + pct / 100.0) / g_esdCount), ST_COMPRESS);
}
static DWORD runEsdExport(const wchar_t* src, const wchar_t* dst, int count) {
    wchar_t exe[MAX_PATH], work[MAX_PATH + 16], cmd[MAX_PATH * 3 + 256];
    systemPath(exe, L"dism.exe");
    swprintf(work, MAX_PATH + 16, L"%s.tmp.esd", dst);
    DeleteFileW(work);
    g_esdCount = count > 0 ? count : 1;
    DWORD r = 0;
    for (g_esdIndex = 0; g_esdIndex < g_esdCount && !r; g_esdIndex++) {
        swprintf(cmd, MAX_PATH * 3 + 256,
                 L"\"%s\" /English /Export-Image /SourceImageFile:\"%s\" /SourceIndex:%d /DestinationImageFile:\"%s\" /Compress:recovery",
                 exe, src, g_esdIndex + 1, work);
        r = runWithPercent(exe, cmd, esdProgress);
    }
    if (!r) { SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL); if (!MoveFileExW(work, dst, MOVEFILE_REPLACE_EXISTING)) r = GetLastError(); }
    if (r) DeleteFileW(work);
    return r;
}

// ---------- ESD to ISO: the ESD's edition list, and fetching files from abbodi1406's GitHub repo ----------
// The job itself is in esdiso.c.
#define ESD_RAW_BASE L"https://raw.githubusercontent.com/abbodi1406/BatUtil/"
#define ESD_ROWS 14                        // list rows on screen at most; the list scrolls past that
static WimEntry g_esd[WIM_MAX];
static volatile LONG g_esdN = 0;
static BOOL g_esdTick[WIM_MAX];
static int  g_esdTop = 0, g_esdRowsTop = 0, g_esdRowH = 1, g_esdVisible = 1;
static wchar_t g_esdLoaded[MAX_PATH];      // the ESD the list shows
static wchar_t g_esdErr[400];              // why a GitHub step failed
static int  g_esdSkipped = 0;              // setup images in the ESD that aren't editions

static DWORD esdReadList(const wchar_t* path) {
    if (!wimLoadApi()) return E_WIMGAPI;
    g_esdN = 0; g_esdTop = 0; g_esdLoaded[0] = 0; g_esdSkipped = 0;
    wchar_t tmp[MAX_PATH]; wimTempDir(tmp);
    DWORD created, err = 0;
    HANDLE h = pWIMCreateFile(path, GENERIC_READ, WIM_OPEN_EXISTING, 0, 0, &created);
    if (!h) return GetLastError();
    pWIMSetTemporaryPath(h, tmp);
    DWORD n = pWIMGetImageCount(h);
    for (DWORD i = 1; i <= n && g_esdN < WIM_MAX; i++) {
        HANDLE img = pWIMLoadImage(h, i);
        if (!img) { err = GetLastError(); break; }
        LONG at = g_esdN;
        // Microsoft's ESDs also carry the setup images (Windows Setup Media, WinPE, Windows Setup), which
        // become the ISO's boot files. They aren't editions, so they're left off the list.
        if (wimReadImageInfo(img, path, i, &g_esd[at])) {
            const wchar_t* ed = g_esd[at].edition;
            if (!ed[0] || !_wcsicmp(ed, L"WindowsPE")) g_esdSkipped++;
            else { g_esdTick[at] = TRUE; InterlockedIncrement(&g_esdN); }
        }
        pWIMCloseHandle(img);
    }
    pWIMCloseHandle(h);
    if (!err && !g_esdN) err = E_ESD_COUNT;
    if (!err) lstrcpynW(g_esdLoaded, path, MAX_PATH);
    else g_esdN = 0;
    return err;
}

// Git's own hash of a file ("blob <size>\0" + the bytes), which the GitHub API lists as "sha"
static BOOL gitBlobSha1(const wchar_t* path, wchar_t* hex /* 41 */) {
    hex[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER size; BOOL ok = GetFileSizeEx(f, &size);
    BYTE* buf = (BYTE*)malloc(1 << 18);
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0;
    if (ok && buf && CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, CALG_SHA1, 0, 0, &h)) {
        char head[40]; int hn = sprintf(head, "blob %llu", (unsigned long long)size.QuadPart) + 1;   // with its zero
        ok = CryptHashData(h, (BYTE*)head, hn, 0);
        DWORD got;
        while (ok && ReadFile(f, buf, 1 << 18, &got, NULL) && got) ok = CryptHashData(h, buf, got, 0);
        BYTE d[20]; DWORD n = 20;
        if (ok && CryptGetHashParam(h, HP_HASHVAL, d, &n, 0)) for (DWORD i = 0; i < n; i++) swprintf(hex + i * 2, 3, L"%02x", d[i]);
        else ok = FALSE;
    } else ok = FALSE;
    if (h) CryptDestroyHash(h);
    if (prov) CryptReleaseContext(prov, 0);
    free(buf);
    CloseHandle(f);
    return ok;
}

typedef struct { wchar_t name[64], url[512], sha[48]; } GhFile;
// The files in one folder of the repo. Returns how many, or -1 with g_esdErr saying why.
static int ghList(const wchar_t* api, GhFile* out, int max) {
    char* body = NULL; DWORD status = 0;
    DWORD e = dlHttp(api, &body, NULL, &status);
    if (e || status != 200) {
        if (e) swprintf(g_esdErr, 400, L"Couldn't reach GitHub to get abbodi1406's ESD to ISO script (error %lu). Check the connection and try again.", e);
        else if (status == 403 || status == 429)
            swprintf(g_esdErr, 400, L"GitHub is limiting requests from this connection right now (HTTP %lu). Try again in an hour.", status);
        else swprintf(g_esdErr, 400, L"GitHub didn't list abbodi1406's ESD to ISO files (HTTP %lu). The folder may have moved.", status);
        free(body);
        return -1;
    }
    JIter it; const char* k; const char* v; int n = 0;
    if (jOpen(body, &it) && !it.obj) while (n < max && jNext(&it, &k, &v)) {
        wchar_t type[16]; jText(jGet(v, "type"), type, 16);
        if (wcscmp(type, L"file")) continue;
        GhFile* f = &out[n];
        jText(jGet(v, "name"), f->name, 64);
        jText(jGet(v, "download_url"), f->url, 512);
        jText(jGet(v, "sha"), f->sha, 48);
        if (f->name[0] && f->url[0] && wcslen(f->sha) == 40) n++;
    }
    free(body);
    if (!n) { swprintf(g_esdErr, 400, L"GitHub's list of abbodi1406's ESD to ISO files was empty or not understood."); return -1; }
    return n;
}
static const GhFile* ghFind(const GhFile* list, int n, const wchar_t* name) {
    for (int i = 0; i < n; i++) if (!_wcsicmp(list[i].name, name)) return &list[i];
    return NULL;
}
// Downloads one listed file into dir and proves it is the file GitHub lists
static BOOL ghFetch(const GhFile* g, const wchar_t* dir, wchar_t* path /* MAX_PATH */) {
    if (wcsncmp(g->url, ESD_RAW_BASE, wcslen(ESD_RAW_BASE)) || wcspbrk(g->name, L"\\/:*?\"<>|") || g->name[0] == L'.') {
        swprintf(g_esdErr, 400, L"GitHub listed an unexpected file (%s), so nothing was run.", g->name);
        return FALSE;
    }
    swprintf(path, MAX_PATH, L"%s\\%s", dir, g->name);
    DWORD status = 0, e = dlHttp(g->url, NULL, path, &status);
    if (e || status != 200) {
        swprintf(g_esdErr, 400, L"Couldn't download %s from abbodi1406's repo (%s %lu).", g->name, e ? L"error" : L"HTTP", e ? e : status);
        return FALSE;
    }
    wchar_t hex[48];
    if (!gitBlobSha1(path, hex) || _wcsicmp(hex, g->sha)) {
        swprintf(g_esdErr, 400, L"%s didn't match the hash GitHub lists for it, so it wasn't run.", g->name);
        DeleteFileW(path);
        return FALSE;
    }
    return TRUE;
}


// ---------- hashes ----------
static wchar_t g_hashOut[4][132];   // MD5, SHA-1, SHA-256, CRC-32

static void hexOf(const BYTE* b, DWORD n, wchar_t* out) {
    for (DWORD i = 0; i < n; i++) swprintf(out + i * 2, 3, L"%02X", b[i]);
    out[n * 2] = 0;
}
static DWORD hashFile(const wchar_t* path, wchar_t out[4][132]) {
    static const ALG_ID algs[3] = { CALG_MD5, CALG_SHA1, CALG_SHA_256 };
    static DWORD crcT[256];
    if (!crcT[1]) for (DWORD i = 0; i < 256; i++) {
        DWORD c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        crcT[i] = c;
    }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    HCRYPTPROV prov = 0; HCRYPTHASH h[3] = {0};
    DWORD err = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) err = GetLastError();
    for (int i = 0; i < 3 && !err; i++) if (!CryptCreateHash(prov, algs[i], 0, 0, &h[i])) err = GetLastError();
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    unsigned long long done = 0; DWORD crc = 0xFFFFFFFF;
    static BYTE buf[1 << 20];
    while (!err) {
        DWORD got = 0;
        if (!ReadFile(f, buf, sizeof(buf), &got, NULL)) { err = GetLastError(); break; }
        if (!got) break;
        for (int i = 0; i < 3 && !err; i++) if (!CryptHashData(h[i], buf, got, 0)) err = GetLastError();
        for (DWORD i = 0; i < got; i++) crc = crcT[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        done += got;
        if (g_aioStop) { err = ERROR_CANCELLED; break; }
        if (size.QuadPart) aioProg((int)(100.0 * done / size.QuadPart), ST_HASH);
    }
    if (!err) {
        for (int i = 0; i < 3 && !err; i++) {
            BYTE d[64]; DWORD n = sizeof(d);
            if (CryptGetHashParam(h[i], HP_HASHVAL, d, &n, 0)) hexOf(d, n, out[i]);
            else err = GetLastError();
        }
        swprintf(out[3], 132, L"%08X", ~crc);
    }
    for (int i = 0; i < 3; i++) if (h[i]) CryptDestroyHash(h[i]);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(f);
    return err;
}

// ---------- USB drives ----------
typedef struct { int number; unsigned long long size; wchar_t label[96]; } UsbDisk;
static UsbDisk g_usb[8];
static int g_usbCount = 0, g_usbSel = -1;

static int diskNumberOfLetter(wchar_t letter) {
    wchar_t dev[16]; swprintf(dev, 16, L"\\\\.\\%c:", letter);
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_DEVICE_NUMBER n; DWORD br; int r = -1;
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &n, sizeof(n), &br, NULL)) r = (int)n.DeviceNumber;
    CloseHandle(h);
    return r;
}

// Decides whether a disk may ever be erased: it must report the USB bus and must not hold Windows.
// Both the page and the elevated helper call this, so the check is repeated just before erasing.
static BOOL usbDiskSafe(int number, UsbDisk* info) {
    wchar_t dev[40]; swprintf(dev, 40, L"\\\\.\\PhysicalDrive%d", number);
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery };
    BYTE buf[1024]; DWORD br; BOOL ok = FALSE;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)buf;
        ok = (d->BusType == BusTypeUsb);
        if (ok && info) {
            char vendor[64] = "", product[64] = "";
            if (d->VendorIdOffset && d->VendorIdOffset < br) lstrcpynA(vendor, (char*)buf + d->VendorIdOffset, 64);
            if (d->ProductIdOffset && d->ProductIdOffset < br) lstrcpynA(product, (char*)buf + d->ProductIdOffset, 64);
            StrTrimA(vendor, " "); StrTrimA(product, " ");
            GET_LENGTH_INFORMATION len; len.Length.QuadPart = 0;
            DISK_GEOMETRY_EX geo;
            if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &br, NULL))
                len.Length = geo.DiskSize;
            info->number = number;
            info->size = len.Length.QuadPart;
            wchar_t sz[32]; fmtSize(info->size, sz, 32);
            swprintf(info->label, 96, L"%hs %hs (%s)", vendor, product, sz);
        }
    }
    CloseHandle(h);
    if (!ok) return FALSE;
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    if (diskNumberOfLetter(win[0]) == number) return FALSE;
    return TRUE;
}
static void usbRefresh(void) {
    g_usbCount = 0; g_usbSel = -1;
    for (int i = 0; i < 32 && g_usbCount < 8; i++) {
        UsbDisk d;
        if (usbDiskSafe(i, &d) && d.size > 0) g_usb[g_usbCount++] = d;
    }
    if (g_usbCount == 1) g_usbSel = 0;
}

// The script that erases the disk. It is built only from a validated number. A FAT32 partition
// (at most 32 GB, the most Windows will format) starts on both BIOS and UEFI PCs.
static void usbDiskpartScript(int number, wchar_t* out, int cch) {
    swprintf(out, cch,
        L"select disk %d\r\nclean\r\nconvert mbr\r\ncreate partition primary size=32000\r\n"
        L"select partition 1\r\nactive\r\nformat fs=fat32 quick label=\"WINSETUP\"\r\nassign\r\nexit\r\n", number);
}
static DWORD runHidden(const wchar_t* exe, wchar_t* cmdline) {
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return GetLastError();
    CloseHandle(pi.hThread);
    return waitExitCode(pi.hProcess);
}
// Runs in the elevated helper.
static DWORD usbWrite(const wchar_t* src, int number) {
    if (!usbDiskSafe(number, NULL)) return E_USB_NOTSAFE;
    unsigned long long total = 0;
    treeSize(src, &total);
    if (total > 31000ull * 1024 * 1024) return E_USB_TOOBIG;

    aioProg(0, ST_PARTITION);
    wchar_t tmpDir[MAX_PATH], script[MAX_PATH], body[512], sys[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH * 3];
    GetTempPathW(MAX_PATH, tmpDir);
    GetTempFileNameW(tmpDir, L"wcu", 0, script);
    usbDiskpartScript(number, body, 512);
    HANDLE f = CreateFileW(script, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    char ascii[512]; int n = WideCharToMultiByte(CP_ACP, 0, body, -1, ascii, 512, NULL, NULL) - 1; DWORD wr;
    WriteFile(f, ascii, n, &wr, NULL);
    CloseHandle(f);
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(exe, MAX_PATH, L"%s\\diskpart.exe", sys);
    swprintf(cmd, MAX_PATH * 3, L"\"%s\" /s \"%s\"", exe, script);
    DWORD dp = runHidden(exe, cmd);
    DeleteFileW(script);
    if (dp) return E_USB_DISKPART;

    wchar_t letter = 0;
    for (int tries = 0; tries < 100 && !letter; tries++) {
        DWORD mask = GetLogicalDrives();
        for (wchar_t d = L'C'; d <= L'Z' && !letter; d++)
            if ((mask >> (d - L'A')) & 1 && diskNumberOfLetter(d) == number) letter = d;
        if (!letter) Sleep(200);
    }
    if (!letter) return E_USB_NOLETTER;

    // FAT32 can't hold a file of 4 GB or more, so a big install.wim is split into .swm parts
    wchar_t wim[MAX_PATH * 2], dst[8];
    swprintf(dst, 8, L"%c:", letter);
    swprintf(wim, MAX_PATH * 2, L"%s\\sources\\install.wim", src);
    WIN32_FILE_ATTRIBUTE_DATA fa; BOOL split = FALSE; unsigned long long wimSize = 0;
    if (GetFileAttributesExW(wim, GetFileExInfoStandard, &fa)) {
        wimSize = fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow);
        split = wimSize >= 0xFFFFFFFFull;
    }
    CopyCtx c = {0};
    c.total = total - (split ? wimSize : 0); c.base = 5; c.span = split ? 50 : 90; c.stage = ST_COPY;
    wchar_t xsrc[MAX_PATH * 2 + 8], xdst[16];
    extendedPath(src, xsrc, MAX_PATH * 2 + 8);
    swprintf(xdst, 16, L"\\\\?\\%s", dst);
    DWORD r = copyTree(xsrc, xdst, &c, split ? L"sources\\install.wim" : NULL, L"");
    if (r) return r;
    if (split) {
        aioProg(55, ST_SPLIT);
        swprintf(exe, MAX_PATH, L"%s\\dism.exe", sys);
        swprintf(cmd, MAX_PATH * 3, L"\"%s\" /English /Split-Image /ImageFile:\"%s\" /SWMFile:\"%s\\sources\\install.swm\" /FileSize:3800",
                 exe, wim, dst);
        if (runHidden(exe, cmd)) return E_USB_SPLIT;
    }
    aioProg(96, ST_BOOTSECT);
    swprintf(exe, MAX_PATH, L"%s\\boot\\bootsect.exe", src);
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
        swprintf(cmd, MAX_PATH * 3, L"\"%s\" /nt60 %s /mbr", exe, dst);
        runHidden(exe, cmd);
    }
    aioProg(100, ST_BOOTSECT);
    return 0;
}

// ---------- worker ----------
static wchar_t g_jA[MAX_PATH], g_jB[MAX_PATH], g_jC[MAX_PATH], g_jD[MAX_PATH];
static wchar_t g_wimLoadList[8192];
static wchar_t g_jobHash[4][132];

static DWORD WINAPI aioWorker(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    DWORD r = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    switch (g_aioJob) {
    case JOB_WIMLOAD: {
        aioProg(-1, ST_LOADING);
        for (const wchar_t* p = g_wimLoadList; *p && !r; p += wcslen(p) + 1) r = wimReadFile(p);
        break;
    }
    case JOB_WIMSAVE:
        aioProg(-1, ST_SAVING);
        if (!wimLoadApi()) r = E_WIMGAPI;
        else r = g_jA[0] ? wimSaveAs(g_jA) : wimSaveInPlace();
        break;
    case JOB_ESD_BUILD:                   // every listed image, edits applied, ready for DISM to compress
        aioProg(0, ST_SAVING);
        if (!wimLoadApi()) r = E_WIMGAPI;
        else { g_wimStage = ST_SAVING; r = wimSaveAsRange(g_jC, 0, 30); }
        break;
    case JOB_FOLDER:
        r = mediaBuild(g_jB, g_jA, 0, 100);
        if (r) removeTreeOwn(g_jA);       // the folder was empty when chosen, so this removes only our files
        break;
    case JOB_ISO:                         // the folder is built beside the ISO and removed afterwards
        swprintf(g_jC, MAX_PATH, L"%s.media", g_jA);
        r = mediaBuild(g_jB, g_jC, 0, 70);
        if (!r) r = isoBuild(g_jC, L"WINDOWS", g_jA, 70, 30);
        removeTreeOwn(g_jC);
        break;
    case JOB_ESD_DISM:
    case JOB_USB:
        r = waitExitCode(helper);
        break;
    case JOB_ESDLOAD: aioProg(-1, ST_LOADING); r = esdReadList(g_jA); break;
    case JOB_F2I:  r = isoBuild(g_jA, g_jC, g_jB, 0, 100); break;
    case JOB_I2F:  r = isoExtract(g_jA, g_jB, 0, 100); break;
    case JOB_HASH: r = hashFile(g_jA, g_jobHash); break;
    }
    CoUninitialize();
    PostMessageW(g_hwnd, WM_APP_AIODONE, (WPARAM)r, 0);
    return 0;
}
static BOOL aioStartJob(int job, HANDLE helper, BOOL canStop) {
    if (!jobBegin()) { if (helper) CloseHandle(helper); return FALSE; }
    g_aioJob = job; g_aioJobTab = g_aioTab;
    g_aioRunning = TRUE; g_aioPct = -1; g_aioStage = ST_NONE; g_aioStop = 0; g_aioCanStop = canStop;
    g_aioNotify = g_hwnd;
    g_aioMsg[g_aioTab][0] = 0;
    startWorker(aioWorker, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}
static void aioErrorText(DWORD code, wchar_t* out, int cch) {
    const wchar_t* t = NULL;
    switch (code) {
    case E_USB_NOTSAFE:  t = L"That drive isn't a USB drive, or it holds Windows, so it was not erased."; break;
    case E_USB_DISKPART: t = L"Windows couldn't prepare the USB drive. Try another port or drive."; break;
    case E_USB_NOLETTER: t = L"The USB drive was prepared but didn't get a drive letter."; break;
    case E_USB_TOOBIG:   t = L"The setup folder is larger than the 32 GB FAT32 partition allows."; break;
    case E_USB_SPLIT:    t = L"DISM couldn't split install.wim to fit on the USB drive."; break;
    case E_ISO_NOLETTER: t = L"The ISO opened but didn't get a drive letter. If it's already mounted, eject it and try again."; break;
    case E_WIMGAPI:      t = L"Windows' image library (wimgapi.dll) couldn't be loaded."; break;
    case E_ESD_COUNT:    t = L"That file holds no Windows editions."; break;
    }
    if (t) { swprintf(out, cch, L"%s", t); return; }
    if (g_dispErr[0]) { swprintf(out, cch, L"%s\n\nError 0x%08lX.", g_dispErr, code); g_dispErr[0] = 0; return; }
    wchar_t sys[256] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, sys, 256, NULL);
    swprintf(out, cch, L"%s\n\nError 0x%08lX.", sys[0] ? sys : L"The operation failed.", code);
}

