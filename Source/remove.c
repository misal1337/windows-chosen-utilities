// Remove Apps page: the Microsoft apps that come with Windows, in a scrollable list with tick boxes. Uninstall removes
// the ticked ones that are installed from the signed-in user's account (Remove-AppxPackage, no administrator rights
// needed); Install brings back the ticked ones that aren't. Installing re-registers the app from the copy Windows keeps
// for new accounts when it's still on the PC, and otherwise gets it from the Microsoft Store with winget (or opens its
// Store page when winget can't). Windows App and Xbox Gaming Services have their own Install buttons.
//
// Left out on purpose, because removing them breaks parts of Windows: the Microsoft Store and its purchase app, App
// Installer (winget), Windows Security, Edge and WebView2, the image and video extensions (HEIF, HEVC, VP9, AV1, WebP,
// Raw, MPEG-2, Web Media), the Widgets runtime, Cross Device, Start's companion, handwriting and language packs, the
// app frameworks, and every package Windows marks as non-removable (Start, the shell, sign-in, the lock screen…).

typedef struct {
    const wchar_t* name;     // shown
    const wchar_t* note;     // a muted word or two after it
    const wchar_t* pkg;      // package name, as Get-AppxPackage -Name takes it
    const wchar_t* pub;      // publisher hash, for the family name
    const wchar_t* store;    // Microsoft Store product ID, or NULL when it's no longer in the Store
} RmApp;
#define MSPUB L"8wekyb3d8bbwe"
static const RmApp RM_APPS[] = {
    { L"Calculator",              NULL,                                L"Microsoft.WindowsCalculator",            MSPUB, L"9WZDNCRFHVN5" },
    { L"Camera",                  NULL,                                L"Microsoft.WindowsCamera",                MSPUB, L"9WZDNCRFJBBG" },
    { L"Clipchamp",               L"video editor",                     L"Clipchamp.Clipchamp",                    L"yxz26nhyzhsrt", L"9P1J8S7CCWWT" },
    { L"Clock",                   L"alarms and timers",                L"Microsoft.WindowsAlarms",                MSPUB, L"9WZDNCRFJ3PR" },
    { L"Copilot",                 NULL,                                L"Microsoft.Copilot",                      MSPUB, L"9NHT9RB2F4HD" },
    { L"Dev Home",                L"Windows Advanced Settings",        L"Microsoft.Windows.DevHome",              MSPUB, L"9N8MHTPHNGVV" },
    { L"Feedback Hub",            NULL,                                L"Microsoft.WindowsFeedbackHub",           MSPUB, L"9NBLGGH4R32N" },
    { L"Game Bar",                L"Win+G overlay and game capture",   L"Microsoft.XboxGamingOverlay",            MSPUB, L"9NZKPSTSNW4P" },
    { L"Get Help",                NULL,                                L"Microsoft.GetHelp",                      MSPUB, L"9PKDZBMV1H3T" },
    { L"Mail and Calendar",       L"retired by Microsoft",             L"microsoft.windowscommunicationsapps",    MSPUB, NULL },
    { L"Maps",                    L"retired by Microsoft",             L"Microsoft.WindowsMaps",                  MSPUB, NULL },
    { L"Media Player",            NULL,                                L"Microsoft.ZuneMusic",                    MSPUB, L"9WZDNCRFJ3PT" },
    { L"Microsoft 365 Copilot",   L"the Office hub app",               L"Microsoft.MicrosoftOfficeHub",           MSPUB, L"9WZDNCRD29V9" },
    { L"Microsoft Bing",          NULL,                                L"Microsoft.BingSearch",                   MSPUB, L"9NZBF4GT040C" },
    { L"Microsoft Family Safety", NULL,                                L"MicrosoftCorporationII.MicrosoftFamily", MSPUB, L"9PDJDJS743XF" },
    { L"Microsoft News",          NULL,                                L"Microsoft.BingNews",                     MSPUB, L"9WZDNCRFHVFW" },
    { L"Microsoft Teams",         NULL,                                L"MSTeams",                                MSPUB, L"XP8BT8DW290MPQ" },
    { L"Movies & TV",             NULL,                                L"Microsoft.ZuneVideo",                    MSPUB, L"9WZDNCRFJ3P2" },
    { L"MSN Weather",             NULL,                                L"Microsoft.BingWeather",                  MSPUB, L"9WZDNCRFJ3Q2" },
    { L"Notepad",                 NULL,                                L"Microsoft.WindowsNotepad",               MSPUB, L"9MSMLRH6LZF3" },
    { L"Outlook (new)",           NULL,                                L"Microsoft.OutlookForWindows",            MSPUB, L"9NRX63209R7B" },
    { L"Paint",                   NULL,                                L"Microsoft.Paint",                        MSPUB, L"9PCFS5B6T72H" },
    { L"Phone Link",              NULL,                                L"Microsoft.YourPhone",                    MSPUB, L"9NMPJ99VJBWV" },
    { L"Photos",                  NULL,                                L"Microsoft.Windows.Photos",               MSPUB, L"9WZDNCRFJBH4" },
    { L"Power Automate",          NULL,                                L"Microsoft.PowerAutomateDesktop",         MSPUB, L"9NFTCH6J7FHV" },
    { L"Quick Assist",            NULL,                                L"MicrosoftCorporationII.QuickAssist",     MSPUB, L"9P7BP5VNWKX5" },
    { L"Snipping Tool",           L"Win+Shift+S and Print Screen use it", L"Microsoft.ScreenSketch",              MSPUB, L"9MZ95KL8MR0L" },
    { L"Solitaire Collection",    NULL,                                L"Microsoft.MicrosoftSolitaireCollection", MSPUB, L"9WZDNCRFHWD2" },
    { L"Sound Recorder",          NULL,                                L"Microsoft.WindowsSoundRecorder",         MSPUB, L"9WZDNCRFHWKN" },
    { L"Sticky Notes",            NULL,                                L"Microsoft.MicrosoftStickyNotes",         MSPUB, L"9NBLGGH4QGHW" },
    { L"Tips",                    NULL,                                L"Microsoft.Getstarted",                   MSPUB, NULL },
    { L"To Do",                   NULL,                                L"Microsoft.Todos",                        MSPUB, L"9NBLGGH5R558" },
    { L"Widgets",                 L"the Widgets board",                L"MicrosoftWindows.Client.WebExperience",  L"cw5n1h2txyewy", L"9MSSGKG348SP" },
    { L"Windows Terminal",        L"Windows falls back to the console", L"Microsoft.WindowsTerminal",             MSPUB, L"9N0DX20HK701" },
    { L"Xbox",                    NULL,                                L"Microsoft.GamingApp",                    MSPUB, L"9MV0B5HZVK9Z" },
    { L"Xbox Game Speech Window", NULL,                                L"Microsoft.XboxSpeechToTextOverlay",      MSPUB, L"9P086NHDNB9W" },
    { L"Xbox Identity Provider",  L"games use it to sign in to Xbox",  L"Microsoft.XboxIdentityProvider",         MSPUB, L"9WZDNCRD1HKW" },
    { L"Xbox Live in-game",       L"friends and invites in games",     L"Microsoft.Xbox.TCUI",                    MSPUB, L"9NKNC0LD5NN6" },
    // not in the list: the page's own Install buttons. Never part of Windows, so only the Store has them.
    { L"Windows App",             NULL,                                L"MicrosoftCorporationII.Windows365",      MSPUB, L"9N1F85V9T8BN" },
    { L"Xbox Gaming Services",    NULL,                                L"Microsoft.GamingServices",               MSPUB, L"9MWPM2CQNLHN" },
};
#define RM_ALL   (int)(sizeof(RM_APPS) / sizeof(RM_APPS[0]))
#define RM_LIST  (RM_ALL - 2)
#define RM_WINAPP (RM_ALL - 2)
#define RM_GAMING (RM_ALL - 1)
_Static_assert(RM_LIST <= ANIM_XCHK_N, "more listed apps than checkbox fade slots");

