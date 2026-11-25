// Windows AIO, "ESD to ISO" tab: turns a Windows setup ESD into a bootable ISO holding the editions you tick, using
// abbodi1406's own ESD -> ISO script (esd-decrypter-wimlib in github.com/abbodi1406/BatUtil).
//
// Nothing of it is bundled. Every run fetches the current decrypt.cmd and its bin folder through GitHub's API, and each
// file must match the git hash GitHub lists for it. The script then runs as it is, unattended:
//   - DecryptConfig.ini sets AutoStart=1 (ISO with install.wim)
//   - one line added at the top of its editions menu hands it the ticked editions from WCU_PICK, in the same form its
//     own "randomly selected editions" choice takes them, so its own export code does the work
// It needs administrator rights, so it runs through the Windows Repair runner: one UAC prompt, live output, Cancel.
// The finished ISO gets the Download Windows page's naming, or keeps the name the script gives it.

#include <sddl.h>
#include <aclapi.h>

#define EI_API L"https://api.github.com/repos/abbodi1406/BatUtil/contents/esd-decrypter-wimlib"
#define EI_SAVED_PREFIX L"Done. ISO saved to "     // the job's last line, which is how the window learns the file's name
static const wchar_t* EI_CHANNELS[] = { L"Retail", L"Release Preview", L"Beta", L"Dev", L"Canary", L"None" };
#define EI_NCHANNELS 6
#define EI_NO_CHANNEL 5

// ---------- the name ----------
// The version a build number was released as, for names whose branch doesn't say it (26100's is just "ge_release")
static const wchar_t* eiVersionOf(int build) {
    static const struct { int b; const wchar_t* v; } T[] = {
        { 28000, L"26H1" }, { 26300, L"26H2" }, { 26200, L"25H2" }, { 26100, L"24H2" }, { 22631, L"23H2" }, { 22621, L"22H2" },
        { 22000, L"21H2" }, { 19045, L"22H2" }, { 19044, L"21H2" }, { 19043, L"21H1" }, { 19042, L"20H2" } };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (T[i].b == build) return T[i].v;
    return NULL;
}

