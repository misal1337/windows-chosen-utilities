// Boot to USB: the rail button, from the standalone Boot to USB app.
//
// Finding a bootable USB drive and making the firmware start it once.
//
// "Bootable" = a FAT partition on a USB (or SD) disk with \EFI\BOOT\BOOTX64.EFI, the file UEFI firmware
// starts from removable media (Rufus, Ventoy, the Windows Media Creation Tool all put one there).
//
// Booting it once: a firmware boot entry (Boot####) for that partition and file is written, added at the
// END of BootOrder (Windows stays first), and BootNext is set to it. The firmware uses BootNext for one
// start and deletes it, so every start after that follows BootOrder again = Windows. The extra entry is
// removed by a one-shot startup task the next time Windows starts (--usbboot-cleanup).

#define UB_EFI_GLOBAL L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}"
#define UB_ENTRY_DESC L"Boot to USB (one time)"
#define UB_REG_KEY L"SOFTWARE\\Boot to USB"
#define UB_TASK_NAME L"Boot to USB cleanup"

typedef struct {
    wchar_t vol[64];        // \\?\Volume{GUID}\ of the FAT partition holding BOOTX64.EFI
    wchar_t name[160];      // "SanDisk Ultra"
    wchar_t letters[32];    // drive letters of the disk's other volumes, "E:"
    ULONGLONG size;         // whole disk, bytes
    DWORD disk;
} UbDrive;

#define UB_MAX 8
static UbDrive g_ubDrive[UB_MAX];
static int g_ubN;

static void ubTrimRight(wchar_t* s) {
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
}
static void ubTrimLeft(wchar_t* s) {
    size_t i = 0;
    while (s[i] == L' ') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof *s);
}

// a volume path for CreateFile: no trailing backslash
static void ubDevPath(const wchar_t* vol, wchar_t* out, size_t cap) {
    wcsncpy(out, vol, cap - 1);
    out[cap - 1] = 0;
    size_t n = wcslen(out);
    if (n && out[n - 1] == L'\\') out[n - 1] = 0;
}

static BOOL ubDiskNumber(HANDLE h, DWORD* disk) {
    STORAGE_DEVICE_NUMBER n;
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &n, sizeof n, &got, NULL)) return FALSE;
    if (n.DeviceType != FILE_DEVICE_DISK) return FALSE;
    *disk = n.DeviceNumber;
    return TRUE;
}

// TRUE for a USB/SD bus; fills the vendor + product name
static BOOL ubUsbBus(HANDLE h, wchar_t* name, size_t cap) {
    STORAGE_PROPERTY_QUERY q;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024];
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, NULL)) return FALSE;
    STORAGE_DEVICE_DESCRIPTOR* d = (STORAGE_DEVICE_DESCRIPTOR*)buf;
    if (d->BusType != BusTypeUsb && d->BusType != BusTypeSd && d->BusType != BusTypeMmc) return FALSE;
    wchar_t vendor[80] = L"", product[80] = L"";
    if (d->VendorIdOffset && d->VendorIdOffset < got) MultiByteToWideChar(CP_ACP, 0, (char*)buf + d->VendorIdOffset, -1, vendor, 80);
    if (d->ProductIdOffset && d->ProductIdOffset < got) MultiByteToWideChar(CP_ACP, 0, (char*)buf + d->ProductIdOffset, -1, product, 80);
    ubTrimLeft(vendor); ubTrimRight(vendor); ubTrimLeft(product); ubTrimRight(product);
    // some bridges repeat the vendor in the product ("Meetiger Meetiger")
    if (vendor[0] && _wcsnicmp(product, vendor, wcslen(vendor)) != 0) swprintf(name, cap, L"%s %s", vendor, product);
    else wcsncpy(name, product[0] ? product : vendor, cap - 1);
    name[cap - 1] = 0;
    if (!name[0]) wcsncpy(name, L"USB drive", cap - 1);
    return TRUE;
}

