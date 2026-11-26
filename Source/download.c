// Download Windows page: finds an x64 or Arm64 Windows build through UUP dump, downloads its files from Microsoft,
// and builds a bootable ISO on the Desktop with UUP dump's own Windows converter.
//
// Nothing about UUP dump is baked in if it can be asked for at run time, so the page keeps working as they change:
//   - editions and languages come from the API, and a build counts as usable if the API lists languages for it,
//     not because of what its title says
//   - the converter, 7zr and aria2c come from the manifests in UUP dump's misc repo, each checked against the
//     SHA-256 listed there. The built-in copy of those details is only used if a manifest can't be read.
//
// To remove this page: set WCU_DOWNLOAD to 0 in core.h and delete this file and dlprogress.c. The other pages don't use anything
// in here. What's left elsewhere is unused names (REP_UUP, A_D_*, PAGE_DOWNLOAD) and search for WCU_DOWNLOAD finds
// the rest. Its work folder, <Documents>\WCU_UUP (or C:\WCU_UUP when Documents can't be used), exists only while a
// build runs. The UUP dump helpers it shares
// with the Manual Updates page (JSON, HTTP, the API call, checksums) are in uupapi.c and stay.

#include <sddl.h>
#include <aclapi.h>

#define DL_MAX_EDITIONS 12
#define DL_MAX_LANGS 64
#define DL_MAX_FILES 8192         // a build's app packages alone can be nearly 3000 files
#define DL_MANIFEST_CONVERTER L"https://git.uupdump.net/uup-dump/misc/raw/branch/master/autodl_files/converter_windows"
#define DL_MANIFEST_ARIA2     L"https://git.uupdump.net/uup-dump/misc/raw/branch/master/autodl_files/get_aria2.ps1"
#define DL_LIST_GAP_MS  1000
#define DL_SAVED_PREFIX L"Done. ISO saved to "   // the job's last line, which is how the window learns the file's name

// The architectures offered: UUP dump's name for each, and how it's shown and written in the ISO's name
typedef struct { const wchar_t* api; const wchar_t* label; } DlArch;
static const DlArch DL_ARCHES[] = { { L"amd64", L"x64" }, { L"arm64", L"arm64" } };
#define DL_NARCHES 2

static const wchar_t* dlArchLabel(const wchar_t* api) {
    for (int i = 0; i < DL_NARCHES; i++) if (!_wcsicmp(api, DL_ARCHES[i].api)) return DL_ARCHES[i].label;
    return api;
}

// Made from a base edition by UUP dump's converter. The name is what its settings file calls them.
typedef struct { const wchar_t* key; const wchar_t* label; } DlVirtual;
static const DlVirtual DL_VIRTUAL[] = {
    { L"Education", L"Windows Education" }, { L"Enterprise", L"Windows Enterprise" },
    { L"ProfessionalEducation", L"Windows Pro Education" }, { L"ProfessionalWorkstation", L"Windows Pro Workstations" } };
#define DL_NVIRTUAL 4

typedef struct { wchar_t uuid[48], title[200], build[32]; } DlBuild;
typedef struct { wchar_t code[40], name[80]; BOOL checked; } DlEdition;
typedef struct { wchar_t code[20], name[80]; } DlLang;


// ---------- what can be built ----------
static int dlLangOrder(const void* a, const void* b) { return _wcsicmp(((const DlLang*)a)->name, ((const DlLang*)b)->name); }

// Reads the languages of a build. Fewer than one means it can't be made into a Windows image, whatever it's called.
static int dlReadLangs(const wchar_t* uuid, DlLang* out, wchar_t* err, int cch) {
    wchar_t q[128]; char* b;
    swprintf(q, 128, L"listlangs.php?id=%s", uuid);
    const char* r = dlApi(q, &b, err, cch);
    if (!r) return -1;
    const char* names = jGet(r, "langFancyNames");
    JIter it; const char* k; const char* v; int n = 0;
    if (jOpen(jGet(r, "langList"), &it)) {
        while (jNext(&it, &k, &v)) {
            wchar_t code[20];
            jText(v, code, 20);
            if (!code[0] || !wcscmp(code, L"neutral") || n >= DL_MAX_LANGS) continue;     // not a language anyone can install
            wcscpy(out[n].code, code);
            out[n].name[0] = 0;
            char key[40]; WideCharToMultiByte(CP_UTF8, 0, code, -1, key, 40, NULL, NULL);
            const char* nv = jGet(names, key);
            if (nv) jText(nv, out[n].name, 80);
            if (!out[n].name[0]) wcscpy(out[n].name, code);
            n++;
        }
    }
    free(b);
    if (n > 1) qsort(out, n, sizeof(DlLang), dlLangOrder);
    return n;
}

static int dlEditionOrder(const void* a, const void* b) { return _wcsicmp(((const DlEdition*)a)->name, ((const DlEdition*)b)->name); }

// N editions (Windows without its media features) aren't offered, at the user's request. One is known by its code
// being another listed edition's code plus an N, such as COREN beside CORE, so no list of them needs keeping.
static int dlDropNEditions(DlEdition* e, int n) {
    int kept = 0;
    for (int i = 0; i < n; i++) {
        size_t len = wcslen(e[i].code);
        BOOL isN = FALSE;
        if (len > 1 && (e[i].code[len - 1] == L'N' || e[i].code[len - 1] == L'n'))
            for (int k = 0; k < n && !isN; k++)
                isN = (k != i && wcslen(e[k].code) == len - 1 && !_wcsnicmp(e[k].code, e[i].code, len - 1));
        if (isN) continue;
        if (kept != i) memcpy(&e[kept], &e[i], sizeof(DlEdition));
        kept++;
    }
    return kept;
}

static int dlReadEditions(const wchar_t* uuid, const wchar_t* lang, DlEdition* out, wchar_t* err, int cch) {
    wchar_t q[160]; char* b;
    swprintf(q, 160, L"listeditions.php?id=%s&lang=%s", uuid, lang);
    const char* r = dlApi(q, &b, err, cch);
    if (!r) return -1;
    const char* names = jGet(r, "editionFancyNames");
    JIter it; const char* k; const char* v; int n = 0;
    if (jOpen(jGet(r, "editionList"), &it)) {
        while (jNext(&it, &k, &v) && n < DL_MAX_EDITIONS) {
            jText(v, out[n].code, 40);
            if (!out[n].code[0]) continue;
            out[n].checked = FALSE;
            out[n].name[0] = 0;
            char key[64]; WideCharToMultiByte(CP_UTF8, 0, out[n].code, -1, key, 64, NULL, NULL);
            const char* nv = jGet(names, key);
            if (nv) jText(nv, out[n].name, 80);
            if (!out[n].name[0]) wcscpy(out[n].name, out[n].code);
            n++;
        }
    }
    free(b);
    n = dlDropNEditions(out, n);
    if (n > 1) qsort(out, n, sizeof(DlEdition), dlEditionOrder);      // the API's order changes from call to call
    return n;
}

static BOOL dlValidBuildText(const wchar_t* s) {
    if (!iswdigit(s[0])) return FALSE;
    int dots = 0;
    for (; *s; s++) {
        if (*s == L'.') { if (++dots > 1 || !s[1]) return FALSE; }
        else if (*s < L'0' || *s > L'9') return FALSE;
    }
    return TRUE;
}
// "26300" matches any update of that build; "26300.9818" only that one.
static BOOL dlBuildMatches(const wchar_t* found, const wchar_t* typed) {
    if (wcschr(typed, L'.')) return !wcscmp(found, typed);
    size_t n = wcslen(typed);
    return !wcsncmp(found, typed, n) && (found[n] == L'.' || !found[n]);
}

// Finds a typed build in UUP dump's list. One number can match several products, such as Windows Server and Windows 11,
// so this returns every usable entry for the person to choose from: those offering Pro or Home first, newest first
// within that, judged by the editions the API lists and not by the titles.
// Returns how many it found, 0 if none is usable (err says why), or -1 if it failed.
#define DL_MAX_MATCHES 8
static int dlFindBuilds(const wchar_t* typed, const wchar_t* arch, DlBuild* out, wchar_t* err, int cch) {
    wchar_t q[128]; char* b;
    swprintf(q, 128, L"listid.php?search=%s&sortByDate=1", typed);
    const char* r = dlApi(q, &b, err, cch);
    if (!r) return -1;
    JIter it; const char* k; const char* v;
    int result = 0, listed = 0, nClient = 0;
    if (jOpen(jGet(r, "builds"), &it)) {
        while (result >= 0 && result < DL_MAX_MATCHES && jNext(&it, &k, &v)) {
            wchar_t a[16], build[32];
            jText(jGet(v, "arch"), a, 16);
            jText(jGet(v, "build"), build, 32);
            if (_wcsicmp(a, arch) || !dlBuildMatches(build, typed)) continue;
            listed++;
            DlBuild c; memset(&c, 0, sizeof(c));
            wcscpy(c.build, build);
            jText(jGet(v, "uuid"), c.uuid, 48);
            jText(jGet(v, "title"), c.title, 200);
            static DlLang langs[DL_MAX_LANGS];
            Sleep(DL_LIST_GAP_MS);
            int n = dlReadLangs(c.uuid, langs, err, cch);
            if (n < 0) { result = -1; break; }
            if (n == 0) continue;
            static DlEdition eds[DL_MAX_EDITIONS];
            const wchar_t* lang = langs[0].code;
            for (int i = 0; i < n; i++) if (!wcscmp(langs[i].code, L"en-us")) lang = langs[i].code;
            Sleep(DL_LIST_GAP_MS);
            int ne = dlReadEditions(c.uuid, lang, eds, err, cch);
            if (ne < 0) { result = -1; break; }
            if (ne == 0) continue;
            BOOL client = FALSE;
            for (int i = 0; i < ne; i++) if (!_wcsicmp(eds[i].code, L"PROFESSIONAL") || !_wcsicmp(eds[i].code, L"CORE")) client = TRUE;
            int at = client ? nClient++ : result;
            memmove(&out[at + 1], &out[at], sizeof(DlBuild) * (size_t)(result - at));
            out[at] = c;
            result++;
        }
    }
    free(b);
    if (result == 0) {
        if (listed) swprintf(err, cch, L"UUP dump lists %s for %s, but only as a partial update that can't become an ISO.", typed, dlArchLabel(arch));
        else swprintf(err, cch, L"UUP dump doesn't list build %s for %s. Check the number.", typed, dlArchLabel(arch));
    }
    return result;
}

// ---------- the job: where its files live ----------
// <Documents>\WCU_UUP, or <system drive>\WCU_UUP as a fallback: made at the start of each run and deleted at its end, whether it worked, failed or was
// cancelled. Elevated code runs tools from it, yet it sits in a folder the user, and so any program they run, can
// change. So it's owned by administrators and only they can write in it, and the job holds it open without delete
// sharing: while that handle is open, neither it nor any folder above it can be renamed, so nothing can swap in a
// folder of other tools under the same path.
static wchar_t g_dlRoot[MAX_PATH];
static HANDLE  g_dlRootH = INVALID_HANDLE_VALUE;

static void dlRootPath(wchar_t* out, const wchar_t* sub) {
    swprintf(out, MAX_PATH, L"%s%s%s", g_dlRoot, sub ? L"\\" : L"", sub ? sub : L"");
}

static void dlRemoveTree(const wchar_t* dir) {
    wchar_t* lp = longPath(dir);
    if (!lp) return;
    deleteContents(lp);
    RemoveDirectoryW(lp);
    free(lp);
}

// A folder, or only the link if a junction stands there: never what a link points to
static void dlRemoveFolderOrLink(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) { if (!RemoveDirectoryW(path)) DeleteFileW(path); return; }
    if (a & FILE_ATTRIBUTE_DIRECTORY) dlRemoveTree(path);
}

// The converter's ReadMe asks for "a simple spaceless path", so only plain letters, digits and . _ - are allowed,
// plus the ~ of short names such as DOCUME~1
static BOOL dlPathSimple(const wchar_t* p) {
    for (; *p; p++)
        if (!((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z') || (*p >= L'0' && *p <= L'9') || wcschr(L"\\:._-~", *p))) return FALSE;
    return TRUE;
}

static BOOL dlOwnedByAdmins(HANDLE h) {
    PSID owner = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) != ERROR_SUCCESS) return FALSE;
    BYTE ba[SECURITY_MAX_SID_SIZE]; DWORD baN = sizeof(ba);
    BOOL ok = owner && CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, ba, &baN) && EqualSid(owner, ba);
    LocalFree(sd);
    return ok;
}

// The folder the user chose for the work files (the Work folder row), or empty for the default. The window keeps it
// in the registry and sends it with each request; the helper sets it from the request.
#define DL_WORK_VALUE L"UupWorkFolder"
static wchar_t g_dlWorkBase[MAX_PATH];

// The places a work folder can be: [0] <chosen folder>\WCU_UUP, empty when none is chosen; [1] <Documents>\WCU_UUP,
// the default; and [2] <system drive>\WCU_UUP, the fallback the user chose for when Documents can't be used (a drive
// that isn't NTFS, a network folder, or a path with spaces on a drive without short names). An empty string for a
// place that can't be found. Cleanup looks in all of them.
#define DL_NPLACES 3
static void dlWorkPlaces(wchar_t places[DL_NPLACES][MAX_PATH + 16]) {
    wchar_t docs[MAX_PATH], sys[8] = L"C:";
    places[0][0] = places[1][0] = 0;
    if (g_dlWorkBase[0]) {
        size_t n = wcslen(g_dlWorkBase);
        swprintf(places[0], MAX_PATH + 16, L"%s%sWCU_UUP", g_dlWorkBase, n && g_dlWorkBase[n - 1] == L'\\' ? L"" : L"\\");
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs))) swprintf(places[1], MAX_PATH + 16, L"%s\\WCU_UUP", docs);
    GetEnvironmentVariableW(L"SystemDrive", sys, 8);
    swprintf(places[2], MAX_PATH + 16, L"%s\\WCU_UUP", sys);
}

// A chosen work folder as the request may carry it: empty, or a full path on a drive letter, nothing odd in it
static BOOL dlWorkBaseOk(const wchar_t* p) {
    if (!p[0]) return TRUE;
    if (wcslen(p) > MAX_PATH - 20 || !iswalpha(p[0]) || p[1] != L':' || p[2] != L'\\') return FALSE;
    if (wcspbrk(p, L"\"|<>*?/\r\n") || wcschr(p + 2, L':') || wcsstr(p, L"\\..")) return FALSE;
    return TRUE;
}

static BOOL dlRootInUse(const wchar_t* path);

// Why Documents (or the chosen folder) can't hold the work folder, or NULL if it can: DISM mounts the image there and
// the folder is made administrators-only, which needs NTFS on a local drive
static const wchar_t* dlDocsUnusable(const wchar_t* path) {
    if (!path[0]) return L"Windows didn't say where Documents is";
    if (!wcsncmp(path, L"\\\\", 2)) return L"it's a network folder";
    wchar_t vol[MAX_PATH], fs[32] = L"";
    if (!GetVolumePathNameW(path, vol, MAX_PATH)) return L"its drive couldn't be read";
    if (GetDriveTypeW(vol) == DRIVE_REMOTE) return L"it's on a network drive";
    if (GetDriveTypeW(vol) == DRIVE_CDROM) return L"it's on a CD/DVD drive";
    if (!GetVolumeInformationW(vol, NULL, 0, NULL, NULL, NULL, fs, 32)) return L"its drive couldn't be read";
    if (_wcsicmp(fs, L"NTFS")) return L"its drive isn't NTFS";
    return NULL;
}

