// Install Apps: NVIDIA and AMD graphics drivers. Nothing is installed: the newest driver for the graphics in this PC is
// looked up, and downloaded to where you choose.
//
// NVIDIA: NVIDIA's own driver lookup (the one behind nvidia.com/drivers) for this GPU, desktop or laptop, gives the
// newest Game Ready Driver. Hotfix drivers aren't listed there; they sit on NVIDIA's download server under one fixed
// name, so the versions above the Game Ready Driver are asked for one by one. Only a hotfix newer than the Game Ready
// Driver is offered: an older one is already part of it.
// AMD: the product page for this GPU (or for the processor, for Radeon graphics built into a Ryzen) on amd.com lists its
// Windows 11 driver, Recommended (WHQL) and sometimes Optional. An Optional driver is offered only when it's newer.
// Laptop graphics have their own entries with both: NVIDIA's "(Notebooks)" series, AMD's mobile models.
#include <setupapi.h>
#include <devguid.h>

enum { GPU_NV, GPU_AMD, GPU_N };
static const wchar_t* GPU_VENDOR[GPU_N] = { L"NVIDIA", L"AMD" };
static const wchar_t* GPU_SIGNER[GPU_N] = { L"NVIDIA Corporation", L"Advanced Micro Devices" };   // as their certificates name them (checked Nov 2026)
typedef struct {
    BOOL found;
    wchar_t name[128];          // as Windows names it
    wchar_t installed[32];      // the driver in the vendor's own numbering (621.35, 26.11.2), empty if unknown
} GpuInfo;
static GpuInfo g_gpu[GPU_N];
static BOOL g_gpuLaptop = FALSE;
static int  g_gpuRun = -1;                     // the vendor whose lookup or download runs, or -1
static int  g_gpuStep = 0;                     // 1 looking up, 2 downloading, 3 checking the signature
static volatile LONG g_gpuCancel = 0;
static volatile LONG64 g_gpuGot = 0, g_gpuTotal = 0;
static wchar_t g_gpuLatest[GPU_N][64];         // what the last lookup found, for the row
static wchar_t g_gpuCpuTest[128];              // the self-test's processor name in place of this PC's
static wchar_t g_gpuAvail[GPU_N][96];          // the newest offered, from the background check ("621.40", "621.40, hotfix 621.52")
static volatile LONG g_gpuChecking = 0;
static ULONGLONG g_gpuCheckedAt = 0;
static _Thread_local BOOL g_gpuQuiet = FALSE;  // the background check writes nothing to the log

static void gpuSay(const wchar_t* fmt, ...) {
    if (g_gpuQuiet) return;
    va_list ap; va_start(ap, fmt); appsSayV(FALSE, fmt, ap); va_end(ap);
}

#define GPU_AMD_INDEX L"https://www.amd.com/en/support/download/drivers.html"
#define GPU_AMD_REFERER L"Referer: https://www.amd.com/\r\n"     // AMD's download server refuses requests without it

// ---------- this PC ----------
// SMBIOS says what kind of case this is; a battery decides when it doesn't
static BOOL gpuIsLaptop(void) {
    DWORD n = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    BYTE* buf = n ? (BYTE*)malloc(n) : NULL;
    int chassis = 0;
    if (buf && GetSystemFirmwareTable('RSMB', 0, buf, n) == n && n > 8) {
        DWORD len = *(DWORD*)(buf + 4);
        BYTE* p = buf + 8; BYTE* end = buf + 8 + min(len, n - 8);
        while (p + 4 <= end) {
            BYTE type = p[0], flen = p[1];
            if (flen < 4 || p + flen > end) break;
            if (type == 3 && flen > 5) { chassis = p[5] & 0x7F; break; }
            if (type == 127) break;
            BYTE* s = p + flen;                    // the strings after it end with two zero bytes
            while (s + 1 < end && (s[0] || s[1])) s++;
            p = s + 2;
        }
    }
    free(buf);
    switch (chassis) {
    case 8: case 9: case 10: case 11: case 14: case 30: case 31: case 32: return TRUE;   // portable ... detachable
    case 3: case 4: case 5: case 6: case 7: case 13: case 15: case 16: case 17: case 23: case 24: case 35: case 36: return FALSE;
    }
    SYSTEM_POWER_STATUS ps;
    return GetSystemPowerStatus(&ps) && ps.BatteryFlag != 255 && !(ps.BatteryFlag & 128);
}

static BOOL gpuGenericName(const wchar_t* n) {
    return !n[0] || wcsstr(n, L"Basic Display") || !_wcsicmp(n, L"AMD Radeon(TM) Graphics") || !_wcsicmp(n, L"AMD Radeon Graphics");
}

