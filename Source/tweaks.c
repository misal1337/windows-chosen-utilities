// Misc Tweaks page: small one-click changes.
#include <sddl.h>

#define RES_TERMINAL_SETTINGS 2           // app.rc embeds settings.json under this id
#define TERMINAL_STATE_DIR L"Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState"

static wchar_t g_tweakStatus[320];

static const BYTE* embeddedData(int id, DWORD* size) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));   // 10 = RT_RCDATA
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    *size = r ? SizeofResource(NULL, r) : 0;
    return g ? (const BYTE*)LockResource(g) : NULL;
}

static BOOL fileMatches(const wchar_t* path, const BYTE* data, DWORD n) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BYTE* buf = (BYTE*)malloc(n + 1);
    DWORD got = 0;                                       // asking for one byte extra catches a longer file
    BOOL same = buf && ReadFile(h, buf, n + 1, &got, NULL) && got == n && memcmp(buf, data, n) == 0;
    free(buf);
    CloseHandle(h);
    return same;
}

// Writes beside the target first, so a failure can't leave a half-written settings file.
static DWORD writeFileReplacing(const wchar_t* path, const BYTE* data, DWORD n) {
    wchar_t tmp[MAX_PATH];
    if (swprintf(tmp, MAX_PATH, L"%s.wcu-tmp", path) < 0) return ERROR_FILENAME_EXCED_RANGE;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wrote = 0, err = 0;
    if (!WriteFile(h, data, n, &wrote, NULL) || wrote != n || !FlushFileBuffers(h)) err = GetLastError();
    CloseHandle(h);
    if (!err) {
        DWORD attr = GetFileAttributesW(path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(path, attr & ~FILE_ATTRIBUTE_READONLY);
        // Terminal or an antivirus may have the file open for a moment
        for (int tries = 0; tries < 5; tries++) {
            if (MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
            err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) break;
            Sleep(100);
        }
    }
    DeleteFileW(tmp);
    return err ? err : ERROR_WRITE_FAULT;
}

// Returns 0 once the file on disk is confirmed identical to the embedded settings.json.
static DWORD restoreTerminalSettings(void) {
    DWORD n = 0;
    const BYTE* data = embeddedData(RES_TERMINAL_SETTINGS, &n);
    if (!data || !n) return ERROR_RESOURCE_DATA_NOT_FOUND;

    wchar_t base[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!len || len >= MAX_PATH) return ERROR_ENVVAR_NOT_FOUND;
    if (swprintf(dir, MAX_PATH, L"%s\\" TERMINAL_STATE_DIR, base) < 0 ||
        swprintf(path, MAX_PATH, L"%s\\settings.json", dir) < 0) return ERROR_FILENAME_EXCED_RANGE;

    int made = SHCreateDirectoryExW(NULL, dir, NULL);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) return (DWORD)made;
    DWORD err = writeFileReplacing(path, data, n);
    if (err) return err;
    return fileMatches(path, data, n) ? 0 : ERROR_INVALID_DATA;
}

static void tweakTerminalRestore(void) {
    DWORD err = restoreTerminalSettings();
    if (!err) wcscpy(g_tweakStatus, L"Terminal settings restored. The file matches byte for byte.");
    else if (err == ERROR_INVALID_DATA)
        wcscpy(g_tweakStatus, L"The file was written, but it no longer matches. Something else changed it straight away.");
    else swprintf(g_tweakStatus, 320, L"Couldn't restore Terminal settings (error %lu).", err);
}

// ---------- Enable / Disable: what each button does ----------
enum { TW_EXPLORE_OFF, TW_EXPLORE_ON, TW_GALLERY_OFF, TW_GALLERY_ON, TW_VBS_OFF, TW_VBS_ON,
       TW_RDP_OFF, TW_RDP_ON, TW_MAPDELAY_OFF, TW_MAPDELAY_ON, TW_WUDRV_OFF, TW_WUDRV_ON, TW_THEME_DARK, TW_THEME_WHITE, TW_REGION, TW_ANNOY_OFF, TW_ANNOY_ON, TW_COUNT };