// Makes the work folder at path. 0 on success; otherwise why is a short reason the caller can show.
static DWORD dlRootTry(const wchar_t* path, const wchar_t** why) {
    wchar_t fin[MAX_PATH + 8], shortP[MAX_PATH];
    *why = L"the folder couldn't be made";
    dlRemoveFolderOrLink(path);                 // left by a run that never reached its end, such as a power cut
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &sd, NULL))
        return GetLastError();
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    DWORD err = CreateDirectoryW(path, &sa) ? 0 : GetLastError();
    LocalFree(sd);
    if (err) return err;
    // Sharing is only enforced for a handle that can read the contents, so it lists the folder, or a rename would go through
    HANDLE h = CreateFileW(path, FILE_LIST_DIRECTORY | READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    // Between making it and opening it, it could have been renamed and replaced: a user can't make one owned by administrators
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !dlOwnedByAdmins(h)) {
        CloseHandle(h);
        return ERROR_ACCESS_DENIED;
    }
    DWORD n = GetFinalPathNameByHandleW(h, fin, MAX_PATH + 8, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const wchar_t* p = fin;
    if (n && n < MAX_PATH + 8 && !wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    if (!n || n >= MAX_PATH + 8 || !wcsncmp(p, L"UNC\\", 4) || !GetShortPathNameW(p, shortP, MAX_PATH) || !dlPathSimple(shortP)) {
        *why = L"the converter needs a path without spaces, and this drive doesn't give folders short names";
        CloseHandle(h);
        dlRemoveFolderOrLink(path);
        return ERROR_BAD_PATHNAME;
    }
    lstrcpynW(g_dlRoot, shortP, MAX_PATH);
    g_dlRootH = h;
    return 0;
}

// The chosen folder if there is one, with no fallback (it was chosen, often for its free space). Otherwise Documents
// first, and the system drive's root only when Documents can't be found or a build can't work there.
static DWORD dlRootEnsure(HANDLE out) {
    wchar_t places[DL_NPLACES][MAX_PATH + 16], line[MAX_PATH * 2 + 300];
    g_dlRoot[0] = 0;
    dlWorkPlaces(places);
    // Another build (from a second copy of the app) holds its folder open: never clear it from under that build
    for (int i = 0; i < DL_NPLACES; i++) if (places[i][0] && dlRootInUse(places[i])) {
        swprintf(line, MAX_PATH * 2 + 300, L"Error: another build is running in %s. Wait for it to finish, or cancel it.\r\n", places[i]);
        repWrite(out, line);
        return ERROR_BUSY;
    }
    const wchar_t* why;
    DWORD e = ERROR_PATH_NOT_FOUND;
    if (places[0][0]) {
        if (GetFileAttributesW(g_dlWorkBase) == INVALID_FILE_ATTRIBUTES) why = L"the folder isn't there any more";
        else if (!(why = dlDocsUnusable(places[0]))) e = dlRootTry(places[0], &why);
        if (!e) {
            swprintf(line, MAX_PATH * 2 + 300, L"Work files go in %s\r\n", places[0]);
            repWrite(out, line);
            return 0;
        }
        swprintf(line, MAX_PATH * 2 + 300, L"Error: the work folder %s can't be used: %s. Choose another one under Work folder.\r\n", g_dlWorkBase, why);
        repWrite(out, line);
        return e;
    }
    why = dlDocsUnusable(places[1]);
    if (!why) {
        e = dlRootTry(places[1], &why);
        if (!e) return 0;
    }
    swprintf(line, MAX_PATH * 2 + 300, L"Documents can't be used for the work files (%s), so they go in %s.\r\n", why, places[2]);
    repWrite(out, line);
    e = dlRootTry(places[2], &why);
    if (e) {
        swprintf(line, MAX_PATH * 2 + 300, L"Error: %s can't be used either: %s.\r\n", places[2], why);
        repWrite(out, line);
    }
    return e;
}

// The end of every run: the contents go while the folder is still held, so the path is still the real folder
static void dlRootRemove(HANDLE out) {
    if (!g_dlRoot[0]) return;
    // a mounted image is never deleted from under DISM: if one couldn't be discarded, the folder waits for Clean up
    wchar_t mnt[MAX_PATH + 16];
    swprintf(mnt, MAX_PATH + 16, L"%s\\MountUUP", g_dlRoot);
    if (GetFileAttributesW(mnt) != INVALID_FILE_ATTRIBUTES && !RemoveDirectoryW(mnt)) {
        repWrite(out, L"The work folder was kept, because a Windows image is still mounted in it. Restart the PC, then press Clean up leftovers.\r\n");
        if (g_dlRootH != INVALID_HANDLE_VALUE) { CloseHandle(g_dlRootH); g_dlRootH = INVALID_HANDLE_VALUE; }
        g_dlRoot[0] = 0;
        return;
    }
    repWrite(out, L"Deleting the work folder\r\n");
    wchar_t* lp = longPath(g_dlRoot);
    if (lp) { deleteContents(lp); free(lp); }
    if (g_dlRootH != INVALID_HANDLE_VALUE) { CloseHandle(g_dlRootH); g_dlRootH = INVALID_HANDLE_VALUE; }
    dlRemoveFolderOrLink(g_dlRoot);             // empty by now, or a link if it was swapped after the handle closed
    g_dlRoot[0] = 0;
}


// ---------- the job: UUP dump's tools ----------
typedef struct { wchar_t url[300], name[80], sha256[65]; } DlTool;
enum { DL_TOOL_7ZR, DL_TOOL_CONVERTER, DL_TOOL_ARIA2, DL_NTOOLS };

// Used only if UUP dump's manifests can't be read or no longer look the way they should.
static const DlTool DL_TOOL_DEFAULTS[DL_NTOOLS] = {
    { L"https://uupdump.net/misc/7zr.exe", L"7zr.exe", L"72c98287b2e8f85ea7bb87834b6ce1ce7ce7f41a8c97a81b307d4d4bf900922b" },
    { L"https://uupdump.net/misc/uup-converter-wimlib-v129.7z", L"uup-converter-wimlib.7z", L"b2c65f5bbc357605aec8cad1739e15bff4b2c5a44daad19eb45d26b46a0ea8dd" },
    { L"https://uupdump.net/misc/aria2c.exe", L"aria2c.exe", L"b9cd71b275af11b63c33457b0f43f2f2675937070c563e195f223efd7fa4c74b" },
};

// UUP dump's FAQ says uupdump.net is its only official address, so tools are taken from nowhere else.
static BOOL dlToolUrlOk(const wchar_t* url) {
    return !wcsncmp(url, L"https://uupdump.net/", 20) || !wcsncmp(url, L"https://git.uupdump.net/", 24);
}

// converter_windows holds blocks like:  <url>\n  out=<name>\n  checksum=sha-256=<hash>
static BOOL dlParseConverterManifest(const char* text, DlTool* seven, DlTool* conv) {
    BOOL got7 = FALSE, gotC = FALSE;
    DlTool cur; memset(&cur, 0, sizeof(cur));
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        int n = (int)(e - p);
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
        const char* s = p;
        while (n > 0 && *s == ' ') { s++; n--; }
        wchar_t line[400]; int w = n > 0 ? MultiByteToWideChar(CP_UTF8, 0, s, n, line, 399) : 0;
        line[w] = 0;
        if (!wcsncmp(line, L"https://", 8)) { memset(&cur, 0, sizeof(cur)); lstrcpynW(cur.url, line, 300); }
        else if (!wcsncmp(line, L"out=", 4)) lstrcpynW(cur.name, line + 4, 80);
        else if (!wcsncmp(line, L"checksum=sha-256=", 17)) {
            lstrcpynW(cur.sha256, line + 17, 65);
            if (cur.url[0] && cur.name[0] && dlToolUrlOk(cur.url) && dlHex64(cur.sha256) && !wcspbrk(cur.name, L"\\/:*?\"<>|")) {
                size_t len = wcslen(cur.name);
                if (!_wcsicmp(cur.name, L"7zr.exe")) { *seven = cur; got7 = TRUE; }
                else if (len > 3 && !_wcsicmp(cur.name + len - 3, L".7z")) { *conv = cur; gotC = TRUE; }
            }
        }
        p = *e ? e + 1 : e;
    }
    return got7 && gotC;
}

// get_aria2.ps1 holds:  $url = '<url>';  and  $hash = '<hash>';
static BOOL dlPsValue(const char* text, const char* var, wchar_t* out, int cch) {
    const char* p = strstr(text, var);
    if (!p) return FALSE;
    p = strchr(p, '\'');
    if (!p) return FALSE;
    const char* e = strchr(++p, '\'');
    if (!e || e - p >= cch) return FALSE;
    int w = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cch - 1);
    out[w] = 0;
    return w > 0;
}
static BOOL dlParseAriaManifest(const char* text, DlTool* aria) {
    DlTool t; memset(&t, 0, sizeof(t));
    if (!dlPsValue(text, "$url", t.url, 300) || !dlPsValue(text, "$hash", t.sha256, 65)) return FALSE;
    if (!dlToolUrlOk(t.url) || !dlHex64(t.sha256)) return FALSE;
    wcscpy(t.name, L"aria2c.exe");
    *aria = t;
    return TRUE;
}

static void dlReadManifests(DlTool* tools, HANDLE out) {
    memcpy(tools, DL_TOOL_DEFAULTS, sizeof(DL_TOOL_DEFAULTS));
    char* b = NULL; DWORD status = 0;
    BOOL ok = FALSE;
    if (!dlHttp(DL_MANIFEST_CONVERTER, &b, NULL, &status) && status == 200 && b) {
        DlTool s7, c;
        if (dlParseConverterManifest(b, &s7, &c)) { tools[DL_TOOL_7ZR] = s7; tools[DL_TOOL_CONVERTER] = c; ok = TRUE; }
    }
    free(b); b = NULL;
    if (!ok) repWrite(out, L"Couldn't read UUP dump's converter manifest, so using the details built into this app.\r\n");
    ok = FALSE;
    if (!dlHttp(DL_MANIFEST_ARIA2, &b, NULL, &status) && status == 200 && b) {
        DlTool a;
        if (dlParseAriaManifest(b, &a)) { tools[DL_TOOL_ARIA2] = a; ok = TRUE; }
    }
    free(b);
    if (!ok) repWrite(out, L"Couldn't read UUP dump's aria2 manifest, so using the details built into this app.\r\n");
    const wchar_t* slash = wcsrchr(tools[DL_TOOL_CONVERTER].url, L'/');
    wchar_t line[200];
    swprintf(line, 200, L"Converter: %s\r\n", slash ? slash + 1 : tools[DL_TOOL_CONVERTER].url);
    repWrite(out, line);
}

// Puts a tool in the tools folder, unless a copy with the right hash is already there.
static BOOL dlEnsureTool(const DlTool* t, HANDLE out, wchar_t* path /* MAX_PATH */) {
    wchar_t dir[MAX_PATH], hex[65], tmp[MAX_PATH + 8], line[400];
    dlRootPath(dir, L"tools");
    CreateDirectoryW(dir, NULL);
    swprintf(path, MAX_PATH, L"%s\\%s", dir, t->name);
    if (dlSha256(path, hex) && !_wcsicmp(hex, t->sha256)) return TRUE;
    swprintf(line, 400, L"Downloading %s\r\n", t->name);
    repWrite(out, line);
    swprintf(tmp, MAX_PATH + 8, L"%s.tmp", path);
    DWORD status = 0, e = dlHttp(t->url, NULL, tmp, &status);
    if (e || status != 200) {     // the repo can be ahead of the website, so the same file is tried there too
        const wchar_t* slash = wcsrchr(t->url, L'/');
        wchar_t repoUrl[400];
        swprintf(repoUrl, 400, L"https://git.uupdump.net/uup-dump/misc/raw/branch/master/%s", slash ? slash + 1 : t->name);
        status = 0;
        e = dlHttp(repoUrl, NULL, tmp, &status);
    }
    if (e || status != 200) {
        swprintf(line, 400, L"Error: couldn't download %s (error %lu, HTTP %lu).\r\n", t->name, e, status);
        repWrite(out, line);
        DeleteFileW(tmp);
        return FALSE;
    }
    if (!dlSha256(tmp, hex) || _wcsicmp(hex, t->sha256)) {
        swprintf(line, 400, L"Error: %s doesn't match the checksum UUP dump lists for it, so it wasn't used.\r\n", t->name);
        repWrite(out, line);
        DeleteFileW(tmp);
        return FALSE;
    }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        swprintf(line, 400, L"Error: couldn't save %s (error %lu).\r\n", t->name, GetLastError());
        repWrite(out, line);
        DeleteFileW(tmp);
        return FALSE;
    }
    return TRUE;
}

// Unpacks the converter into a folder named after its hash, so a new version starts clean. Removes older ones.
static BOOL dlEnsureConverter(const DlTool* conv, const wchar_t* archive, const wchar_t* seven, HANDLE cancel, HANDLE out, wchar_t* dir /* MAX_PATH */) {
    wchar_t sub[32], root[MAX_PATH], script[MAX_PATH + 32], pat[MAX_PATH + 8], old[MAX_PATH + 300];
    wchar_t hash12[13]; lstrcpynW(hash12, conv->sha256, 13);
    swprintf(sub, 32, L"conv-%s", hash12);
    dlRootPath(dir, sub);
    dlRootPath(root, NULL);
    swprintf(pat, MAX_PATH + 8, L"%s\\conv-*", root);
    WIN32_FIND_DATAW fd;
    HANDLE ff = FindFirstFileW(pat, &fd);
    if (ff != INVALID_HANDLE_VALUE) {
        do {
            if (_wcsicmp(fd.cFileName, sub)) { swprintf(old, MAX_PATH + 300, L"%s\\%s", root, fd.cFileName); dlRemoveTree(old); }
        } while (FindNextFileW(ff, &fd));
        FindClose(ff);
    }
    swprintf(script, MAX_PATH + 32, L"%s\\convert-UUP.cmd", dir);
    if (GetFileAttributesW(script) != INVALID_FILE_ATTRIBUTES) return TRUE;

    CreateDirectoryW(dir, NULL);
    wchar_t cmd[MAX_PATH * 3 + 64]; DWORD code = 1;
    swprintf(cmd, MAX_PATH * 3 + 64, L"\"%s\" x \"%s\" \"-o%s\" -y -bso0 -bsp0", seven, archive, dir);
    DWORD e = repRunOnConsole(out, seven, cmd, root, cancel, &code);
    if (e || code || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        wchar_t line[200];
        swprintf(line, 200, L"Error: couldn't unpack the converter (error %lu, exit code %lu).\r\n", e, code);
        repWrite(out, line);
        return FALSE;
    }
    return TRUE;
}

// ---------- the job: the file list ----------
typedef struct { wchar_t name[160]; char url[3000], sha256[65], sha1[41]; unsigned long long size; } DlFile;

static BOOL dlFileNameOk(const wchar_t* s) {
    if (!s[0] || wcschr(s, L'\\') || wcschr(s, L'/') || wcsstr(s, L"..") || wcschr(s, L':')) return FALSE;
    for (; *s; s++) if (*s < 32) return FALSE;
    return TRUE;
}
static void dlNarrow(const wchar_t* w, char* out, int cch) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cch, NULL, NULL);
    if (!n) out[0] = 0;
}
// Adds an edition's files to the list, keeping one copy of those shared between editions. Returns FALSE with err set.
static BOOL dlAddEditionFiles(const char* resp, DlFile* files, int* count, wchar_t* err, int cch) {
    JIter it; const char* k; const char* v;
    if (!jOpen(jGet(resp, "files"), &it) || !it.obj) { swprintf(err, cch, L"UUP dump's reply had no file list."); return FALSE; }
    wchar_t url[3000], name[160], h[80];
    while (jNext(&it, &k, &v)) {
        jText(k, name, 160);
        BOOL seen = FALSE;
        for (int i = 0; i < *count && !seen; i++) seen = !_wcsicmp(files[i].name, name);
        if (seen) continue;
        if (*count >= DL_MAX_FILES) { swprintf(err, cch, L"The file list is longer than expected."); return FALSE; }
        // These names become paths and lines in aria2's list, so nothing unusual is let through
        if (!dlFileNameOk(name)) { swprintf(err, cch, L"UUP dump listed a file with an unsafe name: %s", name); return FALSE; }
        jText(jGet(v, "url"), url, 3000);
        // Microsoft's links are often plain http; the checksum below is what proves each file is right
        if ((wcsncmp(url, L"https://", 8) && wcsncmp(url, L"http://", 7)) || wcspbrk(url, L"\r\n ")) { swprintf(err, cch, L"UUP dump gave no usable download link for %s.", name); return FALSE; }
        DlFile* f = &files[*count];
        memset(f, 0, sizeof(*f));
        wcscpy(f->name, name);
        dlNarrow(url, f->url, 3000);
        jText(jGet(v, "sha256"), h, 80);
        if (dlHex64(h)) dlNarrow(h, f->sha256, 65);
        jText(jGet(v, "sha1"), h, 80);
        if (wcslen(h) == 40) dlNarrow(h, f->sha1, 41);
        if (!f->sha256[0] && !f->sha1[0]) { swprintf(err, cch, L"UUP dump gave no checksum for %s.", name); return FALSE; }
        jText(jGet(v, "size"), h, 80);
        f->size = _wcstoui64(h, NULL, 10);
        (*count)++;
    }
    return TRUE;
}

static int __cdecl dlBySize(const void* a, const void* b) {
    const DlFile* x = *(const DlFile* const*)a; const DlFile* y = *(const DlFile* const*)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return x < y ? -1 : x > y;
}
// Smallest first: their links expire soonest, and behind the big files they'd wait too long on a slow connection.
// skip: NULL, or TRUE for files left out of this list (already finished)
static BOOL dlWriteAriaList(const wchar_t* path, const DlFile* files, int n, const BOOL* skip) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE; DWORD wr;
    char* line = (char*)malloc(4096);
    const DlFile** order = (const DlFile**)malloc(((size_t)n + 1) * sizeof(*order));
    int m = 0;
    for (int i = 0; order && i < n; i++) if (!skip || !skip[i]) order[m++] = &files[i];
    if (order) qsort(order, (size_t)m, sizeof(*order), dlBySize);
    for (int i = 0; ok && line && i < m; i++) {
        const DlFile* d = order[i];
        char name[480]; dlNarrow(d->name, name, 480);
        int len = d->sha256[0]
            ? snprintf(line, 4096, "%s\n  out=%s\n  checksum=sha-256=%s\n\n", d->url, name, d->sha256)
            : snprintf(line, 4096, "%s\n  out=%s\n  checksum=sha-1=%s\n\n", d->url, name, d->sha1);
        ok = len > 0 && len < 4096 && WriteFile(f, line, (DWORD)len, &wr, NULL);
    }
    if (!line || !order) ok = FALSE;
    free(line); free(order);
    CloseHandle(f);
    return ok;
}

// ---------- the preinstalled apps a build offers ----------
// Each edition's metadata names its apps by feature ID, such as Microsoft.WindowsStore_8wekyb3d8bbwe, and that's
// what the converter's CustomAppsList.txt matches against. The file names in UUP dump's list aren't always the same
// (Teams is MSTeamsX64_... there and MSTeams_... in the metadata), so the IDs are read from the metadata itself.
#define DL_MAX_APPS 128
typedef struct { wchar_t id[96], name[80]; BOOL checked, fixed; } DlApp;

// Always installed and never offered for unticking, at the user's request: the Store and what it needs to buy,
// Windows Security, Feedback Hub, and everything Xbox (a new Xbox app is caught by its name).
static BOOL dlAppFixed(const wchar_t* id) {
    static const wchar_t* FIXED[] = { L"Microsoft.WindowsStore_", L"Microsoft.StorePurchaseApp_", L"Microsoft.SecHealthUI_",
                                      L"Microsoft.WindowsFeedbackHub_", L"Microsoft.GamingApp_" };
    for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++) if (!_wcsnicmp(id, FIXED[i], wcslen(FIXED[i]))) return TRUE;
    for (const wchar_t* p = id; *p && *p != L'_'; p++) if (!_wcsnicmp(p, L"xbox", 4)) return TRUE;
    return FALSE;
}

static BOOL dlAppIdOk(const wchar_t* s) {
    if (!s[0] || wcslen(s) >= 96) return FALSE;
    for (; *s; s++)
        if (!((*s >= L'0' && *s <= L'9') || (*s >= L'a' && *s <= L'z') || (*s >= L'A' && *s <= L'Z') || *s == L'.' || *s == L'_' || *s == L'-')) return FALSE;
    return TRUE;
}

