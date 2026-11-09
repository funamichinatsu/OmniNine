/* ===========================================================================
 *  OmniNine Setup -- Win32 GUI installer (Windows 95 / 98 / ME)
 *
 *  只依赖 kernel32 / user32 / advapi32; ANSI; 不用 ComCtl32。
 *
 *  安装: 选 Release/Debug; 选 FIXSYS 的 DEVICE/DEVICEHIGH; 可选装 FIXSYS。
 *        - 拷贝 omninine.drv / minivdd.vxd / fixsys.sys 到系统目录
 *        - 写注册表(按已验证的 template.inf 语义)
 *        - 更新 CONFIG.SYS(DEVICE(HIGH)=<系统目录>\FIXSYS.SYS)
 *        - 安装 OMNININE.INF 到 %WINDIR%\INF
 *        - 备份原注册表值, 卸载时恢复
 *
 *  构建(Open Watcom):
 *    set WATCOM=C:\WATCOM
 *    wcl386 -zq -bt=nt -l=nt_win -i=%WATCOM%\h;%WATCOM%\h\nt -fe=setup.exe setup.c
 * =========================================================================== */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define IDC_REL       1001
#define IDC_DBG       1002
#define IDC_FIXSYS    1003
#define IDC_DEVLOW    1004
#define IDC_DEVHIGH   1005
#define IDC_INSTALL   1010
#define IDC_UNINSTALL 1011
#define IDC_EXIT      1012
#define IDC_LOG       1020
#define IDC_STATUS    1021

#define APPNAME  "OmniNine Setup"
#define VERSION_STR "0.01.42"
#define INTERNAL_VER "r42"
#define DISP_KEY "System\\CurrentControlSet\\Services\\Class\\Display"

static HINSTANCE g_hInst;
static HWND  g_hWnd, g_hLog, g_hStatus;
static char  g_winDir[MAX_PATH], g_sysDir[MAX_PATH], g_appDir[MAX_PATH];
static char  g_backupIni[MAX_PATH];
static char  g_instance[64] = "0000";
static char  g_bindPath[MAX_PATH];
static char  g_nodePath[MAX_PATH];
static char  g_inst[64];
static char  g_modelNode[MAX_PATH];
#define DISP_GUID "{4D36E968-E325-11CE-BFC1-08002BE10318}"
static BOOL  g_busy = FALSE;
static BOOL  g_deferred = FALSE;
static BOOL FindSourceFile(const char *preferSub, const char *name, char *out);
static char *LangGet(const char *key, const char *def);
static void DoUninstall(void);
   /* 有文件需重启时替换 */

/* ---- 前置声明: 这些函数在文件中先后被使用, 统一声明避免 W131 ---- */
static BOOL OpenInstanceKey(HKEY *out);
static BOOL LoadFile(const char *path, char **buf, DWORD *size);
static BOOL SaveFile(const char *path, const char *buf, DWORD size);
static void ExportInstance(void);
static BOOL RestoreInstanceTree(void);
static void CleanWinInit(void);
static BOOL AddWinInitEntry(const char *dst, const char *src);
static BOOL CopyWithReplace(const char *sub, const char *name, const char *dstDir);
static void DeleteOrDefer(const char *path);


/* ---------------- 基础工具 ---------------- */

static void Log(const char *fmt, ...)
{
    char buf[1024], text[16384];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);

    if (g_hLog) {
        GetWindowText(g_hLog, text, sizeof(text));
        if (text[0]) strcat(text, "\r\n");
        strncat(text, buf, sizeof(text) - strlen(text) - 3);
        SetWindowText(g_hLog, text);
        SendMessage(g_hLog, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
        SendMessage(g_hLog, WM_VSCROLL, SB_BOTTOM, 0);
    }
    {   /* r34: 同时落盘, 便于用户回传安装失败原因 */
        char lp[MAX_PATH];
        HANDLE h;
        DWORD wr;
        wsprintf(lp, "%s\\OMNININE-SETUP.LOG", g_winDir);
        h = CreateFile(lp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, buf, (DWORD)strlen(buf), &wr, NULL);
            WriteFile(h, "\r\n", 2, &wr, NULL);
            CloseHandle(h);
        }
    }
}

static void Status(const char *s) { SetWindowText(g_hStatus, s); }

static void JoinPath(char *out, const char *a, const char *b)
{
    strcpy(out, a);
    if (out[0] && out[strlen(out) - 1] != '\\') strcat(out, "\\");
    strcat(out, b);
}

/* [r15] 不能用 GetFileAttributes 判存在: 在 9x 上, 已被 CONFIG.SYS 加载的
 * DOS 设备驱动(如 fixsys.sys)常以独占方式被占用, 任何"打开文件"型 API 都会失败,
 * GetFileAttributes 内部就可能打开文件 ⇒ 明明存在却返回"不存在"。
 * 改为目录枚举(FindFirstFile): 只读目录项, 不打开文件, 对被占用文件同样有效。 */
/* [r16] ProbeExists: 判断"安装源里是否有这个文件"。
 * 9x 上被 CONFIG.SYS 加载的 .SYS 可能连目录枚举/属性查询都失败,
 * 此时 CreateFile 返回 sharing violation(32)/access denied(5) —— 恰恰证明文件存在;
 * 只有 FILE_NOT_FOUND(2)/PATH_NOT_FOUND(3) 才算真的没有。 */
static BOOL ProbeExists(const char *p, DWORD *pErr)
{
    HANDLE h;
    DWORD e;

    SetLastError(0);
    h = CreateFile(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                   OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); if (pErr) *pErr = 0; return TRUE; }
    e = GetLastError();
    if (pErr) *pErr = e;
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION)
        return TRUE;
    return FALSE;
}

static BOOL FileExists(const char *p)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void Upcase(char *s) { while (*s) { if (*s >= 'a' && *s <= 'z') *s -= 32; s++; } }

static BOOL HasNoCase(const char *hay, const char *needle)
{
    char a[512], b[128];
    strncpy(a, hay, sizeof(a) - 1); a[sizeof(a) - 1] = 0;
    strncpy(b, needle, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
    Upcase(a); Upcase(b);
    return strstr(a, b) != NULL;
}

static BOOL IsWin9x(void) { return (GetVersion() & 0x80000000UL) != 0; }

/* [r29] Windows ME = 4.90。KB 269524: 真实模式支持已移除,
 *   "Real-mode drivers can no longer be loaded ... in the Config.sys"
 *   => fixsys.sys 在 WinME 上不可能被加载, setup 不再安装它。 */
static BOOL IsWinNT(void)
{
    return (GetVersion() & 0x80000000UL) == 0;   /* NT 系(含 2000/XP+) */
}

static BOOL IsWinME(void)
{
    DWORD v = GetVersion();
    if (!(v & 0x80000000UL)) return FALSE;
    return ((v & 0xFF) == 4) && (((v >> 8) & 0xFF) == 90);
}

static void GetDirs(void)
{
    char *p;
    GetWindowsDirectory(g_winDir, sizeof(g_winDir));
    GetSystemDirectory(g_sysDir, sizeof(g_sysDir));
    {   int n = (int)strlen(g_sysDir);
        while (n > 3 && g_sysDir[n-1] == '\\') g_sysDir[--n] = 0;
    }
    GetModuleFileName(g_hInst, g_appDir, sizeof(g_appDir));
    p = strrchr(g_appDir, '\\');
    if (p) *p = 0;
    JoinPath(g_backupIni, g_winDir, "OMNININE\\backup.ini");
}

/* ---------------- 注册表: 按 template.inf 语义 ---------------- */

static void SetSZ(HKEY hk, const char *name, const char *val)
{
    RegSetValueEx(hk, name, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
}

static void SetDW(HKEY hk, const char *name, DWORD val)
{
    RegSetValueEx(hk, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
}

/* 找到并(必要时)创建显示类实例键; 不再返回句柄, 避免调用者传 NULL 造成解引用崩溃 */
static BOOL EnsureInstance(void)
{
    HKEY root, hk;
    DWORD i;
    char name[64];
    BOOL found = FALSE;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, DISP_KEY, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        for (i = 0; ; i++) {
            DWORD len = sizeof(name);
            HKEY sub;
            if (RegEnumKeyEx(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (RegOpenKeyEx(root, name, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
                DWORD type = 0;
                if (RegQueryValueEx(sub, "Ver", NULL, &type, NULL, NULL) == ERROR_SUCCESS ||
                    RegQueryValueEx(sub, "DevLoader", NULL, &type, NULL, NULL) == ERROR_SUCCESS) {
                    lstrcpyn(g_instance, name, sizeof(g_instance));
                    found = TRUE;
                    RegCloseKey(sub);
                    break;
                }
                RegCloseKey(sub);
            }
        }
        RegCloseKey(root);
    }
    if (!found) lstrcpyn(g_instance, "0000", sizeof(g_instance));

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, DISP_KEY, 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hk, NULL) != ERROR_SUCCESS) {
        Log("ERROR: cannot open %s", DISP_KEY);
        return FALSE;
    }
    RegCloseKey(hk);
    return TRUE;
}

static BOOL OpenInstanceKey(HKEY *out)
{
    char path[512];
    wsprintf(path, "%s\\%s", DISP_KEY, g_instance);
    return RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0,
                          KEY_ALL_ACCESS, NULL, out, NULL) == ERROR_SUCCESS;
}

static void BackupOld(void)
{
    HKEY hk;
    char buf[512];
    DWORD cb, type;

    if (!OpenInstanceKey(&hk)) return;

    buf[0] = 0; cb = sizeof(buf); type = 0;
    if (RegQueryValueEx(hk, "Ver", NULL, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS)
        WritePrivateProfileString("old", "Ver", buf, g_backupIni);
    buf[0] = 0; cb = sizeof(buf); type = 0;
    if (RegQueryValueEx(hk, "DevLoader", NULL, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS)
        WritePrivateProfileString("old", "DevLoader", buf, g_backupIni);

    RegCloseKey(hk);
    ExportInstance();
    WritePrivateProfileString("old", "Instance", g_instance, g_backupIni);
}

/* template.inf 的 DelReg: 清 Ver/DevLoader 值 + DEFAULT/MODES/CURRENT 子键 */
static void DeleteOldReg(void)
{
    HKEY hk;

    if (!OpenInstanceKey(&hk)) return;

    RegDeleteValue(hk, "Ver");
    RegDeleteValue(hk, "DevLoader");

    RegCloseKey(hk);

    {
        char path[512];
        HKEY k;
        wsprintf(path, "%s\\%s\\DEFAULT", DISP_KEY, g_instance);
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
            RegDeleteKey(HKEY_LOCAL_MACHINE, path);
            RegCloseKey(k);
        }
        wsprintf(path, "%s\\%s\\MODES", DISP_KEY, g_instance);
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
            RegDeleteKey(HKEY_LOCAL_MACHINE, path);
            RegCloseKey(k);
        }
        wsprintf(path, "%s\\%s\\CURRENT", DISP_KEY, g_instance);
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
            RegDeleteKey(HKEY_LOCAL_MACHINE, path);
            RegCloseKey(k);
        }
    }
    Log("registry: old entries cleared (DelReg semantics)");
}

/* 把显示实例重置为"标准 VGA": 卸载后若没有备份, 必须让注册表不再指向 VBE9X,
 * 否则下次启动会出现"设备管理器仍显示 VBE9X、实际退到 VGA 4bpp"的坏状态。 */
static void CleanOurKeys(void)
{
    HKEY hk, dk;

    if (!OpenInstanceKey(&hk)) return;

    RegDeleteValue(hk, "DrvFileName");
    RegDeleteValue(hk, "InfSection");
    RegDeleteValue(hk, "InfPath");
    RegDeleteValue(hk, "ProviderName");
    RegDeleteValue(hk, "DriverDesc");
    SetSZ(hk, "Ver", "4.0");
    SetSZ(hk, "DevLoader", "*vdd");

    if (RegCreateKeyEx(hk, "DEFAULT", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dk, NULL) == ERROR_SUCCESS) {
        SetSZ(dk, "Mode", "4,640,480");
        SetSZ(dk, "drv", "vga.drv");
        SetSZ(dk, "vdd", "*vdd");
        RegDeleteValue(dk, "minivdd");
        SetDW(dk, "RefreshRate", 0xFFFFFFFFUL);
        RegCloseKey(dk);
    }
    RegCloseKey(hk);
    Log("registry: reset display instance to standard VGA (vga.drv)");
}

/* 枚举 Enum 树里所有 Driver="Display\<NNNN>" 的绑定, 为每个 NNNN 建立/写入实例。
 * 9x 启动时按 Enum 节点的 Driver 值去 Services\Class\Display\<NNNN> 取驱动;
 * 若该实例在卸载时被删掉, 启动就退到 VGA —— 这正是"文件已装却 640x480x4bpp"的成因。 */
static void ApplyTemplateTo(const char *inst);

static void ScanEnumBindings(HKEY parent, const char *path, int depth)
{
    HKEY hk;
    DWORD i;
    char names[64][128];
    int n = 0;

    if (depth > 4) return;
    if (RegOpenKeyEx(parent, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) return;

    {   char drv[256];
        DWORD cb = sizeof(drv), type = 0;
        drv[0] = 0;
        if (RegQueryValueEx(hk, "Driver", NULL, &type, (LPBYTE)drv, &cb) == ERROR_SUCCESS
            && strnicmp(drv, "Display\\", 8) == 0 && drv[8]) {
            Log("enum binding: %s -> instance %s", path, drv + 8);
            ApplyTemplateTo(drv + 8);
        }
    }

    for (i = 0; i < 64; i++) {
        DWORD len = sizeof(names[i]);
        if (RegEnumKeyEx(hk, i, names[i], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        n++;
    }
    RegCloseKey(hk);

    for (i = 0; i < n; i++) {
        char sub[MAX_PATH];
        wsprintf(sub, "%s\\%s", path, names[i]);
        ScanEnumBindings(parent, sub, depth + 1);
    }
}

static void ApplyTemplateTo(const char *inst)
{
    char path[MAX_PATH], sub[MAX_PATH];
    HKEY hk, dk;
    static const char *bpp[5] = { "4","8","16","24","32" };
    static const char *res[4] = { "640,480","800,600","1024,768","1280,1024" };
    int i, j;

    wsprintf(path, "%s\\%s", DISP_KEY, inst);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) != ERROR_SUCCESS) {
        Log("WARN: cannot open instance %s", inst);
        return;
    }
    SetSZ(hk, "Ver", "4.0");
    SetSZ(hk, "DevLoader", "*vdd");

    if (RegCreateKeyEx(hk, "DEFAULT", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dk, NULL) == ERROR_SUCCESS) {
        SetSZ(dk, "Mode", "16,1024,768");
        SetSZ(dk, "drv", "omninine.drv");
        SetSZ(dk, "vdd", "*vdd,*vflatd");
        SetSZ(dk, "minivdd", "minivdd.vxd");
        SetDW(dk, "RefreshRate", 0xFFFFFFFFUL);
        SetDW(dk, "DDC", 1);
        SetDW(dk, "ExtModeSwitch", 0);
        RegCloseKey(dk);
    }

    for (i = 0; i < 5; i++) {
        for (j = 0; j < 4; j++) {
            HKEY mk;
            wsprintf(sub, "%s\\%s\\MODES\\%s\\%s", DISP_KEY, inst, bpp[i], res[j]);
            if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &mk, NULL) == ERROR_SUCCESS)
                RegCloseKey(mk);
        }
    }
    RegCloseKey(hk);
    Log("registry: applied OmniNine settings to instance %s", inst);
}