// Our naming, the same as the Download Windows page: <build>_<version>_<arch>_<channel>_<DD>_<Mon>_<YY>.iso.
// label is a name in Microsoft's style, <build>.<rev>.<yymmdd>-<hhmm>.<branch>_..., which is how decrypt.cmd names its
// ISO and Microsoft names its ESDs; the date is the build's. FALSE if label isn't one. channel may be empty.
static BOOL eiOurName(const wchar_t* label, const wchar_t* arch, const wchar_t* channel, wchar_t* out, int cch) {
    static const wchar_t* MONTHS[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    wchar_t* e = NULL;
    if (!iswdigit(label[0])) return FALSE;
    long major = wcstol(label, &e, 10);
    if (*e != L'.' || !iswdigit(e[1])) return FALSE;
    long rev = wcstol(e + 1, &e, 10);
    if (*e != L'.') return FALSE;
    const wchar_t* p = e + 1;
    for (int i = 0; i < 6; i++) if (!iswdigit(p[i])) return FALSE;
    int y = (p[0] - L'0') * 10 + (p[1] - L'0'), m = (p[2] - L'0') * 10 + (p[3] - L'0'), d = (p[4] - L'0') * 10 + (p[5] - L'0');
    if (m < 1 || m > 12 || d < 1 || d > 31) return FALSE;
    wchar_t branch[32] = L"", ver[16] = L"", part[64];
    const wchar_t* dot = wcschr(p + 6, L'.');            // past "-hhmm"
    if (dot) {
        int n = 0;
        for (dot++; dot[n] && dot[n] != L'_' && dot[n] != L'.' && n < 31; n++) branch[n] = dot[n];
        branch[n] = 0;
    }
    if (wcslen(branch) == 4 && iswdigit(branch[0]) && iswdigit(branch[1]) && towlower(branch[2]) == L'h' && iswdigit(branch[3]))
        swprintf(ver, 16, L"%c%cH%c", branch[0], branch[1], branch[3]);
    else if (eiVersionOf((int)major)) wcscpy(ver, eiVersionOf((int)major));
    swprintf(out, cch, L"%ld.%ld", major, rev);
    if (ver[0]) { swprintf(part, 64, L"_%s", ver); wcsncat(out, part, cch - wcslen(out) - 1); }
    if (arch && arch[0]) { swprintf(part, 64, L"_%s", arch); wcsncat(out, part, cch - wcslen(out) - 1); }
    if (channel && channel[0]) { swprintf(part, 64, L"_%s", channel); wcsncat(out, part, cch - wcslen(out) - 1); }
    swprintf(part, 64, L"_%02d_%s_%02d.iso", d, MONTHS[m - 1], y);
    wcsncat(out, part, cch - wcslen(out) - 1);
    for (wchar_t* c = out; *c; c++) if (wcschr(L"\\/:*?\"<>|", *c) || *c < 32) *c = L'-';
    return TRUE;
}
// x64 and arm64 are written the way Download Windows writes them
static const wchar_t* eiArchLabel(const wchar_t* a) {
    if (!_wcsicmp(a, L"ARM64")) return L"arm64";
    return a;
}

// Every image of a WIM or ESD, setup images included. The count, or -1.
static int eiReadImages(const wchar_t* path, WimEntry* out, int max) {
    if (!wimLoadApi()) return -1;
    wchar_t tmp[MAX_PATH]; wimTempDir(tmp);
    DWORD created;
    HANDLE h = pWIMCreateFile(path, GENERIC_READ, WIM_OPEN_EXISTING, 0, 0, &created);
    if (!h) return -1;
    pWIMSetTemporaryPath(h, tmp);
    int n = (int)pWIMGetImageCount(h), got = 0;
    for (int i = 1; i <= n && got < max; i++) {
        HANDLE img = pWIMLoadImage(h, i);
        if (!img) { got = -1; break; }
        if (wimReadImageInfo(img, path, i, &out[got])) got++;
        pWIMCloseHandle(img);
    }
    pWIMCloseHandle(h);
    return got;
}

// ---------- the job's work folder ----------
// <Documents>\WCU_ESD, or <system drive>\WCU_ESD when Documents can't be used, the same rule as Download Windows.
// Made fresh each run, owned by and open only to administrators, held open so it can't be swapped, and the script
// gets a plain path (the long one, else the 8.3 one): decrypt.cmd refuses a path with spaces or symbols, ~ included. Removed at the end of every run.
static wchar_t g_eiRoot[MAX_PATH];
static HANDLE  g_eiRootH = INVALID_HANDLE_VALUE;

static BOOL eiPathSimple(const wchar_t* p) {
    for (; *p; p++)
        if (!((*p >= L'A' && *p <= L'Z') || (*p >= L'a' && *p <= L'z') || (*p >= L'0' && *p <= L'9') || wcschr(L"\\:._-", *p))) return FALSE;   // decrypt.cmd also refuses ~, so 8.3 names rarely help
    return TRUE;
}
static void eiRemoveTree(const wchar_t* dir) {
    DWORD a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) { if (!RemoveDirectoryW(dir)) DeleteFileW(dir); return; }   // only the link
    wchar_t* lp = longPath(dir);
    if (!lp) return;
    deleteContents(lp);
    RemoveDirectoryW(lp);
    free(lp);
}
static BOOL eiOwnedByAdmins(HANDLE h) {
    PSID owner = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) != ERROR_SUCCESS) return FALSE;
    BYTE ba[SECURITY_MAX_SID_SIZE]; DWORD baN = sizeof(ba);
    BOOL ok = owner && CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, ba, &baN) && EqualSid(owner, ba);
    LocalFree(sd);
    return ok;
}
static DWORD eiRootTry(const wchar_t* path) {
    wchar_t fin[MAX_PATH + 8], shortP[MAX_PATH];
    HANDLE probe = CreateFileW(path, FILE_LIST_DIRECTORY, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) return ERROR_BUSY;   // another run holds it
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    eiRemoveTree(path);                          // left by a run that never reached its end, such as a power cut
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &sd, NULL))
        return GetLastError();
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    DWORD err = CreateDirectoryW(path, &sa) ? 0 : GetLastError();
    LocalFree(sd);
    if (err) return err;
    HANDLE h = CreateFileW(path, FILE_LIST_DIRECTORY | READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !eiOwnedByAdmins(h)) {
        CloseHandle(h);
        return ERROR_ACCESS_DENIED;
    }
    DWORD n = GetFinalPathNameByHandleW(h, fin, MAX_PATH + 8, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const wchar_t* p = fin;
    if (n && n < MAX_PATH + 8 && !wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    BOOL ok = n && n < MAX_PATH + 8 && wcsncmp(p, L"UNC\\", 4);
    if (ok && wcslen(p) < MAX_PATH && eiPathSimple(p)) lstrcpynW(shortP, p, MAX_PATH);
    else if (!ok || !GetShortPathNameW(p, shortP, MAX_PATH) || !eiPathSimple(shortP)) {
        CloseHandle(h);
        eiRemoveTree(path);
        return ERROR_BAD_PATHNAME;
    }
    lstrcpynW(g_eiRoot, shortP, MAX_PATH);
    g_eiRootH = h;
    return 0;
}
static DWORD eiRootMake(HANDLE out) {
    wchar_t docs[MAX_PATH], sys[8] = L"C:", path[MAX_PATH + 16], line[MAX_PATH + 200];
    g_eiRoot[0] = 0;
    DWORD e = ERROR_PATH_NOT_FOUND;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs)) && wcsncmp(docs, L"\\\\", 2)) {
        swprintf(path, MAX_PATH + 16, L"%s\\WCU_ESD", docs);
        e = eiRootTry(path);
        if (!e || e == ERROR_BUSY) goto said;
    }
    GetEnvironmentVariableW(L"SystemDrive", sys, 8);
    swprintf(path, MAX_PATH + 16, L"%s\\WCU_ESD", sys);
    repWrite(out, L"Documents can't hold the work files, so they go in ");
    repWrite(out, path);
    repWrite(out, L".\r\n");
    e = eiRootTry(path);
said:
    if (e == ERROR_BUSY) {
        swprintf(line, MAX_PATH + 200, L"Error: another ESD to ISO job is using %s. Wait for it to finish, or cancel it.\r\n", path);
        repWrite(out, line);
    } else if (e) {
        swprintf(line, MAX_PATH + 200, L"Error: couldn't make the work folder %s (error %lu).\r\n", path, e);
        repWrite(out, line);
    }
    return e;
}
static void eiRootRemove(HANDLE out) {
    if (!g_eiRoot[0]) return;
    repWrite(out, L"Deleting the work folder\r\n");
    wchar_t* lp = longPath(g_eiRoot);
    if (lp) { deleteContents(lp); free(lp); }
    if (g_eiRootH != INVALID_HANDLE_VALUE) { CloseHandle(g_eiRootH); g_eiRootH = INVALID_HANDLE_VALUE; }
    eiRemoveTree(g_eiRoot);
    g_eiRoot[0] = 0;
}

