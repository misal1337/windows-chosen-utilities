// ---------- updating the app itself from GitHub ----------
// At start (and every 6 hours while open) the app asks GitHub for the repo's latest release. Only when that release is
// newer than this exe does anything show: the rail's update button turns on and Windows shows one notification.
//
// The update replaces this exe in place, so its name, folder, shortcuts and pins stay as they are:
//   1. the release's exe is downloaded to %TEMP%\WCU-Update (through its public link, so GitHub counts the download)
//   2. it must have the size and SHA-256 GitHub lists for that file, and carry the release's version in its properties;
//      once releases are signed, it must also be signed by the same publisher as this exe
//   3. it's copied next to this exe as "<name>.new" and checked again, this exe is renamed to "<name>.old" (Windows
//      allows that while it runs), and the new one takes its name. Any failure puts the old one back.
//   4. the new exe starts, this one closes, and the new one deletes "<name>.old".
// In a folder that needs administrator rights, step 3 runs in an elevated copy, which repeats the hash check itself.

#define SU_REPO   L"misal1337/windows-chosen-utilities"
#define SU_API    L"https://api.github.com/repos/" SU_REPO L"/releases/latest"
#define SU_DL     L"https://github.com/" SU_REPO L"/releases/download/"
#define SU_ASSET  "Windows.Chosen.Utilities.exe"
#define SU_RECHECK_MS (6 * 60 * 60 * 1000)
#define TIMER_SUCHECK 9

enum { SU_UNKNOWN, SU_CURRENT, SU_AVAILABLE, SU_WORKING };
static int g_suState = SU_UNKNOWN;
static volatile LONG g_suChecking = 0;
static volatile LONG g_suPct = -1;           // download progress while SU_WORKING, -1 before the first bytes
static int g_suMajor, g_suMinor;
static wchar_t g_suTag[32];                  // "v1.13"
static wchar_t g_suUrl[512];
static wchar_t g_suSha[65];
static unsigned long long g_suSize;
static wchar_t* g_suNotes;                   // what's new, as plain text
static wchar_t g_suNotified[32];             // the release the notification was shown for
static BOOL g_suTrayOn = FALSE;
static BOOL g_suNeedsAdmin = FALSE;          // this exe's folder can't be written without administrator rights
static RECT g_rcSelfUpd;