static HANDLE ubOpenDisk(DWORD disk, DWORD access) {
    wchar_t p[40];
    swprintf(p, 40, L"\\\\.\\PhysicalDrive%lu", disk);
    return CreateFileW(p, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL ubDiskGeometry(DWORD disk, ULONGLONG* size, DWORD* sector) {
    HANDLE h = ubOpenDisk(disk, 0);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BYTE buf[256];
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, buf, sizeof buf, &got, NULL);
    CloseHandle(h);
    if (!ok) return FALSE;
    DISK_GEOMETRY_EX* g = (DISK_GEOMETRY_EX*)buf;
    if (size) *size = (ULONGLONG)g->DiskSize.QuadPart;
    if (sector) *sector = g->Geometry.BytesPerSector;
    return TRUE;
}

static void ubScanUsb(void) {
    g_ubN = 0;
    wchar_t vol[MAX_PATH];
    HANDLE f = FindFirstVolumeW(vol, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return;
    // first pass: the bootable FAT partitions on USB disks
    do {
        wchar_t dev[MAX_PATH];
        ubDevPath(vol, dev, MAX_PATH);
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        wchar_t name[160];
        DWORD disk = 0;
        BOOL usb = ubUsbBus(h, name, 160) && ubDiskNumber(h, &disk);
        CloseHandle(h);
        if (!usb) continue;
        BOOL dup = FALSE;
        for (int i = 0; i < g_ubN; i++) if (g_ubDrive[i].disk == disk) dup = TRUE;
        if (dup || g_ubN >= UB_MAX) continue;
        wchar_t fs[32] = L"";
        if (!GetVolumeInformationW(vol, NULL, 0, NULL, NULL, NULL, fs, 32)) continue;
        if (_wcsicmp(fs, L"FAT") && _wcsicmp(fs, L"FAT32")) continue;      // what UEFI firmware can read
        wchar_t efi[MAX_PATH];
        swprintf(efi, MAX_PATH, L"%sEFI\\BOOT\\BOOTX64.EFI", vol);
        DWORD a = GetFileAttributesW(efi);
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        UbDrive* u = &g_ubDrive[g_ubN++];
        memset(u, 0, sizeof *u);
        wcsncpy(u->vol, vol, 63);
        wcsncpy(u->name, name, 159);
        u->disk = disk;
        ubDiskGeometry(disk, &u->size, NULL);
    } while (FindNextVolumeW(f, vol, MAX_PATH));
    FindVolumeClose(f);
    if (!g_ubN) return;
    // second pass: drive letters on those disks, so the user can tell which drive it is
    f = FindFirstVolumeW(vol, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t dev[MAX_PATH];
        ubDevPath(vol, dev, MAX_PATH);
        HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD disk = 0;
        BOOL ok = ubDiskNumber(h, &disk);
        CloseHandle(h);
        if (!ok) continue;
        wchar_t names[MAX_PATH];
        DWORD len = 0;
        if (!GetVolumePathNamesForVolumeNameW(vol, names, MAX_PATH, &len)) continue;
        for (int i = 0; i < g_ubN; i++) {
            if (g_ubDrive[i].disk != disk) continue;
            for (wchar_t* p = names; *p; p += wcslen(p) + 1) {
                if (wcslen(p) == 3 && p[1] == L':' && wcslen(g_ubDrive[i].letters) + 5 < 32) {
                    if (g_ubDrive[i].letters[0]) wcscat(g_ubDrive[i].letters, L" ");
                    wchar_t l[3] = { p[0], L':', 0 };
                    wcscat(g_ubDrive[i].letters, l);
                }
            }
        }
    } while (FindNextVolumeW(f, vol, MAX_PATH));
    FindVolumeClose(f);
}

static void ubSizeText(ULONGLONG b, wchar_t* out, size_t cap) {
    double gb = b / 1e9;
    if (gb >= 1000) swprintf(out, cap, L"%.1f TB", gb / 1000);
    else if (gb >= 10) swprintf(out, cap, L"%.0f GB", gb);
    else swprintf(out, cap, L"%.1f GB", gb);
}

// ---------- firmware variables ----------
static BOOL ubIsUefi(void) {
    FIRMWARE_TYPE t = FirmwareTypeUnknown;
    return GetFirmwareType(&t) && t == FirmwareTypeUefi;
}

#define UB_VAR_ATTR 0x7        // non-volatile | boot service access | runtime access

static DWORD ubReadVar(const wchar_t* name, void* buf, DWORD cap) {
    return GetFirmwareEnvironmentVariableExW(name, UB_EFI_GLOBAL, buf, cap, NULL);
}
static BOOL ubWriteVar(const wchar_t* name, const void* data, DWORD n) {
    return SetFirmwareEnvironmentVariableExW(name, UB_EFI_GLOBAL, (PVOID)data, n, UB_VAR_ATTR);
}
static BOOL ubDeleteVar(const wchar_t* name) {
    if (SetFirmwareEnvironmentVariableExW(name, UB_EFI_GLOBAL, NULL, 0, UB_VAR_ATTR)) return TRUE;
    return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
}

static void ubPutU16(BYTE* p, UINT16 v) { memcpy(p, &v, 2); }
static void ubPutU32(BYTE* p, UINT32 v) { memcpy(p, &v, 4); }
static void ubPutU64(BYTE* p, UINT64 v) { memcpy(p, &v, 8); }

// EFI_LOAD_OPTION: attributes, path list length, description, then a short-form device path
// HD(partition, start, size, signature)/\EFI\BOOT\BOOTX64.EFI/End, as Windows writes its own entry.
static DWORD ubBuildLoadOption(const wchar_t* vol, BYTE* out, DWORD cap, DWORD* err) {
    wchar_t dev[MAX_PATH];
    ubDevPath(vol, dev, MAX_PATH);
    HANDLE h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return 0; }
    PARTITION_INFORMATION_EX pi;
    DWORD got = 0, disk = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &pi, sizeof pi, &got, NULL) && ubDiskNumber(h, &disk);
    if (!ok) *err = GetLastError();
    CloseHandle(h);
    if (!ok) return 0;
    DWORD sector = 512;
    if (!ubDiskGeometry(disk, NULL, &sector) || !sector) { *err = GetLastError(); return 0; }

    BYTE hd[42];
    memset(hd, 0, sizeof hd);
    hd[0] = 0x04; hd[1] = 0x01; ubPutU16(hd + 2, 42);
    ubPutU32(hd + 4, pi.PartitionNumber);
    ubPutU64(hd + 8, (UINT64)pi.StartingOffset.QuadPart / sector);
    ubPutU64(hd + 16, (UINT64)pi.PartitionLength.QuadPart / sector);
    if (pi.PartitionStyle == PARTITION_STYLE_GPT) {
        memcpy(hd + 24, &pi.Gpt.PartitionId, 16);
        hd[40] = 2; hd[41] = 2;
    } else if (pi.PartitionStyle == PARTITION_STYLE_MBR) {
        HANDLE d = ubOpenDisk(disk, 0);
        BYTE lay[8192];
        if (d == INVALID_HANDLE_VALUE || !DeviceIoControl(d, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, lay, sizeof lay, &got, NULL)) {
            *err = GetLastError();
            if (d != INVALID_HANDLE_VALUE) CloseHandle(d);
            return 0;
        }
        CloseHandle(d);
        ubPutU32(hd + 24, ((DRIVE_LAYOUT_INFORMATION_EX*)lay)->Mbr.Signature);
        hd[40] = 1; hd[41] = 1;
    } else { *err = ERROR_NOT_SUPPORTED; return 0; }

    static const wchar_t file[] = L"\\EFI\\BOOT\\BOOTX64.EFI";
    UINT16 fileLen = (UINT16)(4 + sizeof file);
    UINT16 listLen = (UINT16)(sizeof hd + fileLen + 4);
    DWORD descLen = (DWORD)((wcslen(UB_ENTRY_DESC) + 1) * sizeof(wchar_t));
    DWORD total = 4 + 2 + descLen + listLen;
    if (total > cap) { *err = ERROR_INSUFFICIENT_BUFFER; return 0; }
    BYTE* p = out;
    ubPutU32(p, 1);                      // LOAD_OPTION_ACTIVE
    ubPutU16(p + 4, listLen);
    p += 6;
    memcpy(p, UB_ENTRY_DESC, descLen); p += descLen;
    memcpy(p, hd, sizeof hd); p += sizeof hd;
    p[0] = 0x04; p[1] = 0x04; ubPutU16(p + 2, fileLen);
    memcpy(p + 4, file, sizeof file); p += fileLen;
    p[0] = 0x7F; p[1] = 0xFF; ubPutU16(p + 2, 4);
    return total;
}

