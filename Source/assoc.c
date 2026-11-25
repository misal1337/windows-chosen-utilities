// Default apps for file types: writes the per-user choice Windows itself writes when you pick an app in Settings.
//
// Windows only honours a choice that carries the right hash, so both hashes are worked out here:
//  - UserChoice, the older one, follows PS-SFTA (DanysysTeam, MIT licence).
//  - UserChoiceLatest, the newer one, used where Windows has switched to it (HashVersion 1). It's a C port of
//    cssxn/UserChoiceLatestHash (MIT licence, notice in uclhash_tables.inc), checked against the real entries
//    Windows had written on the developer's PC.
// Neither hash is documented by Microsoft, so a Windows update could change them. Every write is checked.

#include <sddl.h>

typedef unsigned char      U8;
typedef unsigned short     U16;
typedef unsigned int       U32;
typedef unsigned long long U64;

#include "uclhash_tables.inc"

// ---------- small helpers ----------
static BOOL ucMd5(const void* data, DWORD n, U32 out[4]) {
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0; BOOL ok = FALSE;
    BYTE d[16]; DWORD cb = 16;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, CALG_MD5, 0, 0, &h)
        && CryptHashData(h, (const BYTE*)data, n, 0) && CryptGetHashParam(h, HP_HASHVAL, d, &cb, 0) && cb == 16) {
        for (int i = 0; i < 4; i++) out[i] = d[i * 4] | (d[i * 4 + 1] << 8) | (d[i * 4 + 2] << 16) | ((U32)d[i * 4 + 3] << 24);
        ok = TRUE;
    }
    if (h) CryptDestroyHash(h);
    if (prov) CryptReleaseContext(prov, 0);
    return ok;
}

// 8 bytes to base64: always 12 characters, the last one '='
static void ucBase64(const U8 in[8], wchar_t out[13]) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < 8; i += 3) {
        U32 v = (U32)in[i] << 16;
        if (i + 1 < 8) v |= (U32)in[i + 1] << 8;
        if (i + 2 < 8) v |= in[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = i + 1 < 8 ? A[(v >> 6) & 63] : L'=';
        out[o++] = i + 2 < 8 ? A[v & 63] : L'=';
    }
    out[o] = 0;
}

static void ucLower(wchar_t* s) { for (; *s; s++) if (*s >= L'A' && *s <= L'Z') *s += 32; }

// ---------- UserChoice (the older hash) ----------
static void ucClassicHash(const wchar_t* lowered, wchar_t out[13]) {
    size_t n = wcslen(lowered);
    U32 bytes = (U32)(n * 2 + 2);                       // the string with its terminator, as UTF-16
    U32 md[4] = { 0 };
    ucMd5(lowered, bytes, md);
    int length = ((bytes & 4) == 0 ? 1 : 0) + (int)(bytes >> 2) - 1;
    U8 res[8] = { 0 };
    if (length > 1) {
        U32* d = (U32*)calloc((bytes >> 2) + 2, 4);
        if (!d) { out[0] = 0; return; }
        memcpy(d, lowered, bytes);
        U32 count = (((U32)length - 2) >> 1) + 1;

        U32 md51 = (md[0] | 1) + 0x69FB0000u, md52 = (md[1] | 1) + 0x13DB0000u;
        U32 cache = 0, o1 = 0, o2 = 0, p = 0;
        for (U32 c = count; c; c--) {
            U32 r0 = d[p] + o1, r1 = d[p + 1]; p += 2;
            U32 r20 = r0 * md51 - 0x10FA9605u * (r0 >> 16);
            U32 r21 = 0x79F8A395u * r20 + 0x689B6B9Fu * (r20 >> 16);
            U32 r3 = 0xEA970001u * r21 - 0x3C101569u * (r21 >> 16);
            U32 r4 = r3 + r1, r5 = cache + r3;
            U32 r60 = r4 * md52 - 0x3CE8EC25u * (r4 >> 16);
            U32 r61 = 0x59C3AF2Du * r60 - 0x2232E0F1u * (r60 >> 16);
            o1 = 0x1EC90001u * r61 + 0x35BD1EC9u * (r61 >> 16);
            o2 = r5 + o1;
            cache = o2;
        }
        U32 a1 = o1, a2 = o2;

        md51 = md[0] | 1; md52 = md[1] | 1;
        cache = 0; o1 = 0; o2 = 0; p = 0;
        for (U32 c = count; c; c--) {
            U32 r0 = d[p] + o1; p += 2;
            U32 r10 = r0 * md51;
            U32 r11 = 0xB1110000u * r10 - 0x30674EEFu * (r10 >> 16);
            U32 r20 = 0x5B9F0000u * r11 - 0x78F7A461u * (r11 >> 16);
            U32 r21 = 0x12CEB96Du * (r20 >> 16) - 0x46930000u * r20;
            U32 r3 = 0x1D830000u * r21 + 0x257E1D83u * (r21 >> 16);
            U32 r40 = md52 * (r3 + d[p - 1]);
            U32 r41 = 0x16F50000u * r40 - 0x5D8BE90Bu * (r40 >> 16);
            U32 r50 = 0x96FF0000u * r41 - 0x2C7C6901u * (r41 >> 16);
            U32 r51 = 0x2B890000u * r50 + 0x7C932B89u * (r50 >> 16);
            o1 = 0x9F690000u * r51 - 0x405B6097u * (r51 >> 16);
            o2 = o1 + cache + r3;
            cache = o2;
        }
        free(d);
        U32 h1 = o1 ^ a1, h2 = o2 ^ a2;
        for (int i = 0; i < 4; i++) { res[i] = (U8)(h1 >> (8 * i)); res[4 + i] = (U8)(h2 >> (8 * i)); }
    }
    ucBase64(res, out);
}