typedef struct { int res[2]; BOOL trustedInstaller; BOOL admin; } TweakAct;    // res: app.rc ids, imported in order
static const TweakAct TWEAK[TW_COUNT] = {
    { { 10, 0 },  TRUE,  TRUE  },
    { { 11, 0 },  TRUE,  TRUE  },
    { { 12, 0 },  FALSE, FALSE },
    { { 13, 0 },  FALSE, FALSE },
    { { 14, 15 }, FALSE, TRUE  },         // the user's file, then what it leaves on
    { { 16, 0 },  FALSE, TRUE  },
    { { 17, 0 },  FALSE, TRUE  },
    { { 18, 0 },  FALSE, TRUE  },
    { { 19, 0 },  FALSE, TRUE  },
    { { 20, 0 },  FALSE, TRUE  },
    { { 21, 0 },  FALSE, TRUE  },
    { { 22, 0 },  FALSE, TRUE  },
    { { 23, 0 },  FALSE, FALSE },
    { { 24, 0 },  FALSE, FALSE },
    { { 0, 0 },   FALSE, FALSE },         // no .reg files: see region.c
    { { 25, 0 },  FALSE, FALSE },
    { { 26, 0 },  FALSE, FALSE },
};

#define SPOTLIGHT_LEARN L"SOFTWARE\\Classes\\DesktopBackground\\Shell\\.SpotlightLearnMore"
#define SPOTLIGHT_NEXT  L"SOFTWARE\\Classes\\DesktopBackground\\Shell\\.SpotlightNextImage"
#define GALLERY_CLSID   L"CLSID\\{e88865ea-0e1c-4e20-9aa6-edcd0212c87c}"
#define DEVICEGUARD     L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard"
#define HVCI_KEY        DEVICEGUARD L"\\Scenarios\\HypervisorEnforcedCodeIntegrity"
#define LSA_KEY         L"SYSTEM\\CurrentControlSet\\Control\\Lsa"
#define NETPROV_KEY     L"SYSTEM\\CurrentControlSet\\Control\\NetworkProvider"
#define POLICY_WINDOWS  L"SOFTWARE\\Policies\\Microsoft\\Windows\\"
#define PERSONALIZE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define CONTENT_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"
#define ENGAGEMENT_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\UserProfileEngagement"

static BOOL g_twExplore, g_twGallery, g_twVbsWanted, g_twVbsHvci, g_twVbsPolicy, g_twVbsLocked;
static BOOL g_twRdp, g_twMapDelay, g_twWuDrv, g_twWuLimited;   // g_twWuLimited: the other two driver policies are set
static BOOL g_twAppsDark, g_twSystemDark;
static wchar_t g_twRegionNow[220];
static int g_twAnnoyOn;                   // how many of the three are on; a missing value counts as on
static volatile LONG g_twVbsStatus = -1;  // Win32_DeviceGuard: 0 off, 1 enabled but not running, 2 running; -1 unknown
static int g_twAct = -1;                  // the job running, or -1
static wchar_t g_twResult[320];