// The NVIDIA and AMD graphics present, with their driver versions
static void gpuRefresh(void) {
    memset(g_gpu, 0, sizeof(g_gpu));
    g_gpuLaptop = gpuIsLaptop();
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA d = { sizeof(d) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t hw[512] = L"", name[128] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, NULL, (BYTE*)hw, sizeof(hw) - 4, NULL)) continue;
        _wcsupr(hw);
        int v = wcsstr(hw, L"VEN_10DE") ? GPU_NV : wcsstr(hw, L"VEN_1002") ? GPU_AMD : -1;
        if (v < 0) continue;
        SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, NULL, (BYTE*)name, sizeof(name) - 2, NULL);
        // with two of one vendor (a Ryzen's graphics and a Radeon card), the one with its own name
        if (g_gpu[v].found && !gpuGenericName(g_gpu[v].name)) continue;
        if (g_gpu[v].found && gpuGenericName(name)) continue;
        g_gpu[v].found = TRUE;
        lstrcpynW(g_gpu[v].name, name, 128);
        g_gpu[v].installed[0] = 0;
        HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            wchar_t ver[64] = L""; DWORD cb = sizeof(ver) - 2;
            if (v == GPU_AMD) {
                if (RegQueryValueExW(k, L"RadeonSoftwareVersion", NULL, NULL, (BYTE*)ver, &cb) == ERROR_SUCCESS) lstrcpynW(g_gpu[v].installed, ver, 32);
            } else if (RegQueryValueExW(k, L"DriverVersion", NULL, NULL, (BYTE*)ver, &cb) == ERROR_SUCCESS) {
                unsigned a, b, c, e;                 // 32.0.16.2135 is 621.35: the last digit of the third part, then the fourth
                if (swscanf(ver, L"%u.%u.%u.%u", &a, &b, &c, &e) == 4 && e < 10000) {
                    unsigned n = (c % 10) * 10000 + e;
                    swprintf(g_gpu[v].installed, 32, L"%u.%02u", n / 100, n % 100);
                }
            }
            RegCloseKey(k);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

// ---------- what was found ----------
typedef struct { wchar_t label[160], what[80], ver[32], url[512], file[200]; } GpuChoice;
typedef struct { int vendor; wchar_t err[300]; int n; GpuChoice c[2]; } GpuOffer;

static void gpuFileFromUrl(const wchar_t* url, wchar_t* out, int cch) {
    const wchar_t* s = wcsrchr(url, L'/');
    lstrcpynW(out, s ? s + 1 : url, cch);
    wchar_t* q = wcschr(out, L'?');
    if (q) *q = 0;
}

static wchar_t* gpuUtf8(const char* s, int n) {
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    wchar_t* out = (wchar_t*)calloc(w + 1, sizeof(wchar_t));
    if (out) MultiByteToWideChar(CP_UTF8, 0, s, n, out, w);
    return out;
}

// lower case, without "(TM)"/"(R)" and the marks themselves, single spaces
static void gpuNorm(const wchar_t* in, wchar_t* out, int cch) {
    int n = 0;
    for (const wchar_t* p = in; *p && n < cch - 1; p++) {
        if (!_wcsnicmp(p, L"(tm)", 4) || !_wcsnicmp(p, L"(r)", 3)) { p += (p[2] == L')' ? 2 : 3); continue; }
        if (*p == 0x2122 || *p == 0x00AE) continue;
        wchar_t c = towlower(*p);
        if (c == L' ' && (n == 0 || out[n - 1] == L' ')) continue;
        out[n++] = c;
    }
    while (n && out[n - 1] == L' ') n--;
    out[n] = 0;
}

// ---------- NVIDIA ----------
typedef struct { int value, parent; wchar_t name[96]; } NvEntry;

// <LookupValue ParentID="1"><Name>GeForce RTX 50 Series</Name><Value>131</Value></LookupValue>, all of them
static int gpuNvList(int typeId, NvEntry** out) {
    wchar_t url[160]; char* body = NULL; DWORD status = 0;
    swprintf(url, 160, L"https://www.nvidia.com/Download/API/lookupValueSearch.aspx?TypeID=%d", typeId);
    *out = NULL;
    if (dlHttp(url, &body, NULL, &status) || status != 200 || !body) { free(body); return -1; }
    int n = 0, cap = 0;
    NvEntry* e = NULL;
    for (const char* p = strstr(body, "<LookupValue "); p; p = strstr(p + 1, "<LookupValue ")) {
        const char* par = strstr(p, "ParentID=\"");
        const char* nm = strstr(p, "<Name>");
        const char* ne = nm ? strstr(nm, "</Name>") : NULL;
        const char* va = ne ? strstr(ne, "<Value>") : NULL;
        const char* next = strstr(p + 1, "<LookupValue ");
        if (!par || !va || (next && va > next)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 512; NvEntry* m = (NvEntry*)realloc(e, cap * sizeof(NvEntry)); if (!m) break; e = m; }
        e[n].parent = atoi(par + 10);
        e[n].value = atoi(va + 7);
        wchar_t* w = gpuUtf8(nm + 6, (int)(ne - nm - 6));
        lstrcpynW(e[n].name, w ? w : L"", 96);
        free(w);
        n++;
    }
    free(body);
    *out = e;
    return n;
}

// NVIDIA's name for a product, compared without "NVIDIA " in front (newer entries have it, older ones don't)
static void gpuNvKey(const wchar_t* in, wchar_t* out, int cch) {
    gpuNorm(in, out, cch);
    if (!wcsncmp(out, L"nvidia ", 7)) memmove(out, out + 7, (wcslen(out) - 6) * sizeof(wchar_t));
}

