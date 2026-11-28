// File and process helpers shared by the pages, and starting the elevated helper.

static unsigned long long fileBytes(DWORD high, DWORD low) { return ((unsigned long long)high << 32) | low; }
static BOOL isDotName(const wchar_t* n) { return !wcscmp(n, L".") || !wcscmp(n, L".."); }
static BOOL isFolder(const wchar_t* p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static BOOL isFile(const wchar_t* p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

// out must hold MAX_PATH characters
static void systemPath(wchar_t* out, const wchar_t* name) {
    GetSystemDirectoryW(out, MAX_PATH);
    wcscat(out, L"\\");
    wcscat(out, name);
}

// A pause of ms milliseconds taken one second at a time. Used for the waits a server asks for (5 to 20 seconds):
// one long Sleep is a pattern anti-malware heuristics flag as sandbox evasion, short slices are not.
static void pauseMs(DWORD ms) {
    while (ms) { DWORD s = ms > 1000 ? 1000 : ms; Sleep(s); ms -= s; }
}

// Runs the page's one background job, replacing the handle of the last one
static void startWorker(LPTHREAD_START_ROUTINE proc, LPVOID arg) {
    if (g_worker) CloseHandle(g_worker);
    g_worker = CreateThread(NULL, 0, proc, arg, 0, NULL);
}

// Waits for a process, closes its handle and returns its exit code (1 if Windows can't say)
static DWORD waitExitCode(HANDLE proc) {
    DWORD code = 1;
    WaitForSingleObject(proc, INFINITE);
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return code;
}

// ---------- path building for long paths ----------
// Build "\\?\" + root into a heap buffer so long trees are handled.
static wchar_t* longPath(const wchar_t* abs) {
    size_t n = wcslen(abs);
    wchar_t* buf = (wchar_t*)malloc((n + 8) * sizeof(wchar_t));
    if (!buf) return NULL;
    if (abs[0] == L'\\' && abs[1] == L'\\') { wcscpy(buf, abs); }     // UNC/already prefixed
    else { wcscpy(buf, L"\\\\?\\"); wcscat(buf, abs); }
    return buf;
}

// malloc'd "dir\name"
static wchar_t* childPath(const wchar_t* dir, const wchar_t* name) {
    size_t dl = wcslen(dir);
    wchar_t* p = (wchar_t*)malloc((dl + wcslen(name) + 2) * sizeof(wchar_t));
    if (!p) return NULL;
    wcscpy(p, dir);
    if (dir[dl - 1] != L'\\') wcscat(p, L"\\");
    wcscat(p, name);
    return p;
}

// recursive size of a directory's contents (bytes). Skips reparse points.
static unsigned long long dirSize(const wchar_t* dir) {
    unsigned long long total = 0;
    wchar_t* pat = childPath(dir, L"*");
    if (!pat) return 0;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (isDotName(fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wchar_t* child = childPath(dir, fd.cFileName);
        if (!child) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += dirSize(child);
        } else {
            total += fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow);
        }
        free(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static unsigned long long folderBytes(const wchar_t* path) {
    unsigned long long sz = 0;
    wchar_t* lp = longPath(path);
    if (lp) { sz = dirSize(lp); free(lp); }
    return sz;
}

// FALSE only when Windows refuses to list the folder (e.g. C:\Windows\Temp without elevation)
static BOOL canListDir(const wchar_t* dir) {
    wchar_t pat[MAX_PATH + 4]; swprintf(pat, MAX_PATH + 4, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return TRUE; }
    return GetLastError() != ERROR_ACCESS_DENIED;
}

// delete a directory's contents (not the dir itself). Returns bytes freed.
static unsigned long long deleteContents(const wchar_t* dir) {
    unsigned long long freed = 0;
    wchar_t* pat = childPath(dir, L"*");
    if (!pat) return 0;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (isDotName(fd.cFileName)) continue;
        wchar_t* child = childPath(dir, fd.cFileName);
        if (!child) continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            freed += deleteContents(child);
            RemoveDirectoryW(child);                      // only if now empty
        } else {
            unsigned long long sz = fileBytes(fd.nFileSizeHigh, fd.nFileSizeLow);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // a file symlink/junction: remove the link, not the target contents
                if (DeleteFileW(child)) { /* size of a reparse point is negligible */ }
            } else if (DeleteFileW(child)) {
                freed += sz;
            }
        }
        free(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return freed;
}

// ---------- getting the foreground back after UAC ----------
// When the UAC prompt closes, Windows activates whichever window is next, and a background program isn't
// allowed to simply take over. Sharing the foreground thread's input state lifts that for this one call.
// Windows refuses that when an administrator window is in front; returns TRUE then.
static BOOL raiseWindow(HWND h) {
    if (!IsWindowEnabled(h)) h = GetLastActivePopup(h);      // a dialog is open: it takes the focus, not the window it disabled
    DWORD me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    BOOL attached = fg && fg != me && AttachThreadInput(me, fg, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, fg, FALSE);
    return fg && !attached;
}

#define TIMER_RAISE 7
static int g_raiseTicks, g_raiseHeld, g_raiseBounce;   // g_raiseBounce: 0 not used, 1 minimized, 2 restored

// The secure desktop takes a moment to go away and something else may activate meanwhile, so this checks
// for a short while. A dialog of this app counts as being in front.
static void raiseTick(HWND h) {
    if (g_raiseBounce == 1) { ShowWindow(h, SW_RESTORE); g_raiseBounce = 2; g_raiseTicks--; return; }
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid == GetCurrentProcessId()) g_raiseHeld++;
    else {
        g_raiseHeld = 0;
        BOOL refused = raiseWindow(h);
        // Last resort, since it shows as a quick animation: restoring from the taskbar is always allowed the
        // foreground. It waits 300 ms, because after an approved prompt the elevated helper hands it over itself.
        if (refused && g_raiseBounce == 0 && g_raiseTicks <= 12 && (GetWindowThreadProcessId(GetForegroundWindow(), &pid), pid != GetCurrentProcessId())) {
            ShowWindow(h, SW_MINIMIZE);
            g_raiseBounce = 1;
        }
    }
    if (g_raiseHeld >= 3 || --g_raiseTicks <= 0) KillTimer(h, TIMER_RAISE);
}

static void raiseStart(HWND h) {
    g_raiseTicks = 15; g_raiseHeld = 0; g_raiseBounce = 0;       // 100 ms apart
    SetTimer(h, TIMER_RAISE, 100, NULL);
    raiseTick(h);
}

// Start an elevated copy of this exe; blocks while the UAC prompt is up. *proc receives its handle.
// Returns FALSE (and sets *cancelled if the user declined) when the helper did not start.
static BOOL startElevatedHelper(const wchar_t* verbArgs, HANDLE* proc, BOOL* cancelled) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t args[MAX_PATH * 2 + 128];
    swprintf(args, MAX_PATH * 2 + 128, L"%s --notify=%llu", verbArgs, (unsigned long long)(ULONG_PTR)g_hwnd);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    *cancelled = FALSE;
    BOOL started = ShellExecuteExW(&sei);
    if (!started) *cancelled = (GetLastError() == ERROR_CANCELLED);
    PostMessageW(g_hwnd, WM_APP_RAISE, 0, 0);
    if (!started) return FALSE;
    *proc = sei.hProcess;
    return TRUE;
}

// The same helper, when this process already has administrator rights. Returns its process, or NULL.
static HANDLE startHelperDirect(const wchar_t* verbArgs) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH * 3 + 128];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(cmd, MAX_PATH * 3 + 128, L"\"%s\" %s --notify=%llu", exe, verbArgs, (unsigned long long)(ULONG_PTR)g_hwnd);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// FALSE if UAC is configured to approve elevation silently, so no prompt would actually appear
static BOOL uacWillPrompt(void) {
    DWORD lua = 1, behaviour = 5, cb; HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        cb = sizeof(lua);       RegQueryValueExW(k, L"EnableLUA", NULL, NULL, (BYTE*)&lua, &cb);
        cb = sizeof(behaviour); RegQueryValueExW(k, L"ConsentPromptBehaviorAdmin", NULL, NULL, (BYTE*)&behaviour, &cb);
        RegCloseKey(k);
    }
    return lua != 0 && behaviour != 0;
}

