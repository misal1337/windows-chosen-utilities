// Install Apps page: each app the newest build from its publisher, installed silently, with an Update button when the
// publisher offers a newer version. An elevated copy of this exe downloads the installer into a folder only
// administrators can write, checks the publisher's digital signature, installs, closes the app if the installer opened
// it, and tidies the shortcuts. Back in the window, as the signed-in user, it sets file types (PotPlayer, NanaZip) and
// PotPlayer's settings. What happens is written to the small log at the bottom of the page.
#include <wintrust.h>
#include <softpub.h>

#define RES_POT_REG 30                    // app.rc embeds apps/PotPlayerMini64.reg under this id

enum { APP_FIREFOX, APP_IDM, APP_POT, APP_CRD, APP_NANAZIP, APP_TAILSCALE, APP_COUNT };
enum { KIND_EXE, KIND_MSI, KIND_MSIX };
typedef struct {
    const wchar_t* label;      // the button
    const wchar_t* name;       // in messages
    const wchar_t* listed;     // how its uninstall entry's DisplayName starts (NULL for a packaged app)
    const wchar_t* exe;        // its program's file name, for its shortcut; NULL if it has none
    BOOL closeFirst;           // it must be closed to install, and is closed again if the installer opens it
    const wchar_t* signer;     // the publisher's name in the installer's signature
    const wchar_t* args;       // silent install (for an .msi, msiexec's options)
    const wchar_t* url;        // NULL: worked out when installing (download page or newest version)
    const wchar_t* link;       // its shortcut, straight in the all-users Start menu's Programs folder; NULL for none
    int kind;
    const wchar_t* dir;        // its install folder, when its uninstall entry doesn't name its program
    const wchar_t* package;    // a packaged (MSIX) app's name, as its full package names start
    const wchar_t* fileName;   // what the download is saved as
    const wchar_t* storeId;    // installed from the Microsoft Store with winget instead (which then keeps it updated)
} AppDef;
static const AppDef APPS[APP_COUNT] = {
    { L"Firefox", L"Firefox", L"Mozilla Firefox", L"firefox.exe", TRUE, L"Mozilla Corporation", L"/S",
      L"https://download.mozilla.org/?product=firefox-latest-ssl&os=win64&lang=en-US", L"Firefox", KIND_EXE, NULL, NULL, L"Firefox_setup.exe" },
    { L"IDM", L"Internet Download Manager", L"Internet Download Manager", L"IDMan.exe", TRUE, L"Tonec Inc.", L"/skipdlgs", NULL,
      L"Internet Download Manager", KIND_EXE, NULL, NULL, L"IDM_setup.exe" },
    { L"Pot Player", L"PotPlayer", L"PotPlayer", L"PotPlayerMini64.exe", TRUE, L"Kakao Corp.", L"/S",
      L"https://t1.daumcdn.net/potplayer/PotPlayer/Version/Latest/PotPlayerSetup64.exe", L"PotPlayer", KIND_EXE, NULL, NULL, L"PotPlayer_setup.exe" },
    // the host only: a service people connect to from remotedesktop.google.com, so no window and no shortcut
    // from Google's update service, as Google's own updater gets it (the website's link lags behind a new version);
    // saved under the website's file name, which an install from there is remembered by
    { L"Chrome Remote Desktop", L"Chrome Remote Desktop", L"Chrome Remote Desktop Host", NULL, FALSE, L"Google LLC", L"/qn /norestart",
      NULL, NULL, KIND_MSI, L"%ProgramFiles(x86)%\\Google\\Chrome Remote Desktop", NULL, L"chromeremotedesktophost.msi" },
    // from the Microsoft Store (winget, as the signed-in user), so the Store keeps it updated; Start lists packaged apps by itself
    { L"NanaZip", L"NanaZip", NULL, NULL, FALSE, NULL, NULL, NULL, NULL, KIND_MSIX,
      NULL, L"40174MouriNaruto.NanaZip_", NULL, L"9N8G7TSCL18R" },
    // its tray app keeps running through an update, as Tailscale's own updater does it
    { L"Tailscale", L"Tailscale", L"Tailscale", L"tailscale-ipn.exe", FALSE, L"Tailscale Inc.", L"/qn /norestart", NULL,
      L"Tailscale", KIND_MSI, L"%ProgramFiles%\\Tailscale", NULL, L"Tailscale_setup.msi" },
};
#define IDM_PAGE L"https://www.internetdownloadmanager.com/download.html"
static const wchar_t* POT_EXTS[] = { L".mp4", L".mkv", L".mpg", L".mpeg", L".wmv" };
static const wchar_t* NANAZIP_EXTS[] = { L".zip", L".rar", L".7z" };

// what the helper's exit code says: the step in the top byte, Windows' or the installer's code below it
enum { APS_OK, APS_LINK, APS_DOWNLOAD, APS_SIGN, APS_INSTALL, APS_MISSING, APS_FOLDER, APS_SAME };
#define APS_CODE(step, detail) (((DWORD)(step) << 24) | ((DWORD)(detail) & 0xFFFFFF))
// progress from the helper (WM_APP_APPSPROG): wParam a step, lParam megabytes so far while downloading
enum { APP_STEP_LINK = 1, APP_STEP_DOWNLOAD, APP_STEP_SIGN, APP_STEP_INSTALL, APP_STEP_CLOSE, APP_STEP_SETTINGS };

static int appsVerCmp(const wchar_t* a, const wchar_t* b);

static int     g_appsRun = -1;            // the app being installed, or -1
static int     g_appsStep = 0, g_appsMb = 0;
static wchar_t g_appsVer[APP_COUNT][64];  // installed version, empty if not installed
static wchar_t g_appsMsg[400];

// ---------- the log at the bottom of the page ----------
// The elevated helper sends its lines with WM_COPYDATA; the window's own work adds them directly.
#define APPS_LOG_MAX  300
#define APPS_LOG_LINE 240
#define APPS_LOG_MAGIC 0x57435541          // "WCUA"; +1 replaces the last line (a progress count)
static wchar_t g_appsLog[APPS_LOG_MAX][APPS_LOG_LINE];
static int     g_appsLogN = 0;
static HWND    g_appsNotify;               // set in the helper: the window to send lines to
static volatile LONG g_logOpen = 0;        // another page's job (Office) is writing to the log

static void appsLogAdd(const wchar_t* s, BOOL replace) {
    EnterCriticalSection(&g_lock);
    if (!replace || g_appsLogN == 0) {
        if (g_appsLogN == APPS_LOG_MAX) { memmove(g_appsLog, g_appsLog + 1, sizeof(g_appsLog[0]) * (APPS_LOG_MAX - 1)); g_appsLogN--; }
        g_appsLogN++;
    }
    wchar_t* d = g_appsLog[g_appsLogN - 1];
    int n = 0;
    for (; s[n] && n < APPS_LOG_LINE - 1; n++) d[n] = (s[n] == L'\r' || s[n] == L'\n' || s[n] == L'\t') ? L' ' : s[n];
    d[n] = 0;
    LeaveCriticalSection(&g_lock);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void appsSayV(BOOL replace, const wchar_t* fmt, va_list ap) {
    wchar_t line[APPS_LOG_LINE];
    vswprintf(line, APPS_LOG_LINE, fmt, ap);
    if (g_appsNotify) {
        COPYDATASTRUCT cd = { APPS_LOG_MAGIC + (replace ? 1 : 0), (DWORD)((wcslen(line) + 1) * sizeof(wchar_t)), line };
        DWORD_PTR r;
        SendMessageTimeoutW(g_appsNotify, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 2000, &r);
    } else appsLogAdd(line, replace);
}
static void appsSay(const wchar_t* fmt, ...) { va_list ap; va_start(ap, fmt); appsSayV(FALSE, fmt, ap); va_end(ap); }
static void appsSayOver(const wchar_t* fmt, ...) { va_list ap; va_start(ap, fmt); appsSayV(TRUE, fmt, ap); va_end(ap); }

// WM_COPYDATA in the window. Only while an install runs, and only text.
static BOOL appsOnCopyData(const COPYDATASTRUCT* cd) {
    if (!cd || (cd->dwData != APPS_LOG_MAGIC && cd->dwData != APPS_LOG_MAGIC + 1)) return FALSE;
    if ((g_appsRun < 0 && !g_logOpen) || !cd->lpData || cd->cbData < 2 || cd->cbData > APPS_LOG_LINE * sizeof(wchar_t)) return TRUE;
    wchar_t line[APPS_LOG_LINE];
    int n = (int)(cd->cbData / sizeof(wchar_t));
    memcpy(line, cd->lpData, n * sizeof(wchar_t));
    line[n - 1] = 0;
    appsLogAdd(line, cd->dwData == APPS_LOG_MAGIC + 1);
    return TRUE;
}

// ---------- what's installed ----------
// A packaged app: the newest of its full package names, such as 40174MouriNaruto.NanaZip_7.0.1882.0_x64__gnj4mf6z9tkrc
static BOOL appsFindPackage(const AppDef* a, wchar_t* ver, int cch) {
    HKEY k;
    ver[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
                      0, KEY_ENUMERATE_SUB_KEYS, &k) != ERROR_SUCCESS) return FALSE;
    size_t pl = wcslen(a->package);
    wchar_t sub[256];
    for (DWORD i = 0; ; i++) {
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (_wcsnicmp(sub, a->package, pl)) continue;
        wchar_t v[64]; int m = 0;
        for (const wchar_t* p = sub + pl; *p && *p != L'_' && m < 63; p++) v[m++] = *p;
        v[m] = 0;
        if (m && iswdigit(v[0]) && (!ver[0] || appsVerCmp(v, ver) > 0)) lstrcpynW(ver, v, cch);
    }
    RegCloseKey(k);
    return ver[0] != 0;
}