static BOOL g_rmTick[RM_ALL];
static signed char g_rmHave[RM_ALL];      // 1 installed, 0 not, -1 not known yet
static wchar_t g_rmVer[RM_ALL][32];       // the installed version
typedef struct { BYTE have[RM_ALL]; wchar_t ver[RM_ALL][32]; } RmList;   // what a listing found, posted to the window
static volatile LONG g_rmChecking = 0;
static BOOL g_rmJob = FALSE;              // an install or uninstall is running
static BOOL g_rmRemoving = FALSE;
static volatile LONG g_rmCur = -1;        // the app the job is on now
static volatile LONG g_rmStep = 0, g_rmSteps = 0;
static int  g_rmTop = 0, g_rmVisible = 1, g_rmThumbH = 0, g_rmDragOfs = 0, g_rmRowsTop = 0, g_rmRowH = 1;
static BOOL g_rmDrag = FALSE;
static RECT g_rcRmBox, g_rcRmBar;

static int rmIndex(const wchar_t* pkg) {
    for (int i = 0; i < RM_ALL; i++) if (!_wcsicmp(RM_APPS[i].pkg, pkg)) return i;
    return -1;
}

// ---------- running PowerShell, its output into the log ----------
typedef struct { BOOL removing, quiet; int n; int idx[RM_ALL]; } RmJob;