// Each worker keeps one connection open for all its questions: a new connection per question took about 2 seconds each
typedef struct { int maj, from, to; volatile LONG next, best; } NvProbe;
static DWORD WINAPI gpuNvProbeProc(LPVOID arg) {
    NvProbe* p = (NvProbe*)arg;
    HINTERNET s = WinHttpOpen(L"WindowsChosenUtilities", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"international.download.nvidia.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    for (; c; ) {
        LONG m = InterlockedIncrement(&p->next) - 1;
        if (m > p->to || g_gpuCancel) break;
        wchar_t path[200]; DWORD status = 0, cb = sizeof(status);
        swprintf(path, 200, L"/Windows/%d.%02ldhf/%d.%02ld-desktop-notebook-win10-win11-64bit-international-dch.hf.exe", p->maj, m, p->maj, m);
        HINTERNET r = WinHttpOpenRequest(c, L"HEAD", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (r) {
            WinHttpSetTimeouts(r, 15000, 15000, 15000, 15000);
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL) &&
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX) &&
                status == 200) {
                LONG b;
                do { b = p->best; } while (m > b && InterlockedCompareExchange(&p->best, m, b) != b);
            }
            WinHttpCloseHandle(r);
        }
    }
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return 0;
}

static void gpuNvLookup(GpuOffer* o) {
    const GpuInfo* g = &g_gpu[GPU_NV];
    NvEntry *prod = NULL, *ser = NULL;
    int np = gpuNvList(3, &prod), ns = gpuNvList(2, &ser);
    if (np <= 0 || ns <= 0) { wcscpy(o->err, L"Couldn't reach NVIDIA's driver lookup. Check the connection and try again."); goto done; }
    wchar_t key[128], k2[128];
    gpuNvKey(g->name, key, 128);
    BOOL laptop = g_gpuLaptop || wcsstr(key, L"laptop gpu") != NULL;
    int psid = 0, pfid = 0, score = -1;
    for (int i = 0; i < np; i++) {
        gpuNvKey(prod[i].name, k2, 128);
        if (wcscmp(k2, key)) continue;
        BOOL nb = FALSE;
        for (int s = 0; s < ns; s++) if (ser[s].value == prod[i].parent) nb = wcsstr(ser[s].name, L"Notebook") != NULL;
        int sc = (nb == laptop) ? 2 : 1;         // the same name sits in a desktop and a notebook series
        if (sc > score) { score = sc; psid = prod[i].parent; pfid = prod[i].value; }
    }
    if (!pfid) {                                  // no driver yet, so Windows doesn't know its name: the newest GeForce series
        for (int s = 0; s < ns; s++) {
            BOOL nb = wcsstr(ser[s].name, L"Notebook") != NULL;
            if (!wcsncmp(ser[s].name, L"GeForce RTX", 11) && nb == laptop && ser[s].value > psid) psid = ser[s].value;
        }
        for (int i = 0; i < np && psid; i++) if (prod[i].parent == psid) { pfid = prod[i].value; break; }
        if (!pfid) { swprintf(o->err, 300, L"NVIDIA's driver lookup doesn't list \"%s\".", g->name); goto done; }
        gpuSay(L"\"%s\" isn't in NVIDIA's list, so this looks up the newest GeForce %s series instead.", g->name, laptop ? L"laptop" : L"desktop");
    }
    gpuSay(L"NVIDIA lists it as product %d in series %d (%s).", pfid, psid, laptop ? L"laptop" : L"desktop");

    wchar_t url[400]; char* body = NULL; DWORD status = 0;
    swprintf(url, 400, L"https://gfwsl.geforce.com/services_toolkit/services/com/nvidia/services/AjaxDriverService.php?"
                       L"func=DriverManualLookup&psid=%d&pfid=%d&osID=57&languageCode=1033&isWHQL=1&dch=1&upCRD=0&sort1=0&numberOfResults=1", psid, pfid);
    if (dlHttp(url, &body, NULL, &status) || status != 200 || !body) {
        free(body); wcscpy(o->err, L"Couldn't reach NVIDIA's driver lookup. Check the connection and try again."); goto done;
    }
    const char* ids = jGet(body, "IDS");
    JIter it; const char* k; const char* first = NULL;
    if (ids && jOpen(ids, &it) && jNext(&it, &k, &first)) {}
    const char* info = first ? jGet(first, "downloadInfo") : NULL;
    wchar_t ver[32] = L"", dl[512] = L"", date[64] = L"", name[160] = L"";
    jText(jGet(info, "Version"), ver, 32);
    jText(jGet(info, "DownloadURL"), dl, 512);
    jText(jGet(info, "ReleaseDateTime"), date, 64);
    jText(jGet(info, "Name"), name, 160);
    free(body);
    for (wchar_t* p = name; (p = wcsstr(p, L"%20")) != NULL; ) { *p = L' '; memmove(p + 1, p + 3, (wcslen(p + 3) + 1) * sizeof(wchar_t)); }
    if (!ver[0] || wcsncmp(dl, L"https://", 8)) { swprintf(o->err, 300, L"NVIDIA lists no Windows 11 driver for %s.", g->name); goto done; }
    // the date reads "Tue Nov 24, 2026": the weekday goes
    const wchar_t* d = wcschr(date, L' ');
    GpuChoice* c = &o->c[o->n++];
    lstrcpynW(c->ver, ver, 32); lstrcpynW(c->url, dl, 512);
    lstrcpynW(c->what, name[0] ? name : L"NVIDIA driver", 80);
    swprintf(c->label, 160, L"%s %s · %s", c->what, ver, d ? d + 1 : date);
    gpuFileFromUrl(dl, c->file, 200);
    gpuSay(L"Newest %s: %s (%s).", c->what, ver, d ? d + 1 : date);
    gpuSay(L"  %s", dl);

    // a hotfix: only one newer than the Game Ready Driver, on the same branch
    int maj = 0, mn = 0;
    if (wcsstr(name, L"Game Ready") && swscanf(ver, L"%d.%d", &maj, &mn) == 2 && mn < 99) {
        gpuSay(L"Looking for a hotfix newer than %s…", ver);
        NvProbe pr = { maj, mn + 1, 99, mn + 1, -1 };
        HANDLE t[12]; int nt = 0;
        for (int i = 0; i < 12; i++) if ((t[nt] = CreateThread(NULL, 0, gpuNvProbeProc, &pr, 0, NULL)) != NULL) nt++;
        if (nt) WaitForMultipleObjects(nt, t, TRUE, INFINITE);
        for (int i = 0; i < nt; i++) CloseHandle(t[i]);
        if (pr.best > 0) {
            GpuChoice* h = &o->c[o->n++];
            swprintf(h->ver, 32, L"%d.%02ld", maj, pr.best);
            swprintf(h->url, 512, L"https://international.download.nvidia.com/Windows/%shf/%s-desktop-notebook-win10-win11-64bit-international-dch.hf.exe", h->ver, h->ver);
            wcscpy(h->what, L"GeForce Hotfix Driver");
            swprintf(h->label, 160, L"GeForce Hotfix Driver %s · newer than %s, not WHQL", h->ver, ver);
            gpuFileFromUrl(h->url, h->file, 200);
            gpuSay(L"Hotfix %s is newer than the Game Ready Driver.", h->ver);
            gpuSay(L"  %s", h->url);
        } else gpuSay(L"No newer hotfix: Game Ready Driver %s is the newest.", ver);
    }
done:
    free(prod); free(ser);
}