static DWORD regDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD fallback) {
    DWORD v = fallback, cb = sizeof(v);
    if (RegGetValueW(root, path, name, RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS) return fallback;
    return v;
}

static void tweaksReadSettings(void) {
    g_twExplore = regKeyExists(HKEY_LOCAL_MACHINE, SPOTLIGHT_LEARN) || regKeyExists(HKEY_LOCAL_MACHINE, SPOTLIGHT_NEXT);
    g_twGallery = regDword(HKEY_CLASSES_ROOT, GALLERY_CLSID, L"System.IsPinnedToNameSpaceTree", 1) != 0;
    g_twVbsHvci = regDword(HKEY_LOCAL_MACHINE, HVCI_KEY, L"Enabled", 0) == 1;
    g_twVbsWanted = g_twVbsHvci || regDword(HKEY_LOCAL_MACHINE, DEVICEGUARD, L"EnableVirtualizationBasedSecurity", 0) == 1;
    g_twVbsPolicy = regDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeviceGuard",
                             L"EnableVirtualizationBasedSecurity", 0) == 1;
    g_twVbsLocked = regDword(HKEY_LOCAL_MACHINE, DEVICEGUARD, L"Locked", 0) == 1
                 || regDword(HKEY_LOCAL_MACHINE, HVCI_KEY, L"Locked", 0) == 1;
    g_twRdp = regDword(HKEY_LOCAL_MACHINE, LSA_KEY, L"LimitBlankPasswordUse", 1) == 0;
    g_twMapDelay = regDword(HKEY_LOCAL_MACHINE, NETPROV_KEY, L"RestoreConnection", 1) == 0;
    g_twWuDrv = regDword(HKEY_LOCAL_MACHINE, POLICY_WINDOWS L"WindowsUpdate", L"ExcludeWUDriversInQualityUpdate", 0) != 1;
    g_twWuLimited = regDword(HKEY_LOCAL_MACHINE, POLICY_WINDOWS L"Device Metadata", L"PreventDeviceMetadataFromNetwork", 0) == 1
                 || regDword(HKEY_LOCAL_MACHINE, POLICY_WINDOWS L"DriverSearching", L"SearchOrderConfig", 1) == 0;
    g_twAppsDark = regDword(HKEY_CURRENT_USER, PERSONALIZE_KEY, L"AppsUseLightTheme", 1) == 0;
    g_twSystemDark = regDword(HKEY_CURRENT_USER, PERSONALIZE_KEY, L"SystemUsesLightTheme", 1) == 0;
    regionDescribeNow(g_twRegionNow, 220);
    g_twAnnoyOn = (regDword(HKEY_CURRENT_USER, CONTENT_KEY, L"SubscribedContent-310093Enabled", 1) != 0)
                + (regDword(HKEY_CURRENT_USER, CONTENT_KEY, L"SubscribedContent-338389Enabled", 1) != 0)
                + (regDword(HKEY_CURRENT_USER, ENGAGEMENT_KEY, L"ScoobeSystemSettingEnabled", 1) != 0);
}

// What is running now, as opposed to what's set for the next start. WMI can take a moment, so it has its own thread.
static DWORD WINAPI tweaksVbsProc(LPVOID arg) {
    (void)arg;
    LONG status = -1;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWbemServices* svc = wmiOpen(L"ROOT\\Microsoft\\Windows\\DeviceGuard");
    if (svc) {
        IEnumWbemClassObject* en = wmiQuery(svc, L"SELECT VirtualizationBasedSecurityStatus FROM Win32_DeviceGuard");
        if (en) {
            IWbemClassObject* o = NULL; ULONG got = 0;
            if (en->lpVtbl->Next(en, WBEM_INFINITE, 1, &o, &got) == S_OK && got) {
                VARIANT v; VariantInit(&v);
                if (SUCCEEDED(o->lpVtbl->Get(o, L"VirtualizationBasedSecurityStatus", 0, &v, NULL, NULL)) && (v.vt == VT_I4 || v.vt == VT_UI4))
                    status = v.lVal;
                VariantClear(&v);
                o->lpVtbl->Release(o);
            }
            en->lpVtbl->Release(en);
        }
        svc->lpVtbl->Release(svc);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    InterlockedExchange(&g_twVbsStatus, status);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    return 0;
}

static void tweaksRefresh(void) {
    tweaksReadSettings();
    HANDLE t = CreateThread(NULL, 0, tweaksVbsProc, NULL, 0, NULL);
    if (t) CloseHandle(t);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Did this job leave the setting the way it should?
static BOOL tweakIsApplied(int act) {
    tweaksReadSettings();
    switch (act) {
    case TW_EXPLORE_OFF: return !g_twExplore;
    case TW_EXPLORE_ON:  return g_twExplore;
    case TW_GALLERY_OFF: return !g_twGallery;
    case TW_GALLERY_ON:  return g_twGallery;
    case TW_VBS_OFF:     return !g_twVbsWanted;
    case TW_VBS_ON:      return g_twVbsWanted && g_twVbsHvci;
    case TW_RDP_OFF:     return !g_twRdp;
    case TW_RDP_ON:      return g_twRdp;
    case TW_MAPDELAY_OFF: return !g_twMapDelay;
    case TW_MAPDELAY_ON: return g_twMapDelay;
    case TW_WUDRV_OFF:   return !g_twWuDrv;
    case TW_WUDRV_ON:    return g_twWuDrv && !g_twWuLimited;
    case TW_THEME_DARK:  return g_twAppsDark && g_twSystemDark;
    case TW_THEME_WHITE: return !g_twAppsDark && !g_twSystemDark;
    case TW_ANNOY_OFF:   return g_twAnnoyOn == 0;
    case TW_ANNOY_ON:    return g_twAnnoyOn == 3;
    }
    return FALSE;
}

// ---------- running a program as TrustedInstaller ----------
static BOOL enablePrivilege(const wchar_t* name) {
    HANDLE tok; TOKEN_PRIVILEGES tp = { 1 };
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return FALSE;
    BOOL ok = LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid);
    if (ok) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        ok = GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(tok);
    return ok;
}

// Windows starts the TrustedInstaller service on demand and stops it when idle. Returns its process id, or 0.
static DWORD trustedInstallerPid(void) {
    DWORD pid = 0;
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE s = m ? OpenServiceW(m, L"TrustedInstaller", SERVICE_START | SERVICE_QUERY_STATUS) : NULL;
    for (int i = 0; s && !pid && i < 150; i++) {          // up to 30 seconds
        SERVICE_STATUS_PROCESS st; DWORD need;
        if (!QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (BYTE*)&st, sizeof(st), &need)) break;
        if (st.dwCurrentState == SERVICE_RUNNING && st.dwProcessId) pid = st.dwProcessId;
        else {
            if (st.dwCurrentState == SERVICE_STOPPED) StartServiceW(s, 0, NULL);
            Sleep(200);
        }
    }
    if (s) CloseServiceHandle(s);
    if (m) CloseServiceHandle(m);
    return pid;
}