/* 对所有现有显示实例都写一遍: 9x 真正加载哪个由 Enum 节点绑定决定,
 * 只写"找到的第一个"可能写着 0000 而实际绑定 0001 —— 这正是"文件已装、
 * 设备名显示我们的、分辨率却仍是 640x480x4bpp"的成因之一。 */
static void ApplyTemplate(void)
{
    HKEY root;
    char names[64][64];
    int n = 0, i;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, DISP_KEY, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        for (i = 0; i < 64; i++) {
            DWORD len = sizeof(names[i]);
            if (RegEnumKeyEx(root, i, names[i], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            n++;
        }
        RegCloseKey(root);
    }
    if (n == 0) { ApplyTemplateTo(g_instance); return; }

    for (i = 0; i < n; i++) ApplyTemplateTo(names[i]);

    /* 还可能有"Enum 绑定到某个已不存在的实例"的情形: 按绑定补建 */
    ScanEnumBindings(HKEY_LOCAL_MACHINE, "Enum", 0);
}

static void RestoreOld(void)
{
    HKEY hk;
    char ver[64], dev[64], inst[64];

    GetPrivateProfileString("old", "Instance", "", inst, sizeof(inst), g_backupIni);
    if (!inst[0]) { Log("no backup found (registry left unchanged)"); return; }
    lstrcpyn(g_instance, inst, sizeof(g_instance));

    GetPrivateProfileString("old", "Ver", "", ver, sizeof(ver), g_backupIni);
    GetPrivateProfileString("old", "DevLoader", "", dev, sizeof(dev), g_backupIni);

    if (OpenInstanceKey(&hk)) {
        if (ver[0]) SetSZ(hk, "Ver", ver); else RegDeleteValue(hk, "Ver");
        if (dev[0]) SetSZ(hk, "DevLoader", dev); else RegDeleteValue(hk, "DevLoader");
        RegCloseKey(hk);
        Log("registry: instance %s restored", g_instance);
    }
}

/* ---------------- 注册表整树备份/还原 ----------------
 * 安装时把当前显示实例(含 DEFAULT/MODES/CURRENT 等子键)整体复制到
 *   HKLM\...\Class\Display\__OMNININEBAK_<实例>
 * 卸载时先删实例键, 再把备份整树复制回来 —— 这样才是"恢复原驱动"。
 */
static void RegDeleteTree(const char *path)
{
    HKEY hk;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &hk) != ERROR_SUCCESS)
        return;
    for (;;) {
        char name[128];
        DWORD len = sizeof(name);
        if (RegEnumKeyEx(hk, 0, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        {
            char sub[512];
            wsprintf(sub, "%s\\%s", path, name);
            RegDeleteTree(sub);
        }
    }
    RegCloseKey(hk);
    RegDeleteKey(HKEY_LOCAL_MACHINE, path);
}

static BOOL RegCopyTree(HKEY src, HKEY dst, int depth)
{
    DWORD i;
    if (depth > 8) return TRUE;

    for (i = 0; ; i++) {
        char name[128];
        DWORD nlen = sizeof(name), type = 0;
        if (RegEnumValue(src, i, name, &nlen, NULL, &type, NULL, NULL) != ERROR_SUCCESS) break;
        {
            BYTE data[2048];
            DWORD cb = sizeof(data);
            if (RegQueryValueEx(src, name, NULL, &type, data, &cb) == ERROR_SUCCESS)
                RegSetValueEx(dst, name, 0, type, data, cb);
        }
    }
    for (i = 0; ; i++) {
        char name[128];
        DWORD len = sizeof(name);
        HKEY s, d;
        if (RegEnumKeyEx(src, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyEx(src, name, 0, KEY_READ, &s) != ERROR_SUCCESS) continue;
        if (RegCreateKeyEx(dst, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &d, NULL) == ERROR_SUCCESS) {
            RegCopyTree(s, d, depth + 1);
            RegCloseKey(d);
        }
        RegCloseKey(s);
    }
    return TRUE;
}

static void BackupKeyName(char *out)
{
    wsprintf(out, "%s\\__OMNININEBAK_%s", DISP_KEY, g_instance);
}

static void ExportInstance(void)
{
    HKEY src, dst;
    char bak[512];

    BackupKeyName(bak);
    RegDeleteTree(bak);

    if (!OpenInstanceKey(&src)) return;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, bak, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dst, NULL) != ERROR_SUCCESS) {
        RegCloseKey(src);
        Log("WARN: cannot create registry backup key");
        return;
    }
    RegCopyTree(src, dst, 0);
    RegCloseKey(dst);
    RegCloseKey(src);
    Log("registry: instance %s backed up to %s", g_instance, bak);
}

static BOOL RestoreInstanceTree(void)
{
    HKEY src, dst;
    char bak[512], inst[512];

    BackupKeyName(bak);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, bak, 0, KEY_READ, &src) != ERROR_SUCCESS) {
        Log("no registry backup found");
        return FALSE;
    }
    wsprintf(inst, "%s\\%s", DISP_KEY, g_instance);

    RegDeleteTree(inst);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, inst, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dst, NULL) != ERROR_SUCCESS) {
        RegCloseKey(src);
        Log("ERROR: cannot recreate instance key");
        return FALSE;
    }
    RegCopyTree(src, dst, 0);
    RegCloseKey(dst);
    RegCloseKey(src);
    RegDeleteTree(bak);
    Log("registry: instance %s restored from backup", g_instance);
    return TRUE;
}

/* 从 WININIT.INI 去掉与本次安装/卸载相关的行 */
static void CleanWinInit(void)
{
    char path[MAX_PATH], *buf = NULL, *out, *q, *e;
    DWORD size = 0, cap, osz = 0;
    BOOL changed = FALSE;

    JoinPath(path, g_winDir, "WININIT.INI");
    if (!LoadFile(path, &buf, &size)) return;

    cap = size + 16;
    out = (char *)malloc(cap);
    if (!out) { free(buf); return; }

    q = buf;
    while (*q) {
        char tmp[1024];
        int n, skip = 0;
        e = q;
        while (*e && *e != '\r' && *e != '\n') e++;
        n = (int)(e - q);
        if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
        memcpy(tmp, q, n); tmp[n] = 0;
        if (HasNoCase(tmp, "omninine") || HasNoCase(tmp, "minivdd") || HasNoCase(tmp, "fixsys")) {
            skip = 1; changed = TRUE;
        }
        if (!skip && osz + n + 3 < cap) {
            memcpy(out + osz, tmp, n); osz += n; out[osz++] = '\r'; out[osz++] = '\n';
        }
        while (*e == '\r' || *e == '\n') e++;
        q = e;
    }
    out[osz] = 0;
    if (changed) {
        SaveFile(path, out, osz);
        Log("WININIT.INI: our entries removed");
    }
    free(out); free(buf);
}

/* ---------------- 文件 ---------------- */

static BOOL CopyTo(const char *sub, const char *name, const char *dstDir)
{
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];

    JoinPath(src, g_appDir, sub);
    JoinPath(src, src, name);
    JoinPath(dst, dstDir, name);
    if (!FileExists(src)) { Log("ERROR: installation source missing: %s", src); return FALSE; }
    if (!CopyFile(src, dst, FALSE)) {
        Log("ERROR: copy %s -> %s failed (%lu)", src, dst, GetLastError());
        return FALSE;
    }
    Log("copied %s", dst);
    return TRUE;
}

