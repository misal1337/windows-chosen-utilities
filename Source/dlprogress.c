// Download Windows: how far the whole job is, how long it has run, and about how long is left. Part of download.c.
//
// The download is measured: the job reports bytes written against UUP dump's file sizes. The conversion can't be: the
// converter only prints a heading as each stage starts. So each stage has a length in seconds, taken from timed builds
// on one PC (Home and Pro, 45 minutes; five editions, 66), and this PC's pace against those is learned as the stages
// go by. The pace is kept for the next build, so its estimate is right from the start.
#define DLP_PREP   100.0      // until the first edition is opened
#define DLP_OPEN    40.0      // per edition: opening it
#define DLP_APPS   160.0      //              Edge and the apps
#define DLP_UPD     30.0      //              the smaller updates
#define DLP_LCU    900.0      //              the cumulative update
#define DLP_IMAGE  (DLP_OPEN + DLP_APPS + DLP_UPD + DLP_LCU)
#define DLP_VIRT   170.0      // per extra edition made from Pro
#define DLP_TAIL   320.0      // after the last edition: setup files, recovery image, the ISO
#define DLP_STALL  0.95       // a stage running longer than expected waits here for the next heading

typedef struct {
    BOOL on;                           // an ISO job is running, or its total time is being shown
    ULONGLONG t0, tEnd;                // when it started; when it ended, 0 until then
    int step;                          // 1 tools, 2 download, 3 conversion
    unsigned long long got, total;     // bytes
    ULONGLONG tBytes, gotAt;           // the last speed sample
    double rate, active;               // bytes per second, and the seconds it was measured over
    int images, virt;                  // editions serviced, and extra ones made from Pro
    int virtMade, image;               // of those, how many have started
    ULONGLONG tConv, tStage;
    double pos, len;                   // the stage's start and length, in the timed builds' seconds
    BOOL inTail, sawLcu;
    double pace0;                      // this PC's pace from its last build: 1 is the timed PC, 2 takes twice as long
    double shown;                      // the bar never moves back
} DlProg;
static DlProg g_dlp;                   // guarded by g_lock: the output reader's thread writes it

static double dlpConvTotal(const DlProg* p) { return DLP_PREP + p->images * DLP_IMAGE + p->virt * DLP_VIRT + DLP_TAIL; }

static void dlpBegin(DlProg* p, int images, int virt, double pace0, ULONGLONG now) {
    memset(p, 0, sizeof(*p));
    p->on = TRUE; p->t0 = now;
    p->images = images < 1 ? 1 : images; p->virt = virt; p->pace0 = pace0;
}

// This PC's pace: the last build's to begin with, then this one's own once it has got through a few stages. A stage
// that's overrunning counts too: the pace is at least what has passed over everything up to that stage's end.
static double dlpPace(const DlProg* p, ULONGLONG now) {
    double w = p->pos < 100 ? 0 : min(1.0, p->pos / 400);
    double own = p->pos > 0 ? (p->tStage - p->tConv) / 1000.0 / p->pos : 0;
    double pace = p->pace0 * (1 - w) + own * w;
    double least = p->pos + p->len >= 200 ? (now - p->tConv) / 1000.0 / (p->pos + p->len) : 0;
    if (pace < least) pace = least;
    return pace < 0.1 ? 0.1 : pace > 10 ? 10 : pace;
}
static void dlpStep(DlProg* p, int step, ULONGLONG now) {
    p->step = step;
    if (step == 3) { p->tConv = p->tStage = now; p->pos = 0; p->len = DLP_PREP; }
}

static void dlpBytes(DlProg* p, unsigned long long got, unsigned long long total, ULONGLONG now) {
    if (p->tBytes && now > p->tBytes && got >= p->gotAt) {
        double dt = (now - p->tBytes) / 1000.0, inst = (got - p->gotAt) / dt;
        // The average so far, until there's a minute of it; then the last minute or so. Waiting for fresh links
        // between the two downloads isn't speed, so a long gap with nothing in it is left out.
        if (!(dt > 5 || (inst < 1 && p->active < 1))) {
            p->active += dt;
            p->rate += (inst - p->rate) * min(1.0, dt / min(60.0, p->active));
        }
    }
    p->tBytes = now; p->gotAt = got;
    p->got = got; p->total = total;
}

static void dlpGoTo(DlProg* p, double pos, double len, ULONGLONG now) {
    if (pos < p->pos) return;                      // a heading out of the usual order never takes it back
    p->pos = pos; p->len = len; p->tStage = now;
}

