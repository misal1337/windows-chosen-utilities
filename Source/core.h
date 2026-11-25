// Shared by every part of the app: Windows headers, messages, types, global state, and animation.

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <math.h>
#include <wchar.h>
#include <ctype.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <shobjidl.h>
#include <wbemcli.h>
#include <d2d1.h>     // must follow the shell headers: it redefines a macro they depend on
#include <dwrite.h>

// ---- DWM constants (define in case the bundled headers are older) ----
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#define WCU_DWMSBT_MAINWINDOW 2   // Mica
#define WCU_DWMWCP_ROUND 2


// ---- app messages ----
#define WM_APP_CATDONE   (WM_APP + 1)   // wParam = category index
#define WM_APP_SCANDONE  (WM_APP + 2)
#define WM_APP_CLEANPROG (WM_APP + 3)   // wParam = category index
#define WM_APP_CLEANDONE (WM_APP + 4)
#define WM_APP_EXPLORERDONE (WM_APP + 5)
#define WM_APP_ADMINSIZE (WM_APP + 6)   // from the elevated helper: wParam = category, lParam = bytes
#define WM_APP_DISMPROG  (WM_APP + 7)   // wParam = percent
#define WM_APP_DISMDONE  (WM_APP + 8)   // wParam = DISM exit code
#define WM_APP_AIOPROG   (WM_APP + 9)   // wParam = percent, lParam = stage
#define WM_APP_AIODONE   (WM_APP + 10)  // wParam = result code
#define WM_APP_SRDONE    (WM_APP + 20)  // wParam = TRUE if System Restore was switched
#define WM_APP_REPAIROUT  (WM_APP + 21) // new text for the Windows Repair output
#define WM_APP_REPAIRDONE (WM_APP + 22) // wParam = the runner's exit code
#define WM_APP_TWEAKDONE  (WM_APP + 23) // wParam = 0 or an error code
#define WM_APP_RAISE      (WM_APP + 24) // a UAC prompt just closed: take the foreground back
#define WM_APP_DLRESULT   (WM_APP + 25) // Download Windows: lParam = a lookup's result, for the window to free
#define WM_APP_UPDFOUND   (WM_APP + 26) // Manual Updates: the newest update was looked up; wParam = UPD_FOUND etc.
#define WM_APP_UPDDONE    (WM_APP + 27) // Manual Updates: its download ended; wParam = 0 or an error code
#define WM_APP_RUNTI_DONE (WM_APP + 28) // Run as TI: the file finished; wParam = 0, an error, or the child's exit code
#define WM_APP_APPSPROG   (WM_APP + 29) // Install Apps: from the helper, wParam = step, lParam = megabytes downloaded
#define WM_APP_APPSDONE   (WM_APP + 30) // Install Apps: the install ended; wParam = 0 or the error of the step after it
#define WM_APP_APPSLATEST (WM_APP + 31) // Install Apps: newest versions looked up; lParam = a malloc'd list for the window to free
#define WM_APP_OFFLATEST  (WM_APP + 32) // MS Office 365: newest build per channel; lParam = a malloc'd list
#define WM_APP_OFFDONE    (WM_APP + 33) // MS Office 365: a download or install ended; wParam = 0 or an error, lParam = folder or NULL
#define WM_APP_OFFVER     (WM_APP + 34) // MS Office 365: after a switch, Office's build changed (or the wait ended); lParam = malloc'd build or NULL
#define WM_APP_RMDONE     (WM_APP + 35) // Remove Apps: wParam = 1 if an install/uninstall ended; lParam = malloc'd installed flags or NULL

// ---- category kinds ----
enum { KIND_FOLDER, KIND_RECYCLEBIN, KIND_PATTERN, KIND_RESTORE_OLD, KIND_RESTORE_ALL };

typedef struct {
    const wchar_t* name;
    const wchar_t* desc;
    int   kind;
    wchar_t path[MAX_PATH];     // resolved folder (for FOLDER / PATTERN)
    const wchar_t* patterns[4]; // for PATTERN (NULL-terminated list)
    unsigned int glyph;         // Segoe Fluent Icons codepoint
    BOOL  needsAdmin;
    // runtime state (guarded by g_lock for the 64-bit/size fields)
    BOOL  available;            // path exists / usable
    BOOL  adminBlocked;         // Windows refused to list this folder without elevation
    BOOL  adminKnown;           // the elevated helper has reported a size for it
    unsigned long long adminBytes;
    BOOL  checked;
    BOOL  scanned;
    unsigned long long bytes;
    int   status;               // 0 none, 1 scanning, 2 cleaning, 3 cleaned
} Category;