// Needs administrator rights. The new process is made a child of the service, so it gets the service's token.
static DWORD runAsTrustedInstaller(const wchar_t* exe, wchar_t* cmd, DWORD* code) {
    if (!enablePrivilege(SE_DEBUG_NAME)) return ERROR_PRIVILEGE_NOT_HELD;
    DWORD pid = trustedInstallerPid();
    if (!pid) return ERROR_SERVICE_NOT_ACTIVE;
    HANDLE parent = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, pid);
    if (!parent) return GetLastError();

    STARTUPINFOEXW si; memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    SIZE_T cb = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &cb);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(cb);
    DWORD err = 0;
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &cb)) {
        err = GetLastError();
    } else {
        PROCESS_INFORMATION pi;
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent, sizeof(parent), NULL, NULL) ||
            !CreateProcessW(exe, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL, &si.StartupInfo, &pi)) {
            err = GetLastError();
        } else {
            CloseHandle(pi.hThread);
            *code = waitExitCode(pi.hProcess);
        }
        DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    free(si.lpAttributeList);
    CloseHandle(parent);
    return err;
}

// ---------- importing the embedded .reg files ----------
static DWORD regImportFile(const wchar_t* path, BOOL asTI) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH * 2 + 32];
    systemPath(exe, L"reg.exe");
    swprintf(cmd, MAX_PATH * 2 + 32, L"\"%s\" import \"%s\"", exe, path);
    if (!asTI) return runWithPercent(exe, cmd, NULL);
    DWORD code = 1, err = runAsTrustedInstaller(exe, cmd, &code);
    return err ? err : code;
}

// Elevated, the temporary file goes in Windows' temp folder, readable by administrators and SYSTEM only:
// a normal-rights program must not be able to swap what gets imported with these rights.
static DWORD regImportEmbedded(int res, BOOL asTI) {
    DWORD n = 0;
    const BYTE* data = embeddedData(res, &n);
    if (!data || !n) return ERROR_RESOURCE_DATA_NOT_FOUND;

    wchar_t dir[MAX_PATH], tmp[MAX_PATH];
    if (g_elevated) { GetWindowsDirectoryW(dir, MAX_PATH); wcscat(dir, L"\\Temp\\"); }
    else GetTempPathW(MAX_PATH, dir);
    if (swprintf(tmp, MAX_PATH, L"%swcu_%lu_%llu_%d.reg", dir, GetCurrentProcessId(), GetTickCount64(), res) < 0)
        return ERROR_FILENAME_EXCED_RANGE;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    if (g_elevated && !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)",
            SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) return GetLastError();
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
    if (err) return err;
    DWORD wrote = 0;
    if (!WriteFile(h, data, n, &wrote, NULL) || wrote != n) err = GetLastError();
    CloseHandle(h);
    if (!err) err = regImportFile(tmp, asTI);
    DeleteFileW(tmp);
    return err;
}

