// Run as TI page: pick a program (.exe) or registry file (.reg) and run it as TrustedInstaller.
// The work (runAsTrustedInstaller, regImportFile) is in tweaks.c; this page is the picker and the button.

static wchar_t g_tiFile[MAX_PATH];
static wchar_t g_tiMsg[512];
static BOOL    g_tiRunning = FALSE;

// Only a plain .exe or .reg path, with no embedded quote (a quote could add arguments to the elevated command line)
static BOOL tiFileKind(const wchar_t* path, BOOL* isReg) {
    if (!path || !*path || wcschr(path, L'"')) return FALSE;
    const wchar_t* ext = PathFindExtensionW(path);
    if (!_wcsicmp(ext, L".reg")) { *isReg = TRUE;  return TRUE; }
    if (!_wcsicmp(ext, L".exe")) { *isReg = FALSE; return TRUE; }
    return FALSE;
}

// Runs in whichever process has administrator rights. Returns a Windows error, or the child's own exit code.
static DWORD runTiReg(const wchar_t* path) {
    BOOL isReg;
    if (!tiFileKind(path, &isReg) || !isReg || !isFile(path)) return ERROR_INVALID_PARAMETER;
    return regImportFile(path, TRUE);               // reg.exe import, as a child of the TrustedInstaller service
}
static DWORD runTiExe(const wchar_t* path) {
    BOOL isReg;
    if (!tiFileKind(path, &isReg) || isReg || !isFile(path)) return ERROR_INVALID_PARAMETER;
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
    lstrcpynW(exe, path, MAX_PATH);
    swprintf(cmd, MAX_PATH + 4, L"\"%s\"", path);
    DWORD code = 1, err = runAsTrustedInstaller(exe, cmd, &code);   // waits until the program exits
    return err ? err : code;
}

static DWORD WINAPI tiProc(LPVOID arg) {
    HANDLE helper = (HANDLE)arg;
    DWORD code = helper ? waitExitCode(helper) : ERROR_INVALID_PARAMETER;
    PostMessageW(g_hwnd, WM_APP_RUNTI_DONE, (WPARAM)code, 0);
    return 0;
}

