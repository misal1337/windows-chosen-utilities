// ---------- HWiNFO: the gear on its row ----------
// A small box with HWiNFO's startup choice (sensors only, minimized to the notification area) and its own on-screen
// display (OSD, not the RTSS one): six readings to tick, at most 5 (the free HWiNFO's limit), in an order the arrows
// change, plus a reset of the sensor settings to what a fresh install has.
//
// Where HWiNFO keeps them (found on HWiNFO 8.60):
//  - startup: HWiNFO64.INI beside HWiNFO64.EXE (Program Files, so administrator rights), [Settings] SensorsOnly,
//    MinimalizeSensors, MinimalizeMainWnd, ShowWelcomeAndProgress. Registry values of those names are ignored.
//  - OSD: HKCU\Software\HWiNFO64\Sensors\<sensor id>_<instance>\<Type><reading id>, values OsdInclude (1 = shown) and
//    OsdItem (its row, 1 first), and Sensors\OsdEnabled. A reading's key is its type name (Temp, Power, Other...) and
//    the low part of its reading id in decimal: "Framerate Displayed (avg)", id 0x0800000A in the PresentMon sensor
//    F0000FF0, is F0000FF0_0\Other10. Core temperatures are the DTS sensor's group row, Group1.
// Sensor ids and instances depend on the hardware (this PC's GPU is E0002000_20), so the helper reads them from
// HWiNFO's shared memory (SensorsSM=1, which it turns on), and only falls back to the ids seen on this PC.
// HWiNFO runs as administrator and so reads the HKCU of the account it was elevated as: the elevated helper writes there.

#define HW_OSD_N   6
#define HW_OSD_MAX 5
#define HW_SM_NAME L"Global\\HWiNFO_SENS_SM2"

typedef struct {
    const wchar_t* label;        // in the box
    DWORD sensor;                // the sensor id seen on this PC, for when shared memory can't be read
    const wchar_t* reading;      // its key there
    const char* sm[4];           // labels to look for in shared memory, best first (NULL: the core temperatures group)
} HwOsdDef;
static const HwOsdDef HW_OSD[HW_OSD_N] = {
    { L"Core Temps (CPU)",          0xF0000400, L"Group1",  { NULL } },
    { L"CPU Package Power",         0xF0002A00, L"Power0",  { "CPU Package Power", NULL } },
    { L"GPU Temp",                  0xE0002000, L"Temp0",   { "GPU Temperature", NULL } },
    { L"GPU Rail Power",            0xE0002000, L"Power0",  { "GPU Power", "GPU ASIC Power", "Total Board Power (TBP)", "GPU PPT" } },
    { L"Framerate Displayed",       0xF0000FF0, L"Other10", { "Framerate Displayed (avg)", NULL } },
    { L"Framerate Displayed (1%)",  0xF0000FF0, L"Other12", { "Framerate Displayed (1%)", NULL } },
};
#define HW_KEY  L"Software\\HWiNFO64"
#define HW_SENS L"Software\\HWiNFO64\\Sensors"

static BOOL hwIniPath(wchar_t* ini /* MAX_PATH */) {
    wchar_t ver[64], exe[MAX_PATH];
    if (!appsFindInstalledEx(&APPS[APP_HWINFO], ver, 64, exe) || !exe[0]) return FALSE;
    lstrcpynW(ini, exe, MAX_PATH);
    PathRemoveFileSpecW(ini);
    return PathAppendW(ini, L"HWiNFO64.INI");
}
static BOOL hwTrayOn(void) {
    wchar_t ini[MAX_PATH];
    return hwIniPath(ini) && GetPrivateProfileIntW(L"Settings", L"SensorsOnly", 0, ini) == 1 &&
           GetPrivateProfileIntW(L"Settings", L"MinimalizeSensors", 0, ini) == 1;
}

// Dark mode: DarkMode=1 with Theme=1 is HWiNFO's default grey dark look, DarkMode=0 its white one (seen on 8.60).
// Its black theme isn't offered. A fresh install has no DarkMode line and starts dark, so no line counts as on.
static BOOL hwDarkOn(void) {
    wchar_t ini[MAX_PATH];
    return !hwIniPath(ini) || GetPrivateProfileIntW(L"Settings", L"DarkMode", 1, ini) != 0;
}
static void hwIniDark(const wchar_t* ini, BOOL on) {
    WritePrivateProfileStringW(L"Settings", L"DarkMode", on ? L"1" : L"0", ini);
    if (on) WritePrivateProfileStringW(L"Settings", L"Theme", L"1", ini);
}

