# OmniNine 显示驱动（0.01.448 / r448）使用说明

这是一个由 AI 辅助开发（DeepSeek / Kimi）的、面向 **Windows 95/98/98SE/ME** 的通用显示驱动。
只要显卡的 Video BIOS 支持 **VBE 2.0 或更高**，就能使用高分辨率与高色深，
并在窗口化 DOS 盒与全屏切换时保持画面正常（WinME 的全屏限制见 §1）。

本驱动尚不成熟，仅供测试/评估使用；请勿用于关键设备，对由此造成的数据丢失或设备损坏不承担责任。

> 只想安装使用：看 §1、§2。想自己编译：看 §5。

---

## 1. 适用环境

| 项       | 要求                                                     |
| -------- | -------------------------------------------------------- |
| 操作系统 | Windows 95 OSR2 / 98 / 98SE / **ME**                     |
| 显卡     | Video BIOS 支持 **VBE 2.0+**（绝大多数 1996 年后的显卡） |
| 磁盘空间 | 驱动本身不到 1MB                                         |

**⇒ Windows ME 用户请注意：**
Windows ME 移除了真实模式驱动支持（微软 KB 269524），
**在默认启动方式下，`fixsys.sys` 无法由 `CONFIG.SYS` 加载**。因此在 ME 上：

- **窗口化 DOS 盒正常**；
- **全屏 DOS 盒可能出现花屏/无字模**。

> 另一种**非官方**做法是用 DOS 8.0 启动 WinME，使 `CONFIG.SYS` 仍被解析，从而加载 `fixsys.sys`。
> 该路径**很可能可行**，但本项目未做正式验证，属进阶用户自担风险的用法。

---

## 2. 安装

安装有两种方式：**简单安装（`setup.exe`）** 与 **进阶安装（INF + 手工 CONFIG.SYS）**。

**⇒ 二选一，不要混用。** 普通用户请直接用 §2.1。

---

### 2.1 简单安装（`setup.exe`，推荐）

#### 2.1.1 准备安装包

把安装包解压到任意目录，**保持以下结构**（缺文件 setup 会报错）：

```
setup.exe
rel\omninine.drv        ← Release 驱动（推荐）
dbg\omninine.drv        ← Debug 驱动（慢，仅排障用）
common\minivdd.vxd
common\fixsys.sys
common\omninine.inf
zh-CN.lng            ← 可选：中文界面
```

> `setup.exe` 必须在 **Windows 中运行**（不能从 DOS 启动后运行）。

#### 2.1.2 运行安装

1. 双击 `setup.exe`；
2. **选择驱动版本**：
   - **Release（推荐）**：日常使用，速度最快；
   - **Debug**：日志详细、速度慢，只在排障时使用；
3. **FIXSYS.SYS 选项**（仅 Win95/98 可用）：
   - 勾选 **Install FIXSYS.SYS**（默认勾选）：修复全屏 DOS 盒花屏；
   - 加载方式：**DEVICEHIGH（默认，省内存）**；若启动异常可改 **DEVICE**；
   - **Windows ME 上该选项会自动禁用**（原因见 §2.3）。
     即使你使用"DOS 8.0 启动 WinME"的非官方方式，setup 仍会按 WinME 禁用；
     那种情况只能走 §2.2 进阶安装（该非官方路径很可能能加载 fixsys）；
4. 点 **Install**，等待日志提示完成；
5. 提示重启时选 **是**。

#### 2.1.3 安装后确认

- **显示属性 → 设置**：应能选择 1024×768 等分辨率；
- **设备管理器 → 显示适配器**：应显示本驱动，而不是"标准 VGA"；
- **Win95/98**：`CONFIG.SYS` 中应有
  `DEVICE(HIGH)=...\FIXSYS.SYS` 一行；开机若看到 `FIXSYS INIT OK`
  （可能一闪而过），说明盒内驱动已加载；
- **Debug 版**会生成日志 `C:\OMNININE.LOG`（Windows 不在 C 盘时以实际盘符为准；Release 版不产生日志）。

#### 2.1.4 已经装过旧驱动？