static DWORD tweakRun(int act) {
    for (int i = 0; i < 2 && TWEAK[act].res[i]; i++) {
        DWORD err = regImportEmbedded(TWEAK[act].res[i], TWEAK[act].trustedInstaller);
        if (err) return err;
    }
    return 0;
}

// The elevated copy of this exe. It takes a job number, never a file, so it can only run what's built in.
static int tweakHelperMain(const wchar_t* cmd) {
    int act = _wtoi(cmd + 8);
    if (act < 0 || act >= TW_COUNT || !TWEAK[act].admin) return ERROR_INVALID_PARAMETER;
    return (int)tweakRun(act);
}

static DWORD WINAPI tweakProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    DWORD code = g_twAct == TW_REGION ? regionApply(&g_region) : helper ? waitExitCode(helper) : tweakRun(g_twAct);
    if (!code && (g_twAct == TW_THEME_DARK || g_twAct == TW_THEME_WHITE))   // open windows and the taskbar switch on this
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000, NULL);
    PostMessageW(g_hwnd, WM_APP_TWEAKDONE, (WPARAM)code, 0);
    return 0;
}

static void tweakOnDone(DWORD code) {
    int act = g_twAct;
    g_twAct = -1;
    InterlockedExchange(&g_busy, 0);
    if (act == TW_REGION) {                  // it has already worked out what to say
        wcscpy(g_twResult, g_regionMsg);
        tweaksRefresh();
        return;
    }
    BOOL vbs = (act == TW_VBS_OFF || act == TW_VBS_ON);
    if (code) swprintf(g_twResult, 320, L"That didn't work (error %lu), so check the status above.", code);
    else if (!tweakIsApplied(act)) wcscpy(g_twResult, L"The import finished, but the setting didn't change.");
    else {
        static const wchar_t* DONE[TW_COUNT] = {
            L"Explore Background disabled.",
            L"Explore Background enabled. Windows only shows it while Windows Spotlight is the desktop background.",
            L"Gallery disabled. Reopen File Explorer windows to see the change.",
            L"Gallery enabled. Reopen File Explorer windows to see the change.",
            L"VBS and Memory Integrity will be off after a restart.",
            L"VBS and Memory Integrity will be on after a restart.",
            L"RDP Blank Password disabled. Accounts with no password can only sign in at this PC.",
            L"RDP Blank Password enabled. Accounts with no password can now sign in over the network.",
            L"Map Network Drives Delay disabled. It applies from the next sign-in.",
            L"Map Network Drives Delay enabled. It applies from the next sign-in.",
            L"WU Driver Update disabled. Windows Update won't offer drivers.",
            L"WU Driver Update enabled. Windows Update can offer drivers.",
            L"Windows is now dark.",
            L"Windows is now white.",
            NULL,
            L"Annoying Notifications disabled. Reopen Settings to see the change.",
            L"Annoying Notifications enabled. Reopen Settings to see the change.",
        };
        wcscpy(g_twResult, DONE[act]);
        if (act <= TW_GALLERY_ON) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    }
    tweaksRefresh();
    if (vbs && !code && tweakIsApplied(act) &&
        showDialog(g_hwnd, L"Restart to finish?", L"This change takes effect when Windows restarts. Save your work first.",
                   NULL, 0, L"Restart now", L"Later", 1)) restartPC();
}