// The "User Choice set via Windows User Experience {…}" text, as shell32.dll holds it
static void ucExperience(wchar_t* out, int cch) {
    static wchar_t cached[160];
    if (!cached[0]) {
        wcscpy(cached, L"User Choice set via Windows User Experience {D18B6DD5-6124-4341-9318-804003BAFA0B}");
        wchar_t path[MAX_PATH];
        systemPath(path, L"shell32.dll");
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE m = f != INVALID_HANDLE_VALUE ? CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
        const BYTE* v = m ? (const BYTE*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
        if (v) {
            LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
            static const wchar_t KEY[] = L"User Choice set via Windows User Experience";
            size_t kb = sizeof(KEY) - 2, total = (size_t)sz.QuadPart;
            for (size_t i = 0; i + kb + 2 * 80 < total; i += 2) {
                if (memcmp(v + i, KEY, kb)) continue;
                const wchar_t* s = (const wchar_t*)(v + i);
                int n = 0;
                while (n < 150 && s[n] && s[n] != L'}') n++;
                if (n < 150 && s[n] == L'}') { memcpy(cached, s, (n + 1) * 2); cached[n + 1] = 0; }
                break;
            }
            UnmapViewOfFile(v);
        }
        if (m) CloseHandle(m);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    lstrcpynW(out, cached, cch);
}

// ---------- UserChoiceLatest (the newer hash) ----------
typedef struct { U8* region; U8* longRw; U8* fallback; U8* byteClass; U8* bitWidth; U8* shortRw; } UcTables;
static U8* ucHexBytes(const char* hex) {
    size_t n = strlen(hex) / 2;
    U8* b = (U8*)malloc(n ? n : 1);
    for (size_t i = 0; b && i < n; i++) {
        char a = hex[2 * i], c = hex[2 * i + 1];
        int hi = (a >= '0' && a <= '9') ? a - '0' : (a | 32) - 'a' + 10;
        int lo = (c >= '0' && c <= '9') ? c - '0' : (c | 32) - 'a' + 10;
        b[i] = (U8)((hi << 4) | lo);
    }
    return b;
}
static const UcTables* ucTables(void) {
    static UcTables t; static volatile LONG ready = 0;
    if (InterlockedCompareExchange(&ready, 0, 0) == 2) return &t;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        t.region = ucHexBytes(kTokenRegionHex);       t.longRw = ucHexBytes(kLongRewindTableHex);
        t.fallback = ucHexBytes(kFallbackTokenCodeTableHex); t.byteClass = ucHexBytes(kByteClassTableHex);
        t.bitWidth = ucHexBytes(kTokenBitWidthTableHex); t.shortRw = ucHexBytes(kShortRewindTableHex);
        InterlockedExchange(&ready, 2);
    }
    while (InterlockedCompareExchange(&ready, 0, 0) != 2) Sleep(1);
    return &t;
}

#define UC_SENTINEL 0xFBFFFFFFFFFFFFFFULL
#define UC_TOK_A    0xFE00000000000000ULL
#define UC_TOK_B    0xFE00800000000000ULL
#define UC_TOK_C    0xFF00000000000000ULL
#define UC_TOK_D    0xFE007DFFFFFFFFFFULL
#define UC_TOK_E    0xFE00801F7FFFFFFFULL
#define UC_SEEN_MOD 0x2803u

typedef struct { U64* e; U64 cap; U64 count; U64 seen[161]; U64 last; U32 repeat; U32 total; BOOL oom; } UcEnc;

static void ucMark(UcEnc* s, U64 v) { U32 i = (U32)(v % UC_SEEN_MOD); s->seen[i >> 6] |= 1ULL << (i & 63); }
static BOOL ucSeen(const UcEnc* s, U64 v) { U32 i = (U32)(v % UC_SEEN_MOD); return (s->seen[i >> 6] & (1ULL << (i & 63))) != 0; }
static void ucPush(UcEnc* s, U64 v) {
    if (s->count >= s->cap) {
        U64* n = (U64*)realloc(s->e, (size_t)(s->cap + 1024) * sizeof(U64));
        if (!n) { s->oom = TRUE; return; }
        memset(n + s->cap, 0, 1024 * sizeof(U64));
        s->e = n; s->cap += 1024;
    }
    s->e[s->count++] = v;
}
static BOOL ucFindPrev(const UcEnc* s, U64 v, U64* dist) {
    if (!ucSeen(s, v) || s->count == 0) return FALSE;
    U64 start = s->count > 0x400 ? s->count - 0x400 : 0;
    for (U64 i = s->count - 1;; i--) {
        if (s->e[i] == v) { *dist = s->count - i; return TRUE; }
        if (i == start) break;
    }
    return FALSE;
}
static U8 ucClass(const UcTables* t, U16 ch) {
    U8 hi = (U8)(ch >> 8);
    return hi ? (U8)(t->byteClass[hi] + 8) : t->byteClass[ch & 0xFF];
}
static U64 ucPackToken(const UcTables* t, const U16* cur, const U16* end, U32 code, U32 len, U32 base) {
    U64 tok = (U64)(U8)code << 56;
    int shift = 56;
    for (U32 i = 0; i < (U8)len; i++) {
        if (end && cur + i >= end) break;
        shift -= t->bitWidth[base + i];
        tok |= (U64)cur[i] << shift;
    }
    return tok;
}
static BOOL ucToken1(const UcTables* t, const U16* cur, const U16* end, U64* tok, U8* kind, U8* len) {
    if (cur >= end) return FALSE;
    U32 code = t->region[ucClass(t, *cur) + 256u], tl = t->region[code];
    if (cur + tl > end) return FALSE;
    U32 base = 168u * code;
    for (;;) {
        U32 pos = 0;
        if (tl == 0) break;
        const U16* pr = cur;
        BOOL fits = FALSE;
        while ((U64)*pr < (1ULL << t->bitWidth[base + pos])) {
            ++pos; ++pr;
            if (pos >= tl) { fits = TRUE; break; }
        }
        if (fits) break;
        ++code; base = 168u * code; tl = t->region[code];
        if (tl > (U32)(end - cur)) break;
    }
    *tok = ucPackToken(t, cur, NULL, code, tl, base);
    *kind = (U8)code; *len = (U8)tl;
    return TRUE;
}
static BOOL ucToken2(const UcTables* t, const U16* cur, const U16* end, U64* tok, U8* kind, U8* len) {
    if (cur >= end) return FALSE;
    U32 remain = (U32)(end - cur);
    U32 code = t->region[ucClass(t, *cur) + 256u], tl = t->region[code];
    if (tl > remain) { code = t->fallback[remain]; tl = t->region[code]; }
    U32 base = 168u * code;
    BOOL again = TRUE;
    while (again) {
        again = FALSE;
        U32 pos = 0;
        if (tl == 0) break;
        for (const U16* pr = cur; pr < end; ) {
            if ((U64)*pr >= (1ULL << t->bitWidth[base + pos])) {
                ++code; base = 168u * code; tl = t->region[code];
                if (tl <= remain) again = TRUE;
                break;
            }
            ++pos; ++pr;
            if (pos >= tl) break;
        }
    }
    *tok = ucPackToken(t, cur, end, code, tl, base);
    *kind = (U8)code; *len = (U8)tl;
    return TRUE;
}
// The repeat handling both passes share. Returns TRUE if it consumed the token.
static BOOL ucRepeat(UcEnc* s, const UcTables* t, const U16** cur, U64 tok, U8 kind) {
    if (tok == s->last && s->repeat < 0xFF) { ++s->repeat; return TRUE; }
    if (s->repeat == 0) return FALSE;
    U64 dist = 0;
    if (!ucFindPrev(s, s->last, &dist)) return FALSE;
    U32 rep = s->repeat - 1;
    U64 entry;
    if (dist >= 0x800) {
        U64 packed = (tok >> 17) | ((U64)rep << 48) | UC_TOK_A;
        ucMark(s, packed); ucPush(s, packed);
        entry = s->last;
    } else {
        entry = (tok >> 27) | ((U64)((((U32)(U16)dist - 1u) & 0x3FFu) | (rep << 11)) << 37) | UC_TOK_B;
    }
    ucMark(s, entry); ucPush(s, entry);
    s->last = UC_SENTINEL; s->repeat = 0;
    *cur -= dist >= 0x800 ? t->longRw[kind] : t->shortRw[kind];
    return TRUE;
}
static void ucEmit1(UcEnc* s, const UcTables* t, const U16** cur, const U16* end, U64 tok, U8 kind) {
    BOOL hadRepeat = s->repeat != 0 && !(tok == s->last && s->repeat < 0xFF);
    if (ucRepeat(s, t, cur, tok, kind)) return;
    if (!hadRepeat) {
        U64 dist = 0;
        if (ucFindPrev(s, tok, &dist) && *cur < end) {
            U8 next = t->region[ucClass(t, **cur) + 256u];
            U64 entry = dist >= 0x800 ? tok : ((tok >> 18) | ((dist - 1) << 46) | UC_TOK_C);
            ucMark(s, entry); ucPush(s, entry);
            if (dist < 0x800) *cur -= t->shortRw[next + 256u];
            s->last = tok;
            return;
        }
    }
    ucMark(s, tok); ucPush(s, tok); s->last = tok;
}
static void ucEmit2(UcEnc* s, const UcTables* t, const U16** cur, U64 tok, U8 kind) {
    if (ucRepeat(s, t, cur, tok, kind)) return;
    ucMark(s, tok); ucPush(s, tok); s->last = tok;
}
static void ucFlush(UcEnc* s) {
    if (s->repeat == 0) return;
    U64 entry;
    if (s->last == UC_SENTINEL) entry = ((U64)s->repeat << 48) | UC_TOK_D;
    else {
        U64 dist = 0;
        if (ucFindPrev(s, s->last, &dist) && dist) {
            entry = ((U64)((((U32)(U16)dist - 1u) & 0x3FFu) | ((s->repeat - 1) << 11)) << 37) | UC_TOK_E;
        } else {
            U64 rt = ((U64)s->repeat << 48) | UC_TOK_D;
            ucMark(s, rt); ucPush(s, rt);
            entry = s->last;
        }
    }
    ucMark(s, entry); ucPush(s, entry);
    s->repeat = 0; s->last = UC_SENTINEL;
}

static U32 ucLowMul(U32 a, U32 b) { return a * b; }
static void ucMixA(const U32* w, int n, const U32 md[4], const U32 seed[4], U32 out[2]) {
    U32 md0 = md[0] | 1u, md1 = (md[1] | 1u) + 0x13DB0000u;
    U32 sum = 0, acc = 0; const U32* p = w; int rem = n;
    U32 loops = (((U32)rem - 2u) >> 1) + 1u;
    if (loops) {
        U32 mul = md0 + seed[0];
        while (loops--) {
            acc += p[0]; rem -= 2;
            U32 x = acc, hi = x >> 16; p += 2;
            U32 t = ucLowMul(x, mul) - ucLowMul(hi, seed[3]);
            U32 s = ucLowMul(t, seed[1]) + ucLowMul(t >> 16, 0x689B6B9Fu);
            U32 part1 = ucLowMul(s, 0xEA970001u) - ucLowMul(s >> 16, 0x3C101569u);
            U32 ww = p[-1] + part1;
            U32 t2 = ucLowMul(ww, md1) - ucLowMul(ww >> 16, seed[2]);
            U32 tmp = ucLowMul(t2, 0x59C3AF2Du) - ucLowMul(t2 >> 16, 0x2232E0F1u);
            acc = ucLowMul(tmp >> 16, 0x35BD1EC9u) + ucLowMul(tmp, 0x1EC90001u);
            sum += part1 + acc;
        }
    }
    if (rem == 1) {
        acc += p[0];
        U32 t = ucLowMul(acc, md0 + seed[0]) - ucLowMul(acc >> 16, seed[3]);
        U32 part = ucLowMul(t, seed[1]) - ucLowMul(t >> 16, 0x689B6B9Fu);
        part += ucLowMul(t >> 16, 0x39646B9Fu);
        U32 t2 = ucLowMul(part, md1) - ucLowMul(part >> 16, seed[2]);
        U32 tmp = ucLowMul(t2, 0x59C3AF2Du) - ucLowMul(t2 >> 16, 0x2232E0F1u);
        acc = ucLowMul(t2, 0x2A18AF2Du) + ucLowMul(t2 >> 16, 0x02941F0Fu) + ucLowMul(tmp >> 16, 0x35BD1EC9u);
        sum += part + acc;
    }
    out[0] = acc; out[1] = sum;
}
static void ucMixB(const U32* w, int n, const U32 md[4], const U32 seed[4], U32 out[2]) {
    U32 md1 = md[1] | 1u, md0 = md[0] | 1u;
    U32 sum = 0, acc = 0; const U32* p = w; int rem = n;
    U32 loops = (((U32)rem - 2u) >> 1) + 1u;
    while (loops--) {
        U32 x = p[0] + acc; rem -= 2; p += 2;
        U32 t = ucLowMul(x, md0);
        U32 a = ucLowMul(t, 0xB1110000u) - ucLowMul(t >> 16, 0x30674EEFu);
        U32 b = ucLowMul(a, seed[2]);
        U32 c = b - ucLowMul(a >> 16, 0x78F7A461u);
        c = ucLowMul(c >> 16, seed[0]) - ucLowMul(a >> 16, 0x164D0000u) - ucLowMul(b, 0x46930000u);
        U32 d = ucLowMul(c >> 16, 0x257E1D83u) + ucLowMul(c, 0x1D830000u);
        U32 e = p[-1] + d;
        U32 f = ucLowMul(e, md1);
        U32 g = ucLowMul(f, 0x16F50000u) - ucLowMul(f >> 16, 0x5D8BE90Bu);
        U32 h = ucLowMul(g >> 16, seed[3]);
        U32 i = ucLowMul(g, 0x69010000u) + h;
        U32 j = ucLowMul(i, seed[1]);
        U32 k = ((0u - h) - ucLowMul(g, 0x69010000u)) >> 16;
        U32 m = ucLowMul(k, 0x7C932B89u) - j;
        U32 nn = ucLowMul(k, 0xF2310000u) - ucLowMul(m >> 16, 0x405B6097u);
        nn += ucLowMul(j, 0x60970000u);
        acc = nn; sum += d + acc;
    }
    if (rem == 1) {
        U32 x = p[0] + acc;
        U32 t = ucLowMul(x, md0);
        U32 a = ucLowMul(t, 0xB1110000u) - ucLowMul(t >> 16, 0x30674EEFu);
        U32 b = ucLowMul(a, seed[2]);
        U32 c = ucLowMul(a >> 16, 0xE9B30000u) - ucLowMul(b, 0x46930000u) + ucLowMul((b - ucLowMul(a >> 16, 0x78F7A461u)) >> 16, seed[0]);
        U32 d = ucLowMul(c >> 16, 0x257E1D83u) + ucLowMul(c, 0x1D830000u);
        U32 e = ucLowMul(d, md1);
        U32 f = ucLowMul(e, 0x16F50000u) - ucLowMul(e >> 16, 0x5D8BE90Bu);
        U32 h = ucLowMul(f >> 16, seed[3]);
        U32 i = ucLowMul(f, 0x69010000u) + h;
        U32 j = ucLowMul(i, seed[1]);
        U32 k = ((0u - h) - ucLowMul(f, 0x69010000u)) >> 16;
        U32 m = ucLowMul(k, 0x7C932B89u) - j;
        U32 nn = ucLowMul(k, 0xF2310000u) - ucLowMul(m >> 16, 0x405B6097u);
        nn += ucLowMul(j, 0x60970000u);
        acc = nn; sum += d + acc;
    }
    out[0] = acc; out[1] = sum;
}

static BOOL ucLatestHash(const wchar_t* canonical, wchar_t out[13]) {
    static const U8 SEED_A[16] = { 0x00,0x00,0xFB,0x69,0x95,0xA3,0xF8,0x79,0x25,0xEC,0xE8,0x3C,0x05,0x96,0xFA,0x10 };
    static const U8 SEED_B[16] = { 0x6D,0xB9,0xCE,0x12,0x00,0x00,0x89,0x2B,0x00,0x00,0x9F,0x5B,0x01,0x69,0x7C,0x2C };
    U32 sa[4], sb[4];
    for (int i = 0; i < 4; i++) {
        sa[i] = SEED_A[i*4] | (SEED_A[i*4+1] << 8) | (SEED_A[i*4+2] << 16) | ((U32)SEED_A[i*4+3] << 24);
        sb[i] = SEED_B[i*4] | (SEED_B[i*4+1] << 8) | (SEED_B[i*4+2] << 16) | ((U32)SEED_B[i*4+3] << 24);
    }
    out[0] = 0;
    const UcTables* t = ucTables();
    if (!t->region || !t->longRw || !t->fallback || !t->byteClass || !t->bitWidth || !t->shortRw) return FALSE;
    size_t n = wcslen(canonical);
    wchar_t* low = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!low) return FALSE;
    for (size_t i = 0; i <= n; i++) low[i] = (wchar_t)towlower(canonical[i]);
    const U16* in = (const U16*)low;

    UcEnc s; memset(&s, 0, sizeof(s));
    s.e = (U64*)calloc(4096, sizeof(U64)); s.cap = s.e ? 4096 : 0;
    s.count = 1; s.last = UC_SENTINEL;
    if (!s.e) { free(low); return FALSE; }

    // first pass, over most of the text
    const U16* end = in + n; const U16* cur = in;
    U32 first = 0;
    U64 r = (2249744775ULL * (U32)n) >> 32;
    U64 quota = (r + ((n - (size_t)r) >> 1)) >> 7;
    while ((U32)quota > 1u) {
        U32 limit = (U32)quota - 1u, done = 0;
        while (done < limit) {
            U64 tok; U8 kind, len;
            if (!ucToken1(t, cur, end, &tok, &kind, &len)) break;
            first += len; cur += len;
            ucEmit1(&s, t, &cur, end, tok, kind);
            ++done;
        }
        r = (2249744775ULL * (U32)(end - cur)) >> 32;
        quota = ((U32)r + (((U32)(end - cur) - (U32)r) >> 1)) >> 7;
    }
    // second pass, over the rest
    cur = in + first;
    while (cur < end) {
        U64 tok; U8 kind, len;
        if (!ucToken2(t, cur, end, &tok, &kind, &len)) break;
        s.total += len; cur += len;
        ucEmit2(&s, t, &cur, tok, kind);
    }
    ucFlush(&s);
    free(low);
    if (s.oom) { free(s.e); return FALSE; }
    s.e[0] = (U64)s.total | (s.count << 32);

    U16* packed = (U16*)malloc((size_t)(s.count * 4 + 2) * sizeof(U16));
    if (!packed) { free(s.e); return FALSE; }
    size_t np = 0;
    for (U64 i = 0; i < s.count; i++)
        for (int sh = 0; sh < 64; sh += 16) { U16 part = (U16)(s.e[i] >> sh); if (part) packed[np++] = part; }
    packed[np++] = 0;
    free(s.e);

    U32 md[4] = { 0 };
    BOOL ok = ucMd5(packed, (DWORD)(np * 2), md);
    int byteLen = (int)(np * 2), dwLen = byteLen >> 2;
    int mixCount = (byteLen & 4) == 0 ? dwLen : dwLen - 1;
    size_t nd = (np + 1) / 2;
    U32* dw = (U32*)calloc(nd + 1, sizeof(U32));
    if (!dw) ok = FALSE;
    for (size_t i = 0; ok && i < nd; i++) dw[i] = packed[i * 2] | ((i * 2 + 1 < np ? (U32)packed[i * 2 + 1] : 0u) << 16);
    U32 pa[2] = { 0 }, pb[2] = { 0 };
    if (ok && mixCount >= 2) { ucMixA(dw, mixCount, md, sa, pa); ucMixB(dw, mixCount, md, sb, pb); }
    free(dw); free(packed);
    if (!ok) return FALSE;
    U32 lo = pa[0] ^ pb[0], hi = pa[1] ^ pb[1];
    U8 f8[8];
    for (int i = 0; i < 4; i++) { f8[i] = (U8)(lo >> (8 * i)); f8[4 + i] = (U8)(hi >> (8 * i)); }
    ucBase64(f8, out);
    return TRUE;
}