// ---------- the job: abbodi1406's script ----------
static BOOL eiFetchFolder(const wchar_t* api, const wchar_t* dir, HANDLE out, int* count) {
    GhFile* list = (GhFile*)malloc(sizeof(GhFile) * 48);
    if (!list) return FALSE;
    int n = ghList(api, list, 48);
    BOOL ok = n >= 0;
    for (int i = 0; ok && i < n; i++) {
        wchar_t path[MAX_PATH];
        if (!ghFetch(&list[i], dir, path)) ok = FALSE;
        else (*count)++;
    }
    if (!ok) { repWrite(out, L"Error: "); repWrite(out, g_esdErr); repWrite(out, L"\r\n"); }
    free(list);
    return ok;
}
// decrypt.cmd, its DecryptConfig.ini and the whole bin folder (wimlib, cdimage, 7-Zip and the rest), as the repo has them now
static BOOL eiFetchTools(HANDLE out) {
    wchar_t bin[MAX_PATH], bin64[MAX_PATH], script[MAX_PATH], line[200];
    swprintf(bin, MAX_PATH, L"%s\\bin", g_eiRoot);
    swprintf(bin64, MAX_PATH, L"%s\\bin\\bin64", g_eiRoot);
    CreateDirectoryW(bin, NULL); CreateDirectoryW(bin64, NULL);
    int n = 0;
    if (!eiFetchFolder(EI_API, g_eiRoot, out, &n) || !eiFetchFolder(EI_API L"/bin", bin, out, &n)
        || !eiFetchFolder(EI_API L"/bin/bin64", bin64, out, &n)) return FALSE;
    swprintf(script, MAX_PATH, L"%s\\decrypt.cmd", g_eiRoot);
    if (!isFile(script)) { repWrite(out, L"Error: abbodi1406's repo no longer has esd-decrypter-wimlib\\decrypt.cmd.\r\n"); return FALSE; }
    swprintf(line, 200, L"%d files downloaded, each matching the hash GitHub lists for it.\r\n", n);
    repWrite(out, line);
    return TRUE;
}

// Hands the script the ticked editions. Its menu is skipped when WCU_PICK is set, and WCU_PICK is read the way its own
// "randomly selected editions" answer is: edition numbers 1, 2, 3... separated by spaces. Each of the two places must
// be found exactly once, or nothing is changed and the job stops, as the script would then have changed shape.
static BOOL eiPatchScript(HANDLE out) {
    static const char A[] = "if %AutoStart% equ 5 (set _single=4&goto :MAINMENU)\r\n";
    static const char B[] = "\r\n:setindex\r\n";
    static const char ADD_A[] = "if defined WCU_PICK (call :WCUPICK&goto :MAINMENU)\r\n";
    static const char ADD_B[] =
        "\r\n:WCUPICK\r\n"
        "set _count=0\r\n"
        "for %%# in (%WCU_PICK%) do call :setindex %%#\r\n"
        "if %_count% equ 1 (set /a _single=%_index1%+3&goto :eof)\r\n"
        "for /L %%# in (1,1,%_count%) do set /a _index%%#+=3\r\n"
        "set \"_index=%WCU_PICK%\"\r\n"
        "goto :eof\r\n";
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\decrypt.cmd", g_eiRoot);
    char* t = dlReadSmallFile(path, 4 << 20);
    if (!t) { repWrite(out, L"Error: couldn't read decrypt.cmd.\r\n"); return FALSE; }
    // Nobody is at its keyboard: its "press any key" waits become no-ops (what its own debug mode does), so an
    // error ends the run and shows here instead of hanging. Same length, so nothing else moves.
    for (char* q = t; (q = strstr(q, "pause >nul")) != NULL; q += 10) memcpy(q, "rem.      ", 10);
    char* a = strstr(t, A); char* b = strstr(t, B);
    if (!a || !b || strstr(a + 1, A) || strstr(b + 1, B) || b < a) {
        repWrite(out, L"Error: decrypt.cmd has changed in a way this app doesn't know, so it can't be given the editions. Nothing was run.\r\n");
        free(t);
        return FALSE;
    }
    size_t len = strlen(t);
    char* o = (char*)malloc(len + sizeof(ADD_A) + sizeof(ADD_B) + 8);
    BOOL ok = FALSE;
    if (o) {
        size_t ia = a - t, ib = b - t, k = 0;
        memcpy(o + k, t, ia); k += ia;
        memcpy(o + k, ADD_A, sizeof(ADD_A) - 1); k += sizeof(ADD_A) - 1;
        memcpy(o + k, a, ib - ia); k += ib - ia;
        memcpy(o + k, ADD_B, sizeof(ADD_B) - 1); k += sizeof(ADD_B) - 1;
        memcpy(o + k, b, len - ib); k += len - ib;
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD w = 0;
        if (f != INVALID_HANDLE_VALUE) { ok = WriteFile(f, o, (DWORD)k, &w, NULL) && w == k; CloseHandle(f); }
        free(o);
    }
    free(t);
    if (!ok) repWrite(out, L"Error: couldn't write decrypt.cmd.\r\n");
    return ok;
}
// AutoStart=1 is "ISO with install.wim". The rest are the script's own defaults, written out so a changed default
// upstream can't make it stop and wait at a menu.
static BOOL eiWriteConfig(HANDLE out) {
    static const char INI[] = "[decrypt-ESD]\r\nAutoStart    =1\r\nISOnameESD   =0\r\nSkipISO      =0\r\nMultiChoice  =1\r\nCheckWinre   =1\r\n";
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%s\\DecryptConfig.ini", g_eiRoot);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0; BOOL ok = FALSE;
    if (f != INVALID_HANDLE_VALUE) { ok = WriteFile(f, INI, sizeof(INI) - 1, &w, NULL) && w == sizeof(INI) - 1; CloseHandle(f); }
    if (!ok) repWrite(out, L"Error: couldn't write DecryptConfig.ini.\r\n");
    return ok;
}

// The newest .iso the script left in the work folder
static BOOL eiFindIso(wchar_t* name /* MAX_PATH */) {
    wchar_t pat[MAX_PATH + 8]; swprintf(pat, MAX_PATH + 8, L"%s\\*.iso", g_eiRoot);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FILETIME best = {0}; name[0] = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; lstrcpynW(name, fd.cFileName, MAX_PATH); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return name[0] != 0;
}