/* 前置声明: 下面 WININIT 逻辑要用 */
static BOOL LoadFile(const char *path, char **buf, DWORD *size);
static BOOL SaveFile(const char *path, const char *buf, DWORD size);

/* ---------- 重启时替换: WININIT.INI ----------
 * Win9x 在关机/重启时读取 <Windows>\WININIT.INI 的 [rename] 段:
 *     destination=source     把 source 改名为 destination(覆盖)
 *     NUL=destination        删除 destination
 * 文件被驱动/系统占用时, 这是唯一可靠的替换方式。
 * WININIT.INI 不是标准 INI(不能用 WritePrivateProfileString 写)。
 */
static BOOL AddWinInitEntry(const char *dst, const char *src)
{
    char path[MAX_PATH], *old = NULL, *out;
    DWORD size = 0, cap, osz = 0;
    BOOL hasSection = FALSE;
    char line[MAX_PATH * 3];

    JoinPath(path, g_winDir, "WININIT.INI");
    if (!LoadFile(path, &old, &size)) { size = 0; old = (char *)malloc(1); old[0] = 0; }

    cap = size + (DWORD)(strlen(dst) * 2 + (src ? strlen(src) : 0) + 256);
    out = (char *)malloc(cap);
    if (!out) { free(old); return FALSE; }
    out[0] = 0;

    if (old && old[0]) {
        char *q = old;
        while (*q) {
            char *e = q;
            char tmp[512];
            int n;
            while (*e && *e != '\r' && *e != '\n') e++;
            n = (int)(e - q);
            if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
            memcpy(tmp, q, n); tmp[n] = 0;
            if (HasNoCase(tmp, "[rename]")) hasSection = TRUE;
            if (osz + n + 2 < cap) { memcpy(out + osz, tmp, n); osz += n; out[osz++] = '\r'; out[osz++] = '\n'; }
            while (*e == '\r' || *e == '\n') e++;
            q = e;
        }
    }
    if (!hasSection && osz + 10 < cap) { strcpy(out + osz, "[rename]\r\n"); osz += 10; }

    /* 顺序必须【先删后改名】: WININIT 按行顺序处理,
       若先写 rename 再写 NUL, 会在改名成功之后把新文件删掉 —— 这正是
       "重启后驱动消失/退到 VGA、CONFIG.SYS 报找不到 FIXSYS.SYS" 的根因。 */
    wsprintf(line, "NUL=%s\r\n", dst);
    if (osz + strlen(line) + 1 < cap) { strcpy(out + osz, line); osz += (DWORD)strlen(line); }
    if (src) {
        wsprintf(line, "%s=%s\r\n", dst, src);
        if (osz + strlen(line) + 1 < cap) { strcpy(out + osz, line); osz += (DWORD)strlen(line); }
    }

    out[osz] = 0;
    if (!SaveFile(path, out, osz)) { Log("ERROR: cannot write WININIT.INI"); free(old); free(out); return FALSE; }
    free(old); free(out);
    Log("WININIT: %s %s", src ? "replace" : "delete", dst);
    g_deferred = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------
 * 用 INF + SetupX 替换被占用的文件 —— 微软 KB Q172456 的官方做法:
 *   [CopyFiles] 行格式为 "目标名, 源名, 临时名";
 *   SetupX 把源文件拷成"临时名"放进目标目录, 并在 WININIT.INI 里登记
 *   "临时名 -> 目标名", 由 WININIT 在【下次开机、驱动加载之前】完成改名。
 *   这比自己手写 WININIT 可靠(顺序/删除语义交给系统)。
 * ------------------------------------------------------------------ */
static BOOL ReplaceViaInf(const char *sub, const char *name)
{
    char dir[MAX_PATH], src[MAX_PATH], stage[MAX_PATH], inf[MAX_PATH], cmd[MAX_PATH * 3];
    char infText[2048];
    char srcName[MAX_PATH];
    char dstName[MAX_PATH];
    char tmpName[MAX_PATH];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    BOOL ok = FALSE;

    /* 临时目录: <Windows>\TEMP\OMNININEREP */
    JoinPath(dir, g_winDir, "TEMP");
    CreateDirectory(dir, NULL);
    {   char sub2[MAX_PATH];
        JoinPath(sub2, dir, "OMNININEREP");
        strcpy(dir, sub2);
        CreateDirectory(dir, NULL);
    }

    /* 源文件 -> 临时目录, 保持原文件名(SetupX 按 [SourceDisksFiles] 找) */
    if (!FindSourceFile(sub, name, src)) return FALSE;
    lstrcpyn(srcName, name, sizeof(srcName));
    JoinPath(stage, dir, srcName);
    if (!CopyFile(src, stage, FALSE)) {
        Log("ERROR: cannot stage source for INF: %lu", GetLastError());
        return FALSE;
    }

    /* 目标名, 临时名(8.3) */
    lstrcpyn(dstName, name, sizeof(dstName));
    lstrcpyn(tmpName, name, sizeof(tmpName));
    {   char *d = strchr(tmpName, '.'); if (d) *d = 0; }
    strcat(tmpName, ".TMP");

    sprintf(infText,
        "[Version]\r\nSignature=\"$CHICAGO$\"\r\nAdvancedINF=2.0\r\n\r\n"
        "[DefaultInstall]\r\nCopyFiles=OMNININE.Copy\r\n\r\n"
        "[DestinationDirs]\r\nOMNININE.Copy=11\r\n\r\n"
        "[OMNININE.Copy]\r\n%s, %s, %s\r\n\r\n"
        "[SourceDisksNames]\r\n55=\"OmniNine\",,1\r\n\r\n"
        "[SourceDisksFiles]\r\n%s=55\r\n",
        dstName, srcName, tmpName, srcName);

    JoinPath(inf, dir, "REPLACE.INF");
    if (!SaveFile(inf, infText, (DWORD)strlen(infText))) {
        Log("ERROR: cannot write REPLACE.INF");
        return FALSE;
    }
    Log("INF replace: %s -> %s (temp %s)", srcName, dstName, tmpName);

    wsprintf(cmd, "rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 130 \"%s\"", inf);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = TRUE;
        Log("SetupX invoked (INF replace scheduled; takes effect at next boot)");
        g_deferred = TRUE;
    } else {
        Log("ERROR: cannot run rundll32/SetupX (%lu)", GetLastError());
    }
    return ok;
}

/* 拷贝; 被占用时按三级策略处理(专治 fixsys.sys 这类被 DOS 设备驱动占用的文件):
 *   1. 直接覆盖                     —— 文件没被占用时首选
 *   2. 先改名 .OLD 再写新文件        —— 关键: 改名通常被允许, 而删除/覆盖不行;
 *                                      这样新文件【立刻就位】, 不会留下"重启后文件不见"的窗口
 *   3. 暂存 .NEW + WININIT.INI      —— 改名也失败时的最后手段(重启时替换)
 */
/* 在安装介质里找文件: 先按给定子目录, 再依次试 common/rel/dbg/根目录。
 * 目的: 不同人打出的安装包布局可能不同(fixsys 放 common 还是 rel/dbg),
 *       这里给出确定的搜索顺序, 并把命中的路径写进日志。 */
static BOOL FindSourceFile(const char *preferSub, const char *name, char *out)
{
    static const char *subs[3] = { "common", "rel", "dbg" };
    char cand[MAX_PATH * 2];
    DWORD e = 0;
    int k;

    JoinPath(cand, g_appDir, preferSub ? preferSub : "common");
    JoinPath(cand, cand, name);
    if (ProbeExists(cand, &e)) { strcpy(out, cand); Log("source: %s (err=%lu)", out, e); return TRUE; }
    Log("  probe %s -> err=%lu", cand, e);

    for (k = 0; k < 3; k++) {
        if (preferSub && lstrcmpi(subs[k], preferSub) == 0) continue;
        JoinPath(cand, g_appDir, subs[k]);
        JoinPath(cand, cand, name);
        e = 0;
        if (ProbeExists(cand, &e)) { strcpy(out, cand); Log("source fallback: %s (err=%lu)", out, e); return TRUE; }
        Log("  probe %s -> err=%lu", cand, e);
    }

    JoinPath(cand, g_appDir, name);
    e = 0;
    if (ProbeExists(cand, &e)) { strcpy(out, cand); Log("source fallback: %s (err=%lu)", out, e); return TRUE; }
    Log("  probe %s -> err=%lu", cand, e);

    Log("ERROR: source not found for %s (see probe lines above)", name);
    return FALSE;
}

static BOOL CopyWithReplace(const char *sub, const char *name, const char *dstDir)
{
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];

    JoinPath(dst, dstDir, name);
    if (!FindSourceFile(sub, name, src)) return FALSE;
    if (CopyFile(src, dst, FALSE)) { Log("copied %s", dst); return TRUE; }

    Log("ERROR: cannot write %s (err=%lu)", dst, GetLastError());
    Log("       old driver is probably still loaded");
    Log("       uninstall it, reboot, then run Setup again");
    return FALSE;
}

/* 删除; 失败则登记为重启时删除 */
static int g_delFail = 0;

static void DeleteOrDefer(const char *path)
{
    if (!FileExists(path)) return;
    SetFileAttributes(path, FILE_ATTRIBUTE_NORMAL);   /* INF 常为只读/隐藏 */
    if (DeleteFile(path)) { Log("deleted %s", path); return; }
    Log("WARN: cannot delete %s (err=%lu)", path, GetLastError());
    g_delFail++;
}

static BOOL LoadFile(const char *path, char **buf, DWORD *size)
{
    HANDLE h;
    DWORD sz, rd;
    *buf = NULL; *size = 0;
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    sz = GetFileSize(h, NULL);
    if (sz == 0xFFFFFFFFUL) { CloseHandle(h); return FALSE; }
    *buf = (char *)malloc(sz + 2);
    if (!*buf) { CloseHandle(h); return FALSE; }
    if (!ReadFile(h, *buf, sz, &rd, NULL)) { free(*buf); *buf = NULL; CloseHandle(h); return FALSE; }
    (*buf)[rd] = 0; *size = rd;
    CloseHandle(h);
    return TRUE;
}

static BOOL SaveFile(const char *path, const char *buf, DWORD size)
{
    HANDLE h;
    DWORD wr;
    h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (!WriteFile(h, buf, size, &wr, NULL) || wr != size) { CloseHandle(h); return FALSE; }
    CloseHandle(h);
    return TRUE;
}

/* ---------------- CONFIG.SYS ---------------- */

/* 找到 DOS 真正使用的那份 CONFIG.SYS:
 *   1) %CONFIG% 环境变量(最权威; 但 GUI 进程里可能读不到)
 *   2) 引导盘 = 根目录同时存在 IO.SYS 与 CONFIG.SYS 的固定盘
 *   3) 回退: Windows 所在盘根
 * 说明: 9x 的引导盘可以用 IO.SYS 可靠识别, 不是"逐盘瞎猜"。
 */