// ---------- what goes into the hashes ----------
static BOOL ucUserSid(wchar_t* out, int cch) {
    HANDLE tok; BYTE buf[256]; DWORD n = 0; BOOL ok = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
        LPWSTR s = NULL;
        if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) { lstrcpynW(out, s, cch); LocalFree(s); ok = TRUE; }
    }
    CloseHandle(tok);
    return ok;
}

static BOOL ucMachineId(wchar_t* out, int cch) {
    wchar_t v[80]; DWORD cb = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\SQMClient", L"MachineID", RRF_RT_REG_SZ, NULL, v, &cb) != ERROR_SUCCESS) return FALSE;
    wchar_t* s = v; size_t n = wcslen(s);
    if (n >= 2 && s[0] == L'{' && s[n - 1] == L'}') { s[n - 1] = 0; s++; }
    if (!*s) return FALSE;
    lstrcpynW(out, s, cch);
    return TRUE;
}

// Has Windows moved this user to UserChoiceLatest? (It then ignores UserChoice.)
static BOOL ucUsesLatest(const wchar_t* sid) {
    wchar_t key[200]; DWORD v = 0, cb = sizeof(v);
    swprintf(key, 200, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SystemProtectedUserData\\%s\\AnyoneRead\\AppDefaults", sid);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, L"HashVersion", RRF_RT_REG_DWORD, NULL, &v, &cb) == ERROR_SUCCESS && v >= 1;
}

static BOOL ucKeyTime(HKEY root, const wchar_t* sub, FILETIME* ft) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return FALSE;
    BOOL ok = RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, ft) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