`setup.exe` 会检测并提示"先卸载"。选 **是** → 卸载完成后 **重启** →
**再运行一次 `setup.exe`** 安装。

> 若**重启后仍然**提示"已安装"，说明有残留文件或注册表项；
> 此时不要反复卸载，应检查系统目录里的 `omninine.drv`/`minivdd.vxd`
> 与 `Services\Class\Display` 下的残留，或换用 §2.2 进阶安装修复。

#### 2.1.5 卸载

运行 `setup.exe` → 点 **Uninstall** → 重启。
卸载会恢复原来的显示驱动、删除驱动文件、清理 `CONFIG.SYS` 中的 FIXSYS 行。

---

### 2.2 进阶安装（INF + 手工 CONFIG.SYS）

**适用场景：** 不想用 setup、想自定义文件位置、setup 失败需要手工修复、
或使用非官方 DOS 8.0 启动 WinME 的进阶用户。

#### 2.2.1 先备份

1. 备份**引导盘根目录**的 `CONFIG.SYS`：复制成 `CONFIG.BAK`；
2. 记下当前显示驱动（显示属性里可看），以便回滚；
3. 可选：用 `regedit` 导出
   `HKLM\System\CurrentControlSet\Services\Class\Display` 与 `HKLM\Enum\Display`。

#### 2.2.2 手动放置文件

