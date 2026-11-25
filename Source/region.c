// Timezone and Region: finds a country by name, then sets Windows' time zone, country or region, and regional
// format to that country's English. All the country data comes from Windows: its geo list for names, its ICU
// library for each country's time zones, and the registry for how those zones are displayed.

#define REGION_MAX_ZONES 32
#define TZ_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"
#define INTL_KEY L"Control Panel\\International"

typedef struct { wchar_t id[128]; wchar_t display[160]; LONG bias; } TzZone;
typedef struct {
    wchar_t iso[8], name[128], locale[16];
    BOOL realLocale;                       // Windows has real data for this English, not just generic values
    TzZone zones[REGION_MAX_ZONES];
    int nZones, zone;
} RegionPick;

static RegionPick g_region;
static wchar_t g_regionMsg[320];

typedef int (WINAPI *GeoEnumFn)(GEOCLASS, GEO_ENUMNAMEPROC, LPARAM);
typedef int (WINAPI *GeoInfoFn)(PCWSTR, GEOTYPE, PWSTR, int, LANGID);
typedef BOOL (WINAPI *GeoSetFn)(PWSTR);

static FARPROC kernelFn(const char* name) { return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name); }

static BOOL geoFriendlyName(const wchar_t* iso, wchar_t* out, int cch) {
    GeoInfoFn info = (GeoInfoFn)kernelFn("GetGeoInfoEx");
    out[0] = 0;
    return info && info(iso, 8 /*GEO_FRIENDLYNAME*/, out, cch, 0) > 0 && out[0];
}

// ---------- finding the country ----------
#define GEO_MAX_MATCH 6
static struct {
    const wchar_t* query;
    wchar_t exact[8];
    wchar_t prefix[GEO_MAX_MATCH][128], inside[GEO_MAX_MATCH][128];
    wchar_t prefixIso[8], insideIso[8];
    int nPrefix, nInside;
} g_geoFind;

static const wchar_t* containsNoCase(const wchar_t* hay, const wchar_t* needle) {
    size_t n = wcslen(needle);
    for (; *hay; hay++) if (!_wcsnicmp(hay, needle, n)) return hay;
    return NULL;
}

static BOOL CALLBACK geoFindProc(PWSTR iso, LPARAM l) {
    (void)l;
    if (wcslen(iso) != 2) return TRUE;
    wchar_t name[128];
    if (!geoFriendlyName(iso, name, 128)) return TRUE;
    if (!_wcsicmp(iso, g_geoFind.query) || !_wcsicmp(name, g_geoFind.query)) { wcscpy(g_geoFind.exact, iso); return FALSE; }
    const wchar_t* at = containsNoCase(name, g_geoFind.query);
    if (at == name) {
        if (g_geoFind.nPrefix < GEO_MAX_MATCH) wcscpy(g_geoFind.prefix[g_geoFind.nPrefix], name);
        g_geoFind.nPrefix++; wcscpy(g_geoFind.prefixIso, iso);
    } else if (at) {
        if (g_geoFind.nInside < GEO_MAX_MATCH) wcscpy(g_geoFind.inside[g_geoFind.nInside], name);
        g_geoFind.nInside++; wcscpy(g_geoFind.insideIso, iso);
    }
    return TRUE;
}

// Everyday names that differ from the ones Windows uses.
static const struct { const wchar_t* name; const wchar_t* iso; } GEO_ALIAS[] = {
    { L"usa", L"US" }, { L"america", L"US" }, { L"united states of america", L"US" },
    { L"uk", L"GB" }, { L"britain", L"GB" }, { L"great britain", L"GB" }, { L"england", L"GB" }, { L"scotland", L"GB" }, { L"wales", L"GB" },
    { L"uae", L"AE" }, { L"emirates", L"AE" }, { L"south korea", L"KR" }, { L"holland", L"NL" },
    { L"czech republic", L"CZ" }, { L"burma", L"MM" }, { L"russian federation", L"RU" },
};

