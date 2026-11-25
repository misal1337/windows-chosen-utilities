// Windows Repair page: SFC and DISM health commands with their output shown live inside the app.
//
// Commands need administrator rights, but the window may not have them, so everything goes through one
// named pipe. A "runner" (an elevated copy of this exe, or a thread if we're already elevated) runs the
// commands and writes their text to the pipe, and a reader thread in the window adds it to the output panel.
// The runner only accepts the fixed jobs below, so its command line can't ask it to run anything else.

// REP_UUP: the Download Windows page. New jobs go at the end: the helper's names below follow this order.
enum { REP_NONE, REP_SFC, REP_SCAN, REP_RESTORE, REP_WIM, REP_UUP, REP_WUC, REP_RSON, REP_RSOFF, REP_ESDISO };   // REP_ESDISO: Windows AIO's ESD to ISO
#define REP_ARG_MAX 8192          // the longest extra argument a job can take: a UUP job lists the apps it keeps

#define REP_RESTART_BIT 0x100     // runner exit code: low byte = failed commands, this bit = restart needed
#define REP_NO_PIPE     0x1000    // the runner couldn't reach the window, so nothing ran
#define REP_CANCELLED   0xFE      // a job that can be cancelled was; low-byte failure counts never reach this high in practice
#define REP_MAX_LINES   6000
#define REP_MAX_LINE    8000
#define REP_STEP_MARK   1         // written ahead of each command; command output can't contain it
#define REP_PROG_MARK   2         // Download Windows: <mark>bytes/total<end>, never shown as text
#define REP_PROG_END    3

typedef struct { const wchar_t* tool; const wchar_t* cmd; } RepCmd;
// /NoRestart: when a restart is pending, DISM otherwise ends by asking "restart now? (Y/N)" on its console.
// These run hidden, so nobody could answer, and the job would wait forever. A restart is reported instead.
static const RepCmd REP_SFC_CMDS[] = {
    { L"sfc.exe",  L"sfc /scannow" },
};
static const RepCmd REP_WUC_CMDS[] = {          // Windows Update Cleanup
    { L"dism.exe", L"Dism.exe /online /Cleanup-Image /AnalyzeComponentStore /NoRestart" },
    { L"dism.exe", L"Dism.exe /online /Cleanup-Image /StartComponentCleanup /NoRestart" },
};
static const RepCmd REP_RSON_CMDS[]  = { { L"dism.exe", L"DISM.exe /online /Set-ReservedStorageState /State:Enabled /NoRestart" } };
static const RepCmd REP_RSOFF_CMDS[] = { { L"dism.exe", L"DISM.exe /online /Set-ReservedStorageState /State:Disabled /NoRestart" } };
static const RepCmd REP_SCAN_CMD    = { L"dism.exe", L"Dism.exe /online /Cleanup-Image /ScanHealth /NoRestart" };
static const RepCmd REP_RESTORE_CMD = { L"dism.exe", L"Dism.exe /online /Cleanup-Image /RestoreHealth /NoRestart" };

// The fixed command list of a job that is one, or NULL
static const RepCmd* repJobCmds(int job, int* n) {
    #define REP_LIST(a) (*n = (int)(sizeof(a) / sizeof(a[0])), a)
    switch (job) {
    case REP_SFC:     return REP_LIST(REP_SFC_CMDS);
    case REP_WUC:     return REP_LIST(REP_WUC_CMDS);
    case REP_RSON:    return REP_LIST(REP_RSON_CMDS);
    case REP_RSOFF:   return REP_LIST(REP_RSOFF_CMDS);
    case REP_SCAN:    *n = 1; return &REP_SCAN_CMD;
    case REP_RESTORE: *n = 1; return &REP_RESTORE_CMD;
    }
    #undef REP_LIST
    *n = 0;
    return NULL;
}

// Reserved storage: DISM keeps PassedPolicy at 1 while it's on and 0 while it's off, and anyone can read it,
// so the light is right even without administrator rights. -1 if it can't be read.
static int repReservedState(void) {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\ReserveManager", L"PassedPolicy",
                     RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS) return -1;
    return v ? 1 : 0;
}

// ---------- this PC ----------
typedef struct { int build; wchar_t full[32], edition[64], arch[8]; } RepLive;
static RepLive g_repLive;

static void repairReadLive(RepLive* l) {
    memset(l, 0, sizeof(*l));
    wchar_t cur[16] = L""; DWORD ubr = 0, cb; HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        cb = sizeof(cur) - sizeof(wchar_t); RegQueryValueExW(k, L"CurrentBuildNumber", NULL, NULL, (BYTE*)cur, &cb);
        cb = sizeof(ubr); RegQueryValueExW(k, L"UBR", NULL, NULL, (BYTE*)&ubr, &cb);
        cb = sizeof(l->edition) - sizeof(wchar_t); RegQueryValueExW(k, L"EditionID", NULL, NULL, (BYTE*)l->edition, &cb);
        RegCloseKey(k);
    }
    l->build = _wtoi(cur);
    swprintf(l->full, 32, L"%s.%lu", cur, ubr);
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    WORD a = si.wProcessorArchitecture;
    wcscpy(l->arch, a == PROCESSOR_ARCHITECTURE_AMD64 ? L"x64" : a == PROCESSOR_ARCHITECTURE_ARM64 ? L"ARM64" :
                    a == PROCESSOR_ARCHITECTURE_INTEL ? L"x86" : L"-");
}

// ---------- the WIM check ----------
// Reads the images into a caller's list, not the WIM Editor's. Returns the count, or -1 if it can't be read.
#define REP_WIM_MAX 64
static int repairReadWim(const wchar_t* path, WimEntry* out, int max) {
    if (!wimLoadApi()) return -1;
    wchar_t tmp[MAX_PATH]; wimTempDir(tmp);
    DWORD created;
    HANDLE h = pWIMCreateFile(path, GENERIC_READ, WIM_OPEN_EXISTING, 0, 0, &created);
    if (!h) return -1;
    pWIMSetTemporaryPath(h, tmp);
    int n = 0;
    DWORD count = pWIMGetImageCount(h);
    for (DWORD i = 1; i <= count && n < max; i++) {
        HANDLE img = pWIMLoadImage(h, i);
        if (!img) continue;
        if (wimReadImageInfo(img, path, i, &out[n])) n++;
        pWIMCloseHandle(img);
    }
    pWIMCloseHandle(h);
    return n;
}

// The image to repair from: same build number and architecture as this PC, and the same edition if
// the WIM has it. Only the build number counts, not the update number after the dot: 26100.1 can
// repair 26100.7019, and an installed PC is rarely on the exact update its ISO shipped with.
// Returns a position in the list, or -1 if nothing matches.
static int repairPickImage(const WimEntry* e, int n, const RepLive* live) {
    int first = -1;
    for (int i = 0; i < n; i++) {
        if (_wtoi(e[i].build) != live->build || _wcsicmp(e[i].arch, live->arch) != 0) continue;
        if (_wcsicmp(e[i].edition, live->edition) == 0) return i;
        if (first < 0) first = i;
    }
    return first;
}