// What people know an app as. A new app keeps its ID, without the publisher's prefix and suffix.
static void dlAppName(const wchar_t* id, wchar_t* out, int cch) {
    static const struct { const wchar_t* key; const wchar_t* name; } NAMES[] = {
        { L"Microsoft.WindowsStore", L"Microsoft Store" }, { L"Microsoft.StorePurchaseApp", L"Store Purchase App" },
        { L"Microsoft.BingNews", L"News" }, { L"Microsoft.BingSearch", L"Bing Search" }, { L"Microsoft.BingWeather", L"Weather" },
        { L"Microsoft.MicrosoftSolitaireCollection", L"Solitaire Collection" }, { L"Microsoft.ZuneMusic", L"Media Player" },
        { L"Microsoft.ZuneVideo", L"Movies & TV" }, { L"Microsoft.WindowsAlarms", L"Clock" },
        { L"Microsoft.ApplicationCompatibilityEnhancements", L"App Compatibility Enhancements" },
        { L"Microsoft.WindowsCalculator", L"Calculator" }, { L"Microsoft.WindowsNotepad", L"Notepad" },
        { L"Microsoft.WindowsTerminal", L"Terminal" }, { L"Microsoft.WindowsFeedbackHub", L"Feedback Hub" },
        { L"Microsoft.GetHelp", L"Get Help" }, { L"Microsoft.Getstarted", L"Tips" }, { L"Microsoft.WindowsSoundRecorder", L"Sound Recorder" },
        { L"Microsoft.MicrosoftOfficeHub", L"Microsoft 365 Copilot (Office)" }, { L"Microsoft.ScreenSketch", L"Snipping Tool" },
        { L"Microsoft.MicrosoftStickyNotes", L"Sticky Notes" }, { L"Microsoft.Todos", L"Microsoft To Do" },
        { L"Microsoft.XboxSpeechToTextOverlay", L"Xbox Speech to Text Overlay" }, { L"Microsoft.GamingApp", L"Xbox" },
        { L"Microsoft.XboxGamingOverlay", L"Xbox Game Bar" }, { L"Microsoft.XboxIdentityProvider", L"Xbox Identity Provider" },
        { L"Microsoft.XboxGameOverlay", L"Xbox Game Overlay" }, { L"Microsoft.Xbox.TCUI", L"Xbox Live In-Game Experience" },
        { L"Microsoft.Windows.Photos", L"Photos" }, { L"Microsoft.PowerAutomateDesktop", L"Power Automate" },
        { L"Microsoft.WindowsCamera", L"Camera" }, { L"Microsoft.DesktopAppInstaller", L"App Installer (winget)" },
        { L"Microsoft.Paint", L"Paint" }, { L"Microsoft.WebMediaExtensions", L"Web Media Extensions" },
        { L"Microsoft.RawImageExtension", L"Raw Image Extension" }, { L"Microsoft.HEIFImageExtension", L"HEIF Image Extensions" },
        { L"Microsoft.HEVCVideoExtension", L"HEVC Video Extensions" }, { L"Microsoft.VP9VideoExtensions", L"VP9 Video Extensions" },
        { L"Microsoft.AV1VideoExtension", L"AV1 Video Extension" }, { L"Microsoft.AVCEncoderVideoExtension", L"AVC Encoder Video Extension" },
        { L"Microsoft.MPEG2VideoExtension", L"MPEG-2 Video Extension" }, { L"Microsoft.WebpImageExtension", L"WebP Image Extension" },
        { L"MicrosoftWindows.Client.WebExperience", L"Widgets" }, { L"MicrosoftWindows.CrossDevice", L"Cross Device Experience Host" },
        { L"Microsoft.SecHealthUI", L"Windows Security" }, { L"Clipchamp.Clipchamp", L"Clipchamp" },
        { L"MicrosoftCorporationII.QuickAssist", L"Quick Assist" }, { L"Microsoft.OutlookForWindows", L"Outlook" },
        { L"MSTeams", L"Microsoft Teams" }, { L"Microsoft.Windows.DevHome", L"Dev Home" },
        { L"Microsoft.MicrosoftPCManager", L"PC Manager" }, { L"Microsoft.YourPhone", L"Phone Link" },
        { L"Microsoft.Whiteboard", L"Whiteboard" }, { L"MicrosoftCorporationII.MicrosoftFamily", L"Microsoft Family" },
        { L"Microsoft.549981C3F5F10", L"Cortana" }, { L"Microsoft.Copilot", L"Copilot" }, { L"Microsoft.WindowsMaps", L"Maps" },
        { L"Microsoft.People", L"People" }, { L"microsoft.windowscommunicationsapps", L"Mail and Calendar" },
        { L"Microsoft.MicrosoftEdge.Stable", L"Microsoft Edge" }, { L"Microsoft.OneDriveSync", L"OneDrive" },
    };
    wchar_t base[96]; lstrcpynW(base, id, 96);
    wchar_t* us = wcsrchr(base, L'_');
    if (us) *us = 0;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (!_wcsicmp(base, NAMES[i].key)) { lstrcpynW(out, NAMES[i].name, cch); return; }
    static const wchar_t* PREFIX[] = { L"MicrosoftCorporationII.", L"MicrosoftWindows.", L"Microsoft." };
    const wchar_t* p = base;
    for (int i = 0; i < 3; i++) { size_t n = wcslen(PREFIX[i]); if (!_wcsnicmp(p, PREFIX[i], n) && p[n]) { p += n; break; } }
    lstrcpynW(out, p, cch);
}

// Adds the apps an edition's metadata lists in its PreinstalledApps group, once each. Returns the new count.
static int dlParseCompDbApps(const char* xml, DlApp* list, int n, int max) {
    for (const char* p = strstr(xml, "<Feature "); p && n < max; p = strstr(p + 1, "<Feature ")) {
        const char* end = strchr(p, '>');
        if (!end) break;
        const char* g = strstr(p, "Group=\"PreinstalledApps\"");
        const char* f = strstr(p, "FeatureID=\"");
        if (!g || g > end || !f || f > end) continue;
        f += 11;
        const char* q = strchr(f, '"');
        if (!q || q > end || q - f >= 96) continue;
        wchar_t id[96]; int k = 0;
        for (const char* c = f; c < q; c++) id[k++] = (wchar_t)(unsigned char)*c;
        id[k] = 0;
        if (!dlAppIdOk(id)) continue;
        BOOL seen = FALSE;
        for (int i = 0; i < n && !seen; i++) seen = !_wcsicmp(list[i].id, id);
        if (seen) continue;
        memset(&list[n], 0, sizeof(DlApp));
        wcscpy(list[n].id, id);
        dlAppName(id, list[n].name, 80);
        list[n].checked = TRUE;
        list[n].fixed = dlAppFixed(id);
        n++;
    }
    return n;
}


// The apps a build installs in the given editions (codes as UUP dump lists them, separated by ;), for one language.
// Reads the build's metadata file (a few MB). Returns how many, 0 if the build has none, -1 if they couldn't be read.
static int dlReadBuildApps(const wchar_t* uuid, const wchar_t* lang, const wchar_t* editions, DlApp* out, int max, wchar_t* err, int cch) {
    wchar_t q[200]; char* b = NULL;
    swprintf(q, 200, L"get.php?id=%s&lang=neutral&edition=app", uuid);
    const char* r = dlApi(q, &b, err, cch);
    if (!r) return (wcsstr(err, L"UNSUPPORTED_COMBINATION") || wcsstr(err, L"EMPTY_FILELIST")) ? 0 : -1;
    wchar_t url[3000] = L"", sha256[80] = L"", sha1[80] = L"";
    JIter it; const char* k; const char* v;
    if (jOpen(jGet(r, "files"), &it)) {
        const wchar_t* tail = L".AggregatedMetadata.cab";
        size_t tl = wcslen(tail);
        while (jNext(&it, &k, &v)) {
            wchar_t name[160]; jText(k, name, 160);
            size_t n = wcslen(name);
            if (n <= tl || _wcsicmp(name + n - tl, tail)) continue;
            jText(jGet(v, "url"), url, 3000);
            jText(jGet(v, "sha256"), sha256, 80);
            jText(jGet(v, "sha1"), sha1, 80);
            break;
        }
    }
    free(b);
    BOOL use256 = dlHex64(sha256);
    if (!url[0] || (!use256 && wcslen(sha1) != 40)) { swprintf(err, cch, L"UUP dump didn't list this build's app metadata."); return -1; }

    wchar_t dir[MAX_PATH], cab[MAX_PATH + 16], args[MAX_PATH * 3], pat[MAX_PATH + 80], hex[65];
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%swcu-apps-%lu-%llu", tmp, GetCurrentProcessId(), GetTickCount64());
    CreateDirectoryW(dir, NULL);
    swprintf(cab, MAX_PATH + 16, L"%s\\meta.cab", dir);
    int result = -1;
    DWORD status = 0, e = dlHttpEx(url, NULL, cab, &status, TRUE);
    if (e || status != 200) {
        swprintf(err, cch, L"Couldn't download this build's app list (error %lu, HTTP %lu).", e, status);
    } else if (!dlHashFile(cab, use256 ? CALG_SHA_256 : CALG_SHA1, hex) || _wcsicmp(hex, use256 ? sha256 : sha1)) {
        swprintf(err, cch, L"This build's app list didn't match its checksum, so it wasn't used.");
    } else {
        // DesktopTargetCompDB_<edition>_<language>.xml.cab, each a cab of its own holding one XML file
        swprintf(args, MAX_PATH * 3, L"\"%s\" -F:DesktopTargetCompDB_*_%s.xml.cab \"%s\"", cab, lang, dir);
        dlExpand(args);
        swprintf(pat, MAX_PATH + 80, L"%s\\DesktopTargetCompDB_*.xml.cab", dir);
        WIN32_FIND_DATAW fd;
        HANDLE ff = FindFirstFileW(pat, &fd);
        int n = 0, read = 0;
        size_t pre = wcslen(L"DesktopTargetCompDB_"), ll = wcslen(lang);
        if (ff != INVALID_HANDLE_VALUE) {
            do {
                // only the editions this build offers: <edition> sits between the prefix and _<language>.xml.cab
                wchar_t ed[64]; size_t fl = wcslen(fd.cFileName), tail = ll + wcslen(L"_.xml.cab");
                if (fl <= pre + tail || fl - pre - tail >= 64) continue;
                wcsncpy(ed, fd.cFileName + pre, fl - pre - tail); ed[fl - pre - tail] = 0;
                BOOL wanted = FALSE;
                wchar_t list[400]; lstrcpynW(list, editions, 400);
                wchar_t* ctx = NULL;
                for (wchar_t* t = wcstok(list, L";", &ctx); t && !wanted; t = wcstok(NULL, L";", &ctx)) wanted = !_wcsicmp(t, ed);
                if (!wanted) continue;
                wchar_t inner[MAX_PATH + 300], xmlPath[MAX_PATH + 32];
                swprintf(inner, MAX_PATH + 300, L"%s\\%s", dir, fd.cFileName);
                swprintf(xmlPath, MAX_PATH + 32, L"%s\\edition.xml", dir);
                DeleteFileW(xmlPath);
                swprintf(args, MAX_PATH * 3, L"\"%s\" \"%s\"", inner, xmlPath);
                dlExpand(args);
                char* xml = dlReadSmallFile(xmlPath, 64u << 20);
                if (xml) { n = dlParseCompDbApps(xml, out, n, max); read++; free(xml); }
            } while (FindNextFileW(ff, &fd));
            FindClose(ff);
        }
        if (read) {
            for (int i = 1; i < n; i++)              // by name
                for (int j = i; j > 0 && _wcsicmp(out[j].name, out[j - 1].name) < 0; j--) { DlApp t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
            result = n;
        } else {
            swprintf(err, cch, L"Couldn't read the app list from this build's metadata.");
        }
    }
    dlRemoveTree(dir);
    return result;
}

// What the window asks for: uuid|lang|EDITION;EDITION|Virtual,Virtual|desktop|cancel event name|apps. dlSpecParse
// checks it, because it arrives on an elevated command line. apps: empty for every app the build has, "-" for none,
// or the IDs of the ticked ones separated by commas.
#define DL_APPS_CCH 6000
typedef struct { wchar_t uuid[48], lang[20], editions[400], virt[300], desktop[MAX_PATH], cancel[64], apps[DL_APPS_CCH], work[MAX_PATH]; } DlSpec;

// ---------- the job: the converter's settings ----------
typedef struct { const char* key; char value[300]; BOOL found; } DlIniSet;

// Replaces "key = anything" lines. Works on the bytes of the file the converter ships with, so a new version's
// other settings stay as they were. Returns the new text, or NULL if a setting we rely on no longer exists,
// because without it the converter would stop and wait for a person to answer, with nobody there to.
static char* dlIniApply(const char* text, DlIniSet* sets, int n, wchar_t* missing, int cch) {
    size_t cap = strlen(text) + 4096;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    const char* p = text;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        DlIniSet* hit = NULL;
        for (int i = 0; i < n && !hit; i++) {
            size_t kl = strlen(sets[i].key);
            if (_strnicmp(p, sets[i].key, kl)) continue;
            const char* q = p + kl;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') hit = &sets[i];
        }
        if (hit) {
            hit->found = TRUE;
            len += (size_t)snprintf(out + len, cap - len, "%s=%s\r\n", hit->key, hit->value);
        } else {
            size_t ln = (size_t)(e - p);
            memcpy(out + len, p, ln); len += ln;
            if (*e) out[len++] = '\n';
        }
        p = *e ? e + 1 : e;
    }
    out[len] = 0;
    for (int i = 0; i < n; i++) {
        if (sets[i].found) continue;
        MultiByteToWideChar(CP_UTF8, 0, sets[i].key, -1, missing, cch);
        free(out);
        return NULL;
    }
    return out;
}

// The converter adds only the apps CustomAppsList2.txt names when CustomList is on, and prefers that file to the
// CustomAppsList.txt it ships with. Written for a partial choice, removed otherwise.
static BOOL dlWriteAppsList(const wchar_t* conv, const wchar_t* apps, HANDLE out) {
    wchar_t path[MAX_PATH + 32];
    swprintf(path, MAX_PATH + 32, L"%s\\CustomAppsList2.txt", conv);
    BOOL custom = apps[0] && wcscmp(apps, L"-");
    if (!custom) { DeleteFileW(path); return TRUE; }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { repWrite(out, L"Error: couldn't write the list of apps to add.\r\n"); return FALSE; }
    BOOL ok = TRUE; DWORD wr;
    const char* head = "### The apps ticked in Windows Chosen Utilities\r\n";
    ok = WriteFile(f, head, (DWORD)strlen(head), &wr, NULL);
    wchar_t list[DL_APPS_CCH]; lstrcpynW(list, apps, DL_APPS_CCH);
    wchar_t* ctx = NULL; int n = 0;
    for (wchar_t* id = wcstok(list, L",", &ctx); id && ok; id = wcstok(NULL, L",", &ctx)) {
        char a[128]; dlNarrow(id, a, 120);
        strcat(a, "\r\n");
        ok = WriteFile(f, a, (DWORD)strlen(a), &wr, NULL);
        n++;
    }
    CloseHandle(f);
    if (!ok) { repWrite(out, L"Error: couldn't write the list of apps to add.\r\n"); return FALSE; }
    wchar_t line[120]; swprintf(line, 120, L"Adding the %d apps ticked.\r\n", n);
    repWrite(out, line);
    return TRUE;
}

// dropPro: Pro wasn't ticked, only extra editions made from it, so the converter deletes Pro once they're built.
static BOOL dlWriteConverterConfig(const wchar_t* conv, const wchar_t* virt, const wchar_t* apps, BOOL dropPro, HANDLE out) {
    wchar_t path[MAX_PATH + 32], line[300];
    swprintf(path, MAX_PATH + 32, L"%s\\ConvertConfig.ini", conv);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { repWrite(out, L"Error: the converter has no ConvertConfig.ini any more, so it can't run unattended.\r\n"); return FALSE; }
    DWORD size = GetFileSize(f, NULL), got = 0;
    char* text = (char*)malloc((size_t)size + 1);
    if (!text || !ReadFile(f, text, size, &got, NULL)) got = 0;
    CloseHandle(f);
    if (!text) return FALSE;
    text[got] = 0;

    char virtA[300]; dlNarrow(virt, virtA, 300);
    BOOL hasVirt = virtA[0] != 0;
    DlIniSet sets[] = {
        { "AutoStart", "1" }, { "AddUpdates", "1" }, { "Cleanup", "1" }, { "ResetBase", "0" }, { "AutoExit", "1" },
        { "SkipISO", "0" }, { "StartVirtual", "0" }, { "vAutoStart", "1" }, { "vDeleteSource", "0" }, { "vSkipISO", "0" }, { "vAutoEditions", "" },
        { "SkipApps", "0" }, { "CustomList", "0" },
    };
    int n = (int)(sizeof(sets) / sizeof(sets[0]));
    if (hasVirt) strcpy(sets[6].value, "1");
    if (hasVirt && dropPro) strcpy(sets[8].value, "1");          // Pro was only fetched to build the others from
    strcpy(sets[10].value, virtA);
    if (!wcscmp(apps, L"-")) strcpy(sets[11].value, "1");      // no apps at all
    else if (apps[0]) strcpy(sets[12].value, "1");             // only those in CustomAppsList2.txt
    if (!dlWriteAppsList(conv, apps, out)) { free(text); return FALSE; }

    wchar_t missing[64] = L"";
    char* result = dlIniApply(text, sets, n, missing, 64);
    free(text);
    if (!result) {
        swprintf(line, 300, L"Error: the converter no longer has the \"%s\" setting, so it could stop and wait for an answer. Nothing was converted.\r\n", missing);
        repWrite(out, line);
        return FALSE;
    }
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok = FALSE; DWORD wr;
    if (f != INVALID_HANDLE_VALUE) { ok = WriteFile(f, result, (DWORD)strlen(result), &wr, NULL); CloseHandle(f); }
    free(result);
    return ok;
}

// The converter puts its unpacked updates (W10UIuup) and the mounted image (MountUUP) at the root of the drive it
// runs from, which is C: here, and deletes and recreates both, so at a drive root any signed-in user could change
// what goes into the image. The user asked for nothing at the root of C:, so its lines naming them are pointed at
// the work folder, where a recreated folder stays administrators-only. Exact lines, replaced wherever they appear;
// if the first two are gone from a future version, nothing runs, rather than the converter falling back to C:\.
// With vDeleteSource on, the converter also deletes Home whenever it made extra editions in the same run, even
// though none of ours come from Home, so a ticked Home would vanish. Home is only ever downloaded when ticked, so
// that delete line is taken out; when Pro is being dropped it must be found, or the job stops.
typedef struct { const char* from; const char* to; int hits; } DlPatch;

