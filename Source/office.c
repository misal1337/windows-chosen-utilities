// MS Office 365 page: download Microsoft 365 Apps (x64) for one language straight from Microsoft's Office CDN into a
// folder you choose, then install from that folder, or from any Office source folder made the same way.
//
// The newest build per channel comes from Microsoft's release service, and the file list for an x64 single-language
// source follows abbodi1406's YAOCTRU (BatUtil), used as a reference only: it states no licence, so nothing is copied.
// Every .cab in a source is checked for Microsoft's signature; Office's installer checks the rest itself. Installing
// uses Microsoft's Office Deployment Tool (setup.exe /configure), downloaded fresh and signature-checked each time.

// Download and install offer Current and Beta; switching an installed Office also offers Dev Main (Microsoft's
// internal daily channel, "DogfoodDevMain" in YAOCTRU's list), which ODT doesn't name.
enum { OFF_CURRENT, OFF_BETA, OFF_DEVMAIN, OFF_NCH };
static const struct { const wchar_t* label; const wchar_t* odt; const wchar_t* ffn; } OFF_CH[OFF_NCH] = {
    { L"Current Channel",  L"Current",     L"492350f6-3a01-4f97-b9c0-c7c6ddf67d60" },
    { L"Beta Channel",     L"BetaChannel", L"5440fd1f-7ecb-4221-8110-145efaa6372f" },
    { L"Dev Main Channel", L"DevMain",     L"ea4a4090-de26-49d7-93c1-91bff9e53fc3" },
};
static int g_offInstCh = -1;              // the installed Office's update channel, or -1
// the one edition offered (the user's rule): Microsoft 365 Family / Personal
#define OFF_ED_LABEL L"Microsoft 365 Family / Personal"
#define OFF_ED_ID    L"O365HomePremRetail"
// apps that can be chosen, with the Office Deployment Tool's ID for leaving each out
#define OFF_NAPPS 11
static const struct { const wchar_t* name; const wchar_t* id; BOOL on; } OFF_APPS[OFF_NAPPS] = {
    { L"Word", L"Word", TRUE }, { L"Excel", L"Excel", TRUE }, { L"PowerPoint", L"PowerPoint", TRUE },
    { L"Outlook (classic)", L"Outlook", FALSE }, { L"Outlook (new)", L"OutlookForWindows", FALSE },
    { L"OneNote", L"OneNote", FALSE }, { L"Access", L"Access", FALSE }, { L"Publisher", L"Publisher", FALSE },
    { L"OneDrive", L"OneDrive", FALSE }, { L"Teams", L"Teams", FALSE }, { L"Skype for Business", L"Lync", FALSE },
};
// the languages Office offers (YAOCTRU's list)
static const wchar_t* OFF_LANGS[] = {
    L"en-US", L"en-GB", L"ar-SA", L"bg-BG", L"cs-CZ", L"da-DK", L"de-DE", L"el-GR", L"es-ES", L"es-MX", L"et-EE", L"fi-FI",
    L"fr-FR", L"fr-CA", L"he-IL", L"hi-IN", L"hr-HR", L"hu-HU", L"id-ID", L"it-IT", L"ja-JP", L"kk-KZ", L"ko-KR", L"lt-LT",
    L"lv-LV", L"ms-MY", L"nb-NO", L"nl-NL", L"pl-PL", L"pt-BR", L"pt-PT", L"ro-RO", L"ru-RU", L"sk-SK", L"sl-SI",
    L"sr-Latn-RS", L"sv-SE", L"th-TH", L"tr-TR", L"uk-UA", L"vi-VN", L"zh-CN", L"zh-TW" };
#define OFF_NLANGS (int)(sizeof(OFF_LANGS) / sizeof(OFF_LANGS[0]))
#define OFF_CDN L"https://officecdn.microsoft.com/db/%s/Office/Data"
#define OFF_ODT L"https://officecdn.microsoft.com/pr/wsus/setup.exe"

static int  g_offCh = OFF_CURRENT, g_offLang = -1;
static wchar_t g_offLatest[OFF_NCH][40];
static wchar_t g_offInstVer[40];          // the installed build, or empty
static volatile LONG g_offChecking = 0;
static wchar_t g_offInstalled[200];       // what's installed now, for the header

// the source to install from
static wchar_t g_offSrc[MAX_PATH];
static wchar_t g_offSrcInfo[300];
static wchar_t g_offSrcVer[40], g_offSrcLangs[200];
static int  g_offSrcCh = -1;              // the channel it was downloaded from, if known
static int  g_offICh = OFF_CURRENT;       // update channel for the install
static BOOL g_offIApps[OFF_NAPPS];

// the running job
enum { OFFJ_NONE, OFFJ_DOWNLOAD, OFFJ_INSTALL, OFFJ_SWITCH };
static int  g_offJob = OFFJ_NONE;
static volatile LONG g_offCancel = 0;
static unsigned long long g_offGot = 0, g_offTotal = 0;
static wchar_t g_offMsg[400];
static ULONGLONG g_offStarted = 0;

// ---------- small helpers ----------
static void offLangName(const wchar_t* code, wchar_t* out, int cch) {
    if (!GetLocaleInfoEx(code, LOCALE_SLOCALIZEDDISPLAYNAME, out, cch)) lstrcpynW(out, code, cch);
}
static int offLangIndex(const wchar_t* code) {
    for (int i = 0; i < OFF_NLANGS; i++) if (!_wcsicmp(OFF_LANGS[i], code)) return i;
    return -1;
}
static void offAppsLabel(const BOOL* on, wchar_t* out, int cch) {
    out[0] = 0;
    int n = 0;
    for (int i = 0; i < OFF_NAPPS; i++) if (on[i]) n++;
    if (n == 0) { lstrcpynW(out, L"No apps chosen", cch); return; }
    if (n > 4) { swprintf(out, cch, L"%d apps", n); return; }
    for (int i = 0; i < OFF_NAPPS; i++) if (on[i]) {
        if (out[0]) wcsncat(out, L", ", cch - wcslen(out) - 1);
        wcsncat(out, OFF_APPS[i].name, cch - wcslen(out) - 1);
    }
}
static int offChannelOfFfn(const wchar_t* text) {
    for (int i = 0; i < OFF_NCH; i++) if (text && wcsstr(text, OFF_CH[i].ffn)) return i;
    return -1;
}

// What's installed: Click-to-Run's own record
static void offReadInstalled(void) {
    wchar_t ver[40] = L"", ids[200] = L"", cdn[300] = L"";
    DWORD cb;
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration";
    cb = sizeof(ver); RegGetValueW(HKEY_LOCAL_MACHINE, key, L"VersionToReport", RRF_RT_REG_SZ, NULL, ver, &cb);
    cb = sizeof(ids); RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProductReleaseIds", RRF_RT_REG_SZ, NULL, ids, &cb);
    cb = sizeof(cdn);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UpdateChannel", RRF_RT_REG_SZ, NULL, cdn, &cb) != ERROR_SUCCESS || !cdn[0]) {
        cb = sizeof(cdn); RegGetValueW(HKEY_LOCAL_MACHINE, key, L"CDNBaseUrl", RRF_RT_REG_SZ, NULL, cdn, &cb);
    }
    g_offInstCh = -1;
    g_offInstVer[0] = 0;
#ifdef WCU_PREVIEW_OFFICE           // a layout preview build only: pretend Office is installed
    wcscpy(ver, L"16.0.20701.20002"); wcscpy(ids, L"O365HomePremRetail"); swprintf(cdn, 300, L"http://officecdn.microsoft.com/pr/%s", OFF_CH[OFF_BETA].ffn);