// Whether this process can create a file next to its exe (Program Files can't be, without administrator rights)
static BOOL suFolderWritable(void) {
    wchar_t self[MAX_PATH], probe[MAX_PATH + 32];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return FALSE;
    wchar_t* slash = wcsrchr(self, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    swprintf(probe, MAX_PATH + 32, L"%s\\~wcu-write-%lu.tmp", self, GetCurrentProcessId());
    HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_ACCESS_DENIED;
    CloseHandle(f);
    return TRUE;
}

typedef struct { int major, minor; wchar_t tag[32], url[512], sha[65]; unsigned long long size; wchar_t* notes; } SuInfo;

static BOOL suNewer(int major, int minor) {
    return major > WCU_VER_MAJOR || (major == WCU_VER_MAJOR && minor > WCU_VER_MINOR);
}

// The release notes as plain text: the part above "## Download", without the "What's new" heading (the dialog's
// title says it), with headings on their own line and bullets as dots.
static wchar_t* suNotesText(const char* body) {
    wchar_t* src = (wchar_t*)malloc(32768 * sizeof(wchar_t));
    wchar_t* out = (wchar_t*)malloc(32768 * sizeof(wchar_t));
    if (!src || !out) { free(src); free(out); return NULL; }
    jText(body, src, 32768);
    int n = 0;
    BOOL lastBlank = TRUE;
    for (wchar_t* line = src; line && *line; ) {
        wchar_t* nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        size_t len = wcslen(line);
        if (len && line[len - 1] == L'\r') line[--len] = 0;
        const wchar_t* p = line;
        while (*p == L' ') p++;
        if (!wcsncmp(p, L"## Download", 11) || !wcsncmp(p, L"**Full Changelog**", 18)) break;
        BOOL heading = FALSE;
        if (!wcsncmp(p, L"## ", 3)) { line = nl ? nl + 1 : NULL; continue; }     // "What's new in v1.13"
        if (!wcsncmp(p, L"### ", 4)) { p += 4; heading = TRUE; }
        if (!*p) {
            if (!lastBlank && n < 32000) { out[n++] = L'\n'; lastBlank = TRUE; }
            line = nl ? nl + 1 : NULL;
            continue;
        }
        if (heading && !lastBlank && n < 32000) out[n++] = L'\n';           // a gap above each section
        if ((p[0] == L'-' || p[0] == L'*') && p[1] == L' ') {
            if (n < 32000) { out[n++] = L'\x2022'; out[n++] = L' '; }
            p += 2;
        }
        for (; *p && n < 32000; p++) {
            if (*p == L'*' || *p == L'`') continue;                           // bold and code marks
            out[n++] = *p;
        }
        if (n < 32000) out[n++] = L'\n';
        lastBlank = FALSE;
        line = nl ? nl + 1 : NULL;
    }
    while (n && (out[n - 1] == L'\n' || out[n - 1] == L' ')) n--;
    out[n] = 0;
    free(src);
    return out;
}

// Looks up the latest release. Posts WM_APP_SUFOUND with a malloc'd SuInfo, or NULL when there's no usable answer.
static DWORD WINAPI suCheckProc(LPVOID arg) {
    (void)arg;
    SuInfo* info = NULL;
    char* body = NULL; DWORD status = 0;
    if (!dlHttp(SU_API, &body, NULL, &status) && status == 200 && body) {
        info = (SuInfo*)calloc(1, sizeof(SuInfo));
        const char* assets = jGet(body, "assets");
        jText(jGet(body, "tag_name"), info->tag, 32);
        if (swscanf(info->tag[0] == L'v' ? info->tag + 1 : info->tag, L"%d.%d", &info->major, &info->minor) != 2) info->major = -1;
        JIter it; const char* k; const char* a;
        if (assets && jOpen(assets, &it))
            while (jNext(&it, &k, &a)) {
                wchar_t name[128], digest[80], size[32];
                jText(jGet(a, "name"), name, 128);
                if (wcscmp(name, L"" SU_ASSET)) continue;
                jText(jGet(a, "browser_download_url"), info->url, 512);
                jText(jGet(a, "digest"), digest, 80);
                jText(jGet(a, "size"), size, 32);
                info->size = _wcstoui64(size, NULL, 10);
                if (!wcsncmp(digest, L"sha256:", 7)) { lstrcpynW(info->sha, digest + 7, 65); _wcslwr(info->sha); }
            }
        info->notes = suNotesText(jGet(body, "body"));
        // only this repo's own release file, with a hash to check it against
        wchar_t want[160];
        swprintf(want, 160, L"%s%s/", SU_DL, info->tag);
        if (info->major < 0 || wcsncmp(info->url, want, wcslen(want)) || !dlHex64(info->sha) ||
            info->size < 100 * 1024 || info->size > (64u << 20)) {
            free(info->notes); free(info); info = NULL;
        }
    }
    free(body);
    if (!PostMessageW(g_hwnd, WM_APP_SUFOUND, 0, (LPARAM)info) && info) { free(info->notes); free(info); }
    InterlockedExchange(&g_suChecking, 0);
    return 0;
}

static void suCheck(void) {
    if (g_suState == SU_WORKING || InterlockedCompareExchange(&g_suChecking, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, suCheckProc, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_suChecking, 0);
}

// ---------- the notification ----------
// A notification area balloon, which Windows 11 shows as a notification. The icon is there only while it shows.
static void suTrayRemove(void) {
    if (!g_suTrayOn) return;
    NOTIFYICONDATAW n; memset(&n, 0, sizeof(n));
    n.cbSize = sizeof(n); n.hWnd = g_hwnd; n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
    g_suTrayOn = FALSE;
}
static void suNotify(void) {
    if (!wcscmp(g_suNotified, g_suTag)) return;              // once per release while the app is open
    lstrcpynW(g_suNotified, g_suTag, 32);
    suTrayRemove();
    NOTIFYICONDATAW n; memset(&n, 0, sizeof(n));
    n.cbSize = sizeof(n); n.hWnd = g_hwnd; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_APP_SUTRAY;
    n.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, g_dpi),
                                GetSystemMetricsForDpi(SM_CYSMICON, g_dpi), 0);
    lstrcpynW(n.szTip, L"Windows Chosen Utilities", ARRAYSIZE(n.szTip));
    wchar_t t[128], b[256];
    swprintf(t, 128, L"Windows Chosen Utilities %s is available", g_suTag);
    swprintf(b, 256, L"You have " WCU_VERSION L". Click to see what's new and update.");
    lstrcpynW(n.szInfoTitle, tr(t), ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, tr(b), ARRAYSIZE(n.szInfo));
    n.dwInfoFlags = NIIF_INFO;                     // the header shows the app's icon; with its own large icon Windows showed nothing
    if (Shell_NotifyIconW(NIM_ADD, &n)) {          // the icon first, then the balloon
        g_suTrayOn = TRUE;
        n.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &n);
        n.uFlags = NIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }
    if (n.hIcon) DestroyIcon(n.hIcon);
}