static int ubRegGetEntry(void) {
    DWORD v = 0, cb = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, UB_REG_KEY, L"Entry", RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS) return -1;
    return v <= 0xFFFF ? (int)v : -1;
}
static void ubRegSetEntry(int n) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UB_REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    DWORD v = (DWORD)n;
    RegSetValueExW(k, L"Entry", 0, REG_DWORD, (BYTE*)&v, sizeof v);
    RegCloseKey(k);
}
static void ubRegClear(void) { RegDeleteKeyW(HKEY_LOCAL_MACHINE, UB_REG_KEY); }

// is Boot#### one of ours (our description)?
static BOOL ubEntryIsOurs(int n) {
    wchar_t name[16];
    swprintf(name, 16, L"Boot%04X", n);
    BYTE buf[2048];
    DWORD got = ubReadVar(name, buf, sizeof buf);
    if (got < 6 + sizeof UB_ENTRY_DESC) return FALSE;
    return memcmp(buf + 6, UB_ENTRY_DESC, sizeof UB_ENTRY_DESC) == 0;
}

static int ubFreeEntry(void) {
    BYTE buf[16];
    for (int n = 0; n <= 0xFFFF; n++) {
        wchar_t name[16];
        swprintf(name, 16, L"Boot%04X", n);
        if (ubReadVar(name, buf, sizeof buf) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return n;
    }
    return -1;
}

// BootOrder without n, then (if add) n at the end
static BOOL ubSetInBootOrder(int n, BOOL add) {
    UINT16 order[512];
    DWORD got = ubReadVar(L"BootOrder", order, sizeof order);
    if (!got && GetLastError() != ERROR_ENVVAR_NOT_FOUND) return FALSE;
    int cnt = (int)(got / 2), w = 0;
    BOOL had = FALSE;
    for (int i = 0; i < cnt; i++) { if (order[i] == n) had = TRUE; else order[w++] = order[i]; }
    if (add) { if (w >= 512) return FALSE; order[w++] = (UINT16)n; }
    else if (!had) return TRUE;
    if (add && had && w == cnt && order[cnt - 1] == n) return TRUE;   // already last
    return ubWriteVar(L"BootOrder", order, (DWORD)w * 2);
}

static DWORD ubRunHidden(wchar_t* cmd) {
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return GetLastError();
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static void ubSysExe(const wchar_t* exe, wchar_t* out, size_t cap) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(out, cap, L"\"%s\\%s\"", sys, exe);
}

static void ubXmlEscape(const wchar_t* s, wchar_t* out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 8 < cap; s++) {
        const wchar_t* r = *s == L'&' ? L"&amp;" : *s == L'<' ? L"&lt;" : *s == L'>' ? L"&gt;" : NULL;
        if (r) { wcscpy(out + o, r); o += wcslen(r); }
        else out[o++] = *s;
    }
    out[o] = 0;
}

