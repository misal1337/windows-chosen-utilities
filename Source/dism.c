// Manual Updates page: fetch the newest update for this PC's channel through UUP dump, and install an update
// file with DISM.

// ---------- fetching the newest update: state shared by the page and its worker ----------
// What this PC is: its build, and which channel's updates it should be offered. Retail PCs are offered Release
// Preview's, at the user's request, since that's where an installed version's next update shows first.
typedef struct {
    wchar_t build[32];       // installed, such as 26300.9818
    wchar_t arch[12];        // UUP dump's name: amd64 or arm64
    wchar_t ring[16];        // UUP dump's ring: RP, Beta, Dev or Canary
    wchar_t channel[40];     // shown on the page
    DWORD sku;
} UpdPc;

// The update found: kept for the download that follows once the person agrees
typedef struct {
    wchar_t title[200], found[32], kb[16], ext[8], ndp[16], uuid[48], file[160];
    wchar_t url[3000];
    wchar_t sha256[65], sha1[41];
    unsigned long long size;
} UpdFound;

enum { UPD_IDLE, UPD_LOOKING, UPD_DOWNLOADING, UPD_CHECKING };
// Two buttons share one download: the cumulative update (.msu), or the .NET Framework update (NDP) that the same
// build lists beside it
enum { UPD_MSU, UPD_NDP };
static RECT g_rcUpdGet, g_rcUpdNdp;
static volatile LONG g_updKind = UPD_MSU;   // which button's job is running, or ran last
static volatile LONG g_updState = UPD_IDLE;
static volatile LONG g_updStop = 0;
static unsigned long long g_updGot = 0;  // bytes so far, written by the worker and only read for drawing
static volatile LONG g_updSpeed = 0;      // KB per second, averaged over the last few seconds
static UpdFound g_updFound;
static wchar_t g_updSaved[MAX_PATH];     // the file on the Desktop
static wchar_t g_updStatus[512];
static wchar_t g_updMsg[512];            // the worker's result, read by the window after the message arrives

static const wchar_t* updRingName(const wchar_t* ring) {
    if (!wcscmp(ring, L"RP")) return L"Release Preview";
    return ring;
}

static void updReadPc(UpdPc* pc) {
    memset(pc, 0, sizeof(*pc));
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    wchar_t cb[16] = L""; DWORD ubr = 0, n = sizeof(cb);
    RegGetValueW(HKEY_LOCAL_MACHINE, cv, L"CurrentBuild", RRF_RT_REG_SZ, NULL, cb, &n);
    n = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, cv, L"UBR", RRF_RT_REG_DWORD, NULL, &ubr, &n);
    swprintf(pc->build, 32, L"%s.%lu", cb[0] ? cb : L"0", ubr);
#ifdef UPD_TEST_BUILD           // a scratch build can pretend to be an older PC, to try a download
    wcscpy(pc->build, UPD_TEST_BUILD);
#endif
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    wcscpy(pc->arch, si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? L"arm64" : L"amd64");
    if (!GetProductInfo(10, 0, 0, 0, &pc->sku) || !pc->sku) pc->sku = 48;     // 48 is Pro
    // A PC in the Windows Insider Program names its channel here; Settings removes it when the PC leaves
    wchar_t br[64] = L""; n = sizeof(br);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\WindowsSelfHost\\Applicability", L"BranchName", RRF_RT_REG_SZ, NULL, br, &n);
    if (StrStrIW(br, L"Canary"))       { wcscpy(pc->ring, L"Canary"); wcscpy(pc->channel, L"Canary channel"); }
    else if (StrStrIW(br, L"Dev"))     { wcscpy(pc->ring, L"Dev");    wcscpy(pc->channel, L"Dev channel"); }
    else if (StrStrIW(br, L"Beta"))    { wcscpy(pc->ring, L"Beta");   wcscpy(pc->channel, L"Beta channel"); }
    else if (StrStrIW(br, L"Release")) { wcscpy(pc->ring, L"RP");     wcscpy(pc->channel, L"Release Preview channel"); }
    else                               { wcscpy(pc->ring, L"RP");     wcscpy(pc->channel, L"Retail (no Insider channel)"); }
}

static const UpdPc* updPc(void) {
    static UpdPc pc; static BOOL read = FALSE;
    if (!read) { updReadPc(&pc); read = TRUE; }
    return &pc;
}