// The two time forms the hashes take: to the millisecond (newer) and to the minute (older)
static void ucTimeHex(FILETIME ft, BOOL toMinute, wchar_t out[17]) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    if (toMinute) { st.wSecond = 0; st.wMilliseconds = 0; }
    SystemTimeToFileTime(&st, &ft);
    swprintf(out, 17, L"%08x%08x", ft.dwHighDateTime, ft.dwLowDateTime);
}

static BOOL ucLatestFor(const wchar_t* ext, const wchar_t* progId, const wchar_t* sid, const wchar_t* machine,
                        FILETIME ft, wchar_t out[13]) {
    static const wchar_t* SALT[3] = {
        L"Copyright (C) Microsoft. All rights reserved {3822B7CA-C2F4-4889-B8CC-4CE39A8FB81C}",
        L"Copyright (C) Microsoft. All rights reserved {D185E0A1-E265-4724-AA21-3A17B038D72E}",
        L"Copyright (C) Microsoft. All rights reserved {97B6BCF4-C367-4577-95BE-73BD3053A5E0}" };
    wchar_t ts[17], in[700];
    ucTimeHex(ft, FALSE, ts);
    int cls = machine[wcslen(machine) - 1] % 3;
    if (cls == 0)      swprintf(in, 700, L"%s%s%s%s%s%s", SALT[0], ext, ts, machine, progId, sid);
    else if (cls == 1) swprintf(in, 700, L"%s%s%s%s%s%s", SALT[1], ts, ext, sid, machine, progId);
    else               swprintf(in, 700, L"%s%s%s%s%s%s", sid, ts, SALT[2], ext, machine, progId);
    return ucLatestHash(in, out);
}