// ---------- AMD ----------
// The product page's path for a name: "AMD Radeon RX 9070 XT" -> .../amd-radeon-rx-9070-xt.html
static BOOL gpuAmdFind(const char* index, const wchar_t* name, wchar_t* path, int cch) {
    wchar_t n[128], slug[3][140];
    gpuNorm(name, n, 128);
    size_t len = wcslen(n);
    if (len > 7 && !wcscmp(n + len - 7, L" series")) n[len - 7] = 0;
    for (wchar_t* p = n; *p; p++) if (*p == L' ' || *p == L'/') *p = L'-';
    swprintf(slug[0], 140, L"/%s.html", n);
    if (!wcsncmp(n, L"amd-", 4)) swprintf(slug[1], 140, L"/%s.html", n + 4); else swprintf(slug[1], 140, L"/amd-%s.html", n);
    slug[2][0] = 0;
    for (int s = 0; s < 2; s++) {
        char a[160];
        WideCharToMultiByte(CP_UTF8, 0, slug[s], -1, a, 160, NULL, NULL);
        for (const char* p = strstr(index, a); p; p = strstr(p + 1, a)) {
            // back to the start of the link: the page quotes them as "...", '...' or &#34;...&#34;
            const char* b = p;
            while (b > index && p - b < 300 && !strchr("\"'&, <>;", b[-1])) b--;
            if (!strncmp(b, "https://www.amd.com/", 20)) b += 19;
            if (strncmp(b, "/en/support/downloads/drivers.html/", 35)) continue;
            int l = (int)(p - b) + (int)strlen(a);
            wchar_t* w = gpuUtf8(b, l);
            if (w) { swprintf(path, cch, L"https://www.amd.com%s", w); free(w); return TRUE; }
        }
    }
    return FALSE;
}

// The text of the first <p> after a label, without tags
static void gpuAmdField(const char* from, const char* until, const char* label, wchar_t* out, int cch) {
    out[0] = 0;
    const char* l = strstr(from, label);
    if (!l || (until && l > until)) return;
    const char* p = strstr(l, "<p>");
    const char* e = p ? strstr(p, "</p>") : NULL;
    if (!p || !e || (until && e > until)) return;
    wchar_t* w = gpuUtf8(p + 3, (int)(e - p - 3));
    if (!w) return;
    int n = 0;
    for (wchar_t* s = w; *s && n < cch - 1; s++) {
        if (*s == L'<') { while (*s && *s != L'>') s++; if (!*s) break; continue; }
        if (*s == 0x200B) continue;                              // zero-width spaces in AMD's pages
        if (iswspace(*s) && (n == 0 || out[n - 1] == L' ')) continue;
        out[n++] = iswspace(*s) ? L' ' : *s;
    }
    while (n && out[n - 1] == L' ') n--;
    out[n] = 0;
    free(w);
}