// Starts a console tool with no window, with its output and errors on *rd. Returns 0 or a Windows error.
static DWORD spawnPiped(const wchar_t* exe, wchar_t* cmd, HANDLE* rd, PROCESS_INFORMATION* pi) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE wr = NULL;
    if (!CreatePipe(rd, &wr, &sa, 0)) return GetLastError();
    SetHandleInformation(*rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    memset(pi, 0, sizeof(*pi));
    DWORD err = 0;
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, pi)) {
        err = GetLastError();
        CloseHandle(*rd);
    }
    CloseHandle(wr);
    return err;
}

// Runs a console tool hidden, reporting the number before each '%' it prints. Returns its exit code.
static DWORD runWithPercent(const wchar_t* exe, wchar_t* cmd, void (*onPercent)(int)) {
    HANDLE rd = NULL; PROCESS_INFORMATION pi;
    DWORD err = spawnPiped(exe, cmd, &rd, &pi);
    if (err) return err;
    char buf[512]; DWORD got;              // DISM draws its progress as "[====  45.0% ===]"
    while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) {
        for (DWORD i = 1; i < got; i++) {
            if (buf[i] != '%' || !isdigit((unsigned char)buf[i-1])) continue;
            DWORD s = i;
            while (s > 0 && (isdigit((unsigned char)buf[s-1]) || buf[s-1] == '.')) s--;
            if (onPercent) onPercent(atoi(buf + s));
        }
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    return waitExitCode(pi.hProcess);
}


