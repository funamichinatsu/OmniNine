/*=============================================================================
 * display.c —— 16 位显示驱动的 DDI 入口与 GDI 胶合层
 *
 * 【设计约束】
 *  1. 帧缓冲通过 DPMI 自建选择子访问：选择子必须 32 位(D/B=1)、界限覆盖整个 LFB，
 *     并且每次改动描述符后都要回读确认 —— 只写不读的"设置成功"不可信。
 *  2. PDEV 优先交给 DIBENG 自己创建；创建失败时才手填，而手填必须补齐
 *     deBeginAccess/deEndAccess（指向引擎自己的 0/1 号取值例程），
 *     否则屏幕的存取不会与引擎内部状态同步，表现为光标/菜单周围留下旧内容。
 *  3. 模式设置之后必须核对硬件真的进入了该模式（4F03h），不能只看 VBE 调用返回值。
 *
 * 【大体思路】
 *  ValidateMode/SetMode 走 VBE(4F01/4F02) → 建/复用选择子 → 填 PDEV → 交给 DIBENG；
 *  光标、调色板、文本输出由本层直接实现，其余绘制交给 DIBENG。
 *===========================================================================*/
/*============================================================================
 *
 *  DISPLAY.C — Win9x 通用 VBE 显示驱动的 16 位 GDI 侧骨架(display.drv)
 *
 *  【在整体架构中的位置】
 *      GDI.EXE ──► display.drv(本文件+drv.c+vddapi16.asm)
 *                        │  DDI 导出(Enable/BitBlt/...)大多转发给
 *                        ▼  DIB Engine(DIBENG.DLL, 软件渲染)
 *                     帧缓冲选择子(16:16)──► LFB 线性地址
 *                        ▲                    ▲
 *                        │ DPMI 建选择子      │ 恒等映射页对齐(./38 起,
 *      ring0:   VDD.VXD(主) ──► mini-VDD(minivdd.asm)  _MapPhysToLinear
 *                                           在 PM API 上下文实机死机, 弃用
 *
 *  【"第 ③ 环注册"完整时序】(对齐 vmdisp9x 的已验证实现)
 *    加载期(DriverInit→DrvInitCore):
 *      1. INT 2F AX=1684h/BX=000Ah     —— 取主 VDD 的 PM API 入口
 *         INT 2F AX=1683h             —— 取本 VM 句柄(OurVMHandle)
 *      2. VDD_REGISTER_DISPLAY_DRIVER_INFO(0x83) —— 告知"驱动已加载"
 *      3. VDD_GET_DISPLAY_CONFIG(0x85) —— 读注册表中的分辨率/位深
 *    使能期(Enable→PhysicalEnable):
 *      4. VDD_PRE_MODE_CHANGE(0x86)    —— 模式切换前通知
 *      5. VBE_SetMode(0x4000|mode)     —— bit14 请求线性帧缓冲
 *      6. VBE_GetModeInfo(4F01) + 推送共享状态给 mini-VDD
 *                                      —— 主 VDD 的 GET_TOTAL_VRAM_SIZE/
 *                                         GET_BANK_SIZE 靠它回答
 *      7. 【r76】DPMI 0800h 映射 LFB 物理地址 + DPMI LDT 选择子
 *         (LfbSelector_Ensure, 全在 ring 3 完成 —— 见文件头"帧缓冲")
 *      8. VDD_DRIVER_REGISTER(0x80)    —— 注册 RestoreDesktopMode 回调,
 *                                         ECX=pitch*height 供 VDD 核算
 *      9. 再打一次 VBE_SetMode         —— 保险(部分 BIOS 在 0x80 期间被打扰)
 *     10. VDD_POST_MODE_CHANGE(0x87)   —— 模式切换完成
 *     11. VDD_SAVE_DRIVER_STATE(0x82)  —— 保存"桌面模式"状态, DOS VM
 *                                         切回时 VDD 据此回调步骤 8 的函数
 *                                          (回调 = RestoreDesktopMode)
 *    注销期(Disable):
 *     12. INT 2F AX=4007h(START_IO_TRAP) —— 恢复 I/O 陷阱
 *     13. VDD_DRIVER_UNREGISTER(0x81)
 *     14. INT 10h AX=3                 —— 回 80x25 文本模式
 *
 *  【帧缓冲(r76 的核心修正)】
 *  帧缓冲选择子由本驱动自己用 DPMI 0800h(物理映射)+ DPMI
 *  0000h/0007h/0008h(LDT 选择子)建立, 物理地址来自 VBE 4F01h 的
 *  PhysBasePtr, 见 drv.c 的 LfbSelector_Ensure()。
 *  旧版让 ring 0 的 mini-VDD 建 GDT 选择子交下来, 实机第一次写帧缓冲
 *  就 #GP(蓝屏 0D @ 031F:00090B = _fmemset_ 的 "mov es,dx")。
 *  原因(r76 定案): ① 那个描述符的 DPL 由 VMM 按"保护模式应用的 RPL"
 *  填, 16 位显示驱动(ring 3)载入时 max(CPL,RPL) > DPL → #GP;
 *  ② 其基址是 VxD 系统区线性, 是 supervisor 页, 就算载入也是 #PF。
 *  一句话: ring 0 造不出 ring 3 能用的帧缓冲指针。
 *
 *  【渲染策略】纯 DIB Engine 软件渲染: 所有 DDI 绘图调用经 vddapi16.asm
 *  中的转发器交给 DIBENG.DLL, 它直接写线性帧缓冲。无硬件加速 → 代码量
 *  最小、行为最可预测, 这正是"通用 VBE 驱动"追求稳定性的代价与收益。
 *
 *  【编译警告】本文件必须【不】用 /Zp1(即保持编译器默认对齐): GDIINFO、
 *  DIBENGINE 等结构是 GDI/DIBENG 按自然对齐读写的。drv.c 才需要 /Zp1
 *  (VBE BIOS 的紧凑结构), 两个文件编译选项不同, 切勿统一。
 *
 *  【内存模型】中模式(medium): 代码远、数据近。所有跨段调用(如 _TEXT
 *  段的 RestoreDesktopMode 调 _INIT 段的 SetDisplayMode)因此天然合法;
 *  单数据段 DGROUP 与 vddapi16.asm 中 "mov ax, DGROUP" 的假设一致。
 *
 *==========================================================================*/

/* --- windows.h 与 gdidefs.h 的冲突规避(DDK 打印机驱动的经典手法) ------- */
#define NOGDI                   /* 屏蔽 wingdi.h(它与 gdidefs.h 重名冲突) */
#define PPOINT  n_PPOINT        /* 把冲突的类型名暂时改名, 包含后恢复     */
#define LPPOINT n_LPPOINT
#define RECT    n_RECT
#define PRECT   n_PRECT
#define LPRECT  n_LPRECT
#include <windows.h>

/* [r447] 构建标签: 改版本时只改这一处, 日志首行与各处引用保持一致 */
#ifndef VBE9X_TAG
#define VBE9X_TAG "r448"

extern void __far __pascal DiagSetLogBase( unsigned short off );
extern void __far __pascal DiagFlushBoot( void );   /* [r447] 开机阶段落盘 */
BYTE g_logBuf[8192];   /* [r447] 静态日志缓冲(进 DGROUP, 8KB) */   /* [r447] 进度标记 */
#endif
#undef  PPOINT
#undef  LPPOINT
#undef  RECT
#undef  PRECT
#undef  LPRECT

#include <gdidefs.h>            /* GDIINFO、PDEVICE、BITMAPINFO 等(DDK)   */
#include <dibeng.h>             /* DIBENGINE、DIB_* 引擎入口(DDK)         */
#include <minivdd.h>            /* VDD_* 功能号、DISPLAYINFO(DDK)         */
#include <string.h>             /* _fmemset / _fmemcpy                    */
/* MK_FP 宏的位置两家编译器不同: MSVC 在 windows.h, Watcom 在 dos.h。
 * 不包 dos.h 时 Watcom 会把 MK_FP 当成隐式函数(W131/W308), 链接出
 * undefined symbol MK_FP_。两家的 dos.h 都定义该宏, 包含它是通解。 */
#include <dos.h>                /* MK_FP(Watcom 侧)                       */

#include "vbeshare.h"           /* ring3↔ring0 共享契约                   */
#include "drv.h"                /* drv.c / vddapi16.asm 的内部接口        */

/* --- 可选诊断日志 ---------------------------------------------------------
 * 【为何放在文件最前】ReadDisplayConfig/LazyGPPI 等 _INIT 函数在文件中
 * 序很靠前, 若本宏定义晚于它们, 那些地方的 DIAG(...) 会被当成隐式
 * 函数声明, 链接出 undefined symbol DIAG_(Watcom 16 位 C 符号默认加
 * 后缀下划线) —— r14 构建实锤。
 * 【用途】真机测试定位加载期/启用期失败点。非诊断构建(DIAGFLAG 为空)
 * 完全不包含此代码(零开销、零行为变化); 诊断构建(默认):
 *   nmake omninine.drv
 * 真正的写文件在 vddapi16.asm 的 DiagWriteLog —— 纯汇编 INT 21h,
 * 与 DriverInit 探针逐字节同机制(该机制真机已证可用)。
 * 【为何弃用 clibm int86】引导期实测(./5 构建): 探针的裸 INT 21h 四次
 * 调用全部成功, 而 C 侧经 int86 的同一组文件调用全部静默失败(日志始终
 * 只有 PROBE 一行)——int86 的 segread_/int86x_/_DoINTR_ 链路在此阶段
 * 有未查明的问题。日志通道是排障命根子, 改走已验证的汇编路径,
 * 同时把 int86 从引导关键路径上彻底摘出去。                              */
/* 【r100】日志分级变量定义在 drv.c; 这里两份构建(诊断/发布)都要能引用。 */
extern uint16_t g_logLevel;
/* 【r110】LogBoot 三件套(见 drv.c 里 g_logBoot 的长注):
 *   g_logBoot   = 开机相级别(0 = 跟随 Log)
 *   g_runLog    = 会话期级别(ReadRunProfile 里与 Log 同步)
 *   g_bootLogOn = 1 = 当前处于"开机相覆盖"中(由 VxdLogBootEnd 清)
 * 发布构建里它们同样存在 —— 汇编/C 两侧共读同一份存储, 不能按构建裁掉。*/
extern uint16_t g_logBoot;
extern uint16_t g_runLog;
extern uint16_t g_bootLogOn;
/* 【r110】g_runPortGrp —— 端口读分组位图, 定义在 drv.c(与 PortProbeRead
 * 同处, 发布构建里也在 —— 它只是个被读的字节, 不产生任何端口 I/O)。*/
extern uint16_t g_runPortGrp;
extern void __far __pascal VxdLogBootEnd( void );
/* 【r110】VxdTakeBiosRefused: 取走(并清)一次"BIOS 调用被回调护栏拒绝"。
 * 见 drv.c 里 s_biosRefused 的长注 —— 它是 ReArm 的判据。            */
extern uint16_t __far __pascal VxdTakeBiosRefused( void );

/* --- r102: 日志通道"被丢弃的行"计数与回调闸门 -----------------------------
 * 三个计数与闸门哨兵【定义在 drv.c】(与 g_logLevel 同处), 因为 VxdTraceDump
 * 也在那边读它们; 这里只做 extern 声明。
 * 为什么不是 #ifdef 里的变量: vddapi16.asm 的 DIAGWRITELOG 是无条件编译的,
 * 它用 `inc word ptr [_DiagSkipXxx]` 直接写这三个字。发布构建里它们同样
 * 存在(只是没人读), 换来的是"两条构建的日志路径逐指令一致"。          */
extern uint16_t DiagSkipInDOS;  /* DOS 正忙(InDOS)而丢弃的行数            */
extern uint16_t DiagSkipReent;  /* 日志例程被重入而丢弃的行数              */
extern uint16_t DiagSkipCb;     /* 主 VDD 回调期(禁 I/O)丢弃的行数         */
/* 【r102】主 VDD 回调窗口哨兵 —— 汇编侧 DIAGWRITELOG 的第一道闸门。
 * 必须是非 static 的 C 全局: 汇编侧写的是 extrn _g_logBlocked:word。
 * 置 1 的窗口内绝不发任何 INT 21h —— 那段代码运行在 VDD 自己的切屏事务
 * 内部, 在那里做 DOS 文件 I/O 等于在 DOS 临界区里重入 DOS。            */
extern uint16_t g_logBlocked;


/* --- r118: "MIN 通道" —— 第三构建(DIAG_MIN) ---------------------------------
 * 【为什么要有第三条构建】./116 D1 那一组(release 构建)给出的症状是"窗口
 * command 花屏、exit 也不恢复", 而整轮【零读数】—— 发布构建里所有日志都是
 * 编译期空宏(DIAG(s) ((void)0) 等), 连实参都不求值。偏偏本轮要回答的问题
 * 就是"debug 与 release 的行为差异从哪来"。于是:
 *     nmake DIAGFLAG=-DVBE9X_DIAG_MIN omninine.drv
 * 得到的构建 = release 【逐指令相同】的高频路径 + 少量"决定性行":
 *     状态边沿 / DISPI 探针与修复 / 模式恢复结果 / 部署自证。
 * 高频路径之所以逐指令相同: MIN 构建里 DIAG/DIAGM/DiagHex/DiagCfg 仍然是
 * ((void)0)(连实参都不求值), 只有 DIAGMIN/DiagHexMin 是真函数。
 * 【与 Log 的关系】MIN 通道走 >=1 档 ⇒ system.ini 的 Log=0 能把它一起静音,
 * 于是【同一个二进制自带"零日志对照臂"】(debug 构建则用 Log=0 做同样的事)。
 * 【为何另开一份静态缓冲】与 drv.c 的写法一致: 两边各持一份, 避免把
 * DiagHex 的缓冲卷进重入路径。
 * 【MIN 构建里【不】补开机相的行】./109 t6 与 ./110 已两次实测"开机相多写
 * 几十行磁盘日志就能改变开机结果"(Log=1 那几次回落 VGA)。MIN 的目的正是
 * 让被测对象尽量等于 release, 所以开机相只留 build tag 一行。            */
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
extern void __far __pascal DiagWriteLog( const char __near *msg );
#define DIAGMIN( s ) do { if (g_logLevel >= 1) DiagWriteLog( s ); } while (0)
static void DiagHexMin( const char *msg, uint16_t v )
{
    static char       bufMin[48];
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = bufMin;

    if (g_logLevel < 1)
        return;
    while (*msg)
        *p++ = *msg++;
    *p++ = '=';
    *p++ = hexd[(v >> 12) & 15];
    *p++ = hexd[(v >> 8) & 15];
    *p++ = hexd[(v >> 4) & 15];
    *p++ = hexd[v & 15];
    *p   = 0;
    DiagWriteLog( bufMin );
}
/* 【r118】DiagCfgMin —— CFG 行(配置自证)的 MIN 通道版本。只有两行用它
 * (CFG7 Dispi / CFG8 DispiFix): 它们决定"这一轮跑的到底是哪套策略"。
 * 其余 CFG 行刻意【不】搬进来: MIN 构建的开机相必须尽量等于 release
 * (./109 t6 实测"开机相多写几十行就能改变开机结果")。              */
static void DiagCfgMin( const char *tag, uint16_t a, uint16_t b,
                        uint16_t c, uint16_t d, uint16_t e )
{
    static char       bufMin[96];
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = bufMin;
    uint16_t          v[5];
    int               i, k;

    if (g_logLevel < 1)
        return;
    while (*tag)
        *p++ = *tag++;
    v[0] = a; v[1] = b; v[2] = c; v[3] = d; v[4] = e;
    for (i = 0; i < 5; i++)
    {
        *p++ = ' ';
        for (k = 12; k >= 0; k -= 4)
            *p++ = hexd[(v[i] >> k) & 15];
    }
    *p = 0;
    DiagWriteLog( bufMin );
}
#else
#define DIAGMIN( s )        ((void)0)
#define DiagHexMin( m, v )  ((void)0)
#define DiagCfgMin( t, a, b, c, d, e ) ((void)0)
#endif

#ifdef VBE9X_DIAG
extern void __far __pascal DiagWriteLog( const char __near *msg );
extern void __far __pascal DiagFlush( void );

/* 【r100 日志分级】g_logLevel 定义在 drv.c(两边共用同一个变量):
 *   0 = 全静默(性能基线 —— 真机登录慢的第一个原因就是逐行日志)
 *   1 = 只打里程碑(DIAGM)
 *   2 = 默认(现状减去高频噪声)
 *   3 = 全量(含每次 VxD 调用前后 / 每次 SetCursor)
 * 真机实测: 每行 = 6 次 INT 21h(open/seek/2×write/commit/close),
 * 一次会话 5953 行 ≈ 3.6 万次 DOS 调用。                              */
#define DIAG( s )   do { if (g_logLevel >= 2) DiagWriteLog( s ); } while (0)
#define DIAGM( s )  do { if (g_logLevel >= 1) DiagWriteLog( s ); } while (0)
/* 里程碑落盘: ./39 实锤 Enable 早期步骤间无落盘点, 死机则整段日志
 * 随缓存丢失。每个 DIAG 里程碑后紧跟 FLUSH()。
 * 【r109】闸门从 >=2 放宽到 >=1: Log=1 的语义是"只打里程碑", 而里程碑
 * 全部由 DIAGM 写出 —— 若 FLUSH 仍卡在 >=2, Log=1 的日志就只留在 DOS
 * 文件缓存里(关机/崩溃即丢), 等于这个级别不可用。放宽后 Log=1 得到的是
 * "里程碑 + 每行落盘", 正好是"低开销但要能追认死在哪儿"的那一档。
 * Log=0 依旧是完全静默(它的已知副作用见 R109_CHANGES.md: Log=0 那几次
 * 启动都回落了 VGA, 机理未定, 所以本轮不鼓励用 0)。                 */
#define FLUSH()     do { if (g_logLevel >= 1) DiagFlush(); } while (0)
/* 数字诊断(与 drv.c 的 DiagHex 同机制, 两边各持一份静态缓冲) */
static void DiagHex( const char *msg, uint16_t v )
{
    static char       buf[48];
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = buf;

    if (g_logLevel < 2)
        return;
    while (*msg)
        *p++ = *msg++;
    *p++ = '=';
    *p++ = hexd[(v >> 12) & 15];
    *p++ = hexd[(v >> 8) & 15];
    *p++ = hexd[(v >> 4) & 15];
    *p++ = hexd[v & 15];
    *p   = 0;
    DiagWriteLog( buf );
}

/* 【r103】DiagCfg —— 一行落盘若干策略值(配置自证)。
 * 【为什么必须加】./102 的两份会话日志: debug 构建只剩 PROBE 一行、
 * release 构建只有 PROBE + VDDGETDISPCONF 两行 —— 事后完全无法确认
 * "那一轮到底跑的是哪套 INI"。把最关键的几个开关放在同一行, 只要日志
 * 活着, 一次 grep 就能读出配置, 不必再靠"用户当时改了哪个键"回忆。
 * 【r105】从 4 个值扩到 5 个(加 CursorSrc): 光标形状取证时机决定
 * `CursorShape:`/`EXCL` 这些行出不出现, 属于"必须能追认"的开关。
 * 缓冲仍然只有 64 字节 —— 每多一个值占 5 字节, 标签本身不能太长。 */
static void DiagCfg( const char *tag, uint16_t a, uint16_t b,
                     uint16_t c, uint16_t d, uint16_t e )
{
    static char       buf[96];      /* r105: 标签 42 + 5*5 个十六进制 + NUL */
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = buf;
    uint16_t          v[5];
    int               i, k;

    if (g_logLevel < 2)
        return;
    while (*tag)
        *p++ = *tag++;
    v[0] = a; v[1] = b; v[2] = c; v[3] = d; v[4] = e;
    for (i = 0; i < 5; i++)
    {
        *p++ = ' ';
        for (k = 12; k >= 0; k -= 4)
            *p++ = hexd[(v[i] >> k) & 15];
    }
    *p = 0;
    DiagWriteLog( buf );
}

/* --- r81: 函数远地址 → DWORD(sel<<16 | off) --------------------------------
 * 用途: 把本模块各导出函数的 sel:off 打进日志 —— 蓝屏给的 CS:EIP 若命中
 * 其中一条, 就能直接算出"在哪个函数的 +多少", 不必依赖 map 文件对版本。
 * 【为何走 union】函数指针直接转成整数在 Watcom 下会给警告, union 是零
 * 风险写法(DrvInitCore 填 g_dibfn[] 用的是同一手法)。               */
static DWORD FarFn( void (__far *fp)( void ) )
{
    union { void (__far *f)( void ); DWORD dw; } u;

    u.f = fp;
    return u.dw;
}
#define FARFN(f)    FarFn( (void (__far *)( void ))(f) )
#else
#define DIAG( s )   ((void)0)
#define DIAGM( s )  ((void)0)
#define DiagHex( m, v ) ((void)0)
#define DiagCfg( t, a, b, c, d, e ) ((void)0)
#define FLUSH()     ((void)0)
#endif

/*
 * FarData —— 把 16:16 数据远指针打包成 DWORD(低字=偏移, 高字=选择子)。
 *   【为何走 union】与 FarFn 同理: 指针直接转整数在 Watcom 下会招警告, union
 *   是零风险写法。r84 用它把 lpInfo/lpDevice/lpBits 交给汇编侧调用
 *   CreateDIBPDevice(见 Cdpd 与 vddapi16.asm 的 CDPD)。
 */
static DWORD FarData( const void __far *p )
{
    union { const void __far *f; DWORD dw; } u;

    u.f = p;
    return u.dw;
}

/* --- 本驱动版本: 0x0400 = Windows 4.0(即 Win95)DDI ---------------------- */
#define DRV_VERSION     0x0400

/*==========================================================================
 *  【r131】PHASEV —— 无条件骨架日志("这一轮到底跑到哪一步"的第三种通道)。
 *   见 drv.c 的 DiagHexPhase 长注。三档构建都会写, 所以:
 *     · release 档不再只有两行日志("release 与 debug 行为不一致"能定位了);
 *     · 判读只靠【位置 + 顺序 + 一个十六进制值】, 不依赖长消息串(体积友好)。
 *   用法一律是"决策点 + 一个值", 例如 PHASEV( "PH: restore ok", ok )。
 *   r131-marker: PHASEV (unconditional skeleton log, all three builds).
 *========================================================================*/
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
#define PHASEV( m, v )  DiagHexPhase( m, (uint16_t)(v) )
#else
/* 【r166】release 档: 骨架日志一并编掉(与 drv.c 的 DiagHexPhase 同一开关)。
 *   依据: ./165 的 release 档写出 906623 字节的 PH: 日志, 而目标是零日志。*/
#define PHASEV( m, v )  ((void)(m), (void)(v))
#endif

/* --- 上报给 GDI 的物理尺寸(影响字体换算, 数值沿用 DDK 示例惯例) -------- */
#define DISPLAY_HORZ_MM     208     /* 假想 208x156 mm 的屏幕             */
#define DISPLAY_VERT_MM     156
#define DISPLAY_SIZE_EN     325     /* 英制单位尺寸                       */
#define DISPLAY_SIZE_TWP    2340    /* twip 单位尺寸                      */

/* --- USER 中未公开的重绘回调的序号(Win9x 上恒为 275) -------------------- */
#define REPAINT_ORDINAL     275

/* --- ValidateMode 的返回值与参数结构 ------------------------------------
 * 【为何不 #include <valmode.h>】DDK 的 valmode.h 里 ValidateMode 的原型
 * 缺少 __loadds, 与本实现的属性冲突(参考实现 vmdisp9x 用 Watcom 专有的
 * #pragma aux loadds 绕过, 不通用)。这里内联等价定义, 值与 DDK 一致。 */
#define VALMODE_YES         0   /* 模式可用                             */
#define VALMODE_NO_WRONGDRV 1   /* 硬件不属于本驱动                     */
#define VALMODE_NO_NOMEM    2   /* 显存不足/模式不存在                  */
#define VALMODE_NO_NODAC    3   /* DAC 不支持该位深                     */
#define VALMODE_NO_UNKNOWN  4   /* 其它问题                             */

typedef struct {
    UINT    dvmSize;            /* 结构大小                             */
    UINT    dvmBpp;             /* 位深                                 */
    int     dvmXRes;            /* 水平分辨率                           */
    int     dvmYRes;            /* 垂直分辨率                           */
} DISPVALMODE;

/* --- Win3.x DDK 的光标形状结构(SetCursor 的参数) ------------------------
 * gdidefs.h/dibeng.h 均未定义它, 这里按 DDK 文档内联。本驱动不解析内容,
 * 直接转交 DIB_SetCursorExt, 定义它只为原型完整。                       */
typedef struct {
    short int   xHotSpot;
    short int   yHotSpot;
    short int   cx;
    short int   cy;
    short int   cbWidth;
    BYTE        Planes;
    BYTE        BitsPixel;
} CURSORSHAPE;

/* --- (已移除) GlobalSmartPageLock: Win3.x 半公开函数, Win9x 的 KERNEL
 * 并未按 Watcom 导入库所据的序号导出它(实测 KERNEL.230 不是它, 调用即
 * 在 KERNEL 内 GP 蓝屏); vmdisp9x 等在售参考驱动也都不锁代码段
 * (其 _LinPageLock 只用于 VxD ring0 侧)。16 位 drv 代码段不锁, 随访。 */

/*==========================================================================
 *  全局变量
 *
 *  【命名警告】VDDEntryPoint / OurVMHandle / lpDriverPDevice / wPalettized
 *  / g_dibfn / wDibDgroup / g_traceDone / g_dibSaveIdx / g_dibSaveDS /
 *  g_dibSaveOff / g_dibSaveSeg / g_thunkPOff / g_thunkPSeg / g_runEdx /
 *  g_edxCount 被 vddapi16.asm 以 _名字 直接引用, 改名必须同步汇编!
 *========================================================================*/

DWORD   VDDEntryPoint = 0;      /* 主 VDD 的 PM API 入口(INT 2F/1684 取得)*/
WORD    OurVMHandle   = 0;      /* 本 VM 句柄(INT 2F/1683 取得)           */

WORD    wScrX       = 640;      /* 配置的 X 分辨率(用户选定, 未必已生效)  */
WORD    wScrY       = 480;      /* 配置的 Y 分辨率                        */
WORD    wDpi        = 96;       /* DPI                                    */
WORD    wBpp        = 8;        /* 位深                                   */
WORD    wPalettized = 0;        /* 8bpp 且使用调色板                      */

WORD    wScreenX          = 0;  /* 实际生效的宽(像素)                     */
WORD    wScreenY          = 0;  /* 实际生效的高(像素)                     */
WORD    wScreenPitchBytes = 0;  /* 实际生效的每行字节数                   */
WORD    wPDeviceFlags     = 0;  /* DIBENGINE deFlags 的基底               */

LPDIBENGINE lpDriverPDevice = 0;    /* 交给 GDI 的 PDEV(DIBENGINE 指针)   */
WORD        wEnabled        = 0;    /* 设备已使能(光标函数据此设防)       */
RGBQUAD FAR *lpColorTable   = 0;    /* 当前颜色表(BITMAPINFO.bmiColors)   */

static BYTE bReEnabling = 0;    /* ReEnable 过程中(跳过一次性初始化)      */
static WORD wDIBPdevSize  = 0;  /* DIB Engine 要求的 PDEV 大小            */

static WORD g_fbSel = 0;        /* 帧缓冲选择子(0=未建)。
                                   r76: 由 drv.c 的 LfbSelector_Ensure 用
                                   DPMI 0800h + DPMI LDT 选择子建立, 只有
                                   建成功(非 0)才可能走到写帧缓冲的代码;
                                   会话内常驻复用(Disable 不释放, 理由见
                                   drv.c LfbSelector_Release 注释)。      */

/* --- 32 位偏移清屏(vddapi16.asm 的 FBCLEAR) ------------------------------
 * 【为何不能直接用 _fmemset + __huge】帧缓冲选择子的界限覆盖整个 LFB
 * (可达数 MB), 但 16 位远指针的偏移只有 16 位: 超过 64KB 就回绕。
 * 若用 Watcom 的 __huge 指针做 p += n, 巨型指针归一化会去"加选择子
 * 号"(保护模式下 __HShift=3, 每 8 字节 +1 个选择子) —— 而我们只有
 * 一个选择子, 结果是写到别的 LDT 表项上去, 灾难。故清屏交给汇编:
 * 装 ES, 用 EDI 的 32 位偏移 + rep stosd 覆盖全部字节。            */
extern void __far __pascal FbClear( void );
WORD  FbClearSel   = 0;         /* FBCLEAR 读的 DGROUP 变量(名字与
                                   vddapi16.asm 的 extrn _FbClearSel 对应) */
DWORD FbClearBytes = 0;         /* 要清的字节数 */
DWORD FbClearOff   = 0;         /* 清屏起始偏移(r80): "0 基址选择子"形态下
                                   等于帧缓冲的线性地址, 退回形态下为 0。
                                   必须与 deBitsOffset 同源(见 drv.c 的
                                   LfbAccessOffset 头注), 否则清屏会清到
                                   别的地方去。 */

/* --- r103: 可见帧缓冲的原始字节取样(花屏取证) -----------------------------
 * 【它回答的问题】"./102 的桌面被 4 像素周期竖条纹盖住" 这类现象, 光靠
 *   截图判不出是谁写的: 截图经过缩放(1021x766), 像素值已经不可信;
 *   而"帧缓冲里那 8 个字节到底是什么"能一眼分辨凶手:
 *     · 文本单元对(如 0x0720 / 0x2055 反复) → 主 VDD 把 DOS 的 VGA 文本
 *       缓冲写进了可见屏(0x0720 按 RGB565 = 纯绿);
 *     · 4 个字节一组的等距图案 → 4 平面/链式写入落到了打包像素屏上;
 *     · 整齐的 16bpp 像素(如 0x0555 反复) → 是 GDI/我们自己的填色。
 * 【为什么只能取前 64KB】16 位远指针的偏移只有 16 位(见 FBCLEAR 头注),
 *   而屏选择子是"0 基址 + 线性偏移"形态 ⇒ MK_FP(sel, off) 只对 off<64K
 *   有效。1024x768x16 的可见屏有 1.5MB, 但整屏条纹是均匀的, 取开头
 *   32KB 足以定性。取不到选择子/几何未定时直接返回(绝不猜)。
 * 【调用点】drv.c 的 VxdTraceDump: 每当 mini-VDD 的 VGA/hires 状态位
 *   变化(即主 VDD 正在进/出 DOS 全屏会话)就打一次, 不产生持续噪声。 */
/* --- r106: 把 16bpp 字按 RGB565 展开成 RRGGBB 打一行 --------------------
 * 【为什么需要它】./104 的分析里, `FB s lo=03EF` 被读成了"VGA 文本单元的
 * 残留", 而 0x03EF 恰恰就是本驱动桌面背景色 (0,127,127) 的 565 编码 ——
 * 一个读数被误读, 整条结论就会反向。人眼可判的颜色能杜绝这类误读:
 *   0x03EF → 007F7F(青绿桌面)   0x0720 → 00E000(纯绿, DOS 文本单元)
 * 只服务诊断; 发布构建下 DiagFbSample 里的调用被空宏吃掉, 故用宏替身,
 * 免得留一个"未被引用的静态函数"警告。                              */
#ifdef VBE9X_DIAG
static void DiagRgb565( const char *msg, WORD w )
{
    static char       buf[56];
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = buf;
    WORD              r, g, b;

    if (g_logLevel < 2)
        return;
    while (*msg)
        *p++ = *msg++;
    *p++ = '=';
    r = (WORD)((w >> 11) & 0x1F);
    g = (WORD)((w >>  5) & 0x3F);
    b = (WORD)( w        & 0x1F);
    r = (WORD)((r << 3) | (r >> 2));        /* 5 位 → 8 位, 位复制 */
    g = (WORD)((g << 2) | (g >> 4));        /* 6 位 → 8 位          */
    b = (WORD)((b << 3) | (b >> 2));
    *p++ = hexd[(r >> 4) & 15];  *p++ = hexd[r & 15];
    *p++ = hexd[(g >> 4) & 15];  *p++ = hexd[g & 15];
    *p++ = hexd[(b >> 4) & 15];  *p++ = hexd[b & 15];
    *p   = 0;
    DiagWriteLog( buf );
}
#else
#define DiagRgb565( m, w )  ((void)0)
#endif

/* 【r106】把这两个 16bpp 字按 RGB565 解出来再打一行。
 * 为什么必须加: ./104 的分析里, 有人把 `FB s lo=03EF` 读成了
 * "VGA 文本单元的残留", 而 0x03EF 其实就是本驱动桌面背景色
 * (0,127,127) 的 565 编码 —— 读数被误读, 结论就会反。
 * 打成人眼可判的颜色后, "帧缓冲里是桌面还是文本单元" 一眼可辨:
 *   0x03EF → 007F7F(青绿桌面) / 0x0720 → 00E000(纯绿文本单元)。 */

void __far __pascal DiagFbSample( void )
{
    BYTE __far *p;
    DWORD       base, span;
    WORD        off;
    int         i, n;

    if (g_logLevel < 2)
        return;
    if (g_fbSel == 0 || wScreenPitchBytes == 0 || wScreenY == 0)
        return;
    base = LfbAccessOffset();               /* 0 基址形态下 = 线性地址 */
    if ((base >> 16) != 0)
        return;                             /* 偏移超出 16 位: 16 位远指针够不着 */
    /* 【r104 修一个真 bug】r103 这里写的是 `span = (WORD)(pitch * height)`,
     * 而 1024x768x16 正好 = 24 × 64KB = 0x180000, 被 WORD 截断成 0
     * ⇒ n = 0 ⇒ 被下面的 n<=0 兜成 1 ⇒ 每次只取了一组 4 字节。
     * ./103 四份日志里 "FB sample" 永远只有 lo/hi 两行, 就是这么来的 ——
     * 一个"取样器"退化成了"看第一个双字"。现在全程 32 位算术。
     * 【仍然只能是 64KB 以内】屏选择子是"0 基址 + 线性偏移"形态,
     * MK_FP(sel, off) 的 off 只有 16 位(见 FBCLEAR 头注)⇒ 只能采到可见屏
     * 最前面 64KB。对 1024x768x16 那是前 32 行 —— 而 VGA 窗口(128KB)正好
     * 覆盖在前 64 行里, 所以这个窗口恰好是"DOS 的 VGA 访问有没有落进可见屏"
     * 最该看的地方。要看整屏请配合截图(截图缩放会改像素值, 但图案周期与
     * 颜色分布仍然可读)。                                             */
    span = (DWORD)wScreenPitchBytes * (DWORD)wScreenY;
    DiagHex( "FB span lo",   (WORD)span );
    DiagHex( "FB span hi",   (WORD)(span >> 16) );
    DiagHex( "FB pitch",     wScreenPitchBytes );
    DiagHex( "FB base lo",   (WORD)base );
    DiagHex( "FB limit",     0xFFFF );       /* 16 位偏移的硬上限 */
    n = 16;                                  /* 64KB / 16 = 4KB 一步 */
    for (i = 0; i < n; i++)
    {
        WORD lo, hi;

        off = (WORD)(i * 0x1000);            /* 4KB 步进, 永不越过 64KB */
        if (((DWORD)base + (DWORD)off) > 0xFFFFUL)
            break;                           /* 越过 16 位可达范围: 停, 不绕回 */
        p   = (BYTE __far *)MK_FP( g_fbSel, (WORD)(base + off) );
        lo  = *(WORD __far *)p;
        hi  = *(WORD __far *)(p + 2);
        DiagHex( "FB s lo", lo );
        DiagHex( "FB s hi", hi );
        DiagRgb565( "FB s rgb lo", lo );
        DiagRgb565( "FB s rgb hi", hi );
    }
    FLUSH();
}

/* --- r82 帧缓冲"写/读回"自检(vddapi16.asm 的 FBTEST) ---------------------
 * 【它回答的问题】"VBE 说的显存"到底是不是"我们真能访问的显存"。
 *   VM 里这不是问题(显存=宿主内存, 孔径永远够); 真机上经常是问题:
 *   VBE 报 256MB 而孔径只有 64MB、或映射成功但高地址别名回低地址 ——
 *   于是按上报值拿到的离屏显存是个空洞, GDI/DIBENG 一往上画就花屏/黑屏。
 *   唯一不依赖任何假设的判据就是: 写进去、读回来、比一比。
 * 【32 位指令】FBTEST 全程用 EAX/ECX/EDX/EDI + a32 前缀, 一次搬 4 字节
 *   (与 FBCLEAR 同款), 花样随位置变化以捕获地址别名。               */
extern WORD __far __pascal FbTest( void );
WORD  FbTestSel = 0;            /* 与 _FbTestSel 对应(通常复用帧缓冲选择子) */
DWORD FbTestOff = 0;            /* 自检起始偏移(32 位, 选择子内偏移)       */
DWORD FbTestLen = 0;            /* 自检字节数(4 的倍数, ≤ 64KB)            */
DWORD FbTestPat = 0;            /* 花样基值(实际写入 pat+i, i 为双字序号)  */

/* 自检结论(供 SetDisplayMode 决定 OFFSCREEN 位与显存上报) */
static WORD  g_fbHeadOk   = 0;  /* 屏尾可写(帧缓冲真的能用)               */
static WORD  g_fbTailOk   = 0;  /* 声称显存的尾部可写(离屏显存可信)       */
static DWORD g_fbHeadBytes = 0; /* 自检时可见帧缓冲的字节数(供日志/钳制)  */
static WORD  g_fbMeasured  = 0; /* r83: 自检是否真的测到过至少一个采样点 ——
                                   "没测"与"测出坏"必须分开: r82 因为一个
                                   返回值被覆盖的 bug, 两者被混为一谈。 */

/* --- r81 运行时配置(system.ini [omninine]) ----------------------------------
 * 【为什么要有它】目前挡在"点亮桌面"前面的, 是几个无法从文档判定的选择:
 *   屏选择子该是"0 基址 + 线性偏移"还是"基址=帧缓冲 + 偏移 0"? DIBENGINE.
 *   deBitsOffset 该填线性地址还是 0? OFFSCREEN 位要不要设? 清屏是否参与
 *   故障? 若每试一种都要重新编译 + 装驱动 + 重启, 一轮只能试一种。
 *   改成 system.ini 里几个键之后, 一轮可以把整个矩阵跑完(改一行、重启),
 *   而且不必再往构建里塞任何"实验分支"。
 * 【通用性说明 —— 与"按 OS 版本写死"是完全不同的两件事】
 *   这些键只影响本驱动【自身】的可选行为, 不读取、不判断、不依赖任何
 *   版本的 DIBENG/GDI/USER; 键不存在或读不到时一律取默认值, 此时的行
 *   为与"完全没有这段代码"逐指令等价。
 * 【键】(节名恒为 vbe9x, 文件恒为 system.ini; 都是可选的)
 *   SelMode  0 = 0 基址(flat)屏选择子 + 线性偏移[默认]; 1 = 基址=帧缓冲
 *   Flags    wPDeviceFlags 的强制值(十六进制; 0 = 用内置默认)
 *   BitsOff  0 = deBitsOffset 用 LfbAccessOffset()[默认]; 1 = 恒填 0
 *   Clear    1 = 切模式后清可见屏[默认]; 0 = 不清
 *   DumpOff  机器码转储的段内偏移(十六进制, 默认 2B60)
 *   Sentinel 1 = DIB_Enable 之前把 PDEV 的字段填成固定花样, 以便看清
 *            DIB_Enable 究竟读写了哪些字段(默认 0 = 不碰)
 *   EndShot  1 = 第一次 EndAccess(DIBENG 画完第一笔)后抄一份 PDEV 原始
 *            字节。默认 0 —— 那是在 DDI 回调里写日志, 与本驱动其它落盘
 *            点(Enable 期)不属于同一上下文, 没在真机验证过之前不默认开。
 *   SelfTest 1 = 帧缓冲"写/读回"自检[默认开]: 抽样验证屏尾与"声称显存"
 *            尾部真的可写; 显存尾失败 → 撤 OFFSCREEN 位 + 把上报显存钳到
 *            可见帧缓冲大小(防真机孔径/别名导致的随机花屏)。0 = 关闭自检
 *            与钳制, 行为退回 r81(用于 A/B 判断是不是自检在捣乱)。
 * 【读到什么一目了然】ReadRunProfile 会把 8 个值全部落盘 —— 日志本身就是
 *   这次的配置记录, 事后不必回忆"那次测的是哪种组合"。               */
static WORD s_inModeSet  = 0;   /* [r406] 1 = ApplyDesktopRestore 执行中 */
static WORD s_modeTick   = 0;   /* [r406] s_inModeSet 置位时刻 */
static WORD g_runSelMode  = 0;
static WORD g_runVerify   = 1;   /* r89: descriptor read-back gate (default on) */
static WORD g_runAccess   = 0;   /* r90: 0 = our Begin/EndAccess callbacks,
                                  *      1 = DIBENG's own (ordinals 105/106) */
static WORD g_runDibEnable = 0;  /* r91: 0 = do NOT call DIB_Enable ourselves
                                  *      (GDI calls the engine's Enable after
                                  *      our Enable returns); 1 = old r80..r90
                                  *      behaviour, i.e. call it here */
static WORD g_runFlags    = 0;
static WORD g_runBitsOff  = 0;
static WORD g_runClear    = 1;
static WORD g_runDumpOff  = 0x2B60;
static WORD g_runDumpLen  = 64;   /* r85: 机器码窗口宽度(DumpLen, 16..512) */
static WORD g_runSentinel = 0;
static WORD g_runEndShot  = 0;
static WORD g_runSelfTest = 1;   /* r82: 帧缓冲写/读回自检(默认开) */
static WORD g_runCrashSel = 0;   /* r83: 上次蓝屏的段选择子(十进制, 0=不抄) */
static WORD g_runCrashOff = 0;   /* r83: 上次蓝屏的段内偏移(十进制)        */

/* --- r99: 策略开关(new defaults may differ from r98; see each entry) -----
 * VddVirtualize —— VDD_DRIVER_REGISTER 的 EDX。文档原文:
 *   "EDX = 0 if the mini-VDD supports VGA 4 plane virtualization in a
 *    window or -1 if the display requires that VGA 4 plane graphics
 *    programs run full-screen and can not be windowed."
 *
 * 【r104: 默认 0 → 1】(= 注册时 EDX = -1)
 *   文档里紧跟着的那句就是判据:
 *     "The VDD supports VGA graphics in a window by allocating a 32K or 64K
 *      block of video memory **right below the visible Windows screen**. …
 *      However, some hardware cannot simultaneously have the VGA data
 *      pathway and the packed-pixel hi-res pathway running. **If your
 *      hardware cannot, set EDX to -1 and the VDD will not attempt VGA
 *      virtualization in a window.**"
 *   我们的可见屏起点就在显存偏移 0(`SetDisplayMode: phys lo=0000`,
 *   1.5MB@1024x768x16), "可见屏之前"根本不存在 ⇒ 主 VDD 无处安放那块
 *   32K/64K 虚拟化区。./103 四份日志给出的是同一个结论, 而且是两条独立
 *   证据:
 *     ① 注册返回值 `reg hi == vis hi`(0018/0018)⇒ 主 VDD 自留 0 字节;
 *     ② GET_VDD_BANK(1) 被调用时入口 `ecx=00180000`(正是可见屏字节数,
 *        即"那就放在可见屏之后?")—— 它在试探, 而 r101 起的 VddBank=1
 *        把这个试探拒掉了。
 *   也就是说: 我们【对外声明】(EDX=0)自己能做窗口化 4 平面虚拟化,
 *   实际上既没有那块显存、也没有实现该族任何一项(minivdd.asm 里 1/2/3/
 *   11/12/13/14/32/33/34/43 全是"记账后原样转给主 VDD"的空壳)。声明与
 *   能力不符, 主 VDD 就会带着这个错误前提去做窗口化虚拟化。
 *   EDX=-1 是文档给出的正式出口, 语义恰好是我们要的: 主 VDD 不再尝试
 *   窗口化 VGA 虚拟化 ⇒ 不会再有"它在可见屏上虚拟化"的路径。文档同时
 *   说明这【不影响文本与非平面图形(如 13h)在窗口里跑】(r101 注释里已
 *   引用过同一句), 所以窗口 `command`(文本模式)仍然可用, 被挡在门外的
 *   只有"窗口化 VGA 4 平面图形"。另外文档明说: `Client_EDX == -1` 时
 *   主 VDD 【根本不会调用 GET_VDD_BANK】⇒ VddBank 那个开关随之失效。
 *
 *   0 = 恒 0(声明支持窗口化 4 平面虚拟化 —— r99..r103 的默认);
 *   1 = 恒 -1【r104 默认】; 2 = 按位深(≥16bpp 取 -1; 8bpp 保留 0)。  */
static WORD  g_runVddVirt = 1;
static WORD  g_runVesa    = 0;   /* VesaPolicy: VESA_SUPPORT(41) 策略     */
static WORD  g_runHires   = 0;   /* HiresAns:   CHECK_HIRES_MODE(35) 策略 */
/* Restore —— ResetHiResMode(=我们的 RestoreDesktopMode) 的策略。
 * 【r100 重定义: 回调本身永远不再做 BIOS/DOS/VxD 调用】
 *   0 = 【新默认】回调只清 BUSY + 置待办标志(纯内存), 真正的重建推迟到
 *       下一次安全上下文: 先验证, 只有确实不对才重设模式;
 *   1 = 同 0, 但推迟的重建【完全不发 INT 10h】(只刷 DAC + 清 BUSY + 重绘)
 *       —— 用于回答"回桌面到底需不需要我们重设模式";
 *   2 = 同 0, 但推迟的重建【总是】重设模式(不做验证; = r99 的 Restore=2);
 *   3 = 【对照】回调里直接做 r98 的完整重建 —— 会被 bInVddCallback 护栏拒绝
 *       (drv.c VbeExecBios), 日志里留下 REFUSED, 用来证明护栏生效。
 * 真机实测(r99 默认 0): 三次会话都死在回调里那次嵌套 V86 4F02 上。   */
/* Heal —— 1 = 发现"模式不是我们设的那个"时, 在下一次 ring-3 上下文
 *   自动重设一次(把 E4/E10 从人工盲操作变成驱动自己做)。默认 0。    */
static WORD  g_runHeal    = 0;
/* 【r100】SetMode2 —— PhysicalEnable 里 VDD_DRIVER_REGISTER 之后那次
 *   "保险"再设模式(对照 boxv9x: 它没有这个动作, 而且它发生在"主 VDD 已被
 *   告知模式是 X"之后 ⇒ 自找状态不一致)。默认 0 = 不做; 1 = r99 行为。 */
/* 【r100】ReSet —— 推迟重建完成后是否再调一次 VDD_DRIVER_REGISTER
 *   (DDK 对 ResetHiResMode 的六项义务之一: "should call the VDD
 *    VDD_DRIVER_REGISTER function just as was done at the initial
 *    PhysicalEnable")。默认 1; 0 = 不重注册(用于 A/B)。              */
static WORD  g_runReSet    = 1;
/* 【r100】Cursor —— 推迟重建时是否用保存下来的 lpCursorShape 重建硬件
 *   光标(六项义务之一, r98/r99 完全没做)。默认 1; 0 = 不重建。       */
static WORD  g_runCursor   = 1;
/* r101-marker: VddBank/Int10Mode policy + HiresAns 4-state (see minivdd.asm) */
/* 【r101】VddBank —— GET_VDD_BANK(1) 是否按 DDK 原文拒绝"窗口化 4 平面
 *   虚拟化"(Client_EDX = 入口 Client_ECX + CY)。
 *   ./100 的统一根因就是这条: 我们在 VDD_DRIVER_REGISTER 里声明 EDX=0
 *   ("支持窗口化 4 平面虚拟化"), 却让主 VDD 带着 0 字节的虚拟化区去做
 *   4 平面虚拟化 ⇒ DOS 会话的 VGA 访问(文本单元 0x0720)落进可见屏。
 *   默认 1 = 拒绝。置 0 = r98/r99 行为(留给 A/B)。详见 minivdd.asm。 */
static WORD  g_runVddBank  = 1;
/* 【r101】Int10Mode —— 48 号 PRE_INT_10_MODE_SET 是否吃掉 DOS 应用的
 *   设模式请求(仅在 Windows 屏幕仍在场时)。默认 0 = 只记录不改行为。  */
static WORD  g_runInt10    = 0;
/* r102-marker: log-channel hardening + Offscreen/Access2 A/B switches */
/* 【r102】Offscreen —— deFlags 里的 OFFSCREEN(0x0008)位。
 *   【为什么把它提成开关】./100 的陈旧行取证(2026-xx 重测)显示: 那条
 *   异常扫描行就是 1024x768 启动画面里 Windows 彩旗的【一行像素】
 *   (FF3300@2+69 FFCC00@71+80 66CC33@151+72 0099FF@223+67 —— 恰好是
 *   0..289 共 290 像素宽的彩旗横截面, 且逐行左侧内缩 1 像素 = 那道
 *   45 度斜边的斜率)。而启动画面消失之后, 它的像素只可能还留在
 *   【显存里屏以外的那 1MB】(4MB 卡 - 3MB 可见帧缓冲)—— 也就是
 *   DIBENG 的离屏堆。⇒ 有东西把离屏堆里的旧像素抄回了可见屏。
 *   嫌疑最大的是 DIBENG 的离屏面用法(软件光标 save-under 也在其中),
 *   而这条路径的开与关【只由这一个位决定】。
 *   默认 1 = 照 r82 自检结论声明(屏尾/显存尾都通过就置位);
 *   置 0 = 不声明 OFFSCREEN(DIBENG 走"没有离屏堆"的另一条路)。
 *   成本极低而信息量极大, 所以做成 INI 键而不是重编。               */
static WORD  g_runOffscreen = 1;
/* 【r102】Access2 —— deBeginAccess/deEndAccess 值得不到的第二形态。
 *   Access=0(默认) 装我们那两个"直接透传给 DIBENG 的 DIB_BeginAccess /
 *   DIB_EndAccess"的回调; 问题在于: DIBENG 在自己的光标/表面对齐流程里
 *   【本来就会调这两个序号】, 于是"引擎→我们的回调→再进引擎"这条回环
 *   在光标路径上随时可能成环。Access2=1 改为装两个什么都不做的空回调
 *   (本驱动没有需要排除的硬件光标; 离屏面访问也不需要闸门)。
 *   语义与 Access 正交(互斥): Access=1 仍表示装引擎自己的。         */
static WORD  g_runAccess2  = 0;

/* 【r110】ReArm —— "重建被护栏拒绝 → 立刻重新挂起"。
 * 【它修的是什么】./109 的 t2 给了本次会话最要紧的一条时间轴: 一次
 *   Alt+Enter 往返里, 主 VDD 连续调用 mini-VDD 的 03h/09h/0Fh 三个银行
 *   服务数百次(把高分辨率屏搬进/搬出 64KB 窗口), 我们的 ring-3 入口随时
 *   可能正落在【主 VDD 的切屏事务内部】。那一刻若延迟重建的边沿被取走,
 *   r100 的硬护栏(VbeExecBios: 回调窗口内绝不发嵌套 V86/BIOS 调用 ——
 *   那是 r99 三次死机换来的)会拒绝, SetDisplayMode 失败, 而 r109 的
 *   代码在那条失败分支里【直接 return】, bRestorePending 已被清零 ⇒
 *   这次重建请求被永久丢弃。用户看到的就是"按 Alt+Enter 回来之后一直花,
 *   要么再按一次 Alt+Enter、要么盲打 exit 才能恢复"。
 *   实测对照: 同一个 20 秒窗口里两次 V2H 边沿(arm n=1 / n=2), 而
 *   48 号(PRE_INT_10_MODE_SET)计数只涨了 2(= 只发生过一次 4F02)——
 *   即"有一次边沿被取走却没有落到硬件上"。
 * 【改法】失败分支不再丢弃: 若判据是"被护栏拒绝"(VxdTakeBiosRefused=1)
 *   或 SetDisplayMode 直接失败, 就把 pending 重新挂起(并把入口侧标记
 *   原样留着, 闸门会再审查一次), 等下一个安全时机重试; 上限 8 次,
 *   避免在本来就坏的机器上空转。ReArm=0 可整体退回 r109 行为。        */
static WORD  g_runReArm     = 1;
/* r110-marker: rebuild re-arm + LogBoot handoff + PortGrp + CFG2 */

/* 【r111】CbRestore —— 恢复路径到底在哪一侧执行。
 *
 *   【为什么本轮要动它 —— 这是用户对 r111 的明确要求】
 *   "完全采用微软官方要求的恢复/绘制路径, 并把我们目前的成果作为官方路径
 *    不成功的兼容层/回落层。"
 *   DDK 的官方路径是明确的(见 DOSBOX_SWITCHING_REFERENCE.md §2.2 的原文):
 *   主 VDD 在把屏幕还给 Windows 时会【回调显示驱动经 VDD_DRIVER_REGISTER
 *   登记的 ResetHiResMode】, 而那个回调的六项义务就是"把硬件设回 Windows
 *   模式 / 重建调色板 / 重建硬件光标 / 清 BUSY / 再调一次 VDD_DRIVER_REGISTER
 *   / 调 VDD_SAVE_DRIVER_STATE"。r100..r110 的实现在那个回调里【只置一个
 *   标志】, 真正的重建推迟到"下一次 DDI 入口"—— 这是我们自己发明的时序,
 *   不是文档的。它的代价在 ./110 里看得很清楚:
 *     · t2/t3/t7: 官方的回调确实被调用了(`CB evt=0101/0102/0103`), 但
 *       屏幕并不会因此变好 —— 变好靠的是我们那条延迟路径;
 *     · t3 收尾那次: 连回调都没有(`CB evt=0508` = 等不到信号走了 8 次兜底),
 *       于是"exit 之后桌面回来"完全是我们的功劳, 与官方路径无关;
 *     · t5: 那条延迟路径在 DOS 会话【进行中】连开 8 枪 4F02 全部失败
 *       (它是在跟拿着硬件的 DOS 盒抢硬件), 然后再重挂 8 次 —— 一整轮
 *       就是在跟官方时序打架。
 *
 *   1 = 【r111 默认】官方路径优先: ResetHiResMode 回调里【直接】把六项义务
 *       做完(模式 + DAC + 光标 + 清 BUSY + 再注册 + SAVE_DRIVER_STATE +
 *       清屏), 只把"请求 USER 重绘"推迟到下一个 DDI 入口(USER 回调不能
 *       从 VDD 的回调里进)。失败/被拒(护栏或 VM 自检)才置 pending, 交给
 *       下面那条回落路径。
 *   0 = r110 行为(回调只置标志), 留作 A/B —— 也是"官方路径把机器搞坏时"
 *       一键退回兼容层的方式。
 *   【安全性】回调里的那次 BIOS 调用要走两道门: ① drv.c 的一次性通行证
 *   (VxdAllowBios: 默认拒绝一切回调期 BIOS 调用, 这是 r99 三次死机
 *   换来的护栏); ② mini-VDD 的 VM 自检(当前 VM 必须是 Windows VM, 否则
 *   回 93 并记账)。任何一道不放行 ⇒ 立刻回落, 绝不死等。             */
static WORD  g_runCbRestore = 1;
/* 【r112】CbHwSet —— 官方路径的"重设模式"用【寄存器回放】而不是嵌套 4F02。
 *   【为什么改默认实现】见 drv.c 的 VxdHwRegSave 长注: ./111 的三组读数把
 *   "回调里发嵌套 V86 调用"这条路判死了(t2/t3/t6 带预算真发 4F02 ⇒ 全冻结;
 *   t8 那次被护栏拒 ⇒ 活着; t5 回调里不发 BIOS ⇒ 活着)。而 DDK §2.2 的
 *   第一项义务"把硬件设回 Windows 的模式"不能不做 ⇒ 换成真实显示驱动的
 *   做法: 直接重编程寄存器(不需要 BIOS、不需要 V86、不需要 VDD 配合)。
 *   1 = 【r112 默认】回调里回放开机存下的那一整组寄存器, 读回比对为 0 才
 *       算模式恢复成功; 失败 ⇒ 置 pending, 由回落层的那次 4F02 收尾。
 *   0 = 回调里不做模式恢复(只做 DAC/光标/BUSY/再注册/SAVE_DRIVER_STATE),
 *       模式一律交给回落层 —— 与 r110 同效, 留作 A/B。               */
static WORD  g_runCbHwSet  = 1;
/* 【r112】CbBios —— 回调里是否允许"领预算发嵌套 4F02"(r111 的行为)。
 *   默认 0 = 不发。留这个开关的理由只有一个: r111 的失败是"相关性极强"而
 *   非"单点复现", 万一将来有人在别的机器上需要它, 一行 INI 就能开回来
 *   (VxdAllowBios 的预算机制原样保留)。开了它就是 r111 的行为。       */
/* 【r112】CbDuty —— 官方路径(回调里)做到第几项义务: 逐级放开的档位。
 *   【为什么要有这个旋钮】./111 里"回调里做官方恢复"这一档连着冻结了三台
 *   现场(t2/t3/t6), 而对照组(t5 不发 BIOS、t8 那次 4F02 被护栏拒)都活着。
 *   r112 把最可疑的那一枪(嵌套 4F02)换成了寄存器回放, 但"回放本身"以及
 *   "在回调里再调 VDD_DRIVER_REGISTER / SAVE_DRIVER_STATE"都还没有被单独
 *   验证过。于是留一个逐级放开的档位 —— 万一 r112 默认档仍冻结, 下一轮
 *   用 CbDuty=1(只回放 + 清 BUSY + 清屏)就能一刀切开: 是回放的问题,
 *   还是回调里调 VDD 的问题。
 *   1 = 回放 + 清 BUSY + 清屏;  2 = + 调色板 + 硬件光标;
 *   3 = 【默认】+ 再注册(VDD_DRIVER_REGISTER) + SAVE_DRIVER_STATE
 *       —— DDK §2.2 的六项义务全做。
 *   回落层不受它影响: 那里永远做全套。                              */
static WORD  g_runCbDuty   = 3;
/* 【r112】DdiTrace —— DDI 入口的"相位面包屑"。
 *   【为什么需要】./111 的 t1 是一种"一进 GUI 就花屏+冻结"的间歇故障(与
 *   r109 的 t3 逐字节同形, .TXT 与 .again 两份日志除时间戳外完全相同),
 *   日志停在 `MARK DIB_DibToDevice` 之后、下一个 DDI 的第一行之前 ——
 *   也就是说我们连"下一个 DDI 到底有没有被调用"都不知道。这个开关在
 *   光标三件套(每次鼠标移动都会走)的入口/出口各落一行相位标记, 于是
 *   下一次复现时: 有入口行没出口行 = 我们的代码里挂了; 连入口行都没有
 *   = GDI/VDD 侧停了(与我方无关)。默认 0(它会给每次光标调用加两行,
 *   只在专门抓这个故障时打开)。                                     */
static WORD  g_runDdiTrace = 0;
/* 【r111】HwFp —— 用 VGA 寄存器指纹判"硬件在不在我们的模式里"。
 *   见 drv.c 的 VxdHwFpRead 长注。
 *
 *   ★【r113 定案: 这个判据已被实测证伪, 默认关闭】
 *   r111 让它当"该不该动硬件"的【否决票】, r112 修好极性后它真的开始投票
 *   —— ./112 的三份日志把它判错的过程逐字记了下来:
 *     · t2 第二次窗口化 command: 屏幕已经花掉, 而 `HwFp: before rebuild`
 *       打出 `HwFp diff (0=same)=0000`、12 个字节 cur==base 全等
 *       (CRTC 00/01/12/13/0C/0D/14/17、SR01/SR04、GR05/GR06);
 *     · 于是每一次"会话开始/结束"的重建请求都被改判成
 *       `DeferredRestore: HwFp matches -> repaint only (no 4F02)`
 *       —— 从第一次成功重建之后【再也没有发过 4F02】, 花屏永久留下;
 *     · 反证: t8(HwFp=0)每一次边沿都真发 4F02(`CB evt=0400` 后紧跟
 *       `SetDisplayMode: vbeMode=0117`), 窗口化 command 反复运行无误
 *       ≈ r110 的读数。
 *   ⇒ "可读的 CRTC/时序器/GCR 全等"【不能】推出"硬件在我们的模式里":
 *     本机被切坏的是这 12 个字节【读不到】的那一半 —— Misc Output(3C2h,
 *     只写, 时钟选择)、属性控制器、以及 VM 侧 VGA 模式状态。这也正好
 *     解释了 t2 那对读数: 主机窗口是黑底彩框(被当文本/平面解释), 而
 *     虚拟机内截屏保存下来的 GUI 无瑕疵(内存内容是对的)。
 *
 *   于是 r113 的语义(全部 INI 一行可换, 便于继续 A/B):
 *     0 = 【新默认】完全不读寄存器, 也不看指纹 —— 边沿一到就按
 *         r110 的规矩办事(该验证验证, 该 4F02 就 4F02)。硬件足迹与
 *         r110 逐字节相同。
 *     1 = 读 + 落盘(纯仪器), 【不否决】—— 想继续收集"花屏时寄存器是不是
 *         真的没变"的读数时用它, 行为与 0 相同。
 *     2 = r111/r112 的否决行为(指纹全等就不发 4F02), 留作反向对照。
 *   为什么默认关掉"读"而不只是"不否决": 端口读要写索引寄存器, 而 r112
 *   新增的开机期寄存器采样与 t1 那 5 次开机崩溃同批出现 —— 官方契约里
 *   驱动不需要在开机相读这些寄存器, 少一个变量少一个嫌疑(见
 *   VxdHwRegSave 的注: 采样点已挪到"已知良好的重建之后")。            */
/* r111-marker: official-path-first in-callback restore + hardware fingerprint */
/* r113-marker: fingerprint veto retired (HwFp default 0 = advisory/instrument),
 * HwReg sample moved out of the boot path, DDI return breadcrumb
 * (TRACESLOTRET/DIBSTUB) added, CFG5 policy readout. */
static WORD  g_runHwFp      = 0;
/* 【r111】g_inCbRestore —— "此刻正在主 VDD 的回调里做官方恢复"。
 *   唯一用途: 让 SetDisplayMode 跳过 VDD_PRE_MODE_CHANGE(0x86) 那一次通知。
 *   理由: ResetHiResMode 本来就是主 VDD 自己发起的"把模式还回来"那一拍 ——
 *   它已经知道模式要变; 我们再从它的回调里回调一次它的 PRE_MODE_CHANGE,
 *   属于在别人的事务里再进一次别人的门。DDK 的六项义务里【没有】这一项,
 *   boxv9x / vmdisp9x 的 ResetHiResMode 也只重编程硬件。
 *   回落层(DDI 上下文)那条路照旧通知 —— 那才是需要通知的场合。
 *   【这是两条路径唯一的行为差异】刻意留成"已知且只此一处"。          */
static WORD  g_inCbRestore  = 0;
/* r111-marker: official-path-first in-callback restore + hardware fingerprint */

/* 【r105】AccessLog —— 把 deCursorExclude/deCursorUnexclude 的记账落盘。
 *   【为什么这一轮才做它】用户的实测把"陈旧块"归到了鼠标移动上, 内容是高彩
 *   陈旧像素, 与 VGA 无关。而 ddk/dibeng.h:7-8 把这两个槽直接定义成
 *   `#define deCursorExclude deBeginAccess` / `deCursorUnexclude deEndAccess`,
 *   同文件 152-153 行又给出 wFlags 的两个语义位:
 *       #define FB_ACCESS      0x0001
 *       #define CURSOREXCLUDE  0x0008
 *   ⇒ 引擎每一次"把光标从屏上摘下来 → 画 → 再贴回去"都会经过这两个回调,
 *   而"贴回去"的内容是它自己保存的那份屏幕位。**只要这一对调用失配
 *   (EndAccess 多于 BeginAccess, 或矩形/标志被喂错), 引擎就会把一份
 *   过期的屏幕位盖到刚画好的像素上 —— 表现正是"鼠标一动就冒出一小块
 *   陈旧的高彩内容"。** 这条路径 r80 起就在, 但从来没有被量过。
 *   【开销】只写内存(几个 word), 绝不在这里打日志 —— 这是被 DIBENG 在
 *   画屏中途调进来的回调, r100 的教训是"回调里做 DOS I/O = 冻结/蓝屏"。
 *   落盘交给 XclDump, 挂在 DDI 入口的安全上下文里。               */
static WORD  g_runAccessLog = 1;
/* 【r105】CursorSrc —— 重建光标时用的形状从哪来。
 *   【r100..r104 的老路(置 0)】SetCursor_driver 把【调用方的远指针】
 *   存进 lpLastCursor; 真正的拷贝发生在主 VDD 的回调里(SaveCursorShape),
 *   也就是【任意久之后】。而 GDI 传给 DDI 102 的那本 CURSORSHAPE 属于
 *   引擎/调用方的临时内存 —— 那时候它还在不在, 我们无法保证:
 *     · 指针悬空但地址仍可读 ⇒ 拷回来的是【别人的内存】, 头部凑巧过检时
 *       就会被当作光标形状话到屏上(一块高彩陈旧像素);
 *     · 头部不合理 ⇒ SaveCursorShape 静默 s_curShapeLen=0, 于是"重建光标"
 *       这条 DDK 义务其实从来没真正做到过(./103 四份日志里一条
 *       `CursorShape: saved len` 都没有 —— 正好印证)。
 *   两种结局都不对。置 1 时拷贝改到 SetCursor_driver 里当场做, 那时
 *   指针【一定】有效, 且头部校验通过后拷下来的就是真形状。
 *   【为何要留 0】它是 r100..r104 的逐字节行为, 用于 A/B 归因。          */
static WORD  g_runCursorSrc = 1;

/* 【r106】HwChk —— 要不要直接问硬件"你现在在什么模式", 以及用它当重建判据。
 *
 *   【要解决的问题】r104 的 ./104 证据把病灶钉到了"输出侧"(同一时刻虚拟
 *   机内截图完好、主机截图是一屏 16 色条纹), 也就是说: 显存里是桌面,
 *   而显示器是按另一种模式/调色板扫出来的。可是驱动侧【没有任何办法读出
 *   硬件真实状态】——
 *     · VerifyDisplayMode 的两条判据全是自证(VxD 软件状态 + 帧缓冲回读),
 *       所以 B2 那次会打出 "mode already correct, no 4F02 issued";
 *     · 4F03/4F06/4F07 的结果在 BX/CX/DX, 而 VBEEXECREQ 只回 AX
 *       (见 drv.c 的 r82 注), 要拿就得改 VxD 结构并让用户重编 minivdd.vxd。
 *   r106 找到了一条【不动 VxD】的路: INT 10h AX=0F00h(取当前显示模式)
 *   的结果就在 AX —— AL=0FFh 表示处于 VBE 模式, AL=03h/12h/13h... 表示
 *   硬件已被切进标准 VGA 通路(见 drv.c VBE_GetVideoMode 的长注)。
 *   这条判据把"硬件还在我们的高分辨率通路里"与"硬件已被切走"分开,
 *   而且它是【只读】的 —— DOS 会话进行中也能反复问, 不像嵌套 4F02 那样
 *   会把系统打死(r99 的教训)。
 *
 *   0 = 完全不问(r105 及以前的行为, 留给 A/B);
 *   1 = 【默认】只在三个安全点问: 会话收尾边沿 / 每次重建之前 / 重建之后
 *       (每次 DOS 会话最多 3 次 BIOS 往返, 风险与既有 4F02 同量级);
 *   2 = 上面全部, 外加"每次 MaybeDeferredRestore 的第 64 次"问一次 ——
 *       用来抓"硬件是在哪一刻被切走的"(窗口化 DOS 会话进行中也能抓到)。
 *       探到不一致不会立刻动硬件: 只置 bRestorePending, 真正那次 4F02
 *       仍旧走既有的"等 POST_VGA_TO_HIRES 安全信号"闸门, 因此重建总是
 *       落在会话收尾之后。
 *
 *   【r108 两条更正, 都来自 ./107 的 t4 日志】
 *   ① 语义: 这条探针读的是【主 VDD 的虚拟 BIOS】, 不是硬件 —— 见 HwProbe
 *      头部的 r108 注。"硬件被切走了"这个结论不能由它得出。
 *   ② 采样率: 那个"每 64 次"的计数器在 MaybeDeferredRestore 里, 而该函数
 *      全驱动只有 4 个调用点(SetPalette / SetCursor / MoveCursor /
 *      CheckCursor)。t4 整场实测【只探到一次】(第 1572 行, ax=8072 OK),
 *      所以 HwChk=2 ≒ "几乎没有采样"。这一轮的真正触发器已经换成 VhEdge
 *      那条事件边沿, HwChk 只作为补充; 想要真采样率的话, 下一轮把它挂到
 *      VxdTraceDump 的节拍上。                                        */
static WORD  g_runHwChk = 1;
/* 【r113】HwReg —— 整组 VGA 模式寄存器(CRTC 00-18 / SR01-04 / GR00-08)的
 *   采样开关, 也就是主 VDD 回调里那条"回放"的数据来源。
 *     0 = 从不采样 ⇒ 回调里的回放自动跳过(drv.c 的 VxdHwRegRestore 返回 -1
 *         "没有样本"), 六项义务里"把硬件设回 Windows 模式"那一步交给回落层
 *         的 4F02 收尾 —— 这是 r110 的形态。
 *     1 = 【新默认】每次【成功的重建之后】采一次样(此刻硬件确实在我们的
 *         模式里), 只落一行摘要;
 *     2 = 再额外落一份逐寄存器的完整转储(25+5+9 行, 只在专门收集读数时用)。
 *   【为什么从 Enable 尾部搬到这里】r112 把采样放在开机相(Enable 尾),
 *   而 ./112 的 t1 五次开机崩溃(0D @ 3A43:… / 394e:… / 冻结)正好与那批
 *   开机期端口访问同批出现。官方的 DDK 契约里, 驱动在开机相不需要读写
 *   VGA 寄存器; 挪到"一次成功重建之后"以后, 开机路径的端口足迹回到
 *   r110 的"零访问", 而样本反而更可信。详见 drv.c 的 VxdHwRegSave 注。 */
static WORD  g_runHwReg = 1;
/* 【r114】VgaFix —— 全屏 VGA 交接后"会话边界收尾寄存器组"的补写开关。
 *   r114-marker: VgaFix policy readout (mini-VDD 文本态修补 at hook 5).
 *   【r129 改默认值: 0 -> 1】它现在管【两个方向】的两组寄存器, 而我们手头
 *   已有的现场读数正好每一组都指向它:
 *     方向 A 高分辨率 → VGA(hook 5, 文本态修补): 文本取指/平面那 8 个寄存器
 *        (SR02/03/04、GCR05/06、CR14/17、AR10, 绝不碰时序)。./110 t3 的两张
 *        主机截图逐单元取证已经证明这一刻"三种取指全部塌到 plane 0"。
 *     方向 B VGA → 高分辨率(hook 7, HiResClockFix, r129 新增): 三个【模式
 *        状态位】bit0(SR01 = 8 点字符时钟、GCR06 = 图形模式、AR10 =
 *        图形模式)。./127 的 r1 现场读数: 会话结束、重建之前
 *        SR01=0x0003 / AR10=0x0000 / Misc 3CC=0x0067(会话前 0x002F) ——
 *        与 1024x768x16bpp 线性打包像素模式自相矛盾, 而主机截图正是
 *        "窗口被收缩、画面横向压扁"(虚拟机内截图完全正常)。
 *   【为什么两个方向合成一个开关】它们是同一件事的两半(把会话边界的硬件
 *   状态还回模式要求的值), 而 r114 那一半【从来没有被开过】—— ./115 的
 *   日志里 VBE_TR_ST_VGAFIX 位恒 0 就是证据: 它一直是个死开关。分开两个
 *   开关只会让"哪一半起了作用"更难判, 因为下面两条都保证正常路径零写入。
 *   【为什么现在敢默认开】两条路径都是"先读、不对才写", 且各有一个自证:
 *     · 方向 A 只在 CR17 bit0 非 0(硬件确在文本态)时才动手;
 *     · 方向 B 只在三个位读回来不足 3 个 1 时才动手, 写完再读回数一遍。
 *   正常路径上它们一个字节都不写 —— 所以"默认开"不会改变正常情况下的
 *   硬件足迹, 只在【已经坏了】的时刻才动手。
 *   【怎么关】system.ini 的 [omninine] VgaFix=0, 不必重编。
 *   r129-marker: VgaFix default 0 -> 1 (both directions of the session boundary).
 *   r114-marker: mixed deploy safe (new drv + old vxd; version stays 0113). */
static WORD  g_runVgaFix = 1;
/* 【r116】Dispi —— Bochs/VBox VBE_DISPI 端口(1CEh/1CFh)虚拟化开关。
 *   r116-marker: Dispi policy readout (mini-VDD 1CEh/1CFh virtualization).
 *     0 = 不装 I/O handler、不开全局陷阱 —— 与 r115 行为逐字节相同;
 *     1 = 【新默认】mini-VDD 装上 1CEh/1CFh 的 I/O handler, 并在 Windows
 *         拥有 CRTC 期间打开全局陷阱: 非 Windows VM(窗口化的 DOS 盒 →
 *         它的 BIOS 调 INT 10h 设模式时会写 1CEh/1CFh)对这两个端口的
 *         读写被【吃掉】, 真硬件 I/O 只在"调用者就是 CRTC 所有者"时放行
 *         (全屏 DOS 时 DOS VM 就是 CRTC 所有者 ⇒ 一切照旧)。
 *   【为什么默认 1, 与 VgaFix 默认 0 不同】这一条不是我们的发明, 而是
 *   参考实现的【唯一动作】: boxv9x 的 mini-VDD(vxd/boxvmini.asm)文件头
 *   原文 —— "it virtualizes accesses to the Bochs VGA registers on I/O
 *   ports 1CEh and 1CFh … This means that when you start an MS-DOS Prompt,
 *   the screen doesn't go wonky when the DOS VM, calling into the VGA ROM
 *   BIOS, ends up trying to perform I/O to those ports"; vmdisp9x 的
 *   virtual_0x1ce() 注释同义("fix of broken screen when open DOS window")。
 *   两个独立实现在同一类 VBE(我们的卡 VBE 3.0 / 16MB / LFB=0xDE000000,
 *   正是 Bochs/VBox VBE 画像)上修同一个症状, 且这正是 DDK 要求的形态
 *   (Install_IO_Handler + 在 ENABLE/DISABLE_TRAPS 里开关全局陷阱)。
 *   【怎么关】system.ini 的 [omninine] Dispi=0, 不必重编。
 *   r116-marker: mixed deploy safe (new drv + old vxd; version stays 0113). */
static WORD  g_runDispi = 1;
/* 【r117】DispiFix —— "官方路径不成功时的回落层": 边沿发现 DISPI 寄存器
 *   与本机当前模式不符时, 当场把 1CEh/1CFh 改回正确格式。
 *   r117-marker: DispiFix policy (in-driver VBE_DISPI repair).
 *     0 = 只读不写: 探针照常落盘(DISPI probe verdict=0), 但不动硬件;
 *     1 = 【新默认】读到的 xres/yres/bpp/virtw/ENABLED 与 wScreenX/
 *         wScreenY/wBpp/wScreenPitchBytes 不符时, 依次写回
 *         XRES/YRES/VIRT_WIDTH/BPP, 最后写 ENABLE=0xC1
 *         (ENABLED|LFB_ENABLED|NOCLEARMEM —— 不清屏, 桌面内容保留),
 *         再回读自证(DISPI fix result=1)。
 *   【为什么敢默认开】三条硬门槛叠在一起才会写:
 *     ① ID 寄存器必须是 0xB0C0..0xB0C7(Bochs 家族; 不是就永久静默,
 *        绝不对陌生硬件写端口) —— ./116 的 VBE 画像(sig=4556 / ver=0300 /
 *        16MB / LFB=0xDE000000)正是这一族;
 *     ② 必须先读出【不符】—— 正常状态下这个函数一个字节都不写;
 *     ③ 调用点只在"非全屏 VGA 会话"的安全 ring-3 上下文(状态边沿与
 *        重建之后), 与 PortProbeRead 同一处纪律。
 *   【与 Dispi(陷阱)的关系】陷阱是"不让别人改", 修复是"改回来";
 *   两者独立记账(TRC st dispi inst/acts 与 DISPI probe/fix 行),
 *   于是下一轮可以分开判"谁起了作用"。
 *   【怎么关】system.ini 的 [omninine] DispiFix=0, 不必重编。           */
static WORD  g_runDispiFix = 1;
/* 【r119】DispiIdProbe —— 是否允许"写索引 0 再回读"确认 DISPI 是否存在。
 *   r119-marker: DispiIdProbe policy (VBE_DISPI ID write-verify ladder).
 *   【为什么必须单独一个开关】r117/r118 用的是"只读索引 0, 读回值在
 *   0xB0C0..0xB0C7 之外就永久静默"。那套判据是错的 —— DISPI 的索引 0
 *   是【只写】的, 不先写就永远读不到东西(上游源码原文见 drv.c 的
 *   DispiIdProbe 头注)。本轮把确认手法换成业界通用的"写—回读—还原",
 *   代价是本驱动第一次【主动往 1CEh/1CFh 的数据端口写一个值】。
 *   写的东西只有索引 0(ID), 它不参与 XRES/YRES/BPP/ENABLE 任何一个决定
 *   扫描格式的字段, 而且写完立刻把原值放回去。
 *     1 = 【新默认】允许这一枪。没有它, "永远问不出 DISPI 到底在不在"
 *         这个状态就会重现, 整轮又变成零信息。
 *     0 = 一个字节都不写: 探针保持 r117/r118 的只读形态(此时日志里会有
 *         一行 `DISPIID write-probe disabled by policy`), 用于排查
 *         "是不是这一枪本身把机器弄坏了"。
 *   【怎么关】system.ini 的 [omninine] DispiIdProbe=0, 不必重编。
 *   【为什么不是 static】drv.c 的 DispiPortProbe 要读它(那一侧只声明
 *   extern), 内部链接的 static 会让链接器报 _g_runDispiIdProbe 未定义。 */
uint16_t g_runDispiIdProbe = 1;
/* 【r120】DispiPhys —— 是否允许 ring 0 的物理 I/O 读 VBE_DISPI(1CEh/1CFh)。
 *   r128-marker- DispiPhys policy (ring 0 physical I/O DISPI readout).
 *   【为什么默认 0】这是本项目第一次从 ring 0【主动对硬件发 I/O】(不是
 *   放行别人的 I/O, 而是自己发起)。./119 已经证明了 ring 3 那条路读不到
 *   东西, 但"读不到"有两种解释(端口不存在 / 我们的 I/O 没抵达硬件), 而
 *   分开它们只有这一条路。它的动作很窄: 索引 0 写一个 ID、回读、把原值
 *   写回, 再读 5 个寄存器; 全是只读语义(唯一那次写落在不参与任何扫描
 *   格式的 ID 寄存器上)。
 *   【为什么要用户显式打开】本项目 120 轮里, 能改变硬件足迹的新动作一律
 *   默认关, 由需要那条信息的那一轮单独打开 —— 这样"这一轮到底做了什么"
 *   永远可以从一行 CFG 读出来, 也不会让日常开机多背一个未知。
 *     1 = 允许: 日志里会出现 `DPHYS ...` 十行 + 一行结论
 *         `DPHYS verdict: port ALIVE ...` 或 `... no DISPI port`。
 *     0 = 【默认】一次物理 I/O 都不发, 与 r119 完全同形。
 *   【怎么开】system.ini 的 [omninine] DispiPhys=1, 不必重编。           */
uint16_t g_runDispiPhys = 0;
/* 【r122, r126 复评】SetModeText —— 重设高分辨率之前, 先走一趟【标准 VGA
 *   文本模式】, 即把"用户在花屏下敲 mode co80"这一枪原样搬进恢复路径。
 *   r128-marker- VGA-text round trip before the hires re-set.
 *   【r122 的动机】./121 的日志把"官方路径确实跑了、而且全部成功"钉死了:
 *   五次 4F02 全部 out=004F, 寄存器重放 diff=0000, VDD_DRIVER_REGISTER 重登记
 *   成功 —— 而主机画面依旧花。同时日志里有一处失配:
 *       SetDisplayMode: vbeMode=0117        (我们刚设的)
 *       VxDexec: in=0F00 -> mode al=0086    (主 VDD 的虚拟 BIOS 随后回答的)
 *   即硬件被设成了 VBE 0117, 而主 VDD 的记账还停在 al=0x86/AH=0x50("文本"),
 *   于是它按"现在是文本模式"去虚拟化/恢复, 我们按"现在是高彩"去绘制。
 *   而 ./116 result.txt 第 8 条说: 全屏花屏下输入 `mode co80` 当场恢复正常。
 *
 *   【r122 的实测 —— 这一枪【不响】, 不要再把它当解法】
 *   ./122 的 debug 构建 + SetModeText=1: 开关确实生效(`run SetModeText=0001`、
 *   `CFG9 ... 0001`、五条 `SetDisplayMode: SetModeText -> BIOS_SetTextMode`),
 *   而用户报告"仍然和默认下 r116 几乎没有区别"。同样一条 INT 10h AX=0003,
 *   在 DOS 程序里敲就修得好, 在驱动里发就修不好 —— 说明【机制不在"设模式"上】,
 *   而在"谁在哪个时刻发"上。r122 预登记的判据("屏幕恢复 ⇒ 记账说成立")因此
 *   判为不成立。
 *   【所以 r126 保留它、默认关、不再加码】它已经完成了取证任务; 留着是为了
 *   以后有人想再验一次时不必重写。真正的下一步见 CrtProbeNow 的历史长注(该函数已于 r407 删除):
 *   先去读"屏幕正花着那一刻的 CRTC"。
 *     1 = 在 VBE_SetMode 之前插入 INT 10h AX=0003(仅推迟重建上下文,
 *         即 g_inCbRestore == 0; 回调里绝不做, 那是 r99 三次死机的形状)。
 *     0 = 【默认】与 r122 的默认档完全同形(即 r121 行为)。
 *   【怎么开】system.ini 的 [omninine] SetModeText=1, 不必重编。          */
uint16_t g_runSetModeText = 0;

/* 【r130】CrtGeo —— "发现扫描几何与帧缓冲不一致时, 强制主 VDD 重新走一遍
 *   完整的模式设置"这一步的开关。
 *
 * 【r130 为什么认定病灶在这里】
 *   r129 的三条硬证据(全部可复算, 不是推断):
 *     ① 主机截图 1_host.png 是 320x771, 虚拟机内 1_guest.png 是 1024x768;
 *        两张图里"青底的行"【都是 740 行、起止行号完全对齐】(0..739);
 *        每行主色一个是 (0,125,123)、一个是 (0,127,127) —— 同一个颜色。
 *        ⇒ 纵向 1:1 正确, 横向只剩约 320 列。也就是说桌面【内容】是
 *          1024 宽的(虚拟机内截屏就是帧缓冲本身), 而【扫描输出】一行
 *          只走了约 320 个像素。这是"扫描几何"问题, 不是绘制问题。
 *     ② 同一份 r129 日志: `HwReg sample diff now=0000`、`SR01 bit0=1`、
 *        `atc10 bit0=1`、`gcr06 bit0=1` —— r129 新补的三个模式状态位
 *        【全都已经是对的】, 屏幕却照旧收缩 ⇒ r129 的机制被实测证伪。
 *     ③ 标准 VGA 上, 一行扫描出多少像素【只由 CRTC 01h 决定】(再乘每字符
 *        点数), 而 00h/01h 在 r129 之前的所有日志里【从来没有被读过】。
 *
 * 【修法为什么是"强制重新走一遍模式设置"而不是"自己写几何寄存器"】
 *   · 自己写 CRTC 时序 = 把某张卡/某个 BIOS 的具体时序值焊进通用驱动,
 *     正是通用性铁律禁止的那一类做法; 而且时序字段彼此耦合(CR00..CR06 +
 *     CR17 的溢出位), 写一半比不写更危险。
 *   · 让 BIOS 重新执行一次模式设置 = 走【标准 VBE 路径】, 由真正知道该模式
 *     时序的那一方去写全部寄存器。这正是"官方恢复路径"的定义。
 *   · 主 VDD 之前那次 4F02 之所以没修好, 最符合读数的解释是: BIOS 的模式
 *     缓存【以为已经是这个模式了】, 于是把重编程整个跳过。所以这里先切到
 *     另一个合法模式再切回来 —— 【强制】它在回程真的写一遍寄存器。
 *     两个动作都是 4F02h, 都在系统 VM 的 ring 3 上下文里发, 不新增任何
 *     端口写路径, 也不碰任何厂商私有寄存器。
 *
 * 【触发条件极严格】只有 CrtGeometryLooksRight() 明确返回 0(算出来的
 *   一行像素数 ≠ 桌面宽度)才动手。返回 1(自洽)或 2(读不到)时【一个字节
 *   都不碰】—— 所以正常机器上这一枪永远不响。
 *   1 = 【默认】开: 不一致时强制重新走一遍模式设置;
 *   0 = 只读数、只落盘, 不重设(留给 A/B 对照)。
 *   【怎么关】system.ini 的 [omninine] CrtGeo=0, 不必重编。
 *   r130-marker: CrtGeo default 1 (forced full mode re-init when the
 *                scan-line pixel count disagrees with the desktop width). */
uint16_t g_runCrtGeo = 1;
/* 【r131】CrtEn —— CRTC 17h bit7(SE, Sync Enable)的"先读、不对才写"开关。
 *   r131-marker: CrtEn policy (CR17h Sync Enable repair).
 *   【它管什么】见 CrtSyncEnsure 的长注: ./130 的主机截图量化出"整幅桌面被
 *   压进 320 列、可见帧缓冲列 ≈ 127 ≈ CR01+1", 而同一轮日志里 CR00/CR01/
 *   SR01/GCR06/AR10 全部正确 —— 唯一没被任何代码看过的一位就是 CR17h bit7。
 *     · SE = 1: 标准 VGA 对一个"该有画面"的模式的要求(FreeVGA Index 17h);
 *     · SE = 0: 行场同步被强制到无效电平 —— 在"以 VGA CRTC 为准"的实现里
 *               行宽会退化成 CR01+1 个像素。
 *   【为什么敢默认开】三道闸门叠在一起才会写: ① 本开关;
 *   ② 不在主 VDD 回调里; ③ **读回 SE = 0 才写**。正常机器上零写入,
 *   所以它不改变任何正常路径的硬件足迹; 只有"已经坏了"的时刻才动一个 bit
 *   (读-改-写, 另外 7 位原样保留, 时序/几何字段一个都不碰)。
 *   【怎么关】system.ini 的 [omninine] CrtEn=0, 不必重编。               */
uint16_t g_runCrtEn = 1;

/* [r132] ProtClr -- clear the CRTC Protect bit (CR11h bit7) in front of every
 *   mode set.  See drv.c VxdCrtProtectClear for the FreeVGA quote and for why
 *   this is a general VGA rule.  Default 1 = on; system.ini
 *   [omninine] ProtClr=0 turns it off without a rebuild (A/B control).
 *   [r132] SnapSess -- take a bounded register snapshot from the cursor tick,
 *   so that we finally have the CRTC *while the DOS box is on screen*, not only
 *   after it ended.  Default 1 = on; SnapSess=0 off.                        */
static WORD  g_runProtClr   = 0;    /* [r133] default back to off */
static WORD  g_runSnapSess  = 1;
/* [r133] Relatch -- re-program the whole mode with the display gate closed and
 *   opened LAST, then measure the display-enable duty cycle on 3DAh.  See
 *   drv.c VxdCrtRelatch / VxdRastProbe.  Default 1 = on; Relatch=0 off. */
static WORD  g_runRelatch   = 1;
/* [r134] HwEss -- which verdict decides whether the OFFICIAL (VDD callback)
 *   restore path succeeded.  See the long note in ApplyDesktopRestore:
 *   1 = [default] judge by MEANING (geometry / SE / 8-dot / screen-on /
 *       graphics / protect) -- the byte count stays as evidence only;
 *   0 = judge by the byte count again (the r133 behaviour, kept as a
 *       one-line A/B that needs no rebuild).
 *   system.ini [omninine] HwEss=0 selects the old verdict.                   */
static WORD  g_runHwEss      = 1;
/* [r134] VhWin -- may the POST_VGA_TO_HIRES edge bypass the suppress gate?
 *   1 = [default] yes (the edge is our own mini-VDD's direct evidence that the
 *       VGA->hires round trip finished, and ./133 line 4645 shows the gate
 *       stopping that very rebuild);
 *   0 = the r111..r133 behaviour (the edge still has to pass the gate), kept
 *       as a one-line A/B.  system.ini [omninine] VhWin=0.                     */
static WORD  g_runVhWin      = 1;
/* [r135] SessNoSet -- the session-END edge must never set a mode.
 *   1 = [default] at that edge do only VDD_SAVE_DRIVER_STATE + clear +
 *       repaint; if the hardware really needs a mode set, defer it until
 *       AFTER the main VDD's switch transaction (POST_VGA_TO_HIRES edge, or
 *       the direction-verified DDI-count net).  The edge sample lands INSIDE
 *       that transaction (./133 ring: `CB evt=0D01` = our own "one BIOS call
 *       at a time" latch tripped by the callback cutting into our rebuild),
 *       and ./134 shows the "repaint only" path giving a clean desktop.
 *   0 = the r104..r134 behaviour (force a full 4F02 at that edge).
 *   system.ini [omninine] SessNoSet=0.                                        */
static WORD  g_runSessNoSet  = 1;
/* [r135] EdgeDir -- decide the entry-side gate by DIRECTION instead of by the
 *   mini-VDD's transient VGA bit.
 *   1 = [default] direction first: 2 (the DOS box just took the screen) is
 *       suppressed outright, 1 (we just came back) is allowed outright, and
 *       only "unknown" falls back to the r134 bit test;
 *   0 = pure r134 behaviour (bit0 + text-session probe), kept as the A/B.
 *   Why: ./133 sampled that bit as 0x53 and ./134 as 0x52 at two ENTR edges
 *   that are otherwise identical (both `i10 n=0004 / i10 ax=0003`), so the
 *   "fix" in ./134 was a race won, not a decision made.
 *   system.ini [omninine] EdgeDir=0.                                          */
static WORD  g_runEdgeDir    = 1;
/* [r135] EntryIn -- when the direction says "the DOS box just took the
 *   screen", may the entry-side heal fire anyway (i.e. exactly what ./134
 *   did, and what left the desktop painted in the LFB)?
 *   0 = [default] no: a 4F02 at that moment programmes our hi-res registers
 *       while the box owns the screen (./134 line 2111 set mode 0x117 two
 *       tick after the box's mode-3 request, and every SNAP taken afterwards
 *       reads CR17 SE=1 / GCR06 bit0=1 / ATC10=0x41, i.e. graphics, not the
 *       text mode the box is drawing into);
 *   1 = the r134 behaviour, kept as the one-line A/B that restores it.
 *   system.ini [omninine] EntryIn=1.                                          */
static WORD  g_runEntryIn    = 0;
/* 【r136】RingDump —— 每次进入 MaybeDeferredRestore(公认安全的 DDI 上下文)
 *   就把回调期的内存环落盘一次, 而不是等到"真去重建"那一刻。
 * 【为什么这是本轮的第一优先项】./135 的账: 从开机到"用户按下 Alt+Enter
 *   进全屏"共发生 34 条环事件, 而环只有 32 格, 且落盘点只有"执行重建"
 *   这一个 —— 于是那一轮最关键的 2 条(ENTR 边沿的方向记录与 VESTART 决定)
 *   恰好落在被挤掉/未落盘的那一段。没有它们, "进入方向判对了没有"这个
 *   问题在 ./135 里【根本无法判定】, 只能靠推断 —— 而本项目已经因为推断
 *   吃过两次亏(r132 的保护位前提、r134 的瞬时位判据)。
 *   RingDump=0 退回 r135 行为(只在重建时落盘)。                     */
static WORD  g_runRingDump    = 1;
/* 【r137】EntryNoSet —— 进入 DOS 会话那一条边沿上【不再设模式】。
 *   ./136 的日志把 r136 的设计漏洞钉死了: 进入边沿那一拍
 *       ENTR: dir ...=0002      (盒子刚拿走屏幕 —— 方向这次答【对】了)
 *       entry need rebuild=0001
 *       entry edge -> force desktop rebuild (hardware not ours)
 *       gate passed, state bits=005A / gate dir=0001
 *       entry heal -> issue now (no POST_VGA_TO_HIRES expected)
 *       SetDisplayMode: vbeMode=0117  ->  VBE_SetMode: exec ax=004F
 *   也就是说: r136 用来"只在盒子拿屏时推迟"的那条判据取的是 mini-VDD 状态位
 *   bit0(VBE_TR_ST_VGA), 而【窗口化的 DOS 盒从来不会置那一位】—— 它是
 *   func 4/5(PRE/POST_HIRES_TO_VGA)才置的, 那两个只在【全屏】交接时发生。
 *   于是这一位在"窗口化 command"这个用户实际报告的场景里【恒为 0】, r136
 *   的核心改动在这个场景下等于不存在, 走的还是 r135 的老路: 在盒子起来的
 *   那一拍把卡设回 0x117。
 *   ⇒ r137 不再问"谁拿着屏幕"这个问题(它答错过两次, 而且窗口化场景下根本
 *     没有正确答案), 改成: 这条边沿【一律不设模式】, 只做"把已知良好的模式
 *     寄存器放回去 + 清屏 + 请 USER 重绘"; 真正的 4F02 仍然由 DDI 计数兜底
 *     (wPostWait)和会话收尾/回 Windows 那条边沿承担。判据仍然在, 但它现在
 *     只决定【哪一次】做, 不决定【做不做】。
 *   EntryNoSet=0 退回 r135/r136 的行为(单行 A/B, 不用重编)。 */
static WORD  g_runEntryNoSet = 1;
/* 【r137】EntryReplay —— 把"寄存器回放"这一半也交给回落层那条路。
 *   原来(how=0)回落层只做清屏+重绘, 回放只在官方回调里做; 而 ./136 里
 *   屏幕变花的主要那次变化发生在【官方回调根本没有覆盖到】的时机上
 *   (AR10h 0041h -> 000Dh, 之后一直没回来)。r137 起 apply_tail 也走
 *   ApplyDesktopRestore(1,...) —— 与官方回调【同一份实现】。 */
static WORD  g_runEntryReplay = 1;
/* 【r138】SampleAtBoot —— 开机相(PhysicalEnable 尾部)就采一份"已知良好模式
 *   寄存器"样本。默认 1。
 *   【为什么必须有 —— ./137 的死锁】r137 的 EntryNoSet=1 取消了会话入口那次
 *   4F02, 而样本的生产者 HwRegSampleAtRebuild() 恰恰挂在"一次成功 4F02 重建
 *   之后"。结果是 ./137 全份日志里 `have sample=0000` 9 次、`diff=EEEE` 4 次、
 *   `HwReg: sample ok=0001` 0 次 —— r128 回放 / r133 重新锁存 / r134 语义判据 /
 *   r137 AR 采样回放 四条路同时变成死代码。【一个动作被取消, 而另一个动作
 *   挂在它后面】—— 这就是 r137 与 r136 行为几乎相同的全部原因。
 *   开机相是驱动里最确定的一刻(刚刚 SetDisplayMode(1) + POST_MODE_CHANGE),
 *   而且采样是【纯读】(idxread + AtcReadIdx, 一个字节都不写), 所以它不引入
 *   任何新的写足迹。SampleAtBoot=0 退回 r137 行为(单行 A/B, 不用重编)。 */
static WORD  g_runSampleAtBoot = 1;
/* 【r138】CbNoSample —— 官方路径(主 VDD 的 ResetHiResMode 回调)在"没有可用
 *   样本"时该怎么判。
 *   【DDK 的原文义务】"The display driver runs through all the code necessary
 *   to reestablish Windows hi-res mode on the hardware."(Win95 DDK,
 *   PRE_HIRES_TO_VGA / POST_HIRES_TO_VGA, 切回 Windows 的第 5 步)
 *   【r137 的实际行为】ApplyDesktopRestore(how=1) 里:
 *       d = VxdHwRegRestore();   // 无样本 ⇒ -1, 一个字节都不写
 *       if (d >= 0 && !es) ok = 0;   // r134 的判据【只在 d>=0 时生效】
 *   ⇒ 无样本 ⇒ ok 恒为 1 ⇒ 回调报"六项义务做完" ⇒ bCbRepaint=1 且
 *     bRestorePending=0 ⇒ 【回落层那一次完整重建也不会跑】。
 *   于是那一次切换里, 关于模式恢复这件事【什么都没做】, 却被记成成功。
 *   这正是 ./137 里 `CbRestore: inline ok=0002 / inline failed=0000` 与
 *   "屏幕依旧花"能同时成立的唯一解释。
 *   ⇒ 默认 1: 没有可用样本 = 模式没有被恢复 = ok = 0 ⇒ 按既有 r111 机制交给
 *     回落层(bRestorePending=1, bForceRebuild=1)。这就是用户要求的
 *     "官方路径优先, 官方路径做不到时由兼容/回落层承担"。
 *   CbNoSample=0 退回 r137 行为(单行 A/B)。 */
static WORD  g_runCbNoSample = 1;
/* 【r138】HwTruth —— 用 VBE 4F03h(硬件【当前】模式号)参与"模式对不对"的判据。
 *   【为什么这是本轮的核心修正】r109 起 VerifyDisplayMode 的两条判据是:
 *     ① FetchStateFromMiniVDD 的 currentMode —— 那是 mini-VDD 记下的
 *        【我们自己设过的】模式号, 是一面镜子;
 *     ② FbSelfTest —— 往帧缓冲写一个模式再读回来, 检验的是【内存】,
 *        与"扫描输出按什么格式解出来"无关。
 *   两条都看不见"硬件其实在 8bpp 模式下扫一块 16bpp 显存"这件事。
 *   ./137 的日志把后果写得很清楚: 每一个会话入口边沿都是
 *     `Verify: vxd mode=0117` → `DeferredRestore: pre-verify ok=0001` →
 *     `DeferredRestore: mode already correct, no 4F02 issued`
 *   —— 唯一能修好画面的那个动作(4F02)被一面【照自己的镜子】挡掉了。
 *   4F03h 不问我们自己, 它问 BIOS"现在是什么模式"。
 *   HwTruth=0 退回 r137 判据(单行 A/B)。 */
static WORD  g_runHwTruth = 1;
/* 【r138】AltFromTbl —— ForceModeReprogram 的中间模式从【本卡自己的模式表】
 *   里挑, 而不是硬编码 VBE 标准号。
 *   【为什么要改】r130 那张候选表写 `{ 0x0112, 0x0103, 0x0111 }`, 注释说
 *   "0112h = 640x480x8"。按 VESA 固定模式表, 0112h 是 640x480x【24bpp】,
 *   8bpp 的 640x480 是 0101h —— 也就是说注释与事实不符, 而 r130..r137 每次
 *   "逼 BIOS 重编程"都会先把卡切到 24bpp 再切回来: 那是一次【比需要的大
 *   得多】的像素格式变动, 而"返回目标模式"那一步一旦被打断, 卡就停在
 *   一个没有 CHECK 看得见的格式上。
 *   ⇒ 换成: 从 g_modes[](每项都有 width/height/bpp)里挑一个【位深与目标
 *     相同、分辨率不同】的模式; 没有就挑任意 8bpp 模式; 再没有才用兜底。
 *     这样挑出来的模式一定是【这张卡自己在 4F00/4F01 里报出来的】, 不需要
 *     假定任何标准编号, 也不会把卡切到与目标不同的位深家族里去 —— 符合
 *     本项目"通用性优先、不把某张卡的细节焊进驱动"的铁律。
 *   AltFromTbl=0 用 r130 的硬编码表(单行 A/B)。 */
static WORD  g_runAltFromTbl = 1;
/* 【r138】HwChkPitch —— 把 4F06h 的读数(每扫描行字节/像素)也打进日志。
 *   只作证据, 绝不单独触发重建(4F06h 有设置形态, 见 drv.h 的安全边界)。*/
static WORD  g_runHwChkPitch = 1;
/* 【r138】CrtGeometryEnsure 的有界重挂预算 —— 见该函数里失败分支的长注。
 *   只在"几何确实不对"(CrtGeometryLooksRight() == 0) 时消耗, 几何一好就
 *   清零。6 是写死的上限: 本项目吃过无界重挂的亏(drv.c 里 ./111 t5 的
 *   "空转重挂 8 次")。 */
#define CRT_GEO_REARM_MAX   6
static WORD  s_crtGeoReArm = 0;

/*==========================================================================
 * 【r139】四个开关 —— 每一个都对应 ./138 日志里一条【可读的失效】。
 *
 * 【本轮的前提事实: ./138 的量测把病灶钉成了数】
 *   主机截图 1_host.png 逐像素量出(桌面底色与任务栏两处互证, 可复算):
 *     · 桌面底色 (0,127,127) ⇒ RGB565 03EFh, 字节 EF,03 ⇒ 渲染成"黑 黑 青 青"
 *     · 任务栏底色 (191,191,191) ⇒ BDF7h, 字节 F7,BD ⇒ 整条渲染成黑
 *     · 那个青 = VGA 16 色 EGA 调色板的第 3 号; 那个黑 = 索引 ≥ 16 的项
 *   ⇒ 硬件在按 8bpp 扫描我们的 16bpp 显存: 每字节横向复制成 2 像素, 并用
 *     一张只有前 16 项的调色板。两条独立来源都指向同一条机制:
 *       ① 微软 DDK(PRE_HIRES_TO_VGA 页, 第 5 步): 全屏 → Windows 时, 显示
 *          驱动的 ResetHiResMode "runs through all the code necessary to
 *          reestablish Windows hi-res mode on the hardware.  It also restores
 *          the palette DAC ..."; ResetHiResMode 页第一条义务原文是
 *          "It must set the display hardware back into the Windows running mode
 *          that it was in before running the full-screen MS-DOS session."
 *       ② 决定"1 字节→2 像素"的那个开关来自属性控制器 10h 的 bit6 —— 而我们
 *          自己的样本读到的正是 AR10h = 0041h(bit6=1)。也就是说: 一旦芯片被
 *          按"VGA 兼容位移出"方式解释(文本模式留下的状态), 它就会按 8bpp
 *          lowres 扫描 —— 而这【不是 VGA 寄存器能表达的】, 所以整组模式寄存器
 *          可以逐字节正确而屏幕仍然是花的。
 *   ⇒ 结论(本轮的立足点): 唯一能把它改回来的动作是【一次真正的模式建立】,
 *     而 ./138 在整整 4 分钟里只发过 1 次 4F02h(开机那次, 日志 525 行)。
 *     下面四个开关就是"为什么一次都没发"的四处修正。
 *========================================================================*/

/* 【r139】BlindNeed —— 让两条"整组寄存器看不见"的见证参与判据。
 *   ./138 的 3186..3188 三行是本次回归的【直接证据】(可见行, 非推断):
 *       DeferredRestore: sessend hwreg have sample=0001
 *       DeferredRestore: sessend hwreg essentials (1=ours)=0001
 *       DeferredRestore: sessend need rebuild=0000
 *   ⇒ need=0 ⇒ 进入边沿走"repaint only"那条【什么都不挂】的分支
 *     (bRestorePending=bForceRebuild=wArmedEntry=wArmedEnd=0) ⇒ 之后任何一个
 *     DDI 入口都没有事可做 ⇒ 整份日志里 Verify:/CrtGeo: 各 0 行、4F02 只有
 *     开机那一次。而 r137 之所以还有救, 恰恰是因为它【没有样本】
 *     (have=0 ⇒ need 恒为 1) 而把机器一直挂着。
 *   见证落在 drv.c 的 VxdHwBlindNeed(): 4F03h 的当前模式号 + DAC 印章。
 *   为什么必须是它们: 上面量到的两半(像素格式 / RAMDAC)都不在 VGA 寄存器里,
 *   我们读得再全也看不见 —— 判据必须由【别人(BIOS)的记账】与【硬件里的物证
 *   (印章)】来补。 */
static WORD  g_runBlindNeed = 1;

/* 【r139】EntryAlways —— 入口(会话开始)边沿永远挂上一次延迟恢复, 绝不"什么都不挂";
 *   且当见证说"硬件不是我们的"时, 这一次恢复里【包含真正的模式建立】。
 *   为什么敢在入口边沿就重建: DDK 的 ResetHiResMode 页把"把显示硬件设回
 *   Windows 运行模式"写成第一条义务, 而 PRE_HIRES_TO_VGA 页第 3/4 步说明
 *   这一拍落在主 VDD 的切屏事务【之后】; 我们把它放在【下一个普通 ring 3 DDI
 *   入口】执行(就是 MaybeDeferredRestore 的调用点), 不在事务内部 —— r99 的
 *   三次死机正是"在事务内部嵌套 BIOS", 那条硬护栏(BeExecBios 在
 *   s_inVddCallback 期间一律拒绝)原封不动。
 *   EntryAlways=0 回到 r137 的"只回放寄存器 + 重绘, 绝不设模式"。 */
static WORD  g_runEntryAlways = 1;

/* 【r139】VhAlways —— POST_VGA_TO_HIRES(7 号)边沿永不被方向判据否决。
 *   【文档依据, 不是推断】DDK 的 PRE_HIRES_TO_VGA 页把这一次切换列成 6 步:
 *     第 4 步 PRE_VGA_TO_HIRES → 第 5 步【显示驱动的 ResetHiResMode】→
 *     第 6 步【POST_VGA_TO_HIRES】(最后一步)。
 *   也就是说: 7 号被调用时, "回到 Windows hi-res 模式"这件事【按定义已经
 *   完成】。用一条"方向推断"(s_edgeDirNow, 由上一次 TRAPS 边沿定格下来的
 *   陈旧值)去否决一个来自主 VDD 的直接事件, 是拿推断盖事实 —— ./137 与
 *   ./138 的环里各有一条 `CB evt=1C01`(VH_SKIP)就是这个否决留下的物证。
 *   VhAlways=0 回到 r135..r138 的"dir==2 就跳过"。
 *   注意这条与 HealGate 的关系: VhEdge 那条 pending 本来就带 wArmedEnd=1,
 *   不受抑制闸门管辖(见 r134 的长注), 所以这里去掉否决之后它一定执行。 */
static WORD  g_runVhAlways = 1;

/* 【r139】DacStamp —— 每次成功建立模式之后往 DAC 前 8 项盖一次章。
 *   印章是把"我们确实在这台硬件上做过一次模式建立"变成【硬件里的物证】;
 *   下一次从 DOS 盒回来读它就够了, 不需要推断谁动过什么。
 *   只在 bpp != 8 时盖章: 8bpp 下 DAC 就是画面本身, 一个字节都不许碰。 */
static WORD  g_runDacStamp = 1;
/* r139-marker: BlindNeed / EntryAlways / VhAlways / DacStamp */

/*==========================================================================
 * 【r140】OwnGate / ForceEdges —— 本轮的两个决定, 都是 ./139 的量测逼出来的。
 *
 * 【一、为什么需要 ForceEdges —— r139 的“判据”其实是一条永远为真的假判据】
 *   r139 让 need 由 VxdHwBlindNeed() 决定, 而它的第一条见证是
 *   “4F03h 的当前模式号 != 我们期望的模式号”。./139 的日志把它量穿了:
 *   同一份日志里 4F03h 被问了【19 次】, 答案【每一次】都是 0072h
 *   (2173/2301/2309/2438/2446/3276/3284/3532/3540/3738/3773/3781/3909/3917/
 *    4357/4393/4401/4529/4537; 其中 8 次走的是 `Verify: hw mode` 那一行),
 *   而我们期望的是 0117h —— 于是这条见证 100% 触发, need 恒为 1。
 *   DDK 的 CHECK_HIRES_MODE(35) 页原文解释了 0072h 是什么:
 *     "you should use the value passed in AL rather than reading 0:449H
 *      yourself since Far East Windows changes the data passed in AL to
 *      compensate for Japanese text modes 72H and 73H.  If the Main VDD sees
 *      that Japanese Windows 95 is running, it passes these modes as modes
 *      02H and 03H instead of 72H and 73H."
 *   也就是说 0072h 是【远东版 Windows 的文本模式号】, 是主 VDD 报出来的
 *   “DOS 看得见的模式”, 与“我们的桌面在不在”毫无关系。r137/r138 的日志里
 *   它同样是 0072h(见 R140_DELIVERY.md §2 的对照表)。
 *
 *   ⇒ 结论(必须写下来, 否则下一个人会“修好”这条判据并把修复一起关掉):
 *     r139 让画面回来的【真正机制】不是这条见证, 而是“need 恒为 1 ⇒ 入口
 *     边沿无条件重建桌面模式”。r134/r135(画面方向正常的两轮)日志里各有
 *     【1 次】入口边沿 4F02, r138(方向彻底坏的一轮)是【0 次】, r139 是 1+3 次。
 *     所以本轮把这件事从“判据的副作用”改成【明文规则】:
 *       ForceEdges=1(默认) ⇒ 在会话边沿一律 eneed/need=1, 重建不依赖任何判据。
 *     【不许再把它改回“由判据决定”】—— 判据可以决定【怎么修】, 不能决定
 *     【修不修】; 而“这条判据永远不会说'不用修'”这件事本身就是它能起作用的原因。
 *
 * 【二、为什么需要 OwnGate —— DDK 明文: 不是我们的屏, 一根线都不许动】
 *   VDD_SET_USER_FLAGS 页原文:
 *     "The BUSY flag ... is automatically set by the Main VDD whenever the
 *      screen group is switched away from the Windows hi-res screen to a
 *      full-screen MS-DOS session.  When this flag is set, the display driver
 *      must not draw on the display surface since the video focus is on a
 *      full-screen MS-DOS session and doing so would corrupt the appearance of
 *      the MS-DOS session.  This protocol is necessary since Windows may call
 *      display driver functions such as SetCursor, MoveCursor, BitBlt,
 *      ExtTextOut, etc. even after the video focus is set onto a full-screen
 *      MS-DOS session."
 *   两句话我们两条都占: ① 我们的 DDI 在全屏期间确实一直在被调用 —— ./139 的
 *   日志里有 47 条 `MARK HB` 心跳, 分布在整份日志上(含全屏时段);
 *   ② "corrupt the appearance of the MS-DOS session" 就是用户从 r134 到
 *   r139 一直在报的那件事(全屏花屏, 只能用 mode co80 修)。
 *   于是本轮加一道门: 动手之前先问【硬件端口答不答话】——
 *     drv.c 的 VxdVgaOwner(): 读 GCR06h(3CEh/3CFh)。./139 的日志里这个读数
 *     在同一份 4972 行里出现过 5 次: 4 次是 0005h(桌面态), 唯一一次 00FFh
 *     出现在 2734 行 —— 而它前后各一行正是
 *         2733 TRC atc10 bit0 (1=graphics)=0000   <- ATC10 读回 0 = 文本态
 *         2734 TRC gcr06 bit0 (1=graphics)=00FF   <- 端口不回话
 *     也就是“主 VDD 把屏交给 DOS VM”那一拍。00FFh 不是任何合法的 GCR06 值
 *     (bit0..3 = 内存映射, 只有 0..F), 所以它是“这块 VGA 现在不属于我”的
 *     【物证】, 不是我们的任何标志位 —— r139 四条纪律的第③条要的就是它。
 *   OwnGate=1(默认): 见证说“不是我们的” ⇒ 延迟重建整段跳过(待办标志原样留着,
 *   下一拍硬件回到我们手里时照跑 —— 所以【回 Windows 那一拍的重建照旧发生】,
 *   r139 的成果按构造被保住)。OwnGate=0 逐字节回到 r139 的行为。
 *
 * 【三、两条一起看】ForceEdges 保“该修的时候一定修”, OwnGate 保“不该动的时候
 *   一根线都不动”。两者判据来源不同(前者=DDK 的时序义务, 后者=硬件物证),
 *   任何一条坏了另一条都还在。
 *========================================================================*/
/* 【r141 默认改为 0】./140 整份日志里这道门只被评估过一次, 而且结果是
 *   “仍然是我们的”(ev=0000) —— GCR06 端口从未在我们的采样点上死过。
 *   一个从未开火的闸门不是保护, 只是风险(它能把一次该做的重建
 *   挡成“什么都不做”)。代码与开关都留着供 A/B 对照, 但默认不参与。 */
static WORD  g_runForceEdges = 1;   /* r140: 会话边沿无条件重建(明文规则) */
/* r140-marker: OwnGate / ForceEdges / VxdVgaOwner */

/*--------------------------------------------------------------------------
 * HwIsOursNow —— 【r140】“这块 VGA 现在是不是 System VM 的?”
 *   返回 1 = 可以动手; 0 = 一根线都不许碰(调用方必须立刻返回)。
 *   【为什么它不是“又一个判据”】它读的是【端口答不答话】这件物理事实
 *   (drv.c 的 VxdVgaOwner, 用的是 ./110/./111 三台机器验证过的 idxread),
 *   不是我们自己的标志位, 也不是主 VDD 的虚拟 BIOS 答话。
 *   【落盘纪律】只有在【见证变化】时才写一行, 因为这个函数会被
 *   每一次 DDI 调用问到 —— 不能刷屏(与 r131 的“读数变化才落”同一条)。
 *------------------------------------------------------------------------*/

/*==========================================================================
 * 【r141】扫描行长度守望者 (PitchWatch) —— 用 VBE 4F06h 当“硬件还在不在我们的
 *   模式里”的【活判据】, 并让它成为一个与会话边沿【无关】的修复触发点。
 *
 * 【一、为什么是 4F06h, 而不是别的】
 *   ./140 的日志里, VerifyDisplayMode 的四个见证里只有这一个会【变】:
 *
 *     重建 #   pre-verify 4F06h 字节/行    post-verify
 *       1            0400h(512 像素)         0800h(1024 像素)
 *       2            0400h                   0800h
 *       3            0800h                   0800h
 *       4            0400h                   0800h
 *       5            0800h                   0800h
 *
 *   其余三个是【恒真的表达式】, 不是判据:
 *     · 4F03h 永远答 0072h(远东版 Windows 的文本模式号, DDK 的 CHECK_HIRES_MODE 页
 *       明写主 VDD 会把 72h/73h 改写成 02h/03h)—— r140 已撤回它的决定权;
 *     · “blind need” 的第一条见证就是上面那个 4F03h;
 *     · GCR06 物证(VxdVgaOwner)整份 ./140 只响过一次, 读数 0000(仍然是我们的)
 *       —— 端口从未“死”过, 所以那道门一次都没挡下东西, 是死重。
 *   四条纪律的第⑤条(STATUS_AND_PLAN.md): 一个只能取一个值的判据不是判据。
 *
 *   而 4F06h 是【VBE 规范里的只读取回形态】(VBE 2.0 §4.9 / VBE 3.0 Fun 06h:
 *   BL=01h Get Scan Line Length; 输入寄存器一个字节都不写 —— 见 drv.c 里
 *   VBE_ProbeScanLine 的长注与 SeaBIOS/VirtualBox 两处实现旁证)。
 *   它答的是“硬件此刻每扫描行多少个字节 / 多少个像素”, 而我们的模式
 *   (1024x768x16bpp, pitch=0800h)要求【正好 0800h 字节 / 0400h 像素】。
 *   答 0400h/0219h 就是“硬件在按另一种模式扫描”的硬证据 —— 与 4F03h 不同,
 *   它在我们的 4F02h 之后【会变成 0800h/0400h】, 所以它既能证伪也能证实。
 *   【纯 VBE 契约, 零厂商知识】判据里不出现任何具体芯片/板卡的名字。
 *
 * 【二、它为什么必须触发动作, 而不只是落一行日志】
 *   r139 的量测结论是: 真正修好画面的是“在会话边沿发一次真的 4F02h”。而
 *   ForceEdges 只覆盖【边沿】。若硬件是在边沿【之后】被切走的(比如全屏那一路
 *   主 VDD 自己那次 INT 10h AX=0 落在我们的重建之后), 下一次边沿之前就没有
 *   任何一枪 —— 这正是用户 ./140 报的“有时无法在 command 窗口存续时自动恢复
 *   正常”。守望者把发现点从“边沿”扩展到“每隔 128 次光标 tick”, 于是这种
 *   事后被切走的状态会在一个 tick 内被发现并修回来。
 *
 * 【三、安全边界(为什么它不会变成一把乱开枪的枪)】
 *   ① 只在【见过一次“对”的读数之后】才允许开火(s_pitchGoodSeen)。于是即使
 *      这台机器的 VBE BIOS 在健康态也答 0400h(那么这条判据就是坏的), 最多
 *      也只是在开机后多重建一次 —— 不会变成每 tick 一次的循环。
 *   ② 开火次数有上限 —— 【r146 已撤除】。r141 写的是"每次开机最多 8 次"
 *      (PITCH_ARM_MAX), 而 ./145 证明那是又一个"用满即永久沉默": 见下方
 *      s_pitchTickN 处的注。现在改成"最小间距 + 盒子持屏时不动手"。
 *   ③ 它只置 bRestorePending/bForceRebuild, 【不】自己碰硬件: 真正动手的仍是
 *      MaybeDeferredRestore 那条老路, 包括它对“DOS 盒还占着屏幕”的抑制闸门。
 *      也就是说 DDK VDD_SET_USER_FLAGS 页那条 "must not draw" 依旧被尊重。
 *   ④ PitchWatch=0 逐字节回到 r140 的行为; 4F06h 的读数照旧落日志(仪器不撤)。
 *========================================================================*/
static WORD  g_runPitchWatch = 1;      /* r141: 4F06h 采样(只作证据) */
/* 【r144】Int10Fix —— 全屏 DOS 盒请求 Far-East 文本模式(72h/73h)时,
 *   在 mini-VDD 的 48 号(PRE_INT_10_MODE_SET)里把客户机寄存器帧的
 *   EAX 改写成 0003h。依据(实证, 见 R144_DELIVERY §3): ./143 里用户
 *   必须打 `mode co80`(= INT 10h AX=0003) 才能把全屏花屏救回来 ⇒
 *   03h 这条路在本机是通的, 72h 不通。本开关只把用户
 *   已经证明有效的那一手挪到官方钩子上自动做一次。
 *   【只在全屏】mini-VDD 侧的闸门是 4→7 武装期(窗口化会话
 *   4 号不响), 所以窗口化的中文模式一个字节都不动。
 *   Int10Fix=0 就回到 r143 行为(只记录, 不改模式号)。            */
/* 【r150 默认 1 → 0, 并且值域扩成 0/1/2】./149 的判决见 vbeshare.h 的 r150
 *   注: 改写一旦落在【非武装期】的 72h 请求上(窗口化 command 的第一次设模式
 *   就是), 主 VDD 的模式记账就变成 0003h, 之后每一次桌面重建的 4F02 都返回
 *   014F, 桌面再也回不来(实测画面 = 全黑 + 文本光标, 即 ./149 的 2.png)。
 *   所以:
 *     0 = 【r150 默认】一个字都不改(与 r143 逐字节相同);
 *     1 = 只在武装期(4→7, 全屏盒子正持屏)改 —— 用户打 mode co80 的现场就是
 *         这里, 窗口化/控制台开机的 72h 请求一个字节都不动;
 *     2 = 无条件改 = 精确复现 ./149(已知会丢桌面, 只做 A/B 对照)。
 *   【为什么留着 2】诊断要有对照: "同一次开机、同一个盒子, 只差这一档"才能把
 *   因果关系钉死。默认 0 是为了先要一个能用的桌面。 */
/* 【r169 默认 1 —— 这一档重新成为默认, 理由见下】
 *   r151 把 mini-VDD 侧那段改写整个删掉了, 两条依据:
 *     ① ./150 的分类计数器: "武装期内 72h/73h 一次都没有"(arm72=0) ⇒
 *        那个闸门等于永久静默;
 *     ② DDK 原文说 Far-East Windows 把 72h/73h 折算成 02h/03h。
 *   r169 重新核对了这两条:
 *     ① 已被后来的日志推翻。./164 的
 *          `TRC fbox n48 72/73 total (r151)=0003`
 *          `TRC fbox n48 72/73 while armed (r151)=0003`
 *        说明在 r164 起的武装时序下, 盒子持屏期间的每一次设模式请求
 *        【全部】是 72h/73h。r150 的零读数只属于 r150 的时序。
 *     ② 那句话讲的是主 VDD 在 CHECK_HIRES_MODE 的 AL 里做换算, 不是
 *        "72h 在硬件上等价于 03h"。./150..163 的 `TRC st i10 ax=0072`
 *        与 ./168 的 `Verify: hw mode (4F03h ...)=0072` 都表明盒子要的
 *        就是 0072h。
 *   而真正硬的判据是用户给的: 全屏花屏时打 `mode co80`(= INT 10h
 *   AX=0003)立刻正常, 盒子自己要的 0072h 在这台机器上不通。于是 r169
 *   把这一手放回官方钩子(PRE_INT_10_MODE_SET, 48 号), 且只在
 *   全屏盒子持屏期(VgaFixArm != 0)动手 —— 窗口化会话的 72h 继续一个
 *   字节都不动, 那正是 ./149 丢桌面的原因。
 *   Int10Fix=0 一行关掉, 回到 r168 的行为(只记录, 不改模式号)。
 *   值域仍是 0/1(drv.c 把 >1 夹成 1; 无条件档已删除 —— ./149 证明它
 *   一定会把桌面弄丢, 没有保留成 A/B 的价值)。 */
static WORD  g_runInt10Fix = 1;        /* r406: default 1 = auto-cure on   */
/* 【r146】HiresFix —— mini-VDD 侧的"进盒前存一份我们的模式/寻址字节,
 *   回 Windows 后写回一份"(见 vbeshare.h 的 VBE_TR_Hires 与 minivdd.asm
 *   的 HiresSave/HiresRestore)。
 *   【为什么需要它】DDK 把"超出标准 VGA 的那部分状态"划给 mini-VDD;
 *   对一个【线性打包像素】的 VBE 模式来说, 那部分就是显存寻址/平面/带宽
 *   那几位。./145 的现场读数把缺口指了出来: 回到 Windows 之后
 *     SNAP GCR06=0005(对我们的模式) 而 SNAP SR01=0003(我们的模式是 0001)
 *   ⇒ 只有 r129 的 HiResClockFix 补过的那几个 bit0 是对的。
 *   而 ./144 的三张主机截图量出的形态(竖条纹 + 调色板颜色 + 只剩左半并
 *   横向拉伸)与"同一块 16bpp 显存被按 4 平面/奇偶解释"完全一致。
 *   HiresFix=0 逐字节回到 r145 的行为(一个寄存器都不写)。 */
static WORD  g_runHiresFix   = 1;      /* r146: mini-VDD 存/写回模式字节 */
/* 【r147】MemCFix —— 盒子持屏期间那六个"CPU 怎么看显存"寄存器的补写。
 *   【为什么必须单开一个开关, 而不是并进 VgaFix】./146 的日志把病灶量在
 *   了盒子持屏期(采集相位 35): CRTC 已经是正确的 720x400 文本模式, 而
 *   SR02/SR04/GCR05/GCR06 还是我们高彩的打包像素值 —— 盒子的文本写进去
 *   的地方不是扫描取指读出来的地方。本轮的行为改动【全部】在持屏期那六个
 *   字节上, 所以它必须能单独关掉:
 *     VgaFix=1 MemCFix=1 -> r147 默认(进盒前补一次 + 持屏期按六字节比较补)
 *     VgaFix=1 MemCFix=0 -> 持屏期一个字节都不写 = r146 的行为
 *     VgaFix=0           -> 连进盒/进盒后那两拍也不写(最靠近 r113)
 *   判据是"六个字节与文本目标逐字节比较", 相等就什么都不做 ⇒ 它自己会停,
 *   没有任何总量上限(与 r104 的边沿上限 / r141 的 PITCH_ARM_MAX / r145 的
 *   边沿节流 / r143 的 64 次补写预算 同属一类反模式, 这是第五次不再犯)。 */
/* 【r149 默认 1 → 0】./148 是它第一次真的开火, 也是判决: fixedN=52 次写入
 *   全部落进硬件(stillBad=0), 而画面从"花屏(co80 可救)"恶化成"灰屏 + co80
 *   失效"。DDK 原文解释了为什么: 窗口里的 DOS 会话 CRTC owner 是 Windows、
 *   MemC owner 是那个 DOS VM, "the CPU sees the video memory as a 4 plane
 *   VGA" —— 那组值(SR02=0F SR04=06 GCR05=00 GCR06=04)是主 VDD 按 DDK 摆的
 *   合法状态, 不是残留。所以默认关掉; 开关保留(=1 可复现 r148 的现象)。 */
/* 【r152 默认 0 → 2, 并给出第三个档位】./151 的日志把 r147 那条路的病根
 *   量出来了: 探针(dirty/seenText)在跑, 而 fixedN 恒为 0 —— 因为写那六个
 *   字节的代码只在 policyMemCFix != 0 时才走, 而 r149 起默认就是 0。
 *   也就是说 r147 的六字节补写在 r149/r150/r151 三轮里【一次都没开火】,
 *   ./148 的灰屏判决一直被继承, 却再没有被重新检验过。
 *
 *   本轮不推翻那个判决, 而是把"修"换到另一件东西上:
 *     MemCFix=0 -> 一个字节都不写(只记账) = r149..r151 的行为
 *     MemCFix=1 -> r147 的六字节文本补写(./148 实测: 灰屏 + co80 失效)
 *     MemCFix=2 -> 【默认】只写 CR0C/CR0D(显示起始地址), 写完自证, 失败还原
 *
 *   为什么"挪显示起始地址"与"改那六个寄存器"是两件不同的事:
 *     前者不改变任何一个模式/寻址位, 只是把扫描起点挪到【盒子真正写了
 *     字符的那块显存】; 后者会把整块显存的解释方式换掉(映射 + 奇偶),
 *     于是已经写进去的内容全部作废 —— ./148 的灰屏正是这么来的。
 *   探针(纯读)无论哪一档都会跑: 它是本轮的判据, 不能挂在策略闸门后面。
 *   依据见 vbeshare.h 的 VBE_TR_Probe 长注与 R152_DELIVERY §2。 */
/* 【r153 —— 这一档位在 r152 里根本没到达 VxD】./152 的日志给出铁证:
 *   `CFG27 r152 ... 0001 0002 0004`(驱动侧看到 2), 而 VxD 侧的
 *   `TRC memcfix fixedN = 0014`(六字节补写真的写了 20 次)⇒ r152 实际跑的
 *   是档位 1, 也就是 ./148 那条已被证伪的路。夹取点在 drv.c 的
 *   `s_polMemCFix = memcfix ? 1 : 0`, 本轮已删除并改成直通。
 *   这解释了两件事: ① r152 的症状与 r148 逐条相同(灰屏 + co80 失效);
 *   ② r152 新加的取指基址修复【一次都没开火】(fix=0000) —— 因为档位不是 2。
 *   本轮的对策不只是改那一行, 而是【两侧各印一次档位】:
 *     display.c: `PH: r153 MemCFix drv=xxxx`
 *     drv.c    : `PH: probe policy=xx`(VxD 真正收到的字节)
 *   这两个数不等 = 中间还有一层夹取, 一眼就能看出来。 */
/* 【r142】WaitSafe —— 入口边沿的重建要不要等“切屏事务收尾”。
 *   【为什么默认 1】./141 的时间轴把 r139 开始的“入口边沿立刻重建”量穿了:
 *     边沿(t=00176048) -> 我们的 4F02(t=11007行) -> **主 VDD 自己的 DOS
 *     模式设置才真正写进硬件**(行 11118..11149: CRTC 29 次 + 序列器 7 次
 *     + GCR 9 次 + DAC 746 次、并且它自己调了 4F01/4F03/4F06、还叫了
 *     SAVE_REGISTERS(8 号, AX=0082)) ⇒ 我们那次重建被后面那一串写盖掉,
 *     而之后再也没有人重建 ⇒ “等再久也不恢复”。
 *   官方依据(DDK PRE_HIRES_TO_VGA 页的 7 步序): “盒子拿屏”这一趟里
 *     第 6 步才是主 VDD 发 INT 10h AX=0 真正改硬件, 第 7 步才回 ENABLE_TRAPS。
 *     所以“硬件回到我们手里”的官方信号只能在第 6/7 步之后——本轮就把
 *     它接成第二个安全信号(blob 的 VBE_TR_State bit0 上升沿)。
 *   【关掉会怎样】WaitSafe=0 逐字节回到 r141 的行为(边沿当场发 4F02)。 */
/* 【r143 撤回 r142 的“等切屏事务收尾”】
 *   ① 它从来没执行过。r142 把等待挂在 `wArmedEntry` 上,
 *      而实际跑的那条入口边沿(EntryAlways=1, display.c 第
 *      8263 行附近)写的是 `wArmedEntry = 0` + `wArmedEnd = 1`
 *      ⇒ 分支条件永不成立。./142 的日志里
 *        `entry arm -> wait for the switch to finish (r142)` 一行都没有,
 *        只有 `no safe signal -> fallback (r142)`(两次)—— 所以 ./142
 *        就是一次 r141 的跑法, 用户报告逐字相同完全合理。
 *   ② 它针对的那个“顺序”问题本身也被 ./142 推翻了:
 *      我们那次 4F02h 落在 T≈17F100(ENRT 之后不到 2 秒),
 *      而 4F06h 从那一刻起一直是 0800h(正确值)直到日志结束;
 *      真正的切屏事务(4/6/7 号)发生在约 100 秒之后的那段
 *      ring 3 空白里(心跳 116 秒没有一行), 两件事不是同
 *      一件。既然没有“被后写覆盖”这回事, 等待就没有意义。
 *   ③ 第二条安全信号(VBE_TR_ST_VGA 上升沿)在结构上就是
 *      坏的: 它是一个由 ring 3 采样的电平, 而它被置起
 *      的那段时间(函数 4/5 到函数 7)恰恰就是 ring 3
 *      不跑的那段时间。./142 里它只亮过一次, 而那一次
 *      是驱动 Disable 时 26 号置的位(行 6501)。
 *   → 默认改回 0(r141 行为); WaitSafe=1 保留作 A/B。
 *   【真正在工作的那条信号】`VxdTakeSafeToSetMode` 同时还由
 *   事件计数器 VBE_TR_Evt[0](7 号 = POST_VGA_TO_HIRES)的变化置起
 *   (drv.c 第 3975 行): 那个计数器在 ring 3 空白期间照样累加,
 *   下一次采样就能看到—— 这才是唯一在结构上可靠的信号。
 *   r142-marker: WaitSafe (retired in r143 -- the branch it guarded never ran).  */
static WORD  g_runWaitSafe   = 0;      /* r143: retired (was 1 in r142) */
static WORD  s_pitchSeen     = 0xFFFF; /* 上次读到的字节/行(变化才落盘) */
static WORD  s_pitchAskN     = 0;      /* 问过几次(记账) */
static WORD  s_pitchGoodSeen = 0;      /* 见过“对”的读数(开火的前提) */
static WORD  s_pitchArmN     = 0;      /* 因它挂上重建的次数 */
static WORD  s_pitchWant     = 0;      /* 1 = 请 MaybeDeferredRestore 重建 */
static WORD  s_pitchTickN    = 0;      /* PitchWatchTick 被调的次数(光标节拍) */
static WORD  s_pitchLastArm  = 0;      /* 上一次请求时的节拍              */
static WORD  s_pitchPb       = 0xFFFF; /* 上次落盘的 BX(字节/行)         */
static WORD  s_pitchPp       = 0xFFFF; /* 上次落盘的 CX(像素/行)         */
/* 【r146】PITCH_ARM_MAX(每次开机最多 8 次)已删除 —— 它与 r144 的边沿上限
 *   是同一个反模式: 用满即【永久】沉默。./145 的硬证据: `PH: pitcharm`
 *   到 0008 之后整场再没变过, 而那段之后硬件一直不是我们的模式。
 *   代替它是两道不会永久失效的边界:
 *     ① PITCH_GAP_TICKS —— 两次请求之间至少隔这么多光标节拍(节拍本身
 *        已经由 CheckCursor 的 128 次采样节流);
 *     ② 活门 —— 盒子【此刻】持屏(arm==1)时一个字节都不碰(DDK:
 *        the display driver must not draw while a DOS box owns the screen)。 */
#define      PITCH_GAP_TICKS 4
/* r141-marker: PitchWatch / HwPitchNow / PitchWatchTick / PitchWatchRequest */
/* r142-marker: WaitSafe (the entry arm waits for the switch to finish) */

/*--------------------------------------------------------------------------
 * HwPitchNow —— 问一次“硬件的每扫描行字节数是多少”。
 *   返回 1 = 就是我们的 pitch(硬件在我们的模式里);
 *        0 = 不是(硬件在别的模式里 —— 这是要修的那种);
 *        2 = 问不到(不冒充结论, 与 4F03h/4F06h 一贯的纪律一致)。
 *   【落盘纪律】读数变化才写一行 —— 本函数会被 cursor tick 反复问到。
 *------------------------------------------------------------------------*/
/* [r406] 提前声明: 观测代码在 HwPitchNow 定义之前就调用它,
 *   隐式声明在 -zu(SS!=DS) 下不可靠, 且会掩盖真实原型。 */
static int HwPitchNow( const char *why );
extern uint16_t __far __pascal VxdStateBitsNow( void );

/* [r406] 电平式状态观测 —— 观测本身【不受任何守卫限制】。
 *   r340 的错: 快照与比对都放在 wEnabled / VxdBoxArmedNow 的守卫里,
 *   结果一行都没输出(日志里 PH: snap/cmp 为 0), 等于什么都没测到。
 *   现在: 每次都打印输入值; 快照缺失时用当前值就地建立。 */
static WORD s_snapPitch = 0;
static WORD s_snapState = 0;
static WORD s_snapValid = 0;

static void DesktopStateCheck( const char *why )
{
    WORD p, st, en, armed;

    PHASEV( "PH: chk entry=", 1 );          /* [r406] 先打点, 再碰硬件 */
    if (wScreenPitchBytes == 0)             /* 模式尚未建立 -> 硬件查询无意义且危险 */
    {
        PHASEV( "PH: chk skip nopitch=", 0 );
        return;
    }
    p     = HwPitchNow( why );
    st    = VxdStateBitsNow();
    en    = (WORD)(wEnabled ? 1 : 0);
    armed = VxdBoxArmedNow();

    PHASEV( "PH: chk pitch=", p );
    PHASEV( "PH: chk state=", st );
    PHASEV( "PH: chk en=",    en );
    PHASEV( "PH: chk armed=", armed );

    if (!s_snapValid)
    {
        s_snapPitch = p;
        s_snapState = st;
        s_snapValid = 1;
        PHASEV( "PH: snap pitch=", s_snapPitch );
        PHASEV( "PH: snap state=", s_snapState );
        return;
    }
    PHASEV( "PH: cmp want=", s_snapPitch );
    if (p != s_snapPitch)
        PHASEV( "PH: MISMATCH pitch=", p );
    if (st != s_snapState)
        PHASEV( "PH: MISMATCH state=", st );
    if (p == s_snapPitch && st == s_snapState)
        PHASEV( "PH: cmp ok=", 1 );
}

/* [r406] 极简闸门用: 与快照一致 => 无需重绘 */
/* [r406] 重绘节流: 实机 LegacyGate=0 曾"一直重绘到黑屏",
 *   根因 (a) 重建后没刷新快照 (b) pitch 为 0 时被判成不一致。 */
static WORD s_healLast  = 0;
static WORD s_healCount = 0;

static int DesktopStateMatches( void )
{
    WORD p, st;
    p  = HwPitchNow( "heal check" );
    st = VxdStateBitsNow();
    if (!s_snapValid)
    {
        s_snapPitch = p; s_snapState = st; s_snapValid = 1;
        return 1;
    }
    PHASEV( "PH: heal chk p=", p );
    /* 任一侧为 0 => 无法判定, 视为一致: 宁可不重绘, 也不要无限重绘 */
    if (p == 0 || s_snapPitch == 0 || wScreenPitchBytes == 0)
        return 1;
    return (p == s_snapPitch && st == s_snapState);
}

static int HwPitchNow( const char *why )
{
    WORD pb;
    WORD pp;

    (void)why;

    if (!g_runPitchWatch)
        return 1;
    if (!wEnabled || wScreenPitchBytes == 0)
        return 2;

    s_pitchAskN++;
    if (!VBE_ProbeScanLine())
    {
        if (s_pitchSeen != 0xFFFF)
        {
            s_pitchSeen = 0xFFFF;
            DIAGM( "HwPitch: 4F06h unavailable (the reading is now unknown)" );
            FLUSH();
        }
        return 2;
    }
    pb = VBE_ProbePitchBytes();
    pp = VBE_ProbePitchPixels();
    /* 【r146】自洽检查 —— r141 那条判据缺的正是这道闸门。
     *   VBE 4F06h BL=01h 的返回是 BX = 每扫描行【字节】数, CX = 每扫描行
     *   【像素】数(VBE 2.0 §4.9); 两者之比必然是 1/2/3/4(8/16/24/32bpp)。
     *   ./145 里出现过一对自相矛盾的读数:
     *     PH: hwpitch==0200  PH: hwpx==1000
     *   (512 字节/行 而 4096 像素/行 = 每像素 0.125 字节) —— 那不是"硬件在
     *   别的模式里", 那是【这次问话没有抵达硬件】: 主 VDD 在 DOS 会话期会
     *   自己虚拟化 INT 10h, 这件事 drv.c 的 HwProbe 早就写在日志里了
     *   ("int10 answer changed (main VDD state, NOT a hardware read)")。
     *   一个自相矛盾的答案不能当成"硬件不是我们的": 按老判据它让守望者在
     *   0.8 秒内连开 8 枪(./145 的 PH: pitcharm 正好走到 8), 而那 8 枪
     *   全被 HealGate 挡下 —— 上限也就此用满。本轮: 不自洽 ⇒ 不知道。 */
    if ((pp == 0) || ((pb % pp) != 0) || ((pb / pp) < 1) || ((pb / pp) > 4))
    {
        if ((s_pitchPb != pb) || (s_pitchPp != pp))
        {
            s_pitchPb = pb;
            s_pitchPp = pp;
            s_pitchSeen = 0xFFFF;
            DiagHex( "HwPitch: 4F06h answer is SELF-INCONSISTENT (bytes)",  pb );
            DiagHex( "HwPitch: ... (pixels)",                               pp );
            DiagHex( "HwPitch: our mode pitch (want)",     wScreenPitchBytes );
            DIAGM( "HwPitch: bytes/pixels is not 1/2/3/4 -> treated as unknown (r146)" );
            PHASEV( "PH: pitchbad=",  0 );          /* 0 = 这次读数不可用     */
            PHASEV( "PH: hwpitch=",   0xFFFF );     /* 与"问不到"同一处置     */
            PHASEV( "PH: hwpxraw=",   pp );
            FLUSH();
        }
        return 2;
    }
    if ((pb != s_pitchSeen) || (pb != s_pitchPb))
    {
        s_pitchSeen = pb;
        s_pitchPb   = pb;
        s_pitchPp   = pp;
        DiagHex( "HwPitch: 4F06h bytes per scan line", pb );
        DiagHex( "HwPitch: 4F06h pixels per scan line", pp );
        DiagHex( "HwPitch: our mode pitch (want)",     wScreenPitchBytes );
        DiagHex( "HwPitch: ask n",                     s_pitchAskN );
        if (pb == wScreenPitchBytes)
            DIAGM( "HwPitch: the hardware scan line IS our mode's (r141)" );
        else
            DIAGM( "HwPitch: the hardware scan line is NOT our mode's (r141)" );
        FLUSH();
    }
    if (pb == wScreenPitchBytes)
    {
        s_pitchGoodSeen = 1;
        return 1;
    }
    return 0;
}

/*--------------------------------------------------------------------------
 * PitchWatchTick —— 挂在光标 tick 上的采样点(CheckCursor 的 128 次节拍)。
 *   【为什么开火要等 s_pitchGoodSeen】见上面【三、①】。
 *------------------------------------------------------------------------*/
static void PitchWatchTick( void )
{
    int v;

    if (!g_runPitchWatch || !wEnabled)
        return;
    s_pitchTickN++;
    /* 【r146】活门: 盒子【此刻】持屏时连问都不问。
     *   依据(实证): ./145 里守望者发现"不是我们的模式"的那几拍
     *   (T=0003B3F5 起) 全都发生在盒子刚把屏还回来之后, 而它开出的 8 枪
     *   全被 HealGate 挡下; 反过来, 真正在持屏期问硬件时得到的又是一对
     *   自相矛盾的读数(见 HwPitchNow 的自洽检查)。两道门必须同时正确:
     *   判据不可信时不判、盒子持屏时不动手。 */
    if (VxdBoxArmedNow() == 1)
        return;
    /* 【r146】间距取代"每次开机 8 次"的硬上限(见 PITCH_GAP_TICKS 的注)。 */
    if ((WORD)(s_pitchTickN - s_pitchLastArm) < PITCH_GAP_TICKS)
        return;

    v = HwPitchNow( "cursor tick" );
    if (v == 0 && s_pitchGoodSeen)
    {
        s_pitchArmN++;
        s_pitchLastArm = s_pitchTickN;
        s_pitchWant = 1;
        DIAGMIN( "HwPitch: hardware is not scanning our mode -> rebuild requested (r141)" );
        PHASEV( "PH: pitchbad=",  s_pitchSeen );
        PHASEV( "PH: pitcharm=",  s_pitchArmN );
        PHASEV( "PH: pitchtick=", s_pitchTickN );
        FLUSH();
    }
}

#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
static WORD  s_rastN        = 0;        /* r133: bounds the RAST print */
#endif
static WORD  s_protClrN     = 0;
static WORD  s_snapTick     = 0;
static WORD  s_snapN        = 0;
/* r132-marker: ProtClr / SnapSess policies. */
/* 探针观测到的硬件模式(AL): 0xFF = VBE 模式; 其它 = 标准 VGA 模式号。
 * 0xFFFF = 还没问过或问不到。                                          */
static WORD  g_hwModeAl = 0xFFFF;
/* 【r107】"已知良好"参考值 —— 见 HwBaseline 的长注。
 *   ./106 的日志把 r106 的判据(AL==0FFh 才算 OK)证伪了: 那次 4F02 重建
 *   【成功】(FbTest 全过 + 用户看到桌面回来了), 而重建前后与重建后的
 *   四次探针一律读到 AL=0x72 —— 也就是说"AL != 0FFh"在这台 BIOS 上
 *   【不是】异常特征。0xFFFF = 还没有参考值。                          */
static WORD  g_hwBaseAl = 0xFFFF;
/* 【r109】AH 的两个快照 —— 入口侧抑制闸门(HealGate)用的就是它们。
 *   【为什么是 AH 而不是 AL】本机实测(./107、./108 六份日志一致):
 *     开机基线      ax=8072   ⇒ AH=0x80(主 VDD 的高分辨率记账值)
 *     DOS 文本会话  ax=5086   ⇒ AH=0x50(80 列 = 文本模式)
 *   即"主 VDD 认为自己处在文本模式"这件事在 AH 上有稳定指纹, 而 AL 在本机
 *   恒为 0x72(与 VESA 的 0FFh 约定不同, 见 g_hwBaseAl 注)。
 *   g_hwBaseAh = 开机基线(0xFFFF = 未取), g_hwLastAh = 最近一次读数。   */
static WORD  g_hwBaseAh = 0xFFFF;
static WORD  g_hwLastAh = 0xFFFF;
/* 重建后的静默期: 免得"重建 -> 立刻又探到不一致 -> 再重建"打转。      */
/* 探针结果计数(落进日志, 便于一眼看出这个开关到底跑了几次) */
static WORD  g_hwProbeN = 0, g_hwProbeBad = 0;
static int   HwProbe( const char *why );     /* r106, 定义在 MaybeDeferredRestore 之前 */
/* 【r127·已删除】CrtProbeNow(已于 r407 删除) —— 见定义处的历史长注。
 *   why       = 落进日志的说明行(只在诊断构建里真的落盘)
 *   bWantDiff = 1 时本函数【真的去问】VxdHwRegDiff()(r126 是传常量 -1,
 *               于是 "CRTP hwreg diff" 永远打印 0EEEE —— 那一轮零信息量)
 *   bDoClock  = 1 时在本次读数里做"点时钟归位"
 *               (见 drv.c 的 VxdClockEnsure8Dot 长注) */
static void  HwBaseline( void );            /* r107, 同上(MARK HWBASE 参考值) */
/* 把探针结论折成一个可打印的字: 1 = 在 VBE 通路, 0 = 被切到标准 VGA 通路,
 * -1(问不到/开关关) → 0EEEE 以免和"真的是 0"混在一起。
 * 【为什么做成宏】发布构建里 DiagHex 是空宏【且不展开实参】, 于是这三个
 * "只为取证"的探针在发布构建里被整条编译掉 —— 发布版不该为诊断多付
 * 一次 BIOS 往返; 而带副作用的探针(周期性那条)不走这里, 两种构建行为
 * 完全一致。                                                          */
/* 【r118】MIN 构建也要这个格式化器 —— 它的消费者是 DiagHexMin。 */

/* 【r104】VgaEdge —— "这次的 DOS 虚拟化会话收尾了"这个信号从哪来。
 *   【要解决的问题】./103 的 B0/B1FS/B1WS 三份日志里, 主 VDD 的
 *   ResetHiResMode 回调(= 我们的 RestoreDesktopMode)【一次都没有被调用】
 *   (只有 B2 调了一次), 而 bRestorePending 只由那个回调或 Heal=1 置位
 *   ⇒ 桌面在 DOS 会话结束后再没有任何人去重建它。用户看到的就是"全屏
 *   DOS 回不到窗口/桌面"。
 *   【为什么 TRAPS 位的下降沿就是那个信号】./103 四份日志状态机完全一致:
 *   会话前 bits=0, 会话中 bits 含 bit1(VBE_TR_ST_TRAPS, 由 ENABLE_TRAPS(13)
 *   置位), 会话结束后回到 bits=0 —— bit1 是整场 DOS 会话期间【持续为 1】
 *   的唯一一位(bit0 VGA 在窗口化会话里被 11/12 号反复开关), 而清掉它的
 *   只有 DISABLE_TRAPS(14), 那正是主 VDD 结束这次虚拟化的时刻。
 *   ⚠ 该判据尚属推断(真正依据只有 4 份日志的 3 个采样点); 所以它是 INI
 *   开关, 且每次触发都会打一行 `TRC evt sessEnd`。若日志显示它在一次
 *   DOS 会话里反复触发(bit1 其实是被逐次开关的), 把它置 0 即可 —— 那就
 *   等于回到 r103 的行为。
 *   1 = 【默认】按 TRAPS 下降沿请求一次桌面重建; 0 = 关闭。          */
static WORD  g_runVgaEdge  = 1;

/* 【r108】VhEdge —— "主 VDD 刚走完一趟 hires→VGA→hires 往返"这条边沿
 *   要不要当成立刻重建桌面的触发。
 *
 *   【为什么必须有它 —— ./107 的日志给出来的时间轴】
 *   t3a(r107, 默认配置)那一趟 DOS 会话的四个时刻(NUL 剥壳后的行号):
 *     T=000EF7D7  会话开始: TRAPS 位置起、i10 ax=0003(DOS 应用请求模式 3)
 *     T=000EF7FC  MARK HB
 *     T=000EF820  postVH/preHV/preVH 三个事件计数同时 +1 ⇒ **主 VDD 刚刚
 *                 走完 4/5/6/7 号一整趟**(hires→VGA→hires)
 *     T=000EF83B  会话收尾: TRAPS 位落下
 *   也就是说: 硬件被主 VDD 借走又还回来发生在【会话进行中】(开始后约
 *   2.5 秒), 而 r104 的 VgaEdge 钉的是最后那一行 —— 比往返晚了约 1.5 秒。
 *   用户看到的现象("运行 command 就花屏, exit 后 GUI 才恢复")正好等于
 *   这一段时差: 往返之后硬件并没有回到我们的 0x117(对 VBE 这种主 VDD
 *   自己不会设的模式, 7 号之后的"重新建立"是 mini-VDD/显示驱动的活;
 *   我们的 mini-VDD 在那四个服务上只记账不动作, 那是对的 —— 回调上下文
 *   里绝不能碰 BIOS, 见 drv.h 的 r100 注), 而驱动侧要等到 TRAPS 落下才
 *   去重建。
 *
 *   【本开关做什么】把重建的触发点从"会话收尾"提前到"往返刚结束":
 *   drv.c 在 VBE_TR_Evt[POST_VGA_TO_HIRES] 变化时置一次请求(见
 *   drv.h 的 VxdTakeVgaToHiresEdge 注), 这里取走后置 bRestorePending +
 *   bForceRebuild —— 真正的 4F02 仍然走既有的"等安全信号"闸门, 而那一刻
 *   安全信号正是刚变化的那一个, 于是重建就落在同一拍里。
 *   【与 VgaEdge 的关系】两条边沿都留: VhEdge 管"会话进行中被借走又还
 *   回来", VgaEdge 管"会话收尾"(仍然可能有别的机型/别的路径只给后者)。
 *   1 = 【默认】按这条边沿请求一次桌面重建; 0 = 关闭(回到 r107 行为)。 */
static WORD  g_runVhEdge   = 1;
/* 已按 VhEdge 触发过几次(落进日志, 便于一眼数出来) */
/* r108-marker: VhEdge arms the rebuild on the hires->VGA->hires round trip */

/* 【r108】EntryHeal —— 入口侧的激进补救(默认 0)。
 *   见 drv.h 的 VxdTakeSessionStartEdge 注: ./107 的日志证明不了"屏幕是
 *   从哪一刻开始花的"。若用户看到的是"DOS 盒一打开就花", 那就说明硬件在
 *   【入口】就被写坏了(主 VDD 只虚拟化标准 VGA 端口, 扩展寄存器不归它管),
 *   而 VhEdge 要等到主 VDD 那趟往返(会话开始后约 4 秒)才能救回来。
 *   EntryHeal=1 时: DOS 会话一开始(TRAPS 上升沿)就请求一次重建。
 *   【风险】这等于在 DOS 盒正在建立画面时插一次 4F02; 它不会像 r99 那样
 *   落在主 VDD 的回调事务里(我们在自己的 DDI 上下文), 但仍然比 VhEdge
 *   粗暴。只作为 A/B, 默认关。                                       */
static WORD  g_runEntryHeal = 1;
/* r109-marker: EntryHeal is ON by default — ./108 t4 proved it fixes the
 * windowed DOS box (garble flashes, then the desktop comes back correct). */

/* 【r109】HealGate —— 入口侧补救的抑制闸门(默认 1)。
 *   【要解决的问题】t4 证明了"会话一开始就重建"能救回窗口化 DOS 盒, 但它
 *   同时暴露两件事: ① 进去/出来都会闪一下; ② 按 Alt+Enter 进全屏后画面
 *   仍然是花的(t4 的 altenter 截图 = 一整屏 VGA 文本模式乱字符)。
 *   ②的机理是"我们和 DOS 盒抢硬件": 全屏 DOS 会话需要硬件停在 VGA 文本
 *   通路上, 而我们那条补救不管三七二十一就发一次 4F02 把它拉回 1024x768x16。
 *   【闸门判据】动手之前先看两条只读证据:
 *     · mini-VDD 状态位的 bit0(VGA 通路已置起) —— 主 VDD 真的把硬件切走了
 *       (VxdStateBitsNow, 见 drv.h);
 *     · 主 VDD 虚拟 BIOS 对 INT 10h AH=0Fh 的答案: 本机开机基线是
 *       AH=0x80(高分辨率记账), 而主 VDD 认为自己在 80x25 文本模式时回
 *       AH=0x50(见 HwProbe 的 r108 注与该函数里的实测值)。
 *   任一命中 ⇒ 这一次补救【放弃】, 并落一行 `entry heal SUPPRESSED` 把两条
 *   输入一起打出来 —— 下一轮据此判断闸门该紧该松。
 *   【为什么判据这么窄】默认配置下这条补救是"窗口化不花屏"的唯一依靠,
 *   判据一宽(例如"答案变过就抑制")就会把窗口化会话也误伤 —— 所以只认
 *   "主 VDD 明确说自己进了文本模式"和"硬件真被切到 VGA"这两条硬信号。
 *   1 = 【默认】开闸门; 0 = 关(回到 r108 t4 的行为, 用于 A/B)。      */
static WORD  g_runHealGate = 1;
/* 【r109】EndHeal —— 会话收尾时那次重建要不要"强制"。
 *   【r104 的原语义】收尾边沿一律强制 4F02(因为当时没有任何别的重建者)。
 *   【为什么现在要改】开了 EntryHeal 之后, 屏幕在会话进行中就已经被修好了;
 *   收尾再强制重设一次模式, 用户看到的就是"退出 DOS 盒时又闪一下"(t4 的
 *   实测)。判据用主 VDD 的 AH=0Fh 答案: 若它已经回到开机基线(AH=0x80,
 *   即主 VDD 自己认为回到高分辨率了), 就只做重绘 + 重新登记, 不发 4F02;
 *   否则照旧强制(那才是真正需要救的情况)。
 *   1 = 【默认】收尾时按判据决定强制与否; 0 = 永远强制(r104..r108 行为)。*/
static WORD  g_runEndHeal   = 1;
/* 【r109】PortProbe —— 直接读 VGA 寄存器拿硬件真相(默认 0)。
 *   见 drv.h 的 PortProbeRead 注: ./108 的主机截图是"2 像素周期条纹 +
 *   DOS 盒黑区从第 278 行开始(guest 是 136 行)", 说明显存没错、错的是
 *   光栅/像素格式; 而 0Fh 探针读的是主 VDD 的记账, 读不到硬件。
 *   PortProbe=1 时在几个安全点(开机基线后 / 每次状态边沿 / 每次重建前后)
 *   把 CRTC 00/01/12/13/0C/0D/14/17、时序器 01/04、GCR 05/06、3CCh、3C6h
 *   读出来落盘。只写索引、只读数据, 不改任何显示状态。
 *   【为什么默认 0】"端口读"在本机是否被允许此前从未验证过(本驱动只用过
 *   端口写, 而且只在 8bpp 的 DAC 路径上用过); 万一那条 in 不被允许就是一次
 *   0D。所以它是本轮唯一需要单独跑一次去证的新变量, 默认配置一个端口都不读。
 *   1 = 开; 0 = 【默认】关。                                        */

/* 【r127】Clk8 —— 会话收尾时"把点时钟归位成 8 点"这一步的开关。
 *   见 drv.c 的 VxdClockEnsure8Dot 长注(为什么这是通用 VGA 收尾义务)与
 *   ./123 的 CRTP 实测(SR01 在会话结束时停在 9 点, 而 4F02 的归位发生在
 *   清屏/重绘之后 —— 顺序反了)。
 *
 *   【r128 降级为"纯观察"】./127 的结果要求把这一枪降级, 理由不是推断而是
 *   实测: R2 用的是 **Clk8=0**(这一步根本不执行), 而"横向缩小"新 bug 在 R2
 *   里【照样出现】; 同时 R1/R2 的花屏行为完全一致。也就是说:
 *     · 这个新 bug 不是这一步写出来的 —— 但也不能证明它是安全的;
 *     · 唯一的处置是"先不要伤害": 默认【只观察不写】, 把决定权交给读数。
 *
 *   【r129 撤回那次降级 —— 理由被后面的读数推翻了】r128 降级时手里只有
 *   "Clk8=0 也缩小"这一条; r129 拿到了真正解释得通的那条链(见上面的
 *   HiResClockFix 与 ./127 r1 的 CRTP n=0011/0012 读数):
 *     会话结束时硬件真的停在 SR01=0x0003(9 点)/ AR10=0x0000(文本)/
 *     Misc 3CC=0x0067 上, 而 86Box 的通用 SVGA 渲染器正是用
 *     `!(GCR6 bit0) && !(AR10 bit0)` 判文本、用 SR01 bit0 定字符宽度 ——
 *     两者都与"桌面按 8 点、1024 宽写显存"矛盾 ⇒ 窗口被收缩、画面横向压扁。
 *   而 r128 之所以"看到缩小还在", 是因为它把这一步降成了"只读" ——
 *   观察不等于修复。所以 r129 把它恢复成【确认后归位】。
 *
 *   三个取值:
 *     2 = **确认后归位(默认)**: 只有读回 bit0 为 0(确实是错的)时才写一个字节
 *     1 = 只观察: 读 SR01 并把"是否 8 点"落盘, 但【一个字节都不写】
 *     0 = 完全不碰(连读都不读) —— 逐字节回到 r126 的行为
 *   【注意】它落在 ApplyDesktopRestore 里 —— 两条收尾路径(重建 / 捷径)
 *   都经过那里, 所以这一个开关同时管住两处。
 *   r129-marker: Clk8 default 1 -> 2 (observe -> fix-if-wrong).           */
static WORD  g_runClk8      = 2;
/* 【r128】Probe2 —— "第二批探针"开关(属性控制器 10h 读数)。
 *   它管的是 r127 新引入的那批端口操作:
 *     · 属性控制器读数(3DAh + 3C0h + 3C1h) —— 见 CrtProbeNow(已于 r407 删除)的说明。
 *   【r128 默认 0 的理由已经撤回】当时把"三个 0D 蓝屏"归到了这批新端口读上
 *   ("只有它在前几轮没跑过")。后续证据全部指向反方向:
 *     · 四份崩溃日志都停在同一个操作 DIB_DibToDevice, 但 DDI 计数各不相同
 *       (001A/001B/001B/001E)⇒ 不是固定某一拍; 而且其中三份的周期探针
 *       【一次都没跑到】(CRTP 计数 = 0)⇒ 不可能是它;
 *     · 全部历史轮次的开关组合放在一起比对: 没有任何组合能分开"崩"与"不崩";
 *     · 用户确认: 这个 0D 只在换驱动/改参数后的第一次进 GUI 时偶发, 在更早
 *       (已被删除)的轮次里同样出现过 —— 与 r127 那批新端口操作无关。
 *   ⇒ 没有理由再把它关着。而且 AR10 bit0 恰恰是"文本还是图形"那两个判定
 *   位之一(另一个是 GCR06 bit0), 少了它, 上面的结论就只能靠一半读数。
 *   1 = 开(默认, 读一个索引、只写一次索引、读完还原); 0 = 关。
 *   r129-marker: Probe2 default 0 -> 1 (the BSOD attribution was retracted). */
static WORD  g_runProbe2     = 1;

/* r99: vddapi16.asm 的 VDDREGISTER 直接把这个全局当 EDX 用
 * (0 / 0xFFFFFFFF)。必须是【非 static】的 C 全局 —— 汇编侧写的是
 * extrn _g_vddEdxValue:dword。由 ApplyRunProfile 按 g_runVddVirt 折算。 */
DWORD g_vddEdxValue = 0;
static WORD g_runDelpDev  = 1;   /* r87: 是否把 delpPDevice 填成"显存远指针" */
/* r94: DIBENG 的 DGROUP 选择子是否用"装载器桩自证"修正(默认 1 = 修)。
 * 置 0 = 回到 r93 的"照模块库解析值用"(用于 A/B)。见 DibDgroupProbe。
 * 【r95】置 0 时 g_dibDgTrusted 保持 0, 于是跳板既不装 DS 也不用这个值,
 * 转发器行为与 r93 完全一致 —— 这一个开关同时关掉"探针"与"装 DS"。 */
static WORD g_runDibDg    = 1;

/* --- r97: 调色板 DDI 走哪条路 + 引擎的"非 Ext 入口 EDX 协议" --------------
 * 【r96 的结果把病灶钉死了】r96 崩点 = 0D @ 0367:00004036(比 r95 的
 * 0367:3FA5 前进 0x91 字节); 而 r96 新增的 DIBCODE 三窗把这一族函数的
 * 机器码抄了回来(r96 日志 off=4018/4058/4098/3FA0/3FA9/3FD0...):
 *     0367:401A  DIB_SetPalette    : 66 59 66 52 66 51   ← 6 字节跳板
 *     0367:4020  DIB_SetPaletteExt : 55 8B EC 1E 56 57 66 83 7E 0A 00 74 49
 *                                    C4 76 0A C5 7E 06 C5 7D 20 66 FF 45 24
 * 0x4036 正好是最后那条 `inc dword [di+0x24]`, 而 di:DS 来自
 * `lds di,[bp+06]` = 第 4 个参数, 也就是 0x401A 那 6 字节跳板
 * (pop ecx / push edx / push ecx)塞进去的【EDX】。
 * ⇒ 结论: DIBENG 的"非 Ext"导出要求调用方把设备指针放在 EDX 里(标准小
 *   驱动里这件事由 DIBLINK 做), 不是"引擎自己记着那台设备"。r96 的
 *   Pal=0 调了非 Ext 入口却没设 EDX ⇒ 引擎拿残留 EDX 当 DIBENGINE, 从
 *   deBitmapInfo(DDK 布局 +0x20)读出的远指针非法, 写 +0x24 时 GP。这条
 *   链与 DDK 头 ddk/dibeng.h 的字段布局逐字吻合(deFlags=+0x1C,
 *   deBitmapInfo=+0x20, deBeginAccess=+0x24)—— 不再是猜测。
 * ⇒ r97 三条路(都可 A/B):
 *   Pal=1(默认) Ext 路, 尾部显式 PDEV: r96 的 Enable 里那次
 *               DIB_SetPaletteTranslateExt(NULL, PDEV) 就是同一形态, 且
 *               r96 日志证明它正常返回("xlat call returned") —— 本机已
 *               验证过的形态, 风险最低。
 *   Pal=0       DIBLINK 形态: 非 Ext + 跳板注入 EDX=lpDriverPDevice。
 *               Edx=0 关掉注入即复现 r96 的 0367:4036。
 *   Pal=2       完全不进引擎: 自己把颜色写进 deBitmapInfo->bmiColors
 *               (并置 CTCHANGE), 再刷 DAC —— 判据兼退路。
 * 三条路后面都照旧刷硬件 DAC, 所以切换只影响"引擎知道不知道",
 * 不影响屏幕颜色。 */
static WORD g_runPalMode = 1;   /* r97-marker: Pal default = 1 (Ext) */

/* --- r97: 引擎非 Ext 入口的 EDX 协议(汇编侧 DIBJUMP 用) -------------------
 * g_runEdx   = 1: 跳板在远跳进 DIBENG 之前把 lpDriverPDevice 装进 EDX。
 *              这是 DIBLINK 在小驱动里做的事; 引擎所有"非 Ext"导出的头
 *              6 字节跳板都把它当第 4 个参数用(见上)。
 * g_edxCount = 汇编侧每注入一次 +1; 日志里落盘, 用来证明"注入真的执行了"
 *              (若某次调用后计数没涨, 说明走的不是注入路径)。
 * 两个变量都必须是非 static 全局: vddapi16.asm 用 extrn 直接引用。 */
WORD g_runEdx   = 1;
WORD g_edxCount = 0;

/* --- r96: Enable 里那次 DIB_SetPaletteTranslateExt 的位置 -----------------
 * 【r81..r95 一直把它放在"填 BITMAPINFO/DIBENGINE 之前"】也就是拿一本
 * 还是垃圾(实测是 DIBENG 常驻名表)的缓冲区当 PDEV 交给引擎 —— 引擎
 * 要么整本读错, 要么把翻译表写进野指针。DDK 的次序是死的:
 *   ① 填 BITMAPINFO(表面几何) → ② CreateDIBPDevice 建 PDEVICE
 *   → ③ 补 deBeginAccess/deEndAccess → ④ 之后才是引擎的其它调用。
 * 本轮把它挪到 ② 之后(Xlat=1, 默认), Xlat=0 则整段跳过(回到"根本不
 * 告诉引擎调色板翻译"的最小状态)。 */
static WORD g_runXlat    = 1;

/* --- r96: DDI 入口的运行环境自证(FS/GS) ----------------------------------
 * 0367 段的机器码里有 `64 88 05`(mov fs:[di],al)这类【FS 相对】访问
 * (见 r95 日志 DUMP sel=0367 off=2B60)。FS 在 Win9x 的 16 位环境里不是
 * 随便什么值: 引擎的绘图/调色板内层循环靠它直接碰表面。若进引擎时 FS
 * 非法(0 或已失效的选择子), 一条 fs: 访问就是 0D。这两个全局由汇编侧
 * 的 DibEnvProbe 回填(见 vddapi16.asm), 在 DDI 上下文里读一次即可。 */
WORD g_envFS = 0;
WORD g_envGS = 0;

/* --- DDI 首次调用追踪(r76c 诊断) -----------------------------------------
 * 【为何要它】r76b 的 0D 蓝屏地址是 0427:00002B70 —— 段选择子 0427
 * (LDT 索引 132)不是本驱动(本驱动代码段 = 索引 99), 偏移 0x2B70 也超出
 * 本模块代码段长度(0x2774)。也就是说【故障发生在别的模块内部】
 * (GDI / DIBENG / KERNEL), 我们自己的代码已经把 Enable 走到
 * "PhysicalEnable ok" 之后。剩下能出问题的只有:
 *   STOP_IO_TRAP / DIB_Enable(PDEVICE 相) / BITMAPINFO+结构填充 /
 *   RepaintFunc 查询 / 或者 Enable 返回后 GDI 立刻来调某个 DDI
 *   (BitBlt/Control/Inquire/CheckCursor... → 转发进 DIBENG)。
 * 光靠"最后一行日志"分不清, 所以这里给【每个转发入口】都装一个
 * "首次被调用就记一行"的探针:
 *   · 汇编侧的 DIBFWD/DIBTHK 转发器调用 TRACESLOT(slot) —— 每槽只记一次;
 *   · C 侧的 BeginAccess/EndAccess/光标/调色板等调用点调用 TraceOnce。
 * 崩溃时日志最后一行就是"死在哪个入口", 一轮就能定性。
 * 【开销】每个入口只多 2 条指令(cmp byte/ jne), 每槽只写一次文件。 */
#define TRACE_ARRAY_SIZE    64
#define TRACE_SLOT_BEGINACCESS 36
#define TRACE_SLOT_ENDACCESS   37
#define TRACE_SLOT_SETPALETTE  38
#define TRACE_SLOT_SETCURSOR   39
#define TRACE_SLOT_MOVECURSOR  40
#define TRACE_SLOT_CHKCURSOR   41
#define TRACE_SLOT_REPAINT     42
#define TRACE_SLOT_RESID       43
#define TRACE_SLOT_REPDIS      44
#define TRACE_SLOT_EXTTEXTOUT  45
#define TRACE_SLOT_DISABLE     46  /* 【r76d 重点】序号 4 是本次唯一
                                    * 未装探针的"有副作用"导出: 它会
                                    * VDD_DRIVER_UNREGISTER + 把硬件切回
                                    * 80x25 文本模式。若 GDI 在 Enable 之后
                                    * 又调了 Disable(日志出现本槽), 说明 GDI
                                    * 认为这次使能不算数, 而它随后继续按
                                    * "已使能"使用显示设备 —— 崩溃在 GDI 内部
                                    * 且没有任何其它 DDI 探针, 与该情形完全吻合。*/
#define TRACE_SLOT_REENABLE    47
#define TRACE_SLOT_VALIDATE    48
#define TRACE_SLOT_PALCHK      49  /* r96: SetPalette 入口的参数帧自证 */

/* 【r96: 为什么这里必须给显式初值】r87..r95 的日志里【从来没有】
 * "DDI first call -> DIB_xxx" 这一类行, 而同一批构建里 C 侧的 TraceOnce
 * 行却正常出现。两条路唯一的差别是: 汇编侧 TRACESLOTMAC 先读
 * `cmp byte ptr [_g_traceDone + slot], 0`, 为 0 才记。此数组原本是
 * 无初值的 BSS —— 万一加载器/链接器没有把它清零(或它被别的写越界踩到),
 * 探针就会【静默地什么都不记】, 看起来像"GDI 从没调过任何 DDI"。
 * 显式 `= { 0 }` 把它从 BSS 挪进 _DATA(文件里有初值), 这一整类不确定性
 * 当场消失; 同时在 Enable 末尾把整个位图打成一行, 一眼可验。 */
BYTE g_traceDone[TRACE_ARRAY_SIZE] = { 0 };
/* 【r113】g_retDone —— "这一槽已经从 DIBENG 返回过一次了吗"。
 *   与 g_traceDone 严格配对: 汇编侧 DIBSTUB 在【被调方返回之后】读它
 *   (`cmp byte ptr [_g_retDone + slot], 0`, 为 0 才进 C), 由 TraceSlotRet
 *   置 1。于是每个槽最多多出一行日志, 而这一行正是把"崩溃发生在 DIBENG
 *   调用【内部】还是【之后】"切成两半的唯一证据:
 *     · 崩溃日志的最后一行是 `MARK DIB_DibToDevice`(入口标记) ⇒ 死在
 *       引擎那次调用里(或我们进引擎的路上);
 *     · 最后一行是 `DDI ret -> DIB_DibToDevice` ⇒ 引擎已经正常返回,
 *       死在 GDI/VDD 的后续动作里, 与我们这一段无关。
 *   同样显式 = { 0 }(理由见 g_traceDone 的注: 别让它落在没清零的 BSS 上)。
 *   【尺寸】用 TRACE_ARRAY_SIZE(64) 而不是 DIBFN_COUNT: 本数组要在这里
 *   声明(紧挨 g_traceDone), 而 DIBFN_COUNT 在下面才定义; 汇编侧只按
 *   DIB 槽号 0..36 索引, 64 字节既够又与前一个位图同尺寸, 便于对照。  */
BYTE g_retDone[TRACE_ARRAY_SIZE] = { 0 };
/* 位图字符串(必须是【全局】: 本工程 -mm + SS≠DS, 栈上数组的近指针
 * 是 DS 相对的, 交给 DIAG 会打到 DGROUP 里去 —— 见 ROADMAP 的自查纪律)。
 * 只在诊断构建里存在: release 下整个位图落盘都编掉了, 留着会多一条 W202。*/
#ifdef VBE9X_DIAG
static char g_traceStr[64];
#endif

/* --- USER 重绘回调与重绘抑制状态(scrsw 逻辑) ---------------------------- */
FARPROC     RepaintFunc   = 0;  /* USER.275, 懒加载                       */
static BOOL bNoRepaint    = 0;  /* USER 要求我们暂停重绘                  */
static BOOL bPaintPending = 0;  /* 抑制期间有未执行的重绘                 */
/* [r134] How many times we actually asked USER for a whole-desktop repaint.
 *   Why a counter and not just a log line: ./133's log drops 1600 lines at the
 *   gate (`LOG skip Reent=0640`), and a TraceOnce whose first call lands in a
 *   dropped window is lost forever -- so "was the desktop ever repainted?" was
 *   unanswerable from the text log alone.  This counter is live in every build
 *   (PHASEV + the CB ring), so the next round reads it and not a maybe. */
static WORD g_repaintN    = 0;

/*==========================================================================
 *  DIB 引擎入口的动态解析(彻底取代静态序号导入)
 *
 *  【为什么不再静态导入 DIBENG】静态导入表里的每个序号, 在加载期都要
 *  由 KRNL386 逐一在本机 DIBENG.DLL 的名表/序号表中查到地址(导入修正),
 *  任何一个查不到 → 整个模块被拒载, bootlog 只有一句
 *  "LoadFail = omninine.drv Failure code is 0000", 入口永远不会被调用,
 *  连写一行日志的机会都没有。真机实测正是如此(DIBENG 已 LoadSuccess,
 *  我们的入口探针却毫无动静)。改为运行期动态解析后:
 *    1. 每个入口按【名字】解析(任何版本的 DIBENG.DLL 都导出了名字),
 *       与 DIBENG 的具体版本彻底解耦;
 *    2. 某个入口缺失只是"记一行日志并拒载", 而不是无声消失;
 *    3. 加载器看到的导入表只剩 KERNEL 的两个序号(47/50,
 *       在售驱动 vesamini.drv 实证存在), 加载期不再依赖 DIBENG。
 *
 *  【为什么解析推迟到首次转发/Enable(懒解析)】本驱动由 KRNL386 引导期
 *  直接加载(bootlog 中 omninine.drv 与 system.drv/keyboard.drv/mouse.drv
 *  同列, 在 "Initializing KERNEL" 段), 此阶段 GDI 尚未启动、DIBENG.DLL
 *  根本不在内存: GetModuleHandle("DIBENG") 只能得到 0, 而 LoadLibrary
 *  兜底会真的去装载 DIBENG —— 该模块依赖尚未就绪的 GDI 环境, 实测在
 *  KERNEL 内部 GP 蓝屏(error 0D:0327:00000683, ./5 构建)。在售参考
 *  vmdisp9x 的 DriverInit 同样完全不碰 DIBENG。因此:
 *    · DrvInitCore 只把 g_dibfn[] 填成 DIBFNSTUB 保底;
 *    · 首次转发(GDI 期, DIBENG 已随 GDI 驻留)由 vddapi16.asm 的转发
 *      宏查 _g_dibfnReady, 未解析则调 DibEnsureResolved() 现场解析。
 *  单线程协作式调度下解析不可重入, 无竞争问题。
 *
 *  【g_dibfn 的布局与 vddapi16.asm 严格绑定】
 *    0..19  纯透传转发(DIBFWD):  GDI 入口 → DIB_同名函数
 *    20..26 插尾参转发(DIBTHK):  GDI 入口 → DIB_同名Ext 函数
 *    27..35 C 侧直接调用点
 *    36     r96 新增: C 侧直接调用点(DIB_SetPalette, 非 Ext, 3 参) ——
 *           序号 22 的 DDK 标准路由目标, 见 g_runPalMode 的长注释
 *  汇编侧以 jmp dword ptr [_g_dibfn + slot*4] 间接跳转; 改动顺序
 *  必须两侧同步!
 *========================================================================*/
#define DIBFN_COUNT 37

DWORD g_dibfn[DIBFN_COUNT];     /* DrvInitCore 先填 DIBFNSTUB 保底,      */
                                /* DibEnsureResolved 成功后填真实地址     */

/* 非 0 = g_dibfn 已填真实地址。vddapi16.asm 的 DIBTHK/DIBFWD/CDIB
 * 转发宏在 jmp 前检查它, 为 0 则先调 DibEnsureResolved() 懒解析。
 * 定义成全局(非 static)是给汇编 extrn 用。                              */
int     g_dibfnReady = 0;

/* 每一项: 导出名字(唯一查询键) + 标准序号(仅存档参考, 不再使用)。
 * 【为何砍掉序号兜底】16 位 GetProcAddress 的序号约定是"远指针段字为 0",
 * 而 C 编译器按远串原型传递时压栈的是 DS:偏移(段≠0), KERNEL 会把
 * 序号当字符串指针解引用 —— 在真机上就是 KERNEL 内 GP。DIBENG 各版本
 * 均导出全部名字(见下), 名字查询足够, 不再冒这个险。
 * 【名字必须带 DIB_ 前缀】DIBENG.DLL 的真实导出名是 DIB_BitBlt、
 * DIB_Enable 这类带前缀的名字(见 ddk/dibeng.lbc 与 ddk/dibeng.h),
 * 裸名 "BitBlt" 在任何版本上都解析不到。                        */
static const struct { const char *name; WORD ord; } g_dibfnTab[DIBFN_COUNT] =
{
    /* --- 0..19: DIBFWD 纯透传(序号 = DIBENG 标准序号) ----------------- */
    { "DIB_BitBlt",              1   },     /*  0 */
    { "DIB_ColorInfo",           2   },     /*  1 */
    { "DIB_Control",             3   },     /*  2 */
    { "DIB_EnumDFonts",          6   },     /*  3 */
    { "DIB_Output",              8   },     /*  4 */
    { "DIB_Pixel",               9   },     /*  5 */
    { "DIB_StrBlt",              11  },     /*  6 */
    { "DIB_ScanLR",              12  },     /*  7 */
    { "DIB_DeviceMode",          13  },     /*  8 */
    { "DIB_GetCharWidth",        15  },     /*  9 */
    { "DIB_DeviceBitmap",        16  },     /* 10 */
    { "DIB_FastBorder",          17  },     /* 11 */
    { "DIB_SetAttribute",        18  },     /* 12 */
    { "DIB_CreateDIBitmap",      20  },     /* 13 */
    { "DIB_DibToDevice",         21  },     /* 14 */
    { "DIB_StretchBlt",          27  },     /* 15 */
    { "DIB_StretchDIBits",       28  },     /* 16 */
    { "DIB_SelectBitmap",        29  },     /* 17 */
    { "DIB_BitmapBits",          30  },     /* 18 */
    { "DIB_Inquire",             101 },     /* 19 */
    /* --- 20..26: DIBTHK 插尾参(目标是 Ext 版本) ------------------------ */
    { "DIB_RealizeObjectExt",         400 }, /* 20 */
    { "DIB_EnumObjExt",               402 }, /* 21 */
    { "DIB_DibBltExt",                401 }, /* 22 */
    { "DIB_GetPaletteExt",            406 }, /* 23 */
    { "DIB_SetPaletteTranslateExt",   407 }, /* 24 */
    { "DIB_GetPaletteTranslateExt",   408 }, /* 25 */
    { "DIB_UpdateColorsExt",          404 }, /* 26 */
    /* --- 27..35: C 侧直接调用 ------------------------------------------ */
    { "DIB_Enable",                   5   }, /* 27 */
    { "DIB_ExtTextOut",               14  }, /* 28 */
    { "DIB_BeginAccess",              105 }, /* 29 */
    { "DIB_EndAccess",                106 }, /* 30 */
    { "DIB_SetPaletteExt",            405 }, /* 31 */
    { "DIB_SetPaletteTranslateExt",   407 }, /* 32 (与槽位 24 同一函数) */
    { "DIB_SetCursorExt",             102 }, /* 33 */
    { "DIB_MoveCursorExt",            103 }, /* 34 */
    { "DIB_CheckCursorExt",           104 }  /* 35 */
    ,
    /* --- 36: r96 —— DDK 标准路由的非 Ext 版调色板入口 ------------------ */
    { "DIB_SetPalette",               22  }  /* 36 */
};

/* --- 给汇编跳板的 DIBENG DGROUP + 调用方现场(r94/r95) --------------------
 * 【r95 实测修正】DIBENG 的导出函数【自己不装 DS】: 装载器为"需要装载数据段"
 * 的导出造了一个桩(本次实测到的就是 DIB_Enable 的 035F:0182), 桩做的是
 *     mov ax, <DIBENG 的 DGROUP>   ; 装 AX
 *     ...(装 DS)...
 *     jmp far <真实入口>
 * 静态导入(CreateDIBPDevice)由同一套机制代劳 —— 这就是"唯一没崩的调用恰好
 * 是 DIB_Enable 与 CDPD"的原因: 它们走的是桩, 桩替我们装好了 DS。
 * 我们动态解析到的是 36 个入口里的 35 个【原始入口】, 所以必须自己把 DS
 * 装成 wDibDgroup(见 vddapi16.asm 的 DIBJUMP)。wDibDgroup 由 DibDgroupProbe
 * 从装载器桩的立即数里取回(模块数据库那条路取出的是文件偏移, 见该函数)。
 * 【g_dibDgTrusted】只有"来自已验证的装载器桩"的值才允许装进 DS —— 探针
 * 没找到桩时保持 r93 行为(不装 DS), 宁可退化也不拿垃圾选择子去装段寄存器。 */
WORD    wDibDgroup    = 0;      /* DIBENG.DLL 的 DGROUP 选择子          */
WORD    g_dibDgTrusted = 0;     /* r95: 1 = 上面这个值来自已验证的桩    */

/* --- r95: 转发器的"调用方现场"LIFO 区 ------------------------------------
 * 【为什么不能用栈】pascal 约定下内存布局是 [返回地址][最后一个参数]…,
 * 而转发器必须把返回地址换成自己的桩 —— 于是 SP 必须【正好】指向桩, 中间
 * 多压一个字都会被 DIBENG 当成参数读走, 而少一个字它就会把别的字当返回
 * 地址。r86..r94 的实现把保存现场 push 在桩之后, SP 因此指向保存的 BP:
 * DIBENG 的 "retf N" 把 [BP][DS] 当返回地址, 直接跳进调用方的数据段 ——
 * 蓝屏地址 0607:00001B5C 正好是 (GDI 的 BP):(GDI 的 DS) 这种形态。
 * r85 之前没有桩(尾跳), 崩的是"调用方 DS 被 DIBENG 的 mov ds,ax 改掉"
 * ——同一个 ABI 病灶的两种表现, 所以两个都修: 现场挪到 DGROUP,
 * DS 由桩自己还回去(见 vddapi16.asm 的 DIBSAVE/DIBSTUB)。
 * 【回绕取模】深度 8、索引不回绕而是取模 —— 进出天然配对, 没有失败分支;
 * DIBENG → 我们的回调 → DIBENG 这种嵌套也在同一层序里配对。            */
#define DIBFWD_SAVE_MAX 8
WORD    g_dibSaveIdx = 0;                       /* 下一个写入槽(不回绕)  */
WORD    g_dibSaveDS [DIBFWD_SAVE_MAX];          /* 每层 调用方 DS        */
WORD    g_dibSaveOff[DIBFWD_SAVE_MAX];          /* 每层 调用方返回偏移   */
WORD    g_dibSaveSeg[DIBFWD_SAVE_MAX];          /* 每层 调用方返回段     */

/* --- r98: 跳板"插入的那个尾部额外参数"的实际字节 ---------------------------
 *  【为什么要有这两个变量】槽 20..26 的转发器(DIBTHK)会往参数帧里插一个
 *  4 字节尾参(设备指针 / _wPalettized)。r98 定案的 0D @ 0367:00003FA5 就是
 *  "插进去的值不对": 老代码在装 DS 之前就用 DS 相对寻址读这个变量, 读到的是
 *  【调用方数据段里同一偏移处】的内容。修好之后再把它原样落盘, 日志里就能
 *  直接看到"到底插了什么", 不需要再靠推理:
 *      · 等于我们的 PDEV(如 2056:0000) → DS 从一开始就是本模块的;
 *      · 别的选择子:别的偏移          → 老病灶(或又出现同类问题)。
 *  由 vddapi16.asm 的 TRACESLOTMAC 在每槽首次调用时从 param 抄进来。      */
WORD    g_thunkPOff = 0;                        /* 尾参低字(远指针 = 偏移) */
WORD    g_thunkPSeg = 0;                        /* 尾参高字(远指针 = 选择子) */

/* --- r84: 让 DIB 引擎自己建 PDEVICE(CreateDIBPDevice, 序号 300) ----------
 *  【为什么要加这条路】在售参考驱动 boxv9x 根本不手填 DIBENGINE, 而是调
 *  DDK 的 CreateDIBPDevice(lpInfo, lpDevice, ScreenSelector:0, wFlags);
 *  我们手填的字段与 DDK 文档逐条核对都对(见下方填充处的注释), 可是
 *  r76b..r83 七轮蓝屏都发生在"Enable 返回之后、引擎自己的代码里" —— 只要
 *  有一处"文档没写、引擎却在读"的东西不对, 崩的就是引擎。让引擎自己建这本
 *  结构, 这一整类未知当场消失: 我们不再"猜"引擎想要什么。
 *  【为何入参经 DGROUP】dibeng.lib 里的名字是混合大小写 CreateDIBPDevice,
 *  而 Watcom 对 WINAPI(__pascal) 一律生成大写符号, C 侧直接声明会链接
 *  不上; 调用放进 vddapi16.asm 的 CDPD(参数经 DGROUP 传递, 与本工程
 *  FBCLEAR/FBTEST 的既有手法一致)。
 *  【返回值】DDK 明写结果在 EAX(不是 DX:AX); 汇编侧只取低 16 位:
 *  0 = 失败(自动回退到下面的手填值), 非 0 = 位图字节数 = 成功。       */
extern WORD __far __pascal Cdpd( void );
DWORD CdpdInfo   = 0;           /* lpInfo  (BITMAPINFO 远指针)         */
DWORD CdpdDevice = 0;           /* lpDevice(本 PDEVICE)                 */
DWORD CdpdBits   = 0;           /* lpBits = 屏选择子:0                  */
WORD  CdpdFlags  = 0;           /* wFlags                               */
static WORD g_cdpdRc   = 0;     /* CDPD 的返回值(落盘用)                */
static WORD g_runCdpd  = 1;     /* r84: system.ini [omninine] CDPD, 默认 1 = 走引擎自建 */

/* --- C 侧调用点: 经 vddapi16.asm 的 DIBX_* 跳板 --------------------------
 * C 代码无法在执行调用前设置 AX, 故 C 侧不直接调 g_dibfn 里的地址,
 * 而是调这些 pascal far 跳板: 跳板只做 "AX←wDibDgroup, 然后
 * jmp dword ptr [_g_dibfn+slot*4]", 栈上参数原样透传。用对象式宏把
 * 12 处调用点原封不动地改道到跳板(签名逐条取自 DDK dibeng.h)。      */
extern UINT  WINAPI DIBX_Enable( LPVOID lpDevice, WORD wStyle,
                                 LPSTR lpDeviceType, LPSTR lpOutputFile,
                                 LPVOID lpStuff );
extern DWORD WINAPI DIBX_ExtTextOut( LPVOID lpDestDev, WORD wDestXOrg,
                                     WORD wDestYOrg, LPRECT lpClipRect,
                                     LPSTR lpString, int wCount,
                                     LPFONTINFO lpFontInfo,
                                     LPDRAWMODE lpDrawMode,
                                     LPTEXTXFORM lpTextXForm,
                                     LPSHORT lpCharWidths,
                                     LPRECT lpOpaqueRect, WORD wOptions );
extern VOID  WINAPI DIBX_BeginAccess( LPVOID lpDevice, WORD wLeft,
                                      WORD wTop, WORD wRight,
                                      WORD wBottom, WORD wFlags );
extern VOID  WINAPI DIBX_EndAccess( LPVOID lpDevice, WORD wFlags );
extern VOID  WINAPI DIBX_SetPaletteExt( WORD wStart, WORD wCount,
                                        LPVOID lpPalette, LPVOID lpDevice );
extern VOID  WINAPI DIBX_SetPaletteTranslateExt( LPWORD lpIndexes,
                                                 LPVOID lpDevice );
extern VOID  WINAPI DIBX_SetCursorExt( LPVOID lpCursor, LPVOID lpDevice );
extern VOID  WINAPI DIBX_MoveCursorExt( WORD absX, WORD absY,
                                        LPVOID lpDevice );
extern VOID  WINAPI DIBX_CheckCursorExt( LPVOID lpDevice );
/* 【r96】非 Ext 版调色板入口(DDK 路由: 序号 22 → DIB_SetPalette, 3 参)。
 * 多出来的"设备"由引擎自己记着(CreateDIBPDevice 建的), 因此跳板
 * 不需要插尾参 —— 与 DIBFWD 同构, 纯透传。 */
extern VOID  WINAPI DIBX_SetPalette( WORD wStart, WORD wCount,
                                     LPVOID lpPalette );

/* 【r96】汇编侧的环境探针: 把当前 FS/GS 存进 g_envFS/g_envGS。
 * 16 位 C 读不到段寄存器, 只能借汇编(call 无参, pascal 无参)。 */
extern void  __far __pascal DibEnvProbe( void );

#define DIB_Enable                  DIBX_Enable
#define DIB_ExtTextOut              DIBX_ExtTextOut
#define DIB_BeginAccess             DIBX_BeginAccess
#define DIB_EndAccess               DIBX_EndAccess
#define DIB_SetPaletteExt           DIBX_SetPaletteExt
#define DIB_SetPaletteTranslateExt  DIBX_SetPaletteTranslateExt
#define DIB_SetCursorExt            DIBX_SetCursorExt
#define DIB_MoveCursorExt           DIBX_MoveCursorExt
#define DIB_CheckCursorExt          DIBX_CheckCursorExt

/*==========================================================================
 *  VBE 模式缓存表
 *
 *  【为何缓存】DrvInitCore 阶段(INT 2F/BIOS 都安全)一次性枚举全部可用
 *  模式; Enable/ReEnable 阶段只查表, 不再碰 BIOS —— GDI 临界状态下调用
 *  实模式 VBE BIOS 是不必要的风险。
 *========================================================================*/
typedef struct {
    WORD    mode;               /* VBE 模式号                           */
    WORD    width, height;      /* 像素几何                             */
    WORD    bpp;                /* 位深                                 */
    WORD    pitch;              /* 每行字节数(BIOS 权威值)              */
} VBEMODE;

#define MAX_CACHED_MODES    64
static VBEMODE  g_modes[MAX_CACHED_MODES];
static int      g_nModes = 0;

/*==========================================================================
 *  8bpp 默认调色板(DDK 标准 20 保留色 + 中间非静态色)
 *  数值逐条取自 DDK 示例/vmdisp9x enable.c, 是 GDI 期望的固定布局:
 *  [0..9] 静态色, [10..245] 全 NONSTATIC, [246..255] 静态色。
 *========================================================================*/
static const DIBColorEntry DIB8ColorTable1[] = {    /* 前 10 项           */
    /* blue  red  green flags */
    { 0   , 0   , 0   , 0          },
    { 0   , 0   , 0x80, 0          },
    { 0   , 0x80, 0   , 0          },
    { 0   , 0x80, 0x80, 0          },
    { 0x80, 0   , 0   , 0          },
    { 0x80, 0   , 0x80, 0          },
    { 0x80, 0x80, 0   , 0          },
    { 0xC0, 0xC0, 0xC0, MAPTOWHITE },
    { 0xC0, 0xDC, 0xC0, MAPTOWHITE | NONSTATIC },
    { 0xF0, 0xCA, 0xA6, MAPTOWHITE | NONSTATIC }
};

static const DIBColorEntry DIB8ColorTable2[] = {    /* 中间 236 项的模板  */
    { 0, 0, 0, NONSTATIC }
};

static const DIBColorEntry DIB8ColorTable3[] = {    /* 后 10 项           */
    { 0xF0, 0xFB, 0xFF, MAPTOWHITE | NONSTATIC },
    { 0xA4, 0xA0, 0xA0, MAPTOWHITE | NONSTATIC },
    { 0x80, 0x80, 0x80, MAPTOWHITE },
    { 0   , 0   , 0xFF, 0          },
    { 0   , 0xFF, 0   , MAPTOWHITE },
    { 0   , 0xFF, 0xFF, MAPTOWHITE },
    { 0xFF, 0   , 0   , 0          },
    { 0   , 0xFF, 0xFF, 0          },
    { 0xFF, 0xFF, 0   , MAPTOWHITE },
    { 0xFF, 0xFF, 0xFF, MAPTOWHITE }
};

/*==========================================================================
 *  前向声明
 *========================================================================*/

/* DriverInit 的 C 本体, 由 vddapi16.asm 的 DriverInit 入口调用。
 * 【必须 __far __pascal】pascal 约定使符号为 DRVINITCORE(纯大写),
 * 与汇编侧 extrn DRVINITCORE:far 精确匹配, MSVC/Watcom 皆然。        */
int  __far __pascal DrvInitCore( void );

/* vddapi16.asm 提供的两项(均 pascal far, 符号大写):
 *   DIBFNSTUB          —— g_dibfn 占位桩(加载期填表用)
 *   DIBENSURERESOLVED  —— 即下方的 DibEnsureResolved, 转发宏懒解析入口  */
extern void __far __pascal DibFnStub( void );

/* DDI 导出(序号称见 omninine.def) */
UINT  WINAPI __loadds Enable( LPVOID lpDevice, UINT style, LPSTR lpDeviceType,
                              LPSTR lpOutputFile, LPVOID lpStuff );
UINT  WINAPI __loadds Disable( LPVOID lpDevice );
UINT  WINAPI __loadds ReEnable( LPVOID lpDevice, LPGDIINFO lpInfo );
UINT  WINAPI __loadds SetPalette( UINT wStartIndex, UINT wNumEntries,
                                  LPVOID lpPalette );
DWORD WINAPI __loadds GetDriverResourceID( WORD wResID, LPSTR lpResType );
VOID  WINAPI __loadds UserRepaintDisable( BOOL bDisable );
UINT  WINAPI __loadds ValidateMode( DISPVALMODE FAR *lpValMode );
DWORD WINAPI __loadds ExtTextOut( LPDIBENGINE lpDestDev, WORD wDestXOrg,
                                  WORD wDestYOrg, LPRECT lpClipRect,
                                  LPSTR lpString, int wCount,
                                  LPFONTINFO lpFontInfo, LPDRAWMODE lpDrawMode,
                                  LPTEXTXFORM lpTextXForm, LPSHORT lpCharWidths,
                                  LPRECT lpOpaqueRect, WORD wOptions );
WORD  WINAPI __loadds SetCursor_driver( CURSORSHAPE __far *lpCursor );
void  WINAPI __loadds MoveCursor( int absX, int absY );
void  WINAPI __loadds CheckCursor( void );

/* VDD 回调: DOS 全屏 → 桌面切换回来时由主 VDD 调用(③ 环注册步骤 8)。
 * 常驻 _TEXT 且 __loadds(代码段加锁已移除, 见 DrvInitCore 注释)。  */
void __far __loadds RestoreDesktopMode( void );
/* r99: "推迟重建/自愈" 共用的入口钩子 —— 在我们自己的 ring-3 上下文里
 * 执行被推迟的模式重建(Restore=2 或 Heal=1 置的 pending)。         */
void __far __pascal MaybeDeferredRestore( void );
/* r99: 判定是否该自愈(轮询 VxD 侧"显示驱动被 DISABLING 过"的迹象) */
void __far __pascal MaybeHeal( void );

/* DIBENGINE.deBeginAccess/deEndAccess 回调(常驻 _TEXT) */
VOID  WINAPI __loadds BeginAccess( LPPDEVICE lpDevice, WORD wLeft, WORD wTop,
                                   WORD wRight, WORD wBottom, WORD wFlags );
VOID  WINAPI __loadds EndAccess( LPPDEVICE lpDevice, WORD wFlags );
/* 【r105】在安全上下文里落盘"光标排斥临界区"的记账(纯读, 见 XclDump) */
static void XclDump( void );
/* r102: 第二形态(INI Access2=1) —— 两个空回调, 见定义处长注释 */
VOID  WINAPI __loadds NullBeginAccess( LPPDEVICE lpDevice, WORD wLeft, WORD wTop,
                                       WORD wRight, WORD wBottom, WORD wFlags );
VOID  WINAPI __loadds NullEndAccess( LPPDEVICE lpDevice, WORD wFlags );

/* 模块内部 */
/* [r406] SettlePoll —— 4F02 之后不要只读一次。
 *   硬件/VDD 的模式切换是异步的: 返回 004Fh 只说明"BIOS 接受了请求", 不代表
 *   读取 4F03h 时它已经生效。带日志的构建里, 紧随其后的文件写恰好把这段
 *   时间"等"完了; 去掉日志后同样的代码会读到旧值 —— 这是两种构建行为不同
 *   的根源。这里改成"最多 polls 次, 直到 4F03h 与目标一致", 不依赖任何延时。
 *   SettlePoll=0 关闭(退回"读一次"), 便于 A/B。 */
static WORD g_runSettlePoll = 4;

static int ModeSettled( uint16_t wanted )
{
    int i;
    for (i = 0; i < (int)g_runSettlePoll; i++)
    {
        if (VBE_ProbeCurrentMode())
        {
            if ((WORD)(VBE_ProbeModeValue() & 0x3FFF) == (WORD)(wanted & 0x3FFF))
                return 1;
        }
    }
    return 0;
}

static int  __far SetDisplayMode( int bFullSet );
static WORD ProtClrBeforeModeSet( const char *why );   /* r132 */
static void CrtRelatchAfterRebuild( const char *why );              /* r133 */
static void HwRegSampleAtRebuild( const char *why );                /* r138 */
static void CrtSnapSession( const char *why, int force );   /* r132 / r135 */
static int  PhysicalEnable( void );
static void PhysicalDisable( void );
static int  ResolveDIBEngine( void );
static WORD GetModuleDgroupSel( HINSTANCE hMod );
static WORD LazyGPPI( const char *sec, const char *key, int def );
static void ReadDisplayConfig( void );
static void ReadRunProfile( void );          /* r81: system.ini [omninine] */
static int  BuildModeTable( void );
static WORD FindVbeMode( WORD w, WORD h, WORD bpp );
static void RepaintScreen( void );
static void TraceOnce( WORD slot, const char *msg );   /* r76c 诊断, 后方定义 */

/*==========================================================================*/
#pragma code_seg( "_INIT" )
/*  以下函数只在加载/使能/模式切换时执行, 放在 MOVEABLE 的 _INIT 段,
 *  不占用宝贵的常驻(锁定)内存。                                        */
/*==========================================================================*/

/* --- r82: 模式被拒的原因码(能力卡直方图用) -------------------------------
 * 【为什么要它】"这台机器上能用的模式只有 1 个"是最难远程诊断的一类问题:
 * 到底是卡不支持、掩码不对、还是被我们自己的上限挡掉了? 逐个模式记下
 * 【第一个不满足的条件】并统计, 一行日志就能看出是"卡的问题"还是
 * "我们的策略问题"。这台机器通用性的真实边界由此可测量。          */
#define MR_OK     0
#define MR_ATTR   1     /* 缺 Supported/Graphics/LFB(老卡: 无线性帧缓冲) */
#define MR_MODEL  2     /* 色彩模型不是 Packed(8bpp)/Direct(16/24/32bpp) */
#define MR_BPP    3     /* 位深不在 8/16/24/32(15bpp、30bpp 等)          */
#define MR_MASKS  4     /* 掩码/位序不符 Windows DIB(红蓝对调、555 等)   */
#define MR_PITCH  5     /* BytesPerScanLine = 0                          */
#define MR_SIZE   6     /* 小于 640x480                                  */
#define MR_VRAM   7     /* 帧缓冲超出显存总量                            */
#define MR_CAP    8     /* 帧缓冲超出 32MB 映射上限(高分屏会撞到)        */
#define MR_N      9

#ifdef VBE9X_DIAG
/* 直方图行名(仅诊断构建需要; 发布构建里连它一起编掉, 免得留下未引用的静态数组) */
static const char * const g_mrName[MR_N] = {
    "ok", "attr", "model", "bpp", "masks", "pitch", "size", "vram", "cap"
};
#endif

/*
 * IsModeOK —— 判定一个 VBE 模式是否可提供给 Windows。
 *   规则(v2 通用化定版 + r76 掩码位序补强, 详见 drv.h 属性宏):
 *     - 属性必须同时具备 Supported+Graphics+LFB(无 LFB 的 banked 模式
 *       直接拒绝: 本驱动只做线性帧缓冲渲染)
 *     - bpp ∈ {8,16,24,32}; 15bpp(555)被排除 —— Win9x GDI/DIBENG
 *       用不好它
 *     - 【r76 位序】Windows 的 DIB 真彩色格式是 B 在低位、R 在高位
 *       (16bpp = RGB565: R 在 bit11/绿 bit5/蓝 bit0; 24/32bpp = 内存
 *       里 B,G,R 顺序)。VBE 允许显卡把红蓝对调(RedFieldPosition=0),
 *       而 DIBENG 只认 Windows 的格式 —— 位序不对就是红蓝互换的花屏。
 *       这里按 4F01 报的掩码位序过滤, 只放行与 Windows DIB 一致的模式。
 *     - 16bpp 必须是 565(GreenMaskSize==6)
 *     - 色彩模型: 8bpp→PackedPixel(4), 其余→DirectColor(6)
 *     - ≥640x480; 帧缓冲不超显存且不超 32MB 映射上限
 *   【r82】多了 pReason 出参: 返回第一个不满足的条件码(见 MR_*)。
 *   出参必须写 __far *: 调用方传的是 &栈局部量, 而本工程的内存模型下
 *   编译器把栈局部量的地址当成 __far 指针(SS 相对), 把它塞进近指针形参
 *   只会得到一条 W112("指针被截断")警告, 段基址被丢掉 → 被调方把原因码
 *   写进 DGROUP 的对应偏移处, 调用方的变量仍是初值。               */
static int IsModeOK( MODEINFOBLOCK __far *info, DWORD vramSize,
                     WORD __far *pReason )
{
    DWORD fbNeed;
    WORD  why = MR_OK;

    if (!(info->ModeAttributes & VBE_ATTR_SUPPORTED) ||
        !(info->ModeAttributes & VBE_ATTR_GRAPHICS)  ||
        !(info->ModeAttributes & VBE_ATTR_LFB))
    {   why = MR_ATTR;  goto done; }

    switch (info->BitsPerPixel)
    {
    case 8:
        if (info->MemoryModel != VBE_MODEL_PACKED) { why = MR_MODEL; goto done; }
        break;
    case 16:
        if (info->MemoryModel != VBE_MODEL_DIRECT) { why = MR_MODEL; goto done; }
        if (info->GreenMaskSize != 6)              { why = MR_MASKS; goto done; }
        if (info->RedMaskSize   != 5)              { why = MR_MASKS; goto done; }
        if (info->BlueMaskSize  != 5)              { why = MR_MASKS; goto done; }
        if (info->RedFieldPosition  != 11)         { why = MR_MASKS; goto done; }
        if (info->BlueFieldPosition != 0)          { why = MR_MASKS; goto done; }
        break;
    case 24:
    case 32:
        if (info->MemoryModel != VBE_MODEL_DIRECT) { why = MR_MODEL; goto done; }
        if (info->RedFieldPosition  != 16)         { why = MR_MASKS; goto done; }
        if (info->BlueFieldPosition != 0)          { why = MR_MASKS; goto done; }
        break;
    default:
        why = MR_BPP;  goto done;
    }

    if (info->BytesPerScanLine == 0)     { why = MR_PITCH; goto done; }
    if (info->XResolution < 640 || info->YResolution < 480)
    {   why = MR_SIZE;  goto done; }

    fbNeed = (DWORD)info->BytesPerScanLine * info->YResolution;
    if (vramSize && fbNeed > vramSize)   { why = MR_VRAM;  goto done; }
    if (fbNeed > VBE_LFB_MAP_CAP)        { why = MR_CAP;   goto done; }

done:
    if (pReason)
        *pReason = why;
    return (why == MR_OK);
}

/*
 * VbeMaxFramebuffer —— 模式缓存表里最大的帧缓冲字节数(drv.c 用)。
 *   【用途】① 显存总量兜底估算(BIOS 谎报 TotalMemory 时);
 *          ② 给 DPMI 0800h 映射定尺寸: 一次按"以后可能用到的最大的
 *             模式"映射好, 会话内不必再重建选择子。
 *   返回 0 = 模式表还是空的(尚未枚举)。
 */
uint32_t __far VbeMaxFramebuffer( void )
{
    DWORD maxFb = 0;
    int   i;

    for (i = 0; i < g_nModes; i++)
    {
        DWORD fb = (DWORD)g_modes[i].pitch * g_modes[i].height;
        if (fb > maxFb)
            maxFb = fb;
    }
    return maxFb;
}


/*
 * BuildModeTable —— 枚举全部 VBE 模式, 滤出可用者缓存于 g_modes。
 *   返回可用模式数(0 = 失败)。
 */
static int BuildModeTable( void )
{
    uint16_t modes[256];
    uint32_t vram = 0;
    uint16_t ver  = 0;
    WORD     rej[MR_N];
    int      i, n;

    g_nModes = 0;
    for (i = 0; i < MR_N; ++i)
        rej[i] = 0;

    n = VBE_EnumModes( modes, 255, &vram, &ver );
    if (n <= 0)
        return 0;

    for (i = 0; i < n && g_nModes < MAX_CACHED_MODES; i++)
    {
        MODEINFOBLOCK mi;
        WORD          why = MR_OK;

        if (!VBE_GetModeInfo( modes[i], &mi )) continue;
        if (!IsModeOK( &mi, vram, &why ))
        {
            rej[why]++;
            continue;
        }
        g_modes[g_nModes].mode   = modes[i];
        g_modes[g_nModes].width  = mi.XResolution;
        g_modes[g_nModes].height = mi.YResolution;
        g_modes[g_nModes].bpp    = mi.BitsPerPixel;
        g_modes[g_nModes].pitch  = mi.BytesPerScanLine;
        g_nModes++;
    }

    /* 【r82 能力卡】一行看清这台机器的通用性边界: 卡给了多少模式、
     *  我们留下多少、其余被【哪一个条件】挡掉。远程诊断时这几行
     *  比任何猜测都值钱 —— "只有 1 个可用模式"到底是卡的毛病,
     *  还是我们把 MR_CAP/MR_MASKS 收得太紧, 一眼可辨。            */
    DiagHex( "CAP: vbe ver",    ver );
    DiagHex( "CAP: modes",      (WORD)n );
    DiagHex( "CAP: kept",       (WORD)g_nModes );
    DiagHex( "CAP: vram lo",    (WORD)vram );
    DiagHex( "CAP: vram hi",    (WORD)(vram >> 16) );
#ifdef VBE9X_DIAG
    for (i = 1; i < MR_N; ++i)
        if (rej[i])
            DiagHex( g_mrName[i], rej[i] );     /* 形如 "masks=0009" */
#endif
    FLUSH();

    return g_nModes;
}

/*
 * ApplyModeFallback —— 配置的模式不在表中时, 按【位深优先】的阶梯回退。
 *
 *   【r82 为什么要改】旧阶梯是"同位深 640x480 → 640x480x8 → 表首",
 *   它会因为一个分辨率不匹配就把用户的 16/32bpp 配置直接摔到 8bpp ——
 *   与本项目的目标("至少实现高彩高分, 不被 VGA 16 色限制")相悖。
 *   新阶梯(先牺牲分辨率, 后牺牲位深):
 *     第 1 轮: 配置的位深, 分辨率从高到低(配置值 → 1024x768 → 800x600 → 640x480)
 *     第 2 轮: 16bpp(高彩), 同样的分辨率阶梯
 *     第 3 轮: 8bpp(256 色, 仍远胜 VGA 16 色), 同样的分辨率阶梯
 *     第 4 轮: 表里第一个模式(最后的兜底, 一定有)
 *   位深序列按"配置值 → 16 → 8"去重后生成, 所以配置 8bpp 的用户仍然
 *   优先拿到 8bpp(他的选择被尊重), 只有在整张表里根本没有 8bpp 时才会
 *   被换成 16bpp, 且日志里写清换了什么。
 *   【调用前提】g_nModes > 0(需先 EnsureModeTable)
 */
static void ApplyModeFallback( void )
{
    static const WORD resList[][2] = { { 1024, 768 }, { 800, 600 }, { 640, 480 } };
    WORD cand[3];
    WORD bppList[3];
    int  nBpp = 0, bi, ri, i, j;
    WORD chosen;

    if (FindVbeMode( wScrX, wScrY, wBpp ) != 0xFFFF)
        return;                                 /* 配置的模式可用: 不动 */

    cand[0] = wBpp; cand[1] = 16; cand[2] = 8;
    for (i = 0; i < 3; ++i)
    {
        int dup = 0;
        for (j = 0; j < nBpp; ++j)
            if (bppList[j] == cand[i]) dup = 1;
        if (!dup) bppList[nBpp++] = cand[i];
    }

    for (bi = 0; bi < nBpp; ++bi)
    {
        chosen = FindVbeMode( wScrX, wScrY, bppList[bi] );
        if (chosen == 0xFFFF)
        {
            for (ri = 0; ri < 3; ++ri)
            {
                chosen = FindVbeMode( resList[ri][0], resList[ri][1],
                                      bppList[bi] );
                if (chosen != 0xFFFF)
                {
                    wScrX = resList[ri][0];
                    wScrY = resList[ri][1];
                    break;
                }
            }
        }
        if (chosen != 0xFFFF)
        {
            if (wBpp != bppList[bi])            /* 位深变了才动 palettized */
            {
                wBpp        = bppList[bi];
                wPalettized = (wBpp == 8);
            }
            DiagHexMin( "ModeFallback: bpp", wBpp );
            DiagHex( "ModeFallback: xRes", wScrX );
            DiagHex( "ModeFallback: yRes", wScrY );
            DiagHex( "ModeFallback: mode", chosen );
            FLUSH();
            return;
        }
    }

    /* 表里第一个模式: 一定有, 只是未必好看 */
    wScrX       = g_modes[0].width;
    wScrY       = g_modes[0].height;
    wBpp        = g_modes[0].bpp;
    wPalettized = (wBpp == 8);
    DiagHex( "ModeFallback: last resort mode", g_modes[0].mode );
    FLUSH();
}

/*
 * EnsureModeTable —— 惰性构建可用模式缓存表(需要时才枚举),
 *   并应用配置回退。返回 1 = 表可用。
 * 【为何惰性 + 仅 Enable 期调用】(./21 裁决) 加载期系统 VM 的 INT 10h
 *   链被引导期残留层接管: VESA_SUPPORT 钩子被调(count=1)、NC 已送达,
 *   缓冲区却原封不动('VBE2' 原样) —— 假成功层不放行。而参考实现
 *   vmdisp9x 从不在加载期调 VESA, 枚举全在 GDI 期(Enable)完成。
 *   加载期探测在 r45 已删除: ./43/44 实锤本机加载期枚举硬死机。
 *   正式枚举唯一入口是 Enable 期的 PhysicalEnable 调用链, 与
 *   vmdisp9x 时序对齐。Enable 处为硬需求: 失败 → PhysicalEnable
 *   返回 0 → GDI 回退 VGA, 失败语义与原来等价。
 */
static int EnsureModeTable( void )
{
    if (g_nModes == 0)
    {
        DIAG( "EnsureModeTable: building now" );
        BuildModeTable();
        DiagHex( "VESA_SUPPORT count", QueryVesaSupportCount() );
    }
    if (g_nModes == 0)
        return 0;
    ApplyModeFallback();
    return 1;
}

/*
 * FindVbeMode —— 在缓存表中查找 宽x高xbpp, 返回 VBE 模式号;
 *   0xFFFF 表示未找到(0xFFFF 是 VBE 模式列表的结束符, 不会是合法模式号)。
 */
static WORD FindVbeMode( WORD w, WORD h, WORD bpp )
{
    int i;
    for (i = 0; i < g_nModes; i++)
        if (g_modes[i].width == w && g_modes[i].height == h &&
            g_modes[i].bpp == bpp)
            return g_modes[i].mode;
    return 0xFFFF;
}

/*
 * ReadDisplayConfig —— 读取用户配置的显示模式。
 *   来源优先级(与参考实现 vmdisp9x 一致):
 *     1. system.ini [display] 段的 dpi/x_resolution/y_resolution/bpp
 *        (setup 阶段的早期配置通道)
 *     2. 主 VDD 的 VDD_GET_DISPLAY_CONFIG(0x85) —— 注册表里的当前配置,
 *        成功时覆盖 system.ini 的值
 *   最后做合法性夹取, 并读取 8bpp 的 palettized 开关。
 *   【与 vmdisp9x 的差异】vmdisp9x 读了 system.ini 的分辨率却没有应用
 *   (疑似其小疏漏); 这里非 0 即应用, 行为更直观。
 */
static void ReadDisplayConfig( void )
{
    DISPLAYINFO di;
    DWORD       rc;
    WORD        wX, wY, wB;

    wDpi = LazyGPPI( "display", "dpi", 96 );
    wX   = LazyGPPI( "display", "x_resolution", 0 );
    wY   = LazyGPPI( "display", "y_resolution", 0 );
    wB   = LazyGPPI( "display", "bpp", 0 );
    if (wX) wScrX = wX;
    if (wY) wScrY = wY;
    if (wB) wBpp  = wB;

    /* 【缓冲区必须初始化】主 VDD 的 0x85 实现会读 diHdrSize 判断缓冲区
       版本/容量, 未初始化的栈垃圾可能让它按荒谬尺寸拷贝 → 在 VDD 内
       GP(0d:032f:0000088d/0acd 实锤于此函数)。DDK 约定 hdrSize 由
       调用方填 sizeof(DISPLAYINFO)。                            */
    _fmemset( &di, 0, sizeof( di ) );
    di.diHdrSize = sizeof( di );
    DIAG( "VddGetDispConf: before" );
    FLUSH();                            /* r43: 进主 VDD 前落盘(0x85 曾是
                                           GP 高发区, 见本函数头注释) */
    rc = VddGetDispConf( VDD_GET_DISPLAY_CONFIG, sizeof( di ), &di );
    DIAG( "VddGetDispConf: after" );
    FLUSH();
    if (rc == 0)    /* VDD 成功返回 0; 失败返回功能号本身 */
    {
        wScrX = di.diXRes;
        wScrY = di.diYRes;
        wBpp  = di.diBpp;
        if (di.diDPI)
            wDpi = di.diDPI;
    }

    /* 合法性夹取: 位深只认 8/16/24/32, 分辨率下限 640x480 */
    if (wBpp != 8 && wBpp != 16 && wBpp != 24 && wBpp != 32) wBpp = 8;
    if (wScrX < 640) wScrX = 640;
    if (wScrY < 480) wScrY = 480;

    /* 8bpp 时读取 palettized 设置, 默认启用 */
    if (wBpp == 8)
        wPalettized = LazyGPPI( "display", "palettized", 1 );
    else
        wPalettized = 0;
}

/*
 * LazyGPPI —— GetPrivateProfileInt 的轻包装(文件名恒为 system.ini)。
 *   【为何改成静态导入】曾用 GetProcAddress 动态解析以"收敛导入表",
 *   但真机实测(./15)在该间接调用处 KERNEL 内 GP(0d:032f:00000b6d)——
 *   自攒的函数指针路径在寄存器/栈序上踩了 Watcom 头文件与 KERNEL
 *   ABI 的暗坑。GetPrivateProfileInt 改回静态导入(KERNEL.127, 与在
 *   售驱动 vesamini.drv 的导入方式一致, wine 序号表与真机双重实证),
 *   由编译器+导入库处理调用约定, 属已解决的问题。GetModuleHandle/
 *   GetProcAddress(47/50)的动态路径保留(DIBENG 解析用, GDI 期才走)。
 */
static WORD LazyGPPI( const char *sec, const char *key, int def )
{
    WORD r;
    DIAG( "LazyGPPI: enter" );
    FLUSH();            /* r43: KERNEL.127 调用前后各落一次(./42c 死亡区) */
    r = (WORD)GetPrivateProfileInt( sec, key, def, "system.ini" );
    FLUSH();
    return r;
}

/*
 * ReadRunProfile —— 读 system.ini [omninine] 的运行时配置(见文件上方全局区
 *   的说明)。默认值 = r80 的行为, 所以"没有这个段"和"有这个段但没写键"
 *   都等价于本轮之前的行为。
 * 【调用时机】Enable 的 PDEVICE 相(在 PhysicalEnable 之前 —— 屏选择子在
 *   PhysicalEnable 里建, 必须先把 SelMode 送下去), 以及 ReEnable(改分辨率
 *   走的是同一批分支)。加载期不调用: 那时 system.ini 的读取虽已证可用
 *   (ReadDisplayConfig), 但没有任何理由提前。
 */
static void ReadRunProfile( void )
{
    WORD off;

    DIAG( "ReadRunProfile: enter (system.ini [omninine])" );
    FLUSH();

    /* 【r88 默认改回形态 A(0 基址平坦选择子)】r87 的日志证明 DIBENG 需要它:
     * 其代码做的是 "mov es,[si+78h]" + "test dword es:[0000001Ch]" —— 绝对
     * 地址 32 位访问, 只有 0 基址选择子才成立。而且 drv.c 里形态 A 之所以
     * "从来没建成", 是因为它一直请求 4GB 界限被宿主拒绝(r88 已改成两级尝试)。
     * SelMode=1 仍可强制形态 B 做对照。                              */
    g_runSelMode  = (LazyGPPI( "omninine", "SelMode",  0 ) == 1) ? 1 : 0;

    /* 【r89】描述符回读校验(见 drv.c 的 LfbSelVerify): 默认 1 = 建好的选择子
     * 必须真的覆盖"我们要交给 DIBENG 的窗口", 否则该级作废、走下一级退路;
     * 置 0 = 回到 r88 的"只看存在位"行为, 用于排除"是不是这道闸门在挡"。 */
    g_runVerify   = (LazyGPPI( "omninine", "Verify", 1 ) == 0) ? 0 : 1;

    /* 【r90】deBeginAccess/deEndAccess 装谁: 0(默认) = 我们的回调(r80 起的行为);
     * 1 = DIBENG 自己的 DIB_BeginAccess/DIB_EndAccess(序号 105/106, 在售参考
     * 驱动 boxv9x 用的就是这一种)。两个地址本来就在 g_dibfn[] 里(槽 29/30),
     * 不需要新增导入 —— 所以这是一条零构建风险的 A/B 开关。 */
    g_runAccess   = (LazyGPPI( "omninine", "Access", 0 ) == 1) ? 1 : 0;

    /* 【r91】DibEnable: 我们自己在 PDEVICE 相里调不调 DIB_Enable。
     * 证据(r89/r90 六种配置全部同址 0D): 调用点在"填 DIBENGINE 之前",
     * 于是引擎拿到的是一本未初始化的 PDEV —— 其 RAW 落盘显示它按
     * deType='DI' / deFlags=0004 / 1x1x24bpp(deBits 03DF:0038) 建了一本
     * 模板设备, 随后又被我们的手填值覆盖。引擎内部状态与 GDI 看到的结构
     * 从此不一致 —— 这正是"所有配置都崩在同一处"的形态。
     * 默认 0 = 不再自己调(GDI 在我们 Enable 返回后会调用引擎的 Enable);
     * 置 1 = 回到 r80..r90 的行为, 用于 A/B。 */
    g_runDibEnable = (LazyGPPI( "omninine", "DibEnable", 0 ) == 1) ? 1 : 0;
    g_runBitsOff  = (LazyGPPI( "omninine", "BitsOff",  0 ) == 1) ? 1 : 0;
    g_runClear    = (LazyGPPI( "omninine", "Clear",    1 ) == 0) ? 0 : 1;
    g_runSentinel = (LazyGPPI( "omninine", "Sentinel", 0 ) == 1) ? 1 : 0;
    g_runEndShot  = (LazyGPPI( "omninine", "EndShot",  0 ) == 1) ? 1 : 0;
    g_runSelfTest = (LazyGPPI( "omninine", "SelfTest", 1 ) == 0) ? 0 : 1;
    g_runFlags    = (WORD)LazyGPPI( "omninine", "Flags", 0 );

    /* 【r83】上次蓝屏地址(从蓝屏屏上抄下来)。GetPrivateProfileInt 只认
     * 十进制, 所以 042F:2B70 要写成 Crash=1071 / CrashOff=11120。
     * 留 0 = 不抄(默认)。【通常不必配】DiagSegMap 会自动把 LDT 里所有
     * 代码段在 DumpOff 处的机器码整片抄下来, 而 DumpOff 默认 0x2B60 的
     * 64 字节窗口本就覆盖历史蓝屏偏移 0x2B70。                          */
    g_runCrashSel = (WORD)LazyGPPI( "omninine", "Crash",    0 );
    g_runCrashOff = (WORD)LazyGPPI( "omninine", "CrashOff", 0 );

    /* 【r84】CDPD=1(默认) 用 DDK 的 CreateDIBPDevice 建 PDEVICE; =0 退回手填。
     * 一条开关就能在【不重编】的前提下对两种建法做 A/B —— 这正是 r81 起
     * 这套 system.ini 开关存在的意义。                                  */
    g_runCdpd     = (LazyGPPI( "omninine", "CDPD", 1 ) == 0) ? 0 : 1;

    /* 【r87】delpPDevice：引擎自建 PDEVICE 时把它留成 0（r84 实测），而 DDK 对
     * 该字段的原文是"物理设备的地址 —— 指向显存 / DDB / DIB 的远指针"。
     * r83 曾把它填成"屏选择子:0"，但**那一轮的结论无效**：当时 DS 丢失的 bug
     * 让所有版本都崩在同一处（见 R76_CHANGES 的 r86 小节），这个实验被掩盖了。
     * 现在 DS 已修好，重新给出这个开关：默认填(1)，置 0 可对照"引擎给什么就是
     * 什么"（即 0）。                                                  */
    /* 【r93】DelpDev 变成三档: 0 = 不碰(留引擎给的 0), 1(默认) = 指向显存,
     * 2 = 指向本 PDEVICE 自己 —— 依据是本工程 ddk/dibeng.h L23 对该字段的
     * 原文 "Associated PDevice pointer"。取值非法时回落 1(与旧行为等价)。 */
    g_runDelpDev  = (WORD)LazyGPPI( "omninine", "DelpDev", 1 );
    if (g_runDelpDev > 2)
        g_runDelpDev = 1;

    /* 【r94】DIBENG 的 DGROUP 选择子自证修正(见 DibDgroupProbe 长注释)。
     * 默认 1 = 用装载器桩里的立即数覆盖模块库解析值; 0 = 回 r93 行为。 */
    g_runDibDg    = (LazyGPPI( "omninine", "DibDg", 1 ) == 0) ? 0 : 1;

    /* 【r97】Pal: 序号 22(SetPalette DDI) 走哪条路 —— 见 g_runPalMode 注释。
     * 默认 1 = Ext 路(尾部显式 PDEV, r96 已在同类调用上验证过形态);
     * 0 = DIBLINK 形态(非 Ext + 跳板注入 EDX); 2 = 完全不进引擎。
     * 取值非法回落 1。 */
    g_runPalMode  = (WORD)LazyGPPI( "omninine", "Pal", 1 );
    if (g_runPalMode > 2)
        g_runPalMode = 1;

    /* 【r97】Edx: 跳板是否在远跳进 DIBENG 前把设备指针装进 EDX(非 Ext 入口
     * 的第 4 个参数)。默认 1 = 注入; 0 = 回 r96 行为(复现 0367:4036 用)。 */
    g_runEdx      = (LazyGPPI( "omninine", "Edx", 1 ) == 0) ? 0 : 1;

    /* 【r96】Xlat: Enable 里那次 DIB_SetPaletteTranslateExt 调不调、在哪调。
     * 默认 1 = 在 CreateDIBPDevice 建好 PDEVICE【之后】调(DDK 次序);
     * 0 = 整段跳过(r81..r95 是在建之前调 —— 那等于把垃圾缓冲当 PDEV)。 */
    g_runXlat     = (LazyGPPI( "omninine", "Xlat", 1 ) == 0) ? 0 : 1;

    /* 【r99/r100】策略开关 —— 一个 build 覆盖全部 A/B。
     * VESA_SUPPORT/CHECK_HIRES_MODE/GET_VDD_BANK 由 mini-VDD 现场读
     * (随 PushState 推过去); EDX 由 g_vddEdxValue 决定。
     * 【r100 关键修正】折算必须在这里(而不是 PhysicalEnable 里)完成 ——
     * PhysicalEnable 内部的 SetDisplayMode→PushState 发生在原折算行之前,
     * 否则第一次 Enable 推给 VxD 的 policyVddEdx 读到的是旧的 0(这正是
     * r99 里 HiresAns/VesaPolicy/VddVirtualize-VxD侧"三个死开关"之一半)。*/
    /* 【r104】默认 0 → 1(= 注册时 EDX=-1)。理由见 g_runVddVirt 的长注释:
     * 可见屏在显存偏移 0 ⇒ 主 VDD 无处安放它那块"可见屏之前的 32K/64K
     * 虚拟化区", 而该族服务我们一项都没实现 ⇒ 如实声明"本显示要求 VGA
     * 4 平面图形程序全屏运行"。0 = r99..r103 行为(留作 A/B)。        */
    g_runVddVirt  = (WORD)LazyGPPI( "omninine", "VddVirtualize", 1 );
    if (g_runVddVirt > 2)
        g_runVddVirt = 1;
    g_runVesa     = (LazyGPPI( "omninine", "VesaPolicy", 0 ) == 1) ? 1 : 0;
    /* 【r100 默认改为 1; r101 语义扩展为 0..3】见 minivdd.asm TraceStub_35。
     *   0 = 不挂钩(主 VDD 默认 NC = "标准 VGA", r98/r99 行为);
     *   1 = 【默认】按 mini-VDD 维护的切屏状态如实回答(在 VBE 模式且未被
     *       切到 VGA ⇒ CY);
     *   2 = 只答 Windows VM(r100 行为, 留作 A/B —— 实测那条 1729/7869 次
     *       的窗口化 DOS 轮询里 EBX 与 PM API 记下的句柄不是同一套取值);
     *   3 = 只答非 Windows VM(反向对照, 用来证明 EBX 的身份)。        */
    /* [r132] default 1 -> 4.  Policy 4 is the DDK answer: CHECK_HIRES_MODE
     * asks about the VM in EBX (see minivdd.asm TraceStub_35), so a DOS VM
     * that polls with AL=03h/06h must get NC, not "yes, hi-res".           */
    /* [r133] back to 1 (answer from our own state, the r131 behaviour).
     *   Policy 4 (the DDK answer) did not stop the f23 poll storm in ./132
     *   -- "TRC + f=23 CHECK_HIRES_MODE n=0001297D", the same order of
     *   magnitude as ./130 -- so it bought nothing, and it is the second
     *   of the two deltas r132 has over r131.  HiresAns=4 restores it. */
    /* [r151] 回到 4 —— 也就是【官方要求的答案】, 理由见 R151_DELIVERY.md §2。
     *   r133 把默认拨回 1 的【唯一】依据是"风暴没变小", 而风暴从来不是这条
     *   开关要修的东西; 它的判据是 DDK 原文:
     *     CHECK_HIRES_MODE (Function 35) -- "AL: Contains the Interrupt 10h
     *     video mode.  EBX: Contains the VM handle to inquire about. ...
     *     CY returned means that the VM is in a hi-res mode."
     *   ⇒ 答案描述的是 EBX 里的【那个】VM。用我们自己的 Windows 模式回答
     *   每一个调用者, 等于告诉一个跑 80x25 文本的 DOS VM"你是高分辨率"。
     *   HiresAns=1 可以一行回到 r133 的行为。                            */
    g_runHires    = (WORD)LazyGPPI( "omninine", "HiresAns",   4 );   /* r151-marker: HiresAns default is 4 = the DDK answer */
    if (g_runHires > 4)
        g_runHires = 4;
    g_runHeal     = (LazyGPPI( "omninine", "Heal", 0 ) == 1) ? 1 : 0;
    g_runReSet    = (LazyGPPI( "omninine", "ReSet",    1 ) == 0) ? 0 : 1;   /* r100 */
    g_runCursor   = (LazyGPPI( "omninine", "Cursor",   1 ) == 0) ? 0 : 1;   /* r100 */
    g_logLevel    = (WORD)LazyGPPI( "omninine", "Log",   2 );               /* r100 */
    if (g_logLevel > 3)
        g_logLevel = 2;
    /* 【r110】LogBoot —— 开机相日志级别(见 drv.c 里 g_logBoot 的长注)。
     * 它要回答的是 ./109 t6 提出的问题: "Log=1 那次开机回落 VGA"到底是
     * 日志量改变时序, 还是 Log 这个键本身有别的副作用。用法:
     *     LogBoot=2 + Log=1  ⇒ 开机相逐行落盘、桌面起来后立刻静音。
     * 判词: 不再回落 ⇒ 与开机相的日志量有关; 仍回落 ⇒ 与它无关。     */
    g_runLog      = g_logLevel;                                          /* r110 */
    g_logBoot     = (WORD)LazyGPPI( "omninine", "LogBoot", 0 );             /* r110 */
    if (g_logBoot > 3)
        g_logBoot = 0;
    if (g_logBoot != 0)
    {
        g_bootLogOn = 1;
        g_logLevel  = g_logBoot;
    }
    else
    {
        g_bootLogOn = 0;
    }
    /* 【r110】PortGrp —— 端口读分组位图(见 drv.c 里 PortProbeRead 的长注)。
     * 默认 7 = CRTC|时序器|GCR(几何与像素格式); 8 = 3CCh/3C6h(DAC 与
     * Misc Out, RAMDAC 侧自副作用最不可控, ./109 t3 的嫌疑就在这一组)。*/
    g_runPortGrp  = (WORD)LazyGPPI( "omninine", "PortGrp", 7 );             /* r110 */
    if (g_runPortGrp > 15)
        g_runPortGrp = 7;
    /* 【r110】ReArm —— 重建被护栏拒绝时是否立刻重新挂起(见 g_runReArm)。*/
    g_runReArm    = (LazyGPPI( "omninine", "ReArm", 1 ) == 0) ? 0 : 1;      /* r110 */
    /* 【r111】CbRestore / HwFp —— 见各自声明处的长注。CbRestore 默认 1
     * ("官方路径优先"); HwFp 【r113 起默认 0】: 那个指纹判据已被 ./112
     * 证伪(t2 的 `HwFp diff (0=same)=0000` 就发生在屏幕已经花掉的时候),
     * 它当否决票用会让窗口化 DOS 会话里的花屏永久留下 —— 详见声明处。 */
    g_runCbRestore= (LazyGPPI( "omninine", "CbRestore", 1 ) == 0) ? 0 : 1;  /* r111 */
    g_runHwFp     = (WORD)LazyGPPI( "omninine", "HwFp",  0 );               /* r113 */
    if (g_runHwFp > 2)
        g_runHwFp = 0;
    /* 【r112】官方路径的实现方式(CbHwSet/CbBios)与故障定位开关(DdiTrace)。
     * 见各自声明处的长注 —— 三个都是"一行 INI 换一种行为"。            */
    g_runCbHwSet  = (LazyGPPI( "omninine", "CbHwSet",   1 ) == 0) ? 0 : 1;  /* r112 */
    g_runCbDuty   = (WORD)LazyGPPI( "omninine", "CbDuty", 3 );              /* r112 */
    if (g_runCbDuty < 1 || g_runCbDuty > 3)
        g_runCbDuty = 3;
    g_runDdiTrace = (LazyGPPI( "omninine", "DdiTrace",  0 ) == 1) ? 1 : 0;  /* r112 */
    /* 【r113】HwReg —— 整组模式寄存器的采样点开关, 见声明处长注:
     * 0 = 不采样(回调里的回放自动跳过, 模式恢复全交给回落层 4F02);
     * 1 = 【默认】只在一次成功重建之后采样; 2 = 再落一份逐寄存器转储。 */
    g_runHwReg    = (WORD)LazyGPPI( "omninine", "HwReg",  1 );              /* r113 */
    if (g_runHwReg > 2)
        g_runHwReg = 1;
    /* 【r114】VgaFix —— 见声明处长注。留 0 = r113 行为; 推给 mini-VDD 的是
     * VBESHAREDSTATE 的 policyVgaFix(原本的保留字节 42, 结构仍 44 字节),
     * 所以"新 drv + 旧 vxd"也能跑(旧 VxD 只是不读它)。               */
    g_runVgaFix   = (LazyGPPI( "omninine", "VgaFix", 1 ) == 0) ? 0 : 1;     /* r114 */
    /*                                     ↑ r129: 默认 0 -> 1。
     * 见变量声明处的长注: 它现在同时管"会话边界两个方向"的寄存器补写
     * (hook 5 的文本取指组 + hook 7 的图形/时钟位), 两条都是"先读、不对
     * 才写", 正常路径零写入。写 VgaFix=0 即可两边都关(回到 r128 行为)。 */
    /* 【r116】Dispi —— 见声明处长注。默认 1(参考实现的动作, 也是本轮
     * 唯一的功能性改动); 留 0 就是回到 r115 的硬件足迹。               */
    /* 【r169】默认从 0 改成 1(见变量声明处的长注: ./164 的武装期计数器与
     *   ./168 的 4F03h=0072h 把 r151 的两条删除理由都推翻了)。
     *   值域仍是 0/1; >1 夹成 1 —— 无条件档(2 = ./149)已删除, 它一定会
     *   把桌面弄丢, 没有保留成 A/B 的价值。 */
    {
        int v10 = LazyGPPI( "omninine", "Int10Fix", 1 );
    g_runSettlePoll = (WORD)LazyGPPI( "omninine", "SettlePoll", 4 );   /* r406 */
        g_runInt10Fix = (WORD)((v10 < 0) ? 0 : ((v10 > 1) ? 1 : v10));
    }
    g_runHiresFix = (LazyGPPI( "omninine", "HiresFix", 1 ) == 0) ? 0 : 1;   /* r146 */
    /* 【r152】MemCFix 现在是三值开关(0/1/2, 见上面长注)。
     *   旧写法 `(LazyGPPI(...) == 0) ? 0 : 1` 会把任何非 0 值压成 1,
     *   等于把新档位 2 静默降级回 ./148 那条路 —— 必须改成直读。 */
    /* r406 C8: the "MemCFix" INI key is no longer read -- it fed a policy byte
     * that nothing has consumed since 取指基址修复 was removed in r303. */
    g_runDispi    = (LazyGPPI( "omninine", "Dispi",  1 ) == 0) ? 0 : 1;     /* r116 */
    /* 【r117】DispiFix —— 见声明处长注。读端口必须 Dispi 也非 0(同一族
     * 端口, 同一个"这台机器是不是 Bochs 家族"的前提); 另留一个无条件
     * 关闭的开关, 便于做"只测陷阱"的对照。                              */
    g_runDispiFix = (LazyGPPI( "omninine", "DispiFix", 1 ) == 0) ? 0 : 1;   /* r117 */
    if (!g_runDispi)
        g_runDispiFix = 0;
    /* 【r119】DispiIdProbe —— 见声明处长注。这是"写索引 0 再回读"那一枪的
     * 总闸: 0 则探针一个字节都不写(退回 r117/r118 的只读形态)。          */
    g_runDispiIdProbe = (LazyGPPI( "omninine", "DispiIdProbe", 1 ) == 0) ? 0 : 1;
    /* 【r120】DispiPhys —— 见声明处长注。默认 0 = 关; 打开后 ring 0 用
     * VDD_Do_Physical_IO 绕开 VM 端口虚拟化去读 DISPI 的六个寄存器。    */
    g_runDispiPhys = (LazyGPPI( "omninine", "DispiPhys", 0 ) == 0) ? 0 : 1;
    /* 【r122, r126 复评】SetModeText —— 默认 0; ./122 实测开=1 也修不好,
     * 见变量声明处的长注(r126 起它是"已验证无效"档, 留着只作对照)。   */
    g_runSetModeText = (LazyGPPI( "omninine", "SetModeText", 0 ) == 0) ? 0 : 1;
    /* 【r130】CrtGeo —— 见变量声明处的长注。默认 1 = 扫描几何与帧缓冲不一致
     * 时强制重新走一遍模式设置; 0 = 只读数不重设(A/B 对照用)。          */
    g_runCrtGeo = (LazyGPPI( "omninine", "CrtGeo", 1 ) == 0) ? 0 : 1;
    /* 【r131】CrtEn —— 见变量声明处的长注。默认 1 = CR17h bit7(SE)读回 0 时
     * 补成 1 并读回自证; 0 = 只读数不写(A/B 对照用, 也是"怀疑这一枪本身"
     * 时的急停开关)。                                                   */
    g_runCrtEn = (LazyGPPI( "omninine", "CrtEn", 1 ) == 0) ? 0 : 1;
    /* [r133] ProtClr is OFF again by default: ./132's own log shows the
     *   Protect bit was cleared before the mode set and stayed clear in
     *   every snapshot, and the strip was still there -- the premise of
     *   the r132 step did not hold.  ProtClr=1 keeps it available for a
     *   one-line A/B, because it is still the only write r132 added to the
     *   boot path. */
    g_runProtClr  = (LazyGPPI( "omninine", "ProtClr",  0 ) == 0) ? 0 : 1;   /* r132 */
    g_runSnapSess = (LazyGPPI( "omninine", "SnapSess", 1 ) == 0) ? 0 : 1;   /* r132 */
    g_runRelatch  = (WORD)LazyGPPI( "omninine", "Relatch",  1 );            /* r133 */
    if (g_runRelatch > 2)                  /* level 2 = also in the boot path */
        g_runRelatch = 1;                  /* anything else: back to the default */
    /* [r134] HwEss -- the verdict that decides whether the official (VDD
     *   callback) restore succeeded: 1 = [default] by MEANING, 0 = by the
     *   byte count that r133 used (which vetoed the official path in five of
     *   six callbacks).  See ApplyDesktopRestore for the full argument. */
    g_runHwEss    = (LazyGPPI( "omninine", "HwEss", 1 ) == 0) ? 0 : 1;      /* r134 */
    g_runVhWin    = (LazyGPPI( "omninine", "VhWin", 1 ) == 0) ? 0 : 1;      /* r134 */
    g_runSessNoSet= (LazyGPPI( "omninine", "SessNoSet", 1 ) == 0) ? 0 : 1;  /* r135 */
    g_runEdgeDir  = (LazyGPPI( "omninine", "EdgeDir", 1 ) == 0) ? 0 : 1;    /* r135 */
    g_runEntryIn  = (LazyGPPI( "omninine", "EntryIn", 0 ) == 0) ? 0 : 1;    /* r135 */
    g_runRingDump = (LazyGPPI( "omninine", "RingDump", 1 ) == 0) ? 0 : 1;   /* r136 */
g_runEntryNoSet  = (LazyGPPI( "omninine", "EntryNoSet", 1 ) == 0) ? 0 : 1; /* r137 */
g_runEntryReplay = (LazyGPPI( "omninine", "EntryReplay", 1 ) == 0) ? 0 : 1;/* r137 */
g_runSampleAtBoot= (LazyGPPI( "omninine", "SampleAtBoot", 1 ) == 0) ? 0 : 1;/* r138 */
g_runCbNoSample  = (LazyGPPI( "omninine", "CbNoSample", 1 ) == 0) ? 0 : 1;  /* r138 */
g_runHwTruth     = (LazyGPPI( "omninine", "HwTruth", 1 ) == 0) ? 0 : 1;     /* r138 */
g_runAltFromTbl  = (LazyGPPI( "omninine", "AltFromTbl", 1 ) == 0) ? 0 : 1;  /* r138 */
g_runHwChkPitch  = (LazyGPPI( "omninine", "HwChkPitch", 1 ) == 0) ? 0 : 1;  /* r138 */
g_runBlindNeed   = (LazyGPPI( "omninine", "BlindNeed",  1 ) == 0) ? 0 : 1;  /* r139 */
g_runEntryAlways = (LazyGPPI( "omninine", "EntryAlways",1 ) == 0) ? 0 : 1;  /* r139 */
g_runVhAlways    = (LazyGPPI( "omninine", "VhAlways",   1 ) == 0) ? 0 : 1;  /* r139 */
g_runDacStamp    = (LazyGPPI( "omninine", "DacStamp",   1 ) == 0) ? 0 : 1;  /* r139 */
g_runForceEdges  = (LazyGPPI( "omninine", "ForceEdges", 1 ) == 0) ? 0 : 1;  /* r140 */
g_runPitchWatch  = (LazyGPPI( "omninine", "PitchWatch", 1 ) == 0) ? 0 : 1;  /* r141 */
g_runWaitSafe    = (LazyGPPI( "omninine", "WaitSafe",   1 ) == 0) ? 0 : 1;  /* r142 */
    /* 【r101 核心修正 1】VddBank —— GET_VDD_BANK(1) 的回答策略。
     *   ./100 实测: 我们注册时声明 EDX=0("支持窗口化 VGA 4 平面虚拟化"),
     *   主 VDD 随即用 ecx=0x00300000(=可见屏字节数)、edx=0 问我们虚拟化区
     *   放哪, 而我们没挂钩 ⇒ 注册返回值算出的"VDD 自留"= 0 字节 ⇒ 主 VDD
     *   带着 0 字节的虚拟化区去做 4 平面虚拟化 ⇒ DOS 会话的 VGA 访问落进
     *   可见屏(实拍: 整屏绿(文本单元 0x0720 按 RGB565 就是纯绿)+ 2KB 宽的
     *   陈旧条 + Alt+Enter 后近乎全黑)。
     *   DDK 原文: "If not allowing VGA 4 plane virtualization, set
     *   EDX = ECX or Client_ECX" —— 于是默认改成【拒绝】。DDK 同时明说
     *   拒绝后"仍允许文本与非平面图形(如 13h)在窗口里跑", 所以窗口 DOS
     *   文本盒不受影响, 被挡住的只有"窗口化 VGA 图形虚拟化"。            */
    g_runVddBank  = (LazyGPPI( "omninine", "VddBank",  1 ) == 0) ? 0 : 1;
    /* 【r101】Int10Mode —— 48 号(PRE_INT_10_MODE_SET)是否"吃掉"DOS 应用的
     *   设模式请求。该服务在 DDK 里没有文档页, 返回约定属推断 ⇒ 默认 0
     *   (只把被请求的模式号记进 blob, 不改主 VDD 行为)。下一轮 A/B。    */
    g_runInt10    = (LazyGPPI( "omninine", "Int10Mode", 0 ) == 1) ? 1 : 0;
    /* 【r102】Offscreen / Access2 —— 见文件上方两处长注释。
     * 都是"一行 INI 换一种行为", 不必重编, 这是本轮能一轮跑完矩阵的前提。*/
    /* 【r103 默认改成 0】依据是 ./102 的 B1 实测(用户报告): 同一份源码、
     * 唯一差别是不声明 OFFSCREEN, 运行 command 就直接进【全屏】DOS 文本
     * 会话并【完全正常显示】(80x25、白字黑底、一个像素都不花); 而声明
     * OFFSCREEN 的 B-1/B0/B2 三份会话里, 窗口化 DOS 盒与整个桌面都是
     * 4 像素周期的竖条纹。
     * 机制(r101 日志 + DDK 原文): ① 主 VDD 在 VDD_DRIVER_REGISTER 的返回值
     * 里自留的显存 = 【0 字节】(所有 r101 日志都是 reg hi == vis hi: vm
     * 0018/0018、x230 0030/0030 与 0009/6000), 也就是说它【没有】那块
     * "right below the visible Windows screen" 的 32K/64K 虚拟化区;
     * ② 我们却声明 OFFSCREEN, 于是 DIBENG 的离屏堆正好从"可见屏之后"
     * 开始 —— 与主 VDD 想放虚拟化区的位置(ecx = 可见屏字节数)完全重合;
     * ③ 结果就是 DOS 会话保存/恢复 VGA 文本屏时踩进 DIBENG 的堆, 两边
     * 互相覆盖(实拍: 桌面被 4 像素周期条纹盖住、DOS 框内容错乱、Alt+Enter
     * 后近全黑)。不声明 OFFSCREEN 时 DIBENG 不做离屏分配, 那块区域空出来
     * 归主 VDD, 两边不再互踩。
     * 【遗留】这等于放弃 DIBENG 的离屏堆(离屏位图/部分加速会变慢)。真正
     * 干净的解法是把"主 VDD 自留量"从我们上报的显存里扣掉(注册返回值
     * 已经在日志里), 见 r103 改动说明的 §5 "后续"。                      */
    g_runOffscreen = (LazyGPPI( "omninine", "Offscreen", 0 ) == 0) ? 0 : 1;
    g_runAccess2   = (LazyGPPI( "omninine", "Access2",   0 ) == 1) ? 1 : 0;
    /* 【r105】AccessLog —— 是否把"光标排斥临界区"(Begin/EndAccess)的记账
     * 落盘。见 g_runAccessLog 声明处。默认 1(它只在安全上下文打十几行)。 */
    g_runAccessLog = (LazyGPPI( "omninine", "AccessLog", 1 ) == 0) ? 0 : 1;
    /* 【r105】CursorSrc —— 光标形状从哪来。见 g_runCursorSrc 声明处。
     * 1 = 【新默认】SetCursor 调用当场拷进 s_curShape(那时指针一定有效);
     * 0 = r100..r104 的老路: 只记下调用方的远指针, 等回调期再去读。     */
    g_runCursorSrc = (LazyGPPI( "omninine", "CursorSrc", 1 ) == 0) ? 0 : 1;
    /* 【r104】VgaEdge —— 见 g_runVgaEdge 声明处。0 = 关掉"会话收尾边沿"
     * 触发的桌面重建(回到 r103 行为)。                                */
    g_runVgaEdge   = (LazyGPPI( "omninine", "VgaEdge",   1 ) == 0) ? 0 : 1;
    /* 【r108】VhEdge —— 见 g_runVhEdge 声明处。0 = 关掉"主 VDD 刚走完一趟
     * hires→VGA→hires 往返"这条边沿触发的桌面重建(回到 r107 行为)。     */
    g_runVhEdge    = (LazyGPPI( "omninine", "VhEdge",    1 ) == 0) ? 0 : 1;
    /* 【r109】EntryHeal —— 见 g_runEntryHeal 声明处。
     * 【r109 默认反转 0 → 1】./108 的 t4(唯一一份 EntryHeal=1 的日志)实测:
     * 打开 command 后花屏一闪而过随即恢复正常, 而 t2/t3(默认, EntryHeal=0)
     * 是"打开就花、一直花到 exit"。用户对病征的答复("打开 DOS 盒立刻就花")
     * 与这条时间轴一致 ⇒ 入口侧补救从 A/B 升为默认。                    */
    g_runEntryHeal = (LazyGPPI( "omninine", "EntryHeal", 1 ) == 0) ? 0 : 1;
    /* 【r109】HealGate / EndHeal / PortProbe —— 见各自的声明处。 */
    g_runHealGate  = (LazyGPPI( "omninine", "HealGate",  1 ) == 0) ? 0 : 1;
    g_runEndHeal   = (LazyGPPI( "omninine", "EndHeal",   1 ) == 0) ? 0 : 1;
    /* 【r127】Clk8 —— 见 g_runClk8 声明处。三档: 0=不碰 / 1=只观察 / 2=确认后归位。
     * 【r129 修正一个静默失真的读取】r127/r128 这里是
     *     (LazyGPPI(...,"Clk8",1) == 0) ? 0 : 1
     * 也就是【任何非 0 值都被压成 1】—— 于是 r128 里"用 Clk8=2 才能恢复
     * r127 那一枪"这句话在代码上根本不成立: 写 2 读回来还是 1(只观察),
     * 一个字节都不会写。三档开关必须原样取值, 与 HwChk 同形。       */
    g_runClk8      = (WORD)LazyGPPI( "omninine", "Clk8", 2 );
    if (g_runClk8 > 2)
        g_runClk8 = 2;
    /* 【r128】Probe2 —— 见 g_runProbe2 声明处。默认 1(读 ATC 索引 10h)。
     * 【同上的读取纪律】它只有 0/1 两档, 但同样按"只有明确写 1 才算开"
     * 之外的写法取值, 免得将来加档位时再踩一次。                     */
    g_runProbe2    = (LazyGPPI( "omninine", "Probe2", 1 ) == 0) ? 0 : 1;
    /* 【r106】HwChk —— 见 g_runHwChk 声明处。0/1/2, 默认 1(只在安全点问)。 */
    g_runHwChk     = (WORD)LazyGPPI( "omninine", "HwChk", 1 );
    g_vddEdxValue = ((g_runVddVirt == 1) ||
                     (g_runVddVirt == 2 && wBpp >= 16)) ? 0xFFFFFFFFUL : 0UL;
    /* 策略字节随 PushState 推给 mini-VDD(此处 wBpp 已是本会话位深;
     * SetDisplayMode 里还会再推一次, 所以注册前一定是最新值)。
     * 【r101】参数从 4 个变成 6 个: 新增 bank(GET_VDD_BANK)与 int10(48 号),
     * 结构体 40 → 44 字节, 两侧的 VBE_STATE_SIZE 必须同步(vbeshare.inc)。 */
    VxdSetPolicies( (g_vddEdxValue == 0xFFFFFFFFUL) ? 1 : 0,
                    g_runVddBank, g_runInt10,
                    g_runVgaFix,                   /* r114: 第 7 个参数 */
                    g_runDispi,                    /* r116: 第 8 个参数 */
                    g_runInt10Fix,                /* r144: 第 9 个参数 */
                    g_runHiresFix,                 /* r146: 第 10 个参数 */
                    g_runVesa, g_runHires, (uint16_t)0,   /* [r412] Restore 已移除 */
                    0 );                          /* r406 C8: policy byte retired */
    /* 【r101】策略与硬件事实矛盾时留痕: VddVirtualize=0(EDX=0)配上高彩
     * 线性模式 = 向主 VDD 承诺"窗口化 4 平面虚拟化可用", 而 VBE 线性帧缓冲
     * 根本没有 4 平面通路 —— 这是 ./100 那一整轮花屏的直接来由。VddBank
     * (默认 1)会在 GET_VDD_BANK 里把这个承诺收回, 但"注册时仍宣称 0"这件
     * 事值得在日志里显式记一笔, 免得 A/B 时误读。                      */
    if (g_vddEdxValue == 0UL && wBpp >= 16)
        DIAGM( "WARN: EDX=0 (claims windowed 4-plane) with >=16bpp linear VBE" );
    /* Heal 的实际判据在 drv.c(轮询 VxD 侧 DISPLAY_DRIVER_DISABLING 计数) */
    VxdSetHeal( (uint16_t)g_runHeal );

    off = (WORD)LazyGPPI( "omninine", "DumpOff", 0x2B60 );
    if (off < 16 || off > 0xFF00)       /* 合理性夹取: 越界值回落默认 */
        off = 0x2B60;
    g_runDumpOff = off;

    /* 【r85】窗口宽度。蓝屏只给一个地址, 64 字节常常只够看到"出事那条
     * 指令"本身; 加宽到 256/512 就能看到它所在函数的序章与它前一条
     * call —— "这是谁的代码"正藏在那几十个字节里。                   */
    g_runDumpLen = (WORD)LazyGPPI( "omninine", "DumpLen", 64 );
    if (g_runDumpLen < 16 || g_runDumpLen > 512)
        g_runDumpLen = 64;

    /* 交给 drv.c: 选择子形态 + 转储偏移 */
    LfbSelModeSet( g_runSelMode );
    LfbSelVerifySet( g_runVerify );
    DiagSetDumpOff( g_runDumpOff );
    DiagSetDumpLen( g_runDumpLen );

    DiagHex( "run SelMode",  g_runSelMode );
    DiagHex( "run Verify",   g_runVerify );
    DiagHex( "run Access",   g_runAccess );
    DiagHex( "run DibEnable", g_runDibEnable );
    DiagHex( "run Flags",    g_runFlags );
    DiagHex( "run BitsOff",  g_runBitsOff );
    DiagHex( "run Clear",    g_runClear );
    DiagHex( "run DumpOff",  g_runDumpOff );
    DiagHex( "run DumpLen",  g_runDumpLen );
    DiagHex( "run Sentinel", g_runSentinel );
    DiagHex( "run EndShot",  g_runEndShot );
    DiagHex( "run SelfTest", g_runSelfTest );
    DiagHex( "run Crash sel", g_runCrashSel );
    DiagHex( "run Crash off", g_runCrashOff );
    DiagHex( "run CDPD", g_runCdpd );
    DiagHex( "run DelpDev", g_runDelpDev );
    DiagHex( "run DibDg",   g_runDibDg );
    DiagHex( "run Pal",     g_runPalMode );
    DiagHex( "run Xlat",    g_runXlat );
    DiagHex( "run Edx",     g_runEdx );
    DiagHex( "run VddVirtualize", g_runVddVirt );
    DiagHex( "run VesaPolicy",    g_runVesa );
    DiagHex( "run HiresAns",      g_runHires );
    DiagHex( "run Heal",          g_runHeal );
    DiagHex( "run ReSet",         g_runReSet );         /* r100 */
    DiagHex( "run Cursor",        g_runCursor );        /* r100 */
    DiagHex( "run Log",           g_logLevel );         /* r100 */
    DiagHex( "run VddBank",       g_runVddBank );       /* r101 */
    DiagHex( "run Int10Mode",     g_runInt10 );         /* r101 */
    DiagHex( "run Offscreen",     g_runOffscreen );     /* r102 */
    DiagHex( "run Access2",       g_runAccess2 );       /* r102 */
    DiagHex( "run VgaEdge",       g_runVgaEdge );       /* r104 */
    DiagHex( "run VhEdge",        g_runVhEdge );        /* r108 */
    DiagHex( "run EntryHeal",     g_runEntryHeal );     /* r108 */
    DiagHex( "run HealGate",      g_runHealGate );      /* r109 */
    DiagHex( "run EndHeal",       g_runEndHeal );       /* r109 */
    DiagHex( "run CbRestore",     g_runCbRestore );     /* r111 */
    DiagHex( "run HwFp",          g_runHwFp );          /* r111 */
    DiagHex( "run CbHwSet",       g_runCbHwSet );       /* r112 */
    DiagHex( "run CbDuty",        g_runCbDuty );        /* r112 */
    DiagHex( "run DdiTrace",      g_runDdiTrace );      /* r112 */
    DiagHex( "run HwChk",         g_runHwChk );         /* r106 */
    DiagHex( "run HwReg",         g_runHwReg );         /* r113 */
    DiagHex( "run VgaFix",        g_runVgaFix );        /* r114 */
    DiagHex( "run Dispi",         g_runDispi );         /* r116 */
    DiagHex( "run Int10Fix",      g_runInt10Fix );      /* r144 */
    DiagHex( "run HiresFix",      g_runHiresFix );      /* r146 */
    DiagHex( "run DispiFix",      g_runDispiFix );      /* r117 */
    DiagHex( "run DispiIdProbe",  g_runDispiIdProbe );  /* r119 */
    DiagHex( "run DispiPhys",     g_runDispiPhys );     /* r120 */
    DiagHex( "run SetModeText",   g_runSetModeText );   /* r126 */
    DiagHex( "run Clk8",          g_runClk8 );          /* r127 */
    DiagHex( "run Probe2",        g_runProbe2 );        /* r128 */
    DiagHex( "run CrtGeo",        g_runCrtGeo );        /* r130 */
    DiagHex( "run CrtEn",         g_runCrtEn );         /* r131 */
    DiagHex( "run ProtClr",       g_runProtClr );       /* r132 */
    DiagHex( "run Relatch",       g_runRelatch );       /* r133 */
    DiagHex( "run HwEss",         g_runHwEss );         /* r134 */
    DiagHex( "run VhWin",         g_runVhWin );         /* r134 */
    DiagHex( "run SessNoSet",     g_runSessNoSet );     /* r135 */
    DiagHex( "run EdgeDir",       g_runEdgeDir );       /* r135 */
    DiagHex( "run EntryIn",       g_runEntryIn );       /* r135 */
    DiagHex( "run RingDump",      g_runRingDump );      /* r136 */
DiagHex( "run EntryNoSet",    g_runEntryNoSet );    /* r137 */
DiagHex( "run EntryReplay",   g_runEntryReplay );   /* r137 */
DiagHex( "run SampleAtBoot",  g_runSampleAtBoot );  /* r138 */
DiagHex( "run CbNoSample",    g_runCbNoSample );    /* r138 */
DiagHex( "run HwTruth",       g_runHwTruth );       /* r138 */
DiagHex( "run AltFromTbl",    g_runAltFromTbl );    /* r138 */
DiagHex( "run HwChkPitch",    g_runHwChkPitch );    /* r138 */
DiagHex( "run BlindNeed",     g_runBlindNeed );     /* r139 */
DiagHex( "run EntryAlways",   g_runEntryAlways );   /* r139 */
DiagHex( "run VhAlways",      g_runVhAlways );      /* r139 */
DiagHex( "run DacStamp",      g_runDacStamp );      /* r139 */
DiagHex( "run ForceEdges",    g_runForceEdges );    /* r140 */
DiagHex( "run PitchWatch",     g_runPitchWatch );     /* r141 */
    DiagHex( "run WaitSafe",       g_runWaitSafe );       /* r142 */
    DiagHex( "run SnapSess",      g_runSnapSess );      /* r132 */
    DiagHex( "run AccessLog",     g_runAccessLog );     /* r105 */
    DiagHex( "run CursorSrc",     g_runCursorSrc );     /* r105 */
    DiagHex( "run edxValue lo",   (WORD)g_vddEdxValue );/* r100: 注册时真的传了 */
    DiagHex( "run edxValue hi",   (WORD)(g_vddEdxValue >> 16) );
    /* 【r103】关键四开关压成一行: 只要日志活着, 一眼就能确认本轮跑的是
     * 哪套配置(顺序: Offscreen / VddBank / EDX 是否 -1 / Log 级别)。
     * ./102 的教训: 日志只剩 1~2 行时, 配置完全无从追认。
     * 【r104】标签里的 r103 改成 r104 —— 这一行同时是"哪一版源码"的戳记,
     * 版本号写错会让下一轮把两版日志混在一起分析。
     * 【r105】再加一个字段: CursorSrc(光标形状取证时机)。陈旧块这一轮的
     * 主角是鼠标路径, 这个开关决定 `CursorShape:` 相关行出不出现, 必须
     * 能从一行里追认。
     * 【r106】标签改成 r106。HwChk 不进这一行 —— 它有自己的 `run HwChk`
     * 行, 而本行的缓冲只剩 5 个数字的余地(见 DiagCfg 的 buf 尺寸注),
     * 硬塞第 6 个值会溢出。
     * 【r108】标签改成 r108(版本戳记)。VhEdge 也不进这一行: 它有自己的
     * `run VhEdge` 行, 同样的溢出理由。
     * 【r109】标签改成 r109。新加的三个开关(HealGate/EndHeal/PortProbe)
     * 各有 `run ...` 行, 同样不进这一行 —— 缓冲只够 5 个数字。
     * 【r110】标签改成 r110。r110 的四个新开关(LogBoot/PortGrp/ReArm 与
     * 仍然默认关的 PortProbe)全部进下面那行 CFG2 —— 本行的 5 个字段与
     * r103 以来逐字节相同, 唯一变化就是这枚版本戳记。
     * 【r112】标签改成 r112(版本戳记, 字段不变)。
     * 【r113】标签改成 r113(版本戳记, 字段不变)。                      */
    DiagCfg( "CFG r128 Off/VddBank/EDX-1/Log/CursorSrc", g_runOffscreen,  /* r128-hot- boot only, stays DIAG */
             (uint16_t)g_runVddBank,
             (uint16_t)((g_vddEdxValue == 0xFFFFFFFFUL) ? 1 : 0),
             (uint16_t)g_logLevel,
             (uint16_t)g_runCursorSrc );
    /* 【r110】第二个 CFG 行: 本轮四个新开关。Log 那一行现在写的是【当前
     * 生效】的级别(开机相里可能是 LogBoot), 所以 LogBoot 必须单独出现,
     * 否则"这次开机到底按哪一档在记"无从追认。                        */
    DiagCfg( "CFG2 r128 LogBoot/PortGrp/ReArm/PortProbe/AccessLog",
             (uint16_t)g_logBoot,
             (uint16_t)g_runPortGrp,
             (uint16_t)g_runReArm,
             (uint16_t)0,   /* [r412] PortProbe 已移除 */
             (uint16_t)g_runAccessLog );
    /* 【r111】第三个 CFG 行: 本轮的"官方路径优先"三件套 + 一个判据。
     * 与 CFG/CFG2 同样的理由 —— 日志只剩几行时, "这一趟到底跑的是官方
     * 路径还是回落层"必须能从一行里追认。                            */
    DiagCfg( "CFG3 r128 CbRestore/HwFp/ReSet/VgaEdge/EndHeal",
             (uint16_t)g_runCbRestore,                   /* r111 */
             (uint16_t)g_runHwFp,                        /* r111 */
             (uint16_t)g_runReSet,                       /* r100 */
             (uint16_t)g_runVgaEdge,                     /* r104 */
             (uint16_t)g_runEndHeal );                   /* r109 */
    /* 【r112】第四个 CFG 行: 官方路径的【实现方式】三件套。./111 的教训是
     * "同一条官方路径, 里面那一枪是 4F02 还是寄存器回放, 结局完全不同"
     * (冻结 vs 活着) —— 这种事必须能从一行里追认。                     */
    DiagCfg( "CFG4 r128 CbHwSet/CbBios/CbDuty/DdiTrace/HwChk",
             (uint16_t)g_runCbHwSet,                     /* r112 */
             (uint16_t)0,   /* [r412] CbBios 已移除 */
             (uint16_t)g_runCbDuty,                      /* r112 */
             (uint16_t)g_runDdiTrace,                    /* r112 */
             (uint16_t)g_runHwChk );                     /* r106 */
    /* 【r113】第五个 CFG 行: 回落层【重建策略】的四件套。本轮的核心结论
     * 就落在这一行里 —— HwFp 从"否决票"退回"可选仪器"(默认 0), HwReg
     * 从开机相搬到"成功重建之后"(默认 1)。两个数一起读, 就能判断这一轮
     * 跑的到底是"r110 式的每次边沿都动硬件"还是"r112 式的指纹否决"。 */
    DiagCfg( "CFG5 r128 HwFp/HwReg/EntryHeal/ReArm/ReSet",
             (uint16_t)g_runHwFp,                        /* r113 */
             (uint16_t)g_runHwReg,                       /* r113 */
             (uint16_t)g_runEntryHeal,                   /* r108 */
             (uint16_t)g_runReArm,                       /* r110 */
             (uint16_t)g_runReSet );                     /* r100 */
    /* 【r114】第六个 CFG 行: 只报一件事 —— VgaFix。
     *   读法: 这一行 0000 = 与 r113 等价(默认); 0001 = 已允许 mini-VDD 在
     *   POST_HIRES_TO_VGA(5) 里补写文本取指/平面寄存器组。
     *   【但它证明不了测试机装的是新 VxD】VBE_VXD_VERSION 刻意没变(新旧
     *   VxD 可以混搭部署), 所以"新 VxD 真的到位"只能靠外屏阶段
     *   `TRC st bits=` 里出现 bit3(0008/000A/000B = VBE_TR_ST_VGAFIX)。 */
    DiagCfg( "CFG6 r128 VgaFix/none/none/none/none",
             (uint16_t)g_runVgaFix,                      /* r114 */
             0, 0, 0, 0 );
    /* 【r116】第七个 CFG 行: 只报一件事 —— Dispi。
     *   读法: 0001(默认) = mini-VDD 已被授权装 1CEh/1CFh 的 I/O handler
     *   并在 Windows 拥有 CRTC 时开全局陷阱; 0000 = 与 r115 等价。
     *   【但它证明不了测试机装的是新 VxD】VBE_VXD_VERSION 依旧没变(新旧
     *   VxD 可混搭部署), 所以"新 VxD 到位"只能靠 `TRC st bits=` 出现
     *   bit4(0010 = 陷阱已开)/bit5(0020 = 至少吃掉过一次 DISPI I/O)/
     *   bit6(0040 = 至少放行过一次)。 */
    DiagCfgMin( "CFG7 r128 Dispi/none/none/none/none",
             (uint16_t)g_runDispi,                       /* r116 */
             0, 0, 0, 0 );
    /* 【r117】第八个 CFG 行: DispiFix —— 只报"探针读到的 DISPI 不符时
     * 允不允许当场写回正确格式"。读法: 0001(默认) = 允许修复(但仍需
     * ID 认得出 + 真的不符两条同时成立才会写); 0000 = 纯观测。
     * 与 CFG7 的 Dispi 配对读, 就能把"陷阱挡住"与"回落层改回"分开归因:
     * Dispi=1/DispiFix=0 跑出来仍然花、且 DISPI probe verdict=0 ⇒ 保护
     * 没起作用或有人在别处改; Dispi=1/DispiFix=1 而 fix result=1 ⇒
     * 回落层确实把格式改回来了。                                        */
    DiagCfgMin( "CFG8 r128 DispiFix/none/none/none/none",
             (uint16_t)g_runDispiFix,                    /* r117 */
             0, 0, 0, 0 );
    /* 【r122, r126 复评】第九个 CFG 行: SetModeText —— "重设高分辨率之前先
     * 走一趟标准 VGA 文本模式(INT 10h AX=0003)"。
     * 读法: 0001 = 开; 0000 = 【默认】与 r122 的默认档同形。
     * 为什么它仍在配置行里: ./122 实测这一枪【不响】(开了也没用, 见变量声明处
     * 的长注), 于是它现在是"已验证无效、默认关"的一档; 保留 CFG9 是为了让
     * 任何一份日志都能自证"这一趟到底开没开", 而不是靠猜。            */
    DiagCfgMin( "CFG9 r128 SetModeText/none/none/none/none",
             (uint16_t)g_runSetModeText,                 /* r122, r126 复评 */
             0, 0, 0, 0 );
    /* 【r130】第十个 CFG 行: CrtGeo —— "发现扫描几何与帧缓冲不一致时强制
     * 重新走一遍模式设置"。
     * 读法: 0001 = 【默认】开; 0000 = 只读数不重设(A/B 对照档)。
     * 【为什么它必须出现在配置行里】这一枪会【真的动硬件】(两次 4F02h),
     * 是 r130 唯一的行为改动 ⇒ 任何一份日志都必须能自证它开没开, 否则
     * 下一轮会把"开了但没修好"和"根本没开"混成同一个结论。           */
    DiagCfgMin( "CFG10 r130 CrtGeo/none/none/none/none",
             (uint16_t)g_runCrtGeo,                      /* r130 */
             0, 0, 0, 0 );
    /* 【r131】CFG11 是与 CFG10 配对的一行: 上一行管 CR00/CR01(行宽按字符
     * 算出来的像素数), 这一行管"这一行到底按不按字符算"(CR17h bit7 = SE)。
     * 下一轮判读时两行必须一起看 —— 几何自检报 ok 而这一行报 0, 就是
     * "长条"的现场。                                                     */
    DiagCfgMin( "CFG11 r131 CrtEn/none/none/none/none",
             (uint16_t)g_runCrtEn,                       /* r131 */
             0, 0, 0, 0 );
    /* [r132] CFG12: this round's two new behaviours, each with its own INI
     * switch, so that any single line of this log says what was actually on:
     *   ProtClr  = clear CR11h bit7 before every mode set (the fix),
     *   SnapSess = tick-driven CRTC snapshot during/after a DOS session.  */
    DiagCfgMin( "CFG12 r132 ProtClr/SnapSess/none/none/none",
             (uint16_t)g_runProtClr,                     /* r132 */
             (uint16_t)g_runSnapSess,                    /* r132 */
             0, 0, 0 );
    /* [r133] CFG13: this round has exactly one new behaviour (Relatch) plus
     *   the two reverted defaults, and all three are one INI line each, so the
     *   next round can separate "the fix" from "the regression" without a
     *   rebuild:
     *     Relatch = gate-closed reprogram, gate opened last (drv.c).
     *   Read this line together with "run HiresAns" and "run ProtClr". */
    DiagCfgMin( "CFG13 r133 Relatch/ProtClrDefault/HiresDefault/none/none",
             (uint16_t)g_runRelatch,                     /* r133 */
             (uint16_t)g_runProtClr,                     /* r133: default 0 */
             (uint16_t)g_runHires,                       /* r133: default 1 */
             0, 0 );
    /* [r134] CFG14: this round changes exactly two decisions and adds one
     *   switch, and each of them is one INI line, so the next round can still
     *   separate them without a rebuild:
     *     HwEss   = which verdict decides the OFFICIAL restore (1 = meaning,
     *               0 = the r133 byte count that vetoed it 5 times out of 6).
     *     VhEdge  = whether the POST_VGA_TO_HIRES edge bypasses the suppress
     *               gate (1 = [r134 default] it does // 0 = the r111..r133
     *               behaviour, where that gate stopped the rebuild -- see the
     *               VhEdge note in MaybeDeferredRestore). */
    DiagCfgMin( "CFG14 r134 HwEss/VhWin/none/none/none",
             (uint16_t)g_runHwEss,                       /* r134: default 1 */
             (uint16_t)g_runVhWin,                       /* r134: 1 = edge wins */
             0, 0, 0 );
    /* [r135] CFG15: 本轮只改三处【决定】, 每处一条 INI, 不重编就能分离因果:
     *   SessNoSet = 会话收尾边沿是否允许设模式(1 = 只重绘并把重建推到事务后);
     *   EdgeDir   = 入口侧闸门是否改用【方向】(有序证据)而不是瞬时位;
     *   EntryIn   = 方向判为"盒子拿屏"时是否仍照 r134 那样插一次 4F02。 */
    DiagCfgMin( "CFG16 r137 EntryNoSet/EntryReplay/ArSnap/none/none",
               (uint16_t)g_runEntryNoSet,                   /* r137: default 1 */
               (uint16_t)g_runEntryReplay,                  /* r137: default 1 */
               1,                                           /* r137: AR snapshot is unconditional */
               0, 0 );
    /* [r138] CFG17: 本轮的四处【决定】(顺序与 r138 的改动清单一致):
     *   SampleAtBoot = 开机相就采一份"已知良好模式寄存器"样本(纯读);
     *   CbNoSample   = 官方回调在"没有可用样本"时判失败(DDK 义务没做完)
     *                  ⇒ 交给回落层; 1 = 新行为, 0 = r137 行为;
     *   HwTruth      = 用 VBE 4F03h(硬件当前模式号)参与"模式对不对"的判据;
     *   AltFromTbl   = ForceModeReprogram 的中间模式从本卡自己的模式表里挑
     *                  (不再用硬编码的 0112h —— 它是 640x480x24bpp, 不是
     *                  注释里写的 8bpp)。
     *   四行都在 vbe9xlog 里, 与 run SampleAtBoot / run CbNoSample 互为对照。 */
    DiagCfgMin( "CFG17 r138 SampleAtBoot/CbNoSample/HwTruth/AltFromTbl/HwPitch",
               (uint16_t)g_runSampleAtBoot,                 /* r138: default 1 */
               (uint16_t)g_runCbNoSample,                   /* r138: default 1 */
               (uint16_t)g_runHwTruth,                      /* r138: default 1 */
               (uint16_t)g_runAltFromTbl,                   /* r138: default 1 */
               (uint16_t)g_runHwChkPitch );                 /* r138: default 1 */
    DiagCfgMin( "CFG15 r136 SessNoSet/EdgeDir/EntryIn/RingDump/EntryHeal",
             (uint16_t)g_runSessNoSet,                   /* r135: default 1 */
             (uint16_t)g_runEdgeDir,                     /* r135: default 1 */
             (uint16_t)g_runEntryIn,                     /* r135: default 0 */
             (uint16_t)g_runRingDump,                    /* r136: default 1 */
             (uint16_t)g_runEntryHeal );                 /* r109: default 1 */
    /* [r139] CFG18: 本轮四处【决定】。./138 的量测已经把病灶钉成"硬件按 8bpp
     *   扫描我们的 16bpp 显存"(见 g_runBlindNeed 上方的长注), 而这四处修的
     *   正是"为什么整整 4 分钟里一次 4F02h 都没发出去":
     *     BlindNeed   = 让两条"整组寄存器看不见"的见证(4F03h + DAC 印章)
     *                   参与判据; 0 = 退回只信寄存器样本(r138 行为);
     *     EntryAlways = 入口边沿永远挂上延迟恢复(绝不"什么都不挂"), 且见证说
     *                   "不是我们的"时这一次恢复里包含真正的模式建立;
     *     VhAlways    = POST_VGA_TO_HIRES(7 号, DDK 那 6 步的第 6 步 = 最后一步)
     *                   永不被"方向推断"否决;
     *     DacStamp    = 每次成功建立模式后往 DAC 前 8 项盖章(只在 bpp != 8 时),
     *                   把"我们做过模式建立"变成硬件里的物证。               */
    DiagCfgMin( "CFG18 r139 BlindNeed/EntryAlways/VhAlways/DacStamp/none",
             (uint16_t)g_runBlindNeed,                   /* r139: default 1 */
             (uint16_t)g_runEntryAlways,                 /* r139: default 1 */
             (uint16_t)g_runVhAlways,                    /* r139: default 1 */
             (uint16_t)g_runDacStamp,                    /* r139: default 1 */
             (uint16_t)0 );
    /* [r140] CFG19: 本轮的决定 —— ./139 的量测逼出来的:
     *   ForceEdges = 会话边沿一律重建(r139 的实际行为写成明文规则)。
     *   (OwnGate 判据已于 r422 随 HwIsOursNow 一并删除。)            */
    DiagCfgMin( "CFG19 r140 ForceEdges/none/none/none/none",
             (uint16_t)g_runForceEdges,                  /* r140: default 1 */
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0 );
    /* [r141] CFG20: 本轮的一处【判据换人】。./140 的五次重建里, 4F06h 的
     *   每行字节数在重建前读到 0400h(或 0800h)、重建后一律 0800h ——
     *   而另两条见证(4F03h 模式号 / GCR06 物证)在整份日志里恒定不变。
     *   PitchWatch=1 时: 判据改用 4F06h, 并在 cursor tick 上守望。
     *   详见 g_runPitchWatch 上方长注与 R141_DELIVERY.md。 */
    DiagCfgMin( "CFG20 r141 (retired)/none/none/none/none",
             (uint16_t)g_runPitchWatch,                  /* r141: default 1 */
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0 );
    /* [r144] CFG22: 本轮的一处行为改动 —— 48 号里把 Far-East 文本模式
     *   72h/73h 改写成 03h(仅全屏盒子持屏期 = mini-VDD 4→7 武装期内)。
     *   依据是用户的现场结论: 全屏花屏必须打 `mode co80` 才能回来。
     *   它的物证是 mini-VDD 侧的计数器 VBE_TR_FBOX_I10MAP("TRC fbox
     *   i10map 72/73->03"), 0 = 改写从未跑过。                     */
    DiagCfgMin( "CFG22 r144 Int10Fix/none/none/none/none",
             (uint16_t)g_runInt10Fix,                    /* r144: default 1 */
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0 );
    /* [r146] CFG23: 三个数分别是"边沿一律置请求 / 守望者最小间距 /
     *   mini-VDD 存回模式字节的开关"; r145 的那两个(突发预算/节流窗口)
     *   已经撤除, 见 drv.c 里 s_edgeSkipEnd 的长注。原来那段注:
     *   [r145] CFG23: 本轮的两个修正都在判据/边沿层, 没有新开关:
     *   ① 入口边沿的"盒子持屏"判据修正(VxdBoxArmedNow 只认 1);
     *   ② 边沿的"每次开机 8 次"永久上限 → 突发预算 + 独立节流,
     *     并在一次成功重建后清零。
     *   两个数字写进日志是为了"为什么某条边沿没被接受"
     *   能直接读出来: 第一个 = 突发预算(连续失败到这个数就降速),
     *   第二个 = 节流窗口(DDI 调用次数)。 */
    /* [r153] CFG28: 第四栏 MemCFix 仍然是本轮的"行为总闸", 但它现在
     *   直通到 VxD(drv.c 的 `memcfix ? 1 : 0` 已于 r153 删除)。
     *   判读这一行时必须看到 `... 0001 0002 0004`: 倒数第二栏 = MemCFix
     *   = 2(取指基址修复), 最后一栏 = HiresAns = 4(DDK 答案)。
     *   看到 0000 说明 INI 把它关了 —— 那时探针照跑、一个字节都不写。
     *   【与 VxD 侧对账】drv.c 的 `PH: probe policy=` 必须和这一行相等。 */
    DiagCfgMin( "CFG35 EdgeAlways/PitchGapTicks/HiresFix/Retired/HiresAns",
             (uint16_t)1,                                /* r146: 边沿一律置请求 */
             (uint16_t)PITCH_GAP_TICKS,                  /* r146: 守望者最小间距 */
             (uint16_t)g_runHiresFix,                    /* r146: mini-VDD 存/写回 */
             (uint16_t)g_runHires,                    /* r151: 4 = DDK 校验 */
             (uint16_t)0 );                             /* r406 C8: MemCFix column retired */
    /* [r142/r143] CFG21: WaitSafe = 入口边沿重建是否等“切屏事务收尾”。
     *   【r143：这个开关已退休，默认 0】./142 证明两件事:
     *     ① 这条分支从来没执行过(wArmedEntry 在实际跑的
     *        入口边沿上是 0) —— 日志里没有
     *        `entry arm -> wait for the switch to finish (r142)`;
     *     ② 它针对的“顺序被后写覆盖”也不成立(4F06h 从那一刻
     *        起一直是 0800h, 而真正的切屏事务在 100 秒之后)。
     *   读法: WaitSafe=0(默认)时应当看到 `no safe signal -> fallback (r142)`
     *   或 `safe signal taken -> hardware ours (r142)`; 看到 entry-arm 那一行就说明
     *   本轮把它重新接上了(那不是 r143 的行为)。 */
    DiagCfgMin( "CFG21 r142 WaitSafe/none/none/none/none",
             (uint16_t)g_runWaitSafe,                    /* r143: default 0 (retired) */
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0,
             (uint16_t)0 );
    DiagHex( "run Log session",   g_runLog );           /* r110: 会话期级别 */
    FLUSH();
    /* 【r107】配置块的 tick 基线: 后面任何一行的 T= 减去它, 就是"开机后
     * 过了多少拍" —— 尾部离崩溃多远、尾部的 T 到底动不动, 全部有据。 */
    DiagMark( "CFG" );
    FLUSH();
}

/*
 * GetModuleDgroupSel —— 取一个已加载 16 位 DLL 的 DGROUP 选择子。
 *
 * 【正确布局 —— v1 的误诊在这里】模块句柄是"内存态模块数据库"的选择子,
 * 它的前 0x40 字节虽然与磁盘 NE 头同源, 但自 +0x02 起各字段已被加载器
 * 改写为【内存态】含义(wine 的 NE_MODULE 结构即照 Pietrek《Windows
 * Internals》p.219 的 Win95 模块数据库转储, 与真实 KRNL386 一致):
 *     +0x08 WORD  指向 DGROUP 段表项的【近指针】(段表内偏移)
 *     +0x0E WORD  DGROUP 的逻辑段号(= 磁盘 NE 头的 ne_autodata)
 *     +0x1C WORD  段数 cseg
 *     +0x22 WORD  段表的【近指针】(相对模块数据库段起点)
 *     +0x24 WORD  资源表近指针(紧跟段表分配, 用来反推段表项步长)
 * 段表每项 8 字节(自 Win3.1 至 WinMe 不变), 首字 = 该段的选择子
 * (未加载为 0)。
 * v1 按【磁盘】NE 头解读: +0x0A 取"自动数据段号"(实为文件信息近指针)、
 * +0x2C 取"段表 far 指针"(实为非驻留名表的【文件偏移】, 作为段:偏移
 * 即 ES=0 的野指针), 一读内存就 GP —— 发生在 DriverInit 内, KRNL386
 * 表现为 LoadFail = omninine.drv Failure code is 0000, 入口 C 代码一行
 * 都执行不到。这就是跨 Win98/WinME 恒定复现的加载失败根因。
 *
 * 【本版读数纪律】所有指针都只解析到模块数据库段内部; 布局校验不过
 * 就返回 0(配合 DIAG 日志定位), 绝不产生野 far 指针。
 */
static WORD GetModuleDgroupSel( HINSTANCE hMod )
{
    BYTE __far *db;
    WORD        magic;
    WORD        autoSeg, cseg, segTab, resTab, dgroupSte;
    WORD        stride, ste, sel;

    if ( !hMod )
        return 0;

    db    = (BYTE __far *)MK_FP( hMod, 0 );
    magic = *(WORD __far *)db;
    if ( magic != 0x454E )              /* 'NE' */
        return 0;

    autoSeg   = *(WORD __far *)( db + 0x0E );
    cseg      = *(WORD __far *)( db + 0x1C );
    segTab    = *(WORD __far *)( db + 0x22 );
    resTab    = *(WORD __far *)( db + 0x24 );
    dgroupSte = *(WORD __far *)( db + 0x08 );

    if ( !autoSeg || !cseg || !segTab || autoSeg > cseg )
        return 0;

    /* 段表项步长 = (资源表偏移 - 段表偏移) / 段数。KRNL386 创建模块
       数据库时各表紧邻分配(Win3.1 起如此), 距离必为 项数×步长。
       期望 8; 得不到合理值就拒绝, 不猜。 */
    if ( resTab <= segTab || (WORD)( resTab - segTab ) % cseg != 0 )
        return 0;
    stride = (WORD)( resTab - segTab ) / cseg;
    if ( stride != 8 && stride != 10 )
        return 0;

    ste = (WORD)( segTab + ( autoSeg - 1 ) * stride );

    /* 交叉验证: +0x08 由 KRNL386 维护(wine NE_CreateModule 同款),
       指向 DGROUP 的段表项。它若指向段表范围内却与我们推算不符,
       说明布局假设错误 —— 安全拒绝; 为 0 或越界视为未维护, 忽略。 */
    if ( dgroupSte >= segTab && dgroupSte < resTab && dgroupSte != ste )
        return 0;

    /* 段表项首字 = 选择子 */
    sel = *(WORD __far *)( db + ste );
    if ( sel == 0 || sel == 0xFFFF )    /* 段未加载 / 无效 */
        return 0;
    return sel;
}

/*==========================================================================
 * r94 根因: 喂给 DIBENG 的 DGROUP 选择子取错了 —— 改用"装载器桩自证"
 *
 * 【现场】r76b..r93 每一版都崩在 Enable(style=0) 返回之后, 且日志里没有
 *   任何一条转发器探针 —— 也就是说 GDI 第一次经我们的跳板进 DIBENG 时就
 *   死了, 形态是"从某个结构里取出一个远指针, 然后 far call 进 DPMI 堆
 *   (0607:00001B5C)"。
 * 【机理】16 位 Windows 的约定: 远调用/jmp 进 DLL 导出函数之前, 调用方
 *   必须把【被调 DLL 的 DGROUP】放进 AX —— 被调方序章是 "push ds /
 *   mov ds, ax"。我们走的是动态解析(不静态导入 DIBENG), 这个 AX 只能自己
 *   给。而它的来源 GetModuleDgroupSel() 读出来的并不是选择子:
 *     r93 日志: MOD DISPLAY dgroup sel=2C14
 *   而 2C14 恰好是 omninine.drv 里【DGROUP 段表项的第一个字 = 文件偏移 sect】
 *   (本地解 NE 头核对: seg#2 sect=2C14 len=2EB4, ne_autodata=2)。
 *   即: 那块内存态模块库的段表里放的是磁盘字段, 我们把【文件偏移】当选择子
 *   喂进了 DIBENG 的 "mov ds, ax" —— 引擎于是把它的全局量(它自己的"当前
 *   设备"指针就在 DS:05FD)全读到别的段去, 取出一个垃圾远指针就 far call。
 *   ⇒ 这解释了此前所有"想不通"的现象: 改 delpPDevice / deFlags / CDPD /
 *     自检 / PDEV 内容都不影响崩溃地址(病灶根本不在 PDEV 里), 崩点固定在
 *     同一个堆地址(错段里的内容是确定的), 以及 CreateDIBPDevice 明明"成功"
 *     却像什么都没做(它在错的 DS 上干活)。
 * 【修法】不猜模块库布局 —— 让【装载器自己造的桩】把答案说出来。
 *   Win16 装载器会给"导出项里标记了需要装载数据段"的导出造一个桩:
 *       mov ax, <该模块的 DGROUP>     ; B8 lo hi
 *       jmp far <真实入口>
 *   (r93 日志里 DIB_Enable=035F:0182 —— 段基址 0x0008F1A0、界限仅 0x21F 的
 *   小代码段 —— 就是这种桩。这也正是"唯独 DIB_Enable 那一调成功(GDIINFO
 *   的 dpCurves..dpStyleLen 与 C1_DIBENGINE 被正确填上), 其余 35 个原始
 *   入口一进就崩"的原因: 桩自己会装 DS, 原始入口要靠我们给的 AX。)
 *   于是:
 *     ① 遍历 36 个已解析入口, 只挑"所在段是代码、可读、且界限 ≤ 0x400"的
 *        —— 桩段都很小, DIBENG 真正的代码段都是数 KB;
 *     ② 在前 24 字节里找 B8 lo hi, 把立即数当选择子【验证】:
 *        存在位=1 / DPL=3 / S=1 / E=0(数据段) / W=1 / 界限 ≥ 0x200 / 基址≠0
 *        —— 桩里的立即数按定义就是被调模块的数据段, 这几条正好把误报挡掉;
 *     ③ 得票最多者胜(同一模块的桩里立即数必然相同, 票数就是佐证)。
 * 【安全】① 全程只读: DPMI 000Bh 读描述符 + 读【已验证可读】的代码段, 不写
 *      任何描述符、不碰任何段内容; 扫描范围还被段的界限夹住, 不会越界;
 *   ② 找不到桩 / 验证不过 / DibDg=0 → 保留原值并落盘, 行为与 r93 逐指令
 *      等价(本修正只在"确实找到一个合法的数据段立即数"时才改变一个变量的
 *      值, 而这个变量旧值已被证明是文件偏移, 不可能是合法数据段选择子);
 *   ③ 候选、票数、候选的描述符、最终值全部落盘, 下一个日志即可复核。
 *
 * 【r95 修正 —— 桩的真实语义与采纳规则】
 *   · r94 的取舍规则里多加了一条"立即数后面必须紧跟 jmp(E9/EA)"的假设。
 *     实测(r94 日志)桩是 "B8 imm" 开头、紧跟的却不是 E9/EA —— 于是 r94 找到
 *     了正确的立即数却把它否掉了, DS 依旧没有被装, 蓝屏地址一模一样。
 *   · 更关键的是桩的语义被读反了: 桩不是"给调用方提供 AX 让被调方自己装 DS"
 *     (DIBENG 的原始入口里根本没有 mov ds,ax), 而是【桩自己把 DS 装好】再跳
 *     进真实入口 —— 静态导入(CreateDIBPDevice)与 DIB_Enable 都走桩, 所以它们
 *     一直正常; 我们动态拿到的另外 35 个原始入口没有桩可用, 必须自己装 DS。
 *     这正是 vddapi16.asm 的 DIBJUMP 现在做的事(仅当 g_dibDgTrusted)。
 *   · 因此本探针的产出物从"AX 的值"变成"DS 的值", 采纳条件放宽为:
 *     桩首 mov ax + 立即数通过 DibDgValid, 或紧跟跳转, 或多桩一致。
 *   · 桩入口前 12 字节与解码出的远跳目标一并落盘, 供离线复核。
 *========================================================================*/
#define DIBDG_THUNK_MAX   0x400     /* "桩段"的界限上限                    */
#define DIBDG_SCAN_BYTES  24        /* 桩里只看前 24 字节                  */
#define DIBDG_MAX_CAND    8
#define DIBDG_MIN_LIM     0x0219UL  /* DGROUP 不可能比这更小                */

/* DibDgValid —— 候选必须是"3 环、可写、非 0 基址、界限像模像样"的数据段。 */
static int DibDgValid( WORD sel, DWORD __far *pBase, DWORD __far *pLim )
{
    DWORD base, lim;
    WORD  acc;

    if (!DescRead( sel, &base, &lim, &acc ))
        return 0;
    if (!(acc & 0x0080))                /* 存在位 P                     */
        return 0;
    if ((acc & 0x0060) != 0x0060)       /* DPL = 3                      */
        return 0;
    if ((acc & 0x001A) != 0x0012)       /* S=1, E=0(数据), W=1          */
        return 0;
    if (lim < DIBDG_MIN_LIM)
        return 0;
    if (base == 0)                      /* DGROUP 不可能 0 基址         */
        return 0;
    *pBase = base;
    *pLim  = lim;
    return 1;
}

/* DibDgroupProbe —— 用装载器桩里的立即数修正 wDibDgroup。
 *   返回 1 = 已改写; 0 = 不改(保持原值, 与 r93 行为等价)。
 *   由 PDEVICE 相在 ReadRunProfile() 之后调用一次: 那里 g_dibfn[] 已解析、
 *   DibDg 开关已读入, 而本相后面所有进 DIBENG 的调用都能吃到修正值。 */
static int DibDgroupProbe( void )
{
    static int done = 0;
    WORD  cand[DIBDG_MAX_CAND];
    WORD  vote[DIBDG_MAX_CAND];
    WORD  startVote[DIBDG_MAX_CAND];
    WORD  jmpVote[DIBDG_MAX_CAND];
    WORD  shapeVote[DIBDG_MAX_CAND];    /* r95: 立即数后紧跟 8E D8(mov ds,ax) */
    WORD  farOff[DIBDG_MAX_CAND];       /* r95: 桩尾 EA 远跳的偏移(诊断用)   */
    WORD  farSeg[DIBDG_MAX_CAND];       /* r95: 桩尾 EA 远跳的段             */
    BYTE  dump[DIBDG_SCAN_BYTES];       /* r95: 桩入口前若干字节(落盘复核用) */
    WORD  dumpSel = 0, dumpOff = 0;
    int   haveDump = 0;
    int   nCand = 0, nThunk = 0, nScanned = 0;
    int   i, k, j, best = -1;
    DWORD base, lim;
    WORD  acc;

    if (done)
        return 0;
    done = 1;

    for (i = 0; i < DIBFN_COUNT; ++i)
    {
        const BYTE __far *b;
        WORD  off, imm;
        DWORD room, scanMax;

        if (!g_dibfn[i])
            continue;
        if ((WORD)(g_dibfn[i] >> 16) == 0)          /* [r447] 桩指针无选择子 */
            continue;
        if (!DescRead( (WORD)(g_dibfn[i] >> 16), &base, &lim, &acc ))
            continue;
        if (!(acc & 0x0080))                    /* 存在                 */
            continue;
        if ((acc & 0x0060) != 0x0060)           /* DPL3                 */
            continue;
        if ((acc & 0x001A) != 0x001A)           /* 代码段且可读         */
            continue;
        if (lim > (DWORD)DIBDG_THUNK_MAX)       /* 只怀疑"小段" = 桩    */
            continue;
        ++nThunk;

        /* 扫描范围必须夹在该段界限之内(p[i] 读越界就是一次 #GP) */
        off  = (WORD)g_dibfn[i];
        room = (lim > (DWORD)off) ? (lim - (DWORD)off) : 0;
        scanMax = (room > (DWORD)DIBDG_SCAN_BYTES) ? (DWORD)DIBDG_SCAN_BYTES
                                                   : room;
        if (scanMax < 3)
            continue;

        b = (const BYTE __far *)g_dibfn[i];
        nScanned++;

        /* r95: 把【第一个】桩入口的前 12 字节抄进日志 —— 桩的形状是判读
         * "立即数到底是不是 DIBENG 的 DGROUP"的唯一凭据, 落盘一次足够。 */
        if (!haveDump)
        {
            int m, n = (int)scanMax;
            if (n > DIBDG_SCAN_BYTES)
                n = DIBDG_SCAN_BYTES;
            for (m = 0; m < n; ++m)
                dump[m] = b[m];
            while (m < DIBDG_SCAN_BYTES)
                dump[m++] = 0;
            dumpSel = (WORD)(g_dibfn[i] >> 16);
            dumpOff = (WORD)g_dibfn[i];
            haveDump = 1;
        }

        for (k = 0; (DWORD)k + 2 < scanMax; ++k)
        {
            int isJmp, isMovDs;
            if (b[k] != 0xB8)                   /* mov ax, imm16        */
                continue;
            imm = (WORD)b[k + 1] | ((WORD)b[k + 2] << 8);
            if (!DibDgValid( imm, &base, &lim ))
                break;                          /* 立即数不是数据段     */
            /* 【r95 实测的桩形状】"mov ax,<DGROUP>" 之后有两种写法:
             *   · 8E D8 (mov ds, ax) 再跳走 —— 桩自己装 DS;
             *   · 直接 E9/EA 跳走            —— 只供 AX, DS 由被调方自理。
             * shapeVote 记前者、jmpVote 记后者: 两种都算"这是桩"。 */
            isMovDs = ((DWORD)k + 4 < scanMax)
                   && (b[k + 3] == 0x8E) && (b[k + 4] == 0xD8);
            isJmp   = ((DWORD)k + 3 < scanMax)
                   && (b[k + 3] == 0xE9 || b[k + 3] == 0xEA);
            for (j = 0; j < nCand; ++j)
                if (cand[j] == imm)
                    break;
            if (j < nCand)
                vote[j]++;
            else if (nCand < DIBDG_MAX_CAND)
            {
                cand[nCand] = imm;
                vote[nCand]  = 1;
                startVote[nCand] = 0;
                jmpVote[nCand]   = 0;
                shapeVote[nCand] = 0;
                farOff[nCand]    = 0;
                farSeg[nCand]    = 0;
                nCand++;
            }
            if (j < nCand)
            {
                if (k == 0)                             /* 桩首就是 mov ax */
                    startVote[j]++;
                if (isJmp)                              /* 紧跟跳转        */
                    jmpVote[j]++;
                if (isMovDs)                            /* 先 mov ds, ax   */
                    shapeVote[j]++;
                /* 桩尾若是远跳, 顺手把真实入口地址解出来: 那就是这个导出的
                 * 原始入口, 写进日志可离线核对"立即数是不是 DIBENG 的
                 * DGROUP"(原始入口必然落在 DIBENG 的代码段里)。 */
                if ((DWORD)k + 7 < scanMax && b[k + 3] == 0xEA)
                {
                    farOff[j] = (WORD)b[k + 4] | ((WORD)b[k + 5] << 8);
                    farSeg[j] = (WORD)b[k + 6] | ((WORD)b[k + 7] << 8);
                }
                else if ((DWORD)k + 9 < scanMax && b[k + 3] == 0x8E
                      && b[k + 4] == 0xD8 && b[k + 5] == 0xEA)
                {
                    farOff[j] = (WORD)b[k + 6] | ((WORD)b[k + 7] << 8);
                    farSeg[j] = (WORD)b[k + 8] | ((WORD)b[k + 9] << 8);
                }
            }
            break;                              /* 每个入口只看第一处   */
        }
    }

    DiagHex( "DS: thunk-like entries", (WORD)nThunk );
    DiagHex( "DS: entries scanned",    (WORD)nScanned );
    DiagHex( "DS: parsed dgroup",      wDibDgroup );
    if (haveDump)
    {
        DiagHex( "DS: thunk seg",  dumpSel );
        DiagHex( "DS: thunk off",  dumpOff );
        DiagHex( "DS: thunk w0", (WORD)(dump[0] | ((WORD)dump[1] << 8)) );
        DiagHex( "DS: thunk w1", (WORD)(dump[2] | ((WORD)dump[3] << 8)) );
        DiagHex( "DS: thunk w2", (WORD)(dump[4] | ((WORD)dump[5] << 8)) );
        DiagHex( "DS: thunk w3", (WORD)(dump[6] | ((WORD)dump[7] << 8)) );
        DiagHex( "DS: thunk w4", (WORD)(dump[8] | ((WORD)dump[9] << 8)) );
        DiagHex( "DS: thunk w5", (WORD)(dump[10] | ((WORD)dump[11] << 8)) );
    }
    for (j = 0; j < nCand; ++j)
    {
        DiagHex( "DS: cand",       cand[j] );
        DiagHex( "DS: cand votes", vote[j] );
        DiagHex( "DS: cand at-start votes", startVote[j] );
        DiagHex( "DS: cand jmp-follow votes", jmpVote[j] );
        DiagHex( "DS: cand mov-ds votes", shapeVote[j] );
        if (farSeg[j] || farOff[j])
        {
            DiagHex( "DS: cand jmp far seg", farSeg[j] );
            DiagHex( "DS: cand jmp far off", farOff[j] );
        }
        if (DibDgValid( cand[j], &base, &lim ))
        {
            DiagHex( "DS: cand base lo", (WORD)base );
            DiagHex( "DS: cand base hi", (WORD)(base >> 16) );
            DiagHex( "DS: cand lim",     (WORD)lim );
        }
    }
    /* 【r95 采纳规则】候选满足其一即采纳:
     *   · startVote ≥ 1 —— 【桩首】就是 "mov ax, imm", imm 又通过了
     *     DibDgValid(存在/DPL3/数据段/可写/界限≥0x200/基址≠0), 而它所在的
     *     段早已被 nThunk 闸门限定为"界限 ≤ 0x400 的小代码段"。这正是装载器
     *     桩的形状。r94 之所以漏掉它, 是在此之上多加了一条"立即数后必须
     *     紧跟 jmp"的假设 —— 实测的桩是 "mov ax,imm; 8E D8; …"。
     *   · jmpVote ≥ 1 —— 立即数后紧跟近/远跳转;
     *   · vote ≥ 2    —— 同一模块的多个桩给出同一个立即数。
     * 三条都不满足 → 判为碰巧的字节花样, 保持 r93 行为(不装 DS, 更安全)。 */
    for (j = 0; j < nCand; ++j)
    {
        if (startVote[j] < 1 && jmpVote[j] < 1 && vote[j] < 2)
            continue;
        if (best < 0 || vote[j] > vote[best])
            best = j;
    }
    FLUSH();

    if (!g_runDibDg)
    {
        DIAG( "DS: probe off (DibDg=0) -> keep the module-database value" );
        FLUSH();
        return 0;
    }
    if (best < 0)
    {
        DIAG( "DS: no loader thunk found -> keep the module-database value" );
        FLUSH();
        return 0;
    }

    wDibDgroup = cand[best];
    g_dibDgTrusted = 1;             /* r95: 只有这一刻起才允许装进 DS       */
    DIAG( "DS: wDibDgroup <- loader thunk immediate (validated, r95 fix)" );
    DiagHex( "DS: wDibDgroup final", wDibDgroup );
    DiagHex( "DS: dg trusted", g_dibDgTrusted );
    FLUSH();
    return 1;
}

/*
 * ResolveDIBEngine —— 动态解析全部 36 个 DIB 引擎入口到 g_dibfn[]。
 *   返回 1 = 全部解析成功; 0 = 有缺失(诊断构建会在日志里逐一点名)。
 *
 *   【调用时机 —— 绝不能在加载期】本驱动由 KRNL386 引导期直接加载,
 *   此刻 GDI 未启动、DIBENG 不在内存(./5 构建实测: 加载期解析 +
 *   LoadLibrary 兜底在 KERNEL 内 GP 蓝屏 0D:0327:00000683)。解析只
 *   由 DibEnsureResolved 在 GDI 期(首次转发或首次 Enable, DIBENG 已
 *   随 GDI 驻留)发起。
 *
 *   【解析顺序】
 *     1. GetModuleHandle("DIBENG") —— GDI 期正常路径, DIBENG 已驻留。
 *     2. 若未加载, 取 KERNEL 的 LoadLibrary 装进它(GDI 期重入安全;
 *        仅作兜底, 正常永走到)。
 *     3. 从模块数据库取出 DIBENG 的 DGROUP 选择子存入 wDibDgroup,
 *        供汇编跳板在 jmp 前装进 AX(__loadds 序章约定)。
 *   【每项入口】只按名字 GetProcAccess —— 见 g_dibfnTab 注释: 序号
 *   兜底的"远指针段字为 0"约定与 C 编译器的 DS:偏移传参冲突, 会在
 *   KERNEL 内 GP, 已砍。缺失时记日志点名, 统一返回 0。
 */
static int ResolveDIBEngine( void )
{
    typedef HINSTANCE (WINAPI *PLOADLIB)( LPCSTR );

    HINSTANCE hDib;
    FARPROC   pfnLoadLib;
    int       i, missing = 0;

    DIAG( "ResolveDIBEngine: enter" );

    hDib = GetModuleHandle( "DIBENG" );
    DIAG( hDib ? "GetModuleHandle(DIBENG) ok" : "GetModuleHandle(DIBENG)=0, fallback LoadLibrary" );
    if (!hDib)
    {
        pfnLoadLib = GetProcAddress( GetModuleHandle( "KERNEL" ),
                                     "LoadLibrary" );
        if (pfnLoadLib)
            hDib = (*(PLOADLIB)&pfnLoadLib)( "DIBENG.DLL" );
    }
    if (!hDib)
    {
        DIAG( "fail 0a: DIBENG module not found" );
        return 0;
    }

    wDibDgroup = GetModuleDgroupSel( hDib );
    if (!wDibDgroup)
    {
        DIAG( "fail 0d: DIBENG DGROUP selector not found" );
        return 0;
    }

    for (i = 0; i < DIBFN_COUNT; i++)
    {
        FARPROC p = GetProcAddress( hDib, g_dibfnTab[i].name );
        if (!p)
        {
            DIAG( "fail 0b: unresolved DIBENG entry ->" );
            DIAG( g_dibfnTab[i].name );
            missing++;
        }
        g_dibfn[i] = (DWORD)p;
    }

    if (missing)
    {
        DIAG( "fail 0c: DIBENG entries missing (see names above)" );
        return 0;
    }
    DIAG( "ok 0: ResolveDIBEngine (36 entries)" );
    return 1;
}

/*
 * DibEnsureResolved —— DIBENG 懒解析的唯一入口(vddapi16.asm 的转发宏
 * 与 Enable 都调它, pascal far 约定)。成功置 g_dibfnReady=1; 失败保持
 * 0 —— 每个转发器会重试并留下日志(解析失败时 g_dibfn 里是桩, 转发
 * 不会跳进 0:0)。单线程协作式环境下解析不可重入, 无需加锁。
 */
int __far __pascal DibEnsureResolved( void )
{
    if (g_dibfnReady)
        return 1;
    if (!ResolveDIBEngine())
        return 0;
    g_dibfnReady = 1;
    return 1;
}

/*
 * DrvInitCore —— DriverInit 的 C 本体(vddapi16.asm 的 DriverInit 调用)。
 *   这是"第 ③ 环注册"的加载期部分, 时序见文件头注释。
 *   返回 1 = 成功; 0 = GDI 放弃加载本驱动。
 */
extern uint16_t LogBufSel;
uint16_t LogReady = 0;   /* [r447] 1 = 已进入 ring-3 DDI, 允许 INT 21h 落盘 */
extern void __far __pascal DiagFlushBuf( void );   /* [r412] DrvInitCore 里要用 */
extern void __far __pascal DiagFlushThresh( void );
int __far __pascal DrvInitCore( void )
{
    /* [r447] 日志缓冲: DGROUP 内 8KB 静态数组 g_logBuf(不再动态分配) */
    /* [r447] 不在 VxD 初始化阶段分配: WinME 下此时 KERNEL 尚不可用(debug 版 0D)。
     * 真正的分配推迟到第一次 ring-3 DDI(SetDisplayMode) 里做。 */
    LogBufSel = 0;
    DiagFlushBuf();   /* [r412] 立刻落盘, 避免该行被开机突发挤掉 */

    /* 【代码段加锁已移除】曾在此 GlobalSmartPageLock(代码段):
       ① wcc 的 __based(__segname("_TEXT")) 会链出指向不存在段号 3 的
          悬空 seg16 fixup → KRNL386 套重定位时段号越界 → 整模块拒载
          (bootlog: LoadFail 0000)—— 此为第一轮 LoadFail 的根因;
       ② 第二轮曾据此判"KERNEL.230 在真机非法"并移除调用 —— 事后
          在售驱动 vesamini.drv 的导入表证明 KERNEL.230 真实存在,
          patchseg 版蓝屏的真正死因与本构建相同(见下条 0) 的加载期
          LoadLibrary("DIBENG.DLL"), 与 230 无关。
       vmdisp9x 等全部在售参考驱动均不锁 16 位代码段, 随访。         */

    /* [r447] 日志缓冲必须从初始化一开始就可用: 否则 LogBufSel==0 会把
     * 所有 DIAG 行丢掉(r438/r439 日志只剩标记的根因, 已由静态缓冲修复)。 */
    if (!LogBufSel) {
        DiagSetLogBase( (unsigned short)(unsigned)g_logBuf );
        LogBufSel = 1;
    }


    DIAG( "DrvInitCore: enter" );
    DiagFlushBoot();
#ifdef VBE9X_DIAG
    DiagWriteLog( "BUILD OmniNine 0.01.448 " VBE9X_TAG " identity (unconditional -- this line must appear)" );
#endif
    DIAGMIN( "OmniNine build tag " VBE9X_TAG ); /* 部署校验: 日志缺本行=测的不是本构建 */
    /* 【r131】同一件事走无条件通道, 值 = 0x0131。release 档从此也有构建标记:
     *   以前三档里只有两档能确认"到底部署的是哪一版", 而 release 恰恰是
     *   行为不一样的那一档。                                            */
    PHASEV( "PH: build tag=", 0x0279 );
    /* 【r152】一条自证行 —— 无条件, 一次。
     *   MemCFix 的档位决定"盒子持屏期到底写不写硬件、写什么":
     *     0 = 只记账(探针照跑); 1 = r147 六字节文本补写(./148 灰屏);
     *     2 = 【默认】只写 CR0C/CR0D(显示起始地址), 写后自证、失败还原。
     *   看到 0000 就说明 INI 把本轮的行为改动关掉了 —— 那这份日志里
     *   `PH: probe fix=` 会一直是 0000, 而这不是"修复没生效", 是"没让它修"。
     * 【r153 修正 —— 这一行曾经是假的】上面说的是 display.c 里的值, 而
     *   ./152 证明它到达 VxD 之前被 drv.c 的 `memcfix ? 1 : 0` 夹成了 1:
     *   这一行印 0002, VxD 却在跑六字节补写(日志里 fixN=0014 = 真写了 20 次)。
     *   所以本轮【两侧各印一次】: 这里印驱动侧的 g_runMemCFix, drv.c 的
     *   `PH: probe policy=` 印 VxD 侧真正收到的档位。两个数必须相等;
     *   不等就说明中间还有一层夹取(这正是 r152 的病灶)。
     */
    /* 【r169】这两行现在【都有行为含义】了, 不再是 r151 的"只表示 INI 写了
     *   什么":
     *   ① Int10Fix=1(默认) ⇒ mini-VDD 的 48 号在全屏盒子持屏期会把
     *      72h/73h 改写成 03h。它到底改了几次看 drv.c 的
     *      `PH: fbox i10map`(0 = 一次都没轮到); 这里的数只说明开关位置。
     *   ② HiresAns: 默认 4(DDK 答案)。看到 1 就说明 INI 把它拨回了 r133。 */
    PHASEV( "PH: Int10Fix armed=", (uint16_t)g_runInt10Fix );
    PHASEV( "PH: HiresAns=",         (uint16_t)g_runHires );
    /* 【r152/r153】本轮唯一的行为开关, 无条件上报一次。
     *   0 = 只记账(探针仍跑); 1 = r147 六字节补写; 2 = 取指基址修复。
     *   与 drv.c 的 `PH: probe policy=` 必须相等(见上面的 r153 修正)。 */
    /* r128-marker- SetModeText: VGA-text round trip before the hires re-set.
     *   【本轮改了显示路径】这是 r112 以来第一次真正动"恢复"这条链:
     *   在 SetDisplayMode 里、VBE_SetMode 之前, 插入一次 INT 10h AX=0003
     *   (复用 drv.c 既有的 BIOS_SetTextMode), 只在推迟重建上下文里发。
     *   依据: ./116 result.txt 第 8 条的用户实测 —— 全屏花屏下输入 mode co80
     *   当场恢复正常, 而 mode co80 就是这一枪。新增开关 SetModeText(默认 0)。
     *   minivdd.asm 与 drv.c 本轮未改; VBE_VXD_VERSION 仍为 0x0114。 */

#ifdef VBE9X_PROBEONLY
    /* 【裁决构建】入口执行到这里即证明 KRNL386 已调用本驱动入口。
       不做任何后续初始化, 直接报告加载成功; Enable 同时被桩成返回 0,
       GDI 会自行回退 VGA, 启动可安全继续。用于区分故障层:
         LoadSuccess + 日志有 PROBEONLY 行 → 之前是初始化某步 return 0
         LoadSuccess + 仍然无日志      → 日志通道坏了(只读/盘符)
         LoadFail   + 无日志           → KRNL386 在 NE 层拒载(入口根本没被调用)
       用法: nmake DIAGFLAG="-dVBE9X_DIAG -dVBE9X_PROBEONLY" omninine.drv  */
    DIAG( "PROBEONLY: skip init, return 1" );
    return 1;
#endif

    /* 0) DIB 转发表填安全桩 —— 【解析不在此阶段做】。本驱动由 KRNL386
          引导期直接加载, 此时 GDI 未启动、DIBENG.DLL 不在内存, 加载期
          GetModuleHandle("DIBENG") 必失败、LoadLibrary 兜底更会在 KERNEL
          内 GP 蓝屏(error 0D, ./5 构建实证)。解析推迟到 GDI 期, 由
          DibEnsureResolved 经首次转发(见 vddapi16.asm 宏)或首次 Enable
          触发; 未解析前任何转发落到 DIBFNSTUB 安全返回, 不会跳进 0:0。 */
    {
        union { void (__far *fp)( void ); DWORD dw; } u;
        int i;

        u.fp = (void (__far *)( void ))DibFnStub;
        for (i = 0; i < DIBFN_COUNT; i++)
            g_dibfn[i] = u.dw;
    }
    DIAG( "ok 0: g_dibfn filled with stubs (DIBENG deferred to GDI time)" );
    DiagFlushBoot();

    /* 1) 取主 VDD 入口与本 VM 句柄(INT 2F 1684h/1683h) */
    VddAcquire();
    if (VDDEntryPoint == 0 || OurVMHandle == 0)
    {
        DIAG( "fail 1: VddAcquire (VDDEntryPoint/OurVMHandle=0)" );
        return 0;           /* 无主 VDD(非正常 Win9x 图形环境) */
    }
    DIAG( "ok 1: VddAcquire" );

    /* 2) 连接我们的 mini-VDD 并核对版本(配套发布, 精确匹配)。
     *    版本号同时是部署校验: 日志里的 VxD version 必须等于
     *    VBE_VXD_VERSION 当前值, 偏低说明测试机跑的还是旧 minivdd.vxd。*/
    if (!VxD_Connect())
    {
        DIAG( "fail 2a: VxD_Connect (minivdd.vxd 未加载? 查 bootlog)" );
        return 0;
    }
    DiagHexMin( "VxD version", QueryMiniVDDVersion() );
    if (QueryMiniVDDVersion() != VBE_VXD_VERSION)
    {
        DIAG( "fail 2b: version mismatch (SYSTEM 下是旧 vxd?)" );
        return 0;
    }
    DIAG( "ok 2: VxD_Connect + version" );
    FLUSH();            /* r43: 死亡区密排落盘(./42c 实锤 ok2 后的步骤死机
                           时整段日志随缓存丢失, 最后落盘行停在第二询版
                           本的 "before", 无法分辨死在远调内还是其后) */

    /* 3) 告知主 VDD: 本 VM 的显示驱动已加载 */
    DIAG( "VddCall 0x83: before" );
    FLUSH();
    VddCall( VDD_REGISTER_DISPLAY_DRIVER_INFO );
    DIAG( "VddCall 0x83: after" );
    FLUSH();

    /* 4) 读显示配置 */
    DIAG( "ReadDisplayConfig: before" );
    FLUSH();
    ReadDisplayConfig();
    DIAG( "ReadDisplayConfig: after" );
    DiagFlushBoot();
    FLUSH();

    /* 5) 【r45 起移除加载期枚举探针】./43/44 实锤: 本机(全新 Win98)
     *    加载期运行到此处后硬死机 —— r43 有 flush 时日志停在
     *    "ReadDisplayConfig: after" 之后, r44 去掉 flush 后整份日志
     *    随 VFAT 卷缓存丢失(连建文件都没留下), 两次症状一致, 死点即
     *    EnsureModeTable()(内部经 VxD 直通 V86 BIOS 枚举)。与 ./21
     *    裁决(加载期 INT10 链被引导残留层接管)及 vmdisp9x 时序
     *    (从不在加载期枚举)一致: 正式枚举唯一入口是 Enable 期
     *    EnsureModeTable(PhysicalEnable 调用链)。                 */
    DIAG( "probe@load: enum SKIPPED, deferred to Enable (r45)" );

    DIAG( "DrvInitCore: success" );
    return 1;
}

/*
 * FbSelfTest —— 帧缓冲"写/读回"自检(r82)。见文件上方 FBTEST 变量区说明。
 *   【两个采样点】
 *     ① 屏尾: 偏移 = 帧缓冲起点 + pitch*yRes - 256 —— 证明"整屏可写";
 *     ② 显存尾: 偏移 = 帧缓冲起点 + min(可访问窗口, 声称显存) - 256
 *        —— 证明"我们上报的显存是真的"。离屏面能不能安全交给 GDI,
 *        全看这一点。
 *   【为什么只抽样两点】目的是发现"孔径不够/高地址别名"这类结构性错误,
 *   256 字节的位置相关花样足以暴露别名; 全扫数 MB 在真机上要几十毫秒,
 *   而且现在还没启用写合并, 读回代价更高。这是抽样, 不是搬运。
 *   【失败怎么办】不拒绝使能(屏首大概率仍可用, 黑屏比花屏更糟), 但:
 *     · 显存尾失败 → 撤 OFFSCREEN 位 + 把上报显存钳到"可见帧缓冲"大小,
 *       于是 GDI/DIBENG 不会再去用那块写不进去的离屏显存;
 *     · 屏尾失败 → 只记日志(自检本身可能被硬件行为干扰, 不足以否决整个
 *       显示通道; 真正的判据是画面能不能出来)。
 *   【关掉它】system.ini [omninine] SelfTest=0(自检与钳制一起关, 回到 r81
 *   的行为, 用于 A/B 判断是不是自检本身在捣乱)。
 */
/*
 * LayerCard -- r89: one block that states which layer of every layered
 * decision is actually in effect on THIS machine.
 *
 * Why it exists: a driver that works on the test machine and fails in
 * production must be triageable from the log alone, without a debug
 * rebuild (by then the failing machine may be gone).  Everything above it
 * says *why* something was chosen; this block says *what* was chosen, in
 * one place, for every layer we know to be machine dependent:
 *   form      0 = 0-based flat selector (offset = linear address)
 *             1 = selector base = framebuffer (offset = 0)
 *   selmode   what system.ini asked for (0 = flat preferred)
 *   verify    descriptor read-back gate on/off (r89)
 *   sel       the selector actually handed to DIBENG (0 = never got one)
 *   bitsoff   1 = deBitsOffset forced to 0 (A/B experiment)
 *   cdpd      1 = CreateDIBPDevice was asked to build the PDEVICE
 *   cdpdrc    what CreateDIBPDevice returned (0 = hand filled one kept)
 *   delpdev   1 = delpPDevice filled with the screen pointer
 *   selftest  bit0 = screen tail writable, bit1 = vram tail writable,
 *             bit2 = at least one sample really ran
 *   offscreen 1 = deFlags carries OFFSCREEN (only if the tail tested good)
 * Read it as "on this machine, layer X took branch Y"; the LfbMap: / SEL: /
 * CDPD: / FbTest lines earlier in the same log explain each branch.
 */
static void LayerCard( WORD deFlags )
{
    DIAG( "CAP: L ===== r89 layer card =====" );
    DiagHex( "CAP: L form",      (WORD)(LfbAccessOffset() ? 0 : 1) );
    DiagHex( "CAP: L selmode",   g_runSelMode );
    DiagHex( "CAP: L verify",    g_runVerify );
    DiagHex( "CAP: L sel",       g_fbSel );
    DiagHex( "CAP: L bitsoff",   g_runBitsOff );
    DiagHex( "CAP: L cdpd",      g_runCdpd );
    DiagHex( "CAP: L cdpdrc",    g_cdpdRc );
    DiagHex( "CAP: L delpdev",   g_runDelpDev );
    DiagHex( "CAP: L selftest",  (WORD)((g_fbHeadOk ? 1 : 0)
                                      | (g_fbTailOk ? 2 : 0)
                                      | (g_fbMeasured ? 4 : 0)) );
    DiagHex( "CAP: L offscreen", (WORD)((deFlags & 0x0008) ? 1 : 0) );
    DIAG( "CAP: L ==========================" );
}

static void FbSelfTest( WORD xRes, WORD yRes, WORD pitch )
{
    DWORD head, win, vram, lim;
    WORD  r;

    (void)xRes;                         /* 抽样点只由 pitch*yRes 决定, xRes 不
                                           参与计算; 保留形参以固定调用签名
                                           (W303 只是"未引用"提示, 不是错误) */

    g_fbHeadOk = 0;
    g_fbTailOk = 0;
    g_fbMeasured = 0;                   /* r83: "没测"≠"测出坏", 见头注 */

    if (!g_fbSel || !g_runSelfTest)
    {
        DIAG( "FbTest: skipped (no selector / SelfTest=0)" );
        FLUSH();
        return;
    }

    /* 【几何必须由调用方传入, 不能读 wScreenX/Y/pitch】首次 Enable 时
     * 那些全局量还没更新(它们在 SetDisplayMode 的后半段才赋值), 读它们
     * 会得到 0 → 屏尾采样被跳过 → 自检形同虚设。 */
    head = (DWORD)pitch * yRes;
    g_fbHeadBytes = head;
    win  = LfbWindowBytes();            /* 选择子从帧缓冲起点可访问的字节数 */
    vram = VbeTotalVram();              /* VBE 4F00h 声称的显存总量         */

    FbTestSel = g_fbSel;
    FbTestLen = 256;                    /* 64 个双字                        */
    FbTestPat = 0xA5C39E71UL;           /* 基花样; 实际写入 pat+i(抓别名)   */

    /* ① 屏尾 */
    if (head >= 1024 && head <= win)
    {
        FbTestOff = LfbAccessOffset() + (head - FbTestLen);
        r = FbTest();
        DiagHex( "FbTest head ret", r );
        if (r != 0xFFFF)                /* 0xFFFF = 参数非法/未测到 */
            g_fbMeasured = 1;
        if (r == 0)
            g_fbHeadOk = 1;
    }
    else
    {
        DIAG( "FbTest: head sample outside window -> skipped" );
    }

    /* ② 显存尾(只有真的声称了"屏以外还有显存"时才测) */
    if (vram > head + 1024 && win > head + 1024)
    {
        lim = (vram < win) ? vram : win;
        FbTestOff = LfbAccessOffset() + (lim - FbTestLen);
        r = FbTest();
        DiagHex( "FbTest tail ret", r );
        if (r != 0xFFFF)
            g_fbMeasured = 1;
        if (r == 0)
            g_fbTailOk = 1;
    }
    else
    {
        DIAG( "FbTest: no vram beyond screen -> tail not tested" );
    }

    if (!g_fbMeasured)
        DIAG( "FbTest: not measured (no sample executed) -> keep r81 behaviour" );
    DiagHex( "FbTest measured",  g_fbMeasured );
    DiagHex( "FbTest head ok", g_fbHeadOk );
    DiagHex( "FbTest tail ok", g_fbTailOk );
    DiagHex( "FbTest win lo",  (WORD)win );
    DiagHex( "FbTest win hi",  (WORD)(win >> 16) );
    DiagHex( "FbTest vram lo", (WORD)vram );
    DiagHex( "FbTest vram hi", (WORD)(vram >> 16) );
    FLUSH();
}

/* ==========================================================================
 *  r82 回桌面验证: "我以为设上了" → "我确认设上了"
 *  【为什么必须有】DOS/console 会话切回桌面时, 主 VDD 回调我们的
 *  RestoreDesktopMode; 而"VBE_SetMode 返回 0x004F"并不等于"显卡真的回到
 *  了我们的模式" —— 真机上 BIOS 只管应答。若不管, 用户看到的就是黑屏,
 *  而日志里一切正常(这正是参考驱动在真机上的经典失败形态)。
 *  【两道判据, 都不需要改动 VxD】
 *    ① 帧缓冲写/读回(FbSelfTest): 画面通道真的能用吗 —— 这是硬判据;
 *    ② VxD 里记的 currentMode: DOS 应用直接调 VESA 设过模式的话, mini-VDD
 *       的 VESA_CALL_POST_PROCESSING(47) 会把当前模式同步进去; 它与我们
 *       期望的模式不一致, 就说明"卡片现在待在我们没设过的模式里"。
 *  【失败处理】重设一次(仅一次, 不循环); 仍失败则如实落盘。
 *  (清 BUSY / 重绘 / 清屏照常执行 —— 那是恢复流程的固定动作, 与验证结果无关。)
 * =========================================================================*/

/*
 * ClearVisibleScreen —— 把整个可见帧缓冲清零(黑屏)。
 *   【为何】模式刚切换时显存里是残留数据, 不清屏会在 GDI 完成首轮
 *   重绘前出现花屏。RestoreDesktopMode 里同样调用: DOS 全屏程序退出
 *   后显存内容不可信。
 *   【实现】走汇编 FBCLEAR: 帧缓冲可达数 MB, 16 位远指针的偏移会
 *   在 64K 处回绕, 而 Watcom 的 __huge 指针算术属于"加选择子号"型
 *   归一化(保护模式 __HShift=3, 每 8 字节 +1 个选择子) —— 我们手里
 *   只有一个选择子, 用它等于去写别的 LDT 表项。FBCLEAR 在 16 位代码
 *   里用 32 位偏移(地址尺寸前缀)+ rep stosd 一次清完整屏。
 */
static void __far ClearVisibleScreen( void )
{
    DWORD bytes;

    if (!g_fbSel)
        return;

    /* 【r81】system.ini [omninine] Clear=0 时跳过清屏 —— 用于判定"清屏本身
     * 是否参与故障"(清屏是本驱动唯一一处【自己】写几 MB 帧缓冲的动作,
     * 若它有问题, 症状与 DIBENG 写屏很不一样, 值得一刀切开)。      */
    if (!g_runClear)
    {
        DIAG( "ClearVisibleScreen: skipped (vbe9x Clear=0)" );
        return;
    }

    bytes = (DWORD)wScreenPitchBytes * wScreenY;
    if (bytes < 4)
        return;

    FbClearSel   = g_fbSel;
    FbClearOff   = LfbAccessOffset();   /* r80: 与 deBitsOffset 同源 */
    FbClearBytes = bytes & ~3UL;        /* 4 字节对齐; 尾部余数不清无害 */
    FbClear();
}

/*
 * SetDisplayMode —— 真正把 wScrX/wScrY/wBpp 设置到硬件。
 *   bFullSet=1: 完整序列(PhysicalEnable 路径), 含帧缓冲选择子建立
 *               与全局几何更新;
 *   bFullSet=0: 仅恢复模式(RestoreDesktopMode 路径), 复用已有选择子
 *               —— 选择子背后的 DPMI 映射是会话级的, 无需重建。
 *
 *   【失败语义与 vmdisp9x 严格一致】PRE_MODE_CHANGE 之后若设模式失败,
 *   直接返回而【不】补 POST_MODE_CHANGE —— 主 VDD 的状态机允许这种
 *   "切换中止"(参考实现多年实机验证如此), 擅自补 POST 反而风险未知。
 */
static int __far SetDisplayMode( int bFullSet )
{
    WORD           vbeMode;
    MODEINFOBLOCK  mi;
    DWORD          fbBytes;
    VBESHAREDSTATE st;

    /* [r447] 首次 SetDisplayMode(开机恢复那一次)仍可能处在 VDD/DOS 临界状态:
     * 不分配日志缓冲、也不落盘; 第二次及以后才启用。 */
    {   static WORD s_sdmN = 0;
        ++s_sdmN;
        LogReady = (WORD)(s_sdmN > 1);
    }

    /* [r447] 缓冲已在 DrvInitCore 入口建立, 这里不再分配 */

    DiagFlushBoot();    /* [r447] 首次模式切换前把开机日志冲出去 */
    vbeMode = FindVbeMode( wScrX, wScrY, wBpp );
    DiagHex( "SetDisplayMode: vbeMode", vbeMode );
    FLUSH();
    if (vbeMode == 0xFFFF)
    {
        DIAG( "SetDisplayMode fail: FindVbeMode=FFFF" );
        DIAGM( "SDM fail 1: FindVbeMode" );     /* r111: Log=1 也看得见 */
        FLUSH();
        return 0;
    }

    /* 切换前通知主 VDD(它会保存 VGA 状态、接管 DOS VM 显示仲裁)。
     * 【r111】官方回调里那一次跳过 —— 见 g_inCbRestore 的注。        */
    if (!g_inCbRestore)
    {
        DIAG( "SetDisplayMode: before PRE_MODE_CHANGE" );
        FLUSH();
        VddCall( VDD_PRE_MODE_CHANGE );
        DIAG( "SetDisplayMode: PRE_MODE_CHANGE ok" );
        FLUSH();
    }
    else
    {
        DIAG( "SetDisplayMode: PRE_MODE_CHANGE skipped (in VDD callback)" );
        FLUSH();
    }

    /* 【r126】SetModeText —— 在重设高分辨率之前先走一趟标准 VGA 文本模式。
     * 【为什么插在这里】必须【先】让主 VDD 看见一次它认得的模式设置, 我们
     *   随后那次 4F02 才是"它记账之后的一次改变"; 反过来(先 4F02 再文本模式)
     *   等于把刚建好的高分辨率又拆掉。
     * 【为什么绝不在回调里做】这是嵌套 INT 10h —— r99 三次死机全部停在
     *   "回调里那次嵌套 4F02"上, 硬护栏 bInVddCallback 也会拒绝它。
     *   所以这条只在推迟重建(普通 ring-3 上下文)里出现。
     * 【调用者是谁】ApplyDesktopRestore(how=2) <- MaybeDeferredRestore,
     *   以及 PhysicalEnable 的首次设模式(那里 g_inCbRestore 本来就是 0)。
     *   ./116 result.txt 第 8 条的用户实测就是这一枪。                    */
    if (g_runSetModeText && !g_inCbRestore)
    {
        DIAG( "SetDisplayMode: SetModeText -> BIOS_SetTextMode (INT 10h AX=0003)" );
        FLUSH();
        BIOS_SetTextMode();
        DIAG( "SetDisplayMode: SetModeText done" );
        FLUSH();
    }

    /* bit14=1 请求线性帧缓冲模式 */
    /* [r132] textbook VGA rule: the timing registers CR00..CR07 ignore writes
     * while CR11h bit7 (Protect) is set -- standard text mode leaves it set
     * (CR11h = 8Eh), so the 4F02h below would be answered 004F while its whole
     * CR00..CR07 group was dropped.  Clear it first; see VxdCrtProtectClear. */
    ProtClrBeforeModeSet( "SetDisplayMode" );
    if (!VBE_SetMode( (uint16_t)(0x4000 | vbeMode) ))
    {
        DIAG( "SetDisplayMode fail: VBE_SetMode" );
        DIAGM( "SDM fail 2: VBE_SetMode(4F02)" );   /* r111 */
        FLUSH();
        return 0;
    }

    /* [r406] 等硬件真的进入该模式(最多 SettlePoll 次)。理由见 g_runSettlePoll。 */
    if (g_runSettlePoll && !ModeSettled( (uint16_t)vbeMode ))
        DiagHexMin( "SDM settle: mode did not converge", 0 );
    /* 【r106】刚设完模式立刻问一次硬件 —— 这一行是整份日志的"基线":
     * 4F02 返回 004F 只说明 BIOS 应答了, 而 0F00h 的 AL 才说明硬件的
     * 显示通路到底在哪。AL=0FFh 是正常基线; 若这里就回 03h/13h, 说明
     * 连"设完模式"这一刻硬件都不在我们的模式里。                    */
    HwProbe( "HW probe: after VBE_SetMode in SetDisplayMode" );

    /* 取模式信息(几何 + PhysBasePtr)。这一步同时是"设模式是否真的
     * 成功"的判据: 4F01 拿不到就说明 BIOS/直通链有问题。            */
    DIAG( "SetDisplayMode: before 4F01" );
    FLUSH();
    if (!VBE_GetModeInfo( vbeMode, &mi ))
    {
        DIAG( "SetDisplayMode fail: VBE_GetModeInfo(4F01)" );
        DIAGM( "SDM fail 3: VBE_GetModeInfo(4F01)" );   /* r111 */
        FLUSH();
        return 0;
    }
    DiagHex( "SetDisplayMode: xRes", mi.XResolution );
    DiagHex( "SetDisplayMode: yRes", mi.YResolution );
    DiagHex( "SetDisplayMode: pitch", mi.BytesPerScanLine );
    DiagHex( "SetDisplayMode: phys lo", (WORD)mi.PhysBasePtr );
    DiagHex( "SetDisplayMode: phys hi", (WORD)(mi.PhysBasePtr >> 16) );
    FLUSH();
    if (mi.PhysBasePtr == 0 || mi.BytesPerScanLine == 0)
    {
        /* 【r112】"4F02 明明成功、紧接着 4F01 却说没有线性帧缓冲" 不再
         * 直接判死。./111 t5 的现场就是这一条:
         *   `VBE_SetMode: exec ax=004F` → `VxDexec: in=4F01` → 没有
         *   `exec4F01 ax=` 那一行 → `SetDisplayMode: phys lo=0000` →
         *   `SDM fail 4` → 重挂; 而失败的原因是 r100 护栏把那次 4F01 拒了
         *   (同一条日志里的 `rebuild refused (in VDD callback)=0001`),
         *   不是硬件没有 LFB —— 开机时同一块卡的 PhysBasePtr=DE000000。
         * 判据: 我们【已经有】一个可用的帧缓冲选择子, 而且当前记录的几何
         * 与这次要设的模式一致(同一块屏、同一个 pitch) ⇒ 就继续用原来
         * 那个选择子。选择子映的是物理地址, 与"模式有没有被重设"无关;
         * 开机那次映射已经过 FbTest 头/尾验证, 不会因为一次被拒的 4F01
         * 失效。真正"没有 LFB"的情况(wEnabled 之前、或几何变了)照旧失败。*/
        if (g_fbSel != 0 && wEnabled && wScreenPitchBytes != 0 && wBpp != 0)
        {
            DIAG( "SetDisplayMode: 4F01 unusable -> keep existing LFB selector" );
            DIAGM( "SDM ok (kept existing LFB selector)" );
            FLUSH();
            return 1;
        }
        DIAG( "SetDisplayMode fail: no LFB (PhysBasePtr=0)" );
        DIAGM( "SDM fail 4: no LFB" );                  /* r111 */
        FLUSH();
        return 0;           /* 无 LFB —— 本驱动不做 banked 渲染 */
    }

    /* 【r76 决定性的一步】帧缓冲选择子 = DPMI 0800h 物理映射 +
     * DPMI LDT 选择子, 全部在 ring 3 由本驱动自己完成。
     * 只有拿到非 0 选择子才允许继续 —— 拿不到就地让 Enable 干净
     * 失败回退 VGA, 【绝不能】拿一个不可用的选择子去写显存: 那正是
     * 蓝屏 0D(031F:00090B = _fmemset_ 的 mov es,dx)的现场。        */
    fbBytes = (DWORD)mi.BytesPerScanLine * mi.YResolution;
    g_fbSel = LfbSelector_Ensure( mi.PhysBasePtr, fbBytes );
    DiagHex( "SetDisplayMode: fb sel", g_fbSel );
    /* 【r89】验一次再往下走(见 drv.c LfbSelVerify): 必须在 FbSelfTest 之前,
     * 因为它可能把"选择子真正可访问的窗口"钳小, 而自检与显存上报都按那个
     * 窗口算 —— 顺序反了就会拿一个偏大的窗口去自检/OFFSCREEN 判定。 */
    if (g_fbSel)
        LfbSelVerify();
    FLUSH();
    if (!g_fbSel)
    {
        DIAG( "SetDisplayMode fail: LfbSelector_Ensure (DPMI 映射/选择子)" );
        DIAGM( "SDM fail 5: LfbSelector(DPMI)" );       /* r111 */
        FLUSH();
        return 0;
    }

    /* 【r82】拿到选择子后立刻自检: "VBE 说的显存"是不是"真能写的显存"。
     * 结果供后面的 wPDeviceFlags(OFFSCREEN 位)与显存上报使用 —— 必须在
     * PushStateToMiniVDD 之前跑, 否则主 VDD 拿到的是未钳制的显存总量。 */
    FbSelfTest( mi.XResolution, mi.YResolution, mi.BytesPerScanLine );

    /* 自检结论落到"可信显存上限": 显存尾写不进去时, 上报值收缩到可见帧
     * 缓冲大小(离屏面不再交给 GDI), 这是"宁可少报, 不可谎报"的具体落实。
     * SelfTest=0(用户显式关掉自检)时不改任何东西, 行为同 r81。        */
    if (g_runSelfTest && g_fbMeasured)      /* r83: 没测到就不钳制 */
    {
        if (g_fbTailOk)
            LfbSetVramCeiling( 0 );                 /* 不设限 */
        else
            LfbSetVramCeiling( g_fbHeadBytes );     /* 只报"看得见"的那部分 */
    }

    /* 推送共享状态给 mini-VDD: 主 VDD 的 GET_TOTAL_VRAM_SIZE /
     * GET_BANK_SIZE 全靠它回答(DOS 全屏会话的保存/恢复要用, 报错就是
     * 花屏); 顺带把刚建好的屏选择子交给 VxD —— 由它代我们调用
     * VDD_Register_Extra_Screen_Selector 登记给主 VDD(顺序必须是
     * "先建选择子, 再推送", 否则 VxD 收到的是 0, 等于没登记)。      */
    if (!PushStateToMiniVDD( &mi, vbeMode ))
    {
        DIAG( "SetDisplayMode fail: PushStateToMiniVDD" );
        DIAGM( "SDM fail 6: PushStateToMiniVDD" );      /* r111 */
        FLUSH();
        return 0;
    }
    DIAG( "SetDisplayMode: PushState ok" );
    FLUSH();

    if (bFullSet)
    {
        wScreenX          = mi.XResolution;
        wScreenY          = mi.YResolution;
        wScreenPitchBytes = mi.BytesPerScanLine;
        wBpp              = mi.BitsPerPixel;

        /* 【r79 纠正 r76 的判断: OFFSCREEN 要设】
         * 两个在售参考驱动(vmdisp9x 的 modes.c 与同源的 boxv9x)在屏幕
         * PDEV 上都写的是:
         *      wPDeviceFlags = MINIDRIVER | VRAM | OFFSCREEN;
         *      if( wBpp == 16 ) wPDeviceFlags |= FIVE6FIVE;
         * r76 曾以"GDI/DIBENG 会把设备位图放进屏幕之后的显存, 与主 VDD
         * 窗口化 VGA 的缓冲互踩"为理由把 OFFSCREEN 去掉。这个理由站不住:
         *   ① 主 VDD 占用多少显存是 VDD_DRIVER_REGISTER **返回给驱动**的
         *      (DDK: "the amount of video memory used by VDD in bytes";
         *      本机实测 = 0x5300 ≈ 21KB), 由 VDD/GDI 统一记账, 不是
         *      "驱动装作那块不存在"就能避免的;
         *   ② 那个 32K/64K 缓冲按 DDK 原文是 "right below the visible
         *      Windows screen"(可见屏幕之下的显存区), 而离屏堆在屏幕
         *      之后 —— 两者本就由 VDD/GDI 的记账划分, 与标志位无关;
         *   ③ 最要紧的是: OFFSCREEN 是"本设备支持显存离屏面"的声明,
         *      DIBENG 据此建立离屏显存堆。少了它, DIBENG 在 VRAM 设备上
         *      走的是"没有离屏堆"的另一条路径 —— 而 r76b/r76c/r77/r78
         *      四次蓝屏都是"Enable 返回后、尚未调用我们任何 DDI, 就崩在
         *      DIBENG/GDI 自己的代码里", 与"标志位少了一位导致它走错
         *      分支"的形态完全一致。
         * 配合 r78 的"映射覆盖整个显存"(16MB), 离屏面无论落在哪里都在
         * 选择子界限之内 —— 两处改动是配套的。
         * 【回退开关】若本轮出现新的花屏/DOS 花屏, 用
         *      nmake DIAGFLAG="-dVBE9X_DIAG -dVBE9X_NO_OFFSCREEN" omninine.drv
         * 构建"不设 OFFSCREEN"的对照版, 一步定位是不是它。           */
        wPDeviceFlags = MINIDRIVER | VRAM;
#ifndef VBE9X_NO_OFFSCREEN
        /* 【r82】OFFSCREEN 只在"离屏显存经自检确认可写"时才声明。
         * 旧行为(无条件声明)在真机上正是"随机花屏"的温床: VBE 报的显存
         * 大于真正可访问的孔径时, DIBENG 会把离屏面放到空洞里。
         * SelfTest=0 时退回 r81 行为(无条件声明), 便于 A/B。
         * 【r102】再叠一层 INI 开关 Offscreen(默认 1): 0 = 这次会话完全不
         * 声明离屏面。理由见 g_runOffscreen 的长注释 —— ./100 那条陈旧行
         * 就是启动画面彩旗的一行, 而它只可能来自"屏以外的显存"。      */
        if (g_runOffscreen)
        {
            if (g_fbTailOk || !g_runSelfTest || !g_fbMeasured)
                wPDeviceFlags |= OFFSCREEN;
            else
                DIAG( "SetDisplayMode: OFFSCREEN withheld (FbTest tail failed)" );
        }
        else
        {
            DIAG( "SetDisplayMode: OFFSCREEN withheld (Offscreen=0)" );
        }
#endif
        if (wBpp == 16)
            wPDeviceFlags |= FIVE6FIVE; /* 告知 DIBENG: 16bpp 是 565 */

        /* 【r81】system.ini [omninine] Flags 可强制覆盖上面的默认组合(0=不覆盖)。
         * 用途: 一轮之内试遍 VRAM/OFFSCREEN/MINIDRIVER 的组合, 不必重编
         * 驱动。FIVE6FIVE 是 16bpp 的正确性位, 不受覆盖影响。       */
        if (g_runFlags)
        {
            wPDeviceFlags = g_runFlags;
            if (wBpp == 16)
                wPDeviceFlags |= FIVE6FIVE;
            DiagHex( "SetDisplayMode: flags override", wPDeviceFlags );
        }
    }

    /* 回读校验: 确认 VxD 真收下了推送的状态(签名/长度二次确认),
     * 也顺带跑一次 GET_STATE 通道。失败只记日志, 不影响本驱动显示。 */
    if (FetchStateFromMiniVDD( &st ))
    {
        DiagHex( "SetDisplayMode: vxd mode", st.currentMode );
        DiagHex( "SetDisplayMode: vxd lfbSel", st.lfbSelector );
        DiagHex( "SetDisplayMode: vxd vram lo", (WORD)st.vramSize );
        DiagHex( "SetDisplayMode: vxd vram hi", (WORD)(st.vramSize >> 16) );
    }    else
    {
        DIAG( "SetDisplayMode: warn: VxD state readback failed" );
    }
    FLUSH();

    /* 【r78 取证】屏选择子登记给主 VDD 之后复查一次描述符: 若存在位/基址
     * 被动过(VDD 或 DPMI 宿主改的), GDI 只要载入它就立刻 #GP —— 而这
     * 完全不需要调用我们任何 DDI, 与"崩溃落在别的模块段内、日志无 DDI
     * 探针"的现象吻合。这里只读不改, 结论直接落盘。                 */
    LfbSelRecheck();
    FLUSH();
    /* 【r139】模式已经建立完成 ⇒ 把"我们在这台硬件上做过一次模式建立"这件事
     *   写进【硬件】: 往 DAC 前 8 项盖一次章。
     *   为什么盖章点选在这里: 本函数就是"一次完整模式建立"的唯一收口
     *   (Enable 走它、ForceModeReprogram 回程也走它), 而它的末尾意味着
     *   VBE_SetMode + 状态推送 + 选择子复查全部成功。
     *   为什么只在不等于 8bpp 时盖: 8bpp 下 DAC 就是画面本身(SetPalette 归
     *   GDI 管), 一个字节都不许碰。16bpp 下 RAMDAC 不参与画面 ⇒ 零影响。
     *   下一次从 DOS 盒回来时, 这个印章还在不在, 就是"我们的模式有没有被
     *   重新建立"的【硬件物证】(见 drv.c 的 VxdDacStampOk)。            */
    if (g_runDacStamp && wBpp != 8)
    {
        VxdDacStamp();
        /* 【为什么回读要分上下文】写 DAC 是官方明文要求的(ResetHiResMode 页:
         *   "It must reinitialize the Windows palette"), 所以回调里写没问题;
         *   但【读】要先往 3C7h 写读索引, 而主 VDD 自己也在写这套端口 —— 回调
         *   期是它的切屏事务内部, 在那里插一次读可能让它下一次 3C9h 写落到
         *   错误的项上。所以回调里只盖章不回读(r139-marker: stamp in cb)。 */
        if (!g_inCbRestore)
            PHASEV( "PH: dacstamp=", VxdDacStampOk() );  /* 立刻回读自证 */
        else
            DIAG( "SetDisplayMode: DAC stamped (read-back skipped, in VDD callback)" );
    }
    return 1;
}

/*
 * PhysicalEnable —— Enable 第二阶段的硬件部分(③ 环注册的使能期)。
 *   时序: SetDisplayMode(1)   —— 含 VBE_SetMode + 状态推送 + 【r76】
 *                                DPMI 帧缓冲映射/LDT 选择子建立
 *         → VDD_DRIVER_REGISTER(0x80, 登记 RestoreDesktopMode)
 *         → 保险性地再打一次 VBE_SetMode
 *         → VDD_POST_MODE_CHANGE(0x87)
 *         → VDD_SAVE_DRIVER_STATE(0x82)
 *         → 清屏(FBCLEAR)
 *   任何一步失败都返回 0 → GDI 干净回退 VGA。帧缓冲选择子/映射拿不到
 *   时【必须】失败, 不能拿一个不可用的选择子去写显存(0D 蓝屏现场)。
 */
static int PhysicalEnable( void )
{
    WORD  vbeMode;
    DWORD dwRegRet;

    DIAG( "PhysicalEnable: enter" );
    FLUSH();

    /* GDI 期的正式枚举点(数据点②): GDI 已初始化、显示驱动已注册,
       系统 VM 中断链处于正常桌面态(对齐 vmdisp9x 时序)。若此处
       4F00 的 post[0] 仍为 4256, 则时机无关, 下一步走 VxD 内
       执行 BIOS 的终极方案。                                       */
    if (!EnsureModeTable())
    {
        DIAG( "PhysicalEnable fail: no VBE modes" );
        FLUSH();
        return 0;
    }
    DiagHex( "PhysicalEnable: modes cached", (WORD)g_nModes );
    FLUSH();

    vbeMode = FindVbeMode( wScrX, wScrY, wBpp );
    DiagHex( "PhysicalEnable: vbeMode", vbeMode );
    FLUSH();
    if (vbeMode == 0xFFFF)
        return 0;

    if (!SetDisplayMode( 1 ))
        return 0;
    DIAG( "PhysicalEnable: SDM ok" );
    FLUSH();

    /* 登记桌面恢复回调。返回值语义: 成功 = VDD 占用的显存字节数;
       失败 = 原样返回功能号。参考实现(vmdisp9x)把失败视为【非致命】
       —— 标准 VDD 不会失败; 万一失败, 只影响 DOS 全屏切回时的自动
       恢复, 不值得让整个桌面加载失败。                              */
    /* 【r100】EDX 策略的折算已移到 ReadRunProfile(必须早于第一次
     * PushState, 否则推给 VxD 的 policyVddEdx 是旧值)。这里只记录。*/
    DiagHex( "PhysicalEnable: edx policy", (WORD)g_vddEdxValue );
    FLUSH();

    PHASEV( "PH: oblig5 reg=", 1 );   /* [r406] DDK 义务⑤ */
    dwRegRet = VddRegister( VDD_DRIVER_REGISTER, wScreenPitchBytes, wScreenY,
                            (void __far *)RestoreDesktopMode );
    /* 【r99: 必须打完整 32 位】r98 只打低字, 而 DDK 的返回值语义是
     *   EAX = 可见屏字节数(即我们传的 ECX) + 主 VDD 为窗口化 VGA 4 平面
     *         虚拟化额外占用的字节数
     * —— 那个"额外量"落在【高字】。1024x768x16 时 ECX = 0x180000, 低字
     * 恒为 0, 只看低字等于什么都没看(这正是 r98 期间"主 VDD 有没有自留
     * 区"一直判不下来的原因)。                                      */
    DiagHex( "PhysicalEnable: reg lo", (WORD)dwRegRet );
    DiagHex( "PhysicalEnable: reg hi", (WORD)(dwRegRet >> 16) );
    DiagHex( "PhysicalEnable: vis lo",
             (WORD)((DWORD)wScreenPitchBytes * (DWORD)wScreenY) );
    DiagHex( "PhysicalEnable: vis hi",
             (WORD)(((DWORD)wScreenPitchBytes * (DWORD)wScreenY) >> 16) );
    FLUSH();
    if (dwRegRet == VDD_DRIVER_REGISTER)
    {
        /* 非致命, 继续 */
    }

    /* 【r100】"保险: 再设一次模式"默认不再做(SetMode2=1 才做)。
     * 依据: ① 对照实现 boxv9x 没有这个动作; ② 它发生在"主 VDD 已被告知
     * 模式是 X"之后 ⇒ 自找的状态不一致; ③ 真机上每次 4F02 都是一次完整
     * 的 V86→VBIOS 往返(登录慢的第三个原因)。                      */

    VddCall( VDD_POST_MODE_CHANGE );
    s_inModeSet = 1;                     /* [r406] 标记: 模式/几何恢复期间禁止落盘 */
    VddCall( VDD_SAVE_DRIVER_STATE );
    DIAG( "PhysicalEnable: VDD post/save ok" );
    FLUSH();

    /*==========================================================================
     * 【r138 本轮最关键的一处 —— 把"已知良好样本"的生产者救回来】
     *
     * 【./137 的日志证明 r137 把自己的仪器锁死了】
     *   全份 ./137/OMNININE.LOG(6046 行)里:
     *     · `HwReg: sample ok (1=usable)=0001`  出现 0 次;
     *     · `DeferredRestore: entry hwreg have sample=0000`  出现 3 次;
     *     · `DeferredRestore: sessend hwreg have sample=0000` 出现 4 次;
     *     · `ApplyDesktopRestore: hwreg replay diff=EEEE`     出现 4 次
     *       (EEEE = "还没有样本");
     *     · `Relatch: ...` 一行都没有(它同样以 s_hwRegOk 为前提)。
     *   也就是说 r128 的寄存器回放、r133 的重新锁存、r134 的语义判据、
     *   r137 自己新加的 AR10h..AR14h 采样与回放 —— 【四条路一次都没走到】。
     *
     * 【为什么】样本的生产者只有一个: HwRegSampleAtRebuild(), 它挂在
     *   MaybeDeferredRestore 里"一次成功 4F02 重建之后"(display.c 的
     *   `if (g_runHwReg >= 1 && ok)`)。而 r137 的核心改动恰恰是
     *   【会话入口边沿一律不再设模式】(EntryNoSet=1) ⇒ 那条 4F02 不再发生
     *   ⇒ 采样点永远不执行 ⇒ 依赖样本的每一层防御同时变成死代码。
     *   对照 ./136: 那一次入口边沿发了 4F02, 于是 4521 行就有了
     *   `HwReg: sample ok (1=usable)=0001`, 之后 4962 行起
     *   `have sample=0001`, 回放也真的跑过(diff last=0000)。
     *   ⇒ 这不是"改错了", 是【改掉了一个动作, 而另一个动作挂在它后面】。
     *
     * 【怎么修才不会再被下一轮踩断】样本的语义是"硬件确实在我们的模式里
     *   的那一刻, 把这组寄存器抄下来"。而【这一行代码所在的位置就是全驱动
     *   最确定的那一时刻】:
     *     · 上面 SetDisplayMode(1) 刚刚用 4F02 把模式设成我们要的;
     *     · 4F01 的几何/pitch 已经读回并存进共享状态(PushState 之后);
     *     · 主 VDD 已经收到 POST_MODE_CHANGE + SAVE_DRIVER_STATE(它此刻
     *       承认的模式就是我们设的);
     *     · 驱动马上要返回 1, DIBENG 即将按这个模式开始画。
     *   所以在这里采样, 得到的样本比"某次重建之后"更可信 —— 而且它与
     *   任何"要不要设模式"的策略都无关: 开机相必然设过一次模式(上面那句
     *   SetDisplayMode(1) 就是), 这一枪永远有东西可采。
     *
     * 【为什么这是"只读"的, 所以可以放在开机相】
     *   r113 把采样从 Enable 尾部搬走, 理由是"开机期的端口访问与 ./112 的
     *   五次开机崩溃同批出现"。那是一个【相关性】观察, 而样本本身是
     *   【纯读】: VxdHwRegSave 只有 idxread(CRTC/SEQ/GCR)与 AtcReadIdx
     *   (AR)—— 一个字节都不写(写发生在 VxdHwRegRestore, 而那一条只在
     *   回调里跑)。所以这里【不引入任何新的写足迹】, 与 r112 那批被怀疑的
     *   动作不是同一类。
     *   另外 AR 那一段读不到时 s_hwRegArOk 会被置 0, 整段作废 —— 宁可没有
     *   AR 样本, 也不会留下一个假的"已知良好"。
     *
     * 【开关】system.ini [omninine] SampleAtBoot, 默认 1。设 0 = 退回 r137 的
     *   行为(样本只在重建后取), 单行可 A/B, 不必重编。
     * 【测什么】下一份日志里 `HwReg: sample at boot` 紧跟一行
     *   `HwReg: sample ok (1=usable)=0001`, 且之后的
     *   `DeferredRestore: entry hwreg have sample` 从 0000 变成 0001。
     *   只要这三行变了, 就说明 r128/r133/r134/r137 四层防御重新活了。
     *========================================================================*/
    if (g_runSampleAtBoot && g_runHwReg >= 1)
        HwRegSampleAtRebuild( "HwReg: sample at boot (r140)" );
    /* 【r139】开机相也盖一次 DAC 印章 —— 这是"健康状态"的印章基线。
     *   为什么必须在开机相盖: 下一次从 DOS 盒回来时, 印章【不在】才是判据
     *   (见 drv.c 的 VxdDacStampOk)。若开机就不盖, 第一条读数永远是"没了",
     *   判据会退化成恒真。
     *   位置: SetDisplayMode(1) 已经在上面跑过一遍并盖过章, 但 PhysicalEnable
     *   随后又补发了一次 VBE_SetMode(保险性再打一次), 所以这里必须再盖一次
     *   —— 保证"最后一次模式建立"之后印章一定在。bpp!=8 才盖, 同 SetDisplayMode。*/
    if (g_runDacStamp && wBpp != 8)
    {
        VxdDacStamp();
        PHASEV( "PH: dacstamp boot=", VxdDacStampOk() );
        PHASEV( "PH: dacfp boot=",    VxdDacFingerprint() );
    }
    /* r138-marker: HwRegSampleAtBoot (the sample producer is no longer the child
     *   of a mode-set policy) + HwTruth (4F03h hardware verdict) + CbNoSample
     *   (no sample = the DDK duty was not done) + AltFromTbl (no hardcoded VBE
     *   mode numbers).  Four decisions, four INI switches, one CFG17 line. */

    /* [r133] Relatch=2 -- re-latch the mode right here as well.
     *   Why it is opt-in and NOT the default: at boot there is no rebuild yet
     *   to hang the re-latch off (the first DeferredRestore only happens when a
     *   DOS session has come and gone), so a boot-time re-latch has to take its
     *   own sample first -- and the boot phase has its own crash history in this
     *   project (./112: five cold-boot failures landed in the same batch as the
     *   first boot-phase port accesses).  Level 1 therefore keeps the boot
     *   footprint exactly as it was; if the strip survives at level 1 while a
     *   session rebuild is clean, Relatch=2 puts the same step in the boot path
     *   for one boot, without a rebuild. */
    if (g_runRelatch >= 2)
    {
        (void)VxdHwRegSave();
        CrtRelatchAfterRebuild( "Relatch: at boot (Relatch=2)" );
    }

    ClearVisibleScreen();
    DIAG( "PhysicalEnable: done" );
    FLUSH();
    return 1;
}

/*
 * PhysicalDisable —— Disable 的硬件部分: 注销 VDD 注册 + 回文本模式。
 *   (I/O 陷阱的恢复由 Disable 在调用本函数【之前】完成, 顺序不可换:
 *   先恢复陷阱, VDD 才会重新虚拟化 VGA 端口, 文本模式设置才被各
 *   DOS VM 正确感知。)
 */
static void PhysicalDisable( void )
{
    VddCall( VDD_DRIVER_UNREGISTER );
    BIOS_SetTextMode();
}

/*
 * DiagRawPdev —— 把整本 DIBENGINE(48 字节)按原始十六进制落盘, 一行。
 *   【为何要"原始"而不只是命名字段】命名字段只能验证"我打算填的对不对";
 *   原始字节能看出"引擎在我填完之后又改了什么"(填充字节、引擎私有字段、
 *   我没注意到的字段)。r79 的 POSTDIB 行已经证明 DIB_Enable 会把 deType
 *   写成 'DI'(4449h)、deFlags 写成 SELECTEDDIB(0004h)、deBits 指到一个
 *   临时 DIB —— 这些都不是文档写明的, 只有原始字节能看见。
 *   【零副作用】纯读, 不改任何字段, 也不依赖 DIBENG 的任何内部知识。
 */
static void DiagRawPdev( const char *tag, LPDIBENGINE lpEng )
{
#ifdef VBE9X_DIAG
    static char       buf[192];
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = buf;
    const BYTE __far *b = (const BYTE __far *)lpEng;
    int               i;

    while (*tag && p < buf + 32)
        *p++ = *tag++;
    if (!lpEng)
    {
        *p = 0;
        DiagWriteLog( buf );
        return;
    }
    for (i = 0; i < 48; ++i)
    {
        *p++ = ' ';
        *p++ = hexd[(b[i] >> 4) & 15];
        *p++ = hexd[b[i] & 15];
    }
    *p = 0;
    DiagWriteLog( buf );
#else
    (void)tag;
    (void)lpEng;
#endif
}

/*
 * Enable —— GDI 分两次调用: 先(style&1)!=0 查询 GDIINFO,
 *   后(style&1)==0 初始化 PDEVICE 与硬件。
 *   【为何先查表后初始化】GDIINFO 阶段经 DIB_Enable 得到
 *   dpDEVICEsize(DIB 引擎要求的 PDEV 大小), 记入 wDIBPdevSize;
 *   PDEVICE 阶段据此把 BITMAPINFO 放到 PDEV 之后的附加空间里。
 */
UINT WINAPI __loadds Enable( LPVOID lpDevice, UINT style, LPSTR lpDeviceType,
                             LPSTR lpOutputFile, LPVOID lpStuff )
{
    WORD rc;

#ifdef VBE9X_PROBEONLY
    /* 裁决构建: 让 GDI 走"驱动不可用→回退 VGA"路径, 保证启动继续 */
    DIAG( "PROBEONLY: Enable stub, return 0" );
    return 0;
#endif

    /* DIBENG 懒解析: GDI 阶段首次进入时 DIBENG 已随 GDI 驻留。
       若此前已有转发器触发过解析, 这里只是轻量复核。                 */
    DIAG( "Enable: enter" );
    /* 【r92 关键取证】把 GDI 传进来的 5 个参数原样落盘(只读, 不改任何东西)。
     * 依据: r89/r90/r91 三次落盘的 "RAW PDEV before/after DIB_Enable" 显示,
     * 我们当作 PDEV 去填的那块内存在【Enable 刚进入时】装的是 DIBENG 自己的
     * NE 常驻名表 —— `12 "Windows DIB Engine" 0000`(描述项, 序号 0)
     * `0B "DIB_DISABLE" 0400`(序号 4)、`1A "DIB_GetPaletteTranslateExt" …`.
     * 名字+序号成对出现, 只可能是模块常驻名表, 不可能是"恰好残留"。
     * 因此必须确认: lpDevice 到底是不是 PDEV 缓冲区? 五个参数有没有错位?
     * (若参数错位, 则我们一直在写别人的结构 —— 这能解释"六/七种配置全部
     * 同址崩"和"不清屏只崩不花"。) */
    DiagHex( "Enable: style",        style );
    DiagHex( "Enable: p1 lpDev seg", (WORD)(((DWORD)lpDevice) >> 16) );
    DiagHex( "Enable: p1 lpDev off", (WORD)((DWORD)lpDevice) );
    DiagHex( "Enable: p2 devType seg", (WORD)(((DWORD)lpDeviceType) >> 16) );
    DiagHex( "Enable: p2 devType off", (WORD)((DWORD)lpDeviceType) );
    DiagHex( "Enable: p3 outFile seg", (WORD)(((DWORD)lpOutputFile) >> 16) );
    DiagHex( "Enable: p3 outFile off", (WORD)((DWORD)lpOutputFile) );
    DiagHex( "Enable: p4 stuff seg", (WORD)(((DWORD)lpStuff) >> 16) );
    DiagHex( "Enable: p4 stuff off", (WORD)((DWORD)lpStuff) );
    /* 【r93】结构尺寸自证。r92 的日志把两处都排除了(PDEV 布局 0x458 正确、
     * GDIINFO 已清零并逐字段填写), 剩下的两种可能都必须先看这三个数:
     *   · 若 sizeof(GDIINFO) 与 DDK 的 GDIINFO 不一致(例如某些 DWORD 字段被
     *     声明成 WORD —— L2530 的 W135 就是这个征兆), 则我们从 dpRaster 之后
     *     的每个字段都写错了位置, GDI 读到的是一份"错位表";
     *   · 若 sizeof(DIBENGINE) 不是 0x30, 则 BITMAPINFO 的落点(wDIBPdevSize
     *     =0x30)与 DDK 布局就不一致。
     * 三个数一比就知道是哪一种, 且不需要再做任何配置实验。 */
    DiagHex( "sizeof GDIINFO",   (WORD)sizeof( GDIINFO ) );
    DiagHex( "sizeof DIBENGINE", (WORD)sizeof( DIBENGINE ) );
    DiagHex( "sizeof BMIHDR",    (WORD)sizeof( BITMAPINFOHEADER ) );
    DiagHex( "sizeof RGBQUAD",   (WORD)sizeof( RGBQUAD ) );
    FLUSH();
    if (!g_dibfnReady)
    {
        DIAG( "Enable: DIBENG not resolved yet, resolving now" );
        FLUSH();
        if (!DibEnsureResolved())
        {
            DIAG( "Enable: DIBENG unavailable, fail" );
            FLUSH();
            return 0;
        }
    }
    DIAG( "Enable: DIBENG ok" );
    FLUSH();

    if (!(style & 1))
    {
        /* ==================== PDEVICE 初始化阶段 ==================== */
        LPDIBENGINE  lpEng = lpDevice;
        LPBITMAPINFO lpInfo;
        WORD         wFlags;

        DIAG( "Enable: PDEVICE phase" );
        FLUSH();
        lpDriverPDevice = lpDevice;

        /* 【r81】先读运行时配置(system.ini [omninine])再进 PhysicalEnable ——
         * 屏选择子的形态(SelMode)与 wPDeviceFlags 覆盖(Flags)都在
         * PhysicalEnable→SetDisplayMode 里生效, 必须在此之前读。
         * ReEnable 路径已经读过一次, 不重复。                       */
        if (!bReEnabling)
            ReadRunProfile();

        /* 【r94 发现 + r95 定案】本相后面每一次进 DIBENG(CreateDIBPDevice、
         * DIB_SetPaletteTranslateExt, 以及 GDI 之后经跳板的全部绘图调用)
         * 都要让被调方拿到 DIBENG 自己的 DS —— 实测被调方自己不装 DS, 装 DS
         * 的是装载器桩, 所以我们必须替它装(vddapi16.asm 的 DIBJUMP)。
         * 模块库解析出来的那个值已被证明是文件偏移(见 DibDgroupProbe 长注释),
         * 这里用装载器桩自证修正, 并把"值可信"记进 g_dibDgTrusted(只有它为 1
         * 时跳板才敢装 DS)。
         * 只在 !bReEnabling 时做一次: 它是会话级常量, 校正后一直有效。 */
        if (!bReEnabling)
            DibDgroupProbe();

        rc = PhysicalEnable();
        if (!rc)
        {
            DIAG( "Enable fail: PhysicalEnable" );
            FLUSH();
            return 0;
        }
        DIAG( "Enable: PhysicalEnable ok" );
        FLUSH();

        /* 【r76c 分步落盘】下面每一步都可能进到别的模块, 而 r76b 的
         * 0D 蓝屏(0427:00002B70 —— 不是本模块的段/偏移)就发生在这一段。
         * 每步前后各写一行, 下次日志的最后一行就直接指出死在哪一步。 */
        DiagHex( "Enable: wDIBPdevSize", wDIBPdevSize );
        DiagHex( "Enable: lpDev seg", (WORD)(((DWORD)lpDevice) >> 16) );
        DiagHex( "Enable: wScreenX", wScreenX );
        DiagHex( "Enable: wScreenY", wScreenY );
        DiagHex( "Enable: pitch", wScreenPitchBytes );
        DiagHex( "Enable: wBpp", wBpp );
        DiagHex( "Enable: wPDeviceFlags", wPDeviceFlags );
        FLUSH();

        /* 驱动已接管屏幕: 通知主 VDD 停止虚拟化系统 VM 的显示端口,
           此后我们的 DAC/帧缓冲访问直达硬件。ReEnable 时不重复。      */
        if (!bReEnabling)
        {
            DIAG( "Enable: before STOP_IO_TRAP" );
            FLUSH();
            VDD_StopIOTrap();
            DIAG( "Enable: after STOP_IO_TRAP" );
            FLUSH();
        }

        /* 让 DIB 引擎做它的那部分 PDEV 初始化 */
        /* 【r81 取证】进门前的原始 PDEV: 这是 GDI 分配的缓冲区, 里面原本
         * 是什么只有这里能看见(引擎进门后就会改写一部分)。          */
        DiagRawPdev( "RAW PDEV before DIB_Enable:", lpEng );

        /* 【r81 可选实验 · 默认关闭】Sentinel=1 时把 4 个"文档没说引擎会
         * 不会碰"的字段填成可识别花样(其余字段一概不动 —— deBits/
         * deBitmapInfo/deBeginAccess 有被引擎读取的可能, 绝不拿它们做实验)。
         * 对照后面的 POSTDIB/RAW 行即可判定 DIB_Enable 是否读写它们。 */
        if (g_runSentinel)
        {
            lpEng->deType           = 0xB0B0;
    lpEng->deFlags          = 0x8041;   /* 【r406】此处恢复原值; 真正的 A/B 在 wFlags 赋值之后 */
            lpEng->deReserved1      = 0xB2B2B2B2UL;
            lpEng->deDriverReserved = 0xB3B3B3B3UL;
            DIAG( "SENTINEL: deType/deFlags/deReserved1/deDriverReserved prefilled B0..B3" );
            DiagRawPdev( "RAW PDEV sentinel:", lpEng );
        }

        /* 【r91 关键修正】不再无条件自己调 DIB_Enable。
         * r80..r90 在这里(DIBENGINE 还没填之前)调用它, 引擎于是按一本
         * 未初始化/残留的 PDEV 建了 1x1x24bpp 模板设备(见 RAW/POSTDIB 行),
         * 我们再用手填值覆盖 —— 引擎内部状态与 GDI 读到的结构就此不一致。
         * 六种配置(CDPD/DelpDev/Flags/Access/SelfTest)全部同址蓝屏, 说明
         * 病灶就在这条所有配置共有的调用上。默认跳过(GDI 会调用引擎的
         * Enable); DibEnable=1 可回到旧行为做对照。 */
        if (g_runDibEnable)
        {
            DIAG( "Enable: before DIB_Enable(PDEV) [DibEnable=1]" );
            FLUSH();
            DIB_Enable( lpDevice, style, lpDeviceType, lpOutputFile, lpStuff );
            DIAG( "Enable: after DIB_Enable(PDEV)" );
        }
        else
        {
            DIAG( "Enable: DIB_Enable NOT called here (DibEnable=0)" );
        }
        FLUSH();
        DiagRawPdev( "RAW PDEV after DIB_Enable:", lpEng );

        /* 【r78 取证: 引擎留下了什么】DIB_Enable 会把这本 PDEV 初始化一遍
         * (DDK 的 CreateDIBPDevice 亦同: deDriverReserved 置 0,
         * deBeginAccess/deEndAccess 置 0)。下面在【我们覆盖之前】把引擎
         * 留下的值原样落盘 —— 万一某个字段是引擎放进去、而我们随后盖成
         * 0 的"活指针", 这就是第一现场。(参考驱动的做法是"不碰
         * deDriverReserved/delpPDevice", 与 CreateDIBPDevice 的置 0 等价。)*/
        DiagHex( "POSTDIB: deType", lpEng->deType );
        DiagHex( "POSTDIB: deFlags", lpEng->deFlags );
        DiagHex( "POSTDIB: deBitsOffset lo", (WORD)lpEng->deBitsOffset );
        DiagHex( "POSTDIB: deBitsOffset hi", (WORD)(lpEng->deBitsOffset >> 16) );
        DiagHex( "POSTDIB: deBitsSelector", lpEng->deBitsSelector );
        DiagHex( "POSTDIB: deDrvResv lo", (WORD)lpEng->deDriverReserved );
        DiagHex( "POSTDIB: deDrvResv hi", (WORD)(lpEng->deDriverReserved >> 16) );
        DiagHex( "POSTDIB: delpPDev seg", (WORD)(((DWORD)lpEng->delpPDevice) >> 16) );
        DiagHex( "POSTDIB: deVersion", lpEng->deVersion );
        DiagHex( "POSTDIB: beginAcc seg", (WORD)(((DWORD)lpEng->deBeginAccess) >> 16) );
        DiagHex( "POSTDIB: beginAcc off", (WORD)((DWORD)lpEng->deBeginAccess) );
        DiagHex( "POSTDIB: endAcc seg", (WORD)(((DWORD)lpEng->deEndAccess) >> 16) );
        FLUSH();

        /* 【r96 挪走了】r81..r95 在这里调
         *     if (wPalettized) DIB_SetPaletteTranslateExt( NULL, lpDriverPDevice );
         * r96-marker: xlat call moved below the PDEVICE build (see the
         * g_runXlat comment; it used to run against an unbuilt PDEVICE)
         * —— 此刻 lpDriverPDevice 指向的缓冲区里装的还是 GDI 堆的残留
         * (r89..r95 的 "RAW PDEV" 行: DIBENG 自己的常驻名表), BITMAPINFO
         * 与本结构一个字都还没填。拿这种东西当 PDEV 交给引擎, 轻则引擎
         * 整本读错, 重则它按里面的野指针去写翻译表。
         * 现在这一步移到 CreateDIBPDevice 建好 PDEVICE【之后】(见下方
         * "r96: 引擎的调色板翻译" 一段), 由 Xlat= 开关控制。 */

        /* BITMAPINFO 紧接 PDEV(wDIBPdevSize 偏移)之后 */
        lpInfo = (LPBITMAPINFO)((LPBYTE)lpDevice + wDIBPdevSize);
        DIAG( "Enable: BITMAPINFO at PDEV+dpDEVICEsize" );
        FLUSH();
        _fmemset( &lpInfo->bmiHeader, 0, sizeof( lpInfo->bmiHeader ) );
        lpInfo->bmiHeader.biSize     = sizeof( lpInfo->bmiHeader );
        lpInfo->bmiHeader.biWidth    = wScreenX;
        lpInfo->bmiHeader.biHeight   = wScreenY;
        lpInfo->bmiHeader.biPlanes   = 1;
        lpInfo->bmiHeader.biBitCount = wBpp;

        /* 8bpp: 建立默认颜色表(DDK 标准 20 保留色布局) */
        if (wBpp == 8)
        {
            lpColorTable = (RGBQUAD FAR *)&lpInfo->bmiColors;
            if (!bReEnabling)
            {
                int i;
                _fmemcpy( lpColorTable, DIB8ColorTable1,
                          sizeof( DIB8ColorTable1 ) );
                for (i = 10; i < 246; ++i)
                    lpColorTable[i] =
                        *(const RGBQUAD FAR *)&DIB8ColorTable2[0];
                _fmemcpy( &lpColorTable[246], DIB8ColorTable3,
                          sizeof( DIB8ColorTable3 ) );
            }
        }

        /* 直接填写 DIBENGINE(vmdisp9x 验证: 不必走 CreateDIBPDevice,
           填好结构即可; 且后者返回值的寄存器约定与文档不符, 易踩坑)
           【r76】补上 deDriverReserved=0: 这是结构里唯一没被显式初始化
           的字段(留给小驱动的 DWORD), 不写就是 GDI 给的 PDEV 缓冲区里
           的残留数据。deBitsOffset/deBitsSelector 构成"DIB 引擎 48 位
           指针"的前半/后半(见 ddk/dibeng.h 字段注释): 偏移 0 表示
           选择子基址就是帧缓冲起点。                              */
        wFlags = wPDeviceFlags;
        if (wPalettized)
            wFlags |= PALETTIZED;

        lpEng->deType          = TYPE_DIBENG;
        lpEng->deFlags         = wFlags;

    DiagHex( "R290: wFlags",      wFlags );
    DiagHex( "R290: deFlags now", lpEng->deFlags );
        lpEng->deWidth         = wScreenX;
        lpEng->deHeight        = wScreenY;
        lpEng->deWidthBytes    = wScreenPitchBytes;
        lpEng->deDeltaScan     = wScreenPitchBytes;
        lpEng->dePlanes        = 1;
        lpEng->deBitsPixel     = (wBpp + 7) & 0xFFF8; /* 必须为 8 的倍数 */
        lpEng->deReserved1     = 0;
        /* 【r83 根因修正】delpPDevice 必须指向"物理设备 = 显存", 绝不能留 0。
         * 依据两条, 都不是猜测:
         *   ① Win95 DDK 对该字段的原文: "delpPDevice — Address of the
         *      physical device. This can be a pointer to either video
         *      memory, a device-dependent bitmap, or a device-independent
         *      bitmap." —— 字段名里的 lpPDevice 是误导: 它不是 PDEVICE
         *      结构指针, 而是"画面本体在哪"; 屏幕设备这一档就是显存。
         *   ② 在售参考驱动 boxv9x(enable.c) 根本不手填这本结构, 而是调
         *      DDK 的 CreateDIBPDevice(lpInfo, lpDevice, ScreenSelector:0,
         *      wFlags); 该函数文档写明 lpBits 非 NULL 时就用它填 deBits 的
         *      选择子与偏移, 而 delpPDevice 正落在"video memory"那一档 ——
         *      也就是 ScreenSelector:0。我们手填的其余字段与它的产物逐字段
         *      一致, 唯独这一格留了 0。
         * 【为什么 0 会蓝屏 0D】引擎按 16:16 远指针解引用它 → 选择子 0 是
         * 空选择子, 一访问就是 #GP(0) → 0D; 而且崩在引擎/GDI 自己的代码
         * 里, 与"没有任何 DDI 探针触发、崩点在 Enable 返回之后"的现场完全
         * 吻合(r76b..r82 五轮皆然)。
         * 【形态】形态 B(选择子基址=帧缓冲)下 LfbAccessOffset()=0, 得到的
         * 就是"屏选择子:0"; 形态 A(0 基址 + 线性偏移)的偏移是线性地址,
         * 装不进 16 位 —— 那种形态下本字段无法表达, 下面另打一行警告。
         * 【r82 的旧注释错在哪】它把 DDK 关于 CreateDIBPDevice 的
         * "lpBits 为 NULL 时……" 那句【条件句】读成了对小驱动的通用建议,
         * 于是认定"0 基址选择子 + 偏移放线性地址"是标准形态, 并顺手把
         * delpPDevice 也置成 0。参考驱动传的是【非 NULL】的 lpBits(屏选择
         * 子), 那句根本不适用 —— 这正是本轮翻案的那一处误读。          */
        lpEng->delpPDevice     = (LPBYTE)MK_FP( g_fbSel, (WORD)LfbAccessOffset() );
        if ((DWORD)(WORD)LfbAccessOffset() != LfbAccessOffset())
            DIAG( "PDEV: delpPDev: form A offset >64KB -> video pointer not expressible" );
        /* 【r80】deBitsOffset 与选择子形态同源(见 drv.c LfbAccessOffset):
         * "0 基址选择子"形态 → 线性地址; 退回形态 → 0。
         * DDK 对 CreateDIBPDevice 的 lpBits 原文: "the minidriver must
         * allocate a 0-based buffer and fill in the selector part of the
         * deBits field" —— 小驱动这一侧的标准形态就是"0 基址选择子 +
         * 偏移里放线性地址"(在售参考驱动 vmdisp9x 亦然)。这样 DIBENG
         * 无论把 deBits 当"选择子:偏移"还是当线性地址用, 结果都对;
         * 旧形态(基址=帧缓冲、偏移=0)只在第一种解读下正确。
         * 【r81】system.ini [omninine] BitsOff=1 可强制填 0 —— 用于把"偏移
         * 填法"与"选择子形态"这两个变量分开验证(SelMode=1 时偏移本就是
         * 0; SelMode=0 配 BitsOff=1 是一种【故意无效】的对照组合)。 */
        lpEng->deBitsOffset    = g_runBitsOff ? 0 : LfbAccessOffset();
        lpEng->deBitsSelector  = g_fbSel;   /* DPMI 自建, ring 3 可载   */
        lpEng->deBitmapInfo    = lpInfo;

    /* 【r406 已撤回 r288 的改动】不再覆盖 deBeginAccess/deEndAccess。
     *   依据(微软 Win95 DDK《DIBENGINE》原文):
     *     deBeginAccess - Begin surface access call back.
     *     deEndAccess   - End surface access call back.
     *   它们是"表面访问"回调, 不是光标排他; r288 把它们从引擎自己的
     *   1A00:0445 换成我们的 02E7:3482, 实测对陈旧块无效, 故撤回, 恢复原状。
     *   【r406】补上"访问窗口"回调。
     *   r287 的日志证明 PDEV 其余字段全对(deBitsSelector=2077=我们的选择子,
     *   deBitsOffset=0, 0400x0300, pitch 0800, 16bpp, BMI 自洽), 但
     *       POSTDIB/PDEV: beginAcc = 0000:0000
     *       POSTDIB/PDEV: endAcc   = 0000:0000
     *   即【引擎读写物理帧缓冲前后没有任何"取访问权"的动作】。
     *   写往往能落下去, 读则可能拿到未同步的内容 —— 这正是"写对、读黑"的形态,
     *   也就是光标/菜单背景被存成 0、撤走时写回黑块的来源。
     *   CreateDIBPDevice 已失败(CDPD ret=0000), 这两个字段只能由我们补。 */
    lpEng->deBeginAccess = BeginAccess;
    lpEng->deEndAccess   = EndAccess;

    /* 回读校验: 填了就必须读回来确认, 不允许"以为填好了" */
    DiagHex( "R288: beginAcc seg", (WORD)(((DWORD)lpEng->deBeginAccess) >> 16) );
    DiagHex( "R288: beginAcc off", (WORD)((DWORD)lpEng->deBeginAccess) );
    DiagHex( "R288: endAcc seg",   (WORD)(((DWORD)lpEng->deEndAccess) >> 16) );
    DiagHex( "R288: endAcc off",   (WORD)((DWORD)lpEng->deEndAccess) );
        lpEng->deVersion       = 0x400;
        lpEng->deBeginAccess   = BeginAccess;
        lpEng->deEndAccess     = EndAccess;
        lpEng->deDriverReserved = 0;

        /* 【r84】让引擎自己建这本 PDEVICE(见 Cdpd 处说明)。放在手填【之后】:
         *   · CDPD=0        → 与 r83 完全相同(手填);
         *   · CDPD=1(默认)  → 引擎整本覆盖我们的手填值;
         *   · 引擎返回 0    → 手填值原样保留 = 自动回退, 决不因此让 Enable 失败。
         * 两种路径都不必改动上面那段逐字段赋值 —— 改动面最小、可逆。 */
        if (g_runCdpd)
        {
            CdpdInfo   = FarData( (const void __far *)lpInfo );
            CdpdDevice = FarData( (const void __far *)lpDevice );
            CdpdBits   = FarData( (const void __far *)MK_FP( g_fbSel,
                                           (WORD)LfbAccessOffset() ) );
            CdpdFlags  = wFlags;
            DiagHex( "CDPD: lpInfo seg",   (WORD)(CdpdInfo >> 16) );
            DiagHex( "CDPD: lpInfo off",   (WORD)CdpdInfo );
            DiagHex( "CDPD: lpDevice seg", (WORD)(CdpdDevice >> 16) );
            DiagHex( "CDPD: lpBits sel",   (WORD)(CdpdBits >> 16) );
            DiagHex( "CDPD: wFlags",       CdpdFlags );
            FLUSH();
            g_cdpdRc = Cdpd();
            DiagHex( "CDPD: CreateDIBPDevice ret", g_cdpdRc );
            if (g_cdpdRc)
            {
                /* DDK 原文: "The DIB engine initializes the deBeginAccess and
                 * deEndAccess members ... to zero. The minidriver needs to
                 * initialize these fields after the call to CreateDIBPDevice."
                 * —— 这两个回调是引擎特意留给小驱动的, 必须补上。      */
                /* r90: g_dibfn[] is a DWORD array (far pointer values), so the
                 * two choices below are stored as raw 32-bit far pointers --
                 * that keeps the assignment independent of the field's exact
                 * function-pointer type. */
                if (g_runAccess == 1 && g_dibfn[29] && g_dibfn[30])
                {
                    *(DWORD __far *)&lpEng->deBeginAccess = g_dibfn[29];
                    *(DWORD __far *)&lpEng->deEndAccess   = g_dibfn[30];
                }
                else if (g_runAccess2)
                {
                    /* 【r102】两个空回调。Access2 的来由见 g_runAccess2 注释:
                     * 透传版本会让"引擎→回调→引擎"在光标路径上成环, 而本
                     * 驱动并没有需要排除的硬件光标, 因此"什么都不做"在语义
                     * 上就是正确的回调内容, 不是降级。                    */
                    lpEng->deBeginAccess = NullBeginAccess;
                    lpEng->deEndAccess   = NullEndAccess;
                }
                else
                {
                    lpEng->deBeginAccess = BeginAccess;
                    lpEng->deEndAccess   = EndAccess;
                }
                DiagHex( "CDPD: slot105 seg", (WORD)(g_dibfn[29] >> 16) );
                DiagHex( "CDPD: slot105 off", (WORD)g_dibfn[29] );
                DiagHex( "CDPD: slot106 seg", (WORD)(g_dibfn[30] >> 16) );
                DiagHex( "CDPD: slot106 off", (WORD)g_dibfn[30] );
                DiagHex( "CDPD: beginAcc seg", (WORD)(((DWORD)lpEng->deBeginAccess) >> 16) );
                DiagHex( "CDPD: beginAcc off", (WORD)((DWORD)lpEng->deBeginAccess) );
                DIAG( "CDPD: deBegin/EndAccess chosen (see run Access)" );

                /* 【r87】DDK 说 delpPDevice 是"物理设备(=显存)的地址"; 引擎
                 * 自建时把它留 0。r83 填过一次但被 DS bug 掩盖，这里重新给上，
                 * 并保留 DelpDev=0 的对照开关。                            */
                if (g_runDelpDev == 2)
                {
                    /* 【r93】DDK 头(本工程 ddk/dibeng.h L23)对 delpPDevice 的
                     * 原文是 "Associated PDevice pointer" —— 若那是字面意思,
                     * 它应当指回【本 PDEVICE 自己】, 而不是显存。这一支就是
                     * 那个读法的对照: DelpDev=2。 */
                    lpEng->delpPDevice = (LPBYTE)lpDevice;
                    DIAG( "CDPD: delpPDevice = the PDEVICE itself (DelpDev=2)" );
                }
                else if (g_runDelpDev == 1)
                {
                    lpEng->delpPDevice = (LPBYTE)MK_FP( g_fbSel,
                                             (WORD)LfbAccessOffset() );
                    DIAG( "CDPD: delpPDevice set to screen pointer (DelpDev=1)" );
                }
                else
                {
                    DIAG( "CDPD: delpPDevice left as the engine built it (DelpDev=0)" );
                }
                DiagHex( "CDPD: delpPDev seg", (WORD)(((DWORD)lpEng->delpPDevice) >> 16) );
                DiagHex( "CDPD: delpPDev off", (WORD)((DWORD)lpEng->delpPDevice) );
                DIAG( "CDPD: PDEVICE built by CreateDIBPDevice (engine path)" );
            }
            else
            {
                DIAG( "CDPD: CreateDIBPDevice returned 0 -> keep hand filled PDEVICE" );

    /* r406: the fallback path was missing what the engine sets for itself.
     *   Evidence (293d/293e logs): whenever CreateDIBPDevice succeeds the log shows
     *       slot105 = 031F:0F50   slot106 = 031F:0FD4   deBegin/EndAccess chosen
     *   and there is no artefact; whenever it fails (1024x768, ret=0000) the PDEV is
     *   hand filled, deBeginAccess/deEndAccess stay 0 (r293) or point at OUR routines
     *   (r288) -- neither is what the engine installs -- and the artefact appears.
     *   So point the two callbacks at DIBENG own ordinals 105/106, as the engine does.
     *   g_dibfn[29] / g_dibfn[30] are those entry points (already fetched). */
    if (g_dibfn[29] && g_dibfn[30])
    {
        *(DWORD __far *)(void __far *)&lpEng->deBeginAccess = g_dibfn[29];
        *(DWORD __far *)(void __far *)&lpEng->deEndAccess   = g_dibfn[30];
        DiagHex( "R296: beginAcc seg", (WORD)((DWORD)lpEng->deBeginAccess >> 16) );
        DiagHex( "R296: beginAcc off", (WORD)(DWORD)lpEng->deBeginAccess );
        DiagHex( "R296: endAcc seg",   (WORD)((DWORD)lpEng->deEndAccess >> 16) );
        DiagHex( "R296: endAcc off",   (WORD)(DWORD)lpEng->deEndAccess );
    }
    else
    {
        DiagHex( "R296: dibfn 105/106 missing", 0 );
    }

    /* 【r406】写后立即读回 —— 判缓存/写合并一致性(本版唯一改动)。
     *   症状"光标处出现鼠标之前扫过的旧内容 / 偶尔纯黑"等价于"读到的总是上一版画面"。
     *   若 LFB 线性映射可缓存而显存不 snoop: 写停在 CPU cache, 读拿到显存旧值。
     *   做法: 存原值 -> 写花样 -> 立刻读回 -> 恢复, 统计不匹配数。 */
    if (g_fbSel)
    {
        WORD nBad = 0;
        WORD kk;
        for (kk = 0; kk < 4; kk++)
        {
            BYTE __far *pp = (BYTE __far *)MK_FP( g_fbSel, (WORD)(0x2000 + kk * 0x2000) );
            BYTE s0 = *pp;
            BYTE s1 = *(pp+1);
            *pp = 0x5A;
            *(pp+1) = 0xA5;
            if ( (*pp != 0x5A) || (*(pp+1) != 0xA5) ) nBad++;
            *pp = s0;
            *(pp+1) = s1;
        }
        DiagHex( "R293: cache-prb bad of 4 (0 = coherent)", nBad );
        PHASEV( "PH: r412 cbad=", nBad );
    }

    /* 【r406】覆盖点搬到 CreateDIBPDevice 【之后】。
     *   依据 r290 日志的时间线:
     *     733 R290: deFlags now=0041   ← 我们的覆盖确实生效
     *     742 CDPD: CreateDIBPDevice ret=0000 (失败)
     *     748 PDEV: deFlags=8041       ← 又变回去了
     *   源码树里 deFlags 只有两处赋值, 都在覆盖之前 ⇒ 只能是引擎在
     *   CreateDIBPDevice 内部重写了它, 然后才返回失败。 */
    lpEng->deFlags = wFlags;   /* r406: 复原 r291 的 A/B, 本版只做一致性探针 */
    DiagHex( "R291: deFlags after CDPD", lpEng->deFlags );   /* 必须 0041 */

    /* 【r406】补上 delpPDevice —— 唯一一个我们一直留 0 的文档字段。
     *   DDK《DIBENGINE》原文: "delpPDevice - Address of the physical device.
     *   This can be a pointer to either video memory, a DDB, or a DIB."
     *   我们的 PDEV 日志一直是 delpPDev seg=0000 off=0000。
     *   症状(鼠标扫过的旧内容出现在光标处)指向"偏移/跨距算错", 而引擎算物理
     *   表面地址时若拿不到这个指针, 就会退回另一套推算方式。 */
    lpEng->delpPDevice = 0;   /* r406: 复原 r292, 本版只做一致性探针 */
    DiagHex( "R292: delpPDev sel", (WORD)FP_SEG(lpEng->delpPDevice) );
    DiagHex( "R292: delpPDev off", (WORD)FP_OFF(lpEng->delpPDevice) );
            }
            FLUSH();
        }

        DIAG( "Enable: DIBENGINE filled" );
        FLUSH();

        /* 【r96: 引擎的调色板翻译】这一步的位置是本轮改动之一。
         * 依据(DDK 次序, 一条都不靠猜):
         *   ① CreateDIBPDevice 之前必须有 BITMAPINFO(表面几何), 它建出
         *      PDEVICE 并把 deBitmapInfo 指过去, 还把 deBeginAccess/
         *      deEndAccess 清 0 让小驱动自己补 —— 上面都已照做;
         *   ② 引擎的调色板翻译表是【按 PDEVICE 的调色板】建的, 所以必须在
         *      PDEVICE 存在且有颜色表之后调, 否则它读到的是一本没建好的
         *      结构(r81..r95 就是这样: 调在"填 BITMAPINFO 之前")。
         * Xlat=0 时整段跳过: 桌面调色板只由我们刷 DAC(SetPalette/Enable
         * 末尾那两处), 引擎那侧不做翻译 —— 这是最小可用退路。 */
        if (g_runXlat && wPalettized)
        {
            DIAG( "Enable: DIB_SetPaletteTranslateExt (after PDEVICE build)" );
            FLUSH();
            DIB_SetPaletteTranslateExt( NULL, lpDriverPDevice );
            DIAG( "Enable: xlat call returned" );
            DiagHex( "Enable: PDEV deFlags after xlat", lpDriverPDevice->deFlags );
            FLUSH();
        }

        /* 【r76d 全量取证】把交给 GDI 的 PDEV/DIBENGINE 逐字段落盘。
         * GDI 在 Enable 返回后会【直接读这些字段】去搭屏幕位图/DC; 只要
         * 有一个字段不对, 崩的就是 GDI 自己的代码 —— 这与本轮"没有任何
         * DDI 探针触发、崩溃点在 GDI 段内"的现象完全吻合。把每个字段都
         * 写进日志, 就能逐条与 DDK/在售驱动的要求核对。              */
        DiagHex( "PDEV: deType",         lpEng->deType );
        DiagHex( "PDEV: deFlags",        lpEng->deFlags );
        DiagHex( "PDEV: deWidth",        lpEng->deWidth );
        DiagHex( "PDEV: deHeight",       lpEng->deHeight );
        DiagHex( "PDEV: deWidthBytes",   lpEng->deWidthBytes );
        DiagHex( "PDEV: deDeltaScan lo", (WORD)lpEng->deDeltaScan );
        DiagHex( "PDEV: deDeltaScan hi", (WORD)(lpEng->deDeltaScan >> 16) );
        DiagHex( "PDEV: dePlanes",       (WORD)lpEng->dePlanes );
        DiagHex( "PDEV: deBitsPixel",    (WORD)lpEng->deBitsPixel );
        DiagHex( "PDEV: deBitsOffset lo", (WORD)lpEng->deBitsOffset );
        DiagHex( "PDEV: deBitsOffset hi", (WORD)(lpEng->deBitsOffset >> 16) );
        DiagHex( "PDEV: deBitsSelector", lpEng->deBitsSelector );
        DiagHex( "PDEV: deVersion",      lpEng->deVersion );
        DiagHex( "PDEV: bmi seg",  (WORD)(((DWORD)lpEng->deBitmapInfo) >> 16) );
        DiagHex( "PDEV: bmi off",  (WORD)((DWORD)lpEng->deBitmapInfo) );
        DiagHex( "PDEV: bmi biSize",     lpInfo->bmiHeader.biSize );
        DiagHex( "PDEV: bmi biWidth",    lpInfo->bmiHeader.biWidth );
        DiagHex( "PDEV: bmi biHeight",   lpInfo->bmiHeader.biHeight );
        DiagHex( "PDEV: bmi biBitCount", lpInfo->bmiHeader.biBitCount );
        DiagHex( "PDEV: beginAcc seg", (WORD)(((DWORD)lpEng->deBeginAccess) >> 16) );
        DiagHex( "PDEV: beginAcc off", (WORD)((DWORD)lpEng->deBeginAccess) );
        DiagHex( "PDEV: endAcc seg",   (WORD)(((DWORD)lpEng->deEndAccess) >> 16) );
        DiagHex( "PDEV: endAcc off",   (WORD)((DWORD)lpEng->deEndAccess) );
        DiagHex( "PDEV: delpPDev seg", (WORD)(((DWORD)lpEng->delpPDevice) >> 16) );
        DiagHex( "PDEV: delpPDev off", (WORD)((DWORD)lpEng->delpPDevice) );
        DiagHex( "PDEV: deReserved1 lo", (WORD)lpEng->deReserved1 );

        /* 【r89】分层卡: 本机每一处"分层决策"实际走的是哪一支。放在最后,
         * 前面所有 LfbMap:/SEL:/FbTest/CDPD: 行是"为什么", 这里是"是什么"。*/
        LayerCard( lpEng->deFlags );
        FLUSH();

        /* 8bpp: 把默认调色板写入硬件 DAC */
        if (wBpp == 8 && lpColorTable)
            DAC_SetPalette( 0, 256, (const uint8_t __far *)lpColorTable );

        /* 懒加载 USER 的重绘回调(序号 275, 未公开但 Win9x 恒定) */
        if (!RepaintFunc)
        {
            HMODULE hMod;
            DIAG( "Enable: before USER repaint lookup" );
            FLUSH();
            hMod = GetModuleHandle( "USER" );
            DiagHex( "Enable: USER handle", hMod );
            FLUSH();
            RepaintFunc = GetProcAddress( hMod, (LPCSTR)REPAINT_ORDINAL );
            DiagHex( "Enable: repaint lo", (WORD)RepaintFunc );
            DiagHex( "Enable: repaint hi", (WORD)((DWORD)RepaintFunc >> 16) );
            FLUSH();
        }

        /* 【r78 起: 崩溃地址归属 / r81 升级为"有出处的段"地图】
         * Enable 已经走到最后一行, 崩溃就发生在我们返回之后。此时把
         * 选择子地图整份落盘, 下次蓝屏的 SSSS:OOOOOOOO 就能直接对上:
         *   · DBFN 行(r81 起带完整 sel:off): DIBENG 各入口的远指针。
         *     崩溃段命中其一 → 凶手是 DIBENG, 且能算出"在哪个入口的
         *     哪个偏移"(r78 只打了段字, 偏移丢失, 所以 0427:2B70 至今
         *     没能定性)。
         *   · MOD/SEGM 行(r81 新增): 各模块数据库 + 段表里全部已加载段。
         *   · DUMP 行(r81 新增): 覆盖 DumpOff 的段, 该处 64 字节机器码。
         *     —— 蓝屏只给 CS:EIP, 有了机器码就能直接反汇编出出事的那条
         *     指令, 从而知道它在读哪个字段, 不必再猜模块、猜结构。
         *   · LDT 行: 0x0307..0x06C7 区间所有存在的描述符(只读描述符)。
         * 全部来自公开接口, 无一处版本判断或硬编码地址。             */
#ifdef VBE9X_DIAG
        {
            int k;
            for (k = 0; k < DIBFN_COUNT; ++k)
                DiagLogPtr( g_dibfnTab[k].name, g_dibfn[k] );
            FLUSH();
            DiagLogPtr( "PTR ours Enable",      FARFN(Enable) );
            DiagLogPtr( "PTR ours BeginAccess", FARFN(BeginAccess) );
            DiagLogPtr( "PTR ours EndAccess",   FARFN(EndAccess) );
            DiagLogPtr( "PTR ours ExtTextOut",  FARFN(ExtTextOut) );
            DiagLogPtr( "PTR ours SetPalette",  FARFN(SetPalette) );
            DiagLogPtr( "PTR ours ReEnable",    FARFN(ReEnable) );
            DiagLogPtr( "PTR ours Disable",     FARFN(Disable) );
            FLUSH();
            DiagSegMap();
            FLUSH();

            /* ---------------- r96 新增的三份取证 ----------------
             * ① DIBENG 调色板族的机器码(相对入口取窗口 —— 天然可移植:
             *    段选择子每台机器都不同, 而"入口前 0x40/0x80/0xC0 字节"
             *    在任何机器上都是同一段指令)。
             *    r95 的崩点 0367:3FA5 = (DIB_SetPaletteExt 0367:4020) - 0x7B:
             *    三窗合起来覆盖 [入口-0xC0, 入口-0x40), 正好把它包进去。
             *    下一轮日志里就有那条出错指令的字节, 不必再等用户抄地址。
             * ② g_traceDone 位图 —— 验证汇编侧转发探针到底有没有跑起来。
             * ③ lpDevice 身份自证 —— 它到底是不是 GDI 给的 PDEV 缓冲? */
            {
                static const WORD palSlot[6] = { 23, 24, 25, 26, 31, 36 };
                int ki;

                for (ki = 0; ki < 6; ++ki)
                {
                    WORD ks    = palSlot[ki];
                    WORD kSeg  = (WORD)(g_dibfn[ks] >> 16);
                    WORD kOff  = (WORD)g_dibfn[ks];

                    DiagLogPtr( g_dibfnTab[ks].name, g_dibfn[ks] );
                    if (!kSeg || kOff < 0xC0)
                        continue;
                    DiagDumpWin( kSeg, (WORD)(kOff - 0x40) );
                    DiagDumpWin( kSeg, (WORD)(kOff - 0x80) );
                    DiagDumpWin( kSeg, (WORD)(kOff - 0xC0) );
                }
                FLUSH();

                /* ② 探针位图: 36 个汇编侧槽位谁跑过 */
                {
                    int ti;
                    for (ti = 0; ti < 36; ++ti)
                        g_traceStr[ti] = g_traceDone[ti] ? '1' : '0';
                    g_traceStr[36] = 0;
                    DIAG( "TRACEDONE 0..35(1=汇编转发器已记录过该槽):" );
                    DIAG( g_traceStr );
                    FLUSH();
                }

                /* ③ lpDevice: 选择子 + 描述符 + 它前面 0x40 字节的头两个字。
                 * 【为什么看 -0x40】r89..r95 的 RAW PDEV 行显示这块内存里装
                 * 的是 DIBENG 的【常驻名表】(12 "Windows DIB Engine" 0000
                 * 0B "DIB_DISABLE" 0400 ...)。名表在 NE 文件里紧跟在 0x40
                 * 字节的模块头之后 —— 若 -0x40 处真是 'N','E'(4E 45), 那么
                 * GDI 交给我们的 lpDevice 根本不是它自己分配的 PDEV 缓冲,
                 * 而是 DIBENG 模块头的 +0x40, 我们一直在写别人的户口本。
                 * 读取有闸门: 只在此选择子覆盖 64KB 以上、且基址落在
                 * 0x80000000..0x807FFFFF(已提交的模块/堆区)时才读一个字 —— 
                 * r79 那次 0E 就是盲读惰性堆页造成的, 这类闸门不能省。 */
                {
                    uint32_t pdBase = 0, pdLim = 0;
                    WORD     pdAcc = 0;
                    WORD     pdSel = (WORD)(((DWORD)lpDevice) >> 16);
                    WORD     pdOff = (WORD)((DWORD)lpDevice);

                    DiagHex( "PDEV: sel", pdSel );
                    DiagHex( "PDEV: off", pdOff );
                    if (DescRead( pdSel, &pdBase, &pdLim, &pdAcc ))
                    {
                        uint32_t pdLin = pdBase + (uint32_t)pdOff;

                        DiagHex( "PDEV: desc acc", pdAcc );
                        DiagHex( "PDEV: desc base lo", (WORD)pdBase );
                        DiagHex( "PDEV: desc base hi", (WORD)(pdBase >> 16) );
                        DiagHex( "PDEV: desc lim lo", (WORD)pdLim );
                        DiagHex( "PDEV: desc lim hi", (WORD)(pdLim >> 16) );
                        DiagHex( "PDEV: lin lo", (WORD)pdLin );
                        DiagHex( "PDEV: lin hi", (WORD)(pdLin >> 16) );
                        if (pdLim >= 0x00010000UL &&
                            pdBase >= 0x80000000UL && pdBase < 0x80800000UL)
                        {
                            BYTE __far *pb =
                                (BYTE __far *)MK_FP( pdSel, (WORD)(pdOff - 0x40) );
                            DiagHex( "PDEV: minus40 w0", *(WORD __far *)pb );
                            DiagHex( "PDEV: minus40 w1",
                                     *(WORD __far *)(pb + 2) );
                        }
                        else
                            DIAG( "PDEV: -0x40 probe skipped (descriptor gate)" );
                    }
                    else
                        DIAG( "PDEV: descriptor unreadable" );
                    FLUSH();
                }
            }

            /* 【r83】蓝屏现场。① 用户若配了 Crash=/CrashOff=(十进制), 就把
             * 那处的机器码抄下来; ② 再把【同一偏移】在我们自己代码段与
             * DIBENG 各入口所在段里也抄一份作横向对比(同段自动去重)。
             * 判据很简单: 若该偏移在那些段里是正常指令序列, 而崩溃段不是
             * 它们中的任何一个, 那就是"选择子被写坏", 而不是它们真出事。
             * ③ 与 Crash= 无关的自动通道见 drv.c 的 DiagLdtCodeDump:
             * 它把 LDT 里所有代码段在 DumpOff 处的机器码整片抄下来。 */
            if (g_runCrashSel)
            {
                int k;
                DiagHex( "CRASH: cfg sel", g_runCrashSel );
                DiagHex( "CRASH: cfg off", g_runCrashOff );
                DiagCrashSite( (WORD)g_runCrashSel, (WORD)g_runCrashOff );
                DiagCrashSite( (WORD)(FARFN(Enable) >> 16), (WORD)g_runCrashOff );
                for (k = 0; k < DIBFN_COUNT; ++k)
                    DiagCrashSite( (WORD)(g_dibfn[k] >> 16), (WORD)g_runCrashOff );
                FLUSH();
            }
        }
#endif

        wEnabled = 1;
        DIAG( "Enable: PDEVICE done" );
        FLUSH();
        /* 【r107】开机基线探针: 此刻 4F02 已成功、屏已清、VDD 已注册 ⇒
         * 这是"硬件确实在我们的模式里"的已知良好时刻(见 HwBaseline 长注)。
         * ./106 的日志里【缺的正是这一枪】, 于是会话收尾处那四次 AL=0x72
         * 既可能是异常也可能是本机方言 —— 有了基线, 下一次就是读数。 */
        HwBaseline();
        /* 【r113: 整组寄存器采样已从这里搬走】r112 把 VxdHwRegSave() 放在
         * 这一拍(开机相, 与 HwBaseline 同一时刻, 见当时的注)。搬走的理由
         * 有两条, 都写在 HwRegSampleAtRebuild 的头注里:
         *   · 语义: 这里只是"我们【认为】已经进模式了", 而真正的已知良好
         *     时刻是"4F02 之后 post-verify 通过"那一拍;
         *   · 风险: 这批端口访问(25+5+9 次索引写 + 读)是 r112 新引入的开机
         *     期硬件活动, 而 ./112 的 t1 五次开机崩溃(0D @ 3A43:…/394e:…/
         *     冻结)与它同批出现。官方契约不要求驱动在开机相读写这些寄存器,
         *     去掉它, 开机路径的端口足迹就与 r110 逐字节相同。
         * 采样点现在在 MaybeDeferredRestore 的成功重建之后(HwReg>=1)。 */
        /* 【r107】VM 句柄对表: 分派记录里的 EBX 是"发起调用的 VM", 这里
         * 取到的是"当前 VM"(我们的调用 = Windows VM)。两者若同值, 则
         * HiresAns=2(只答 Windows VM)就是一个可用的 A/B 开关; 若不同值,
         * 那条路径就是死的, 本轮直接排除, 不再浪费一轮。
         * 【只在诊断构建里问】发布版不该为一次纯取证多付一次 VxD 往返。 */
#ifdef VBE9X_DIAG
        {
            uint32_t vmh = VBE_GetVMHandle();

            DiagHex( "VMH cur lo", (WORD)vmh );
            DiagHex( "VMH cur hi", (WORD)(vmh >> 16) );
        }
#endif
        FLUSH();
        return 1;
    }
    else
    {
        /* ==================== GDIINFO 查询阶段 ==================== */
        LPGDIINFO lpInfo = lpDevice;
        _fmemset( lpInfo, 0, sizeof( GDIINFO ) );

        /* 先交给 DIB 引擎: 它会填 dpCurves..dpStyleLen 并给出
           dpDEVICEsize 的基础值                                    */
        DIB_Enable( lpDevice, style, lpDeviceType, lpOutputFile, lpStuff );

        lpInfo->dpVersion    = DRV_VERSION;
        lpInfo->dpTechnology = DT_RASDISPLAY;
        lpInfo->dpHorzSize   = DISPLAY_HORZ_MM;
        lpInfo->dpVertSize   = DISPLAY_VERT_MM;
        lpInfo->dpPlanes     = 1;
        lpInfo->dpNumFonts   = 0;

        lpInfo->dpHorzRes = wScrX;
        lpInfo->dpVertRes = wScrY;

        lpInfo->dpMLoWin.xcoord = DISPLAY_HORZ_MM * 10;
        lpInfo->dpMLoWin.ycoord = DISPLAY_VERT_MM * 10;
        lpInfo->dpMLoVpt.xcoord = wScrX;
        lpInfo->dpMLoVpt.ycoord = -wScrY;

        lpInfo->dpMHiWin.xcoord = DISPLAY_HORZ_MM * 100;
        lpInfo->dpMHiWin.ycoord = DISPLAY_VERT_MM * 100;
        lpInfo->dpMHiVpt.xcoord = wScrX;
        lpInfo->dpMHiVpt.ycoord = -wScrY;

        /* 以下比例换算沿用 DDK 示例的经验值, 不影响正确性 */
        lpInfo->dpELoWin.xcoord = DISPLAY_SIZE_EN;
        lpInfo->dpELoWin.ycoord = DISPLAY_SIZE_EN;
        lpInfo->dpELoVpt.xcoord = wScrX;
        lpInfo->dpELoVpt.ycoord = -lpInfo->dpELoVpt.xcoord;

        lpInfo->dpEHiWin.xcoord = DISPLAY_SIZE_EN * 5;
        lpInfo->dpEHiWin.ycoord = DISPLAY_SIZE_EN * 5;
        lpInfo->dpEHiVpt.xcoord = -wScrX / 2;
        lpInfo->dpEHiVpt.ycoord = -lpInfo->dpEHiVpt.xcoord;

        lpInfo->dpTwpWin.xcoord = DISPLAY_SIZE_TWP;
        lpInfo->dpTwpWin.ycoord = DISPLAY_SIZE_TWP;
        lpInfo->dpTwpVpt.xcoord = -wScrX / 2;
        lpInfo->dpTwpVpt.ycoord = -lpInfo->dpTwpVpt.xcoord;

        lpInfo->dpLogPixelsX = wDpi;
        lpInfo->dpLogPixelsY = wDpi;
        lpInfo->dpBitsPixel  = (wBpp + 7) & 0xFFF8;
        lpInfo->dpDCManage   = DC_IgnoreDFNP;

        /* 能力位: 彩标 | 支持 ReEnable | 字节压缩字形 | 字形索引。
           故意【不】设 C1_GAMMA_RAMP(无 gamma 硬件通道)与
           C1_SLOW_CARD(软渲染在现代宿主上足够快, 设了反而禁用
           一些视觉效果)。                                            */
        lpInfo->dpCaps1 |= C1_COLORCURSOR | C1_REINIT_ABLE |
                           C1_BYTE_PACKED | C1_GLYPH_INDEX;
        lpInfo->dpText  |= TC_CP_STROKE | TC_RA_ABLE;

        /* 记下 DIB 引擎要求的基础 PDEV 大小, 供 PDEVICE 阶段使用。
         * 【r76c 双保险】dpDEVICEsize 是 GDI 给本驱动 PDEV 缓冲区分配的
         * 字节数; 我们往这块缓冲里放两样东西:
         *     PDEV 起始        = DIBENGINE(48 字节)
         *     PDEV+wDIBPdevSize = BITMAPINFO(后面可能跟 8bpp 调色板)
         * 只要"我们写的量" > "上报给 GDI 的量", 就是踩 GDI 的堆 ——
         * 症状恰好是"Enable 之后在 GDI 内部 0D 蓝屏"(r76b 的
         * 0427:00002B70 就落在 GDI 的段里)。所以这里做两件事:
         *   ① 基础大小至少 sizeof(DIBENGINE): 否则 BITMAPINFO 会被写在
         *      DIBENGINE 头上, 结构自毁;
         *   ② 最后把上报的 dpDEVICEsize 抬到"基础+追加"的实际需求
         *      (只增不减: DIB_Enable 若要求得更多, 保持它的值)。      */
        wDIBPdevSize = lpInfo->dpDEVICEsize;
        if (wDIBPdevSize < sizeof( DIBENGINE ))
            wDIBPdevSize = sizeof( DIBENGINE );

        lpInfo->dpNumBrushes = -1;  /* 太多, 不数了(DDK 惯例) */

        if (wBpp == 8)
        {
            if (wPalettized)
            {
                lpInfo->dpNumPens     = 16;
                lpInfo->dpNumColors   = 20;
                lpInfo->dpNumPalReg   = 256;
                lpInfo->dpPalReserved = 20;
                lpInfo->dpColorRes    = 18;
                lpInfo->dpRaster     |= RC_DIBTODEV + RC_PALETTE +
                                        RC_SAVEBITMAP;
            }
            else
            {
                lpInfo->dpNumPens     = 256;
                lpInfo->dpNumColors   = 256;
                lpInfo->dpNumPalReg   = 0;
                lpInfo->dpPalReserved = 0;
                lpInfo->dpColorRes    = 0;
                lpInfo->dpRaster     |= RC_DIBTODEV;
            }
            lpInfo->dpDEVICEsize += sizeof( BITMAPINFOHEADER ) + 256 * 4;
        }
        else    /* 16/24/32bpp */
        {
            lpInfo->dpNumPens     = -1;
            lpInfo->dpNumColors   = -1;
            lpInfo->dpNumPalReg   = 0;
            lpInfo->dpPalReserved = 0;
            lpInfo->dpColorRes    = 0;
            lpInfo->dpRaster     |= RC_DIBTODEV;
            lpInfo->dpDEVICEsize += sizeof( BITMAPINFOHEADER );
        }
        {
            DWORD wNeed = (DWORD)wDIBPdevSize + sizeof( BITMAPINFOHEADER );
            if (wBpp == 8)
                wNeed += 256UL * 4UL;
            if ((DWORD)lpInfo->dpDEVICEsize < wNeed)
                lpInfo->dpDEVICEsize = (WORD)wNeed;
        }

        /* 【r76c/r76d 取证】把交给 GDI 的 GDIINFO 关键字段记下来: 若
         * dpDEVICEsize 异常(0 或过小), GDI 会分配过小的 PDEV 缓冲区,
         * 而我们往 "PDEV + dpDEVICEsize" 处写 BITMAPINFO / 往 PDEV
         * 头部写 DIBENGINE 就是踩 GDI 的堆 —— 症状恰好是"Enable 之后
         * 在 GDI/DIBENG 内部 0D"。这一步就是为排除该嫌疑而加的。   */
        DiagHex( "GDIINFO: dpDEVICEsize", lpInfo->dpDEVICEsize );
        DiagHex( "GDIINFO: dpBitsPixel", lpInfo->dpBitsPixel );
        DiagHex( "GDIINFO: dpRaster", (WORD)lpInfo->dpRaster );
        DiagHex( "GDIINFO: dpHorzRes", lpInfo->dpHorzRes );
        DiagHex( "GDIINFO: dpVertRes", lpInfo->dpVertRes );
        DiagHex( "GDIINFO: dpTechnology", lpInfo->dpTechnology );
        DiagHex( "GDIINFO: dpPlanes", lpInfo->dpPlanes );
        DiagHex( "GDIINFO: dpVersion", lpInfo->dpVersion );
        DiagHex( "GDIINFO: dpNumPens", (WORD)lpInfo->dpNumPens );
        DiagHex( "GDIINFO: dpNumColors", (WORD)lpInfo->dpNumColors );
        DiagHex( "GDIINFO: dpNumBrushes", (WORD)lpInfo->dpNumBrushes );
        DiagHex( "GDIINFO: dpCaps1 lo", (WORD)lpInfo->dpCaps1 );
        DiagHex( "GDIINFO: dpCaps1 hi", (WORD)((DWORD)lpInfo->dpCaps1 >> 16) );   /* r406: 消除 W135 */
        DiagHex( "GDIINFO: dpText", (WORD)lpInfo->dpText );
        DiagHex( "GDIINFO: dpLogPixelsX", lpInfo->dpLogPixelsX );
        DiagHex( "GDIINFO: dpDCManage", lpInfo->dpDCManage );
        FLUSH();

        return sizeof( GDIINFO );
    }
}

/*
 * ReEnable —— 动态切换分辨率(显示属性→应用)。
 *   【为何包装 BeginAccess/EndAccess(CURSOREXCLUDE)】切模式期间帧缓冲
 *   基址/几何会变化, 必须先把光标排除在外, 避免光标引擎写野指针。
 *   【与 vmdisp9x 的差异(修正)】失败回滚时 vmdisp9x 只恢复 wScreenX/Y
 *   而没恢复 wScrX/wScrY(配置值), 导致回滚重设的仍是失败的新模式;
 *   这里保存并恢复的是配置值, 回滚才真正回到旧模式。
 *   注: Win9x 不会运行时改位深(MS KB Q127139), 只改分辨率。
 */
UINT WINAPI __loadds ReEnable( LPVOID lpDevice, LPGDIINFO lpInfo )
{
    WORD wLastScrX = wScrX;
    WORD wLastScrY = wScrY;
    WORD wLastBpp  = wBpp;
    WORD rc;

    TraceOnce( TRACE_SLOT_REENABLE, "DDI first call: ReEnable(31)" );

    ReadDisplayConfig();        /* 读入新的配置 */
    ReadRunProfile();           /* r81: 刷新 system.ini [omninine](改键后无需重编) */
    bReEnabling = 1;

    DIB_BeginAccess( lpDevice, 0, 0, wScreenX - 1, wScreenY - 1,
                     CURSOREXCLUDE );
    rc = Enable( lpDevice, 0, NULL, NULL, NULL );
    DIB_EndAccess( lpDevice, CURSOREXCLUDE );

    if (rc)
    {
        Enable( lpInfo, 1, NULL, NULL, NULL );  /* 刷新 GDIINFO */
        rc = 1;
    }
    else
    {
        /* 回滚到旧模式并强制重绘 */
        wScrX = wLastScrX;
        wScrY = wLastScrY;
        wBpp  = wLastBpp;
        Enable( lpDevice, 0, NULL, NULL, NULL );
        if (RepaintFunc)
            RepaintFunc();
        rc = 0;
    }

    bReEnabling = 0;
    return rc;
}

/*
 * Disable —— 卸载: 恢复 I/O 陷阱 → 注销 VDD 注册 → 回 80x25 文本模式。
 *   【r76: 为何不释放帧缓冲选择子】Disable 之后 GDI 与本驱动都还会
 *   有机会再走一次 Enable(切到标准 VGA 再切回来), 而 DPMI 0.9 没有
 *   "解除物理映射"的功能 —— 每次重建都会把上一次的映射(数 MB 线性
 *   地址空间)永久泄漏。选择子是会话级资源, 与在售参考驱动 boxv9x
 *   一致: 建一次用到关机, 需要时用 LfbSelector_Release 兜底释放。
 *   注销动作(VDD_DRIVER_UNREGISTER → mini-VDD 的 DISPLAY_DRIVER_
 *   DISABLING)已把 VxD 的共享状态作废, 不会留下过期几何参数。
 */
UINT WINAPI __loadds Disable( LPVOID lpDevice )
{
    LPDIBENGINE lpEng = lpDevice;

    TraceOnce( TRACE_SLOT_DISABLE, "DDI first call: Disable(4)  <-- GDI 主动卸载!" );
    /* 【r99: 逐步 flush 日志】r98 这里只有一行入口日志, 于是"Disable 到底
     * 跑完了没有"完全判不出来(E11 的关机路径因此一直定性不了 —— 实际
     * 那次是 ACPI 正常断电)。每一步前后各一行, 死机点会精确停在某一行。
     * 【为何不在这里做延迟重建】Disable 的语义就是"别再碰硬件了", 重建
     * 无意义; wEnabled=0 之后 MaybeDeferredRestore 自己会放弃。        */
    DIAG( "Disable: enter" );
    /* 【r131 无条件通道】这里用 wScrX(用户选定/已生效的桌面宽度)当值 ——
     *   "Disable 那一刻我们认为屏有多宽"是 release 档最需要的一格。
     *   g_cbSeen 在这一行【还没声明】(它在文件后部的回调台账区), 编译期
     *   直接报 E1011 —— 记录在此, 免得下次又写一遍。 */
    PHASEV( "PH: disable X=", wScrX );
    FLUSH();
    VxdTraceDump();                     /* 把 VxD 侧最后的调用历史捞出来 */
    /* 【r105】会话收尾: 把光标排斥临界区的记账落盘(这是本次会话最后一次
     * 能安全写文件的机会)。见 XclDump / g_runAccessLog。              */
    XclDump();
    DIAG( "Disable: dump done" );
    FLUSH();

    wEnabled = 0;
    lpEng->deFlags |= BUSY;     /* 阻止后续绘制进入 */

    /* 先恢复 I/O 陷阱(VDD 重新接管端口虚拟化), 再动硬件 */
    DIAG( "Disable: step StartIOTrap pre" );
    FLUSH();
    VDD_StartIOTrap();
    DIAG( "Disable: step StartIOTrap post" );
    FLUSH();
    DIAG( "Disable: step PhysicalDisable pre" );
    FLUSH();
    PhysicalDisable();
    DIAG( "Disable: step PhysicalDisable post" );
    FLUSH();

    return 1;
}

/*
 * ValidateMode —— GDI/SETUP 查询某模式是否可用(序号称 700)。
 *   可能在本驱动不是当前驱动时被调用, 因此只查缓存表不碰硬件。
 */
UINT WINAPI __loadds ValidateMode( DISPVALMODE FAR *lpValMode )
{
    TraceOnce( TRACE_SLOT_VALIDATE, "DDI first call: ValidateMode(700)" );
    if (g_nModes == 0)
        return VALMODE_NO_WRONGDRV; /* 无 VBE 2.0 LFB 模式: 不是我们的卡 */

    if (FindVbeMode( (WORD)lpValMode->dvmXRes, (WORD)lpValMode->dvmYRes,
                     (WORD)lpValMode->dvmBpp ) != 0xFFFF)
        return VALMODE_YES;

    return VALMODE_NO_NOMEM;    /* 模式不存在(对调用方等同"资源不足") */
}

/*==========================================================================*/
#pragma code_seg( "_TEXT" )
/*  以下函数在任意时机都可能被调用(VDD 回调、USER 回调、DIBENG 回调、
 *  GDI 绘图路径), 常驻 _TEXT 段(不加锁; 参考驱动 vmdisp9x 同样不锁
 *  16 位代码段, 见 DrvInitCore 注释)。                                */
/*==========================================================================*/

/*
 * TraceSlot —— 汇编转发器(DIBFWD/DIBTHK)首次被调用时调进来(r76c 诊断)。
 *   C 原型: void __far __pascal TraceSlot( WORD slot )  → 符号 TRACESLOT
 *   调用约定: vddapi16.asm 里 push 一个槽号后 call, 被调方 ret 2 清参。
 *   槽号 0..35 与 g_dibfnTab[] 一一对应(0=DIB_BitBlt, 2=DIB_Control,
 *   3=DIB_EnumDFonts, 14=DIB_ExtTextOut, 19=DIB_Inquire, ...);
 *   36 以上是 C 侧调用点(见文件头 TRACE_SLOT_* 定义)。
 *   每个槽只写一次日志, 因此不会刷屏、也不影响绘图性能。
 *   进入本函数时 DS 已由转发器设为本模块 DGROUP。
 */
void __far __pascal TraceSlot( WORD slot )
{
#ifdef VBE9X_DIAG
    /* 【r98 修正: 老代码在这里把每一行都吃掉了】旧版本第一句是
     *     if (slot >= DIBFN_COUNT || g_traceDone[slot]) return;
     *  可汇编转发器是【先置位 g_traceDone[slot]=1 再 call 进来的】
     *  (vddapi16.asm 的 TRACESLOTMAC), 于是条件恒真、直接 return ——
     *  r76c 起"每槽首次调用落一行"的设计从来没生效过: r87..r97 的日志里
     *  一条 "DDI first call ->" 都没有(只有 g_traceDone 的位图能证明转发器
     *  被进过)。现在只挡非法槽号; "每槽只落一次"由转发器那边保证。
     *  r98-marker: TraceSlot no longer eats its own first-call trace */
    if (slot >= DIBFN_COUNT)
        return;
    DIAG( "DDI first call ->" );       /* r128-hot- per-DDI, stays DIAG */
    DIAG( g_dibfnTab[slot].name );      /* 形如 DIB_BitBlt */
    DiagHex( "DDI slot", slot );
    /* 【r95 新增: 调用方是谁 + 我们要跳到哪 + 装不装 DS】
     *  汇编跳板此刻已把调用方现场存进 DGROUP 的 LIFO 区(见 vddapi16.asm
     *  的 DIBSAVE), 于是这里能打出"调用方 DS"与"调用方返回地址" ——
     *  蓝屏地址里出现别的模块段号时, 直接对上: 那就是"谁在调我们"。 */
    DiagHex( "DDI target seg", (WORD)(g_dibfn[slot] >> 16) );
    DiagHex( "DDI target off", (WORD)g_dibfn[slot] );
    DiagHex( "DDI wDibDgroup", wDibDgroup );
    DiagHex( "DDI dg trusted", g_dibDgTrusted );
    /* 【r98 新增】插进参数帧的尾参到底是什么(见 g_thunkPOff 的注释)。
     *  与下面的 "DDI ours dg" 一比即可判定 DS 病灶是否还在。 */
    DiagHex( "DDI thunk param off", g_thunkPOff );
    DiagHex( "DDI thunk param seg", g_thunkPSeg );
    DiagHex( "DDI ours dg", FP_SEG((const void __far *)&g_dibfn[0]) );
    if (g_dibSaveIdx > 0)
    {
        WORD k = (WORD)((g_dibSaveIdx - 1) % DIBFWD_SAVE_MAX);
        DiagHex( "DDI caller DS",      g_dibSaveDS [k] );
        DiagHex( "DDI caller ret seg", g_dibSaveSeg[k] );
        DiagHex( "DDI caller ret off", g_dibSaveOff[k] );
    }
    /* 【r107】把这一条 DDI 首调钉在 tick 时间轴上: "崩溃前最后一条 DDI 是
     * 在开机后第几拍"从此是读数而不是推测 —— ./104 的 .bsod 尾部正是
     * "DDI first call -> DIB_DibToDevice", 而它离崩溃到底多远, 此前无从判断。*/
    DiagMark( g_dibfnTab[slot].name );
    /* 【r107】把用户配的蓝屏段号在【每一个 DDI 首调】上再探一次。
     *   为什么必须挂在这里: 你确认了那次 0D 是"**无任何操作、刚进桌面就崩**",
     *   而那一刻【一个状态边沿都还没发生】—— Enable 那一枪与状态边沿那一枪
     *   都够不着它。./104 的 .bsod 尾部正是 "DDI first call -> DIB_DibToDevice",
     *   也就是说**最后一次 DDI 首调就是崩溃现场**, 把 SELPROBE 挂在这一行
     *   之前, 才有可能在"崩溃前的最后一秒"问出 `3A43` 到底是不是一个真实的
     *   GDT 段、基址/界限是多少。
     *   只在 Crash=<段号> 非 0(默认 0)时执行 ⇒ 常规运行零开销、零风险。
     *   r107-marker: selprobe at every DDI first call */
    if (g_runCrashSel)
        DiagSelProbe( (uint16_t)g_runCrashSel );
    /* r94-marker: TraceSlot must FLUSH (buffered lines were lost at the crash) */
    /* 【r94 补】必须落盘。r87..r93 的日志里一条转发器探针都没有, 一度被
     * 读成"GDI 从没调用任何 DDI"; 实际上本函数只 DIAG 不 FLUSH —— 日志
     * 是缓冲的, 而"第一次经跳板进 DIBENG"恰恰就是崩溃现场, 那一行永远
     * 留在了内存缓冲里没写出去。加上 FLUSH 之后, 崩溃前最后一条
     * "DDI first call -> DIB_xxx" 就是最直接的第一现场。
     * 每槽只写一次, 对绘图性能无影响。 */
    FLUSH();
#else
    (void)slot;
#endif
}

/*
 * 【r113】TraceSlotRet —— 汇编返回桩(DIBSTUB)在每个 DDI 【从 DIBENG 返回】
 *   时调进来一次(每槽只进一次, 开关在汇编侧: 读 g_retDone[slot])。
 *   C 原型: void __far __pascal TraceSlotRet( WORD slot ) → 符号 TRACESLOTRET
 *
 *   【它回答的问题】./104 起每一轮的崩溃日志都以同一形态收尾:
 *     `DDI first call ->` / `DIB_DibToDevice` / `MARK DIB_DibToDevice …`
 *   —— 也就是说我们只知道"进了引擎", 不知道引擎有没有回来。入口标记在
 *   跳进 DIBENG 【之前】落盘, 所以"最后一行"这个事实有两种完全不同的
 *   解释(死在引擎里 / 死在引擎返回之后), 而此前没有任何数据能区分。
 *   有了本函数, 下一次崩溃日志的最后两行就足以定案。
 *
 *   【为什么可以在这里做】DIBSTUB 本来就是"被调方返回后的落点"(r97 起
 *   所有转发器都走 call 语义 + 自己的返回桩), DS 已由它钉回本模块 DGROUP;
 *   汇编侧把 DX:AX(被调方返回值)与 ES 保护起来再 call, 本函数按 Watcom
 *   的 16 位 ABI 只需遵守"不动 SI/DI/BP", 栈上不多留一个字。
 *   【代价】每槽一次, 之后只剩汇编侧一条 cmp/jne。
 */
void __far __pascal TraceSlotRet( WORD slot )
{
#ifdef VBE9X_DIAG
    if (slot >= DIBFN_COUNT)
        return;
    DIAG( "DDI ret ->" );
    DIAG( g_dibfnTab[slot].name );
    DiagHex( "DDI ret slot", slot );
    /* 与入口侧同理: 缓冲里的行在崩溃时等于没写 —— 必须落盘。 */
    FLUSH();
#else
    (void)slot;
#endif
}

/*
 * TraceOnce —— C 侧调用点的一次性追踪(与 TraceSlot 共用 g_traceDone)。
 */
static void TraceOnce( WORD slot, const char *msg )
{
#ifdef VBE9X_DIAG
    if (slot >= TRACE_ARRAY_SIZE || g_traceDone[slot])
        return;
    g_traceDone[slot] = 1;
    DIAG( msg );
    /* r95-marker: TraceOnce must FLUSH too (C-side DDI probes died in the buffer)
     * —— 与 TraceSlot 同一个坑: r91..r94 的 C 侧入口探针
     * (SetPalette/ExtTextOut/光标三件套/ReEnable/Disable/BeginAccess…)只 DIAG
     * 不 FLUSH, 而"GDI 在 Enable 之后第一次调进来的那个 DDI"恰恰就是崩溃
     * 现场, 那一行永远留在内存缓冲里 —— 于是日志看着像"什么都没调用就崩了"。
     * 每槽只写一次, 对性能无影响。 */
    FLUSH();
#else
    (void)slot;
    (void)msg;
#endif
}

/*
 * RestoreDesktopMode —— 主 VDD 的回调: DOS 全屏会话切回桌面时调用。
 *   由 ③ 环注册的 VDD_DRIVER_REGISTER(0x80) 登记。__loadds 必不可少:
 *   VDD 按 Windows 回调约定在 AX 中传入我们的 DGROUP。
 *   【为何是"不黑屏/不花屏"的关键】DOS 全屏程序会把显卡拉到 VGA/VESA
 *   文本或图形模式, 退出(ALT-TAB/ALT-ENTER 切回)时由主 VDD 调本函数:
 *   重设桌面模式、重刷调色板、清 BUSY、清屏、请求重绘。任何一步漏掉
 *   就是黑屏或花屏。
 *   【重绘】本驱动不钩 INT 2F(屏幕切换仲裁完全交给主 VDD), 因此没有
 *   SCREEN_SWITCH_IN 通知 —— 恢复模式后由我们自己请求重绘。
 *   【r82 新增: 验证 + 一次重试】恢复"调用成功"不等于"显卡真的回来了"
 *   (真机 DOS 会话后 BIOS 只管应答)。这里用两道判据核对(见
 *   VerifyDisplayMode), 不一致就重设一次; 仍不一致则把结果写进日志 ——
 *   把"用户看到黑屏而我们毫无线索"变成"日志里写着哪一步没生效"。
 *   重试只做一次: 不做循环, 免得在坏卡上把启动拖住。
 */
/* r99: 推迟恢复/自愈共用的"待重建模式"标志。置位者: RestoreDesktopMode
 * (Restore=2) 或 MaybeHeal(Heal=1); 执行者: MaybeDeferredRestore, 在下一次
 * ring-3 入口跑。这样"发起"与"执行"分处两个上下文 —— 正是要验证的假设。
 * 【r104】第三个置位者: 功能收尾边沿(g_runVgaEdge) —— 因为 ./103 证明
 * 那个回调在多数会话里根本不会被调用。                                */
static BOOL bRestorePending = 0;
/* 【r104】本次重建是否必须真的重设模式(即不许走"验证通过就跳过"的捷径)。
 * 由 VgaEdge 路径置位: 会话收尾时硬件的真实状态我们判不出来(见下方
 * MaybeDeferredRestore 里的说明), 只能实打实再设一次。               */
static BOOL bForceRebuild = 0;
/* 【r111】官方路径(回调里直接恢复)的账本与待办重绘。
 *   bCbRepaint: 回调里已经成功重建过一次模式, 只欠"请求 USER 重绘"这一
 *   步 —— 那一步不能在 VDD 的回调里做, 由下一个 DDI 入口补(见
 *   ApplyDesktopRestore 的注)。计数三个: 回调进来几次 / 内联成功几次 /
 *   内联失败几次(失败必然跟一条回落重建或一条 ReArm)。              */
static BOOL   bCbRepaint = 0;
static WORD   g_cbSeen   = 0;
static WORD   g_cbOk     = 0;
static WORD   g_cbFail   = 0;
/* 【r112】官方路径台账的补充三项 + 打印去重键。
 *   wCbLedgerN: 上一次打台账时 g_cbSeen 的值 —— 只要回调又来了一次就打,
 *     不再依赖"内联成功"(见 MaybeDeferredRestore 里的 r112 更正注)。
 *   g_cbNested: 回调切进"我们自己的重建序列"里被挡回去的次数(应打
 *     CB evt=0Fxx)。它是 ./111 t5 那条 `rebuild refused (in VDD callback)`
 *     现象的正面计数器 —— 有它才说明这道新加的判断真的在工作。
 *   g_cbHwRegLast: 最近一次寄存器回放的读回差异数(0xFF = 没有样本)。   */
static WORD   g_cbNested  = 0;
static WORD   g_cbHwRegLast = 0xFF;

/* 【r100】r98/r99 的老路径整体改名保留 —— 只在 Restore=3 对照下使用。
 * 为什么保留: 它是"回调里直接重建模式"的原始实现, 而 r100 的护栏会让它
 * 的那次 4F02 被拒绝(CB evt=0002), 正好用来证明护栏真的拦得住。      */

/*==========================================================================
 * 【r100】光标的保存与重建(DDK 对 ResetHiResMode 的六项义务之一)
 *
 * DDK 原文: "It must reinitialize and reenable the hardware cursor. Because
 * the SetCursor function will not be called in response to returning from a
 * full-screen VM, the display driver should save the complete lpCursorShape
 * data structure in its data segment every time SetCursor is called."
 * r98/r99 完全没做这一条 ⇒ 全屏 DOS 会话回来后光标可能丢失/残留。
 * 【尺寸怎么来】CURSORSHAPE 前 6 个字 = 热点(x,y)、尺寸(cx,cy)、
 * cbWidth(每行字节)、Planes/BitsPixel; 位图紧随结构之后, 单色光标是
 * AND/XOR 两个掩码 ⇒ 字节数 = cbWidth * cy * 2。取不到合理值时干脆不存
 * (宁可不重建, 也绝不越界读别人的内存)。
 *========================================================================*/
#define CURSHAPE_HDR     12                 /* 6 个 WORD               */
#define CURSHAPE_MAXBMP  1024               /* 上限: 32x32x1bpp x2     */
static uint8_t s_curShape[CURSHAPE_HDR + CURSHAPE_MAXBMP];
static WORD    s_curShapeLen = 0;
static CURSORSHAPE __far *lpLastCursor = 0; /* 最近一次 SetCursor 的指针 */

static void SaveCursorShape( void )
{
    WORD __far *pw;
    WORD  cx, cy, cbw;
    DWORD len;

    /* 【r105】CursorSrc=1(默认) 时形状早在 SetCursor_driver 里就拷好了,
     * 这里【绝不】再碰 lpLastCursor —— 那个指针到此刻可能已经悬空。    */
    if (g_runCursorSrc)
        return;
    if (lpLastCursor == 0)
        return;
    pw  = (WORD __far *)lpLastCursor;
    cx  = pw[2];
    cy  = pw[3];
    cbw = pw[4];
    if (cx == 0 || cx > 64 || cy == 0 || cy > 64 || cbw == 0 || cbw > 16)
    {
        s_curShapeLen = 0;                  /* 形状不合理: 不存 */
        return;
    }
    len = (DWORD)CURSHAPE_HDR + (DWORD)cbw * cy * 2;
    if (len > sizeof(s_curShape))
        len = sizeof(s_curShape);
    _fmemcpy( s_curShape, (void __far *)lpLastCursor, (size_t)len );
    s_curShapeLen = (WORD)len;
    DiagHex( "CursorShape: saved len", s_curShapeLen );
}

/* 【r105】CaptureCursorShape —— 在 SetCursor(DDI 102) 调用【当场】把形状
 *   拷进 s_curShape。此刻调用方给的那本 CURSORSHAPE 一定有效, 拷下来的
 *   就是真形状; r100..r104 把它推迟到主 VDD 回调里做, 那时指针可能已经
 *   悬空 —— 或者头部不合理被静默丢弃(./103 四份日志里一条
 *   `CursorShape: saved len` 都没有, 说明"重建光标"这条 DDK 义务其实
 *   从来没真正做到过)。
 *   校验沿用老代码的口径(宁可不重建, 也绝不越界读别人的内存)。       */
static void CaptureCursorShape( CURSORSHAPE __far *lpCursor )
{
    WORD __far *pw;
    WORD  cx, cy, cbw;
    DWORD len;

    if (lpCursor == 0)
        return;
    pw  = (WORD __far *)lpCursor;
    cx  = pw[2];
    cy  = pw[3];
    cbw = pw[4];
    if (cx == 0 || cx > 64 || cy == 0 || cy > 64 || cbw == 0 || cbw > 16)
    {
        s_curShapeLen = 0;
        return;
    }
    len = (DWORD)CURSHAPE_HDR + (DWORD)cbw * cy * 2;
    if (len > sizeof(s_curShape))
        len = sizeof(s_curShape);
    _fmemcpy( s_curShape, (void __far *)lpCursor, (size_t)len );
    s_curShapeLen = (WORD)len;
}

static void RestoreCursorShape( void )
{
    if (s_curShapeLen == 0 || lpDriverPDevice == 0)
        return;
    DIAG( "CursorShape: rebuilding hardware cursor" );
    DIB_SetCursorExt( (void __far *)s_curShape, lpDriverPDevice );
}

/*==========================================================================
 * 【r130】ForceModeReprogram —— 强制 BIOS 在【回程】真的把模式寄存器写一遍。
 *
 * 【为什么需要"强制"】主 VDD / drv.c 在"回 Windows 高分辨率"那一刻本来就
 *   会发一次 4F02h(见 r129 之前所有轮次的日志: 每次会话收尾都有
 *   `SetDisplayMode: vbeMode=0117` → `VBE_SetMode: exec ax=004F`), 可屏幕
 *   依旧收缩。最符合全部读数的解释是: BIOS 的模式缓存里【当前模式已经等于
 *   请求的模式】, 于是它认为"没什么要做的"而跳过整个重编程 —— 4F02h 返回
 *   成功(004Fh), 硬件却停在文本模式下那一套水平时序上。
 *   这与 r122 的实测完全一致: INT 10h AX=0003 连发五遍"成功"而屏幕不变 ——
 *   同一个"缓存说不用做"的机制。
 *   所以这里【先切到另一个合法模式再切回来】, 让"请求的模式 ≠ 缓存的模式"
 *   在回程那一刻成立, 从而逼它真的写一遍。
 *
 * 【为什么只发标准 VBE 调用, 一个寄存器都不自己写】
 *   自己写 CRTC 水平时序 = 把某张卡/某个 BIOS 的具体时序焊进通用驱动, 违反
 *   本项目的通用性铁律; 而且 CR00..CR06 与 CR17 的溢出位彼此耦合, 写一半
 *   比不写更危险。让 BIOS 自己写, 才是"官方恢复路径"。
 *
 * 【中间模式怎么选】优先用【同一张卡的另一个 VBE 模式】—— 它一定存在、一定
 *   是这个 BIOS 认得的、而且不会把显示通路切到我们控制不了的形态。候选顺序
 *   刻意从"最普通"开始: 640x480x8(0112h) → 800x600x8(0103h) →
 *   640x480x16(0111h)。全都要带 bit14(线性帧缓冲), 因为中间态也要让 BIOS
 *   走线性通路 —— 纯打包模式(0101h)会把链 4/平面打开, 那是另一个坑。
 *   VBE 模式号本身是【标准】的(VBE 1.2 定的固定号), 不是厂商私有。
 *
 * 【失败了会怎样】三次候选都不成功 ⇒ 返回 0, 调用方照旧往下走(清屏/重绘),
 *   等于回到 r129 的行为 —— 不会把情况弄得更糟; 而中间的每一次成功切换都会
 *   被后续那次 4F02h 覆盖, 不会留下"停在 640x480"这种半成品状态。
 *
 * 【回程为什么调用 SetDisplayMode(0) 而不是自己再发一次 4F02h】
 *   SetDisplayMode(0) 是【本项目唯一被反复验证过的"回到桌面模式"入口】:
 *   它做 4F02h(目标模式) + 4F01h(刷新几何/pitch) + 帧缓冲选择子确认 +
 *   PushState(把几何交给 mini-VDD) + LfbSelRecheck。自己再发一次裸 4F02h
 *   会把"选择子与几何的再确认"漏掉 —— 而重设模式之后恰恰需要它。
 *   代价只是多一次 4F02h(从中间模式回目标模式那一次), 而且那一次正好是
 *   "BIOS 必须真的重编程"的那一次, 不是冗余。
 *
 * 【返回值】1 = 至少一次"切出去再切回来"完整成功(BIOS 被逼着重编程了);
 *          0 = 没有可用的中间模式 / 中间切换失败(硬件已尽力切回目标模式)。
 *========================================================================*/
static int ForceModeReprogram( void )
{
    /*==========================================================================
     * 【r138 中间模式的选法改了 —— 从"硬编码标准号"改成"问这张卡自己"】
     *
     * 【r130..r137 写的是什么, 错在哪】
     *   旧表: static const WORD alt[3] = { 0x0112, 0x0103, 0x0111 };
     *   注释(上面那三行)写着 "0112h = 640x480x8"。按 VESA 规范, 0112h 是
     *   640x480x【24bpp】(16.8M 色, 8:8:8); 640x480 的 256 色(8bpp)模式是
     *   0101h。原文(VBE 3.0, "VBE Mode Numbers" 表, pp.18-19):
     *     "101h - 640x480 256 | 103h - 800x600 256 | ... |
     *      110h - 640x480 32K (1:5:5:5) | 111h - 640x480 64K (5:6:5) |
     *      112h - 640x480 16.8M (8:8:8) | 113h - 800x600 32K (1:5:5:5) | ..."
     *   旁证三条: VirtualBox/BOCHS 编译进 VBE BIOS 的模式表
     *   (vbetables-gen.c: `{ 640, 480, 8 , 0x101}` … `{ 640, 480, 24 , 0x112}`);
     *   Linux 内核 Documentation/fb/vesafb.txt(16M 那一行是
     *   0x112 0x115 0x118 0x11B); RTEMS vbe3.h(`VBE_R640x480C17M 0x112`)。
     *   也就是说: r130 起每一次"逼 BIOS 重编程"都会先把卡切到【24bpp】再
     *   切回来。
     *   本机读数也指向同一结论: ./137 的模式枚举里 0111h/0112h 都在, 而
     *   0110h(15bpp)不在; 0114h/0115h 在而 0113h 不在; 0117h/0118h 在而
     *   0116h 不在 —— 缺的恰好都是 15bpp 那三个, 而驱动自己按 bpp 匹配选中
     *   的 1024x768x16 正是标准号 0117h。标准编号在这张卡上成立。
     *
     * 【为什么这不是"小笔误"而要认真改】
     *   中间模式的唯一目的是让"BIOS 缓存的当前模式 ≠ 目标模式"成立(r122 的
     *   实测: 缓存相等时 4F02 回报成功却不重编程)。达到这个目的【不需要】
     *   换位深 —— 换分辨率就够了。而切到 24bpp 再切回来, 把卡的像素格式
     *   走了一趟比需要的大得多的变动; 只要回程那一腿被打断(./137 的
     *   3721/3723/3725 三连 FFFF 与 3727 `forced reprogram FAILED` 就是
     *   回程腿失败的实拍), 卡就停在一个【没有任何 CHECK 看得见】的格式上 ——
     *   那正好是"几何全对、SE 全对、AR10h 干净, 画面却是条纹"的形状。
     *
     * 【通用性铁律: 一个硬编码的厂商无关标准号, 仍然是硬编码】
     *   本项目最不愿意做的事就是把某张卡/某个 BIOS 的细节焊进驱动。标准号
     *   看起来"通用", 但它假定了 BIOS 真的按标准表实现。r138 改成【问这张卡
     *   自己】: g_modes[] 里每一项都是本卡经 4F00/4F01 亲口报出来的
     *   (mode/width/height/bpp/pitch 齐备), 从里面挑候选 —— 不需要假定任何
     *   编号, 也天然排除了"这个卡根本没有的模式"。
     *
     * 【候选顺序(两轮, 各最多 3 个)】
     *   第 1 轮: 位深与目标【相同】、分辨率不同 —— 像素格式家族不变, 这是
     *           最小的扰动, 也是最能保证回程一定成功的中间态。
     *   第 2 轮: 8bpp(256 色)。仍然远胜 VGA 16 色, 且与任何高彩目标都不同族,
     *           因此"缓存 ≠ 目标"必然成立。
     *   两轮都挑不出候选(只有 1 个可用模式) ⇒ 返回 0, 调用方照旧走清屏/重绘,
     *   等于回到 r129 的行为, 不会把情况弄得更糟。
     *
     * 【AltFromTbl=0】退回 r130 的硬编码表(单行 A/B, 不用重编)。
     * 【每一枪都落盘】`CrtGeo: forced via alt mode` 后面新增三行
     *   `CrtGeo: alt w/h/bpp` —— 下一轮可以直接看到挑的是哪一个模式、什么
     *   位深, 而不是靠猜 0112h 到底是什么。
     *========================================================================*/
    static const WORD altLegacy[3] = { 0x0112, 0x0103, 0x0111 };
    WORD wantMode;
    int  i, k, pass;

    wantMode = FindVbeMode( wScrX, wScrY, wBpp );
    if (wantMode == 0xFFFF)
        return 0;

    for (pass = 0; pass < 2; pass++)
    {
        int triedThisPass = 0;

        for (i = 0; i < g_nModes; i++)
        {
            WORD m = g_modes[i].mode;

            if (m == wantMode)
                continue;                   /* 中间模式不得等于目标模式 */
            if (pass == 0)
            {
                /* 位深相同、分辨率不同 —— 最小扰动 */
                if (g_modes[i].bpp != wBpp)
                    continue;
                if (g_modes[i].width == wScrX && g_modes[i].height == wScrY)
                    continue;
            }
            else
            {
                /* 8bpp 家族 —— 与高彩目标必然不同族 */
                if (g_modes[i].bpp != 8)
                    continue;
            }
            if (triedThisPass >= 3)
                break;                      /* 每一轮最多三枪 */

            /* 先到中间模式: 这一步只需 BIOS 接受(AX=004Fh), 不看回读 ——
             * 它的唯一目的就是让"模式缓存 ≠ 目标模式"成立。          */
            ProtClrBeforeModeSet( "ForceModeReprogram" );          /* r132 */
            triedThisPass++;                /* 记账的是"试了几枪", 不是"成功几次" */
            if (!VBE_SetMode( (uint16_t)(0x4000 | m) ))
                continue;
            DiagHex( "CrtGeo: forced via alt mode", m );
            DiagHex( "CrtGeo: alt w",  g_modes[i].width );    /* r138 */
            DiagHex( "CrtGeo: alt h",  g_modes[i].height );   /* r138 */
            DiagHex( "CrtGeo: alt bpp", g_modes[i].bpp );     /* r138 */
            PHASEV( "PH: altbpp=", g_modes[i].bpp );          /* r138: 无条件 */
            /* 再切回目标模式 —— 用项目里唯一被反复验证过的那条入口。 */
            if (SetDisplayMode( 0 ))
            {
                DiagHex( "CrtGeo: back to target mode", wantMode );
                return 1;
            }
            /* 回程失败: 再试下一个候选(下一次循环会先切出去, 正好再给一次
             * 机会)。 两轮都不行就认输, 交给调用方照旧流程走。        */
            DiagHex( "CrtGeo: return to target FAILED, try next alt", m );
        }
        if (!g_runAltFromTbl)
            break;                          /* 对照档: 只走硬编码表 */
    }

    /* 【AltFromTbl=0 的对照路径】r130 的老表, 一字不改 —— 保留它是为了
     *   "新选法到底有没有差别"能被单行 INI 切开, 而不是靠回忆。 */
    if (!g_runAltFromTbl)
    {
        for (k = 0; k < 3; k++)
        {
            if (altLegacy[k] == wantMode)
                continue;
            ProtClrBeforeModeSet( "ForceModeReprogram" );
            if (!VBE_SetMode( (uint16_t)(0x4000 | altLegacy[k]) ))
                continue;
            DiagHex( "CrtGeo: forced via alt mode", altLegacy[k] );
            if (SetDisplayMode( 0 ))
            {
                DiagHex( "CrtGeo: back to target mode", wantMode );
                return 1;
            }
            DiagHex( "CrtGeo: return to target FAILED, try next alt", altLegacy[k] );
        }
    }
    return 0;
}

/*==========================================================================
 * 【r130】CrtGeometryEnsure —— "回到 Windows 之前, 确认扫描输出一行真的
 *   输出 wScrX 个像素; 不是就强制 BIOS 重走一遍模式设置"。
 *
 *   调用点只有一个: ApplyDesktopRestore 的【清屏/重绘之前】(见那里的长注)。
 *   放在那里有两个理由:
 *     ① 顺序: 必须"先把栅格弄对, 再清屏/重绘" —— 反过来的话画面已经被
 *        按错的栅格写过一遍(mode co80 之所以有效正是因为它把这个顺序做对了);
 *     ② 上下文: 那里是普通 ring 3 的 DDI 入口或推迟重建, VBE 调用安全;
 *        绝不在主 VDD 的回调里做嵌套 BIOS(r99 三次死机的形状)。
 *
 *   【闸门有三道, 任何一道都不放行就一个字节都不碰】
 *     ① g_runCrtGeo(system.ini [omninine] CrtGeo, 默认 1);
 *     ② 不在主 VDD 回调里(g_inCbRestore == 0);
 *     ③ CrtGeometryLooksRight() 明确返回 0(算出来的一行像素数 ≠ 桌面宽度)。
 *       返回 1(自洽)或 2(读不到)一律不动手 —— 所以正常机器上这一枪不响。
 *
 *   【修完再量一遍】ForceModeReprogram 之后重新判定一次, 结果落盘。下一轮
 *   只要看 `CrtGeo: before/after verdict` 两行就能知道这一枪【够不够】:
 *     before=0000 after=0001 ⇒ 机制成立, 收缩是"BIOS 跳过重编程"造成的;
 *     before=0000 after=0000 ⇒ 机制不成立, 得换方向(读数里已经排除掉的
 *                               那些寄存器之外的东西)。
 *   这两行是 r130 的预登记判据, 不许事后改口径。
 *
 *   【返回值】1 = 本来就好(没动硬件); 0 = 本来不好且重设后好了;
 *            -1 = 本来不好、重设后仍不好; -2 = 读不到几何(不知道);
 *            -3 = 读不到/策略关(完全没动手)。日志把它原样打出来。
 *========================================================================*/
static int CrtGeometryEnsure( void )
{
    int before;
    int after;

    if (!g_runCrtGeo)
    {
        DiagHexMin( "CrtGeo: skipped (policy off)", 0 );
        FLUSH();
        return -3;
    }
    if (g_inCbRestore)
    {
        /* 回调里绝不发嵌套 BIOS —— 留给下一个 DDI 入口那次。 */
        DiagHexMin( "CrtGeo: deferred (in VDD callback)", 0 );
        FLUSH();
        return -3;
    }

    before = CrtGeometryLooksRight( (uint16_t)wScrX );
    DiagHexMin( "CrtGeo: before verdict (1=ok,0=MISMATCH,2=unknown)",
                (WORD)((before < 0) ? 0xEEEE : before) );
    PHASEV( "PH: crtgeo before=", (before < 0) ? 0xEEEE : before );
    DiagHexMin( "CrtGeo: before width (pixels per scan line)", (WORD)CrtHWidth );
    DiagHexMin( "CrtGeo: before CR01 (horizontal display end)", (WORD)CrtHDe );
    DiagHexMin( "CrtGeo: before CR00 (horizontal total)", (WORD)CrtHTo );
    FLUSH();

    if (before != 0)
    {
        /* 【r138】自洽时不欠债: 把有界重挂的预算清零。 */
        if (before == 1)
            s_crtGeoReArm = 0;
        return (before == 1) ? 1 : -2;      /* 自洽 或 读不到: 不动手 */
    }

    if (!ForceModeReprogram())
    {
        DiagHexMin( "CrtGeo: forced reprogram FAILED", 0 );
        FLUSH();
        /*==========================================================================
         * 【r138】有界重挂 —— 这一次失败【不是猜出来的】
         *
         * 【为什么值得重试】
         *   走到这里的前提是 before == 0: CrtGeometryLooksRight() 用
         *   (CR01+1) × 每字符点数【算出来】扫描一行不是 wScrX 个像素。几何
         *   确实不对, 不需要任何推断。
         *   而 ./137 的实测(3716..3744)把"修不动"的原因指了出来: 三连
         *     VBE_SetMode: exec ax=FFFF   (3721/3723/3725)
         *     CrtGeo: forced reprogram FAILED=0000   (3727)
         *   紧接着 90 行之后, 同一条调用在【另一个上下文】里成功了:
         *     VBE_SetMode: exec ax=004F   (3743)  ->  forced via alt mode=0112
         *   内存环同时记下了 7 条 `CB evt=0Dxx`(CB_EVT_BIOS_BUSY) —— 也就是
         *   说那三次失败是【我们自己没让调用出去】(直通闩还闩着 / 在回调
         *   窗口里), 不是硬件说不。时机问题 ⇒ 换一个上下文再试是有意义的。
         *
         * 【为什么必须有界 —— 本项目吃过空转重挂的亏】
         *   drv.c 里 ./111 t5 的记录就是"4F01 被护栏拒 ⇒ 空转重挂 8 次"。
         *   所以这里给一个写死的预算(6), 到顶就彻底放弃, 并落一行明确的
         *   `re-arm exhausted` —— 判读时"没修好"与"没试够"必须是两件事。
         *   预算只在【几何确实不对】时消耗, 几何一好就清零, 不会跨会话累积。
         *========================================================================*/
        if (s_crtGeoReArm < CRT_GEO_REARM_MAX)
        {
            s_crtGeoReArm++;
            bRestorePending = 1;
            bForceRebuild   = 1;
            DiagHexMin( "CrtGeo: re-arm (bounded, 1..6)", s_crtGeoReArm );
            PHASEV( "PH: crtgeo rearm=", s_crtGeoReArm );
        }
        else
        {
            DIAGMIN( "CrtGeo: re-arm exhausted (6) - giving up this session" );
            PHASEV( "PH: crtgeo rearm=", 0xFFFF );
        }
        FLUSH();
        return -1;
    }

    /* 【帧缓冲选择子与几何的再确认已经在 ForceModeReprogram 内部的
     *   SetDisplayMode(0) 里做过(4F01 刷新 + LfbSelector_Ensure +
     *   PushState + LfbSelRecheck)—— 见该函数头注, 这里不再重复一次端口
     *   访问。原来那一版这里写了一次 LfbSelRecheck(), 但本文件里根本没有
     *   这个函数(真名是 drv.c 的 LfbSelRecheck 挂在 SetDisplayMode 尾部),
     *   直接调会让 wcc 报未定义符号 —— 记录在此, 免得下次又写一遍。    */

    after = CrtGeometryLooksRight( (uint16_t)wScrX );
    DiagHexMin( "CrtGeo: after verdict (1=ok,0=still wrong,2=unknown)",
                (WORD)((after < 0) ? 0xEEEE : after) );
    PHASEV( "PH: crtgeo after=", (after < 0) ? 0xEEEE : after );
    DiagHexMin( "CrtGeo: after width (pixels per scan line)", (WORD)CrtHWidth );
    DiagHexMin( "CrtGeo: after CR01 (horizontal display end)", (WORD)CrtHDe );
    FLUSH();

    /* 【r138】修好了就把有界重挂的预算清零(欠债还清)。 */
    if (after == 1)
        s_crtGeoReArm = 0;

    return (after == 1) ? 0 : -1;
}

/*==========================================================================
 * 【r131】CrtSyncEnsure —— "回到 Windows 之前, 确认 CRTC 17h 的同步使能位
 *   (SE, bit7)确实是 1; 读回 0 就补成 1, 并读回自证"。
 *
 *   【为什么是这一位 —— 全部来自 ./130 的实测, 不是推断】
 *     ① 主机截图 1_host.png(320x771)与虚拟机内截图 1_guest.png(1024x768)
 *        做逐像素取证: 整幅桌面(六个图标 + 整条任务栏, 含任务栏上 14 段
 *        前景)都在, 纵向 1:1(青底都是第 0..739 行), 而横向被压缩进 320
 *        列。用任务栏前景段做最小二乘拟合:
 *            host_x = 2.516 * fb_x + 1.0   ⇒ 可见帧缓冲列 = 320/2.516 ≈ 127
 *     ② 同一轮日志里 `HwReg: geo CR01=007F / CR00=00A3 / dots=0008 /
 *        width=0400 / verdict=0001`, `SR01=0001`, `GCR06=0005`,
 *        `ATC10=0041` —— **全部正确**, 所以 r130 那套几何自检报"ok"。
 *     ③ 127 ≈ 128 = CR01 + 1。也就是说: 硬件把 CR01 当成了【像素数】而不是
 *        【字符数】—— 正好是"点时钟乘法器没被使能"的样子。
 *     ⇒ r129 补的三个位(SR01 bit0 / GCR06 bit0 / AR10 bit0)与 r130 查的
 *       CR00/CR01【都不是】病灶; 唯一没被任何一行代码看过的是 CR17h bit7。
 *
 *   【标准依据】FreeVGA《CRT Controller Registers》Index 17h(CRTC Mode
 *     Control Register)bit7:
 *       "SE -- Sync Enable: when set to 0, this bit disables the horizontal
 *        and vertical retrace signals and forces them to an inactive level.
 *        When set to 1, this bit enables the horizontal and vertical retrace
 *        signals."
 *     任何我们期望有画面的模式, SE 必须是 1。它既不在 CR00..CR18 的几何
 *     字段里(所以 r130 的几何自检看不见), 也不在 r129 那三个"文本/图形"
 *     位里 —— 这就是"长条"能一路活下来的原因。
 *
 *   【为什么会读到 0】本卡是 3dfx Voodoo3(VBE 3.0 / LFB / 16MB)。它的
 *     扩展(VBE LFB 高彩)模式里真正驱动行场同步的是 Voodoo 自己的时序
 *     发生器, VGA CRTC 的同步发生器不参与输出 —— BIOS 把 SE 留在 0 完全
 *     可能, 而且对真机无副作用。可是在任何"以 VGA CRTC 为准"的实现里
 *     (86box 的 SVGA 核心即如此: hdisp = crtc[1](奇数再 +1), 再乘
 *     dots_per_clock, 而乘法那一段的门槛正是 `!scrblank && (crtc[0x17] &
 *     0x80)`), SE = 0 会把行宽算成 CR01+1 = 128 ⇒ 桌面只剩最左 128 列。
 *     ⇒ 补成 1: 对"以 CRTC 为准"的实现是【唯一正确的值】, 对"以芯片自身
 *     时序为准"的实现是【无关位】。两条路都不亏。而且只动一个 bit ——
 *     与 文本态修补 / HiResClockBits 同一条纪律: **绝不碰任何时序/几何**。
 *
 *   【闸门三道, 任何一道不放行就一个字节都不写】
 *     ① g_runCrtEn(system.ini [omninine] CrtEn, 默认 1);
 *     ② 不在主 VDD 回调里(g_inCbRestore == 0)—— 理由同 CrtGeometryEnsure;
 *     ③ **读回 SE = 0 才写**: 硬件自己回答"该不该写", 比任何推断都准。
 *   正常机器上这一枪不响(零写入), 所以它不会改变任何正常路径的硬件足迹。
 *
 *   【返回值】1 = 本来就是 1(零写入); 0 = 补回并读回确认; -1 = 写了但读不回
 *            1; -2 = 读不到 CR17h; -3 = 策略关/在回调里。
 *   【与 r130 的关系】这是 r130 那套"几何自检"的【补充】而不是替代: r130
 *     管 CR00/CR01(字符数意义上的行宽), r131 管"这一行到底按不按字符算"。
 *     两条都过, 一行才真的输出 wScrX 个像素。
 *   r131-marker: CrtSyncEnsure (CR17h bit7 Sync Enable).
 *========================================================================*/
static int CrtSyncEnsure( void )
{
    WORD v;
    WORD sr;
    int  r;

    if (!g_runCrtEn)
    {
        DiagHexMin( "CrtSync: skipped (policy off)", 0 );
        FLUSH();
        return -3;
    }
    if (g_inCbRestore)
    {
        /* 回调里不做端口写(RestoreDesktopMode 的纪律), 留给下一个 DDI 入口。 */
        DiagHexMin( "CrtSync: deferred (in VDD callback)", 0 );
        FLUSH();
        return -3;
    }

    v  = VxdCrtMode17();
    sr = VxdSeqClocking();
    DiagHexMin( "CrtSync: CR17 raw (bit7=SE,bit6=Word/Byte,bit5=AW)", v );
    DiagHexMin( "CrtSync: SE state (1=SET,0=CLEAR,FFFF=unreadable)",
                (WORD)((v == 0xFFFF) ? 0xFFFF : ((v & 0x80) ? 1 : 0)) );
    DiagHexMin( "CrtSync: SR01 raw (bit5=screen off,bit0=8dot)", sr );
    /* 【r131 无条件通道】这两行是 release 档唯一能读到 CR17h/SE 的地方:
     *   `PH: crtsync CR17=` 给出 17h 原值(SE 是 bit7), `PH: crtsync SE=`
     *   给出 1/0/FFFF 的判定。判读时先看这两行 —— 如果 release 与 debug
     *   在这一行上不同, "行为不一致"当场就有了一个可比较的量。         */
    PHASEV( "PH: crtsync CR17=", v );
    PHASEV( "PH: crtsync SE=",
            (v == 0xFFFF) ? 0xFFFF : ((v & 0x80) ? 1 : 0) );
    PHASEV( "PH: crtsync SR01=", sr );
    FLUSH();

    if (v == 0xFFFF)
        return -2;                      /* 端口读不到: 不猜, 不动手 */

    if (v & 0x80)
        return 1;                       /* 正常路径: 零写入 */

    r = VxdCrtSyncFix();                /* 内部再读一次, 只补 bit7, 写完读回 */
    DiagHexMin( "CrtSync: fix result (0=already ok,1=fixed,FFFF=failed)",
                (WORD)((r == 1) ? 1 : ((r == 0) ? 0 : 0xFFFF)) );
    DiagHexMin( "CrtSync: after CR17 raw", VxdCrtMode17() );
    PHASEV( "PH: crtsync fix=", (r == 1) ? 1 : ((r == 0) ? 0 : 0xFFFF) );
    PHASEV( "PH: crtsync CR17 after=", VxdCrtMode17() );
    FLUSH();
    return (r == 1) ? 0 : -1;
}

/*==========================================================================
 * 【r111】ApplyDesktopRestore —— DDK 给 ResetHiResMode 定的六项义务, 只有
 *   这【一份】实现, 两条路径共用:
 *     · 官方路径: 主 VDD 回调进来时直接跑(见 RestoreDesktopMode);
 *     · 回落层  : 官方路径没来/失败时, 在下一个安全 ring-3 上下文跑。
 *   共用一份实现是刻意的 —— 这样"两条路径的差别"只剩【时机与上下文】,
 *   排查时不必怀疑"是不是搬代码时漏了一步"。
 *
 *   六项义务(原文见 DOSBOX_SWITCHING_REFERENCE.md §2.2):
 *     ① 把硬件设回 Windows 的运行模式   → 见下面 how 的三种取值
 *     ② 重建硬件光标                   → RestoreCursorShape(形状由 DDI 102 存下)
 *     ③ 重建 Windows 调色板            → 8bpp 时 DAC_SetPalette
 *     ④ 清 BUSY 位                     → deFlags &= ~BUSY
 *     ⑤ 再调一次 VDD_DRIVER_REGISTER    → g_runReSet
 *     ⑥ 调 VDD_SAVE_DRIVER_STATE        → VddCall(0x82)
 *   外加"清屏", 以及(只在普通 DDI 上下文里才允许的)请求 USER 重绘。
 *
 *   【r112】how —— 义务①怎么做(这是本轮唯一改动的地方):
 *     0 = 不动模式(回落层已经自己按指纹/验证决定过要不要发 4F02);
 *     1 = 【官方路径】回放开机存下的整组模式寄存器(drv.c 的 VxdHwRegRestore:
 *         纯端口写 + 读回比对, 不含任何 BIOS/V86 调用);
 *     2 = SetDisplayMode(0)(= 嵌套 VBE 4F02, 只有 CbBios=1 的对照档才走)。
 *   返回 1 = 义务全部做完(且模式恢复那一步成功); 0 = 模式恢复失败。
 * 【为什么重绘要单独一个参数】RepaintFunc 是 USER 的回调入口, 从主 VDD 的
 *   切屏事务内部去调它 = 在别人的事务里回调 USER, 风险与 r99 的"回调里
 *   嵌套 BIOS"同一类。所以官方路径只置 bCbRepaint, 由下一个 DDI 入口补。
 *========================================================================*/
static int ApplyDesktopRestore( int how, int bRepaint )
{

    int ok = 1;
    int duty = g_inCbRestore ? (int)g_runCbDuty : 3;   /* r112: 见 g_runCbDuty 注 */

    if (how == 1)
    {
        /* 【r112 官方路径】寄存器回放。返回值 = 写回后与"我们真正写进去的值"
         * 的差异字节数(见 drv.c 的 VxdHwRegDiffVsWritten)。
         * 【r134 起这个数字只是证据, 不再是判据】判据改用语义(见下);
         * r112..r133 用它当判据时, 官方路径在 ./133 里 5/6 次被判失败。
         * 【r113】<0 = 还没有样本(HwReg=0, 或开机后还没发生过一次成功重建)。
         * 【r138 改口 —— 这一条正是 r137 的致命处】
         *   r113..r137 的语义是"d<0 不算失败: 回放做不到就不做, 由回落层
         *   的 4F02 承担"。这句话在 r137 里【不成立】了, 因为 r137 同时把
         *   入口边沿那次 4F02 也取消了 ⇒ 两条路同时不作为, 而那一次切换里
         *   关于"把硬件设回 Windows 模式"这件事【什么都没发生】, 却被记成
         *   成功(`CbRestore: inline ok=0002 / inline failed=0000`, 而
         *   ./137 的屏幕一直是条纹)。
         *   DDK 的义务是明确的: "The display driver runs through all the code
         *   necessary to reestablish Windows hi-res mode on the hardware."
         *   (Win95 DDK, PRE_HIRES_TO_VGA/POST_HIRES_TO_VGA, 切回 Windows 第 5 步)
         *   ⇒ r138 起, "没有可用样本"就是"这一项义务【没有】完成", ok = 0,
         *     由既有的 r111 机制交给回落层(bRestorePending=1 +
         *     bForceRebuild=1 + CB_EVT_REARM)。这才是"官方路径优先、做不到
         *     就明确交给回落层", 而不是"做不到也报成功"。
         *   CbNoSample=0 退回 r137 语义(单行 A/B, 不用重编)。            */
        int d  = VxdHwRegRestore();
        /* 【r134 本轮的主判据 —— 把"字节计数否决"换成"语义判据"】
         * ./133 的台账直接指出官方路径为什么从来没成功过:
         *     CbRestore: callbacks seen=0006  inline ok=0001  inline failed=0005
         *     CbRestore: hwreg replay diff last=0001
         * 六个回调里五个判失败, 而唯一那个 ok 是"还没有样本可回放"(d<0)。
         * 于是 DDK 要求的六项义务在回调里【一次都没有做完】, 桌面只能等回落
         * 层那次 4F02; 而回落层那一拍又被抑制闸门按住了(见 VhEdge 处的 r134
         * 注)。用户看到的"切回来还是花的/黑屏"就落在这条链上。
         * 为什么一个字节的差异不该否决: 这个计数只回答"有多少字节读回来不
         * 一样", 而"硬件在不在我们的模式里"该由那些决定画面成立与否的位来
         * 回答 —— 几何(CR00/CR01/CR13) / SE(CR17 bit7) / 8 点时钟(SR01 bit0) /
         * 屏开(SR01 bit5) / 图形模式(GCR06 bit0) / 保护位(CR11 bit7)。
         * 所以 r134 起:
         *   · d < 0(无样本)          ⇒ 不做, 由回落层承担(r113 语义, 不变);
         *   · 语义判据成立            ⇒ 官方路径【成功】, 六项义务照做;
         *   · 语义判据不成立          ⇒ 才算失败, 交给回落层。
         * 字节差异与逐项差异【仍然全部落盘】并进内存环(下一轮可点名到具体
         * 寄存器) —— 判据放宽的是"结论", 不是"证据"。
         * HwEss=0 退回 r133 的字节计数判定(单行 A/B, 不用重编)。        */
        int es = g_runHwEss ? VxdHwRegEssentialsOk() : 1;

        DiagHex( "ApplyDesktopRestore: hwreg replay diff", (WORD)((d < 0) ? 0xEEEE : d) );
        DiagHex( "ApplyDesktopRestore: hwreg essentials ok (r134)", (WORD)es );
        if (d < 0)
            DIAG( "ApplyDesktopRestore: hwreg replay skipped (no sample yet)" );
        g_cbHwRegLast = (WORD)((d < 0) ? 0xFF : (d & 0xFF));
        CbRingPut( (uint16_t)(((uint16_t)CB_EVT_CB_HWREG << 8) |
                              (uint8_t)((d < 0) ? 0xFF : (d & 0xFF))) );
        CbRingPut( (uint16_t)(((uint16_t)CB_EVT_CB_ESSENTIALS << 8) |
                              (uint8_t)(es ? 1 : 0)) );
        /* 【r134】逐项点名: 前 4 个不等的寄存器(组<<8|索引, 然后 now/sav)。
         * 这四对进内存环 —— 文本日志会被闸门成片丢掉(./133:
         * `LOG skip Reent=0640`), 内存环会在下一次重建时被 CbRingDump 落盘。*/
        {
            int k;

            for (k = 0; k < 4; k++)
            {
                uint16_t info = VxdHwRegMismatchInfo( k );
                uint16_t now, sav;

                if (info == 0xFFFF || info == 0xFFFE)
                    break;
                now = VxdHwRegMismatchVal( k, 0 );
                sav = VxdHwRegMismatchVal( k, 1 );
                CbRingPut( (uint16_t)(((uint16_t)(CB_EVT_CB_MM_REG + k) << 8) |
                                      (uint8_t)((((info >> 8) & 0x07) << 5) |
                                                (info & 0x1F))) );
                CbRingPut( (uint16_t)(((uint16_t)now << 8) | (uint8_t)sav) );
                DiagHex( "ApplyDesktopRestore: mismatch grp/idx", info );
                DiagHex( "ApplyDesktopRestore: mismatch now", now );
                DiagHex( "ApplyDesktopRestore: mismatch sav", sav );
            }
        }
        if (d < 0)
        {
            /* 【r138】没有可用样本 = 义务①("把硬件设回 Windows 的运行模式")
             *   的两种做法【一种都没做成】: 寄存器回放没有源, 而这里又不会
             *   发 4F02(那正是"官方路径不用嵌套 BIOS"的全部意义)。
             *   所以默认判失败 —— 交给回落层, 而不是把"什么都没做"记成成功。*/
            if (g_runCbNoSample)
            {
                ok = 0;
                DIAG( "ApplyDesktopRestore: no sample -> mode NOT re-established "
                      "(r138: hand to fallback)" );
                PHASEV( "PH: cbnosample=", 1 );
            }
        }
        else if (!es)
        {
            ok = 0;                     /* r134: 判据是语义, 不是字节数 */
        }
    }
    else if (how == 2)
    {
        if (!SetDisplayMode( 0 ))
            ok = 0;
    }
    if (duty >= 2)
    {
        if (wBpp == 8 && lpColorTable)
            DAC_SetPalette( 0, 256, (const uint8_t __far *)lpColorTable );
        if (g_runCursor)
            RestoreCursorShape();           /* DDK 义务 ② */
    }
    if (lpDriverPDevice)
        lpDriverPDevice->deFlags &= ~BUSY;
    if (g_runReSet && duty >= 3)
    {
        /* DDK 义务 ⑤: "should call the VDD VDD_DRIVER_REGISTER function
         * just as was done at the initial PhysicalEnable routine"。     */
    PHASEV( "PH: oblig5 reg=", 1 );   /* [r406] DDK 义务⑤ */
        (void)VddRegister( VDD_DRIVER_REGISTER, wScreenPitchBytes, wScreenY,
                           (void __far *)RestoreDesktopMode );
        DIAG( "ApplyDesktopRestore: re-registered with VDD" );
    }
    if (duty >= 3)
    PHASEV( "PH: oblig6 save=", 1 );  /* [r412] DDK 义务⑥(restore 路径) */
        VddCall( VDD_SAVE_DRIVER_STATE );   /* DDK 义务 ⑥ */
    /* 【r127 时序修复 —— 本轮的针对点】
     * 见 drv.c 的 VxdClockEnsure8Dot 长注与 ./123 的 CRTP 读数:
     *   会话结束、屏幕正花那一刻 SR01 = 0x0002(9 点字符时钟);
     *   我们的重放(含 SR01)跑过之后仍是 0x0002;
     *   唯独主 VDD 那次 4F02 把它变成 0x0001(8 点) —— 而 4F02 就排在这
     *   两行【之后】。也就是说: 时钟最终是对的, 但"清屏 + 请求重绘"这两步
     *   已经用【错的栅格】做完并提交了 —— 于是清屏没盖住右边多出来的那段,
     *   重绘也是按错的步长落的笔。
     * 所以这里在【清屏/重绘之前】把它归位, 并且读回自证:
     *   0   = 本来就好(一个字节都没写)
     *   1   = 写了并读回确认
     *   -1  = 写了但读回仍未修好(硬件不认 / 端口被虚拟化挡住) —— 此时我们
     *         至少【知道】自己没修好, 而不是像 r123 那样以为修好了。
     * 用独立的 Clk8 开关(默认 1 = **只观察**)是因为它改的是行为:
     *   Clk8=0 完全不碰 / Clk8=1 只读不写(默认) / Clk8=2 确认后才写。
     * 【r128 为什么默认降到"只读不写"】见 g_runClk8 声明处的说明:
     *   R2(Clk8=0)里新 bug 照样出现 ⇒ 不能证明这一枪有害, 但也不能证明它
     *   无害; 而它【会写硬件】。所以先把写权收回来, 只留读数。
     *   要恢复 r127 的行为用 Clk8=2。                                  */
    if (g_runClk8 && duty >= 3)
    {
        /* 【r128 三档】cf: 0=本来就好 1=写了并读回确认 -1=写了仍不对
         *                  -2=只观察(本次没写) */
        int cf = (g_runClk8 >= 2) ? VxdClockEnsure8Dot()
                                  : VxdClockObserve8Dot();

        (void)cf;
        DiagHexMin( "Restore: clock 8dot (0=ok,1=fixed,FFFF=failed,FFFE=observe-only)",
                    (WORD)((cf < -1) ? 0xFFFE : ((cf < 0) ? 0xFFFF : cf)) );
        /* 【r128】把"回放有没有往错的方向修"也记下来: 样本若存成了 9 点,
         * 回放就会把它写回去, 而这一步会把 bit0 归一化成 1。计数不为 0
         * 就说明样本质量问题真实发生过 —— 不再靠推断。            */
        DiagHexMin( "Restore: replay normalized 9dot->8dot count",
                    VxdHwRegNormCount() );
    }
    /* 【r130 本轮唯一的行为改动 —— 直击"窗口 320 宽"】
     * 见 CrtGeometryEnsure 的长注。位置是刻意的: 必须在 ClearVisibleScreen /
     * RepaintScreen 【之前】—— 与 r127 把"时钟归位"提到清屏之前是同一条纪律,
     * 因为"先把栅格弄对, 再按新栅格整幅重画"才是 mode co80 有效的那个顺序。
     * 闸门有三道(策略 / 不在回调里 / 几何确实不一致), 正常机器上不响。 */
    {
        int g = CrtGeometryEnsure();

        DiagHexMin( "Restore: crt geometry (1=ok,0=fixed,FFFF=failed,"
                    "FFFE=unknown,FFFD=skipped)",
                    (WORD)((g == 1) ? 1 :
                           ((g == 0) ? 0 :
                            ((g == -1) ? 0xFFFF :
                             ((g == -2) ? 0xFFFE : 0xFFFD)))) );
        /* 【为什么不拿它改 ok】ok 的语义是"DDK 那六项义务做完了没有", 而
         * 几何重设是【额外】的一层; 把它并进 ok 会让"回调失败 -> 重挂 pending"
         * 这条既有逻辑被一个全新的判据触发, 多出一层我们还没验证过的重试。
         * 所以这里只落盘、不改 ok —— 判据看的是上面那两行 verdict。 */
        (void)g;
    }
    /* 【r131 新的一层 —— 必须在 ClearVisibleScreen / RepaintScreen 之前】
     *   r130 那一层确认的是 CR00/CR01(行宽按字符算出来的像素数); 这一层确认
     *   的是"这一行到底按不按字符算"——CRTC 17h bit7 = SE(Sync Enable)。
     *   ./130 的取证把"长条"锁在 127 ≈ CR01+1 上, 也就是说几何寄存器全对,
     *   而点时钟乘法器没被使能 —— 见 CrtSyncEnsure 的长注。
     *   顺序与上面那条纪律一致: **先确认这一行是 wScrX 个像素, 再清屏重绘**;
     *   反过来的话重绘出来的画面又会被按错的宽度写过一遍。                 */
    {
        int y = CrtSyncEnsure();

        DiagHexMin( "Restore: crt sync enable (1=ok,0=fixed,FFFF=failed,"
                    "FFFE=unknown,FFFD=skipped)",
                    (WORD)((y == 1) ? 1 :
                           ((y == 0) ? 0 :
                            ((y == -1) ? 0xFFFF :
                             ((y == -2) ? 0xFFFE : 0xFFFD)))) );
        (void)y;                        /* 同样不并进 ok, 理由同上 */
    }
    ClearVisibleScreen();
    if (bRepaint)
        RepaintScreen();

    s_inModeSet = 0;                     /* [r406] 恢复结束, 允许落盘 */
    PHASEV( "PH: restore ok=", ok );
    bForceRebuild = 0;
    return ok;
}

/*==========================================================================
 * 【r113】HwRegSampleAtRebuild —— 在"一次成功重建之后"采一份整组模式寄存器,
 *   作为主 VDD 回调里那条回放(VxdHwRegRestore)的源。
 *
 *   【为什么采样点在这里, 而不是 r112 的 Enable 尾部】
 *     ① 语义上这里才是"已知良好": 刚发过 4F02 且 post-verify 通过 ——
 *        Enable 尾部那一刻硬件是不是已经进了我们的模式, 只能靠假设;
 *     ② 风险上这里才安全: 开机相(Enable)的端口访问是 r112 新引入的东西,
 *        而 ./112 的 t1 五次开机崩溃与它同批出现。搬走以后开机路径不再
 *        碰任何 VGA 端口, 硬件足迹与 r110 一致。
 *   【为什么不是每次都采】回放源应当稳定: 只在成功重建之后刷新, 于是
 *   "回放 = 把硬件写回上一次已知良好的样子"这句话在日志里可验证
 *   (HwReg sample ok=1 + diff 读数)。
 *   【打印量】默认一行摘要; 逐寄存器转储(约 39 行)放在 HwReg=2。
 *========================================================================*/
static void HwRegSampleAtRebuild( const char *why )
{
    int s = VxdHwRegSave();
    /* 差值只算一次(它要读 34 个寄存器)。s<=0 时没有样本, 直接记 EEEE。 */
    int d = (s > 0) ? VxdHwRegDiff() : -1;
    /* 【r128】把"样本本身质量"当场打出来。./127 的 r1 是靠
     *   `HwReg diff seq idx=0001 now=0003 sav=0002`
     * 才看出样本存成了 9 点时钟(0x0002) —— 也就是说我们一直在拿一个
     * 【本身就错】的基准当"已知良好"。这两行让下一轮不必再绕这一圈。 */
    WORD samp01 = (s > 0) ? VxdHwRegSaveClocking() : 0xFFFF;
    WORD samp8  = (s > 0) ? VxdHwRegSaveIs8Dot()   : 0xFFFF;
    /* 【r129】同一刻把属性控制器 10h bit0 也读出来(g_runProbe2 门, 默认 1)。
     *   r127 的 CRTP 已经量到它在会话里会变成 0(文本), 而 86Box 的通用 SVGA
     *   渲染器判"文本/图形"用的正是 `!(GCR6 bit0) && !(AR10 bit0)` ——
     *   GCR06 早就在 CRTP 里读, AR10 是另一半。缺了它, "为什么横向缩小"
     *   就只能靠一半判定位。见 drv.c 的 VxdReadAtcReg 长注(内部只写一次
     *   索引、读完立刻还原)。                                              */
    WORD atc10  = g_runProbe2 ? VxdReadAtcReg( 0x10 ) : 0xFFFF;

    (void)why;
    (void)s;                            /* 发布构建里下面几行是空宏 */
    (void)d;
    DIAG( why );
    DiagHexMin( "HwReg: sample ok (1=usable)", (WORD)((s > 0) ? 1 : 0) );
    /* 【r138 无条件通道 —— 本轮最要紧的一行】
     *   ./137 全份日志里 `HwReg: sample ok=0001` 出现 0 次, 而这一行
     *   (DiagHexMin)只在 MIN/DIAG 档里存在: 也就是说 release 档连"有没有
     *   样本"都读不出来。r138 把这件事放到 PHASEV 上 —— 三档都有, 而且
     *   release 档的判读只需要看 `PH: hwreg sample=` 是 0001 还是 0000。
     *   值: 0001 = 采到了可用样本(HTeg 四层防御全部挂在这一位上);
     *       0000 = 没采到(闸门/时序不对, 下一轮要查的是采样点而不是回放)。*/
    PHASEV( "PH: hwreg sample=", (WORD)((s > 0) ? 1 : 0) );
    DiagHex( "HwReg: sample diff now",      (WORD)((d < 0) ? 0xEEEE : d) );
    DiagHex( "HwReg: sample SR01 (bit0: 1=8dot, 0=9dot)", samp01 );
    DiagHex( "HwReg: sample is 8dot",       samp8 );
    /* FFFF = 本次没读(Probe2=0); 其它值 bit0 = 1 表示"图形模式"。 */
    DiagHex( "HwReg: sample atc10 read (FFFF=not read, bit0: 1=graphics)", atc10 );
    DiagHex( "HwReg: replay norm count",    VxdHwRegNormCount() );
    /* 【r138】采样点顺带把"显示起始"读一次(4F07h 取回形态, 只读)。
     *   为什么放在采样点而不是每一次 Verify: 采样点本身就是低频率的安全
     *   上下文(开机相 / 一次成功重建之后), 而且"显示起始"与整组模式寄存器
     *   属于同一份"已知良好的样子" —— 起址被搬走同样能造成整屏错位, 而它
     *   与本轮的主角(像素格式)是两件不同的事, 分开记、分开判。      */
    if (g_runHwChkPitch && VBE_ProbeDisplayStart())
    {
        PHASEV( "PH: hwstart line=",  VBE_ProbeStartLine() );
        PHASEV( "PH: hwstart pixel=", VBE_ProbeStartPixel() );
    }

    /* 【r130】采样点同时把"扫描几何"读出来 —— 这一行是 r130 判据的落点。
     *   读法(四行一组):
     *     HwReg: geo CR01 ...  = CRTC 01h 原值(水平显示结束, 单位=字符)
     *     HwReg: geo CR00 ...  = CRTC 00h 原值(水平总数, 单位=字符)
     *     HwReg: geo dots/char = 8 / 9 / 16(时序器 01h: bit3=1 ⇒ 16;
     *                            否则 bit0=1 ⇒ 8, bit0=0 ⇒ 9 —— r138 修正;
     *                            旧注释与旧代码都只认 8 与 16 两个值)
     *     HwReg: geo width ... = (CR01+1) * 每字符点数 = 【扫描一行多少像素】
     *     HwReg: geo verdict  = 0001 与桌面宽度一致 / 0000 不一致(病灶) /
     *                           0002 读不到
     *   1024 宽的桌面 + 8 点字符 ⇒ CR01 应当是 0x7F、width 应当是 0400。
     *   如果这里读到 0x27/0140(320), 病灶就当场被证实, 不需要再推断。    */
    {
        int geo = CrtGeometryLooksRight( (uint16_t)wScrX );

        DiagHex( "HwReg: geo CR01 (horizontal display end, chars)",
                 (WORD)CrtHDe );
        DiagHex( "HwReg: geo CR00 (horizontal total, chars)", (WORD)CrtHTo );
        DiagHex( "HwReg: geo dots per char (8 or 16)",         (WORD)CrtHDots );
        DiagHex( "HwReg: geo width (pixels per scan line)",    (WORD)CrtHWidth );
        DiagHex( "HwReg: geo verdict (1=matches desktop, 0=MISMATCH)",
                 (WORD)geo );
    }
    /* 【r131】采样点同时把 CRTC 17h 与"保护位"读出来 —— 这是 r131 判据的落点。
     *   三行一组:
     *     HwReg: crt CR17 raw      = CRTC 17h 原值(bit7 = SE 同步使能,
     *                                bit6 = Word/Byte, bit5 = AW, bit3 = DIV2)
     *     HwReg: crt SE state      = 0001 使能 / 0000 关着(病灶) / FFFF 读不到
     *     HwReg: crt protect       = 样本里 CR11h bit7(保护位)
     *   为什么 CR17h 能定案: ./130 的 CR00/CR01/SR01/GCR06/AR10 全是正确值,
     *   而主机截图量出"可见帧缓冲列 = 320/2.516 ≈ 127 ≈ CR01+1" —— 那正是
     *   "点时钟乘法器没被使能"的形状, 而 CR17h bit7(SE)是这条路上唯一没被
     *   任何一行代码看过的一位(FreeVGA Index 17h: "SE -- Sync Enable")。
     *   第三行是 r131 那处寄存器号修正的自证: VxdHwRegRestore 过去把 CR11h
     *   写成 0x11(= CR17h), 所以保护位从来没被清过、也从来没被读过。     */
    {
        WORD c17 = VxdCrtMode17();

        DiagHexMin( "HwReg: crt CR17 raw (bit7=SE,bit6=Word/Byte,bit5=AW)", c17 );
        DiagHexMin( "HwReg: crt SE state (1=SET,0=CLEAR,FFFF=unreadable)",
                    (WORD)((c17 == 0xFFFF) ? 0xFFFF : ((c17 & 0x80) ? 1 : 0)) );
        DiagHexMin( "HwReg: crt protect (CR11h bit7, 0=unprotected)",
                    VxdHwRegProtect() );
        PHASEV( "PH: sample CR17=", c17 );
        PHASEV( "PH: sample SE=",
                (c17 == 0xFFFF) ? 0xFFFF : ((c17 & 0x80) ? 1 : 0) );
        PHASEV( "PH: sample protect11=", VxdHwRegProtect() );
    }
    /* [r133] Re-establish the mode with the display gate closed -> programmed
     *   -> gate opened LAST, and then measure the raster itself.  This runs at
     *   every successful rebuild (the boot one included), i.e. exactly at the
     *   moments where "all registers correct, screen wrong" has been observed.
     *   It is placed after the sample so that the sample is available, and it
     *   refreshes the sample afterwards when it changed the hardware. */
    CrtRelatchAfterRebuild( "Relatch: after known-good rebuild" );
    if (s > 0 && g_runHwReg >= 2)
        (void)VxdHwRegDump( "HwReg: full dump (HwReg=2)" );
    FLUSH();
}

/*==========================================================================
 * RestoreDesktopMode —— 主 VDD 的回调(= 注册给 VDD_DRIVER_REGISTER 的
 * ResetHiResMode)。
 *
 *  【r100..r110 的行为】只做纯内存操作: 记一条 CB 事件、存光标形状、清
 *   BUSY、置 bRestorePending, 真正的重建推迟到下一次 DDI 入口。
 *   它换来了"绝不死在回调里", 但也让【官方路径名存实亡】—— ./110 的
 *   实测: 回调确实被调了(t2/t3 里 CB evt=0101/0102/0103), 可屏幕变好
 *   靠的是那条延迟路径; t3 收尾那次连回调都没来(CB evt=0508 = 等不到
 *   信号走了 8 次兜底)。
 *
 *  【r111 的行为(用户明确要求)】官方路径优先: 回调里【直接】把
 *   ApplyDesktopRestore 跑完(模式/DAC/光标/BUSY/再注册/SAVE_DRIVER_STATE/
 *   清屏), 只把"请求 USER 重绘"留给下一个 DDI 入口。失败或被拒 ⇒ 置
 *   pending, 由回落层接手 —— 回落层就是我们 r104..r110 积累的那一套。
 *
 *  【回调里为什么现在敢做 4F02】两道门, 任何一道不放行就立刻回落:
 *    ① drv.c 的一次性通行证 VxdAllowBios(): r100 的硬护栏是"回调期
 *       一律拒绝"(x230 三次死机换来的), 现在改成"默认拒绝 + 官方路径
 *       显式领一张一次性通行证", 嵌套的第二层照样被拒;
 *    ② mini-VDD 的 VM 自检(vbeshare.inc 的 VBE_API_SET_INCB): 嵌套 V86
 *       执行是"在【当前 VM】里跑一次 INT 10h", 而回调可能被切在别的 VM
 *       上下文里 —— 当前 VM ≠ Windows VM 就回 93 记账并拒绝, 绝不死等。
 *   日志仍然全程禁写(回调期 DOS I/O = r100 那次冻结), 结果只记内存环与
 *   静态计数, 由下一个安全上下文打成 "CbRestore:" 一行。
 *========================================================================*/
/* 【r126 调研结论: DDK 的 FIXED 代码段要求 —— 本工具链做不到, 故不动】
 * DDK 的 VDD_DRIVER_REGISTER 页原文:
 *   "ResetHiResMode is a required display driver entry point which must be
 *    placed in a FIXED code segment in the display driver."
 * 我们注册的 ES:DI 就是本函数(见本文件 PhysicalEnable 里的 VddRegister
 * 调用), 所以这一条适用于它。链接结果里本函数落在 _TEXT(AUTO = 可移动),
 * 严格说【不满足】这一条。
 *
 * 【r126 试了什么, 为什么两条都撤回】
 *   ① 只把本函数挪进自己的段(一对 #pragma code_seg):
 *      实测确实把它单独分了出来(display_TEXT 038e / VBE9XCB 12b / _TEXT
 *      3e69), 但代价是段【顺序】变了(display_TEXT 被排到 _TEXT 之前),
 *      连带改变了链接器对段的合并方式:
 *        debug   76236 -> 76252 (+16)
 *        MIN     38032 -> 38064 (+32)
 *        release 33666 -> 33714 (+48)
 *      注意 release 里【没有任何版本字符串】, 它多出的 48 字节纯粹来自
 *      分段结构的变化 —— 而代码一行都没动。
 *   ② 再把该段标成 FIXED(omninine.lnk 里加一行):
 *      NE 段属性字节仍是 0x0D60(FIXED 位根本没置上), 而段组被拆开 ——
 *      NE 段数 2 → 4, 文件 76236 → 79656(+3420)。与 r118 把 _TEXT 改 fixed
 *      是同一个失败形态。
 *
 * 【所以 r126 的净改动是零】两条路都撤回, 本函数仍在 _TEXT 里, 与 r123 起
 * 的行为逐字节相同。判据是"能证明的收益为零、能证明的代价非零":
 *   · 回调【本来就被调用过】(./110 的 t2/t3 有 CB evt=0101/0102/0103),
 *     所以"回调调不到"不是我们那个故障的成因;
 *   · FIXED 位又置不上 —— 花了 +48 字节和一次段结构改动, 换不到那条要求。
 * 真要满足 FIXED, 剩下的路是【运行期】把该段钉住(GlobalSmartPageLock),
 * 那是行为改动、该单独一轮, 而且要有真机证据说明它确实解决问题才做。
 *========================================================================*/
void __far __loadds RestoreDesktopMode( void )
{
    static WORD wRestoreSeq = 0;
    static WORD wNestedN = 0;            /* r112: 被"自己的重建"挡回去几次 */

    wRestoreSeq++;
    g_cbSeen++;
    VxdSetInCallback( 1 );               /* 【r100】护栏开: 本函数期间
                                          * VbeExecBios 一律拒绝并记内存环
                                          * (【r111】官方路径会领一张一次性
                                          *  通行证, 见下面的 VxdAllowBios) */
    CbRingPut( (uint16_t)(((uint16_t)CB_EVT_CALLBACK_ENTER << 8) |
                          (wRestoreSeq & 0xFF)) );
    SaveCursorShape();
    if (lpDriverPDevice)
        lpDriverPDevice->deFlags &= ~BUSY;   /* DDK 义务: 立刻清 BUSY */

    /* 【r112】第一道判断: 这个回调是不是切进了【我们自己正在跑的那一段
     * 模式重建】中间?
     *   ./111 t5 的硬证据: 4F02 成功之后紧跟着的那次 4F01 被 r100 护栏拒了
     *   (`DeferredRestore: rebuild refused (in VDD callback)=0001`, 并且
     *   4F01 之后没有 `exec4F01 ax=` 那一行) —— 只有"回调在我们自己的序列
     *   中间切进来"能解释这件事。
     *   这时唯一正确的动作是【什么都不做然后立刻返回】: 外层那段序列正在
     *   做同一件事(把桌面恢复回来), 我们再套一层只会互相拆台; 而 DDK 要求
     *   的清 BUSY 上面已经做了。记一条环记录留证, 不置 pending(外层会收尾)。
     *   这也是 drv.c 里 s_inRebuild 放行 BIOS 直通的配套 —— 两边成对。 */
    if (VxdGetRebuild())
    {
        wNestedN++;
        g_cbNested = wNestedN;
        CbRingPut( (uint16_t)(((uint16_t)CB_EVT_CB_NESTED << 8) | (wNestedN & 0xFF)) );
        VxdSetInCallback( 0 );
        return;
    }

    /* 【r111】官方路径: 就在这个回调里把六项义务做完(见
     * ApplyDesktopRestore 与上面的长注)。三个前提缺一不可:
     *   · g_runCbRestore(默认 1, INI 可关 —— 关掉就是 r110 行为);
     *   · wEnabled(驱动已启用; Windows 刚引导时 VDD 也会调这个回调,
     *     那时没有"我们的模式"可恢复, 直接回落即可);
     *     否则就证不了 r100 那道护栏还在)。                          */
    if (g_runCbRestore && wEnabled)
    {
        int ok;

        CbRingPut( (uint16_t)(((uint16_t)CB_EVT_CB_RESTORE << 8) |
                              (wRestoreSeq & 0xFF)) );
        g_inCbRestore = 1;

        if (g_runCbHwSet)
        {
            /* 【r112 默认】官方路径的模式恢复 = 寄存器回放(无 BIOS)。 */
            ok = ApplyDesktopRestore( 1, 0 );
        }
        else
        {
            /* 只做"非模式"的那几项义务; 模式交给回落层。 */
            ok = ApplyDesktopRestore( 0, 0 );
        }
        g_inCbRestore = 0;
        if (ok)
        {
            g_cbOk++;
            bCbRepaint      = 1;         /* 重绘留给下一个 DDI 入口 */
            bRestorePending = 0;
            bForceRebuild   = 0;
        }
        else
        {
            /* 失败(寄存器写不进去 / 硬件拒绝 / 护栏拒绝 / VM 自检拒绝)
             * ⇒ 回落层接手。通行证若没用掉就地作废, 免得留给回调之后的
             * 某次调用。                                              */
            g_cbFail++;
            VxdDenyBios();
            bRestorePending = 1;
            bForceRebuild   = 1;
            CbRingPut( (uint16_t)(((uint16_t)CB_EVT_REARM << 8) |
                                  (wRestoreSeq & 0xFF)) );
        }
    }
    else
    {
        /* r110 行为(CbRestore=0): 只置待办, 真正的重建在下一个安全上下文 */
        bRestorePending = 1;
    }
    VxdSetInCallback( 0 );
}

/*
 * 【r107】HwBaseline —— 取一次"已知良好"的硬件模式读数, 作为后续所有
 *   探针的参考值。
 *   【为什么必须有它】r106 的判据是"AL==0FFh 才算在 VBE 通路", 但 ./106
 *   的实测把这条判据证伪了:
 *     · 那台 BIOS 在【重建成功的 4F02 之后】仍旧回 AL=0x72(AH=0x80),
 *       而同一刻 FbTest 头/尾全过、用户看到桌面确实回来了;
 *     · 也就是说 AL 到底回什么, 取决于 BIOS 自己的方言(0F00h 在 VBE 模式
 *       下回 0FFh 是 VESA 的规定, 但这台机器不吃这一套)。
 *   ⇒ 绝对判据(AL==0FFh)在本机无效; 有效的只有【差分判据】: 把开机后
 *   刚设好模式、桌面已起来的这一刻读到的值记下来, 之后的探针只回答
 *   "和这一枪比, 变没变"。变了 = 硬件真的离开了我们的模式(病灶坐实);
 *   没变 = 探针看不出差别(那花屏就另有原因, 不能拿它当重建依据)。
 *   【调用点】display.c 的 Enable PDEVICE 相末尾(wEnabled=1 之后、返回前):
 *   那一刻 4F02/ClearVisibleScreen/VDD 注册都已完成, 是"已知良好"的
 *   最干净时刻; 而 SetDisplayMode 内部那次探针因为 wEnabled/wBpp 还没
 *   置位(守卫)永远打不出来 —— 这正是 ./106 日志里【没有开机基线】的原因。
 */
/* 【r109·已删除】MaybePortProbe(已于 r407 删除) —— 以下仅存史。
 *   DOS 会话最多两三条边沿, 每次读 14 个端口 = 14 行日志; 上限只防"边沿
 *   风暴"把日志刷爆(真出现那种情况本身就是线索)。
 *   放在这里(HwBaseline 之前)是因为三个调用点全在这之后。           */

/*==========================================================================
 * 【r126·已删除】CrtProbeNow(已于 r407 删除) —— "此刻屏幕在扫描什么"的现场读数(仅存史)。
 *
 * 【为什么这一轮要加它 —— r126 把问题逼到了二选一】
 *   r126 的 SetModeText=1 实测(./122): 五枪 `INT 10h AX=0003` 全部执行
 *   (`TRC st i10 ax=0003` 在日志里出现过), 屏幕依旧花屏, 且与 r116 默认
 *   档"几乎没有区别"。于是剩下两种可能, 必须用硬件读数分开:
 *     A. 显示通路(CRTC 光栅几何/起始地址/偏移)真的停在错误的值上
 *        ⇒ 病灶在硬件寄存器, 属于我们驱动能自己修的范围;
 *     B. 显示通路的值本来就对(与我们存下的已知良好样本逐字节相同),
 *        而屏幕还是花的 ⇒ 病灶在"VDD 把谁的画面送到 DAC"这一层,
 *        寄存器回放这类手段先天无效。
 *   两种情况的处置完全不同, 而此前所有读数(HwProbe 的 0F00h / 我们自己的
 *   VerifyDisplayMode / HwReg 的 diff)都【不能】区分它们: 0F00h 问的是主
 *   VDD 的虚拟 BIOS(它的记账), Verify 读的是自己的软件状态与显存回读,
 *   而 HwReg diff 只在回放【刚做完】那一刻等于 0 —— 那正是我们自己写进去的。
 *
 * 【读什么】与 drv.c 的 PortProbeRead 同一套 8 个 CRTC 寄存器(只读, 无
 *   副作用: 不碰 3DAh/3C0h/3C7h 这些会动翻转触发器或 DAC 索引的端口),
 *   直接用 VxdHwRegOk()/VxdHwRegDiff() 拿"当前是否等于已知良好样本"。
 *   一次只写【一行】, 形如:
 *     CRTP 07 win=1 same=1 start=0000 off=0080 vde=00DF cr17=00E3 sr1=00x
 *   其中: win = 调用点编号(07 见下), same = HwReg diff 是否为 0,
 *         start = CR0C/0D 拼出的显示起始地址(双字单位由 cr17 bit6 决定),
 *         off = CR13, vde = CR12, cr17 = CR17, sr1 = SR01, sr4 = SR04。
 *   数值全为 0xFFFF  = 端口没被真的读回来(VDD 仍在虚拟化 / 未放行);
 *   数值与我们存下的样本一致 = 硬件确实在我们的模式里(=> 情况 B)。
 *
 * 【调用点与编号(gate)】一律受 PortProbe 管辖(默认 0 —— 于是 r126 在
 *   默认配置下的日志开销与 r126 【逐行相同】, 用户已有的"与默认 r116 对比"
 *   这条基线不会被仪器本身扰乱: ./109 t6 与 ./110 两次实测过"开机相多写
 *   几十行就能改变结果")。要看读数就把 [omninine] PortProbe=1 打开。
 *     01 会话收尾边沿(EndHeal 判据之后、重建之前——屏幕此刻还是花的)
 *     02 重建前(Restore 路径已自带 PORT probe)
 *     03 重建后
 *     04 周期性(HwChk=2, 每 64 次 DDI 问一次——"花屏发生在哪一刻"的现场)
 *     05 回调路径 ApplyDesktopRestore 之后
 *     06 开机基线之后
 *     07 DOS 会话进行中首次给端口放行之后(即 HwChk=2 那条 04 的第一拍)
 *   编号随日志一起落盘, 于是 r126 的日志可以按编号与 r126 的 VDD 事件
 *   计数(TRC evt postVH/preVH/sessEnd)对齐。
 *
 * 【为什么不用 HwReg=2 的全量转储代替它】那是 39 个寄存器 × 3 行 = 117 行,
 *   而这里要回答的只是"第 1 行里那 5 个数对不对"; 全量转储留给 HwReg=2
 *   档(HwReg>=2 时本函数会额外调一次, 见下)。
 *========================================================================*/
/*==========================================================================
 * [r132] ProtClrBeforeModeSet -- clear CRTC Protect (CR11h bit7) first.
 *   Why this is general and not emulator-specific: FreeVGA, "CRT Controller
 *   Registers", Index 11h bit7 (Protect) -- "when this field is set to 1, the
 *   CRTC register indexes 00h-07h ignore write access, with the exception of
 *   bit 4 of the Overflow Register (07h)".  Every VGA mode set in the world
 *   clears it before programming the timing registers; a VBE mode set goes to
 *   the card's BIOS, and if Protect is still set the BIOS's CR00..CR07 writes
 *   are silently discarded while 4F02h returns 004F.
 *   Return: 0 = already clear, 1 = cleared+verified, 2 = write failed,
 *           3 = policy off (nothing touched).
 *   r132-marker: ProtClrBeforeModeSet.
 *========================================================================*/
static WORD ProtClrBeforeModeSet( const char *why )
{
    int r;
    WORD w;

    if (!g_runProtClr)
        return 3;
    r = VxdCrtProtectClear();
    w = (WORD)((r < 0) ? 2 : (WORD)r);
    if (s_protClrN < 32)
    {
        s_protClrN++;
        DiagHex( "ProtClr: 0=was clear 1=cleared 2=write failed 3=off", w );
        PHASEV( "PH: protclr=", w );
        (void)why;
        FLUSH();
    }
    return w;
}

/*==========================================================================
 * [r133] CrtRelatchAfterRebuild -- put the mode back in the one order that
 *        cannot leave a stale latch behind, then MEASURE the scan-out.
 *
 *   Why the order is the fix and not a guess: see drv.c VxdCrtRelatch.  In one
 *   line -- every register we can read reports a correct 1024x768 raster in
 *   the very sessions whose host capture shows 128 columns (./129 ./130
 *   ./131 ./132), and a register read cannot see a latch, only a write can
 *   refresh one.  So: drive the display gate (CR17h bit7, SE) to 0, program
 *   every timing/clock/mode register from our known-good sample, then open the
 *   gate LAST -- the last write is a real change, so it is the one that
 *   survives in whatever latch the implementation keeps.
 *
 *   Why the measurement is here too: the same four sessions show that "read
 *   the registers back" cannot decide this question.  VxdRastProbe reads Input
 *   Status 1 (3DAh) instead and counts the display-enable duty cycle; 762 per
 *   mille means 1024 of 1344 pixels, 95 per mille means 128 of 1344.  The
 *   verdict line says which one it saw, and "inconclusive" when the samples
 *   never changed (a machine that does not advance its raster between two port
 *   reads must not be reported as a measurement).
 *
 *   Switches: [omninine] Relatch=0 (off), 1 (default).  Nothing here writes a
 *   register value that the sample did not already hold.
 *   r133-marker: CrtRelatchAfterRebuild.
 *========================================================================*/
static void CrtRelatchAfterRebuild( const char *why )
{
    int  d;

    (void)why;

    if (!g_runRelatch)
    {
        DiagHexMin( "Relatch: skipped (Relatch=0)", 1 );
        return;
    }

    d = VxdCrtRelatch();
    if (d < 0)
    {
        DiagHexMin( "Relatch: skipped (no usable sample yet)", 1 );
    }
    else
    {
        /* The relatch wrote, so the replay source has to be re-read from the
         * state it produced, otherwise the next session replays the state
         * before the fix. */
        int s2 = VxdHwRegSave();
        int d2 = (s2 > 0) ? VxdHwRegDiff() : -1;

        DiagHexMin( "Relatch: diff after relatch (0=exact)", (WORD)d );
        DiagHex( "Relatch: sample re-read ok (1=usable)", (WORD)((s2 > 0) ? 1 : 0) );
        DiagHex( "Relatch: sample diff after re-read", (WORD)((d2 < 0) ? 0xEEEE : d2) );
        PHASEV( "PH: relatch=", (WORD)d );
        PHASEV( "PH: relatch sample=", (WORD)((s2 > 0) ? 1 : 0) );
        /* d2 goes through PHASEV, not through DiagHex: the release build has no
         * DiagHex at all, and a variable that only the diagnostic macro touches
         * would come back as a new W202 in exactly the build the warning set is
         * compared against.  PHASEV is live in all three builds. */
        PHASEV( "PH: relatch sample diff=", (WORD)((d2 < 0) ? 0xEEEE : d2) );
    }

    /* ---- measure the raster as it is actually being generated ----------
     * This half is diagnosis, not a repair: it costs a bounded burst of port
     * reads (no waiting loop), so the release build -- which writes no log at
     * all -- must not pay for it.  MIN and DIAG both carry it, exactly like
     * every other decisive read in this file. */
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
    if (s_rastN < 64)
    {
        WORD n, de1, vb, pm, verdict;

        n   = VxdRastRun();
        de1 = VxdRastDe1();
        vb  = VxdRastVbl();
        pm  = VxdRastPermille();

        if (n == 0)
            verdict = 0;                        /* no data at all          */
        else if (de1 == 0 || de1 == n)
            verdict = 1;                        /* constant: inconclusive  */
        else if (pm >= 600)
            verdict = 2;                        /* wide: 1024 of 1344      */
        else if (pm <= 300)
            verdict = 3;                        /* narrow: 128 of 1344     */
        else
            verdict = 4;                        /* in between              */

        DiagHexMin( "RAST: samples", n );
        DiagHexMin( "RAST: de1 (display enable high)", de1 );
        DiagHexMin( "RAST: de transitions", VxdRastTrans() );
        DiagHexMin( "RAST: vsync samples", vb );
        DiagHexMin( "RAST: duty permille (762 = 1024/1344, 95 = 128/1344)", pm );
        DiagHexMin( "RAST: verdict (0=no data 1=constant 2=wide 3=narrow 4=other)",
                    verdict );
        s_rastN++;
    }
#endif
    FLUSH();
}

/*==========================================================================
 * [r132] PredictEmuHdisp -- recompute the display size the SVGA core will use,
 *        from the CRTC registers we just read.
 *
 *   Source (quoted while analysing this failure; both are public):
 *     vid_svga.c, svga_recalctimings():
 *         hdisp = crtc[1]; if (crtc[1] & 1) hdisp++;
 *         if (!scrblank && (crtc[0x17] & 0x80)) { hdisp *= dots; }
 *         else                                     dots = 1;
 *         ... walk dot from hblankstart, wrap at htotal, break at the first
 *             (dot & eff_mask) == (hblank_end_val & eff_mask), counting
 *             hblank_sub for every step past htotal ...
 *         hdisp -= hblank_sub * dots;
 *         non_blanked = (hblankstart + 1 - hblank_sub) * dots;
 *         hdisp = MIN(hdisp, non_blanked);
 *     vid_voodoo_banshee.c, banshee_recalctimings() (VGA path):
 *         hblankstart    = (((crtc[0x1a] & 0x10) >> 4) << 8) + crtc[2];
 *         hblank_end_val = (crtc[3] & 0x1f) | ((crtc[5] & 0x80) >> 2)
 *                          | ((crtc[0x1a] & 0x20) << 1);
 *         hblank_end_mask = 0x7f;  eff_mask = (hblank_end_val & ~0x3f) ? 0x7f
 *                                                                     : 0x3f;
 *         dispend = crtc[0x12] | (crtc[7] & 2 ? 0x100 : 0)
 *                             | (crtc[7] & 64 ? 0x200 : 0), + 1, and clamped
 *                   down to vblankstart.
 *   With CR01 = 7Fh (128 chars), CR00 = A3h (htotal 168) the collapse to
 *   exactly 128 = CR01+1 needs hblank_end_val & 7Fh == 70h (=> CR03 & 1Fh =
 *   10h, CR05 bit7 = 1, CR1A bit5 = 1) together with hblankstart > 112: the
 *   walk then finds no end-match at or after the start, wraps, and pays
 *   hblank_sub = 112 => 1024 - 112*8 = 128, MIN(128,128) = 128.  Both CR03
 *   (index 3) and CR07 are inside the Protect range, which is why the collapse
 *   survives a mode set until Protect is cleared.
 *   r132-marker: PredictEmuHdisp (vid_svga clamp model).
 *========================================================================*/
static WORD s_predVdisp;                                 /* r132 */
static WORD PredictEmuHdisp( void )
{
    DWORD htotal, hdisp, dots, start, endval, mask, dot, adj, sub;
    DWORD dispend, vbs;
    LONG  nonblank;
    WORD  cr00, cr01, cr02, cr03, cr05, cr07, cr09, cr12, cr15, cr1a, cr17, sr01;

    /* No array parameters on purpose: in this memory model a pointer argument
     * mixes near/far sizes and wcc warns (W112 + N2002..N2004 notes).  Reading
     * the twelve registers again costs nothing next to the tick that called. */
    cr00 = VxdCrtProbeRead( 0x000 );
    cr01 = VxdCrtProbeRead( 0x001 );
    cr02 = VxdCrtProbeRead( 0x002 );
    cr03 = VxdCrtProbeRead( 0x003 );
    cr05 = VxdCrtProbeRead( 0x005 );
    cr07 = VxdCrtProbeRead( 0x007 );
    cr09 = VxdCrtProbeRead( 0x009 );
    cr12 = VxdCrtProbeRead( 0x012 );
    cr15 = VxdCrtProbeRead( 0x015 );
    cr17 = VxdCrtProbeRead( 0x017 );
    cr1a = VxdCrtProbeRead( 0x01A );
    sr01 = VxdCrtProbeRead( 0x101 );

    htotal = (DWORD)cr00 + 5;                       /* CR00 + 5 (86box note)  */
    if (cr1a & 0x01)
        htotal += 0x100;                            /* CR1A bit0              */
    hdisp = (DWORD)cr01;
    if (cr01 & 1)
        hdisp++;
    if (sr01 & 0x08)                                /* SR01 bit3: /2 clock    */
        dots = (sr01 & 0x01) ? 16 : 18;
    else
        dots = (sr01 & 0x01) ? 8 : 9;

    if ((sr01 & 0x20) || !(cr17 & 0x80))            /* scrblank or SE = 0     */
    {
        dots = 1;                                   /* blank renderer         */
    }
    else
    {
        hdisp *= dots;
        if (cr1a & 0x04)
            hdisp += 0x100;                         /* CR1A bit2              */
    }

    start  = (((DWORD)(cr1a & 0x10)) << 4) + (DWORD)cr02;
    endval = (DWORD)(cr03 & 0x1F) | ((DWORD)(cr05 & 0x80) >> 2)
                                  | ((DWORD)(cr1a & 0x20) << 1);
    mask   = (endval & ~0x3FUL) ? 0x7FUL : 0x3FUL;

    dot = start; adj = start; sub = 0;
    while (adj < (htotal << 1))
    {
        if (dot == htotal)
            dot = 0;
        if (adj >= htotal)
            sub++;
        if ((dot & mask) == (endval & mask))
            break;
        dot++; adj++;
    }

    if (hdisp > sub * dots)
        hdisp -= sub * dots;
    else
        hdisp = 0;
    nonblank = ((LONG)start + 1 - (LONG)sub) * (LONG)dots;
    if (nonblank < 0)
        nonblank = 0;
    if ((LONG)hdisp > nonblank)
        hdisp = (DWORD)nonblank;

    dispend = (DWORD)cr12;
    if (cr07 & 0x02)
        dispend |= 0x100;
    if (cr07 & 0x40)
        dispend |= 0x200;
    dispend++;
    vbs = (DWORD)cr15;
    if (cr07 & 0x08)
        vbs |= 0x100;
    if (cr09 & 0x20)
        vbs |= 0x200;
    vbs++;
    if (vbs < dispend)
        dispend = vbs;                              /* 86box clamp            */

    s_predVdisp = (WORD)dispend;
    (void)sub;
    return (WORD)hdisp;
}

/*==========================================================================
 * [r132] CrtSnapSession -- capture the CRTC while the DOS session is on screen.
 *   Every previous round could only read the registers *after* the session had
 *   ended (CrtProbeNow(已于 r407 删除) call sites), which is exactly where the damage is no
 *   longer visible: the sample we then took became the "known good" one.  The
 *   cursor tick keeps running throughout a windowed DOS box (the log's
 *   `MARK HB` T= deltas prove it), so hook the snapshot there.
 *   Bounded twice: at most one snapshot per 32 ticks and at most 16 per boot.
 *   A snapshot is written when the geometry bytes or the state bits changed,
 *   plus one in every four for the record.
 *   r132-marker: CrtSnapSession (tick-driven CRTC snapshot).
 *========================================================================*/
static void CrtSnapSession( const char *why, int force )
{
    static WORD last[16];
    static WORD lastSt = 0xFFFF;
    WORD v[16];
    WORD i, st, diff = 0, pred, pv;

    (void)why;                              /* DIAG() is empty in release */
    if (!g_runSnapSess || s_snapN >= 16)
        return;
    /* 【r135】force=1: 由"方向 = 盒子刚拿走屏幕"这条边沿直接调用 —— 那一拍
     *   的读数就是 (B) 的现场, 不能被 tick 门或去重门挡掉。其余调用点
     *   (光标 tick)仍按 r132 的节流走, 行为逐字不变。               */
    if (!force)
    {
        if (s_snapTick++ < 32)
            return;
        s_snapTick = 0;
    }

    v[0]  = VxdCrtProbeRead( 0x000 );   /* CR00 horizontal total            */
    v[1]  = VxdCrtProbeRead( 0x001 );   /* CR01 end horizontal display      */
    v[2]  = VxdCrtProbeRead( 0x002 );   /* CR02 start horizontal blanking   */
    v[3]  = VxdCrtProbeRead( 0x003 );   /* CR03 end horizontal blanking     */
    v[4]  = VxdCrtProbeRead( 0x005 );   /* CR05 end horizontal retrace      */
    v[5]  = VxdCrtProbeRead( 0x007 );   /* CR07 overflow (VDE8/VDE9)        */
    v[6]  = VxdCrtProbeRead( 0x01A );   /* CR1A (hblank bit8 / hdisp bit8)  */
    v[7]  = VxdCrtProbeRead( 0x011 );   /* CR11 bit7 = Protect              */
    v[8]  = VxdCrtProbeRead( 0x101 );   /* SR01 clocking mode               */
    v[9]  = VxdCrtProbeRead( 0x206 );   /* GCR06                            */
    v[10] = VxdCrtProbeRead( 0x012 );   /* CR12 vertical display end        */
    v[11] = VxdCrtProbeRead( 0x015 );   /* CR15 start vertical blanking     */
    v[12] = (g_runProbe2 ? VxdCrtProbeRead( 0x310 ) : 0xFFFF);  /* ATC10    */
    v[13] = VxdCrtProbeRead( 0x009 );   /* CR09 (VBS bit9)                  */
    v[14] = VxdCrtProbeRead( 0x017 );   /* CR17 bit7 = SE                   */
    v[15] = VxdCrtProbeRead( 0x013 );   /* CR13 offset                      */

    st = VxdStateBitsNow();
    if (st != lastSt)
        diff = 1;
    for (i = 0; i < 16; i++)
        if (v[i] != last[i])
            diff = 1;
    if (!force && !diff && ((s_snapN & 3) != 0))
        return;

    for (i = 0; i < 16; i++)
        last[i] = v[i];
    lastSt = st;
    s_snapN++;

    pred = PredictEmuHdisp();
    pv   = s_predVdisp;

    DiagHex( "SNAP n", (WORD)s_snapN );
    DiagHex( "SNAP state bits", st );
    DiagHex( "SNAP CR00", v[0] );
    DiagHex( "SNAP CR01", v[1] );
    DiagHex( "SNAP CR02", v[2] );
    DiagHex( "SNAP CR03", v[3] );
    DiagHex( "SNAP CR05", v[4] );
    DiagHex( "SNAP CR07", v[5] );
    DiagHex( "SNAP CR1A", v[6] );
    DiagHex( "SNAP CR11 protect (bit7)", (WORD)((v[7] & 0x80) ? 1 : 0) );
    DiagHex( "SNAP SR01", v[8] );
    DiagHex( "SNAP GCR06", v[9] );
    DiagHex( "SNAP CR12", v[10] );
    DiagHex( "SNAP CR15", v[11] );
    DiagHex( "SNAP ATC10", v[12] );
    /* 【r137】把属性控制器那一组全部点名。./136 里能判定的只有 ATC10h 一个
     *   读数(0041h -> 000Dh), AR11h..AR14h 从来没被读过 —— 而"屏幕的
     *   颜色/平面/平移"恰好都住在它们里面。多三个只读读数, 下一轮才有
     *   "是只有一个寄存器变了还是一片都变了"这个问题的答案。          */
    {
        uint16_t a11 = VxdReadAtcReg( 0x11 );
        uint16_t a12 = VxdReadAtcReg( 0x12 );
        uint16_t a13 = VxdReadAtcReg( 0x13 );
        uint16_t a14 = VxdReadAtcReg( 0x14 );

        DiagHex( "SNAP AR11 overscan", a11 );
        DiagHex( "SNAP AR12 plane en", a12 );
        DiagHex( "SNAP AR13 pel pan",  a13 );
        DiagHex( "SNAP AR14 color sel",a14 );
        DiagHex( "SNAP ar ok (r137)", (WORD)VxdHwRegArOk() );
        /* 【发布档也要有这四个读数】DiagHex 在 MIN/REL 里是不展开实参的空宏,
         *   于是上面四行(以及 a11..a14 本身)在发布构建里会整条消失 —— 这正是
         *   r127/r129 已经栽过两次的那个坑(端口读写写在日志宏实参里, release
         *   里连 in 指令都不存在)。PHASEV 是不受任何构建宏影响的通道, 用它把
         *   这四个数【无条件】落盘, 两个构建因此是同一台仪器。          */
        PHASEV( "PH: snap AR11=", a11 );
        PHASEV( "PH: snap AR12=", a12 );
        PHASEV( "PH: snap AR13=", a13 );
        PHASEV( "PH: snap AR14=", a14 );
    }
    DiagHex( "SNAP CR09", v[13] );
    DiagHex( "SNAP CR17 SE (bit7)", (WORD)((v[14] & 0x80) ? 1 : 0) );
    DiagHex( "SNAP CR13", v[15] );
    DiagHex( "SNAP predicted hdisp (SVGA model)", pred );
    DiagHex( "SNAP predicted vdisp (SVGA model)", pv );
    /* 【r135】一句话判据: 卡现在扫的是"VGA 文本"还是"高分辨率图形"?
     *   这一行就是 (B)"窗口 -> 全屏 花屏"的方向盘读数 —— 盒子在全屏时
     *   如果读到图形态, 那它往文本显存里画的字就不可能出现在屏幕上。
     *     1 = 文本态: SE=0(CR17 bit7) + GCR06 bit0=0(字符模式) +
     *         ATC10 bit0=0 + SR01 bit0=1(8 点时钟) + CR00 是文本时序
     *     0 = 图形态: SE=1 且 GCR06 bit0=1(正是我们 hires 模式的样子)
     *     2 = 判不出(端口读不到 / Probe2 关)                          */
    {
        WORD tv = 2;

        if (v[12] != 0xFFFF)
        {
            if ((v[14] & 0x80) && (v[9] & 0x01))
                tv = 0;
            else if (!(v[14] & 0x80) && !(v[9] & 0x01) && !(v[12] & 0x01) &&
                     (v[8] & 0x01) && (v[0] <= 0x7F))
                tv = 1;
        }
        DiagHex( "SNAP box verdict (0=graphics,1=VGA text-like,2=unknown)", tv );
        PHASEV( "PH: snap box verdict=", tv );
    }
    DIAG( why );
    if (s_snapN <= 8)
    {
        PHASEV( "PH: snap n=", (WORD)s_snapN );
        PHASEV( "PH: snap CR02=", v[2] );
        PHASEV( "PH: snap CR03=", v[3] );
        PHASEV( "PH: snap CR05=", v[4] );
        PHASEV( "PH: snap CR07=", v[5] );
        PHASEV( "PH: snap CR1A=", v[6] );
        PHASEV( "PH: snap prot=", (WORD)((v[7] & 0x80) ? 1 : 0) );
        PHASEV( "PH: snap predH=", pred );
        PHASEV( "PH: snap predV=", pv );
    }
    FLUSH();
}


static void HwBaseline( void )
{
    WORD ax;

    if (!g_runHwChk || !wEnabled || wBpp == 0)
    {
        DIAG( "HW probe: boot baseline SKIPPED (HwChk=0 or not enabled yet)" );
        FLUSH();
        return;
    }
    ax = VBE_GetVideoMode();
    DiagHex( "HW probe: boot baseline int10 ax", ax );
    if (ax == 0xFFFF)
    {
        DIAG( "HW probe: boot baseline unavailable (no V86 passthrough)" );
        FLUSH();
        return;
    }
    g_hwBaseAl  = (WORD)(ax & 0xFF);
    g_hwBaseAh  = (WORD)((ax >> 8) & 0xFF);     /* r109: HealGate 的基线 */
    g_hwModeAl  = g_hwBaseAl;
    DiagHex( "HW probe: boot baseline al", g_hwBaseAl );
    DiagHex( "HW probe: boot baseline ah", (WORD)((ax >> 8) & 0xFF) );
    DIAG( "HW probe: boot baseline taken -> all later verdicts are differential" );
    /* 【r108 更正】这一族探针的名字叫 "HW probe", 但它【读不到硬件】——
     * 见 HwProbe 头部的 r108 注: INT 10h 在 Windows VM 里由主 VDD 的虚拟
     * BIOS 接走, 回的是主 VDD 自己的模式记账。这一行把语义钉在日志里,
     * 免得下一轮再把它当成"硬件读数"用。                            */
    DIAG( "HW probe: NOTE this is the main VDD virtual BIOS answer (INT 10h AH=0Fh), NOT a hardware read" );
    DiagMark( "HWBASE" );
    FLUSH();
    /* 【r109】开机基线这一刻同时取一份【真硬件】读数(默认关)。
     * 只有这一枪是"已知良好"的: 硬件确实在 1024x768x16bpp、pitch=0x800。
     * 拿它当标尺, 才能把 CRTC 13h(offset) 的编码单位标定出来 —— 之后每次
     * 边沿再读, 一眼看出 DOS 盒/主 VDD 把 pitch 与起址改成了什么。
     * 【r110 已撤除这一枪】./109 的 t3 实测: 打开 PortProbe 的那次开机
     * 桌面根本没起来(640x480 空白青底 + 2 行文本残留)且随后整机冻结。
     * 端口读本身回了完全合理的值, 所以嫌疑落在"读的时机/读的端口组":
     * r110 起端口读只发生在【重建前/重建后】这一对位置(那时屏幕本来就是
     * 坏的, 读一次的边际风险最小), 并且默认不读 3CCh/3C6h(DAC/Misc Out)。
     * 开机基线的标尺作用由"第一次重建前"那一枪代替 —— 两者都是
     * 1024x768x16bpp 现场, 后者反而是"出问题的此刻", 更有用。        */
    (void)0;
}

/*
 * 【r106】HwProbe —— 问主 VDD 的虚拟 BIOS "显示模式现在是什么"。
 *   见 g_runHwChk 声明处的长注: 走 INT 10h AX=0F00h(drv.c 的
 *   VBE_GetVideoMode), 与 VBE_SetMode 同一条 VxD 嵌套 V86 直通,
 *   只读、不改硬件, 所以 DOS 会话进行中也能问。
 *
 *   【r108 更正 —— 这条探针读不到硬件, 名字里的 HW 是 r106 的误称】
 *   证据(全部来自 ./107 的同一批日志):
 *     ① 4F02 这条通道确实是真硬件通道: `VBE_SetMode: exec ax=004F` 且
 *        用户看到 1024x768 桌面 ⇒ AX 回读可信;
 *     ② 可同一条通道上 0F00h 回的是 `ax=8072`(AH=0x80=128 列, AL=0x72),
 *        它在 VESA 里不是一个合法答案, 在 VGA 里也不是(0x72 不是模式号);
 *     ③ 会话收尾处同一条调用变成 `ax=5086`(AH=0x50=80 列 = 文本模式),
 *        之后我们一 4F02 重建, 它又回到 0x8072。
 *   ⇒ 3 条合起来只有一个解释: INT 10h 在 Windows VM 里被【主 VDD 的虚拟
 *   BIOS】接走了(VBE 的 4Fxx 由它转发给真 BIOS, 所以 4F02 有效; 而 AH=0Fh
 *   这种标准 VGA 功能它自己就答了), 这个读数反映的是【主 VDD 的模式记账】,
 *   不是硬件光栅/调色板状态。它的价值因此只剩"主 VDD 认为模式换过了"这一
 *   差分信息(②→③ 就是那次 hires→VGA→hires 往返的指纹)。
 *   返回:  1 = 与开机基线同值(主 VDD 的记账没变);
 *           0 = 变了(主 VDD 认为模式换过 —— 但这不是屏幕花不花的判据);
 *          -1 = 没问(开关关掉 / 取不到 / 还没进高彩模式 / 基线未知)。
 *   无论返回什么都会打日志: `HW probe:` 三行 + 一行结论。             */
static int HwProbe( const char *why )
{
    WORD ax, al;

    if (!g_runHwChk || !wEnabled || wBpp == 0)
        return -1;
    (void)why;
    DIAG( why );                        /* 一行"这次是谁问的", 便于对照日志 */
    ax = VBE_GetVideoMode();
    g_hwProbeN++;
    DiagHex( "HW probe: int10 ax", ax );
    if (ax == 0xFFFF)
    {
        g_hwModeAl = 0xFFFF;
        g_hwLastAh = 0xFFFF;            /* r109 */
        DIAG( "HW probe: unavailable (no V86 passthrough)" );
        FLUSH();
        return -1;
    }
    g_hwLastAh = (WORD)((ax >> 8) & 0xFF);      /* r109: HealGate 的判据之一 */
    al = (WORD)(ax & 0xFF);
    g_hwModeAl = al;
    DiagHex( "HW probe: mode al", al );
    DiagHex( "HW probe: cols ah", (WORD)((ax >> 8) & 0xFF) );
    if (al == 0xFF)
    {
        DIAG( "HW probe: OK, VESA canonical answer (AL=FFh)" );
        FLUSH();
        return 1;                       /* VESA: VBE 模式一律回 0FFh */
    }
    /* 【r107 差分判据】不是 0FFh 并不等于异常(见 HwBaseline 的长注);
     * 只有【与开机基线不同】才是异常。                                */
    if (g_hwBaseAl != 0xFFFF && al == g_hwBaseAl)
    {
        DIAG( "HW probe: OK, same as boot baseline (main VDD virtual BIOS answer)" );
        FLUSH();
        return 1;
    }
    if (g_hwBaseAl == 0xFFFF)
    {
        /* 基线还没取到(例如 HwChk=1 但 Enable 基线那一步被跳过):
         * 这时【不下结论】—— 一个没有参考的"不一致"不足以当重建依据。*/
        DIAG( "HW probe: no baseline yet -> reading recorded, verdict withheld" );
        FLUSH();
        return -1;
    }
    /* 走到这里 = 主 VDD 的虚拟 BIOS 换了一个答案(本机实测: 开机/会话中是
     * 0x8072 = AH 0x80 列, 会话收尾处变成 0x5086 = AH 0x50 列 80 列文本)。
     * 【r108 更正】这【不是】"硬件被切走了"的读数 —— 见 HwProbe 头部注。
     * 它真正说明的是: 主 VDD 自己认为"显示模式换过了"。屏幕花不花要靠
     * 用户的眼睛 + 下面这条边沿的重建结果来判, 不能拿它当硬件判据。 */
    g_hwProbeBad++;
    DiagHex( "HW probe: int10 changed vs boot baseline, al", al );
    DiagHex( "HW probe: boot baseline al", g_hwBaseAl );
    DIAG( "HW probe: int10 answer changed (main VDD state, NOT a hardware read)" );
    FLUSH();
    return 0;
}

/* 【r117】DiagDispiCheck —— 前进声明(定义在 DiagEdgeProbe 之后)。
 *   把"本机当前模式"填进 DispiPortProbe 并调用它; 见定义处注释。 */
void __far DiagDispiCheck(const char *why, int allowFix);

/*
 * 【r107】DiagEdgeProbe —— 切屏状态位的边沿探针。
 *   调用者: drv.c 的 VxdTraceDump, 在"blob 头 204 的状态字节发生变化"、
 *   即主 VDD 刚把硬件切进/切出 DOS 会话的那一刻(也正是 ./104 那次 0D 蓝屏
 *   所在的一瞬间)。做三件事, 全部只读/只记录:
 *     ① 问硬件当前模式(r106 的 INT 10h AX=0F00h 直通, 见 HwProbe)——
 *        这条边沿上硬件到底在 VBE 通路还是被切成了 VGA, 当场成读数;
 *     ② 若用户配了 Crash=<段号>(system.ini), 就地探一次那个选择子 ——
 *        段号有可能是在这条边沿【之后】才被分配出来的, 只有在这里问才
 *        问得到(Enable 时刻的地图是快照, 看不见后来者);
 *     ③ 落一行 MARK EDGE: 把边沿钉在 tick 时间轴上。
 *   【为什么放在这里而不是别处】本条边沿在好会话里也只出现两三次, 代价
 *   可忽略; 而它是"DOS 会话开始"唯一可被驱动观测到的时刻。            */
void __far DiagEdgeProbe(void)
{
    (void)HwProbe( "HW probe: at DOS session state edge" );
    /* 【r109】同一条边沿上取一份【真硬件】读数(默认关)。
     * 这是整轮方案的关键观测点: 屏幕在这一刻前后开始花, 而 0Fh 探针只能
     * 说出"主 VDD 的记账变没变"。CRTC 的 offset/start 与 时序器/GCR 的
     * 像素格式位会直接说明硬件到底被改成了什么(见 PortProbeRead 注)。
     * 【r110 已撤除这一枪】理由同 HwBaseline 处的长注: ./109 t3 那次
     * 开 PortProbe 的开机没能起来, 而"状态边沿"正是主 VDD 正在切屏的
     * 那一刻 —— 在别人的事务里多做 14 次端口 I/O 是最不该冒的险。
     * 端口读现在只留在重建前/后(见 g_runPortGrp 注)。                */
    /* 【r107】同一条边沿上再取一次 VM 句柄(只读) —— 若它在这条边沿前后
     * 会变, 说明主 VDD 的"当前 VM"确实随切屏换了, 那么分派记录里的 EBX
     * 与它就是同一个量; 若始终不变, 那 EBX 是"发起者"而它是"当前 VM",
     * HiresAns=2 那条路可以直接划掉。                                */
#ifdef VBE9X_DIAG
    {
        uint32_t vmh = VBE_GetVMHandle();

        DiagHex( "VMH edge lo", (WORD)vmh );
        DiagHex( "VMH edge hi", (WORD)(vmh >> 16) );
    }
#endif
    if (g_runCrashSel)
        DiagSelProbe( (uint16_t)g_runCrashSel );
    /* 【r117】同一条边沿上读一次 VBE_DISPI(1CEh/1CFh) —— 这是本轮的核心
     * 仪器: "硬件此刻按什么 xres/yres/bpp/pitch 扫描"从此是读数而不是推断。
     * 【为什么只在"不在 VGA 会话里"时读】bit0 置起时主 VDD 正拿着硬件,
     * 那一刻多做任何端口 I/O 都是往别人的事务里插一脚(r110 撤掉 PortProbe
     * 的教训); 而我们要抓的是"回到 Windows 之后格式对不对"。
     * 【为什么这里允许修复】状态边沿本身就是"主 VDD 刚把硬件交回来"的
     * 时刻(bit0 已清), ring 3 上下文安全, 且正是花屏刚发生的那一拍 ——
     * 修复落在这一拍上, 用户看到的就是"一闪而过"而不是"一直花"。
     * 说明: 修复写的是 Bochs 家族私有端口, 由 DispiPortProbe 内部的 ID
     * 门槛保护(ID 不是 0xB0C0..0xB0C7 则永久静默, 一个字节都不写)。   */
    if (!(VxdStateBitsNow() & VBE_TR_ST_VGA))
        DiagDispiCheck( "DISPI probe at state edge (back to Windows mode)",
                        1 /* 允许修复(还要 DispiFix 策略与"真的不符") */ );
    DiagMark( "EDGE" );
    FLUSH();
}

/* 【r117】DiagDispiCheck —— 把"本机当前模式"填进探针并调用它。
 *   单独封一层的原因: 期望值来自本文件的三张全局表(wScreenX/wScreenY/
 *   wBpp/wScreenPitchBytes), 而探针本体在 drv.c(那里有 inp8/outp8 与
 *   全部端口 I/O 的家法)。任何一处忘掉 allowFix 都会静默降级成只读,
 *   所以统一从这里走。                                               */
void __far DiagDispiCheck(const char *why, int allowFix)
{
    if (wScreenX == 0 || wScreenY == 0 || wBpp == 0 || wScreenPitchBytes == 0)
        return;                         /* 模式还没建立: 无从比较 */
    DispiPortProbe( why, wScreenX, wScreenY, wBpp, wScreenPitchBytes,
                    (int)(allowFix && g_runDispiFix) );
}

/*
 * MaybeDeferredRestore —— 在下一次 ring-3 上下文里执行被推迟的模式重建。
 *   调用点全是我们自己的 DDI 入口(光标三件套/调色板/BeginAccess/
 *   EndAccess/ExtTextOut/Disable/UserRepaintDisable): 那些都是"普通
 *   ring-3 上下文", 不再是主 VDD 的切屏事务内部。
 *   【限流】一次只做一次; 失败只记日志, 不递归、不循环、不重试 —— 免得
 *   在一个本来就坏的卡上把系统拖住。
 */

/* [r406] 极简闸门(默认): 只问"状态对不对", 不问"刚才发生了什么"。
 *   旧闸门整体旁路, LegacyGate=1 可切回做 A/B。 */
/* [r406] 两分支极简闸门。
 *   分支 A(盒子在画): 只重绘, 绝不 4F02 —— 模式没变, 重设模式既无必要也有风险。
 *   分支 B(几何真的不一致): 走 VBE 级重建 how=0 (旧闸门默认值;
 *          how=1 会做 VxdHwRegRestore 芯片寄存器回放, 那是 r345 闪屏的根源)。
 *   节流与快照刷新沿用 r346。 */
#ifndef RB_SECS
#define RB_SECS 6      /* [r406] 盲发强制重建间隔(秒), 实机建议 5-8 */
#endif

#ifndef RB_SAFETY_SECS
#define RB_SAFETY_SECS 300   /* [r406] 官方路径实验期: 仅作最后兜底 */
#endif

static WORD  s_rbLast = 0;
static WORD  s_rbN    = 0;
static WORD  s_wait   = 0;
static WORD  s_pend   = 0;
static WORD  s_gateN  = 0;
static WORD  s_lastTraps = 0;
static WORD  s_evTick  = 0;
static WORD  s_trig    = 0;
static WORD  s_lastBits = 0;
static WORD  s_evTraps = 0;
static WORD  s_evV2h   = 0;

extern void __far __pascal DiagFlushBuf( void );

extern uint16_t __far __pascal DiagGetDrop( void );
extern uint16_t DiagSkipInDOS, DiagSkipReent, DiagSkipCb;

void __far __pascal MaybeDeferredRestore( void )
{
    if ((!s_inModeSet || (WORD)(*(WORD __far *)0x0040006CL - s_modeTick) > 36) && !g_inCbRestore)
        DiagFlushBuf();   /* [r406] 安全点(带守卫) */
    /* [r406] 入口计数: 每 16 次打印一组, 单行可能被 DLOG_skip 丢掉,
     *   但周期性的行不会全部丢 -> 用它判断"闸门到底有没有被调用". */
    s_gateN++;
    if ((s_gateN & 0x0F) == 0)
    {
        PHASEV( "PH: gate n=", s_gateN );
        PHASEV( "PH: evT n=",  s_evTraps );
        PHASEV( "PH: evV n=",  s_evV2h );
        PHASEV( "PH: rb n=",   s_rbN );
        PHASEV( "PH: drop n=", DiagGetDrop() );
        PHASEV( "PH: skipDOS=", DiagSkipInDOS );
        PHASEV( "PH: skipRent=", DiagSkipReent );
        PHASEV( "PH: skipCb=", DiagSkipCb );
    }


    /* [r406] three-way event-triggered repair:
     *   A) ENABLE_TRAPS 0->1 (windowed box starts)      -> repair immediately
     *   B) POST_VGA_TO_HIRES (fullscreen -> window/exit) -> delay 8 ticks, then repair
     *   C) windowed box exit                             -> DO NOTHING
     *   dedupe: at most one repair per 2 seconds. */
    if (!wEnabled)
        return;

    {
        WORD bits = VxdStateBitsNow();
        WORD v2h  = VxdTakeVgaToHiresEdge();
        WORD traps = (WORD)((bits & VBE_TR_ST_TRAPS) ? 1 : 0);
        WORD now = *(WORD __far *)0x0040006CL;   /* [r447] 必须先取, 否则 d 用未初始化值 */

        if ((bits & VBE_TR_ST_TRAPS) && !(s_lastBits & VBE_TR_ST_TRAPS))
        {
            if (!s_pend) { s_pend = 1; s_trig = 1; s_evTick = now; }
            PHASEV( "PH: ev traps=", ++s_evTraps );
        }

        if (v2h || bRestorePending)   /* [r406] 恢复 V2H 边沿(全屏->窗口唯一触发) */
        {
            if (!s_pend)
            {
                s_pend = 1;
                s_trig = 3;
                s_evTick = now;      /* [r447] 只在首次武装时记时 */
                PHASEV( "PH: ev v2h=", ++s_evV2h );
            }
        }
        s_lastTraps = traps;
        s_lastBits = bits;

        if (s_pend)
        {
            WORD d    = (WORD)(now - s_evTick);
            /* [r447] B 路不再等 8 tick: r442 实测"边沿后任意时刻修"都有效,
             *   真正需要的是"不在触发的那一拍当场修"(now 已初始化即可)。
             *   统一为 2 tick(≈110ms), 与 A 路一致; 若个别机器仍见花屏,
             *   把下面这个常量调回 4 或 8 即可。 */
#define RB_B_TICKS 2
            WORD need = (WORD)((s_trig == 1) ? 2 : RB_B_TICKS);

            (void)VxdTakeSafeToSetMode();

            /* [r447] DDK(VDD_SET_USER_FLAGS): BUSY=1 表示全屏 DOS 会话, 绝不画 */
            if (lpDriverPDevice && (lpDriverPDevice->deFlags & 0x0010))
                return;

            if (d >= need)
            {
                now = *(WORD __far *)0x0040006CL;
                if ((WORD)(now - s_rbLast) >= 36)
                {
                    s_rbLast = now;
                    s_pend = 0;
                    s_wait = 0;
                    bRestorePending = 0;
                    PHASEV( "PH: rb ev=", ++s_rbN );
                    PHASEV( "PH: rb trig=", s_trig );
                    ApplyDesktopRestore( 2, 1 );
                    PHASEV( "PH: rb done=", s_rbN );
                }
                else
                {
                    /* [r447] 速率未到: 保留 pending, 下一拍再试(旧代码会丢弃) */
                    return;
                }
            }
            return;
        }


        now = *(WORD __far *)0x0040006CL;
        if ((WORD)(now - s_rbLast) >= (WORD)(RB_SAFETY_SECS * 18))
        {
            s_rbLast = now;
            PHASEV( "PH: rb safety=", ++s_rbN );
            ApplyDesktopRestore( 2, 1 );
            PHASEV( "PH: rb done=", s_rbN );
        }
        return;
    }
    if (!wEnabled)
        return;

    /* --- A: 盒子在画 -> 只重绘 --- */
    if (VxdBoxArmedNow() == 1)
    {
        VddCall( VDD_SAVE_DRIVER_STATE );
        ClearVisibleScreen();
        RepaintScreen();
        PHASEV( "PH: heal repaintonly=", 1 );
        return;
    }

    /* --- B: 几何不一致 -> VBE 级重建 + 重绘 --- */
    if (DesktopStateMatches())
        return;

    {
        WORD now = *(WORD __far *)0x0040006CL;

        if ((WORD)(now - s_healLast) < 36)
            return;
        if (s_healCount >= 5)
        {
            PHASEV( "PH: heal cap=", 5 );
            return;
        }
        s_healLast = now;
        s_healCount++;
    }

    PHASEV( "PH: heal rebuild how0=", 1 );
    ApplyDesktopRestore( 0, 1 );

    s_snapPitch = HwPitchNow( "after heal" );
    s_snapState = VxdStateBitsNow();
    s_snapValid = 1;
    PHASEV( "PH: heal done=", 1 );
}


/*
 * MaybeHeal —— Heal=1 时的自愈判据。
 *   判据来自 VxD 侧的追踪计数: DISPLAY_DRIVER_DISABLING(26) 被调用过
 *   (说明主 VDD 认为显示驱动已被卸下/硬件被切走), 而此后我们并没有走
 *   RestoreDesktopMode —— 于是桌面所处的模式可能早已不是我们设的那个
 *   (E3/E4: 花屏后"重设模式能不能自救"这个实验一直没法人工做)。
 *   Heal=1 时把它变成驱动自动做的: 置 pending, 由上面的延迟路径重建一次。
 *   默认 0: 完全不影响 r98 行为。
 *   r99-marker: heal is polled at our ring-3 entries only.
 */
/*--------------------------------------------------------------------------
 * PitchWatchRequest —— 把守望者的请求交给 MaybeDeferredRestore 那条老路。
 *   定义放在这里是因为 bRestorePending/bForceRebuild 就在上面声明
 *   (C89: 先声明后使用); 调用点在 MaybeHeal() 之后。
 *   【为什么不自己动手】真正碰硬件的是那条老路, 它自带“DOS 盒还占着屏幕
 *   就不许画”的抑制闸门 —— DDK VDD_SET_USER_FLAGS 页的 "must not draw"。
 *------------------------------------------------------------------------*/

static WORD g_runHealOnDisabling = 1;   /* [r406] 允许用 heal 请求武装 */   /* [r406] 1 = 用 DISPLAY_DRIVER_DISABLING 武装(旧行为) */

void __far __pascal MaybeHeal( void )
{
    if ((!s_inModeSet || (WORD)(*(WORD __far *)0x0040006CL - s_modeTick) > 36) && !g_inCbRestore)
        DiagFlushThresh();   /* [r406] 只在"无待办/非回调"时落盘 */
    if (!g_runHeal)
        return;
    if (!wEnabled || !lpDriverPDevice)
        return;
    if (bRestorePending)
        return;
    if (VxdTakeHealPending())
    {
        /* [r406] DISPLAY_DRIVER_DISABLING = "VDD 要接管显示(进全屏)" -> 场景③,
         *   按设计【不动作】; 默认不再用它武装(开关可回退), 但仍消费标志。 */
        DIAG( "MaybeHeal: DISPLAY_DRIVER_DISABLING seen (arm=%d)" );
        FLUSH();
        if (g_runHealOnDisabling)
            bRestorePending = 1;
    }
}

/*
 * RepaintScreen —— 请求 USER 重绘整个桌面; 若 USER 正抑制重绘
 *   (UserRepaintDisable), 则记下待处理标志。
 */
static void RepaintScreen( void )
{
    if (RepaintFunc)
    {
        TraceOnce( TRACE_SLOT_REPAINT, "DDI first call: USER repaint callback" );
        if (!bNoRepaint)
        {
            g_repaintN++;
            /* [r134] Gate-proof evidence: the ring is dumped at the next
             * rebuild by CbRingDump, so "the desktop was really repainted"
             * survives even when the text log gate drops the lines around it. */
            CbRingPut( (uint16_t)(((uint16_t)CB_EVT_REPAINT << 8) |
                                  (g_repaintN & 0xFF)) );
            PHASEV( "PH: repaint n=", g_repaintN );
            RepaintFunc();
        }
        else
        {
            bPaintPending = TRUE;
            CbRingPut( (uint16_t)(((uint16_t)CB_EVT_REPAINT_DEFER << 8) |
                                  (g_repaintN & 0xFF)) );
            PHASEV( "PH: repaint deferred=", g_repaintN );
        }
    }
    else
    {
        /* No callback resolved: say so in the ring instead of silently doing
         * nothing (this is the one case where the desktop cannot be redrawn). */
        CbRingPut( (uint16_t)(((uint16_t)CB_EVT_REPAINT_DEFER << 8) | 0xFF) );
        PHASEV( "PH: repaint n=", g_repaintN );
    }
}

/*
 * UserRepaintDisable —— 序号称 500, USER 调用: 暂时禁止/恢复重绘请求。
 */
VOID WINAPI __loadds UserRepaintDisable( BOOL bDisable )
{
    TraceOnce( TRACE_SLOT_REPDIS, "DDI first call: UserRepaintDisable(500)" );
    bNoRepaint = bDisable;
    if (!bDisable && bPaintPending)
    {
        RepaintScreen();
        bPaintPending = 0;
    }
}

/*
 * GetDriverResourceID —— 序号称 450, USER 的未公开回调: 询问驱动是否
 *   自带更合适的资源版本(典型用途: 大字体 DPI 下的 fonts120.bin)。
 *   本驱动不附带任何资源, 原样返回 —— DPI≠96 时由 GDI 自行缩放。
 */
DWORD WINAPI __loadds GetDriverResourceID( WORD wResID, LPSTR lpResType )
{
    (void)lpResType;   /* [r406] 消除 W303 */
    TraceOnce( TRACE_SLOT_RESID, "DDI first call: GetDriverResourceID(450)" );
    return wResID;
}

/*
 * BeginAccess/EndAccess —— 填进 DIBENGINE.deBeginAccess/deEndAccess 的
 *   回调: DIB 引擎在访问屏幕表面前后调用。无硬件鼠标需要排除,
 *   直接透传 DIB 引擎的光标排除逻辑。
 *
 * 【r102 新增: 第二形态(INI Access2=1)】为什么值得有一个"什么都不做"的
 *   版本: 透传写法让 DIBENG 的调用链变成
 *       DIBENG 内部 → 我们的 BeginAccess → DIB_BeginAccess(又进 DIBENG)
 *   而 DIBENG 在光标保存/恢复与离屏面搬运时【本来就会调序号 105/106】,
 *   于是光标路径上随时可能出现"引擎正在搬运、回调又进引擎"的环。.100 的
 *   两次冻结都停在 DIBENG 前向调用里(光标三件套 / DIB_DibToDevice), 这
 *   条回环是首选嫌疑。本驱动没有需要排除的硬件光标(软件光标由引擎自己
 *   画), 所以"空回调"在语义上就是正确内容 —— 不是降级开关。
 */
VOID WINAPI __loadds NullBeginAccess( LPPDEVICE lpDevice, WORD wLeft, WORD wTop,
                                      WORD wRight, WORD wBottom, WORD wFlags )
{
    (void)lpDevice; (void)wLeft; (void)wTop; (void)wRight;
    (void)wBottom;  (void)wFlags;
}

VOID WINAPI __loadds NullEndAccess( LPPDEVICE lpDevice, WORD wFlags )
{
    (void)lpDevice; (void)wFlags;
}

/*==========================================================================
 * 【r105】光标排斥临界区(deCursorExclude / deCursorUnexclude)记账
 *
 * 为什么是这两个回调、为什么与鼠标有关 —— 见 g_runAccessLog 声明处。
 * 这里只做三件事, 全部是内存写:
 *   ① 数一对调用各来了多少次;
 *   ② 维护嵌套深度, 并记下"EndAccess 多于 BeginAccess"的次数(mismatch);
 *   ③ 抄下最前面 XCL_RECS 个 BeginAccess 的矩形与标志位。
 * 真正落盘在 XclDump, 由 DDI 入口(安全上下文)调用一次。
 *
 * 【为什么矩形和标志位都要抄】ddk/dibeng.h 给的 wFlags 里
 *   FB_ACCESS(0x0001) / CURSOREXCLUDE(0x0008) 是两个语义位。如果日志里
 *   出现"矩形等于整屏"或"标志位恒 0(即从来不是光标排斥)", 那说明引擎
 *   用的不是我们以为的那条路 —— 这本身就是结论。
 *========================================================================*/
#define XCL_RECS   8
static WORD s_xclBeg   = 0;         /* BeginAccess 次数                  */
static WORD s_xclEnd   = 0;         /* EndAccess 次数                    */
static WORD s_xclDep   = 0;         /* 当前嵌套深度                      */
static WORD s_xclDepMax= 0;         /* 观察到的最大深度                  */
static WORD s_xclMis   = 0;         /* EndAccess 多于 BeginAccess 的次数 */
static WORD s_xclCur   = 0;         /* 带 CURSOREXCLUDE(0x0008) 的 Begin  */
static WORD s_xclFb    = 0;         /* 带 FB_ACCESS(0x0001) 的 Begin     */
static WORD s_xclOther = 0;         /* 两个标志位都没有的 Begin          */
static WORD s_xclRecN  = 0;
static WORD s_xclRec[XCL_RECS][5];  /* l, t, r, b, flags                 */
static WORD s_xclDumped = 0;

static void XclNote( WORD wLeft, WORD wTop, WORD wRight, WORD wBottom, WORD wFlags )
{
    s_xclBeg++;
    s_xclDep++;
    if (s_xclDep > s_xclDepMax)
        s_xclDepMax = s_xclDep;
    if (wFlags & 0x0008)        s_xclCur++;
    else if (wFlags & 0x0001)   s_xclFb++;
    else                        s_xclOther++;
    if (s_xclRecN < XCL_RECS)
    {
        s_xclRec[s_xclRecN][0] = wLeft;
        s_xclRec[s_xclRecN][1] = wTop;
        s_xclRec[s_xclRecN][2] = wRight;
        s_xclRec[s_xclRecN][3] = wBottom;
        s_xclRec[s_xclRecN][4] = wFlags;
        s_xclRecN++;
    }
}

static void XclNoteEnd( void )
{
    s_xclEnd++;
    if (s_xclDep > 0)
        s_xclDep--;
    else
        s_xclMis++;             /* 多出来的 unexclude: 会把一份过期屏幕位
                                 * 盖回屏幕上 —— 正是"陈旧块"的形态      */
}

static void XclDump( void )
{
    WORD i;

    if (!g_runAccessLog || s_xclDumped)
        return;
    s_xclDumped = 1;
    DiagHex( "EXCL begin", s_xclBeg );
    DiagHex( "EXCL end",   s_xclEnd );
    DiagHex( "EXCL dep max", s_xclDepMax );
    DiagHex( "EXCL dep end", s_xclDep );      /* 结束时应当回到 0 */
    DiagHex( "EXCL mismatch", s_xclMis );     /* 非 0 = 一对调用失配 */
    DiagHex( "EXCL f cursor", s_xclCur );     /* CURSOREXCLUDE */
    DiagHex( "EXCL f fb",     s_xclFb );      /* FB_ACCESS     */
    DiagHex( "EXCL f none",   s_xclOther );
    DiagHex( "EXCL recs",     s_xclRecN );
    for (i = 0; i < s_xclRecN; i++)
    {
        DiagHex( "EXCL r l", s_xclRec[i][0] );
        DiagHex( "EXCL r t", s_xclRec[i][1] );
        DiagHex( "EXCL r r", s_xclRec[i][2] );
        DiagHex( "EXCL r b", s_xclRec[i][3] );
        DiagHex( "EXCL r f", s_xclRec[i][4] );
    }
    FLUSH();
}

VOID WINAPI __loadds BeginAccess( LPPDEVICE lpDevice, WORD wLeft, WORD wTop,
                                  WORD wRight, WORD wBottom, WORD wFlags )
{
    TraceOnce( TRACE_SLOT_BEGINACCESS, "DDI first call: BeginAccess callback" );
    /* 【r105】记账必须在调用【之前】: 万一引擎在内层不再返回, 日志里至少
     * 留下"最后一次排斥的是哪一块"。纯内存写, 不打日志(DOS I/O)。      */
    XclNote( wLeft, wTop, wRight, wBottom, wFlags );
    DIB_BeginAccess( lpDevice, wLeft, wTop, wRight, wBottom, wFlags );
}

VOID WINAPI __loadds EndAccess( LPPDEVICE lpDevice, WORD wFlags )
{
    static WORD wShotDone = 0;

    TraceOnce( TRACE_SLOT_ENDACCESS, "DDI first call: EndAccess callback" );
    XclNoteEnd();
    DIB_EndAccess( lpDevice, wFlags );

    /* 【r81 取证 · 需 system.ini [omninine] EndShot=1】DIBENG 每画完一批都会
     * 回调 EndAccess —— 这是"引擎刚写完屏"的观测点。第一次回调后把 PDEV
     * 原始 48 字节抄一遍, 与 Enable 末尾的 RAW 行对比, 就能看出引擎在真正
     * 作图时又改写了哪些字段(纯读, 不影响引擎)。
     * 【为何默认关闭】这里是在 DDI 回调上下文里写日志文件, 与本驱动其它
     * 落盘点(Enable 期)不同; 未在真机验证前不默认打开 —— 本轮不引入任何
     * 未验证的默认行为。只有一次, 不刷屏。                          */
    if (g_runEndShot && !wShotDone)
    {
        wShotDone = 1;
        DiagRawPdev( "RAW PDEV after 1st EndAccess:", lpDriverPDevice );
        FLUSH();
    }
}

/*
 * SetPalette —— 序号称 22。DDK 对该 DDI 的原文(win98ddk graphcnt):
 *      VOID SetPalette( WORD nStartIndex, WORD nNumEntries, RGBQUAD lpPalette );
 * "把硬件调色板的一项或多项设成指定颜色"。也就是说:【刷 DAC 是这个 DDI 的
 * 本职】, 让引擎知道新颜色只是附带(引擎自己要拿它做颜色匹配/翻译)。
 *
 * 【r96: 为什么这里要加参数帧自证 + 三条路开关】
 * r95 修好 ABI 后崩点变成 0D @ 0367:00003FA5 —— DIBENG 调色板族那一段
 * (SetPaletteExt=4020, 前面 0x23 字节处)。而本函数正是 Enable 返回后
 * GDI 调进来的第一个 DDI。要定案需要两样东西, 本轮都落到日志里:
 *   ① GDI 到底压了什么: 把参数帧的头 6 个字原样抄下来。DDK 文档把形参
 *      写成 `RGBQUAD lpPalette`(按值)而正文说是"数组地址", 只有实测帧
 *      能分辨"远指针 / 近指针 / 按值"这三种可能 —— 传错一种, 被调方拿到的
 *      就是野指针(选择子 0 或 GDI 堆里的垃圾), 一 lea/les 就 0D。
 *   ② 进引擎那一刻的 FS/GS: 0367 段的机器码里确有 `mov fs:[di],al`
 *      (r95 日志 DUMP sel=0367 off=2B60)。FS 若非法, 引擎内层循环一访问
 *      就 0D, 而崩点会精确落在"某条 fs: 指令"上。
 * 三条路(g_runPalMode):
 *    0(默认) DIBX_SetPalette —— DDK 标准路由(序号 22 → DIB_SetPalette),
 *            设备由引擎自己记着(CreateDIBPDevice 建的), 跳板纯透传;
 *    1       DIB_SetPaletteExt(4 参, 尾部多一个显式 PDEV) —— r95 的老路,
 *            保留下来做 A/B;
 *    2       完全不进引擎: 自己把颜色抄进 deBitmapInfo->bmiColors 并置
 *            CTCHANGE, 然后照常刷 DAC。这一条既是判定"崩点是否由这次引擎
 *            调用引起"的对照, 本身也是一条可用退路(引擎读颜色表时从
 *            bmiColors 读, 结果一致)。
 */
UINT WINAPI __loadds SetPalette( UINT wStartIndex, UINT wNumEntries,
                                 LPVOID lpPalette )
{
    TraceOnce( TRACE_SLOT_SETPALETTE, "DDI first call: SetPalette(22)" );
    /* r99: 普通 ring-3 上下文 —— 推迟恢复/自愈的另一个执行点, 同时也是
     * 追踪落盘点。BeginAccess/EndAccess 刻意【不挂】: 那两个是"排斥光标
     * 的写屏临界区", 在里面再调 VxD/重设模式属于自找麻烦。            */
    /* 【r100】顺序: 先取追踪 —— "切屏事务已收尾"(POST_VGA_TO_HIRES 计数
     * 变化)这个安全信号就是 VxdTraceDump 里比较出来的, 延迟重建必须
     * 在它之后才有机会看到。                                          */
    VxdTraceDump();
    MaybeDeferredRestore();

    /* ① 参数帧原样落盘(只读调用方的栈, 不动它一个字节)。
     * 【为什么从 &wStartIndex 往上读】pascal 约定: 第一个参数在最高地址,
     * 之后每个参数地址递减 2 字节。首参的地址就是帧基址, 于是 frame[0..5]
     * 依次是第 1..6 个参数 —— 无论真实签名是"3 个还是 4 个参数", 抄下来
     * 就能反推 GDI 压了几个字、每个字是什么。 */
    if (!g_traceDone[TRACE_SLOT_PALCHK])
    {
#ifdef VBE9X_DIAG
        WORD __far *pFrame = (WORD __far *)&wStartIndex;

        g_traceDone[TRACE_SLOT_PALCHK] = 1;
        DiagHex( "SetPalette: nStart",   (WORD)wStartIndex );
        DiagHex( "SetPalette: nNum",     (WORD)wNumEntries );
        DiagHex( "SetPalette: pal seg",  (WORD)((DWORD)lpPalette >> 16) );
        DiagHex( "SetPalette: pal off",  (WORD)(DWORD)lpPalette );
        DiagHex( "SetPalette: PDEV seg", (WORD)(((DWORD)lpDriverPDevice) >> 16) );
        DiagHex( "SetPalette: PDEV off", (WORD)(DWORD)lpDriverPDevice );
        DiagHex( "SetPalette: frame0",   pFrame[0] );
        DiagHex( "SetPalette: frame1",   pFrame[1] );
        DiagHex( "SetPalette: frame2",   pFrame[2] );
        DiagHex( "SetPalette: frame3",   pFrame[3] );
        DiagHex( "SetPalette: frame4",   pFrame[4] );
        DiagHex( "SetPalette: frame5",   pFrame[5] );
        /* ② 进引擎前的环境(FS/GS 由汇编探针回填; DS 用本模块 DGROUP 自证) */
        DibEnvProbe();
        DiagHex( "SetPalette: env FS", g_envFS );
        DiagHex( "SetPalette: env GS", g_envGS );
        DiagHex( "SetPalette: env DS", FP_SEG((const void __far *)&g_dibfn[0]) );
        DiagHex( "SetPalette: deFlags", lpDriverPDevice->deFlags );
        DiagHex( "SetPalette: path (0=nonExt/1=Ext/2=self)", g_runPalMode );
        FLUSH();
#endif
    }

    /* ③ 交给引擎(g_runPalMode 决定走哪条路)
     * 【r97 前提】引擎要的那台"设备"必须是我们的 PDEV: Ext 路靠第 4 个实参,
     * 非 Ext 路靠跳板注入的 EDX(见 g_runPalMode 长注释)。PDEV 为空时两条路
     * 都会拿 NULL 去读 deBitmapInfo —— 那正是 r95 那个 0x3FA5 的直接成因,
     * 所以先挡一道。 */
    if (!lpDriverPDevice)
    {
        DIAG( "SetPalette: PDEV is NULL - engine call skipped" );
        FLUSH();
    }
    else if (g_runPalMode == 2)
    {
        /* 自己写颜色表: 直接抄进 PDEV 的 BITMAPINFO->bmiColors, 并把
         * "颜色表已改" 记进 deFlags(CTCHANGE) —— 引擎读颜色时读的就是这里。*/
        if (lpDriverPDevice->deBitmapInfo && lpPalette && wNumEntries)
        {
            UINT n = wNumEntries;

            if (wStartIndex >= 256)
                n = 0;
            else if (wStartIndex + n > 256)
                n = 256 - wStartIndex;
            if (n)
            {
                _fmemcpy( &lpDriverPDevice->deBitmapInfo->bmiColors[wStartIndex],
                          lpPalette, n * sizeof( RGBQUAD ) );
                lpDriverPDevice->deFlags |= CTCHANGE;
            }
        }
        DIAG( "SetPalette: self path (no engine call)" );
        DiagHex( "SetPalette: deFlags after", lpDriverPDevice->deFlags );
        FLUSH();
    }
    else if (g_runPalMode == 1)
    {
        /* Ext 路: 设备指针是显式第 4 实参, 不依赖 EDX(但跳板照样注入,
         * 计数只作为"注入机制在执行"的证据)。 */
        DiagHex( "SetPalette: Ext edxCount before", g_edxCount );
        FLUSH();
        DIB_SetPaletteExt( wStartIndex, wNumEntries, lpPalette, lpDriverPDevice );
        DIAG( "SetPalette: engine call (Ext) returned" );
        DiagHex( "SetPalette: deFlags after", lpDriverPDevice->deFlags );
        DiagHex( "SetPalette: Ext edxCount after", g_edxCount );
        FLUSH();
    }
    else
    {
        /* 非 Ext 路(DIBLINK 形态): 设备指针只能靠跳板注入的 EDX 传进去。
         * 计数没涨 = 注入没执行 = 引擎会拿残留 EDX 当 DIBENGINE(0367:4036)。*/
        DiagHex( "SetPalette: nonExt edxCount before", g_edxCount );
        FLUSH();
        DIBX_SetPalette( (WORD)wStartIndex, (WORD)wNumEntries, lpPalette );
        DIAG( "SetPalette: engine call (non-Ext) returned" );
        DiagHex( "SetPalette: deFlags after", lpDriverPDevice->deFlags );
        DiagHex( "SetPalette: nonExt edxCount after", g_edxCount );
        FLUSH();
    }

    /* ④ 刷硬件 DAC。【为何进出都落盘】r95 的崩溃现场(0367:3FA5)落在
     * DIBENG 的"调色板翻译表"函数里, 而 r95 的 SetPalette 没有"引擎调用
     * 已返回"这一行, 于是"崩在引擎里"还是"崩在这之后"一直分不开。r97 把
     * 这一步的进入/返回都写进日志, 一轮就能定性。 */
    if (wBpp == 8 && lpColorTable && lpDriverPDevice &&
        !(lpDriverPDevice->deFlags & BUSY))
    {
        DiagHex( "SetPalette: DAC enter nStart", (WORD)wStartIndex );
        DiagHex( "SetPalette: DAC enter nNum",   (WORD)wNumEntries );
        FLUSH();
        /* DAC_SetPalette 从传入指针顺序读 count 项, 故传入起始项地址 */
        DAC_SetPalette( wStartIndex, wNumEntries,
                        (const uint8_t __far *)(lpColorTable + wStartIndex) );
        DIAG( "SetPalette: DAC_SetPalette returned" );
        FLUSH();
    }
    else
    {
        DiagHex( "SetPalette: DAC skipped (bpp/LUT/BUSY)", wBpp );
        FLUSH();
    }
    DIAG( "SetPalette: done" );
    FLUSH();
    return 0;
}

/*
 * 光标三件套(序号称 102/103/104)。SetCursor 与 windows.h 的用户态
 * SetCursor 同名, C 实体命名为 SetCursor_driver, 导出名 SetCursor
 * 由 vddapi16.asm 的别名跳转提供(手法同 vmdisp9x dibthunk.asm)。
 * wEnabled 守卫: 设备未使能/被切走时不碰帧缓冲。
 */
/*
 * 【r99 埋点说明】光标三件套是纯转发给 DIBENG 的(引擎负责 save-under 与
 * 掩码展开), 我们能提供的只有 lpCursor 内容与 PDEVICE 几何。用户的观察
 * 是"一进 login 出现光标就有陈旧块、点哪哪有、8bpp@1024x768 也一样、
 * 800x600x8bpp 不出现" —— 与 DOS 盒、位深都无关, 因此先埋点取证。
 * 【限流】MoveCursor/CheckCursor 每次鼠标移动都会被调用; 只在
 *   (a) VxD 那边有新追踪记录, 或 (b) 每 128 次调用时落盘,
 * 否则日志会被鼠标冲爆。
 * 同时这三处也是"推迟恢复/自愈"的天然执行点(普通 ring-3 上下文)。
 */
static WORD wCurTick = 0;      /* 光标调用限流计数 */

/* 【r112】DdiTrace —— 光标三件套的"相位面包屑"(见 g_runDdiTrace 的长注)。
 *   只在这个开关打开时落盘: 每次调用两行(入口/出口), 用来把"下一次
 *   一进 GUI 就冻结"的现场切成两半 —— 有入口没出口 = 挂在我们的代码里;
 *   连入口都没有 = GDI/VDD 侧停了。用 DIAG 而不是 DIAGM: 它只在 Log=2
 *   时落盘, 免得在 Log=1 的会话里再压一层日志闸门。                  */
#define DDITRACE_ENTER(id)  do { if (g_runDdiTrace) { DIAG( "DDITRACE enter " id ); FLUSH(); } } while (0)
#define DDITRACE_EXIT(id)   do { if (g_runDdiTrace) { DIAG( "DDITRACE exit " id );  FLUSH(); } } while (0)

WORD WINAPI __loadds SetCursor_driver( CURSORSHAPE __far *lpCursor )
{
    WORD __far *pw = (WORD __far *)lpCursor;

    (void)pw;                           /* 发布构建下 DiagHex 被编掉 */

    DDITRACE_ENTER( "102 SetCursor" );
    TraceOnce( TRACE_SLOT_SETCURSOR, "DDI first call: SetCursor(102)" );
    /* 【r100】顺序: 先取追踪 —— "切屏事务已收尾"(POST_VGA_TO_HIRES 计数
     * 变化)这个安全信号就是 VxdTraceDump 里比较出来的, 延迟重建必须
     * 在它之后才有机会看到。                                          */
    VxdTraceDump();
    MaybeDeferredRestore();

    /* 【r100】保存形状供 ResetHiResMode 之后重建光标(DDK 六项义务);
     *       形状没变时不再重复打 11 行(真机上这就是最大的日志量来源)。 */
    if (lpCursor && lpCursor != lpLastCursor)
    {
        DiagHex( "Cur set w0", pw[0] );   /* r128-hot- per-cursor, stays DIAG */
        DiagHex( "Cur set w1", pw[1] );
        DiagHex( "Cur set w2", pw[2] );
        DiagHex( "Cur set w3", pw[3] );
        DiagHex( "Cur set w4", pw[4] );
        DiagHex( "Cur set w5", pw[5] );
        if (lpDriverPDevice)
        {
            DiagHex( "Cur dev width",   lpDriverPDevice->deWidth );
            DiagHex( "Cur dev height",  lpDriverPDevice->deHeight );
            DiagHex( "Cur dev widthB",  lpDriverPDevice->deWidthBytes );
            DiagHex( "Cur dev bitsSel", lpDriverPDevice->deBitsSelector );
            DiagHex( "Cur dev flags",   lpDriverPDevice->deFlags );
        }
        FLUSH();
    }
    if (lpCursor)
    {
        lpLastCursor = lpCursor;        /* r105 起只当"形状变了没有"的比较键 */

        /* 【r105】CursorSrc=1: 在这里就把形状拷走, 之后【永不】再解引用
         * lpLastCursor。上面那次按指针去重只影响"要不要打日志", 拷贝则
         * 【每次】都做 —— GDI 完全可能复用同一个地址放另一份形状。      */
        if (g_runCursorSrc)
            CaptureCursorShape( lpCursor );
    }

    if (wEnabled)
    {
        DIB_SetCursorExt( lpCursor, lpDriverPDevice );
        DDITRACE_EXIT( "102 SetCursor" );
        return 1;
    }
    DDITRACE_EXIT( "102 SetCursor" );
    return 0;
}

#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
extern void __far __pascal DiagRefill( void );
static DWORD s_dwRefill = 0;
static void DiagMaybeRefill( void )
{
    DWORD dwNow = *(DWORD __far *)0x0040006CL;
    if (s_dwRefill == 0) { s_dwRefill = dwNow; return; }
    if ((DWORD)(dwNow - s_dwRefill) >= 18) { s_dwRefill = dwNow; DiagRefill(); }
}
#endif

void WINAPI __loadds MoveCursor( int absX, int absY )
{
    WORD tick = wCurTick++;
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
    if ((wCurTick & 0x1F) == 0) DesktopStateCheck( "move" );
#endif
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
    DiagMaybeRefill();
#endif

    DDITRACE_ENTER( "103 MoveCursor" );
    TraceOnce( TRACE_SLOT_MOVECURSOR, "DDI first call: MoveCursor(103)" );
    /* 【r100】先取追踪(安全信号在其中比较出来), 再决定是否延迟重建 */
    if (VxdTraceDump() || ((tick & 0x7F) == 0))
    {
        DiagHex( "Cur move x", (WORD)absX );
        DiagHex( "Cur move y", (WORD)absY );
        FLUSH();
    }
    MaybeDeferredRestore();
    if (wEnabled)
        DIB_MoveCursorExt( absX, absY, lpDriverPDevice );
    DDITRACE_EXIT( "103 MoveCursor" );
}

void WINAPI __loadds CheckCursor( void )
{
    WORD tick = wCurTick++;
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
    if ((wCurTick & 0x3F) == 0) DesktopStateCheck( "tick" );
#endif
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
    DiagMaybeRefill();
#endif

    DDITRACE_ENTER( "104 CheckCursor" );
    TraceOnce( TRACE_SLOT_CHKCURSOR, "DDI first call: CheckCursor(104)" );
    /* 【r100】先取追踪(安全信号在其中比较出来), 再决定是否延迟重建 */
    if (VxdTraceDump() || ((tick & 0x7F) == 0))
    {
        DIAG( "Cur check (tick)" );
        FLUSH();
    }
    /* 【r141】与会话边沿无关的第三条发现路径: 每 128 次光标 tick 问一次 4F06h。
     *   只读; 开火上限与“见过对的读数”两道边界见 g_runPitchWatch 长注【三】。 */
    PitchWatchTick();
    CrtSnapSession( "SNAP: cursor tick (during/after DOS session)", 0 );  /* r132 */
    MaybeDeferredRestore();
    if (wEnabled)
        DIB_CheckCursorExt( lpDriverPDevice );
    DDITRACE_EXIT( "104 CheckCursor" );
}

/*
 * ExtTextOut —— 序号称 14: 直接转发 DIB 引擎(需要显式定义而非
 *   vddapi16.asm 跳转, 只是为了保留这一处说明性注释的位置;
 *   语义与纯转发完全一致)。
 */
DWORD WINAPI __loadds ExtTextOut( LPDIBENGINE lpDestDev, WORD wDestXOrg,
                                  WORD wDestYOrg, LPRECT lpClipRect,
                                  LPSTR lpString, int wCount,
                                  LPFONTINFO lpFontInfo, LPDRAWMODE lpDrawMode,
                                  LPTEXTXFORM lpTextXForm, LPSHORT lpCharWidths,
                                  LPRECT lpOpaqueRect, WORD wOptions )
{
    TraceOnce( TRACE_SLOT_EXTTEXTOUT, "DDI first call: ExtTextOut(14)" );
    return DIB_ExtTextOut( lpDestDev, wDestXOrg, wDestYOrg, lpClipRect,
                           lpString, wCount, lpFontInfo, lpDrawMode,
                           lpTextXForm, lpCharWidths, lpOpaqueRect, wOptions );
}
