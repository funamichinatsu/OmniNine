# OmniNine Display Driver (0.01.448 / r448) — User Guide

A general-purpose display driver for **Windows 95/98/98SE/ME**, developed with AI
assistance (DeepSeek / Kimi). As long as the video BIOS supports **VBE 2.0 or later**,
you can use high resolutions and color depths, and the screen stays correct when
switching between windowed DOS boxes and full screen (see §1 for the Windows ME
full-screen limitation).

This driver is not mature yet. It is intended for **testing/evaluation only**; do not
use it on critical systems. We accept no liability for data loss or hardware damage
caused by its use.

> Just want to install: see §1 and §2. Want to build it yourself: see §5.

---

## 1. Requirements

| Item | Requirement |
|---|---|
| Operating system | Windows 95 OSR2 / 98 / 98SE / **ME** |
| Graphics card | Video BIOS supports **VBE 2.0+** (most cards from 1996 or later) |
| Disk space | The driver itself is under 1 MB |

**⇒ Note for Windows ME users:**
Windows ME removed real-mode driver support (Microsoft KB 269524). **With the default
boot method, `fixsys.sys` cannot be loaded from `CONFIG.SYS`.** Therefore, on ME:

- **Windowed DOS boxes work normally**;
- **Full-screen DOS boxes may appear garbled or without fonts**.

> Another **unofficial** approach is to boot Windows ME with DOS 8.0 so that
> `CONFIG.SYS` is still parsed and `fixsys.sys` can be loaded. This path is **likely
> workable**, but it is unofficial and has not been formally validated by this
> project; advanced users assume the risk.

---

## 2. Installation

There are two ways: **simple installation (`setup.exe`)** and **advanced installation
(INF + manual CONFIG.SYS)**.

**⇒ Choose one; do not mix them.** Most users should use §2.1.

---

### 2.1 Simple installation (`setup.exe`, recommended)

#### 2.1.1 Prepare the package

Extract the package to any directory and **keep this structure** (missing files will
make setup report an error):

```
setup.exe
rel\omninine.drv        ← Release driver (recommended)
dbg\omninine.drv        ← Debug driver (slower; troubleshooting only)
common\minivdd.vxd
common\fixsys.sys
common\omninine.inf
zh-CN.lng            ← Optional: Chinese UI
```

> `setup.exe` must be run **from Windows** (it cannot be run after booting to DOS).

#### 2.1.2 Run the installer

1. Double-click `setup.exe`.
2. **Choose the driver build**:
   - **Release (recommended)**: for daily use, fastest;
   - **Debug**: detailed logging, slower; use only for troubleshooting.
3. **FIXSYS.SYS option** (available on Windows 95/98 only):
   - Check **Install FIXSYS.SYS** (checked by default): fixes garbled full-screen DOS boxes;
   - Load method: **DEVICEHIGH (default, saves memory)**; if boot problems occur, use **DEVICE** instead;
   - **On Windows ME this option is disabled automatically** (see §2.3).
     Even if you use the unofficial "boot with DOS 8.0" method, setup still disables it
     because it detects ME; in that case use the advanced installation in §2.2 (that
     unofficial path can likely load fixsys).
4. Click **Install** and wait until the log reports completion.
5. When prompted to restart, choose **Yes**.

#### 2.1.3 After installation

- **Display Properties → Settings**: you should be able to select 1024×768 and other modes;
- **Device Manager → Display adapters**: should show this driver, not "Standard VGA";
- **Windows 95/98**: `CONFIG.SYS` should contain a line
  `DEVICE(HIGH)=...\FIXSYS.SYS`; if you see `FIXSYS INIT OK` during boot (it may
  flash by), the in-box driver has loaded;
- The **Debug build** writes a log to `C:\OMNININE.LOG` (the drive letter may differ if
  Windows is not on C:; the Release build does not produce logs).

#### 2.1.4 Already have an older driver installed?

`setup.exe` detects it and asks you to uninstall first. Choose **Yes** → after
uninstallation **restart** → **run `setup.exe` again** to install.

> If it **still** says "already installed" after a restart, there are leftover files or
> registry entries. Do not keep uninstalling in a loop; check for
> `omninine.drv`/`minivdd.vxd` in the system directory and leftovers under
> `Services\Class\Display`, or repair the installation using §2.2.

#### 2.1.5 Uninstall

Run `setup.exe` → click **Uninstall** → restart.
Uninstallation restores the previous display driver, removes the driver files, and
cleans the FIXSYS line from `CONFIG.SYS`.

---

### 2.2 Advanced installation (INF + manual CONFIG.SYS)

**When to use it:** you do not want to use setup, you want custom file locations,
setup failed and you need manual repair, or you are an advanced user using the
unofficial DOS 8.0 boot method for Windows ME.

#### 2.2.1 Back up first

1. Back up `CONFIG.SYS` **in the root of the boot drive**: copy it to `CONFIG.BAK`;
2. Note the current display driver (visible in Display Properties) so you can roll back;
3. Optional: export
   `HKLM\System\CurrentControlSet\Services\Class\Display` and `HKLM\Enum\Display`
   with `regedit`.