static void repairWimCommand(const wchar_t* path, int index, wchar_t* out, int cch) {
    swprintf(out, cch, L"Dism.exe /online /Cleanup-Image /RestoreHealth /Source:WIM:\"%s\":%d /LimitAccess /NoRestart", path, index);
}

// ---------- the runner ----------
static void repWrite(HANDLE out, const wchar_t* s) {
    DWORD wr;
    WriteFile(out, s, (DWORD)(wcslen(s) * sizeof(wchar_t)), &wr, NULL);   // if the window is gone, carry on regardless
}

// Each tool runs on a hidden console and we read the text off its screen. Through a pipe, sfc holds its
// output back and releases it in 4 KB blocks, so its progress never shows live and a block can end mid-line.
#define REP_CON_WIDTH 512          // wide enough that the tools' lines never wrap
#define REP_CON_ROWS  9999

// Gets this process onto a new hidden console. A throwaway "pause" holds the console open while we attach,
// so even a tool that fails at once can't take its message away with it. Call FreeConsole when finished.
// Which step of starting a tool failed last, for the error line: a bare error number didn't say (seen on a friend's PC
// as "error 87" with nothing else to go on).
static const wchar_t* g_repConStep = L"";

static DWORD repConsoleOpen(HANDLE* conIn, HANDLE* conOut) {
    wchar_t exe[MAX_PATH]; systemPath(exe, L"cmd.exe");
    wchar_t cmd[] = L"cmd.exe /d /c pause>nul";
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    FreeConsole();
    g_repConStep = L"starting the console holder (cmd.exe)";
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return GetLastError();
    DWORD err = ERROR_TIMEOUT;
    g_repConStep = L"joining the hidden console";
    for (int i = 0; i < 500 && err; i++) {        // the holder needs a moment before it has a console to join
        if (AttachConsole(pi.dwProcessId)) err = 0;
        else Sleep(10);
    }
    if (!err) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        g_repConStep = L"opening the hidden console";
        *conIn  = CreateFileW(L"CONIN$",  GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        *conOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        BOOL sized = FALSE;
        if (*conIn != INVALID_HANDLE_VALUE && *conOut != INVALID_HANDLE_VALUE) {
            // the biggest first; some console settings refuse a buffer this size, and a smaller one still works,
            // with long lines wrapping. Never narrower than the console's own window, which Windows refuses too.
            g_repConStep = L"sizing the hidden console";
            CONSOLE_SCREEN_BUFFER_INFO cur; SHORT minW = 80;
            if (GetConsoleScreenBufferInfo(*conOut, &cur)) minW = (SHORT)(cur.srWindow.Right - cur.srWindow.Left + 1);
            const COORD sizes[] = { { REP_CON_WIDTH, REP_CON_ROWS }, { REP_CON_WIDTH, 3000 }, { 256, 3000 }, { 160, 2000 } };
            for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])) && !sized; i++)
                if (sizes[i].X >= minW) sized = SetConsoleScreenBufferSize(*conOut, sizes[i]);
            if (!sized && GetConsoleScreenBufferInfo(*conOut, &cur) && cur.dwSize.X >= 80) sized = TRUE;   // keep its own size
        }
        if (!sized) {
            err = GetLastError();
            if (!err) err = ERROR_INVALID_PARAMETER;
            if (*conIn != INVALID_HANDLE_VALUE) CloseHandle(*conIn);
            if (*conOut != INVALID_HANDLE_VALUE) CloseHandle(*conOut);
            FreeConsole();
        }
    }
    TerminateProcess(pi.hProcess, 0);             // our own attachment now keeps the console alive
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return err;
}

// One screen row without its padding. text must hold REP_CON_WIDTH + 1 characters.
static void repReadRow(HANDLE con, SHORT row, wchar_t* text) {
    COORD at = { 0, row }; DWORD got = 0, w = REP_CON_WIDTH;
    CONSOLE_SCREEN_BUFFER_INFO b;               // the console may be narrower, if it refused the full size
    if (GetConsoleScreenBufferInfo(con, &b) && b.dwSize.X > 0 && (DWORD)b.dwSize.X < w) w = (DWORD)b.dwSize.X;
    if (!ReadConsoleOutputCharacterW(con, text, w, at, &got)) got = 0;
    while (got && text[got - 1] == L' ') got--;
    text[got] = 0;
}

// DISM prints its closing message right after the finished bar, on the same row: "[==100.0%==] No corruption".
// Returns where that message starts, or NULL if this row isn't a bar with a message.
static wchar_t* repBarMessage(wchar_t* row) {
    if (row[0] != L'[' || !wcschr(row, L'%')) return NULL;
    wchar_t* end = wcsstr(row, L"] ");
    return (end && end[2]) ? end + 2 : NULL;
}

// aria2's report of a link Microsoft refused with 403 (they expire after a few minutes) is several rows of raw text
// per file. While g_repAria is set, its rows go through here: a 403 report is counted and left out, with one plain
// line in its place, and every other error is passed on whole. Progress rows overwrite each other.
#define REP_HELD_MAX 16
static BOOL g_repAria = FALSE;
static int  g_repRefused = 0, g_repDropped = 0, g_repHeldN = 0;
static wchar_t g_repHeld[REP_HELD_MAX][REP_CON_WIDTH + 1];

static BOOL repAriaProgress(const wchar_t* t) {
    return !t[0] || !wcsncmp(t, L"[DL:", 4) || !wcsncmp(t, L"[#", 2) || !wcsncmp(t, L"[Checksum:", 10);
}
static void repRowOut(HANDLE out, const wchar_t* text) {
    repWrite(out, L"\r"); repWrite(out, text); repWrite(out, L"\r\n");
}
static void repAriaFlush(HANDLE out) {
    for (int i = 0; i < g_repHeldN; i++) repRowOut(out, g_repHeld[i]);
    g_repHeldN = 0;
}
// One finished row
static void repAriaRow(HANDLE out, const wchar_t* text) {
    BOOL start = wcsstr(text, L"[ERROR] CUID#") && wcsstr(text, L"Download aborted");
    if (start || (g_repHeldN && repAriaProgress(text) && text[0])) repAriaFlush(out);
    if (!start && !g_repHeldN) {
        if (!repAriaProgress(text)) repRowOut(out, text);
        else if (text[0]) { repWrite(out, L"\r"); repWrite(out, text); }
        return;
    }
    if (!wcsncmp(text, L"  -> ", 5) && wcsstr(text, L"status=403")) {
        g_repHeldN = 0;
        if (!g_repRefused++)
            repRowOut(out, L"Some files' links have expired. The rest carry on, and those files are fetched again with fresh links.");
        return;
    }
    // A dropped connection reports every open transfer at once, dozens of rows saying the same thing. The cause is
    // kept once, in case the download never recovers.
    const wchar_t* why = wcsstr(text, L"Network problem has occurred. cause:");
    if (!wcsncmp(text, L"Exception: ", 11) && why) {
        g_repHeldN = 0;
        if (!g_repDropped++) {
            wchar_t line[REP_CON_WIDTH + 200];
            swprintf(line, REP_CON_WIDTH + 200, L"The connection dropped (%s). Files that don't finish are fetched again.", why + 36);
            wchar_t* end = wcsstr(line, L".). ");            // the cause's own full stop
            if (end) wmemmove(end, end + 1, wcslen(end));
            repRowOut(out, line);
        }
        return;
    }
    wcscpy(g_repHeld[g_repHeldN++], text);
    if (!wcsncmp(text, L"  -> ", 5) || g_repHeldN == REP_HELD_MAX) repAriaFlush(out);
}