// Takes what was typed. On failure, err says what to do next.
static BOOL regionFindCountry(const wchar_t* typed, wchar_t* iso, wchar_t* err, int cch) {
    wchar_t q[64];
    while (*typed == L' ') typed++;
    wcsncpy(q, typed, 63); q[63] = 0;
    for (int n = (int)wcslen(q); n > 0 && q[n - 1] == L' '; n--) q[n - 1] = 0;
    if (!q[0]) { wcsncpy(err, L"Type a country name.", cch); return FALSE; }

    for (size_t i = 0; i < sizeof(GEO_ALIAS) / sizeof(GEO_ALIAS[0]); i++)
        if (!_wcsicmp(q, GEO_ALIAS[i].name)) { wcscpy(iso, GEO_ALIAS[i].iso); return TRUE; }

    GeoEnumFn en = (GeoEnumFn)kernelFn("EnumSystemGeoNames");
    if (!en) { wcsncpy(err, L"This version of Windows can't list countries.", cch); return FALSE; }
    memset(&g_geoFind, 0, sizeof(g_geoFind));
    g_geoFind.query = q;
    en(GEOCLASS_NATION, geoFindProc, 0);

    if (g_geoFind.exact[0]) { wcscpy(iso, g_geoFind.exact); return TRUE; }
    if (g_geoFind.nPrefix == 1) { wcscpy(iso, g_geoFind.prefixIso); return TRUE; }
    if (!g_geoFind.nPrefix && g_geoFind.nInside == 1) { wcscpy(iso, g_geoFind.insideIso); return TRUE; }

    int n = g_geoFind.nPrefix ? g_geoFind.nPrefix : g_geoFind.nInside;
    if (!n) { swprintf(err, cch, L"Windows doesn't list a country called \"%s\". Check the spelling.", q); return FALSE; }
    wchar_t (*list)[128] = g_geoFind.nPrefix ? g_geoFind.prefix : g_geoFind.inside;
    int shown = n < 3 ? n : 3;
    int len = swprintf(err, cch, L"Which one? ");
    for (int i = 0; i < shown && len >= 0 && len < cch; i++)
        len += swprintf(err + len, cch - len, L"%s%s", i ? L", " : L"", list[i]);
    if (n > shown && len >= 0 && len < cch) swprintf(err + len, cch - len, L" and %d more.", n - shown);
    return FALSE;
}

// ---------- its time zones ----------
typedef void* (__cdecl *UOpenCountryFn)(const char*, int*);
typedef const wchar_t* (__cdecl *UNextFn)(void*, int*, int*);
typedef void (__cdecl *UCloseFn)(void*);
typedef int (__cdecl *UWindowsIdFn)(const wchar_t*, int, wchar_t*, int, int*);

static BOOL zoneRead(const wchar_t* id, TzZone* z) {
    wchar_t path[256];
    swprintf(path, 256, TZ_KEY L"\\%s", id);
    DWORD cb = sizeof(z->display);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path, L"Display", RRF_RT_REG_SZ, NULL, z->display, &cb) != ERROR_SUCCESS) return FALSE;
    BYTE tzi[64]; cb = sizeof(tzi);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path, L"TZI", RRF_RT_REG_BINARY, NULL, tzi, &cb) != ERROR_SUCCESS || cb < 4) return FALSE;
    memcpy(&z->bias, tzi, sizeof(LONG));
    wcsncpy(z->id, id, 127); z->id[127] = 0;
    return TRUE;
}

static int zoneOrder(const void* a, const void* b) {          // west to east
    const TzZone* x = (const TzZone*)a; const TzZone* y = (const TzZone*)b;
    if (x->bias != y->bias) return x->bias > y->bias ? -1 : 1;
    return wcscmp(x->display, y->display);
}

// The Windows time zones in use in a country, west to east.
static int regionZones(const wchar_t* iso, TzZone* out, int max) {
    int n = 0;
    if (!wcscmp(iso, L"XK")) {                                 // Kosovo isn't in Windows' time zone data
        if (zoneRead(L"Central Europe Standard Time", &out[0])) n = 1;
        return n;
    }
    HMODULE icu = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!icu) return 0;
    UOpenCountryFn open = (UOpenCountryFn)GetProcAddress(icu, "ucal_openCountryTimeZones");
    UNextFn next = (UNextFn)GetProcAddress(icu, "uenum_unext");
    UCloseFn close = (UCloseFn)GetProcAddress(icu, "uenum_close");
    UWindowsIdFn windowsId = (UWindowsIdFn)GetProcAddress(icu, "ucal_getWindowsTimeZoneID");
    if (open && next && close && windowsId) {
        char cc[3] = { (char)iso[0], (char)iso[1], 0 };
        int st = 0;
        void* e = open(cc, &st);
        if (e && st <= 0) {
            const wchar_t* z; int len = 0, s2 = 0;
            while (n < max && (z = next(e, &len, &s2)) != NULL && s2 <= 0) {
                wchar_t id[128]; int s3 = 0;
                int w = windowsId(z, len, id, 127, &s3);
                if (w <= 0 || s3 > 0) continue;
                id[w] = 0;
                BOOL dup = FALSE;
                for (int i = 0; i < n; i++) if (!wcscmp(out[i].id, id)) dup = TRUE;
                if (!dup && zoneRead(id, &out[n])) n++;
            }
        }
        if (e) close(e);
    }
    FreeLibrary(icu);
    qsort(out, n, sizeof(TzZone), zoneOrder);
    return n;
}

// ---------- its English ----------
static struct { const wchar_t* want; BOOL found; } g_localeFind;
static BOOL CALLBACK localeFindProc(LPWSTR name, DWORD flags, LPARAM l) {
    (void)flags; (void)l;
    if (!_wcsicmp(name, g_localeFind.want)) { g_localeFind.found = TRUE; return FALSE; }
    return TRUE;
}
// Windows accepts any en-XX, but only those it lists have the country's own date and number formats.
static BOOL localeIsListed(const wchar_t* name) {
    g_localeFind.want = name; g_localeFind.found = FALSE;
    EnumSystemLocalesEx(localeFindProc, LOCALE_ALL, 0, NULL);
    return g_localeFind.found;
}