static void tiBrowse(void) {
    wchar_t file[MAX_PATH] = L"";
    if (!pickOpen(file, L"Programs and registry files (*.exe;*.reg)\0*.exe;*.reg\0"
                        L"Program (*.exe)\0*.exe\0Registry file (*.reg)\0*.reg\0All files (*.*)\0*.*\0",
                  L"Choose a program or registry file to run as TrustedInstaller")) return;
    wcscpy(g_tiFile, file);
    g_tiMsg[0] = 0;
    g_focus = A_TI_FILE; g_focusBuf = g_tiFile; g_focusCch = MAX_PATH; g_focusRo = FALSE;
    g_caret = g_selA = (int)wcslen(g_tiFile); g_fieldStart = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void tiRun(void) {
    BOOL isReg;
    if (!tiFileKind(g_tiFile, &isReg)) {
        okDialog(L"Choose a file first", L"Pick a program (.exe) or a registry file (.reg) to run as TrustedInstaller.");
        return;
    }
    if (!isFile(g_tiFile)) {
        okDialog(L"File not found", L"That file no longer exists. Choose another.");
        return;
    }
    const wchar_t* name = PathFindFileNameW(g_tiFile);
    wchar_t msg[MAX_PATH + 320];
    if (isReg)
        swprintf(msg, MAX_PATH + 320, L"%s\n\nThis imports the registry file with TrustedInstaller rights, so it can "
                 L"change keys that even administrators can't. Only run files you trust.", name);
    else
        swprintf(msg, MAX_PATH + 320, L"%s\n\nThis runs the program with TrustedInstaller rights, the highest level on "
                 L"Windows. The app waits until the program closes. Only run programs you trust.", name);
    if (askInApp() && !showDialog(g_hwnd, L"Run as TrustedInstaller?", msg, NULL, 0, L"Run", L"Cancel", 1)) return;

    wchar_t args[MAX_PATH + 32]; BOOL cancelled = FALSE;
    swprintf(args, MAX_PATH + 32, L"--runti-%s=\"%s\"", isReg ? L"reg" : L"exe", g_tiFile);
    HANDLE helper = NULL;
    if (g_elevated) helper = startHelperDirect(args);
    else if (!startElevatedHelper(args, &helper, &cancelled)) helper = NULL;
    if (!helper) {
        wcscpy(g_tiMsg, cancelled ? L"Cancelled — nothing ran." : L"Couldn't start the administrator helper — nothing ran.");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (!jobBegin()) {
        WaitForSingleObject(helper, INFINITE); CloseHandle(helper);
        return;
    }
    g_tiRunning = TRUE;
    g_tiMsg[0] = 0;
    startWorker(tiProc, helper);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void tiOnDone(DWORD code) {
    g_tiRunning = FALSE;
    InterlockedExchange(&g_busy, 0);
    BOOL isReg = FALSE; tiFileKind(g_tiFile, &isReg);
    const wchar_t* name = PathFindFileNameW(g_tiFile);
    if (code == 0)
        swprintf(g_tiMsg, 512, isReg ? L"Imported %s as TrustedInstaller." : L"Ran %s as TrustedInstaller.", name);
    else if (isReg)
        swprintf(g_tiMsg, 512, L"Couldn't import %s (error 0x%08lX).", name, code);
    else
        swprintf(g_tiMsg, 512, L"%s ran as TrustedInstaller but reported exit code %lu.", name, code);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void tiClick(int id) {
    if (id == A_TI_BROWSE) tiBrowse();
    else if (id == A_TI_RUN) tiRun();
}

// ---------- Run as TI page ----------
static void drawRunTI(int cL, int cR) {
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    fillRoundRect(cL, top, cR, top + dp(300), dp(8), sf.r, sf.g, sf.b, sf.a);

    drawPageHeader(0xE72E, cL, cR, top, L"Run as TrustedInstaller",
                   L"Run a program or import a registry file with TrustedInstaller rights.");

    int x1 = cL + dp(20), x2 = cR - dp(20), y = top + dp(100);
    RECT note = { x1, y, x2, y + dp(40) };
    drawText(g_fSmall,
        L"Choose an .exe to run, or a .reg to import. It runs as TrustedInstaller, which can change files and "
        L"registry keys that even administrators can't. Asks for administrator permission.",
        note, mutedRGB(), DT_LEFT | DT_WORDBREAK);
    y += dp(48);

    // file box + Browse
    BOOL idle = (g_busy == 0);
    RECT fr = { x1, y, x2 - dp(92), y + dp(34) };
    aioField(A_TI_FILE, fr, g_tiFile, MAX_PATH, FALSE, L"Path to an .exe or .reg file");
    aioButton(A_TI_BROWSE, x2 - dp(84), y, x2, y + dp(34), L"Browse", FALSE, idle, FALSE);
    y += dp(46);

    // Run as TI (accent, with the UAC shield when not already elevated)
    BOOL hasFile = g_tiFile[0] != 0;
    aioButton(A_TI_RUN, x1, y, x1 + dp(200), y + dp(36), L"Run as TI", TRUE, idle && hasFile, !g_elevated);
    y += dp(44);

    if (g_tiRunning) {
        wchar_t line[MAX_PATH + 48];
        swprintf(line, MAX_PATH + 48, L"Running %s as TrustedInstaller…", PathFindFileNameW(g_tiFile));
        RECT r = { x1, y, x2, y + dp(20) };
        drawText(g_fSmall, line, r, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else if (g_tiMsg[0]) {
        RECT r = { x1, y, x2, y + dp(40) };
        drawText(g_fSmall, g_tiMsg, r, textRGB(), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
}