// Passes on what changed: rows the cursor has left are finished, and the cursor's row is sent again whenever
// it changes. The carriage return in front tells the output panel to redraw that line.
static void repRelayScreen(HANDLE con, HANDLE out, SHORT* sent, wchar_t* shown) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(con, &info)) return;
    wchar_t text[REP_CON_WIDTH + 1];
    if (info.dwCursorPosition.Y < *sent) {       // the tool cleared its screen; what it already printed stays in the panel
        if (shown[0]) repWrite(out, L"\r\n");
        shown[0] = 0;
        *sent = info.dwCursorPosition.Y;
    }
    while (*sent < info.dwCursorPosition.Y) {
        repReadRow(con, *sent, text);
        if (g_repAria) { repAriaRow(out, text); shown[0] = 0; (*sent)++; continue; }
        wchar_t* msg = repBarMessage(text);
        repWrite(out, L"\r");
        if (msg) {
            msg[-1] = 0;
            repWrite(out, text);
            repWrite(out, L"\r\n");
            repWrite(out, msg);
        } else {
            repWrite(out, text);
        }
        repWrite(out, L"\r\n");
        shown[0] = 0;
        (*sent)++;
    }
    repReadRow(con, *sent, text);
    if (g_repAria && (g_repHeldN || !text[0] || !repAriaProgress(text))) return;    // an error still being printed
    if (wcscmp(text, shown)) {
        repWrite(out, L"\r");
        repWrite(out, text);
        wcscpy(shown, text);
    }
}

// Runs a tool on a hidden console and relays its screen to out until it ends. Returns 0 and sets *code to its
// exit code, or returns why it couldn't start. With a cancel event, setting that event ends the tool and
// everything it started, and *code is ERROR_CANCELLED. cwd may be NULL.
// The line written when a tool couldn't start: which step, which program, from which folder, and Windows' own words
static void repStartFailed(HANDLE out, const wchar_t* exe, const wchar_t* cwd, DWORD err) {
    wchar_t msg[300] = L"", line[MAX_PATH * 2 + 600];
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, msg, 300, NULL);
    for (wchar_t* p = msg; *p; p++) if (*p == L'\r' || *p == L'\n') *p = L' ';
    swprintf(line, MAX_PATH * 2 + 600, L"Couldn't run %s%s%s: %s failed, error %lu (%s)\r\n",
             exe, cwd ? L" in " : L"", cwd ? cwd : L"", g_repConStep, err, msg);
    repWrite(out, line);
}

static void (*g_repTick)(HANDLE proc, HANDLE out) = NULL;     // if set, called about ten times a second while the tool runs

static DWORD repRunOnConsole(HANDLE out, const wchar_t* exe, wchar_t* cmd, const wchar_t* cwd, HANDLE cancel, DWORD* code) {
    HANDLE conIn = NULL, conOut = NULL, job = NULL; PROCESS_INFORMATION pi;
    DWORD err = repConsoleOpen(&conIn, &conOut);
    if (err) { repStartFailed(out, exe, cwd, err); return err; }
    if (cancel) {
        job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim; memset(&lim, 0, sizeof(lim));
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   // nothing outlives us
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    }
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = conIn; si.hStdOutput = conOut; si.hStdError = conOut;
    g_repConStep = L"starting the program";
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, job ? CREATE_SUSPENDED : 0, NULL, cwd, &si, &pi)) {
        err = GetLastError();
        repStartFailed(out, exe, cwd, err);
        if (job) CloseHandle(job);
        CloseHandle(conIn); CloseHandle(conOut);
        FreeConsole();
        return err;
    }
    if (job) { AssignProcessToJobObject(job, pi.hProcess); ResumeThread(pi.hThread); }

    g_repHeldN = 0; g_repRefused = 0; g_repDropped = 0;
    SHORT sent = 0; wchar_t shown[REP_CON_WIDTH + 1] = L"";
    HANDLE w[2] = { pi.hProcess, cancel };
    BOOL cancelled = FALSE;
    for (;;) {
        DWORD r = WaitForMultipleObjects(cancel ? 2 : 1, w, FALSE, 100);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) {
            cancelled = TRUE;
            if (job) TerminateJobObject(job, ERROR_CANCELLED);
            WaitForSingleObject(pi.hProcess, 10000);
            break;
        }
        repRelayScreen(conOut, out, &sent, shown);
        if (g_repTick) g_repTick(pi.hProcess, out);
    }
    repRelayScreen(conOut, out, &sent, shown);   // whatever it printed just before it ended
    if (g_repAria) {
        wchar_t last[REP_CON_WIDTH + 1];
        repReadRow(conOut, sent, last);           // a last row with no line end after it
        if (g_repHeldN || !repAriaProgress(last)) repAriaRow(out, last);
        repAriaFlush(out);
    }
    if (job) CloseHandle(job);
    CloseHandle(conIn); CloseHandle(conOut);
    FreeConsole();
    CloseHandle(pi.hThread);
    *code = waitExitCode(pi.hProcess);
    if (cancelled) *code = ERROR_CANCELLED;
    return 0;
}

static DWORD repairRunOne(HANDLE out, const wchar_t* tool, const wchar_t* cmdline, BOOL* restart) {
    wchar_t mark[2] = { REP_STEP_MARK, 0 };
    repWrite(out, mark);
    repWrite(out, L"\r\n> ");
    repWrite(out, cmdline);
    repWrite(out, L"\r\n");

    wchar_t exe[MAX_PATH]; systemPath(exe, tool);
    size_t cn = wcslen(cmdline) + 1;
    wchar_t* cmd = (wchar_t*)malloc(cn * sizeof(wchar_t));
    if (!cmd) return ERROR_OUTOFMEMORY;
    wcscpy(cmd, cmdline);

    wchar_t line[128];
    DWORD code = 0;
    DWORD err = repRunOnConsole(out, exe, cmd, NULL, NULL, &code);
    free(cmd);
    if (err) {
        swprintf(line, 128, L"Couldn't start %s (error %lu).\r\n", tool, err);
        repWrite(out, line);
        return err;
    }

    if (code == ERROR_SUCCESS_REBOOT_REQUIRED) {
        *restart = TRUE;
        repWrite(out, L"\r\nDone. Restart the PC to finish.\r\n");
    } else if (code == 0) {
        repWrite(out, L"\r\nDone.\r\n");
    } else {
        swprintf(line, 128, L"\r\nFailed with exit code %lu (0x%08lX).\r\n", code, code);
        repWrite(out, line);
    }
    return code;
}