#endif
    if (!ver[0]) { wcscpy(g_offInstalled, L"Office isn't installed"); return; }
    lstrcpynW(g_offInstVer, ver, 40);
    g_offInstCh = offChannelOfFfn(cdn);
    const wchar_t* edName = wcsstr(ids, OFF_ED_ID) ? OFF_ED_LABEL : ids;   // another product shows by its ID
    int ch = g_offInstCh;
    swprintf(g_offInstalled, 200, L"Installed: %s %s%s%s", edName, ver, ch >= 0 ? L", " : L"", ch >= 0 ? OFF_CH[ch].label : L"");
}

// Newest build of a channel, from Microsoft's release service (as Office's own updater sees it)
static BOOL offLatest(int ch, wchar_t* out, int cch) {
    wchar_t url[300]; char* body = NULL; DWORD status = 0;
    swprintf(url, 300, L"https://mrodevicemgr.officeapps.live.com/mrodevicemgrsvc/api/v2/C2RReleaseData?audienceFFN=%s", OFF_CH[ch].ffn);
    BOOL ok = !dlHttp(url, &body, NULL, &status) && status == 200 && appsJsonVersion(body, "\"AvailableBuild\"", out, cch);
    free(body);
    return ok;
}

static DWORD WINAPI offLatestProc(LPVOID arg) {
    (void)arg;
    wchar_t* list = (wchar_t*)calloc(OFF_NCH * 40, sizeof(wchar_t));
    if (list) for (int i = 0; i < OFF_NCH; i++) offLatest(i, list + i * 40, 40);
    if (!list || !PostMessageW(g_hwnd, WM_APP_OFFLATEST, 0, (LPARAM)list)) { free(list); InterlockedExchange(&g_offChecking, 0); }
    return 0;
}
static void offOnLatest(wchar_t* list) {
    for (int i = 0; i < OFF_NCH; i++) if (list[i * 40]) lstrcpynW(g_offLatest[i], list + i * 40, 40);
    free(list);
    InterlockedExchange(&g_offChecking, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Entering the page
static void offSetSource(const wchar_t* path);
static void offRefresh(void) {
    static BOOL first = TRUE;
    if (first) {
        first = FALSE;
        for (int i = 0; i < OFF_NAPPS; i++) g_offIApps[i] = OFF_APPS[i].on;
#ifdef WCU_PREVIEW_LOG              // a layout preview build only: long lines in the Progress log
        for (int i = 1; i <= 12; i++) appsSay(L"Earlier entry %d", i);
        appsLogAdd(L"> Switch Office to Beta Channel", FALSE);
        appsLogAdd(L"Office changes from build 16.0.20527.20150 to build 16.0.20701.20002 once the switch and its update have finished.", FALSE);
        appsLogAdd(L"Update channel set to Beta Channel (http://officecdn.microsoft.com/pr/5440fd1f-7ecb-4221-8110-145efaa6372f).", FALSE);
        appsLogAdd(L"Started Office's updater: it downloads the Beta Channel build in its own window.", FALSE);
        appsLogAdd(L"Office now updates from Beta Channel. Its updater is fetching the build; this log says when it has finished.", FALSE);
        appsLogAdd(L"Office's update finished: build 16.0.20527.20150 → build 16.0.20701.20002.", FALSE);
#endif
        wchar_t ui[LOCALE_NAME_MAX_LENGTH];
        g_offLang = GetUserDefaultLocaleName(ui, LOCALE_NAME_MAX_LENGTH) ? offLangIndex(ui) : -1;
        if (g_offLang < 0) g_offLang = 0;
        wchar_t last[MAX_PATH]; DWORD cb = sizeof(last);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WindowsChosenUtilities", L"OfficeSource", RRF_RT_REG_SZ, NULL, last, &cb) == ERROR_SUCCESS
            && isFolder(last)) offSetSource(last);
    }
    if (g_offJob == OFFJ_NONE) offReadInstalled();
    if (InterlockedCompareExchange(&g_offChecking, 1, 0) == 0) {
        HANDLE t = CreateThread(NULL, 0, offLatestProc, NULL, 0, NULL);
        if (t) CloseHandle(t); else InterlockedExchange(&g_offChecking, 0);
    }
}

// ---------- reading a source folder ----------
// A source is a folder holding Office\Data\<version>\stream.x64.x-none.dat. Fills version and languages.
static BOOL offReadSource(const wchar_t* src, wchar_t* ver, int vcch, wchar_t* langs, int lcch, int* ch) {
    wchar_t pat[MAX_PATH + 40], best[40] = L"";
    ver[0] = 0; langs[0] = 0; *ch = -1;
    if (!src[0] || wcschr(src, L'"') || swprintf(pat, MAX_PATH + 40, L"%s\\Office\\Data\\16.0.*", src) < 0) return FALSE;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        BOOL plain = TRUE;
        for (const wchar_t* c = fd.cFileName; *c; c++) if (!iswdigit(*c) && *c != L'.') plain = FALSE;
        wchar_t base[MAX_PATH + 80];
        swprintf(base, MAX_PATH + 80, L"%s\\Office\\Data\\%s\\stream.x64.x-none.dat", src, fd.cFileName);
        if (plain && isFile(base) && (!best[0] || appsVerCmp(fd.cFileName, best) > 0)) lstrcpynW(best, fd.cFileName, 40);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best[0]) return FALSE;
    lstrcpynW(ver, best, vcch);
    swprintf(pat, MAX_PATH + 40, L"%s\\Office\\Data\\%s\\stream.x64.*.dat", src, best);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t l[40]; const wchar_t* p = fd.cFileName + 11;        // after "stream.x64."
            int n = 0;
            while (p[n] && p[n] != L'.' && n < 39) { l[n] = p[n]; n++; }
            l[n] = 0;
            if (!_wcsicmp(p + n, L".dat") && _wcsicmp(l, L"x-none") && offLangIndex(l) >= 0) {
                if (langs[0]) wcsncat(langs, L",", lcch - wcslen(langs) - 1);
                wcsncat(langs, OFF_LANGS[offLangIndex(l)], lcch - wcslen(langs) - 1);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!langs[0]) return FALSE;
    wchar_t mk[MAX_PATH + 40]; char* t;
    swprintf(mk, MAX_PATH + 40, L"%s\\WCU_Office.txt", src);
    if ((t = dlReadSmallFile(mk, 4096)) != NULL) {
        wchar_t w[4096]; MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 4096);
        *ch = offChannelOfFfn(w);
        free(t);
    }
    return TRUE;
}