// Opens the finished ISO and checks its install.wim holds exactly the editions asked for, in that order
static BOOL eiCheckIso(const wchar_t* iso, const WimEntry* want, int nWant, HANDLE out) {
    HANDLE vd = NULL; wchar_t root[4], line[400];
    DWORD r = isoMount(iso, &vd, root);
    if (r) { swprintf(line, 400, L"Error: couldn't open the ISO to check it (error %lu).\r\n", r); repWrite(out, line); return FALSE; }
    wchar_t wim[32]; swprintf(wim, 32, L"%ssources\\install.wim", root);
    WimEntry* got = (WimEntry*)malloc(sizeof(WimEntry) * WIM_MAX);
    int n = got ? eiReadImages(wim, got, WIM_MAX) : -1;
    BOOL ok = n == nWant;
    for (int i = 0; ok && i < n; i++) if (_wcsicmp(got[i].edition, want[i].edition)) ok = FALSE;
    if (n > 0) {
        repWrite(out, L"The ISO's install.wim holds:\r\n");
        for (int i = 0; i < n; i++) {
            swprintf(line, 400, L"  %d  %s  (%s, %s, %s)\r\n", got[i].index, got[i].name, got[i].edition, got[i].arch, got[i].build);
            repWrite(out, line);
        }
    }
    if (!ok) repWrite(out, n < 0 ? L"Error: the ISO has no readable sources\\install.wim.\r\n"
                                 : L"Error: the ISO doesn't hold the editions that were ticked.\r\n");
    free(got);
    isoUnmount(vd);
    return ok;
}