// The INI settings every install gets: shared memory on (so the gear can find the sensors) and no beta versions offered
static void hwIniBase(const wchar_t* ini) {
    WritePrivateProfileStringW(L"Settings", L"SensorsSM", L"1", ini);
    WritePrivateProfileStringW(L"Settings", L"AutoUpdateBetaDisable", L"1", ini);
}
static void hwIniTray(const wchar_t* ini, BOOL on) {
    static const wchar_t* KEYS[] = { L"SensorsOnly", L"MinimalizeSensors", L"MinimalizeMainWnd" };
    for (int i = 0; i < 3; i++) WritePrivateProfileStringW(L"Settings", KEYS[i], on ? L"1" : NULL, ini);
    WritePrivateProfileStringW(L"Settings", L"ShowWelcomeAndProgress", on ? L"0" : NULL, ini);   // removed: HWiNFO's default
}

// ---------- shared memory (the layout HWiNFO documents for its shared memory, version 2) ----------
#pragma pack(push, 1)
typedef struct { DWORD sig, ver, rev; long long poll; DWORD offS, sizeS, numS, offR, sizeR, numR; } HwSmHdr;
typedef struct { DWORD id, inst; char nameOrig[128], nameUser[128]; } HwSmSensor;
typedef struct { DWORD type, sensIdx, readId; char labelOrig[128], labelUser[128], unit[16]; double v, vmin, vmax, vavg; } HwSmReading;
#pragma pack(pop)
static const wchar_t* HW_TYPES[] = { L"None", L"Temp", L"Volt", L"Fan", L"Current", L"Power", L"Clock", L"Usage", L"Other" };

// Finds each reading's key from HWiNFO's shared memory. keys[i] stays empty for one this PC doesn't have.
// Returns how many sensors HWiNFO listed (0: no shared memory).
static int hwSmKeys(wchar_t keys[HW_OSD_N][64]) {
    for (int i = 0; i < HW_OSD_N; i++) keys[i][0] = 0;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, HW_SM_NAME);
    if (!m) return 0;
    BYTE* p = (BYTE*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    int ns = 0;
    if (p) {
        HwSmHdr* h = (HwSmHdr*)p;
        MEMORY_BASIC_INFORMATION mb;
        SIZE_T size = VirtualQuery(p, &mb, sizeof(mb)) ? mb.RegionSize : 0;
        BOOL sane = h->sig == 0x53695748 && h->sizeS >= sizeof(HwSmSensor) && h->sizeR >= sizeof(HwSmReading) &&
                    (unsigned long long)h->offS + (unsigned long long)h->sizeS * h->numS <= size &&
                    (unsigned long long)h->offR + (unsigned long long)h->sizeR * h->numR <= size;
        if (sane) {
            ns = (int)h->numS;
            // the GPU: a discrete one ("dGPU [#1]: ...") before one built into the CPU
            int gpu = -1;
            for (DWORD i = 0; i < h->numR; i++) {
                HwSmReading* r = (HwSmReading*)(p + h->offR + i * h->sizeR);
                if (r->type != 1 || strcmp(r->labelOrig, "GPU Temperature") || r->sensIdx >= h->numS) continue;
                HwSmSensor* s = (HwSmSensor*)(p + h->offS + r->sensIdx * h->sizeS);
                if (gpu < 0 || !strncmp(s->nameOrig, "dGPU", 4)) gpu = (int)r->sensIdx;
            }
            // the core temperatures: the sensor with the most per-core temperatures ("P-core 0", "Core 0", "Core0 (CCD1)")
            int coreS = -1, coreN = 0;
            for (DWORD s = 0; s < h->numS; s++) {
                int n = 0;
                for (DWORD i = 0; i < h->numR; i++) {
                    HwSmReading* r = (HwSmReading*)(p + h->offR + i * h->sizeR);
                    if (r->sensIdx != s || r->type != 1 || strstr(r->labelOrig, "Distance")) continue;
                    const char* c = strstr(r->labelOrig, "ore");     // Core / core
                    if (c && c > r->labelOrig && (c[-1] == 'C' || c[-1] == 'c') && (isdigit((unsigned char)c[3]) || (c[3] == ' ' && isdigit((unsigned char)c[4])))) n++;
                }
                if (n > coreN) { coreN = n; coreS = (int)s; }
            }
            if (coreS >= 0) {
                HwSmSensor* s = (HwSmSensor*)(p + h->offS + coreS * h->sizeS);
                swprintf(keys[0], 64, L"%08lX_%lu\\Group1", s->id, s->inst);
            }
            for (int d = 1; d < HW_OSD_N; d++) {
                BOOL isGpu = HW_OSD[d].sensor == 0xE0002000;
                for (int k = 0; k < 4 && HW_OSD[d].sm[k] && !keys[d][0]; k++)
                    for (DWORD i = 0; i < h->numR && !keys[d][0]; i++) {
                        HwSmReading* r = (HwSmReading*)(p + h->offR + i * h->sizeR);
                        if (r->sensIdx >= h->numS || r->type > 8 || strcmp(r->labelOrig, HW_OSD[d].sm[k])) continue;
                        if (isGpu && gpu >= 0 && (int)r->sensIdx != gpu) continue;
                        HwSmSensor* s = (HwSmSensor*)(p + h->offS + r->sensIdx * h->sizeS);
                        swprintf(keys[d], 64, L"%08lX_%lu\\%s%lu", s->id, s->inst, HW_TYPES[r->type], r->readId & 0xFFFFFF);
                    }
            }
        }
        UnmapViewOfFile(p);
    }
    CloseHandle(m);
    return ns;
}