static void offSetSource(const wchar_t* path) {
    int ch;
    lstrcpynW(g_offSrc, path, MAX_PATH);
    // remembered, so a source downloaded earlier is still chosen next time
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\WindowsChosenUtilities", L"OfficeSource", REG_SZ, g_offSrc,
                    (DWORD)((wcslen(g_offSrc) + 1) * sizeof(wchar_t)));
    if (!offReadSource(g_offSrc, g_offSrcVer, 40, g_offSrcLangs, 200, &ch)) {
        g_offSrcVer[0] = 0;
        swprintf(g_offSrcInfo, 300, L"No x64 Office source in this folder. Choose the folder that holds the \"Office\" folder.");
        g_offSrcCh = -1;
    } else {
        g_offSrcCh = ch;
        if (ch >= 0) g_offICh = ch;
        swprintf(g_offSrcInfo, 300, L"%s · x64 · %s%s%s", g_offSrcVer, g_offSrcLangs, ch >= 0 ? L" · " : L"", ch >= 0 ? OFF_CH[ch].label : L"");
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- downloading (in the window's process, as the user) ----------
typedef struct { wchar_t rel[120]; unsigned long long size; } OffFile;

static BOOL offHeadSize(const wchar_t* url, unsigned long long* size) {
    URL_COMPONENTS u; memset(&u, 0, sizeof(u));
    wchar_t host[256], path[1024];
    u.dwStructSize = sizeof(u); u.lpszHostName = host; u.dwHostNameLength = 256; u.lpszUrlPath = path; u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) return FALSE;
    BOOL ok = FALSE;
    HINTERNET s = WinHttpOpen(L"WindowsChosenUtilities", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"HEAD", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD status = 0, cb = sizeof(status);
        wchar_t len[32]; DWORD lb = sizeof(len);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX)
            && status == 200 && WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lb, WINHTTP_NO_HEADER_INDEX)) {
            *size = _wcstoui64(len, NULL, 10);
            ok = *size > 0;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

typedef struct { unsigned long long base; ULONGLONG lastPost; } OffTick;
static BOOL offDlTick(unsigned long long got, void* ctx) {
    OffTick* t = (OffTick*)ctx;
    g_offGot = t->base + got;
    ULONGLONG now = GetTickCount64();
    if (now - t->lastPost > 500) {
        t->lastPost = now;
        appsSayOver(L"Downloading… %.0f of %.0f MB (%d%%)", g_offGot / 1048576.0, g_offTotal / 1048576.0,
                    g_offTotal ? (int)(g_offGot * 100 / g_offTotal) : 0);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    return !g_offCancel;
}

typedef struct { int ch; wchar_t lang[16]; wchar_t dest[MAX_PATH]; } OffDlJob;

static DWORD offDownload(OffDlJob* j) {
    wchar_t ver[40];
    appsSay(L"Asking Microsoft for the newest %s build…", OFF_CH[j->ch].label);
    if (!offLatest(j->ch, ver, 40)) { appsSay(L"Couldn't reach Microsoft's release service."); return ERROR_NOT_FOUND; }
    appsSay(L"Newest: %s (x64, %s)", ver, j->lang);
    wchar_t lang[16]; lstrcpynW(lang, j->lang, 16);
    LCID lcid = LocaleNameToLCID(lang, 0);
    if (!lcid) { appsSay(L"Unknown language %s.", lang); return ERROR_INVALID_PARAMETER; }
    wchar_t low[16]; lstrcpynW(low, lang, 16); ucLower(low);

    OffFile f[8]; int n = 0;
    swprintf(f[n++].rel, 120, L"v64_%s.cab", ver);
    swprintf(f[n++].rel, 120, L"%s\\i640.cab", ver);
    swprintf(f[n++].rel, 120, L"%s\\s640.cab", ver);
    swprintf(f[n++].rel, 120, L"%s\\i64%lu.cab", ver, (unsigned long)lcid);
    swprintf(f[n++].rel, 120, L"%s\\s64%lu.cab", ver, (unsigned long)lcid);
    swprintf(f[n++].rel, 120, L"%s\\stream.x64.%s.dat", ver, low);
    swprintf(f[n++].rel, 120, L"%s\\stream.x64.x-none.dat", ver);

    wchar_t base[200]; swprintf(base, 200, OFF_CDN, OFF_CH[j->ch].ffn);
    g_offTotal = 0; g_offGot = 0;
    for (int i = 0; i < n; i++) {
        wchar_t url[400], rel[120];
        lstrcpynW(rel, f[i].rel, 120);
        for (wchar_t* c = rel; *c; c++) if (*c == L'\\') *c = L'/';
        swprintf(url, 400, L"%s/%s", base, rel);
        if (!offHeadSize(url, &f[i].size)) { appsSay(L"Microsoft's CDN doesn't have %s.", f[i].rel); return ERROR_FILE_NOT_FOUND; }
        g_offTotal += f[i].size;
    }
    appsSay(L"%d files, %.2f GB, into %s\\Office", n, g_offTotal / 1073741824.0, j->dest);
    ULARGE_INTEGER freeB;
    if (GetDiskFreeSpaceExW(j->dest, &freeB, NULL, NULL) && freeB.QuadPart < g_offTotal + (200ull << 20)) {
        appsSay(L"Not enough free space: %.2f GB free, %.2f GB needed.", freeB.QuadPart / 1073741824.0, g_offTotal / 1073741824.0);
        return ERROR_DISK_FULL;
    }
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%s\\Office", j->dest); CreateDirectoryW(dir, NULL);
    swprintf(dir, MAX_PATH, L"%s\\Office\\Data", j->dest); CreateDirectoryW(dir, NULL);
    swprintf(dir, MAX_PATH, L"%s\\Office\\Data\\%s", j->dest, ver); CreateDirectoryW(dir, NULL);

    appsSay(L"Downloading…");
    for (int i = 0; i < n; i++) {
        wchar_t url[400], rel[120], path[MAX_PATH], part[MAX_PATH];
        lstrcpynW(rel, f[i].rel, 120);
        for (wchar_t* c = rel; *c; c++) if (*c == L'\\') *c = L'/';
        swprintf(url, 400, L"%s/%s", base, rel);
        swprintf(path, MAX_PATH, L"%s\\Office\\Data\\%s", j->dest, f[i].rel);
        swprintf(part, MAX_PATH, L"%s.part", path);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) == f[i].size) {
            g_offGot += f[i].size;                        // already here from an earlier run
            continue;
        }
        OffTick t = { g_offGot, 0 };
        DWORD status = 0;
        DWORD e = dlHttpCore(url, NULL, part, &status, FALSE, offDlTick, &t);
        if (g_offCancel) { DeleteFileW(part); appsSay(L"Cancelled. Files already finished stay, so Download carries on from there."); return ERROR_CANCELLED; }
        if (e || status != 200) { DeleteFileW(part); appsSay(L"Download of %s failed (%s %lu).", f[i].rel, e ? L"error" : L"HTTP", e ? e : status); return e ? e : ERROR_INVALID_DATA; }
        if (!GetFileAttributesExW(part, GetFileExInfoStandard, &fa) || fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) != f[i].size) {
            DeleteFileW(part); appsSay(L"%s came out the wrong size.", f[i].rel); return ERROR_INVALID_DATA;
        }
        if (!MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) { e = GetLastError(); appsSay(L"Couldn't save %s (error %lu).", f[i].rel, e); return e; }
        g_offGot = t.base + f[i].size;
    }
    appsSayOver(L"Downloaded %.2f GB.", g_offTotal / 1073741824.0);
    // Office keeps a copy of the version cab under its plain name too
    wchar_t v1[MAX_PATH], v2[MAX_PATH];
    swprintf(v1, MAX_PATH, L"%s\\Office\\Data\\v64_%s.cab", j->dest, ver);
    swprintf(v2, MAX_PATH, L"%s\\Office\\Data\\v64.cab", j->dest);
    CopyFileW(v1, v2, FALSE);
    // every cab must be Microsoft's
    for (int i = 0; i < n; i++) {
        if (_wcsicmp(PathFindExtensionW(f[i].rel), L".cab")) continue;
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\Office\\Data\\%s", j->dest, f[i].rel);
        if (appsCheckSigner(path, L"Microsoft Corporation") != S_OK) {
            DeleteFileW(path);
            appsSay(L"%s isn't signed by Microsoft, so it was deleted.", f[i].rel);
            return (DWORD)TRUST_E_NOSIGNATURE;
        }
    }
    appsSay(L"Signatures checked: every cab is signed by Microsoft Corporation");
    wchar_t mk[MAX_PATH];
    swprintf(mk, MAX_PATH, L"%s\\WCU_Office.txt", j->dest);
    HANDLE h = CreateFileW(mk, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char t[400]; DWORD w;
        int len = snprintf(t, sizeof(t), "Microsoft 365 Apps source made by Windows Chosen Utilities\r\nversion=%ls\r\narch=x64\r\nlanguage=%ls\r\n"
                                         "channel=%ls\r\naudienceFFN=%ls\r\n", ver, lang, OFF_CH[j->ch].odt, OFF_CH[j->ch].ffn);
        WriteFile(h, t, (DWORD)len, &w, NULL);
        CloseHandle(h);
    }
    return 0;
}

