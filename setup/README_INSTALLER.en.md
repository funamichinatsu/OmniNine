# OmniNine Setup Installer (r42 / 0.01.42)

A standalone 32-bit Windows installer that installs/uninstalls the OmniNine
display driver. **Windows 95/98/98SE/ME only**; on Windows NT/2000/XP or later it
shows an "unsupported" message and exits immediately.

---

## 1. Package layout

```
setup.exe
rel\omninine.drv        ← Release driver
dbg\omninine.drv        ← Debug driver (slow; troubleshooting only)
common\minivdd.vxd
common\fixsys.sys
common\omninine.inf
zh-CN.lng            ← Optional: Simplified Chinese UI
```

Only `omninine.drv` has release/debug variants; the other files are shared in
`common\`. `setup.exe` must be in the same directory as `rel`/`dbg`/`common`.

---

## 2. Installation

### 2.1 Running

Double-click `setup.exe`, then:

1. Choose the driver build: **Release (recommended)** or **Debug**;
2. Optionally check **Install FIXSYS.SYS** (Windows 95/98 only):
   - fixes garbled full-screen DOS boxes;
   - load method **DEVICEHIGH (default)** or **DEVICE**;
   - **automatically disabled on Windows ME** (see §4);
3. Click **Install**, then restart when prompted.

### 2.2 If an older driver is already installed

The installer detects it and asks you to uninstall first: choose **Yes** →
uninstall → **restart** → run `setup.exe` again. It never hot-replaces a driver
that is currently loaded.

### 2.3 What the installer actually does

1. Locates the display class instance
   (`HKLM\System\CurrentControlSet\Services\Class\Display\000x`) and backs up the
   entire instance subtree to `...\Class\Display\__VBEBAK_<instance>`;
2. Copies `omninine.drv` and `minivdd.vxd` to the **system directory**;
   on Windows 95/98, copies `fixsys.sys` to the **Windows directory**;
3. Rewrites `CONFIG.SYS` (Windows 95/98):
   - locates it via the `%CONFIG%` environment variable first, otherwise the boot
     drive whose root contains `IO.SYS`;
   - removes old FIXSYS lines and appends
     `DEVICE=` or `DEVICEHIGH=<Windows directory>\FIXSYS.SYS`;
   - backup: `CONFIG.SYS.BAK`, then `.BA0` if busy, otherwise overwrite `.BAK`;
4. Writes `[boot] display.drv=pnpdrvr.drv` into `SYSTEM.INI`;
5. **Writes the registry entries that bind the device directly** (not relying on
   whether SetupX actually runs):
   - writes the class instance values `Ver/DevLoader/InfPath/InfSection/DeviceDesc/...`
     and the `DEFAULT` values;
   - scans `Enum\Display\<device>\<instance>` (two levels) and writes
     `Driver="Display\<instance>"`, `ConfigFlags=0`, `Class`/`ClassGUID`;
   - also generates a temporary INF (`%WINDIR%\TEMP\OMNININ.INF`) and runs
     `rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 130 <INF>` as the
     standard registration step;
   - **reads the values back**: the `Enum` node's `Driver` and the instance's
     `DEFAULT\drv` must both point at this driver; if not, it retries the direct
     writes and only then reports failure;
6. Copies `omninine.inf` to `%WINDIR%\INF\OmniNine.INF` (deleting the old file first so
   no stale INF remains), and verifies that the new INF no longer references
   `fixsys.sys`;
7. Prompts for a restart.

> All registry writes during installation are performed by SetupX from the INF;
> the installer itself does not write display-class values directly.

---

## 3. Uninstallation

Run `setup.exe` → **Uninstall**:

1. If a `__VBEBAK_<instance>` backup exists, the whole instance is restored from it;
   otherwise keys under `Class\Display` and `Enum` containing the `OmniNine` marker are
   removed, and the instance is reset to Standard VGA (`vga.drv` / `4,640,480`);
2. Removes the FIXSYS line from `CONFIG.SYS` (if the file exists);
3. Deletes `omninine.drv`, `minivdd.vxd`, `FIXSYS.SYS`, `OmniNine.INF` and any staged
   `*.NEW` files;
4. Prompts for a restart.

> If a file cannot be deleted because it is in use, the log records
> `cannot delete ...`; **restart and run Uninstall again** to finish.

---

## 4. Windows ME notes

Windows ME removed real-mode driver support (KB 269524), so `fixsys.sys` cannot be
loaded from `CONFIG.SYS`. Therefore, on ME the installer:

- automatically disables and skips FIXSYS;
- does not modify `CONFIG.SYS`;
- installs only the display driver itself.

On ME, full-screen DOS boxes may be garbled; windowed DOS boxes work normally.

---

## 5. UI language (external language files)

- The program selects automatically by system language: Simplified Chinese →
  `zh-CN`; otherwise built-in English;
- Override with the `SETUP_LANG` environment variable, e.g. `SETUP_LANG=zh-CN`;
- Search order (relative to the directory containing `setup.exe`):
  1. `setup.<code>.lng`
  2. `lang\setup.<code>.lng`
  3. `<code>.lng`
- Format: one `key=value` per line; `\n` in the value is converted to a newline;
- Encoding: **ANSI/GBK** (Simplified Chinese); do not use UTF-8 or the UI will be
  garbled;
- Missing keys fall back to the built-in English text.

Common keys: `ui.title`, `ui.build`, `ui.rel`, `ui.dbg`, `ui.fixchk`, `ui.install`,
`ui.uninstall`, `ui.exit`, `msg.nt`, `msg.me`, `msg.existing`,
`msg.install_reboot`, `msg.uninst_reboot`, `msg.uninst_confirm`.

---

## 6. Building the installer

Tools: **Open Watcom 2.0** (`wcl386`, `wrc`).

```bat
build_setup.bat
```

Equivalent to:

```bat
wrc -q -bt=nt -fo=setup.res setup.rc
wcl386 -zq -bt=nt -l=nt_win -i=%WATCOM%\h;%WATCOM%\h\nt -fe=setup.exe setup.c setup.res
```

- `setup.rc` references `setup-installer.ico` (program icon) and version info;
- **The `-i` option pointing at the Win32 headers is mandatory**; otherwise the
  16-bit `windows.h` is used and you get errors such as
  `MAX_PATH/HKEY/RegQueryValueEx not declared`.

---

## 7. Known limitations

1. Windows 9x only; NT-family systems are rejected;
2. Installation/uninstallation require a **restart**; loaded drivers are never
   hot-replaced;
3. "Already installed" detection relies on driver files or registry entries; if a
   manual cleanup was incomplete, setup may still report installed — uninstall and
   restart first (see §3);
4. `CONFIG.SYS` location depends on `%CONFIG%` or the boot drive's `IO.SYS`; for
   multi-boot or non-standard layouts, confirm the actual `CONFIG.SYS` first;
5. If automatic binding fails (for example no display node is found under
   `Enum`), the installer shows a message; the files and INF are already in place,
   so finish with "Display Properties → Change → Have Disk" pointing at `omninine.inf`.