// Fills in everything needed for a country.
static BOOL regionPrepare(const wchar_t* iso, RegionPick* p, wchar_t* err, int cch) {
    memset(p, 0, sizeof(*p));
    wcsncpy(p->iso, iso, 7);
    if (!geoFriendlyName(iso, p->name, 128)) wcsncpy(p->name, iso, 127);
    swprintf(p->locale, 16, L"en-%s", iso);
    p->realLocale = localeIsListed(p->locale);
    p->nZones = regionZones(iso, p->zones, REGION_MAX_ZONES);
    if (!p->nZones) { swprintf(err, cch, L"Windows has no time zone for %s.", p->name); return FALSE; }
    // keep the current zone selected if it's one of them
    DYNAMIC_TIME_ZONE_INFORMATION now;
    if (GetDynamicTimeZoneInformation(&now) != TIME_ZONE_ID_INVALID)
        for (int i = 0; i < p->nZones; i++) if (!wcscmp(p->zones[i].id, now.TimeZoneKeyName)) p->zone = i;
    return TRUE;
}

// The dialog calls this on Apply, and stays open with the message if it fails.
static BOOL regionCheckInput(const wchar_t* typed, wchar_t* err, int cch) {
    wchar_t iso[8];
    return regionFindCountry(typed, iso, err, cch) && regionPrepare(iso, &g_region, err, cch);
}

// ---------- what's set now ----------
static void regValueText(const wchar_t* sub, const wchar_t* name, wchar_t* out, DWORD cch) {
    DWORD cb = cch * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, sub, name, RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS) out[0] = 0;
}

static void regionDescribeNow(wchar_t* out, int cch) {
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    wchar_t iso[16], name[128], locale[32];
    regValueText(INTL_KEY L"\\Geo", L"Name", iso, 16);
    regValueText(INTL_KEY, L"LocaleName", locale, 32);
    if (!iso[0] || !geoFriendlyName(iso, name, 128)) wcscpy(name, iso);
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) tz.TimeZoneKeyName[0] = 0;
    swprintf(out, cch, L"%s, %s, %s", tz.TimeZoneKeyName, name, locale);
}

// ---------- applying it ----------
// Runs on a worker thread. None of this needs administrator rights.
static DWORD regionApply(const RegionPick* p) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 256], now[64];
    const wchar_t* zone = p->zones[p->zone].id;

    systemPath(exe, L"tzutil.exe");
    swprintf(cmd, MAX_PATH + 256, L"\"%s\" /s \"%s\"", exe, zone);
    runWithPercent(exe, cmd, NULL);

    GeoSetFn setGeo = (GeoSetFn)kernelFn("SetUserGeoName");
    if (setGeo) setGeo((PWSTR)p->iso);

    // Windows has no documented call that sets the whole regional format, so this is its own cmdlet.
    // The name comes from Windows' country list, never from what was typed.
    systemPath(exe, L"WindowsPowerShell\\v1.0\\powershell.exe");
    swprintf(cmd, MAX_PATH + 256, L"\"%s\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command Set-Culture %s", exe, p->locale);
    runWithPercent(exe, cmd, NULL);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"intl", SMTO_ABORTIFHUNG, 1000, NULL);

    // judge by what Windows now reports, not by exit codes
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    BOOL tzOk = GetDynamicTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID && !wcscmp(tz.TimeZoneKeyName, zone);
    regValueText(INTL_KEY L"\\Geo", L"Name", now, 64);
    BOOL geoOk = !_wcsicmp(now, p->iso);
    regValueText(INTL_KEY, L"LocaleName", now, 64);
    BOOL localeOk = !_wcsicmp(now, p->locale);

    if (!tzOk || !geoOk || !localeOk) {
        swprintf(g_regionMsg, 320, L"%s didn't finish. Time zone: %s. Country or region: %s. Regional format: %s.", p->name,
                 tzOk ? L"set" : L"not changed", geoOk ? L"set" : L"not changed", localeOk ? L"set" : L"not changed");
        return ERROR_GEN_FAILURE;
    }
    int len = swprintf(g_regionMsg, 320, L"Set to %s: %s, %s.", p->name, zone, p->locale);
    DWORD autoTz = 4, cb = sizeof(autoTz);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\tzautoupdate", L"Start", RRF_RT_REG_DWORD, NULL, &autoTz, &cb);
    if (autoTz != 4) swprintf(g_regionMsg + len, 320 - len, L" \"Set time zone automatically\" is on, so Windows may change the zone back.");
    else if (!p->realLocale) swprintf(g_regionMsg + len, 320 - len, L" Windows has only generic data for this English, so dates may use US style.");
    return 0;
}