static void tweakClick(int act) {
    const TweakAct* t = &TWEAK[act];
    g_twResult[0] = 0;
    HANDLE helper = NULL;
    if (t->admin) {
        static const wchar_t* TITLE[TW_COUNT] = { L"Disable Explore Background?", L"Enable Explore Background?", NULL, NULL,
                                                  L"Disable VBS?", L"Enable VBS?",
                                                  L"Disable RDP Blank Password?", L"Enable RDP Blank Password?",
                                                  L"Disable Map Network Drives Delay?", L"Enable Map Network Drives Delay?",
                                                  L"Disable WU Driver Update?", L"Enable WU Driver Update?", NULL, NULL };
        static const wchar_t* BODY[TW_COUNT] = {
            L"This removes the Spotlight items from the desktop right-click menu for all users.",
            L"This puts the Spotlight items back in the desktop right-click menu for all users.", NULL, NULL,
            L"This turns off virtualization-based security and Memory Integrity, which lowers this PC's protection against kernel attacks. It needs a restart.",
            L"This turns on virtualization-based security and Memory Integrity. It needs a restart.",
            L"This goes back to the Windows default: accounts with no password can only sign in at this PC.",
            L"This lets accounts with no password sign in over the network, through Remote Desktop and other network sign-ins. It lowers this PC's security.",
            L"This goes back to the Windows default: mapped network drives reconnect at sign-in.",
            L"This stops mapped network drives reconnecting at sign-in, which can shorten sign-in. They connect when first opened.",
            L"This sets three policies so Windows Update and device setup stop fetching drivers and device metadata.",
            L"This removes those three policies, so Windows Update can offer drivers again.", NULL, NULL };
        if (askInApp() && !showDialog(g_hwnd, TITLE[act], BODY[act], NULL, 0, L"Continue", L"Cancel", 1)) return;
        wchar_t args[32]; BOOL cancelled = FALSE;
        swprintf(args, 32, L"--tweak=%d", act);
        if (g_elevated) helper = startHelperDirect(args);
        else if (!startElevatedHelper(args, &helper, &cancelled)) helper = NULL;
        if (!helper) {
            wcscpy(g_twResult, cancelled ? L"Cancelled. Nothing was changed." : L"Couldn't start the administrator helper. Nothing was changed.");
            InvalidateRect(g_hwnd, NULL, FALSE);
            return;
        }
    }
    if (!jobBegin()) {
        if (helper) { WaitForSingleObject(helper, INFINITE); CloseHandle(helper); }
        tweaksRefresh();
        return;
    }
    g_twAct = act;
    startWorker(tweakProc, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Asks for the country, and for the zone if it has several, then does the work on a worker thread.
static void regionClick(void) {
    static wchar_t typed[64];
    wcscpy(typed, L"India");
    DlgExtra ask = { 0 };
    ask.input = typed; ask.inputCch = 64; ask.check = regionCheckInput;
    if (!showDialogEx(g_hwnd, L"Timezone and Region",
                      L"Type a country. Windows' time zone, country or region, and regional format change to that "
                      L"country and its English. Custom date, time and currency formats go back to that format's defaults.",
                      NULL, 0, L"Apply", L"Cancel", 0, &ask)) return;

    if (g_region.nZones > 1) {
        static const wchar_t* names[REGION_MAX_ZONES];
        for (int i = 0; i < g_region.nZones; i++) names[i] = g_region.zones[i].display;
        wchar_t title[180];
        swprintf(title, 180, L"Time zone for %s", g_region.name);
        DlgExtra list = { 0 };
        list.items = names; list.nItems = g_region.nZones; list.pick = &g_region.zone;
        if (!showDialogEx(g_hwnd, title, L"This country has more than one time zone. Pick yours.",
                          NULL, 0, L"Apply", L"Cancel", 0, &list)) return;
    }
    if (!jobBegin()) return;
    g_twResult[0] = 0;
    g_twAct = TW_REGION;
    startWorker(tweakProc, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void tweakButtonClicked(int id) {
    if (id == A_T_REGION) { regionClick(); return; }
    if (id == A_T_ANNOY) { tweakClick(g_twAnnoyOn ? TW_ANNOY_OFF : TW_ANNOY_ON); return; }
    if (id == A_T_EXPLORE) tweakClick(g_twExplore ? TW_EXPLORE_OFF : TW_EXPLORE_ON);
    else if (id == A_T_GALLERY) tweakClick(g_twGallery ? TW_GALLERY_OFF : TW_GALLERY_ON);
    else if (id == A_T_VBS) tweakClick(g_twVbsWanted ? TW_VBS_OFF : TW_VBS_ON);
    else if (id == A_T_RDP) tweakClick(g_twRdp ? TW_RDP_OFF : TW_RDP_ON);
    else if (id == A_T_MAPDELAY) tweakClick(g_twMapDelay ? TW_MAPDELAY_OFF : TW_MAPDELAY_ON);
    else if (id == A_T_WUDRV) tweakClick(g_twWuDrv ? TW_WUDRV_OFF : TW_WUDRV_ON);
    else if (id == A_T_THEME) tweakClick(g_twAppsDark ? TW_THEME_WHITE : TW_THEME_DARK);
}

static const wchar_t* tweakVbsText(void) {
    if (g_twVbsLocked) return L"Locked in UEFI, so this won't last.";
    if (g_twVbsPolicy) return L"Group Policy keeps it on.";
    LONG st = g_twVbsStatus;
    if (st < 0) return L"Checking…";
    if (g_twVbsWanted) return st == 2 ? (g_twVbsHvci ? L"Enabled: Running, with Memory Integrity" : L"Enabled: Running")
                                      : L"Enabled: Restart to Finish";
    return st == 2 ? L"Disabled: Restart to Finish" : L"Disabled";
}

enum { DOT_RED, DOT_GREEN, DOT_DARK, DOT_WHITE, DOT_NONE };

// The dot and name sit at the button's left edge, and a click switches to the other state.
static void tweakRow(int id, int x1, int x2, int y, const wchar_t* label, int dotKind, const wchar_t* state, BOOL shield) {
    int bh = dp(32), bw = dp(240), en = g_busy == 0;
    RECT rc = { x1, y, x1 + bw, y + bh };
    if (en) aioRegister(id, rc, NULL, 0, FALSE);
    drawButtonFace(rc, dp(4), FALSE, anim(ANIM_AIO + id, g_hotBtn == BTN_WIDGET + id && en), en ? 1.0 : 0.4);

    int d = dp(10), dx = x1 + dp(12), dy = y + (bh - d) / 2;
    double dotA = en ? 1.0 : 0.4;
    if (dotKind == DOT_NONE) {
        dx = x1 + dp(12) - d - dp(10);    // no dot, so the name starts at the button's edge
    } else if (dotKind >= DOT_DARK) {
        // a rim keeps the dark dot visible on a dark button, and the white one on a light button
        int rim = g_dark ? 165 : 130, in = dotKind == DOT_DARK ? 16 : 255, w = dp(1);
        fillRoundRect(dx, dy, dx + d, dy + d, d / 2.0, rim, rim, rim, dotA);
        fillRoundRect(dx + w, dy + w, dx + d - w, dy + d - w, (d - 2 * w) / 2.0, in, in, in, dotA);
    } else {
        Col dot = dotKind == DOT_GREEN ? (g_dark ? (Col){ 76,194,88,1 } : (Col){ 16,137,62,1 })
                                       : (g_dark ? (Col){ 232,70,60,1 } : (Col){ 196,43,28,1 });
        fillRoundRect(dx, dy, dx + d, dy + d, d / 2.0, dot.r, dot.g, dot.b, dotA);
    }

    RECT t = { dx + d + dp(10), y, rc.right - dp(8), y + bh };
    drawText(g_fBody, label, t, en ? textRGB() : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (shield) drawShield(rc.right - dp(24), y + (bh - dp(16)) / 2, dp(16));

    // one line beside the button, or two when a narrow window makes it wrap
    RECT r = { rc.right + dp(14), y, x2, y + bh };
    if (textWidth(g_fSmall, state) <= r.right - r.left)
        drawText(g_fSmall, state, r, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    else
        drawText(g_fSmall, state, r, mutedRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}

static void drawTweaks(int cL, int cR) {
    int pad = dp(16), top = pad, cardH = dp(176);
    Col sf = surfaceCol();
    fillRoundRect(cL, top, cR, top + cardH, dp(8), sf.r, sf.g, sf.b, sf.a);
    drawPageHeader(0xE713, cL, cR, top, L"Misc Tweaks", L"Small one-click changes.");

    int x1 = cL + dp(20), x2 = cR - dp(20), y = top + dp(92), bh = dp(36);
    aioButton(A_T_TERMINAL, x1, y, x1 + dp(240), y + bh, L"Terminal Settings Restore", TRUE, TRUE, FALSE);
    y += bh + dp(10);
    RECT note = { x1, y, x2, y + dp(36) };
    if (g_tweakStatus[0]) drawText(g_fSmall, g_tweakStatus, note, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    else drawText(g_fSmall, L"Replaces Windows Terminal's settings.json with the copy built into this app. "
                            L"It acts straight away, and the current file isn't kept.",
                  note, mutedRGB(), DT_LEFT | DT_WORDBREAK);

    int ct = top + cardH + dp(12);
    fillRoundRect(cL, ct, cR, g_bh - pad, dp(8), sf.r, sf.g, sf.b, sf.a);   // ends level with the rail
    y = ct + dp(14);
    RECT head = { x1, y, x2, y + dp(24) };
    drawText(g_fBodyBold, L"Enable / Disable", head, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += dp(28);
    BOOL shield = !g_elevated;
    tweakRow(A_T_EXPLORE, x1, x2, y, L"Explore Background", g_twExplore,
             g_twExplore ? L"Enabled: In the Desktop Context Menu" : L"Disabled: Not in the Desktop Context Menu", shield);
    y += dp(36);
    tweakRow(A_T_GALLERY, x1, x2, y, L"Gallery", g_twGallery,
             g_twGallery ? L"Enabled: In File Explorer" : L"Disabled: Not in File Explorer", FALSE);
    y += dp(36);
    tweakRow(A_T_VBS, x1, x2, y, L"VBS", g_twVbsWanted, tweakVbsText(), shield);
    y += dp(36);
    tweakRow(A_T_RDP, x1, x2, y, L"RDP Blank Password", g_twRdp,
             g_twRdp ? L"Enabled: Blank Passwords Allowed Remotely" : L"Disabled: Windows Default", shield);
    y += dp(36);
    tweakRow(A_T_MAPDELAY, x1, x2, y, L"Map Network Drives Delay", g_twMapDelay,
             g_twMapDelay ? L"Enabled: Drives Connect when First Opened" : L"Disabled: Drives Reconnect at Sign-in", shield);
    y += dp(36);
    tweakRow(A_T_WUDRV, x1, x2, y, L"WU Driver Update", g_twWuDrv,
             !g_twWuDrv ? L"Disabled: No Drivers from Windows Update"
                        : g_twWuLimited ? L"Enabled: Other Driver Policies Are Set" : L"Enabled: Windows Update Can Offer Drivers", shield);
    y += dp(36);
    tweakRow(A_T_THEME, x1, x2, y, L"Windows Theme", g_twAppsDark ? DOT_DARK : DOT_WHITE,
             g_twAppsDark != g_twSystemDark ? (g_twAppsDark ? L"Mixed: apps dark, Windows white." : L"Mixed: apps white, Windows dark.")
                                            : g_twAppsDark ? L"Dark. Click for white." : L"White. Click for dark.", FALSE);
    y += dp(36);
    tweakRow(A_T_REGION, x1, x2, y, L"Timezone and Region", DOT_NONE, g_twRegionNow, FALSE);
    y += dp(36);
    wchar_t annoy[48];
    if (g_twAnnoyOn == 0 || g_twAnnoyOn == 3) wcscpy(annoy, g_twAnnoyOn ? L"Enabled: All Three Are On" : L"Disabled: All Three Are Off");
    else swprintf(annoy, 48, L"Enabled: %d of the Three Still On", g_twAnnoyOn);
    tweakRow(A_T_ANNOY, x1, x2, y, L"Annoying Notifications", g_twAnnoyOn > 0, annoy, FALSE);
    y += dp(35);
    RECT what = { x1, y, x2, y + dp(34) };
    drawText(g_fSmall, L"In Settings > System > Notifications > Additional settings: welcome experience "
                       L"after updates, setup suggestions, and Windows tips.",
             what, mutedRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    y += dp(35);
    RECT res = { x1, y, x2, y + dp(36) };
    drawText(g_fSmall, g_twAct >= 0 ? L"Working…" : g_twResult, res, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}