static DWORD WINAPI offDownloadProc(LPVOID arg) {
    OffDlJob* j = (OffDlJob*)arg;
    DWORD e = offDownload(j);
    if (!e) {
        wchar_t* p = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
        if (p) { lstrcpynW(p, j->dest, MAX_PATH); if (!PostMessageW(g_hwnd, WM_APP_OFFDONE, 0, (LPARAM)p)) free(p); }
        else PostMessageW(g_hwnd, WM_APP_OFFDONE, 0, 0);
    } else PostMessageW(g_hwnd, WM_APP_OFFDONE, e, 0);
    free(j);
    return 0;
}

static BOOL offLogBegin(const wchar_t* head) {
    if (!jobBegin()) return FALSE;
    InterlockedExchange(&g_logOpen, 1);
    if (g_appsLogN) appsLogAdd(L"", FALSE);
    appsLogAdd(head, FALSE);
    g_offMsg[0] = 0;
    g_offStarted = GetTickCount64();
    return TRUE;
}

static void offDownloadClick(void) {
    if (g_busy != 0) return;
    wchar_t dest[MAX_PATH] = L"";
    if (!pickFolder(dest)) return;
    if (wcschr(dest, L'"') || wcslen(dest) > MAX_PATH - 80) { okDialog(L"Choose another folder", L"That folder's path is too long or unusual for Office's files."); return; }
    OffDlJob* j = (OffDlJob*)calloc(1, sizeof(OffDlJob));
    if (!j) return;
    j->ch = g_offCh; lstrcpynW(j->lang, OFF_LANGS[g_offLang], 16); lstrcpynW(j->dest, dest, MAX_PATH);
    wchar_t head[300];
    swprintf(head, 300, L"> Download Microsoft 365 Apps, %s, x64, %s", OFF_CH[g_offCh].label, j->lang);
    if (!offLogBegin(head)) { free(j); return; }
    g_offJob = OFFJ_DOWNLOAD; g_offCancel = 0; g_offGot = g_offTotal = 0;
    startWorker(offDownloadProc, j);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- installing (the elevated helper) ----------
static void offXml(const wchar_t* s, wchar_t* out, int cch) {
    int n = 0;
    for (; *s && n < cch - 7; s++) {
        const wchar_t* r = *s == L'&' ? L"&amp;" : *s == L'<' ? L"&lt;" : *s == L'>' ? L"&gt;" : *s == L'\'' ? L"&apos;" : NULL;
        if (r) { wcscpy(out + n, r); n += (int)wcslen(r); } else out[n++] = *s;
    }
    out[n] = 0;
}

// Points Click-to-Run at a channel's Microsoft address. Click-to-Run takes the channel it checks from CDNBaseUrl (seen in
// testing 2026-11-09: with only UpdateChannel changed it cleared UpdateChannelChanged but kept checking the old channel).
// This is also what Microsoft's own "OfficeC2RClient.exe /changesetting Channel=..." changes, and it works for Dev Main,
// which neither that command nor the Office Deployment Tool can name. Anything pinning a version is cleared.
static DWORD offSetChannelReg(int ch) {
    const wchar_t* cfg = L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration";
    wchar_t url[160];
    swprintf(url, 160, L"http://officecdn.microsoft.com/pr/%s", OFF_CH[ch].ffn);
    HKEY k;
    LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, cfg, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
    if (e != ERROR_SUCCESS) { appsSay(L"Couldn't open Click-to-Run's settings (error %ld).", e); return (DWORD)e; }
    e = RegSetValueExW(k, L"UpdateChannel", 0, REG_SZ, (const BYTE*)url, (DWORD)(wcslen(url) + 1) * 2);
    if (e == ERROR_SUCCESS) e = RegSetValueExW(k, L"CDNBaseUrl", 0, REG_SZ, (const BYTE*)url, (DWORD)(wcslen(url) + 1) * 2);
    if (e == ERROR_SUCCESS) e = RegSetValueExW(k, L"UpdateChannelChanged", 0, REG_SZ, (const BYTE*)L"True", 5 * 2);
    RegDeleteValueW(k, L"UpdateUrl");
    RegDeleteValueW(k, L"UpdateToVersion");
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) { appsSay(L"Couldn't change the channel (error %ld).", e); return (DWORD)e; }
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Updates", L"UpdateToVersion");
    appsSay(L"Update channel set to %s (%s).", OFF_CH[ch].label, url);
    return 0;
}