static void ConfigSysPath(char *out)
{
    char env[MAX_PATH], root[MAX_PATH], probe[MAX_PATH], io[MAX_PATH];
    int d;

    env[0] = 0;
    if (GetEnvironmentVariable("CONFIG", env, sizeof(env)) > 0 && FileExists(env)) {
        strcpy(out, env);
        Log("CONFIG.SYS from %%CONFIG%% = %s", out);
        return;
    }

    for (d = 'C'; d <= 'Z'; d++) {
        char r[8];
        wsprintf(r, "%c:\\", d);
        if (GetDriveType(r) != DRIVE_FIXED) continue;
        JoinPath(io, r, "IO.SYS");
        if (!FileExists(io)) continue;
        JoinPath(probe, r, "CONFIG.SYS");
        if (FileExists(probe)) {
            strcpy(out, probe);
            Log("CONFIG.SYS on boot drive (IO.SYS) = %s", out);
            return;
        }
    }

    strcpy(root, g_winDir);
    if (root[1] == ':') root[3] = 0; else strcpy(root, "C:\\");
    JoinPath(out, root, "CONFIG.SYS");
    Log("WARN: boot CONFIG.SYS not located; fallback %s", out);
}

/* 打印该 CONFIG.SYS 里现有的 FIXSYS 行(诊断用, 便于发现旧安装残留) */
static void DumpFixsysLines(const char *cfgPath)
{
    char *buf = NULL;
    DWORD size = 0, i = 0;

    if (!LoadFile(cfgPath, &buf, &size)) return;
    while (i < size) {
        char tmp[512];
        DWORD n = 0;
        while (i < size && buf[i] != '\r' && buf[i] != '\n' && n < sizeof(tmp) - 1)
            tmp[n++] = buf[i++];
        tmp[n] = 0;
        while (i < size && (buf[i] == '\r' || buf[i] == '\n')) i++;
        if (HasNoCase(tmp, "FIXSYS")) Log("  existing line: %s", tmp);
    }
    free(buf);
}

static BOOL RewriteConfigSys(BOOL fixsys, int devHigh)
{
    char path[MAX_PATH], bak[MAX_PATH], line[MAX_PATH * 2];
    char *buf = NULL, *out, *p, *e;
    DWORD size = 0, osz = 0, cap;
    BOOL ok = TRUE;

    ConfigSysPath(path);
    strcpy(bak, path);
    strcat(bak, ".VBE");

    if (!LoadFile(path, &buf, &size)) {
        Log("WARN: %s missing; creating new", path);
        buf = (char *)malloc(1); buf[0] = 0; size = 0;
    } else {
        char cb[MAX_PATH];
        strcpy(cb, path); strcat(cb, ".BAK");
        if (!SaveFile(cb, buf, size)) {
            Log("WARN: %s busy; try .BA0", cb);
            strcpy(cb, path); strcat(cb, ".BA0");
            if (!SaveFile(cb, buf, size)) {
                Log("WARN: .BA0 busy too; overwrite .BAK");
                strcpy(cb, path); strcat(cb, ".BAK");
                SaveFile(cb, buf, size);
            } else Log("CONFIG.SYS backed up to %s", cb);
        } else Log("CONFIG.SYS backed up to %s", cb);
    }

    cap = size + 1024;
    out = (char *)malloc(cap);
    if (!out) { free(buf); return FALSE; }
    out[0] = 0;

    p = buf;
    while (p && *p) {
        char tmp[1024];
        int n, keep = 1;
        e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        n = (int)(e - p);
        if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
        memcpy(tmp, p, n); tmp[n] = 0;
        if (HasNoCase(tmp, "FIXSYS.SYS")) keep = 0;
        if (keep && osz + n + 3 < cap) {
            memcpy(out + osz, tmp, n); osz += n;
            out[osz++] = '\r'; out[osz++] = '\n';
        }
        while (*e == '\r' || *e == '\n') e++;
        p = e;
    }

    if (fixsys) {
        strcpy(line, devHigh ? "DEVICEHIGH=" : "DEVICE=");
        strcat(line, g_winDir);
        strcat(line, "\\FIXSYS.SYS\r\n");
        if (osz + strlen(line) + 1 < cap) { strcpy(out + osz, line); osz += (DWORD)strlen(line); }
        Log("CONFIG.SYS line: %s", line);
        Log("  (system dir from GetSystemDirectory = %s)", g_sysDir);
    } else {
        Log("CONFIG.SYS: FIXSYS line removed");
    }
    out[osz] = 0;

    if (!SaveFile(path, out, osz)) { Log("ERROR: cannot write CONFIG.SYS"); ok = FALSE; }
    else Log("CONFIG.SYS updated");

    free(out); free(buf);
    return ok;
}

/* ---------------- INF ---------------- */


/* ---- r42: 生成"首选" INF: 把本机 HardwareID/CompatibleIDs 写进 Models ---- */
static void AppendModelLine(char *out, int cb, const char *id)
{
    if (!id || !id[0]) return;
    if ((int)strlen(out) + (int)strlen(id) + 48 < cb) {
        strcat(out, "%OmniNine.Desc% = OmniNine.Install, ");
        strcat(out, id);
        strcat(out, "\r\n");
    }
}

static void AppendIdToken(char *out, int cb, const char *id)
{
    char tok[256];
    char *s, *e;

    lstrcpyn(tok, id, sizeof(tok));
    s = tok;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    if (!*s) return;
    if (strnicmp(s, "PCI\\CC_", 7) == 0 && strlen(s) > 11) s[11] = 0;
    AppendModelLine(out, cb, s);
}

static void AppendIdsSplit(char *out, int cb, const char *id)
{
    char tmp[1024];
    char *p, *q;

    lstrcpyn(tmp, id, sizeof(tmp));
    p = tmp;
    while (p && *p) {
        q = strchr(p, ',');
        if (q) *q = 0;
        AppendIdToken(out, cb, p);
        if (!q) break;
        p = q + 1;
    }
}

static void AppendIdsFromKey(HKEY hk, char *out, int cb)
{
    static const char *names[2] = { "HardwareID", "CompatibleIDs" };
    int k;
    for (k = 0; k < 2; k++) {
        BYTE buf[2048];
        DWORD sz = sizeof(buf), type = 0;
        if (RegQueryValueEx(hk, names[k], NULL, &type, buf, &sz) != ERROR_SUCCESS) continue;
        if (type == REG_MULTI_SZ) {
            const char *p = (const char *)buf;
            while (*p) { AppendIdsSplit(out, cb, p); p += strlen(p) + 1; }
        } else if (type == REG_SZ) {
            AppendIdsSplit(out, cb, (const char *)buf);
        }
    }
}

static void StripInfComments(char *buf)
{
    char *p = buf, *w = buf;
    while (*p) {
        char *ls = p;
        while (*p && *p != '\n') p++;
        {   char *le = p;
            int nl = 0;
            if (*p == '\n') { p++; nl = 1; }
            {   char *s = ls;
                while (s < le && (*s == ' ' || *s == '\t')) s++;
                if (!(s < le && *s == ';')) {
                    memmove(w, ls, (size_t)(le - ls));
                    w += le - ls;
                    if (nl) *w++ = '\n';
                }
            }
        }
    }
    *w = 0;
}