// ---------- Manual Updates page ----------
static void drawDism(int cL, int cR) {
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    fillRoundRect(cL, top, cR, top + dp(392), dp(8), sf.r, sf.g, sf.b, sf.a);

    drawPageHeader(0xE90F, cL, cR, top, L"Windows updates", L"Get this PC's next update, or install one you've downloaded, using DISM.");
    int bh = dp(36), by = top + dp(96);

    // fetch: the newest update in this PC's channel, saved to the Desktop. While one downloads, its button cancels
    // and the other waits.
    {
        const UpdPc* pc = updPc();
        LONG st = g_updState;
        for (int k = UPD_MSU; k <= UPD_NDP; k++) {
            int top2 = by + k * (bh + dp(10));
            RECT b = { cL + dp(20), top2, cL + dp(20) + dp(300), top2 + bh };
            if (k == UPD_MSU) g_rcUpdGet = b; else g_rcUpdNdp = b;
            BOOL mine = (st != UPD_IDLE && g_updKind == k);
            BOOL cancel = mine && st == UPD_DOWNLOADING;
            BOOL en = st == UPD_IDLE || cancel;
            const wchar_t* label = (mine && st == UPD_LOOKING) ? L"Looking for the latest update…"
                                 : cancel ? L"Cancel download"
                                 : (mine && st == UPD_CHECKING) ? L"Checking the file…"
                                 : k == UPD_MSU ? L"Download latest update (.msu)" : L"Download latest update (ndp481)";
            int btn = k == UPD_MSU ? BTN_UPDGET : BTN_UPDNDP;
            drawButtonFace(b, dp(6), !cancel, anim(ANIM_BTN + btn, g_hotBtn == btn && en), en ? 1.0 : 0.4);
            drawButtonLabel(b, g_fBodyBold, label, cancel ? textRGB() : RGB(255,255,255), FALSE, 0, 0);
        }
        int y = by + 2 * bh + dp(10) + dp(14);
        wchar_t info[200];
        swprintf(info, 200, L"This PC: build %s, %s. Gets the newest %s update.",
                 pc->build, pc->channel, updRingName(pc->ring));
        RECT e = { cL + dp(20), y, cR - dp(20), y + dp(20) };
        drawText(g_fSmall, info, e, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += dp(26);
        RECT p = { cL + dp(20), y, cR - dp(20), y + dp(20) };
        if (st == UPD_DOWNLOADING && g_updFound.size) {
            wchar_t got[32], all[32], line[300];
            unsigned long long g = g_updGot;
            fmtSize(g, got, 32); fmtSize(g_updFound.size, all, 32);
            int pct = (int)(g * 100 / g_updFound.size);
            wchar_t speed[48] = L"";
            if (g_updSpeed > 0) { wchar_t sp[32]; fmtSize((unsigned long long)g_updSpeed << 10, sp, 32); swprintf(speed, 48, L" at %s/s", sp); }
            swprintf(line, 300, L"Downloading %s — %s of %s (%d%%)%s", PathFindFileNameW(g_updSaved), got, all, pct, speed);
            drawText(g_fSmall, line, p, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            drawProgressBar(cL + dp(20), cR - dp(20), y + dp(26), pct);
        } else if (g_updStatus[0]) {
            RECT q = { cL + dp(20), y, cR - dp(20), y + dp(36) };
            drawText(g_fSmall, g_updStatus, q, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        }
        by = y + dp(48);
    }

    // install button (accent); the shield shows that it will ask for administrator permission
    RECT b = { cL + dp(20), by, cL + dp(20) + dp(300), by + bh };
    g_rcDismInstall = b;
    BOOL en = (g_busy == 0);
    drawButtonFace(b, dp(6), TRUE, anim(ANIM_BTN + BTN_DISM, g_hotBtn == BTN_DISM && en), en ? 1.0 : 0.4);
    drawButtonLabel(b, g_fBodyBold, L"Install update file (.msu or .cab)", RGB(255,255,255), !g_elevated, dp(16), dp(6));
    int y = by + bh + dp(14);
    RECT e = { cL + dp(20), y, cR - dp(20), y + dp(36) };
    drawText(g_fSmall, L"Installing can take several minutes and can't be cancelled once it starts. "
                       L"Some updates need a restart to finish.",
             e, mutedRGB(), DT_LEFT | DT_WORDBREAK);
    y += dp(44);

    if (g_dismRunning) {
        wchar_t line[MAX_PATH + 48];
        const wchar_t* name = PathFindFileNameW(g_dismPath);
        if (g_dismPct >= 0) swprintf(line, MAX_PATH + 48, L"Installing %s — %d%%", name, g_dismPct);
        else                swprintf(line, MAX_PATH + 48, L"Installing %s…", name);
        RECT p = { cL + dp(20), y, cR - dp(20), y + dp(20) };
        drawText(g_fSmall, line, p, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        drawProgressBar(cL + dp(20), cR - dp(20), y + dp(26), g_dismPct);
    } else if (g_dismStatus[0]) {
        RECT p = { cL + dp(20), y, cR - dp(20), y + dp(36) };
        drawText(g_fSmall, g_dismStatus, p, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
}

// ---------- install an update file with DISM ----------
static BOOL isUpdateFile(const wchar_t* path) {
    if (!path || !*path || wcschr(path, L'"')) return FALSE;     // a quote could add DISM arguments
    const wchar_t* ext = PathFindExtensionW(path);
    return _wcsicmp(ext, L".msu") == 0 || _wcsicmp(ext, L".cab") == 0;
}

static HWND g_dismNotify = NULL;
static void dismOnPercent(int pct) { if (g_dismNotify) PostMessageW(g_dismNotify, WM_APP_DISMPROG, (WPARAM)pct, 0); }

// Runs in whichever process has administrator rights. Returns DISM's exit code and posts progress.
static DWORD runDismInstall(const wchar_t* path, HWND notify) {
    if (!isUpdateFile(path)) return ERROR_INVALID_PARAMETER;
    wchar_t exe[MAX_PATH]; systemPath(exe, L"dism.exe");
    wchar_t cmd[MAX_PATH * 2 + 128];
    swprintf(cmd, MAX_PATH * 2 + 128, L"\"%s\" /Online /English /NoRestart /Add-Package /PackagePath:\"%s\"", exe, path);
    g_dismNotify = notify;
    return runWithPercent(exe, cmd, dismOnPercent);
}

static const wchar_t* dismErrorText(DWORD code) {
    switch (code) {
    case 0x800F081E: return L"This update doesn't apply to this PC. It may be for another Windows version, or it may already be included.";
    case 0x800F0823: return L"This update needs a newer servicing stack. Install the latest servicing stack update first, then try again.";
    case 0x800F0831: return L"An earlier update that this one depends on is missing. Install it first, then try again.";
    case 0x800F0922: return L"Windows couldn't finish installing this update. Restart the PC and try again.";
    case 0x80070BC9:
    case 3017:       return L"Windows has another update waiting for a restart. Restart the PC, then try again.";
    case 2: case 3:
    case 0x80070002:
    case 0x80070003: return L"Windows couldn't find the file. If it's on a mapped network drive, copy it to this PC and try again.";
    case 5:
    case 0x80070005: return L"Windows denied access to the file or to the servicing components.";
    case 87:
    case 0xCA00A005:
    case 0x80070057: return L"DISM couldn't use this file. It may not be a valid update package, or it may be damaged.";
    case 1726:       return L"The Windows servicing process stopped unexpectedly. Restart the PC and check whether the update installed.";
    default:         return L"DISM couldn't install this update.";
    }
}

static void restartPC(void) {
    wchar_t exe[MAX_PATH]; systemPath(exe, L"shutdown.exe");
    ShellExecuteW(NULL, L"open", exe, L"/r /t 0", NULL, SW_HIDE);   // a normal restart, not forced
}

// arg = the elevated helper's process, or NULL when this process is already elevated
static DWORD WINAPI dismProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    DWORD code = helper ? waitExitCode(helper) : runDismInstall(g_dismPath, g_hwnd);
    PostMessageW(g_hwnd, WM_APP_DISMDONE, (WPARAM)code, 0);
    return 0;
}

static void doDismInstall(void) {
    wchar_t file[MAX_PATH] = L"";
    if (!pickOpen(file, L"Windows update files (*.msu, *.cab)\0*.msu;*.cab\0", L"Choose an update to install")) return;
    if (!isUpdateFile(file)) {
        okDialog(L"Not an update file", L"Choose a .msu or .cab file.");
        return;
    }

    if (askInApp()) {
        wchar_t msg[MAX_PATH + 96];
        swprintf(msg, MAX_PATH + 96, L"%s\n\nThis can take several minutes and can't be cancelled once it starts.",
                 PathFindFileNameW(file));
        if (!showDialog(g_hwnd, L"Install this update?", msg, NULL, 0, L"Install", L"Cancel", 1)) return;
    }

    HANDLE helper = NULL;
    if (!g_elevated) {
        wchar_t args[MAX_PATH + 32]; BOOL cancelled = FALSE;
        swprintf(args, MAX_PATH + 32, L"--dism-install=\"%s\"", file);
        if (!startElevatedHelper(args, &helper, &cancelled)) {
            wcscpy(g_dismStatus, cancelled ? L"Cancelled — nothing was installed."
                                           : L"Couldn't start the administrator helper — nothing was installed.");
            InvalidateRect(g_hwnd, NULL, FALSE);
            return;
        }
    }
    if (!jobBegin()) {
        if (helper) CloseHandle(helper);
        return;
    }
    wcscpy(g_dismPath, file);
    g_dismRunning = TRUE;
    g_dismPct = -1;
    g_dismStatus[0] = 0;
    startWorker(dismProc, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void dismOnDone(DWORD code) {
    const wchar_t* name = PathFindFileNameW(g_dismPath);
    InterlockedExchange(&g_busy, 0);
    g_dismRunning = FALSE;
    if (code == 0) {
        swprintf(g_dismStatus, MAX_PATH + 64, L"Installed %s.", name);
    } else if (code == ERROR_SUCCESS_REBOOT_REQUIRED) {
        swprintf(g_dismStatus, MAX_PATH + 64, L"Installed %s. Restart to finish.", name);
        InvalidateRect(g_hwnd, NULL, FALSE);
        if (showDialog(g_hwnd, L"Restart to finish installing?",
                       L"The update is installed, and Windows needs to restart to finish. Save your work first.",
                       NULL, 0, L"Restart now", L"Later", 0)) restartPC();
    } else {
        wchar_t msg[512];
        swprintf(msg, 512, L"%s\n\nError 0x%08lX. Details are in C:\\Windows\\Logs\\DISM\\dism.log.",
                 dismErrorText(code), code);
        swprintf(g_dismStatus, MAX_PATH + 64, L"Couldn't install %s (error 0x%08lX).", name, code);
        InvalidateRect(g_hwnd, NULL, FALSE);
        okDialog(L"Update not installed", msg);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- fetching the newest update for this PC's channel ----------
// "26300.9818" against "26300.10008": by build, then by revision
static int updCmpBuild(const wchar_t* a, const wchar_t* b) {
    wchar_t* e;
    unsigned long a1 = wcstoul(a, &e, 10), a2 = (*e == L'.') ? wcstoul(e + 1, NULL, 10) : 0;
    unsigned long b1 = wcstoul(b, &e, 10), b2 = (*e == L'.') ? wcstoul(e + 1, NULL, 10) : 0;
    if (a1 != b1) return a1 < b1 ? -1 : 1;
    return a2 < b2 ? -1 : (a2 > b2);
}

// what goes into the file's name and the next request: nothing but these characters
static BOOL updOnly(const wchar_t* s, const wchar_t* extra) {
    if (!*s) return FALSE;
    for (; *s; s++) if (!iswxdigit(*s) && !wcschr(extra, *s)) return FALSE;
    return TRUE;
}

// UUP dump lists a build's .NET, out-of-box and setup updates beside it; only Windows' own update is wanted
static BOOL updTitleWanted(const wchar_t* t) {
    static const wchar_t* SKIP[] = { L".NET", L"OOBE", L"Dynamic", L"Safe OS", L"Setup", L"Servicing Stack", L"Language", L"Feature on Demand" };
    for (size_t i = 0; i < sizeof(SKIP) / sizeof(SKIP[0]); i++) if (StrStrIW(t, SKIP[i])) return FALSE;
    return TRUE;
}

enum { UPD_FOUND, UPD_UPTODATE, UPD_FAILED };

// Asks UUP dump what this PC's channel offers its build, then for that update's files. For the cumulative update
// it keeps the .msu with the highest KB number; others beside it, such as a checkpoint update, are older. For
// .NET it keeps the NDP file with the highest KB number, such as Windows11.0-KB5127694-x64-NDP481.cab.
static DWORD WINAPI updLookProc(LPVOID arg) {
    LONG kind = (LONG)(INT_PTR)arg;
    const UpdPc* pc = updPc();
    static UpdFound f; memset(&f, 0, sizeof(f));
    wchar_t q[300], err[400] = L"", uuid[48] = L"";
    char* b = NULL;
    LONG result = UPD_FOUND;
    swprintf(q, 300, L"fetchupd.php?arch=%s&ring=%s&flight=Active&build=%s&sku=%lu&type=Production",
             pc->arch, pc->ring, pc->build, pc->sku);
    const char* r = dlApi(q, &b, err, 400);
    if (!r) result = UPD_FAILED;
    else {
        JIter it; const char* k; const char* v;
        if (jOpen(jGet(r, "updateArray"), &it)) {
            while (jNext(&it, &k, &v)) {
                wchar_t t[200], fb[32], id[48];
                jText(jGet(v, "updateTitle"), t, 200);
                jText(jGet(v, "foundBuild"), fb, 32);
                jText(jGet(v, "updateId"), id, 48);
                if (!updOnly(fb, L".") || !updOnly(id, L"-") || !updTitleWanted(t)) continue;
                if (!f.found[0] || updCmpBuild(fb, f.found) > 0) { wcscpy(f.found, fb); wcscpy(f.title, t); wcscpy(uuid, id); }
            }
        }
        free(b); b = NULL;
        if (!uuid[0]) {
            result = UPD_UPTODATE;
            swprintf(err, 400, L"UUP dump lists no %s update for build %s right now.", updRingName(pc->ring), pc->build);
        } else if (updCmpBuild(f.found, pc->build) < 0) {
            // the same build is still offered: the window asks whether to save it anyway
            result = UPD_UPTODATE;
            swprintf(err, 400, L"This PC is up to date: build %s is the newest %s update UUP dump lists.", f.found, updRingName(pc->ring));
        }
    }
    if (result == UPD_FOUND) {
        pauseMs(5000);                    // UUP dump refuses requests that come too close together
        swprintf(q, 300, L"get.php?id=%s&lang=en-us&edition=professional", uuid);
        r = dlApi(q, &b, err, 400);
        if (!r) result = UPD_FAILED;
        else {
            unsigned long best = 0;
            JIter it; const char* k; const char* v;
            if (jOpen(jGet(r, "files"), &it) && it.obj) {
                while (jNext(&it, &k, &v)) {
                    wchar_t name[160], url[3000], s256[80], s1[80], sz[32];
                    jText(k, name, 160);
                    size_t n = wcslen(name);
                    if (n < 5) continue;
                    const wchar_t* ext = name + n - 4;
                    const wchar_t* ndp = StrStrIW(name, L"NDP");
                    if (kind == UPD_MSU ? (_wcsicmp(ext, L".msu") || ndp)
                                        : (!ndp || (_wcsicmp(ext, L".cab") && _wcsicmp(ext, L".msu")))) continue;
                    const wchar_t* kb = StrStrIW(name, L"KB");
                    unsigned long num = kb ? wcstoul(kb + 2, NULL, 10) : 0;
                    if (!num || num <= best) continue;
                    jText(jGet(v, "url"), url, 3000);
                    // Microsoft's links are often plain http; the checksum is what proves the file
                    if ((wcsncmp(url, L"https://", 8) && wcsncmp(url, L"http://", 7)) || wcspbrk(url, L"\r\n ")) continue;
                    jText(jGet(v, "sha256"), s256, 80);
                    jText(jGet(v, "sha1"), s1, 80);
                    if (!dlHex64(s256) && !(wcslen(s1) == 40 && updOnly(s1, L""))) continue;
                    jText(jGet(v, "size"), sz, 32);
                    best = num;
                    swprintf(f.kb, 16, L"KB%lu", num);
                    wcscpy(f.ext, _wcsicmp(ext, L".cab") ? L".msu" : L".cab");
                    f.ndp[0] = 0;
                    if (ndp) {          // NDP481: .NET Framework 4.8.1, shown in the dialog
                        int i = 0;
                        for (const wchar_t* c = ndp; i < 15 && (iswalnum(*c)); c++) f.ndp[i++] = *c;
                        f.ndp[i] = 0;
                    }
                    wcscpy(f.url, url);
                    wcscpy(f.file, name);
                    wcscpy(f.uuid, uuid);
                    wcscpy(f.sha256, dlHex64(s256) ? s256 : L"");
                    wcscpy(f.sha1, dlHex64(s256) ? L"" : s1);
                    f.size = _wcstoui64(sz, NULL, 10);
                }
            }
            free(b);
            if ((!best || !f.size) && kind == UPD_NDP) {
                result = UPD_FAILED;
                swprintf(err, 400, L"Build %s, the newest in the %s, lists no .NET Framework update (NDP) among its files.",
                         f.found, updRingName(pc->ring));
            } else if (!best || !f.size) {
                result = UPD_FAILED;
                swprintf(err, 400, L"UUP dump lists build %s for the %s, but not as a .msu update file: it's a full new build. "
                         L"Make an ISO of it on the Download Windows page instead.", f.found, updRingName(pc->ring));
            }
        }
    }
    g_updFound = f;
    lstrcpynW(g_updMsg, err, 512);
    PostMessageW(g_hwnd, WM_APP_UPDFOUND, (WPARAM)result, 0);
    return 0;
}

static BOOL updOnProgress(unsigned long long got, void* ctx) {
    (void)ctx;
    static ULONGLONG last = 0;
    g_updGot = got;
    ULONGLONG now = GetTickCount64();
    // the speed: what arrived over about the last 3 seconds
    static struct { ULONGLONG t, got; } hist[16]; static int nHist = 0;
    if (nHist && got < hist[nHist - 1].got) nHist = 0;                 // another download
    if (!nHist || now - hist[nHist - 1].t >= 250) {
        if (nHist == 16) { memmove(hist, hist + 1, sizeof(hist[0]) * 15); nHist--; }
        hist[nHist].t = now; hist[nHist].got = got; nHist++;
        int i = 0;
        while (i < nHist - 2 && now - hist[i].t > 3000) i++;
        ULONGLONG ms = now - hist[i].t;
        if (ms >= 500) InterlockedExchange(&g_updSpeed, (LONG)((got - hist[i].got) * 1000 / ms / 1024));
    }
    if (now - last > 250) { last = now; InvalidateRect(g_hwnd, NULL, FALSE); }
    return !g_updStop;
}

// A fresh link to the same file, when the one in use is no longer accepted
static BOOL updFreshLink(wchar_t* url, void* ctx) {
    const UpdFound* f = (const UpdFound*)ctx;
    wchar_t q[300], err[400]; char* b;
    swprintf(q, 300, L"get.php?id=%s&lang=en-us&edition=professional", f->uuid);
    const char* r = dlApi(q, &b, err, 400);
    JIter it; const char* k; const char* v; BOOL ok = FALSE;
    if (r && jOpen(jGet(r, "files"), &it) && it.obj)
        while (!ok && jNext(&it, &k, &v)) {
            wchar_t name[160];
            jText(k, name, 160);
            if (wcscmp(name, f->file)) continue;
            jText(jGet(v, "url"), url, 4096);
            ok = (!wcsncmp(url, L"https://", 8) || !wcsncmp(url, L"http://", 7)) && !wcspbrk(url, L"\r\n ");
        }
    free(b);
    return ok;
}

static BOOL updFileMatches(const wchar_t* path, const UpdFound* f) {
    wchar_t hex[65];
    if (f->sha256[0]) return dlHashFile(path, CALG_SHA_256, hex) && !_wcsicmp(hex, f->sha256);
    return dlHashFile(path, CALG_SHA1, hex) && !_wcsicmp(hex, f->sha1);
}

static const wchar_t* updArchLabel(const wchar_t* arch) { return wcscmp(arch, L"amd64") ? arch : L"x64"; }

// The .NET update's own version, from the package manifest inside its cab:
// <assemblyIdentity name="Package_for_DotNetRollup_481" version="10.0.10008.1" ...
static BOOL updNdpVersion(const wchar_t* cab, wchar_t* ver, int cch) {
    ver[0] = 0;
    wchar_t tmp[MAX_PATH], dir[MAX_PATH], args[MAX_PATH * 3], mum[MAX_PATH + 16];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%swcu-ndp-%lu-%llu", tmp, GetCurrentProcessId(), GetTickCount64());
    if (!CreateDirectoryW(dir, NULL)) return FALSE;
    swprintf(args, MAX_PATH * 3, L"\"%s\" -F:update.mum \"%s\"", cab, dir);
    dlExpand(args);
    swprintf(mum, MAX_PATH + 16, L"%s\\update.mum", dir);
    char* x = dlReadSmallFile(mum, 1 << 20);
    DeleteFileW(mum);
    RemoveDirectoryW(dir);
    if (!x) return FALSE;
    const char* a = strstr(x, "<assemblyIdentity ");
    const char* e = a ? strchr(a, '>') : NULL;
    const char* v = a ? strstr(a, " version=\"") : NULL;
    if (v && e && v < e) {
        v += 10;
        int i = 0;
        while (i < cch - 1 && ((*v >= '0' && *v <= '9') || *v == '.')) ver[i++] = (wchar_t)*v++;
        ver[i] = 0;
        if (*v != '"' || !i) ver[0] = 0;
    }
    free(x);
    return ver[0] != 0;
}

// An earlier download of the same file that still checks out: the name passed for the .msu, or for .NET any
// <base>_<version>.cab, since its version is only known from the file
static BOOL updFindExisting(const wchar_t* saved, const UpdFound* f, wchar_t* found) {
    wchar_t pat[MAX_PATH + 8], dir[MAX_PATH], path[MAX_PATH + 300];
    if (g_updKind != UPD_NDP) { wcscpy(pat, saved); }
    else {
        wcscpy(pat, saved);
        PathRemoveExtensionW(pat);
        wcscat(pat, L"_*");
        wcscat(pat, f->ext);
    }
    wcscpy(dir, saved);
    PathRemoveFileSpecW(dir);
    WIN32_FIND_DATAW fd;
    HANDLE ff = FindFirstFileW(pat, &fd);
    if (ff == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = FALSE;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow) != f->size) continue;
        swprintf(path, MAX_PATH + 300, L"%s\\%s", dir, fd.cFileName);
        if (wcslen(path) >= MAX_PATH) continue;
        InterlockedExchange(&g_updState, UPD_CHECKING);
        InvalidateRect(g_hwnd, NULL, FALSE);
        if (updFileMatches(path, f)) { wcscpy(found, path); ok = TRUE; }
    } while (!ok && FindNextFileW(ff, &fd));
    FindClose(ff);
    InterlockedExchange(&g_updState, UPD_DOWNLOADING);
    return ok;
}

// Downloads beside the final name as .part, checks it against UUP dump's checksum, then gives it its name
static DWORD WINAPI updGetProc(LPVOID arg) {
    (void)arg;
    const UpdFound* f = &g_updFound;
    const wchar_t* name = PathFindFileNameW(g_updSaved);
    DWORD code = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    wchar_t had[MAX_PATH];
    if (updFindExisting(g_updSaved, f, had)) {
        wcscpy(g_updSaved, had);
        swprintf(g_updMsg, 512, L"%s was already on the Desktop, and it checks out.", PathFindFileNameW(g_updSaved));
        PostMessageW(g_hwnd, WM_APP_UPDDONE, 0, 0);
        return 0;
    }
    wchar_t part[MAX_PATH + 8];
    swprintf(part, MAX_PATH + 8, L"%s.part", g_updSaved);
    DWORD status = 0;
    DWORD e = dlHttpParallel(f->url, part, f->size, &status, TRUE, updOnProgress, updFreshLink, (void*)f);
    if (!e) status = 200;
    else if (e == ERROR_NOT_SUPPORTED) e = dlHttpCore(f->url, NULL, part, &status, TRUE, updOnProgress, NULL);   // one connection then
    if (e == ERROR_CANCELLED) {
        code = e;
        wcscpy(g_updMsg, L"Cancelled. The part downloaded was deleted.");
    } else if (e || status != 200) {
        code = e ? e : ERROR_BAD_NET_RESP;
        swprintf(g_updMsg, 512, L"Couldn't download %s (error %lu, HTTP %lu). Check the connection, or try again later.", name, e, status);
    } else {
        InterlockedExchange(&g_updState, UPD_CHECKING);
        InvalidateRect(g_hwnd, NULL, FALSE);
        if (!GetFileAttributesExW(part, GetFileExInfoStandard, &fa) || fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) != f->size || !updFileMatches(part, f)) {
            code = ERROR_INVALID_DATA;
            swprintf(g_updMsg, 512, L"The downloaded file didn't match the checksum UUP dump lists, so it was deleted. Try again.");
        } else {
            // .NET's name ends with its own version, read from the checked file: KB5127694_x64_NDP481_10.0.10008.1.cab
            wchar_t final[MAX_PATH + 40], ver[40];
            wcscpy(final, g_updSaved);
            if (g_updKind == UPD_NDP && updNdpVersion(part, ver, 40)) {
                PathRemoveExtensionW(final);
                wcscat(final, L"_");
                wcscat(final, ver);
                wcscat(final, f->ext);
            }
            if (wcslen(final) >= MAX_PATH || !MoveFileExW(part, final, MOVEFILE_REPLACE_EXISTING)) {
                code = wcslen(final) >= MAX_PATH ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
                swprintf(g_updMsg, 512, L"Couldn't name the file %s on the Desktop (error %lu).", PathFindFileNameW(final), code);
            } else {
                wcscpy(g_updSaved, final);
                swprintf(g_updMsg, 512, L"Saved %s to the Desktop.", PathFindFileNameW(g_updSaved));
            }
        }
    }
    if (code) DeleteFileW(part);
    PostMessageW(g_hwnd, WM_APP_UPDDONE, (WPARAM)code, 0);
    return 0;
}

static BOOL updStart(LPTHREAD_START_ROUTINE proc, LONG state) {
    InterlockedExchange(&g_updState, state);
    HANDLE t = CreateThread(NULL, 0, proc, (LPVOID)(INT_PTR)g_updKind, 0, NULL);
    if (!t) { InterlockedExchange(&g_updState, UPD_IDLE); return FALSE; }
    CloseHandle(t);
    return TRUE;
}

static void updClicked(LONG kind) {
    if (g_updState == UPD_DOWNLOADING) {
        if (kind != g_updKind) return;        // only its own button cancels
        InterlockedExchange(&g_updStop, 1);
        return;
    }
    if (g_updState != UPD_IDLE) return;
    InterlockedExchange(&g_updKind, kind);
    InterlockedExchange(&g_updStop, 0);
    // Parts left by a download that never finished, such as after a power cut. One still being written by
    // another copy of the app is open without delete sharing, so it stays.
    wchar_t desktop[MAX_PATH], pat[MAX_PATH + 32], part[MAX_PATH + 300];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop))) {
        static const wchar_t* PARTS[] = { L"KB*_*.msu.part", L"KB*_*.cab.part" };
        for (int i = 0; i < 2; i++) {
            swprintf(pat, MAX_PATH + 32, L"%s\\%s", desktop, PARTS[i]);
            WIN32_FIND_DATAW fd;
            HANDLE ff = FindFirstFileW(pat, &fd);
            if (ff == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                swprintf(part, MAX_PATH + 300, L"%s\\%s", desktop, fd.cFileName);
                DeleteFileW(part);
            } while (FindNextFileW(ff, &fd));
            FindClose(ff);
        }
    }
    swprintf(g_updStatus, 512, L"Asking UUP dump for the newest %s update%s. This can take a minute when it's busy.",
             updRingName(updPc()->ring), kind == UPD_NDP ? L" and its .NET Framework update" : L"");
    if (!updStart(updLookProc, UPD_LOOKING)) wcscpy(g_updStatus, L"Couldn't start looking for the update.");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void updOnFound(LONG result) {
    InterlockedExchange(&g_updState, UPD_IDLE);
    wcscpy(g_updStatus, g_updMsg);
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (result == UPD_FAILED) { okDialog(L"Couldn't get the update", g_updMsg); return; }
    if (result != UPD_FOUND) return;

    const UpdFound* f = &g_updFound;
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop))) {
        wcscpy(g_updStatus, L"Couldn't find the Desktop folder.");
        return;
    }
    // named as the user asked: the KB, then the full build it comes with, such as KB5127936_26300.9818.msu
    // .NET's, also as asked: KB, architecture, NDP version, then its own version, which is added once it's downloaded
    if (g_updKind == UPD_NDP)
        swprintf(g_updSaved, MAX_PATH, L"%s\\%s_%s_%s%s", desktop, f->kb, updArchLabel(updPc()->arch), f->ndp[0] ? f->ndp : L"NDP", f->ext);
    else
        swprintf(g_updSaved, MAX_PATH, L"%s\\%s_%s%s", desktop, f->kb, f->found, f->ext);
    wchar_t size[32], msg[1200], base[MAX_PATH];
    fmtSize(f->size, size, 32);
    // This PC already runs the build the update is for: it may well be installed, so saving it is a choice
    BOOL same = updCmpBuild(f->found, updPc()->build) == 0;
    wchar_t intro[200] = L"";
    if (same) swprintf(intro, 200, L"This PC is already on build %s, the same build this update is for, so it may already be installed.\n\n", f->found);
    if (g_updKind == UPD_NDP) {
        wcscpy(base, PathFindFileNameW(g_updSaved));
        PathRemoveExtensionW(base);
        swprintf(msg, 1200, L"%s%s\n\n%s is the .NET Framework update (%s) that comes with build %s. It's %s, saved to the Desktop as "
                 L"%s_<version>%s, with its version read from the file.",
                 intro, f->title, f->kb, f->ndp[0] ? f->ndp : L"NDP", f->found, size, base, f->ext);
    } else if (same)
        swprintf(msg, 1200, L"%s%s\n\n%s is the cumulative update for build %s. It's %s, saved to the Desktop as %s.",
                 intro, f->title, f->kb, f->found, size, PathFindFileNameW(g_updSaved));
    else
        swprintf(msg, 1200, L"%s\n\n%s brings this PC from build %s to %s. It's %s, saved to the Desktop as %s.",
                 f->title, f->kb, updPc()->build, f->found, size, PathFindFileNameW(g_updSaved));
    if (!showDialog(g_hwnd, same ? L"Save it anyway?" : L"Download this update?", msg, NULL, 0,
                    same ? L"Save" : L"Download", same ? L"Abort" : L"Cancel", same ? 1 : 0)) {
        wcscpy(g_updStatus, same ? L"Aborted. Nothing was saved." : L"Not downloaded.");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    ULARGE_INTEGER freeB;
    if (GetDiskFreeSpaceExW(desktop, &freeB, NULL, NULL) && freeB.QuadPart < f->size + (256ull << 20)) {
        wchar_t need[32]; fmtSize(f->size + (256ull << 20), need, 32);
        swprintf(msg, 1200, L"The Desktop's drive needs %s free for this update. Free some space, then try again.", need);
        wcscpy(g_updStatus, L"Not downloaded: not enough free space.");
        okDialog(L"Not enough space", msg);
        return;
    }
    g_updGot = 0;
    g_updSpeed = 0;
    g_updStatus[0] = 0;
    if (!updStart(updGetProc, UPD_DOWNLOADING)) wcscpy(g_updStatus, L"Couldn't start the download.");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void updOnDone(DWORD code) {
    InterlockedExchange(&g_updState, UPD_IDLE);
    wcscpy(g_updStatus, g_updMsg);
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (code == 0) {
        wchar_t msg[1200];
        swprintf(msg, 1200, L"%s is on your Desktop. Install it with \"Install update file\" below when you're ready.", PathFindFileNameW(g_updSaved));
        okDialog(L"Update saved", msg);
    } else if (code != ERROR_CANCELLED) {
        okDialog(L"Update not downloaded", g_updMsg);
    }
}