// One prompt per action: UAC is the confirmation, so the app only asks itself when we're already
// elevated or UAC would approve silently.
static BOOL askInApp(void) { return g_elevated || !uacWillPrompt(); }
// ---------- reading the elevated helper's command line ----------
// Copies a quoted argument, starting just after its opening quote. Returns where it stopped: the closing quote.
static const wchar_t* cmdQuoted(const wchar_t* p, wchar_t* out, int cch) {
    int n = 0;
    for (; *p && *p != L'"' && n < cch - 1; p++) out[n++] = *p;
    out[n] = 0;
    return p;
}
// The window to report to. The real --notify is always added last, after any file path, so the last one counts.
static HWND cmdNotify(const wchar_t* from) {
    HWND notify = NULL;
    for (const wchar_t* p = wcsstr(from, L"--notify="); p; p = wcsstr(p + 1, L"--notify="))
        notify = (HWND)(ULONG_PTR)_wcstoui64(p + 9, NULL, 10);
    return notify;
}

// ---------- pickers ----------
static BOOL pickFolder(wchar_t* out) {
    IFileOpenDialog* d = NULL; BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&d))) return FALSE;
    DWORD o = 0;
    d->lpVtbl->GetOptions(d, &o);
    d->lpVtbl->SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(d->lpVtbl->Show(d, g_hwnd))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                lstrcpynW(out, p, MAX_PATH); CoTaskMemFree(p); ok = TRUE;
            }
            it->lpVtbl->Release(it);
        }
    }
    d->lpVtbl->Release(d);
    return ok;
}
static BOOL pickOpen(wchar_t* out, const wchar_t* filter, const wchar_t* title) {
    wchar_t f[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = filter; ofn.lpstrFile = f; ofn.nMaxFile = MAX_PATH; ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return FALSE;
    wcscpy(out, f);
    return TRUE;
}
static BOOL pickSave(wchar_t* out, const wchar_t* filter, const wchar_t* ext, const wchar_t* title) {
    wchar_t f[MAX_PATH] = L"";
    lstrcpynW(f, out, MAX_PATH);
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = filter; ofn.lpstrFile = f; ofn.nMaxFile = MAX_PATH; ofn.lpstrTitle = title; ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return FALSE;
    wcscpy(out, f);
    return TRUE;
}