static void CollectIdsAt(const char *path, int depth, char *out, int cb)
{
    HKEY hk;
    DWORD i;
    char v[160];
    DWORD sz, type;
    int disp = 0;

    if (depth > 4) return;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) return;

    sz = sizeof(v); v[0] = 0;
    if (RegQueryValueEx(hk, "Class", NULL, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS
        && lstrcmpi(v, "Display") == 0) disp = 1;
    if (!disp) { sz = sizeof(v); v[0] = 0;
        if (RegQueryValueEx(hk, "ClassGUID", NULL, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS
            && lstrcmpi(v, DISP_GUID) == 0) disp = 1; }
    if (!disp && HasNoCase(path, "\\Display\\")) disp = 1;
    if (disp) AppendIdsFromKey(hk, out, cb);

    for (i = 0; ; i++) {
        char name[128], sub[512];
        DWORD len = sizeof(name);
        if (RegEnumKeyEx(hk, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wsprintf(sub, "%s\\%s", path, name);
        CollectIdsAt(sub, depth + 1, out, cb);
    }
    RegCloseKey(hk);
}

static void CollectModelLines(const char *node, char *out, int cb)
{
    out[0] = 0;
    (void)node;
    CollectIdsAt("Enum", 0, out, cb);
}


static void ReplaceOnce(char *buf, int cap, const char *from, const char *to)
{
    char *p = strstr(buf, from);
    if (p) {
        int fl = (int)strlen(from), tl = (int)strlen(to);
        int len = (int)strlen(buf);
        if (len + tl - fl < cap) {
            memmove(p + tl, p + fl, (size_t)(len - (int)(p - buf) - fl) + 1);
            memcpy(p, to, (size_t)tl);
        }
    }
}

static void InstallInf(void)
{
    char dir[MAX_PATH], dst[MAX_PATH], src[MAX_PATH], tmp[MAX_PATH];
    char models[2048];
    char *buf = NULL, *out = NULL;
    DWORD size = 0, osz = 0;
    char *mark;

    JoinPath(dir, g_winDir, "INF");
    CreateDirectory(dir, NULL);
    JoinPath(dst, dir, "OMNININE.INF");
    JoinPath(src, g_appDir, "common");
    JoinPath(src, src, "omninine.inf");
    if (!FileExists(src)) JoinPath(src, g_appDir, "omninine.inf");
    if (!FileExists(src)) { Log("WARN: omninine.inf not found in package"); return; }

    CollectModelLines(g_modelNode[0] ? g_modelNode : g_nodePath, models, sizeof(models));
    { int nl = 0; const char *q = models; while ((q = strstr(q, "OmniNine.Install")) != NULL) { nl++; q += 15; }
      Log("preferred INF: %d hardware-id model line(s) from %s", nl, g_nodePath[0] ? g_nodePath : "(none)"); }

    out = (char *)malloc(16384);
    if (out && LoadFile(src, &buf, &size)) {
        mark = strstr(buf, "[OmniNine.Models]");
        if (mark) {
            mark = strchr(mark, '\n');
            if (mark) mark++;
        }
        if (mark) {
            size_t pre = (size_t)(mark - buf);
            if (pre + strlen(models) + size - pre + 1 < 16384) {
                memcpy(out, buf, pre);
                osz = (DWORD)pre;
                memcpy(out + osz, models, strlen(models)); osz += (DWORD)strlen(models);
                memcpy(out + osz, mark, size - pre); osz += (DWORD)(size - pre);
                out[osz] = 0;
            }
        }
        free(buf);
    }
    if (!out) { Log("ERROR: out of memory for INF"); return; }
    StripInfComments(out);
    {   char from[64], to[64];
        wsprintf(from, "Class\\Display\\0000");
        wsprintf(to,   "Class\\Display\\%s", g_inst[0] ? g_inst : "0000");
        if (lstrcmpi(from, to) != 0)
            while (strstr(out, from)) ReplaceOnce(out, 16000, from, to);
    }

    SetFileAttributes(dst, FILE_ATTRIBUTE_NORMAL);
    DeleteFile(dst);
    {   /* 源路径用绝对 System 目录: 9x 可能把 subdir 拼到缓存的安装源路径上 */
        char sn[512];
        wsprintf(sn, "1 = \"OmniNine display driver files\",,0,\"%s\"", g_sysDir);
        ReplaceOnce(out, 16000, "1 = \"OmniNine display driver files\",,0", sn);
        Log("INF source path: %s", g_sysDir);
    }

    if (!SaveFile(dst, out, (DWORD)strlen(out))) {
        Log("ERROR: cannot write %s", dst);
        free(out); return;
    }
    free(out);

    Log("INF installed: %s (source files resolve to SYSTEM via ..\\SYSTEM)", dst);
}

/* ------------------------------------------------------------------
 * 卸载清理: 用 INF 的 DelReg/DelFiles(微软文档化的 INF 指令) + SetupX 执行。
 * 为什么必须连 Enum 一起删:
 *   9x 把设备节点放在 HKLM\Enum\Display\<节点>, 其中 Driver 值形如 "Display\0001",
 *   指向 HKLM\...\Services\Class\Display\0001。只删 Class 侧, 设备管理器仍会显示旧名称,
 *   并可能在下次启动时按残留节点重建 —— 这正是"卸载后仍显示 VBE9X、重装卡 4bpp"的成因。
 * 本函数枚举 Enum\Display, 凡 Driver == 我们的实例名, 就一并列入 DelReg。
 * 全部按"结构关系"推导, 不假设具体机器内容。
 * ------------------------------------------------------------------ */
static BOOL ReadKeyStr(HKEY parent, const char *subkey, const char *value, char *out, DWORD cb)
{
    HKEY k;
    DWORD type = 0, sz = cb;
    out[0] = 0;
    if (RegOpenKeyEx(parent, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
    if (RegQueryValueEx(k, value, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS) { RegCloseKey(k); return FALSE; }
    RegCloseKey(k);
    out[cb-1] = 0;
    return TRUE;
}

/* 收集所有"指向 omninine.drv"的类实例, 以及所有描述/驱动指向我们的 Enum 节点,
 * 生成 DelReg 列表交给 SetupX 执行(微软文档化的 INF 卸载方式)。
 * 判定全部基于 9x 的结构关系(Class\...\DEFAULT\drv 与 Enum\...\Driver/DeviceDesc),
 * 不针对具体机器内容。 */
static BOOL RunUninstallInf(void)
{
    static char del[8192];
    char insts[16][64];
    int ninst = 0, i;
    HKEY root;
    char infPath[MAX_PATH], infText[12288], cmd[MAX_PATH * 3];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    del[0] = 0;

    /* --- 1) 找出所有 Class\Display 实例中 DEFAULT\drv == omninine.drv 的 --- */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, DISP_KEY, 0, KEY_READ, &root) == ERROR_SUCCESS) {
        for (i = 0; ; i++) {
            char name[64], drv[128], path[512];
            DWORD len = sizeof(name);
            if (RegEnumKeyEx(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wsprintf(path, "%s\\DEFAULT", name);
            {
                char desc[256], prov[256];
                int mine = 0;
                if (ReadKeyStr(root, path, "drv", drv, sizeof(drv)) && lstrcmpi(drv, "omninine.drv") == 0)
                    mine = 1;
                if (!mine && ReadKeyStr(root, name, "DriverDesc", desc, sizeof(desc))
                          && HasNoCase(desc, "OmniNine")) mine = 1;
                if (!mine && ReadKeyStr(root, name, "ProviderName", prov, sizeof(prov))
                          && HasNoCase(prov, "OmniNine")) mine = 1;
                if (!mine) continue;
            }
            if (ninst < 16) lstrcpyn(insts[ninst++], name, sizeof(insts[0]));

            strcat(del, "HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\");
            strcat(del, name); strcat(del, "\"\r\n");
            strcat(del, "HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\__OMNININEBAK_");
            strcat(del, name); strcat(del, "\"\r\n");
            Log("uninstall: class instance %s -> omninine.drv", name);
        }
        RegCloseKey(root);
    }
    if (ninst == 0) Log("uninstall: no Class\\Display instance points to omninine.drv");

    /* --- 2) Enum\Display 节点: Driver 指向上面任一实例, 或 DeviceDesc 含 VBE9X --- */
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Enum\\Display", 0, KEY_READ, &root) == ERROR_SUCCESS) {
        for (i = 0; ; i++) {
            char name[128], drv[256], desc[256];
            DWORD len = sizeof(name);
            int k, match = 0;
            if (RegEnumKeyEx(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

            if (ReadKeyStr(root, name, "Driver", drv, sizeof(drv))) {
                for (k = 0; k < ninst; k++) {
                    char tok[80];
                    wsprintf(tok, "Display\\%s", insts[k]);
                    if (lstrcmpi(drv, tok) == 0) { match = 1; break; }
                }
            }
            if (!match && ReadKeyStr(root, name, "DeviceDesc", desc, sizeof(desc))) {
                if (HasNoCase(desc, "OmniNine") || HasNoCase(desc, "Generic VBE")) match = 1;
            }
            if (match) {
                strcat(del, "HKLM,\"Enum\\Display\\");
                strcat(del, name); strcat(del, "\"\r\n");
                Log("uninstall: Enum node %s selected", name);
            }
        }
        RegCloseKey(root);
    }

    if (!del[0]) { Log("uninstall: nothing to delete in registry"); return FALSE; }

    sprintf(infText,
        "[Version]\r\nSignature=\"$CHICAGO$\"\r\n\r\n"
        "[DefaultInstall]\r\nDelReg=OMNININE.DelReg\r\nDelFiles=OMNININE.DelFiles\r\n\r\n"
        "[DestinationDirs]\r\nOMNININE.DelFiles=11\r\n\r\n"
        "[OMNININE.DelFiles]\r\nomninine.drv\r\nminivdd.vxd\r\nfixsys.sys\r\n\r\n"
        "[OMNININE.DelReg]\r\n%s", del);

    JoinPath(infPath, g_winDir, "TEMP");
    CreateDirectory(infPath, NULL);
    {   char tmp[MAX_PATH];
        JoinPath(tmp, infPath, "OMNININEUN.INF");
        strcpy(infPath, tmp);
    }
    if (!SaveFile(infPath, infText, (DWORD)strlen(infText))) { Log("ERROR: cannot write uninstall INF"); return FALSE; }
    Log("uninstall INF: %s", infPath);

    wsprintf(cmd, "rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 132 \"%s\"", infPath);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        Log("SetupX invoked for uninstall (DelReg/DelFiles)");
        return TRUE;
    }
    Log("ERROR: cannot run SetupX for uninstall (%lu)", GetLastError());
    return FALSE;
}

/* ---- 递归扫描并删除"带我们标记"的注册表子树 ----
 * 判据: 该键的任一字符串值包含 "OmniNine" 或 "Generic VBE"。
 * 9x 的 Enum\Display 可能是两层(Enum\Display\<id>\<实例>), 只查第一层会漏,
 * 所以递归到深度 4; 命中即整键删除(含子键)。
 */
static BOOL StrHasOurMark(const char *s)
{
    return HasNoCase(s, "OmniNine") || HasNoCase(s, "VBE9X") || HasNoCase(s, "Generic VBE");
}

/* 整棵子树里是否有任一字符串值带我们的标记(只读检查) */
static BOOL TreeHasMark(const char *path, int depth)
{
    HKEY hk;
    DWORD i;
    char names[64][128];
    int n = 0;

    if (depth > 4) return FALSE;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) return FALSE;

    for (i = 0; ; i++) {
        char vn[128];
        DWORD len = sizeof(vn), type = 0, cb = 512;
        BYTE data[512];
        if (RegEnumValue(hk, i, vn, &len, NULL, &type, data, &cb) != ERROR_SUCCESS) break;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            data[cb < 511 ? cb : 511] = 0;
            if (StrHasOurMark((char *)data)) { RegCloseKey(hk); return TRUE; }
        }
    }
    for (i = 0; i < 64; i++) {
        DWORD len = sizeof(names[i]);
        if (RegEnumKeyEx(hk, i, names[i], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        n++;
    }
    RegCloseKey(hk);

    for (i = 0; i < n; i++) {
        char sub[MAX_PATH];
        wsprintf(sub, "%s\\%s", path, names[i]);
        if (TreeHasMark(sub, depth + 1)) return TRUE;
    }
    return FALSE;
}

static int g_delKeys = 0;

static void ScanDeleteTree(const char *path, int depth)
{
    HKEY hk;
    DWORD i;
    BOOL marked = FALSE;
    char names[64][128];
    int n = 0;

    if (depth > 4) return;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) return;

    for (i = 0; ; i++) {
        char vn[128];
        DWORD len = sizeof(vn), type = 0, cb = 512;
        BYTE data[512];
        if (RegEnumValue(hk, i, vn, &len, NULL, &type, data, &cb) != ERROR_SUCCESS) break;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            data[cb < 511 ? cb : 511] = 0;
            if (StrHasOurMark((char *)data)) { marked = TRUE; break; }
        }
    }
    RegCloseKey(hk);

    if (marked) {
        RegDeleteTree(path);
        g_delKeys++;
        Log("uninstall: removed key %s", path);
        return;
    }

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hk) != ERROR_SUCCESS) return;
    for (i = 0; i < 64; i++) {
        DWORD len = sizeof(names[i]);
        if (RegEnumKeyEx(hk, i, names[i], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        n++;
    }
    RegCloseKey(hk);

    for (i = 0; i < n; i++) {
        char sub[MAX_PATH];
        wsprintf(sub, "%s\\%s", path, names[i]);
        ScanDeleteTree(sub, depth + 1);
    }
}

/* ================= Enum 显示节点扫描与绑定 (r32) =================
 * Windows 9x 的 Enum 结构是两级:
 *     Enum\Display\<设备ID>\<实例>        (实例键里才有 Driver/DeviceDesc)
 * 旧代码只在第一级找 Driver, 永远找不到 -> 不会写绑定 -> 重启后仍是 VGA。
 * 下面按两级扫描, 并直接写 Driver/ConfigFlags (不再只依赖 SetupX)。
 */


static BOOL OpenClassInst(const char *inst, HKEY *out)
{
    char path[512];
    wsprintf(path, "System\\CurrentControlSet\\Services\\Class\\Display\\%s", inst);
    return RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, out, NULL) == ERROR_SUCCESS;
}

static void SetDwordVal(HKEY hk, const char *name, DWORD v)
{
    RegSetValueEx(hk, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
}

/* 把显示类实例写成"用 omninine.drv"的完整值 */
static void WriteClassValues(const char *inst)
{
    HKEY hk;
    char path[512];

    if (!OpenClassInst(inst, &hk)) return;
    SetSZ(hk, "Ver", "4.0");
    SetSZ(hk, "DevLoader", "*vdd");
    SetSZ(hk, "InfPath", "omninine.inf");
    SetSZ(hk, "InfSection", "OMNININE.Install");
    SetSZ(hk, "DeviceDesc", "Generic VBE (LFB) display adapter");
    SetSZ(hk, "DriverDesc", "Generic VBE (LFB) display adapter");
    SetSZ(hk, "Mfg", "OmniNine");
    SetSZ(hk, "ProviderName", "OmniNine");
    RegCloseKey(hk);

    wsprintf(path, "System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT", inst);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
        SetSZ(hk, "Mode", "16,1024,768");
        SetSZ(hk, "drv", "omninine.drv");
        SetSZ(hk, "vdd", "*vdd,*vflatd");
        SetSZ(hk, "minivdd", "minivdd.vxd");
        SetDwordVal(hk, "RefreshRate", 0xFFFFFFFFUL);
        SetDwordVal(hk, "DDC", 1);
        SetDwordVal(hk, "ExtModeSwitch", 0);
        RegCloseKey(hk);
    }
    Log("registry: class instance %s set to omninine.drv", inst);
}

/* 把 Enum 里的显示节点绑定到我们的实例 */
static BOOL BindNode(const char *node, const char *inst)
{
    HKEY hk;
    char drv[64];

    if (!node || !node[0]) return FALSE;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, node, 0, KEY_ALL_ACCESS, &hk) != ERROR_SUCCESS)
        return FALSE;
    wsprintf(drv, "Display\\%s", inst);
    RegSetValueEx(hk, "Driver", 0, REG_SZ, (const BYTE *)drv, (DWORD)strlen(drv) + 1);
    SetDwordVal(hk, "ConfigFlags", 0);
    SetSZ(hk, "Class", "Display");
    SetSZ(hk, "ClassGUID", DISP_GUID);
    SetSZ(hk, "InfPath", "omninine.inf");
    SetSZ(hk, "InfSection", "OmniNine.Install");
    SetSZ(hk, "Mfg", "OmniNine");
    RegCloseKey(hk);
    Log("registry: %s -> %s", node, drv);
    return TRUE;
}

/* 回读校验: 节点 Driver 必须指向 inst, 且实例 DEFAULT\drv 必须是 omninine.drv */
static BOOL VerifyBinding(const char *node, const char *inst)
{
    HKEY hk;
    char v[256];
    DWORD cb, type;
    BOOL ok = FALSE;

    if (!node || !node[0]) return FALSE;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, node, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        cb = sizeof(v); type = 0; v[0] = 0;
        if (RegQueryValueEx(hk, "Driver", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
            && strnicmp(v, "Display\\", 8) == 0 && lstrcmpi(v + 8, inst) == 0)
            ok = TRUE;
        RegCloseKey(hk);
    }
    if (ok && OpenClassInst(inst, &hk)) {
        v[0] = 0;
        ok = ReadKeyStr(hk, "DEFAULT", "drv", v, sizeof(v)) && lstrcmpi(v, "omninine.drv") == 0;
        RegCloseKey(hk);
    }
    return ok;
}

/* 两级扫描 Enum\Display, 返回第一个带 Driver 的节点; 否则返回第一个节点作为候选 */
static BOOL FindBoundNode(char *node, int cb, char *inst, int icb)
{
    HKEY root, dev;
    DWORD i, j;
    char best[MAX_PATH];

    node[0] = 0; best[0] = 0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Enum\\Display", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return FALSE;

    for (i = 0; ; i++) {
        char devname[128], devpath[MAX_PATH];
        DWORD len = sizeof(devname);
        if (RegEnumKeyEx(root, i, devname, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wsprintf(devpath, "Enum\\Display\\%s", devname);
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, devpath, 0, KEY_READ, &dev) != ERROR_SUCCESS) continue;

        /* 某些 9x 把值直接放在设备键下 */
        {
            char v[256]; DWORD vb = sizeof(v), type = 0;
            v[0] = 0;
            if (RegQueryValueEx(dev, "Driver", NULL, &type, (LPBYTE)v, &vb) == ERROR_SUCCESS
                && strnicmp(v, "Display\\", 8) == 0 && v[8]) {
                lstrcpyn(inst, v + 8, icb);
                lstrcpyn(node, devpath, cb);
                RegCloseKey(dev); RegCloseKey(root);
                return TRUE;
            }
            if (!best[0]) lstrcpyn(best, devpath, sizeof(best));
        }

        for (j = 0; ; j++) {
            char iname[128], inpath[MAX_PATH];
            DWORD l2 = sizeof(iname);
            HKEY ins;
            char v[256]; DWORD vb = sizeof(v), type = 0;

            if (RegEnumKeyEx(dev, j, iname, &l2, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wsprintf(inpath, "%s\\%s", devpath, iname);
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, inpath, 0, KEY_READ, &ins) != ERROR_SUCCESS) continue;
            v[0] = 0;
            if (RegQueryValueEx(ins, "Driver", NULL, &type, (LPBYTE)v, &vb) == ERROR_SUCCESS
                && strnicmp(v, "Display\\", 8) == 0 && v[8]) {
                lstrcpyn(inst, v + 8, icb);
                lstrcpyn(node, inpath, cb);
                RegCloseKey(ins); RegCloseKey(dev); RegCloseKey(root);
                return TRUE;
            }
            RegCloseKey(ins);
            if (!best[0]) lstrcpyn(best, inpath, sizeof(best));
        }
        RegCloseKey(dev);
    }
    RegCloseKey(root);

    if (best[0]) { lstrcpyn(node, best, cb); return TRUE; }
    return FALSE;
}


/* ---- r34: 递归绑定回退 ---- */


static BOOL KeyIsDisplay(HKEY hk, const char *path)
{
    char v[160];
    DWORD cb, type;

    cb = sizeof(v); v[0] = 0;
    if (RegQueryValueEx(hk, "Class", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
        && lstrcmpi(v, "Display") == 0) return TRUE;
    cb = sizeof(v); v[0] = 0;
    if (RegQueryValueEx(hk, "ClassGUID", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
        && lstrcmpi(v, DISP_GUID) == 0) return TRUE;
    cb = sizeof(v); v[0] = 0;
    if (RegQueryValueEx(hk, "DeviceDesc", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
        && (HasNoCase(v, "VGA") || HasNoCase(v, "Display") || HasNoCase(v, "OmniNine")))
        return TRUE;
    if (HasNoCase(path, "\\Display\\")) return TRUE;
    return FALSE;
}

static int BindDisplayNodesAt(const char *path, int depth, const char *inst)
{
    HKEY hk;
    DWORD i;
    int n = 0;

    if (depth > 4) return 0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, &hk) != ERROR_SUCCESS)
        return 0;

    if (KeyIsDisplay(hk, path)) {
        char drv[64];
        DWORD z = 0;
        wsprintf(drv, "Display\\%s", inst);
        RegSetValueEx(hk, "Driver", 0, REG_SZ, (const BYTE *)drv, (DWORD)strlen(drv) + 1);
        RegSetValueEx(hk, "ConfigFlags", 0, REG_DWORD, (const BYTE *)&z, sizeof(z));
        SetSZ(hk, "Class", "Display");
        SetSZ(hk, "ClassGUID", DISP_GUID);
        if (!g_bindPath[0]) lstrcpyn(g_bindPath, path, sizeof(g_bindPath));
        if (!g_modelNode[0]) {
            char hv[256]; DWORD hb = sizeof(hv), ht = 0;
            if (RegQueryValueEx(hk, "HardwareID", NULL, &ht, (LPBYTE)hv, &hb) == ERROR_SUCCESS)
                lstrcpyn(g_modelNode, path, sizeof(g_modelNode));
        }
        n++;
    }
    for (i = 0; ; i++) {
        char name[128], sub[512];
        DWORD len = sizeof(name);
        if (RegEnumKeyEx(hk, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wsprintf(sub, "%s\\%s", path, name);
        n += BindDisplayNodesAt(sub, depth + 1, inst);
    }
    RegCloseKey(hk);
    return n;
}


/* r42: 把所有显示类实例都指向本驱动, 避免 PnP 按另一个实例加载 VGA/厂商驱动 */
static void SetOneInstanceOurs(const char *inst)
{
    HKEY hk;
    char path[512];

    wsprintf(path, "System\\CurrentControlSet\\Services\\Class\\Display\\%s", inst);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
        SetSZ(hk, "Ver", "4.0");
        SetSZ(hk, "DevLoader", "*vdd");
        SetSZ(hk, "InfPath", "omninine.inf");
        SetSZ(hk, "InfSection", "OmniNine.Install");
        SetSZ(hk, "DeviceDesc", "Generic VBE (LFB) display adapter");
        SetSZ(hk, "DriverDesc", "Generic VBE (LFB) display adapter");
        SetSZ(hk, "Mfg", "OmniNine");
        SetSZ(hk, "ProviderName", "OmniNine");
        RegCloseKey(hk);
    }
    wsprintf(path, "System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT", inst);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL) == ERROR_SUCCESS) {
        SetSZ(hk, "Mode", "16,1024,768");
        SetSZ(hk, "drv", "omninine.drv");
        SetSZ(hk, "vdd", "*vdd,*vflatd");
        SetSZ(hk, "minivdd", "minivdd.vxd");
        SetDwordVal(hk, "RefreshRate", 0xFFFFFFFFUL);
        SetDwordVal(hk, "DDC", 1);
        SetDwordVal(hk, "ExtModeSwitch", 0);
        RegCloseKey(hk);
    }
}

static void WriteAllClassInstances(const char *pref)
{
    HKEY root;
    DWORD i;
    int n = 0;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Services\\Class\\Display",
                     0, KEY_READ, &root) == ERROR_SUCCESS) {
        for (i = 0; ; i++) {
            char name[128];
            DWORD len = sizeof(name);
            if (RegEnumKeyEx(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (strnicmp(name, "__", 2) == 0) continue;
            SetOneInstanceOurs(name);
            n++;
        }
        RegCloseKey(root);
    }
    SetOneInstanceOurs(pref);
    Log("class instances pointed to omninine.drv: %d (+preferred)", n);
}

static BOOL InstallViaInf(void)
{
    char inst[64], node[MAX_PATH], path[MAX_PATH], inf[8192], line[640];
    char cmd[1024];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD ec = 0;

    inst[0] = 0; node[0] = 0;
    if (!FindBoundNode(node, sizeof(node), inst, sizeof(inst)))
        Log("WARN: no display node found under Enum\\Display");
    if (!inst[0]) lstrcpyn(inst, g_instance[0] ? g_instance : "0000", sizeof(inst));
    lstrcpyn(g_nodePath, node, sizeof(g_nodePath));
    lstrcpyn(g_inst, inst, sizeof(g_inst));
    Log("install target: node=%s instance=%s", node[0] ? node : "(none)", inst);

    /* 先直接写类值与节点绑定: 不依赖 SetupX 是否真的执行/是否静默失败 */
    WriteClassValues(inst);
    WriteAllClassInstances(inst);
    if (node[0]) BindNode(node, inst);
    {
        int bn;
        g_bindPath[0] = 0;
        bn = BindDisplayNodesAt("Enum", 0, inst);
        Log("bind-all: %d display node(s)", bn);
        if (!node[0] && g_bindPath[0]) { lstrcpyn(node, g_bindPath, sizeof(node)); lstrcpyn(g_nodePath, node, sizeof(g_nodePath)); }
    }

    inf[0] = 0;
    strcat(inf, "[Version]\r\nSignature=\"$CHICAGO$\"\r\n\r\n"
                "[DefaultInstall]\r\nAddReg=OMNININE.Install.Reg\r\n\r\n"
                "[OMNININE.Install.Reg]\r\n");

#define REGLINE(...) do { sprintf(line, __VA_ARGS__); strcat(inf, line); } while (0)
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",Ver,0,\"4.0\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",DevLoader,0,\"*vdd\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",InfPath,0,\"omninine.inf\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",InfSection,0,\"OmniNine.Install\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",DeviceDesc,0,\"Generic VBE (LFB) display adapter\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",Mfg,0,\"OmniNine\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",DriverDesc,0,\"Generic VBE (LFB) display adapter\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\",ProviderName,0,\"OmniNine\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",Mode,0,\"16,1024,768\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",drv,0,\"omninine.drv\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",vdd,0,\"*vdd,*vflatd\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",minivdd,0,\"minivdd.vxd\"\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",RefreshRate,0x00010001,0xFFFFFFFF\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",DDC,0x00010001,1\r\n", inst);
    REGLINE("HKLM,\"System\\CurrentControlSet\\Services\\Class\\Display\\%s\\DEFAULT\",ExtModeSwitch,0x00010001,0\r\n", inst);
    if (node[0]) {
        REGLINE("HKLM,\"%s\",Driver,0,\"Display\\%s\"\r\n", node, inst);
        REGLINE("HKLM,\"%s\",ConfigFlags,0x00010001,0\r\n", node);
    }
#undef REGLINE

    JoinPath(path, g_winDir, "TEMP");
    CreateDirectory(path, NULL);
    {   char tmp[MAX_PATH];
        JoinPath(tmp, path, "OMNININ.INF");
        strcpy(path, tmp);
    }
    if (!SaveFile(path, inf, (DWORD)strlen(inf))) {
        Log("ERROR: cannot write install INF (direct registry writes were applied)");
    } else {
        wsprintf(cmd, "rundll32.exe setupx.dll,InstallHinfSection DefaultInstall 130 \"%s\"", path);
        memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            Log("WARN: cannot run SetupX (%lu); relying on direct registry writes", GetLastError());
        } else {
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &ec);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            Log("SetupX exit=%lu", ec);
        }
    }

    if (!VerifyBinding(node, inst)) {
        Log("WARN: binding verify failed; retrying direct writes");
        WriteClassValues(inst);
        if (node[0]) BindNode(node, inst);
        else {
            int bn;
            g_bindPath[0] = 0;
            bn = BindDisplayNodesAt("Enum", 0, inst);
            Log("recursive rebind: %d display node(s)", bn);
            if (g_bindPath[0]) lstrcpyn(node, g_bindPath, sizeof(node));
        }
    }
    if (!VerifyBinding(node, inst)) {
        Log("ERROR: automatic binding failed (node=%s inst=%s)", node[0] ? node : "(none)", inst);
        return FALSE;
    }
    Log("OK: binding verified (%s -> Display\\%s)", node, inst);
    return TRUE;
}