// Looks through the uninstall entries Windows lists in Apps > Installed apps. exe gets its program, if it has one.
// listedAs gets its full DisplayName (256), if asked.
static BOOL appsFindInstalledName(const AppDef* a, wchar_t* ver, int cch, wchar_t* exe /* MAX_PATH, or NULL */, wchar_t* listedAs) {
    static const struct { HKEY root; REGSAM view; } WHERE[] = {
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY }, { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY }, { HKEY_CURRENT_USER, 0 } };
    ver[0] = 0;
    if (exe) exe[0] = 0;
    if (listedAs) listedAs[0] = 0;
    if (a->kind == KIND_MSIX) return appsFindPackage(a, ver, cch);
    size_t pl = wcslen(a->listed);
    for (int w = 0; w < 3; w++) {
        HKEY k;
        if (RegOpenKeyExW(WHERE[w].root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                          KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | WHERE[w].view, &k) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0; ; i++) {
            DWORD n = 256;
            if (RegEnumKeyExW(k, i, sub, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wchar_t name[256]; DWORD cb = sizeof(name);       // k already looks at the right view, and so do its subkeys
            if (RegGetValueW(k, sub, L"DisplayName", RRF_RT_REG_SZ, NULL, name, &cb) != ERROR_SUCCESS) continue;
            if (_wcsnicmp(name, a->listed, pl)) continue;
            if (listedAs) lstrcpynW(listedAs, name, 256);
            cb = cch * sizeof(wchar_t);
            if (RegGetValueW(k, sub, L"DisplayVersion", RRF_RT_REG_SZ, NULL, ver, &cb) != ERROR_SUCCESS || !ver[0])
                lstrcpynW(ver, L"installed", cch);
            if (exe && a->exe) {               // its program, from the icon it lists ("C:\…\app.exe",0), or its folder
                wchar_t icon[MAX_PATH + 16]; cb = sizeof(icon);
                if (RegGetValueW(k, sub, L"DisplayIcon", RRF_RT_REG_SZ, NULL, icon, &cb) == ERROR_SUCCESS) {
                    wchar_t* s = icon;
                    if (*s == L'"') { s++; wchar_t* q = wcschr(s, L'"'); if (q) *q = 0; }
                    else { wchar_t* c = wcsrchr(s, L','); if (c && c > wcsrchr(s, L'\\')) *c = 0; }
                    if (!_wcsicmp(PathFindFileNameW(s), a->exe) && isFile(s)) lstrcpynW(exe, s, MAX_PATH);
                }
                wchar_t d[MAX_PATH], p[MAX_PATH];
                if (!exe[0] && a->dir && ExpandEnvironmentStringsW(a->dir, d, MAX_PATH) &&
                    swprintf(p, MAX_PATH, L"%s\\%s", d, a->exe) > 0 && isFile(p)) lstrcpynW(exe, p, MAX_PATH);
            }
            RegCloseKey(k);
            return TRUE;
        }
        RegCloseKey(k);
    }
    return FALSE;
}
static BOOL appsFindInstalledEx(const AppDef* a, wchar_t* ver, int cch, wchar_t* exe) { return appsFindInstalledName(a, ver, cch, exe, NULL); }
static BOOL appsFindInstalled(const AppDef* a, wchar_t* ver, int cch) { return appsFindInstalledEx(a, ver, cch, NULL); }

static void appsRefresh(void) {
    for (int i = 0; i < APP_COUNT; i++) appsFindInstalled(&APPS[i], g_appsVer[i], 64);
}

// Process ids running this program, up to max
static int appsPids(const wchar_t* exe, DWORD* pids, int max) {
    int n = 0;
    if (!exe) return 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (!_wcsicmp(pe.szExeFile, exe) && n < max) pids[n++] = pe.th32ProcessID;
    CloseHandle(s);
    return n;
}

// ---------- the newest version each publisher offers ----------
// Compares dotted versions number by number; a missing part counts as 0 ("26.10.21" equals "26.10.21.0")
static int appsVerCmp(const wchar_t* a, const wchar_t* b) {
    while (*a || *b) {
        unsigned long x = wcstoul(a, (wchar_t**)&a, 10), y = wcstoul(b, (wchar_t**)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        while (*a && *a != L'.') a++;
        while (*b && *b != L'.') b++;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

// The text of a JSON string value after "name": in text, as a plain version (digits and dots only)
static BOOL appsJsonVersion(const char* text, const char* name, wchar_t* out, int cch) {
    const char* p = text ? strstr(text, name) : NULL;
    if (!p) return FALSE;
    p = strchr(p + strlen(name), ':');
    if (!p) return FALSE;
    while (*++p == ' ');
    if (*p != '"') return FALSE;
    int n = 0;
    for (p++; *p && *p != '"' && n < cch - 1; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.') return FALSE;
        out[n++] = (wchar_t)*p;
    }
    out[n] = 0;
    return n > 0 && isdigit((unsigned char)out[0]);
}

// The newest "idmanNNNbuildNN.exe" link on IDM's download page, on IDM's own servers
static BOOL appsIdmLink(wchar_t* url, int cch, DWORD* err) {
    char* page = NULL; DWORD status = 0;
    *err = dlHttp(IDM_PAGE, &page, NULL, &status);
    if (*err || status != 200) { if (!*err) *err = status; free(page); return FALSE; }
    long bestV = -1, bestB = -1;
    static const char HOST[] = "https://";
    for (const char* p = strstr(page, HOST); p; p = strstr(p + 1, HOST)) {
        const char* h = p + 8;
        const char* slash = strchr(h, '/');
        if (!slash || slash - h > 80) continue;
        static const char DOM[] = ".internetdownloadmanager.com";
        size_t hl = (size_t)(slash - h), dl = sizeof(DOM) - 1;
        if (hl <= dl || _strnicmp(slash - dl, DOM, dl)) continue;
        BOOL hostOk = TRUE;
        for (const char* c = h; c < slash; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-') hostOk = FALSE;
        if (!hostOk || _strnicmp(slash + 1, "idman", 5)) continue;
        const char* q = slash + 6;
        char* e1; long v = strtol(q, &e1, 10);
        if (e1 == q || _strnicmp(e1, "build", 5)) continue;
        char* e2; long b = strtol(e1 + 5, &e2, 10);
        if (e2 == e1 + 5 || _strnicmp(e2, ".exe", 4)) continue;
        if (v > bestV || (v == bestV && b > bestB)) {
            bestV = v; bestB = b;
            char a8[200];
            snprintf(a8, 200, "https://%.*s/idman%ldbuild%ld.exe", (int)hl, h, v, b);
            MultiByteToWideChar(CP_ACP, 0, a8, -1, url, cch);
        }
    }
    free(page);
    if (bestV < 0) { *err = ERROR_NOT_FOUND; return FALSE; }
    return TRUE;
}

// Google's update service (the one Google's own updaters ask): the version it offers, and where that exact package is
// with its SHA-256. A new version is rolled out gradually, so one answer may name it and the next the version before.
static BOOL appsGoogleOnce(const char* appId, wchar_t* out, int cch, wchar_t* url, int urlCch, wchar_t* sha /* 65 */) {
    url[0] = 0; sha[0] = 0;
    // it only answers when told the Windows version (GetVersionEx would report an old one, so RtlGetVersion)
    RTL_OSVERSIONINFOW os = { sizeof(os) };
    LONG (WINAPI *rtlGetVersion)(RTL_OSVERSIONINFOW*) =
        (LONG (WINAPI *)(RTL_OSVERSIONINFOW*))(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!rtlGetVersion || rtlGetVersion(&os) != 0) { os.dwMajorVersion = 10; os.dwMinorVersion = 0; os.dwBuildNumber = 19045; }
    // a fresh request and session id each time, as Google's own updater sends: the rollout decides per request
    char ids[2][40];
    for (int i = 0; i < 2; i++) {
        GUID g; CoCreateGuid(&g);
        snprintf(ids[i], 40, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3,
                 g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    }
    char body[800];
    snprintf(body, sizeof(body), "{\"request\":{\"protocol\":\"3.1\",\"ismachine\":1,\"requestid\":\"%s\",\"sessionid\":\"%s\","
             "\"os\":{\"platform\":\"win\",\"version\":\"%lu.%lu.%lu\",\"arch\":\"x86_64\"},\"arch\":\"x86_64\","
             "\"app\":[{\"appid\":\"%s\",\"version\":\"1.0.0.0\",\"updatecheck\":{}}]}}",
             ids[0], ids[1], os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, appId);
    BOOL ok = FALSE;
    HINTERNET s = WinHttpOpen(L"WindowsChosenUtilities", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"update.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", L"/service/update2/json", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r) {
        WinHttpSetTimeouts(r, 15000, 15000, 30000, 30000);
        DWORD status = 0, cb = sizeof(status);
        if (WinHttpSendRequest(r, L"Content-Type: application/json\r\n", (DWORD)-1L, body, (DWORD)strlen(body), (DWORD)strlen(body), 0) &&
            WinHttpReceiveResponse(r, NULL) &&
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            char buf[16384]; DWORD len = 0, got;
            while (len < sizeof(buf) - 1 && WinHttpReadData(r, buf + len, (DWORD)(sizeof(buf) - 1 - len), &got) && got) len += got;
            buf[len] = 0;
            const char* m = strstr(buf, "\"manifest\"");
            ok = m && appsJsonVersion(m, "\"version\"", out, cch);
            // the package: Google's https download server, the file's name and its SHA-256
            const char* cb = strstr(buf, "\"codebase\":\"https://dl.google.com/");
            const char* pk = m ? strstr(m, "\"package\"") : NULL;
            const char* nm = pk ? strstr(pk, "\"name\":\"") : NULL;
            const char* hs = pk ? strstr(pk, "\"hash_sha256\":\"") : NULL;
            if (ok && cb && nm && hs) {
                char a[600] = "";
                const char* ce = strchr(cb + 12, '"');
                const char* ne = strchr(nm + 8, '"');
                if (ce && ne && (ce - cb - 12) + (ne - nm - 8) < 590) {
                    memcpy(a, cb + 12, ce - cb - 12);
                    memcpy(a + (ce - cb - 12), nm + 8, ne - nm - 8);
                    a[(ce - cb - 12) + (ne - nm - 8)] = 0;
                    BOOL safe = TRUE;               // a plain path: no quotes, spaces or escapes
                    for (const char* q = a; *q; q++) if (!isalnum((unsigned char)*q) && !strchr(":/._-", *q)) safe = FALSE;
                    if (safe) MultiByteToWideChar(CP_UTF8, 0, a, -1, url, urlCch);
                }
                for (int i = 0; i < 64 && isxdigit((unsigned char)hs[15 + i]); i++) { sha[i] = (wchar_t)tolower((unsigned char)hs[15 + i]); sha[i + 1] = 0; }
                if (wcslen(sha) != 64) { sha[0] = 0; url[0] = 0; }
            }
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

// The newest of a few answers, so a version being rolled out is found (and is what Update then installs)
static BOOL appsGoogleOffer(const char* appId, int tries, wchar_t* out, int cch, wchar_t* url, int urlCch, wchar_t* sha /* 65 */) {
    out[0] = 0; url[0] = 0; sha[0] = 0;
    for (int i = 0; i < tries; i++) {
        wchar_t v[64], u[600], h[65];
        if (!appsGoogleOnce(appId, v, 64, u, 600, h) || !u[0]) continue;
        if (!out[0] || appsVerCmp(v, out) > 0) { lstrcpynW(out, v, cch); lstrcpynW(url, u, urlCch); wcscpy(sha, h); }
    }
    return out[0] != 0;
}
#define CRD_APPID "{b210701e-ffc4-49e3-932b-370728c72662}"
static wchar_t g_appsSha[65];          // the helper: the SHA-256 the publisher lists for the download, when it lists one
static wchar_t g_appsOffered[64];      // the helper: the version that download is

// Chrome Remote Desktop's newest package, once Google's service has offered it. While a version is rolled out only
// some answers name it, so the first one that does is remembered (version, Google's link, SHA-256) and used from then
// on, until it's installed or Google takes the file down.
#define CRD_KEY L"Software\\WindowsChosenUtilities\\ChromeRemoteDesktop"
static BOOL appsCrdStored(wchar_t* ver, wchar_t* url, int urlCch, wchar_t* sha /* 65 */) {
    DWORD cb = 64 * sizeof(wchar_t), cu = urlCch * sizeof(wchar_t), cs = 65 * sizeof(wchar_t);
    ver[0] = url[0] = sha[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, CRD_KEY, L"Version", RRF_RT_REG_SZ, NULL, ver, &cb) ||
        RegGetValueW(HKEY_CURRENT_USER, CRD_KEY, L"Url", RRF_RT_REG_SZ, NULL, url, &cu) ||
        RegGetValueW(HKEY_CURRENT_USER, CRD_KEY, L"Sha256", RRF_RT_REG_SZ, NULL, sha, &cs)) { ver[0] = url[0] = sha[0] = 0; return FALSE; }
    // only a package on Google's download server, with a hash to check it against
    if (wcsncmp(url, L"https://dl.google.com/", 22) || wcschr(url, L'"') || wcschr(url, L' ') || !dlHex64(sha)) { ver[0] = url[0] = sha[0] = 0; return FALSE; }
    return iswdigit(ver[0]);
}
static void appsCrdStore(const wchar_t* ver, const wchar_t* url, const wchar_t* sha) {
    if (!ver) { RegDeleteTreeW(HKEY_CURRENT_USER, CRD_KEY); RegDeleteKeyW(HKEY_CURRENT_USER, CRD_KEY); return; }
    RegSetKeyValueW(HKEY_CURRENT_USER, CRD_KEY, L"Version", REG_SZ, ver, (DWORD)((wcslen(ver) + 1) * sizeof(wchar_t)));
    RegSetKeyValueW(HKEY_CURRENT_USER, CRD_KEY, L"Url", REG_SZ, url, (DWORD)((wcslen(url) + 1) * sizeof(wchar_t)));
    RegSetKeyValueW(HKEY_CURRENT_USER, CRD_KEY, L"Sha256", REG_SZ, sha, (DWORD)((wcslen(sha) + 1) * sizeof(wchar_t)));
}

// The newest package known: the service's answers now, or the one remembered if that's newer and still on Google's
// server. Remembers a newer answer. FALSE if neither gives one.
static BOOL appsCrdNewest(int tries, wchar_t* ver, int cch, wchar_t* url, int urlCch, wchar_t* sha /* 65 */) {
    wchar_t sv[64], su[600], ss[65], have[64] = L"";
    BOOL fresh = appsGoogleOffer(CRD_APPID, tries, ver, cch, url, urlCch, sha);
    BOOL stored = appsCrdStored(sv, su, 600, ss);
    if (stored && appsFindInstalled(&APPS[APP_CRD], have, 64) && appsVerCmp(sv, have) <= 0) { appsCrdStore(NULL, NULL, NULL); stored = FALSE; }
    if (stored && (!fresh || appsVerCmp(sv, ver) > 0)) {
        DWORD status = 0;                   // still there? Google may have taken a withdrawn version down
        if (!dlHttpReq(su, L"HEAD", NULL, NULL, NULL, &status, NULL, FALSE, NULL, NULL) && status == 200) {
            lstrcpynW(ver, sv, cch); lstrcpynW(url, su, urlCch); wcscpy(sha, ss);
            return TRUE;
        }
        if (status == 404 || status == 403 || status == 410) appsCrdStore(NULL, NULL, NULL);
    }
    if (fresh && (!stored || appsVerCmp(ver, sv) > 0) && (!have[0] || appsVerCmp(ver, have) > 0)) appsCrdStore(ver, url, sha);
    return fresh;
}
static BOOL appsGoogleLatest(const char* appId, wchar_t* out, int cch) {
    wchar_t url[600], sha[65];
    (void)appId;
    return appsCrdNewest(8, out, cch, url, 600, sha);
}

static BOOL appsLatestVersion(int app, wchar_t* out, int cch) {
    out[0] = 0;
    char* body = NULL; DWORD status = 0; BOOL ok = FALSE;
    switch (app) {
    case APP_FIREFOX:
        if (!dlHttp(L"https://product-details.mozilla.org/1.0/firefox_versions.json", &body, NULL, &status) && status == 200)
            ok = appsJsonVersion(body, "\"LATEST_FIREFOX_VERSION\"", out, cch);
        break;
    case APP_IDM: {                       // idman643build18.exe is 6.43.18
        wchar_t url[600]; DWORD e;
        if (appsIdmLink(url, 600, &e)) {
            const wchar_t* f = PathFindFileNameW(url);
            long v = 0, b = 0;
            if (swscanf(f, L"idman%ldbuild%ld", &v, &b) == 2 && v > 0) { swprintf(out, cch, L"%ld.%02ld.%ld", v / 100, v % 100, b); ok = TRUE; }
        }
        break;
    }
    case APP_POT: {                       // the update history starts with the newest build, as [YYMMDD]
        if (!dlHttp(L"https://t1.kakaocdn.net/potplayer/PotPlayer/v4/Update2/UpdateEng.html", &body, NULL, &status) && status == 200) {
            for (const char* p = strchr(body, '['); p && !ok; p = strchr(p + 1, '[')) {
                int d = 0;
                while (d < 7 && isdigit((unsigned char)p[1 + d])) d++;
                if (d == 6 && p[7] == ']') {
                    swprintf(out, cch, L"%c%c.%c%c.%c%c", p[1], p[2], p[3], p[4], p[5], p[6]);
                    ok = TRUE;
                }
            }
        }
        break;
    }
    case APP_CRD:
        ok = appsGoogleLatest(CRD_APPID, out, cch);
        break;
    case APP_NANAZIP:                     // the Microsoft Store updates it, so no Update button
        break;
    case APP_TAILSCALE:
        if (!dlHttp(L"https://pkgs.tailscale.com/stable/?mode=json", &body, NULL, &status) && status == 200)
            ok = appsJsonVersion(body, "\"MSIsVersion\"", out, cch);
        break;
    }
    free(body);
    if (!ok) out[0] = 0;
    return ok;
}

// Where to download the app from now. Fixed for most; worked out from the publisher for the rest.
static BOOL appsResolveUrl(int app, wchar_t* url, int cch, DWORD* err) {
    const AppDef* a = &APPS[app];
    *err = 0;
    g_appsSha[0] = 0; g_appsOffered[0] = 0;
    if (a->url) { lstrcpynW(url, a->url, cch); return TRUE; }
    if (app == APP_IDM) return appsIdmLink(url, cch, err);
    if (app == APP_CRD) {               // the package Google's update service offers, not the website's link, which lags behind
        if (!appsCrdNewest(8, g_appsOffered, 64, url, cch, g_appsSha)) { *err = ERROR_NOT_FOUND; return FALSE; }
        return TRUE;
    }
    wchar_t v[64];
    if (!appsLatestVersion(app, v, 64)) { *err = ERROR_NOT_FOUND; return FALSE; }
    if (app == APP_TAILSCALE) swprintf(url, cch, L"https://pkgs.tailscale.com/stable/tailscale-setup-%s-amd64.msi", v);
    else { *err = ERROR_NOT_FOUND; return FALSE; }
    return TRUE;
}

// ---------- the elevated helper ----------
static void appsProgress(int step, int mb) { if (g_appsNotify) PostMessageW(g_appsNotify, WM_APP_APPSPROG, step, mb); }
static BOOL appsDownloadTick(unsigned long long got, void* ctx) {
    int* last = (int*)ctx, mb = (int)(got >> 20);
    if (mb != *last) { *last = mb; appsProgress(APP_STEP_DOWNLOAD, mb); appsSayOver(L"Downloading… %d MB", mb); }
    return TRUE;
}

// Is the file signed, with a signature Windows trusts, by this publisher?
static HRESULT appsCheckSigner(const wchar_t* path, const wchar_t* signer) {
    WINTRUST_FILE_INFO fi = { sizeof(fi) };
    fi.pcwszFilePath = path;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = { sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    HRESULT hr = (HRESULT)WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (hr == S_OK) {
        wchar_t who[200] = L"";
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
        if (sg && sg->csCertChain && sg->pasCertChain && sg->pasCertChain[0].pCert)
            CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, who, 200);
        if (wcscmp(who, signer)) { hr = TRUST_E_SUBJECT_NOT_TRUSTED; appsSay(L"Signed by \"%s\", not \"%s\".", who, signer); }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return hr;
}

// Closes copies of the program that weren't running before (the installer starting the app). It watches for a while,
// because some installers start it just as they exit.
static void appsCloseNew(const wchar_t* exe, const DWORD* before, int nb, DWORD ms) {
    if (!exe) return;
    int closed = 0;
    ULONGLONG until = GetTickCount64() + ms;
    do {
        DWORD now[64]; int n = appsPids(exe, now, 64);
        for (int i = 0; i < n; i++) {
            BOOL old = FALSE;
            for (int k = 0; k < nb; k++) if (before[k] == now[i]) old = TRUE;
            if (old) continue;
            HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, now[i]);
            if (p) { if (TerminateProcess(p, 0)) closed++; WaitForSingleObject(p, 3000); CloseHandle(p); }
        }
        Sleep(250);
    } while (GetTickCount64() < until);
    if (closed) appsSay(L"Closed %s, which the installer had opened.", exe);
}

// ---------- shortcuts: the user's rule for every app ----------
// No desktop shortcuts, and in the Start menu one shortcut straight in the all-users Programs folder, with no folder
// of the app's own there or in the user's Programs. A shortcut counts as the app's when it points into its install
// folder (the folder of its program), which also catches its help and uninstall shortcuts.

// Does this .lnk point into dir (ending in '\')?
static BOOL appsLinkInto(const wchar_t* lnk, const wchar_t* dir) {
    BOOL hit = FALSE;
    IShellLinkW* sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&sl))) return FALSE;
    IPersistFile* pf = NULL;
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void**)&pf))) {
        wchar_t target[MAX_PATH] = L"";
        if (SUCCEEDED(pf->lpVtbl->Load(pf, lnk, STGM_READ)) &&
            SUCCEEDED(sl->lpVtbl->GetPath(sl, target, MAX_PATH, NULL, SLGP_RAWPATH))) {
            wchar_t full[MAX_PATH];
            if (!ExpandEnvironmentStringsW(target, full, MAX_PATH)) lstrcpynW(full, target, MAX_PATH);
            hit = full[0] && !_wcsnicmp(full, dir, wcslen(dir));
        }
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return hit;
}

// Deletes .lnk files straight in folder that point into dir
static void appsDropLinks(const wchar_t* folder, const wchar_t* dir, const wchar_t* where) {
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    if (swprintf(pat, MAX_PATH, L"%s\\*.lnk", folder) < 0) return;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (swprintf(p, MAX_PATH, L"%s\\%s", folder, fd.cFileName) < 0) continue;
        if (appsLinkInto(p, dir) && DeleteFileW(p)) appsSay(L"Removed shortcut from %s: %s", where, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Removes each folder directly under programs holding a shortcut into dir. With topToo, the app's shortcuts straight
// in programs go as well (the user's own Programs, where none belong).
static void appsDropStartFolders(const wchar_t* programs, const wchar_t* dir, BOOL topToo, const wchar_t* where) {
    wchar_t pat[MAX_PATH], sub[MAX_PATH], inner[MAX_PATH];
    if (swprintf(pat, MAX_PATH, L"%s\\*", programs) < 0) return;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || isDotName(fd.cFileName)) continue;
        if (swprintf(sub, MAX_PATH, L"%s\\%s", programs, fd.cFileName) < 0) continue;
        // Windows' own folders (Startup, Accessories, System Tools…) carry a desktop.ini; never touch those
        wchar_t ini[MAX_PATH];
        if (!_wcsicmp(fd.cFileName, L"Startup") || (swprintf(ini, MAX_PATH, L"%s\\desktop.ini", sub) > 0 && isFile(ini))) continue;
        BOOL ours = FALSE;
        if (swprintf(inner, MAX_PATH, L"%s\\*.lnk", sub) > 0) {
            WIN32_FIND_DATAW f2; HANDLE h2 = FindFirstFileW(inner, &f2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    wchar_t l[MAX_PATH];
                    if (swprintf(l, MAX_PATH, L"%s\\%s", sub, f2.cFileName) > 0 && appsLinkInto(l, dir)) ours = TRUE;
                } while (!ours && FindNextFileW(h2, &f2));
                FindClose(h2);
            }
        }
        if (ours) {
            deleteContents(sub);
            if (RemoveDirectoryW(sub)) appsSay(L"Removed Start menu folder from %s: %s", where, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (topToo) appsDropLinks(programs, dir, where);
}

// common: the all-users places (needs administrator rights); otherwise this user's own Desktop and Programs
static void appsTidyShortcuts(const AppDef* a, BOOL common) {
    wchar_t ver[64], exe[MAX_PATH], dir[MAX_PATH];
    if (a->kind == KIND_MSIX || !appsFindInstalledEx(a, ver, 64, exe)) return;    // a packaged app makes no shortcut files
    if (exe[0]) { lstrcpynW(dir, exe, MAX_PATH); PathRemoveFileSpecW(dir); }
    else if (!a->dir || !ExpandEnvironmentStringsW(a->dir, dir, MAX_PATH) || !isFolder(dir)) return;
    if (wcslen(dir) < 4 || PathIsRootW(dir)) return;         // never "everything on C:"
    wcscat(dir, L"\\");
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    PWSTR desk = NULL, prog = NULL;
    SHGetKnownFolderPath(common ? &FOLDERID_PublicDesktop : &FOLDERID_Desktop, 0, NULL, &desk);
    SHGetKnownFolderPath(common ? &FOLDERID_CommonPrograms : &FOLDERID_Programs, 0, NULL, &prog);
    if (desk) appsDropLinks(desk, dir, common ? L"the Public Desktop" : L"your Desktop");
    if (prog) appsDropStartFolders(prog, dir, !common, common ? L"all users' Start menu" : L"your Start menu");
    if (desk) CoTaskMemFree(desk);
    if (prog) CoTaskMemFree(prog);
    if (SUCCEEDED(co)) CoUninitialize();
}

// The app's shortcut straight in the all-users Programs folder, made if it isn't there
static void appsPlaceShortcut(const AppDef* a) {
    wchar_t ver[64], exe[MAX_PATH], lnk[MAX_PATH], dir[MAX_PATH];
    if (!a->link || !appsFindInstalledEx(a, ver, 64, exe) || !exe[0]) return;
    PWSTR base = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &base))) return;
    swprintf(lnk, MAX_PATH, L"%s\\%s.lnk", base, a->link);
    CoTaskMemFree(base);
    if (isFile(lnk)) return;
    lstrcpynW(dir, exe, MAX_PATH);
    PathRemoveFileSpecW(dir);
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void**)&sl))) {
        sl->lpVtbl->SetPath(sl, exe);
        sl->lpVtbl->SetWorkingDirectory(sl, dir);
        IPersistFile* pf = NULL;
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->lpVtbl->Save(pf, lnk, TRUE))) appsSay(L"Added Start menu shortcut: Programs\\%s.lnk", a->link);
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

