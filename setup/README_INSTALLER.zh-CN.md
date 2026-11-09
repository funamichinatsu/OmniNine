# OmniNine Setup 安装器（r42 / 0.01.42）

独立的 32 位 Windows 安装程序，负责安装/卸载 OmniNine 显示驱动。
**仅支持 Windows 95/98/98SE/ME**；在 Windows NT/2000/XP 及更高版本上启动时
会直接提示"不支持"并退出。

---

## 1. 目录布局

```
setup.exe
rel\omninine.drv        ← Release 驱动
dbg\omninine.drv        ← Debug 驱动（慢，仅排障）
common\minivdd.vxd
common\fixsys.sys
common\omninine.inf
zh-CN.lng            ← 可选：简体中文界面
```

只有 `omninine.drv` 分 release/debug；其余文件在 `common\` 中共用。
`setup.exe` 必须与 `rel`/`dbg`/`common` 位于同一目录。

---

## 2. 安装

### 2.1 运行

双击 `setup.exe`，然后：

1. 选择驱动版本：**Release（推荐）** 或 **Debug**；
2. 可选 **Install FIXSYS.SYS**（仅 Win95/98）：
   - 勾选后修复全屏 DOS 盒花屏；
   - 加载方式 **DEVICEHIGH（默认）** 或 **DEVICE**；
   - **Windows ME 上该选项自动禁用**（见 §4）；
3. 点 **Install**，完成后按提示重启。

### 2.2 已安装旧驱动时

安装器检测到已安装会提示先卸载：选 **是** → 卸载 → **重启** →
再次运行 `setup.exe` 安装。安装器**不会热替换正在使用的驱动**。

### 2.3 安装器实际做了什么

1. 定位显示类实例（`HKLM\System\CurrentControlSet\Services\Class\Display\000x`），
   把整个实例子树备份到 `...\Class\Display\__VBEBAK_<实例>`；
2. 复制 `omninine.drv`、`minivdd.vxd` 到**系统目录**；
   Win95/98 下把 `fixsys.sys` 复制到 **Windows 目录**；
3. 改写 `CONFIG.SYS`（Win95/98）：
   - 定位方式：优先 `%CONFIG%` 环境变量，其次找根目录含 `IO.SYS` 的引导盘；
   - 删除旧的 FIXSYS 行，追加
     `DEVICE=` 或 `DEVICEHIGH=<Windows目录>\FIXSYS.SYS`；
   - 备份：先 `CONFIG.SYS.BAK`，被占用则 `.BA0`，仍被占用则覆盖 `.BAK`；
4. 写 `SYSTEM.INI` 的 `[boot] display.drv=pnpdrvr.drv`；
5. **直接写注册表完成"驱动绑定"**（不依赖 SetupX 是否执行成功）：
   - 写显示类实例的 `Ver/DevLoader/InfPath/InfSection/DeviceDesc/...` 与 `DEFAULT` 值；
   - 两级扫描 `Enum\Display\<设备>\<实例>`，写
     `Driver="Display\<实例>"`、`ConfigFlags=0`、`Class`/`ClassGUID`；
   - 再生成临时 INF（`%WINDIR%\TEMP\OMNININ.INF`）并调用
     `rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 130 <INF>` 作标准登记；
   - **回读校验**：`Enum` 节点的 `Driver` 与实例 `DEFAULT\drv` 必须都指向本驱动；
     不通过则再直写一次，仍失败才报错；
6. 把 `omninine.inf` 复制到 `%WINDIR%\INF\OmniNine.INF`（先删旧文件，避免旧 INF 残留），
   并校验新 INF 中不再引用 `fixsys.sys`；
7. 提示重启。

> 安装阶段的注册表写入全部由 SetupX 按 INF 完成，安装器本身不直接写显示类值。

---

## 3. 卸载

运行 `setup.exe` → **Uninstall**：

1. 若存在 `__VBEBAK_<实例>` 备份，则用它整树恢复原实例；
   否则扫描 `Class\Display` 与 `Enum` 中带 `OmniNine` 标记的键并删除，
   然后把实例重置为标准 VGA（`vga.drv` / `4,640,480`）；
2. 从 `CONFIG.SYS` 删除 FIXSYS 行（若文件存在）；
3. 删除 `omninine.drv`、`minivdd.vxd`、`FIXSYS.SYS`、`OmniNine.INF` 及暂存的
   `*.NEW` 文件；
4. 提示重启。

> 若某个文件正被占用而删不掉，日志会记录 `cannot delete ...`；
> **重启后再运行一次卸载**即可完成。

---

## 4. Windows ME 说明

Windows ME 移除了真实模式驱动支持（KB 269524），`fixsys.sys` 无法由
`CONFIG.SYS` 加载。因此安装器在 ME 上：

- 自动禁用并跳过 FIXSYS；
- 不修改 `CONFIG.SYS`；
- 仅安装显示驱动本身。

ME 上全屏 DOS 盒可能花屏，窗口化 DOS 盒正常。

---

## 5. 界面语言（外挂语言文件）

- 程序按系统语言自动选择：简体中文 → `zh-CN`；找不到则用内置英文；
- 可用环境变量强制：`SETUP_LANG=zh-CN`；
- 查找顺序（相对 `setup.exe` 所在目录）：
  1. `setup.<code>.lng`
  2. `lang\setup.<code>.lng`
  3. `<code>.lng`
- 文件格式：每行 `key=value`，值里的 `\n` 会转换为换行；
- 编码：**ANSI/GBK**（简体中文）；不要用 UTF-8，否则界面会乱码；
- 缺失的键自动回落到内置英文。

常用键：`ui.title`、`ui.build`、`ui.rel`、`ui.dbg`、`ui.fixchk`、`ui.install`、
`ui.uninstall`、`ui.exit`、`msg.nt`、`msg.me`、`msg.existing`、`msg.install_reboot`、
`msg.uninst_reboot`、`msg.uninst_confirm`。

---

## 6. 编译安装器

工具：**Open Watcom 2.0**（`wcl386`、`wrc`）。

```bat
build_setup.bat
```

等价于：

```bat
wrc -q -bt=nt -fo=setup.res setup.rc
wcl386 -zq -bt=nt -l=nt_win -i=%WATCOM%\h;%WATCOM%\h\nt -fe=setup.exe setup.c setup.res
```

- `setup.rc` 引用 `setup-installer.ico`（程序图标）与版本信息资源；
- **必须带 `-i` 指向 Win32 头**，否则会误用 16 位 `windows.h`，
  出现 `MAX_PATH/HKEY/RegQueryValueEx 未声明` 一类错误。

---

## 7. 已知限制

1. 仅支持 Windows 9x；NT 系直接拒绝；
2. 安装/卸载都需要**重启**生效；不能热替换正在加载的驱动；
3. 安装器检测"已安装"依赖驱动文件或注册表；
   若手工删除不彻底，可能仍提示已安装——按 §3 的说明先卸载并重启；
4. `CONFIG.SYS` 的定位依赖 `%CONFIG%` 或引导盘 `IO.SYS`；
   多引导/非标准布局请先在目标系统确认 `CONFIG.SYS` 位置；
5. 若自动绑定失败（例如 `Enum` 下找不到显示节点），安装器会弹出提示；
   此时文件与 INF 已就位，可按提示用
   "显示属性 → 更改 → 从磁盘安装" 指向 `omninine.inf` 完成。