安装包里的文件都在 `common\`（驱动在 `rel\` 或 `dbg\`）：

| 文件          | 放到哪里                                            | 示例                 |
| ------------- | --------------------------------------------------- | -------------------- |
| `omninine.drv`   | **Windows 系统目录**                                | `C:\WINDOWS\SYSTEM\` |
| `minivdd.vxd` | 同上                                                | `C:\WINDOWS\SYSTEM\` |
| `fixsys.sys`  | **Windows 目录**（仅 Win95/98 或非官方 WinME 方案） | `C:\WINDOWS\`        |
| `omninine.inf`   | 任意固定目录（供安装时浏览）                        | `C:\OmniNine\`          |

> 系统目录一般是 `%WINDIR%\SYSTEM`。**不要把 INF 放在临时目录后又删掉**，
> 以后"更新驱动"还会用到它。

#### 2.2.3 用 INF 安装显示驱动

**方式 A（图形界面，推荐）**

1. 桌面右键 → **属性** → **设置** → **高级** → **适配器** → **更改**；
2. 按向导点"下一步" → 选择"从磁盘安装"；
3. 浏览到 `omninine.inf` → 确定；
4. 在列表中选择 **`Generic VBE (LFB) Display Adapter`** →
   下一步 → 完成；
5. 先不要重启，继续 §2.2.4（或按提示重启后再补 `CONFIG.SYS`）。

**方式 B（命令行 / 右键安装）**

```bat
rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 132 C:\OmniNine\omninine.inf
```

`omninine.inf` 自带 `[DefaultInstall]` 段：会把 `omninine.drv`、`minivdd.vxd`
复制到系统目录、`fixsys.sys` 复制到 Windows 目录，并按**实例 0000**
写入显示类注册表值。

**⇒ 但方式 B 不会**：

- 写 `CONFIG.SYS`（fixsys 的 `DEVICE=` 行仍需 §2.2.4 手工加）；
- 创建 `Enum` 设备绑定（若系统当前绑定不是 0000，可能仍需用方式 A 或 setup）。

#### 2.2.4 手工编辑 CONFIG.SYS（仅 Win95/98；WinME 见 §2.3）

1. 用记事本打开**引导盘根目录**的 `CONFIG.SYS`（通常是 `C:\CONFIG.SYS`，
   **不是** Windows 目录里的同名文件）；

2. 在文件末尾添加一行（路径按你实际放置的位置）：

   ```
   DEVICEHIGH=C:\WINDOWS\FIXSYS.SYS
   ```

   若开机异常，改成不使用高端内存的写法：

   ```
   DEVICE=C:\WINDOWS\FIXSYS.SYS
   ```

3. 保存（确保是纯文本，文件名就是 `CONFIG.SYS`，不是 `CONFIG.SYS.TXT`）；

4. 重启。

**⇒ 四条要点：**

- 必须写在 `CONFIG.SYS`，**不能**写 `AUTOEXEC.BAT`；
- 用**绝对路径**；
- 只保留**一行** FIXSYS，若已有旧行先删掉；
- **WinME 默认不支持**；但若采用"DOS 8.0 启动"的非官方方案，`CONFIG.SYS` 很可能仍被解析，
  因而很可能可以加载 fixsys（本项目未做正式验证）。

#### 2.2.5 安装后确认

- 显示属性里能选分辨率/色深；
- 设备管理器显示本驱动；
- Win95/98：`CONFIG.SYS` 中 FIXSYS 行只有一条，开机看到 `FIXSYS INIT OK`；
- **Debug 版**日志 `C:\OMNININE.LOG` 可正常生成（Release 版不产生日志；盘符以实际为准）。

#### 2.2.6 进阶卸载 / 回滚

1. 显示属性 → 适配器 → 更改 → 选 **标准显示适配器（VGA）** → **重启**；
2. 重启后删除 `omninine.drv`、`minivdd.vxd`（系统目录）与
   `fixsys.sys`（Windows 目录）；
3. 编辑 `CONFIG.SYS` 删掉 FIXSYS 行（或恢复 `CONFIG.BAK`）；
4. 再重启一次。

---

### 2.3 Windows ME 特别说明

- WinME **移除了真实模式驱动支持**（微软 KB 269524），
  `fixsys.sys` **无法由 CONFIG.SYS 加载**；
- **简单安装会自动跳过 FIXSYS**；
- 进阶安装默认也**不要**手工加 `CONFIG.SYS` 行；
- 例外：使用"DOS 8.0 启动 WinME"的**非官方**方案时，`CONFIG.SYS` 很可能仍会被解析，
  因而 `fixsys.sys` **很可能可以加载**——但本项目未做正式验证，风险自负；
- 因此 ME 上**全屏 DOS 盒可能花屏/无中文**；**窗口化 DOS 盒正常**。

---

### 2.4 两种方式怎么选

| 情况                                       | 建议                                                |
| ------------------------------------------ | --------------------------------------------------- |
| 普通安装                                   | **§2.1 简单安装**                                   |
| 想放到自定义目录 / 没有 setup / setup 失败 | §2.2 进阶安装                                       |
| WinME（常规）                              | 任一方式都可以，但都**不装 fixsys**，全屏盒可能花屏 |
| WinME + 非官方 DOS 8.0 启动                | 只能走 §2.2；fixsys **很可能可加载**（非官方、未验证） |
| 已经用 setup 装过                          | **不要**再用 INF 重复安装                           |

---

## 3. 日常使用

- **改分辨率/色深**：显示属性 → 设置。推荐 **1024×768**，色深 8/16/32bpp 均可。
- **DOS 盒**：
  - 窗口化正常；
  - 全屏需要 `fixsys.sys`（见 §6）；若未加载，全屏可能花屏，
    可尝试按 `Alt+Enter` 切回窗口模式。
- **性能**：日常使用 Release 版；需要排错日志时换 Debug 版（会慢一些）。

**⇒ 日志位置：**`C:\OMNININE.LOG`（Windows 不在 C 盘时以实际盘符为准；
个别情况会退到引导盘根目录）。**Release 不带日志，需要日志请用 Debug 版。**

---

## 4. 常见问题

| 现象                        | 原因                          | 处理                                                                                                     |
| --------------------------- | ----------------------------- | -------------------------------------------------------------------------------------------------------- |
| 安装重启后仍是标准 VGA      | 没有真正绑定到本驱动          | 重新运行 `setup.exe` → 卸载 → 重启 → 再安装；仍不行请提供 `HKLM\Enum` 下显示设备的 `Driver`/`DeviceDesc` |
| 反复提示"已安装"            | 上次卸载有文件/注册表残留     | 检查系统目录 `omninine.drv`/`minivdd.vxd` 与 `Services\Class\Display` 残留；不要反复卸载                    |
| 全屏 DOS 盒花屏（Win95/98） | `CONFIG.SYS` 里没有 FIXSYS 行 | 按 §2.2.4 添加；或重跑 `setup.exe` → 卸载 → 重启 → 再安装（勾选 FIXSYS）                                                         |
| 全屏 DOS 盒花屏（WinME）    | ME 不支持真实模式驱动         | **预期限制**：改用窗口化 DOS 盒（§2.3）                                                                  |
| 24bpp 花屏/卡死             | 该色深未验证                  | 改用 8/16/32bpp                                                                                          |
| 切换全屏/窗口偶发异常       | 极少数显卡时序差异            | 保留 **Debug 版**日志 `C:\OMNININE.LOG`，并记录操作顺序                                                                    |

---

## 5. 从源码编译（简述）

### 5.1 环境

- **Open Watcom 2.0**：装到 `C:\WATCOM`
  （需含 `binnt`、`lib286\win`、`lib386\nt`）；
- **MASM 6.11+**（`ml.exe`，应在 `PATH` 中）；
- **Windows 98 DDK**：装到 `C:\98DDK`（需要 `Inc\win98` 与 `BIN\LINK.EXE`）。

```bat
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\H;%WATCOM%\H\WIN
set LIB=%WATCOM%\lib286\win
```

> **两个易错点：**
>
> 1. 编译 `minivdd.vxd` 时 **DDK 的 `BIN` 必须排在 `%WATCOM%\binnt` 之前**
>    （否则会抓到 Watcom 的同名 `link.exe`，它不认 `-vxd`）；
> 2. `fixver.exe` 由 `wcl386` 链接，若报找不到库，把
>    `%WATCOM%\lib386\nt` 加入 `LIB`。

### 5.2 编译

```bat
builddrv.bat     rem 显示驱动   -> omninine.drv
buildvxd.bat     rem mini-VDD   -> minivdd.vxd
buildsys.bat     rem 盒内 TSR   -> fixsys.sys
```

**⇒ 成功标志：**三个文件都生成，且 `omninine.drv` 可被 `setup.exe` 正常安装。

> **注意：** `builddrv.bat` 跑的是 **debug** 构建（带日志）。
> 发布用 `nmake omninine-rel.drv` 得到 `omninine-rel.drv`，
> **需改名为 `omninine.drv`** 后再放进 `rel\` 供 setup 使用。

---

## 6. `fixsys.sys` 的补充说明

**作用：**在全屏 DOS 盒里重新激活字模并清屏，解决全屏盒花屏/无中文。
Windows 侧驱动改不到盒子画面，所以只能由盒内 TSR 完成。

- **Win95/98**：`setup.exe` 会自动把它放到 **Windows 目录** 并写入 `CONFIG.SYS`；
- 手工检查：`CONFIG.SYS` 应有
  `DEVICEHIGH=<Windows目录>\FIXSYS.SYS`；
- 开机看到 `FIXSYS INIT OK` 表示加载成功；
- **Windows ME 默认不支持**；但"DOS 8.0 启动"的非官方方案很可能可用（见 §2.3）。

---

## 7. 版本说明

本项目同时使用两个版本号：

| 类型 | 当前值 | 在哪里看 |
|---|---|---|
| **内部版本** | **r448** | 驱动日志首行 `BUILD OmniNine 0.01.448 r448 ...`；`PH: build tag=0279` |
| **发布版本** | **0.01.448** | `setup.exe` 窗口标题、文件属性 → 版本；发布包名 |

**⇒ 规则：**

- `0.01` 是**公开发布线**（第一个公开版本）；
- `448` 是**内部版本号**，与源码树的 `r448` 一一对应（每次改动递增）；
- 同一发布线内的修复：内部 `r449` → 发布 `0.01.449`；
- 功能/行为有较大变化：发布线升到 `0.02.x`，内部号继续顺延。

**⇒ 查看方式：**

1. 安装器：窗口标题显示 `OmniNine Setup 0.01.448`；右键 `setup.exe` → 属性 → 版本；
2. 驱动：Debug 版日志 `C:\OMNININE.LOG` 第一行含 `BUILD OmniNine 0.01.448 r448`。