// Runs a command hidden, passing what it prints to the log. Returns its exit code, or a Windows error.
static DWORD appsRunLogged(const wchar_t* exe, wchar_t* cmd) {
    HANDLE rd = NULL; PROCESS_INFORMATION pi;
    DWORD err = spawnPiped(exe, cmd, &rd, &pi);
    if (err) return err;
    char buf[1024], line[APPS_LOG_LINE]; int ln = 0; DWORD got;
    while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n' || ln == APPS_LOG_LINE - 1) {
                line[ln] = 0;
                if (ln) { wchar_t w[APPS_LOG_LINE]; MultiByteToWideChar(CP_OEMCP, 0, line, -1, w, APPS_LOG_LINE); appsSay(L"  %s", w); }
                ln = 0;
                if (c == '\n') continue;
            }
            line[ln++] = c;
        }
    }
    if (ln) { line[ln] = 0; wchar_t w[APPS_LOG_LINE]; MultiByteToWideChar(CP_OEMCP, 0, line, -1, w, APPS_LOG_LINE); appsSay(L"  %s", w); }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    return waitExitCode(pi.hProcess);
}

// IDM's own update (Help > Check for updates): idmupdt.exe from its update folder, not the full installer from the
// download page. IDM's servers send it to one of their mirrors, and some mirrors still hold an older build, so each is
// tried in turn until one gives a signed idmupdt.exe of the newest version (its file version, such as 6.43.19.x).
static const wchar_t* IDM_UPD_HOSTS[] = {
    L"www", L"mirror2", L"mirror3", L"mirror4", L"mirror5", L"test", L"mirror1", L"mirror6" };
#define IDM_UPD_PATH L"/updates/dfghrtv3465773FRJD843dedhx250dlAsb/idmupdt.exe"

static BOOL appsFileVersion(const wchar_t* path, DWORD* ms, DWORD* ls) {
    DWORD h, n = GetFileVersionInfoSizeW(path, &h);
    if (!n) return FALSE;
    void* b = malloc(n);
    VS_FIXEDFILEINFO* fi = NULL; UINT fl = 0;
    BOOL ok = b && GetFileVersionInfoW(path, 0, n, b) && VerQueryValueW(b, L"\\", (void**)&fi, &fl) && fi && fl >= sizeof(*fi);
    if (ok) { *ms = fi->dwFileVersionMS; *ls = fi->dwFileVersionLS; }
    free(b);
    return ok;
}

static DWORD appsIdmFetchUpdate(const wchar_t* path) {
    wchar_t page[600], url[600]; DWORD e;
    appsProgress(APP_STEP_LINK, 0);
    appsSay(L"Finding IDM's newest update…");
    if (!appsIdmLink(page, 600, &e)) { appsSay(L"Couldn't find it (error %lu).", e); return APS_CODE(APS_LINK, e); }
    long v = 0, b = 0;
    if (swscanf(PathFindFileNameW(page), L"idman%ldbuild%ld", &v, &b) != 2 || v <= 0) {
        appsSay(L"Couldn't read the newest version from %s.", page); return APS_CODE(APS_LINK, ERROR_NOT_FOUND); }
    DWORD want = ((DWORD)(v / 100) << 16) | (DWORD)(v % 100);
    DWORD last = APS_CODE(APS_DOWNLOAD, ERROR_NOT_FOUND);
    for (int i = 0; i < (int)(sizeof(IDM_UPD_HOSTS) / sizeof(IDM_UPD_HOSTS[0])); i++) {
        swprintf(url, 600, L"https://%s.internetdownloadmanager.com" IDM_UPD_PATH L"?v=%ldbuild%ld&filename=idmupdt.exe", IDM_UPD_HOSTS[i], v, b);
        appsSay(L"From %s", url);
        appsProgress(APP_STEP_DOWNLOAD, 0);
        appsSay(L"Downloading…");
        DWORD status = 0; int lastMb = -1;
        e = dlHttpCore(url, NULL, path, &status, FALSE, appsDownloadTick, &lastMb);
        if (e || status != 200) {
            appsSay(e ? L"Download failed (error %lu)." : L"Download failed (HTTP %lu).", e ? e : status);
            last = APS_CODE(APS_DOWNLOAD, e ? e : status);
            DeleteFileW(path);
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        double mb = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) ? (double)fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) / 1048576.0 : 0;
        appsSayOver(L"Downloaded %.1f MB.", mb);
        appsProgress(APP_STEP_SIGN, 0);
        HRESULT hr = appsCheckSigner(path, L"Tonec Inc.");
        if (hr != S_OK) {
            appsSay(L"Signature check failed (0x%08lX). Deleted the download.", (DWORD)hr);
            DeleteFileW(path);
            return APS_CODE(APS_SIGN, (DWORD)hr & 0xFFFF);
        }
        appsSay(L"Signature checked: signed by Tonec Inc.");
        DWORD ms = 0, ls = 0;
        if (appsFileVersion(path, &ms, &ls) && ms == want && HIWORD(ls) == (DWORD)b) return 0;
        appsSay(L"That server still has the update for %u.%02u build %u, not %ld.%02ld build %ld. Trying the next one…",
                HIWORD(ms), LOWORD(ms), HIWORD(ls), v / 100, v % 100, b);
        last = APS_CODE(APS_DOWNLOAD, ERROR_NOT_FOUND);
        DeleteFileW(path);
    }
    appsSay(L"No IDM server had the update for %ld.%02ld build %ld yet. Try again later.", v / 100, v % 100, b);
    return last;
}

