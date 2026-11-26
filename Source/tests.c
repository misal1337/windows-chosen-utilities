// ---- test harness: proves scan/delete are correct and bounded, on a throwaway sandbox ----
#include <stdio.h>
static void mkfile(const wchar_t* path, int bytes) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char* buf = (char*)calloc(bytes, 1); DWORD wr;
    WriteFile(h, buf, bytes, &wr, NULL); CloseHandle(h); free(buf);
}
int wmain(void) {
    setvbuf(stdout, NULL, _IONBF, 0);       // every line out at once, so a crash shows where it happened
    InitializeCriticalSection(&g_lock);     // as the app does at its start: the output panel's tests take it early
    wchar_t base[MAX_PATH], sand[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    swprintf(sand, MAX_PATH, L"%swcu_test_%lu", base, GetCurrentProcessId());
    CreateDirectoryW(sand, NULL);
    wchar_t sub[MAX_PATH], deep[MAX_PATH], p[MAX_PATH];
    swprintf(sub, MAX_PATH, L"%s\\sub", sand); CreateDirectoryW(sub, NULL);
    swprintf(deep, MAX_PATH, L"%s\\deep", sub); CreateDirectoryW(deep, NULL);
    swprintf(p, MAX_PATH, L"%s\\a.txt", sand);  mkfile(p, 100);
    swprintf(p, MAX_PATH, L"%s\\sub\\b.txt", sand); mkfile(p, 200);
    swprintf(p, MAX_PATH, L"%s\\sub\\deep\\c.txt", sand); mkfile(p, 300);
    swprintf(p, MAX_PATH, L"%s\\ro.txt", sand); mkfile(p, 50);
    SetFileAttributesW(p, FILE_ATTRIBUTE_READONLY);

    wchar_t* lp = longPath(sand);
    unsigned long long sz = dirSize(lp);
    wprintf(L"dirSize = %llu (expect 650)\n", sz);

    unsigned long long freed = deleteContents(lp);
    wprintf(L"deleteContents freed = %llu (expect 650)\n", freed);

    // sandbox root must still exist, but be empty
    DWORD a = GetFileAttributesW(sand);
    BOOL rootExists = (a != INVALID_FILE_ATTRIBUTES);
    unsigned long long after = dirSize(lp);
    wprintf(L"root still exists = %d (expect 1), bytes after = %llu (expect 0)\n", rootExists, after);
    free(lp);

    RemoveDirectoryW(sand);
    BOOL filePass = (sz == 650 && freed == 650 && rootExists && after == 0 && GetFileAttributesW(sand) == INVALID_FILE_ATTRIBUTES);
    wprintf(L"file test: %s\n", filePass ? L"PASS" : L"FAIL");

    // ---- the elevated helper must only touch admin categories, and only selected ones ----
    wchar_t sa[MAX_PATH], sb[MAX_PATH], su[MAX_PATH], fa[MAX_PATH], fb[MAX_PATH], fu[MAX_PATH];
    swprintf(sa, MAX_PATH, L"%swcu_adm_a_%lu", base, GetCurrentProcessId());
    swprintf(sb, MAX_PATH, L"%swcu_adm_b_%lu", base, GetCurrentProcessId());
    swprintf(su, MAX_PATH, L"%swcu_usr_%lu", base, GetCurrentProcessId());
    CreateDirectoryW(sa, NULL); CreateDirectoryW(sb, NULL); CreateDirectoryW(su, NULL);
    swprintf(fa, MAX_PATH, L"%s\\x.txt", sa); mkfile(fa, 10);
    swprintf(fb, MAX_PATH, L"%s\\x.txt", sb); mkfile(fb, 10);
    swprintf(fu, MAX_PATH, L"%s\\x.txt", su); mkfile(fu, 10);
    memset(g_cat, 0, sizeof(g_cat));
    wcscpy(g_cat[0].path, su); g_cat[0].kind = KIND_FOLDER; g_cat[0].available = TRUE;                             // user
    wcscpy(g_cat[1].path, sa); g_cat[1].kind = KIND_FOLDER; g_cat[1].available = TRUE; g_cat[1].needsAdmin = TRUE; // admin, selected
    wcscpy(g_cat[2].path, sb); g_cat[2].kind = KIND_FOLDER; g_cat[2].available = TRUE; g_cat[2].needsAdmin = TRUE; // admin, unselected
    runAdminClean((1 << 0) | (1 << 1));   // selects the user category too: it must be ignored
    BOOL scopePass = GetFileAttributesW(fa) == INVALID_FILE_ATTRIBUTES
                  && GetFileAttributesW(fb) != INVALID_FILE_ATTRIBUTES
                  && GetFileAttributesW(fu) != INVALID_FILE_ATTRIBUTES
                  && GetFileAttributesW(sa) != INVALID_FILE_ATTRIBUTES;
    wprintf(L"admin helper scope test: %s\n", scopePass ? L"PASS" : L"FAIL");
    DeleteFileW(fb); DeleteFileW(fu);
    RemoveDirectoryW(sa); RemoveDirectoryW(sb); RemoveDirectoryW(su);
    filePass = filePass && scopePass;

    // ---- only .msu/.cab paths with no embedded quote may reach the DISM command line ----
    BOOL dismPass = isUpdateFile(L"C:\\u\\kb1.msu") && isUpdateFile(L"C:\\my updates\\KB2.CAB")
                 && !isUpdateFile(L"C:\\u\\kb1.exe") && !isUpdateFile(L"C:\\u\\kb1.msu.txt")
                 && !isUpdateFile(L"C:\\u\\a\" /Add-Driver \"b.msu") && !isUpdateFile(L"") && !isUpdateFile(NULL);
    wprintf(L"update file filter test: %s\n", dismPass ? L"PASS" : L"FAIL");
    filePass = filePass && dismPass;

    // ---- Run as TI: only .exe / .reg paths, no embedded quote; the kind is reported ----
    {
        BOOL isReg = FALSE;
        BOOL tiPass = tiFileKind(L"C:\\tools\\app.exe", &isReg) && !isReg
                   && tiFileKind(L"C:\\my keys\\fix.REG", &isReg) && isReg
                   && !tiFileKind(L"C:\\tools\\script.bat", &isReg)
                   && !tiFileKind(L"C:\\tools\\a\" /x \"b.exe", &isReg)
                   && !tiFileKind(L"C:\\tools\\app.exe.txt", &isReg)
                   && !tiFileKind(L"", &isReg) && !tiFileKind(NULL, &isReg);
        wprintf(L"run-as-TI filter test: %s\n", tiPass ? L"PASS" : L"FAIL");
        filePass = filePass && tiPass;
    }

    // ---- the newest update: builds compare by revision as numbers, and only Windows' own updates count ----
    {
        const UpdPc* pc = updPc();
        BOOL cmpOk = updCmpBuild(L"26300.10008", L"26300.9818") > 0 && updCmpBuild(L"26300.9818", L"26300.9818") == 0
                  && updCmpBuild(L"26220.7000", L"26300.1") < 0 && updCmpBuild(L"29704.1000", L"26300.9818") > 0;
        BOOL titleOk = updTitleWanted(L"Windows 11, version 26H2 (26300.9818)") && updTitleWanted(L"Windows 11 Insider Preview 29704.1000 (rs_prerelease)")
                    && !updTitleWanted(L".NET Framework Security Update for Windows 11 - KB5127694 (26300.10008)")
                    && !updTitleWanted(L"Critical OOBE Update for Windows 11 - KB5128942 (26100.9544) (3)");
        BOOL safeOk = updOnly(L"26300.9818", L".") && !updOnly(L"26300.98\\18", L".") && !updOnly(L"", L".")
                   && updOnly(L"6c7bf258-2632-47dd-83ee-c23945978da1", L"-") && !updOnly(L"6c7b&x=1", L"-");
        BOOL pcOk = updOnly(pc->build, L".") && wcschr(pc->build, L'.') && pc->ring[0] && pc->channel[0];
        BOOL updPass = cmpOk && titleOk && safeOk && pcOk;
        wprintf(L"newest update test: %s (cmp %d title %d safe %d pc %d: %s %s %s)\n", updPass ? L"PASS" : L"FAIL",
                cmpOk, titleOk, safeOk, pcOk, pc->build, pc->ring, pc->channel);
        filePass = filePass && updPass;
    }

    // ---- the elevated helper's command line: a path can't be mistaken for its other arguments ----
    {
        wchar_t q[16], longQ[4];
        const wchar_t* end = cmdQuoted(L"C:\\a b\" --x", q, 16);
        BOOL ok = !wcscmp(q, L"C:\\a b") && *end == L'"';
        end = cmdQuoted(L"abcdef", longQ, 4);
        ok = ok && !wcscmp(longQ, L"abc") && *end == L'd';
        ok = ok && cmdNotify(L"none") == NULL && cmdNotify(L"--notify=5") == (HWND)5
                && cmdNotify(L"\"C:\\--notify=7\\a\" --notify=9") == (HWND)9;
        wprintf(L"helper command line test: %s\n", ok ? L"PASS" : L"FAIL");
        filePass = filePass && ok;
    }
    // ---- Windows Repair: the build check, the command line, decoding and the output text ----
    {
        RepLive live; memset(&live, 0, sizeof(live));
        live.build = 26300; wcscpy(live.arch, L"x64"); wcscpy(live.edition, L"Professional");
        WimEntry e[4]; memset(e, 0, sizeof(e));
        wcscpy(e[0].build, L"26100.1742"); wcscpy(e[0].arch, L"x64"); wcscpy(e[0].edition, L"Professional");
        wcscpy(e[1].build, L"26300.9457"); wcscpy(e[1].arch, L"ARM64"); wcscpy(e[1].edition, L"Professional");
        wcscpy(e[2].build, L"26300.9457"); wcscpy(e[2].arch, L"x64"); wcscpy(e[2].edition, L"Core");
        wcscpy(e[3].build, L"26300.9457"); wcscpy(e[3].arch, L"x64"); wcscpy(e[3].edition, L"Professional");
        BOOL pickOk = repairPickImage(e, 4, &live) == 3      // same edition preferred
                   && repairPickImage(e, 3, &live) == 2      // any edition of the right build and architecture
                   && repairPickImage(e, 2, &live) == -1     // wrong architecture, wrong build
                   && repairPickImage(e, 1, &live) == -1
                   && repairPickImage(e, 0, &live) == -1;
        wchar_t c[400];
        repairWimCommand(L"D:\\my iso\\install.wim", 6, c, 400);
        BOOL cmdOk = !wcscmp(c, L"Dism.exe /online /Cleanup-Image /RestoreHealth /Source:WIM:\"D:\\my iso\\install.wim\":6 /LimitAccess /NoRestart");

        // no DISM command may be able to stop and ask about restarting: nobody could answer it
        BOOL noPrompt = wcsstr(c, L"/NoRestart") && wcsstr(REP_SCAN_CMD.cmd, L"/NoRestart") && wcsstr(REP_RESTORE_CMD.cmd, L"/NoRestart");
        static const int JOBS[] = { REP_SFC, REP_SCAN, REP_RESTORE, REP_WUC, REP_RSON, REP_RSOFF };
#if WCU_DOWNLOAD
    {
        static const struct { int at; const wchar_t* s; } RUN[] = {
            { 0, L"=== Checking UUP Info . . ." }, { 40, L"=== Creating install.wim . . ." }, { 100, L"=== Updating install.wim / 2 image{s} . . ." },
            { 120, L"=== Servicing Index: 1" }, { 160, L"=== Adding Microsoft Edge . . ." }, { 160, L"=== Adding Apps . . ." }, { 250, L"Deployment Image Servicing and Management tool" }, { 321, L"=== Adding Updates . . ." },
            { 341, L"=== Adding LCU 09818-Windows11.0-KB5127936-x64.msu" },
            { 668, L"The operation completed successfully." }, { 775, L"The operation completed successfully." }, { 1064, L"Unmounting image" },      // inside the update: split in proportion to RUN1's
            { 1102, L"=== Servicing Index: 2" }, { 1142, L"=== Adding Microsoft Edge . . ." },
            { 1142, L"=== Adding Apps . . ." }, { 1230, L"Deployment Image Servicing and Management tool" }, { 1302, L"=== Adding Updates . . ." }, { 1322, L"=== Adding LCU 09818-Windows11.0-KB5127936-x64.msu" },
            { 1770, L"The operation completed successfully." }, { 1916, L"The operation completed successfully." }, { 2312, L"Unmounting image" },
            { 2364, L"=== Rebuilding install.wim . . ." }, { 2384, L"=== Creating winre.wim . . ." }, { 2444, L"=== Remounting image . . ." }, { 2444, L"=== Adding LCU for boot.wim . . ." },
            { 2464, L"=== Adding LCU 09818-Windows11.0-KB5127936-x64.msu" }, { 2645, L"=== Adding winre.wim to install.wim . . ." }, { 2665, L"=== Creating ISO . . ." } };
        // and the Pro build of 25 Nov 2026, from DISM's log
        static const struct { int at; const wchar_t* s; } RUN1[] = {
            { 0, L"=== Checking UUP Info . . ." }, { 135, L"=== Updating install.wim / 1 image{s} . . ." }, { 148, L"=== Servicing Index: 1" }, { 180, L"=== Adding Microsoft Edge . . ." },
            { 185, L"=== Adding Apps . . ." }, { 235, L"Deployment Image Servicing and Management tool" }, { 270, L"=== Adding Updates . . ." }, { 307, L"=== Adding LCU 10075-Windows11.0-KB5132057-x64.msu" }, { 758, L"The operation completed successfully." }, { 904, L"The operation completed successfully." },
            { 1300, L"Saving image" }, { 1330, L"The operation completed successfully." }, { 1332, L"Unmounting image" }, { 1383, L"The operation completed successfully." }, { 1384, L"=== Rebuilding install.wim . . ." }, { 1434, L"=== Creating winre.wim . . ." },
            { 1510, L"=== Remounting image . . ." }, { 1535, L"=== Adding LCU for boot.wim . . ." }, { 1773, L"=== Adding winre.wim to install.wim . . ." }, { 1785, L"=== Creating ISO . . ." } };
        BOOL ok1 = TRUE;
        {
            DlProg p; ULONGLONG c = 1000000, end = c + 1807000; int last = 0, k = 0;
            dlpBegin(&p, 1, 0, 10, 1, c - 313000); dlpStep(&p, 3, c);
            for (ULONGLONG n = c; n < end; n += 60000) {
                for (; k < (int)(sizeof(RUN1) / sizeof(RUN1[0])) && c + RUN1[k].at * 1000ULL <= n; k++) dlpLine(&p, RUN1[k].s, c + RUN1[k].at * 1000ULL);
                double left = dlpLeft(&p, n), real = (end - n) / 1000.0; int pct = dlpPercent(&p, n); wchar_t x[160];
                if (pct < last || fabs(left - real) > 240) ok1 = FALSE;      // within 4 minutes all the way
                last = pct; dlpText(&p, n, x, 160); wprintf(L"  pro %s  (really %.1f min)\n", x, real / 60);
            }
        }
        // and the Home and Pro build of the same day, with all the apps: the save after them held the bar for four minutes
        static const struct { int at; const wchar_t* s; } RUN2[] = {
            { 0, L"=== Checking UUP Info . . ." }, { 135, L"=== Updating install.wim / 2 image{s} . . ." }, { 149, L"=== Servicing Index: 1" }, { 196, L"=== Adding Microsoft Edge . . ." },
            { 196, L"Deployment Image Servicing and Management tool" }, { 200, L"=== Adding Apps . . ." }, { 371, L"Deployment Image Servicing and Management tool" }, { 508, L"=== Adding Updates . . ." },
            { 548, L"=== Adding LCU 09974-Windows11.0-KB5132061-x64.msu" }, { 1000, L"The operation completed successfully." }, { 1154, L"The operation completed successfully." }, { 1647, L"Unmounting image" },
            { 1745, L"=== Servicing Index: 2" }, { 1792, L"=== Adding Microsoft Edge . . ." }, { 1792, L"Deployment Image Servicing and Management tool" }, { 1796, L"=== Adding Apps . . ." },
            { 1974, L"Deployment Image Servicing and Management tool" }, { 2087, L"=== Adding Updates . . ." }, { 2127, L"=== Adding LCU 09974-Windows11.0-KB5132061-x64.msu" }, { 2584, L"The operation completed successfully." },
            { 2738, L"The operation completed successfully." }, { 3223, L"Unmounting image" }, { 3320, L"=== Rebuilding install.wim . . ." }, { 3345, L"=== Creating winre.wim . . ." }, { 3385, L"=== Remounting image . . ." },
            { 3430, L"=== Adding LCU for boot.wim . . ." }, { 3670, L"=== Adding winre.wim to install.wim . . ." }, { 3680, L"=== Creating ISO . . ." } };
        {
            DlProg p; ULONGLONG c = 1000000, end = c + 3709000, moved = c; int last = 0, k = 0;
            dlpBegin(&p, 2, 0, 48, 1, c - 320000); dlpStep(&p, 3, c);
            for (ULONGLONG n = c; n < end; n += 10000) {
                for (; k < (int)(sizeof(RUN2) / sizeof(RUN2[0])) && c + RUN2[k].at * 1000ULL <= n; k++) dlpLine(&p, RUN2[k].s, c + RUN2[k].at * 1000ULL);
                double left = dlpLeft(&p, n), real = (end - n) / 1000.0; int pct = dlpPercent(&p, n); wchar_t x[160];
                if (pct < last || fabs(left - real) > 600) ok1 = FALSE;
                if (pct > last) moved = n;
                if (n - moved > 150000) ok1 = FALSE;                         // it never sits still for more than 2.5 minutes
                last = pct; dlpText(&p, n, x, 160);
                if ((n - c) % 60000 == 0) wprintf(L"  both %s  (really %.1f min)\n", x, real / 60);
            }
        }
        wprintf(L"download progress, one edition: %s\n", ok1 ? L"PASS" : L"FAIL");
        BOOL ok = TRUE;
        for (int slow = 1; slow <= 3; slow++) {                     // the same PC, then ones two and three times slower
            DlProg p; ULONGLONG t = 1000000;
            dlpBegin(&p, 2, 0, 22, 1, t);
            dlpStep(&p, 1, t); dlpStep(&p, 2, t + 20000);
            for (int s = 0; s <= 600; s++) dlpBytes(&p, 16000000ULL * s, 9600000000ULL, t + 40000 + s * 1000ULL);
            ULONGLONG c = t + 640000, end = c + 2685000ULL * slow; int last = 0, k = 0;
            dlpStep(&p, 3, c);
            for (ULONGLONG n = c; n < end; n += 60000) {
                for (; k < (int)(sizeof(RUN) / sizeof(RUN[0])) && c + RUN[k].at * 1000ULL * slow <= n; k++) dlpLine(&p, RUN[k].s, c + RUN[k].at * 1000ULL * slow);
                double left = dlpLeft(&p, n), real = (end - n) / 1000.0; int pct = dlpPercent(&p, n);
                if (pct < last || pct > 99) ok = FALSE;
                last = pct;
                if ((n - c) % 600000 == 0) { wchar_t x[160]; dlpText(&p, n, x, 160); wprintf(L"  x%d  %s  (really %.0f min)\n", slow, x, real / 60); }
                if (slow == 1 && fabs(left - real) > 420) ok = FALSE;                                  // within 7 minutes all the way
                if (slow > 1 && n - c > (end - c) / 2 && fabs(left - real) > real * 0.35 + 240) ok = FALSE;   // learned by halfway
            }
            if (!p.sawLcu || !p.inTail) ok = FALSE;
        }
        // a report cut in two by the pipe isn't shown as text, and counts
        repairClear(); dlpBegin(&g_dlp, 1, 0, 22, 1, GetTickCount64());
        wchar_t m1[] = { 'a', REP_PROG_MARK, '1', '2' }, m2[] = { '/', '5', '0', REP_PROG_END, 'b' };
        repairFeed(m1, 4); repairFeed(m2, 5);
        if (g_repN != 1 || wcscmp(g_repLines[0].s, L"ab") || g_dlp.got != 12 || g_dlp.total != 50) ok = FALSE;
        g_dlp.on = FALSE; repairClear();
        wprintf(L"download progress test: %s\n", ok ? L"PASS" : L"FAIL");
    }
#endif
        for (size_t j = 0; j < sizeof(JOBS) / sizeof(JOBS[0]); j++) {
            int n = 0; const RepCmd* list = repJobCmds(JOBS[j], &n);
            if (!list || n < 1) noPrompt = FALSE;
            for (int i = 0; list && i < n; i++)
                if (!_wcsicmp(list[i].tool, L"dism.exe") && !wcsstr(list[i].cmd, L"/NoRestart")) noPrompt = FALSE;
        }
        // the split the user asked for: SFC is only sfc, the cleanup is the two DISM steps, the toggle is one each
        int nS = 0, nW = 0, nOn = 0, nOff = 0;
        const RepCmd* s = repJobCmds(REP_SFC, &nS); const RepCmd* w = repJobCmds(REP_WUC, &nW);
        const RepCmd* on = repJobCmds(REP_RSON, &nOn); const RepCmd* off = repJobCmds(REP_RSOFF, &nOff);
        BOOL splitOk = nS == 1 && !wcscmp(s[0].cmd, L"sfc /scannow")
                    && nW == 2 && wcsstr(w[0].cmd, L"/AnalyzeComponentStore") && wcsstr(w[1].cmd, L"/StartComponentCleanup")
                    && nOn == 1 && wcsstr(on[0].cmd, L"/State:Enabled") && nOff == 1 && wcsstr(off[0].cmd, L"/State:Disabled")
                    && repJobCmds(REP_WIM, &nS) == NULL;
        cmdOk = cmdOk && noPrompt && splitOk;

        // a bare carriage return redraws the line; CRLF and CR CR LF end it, even split across reads
        repairClear();
        repairFeed(L"one\r\ntwo 10%\rtwo 20%\r", 21);
        repairFeed(L"\nthr\r\r\nfour", 11);
        BOOL feedOk = g_repN == 4 && !wcscmp(g_repLines[0].s, L"one") && !wcscmp(g_repLines[1].s, L"two 20%")
                   && !wcscmp(g_repLines[2].s, L"thr") && !wcscmp(g_repLines[3].s, L"four");
        wchar_t mark[2] = { REP_STEP_MARK, 0 };
        repairFeed(mark, 1);
        BOOL stepOk = g_repStep == 1 && g_repN == 4;
        repairClear();

        // DISM's closing message gets its own line; a bar alone or other progress text is left whole
        wchar_t r1[] = L"[==100.0%==] No corruption", r2[] = L"[==100.0%==]", r3[] = L"Verification 5% complete.";
        BOOL barOk = repBarMessage(r1) == r1 + 13 && !repBarMessage(r2) && !repBarMessage(r3);
        // rows end between words or progress parts, lose nothing but spaces, and always fit
        const wchar_t* WR[] = { L"Downloading the Windows files: 98 files, 5 at a time. Getting fresh links for them first.",
            L"[DL:1.9MiB][#899749 165MiB/171MiB(96%)][#feff89 22MiB/27MiB(82%)] [Checksum:#06372c 67MiB/151MiB(44%)]",
            L"      indented   text   with   gaps and trailing spaces                                              ",
            L"URI=http://tlu.dl.delivery.mp.microsoft.com/filestreamingservice/files/55637849-0c2a?P1=1795591931&P2=404",
            L"nospacesatallnospacesatallnospacesatallnospacesatallnospacesatallnospacesatall", L"                                  " };
        BOOL wrapOk = TRUE;
        for (int i = 0; i < 6; i++) for (int room = 10; room < 120; room++) {
            int len = (int)wcslen(WR[i]), off = 0, next = 0, guard = 0;
            while (off < len && guard++ < 300) {
                int n = wrapRow(WR[i], len, off, room, &next);
                if (n < 0 || n > room || next <= off || next > len || next < off + n) { wrapOk = FALSE; break; }
                for (int k = off + n; k < next; k++) if (WR[i][k] != L' ') wrapOk = FALSE;
                if (i == 0 && room >= 12 && next < len && WR[i][next - 1] != L' ') wrapOk = FALSE;
                if (i == 1 && room >= 40 && next < len && WR[i][next] != L'[') wrapOk = FALSE;
                off = next;
            }
            if (off != len) wrapOk = FALSE;
        }

        // a 403 report becomes one plain line; any other error comes through whole
        wchar_t af[MAX_PATH]; swprintf(af, MAX_PATH, L"%swcu_aria_%lu.txt", base, GetCurrentProcessId());
        HANDLE aw = CreateFileW(af, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        const wchar_t* AR[] = { L"[DL:5MiB][#a 1MiB/2MiB(50%)]", L"", L"11/25 07:34:39 [ERROR] CUID#1 - Download aborted. URI=http://x", L"Exception: [A.cc:351] errorCode=22 URI=http://x", L"  -> [H.cc:239] errorCode=22 The response status is not successful. status=403",
            L"[DL:6MiB][#a 1MiB/2MiB(60%)]", L"11/25 07:34:40 [ERROR] CUID#2 - Download aborted. URI=http://y", L"Exception: [A.cc:351] errorCode=22 URI=http://y", L"  -> [H.cc:239] errorCode=22 The response status is not successful. status=403",
            L"11/25 07:34:41 [ERROR] CUID#3 - Download aborted. URI=http://z", L"Exception: [A.cc:351] errorCode=3 URI=http://z", L"  -> [H.cc:239] errorCode=3 Resource not found. status=404", L"other text" };
        g_repRefused = g_repDropped = g_repHeldN = 0;
        for (int i = 0; i < 13; i++) repAriaRow(aw, AR[i]);
        repAriaFlush(aw);
        static wchar_t ab[8192]; DWORD ag = 0;
        SetFilePointer(aw, 0, NULL, FILE_BEGIN); ReadFile(aw, ab, sizeof(ab) - 2, &ag, NULL); CloseHandle(aw);
        repairFeed(ab, (int)(ag / sizeof(wchar_t)));
        BOOL ariaOk = g_repRefused == 2 && g_repN == 6 && wcsstr(g_repLines[0].s, L"expired") && wcsstr(g_repLines[1].s, L"CUID#3")
                   && wcsstr(g_repLines[3].s, L"404") && !wcscmp(g_repLines[4].s, L"other text");
        repairClear();
        if (!wrapOk || !ariaOk) wprintf(L"wrap %d aria %d lines %d\n", wrapOk, ariaOk, g_repN);
        BOOL repPass = pickOk && cmdOk && feedOk && stepOk && barOk && wrapOk && ariaOk;
        wprintf(L"repair test: %s (pick %d cmd %d feed %d step %d bar %d)\n", repPass ? L"PASS" : L"FAIL",
                pickOk, cmdOk, feedOk, stepOk, barOk);
        filePass = filePass && repPass;

        // WCU_TEST_WIM=<file>: what the build check decides for a real WIM. Only reads; DISM isn't run.
        wchar_t wimPath[MAX_PATH];
        if (GetEnvironmentVariableW(L"WCU_TEST_WIM", wimPath, MAX_PATH)) {
            RepLive me; repairReadLive(&me);
            WimEntry* list = (WimEntry*)malloc(sizeof(WimEntry) * REP_WIM_MAX);
            int n = repairReadWim(wimPath, list, REP_WIM_MAX), pick = n > 0 ? repairPickImage(list, n, &me) : -1;
            wprintf(L"live %s %s %s; %d image(s) in the WIM\n", me.full, me.arch, me.edition, n);
            for (int i = 0; i < n; i++)
                wprintf(L"  %c %d: %s | %s | %s %s\n", i == pick ? L'*' : L' ', list[i].index, list[i].name, list[i].edition, list[i].build, list[i].arch);
            if (pick >= 0) { wchar_t rc[MAX_PATH + 128]; repairWimCommand(wimPath, list[pick].index, rc, MAX_PATH + 128); wprintf(L"would run: %s\n", rc); }
            else wprintf(L"refused: no matching image\n");
            free(list);
        }

        // WCU_TEST_TOOL="sfc.exe|sfc /?": run a read-only tool the way a job does, and show the resulting lines
        wchar_t spec[256];
        if (GetEnvironmentVariableW(L"WCU_TEST_TOOL", spec, 256)) {
            wchar_t* bar = wcschr(spec, L'|');
            wchar_t file[MAX_PATH]; swprintf(file, MAX_PATH, L"%swcu_tool_%lu.txt", base, GetCurrentProcessId());
            HANDLE w = CreateFileW(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
            BOOL rs = FALSE;
            *bar = 0;
            DWORD rc = repairRunOne(w, spec, bar + 1, &rs);
            CloseHandle(w);
            HANDLE r = CreateFileW(file, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
            DWORD sz = GetFileSize(r, NULL), got = 0;
            wchar_t* all = (wchar_t*)malloc(sz + 2);
            ReadFile(r, all, sz, &got, NULL); CloseHandle(r);
            repairClear();
            repairFeed(all, (int)(got / sizeof(wchar_t)));
            free(all);
            wprintf(L"tool exit %lu, step %ld, %d lines:\n", rc, g_repStep, g_repN);
            for (int i = 0; i < g_repN; i++) wprintf(L"  [%ls]\n", g_repLines[i].s ? g_repLines[i].s : L"");
            repairClear();
        }
    }

#if WCU_DOWNLOAD
    // ---- Download Windows: reading the API and manifests, and what may reach the elevated job ----
    {
        const char* js = "{\"response\":{\"a\\\"b\":[1,{\"x\":\"}\"}],\"list\":[\"en-us\",\"neutral\"],\"n\":{\"en-us\":\"Caf\\u00e9\"},\"size\":\"42\",\"num\":7,\"none\":null},\"v\":1}";
        const char* resp = jGet(js, "response");
        wchar_t t[64];
        jText(jGet(jGet(resp, "n"), "en-us"), t, 64);
        BOOL jsonOk = !wcscmp(t, L"Caf\u00e9");
        jText(jGet(resp, "size"), t, 64); jsonOk = jsonOk && !wcscmp(t, L"42");
        jText(jGet(resp, "num"), t, 64);  jsonOk = jsonOk && !wcscmp(t, L"7");
        jText(jGet(resp, "none"), t, 64); jsonOk = jsonOk && !t[0];
        jsonOk = jsonOk && jGet(js, "v") && !jGet(resp, "missing");     // a brace inside a string doesn't end the object

        // a new converter version is picked up from the manifest; a tool from elsewhere is not
        const char* mf = "https://uupdump.net/misc/7zr.exe\n  out=7zr.exe\n  checksum=sha-256=72c98287b2e8f85ea7bb87834b6ce1ce7ce7f41a8c97a81b307d4d4bf900922b\n\n"
                         "https://uupdump.net/misc/uup-converter-wimlib-v999.7z\r\n  out=uup-converter-wimlib.7z\r\n  checksum=sha-256=b2c65f5bbc357605aec8cad1739e15bff4b2c5a44daad19eb45d26b46a0ea8dd\r\n";
        DlTool s7, conv, aria;
        BOOL mfOk = dlParseConverterManifest(mf, &s7, &conv) && wcsstr(conv.url, L"v999") && !wcscmp(s7.name, L"7zr.exe");
        const char* bad = "https://evil.example/7zr.exe\n  out=7zr.exe\n  checksum=sha-256=72c98287b2e8f85ea7bb87834b6ce1ce7ce7f41a8c97a81b307d4d4bf900922b\n"
                          "https://uupdump.net/c.7z\n  out=c.7z\n  checksum=sha-256=short\n";
        mfOk = mfOk && !dlParseConverterManifest(bad, &s7, &conv);
        mfOk = mfOk && dlParseAriaManifest("$file = 'x';\n$url = 'https://uupdump.net/misc/aria2c.exe';\n$hash = 'b9cd71b275af11b63c33457b0f43f2f2675937070c563e195f223efd7fa4c74b';", &aria)
                    && !wcscmp(aria.name, L"aria2c.exe")
                    && !dlParseAriaManifest("$url = 'https://evil.example/a.exe';\n$hash = 'b9cd71b275af11b63c33457b0f43f2f2675937070c563e195f223efd7fa4c74b';", &aria);

        // the settings are applied, others are kept, and a setting that has gone is refused rather than left to a prompt
        DlIniSet s1[] = { { "AutoStart", "1" }, { "vAutoEditions", "Enterprise,Education" } };
        wchar_t miss[32];
        char* ini = dlIniApply("[a]\r\nAutoStart    =0\r\nAutoStartX=0\r\nvAutoEditions=\r\nKeep=5\r\n", s1, 2, miss, 32);
        BOOL iniOk = ini && strstr(ini, "AutoStart=1\r\n") && strstr(ini, "AutoStartX=0") && strstr(ini, "vAutoEditions=Enterprise,Education\r\n") && strstr(ini, "Keep=5");
        free(ini);
        DlIniSet s2[] = { { "Gone", "1" } };
        iniOk = iniOk && !dlIniApply("[a]\r\nKeep=5\r\n", s2, 1, miss, 32) && !wcscmp(miss, L"Gone");

        // the elevated job's request: quotes, paths and options can't be smuggled in
        static DlSpec sp;
        BOOL specOk = dlSpecParse(L"6c7bf258-2632|en-us|PROFESSIONAL;CORE|Enterprise,Education|C:\\Users\\Me\\Desktop|Local\\wcu-uup-stop-1-2|", &sp)
                   && !wcscmp(sp.editions, L"PROFESSIONAL;CORE") && !wcscmp(sp.desktop, L"C:\\Users\\Me\\Desktop") && !sp.apps[0];
        specOk = specOk && dlSpecParse(L"abc|en-us|PRO||C:\\D||Microsoft.WindowsStore_8wekyb3d8bbwe,MSTeams_8wekyb3d8bbwe", &sp)
                        && !wcscmp(sp.apps, L"Microsoft.WindowsStore_8wekyb3d8bbwe,MSTeams_8wekyb3d8bbwe")
                        && dlSpecParse(L"abc|en-us|PRO||C:\\D||-", &sp) && !wcscmp(sp.apps, L"-");
        specOk = specOk && !dlSpecParse(L"a&b|en-us|PRO||C:\\D||", &sp) && !dlSpecParse(L"abc|en-us|PRO&calc||C:\\D||", &sp)
                        && !dlSpecParse(L"abc|en-us|PRO|x y|C:\\D||", &sp) && !dlSpecParse(L"abc|en-us|PRO||C:\\D\"x||", &sp)
                        && !dlSpecParse(L"abc|en-us", &sp) && !dlSpecParse(L"abc||PRO||C:\\D||", &sp)
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D|", &sp)                          // the apps part can't be missing
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D||a\r\nb", &sp)                   // nor carry another line into the list
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D||a|b", &sp);
        // the chosen work folder: optional, a full path on a drive letter, nothing that climbs out or carries options
        specOk = specOk && dlSpecParse(L"abc|en-us|PRO||C:\\D||-|D:\\Builds", &sp) && !wcscmp(sp.work, L"D:\\Builds")
                        && dlSpecParse(L"abc|en-us|PRO||C:\\D||-|", &sp) && !sp.work[0]
                        && dlSpecParse(L"abc|en-us|PRO||C:\\D||-", &sp) && !sp.work[0]
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D||-|\\\\srv\\share", &sp)
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D||-|D:\\a\\..\\b", &sp)
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D||-|D:\\a\" /x", &sp)
                        && !dlSpecParse(L"abc|en-us|PRO||C:\\D||-|D:\\a|x", &sp);

        // the apps an edition's metadata lists: only the PreinstalledApps group, once each, with names people know
        const char* cdb = "<Features>"
            "<Feature FeatureID=\"Microsoft.WindowsStore_8wekyb3d8bbwe\" FMID=\"MSDN\" Group=\"PreinstalledApps\" Type=\"Optional\" />"
            "<Feature FeatureID=\"MSTeams_8wekyb3d8bbwe\" Group=\"PreinstalledApps\" Type=\"Optional\" />"
            "<Feature FeatureID=\"Microsoft-Windows-Client-Features\" Group=\"Base\" Type=\"Required\" />"
            "<Feature FeatureID=\"Bad&amp;Id_x\" Group=\"PreinstalledApps\" />"
            "<Feature FeatureID=\"Microsoft.WindowsStore_8wekyb3d8bbwe\" Group=\"PreinstalledApps\" />"
            "<Feature FeatureID=\"Contoso.NewThing_abc\" Group=\"PreinstalledApps\" /></Features>";
        static DlApp ap[8];
        int napps = dlParseCompDbApps(cdb, ap, 0, 8);
        BOOL appsOk = napps == 3 && !wcscmp(ap[0].id, L"Microsoft.WindowsStore_8wekyb3d8bbwe") && !wcscmp(ap[0].name, L"Microsoft Store")
                   && !wcscmp(ap[1].name, L"Microsoft Teams") && !wcscmp(ap[2].name, L"Contoso.NewThing") && ap[0].checked && ap[2].checked;
        wchar_t an[80];
        dlAppName(L"Microsoft.SomethingNew_8wekyb3d8bbwe", an, 80); appsOk = appsOk && !wcscmp(an, L"SomethingNew");
        dlAppName(L"MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy", an, 80); appsOk = appsOk && !wcscmp(an, L"Widgets");
        // always installed and not offered: the Store, Store Purchase, Windows Security, Feedback Hub, anything Xbox
        appsOk = appsOk && ap[0].fixed && !ap[1].fixed && !ap[2].fixed
              && dlAppFixed(L"Microsoft.StorePurchaseApp_8wekyb3d8bbwe") && dlAppFixed(L"Microsoft.SecHealthUI_8wekyb3d8bbwe")
              && dlAppFixed(L"Microsoft.WindowsFeedbackHub_8wekyb3d8bbwe") && dlAppFixed(L"Microsoft.GamingApp_8wekyb3d8bbwe")
              && dlAppFixed(L"Microsoft.XboxGamingOverlay_8wekyb3d8bbwe") && dlAppFixed(L"Microsoft.Xbox.TCUI_8wekyb3d8bbwe")
              && dlAppFixed(L"Microsoft.XboxIdentityProvider_8wekyb3d8bbwe") && dlAppFixed(L"Microsoft.XboxSpeechToTextOverlay_8wekyb3d8bbwe")
              && !dlAppFixed(L"Microsoft.WindowsCamera_8wekyb3d8bbwe") && !dlAppFixed(L"Clipchamp.Clipchamp_yxz26nhyzhsrt");
        // what the job is told: everything (empty), the ticked IDs plus the fixed ones, or nothing at all ("-")
        memcpy(g_dlApps, ap, sizeof(DlApp) * 3); g_dlAppN = 3; g_dlAppsState = 1;
        wchar_t asp[400];
        dlAppsSpec(asp, 400); appsOk = appsOk && !asp[0];
        g_dlApps[1].checked = FALSE;
        dlAppsSpec(asp, 400); appsOk = appsOk && !wcscmp(asp, L"Microsoft.WindowsStore_8wekyb3d8bbwe,Contoso.NewThing_abc");
        g_dlApps[0].checked = g_dlApps[2].checked = FALSE;                                       // the Store is fixed: it stays
        dlAppsSpec(asp, 400); appsOk = appsOk && !wcscmp(asp, L"Microsoft.WindowsStore_8wekyb3d8bbwe");
        g_dlApps[0].fixed = FALSE;
        dlAppsSpec(asp, 400); appsOk = appsOk && !wcscmp(asp, L"-");
        g_dlAppsState = 3;
        dlAppsSpec(asp, 400); appsOk = appsOk && !asp[0];                                       // list unreadable: all apps, as before
        g_dlAppN = 0; g_dlAppsState = 0;

        // file names from the API become paths
        BOOL nameOk = dlFileNameOk(L"professional_en-us.esd") && !dlFileNameOk(L"..\\x.esd") && !dlFileNameOk(L"a/b.esd")
                   && !dlFileNameOk(L"C:x.esd") && !dlFileNameOk(L"a\nb") && !dlFileNameOk(L"");
        BOOL buildOk = dlValidBuildText(L"26300.9818") && dlValidBuildText(L"26300") && !dlValidBuildText(L"26300.") && !dlValidBuildText(L"1.2.3")
                    && !dlValidBuildText(L"abc") && !dlValidBuildText(L"26300&x=1") && !dlValidBuildText(L"")
                    && dlBuildMatches(L"26300.9818", L"26300") && dlBuildMatches(L"26300.9818", L"26300.9818")
                    && !dlBuildMatches(L"26300.9725", L"26300.9818") && !dlBuildMatches(L"263000.1", L"26300");
        // the ISO's name, from real API values: every channel, a build with no version in its title, and missing facts
        wchar_t isoN[MAX_PATH];
        DlFacts ff; memset(&ff, 0, sizeof(ff));
        const wchar_t* conv1 = L"26300.9818.261025-0959.26H2_GE_RELEASE_SVC_PROD3_CLIENTMULTI_X64FRE_EN-US.ISO";
        wcscpy(ff.title, L"Windows 11, version 26H2 (26300.9818)"); wcscpy(ff.build, L"26300.9818"); wcscpy(ff.ring, L"RP"); ff.created = 1793206836;
        wcscpy(ff.arch, L"arm64");
        dlIsoName(&ff, conv1, L"", L"", isoN, MAX_PATH);
        BOOL isoOk = !wcscmp(isoN, L"26300.9818_26H2_arm64_Release Preview_28_Oct_26.iso");
        wcscpy(ff.arch, L"amd64");
        dlIsoName(&ff, conv1, L"", L"", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, L"26300.9818_26H2_x64_Release Preview_28_Oct_26.iso");
        // the editions after the version, in the user's fixed order whatever the tick order, Pro only when ticked
        dlIsoName(&ff, conv1, L"PROFESSIONAL;CORE", L"ProfessionalWorkstation,Education,Enterprise,ProfessionalEducation", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, L"26300.9818_26H2_home.pro.edu.ent.pro-edu.pro-work_x64_Release Preview_28_Oct_26.iso");
        dlIsoName(&ff, conv1, L"PROFESSIONAL", L"", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, L"26300.9818_26H2_pro_x64_Release Preview_28_Oct_26.iso");
        dlIsoName(&ff, conv1, L"", L"Enterprise", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, L"26300.9818_26H2_ent_x64_Release Preview_28_Oct_26.iso");
        dlIsoName(&ff, conv1, L"CORE;PPIPRO", L"", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, L"26300.9818_26H2_home.ppipro_x64_Release Preview_28_Oct_26.iso");
        wcscpy(ff.title, L"Windows 11 Insider Preview 26100.1 (ge_release)"); wcscpy(ff.build, L"26100.1"); wcscpy(ff.ring, L"CANARY"); ff.created = 1712163614;
        dlIsoName(&ff, L"26100.1.240331-1435.GE_RELEASE_CLIENTMULTI_X64FRE_EN-US.ISO", L"", L"", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, L"26100.1_GE_x64_Canary_03_Apr_24.iso");
        wcscpy(ff.ring, L"WIS"); dlIsoName(&ff, L"x", L"", L"", isoN, MAX_PATH);  isoOk = isoOk && wcsstr(isoN, L"_x64_Beta_");
        wcscpy(ff.ring, L"WIF"); dlIsoName(&ff, L"x", L"", L"", isoN, MAX_PATH);  isoOk = isoOk && wcsstr(isoN, L"_x64_Dev_");
        wcscpy(ff.ring, L"RETAIL"); dlIsoName(&ff, L"x", L"", L"", isoN, MAX_PATH); isoOk = isoOk && wcsstr(isoN, L"_x64_Retail_");
        wcscpy(ff.ring, L"NEW/RING"); dlIsoName(&ff, L"x", L"", L"", isoN, MAX_PATH); isoOk = isoOk && wcsstr(isoN, L"_x64_NEW-RING_");   // a new ring still names safely
        memset(&ff, 0, sizeof(ff));
        dlIsoName(&ff, conv1, L"", L"", isoN, MAX_PATH);
        isoOk = isoOk && !wcscmp(isoN, conv1);                                                                               // no facts: keep the converter's name

        // N editions go, whatever else is listed; a code ending in N with no base edition beside it stays
        DlEdition eds[5]; memset(eds, 0, sizeof(eds));
        wcscpy(eds[0].code, L"COREN"); wcscpy(eds[1].code, L"CORE"); wcscpy(eds[2].code, L"PROFESSIONAL"); wcscpy(eds[3].code, L"PROFESSIONALN"); wcscpy(eds[4].code, L"SERVERN");
        int kept = dlDropNEditions(eds, 5);
        BOOL nOk = kept == 3 && !wcscmp(eds[0].code, L"CORE") && !wcscmp(eds[1].code, L"PROFESSIONAL") && !wcscmp(eds[2].code, L"SERVERN");

        // the converter's temporary folders move into the work folder; a script without those lines isn't run
        BOOL patchOk = FALSE;
        {
            wchar_t pdir[MAX_PATH], pscr[MAX_PATH + 32];
            swprintf(pdir, MAX_PATH, L"%swcu_patch_%lu", base, GetCurrentProcessId());
            CreateDirectoryW(pdir, NULL);
            swprintf(pscr, MAX_PATH + 32, L"%s\\convert-UUP.cmd", pdir);
            const char* scr = "@echo off\r\nset _drv=%~d0\r\nset \"_cabdir=%_drv%\\W10UIuup\"\r\necho x\r\n"
                              "set \"_mount=%_drv%\\MountUUP\"\r\nif 1==1 (\r\nset \"_mount=%SystemDrive%\\MountUUP\"\r\n)\r\n";
            HANDLE pf = CreateFileW(pscr, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            DWORD pw; WriteFile(pf, scr, (DWORD)strlen(scr), &pw, NULL); CloseHandle(pf);
            wchar_t vscr[MAX_PATH + 48]; swprintf(vscr, MAX_PATH + 48, L"%s\\create_virtual_editions.cmd", pdir);
            const char* vs = "set \"_mount=%_vdrv%\\MountUUP\"\r\nif x==y (\r\nset \"_mount=%SystemDrive%\\MountUUP\"\r\n)\r\n";
            pf = CreateFileW(vscr, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            WriteFile(pf, vs, (DWORD)strlen(vs), &pw, NULL); CloseHandle(pf);
            BOOL r1 = dlPatchConverterPaths(pdir, L"C:\\Users\\Me\\DOCUME~1\\WCU_UUP", FALSE, NULL);
            char* got = dlReadSmallFile(pscr, 1 << 20);
            char* vgot = dlReadSmallFile(vscr, 1 << 20);
            patchOk = r1 && got && !strcmp(got,
                "@echo off\r\nset _drv=%~d0\r\nset \"_cabdir=C:\\Users\\Me\\DOCUME~1\\WCU_UUP\\W10UIuup\"\r\necho x\r\n"
                "set \"_mount=C:\\Users\\Me\\DOCUME~1\\WCU_UUP\\MountUUP\"\r\nif 1==1 (\r\nset \"_mount=C:\\Users\\Me\\DOCUME~1\\WCU_UUP\\MountUUP\"\r\n)\r\n")
                && vgot && !strcmp(vgot, "set \"_mount=C:\\Users\\Me\\DOCUME~1\\WCU_UUP\\MountUUP\"\r\nif x==y (\r\n"
                                        "set \"_mount=C:\\Users\\Me\\DOCUME~1\\WCU_UUP\\MountUUP\"\r\n)\r\n");
            free(got); free(vgot);
            const char* old = "@echo off\r\nset \"_mount=%_drv%\\MountUUP\"\r\n";              // no _cabdir line: refused, file untouched
            pf = CreateFileW(pscr, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            WriteFile(pf, old, (DWORD)strlen(old), &pw, NULL); CloseHandle(pf);
            BOOL r2 = dlPatchConverterPaths(pdir, L"C:\\W", FALSE, NULL);
            got = dlReadSmallFile(pscr, 1 << 20);
            patchOk = patchOk && !r2 && got && !strcmp(got, old);
            free(got);
            dlRemoveTree(pdir);
        }

        // architecture labels: UUP dump's names to what's shown, anything new passed through as it comes
        BOOL archOk = !wcscmp(dlArchLabel(L"amd64"), L"x64") && !wcscmp(dlArchLabel(L"ARM64"), L"arm64") && !wcscmp(dlArchLabel(L"x86"), L"x86");

        // the work folder: plain paths only for the converter
        BOOL pathOk = dlPathSimple(L"C:\\Users\\Me\\DOCUME~1\\WCU_UUP") && !dlPathSimple(L"C:\\Users\\John Smith\\Documents")
                   && !dlPathSimple(L"C:\\Users\\A&B\\x") && !dlPathSimple(L"C:\\Users\\J\x00F6rg");
        // as administrator: the real folder in Documents is made admin-only, held, and gone afterwards with its contents
        if (IsUserAnAdmin()) {
            wchar_t docs[MAX_PATH], made[MAX_PATH], sub[MAX_PATH], fpath[MAX_PATH + 16];
            SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs);
            swprintf(made, MAX_PATH, L"%s\\WCU_UUP", docs);
            BOOL rootOk = dlRootEnsure(NULL) == 0 && dlPathSimple(g_dlRoot) && dlOwnedByAdmins(g_dlRootH);
            wprintf(L"work folder test: %s\n", g_dlRoot);
            dlRootPath(sub, L"tools");
            rootOk = rootOk && CreateDirectoryW(sub, NULL);
            swprintf(fpath, MAX_PATH + 16, L"%s\\x.bin", sub); mkfile(fpath, 100);
            wchar_t moved[MAX_PATH]; swprintf(moved, MAX_PATH, L"%s\\WCU_UUP_moved", docs);
            // in use while held, so a second build or the clean-up leaves it alone
            rootOk = rootOk && dlRootInUse(made);
            wprintf(L"work folder seen as in use: %d\n", dlRootInUse(made));
            // held: it can't be renamed while the job runs
            BOOL movedIt = MoveFileW(made, moved);
            DWORD moveErr = GetLastError();
            if (movedIt) { MoveFileW(moved, made); rootOk = FALSE; }
            else rootOk = rootOk && moveErr == ERROR_SHARING_VIOLATION;
            wprintf(L"work folder rename refused: %d (error %lu)\n", !movedIt, moveErr);
            PSECURITY_DESCRIPTOR rsd = NULL; PACL dacl = NULL;
            if (GetNamedSecurityInfoW(made, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &rsd) == ERROR_SUCCESS) {
                rootOk = rootOk && dacl && dacl->AceCount == 2;          // administrators and SYSTEM only
                LocalFree(rsd);
            } else rootOk = FALSE;
            dlRootRemove(NULL);
            rootOk = rootOk && GetFileAttributesW(made) == INVALID_FILE_ATTRIBUTES && !g_dlRoot[0] && !dlRootInUse(made);
            pathOk = pathOk && rootOk;
        }
        // nor can a folder above it be renamed, which is what stands in for Documents here
        {
            wchar_t pp[MAX_PATH], pc[MAX_PATH], pm[MAX_PATH];
            swprintf(pp, MAX_PATH, L"%swcu_parent_%lu", base, GetCurrentProcessId());
            swprintf(pc, MAX_PATH, L"%s\\child", pp);
            swprintf(pm, MAX_PATH, L"%s_moved", pp);
            CreateDirectoryW(pp, NULL); CreateDirectoryW(pc, NULL);
            HANDLE hc = CreateFileW(pc, FILE_LIST_DIRECTORY | READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            BOOL parentMoved = MoveFileW(pp, pm);
            wprintf(L"parent folder rename refused: %d\n", !parentMoved);
            if (hc != INVALID_HANDLE_VALUE) CloseHandle(hc);
            if (parentMoved) MoveFileW(pm, pp);
            pathOk = pathOk && hc != INVALID_HANDLE_VALUE && !parentMoved;
            RemoveDirectoryW(pc); RemoveDirectoryW(pp);
        }

        // the log: the panel's lines, as UTF-8 with a BOM, beside the ISO under the same name
        wchar_t lIso[MAX_PATH], lTxt[MAX_PATH];
        swprintf(lIso, MAX_PATH, L"%swcu_log_%lu_x64_28_Oct_26.iso", base, GetCurrentProcessId());
        swprintf(lTxt, MAX_PATH, L"%swcu_log_%lu_x64_28_Oct_26.txt", base, GetCurrentProcessId());
        repairClear();
        const wchar_t* lIn = L"Converting\r\nError: x \x2713\r\nDone";
        repairFeed(lIn, (int)wcslen(lIn));
        const char* lWant = "\xEF\xBB\xBF" "Converting\r\nError: x \xE2\x9C\x93\r\nDone\r\n";
        BOOL logOk = dlSaveLog(lIso) && !dlSaveLog(L"C:\\no.wim");
        char lBuf[64] = { 0 }; DWORD lGot = 0;
        HANDLE lh = CreateFileW(lTxt, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (lh != INVALID_HANDLE_VALUE) { ReadFile(lh, lBuf, sizeof(lBuf) - 1, &lGot, NULL); CloseHandle(lh); }
        logOk = logOk && lGot == strlen(lWant) && !strcmp(lBuf, lWant);
        DeleteFileW(lTxt);
        repairClear();
        BOOL dlPass = jsonOk && mfOk && iniOk && specOk && nameOk && buildOk && isoOk && nOk && pathOk && logOk && archOk && appsOk && patchOk;
        wprintf(L"download test: %s (json %d manifest %d ini %d spec %d name %d build %d iso %d n %d path %d log %d arch %d apps %d patch %d)\n", dlPass ? L"PASS" : L"FAIL", jsonOk, mfOk, iniOk, specOk, nameOk, buildOk, isoOk, nOk, pathOk, logOk, archOk, appsOk, patchOk);
        filePass = filePass && dlPass;
    }
#endif

    // ---- "Old restore points" must always spare the newest; "All" must take every point ----
    {
        DWORD s[4] = { 7, 12, 3, 9 }, o[4], one[1] = { 6 };
        int keep = restorePickRemovals(s, 4, TRUE, o);
        BOOL spared = keep == 3;
        for (int k = 0; k < keep; k++) if (o[k] == 12) spared = FALSE;
        BOOL restorePass = spared && restorePickRemovals(s, 4, FALSE, o) == 4
                        && restorePickRemovals(one, 1, TRUE, o) == 0
                        && restorePickRemovals(one, 0, TRUE, o) == 0;
        wprintf(L"restore point selection test: %s\n", restorePass ? L"PASS" : L"FAIL");
        filePass = filePass && restorePass;
        DWORD real[256]; int rn = restoreList(real, 256);   // read-only
        wprintf(L"restore points listed = %d, shadow storage used = %llu, off = %d\n", rn, shadowStorageUsed(), restoreIsOff());
    }

    // ---- image metadata: reading, editing, escaping ----
    {
        const wchar_t* x = L"<IMAGE INDEX=\"1\"><NAME>A &amp; B</NAME><DESCRIPTION>old</DESCRIPTION>"
                           L"<DISPLAYNAME>A</DISPLAYNAME><WINDOWS><ARCH>9</ARCH><EDITIONID>Pro</EDITIONID></WINDOWS></IMAGE>";
        WimEntry e; memset(&e, 0, sizeof(e));
        wimParseInfo(x, &e);
        BOOL ok = !wcscmp(e.name, L"A & B") && !wcscmp(e.desc, L"old") && !wcscmp(e.edition, L"Pro") && !wcscmp(e.arch, L"x64");
        wchar_t* y = xmlSet(x, L"NAME", L"C<D", TRUE);
        ok = ok && y && wcsstr(y, L"<NAME>C&lt;D</NAME>") && wcsstr(y, L"<EDITIONID>Pro</EDITIONID>");
        wchar_t* z = y ? xmlSet(y, L"DISPLAYDESCRIPTION", L"zz", FALSE) : NULL;   // absent, optional: unchanged
        ok = ok && z && !wcsstr(z, L"DISPLAYDESCRIPTION");
        wchar_t* w = z ? xmlSet(z, L"NEWTAG", L"v", TRUE) : NULL;                 // absent, required: added
        ok = ok && w && wcsstr(w, L"<NEWTAG>v</NEWTAG></IMAGE>");
        free(y); free(z); free(w);
        wprintf(L"image metadata test: %s\n", ok ? L"PASS" : L"FAIL");
        filePass = filePass && ok;
    }

    // ---- pasted registry keys: every form people copy must reach the same canonical path ----
    {
        static const struct { const wchar_t* in; const wchar_t* want; const wchar_t* sub; } cases[] = {
            { L"Computer\\HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft", L"SOFTWARE\\Microsoft" },
            { L"computer\\hklm\\SOFTWARE", L"HKEY_LOCAL_MACHINE\\SOFTWARE", L"SOFTWARE" },
            { L"My Computer\\HKCU\\Software", L"HKEY_CURRENT_USER\\Software", L"Software" },
            { L"HKLM\\SOFTWARE\\Microsoft", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft", L"SOFTWARE\\Microsoft" },
            { L"hkcu\\Software", L"HKEY_CURRENT_USER\\Software", L"Software" },
            { L"HKCR\\.txt", L"HKEY_CLASSES_ROOT\\.txt", L".txt" },
            { L"HKU\\.DEFAULT", L"HKEY_USERS\\.DEFAULT", L".DEFAULT" },
            { L"HKCC\\System", L"HKEY_CURRENT_CONFIG\\System", L"System" },
            { L"HKLM:\\SOFTWARE\\Foo", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Foo", L"SOFTWARE\\Foo" },
            { L"Registry::HKEY_USERS\\S-1-5-18", L"HKEY_USERS\\S-1-5-18", L"S-1-5-18" },
            { L"Microsoft.PowerShell.Core\\Registry::HKLM\\X", L"HKEY_LOCAL_MACHINE\\X", L"X" },
            { L"[HKEY_CURRENT_USER\\Software\\App]", L"HKEY_CURRENT_USER\\Software\\App", L"Software\\App" },
            { L"[-HKEY_CURRENT_USER\\Software\\App]", L"HKEY_CURRENT_USER\\Software\\App", L"Software\\App" },
            { L"  \"HKLM\\SOFTWARE\\\"  ", L"HKEY_LOCAL_MACHINE\\SOFTWARE", L"SOFTWARE" },
            { L"HKLM\\\\SOFTWARE\\\\Microsoft\\\\", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft", L"SOFTWARE\\Microsoft" },
            { L"HKLM/SOFTWARE/Microsoft", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft", L"SOFTWARE\\Microsoft" },
            { L"HKLM\\SOFTWARE\\Name/With/Slash", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Name/With/Slash", L"SOFTWARE\\Name/With/Slash" },
            { L"HKLM", L"HKEY_LOCAL_MACHINE", L"" },
            { L"Computer\\HKEY_CURRENT_USER", L"HKEY_CURRENT_USER", L"" },
        };
        static const wchar_t* bad[] = { L"", L"Computer", L"Computer\\", L"SOFTWARE\\Microsoft", L"HKLMX\\SOFTWARE", L"C:\\Windows", L"hello" };
        BOOL ok = TRUE;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            wchar_t o[1024]; HKEY root; const wchar_t* sub = NULL;
            if (!regNormalize(cases[i].in, o, 1024, &root, &sub) || wcscmp(o, cases[i].want) || wcscmp(sub, cases[i].sub)) {
                ok = FALSE; wprintf(L"  WRONG: '%s' -> '%s'\n", cases[i].in, o);
            }
        }
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            wchar_t o[1024]; HKEY root; const wchar_t* sub;
            if (regNormalize(bad[i], o, 1024, &root, &sub)) { ok = FALSE; wprintf(L"  ACCEPTED: '%s'\n", bad[i]); }
        }
        ok = ok && regKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE") && !regKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WCU_No_Such_Key_12345") && regKeyExists(HKEY_CURRENT_USER, L"");
        wprintf(L"registry path test: %s\n", ok ? L"PASS" : L"FAIL");
        filePass = filePass && ok;
    }

    // ---- USB safety: nothing but a USB disk without Windows may be erased ----
    {
        wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
        int sysDisk = diskNumberOfLetter(win[0]);
        BOOL ok = sysDisk >= 0 && !usbDiskSafe(sysDisk, NULL) && !usbDiskSafe(99, NULL);
        wchar_t body[512]; usbDiskpartScript(7, body, 512);
        ok = ok && wcsstr(body, L"select disk 7\r\n") == body && wcsstr(body, L"fs=fat32");
        usbRefresh();
        for (int i = 0; i < g_usbCount; i++) if (g_usb[i].number == sysDisk) ok = FALSE;
        wprintf(L"usb safety test: %s (system disk %d, %d usb disks listed)\n", ok ? L"PASS" : L"FAIL", sysDisk, g_usbCount);
        filePass = filePass && ok;
    }

    // ---- hashes of "abc", checked against the published test vectors ----
    {
        wchar_t hf[MAX_PATH]; swprintf(hf, MAX_PATH, L"%swcu_hash_%lu.txt", base, GetCurrentProcessId());
        HANDLE f = CreateFileW(hf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wr; WriteFile(f, "abc", 3, &wr, NULL); CloseHandle(f);
        wchar_t o[4][132];
        DWORD r = hashFile(hf, o);
        BOOL ok = !r && !wcscmp(o[0], L"900150983CD24FB0D6963F7D28E17F72")
                     && !wcscmp(o[1], L"A9993E364706816ABA3E25717850C26C9CD0D89D")
                     && !wcscmp(o[2], L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD")
                     && !wcscmp(o[3], L"352441C2");
        DeleteFileW(hf);
        wprintf(L"hash test: %s\n", ok ? L"PASS" : L"FAIL");
        filePass = filePass && ok;
    }
    // Optional integration runs, each named by an environment variable (see the test scripts)
    {
        wchar_t a[MAX_PATH], b[MAX_PATH], c[MAX_PATH], d[MAX_PATH];
        static wchar_t many[2048];
        if (GetEnvironmentVariableW(L"WCU_MEDIA_WIMS", many, 2048) && GetEnvironmentVariableW(L"WCU_MEDIA_KIND", a, MAX_PATH)
            && GetEnvironmentVariableW(L"WCU_MEDIA_OUT", b, MAX_PATH)) {
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
            memset(g_wim, 0, sizeof(g_wim)); g_wimCount = 0; g_wimFirst[0] = 0;
            DWORD r = 0;
            wchar_t* tokCtx = NULL;
            for (wchar_t* tok = wcstok(many, L";", &tokCtx); tok && !r; tok = wcstok(NULL, L";", &tokCtx)) r = wimReadFile(tok);
            wprintf(L"loaded: 0x%08lX images=%ld\n", r, (long)g_wimCount);
            for (LONG i = 0; i < g_wimCount; i++) wprintf(L"  %d | %s | build %s\n", g_wim[i].index, g_wim[i].name, g_wim[i].build);
            if (GetEnvironmentVariableW(L"WCU_MEDIA_TICK", c, MAX_PATH)) wcscpy(g_mediaSelBuild, c);
            wimRefreshMedia();
            for (int m = 0; m < g_mediaCount; m++) wprintf(L"  choice %s%s  %s\n", g_mediaOpts[m].build, m == g_mediaSel ? L" [ticked]" : L"", g_mediaOpts[m].root);
            if (!r && g_mediaSel < g_mediaCount) {
                if (!wcscmp(a, L"folder")) r = mediaBuild(g_mediaOpts[g_mediaSel].root, b, 0, 100);
                else if (!wcscmp(a, L"iso")) {
                    swprintf(c, MAX_PATH, L"%s.media", b);
                    r = mediaBuild(g_mediaOpts[g_mediaSel].root, c, 0, 70);
                    if (!r) r = isoBuild(c, L"WINDOWS", b, 70, 30);
                    removeTreeOwn(c);
                } else if (!wcscmp(a, L"esd")) {
                    swprintf(c, MAX_PATH, L"%s.build.wim", b);
                    r = wimSaveAsRange(c, 0, 30);
                    if (!r) { r = runEsdExport(c, b, (int)g_wimCount); DeleteFileW(c); }
                }
            }
            wprintf(L"media %s: 0x%08lX %s\n", a, r, g_dispErr);
            if (r) filePass = FALSE;
        }
        if (GetEnvironmentVariableW(L"WCU_F2I_SRC", a, MAX_PATH) && GetEnvironmentVariableW(L"WCU_F2I_OUT", b, MAX_PATH)) {
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
            DWORD r = isoBuild(a, L"WCUTEST", b, 0, 100);
            wprintf(L"isoBuild: 0x%08lX %s\n", r, g_dispErr);
            if (r) filePass = FALSE;
        }
        if (GetEnvironmentVariableW(L"WCU_I2F_ISO", a, MAX_PATH) && GetEnvironmentVariableW(L"WCU_I2F_DST", b, MAX_PATH)) {
            DWORD r = isoExtract(a, b, 0, 100);
            wprintf(L"isoExtract: 0x%08lX\n", r);
            if (r) filePass = FALSE;
        }
        if (GetEnvironmentVariableW(L"WCU_WIM_FILE", a, MAX_PATH) && GetEnvironmentVariableW(L"WCU_WIM_OUT", b, MAX_PATH)) {
            DWORD r = wimReadFile(a);
            wprintf(L"wimRead: 0x%08lX images=%ld\n", r, (long)g_wimCount);
            for (LONG i = 0; i < g_wimCount; i++)
                wprintf(L"  %d | %s | %s | %s | %s\n", g_wim[i].index, g_wim[i].name, g_wim[i].desc, g_wim[i].edition, g_wim[i].arch);
            if (!r && g_wimCount >= 2) {
                // rename the first image and drop the second: export, then in place on the copy
                wcscpy(g_wim[0].name, L"Renamed & tested"); wcscpy(g_wim[0].desc, L"edited"); g_wim[0].dirty = TRUE;
                r = wimSaveAs(b);
                wprintf(L"wimSaveAs: 0x%08lX\n", r);
                if (!r) {
                    g_wimCount = 0; g_wimDelCount = 0;
                    r = wimReadFile(b);
                    wprintf(L"reread: 0x%08lX images=%ld first='%s'\n", r, (long)g_wimCount, g_wim[0].name);
                    if (wcscmp(g_wim[0].name, L"Renamed & tested")) r = 1;
                }
                if (!r) {
                    wcscpy(g_wimDel[0].path, b); g_wimDel[0].index = 2; g_wimDelCount = 1;
                    wcscpy(g_wim[0].name, L"Second rename"); g_wim[0].dirty = TRUE;
                    r = wimSaveInPlace();
                    wprintf(L"wimSaveInPlace: 0x%08lX\n", r);
                    LONG before = g_wimCount;
                    g_wimCount = 0; g_wimDelCount = 0;
                    r = r ? r : wimReadFile(b);
                    wprintf(L"reread: images=%ld (was %ld) first='%s'\n", (long)g_wimCount, (long)before, g_wim[0].name);
                    if (g_wimCount != before - 1 || wcscmp(g_wim[0].name, L"Second rename")) r = 1;
                }
            }
            wprintf(L"wim test: %s\n", r ? L"FAIL" : L"PASS");
            if (r) filePass = FALSE;
        }
        // ESD to ISO, the whole job as the elevated runner does it (run this test elevated): WCU_ESDISO_SPEC is the job's
        // spec (esd|picks|folder|naming|channel|), and the output goes to WCU_ESDISO_LOG as UTF-16
        static wchar_t eiSpecT[2048];
        if (GetEnvironmentVariableW(L"WCU_ESDISO_SPEC", eiSpecT, 2048) && GetEnvironmentVariableW(L"WCU_ESDISO_LOG", b, MAX_PATH)) {
            HANDLE lg = CreateFileW(b, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
            DWORD r = esdIsoRunJob(eiSpecT, lg);
            CloseHandle(lg);
            wprintf(L"esdIsoRunJob: %lu\n", r);
            if (r) filePass = FALSE;
            (void)c; (void)d;
        }
    }

    // ---- ESD to ISO naming ----
    {
        wchar_t nm[MAX_PATH];
        BOOL n1 = eiOurName(L"26300.9974.261109-1159.26h2_ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_en-us", L"x64", L"Release Preview", nm, MAX_PATH)
                  && !wcscmp(nm, L"26300.9974_26H2_x64_Release Preview_09_Nov_26.iso");
        BOOL n2 = eiOurName(L"26100.4349.250607-1500.ge_release_svc_refresh_CLIENTCONSUMER_RET_x64FRE_en-us", L"arm64", L"", nm, MAX_PATH)
                  && !wcscmp(nm, L"26100.4349_24H2_arm64_07_Jun_25.iso");
        BOOL n3 = !eiOurName(L"install", L"x64", L"Retail", nm, MAX_PATH) && !eiOurName(L"26100.1.2506", L"x64", L"", nm, MAX_PATH);
        wprintf(L"esd iso name test: %s (%d %d %d)\n", n1 && n2 && n3 ? L"PASS" : L"FAIL", n1, n2, n3);
        if (!(n1 && n2 && n3)) filePass = FALSE;
    }

    // ---- registry purge test on a throwaway HKCU sandbox ----
    const wchar_t* ROOT = L"Software\\WCU_RegTest\\Profiles";
    HKEY proot;
    RegCreateKeyExW(HKEY_CURRENT_USER, ROOT, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &proot, NULL);
    struct { const wchar_t* guid; const wchar_t* pn; BOOL del; } fk[] = {
        { L"{A}", L"Network",       FALSE },
        { L"{B}", L"Network 1",     TRUE  },
        { L"{C}", L"Network 2",     TRUE  },
        { L"{D}", L"Network 100",   TRUE  },
        { L"{E}", L"Network 101",   FALSE },
        { L"{F}", L"Tailscale",     FALSE },
        { L"{G}", L"net-merge",     FALSE },
        { L"{H}", L"HomeWiFi 5G",   FALSE },
        { L"{I}", L"My Network 2",  FALSE },
        { L"{J}", L"network 7",     TRUE  },   // case-insensitive
    };
    int nfk = sizeof(fk)/sizeof(fk[0]), expectDel = 0;
    for (int i = 0; i < nfk; i++) {
        if (fk[i].del) expectDel++;
        HKEY sk;
        RegCreateKeyExW(proot, fk[i].guid, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sk, NULL);
        RegSetValueExW(sk, L"ProfileName", 0, REG_SZ, (const BYTE*)fk[i].pn,
                       (DWORD)((wcslen(fk[i].pn)+1)*sizeof(wchar_t)));
        HKEY child;   // a nested subkey, to prove the whole tree is removed
        RegCreateKeyExW(sk, L"Nested", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &child, NULL);
        RegCloseKey(child); RegCloseKey(sk);
    }
    RegCloseKey(proot);

    int rdel = 0; BOOL rden = FALSE; wchar_t rrep[2048];
    int rmatched = purgeNetworks(HKEY_CURRENT_USER, ROOT, TRUE, &rdel, &rden, rrep, 2048);
    wprintf(L"purge matched=%d deleted=%d (expect %d)\n", rmatched, rdel, expectDel);

    BOOL regPass = (rmatched == expectDel && rdel == expectDel && !rden);
    for (int i = 0; i < nfk; i++) {
        wchar_t sub[128]; swprintf(sub, 128, L"%s\\%s", ROOT, fk[i].guid);
        HKEY sk; BOOL exists = (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &sk) == ERROR_SUCCESS);
        if (exists) RegCloseKey(sk);
        BOOL ok = fk[i].del ? !exists : exists;      // deleted ones gone, kept ones present
        if (!ok) { regPass = FALSE; wprintf(L"  WRONG: %s '%s' exists=%d\n", fk[i].guid, fk[i].pn, exists); }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\WCU_RegTest");   // remove sandbox
    wprintf(L"registry test: %s\n", regPass ? L"PASS" : L"FAIL");

    // ---- Terminal Settings Restore, with %LocalAppData% pointed at a throwaway folder ----
    {
        wchar_t sb2[MAX_PATH], saved[MAX_PATH], tp[MAX_PATH], tmpf[MAX_PATH], dir[MAX_PATH];
        swprintf(sb2, MAX_PATH, L"%swcu_term_%lu", base, GetCurrentProcessId());
        swprintf(dir, MAX_PATH, L"%s\\" TERMINAL_STATE_DIR, sb2);
        swprintf(tp, MAX_PATH, L"%s\\settings.json", dir);
        swprintf(tmpf, MAX_PATH, L"%s.wcu-tmp", tp);
        GetEnvironmentVariableW(L"LOCALAPPDATA", saved, MAX_PATH);
        CreateDirectoryW(sb2, NULL);
        SetEnvironmentVariableW(L"LOCALAPPDATA", sb2);

        DWORD n = 0;
        const BYTE* data = embeddedData(RES_TERMINAL_SETTINGS, &n);
        BOOL embedded = data && n > 0;
        BOOL created = restoreTerminalSettings() == 0 && fileMatches(tp, data, n);   // no folders yet

        mkfile(tp, n + 1000);                                                        // longer and different
        BOOL detects = !fileMatches(tp, data, n);
        SetFileAttributesW(tp, FILE_ATTRIBUTE_READONLY);
        BOOL replaced = restoreTerminalSettings() == 0 && fileMatches(tp, data, n);
        mkfile(tp, 10);                                                              // shorter
        detects = detects && !fileMatches(tp, data, n);
        replaced = replaced && restoreTerminalSettings() == 0 && fileMatches(tp, data, n);
        BOOL noTemp = GetFileAttributesW(tmpf) == INVALID_FILE_ATTRIBUTES;

        DeleteFileW(tp);                                                             // can't be replaced: reports an error
        CreateDirectoryW(tp, NULL);
        BOOL fails = restoreTerminalSettings() != 0 && GetFileAttributesW(tmpf) == INVALID_FILE_ATTRIBUTES;

        SetEnvironmentVariableW(L"LOCALAPPDATA", saved);
        wchar_t* lp2 = longPath(sb2);
        deleteContents(lp2);
        free(lp2);
        RemoveDirectoryW(sb2);

        BOOL ok = embedded && created && replaced && detects && noTemp && fails;
        wprintf(L"terminal settings test: %s (embedded %d created %d replaced %d detects %d noTemp %d fails %d)\n",
                ok ? L"PASS" : L"FAIL", embedded, created, replaced, detects, noTemp, fails);
        filePass = filePass && ok;
    }

    // ---- Enable / Disable: the jobs run what's built in, unchanged, and TrustedInstaller really is TrustedInstaller ----
    {
        static const struct { int res; const wchar_t* file; } EMB[] = {     // run from the Source folder
            { 10, L"tweaks\\Remove_Explore_background_context-menu_for_all_users.reg" },
            { 11, L"tweaks\\Add_Explore_background_context-menu_for_all_users.reg" },
            { 12, L"tweaks\\Remove_Gallery_from_navigation_pane_in_File_Explorer_for_current_user.reg" },
            { 13, L"tweaks\\Add_Gallery_to_navigation_pane_in_File_Explorer_for_current_user.reg" },
            { 14, L"tweaks\\Disable_Virtualized_Based_Security.reg" },
            { 15, L"tweaks\\Disable_VBS_extras.reg" },
            { 16, L"tweaks\\Enable_Virtualized_Based_Security.reg" },
            { 17, L"tweaks\\BlankPasswordRDP_Disable.reg" },
            { 18, L"tweaks\\BlankPasswordRDP.reg" },
            { 19, L"tweaks\\MapNetworkDrivesDelay_Disable.reg" },
            { 20, L"tweaks\\MapNetworkDrivesDelay.reg" },
            { 21, L"tweaks\\WU_Driver_Updates_Disable.reg" },
            { 22, L"tweaks\\WU_Driver_Updates_Enable.reg" },
            { 23, L"tweaks\\Windows_Theme_Dark.reg" },
            { 24, L"tweaks\\Windows_Theme_White.reg" },
            { 25, L"tweaks\\Annoying_Notifications_Disable.reg" },
            { 26, L"tweaks\\Annoying_Notifications_Enable.reg" } };
        BOOL embOk = TRUE;
        for (size_t i = 0; i < sizeof(EMB) / sizeof(EMB[0]); i++) {
            DWORD n = 0;
            const BYTE* d = embeddedData(EMB[i].res, &n);
            if (!d || !fileMatches(EMB[i].file, d, n)) { embOk = FALSE; wprintf(L"  WRONG: %s\n", EMB[i].file); }
        }
        // only jobs that need administrator rights can be asked of the elevated helper
        BOOL helperOk = tweakHelperMain(L"--tweak=-1") == ERROR_INVALID_PARAMETER && tweakHelperMain(L"--tweak=17") == ERROR_INVALID_PARAMETER && tweakHelperMain(L"--tweak=16") == ERROR_INVALID_PARAMETER
                     && tweakHelperMain(L"--tweak=15") == ERROR_INVALID_PARAMETER && tweakHelperMain(L"--tweak=14") == ERROR_INVALID_PARAMETER
                     && tweakHelperMain(L"--tweak=2") == ERROR_INVALID_PARAMETER && tweakHelperMain(L"--tweak=3") == ERROR_INVALID_PARAMETER
                     && tweakHelperMain(L"--tweak=12") == ERROR_INVALID_PARAMETER && tweakHelperMain(L"--tweak=13") == ERROR_INVALID_PARAMETER;
        BOOL tiOk = TRUE;
        g_elevated = isElevated();
        if (g_elevated) {
            wchar_t who[MAX_PATH], cmd[MAX_PATH + 64], exe[MAX_PATH], rf[MAX_PATH];
            GetWindowsDirectoryW(who, MAX_PATH); wcscat(who, L"\\Temp\\wcu_test_who.txt");
            systemPath(exe, L"cmd.exe");
            swprintf(cmd, MAX_PATH + 64, L"cmd /c whoami /groups > \"%s\"", who);
            DWORD code = 1, err = runAsTrustedInstaller(exe, cmd, &code);
            char buf[8192] = ""; DWORD got = 0;
            HANDLE h = CreateFileW(who, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) { ReadFile(h, buf, sizeof(buf) - 1, &got, NULL); CloseHandle(h); }
            DeleteFileW(who);
            BOOL isTi = !err && !code && strstr(buf, "NT SERVICE\\TrustedInstaller") != NULL;

            // a throwaway key: created through HKLM and deleted through HKCR as TI, the way the real files do it
            swprintf(rf, MAX_PATH, L"%swcu_test_ti.reg", base);
            const char* add = "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\WCU_TiTest]\r\n\"A\"=\"1\"\r\n";
            const char* del = "Windows Registry Editor Version 5.00\r\n\r\n[-HKEY_CLASSES_ROOT\\WCU_TiTest]\r\n";
            DWORD wr;
            h = CreateFileW(rf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); WriteFile(h, add, (DWORD)strlen(add), &wr, NULL); CloseHandle(h);
            DWORD e1 = regImportFile(rf, TRUE);
            BOOL made = regKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\WCU_TiTest");
            h = CreateFileW(rf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); WriteFile(h, del, (DWORD)strlen(del), &wr, NULL); CloseHandle(h);
            DWORD e2 = regImportFile(rf, TRUE);
            BOOL gone = !regKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\WCU_TiTest");
            DeleteFileW(rf);
            tiOk = isTi && !e1 && made && !e2 && gone;
            wprintf(L"trustedinstaller test: %s (identity %d add %lu made %d delete %lu gone %d)\n", tiOk ? L"PASS" : L"FAIL", isTi, e1, made, e2, gone);
        } else wprintf(L"trustedinstaller test: skipped, not elevated\n");
        BOOL ok = embOk && helperOk && tiOk;
        wprintf(L"enable/disable test: %s (embedded %d helper %d)\n", ok ? L"PASS" : L"FAIL", embOk, helperOk);
        filePass = filePass && ok;
    }

    // ---- Timezone and Region: finding countries, their zones and their English. Reads only. ----
    {
        static const struct { const wchar_t* typed; const wchar_t* iso; } FIND[] = {
            { L"India", L"IN" }, { L"  india ", L"IN" }, { L"IN", L"IN" }, { L"Jap", L"JP" }, { L"usa", L"US" }, { L"United States", L"US" },
            { L"UK", L"GB" }, { L"uae", L"AE" }, { L"South Korea", L"KR" }, { L"Korea", L"KR" }, { L"sri lanka", L"LK" }, { L"Germany", L"DE" },
            { L"xyzzy", NULL }, { L"", NULL }, { L"   ", NULL }, { L"United", NULL }, { L"Ind", NULL } };
        BOOL findOk = TRUE;
        for (size_t i = 0; i < sizeof(FIND) / sizeof(FIND[0]); i++) {
            wchar_t iso[8] = L"", err[200] = L"";
            BOOL found = regionFindCountry(FIND[i].typed, iso, err, 200);
            BOOL right = FIND[i].iso ? (found && !wcscmp(iso, FIND[i].iso)) : (!found && err[0]);
            if (!right) { findOk = FALSE; wprintf(L"  WRONG: '%s' -> %s (%s)\n", FIND[i].typed, iso, err); }
        }
        static RegionPick p;
        wchar_t err[200];
        BOOL india = regionPrepare(L"IN", &p, err, 200) && p.nZones == 1 && !wcscmp(p.zones[0].id, L"India Standard Time")
                  && !wcscmp(p.locale, L"en-IN") && p.realLocale;
        BOOL japan = regionPrepare(L"JP", &p, err, 200) && p.nZones == 1 && !wcscmp(p.locale, L"en-JP") && !p.realLocale;
        BOOL us = regionPrepare(L"US", &p, err, 200) && p.nZones > 3 && p.realLocale;
        BOOL west = TRUE;
        for (int i = 1; i < p.nZones; i++) if (p.zones[i - 1].bias < p.zones[i].bias) west = FALSE;
        BOOL kosovo = regionPrepare(L"XK", &p, err, 200) && p.nZones == 1;
        BOOL ok = findOk && india && japan && us && west && kosovo;
        wprintf(L"region test: %s (find %d india %d japan %d us %d order %d kosovo %d)\n", ok ? L"PASS" : L"FAIL", findOk, india, japan, us, west, kosovo);
        filePass = filePass && ok;
    }
#if WCU_DOWNLOAD
    // ---- the PC's facts at the end of a build's log: the drive under C: is found, and sizes read as disk makers write them ----
    {
        wchar_t kind[40], s1[24], s2[24], s3[24]; unsigned long long size = 0;
        BOOL disk = dlDiskFacts(L'C', kind, 40, &size) && size > 0;
        dlDiskSize(1000204886016ULL, s1, 24); dlDiskSize(512110190592ULL, s2, 24); dlDiskSize(2000398934016ULL, s3, 24);
        BOOL fmt = !wcscmp(s1, L"1 TB") && !wcscmp(s2, L"512 GB") && !wcscmp(s3, L"2 TB");
        wchar_t cap[24] = L""; if (size) dlDiskSize(size, cap, 24);
        wprintf(L"pc facts test: %s (C: is %s, %s; sizes %s %s %s)\n", disk && fmt ? L"PASS" : L"FAIL", disk ? kind : L"?", cap, s1, s2, s3);
        filePass = filePass && disk && fmt;
    }
#endif

    // Default-app hashes: every choice Windows has written for this user must check out. Only reads.
    {
        int nLatest = 0, nClassic = 0, bad = 0;
        HKEY fe;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts", 0,
                          KEY_ENUMERATE_SUB_KEYS, &fe) == ERROR_SUCCESS) {
            wchar_t ext[128];
            for (DWORD i = 0; ; i++) {
                DWORD n = 128;
                if (RegEnumKeyExW(fe, i, ext, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
                wchar_t sub[300], v[200];
                swprintf(sub, 300, UC_FILEEXTS L"%s\\UserChoiceLatest", ext);
                if (ucReadString(HKEY_CURRENT_USER, sub, L"Hash", v, 200)) {
                    nLatest++;
                    if (!ucCheck(ext, NULL, TRUE)) { bad++; wprintf(L"  latest mismatch: %s\n", ext); }
                }
                swprintf(sub, 300, UC_FILEEXTS L"%s\\UserChoice", ext);
                if (ucReadString(HKEY_CURRENT_USER, sub, L"Hash", v, 200)) {
                    nClassic++;
                    if (!ucCheck(ext, NULL, FALSE)) { bad++; wprintf(L"  classic mismatch: %s\n", ext); }
                }
            }
            RegCloseKey(fe);
        }
        BOOL ok = bad == 0;
        wprintf(L"default app hash test: %s (%d latest, %d classic checked, %d wrong)\n", ok ? L"PASS" : L"FAIL", nLatest, nClassic, bad);
        filePass = filePass && ok;
    }

    // Optional, as it downloads about 3.3 GB: WCU_OFFICE_DLTEST=<folder> runs the Office download into it, then reads it back
    {
        wchar_t dest[MAX_PATH];
        if (GetEnvironmentVariableW(L"WCU_OFFICE_DLTEST", dest, MAX_PATH)) {
            OffDlJob j; memset(&j, 0, sizeof(j));
            j.ch = OFF_CURRENT; wcscpy(j.lang, L"en-US"); lstrcpynW(j.dest, dest, MAX_PATH);
            DWORD e = offDownload(&j);
            EnterCriticalSection(&g_lock);
            for (int i = 0; i < g_appsLogN; i++) wprintf(L"  | %s\n", g_appsLog[i]);
            LeaveCriticalSection(&g_lock);
            wchar_t ver[40], langs[200]; int ch = -1;
            BOOL read = !e && offReadSource(dest, ver, 40, langs, 200, &ch);
            BOOL ok = read && ch == OFF_CURRENT && !wcscmp(langs, L"en-US");
            wprintf(L"office download test: %s (error %lu, source %s %s channel %d)\n", ok ? L"PASS" : L"FAIL", e, read ? ver : L"-", read ? langs : L"-", ch);
            filePass = filePass && ok;
        }
    }

    BOOL pass = filePass && regPass;
    wprintf(L"%s\n", pass ? L"PASS" : L"FAIL");
    return pass ? 0 : 1;
}