static BOOL IsDriverInstalled(void)
{
    char p[MAX_PATH], v[256];
    HKEY hk;

    /* 1) 驱动文件存在 才是"已安装"的硬证据 */
    JoinPath(p, g_sysDir, "omninine.drv");
    if (FileExists(p)) return TRUE;
    JoinPath(p, g_sysDir, "vbe9x.drv");          /* 旧名兼容 */
    if (FileExists(p)) return TRUE;
    JoinPath(p, g_sysDir, "minivdd.vxd");
    if (FileExists(p)) return TRUE;

    /* 2) 注册表: 必须指向 omninine.drv 或描述含 VBE9X。
     *    注意: 不能用 DevLoader=="*vdd" 判断 —— 标准 VGA 也用 *vdd,
     *    否则卸载后(我们把实例重置成 vga.drv/*vdd)会被误判为"仍安装"。 */
    if (OpenInstanceKey(&hk)) {
        if (ReadKeyStr(hk, "DEFAULT", "drv", v, sizeof(v)) && lstrcmpi(v, "omninine.drv") == 0) {
            RegCloseKey(hk); return TRUE;
        }
        {   /* 直接读实例自身的值(不能通过子键路径) */
            DWORD type2 = 0, cb2;
            v[0] = 0; cb2 = sizeof(v);
            if (RegQueryValueEx(hk, "DriverDesc", NULL, &type2, (LPBYTE)v, &cb2) == ERROR_SUCCESS
                && HasNoCase(v, "OmniNine")) { RegCloseKey(hk); return TRUE; }
            v[0] = 0; cb2 = sizeof(v);
            if (RegQueryValueEx(hk, "ProviderName", NULL, &type2, (LPBYTE)v, &cb2) == ERROR_SUCCESS
                && HasNoCase(v, "OmniNine")) { RegCloseKey(hk); return TRUE; }
        }
        RegCloseKey(hk);
    }
    return FALSE;
}