// idmupdt.exe unpacks IDM's setup (IDM1.tmp) and exits at once, so the setup is waited for here. Run as an update it
// still opens its licence page in silent mode; the page is accepted (the full setup accepts it silently too), and the
// rest runs without dialogs. Returns the setup's exit code.
typedef struct { int id; HWND found; } AppsFindCtl;
static BOOL CALLBACK appsFindCtlProc(HWND h, LPARAM lp) {
    AppsFindCtl* f = (AppsFindCtl*)lp;
    if (GetDlgCtrlID(h) == f->id) { f->found = h; return FALSE; }
    return TRUE;
}
static HWND appsFindCtl(HWND top, int id) {      // a control anywhere inside the window (the pages are child dialogs)
    AppsFindCtl f = { id, NULL };
    EnumChildWindows(top, appsFindCtlProc, (LPARAM)&f);
    return f.found;
}
static BOOL CALLBACK appsIdmPageProc(HWND h, LPARAM lp) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    const DWORD* pids = (const DWORD*)lp;
    BOOL ours = FALSE;
    for (int i = 1; i <= (int)pids[0]; i++) if (pids[i] == pid) ours = TRUE;
    if (!ours || !IsWindowVisible(h)) return TRUE;
    HWND accept = appsFindCtl(h, 1063), next = appsFindCtl(h, 1008);   // "I accept the terms…", "Next >"
    if (accept && next && IsWindowVisible(accept) && IsWindowEnabled(accept)) {
        SendMessageTimeoutW(accept, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL);
        PostMessageW(next, BM_CLICK, 0, 0);
        appsSay(L"Accepted IDM's licence page (its update shows it even when silent).");
    }
    return TRUE;
}
static DWORD appsIdmWaitSetup(void) {
    DWORD code = 0;
    HANDLE setup = NULL;
    ULONGLONG until = GetTickCount64() + 30 * 60 * 1000, lastAccept = 0;
    for (;;) {
        DWORD pids[17]; int n = appsPids(L"IDM1.tmp", pids + 1, 16);
        if (!n) break;
        if (!setup) setup = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[1]);
        if (GetTickCount64() - lastAccept > 3000) {   // gives the page time to change before looking again
            pids[0] = (DWORD)n;
            EnumWindows(appsIdmPageProc, (LPARAM)pids);
            lastAccept = GetTickCount64();
        }
        if (GetTickCount64() > until) { code = WAIT_TIMEOUT; break; }
        Sleep(500);
    }
    if (setup) {
        if (!code && WaitForSingleObject(setup, 5000) == WAIT_OBJECT_0) GetExitCodeProcess(setup, &code);
        CloseHandle(setup);
    }
    return code;
}

// ---------- Firefox: Mozilla's own delta update ----------
// Firefox updates itself with a small "partial" MAR from Mozilla's update service (aus5.mozilla.org) when there is
// one for the exact build installed. Mozilla makes those only from its last few builds (in Nov 2026: 159.0 to 160.0.1
// got one to 161.0, 158.0.1 and older only the complete update), so an older Firefox gets the full setup. The service
// is asked as Firefox itself asks it, the MAR is checked against the SHA-512 it lists, and it's applied with the
// installed Firefox's own updater.exe, which checks Mozilla's signature and the channel on the MAR and puts every file
// back if anything fails. Returns 0 when Firefox is updated; otherwise the log says why, and the full setup is used.

// An XML attribute's value in the element starting at el (up to its '>'), with &amp; turned back into &
static BOOL appsXmlAttr(const char* el, const char* name, char* out, int cch) {
    const char* end = strchr(el, '>');
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char* p = strstr(el, name); p && (!end || p < end); p = strstr(p + 1, name)) {
        if (p == el || p[-1] != ' ' || p[nl] != '=' || p[nl + 1] != '"') continue;
        const char* v = p + nl + 2; int n = 0;
        while (*v && *v != '"' && n < cch - 1) {
            if (!strncmp(v, "&amp;", 5)) { out[n++] = '&'; v += 5; } else out[n++] = *v++;
        }
        out[n] = 0;
        return *v == '"' && n > 0;
    }
    return FALSE;
}

static BOOL appsAllowed(const wchar_t* s, const wchar_t* extra) {    // letters, digits and the extra characters only
    if (!s[0]) return FALSE;
    for (; *s; s++) if (!iswalnum(*s) && !wcschr(extra, *s)) return FALSE;
    return TRUE;
}

// The last few lines of the updater's log, for when it fails
static void appsSayLogTail(const wchar_t* path, int lines) {
    char* t = NULL; DWORD n = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && size < (8u << 20) && (t = (char*)malloc(size + 1)) && ReadFile(f, t, size, &n, NULL)) {
        t[n] = 0;
        int found = 0; char* p = t + n;
        while (p > t && found <= lines) { p--; if (*p == '\n' && p + 1 < t + n && p[1] != '\n' && p[1] != '\r') found++; }
        char* line = (found > lines) ? p + 1 : t;
        for (char* e; line && *line; line = e ? e + 1 : NULL) {
            e = strchr(line, '\n');
            if (e) *e = 0;
            size_t l = strlen(line);
            if (l && line[l - 1] == '\r') line[l - 1] = 0;
            if (*line) { wchar_t w[APPS_LOG_LINE]; MultiByteToWideChar(CP_UTF8, 0, line, -1, w, APPS_LOG_LINE); w[APPS_LOG_LINE - 1] = 0; appsSay(L"  %s", w); }
        }
    }
    free(t);
    CloseHandle(f);
}

static DWORD appsFirefoxDeltaIn(const wchar_t* dir, const wchar_t* patch) {
    const AppDef* a = &APPS[APP_FIREFOX];
    wchar_t ver[64], exe[MAX_PATH], listed[256], inst[MAX_PATH], ini[MAX_PATH];
    appsProgress(APP_STEP_LINK, 0);
    appsSay(L"Asking Mozilla's update service for a delta update…");
    if (!appsFindInstalledName(a, ver, 64, exe, listed) || !exe[0]) { appsSay(L"Couldn't find Firefox's folder."); return 1; }
    lstrcpynW(inst, exe, MAX_PATH);
    PathRemoveFileSpecW(inst);

    // the installed build, its channel, and its CPU and language (its uninstall entry reads "Mozilla Firefox (x64 en-US)")
    wchar_t appVer[64] = L"", build[32] = L"", channel[40] = L"release", arch[16] = L"", locale[24] = L"";
    swprintf(ini, MAX_PATH, L"%s\\application.ini", inst);
    GetPrivateProfileStringW(L"App", L"Version", L"", appVer, 64, ini);
    GetPrivateProfileStringW(L"App", L"BuildID", L"", build, 32, ini);
    if (!build[0]) {
        swprintf(ini, MAX_PATH, L"%s\\platform.ini", inst);
        GetPrivateProfileStringW(L"Build", L"Milestone", L"", appVer, 64, ini);
        GetPrivateProfileStringW(L"Build", L"BuildID", L"", build, 32, ini);
    }
    if (!appsAllowed(appVer, L".") || !appsAllowed(build, L"")) { appsSay(L"Couldn't read Firefox's version and build from its folder."); return 1; }
    {
        wchar_t prefs[MAX_PATH]; char* js = NULL;
        swprintf(prefs, MAX_PATH, L"%s\\defaults\\pref\\channel-prefs.js", inst);
        HANDLE f = CreateFileW(prefs, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(f, NULL), n = 0;
            if (size < 65536 && (js = (char*)malloc(size + 1)) && ReadFile(f, js, size, &n, NULL)) {
                js[n] = 0;
                const char* p = strstr(js, "\"app.update.channel\"");
                if (p && (p = strchr(p + 20, '"'))) {
                    wchar_t c[40]; int k = 0;
                    for (p++; *p && *p != '"' && k < 39; p++) c[k++] = (wchar_t)(unsigned char)*p;
                    c[k] = 0;
                    if (appsAllowed(c, L"-")) lstrcpynW(channel, c, 40);
                }
            }
            free(js);
            CloseHandle(f);
        }
    }
    const wchar_t* open = wcschr(listed, L'(');
    if (open) swscanf(open + 1, L"%15s %23[^)]", arch, locale);
    const wchar_t* target = !_wcsicmp(arch, L"x64") ? L"WINNT_x86_64-msvc-x64" : !_wcsicmp(arch, L"x86") ? L"WINNT_x86-msvc-x86" :
                            !_wcsicmp(arch, L"AArch64") || !_wcsicmp(arch, L"arm64") ? L"WINNT_aarch64-msvc-aarch64" : NULL;
    if (!target || !appsAllowed(locale, L"-")) { appsSay(L"Couldn't tell Firefox's CPU and language from \"%s\".", listed); return 1; }

    // asked as Firefox asks it; force=1 is what its own "Check for updates" sends, so a gradual rollout doesn't hide it
    RTL_OSVERSIONINFOW os = { sizeof(os) };
    LONG (WINAPI *rtlGetVersion)(RTL_OSVERSIONINFOW*) =
        (LONG (WINAPI *)(RTL_OSVERSIONINFOW*))(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!rtlGetVersion || rtlGetVersion(&os) != 0) { os.dwMajorVersion = 10; os.dwMinorVersion = 0; os.dwBuildNumber = 19045; }
    DWORD ubr = 0, cb = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, NULL, &ubr, &cb);
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    wchar_t url[1200];
    swprintf(url, 1200, L"https://aus5.mozilla.org/update/6/Firefox/%s/%s/%s/%s/%s/Windows_NT%%20%lu.%lu.0.0.%lu.%lu%%20(%s)/"
             L"ISET:SSE4_2,MEM:%llu/default/default/update.xml?force=1",
             appVer, build, target, locale, channel, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, ubr,
             wcsstr(target, L"aarch64") ? L"aarch64" : wcsstr(target, L"x86_64") ? L"x64" : L"x86", ms.ullTotalPhys >> 20);
    char* xml = NULL; DWORD status = 0;
    DWORD e = dlHttp(url, &xml, NULL, &status);
    if (e || status != 200 || !xml) { appsSay(e ? L"The update service didn't answer (error %lu)." : L"The update service didn't answer (HTTP %lu).", e ? e : status); free(xml); return 1; }
    const char* up = strstr(xml, "<update ");
    char offered[64], type[24], link[1200], hashFn[16], hash[160], sizeA[24];
    if (!up || !appsXmlAttr(up, "appVersion", offered, 64)) { appsSay(L"The update service offers nothing newer for Firefox %s (build %s).", appVer, build); free(xml); return 1; }
    wchar_t offeredW[64], latest[64];
    MultiByteToWideChar(CP_UTF8, 0, offered, -1, offeredW, 64);
    if (appsLatestVersion(APP_FIREFOX, latest, 64) && appsVerCmp(offeredW, latest) < 0) {
        // a very old Firefox is first sent to a stepping-stone version; the full setup goes straight to the newest
        appsSay(L"The update service offers %s first, not %s.", offeredW, latest); free(xml); return 1;
    }
    const char* partial = NULL;
    for (const char* p = strstr(up, "<patch "); p; p = strstr(p + 1, "<patch "))
        if (appsXmlAttr(p, "type", type, 24) && !strcmp(type, "partial")) { partial = p; break; }
    if (!partial) {
        appsSay(L"No delta update from Firefox %s: Mozilla makes them only from its last few releases.", appVer);
        free(xml); return 1;
    }
    BOOL ok = appsXmlAttr(partial, "URL", link, 1200) && appsXmlAttr(partial, "hashFunction", hashFn, 16) &&
              appsXmlAttr(partial, "hashValue", hash, 160) && appsXmlAttr(partial, "size", sizeA, 24);
    free(xml);
    unsigned long long want = ok ? _strtoui64(sizeA, NULL, 10) : 0;
    if (!ok || _stricmp(hashFn, "sha512") || strlen(hash) != 128 || !want || want > (500ull << 20) || _strnicmp(link, "https://", 8)) {
        appsSay(L"The update service's answer didn't look right."); return 1; }
    {   // only from Mozilla's own servers
        const char* h = link + 8, *slash = strchr(h, '/');
        size_t hl = slash ? (size_t)(slash - h) : strlen(h);
        BOOL moz = (hl > 12 && !_strnicmp(h + hl - 12, ".mozilla.org", 12)) || (hl > 12 && !_strnicmp(h + hl - 12, ".mozilla.net", 12)) ||
                   (hl == 11 && !_strnicmp(h, "mozilla.org", 11));
        for (const char* c = h; c < h + hl; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-') moz = FALSE;
        if (!moz) { appsSay(L"The update service pointed outside Mozilla's servers."); return 1; }
    }
    wchar_t linkW[1200];
    MultiByteToWideChar(CP_UTF8, 0, link, -1, linkW, 1200);
    appsSay(L"Delta update %s -> %s: %.1f MB (the full setup is about 90 MB).", appVer, offeredW, (double)want / 1048576.0);

    // the MAR, as update.mar in the patch folder, with update.status "pending" beside it
    wchar_t mar[MAX_PATH], stat[MAX_PATH], logf[MAX_PATH];
    swprintf(mar, MAX_PATH, L"%s\\update.mar", patch);
    swprintf(stat, MAX_PATH, L"%s\\update.status", patch);
    swprintf(logf, MAX_PATH, L"%s\\update.log", patch);
    appsSay(L"From %s", linkW);
    appsProgress(APP_STEP_DOWNLOAD, 0);
    appsSay(L"Downloading…");
    int lastMb = -1;
    e = dlHttpCore(linkW, NULL, mar, &status, FALSE, appsDownloadTick, &lastMb);
    if (e || status != 200) { appsSay(e ? L"Download failed (error %lu)." : L"Download failed (HTTP %lu).", e ? e : status); return 1; }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    unsigned long long got = GetFileAttributesExW(mar, GetFileExInfoStandard, &fa) ? fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) : 0;
    appsSayOver(L"Downloaded %.1f MB.", (double)got / 1048576.0);
    appsProgress(APP_STEP_SIGN, 0);
    wchar_t hex[129], hashW[160];
    MultiByteToWideChar(CP_UTF8, 0, hash, -1, hashW, 160);
    if (got != want || !dlHashFile(mar, CALG_SHA_512, hex) || _wcsicmp(hex, hashW)) {
        appsSay(L"The delta update doesn't match the size and SHA-512 the update service listed."); return 1; }
    appsSay(L"SHA-512 checked. Firefox's updater checks Mozilla's signature on it as well.");
    HANDLE f = CreateFileW(stat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    BOOL wrote = f != INVALID_HANDLE_VALUE && WriteFile(f, "pending\n", 8, &wr, NULL) && wr == 8;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!wrote) { appsSay(L"Couldn't prepare the update (error %lu).", GetLastError()); return 1; }

    // Firefox runs a copy of its updater from outside its folder, since the update may replace updater.exe itself
    wchar_t upd[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    swprintf(src, MAX_PATH, L"%s\\updater.exe", inst);
    swprintf(upd, MAX_PATH, L"%s\\updater.exe", dir);
    if (!CopyFileW(src, upd, FALSE)) { appsSay(L"Couldn't copy Firefox's updater (error %lu).", GetLastError()); return 1; }
    swprintf(src, MAX_PATH, L"%s\\updater.ini", inst);
    swprintf(dst, MAX_PATH, L"%s\\updater.ini", dir);
    CopyFileW(src, dst, FALSE);
    HRESULT hr = appsCheckSigner(upd, a->signer);
    if (hr != S_OK) { appsSay(L"Firefox's updater.exe isn't signed by %s (0x%08lX).", a->signer, (DWORD)hr); return 1; }

    // updater.exe arg-version patch-dir install-dir apply-to-dir which-invocation [wait-pid]: with no callback app it
    // applies the update here and now, with this helper's administrator rights, and doesn't start Firefox
    appsProgress(APP_STEP_INSTALL, 0);
    appsSay(L"Applying it with Firefox's own updater…");
    wchar_t cmd[MAX_PATH * 4 + 64];
    swprintf(cmd, MAX_PATH * 4 + 64, L"\"%s\" 3 \"%s\" \"%s\" \"%s\" first 0", upd, patch, inst, inst);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (!CreateProcessW(upd, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { appsSay(L"Couldn't start Firefox's updater (error %lu).", GetLastError()); return 1; }
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, 30 * 60 * 1000) == WAIT_TIMEOUT) code = WAIT_TIMEOUT;
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    char result[64] = "";
    f = CreateFileW(stat, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        if (ReadFile(f, result, sizeof(result) - 1, &n, NULL)) result[n] = 0;
        CloseHandle(f);
        for (char* c = result; *c; c++) if (*c == '\r' || *c == '\n') { *c = 0; break; }
    }
    if (code || strcmp(result, "succeeded")) {
        wchar_t rw[64]; MultiByteToWideChar(CP_UTF8, 0, result[0] ? result : "no status", -1, rw, 64);
        appsSay(L"Firefox's updater didn't finish (exit code %lu, \"%s\"); it puts back what it changed. Its log ends:", code, rw);
        appsSayLogTail(logf, 4);
        return 1;
    }
    appsSay(L"Firefox's updater finished: succeeded.");

    // what Firefox's updater does next when Firefox starts it: its log goes to uninstall\uninstall.update for the
    // uninstaller, and helper.exe /PostUpdate updates the uninstall entry (the version Windows lists) and the like.
    // Without /DesktopLauncher, so no desktop shortcut.
    wchar_t helper[MAX_PATH], hcmd[MAX_PATH + 100];
    swprintf(dst, MAX_PATH, L"%s\\uninstall\\uninstall.update", inst);
    CopyFileW(logf, dst, FALSE);
    swprintf(helper, MAX_PATH, L"%s\\uninstall\\helper.exe", inst);
    if (isFile(helper)) {
        swprintf(hcmd, MAX_PATH + 100, L"\"%s\" /PostUpdate /PostUpdateTarget:Installation", helper);
        if (CreateProcessW(helper, hcmd, NULL, NULL, FALSE, 0, NULL, inst, &si, &pi)) {
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
            CloseHandle(pi.hProcess);
            appsSay(L"Ran Firefox's post-update step (helper.exe /PostUpdate).");
        } else appsSay(L"Couldn't run Firefox's post-update step (error %lu).", GetLastError());
    }
    return 0;
}