// Moves the ISO out of the administrators-only folder. It takes its new folder's rights, so a normal window can open it.
static BOOL eiDeliver(const wchar_t* from, const wchar_t* dir, const wchar_t* name, wchar_t* finalPath, int cch) {
    wchar_t base[MAX_PATH]; lstrcpynW(base, name, MAX_PATH);
    wchar_t* dot = wcsrchr(base, L'.');
    if (dot) *dot = 0;
    swprintf(finalPath, cch, L"%s\\%s.iso", dir, base);
    for (int i = 2; GetFileAttributesW(finalPath) != INVALID_FILE_ATTRIBUTES && i < 100; i++)
        swprintf(finalPath, cch, L"%s\\%s (%d).iso", dir, base, i);
    if (!MoveFileExW(from, finalPath, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return FALSE;
    ACL empty;
    if (InitializeAcl(&empty, sizeof(empty), ACL_REVISION))
        SetNamedSecurityInfoW(finalPath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL, &empty, NULL);
    return TRUE;
}

// esd|picks|folder|naming (0 ours, 1 the script's)|channel|cancel event
typedef struct { wchar_t esd[MAX_PATH], picks[400], dir[MAX_PATH], channel[32], cancel[64]; int mode; } EiSpec;
static BOOL eiSpecParse(const wchar_t* t, EiSpec* s) {
    memset(s, 0, sizeof(*s));
    wchar_t* f[6]; wchar_t buf[REP_ARG_MAX];
    lstrcpynW(buf, t, REP_ARG_MAX);
    int n = 0; f[n++] = buf;
    for (wchar_t* p = buf; *p && n < 6; p++) if (*p == L'|') { *p = 0; f[n++] = p + 1; }
    if (n != 6 || wcschr(f[5], L'|')) return FALSE;
    if (wcslen(f[0]) >= MAX_PATH || wcslen(f[1]) >= 400 || wcslen(f[2]) >= MAX_PATH || wcslen(f[4]) >= 32 || wcslen(f[5]) >= 64) return FALSE;
    wcscpy(s->esd, f[0]); wcscpy(s->picks, f[1]); wcscpy(s->dir, f[2]); wcscpy(s->channel, f[4]); wcscpy(s->cancel, f[5]);
    for (const wchar_t* p = s->picks; *p; p++) if (!iswdigit(*p) && *p != L' ') return FALSE;   // it reaches the script
    if (wcscmp(f[3], L"0") && wcscmp(f[3], L"1")) return FALSE;
    s->mode = f[3][0] - L'0';
    BOOL chOk = !s->channel[0];
    for (int i = 0; i < EI_NO_CHANNEL; i++) if (!wcscmp(s->channel, EI_CHANNELS[i])) chOk = TRUE;
    if (!chOk) return FALSE;
    if (s->cancel[0] && wcsncmp(s->cancel, L"Local\\wcu-esd-stop-", 19)) return FALSE;
    return s->esd[0] && s->dir[0] && PathIsRelativeW(s->esd) == FALSE && PathIsRelativeW(s->dir) == FALSE;
}

static void eiStep(HANDLE out, const wchar_t* title) {
    wchar_t mark[2] = { REP_STEP_MARK, 0 };
    repWrite(out, mark);
    repWrite(out, L"\r\n> ");
    repWrite(out, title);
    repWrite(out, L"\r\n");
}

static DWORD esdIsoRunJob(const wchar_t* specText, HANDLE out) {
    static EiSpec s; static WimEntry imgs[WIM_MAX], want[WIM_MAX];
    wchar_t line[MAX_PATH * 2 + 300];
    if (!eiSpecParse(specText, &s)) { repWrite(out, L"Error: the request wasn't valid. Nothing was run.\r\n"); return 1; }
    SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", NULL);    // the script runs its tools by bare name
    HANDLE cancel = s.cancel[0] ? OpenEventW(SYNCHRONIZE, FALSE, s.cancel) : NULL;
    if (!cancel) cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
#define EI_STOPPED() (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
    DWORD result = 1;

    // ---- the ESD: setup images 1 to 3, then the editions, which is what decrypt.cmd expects ----
    eiStep(out, L"Reading the ESD");
    int n = eiReadImages(s.esd, imgs, WIM_MAX);
    if (n < 4) {
        repWrite(out, n < 0 ? L"Error: Windows couldn't read the ESD. An encrypted ESD (an old Windows Update download) can't be used.\r\n"
                            : L"Error: this isn't a Windows setup ESD: it needs Microsoft's 3 setup images and at least one edition.\r\n");
        goto done;
    }
    int nWant = 0;
    if (!s.picks[0]) for (int i = 3; i < n; i++) want[nWant++] = imgs[i];
    else {
        wchar_t tmp[400], *ctx = NULL; wcscpy(tmp, s.picks);
        for (wchar_t* t = wcstok(tmp, L" ", &ctx); t; t = wcstok(NULL, L" ", &ctx)) {
            int k = _wtoi(t);
            if (k < 1 || k > n - 3 || nWant >= WIM_MAX) { repWrite(out, L"Error: an edition asked for isn't in this ESD.\r\n"); goto done; }
            want[nWant++] = imgs[k + 2];
        }
    }
    if (!nWant) { repWrite(out, L"Error: no editions were chosen.\r\n"); goto done; }
    swprintf(line, MAX_PATH * 2 + 300, L"%s\r\nBuild %s %s. Editions for the ISO, in this order:\r\n", s.esd, want[0].build, want[0].arch);
    repWrite(out, line);
    for (int i = 0; i < nWant; i++) { swprintf(line, 400, L"  %s  (%s)\r\n", want[i].name, want[i].edition); repWrite(out, line); }

    // ---- abbodi1406's script, as the repo has it today ----
    eiStep(out, L"Getting abbodi1406's ESD -> ISO script (esd-decrypter-wimlib)");
    if (eiRootMake(out)) goto done;
    {   // room: install.wim and the ISO are each well over the ESD's size
        WIN32_FILE_ATTRIBUTE_DATA fa; ULARGE_INTEGER freeB;
        if (GetFileAttributesExW(s.esd, GetFileExInfoStandard, &fa) && GetDiskFreeSpaceExW(g_eiRoot, &freeB, NULL, NULL)) {
            unsigned long long need = fileBytes(fa.nFileSizeHigh, fa.nFileSizeLow) * 3 + (4ull << 30);
            if (freeB.QuadPart < need) {
                wchar_t a[48], b[48]; fmtSize(need, a, 48); fmtSize(freeB.QuadPart, b, 48);
                swprintf(line, MAX_PATH * 2 + 300, L"Error: the work folder's drive needs about %s free for this ESD, and has %s.\r\n", a, b);
                repWrite(out, line);
                goto done;
            }
        }
    }
    if (!eiFetchTools(out) || !eiPatchScript(out) || !eiWriteConfig(out)) goto done;
    if (EI_STOPPED()) goto stopped;

    // ---- the script itself ----
    eiStep(out, L"Making the ISO with decrypt.cmd");
    // A link to the ESD inside the work folder, so the script sees a plain path whatever the real one holds (it can't
    // take symbols such as & or !), and it never renames the real file (it does that to one called install.esd).
    wchar_t esdArg[MAX_PATH];
    swprintf(esdArg, MAX_PATH, L"%s\\source.esd", g_eiRoot);
    if (!CreateHardLinkW(esdArg, s.esd, NULL)) {
        if (wcspbrk(s.esd, L"!%^&()=;,'`+[]{}~@")) {
            repWrite(out, L"Error: the ESD's path has symbols decrypt.cmd can't take, and it's on another drive from the work folder. Move it to a plain folder, such as one on the Desktop.\r\n");
            goto done;
        }
        if (!_wcsicmp(PathFindFileNameW(s.esd), L"install.esd")) {
            repWrite(out, L"Error: decrypt.cmd renames a file called install.esd while it works. Rename the ESD, or put it on the same drive as Documents.\r\n");
            goto done;
        }
        lstrcpynW(esdArg, s.esd, MAX_PATH);
    }
    SetEnvironmentVariableW(L"WCU_PICK", s.picks[0] ? s.picks : NULL);
    wchar_t cmdExe[MAX_PATH], cmd[MAX_PATH * 3 + 64]; DWORD code = 0;
    systemPath(cmdExe, L"cmd.exe");
    // -qedit: the script's own flag for "console already set up"; without it, it restarts itself through PowerShell
    swprintf(cmd, MAX_PATH * 3 + 64, L"cmd.exe /d /c \"\"%s\\decrypt.cmd\" \"%s\" -qedit\"", g_eiRoot, esdArg);
    DWORD e = repRunOnConsole(out, cmdExe, cmd, g_eiRoot, cancel, &code);
    SetEnvironmentVariableW(L"WCU_PICK", NULL);
    if (e) { swprintf(line, 300, L"Error: couldn't start decrypt.cmd (error %lu).\r\n", e); repWrite(out, line); goto done; }
    if (code == ERROR_CANCELLED) goto stopped;

    // ---- check it, name it, save it ----
    eiStep(out, L"Checking and saving the ISO");
    wchar_t iso[MAX_PATH], from[MAX_PATH * 2], name[MAX_PATH], finalPath[MAX_PATH * 2];
    if (!eiFindIso(iso)) { repWrite(out, L"\r\nError: decrypt.cmd finished without making an ISO. See its messages above.\r\n"); goto done; }
    swprintf(from, MAX_PATH * 2, L"%s\\%s", g_eiRoot, iso);
    if (!eiCheckIso(from, want, nWant, out)) goto done;
    wchar_t label[MAX_PATH]; lstrcpynW(label, iso, MAX_PATH);
    PathRemoveExtensionW(label);
    if (s.mode == 0 && eiOurName(label, eiArchLabel(want[0].arch), s.channel, name, MAX_PATH)) {}
    else {
        if (s.mode == 0) repWrite(out, L"The script's ISO name doesn't carry a build date, so its own name is kept.\r\n");
        swprintf(name, MAX_PATH, L"%s.iso", label);
    }
    if (!eiDeliver(from, s.dir, name, finalPath, MAX_PATH * 2)) {
        DWORD me = GetLastError();
        swprintf(line, MAX_PATH * 2 + 300, me == ERROR_DISK_FULL || me == ERROR_HANDLE_DISK_FULL
                 ? L"\r\nError: the ISO was made, but %s ran out of space while it was being moved there.\r\n"
                 : L"\r\nError: the ISO was made but couldn't be moved to %s.\r\n", s.dir);
        repWrite(out, line);
        goto done;
    }
    eiRootRemove(out);              // before the last line, which the window reads as the end
    swprintf(line, MAX_PATH * 2 + 300, L"\r\n" EI_SAVED_PREFIX L"%s\r\n", finalPath);
    repWrite(out, line);
    result = 0;
    goto done;

stopped:
    result = REP_CANCELLED;
    repWrite(out, L"\r\nCancelled.\r\n");
done:
    eiRootRemove(out);
    CloseHandle(cancel);
#undef EI_STOPPED
    return result;
}

// ---------- the tab ----------
static int  g_eiNameMode = 0;                // 0: our naming, 1: the script's
static int  g_eiChannel = 0;                 // into EI_CHANNELS
static HANDLE g_eiCancelEv = NULL;

static BOOL eiJobOn(void) { return g_repRunning && g_repJob == REP_ESDISO; }
static int esdTicked(void) {
    int n = 0;
    for (LONG i = 0; i < g_esdN; i++) if (g_esdTick[i]) n++;
    return n;
}
static void eiDefaultDir(void) {
    if (!g_eDir[0] && FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, g_eDir))) g_eDir[0] = 0;
}
// What the ISO will be called, as far as the ESD's own name tells
static void eiPreview(wchar_t* out, int cch) {
    if (g_eiNameMode == 1) { wcscpy(out, L"decrypt.cmd's own name, in Microsoft's style"); return; }
    wchar_t label[MAX_PATH], name[MAX_PATH]; const wchar_t* arch = L"";
    lstrcpynW(label, PathFindFileNameW(g_esdLoaded), MAX_PATH);
    PathRemoveExtensionW(label);
    for (LONG i = 0; i < g_esdN; i++) if (g_esdTick[i]) { arch = eiArchLabel(g_esd[i].arch); break; }
    if (g_esdLoaded[0] && eiOurName(label, arch, g_eiChannel == EI_NO_CHANNEL ? L"" : EI_CHANNELS[g_eiChannel], name, MAX_PATH))
        lstrcpynW(out, name, cch);
    else wcscpy(out, L"named from the build once the ISO is made");
}
// The last thing the script printed, for the status line, and any percentage in it
static void eiLastLine(wchar_t* out, int cch, int* pct) {
    out[0] = 0; *pct = -1;
    EnterCriticalSection(&g_lock);
    for (int i = g_repN - 1; i >= 0 && !out[0]; i--) {
        const wchar_t* s = g_repLines[i].s;
        if (!s) continue;
        while (*s == L' ') s++;
        if (!*s || !wcsncmp(s, L"=====", 5)) continue;
        if (*s == L'>' && s[1] == L' ') s += 2;
        lstrcpynW(out, s, cch);
    }
    LeaveCriticalSection(&g_lock);
    wchar_t* pc = wcsrchr(out, L'%');
    if (pc && pc > out && iswdigit(pc[-1])) { wchar_t* d = pc; while (d > out && iswdigit(d[-1])) d--; *pct = _wtoi(d); }
}