#### 2.2.2 Place the files manually

The package files are in `common\` (the drivers are in `rel\` or `dbg\`):

| File | Where to put it | Example |
|---|---|---|
| `omninine.drv` | **Windows system directory** | `C:\WINDOWS\SYSTEM\` |
| `minivdd.vxd` | Same as above | `C:\WINDOWS\SYSTEM\` |
| `fixsys.sys` | **Windows directory** (Win95/98 or the unofficial WinME method only) | `C:\WINDOWS\` |
| `omninine.inf` | Any permanent directory (so you can browse to it) | `C:\OmniNine\` |

> The system directory is usually `%WINDIR%\SYSTEM`. **Do not put the INF in a
> temporary directory and delete it later**; "Update Driver" will need it again.

#### 2.2.3 Install the display driver from the INF

**Method A (GUI, recommended)**

1. Right-click the desktop → **Properties** → **Settings** → **Advanced** → **Adapter** → **Change**;
2. Click "Next" in the wizard → choose "Have Disk";
3. Browse to `omninine.inf` → OK;
4. Select **`Generic VBE (LFB) Display Adapter`** from the list → Next → Finish;
5. Do not restart yet; continue with §2.2.4 (or reboot if prompted and add `CONFIG.SYS` afterwards).

**Method B (command line / right-click install)**

```bat
rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 132 C:\OmniNine\omninine.inf
```

`omninine.inf` contains a `[DefaultInstall]` section: it copies `omninine.drv` and
`minivdd.vxd` to the system directory, copies `fixsys.sys` to the Windows directory,
and writes display-class registry values for **instance 0000**.

**⇒ Method B does not:**

- write `CONFIG.SYS` (the fixsys `DEVICE=` line must still be added manually in §2.2.4);
- create the `Enum` device binding (if the current binding is not 0000, you may still
  need Method A or setup).

#### 2.2.4 Edit CONFIG.SYS manually (Windows 95/98 only; for ME see §2.3)

1. Open `CONFIG.SYS` **in the root of the boot drive** with Notepad (usually
   `C:\CONFIG.SYS`; **not** the file with the same name in the Windows directory);
2. Add one line at the end of the file (use the path where you actually placed it):

   ```
   DEVICEHIGH=C:\WINDOWS\FIXSYS.SYS
   ```

   If boot problems occur, use the form that does not use upper memory:

   ```
   DEVICE=C:\WINDOWS\FIXSYS.SYS
   ```
3. Save it (make sure it is plain text and the file name is `CONFIG.SYS`, not
   `CONFIG.SYS.TXT`);
4. Restart.

**⇒ Four key points:**

- It must be in `CONFIG.SYS`, **not** `AUTOEXEC.BAT`;
- Use an **absolute path**;
- Keep only **one** FIXSYS line; delete any old line first;
- **Windows ME does not support it by default**; but with the unofficial
  "DOS 8.0 boot" method, `CONFIG.SYS` is likely still parsed, so fixsys can
  **likely** be loaded (not formally validated by this project).

#### 2.2.5 After installation

- Display Properties can select resolution/color depth;
- Device Manager shows this driver;
- Windows 95/98: exactly one FIXSYS line in `CONFIG.SYS`, and `FIXSYS INIT OK`
  appears during boot;
- The **Debug build** log `C:\OMNININE.LOG` is created (the Release build does not
  produce logs; the drive letter may differ).

#### 2.2.6 Advanced uninstall / rollback

1. Display Properties → Adapter → Change → select **Standard Display Adapter (VGA)** → **restart**;
2. After restarting, delete `omninine.drv` and `minivdd.vxd` (system directory) and
   `fixsys.sys` (Windows directory);
3. Edit `CONFIG.SYS` to remove the FIXSYS line (or restore `CONFIG.BAK`);
4. Restart once more.

---

### 2.3 Windows ME notes

- Windows ME **removed real-mode driver support** (Microsoft KB 269524);
  under the default boot method `fixsys.sys` **cannot be loaded from CONFIG.SYS**;
- **Simple installation skips FIXSYS automatically**;
- Advanced installation should also **not** add a `CONFIG.SYS` line by default;
- Exception: with the **unofficial** "boot Windows ME with DOS 8.0" method,
  `CONFIG.SYS` is very likely still parsed, so `fixsys.sys` can **very likely** be
  loaded — but this has not been formally validated by this project; use at your own risk;
- Therefore, on ME a **full-screen DOS box may be garbled or lack Chinese characters**;
  **windowed DOS boxes work normally**.

---

### 2.4 Which installation method to choose

| Situation | Recommendation |
|---|---|
| Normal installation | **§2.1 Simple installation** |
| Custom directories / no setup / setup failed | §2.2 Advanced installation |
| Windows ME (normal) | Either method works, but **neither installs fixsys**; full-screen boxes may be garbled |
| Windows ME + unofficial DOS 8.0 boot | §2.2 only; fixsys **very likely loads** (unofficial, unvalidated) |
| Already installed with setup | **Do not** install again via the INF |

---

## 3. Daily use

- **Changing resolution/color depth**: Display Properties → Settings. **1024×768** is
  recommended; 8/16/32bpp all work.
- **DOS boxes**:
  - Windowed mode works normally;
  - Full screen requires `fixsys.sys` (see §6). If it is not loaded, the full-screen
    box may be garbled; try pressing `Alt+Enter` to switch back to windowed mode.
- **Performance**: use the Release build for daily work; switch to the Debug build when
  you need logs for troubleshooting (it is slower).

**⇒ Log location:** the **Debug build** writes `C:\OMNININE.LOG` (the drive letter may
differ if Windows is not on C:; in some cases it falls back to the root of the boot
drive). **The Release build does not produce logs; use Debug if you need logs.**

---

## 4. Troubleshooting

| Symptom | Cause | Action |
|---|---|---|
| Still Standard VGA after installation and restart | The driver was not actually bound | Run `setup.exe` again → uninstall → restart → install again; if it still fails, provide the `Driver`/`DeviceDesc` values of the display device under `HKLM\Enum` |
| Keeps saying "already installed" | Leftover files/registry entries from the previous uninstall | Check for `omninine.drv`/`minivdd.vxd` in the system directory and leftovers under `Services\Class\Display`; do not uninstall in a loop |
| Full-screen DOS box garbled (Win95/98) | No FIXSYS line in `CONFIG.SYS` | Add it as in §2.2.4; or run `setup.exe` → uninstall → restart → install again (with FIXSYS checked) |
| Full-screen DOS box garbled (WinME) | ME does not support real-mode drivers | **Expected limitation**: use windowed DOS boxes (§2.3) |
| 24bpp garbled/hangs | That color depth is not validated | Use 8/16/32bpp |
| Occasional anomalies when switching full screen/window | Rare timing differences on a few cards | Keep the **Debug build** log `C:\OMNININE.LOG` and record the sequence of actions |

---

## 5. Building from source (brief)

### 5.1 Requirements

- **Open Watcom 2.0**: installed at `C:\WATCOM`
  (must include `binnt`, `lib286\win`, `lib386\nt`);
- **MASM 6.11+** (`ml.exe`, available in `PATH`);
- **Windows 98 DDK**: installed at `C:\98DDK` (`Inc\win98` and `BIN\LINK.EXE` required).

```bat
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\H;%WATCOM%\H\WIN
set LIB=%WATCOM%\lib286\win
```

> **Two common pitfalls:**
>
> 1. When building `minivdd.vxd`, the **DDK `BIN` directory must come before
>    `%WATCOM%\binnt` in `PATH`** (otherwise the same-named Watcom `link.exe` is
>    picked up, which does not understand `-vxd`);
> 2. `fixver.exe` is linked by `wcl386`; if it reports missing libraries, add
>    `%WATCOM%\lib386\nt` to `LIB`.

### 5.2 Building

```bat
builddrv.bat     rem display driver -> omninine.drv
buildvxd.bat     rem mini-VDD       -> minivdd.vxd
buildsys.bat     rem in-box TSR     -> fixsys.sys
```

**⇒ Success criteria:** all three files are produced, and `omninine.drv` can be installed
by `setup.exe`.

> **Note:** `builddrv.bat` performs a **debug** build (with logging).
> For a release build use `nmake omninine-rel.drv`, which produces `omninine-rel.drv`;
> **rename it to `omninine.drv`** before placing it in `rel\` for setup.

---

## 6. Notes on `fixsys.sys`

**Purpose:** in a full-screen DOS box, it re-activates the font and clears the screen,
fixing garbled/missing Chinese text. The Windows-side driver cannot modify the box's
screen, so this can only be done by an in-box TSR.

- **Windows 95/98**: `setup.exe` places it in the **Windows directory** and writes it
  into `CONFIG.SYS` automatically;
- Manual check: `CONFIG.SYS` should contain
  `DEVICEHIGH=<Windows directory>\FIXSYS.SYS`;
- Seeing `FIXSYS INIT OK` during boot means it loaded successfully;
- **Windows ME does not support it by default**; however, the unofficial
  "DOS 8.0 boot" method will very likely work (see §2.3).

---

## 7. Versioning

The project uses two version numbers:

| Type | Current value | Where to see it |
|---|---|---|
| **Internal build** | **r448** | First line of the driver log: `BUILD OmniNine 0.01.448 r448 ...`; `PH: build tag=0279` |
| **Release version** | **0.01.448** | `setup.exe` window title and file properties → Version; package names |

**⇒ Rules:**

- `0.01` is the **public release line** (first public release);
- `448` is the **internal build number**, matching the source tree's `r448` (incremented on every change);
- A fix within the same release line: internal `r449` → release `0.01.449`;
- Larger functional changes: release line becomes `0.02.x`, with the internal number continuing.

**⇒ How to check:**

1. Installer: the window title shows `OmniNine Setup 0.01.448`; right-click `setup.exe` → Properties → Version;
2. Driver: the Debug log `C:\OMNININE.LOG` starts with `BUILD OmniNine 0.01.448 r448`.