static void ucClassicFor(const wchar_t* ext, const wchar_t* progId, const wchar_t* sid, FILETIME ft, wchar_t out[13]) {
    wchar_t ts[17], exp[160], in[700];
    ucTimeHex(ft, TRUE, ts);
    ucExperience(exp, 160);
    swprintf(in, 700, L"%s%s%s%s%s", ext, sid, progId, ts, exp);
    ucLower(in);
    ucClassicHash(in, out);
}

// ---------- checking what's there ----------
#define UC_FILEEXTS L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\"

static BOOL ucReadString(HKEY root, const wchar_t* sub, const wchar_t* name, wchar_t* out, DWORD cch) {
    DWORD cb = cch * sizeof(wchar_t);
    return RegGetValueW(root, sub, name, RRF_RT_REG_SZ, NULL, out, &cb) == ERROR_SUCCESS;
}

// Does this extension's UserChoiceLatest (latest) or UserChoice name progId with a hash Windows will accept?
static BOOL ucCheck(const wchar_t* ext, const wchar_t* progId, BOOL latest) {
    wchar_t sid[100], machine[80], key[300], pk[300], have[200], hash[40], want[13];
    if (!ucUserSid(sid, 100)) return FALSE;
    FILETIME ft;
    if (latest) {
        if (!ucMachineId(machine, 80)) return FALSE;
        swprintf(key, 300, UC_FILEEXTS L"%s\\UserChoiceLatest", ext);
        swprintf(pk, 300, L"%s\\ProgId", key);
        if (!ucReadString(HKEY_CURRENT_USER, pk, L"ProgId", have, 200) || !ucReadString(HKEY_CURRENT_USER, key, L"Hash", hash, 40)
            || !ucKeyTime(HKEY_CURRENT_USER, pk, &ft)) return FALSE;
        if (progId && _wcsicmp(have, progId)) return FALSE;
        return ucLatestFor(ext, have, sid, machine, ft, want) && !wcscmp(want, hash);
    }
    swprintf(key, 300, UC_FILEEXTS L"%s\\UserChoice", ext);
    if (!ucReadString(HKEY_CURRENT_USER, key, L"ProgId", have, 200) || !ucReadString(HKEY_CURRENT_USER, key, L"Hash", hash, 40)
        || !ucKeyTime(HKEY_CURRENT_USER, key, &ft)) return FALSE;
    if (progId && _wcsicmp(have, progId)) return FALSE;
    ucClassicFor(ext, have, sid, ft, want);
    return !wcscmp(want, hash);
}