// A finished line of the job's output
static void dlpLine(DlProg* p, const wchar_t* s, ULONGLONG now) {
    if (p->step != 3 || wcsncmp(s, L"=== ", 4)) return;
    s += 4;
    #define DLP_IS(t) (!wcsncmp(s, t, wcslen(t)))
    double tail = DLP_PREP + p->images * DLP_IMAGE + p->virt * DLP_VIRT;
    double img = DLP_PREP + (p->image > 0 ? p->image - 1 : 0) * DLP_IMAGE + p->virtMade * DLP_VIRT;
    if (DLP_IS(L"Updating install.wim / ")) {      // says how many editions it's about to do
        int n = _wtoi(s + 23);
        if (n > 0 && n < 20) p->images = n;
        dlpGoTo(p, DLP_PREP - 20, 20, now);
    } else if (DLP_IS(L"Servicing Index: ")) {
        int k = _wtoi(s + 17);
        if (p->inTail || k < 1 || k > p->images) return;
        p->image = k;
        dlpGoTo(p, DLP_PREP + (k - 1) * DLP_IMAGE + p->virtMade * DLP_VIRT, DLP_OPEN, now);
    } else if (DLP_IS(L"Creating Edition: ")) {    // extra editions, whether inside Pro's servicing or at the very end
        double at = p->inTail ? tail : DLP_PREP + max(p->image, 1) * DLP_IMAGE + p->virtMade * DLP_VIRT;
        if (p->virtMade < p->virt) p->virtMade++;
        dlpGoTo(p, at, DLP_VIRT, now);
    } else if (!p->inTail && p->image && DLP_IS(L"Adding Microsoft Edge")) dlpGoTo(p, img + DLP_OPEN, DLP_APPS, now);
    else if (!p->inTail && p->image && DLP_IS(L"Adding Apps"))    dlpGoTo(p, img + DLP_OPEN, DLP_APPS, now);
    else if (!p->inTail && p->image && DLP_IS(L"Adding Updates")) dlpGoTo(p, img + DLP_OPEN + DLP_APPS, DLP_UPD, now);
    else if (!p->inTail && p->image && DLP_IS(L"Adding LCU "))  { p->sawLcu = TRUE; dlpGoTo(p, img + DLP_OPEN + DLP_APPS + DLP_UPD, DLP_LCU, now); }
    else if (DLP_IS(L"Rebuilding install.wim")) { p->inTail = TRUE; dlpGoTo(p, tail, 20, now); }
    else if (DLP_IS(L"Creating winre.wim"))     { p->inTail = TRUE; dlpGoTo(p, tail + 20, 60, now); }
    else if (DLP_IS(L"Adding LCU for "))        dlpGoTo(p, tail + 80, 200, now);
    else if (DLP_IS(L"Adding winre.wim to"))    dlpGoTo(p, tail + 280, 20, now);
    else if (p->inTail && DLP_IS(L"Creating ISO")) dlpGoTo(p, tail + 300, 20, now);
    #undef DLP_IS
}

// Seconds left in the whole job, or -1 when there's nothing to go on yet
static double dlpLeft(const DlProg* p, ULONGLONG now) {
    double conv = dlpConvTotal(p);
    if (p->step < 3) {
        if (p->step < 2 || !p->total || p->active < 5 || p->rate < 1) return -1;
        return (p->total > p->got ? p->total - p->got : 0) / p->rate + conv * p->pace0;
    }
    double pace = dlpPace(p, now), in = (now - p->tStage) / 1000.0 / pace;
    if (in > p->len * DLP_STALL) in = p->len * DLP_STALL;
    double left = (conv - p->pos - in) * pace;
    return left < 0 ? 0 : left;
}

// How much of the whole job's time has gone, 0 to 99: 100 is only for a finished job
static int dlpPercent(DlProg* p, ULONGLONG now) {
    double left = dlpLeft(p, now), gone = (now - p->t0) / 1000.0;
    if (left >= 0 && gone + left > 0) {
        double pct = gone * 100 / (gone + left);
        if (pct > 99) pct = 99;
        if (pct > p->shown) p->shown = pct;
    }
    return (int)p->shown;
}