// The startup task that removes the entry again runs as SYSTEM, so it must not run a file the user's own programs could
// replace: it gets its own copy of this exe in Program Files, where only administrators can write.
static BOOL ubCleanupExe(wchar_t* dir, wchar_t* exe) {
    wchar_t pf[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf)) || wcslen(pf) > 200) return FALSE;
    swprintf(dir, MAX_PATH, L"%s\\Boot to USB cleanup", pf);
    swprintf(exe, MAX_PATH, L"%s\\cleanup.exe", dir);
    return TRUE;
}

static BOOL ubScheduleCleanup(void) {
    wchar_t me[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], esc[MAX_PATH * 2], file[MAX_PATH];
    GetModuleFileNameW(NULL, me, MAX_PATH);
    if (!ubCleanupExe(dir, exe)) return FALSE;
    CreateDirectoryW(dir, NULL);
    if (!CopyFileW(me, exe, FALSE)) return FALSE;
    ubXmlEscape(exe, esc, MAX_PATH * 2);
    static wchar_t xml[4096];
    swprintf(xml, 4096,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>Removes the one-time USB boot entry made by Windows Chosen Utilities, then deletes itself.</Description></RegistrationInfo>\r\n"
        L"  <Triggers><BootTrigger><Enabled>true</Enabled></BootTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable>"
        L"<ExecutionTimeLimit>PT5M</ExecutionTimeLimit><Enabled>true</Enabled></Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>%s</Command><Arguments>--usbboot-cleanup</Arguments></Exec></Actions>\r\n"
        L"</Task>\r\n", esc);
    swprintf(file, MAX_PATH, L"%s\\task.xml", dir);
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD w;
    WORD bom = 0xFEFF;
    WriteFile(h, &bom, 2, &w, NULL);
    WriteFile(h, xml, (DWORD)(wcslen(xml) * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
    wchar_t st[MAX_PATH], cmd[MAX_PATH * 2];
    ubSysExe(L"schtasks.exe", st, MAX_PATH);
    swprintf(cmd, MAX_PATH * 2, L"%s /create /f /tn \"%s\" /xml \"%s\"", st, UB_TASK_NAME, file);
    DWORD rc = ubRunHidden(cmd);
    DeleteFileW(file);
    return rc == 0;
}
static void ubUnscheduleCleanup(void) {
    wchar_t st[MAX_PATH], cmd[MAX_PATH * 2];
    ubSysExe(L"schtasks.exe", st, MAX_PATH);
    swprintf(cmd, MAX_PATH * 2, L"%s /delete /f /tn \"%s\"", st, UB_TASK_NAME);
    ubRunHidden(cmd);
}

// Exit codes of --usbboot (the window turns them into words)
enum { UB_OK = 0, UB_NOT_UEFI = 10, UB_NO_PRIV = 11, UB_NO_PART = 12, UB_NO_ENTRY = 13,
       UB_NO_NEXT = 14, UB_NO_RESTART = 15, UB_NO_FREE = 16 };

// --usbboot-cleanup: take our entry out of the firmware (and BootNext, if it's still there unused)
static int ubCleanupEntry(void) {
    if (!ubIsUefi() || !enablePrivilege(SE_SYSTEM_ENVIRONMENT_NAME)) return 1;
    int n = ubRegGetEntry();
    if (n >= 0 && ubEntryIsOurs(n)) {
        UINT16 next = 0;
        if (ubReadVar(L"BootNext", &next, sizeof next) == 2 && next == n) ubDeleteVar(L"BootNext");
        ubSetInBootOrder(n, FALSE);
        wchar_t name[16];
        swprintf(name, 16, L"Boot%04X", n);
        ubDeleteVar(name);
    }
    ubRegClear();
    ubUnscheduleCleanup();
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (ubCleanupExe(dir, exe)) {                         // the copy, which may be the one running: gone at the next restart
        if (!DeleteFileW(exe)) MoveFileExW(exe, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (!RemoveDirectoryW(dir) && GetLastError() == ERROR_DIR_NOT_EMPTY) MoveFileExW(dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    return 0;
}

// --usbboot=<volume>: entry + BootNext, then restart (unless noRestart, for testing)
static int ubBootOnce(const wchar_t* vol, BOOL noRestart) {
    if (!ubIsUefi()) return UB_NOT_UEFI;
    if (!enablePrivilege(SE_SYSTEM_ENVIRONMENT_NAME)) return UB_NO_PRIV;
    BYTE opt[1024];
    DWORD err = 0;
    DWORD n = ubBuildLoadOption(vol, opt, sizeof opt, &err);
    if (!n) return UB_NO_PART;
    int num = ubRegGetEntry();
    if (num < 0 || !ubEntryIsOurs(num)) num = ubFreeEntry();
    if (num < 0) return UB_NO_FREE;
    wchar_t name[16];
    swprintf(name, 16, L"Boot%04X", num);
    if (!ubWriteVar(name, opt, n)) return UB_NO_ENTRY;
    ubRegSetEntry(num);
    ubSetInBootOrder(num, TRUE);          // last: some firmware drops entries that aren't in BootOrder
    UINT16 next = (UINT16)num;
    if (!ubWriteVar(L"BootNext", &next, sizeof next)) { ubCleanupEntry(); return UB_NO_NEXT; }
    ubScheduleCleanup();
    if (noRestart) return UB_OK;
    enablePrivilege(SE_SHUTDOWN_NAME);
    if (!ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
        return UB_NO_RESTART;
    return UB_OK;
}

static const wchar_t* ubBootResultText(int rc) {
    switch (rc) {
    case UB_NOT_UEFI: return L"This PC doesn't start in UEFI mode, so it can't be told to start from USB once.";
    case UB_NO_PRIV: return L"Windows didn't allow changing the firmware boot settings.";
    case UB_NO_PART: return L"Couldn't read the USB drive's partition. Is it still plugged in?";
    case UB_NO_ENTRY: return L"The firmware didn't accept the boot entry.";
    case UB_NO_NEXT: return L"The firmware didn't accept the one-time boot setting. Nothing was changed.";
    case UB_NO_RESTART: return L"Set, but Windows didn't restart. The next restart starts from the USB drive.";
    case UB_NO_FREE: return L"The firmware has no free boot entry slot.";
    }
    return L"Something went wrong.";
}

// ---------- the helper and the button ----------
// --usbboot=<volume>, elevated. The volume has to be one of the bootable USB drives found now, whatever was passed.
static int ubHelperMain(const wchar_t* cmd) {
    if (wcsstr(cmd, L"--usbboot-cleanup")) return ubCleanupEntry();
    const wchar_t* p = wcsstr(cmd, L"--usbboot=");
    if (!p) return UB_NO_PART;
    wchar_t vol[64]; int n = 0;
    for (p += 10; *p && *p != L' ' && n < 63; p++) vol[n++] = *p;
    vol[n] = 0;
    ubScanUsb();
    for (int i = 0; i < g_ubN; i++)
        if (!_wcsicmp(g_ubDrive[i].vol, vol)) return ubBootOnce(g_ubDrive[i].vol, wcsstr(cmd, L"--usbboot-norestart") != NULL);
    return UB_NO_PART;
}

static void ubLabel(const UbDrive* d, wchar_t* out, int cch) {
    wchar_t sz[32];
    ubSizeText(d->size, sz, 32);
    if (d->letters[0]) swprintf(out, cch, L"%s (%s, %s)", d->name, sz, d->letters);
    else swprintf(out, cch, L"%s (%s)", d->name, sz);
}

// Restarts once into a bootable USB drive; every start after that is Windows again
static void bootToUsb(void) {
    if (!ubIsUefi()) {
        okDialog(L"Boot to USB isn't available",
                 L"Windows started in Legacy BIOS mode on this PC, so it can't be told to start from USB once. "
                 L"Use the boot menu during startup instead.");
        return;
    }
    ubScanUsb();
    if (!g_ubN) {
        okDialog(L"No bootable USB drive",
                 L"Plug in a USB drive that can start in UEFI mode, for example one made with Rufus, Ventoy or the "
                 L"Media Creation Tool, then try again.");
        return;
    }
    int pick = 0;
    wchar_t name[256];
    if (g_ubN > 1) {
        static wchar_t text[UB_MAX][256]; static const wchar_t* items[UB_MAX];
        for (int i = 0; i < g_ubN; i++) { ubLabel(&g_ubDrive[i], text[i], 256); items[i] = text[i]; }
        DlgExtra list; memset(&list, 0, sizeof(list));
        list.items = items; list.nItems = g_ubN; list.pick = &pick;
        if (!showDialogEx(g_hwnd, L"Restart to which USB drive?",
                          L"Your PC restarts now and starts from the drive you choose, once. After that, Windows starts as usual. "
                          L"Save your work first.", NULL, 0, L"Restart", L"Cancel", 1, &list)) return;
    } else if (askInApp()) {
        wchar_t msg[600];
        ubLabel(&g_ubDrive[0], name, 256);
        swprintf(msg, 600, L"Your PC restarts now and starts from %s, once. After that, Windows starts as usual.\n\nSave your work first.", name);
        if (!showDialog(g_hwnd, L"Restart to USB?", msg, NULL, 0, L"Restart", L"Cancel", 1)) return;
    }
    wchar_t args[128];
    HANDLE proc = NULL; BOOL cancelled = FALSE;
    swprintf(args, 128, L"--usbboot=%s", g_ubDrive[pick].vol);
    if (g_elevated) proc = startHelperDirect(args);
    else if (!startElevatedHelper(args, &proc, &cancelled) && cancelled) return;
    if (!proc) { okDialog(L"Couldn't restart", L"The administrator helper didn't start, so nothing was changed."); return; }
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    DWORD code = waitExitCode(proc);                       // a second or two
    SetCursor(old);
    if (code != UB_OK) okDialog(L"Couldn't restart to USB", ubBootResultText((int)code));
}