// Replaces each "from" with its "to" wherever it appears in conv\name, counting the hits. FALSE if it couldn't.
// With write FALSE it only counts, so nothing changes unless every script has what's needed.
static BOOL dlPatchScript(const wchar_t* conv, const wchar_t* name, DlPatch* R, int nr, BOOL write, HANDLE out) {
    for (int i = 0; i < nr; i++) R[i].hits = 0;
    wchar_t path[MAX_PATH + 64], line[200];
    swprintf(path, MAX_PATH + 64, L"%s\\%s", conv, name);
    char* text = dlReadSmallFile(path, 16u << 20);
    if (!text) { swprintf(line, 200, L"Error: couldn't read the converter's %s.\r\n", name); repWrite(out, line); return FALSE; }
    size_t cap = strlen(text) + 4096, len = 0;
    char* outText = (char*)malloc(cap);
    BOOL ok = outText != NULL;
    for (const char* p = text; ok && *p; ) {
        int hit = -1;
        for (int i = 0; i < nr && hit < 0; i++) if (!strncmp(p, R[i].from, strlen(R[i].from))) hit = i;
        if (hit >= 0) {
            size_t tl = strlen(R[hit].to);
            if (len + tl + 1 >= cap) { ok = FALSE; break; }
            memcpy(outText + len, R[hit].to, tl); len += tl;
            p += strlen(R[hit].from);
            R[hit].hits++;
        } else {
            if (len + 2 >= cap) { ok = FALSE; break; }
            outText[len++] = *p++;
        }
    }
    free(text);
    if (ok && write) {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wr = 0;
        ok = f != INVALID_HANDLE_VALUE && WriteFile(f, outText, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    free(outText);
    if (!ok) { swprintf(line, 200, L"Error: couldn't update the converter's %s.\r\n", name); repWrite(out, line); }
    return ok;
}

// Both scripts set their own mount folder. create_virtual_editions.cmd, which makes Enterprise and the others, must
// use the same one as convert-UUP.cmd: when it pointed at C:\MountUUP while the image was mounted in the work
// folder, DISM couldn't find the image and every extra edition was left out (seen 2026-11-08 on 26300.9818).
static BOOL dlPatchConverterPaths(const wchar_t* conv, const wchar_t* root, BOOL dropPro, HANDLE out) {
    char rootA[MAX_PATH]; dlNarrow(root, rootA, MAX_PATH);
    char cab[MAX_PATH + 40], mnt[MAX_PATH + 40];
    snprintf(cab, sizeof(cab), "set \"_cabdir=%s\\W10UIuup\"", rootA);
    snprintf(mnt, sizeof(mnt), "set \"_mount=%s\\MountUUP\"", rootA);
    DlPatch C[] = {
        { "set \"_cabdir=%_drv%\\W10UIuup\"", cab, 0 },                     // [0] required
        { "set \"_mount=%_drv%\\MountUUP\"", mnt, 0 },                      // [1] required
        { "set \"_mount=%SystemDrive%\\MountUUP\"", mnt, 0 },
        { "if %_didHome% equ 1 call :dDelete Core", "rem Windows Chosen Utilities keeps Home", 0 },   // [3] if dropPro
    };
    DlPatch V[] = {
        { "set \"_mount=%_vdrv%\\MountUUP\"", mnt, 0 },                     // [0] required
        { "set \"_mount=%SystemDrive%\\MountUUP\"", mnt, 0 },
    };
    if (!dlPatchScript(conv, L"convert-UUP.cmd", C, 4, FALSE, out)) return FALSE;
    if (!dlPatchScript(conv, L"create_virtual_editions.cmd", V, 2, FALSE, out)) return FALSE;
    if (!C[0].hits || !C[1].hits || !V[0].hits) {
        repWrite(out, L"Error: this converter version names its temporary folders differently, so it would use the root of C:. Nothing was converted.\r\n");
        return FALSE;
    }
    if (dropPro && !C[3].hits) {
        repWrite(out, L"Error: this converter version removes Pro differently, so it could also remove Home. Nothing was converted.\r\n");
        return FALSE;
    }
    return dlPatchScript(conv, L"convert-UUP.cmd", C, 4, TRUE, out) && dlPatchScript(conv, L"create_virtual_editions.cmd", V, 2, TRUE, out);
}

// Opens the finished ISO, lists the editions really in it, and says if a ticked one is missing, since the
// converter can skip one without reporting a failure. Returns FALSE if one is. Not being able to check isn't a failure.
// Does the first image hold any apps? 1 yes, 0 no, -1 couldn't tell. Asks the converter's own wimlib, without unpacking.
static int dlWimHasApps(const wchar_t* conv, const wchar_t* wimPath) {
    wchar_t exe[MAX_PATH + 48], cmd[MAX_PATH * 3 + 128];
    swprintf(exe, MAX_PATH + 48, L"%s\\bin\\bin64\\wimlib-imagex.exe", conv);
    swprintf(cmd, MAX_PATH * 3 + 128, L"\"%s\" dir \"%s\" 1 --path=\"Program Files\\WindowsApps\"", exe, wimPath);
    HANDLE rd = NULL; PROCESS_INFORMATION pi;
    if (spawnPiped(exe, cmd, &rd, &pi)) return -1;
    char buf[65536]; DWORD got; BOOL found = FALSE;
    while (ReadFile(rd, buf, sizeof(buf) - 1, &got, NULL) && got) {     // read it all, so the tool can finish
        buf[got] = 0;
        if (strstr(buf, "_8wekyb3d8bbwe")) found = TRUE;
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    waitExitCode(pi.hProcess);
    return found ? 1 : 0;
}

static BOOL dlCheckEditions(const wchar_t* conv, const wchar_t* isoName, const DlSpec* s, BOOL wantApps, BOOL dropPro, HANDLE cancel, HANDLE out) {
    wchar_t tmp[MAX_PATH], seven[MAX_PATH + 32], wimPath[MAX_PATH + 32], cmd[MAX_PATH * 4 + 96], line[400];
    dlRootPath(tmp, L"check");
    dlRemoveTree(tmp);
    CreateDirectoryW(tmp, NULL);
    swprintf(seven, MAX_PATH + 32, L"%s\\bin\\7z.exe", conv);
    swprintf(wimPath, MAX_PATH + 32, L"%s\\install.wim", tmp);
    swprintf(cmd, MAX_PATH * 4 + 96, L"\"%s\" e \"%s\\%s\" sources\\install.wim \"-o%s\" -y -bso0 -bsp0", seven, conv, isoName, tmp);
    repWrite(out, L"\r\nChecking which editions are in the ISO\r\n");
    DWORD code = 1, e = repRunOnConsole(out, seven, cmd, conv, cancel, &code);
    WimEntry* list = (WimEntry*)malloc(sizeof(WimEntry) * REP_WIM_MAX);
    int n = (!e && !code && list) ? repairReadWim(wimPath, list, REP_WIM_MAX) : -1;
    BOOL ok = TRUE;
    if (n <= 0) {
        repWrite(out, L"Couldn't read the ISO's install.wim to check its editions.\r\n");
    } else {
        for (int i = 0; i < n; i++) {
            swprintf(line, 400, L"  %s  (%s, build %s)\r\n", list[i].name, list[i].edition, list[i].build);
            repWrite(out, line);
        }
        wchar_t want[720];
        swprintf(want, 720, L"%s;%s", s->editions, s->virt);
        wchar_t* ctx = NULL;
        for (wchar_t* w = wcstok(want, L";,", &ctx); w; w = wcstok(NULL, L";,", &ctx)) {
            BOOL found = FALSE;
            for (int i = 0; i < n && !found; i++) found = !_wcsicmp(list[i].edition, w);
            if (found) continue;
            swprintf(line, 400, L"Error: %s was ticked, but it isn't in the ISO.\r\n", w);
            repWrite(out, line);
            ok = FALSE;
        }
        if (dropPro) for (int i = 0; i < n; i++) if (!_wcsicmp(list[i].edition, L"Professional")) {
            repWrite(out, L"Error: Pro wasn't ticked, but it's still in the ISO.\r\n");
            ok = FALSE;
            break;
        }
        if (wantApps) {
            int apps = dlWimHasApps(conv, wimPath);
            if (apps == 1) repWrite(out, L"Preinstalled apps: present.\r\n");
            else if (apps == 0) { repWrite(out, L"Error: the apps were downloaded, but they aren't in the ISO.\r\n"); ok = FALSE; }
            else repWrite(out, L"Couldn't check for the preinstalled apps.\r\n");
        }
    }
    free(list);
    dlRemoveTree(tmp);
    return ok;
}

#include "dlprogress.c"

// ---------- the job itself ----------
#define DL_STEP_TOOLS 1
#define DL_STEP_DOWNLOAD 2
#define DL_STEP_CONVERT 3
static void dlStep(HANDLE out, const wchar_t* title) {
    wchar_t mark[2] = { REP_STEP_MARK, 0 };
    repWrite(out, mark);
    repWrite(out, L"\r\n> ");
    repWrite(out, title);
    repWrite(out, L"\r\n");
}

static BOOL dlOnly(const wchar_t* s, const wchar_t* allowedPunct, BOOL alnum) {
    for (; *s; s++) {
        BOOL ok = wcschr(allowedPunct, *s) != NULL;
        if (alnum && ((*s >= L'0' && *s <= L'9') || (*s >= L'a' && *s <= L'z') || (*s >= L'A' && *s <= L'Z'))) ok = TRUE;
        if (!ok) return FALSE;
    }
    return TRUE;
}
static BOOL dlSpecParse(const wchar_t* text, DlSpec* s) {
    memset(s, 0, sizeof(*s));
    // the 8th part, the chosen work folder, may be left out
    struct { wchar_t* to; int cch; } parts[8] = {
        { s->uuid, 48 }, { s->lang, 20 }, { s->editions, 400 }, { s->virt, 300 }, { s->desktop, MAX_PATH }, { s->cancel, 64 },
        { s->apps, DL_APPS_CCH }, { s->work, MAX_PATH } };
    const wchar_t* p = text;
    for (int i = 0; i < 8 && p; i++) {
        const wchar_t* e = wcschr(p, L'|');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        if ((int)n >= parts[i].cch || (i < 6 && !e) || (i == 7 && e)) return FALSE;
        wmemcpy(parts[i].to, p, n); parts[i].to[n] = 0;
        p = e ? e + 1 : NULL;
    }
    return s->uuid[0] && dlOnly(s->uuid, L"-", TRUE) && s->lang[0] && dlOnly(s->lang, L"-", TRUE)
        && (s->editions[0] || s->virt[0]) && dlOnly(s->editions, L";_", TRUE) && dlOnly(s->virt, L",", TRUE)
        && s->desktop[0] && !wcspbrk(s->desktop, L"\"|<>*?") && dlOnly(s->cancel, L"\\-_", TRUE)
        && dlOnly(s->apps, L"._-,", TRUE) && dlWorkBaseOk(s->work);
}

static void dlShowErrorLogs(const wchar_t* conv, HANDLE out) {
    wchar_t pat[MAX_PATH + 32], path[MAX_PATH + 300];
    swprintf(pat, MAX_PATH + 32, L"%s\\ErrorLog_*.txt", conv);
    WIN32_FIND_DATAW fd;
    HANDLE ff = FindFirstFileW(pat, &fd);
    if (ff == INVALID_HANDLE_VALUE) return;
    do {
        swprintf(path, MAX_PATH + 300, L"%s\\%s", conv, fd.cFileName);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[8192]; DWORD got = 0;
            if (ReadFile(f, buf, sizeof(buf) - 1, &got, NULL) && got) {
                buf[got] = 0;
                wchar_t w[8200];
                int n = MultiByteToWideChar(CP_OEMCP, 0, buf, (int)got, w, 8199);
                w[n] = 0;
                repWrite(out, L"\r\nError log from the converter:\r\n");
                repWrite(out, w);
                repWrite(out, L"\r\n");
            }
            CloseHandle(f);
        }
        DeleteFileW(path);
    } while (FindNextFileW(ff, &fd));
    FindClose(ff);
}

// The newest ISO in a folder. Returns FALSE if there's none.
static BOOL dlNewestIso(const wchar_t* dir, wchar_t* name /* MAX_PATH */) {
    wchar_t pat[MAX_PATH + 16];
    swprintf(pat, MAX_PATH + 16, L"%s\\*.iso", dir);
    WIN32_FIND_DATAW fd; FILETIME best = {0};
    name[0] = 0;
    HANDLE ff = FindFirstFileW(pat, &fd);
    if (ff == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!name[0] || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { wcscpy(name, fd.cFileName); best = fd.ftLastWriteTime; }
    } while (FindNextFileW(ff, &fd));
    FindClose(ff);
    return name[0] != 0;
}
static void dlDeleteIsos(const wchar_t* dir) {
    wchar_t name[MAX_PATH], path[MAX_PATH * 2 + 2];
    while (dlNewestIso(dir, name)) {
        swprintf(path, MAX_PATH * 2 + 2, L"%s\\%s", dir, name);
        if (!DeleteFileW(path)) break;
    }
}

// What UUP dump records about a build, for naming the ISO. Anything it doesn't say stays empty.
typedef struct { wchar_t title[200], build[32], ring[24], arch[16]; long long created; } DlFacts;

static void dlReadFacts(const wchar_t* uuid, DlFacts* f) {
    memset(f, 0, sizeof(*f));
    wchar_t q[128], err[200], num[32]; char* b = NULL;
    swprintf(q, 128, L"listlangs.php?id=%s", uuid);
    const char* r = dlApi(q, &b, err, 200);
    const char* info = r ? jGet(r, "updateInfo") : NULL;
    if (info) {
        jText(jGet(info, "title"), f->title, 200);
        jText(jGet(info, "build"), f->build, 32);
        jText(jGet(info, "ring"), f->ring, 24);
        jText(jGet(info, "arch"), f->arch, 16);
        jText(jGet(info, "created"), num, 32);
        f->created = _wtoi64(num);
    }
    free(b);
}

// The names Microsoft uses for the channels. A ring UUP dump adds later is shown as it comes.
static const wchar_t* dlChannelName(const wchar_t* ring) {
    static const struct { const wchar_t* ring; const wchar_t* name; } MAP[] = {
        { L"RETAIL", L"Retail" }, { L"RP", L"Release Preview" }, { L"WIS", L"Beta" }, { L"WIF", L"Dev" }, { L"CANARY", L"Canary" } };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) if (!_wcsicmp(ring, MAP[i].ring)) return MAP[i].name;
    return ring;
}

// The Windows version: "26H2" from the title if it has one, or else the branch part of the converter's name,
// which reads <build>.<date>.<branch>_..., such as 26H2_GE_RELEASE or GE_RELEASE.
static void dlVersionToken(const wchar_t* title, const wchar_t* isoName, wchar_t* out, int cch) {
    out[0] = 0;
    for (const wchar_t* p = title; p[0] && p[1] && p[2] && p[3]; p++) {
        if (iswdigit(p[0]) && iswdigit(p[1]) && (p[2] == L'H' || p[2] == L'h') && iswdigit(p[3]) && !iswalnum(p[4]) && (p == title || !iswalnum(p[-1]))) {
            swprintf(out, cch, L"%c%cH%c", p[0], p[1], p[3]);
            return;
        }
    }
    const wchar_t* d = isoName;
    for (int i = 0; i < 3 && d; i++) { d = wcschr(d, L'.'); if (d) d++; }
    if (!d) return;
    int n = 0;
    while (d[n] && d[n] != L'_' && d[n] != L'.' && n < cch - 1) { out[n] = d[n]; n++; }
    out[n] = 0;
}

// The editions in the ISO, short and joined by dots, always in this order: home.pro.edu.ent.pro-edu.pro-work (the
// user's names). Pro is in it only when ticked: when it's only downloaded to make the others, it's left out of the
// ISO. An edition not in the list (one UUP dump adds later) comes after them, in lower case.
static void dlEditionsToken(const wchar_t* editions, const wchar_t* virt, wchar_t* out, int cch) {
    static const struct { const wchar_t* code; const wchar_t* tag; } ORDER[] = {
        { L"CORE", L"home" }, { L"PROFESSIONAL", L"pro" }, { L"Education", L"edu" }, { L"Enterprise", L"ent" },
        { L"ProfessionalEducation", L"pro-edu" }, { L"ProfessionalWorkstation", L"pro-work" } };
    out[0] = 0;
    wchar_t list[720], *ctx = NULL;
    swprintf(list, 720, L"%s;%s", editions, virt);
    for (size_t i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]); i++) {
        wchar_t tmp[720]; wcscpy(tmp, list);
        for (wchar_t* e = wcstok(tmp, L";,", &ctx); e; e = wcstok(NULL, L";,", &ctx))
            if (!_wcsicmp(e, ORDER[i].code)) {
                if (out[0]) wcsncat(out, L".", cch - wcslen(out) - 1);
                wcsncat(out, ORDER[i].tag, cch - wcslen(out) - 1);
                break;
            }
    }
    for (wchar_t* e = wcstok(list, L";,", &ctx); e; e = wcstok(NULL, L";,", &ctx)) {
        BOOL known = FALSE;
        for (size_t i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]); i++) if (!_wcsicmp(e, ORDER[i].code)) known = TRUE;
        if (known) continue;
        if (out[0]) wcsncat(out, L".", cch - wcslen(out) - 1);
        size_t at = wcslen(out);
        wcsncat(out, e, cch - at - 1);
        for (wchar_t* c = out + at; *c; c++) *c = towlower(*c);
    }
}