static BOOL repFailed(DWORD code) { return code != 0 && code != ERROR_SUCCESS_REBOOT_REQUIRED; }

#if WCU_DOWNLOAD
static DWORD uupRunJob(const wchar_t* spec, HANDLE out);   // download.c
#endif
static DWORD esdIsoRunJob(const wchar_t* spec, HANDLE out);   // esdiso.c

// Runs the whole job. The WIM check repeats here, right before the command, so nothing but a matching
// WIM can ever reach DISM, whatever happened between choosing the file and approving UAC.
static DWORD repairRunJob(int job, const wchar_t* wim, HANDLE out) {
    DWORD failed = 0; BOOL restart = FALSE;
#if WCU_DOWNLOAD
    if (job == REP_UUP) return uupRunJob(wim, out);
#endif
    if (job == REP_ESDISO) return esdIsoRunJob(wim, out);
    int nCmds = 0;
    const RepCmd* cmds = repJobCmds(job, &nCmds);
    if (cmds) {
        for (int i = 0; i < nCmds; i++)
            if (repFailed(repairRunOne(out, cmds[i].tool, cmds[i].cmd, &restart))) failed++;
    } else if (job == REP_WIM) {
        RepLive live; repairReadLive(&live);
        WimEntry* list = (WimEntry*)malloc(sizeof(WimEntry) * REP_WIM_MAX);
        int n = list ? repairReadWim(wim, list, REP_WIM_MAX) : -1;
        int pick = (n > 0) ? repairPickImage(list, n, &live) : -1;
        if (pick < 0) {
            repWrite(out, n < 0 ? L"Couldn't read the WIM file. If it's on a mapped network drive, copy it to this PC first. Nothing was run.\r\n"
                                : L"The WIM doesn't hold this PC's Windows build. Nothing was run.\r\n");
            failed = 1;
        } else {
            wchar_t line[512], cmd[MAX_PATH + 128];
            swprintf(line, 512, L"This PC is build %s %s. Using image %d of the WIM: %s, build %s %s.\r\n",
                     live.full, live.arch, list[pick].index, list[pick].name, list[pick].build, list[pick].arch);
            repWrite(out, line);
            repairWimCommand(wim, list[pick].index, cmd, MAX_PATH + 128);
            if (repFailed(repairRunOne(out, L"dism.exe", cmd, &restart))) failed++;
        }
        free(list);
    }
    if (failed > 0xFF) failed = 0xFF;
    return failed | (restart ? REP_RESTART_BIT : 0);
}

static DWORD repairRunToPipe(int job, const wchar_t* wim, const wchar_t* pipeName) {
    HANDLE out = CreateFileW(pipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return REP_NO_PIPE;
    DWORD code = repairRunJob(job, wim, out);
    CloseHandle(out);
    return code;
}

// The elevated helper: --repair=<job> --pipe=<name> [--wim="<path>"]
static int repairHelperMain(const wchar_t* cmd) {
    static const struct { const wchar_t* name; int job; } JOBS[] = {
        { L"sfc ", REP_SFC }, { L"scan ", REP_SCAN }, { L"restore ", REP_RESTORE }, { L"wim ", REP_WIM },
        { L"wuc ", REP_WUC }, { L"rson ", REP_RSON }, { L"rsoff ", REP_RSOFF }, { L"esdiso ", REP_ESDISO },
#if WCU_DOWNLOAD
        { L"uup ", REP_UUP },
#endif
    };
    int job = REP_NONE;
    const wchar_t* a = cmd + 9;
    for (size_t i = 0; i < sizeof(JOBS) / sizeof(JOBS[0]); i++) if (!wcsncmp(a, JOBS[i].name, wcslen(JOBS[i].name))) job = JOBS[i].job;
    const wchar_t* p = wcsstr(cmd, L"--pipe=");
    if (job == REP_NONE || !p) return ERROR_INVALID_PARAMETER;
    wchar_t pipe[128]; int n = 0;
    for (p += 7; *p && *p != L' ' && n < 127; p++) pipe[n++] = *p;
    pipe[n] = 0;
    if (wcsncmp(pipe, L"\\\\.\\pipe\\wcu-repair-", 20) != 0) return ERROR_INVALID_PARAMETER;
    wchar_t wim[REP_ARG_MAX] = L"";
    if (job == REP_WIM || job == REP_UUP || job == REP_ESDISO) {
        const wchar_t* w = wcsstr(p, L"--wim=\"");
        if (!w) return ERROR_INVALID_PARAMETER;
        n = 0;
        for (w += 7; *w && *w != L'"' && n < REP_ARG_MAX - 1; w++) wim[n++] = *w;
        wim[n] = 0;
    }
    return (int)repairRunToPipe(job, wim, pipe);
}

// ---------- the output panel's text ----------
typedef struct { wchar_t* s; int len, cap; } RepLine;
static RepLine g_repLines[REP_MAX_LINES];
static int  g_repN = 0;
static BOOL g_repCR = FALSE;              // a carriage return is waiting: a new line, or a progress overwrite?
static volatile LONG g_repStep = 0;
static volatile LONG g_repPosted = 0;

#if WCU_DOWNLOAD
static void dlpFeedLine(const wchar_t* s);      // dlprogress.c
static void dlpFeedStep(int step);
static void dlpFeedBytes(const wchar_t* s);
static void dlpEnd(BOOL ok);
#endif
static wchar_t g_repProg[64];              // a progress report part-way in: one can be cut in two by the pipe
static int  g_repProgN = -1;               // -1: not inside one

// The last line is whole
static void repLineDone(void) {
#if WCU_DOWNLOAD
    if (g_repLines[g_repN - 1].s) dlpFeedLine(g_repLines[g_repN - 1].s);
#endif
}
static void repLineNew(void) {
    if (g_repN >= REP_MAX_LINES) {        // keep the newest lines of a very long run
        for (int i = 0; i < 500; i++) free(g_repLines[i].s);
        memmove(g_repLines, g_repLines + 500, sizeof(RepLine) * (REP_MAX_LINES - 500));
        g_repN -= 500;
    }
    RepLine* l = &g_repLines[g_repN++];
    l->s = NULL; l->len = 0; l->cap = 0;
}
static void repLineAdd(wchar_t c) {
    RepLine* l = &g_repLines[g_repN - 1];
    if (l->len >= REP_MAX_LINE) return;
    if (l->len + 1 >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 128;
        wchar_t* s = (wchar_t*)realloc(l->s, (size_t)cap * sizeof(wchar_t));
        if (!s) return;
        l->s = s; l->cap = cap;
    }
    l->s[l->len++] = c;
    l->s[l->len] = 0;
}
// A carriage return on its own means the runner is redrawing the line, for a progress update
static void repairFeed(const wchar_t* s, int n) {
    EnterCriticalSection(&g_lock);
    if (g_repN == 0) repLineNew();
    for (int i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (c == REP_PROG_MARK) { g_repProgN = 0; continue; }
        if (g_repProgN >= 0) {
            if (c == REP_PROG_END || g_repProgN >= 63) {
                g_repProg[g_repProgN] = 0; g_repProgN = -1;
#if WCU_DOWNLOAD
                dlpFeedBytes(g_repProg);
#endif
            } else g_repProg[g_repProgN++] = c;
            continue;
        }
        if (c == REP_STEP_MARK) {
            g_repStep++;
#if WCU_DOWNLOAD
            dlpFeedStep((int)g_repStep);
#endif
            continue;
        }
        if (g_repCR) {
            if (c == L'\r') continue;
            g_repCR = FALSE;
            if (c == L'\n') { repLineDone(); repLineNew(); continue; }
            RepLine* l = &g_repLines[g_repN - 1];
            l->len = 0;
            if (l->s) l->s[0] = 0;
        }
        if (c == L'\r') { g_repCR = TRUE; continue; }
        if (c == L'\n') { repLineDone(); repLineNew(); continue; }
        repLineAdd(c);
    }
    LeaveCriticalSection(&g_lock);
}
static void repairClear(void) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_repN; i++) free(g_repLines[i].s);
    g_repN = 0; g_repCR = FALSE; g_repStep = 0; g_repProgN = -1;
    LeaveCriticalSection(&g_lock);
}