static void gpuAmdLookup(GpuOffer* o) {
    const GpuInfo* g = &g_gpu[GPU_AMD];
    char* index = NULL; DWORD status = 0;
    if (dlHttp(GPU_AMD_INDEX, &index, NULL, &status) || status != 200 || !index) {
        free(index); wcscpy(o->err, L"Couldn't reach amd.com. Check the connection and try again."); return;
    }
    wchar_t page[400];
    BOOL found = !gpuGenericName(g->name) && gpuAmdFind(index, g->name, page, 400);
    if (!found) {                                  // Radeon graphics built into the processor: the processor's page
        wchar_t cpu[128] = L""; DWORD cb = sizeof(cpu) - 2;
        if (g_gpuCpuTest[0]) lstrcpynW(cpu, g_gpuCpuTest, 128);
        else RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &cb);
        wchar_t* cut;
        if ((cut = wcsstr(cpu, L" w/")) != NULL) *cut = 0;
        if ((cut = wcsstr(cpu, L" with ")) != NULL) *cut = 0;
        for (wchar_t* p = cpu; *p; p++) {            // "AMD Ryzen 9 9950X 16-Core Processor": up to " 16-Core"
            if (*p != L' ' || !iswdigit(p[1])) continue;
            wchar_t* q = p + 1;
            while (iswdigit(*q)) q++;
            if (!_wcsnicmp(q, L"-Core", 5)) { *p = 0; break; }
        }
        while (wcslen(cpu) && cpu[wcslen(cpu) - 1] == L' ') cpu[wcslen(cpu) - 1] = 0;
        if (cpu[0] && gpuAmdFind(index, cpu, page, 400)) { found = TRUE; gpuSay(L"Using the page for the processor, %s.", cpu); }
    }
    free(index);
    if (!found) { swprintf(o->err, 300, L"amd.com has no driver page for \"%s\".", g->name); return; }
    gpuSay(L"AMD's page: %s", page);

    char* body = NULL;
    if (dlHttp(page, &body, NULL, &status) || status != 200 || !body) {
        free(body); wcscpy(o->err, L"Couldn't read the driver page on amd.com. Check the connection and try again."); return;
    }
    const char* w11 = strstr(body, "Windows 11 - 64-Bit");
    const char* w10 = w11 ? strstr(w11, "Windows 10 - 64-Bit") : NULL;
    GpuChoice rec, opt; BOOL haveRec = FALSE, haveOpt = FALSE;
    memset(&rec, 0, sizeof(rec)); memset(&opt, 0, sizeof(opt));
    for (const char* a = w11 ? strstr(w11, "driver-download-details") : NULL; a && (!w10 || a < w10); a = strstr(a + 1, "driver-download-details")) {
        const char* end = strstr(a + 1, "driver-download-details");
        if (w10 && (!end || end > w10)) end = w10;
        wchar_t rev[160], date[40];
        gpuAmdField(a, end, "Revision Number", rev, 160);
        gpuAmdField(a, end, "Release Date", date, 40);
        if (!wcsstr(rev, L"Adrenalin")) continue;                // the auto-detect tool has none
        const char* h = strstr(a, "href=\"https://drivers.amd.com/");
        if (!h || (end && h > end)) continue;
        const char* he = strchr(h + 6, '"');
        wchar_t* url = he ? gpuUtf8(h + 6, (int)(he - h - 6)) : NULL;
        if (!url) continue;
        BOOL optional = wcsstr(rev, L"Optional") != NULL;
        GpuChoice* c = optional ? &opt : &rec;
        if (optional ? haveOpt : haveRec) { free(url); continue; }   // the first of each is the newest
        const wchar_t* v = rev;
        while (*v && !iswdigit(*v)) v++;
        int n = 0;
        while ((iswdigit(v[n]) || v[n] == L'.') && n < 31) { c->ver[n] = v[n]; n++; }
        c->ver[n] = 0;
        lstrcpynW(c->url, url, 512);
        free(url);
        wcscpy(c->what, L"AMD Software: Adrenalin Edition");
        swprintf(c->label, 160, L"%s · %s", rev, date);
        gpuFileFromUrl(c->url, c->file, 200);
        if (optional) haveOpt = TRUE; else haveRec = TRUE;
    }
    free(body);
    if (!haveRec && !haveOpt) { swprintf(o->err, 300, L"amd.com lists no Windows 11 driver for %s.", g->name); return; }
    if (haveRec) { o->c[o->n++] = rec; gpuSay(L"Recommended: %s.", rec.label); gpuSay(L"  %s", rec.url); }
    if (haveOpt && (!haveRec || appsVerCmp(opt.ver, rec.ver) > 0)) { o->c[o->n++] = opt; gpuSay(L"Optional, newer: %s.", opt.label); gpuSay(L"  %s", opt.url); }
    else if (haveOpt) gpuSay(L"Optional %s isn't newer than Recommended, so it's not offered.", opt.ver);
}