// <build>_<version>_<editions>_<x64 or Arm64>_<channel>_<DD>_<Mon>_<YY>.iso. Parts nobody could tell us are left out, and with
// no build number to go on the converter's own name is kept.
static void dlIsoName(const DlFacts* f, const wchar_t* isoName, const wchar_t* editions, const wchar_t* virt, wchar_t* out, int cch) {
    static const wchar_t* MONTHS[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    if (!f->build[0]) { lstrcpynW(out, isoName, cch); return; }
    wchar_t ver[32], part[200], eds[160];
    dlVersionToken(f->title, isoName, ver, 32);
    dlEditionsToken(editions, virt, eds, 160);
    swprintf(out, cch, L"%s", f->build);
    if (ver[0]) { swprintf(part, 64, L"_%s", ver); wcsncat(out, part, cch - wcslen(out) - 1); }
    if (eds[0]) { swprintf(part, 200, L"_%s", eds); wcsncat(out, part, cch - wcslen(out) - 1); }
    if (f->arch[0]) { swprintf(part, 64, L"_%s", dlArchLabel(f->arch)); wcsncat(out, part, cch - wcslen(out) - 1); }
    if (f->ring[0]) { swprintf(part, 64, L"_%s", dlChannelName(f->ring)); wcsncat(out, part, cch - wcslen(out) - 1); }
    if (f->created > 0) {
        ULARGE_INTEGER ticks; FILETIME ft; SYSTEMTIME st;          // seconds since 1970, to Windows' own time format
        ticks.QuadPart = (unsigned long long)f->created * 10000000ULL + 116444736000000000ULL;
        ft.dwLowDateTime = ticks.LowPart; ft.dwHighDateTime = ticks.HighPart;
        if (FileTimeToSystemTime(&ft, &st)) { swprintf(part, 64, L"_%02d_%s_%02d", st.wDay, MONTHS[st.wMonth - 1], st.wYear % 100); wcsncat(out, part, cch - wcslen(out) - 1); }
    }
    for (wchar_t* c = out; *c; c++) if (wcschr(L"\\/:*?\"<>|", *c) || *c < 32) *c = L'-';   // it came over the network
    wcsncat(out, L".iso", cch - wcslen(out) - 1);
}

// ---------- this PC, for the end of the log ----------
// So a slow build can be told apart from a slow PC: the processor, the memory, the drive the work was done on, and how
// fast the files came down.

// The disk under a volume ("C:"): what kind it is and how big. FALSE if Windows won't say.
static BOOL dlDiskFacts(wchar_t letter, wchar_t* kind, int cchKind, unsigned long long* size) {
    wchar_t vol[16]; swprintf(vol, 16, L"\\\\.\\%c:", letter);
    HANDLE v = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (v == INVALID_HANDLE_VALUE) return FALSE;
    BYTE ext[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)]; DWORD got = 0;
    BOOL ok = DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, ext, sizeof(ext), &got, NULL)
              && ((VOLUME_DISK_EXTENTS*)ext)->NumberOfDiskExtents >= 1;
    CloseHandle(v);
    if (!ok) return FALSE;
    wchar_t disk[40]; swprintf(disk, 40, L"\\\\.\\PhysicalDrive%lu", ((VOLUME_DISK_EXTENTS*)ext)->Extents[0].DiskNumber);
    HANDLE d = CreateFileW(disk, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (d == INVALID_HANDLE_VALUE) return FALSE;

    STORAGE_PROPERTY_QUERY q; memset(&q, 0, sizeof(q));
    q.PropertyId = StorageAdapterProperty; q.QueryType = PropertyStandardQuery;
    STORAGE_ADAPTER_DESCRIPTOR ad; memset(&ad, 0, sizeof(ad));
    BOOL nvme = DeviceIoControl(d, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &ad, sizeof(ad), &got, NULL) && ad.BusType == BusTypeNvme;
    BOOL usb  = ad.BusType == BusTypeUsb;
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp; memset(&sp, 0, sizeof(sp));
    BOOL seekKnown = DeviceIoControl(d, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), &got, NULL) && got >= sizeof(sp);
    // the size from the drive's geometry: unlike IOCTL_DISK_GET_LENGTH_INFO, it needs no read access to the disk
    BYTE geo[sizeof(DISK_GEOMETRY_EX) + 256]; memset(geo, 0, sizeof(geo));
    *size = DeviceIoControl(d, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, geo, sizeof(geo), &got, NULL)
            ? (unsigned long long)((DISK_GEOMETRY_EX*)geo)->DiskSize.QuadPart : 0;
    CloseHandle(d);

    const wchar_t* k = nvme ? L"NVMe" : !seekKnown ? L"unknown type" : sp.IncursSeekPenalty ? L"HDD" : L"SSD";
    swprintf(kind, cchKind, L"%s%s", k, usb ? L" (USB)" : L"");
    return TRUE;
}

// Disk makers count in powers of ten (a "1 TB" disk is 1,000,204,886,016 bytes), so the capacity is shown their way
static void dlDiskSize(unsigned long long b, wchar_t* out, int cch) {
    if (b >= 1000000000000ULL) swprintf(out, cch, L"%.1f TB", b / 1e12);
    else swprintf(out, cch, L"%.0f GB", b / 1e9);
    size_t n = wcslen(out);                                     // "1.0 TB" reads better as "1 TB"
    if (n > 5 && !wcscmp(out + n - 5, L".0 TB")) wcscpy(out + n - 5, L" TB");
}

// The lines just above "Time taken". bytes: what was downloaded; ms: how long the download itself ran.
static void dlWriteSystemFacts(HANDLE out, wchar_t workDrive, unsigned long long bytes, ULONGLONG ms) {
    wchar_t line[400], cpu[128] = L""; DWORD cb = sizeof(cpu);
    repWrite(out, L"\r\n");
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, NULL, cpu, &cb) == ERROR_SUCCESS) {
        wchar_t* c = cpu; while (*c == L' ') c++;                       // some list it with spaces in front
        for (wchar_t* e = c + wcslen(c); e > c && e[-1] == L' '; ) *--e = 0;
        swprintf(line, 400, L"CPU: %s\r\n", c);
        repWrite(out, line);
    }
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb) && kb) {
        swprintf(line, 400, L"RAM: %.0f GB\r\n", kb / 1048576.0);
        repWrite(out, line);
    }
    wchar_t kind[40], cap[24]; unsigned long long size = 0;
    if (workDrive && dlDiskFacts(workDrive, kind, 40, &size)) {
        if (size) dlDiskSize(size, cap, 24); else wcscpy(cap, L"size unknown");
        swprintf(line, 400, L"Work drive (%c:): %s, %s\r\n", workDrive, kind, cap);
        repWrite(out, line);
    }
    if (bytes && ms >= 1000) {
        double mbs = bytes / 1048576.0 / (ms / 1000.0);
        swprintf(line, 400, L"Average download speed: %.1f MB/s (%.0f Mbit/s)\r\n", mbs, bytes * 8 / 1e6 / (ms / 1000.0));
        repWrite(out, line);
    }
}

// Room for a download of total bytes, checked before anything is downloaded. The work folder's drive holds the
// download, the images built from it and working room. The finished ISO is then copied to the Desktop: on the same
// drive that's only a rename, already counted; on another drive it needs room for the whole ISO, which is never
// bigger than the download plus a margin. Checked up front, so a full Desktop drive stops the job before an hour
// of work rather than at its last step. Couldn't read a drive's free space: not treated as too little.
static BOOL dlCheckSpace(const wchar_t* root, const wchar_t* desktop, unsigned long long total, HANDLE out) {
    wchar_t rootSlash[MAX_PATH + 1], workVol[MAX_PATH], deskVol[MAX_PATH], nd[48], fr[48], line[600];
    swprintf(rootSlash, MAX_PATH + 1, L"%s\\", root);
    // Measured 2026-11-08 on 26300.9818, 5 editions plus apps, a 10 GB download: the drive's use peaked at 52 GB,
    // while updates went into the mounted image (download, expanded updates, the image and DISM's scratch at once).
    // The old download x 2 + 15 GB (35 GB) would have let a build start that then ran out of space.
    unsigned long long need = total * 5 + (10ULL << 30);
    ULARGE_INTEGER freeB;
    if (GetDiskFreeSpaceExW(rootSlash, &freeB, NULL, NULL) && freeB.QuadPart < need) {
        fmtSize(need, nd, 48); fmtSize(freeB.QuadPart, fr, 48);
        swprintf(line, 600, L"Error: this needs about %s free on the drive with the work folder (%c:), and it has %s. "
                 L"Choose a folder on a drive with more room under Work folder.\r\n", nd, root[0], fr);
        repWrite(out, line);
        return FALSE;
    }
    if (GetVolumePathNameW(rootSlash, workVol, MAX_PATH) && GetVolumePathNameW(desktop, deskVol, MAX_PATH) && _wcsicmp(workVol, deskVol)) {
        unsigned long long deskNeed = total + (2ULL << 30);
        if (GetDiskFreeSpaceExW(deskVol, &freeB, NULL, NULL) && freeB.QuadPart < deskNeed) {
            fmtSize(deskNeed, nd, 48); fmtSize(freeB.QuadPart, fr, 48);
            swprintf(line, 600, L"Error: the finished ISO goes to your Desktop on %s, which needs about %s free for it, and it has %s.\r\n",
                     deskVol, nd, fr);
            repWrite(out, line);
            return FALSE;
        }
    }
    return TRUE;
}

// Moves the ISO out of the administrators-only folder. It takes the Desktop's rights, so a normal window can open it.
static BOOL dlDeliverIso(const wchar_t* conv, const wchar_t* isoName, const wchar_t* saveName, const wchar_t* desktop, wchar_t* finalPath, int cch) {
    wchar_t from[MAX_PATH * 2 + 2];
    swprintf(from, MAX_PATH * 2 + 2, L"%s\\%s", conv, isoName);
    swprintf(finalPath, cch, L"%s\\%s", desktop, saveName);
    wchar_t base[MAX_PATH]; lstrcpynW(base, saveName, MAX_PATH);
    wchar_t* dot = wcsrchr(base, L'.');
    if (dot) *dot = 0;
    for (int i = 2; GetFileAttributesW(finalPath) != INVALID_FILE_ATTRIBUTES && i < 100; i++)
        swprintf(finalPath, cch, L"%s\\%s (%d).iso", desktop, base, i);
    if (!MoveFileExW(from, finalPath, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return FALSE;
    ACL empty;
    if (InitializeAcl(&empty, sizeof(empty), ACL_REVISION))
        SetNamedSecurityInfoW(finalPath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, &empty, NULL);
    return TRUE;
}

// A converter stopped part-way (cancelled, failed, or cut off by a crash or power cut) can leave the Windows image
// mounted at <drive>\MountUUP, that image's registry hives loaded, and its temporary folder W10UIuup. This clears
// them. Only what the converter and DISM make for it is touched: hives named uiSOFTWARE, uiCOMPONENTS or WinPEwim,
// or those DISM names after the MountUUP folder. Returns FALSE if something couldn't be cleared.
static BOOL dlClearConverterLeftovers(HANDLE out) {
    wchar_t line[700], exe[MAX_PATH], drives[2][8], docs[MAX_PATH];
    BOOL ok = TRUE;
    systemPath(exe, L"dism.exe");
    wcscpy(drives[0], L"C:");
    GetEnvironmentVariableW(L"SystemDrive", drives[0], 8);
    int nd = 1;                                  // the converter mounts on the drive it works from, so the Documents drive too
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs)) && docs[1] == L':' && _wcsnicmp(docs, drives[0], 2)) {
        swprintf(drives[1], 8, L"%c:", docs[0]);
        nd = 2;
    }

    // hives first: while one is loaded, its file stays open and the image can't be unmounted
    enablePrivilege(SE_RESTORE_NAME);
    enablePrivilege(SE_BACKUP_NAME);
    static wchar_t names[64][300];
    int nn = 0;
    for (DWORD i = 0; nn < 64; i++) {
        wchar_t nm[300]; DWORD cc = 300;
        LONG e = RegEnumKeyExW(HKEY_LOCAL_MACHINE, i, nm, &cc, NULL, NULL, NULL, NULL);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS) continue;
        if (wcsstr(nm, L"/MountUUP/") || !_wcsicmp(nm, L"uiSOFTWARE") || !_wcsicmp(nm, L"uiCOMPONENTS") || !_wcsicmp(nm, L"WinPEwim"))
            wcscpy(names[nn++], nm);
    }
    for (int i = 0; i < nn; i++) {
        LONG e = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, names[i]);
        swprintf(line, 700, e == ERROR_SUCCESS ? L"Unloaded the registry hive %s\r\n" : L"Error: couldn't unload the registry hive %s (error %ld).\r\n", names[i], e);
        repWrite(out, line);
        if (e != ERROR_SUCCESS) ok = FALSE;
    }

    // Where the converter mounts and unpacks: now inside the work folder, and at drive roots for builds made before
    // that changed
    wchar_t places[DL_NPLACES + 2][MAX_PATH + 16], work[DL_NPLACES][MAX_PATH + 16]; int np = 0;
    dlWorkPlaces(work);
    for (int i = 0; i < DL_NPLACES; i++) if (work[i][0]) wcscpy(places[np++], work[i]);
    for (int d = 0; d < nd; d++) wcscpy(places[np++], drives[d]);
    for (int i = 0; i < np; i++) {
        wchar_t mnt[MAX_PATH + 32], cmd[MAX_PATH + 120]; DWORD code = 0;
        swprintf(mnt, MAX_PATH + 32, L"%s\\MountUUP", places[i]);
        if (GetFileAttributesW(mnt) != INVALID_FILE_ATTRIBUTES) {
            swprintf(line, 700, L"Discarding the image mounted at %s\r\n", mnt);
            repWrite(out, line);
            swprintf(cmd, MAX_PATH + 120, L"dism.exe /English /Unmount-Image /MountDir:\"%s\" /Discard", mnt);
            repRunOnConsole(out, exe, cmd, NULL, NULL, &code);
        }
    }
    wchar_t clean[] = L"dism.exe /English /Cleanup-Mountpoints";
    DWORD code = 0;
    repRunOnConsole(out, exe, clean, NULL, NULL, &code);
    for (int i = 0; i < np; i++) {
        wchar_t mnt[MAX_PATH + 32], tmp[MAX_PATH + 32];
        swprintf(mnt, MAX_PATH + 32, L"%s\\MountUUP", places[i]);
        swprintf(tmp, MAX_PATH + 32, L"%s\\W10UIuup", places[i]);
        dlRemoveFolderOrLink(tmp);
        // an unmounted MountUUP is empty; one that isn't may still be in use, so it's left alone
        if (GetFileAttributesW(mnt) != INVALID_FILE_ATTRIBUTES && !RemoveDirectoryW(mnt)) {
            swprintf(line, 700, L"Error: %s still has files in it. Restart the PC and press Clean up again.\r\n", mnt);
            repWrite(out, line);
            ok = FALSE;
        }
    }
    return ok;
}

// After a cancel or failure the converter may have left an image mounted, and its temporary folders behind.
static void dlCleanUpAfterStop(const wchar_t* conv, HANDLE out) {
    repWrite(out, L"Cleaning up what the converter left behind…\r\n");
    dlClearConverterLeftovers(out);
    dlRemoveTree(conv);
}

// Is a build using this work folder? Its job holds it open without delete sharing, so asking to delete it fails.
static BOOL dlRootInUse(const wchar_t* path) {
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return FALSE; }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

// The Clean up button, run as administrator: everything a build cut short by a crash or power cut can leave.
static DWORD dlCleanUpAll(HANDLE out) {
    wchar_t places[DL_NPLACES][MAX_PATH + 16], line[MAX_PATH + 120];
    dlWorkPlaces(places);
    for (int i = 0; i < DL_NPLACES; i++) if (places[i][0] && dlRootInUse(places[i])) {
        repWrite(out, L"A build is running, so nothing was cleaned up. Cancel it first.\r\n");
        return 1;
    }
    repWrite(out, L"Looking for what an unfinished build left behind\r\n");
    BOOL ok = dlClearConverterLeftovers(out);
    for (int i = 0; i < DL_NPLACES; i++) {
        const wchar_t* work = places[i];
        if (!work[0] || GetFileAttributesW(work) == INVALID_FILE_ATTRIBUTES) continue;
        wchar_t mnt[MAX_PATH + 32];
        swprintf(mnt, MAX_PATH + 32, L"%s\\MountUUP", work);
        if (GetFileAttributesW(mnt) != INVALID_FILE_ATTRIBUTES) {        // never deleted from under DISM
            swprintf(line, MAX_PATH + 120, L"%s was kept, because a Windows image is still mounted in it.\r\n", work);
            repWrite(out, line);
            continue;
        }
        swprintf(line, MAX_PATH + 120, L"Deleting the unfinished downloads in %s\r\n", work);
        repWrite(out, line);
        dlRemoveFolderOrLink(work);
        if (GetFileAttributesW(work) != INVALID_FILE_ATTRIBUTES) {
            swprintf(line, MAX_PATH + 120, L"Error: some files in %s couldn't be deleted.\r\n", work);
            repWrite(out, line);
            ok = FALSE;
        }
    }
    repWrite(out, ok ? L"\r\nClean-up finished. Nothing is left over.\r\n" : L"\r\nClean-up finished, with the errors above.\r\n");
    return ok ? 0 : 1;
}

// ---------- the job: Microsoft's links expire ----------
// Most links UUP dump hands out are valid for only about 10 minutes (bigger files get longer). aria2 keeps going on
// a file it has started, but every file it hasn't reached by then is refused with 403 and the download stops. On a
// connection slower than about 15-20 MB/s that's most of a 10 GB build (seen on a friend's PC at 10 MB/s, 2026-11-08:
// 1421 files refused). So the job asks for fresh links and carries on, as long as each round gets further.

// Finished: the right size and no aria2 control file beside it. Its checksum was checked when aria2 finished it.
static BOOL dlFileDone(const wchar_t* uups, const DlFile* f) {
    wchar_t p[MAX_PATH * 2], c[MAX_PATH * 2 + 8];
    swprintf(p, MAX_PATH * 2, L"%s\\%s", uups, f->name);
    swprintf(c, MAX_PATH * 2 + 8, L"%s.aria2", p);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) return FALSE;
    unsigned long long sz = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return sz == f->size && GetFileAttributesW(c) == INVALID_FILE_ATTRIBUTES;
}

// How much of the download is on disk, finished or not: what tells one round from the next
static unsigned long long dlBytesOnDisk(const wchar_t* uups, const DlFile* files, int n) {
    unsigned long long sum = 0;
    for (int i = 0; i < n; i++) {
        wchar_t p[MAX_PATH * 2]; WIN32_FILE_ATTRIBUTE_DATA a;
        swprintf(p, MAX_PATH * 2, L"%s\\%s", uups, files[i].name);
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &a)) sum += ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    }
    return sum;
}

// Asks UUP dump again, waiting the gap it needs first, and gives each file in files[0..n) its new link, matched by
// name. eds is the editions, separated by ';', or NULL for the apps. The checksums stay the ones from the first list.
// FALSE if a list couldn't be had.
static BOOL dlRefreshLinks(const DlSpec* s, const wchar_t* eds, DlFile* files, int n, HANDLE cancel, HANDLE out) {
    DlFile* fresh = (DlFile*)malloc(sizeof(DlFile) * DL_MAX_FILES);
    if (!fresh) return FALSE;
    int nf = 0; BOOL ok = TRUE;
    wchar_t list[420], line[400]; lstrcpynW(list, eds ? eds : L"app", 420);
    wchar_t* ctx = NULL;
    for (wchar_t* ed = wcstok(list, L";", &ctx); ed && ok; ed = wcstok(NULL, L";", &ctx)) {
        if (WaitForSingleObject(cancel, DL_LINKS_GAP_MS) == WAIT_OBJECT_0) { ok = FALSE; break; }
        wchar_t q[256], err[300] = L""; char* b = NULL;
        if (eds) swprintf(q, 256, L"get.php?id=%s&lang=%s&edition=%s", s->uuid, s->lang, ed);
        else     swprintf(q, 256, L"get.php?id=%s&lang=neutral&edition=app", s->uuid);
        const char* r = dlApi(q, &b, err, 300);
        if (!r || !dlAddEditionFiles(r, fresh, &nf, err, 300)) {
            swprintf(line, 400, L"Couldn't get fresh links for %s: %s\r\n", eds ? ed : L"the apps", err);
            repWrite(out, line);
            ok = FALSE;
        }
        free(b);
    }
    if (ok) for (int i = 0; i < n; i++)
        for (int k = 0; k < nf; k++) if (!_wcsicmp(files[i].name, fresh[k].name)) { strcpy(files[i].url, fresh[k].url); break; }
    free(fresh);
    return ok;
}