// ---------- writing a choice ----------
// Windows puts a deny entry on these keys, so they're deleted and made again rather than edited.
static DWORD ucWriteClassic(const wchar_t* ext, const wchar_t* progId, const wchar_t* sid) {
    wchar_t key[300], hash[13];
    swprintf(key, 300, UC_FILEEXTS L"%s\\UserChoice", ext);
    for (int attempt = 0; attempt < 3; attempt++) {
        LSTATUS e = RegDeleteTreeW(HKEY_CURRENT_USER, key);
        if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return (DWORD)e;
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ucClassicFor(ext, progId, sid, now, hash);
        HKEY k;
        e = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &k, NULL);
        if (e != ERROR_SUCCESS) return (DWORD)e;
        e = RegSetValueExW(k, L"ProgId", 0, REG_SZ, (const BYTE*)progId, (DWORD)(wcslen(progId) + 1) * 2);
        if (e == ERROR_SUCCESS) e = RegSetValueExW(k, L"Hash", 0, REG_SZ, (const BYTE*)hash, (DWORD)(wcslen(hash) + 1) * 2);
        RegCloseKey(k);
        if (e != ERROR_SUCCESS) return (DWORD)e;
        if (ucCheck(ext, progId, FALSE)) return 0;      // a new minute began between the hash and the write: again
    }
    return ERROR_INVALID_DATA;
}