// ---------- running a job from the window ----------
static BOOL g_repRunning = FALSE, g_repShow = TRUE, g_repFollow = TRUE, g_repDrag = FALSE;
static int  g_repTotal = 1, g_repTop = 0, g_repRows = 0, g_repVisible = 1, g_repDragOfs = 0, g_repThumbH = 0;
static RECT g_rcRepBar;
static wchar_t g_repStatus[256];
static HANDLE g_repPipe = NULL, g_repRunner = NULL;
static int g_repThreadJob = REP_NONE, g_repJob = REP_NONE;
static wchar_t g_repThreadWim[REP_ARG_MAX], g_repPipeName[128];

static DWORD WINAPI repairRunnerThread(LPVOID arg) {
    (void)arg;
    return repairRunToPipe(g_repThreadJob, g_repThreadWim, g_repPipeName);
}

static DWORD WINAPI repairReaderProc(LPVOID arg) {
    (void)arg;
    HANDLE pipe = g_repPipe, runner = g_repRunner;
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov)); ov.hEvent = ev;
    BOOL connected = ConnectNamedPipe(pipe, &ov);
    if (!connected) {
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) connected = TRUE;
        else if (e == ERROR_IO_PENDING) {
            HANDLE w[2] = { ev, runner };                   // the runner may exit without ever connecting
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) == WAIT_OBJECT_0) connected = TRUE;
            else { CancelIo(pipe); GetOverlappedResult(pipe, &ov, &e, TRUE); }
        }
    }
    if (connected) {
        wchar_t buf[2048];
        for (;;) {
            DWORD got = 0;
            ResetEvent(ev);
            if (!ReadFile(pipe, buf, sizeof(buf), &got, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) break;
                if (!GetOverlappedResult(pipe, &ov, &got, TRUE)) break;
            }
            repairFeed(buf, (int)(got / sizeof(wchar_t)));
            if (InterlockedExchange(&g_repPosted, 1) == 0) PostMessageW(g_hwnd, WM_APP_REPAIROUT, 0, 0);
        }
    }
    WaitForSingleObject(runner, INFINITE);
    DWORD code = REP_NO_PIPE;
    if (!GetExitCodeProcess(runner, &code)) GetExitCodeThread(runner, &code);
    CloseHandle(runner); CloseHandle(pipe); CloseHandle(ev);
    PostMessageW(g_hwnd, WM_APP_REPAIRDONE, (WPARAM)code, 0);
    return 0;
}

#if WCU_DOWNLOAD
static void dlOnJobSucceeded(void);   // download.c
#endif
static void esdIsoOnDone(DWORD code);  // esdiso.c

static void repairOnDone(DWORD code) {
    g_repRunning = FALSE;
    InterlockedExchange(&g_busy, 0);
    DWORD failed = code & 0xFF;
    const wchar_t* tail = (code & REP_RESTART_BIT) ? L" Restart the PC to finish." : L"";
    if ((g_repJob == REP_UUP || g_repJob == REP_ESDISO) && code == REP_CANCELLED)
        wcscpy(g_repStatus, L"Cancelled.");
    else if (code > (REP_RESTART_BIT | 0xFF))
        wcscpy(g_repStatus, L"The administrator helper stopped, so the job didn't finish.");
    else if (failed == 0 && g_repTotal > 1)
        swprintf(g_repStatus, 256, L"Finished: all %d commands succeeded.%s", g_repTotal, tail);
    else if (failed == 0)
        swprintf(g_repStatus, 256, L"Finished successfully.%s", tail);
    else if (g_repTotal > 1)
        swprintf(g_repStatus, 256, L"Finished: %lu of %d commands failed.%s", failed, g_repTotal, tail);
    else
        swprintf(g_repStatus, 256, L"Failed. See the output for the reason.%s", tail);
    if (failed || code > (REP_RESTART_BIT | 0xFF)) g_repShow = TRUE;
    InvalidateRect(g_hwnd, NULL, FALSE);
#if WCU_DOWNLOAD
    if (g_repJob == REP_UUP) dlpEnd(code == 0);
    if (g_repJob == REP_UUP && code == 0) dlOnJobSucceeded();
#endif
    if (g_repJob == REP_ESDISO) esdIsoOnDone(code);
}