// Downloads files[0..n) into uups the way UUP dump's own script does: links fetched just before aria2 runs, jobs
// files at a time (25 for the many small app files, 5 for the rest), and fresh links whenever aria2 stops short.
// eds as for dlRefreshLinks. linksFresh: the list was fetched moments ago, so the first round uses it as it is.
// 0 when all are there, ERROR_CANCELLED, or another value after writing why.
static DWORD dlDownloadGroup(const DlSpec* s, const wchar_t* eds, DlFile* files, int n, int jobs, BOOL linksFresh,
                             const wchar_t* pAria, const wchar_t* uups, const wchar_t* work, HANDLE cancel, HANDLE out) {
    wchar_t listPath[MAX_PATH + 16], line[600], cmd[MAX_PATH * 4 + 300];
    swprintf(listPath, MAX_PATH + 16, L"%s\\files.txt", work);
    BOOL* fin = (BOOL*)calloc((size_t)n + 1, sizeof(BOOL));
    if (!fin) { repWrite(out, L"Error: not enough memory.\r\n"); return ERROR_OUTOFMEMORY; }
    unsigned long long lastBytes = 0;
    DWORD code = 0, result = 1;
    g_dlpGroup = 0;
    for (int i = 0; i < n; i++) g_dlpGroup += files[i].size;
    for (int round = 1; round <= 20; round++) {
        int left = 0;
        for (int i = 0; i < n; i++) { fin[i] = dlFileDone(uups, &files[i]); if (!fin[i]) left++; }
        if (!left) { result = 0; break; }
        if (round > 1) {
            unsigned long long now = dlBytesOnDisk(uups, files, n);
            // From the third round on, a round that got nothing more means fresh links don't help: a real failure.
            // The first retry is always made, since links can expire before aria2 reaches a single file.
            if (round > 2 && now <= lastBytes) break;
            lastBytes = now;
            swprintf(line, 600, L"\r\n%d files weren't finished. Getting fresh links and carrying on (round %d).\r\n", left, round);
            repWrite(out, line);
        }
        if (round > 1 || !linksFresh) {
            if (!dlRefreshLinks(s, eds, files, n, cancel, out)) {
                if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) result = ERROR_CANCELLED;
                break;
            }
        }
        if (!dlWriteAriaList(listPath, files, n, fin)) { repWrite(out, L"Error: couldn't write the download list.\r\n"); result = 2; break; }
        swprintf(cmd, MAX_PATH * 4 + 300,
                 L"\"%s\" --no-conf=true --async-dns=false -x16 -s16 -j%d -c -R --file-allocation=none --console-log-level=warn --download-result=hide --summary-interval=0 -d \"%s\" -i \"%s\"",
                 pAria, jobs, uups, listPath);
        {   // what earlier rounds finished, or more if they left files part done, which aria2 carries on with
            unsigned long long have = g_dlpBefore;
            for (int i = 0; i < n; i++) if (fin[i]) have += files[i].size;
            g_dlpRound = max(have, g_dlpLast);
        }
        g_repAria = TRUE; g_repTick = dlpOnTick; g_dlpTick = 0;
        DWORD e = repRunOnConsole(out, pAria, cmd, work, cancel, &code);
        g_repAria = FALSE; g_repTick = NULL;
        if (e) { swprintf(line, 600, L"Error: couldn't start aria2c (error %lu).\r\n", e); repWrite(out, line); result = e; break; }
        if (code == ERROR_CANCELLED) { result = ERROR_CANCELLED; break; }
        if (code == 0) { result = 0; break; }                    // aria2 finished every file in its list and checked it
    }
    free(fin);
    if (result == 0) { g_dlpLast = g_dlpBefore += g_dlpGroup; dlpReport(out); }
    if (result == 1) {
        swprintf(line, 600, L"\r\nError: the download failed (aria2c exit code %lu), and fresh links didn't get it any further.\r\n", code);
        repWrite(out, line);
        if (g_repRefused) {
            swprintf(line, 600, L"Microsoft refused %d links (403) in the last round.\r\n", g_repRefused);
            repWrite(out, line);
        }
    }
    return result;
}

// The apps first, then the edition files with links fetched once the apps are done, as UUP dump's script does.
// files: the editions' files in [0..nEd), the apps' after them.
static DWORD dlDownloadAll(const DlSpec* s, const wchar_t* edAll, DlFile* files, int nEd, int nFiles,
                           const wchar_t* pAria, const wchar_t* uups, const wchar_t* work, HANDLE cancel, HANDLE out) {
    wchar_t line[200];
    if (nFiles > nEd) {
        swprintf(line, 200, L"\r\nDownloading the apps: %d files, 25 at a time.\r\n", nFiles - nEd);
        repWrite(out, line);
        DWORD r = dlDownloadGroup(s, NULL, files + nEd, nFiles - nEd, 25, TRUE, pAria, uups, work, cancel, out);
        if (r) return r;
    }
    swprintf(line, 200, L"\r\nDownloading the Windows files: %d files, 5 at a time. Getting fresh links for them first.\r\n", nEd);
    repWrite(out, line);
    return dlDownloadGroup(s, edAll, files, nEd, 5, FALSE, pAria, uups, work, cancel, out);
}

static DWORD uupRunJob(const wchar_t* specText, HANDLE out) {
    if (!wcsncmp(specText, L"cleanup", 7) && (!specText[7] || specText[7] == L'|')) {     // "cleanup" or "cleanup|<work folder>"
        const wchar_t* w = specText[7] ? specText + 8 : L"";
        if (wcslen(w) >= MAX_PATH || !dlWorkBaseOk(w)) { repWrite(out, L"Error: the request wasn't valid. Nothing was run.\r\n"); return 1; }
        lstrcpynW(g_dlWorkBase, w, MAX_PATH);
        return dlCleanUpAll(out);
    }
    static DlSpec s; wchar_t line[600], conv[MAX_PATH] = L"";
    if (!dlSpecParse(specText, &s)) { repWrite(out, L"Error: the request wasn't valid. Nothing was run.\r\n"); return 1; }
    lstrcpynW(g_dlWorkBase, s.work, MAX_PATH);
    // The converter runs its helpers by bare name from the folder it's in. With this setting on, cmd won't look there,
    // and the converter just carries on, leaving out the apps and the extra editions. So its tools never inherit it.
    SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", NULL);
    HANDLE cancel = s.cancel[0] ? OpenEventW(SYNCHRONIZE, FALSE, s.cancel) : NULL;
    if (!cancel) cancel = CreateEventW(NULL, TRUE, FALSE, NULL);    // the job always needs an event to wait on

    DWORD result = 1;
    DlFile* files = NULL;
    BOOL converting = FALSE;
#define DL_STOPPED() (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)

    g_dlpT0 = GetTickCount64(); g_dlpTDown = g_dlpTConv = 0;

    // ---- UUP dump's tools ----
    dlStep(out, L"Getting UUP dump's tools");
    DWORD e = dlRootEnsure(out);
    if (e) { swprintf(line, 600, L"Error: couldn't prepare a work folder (error %lu).\r\n", e); repWrite(out, line); goto done; }
    DlTool tools[DL_NTOOLS];
    dlReadManifests(tools, out);
    wchar_t p7z[MAX_PATH], pConvArc[MAX_PATH], pAria[MAX_PATH];
    if (!dlEnsureTool(&tools[DL_TOOL_7ZR], out, p7z) || !dlEnsureTool(&tools[DL_TOOL_CONVERTER], out, pConvArc)
        || !dlEnsureTool(&tools[DL_TOOL_ARIA2], out, pAria)) goto done;
    if (!dlEnsureConverter(&tools[DL_TOOL_CONVERTER], pConvArc, p7z, cancel, out, conv)) goto done;
    if (DL_STOPPED()) goto stopped;

    wchar_t work[MAX_PATH], uups[MAX_PATH + 16], root[MAX_PATH];
    dlRootPath(work, s.uuid);
    dlRootPath(root, NULL);
    CreateDirectoryW(work, NULL);
    swprintf(uups, MAX_PATH + 16, L"%s\\UUPs", work);
    CreateDirectoryW(uups, NULL);

    // ---- the download ----
    dlStep(out, L"Downloading Windows from Microsoft");
    g_dlpTDown = GetTickCount64();
    DlFacts facts;
    dlReadFacts(s.uuid, &facts);       // asked now, so a dropped connection an hour from now can't cost the ISO its name
    files = (DlFile*)malloc(sizeof(DlFile) * DL_MAX_FILES);
    if (!files) { repWrite(out, L"Error: not enough memory.\r\n"); goto done; }
    int nFiles = 0;
    // Enterprise, Education, Pro Education and Pro Workstation are made from Pro, so Pro is downloaded for them even
    // when it isn't ticked, and then left out of the ISO.
    BOOL dropPro = FALSE;
    {
        wchar_t probe[400]; wcscpy(probe, s.editions);
        BOOL hasPro = FALSE; wchar_t* pc = NULL;
        for (wchar_t* ed = wcstok(probe, L";", &pc); ed; ed = wcstok(NULL, L";", &pc)) if (!_wcsicmp(ed, L"PROFESSIONAL")) hasPro = TRUE;
        dropPro = s.virt[0] && !hasPro;
    }
    wchar_t edList[420]; wcscpy(edList, s.editions);
    if (dropPro) {
        wcscat(edList, edList[0] ? L";PROFESSIONAL" : L"PROFESSIONAL");
        repWrite(out, L"Pro is downloaded to build the other editions from, then removed from the ISO.\r\n");
    }
    wchar_t edAll[420]; wcscpy(edAll, edList);      // the loop below cuts edList up; fresh links need the whole list
    wchar_t* ctx = NULL;
    for (wchar_t* ed = wcstok(edList, L";", &ctx); ed; ed = wcstok(NULL, L";", &ctx)) {
        wchar_t q[256], err[300] = L""; char* b = NULL;
        swprintf(line, 600, L"Asking UUP dump for the %s files, language %s\r\n", ed, s.lang);
        repWrite(out, line);
        swprintf(q, 256, L"get.php?id=%s&lang=%s&edition=%s", s.uuid, s.lang, ed);
        const char* r = dlApi(q, &b, err, 300);
        if (!r || !dlAddEditionFiles(r, files, &nFiles, err, 300)) {
            swprintf(line, 600, L"Error: %s\r\n", err);
            repWrite(out, line);
            free(b);
            goto done;
        }
        free(b);
        if (DL_STOPPED()) goto stopped;
        Sleep(DL_LINKS_GAP_MS);
    }
    if (!nFiles) { repWrite(out, L"Error: UUP dump listed no files for this choice.\r\n"); goto done; }

    // The Store, Photos, Terminal and the other preinstalled apps aren't part of any edition's list. Without this the
    // ISO installs fine but has none of them. Only "this build has no apps" is accepted; any other failure stops the
    // job, because an ISO quietly missing its apps is worse than a clear error.
    int nEd = nFiles;                   // the apps' files go after the editions'
    BOOL wantApps = FALSE;
    if (!wcscmp(s.apps, L"-")) {
        repWrite(out, L"No apps were ticked, so none are downloaded.\r\n");
    } else {
        wchar_t q[256], err[300] = L""; char* b = NULL;
        repWrite(out, L"Asking UUP dump for the preinstalled apps\r\n");
        swprintf(q, 256, L"get.php?id=%s&lang=neutral&edition=app", s.uuid);
        const char* r = dlApi(q, &b, err, 300);
        if (r) {
            int before = nFiles;
            if (!dlAddEditionFiles(r, files, &nFiles, err, 300)) {
                swprintf(line, 600, L"Error: %s\r\n", err);
                repWrite(out, line);
                free(b);
                goto done;
            }
            wantApps = nFiles - before > 0;
        } else if (wcsstr(err, L"UNSUPPORTED_COMBINATION") || wcsstr(err, L"EMPTY_FILELIST")) {
            repWrite(out, L"This build lists no separate apps.\r\n");
        } else {
            swprintf(line, 600, L"Error: couldn't get the list of preinstalled apps. %s\r\n", err);
            repWrite(out, line);
            goto done;
        }
        free(b);
        if (DL_STOPPED()) goto stopped;
    }
    unsigned long long total = 0;
    for (int i = 0; i < nFiles; i++) total += files[i].size;
    wchar_t tot[48];
    fmtSize(total, tot, 48);
    if (!dlCheckSpace(root, s.desktop, total, out)) goto done;
    swprintf(line, 600, L"%d files, %s. Finished files are checked against their checksums.\r\n", nFiles, tot);
    repWrite(out, line);

    g_dlpAll = total; g_dlpBefore = g_dlpLast = 0;
    dlpReport(out);
    ULONGLONG tAria = GetTickCount64();
    DWORD dr = dlDownloadAll(&s, edAll, files, nEd, nFiles, pAria, uups, work, cancel, out);
    ULONGLONG ariaMs = GetTickCount64() - tAria;
    unsigned long long dlBytes = total;
    if (dr == ERROR_CANCELLED) goto stopped;
    if (dr) goto done;
    repWrite(out, L"\r\nDownload finished.\r\n");

    // ---- the conversion ----
    dlStep(out, L"Converting the files to an ISO");
    g_dlpTConv = GetTickCount64();
    if (!dlWriteConverterConfig(conv, s.virt, s.apps, dropPro, out)) goto done;
    if (!dlPatchConverterPaths(conv, root, dropPro, out)) goto done;
    dlDeleteIsos(conv);
    wchar_t scriptCmd[MAX_PATH * 3 + 64], cmdExe[MAX_PATH]; DWORD code = 0;
    systemPath(cmdExe, L"cmd.exe");
    swprintf(scriptCmd, MAX_PATH * 3 + 64, L"cmd.exe /d /c \"\"%s\\convert-UUP.cmd\" \"%s\"\"", conv, uups);
    converting = TRUE;
    e = repRunOnConsole(out, cmdExe, scriptCmd, conv, cancel, &code);
    if (e) { swprintf(line, 600, L"Error: couldn't start the converter (error %lu).\r\n", e); repWrite(out, line); goto done; }
    if (code == ERROR_CANCELLED) goto stopped;

    dlShowErrorLogs(conv, out);
    wchar_t iso[MAX_PATH];
    if (!dlNewestIso(conv, iso)) {
        repWrite(out, L"\r\nError: the converter finished without making an ISO. See the messages above.\r\n");
        goto done;
    }
    BOOL allEditions = dlCheckEditions(conv, iso, &s, wantApps, dropPro, cancel, out);
    wchar_t finalPath[MAX_PATH * 2], saveName[MAX_PATH];
    dlIsoName(&facts, iso, s.editions, s.virt, saveName, MAX_PATH);
    if (!dlDeliverIso(conv, iso, saveName, s.desktop, finalPath, MAX_PATH * 2)) {
        DWORD me = GetLastError();
        if (me == ERROR_DISK_FULL || me == ERROR_HANDLE_DISK_FULL)     // something else filled it during the build
            swprintf(line, 600, L"\r\nError: the ISO was built, but the Desktop's drive ran out of space while it was being copied there.\r\n");
        else
            swprintf(line, 600, L"\r\nError: the ISO was built but couldn't be moved to the Desktop (error %lu).\r\n", me);
        repWrite(out, line);
        goto done;
    }
    if (!allEditions) {
        swprintf(line, 600, L"\r\nError: the ISO was saved to %s, but it's missing what's listed above.\r\n", finalPath);
        repWrite(out, line);
        goto done;
    }
    wchar_t workDrive = root[1] == L':' ? root[0] : 0;   // read before the folder goes
    dlRootRemove(out);          // before the last line, which the window reads as the end
    dlWriteSystemFacts(out, workDrive, dlBytes, ariaMs);
    dlpWriteTimes(out, L"Time taken", FALSE);
    swprintf(line, 600, L"\r\n" DL_SAVED_PREFIX L"%s\r\n", finalPath);
    repWrite(out, line);
    result = 0;
    goto done;

stopped:
    result = REP_CANCELLED;
    repWrite(out, L"\r\nCancelled.\r\n");
done:
    if (converting && result != 0) dlCleanUpAfterStop(conv, out);   // a stopped converter can leave an image mounted
    dlRootRemove(out);
    if (result != 0) dlpWriteTimes(out, L"Stopped after", TRUE);
    free(files);
    CloseHandle(cancel);
    return result;
}

// ---------- the page: what's on it ----------
static int  g_dlArch = 0;                         // index into DL_ARCHES: x64 by default
static wchar_t g_dlBox[32] = L"";
static BOOL g_dlHas = FALSE;                      // a build is chosen
static DlBuild g_dlCur;
static wchar_t g_dlMsg[320] = L"";
static BOOL g_dlMsgErr = FALSE;
static volatile LONG g_dlQuery = 0;               // a lookup is running on another thread
static DlEdition g_dlEd[DL_MAX_EDITIONS];
static int  g_dlEdN = 0;
static BOOL g_dlVirt[DL_NVIRTUAL];
static DlLang g_dlLangs[DL_MAX_LANGS];
static int  g_dlLangN = 0, g_dlLangSel = -1;
static wchar_t g_dlLangCode[20] = L"en-us";       // the default, kept while languages change between builds
static wchar_t g_dlLangName[80] = L"English (United States)";
static BOOL g_dlDetailsBusy = FALSE;
static HANDLE g_dlCancelEv = NULL;
static DlApp g_dlApps[DL_MAX_APPS];               // the build's apps, all ticked until unticked in "Choose apps"
static int  g_dlAppN = 0;
static int  g_dlAppsState = 0;                    // 0 reading, 1 read, 2 the build has none, 3 couldn't be read
static wchar_t g_dlAppsErr[320] = L"";

static wchar_t g_dlExample[32] = L"";             // the newest Release Preview build, once looked up
static int  g_dlExampleState = 0;                 // 0 not asked yet, 1 asking, 2 asked

enum { DLR_FOUND, DLR_DETAILS, DLR_APPS, DLR_EXAMPLE };
typedef struct {
    int kind, index, state;
    DlBuild build;
    DlBuild matches[DL_MAX_MATCHES + 1];
    wchar_t err[320];
    DlEdition ed[DL_MAX_EDITIONS]; int edN;
    DlLang langs[DL_MAX_LANGS]; int langN;
    wchar_t lang[20];
    DlApp apps[DL_MAX_APPS]; int appN;
} DlResult;

static void dlPost(DlResult* r) { if (!PostMessageW(g_hwnd, WM_APP_DLRESULT, 0, (LPARAM)r)) free(r); }