static DWORD ucWriteLatest(const wchar_t* ext, const wchar_t* progId, const wchar_t* sid) {
    wchar_t machine[80], key[300], pk[300], hash[13];
    if (!ucMachineId(machine, 80)) return ERROR_FILE_NOT_FOUND;
    swprintf(key, 300, UC_FILEEXTS L"%s\\UserChoiceLatest", ext);
    swprintf(pk, 300, L"%s\\ProgId", key);
    LSTATUS e = RegDeleteTreeW(HKEY_CURRENT_USER, key);
    if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) return (DWORD)e;
    HKEY k, p;
    e = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE | KEY_CREATE_SUB_KEY, NULL, &k, NULL);
    if (e != ERROR_SUCCESS) return (DWORD)e;
    e = RegCreateKeyExW(k, L"ProgId", 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &p, NULL);
    if (e == ERROR_SUCCESS) {
        e = RegSetValueExW(p, L"ProgId", 0, REG_SZ, (const BYTE*)progId, (DWORD)(wcslen(progId) + 1) * 2);
        RegCloseKey(p);
    }
    FILETIME ft;
    if (e == ERROR_SUCCESS && !ucKeyTime(HKEY_CURRENT_USER, pk, &ft)) e = ERROR_CANTREAD;
    if (e == ERROR_SUCCESS && !ucLatestFor(ext, progId, sid, machine, ft, hash)) e = ERROR_OUTOFMEMORY;
    if (e == ERROR_SUCCESS) e = RegSetValueExW(k, L"Hash", 0, REG_SZ, (const BYTE*)hash, (DWORD)(wcslen(hash) + 1) * 2);
    RegCloseKey(k);
    if (e != ERROR_SUCCESS) return (DWORD)e;
    return ucCheck(ext, progId, TRUE) ? 0 : ERROR_INVALID_DATA;
}

// Makes progId the current user's default for ext (".mp4"). Returns 0 or a Windows error.
static DWORD assocSetDefault(const wchar_t* ext, const wchar_t* progId) {
    wchar_t sid[100], key[300];
    if (!ucUserSid(sid, 100)) return GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED;
    // listed under the type's "Open with" choices, and no "new app" notice for it
    swprintf(key, 300, UC_FILEEXTS L"%s\\OpenWithProgids", ext);
    RegSetKeyValueW(HKEY_CURRENT_USER, key, progId, REG_NONE, NULL, 0);
    wchar_t toast[200]; DWORD zero = 0;
    swprintf(toast, 200, L"%s_%s", progId, ext);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ApplicationAssociationToasts",
                    toast, REG_DWORD, &zero, sizeof(zero));

    DWORD e = ucWriteClassic(ext, progId, sid);
    if (!e && ucUsesLatest(sid)) e = ucWriteLatest(ext, progId, sid);
    return e;
}