static void suOnFound(SuInfo* info) {
    if (g_suState == SU_WORKING) { if (info) { free(info->notes); free(info); } return; }
    if (!info) { InvalidateRect(g_hwnd, NULL, FALSE); return; }   // no answer: whatever was known stays
    if (!suNewer(info->major, info->minor)) {
        g_suState = SU_CURRENT;
        free(info->notes); free(info);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    g_suMajor = info->major; g_suMinor = info->minor;
    lstrcpynW(g_suTag, info->tag, 32);
    lstrcpynW(g_suUrl, info->url, 512);
    lstrcpynW(g_suSha, info->sha, 65);
    g_suSize = info->size;
    free(g_suNotes); g_suNotes = info->notes;
    free(info);
    g_suState = SU_AVAILABLE;
    g_suNeedsAdmin = !g_elevated && !suFolderWritable();
    InvalidateRect(g_hwnd, NULL, FALSE);
    suNotify();
}

// ---------- checking a file ----------
static BOOL suFileVersion(const wchar_t* path, int* major, int* minor) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    void* buf = n ? malloc(n) : NULL;
    VS_FIXEDFILEINFO* fi = NULL; UINT len = 0;
    BOOL ok = buf && GetFileVersionInfoW(path, 0, n, buf) && VerQueryValueW(buf, L"\\", (void**)&fi, &len) && fi && len >= sizeof(*fi);
    if (ok) { *major = HIWORD(fi->dwFileVersionMS); *minor = LOWORD(fi->dwFileVersionMS); }
    free(buf);
    return ok;
}
// Who signed the file, if Windows trusts the signature; otherwise an empty name.
static void suSigner(const wchar_t* path, wchar_t* who, int cch) {
    who[0] = 0;
    WINTRUST_FILE_INFO fi = { sizeof(fi) };
    fi.pcwszFilePath = path;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = { sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    if ((HRESULT)WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd) == S_OK) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
        if (sg && sg->csCertChain && sg->pasCertChain && sg->pasCertChain[0].pCert)
            CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, who, cch);
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
}