// The newest Release Preview build, shown as the example in the Find box. Which Windows versions exist comes from
// UUP dump's list ("Windows 11, version 26H2 (26300.10075)"); the channel is asked about the newest first.
static DWORD WINAPI dlExampleProc(LPVOID arg) {
    (void)arg;
    DlResult* out = (DlResult*)calloc(1, sizeof(DlResult));
    if (!out) return 0;
    out->kind = DLR_EXAMPLE;
    struct { wchar_t name[8]; unsigned long major; } vers[16], tmp; int n = 0;
    wchar_t err[320]; char* b;
    const char* r = dlApi(L"listid.php?search=Windows+11,+version&sortByDate=1", &b, err, 320);
    JIter it; const char* k; const char* v;
    if (r && jOpen(jGet(r, "builds"), &it)) {
        while (jNext(&it, &k, &v)) {
            wchar_t arch[16], title[200], build[32];
            jText(jGet(v, "arch"), arch, 16);
            jText(jGet(v, "title"), title, 200);
            jText(jGet(v, "build"), build, 32);
            const wchar_t* p = wcsstr(title, L", version ");
            if (wcscmp(arch, L"amd64") || wcsncmp(title, L"Windows 1", 9) || !p || !dlValidBuildText(build)) continue;
            p += 10;
            if (!iswdigit(p[0]) || !iswdigit(p[1]) || p[2] != L'H' || !iswdigit(p[3])) continue;
            int i = 0;
            while (i < n && wcsncmp(vers[i].name, p, 4)) i++;
            if (i < n) continue;                        // newest first, so the first entry of a version is enough
            if (n >= 16) break;
            lstrcpynW(vers[n].name, p, 5);
            vers[n++].major = wcstoul(build, NULL, 10);
        }
    }
    free(b);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (wcscmp(vers[j].name, vers[i].name) > 0) { tmp = vers[i]; vers[i] = vers[j]; vers[j] = tmp; }
    for (int i = 0; i < n && i < 3 && !out->build.build[0]; i++) {
        wchar_t q[200];
        Sleep(5000);                                    // UUP dump refuses requests that come too close together
        swprintf(q, 200, L"fetchupd.php?arch=amd64&ring=ReleasePreview&flight=Active&build=%lu.1&sku=48&type=Production", vers[i].major);
        r = dlApi(q, &b, err, 320);
        if (r && jOpen(jGet(r, "updateArray"), &it)) {
            while (jNext(&it, &k, &v)) {
                wchar_t title[200], found[32];
                jText(jGet(v, "updateTitle"), title, 200);
                jText(jGet(v, "foundBuild"), found, 32);
                if (StrStrIW(title, L".NET") || !dlValidBuildText(found) || wcstoul(found, NULL, 10) != vers[i].major) continue;
                if (!out->build.build[0] || updCmpBuild(found, out->build.build) > 0) wcscpy(out->build.build, found);
            }
        }
        free(b);
    }
    dlPost(out);
    return 0;
}

// Once per run, when the page is first shown
static void dlWantExample(void) {
    if (g_dlExampleState) return;
    g_dlExampleState = 1;
    HANDLE t = CreateThread(NULL, 0, dlExampleProc, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else g_dlExampleState = 2;
}
static const wchar_t* dlExample(void) { return g_dlExample[0] ? g_dlExample : L"26300.9818"; }

typedef struct { wchar_t typed[32]; int arch; } DlFindArg;
static DWORD WINAPI dlFindProc(LPVOID arg) {
    DlFindArg* a = (DlFindArg*)arg;
    DlResult* r = (DlResult*)calloc(1, sizeof(DlResult));
    if (r) {
        r->kind = DLR_FOUND;
        r->index = a->arch;                          // a reply for an architecture since switched away from is dropped
        r->state = dlFindBuilds(a->typed, DL_ARCHES[a->arch].api, r->matches, r->err, 320);
        dlPost(r);
    }
    free(a);
    InterlockedExchange(&g_dlQuery, 0);
    return 0;
}

typedef struct { wchar_t uuid[48], lang[20]; } DlDetailsArg;
static DWORD WINAPI dlDetailsProc(LPVOID arg) {
    DlDetailsArg* a = (DlDetailsArg*)arg;
    DlResult* r = (DlResult*)calloc(1, sizeof(DlResult));
    if (r) {
        r->kind = DLR_DETAILS;
        wcscpy(r->build.uuid, a->uuid);
        r->langN = dlReadLangs(a->uuid, r->langs, r->err, 320);
        if (r->langN > 0) {
            const wchar_t* use = NULL;                   // the chosen language if this build has it, or else en-us
            for (int i = 0; i < r->langN && !use; i++) if (!wcscmp(r->langs[i].code, a->lang)) use = r->langs[i].code;
            for (int i = 0; i < r->langN && !use; i++) if (!wcscmp(r->langs[i].code, L"en-us")) use = r->langs[i].code;
            if (!use) use = r->langs[0].code;
            r->index = (int)(use - r->langs[0].code) / (int)(sizeof(DlLang) / sizeof(wchar_t));
            Sleep(DL_LIST_GAP_MS);
            r->edN = dlReadEditions(a->uuid, use, r->ed, r->err, 320);
        }
        // then the apps, for the editions and language just found; the page is usable while they load
        wchar_t lang[20] = L"", eds[400] = L"";
        if (r->langN > 0 && r->edN > 0) {
            lstrcpynW(lang, r->langs[r->index].code, 20);
            for (int i = 0; i < r->edN; i++) {
                if (wcslen(eds) + wcslen(r->ed[i].code) + 2 >= 400) break;
                if (eds[0]) wcscat(eds, L";");
                wcscat(eds, r->ed[i].code);
            }
        }
        dlPost(r);
        if (lang[0]) {
            DlResult* ra = (DlResult*)calloc(1, sizeof(DlResult));
            if (ra) {
                ra->kind = DLR_APPS;
                wcscpy(ra->build.uuid, a->uuid);
                wcscpy(ra->lang, lang);
                Sleep(DL_LIST_GAP_MS);
                ra->appN = dlReadBuildApps(a->uuid, lang, eds, ra->apps, DL_MAX_APPS, ra->err, 320);
                dlPost(ra);
            }
        }
    }
    free(a);
    return 0;
}

static void dlLoadDetails(void) {
    DlDetailsArg* a = (DlDetailsArg*)malloc(sizeof(DlDetailsArg));
    if (!a) return;
    wcscpy(a->uuid, g_dlCur.uuid);
    wcscpy(a->lang, g_dlLangCode);
    g_dlDetailsBusy = TRUE;
    g_dlAppsState = 0;
    HANDLE t = CreateThread(NULL, 0, dlDetailsProc, a, 0, NULL);
    if (t) CloseHandle(t);
    else { free(a); g_dlDetailsBusy = FALSE; }
}

static void dlChoose(const DlBuild* b) {
    g_dlCur = *b;
    g_dlHas = TRUE;
    g_dlEdN = 0;
    g_dlLangN = 0;
    g_dlAppN = 0;                                   // a new build starts with all its apps ticked
    memset(g_dlVirt, 0, sizeof(g_dlVirt));
    dlLoadDetails();
}

static void dlOnResult(DlResult* r) {
    switch (r->kind) {
    case DLR_FOUND:
        if (r->index != g_dlArch) break;
        if (r->state > 1) {
            static const wchar_t* items[DL_MAX_MATCHES];
            for (int i = 0; i < r->state; i++) items[i] = r->matches[i].title;
            int pick = 0;
            DlgExtra list; memset(&list, 0, sizeof(list));
            list.items = items; list.nItems = r->state; list.pick = &pick;
            g_dlMsg[0] = 0; g_dlMsgErr = FALSE;
            if (showDialogEx(g_hwnd, L"Which one?", L"UUP dump lists more than one entry for this build number.",
                             NULL, 0, L"Choose", L"Cancel", 0, &list)) dlChoose(&r->matches[pick]);
        } else if (r->state == 1) {
            g_dlMsg[0] = 0; g_dlMsgErr = FALSE;
            dlChoose(&r->matches[0]);
        } else {
            wcscpy(g_dlMsg, r->err); g_dlMsgErr = TRUE;
        }
        break;
    case DLR_EXAMPLE:
        g_dlExampleState = 2;
        lstrcpynW(g_dlExample, r->build.build, 32);      // empty when it couldn't be looked up
        break;
    case DLR_DETAILS:
        if (!g_dlHas || wcscmp(r->build.uuid, g_dlCur.uuid)) break;    // the choice moved on while this was loading
        g_dlDetailsBusy = FALSE;
        if (r->langN <= 0 || r->edN <= 0) {
            wcscpy(g_dlMsg, r->err[0] ? r->err : L"UUP dump lists no editions for this build, so it can't become an ISO.");
            g_dlMsgErr = TRUE;
            g_dlEdN = 0;
            break;
        }
        g_dlMsg[0] = 0; g_dlMsgErr = FALSE;
        memcpy(g_dlLangs, r->langs, sizeof(DlLang) * r->langN);
        g_dlLangN = r->langN;
        g_dlLangSel = r->index;
        wcscpy(g_dlLangCode, g_dlLangs[r->index].code);
        wcscpy(g_dlLangName, g_dlLangs[r->index].name);
        {
            DlEdition keep[DL_MAX_EDITIONS]; int keepN = g_dlEdN;       // a language change keeps the ticks
            memcpy(keep, g_dlEd, sizeof(keep));
            memcpy(g_dlEd, r->ed, sizeof(DlEdition) * r->edN);
            g_dlEdN = r->edN;
            for (int i = 0; i < g_dlEdN; i++) {
                for (int k = 0; k < keepN; k++) if (!wcscmp(keep[k].code, g_dlEd[i].code)) g_dlEd[i].checked = keep[k].checked;
                if (!keepN && !_wcsicmp(g_dlEd[i].code, L"PROFESSIONAL")) g_dlEd[i].checked = TRUE;
            }
        }
        break;
    case DLR_APPS:
        if (!g_dlHas || wcscmp(r->build.uuid, g_dlCur.uuid) || wcscmp(r->lang, g_dlLangCode)) break;
        if (r->appN < 0) {
            g_dlAppsState = 3;
            lstrcpynW(g_dlAppsErr, r->err, 320);
            break;
        }
        {
            for (int i = 0; i < r->appN; i++)                           // a language change keeps the ticks
                for (int k = 0; k < g_dlAppN; k++) if (!_wcsicmp(g_dlApps[k].id, r->apps[i].id)) r->apps[i].checked = g_dlApps[k].checked;
            memcpy(g_dlApps, r->apps, sizeof(DlApp) * (size_t)r->appN);
            g_dlAppN = r->appN;
            g_dlAppsState = r->appN > 0 ? 1 : 2;
        }
        break;
    }
    free(r);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- the page: actions ----------
static void dlFind(void) {
    if (g_dlQuery || g_busy) return;
    wchar_t t[32]; int n = 0;
    for (const wchar_t* p = g_dlBox; *p && n < 31; p++) if (*p != L' ') t[n++] = *p;
    t[n] = 0;
    if (!dlValidBuildText(t)) {
        swprintf(g_dlMsg, 320, L"Type a build number, for example %s.", dlExample());
        g_dlMsgErr = TRUE;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    DlFindArg* arg = (DlFindArg*)malloc(sizeof(DlFindArg));
    if (!arg) return;
    wcscpy(arg->typed, t);
    arg->arch = g_dlArch;
    g_dlHas = FALSE; g_dlEdN = 0; g_dlLangN = 0; g_dlDetailsBusy = FALSE;
    swprintf(g_dlMsg, 320, L"Looking for build %s for %s…", t, DL_ARCHES[g_dlArch].label);
    g_dlMsgErr = FALSE;
    InterlockedExchange(&g_dlQuery, 1);
    HANDLE th = CreateThread(NULL, 0, dlFindProc, arg, 0, NULL);
    if (th) CloseHandle(th);
    else { free(arg); InterlockedExchange(&g_dlQuery, 0); }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// The same build number is a different UUP dump entry for each architecture, so a chosen build is looked up again
static void dlSetArch(int arch) {
    if (arch == g_dlArch) return;
    g_dlArch = arch;
    BOOL had = g_dlHas || g_dlQuery;
    g_dlHas = FALSE; g_dlEdN = 0; g_dlLangN = 0; g_dlDetailsBusy = FALSE; g_dlMsg[0] = 0; g_dlMsgErr = FALSE;
    if (had && !g_dlQuery) dlFind();
}

static void dlPickLanguage(void) {
    if (!g_dlLangN) return;
    static const wchar_t* items[DL_MAX_LANGS];
    for (int i = 0; i < g_dlLangN; i++) items[i] = g_dlLangs[i].name;
    int pick = g_dlLangSel < 0 ? 0 : g_dlLangSel;
    DlgExtra list; memset(&list, 0, sizeof(list));
    list.items = items; list.nItems = g_dlLangN; list.pick = &pick;
    if (!showDialogEx(g_hwnd, L"Language", L"The language of the Windows image. Other languages can still be added to Windows later.",
                      NULL, 0, L"Choose", L"Cancel", 0, &list)) return;
    if (pick == g_dlLangSel) return;
    g_dlLangSel = pick;
    wcscpy(g_dlLangCode, g_dlLangs[pick].code);
    wcscpy(g_dlLangName, g_dlLangs[pick].name);
    dlLoadDetails();                                // editions can differ by language
}

// Counts of the apps that can be chosen: the fixed ones are always in and aren't counted
static int dlAppsTicked(void) {
    int n = 0;
    for (int i = 0; i < g_dlAppN; i++) if (g_dlApps[i].checked && !g_dlApps[i].fixed) n++;
    return n;
}
static int dlAppsChoosable(void) {
    int n = 0;
    for (int i = 0; i < g_dlAppN; i++) if (!g_dlApps[i].fixed) n++;
    return n;
}

// What the Apps button says
static void dlAppsLabel(wchar_t* out, int cch) {
    if (!g_dlHas || g_dlEdN == 0) { lstrcpynW(out, L"All apps", cch); return; }
    switch (g_dlAppsState) {
    case 0: lstrcpynW(out, L"Reading apps…", cch); return;
    case 2: lstrcpynW(out, L"This build has no apps", cch); return;
    case 3: lstrcpynW(out, L"All apps (list unreadable)", cch); return;
    }
    int on = dlAppsTicked(), all = dlAppsChoosable();
    if (on == all) swprintf(out, cch, L"All %d apps", all);
    else if (on == 0) lstrcpynW(out, L"Only the essential apps", cch);
    else swprintf(out, cch, L"%d of %d apps", on, all);
}

static void dlPickApps(void) {
    if (g_dlAppsState != 1 || !dlAppsChoosable()) return;
    static const wchar_t* items[DL_MAX_APPS];
    static BOOL checks[DL_MAX_APPS];
    static int at[DL_MAX_APPS];                     // which app each row is: the fixed ones aren't listed
    int n = 0;
    for (int i = 0; i < g_dlAppN; i++) {
        if (g_dlApps[i].fixed) continue;
        items[n] = g_dlApps[i].name; checks[n] = g_dlApps[i].checked; at[n] = i; n++;
    }
    int pick = 0;
    DlgExtra list; memset(&list, 0, sizeof(list));
    list.items = items; list.nItems = n; list.pick = &pick; list.checks = checks;
    if (!showDialogEx(g_hwnd, L"Choose apps",
                      L"Unticked apps are left out of the ISO and can be installed from the Microsoft Store later. "
                      L"The Store, Store Purchase App, Windows Security, Feedback Hub and the Xbox apps are always installed.",
                      NULL, 0, L"OK", L"Cancel", 0, &list)) return;
    for (int k = 0; k < n; k++) g_dlApps[at[k]].checked = checks[k];
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// The apps part of the job's request: empty when every app goes in, as it always did, "-" for none at all, or the
// IDs to add: the ticked ones and the fixed ones
static void dlAppsSpec(wchar_t* out, int cch) {
    out[0] = 0;
    if (g_dlAppsState != 1) return;                 // not read: the converter adds all of them
    if (dlAppsTicked() == dlAppsChoosable()) return;
    BOOL any = FALSE;
    for (int i = 0; i < g_dlAppN; i++) if (g_dlApps[i].checked || g_dlApps[i].fixed) any = TRUE;
    if (!any) { wcscpy(out, L"-"); return; }
    for (int i = 0; i < g_dlAppN; i++) {
        if (!g_dlApps[i].checked && !g_dlApps[i].fixed) continue;
        if (wcslen(out) + wcslen(g_dlApps[i].id) + 2 >= (size_t)cch) break;
        if (out[0]) wcscat(out, L",");
        wcscat(out, g_dlApps[i].id);
    }
}

static BOOL dlAnyEdition(void) {
    for (int i = 0; i < g_dlEdN; i++) if (g_dlEd[i].checked) return TRUE;
    for (int i = 0; i < DL_NVIRTUAL; i++) if (g_dlVirt[i]) return TRUE;
    return FALSE;
}

static void dlWorkLabel(wchar_t* path, int cchPath, wchar_t* freeTx, int cchFree);
static void dlStart(void) {
    if (!g_dlHas || !dlAnyEdition() || g_busy) return;
    if (askInApp()) {
        wchar_t msg[1200], appsTx[80], wp[MAX_PATH], wf[80];
        dlAppsLabel(appsTx, 80);
        dlWorkLabel(wp, MAX_PATH, wf, 80);
        swprintf(msg, 1200, L"%s\n\nArchitecture: %s\nLanguage: %s\nApps: %s\nWork folder: %s\n\nThis downloads several GB from Microsoft and can take over an hour. The ISO is saved to your Desktop.",
                 g_dlCur.title, DL_ARCHES[g_dlArch].label, g_dlLangName, appsTx, wp);
        if (!showDialog(g_hwnd, L"Download and build this ISO?", msg, NULL, 0, L"Start", L"Cancel", 1)) return;
    }
    wchar_t eds[400] = L"", virt[300] = L"", desktop[MAX_PATH] = L"", evName[64], spec[REP_ARG_MAX];
    for (int i = 0; i < g_dlEdN; i++) {
        if (!g_dlEd[i].checked) continue;
        if (eds[0]) wcscat(eds, L";");
        wcscat(eds, g_dlEd[i].code);
    }
    for (int i = 0; i < DL_NVIRTUAL; i++) {
        if (!g_dlVirt[i]) continue;
        if (virt[0]) wcscat(virt, L",");
        wcscat(virt, DL_VIRTUAL[i].key);
    }
    // the window's own Desktop: an elevated helper could be running under a different administrator's account
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop))) {
        wcscpy(g_repStatus, L"Couldn't find the Desktop folder, so nothing was run.");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    swprintf(evName, 64, L"Local\\wcu-uup-stop-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    if (g_dlCancelEv) CloseHandle(g_dlCancelEv);
    g_dlCancelEv = CreateEventW(NULL, TRUE, FALSE, evName);
    static wchar_t apps[DL_APPS_CCH];
    dlAppsSpec(apps, DL_APPS_CCH);
    swprintf(spec, REP_ARG_MAX, L"%s|%s|%s|%s|%s|%s|%s|%s", g_dlCur.uuid, g_dlLangCode, eds, virt, desktop, g_dlCancelEv ? evName : L"", apps, g_dlWorkBase);
    g_repShow = TRUE;
    int nImg = 0, nVirt = 0; BOOL pro = FALSE;
    for (int i = 0; i < g_dlEdN; i++) if (g_dlEd[i].checked) { nImg++; if (!_wcsicmp(g_dlEd[i].code, L"PROFESSIONAL")) pro = TRUE; }
    for (int i = 0; i < DL_NVIRTUAL; i++) if (g_dlVirt[i]) nVirt++;
    double pace = dlpLoadPace();
    EnterCriticalSection(&g_lock);
    int nApps = 0;
    for (int i = 0; i < g_dlAppN; i++) if (g_dlApps[i].checked || g_dlApps[i].fixed) nApps++;
    if (g_dlAppsState != 1) nApps = g_dlAppsState == 2 ? 0 : DLP_ALLAPPS;
    dlpBegin(&g_dlp, nImg + (nVirt && !pro), nVirt, nApps, pace, GetTickCount64());   // before the job: its first words come at once
    LeaveCriticalSection(&g_lock);
    repairStart(REP_UUP, spec);
    if (!g_repRunning) { dlpHide(); return; }
    EnterCriticalSection(&g_lock);
    g_dlp.t0 = GetTickCount64();        // waiting at the UAC prompt isn't the job's time
    LeaveCriticalSection(&g_lock);
    SetTimer(g_hwnd, TIMER_DLP, 1000, NULL);
}


// The panel's text, saved beside the ISO under the same name with .txt. It's written by this window, not the
// administrator helper, so the file belongs to the user like anything else on their Desktop.
static BOOL dlSaveLog(const wchar_t* isoPath) {
    wchar_t path[MAX_PATH * 2 + 8];
    lstrcpynW(path, isoPath, MAX_PATH * 2);
    wchar_t* dot = wcsrchr(path, L'.');
    if (!dot || _wcsicmp(dot, L".iso")) return FALSE;
    wcscpy(dot, L".txt");
    wchar_t* all = repJoinLines();
    if (!all) return FALSE;
    BOOL ok = FALSE;
    int n = WideCharToMultiByte(CP_UTF8, 0, all, -1, NULL, 0, NULL, NULL);
    char* utf8 = n > 0 ? (char*)malloc(n) : NULL;
    if (utf8 && WideCharToMultiByte(CP_UTF8, 0, all, -1, utf8, n, NULL, NULL)) {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w1 = 0, w2 = 0;
            ok = WriteFile(h, "\xEF\xBB\xBF", 3, &w1, NULL) && WriteFile(h, utf8, (DWORD)(n - 1), &w2, NULL) && w2 == (DWORD)(n - 1);
            CloseHandle(h);
        }
    }
    free(utf8); free(all);
    return ok;
}

// The job can run for an hour with the app in the background, so it's announced without a dialog (note.c)
static void dlOnJobSucceeded(void) {
    size_t pre = wcslen(DL_SAVED_PREFIX);
    wchar_t isoPath[MAX_PATH * 2 + 2] = L"";
    EnterCriticalSection(&g_lock);
    for (int i = g_repN - 1; i >= 0 && !isoPath[0]; i--) {
        if (!g_repLines[i].s || wcsncmp(g_repLines[i].s, DL_SAVED_PREFIX, pre)) continue;
        lstrcpynW(isoPath, g_repLines[i].s + pre, MAX_PATH * 2);
    }
    LeaveCriticalSection(&g_lock);
    if (!isoPath[0]) return;
    BOOL logged = dlSaveLog(isoPath);
    const wchar_t* slash = wcsrchr(isoPath, L'\\');
    swprintf(g_dlReady, MAX_PATH * 2 + 64, L"Ready on your Desktop: %s", slash ? slash + 1 : isoPath);
    wcscpy(g_repStatus, logged ? L"Finished. Log saved beside the ISO." : L"Finished successfully.");
    noteShow(PAGE_DOWNLOAD);
}

// ---------- the Work folder row ----------
// Where the download and the conversion's files go (about 5 times the download, 40-60 GB): Documents by default.
// A folder chosen here is kept in the registry for the next runs.
static BOOL g_dlWorkLoaded = FALSE;
static void dlWorkLoad(void) {
    if (g_dlWorkLoaded) return;
    g_dlWorkLoaded = TRUE;
    wchar_t v[MAX_PATH] = L""; DWORD cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, LANG_KEY, DL_WORK_VALUE, RRF_RT_REG_SZ, NULL, v, &cb) == ERROR_SUCCESS && dlWorkBaseOk(v))
        lstrcpynW(g_dlWorkBase, v, MAX_PATH);
}

// What the row shows: the folder, and the free space on its drive
static void dlWorkLabel(wchar_t* path, int cchPath, wchar_t* freeTx, int cchFree) {
    wchar_t docs[MAX_PATH] = L"", vol[MAX_PATH], fr[48];
    const wchar_t* at = g_dlWorkBase;
    if (!at[0]) {
        SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs);
        at = docs;
        swprintf(path, cchPath, L"Documents");
    } else lstrcpynW(path, at, cchPath);
    freeTx[0] = 0;
    ULARGE_INTEGER freeB;
    if (at[0] && GetVolumePathNameW(at, vol, MAX_PATH) && GetDiskFreeSpaceExW(vol, &freeB, NULL, NULL)) {
        fmtSize(freeB.QuadPart, fr, 48);
        swprintf(freeTx, cchFree, L"%s free", fr);
    }
}