static DWORD appsFirefoxDelta(const wchar_t* dir) {
    wchar_t patch[MAX_PATH], p[MAX_PATH];
    swprintf(patch, MAX_PATH, L"%s\\patch", dir);
    if (!CreateDirectoryW(patch, NULL)) { appsSay(L"Couldn't make the update folder (error %lu).", GetLastError()); return 1; }
    DWORD r = appsFirefoxDeltaIn(dir, patch);
    deleteContents(patch);
    RemoveDirectoryW(patch);
    swprintf(p, MAX_PATH, L"%s\\updater.exe", dir); DeleteFileW(p);
    swprintf(p, MAX_PATH, L"%s\\updater.ini", dir); DeleteFileW(p);
    return r;
}

static void appsIdmDriver(const AppDef* a);
static DWORD appsInstall(int app, BOOL update) {
    const AppDef* a = &APPS[app];
    // a folder only administrators and SYSTEM can write, so a normal-rights program can't swap the installer
    wchar_t dir[MAX_PATH], path[MAX_PATH], url[600];
    GetWindowsDirectoryW(dir, MAX_PATH);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"\\Temp\\WCU_Apps_%lu_%llu", GetCurrentProcessId(), GetTickCount64());
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                                                              &sa.lpSecurityDescriptor, NULL)) return APS_CODE(APS_FOLDER, GetLastError());
    BOOL made = CreateDirectoryW(dir, &sa);
    DWORD e = made ? 0 : GetLastError();
    LocalFree(sa.lpSecurityDescriptor);
    if (!made) { appsSay(L"Couldn't make the download folder (error %lu).", e); return APS_CODE(APS_FOLDER, e); }
    BOOL quick = app == APP_IDM && update;  // IDM updates with its own idmupdt.exe
    swprintf(path, MAX_PATH, L"%s\\%s", dir, quick ? L"idmupdt.exe" : a->fileName);

    DWORD result = 0;
    BOOL delta = FALSE;                     // Firefox updated with Mozilla's delta update, so no setup to run
    if (quick) {
        result = appsIdmFetchUpdate(path);
        goto run;
    }
    if (app == APP_FIREFOX && update) {
        if (appsFirefoxDelta(dir) == 0) { delta = TRUE; goto run; }
        appsSay(L"Using Firefox's full setup instead.");
    }
    appsProgress(APP_STEP_LINK, 0);
    appsSay(L"Finding the newest %s…", a->name);
    if (!appsResolveUrl(app, url, 600, &e)) { result = APS_CODE(APS_LINK, e); appsSay(L"Couldn't find it (error %lu).", e); }
    else {
        appsSay(L"From %s", url);
        // An .msi keeps the name its publisher gave it: reinstalling the same version, Windows Installer checks the
        // package against the one it remembers by name, and fails (1603) if it differs.
        const wchar_t* fn = PathFindFileNameW(url);
        BOOL plain = a->kind == KIND_MSI && wcslen(fn) < 100 && !wcschr(fn, L'?');
        for (const wchar_t* c = fn; plain && *c; c++) if (!iswalnum(*c) && *c != L'-' && *c != L'_' && *c != L'.') plain = FALSE;
        if (plain && !_wcsicmp(PathFindExtensionW(fn), L".msi") && app != APP_CRD) swprintf(path, MAX_PATH, L"%s\\%s", dir, fn);
        // An Update that would only reinstall the same version: Windows Installer refuses that (1603), so stop here
        wchar_t have[64];
        if (update && g_appsOffered[0] && appsFindInstalled(a, have, 64) && appsVerCmp(g_appsOffered, have) <= 0) {
            appsSay(L"The publisher offers %s right now, which is already installed.", g_appsOffered);
            result = APS_CODE(APS_SAME, 0);
        }
    }

    if (!result) {
        appsProgress(APP_STEP_DOWNLOAD, 0);
        appsSay(L"Downloading…");
        DWORD status = 0; int lastMb = -1;
        e = dlHttpCore(url, NULL, path, &status, FALSE, appsDownloadTick, &lastMb);
        if (e || status != 200) { result = APS_CODE(APS_DOWNLOAD, e ? e : status); appsSay(e ? L"Download failed (error %lu)." : L"Download failed (HTTP %lu).", e ? e : status); }
        else {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            double mb = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) ? (double)fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) / 1048576.0 : 0;
            appsSayOver(L"Downloaded %.1f MB.", mb);
            wchar_t hex[65];
            if (g_appsSha[0]) {                 // the publisher listed the file's SHA-256
                if (!dlSha256(path, hex) || _wcsicmp(hex, g_appsSha)) {
                    result = APS_CODE(APS_SIGN, ERROR_INVALID_DATA);
                    appsSay(L"The download's SHA-256 doesn't match the one the publisher lists. Deleted it.");
                } else appsSay(L"SHA-256 matches the publisher's (%s %s).", a->name, g_appsOffered);
            }
        }
    }
    if (!result) {
        appsProgress(APP_STEP_SIGN, 0);
        HRESULT hr = appsCheckSigner(path, a->signer);
        if (hr != S_OK) { result = APS_CODE(APS_SIGN, (DWORD)hr & 0xFFFF); appsSay(L"Signature check failed (0x%08lX). Deleted the download.", (DWORD)hr); }
        else appsSay(L"Signature checked: signed by %s", a->signer);
    }
run:
    if (!result) {
        appsProgress(APP_STEP_INSTALL, 0);
        DWORD before[64]; int nb = a->closeFirst ? appsPids(a->exe, before, 64) : 0;
        wchar_t cmd[MAX_PATH * 2 + 400], runner[MAX_PATH];
        DWORD code = 1;
        if (delta) code = 0;
        else if (a->kind == KIND_MSIX) {
            // Add-AppxPackage, for the signed-in user. If the app is open it finishes when it closes.
            GetSystemDirectoryW(runner, MAX_PATH);
            wcscat(runner, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
            swprintf(cmd, MAX_PATH * 2 + 400, L"\"%s\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"$ProgressPreference='SilentlyContinue'; "
                     L"try { Add-AppxPackage -Path '%s' -DeferRegistrationWhenPackagesAreInUse -ErrorAction Stop; exit 0 } "
                     L"catch { Write-Output $_.Exception.Message; exit 1 }\"", runner, path);
            appsSay(L"Installing the package (Add-AppxPackage)…");
            code = appsRunLogged(runner, cmd);
        } else {
            if (a->kind == KIND_MSI) {
                systemPath(runner, L"msiexec.exe");
                swprintf(cmd, MAX_PATH * 2 + 400, L"\"%s\" /i \"%s\" %s", runner, path, a->args);
                appsSay(L"Running: msiexec /i %s %s", PathFindFileNameW(path), a->args);
            } else {
                lstrcpynW(runner, path, MAX_PATH);
                swprintf(cmd, MAX_PATH * 2 + 400, L"\"%s\" %s", path, a->args);
                appsSay(L"Running: %s %s", PathFindFileNameW(path), a->args);
            }
            STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
            if (!CreateProcessW(runner, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) code = GetLastError();
            else {
                CloseHandle(pi.hThread);
                if (WaitForSingleObject(pi.hProcess, 30 * 60 * 1000) == WAIT_TIMEOUT) code = WAIT_TIMEOUT;
                else GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                if (quick && code == 0) code = appsIdmWaitSetup();
            }
        }
        if (!delta) appsSay(L"Installer finished, exit code %lu%s.", code,
                a->kind == KIND_MSI && code == ERROR_SUCCESS_REBOOT_REQUIRED ? L" (a restart finishes it)" : L"");
        if (a->kind == KIND_MSI && code == ERROR_SUCCESS_REBOOT_REQUIRED) code = 0;
        appsProgress(APP_STEP_CLOSE, 0);
        if (a->closeFirst) appsCloseNew(a->exe, before, nb, 15000);
        wchar_t ver[64];
        if (code) result = APS_CODE(APS_INSTALL, code);
        else if (!appsFindInstalled(a, ver, 64)) { result = APS_CODE(APS_MISSING, 0); appsSay(L"%s doesn't show as installed.", a->name); }
        else appsSay(L"Installed: %s %s.", a->name, ver);
        if (!result) {                      // no desktop shortcut; one in Programs, and no folder of its own
            appsTidyShortcuts(a, TRUE);
            appsPlaceShortcut(a);
            if (app == APP_IDM) appsIdmDriver(a);
        }
    }
    DeleteFileW(path);
    RemoveDirectoryW(dir);
    return result;
}

// IDM's network driver (IDMWFP). Its silent setup leaves it out; IDM then asks for admin (UAC) on every start
// to add it. Done here as idmwfp.inf does it: the .sys for this CPU copied to drivers\idmwfp.sys, an
// auto-start file-system driver service that depends on BFE, then started. Left alone if it's already there.
static void appsIdmDriver(const AppDef* a) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) { appsSay(L"Couldn't check IDM's network driver (error %lu).", GetLastError()); return; }
    SC_HANDLE s = OpenServiceW(scm, L"IDMWFP", SERVICE_QUERY_STATUS | SERVICE_START);
    wchar_t ver[64], exe[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    if (!s) {
        USHORT proc = 0, native = 0;
        if (!IsWow64Process2(GetCurrentProcess(), &proc, &native)) native = IMAGE_FILE_MACHINE_AMD64;
        const wchar_t* sys = native == IMAGE_FILE_MACHINE_ARM64 ? L"idmwfpAA.sys" : native == IMAGE_FILE_MACHINE_I386 ? L"idmwfp32.sys" : L"idmwfp64.sys";
        if (!appsFindInstalledEx(a, ver, 64, exe) || !exe[0]) { appsSay(L"Couldn't find IDM's folder to add its network driver."); CloseServiceHandle(scm); return; }
        lstrcpynW(src, exe, MAX_PATH);
        PathRemoveFileSpecW(src);
        PathAppendW(src, sys);
        GetSystemDirectoryW(dst, MAX_PATH);
        PathAppendW(dst, L"drivers\\idmwfp.sys");
        if (!CopyFileW(src, dst, FALSE)) { appsSay(L"Couldn't copy IDM's network driver (error %lu). IDM will ask for admin to add it.", GetLastError()); CloseServiceHandle(scm); return; }
        s = CreateServiceW(scm, L"IDMWFP", L"IDMWFP", SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_CHANGE_CONFIG,
                           SERVICE_FILE_SYSTEM_DRIVER, SERVICE_AUTO_START, SERVICE_ERROR_IGNORE,
                           L"System32\\drivers\\idmwfp.sys", NULL, NULL, L"BFE\0", NULL, NULL);
        if (!s) { DWORD e = GetLastError(); DeleteFileW(dst); appsSay(L"Couldn't add IDM's network driver (error %lu). IDM will ask for admin to add it.", e); CloseServiceHandle(scm); return; }
        SERVICE_DESCRIPTIONW d = { L"Internet Download Manager WFP Driver" };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
        appsSay(L"Added IDM's network driver (IDMWFP).");
    }
    SERVICE_STATUS st;
    if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_STOPPED && !StartServiceW(s, 0, NULL))
        appsSay(L"IDM's network driver starts after a restart (error %lu).", GetLastError());
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
}