// ---- globals ----
static HINSTANCE g_inst;
static HWND  g_hwnd;
static BOOL  g_dark = TRUE;
static BOOL  g_winDark = FALSE;      // Windows' own theme, which g_dark follows unless the switch overrides it
static BOOL  g_elevated = FALSE;
static COLORREF g_accent = RGB(0, 120, 212);
static int   g_dpi = 96;
static CRITICAL_SECTION g_lock;
static volatile LONG g_busy = 0;       // 0 idle, 1 scanning, 2 cleaning/restarting
static HANDLE g_worker = NULL;
static BOOL g_askExplorer = FALSE;      // offer an Explorer restart after a thumbnail-cache clean
static BOOL g_justCleaned = FALSE;      // the scan in progress follows a clean-up
static int  g_dlgDepth = 0;             // >0 while a modal dialog dims the main window

// Windows 11 style modal dialog. Returns 1 for the primary button, 0 otherwise.
typedef struct { const wchar_t* left; const wchar_t* right; } DlgRow;
// A dialog can also ask for text, for one choice from a list, or, with checks, for any number of them.
typedef struct {
    wchar_t* input; int inputCch;                                   // edited in place
    BOOL (*check)(const wchar_t* text, wchar_t* err, int errCch);   // on the primary button: FALSE keeps it open, showing err
    const wchar_t* const* items; int nItems; int* pick;
    BOOL* checks;                   // one per item, toggled in place; adds Select all and Select none
} DlgExtra;
static int showDialog(HWND owner, const wchar_t* title, const wchar_t* body,
                      const DlgRow* rows, int nrows,
                      const wchar_t* primary, const wchar_t* secondary, int focusBtn);

typedef IDWriteTextFormat* WFont;
static WFont g_fBody, g_fBodyBold, g_fSmall, g_fH1, g_fTitle, g_fIcon, g_fIconLg, g_fHeadIcon, g_fMono, g_fTiny;

// Shown in the window's bottom-right corner. Changed only when the user says so.
#include "version.h"
#define WCU_VERSION L"v" WCU_VER_STR
static IDWriteFactory* g_dw = NULL;
static ID2D1Factory* g_d2d = NULL;
static ID2D1DCRenderTarget* g_rt = NULL;
static ID2D1SolidColorBrush* g_textBrush = NULL;
static IDWriteRenderingParams* g_textParams = NULL;

// backbuffer + a scratch buffer used as an alpha mask for text/icons
static HDC     g_memDC = NULL;
static HBITMAP g_memBmp = NULL;
static void*   g_bits = NULL;
static int     g_bw = 0, g_bh = 0;
static HDC     g_scratchDC = NULL;
static HBITMAP g_scratchBmp = NULL;
static void*   g_scratchBits = NULL;

static int g_hotRow = -1;      // hovered category row
static int g_hotBtn = -1;      // hovered button: what hitButton returned
static int g_hotNav = -1;      // hovered nav item
static int g_pressBtn = -1;
static wchar_t g_status[256];

// pages selected from the left rail
// The Download Windows page depends on UUP dump and Microsoft's servers. Set this to 0 to take it out of the app.
#ifndef WCU_DOWNLOAD
#define WCU_DOWNLOAD 1
#endif
// A page's number is its place in the rail. Install Apps comes after Download Windows, or takes its place.
#if WCU_DOWNLOAD
enum { PAGE_CLEANUP = 0, PAGE_REGNET = 1, PAGE_RUNTI = 2, PAGE_DISM = 3, PAGE_AIO = 4, PAGE_REPAIR = 5, PAGE_TWEAKS = 6, PAGE_DOWNLOAD = 7, PAGE_APPS = 8, PAGE_OFFICE = 9, PAGE_REMOVE = 10 };
#define NAV_COUNT 11
#else
enum { PAGE_CLEANUP = 0, PAGE_REGNET = 1, PAGE_RUNTI = 2, PAGE_DISM = 3, PAGE_AIO = 4, PAGE_REPAIR = 5, PAGE_TWEAKS = 6, PAGE_APPS = 7, PAGE_OFFICE = 8, PAGE_REMOVE = 9, PAGE_DOWNLOAD = 100 };
#define NAV_COUNT 10
#endif
static int g_page = PAGE_CLEANUP;
static int g_jobPage = -1;              // the page the running job (g_busy 2) was started on
static BOOL jobBegin(void) {            // FALSE when something else is running
    if (InterlockedCompareExchange(&g_busy, 2, 0) != 0) return FALSE;
    g_jobPage = g_page;
    return TRUE;
}
static RECT g_rcNav[NAV_COUNT];
static RECT g_rcUefi, g_rcUsbBoot;
static RECT g_rcTheme, g_rcLang;

