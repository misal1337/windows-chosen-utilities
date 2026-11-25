// ---------- languages ----------
// Text is translated where it's drawn and measured, so every page, dialog and message already on screen switches at
// once. lang.txt holds the translations (lang.ps1 turns it into lang_data.inc at build time). A key is the whole
// English text; {1}, {2}... stand for the parts that vary, and a translation may reorder them. Text with no entry stays
// English. Output boxes use g_fMono, which is never translated, and neither is what's typed in a box (g_trOff).
#define LANG_N 7
typedef struct { const wchar_t* s[LANG_N]; } LangRow; // English first, then in LANG_LIST's order
#include "lang_data.inc"

typedef struct { const wchar_t* code; const wchar_t* name; WORD primary; } LangInfo;
static const LangInfo LANG_LIST[LANG_N] = {
    { L"en",    L"English",                        LANG_ENGLISH },
    { L"de",    L"Deutsch",                        LANG_GERMAN },
    { L"fr",    L"Français",                  LANG_FRENCH },
    { L"es",    L"Español",                   LANG_SPANISH },
    { L"pt-BR", L"Português (Brasil)",        LANG_PORTUGUESE },
    { L"zh-CN", L"简体中文",       LANG_CHINESE },
    { L"ru",    L"Русский",       LANG_RUSSIAN },
};
static int  g_lang = 0;          // index into LANG_LIST
static int  g_langSet = -1;      // what was chosen: -1 follows Windows
static BOOL g_trOff = FALSE;

#define LANG_ROWS ((int)(sizeof(LANG_DATA) / sizeof(LANG_DATA[0])))
#define TR_CACHE 1024            // a power of two
#define TR_MAX   2048
typedef struct { wchar_t* en; wchar_t* tr; } TrEntry;
static TrEntry g_trCache[TR_CACHE];
static int g_trUsed = 0;

static void trFlush(void) {
    for (int i = 0; i < TR_CACHE; i++) {
        if (g_trCache[i].tr != g_trCache[i].en) free(g_trCache[i].tr);
        free(g_trCache[i].en);
        g_trCache[i].en = g_trCache[i].tr = NULL;
    }
    g_trUsed = 0;
}

typedef struct { const wchar_t* p; int n; } TrArg;

// Whether s is the template t with something in place of each {n}. A part that varies takes the shortest text that
// lets the rest match, and never nothing at all.
static BOOL trMatch(const wchar_t* t, const wchar_t* s, TrArg* a) {
    for (; *t; t++, s++) {
        if (t[0] == L'{' && t[1] >= L'1' && t[1] <= L'9' && t[2] == L'}') {
            int k = t[1] - L'1';
            if (!t[3]) { if (!*s) return FALSE; a[k].p = s; a[k].n = (int)wcslen(s); return TRUE; }
            for (const wchar_t* e = s; *e; ) {
                e++;
                if (*e != t[3]) continue;
                if (trMatch(t + 3, e, a)) { a[k].p = s; a[k].n = (int)(e - s); return TRUE; }
            }
            return FALSE;
        }
        if (*t != *s) return FALSE;
    }
    return !*s;
}

// The translation of s into out, or FALSE if there is none
static BOOL trMake(const wchar_t* s, wchar_t* out, int cch, int depth) {
    for (int i = 0; i < LANG_ROWS; i++)
        if (!wcscmp(LANG_DATA[i].s[0], s)) { lstrcpynW(out, LANG_DATA[i].s[g_lang], cch); return TRUE; }
    if (depth > 1) return FALSE;
    for (int i = 0; i < LANG_ROWS; i++) {              // lang.ps1 puts the most specific templates first
        const wchar_t* t = LANG_DATA[i].s[0];
        if (!wcschr(t, L'{')) continue;
        TrArg a[9]; memset(a, 0, sizeof(a));
        if (!trMatch(t, s, a)) continue;
        int n = 0;
        for (const wchar_t* p = LANG_DATA[i].s[g_lang]; *p && n < cch - 1; p++) {
            if (p[0] == L'{' && p[1] >= L'1' && p[1] <= L'9' && p[2] == L'}') {
                TrArg* g = &a[p[1] - L'1'];
                wchar_t in[512], tr[1024];
                int len = min(g->n, 511);
                if (g->p) wmemcpy(in, g->p, len); else len = 0;
                in[len] = 0;
                const wchar_t* use = trMake(in, tr, 1024, depth + 1) ? tr : in;
                while (*use && n < cch - 1) out[n++] = *use++;
                p += 2;
            } else out[n++] = *p;
        }
        out[n] = 0;
        return TRUE;
    }
    return FALSE;
}

// Only the window's thread draws, so the cache needs no lock. The result is good until the next call.
static const wchar_t* tr(const wchar_t* s) {
    if (!g_lang || g_trOff || !s || !s[0] || !s[1]) return s;
    size_t len = wcslen(s);
    if (len > 1000) return s;
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ s[i]) * 16777619u;
    if (g_trUsed > TR_CACHE * 3 / 4) trFlush();
    unsigned at = h & (TR_CACHE - 1);
    while (g_trCache[at].en) {
        if (!wcscmp(g_trCache[at].en, s)) return g_trCache[at].tr;
        at = (at + 1) & (TR_CACHE - 1);
    }
    wchar_t* key = _wcsdup(s);
    if (!key) return s;
    static wchar_t out[TR_MAX];
    wchar_t* val = trMake(s, out, TR_MAX, 0) ? _wcsdup(out) : key;
    g_trCache[at].en = key;
    g_trCache[at].tr = val ? val : key;
    g_trUsed++;
    return g_trCache[at].tr;
}

static int langOfWindows(void) {
    LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_CHINESE)             // only Simplified is here; Traditional readers get English
        return (SUBLANGID(id) == SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(id) == SUBLANG_CHINESE_SINGAPORE) ? 5 : 0;
    for (int i = 0; i < LANG_N; i++) if (LANG_LIST[i].primary == PRIMARYLANGID(id)) return i;
    return 0;
}
static const wchar_t* LANG_KEY = L"Software\\WindowsChosenUtilities";
static void langLoad(void) {
    wchar_t code[16] = L""; DWORD cb = sizeof(code);
    g_langSet = -1;
    if (RegGetValueW(HKEY_CURRENT_USER, LANG_KEY, L"Language", RRF_RT_REG_SZ, NULL, code, &cb) == ERROR_SUCCESS)
        for (int i = 0; i < LANG_N; i++) if (!_wcsicmp(code, LANG_LIST[i].code)) g_langSet = i;
    g_lang = g_langSet < 0 ? langOfWindows() : g_langSet;
}
static void langChoose(int set) {
    g_langSet = set;
    g_lang = set < 0 ? langOfWindows() : set;
    trFlush();
    if (set < 0) RegDeleteKeyValueW(HKEY_CURRENT_USER, LANG_KEY, L"Language");
    else RegSetKeyValueW(HKEY_CURRENT_USER, LANG_KEY, L"Language", REG_SZ, LANG_LIST[set].code,
                         (DWORD)((wcslen(LANG_LIST[set].code) + 1) * sizeof(wchar_t)));
}