// Without shared memory: the ids seen on this PC, at an instance HWiNFO already has a key for (else 0)
static void hwFallbackKey(int d, wchar_t* key /* 64 */) {
    wchar_t pre[16], name[64];
    swprintf(pre, 16, L"%08lX_", HW_OSD[d].sensor);
    swprintf(key, 64, L"%s0\\%s", pre, HW_OSD[d].reading);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, HW_SENS, 0, KEY_ENUMERATE_SUB_KEYS, &k)) return;
    BOOL shown = FALSE, any = FALSE;      // an instance whose reading is on the OSD wins (old instances keep their keys)
    for (DWORD i = 0; !shown; i++) {
        DWORD n = 64;
        if (RegEnumKeyExW(k, i, name, &n, NULL, NULL, NULL, NULL)) break;
        if (_wcsnicmp(name, pre, 9)) continue;
        wchar_t sub[140]; HKEY r;
        swprintf(sub, 140, L"%s\\%s", name, HW_OSD[d].reading);
        if (RegOpenKeyExW(k, sub, 0, KEY_QUERY_VALUE, &r)) continue;
        DWORD v = 0, cb = sizeof(v);
        shown = !RegQueryValueExW(r, L"OsdInclude", NULL, NULL, (BYTE*)&v, &cb) && v == 1;
        RegCloseKey(r);
        if (shown || !any) { swprintf(key, 64, L"%s", sub); any = TRUE; }
    }
    RegCloseKey(k);
}

static DWORD hwRegDword(const wchar_t* sub, const wchar_t* name, DWORD def) {
    wchar_t path[160]; DWORD v = def, cb = sizeof(v);
    swprintf(path, 160, HW_SENS L"\\%s", sub);
    if (RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, NULL, &v, &cb)) v = def;
    return v;
}

#define HW_SIZE_DEF 20                // OsdFontSizeText when a reading has none yet
#define HW_SIZE_MIN 8
#define HW_SIZE_MAX 48

// What the OSD shows now, for the box: per reading, its row (1...) or 0 when it isn't shown, and its text size
static void hwOsdNow(int item[HW_OSD_N], int size[HW_OSD_N]) {
    wchar_t keys[HW_OSD_N][64];
    BOOL sm = hwSmKeys(keys) > 0;
    for (int d = 0; d < HW_OSD_N; d++) {
        if (!sm || !keys[d][0]) hwFallbackKey(d, keys[d]);
        item[d] = hwRegDword(keys[d], L"OsdInclude", 0) == 1 ? (int)hwRegDword(keys[d], L"OsdItem", 99) : 0;
        if (item[d] < 0) item[d] = 99;
        size[d] = (int)hwRegDword(keys[d], L"OsdFontSizeText", HW_SIZE_DEF);
        if (size[d] < HW_SIZE_MIN || size[d] > HW_SIZE_MAX) size[d] = HW_SIZE_DEF;
    }
}