static int appsHelperMain(const wchar_t* cmd) {
    int app = _wtoi(cmd + 15);                 // --apps-install=N
    if (app < 0 || app >= APP_COUNT) return (int)APS_CODE(APS_INSTALL, ERROR_INVALID_PARAMETER);
    g_appsNotify = cmdNotify(cmd);
    return (int)appsInstall(app, wcsstr(cmd, L" --update") != NULL);
}

// ---------- default apps for file types (as the signed-in user) ----------
// PotPlayer registers one ProgId per type, such as PotPlayerMini64.MP4
static BOOL appsPotProgId(const wchar_t* ext, wchar_t* out, int cch) {
    wchar_t up[16]; int i = 0;
    for (const wchar_t* p = ext + 1; *p && i < 15; p++) up[i++] = (wchar_t)towupper(*p);
    up[i] = 0;
    swprintf(out, cch, L"PotPlayerMini64.%s", up);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, out, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) { RegCloseKey(k); return TRUE; }
    wchar_t sub[64];
    swprintf(sub, 64, L"%s\\OpenWithProgids", ext);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return FALSE;
    BOOL found = FALSE;
    for (DWORD n = 0; !found; n++) {
        wchar_t name[200]; DWORD nl = 200;
        if (RegEnumValueW(k, n, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (!_wcsnicmp(name, L"PotPlayerMini64.", 16)) { lstrcpynW(out, name, cch); found = TRUE; }
    }
    RegCloseKey(k);
    return found;
}

// A packaged app's ProgId for a type is an "AppX…" name; its Application key names the package it belongs to
static BOOL appsNanaZipProgId(const wchar_t* ext, wchar_t* out, int cch) {
    wchar_t sub[64];
    swprintf(sub, 64, L"%s\\OpenWithProgids", ext);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return FALSE;
    BOOL found = FALSE;
    for (DWORD n = 0; !found; n++) {
        wchar_t name[200]; DWORD nl = 200;
        if (RegEnumValueW(k, n, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (_wcsnicmp(name, L"AppX", 4)) continue;
        wchar_t app[300], aumid[300]; DWORD cb = sizeof(aumid);
        swprintf(app, 300, L"%s\\Application", name);
        if (RegGetValueW(HKEY_CLASSES_ROOT, app, L"AppUserModelID", RRF_RT_REG_SZ, NULL, aumid, &cb) == ERROR_SUCCESS &&
            !_wcsnicmp(aumid, APPS[APP_NANAZIP].package, wcslen(APPS[APP_NANAZIP].package))) {
            lstrcpynW(out, name, cch); found = TRUE;
        }
    }
    RegCloseKey(k);
    return found;
}

// Windows' own disc image type: Explorer opens it by mounting it as a drive
static BOOL appsIsoProgId(const wchar_t* ext, wchar_t* out, int cch) {
    (void)ext;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Windows.IsoFile", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return FALSE;
    RegCloseKey(k);
    lstrcpynW(out, L"Windows.IsoFile", cch);
    return TRUE;
}

// Makes the app the default for each type, then asks Windows which ProgId it now uses. failedExt names a type that failed.
static DWORD appsSetDefaults(const wchar_t* const* exts, int n, BOOL (*find)(const wchar_t*, wchar_t*, int),
                             const wchar_t* appName, wchar_t* failedExt, int cch) {
    for (int i = 0; i < n; i++) {
        wchar_t p[200];
        lstrcpynW(failedExt, exts[i], cch);
        if (!find(exts[i], p, 200)) { appsSay(L"%s: the app didn't register this type.", exts[i]); return ERROR_NOT_FOUND; }
        DWORD e = assocSetDefault(exts[i], p);
        if (e) { appsSay(L"%s: couldn't set %s (error %lu).", exts[i], p, e); return e; }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    for (int i = 0; i < n; i++) {
        wchar_t want[200], got[200]; DWORD cc = 200;
        lstrcpynW(failedExt, exts[i], cch);
        find(exts[i], want, 200);
        if (AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, exts[i], NULL, got, &cc) != S_OK || _wcsicmp(got, want)) {
            appsSay(L"%s: Windows still opens it with something else.", exts[i]);
            return ERROR_INVALID_DATA;
        }
        appsSay(L"%s now opens with %s (%s).", exts[i], appName, want);
    }
    failedExt[0] = 0;
    return 0;
}

// ---------- the window's side ----------
static wchar_t g_appsLatest[APP_COUNT][64];   // newest offered, empty if unknown
static volatile LONG g_appsChecking = 0;
static ULONGLONG g_appsCheckedAt = 0;
static BOOL g_appsUpdating = FALSE;           // the job running is an update
static BOOL g_appsWasThere = FALSE;           // the app was installed before this job

static DWORD WINAPI appsLatestProc(LPVOID arg) {
    (void)arg;
    wchar_t* list = (wchar_t*)calloc(APP_COUNT * 64, sizeof(wchar_t));
    if (list) for (int i = 0; i < APP_COUNT; i++) appsLatestVersion(i, list + i * 64, 64);
    if (!list || !PostMessageW(g_hwnd, WM_APP_APPSLATEST, 0, (LPARAM)list)) { free(list); InterlockedExchange(&g_appsChecking, 0); }
    return 0;
}

// Looks up the newest versions in the background, at most every 10 minutes unless forced
static void appsCheckLatest(BOOL force) {
    if (!force && g_appsCheckedAt && GetTickCount64() - g_appsCheckedAt < 10 * 60 * 1000) return;
    if (InterlockedCompareExchange(&g_appsChecking, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, appsLatestProc, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_appsChecking, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void appsOnLatest(wchar_t* list) {
    for (int i = 0; i < APP_COUNT; i++) {           // a failed lookup keeps what was known
        if (list[i * 64]) lstrcpynW(g_appsLatest[i], list + i * 64, 64);
    }
    free(list);
    g_appsCheckedAt = GetTickCount64();
    InterlockedExchange(&g_appsChecking, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static BOOL appsHasUpdate(int i) {
    const wchar_t* v = g_appsVer[i];
    return g_appsLatest[i][0] && v[0] && iswdigit(v[0]) && appsVerCmp(g_appsLatest[i], v) > 0;
}

typedef struct { HANDLE helper; int app; BOOL update; } AppsJob;
static DWORD g_appsCode;
static const wchar_t* g_appsWhat;
static wchar_t g_appsExt[16];

static DWORD WINAPI appsProc(LPVOID arg) {
    AppsJob* j = (AppsJob*)arg;
    DWORD code = waitExitCode(j->helper);
    DWORD fin = 0;
    g_appsWhat = NULL; g_appsExt[0] = 0;
    // the shortcuts the installer also made for this user (done here, as the signed-in user)
    if (!code) appsTidyShortcuts(&APPS[j->app], FALSE);
    if (!code && !j->update) {          // an update keeps the settings and defaults already there
        if (j->app == APP_POT) {
            PostMessageW(g_hwnd, WM_APP_APPSPROG, APP_STEP_SETTINGS, 0);
            g_appsWhat = L"apply PotPlayer's settings";
            appsSay(L"Applying PotPlayer's settings (reg import)…");
            fin = regImportEmbedded(RES_POT_REG, FALSE);
            if (fin) appsSay(L"Settings import failed (error %lu).", fin);
            else {
                appsSay(L"Settings applied.");
                g_appsWhat = L"make PotPlayer the default";
                fin = appsSetDefaults(POT_EXTS, (int)(sizeof(POT_EXTS) / sizeof(POT_EXTS[0])), appsPotProgId, L"PotPlayer", g_appsExt, 16);
            }
        } else if (j->app == APP_NANAZIP) {
            PostMessageW(g_hwnd, WM_APP_APPSPROG, APP_STEP_SETTINGS, 0);
            g_appsWhat = L"make NanaZip the default";
            fin = appsSetDefaults(NANAZIP_EXTS, (int)(sizeof(NANAZIP_EXTS) / sizeof(NANAZIP_EXTS[0])), appsNanaZipProgId, L"NanaZip", g_appsExt, 16);
        }
    }
    free(j);
    g_appsCode = code;
    PostMessageW(g_hwnd, WM_APP_APPSDONE, (WPARAM)fin, 0);
    return 0;
}

// ---------- an app from the Microsoft Store (in the window's process, as the signed-in user) ----------
// Runs a console program hidden, its UTF-8 output into the log: a lone spinner character is dropped, and a progress
// bar replaces the one before it. Returns its exit code.
static DWORD appsRunLines(const wchar_t* exe, wchar_t* cmd) {
    HANDLE rd = NULL; PROCESS_INFORMATION pi;
    DWORD err = spawnPiped(exe, cmd, &rd, &pi);
    if (err) return err;
    char buf[1024], line[APPS_LOG_LINE * 2]; int ln = 0; DWORD got;
    BOOL lastBar = FALSE;
    for (;;) {
        BOOL more = ReadFile(rd, buf, sizeof(buf), &got, NULL) && got;
        for (DWORD i = 0; i <= (more ? got - 1 : 0) && (more || ln); i++) {
            char c = more ? buf[i] : '\n';
            if (c == '\r' || c == '\n' || ln == (int)sizeof(line) - 1) {
                line[ln] = 0;
                wchar_t w[APPS_LOG_LINE];
                if (ln && MultiByteToWideChar(CP_UTF8, 0, line, -1, w, APPS_LOG_LINE)) {
                    w[APPS_LOG_LINE - 1] = 0;
                    const wchar_t* s = w;
                    while (*s == L' ' || *s == 0xFEFF) s++;
                    BOOL spin = !s[1] && (s[0] == L'-' || s[0] == L'\\' || s[0] == L'|' || s[0] == L'/');
                    if (*s && !spin) {
                        BOOL bar = wcschr(s, 0x2588) || wcschr(s, 0x2592);
                        if (bar && lastBar) appsSayOver(L"  %s", s); else appsSay(L"  %s", s);
                        lastBar = bar;
                    }
                }
                ln = 0;
                if (c != '\r' && c != '\n') line[ln++] = c;
            } else line[ln++] = c;
        }
        if (!more) break;
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    return waitExitCode(pi.hProcess);
}

// winget gets it from the Store (--force also takes over a copy installed from elsewhere, so the Store updates it from
// then on); without winget, the app's Store page opens. Then the same finishing steps as an install by the helper.
static DWORD WINAPI appsStoreProc(LPVOID arg) {
    int app = (int)(INT_PTR)arg;
    const AppDef* a = &APPS[app];
    DWORD code = 0, fin = 0;
    g_appsWhat = NULL; g_appsExt[0] = 0;
    wchar_t wg[MAX_PATH] = L"", cmd[MAX_PATH + 200], ver[64];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe", wg, MAX_PATH);
    if (!isFile(wg)) {
        wchar_t page[120];
        swprintf(page, 120, L"ms-windows-store://pdp/?ProductId=%s", a->storeId);
        ShellExecuteW(NULL, L"open", page, NULL, NULL, SW_SHOWNORMAL);
        appsSay(L"winget (App Installer) isn't on this PC, so %s's Microsoft Store page is open. Press Get there.", a->name);
        code = APS_CODE(APS_LINK, ERROR_FILE_NOT_FOUND);
    } else {
        PostMessageW(g_hwnd, WM_APP_APPSPROG, APP_STEP_INSTALL, 0);
        appsSay(L"Getting %s from the Microsoft Store (winget, product %s)…", a->name, a->storeId);
        swprintf(cmd, MAX_PATH + 200, L"\"%s\" install --id %s --source msstore --force --accept-package-agreements "
                                      L"--accept-source-agreements --disable-interactivity", wg, a->storeId);
        DWORD e = appsRunLines(wg, cmd);
        appsSay(L"winget finished, exit code %ld.", (long)e);
        if (e) code = APS_CODE(APS_INSTALL, e);
        else if (!appsFindInstalled(a, ver, 64)) { code = APS_CODE(APS_MISSING, 0); appsSay(L"%s doesn't show as installed.", a->name); }
        else appsSay(L"Installed: %s %s, from the Microsoft Store, which keeps it updated.", a->name, ver);
    }
    if (!code && app == APP_NANAZIP) {
        PostMessageW(g_hwnd, WM_APP_APPSPROG, APP_STEP_SETTINGS, 0);
        g_appsWhat = L"make NanaZip the default";
        fin = appsSetDefaults(NANAZIP_EXTS, (int)(sizeof(NANAZIP_EXTS) / sizeof(NANAZIP_EXTS[0])), appsNanaZipProgId, L"NanaZip", g_appsExt, 16);
        if (!fin) {                       // NanaZip also offers to open disc images; they stay with Explorer, which mounts them
            static const wchar_t* ISO_EXT[] = { L".iso" };
            g_appsWhat = L"keep ISO files with Windows Explorer";
            fin = appsSetDefaults(ISO_EXT, 1, appsIsoProgId, L"Windows Explorer", g_appsExt, 16);
        }
    }
    g_appsCode = code;
    PostMessageW(g_hwnd, WM_APP_APPSDONE, (WPARAM)fin, 0);
    return 0;
}

static void appsStoreStart(int app) {
    const AppDef* a = &APPS[app];
    wchar_t title[80], body[400];
    swprintf(title, 80, L"Install %s?", a->name);
    swprintf(body, 400, L"This installs %s from the Microsoft Store, which keeps it updated from then on%s%s.", a->name,
             g_appsVer[app][0] ? L" (it replaces the copy installed now and keeps its settings)" : L"",
             app == APP_NANAZIP ? L", and makes it the default for ZIP, RAR and 7Z files. ISO files stay with Windows Explorer" : L"");
    if (!showDialog(g_hwnd, title, body, NULL, 0, L"Install", L"Cancel", 1)) return;    // no UAC here, so this is the one prompt
    if (!jobBegin()) return;
    g_appsUpdating = FALSE;
    g_appsWasThere = g_appsVer[app][0] != 0;
    g_appsRun = app; g_appsStep = APP_STEP_INSTALL; g_appsMb = 0; g_appsMsg[0] = 0;
    if (g_appsLogN) appsLogAdd(L"", FALSE);
    wchar_t head[200];
    swprintf(head, 200, L"> Install %s from the Microsoft Store", a->name);
    appsLogAdd(head, FALSE);
    startWorker(appsStoreProc, (LPVOID)(INT_PTR)app);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void appsStart(int app, BOOL update) {
    if (g_busy != 0 || app < 0 || app >= APP_COUNT) return;
    const AppDef* a = &APPS[app];
    if (a->storeId) { appsStoreStart(app); return; }
    BOOL reinstall = !update && app == APP_IDM && g_appsVer[app][0];    // IDM's full setup over the copy there
    DWORD pids[4];
    if (a->closeFirst && appsPids(a->exe, pids, 4)) {
        wchar_t title[80], body[300];
        swprintf(title, 80, L"Close %s first", a->name);
        swprintf(body, 300, app == APP_IDM
            ? L"%s is running. It may only show in the notification area: right-click its icon and choose Exit. Then press %s again."
            : L"%s is open. Close it, then press %s again.", a->name, update ? L"Update" : reinstall ? L"Reinstall IDM" : a->label);
        okDialog(title, body);
        return;
    }
    if (!update && askInApp()) {            // Update starts at once (user's choice); install and reinstall ask first
        wchar_t title[80], body[400];
        if (reinstall) {
            swprintf(title, 80, L"Reinstall %s?", a->name);
            swprintf(body, 400, L"This downloads the full setup of the newest %s from its publisher and installs it silently over %s. "
                     L"Its settings are kept. Afterwards you can import your own settings from a .reg file.", a->name, g_appsVer[app]);
        } else {
            swprintf(title, 80, L"Install %s?", a->name);
            if (app == APP_POT)
                swprintf(body, 400, L"This downloads the newest %s, installs it silently, applies the settings built into this app "
                         L"(replacing its current settings), and makes it the default for MP4, MKV, MPG, MPEG and WMV files.", a->name);
            else if (app == APP_NANAZIP)
                swprintf(body, 400, L"This downloads the newest %s, installs it silently, and makes it the default for ZIP, RAR and 7Z files.", a->name);
            else if (app == APP_IDM)
                swprintf(body, 400, L"This downloads the newest %s from its publisher and installs it silently. "
                         L"Afterwards you can import your own settings from a .reg file.", a->name);
            else swprintf(body, 400, L"This downloads the newest %s from its publisher and installs it silently.", a->name);
        }
        if (!showDialog(g_hwnd, title, body, NULL, 0, reinstall ? L"Reinstall" : L"Install", L"Cancel", 1)) return;
    }
    wchar_t args[32]; BOOL cancelled = FALSE;
    swprintf(args, 32, L"--apps-install=%d%s", app, update ? L" --update" : L"");
    HANDLE helper = NULL;
    if (g_elevated) helper = startHelperDirect(args);
    else if (!startElevatedHelper(args, &helper, &cancelled)) helper = NULL;
    if (!helper) {
        wcscpy(g_appsMsg, cancelled ? L"Cancelled. Nothing was installed." : L"Couldn't start the administrator helper. Nothing was installed.");
        appsLogAdd(g_appsMsg, FALSE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    AppsJob* j = (AppsJob*)malloc(sizeof(AppsJob));
    if (!j || !jobBegin()) {
        free(j);
        WaitForSingleObject(helper, INFINITE); CloseHandle(helper);
        return;
    }
    j->helper = helper; j->app = app; j->update = update;
    g_appsUpdating = update;
    g_appsWasThere = g_appsVer[app][0] != 0;
    g_appsRun = app; g_appsStep = APP_STEP_LINK; g_appsMb = 0; g_appsMsg[0] = 0;
    if (g_appsLogN) appsLogAdd(L"", FALSE);
    wchar_t head[200];
    if (update) swprintf(head, 200, L"> Update %s %s -> %s", a->name, g_appsVer[app], g_appsLatest[app]);
    else swprintf(head, 200, reinstall ? L"> Reinstall %s (full setup)" : L"> Install %s", a->name);
    appsLogAdd(head, FALSE);
    startWorker(appsProc, j);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void appsClick(int app) { appsStart(app, FALSE); }
static void appsUpdateClick(int app) { if (app >= 0 && app < APP_COUNT && appsHasUpdate(app)) appsStart(app, TRUE); }

static void appsOnProgress(int step, int mb) {
    if (g_appsRun < 0) return;
    g_appsStep = step; g_appsMb = mb;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void appsIdmOfferReg(void);
static void appsOnDone(DWORD fin) {
    int app = g_appsRun;
    DWORD code = g_appsCode;
    g_appsRun = -1;
    InterlockedExchange(&g_busy, 0);
    appsRefresh();
    if (app < 0) return;
    const AppDef* a = &APPS[app];
    DWORD step = code >> 24, detail = code & 0xFFFFFF;
    if (code == 0 && fin == 0 && g_appsUpdating) {
        if (g_appsLatest[app][0] && appsVerCmp(g_appsVer[app], g_appsLatest[app]) < 0)
            swprintf(g_appsMsg, 400, a->kind == KIND_MSIX
                ? L"%s %s is downloaded and finishes installing when %s is closed (it shows %s until then)."
                : L"The %s installer finished, but %s still shows as %s.", a->name,
                a->kind == KIND_MSIX ? g_appsLatest[app] : a->name, a->kind == KIND_MSIX ? a->name : g_appsVer[app], g_appsVer[app]);
        else swprintf(g_appsMsg, 400, L"%s is updated to %s.", a->name, g_appsVer[app]);
    } else if (code == 0 && fin == 0) {
        if (app == APP_POT)
            swprintf(g_appsMsg, 400, L"%s %s is installed, with its settings applied. It's now the default for MP4, MKV, MPG, MPEG and WMV files.",
                     a->name, g_appsVer[app]);
        else if (app == APP_NANAZIP)
            swprintf(g_appsMsg, 400, L"%s %s is installed from the Microsoft Store, which keeps it updated. It's now the default for ZIP, RAR and 7Z files; "
                                     L"ISO files open with Windows Explorer.",
                     a->name, g_appsVer[app]);
        else if (app == APP_CRD)
            swprintf(g_appsMsg, 400, L"%s Host %s is installed. To reach this PC, set it up at remotedesktop.google.com/access.",
                     a->name, g_appsVer[app]);
        else if (app == APP_TAILSCALE)
            swprintf(g_appsMsg, 400, g_appsWasThere ? L"%s %s is installed." : L"%s %s is installed. Sign in from its icon in the notification area.", a->name, g_appsVer[app]);
        else swprintf(g_appsMsg, 400, L"%s %s is installed.", a->name, g_appsVer[app]);
    } else if (code == 0) {
        if (fin == ERROR_NOT_FOUND && g_appsWhat && g_appsExt[0])
            swprintf(g_appsMsg, 400, L"%s is installed, but it didn't register %s files, so it couldn't be made their default.", a->name, g_appsExt);
        else if (fin == ERROR_INVALID_DATA && g_appsExt[0])
            swprintf(g_appsMsg, 400, L"%s is installed, but Windows didn't accept it as the default for %s files. "
                                     L"Set it in Settings > Apps > Default apps.", a->name, g_appsExt);
        else swprintf(g_appsMsg, 400, L"%s is installed, but the app couldn't %s (error %lu).", a->name, g_appsWhat ? g_appsWhat : L"finish", fin);
    } else if (a->storeId && step == APS_LINK)
        swprintf(g_appsMsg, 400, L"winget isn't on this PC, so %s's Microsoft Store page is open instead. Press Get there.", a->name);
    else if (a->storeId && step == APS_INSTALL)
        swprintf(g_appsMsg, 400, L"winget couldn't install %s from the Microsoft Store. See the lines above.", a->name);
    else if (step == APS_LINK)
        swprintf(g_appsMsg, 400, L"Couldn't find the newest %s from its publisher (error %lu). Nothing was installed.", a->name, detail);
    else if (step == APS_DOWNLOAD)
        swprintf(g_appsMsg, 400, L"Couldn't download %s (error %lu). Check the connection. Nothing was installed.", a->name, detail);
    else if (step == APS_SIGN && detail == ERROR_INVALID_DATA)
        swprintf(g_appsMsg, 400, L"The downloaded %s installer didn't match the SHA-256 its publisher lists, so it was deleted and nothing was installed.", a->name);
    else if (step == APS_SIGN)
        swprintf(g_appsMsg, 400, L"The downloaded %s installer isn't signed by %s, so it was deleted and nothing was installed.", a->name, a->signer);
    else if (step == APS_MISSING)
        swprintf(g_appsMsg, 400, L"The %s installer finished, but %s doesn't show as installed.", a->name, a->name);
    else if (step == APS_FOLDER)
        swprintf(g_appsMsg, 400, L"Couldn't make a folder for the download (error %lu). Nothing was installed.", detail);
    else if (step == APS_SAME)
        swprintf(g_appsMsg, 400, L"%s's newer version is still being rolled out, and right now only %s is offered. Nothing was changed; try Update again later.",
                 a->name, g_appsVer[app]);
    else
        swprintf(g_appsMsg, 400, L"The %s installer stopped with code %lu.", a->name, detail);
    appsLogAdd(g_appsMsg, FALSE);
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (app == APP_IDM && code == 0 && fin == 0 && !g_appsUpdating) appsIdmOfferReg();
}

// ---------- IDM: the user's own settings after a fresh install ----------
#define IDM_KEY L"Software\\DownloadManager"

// A .reg file (ANSI or UTF-16) that writes HKEY_CURRENT_USER\Software\DownloadManager
static BOOL appsIdmRegOk(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(h, NULL), got = 0;
    BOOL ok = FALSE;
    if (size > 2 && size < 16 * 1024 * 1024) {
        BYTE* raw = (BYTE*)malloc(size + 2);
        wchar_t* w = NULL;
        if (raw && ReadFile(h, raw, size, &got, NULL) && got == size) {
            raw[size] = raw[size + 1] = 0;
            if (raw[0] == 0xFF && raw[1] == 0xFE) w = _wcsdup((wchar_t*)(raw + 2));
            else {
                int n = MultiByteToWideChar(CP_ACP, 0, (char*)raw, -1, NULL, 0);
                w = n > 0 ? (wchar_t*)malloc(n * sizeof(wchar_t)) : NULL;
                if (w) MultiByteToWideChar(CP_ACP, 0, (char*)raw, -1, w, n);
            }
        }
        if (w) {
            _wcslwr(w);
            ok = (!wcsncmp(w, L"windows registry editor", 23) || !wcsncmp(w, L"regedit4", 8)) &&
                 (wcsstr(w, L"[hkey_current_user\\software\\downloadmanager") || wcsstr(w, L"[hkcu\\software\\downloadmanager"));
            free(w);
        }
        free(raw);
    }
    CloseHandle(h);
    return ok;
}

// Offers to replace IDM's fresh defaults with the user's own .reg. Only after an install, never an update.
static void appsIdmOfferReg(void) {
    if (!showDialog(g_hwnd, L"Import your IDM settings?",
                    L"IDM was installed with its default settings. You can import your own .reg file instead: "
                    L"IDM's current settings (HKEY_CURRENT_USER\\Software\\DownloadManager) are deleted first, then your file is imported.",
                    NULL, 0, L"Choose .reg file", L"Not now", 1)) return;
    wchar_t path[MAX_PATH];
    if (!pickOpen(path, L"Registry files (*.reg)\0*.reg\0", L"Choose your IDM settings")) {
        appsLogAdd(L"IDM settings import cancelled. IDM keeps its default settings.", FALSE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (!appsIdmRegOk(path)) {
        okDialog(L"Not an IDM settings file",
                 L"That file isn't a .reg file for HKEY_CURRENT_USER\\Software\\DownloadManager, so nothing was changed.");
        appsLogAdd(L"IDM settings not imported: the file has no DownloadManager key. Nothing was changed.", FALSE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    // a copy of the current key, put back if the import fails
    wchar_t exe[MAX_PATH], dir[MAX_PATH], bak[MAX_PATH], cmd[MAX_PATH * 2 + 96];
    systemPath(exe, L"reg.exe");
    GetTempPathW(MAX_PATH, dir);
    swprintf(bak, MAX_PATH, L"%swcu_idm_%lu_%llu.reg", dir, GetCurrentProcessId(), GetTickCount64());
    swprintf(cmd, MAX_PATH * 2 + 96, L"\"%s\" export \"HKCU\\%s\" \"%s\" /y", exe, IDM_KEY, bak);
    BOOL haveBak = runWithPercent(exe, cmd, NULL) == 0;

    LSTATUS d = RegDeleteTreeW(HKEY_CURRENT_USER, IDM_KEY);
    if (d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND) RegDeleteKeyW(HKEY_CURRENT_USER, IDM_KEY);
    DWORD e = (d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND) ? regImportFile(path, FALSE) : (DWORD)d;
    if (e) {
        if (haveBak && d == ERROR_SUCCESS) regImportFile(bak, FALSE);
        swprintf(g_appsMsg, 400, L"IDM is installed, but your settings couldn't be imported (error %lu). %s", e,
                 d == ERROR_SUCCESS ? (haveBak ? L"Its previous settings were put back." : L"Its settings are now empty.")
                                    : L"Nothing was changed.");
    } else {
        const wchar_t* name = wcsrchr(path, L'\\');
        swprintf(g_appsMsg, 400, L"IDM %s is installed, with your settings from %s.", g_appsVer[APP_IDM], name ? name + 1 : path);
    }
    if (haveBak) DeleteFileW(bak);
    appsLogAdd(g_appsMsg, FALSE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

#include "gpu.c"

// ---------- the page ----------
static void appsDrawLog(int cL, int cR, int oTop, const wchar_t* now);
static void drawApps(int cL, int cR) {
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    int x1 = cL + dp(20), x2 = cR - dp(20), bh = dp(30), bw = dp(172), rowGap = dp(6);   // compact, so the log below has room
    int noteH = dp(56);
    int cardB = top + dp(92) + (APP_COUNT + GPU_N) * (bh + rowGap) + dp(4) + noteH + dp(8);
    fillRoundRect(cL, top, cR, cardB, dp(8), sf.r, sf.g, sf.b, sf.a);
    drawPageHeader(0xE71D, cL, cR, top, L"Install Apps", L"The newest build of each app, installed silently.");

    int y = top + dp(92);
    BOOL en = g_busy == 0, shield = !g_elevated;
    g_aioSmall = TRUE;                   // every button here in the smaller font, so the rows look even
    static const int IDS[APP_COUNT] = { A_P_FIREFOX, A_P_IDM, A_P_POT, A_P_CRD, A_P_NANAZIP, A_P_TAILSCALE };
    for (int i = 0; i < APP_COUNT; i++) {
        // IDM installed: its button reinstalls with the full setup (Update uses IDM's own smaller update)
        const wchar_t* lbl = i == APP_IDM && g_appsVer[i][0] ? L"Reinstall IDM" : APPS[i].label;
        aioButton(IDS[i], x1, y, x1 + bw, y + bh, lbl, FALSE, en, shield && !APPS[i].storeId);
        wchar_t st[160];
        BOOL upd = g_appsRun != i && appsHasUpdate(i);
        if (g_appsRun == i) wcscpy(st, g_appsUpdating ? L"Updating…" : L"Installing…");
        else if (APPS[i].storeId && g_appsVer[i][0]) swprintf(st, 160, L"Installed: %s · the Microsoft Store keeps it updated", g_appsVer[i]);
        else if (APPS[i].storeId) wcscpy(st, L"Not installed · from the Microsoft Store");
        else if (upd) swprintf(st, 160, L"Installed: %s · %s available", g_appsVer[i], g_appsLatest[i]);
        else if (g_appsVer[i][0] && g_appsLatest[i][0] && iswdigit(g_appsVer[i][0])) swprintf(st, 160, L"Installed: %s (latest)", g_appsVer[i]);
        else if (g_appsVer[i][0]) swprintf(st, 160, L"Installed: %s%s", g_appsVer[i], g_appsChecking ? L" · checking for updates…" : L"");
        else wcscpy(st, L"Not installed");
        // a small Update button at the row's right end, only when a newer version is offered
        int ubw = dp(68), ubh = dp(24), textR = x2;
        if (upd) {
            int uy = y + (bh - ubh) / 2;
            aioButton(A_P_UPD0 + i, x2 - ubw, uy, x2, uy + ubh, L"Update", TRUE, en, FALSE);
            textR = x2 - ubw - dp(10);
        }
        RECT r = { x1 + bw + dp(14), y, textR, y + bh };
        drawText(g_fSmall, st, r, upd ? textRGB() : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += bh + rowGap;
    }
    gpuDrawRows(x1, x2, bw, bh, rowGap, &y);
    g_aioSmall = FALSE;
    y += dp(4);
    RECT n1 = { x1, y, x2, y + noteH };
    drawText(g_fSmall, L"Installed silently from the publisher, signature checked, with one shortcut in Start Menu and none on the desktop. "
                       L"Pot Player gets this app's settings and opens MP4, MKV, MPG, MPEG and WMV. NanaZip comes from the "
                       L"Microsoft Store, which updates it, and opens ZIP, RAR and 7Z.",
             n1, mutedRGB(), DT_LEFT | DT_WORDBREAK);

    wchar_t now[200] = L"";
    if (gpuNowText(now, 200)) {}
    else if (g_appsRun >= 0) {
        const wchar_t* name = APPS[g_appsRun].name;
        switch (g_appsStep) {
        case APP_STEP_LINK:     swprintf(now, 200, L"%s: finding the newest build…", name); break;
        case APP_STEP_DOWNLOAD: swprintf(now, 200, L"%s: downloading… %d MB", name, g_appsMb); break;
        case APP_STEP_SIGN:     swprintf(now, 200, L"%s: checking the signature…", name); break;
        case APP_STEP_INSTALL:  swprintf(now, 200, L"%s: installing…", name); break;
        case APP_STEP_CLOSE:    swprintf(now, 200, L"%s: finishing…", name); break;
        default:                swprintf(now, 200, L"%s: setting default apps…", name); break;
        }
    }
    appsDrawLog(cL, cR, cardB + dp(12), now);
}

// Scrolling the log: the mouse wheel over it, or dragging its scrollbar. It follows the newest line until scrolled
// up, and follows again once scrolled back to the bottom.
static int  g_logTop = 0, g_logRows = 0, g_logVisible = 1, g_logThumbH = 0, g_logDragOfs = 0;
static BOOL g_logFollow = TRUE, g_logDrag = FALSE;
static RECT g_rcLogBox, g_rcLogBar;

static BOOL appsLogWheel(int x, int y, int delta) {
    POINT p = { x, y };
    if (!PtInRect(&g_rcLogBox, p) || g_logRows <= g_logVisible) return FALSE;
    g_logTop += (delta > 0) ? -3 : 3;
    if (g_logTop < 0) g_logTop = 0;
    g_logFollow = g_logTop >= g_logRows - g_logVisible;
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}
static void appsLogDragTo(int y) {
    int span = (g_rcLogBar.bottom - g_rcLogBar.top) - g_logThumbH, maxTop = g_logRows - g_logVisible;
    if (span <= 0 || maxTop <= 0) return;
    int v = y - g_rcLogBar.top - g_logDragOfs;
    if (v < 0) v = 0;
    if (v > span) v = span;
    g_logTop = (int)((long long)v * maxTop / span);
    g_logFollow = g_logTop >= maxTop;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
// Links in the log: underlined, and a click copies the whole link (also when it wraps onto a second row)
#define LOG_LINKS 64
#define TIMER_LOGCOPIED 11
typedef struct { RECT rc; wchar_t url[APPS_LOG_LINE]; } LogLink;
static LogLink g_logLinks[LOG_LINKS];
static int g_logLinkN = 0;
static ULONGLONG g_logCopiedAt = 0;

// The next link in s at or after from: [*us, *ue). FALSE if there's none.
static BOOL appsLogLink(const wchar_t* s, int from, int* us, int* ue) {
    const wchar_t* a = wcsstr(s + from, L"https://");
    const wchar_t* b = wcsstr(s + from, L"http://");
    if (!a || (b && b < a)) a = b;
    if (!a) return FALSE;
    int e = (int)(a - s);
    while (s[e] && s[e] != L' ' && s[e] != L'"' && s[e] != L'\'' && s[e] != L'<' && s[e] != L'>') e++;
    while (e > a - s && wcschr(L".,;:)]", s[e - 1])) e--;       // the sentence's punctuation isn't part of it
    *us = (int)(a - s); *ue = e;
    return e - *us > 8;
}
static const wchar_t* appsLogLinkAt(int x, int y) {
    POINT p = { x, y };
    for (int i = 0; i < g_logLinkN; i++) if (PtInRect(&g_logLinks[i].rc, p)) return g_logLinks[i].url;
    return NULL;
}

static BOOL appsLogMouseDown(int x, int y) {
    POINT p = { x, y };
    const wchar_t* url = appsLogLinkAt(x, y);
    if (url) {
        copyText(url);
        g_logCopiedAt = GetTickCount64();
        SetTimer(g_hwnd, TIMER_LOGCOPIED, 2500, NULL);    // the notice goes after a while
        InvalidateRect(g_hwnd, NULL, FALSE);
        return TRUE;
    }
    if (g_logRows <= g_logVisible || !PtInRect(&g_rcLogBar, p)) return FALSE;
    int span = (g_rcLogBar.bottom - g_rcLogBar.top) - g_logThumbH, maxTop = g_logRows - g_logVisible;
    int ty = g_rcLogBar.top + (maxTop > 0 ? (int)((long long)span * g_logTop / maxTop) : 0);
    g_logDragOfs = (y >= ty && y < ty + g_logThumbH) ? y - ty : g_logThumbH / 2;
    g_logDrag = TRUE;
    SetCapture(g_hwnd);
    appsLogDragTo(y);
    return TRUE;
}

// The log: a small console from oTop down to the bottom of the window, shared with the Office page
static void appsDrawLog(int cL, int cR, int oTop, const wchar_t* now) {
    int pad = dp(16), x1 = cL + dp(20), x2 = cR - dp(20);
    Col sf = surfaceCol();
    int oB = g_bh - pad, hdrH = dp(32);
    SetRectEmpty(&g_rcLogBox); SetRectEmpty(&g_rcLogBar);
    g_logLinkN = 0;
    if (oB - oTop < hdrH + dp(30)) return;
    fillRoundRect(cL, oTop, cR, oB, dp(8), sf.r, sf.g, sf.b, sf.a);
    RECT oh = { x1, oTop, x1 + dp(80), oTop + hdrH };
    BOOL off = termOff();
    drawText(g_fBodyBold, L"Progress", oh, off ? mutedRGB() : textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int statusR = x2;
    if (g_logCopiedAt && GetTickCount64() - g_logCopiedAt < 2400) {     // the notice after a link was clicked
        const wchar_t* note = L"Link copied to the clipboard";
        int w = textWidth(g_fSmall, note), gw = dp(22);
        RECT nr = { x2 - w, oTop, x2, oTop + hdrH };
        COLORREF ok = noteGreen();
        drawGlyph(g_fIcon, 0xE73E, x2 - w - gw / 2 - dp(2), oTop + hdrH / 2, ok);   // a tick
        drawText(g_fSmall, note, nr, ok, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        statusR = x2 - w - gw - dp(12);
    }
    RECT os = { oh.right + dp(8), oTop, statusR, oTop + hdrH };
    drawText(g_fSmall, off ? L"" : now, os, g_accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT box = { cL + dp(12), oTop + hdrH, cR - dp(12), oB - dp(12) };
    Col kf = g_dark ? (Col){ 0,0,0, 0.28 } : (Col){ 0,0,0, 0.045 };
    fillRoundRect(box.left, box.top, box.right, box.bottom, dp(4), kf.r, kf.g, kf.b, kf.a);
    if (off) {                                           // another section's job has the terminal (termOwner, repair.c)
        RECT hint = { box.left + dp(10), box.top + dp(6), box.right - dp(10), box.top + dp(23) };
        drawText(g_fSmall, termOffText(), hint, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    g_rcLogBox = box;
    int rowH = dp(17), tl = box.left + dp(10), tr = box.right - dp(18), tt = box.top + dp(6);   // room on the right for the scrollbar
    int visible = (box.bottom - dp(6) - tt) / rowH;
    if (visible < 1) visible = 1;
    EnterCriticalSection(&g_lock);
    if (g_appsLogN == 0) {
        RECT hint = { tl, tt, tr, tt + rowH };
        drawText(g_fSmall, L"Each step of an install or update shows here.", hint, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // Long lines wrap onto further rows (at a space where there is one) rather than being cut off. The font is
    // monospaced, so a row holds a fixed number of characters. Only the newest rows that fit are shown.
    double charW = textWidth(g_fMono, L"MMMMMMMMMM") / 10.0;
    int cols = charW > 0 ? (int)((tr - tl) / charW) - 1 : 200;       // one spare, so a row never runs into the edge
    if (cols < 20) cols = 20;
    static struct { short line, start, len; } rows[APPS_LOG_MAX * 8];
    int nRows = 0;
    for (int i = 0; i < g_appsLogN; i++) {
        const wchar_t* s = g_appsLog[i];
        int n = (int)wcslen(s), p = 0;
        if (n == 0 && nRows < APPS_LOG_MAX * 8) { rows[nRows].line = (short)i; rows[nRows].start = 0; rows[nRows].len = 0; nRows++; }
        while (p < n && nRows < APPS_LOG_MAX * 8) {
            int room = p ? cols - 2 : cols;                // continuation rows are indented by two
            int next, len = wrapRow(s, n, p, room, &next);
            rows[nRows].line = (short)i; rows[nRows].start = (short)p; rows[nRows].len = (short)len; nRows++;
            p = next;
        }
    }
    int maxTop = nRows > visible ? nRows - visible : 0;
    if (g_logFollow || g_logTop > maxTop) g_logTop = maxTop;         // the newest rows, unless scrolled up
    if (g_logTop < 0) g_logTop = 0;
    if (g_logTop >= maxTop) g_logFollow = TRUE;
    g_logRows = nRows; g_logVisible = visible;
    int first = g_logTop;
    for (int r = first; r < nRows && r < first + visible; r++) {
        const wchar_t* s = g_appsLog[rows[r].line];
        wchar_t seg[APPS_LOG_LINE + 2];
        int ind = rows[r].start ? 2 : 0;
        for (int k = 0; k < ind; k++) seg[k] = L' ';
        memcpy(seg + ind, s + rows[r].start, rows[r].len * sizeof(wchar_t));
        seg[ind + rows[r].len] = 0;
        RECT rr = { tl, tt + (r - first) * rowH, tr, tt + (r - first + 1) * rowH };
        COLORREF c = (s[0] == L'>' && s[1] == L' ') ? g_accent : textRGB();
        drawText(g_fMono, seg, rr, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        // the links on this row: underlined, and remembered for clicks
        int rs = rows[r].start, re = rs + rows[r].len, us, ue;
        for (int from = 0; appsLogLink(s, from, &us, &ue); from = ue) {
            int a = max(us, rs), b = min(ue, re);
            if (a >= b) { if (us >= re) break; continue; }
            int lx = tl + textWidthN(g_fMono, seg, ind + a - rs), rx = tl + textWidthN(g_fMono, seg, ind + b - rs);
            int uy = rr.top + (rowH + dp(13)) / 2;                     // just under the text
            fillRect(lx, uy, rx, uy + max(1, dp(1)), GetRValue(c), GetGValue(c), GetBValue(c), 0.85);
            if (g_logLinkN < LOG_LINKS) {
                LogLink* k = &g_logLinks[g_logLinkN++];
                k->rc = (RECT){ lx, rr.top, rx, rr.bottom };
                int n = min(ue - us, APPS_LOG_LINE - 1);
                wmemcpy(k->url, s + us, n);
                k->url[n] = 0;
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    // scrollbar, shown only when there's more than fits
    if (nRows > visible) {
        RECT bar = { box.right - dp(12), box.top + dp(4), box.right - dp(2), box.bottom - dp(4) };
        g_rcLogBar = bar;
        int trackH = bar.bottom - bar.top;
        int thumb = trackH * visible / nRows;
        if (thumb < dp(24)) thumb = dp(24);
        g_logThumbH = thumb;
        int ty = bar.top + (int)((long long)(trackH - thumb) * g_logTop / (maxTop ? maxTop : 1));
        Col s = sepCol();
        fillRoundRect(bar.right - dp(6), ty, bar.right - dp(2), ty + thumb, dp(2), s.r, s.g, s.b, s.a * (g_logDrag ? 5 : 3));
    }
}