// Adds one line of the script's output. Lines starting @@ are the script talking to the window.
static void rmLine(RmJob* j, const wchar_t* s, RmList* have, BOOL* listed, int* ok, int* fail, BOOL* lastProgress) {
    while (*s == 0xFEFF || *s == L' ') s++;
    if (!*s) return;
    if (!wcsncmp(s, L"@@PKG ", 6)) {                   // @@PKG <name> <version>
        wchar_t pkg[120]; int k = 0;
        const wchar_t* p = s + 6;
        while (*p && *p != L' ' && k < 119) pkg[k++] = *p++;
        pkg[k] = 0;
        while (*p == L' ') p++;
        int i = rmIndex(pkg);
        if (i >= 0) {
            // a package can be listed more than once (one per architecture): keep the newest version
            if (!have->have[i] || appsVerCmp(p, have->ver[i]) > 0) lstrcpynW(have->ver[i], p, 32);
            have->have[i] = 1;
        }
        *listed = TRUE;
        return;
    }
    if (j->quiet) return;
    if (s[0] == L'@' && s[1] == L'@') {
        wchar_t kind[8] = L"", pkg[120] = L"";
        const wchar_t* p = s + 2;
        int k = 0;
        while (*p && *p != L' ' && k < 7) kind[k++] = *p++;
        kind[k] = 0;
        while (*p == L' ') p++;
        k = 0;
        while (*p && *p != L' ' && k < 119) pkg[k++] = *p++;
        pkg[k] = 0;
        while (*p == L' ') p++;
        int i = rmIndex(pkg);
        const wchar_t* name = i >= 0 ? RM_APPS[i].name : pkg;
        *lastProgress = FALSE;
        if (!wcscmp(kind, L"BEGIN")) {
            InterlockedExchange(&g_rmCur, i);
            InterlockedIncrement(&g_rmStep);
            appsSay(j->removing ? L"Removing %s…" : L"Installing %s…", name);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else if (!wcscmp(kind, L"OK")) {
            (*ok)++;
            appsSay(j->removing ? L"%s removed." : L"%s installed.", name);
        } else if (!wcscmp(kind, L"REG")) {
            appsSay(L"  Re-registered from the copy Windows keeps on this PC.");
        } else if (!wcscmp(kind, L"STORE")) {
            appsSay(L"  Not on this PC, so from the Microsoft Store.");
        } else if (!wcscmp(kind, L"PAGE")) {
            (*ok)++;
            appsSay(L"%s: winget couldn't get it, so its Microsoft Store page is open. Press Get there.", name);
        } else if (!wcscmp(kind, L"GONE")) {
            appsSay(L"%s wasn't installed.", name);
        } else if (!wcscmp(kind, L"FAIL")) {
            (*fail)++;
            if (*p) appsSay(j->removing ? L"%s wasn't removed: %s" : L"%s wasn't installed: %s", name, p);
            else appsSay(j->removing ? L"%s wasn't removed." : L"%s wasn't installed.", name);
        }
        return;
    }
    // winget's own lines: a spinner character alone is dropped, and a progress bar replaces the one before it
    if (!s[1] && (s[0] == L'-' || s[0] == L'\\' || s[0] == L'|' || s[0] == L'/')) return;
    BOOL progress = wcschr(s, 0x2588) || wcschr(s, 0x2592);
    if (progress && *lastProgress) appsSayOver(L"  %s", s);
    else appsSay(L"  %s", s);
    *lastProgress = progress;
}

static DWORD rmRunScript(RmJob* j, const wchar_t* script, RmList* have, BOOL* listed, int* ok, int* fail) {
    // -EncodedCommand takes the script as base64 of UTF-16LE, so nothing in it needs quoting
    DWORD bytes = (DWORD)(wcslen(script) * sizeof(wchar_t)), cch = 0;
    if (!CryptBinaryToStringW((const BYTE*)script, bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &cch)) return GetLastError();
    wchar_t exe[MAX_PATH];
    systemPath(exe, L"WindowsPowerShell\\v1.0\\powershell.exe");
    size_t total = wcslen(exe) + cch + 120;
    if (total > 32000) return ERROR_FILENAME_EXCED_RANGE;
    wchar_t* cmd = (wchar_t*)malloc(total * sizeof(wchar_t));
    if (!cmd) return ERROR_OUTOFMEMORY;
    int n = swprintf(cmd, total, L"\"%s\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ", exe);
    DWORD left = (DWORD)(total - n);
    if (!CryptBinaryToStringW((const BYTE*)script, bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, cmd + n, &left)) { free(cmd); return GetLastError(); }
    HANDLE rd = NULL; PROCESS_INFORMATION pi;
    DWORD e = spawnPiped(exe, cmd, &rd, &pi);
    free(cmd);
    if (e) return e;
    char buf[2048], line[4096]; DWORD got; int ln = 0;
    BOOL lastProgress = FALSE;
    while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r' || c == '\n' || ln == (int)sizeof(line) - 1) {
                line[ln] = 0;
                if (ln) {
                    wchar_t w[APPS_LOG_LINE];
                    if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, w, APPS_LOG_LINE)) w[0] = 0;
                    w[APPS_LOG_LINE - 1] = 0;
                    rmLine(j, w, have, listed, ok, fail, &lastProgress);
                }
                ln = 0;
                if (c != '\r' && c != '\n') line[ln++] = c;
            } else line[ln++] = c;
        }
    }
    if (ln) {
        line[ln] = 0;
        wchar_t w[APPS_LOG_LINE];
        if (MultiByteToWideChar(CP_UTF8, 0, line, -1, w, APPS_LOG_LINE)) { w[APPS_LOG_LINE - 1] = 0; rmLine(j, w, have, listed, ok, fail, &lastProgress); }
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    return waitExitCode(pi.hProcess);
}