// --office-install="<source>" --office-opt=<update channel>,<apps bit mask>
static int offHelperMain(const wchar_t* cmd) {
    g_appsNotify = cmdNotify(cmd);
    wchar_t src[MAX_PATH];
    const wchar_t* p = wcsstr(cmd, L"--office-install=\"");
    if (!p) return ERROR_INVALID_PARAMETER;
    p = cmdQuoted(p + 18, src, MAX_PATH);
    const wchar_t* o = *p ? wcsstr(p, L"--office-opt=") : NULL;
    if (!o) return ERROR_INVALID_PARAMETER;
    int ch = -1; unsigned mask = 0;
    if (swscanf(o + 13, L"%d,%u", &ch, &mask) != 2 || ch < 0 || ch >= OFF_NCH || !mask) return ERROR_INVALID_PARAMETER;

    wchar_t ver[40], langs[200]; int srcCh;
    if (!offReadSource(src, ver, 40, langs, 200, &srcCh)) { appsSay(L"No x64 Office source found in %s.", src); return ERROR_PATH_NOT_FOUND; }
    appsSay(L"Source: %s, version %s, %s", src, ver, langs);

    // Microsoft's Office Deployment Tool and its configuration, in a folder only administrators can write
    wchar_t dir[MAX_PATH], odt[MAX_PATH], xml[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"\\Temp\\WCU_Office_%lu_%llu", GetCurrentProcessId(), GetTickCount64());
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
        return GetLastError();
    BOOL made = CreateDirectoryW(dir, &sa);
    DWORD e = made ? 0 : GetLastError();
    LocalFree(sa.lpSecurityDescriptor);
    if (!made) { appsSay(L"Couldn't make a work folder (error %lu).", e); return e; }
    swprintf(odt, MAX_PATH, L"%s\\setup.exe", dir);
    swprintf(xml, MAX_PATH, L"%s\\config.xml", dir);

    DWORD result = 0;
    appsSay(L"Getting the Office Deployment Tool from Microsoft…");
    DWORD status = 0;
    e = dlHttp(OFF_ODT, NULL, odt, &status);
    if (e || status != 200) { appsSay(L"Couldn't download it (%s %lu).", e ? L"error" : L"HTTP", e ? e : status); result = e ? e : ERROR_INVALID_DATA; }
    else if (appsCheckSigner(odt, L"Microsoft Corporation") != S_OK) { appsSay(L"It isn't signed by Microsoft, so nothing was installed."); result = (DWORD)TRUST_E_NOSIGNATURE; }
    else appsSay(L"Signature checked: signed by Microsoft Corporation");

    if (!result) {
        wchar_t esrc[MAX_PATH * 2], body[6000], line[200];
        offXml(src, esrc, MAX_PATH * 2);
        // The Office Deployment Tool has no name for Dev Main, so its channel is left out there and set after installing
        BOOL named = srcCh >= 0 && srcCh != OFF_DEVMAIN;
        int n = swprintf(body, 6000, L"<Configuration>\r\n  <Add OfficeClientEdition=\"64\" SourcePath=\"%s\" Version=\"%s\"%s%s%s AllowCdnFallback=\"FALSE\">\r\n"
                                     L"    <Product ID=\"%s\">\r\n", esrc, ver,
                         named ? L" Channel=\"" : L"", named ? OFF_CH[srcCh].odt : L"", named ? L"\"" : L"", OFF_ED_ID);
        wchar_t lc[200]; lstrcpynW(lc, langs, 200);
        wchar_t* ctx = NULL;
        for (wchar_t* t = wcstok(lc, L",", &ctx); t; t = wcstok(NULL, L",", &ctx)) {
            swprintf(line, 200, L"      <Language ID=\"%s\" />\r\n", t);
            wcsncat(body, line, 6000 - wcslen(body) - 1);
        }
        for (int i = 0; i < OFF_NAPPS; i++) if (!(mask & (1u << i))) {
            swprintf(line, 200, L"      <ExcludeApp ID=\"%s\" />\r\n", OFF_APPS[i].id);
            wcsncat(body, line, 6000 - wcslen(body) - 1);
        }
        (void)n;
        // Updates come from Microsoft's servers for the chosen channel. Without UpdatePath, Office would keep looking for
        // updates in the source folder it was installed from (seen in testing: UpdateChannel was set to that folder).
        if (ch == OFF_DEVMAIN)
            swprintf(line, 200, L"    </Product>\r\n  </Add>\r\n  <Updates Enabled=\"TRUE\" UpdatePath=\"http://officecdn.microsoft.com/pr/%s\" />\r\n", OFF_CH[ch].ffn);
        else
            swprintf(line, 200, L"    </Product>\r\n  </Add>\r\n  <Updates Enabled=\"TRUE\" Channel=\"%s\" UpdatePath=\"http://officecdn.microsoft.com/pr/%s\" />\r\n",
                     OFF_CH[ch].odt, OFF_CH[ch].ffn);
        wcsncat(body, line, 6000 - wcslen(body) - 1);
        wcsncat(body, L"  <Display Level=\"None\" AcceptEULA=\"TRUE\" />\r\n  <Property Name=\"FORCEAPPSHUTDOWN\" Value=\"TRUE\" />\r\n", 6000 - wcslen(body) - 1);
        wchar_t edir[MAX_PATH * 2]; offXml(dir, edir, MAX_PATH * 2);
        swprintf(line, 200, L"  <Logging Level=\"Standard\" Path=\"%s\" />\r\n</Configuration>\r\n", edir);
        wcsncat(body, line, 6000 - wcslen(body) - 1);
        char u8[12000];
        int len = WideCharToMultiByte(CP_UTF8, 0, body, -1, u8, sizeof(u8), NULL, NULL);
        HANDLE h = CreateFileW(xml, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD w = 0;
        if (h == INVALID_HANDLE_VALUE || len <= 1 || !WriteFile(h, u8, (DWORD)(len - 1), &w, NULL)) result = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        wchar_t apps[300] = L"";
        for (int i = 0; i < OFF_NAPPS; i++) if (mask & (1u << i)) { if (apps[0]) wcscat(apps, L", "); wcscat(apps, OFF_APPS[i].name); }
        appsSay(L"%s with %s; updates from %s", OFF_ED_LABEL, apps, OFF_CH[ch].label);
    }
    if (!result) {
        wchar_t cl[MAX_PATH * 2 + 40];
        swprintf(cl, MAX_PATH * 2 + 40, L"\"%s\" /configure \"%s\"", odt, xml);
        appsSay(L"Running: setup.exe /configure config.xml (open Office apps are closed)");
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (!CreateProcessW(odt, cl, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) result = GetLastError();
        else {
            CloseHandle(pi.hThread);
            ULONGLONG t0 = GetTickCount64();
            appsSay(L"Installing…");
            while (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) {
                ULONGLONG s = (GetTickCount64() - t0) / 1000;
                appsSayOver(L"Installing… %llu:%02llu", s / 60, s % 60);
            }
            DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            ULONGLONG s = (GetTickCount64() - t0) / 1000;
            appsSayOver(L"Office Deployment Tool finished in %llu:%02llu, exit code %lu.", s / 60, s % 60, code);
            result = code;
            // make sure updates come from the chosen channel (the only way to choose Dev Main)
            if (!code) offSetChannelReg(ch);
            if (code) {                                  // the errors from its log
                wchar_t pat[MAX_PATH + 8]; swprintf(pat, MAX_PATH + 8, L"%s\\*.log", dir);
                WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    wchar_t lp[MAX_PATH]; swprintf(lp, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                    FindClose(h);
                    char* t = dlReadSmallFile(lp, 8u << 20);
                    int shown = 0;
                    for (char* q = t ? strtok(t, "\r\n") : NULL; q && shown < 6; q = strtok(NULL, "\r\n"))
                        if (strstr(q, "Error") || strstr(q, "error code")) {
                            wchar_t wl[APPS_LOG_LINE]; MultiByteToWideChar(CP_UTF8, 0, q, -1, wl, APPS_LOG_LINE);
                            appsSay(L"  %s", wl); shown++;
                        }
                    free(t);
                }
            }
        }
    }
    DeleteFileW(odt); DeleteFileW(xml);
    {   // its logs
        wchar_t pat[MAX_PATH + 8]; swprintf(pat, MAX_PATH + 8, L"%s\\*", dir);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { wchar_t f[MAX_PATH]; if (!isDotName(fd.cFileName) && swprintf(f, MAX_PATH, L"%s\\%s", dir, fd.cFileName) > 0) DeleteFileW(f); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    RemoveDirectoryW(dir);
    return (int)result;
}

// ---------- switching an installed Office's update channel (the elevated helper) ----------
// --office-channel=N. Points Click-to-Run at the channel's Microsoft address the way its own channel change does, clears
// anything pinning a version or another address, then asks Office's updater to update now (it shows its own window).
static int offChannelHelperMain(const wchar_t* cmd) {
    g_appsNotify = cmdNotify(cmd);
    int ch = _wtoi(cmd + 17);                           // --office-channel=
    if (ch < 0 || ch >= OFF_NCH) return ERROR_INVALID_PARAMETER;
    const wchar_t* cfg = L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration";
    wchar_t ver[40] = L""; DWORD cb = sizeof(ver);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, cfg, L"VersionToReport", RRF_RT_REG_SZ, NULL, ver, &cb) != ERROR_SUCCESS || !ver[0]) {
        appsSay(L"Office (Click-to-Run) isn't installed.");
        return ERROR_PRODUCT_UNINSTALLED;
    }
    DWORD e = offSetChannelReg(ch);
    if (e) return (int)e;
    // policies would override the choice, so say so rather than silently fail
    static const wchar_t* POL[] = { L"updatebranch", L"updatepath", L"updatetargetversion", L"updatechannel" };
    for (int i = 0; i < 4; i++) {
        wchar_t v[300]; DWORD vb = sizeof(v);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\office\\16.0\\common\\officeupdate", POL[i],
                         RRF_RT_ANY, NULL, v, &vb) == ERROR_SUCCESS)
            appsSay(L"Note: a policy (%s) also sets Office's updates and takes precedence.", POL[i]);
    }
    wchar_t exe[MAX_PATH], cl[MAX_PATH + 120];
    if (!ExpandEnvironmentStringsW(L"%CommonProgramFiles%\\Microsoft Shared\\ClickToRun\\OfficeC2RClient.exe", exe, MAX_PATH) || !isFile(exe)) {
        appsSay(L"Office's updater wasn't found; Office switches at its next automatic update.");
        return 0;
    }
    swprintf(cl, MAX_PATH + 120, L"\"%s\" /update user updatepromptuser=False forceappshutdown=False displaylevel=True", exe);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        appsSay(L"Couldn't start Office's updater (error %lu); Office switches at its next automatic update.", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    appsSay(L"Started Office's updater: it downloads the %s build in its own window.", OFF_CH[ch].label);
    return 0;
}

static DWORD WINAPI offInstallProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    DWORD code = waitExitCode(helper);
    PostMessageW(g_hwnd, WM_APP_OFFDONE, code, 0);
    return 0;
}