static void dlWorkSave(void) {
    if (g_dlWorkBase[0]) RegSetKeyValueW(HKEY_CURRENT_USER, LANG_KEY, DL_WORK_VALUE, REG_SZ, g_dlWorkBase, (DWORD)((wcslen(g_dlWorkBase) + 1) * sizeof(wchar_t)));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, LANG_KEY, DL_WORK_VALUE);
}

static void dlPickWork(void) {
    wchar_t p[MAX_PATH] = L"", docs[MAX_PATH] = L"", sp[MAX_PATH];
    if (!pickFolder(p)) return;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs)) && !_wcsicmp(p, docs)) {   // Documents is the default
        g_dlWorkBase[0] = 0;
        dlWorkSave();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    const wchar_t* why = NULL;
    if (!dlWorkBaseOk(p)) why = L"Choose a folder on a drive of this PC (with a drive letter).";
    else if (dlDocsUnusable(p)) why = L"Choose a folder on an NTFS drive of this PC. Network folders, CD/DVD drives and drives such as FAT32 or exFAT USB sticks can't hold a Windows image while it's being built.";
    else if (!GetShortPathNameW(p, sp, MAX_PATH) || !dlPathSimple(sp))
        why = L"UUP dump's converter needs a path without spaces, and this drive doesn't give folders short names. Choose a folder whose path has no spaces.";
    if (why) { okDialog(L"This folder can't be used", why); return; }
    lstrcpynW(g_dlWorkBase, p, MAX_PATH);
    dlWorkSave();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void dlWorkDefault(void) {
    g_dlWorkBase[0] = 0;
    dlWorkSave();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Clears what a build cut short can leave: a mounted image, loaded hives, unfinished downloads
static void dlCleanPressed(void) {
    if (g_busy) return;
    if (askInApp() && !showDialog(g_hwnd, L"Clean up leftovers?",
            L"If a build was cut short, for example by a crash or a power cut, this discards the Windows image it left "
            L"mounted, unloads that image's registry hives, and deletes the unfinished downloads and temporary folders.",
            NULL, 0, L"Clean up", L"Cancel", 1)) return;
    g_repShow = TRUE;
    dlpHide();
    wchar_t spec[MAX_PATH + 16];
    swprintf(spec, MAX_PATH + 16, g_dlWorkBase[0] ? L"cleanup|%s" : L"cleanup%s", g_dlWorkBase);
    repairStart(REP_UUP, spec);
}

static void dlCancelPressed(void) {
    if (g_repRunning && g_repJob == REP_UUP && g_dlCancelEv) {
        SetEvent(g_dlCancelEv);
        wcscpy(g_repStatus, L"Cancelling…");
    }
}

static void dlClick(int id) {
    if (id == A_D_CANCEL) { dlCancelPressed(); return; }
    if (g_busy) return;
    if (id == A_D_X64) dlSetArch(0);
    else if (id == A_D_ARM64) dlSetArch(1);
    else if (id == A_D_FIND) dlFind();
    else if (id == A_D_LANG) dlPickLanguage();
    else if (id == A_D_APPS) dlPickApps();
    else if (id == A_D_CLEAN) dlCleanPressed();
    else if (id == A_D_GO) dlStart();
    else if (id == A_D_WORK) dlPickWork();
    else if (id == A_D_WORKDEF) dlWorkDefault();
    else if (id >= A_D_ED0 && id <= A_D_EDLAST) {
        int i = id - A_D_ED0;
        // each tick stands alone: the extra editions are made from Pro, but the job downloads Pro for them itself
        // and leaves it out of the ISO when Pro isn't ticked
        if (i < g_dlEdN) g_dlEd[i].checked = !g_dlEd[i].checked;
    } else if (id >= A_D_V0 && id <= A_D_VLAST) {
        int i = id - A_D_V0;
        g_dlVirt[i] = !g_dlVirt[i];
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------- the page: drawing ----------
static void dlDrawCheck(int id, RECT rc, const wchar_t* label, BOOL checked, BOOL en) {
    if (en) aioRegister(id, rc, NULL, 0, FALSE);
    fillHover(rc.left, rc.top, rc.right, rc.bottom, dp(4), anim(ANIM_AIO + id, g_hotBtn == BTN_WIDGET + id && en));
    int mid = (rc.top + rc.bottom) / 2;
    drawCheckbox(rc.left + dp(14), mid, dp(16), anim(ANIM_AIOF + id, checked), en);
    RECT t = { rc.left + dp(30), rc.top, rc.right - dp(2), rc.bottom };
    drawText(g_fSmall, label, t, en ? textRGB() : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void drawDownload(int cL, int cR) {
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    BOOL en = (g_busy == 0);
    int x1 = cL + dp(20), x2 = cR - dp(20), gap = dp(10), bh = dp(36);

    // the labels' column fits the longest label in the language shown ("Arbeitsordner")
    int labelW = dp(68), rightW = dp(220);
    {
        static const wchar_t* LABELS[] = { L"Language", L"Apps", L"Work folder" };
        for (int i = 0; i < 3; i++) { int w = textWidth(g_fSmall, LABELS[i]) + dp(12); if (w > labelW) labelW = w; }
    }
    // kept compact (no heading over the build number, tight rows) so the output panel below gets the height
    int modeH = dp(32) + dp(26), rh = dp(34), rowGap = dp(6), edH = dp(56);
    int cardH = dp(82) + modeH + edH + dp(4) + rh + rowGap + rh + rowGap + rh + dp(12);
    dlWorkLoad();
    int cardB = top + cardH;
    fillRoundRect(cL, top, cR, cardB, dp(8), sf.r, sf.g, sf.b, sf.a);
    drawGlyph(g_fHeadIcon, 0xE896, cL + dp(44), top + dp(44), g_accent);
    RECT t1 = { cL + dp(84), top + dp(14), cR - dp(16), top + dp(46) };
    drawText(g_fH1, L"Download Windows", t1, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t2 = { cL + dp(84), top + dp(46), cR - dp(16), top + dp(68) };
    BOOL jobOn = g_repRunning && g_repJob == REP_UUP;
    BOOL ready = !jobOn && g_dlReady[0];
    drawText(jobOn || ready ? g_fBodyBold : g_fBody,
             jobOn ? L"UUP Converter is running. Do not turn off your PC" : ready ? g_dlReady : L"Builds a bootable Windows ISO on your Desktop, using UUP dump",
             t2, jobOn ? RGB(232, 72, 85) : ready ? noteGreen() : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    dlpDraw(x1, x2, cL + dp(84) + textWidth(g_fH1, L"Download Windows") + dp(12), top);

    // the build number, the architecture beside it (x64 or arm64, one always chosen), and Find
    int y = top + dp(82);
    int fw = dp(72), fh = dp(32), findW = dp(88);
    int archR = x2 - findW - dp(8);
    aioButton(A_D_X64, archR - fw * 2 - dp(6), y, archR - fw - dp(6), y + fh, DL_ARCHES[0].label, g_dlArch == 0, en, FALSE);
    aioButton(A_D_ARM64, archR - fw, y, archR, y + fh, DL_ARCHES[1].label, g_dlArch == 1, en, FALSE);

    RECT fr = { x1, y, archR - fw * 2 - dp(6) - dp(8), y + dp(32) };
    dlWantExample();
    wchar_t hint[80] = L"Build number";                  // the example appears when the lookup is done
    if (g_dlExampleState == 2) swprintf(hint, 80, L"Build number, for example %s%s", dlExample(), g_dlExample[0] ? L" (latest Release Preview)" : L"");
    aioField(A_D_BOX, fr, g_dlBox, 32, !en, hint);
    aioButton(A_D_FIND, x2 - findW, y, x2, y + dp(32), L"Find", FALSE, en && !g_dlQuery, FALSE);
    RECT m = { x1, y + dp(36), x2, y + dp(58) };
    if (g_dlMsg[0]) drawText(g_fSmall, g_dlMsg, m, g_dlMsgErr ? RGB(232, 72, 85) : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    else if (g_dlHas) drawText(g_fSmall, g_dlCur.title, m, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += modeH;

    // the editions: what UUP dump lists for this build, and the ones the converter can make from Pro
    BOOL hasPro = FALSE;
    for (int i = 0; i < g_dlEdN; i++) if (!_wcsicmp(g_dlEd[i].code, L"PROFESSIONAL")) hasPro = TRUE;
    // one even grid across two rows: three columns for the usual six, more if a build lists more
    int nShown = g_dlEdN + (hasPro ? DL_NVIRTUAL : 0);
    int cols = (nShown + 1) / 2;
    if (cols < 3) cols = 3;
    int cw = (x2 - x1) / cols, rowH = dp(26);
    if (!g_dlEdN) {
        RECT m = { x1, y, x2, y + edH };
        const wchar_t* tx = g_dlDetailsBusy ? L"Reading this build's editions and languages…" : L"Find a build to see its editions.";
        drawText(g_fSmall, tx, m, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        for (int i = 0; i < nShown; i++) {
            int col = i % cols, top = y + (i / cols) * dp(28);
            RECT r = { x1 + cw * col, top, x1 + cw * (col + 1) - dp(4), top + rowH };
            if (i < g_dlEdN) dlDrawCheck(A_D_ED0 + i, r, g_dlEd[i].name, g_dlEd[i].checked, en);
            else dlDrawCheck(A_D_V0 + (i - g_dlEdN), r, DL_VIRTUAL[i - g_dlEdN].label, g_dlVirt[i - g_dlEdN], en);
        }
    }
    y += edH + dp(4);
    bh = rh;

    // language, the apps, and the button that starts it all. The two pickers shrink before they'd reach the
    // buttons on the right; the window's minimum width keeps them from getting too small.
    int pickL = x1 + labelW, pickR = min(pickL + dp(240), x2 - rightW - gap);
    RECT ll = { x1, y, pickL, y + bh };
    drawText(g_fSmall, L"Language", ll, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    aioButton(A_D_LANG, pickL, y, pickR, y + bh, g_dlLangName, FALSE, en && g_dlLangN > 0, FALSE);
    aioButton(A_D_CLEAN, x2 - rightW, y, x2, y + bh, L"Clean up leftovers", FALSE, en, !g_elevated);
    y += bh + rowGap;
    RECT al = { x1, y, pickL, y + bh };
    drawText(g_fSmall, L"Apps", al, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    wchar_t appsTx[80]; dlAppsLabel(appsTx, 80);
    aioButton(A_D_APPS, pickL, y, pickR, y + bh, appsTx, FALSE, en && g_dlHas && g_dlAppsState == 1, FALSE);
    if (g_repRunning && g_repJob == REP_UUP)
        aioButton(A_D_CANCEL, x2 - rightW, y, x2, y + bh, L"Cancel", FALSE, TRUE, FALSE);
    else
        aioButton(A_D_GO, x2 - rightW, y, x2, y + bh, L"Download and build ISO", TRUE, en && g_dlHas && !g_dlDetailsBusy && dlAnyEdition(), !g_elevated);
    y += bh + rowGap;

    // where the work files go: Documents unless another folder is chosen, with the room on its drive
    RECT wl = { x1, y, pickL, y + bh };
    drawText(g_fSmall, L"Work folder", wl, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    wchar_t wp[MAX_PATH], wpShow[MAX_PATH], wf[80];
    dlWorkLabel(wp, MAX_PATH, wf, 80);
    lstrcpynW(wpShow, wp, MAX_PATH);
    {   // "D:\Builds  ·  880 GB free"; a long path is shortened in the middle, so its drive and last folders stay in view
        int room = (pickR - pickL) - dp(24);
        wchar_t t[MAX_PATH + 100], tail[90] = L"";
        if (wf[0]) swprintf(tail, 90, L"  ·  %s", tr(wf));
        swprintf(t, MAX_PATH + 100, L"%s%s", tr(wp), tail);
        size_t n = wcslen(wp);
        if (g_dlWorkBase[0] && n > 4 && textWidth(g_fBody, t) > room) {
            for (size_t keep = n - 4; keep > 0; keep--) {
                swprintf(t, MAX_PATH + 100, L"%.3s…%s%s", wp, wp + n - keep, tail);
                if (textWidth(g_fBody, t) <= room) break;
            }
        }
        lstrcpynW(wpShow, t, MAX_PATH);
    }
    aioButton(A_D_WORK, pickL, y, pickR, y + bh, wpShow, FALSE, en, FALSE);
    if (g_dlWorkBase[0]) aioButton(A_D_WORKDEF, x2 - rightW, y, x2, y + bh, L"Use Documents", FALSE, en, FALSE);

    // the conversion window: every step, what the tools print, and errors in red
    static const wchar_t* STEPS[] = { L"Getting ready…", L"Step 1 of 3: getting UUP dump's tools…", L"Step 2 of 3: downloading…", L"Step 3 of 3: converting to an ISO…" };
    LONG s = g_repStep;
    if (s < 0) s = 0;
    if (s > 3) s = 3;
    repDrawOutput(cL, cR, cardB + dp(12), L"Download and conversion", STEPS[s], TRUE);
}