// ---- fades: every interactive element owns a slot that eases toward on/off (83 ms, as net-merge) ----
// Each range must hold all its items: an overlap makes two things pull one slot both ways every frame, which
// repaints the window nonstop (a ninth rail item once shared the dialog dim's slot and made the whole app flicker).
enum { ANIM_BTN = 0, ANIM_ROW = 10, ANIM_CHK = 20, ANIM_NAV = 30, ANIM_NAVSEL = 46, ANIM_DIM = 62, ANIM_DLG = 63,
       ANIM_TAB = 66, ANIM_WROW = 74, ANIM_DROW = 90, ANIM_AIO = 100, ANIM_AIOF = 280, ANIM_MOPT = 460,
       ANIM_VIEW = 470, ANIM_SROFF = 480, ANIM_SROFFH = 481,
       ANIM_XROW = 482, ANIM_XCHK = 506, ANIM_LANG = 554, ANIM_MAX = 555 };
#define ANIM_XROW_N (ANIM_XCHK - ANIM_XROW)   // Remove Apps: hover of each visible list row
#define ANIM_XCHK_N (ANIM_LANG - ANIM_XCHK)    // Remove Apps: each app's tick
_Static_assert(NAV_COUNT <= ANIM_NAVSEL - ANIM_NAV && NAV_COUNT <= ANIM_DIM - ANIM_NAVSEL, "more rail items than animation slots");
typedef struct { float v; ULONGLONG t; } AnimSlot;
static AnimSlot g_animS[ANIM_MAX];
static BOOL   g_needFrame = FALSE;
static double g_alphaMul = 1.0;         // applied to everything drawn while a page or dialog fades
static double g_tabMul = 1.0;

static double anim(int id, BOOL on) {
    if (id < 0 || id >= ANIM_MAX) return on ? 1.0 : 0.0;
    AnimSlot* s = &g_animS[id];
    float target = on ? 1.0f : 0.0f;
    ULONGLONG now = GetTickCount64();
    if (s->v == target) { s->t = now; return target; }
    ULONGLONG dt = now - s->t;
    if (dt > 100) dt = 16;               // first step after idling
    float step = (float)dt / 83.0f;
    s->v = on ? (s->v + step > 1.0f ? 1.0f : s->v + step) : (s->v - step < 0.0f ? 0.0f : s->v - step);
    s->t = now;
    if (s->v != target) g_needFrame = TRUE;
    return s->v;
}

// Page, tool and theme changes: the content fades out (60 ms), the change happens, then it fades in (90 ms).
enum { FADE_NONE = -1, FADE_PAGE = 0, FADE_TAB = 1, FADE_THEME = 2 };
static int    g_fadeAct = FADE_NONE, g_fadeArg = 0, g_fadePhase = 0;
static double g_fadeV = 1.0;
static ULONGLONG g_fadeT = 0;
static void applyFade(void);
static void startFade(int act, int arg) {
    if (g_fadeAct != FADE_NONE && g_fadePhase == 0) applyFade();   // a fade already running finishes at once
    g_fadeAct = act; g_fadeArg = arg; g_fadePhase = 0; g_fadeV = 1.0; g_fadeT = GetTickCount64();
    InvalidateRect(g_hwnd, NULL, FALSE);
}
static void stepFade(void) {
    if (g_fadeAct == FADE_NONE) return;
    ULONGLONG now = GetTickCount64();
    double dt = (double)(now - g_fadeT); g_fadeT = now;
    if (g_fadePhase == 0) {
        g_fadeV -= dt / 60.0;
        if (g_fadeV <= 0) { g_fadeV = 0; applyFade(); g_fadePhase = 1; }
    } else {
        g_fadeV += dt / 90.0;
        if (g_fadeV >= 1) { g_fadeV = 1; g_fadeAct = FADE_NONE; }
    }
    g_needFrame = TRUE;
}
static RECT g_rcDismInstall;
static BOOL g_dismRunning = FALSE;
static int  g_dismPct = -1;              // -1 until DISM reports progress
static wchar_t g_dismPath[MAX_PATH];
static wchar_t g_dismStatus[MAX_PATH + 64];
static RECT g_rcRegOpen;
static RECT g_rcRegPurge;

#define NCAT 9
static Category g_cat[NCAT];

// What hitButton returns. View buttons are BTN_VIEW + row; widgets are BTN_WIDGET + their A_ id.
enum { BTN_RESCAN = 0, BTN_CLEAN, BTN_REGOPEN, BTN_REGPURGE, BTN_UEFI, BTN_DISM, BTN_THEME, BTN_UPDGET, BTN_UPDNDP, BTN_USBBOOT,
       BTN_VIEW = 50, BTN_SROFF = 60, BTN_LANG, BTN_WIDGET = 100 };

// layout rects (filled each paint, used for hit testing), in device px
static RECT g_rcRow[NCAT];
static RECT g_rcSizeLink[NCAT];   // "Show size" link on rows Windows won't let us list
static RECT g_rcView[NCAT];       // small "View files" button on each row
static RECT g_rcSrOff;            // "Turn off System Restore" checkbox and label
static RECT g_rcBtnRescan, g_rcBtnClean;
static int  g_rowsEnabled[NCAT];