// Nothing ran: say why and free the app for the next job
static void repairNotStarted(const wchar_t* why) {
    wcscpy(g_repStatus, why);
    InterlockedExchange(&g_busy, 0);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void repairStart(int job, const wchar_t* wim) {
    if (!jobBegin()) return;
    noteHide();                                    // the last job's result
    repairClear();
    g_repJob = job;                                // from here on the text and status are this job's
    g_repStatus[0] = 0;
    swprintf(g_repPipeName, 128, L"\\\\.\\pipe\\wcu-repair-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    // The default security lets this user and administrators write to it, and nobody else
    g_repPipe = CreateNamedPipeW(g_repPipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 65536, 0, NULL);
    if (g_repPipe == INVALID_HANDLE_VALUE) {
        repairNotStarted(L"Couldn't set up the output window, so nothing was run.");
        return;
    }
    g_repRunner = NULL;
    if (g_elevated) {
        g_repThreadJob = job;
        g_repThreadWim[0] = 0;
        if (wim) lstrcpynW(g_repThreadWim, wim, REP_ARG_MAX);
        g_repRunner = CreateThread(NULL, 0, repairRunnerThread, NULL, 0, NULL);
    } else {
        static const wchar_t* NAMES[] = { L"", L"sfc", L"scan", L"restore", L"wim", L"uup", L"wuc", L"rson", L"rsoff", L"esdiso" };
        wchar_t args[REP_ARG_MAX + 200]; BOOL cancelled = FALSE;
        if (wim) swprintf(args, REP_ARG_MAX + 200, L"--repair=%s --pipe=%s --wim=\"%s\"", NAMES[job], g_repPipeName, wim);
        else     swprintf(args, REP_ARG_MAX + 200, L"--repair=%s --pipe=%s", NAMES[job], g_repPipeName);
        if (!startElevatedHelper(args, &g_repRunner, &cancelled)) {
            CloseHandle(g_repPipe);
            repairNotStarted(cancelled ? L"Cancelled, so nothing was run." : L"Couldn't start the administrator helper, so nothing was run.");
            return;
        }
    }
    if (!g_repRunner) {
        CloseHandle(g_repPipe);
        repairNotStarted(L"Couldn't start the job, so nothing was run.");
        return;
    }
    int nCmds = 0;
    g_repTotal = repJobCmds(job, &nCmds) ? nCmds : 1;
    g_repFollow = TRUE;
    g_repRunning = TRUE;
    startWorker(repairReaderProc, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void repairWimClicked(void) {
    wchar_t file[MAX_PATH] = L"";
    if (!pickOpen(file, L"WIM files (*.wim)\0*.wim\0", L"Choose a WIM file")) return;
    WimEntry* list = (WimEntry*)malloc(sizeof(WimEntry) * REP_WIM_MAX);
    if (!list) return;
    int n = repairReadWim(file, list, REP_WIM_MAX);
    if (n <= 0) {
        okDialog(L"Couldn't read this WIM", L"Windows couldn't read this file as a WIM, so nothing was run.");
        free(list);
        return;
    }
    int pick = repairPickImage(list, n, &g_repLive);
    if (pick < 0) {
        static wchar_t mine[64], theirs[4][64];
        DlgRow rows[5]; int nr = 0;
        swprintf(mine, 64, L"%s %s", g_repLive.full, g_repLive.arch);
        rows[nr].left = L"This PC"; rows[nr].right = mine; nr++;
        for (int i = 0; i < n && nr < 5; i++) {                  // each different build once
            wchar_t b[64]; BOOL seen = FALSE;
            swprintf(b, 64, L"%s %s", list[i].build, list[i].arch);
            for (int k = 0; k < nr - 1; k++) if (!wcscmp(theirs[k], b)) seen = TRUE;
            if (seen) continue;
            wcscpy(theirs[nr - 1], b);
            rows[nr].left = L"In the WIM"; rows[nr].right = theirs[nr - 1]; nr++;
        }
        showDialog(g_hwnd, L"This WIM doesn't match this PC",
                   L"Restore using WIM only runs when the WIM holds the same Windows build and architecture as this PC. Nothing was run.",
                   rows, nr, L"OK", NULL, 0);
        free(list);
        return;
    }
    if (askInApp()) {
        wchar_t msg[MAX_PATH + 300];
        swprintf(msg, MAX_PATH + 300, L"%s\n\nImage %d: %s, build %s.\n\nThis can take a long time and can't be cancelled once it starts.",
                 file, list[pick].index, list[pick].name, list[pick].build);
        if (!showDialog(g_hwnd, L"Restore using this WIM?", msg, NULL, 0, L"Restore", L"Cancel", 1)) { free(list); return; }
    }
    free(list);
    repairStart(REP_WIM, file);
}

static void repairAction(int id) {
    BOOL ask = askInApp();
    switch (id) {
    case A_R_RS: {                                  // a toggle: it turns reserved storage the other way
        BOOL on = repReservedState() == 1;
        if (ask && !showDialog(g_hwnd, on ? L"Turn reserved storage off?" : L"Turn reserved storage on?",
                on ? L"Windows stops keeping disk space set aside for updates, temporary files and caches, so that space becomes free. "
                     L"DISM refuses while an update is installing or waiting for a restart."
                   : L"Windows sets disk space aside (usually about 7 GB) so updates always have room to install. "
                     L"DISM refuses while an update is installing or waiting for a restart.",
                NULL, 0, on ? L"Turn off" : L"Turn on", L"Cancel", 1)) return;
        repairStart(on ? REP_RSOFF : REP_RSON, NULL);
        break;
    }
    case A_R_WUC:
        if (ask && !showDialog(g_hwnd, L"Run Windows Update Cleanup?",
                L"This analyzes the component store, then removes old versions of updated Windows components. "
                L"It can take a long time and can't be cancelled once it starts.",
                NULL, 0, L"Run", L"Cancel", 1)) return;
        repairStart(REP_WUC, NULL);
        break;
    case A_R_SFC:
        if (ask && !showDialog(g_hwnd, L"Run SFC Scannow?",
                L"This checks Windows' system files and repairs any that are damaged. It can take a long time and can't be "
                L"cancelled once it starts. Right after Windows Update Cleanup, restart the PC first so it finishes faster.",
                NULL, 0, L"Run", L"Cancel", 1)) return;
        repairStart(REP_SFC, NULL);
        break;
    case A_R_SCAN:
        repairStart(REP_SCAN, NULL);               // only reads, so it needs no confirmation of its own
        break;
    case A_R_RESTORE:
        if (ask && !showDialog(g_hwnd, L"Run Restore Health?",
                L"DISM may download repair files from Windows Update. This can take a long time and can't be cancelled once it starts.",
                NULL, 0, L"Run", L"Cancel", 1)) return;
        repairStart(REP_RESTORE, NULL);
        break;
    case A_R_WIM:
        repairWimClicked();
        break;
    }
}

// The whole output as one text, one line per row, for Copy and for the UUP log. The caller frees it.
static wchar_t* repJoinLines(void) {
    EnterCriticalSection(&g_lock);
    size_t total = 1;
    for (int i = 0; i < g_repN; i++) total += (size_t)g_repLines[i].len + 2;
    wchar_t* all = (wchar_t*)malloc(total * sizeof(wchar_t));
    if (all) {
        wchar_t* p = all;
        for (int i = 0; i < g_repN; i++) {
            if (g_repLines[i].len) { wmemcpy(p, g_repLines[i].s, g_repLines[i].len); p += g_repLines[i].len; }
            *p++ = L'\r'; *p++ = L'\n';
        }
        *p = 0;
    }
    LeaveCriticalSection(&g_lock);
    return all;
}

static void repairCopy(void) {
    wchar_t* all = repJoinLines();
    if (all) { copyText(all); free(all); }
}

// ---------- drawing ----------
static BOOL repLooksLikeError(const RepLine* l) {
    if (!l->s) return FALSE;
    for (const wchar_t* p = l->s; *p; p++)
        if (!_wcsnicmp(p, L"error", 5) || !_wcsnicmp(p, L"failed", 6) || !_wcsnicmp(p, L"is not recognized", 17)) return TRUE;
    return FALSE;
}

static void repDrawOutput(int cL, int cR, int oTop, const wchar_t* title, const wchar_t* runningText, BOOL markErrors);

// ---------- one terminal at a time ----------
// While a job that prints to a terminal box (or a DISM update install) runs, only the section it belongs to keeps its
// box; every other section's is switched off until the job ends. Returns that section's page, or -1.
static int termOwner(void) {
    if (g_busy != 2) return -1;
    if (g_repRunning) return g_repJob == REP_UUP ? PAGE_DOWNLOAD : PAGE_REPAIR;    // ESD to ISO's output is kept on Windows Repair
    switch (g_jobPage) {
    case PAGE_REPAIR: case PAGE_DOWNLOAD: case PAGE_DISM: case PAGE_APPS: case PAGE_OFFICE: case PAGE_REMOVE: return g_jobPage;
    }
    return -1;
}
static BOOL termOff(void) { int o = termOwner(); return o >= 0 && o != g_page; }

static const wchar_t* termOffText(void) {
    static wchar_t t[160];
    int o = termOwner();
    const wchar_t* who = o == PAGE_DOWNLOAD ? L"Download Windows" : o == PAGE_DISM ? L"Manual Updates" : o == PAGE_APPS ? L"Install Apps"
                       : o == PAGE_OFFICE ? L"MS Office 365" : o == PAGE_REMOVE ? L"Remove Apps"
                       : (g_repRunning && g_repJob == REP_ESDISO) ? L"ESD to ISO" : L"Windows Repair";
    swprintf(t, 160, L"Not available while %s is working. Its progress is on %s.", who,
             (g_repRunning && g_repJob == REP_ESDISO) ? L"the Windows AIO page" : L"that page");
    return t;
}

static void drawRepair(int cL, int cR) {
    if (!g_repLive.build) repairReadLive(&g_repLive);
    int pad = dp(16), top = pad;
    Col sf = surfaceCol();
    BOOL en = (g_busy == 0), shield = !g_elevated;

    // ---------- the commands ----------
    int cardB = top + dp(268);
    fillRoundRect(cL, top, cR, cardB, dp(8), sf.r, sf.g, sf.b, sf.a);
    drawGlyph(g_fHeadIcon, 0xE95E, cL + dp(44), top + dp(44), g_accent);
    RECT t1 = { cL + dp(84), top + dp(14), cR - dp(16), top + dp(46) };
    drawText(g_fH1, L"Windows Repair", t1, textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    wchar_t sub[128];
    swprintf(sub, 128, L"This PC is Windows build %s %s", g_repLive.full, g_repLive.arch);
    RECT t2 = { cL + dp(84), top + dp(46), cR - dp(16), top + dp(68) };
    drawText(g_fBody, sub, t2, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int x1 = cL + dp(20), x2 = cR - dp(20), bh = dp(36), gap = dp(10);
    int y = top + dp(82);
    int w3 = (x2 - x1 - gap * 2) / 3;
    // Reserved Storage, Windows Update Cleanup, then SFC Scannow last: the order they're best run in
    int rs = repReservedState();
    const wchar_t* rsLabel = L"Reserved Storage";
    RECT rb = { x1, y, x1 + w3, y + bh };
    aioButton(A_R_RS, rb.left, rb.top, rb.right, rb.bottom, rsLabel, FALSE, en, shield);
    {   // the status light: green on, red off, grey if Windows won't say. It sits left of the centred label.
        int d = dp(10), sh = shield ? dp(16) + dp(6) : 0;
        int lx = rb.left + ((rb.right - rb.left) - (sh + textWidth(g_fBody, rsLabel))) / 2 - d - dp(8);
        int ly = (rb.top + rb.bottom) / 2 - d / 2;
        BYTE r = 128, g = 128, b = 128;
        if (rs == 1) { r = 16;  g = 172; b = 72; }
        if (rs == 0) { r = 212; g = 48;  b = 40; }
        fillRoundRect(lx, ly, lx + d, ly + d, d / 2, r, g, b, en ? 1.0 : 0.5);
    }
    aioButton(A_R_WUC, x1 + w3 + gap, y, x1 + w3 * 2 + gap, y + bh, L"Windows Update Cleanup", FALSE, en, shield);
    aioButton(A_R_SFC, x1 + w3 * 2 + gap * 2, y, x2, y + bh, L"SFC Scannow", TRUE, en, shield);
    y += bh + dp(6);
    // one hint under Reserved Storage, and one shared by Windows Update Cleanup and SFC Scannow
    RECT n1 = { x1 + dp(2), y, x1 + w3 - dp(2), y + dp(36) };
    drawText(g_fSmall, L"Frees up 7–10 GB when turned off. Green is on, red is off.", n1, mutedRGB(), DT_LEFT | DT_WORDBREAK);
    RECT n2 = { x1 + w3 + gap + dp(2), y, x2 - dp(2), y + dp(36) };
    drawText(g_fSmall, L"If you have run Windows Update Cleanup, restart the PC before SFC Scannow so it finishes faster.",
             n2, mutedRGB(), DT_LEFT | DT_WORDBREAK);
    y += dp(44);
    aioButton(A_R_SCAN, x1, y, x1 + w3, y + bh, L"Scan Health", FALSE, en, shield);
    aioButton(A_R_RESTORE, x1 + w3 + gap, y, x1 + w3 * 2 + gap, y + bh, L"Restore Health", FALSE, en, shield);
    aioButton(A_R_WIM, x1 + w3 * 2 + gap * 2, y, x2, y + bh, L"Restore using WIM", FALSE, en, shield);
    y += bh + dp(6);
    // a short line under each of the three
    static const wchar_t* DESC[3] = {
        L"Checks Windows for damage. Only reads, changes nothing.",
        L"Repairs damage, downloading good files from Windows Update.",
        L"Repairs damage offline, from a WIM of this PC's exact build.",
    };
    for (int i = 0; i < 3; i++) {
        int l = x1 + (w3 + gap) * i, r = (i == 2) ? x2 : l + w3;
        RECT d = { l + dp(2), y, r - dp(2), y + dp(36) };
        drawText(g_fSmall, DESC[i], d, mutedRGB(), DT_LEFT | DT_WORDBREAK);
    }

    wchar_t st[256] = L"";
    if (g_repRunning) {
        LONG s = g_repStep; if (s < 1) s = 1;
        if (g_repTotal > 1) swprintf(st, 256, L"Running command %ld of %d…", s, g_repTotal);
        else wcscpy(st, L"Running…");
    }
    repDrawOutput(cL, cR, cardB + dp(12), L"Output", st, FALSE);
}

// How many rows a line takes; the drawing below walks the same breaks
static int repLineRows(const RepLine* l, int perRow) {
    int rows = 0, off = 0;
    while (off < l->len) { wrapRow(l->s, l->len, off, perRow, &off); rows++; }
    return rows ? rows : 1;
}

// The output window, shared with the Download Windows page: a title, what's happening now, and the text.
static void repDrawOutput(int cL, int cR, int oTop, const wchar_t* title, const wchar_t* runningText, BOOL markErrors) {
    Col sf = surfaceCol();
    int pad = dp(16), hdrH = dp(44);
    int oBottom = g_repShow ? g_bh - pad : oTop + hdrH;
    fillRoundRect(cL, oTop, cR, oBottom, dp(8), sf.r, sf.g, sf.b, sf.a);

    int smallW = dp(64), hideW = dp(104), by = oTop + dp(8), sbh = dp(28);
    BOOL off = termOff();
    // Windows Repair and Download Windows share one text store: each shows only its own job's text
    BOOL mine = !off && (g_repJob == REP_NONE || (g_repJob == REP_UUP) == (g_page == PAGE_DOWNLOAD));
    aioButton(A_R_TOGGLE, cR - dp(12) - hideW, by, cR - dp(12), by + sbh, g_repShow ? L"Hide output" : L"Show output", FALSE, !off, FALSE);
    aioButton(A_R_COPY, cR - dp(12) - hideW - dp(8) - smallW, by, cR - dp(12) - hideW - dp(8), by + sbh, L"Copy", FALSE, mine && g_repN > 0, FALSE);

    int titleW = textWidth(g_fBodyBold, title);
    RECT oh = { cL + dp(20), oTop, cL + dp(20) + titleW + dp(2), oTop + hdrH };
    drawText(g_fBodyBold, title, oh, off ? mutedRGB() : textRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT ost = { oh.right + dp(10), oTop, cR - dp(12) - hideW - dp(8) - smallW - dp(10), oTop + hdrH };
    if (mine || g_repJob == REP_NONE) drawText(g_fSmall, off ? L"" : g_repRunning ? runningText : g_repStatus, ost, g_repRunning ? g_accent : mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SetRectEmpty(&g_rcRepBar);
    if (!g_repShow) return;

    RECT box = { cL + dp(12), oTop + hdrH, cR - dp(12), oBottom - dp(12) };
    Col kf = g_dark ? (Col){ 0,0,0, 0.28 } : (Col){ 0,0,0, 0.045 };
    fillRoundRect(box.left, box.top, box.right, box.bottom, dp(4), kf.r, kf.g, kf.b, kf.a);

    if (!mine) {
        RECT hint = { box.left + dp(10), box.top + dp(6), box.right - dp(10), box.top + dp(23) };
        drawText(g_fSmall, off ? termOffText() : L"What each command prints shows here as it runs.", hint, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    int rowH = dp(17), inX = dp(10), barW = dp(10);
    int textL = box.left + inX, textR = box.right - barW - dp(4), textT = box.top + dp(6);
    int visible = (box.bottom - dp(6) - textT) / rowH;
    if (visible < 1) visible = 1;
    double cw = textWidth(g_fMono, L"0000000000") / 10.0;
    int perRow = (cw > 0) ? (int)((textR - textL) / cw) : 80;
    if (perRow < 10) perRow = 10;
    if (perRow > 1000) perRow = 1000;

    EnterCriticalSection(&g_lock);
    int total = 0;
    for (int i = 0; i < g_repN; i++) total += repLineRows(&g_repLines[i], perRow);
    int maxTop = total > visible ? total - visible : 0;
    if (g_repFollow || g_repTop >= maxTop) { g_repTop = maxTop; g_repFollow = TRUE; }
    if (g_repTop < 0) g_repTop = 0;
    g_repRows = total; g_repVisible = visible;

    if (total == 0 || (g_repN == 1 && !g_repLines[0].len)) {
        RECT hint = { textL, textT, textR, textT + rowH };
        drawText(g_fSmall, L"What each command prints shows here as it runs.", hint, mutedRGB(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        int row = 0;
        for (int i = 0; i < g_repN && row < g_repTop + visible; i++) {
            RepLine* l = &g_repLines[i];
            int rows = repLineRows(l, perRow), off = 0;
            if (row + rows <= g_repTop) { row += rows; continue; }
            BOOL cmdLine = (l->len > 1 && l->s[0] == L'>' && l->s[1] == L' ');
            COLORREF lineCol = cmdLine ? g_accent : (markErrors && repLooksLikeError(l)) ? RGB(232, 72, 85) : textRGB();
            for (int r = 0; r < rows; r++, row++) {
                if (!l->len) continue;
                int next, n = wrapRow(l->s, l->len, off, perRow, &next), from = off;
                off = next;
                if (row < g_repTop || row >= g_repTop + visible) continue;
                wchar_t t[1001];
                wmemcpy(t, l->s + from, n); t[n] = 0;
                int yy = textT + (row - g_repTop) * rowH;
                RECT rr = { textL, yy, textR, yy + rowH };
                drawText(g_fMono, t, rr, lineCol, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    // scrollbar, shown only when there's more than fits
    if (total > visible) {
        RECT tr = { box.right - barW - dp(2), box.top + dp(4), box.right - dp(2), box.bottom - dp(4) };
        g_rcRepBar = tr;
        int trackH = tr.bottom - tr.top;
        int thumb = trackH * visible / total;
        if (thumb < dp(24)) thumb = dp(24);
        g_repThumbH = thumb;
        int ty = tr.top + (int)((long long)(trackH - thumb) * g_repTop / (maxTop ? maxTop : 1));
        Col s = sepCol();
        fillRoundRect(tr.right - dp(6), ty, tr.right - dp(2), ty + thumb, dp(2), s.r, s.g, s.b, s.a * (g_repDrag ? 5 : 3));
    }
}

static void repairWheel(int delta) {
    if (!g_repShow || termOff()) return;
    g_repFollow = FALSE;
    g_repTop += (delta > 0) ? -3 : 3;
    if (g_repTop < 0) g_repTop = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void repairDragTo(int y) {
    int span = (g_rcRepBar.bottom - g_rcRepBar.top) - g_repThumbH;
    int maxTop = g_repRows - g_repVisible;
    if (span <= 0 || maxTop <= 0) return;
    int v = y - g_rcRepBar.top - g_repDragOfs;
    if (v < 0) v = 0;
    if (v > span) v = span;
    g_repTop = (int)((long long)v * maxTop / span);
    g_repFollow = FALSE;
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void repairMouseDown(int x, int y) {
    POINT p = { x, y };
    if (!PtInRect(&g_rcRepBar, p)) return;
    int span = (g_rcRepBar.bottom - g_rcRepBar.top) - g_repThumbH;
    int maxTop = g_repRows - g_repVisible;
    int ty = g_rcRepBar.top + (maxTop > 0 ? (int)((long long)span * g_repTop / maxTop) : 0);
    g_repDragOfs = (y >= ty && y < ty + g_repThumbH) ? y - ty : g_repThumbH / 2;
    g_repDrag = TRUE;
    SetCapture(g_hwnd);
    repairDragTo(y);
}