static void rmAppend(wchar_t* s, size_t cch, const wchar_t* fmt, ...) {
    size_t n = wcslen(s);
    if (n >= cch - 1) return;
    va_list ap; va_start(ap, fmt);
    vswprintf(s + n, cch - n, fmt, ap);
    va_end(ap);
}

#define RM_SCRIPT 12000
static DWORD WINAPI rmProc(LPVOID arg) {
    RmJob* j = (RmJob*)arg;
    wchar_t* s = (wchar_t*)calloc(RM_SCRIPT, sizeof(wchar_t));
    RmList* have = (RmList*)calloc(1, sizeof(RmList));
    BOOL listed = FALSE; int ok = 0, fail = 0;
    DWORD e = ERROR_OUTOFMEMORY;
    if (s && have) {
        rmAppend(s, RM_SCRIPT, L"$ProgressPreference='SilentlyContinue';[Console]::OutputEncoding=New-Object Text.UTF8Encoding $false\n");
        if (j->removing) {
            // The signed-in user's copy only; Windows keeps the one it gives new accounts. Gaming Services ($a) is installed for
            // the whole PC, so with administrator rights it's removed for every account, as Microsoft's own steps do.
            rmAppend(s, RM_SCRIPT, L"$adm=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrators')\n"
                                   L"function U($n,$a){'@@BEGIN '+$n;$p=@(Get-AppxPackage -Name $n);if(!$p){'@@GONE '+$n;return}\n"
                                   L"try{if($a -and $adm){Get-AppxPackage -AllUsers -Name $n|Remove-AppxPackage -AllUsers -ErrorAction Stop}else{$p|Remove-AppxPackage -ErrorAction Stop};'@@OK '+$n}\n"
                                   L"catch{$m=($_.Exception.Message -split \"`r?`n\")[0];if($a -and !$adm){$m+=' It may need this app started as administrator.'};'@@FAIL '+$n+' '+$m}}\n");
            for (int k = 0; k < j->n; k++)
                rmAppend(s, RM_SCRIPT, L"U '%s' %s\n", RM_APPS[j->idx[k]].pkg, j->idx[k] == RM_GAMING ? L"$true" : L"$false");
        } else if (j->n) {
            // Re-register the copy Windows keeps for new accounts, if it's still here; otherwise the Microsoft Store through
            // winget; and when winget can't, open the app's Store page.
            rmAppend(s, RM_SCRIPT, L"$wg=Get-Command winget -ErrorAction SilentlyContinue\n"
                                   L"function I($n,$f,$s,$r){'@@BEGIN '+$n\n"
                                   L"if($r){try{Add-AppxPackage -RegisterByFamilyName -MainPackage $f -ErrorAction Stop;'@@REG '+$n;'@@OK '+$n;return}catch{}}\n"
                                   L"if(!$s){'@@FAIL '+$n+' it is no longer on this PC or in the Microsoft Store.';return}\n"
                                   L"'@@STORE '+$n\n"
                                   L"if($wg){& $wg.Source install --id $s --source msstore --accept-package-agreements --accept-source-agreements --disable-interactivity 2>&1|%%{\"$_\"}\n"
                                   L"if($LASTEXITCODE -eq 0 -or (Get-AppxPackage -Name $n)){'@@OK '+$n;return}}\n"
                                   L"Start-Process \"ms-windows-store://pdp/?ProductId=$s\";'@@PAGE '+$n}\n");
            for (int k = 0; k < j->n; k++) {
                const RmApp* a = &RM_APPS[j->idx[k]];
                BOOL inbox = j->idx[k] < RM_LIST;              // Windows App and Gaming Services were never part of Windows
                rmAppend(s, RM_SCRIPT, L"I '%s' '%s_%s' '%s' %s\n", a->pkg, a->pkg, a->pub, a->store ? a->store : L"", inbox ? L"$true" : L"$false");
            }
        }
        rmAppend(s, RM_SCRIPT, L"Get-AppxPackage|%%{'@@PKG '+$_.Name+' '+$_.Version}\n");
        e = rmRunScript(j, s, have, &listed, &ok, &fail);
    }
    if (!j->quiet) {
        if (e && !ok && !fail) appsSay(L"PowerShell couldn't run (error %lu).", e);
        else if (j->removing) appsSay(L"Done: %d removed, %d failed.", ok, fail);
        else appsSay(L"Done: %d installed, %d failed.", ok, fail);
    }
    free(s);
    if (!listed) { free(have); have = NULL; }
    BOOL quiet = j->quiet;
    free(j);
    if (!PostMessageW(g_hwnd, WM_APP_RMDONE, quiet ? 0 : 1, (LPARAM)have)) free(have);
    return 0;
}