/* ---------------- 安装 / 卸载 ---------------- */

static void DoInstall(void)
{
    int build  = (SendMessage(GetDlgItem(g_hWnd, IDC_DBG), BM_GETCHECK, 0, 0) == BST_CHECKED);
    int devHigh= (SendMessage(GetDlgItem(g_hWnd, IDC_DEVHIGH), BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL fixsys= (SendMessage(GetDlgItem(g_hWnd, IDC_FIXSYS), BM_GETCHECK, 0, 0) == BST_CHECKED);
    int  winme = IsWinME();   /* [r29] */
    BOOL bindok;
    const char *sub = build ? "dbg" : "rel";
    char ini[MAX_PATH];

    if (winme) {
        fixsys = FALSE;      /* [r29] WinME: CONFIG.SYS 不加载真实模式驱动 */
        Log("Windows ME detected: real-mode drivers are NOT supported (KB 269524)");
        Log("  -> FIXSYS.SYS skipped; existing FIXSYS line will be removed from CONFIG.SYS");
        MessageBox(g_hWnd,
            LangGet("msg.me",
                "Windows ME does not support real-mode drivers.\n\n"
                "CONFIG.SYS is no longer used to load DEVICE= drivers,\n"
                "so FIXSYS.SYS cannot be installed on this system.\n\n"
                "Setup will continue and install the display driver only."),
            APPNAME, MB_OK | MB_ICONINFORMATION);
    }

    Status(LangGet("ui.status.installing", "Installing..."));
    Log("=== Install (%s driver, FIXSYS=%s, %s) ===",
        build ? "debug" : "release", fixsys ? "yes" : "no",
        devHigh ? "DEVICEHIGH" : LangGet("ui.dev", "DEVICE"));

    if (!IsWin9x()) Log("WARNING: designed for Windows 95/98/ME");
    if (!EnsureInstance()) return;

    if (IsDriverInstalled()) {
        Log("detected existing OmniNine installation");
        if (MessageBox(g_hWnd,
                LangGet("msg.existing",
                "An existing OmniNine installation was detected.\n\n"
                "Setup cannot replace a loaded driver.\n"
                "Uninstall the old driver first?\n\n"
                "(Reboot after uninstall, then run Setup again.)"),
                APPNAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            DoUninstall();
            MessageBox(g_hWnd,
                LangGet("msg.oldremoved", "Old driver removed.\n\nReboot, then run Setup again."),
                APPNAME, MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    Log("display instance: %s", g_instance);

    BackupOld();

    /* 布局: rel/ 或 dbg/ 只放 omninine.drv; 其余公用文件在 common/ */
    if (!CopyWithReplace(sub,      "omninine.drv",   g_sysDir)) return;
    if (!CopyWithReplace("common", "minivdd.vxd", g_sysDir)) return;
    if (fixsys && !CopyWithReplace("common", "fixsys.sys", g_winDir)) return;
    if (fixsys) {
        char legacy[MAX_PATH];
        JoinPath(legacy, g_sysDir, "fixsys.sys");     /* 旧版 INF 放在系统目录 */
        if (lstrcmpi(g_sysDir, g_winDir) != 0) DeleteOrDefer(legacy);
    }

    if (fixsys) {
        char f1[MAX_PATH], f2[MAX_PATH], f3[MAX_PATH];
        BOOL b1, b2, b3;

        JoinPath(f1, g_winDir, "fixsys.sys");
        JoinPath(f2, g_winDir, "FIXSYS.NEW");
        JoinPath(f3, g_winDir, "FIXSYS.TMP");
        b1 = FileExists(f1);
        b2 = FileExists(f2);
        b3 = FileExists(f3);
        Log("fixsys readiness: %s=%d | %s=%d | %s=%d", f1, b1, f2, b2, f3, b3);
        if (!b1 && !b2 && !b3) {
            Log("ERROR: fixsys target not ready (none of the three paths exist); CONFIG.SYS untouched");
            return;
        }
        if (!b1)
            Log("NOTE: fixsys will be put in place at next boot (staged .NEW/.TMP)");
    }

    {
        char cfg[MAX_PATH];
        ConfigSysPath(cfg);
        Log("target CONFIG.SYS: %s", cfg);
        DumpFixsysLines(cfg);
    }

    if (!RewriteConfigSys(fixsys, devHigh)) return;

    JoinPath(ini, g_winDir, "SYSTEM.INI");
    WritePrivateProfileString("boot", "display.drv", "pnpdrvr.drv", ini);
    Log("SYSTEM.INI: [boot] display.drv=pnpdrvr.drv");

    bindok = InstallViaInf();
    InstallInf();                     /* 绑定后生成"首选" INF, 并复制驱动文件到 INF 旁 */

    if (!bindok) {
        Log("ERROR: automatic driver binding failed");
        Log("       files and INF are in place; finish manually:");
        Log("       Display Properties -> Settings -> Advanced -> Adapter -> Change -> Have Disk -> omninine.inf");
        MessageBox(g_hWnd,
            LangGet("msg.bindfail",
                "The driver files are installed, but the display device could not be\n"
                "bound to the new driver automatically.\n\n"
                "Please finish manually:\n"
                "Display Properties -> Settings -> Advanced -> Adapter -> Change\n"
                "-> Have Disk -> select omninine.inf.\n\n"
                "Then reboot."),
            APPNAME, MB_OK | MB_ICONWARNING);
        return;
    }

    Log("=== Install completed. ===");
    if (g_deferred)
        Log("NOTE: in-use files will be replaced at restart (WININIT.INI).");
    Status(LangGet(g_deferred ? "ui.status.installed_deferred" : "ui.status.installed_reboot",
                      g_deferred ? "Installed - restart to replace in-use files" : "Installed - reboot required"));
    if (MessageBox(g_hWnd, g_deferred
                   ? LangGet("msg.install_deferred", "Install completed.\n\nSome driver files are in use and will be\nreplaced automatically at restart.\n\nRestart now?")
                   : LangGet("msg.install_reboot", "Install completed.\n\nReboot now?"), APPNAME,
                   MB_YESNO | MB_ICONQUESTION) == IDYES)
        ExitWindowsEx(EWX_REBOOT, 0);
}

static void DoUninstall(void)
{
    char path[MAX_PATH], cfg[MAX_PATH];

    Status(LangGet("ui.status.uninstalling", "Uninstalling..."));
    Log("=== Uninstall ===");

    if (!RestoreInstanceTree()) {
        /* 直接按"值里带我们标记"递归删除 Class 与 Enum 两侧, 不依赖 INF/猜测 */
        g_delKeys = 0;
        /* Class 侧: 逐个实例整棵判断(标记可能只在 DEFAULT 子键里) */
        {
            HKEY root;
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, DISP_KEY, 0, KEY_READ, &root) == ERROR_SUCCESS) {
                char names[64][64];
                int n = 0, k;
                DWORD i;
                for (i = 0; i < 64; i++) {
                    DWORD len = sizeof(names[i]);
                    if (RegEnumKeyEx(root, i, names[i], &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
                    n++;
                }
                RegCloseKey(root);
                for (k = 0; k < n; k++) {
                    char sub[MAX_PATH];
                    wsprintf(sub, "%s\\%s", DISP_KEY, names[k]);
                    if (TreeHasMark(sub, 0)) {
                        RegDeleteTree(sub);
                        g_delKeys++;
                        Log("uninstall: removed instance %s (whole subtree)", sub);
                    }
                }
            }
        }
        /* Enum 侧: 全树扫描(不只是 Enum\Display, 也可能是 Enum\Root/PCI) */
        ScanDeleteTree("Enum", 0);
        Log("uninstall: removed %d registry key(s) by marker scan", g_delKeys);

        /* 保证卸载后仍有一个可用的标准 VGA 实例 */
        EnsureInstance();
        CleanOurKeys();
    }

    CleanWinInit();

    /* 只在 CONFIG.SYS 存在时改写(避免"卸载时凭空创建空 CONFIG.SYS") */
    {
        char root[MAX_PATH];
        strcpy(root, g_winDir);
        if (root[1] == ':') root[3] = 0; else strcpy(root, "C:\\");
        JoinPath(cfg, root, "CONFIG.SYS");
    }
    if (FileExists(cfg)) RewriteConfigSys(FALSE, 0);
    else Log("CONFIG.SYS not present - nothing to undo");

    JoinPath(path, g_sysDir, "omninine.drv");      DeleteOrDefer(path);
    JoinPath(path, g_sysDir, "minivdd.vxd");    DeleteOrDefer(path);
    JoinPath(path, g_winDir, "FIXSYS.SYS");     DeleteOrDefer(path);
    JoinPath(path, g_sysDir, "fixsys.sys");     DeleteOrDefer(path);
    JoinPath(path, g_winDir, "INF\\OMNININE.INF"); DeleteOrDefer(path);

    JoinPath(path, g_sysDir, "OMNININE.NEW");   DeleteOrDefer(path);
    JoinPath(path, g_sysDir, "MINIVDD.NEW"); DeleteOrDefer(path);
    JoinPath(path, g_sysDir, "FIXSYS.NEW");  DeleteOrDefer(path);

    Log("=== Uninstall completed. Reboot required. ===");
    Status(LangGet("ui.status.uninstalled_reboot", "Uninstalled - reboot required"));
    if (MessageBox(g_hWnd, LangGet("msg.uninst_reboot", "Uninstall completed.\n\nReboot now?"), APPNAME,
                   MB_YESNO | MB_ICONQUESTION) == IDYES)
        ExitWindowsEx(EWX_REBOOT, 0);
}

/* ---------------- 界面 ---------------- */


/* ---------------- 外挂语言文件 ---------------- */
static char *g_langBuf = NULL;

/* 取字符串: key 不存在则返回内置英文 def。
 * 语言文件格式: 每行 "key=value"; 值里的 \n 会被转换成真正的换行。 */
static char *LangGet(const char *key, const char *def)
{
    static char out[1024];
    char *p;

    if (!g_langBuf) return (char *)def;
    p = g_langBuf;
    while (p && *p)
    {
        char *e = strchr(p, '=');
        char *nl = strchr(p, '\n');
        if (e && (!nl || e < nl))
        {
            char *k = p;
            size_t klen = (size_t)(e - p);
            while (klen && (k[klen-1] == ' ' || k[klen-1] == '\t' || k[klen-1] == '\r')) klen--;
            if (strlen(key) == klen && strnicmp(k, key, klen) == 0)
            {
                char *v = e + 1;
                int j = 0;
                while (*v == ' ' || *v == '\t') v++;
                while (*v && v != nl && j < (int)sizeof(out) - 1)
                {
                    if (v[0] == '\\' && v[1] == 'n') { out[j++] = '\n'; v += 2; }
                    else if (v[0] == '\r') { v++; }
                    else out[j++] = *v++;
                }
                out[j] = 0;
                return out;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return (char *)def;
}

static void LoadLang(void)
{
    char path[MAX_PATH], tmp[MAX_PATH], code[32], env[32];
    LANGID id;
    int n;

    code[0] = 0;
    if (GetEnvironmentVariable("SETUP_LANG", env, sizeof(env)) > 0)
        lstrcpyn(code, env, sizeof(code));
    else
    {
        id = GetSystemDefaultLangID();
        if ((id & 0x3FF) == 0x04)                        /* LANG_CHINESE */
        {
            int sub = (id >> 10) & 0x3F;
            if (sub == 0x02) lstrcpyn(code, "zh-CN", sizeof(code));   /* 简体 */
            else if (sub == 0x01) lstrcpyn(code, "zh-TW", sizeof(code));
        }
    }
    if (!code[0]) return;                                 /* 用内置英文 */

    n = 0;
    wsprintf(tmp, "setup.%s.lng", code);
    JoinPath(path, g_appDir, tmp);
    if (FileExists(path)) n = 1;
    if (!n) { char sub[MAX_PATH]; JoinPath(sub, g_appDir, "common"); JoinPath(path, sub, tmp); if (FileExists(path)) n = 1; }
    if (!n) { char sub[MAX_PATH]; JoinPath(sub, g_appDir, "lang");   JoinPath(path, sub, tmp); if (FileExists(path)) n = 1; }
    if (!n) { wsprintf(tmp, "%s.lng", code); JoinPath(path, g_appDir, tmp); if (FileExists(path)) n = 1; }
    if (!n) { char sub[MAX_PATH]; JoinPath(sub, g_appDir, "common"); wsprintf(tmp, "%s.lng", code); JoinPath(path, sub, tmp); if (FileExists(path)) n = 1; }
    if (n) {
        DWORD sz = 0;
        if (LoadFile(path, &g_langBuf, &sz)) { /* ok: g_langBuf 指向文件内容 */ }
        else Log("WARN: cannot read language file %s", path);
    }
}

static HWND Mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    DWORD extra = 0;

    /* [r30] Tab 只在按钮之间移动; 日志框(EDIT)会吞掉 Tab, 故不参与导航 */
    if (lstrcmpi(cls, "BUTTON") == 0 && (style & 0x0F) != BS_GROUPBOX) extra |= WS_TABSTOP;

    return CreateWindow(cls, text, WS_CHILD | WS_VISIBLE | extra | style,
                        x, y, w, h, g_hWnd, (HMENU)id, g_hInst, NULL);
}

static void CreateControls(void)
{
    Mk("BUTTON", LangGet("ui.build", "Driver build:"), BS_GROUPBOX, 10, 8, 240, 74, 0);
    Mk("BUTTON", LangGet("ui.rel", "Release (recommended)"), BS_AUTORADIOBUTTON | WS_GROUP, 20, 28, 220, 18, IDC_REL);
    Mk("BUTTON", LangGet("ui.dbg", "Debug (slow, verbose log)"), BS_AUTORADIOBUTTON, 20, 50, 220, 18, IDC_DBG);
    SendMessage(GetDlgItem(g_hWnd, IDC_REL), BM_SETCHECK, BST_CHECKED, 0);

    Mk("BUTTON", LangGet("ui.fixgrp", "FIXSYS.SYS"), BS_GROUPBOX, 260, 8, 250, 96, 0);
    Mk("BUTTON", LangGet("ui.fixchk", "Install FIXSYS.SYS"), BS_AUTOCHECKBOX, 270, 26, 230, 18, IDC_FIXSYS);
    SendMessage(GetDlgItem(g_hWnd, IDC_FIXSYS), BM_SETCHECK, BST_CHECKED, 0);
    Mk("BUTTON", LangGet("ui.dev", "DEVICE"), BS_AUTORADIOBUTTON | WS_GROUP, 282, 48, 210, 18, IDC_DEVLOW);
    Mk("BUTTON", LangGet("ui.devhigh", "DEVICEHIGH (saves memory)"), BS_AUTORADIOBUTTON, 282, 68, 220, 18, IDC_DEVHIGH);
    SendMessage(GetDlgItem(g_hWnd, IDC_DEVHIGH), BM_SETCHECK, BST_CHECKED, 0);

    Mk("STATIC", LangGet("ui.layout", "Layout: rel\\omninine.drv (release) | dbg\\omninine.drv (debug) | common\\minivdd.vxd, fixsys.sys, omninine.inf"),
       SS_LEFT, 10, 112, 500, 16, 0);

    Mk("EDIT", "", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
       10, 134, 500, 190, IDC_LOG);
    Mk("STATIC", LangGet("ui.ready", "Ready."), SS_LEFT, 10, 330, 500, 16, IDC_STATUS);

    Mk("BUTTON", LangGet("ui.install", "Install"), BS_PUSHBUTTON, 250, 356, 80, 26, IDC_INSTALL);
    Mk("BUTTON", LangGet("ui.uninstall", "Uninstall"), BS_PUSHBUTTON, 340, 356, 80, 26, IDC_UNINSTALL);
    Mk("BUTTON", LangGet("ui.exit", "Exit"), BS_PUSHBUTTON, 430, 356, 80, 26, IDC_EXIT);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hWnd = hWnd;
        CreateControls();
    if (IsWinME()) {
        EnableWindow(GetDlgItem(hWnd, IDC_FIXSYS), FALSE);
        EnableWindow(GetDlgItem(hWnd, IDC_DEVLOW), FALSE);
        EnableWindow(GetDlgItem(hWnd, IDC_DEVHIGH), FALSE);
        SetWindowText(GetDlgItem(hWnd, IDC_FIXSYS),
                      LangGet("ui.fixchk_me", "Install FIXSYS.SYS (not supported on Windows ME)"));
        Log("Windows ME: FIXSYS.SYS option disabled (real-mode drivers removed)");
    }
        SetFocus(GetDlgItem(hWnd, IDC_INSTALL));
        g_hLog = GetDlgItem(hWnd, IDC_LOG);
        g_hStatus = GetDlgItem(hWnd, IDC_STATUS);
        Log("%s %s (%s)", APPNAME, VERSION_STR, INTERNAL_VER);
        Log("Windows : %s", g_winDir);
        Log("System  : %s", g_sysDir);
        Log("Program : %s", g_appDir);
        return 0;

    case WM_COMMAND:
        if (g_busy) return 0;
        switch (LOWORD(wp)) {
        case IDC_INSTALL:
            g_busy = TRUE; EnableWindow(GetDlgItem(hWnd, IDC_INSTALL), FALSE);
            DoInstall();
            EnableWindow(GetDlgItem(hWnd, IDC_INSTALL), TRUE); g_busy = FALSE;
            return 0;
        case IDC_UNINSTALL:
            if (MessageBox(hWnd, LangGet("msg.uninst_confirm",
                           "Uninstall OmniNine and restore the previous display driver?"),
                           APPNAME, MB_YESNO | MB_ICONWARNING) == IDYES) {
                g_busy = TRUE; EnableWindow(GetDlgItem(hWnd, IDC_UNINSTALL), FALSE);
                DoUninstall();
                EnableWindow(GetDlgItem(hWnd, IDC_UNINSTALL), TRUE); g_busy = FALSE;
            }
            return 0;
        case IDC_EXIT:
            DestroyWindow(hWnd);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    WNDCLASS wc;
    MSG msg;
    HWND hWnd;

    (void)hPrev; (void)cmd;
    g_hInst = hInst;
    GetDirs();
    LoadLang();
    if (IsWinNT()) {
        MessageBox(NULL,
            LangGet("msg.nt", "This installer supports Windows 95/98/ME only.\nWindows NT/2000/XP and later are not supported."),
            APPNAME, MB_OK | MB_ICONERROR);
        return 0;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon   = LoadIcon(hInst, "MAINICON");   /* [r31] 资源图标 */
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "OMNININESETUP";
    if (!RegisterClass(&wc)) return 1;

    hWnd = CreateWindowEx(WS_EX_CONTROLPARENT, "OMNININESETUP", LangGet("ui.title", APPNAME " " VERSION_STR " - Windows 95/98/ME"),
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 530, 420,
                          NULL, NULL, hInst, NULL);
    if (!hWnd) return 1;

    ShowWindow(hWnd, show);
    UpdateWindow(hWnd);
    while (GetMessage(&msg, NULL, 0, 0)) {
        /* Tab/Shift+Tab/方向键在控件间移动; Enter 触发当前按钮 */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND f = GetFocus();
            char cn[32];
            cn[0] = 0;
            if (f) GetClassName(f, cn, sizeof(cn));
            if (lstrcmpi(cn, "BUTTON") == 0) { SendMessage(f, BM_CLICK, 0, 0); continue; }
        }
        if (!IsDialogMessage(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