// ---------- the jobs ----------
static DWORD WINAPI gpuLookupProc(LPVOID arg) {
    int v = (int)(INT_PTR)arg;
    GpuOffer* o = (GpuOffer*)calloc(1, sizeof(GpuOffer));
    if (o) {
        o->vendor = v;
        if (v == GPU_NV) gpuNvLookup(o); else gpuAmdLookup(o);
        if (!o->err[0] && !o->n) wcscpy(o->err, L"No driver was found.");
    }
    if (!PostMessageW(g_hwnd, WM_APP_GPUFOUND, 0, (LPARAM)o)) free(o);
    return 0;
}

// The rows' "newest" versions: the same lookups, in the background when the page opens, at most every 10 minutes
static DWORD WINAPI gpuCheckProc(LPVOID arg) {
    (void)arg;
    g_gpuQuiet = TRUE;
    wchar_t* list = (wchar_t*)calloc(GPU_N * 96, sizeof(wchar_t));
    GpuOffer* o = (GpuOffer*)calloc(1, sizeof(GpuOffer));
    for (int v = 0; v < GPU_N && list && o; v++) {
        if (!g_gpu[v].found) continue;
        memset(o, 0, sizeof(*o));
        o->vendor = v;
        if (v == GPU_NV) gpuNvLookup(o); else gpuAmdLookup(o);
        if (o->err[0] || !o->n) continue;
        if (o->n == 1) lstrcpynW(list + v * 96, o->c[0].ver, 96);
        else swprintf(list + v * 96, 96, L"%s, %s %s", o->c[0].ver, v == GPU_NV ? L"hotfix" : L"Optional", o->c[1].ver);
    }
    free(o);
    if (!list || !PostMessageW(g_hwnd, WM_APP_GPULATEST, 0, (LPARAM)list)) { free(list); InterlockedExchange(&g_gpuChecking, 0); }
    return 0;
}
static void gpuCheckLatest(void) {
    if (!g_gpu[GPU_NV].found && !g_gpu[GPU_AMD].found) return;
    if (g_gpuCheckedAt && GetTickCount64() - g_gpuCheckedAt < 10 * 60 * 1000) return;
    if (InterlockedCompareExchange(&g_gpuChecking, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, gpuCheckProc, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_gpuChecking, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void gpuOnLatest(wchar_t* list) {
    for (int v = 0; v < GPU_N; v++) if (list[v * 96]) lstrcpynW(g_gpuAvail[v], list + v * 96, 96);   // a failed lookup keeps what was known
    free(list);
    g_gpuCheckedAt = GetTickCount64();
    InterlockedExchange(&g_gpuChecking, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

typedef struct { int vendor; wchar_t url[512], path[MAX_PATH]; } GpuDl;
#define GPU_PARTS 4                                    // the download comes in 4 parts at once, each over its own connection
static volatile LONG64 g_gpuSpeed = 0;                 // bytes a second, over the last couple of seconds
static ULONGLONG g_gpuStartTick = 0;
static BOOL gpuTick(unsigned long long got, void* ctx) {
    (void)ctx;
    static ULONGLONG last, sampleTick;
    static unsigned long long sampleGot;
    ULONGLONG now = GetTickCount64();
    InterlockedExchange64(&g_gpuGot, (LONG64)got);
    if (got < sampleGot || !sampleTick) { sampleTick = now; sampleGot = got; }   // a new download
    if (now - sampleTick >= 1000) {                    // the speed over the last second, smoothed a little
        LONG64 v = (LONG64)((got - sampleGot) * 1000 / (now - sampleTick)), old = g_gpuSpeed;
        InterlockedExchange64(&g_gpuSpeed, old ? (old + 2 * v) / 3 : v);
        sampleTick = now; sampleGot = got;
    }
    if (now - last > 250) { last = now; InvalidateRect(g_hwnd, NULL, FALSE); }
    return !g_gpuCancel;
}
static void gpuSpeedText(double bytesPerSec, wchar_t* out, int cch) {
    if (bytesPerSec >= 1024.0 * 1024) swprintf(out, cch, L"%.1f MB/s", bytesPerSec / (1024.0 * 1024));
    else swprintf(out, cch, L"%.0f KB/s", bytesPerSec / 1024.0);
}
static DWORD WINAPI gpuDownloadProc(LPVOID arg) {
    GpuDl* j = (GpuDl*)arg;
    wchar_t part[MAX_PATH + 8];
    swprintf(part, MAX_PATH + 8, L"%s.part", j->path);
    DWORD status = 0; unsigned long long len = 0;
    // the announced size, for the progress
    const wchar_t* hdrs = j->vendor == GPU_AMD ? GPU_AMD_REFERER : NULL;
    if (dlHttpReq(j->url, L"HEAD", hdrs, NULL, NULL, &status, &len, FALSE, NULL, NULL) || status != 200) len = 0;
    InterlockedExchange64(&g_gpuTotal, (LONG64)len);
    InterlockedExchange64(&g_gpuSpeed, 0);
    g_gpuStartTick = GetTickCount64();
    DWORD err = ERROR_NOT_SUPPORTED;
    int parts = 1;
    if (len) {                                     // in parts; a server that won't send parts gets one plain download
        err = dlHttpParallelEx(j->url, hdrs, GPU_PARTS, part, len, &status, FALSE, gpuTick, NULL, NULL);
        if (!err) { parts = GPU_PARTS; status = 200; }
    }
    if (err == ERROR_NOT_SUPPORTED) {
        InterlockedExchange64(&g_gpuGot, 0);
        err = dlHttpReq(j->url, NULL, hdrs, NULL, part, &status, NULL, FALSE, gpuTick, NULL);
        if (!err && status != 200) err = ERROR_BAD_NET_RESP;
    } else if (err && status && status != 206 && status != 200) err = ERROR_BAD_NET_RESP;
    if (!err) {
        double secs = (GetTickCount64() - g_gpuStartTick) / 1000.0;
        wchar_t sp[32];
        gpuSpeedText(secs > 0 ? g_gpuGot / secs : 0, sp, 32);
        gpuSay(L"Downloaded %lld MB in %.0f s, %s on average (%d part%s).", (long long)(g_gpuGot >> 20), secs, sp, parts, parts > 1 ? L"s" : L"");
    }
    if (!err) {
        g_gpuStep = 3;
        InvalidateRect(g_hwnd, NULL, FALSE);
        if (appsCheckSigner(part, GPU_SIGNER[j->vendor]) != S_OK) err = (DWORD)TRUST_E_NOSIGNATURE;
    }
    if (!err && !MoveFileExW(part, j->path, MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
    if (err) DeleteFileW(part);
    free(j);
    PostMessageW(g_hwnd, WM_APP_GPUDONE, err, status);
    return 0;
}

static void gpuEnd(void) {
    g_gpuRun = -1; g_gpuStep = 0;
    InterlockedExchange(&g_busy, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void gpuClick(int v) {
    if (g_gpuRun == v && g_gpuStep == 2) { InterlockedExchange(&g_gpuCancel, 1); return; }   // "Cancel download"
    if (v < 0 || v >= GPU_N || !g_gpu[v].found || !jobBegin()) return;
    g_gpuRun = v; g_gpuStep = 1;
    InterlockedExchange(&g_gpuCancel, 0);
    if (g_appsLogN) appsLogAdd(L"", FALSE);
    wchar_t head[240];
    if (g_gpu[v].installed[0])
        swprintf(head, 240, L"> %s driver for %s (%s, installed %s)", GPU_VENDOR[v], g_gpu[v].name, g_gpuLaptop ? L"laptop" : L"desktop", g_gpu[v].installed);
    else swprintf(head, 240, L"> %s driver for %s (%s)", GPU_VENDOR[v], g_gpu[v].name, g_gpuLaptop ? L"laptop" : L"desktop");
    appsLogAdd(head, FALSE);
    startWorker(gpuLookupProc, (LPVOID)(INT_PTR)v);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void gpuOnFound(GpuOffer* o) {
    if (!o) { appsLogAdd(L"Out of memory.", FALSE); gpuEnd(); return; }
    int v = o->vendor;
    if (o->err[0]) { appsLogAdd(o->err, FALSE); free(o); gpuEnd(); return; }
    if (o->n == 1) lstrcpynW(g_gpuAvail[v], o->c[0].ver, 96);             // the row shows what was just found
    else swprintf(g_gpuAvail[v], 96, L"%s, %s %s", o->c[0].ver, v == GPU_NV ? L"hotfix" : L"Optional", o->c[1].ver);
    int pick = 0;
    if (o->n > 1) {                              // the newest of the safe ones, or the newer hotfix / optional one
        const wchar_t* items[2] = { o->c[0].label, o->c[1].label };
        DlgExtra x; memset(&x, 0, sizeof(x));
        x.items = items; x.nItems = 2; x.pick = &pick;
        wchar_t title[80];
        swprintf(title, 80, L"Choose the %s driver", GPU_VENDOR[v]);
        if (!showDialogEx(g_hwnd, title, v == GPU_NV
                ? L"A hotfix newer than the Game Ready Driver is out. Hotfixes fix specific problems quickly and skip NVIDIA's full testing."
                : L"An Optional driver newer than the Recommended one is out. Optional drivers add newer fixes and games before AMD's full certification.",
                NULL, 0, L"Download", L"Cancel", 0, &x)) {
            appsLogAdd(L"Cancelled. Nothing was downloaded.", FALSE); free(o); gpuEnd(); return;
        }
    }
    GpuChoice* c = &o->c[pick];
    lstrcpynW(g_gpuLatest[v], c->ver, 64);
    wchar_t path[MAX_PATH] = L"", title[160];
    PWSTR dl = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &dl)) && dl) swprintf(path, MAX_PATH, L"%s\\%s", dl, c->file);
    else lstrcpynW(path, c->file, MAX_PATH);
    CoTaskMemFree(dl);
    swprintf(title, 160, L"Save %s %s", c->what, c->ver);
    if (!pickSave(path, L"Programs (*.exe)\0*.exe\0", L"exe", title)) {
        appsLogAdd(L"Cancelled. Nothing was downloaded.", FALSE); free(o); gpuEnd(); return;
    }
    GpuDl* j = (GpuDl*)calloc(1, sizeof(GpuDl));
    if (!j) { free(o); gpuEnd(); return; }
    j->vendor = v;
    lstrcpynW(j->url, c->url, 512);
    lstrcpynW(j->path, path, MAX_PATH);
    gpuSay(L"Downloading %s to %s", c->url, path);
    free(o);
    g_gpuStep = 2;
    InterlockedExchange64(&g_gpuGot, 0); InterlockedExchange64(&g_gpuTotal, 0);
    startWorker(gpuDownloadProc, j);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void gpuOnDone(DWORD err, DWORD status) {
    int v = g_gpuRun;
    wchar_t msg[400];
    if (!err) swprintf(msg, 400, L"Downloaded, signed by %s. Run it to install the driver.", v >= 0 ? GPU_SIGNER[v] : L"its publisher");
    else if (err == ERROR_CANCELLED) wcscpy(msg, L"Download cancelled. The unfinished file was deleted.");
    else if (err == (DWORD)TRUST_E_NOSIGNATURE) swprintf(msg, 400, L"The download isn't signed by %s, so it was deleted.", v >= 0 ? GPU_SIGNER[v] : L"its publisher");
    else if (err == ERROR_BAD_NET_RESP) swprintf(msg, 400, L"The server refused the download (HTTP %lu). Nothing was saved.", status);
    else swprintf(msg, 400, L"The download failed (error %lu). Nothing was saved.", err);
    appsLogAdd(msg, FALSE);
    gpuEnd();
}

// The two rows under the apps
static void gpuDrawRows(int x1, int x2, int bw, int bh, int rowGap, int* y) {
    static const int IDS[GPU_N] = { A_P_NVIDIA, A_P_AMD };
    static const wchar_t* LABEL[GPU_N] = { L"Download NVIDIA driver", L"Download AMD driver" };
    for (int v = 0; v < GPU_N; v++) {
        BOOL cancel = g_gpuRun == v && g_gpuStep == 2;
        BOOL en = g_gpu[v].found && (g_busy == 0 || cancel);
        aioButton(IDS[v], x1, *y, x1 + bw, *y + bh, cancel ? L"Cancel download" : LABEL[v], FALSE, en, FALSE);
        // "GeForce RTX 5080 (desktop) · Installed: 621.35 · 621.40 available", as the app rows put it
        wchar_t st[300], model[128], have[48], avail[140] = L"";
        const wchar_t* nm = g_gpu[v].name;
        if (!_wcsnicmp(nm, L"NVIDIA ", 7)) nm += 7;
        else if (!_wcsnicmp(nm, L"AMD ", 4)) nm += 4;
        swprintf(model, 128, L"%s (%s)", nm, g_gpuLaptop ? L"laptop" : L"desktop");
        if (g_gpu[v].installed[0]) swprintf(have, 48, L"Installed: %s", g_gpu[v].installed); else wcscpy(have, L"Installed: unknown");
        const wchar_t* av = g_gpuAvail[v];
        BOOL newer = FALSE;
        if (av[0]) {
            wchar_t first[32]; int n = 0;              // the first version listed is the safe one (Game Ready / Recommended)
            while (av[n] && av[n] != L',' && n < 31) { first[n] = av[n]; n++; }
            first[n] = 0;
            BOOL latest = g_gpu[v].installed[0] && !wcschr(av, L',') && appsVerCmp(first, g_gpu[v].installed) <= 0;
            newer = !latest && g_gpu[v].installed[0];
            swprintf(avail, 140, latest ? L" (latest)" : L" · %s available", av);
        } else if (g_gpuChecking) wcscpy(avail, L" · checking for drivers…");
        if (!g_gpu[v].found) swprintf(st, 300, L"No %s graphics in this PC", GPU_VENDOR[v]);
        else swprintf(st, 300, L"%s · %s%s", model, have, avail);
        RECT r = { x1 + bw + dp(14), *y, x2, *y + bh };
        drawText(g_fSmall, st, r, newer ? textRGB() : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        *y += bh + rowGap;
    }
}

// The line above the log while a driver job runs
static BOOL gpuNowText(wchar_t* now, int cch) {
    if (g_gpuRun < 0) return FALSE;
    const wchar_t* n = GPU_VENDOR[g_gpuRun];
    long long got = g_gpuGot >> 20, total = g_gpuTotal >> 20;
    if (g_gpuStep == 1) swprintf(now, cch, L"%s: finding the newest driver…", n);
    else if (g_gpuStep == 2) {
        wchar_t sp[32] = L"";
        if (g_gpuSpeed) { sp[0] = L' '; sp[1] = 0x00B7; sp[2] = L' '; gpuSpeedText((double)g_gpuSpeed, sp + 3, 29); }
        if (total) swprintf(now, cch, L"%s: downloading… %lld of %lld MB%s", n, got, total, sp);
        else swprintf(now, cch, L"%s: downloading… %lld MB%s", n, got, sp);
    }
    else swprintf(now, cch, L"%s: checking the signature…", n);
    return TRUE;
}