static void offInstallClick(void) {
    if (g_busy != 0) return;
    if (!g_offSrcVer[0]) { okDialog(L"Choose a source first", L"Choose the folder you downloaded Office into (the one holding the \"Office\" folder)."); return; }
    unsigned mask = 0;
    for (int i = 0; i < OFF_NAPPS; i++) if (g_offIApps[i]) mask |= 1u << i;
    if (!mask) { okDialog(L"Choose at least one app", L"Tick the Office apps to install."); return; }
    if (askInApp()) {
        wchar_t body[500], apps[200];
        offAppsLabel(g_offIApps, apps, 200);
        swprintf(body, 500, L"This installs %s %s (%s) from %s, with updates from %s. Office apps that are open are closed first.",
                 OFF_ED_LABEL, g_offSrcVer, apps, g_offSrc, OFF_CH[g_offICh].label);
        if (!showDialog(g_hwnd, L"Install Microsoft 365 Apps?", body, NULL, 0, L"Install", L"Cancel", 1)) return;
    }
    wchar_t args[MAX_PATH + 80]; BOOL cancelled = FALSE;
    swprintf(args, MAX_PATH + 80, L"--office-install=\"%s\" --office-opt=%d,%u", g_offSrc, g_offICh, mask);
    wchar_t head[300];
    swprintf(head, 300, L"> Install Microsoft 365 Apps %s from %s", g_offSrcVer, g_offSrc);
    if (!offLogBegin(head)) return;
    g_offJob = OFFJ_INSTALL;                         // so the helper's lines are taken while UAC is up
    HANDLE helper = NULL;
    if (g_elevated) helper = startHelperDirect(args);
    else if (!startElevatedHelper(args, &helper, &cancelled)) helper = NULL;
    if (!helper) {
        g_offJob = OFFJ_NONE;
        InterlockedExchange(&g_logOpen, 0);
        InterlockedExchange(&g_busy, 0);
        wcscpy(g_offMsg, cancelled ? L"Cancelled. Nothing was installed." : L"Couldn't start the administrator helper. Nothing was installed.");
        appsLogAdd(g_offMsg, FALSE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    startWorker(offInstallProc, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// After a switch, Office's own updater changes the build in the background: watch for it (up to an hour) so the log can
// say when it's done and what build Office ended up on.
static volatile LONG g_offWatching = 0;
static wchar_t g_offWatchFrom[40];
static DWORD WINAPI offWatchProc(LPVOID arg) {
    (void)arg;
    wchar_t* found = NULL;
    for (int i = 0; i < 720 && !found; i++) {
        Sleep(5000);
        wchar_t ver[40] = L""; DWORD cb = sizeof(ver);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration", L"VersionToReport",
                     RRF_RT_REG_SZ, NULL, ver, &cb);
        if (ver[0] && wcscmp(ver, g_offWatchFrom) && (found = (wchar_t*)malloc(40 * sizeof(wchar_t))) != NULL) lstrcpynW(found, ver, 40);
    }
    if (!PostMessageW(g_hwnd, WM_APP_OFFVER, 0, (LPARAM)found)) { free(found); InterlockedExchange(&g_offWatching, 0); }
    return 0;
}
static void offOnVersion(wchar_t* ver) {
    InterlockedExchange(&g_offWatching, 0);
    wchar_t t[200];
    if (ver) swprintf(t, 200, L"Office's update finished: build %s → build %s.", g_offWatchFrom, ver);
    else swprintf(t, 200, L"Office is still on build %s an hour later. Its updater may have stopped; check Office's Account page.", g_offWatchFrom);
    free(ver);
    appsLogAdd(t, FALSE);
    if (g_offJob == OFFJ_NONE) offReadInstalled();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// "Office changes from build X to build Y", for a switch to a channel
static void offChangeText(int ch, wchar_t* out, int cch) {
    if (!g_offInstVer[0]) { out[0] = 0; return; }
    if (!g_offLatest[ch][0]) { swprintf(out, cch, L"Office is on build %s now; %s's build number isn't known yet.", g_offInstVer, OFF_CH[ch].label); return; }
    int c = appsVerCmp(g_offLatest[ch], g_offInstVer);
    swprintf(out, cch, L"Office changes from build %s to build %s%s once the switch and its update have finished.",
             g_offInstVer, g_offLatest[ch], c < 0 ? L" (an older build)" : c == 0 ? L" (the same build)" : L"");
}

static void offSwitchClick(int ch) {
    if (g_busy != 0 || ch < 0 || ch >= OFF_NCH || ch == g_offInstCh) return;
    wchar_t change[300];
    offChangeText(ch, change, 300);
    {   // always asked, even when UAC will prompt too: the user wants to see the build change before anything starts
        wchar_t body[700];
        swprintf(body, 700, L"%s\n\nOffice updates from %s from now on, and its updater starts as soon as you press OK. %s",
                 change[0] ? change : L"Office's current build isn't known.", OFF_CH[ch].label,
                 ch == OFF_DEVMAIN ? L"Dev Main is Microsoft's internal daily build: expect bugs." : L"");
        wchar_t title[80];
        swprintf(title, 80, L"Switch Office to %s?", OFF_CH[ch].label);
        if (!showDialog(g_hwnd, title, body, NULL, 0, L"OK", L"Cancel", 1)) return;
    }
    wchar_t args[40], head[200]; BOOL cancelled = FALSE;
    swprintf(args, 40, L"--office-channel=%d", ch);
    swprintf(head, 200, L"> Switch Office to %s", OFF_CH[ch].label);
    if (!offLogBegin(head)) return;
    if (change[0]) appsLogAdd(change, FALSE);           // shown as the button is pressed, before the UAC prompt
    lstrcpynW(g_offWatchFrom, g_offInstVer, 40);
    g_offJob = OFFJ_SWITCH;
    HANDLE helper = NULL;
    if (g_elevated) helper = startHelperDirect(args);
    else if (!startElevatedHelper(args, &helper, &cancelled)) helper = NULL;
    if (!helper) {
        g_offJob = OFFJ_NONE;
        InterlockedExchange(&g_logOpen, 0);
        InterlockedExchange(&g_busy, 0);
        wcscpy(g_offMsg, cancelled ? L"Cancelled. Nothing was changed." : L"Couldn't start the administrator helper. Nothing was changed.");
        appsLogAdd(g_offMsg, FALSE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    startWorker(offInstallProc, helper);              // waits for the helper and reports, as for an install
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// wParam: 0 or an error; lParam: for a finished download, its folder (malloc'd)
static void offOnDone(DWORD code, wchar_t* path) {
    int job = g_offJob;
    g_offJob = OFFJ_NONE;
    InterlockedExchange(&g_logOpen, 0);
    InterlockedExchange(&g_busy, 0);
    if (job == OFFJ_DOWNLOAD) {
        if (!code && path) {
            offSetSource(path);
            swprintf(g_offMsg, 400, L"Downloaded to %s. It's chosen below: press Install when ready.", path);
        } else if (code == ERROR_CANCELLED) wcscpy(g_offMsg, L"Download cancelled.");
        else swprintf(g_offMsg, 400, L"The download stopped (error %lu). See the details above.", code);
    } else if (job == OFFJ_SWITCH) {
        offReadInstalled();
        if (!code) {
            swprintf(g_offMsg, 400, L"Office now updates from %s. Its updater is fetching the build; this log says when it has finished.",
                     g_offInstCh >= 0 ? OFF_CH[g_offInstCh].label : L"the new channel");
            if (g_offWatchFrom[0] && InterlockedCompareExchange(&g_offWatching, 1, 0) == 0) {
                HANDLE t = CreateThread(NULL, 0, offWatchProc, NULL, 0, NULL);
                if (t) CloseHandle(t); else InterlockedExchange(&g_offWatching, 0);
            }
        }
        else swprintf(g_offMsg, 400, L"The channel wasn't changed (error %lu).", code);
    } else if (job == OFFJ_INSTALL) {
        offReadInstalled();
        if (!code) swprintf(g_offMsg, 400, L"Microsoft 365 Apps is installed. %s.", g_offInstalled);
        else swprintf(g_offMsg, 400, L"The install stopped with code %lu. See the details above.", code);
    }
    free(path);
    if (g_offMsg[0]) appsLogAdd(g_offMsg, FALSE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- choices ----------
static void offPickLanguage(void) {
    static wchar_t names[OFF_NLANGS][90];
    static const wchar_t* items[OFF_NLANGS];
    for (int i = 0; i < OFF_NLANGS; i++) {
        wchar_t n[80]; offLangName(OFF_LANGS[i], n, 80);
        swprintf(names[i], 90, L"%s (%s)", n, OFF_LANGS[i]);
        items[i] = names[i];
    }
    int pick = g_offLang;
    DlgExtra list; memset(&list, 0, sizeof(list));
    list.items = items; list.nItems = OFF_NLANGS; list.pick = &pick;
    if (!showDialogEx(g_hwnd, L"Office language", L"The language Office is downloaded and installed in.", NULL, 0, L"Choose", L"Cancel", 0, &list)) return;
    g_offLang = pick;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void offPickApps(BOOL* on) {
    static const wchar_t* items[OFF_NAPPS];
    static BOOL checks[OFF_NAPPS];
    for (int i = 0; i < OFF_NAPPS; i++) { items[i] = OFF_APPS[i].name; checks[i] = on[i]; }
    int pick = 0;
    DlgExtra list; memset(&list, 0, sizeof(list));
    list.items = items; list.nItems = OFF_NAPPS; list.pick = &pick; list.checks = checks;
    if (!showDialogEx(g_hwnd, L"Office apps", L"The apps to install. Unticked ones are left out, and can be added later by installing again.",
                      NULL, 0, L"OK", L"Cancel", 0, &list)) return;
    for (int i = 0; i < OFF_NAPPS; i++) on[i] = checks[i];
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void offChooseSource(void) {
    wchar_t dir[MAX_PATH] = L"";
    if (!pickFolder(dir)) return;
    // the "Office" folder itself is fine too: use the folder above it
    if (!_wcsicmp(PathFindFileNameW(dir), L"Office")) {
        wchar_t up[MAX_PATH]; lstrcpynW(up, dir, MAX_PATH); PathRemoveFileSpecW(up);
        wchar_t probe[MAX_PATH + 16]; swprintf(probe, MAX_PATH + 16, L"%s\\Data", dir);
        if (isFolder(probe)) lstrcpynW(dir, up, MAX_PATH);
    }
    offSetSource(dir);
}

static void offClick(int id) {
    if (id == A_O_CANCEL) { if (g_offJob == OFFJ_DOWNLOAD) InterlockedExchange(&g_offCancel, 1); return; }
    if (g_busy != 0) return;
    switch (id) {
    case A_O_CUR: g_offCh = OFF_CURRENT; break;
    case A_O_BETA: g_offCh = OFF_BETA; break;
    case A_O_DEV: g_offCh = OFF_DEVMAIN; break;
    case A_O_IDEV: g_offICh = OFF_DEVMAIN; break;
    case A_O_LANG: offPickLanguage(); break;
    case A_O_DOWNLOAD: offDownloadClick(); break;
    case A_O_SRC: offChooseSource(); break;
    case A_O_ICUR: g_offICh = OFF_CURRENT; break;
    case A_O_IBETA: g_offICh = OFF_BETA; break;
    case A_O_IAPPS: offPickApps(g_offIApps); break;
    case A_O_INSTALL: offInstallClick(); break;
    case A_O_SW0: case A_O_SW1: case A_O_SW2: offSwitchClick(id - A_O_SW0); break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- the page ----------
// Laid out like a Windows 11 Settings card: each row has its name (and a line about it) on the left and one compact
// control on the right, all right-aligned to the same width. Kept tight enough that the Progress log still shows at
// the window's smallest size.
#define OFF_CTL_W 280                      // the right-hand controls' width, in dp
#define OFF_CTL_H 30
#define OFF_ROW2 40                        // a row with a line under its name
#define OFF_ROW1 34                        // a row with just its name
#define OFF_TITLE 28

// A row's name and, under it, a muted line. Returns the row's height.
static int offRow(int x1, int xr, int y, const wchar_t* name, const wchar_t* sub, COLORREF subCol) {
    int h = dp(sub && sub[0] ? OFF_ROW2 : OFF_ROW1);
    if (sub && sub[0]) {
        RECT a = { x1, y + dp(2), xr, y + dp(20) }, b = { x1, y + dp(20), xr, y + dp(38) };
        drawText(g_fBody, name, a, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        drawText(g_fSmall, sub, b, subCol, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        RECT a = { x1, y, xr, y + h };
        drawText(g_fBody, name, a, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    return h;
}

// Current / Beta / Dev Main side by side, the chosen one in the accent colour
static const wchar_t* OFF_SHORT[OFF_NCH] = { L"Current", L"Beta", L"Dev Main" };
static void offSegment3(int id0, int x, int y, int w, int chosen, BOOL en) {
    int third = (w - dp(8)) / 3, h = dp(OFF_CTL_H);
    for (int i = 0; i < OFF_NCH; i++) {
        int bx = x + i * (third + dp(4));
        aioButton(id0 + i, bx, y, i == OFF_NCH - 1 ? x + w : bx + third, y + h, OFF_SHORT[i], i == chosen, en, FALSE);
    }
}

static void offSep(int x1, int x2, int y) {
    Col s = sepCol();
    fillRect(x1, y, x2, y + max(1, dp(1)), s.r, s.g, s.b, s.a * 0.7);
}

// The newest build of each channel, on one line
static void offBuildsLine(wchar_t* out, int cch) {
    static const wchar_t* NAME[OFF_NCH] = { L"Current", L"Beta", L"Dev Main" };
    out[0] = 0;
    for (int i = 0; i < OFF_NCH; i++) {
        wchar_t part[80];
        swprintf(part, 80, L"%s%s %s", i ? L"   ·   " : L"", NAME[i], g_offLatest[i][0] ? g_offLatest[i] : (g_offChecking ? L"checking…" : L"unknown"));
        wcsncat(out, part, cch - wcslen(out) - 1);
    }
}

static void drawOffice(int cL, int cR) {
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    int x1 = cL + dp(20), x2 = cR - dp(20), cw = dp(OFF_CTL_W), ch = dp(OFF_CTL_H);
    int cx = x2 - cw, lr = cx - dp(16);               // control's left, and where the names must stop
    BOOL en = g_busy == 0;
    BOOL installed = !wcsncmp(g_offInstalled, L"Installed:", 10);

    // the card's height, row by row as drawn below
    int body = 2 * OFF_TITLE + 3 * OFF_ROW2 + 2 * OFF_ROW1 + 4 + 2 * (4 + 34) + 4 + 1 + 12;
    int cardB = top + dp(86) + (installed ? dp(OFF_ROW2) : 0) + dp(OFF_ROW2 + 4 + 1) + dp(body);
    fillRoundRect(cL, top, cR, cardB, dp(8), sf.r, sf.g, sf.b, sf.a);
    drawPageHeader(0xE8A5, cL, cR, top, L"MS Office 365", g_offInstalled[0] ? g_offInstalled : L"Microsoft 365 Apps, x64.");
    int y = top + dp(86);
    wchar_t t[200];

    // ---- the installed Office: switch its update channel (the channel it's on now is shown chosen; pressing it does
    //      nothing, see offSwitchClick) ----
    if (installed) {
        wchar_t sub[200];
        int sel = g_offInstCh;
        if (sel >= 0) swprintf(sub, 200, L"Now %s", OFF_CH[sel].label);
        else wcscpy(sub, L"Now on another channel");
        int sh = offRow(x1, lr, y, L"Switch update channel", sub, mutedRGB());
        offSegment3(A_O_SW0, cx, y + (sh - ch) / 2, cw, sel, en);
        y += sh;
    }

    {   // the newest build of all three channels
        wchar_t b[300];
        offBuildsLine(b, 300);
        RECT a = { x1, y + dp(2), x2, y + dp(20) }, s = { x1, y + dp(20), x2, y + dp(38) };
        drawText(g_fBody, L"Latest builds", a, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        drawText(g_fSmall, b, s, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += dp(OFF_ROW2 + 4);
        offSep(cL, cR, y); y += dp(1);
    }

    // ---- download ----
    RECT h1 = { x1, y, x2, y + dp(OFF_TITLE) };
    drawText(g_fBodyBold, L"Download from Microsoft", h1, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += dp(OFF_TITLE);

    int rh = offRow(x1, lr, y, L"Channel", L"Its newest build, x64", mutedRGB());
    offSegment3(A_O_CUR, cx, y + (rh - ch) / 2, cw, g_offCh, en);
    y += rh; offSep(x1, x2, y); y += dp(1);

    rh = offRow(x1, lr, y, L"Language", NULL, 0);
    { wchar_t n[80]; offLangName(OFF_LANGS[g_offLang < 0 ? 0 : g_offLang], n, 80); swprintf(t, 200, L"%s  ▾", n); }
    aioButton(A_O_LANG, cx, y + (rh - ch) / 2, x2, y + (rh - ch) / 2 + ch, t, FALSE, en, FALSE);
    y += rh + dp(4);

    {   // the action: a note on the left, the button at the right
        RECT r = { x1, y, lr, y + dp(34) };
        drawText(g_fSmall, L"About 3–4 GB into a folder you choose. Pressing Download again carries on where it stopped.", r, mutedRGB(), DT_LEFT | DT_WORDBREAK);
        int bw = dp(140);
        if (g_offJob == OFFJ_DOWNLOAD) aioButton(A_O_CANCEL, x2 - bw, y + dp(1), x2, y + dp(1) + ch + dp(2), L"Cancel", FALSE, TRUE, FALSE);
        else aioButton(A_O_DOWNLOAD, x2 - bw, y + dp(1), x2, y + dp(1) + ch + dp(2), L"Download…", TRUE, en, FALSE);
    }
    y += dp(34) + dp(4);

    // ---- install from a source ----
    offSep(cL, cR, y); y += dp(1);
    RECT h2 = { x1, y, x2, y + dp(OFF_TITLE) };
    drawText(g_fBodyBold, L"Install from a downloaded source", h2, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += dp(OFF_TITLE);

    {   // the source: what's in it, and where it is
        wchar_t sub[MAX_PATH + 320];
        const wchar_t* name = g_offSrc[0] ? (g_offSrcVer[0] ? g_offSrcInfo : L"Not an Office source") : L"Source";
        if (g_offSrc[0]) lstrcpynW(sub, g_offSrc, MAX_PATH);
        else wcscpy(sub, L"The folder you downloaded Office into");
        int hh = dp(OFF_ROW2);
        RECT a = { x1, y + dp(2), lr, y + dp(20) }, b = { x1, y + dp(20), lr, y + dp(38) };
        drawText(g_fBody, name, a, g_offSrc[0] && !g_offSrcVer[0] ? RGB(232, 72, 85) : textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        drawText(g_fSmall, sub, b, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        aioButton(A_O_SRC, cx, y + (hh - ch) / 2, x2, y + (hh - ch) / 2 + ch, L"Choose folder…", FALSE, en, FALSE);
        y += hh; offSep(x1, x2, y); y += dp(1);
    }

    rh = offRow(x1, lr, y, L"Update channel", L"Where Office gets its updates from", mutedRGB());
    offSegment3(A_O_ICUR, cx, y + (rh - ch) / 2, cw, g_offICh, en);
    y += rh; offSep(x1, x2, y); y += dp(1);

    rh = offRow(x1, lr, y, L"Apps", NULL, 0);
    { wchar_t a[160]; offAppsLabel(g_offIApps, a, 160); swprintf(t, 200, L"%s  ▾", a); }
    aioButton(A_O_IAPPS, cx, y + (rh - ch) / 2, x2, y + (rh - ch) / 2 + ch, t, FALSE, en, FALSE);
    y += rh + dp(4);

    {
        RECT r = { x1, y, lr, y + dp(34) };
        swprintf(t, 200, L"Installs %s. Open Office apps are closed first.", OFF_ED_LABEL);
        drawText(g_fSmall, t, r, mutedRGB(), DT_LEFT | DT_WORDBREAK);
        int bw = dp(140);
        aioButton(A_O_INSTALL, x2 - bw, y + dp(1), x2, y + dp(1) + ch + dp(2), L"Install", TRUE, en && g_offSrcVer[0], !g_elevated);
    }

    // ---- the log ----
    wchar_t now[200] = L"";
    if (g_offJob == OFFJ_DOWNLOAD && g_offTotal)
        swprintf(now, 200, L"Downloading %d%% · %.2f of %.2f GB", (int)(g_offGot * 100 / g_offTotal), g_offGot / 1073741824.0, g_offTotal / 1073741824.0);
    else if (g_offJob == OFFJ_DOWNLOAD) wcscpy(now, L"Preparing the download…");
    else if (g_offJob == OFFJ_INSTALL) wcscpy(now, L"Installing Office…");
    else if (g_offJob == OFFJ_SWITCH) wcscpy(now, L"Switching the update channel…");
    appsDrawLog(cL, cR, cardB + dp(10), now);
}