// ---------- the helper (elevated: HWiNFO's folder, and the HKCU HWiNFO reads) ----------
static void hwKillAll(void) {
    DWORD pids[16]; int n = appsPids(L"HWiNFO64.exe", pids, 16);
    for (int i = 0; i < n; i++) {
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pids[i]);
        if (p) { TerminateProcess(p, 0); WaitForSingleObject(p, 5000); CloseHandle(p); }
    }
}
static BOOL hwStart(void) {
    wchar_t ver[64], exe[MAX_PATH], dir[MAX_PATH];
    if (!appsFindInstalledEx(&APPS[APP_HWINFO], ver, 64, exe) || !exe[0]) return FALSE;
    lstrcpynW(dir, exe, MAX_PATH); PathRemoveFileSpecW(dir);
    // HWiNFO's manifest asks for administrator rights, which CreateProcess refuses (740) even from this elevated
    // helper; ShellExecute starts it, with no prompt since the helper already has them
    SHELLEXECUTEINFOW se = { sizeof(se) };
    se.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    se.lpVerb = L"open"; se.lpFile = exe; se.lpDirectory = dir; se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) { appsSay(L"Couldn't start HWiNFO (error %lu).", GetLastError()); return FALSE; }
    return TRUE;
}

// --hwinfo-osd=reset, or --hwinfo-osd=<tray 0|1><dark 0|1>:<reading>.<size>,<reading>.<size>... (the ticked ones, in order)
static int hwOsdHelper(const wchar_t* cmd) {
    g_appsNotify = cmdNotify(cmd);
    const wchar_t* a = cmd + 13;
    BOOL reset = !wcsncmp(a, L"reset", 5);
    int tray = reset ? -1 : (a[0] == L'1'), dark = reset ? -1 : (a[1] == L'1'), pick[HW_OSD_N], psize[HW_OSD_N], np = 0;
    if (!reset) {
        for (const wchar_t* q = wcschr(a, L':'); q && *q && np < HW_OSD_MAX; ) {
            q++;
            if (*q < L'0' || *q > L'9') break;
            int d = (int)wcstol(q, (wchar_t**)&q, 10), sz = HW_SIZE_DEF;
            if (*q == L'.') sz = (int)wcstol(q + 1, (wchar_t**)&q, 10);
            if (sz < HW_SIZE_MIN || sz > HW_SIZE_MAX) sz = HW_SIZE_DEF;
            if (d >= 0 && d < HW_OSD_N) { pick[np] = d; psize[np] = sz; np++; }
        }
    }
    wchar_t ini[MAX_PATH];
    if (!hwIniPath(ini)) { appsSay(L"Couldn't find HWiNFO's folder."); return ERROR_FILE_NOT_FOUND; }
    DWORD one[1];
    BOOL wasRunning = appsPids(L"HWiNFO64.exe", one, 1) > 0;
    hwIniBase(ini);
    if (dark >= 0) {
        hwIniDark(ini, dark);
        appsSay(dark ? L"HWiNFO uses its dark mode (grey)." : L"HWiNFO uses its light mode (white).");
    }
    if (tray >= 0) {
        hwIniTray(ini, tray);
        appsSay(tray ? L"HWiNFO starts with its sensors only, minimized to the notification area (HWiNFO64.INI)."
                     : L"HWiNFO starts normally again (HWiNFO64.INI).");
    }

    wchar_t keys[HW_OSD_N][64];
    if (!reset) {
        int ns = hwSmKeys(keys);
        BOOL started = FALSE;
        if (!ns) {                          // read the sensors from a HWiNFO started for that (shared memory is on now)
            if (wasRunning) { appsSay(L"Closing HWiNFO to turn on its shared memory…"); hwKillAll(); }
            appsSay(L"Starting HWiNFO to find this PC's sensors…");
            started = hwStart();
            for (int t = 0, last = -1, same = 0; started && t < 90; t++) {     // up to 45 s, until the list stops growing
                Sleep(500);
                HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, HW_SM_NAME);
                if (!m) continue;
                CloseHandle(m);
                int n = hwSmKeys(keys);
                int got = 0; for (int d = 0; d < HW_OSD_N; d++) if (keys[d][0]) got++;
                if (n > 0 && n * 10 + got == last) { if (++same >= 6) break; } else same = 0;
                last = n * 10 + got;
            }
            ns = hwSmKeys(keys);
        }
        if (ns) appsSay(L"Found HWiNFO's sensors (%d) in its shared memory.", ns);
        else appsSay(L"HWiNFO's shared memory isn't readable, so the sensor ids seen on the developer's PC are used.");
        for (int d = 0; d < HW_OSD_N; d++) if (!keys[d][0]) {
            hwFallbackKey(d, keys[d]);
            if (ns) appsSay(L"  %s: not listed by HWiNFO on this PC; using %s", HW_OSD[d].label, keys[d]);
        }
        (void)started;
    }

    appsSay(L"Closing HWiNFO while its settings are written…");
    hwKillAll();
    HKEY s;
    if (reset) {
        LSTATUS e = RegDeleteTreeW(HKEY_CURRENT_USER, HW_SENS);
        RegDeleteKeyW(HKEY_CURRENT_USER, HW_SENS);
        appsSay(e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND ? L"Sensor settings reset: HKCU\\Software\\HWiNFO64\\Sensors removed, as on a fresh install."
                                                               : L"Couldn't reset the sensor settings (error %ld).", (long)e);
    } else if (!RegCreateKeyExW(HKEY_CURRENT_USER, HW_SENS, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &s, NULL)) {
        // every reading shown on the OSD now is taken off it, so the box's choice is the whole OSD
        wchar_t name[64], rd[64];
        for (DWORD i = 0; ; i++) {
            DWORD n = 64;
            if (RegEnumKeyExW(s, i, name, &n, NULL, NULL, NULL, NULL)) break;
            HKEY sk;
            if (RegOpenKeyExW(s, name, 0, KEY_ENUMERATE_SUB_KEYS, &sk)) continue;
            for (DWORD j = 0; ; j++) {
                DWORD m = 64;
                if (RegEnumKeyExW(sk, j, rd, &m, NULL, NULL, NULL, NULL)) break;
                HKEY rk; DWORD v = 0, cb = sizeof(v);
                if (RegOpenKeyExW(sk, rd, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &rk)) continue;
                if (!RegQueryValueExW(rk, L"OsdInclude", NULL, NULL, (BYTE*)&v, &cb) && v) { v = 0; RegSetValueExW(rk, L"OsdInclude", 0, REG_DWORD, (BYTE*)&v, sizeof(v)); }
                RegCloseKey(rk);
            }
            RegCloseKey(sk);
        }
        for (int i = 0; i < np; i++) {
            HKEY rk; DWORD one = 1, row = (DWORD)(i + 1), v, cb;
            if (RegCreateKeyExW(s, keys[pick[i]], 0, NULL, 0, KEY_ALL_ACCESS, NULL, &rk, NULL)) { appsSay(L"Couldn't write %s.", keys[pick[i]]); continue; }
            RegSetValueExW(rk, L"OsdInclude", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            RegSetValueExW(rk, L"OsdItem", 0, REG_DWORD, (BYTE*)&row, sizeof(row));
            DWORD fs = (DWORD)psize[i];
            RegSetValueExW(rk, L"OsdFontSizeText", 0, REG_DWORD, (BYTE*)&fs, sizeof(fs));
            cb = sizeof(v); if (RegQueryValueExW(rk, L"OsdShowLabel", NULL, NULL, (BYTE*)&v, &cb)) RegSetValueExW(rk, L"OsdShowLabel", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            cb = sizeof(v); if (RegQueryValueExW(rk, L"OsdShowUnit", NULL, NULL, (BYTE*)&v, &cb)) RegSetValueExW(rk, L"OsdShowUnit", 0, REG_DWORD, (BYTE*)&one, sizeof(one));
            RegCloseKey(rk);
            appsSay(L"  OSD row %d: %s, size %d (%s)", i + 1, HW_OSD[pick[i]].label, psize[i], keys[pick[i]]);
        }
        DWORD en = np > 0;
        RegSetValueExW(s, L"OsdEnabled", 0, REG_DWORD, (BYTE*)&en, sizeof(en));
        RegCloseKey(s);
        appsSay(np ? L"OSD set: %d readings." : L"OSD turned off: nothing ticked.", np);
    }
    if (wasRunning) { appsSay(hwStart() ? L"HWiNFO started again." : L"Couldn't start HWiNFO again; start it from Start."); }
    return 0;
}

// ---------- the window's side ----------
static BOOL g_hwOsdJob = FALSE;

static DWORD WINAPI hwOsdProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    g_appsCode = waitExitCode(helper);
    PostMessageW(g_hwnd, WM_APP_APPSDONE, 0, 0);
    return 0;
}
// appsOnDone's part for this job: TRUE when it was this one
static BOOL hwOsdOnDone(void) {
    if (!g_hwOsdJob) return FALSE;
    g_hwOsdJob = FALSE;
    InterlockedExchange(&g_logOpen, 0);
    InterlockedExchange(&g_busy, 0);
    swprintf(g_appsMsg, 400, g_appsCode ? L"HWiNFO's settings couldn't be changed (code %lu). See the lines above." : L"HWiNFO's settings are changed.", g_appsCode);
    appsLogAdd(g_appsMsg, FALSE);
    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}