static void paneEsd(int x1, int x2, int top, int bottom) {
    BOOL idle = (g_busy == 0), job = eiJobOn();
    LONG count = g_esdN;
    BOOL listed = count > 0 && g_esdLoaded[0];
    int ticked = esdTicked();
    eiDefaultDir();
    int y = aioPathRow(x1, x2, top, L"ESD file", A_E_SRC, g_eSrc, A_E_SRCB);

    wchar_t head[64];
    if (listed) swprintf(head, 64, L"Editions to keep  (%d of %ld)", ticked, (long)count);
    else wcscpy(head, L"Editions to keep");
    RECT hl = { x1, y, x2 - dp(232), y + dp(32) };
    drawText(g_fBodyBold, head, hl, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    aioButton(A_E_ALL, x2 - dp(224), y, x2 - dp(116), y + dp(32), L"Select all", FALSE, idle && listed && ticked < count, FALSE);
    aioButton(A_E_NONE, x2 - dp(108), y, x2, y + dp(32), L"Select none", FALSE, idle && listed && ticked > 0, FALSE);
    y += dp(40);

    // the list: tick, Index, Name, Edition ID, Arch, Build
    int listB = bottom - dp(214);
    Col s = sepCol();
    Col lf = g_dark ? (Col){255,255,255,0.03} : (Col){255,255,255,0.45};
    fillRoundRect(x1, y, x2, listB, dp(4), lf.r, lf.g, lf.b, lf.a);
    strokeRoundRect(x1, y, x2, listB, dp(4), dp(1), s.r, s.g, s.b, s.a * 0.7);
    int cw[5];
    cw[0] = textWidth(g_fSmall, L"Index") + dp(14);
    cw[2] = textWidth(g_fSmall, L"ProfessionalWorkstationN") + dp(14);
    cw[3] = textWidth(g_fSmall, L"ARM64") + dp(14);
    cw[4] = textWidth(g_fSmall, L"26100.1742") + dp(10);
    int cx0 = x1 + dp(40);
    cw[1] = max(dp(80), (x2 - dp(10)) - cx0 - cw[0] - cw[2] - cw[3] - cw[4]);
    int cx[5]; cx[0] = cx0;
    for (int c = 1; c < 5; c++) cx[c] = cx[c - 1] + cw[c - 1];
    RECT clip = { x1 + dp(2), y, x2 - dp(8), listB };
    g_textClip = clip; g_textClipOn = TRUE;
    static const wchar_t* hdr[5] = { L"Index", L"Name", L"Edition ID", L"Arch", L"Build" };
    for (int c = 0; c < 5; c++) {
        RECT r = { cx[c], y, cx[c] + cw[c] - dp(6), y + dp(28) };
        drawText(g_fSmall, hdr[c], r, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    fillRect(x1 + dp(1), y + dp(28), x2 - dp(1), y + dp(28) + 1, s.r, s.g, s.b, s.a);
    int ry = y + dp(29), rowH = dp(30), bodyB = listB - dp(2);
    int visible = (bodyB - ry) / rowH;
    if (visible > ESD_ROWS) visible = ESD_ROWS;
    if (visible < 1) visible = 1;
    if (g_esdTop > count - visible) g_esdTop = count - visible;
    if (g_esdTop < 0) g_esdTop = 0;
    g_esdRowsTop = ry; g_esdRowH = rowH; g_esdVisible = visible;
    if (!listed) {
        RECT r = { x1, ry, x2, bodyB };
        drawText(g_fSmall, g_aioRunning && g_aioJob == JOB_ESDLOAD ? L"Reading the editions…"
                 : L"Choose an ESD file, or type its path and press Enter, to list the Windows editions inside it.",
                 r, mutedRGB(), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    for (int v = 0; listed && v < visible && g_esdTop + v < count; v++) {
        int i = g_esdTop + v, id = A_E_ROW0 + v, t = ry + v * rowH;
        WimEntry* e = &g_esd[i];
        RECT row = { x1 + dp(3), t + dp(1), x2 - dp(9), t + rowH - dp(1) };
        if (idle) aioRegister(id, row, NULL, 0, FALSE);
        double hv = anim(ANIM_AIO + id, g_hotBtn == BTN_WIDGET + id && idle);
        fillHover(row.left, row.top, row.right, row.bottom, dp(3), hv);
        drawCheckbox(x1 + dp(20), t + rowH / 2, dp(16), anim(ANIM_AIOF + id, g_esdTick[i]), idle);
        wchar_t idx[16]; swprintf(idx, 16, L"%d", e->index);
        const wchar_t* val[5] = { idx, e->name, e->edition, e->arch, e->build };
        for (int c = 0; c < 5; c++) {
            RECT r = { cx[c], t, cx[c] + cw[c] - dp(6), t + rowH };
            drawText(g_fSmall, val[c], r, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    g_textClipOn = FALSE;
    if (listed && count > visible) {      // scroll indicator; the wheel scrolls
        Col barCol = textCol();
        int trackT = ry + dp(2), trackH = bodyB - ry - dp(4);
        int th = max(dp(16), trackH * visible / count);
        int ty = trackT + (trackH - th) * g_esdTop / max(1, count - visible);
        fillRoundRect(x2 - dp(6), ty, x2 - dp(3), ty + th, dp(1), barCol.r, barCol.g, barCol.b, 0.35);
    }

    // where, and what it's called
    y = listB + dp(10);
    y = aioPathRow(x1, x2, y, L"Save ISO in", A_E_DIR, g_eDir, A_E_DIRB);
    RECT nl = { x1, y, x1 + dp(104), y + dp(30) };
    drawText(g_fSmall, L"ISO name", nl, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int bx = x1 + dp(108), bw = dp(150);
    aioButton(A_E_NOURS, bx, y, bx + bw, y + dp(30), L"Our naming", g_eiNameMode == 0, idle, FALSE);
    aioButton(A_E_NABB, bx + bw + dp(6), y, bx + bw * 2 + dp(6), y + dp(30), L"abbodi1406's naming", g_eiNameMode == 1, idle, FALSE);
    y += dp(36);
    RECT cl = { x1, y, x1 + dp(104), y + dp(30) };
    drawText(g_fSmall, L"Channel", cl, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // "Release Preview" gets two shares of the row so it isn't squeezed; the rest get one each
    int gap = dp(6), shares = EI_NCHANNELS + 1, unit = (x2 - bx - gap * (EI_NCHANNELS - 1)) / shares, l = bx;
    for (int i = 0; i < EI_NCHANNELS; i++) {
        int r = (i == EI_NCHANNELS - 1) ? x2 : l + unit * (i == 1 ? 2 : 1);
        aioButton(A_E_CH0 + i, l, y, r, y + dp(30), EI_CHANNELS[i], g_eiChannel == i && g_eiNameMode == 0, idle && g_eiNameMode == 0, FALSE);
        l = r + gap;
    }
    y += dp(36);
    wchar_t name[MAX_PATH];
    eiPreview(name, MAX_PATH);
    RECT sl = { x1, y, x1 + dp(104), y + dp(22) };
    drawText(g_fSmall, L"Saves as", sl, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT pr = { bx, y, x2, y + dp(22) };
    drawText(g_fSmall, name, pr, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += dp(26);

    // the button, and the job's progress or its result
    int by = bottom - dp(34);
    if (job) {
        wchar_t last[300]; int pct;
        eiLastLine(last, 300, &pct);
        RECT r = { x1, by, x2 - dp(152), by + dp(20) };
        drawText(g_fSmall, last[0] ? last : L"Starting…", r, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        drawProgressBar(x1, x2 - dp(152), by + dp(24), pct);
        aioButton(A_E_CANCEL, x2 - dp(140), by, x2, by + dp(34), L"Cancel", FALSE, TRUE, FALSE);
    } else {
        aioButton(A_E_GO, x2 - dp(140), by, x2, by + dp(34), L"Create ISO", TRUE, idle && listed && ticked > 0 && g_eDir[0], !g_elevated);
        if (g_aioMsg[AT_ESD][0]) {
            RECT r = { x1, by - dp(4), x2 - dp(152), by + dp(38) };
            drawText(g_fSmall, g_aioMsg[AT_ESD], r, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
            aioNote(x1, x2 - dp(152), by - dp(4), dp(42), L"Made by abbodi1406's decrypt.cmd (esd-decrypter-wimlib), fetched from GitHub and "
                                                           L"hash-checked each time. Needs administrator rights. The ESD isn't changed.");
        }
    }
}

// ---------- the tab's actions ----------
static void esdUnquote(wchar_t* p) {
    size_t n = wcslen(p);
    while (n && (p[n - 1] == L' ' || p[n - 1] == L'"')) p[--n] = 0;
    size_t s = 0; while (p[s] == L' ' || p[s] == L'"') s++;
    if (s) wmemmove(p, p + s, wcslen(p + s) + 1);
}
static void actionEsdLoad(void) {
    if (g_busy != 0) return;
    esdUnquote(g_eSrc);
    if (!isFile(g_eSrc)) { okDialog(L"ESD not found", L"That file wasn't found. Choose Browse to pick it."); return; }
    focusNone();
    g_esdN = 0; g_esdLoaded[0] = 0; g_esdTop = 0;
    if (!GetFullPathNameW(g_eSrc, MAX_PATH, g_jA, NULL)) wcscpy(g_jA, g_eSrc);
    wcscpy(g_eSrc, g_jA);
    g_aioMsg[AT_ESD][0] = 0;
    aioStartJob(JOB_ESDLOAD, NULL, FALSE);
}
static void actionEsdBrowse(void) {
    if (pickOpen(g_eSrc, L"ESD files (*.esd)\0*.esd\0", L"Choose an ESD file")) actionEsdLoad();
}
static void actionEsdTickAll(BOOL on) {
    for (LONG i = 0; i < g_esdN; i++) g_esdTick[i] = on;
}
static void actionEsdDir(void) {
    wchar_t f[MAX_PATH];
    if (pickFolder(f)) wcscpy(g_eDir, f);
}

static void actionEsdIso(void) {
    if (g_busy) return;
    esdUnquote(g_eSrc);
    if (!g_esdLoaded[0] || _wcsicmp(g_eSrc, g_esdLoaded)) {
        okDialog(L"List the editions first", L"The ESD file box has changed. Press Enter in it, or choose Browse, so this ESD's editions are listed.");
        return;
    }
    if (!isFile(g_esdLoaded)) { okDialog(L"ESD not found", L"The ESD isn't there any more."); return; }
    // decrypt.cmd works on Microsoft's layout: setup images 1 to 3, editions from 4
    if (g_esdSkipped < 3 || g_esd[0].index != 4) {
        okDialog(L"Not a Windows setup ESD", L"This ESD doesn't have Microsoft's setup images (Windows Setup Media, WinPE and Windows Setup) "
                                             L"ahead of its editions, so an ISO can't be made from it. ESDs from Microsoft's Media Creation Tool "
                                             L"and Windows Update have them.");
        return;
    }
    esdUnquote(g_eDir);
    trimSlash(g_eDir);
    if (!isFolder(g_eDir)) { okDialog(L"Folder not found", L"The folder to save the ISO in doesn't exist."); return; }
    wchar_t dir[MAX_PATH];
    if (!GetFullPathNameW(g_eDir, MAX_PATH, dir, NULL)) wcscpy(dir, g_eDir);
    int ticked = esdTicked();
    if (!ticked) return;
    wchar_t picks[400] = L"";
    if (ticked < g_esdN)                  // all ticked: the script's own "all editions"
        for (LONG i = 0; i < g_esdN; i++) if (g_esdTick[i]) {
            wchar_t t[8]; swprintf(t, 8, L"%s%d", picks[0] ? L" " : L"", g_esd[i].index - 3);
            if (wcslen(picks) + wcslen(t) < 399) wcscat(picks, t);
        }
    if (askInApp()) {
        wchar_t msg[600], name[MAX_PATH];
        eiPreview(name, MAX_PATH);
        swprintf(msg, 600, L"%d edition%s, saved in %s as\n%s\n\nabbodi1406's decrypt.cmd is downloaded from GitHub and run. This takes several minutes.",
                 ticked, ticked == 1 ? L"" : L"s", dir, name);
        if (!showDialog(g_hwnd, L"Make this ISO?", msg, NULL, 0, L"Create ISO", L"Cancel", 1)) return;
    }
    wchar_t evName[64], spec[REP_ARG_MAX];
    swprintf(evName, 64, L"Local\\wcu-esd-stop-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    if (g_eiCancelEv) CloseHandle(g_eiCancelEv);
    g_eiCancelEv = CreateEventW(NULL, TRUE, FALSE, evName);
    swprintf(spec, REP_ARG_MAX, L"%s|%s|%s|%d|%s|%s", g_esdLoaded, picks, dir, g_eiNameMode,
             g_eiChannel == EI_NO_CHANNEL ? L"" : EI_CHANNELS[g_eiChannel], g_eiCancelEv ? evName : L"");
    g_aioMsg[AT_ESD][0] = 0;
    repairStart(REP_ESDISO, spec);
    if (!g_repRunning) {                  // nothing ran: the reason is in the Repair page's status
        lstrcpynW(g_aioMsg[AT_ESD], g_repStatus, MAX_PATH + 96);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}
static void actionEsdCancel(void) {
    if (eiJobOn() && g_eiCancelEv) { SetEvent(g_eiCancelEv); wcscpy(g_aioMsg[AT_ESD], L"Cancelling…"); }
}

// The output, saved beside the ISO as a .txt of the same name, like Download Windows does
static BOOL eiSaveLog(const wchar_t* isoPath) {
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

// From repairOnDone: what the job ended with, on the tab
static void esdIsoOnDone(DWORD code) {
    wchar_t* m = g_aioMsg[AT_ESD]; int cch = MAX_PATH + 96;
    if (code == REP_CANCELLED) { wcscpy(m, L"Cancelled. No ISO was saved."); InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (code) {
        wchar_t err[300] = L"";
        EnterCriticalSection(&g_lock);
        for (int i = g_repN - 1; i >= 0 && !err[0]; i--) {
            const wchar_t* s = g_repLines[i].s;
            if (s && (!wcsncmp(s, L"Error", 5) || wcsstr(s, L"Errors were reported") || wcsstr(s, L"==== ERROR"))) lstrcpynW(err, s, 300);
        }
        LeaveCriticalSection(&g_lock);
        if (code > (REP_RESTART_BIT | 0xFF)) swprintf(m, cch, L"The administrator helper stopped, so no ISO was made.");
        else swprintf(m, cch, L"No ISO was made. %s", err[0] ? err : L"The full output is on the Windows Repair page.");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    size_t pre = wcslen(EI_SAVED_PREFIX);
    wchar_t isoPath[MAX_PATH * 2 + 2] = L"";
    EnterCriticalSection(&g_lock);
    for (int i = g_repN - 1; i >= 0 && !isoPath[0]; i--)
        if (g_repLines[i].s && !wcsncmp(g_repLines[i].s, EI_SAVED_PREFIX, pre)) lstrcpynW(isoPath, g_repLines[i].s + pre, MAX_PATH * 2);
    LeaveCriticalSection(&g_lock);
    if (!isoPath[0]) return;
    BOOL logged = eiSaveLog(isoPath);
    swprintf(m, cch, L"Saved %s", PathFindFileNameW(isoPath));
    wchar_t msg[MAX_PATH * 2 + 160];
    swprintf(msg, MAX_PATH * 2 + 160, L"%s\nSaved in %s%s", PathFindFileNameW(isoPath), g_eDir,
             logged ? L", with the output beside it as a .txt file of the same name." : L".");
    InvalidateRect(g_hwnd, NULL, FALSE);
    noteShow(L"Windows ISO is ready", msg);
}
