# Windows Chosen Utilities v1.5

A small, native Windows 11 app: one portable exe with no installer and no dependencies. It has a Mica window,
follows your dark/light theme and accent colour, and uses Windows 11 style dialogs. Anything that needs
administrator rights asks once through UAC.

## Download
Get `Windows Chosen Utilities.exe` from the [latest release](https://github.com/misal1337/windows-chosen-utilities/releases/latest)
and run it. Nothing is installed.

## Pages
- **Disk Clean-up**: clears temp files, the Recycle Bin, the Windows Update cache, the thumbnail cache, crash
  dumps, error reports and, if you tick them, System Restore points. It only touches a fixed list of locations.
- **Reg Networks**: removes the generic "Network N" profiles and opens any pasted registry key in Registry Editor.
- **Run as TI**: runs an `.exe` or imports a `.reg` as TrustedInstaller.
- **Manual Updates**: downloads the newest `.msu` and .NET (ndp481) update for this PC's channel, and installs
  `.msu`/`.cab` files with DISM.
- **Windows AIO**: WIM Editor, ESD to ISO (pick the editions to keep; runs abbodi1406's esd-decrypter-wimlib, fetched each time, and names the ISO like Download Windows), Folder2ISO / ISO2Folder, WinToUSB and Hash.
- **Windows Repair**: SFC, DISM Scan/Restore Health (also from a WIM), Windows Update Cleanup and Reserved Storage,
  with live output.
- **Misc Tweaks**: Terminal settings restore, plus Enable/Disable switches for Explore Background, Gallery, VBS,
  RDP blank password, map drives delay, WU drivers, notifications, Windows theme, and Timezone and Region.
- **Download Windows**: builds a bootable x64 or Arm64 ISO from Microsoft's files via UUP dump, with the
  editions, language and preinstalled apps you choose.
- **Install Apps**: silent installs and updates of Firefox, IDM, PotPlayer, Chrome Remote Desktop, NanaZip and
  Tailscale, with signature checks and tidy Start menu shortcuts.
- **MS Office 365**: downloads Microsoft 365 Apps (x64) from the Office CDN, installs the apps you pick, and
  switches between the Current, Beta and Dev Main channels.
- **Boot to UEFI BIOS** (bottom of the rail): restarts straight into the firmware settings.

## Notes
- The exe is not code-signed yet, so SmartScreen may warn you on first launch: *More info → Run anyway*.
  On PCs with Smart App Control turned on, Windows blocks it until it is signed.
- Download Windows, Manual Updates and Office need the internet and rely on Microsoft's and UUP dump's servers.
- To build from source: install [Zig](https://ziglang.org/download/) (no Visual Studio needed), then `cd Source`
  and run `.\build.ps1`.

## Licence
MIT; see [LICENSE](LICENSE). Two small parts come from other MIT projects, credited there and in the source.

## Changelog
**6 Nov 2026: first build**
- Disk Clean-up, Reg Networks, the DISM install page and the Boot to UEFI button.
- Windows AIO page (WIM Editor, ESD to ISO, Folder2ISO / ISO2Folder, WinToUSB, Hash).

**7 Nov 2026**
- Restyled to the net-merge Windows 11 look: Mica, fades, DirectWrite text.
- System Restore options added to Disk Clean-up.
- Misc Tweaks page added (Terminal settings and the Enable/Disable switches).
- Download Windows page added. The DISM page was renamed Manual Updates, and the source moved to `Source`.

**8 Nov 2026**
- Manual Updates now downloads the latest `.msu` and ndp481 updates itself.
- Run as TI and Windows Repair pages added.
- Download Windows: preinstalled app picker, Arm64, more reliable downloads, nothing left in the root of C:,
  and a leftovers clean-up.
- Version label added.

**9 Nov 2026: v1.3**
- Install Apps page added, with update buttons and default file types for PotPlayer and NanaZip.
- MS Office 365 page added: download, install and channel switching.
- Windows 11 style dialogs replace message boxes, with one prompt per action. The Progress log scrolls.