static void dlpClock(ULONGLONG ms, wchar_t* out, int cch) {
    unsigned s = (unsigned)(ms / 1000);
    if (s >= 3600) swprintf(out, cch, L"%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else           swprintf(out, cch, L"%u:%02u", s / 60, s % 60);
}

// The line on the page. The time left is an estimate, so it's whole minutes, and steps of 5 when it's far off.
static void dlpText(DlProg* p, ULONGLONG now, wchar_t* out, int cch) {
    wchar_t el[24], lf[32];
    dlpClock((p->tEnd ? p->tEnd : now) - p->t0, el, 24);
    if (p->tEnd) { swprintf(out, cch, L"Took %s", el); return; }
    double left = dlpLeft(p, now);
    if (left < 0) { swprintf(out, cch, L"Elapsed %s  •  estimating the time left…", el); return; }
    int pct = dlpPercent(p, now), m = (int)ceil(left / 60);
    if (left < 60) { swprintf(out, cch, L"%d%%  •  Elapsed %s  •  less than a minute left", pct, el); return; }
    if (m > 20) m = (m + 2) / 5 * 5;
    if (m >= 60) swprintf(lf, 32, L"%d h %d min", m / 60, m % 60);
    else         swprintf(lf, 32, L"%d min", m);
    swprintf(out, cch, L"%d%%  •  Elapsed %s  •  about %s left", pct, el, lf);
}

// ---------- from the job ----------
// Bytes written by aria2c itself. The files' sizes on disk don't tell: aria2 writes each file in 16 places at once, so
// a file is nearly its full size long before it's whole.
static unsigned long long g_dlpAll, g_dlpBefore, g_dlpGroup, g_dlpRound, g_dlpLast;   // all, earlier groups', this group's, this round's start, newest
static ULONGLONG g_dlpTick;

static void dlpReport(HANDLE out) {
    wchar_t m[64];
    swprintf(m, 64, L"%c%llu/%llu%c", REP_PROG_MARK, g_dlpLast, g_dlpAll, REP_PROG_END);
    repWrite(out, m);
}
static void dlpOnTick(HANDLE proc, HANDLE out) {
    ULONGLONG now = GetTickCount64();
    if (now - g_dlpTick < 1000) return;
    g_dlpTick = now;
    IO_COUNTERS io;
    if (!GetProcessIoCounters(proc, &io)) return;
    unsigned long long v = g_dlpRound + io.WriteTransferCount, cap = g_dlpBefore + g_dlpGroup;
    if (v > cap) v = cap;                          // a file fetched twice would count twice
    if (v > g_dlpLast) g_dlpLast = v;
    dlpReport(out);
}

// ---------- in the window ----------
#define TIMER_DLP 8
static double dlpLoadPace(void) {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, LANG_KEY, L"UupPace", RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS) return 1;
    return v < 20 || v > 1000 ? 1 : v / 100.0;
}
// Called by the output reader, inside g_lock
static void dlpFeedLine(const wchar_t* s) { if (g_dlp.on && !g_dlp.tEnd) dlpLine(&g_dlp, s, GetTickCount64()); }
static void dlpFeedStep(int step)         { if (g_dlp.on && !g_dlp.tEnd) dlpStep(&g_dlp, step, GetTickCount64()); }
static void dlpFeedBytes(const wchar_t* s) {
    wchar_t* e = NULL;
    unsigned long long got = wcstoull(s, &e, 10), total = e && *e == L'/' ? wcstoull(e + 1, NULL, 10) : 0;
    if (g_dlp.on && !g_dlp.tEnd && total && got <= total) dlpBytes(&g_dlp, got, total, GetTickCount64());
}

static void dlpHide(void) {
    EnterCriticalSection(&g_lock);
    g_dlp.on = FALSE;
    LeaveCriticalSection(&g_lock);
    KillTimer(g_hwnd, TIMER_DLP);
}
// The job ended. Only a finished ISO leaves its total time on the page, and teaches the pace.
static void dlpEnd(BOOL ok) {
    KillTimer(g_hwnd, TIMER_DLP);
    EnterCriticalSection(&g_lock);
    DlProg* p = &g_dlp;
    DWORD save = 0;
    if (p->on && ok) {
        p->tEnd = GetTickCount64();
        if (p->sawLcu && p->inTail && p->tConv) {
            double pace = (p->tEnd - p->tConv) / 1000.0 / dlpConvTotal(p);
            if (pace >= 0.2 && pace <= 10) save = (DWORD)(pace * 100);
        }
    } else p->on = FALSE;
    LeaveCriticalSection(&g_lock);
    if (save) RegSetKeyValueW(HKEY_CURRENT_USER, LANG_KEY, L"UupPace", REG_DWORD, &save, sizeof(save));
}

// In the page's heading, where there was nothing: the line at the right, the bar under the title. Covers no control.
static void dlpDraw(int x1, int x2, int textL, int top) {
    wchar_t t[160] = L""; int pct = 0; BOOL bar = FALSE;
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&g_lock);
    if (g_dlp.on) {
        dlpText(&g_dlp, now, t, 160);
        bar = !g_dlp.tEnd;
        pct = dlpPercent(&g_dlp, now);
    }
    LeaveCriticalSection(&g_lock);
    if (!t[0]) return;
    RECT r = { textL, top + dp(14), x2, top + dp(46) };
    drawText(g_fSmall, t, r, bar ? g_accent : mutedRGB(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (bar) drawProgressBar(x1, x2, top + dp(73), pct);
}