static void hwRunHelper(const wchar_t* args, const wchar_t* head) {
    HANDLE helper = NULL; BOOL cancelled = FALSE;
    if (g_elevated) helper = startHelperDirect(args);
    else if (!startElevatedHelper(args, &helper, &cancelled)) helper = NULL;
    if (g_appsLogN) appsLogAdd(L"", FALSE);
    appsLogAdd(head, FALSE);
    if (!helper) {
        appsLogAdd(cancelled ? L"Cancelled. Nothing was changed." : L"Couldn't start the administrator helper. Nothing was changed.", FALSE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (!jobBegin()) { WaitForSingleObject(helper, INFINITE); CloseHandle(helper); return; }
    g_hwOsdJob = TRUE;
    InterlockedExchange(&g_logOpen, 1);
    startWorker(hwOsdProc, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void appsHwinfoSettingsClick(void) {
    if (g_busy != 0 || !g_appsVer[APP_HWINFO][0]) return;
    // the rows: those on the OSD now in their order, then the rest in the list's order
    int now[HW_OSD_N], order[HW_OSD_N], sizeOf[HW_OSD_N], sizes[HW_OSD_N];
    BOOL checks[HW_OSD_N];
    const wchar_t* items[HW_OSD_N];
    hwOsdNow(now, sizeOf);
    int n = 0;
    for (int d = 0; d < HW_OSD_N; d++) if (now[d] > 0) order[n++] = d;
    for (int i = 1; i < n; i++)                     // by their OSD row (a stable insertion sort)
        for (int k = i; k > 0 && now[order[k]] < now[order[k - 1]]; k--) { int t = order[k]; order[k] = order[k - 1]; order[k - 1] = t; }
    int shown = n;
    for (int d = 0; d < HW_OSD_N; d++) if (now[d] == 0) order[n++] = d;
    for (int i = 0; i < HW_OSD_N; i++) checks[i] = i < shown;
    int on = 0;
    for (int i = 0; i < HW_OSD_N; i++) {
        if (checks[i] && ++on > HW_OSD_MAX) checks[i] = FALSE;
        items[i] = HW_OSD[order[i]].label;
        sizes[i] = sizeOf[order[i]];
    }
    BOOL tray = hwTrayOn(), dark = hwDarkOn();
    int pick = 0;
    DlgExtra x = { 0 };
    x.items = items; x.nItems = HW_OSD_N; x.pick = &pick; x.checks = checks;
    x.maxChecks = HW_OSD_MAX; x.reorder = TRUE; x.order = order;
    x.toggleText = L"Hide sensors and open only to the notification area"; x.toggle = &tray;
    x.toggleText2 = L"Dark mode (grey); unticked: light mode (white)"; x.toggle2 = &dark;
    x.resetText = L"Reset sensors";
    x.sizes = sizes; x.sizeMin = HW_SIZE_MIN; x.sizeMax = HW_SIZE_MAX;
    int r = showDialogEx(g_hwnd, L"HWiNFO settings",
                         L"On-screen display (HWiNFO's own, not RTSS): tick up to 5, the free version's limit; - and + set a reading's text size, "
                         L"the arrows its place. HWiNFO is closed while this is saved and started again if it was running.",
                         NULL, 0, L"Apply", L"Cancel", 0, &x);
    if (r == 2) {
        if (askInApp() && !showDialog(g_hwnd, L"Reset HWiNFO's sensors?",
                L"This removes all of HWiNFO's sensor settings (HKEY_CURRENT_USER\\Software\\HWiNFO64\\Sensors): the sensor window's "
                L"layout and the on-screen display, as on a fresh install.", NULL, 0, L"Reset", L"Cancel", 1)) return;
        hwRunHelper(L"--hwinfo-osd=reset", L"> Reset HWiNFO's sensor settings");
        return;
    }
    if (r != 1) return;
    wchar_t args[96], *p = args;
    p += swprintf(p, 96, L"--hwinfo-osd=%d%d:", tray ? 1 : 0, dark ? 1 : 0);
    BOOL first = TRUE;
    for (int i = 0; i < HW_OSD_N; i++) if (checks[i]) { p += swprintf(p, 96 - (p - args), first ? L"%d.%d" : L",%d.%d", order[i], sizes[i]); first = FALSE; }
    hwRunHelper(args, L"> HWiNFO settings");
}