// ---------- putting the new exe in place ----------
// Runs in the window's process or in the elevated copy. Returns 0 or a Windows error; on failure this exe is unchanged.
static DWORD suSwap(const wchar_t* downloaded, const wchar_t* sha) {
    wchar_t self[MAX_PATH], fresh[MAX_PATH + 8], old[MAX_PATH + 8], hex[65];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return GetLastError();
    swprintf(fresh, MAX_PATH + 8, L"%s.new", self);
    swprintf(old, MAX_PATH + 8, L"%s.old", self);
    if (!CopyFileW(downloaded, fresh, FALSE)) return GetLastError();     // a copy takes the folder's permissions
    if (!dlSha256(fresh, hex) || _wcsicmp(hex, sha)) { DeleteFileW(fresh); return ERROR_INVALID_DATA; }
    DeleteFileW(old);                                                    // left by an earlier update
    if (!MoveFileExW(self, old, MOVEFILE_REPLACE_EXISTING)) { DWORD e = GetLastError(); DeleteFileW(fresh); return e; }
    if (!MoveFileExW(fresh, self, MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        MoveFileExW(old, self, 0);
        DeleteFileW(fresh);
        return e;
    }
    return 0;
}

// The elevated copy: --self-update="<downloaded>" --sha=<hex>
static int suHelperMain(const wchar_t* cmd) {
    const wchar_t* a = wcsstr(cmd, L"--self-update=\"");
    const wchar_t* s = wcsstr(cmd, L"--sha=");
    if (!a || !s) return ERROR_INVALID_PARAMETER;
    wchar_t path[MAX_PATH], sha[65];
    cmdQuoted(a + 15, path, MAX_PATH);
    lstrcpynW(sha, s + 6, 65);
    if (!dlHex64(sha)) return ERROR_INVALID_PARAMETER;
    DWORD err = suSwap(path, sha);
    if (!err) {                                    // the unelevated app can't delete it here, so Windows does at the next start
        wchar_t self[MAX_PATH], old[MAX_PATH + 8];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        swprintf(old, MAX_PATH + 8, L"%s.old", self);
        MoveFileExW(old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return (int)err;
}

// The new exe, once it starts: the old one is closing, so this tries for a few seconds
static DWORD WINAPI suCleanupProc(LPVOID arg) {
    (void)arg;
    wchar_t self[MAX_PATH], old[MAX_PATH + 8];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return 0;
    swprintf(old, MAX_PATH + 8, L"%s.old", self);
    for (int i = 0; i < 20 && GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES; i++) {
        if (DeleteFileW(old) || (GetLastError() == ERROR_ACCESS_DENIED && i > 4)) break;
        Sleep(500);
    }
    return 0;
}

// ---------- the job ----------
typedef struct { wchar_t url[512], sha[65]; unsigned long long size; int major, minor; } SuJob;
static wchar_t g_suFile[MAX_PATH];

static BOOL suTick(unsigned long long got, void* ctx) {
    SuJob* j = (SuJob*)ctx;
    LONG pct = j->size ? (LONG)(got * 100 / j->size) : 0;
    if (pct > 100) pct = 100;
    if (InterlockedExchange(&g_suPct, pct) != pct) PostMessageW(g_hwnd, WM_APP_SUPROG, 0, 0);
    return TRUE;
}

// Download, check, replace. Posts WM_APP_SUDONE: 0, a Windows error, or ERROR_CANCELLED when UAC was declined.
static DWORD WINAPI suJobProc(LPVOID arg) {
    SuJob* j = (SuJob*)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // for the UAC prompt
    wchar_t dir[MAX_PATH];
    DWORD err = 0, status = 0;
    GetTempPathW(MAX_PATH, dir);
    wcsncat(dir, L"WCU-Update", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    swprintf(g_suFile, MAX_PATH, L"%s\\" L"" SU_ASSET, dir);
    // a busy or briefly dropped connection (a timeout, a reset) gets a few more tries before the update gives up
    for (int attempt = 1; ; attempt++) {
        status = 0;
        err = dlHttpCore(j->url, NULL, g_suFile, &status, FALSE, suTick, j);
        if (!err && status != 200) err = (status >= 500) ? ERROR_WINHTTP_TIMEOUT : ERROR_BAD_NET_RESP;
        BOOL passing = err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_CONNECTION_ERROR ||
                       err == ERROR_WINHTTP_NAME_NOT_RESOLVED || err == ERROR_WINHTTP_INVALID_SERVER_RESPONSE || err == ERROR_WINHTTP_SECURE_FAILURE;
        if (!err || !passing || attempt == 4) break;
        DeleteFileW(g_suFile);
        InterlockedExchange(&g_suPct, -1);
        PostMessageW(g_hwnd, WM_APP_SUPROG, 0, 0);
        pauseMs(attempt * 3000);                          // 3, 6 and 9 seconds
    }
    wchar_t hex[65];
    if (!err) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        int ma = 0, mi = 0;
        if (!GetFileAttributesExW(g_suFile, GetFileExInfoStandard, &fa) ||
            (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) != j->size) err = ERROR_INVALID_DATA;
        else if (!dlSha256(g_suFile, hex) || _wcsicmp(hex, j->sha)) err = ERROR_INVALID_DATA;
        else if (!suFileVersion(g_suFile, &ma, &mi) || ma != j->major || mi != j->minor) err = ERROR_INVALID_DATA;
    }
    if (!err) {                                    // a signed app only takes an update from the same publisher
        wchar_t self[MAX_PATH], mine[200], theirs[200];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        suSigner(self, mine, 200);
        if (mine[0]) {
            suSigner(g_suFile, theirs, 200);
            if (wcscmp(mine, theirs)) err = TRUST_E_SUBJECT_NOT_TRUSTED;
        }
    }
    if (!err) {
        err = suSwap(g_suFile, j->sha);
        if (err == ERROR_ACCESS_DENIED && !g_elevated) {     // the folder needs administrator rights
            wchar_t args[MAX_PATH + 128];
            swprintf(args, MAX_PATH + 128, L"--self-update=\"%s\" --sha=%s", g_suFile, j->sha);
            HANDLE proc = NULL; BOOL cancelled = FALSE;
            if (startElevatedHelper(args, &proc, &cancelled)) err = waitExitCode(proc);
            else err = cancelled ? ERROR_CANCELLED : GetLastError();
        }
    }
    DeleteFileW(g_suFile);
    RemoveDirectoryW(dir);
    CoUninitialize();
    free(j);
    PostMessageW(g_hwnd, WM_APP_SUDONE, err, 0);
    return 0;
}

static const wchar_t* jobRunningTitle(const wchar_t** body);

static void suClick(void) {
    if (g_suState != SU_AVAILABLE) return;
    const wchar_t* why = NULL;
    const wchar_t* title = jobRunningTitle(&why);
    if (!title && g_busy == 2) { title = L"Still working"; why = L"Wait for the current job to finish first."; }
    if (title) {
        wchar_t msg[512];
        swprintf(msg, 512, L"%s The update closes and restarts the app.", tr(why));
        okDialog(tr(title), msg);
        return;
    }
    wchar_t head[64], body[34000];
    swprintf(head, 64, L"What's new in %s", g_suTag);
    swprintf(body, 34000, L"%s\n\nYou have " WCU_VERSION L". The app downloads %s from GitHub, replaces this exe with it and restarts.%s",
             g_suNotes && g_suNotes[0] ? g_suNotes : L"", g_suTag,
             g_suNeedsAdmin ? L" Its folder needs administrator rights, so Windows asks for them first." : L"");
    DlgExtra x; memset(&x, 0, sizeof(x));
    x.scrollBody = TRUE;
    if (!showDialogEx(g_hwnd, tr(head), body[0] == L'\n' ? body + 2 : body, NULL, 0, L"Update now", L"Not now", 0, &x)) return;
    if (jobRunningTitle(&why) || g_busy == 2) return;       // started meanwhile
    SuJob* j = (SuJob*)calloc(1, sizeof(SuJob));
    if (!j) return;
    lstrcpynW(j->url, g_suUrl, 512); lstrcpynW(j->sha, g_suSha, 65);
    j->size = g_suSize; j->major = g_suMajor; j->minor = g_suMinor;
    g_suState = SU_WORKING;
    InterlockedExchange(&g_suPct, -1);
    HANDLE t = CreateThread(NULL, 0, suJobProc, j, 0, NULL);
    if (t) CloseHandle(t);
    else { free(j); g_suState = SU_AVAILABLE; }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void suOnDone(DWORD err) {
    if (!err) {
        wchar_t self[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, self, MAX_PATH);           // the name now belongs to the new exe
        swprintf(cmd, MAX_PATH + 32, L"\"%s\" --updated", self);
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            suTrayRemove();
            DestroyWindow(g_hwnd);
            return;
        }
        wchar_t msg[256];
        swprintf(msg, 256, L"%s is in place, but Windows couldn't start it (error %lu). Close this window and open the app again.", g_suTag, GetLastError());
        okDialog(L"Updated", msg);
        g_suState = SU_CURRENT;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    g_suState = SU_AVAILABLE;
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (err == ERROR_CANCELLED) return;                       // UAC declined: nothing changed
    wchar_t msg[400];
    if (err == ERROR_INVALID_DATA)
        swprintf(msg, 400, L"The downloaded file didn't match the one listed on GitHub, so nothing was changed. Try again later.");
    else if (err == TRUST_E_SUBJECT_NOT_TRUSTED)
        swprintf(msg, 400, L"The downloaded file isn't signed by the same publisher as this app, so nothing was changed.");
    else if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED)
        swprintf(msg, 400, L"Windows wouldn't let the app replace its own file (error %lu), so nothing was changed. Is another copy of the app open?", err);
    else
        swprintf(msg, 400, L"The update couldn't be downloaded (error %lu), so nothing was changed. Check the connection and try again.", err);
    okDialog(L"Couldn't update", msg);
}

static void suOnTray(LPARAM l) {
    UINT ev = LOWORD(l);
    if (ev == NIN_BALLOONUSERCLICK || ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONUP) {
        suTrayRemove();
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        if (!g_dlgDepth) suClick();
    } else if (ev == NIN_BALLOONTIMEOUT) {
        suTrayRemove();
    }
}

// The rail's button, above Boot to USB
static void suDrawButton(RECT b) {
    g_rcSelfUpd = b;
    BOOL en = g_suState == SU_AVAILABLE;
    wchar_t label[64];
    if (g_suState == SU_AVAILABLE) swprintf(label, 64, L"Update to %s", g_suTag);
    else if (g_suState == SU_WORKING) {
        if (g_suPct < 0 || g_suPct >= 100) wcscpy(label, L"Updating…");
        else swprintf(label, 64, L"Downloading… %ld%%", g_suPct);
    }
    else if (g_suState == SU_CURRENT) wcscpy(label, L"Up to date");
    else wcscpy(label, L"Update app");
    drawButtonFace(b, dp(6), en, anim(ANIM_SELFUPD, g_hotBtn == BTN_SELFUPD && en), en ? 1.0 : 0.4);
    drawButtonLabel(b, g_fBody, label, en ? RGB(255, 255, 255) : mutedRGB(), en && g_suNeedsAdmin, dp(16), dp(6));
}
