// What the Download Windows page and the Manual Updates page both use to talk to UUP dump: a small JSON reader,
// HTTP(S) downloads, the API call with its rate-limit retry, and file checksums. Kept apart from download.c so
// that page can still be removed on its own.

#include <winhttp.h>
#include <winioctl.h>

#define DL_API L"https://api.uupdump.net/"
#define DL_LINKS_GAP_MS 15000     // real download links are limited harder than lists: 3 seconds apart gets refused

// ---------- JSON: just enough to walk the API's replies, ignoring anything it adds later ----------
typedef struct { const char* p; BOOL obj; } JIter;

static const char* jWs(const char* p) { while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; return p; }
static const char* jSkipString(const char* p) {
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
}
static const char* jSkip(const char* p) {
    p = jWs(p);
    if (*p == '"') return jSkipString(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        do {
            if (*p == '"') { p = jSkipString(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') depth--;
            if (*p) p++;
        } while (*p && depth > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
static BOOL jOpen(const char* v, JIter* it) {
    v = jWs(v);
    if (!v || (*v != '{' && *v != '[')) return FALSE;
    it->obj = (*v == '{'); it->p = v + 1;
    return TRUE;
}
// key is NULL when walking an array
static BOOL jNext(JIter* it, const char** key, const char** val) {
    const char* p = jWs(it->p);
    while (*p == ',') p = jWs(p + 1);
    if (!*p || *p == '}' || *p == ']') return FALSE;
    if (it->obj) {
        if (*p != '"') return FALSE;
        *key = p;
        p = jWs(jSkipString(p));
        if (*p != ':') return FALSE;
        p = jWs(p + 1);
    } else {
        *key = NULL;
    }
    *val = p;
    it->p = jSkip(p);
    return TRUE;
}
static BOOL jKeyIs(const char* key, const char* name) {
    size_t n = strlen(name);
    return key && key[0] == '"' && !strncmp(key + 1, name, n) && key[n + 1] == '"';
}
static const char* jGet(const char* obj, const char* name) {
    JIter it; const char* k; const char* v;
    if (!obj || !jOpen(obj, &it) || !it.obj) return NULL;
    while (jNext(&it, &k, &v)) if (jKeyIs(k, name)) return v;
    return NULL;
}
// Copies a string or number as text. null and anything else gives an empty string.
static void jText(const char* v, wchar_t* out, int cch) {
    out[0] = 0;
    v = jWs(v);
    if (!v) return;
    char* b = (char*)malloc(32768);
    if (!b) return;
    int n = 0;
    if (*v == '"') {
        for (const char* p = v + 1; *p && *p != '"' && n < 32760; p++) {
            if (*p != '\\') { b[n++] = *p; continue; }
            p++;
            if (!*p) break;
            switch (*p) {
            case 'n': b[n++] = '\n'; break;
            case 'r': b[n++] = '\r'; break;
            case 't': b[n++] = '\t'; break;
            case 'b': case 'f': break;
            case 'u': {
                unsigned c = 0; int i;
                for (i = 1; i <= 4 && isxdigit((unsigned char)p[i]); i++)
                    c = c * 16 + (isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10));
                if (i <= 4) { p += i - 1; break; }
                p += 4;
                if (c < 0x80) b[n++] = (char)c;
                else if (c < 0x800) { b[n++] = (char)(0xC0 | (c >> 6)); b[n++] = (char)(0x80 | (c & 63)); }
                else { b[n++] = (char)(0xE0 | (c >> 12)); b[n++] = (char)(0x80 | ((c >> 6) & 63)); b[n++] = (char)(0x80 | (c & 63)); }
                break;
            }
            default: b[n++] = *p;
            }
        }
    } else if (isdigit((unsigned char)*v) || *v == '-') {
        for (const char* p = v; *p && *p != ',' && *p != '}' && *p != ']' && n < 64; p++) b[n++] = *p;
    }
    b[n] = 0;
    int w = MultiByteToWideChar(CP_UTF8, 0, b, -1, out, cch);
    if (!w) out[0] = 0;
    out[cch - 1] = 0;
    free(b);
}

// ---------- HTTP ----------
// Fetches a URL into memory (body) or a file (path). Returns 0 and sets *status, or a Windows error.
// Only https, unless allowHttp: Microsoft's file links are often plain http, so a caller allowing it must check
// the file against the checksum UUP dump lists.
// progress, if given, is told the bytes received so far after each read; returning FALSE stops with ERROR_CANCELLED.
typedef BOOL (*HttpProgress)(unsigned long long got, void* ctx);
// The same with a verb (NULL = GET; "HEAD" reads no body), extra request headers ("Name: value\r\n...") and, if given,
// the length the server announces in *length (0 when it doesn't say).
static DWORD dlHttpReq(const wchar_t* url, const wchar_t* verb, const wchar_t* headers, char** body, const wchar_t* path,
                       DWORD* status, unsigned long long* length, BOOL allowHttp, HttpProgress progress, void* ctx);
static DWORD dlHttpCore(const wchar_t* url, char** body, const wchar_t* path, DWORD* status, BOOL allowHttp,
                        HttpProgress progress, void* ctx) {
    return dlHttpReq(url, NULL, NULL, body, path, status, NULL, allowHttp, progress, ctx);
}
static DWORD dlHttpReqWin(const wchar_t* url, const wchar_t* verb, const wchar_t* headers, char** body, const wchar_t* path,
                          DWORD* status, unsigned long long* length, BOOL allowHttp, HttpProgress progress, void* ctx);

// Windows' own curl.exe (in System32 since Windows 10 1803) fetching an https file, following redirects on https only.
// The fallback for a network where WinHTTP can't reach a server that curl can (seen on one ISP line with GitHub's
// download server: WinHTTP timed out every time, curl connected at once). *status gets the final HTTP status.
static DWORD dlCurlFile(const wchar_t* url, const wchar_t* path, DWORD* status) {
    wchar_t exe[MAX_PATH], cmd[4400 + MAX_PATH + 300];
    *status = 0;
    systemPath(exe, L"curl.exe");
    if (!isFile(exe)) return ERROR_FILE_NOT_FOUND;
    if (wcsncmp(url, L"https://", 8) || wcspbrk(url, L"\" \t\r\n") || wcschr(path, L'"')) return ERROR_INVALID_PARAMETER;
    swprintf(cmd, 4400 + MAX_PATH + 300, L"\"%s\" -sS -L --proto =https --proto-redir =https --connect-timeout 15 --max-time 1800 "
             L"-A WindowsChosenUtilities -o \"%s\" -w \"\\nWCUHTTP=%%{http_code}\\n\" \"%s\"", exe, path, url);
    HANDLE rd = NULL; PROCESS_INFORMATION pi;
    DWORD err = spawnPiped(exe, cmd, &rd, &pi);
    if (err) return err;
    char out[4096]; DWORD len = 0, got;
    while (ReadFile(rd, out + len, (DWORD)(sizeof(out) - 1 - len), &got, NULL) && got) {
        len += got;
        if (len >= sizeof(out) - 1) {          // keep the end, where the status line is
            memmove(out, out + len - 1024, 1024); len = 1024;
        }
    }
    out[len] = 0;
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    DWORD code = waitExitCode(pi.hProcess);
    const char* s = NULL;
    for (const char* p = strstr(out, "WCUHTTP="); p; p = strstr(p + 1, "WCUHTTP=")) s = p;
    if (s) *status = (DWORD)strtoul(s + 8, NULL, 10);
    if (code) { DeleteFileW(path); return code == 28 ? ERROR_WINHTTP_TIMEOUT : ERROR_WINHTTP_CANNOT_CONNECT; }
    return 0;
}

static DWORD dlHttpReq(const wchar_t* url, const wchar_t* verb, const wchar_t* headers, char** body, const wchar_t* path,
                       DWORD* status, unsigned long long* length, BOOL allowHttp, HttpProgress progress, void* ctx) {
    // WCU_FORCE_CURL=1 (testing only) skips WinHTTP for file downloads
    BOOL force = path && !verb && !headers && GetEnvironmentVariableW(L"WCU_FORCE_CURL", NULL, 0) > 0;
    DWORD err = force ? ERROR_WINHTTP_CANNOT_CONNECT : dlHttpReqWin(url, verb, headers, body, path, status, length, allowHttp, progress, ctx);
    BOOL noReach = err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_CONNECTION_ERROR ||
                   err == ERROR_WINHTTP_SECURE_FAILURE;
    if (noReach && path && !verb && !headers && !body && !wcsncmp(url, L"https://", 8)) {
        DWORD st = 0, e2 = dlCurlFile(url, path, &st);
        if (!e2) {
            *status = st;
            if (progress) {                      // one last tick with the size, so a bar shows it finished
                WIN32_FILE_ATTRIBUTE_DATA fa;
                if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) progress(((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, ctx);
            }
            return 0;
        }
    }
    return err;
}

static DWORD dlHttpReqWin(const wchar_t* url, const wchar_t* verb, const wchar_t* headers, char** body, const wchar_t* path,
                          DWORD* status, unsigned long long* length, BOOL allowHttp, HttpProgress progress, void* ctx) {
    BOOL head = verb && !wcscmp(verb, L"HEAD");
    if (length) *length = 0;
    URL_COMPONENTS u; memset(&u, 0, sizeof(u));
    wchar_t host[256], rest[4096];
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = rest; u.dwUrlPathLength = 4096;
    u.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) return GetLastError();
    BOOL secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(allowHttp && u.nScheme == INTERNET_SCHEME_HTTP)) return ERROR_INVALID_PARAMETER;
    wchar_t full[4400];
    swprintf(full, 4400, L"%s%s", rest, u.lpszExtraInfo && u.dwExtraInfoLength ? u.lpszExtraInfo : L"");

    DWORD err = 0;
    HINTERNET s = WinHttpOpen(L"WindowsChosenUtilities", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, verb ? verb : L"GET", full, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!r) err = GetLastError();
    if (!err) WinHttpSetTimeouts(r, 15000, 15000, 30000, 120000);
    if (!err && (!WinHttpSendRequest(r, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                 !WinHttpReceiveResponse(r, NULL)))
        err = GetLastError();
    if (!err) {
        DWORD cb = sizeof(*status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, status, &cb, WINHTTP_NO_HEADER_INDEX);
        wchar_t cl[32]; cb = sizeof(cl);
        if (length && WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cb, WINHTTP_NO_HEADER_INDEX))
            *length = _wcstoui64(cl, NULL, 10);
    }
    HANDLE f = INVALID_HANDLE_VALUE;
    char* mem = NULL; size_t len = 0, cap = 0;
    unsigned long long total = 0;
    if (!err && path && !head) {
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) err = GetLastError();
    }
    while (!err && !head) {
        char buf[16384]; DWORD got = 0;
        if (!WinHttpReadData(r, buf, sizeof(buf), &got)) { err = GetLastError(); break; }
        if (!got) break;
        if (path) {
            DWORD wr;
            if (!WriteFile(f, buf, got, &wr, NULL)) err = GetLastError();
        } else {
            if (len + got + 1 > cap) {
                size_t nc = cap ? cap * 2 : 65536;
                while (nc < len + got + 1) nc *= 2;
                if (nc > (64u << 20)) { err = ERROR_FILE_TOO_LARGE; break; }
                char* m = (char*)realloc(mem, nc);
                if (!m) { err = ERROR_OUTOFMEMORY; break; }
                mem = m; cap = nc;
            }
            memcpy(mem + len, buf, got);
            len += got;
        }
        total += got;
        if (!err && progress && !progress(total, ctx)) err = ERROR_CANCELLED;
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!err && body) {
        if (!mem) mem = (char*)calloc(1, 1);
        else mem[len] = 0;
        *body = mem;
    } else {
        free(mem);
    }
    return err;
}

static DWORD dlHttpEx(const wchar_t* url, char** body, const wchar_t* path, DWORD* status, BOOL allowHttp) {
    return dlHttpCore(url, body, path, status, allowHttp, NULL, NULL);
}

static DWORD dlHttp(const wchar_t* url, char** body, const wchar_t* path, DWORD* status) {
    return dlHttpEx(url, body, path, status, FALSE);
}

// ---------- one file in parallel sections ----------
// The file is split into DL_SECTIONS equal parts, each fetched over its own connection (HTTP Range) straight into its
// place. A section whose connection breaks carries on where it stopped. If the link itself is refused (Microsoft's
// links expire), the caller is asked for a fresh one.
#define DL_SECTIONS 8
typedef BOOL (*DlFreshLink)(wchar_t* url /* 4096 */, void* ctx);
typedef struct {
    wchar_t url[4096];
    ULONGLONG urlTick;            // when the link was last replaced
    const wchar_t* path;
    const wchar_t* headers;       // extra request headers ("Name: value\r\n..."), or NULL
    BOOL allowHttp;
    DlFreshLink fresh;
    void* ctx;
    CRITICAL_SECTION lock;
    volatile LONG64 got;
    volatile LONG stop, noRange;
} DlPar;
typedef struct { DlPar* p; unsigned long long from, len, done; DWORD err, status; } DlSection;

// One try: fetches what is left of the section. Returns 0 once it is complete.
static DWORD dlSectionTry(DlSection* s, HANDLE f) {
    DlPar* p = s->p;
    wchar_t url[4096];
    EnterCriticalSection(&p->lock);
    lstrcpynW(url, p->url, 4096);
    LeaveCriticalSection(&p->lock);
    URL_COMPONENTS u; memset(&u, 0, sizeof(u));
    wchar_t host[256], rest[4096], hdr[400];
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = rest; u.dwUrlPathLength = 4096;
    u.dwExtraInfoLength = (DWORD)-1;
    s->status = 0;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) return GetLastError();
    BOOL secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && !(p->allowHttp && u.nScheme == INTERNET_SCHEME_HTTP)) return ERROR_INVALID_PARAMETER;
    wchar_t full[4400];
    swprintf(full, 4400, L"%s%s", rest, u.lpszExtraInfo && u.dwExtraInfoLength ? u.lpszExtraInfo : L"");
    swprintf(hdr, 400, L"Range: bytes=%llu-%llu\r\n%s", s->from + s->done, s->from + s->len - 1, p->headers ? p->headers : L"");

    DWORD err = 0;
    HINTERNET w = WinHttpOpen(L"WindowsChosenUtilities", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = w ? WinHttpConnect(w, host, u.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", full, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!r) err = GetLastError();
    if (!err) WinHttpSetTimeouts(r, 15000, 15000, 30000, 30000);
    if (!err && (!WinHttpSendRequest(r, hdr, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL)))
        err = GetLastError();
    if (!err) {
        DWORD cb = sizeof(s->status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &s->status, &cb, WINHTTP_NO_HEADER_INDEX);
        if (s->status == 200) InterlockedExchange(&p->noRange, 1);     // the server sends the whole file or nothing
        if (s->status != 206) err = ERROR_BAD_NET_RESP;
    }
    char* buf = err ? NULL : (char*)malloc(1 << 16);
    if (!err && !buf) err = ERROR_OUTOFMEMORY;
    while (!err && s->done < s->len) {
        DWORD got = 0, wr = 0;
        if (p->stop || p->noRange) { err = ERROR_CANCELLED; break; }
        if (!WinHttpReadData(r, buf, 1 << 16, &got)) { err = GetLastError(); break; }
        if (!got) { err = ERROR_HANDLE_EOF; break; }
        if (got > s->len - s->done) { err = ERROR_BAD_NET_RESP; break; }
        OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
        unsigned long long at = s->from + s->done;
        ov.Offset = (DWORD)at; ov.OffsetHigh = (DWORD)(at >> 32);
        if (!WriteFile(f, buf, got, &wr, &ov) || wr != got) { err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT; p->stop = 2; break; }
        s->done += got;
        InterlockedExchangeAdd64(&p->got, got);
    }
    free(buf);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (w) WinHttpCloseHandle(w);
    return err;
}

static DWORD WINAPI dlSectionProc(LPVOID arg) {
    DlSection* s = (DlSection*)arg;
    DlPar* p = s->p;
    HANDLE f = CreateFileW(p->path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { s->err = GetLastError(); return 0; }
    int fails = 0;
    while (s->done < s->len && !p->stop && !p->noRange) {
        unsigned long long before = s->done;
        ULONGLONG started = GetTickCount64();
        s->err = dlSectionTry(s, f);
        if (!s->err || p->stop || p->noRange) break;
        fails = s->done > before ? 1 : fails + 1;                      // progress starts the count again
        if (fails > 6) break;
        if ((s->status == 403 || s->status == 404 || s->status == 410) && p->fresh) {
            EnterCriticalSection(&p->lock);
            if (p->urlTick < started && !p->stop) {                    // not replaced by another section meanwhile
                wchar_t url[4096];
                if (p->fresh(url, p->ctx)) lstrcpynW(p->url, url, 4096);
                p->urlTick = GetTickCount64();
            }
            LeaveCriticalSection(&p->lock);
        }
        for (int i = 0; i < fails * 8 && !p->stop && !p->noRange; i++) Sleep(250);
    }
    CloseHandle(f);
    return 0;
}

// Downloads size bytes to path in parts (1 to DL_SECTIONS) sections, sending headers with each. Returns 0, ERROR_CANCELLED,
// or an error with *status when it was the server's answer. ERROR_NOT_SUPPORTED: the server doesn't do sections, and
// nothing is left behind.
static DWORD dlHttpParallelEx(const wchar_t* url, const wchar_t* headers, int parts, const wchar_t* path, unsigned long long size,
                              DWORD* status, BOOL allowHttp, HttpProgress progress, DlFreshLink fresh, void* ctx) {
    *status = 0;
    if (parts < 1) parts = 1;
    if (parts > DL_SECTIONS) parts = DL_SECTIONS;
    if (size < (unsigned long long)parts) return ERROR_NOT_SUPPORTED;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    // Sparse while it fills: otherwise the first write near the end makes Windows zero everything before it
    DWORD n;
    DeviceIoControl(f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &n, NULL);
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)size;
    DWORD err = (SetFilePointerEx(f, li, NULL, FILE_BEGIN) && SetEndOfFile(f)) ? 0 : GetLastError();

    DlPar* p = (DlPar*)calloc(1, sizeof(DlPar));
    DlSection s[DL_SECTIONS]; HANDLE t[DL_SECTIONS]; int started = 0;
    memset(s, 0, sizeof(s));
    if (!p) err = ERROR_OUTOFMEMORY;
    if (!err) {
        lstrcpynW(p->url, url, 4096);
        p->path = path; p->headers = headers; p->allowHttp = allowHttp; p->fresh = fresh; p->ctx = ctx;
        InitializeCriticalSection(&p->lock);
        unsigned long long each = size / parts;
        for (int i = 0; i < parts; i++) {
            s[i].p = p;
            s[i].from = each * i;
            s[i].len = (i == parts - 1) ? size - each * i : each;
            t[started] = CreateThread(NULL, 0, dlSectionProc, &s[i], 0, NULL);
            if (!t[started]) { err = GetLastError(); InterlockedExchange(&p->stop, 1); break; }
            started++;
        }
        while (started && WaitForMultipleObjects(started, t, TRUE, 250) == WAIT_TIMEOUT) {
            if (progress && !progress((unsigned long long)p->got, ctx)) InterlockedExchange(&p->stop, 1);
            for (int i = 0; i < DL_SECTIONS && !p->stop; i++)          // one section given up: the rest is pointless
                if (i < started && s[i].err && s[i].done < s[i].len && WaitForSingleObject(t[i], 0) == WAIT_OBJECT_0) InterlockedExchange(&p->stop, 2);
        }
        for (int i = 0; i < started; i++) CloseHandle(t[i]);
        if (!err && p->noRange) err = ERROR_NOT_SUPPORTED;
        else if (!err && p->stop == 1) err = ERROR_CANCELLED;
        for (int i = 0; i < DL_SECTIONS && !err; i++)
            if (s[i].done < s[i].len && s[i].err != ERROR_CANCELLED) { err = s[i].err ? s[i].err : ERROR_HANDLE_EOF; *status = s[i].status; }
        for (int i = 0; i < DL_SECTIONS && !err; i++) if (s[i].done < s[i].len) err = ERROR_HANDLE_EOF;
        if (!err && progress) progress((unsigned long long)p->got, ctx);
        DeleteCriticalSection(&p->lock);
    }
    free(p);
    if (!err) {                                                        // an ordinary file again
        FILE_SET_SPARSE_BUFFER sb = { FALSE };
        DeviceIoControl(f, FSCTL_SET_SPARSE, &sb, sizeof(sb), NULL, 0, &n, NULL);
    }
    CloseHandle(f);
    if (err) DeleteFileW(path);
    return err;
}
static DWORD dlHttpParallel(const wchar_t* url, const wchar_t* path, unsigned long long size, DWORD* status, BOOL allowHttp,
                            HttpProgress progress, DlFreshLink fresh, void* ctx) {
    return dlHttpParallelEx(url, NULL, DL_SECTIONS, path, size, status, allowHttp, progress, fresh, ctx);
}

// Calls the API and returns its "response" part inside *body, which the caller frees. If the API is busy it waits
// and tries again. On failure returns NULL and says why in err.
static const char* dlApi(const wchar_t* query, char** body, wchar_t* err, int cch) {
    wchar_t url[1024];
    swprintf(url, 1024, L"%s%s", DL_API, query);
    *body = NULL;
    for (int attempt = 0; attempt < 10; attempt++) {
        DWORD status = 0;
        char* b = NULL;
        DWORD e = dlHttp(url, &b, NULL, &status);
        if (e) {
            swprintf(err, cch, L"Couldn't reach UUP dump (error %lu). Check the connection, or try again later.", e);
            return NULL;
        }
        if (status == 429) {            // told to slow down
            free(b);
            pauseMs(20000);
            continue;
        }
        const char* resp = jGet(b, "response");
        const char* apiErr = jGet(resp, "error");
        if (status != 200 || !resp || apiErr) {
            wchar_t what[120] = L"no details";
            if (apiErr) jText(apiErr, what, 120);
            if (!wcscmp(what, L"EMPTY_FILELIST"))
                swprintf(err, cch, L"This entry has no downloadable files for that choice (EMPTY_FILELIST). If the build number has other entries, try another one.");
            else
                swprintf(err, cch, L"UUP dump couldn't answer (HTTP %lu: %s).", status, what);
            free(b);
            return NULL;
        }
        *body = b;
        return resp;
    }
    swprintf(err, cch, L"UUP dump is limiting requests right now. Try again in a few minutes.");
    return NULL;
}

// A file's SHA-256 or SHA-1 (or SHA-512, into 129) as lowercase hex. The window and the job can both be hashing, so each call has its own buffer.
static BOOL dlHashFile(const wchar_t* path, ALG_ID alg, wchar_t* hex /* 65 */) {
    hex[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BYTE* buf = (BYTE*)malloc(1 << 18);
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0; BOOL ok = FALSE;
    if (buf && CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) && CryptCreateHash(prov, alg, 0, 0, &h)) {
        DWORD got;
        ok = TRUE;
        while (ok && ReadFile(f, buf, 1 << 18, &got, NULL) && got) ok = CryptHashData(h, buf, got, 0);
        BYTE d[64]; DWORD n = 64;
        if (ok && CryptGetHashParam(h, HP_HASHVAL, d, &n, 0)) {
            for (DWORD i = 0; i < n; i++) swprintf(hex + i * 2, 3, L"%02x", d[i]);
        } else ok = FALSE;
    }
    if (h) CryptDestroyHash(h);
    if (prov) CryptReleaseContext(prov, 0);
    free(buf);
    CloseHandle(f);
    return ok;
}

static BOOL dlSha256(const wchar_t* path, wchar_t* hex /* 65 */) { return dlHashFile(path, CALG_SHA_256, hex); }

static BOOL dlHex64(const wchar_t* s) {
    int n = 0;
    for (; *s; s++, n++) if (!iswxdigit(*s)) return FALSE;
    return n == 64;
}

// ---------- small files and cabinets ----------
static char* dlReadSmallFile(const wchar_t* path, DWORD maxBytes) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    char* t = (size != INVALID_FILE_SIZE && size <= maxBytes) ? (char*)malloc((size_t)size + 1) : NULL;
    if (t && (!ReadFile(f, t, size, &got, NULL) || got != size)) { free(t); t = NULL; }
    if (t) t[size] = 0;
    CloseHandle(f);
    return t;
}

// Windows' own expand.exe, hidden. Its exit code isn't reliable, so callers look for the files it should have made.
static void dlExpand(const wchar_t* args) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH * 4];
    systemPath(exe, L"expand.exe");
    swprintf(cmd, MAX_PATH * 4, L"\"%s\" %s", exe, args);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return;
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
}