// wParam: 1 when an install or uninstall ended; lParam: what's installed (malloc'd), or NULL if it couldn't be listed
static void rmOnDone(BOOL job, RmList* have) {
    if (have) {
        for (int i = 0; i < RM_ALL; i++) {
            g_rmHave[i] = have->have[i] ? 1 : 0;
            lstrcpynW(g_rmVer[i], have->have[i] ? have->ver[i] : L"", 32);
        }
        free(have);
    }
    if (job) {
        g_rmJob = FALSE;
        InterlockedExchange(&g_rmCur, -1);
        InterlockedExchange(&g_logOpen, 0);
        InterlockedExchange(&g_busy, 0);
        // what was done is unticked; a tick that failed stays, so it can be tried again
        for (int i = 0; i < RM_ALL; i++)
            if (g_rmTick[i] && g_rmHave[i] == (g_rmRemoving ? 0 : 1)) g_rmTick[i] = FALSE;
    } else InterlockedExchange(&g_rmChecking, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Entering the page: what's installed
static void rmRefresh(void) {
    static BOOL first = TRUE;
    if (first) { first = FALSE; for (int i = 0; i < RM_ALL; i++) g_rmHave[i] = -1; }
    if (g_rmJob || InterlockedCompareExchange(&g_rmChecking, 1, 0) != 0) return;
    RmJob* j = (RmJob*)calloc(1, sizeof(RmJob));
    if (!j) { InterlockedExchange(&g_rmChecking, 0); return; }
    j->quiet = TRUE;
    HANDLE t = CreateThread(NULL, 0, rmProc, j, 0, NULL);
    if (t) CloseHandle(t); else { free(j); InterlockedExchange(&g_rmChecking, 0); }
}

static void rmStart(BOOL removing, const int* idx, int n) {
    if (n <= 0 || g_busy != 0) return;
    RmJob* j = (RmJob*)calloc(1, sizeof(RmJob));
    if (!j) return;
    j->removing = removing; j->n = n;
    memcpy(j->idx, idx, n * sizeof(int));
    if (!jobBegin()) { free(j); return; }
    InterlockedExchange(&g_logOpen, 1);
    if (g_appsLogN) appsLogAdd(L"", FALSE);
    wchar_t head[200];
    if (n == 1) swprintf(head, 200, removing ? L"> Uninstall %s" : L"> Install %s", RM_APPS[idx[0]].name);
    else swprintf(head, 200, removing ? L"> Uninstall %d apps" : L"> Install %d apps", n);
    appsLogAdd(head, FALSE);
    g_rmJob = TRUE; g_rmRemoving = removing;
    InterlockedExchange(&g_rmCur, -1);
    InterlockedExchange(&g_rmStep, 0);
    InterlockedExchange(&g_rmSteps, n);
    HANDLE t = CreateThread(NULL, 0, rmProc, j, 0, NULL);
    if (t) CloseHandle(t);
    else {
        free(j);
        g_rmJob = FALSE;
        InterlockedExchange(&g_logOpen, 0);
        InterlockedExchange(&g_busy, 0);
        appsLogAdd(L"Couldn't start.", FALSE);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void rmUninstallClick(void) {
    int idx[RM_ALL], n = 0;
    wchar_t names[1600] = L"";
    for (int i = 0; i < RM_LIST; i++) if (g_rmTick[i] && g_rmHave[i] == 1) {
        idx[n++] = i;
        if (names[0]) wcsncat(names, L", ", 1600 - wcslen(names) - 1);
        wcsncat(names, RM_APPS[i].name, 1600 - wcslen(names) - 1);
    }
    if (!n) { okDialog(L"Nothing to uninstall", L"Tick the installed apps you want removed."); return; }
    wchar_t body[2000], title[80];
    swprintf(body, 2000, n == 1 ? L"%s.\n\nIt's removed from your account only. You can bring it back with Install on this page."
                                : L"%s.\n\nThey're removed from your account only. You can bring them back with Install on this page.", names);
    swprintf(title, 80, n == 1 ? L"Uninstall %d app?" : L"Uninstall %d apps?", n);
    if (!showDialog(g_hwnd, title, body, NULL, 0, L"Uninstall", L"Cancel", 1)) return;
    rmStart(TRUE, idx, n);
}

static void rmInstallClick(void) {
    int idx[RM_ALL], n = 0;
    for (int i = 0; i < RM_LIST; i++) if (g_rmTick[i] && g_rmHave[i] == 0) idx[n++] = i;
    if (!n) { okDialog(L"Nothing to install", L"Tick the apps marked \"Not installed\" that you want back."); return; }
    rmStart(FALSE, idx, n);
}

// ---------- the list: scrolling and ticking ----------
// The rows shown: an app Microsoft has retired (no Store ID) only while it's still installed, as it can't come back
static int g_rmShow[RM_LIST], g_rmShowN = 0;
static void rmBuildRows(void) {
    g_rmShowN = 0;
    for (int i = 0; i < RM_LIST; i++) {
        if (!RM_APPS[i].store && g_rmHave[i] != 1) { g_rmTick[i] = FALSE; continue; }
        g_rmShow[g_rmShowN++] = i;
    }
}
static void rmClampTop(void) {
    int maxTop = g_rmShowN - g_rmVisible;
    if (g_rmTop > maxTop) g_rmTop = maxTop;
    if (g_rmTop < 0) g_rmTop = 0;
}
static BOOL rmWheel(int x, int y, int delta) {
    POINT p = { x, y };
    if (!PtInRect(&g_rcRmBox, p)) return FALSE;
    g_rmTop += (delta > 0) ? -3 : 3;
    rmClampTop();
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}
static void rmDragTo(int y) {
    int span = (g_rcRmBar.bottom - g_rcRmBar.top) - g_rmThumbH, maxTop = g_rmShowN - g_rmVisible;
    if (span <= 0 || maxTop <= 0) return;
    int v = y - g_rcRmBar.top - g_rmDragOfs;
    if (v < 0) v = 0;
    if (v > span) v = span;
    g_rmTop = (int)(((long long)v * maxTop + span / 2) / span);
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static BOOL rmMouseDown(int x, int y) {
    POINT p = { x, y };
    if (g_rmShowN <= g_rmVisible || !PtInRect(&g_rcRmBar, p)) return FALSE;
    int span = (g_rcRmBar.bottom - g_rcRmBar.top) - g_rmThumbH, maxTop = g_rmShowN - g_rmVisible;
    int ty = g_rcRmBar.top + (maxTop > 0 ? (int)((long long)span * g_rmTop / maxTop) : 0);
    g_rmDragOfs = (y >= ty && y < ty + g_rmThumbH) ? y - ty : g_rmThumbH / 2;
    g_rmDrag = TRUE;
    SetCapture(g_hwnd);
    rmDragTo(y);
    return TRUE;
}

// The hovered row changes without the hovered widget changing, so a move over the list repaints when it crosses rows
static void rmMouseMove(int x, int y) {
    static int last = -2;
    POINT p = { x, y };
    int r = PtInRect(&g_rcRmBox, p) ? (y - g_rmRowsTop) / max(1, g_rmRowH) : -1;
    if (r != last) { last = r; InvalidateRect(g_hwnd, NULL, FALSE); }
}

static void rmStoreClick(int i);
static void rmClick(int id, int y) {
    if (g_busy != 0) return;
    switch (id) {
    case A_X_LIST: {
        int r = g_rmTop + (y - g_rmRowsTop) / max(1, g_rmRowH);
        if (y >= g_rmRowsTop && r >= 0 && r < g_rmShowN && g_rmHave[g_rmShow[r]] >= 0) g_rmTick[g_rmShow[r]] = !g_rmTick[g_rmShow[r]];
        break;
    }
    case A_X_ALL:  for (int r = 0; r < g_rmShowN; r++) if (g_rmHave[g_rmShow[r]] >= 0) g_rmTick[g_rmShow[r]] = TRUE; break;
    case A_X_NONE: for (int i = 0; i < RM_LIST; i++) g_rmTick[i] = FALSE; break;
    case A_X_UNINSTALL: rmUninstallClick(); break;
    case A_X_INSTALL:   rmInstallClick(); break;
    case A_X_WINAPP: rmStoreClick(RM_WINAPP); break;
    case A_X_GAMING: rmStoreClick(RM_GAMING); break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- the page ----------
#define RM_ROW 32                          // a list row, in dp
#define RM_LOG_MIN 150                     // the Progress log's least height, in dp

static const wchar_t* rmState(int i) {
    if (g_rmJob && g_rmCur == i) return g_rmRemoving ? L"Removing…" : L"Installing…";
    if (g_rmHave[i] < 0) return L"Checking…";
    return g_rmHave[i] ? L"Installed" : L"Not installed";
}

// A Store app's own row: its name, its version under it, and Install or Uninstall at the right
static void rmStoreRow(int x1, int x2, int y, int id, int i) {
    wchar_t sub[80];
    BOOL busyHere = g_rmJob && g_rmCur == i;
    if (!busyHere && g_rmHave[i] == 1 && g_rmVer[i][0]) swprintf(sub, 80, L"Version %s", g_rmVer[i]);
    else lstrcpynW(sub, rmState(i), 80);
    int lr = x2 - dp(140) - dp(16);
    int h = offRow(x1, lr, y, RM_APPS[i].name, sub, busyHere ? g_accent : mutedRGB());
    int ch = dp(OFF_CTL_H), by = y + (h - ch) / 2;
    BOOL have = g_rmHave[i] == 1;
    aioButton(id, x2 - dp(140), by, x2, by + ch, have ? L"Uninstall" : L"Install", FALSE, g_busy == 0 && g_rmHave[i] >= 0, FALSE);
}

// Its button: Install when it isn't there, Uninstall (after one question) when it is
static void rmStoreClick(int i) {
    if (g_rmHave[i] == 0) { rmStart(FALSE, &i, 1); return; }
    if (g_rmHave[i] != 1) return;
    wchar_t title[80], body[400];
    swprintf(title, 80, L"Uninstall %s?", RM_APPS[i].name);
    if (i == RM_GAMING)
        swprintf(body, 400, L"The Xbox app and Game Pass PC games need Gaming Services, and won't install or start games without it. "
                            L"%s You can install it again here.", g_elevated ? L"It's removed for every account on this PC." : L"");
    else
        swprintf(body, 400, L"It's removed from your account. You can install it again here.");
    if (!showDialog(g_hwnd, title, body, NULL, 0, L"Uninstall", L"Cancel", 1)) return;
    rmStart(TRUE, &i, 1);
}

static void drawRemove(int cL, int cR) {
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    int x1 = cL + dp(20), x2 = cR - dp(20), ch = dp(OFF_CTL_H);
    BOOL en = g_busy == 0;

    // the list takes the height the log doesn't need, in whole rows
    int rowH = dp(RM_ROW);
    int fixed = dp(92) + 2 * dp(OFF_ROW2 + 1) + dp(OFF_TITLE) + dp(4) + dp(8) + dp(OFF_ROW2) + dp(8);
    int room = g_bh - pad - dp(RM_LOG_MIN) - dp(10) - top - fixed;
    int vis = room / rowH;
    rmBuildRows();
    if (vis > g_rmShowN) vis = g_rmShowN;
    if (vis > ANIM_XROW_N) vis = ANIM_XROW_N;
    if (vis < 3) vis = 3;
    g_rmVisible = vis;
    rmClampTop();
    int listH = vis * rowH;
    int cardB = top + fixed + listH;
    fillRoundRect(cL, top, cR, cardB, dp(8), sf.r, sf.g, sf.b, sf.a);
    drawPageHeader(0xE74D, cL, cR, top, L"Remove Apps", L"Microsoft's apps that come with Windows, for your account only.");
    int y = top + dp(92);

    // ---- the two Store apps ----
    rmStoreRow(x1, x2, y, A_X_WINAPP, RM_WINAPP);
    y += dp(OFF_ROW2); offSep(x1, x2, y); y += dp(1);
    rmStoreRow(x1, x2, y, A_X_GAMING, RM_GAMING);
    y += dp(OFF_ROW2); offSep(cL, cR, y); y += dp(1);

    // ---- the list ----
    {
        RECT h = { x1, y, x2 - dp(200), y + dp(OFF_TITLE) };
        drawText(g_fBodyBold, L"Apps that come with Windows", h, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        int bh = dp(24), by = y + (dp(OFF_TITLE) - bh) / 2;
        aioButton(A_X_NONE, x2 - dp(80), by, x2, by + bh, L"Clear", FALSE, en, FALSE);
        aioButton(A_X_ALL, x2 - dp(168), by, x2 - dp(88), by + bh, L"Select all", FALSE, en, FALSE);
        y += dp(OFF_TITLE) + dp(4);
    }
    RECT box = { cL + dp(12), y, cR - dp(12), y + listH };
    g_rcRmBox = box;
    Col kf = g_dark ? (Col){ 0,0,0, 0.18 } : (Col){ 0,0,0, 0.03 };
    fillRoundRect(box.left, box.top, box.right, box.bottom, dp(4), kf.r, kf.g, kf.b, kf.a);
    BOOL scroll = g_rmShowN > vis;
    RECT rows = { box.left, box.top, box.right - (scroll ? dp(14) : 0), box.bottom };
    aioRegister(A_X_LIST, rows, NULL, 0, FALSE);
    g_rmRowsTop = box.top; g_rmRowH = rowH;
    POINT m; GetCursorPos(&m); ScreenToClient(g_hwnd, &m);
    BOOL overList = g_hotBtn == BTN_WIDGET + A_X_LIST && PtInRect(&rows, m);
    for (int v = 0; v < vis; v++) {
        if (g_rmTop + v >= g_rmShowN) break;
        int i = g_rmShow[g_rmTop + v];
        int ry = box.top + v * rowH;
        BOOL known = g_rmHave[i] >= 0, ren = en && known;
        double hv = anim(ANIM_XROW + v, overList && ren && m.y >= ry && m.y < ry + rowH);
        fillHover(rows.left + dp(2), ry + dp(2), rows.right - dp(2), ry + rowH - dp(2), dp(4), hv);
        int mid = ry + rowH / 2;
        drawCheckbox(rows.left + dp(20), mid, dp(16), anim(ANIM_XCHK + i, g_rmTick[i]), ren);
        // the name, then its note in muted text; the state at the right
        const wchar_t* st = rmState(i);
        int stW = dp(110);
        RECT sr = { rows.right - dp(12) - stW, ry, rows.right - dp(12), ry + rowH };
        BOOL busyHere = g_rmJob && g_rmCur == i;
        drawText(g_fSmall, st, sr, busyHere ? g_accent : (g_rmHave[i] == 1 ? textRGB() : mutedRGB()), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int nx = rows.left + dp(40);
        RECT nr = { nx, ry, sr.left - dp(8), ry + rowH };
        drawText(g_fBody, RM_APPS[i].name, nr, g_rmHave[i] == 0 ? mutedRGB() : textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (RM_APPS[i].note) {
            int nw = textWidth(g_fBody, RM_APPS[i].name) + dp(10);
            RECT tr = { nx + nw, ry, sr.left - dp(8), ry + rowH };
            if (tr.right - tr.left > dp(40))
                drawText(g_fSmall, RM_APPS[i].note, tr, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (v < vis - 1) { Col s = sepCol(); fillRect(rows.left + dp(40), ry + rowH - max(1, dp(1)), rows.right - dp(8), ry + rowH, s.r, s.g, s.b, s.a * 0.5); }
    }
    SetRectEmpty(&g_rcRmBar);
    if (scroll) {
        RECT bar = { box.right - dp(12), box.top + dp(4), box.right - dp(2), box.bottom - dp(4) };
        g_rcRmBar = bar;
        int trackH = bar.bottom - bar.top, maxTop = g_rmShowN - vis;
        int thumb = trackH * vis / g_rmShowN;
        if (thumb < dp(24)) thumb = dp(24);
        g_rmThumbH = thumb;
        int ty = bar.top + (int)((long long)(trackH - thumb) * g_rmTop / (maxTop ? maxTop : 1));
        Col s = sepCol();
        fillRoundRect(bar.right - dp(6), ty, bar.right - dp(2), ty + thumb, dp(2), s.r, s.g, s.b, s.a * (g_rmDrag ? 5 : 3));
    }
    y = box.bottom + dp(8);

    // ---- the actions ----
    {
        int nUn = 0, nIn = 0;
        for (int i = 0; i < RM_LIST; i++) if (g_rmTick[i]) { if (g_rmHave[i] == 1) nUn++; else if (g_rmHave[i] == 0) nIn++; }
        int bw = dp(120), gap = dp(8);
        RECT r = { x1, y, x2 - 2 * bw - gap - dp(16), y + dp(OFF_ROW2) };
        drawText(g_fSmall, L"Uninstall removes the ticked apps that are installed; Install brings back ticked ones that aren't.",
                 r, mutedRGB(), DT_LEFT | DT_WORDBREAK);
        wchar_t tu[40], ti[40];
        if (nUn) swprintf(tu, 40, L"Uninstall (%d)", nUn); else wcscpy(tu, L"Uninstall");
        if (nIn) swprintf(ti, 40, L"Install (%d)", nIn); else wcscpy(ti, L"Install");
        int by = y + (dp(OFF_ROW2) - ch - dp(2)) / 2;
        aioButton(A_X_INSTALL, x2 - 2 * bw - gap, by, x2 - bw - gap, by + ch + dp(2), ti, FALSE, en && nIn > 0, FALSE);
        aioButton(A_X_UNINSTALL, x2 - bw, by, x2, by + ch + dp(2), tu, TRUE, en && nUn > 0, FALSE);
    }

    wchar_t now[200] = L"";
    if (g_rmJob) {
        LONG cur = g_rmCur;
        if (cur >= 0 && cur < RM_ALL)
            swprintf(now, 200, L"%s %s (%ld of %ld)…", g_rmRemoving ? L"Removing" : L"Installing", RM_APPS[cur].name, g_rmStep, g_rmSteps);
        else wcscpy(now, L"Starting PowerShell…");
    } else if (g_rmChecking) wcscpy(now, L"Checking what's installed…");
    appsDrawLog(cL, cR, cardB + dp(10), now);
}
