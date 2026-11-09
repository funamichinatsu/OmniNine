/*=============================================================================
 * drv.c —— 16 位显示驱动的 VxD 侧胶合层
 *
 * 【设计约束】
 *  1. 本文件里的策略变量只决定"驱动愿意做什么"，不决定"盒子当前的硬件状态"。
 *     盒子(全屏 DOS)的显示状态由盒内 BIOS 与 TSR 负责；这里读到的任何 VGA 端口值
 *     都来自 VDD 为 Windows 维护的影子，不是盒子的真值。
 *  2. 策略值经 VxdSetPolicies 一次性灌入；共享结构字段的顺序由 vbeshare.h/.inc 双方
 *     约定，改动任何字段都必须两侧同时重建，否则 C 与汇编的视图会错位。
 *  3. 打印逻辑只用于定位问题：一条测不到真值的探针比没有探针更糟，
 *     因为它会把"值没传过来"读成"值被夹取了"。
 *
 * 【大体思路】
 *  DrvInitCore 建立与主 VDD 的联系 → ReadDisplayConfig 取显示配置 →
 *  Enable 建立帧缓冲选择子并把 PDEV 交给 DIBENG → 之后各 DDI 入口转发。
 *  凡是需要"改盒子硬件"的动作，都不在本文件完成。
 *===========================================================================*/
/*============================================================================
 *
 *  DRV.C — 显示驱动一侧(16 位 ring 3)：VBE 模式信息 → mini-VDD
 *
 *  【本模块的角色】
 *  它是 16 位 display.drv 的一部分，负责：
 *    1) 通过 VBE BIOS(INT 10h AX=4Fxx)取得当前模式的几何参数；
 *    2) 组装 VBESHAREDSTATE(见 vbeshare.h)；
 *    3) 通过 INT 2F/AX=1684h 找到 MINIVDD.VXD 的 PM API 入口，
 *       把共享状态推送进 ring 0。
 *
 *  【设计定位(v2)：纯通用 VBE 驱动】
 *  本模块只通过 VBE 标准接口(INT 10h AX=4F00h/4F01h/4F02h)获取
 *  信息，不检测、不识别任何具体显卡型号。符合标准 VBE 模型的卡
 *  都能工作；VBE 支持不完善的老卡请使用其官方 Win9x 驱动——
 *  为它们叠加私有探测/补丁代码，历来是不稳定的根源。
 *
 *  【r76 主要变更：帧缓冲映射搬到 ring 3 自己做】
 *  旧版由 mini-VDD 在 ring 0 建 GDT 屏选择子交给本驱动使用，实机
 *  在第一次写帧缓冲时 0D 蓝屏(031F:00090B = _fmemset_ 的 "mov es,dx")。
 *  根因见 vbeshare.h 文件头；本版改为：
 *      DPMI 0800h 映射 LFB 物理地址 → DPMI LDT 选择子指向它
 *  全部在 ring 3 完成(在售参考驱动 boxv9x 的同款做法)，mini-VDD 只
 *  把选择子登记给主 VDD。见下方 LfbSelector_Ensure()。
 *
 *  【对原草案的主要修正】
 *  1. 原草案用 CreateFile("\\\\.\\MINIVDD.VXD") + DeviceIoControl 推送
 *     状态 —— 那是 Win32 API，16 位 display.drv 里根本不存在。正确的
 *     16 位通道是 INT 2F/AX=1684h 取 VxD 的 PM API 入口后远调用
 *     (本文件的 VxD_Connect / VxDCallApi)。
 *  2. 原草案的 GetBankPortForDetectedCard() 等三个函数是空壳，
 *     且服务于"识别具体卡后直连其 bank 寄存器"的卡专用设计。
 *     v2 起该方案整体废弃(含 VxD 侧的 PCI 扫描/已知卡表/端口
 *     陷阱)：bank 切换交给主 VDD 的 VESA BIOS 路径，空壳函数
 *     与共享结构中的卡专用字段一并删除，无需任何"补全"。
 *  3. 原草案在 PM 下直接把驱动数据段的 ES:DI 交给 INT 10h/4F01h ——
 *     V86 下 BIOS 会把选择子当段值用，写到错误内存。本版经 VxD 侧
 *     V86 常驻缓冲中转(r46 起): 客户端 DGROUP 静态缓冲与 VxD 经
 *     bufLen/bufLin 线性拷贝交换数据, 客户端零低内存接触(./45 实锤
 *     客户端自建选择子写低内存在全新 Win98 Enable 期 GP 死机)。
 *  4. 原草案用 regs.w.ax 访问寄存器包 —— .w 是 Watcom 方言，
 *     MSVC 1.52 的 union REGS 只有 .x 与 .h。本版统一用 .x。
 *
 *  【编译】本文件是 display.drv 的一个编译单元，用驱动工程自己的
 *  16 位 DLL 开关编译即可，例如：
 *     MSVC 1.52:   cl /c /W3 /Awnd /GD /Zp1 drv.c
 *     Open Watcom: wcc -q -wx -s -zu -zls -zW -bt=windows drv.c
 *  关键点只有一个：Win16 DLL 必然 SS!=DS(MSVC /Awnd, Watcom -zu)，
 *  本文件已按此假设编写(见 CALL_INT 宏的注释)。
 *  刻意只用 16 位寄存器做内联汇编 —— MSVC 1.52 的内联汇编器不认识
 *  386 的 32 位寄存器。
 *
 *==========================================================================*/

#include <windows.h>

extern unsigned short LogReady;   /* [r447] display.c 定义(uint16_t) */   /* [r447] 进度标记(asm) */
#include <dos.h>
#include <string.h>

#include "vbeshare.h"
#include "drv.h"

/* --- 诊断日志(与 display.c 的 DIAG 同一通道; 仅 VBE9X_DIAG 构建启用) ----
 * 实现在 vddapi16.asm 的 DiagWriteLog —— 纯汇编 INT 21h, 引导期安全。
 * 【r100 日志分级】g_logLevel 定义在这里, display.c extern 同一个变量:
 *   0 = 全静默(性能基线 —— 真机"登录慢"的第一个原因就是逐行日志:
 *           每行 6 次 INT 21h(open/seek/write/write/commit/close),
 *           一次会话近 6000 行 ⇒ 约 3.6 万次 DOS 调用)
 *   1 = 只打里程碑(DIAGM)    2 = 默认    3 = 全量(含每次 VxD 调用前后) */
uint16_t g_logLevel = 2;
/* 【r110】LogBoot —— "开机相"的日志级别, 与"会话期"级别分开。
 * 【为什么必须有这一对】./109 的 t6 是本项目第一个【日志级别改变启动结果】
 *   的实测: Log=1 那次开机回落了 VGA(BOOTLOG 里多出一次 vga.drv 装载,
 *   47305 字节的回落 bootlog 与 ./108 的 Log=0 那次逐字节同形), 而 Log=2
 *   正常。更麻烦的是 t3: 只把 PortProbe 打开(Log 仍是 2, 只多十几行日志),
 *   也起不来。也就是说"开机相里多写/少写几十行磁盘日志"能决定开机成败 ——
 *   这本身就是一条必须被独立验证的线索, 而它此前无法验证: 想要一份完整的
 *   开机日志, 就必须把整个会话都开到 Log=2(那会改变被测对象)。
 * 【怎么用】LogBoot=2 + Log=1 = 开机相照常逐行落盘, 桌面起来之后立刻降到
 *   里程碑档。若这一次开机【不再】回落, 则"回落与开机相的日志量有关"成立;
 *   若仍然回落, 则日志级别根本不是变量(那几行磁盘 I/O 只是碰巧的同伙)。
 * LogBoot=0(默认)= 与 Log 同档, 即 r109 行为, 一个字节都不变。      */
uint16_t g_logBoot  = 0;            /* 开机相级别; 0 = 跟随 Log          */
uint16_t g_runLog   = 2;            /* 会话期级别(ReadRunProfile 填)     */
uint16_t g_bootLogOn = 0;           /* 1 = 当前处于"开机相覆盖"状态      */
/* VxdLogBootEnd —— 退出"开机相级别覆盖": 回到 Log。可重复调用(幂等)。
 * 调用点(display.c): Enable 的 PDEVICE 相末尾 + MaybeDeferredRestore 入口
 * —— 两个都是"桌面已经/即将接管"的位置。                             */
void __far __pascal VxdLogBootEnd( void )
{
    if (g_bootLogOn)
    {
        g_bootLogOn = 0;
        g_logLevel  = g_runLog;
    }
}

/* --- r118: "MIN 通道" —— 第三构建(DIAG_MIN), 与 display.c 同名同义 --------
 *   DIAGMIN/DiagHexMin 在 DIAG 与 DIAG_MIN 两种构建里都是真函数, 其余
 *   宏(DIAG/DIAGM/DiagHex/...)在 MIN 构建里仍是 ((void)0) —— 于是 MIN
 *   构建 = release 的高频路径 + 少量决定性行。见 R118_CHANGES.md §2。     */
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
#else
#define DIAGMIN( s )        ((void)0)
#define DiagHexMin( m, v )  ((void)0)
#endif

#ifdef VBE9X_DIAG
extern void __far __pascal DiagWriteLog( const char __near *msg );
#define DIAG( s )   do { if (g_logLevel >= 2) DiagWriteLog( s ); } while (0)
#define DIAGM( s )  do { if (g_logLevel >= 1) DiagWriteLog( s ); } while (0)
#define DIAG3( s )  do { if (g_logLevel >= 3) DiagWriteLog( s ); } while (0)
#else
#define DIAG( s )   ((void)0)
#define DIAGM( s )  ((void)0)
#define DIAG3( s )  ((void)0)
#endif

/* 【r131】DiagHexPhase 在 release 档也要能调 DiagWriteLog, 而 release 档里
 *   上面那两个 #ifdef 块里的 extern 声明都不存在 —— 于是这里【无条件】再声明
 *   一次(重复声明在 DIAG/MIN 档无害)。少了它, release 编译会报
 *     drv.c(182): Warning! W131: 'DiagWriteLog' のプロトタイプがありま
 *   以及配对的 W308 —— 而本项目把"新增警告"当作回归信号看。 */
extern void __far __pascal DiagWriteLog( const char __near *msg );

/*==========================================================================
 *  【r131】DiagHexPhase —— 第三种通道: **永不编译掉**的最小骨架日志。
 *  【为什么必须有第三种通道】三个构建档共用一份源码:
 *      debug = -dVBE9X_DIAG       (DIAG/DIAGMIN 都有)
 *      MIN   = -dVBE9X_DIAG_MIN   (只有 DIAGMIN)
 *      release = 无宏             (两者都是空宏)
 *    于是 release 的 OMNININE.LOG 只有两行(PROBE 与 VDDGETDISPCONF, 它们走
 *    的是 vddapi16.asm 里另外那条无条件探针)—— ./129 与 ./130 连续两轮都
 *    卡在"release 行为与 debug 不一致, 而 release 的日志什么都没记"上,
 *    只能靠反汇编猜。这一轮把这件事直接解决: 在【两条路都必须经过的决策点】
 *    各落一行"值", 三种构建都能读出同一份骨架。
 *
 *  【为什么只落"值"】消息串会占 NE 文件体积(release 现在是 35KB, 对它的
 *    体积敏感)。所以本通道只在十几处固定的决策点各带【一个十六进制数】,
 *    判读靠位置与顺序, 不靠长句子; 需要细节时仍然开 debug/MIN 档。
 *
 *  【与 DIAGMIN 的区别】DIAGMIN 的返回值受 g_logLevel 与构建宏影响;
 *    DiagHexPhase **不看 g_logLevel、不受任何构建宏影响** —— 它是"这台机器
 *    上到底跑到哪一步"的唯一无条件证据, 也是 release 档唯一的行为遥测。
 *    (它与 DIAGMIN 共用 vddapi16.asm 的 DiagWriteLog 与同一套 InDOS/重入/
 *     回调闸门, 所以不会新增一条 DOS I/O 路径。)
 *  r131-marker: DiagHexPhase (unconditional skeleton log, all three builds).
 *========================================================================*/
/* 【r166】整块(含 PHASEV)在 release 档编掉 —— 见 g_logEnable 的注。 */
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
static const char hexdPhase[16] = "0123456789ABCDEF";
/* 【为什么自带缓冲】release 档里 DIAGMIN 那一整块(含 bufMin)是被 #else 掉
 * 的, 所以本通道不能用 bufMin —— 它自己带一个 96 字节的静态缓冲。 */
static char s_phaseBuf[96];
void __far __pascal DiagHexPhase( const char __near *msg, uint16_t v )
{
    char __near *p = s_phaseBuf;
    int i = 0;

    while (*msg && i < 88)
    {
        *p++ = *msg++;
        i++;
    }
    *p++ = '=';
    *p++ = hexdPhase[(v >> 12) & 15];
    *p++ = hexdPhase[(v >> 8) & 15];
    *p++ = hexdPhase[(v >> 4) & 15];
    *p++ = hexdPhase[v & 15];
    *p   = 0;
    DiagWriteLog( s_phaseBuf );
}
/* 【r131】drv.c 侧也用同一个宏名(display.c 里另有一份同名定义, 两边都指向
 *   上面这个无条件函数)。本文件里只在 VxdTraceDump 的事件边沿用几行。 */
#define PHASEV( m, v )  DiagHexPhase( m, (uint16_t)(v) )
#else
#define PHASEV( m, v )  ((void)(m), (void)(v))
#endif

/* --- 与 VxD 的连接状态 --------------------------------------------------- */
static uint32_t g_vxdApi = 0;       /* PM API 入口(选择子:偏移，存成双字) */

/* --- 【r100】回调路径安全用的文件级状态(定义提前: VbeExecBios/PushState
 * 在本文件前部就要用它们; 说明见后部"策略字节/内存环/回调哨兵"一节) ---- */
static uint8_t  s_polVddEdx = 0;     /* VddVirtualize 的 VxD 侧一半        */
static uint8_t  s_polVesa   = 0;     /* VesaPolicy                         */
static uint8_t  s_polHires  = 0;     /* HiresAns                           */
static uint8_t  s_polRestore= 0;     /* Restore(VxD 不读, 只为布局对齐)    */
/* 【r101】新增两个策略字节(VBESHAREDSTATE 40 → 44 字节) */
static uint8_t  s_polBank   = 1;     /* VddBank: GET_VDD_BANK 拒绝窗口化
                                        4 平面虚拟化(默认 1 = 拒绝)       */
static uint8_t  s_polInt10  = 0;     /* Int10Mode: 48 号是否吃掉设模式      */
/* 【r114】VgaFix —— 用 VBESHAREDSTATE 里原本保留的 42 号字节(结构仍是
 * 44 字节, 所以新旧 minivdd.vxd 可以混搭部署: 老 VxD 只是不读它)。
 * 它到 VxD 侧就是 policyVgaFix, 由 POST_HIRES_TO_VGA(5) 的 文本态修补 读:
 *   else 0 = 一个寄存器都不写(与 r113 行为逐字节相同);
 *        1 = 补写文本取指/平面那 8 个寄存器(见 minivdd.asm 的长注)。
 * 为什么默认 0: 这是一次【在 VDD 切屏事务里写硬件】的动作, 与前几轮
 * "先在 ring 3 里证明无害"的纪律不同 —— 必须由用户显式开、单变量对比。*/
static uint8_t  s_polVgaFix = 0;
static uint8_t  s_polInt10Fix = 0;   /* r144: 48 号里 72h/73h→03h */     /* VgaFix(r114)                       */
static uint8_t  s_polHiresFix= 0;    /* r146: mini-VDD 存/写回模式字节 (4 号存 / 7 号写回, 默认 1) */
static uint8_t  s_polMemCFix = 0;    /* r147: 持屏期 MemC 六寄存器补写 (默认 1) */
/* 【r116】Dispi —— 用 VBESHAREDSTATE 里原本保留的 43 号字节(结构仍是
 * 44 字节, 所以新旧 minivdd.vxd 可以混搭部署: 老 VxD 只是不读它)。
 * 它到 VxD 侧就是 policyDispi, 由 MiniVDD_Virtual1CE / MiniVDD_DispiTrapOn
 * / MiniVDD_DispiTrapOff 读:
 *   0 = 不装 I/O handler、不开全局陷阱(与 r115 行为逐字节相同);
 *   1 = 【r116 默认】把 Bochs/VBox 的 VBE_DISPI 端口 1CEh/1CFh 虚拟化 —— 
 *       Windows 拥有 CRTC 时, 非 Windows VM 对这两个端口的读写被吃掉。
 * 为什么默认 1: 这是【照抄参考实现】的一步(boxv9x 的 vxd/boxvmini.asm、
 * vmdisp9x 的 virtual_0x1ce()), 两个独立实现在同一类 VBE 上用同一手法
 * 修掉同一个症状("窗口 DOS 花屏"), 而且它是 DDK 明文要求的形态
 * (Install_IO_Handler + ENABLE/DISABLE_TRAPS 里开关全局陷阱)。
 * 关掉它就是回到 r115。                                                  */
static uint8_t  s_polDispi   = 1;    /* Dispi(r116)                       */
static uint8_t  s_inVddCallback = 0; /* 回调哨兵: VbeExecBios 的护栏依据   */
/* --- r102: 日志通道闸门与"被丢弃的行"计数 -------------------------------
 * 三个计数由 vddapi16.asm 的 DIAGWRITELOG 无条件累加(它不发 INT 21h 就返回,
 * 所以"记一笔"这件事本身不会再把我们拖回 DOS)。定义放在本文件, 因为
 * VxdTraceDump 也读它们; display.c 侧只做 extern 声明。
 * 闸门哨兵 g_logBlocked 与 s_inVddCallback 由 VxdSetInCallback 同步改写。  */
uint16_t DiagSkipInDOS  = 0;         /* InDOS 读到"忙"的次数(含被拦与自愈放行) */
uint16_t DiagSkipReent  = 0;         /* 日志例程重入 → 丢弃                */
uint16_t DiagSkipCb     = 0;         /* 主 VDD 回调窗口 → 丢弃             */
uint16_t g_logBlocked   = 0;
/* 【r166】日志总闸 —— 与 g_logBlocked 同一套路(汇编侧 extrn 同一个单元)。
 *   debug/MIN 档 = 1, release 档 = 0。汇编的两个入口(探针建文件、每一行
 *   落盘)都先看它, 于是 release 档不产生 OMNININE.LOG。
 *   为什么需要它: ./165 的 release 档仍然写出 906623 字节的骨架日志
 *   (r131 的 DiagHexPhase 是【无条件】的第三种通道), 而"release 驱动不写
 *   任何日志文件"是本项目的既定目标。它同时也是"三个构建档共用一份源码"
 *   这个设计的最后一处漏洞。 */
#if defined(VBE9X_DIAG) || defined(VBE9X_DIAG_MIN)
uint16_t g_logEnable    = 1;
#else
uint16_t g_logEnable    = 0;
#endif         /* 非 0 = 禁一切日志 I/O(r102)        */
/* 【r103】第②道闸门(InDOS)的自愈状态 —— 由 vddapi16.asm 的 DLOGGUARD 维护。
 * r102 的 debug 会话日志只剩 PROBE 一行, 根因有两条: 出口无条件 dec 重入
 * 计数(已被拦下的那次没有 inc 过, 却照样 -1, 于是计数 = 0xFFFF, 第③道
 * "≠0 即忙"从此永远成立), 以及 InDOS 标志在 Win9x 里长期非 0(闸门② 把
 * 每一行都拦掉)。现在: 只有登记过的路径才释放; 且连续 8 次读到"忙"就判定
 * 该标志不可用并永久停用第②道 —— 停用这件事本身会由下面 VxdTraceDump
 * 的 "LOG gate" 三行打出来。
 * 【为什么这三个量在 C 侧定义、汇编侧 extrn】它们是两侧共读的同一份存储:
 * 汇编片内再定义一份的话, C 读到的会是另一个永远不动的单元(静默死开关)。
 * 初始值: Ok=1(第②道启用, 首次调用时才去取 InDOS 远址)、Val=FFFF(未读)、
 * Busy=0。Ok 被改成 0 只有两个来由: AH=34h 取不到远址, 或连续 8 次"忙"。 */
uint16_t DiagInDosOk    = 1;         /* 1 = 第②道启用(首次调用时才去取远址) */
uint16_t DiagInDosVal   = 0xFFFF;    /* 首次读到的 InDOS 字节(FFFF=未读)   */
uint16_t DiagInDosBusy  = 0;         /* 当前连续"忙"次数                   */
static uint8_t  s_safeToSetMode = 0; /* POST_VGA_TO_HIRES 计数变化后的信号 */
/* r110-marker: trace flood control + LogBoot + BIOS-refusal latch */
/* 【r110】BIOS 调用被护栏拒绝的标志(取走即清)。
 * 【为什么需要它】./109 的 t2 给出了本次会话最要紧的一条读数: 一次
 *   Alt+Enter 往返里, 主 VDD 会连续调用 mini-VDD 的 03/09/0F 三个服务
 *   数百次(银行搬屏), 我们的 ring-3 代码随时可能【正落在主 VDD 的
 *   回调窗口里】。此时若延迟重建被触发, VbeExecBios 会按 r100 的硬护栏
 *   拒绝(那是防 r99 三次死机的护栏, 必须保留), SetDisplayMode 于是
 *   失败 —— 而 r109 的代码在失败分支直接 return, 把 bRestorePending
 *   清掉了: **一次"在回调里被拒"的重建 = 这个请求被永久丢弃**,
 *   屏幕只能等下一次边沿(再按一次 Alt+Enter)或 DOS 会话收尾才回来。
 *   display.c 的 ReArm 分支拿这个标志判"是护栏拒绝还是真失败", 前者
 *   立刻重新挂起, 等下一个安全时机重试。                            */
static uint8_t  s_biosRefused = 0;
/* 【r111】回调期"BIOS 预算"。
 *   r100 的硬护栏是"回调期一律拒绝"(x230 三次死在回调里那次嵌套 4F02)。
 *   r111 把微软文档规定的恢复路径搬回回调里(display.c 的 CbRestore), 于是
 *   护栏改成: 默认仍拒绝(预算 0), 但官方恢复路径可以显式领一份【有界】
 *   预算。为什么不是一个"一次性"开关: 官方恢复内部的一次 SetDisplayMode
 *   要做 4F02 + 0Fh 探针 + 4F01 三次 BIOS 往返 —— 只放行一次等于把恢复
 *   做一半。预算用完立刻作废, 于是回调之后的任何代码、以及任何嵌套第二
 *   层, 仍然一律被拒。用了多少也记账(见 g_cbBiosUsed)。             */
static uint8_t  s_biosAllow = 0;
/* 【r111】BIOS 直通的"同时只有一次"闩 —— 见 VbeExecBios 里的使用点。
 *   为什么需要: 请求块 g_xreq 是【共享静态量】, 而嵌套 V86 执行期间完全
 *   可能被主 VDD 的回调打断(回调里 r111 也会发 4F02)。两层同时用同一个
 *   请求块 ⇒ 外层拿到的会是内层的结果。r100 靠"回调里一律拒绝 BIOS"顺带
 *   排除了这种嵌套; r111 开了回调这条路, 就得显式闩住。               */
static uint8_t  s_biosBusy = 0;
uint16_t g_cbBiosBusy = 0;              /* r111: 被闩拒了几次(可读证据) */
/* 【r138】被 r100 回调护栏拒绝的次数 —— 与 g_cbBiosBusy 一起, 把"这次
 *   调用为什么没出去"从一句 `exec ax=FFFF` 拆成两种可分辨的原因。
 *   两者都不是硬件失败: 前者是"上一次直通还没收尾", 后者是"我们正处在
 *   VDD 回调窗口里"。见 VbeExecBios 里两处的注。 */
uint16_t g_cbGuardRefuse = 0;
/* 【r119】DispiIdProbe —— 是否允许"写索引 0 再回读"来确认 DISPI 存在。
 *   定义在 display.c(与其余 INI 开关同一处), 这里只引用。
 *   为什么要有这个开关: 这是本驱动【唯一】会往 1CEh/1CFh 的【数据端口】
 *   写东西的探针路径的入口(而且只写索引 0 —— 它不参与任何扫描格式)。
 *   万一在别的机器上 1CEh/1CFh 被某个我们没预料到的设备占用, 用户可以用
 *   system.ini 的 DispiIdProbe=0 把这一枪彻底关掉, 回到 r117/r118 的
 *   "只读探针"形态。默认 1: 没有它, "永远问不出结论"的状态就会重现。 */
extern uint16_t g_runDispiIdProbe;
/* 【r120】DispiPhys —— 是否允许 ring 0 的物理 I/O 读 VBE_DISPI(1CEh/1CFh)。
 *   定义在 display.c(与其余 INI 开关同一处), 这里只引用。
 *   默认 0(关): 关着时一个字节都不写、一次物理 I/O 都不发。
 *   打开后由 mini-VDD 用 VDD_Do_Physical_IO 绕开 VM 的端口虚拟化去读那
 *   六个寄存器 —— 这是唯一能分开"端口不存在"与"ring 3 的 I/O 没抵达
 *   硬件"的通路(见 minivdd.asm 的 MiniVDD_DispiPhysRead 长注)。 */
extern uint16_t g_runDispiPhys;
/* 【r130】CrtGeo —— 是否允许"扫描几何与帧缓冲不一致时强制重新走一遍模式
 *   设置"。定义在 display.c(与其余 INI 开关同一处), 这里只引用 —— 由
 *   VxdReadCrtPropRead 一族与 CrtGeometryLooksRight 一起被 drv.c 与
 *   display.c 共用。默认 1(见 display.c 变量声明处的长注)。            */
extern uint16_t g_runCrtGeo;
/* 【r111】回调窗口里真正发出去的 BIOS 调用次数 + VM 自检拒绝次数。 */
static uint16_t s_cbBiosUsed = 0;
uint16_t g_cbBiosUsed = 0;
uint16_t g_cbVmRefuse = 0;
void __far __pascal VxdAllowBios( uint16_t budget )
{
    s_biosAllow = (uint8_t)budget;
    s_cbBiosUsed = 0;
}

void __far __pascal VxdDenyBios( void )
{
    s_biosAllow = 0;                    /* 官方路径没用到就作废 */
}
/* 【r112】g_inRebuild —— "我们自己的那一段模式重建正在跑"。
 *   见 drv.h 的声明注: ./111 t5 实锤"主 VDD 的回调切进了我们自己的重建
 *   序列中间", 于是护栏拒掉了我们自己序列里的第二次 BIOS 调用。这一位
 *   就是那个事实的修复: 置起期间 BIOS 直通放行(它是同一条序列的后续调用),
 *   而回调侧同时被 display.c 告知"别在这儿做官方恢复"。                 */
static uint8_t s_inRebuild = 0;
void __far __pascal VxdSetRebuild( uint16_t on )
{
    s_inRebuild = on ? 1 : 0;
}
uint16_t __far __pascal VxdGetRebuild( void )
{
    return (uint16_t)s_inRebuild;
}
uint16_t __far __pascal VxdTakeBiosRefused( void )
{
    uint16_t v = s_biosRefused;

    s_biosRefused = 0;
    return v;
}
static uint8_t  s_evtPost = 0, s_evtPreH2V = 0, s_evtPreV2H = 0, s_evtPostSeen = 0;
/* 【r131 删除】这里原本是 r129 的两个静态变量 —— "HiResClockFix 两个计数器"
 *   的上一次取值。删除理由(实证, 见 VxdTraceDump 里那段长注):
 *   blob 的 `VBE_TR_Evt` 区只有 4 字节(+0..+3), `VBE_TR_EVT_HIRES_CLK_FIX/NOP`
 *   = +4/+5 实际落在 `VBE_TR_Counts[0]/[1]`, 读到的永远是"0 号功能被调用次数",
 *   是一台【假仪器】。它已经被"同一事件边沿直接读 SR01/GCR06/AR10/CR17h"
 *   取代 —— 直接读数不需要"上一次取值"这种状态。 */
/* 【r101】mini-VDD 切屏状态机(blob 头 204..207)的上一份取值, 变化才落盘 */
static uint8_t  s_stFlags = 0xFF, s_stI10Cnt = 0xFF;
static uint16_t s_stI10Ax = 0xFFFF;
/* 【r109】状态位的"最近一次采样值"(不是"上一份变化前值")。
 * 【为什么要单独留一份】s_stFlags 的语义是"上一次落盘时的值", 它的比较
 *   对象是 s_stFlags 自己; 而 display.c 需要的是"现在主 VDD 把硬件当成
 *   VGA 通路还是 hires", 与"变没变"无关 —— 例如入口侧补救要判"这次会话
 *   是全屏(硬件真被切走)还是窗口化(只是虚拟化)", 依据就是这一刻的
 *   bit0。r108 之前 display.c 看不到这个量, 只能拿 Hw 探针(0Fh)
 *   间接猜(而那是主 VDD 的模式记账, 不是这一位)。
 * 【语义边界】它只在 VxdTraceDump(我们的 ring-3 入口)被刷新, 所以是
 *   "最近一次采样", 两次采样之间主 VDD 若自己开关过, 这里看不到; 因此
 *   它只配当"抑制条件"(见到 VGA 位就不动手), 不能当"必须动手"的依据。*/
static uint8_t  s_stBitsNow = 0;
/* 【r104】DOS 虚拟化会话"收尾边沿"----------------------------------------
 * 【信号】VBE_TR_State 的 TRAPS 位(bit1)下降沿。
 *   ./103 四份日志的状态机完全同形: 会话前 bits=0; 会话期间 bit1 一直为 1
 *   (由 ENABLE_TRAPS(13) 置位, 只有 DISABLE_TRAPS(14) 清它); 会话结束后
 *   回到 bits=0。而 bit0(VGA 通路)在窗口化会话里被 ACCESS_VGA_MEMORY_MODE(11)
 *   / ACCESS_LINEAR_MEMORY_MODE(12) 反复开关, 不能用来划分会话边界 ——
 *   只有 bit1 是"整场会话持续为 1"的那一位。
 * 【为什么必需】主 VDD 结束一次 DOS 会话时【本应】回调我们的 ResetHiResMode
 *   (= RestoreDesktopMode)让我们把桌面重建起来; 而 ./103 的 B0/B1FS/B1WS
 *   三份日志里那个回调一次都没被调用(只有 B2 调了一次)⇒ 桌面再也回不来,
 *   正是用户报的"DOS 全屏切不回窗口"。这条边沿是驱动侧唯一能自己拿到的
 *   收尾信号, 与主 VDD 是否记得回调无关。
 * 【保守措施】① 只认"先置起、后落下"的真边沿(初始值 0, 首次采样不会误触发);
 *   ② 每次 Windows 会话内最多上报 8 次, 免得开关解读错时把重建打成风暴;
 *   ③ 每次触发都打一行 TRC evt sessEnd*, 日志上一眼能数出触发了几次。  */
static uint16_t s_vgaSessEnd  = 0;   /* 待取走的会话收尾请求(0/1)          */
static uint8_t  s_prevTrcFlags = 0;  /* 上一份状态位(算边沿用)              */
static uint8_t  s_vgaEdgeN     = 0;  /* 本会话已上报次数(上限 8)            */
/* 【r142】“盒子拿走屏幕”边沿 —— VBE_TR_State 的 VGA 位(bit0)上升沿。
 *   【为什么必須再要一条】r100 的安全门只认 7 号(POST_VGA_TO_HIRES)的
 *   计数变化 = “回 Windows”那一趟的收尾。而用户这轮要修的方向
 *   恰恰相反(窗口 -> 全屏): 那一趟里 7 号根本不会被调, 于是入口侧的
 *   重建永远只能靠末尾那条“八次兜底”撞出来 —— ./141 的
 *   `no POST_VGA_TO_HIRES signal -> fallback` 就是它, 而它撞在主 VDD
 *   写完 DOS 模式之前(行 11007 vs 11118)。
 *   【信号的出处】mini-VDD 的 TraceStub_5(POST_HIRES_TO_VGA, 即 DDK 第 5 步)
 *   会 `or byte ptr [TraceState], VBE_TR_ST_VGA`, 而 TraceState 就在 blob 的
 *   VBE_TR_State(204) 里 —— 它是 mini-VDD 自己维护的状态机,
 *   不需要改汇编、也不需要新字节。
 *   【为什么只能当“可以动手”而不能当“必須动手”】它是采样到的边沿,
 *   且窗口化 DOS 会话里 ACCESS_VGA_MEMORY_MODE(11) 会反复开关它
 *   (r104 已记录)。所以本轮只用它「解除等待」, 不用它「发起重建」。 */
static uint8_t  s_prevVgaBit    = 0;
/* 【r144】全屏 DOS 盒持屏期(4 号武装 / 7 号解除) + 持屏期指纹。
 *   【为什么取代 r143 的 S1/S2】那四个字节整场日志里一直是 0
 *   (见 R144_DELIVERY §2): 它们挂在 5 号与 35 号上, 而那两个
 *   钩子在盒子真正持屏的那段时间里没有被调到(或被闸门挡住)。
 *   本轮改成两组计数器 + 一拍硬件指纹, 全部在 48 号
 *   (已证实会在全屏期间发生)里采, 并且每个状态边沿
 *   【无条件】落盘 —— 静默的仪器等于没有仪器。            */
static uint8_t  s_boxArmed    = 0;   /* 原始字节: 0/1/2(见 VxdBoxArmedNow 注) */
/* 【r145】边沿节流与【曾经失锁】的证据。
 *   r104..r144 用"每次开机最多 8 次"当风暴保护, 而它是【永久性】的:
 *   用满之后 s_vgaSessEnd 再也不置 ⇒ 显示驱动永远收不到收尾边沿。
 *   ./144 的硬证据: 日志 40502 行
 *     TRC evt sessEnd: cap 8 reached, further edges ignored
 *   而其后 16700 行(约 2.8 分钟、九次 alt+enter/退出)里
 *   SetDisplayMode 与 "DeferredRestore: enter" 各 0 次 —— 用户看到的
 *   "进一步恶化成无法恢复的青色花屏"就是这一段。
 *   本轮改成: 边沿一律置请求(永不静默丢弃), 只在两种情况下
 *   节流密度: ①很密的重复边沿(当年设上限的唯一理由);
 *   ②连续 8 次都没有一次成功重建 —— 而②的计数器会在
 *   任何一次成功重建后清零(VxdEdgeBudgetReset)。
 *   两个计数器分开, 因为"开盒子立刻关掉"这种动作两条边沿
 *   只隔几个 tick, 共用一个会把第二条挡掉 —— 而第二条正是
 *   "退出到桌面"的修复点。被挡下的次数逐个累加并落盘。 */
/* 【r146 —— r145 的节流整体撤除, 见上面那段长注的末段】
 *   r145 用"D D I 调用次数"当节流单位, 而本机 D D I 调用极稀:
 *   ./145 里两次会话边沿之间只减了 39 次(0x2000 - 0x1FD9), 而
 *   0x2000 = 8192 次按这个速率要 30 分钟以上 —— 事实上等于把
 *   "会话收尾"这条边沿永久关掉。硬证据: 日志 14177 行
 *     TRC evt sessEnd held (edgeHold>0)=1FD9
 *   之后 3200 行(约 2.5 分钟、两次盒子往返)里 `SetDisplayMode:` 与
 *   `DeferredRestore: enter` 各 0 次 —— 正是用户报的
 *   "几乎只有第一次窗口运行 command 才能恢复正常 gui, 之后就都是花屏"。
 *   本版不再对边沿做任何抑制: 边沿 = 主 VDD 的切屏事务 = 用户动作的
 *   粒度, 它天然不可能形成风暴。真正要防的是"重建还没执行完又来一次",
 *   而那件事由待办标志(wArmedEnd/wArmedEntry/bRestorePending)与
 *   HwIsOursNow 那道门管, 与"边沿计数"无关。
 *   两个 skip 计数器保留: 它们的值恒为 0, 这就是"没有静默丢弃"的读数。 */
static uint16_t s_edgeSkipEnd = 0;   /* 【r146】被挡下的收尾边沿(恒 0)    */
static uint16_t s_edgeSkipEnt = 0;   /* 【r146】被挡下的入口侧边沿(恒 0)  */
/* 【r146】硬件指纹从 4 字节长到 16 字节(13 个模式/寻址寄存器 + 相位),
 *   布局见 vbeshare.h 的 VBE_TR_FS_*。缓存整块以供落盘与"变化才写"。 */
static uint8_t  s_fboxSnap[16];
/* 【r146】hi-res 快照/回放记账(blob 248..251 的前四个字节)。 */
static uint8_t  s_hires[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
/* 【r151】s_own[24] = blob 的 VBE_TR_Own 块(屏幕所有权 / 特殊 VM /
 *   35 号的 AL)。布局见 vbeshare.h 的 VBE_TR_OWN_*。
 *   【它回答什么】DDK《RESTORE_REGISTERS (Function 9)》:"ESI: Contains the VM
 *   handle for the MemC owner VM. ECX: Contains the VM handle for the CRTC
 *   owner VM." 而《Virtual Display Minidriver》说明这两个所有者就是"画面
 *   外观"与"CPU 看到的内存"的分工 —— 全屏花屏的主线判据就在这里。
 *   【分类码】0 未知 / 1 Windows / 2 盒子(4 号入口 EBX) / 3 其它(两个伪 VM)。 */
static uint8_t  s_own[VBE_TR_OWN_LEN];
/* 【r152】s_probe[32] = blob 的 VBE_TR_Probe 块(取指基址探针 + 修复记账)。
 *   布局见 vbeshare.h 的 VBE_TR_PR_*。VxD 侧同名同值(Probe 是逐字节映像),
 *   所以这里不像 s_own 那样需要两套名字。                             */
static uint8_t  s_probe[VBE_TR_PR_LEN];

/* 【r150】+4..+7 = 48 号分类计数(见 vbeshare.h 的 VBE_TR_HR_*)。
 *   VxD 侧一直是 HiresSpare 的四个零字节, api_gettrace 一直在搬 ⇒ 本轮只把
 *   "搬"变成"读", 传输侧一个字节都没动。 */
/* 【r146】上一拍落盘的 (计数器4 + 指纹16 + 记账4 [+ r147: 补写记账16])
 *   副本 —— 一模一样就不再写第二遍。./145 的 17388 行里有 7840 行是这段
 *   块(45%), 而其中绝大多数是"值一个字节都没变"的重复。
 *   【r147】键长 24 → 40: 加进 MemC 那 16 字节。缺了它, ./146 里那种
 *   "计数器在长、指纹不变"的重复会话会被整块跳过(六次落盘全是同一份值,
 *   13 次切屏只剩 4 行证据)。 */
#define VBE9X_FBOX_KEYLEN (44 + VBE_TR_PR_LEN)  /* r152: 44+32=76; r153: 44+248=292
                                                 *   —— 写成算式, 再也不用手改 */
static uint8_t  s_fboxLast[VBE9X_FBOX_KEYLEN];
static uint8_t  s_fboxHave = 0;
/* 【r147】每个采集点各占一个 16 字节槽(blob 256..351) —— ./146 证明所有
 *   采集点共用一块时"最后一个写的人赢", 4/5/7 三拍整条因果链被抹掉。 */
static uint8_t  s_phase[VBE_TR_PS_SLOTS * VBE_TR_PS_LEN];
/* 【r147】持屏期"文本取指/平面"补写的记账(blob 352..367): 见得多而写得少
 *   = 判据还在挡; 写了而 stillBad>0 = 写没落进硬件; 写了且 stillBad=0
 *   而画面仍花 = 目标值本身不对。 */
static uint8_t  s_memc[VBE_TR_MC_LEN];
/* 【r108】"主 VDD 完成一次 hires→VGA→hires 往返"边沿 ----------------------
 * 【为什么加它 —— ./107 的日志把 r104 那条收尾边沿的时机读出来了】
 *   t3a 的时间轴(NUL 剥壳行号): T=000EF7D7 会话开始(TRAPS 置起、i10 ax=0003)
 *   → T=000EF7FC → T=000EF820 处 postVH/preHV/preVH 三个事件计数同时 +1
 *   → T=000EF83B 会话收尾(TRAPS 落下)。也就是: **主 VDD 的 4/5/6/7 号一整趟
 *   往返发生在会话【进行中】(开始后约 2.5 秒), 而不是会话收尾那一刻**;
 *   r104 的 s_vgaSessEnd 钉在 TRAPS 下降沿上, 于是那次重建比往返晚了约
 *   1.5 秒 —— 用户看到的就是"运行 command 花屏, exit 后才恢复"。
 * 【DDK 语义】7 号(POST_VGA_TO_HIRES)的原文含义是"主 VDD 的切屏事务收尾、
 *   硬件归 Windows 了"。对 VBE 这类主 VDD 自己不会设的模式, 收尾之后
 *   **必须由 mini-VDD/显示驱动把高分辨率模式重新建立起来** —— 本驱动的
 *   mini-VDD 在 4/5/6/7 上只记账不动作(那是对的: 回调上下文里绝不碰
 *   BIOS), 所以这件事只能在 ring 3 的 DDI 上下文里做, 而触发点就是这条
 *   事件计数。VBE_TR_Evt[0] 正是 7 号的累计次数(见 minivdd.asm 的
 *   TraceStubState 7), 它一变就说明"刚刚往返过一次"。
 * 【与 s_safeToSetMode 的关系】同一个字节既当"可以安全设模式"的闸门(旧),
 *   又当"该去设一次"的触发(新): 旧代码只在已经有人置了 bRestorePending 时
 *   才消费它, 没人置就白等 —— 这就是 r104..r107 在会话进行中什么都不做的原因。
 * 【保守措施】① 只在"本 Windows 会话里确实见过 VGA/TRAPS 位"之后才认这条
 *   边沿(s_seenVgaSess), 免得开机期 VDD 自己的一次往返就触发一次重建;
 *   ② 每次开机最多 8 次; ③ 每次触发都落 V2H 三行日志, 数得出来。        */
static uint8_t  s_v2hEdge      = 0;  /* 待取走的"刚往返过"请求(0/1)        */
static uint8_t  s_seenVgaSess  = 0;  /* 见过 VGA/TRAPS 位(粘滞)            */
static uint8_t  s_v2hN         = 0;  /* 已上报次数(上限 8)                 */
/* 【r108】"DOS 会话开始"边沿(TRAPS 上升沿)—— EntryHeal 用。
 * 【为什么还留一条入口侧的补救】./107 的日志只能证明"主 VDD 那趟
 *   hires→VGA→hires 往返之后没有任何人把高分辨率模式重新建立起来", 它
 *   【不能】证明屏幕是在哪一刻开始花的: 往返发生在会话开始后约 4 秒
 *   (t3a: 开始 T=EF7D7、往返 ~T=EF820、收尾 T=EF83B), 若用户看到的
 *   花屏是从"打开 DOS 盒"那一刻就开始的, 那就说明硬件在【入口】就已经
 *   被 DOS VM 的 ROM BIOS 写坏了(主 VDD 只虚拟化标准 VGA 端口, 厂商/PCI
 *   扩展寄存器不在它的管辖内 —— 见 DOSBOX_SWITCHING_REFERENCE.md 2.11),
 *   而 VhEdge 只能把屏幕在往返那一刻救回来(晚了几秒)。
 *   这条边沿给入口侧一个【激进】选项: 会话一开始就请求一次重建。
 *   【为什么默认 0】它会在主 VDD 正给 DOS 盒建画面的时候插一次 4F02,
 *   时机比 VhEdge 粗暴得多(等于"DOS 盒一开就重设桌面模式"); 只作为
 *   A/B, 不默认开。                                                      */
static uint8_t  s_entryEdge    = 0;  /* 待取走的"会话刚开始"请求(0/1)      */
static uint8_t  s_entryN       = 0;  /* 已上报次数(上限 8)                 */
/* 【r135】边沿方向判定的三个量(设计说明见 drv.h 的 VxdEdgeDirNow 长注) ----
 *   s_evtDir    : 事件计数器的【增量方向】, 每次计数器变化时刷新。
 *                 bit0 = 见到 POST_VGA_TO_HIRES(+0) 增量 ⇒ 回 Windows 的证据;
 *                 bit1 = 见到 PRE_HIRES_TO_VGA(+1)  增量 ⇒ 盒子拿屏的证据;
 *                 两个都涨(= 一次事务里走了个来回)时它判不出, 让位给下面那条。
 *   s_lastSetDir: 48 号(PRE_INT_10_MODE_SET)最近一次"为 VM 设了什么模式" ——
 *                 【有序】证据。本机实测两个取值:
 *                   0x0003(标准 BIOS/VGA 模式, <= 0x13) ⇒ 屏幕交给 DOS 盒 = 2;
 *                   0x0072(扩展/伪模式, > 0x13)         ⇒ 设回高分辨率   = 1。
 *   s_edgeDirNow: 在 ENTR 边沿(TRAPS 上升沿 = 一次切屏事务的收尾)上【定格】
 *                 的结论, 由 VxdEdgeDirNow 读走。三个都是只读投影。        */
static uint8_t  s_evtDir      = 0;
static uint8_t  s_lastSetDir  = 0;
static uint8_t  s_edgeDirNow  = 0;
/* r108-marker: POST_VGA_TO_HIRES event counter is the r108 trigger */
/* 【r102】日志闸门丢弃计数的上一份取值(汇编侧无条件累加, 这里只读) */
static uint16_t s_skipInDos = 0xFFFF, s_skipReent = 0xFFFF, s_skipCb = 0xFFFF;
/* 【r103】InDOS 闸门自愈状态的上一份取值 */
static uint16_t s_dosOk = 0xFFFF, s_dosVal = 0xFFFF;
/* 【r134 容量 16 -> 32】本环是"文本日志被闸门丢掉时唯一还活着的证据通道"
 * (./133 实测一次会话丢 1600 行), 而 r134 往每次回调里多塞了"语义判据 +
 * 最多 4 个不等的寄存器(每个两条)"≈ 10 条 —— 16 格会在一次回调里被填满,
 * 把后面的 CALLBACK_ENTER/GATE_HOLD 挤掉。32 格的代价是 64 字节静态数据。*/
/* 【r136】容量 32 → 64。./135 的账: 从开机到"进全屏那一刻"共 34 条事件,
 *   而环只有 32 格 —— 最早的 2 条会先被挤掉, 也就是【这一轮最想看的
 *   那几条】。64 格 = 一次完整 Alt+Enter 往返的事件量(实测一轮 10~12 条,
 *   加上 VGA 状态机翻转)留出 4 倍余量, 而字节数只多 64 字节。
 * r136-marker: CB ring capacity doubled (32 -> 64) after ./135 overflowed it. */
static uint16_t s_cbRing[64];        /* 回调期事件环(只写内存)             */
static uint8_t  s_cbRingN = 0;
/* 【r136】溢出计数(见 drv.h 的 CB_EVT_RING_DROP): 满了不再静默丢弃。 */
static uint8_t  s_cbRingDrop = 0;

/* 推送用的共享状态实例：放静态数据段，保证有稳定的 far 地址可取。
 * 【为何不用栈】DPMI 换算线性地址时需要选择子基址，栈/堆的 far 指针
 * 同样可用，但静态缓冲区生命周期清晰、可被多次推送复用。             */
static VBESHAREDSTATE g_state;

/* 【__HShift 的归属】Watcom 运行库的巨型指针运算辅助 __PIA/__PIS
 * (display.c 清屏循环的 __huge 指针算术会调到它)需要外部符号
 * __HShift——huge 指针归一化的位移量: 实模式 12, 保护模式 3。
 * 该符号在 clibm.lib 中只由 Win16 EXE 启动模块 wstart 提供; 把它
 * 拉进链接会携带自己的起始地址(与本驱动 vddapi16.asm 的 END
 * DriverInit 冲突, wlink 报 E2030)并级联引用 WinMain/main(报
 * E2028 main_ undefined)。16 位显示驱动恒运行于保护模式, 这里
 * 自行定义为 3 即可切断这条依赖链——wstart/wmain 不再进入链接,
 * 这正是预期结果(驱动不需要 CRT 启动代码)。Watcom C 命名规则:
 * 变量 _HShift → 目标文件符号 __HShift(变量加前导下划线)。
 * 【r76 现状】清屏已改用汇编 FBCLEAR(32 位偏移 + rep stosd), 本驱动
 * 里不再有 __huge 指针算术; 本定义保留作链接保险(万一库内还有别的
 * 路径引用 __PIA), 代价 1 字节, 无行为影响。                     */
uint8_t _HShift = 3;

/* --- 中断调用的汇编封装 ----------------------------------------------------
 * 【为何把寄存器包放静态区】Win16 DLL 以 SS!=DS 编译(MSVC /Awnd,
 * Watcom -zu)：近指针相对 DS 寻址。栈变量的地址在 SS 段，一旦被截成
 * 近指针就是野指针 —— 这是 16 位 DLL 里最隐蔽的崩溃源之一。静态存储
 * 期对象位于 DGROUP，近指针永远合法；Win9x 的 16 位显示驱动本质单
 * 线程，静态缓冲区无重入问题。
 *
 * 【为何不用 clibm int86/int86x】(./6 构建实锤)clibm 的 int86x 把
 * 寄存器包拷进【栈】上缓冲区后调 _DoINTR_, 而 _DoINTR_ 用
 * "mov ds,cx"(cx=int86x 入口的 DS)去寻址该缓冲区 —— 只有 DS==SS 的
 * EXE 目标下栈缓冲才经 DS 可达。本驱动 -zu: DS=DGROUP, SS=调用方栈
 * (引导期是 KERNEL 的大初始化栈) → DoINTR 用 DGROUP 选择子寻址栈
 * 偏移, 轻则读垃圾(此前 DiagLog 静默失败即此因), 重则段界限 GP
 * (蓝屏 0d:0327:00000907 = _DoINTR_ 内首条 [bx] 访问)。
 * vddapi16.asm 的 EmuInt86/EmuInt86x 全程只用 DS=DGROUP 寻址静态
 * 寄存器包, 只实现本驱动用到的 10h/21h/2Fh/31h 四个中断。
 * 寄存器包布局与 Watcom union REGS 一致(x.ax..x.di,+12 cflag,+14
 * flags), g_regs/g_sregs 用法不变。                                  */
extern void __far __pascal EmuInt86( WORD intno, union REGS __near *rp );
extern void __far __pascal EmuInt86x( WORD intno, union REGS __near *rp,
                                      struct SREGS __near *sp );
/* VxDCallApi 定义在本文件后部(VxD 通信层); 前部 VbeExecBios(直通
 * 生产路径)也要调用它, 此处前向声明避免隐式声明的返回类型冲突。 */
static uint16_t VxDCallApi( uint16_t func, uint32_t linear, uint16_t size,
                            int far *pOk );

static union REGS   g_regs;
static struct SREGS g_sregs;

/* r412: AH=25h 需要 DS:DX, 而 EmuInt86x 只传 ES —— 自己设 DS 再发 INT 21h */
static void __far SwSetVec( BYTE n, WORD seg, WORD off );
#pragma aux SwSetVec = \
    "push ds" "mov ds, cx" "mov dx, bx" "mov ah, 25h" "int 21h" "pop ds" \
    parm [al] [cx] [bx] modify exact [ax dx];

/* r412: INT 2Fh AX=4001h/4002h observer (count only) */
static void (__interrupt __far *s_oldInt2F)(void) = 0;
static WORD s_sw4001 = 0, s_sw4002 = 0, s_swInst = 0;
static WORD s_swOldSeg = 0, s_swOldOff = 0, s_swNewSeg = 0, s_swNewOff = 0;

#define CALL_INT(n)     EmuInt86((n), (union REGS near *)&g_regs)
#define CALL_INTX(n)    EmuInt86x((n), (union REGS near *)&g_regs, \
                                  (struct SREGS near *)&g_sregs)
/* EmuInt86x 只消费 g_sregs.es(其余段字段忽略), 各调用点自行赋值,
 * segread 填充整个 SREGS 已无必要。                                  */
#define SEGREAD()       ((void)0)

/*==========================================================================
 *
 *  DPMI 辅助函数
 *
 *  【为何需要 DPMI】
 *  16 位保护模式代码手里只有"选择子:偏移"。而：
 *   - 交给 VxD 的缓冲区地址必须是线性地址(契约见 vbeshare.h)；
 *   - 交给 V86 BIOS 的缓冲区必须落在低 1MB(实模式可寻址)；
 *   - 帧缓冲(LFB)是【物理地址】，ring 3 要访问它必须先让 DPMI
 *     把它映射进本 VM 的地址空间(DPMI 0800h)。
 *  三件事都靠 DPMI(int 31h)完成。Windows 9x 内建 DPMI 服务器，
 *  Win16 DLL 可直接调用 int 31h。
 *
 *========================================================================*/

/* DPMI 功能 0006h：取选择子段基址，把 far 指针换算成线性地址。
 * 入口: p = 任意 far 指针
 * 返回: 该指针在当前 VM 中的 32 位线性地址(失败时返回 0)             */
static uint32_t LinearOf(const void far *p)
{
    g_regs.x.ax = 0x0006;               /* DPMI Get Segment Base Address */
    g_regs.x.bx = FP_SEG(p);
    CALL_INT(0x31);
    if (g_regs.x.cflag)
        return 0;
    /* CX:DX = 段基址(CX 为高字)，加上偏移即线性地址 */
    return ((((uint32_t)g_regs.x.cx) << 16) | g_regs.x.dx) + FP_OFF(p);
}

/* --- 诊断日志强制落盘 ------------------------------------------------------
 * 【r45 恢复】r44 把本函数清空, 结果实锤了"close 不落盘": 硬死机后
 * 整份日志(连建文件)随 VFAT 卷缓存蒸发(./44)。r43 的日志能幸存纯属
 * AH=0Dh 副作用; 而 AH=0Dh 本身是否有罪未决(43/44 表现都与"加载期
 * EnsureModeTable 直通枚举硬死机"兼容)。落盘改走 benign 的 DOS4+
 * AX=6800h commit(文件级, 不支持则 CF=1 无害), 实现在 vddapi16.asm
 * 的 DiagFlush; DiagWriteLog 每行已自带 commit, 本函数只给 FLUSH()
 * 里程碑语义兜底。 */
#ifdef VBE9X_DIAG
extern void __far __pascal DiagFlush( void );
static void FlushLog(void)
{
    if (g_logLevel >= 2)
        DiagFlush();
}
#else
#define FlushLog() ((void)0)
#endif

/* --- 数字诊断: "msg=XXXX"(4 位十六进制)拼进静态缓冲写一行 --------------
 * 【为何在 C 侧拼串】DiagWriteLog 只收字符串; 汇编侧加数字助手要动
 * vddapi16.asm 的写文件序列。引导期是单线程协作环境, DGROUP 静态
 * 缓冲无重入问题, C 拼串零风险。                                       */
#ifdef VBE9X_DIAG
static void DiagHex(const char *msg, uint16_t v)
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
    DiagWriteLog(buf);
}
/* --- r104: 32 位十六进制诊断一行 -----------------------------------------
 * 【为什么要它】r103 及以前 `TRC calls` / `TRC lost` 都是把 32 位计数截成
 * 16 位打印的, 于是在 ./103 的日志里出现 "TRC calls=D220" 紧接
 * "TRC calls=0013" 这种看着像"计数器清零"的序列 —— 那其实只是回绕。
 * 窗口化 DOS 会话期间这两个量是"主 VDD 到底有多忙"的唯一读数, 截断等于
 * 把它变成假的。本函数与 DiagHex 同处【外层 VBE9X_DIAG 块】内(发布构建
 * 由该块的 #else 统一替换成空宏)。                                     */
static void DiagHex32(const char *msg, uint32_t v)
{
    static char       buf[64];
    static const char hexd[] = "0123456789ABCDEF";
    char             *p = buf;
    int               i;

    if (g_logLevel < 2)
        return;
    while (*msg)
        *p++ = *msg++;
    *p++ = '=';
    for (i = 28; i >= 0; i -= 4)
        *p++ = hexd[(v >> i) & 15];
    *p   = 0;
    DiagWriteLog(buf);
}
/* --- r78 一行多值取证 -----------------------------------------------------
 * 【为何另起一套】LDT 扫描一次就是几十条记录, 若每个数值都借 DiagHex
 * 各写一行, 日志会从 ~470 行膨胀到两三千行, 真正要紧的几行反而被埋掉。
 * 这里给一个极简拼串器, 一行里塞多个十六进制数值; 落盘机制与 DiagHex
 * 完全相同(DGROUP 静态缓冲 + DiagWriteLog)。                            */
static char *DiagPutS(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *DiagPutX4(char *p, uint16_t v)
{
    static const char hexd[] = "0123456789ABCDEF";

    *p++ = hexd[(v >> 12) & 15];
    *p++ = hexd[(v >> 8) & 15];
    *p++ = hexd[(v >> 4) & 15];
    *p++ = hexd[v & 15];
    return p;
}

/* r79: 两字节(机器码转储用) */
static char *DiagPutX2(char *p, uint16_t v)
{
    static const char hexd[] = "0123456789ABCDEF";

    *p++ = hexd[(v >> 4) & 15];
    *p++ = hexd[v & 15];
    return p;
}

static char *DiagPutX8(char *p, uint32_t v)
{
    p = DiagPutX4(p, (uint16_t)(v >> 16));
    p = DiagPutX4(p, (uint16_t)v);
    return p;
}

/*==========================================================================
 * r107: 时间轴(tick / MARK / HB)
 *
 * 【为什么这一轮先做它】./104 的 t4/.bsod 分析有一个从未被证实的前提:
 *   "日志最后一行 = 崩溃现场"。日志是 DGROUP 静态缓冲 + 每行一次 DOS
 *   commit 写出去的, FLUSH 只在特定里程碑发生 —— 于是崩溃完全可能发生在
 *   最后一行之后很久, 那时尾部只说明"最后一次被 GDI 调到是什么时候"。
 *   这一轮把 BIOS tick 打进日志, 尾部与文件落盘时刻(主机上能看到的
 *   mtime)一比, 这个前提就能被证实或推翻。
 *
 * 【为什么用 0040:006C 而不是 DOS 时间】INT 21h AH=2Ch 也是一次 DOS 调用,
 *   而"日志写文件"这条通道正是 r102/r103 冻结事故的当事者(DOS 重入闸门
 *   就是为它设的)。0040:006C 只读一个 DWORD, 不碰 DOS、不碰 VxD, 在任何
 *   上下文(含 DDI 内部、BeginAccess 回调里)都安全。
 *========================================================================*/
uint32_t __far DiagTickNow(void)
{
    return *(volatile uint32_t __far *)MK_FP( 0x0040, 0x006C );
}

static uint16_t s_markN = 0;        /* MARK 序号(心跳 C=) */

void __far DiagMark(const char *tag)
{
    static char buf[80];
    char *p = buf;

    if (g_logLevel < 2)
        return;
    p = DiagPutS(p, "MARK ");
    p = DiagPutS(p, tag);
    p = DiagPutS(p, " T=");  p = DiagPutX8(p, DiagTickNow());
    p = DiagPutS(p, " C=");  p = DiagPutX4(p, s_markN++);
    *p = 0;
    DiagWriteLog(buf);
}

/* DiagBeat —— 心跳: 每 ~36 拍(2 秒)最多一行, 只在 VxdTraceDump 入口调。
 * 于是"崩溃前系统还在跑我们的代码、跑到第几拍"在日志里可以直接数出来;
 * 若整份日志里 T 完全不变, 说明 System VM 里没人维护这个 tick(读数本身
 * 就是结论, 下一轮换时间源)。                                        */
void __far DiagBeat(void)
{
    static uint32_t s_last = 0;
    uint32_t now = DiagTickNow();

    if (s_last == 0)                    /* 第一次只记账, 不打(避免开机噪声) */
    {
        s_last = now;
        return;
    }
    if ((uint16_t)(now - s_last) < 36)  /* 16 位差值: 只要间隔 << 1 小时就正确 */
        return;
    s_last = now;
    DiagMark( "HB" );
}

/* --- 选择子探针(DPMI 两路) ------------------------------------------------
 * 【为什么不用 LAR/LSL 补第三路】r107 试过: LAR/LSL 是"读描述符访问权/
 *   界限"且对无效选择子只清 ZF 的环 3 指令, 正好能补"GDT 里到底存不存在"。
 *   但本工程的 C 编译【不带 386 开关】(lib286/win, 无 -3), Watcom 的
 *   #pragma aux 里一写 eax 就 E1156 "Cannot use 386 register with current
 *   CPU setting"。为了这一路去改 makefile 的 CPU 级别, 代价与风险都大于
 *   收益 —— 于是只保留 DPMI 两路, 并把"问不出来的那半"明写在日志里。
 * 【于是能问出什么】
 *   ① DPMI 000Bh 成功 ⇒ 它【在 LDT 里】: 打完整描述符(基址/界限/访问权);
 *   ② 000Bh 失败 ⇒ 走 DPMI 0006h 要基址 —— 宿主若支持 GDT 选择子, 这一路
 *      直接给出"它在 GDT 里, 基址 = X"。基址非 0 就说明它是【真实存在的
 *      32 位段】, 基址为 0 则要么宿主不支持、要么这个段号根本不存在
 *      (后者 = 野跳, 与 `3A43:3D726964` 的"地址里是文本"互相印证)。
 *   这两行合起来足以把 ./104 那个 0x3A43 定性, 而不必再猜。          */
/* DiagDesc 定义在本文件更后面(它自己又依赖 g_descBuf/DpmiGetDescriptor),
 * 这里先声明 —— 免得 C89 下 "No prototype found"(W131)。            */
static int DiagDesc(uint16_t sel, uint32_t __far *pbase, uint32_t __far *plim,
                    BYTE __far *pacc, BYTE __far *pflg);

void __far DiagSelProbe(uint16_t sel)
{
    static char buf[96];
    char    *p;
    uint32_t base, lim;
    BYTE     acc, flg;

    if (sel == 0)
        return;

    if (DiagDesc(sel, &base, &lim, &acc, &flg))     /* ① 在 LDT 里 */
    {
        p = buf;
        p = DiagPutS(p, "SELPROBE sel=");  p = DiagPutX4(p, sel);
        p = DiagPutS(p, " ldt=1 base=");   p = DiagPutX8(p, base);
        p = DiagPutS(p, " lim=");          p = DiagPutX8(p, lim);
        p = DiagPutS(p, " acc=");          p = DiagPutX4(p, (uint16_t)acc);
        p = DiagPutS(p, " flg=");          p = DiagPutX4(p, (uint16_t)flg);
        *p = 0;
        DiagWriteLog(buf);
        return;
    }

    /* ② 000Bh 拒了: 不存在, 或属于 GDT(见本文件上方 r78 的实测记录:
     *    宿主对 GDT 选择子一律拒 000Bh)。再问一次 DPMI 0006h 要基址。 */
    base = LinearOf(MK_FP(sel, 0));
    p = buf;
    p = DiagPutS(p, "SELPROBE sel=");       p = DiagPutX4(p, sel);
    p = DiagPutS(p, " ldt=0 base0006=");    p = DiagPutX8(p, base);
    p = DiagPutS(p, base ? " (exists, not in LDT -> GDT)"
                         : " (base 0: host refused OR selector absent)");
    *p = 0;
    DiagWriteLog(buf);
}
#else
/* 发布构建: 同名同型, 保证两套构建链接一致(display.c 侧也引用它们)。 */
uint32_t __far DiagTickNow(void)            { return 0UL; }
void     __far DiagMark(const char *tag)    { (void)tag; }
void     __far DiagBeat(void)               { }
void     __far DiagSelProbe(uint16_t sel)   { (void)sel; }

#define DiagHex( m, v ) ((void)0)
#define DiagHex32( m, v ) ((void)0)     /* r104: 发布构建同样是空宏 */
#endif

/* --- LDT 选择子管理前向声明 ----------------------------------------------
 * 【r46】低内存缓冲分配(DPMI 0100h / DOS AH=48h 回退)已整段删除 ——
 * VBE 数据中转全部经 VxD 侧常驻缓冲 + 客户端 DGROUP 静态缓冲线性
 * 拷贝(./45 实锤: 客户端自建选择子写低内存在本机 Enable 期 GP 死机)。
 * 保留的 Alloc/Free/Base/Limit 四个函数仅供【帧缓冲选择子】使用
 * (LfbSelector_Ensure, Enable 期)。
 * Base/Limit 的定义是 static, 原型必须同为 static。                 */
uint16_t DPMI_AllocSelector(void);
void     DPMI_FreeSelector(uint16_t sel);
static int DPMI_SetSelectorBase(uint16_t sel, uint32_t linear);
static int DPMI_SetSelectorLimit(uint16_t sel, uint32_t limit);
static uint32_t DPMI_MapPhysical(uint32_t phys, uint32_t size);
static uint16_t FBSelector_Create(uint32_t linear, uint32_t size);

/* r78: 显存总量(字节; 0=未知)。定义在文件后部 4F00h 缓存区旁边, 与
 * PushStateToMiniVDD 共用同一份缓存; 这里前向声明, 供 LfbSelector_Ensure
 * 决定"帧缓冲到底映射多大"。                                          */
uint32_t __far VbeTotalVram(void);


/* --- 选择子管理(DPMI 0000h/0001h/0007h/0008h + 000Bh 只读取证) ----------
 * 【用途】为 DIBENG 建立覆盖线性帧缓冲(LFB)的 16:16 远指针：
 * DIBENGINE.deBitsSelector 是一个 16 位选择子，其段基址必须是
 * LFB 在当前 VM 中的线性地址(由 DPMI 0800h 映射得到, 见
 * LfbSelector_Ensure)。
 *
 * 【DPMI 0.9 函数号对照(已逐条核对 DPMI 规范, 血泪教训: 别记反!)】
 *   0000h Allocate LDT Descriptors   CX=个数 → AX=选择子
 *         宿主初始化为 "data 段, 存在位=1, 基址/界限=0, DPL=调用方 CPL"
 *   0001h Free LDT Descriptor        BX=选择子
 *   0006h Get Segment Base Address   BX=选择子 → CX:DX
 *   0007h Set Segment Base Address   BX=选择子, CX:DX=基址
 *   0008h Set Segment Limit          BX=选择子, CX:DX=界限(≥1MB 必须页对齐)
 *   0009h **Set Descriptor Access Rights**  BX=选择子, CL=访问权字节,
 *         CH=扩展字节(386+: G/D/B/AVL + 界限高 4 位)
 *         —— 这是【写】, 不是读! CL 的 DPL 位必须等于调用方 CPL;
 *            写 CL=0 会把存在位清掉, 载入即 #NP(异常 0B)。
 *   000Bh **Get Descriptor**         BX=选择子, ES:DI → 8 字节描述符
 *   000Ch Set Descriptor             BX=选择子, ES:DI → 8 字节描述符
 *   0800h Physical Address Mapping   BX:CX=物理地址, SI:DI=字节数 →
 *         BX:CX=线性地址(宿主为客户端建页表项, 客户端自己负责建描述符)
 * ---------------------------------------------------------------------- */

/* DPMI 功能 0000h：分配一个 LDT 选择子。返回 0 = 失败 */
uint16_t DPMI_AllocSelector(void)
{
    g_regs.x.ax = 0x0000;
    g_regs.x.cx = 1;
    CALL_INT(0x31);
    if (g_regs.x.cflag)
        return 0;
    return g_regs.x.ax;
}

/* DPMI 功能 0001h：释放选择子 */
void DPMI_FreeSelector(uint16_t sel)
{
    if (sel == 0)
        return;
    g_regs.x.ax = 0x0001;
    g_regs.x.bx = sel;
    CALL_INT(0x31);
}

/* DPMI 功能 0007h：设置选择子段基址(线性地址) */
static int DPMI_SetSelectorBase(uint16_t sel, uint32_t linear)
{
    g_regs.x.ax = 0x0007;
    g_regs.x.bx = sel;
    g_regs.x.cx = (uint16_t)(linear >> 16);
    g_regs.x.dx = (uint16_t)(linear & 0xFFFF);
    CALL_INT(0x31);
    return !g_regs.x.cflag;
}

/* DPMI 功能 0008h：设置选择子界限。
 * 【为何要 4KB 对齐】DPMI 0.9 规定：界限超过 1MB 时，描述符必须以
 * 页粒度(4KB)记录，低 12 位必须全 1；不按页对齐设置大界限会被
 * DPMI 主机拒绝。统一向上对齐到页 —— 多读几个对齐字节不会有副作用，
 * 因为 DIBENG 只在 pitch*yRes 范围内寻址。                             */
static int DPMI_SetSelectorLimit(uint16_t sel, uint32_t limit)
{
    g_regs.x.ax = 0x0008;
    g_regs.x.bx = sel;
    g_regs.x.cx = (uint16_t)(limit >> 16);
    g_regs.x.dx = (uint16_t)(limit & 0xFFFF);
    CALL_INT(0x31);
    return !g_regs.x.cflag;
}

/*==========================================================================
 *  帧缓冲(LFB)映射 —— r76 的核心修正
 *
 *  【实机故障】蓝屏 0D 于 031F:00090B。
 *  该地址落在 omninine.map 里 _fmemset_(0001:0902) 的 +9 处, 反汇编
 *  (75/omninine.drv 段 1 文件偏移 0xD68 起)为:
 *      902: 45        inc  bp
 *      903: 55        push bp
 *      904: 89E5      mov  bp,sp
 *      906: 57        push di
 *      907: 89C7      mov  di,ax        ; AX=远指针偏移(DX=段)
 *      909: 88D8      mov  al,bl        ; BL=填充字节
 *      90B: 8EC2      mov  es,dx        ; ← #GP 就是这条
 *  ClearVisibleScreen() 是"模式设好后第一次碰帧缓冲"的地方(日志最后
 *  一行 "PhysicalEnable: VDD post/save ok" 之后), 装进 ES 的就是
 *  mini-VDD(r73-r75)建的 GDT 屏选择子。两条病因, 都是架构性的:
 *    ① 描述符 DPL: _BuildDescriptorDWORDs 不指定 BDDExplicitDPL 时,
 *       VMM 按"保护模式应用的 RPL 位"填 DPL(在 VxD 上下文里得到的是
 *       低特权值), 16 位显示驱动(ring 3)载入选择子时
 *       max(CPL,RPL) > DPL → #GP;
 *    ② 基址是 VxD 系统区线性(>=0xC0000000), 那些页是 supervisor 页,
 *       就算描述符能载入, ring 3 第一次读写仍是 #PF(0E)。
 *  结论: 【ring 0 造不出 ring 3 能用的帧缓冲指针】。
 *
 *  【正解】显示驱动自己在 ring 3 用 DPMI 完成两件事:
 *      DPMI 0800h  物理地址映射 → 得到本 VM 可访问的线性地址
 *      DPMI 0000h/0007h/0008h   → 自建 LDT 选择子指向该线性地址
 *  DPMI 宿主给 ring 3 客户端建的选择子 DPL=3, 载入必然合法; 映射出来的
 *  页也是客户端可访问的。这是两条在售生产路线的做法(boxv9x 的
 *  AllocLinearSelector 就是 DPMI_MapPhys + DPMI_SetSegBase/Limit)。
 *  VxD 侧从此不再碰任何内存/描述符服务(r69-r75 的 Reserve/PCP/MPTL/
 *  BuildDescriptorDWORDs/Allocate_GDT_Selector 全部删除)。
 *
 *  【为何按"模式表里最大的帧缓冲"映射】选择子只在 bFullSet 路径重建,
 *  而 ReEnable 会换分辨率; 一次映射到模式表上限后, 会话内不再重建,
 *  也就不会出现"旧选择子已释放、新选择子没建成"的窗口。
 * =========================================================================*/

/* 帧缓冲映射的缓存状态 */
static uint16_t g_lfbSel    = 0;    /* 已建选择子(0=未建)                 */
static uint32_t g_lfbLinear = 0;    /* DPMI 0800h 返回的线性地址(诊断用)  */
static uint32_t g_lfbPhys   = 0;    /* 建档时的物理基址(页对齐)           */
static uint32_t g_lfbBytes  = 0;    /* 建档时映射的字节数(页对齐)         */
static uint32_t g_lfbSelBase = 0;   /* 建档时写进描述符的段基址(r78 复查) */
static uint32_t g_lfbAccOff  = 0;   /* 本驱动访问帧缓冲要用的 32 位偏移:
                                     * 0 基址选择子 → 线性地址; 旧形态 → 0 */
static uint32_t g_lfbWinBytes = 0;  /* r82: 从帧缓冲起点算起、选择子真正可
                                     * 访问的字节数(= 映射字节数 - 页内偏移)。
                                     * 自检/上报显存都不得越过它。          */

/* --- r81: 屏选择子形态的运行时开关(来自 system.ini [omninine] SelMode) -----
 * 【为什么要一个运行时开关】"0 基址选择子 + 线性偏移"与"基址=帧缓冲 +
 * 偏移 0"这两种形态在两种解读下各有对错(见 LfbSelector_Ensure 头注),
 * 目前无法从文档判定哪一个才是 DIBENG 真正要的。与其让用户为每种组合
 * 重新编译+装驱动, 不如把选择做成 sys.ini 一个键: 改一行、重启, 一轮
 * 就能把矩阵跑完 —— 而且【完全不引入任何版本/机型依赖】(键不存在时
 * 一律走默认值, 与不设该键的构建逐指令等价)。
 *   0(默认) = 先试 0 基址(flat), 被拒则退回基址=帧缓冲(r80 行为)
 *   1        = 先试 基址=帧缓冲, 被拒则退回 0 基址(r79 行为)
 * 【为何放 drv.c】形态在 LfbSelector_Ensure 里定, g_lfbAccOff 也在这里
 * 维护; 由 display.c 在 Enable 期读 sys.ini 后调用 LfbSelModeSet 设置。 */
static int g_lfbSelMode = 0;

void __far LfbSelModeSet(int mode)
{
    g_lfbSelMode = (mode == 1) ? 1 : 0;
}

/* --- r89: descriptor verification layer ---------------------------------
 * "Never trust a host."  A DPMI host is *allowed* to normalise a limit to
 * page granularity, and it is allowed to refuse or clamp one.  If it hands
 * back a selector whose base/limit do not really cover the window we are
 * about to hand to DIBENG, then accepting it silently is exactly how a
 * driver works on the test machine and faults on the next one - with a
 * release build and no way to tell which layer took which branch.
 * So every selector we build is now read back (DPMI 000Bh, read only) and
 * checked against the request *before* it is accepted; a level of the
 * ladder that fails this check is discarded and the next level runs, which
 * keeps the previous (already proven) behaviour as the fallback.
 * system.ini [omninine] Verify=0 turns both gates off and restores the r88
 * behaviour instruction for instruction - the off switch exists so that a
 * suspect gate can be eliminated in one reboot instead of one rebuild.
 * ====================================================================== */
static int g_lfbSelVerify = 1;

void __far LfbSelVerifySet(int on)
{
    g_lfbSelVerify = (on == 0) ? 0 : 1;
}

/* DPMI 功能 0800h：把物理地址映射进当前客户端的线性地址空间。
 * 入口: BX:CX = 物理地址(应页对齐; BX 为高字), SI:DI = 字节数
 * 出口: CF=0 时 BX:CX = 映射到的线性地址
 * 返回 0 = 失败。
 * 【用途】DPMI 规范里这就是给"设备/显存窗口"用的功能; 显卡 LFB 的
 * 物理地址在 4F01h 的 PhysBasePtr 里拿到, 直接交给它即可。        */
static uint32_t DPMI_MapPhysical(uint32_t phys, uint32_t size)
{
    g_regs.x.ax = 0x0800;
    g_regs.x.bx = (uint16_t)(phys >> 16);
    g_regs.x.cx = (uint16_t)(phys & 0xFFFF);
    g_regs.x.si = (uint16_t)(size >> 16);
    g_regs.x.di = (uint16_t)(size & 0xFFFF);
    CALL_INT(0x31);
    if (g_regs.x.cflag)
        return 0;
    return ((((uint32_t)g_regs.x.bx) << 16) | g_regs.x.cx);
}

/* --- 描述符取证缓冲(DPMI 000Bh 用, 8 字节) ---------------------------- */
static BYTE g_descBuf[8];

/* DpmiGetDescriptor —— DPMI 000Bh "Get Descriptor"(只读, 无副作用)。
 *   入口: AX=000Bh, BX=选择子, ES:DI = 8 字节缓冲
 *   返回: 1 = 成功(描述符已拷进 g_descBuf), 0 = 失败(CF=1)
 * 【r76b 血的教训 —— 千万别再搞错这两个函数号】
 *   DPMI 0.9 的 0009h 是 **Set Descriptor Access Rights**(CL=访问权字节,
 *   CH=扩展字节), 真正的"读描述符"是 000Bh。
 *   r76 的日志代码把 0009h 当只读用, 而 CL/CH 正好是上一次 0008h 调用
 *   的残留(0), 于是把刚建好的屏选择子的访问权字节写成 0x00 ——
 *   存在位被清零。下一次 "mov es,选择子"(031F:03D2 = FBCLEAR 里的
 *   "mov es,[_FbClearSel]")立刻 #NP, 蓝屏异常 0B。
 *   本函数只做 000Bh, 不可能再改到描述符。                            */
static int DpmiGetDescriptor(uint16_t sel)
{
    g_regs.x.ax = 0x000B;
    g_regs.x.bx = sel;
    g_sregs.es  = FP_SEG((void __far *)g_descBuf);
    g_regs.x.di = FP_OFF((void __far *)g_descBuf);
    CALL_INTX(0x31);
    return !g_regs.x.cflag;
}

/* FbSelUsable —— 建好选择子后的最后一道闸门(自校准, 不会误杀)。
 * 返回: 1 = 可用(或无法判定, 按可用处理); 0 = 判定不可用。
 * 【自校准】先读本模块 DGROUP 的描述符作对照 —— 它必然存在、必然可
 *   载入(DS 里就装着它):
 *     · 对照读成功且 byte5(访问权字节)非 0 → 000Bh 可信, 继续判定;
 *     · 对照读失败或 byte5==0 → 宿主没实现/实现坏了 000Bh, 放弃闸门
 *       返回 1 —— 诊断手段坏了不能当成选择子坏了(否则会因为一个取
 *       证调用把整个驱动挡在 VGA 上)。
 * 【判定】读失败(8022h 无效选择子) / 存在位=0 / S 位=0(非代码数据段)
 *   → 判定不可用, 让 Enable 干净回退 VGA。这正是 r76 那次 #NP 的形态:
 *   提前拦下就只是"没装上驱动", 而不是第一次写显存时蓝屏。          */
/* r89: the gate now also takes the *request* (base/bytes/exact-base flag)
 * and verifies that the descriptor the host actually built covers it — see
 * the body below for the layout decode and the four rejection reasons. */
static int FbSelUsable(uint16_t sel, uint32_t wantBase, uint32_t wantBytes,
                       int exactBase)
{
    _fmemset(g_descBuf, 0, sizeof(g_descBuf));
    if (!DpmiGetDescriptor(FP_SEG((void __far *)g_descBuf)) ||
        g_descBuf[5] == 0)
    {
        DIAG("FbSel: DPMI 000Bh unusable -> descriptor gate skipped");
        return 1;                       /* 无法判定: 按可用处理 */
    }

    _fmemset(g_descBuf, 0, sizeof(g_descBuf));
    if (!DpmiGetDescriptor(sel))
    {
        DIAG("FbSel: get descriptor failed -> reject (invalid selector)");
        return 0;
    }

    /* --- r89: decode base/limit from the descriptor we just built, and
     * verify that [wantBase, wantBase+wantBytes-1] really lies inside
     * [base, base+limit] (the limit is inclusive).
     * DPMI 000Bh descriptor image, byte offsets:
     *   0..1 limit 15:0    2..3 base 15:0    4 base 23:16
     *   5    access rights 6    limit 19:16 + G/D   7 base 31:24
     * exactBase is set for the flat (form A) selector: the whole engine
     * hand-off then assumes "offset == linear address", which only holds
     * when the base is exactly 0, so a host that rounds the base must not
     * be accepted there.
     * Rejection here is never fatal: FBSelector_Create frees the half-built
     * selector and returns 0, so LfbSelector_Ensure moves to the next rung
     * of its ladder (and finally to the other form / clean VGA fallback). */
    if (g_lfbSelVerify)
    {
        uint32_t base, lim, last;

        base = ((uint32_t)g_descBuf[7] << 24) | ((uint32_t)g_descBuf[4] << 16)
             | ((uint32_t)g_descBuf[3] << 8)  | (uint32_t)g_descBuf[2];
        lim  = ((uint32_t)(g_descBuf[6] & 0x0F) << 16)
             | ((uint32_t)g_descBuf[1] << 8) | (uint32_t)g_descBuf[0];
        if (g_descBuf[6] & 0x80)
            lim = (lim << 12) | 0xFFFUL;    /* G=1: limit is in 4K pages */

        DiagHex("FbSel: desc base lo", (uint16_t)base);
        DiagHex("FbSel: desc base hi", (uint16_t)(base >> 16));
        DiagHex("FbSel: desc lim lo",  (uint16_t)lim);
        DiagHex("FbSel: desc lim hi",  (uint16_t)(lim >> 16));

        last = wantBase + ((wantBytes > 0) ? (wantBytes - 1) : 0);
        if (exactBase && base != wantBase)
        {
            DIAG("FbSel: flat form needs base 0, host gave otherwise -> reject");
            return 0;
        }
        if (base > wantBase)
        {
            DIAG("FbSel: base above requested start -> reject");
            return 0;
        }
        if (last < wantBase)                /* 32-bit wrap: range >= 4GB */
        {
            DIAG("FbSel: requested range wraps 32 bits -> reject");
            return 0;
        }
        if (lim < (last - base))
        {
            DIAG("FbSel: limit shorter than requested range -> reject");
            return 0;
        }
        DIAG("FbSel: descriptor covers request -> accept");
    }
    DiagHex("FbSel: descr b5", (uint16_t)g_descBuf[5]); /* P/DPL/S/Type */
    DiagHex("FbSel: descr b6", (uint16_t)g_descBuf[6]); /* G/D + limit 高4 */
    DiagHex("FbSel: descr b0", (uint16_t)g_descBuf[0]); /* 基址低 16 */
    if (!(g_descBuf[5] & 0x80))
    {
        DIAG("FbSel: descriptor NOT PRESENT -> reject");
        return 0;                       /* 存在位 = 0: 载入即 #NP */
    }
    if (!(g_descBuf[5] & 0x10))
    {
        DIAG("FbSel: descriptor not code/data -> reject");
        return 0;                       /* S 位 = 0: 不是代码/数据段 */
    }
    return 1;
}

/* 建立帧缓冲选择子：base = linear, limit = size-1(页对齐)。
 * 返回 0 = 失败(已自动回收半成品)                                     */
static uint16_t FBSelector_Create(uint32_t linear, uint32_t size)
{
    uint16_t sel;
    uint32_t limit;

    if (linear == 0 || size == 0)
        return 0;

    sel = DPMI_AllocSelector();
    if (sel == 0)
        return 0;

    /* 【规范依据】DPMI 0000h 分配出来的描述符由宿主初始化为
     * "data, 存在位=1, 基址/界限=0, DPL=调用方 CPL" —— 也就是说
     * 它本来就合法可载入; 我们只需把基址与界限填上。
     * 【绝对不要】在这之后调用 0009h/000Ch 改访问权: 0009h 要求
     * CL 的 DPL 位必须等于调用方 CPL, 写错一位(比如 CL=0)就把存在
     * 位清掉, 下一次载入直接 #NP —— r76 蓝屏异常 0B 就是这么来的。 */
    limit = ((size + 0xFFFUL) & ~0xFFFUL) - 1;  /* 页对齐后的界限 */

    if (!DPMI_SetSelectorBase(sel, linear) ||
        !DPMI_SetSelectorLimit(sel, limit)) {
        DPMI_FreeSelector(sel);
        return 0;
    }

    /* 只读取证 + 安全闸门(见 FbSelUsable 头注) */
    if (!FbSelUsable(sel, linear, size, (linear == 0) ? 1 : 0)) {
        DPMI_FreeSelector(sel);
        return 0;
    }
    return sel;
}

/* ======================================================================
 * r89: LfbSelVerify -- post-acceptance cross-check.
 *
 * The creation gate (FbSelUsable) already refuses a descriptor that does
 * not cover what was requested, per rung of the ladder.  This second check
 * runs once Enable() has settled on a selector, because:
 *   - the accepted level may have been clamped by the host afterwards, and
 *   - a third party (another VxD, a mode switch, a VDD screen-selector
 *     registration) may rewrite the LDT entry later.
 * It verifies the two invariants the whole engine hand-off depends on:
 *   - the descriptor base must be exactly the base we asked for
 *     (0 for form A, linear+off for form B): a "helpfully" rounded base
 *     would silently shift the framebuffer;
 *   - the limit must cover the window the driver will really touch, which
 *     is the segment-relative range [accOff, accOff+win-1] - form A has
 *     accOff = the linear address, form B has accOff = 0.
 * A too-short limit is NOT made fatal - a black screen would be worse than
 * a small one - instead the accessible window is clamped down to what the
 * descriptor really covers, so the VRAM ceiling / OFFSCREEN decision stops
 * advertising memory that would fault on the first write.
 * Undecidable cases follow FbSelUsable's rule: if DPMI 000Bh itself is
 * broken we must not punish the selector for a broken diagnostic.
 * Return: 1 = verified (or undecidable), 0 = definitely inconsistent.
 * ====================================================================== */
int __far LfbSelVerify(void)
{
    uint32_t base, lim, need, cov;
    int      ok = 1;

    if (g_lfbSel == 0)
        return 0;
    if (!g_lfbSelVerify)
    {
        DIAG("SEL: verify disabled (Verify=0) -> skipped");
        return 1;
    }

    /* calibration: our own DGROUP descriptor is loadable by definition */
    _fmemset(g_descBuf, 0, sizeof(g_descBuf));
    if (!DpmiGetDescriptor(FP_SEG((void __far *)g_descBuf)) ||
        g_descBuf[5] == 0)
    {
        DIAG("SEL: DPMI 000Bh unusable -> verify skipped");
        return 1;
    }

    _fmemset(g_descBuf, 0, sizeof(g_descBuf));
    if (!DpmiGetDescriptor(g_lfbSel))
    {
        DIAG("SEL: read failed -> reject (invalid selector)");
        return 0;
    }
    base = ((uint32_t)g_descBuf[7] << 24) | ((uint32_t)g_descBuf[4] << 16)
         | ((uint32_t)g_descBuf[3] << 8)  | (uint32_t)g_descBuf[2];
    lim  = ((uint32_t)(g_descBuf[6] & 0x0F) << 16)
         | ((uint32_t)g_descBuf[1] << 8) | (uint32_t)g_descBuf[0];
    if (g_descBuf[6] & 0x80)
        lim = (lim << 12) | 0xFFFUL;        /* G=1: limit is in 4K pages */

    DiagHex("SEL: acc byte",   (uint16_t)g_descBuf[5]);
    DiagHex("SEL: base lo",    (uint16_t)base);
    DiagHex("SEL: base hi",    (uint16_t)(base >> 16));
    DiagHex("SEL: lim lo",     (uint16_t)lim);
    DiagHex("SEL: lim hi",     (uint16_t)(lim >> 16));
    DiagHex("SEL: acc off lo", (uint16_t)g_lfbAccOff);
    DiagHex("SEL: acc off hi", (uint16_t)(g_lfbAccOff >> 16));
    DiagHex("SEL: win lo",     (uint16_t)g_lfbWinBytes);
    DiagHex("SEL: win hi",     (uint16_t)(g_lfbWinBytes >> 16));

    if (!(g_descBuf[5] & 0x80) || !(g_descBuf[5] & 0x10))
    {
        DIAG("SEL: not present / not a data segment -> reject");
        return 0;
    }
    if (base != g_lfbSelBase)
    {
        DIAG("SEL: base != the base we asked for -> reject");
        return 0;
    }
    /* The driver addresses the segment as sel:accOff for win bytes, so the
     * requirement is a segment-relative one: lim >= accOff+win-1.  Do NOT
     * mix this with absolute linear addresses (form B has accOff = 0 and a
     * base equal to the framebuffer's linear address). */
    need = g_lfbAccOff + ((g_lfbWinBytes > 0) ? (g_lfbWinBytes - 1) : 0);
    if (need < g_lfbAccOff)                 /* 32-bit wrap */
        need = 0xFFFFFFFFUL;

    if (lim < need)
    {
        /* Short limit: tell the truth about the window instead of lying. */
        cov = (lim >= g_lfbAccOff) ? (lim - g_lfbAccOff + 1) : 1;
        DIAG("SEL: limit shorter than window -> clamp accessible window");
        g_lfbWinBytes = cov;
        DiagHex("SEL: win clamped lo", (uint16_t)g_lfbWinBytes);
        DiagHex("SEL: win clamped hi", (uint16_t)(g_lfbWinBytes >> 16));
        ok = 0;
    }
    if (ok)
        DIAG("SEL: consistency OK (base+limit cover the access window)");
    return ok;
}

/* LfbSelector_Ensure —— 取(必要时建立)帧缓冲选择子。
 * 入口: physBase = VBE 4F01h 的 PhysBasePtr(物理地址)
 *       bytes    = 本次模式需要的帧缓冲字节数(pitch*yRes)
 * 返回: 16 位选择子; 0 = 失败(调用方应让 Enable 干净地失败回退 VGA,
 *       而不是拿个野选择子去写)。
 * 【物理地址页对齐】虽然 4F01 报的 PhysBasePtr 通常页对齐, 这里仍
 * 按页对齐映射、把页内偏移补回选择子基址, 不做任何假设。            */
uint16_t __far LfbSelector_Ensure(uint32_t physBase, uint32_t bytes)
{
    uint32_t pageBase, want, linear, small, big;
    uint16_t off, newSel;

    if (physBase == 0 || bytes == 0)
        return 0;

    /* 两级尺寸:
     *   small = r77 的尺寸(本次模式所需与模式表上限的较大者) —— 永远先
     *           保证它能映射成功, 这是已验证可用的下界;
     *   big   = small 再并上 BIOS 报的显存总量。
     * 【r78 为何要 big】主 VDD 通过 GET_TOTAL_VRAM_SIZE 拿到的是【显存
     * 总量】(本机 16MB), 而 ring 3 能真正访问的范围完全由本选择子的界限
     * (= 这里映射的字节数)决定。r77 之前只映射 4MB 却上报 16MB: 任何按
     * 16MB 算出来的偏移都会撞段界限 #GP —— 而 r76b/r76c/r77 三次蓝屏恰好
     * 都是"落在别的模块段内、整份日志没有任何一条 DDI 探针"(见 display.c
     * 的 r78 取证注释)。
     * 【为何要退回】16MB 的 DPMI 0800h 映射不是所有宿主都肯给; 大映射被拒
     * 时立刻退回 small, 行为与 r77 完全一致 —— 再差也不会比 r77 差。
     * VbeTotalVram() 会触发一次 4F00h(走已验证的 VxD 嵌套直通); 拿不到就
     * 返回 0, big == small, 同 r77。                                   */
    small = bytes;
    if (g_lfbSel == 0)
    {
        uint32_t cap = VbeMaxFramebuffer();
        if (cap > small)
            small = cap;
    }
    big = small;
    if (g_lfbSel == 0)
    {
        uint32_t vr = VbeTotalVram();
        if (vr > big)
            big = vr;
    }
    if (small > VBE_LFB_MAP_CAP)
        small = VBE_LFB_MAP_CAP;
    if (big > VBE_LFB_MAP_CAP)
        big = VBE_LFB_MAP_CAP;

    DiagHex("LfbMap: small lo", (uint16_t)small);
    DiagHex("LfbMap: small hi", (uint16_t)(small >> 16));
    DiagHex("LfbMap: big lo", (uint16_t)big);
    DiagHex("LfbMap: big hi", (uint16_t)(big >> 16));

    pageBase = physBase & 0xFFFFF000UL;
    off      = (uint16_t)(physBase & 0xFFFUL);

    /* 复用判定按 small 判: 现有映射够 small 就不必重建 */
    want = (off + small + 0xFFFUL) & 0xFFFFF000UL;
    if (want == 0)
        return 0;
    if (g_lfbSel && pageBase == g_lfbPhys && want <= g_lfbBytes)
        return g_lfbSel;                    /* 已有映射够用: 复用 */

    want = (off + big + 0xFFFUL) & 0xFFFFF000UL;
    DiagHex("LfbMap: phys lo", (uint16_t)pageBase);
    DiagHex("LfbMap: phys hi", (uint16_t)(pageBase >> 16));
    DiagHex("LfbMap: bytes lo", (uint16_t)want);
    DiagHex("LfbMap: bytes hi", (uint16_t)(want >> 16));
    FlushLog();

    linear = DPMI_MapPhysical(pageBase, want);
    if (linear == 0 && big != small)
    {
        DIAG("LfbMap: big mapping rejected -> retry r77 size");
        FlushLog();
        want   = (off + small + 0xFFFUL) & 0xFFFFF000UL;
        linear = DPMI_MapPhysical(pageBase, want);
    }
    if (linear == 0)
    {
        DIAG("LfbMap fail: DPMI 0800h (物理映射被拒)");
        FlushLog();
        return 0;
    }
    DiagHex("LfbMap: linear lo", (uint16_t)linear);
    DiagHex("LfbMap: linear hi", (uint16_t)(linear >> 16));
    FlushLog();

    /* 【先建后弃】新选择子建成前不释放旧的(失败时旧映射仍然可用)。
     * 【r81: 形态由 g_lfbSelMode 选, 且永远留退路】
     *   形态 A = 0 基址(flat)选择子 + 偏移 = 线性地址  (DDK 原文的 0-based)
     *   形态 B = 选择子基址 = 帧缓冲, 偏移 = 0        (r79 及以前的行为)
     * 先按 SelMode 指定的那个试; 被宿主拒绝就自动试另一个 —— 于是不管
     * 哪个形态才是 DIBENG 要的, 我们都还能拿到一个可用选择子, 最坏
     * 情况与改动前逐指令等价(不会更糟)。                          */
    DiagHex("LfbMap: SelMode", (uint16_t)g_lfbSelMode);
    if (g_lfbSelMode == 0)
    {
        /* 【r88 关键修正】形态 A(0 基址平坦选择子)原先一律请求 0xFFFFFFFF
         * (4GB)界限, 本机宿主【拒绝】这个界限 —— 于是形态 A 从来没建成过,
         * 每次都静默退回形态 B。而 r87 的日志证明 DIBENG 需要形态 A: 它做的
         * 是 "mov es,[si+78h] + test dword es:[0000001Ch]" —— 绝对地址 32 位
         * 访问, 只有 0 基址的平坦选择子才成立; 拿到基址=帧缓冲的选择子时, 它
         * 读到的是显存偏移 0x1C 的垃圾, 从垃圾里"取出"一个指针去调用
         * → 跳进 DPMI 堆 → 0D。现改为"精确上界 → 4GB"两级尝试, 每级都落盘。 */
        uint32_t need = linear + off + want;    /* 真正需要覆盖到的线性上界 */

        if (need > 0xFFFFE000UL)
            need = 0xFFFFE000UL;                /* 保护: 界限保持在可表示范围 */
        newSel = FBSelector_Create(0, need);
        DiagHex("LfbMap: formA need lo", (uint16_t)need);
        DiagHex("LfbMap: formA need hi", (uint16_t)(need >> 16));
        DiagHex("LfbMap: formA precise limit", newSel);
        if (!newSel)
        {
            newSel = FBSelector_Create(0, 0xFFFFFFFFUL);
            DiagHex("LfbMap: formA 4GB limit", newSel);
        }
    }
    else
        newSel = FBSelector_Create(linear + off, want - off);
    if (newSel)
    {
        if (g_lfbSelMode == 0)
        {
            g_lfbAccOff  = linear;          /* 本驱动自己的访问偏移 */
            g_lfbSelBase = 0;               /* 描述符基址(r78 复查用) */
            DIAG("LfbMap: form A ok: 0-based sel, deBitsOffset=linear");
        }
        else
        {
            g_lfbAccOff  = 0;
            g_lfbSelBase = linear + off;
            DIAG("LfbMap: form B ok: sel base=LFB, deBitsOffset=0");
        }
    }
    else
    {
        DIAG("LfbMap: preferred form refused -> try the other form");
        FlushLog();
        if (g_lfbSelMode == 0)
            newSel = FBSelector_Create(linear + off, want - off);
        else
            newSel = FBSelector_Create(0, 0xFFFFFFFFUL);
        if (newSel)
        {
            if (g_lfbSelMode == 0)
            {
                g_lfbAccOff  = 0;
                g_lfbSelBase = linear + off;
            }
            else
            {
                g_lfbAccOff  = linear;
                g_lfbSelBase = 0;
            }
            DIAG("LfbMap: other form OK (see SelMode value above)");
        }
    }
    if (newSel == 0)
    {
        DIAG("LfbMap fail: FBSelector_Create (DPMI LDT 选择子)");
        FlushLog();
        return 0;
    }
    FlushLog();

    if (g_lfbSel)
        DPMI_FreeSelector(g_lfbSel);
    g_lfbSel    = newSel;
    g_lfbLinear = linear;
    g_lfbPhys   = pageBase;
    g_lfbBytes  = want;
    g_lfbWinBytes = want - off;         /* r82: 从帧缓冲起点可访问的字节数 */

    DiagHex("LfbMap: sel", newSel);
    FlushLog();
    return newSel;
}

/* LfbAccessOffset —— 本驱动(以及交给 DIBENG 的 deBitsOffset)访问帧缓冲时
 * 应当使用的 32 位偏移(r80)。
 *   0 基址选择子形态 → 返回 DPMI 映射得到的线性地址(与 deBits 的
 *     "0-based buffer + offset=线性地址" 约定一致);
 *   退回形态(基址=帧缓冲) → 返回 0。
 * ClearVisibleScreen 会把它填进 FBCLEAR 的 _FbClearOff, display.c 会把它
 * 填进 DIBENGINE.deBitsOffset —— 两处必须同源, 否则就是"清屏清了别处"。 */
uint32_t __far LfbAccessOffset(void)
{
    return g_lfbAccOff;
}

/* LfbWindowBytes —— r82: 从【帧缓冲起点】算起、选择子真正可访问的字节数。
 *   形态 A(0 基址 + 线性偏移): 可用线性区间 = [linear, linear+want-off),
 *     起点是 linear, 所以从帧缓冲起点算起就是 want-off;
 *   形态 B(基址=帧缓冲): 可用区间 = [base, base+want-off), 同样 want-off。
 * 【为什么需要它】自检与显存上报都必须止步于此 —— 越过它就是读到/写到
 * "映射之外"的线性地址: 形态 A 下那是别人的内存(选择子界限是 4GB, 不会
 * 拦你), 形态 B 下会 #GP。两者都是灾难。                            */
uint32_t __far LfbWindowBytes(void)
{
    return g_lfbWinBytes;
}

/* LfbVramCeiling —— r82: 自检后的"可信显存上限"(0 = 不设限)。
 *   由 display.c 的帧缓冲自检在发现"报的显存其实写不进去"时下调, 之后
 *   ReportVram 会用它钳制上报值, 于是 GDI/DIBENG 不会再拿到一个不存在的
 *   离屏显存区。宁可少报, 不可谎报 —— 谎报的代价是随机花屏/黑屏。     */
static uint32_t g_vramCeiling = 0;

void __far LfbSetVramCeiling(uint32_t bytes)
{
    g_vramCeiling = bytes;
}

/* LfbSelector_Release —— 释放会话内建立的帧缓冲选择子(仅诊断/兜底用)。
 * 【正常路径不调用】DIBENG 会一直拿着这个选择子画图, Disable 之后
 * 如果再 Enable(切回本驱动), 复用旧映射比"释放-重建"少一次物理映射
 * (DPMI 0.9 没有解除物理映射的功能, 反复重建只会泄漏地址空间)。  */
void __far LfbSelector_Release(void)
{
    if (g_lfbSel)
    {
        DPMI_FreeSelector(g_lfbSel);
        g_lfbSel    = 0;
        g_lfbLinear = 0;
        g_lfbPhys   = 0;
        g_lfbBytes  = 0;
        g_lfbSelBase = 0;
        g_lfbAccOff  = 0;
        g_lfbWinBytes = 0;
    }
}

/*==========================================================================
 *  r78 取证: 崩溃地址归属 + 屏选择子复查
 *  【r81 说明】本节是历史动机记录; 具体做法已在 r81 重做(见下一节
 *  "r81 取证总纲"): 不再对 LDT 盲扫段内容, 改成"有出处的段"地图。
 *
 *  【要回答的问题】r76b/r76c/r77 三次蓝屏都是同一个 16 位地址
 *  0427:00002B70 —— 段选择子 0x0427 不是本模块(本模块代码段 0x031F,
 *  见日志 "PDEV: beginAcc seg=031F"), 偏移 0x2B70 也超出本模块代码段
 *  长度(0x2FA5)。更要命的是: 整份日志【没有任何一条 DDI 探针】——
 *  说明 GDI/USER 在 Enable 返回之后、还没调用我们任何一个入口, 就在
 *  自己(或 DIBENG)的代码里崩了。
 *
 *  要定性只需两件事:
 *   ① 0x0427 到底是谁的段? 把各模块 DGROUP 选择子/基址、DIBENG 各
 *      入口的代码段选择子(display.c 的 DBFN 行)、以及 0x0320..0x04A0
 *      区间内所有"存在"的 LDT 描述符(选择子/基址/界限/访问权)落盘。
 *      基址连成一片的就是同一个模块: 与模块句柄基址一比即知。
 *      按加载顺序(bootlog): omninine.drv → DIBENG.DLL → GDI.EXE → USER.EXE,
 *      本模块代码段 = LDT 索引 99, 因此 0x0427(索引 132)= 本模块之后
 *      第 33 个选择子 —— 最可能是 DIBENG 或 GDI 的某个段。
 *   ② 屏选择子登记给主 VDD 之后有没有被动过? LfbSelRecheck 重新读一次
 *      描述符: 基址/访问权与建档时不一致 = 被改了(那样 GDI 载入它就
 *      立刻 #GP, 且完全不需要调用我们任何入口 —— 与现象高度吻合)。
 *========================================================================*/
#ifdef VBE9X_DIAG

/*==========================================================================
 *  r81 取证总纲 —— "有出处的段"原则
 *
 *  【要回答的唯一问题】蓝屏给的 SSSS:OOOOOOOO 到底落在谁身上、是哪一条
 *  指令。r76b..r78 的 0427:00002B70 至今没有定性, 原因不是信息不够,
 *  而是取信息的手法错了。
 *
 *  【r79 的教训(代价: 一次完整构建+装机+重启)】r79 对 LDT 做盲扫, 对
 *  每个"存在/S=1/DPL=3/可读/界限够"的段都去读它的 0x2B60 处 48 字节。
 *  结果读到 DPMI 惰性提交的大堆段(base 0x81F48000, 界限 2MB+)里【未提交
 *  的页】→ #PF → VMM 解决不了 → 蓝屏 0E。一句话:
 *      "描述符合法" != "页已提交"。
 *  ring 3 没有任何干净的办法查提交位(DPMI 0.9 无此功能), 所以:
 *
 *  【r81 原则一 · 安全】只读【有出处的段】:
 *    · 本模块自己的段(FP_SEG 取自真实函数/全局变量);
 *    · 各模块数据库(GetModuleHandle)→ 段表里列出的段: 那些是加载器
 *      分配并装载过的, 必然已提交, 且一定在 DPL=3 可读;
 *    · 各 DIBENG 入口的远指针(GetProcAddress 给的 sel:off)。
 *    LDT 盲扫只留【描述符】(DPMI 000Bh 只读描述符表, 不碰被描述的页),
 *    那部分是纯安全的, 而且正是它给出了 r78/r79 两轮的模块地图。
 *
 *  【r81 原则二 · 通用】以上全部经【公开接口】获得, 没有一处硬编码
 *    地址、没有一处版本判断、没有一处依赖 DLL 内部结构偏移以外的假设
 *    (模块数据库布局是 Win3.1 起的冻结 ABI, 且本文件逐字段校验后才用)。
 *    换任何机器、任何 9x 版本, 这些行照样打印。
 *========================================================================*/

/* --- 机器码窗口 ---------------------------------------------------------
 * g_dumpOff 就是要抄回机器码的段内偏移: 默认 0x2B60, 覆盖历史蓝屏偏移
 * 0x2B70 的前 16 字节 —— 足够手工反汇编出那一条指令。
 * 【为何可配置】下次蓝屏偏移若变(比如变成 0x31C0), 不必重新编译:
 * system.ini 的 [omninine] DumpOff=31C0 即可。这是个纯诊断参数, 不参与
 * 任何驱动逻辑, 也不构成"按地址打补丁"。                              */
static uint16_t g_dumpOff = 0x2B60;
#define DIAG_DUMP_LEN   64      /* 默认窗口(r82) */
#define DIAG_DUMP_MAX   512     /* r85: system.ini DumpLen 的上限 */
#define DIAG_DUMP_BUF   2048
static uint16_t g_dumpLen = DIAG_DUMP_LEN;  /* r85: 窗口宽度(system.ini DumpLen) */

void __far DiagSetDumpOff(uint16_t off)
{
    g_dumpOff = off;
}

/* DiagSetDumpLen —— r85: 机器码窗口宽度(system.ini [omninine] DumpLen)。
 *   【为什么要能加宽】蓝屏只给一个地址, 64 字节往往只能看到"出事那条指令"
 *   本身, 看不到它所在函数的序章与它前一条 call —— 而"这是谁的代码"恰恰
 *   藏在那几十个字节里。加宽到 512 字节后, 整段函数体都会落进日志。      */
void __far DiagSetDumpLen(uint16_t len)
{
    if (len < 16)
        len = DIAG_DUMP_LEN;
    if (len > DIAG_DUMP_MAX)
        len = DIAG_DUMP_MAX;
    g_dumpLen = len;
}

/* 一行最多 "DUMP sel=XXXX off=XXXX :" + 64*3 字节十六进制 + NUL */
static char g_ldtBuf[DIAG_DUMP_BUF];

/* DiagFnPtr —— 函数远地址 → DWORD(sel<<16 | off), 供 DiagLogPtr 打印。
 * 【为何用 union】Watcom 对"函数指针 → 数据指针/整数"的直接强转可能给出
 * 警告; union 是 16 位 Watcom 里的常规写法(display.c 的 DrvInitCore 用
 * 同一手法把 DibFnStub 塞进 g_dibfn[])。                          */
static uint32_t DiagFnPtr(void (__far *fp)(void))
{
    union { void (__far *f)(void); uint32_t dw; } u;

    u.f = fp;
    return u.dw;
}
#define DIAGFN(f)   DiagFnPtr((void (__far *)(void))(f))

#endif /* VBE9X_DIAG (r101: DescRead below must survive release builds) */

/* r101-marker: release build fix (DescRead out of the diag guard) + state log */
/*==========================================================================
 *  r101 修正(R-1): 描述符解码必须在【两种构建】里都存在
 *
 *  【r100 的实测故障】下面这两个函数原本写在上面那段 `#ifdef VBE9X_DIAG`
 *  里面, 而 display.c 的 DibDgValid/DibDgroupProbe 是【生产路径】——
 *  release 构建(`nmake DIAGFLAG= omninine.drv` = 用户的 B-1 测试)因此
 *  链接失败:
 *      Error! E2028: DescRead_ is an undefined reference
 *      file display.obj(display.obj): undefined symbol DescRead_
 *  ⇒ B-1(零日志构建)根本无法产出, 于是"冻结/花屏到底是不是日志造成的"
 *  这一问在 r100 那一轮【无法回答】(用户 ./100/release/error.txt 实录)。
 *
 *  【为什么不是"把 display.c 的调用点也 #ifdef 掉"】因为那两个函数的
 *  产出物 g_dibDgTrusted 决定 vddapi16.asm 的 DIBJUMP 桩要不要给
 *  DIBENG 装 DS —— 是【行为】不是日志(日志里 `DDI dg trusted=0000/0001`
 *  两种取值都出现过)。把它在 release 里删掉等于让两种构建行为不同。
 *  DPMI 000Bh 只读描述符、无副作用、不依赖任何诊断设施, 放出来是安全的。
 *========================================================================*/
/* --- 描述符解码(DPMI 000Bh 只读; 任何选择子都可安全询问) --------------- */
/* 【出参为什么必须是 __far 指针】本工程的内存模型(-mm 数据近指针 + -zu
 * SS≠DS)下, 编译器把 &栈局部量 当成 __far 指针(SS 相对), 而形参里写的
 * 普通指针是 DS 相对的近指针。漏写 __far 时编译器只给一条 W112("指针被
 * 截断"), 段基址被丢掉 —— 被调方写的是 DS:栈偏移, 也就是 DGROUP 里的
 * 某个位置: 调用方的局部量保持初值(后面读到垃圾), 同时 DGROUP 被踩坏。
 * r81 的取证代码正是这么写的, 只因 r80/r81 从未编译过而没暴露。       */
static int DiagDesc(uint16_t sel, uint32_t __far *pbase, uint32_t __far *plim,
                    BYTE __far *pacc, BYTE __far *pflg)
{
    /* [r447] 无效选择子直接拒绝: 0/1..7/FFFF 交给 DPMI 000Bh/0006h 时,
     * WinME 直接 #GP(0D), Win95 只是返回错误。DiagDesc 是最后一道闸门:
     * DiagSelProbe/DiagDumpAt 都直接调它, 绕过了 DescRead 的检查。 */
    if (sel == 0 || sel < 8 || sel == 0xFFFF)
        return 0;

    if (!DpmiGetDescriptor(sel))
        return 0;                           /* 无效选择子           */
    if (!(g_descBuf[5] & 0x80))
        return 0;                           /* 存在位=0: 不记       */

    *pacc  = g_descBuf[5];
    *pflg  = g_descBuf[6];
    *pbase = LinearOf(MK_FP(sel, 0));       /* DPMI 0006h: 32 位基址 */
    *plim  = (uint32_t)g_descBuf[0] | ((uint32_t)g_descBuf[1] << 8) |
             (((uint32_t)g_descBuf[6] & 0x0F) << 16);
    if (g_descBuf[6] & 0x80)                /* G=1: 界限以 4KB 计    */
        *plim = (*plim << 12) | 0x0FFFUL;
    return 1;
}

/* DescRead —— r94: 描述符只读解码(供 display.c 的 DGROUP 自证使用)。
 *   入参: sel = 任意选择子; 出参: 基址/界限/访问权字节(A 位含在 acc 里)。
 *   返回: 1 = 该选择子存在(描述符已填), 0 = 无效/不存在(出参不动)。
 * 【为何包一层】DiagDesc 是本文件的 static; display.c 需要同一套解码(它已
 *   被 r81..r93 的日志反复验证)。出参一律 __far *—— 见上面那条 W112 教训:
 *   本工程 -mm + SS≠DS 下, 普通指针形参会把基址丢掉并踩坏 DGROUP。     */
int __far DescRead(uint16_t sel, uint32_t __far *pbase, uint32_t __far *plim,
                   uint16_t __far *pacc)
{
    uint32_t base, lim;
    BYTE     acc, flg;
    /* [r447] 无效选择子绝不交给 DPMI: 0/1..7(空选择子)/FFFF 在 WinME 上
     * 会让 0006h/000Bh 直接 GP(0D), 而 Win95 只是返回错误。
     * map 证据: 崩溃点 0x9d6c 正落在 DescRead(0x9c2d..0x9d8e) 内。 */
    if (sel == 0 || sel < 8 || sel == 0xFFFF)
        return 0;

    if (!DiagDesc(sel, &base, &lim, &acc, &flg))
        return 0;
    *pbase = base;
    *plim  = lim;
    *pacc  = (uint16_t)acc | ((uint16_t)flg << 8);
    return 1;
}

#ifdef VBE9X_DIAG

/* 模块内存区判定 —— 字节转储的最后一道闸门。
 * 【依据】r79 的实测 LDT 地图: 模块段全部落在低 1MB(KERNEL/USER/GDI 的
 * 低端段, base 0x0006xxxx..0x0009xxxx)或 0x804E0000..0x80560000 一带;
 * 而 DPMI 的惰性大堆在 0x81F48000 起。两段之间留足余量, 中间一律不碰。*/
static int DiagIsModuleMem(uint32_t base)
{
    if (base < 0x00100000UL)
        return 1;
    if (base >= 0x80000000UL && base < 0x80800000UL)
        return 1;
    return 0;
}

/* DiagDumpAt —— 把 sel:off 起的 DIAG_DUMP_LEN 字节抄成一行十六进制。
 * 【六道闸门, 一条都不能省】存在 / S=1 / DPL=3 / (代码段必须可读) /
 * 界限覆盖 / 落在模块内存区且 < 64KB(大段必是 DPMI 堆)。任何一条不过
 * 就整行不打 —— 宁可少一行, 不再冒第二次 0E 的风险。
 * 【relaxBig 参数只给"蓝屏现场"用(r83)】地址由用户从蓝屏屏上抄来, 说明
 * 那条指令刚刚被执行/取指过, 对应页【必然已提交】, 于是 <64KB 这条可以
 * 放宽; 真正拦住 r79 那次 0E 的是"基址必须落在模块内存区"这条(0x81F48000
 * 起的 DPMI 惰性堆正是被它挡在外面的), 它依然在。                  */
static void DiagDumpAt(const char *tag, uint16_t sel, uint32_t off, int relaxBig)
{
    uint32_t    base, lim;
    BYTE        acc, flg;
    BYTE __far *fp;
    char       *p;
    int         i;

    /* [r447] 开机首次 SetDisplayMode 期间不做描述符转储:
     * DiagDesc 走 DPMI 000Bh, WinME 在模式切换窗口中调用会 0D。 */
    if (!LogReady)
        return;

    if (!DiagDesc(sel, &base, &lim, &acc, &flg))
        return;
    if ((acc & 0x10) == 0)                      /* S=0: 系统段     */
        return;
    if ((acc & 0x60) != 0x60)                   /* DPL != 3        */
        return;
    if ((acc & 0x08) && !(acc & 0x02))          /* 代码段不可读    */
        return;
    if (lim < off + g_dumpLen)                  /* 界限不够        */
        return;
    if (!DiagIsModuleMem(base))                 /* 非模块内存      */
        return;
    if (!relaxBig && lim >= 0x00010000UL)       /* ≥64KB: 必是堆   */
        return;

    fp = (BYTE __far *)MK_FP(sel, (uint16_t)off);
    p  = g_ldtBuf;
    p  = DiagPutS(p, tag);
    p  = DiagPutS(p, " sel="); p = DiagPutX4(p, sel);
    p  = DiagPutS(p, " off="); p = DiagPutX4(p, (uint16_t)off);
    p  = DiagPutS(p, " :");
    for (i = 0; i < g_dumpLen; ++i)
    {
        *p++ = ' ';
        p = DiagPutX2(p, fp[i]);
    }
    *p = 0;
    DiagWriteLog(g_ldtBuf);
}

/* DiagLogPtr —— 一行 "标签=SSSS:OOOO"(报告"某个入口在哪个段"的最短形式) */
void __far DiagLogPtr(const char *tag, uint32_t p)
{
    char *q = g_ldtBuf;
    int   n = 0;

    while (*tag && n < 64)                      /* 截断保护: 缓冲必够 */
    {
        *q++ = *tag++;
        n++;
    }
    *q++ = '=';
    q = DiagPutX4(q, (uint16_t)(p >> 16));
    *q++ = ':';
    q = DiagPutX4(q, (uint16_t)p);
    *q = 0;
    DiagWriteLog(g_ldtBuf);
}

/* --- 模块数据库(NE)读取: 与 display.c GetModuleDgroupSel 同一套校验 -----
 * 布局来源: Pietrek《Windows Internals》p.219 的 Win95 模块数据库转储
 * (wine 的 NE_MODULE 同款, 自 Win3.1 冻结至今):
 *     +0x00 WORD 'NE'         +0x08 WORD DGROUP 段表项近指针(可 0)
 *     +0x0E WORD DGROUP 逻辑段号   +0x1C WORD 段数 cseg
 *     +0x22 WORD 段表近指针        +0x24 WORD 资源表近指针
 * 段表每项 8 字节(个别版本 10), 首字 = 该段的选择子(未加载为 0)。
 * 【本函数只做"读 + 校验 + 拒绝"】任何一项对不上就返回 0 —— 宁可不打印,
 * 也绝不产生野 far 指针(v1 曾按磁盘 NE 头解读内存态模块库, 一读就 GP)。*/
static int DiagNeLayout(uint16_t hMod, uint16_t __far *pcseg,
                        uint16_t __far *psegTab, uint16_t __far *pstride,
                        uint16_t __far *pdgSel)
{
    BYTE     __far *db;
    WORD            autoSeg, cseg, segTab, resTab, dgroupSte, stride;
    uint32_t        base, lim;
    BYTE            acc, flg;

    if (!hMod)
        return 0;

    db    = (BYTE __far *)MK_FP(hMod, 0);
    if (*(WORD __far *)db != 0x454E)            /* 'NE' */
        return 0;
    autoSeg   = *(WORD __far *)(db + 0x0E);
    cseg      = *(WORD __far *)(db + 0x1C);
    segTab    = *(WORD __far *)(db + 0x22);
    resTab    = *(WORD __far *)(db + 0x24);
    dgroupSte = *(WORD __far *)(db + 0x08);

    if (!autoSeg || !cseg || !segTab || autoSeg > cseg)
        return 0;
    if (resTab <= segTab || (WORD)(resTab - segTab) % cseg != 0)
        return 0;
    stride = (WORD)(resTab - segTab) / cseg;
    if (stride != 8 && stride != 10)
        return 0;
    if (dgroupSte >= segTab && dgroupSte < resTab &&
        dgroupSte != (WORD)(segTab + (autoSeg - 1) * stride))
        return 0;

    /* 【r81 新增的安全闸门】整张段表必须落在模块库段自身的界限之内 ——
     * 否则遍历到后半截就是越界读, 那是一次 #GP。界限读不出来就不展开。*/
    if (!DiagDesc(hMod, &base, &lim, &acc, &flg))
        return 0;
    if ((uint32_t)segTab + (uint32_t)cseg * stride > lim)
        return 0;

    if (cseg > 64)                              /* 防御: 异常大的段数 */
        cseg = 64;

    *pcseg   = cseg;
    *psegTab = segTab;
    *pstride = stride;
    *pdgSel  = (autoSeg && autoSeg <= cseg)
             ? *(WORD __far *)(db + segTab + (autoSeg - 1) * stride)
             : 0;
    return 1;
}

/* DiagSegLine —— 一行 "SEGM <mod>#nn sel=.. base=.. lim=.. acc=.. flg=.."。
 * 段表里的选择子必然指向加载器分配过的段(已提交、DPL=3), 所以这里可以
 * 放心取描述符; 覆盖了 g_dumpOff 的段再顺带抄机器码。               */
static void DiagSegLine(const char *mod, uint16_t idx, uint16_t sel)
{
    uint32_t base, lim;
    BYTE     acc, flg;
    char    *p = g_ldtBuf;

    if (!DiagDesc(sel, &base, &lim, &acc, &flg))
        return;

    p = DiagPutS(p, "SEGM ");
    p = DiagPutS(p, mod);
    *p++ = '#';
    p = DiagPutX2(p, idx);
    p = DiagPutS(p, " sel=");  p = DiagPutX4(p, sel);
    p = DiagPutS(p, " base="); p = DiagPutX8(p, base);
    p = DiagPutS(p, " lim=");  p = DiagPutX8(p, lim);
    p = DiagPutS(p, " acc=");  p = DiagPutX4(p, (uint16_t)acc);
    p = DiagPutS(p, " flg=");  p = DiagPutX4(p, (uint16_t)flg);
    *p = 0;
    DiagWriteLog(g_ldtBuf);

    if (lim >= (uint32_t)g_dumpOff + g_dumpLen)
        DiagDumpAt("DUMP", sel, g_dumpOff, 0);
}

/* DiagModSegs —— 一个模块的全部已加载段(名字 + 选择子 + 描述符 + 转储)。
 * 【这是 r81 的主力取证】它把"崩溃地址 SSSS:OOOO"从"某个陌生选择子"
 * 变成"<模块> 的第 n 个段, 该段在偏移 X 处的机器码是 ..."。         */
static void DiagModSegs(const char *name)
{
    uint16_t    hMod = GetModuleHandle(name);
    uint16_t    cseg, segTab, stride, dgSel, i;
    BYTE __far *db;
    char       *p;

    p = g_ldtBuf;
    p = DiagPutS(p, "MOD ");
    p = DiagPutS(p, name);
    if (!hMod)
    {
        p = DiagPutS(p, " = 0 (not loaded)");
        *p = 0;
        DiagWriteLog(g_ldtBuf);
        return;
    }
    p = DiagPutS(p, " hMod=");   p = DiagPutX4(p, hMod);
    p = DiagPutS(p, " base=");   p = DiagPutX8(p, LinearOf(MK_FP(hMod, 0)));
    *p = 0;
    DiagWriteLog(g_ldtBuf);

    if (!DiagNeLayout(hMod, &cseg, &segTab, &stride, &dgSel))
    {
        DIAG("MOD: module database layout unrecognized -> not walked");
        return;
    }
    db = (BYTE __far *)MK_FP(hMod, 0);
    DiagHex("MOD cseg", cseg);
    DiagHex("MOD stride", stride);
    DiagHex("MOD dgroup sel", dgSel);
    DiagHex("MOD dgroup base lo",
            (uint16_t)LinearOf(MK_FP(dgSel, 0)));
    DiagHex("MOD dgroup base hi",
            (uint16_t)(LinearOf(MK_FP(dgSel, 0)) >> 16));

    for (i = 0; i < cseg; ++i)
    {
        uint16_t sel = *(WORD __far *)(db + segTab + i * stride);
        if (sel == 0 || sel == 0xFFFF)
            continue;
        DiagSegLine(name, i, sel);
    }
}

/* DiagLdtOne —— 打印一个 LDT 选择子的描述符(只读描述符本身)。
 * 【r81 把读段内容的部分整段删掉了】r79 在这里对【任意】合法段读 0x2B60
 * 处 48 字节, 读到 DPMI 惰性堆(0x81F48000 起)的未提交页 → #PF → 蓝屏
 * 0E(031F:0000296C 就在本函数里)。见本轮头的"原则一"。
 * LDT 盲扫只保留描述符: DPMI 000Bh 读的是描述符表, 不碰被描述的页,
 * 对任何选择子都安全 —— 而模块地图正是靠它才画出来的。            */
static void DiagLdtOne(uint16_t sel)
{
    char    *p = g_ldtBuf;
    uint32_t base, lim;
    BYTE     acc, flg;

    if (!DiagDesc(sel, &base, &lim, &acc, &flg))
        return;                             /* 无效/不存在: 不记 */

    p = DiagPutS(p, "LDT sel=");  p = DiagPutX4(p, sel);
    p = DiagPutS(p, " base=");    p = DiagPutX8(p, base);
    p = DiagPutS(p, " lim=");     p = DiagPutX8(p, lim);
    p = DiagPutS(p, " acc=");     p = DiagPutX4(p, (uint16_t)acc);
    p = DiagPutS(p, " flg=");     p = DiagPutX4(p, (uint16_t)flg);
    *p = 0;
    DiagWriteLog(g_ldtBuf);

    /* 【r81: 这一段"抄机器码"的代码已整体删除】
     * r79 在这里对【任意】合法段(存在/S=1/DPL=3/可读/界限够)读 0x2B60 处
     * 48 字节, 结果读进 DPMI 惰性堆段的未提交页 → #PF → 蓝屏 0E。
     * 转储现在只对"有出处的段"执行(见 DiagCodeWin / DiagSegLine /
     * DiagModSegs), 那些段必然由加载器分配并提交过。                 */
}

/* (r78/r79 的 DiagModuleOne 已被 r81 的 DiagModSegs 取代并删除:
 *  旧版只打印 hMod 与基址, 新版把模块段表整张展开, 见上。)        */

/* DiagLdtCodeDump —— 【r83 新增: 让蓝屏现场自己现身】
 *   把 LDT 扫描区间里【每一段可执行、可读、DPL=3】的代码段在 g_dumpOff
 *   处的机器码各抄一行(其余闸门由 DiagDumpAt 内部把守)。
 *   【为什么必须扫 LDT, 而不是只扫模块段表】r76b..r82 五轮的蓝屏偏移恒为
 *   0x2B70(= DumpOff + 0x10), 段号却每轮都变(0427 → 042F): 它是某个模块
 *   【代码段】里的固定位置, 而那个段既不在我们 walk 到的模块段表里
 *   (DIBENG 只 walk 到 5 段、GDI 只 1 段 —— 段表读取本就不可靠), 也没有
 *   任何 PTR 行指向它 → r82 的 DUMP 一行都没打出来, 蓝屏地址至今只能靠
 *   "猜模块"。把 LDT 里所有像代码的段都抄一遍, 真正的崩溃段必然在其中,
 *   于是不必再等"用户从蓝屏抄段号 → 配 Crash= → 再启动"这一轮。
 *   【安全性】只读【代码段】(E=1 且可读), 且基址必须落在模块内存区;
 *   r79 的 0E 是读数据段盲扫命中的 DPMI 惰性堆(0x81F48000 起)造成的,
 *   那条路径被基址闸门整段排除。r82 已用同一套闸门对 13 个模块段跑通
 *   (日志完整、无异常), 本函数只是把同样的读取扩到 LDT 的代码段类。
 *   【量】实测区间内约 29 个段通过, 每段 1 行 × ~200 字节 ≈ 6KB 日志。 */
static void DiagLdtCodeDump(void)
{
    int idx;

    for (idx = 96; idx <= 216; ++idx)
    {
        uint16_t sel = (uint16_t)(((uint16_t)idx << 3) | 7);
        uint32_t base, lim;
        BYTE     acc, flg;

        if (!DiagDesc(sel, &base, &lim, &acc, &flg))
            continue;
        if ((acc & 0x10) == 0)                  /* 系统段          */
            continue;
        if ((acc & 0x08) == 0)                  /* 数据段: 不读    */
            continue;
        if ((acc & 0x02) == 0)                  /* 代码段不可读    */
            continue;
        if ((acc & 0x60) != 0x60)               /* DPL != 3        */
            continue;
        DiagDumpAt("DUMP", sel, g_dumpOff, 0);

        /* 【r87】再加一窗: 紧接第一窗【之后】。r86 的教训是"修掉一层毛病后,
         * 崩溃点会后移到同一条指令之后" —— 那时最需要看的正是"后续代码",
         * 而单窗只给到出事点之前的 512 字节。两窗连起来 = 1KB, 足够看清一个
         * 函数后续分支里到底 call 了谁。                                */
        DiagDumpAt("DUMP2", sel, (uint32_t)g_dumpOff + g_dumpLen, 0);
    }
}

/* DiagCrashSite —— 【r83】把用户从蓝屏抄来的 SSSS:OOOO 变成机器码。
 *   描述符一行 + 该处 DIAG_DUMP_LEN 字节。同一选择子重复喂入只打一次
 *   (display.c 会把 DIBENG 各入口所在的段逐个喂进来, 做"同一偏移在各段
 *   里长什么样"的横向对比: 若该偏移在别的代码段里是正常指令序列, 而崩溃
 *   段不是那些段, 就是"选择子被写坏"的直接证据)。                  */
void __far DiagCrashSite(uint16_t sel, uint16_t off)
{
    static uint16_t lastSel = 0;
    uint32_t base, lim;
    BYTE     acc, flg;
    char    *p;

    if (sel == 0 || sel == lastSel)
        return;
    lastSel = sel;

    if (!DiagDesc(sel, &base, &lim, &acc, &flg))
    {
        /* 【r107】000Bh 拒绝 ≠ "这个段号不存在" —— 它也可能是【GDT 选择子】
         * (见本文件上方 r78 的实测记录: 000Bh 对 GDT 一律拒)。/104 的
         * `3A43:3D726964` 的 3A43 正是这种情况(RPL=3 且 TI=0 ⇒ GDT),
         * 于是此前这一行日志等于什么都没说。现在补一条探针: 先 DPMI 0006h
         * 要基址, SelProbe=1 时再补 LAR/LSL。                       */
        DIAG("CRASH: selector not present in LDT (see SELPROBE below)");
        DiagSelProbe(sel);
        return;
    }

    p = g_ldtBuf;
    p = DiagPutS(p, "CRASH sel=");  p = DiagPutX4(p, sel);
    p = DiagPutS(p, " base=");      p = DiagPutX8(p, base);
    p = DiagPutS(p, " lim=");       p = DiagPutX8(p, lim);
    p = DiagPutS(p, " acc=");       p = DiagPutX4(p, (uint16_t)acc);
    p = DiagPutS(p, " flg=");       p = DiagPutX4(p, (uint16_t)flg);
    *p = 0;
    DiagWriteLog(g_ldtBuf);

    /* 【r85】窗口【以出事点结尾】而不是从它开始: 我们要看的是"这条指令
     * 属于哪个函数、它前面刚刚 call 了什么", 而不是它后面的字节。     */
    {
        uint32_t start = ((uint32_t)off + 16 > (uint32_t)g_dumpLen)
                       ? ((uint32_t)off + 16 - (uint32_t)g_dumpLen) : 0;

        DiagDumpAt("CRASHCODE", sel, start, 1);
    }
}

/* DiagDumpWin —— r96: 把任意"有出处的"段在 off 处的 DIAG_DUMP_LEN 字节抄一行。
 *   与 DiagCrashSite 的区别: ① 不做"同一选择子只打一次"的去重(display.c 要
 *   对同一个段按【相对入口】的多个偏移各抄一窗); ② 窗口从 off 起算(不是以
 *   出事点结尾); ③ relaxBig=0 —— 闸门全开(存在/S=1/DPL=3/可读/界限够/
 *   模块内存区/小于 64KB), 一条都不放宽。
 *   【用途】DIBENG 的入口地址每台机器都不同(选择子由加载器决定), 但
 *   "入口前 0x40/0x80/0xC0 字节"在任何机器上都是同一段指令 —— 于是把
 *   调用方给出的相对偏移窗口抄下来, 就等于把引擎那段函数体搬进日志,
 *   下一轮可以直接对着字节反汇编, 不必再让用户抄蓝屏地址。            */
void __far DiagDumpWin(uint16_t sel, uint16_t off)
{
    DiagDumpAt("DIBCODE", sel, off, 0);
}

/*==========================================================================
 *  DiagSegMap —— r81 的主力取证入口(Enable 末尾调用)
 *
 *  【它回答的问题】蓝屏地址 SSSS:OOOOOOOO 是谁? 那里是什么指令?
 *  【它怎么回答】三份信息, 全部来自公开接口:
 *    ① PTR 行: 本模块各导出函数/DIBENG 各入口的 sel:off —— 崩溃段若命中
 *       其中任何一个, 凶手与"在哪个入口的哪个偏移"当场确定;
 *    ② MOD/SEGM 行: 每个模块数据库(GetModuleHandle)+ 其段表里【全部已
 *       加载段】的选择子/基址/界限/访问权 —— 崩溃段若命中, 就知道是哪个
 *       模块的第几段; 覆盖 g_dumpOff 的段还会跟一行 DUMP 机器码;
 *    ③ LDT 行: 0x0307..0x06DF 区间所有存在描述符(只读描述符表, 安全),
 *       用于兜住"不在任何模块段表里"的选择子(如 GDI 临时段)。
 *  【r81 与 r78/r79 的差别】不再对 LDT 段做盲扫字节转储(那是 r79 蓝屏
 *  0E 的直接原因), 转储只对 ①② 这两类"有出处的段"执行。
 *========================================================================*/
void __far DiagSegMap(void)
{
    int      idx;
    uint32_t dgSel, dgBase;

    DIAG("MAP r81: provenance segment map (no blind LDT byte reads)");
    DiagHex("MAP DumpOff", g_dumpOff);

    /* ① 本模块自己的段 —— 出处: 真实函数/全局变量的远地址 */
    dgSel  = FP_SEG((const void far *)&g_state);
    dgBase = LinearOf(&g_state);
    DiagHex("MAP ours dg sel", (uint16_t)dgSel);
    DiagHex("MAP ours dg base lo", (uint16_t)dgBase);
    DiagHex("MAP ours dg base hi", (uint16_t)(dgBase >> 16));
    DiagLogPtr("PTR ours LfbAccessOffset", DIAGFN(LfbAccessOffset));
    DiagLogPtr("PTR ours VBE_SetMode",     DIAGFN(VBE_SetMode));
    DiagLogPtr("PTR ours DAC_SetPalette",  DIAGFN(DAC_SetPalette));
    DiagLogPtr("PTR ours VDD_StopIOTrap",  DIAGFN(VDD_StopIOTrap));

    /* ② 各模块数据库 + 段表(顺序按 bootlog 的加载次序) */
    DiagModSegs("DISPLAY");     /* 本模块(modname=DISPLAY) */
    DiagModSegs("DIBENG");
    DiagModSegs("GDI");
    DiagModSegs("USER");
    DiagModSegs("KERNEL");

    /* ③ LDT 描述符清单(纯描述符读取, 不碰任何被描述的页)
     * 【r79 修正 r78 的空转】选择子不是 (index<<3) 而是 (index<<3)|7:
     * bit2(TI)=1 表示走 LDT, 低 2 位(RPL)=3。本驱动代码段 0x031F、DGROUP
     * 0x0327、屏选择子 0x2077、DIBENG 各入口 0x0357/0x0367/…
     * 全部以 7 结尾。r78 按 "0x320 起每 +8" 扫的是 GDT 选择子(TI=0),
     * DPMI 000Bh 一律拒之 —— 所以那轮一条 LDT 行都没打印出来。
     * r81 把上界从 200 放宽到 216(0x0307..0x06C7), 覆盖到实测出现的
     * 0x061F(索引 195), 再留一截余量。                              */
    for (idx = 96; idx <= 216; ++idx)
        DiagLdtOne((uint16_t)(((uint16_t)idx << 3) | 7));

    /* ④ 【r83】LDT 里所有代码段在 g_dumpOff 处的机器码 —— 蓝屏现场。
     * ①②③ 只能"指认段号", 这一份直接给出【出事那条指令的字节】。 */
    DiagLdtCodeDump();

    DIAG("MAP: done");
}

/* 兼容别名: r78/r79 的调用点(display.c)按旧名调用 */
void __far DiagModuleMap(void)
{
    DiagSegMap();
}

void __far LfbSelRecheck(void)
{
    uint32_t base;

    if (!g_lfbSel)
    {
        DIAG("LfbRecheck: no selector");
        return;
    }
    if (!DpmiGetDescriptor(g_lfbSel))
    {
        DIAG("LfbRecheck: DPMI 000Bh failed");
        return;
    }
    /* 【r79 修正 r78 的误报】描述符的基址是 32 位: byte2=base7:0,
     * byte3=base15:8, byte4=base23:16, **byte7=base31:24**。r78 漏了
     * byte7, 于是把 0xD3E1C000 读成 0x00E1C000 并报出 "BASE CHANGED"
     * —— 那是本函数自己的 bug, 选择子其实没被动过。此处补齐 4 个字节。*/
    base = (uint32_t)g_descBuf[2] | ((uint32_t)g_descBuf[3] << 8) |
           ((uint32_t)g_descBuf[4] << 16) | ((uint32_t)g_descBuf[7] << 24);

    DiagHex("LfbRecheck: sel", g_lfbSel);
    DiagHex("LfbRecheck: base hi", (uint16_t)(base >> 16));
    DiagHex("LfbRecheck: base lo", (uint16_t)base);
    DiagHex("LfbRecheck: acc", (uint16_t)g_descBuf[5]);
    DiagHex("LfbRecheck: want hi", (uint16_t)(g_lfbSelBase >> 16));
    DiagHex("LfbRecheck: want lo", (uint16_t)g_lfbSelBase);
    /* 交叉核对: DPMI 0006h 独立给出的基址(两路一致才可信) */
    DiagHex("LfbRecheck: base2 hi", (uint16_t)(LinearOf(MK_FP(g_lfbSel, 0)) >> 16));
    DiagHex("LfbRecheck: base2 lo", (uint16_t)LinearOf(MK_FP(g_lfbSel, 0)));

    if (!(g_descBuf[5] & 0x80))
        DIAG("LfbRecheck: DESCRIPTOR NOT PRESENT (VDD/host changed it!)");
    else if (base != g_lfbSelBase)
        DIAG("LfbRecheck: BASE CHANGED (VDD/host changed it!)");
    else
        DIAG("LfbRecheck: ok (untouched)");
}

#else
/* 非诊断构建: 全部取证入口都是空函数(零开销、零行为变化) */
void __far DiagModuleMap(void) { }
void __far DiagSegMap(void) { }
void __far LfbSelRecheck(void) { }
void __far DiagSetDumpOff(uint16_t off) { (void)off; }
void __far DiagSetDumpLen(uint16_t len) { (void)len; }
void __far DiagLogPtr(const char *tag, uint32_t p) { (void)tag; (void)p; }
void __far DiagCrashSite(uint16_t sel, uint16_t off) { (void)sel; (void)off; }
#endif

/*==========================================================================
 *
 *  VxD 嵌套直通生产路径 (r36)
 *
 *  【背景】./23 实锤: 系统 VM 的直接 INT 10h VESA 调用(4F00/4F01/4F02)
 *  被假成功层吞掉(AX=004F 但缓冲不动, 经典功能正常)。./35 实锤: 经
 *  mini-VDD 借道 IVT[0x63] 的嵌套 V86 直通拿到真 "VESA" 数据。故全部
 *  VBE BIOS 调用统一走 VbeExecBios(); 直通不可用时回退直接 INT 10h。
 *
 *========================================================================*/
static VBEEXECREQ g_xreq;           /* 静态: 需稳定线性地址给 VxD       */
static uint32_t   g_xlin = 0;       /* g_xreq 的线性地址(初始化时缓存) */
static int        g_xstate = 0;     /* 0=未初始化 1=可用 -1=不可用       */
static uint16_t   g_v86SegClient = 0; /* VxD 中转缓冲段(枚举预置用)     */
static uint16_t   g_v86OkVal = 0;   /* 0=未取 1=可用(来自 GET_MAPINFO)  */
/* 【r138】上一次 VbeExecBios 的【完整】输出寄存器组。
 *   为什么要有它: 见 vbeshare.h 里 VBEEXECREQ 的尾注 —— 决定"画面按什么
 *   像素格式扫出来"的那份状态不在 VGA 寄存器里, 唯一通用地问到它的通路
 *   是 VBE 自己的读回功能(4F03h 当前模式 / 4F06h 每行字节数 / 4F07h 显示
 *   起始), 而它们的结果都在 BX/CX/DX 里。旧版请求块只搬 AX 与 flags, 所以
 *   ring 3 侧从来没有过"硬件此刻在什么模式"的读数 —— ./137 的
 *   `Verify: vxd mode=0117` 读的是我们【自己】的软件镜像, 于是 pre-verify
 *   恒 ok, `mode already correct, no 4F02 issued` 在每个会话入口边沿都出现,
 *   而用户看到的是一屏条纹。
 *   本组量只由 VbeExecBios 在每次直通返回后写入, 只读不推断。          */
static uint16_t   s_xoutBx = 0;
static uint16_t   s_xoutCx = 0;
static uint16_t   s_xoutDx = 0;
static uint16_t   s_xoutSi = 0;
static uint16_t   s_xoutDi = 0;
static uint16_t   s_xoutEs = 0;
/* 【r138】4F03h / 4F06h 三个探针读数。0xFFFF = 从未取到(或取失败)。 */
static uint16_t   g_probeMode   = 0xFFFF;   /* 4F03h: 当前模式号   */
static uint16_t   g_probePitchB = 0xFFFF;   /* 4F06h: 字节/扫描行  */
static uint16_t   g_probePitchP = 0xFFFF;   /* 4F06h: 像素/扫描行  */
/* 【r138】4F07h 显示起始(get 形态, 只作证据)。 */
static uint16_t   g_probeStartLine  = 0xFFFF;
static uint16_t   g_probeStartPixel = 0xFFFF;
static uint16_t   g_probeStartOk    = 0;
static uint16_t   g_statDumped = 0xFFFF; /* 上次转储过的 stat 值         */
/* r46: VBE 数据中转的客户端侧缓冲(DGROUP 静态)。VxD 经 bufLen/bufLin
 * 线性拷贝与之交换数据 —— 取代旧版 DOS 低内存缓冲+自建选择子(./45
 * 实锤: 经自建选择子写低内存在本机 Enable 期 GP 死机)。            */
static uint8_t    s_enumBuf[1024];  /* 4F00: 信息块 512B + 模式表 512B  */
static uint8_t    s_modeBuf[256];   /* 4F01: MODEINFOBLOCK             */
static uint8_t    s_infoBuf[512];   /* 4F00(PushState 取 VBE 总信息)    */

/* 转储 GET_MAPINFO 的结果(只在 stat 位图变化时打一次, 避免刷屏)。
 * VxD 在回报前会惰性建立 V86 中转缓冲(EnsureV86), 故每次本调用
 * 都是一次分配重试 —— 分配失败的旧状态不会被缓存锁死。
 * 【r76】字段瘦身: 只报告生产路径上的量(缓冲段/可用/里程碑位图/
 * 状态机步码/子码快照); r69-r75 的阶梯/ground-truth/MMGR 探针
 * 字段随 VxD 侧的实验代码一并删除。                              */
static void VbeQueryMapinfo(void)
{
    int ok;
    g_xreq.in_ax = 0;  g_xreq.in_bx = 0;  g_xreq.in_cx = 0;
    g_xreq.in_dx = 0;  g_xreq.in_si = 0;  g_xreq.in_di = 0;
    g_xreq.in_ds = 0;  g_xreq.in_es = 0;  g_xreq.stubSeg = 0;
    g_xreq.stubOff = 0; g_xreq.vec = 0;   g_xreq.out_ax = 0;
    g_xreq.out_flags = 0; g_xreq.bufLen = 0; g_xreq.bufLin = 0;
    g_xreq.v86Seg = 0; g_xreq.v86Ok = 0; g_xreq.stat = 0;
    g_xreq.step = 0;   g_xreq.lastEax = 0; g_xreq.stubLin = 0;
    VxDCallApi(VBE_API_GET_MAPINFO, g_xlin, sizeof(g_xreq), &ok);
    g_v86SegClient = g_xreq.v86Seg;
    g_v86OkVal     = g_xreq.v86Ok;

    if (g_statDumped != g_xreq.stat)
    {
        g_statDumped = g_xreq.stat;
        DiagHex("MapInfo: stat",   g_xreq.stat);   /* VBE_VXD_STAT_* 位图 */
        DiagHex("MapInfo: step",   g_xreq.step);   /* InitStep 状态机步码 */
        DiagHex("MapInfo: v86seg", g_xreq.v86Seg); /* V86 中转缓冲段     */
        DiagHex("MapInfo: v86ok",  g_xreq.v86Ok);  /* 缓冲可用           */
        DiagHex("MapInfo: lastEax lo", (uint16_t)g_xreq.lastEax);
        DiagHex("MapInfo: lastEax hi", (uint16_t)(g_xreq.lastEax >> 16));
        FlushLog();
    }
}

/* 惰性初始化: 连接 VxD、定位请求块、首次查询并转储映射/缓冲状态。
 * 只做一次; 失败置 -1, 本次开机不再重试(上层如实失败 → 回退 VGA)。
 * 【r46】不再有 DOS 内存分配/选择子创建/桩预写 —— 那些全部移入 VxD
 * (./45 实锤: 客户端自建选择子写低内存在本机 Enable 期 GP 死机)。 */
static int VbeExecEnsure(void)
{

    if (g_xstate != 0)
        return g_xstate > 0;
    g_xstate = -1;                      /* 先置负, 逐级通关后才翻正     */

    if (!VxD_Connect())
        return 0;                       /* VxD 不在 → 全程不可用        */

    g_xlin = LinearOf(&g_xreq);
    if (g_xlin == 0)
        return 0;

    /* 【r48】GET_MAPINFO 查询(归零请求块 → 调用 → 缓存 v86Seg/v86Ok)。
     * VxD 侧在本 API 回报前惰性建立 V86 中转缓冲(EnsureV86), 故本
     * 查询同时是"分配重试"。Device_Init 期分配的 r47 旧方案已废:
     * ./47 实锤本机 Device_Init 期嵌套 DOS 调用失败(103)。结果转储
     * 由 VbeQueryMapinfo 内部按 stat 变化触发(不再只打一次)。       */
    VbeQueryMapinfo();

    g_xstate = 1;
    return 1;
}

/* 缓冲就绪门槛(r48): 各 VBE 调用门前的统一判定。首次走 VbeExecEnsure
 * (含一次性日志转储); 未就绪则再发一次 GET_MAPINFO —— VxD 在回报
 * 前会重试惰性分配(EnsureV86), 早期失败(Enable 初期 DOS 繁忙等)
 * 因此不锁死整个会话。                                            */
static int VbeV86Ok(void)
{
    if (!VbeExecEnsure())
        return 0;
    if (g_v86OkVal == 1)
        return 1;
    VbeQueryMapinfo();
    return g_v86OkVal == 1;
}

/* 直通执行一次 VBE BIOS 调用(AX=4Fxx 族)。
 * 入口: 各输入寄存器; buf/bufBytes != 0 时启用 VxD 数据中转(执行前
 *       VxD 把 buf 处 bufBytes 字节拷入其 V86 缓冲并让 BIOS 指向它,
 *       执行后整批拷回 —— BIOS 结果直接在 buf 里, 全经线性地址)。
 * 返回: BIOS 的 AX(==0x004F 为成功); *pfl = 返回 flags(bit0=CF)。
 *       直通通道本身失败返回 0xFFFF(==0x004F 检查自然不成立)。
 * 【r46】桩由 VxD 每次执行前自写(callf 目标当次现取真 V86 IVT[10h],
 *  VDD 若在会话中重定向 10h 也能跟随), 客户端零预写、零选择子、
 *  零低内存接触。                                                */
static uint16_t VbeExecBios(uint16_t ax, uint16_t bx, uint16_t cx,
                            uint16_t dx, uint16_t si, uint16_t di,
                            uint16_t ds, uint16_t es,
                            void *buf, uint16_t bufBytes, uint16_t far *pfl)
{
    int ok;

    if (!VbeExecEnsure())
        return 0xFFFF;

    /* 【r111 闩】同一时刻只允许一次直通: 请求块 g_xreq 是共享静态量,
     * 两层同时用会让外层读到内层的结果(见 s_biosBusy 的注)。被闩住时
     * 只写内存环(回调期不写日志), 并置与"护栏拒绝"同一个标志 —— 调用
     * 方(回落层/官方路径)据此知道"这次不是硬件失败", 从而走重挂。  */
    if (s_biosBusy)
    {
        s_biosRefused = 1;
        g_cbBiosBusy++;
        CbRingPut( (uint16_t)((CB_EVT_BIOS_BUSY << 8) | (g_cbBiosBusy & 0xFF)) );
        /* 【r138】把"被自己的闩拒绝"变成一行能读的证据。
         *   ./137 的日志里这条拒绝【只】留下了内存环记录(CB evt=0D01..0D07
         *   共 7 次), 文本侧一行都没有 —— 于是 3721/3723/3725 那三次
         *   `VBE_SetMode: exec ax=FFFF` 看起来像"硬件说不", 实际是"我们
         *   自己没让这次调用出去"。两者对下一轮的含义完全相反:
         *     · 硬件说不 ⇒ 该换做法(改时序/改模式号/改寄存器);
         *     · 自己闩住 ⇒ 该改【时机】(把这次重设推到上下文干净之后再发)。
         *   PHASEV 是无条件通道, release 构建里同样落盘; 它在回调窗口里会
         *   被日志闸门挡掉(那时打日志本身是重入), 而"挡掉"同样是可读的
         *   (DiagSkipCb 计数), 不会伪装成"没发生"。                    */
        PHASEV( "PH: biosbusy=", g_cbBiosBusy );
        return 0xFFFF;
    }

    /* 【r100 硬护栏 / r111 一次性通行证】回调上下文里默认绝不发嵌套
     * V86/BIOS 调用: 真机 x230 的三次蓝屏/冻结(0E @0028:C00315BC /
     * 0D @0147:0000000E / 调用不返回)全部落在"ResetHiResMode 回调里那次
     * 4F02"上 —— 那正是主 VDD 自己正在走的通路, 而且回调可能切进我们任意
     * 一行代码中间。这里拒绝, 并且只写内存环(绝不打日志: 回调期 DOS 调用
     * 是重入)。
     * 【r111】例外只有一条: 显示驱动按微软文档在同一个回调里做"官方恢复"
     * 时, 先领一张一次性通行证(VxdAllowBios) —— 领走即作废, 于是
     * 嵌套的第二层仍然被这道护栏挡住。
     * 【r112】第二个例外: s_inRebuild —— 那是"我们自己的重建序列正在跑"
     * 的那几拍, 回调恰好在中间切进来时护栏同样会误伤自己(./111 t5 实锤:
     * 4F02 成功之后的 4F01 被拒 ⇒ SDM fail 4 ⇒ 空转重挂 8 次)。        */
    if (s_inVddCallback && !s_inRebuild)
    {
        if (!s_biosAllow)
        {
            s_biosRefused = 1;      /* r110: 让重建方知道"是被护栏拒的" */
            CbRingPut( 2 );         /* 2 = BIOS 调用被护栏拒绝 */
            /* 【r138】与上面的"闩拒绝"分开留痕: 两者都属于"我们自己没让
             *   这次调用出去", 而不是"硬件说不"。见 s_biosBusy 处的注。 */
            g_cbGuardRefuse++;
            PHASEV( "PH: biosguard=", g_cbGuardRefuse );
            return 0xFFFF;
        }
        s_biosAllow--;              /* r111: 从官方路径领来的有界预算里扣 */
        s_cbBiosUsed++;
        g_cbBiosUsed = s_cbBiosUsed;
    }

    g_xreq.in_ax = ax;  g_xreq.in_bx = bx;
    g_xreq.in_cx = cx;  g_xreq.in_dx = dx;
    g_xreq.in_si = si;  g_xreq.in_di = di;
    g_xreq.in_ds = ds;  g_xreq.in_es = es;
    g_xreq.out_ax = 0;  g_xreq.out_flags = 0;
    g_xreq.vec = VBE_EXEC_VEC;  /* r51: api_exec 门槛 vec>=0x40; 旧版靠
        getmapinfo 回填 MapLfbStat 到此字段, <0x40 被 ae_fail(87) 拒,
         borrow 根本没执行(./50 实锤 out=0/tgt=0/post[0]='VB' 四连证) */
    if (buf != 0 && bufBytes != 0) {
        g_xreq.bufLen = bufBytes;
        g_xreq.bufLin = LinearOf(buf);
    } else {
        g_xreq.bufLen = 0;
        g_xreq.bufLin = 0;
    }
    DiagHex("VxDexec: in", ax);
    s_biosBusy = 1;                     /* r111: 闩上 */
    VxDCallApi(VBE_API_EXEC_BIOS, g_xlin, sizeof(g_xreq), &ok);
    s_biosBusy = 0;                     /* r111: 开闩 */
    DiagHex("VxDexec: out", g_xreq.out_ax);
    DiagHex("VxDexec: tgt lo", (uint16_t)(g_xreq.stubLin & 0xFFFF));   /* 目标 off */
    DiagHex("VxDexec: tgt hi", (uint16_t)(g_xreq.stubLin >> 16));      /* 目标 seg */
    /* 【r138】把这一趟 BIOS 调用的其余输出寄存器收下来 —— 见 s_xoutBx 的注。
     *   位置就在 VxDCallApi 返回之后、任何提前返回之前: 即使这次调用随后
     *   被判定为拒绝(0x93), "硬件当时回了什么"本身仍是证据, 不能丢。
     *   纯赋值, 无端口访问、无副作用、不参与任何控制流。               */
    s_xoutBx = g_xreq.out_bx;
    s_xoutCx = g_xreq.out_cx;
    s_xoutDx = g_xreq.out_dx;
    s_xoutSi = g_xreq.out_si;
    s_xoutDi = g_xreq.out_di;
    s_xoutEs = g_xreq.out_es;
    /* 【r111】93 = VxD 侧的回调期 VM 自检拒绝(当前 VM 不是 Windows VM)。
     * 与"护栏拒绝"同等对待: 置标志(重建方据此回落重挂)、记进内存环 ——
     * 于是"回调里的那次 4F02 到底是不是死路"有了可读的数字。        */
    if (g_xreq.out_ax == 0x0093)
    {
        s_biosRefused = 1;
        g_cbVmRefuse++;
        CbRingPut( (uint16_t)((CB_EVT_CB_VMREFUSE << 8) | (g_cbVmRefuse & 0xFF)) );
        return 0xFFFF;
    }
    if (pfl)
        *pfl = g_xreq.out_flags;
    if (!ok)
        return 0xFFFF;
    return g_xreq.out_ax;
}

/*==========================================================================
 *
 *  VBE BIOS 调用层
 *
 *  【适用范围警告】
 *  INT 10h 只有在 V86/实模式上下文才会直达 VBE BIOS。在 Windows 保护
 *  模式下，INT 10h 会被 VDD 虚拟化：标准 VGA 功能被模拟，4Fxx 经
 *  主 VDD(可能再经 mini-VDD 的 VESA_SUPPORT 钩子)转发。本模块不再
 *  直接发 INT 10h/4Fxx(./23 实锤被假成功层吞), 全部经 VxD 嵌套
 *  V86 直通(r46 起数据中转亦全在 VxD 侧, 见 VBEEXECREQ.bufLen)。
 *  注意：在完整的 Win9x 显示驱动里，模式枚举/设置通常交给 VDD 完成，
 *  这里保留直接 VBE 调用是为了安装/检测工具与教学完整性。
 *
 *========================================================================*/

/*==========================================================================
 *  【r109】Int10PolicyHold / Int10PolicyRelease —— 让"我们自己"的 4F02 不被
 *  mini-VDD 的 policyInt10 钩子吃掉。
 *
 *  【为什么需要 —— ./108 t5 的实测】
 *  t5 是唯一一份 Int10Mode=1 的日志。它在 VhEdge 触发的那次重建里打出:
 *      DeferredRestore: pre-verify ok=0001
 *      DeferredRestore: SetDisplayMode FAILED
 *  即"我们发出去的模式设置失败了"。而 48 号钩子(minivdd.asm 48 号钩子的实现体)
 *  的判据是 `policyInt10 != 0 && StateValid && (状态位 & VGA) == 0` —— 这三条
 *  在"显示驱动自己发 4F02"时【全部成立】: 钩子无法区分发起者。它回 CY +
 *  AX=014F(VESA "功能调用不成功"), 我们的 VBE_SetMode 于是如实报失败。
 *  后果是致命的: 只要 Int10Mode=1, 驱动的重建能力就被自己关掉了 ——
 *  t5 里 Alt+Enter 之后回不来的直接原因就是"想救也救不了"。
 *
 *  【为什么只能在 ring 3 修】钩子读的是共享状态里的 policyInt10 字节(现场读,
 *  见 vbeshare.h 该字段注), 而这个字节由 display.drv 经 PushStateToMiniVDD
 *  推过去。所以"临时把策略清 0、调用完再推回"是完全在 ring 3 能做的事,
 *  不需要重编 minivdd.vxd(VBE_VXD_VERSION 因此保持不变, 部署可以混搭)。
 *
 *  【为什么推完整状态而不是只改一个字节】共享状态是主 VDD 算显存/bank 的
 *  唯一依据(vramSize / bytesPerScanLine / bankSizeKB), 现拼一份就有拼错的
 *  风险; 这里直接 4F01 取真值再推, 与 SetDisplayMode 里的那次推送同源。
 *  代价: Int10Mode=1 时每次模式设置多两次 4F01 + 两次 PushState; 默认(0)下
 *  Int10PolicyHold 立刻返回 0, 一次额外调用都没有。
 *
 *  【只读性】4F01 是只读查询, PushState 只写 VxD 的共享结构 —— 都不碰硬件。
 *========================================================================*/
static int Int10PolicyHold( uint16_t mode )
{
    MODEINFOBLOCK mi;

    if (!s_polInt10)
        return 0;                       /* 默认路径: 什么都不做 */
    if (!VBE_GetModeInfo( mode, &mi ))
    {
        DIAG( "Int10Mode: hold skipped (4F01 failed)" );
        return 0;
    }
    s_polInt10 = 0;                     /* 临时把策略清 0 */
    if (!PushStateToMiniVDD( &mi, mode ))
    {
        s_polInt10 = 1;                 /* 推失败: 还原, 宁可被吃掉也不要状态不一致 */
        DIAG( "Int10Mode: hold push FAILED" );
        return 0;
    }
    DIAG( "Int10Mode: policyInt10 temporarily cleared for our own 4F02" );
    FlushLog();
    return 1;
}

static void Int10PolicyRelease( uint16_t mode )
{
    MODEINFOBLOCK mi;

    s_polInt10 = 1;                     /* 还原真值(本函数只在 Int10Mode=1 时被调用) */
    if (!VBE_GetModeInfo( mode, &mi ))
    {
        DIAG( "Int10Mode: release skipped (4F01 failed)" );
        return;
    }
    if (!PushStateToMiniVDD( &mi, mode ))
        DIAG( "Int10Mode: release push FAILED" );
    else
        DIAG( "Int10Mode: policyInt10 restored" );
    FlushLog();
}

/* 设置 VBE 模式。
 * 入口: mode = VBE 模式号(bit14 置位表示使用 LFB，bit15 表示不清屏)
 * 返回: 非 0 = 成功(AX == 004Fh)
 * 【r46】直接 INT10 回退删除: ./23 实锤它被假成功层"假成功"(模式并没
 *  切), 留着只会向上层报谎。直通不可用 → 如实返回 0。                */
int __far VBE_SetMode(uint16_t mode)
{
    uint16_t fl;
    uint16_t rmode = (uint16_t)(mode & 0x3FFF);
    int      held;

    /* 【r109】mini-VDD 的 policyInt10 会把【我们自己】这次 4F02 一起吃掉。
     * 【证据】./108 的 t5(唯一一份 Int10Mode=1 的日志)在重建那一刻打出
     *   `DeferredRestore: pre-verify ok=0001` → `SetDisplayMode FAILED`:
     *   我们的 4F02 是被 48 号钩子(CY + AX=014F)挡下的 —— 因为钩子的判据
     *   ("策略开 && StateValid && VGA 位清")在我们自己发模式设置时【同样
     *   成立】, 它分不清"DOS 应用要设模式"和"显示驱动要设模式"。
     *   后果: 一旦开了 Int10Mode=1, 驱动就再也修不回桌面(t5 的全屏灾难
     *   就是这么来的: 想救的时候救不了)。
     * 【怎么修】只能从 ring 3 这一侧: 发模式设置之前先把 policyInt10 临时
     *   清 0 推给 mini-VDD(它是在分派入口【现场读】这个字节的, 见
     *   vbeshare.h 的 policyInt10 注), 发完再推回真值。两次 PushState 都是
     *   普通 VxD API 调用(无 BIOS、无 DOS), 而且只在 Int10Mode=1 时才发生
     *   —— 默认配置(0)下零开销、零行为变化。
     * 【推什么内容】必须推一份【完整且正确】的共享状态: 主 VDD 的显存/几何
     *   计算全靠它, 所以这里现取一次 4F01(只读), 而不是拿几何全局量拼一个
     *   —— 拼错的 WinGranularity 会让主 VDD 的 bank 换算错位(见 vbeshare.h
     *   里 bankSizeKB 的注)。                                            */
    held = Int10PolicyHold( rmode );

    if (VbeV86Ok()) {
        uint16_t ax = VbeExecBios(0x4F02, mode, 0, 0, 0, 0, 0, 0, 0, 0, &fl);
        DiagHex("VBE_SetMode: exec ax", ax);
        if (ax == 0x004F && !(fl & 1))
        {
            if (held)
                Int10PolicyRelease( rmode );
            return 1;
        }
    }
    if (held)
        Int10PolicyRelease( rmode );
    DIAG( "VBE_SetMode: exec unavailable/fail" );
    return 0;
}

/*==========================================================================
 * 【r138】硬件真相的三个探针 —— 4F03h / 4F06h / 4F07h。
 *
 * 【为什么本轮必须有它们 —— ./137 日志给出的、无法用 VGA 寄存器绕过的证据】
 *   r112..r137 每一条"硬件在不在我们的模式里"的判据, 读的都是 VGA 寄存器:
 *       CrtGeo: before verdict=0001   (CR00/CR01 算出来的行宽 = 1024)
 *       CrtSync: CR17 raw=00E3 / SE state=0001
 *       SNAP ATC10=0041 / AR10h 0851 干净
 *   这三条【全都说"对"】, 而用户看到的是一屏"按 8bpp 重解出来的条纹"。
 *   也就是说, 决定画面成立与否的还有一份状态, 它不在 VGA 寄存器里 ——
 *   线性打包像素模式的【像素格式】是芯片扩展状态, 通用 VBE 驱动结构上不
 *   知道它住在哪个端口(本文件 r112 的注释已经诚实列出过这一条)。
 *   唯一通用、标准、厂商无关的问法是 VBE 自己的读回功能。
 *
 * 【4F03h 是本轮唯一参与判据的读数 —— 理由: 它没有"设置"形态】
 *   AX=4F03h 只有一个动作: 回 BX = 当前 VBE 模式号。一个"当前模式号不是
 *   0117h"的读数, 是对"硬件不在我们的模式里"最直接、最硬的证据, 而且
 *   即使 BIOS 说谎(主 VDD 的假成功层), 它也只是回一个我们自己的模式号 ——
 *   那种情况下这一枪不响, 退化成 r137 的行为, 不会更糟。
 *
 * 【4F06h / 4F07h 只作证据, 绝不单独当判据 —— 安全边界】
 *   4F06h 有"设置"形态(set logical scan line length); 一旦输入寄存器选错,
 *   "读一行长度"就变成"改一行长度" —— 那是把硬件改坏的写法。所以:
 *     · 只用取回形态(BL=01h, VBE 3.0 §4.6 的 get form);
 *     · 结果只落日志与内存, 不参与任何"要不要重建"的决定;
 *     · 返回值先按 VBE 成功码(004Fh)筛过, 不支持的 BIOS 回 0160h ⇒ 记为未知。
 *   4F07h 同理, 只作证据。
 *
 * 【与 ROADMAP 的关系】本文件 2414 行附近早就记着: "要拿到它们就得改 VxD
 *   结构 + 递增 VBE_VXD_VERSION + 用户重编 minivdd.vxd"。r138 做的正是
 *   这一条 —— VBEEXECREQ 从 48 字节扩到 60 字节, api_exec 回填 BX/CX/DX/
 *   SI/DI/ES, 版本 0x0118 -> 0x0119。
 *
 * 【返回值约定】1 = 这次真的拿到了 BIOS 的答案(004Fh);
 *              0 = 拿不到(通道不可用 / BIOS 不支持 / 调用失败 / VM 拒绝)。
 *   拿不到时【不保留上一次的旧值】—— 旧值会被误当成"现在的硬件状态",
 *   那正是本项目反复吃过的"自证"陷阱。所以三个 getter 一律在失败时回 0xFFFF。
 *========================================================================*/
/* 【r138】探针的统一出口: 三个输出寄存器经文件级静态量取回。
 *   为什么不用输出指针: 本文件是 -mm(medium)编译的, "参数里的普通指针到底
 *   是 near 还是 far"在这种混合模型里要靠显式限定词才明确; 传一个局部量的
 *   地址进去会拿到一条 W112(指针截断)警告 —— 而本项目的纪律是"编译警告
 *   集合必须与基线逐条相同", 那条警告既不是真实缺陷、又会让每轮对比失真。
 *   用静态量做出口: 零指针、零歧义、零警告, 且与本文件既有的
 *   VxdHwRegSaveClocking / ProbeModeValue 一族写法一致。               */
/* r138-marker: VBE_ProbeCurrentMode / VBE_ProbeScanLine / VBE_ProbeDisplayStart
 *   (the VBE "get" functions are the only vendor-neutral way to ask the HARDWARE
 *   what mode it is scanning; until now every verdict read a mirror of our own
 *   software state).  Also r138: the api_exec request block returns the whole
 *   output register set, so BX/CX/DX are finally readable at ring 3. */
static uint16_t g_probeBx = 0xFFFF;
static uint16_t g_probeCx = 0xFFFF;
static uint16_t g_probeDx = 0xFFFF;

static int VbeProbe( uint16_t ax, uint16_t bx, uint16_t cx )
{    uint16_t fl = 0;
    uint16_t r;

    g_probeBx = 0xFFFF;
    g_probeCx = 0xFFFF;
    g_probeDx = 0xFFFF;
    if (!VbeV86Ok())
        return 0;
    r = VbeExecBios( ax, bx, cx, 0, 0, 0, 0, 0, 0, 0, &fl );
    /* 输出的三个寄存器由 VbeExecBios 存进 s_xout* —— 见那里的注。 */
    if (r != 0x004F || (fl & 1))
        return 0;
    g_probeBx = s_xoutBx;
    g_probeCx = s_xoutCx;
    g_probeDx = s_xoutDx;
    return 1;
}

/* 4F03h: 硬件当前的 VBE 模式号。返回 1 = 拿到, 0 = 拿不到(此时 getter 回 FFFF)。
 *   【为什么把"拿不到"与"拿到一个不同的模式"分开】两者对调用方的含义完全
 *   不同: 前者是"不知道"(不许据此动手), 后者是"知道它不对"(该动手)。  */
int __far __pascal VBE_ProbeCurrentMode( void )
{
    uint16_t m;

    g_probeMode = 0xFFFF;
    if (!VbeProbe( 0x4F03, 0, 0 ))
    {
        /* 【r138 无条件通道】拿不到也留痕 —— 否则"没有这一行"会被误读成
         *   "这一枪没响"。release 构建里 PHASEV 同样有效(不像 DiagHex)。*/
        PHASEV( "PH: hwmode=", 0xFFFF );
        return 0;
    }
    m = g_probeBx;
    g_probeMode = m;
    PHASEV( "PH: hwmode=", m );
    return 1;
}

uint16_t __far __pascal VBE_ProbeModeValue( void )
{
    return g_probeMode;
}

/* 4F06h 取回形态: 每扫描行【字节数】/【像素数】。
 *   【为什么这一对数字是"像素格式"的直接证据】1024x768x16bpp、pitch=0x800
 *   的桌面: 字节/行 必须是 0800h, 像素/行 必须是 0400h。硬件若被按 8bpp
 *   扫出, 这两项会变成 0400h/0400h —— 那正是"2 像素周期竖条纹"的形状。
 *
 *   【调用约定 —— 逐字对照规范, 因为这一个是"选错就改坏硬件"的那类】
 *     VBE 2.0 §4.9 / VBE 3.0 Function 06h:
 *       Input : AX = 4F06h
 *               BL = 00h Set Scan Line Length in Pixels
 *                    01h Get Scan Line Length
 *                    02h Set Scan Line Length in Bytes   (VBE 2.0+)
 *                    03h Get Maximum Scan Line Length    (VBE 2.0+)
 *               CX = If BL=00h/02h 期望宽度 ("Ignored for Get Functions")
 *       Output: AX = status, BX = Bytes Per Scan Line,
 *               CX = Actual Pixels Per Scan Line, DX = Maximum Number of Scan Lines
 *   子功能在 **BL** —— 不是 BH。BX = BH<<8 | BL ⇒ 取回形态是 BX = **0001h**。
 *   (BH=00h/01h 那对 set/get 选择子是 4F05h Display Window Control 的约定,
 *    与 4F06h 无关 —— 这里如果把 0x0100 传进去, 落在硬件上就是
 *    "BL=00h Set + CX=0" = 把每行长度设成 0 像素。)
 *   来源: VBE 2.0 Doc Rev 1.2 §4.9; Ralf Brown INTERRUP.LST INT 10/AX=4F06h
 *   (rb-0279/0280/0281); SeaBIOS vgasrc/vbe.c(只有 BL=00h/02h 调 setter,
 *   BL=01h 直接落到读取)与 VirtualBox/Bochs vbe.c(同形, case 0x01 跳过两次写)。
 *   后两条实现证据是"取回形态一个字节都不写"的旁证。               */
int __far __pascal VBE_ProbeScanLine( void )
{
    g_probePitchB = 0xFFFF;
    g_probePitchP = 0xFFFF;
    /* BX = 0001h ⇒ BL = 01h = Get(只读)。失败一律记未知, 绝不保留旧值。 */
    if (!VbeProbe( 0x4F06, 0x0001, 0 ))
    {
        PHASEV( "PH: hwpitch=", 0xFFFF );
        return 0;
    }
    g_probePitchB = g_probeBx;      /* BX = 每扫描行字节数 */
    g_probePitchP = g_probeCx;      /* CX = 每扫描行像素数 */
    PHASEV( "PH: hwpitch=", g_probePitchB );
    PHASEV( "PH: hwpx=", g_probePitchP );
    return 1;
}

uint16_t __far __pascal VBE_ProbePitchBytes( void )
{
    return g_probePitchB;
}

uint16_t __far __pascal VBE_ProbePitchPixels( void )
{
    return g_probePitchP;
}

/* 4F07h 取回形态: 显示起始。
 *   【调用约定】VBE 2.0 §4.10 / VBE 3.0 Function 07h:
 *       Input : AX = 4F07h, BH = 00h(Reserved and must be 00h),
 *               BL = 00h Set / 01h Get (VBE 2.0+ 另有 80h = 垂直回扫期设置)
 *       Output: AX = status, BH = 00h,
 *               CX = First Displayed Pixel In Scan Line,
 *               DX = First Displayed Scan Line   (Get 形态; 均从 0 起算)
 *   同样在 **BL** ⇒ 取回形态 BX = 0001h。传 0x0100 会变成
 *   "BL=00h Set + CX=0 + DX=0" —— 那是一次【写】显示起始, 不是我们要的读。
 *   只作证据: "起址被搬走"同样能造成整屏错位, 而它与像素格式是两件事。 */
int __far __pascal VBE_ProbeDisplayStart( void )
{
    if (!VbeProbe( 0x4F07, 0x0001, 0 ))
    {
        g_probeStartOk = 0;
        return 0;
    }
    g_probeStartLine   = g_probeDx;     /* DX = 首个显示的扫描行 */
    g_probeStartPixel  = g_probeCx;     /* CX = 首个显示的像素    */
    g_probeStartOk     = 1;
    return 1;
}

uint16_t __far __pascal VBE_ProbeStartLine( void )
{
    return g_probeStartOk ? g_probeStartLine : 0xFFFF;
}

uint16_t __far __pascal VBE_ProbeStartPixel( void )
{
    return g_probeStartOk ? g_probeStartPixel : 0xFFFF;
}

/* 【r82 未加 4F03/4F06/4F07 回读 —— 有意为之】这三个功能的结果都在【寄存器】
 *  (4F03→BX 模式号, 4F06→BX/CX/DX 逻辑行宽, 4F07→BX/CX 显示起点), 而我们的
 *  共享请求块 VBEEXECREQ 目前只由 VxD 回填 out_ax/out_flags(见 vbeshare.h),
 *  要拿到它们就得改 VxD 结构 + 递增 VBE_VXD_VERSION + 用户重编 minivdd.vxd。
 *  为了不让本轮(仍在定位点亮问题)多带一个变量, r82 不改 VxD; 这一步列进
 *  ROADMAP 的 T1.2/T2.2(真机 pitch/display-start 校正), 与 VxD 版本升级一起做。*/

/*==========================================================================
 *  【r106】VBE_GetVideoMode —— 不加任何 VxD 字段就能问到的"硬件此刻在什么
 *  模式"。这是 r106 的核心取证手段, 说明如下。
 *
 *  【为什么需要它】r104 的 ./104 证据把病灶钉到了"显示器输出"这一侧:
 *  同一时刻, 虚拟机内截图(走 GDI 读显存)是完好的桌面, 主机截图却是一屏
 *  被按 16 色 VGA 调色板重新解出来的条纹 —— 显存内容对, 光栅/调色板不对。
 *  可是驱动侧此前没有任何办法【读出硬件真实状态】:
 *    · VerifyDisplayMode 的两条判据全是自证(VxD 软件状态、帧缓冲读写回读),
 *      所以 B2 那次会打出 "mode already correct, no 4F02 issued" 而用户
 *      看到的是回不来(见 MaybeDeferredRestore 里的原注释);
 *    · 4F03/4F06/4F07 的结果在 BX/CX/DX, 我们的 VBEEXECREQ 只回 AX。
 *
 *  【为什么 0F00h 恰好够用】INT 10h AX=0F00h(取当前显示模式)的【结果就在
 *  AX】: AL = 当前模式, AH = 每行字符数。VESA 规定: 处于 VBE 模式时
 *  AL 返回 0FFh(模式号另在 BX, 我们不需要), 而处于标准 VGA 模式时
 *  AL 返回该模式号(3 = 文本, 13h = 320x200x8 ...)。
 *  ⇒ 一条判据把"硬件还在我们的 VBE 高分辨率通路里(AL=FF)"与"硬件已被
 *    切进标准 VGA 通路(AL=03/12/13...)"彻底分开, 不需要动 minivdd.asm,
 *    也就不需要用户重编 minivdd.vxd(VBE_VXD_VERSION 不变)。
 *
 *  【走哪条路】与 VBE_SetMode 完全同一条: VxD 侧嵌套 V86 直通。
 *  ./23 实锤过"保护模式直接发 INT 10h 会被假成功层吞", 而 VxD 嵌套 V86
 *  的那条路是有真实副作用的(VBE_SetMode 的 4F02 确实改得动硬件, r104 的
 *  VgaEdge 恢复就是靠它) —— 所以 0F00h 问到的也是【真 BIOS 的答案】。
 *
 *  【只读】0F00h 不改变任何硬件状态, 这是它相对 4F02 的全部价值: 可以在
 *  一次 DOS 会话【进行中】反复问, 而不会像 r99 那次嵌套 4F02 一样死机。
 *========================================================================*/
uint16_t __far VBE_GetVideoMode(void)
{
    uint16_t fl;

    if (VbeV86Ok()) {
        uint16_t ax = VbeExecBios(0x0F00, 0, 0, 0, 0, 0, 0, 0, 0, 0, &fl);
        DiagHex("VBE_GetVideoMode: exec ax", ax);
        if (ax != 0xFFFF)
            return ax;
    }
    DIAG( "VBE_GetVideoMode: exec unavailable" );
    return 0xFFFF;
}

/*==========================================================================
 *  【r107】VBE_GetVMHandle —— 问 VxD"此刻的当前 VM 句柄"(VBE_API_VM_HANDLE_PROBE)。
 *
 *  【为什么要它】./106 的日志给出一个此前只能猜的事实: 分派记录里的 EBX
 *  确实能分辨 VM —— 我们驱动自己的模式设置/枚举走的是 `ebx=C2920074`,
 *  而窗口化 DOS 盒里的 10 万次 CHECK_HIRES_MODE 轮询一律 `ebx=D5520074`。
 *  但"能分辨"还不等于"HiresAns=2(只答 Windows VM)这条开关能用": 那条路
 *  比较的是 VxD 在 PM API 入口记下的 WinVMHandle, 而 r100 当年实测"两种
 *  上下文取到的 ebx 不同"。本函数把 PM API 上下文里的句柄拿出来落进日志,
 *  与 TRC 记录里的 EBX 对一下就知道答案 —— 只读、零副作用。
 *  【为什么不去改 VxD】VBE_API_VM_HANDLE_PROBE(9) 早就在 minivdd.asm 里
 *  (VMMCall Get_Cur_VM_Handle), 这里只是第一次真的用它。
 *  出口: 句柄; 0 = 取不到(没连上/调用失败)。
 *========================================================================*/
uint32_t __far VBE_GetVMHandle(void)
{
    int      ok = 0;
    uint16_t st;

    if (g_xlin == 0)
        return 0UL;
    st = VxDCallApi(VBE_API_VM_HANDLE_PROBE, g_xlin, sizeof(g_xreq), &ok);
    if (!ok || st == 0xFFFF)
        return 0UL;
    /* VxD 把 Get_Cur_VM_Handle 的 EBX 拆成 out_ax(低字):out_flags(高字) */
    return ((uint32_t)g_xreq.out_flags << 16) | (uint32_t)g_xreq.out_ax;
}

/* 取模式信息块。
 * 【r46】经 VxD 数据中转: BIOS 写入 VxD 自有 V86 缓冲, VxD 拷回
 *  s_modeBuf(DGROUP 静态), 全程不建选择子、不碰低内存(./45 实锤
 *  客户端自建选择子写低内存在本机 Enable 期 GP 死机)。            */
int __far VBE_GetModeInfo(uint16_t mode, MODEINFOBLOCK far *mi)
{
    uint16_t ax, fl;

    DiagHex("VBE_GetModeInfo: mode", mode);

    if (!VbeV86Ok()) {
        DIAG( "VBE_GetModeInfo: exec unavailable" );
        return 0;
    }
    memset(s_modeBuf, 0, sizeof(MODEINFOBLOCK));
    ax = VbeExecBios(0x4F01, 0, mode, 0, 0, 0, 0, 0, s_modeBuf, 256, &fl);
    DiagHex("VBE_GetModeInfo: exec4F01 ax", ax);
    if (ax == 0x004F && !(fl & 1)) {
        _fmemcpy(mi, (void far *)s_modeBuf, sizeof(MODEINFOBLOCK));
        return 1;
    }
    return 0;
}

/* 取 VBE 总信息(版本与显存总量)。
 * 返回: 非 0 = 成功且签名 'VESA' 校验通过。机制同 GetModeInfo(r46)。
 * 【r46 注】旧实现只分 16 段(256B)却清零 512B, 越界写 256 字节 ——
 *  随本次重写一并消失。                                          */
static int VBE_GetInfo(VBEINFOBLOCK far *vi)
{
    uint16_t ax, fl;

    DIAG( "VBE_GetInfo: enter" );
    if (!VbeV86Ok()) {
        DIAG( "VBE_GetInfo: exec unavailable" );
        return 0;
    }

    /* 4F00h 要求输入缓冲区先写 'VBE2' 签名以请求 VBE 2.0 信息；
     * 即使 BIOS 只支持 1.x，也会忽略并返回 1.x 格式，兼容。     */
    memset(s_infoBuf, 0, sizeof(VBEINFOBLOCK));
    s_infoBuf[0] = 'V'; s_infoBuf[1] = 'B'; s_infoBuf[2] = 'E'; s_infoBuf[3] = '2';

    ax = VbeExecBios(0x4F00, 0, 0, 0, 0, 0, 0, 0, s_infoBuf, 512, &fl);
    DiagHex("VBE_GetInfo: exec4F00 ax", ax);
    if (ax != 0x004F || (fl & 1))
        return 0;
    if (((uint16_t *)s_infoBuf)[0] != 0x4556)   /* 'V''E' 签名(LE) */
        return 0;
    _fmemcpy(vi, (void far *)s_infoBuf, sizeof(VBEINFOBLOCK));
    return (vi->VESASignature[0] == 'V' && vi->VESASignature[1] == 'E' &&
            vi->VESASignature[2] == 'S' && vi->VESASignature[3] == 'A');
}

/* 枚举显卡支持的 VBE 模式号列表。
 * 入口: outModes = 接收模式号的数组；maxModes = 数组容量
 *       pVram    = 接收显存总字节数(可 NULL)
 *       pVer     = 接收 VBE 版本(可 NULL)
 * 返回: 模式个数；0 = 无 VBE 或调用失败
 *
 * 【r46 机制变更】低内存缓冲/桩全部 VxD 侧化: 客户端只在 DGROUP 静态
 * 缓冲 s_enumBuf 里预填('VBE2' 签名 + VideoModePtr 预置), 经 bufLen/
 * bufLin 让 VxD 拷入其 V86 缓冲、执行 4F00、再整批拷回 —— 全程不建
 * 选择子、不碰低内存(./45 实锤: 客户端自建选择子写低内存在本机
 * Enable 期 GP 死机; InitMapLfb 自 r42 起就在用 VxD 侧直读直写同一
 * 缓冲, 触摸测试把关)。直接 INT10 的 4F00 回退整段删除: ./23 实锤
 * 它被假成功层吞, 从未给出过真数据。VMP 直读(VBE 1.x)回退也随之
 * 删除 —— 与枚举缓冲同源于选择子写入, 而 VBE 1.x BIOS 早绝迹。
 * 【容量约束】V86 缓冲 1KB 布局: 0-511 信息块, 512-767 模式表(≤120 项),
 * 768-774 桩(r46 起 VxD 写)。模式表上限因此从 255 收紧到 120。      */
int __far VBE_EnumModes(uint16_t far *outModes, int maxModes,
                  uint32_t far *pVram, uint16_t far *pVer)
{
    uint16_t        ax, fl;
    uint16_t *      list;
    VBEINFOBLOCK    vi;
    int             n = 0;

    DIAG( "VBE_EnumModes: enter" );

    /* 【裁决探针⑤】(./23) 经典 INT 10h 对照组: 0F00(取当前模式)与
     *  1A00(VGA 识别码)是 VDD 必有完善虚拟化的标准功能。正常而
     *  4Fxx 被吞 → "只吃 VESA"实锤; 连它们也异常 → PM INT 10h
     *  通道整体残缺。不占低内存缓冲。                              */
    g_regs.x.ax = 0x0F00;
    CALL_INT(0x10);
    DiagHex("Enum: int10 0F ax", g_regs.x.ax);
    g_regs.x.ax = 0x1A00;
    g_regs.x.bx = 0x0000;
    CALL_INT(0x10);
    DiagHex("Enum: int10 1A ax", g_regs.x.ax);
    DiagHex("Enum: int10 1A bl", g_regs.x.bx & 0xFF);

    if (!VbeV86Ok()) {
        DIAG( "VBE_EnumModes: exec/v86buf unavailable" );
        return 0;
    }

    /* 在 DGROUP 静态缓冲预填(普通 near 操作, 零风险):
     * 清零 + 'VBE2' 签名 + VideoModePtr 预置为 V86 缓冲内 seg:0200,
     * 模式列表就地拷进同一缓冲后半, VxD 整批拷回, 无需第二个选择子。 */
    memset(s_enumBuf, 0, 1024);
    s_enumBuf[0] = 'V'; s_enumBuf[1] = 'B'; s_enumBuf[2] = 'E'; s_enumBuf[3] = '2';
    ((uint16_t *)s_enumBuf)[7] = 0x0219;            /* VideoModePtr off */
    ((uint16_t *)s_enumBuf)[8] = g_v86SegClient;    /*              seg */

    ax = VbeExecBios(0x4F00, 0, 0, 0, 0, 0, 0, 0, s_enumBuf, 1024, &fl);
    DiagHex("Enum: exec4F00 ax", ax);
    DiagHex("Enum: exec4F00 post[0]", ((uint16_t *)s_enumBuf)[0]);
    if (ax != 0x004F || (fl & 1) ||
        ((uint16_t *)s_enumBuf)[0] != 0x4556) {     /* 'V''E' (LE) */
        DIAG( "VBE_EnumModes: exec4F00 fail" );
        return 0;
    }
    DIAG( "VBE_EnumModes: exec ok" );

    /* 【r77 修 · 本驱动最凶的一次 DGROUP 自毁】这里原来是
     *        memcpy(&vi, s_enumBuf, sizeof(VBEINFOBLOCK));
     *   `vi` 是【栈上】的局部结构, 而 memcpy 是【近指针】版: 它用
     *   ES=DS=DGROUP 去寻址目标 —— 于是 256 字节 VBE 信息块被抄进
     *   DGROUP 自己的中段(偏移 = vi 的栈偏移), 把那里的全局变量砸烂。
     *   这与本文件头部"SS≠DS 时栈变量近指针即野指针"的警告是同一类
     *   错误。它的后果(事后全部对上号):
     *     · vi 本身从未被填过 → vi.VESAVersion / vi.TotalMemory 全是
     *       栈垃圾: 之前日志里"BIOS 谎报显存 128KB / 版本 2CF4"其实是
     *       我们自己在读未初始化栈, 不是 BIOS 的问题;
     *     · 被砸的 DGROUP 区域随【栈深度】漂移, 所以不同构建表现不同:
     *       r76d 砸中 DiagHex 的静态缓冲(日志出现 "sig0=" 后面没数字)与
     *       模式表逻辑 → "no VBE modes" 直接回退 VGA;
     *       r76b/r76c 砸中的是别处(转发地址表 _g_dibfn / _wDibDgroup /
     *       _lpDriverPDevice / _g_traceDone 之一) → DDI 转发跳进垃圾、
     *       探针被当成"已记录"而静默 → 表现为"没有任何 DDI 日志却在
     *       GDI/DIBENG 段里 0D"。
     *   【正确写法】栈变量要用 far 版: `(void __far *)&vi` 由编译器按
     *   SS 生成远指针(与 LinearOf(&di) 同机制, 已验证)。            */
    _fmemcpy((void __far *)&vi, (const void __far *)s_enumBuf,
             sizeof(VBEINFOBLOCK));
    DiagHex("VBE_EnumModes: sig0", ((uint16_t *)s_enumBuf)[0]);
    DiagHex("VBE_EnumModes: ver", vi.VESAVersion);
    DiagHex("VBE_EnumModes: vram64k", vi.TotalMemory);

    /* 【r52 的"BIOS 谎报"结论已在 r77 推翻】当时的日志显示 TotalMemory=2
     * (128KB)、版本 0x2CF4, 于是加了"<1MB 视为撒谎, 报 0 让 display.c
     * 跳过显存门禁"的地板。r77 查明那其实是上面 memcpy 砸 DGROUP 的
     * 副作用 —— vi 根本没被填, 读的是栈垃圾。现在 vi 是真实数据:
     * 本机 VBE_GetInfo(far 版拷贝, 一直是对的)报 TotalMemory=0x0100
     * (16MB), 与显卡相符。地板逻辑保留(仍然防某些 BIOS 真的报 0),
     * 但不再是"本机必踩"的路径了。                                */
    if (pVram)  *pVram = (vi.TotalMemory >= 16) ? ((uint32_t)vi.TotalMemory << 16) : 0;
    if (pVer)   *pVer  = vi.VESAVersion;

    /* 【r52】模式表经 VxD GET_MODELIST(12) 直读: ./51 实锤本机 BIOS
     * 无视 VideoModePtr 预置(inplace[0]=0), 列表只在 BIOS 自己的指针
     * 所指处(ROM/低内存) —— 那里 drv 不能读(./45), 由 VxD 线性遍历
     * (低内存/ROM 直读已验证) 整批拷回 mlist。inplace 旧路径留后备。 */
    {
        uint16_t mlist[128];
        int mcount = 0, i, mok;

        g_xreq.bufLen = sizeof(mlist);
        g_xreq.bufLin = LinearOf(mlist);
        VxDCallApi(VBE_API_GET_MODELIST, g_xlin, sizeof(g_xreq), &mok);
        if (mok)
            mcount = g_xreq.out_ax;
        DiagHex("Enum: modelist n", (uint16_t)mcount);

        if (mcount > 0) {
            for (i = 0; i < mcount && n < maxModes; i++) {
                uint16_t m = mlist[i];
                if (m == 0xFFFF || m == 0x0000)
                    break;
                outModes[n++] = m;
            }
        } else {
            /* 后备: 旧就地拷贝路径(理会预置指针的 VBE 2.0+ BIOS)。
             * 缓冲调用前清过, 首字非 0 即拷贝确证; 上限 120 项(768
             * 起是桩字节, 越界会读到 9C/9A 被当模式号)。             */
            uint16_t firstInPlace = ((uint16_t *)(s_enumBuf + 512))[0];

            DiagHex("VBE_EnumModes: inplace[0]", firstInPlace);
            if (vi.VESAVersion >= 0x0219 && firstInPlace != 0x0000) {
                list = (uint16_t *)(s_enumBuf + 512);
                while (n < maxModes && n < 120) {
                    uint16_t m = list[n];
                    if (m == 0xFFFF || m == 0x0000)
                        break;
                    outModes[n++] = m;
                }
            }
        }
    }

    DiagHex("VBE_EnumModes: n", (uint16_t)n);
    return n;
}

/*==========================================================================
 *
 *  VxD 通信层(INT 2F/AX=1684h + PM API 远调用)
 *
 *========================================================================*/

/* 连接 MINIVDD.VXD：取它的 PM API 入口。
 * 【原理】INT 2F/AX=1684h 是 Win9x 约定的"取 VxD API 入口"多路中断：
 *   BX = 设备 ID(我们的 VBE_VXD_DEVICE_ID)，ES:DI = 0 进入；
 *   返回 ES:DI = API 入口。调用方处于保护模式时，VMM 返回的是
 *   选择子:偏移 形式的 PM 入口(V86 调用方则得到 段:偏移 的 V86 入口)，
 *   因此同一段代码无需区分模式。
 * 【为何不能用 Win32 方式】16 位 display.drv 里没有 CreateFile/
 *   DeviceIoControl —— 那是原草案的致命错误。
 * 返回: TRUE = 已连接(VxD 已加载)；FALSE = VxD 不在系统中           */
BOOL __far VxD_Connect(void)
{
    if (g_vxdApi != 0)
        return TRUE;

    g_regs.x.ax = 0x1684;
    g_regs.x.bx = VBE_VXD_DEVICE_ID;
    g_regs.x.di = 0;
    g_sregs.es  = 0;
    DIAG( "VxD_Connect: before INT2F 1684" );
    CALL_INTX(0x2F);
    DIAG( "VxD_Connect: after INT2F 1684" );

    if (g_sregs.es == 0 && g_regs.x.di == 0)
    {
        DIAG( "VxD_Connect: no PM API (es=di=0)" );
        return FALSE;                   /* VxD 未加载(检查 SYSTEM.INI) */
    }
    DIAG( "VxD_Connect: PM API entry obtained" );

    /* 按 [偏移:低字, 选择子:高字] 的双字布局保存，供 far call 使用 */
    g_vxdApi = ((uint32_t)g_sregs.es << 16) | g_regs.x.di;
    return TRUE;
}

/* 调用 VxD API 的底层封装。
 * 入口: func   = 功能号(VBE_API_*)，放 AX
 *       linear = 缓冲区线性地址，拆成 DX(高字):CX(低字)
 *       size   = 缓冲区字节数，放 BX
 * 出口: 返回 VxD 在 AX 中给的状态码；*pOk = 0 表示 CF 置位(出错)
 * 【为何用静态变量中转】内联汇编经内存变量收发参数，不依赖特定的
 *   寄存器生存期，在 MSVC 1.52 与 Watcom 两种编译器下行为一致。     */
static uint16_t VxDCallApi(uint16_t func, uint32_t linear, uint16_t size,
                           int far *pOk)
{
    static uint16_t status;
    static uint16_t ok;
    static uint16_t sFunc;
    static uint16_t sSize;
    static uint32_t sLinear;
    uint16_t saveFunc;                  /* r111: 可重入保护, 见函数尾 */
    uint16_t saveSize;
    uint32_t saveLinear;

    saveFunc   = sFunc;                 /* r111 */
    saveSize   = sSize;
    saveLinear = sLinear;

    sFunc   = func;
    sLinear = linear;
    sSize   = size;

    if (g_vxdApi == 0)
    {
        *pOk = 0;
        sFunc   = saveFunc;             /* r111: 早退也要还原 */
        sLinear = saveLinear;
        sSize   = saveSize;
        return 0x87;            /* 未连接(防御, 不应到达) */
    }
    /* 【r100】这一对 before/back 行降为 3 级(默认 2 级不再有):
     * 它们是日志量的最大来源(每次光标 DDI 一行 VxD 调用 = 两行)。
     * 蓝屏定位改由 VbeExecBios 前后的 "VxDexec: in/out" 承担。 */
    DIAG3( "VxDCallApi: before far call to VxD" );
    FlushLog();         /* 死机路径不回写缓存: 出事时让日志停在本行 */

    _asm {
        push    bx
        push    cx
        push    dx
        mov     ax, [sFunc]
        mov     dx, word ptr [sLinear+2]    /* 线性地址高字 → DX */
        mov     cx, word ptr [sLinear]      /* 线性地址低字 → CX */
        mov     bx, [sSize]
        call    dword ptr [g_vxdApi]        /* 远调用 PM API 入口 */
        jc      api_err
        mov     [ok], 1
        mov     [status], ax
        jmp     api_done
api_err:
        mov     [ok], 0
        mov     [status], ax
api_done:
        pop     dx
        pop     cx
        pop     bx
    }

    DIAG3( "VxDCallApi: back from VxD" );
    *pOk = (int)ok;
    /* 【r111 可重入修复】把三个中转静态量恢复成"调用前"的值。
     *   为什么现在必须做: r111 起, `VxdSetInCallback()` 自己也要走一次
     *   VxDCallApi(VBE_API_SET_INCB), 而它是从【主 VDD 的回调】里被调用的
     *   —— 那个回调可以切进我们 ring-3 代码的任意两条指令之间, 包括
     *   "已经写好 sFunc/sLinear/sSize、还没发远调用"的那几拍。一旦如此,
     *   外层那一次调用就会拿着内层的参数发出【错误的功能号】。
     *   r99 已经证明"回调会切进任意一行"(两行日志粘在同一条物理行上),
     *   所以这里不是理论风险。恢复之后, 嵌套调用返回时外层的值原样还在。 */
    sFunc   = saveFunc;                 /* r111 */
    sLinear = saveLinear;               /* r111 */
    sSize   = saveSize;                 /* r111 */
    return status;
}

/*==========================================================================
 *
 *  对外功能
 *
 *========================================================================*/

/* 4F00h 结果缓存: PushState 每次都要用它, 但 BIOS 调用不便宜,
 * 成功一次就缓存(vramSize 的合理性兜底见 GuessVramSize)。            */
static uint32_t g_vramSize = 0;
static uint16_t g_vbeVer   = 0;
static uint8_t  g_infoOk   = 0;

/* GuessVramSize —— 显存总量兜底估算(r76)。
 * 【何时才用】只在 4F00h 拿不到、或 TotalMemory 明显不合理(<1MB)时兜底:
 * 那种情况下主 VDD 会拿 GET_TOTAL_VRAM_SIZE 的返回值决定"切到全屏 DOS
 * 会话时要保存多少显存", 报小了会把屏幕内容截断, 切回桌面时下半屏花屏。
 * 【r77 更正】r51/r52 那段"本机 VBE BIOS 撒谎, 把 TotalMemory 报成
 * 0x0002(128KB)、版本报成 0x2CF4"的注释是【错的】—— 那正是 r77 修掉的
 * memcpy 近指针 bug(把 256 字节 VBE 信息块写进 DGROUP)造成的假象:
 * 读到的 vi 从来就不是 BIOS 给的数据。修复后的实测是 sig0=4556、
 * ver=0300、TotalMemory=0x0100(16MB), 完全正常, 本兜底路径因此不再被
 * 触发(保留它只为对付真的坏 BIOS)。
 * 兜底值 = 模式缓存表里最大的帧缓冲字节数向上取整到 2 的幂(≥1MB):
 * 它必然覆盖当前可见屏幕, 且不会大得离谱。拿不到模式表就返回 0
 * (VxD 对 GET_TOTAL_VRAM_SIZE 返回 NC, 让主 VDD 自己去问 BIOS)。  */
static uint32_t GuessVramSize(void)
{
    uint32_t need = VbeMaxFramebuffer();
    uint32_t sz   = 0x100000UL;         /* 最小 1MB */

    if (need == 0)
        return 0;
    while (sz < need)
    {
        if (sz >= (VBE_LFB_MAP_CAP >> 1))
            return VBE_LFB_MAP_CAP;
        sz <<= 1;
    }
    return sz;
}

/* VbeTotalVram —— 显存总量(字节); 0 = 未知。r78 新增。
 *   与 PushStateToMiniVDD 共用同一份 4F00h 缓存(g_infoOk/g_vramSize),
 *   会话内只查一次 BIOS。LfbSelector_Ensure 用它决定"帧缓冲映射多大":
 *   只映射到模式表上限(4MB)而向主 VDD 报 16MB, 会让任何按上报量算出来
 *   的偏移撞上选择子界限(#GP)。                                    */
uint32_t __far VbeTotalVram(void)
{
    VBEINFOBLOCK vi;

    if (g_infoOk)
        return g_vramSize;

    if (VBE_GetInfo(&vi))
    {
        g_vbeVer = vi.VESAVersion;
        /* TotalMemory 单位 64KB; <16(即 <1MB)对任何带 LFB 的卡都不合理,
         * 视为 BIOS 撒谎 → 走兜底估算。                             */
        g_vramSize = (vi.TotalMemory >= 16)
                   ? ((uint32_t)vi.TotalMemory << 16)
                   : GuessVramSize();
        g_infoOk = 1;
        DiagHex("VbeTotalVram: vram63k", vi.TotalMemory);
        DiagHex("VbeTotalVram: vram lo", (uint16_t)g_vramSize);
        DiagHex("VbeTotalVram: vram hi", (uint16_t)(g_vramSize >> 16));
        DiagHex("VbeTotalVram: vbe ver", g_vbeVer);
    }
    return g_infoOk ? g_vramSize : 0;
}

/* ReportVram —— 上报给主 VDD 的显存总量(r78 一致性钳制: 取"真实总量"与
 * "已映射字节数"的小者)。
 * 【为何必须钳】GET_TOTAL_VRAM_SIZE 是主 VDD 保存/恢复全屏 DOS 画面时
 * "要保存多少显存"的依据, 而 ring 3 真正能访问的范围由帧缓冲选择子的
 * 界限(= DPMI 映射字节数)决定。两者一旦不一致, 任何按上报值算出来的
 * 访问都可能越界; 报小了顶多是 DOS 画面保存得少, 报大了就是越界访问。 */
static uint32_t ReportVram(void)
{
    uint32_t vr = VbeTotalVram();

    if (g_lfbBytes && (vr == 0 || vr > g_lfbBytes))
        vr = g_lfbBytes;
    /* r82: 自检判定"写不进去"的那部分显存不算数(见 LfbSetVramCeiling) */
    if (g_vramCeiling && (vr == 0 || vr > g_vramCeiling))
        vr = g_vramCeiling;
    return vr;
}

/* 把 VBE 模式信息组装成共享状态并推送进 mini-VDD。
 * 原注释翻译: Fill the shared state structure from VBE mode info
 *             and hand it to the mini-VDD.
 * 入口: mi      = 已取得的模式信息块
 *       modeNum = 当前 VBE 模式号
 * 返回: TRUE = 推送成功
 *
 * 【字段填充责任划分(v2)】
 *   本模块只填纯 VBE 信息：模式几何参数、bank 粒度(WinGranularity)、
 *   显存总量、VBE 版本，LFB 地址仅作记录。v1 的 bank 端口/影子
 *   字段已随卡专用方案一并删除——通用驱动没有任何需要"补全"的
 *   东西；WinGranularity 异常值(0 或 >64KB)由 VxD 侧钳制修正。
 * 【r76】lfbLinear / lfbSelector 由本模块填【自己 DPMI 映射的结果】,
 *   VxD 只做两件事: 抄进 VbeState(供回读) + 代我们把选择子登记给
 *   主 VDD(VDD_Register_Extra_Screen_Selector)。
 */
BOOL __far PushStateToMiniVDD(MODEINFOBLOCK far *mi, uint16_t modeNum)
{
    uint32_t     linear;
    int          ok;

    if (!VxD_Connect())
        return FALSE;

    _fmemset(&g_state, 0, sizeof(g_state));

    g_state.signature        = VBESTATE_SIG;
    g_state.structSize       = sizeof(VBESHAREDSTATE);
    g_state.currentMode      = modeNum;
    g_state.bytesPerScanLine = mi->BytesPerScanLine;
    g_state.xRes             = mi->XResolution;
    g_state.yRes             = mi->YResolution;
    g_state.bitsPerPixel     = mi->BitsPerPixel;
    g_state.bankSizeKB       = (uint8_t)mi->WinGranularity;
    g_state.physBasePtr      = mi->PhysBasePtr;
    g_state.hasLFB           = (mi->PhysBasePtr != 0) ? 1 : 0;
    g_state.lfbLinear        = g_lfbLinear;   /* DPMI 0800h 映射线性   */
    g_state.lfbSelector      = g_lfbSel;      /* DPMI LDT 屏选择子     */

    /* 【r100 / D3 修正】四个策略字节必须在这里填上 —— r99 从 memset 到推送
     * 结束没有任何一处给它们赋值, 于是 VxD 侧读到的 policyVddEdx /
     * policyVesa / policyHires 恒为 0: HiresAns 与 VesaPolicy 开了也不生效
     * (两个死开关), TraceStub_1(GET_VDD_BANK)的"拒绝窗口化 4 平面"分支也
     * 永远进不去。值由 ReadRunProfile 折算后经 VxdSetPolicies 存下。   */
    g_state.policyVddEdx     = s_polVddEdx;
    g_state.policyVesa       = s_polVesa;
    g_state.policyHires      = s_polHires;
    g_state.policyRestore    = s_polRestore;  /* VxD 不读, 只为两侧布局对齐 */
    /* 【r101】新增的两个策略字节。结构 40 → 44 字节, 两侧的
     * VBE_STATE_SIZE 与 rep movsd 长度必须同时改(vbeshare.inc 已同步)。 */
    g_state.policyBank       = s_polBank;
    g_state.policyInt10      = s_polInt10;
    g_state.policyVgaFix     = s_polVgaFix;   /* r114: 原本的保留字节约 */
    g_state.policyDispi      = s_polDispi;    /* r116: 原本的保留字节约 */
    g_state.policyInt10Fix   = s_polInt10Fix; /* r144: 结构 44 → 48 字节 */
    /* 【r147】policyHiresFix 与 policyMemCFix 都是 r144 那三个填充字节里的
     *   —— 结构大小仍是 48, 所以推送长度不变(见 vbeshare.h 处关于
     *   "sizeof(g_state) < VBE_STATE_SIZE 会被整包拒收"的长注)。 */
    g_state.policyHiresFix   = s_polHiresFix; /* r146: 45 */
    g_state.policyMemCFix    = s_polMemCFix;  /* r147: 46 */

    /* 显存总量与 VBE 版本: 取自 4F00h(VbeTotalVram 内缓存); 失败或明显
     * 不合理则用兜底估算, 仍取不到就置 0, VxD 会对 GET_TOTAL_VRAM_SIZE
     * 返回 NC, 让主 VDD 自己调 VESA BIOS 查询(回退永远正确)。
     * 【r78】上报值再按"已映射字节数"钳一次(见 ReportVram 头注): 上报量
     * 与 ring 3 真正能访问的范围必须一致, 否则主 VDD 会按上报量算出越界
     * 偏移。注意 g_vramSize 缓存里保留的仍是【真实总量】(映射尺寸决策
     * 要用它), 钳制只作用于本次上报值。                            */
    {
        uint32_t vr = ReportVram();

        g_state.vramSize = vr;
        DiagHex("PushState: vram rep lo", (uint16_t)vr);
        DiagHex("PushState: vram rep hi", (uint16_t)(vr >> 16));
        DiagHex("PushState: vram real hi", (uint16_t)(g_vramSize >> 16));
        DiagHex("PushState: lfb mapped lo", (uint16_t)g_lfbBytes);
        DiagHex("PushState: lfb mapped hi", (uint16_t)(g_lfbBytes >> 16));
    }
    g_state.vbeVersion = g_vbeVer;

    /* 把静态缓冲区的 far 地址换算成线性地址交给 VxD */
    linear = LinearOf(&g_state);
    if (linear == 0)
        return FALSE;

    VxDCallApi(VBE_API_PUSH_STATE, linear, sizeof(g_state), &ok);
    return ok ? TRUE : FALSE;
}

/* 从 mini-VDD 取回当前共享状态(调试/校验用)。
 * 返回: TRUE = 成功，out 已填                                          */
BOOL __far FetchStateFromMiniVDD(VBESHAREDSTATE far *out)
{
    uint32_t linear;
    int      ok;

    if (!VxD_Connect())
        return FALSE;

    /* 刷新一次 GET_MAPINFO(顺带触发 VxD 侧的惰性缓冲建立与状态转储),
     * 让随后的回读反映最新状态。                                    */
    VbeQueryMapinfo();

    linear = LinearOf(out);
    if (linear == 0)
        return FALSE;

    VxDCallApi(VBE_API_GET_STATE, linear, sizeof(*out), &ok);
    return ok ? TRUE : FALSE;
}

/* 查询 mini-VDD 版本(可用于安装自检)。
 * 返回: 版本号(如 0x0100)；0 = VxD 不在系统中                          */
uint16_t __far QueryMiniVDDVersion(void)
{
    int ok;

    if (!VxD_Connect())
        return 0;

    return VxDCallApi(VBE_API_GET_VERSION, 0, 0, &ok);
}

/* 查询 mini-VDD 的 VESA_SUPPORT 钩子被主 VDD 调用的累计次数。
 * 排障裁决: 0 = 钩子根本没被调用(放行未生效)；>0 = 钩子被调用。
 * 与共享状态无关，未推送状态时也可查询。                          */
uint16_t __far QueryVesaSupportCount(void)
{
    int ok;

    if (!VxD_Connect())
        return 0;

    return VxDCallApi(VBE_API_GET_DBG, 0, 0, &ok);
}

/*==========================================================================
 *  r99: mini-VDD 分派调用追踪的落盘 + 自愈请求通道
 *
 *  【为什么需要】vbe9xlog 里每一行都是 ring 3 写的, 主 VDD 对我们 49 个
 *  mini-VDD 分派服务的调用在日志里【一个都看不见】—— r98 之前"窗口化
 *  DOS 花屏到底是谁把硬件改了"只能靠推断。VxD 侧现在在每个分派入口记账
 *  (环形 64 条 + 每功能计数), 这里在【任意 ring-3 入口】整块取回落盘:
 *  只要系统还活着(鼠标一动 → MoveCursor, 任务栏时钟一跳 → ExtTextOut),
 *  证据就能出来 —— 这恰恰是花屏时唯一还通的通道。
 *
 *  【限流】只打"上一条之后"的新记录, 每次最多 TRC_MAXPRINT 条; 环被覆盖
 *  时打一行 lost= 说明丢了多少。否则一个长会话(每次鼠标移动都会调用我们
 *  的 DDI)会把日志冲爆。
 *
 *  【Heal 判据】DISPLAY_DRIVER_DISABLING(26) 的累计计数发生变化 = 主 VDD
 *  认为显示驱动已被卸下/硬件被切走。Heal=1 时据此置一次自愈请求, 由
 *  display.c 在下一次 ring-3 上下文重建模式(E4/E10 的自动化替代)。
 *
 *  r99-marker: dispatch trace is fetched and dumped from ring 3 only.
 *========================================================================*/
#ifdef VBE9X_DIAG
/* --- 分派功能号名字表 -----------------------------------------------------
 * 索引与主 VDD 的 mini-VDD 分派表严格一一对应, 名字逐字取自 DDK 的
 * MINIVDD.H(即 vbeshare.inc 的 RegList 注释同源)。带名字的理由很实在:
 * 一次屏幕切换就是十几条记录, 光看 "func=0023" 读不出任何东西; 有名字
 * 时日志自己是证据("窗口化 DOS 时主 VDD 先问了什么、后问了什么")。   */
static const char * const s_trcName[VBE_TRACE_FUNCS] = {
    "REGISTER_DISPLAY_DRIVER",      /*  0 */
    "GET_VDD_BANK",                 /*  1 */
    "SET_VDD_BANK",                 /*  2 */
    "RESET_BANK",                   /*  3 */
    "PRE_HIRES_TO_VGA",             /*  4 */
    "POST_HIRES_TO_VGA",            /*  5 */
    "PRE_VGA_TO_HIRES",             /*  6 */
    "POST_VGA_TO_HIRES",            /*  7 */
    "SAVE_REGISTERS",               /*  8 */
    "RESTORE_REGISTERS",            /*  9 */
    "MODIFY_REGISTER_STATE",        /* 10 */
    "ACCESS_VGA_MEMORY_MODE",       /* 11 */
    "ACCESS_LINEAR_MEMORY_MODE",    /* 12 */
    "ENABLE_TRAPS",                 /* 13 */
    "DISABLE_TRAPS",                /* 14 */
    "MAKE_HARDWARE_NOT_BUSY",       /* 15 */
    "VIRTUALIZE_CRTC_IN",           /* 16 */
    "VIRTUALIZE_CRTC_OUT",          /* 17 */
    "VIRTUALIZE_SEQUENCER_IN",      /* 18 */
    "VIRTUALIZE_SEQUENCER_OUT",     /* 19 */
    "VIRTUALIZE_GCR_IN",            /* 20 */
    "VIRTUALIZE_GCR_OUT",           /* 21 */
    "SET_LATCH_BANK",               /* 22 */
    "RESET_LATCH_BANK",             /* 23 */
    "SAVE_LATCHES",                 /* 24 */
    "RESTORE_LATCHES",              /* 25 */
    "DISPLAY_DRIVER_DISABLING",     /* 26 */
    "SELECT_PLANE",                 /* 27 */
    "PRE_CRTC_MODE_CHANGE",         /* 28 */
    "POST_CRTC_MODE_CHANGE",        /* 29 */
    "VIRTUALIZE_DAC_OUT",           /* 30 */
    "VIRTUALIZE_DAC_IN",            /* 31 */
    "GET_CURRENT_BANK_WRITE",       /* 32 */
    "GET_CURRENT_BANK_READ",        /* 33 */
    "SET_BANK",                     /* 34 */
    "CHECK_HIRES_MODE",             /* 35 */
    "GET_TOTAL_VRAM_SIZE",          /* 36 */
    "GET_BANK_SIZE",                /* 37 */
    "SET_HIRES_MODE",               /* 38 */
    "PRE_HIRES_SAVE_RESTORE",       /* 39 */
    "POST_HIRES_SAVE_RESTORE",      /* 40 */
    "VESA_SUPPORT",                 /* 41 */
    "GET_CHIP_ID",                  /* 42 */
    "CHECK_SCREEN_SWITCH_OK",       /* 43 */
    "VIRTUALIZE_BLTER_IO",          /* 44 */
    "SAVE_MESSAGE_MODE_STATE",      /* 45 */
    "SAVE_FORCED_PLANAR_STATE",     /* 46 */
    "VESA_CALL_POST_PROCESSING",    /* 47 */
    "PRE_INT_10_MODE_SET"           /* 48 */
};

/* 一条记录一行:
 *   "TRC n=SSSSSSSS f=NN NAME ecx=XXXXXXXX edx=XXXXXXXX ebx=HHHHHHHH eax=HHHHHHHH"
 * 【为何一行而非六行】旧写法一条记录写六行, 一次 dump 16 条就是 96 行,
 * 日志被追踪数据淹没; 一行既省 6 倍篇幅又更好读(数值顺序固定)。
 * 【r100 新增 ebx/eax】EBX = 发起调用的 VM 句柄(分派约定) —— 这是分辨
 * "Windows 自己 vs DOS VM"的唯一依据; EAX = 入口 Client_EAX(例: 48 号
 * PRE_INT_10_MODE_SET 时就是被请求的模式号)。
 * 【r106 修 n 只打 16 位】r105 及以前这里写的是 DiagPutX4((uint16_t)seq),
 * 于是 ./104 的日志里出现 "TRC n=AB53" 紧接 "TRC n=1830" 这种看着像
 * "记录号倒退" 的序列 —— 实际是 0x11830 被截断(./104 分析时已按此解释,
 * 但一条会骗人的日志本身就是缺陷)。与 r104 修 TRC calls/lost 同理,
 * 这里改成 32 位; buf 由 128 字节保证(最长行仍在 110 字节内)。          */
static void DiagTraceRec(uint32_t seq, uint16_t f, uint32_t ecx, uint32_t edx,
                         uint32_t ebx, uint32_t eax)
{
    static char buf[128];
    char       *p = buf;

    p = DiagPutS(p, "TRC n=");  p = DiagPutX8(p, seq);
    p = DiagPutS(p, " f=");     p = DiagPutX2(p, f);
    p = DiagPutS(p, " ");
    p = DiagPutS(p, s_trcName[(f < VBE_TRACE_FUNCS) ? f : 0]);
    p = DiagPutS(p, " ecx=");   p = DiagPutX8(p, ecx);
    p = DiagPutS(p, " edx=");   p = DiagPutX8(p, edx);
    p = DiagPutS(p, " ebx=");   p = DiagPutX8(p, ebx);
    p = DiagPutS(p, " eax=");   p = DiagPutX8(p, eax);
    *p = 0;
    DiagWriteLog(buf);
}
/* 计数表一行: "TRC cnt f=NN NAME n=XXXXXXXX"(开机以来该服务被问过几次)。
 * 这就是"主 VDD 到底问过我们什么"的基线 —— 有它才谈得上判 M1/M3。 */
static void DiagTraceCnt(uint16_t f, uint32_t c)
{
    static char buf[80];
    char       *p = buf;

    p = DiagPutS(p, "TRC cnt f=");  p = DiagPutX2(p, f);
    p = DiagPutS(p, " ");
    p = DiagPutS(p, s_trcName[(f < VBE_TRACE_FUNCS) ? f : 0]);
    p = DiagPutS(p, " n=");         p = DiagPutX8(p, c);
    *p = 0;
    DiagWriteLog(buf);
}

/* 【r107】增量计数表一行: "TRC + f=NN NAME n=XXXXXXXX"
 *   n 是"自上一次状态边沿以来"该服务被问过几次。开机以来那张完整表只在
 *   第一帧打过一次(那时还没有任何 DOS 会话), 于是 ./106 的日志里
 *   "DOS 会话期间主 VDD 到底调了哪些服务"【完全看不见】—— 唯一能看见的
 *   是环形缓冲里被 35 号洪泛冲剩下的残渣(实测 105168 次分派里 99% 是 35 号,
 *   打印出来的 16 条全是它)。增量表把那段窗口一次性变成读数。        */
static void DiagTraceDelta(uint16_t f, uint32_t d)
{
    static char buf[80];
    char       *p = buf;

    p = DiagPutS(p, "TRC + f=");    p = DiagPutX2(p, f);
    p = DiagPutS(p, " ");
    p = DiagPutS(p, s_trcName[(f < VBE_TRACE_FUNCS) ? f : 0]);
    p = DiagPutS(p, " n=");         p = DiagPutX8(p, d);
    *p = 0;
    DiagWriteLog(buf);
}
#else
#define DiagTraceRec( a, b, c, d, e, f )  ((void)0)
#define DiagTraceCnt( a, b )        ((void)0)
#define DiagTraceDelta( a, b )      ((void)0)
#endif

static uint8_t  s_trcBuf[VBE_TRACE_SIZE];
static uint32_t s_trcLastSeq = 0;    /* 已打印到的记录序号          */
static uint32_t s_trcDis26   = 0;    /* 上次见到的功能 26 计数      */
static uint32_t s_trcTotalSeen = 0;  /* 【r100】上次见到的 WriteIdx(TRC calls) */
static uint16_t s_trcHdrDone = 0;    /* 首帧是否已打过计数表        */
/* 【r120】ring 0 物理读 DISPI 的去重状态: 只在"读数变了"时落盘,
 * 与 TRC st bits / dispi 记账同一套"变化才记"纪律。 */
static uint8_t  s_dpSeen  = 0;       /* 上次的 DispiPhysN(0 = 还没读过) */
static uint16_t s_dpIdLast = 0;
static uint16_t s_dpXrLast = 0;
static uint16_t s_dpYrLast = 0;
static uint16_t s_dpBpLast = 0;
static uint16_t s_dpEnLast = 0;
static uint16_t s_dpVwLast = 0;
static uint8_t  s_dpOkLast = 0;
uint16_t LogBufSel = 0;   /* [r447] 1 = 日志缓冲已启用(不再是选择子) */
static uint16_t s_healReq    = 0;    /* 自愈请求(等 display.c 取走) */
static uint32_t s_trcDis12 = 0;   /* [r412] 钩子12 计数快照 */
static uint16_t s_healWanted = 0;    /* Heal=1                      */
/* 【r107】上一次状态边沿时 49 个计数桶的值(增量表用)。
 * 49*4 = 196 字节 —— 相对 s_trcBuf 的 1960 字节是小头(r152: 头部 424 +
 * 64×24 记录)。
 * 只在诊断构建里存在(发布构建的增量表是空宏)。                     */
#ifdef VBE9X_DIAG
static uint32_t s_cntPrev[VBE_TRACE_FUNCS];
static uint16_t s_cntPrevOk = 0;
#endif

#define TRC_MAXPRINT 16
/* 【r107】CHECK_HIRES_MODE(35 = 0x23): 窗口化 DOS 会话里主 VDD 会以
 * ~1000 次/秒 的频率问它(./106 实测一个会话 105168 次分派里 99% 是它)。
 * 打印时把它挡掉, 否则环形缓冲里其余服务一条都看不见。             */
#define TRC_FN_HIRES 0x0023

/* 【r110】第二类洪泛: 主 VDD 的"把高分辨率屏搬进搬出"银行循环。
 * ./109 的 t2 实测: 一次 Alt+Enter 往返(会话收尾)期间, 主 VDD 会连续
 * 调用 mini-VDD 的 03h RESET_BANK / 09h RESTORE_REGISTERS /
 * 0Fh MAKE_HARDWARE_NOT_BUSY 各数十到数百次(同一组寄存器反复出现),
 * 环被它们冲满(日志里 `TRC lost=0000032A` = 一次丢 810 条), 而按序号
 * 打印的预算又被它们吃光 —— 结果**恰好是"最想看的那一瞬间"看不见**:
 * t2 日志里 296 KB、8364 行, 其中绝大多数是这三号记录, 并且同一时刻
 * 日志闸门开始 `LOG skip: ... Reent=01EC` 成片丢行, 于是
 * `DeferredRestore:` 这类关键行是否真的执行过, 事后无法判定。
 * 处理: 与 35 号同策 —— 记录不进正文, 只计数, 每次 dump 打一行汇总。
 * 计数桶(TRC + f=03/f=09/f=0F)本来就完整保留这三个号的信息。        */
#define TRC_FN_BANK_RST   0x0003
#define TRC_FN_BANK_RES   0x0009
#define TRC_FN_BANK_BUSY  0x000F
/* 【r110】单次 dump 的"新记录跨度"上限: 超过它就整段只报一条
 * `TRC flood`, 明细一条不打。窗口化 DOS 会话里一次 dump 之间可能有上万
 * 条记录, 逐条判断 + 逐条写盘会把光标本该走的路拖慢(而卡顿本身会掩盖
 * 花屏复现)。8 号(寄存器保存)那类不洪泛的服务, 只要跨度不大就照旧看得见。*/
#define TRC_FLOOD_SPAN    192

/* 【r107】把环内 [lo, total] 的记录按序号打出来, 但跳过 35 号洪泛。
 * 返回实际打印条数; *pF23 回传被跳过的 35 号条数。
 * 【r110】再加: *pStorm[0..2] 回传被跳过的 03/09/0F 三个号各多少条。
 * 【只在诊断构建里存在】它要扫完整个新记录区间(窗口化 DOS 会话里一次
 * dump 前可能有上万条 35 号), 发布构建不该为此付出代价 —— 那里的
 * DiagTraceRec 本来就是空宏, 走 r106 的原循环即可。                  */
#ifdef VBE9X_DIAG
static uint16_t TraceDumpFiltered( uint32_t total, uint32_t lo,
                                   uint16_t budget, uint32_t __far *pF23,
                                   uint32_t __far *pStorm )
{
    uint32_t seq;
    uint32_t f23 = 0;
    uint32_t storm[3];
    uint16_t printed = 0;

    storm[0] = storm[1] = storm[2] = 0;
    for (seq = lo; seq <= total; seq++)
    {
        uint8_t *r = s_trcBuf + VBE_TR_Recs +
                     (uint16_t)((seq - 1) % (uint32_t)VBE_TRACE_RECS) * VBE_TRACE_RECB;
        uint16_t fn = *(uint16_t *)(void *)(r + VBE_TREC_Func);

        if (fn == TRC_FN_HIRES)
        {
            f23++;
            continue;
        }
        if (fn == TRC_FN_BANK_RST || fn == TRC_FN_BANK_RES || fn == TRC_FN_BANK_BUSY)
        {
            storm[(fn == TRC_FN_BANK_RST) ? 0 : ((fn == TRC_FN_BANK_RES) ? 1 : 2)]++;
            continue;
        }
        if (budget == 0)
            continue;                   /* r110: budget=0 ⇒ 只计数不打印 */
        if (printed >= budget)
            break;
        DiagTraceRec( seq, fn,
                      *(uint32_t *)(void *)(r + VBE_TREC_Ecx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Edx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Ebx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Eax) );
        printed++;
    }
    if (pF23)
        *pF23 = f23;
    if (pStorm)
    {
        pStorm[0] = storm[0];
        pStorm[1] = storm[1];
        pStorm[2] = storm[2];
    }
    return printed;
}

/* 【r109】TraceDumpFunc48 —— 把环里 48 号(PRE_INT_10_MODE_SET)的记录单独捞出来。
 * 【为什么必须有】48 号是"某个 VM 正要执行 INT 10h 设模式"的第一现场: 它的
 *   EBX = 发起 VM、ECX/EDX = 该调用入口的上下文、EAX = 被请求的模式号。
 *   但 35 号(CHECK_HIRES_MODE)的轮询会把 64 条的环冲满 —— 日志里那些
 *   `TRC f23 skipped (hires poll, suppressed)=00000xxx` 就是证据 —— 于是
 *   常规"按序号打印"打出来的永远是 35 号(./100、./107 两次都栽在这里,
 *   只剩 blob 头那两个字节: 次数与最近一次的模式号)。
 * 【为什么按"计数变化"调用它】只在 VBE_TR_I10Cnt 变化的那一拍捞, 一开机
 *   最多十几次, 代价可忽略; 而"用户按了 Alt+Enter / 开了 DOS 盒"这些
 *   关键时刻恰好都是 48 号发生的那一拍。
 * 【上限 16 条】一次边沿最多捞 16 条, 免得日志爆掉。                 */
static void TraceDumpFunc48( uint32_t total, uint32_t lo )
{
    uint32_t seq;
    uint16_t printed = 0;

    for (seq = lo; seq <= total && printed < 16; seq++)
    {
        uint8_t *r = s_trcBuf + VBE_TR_Recs +
                     (uint16_t)((seq - 1) % (uint32_t)VBE_TRACE_RECS) * VBE_TRACE_RECB;
        uint16_t fn = *(uint16_t *)(void *)(r + VBE_TREC_Func);

        if (fn != 48)
            continue;
        DIAG( "I10: f=30 PRE_INT_10_MODE_SET record (EBX=VM, EAX=requested mode)" );
        DiagTraceRec( seq, fn,
                      *(uint32_t *)(void *)(r + VBE_TREC_Ecx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Edx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Ebx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Eax) );
        printed++;
    }
    if (printed)
        DiagHex( "I10: records dumped", printed );
}

/* 【r107】增量计数表: 打印"自上次调用以来"每个计数桶的增量。 */
static void TraceCntDelta( uint8_t *b )
{
    uint16_t f;

    for (f = 0; f < VBE_TRACE_FUNCS; f++)
    {
        uint32_t c = *(uint32_t *)(void *)(b + VBE_TR_Counts + f * 4);
        uint32_t d = s_cntPrevOk ? (c - s_cntPrev[f]) : c;

        s_cntPrev[f] = c;
        if (d)
            DiagTraceDelta( f, d );
    }
    s_cntPrevOk = 1;
}
#endif  /* VBE9X_DIAG */

void __far __pascal VxdSetHeal( uint16_t on )
{
    s_healWanted = on ? 1 : 0;
}

/* 取走一次自愈请求(取走即清, 保证只重建一次) */
uint16_t __far __pascal VxdTakeHealPending( void )
{
    uint16_t v = s_healReq;

    s_healReq = 0;
    return v;
}

/*==========================================================================
 * 【r100】策略字节 / 回调期内存环 / 回调哨兵 / 切屏安全信号
 *
 * 背景(真机 x230 实测): ResetHiResMode 回调是主 VDD 在【它自己的切屏事务
 * 内部】调进来的, 而且实测会切进我们 ring-3 代码的执行中途(日志里出现过
 * "VxDCallApi: before far call to VxD" 与 "RestoreDesktopMode: enter" 粘在
 * 同一物理行 —— 外层正卡在 DiagWriteLog 的两次 INT 21h 之间)。因此回调里:
 *   ① 绝不发 BIOS/VESA(嵌套 V86)调用;  ② 绝不调 DOS(含打日志);
 *   ③ 只做纯内存操作。真正的重建由 display.c 的 MaybeDeferredRestore 在
 *   "切屏事务已收尾"之后做。下面的内存环就是回调唯一允许的记录方式。
 * 【注意】这些状态量本身声明在本文件前部(VbeExecBios 要用)。
 *========================================================================*/
void __far __pascal VxdSetPolicies( uint16_t vddEdx, uint16_t vesa,
                                    uint16_t hires, uint16_t restore,
                                    uint16_t bank, uint16_t int10,
                                    uint16_t vgafix, uint16_t dispi,
                                    uint16_t int10fix, uint16_t hiresfix,
                                    uint16_t memcfix )
{
    s_polVddEdx  = vddEdx  ? 1 : 0;
    s_polVesa    = vesa    ? 1 : 0;
    s_polHires   = (uint8_t)hires;   /* r101: 0/1/2/3 四态, 不再压成 0/1 */
    s_polRestore = (uint8_t)restore;
    s_polBank    = bank    ? 1 : 0;  /* r101 */
    s_polInt10   = int10   ? 1 : 0;  /* r101 */
    s_polVgaFix  = vgafix  ? 1 : 0;  /* r114: mini-VDD 的文本取指补写 */
    s_polDispi   = dispi   ? 1 : 0;  /* r116: mini-VDD 的 1CEh/1CFh 虚拟化 */
    s_polInt10Fix= int10fix? 1 : 0;  /* r144: 48 号把 72h/73h 改写成 03h */
    s_polHiresFix= hiresfix? 1 : 0;  /* r146: 进盒前存 / 回 Windows 后写回 */
    /* 【r153 —— ./152 的伤在这里】r152 把 MemCFix 从 0/1 扩成 0/1/2, 并在
     *   display.c 里把 `(val == 0) ? 0 : 1` 改成直通; 但【值要经过这里】
     *   才到达 VxD, 而这一行仍然是 `memcfix ? 1 : 0` —— 于是 2 被夹回 1,
     *   VxD 侧看到的是"六字节文本补写"(./148 已实测有害的那一档), r152
     *   等于 r148 + 一个新探针。日志里的铁证是: 驱动侧 `CFG27 ... 0002`
     *   印着 2, 而 VxD 侧 fixN 涨到 0x14(真的写了 20 次)。
     *   r153 直通 0/1/2, 并且把这个值【两侧各印一次】(见 display.c 的
     *   `PH: r153 MemCFix drv=` 与 drv.c 的 `PH: probe policy=`)。 */
    /* r153: clamp the UPPER side only -- `(memcfix < 0)` is always false for an
     *   unsigned parameter and makes Watcom emit W124, which would break the
     *   "warning line counts identical to r146..r152" baseline. */
    s_polMemCFix = (uint8_t)((memcfix > 2) ? 2 : memcfix);
}

/* 【r146】VxdHiresInfo —— 见 drv.h 的长注。idx 越界返回 0xFF(不冒充 0)。 */
uint8_t __far __pascal VxdHiresInfo( uint16_t idx )
{
    if (idx > 3)
        return 0xFF;
    return s_hires[idx];
}

/* 【r147】VxdMemCInfo —— 见 drv.h 的长注。idx 越界返回 0xFF(不冒充 0)。
 *   读的是本文件已经缓存好的副本(每次 blob 轮询刷新), 所以它和
 *   "TRC memcfix ..." 那几行永远同源, 不会出现两处读数不一致。 */
uint8_t __far __pascal VxdMemCInfo( uint16_t idx )
{
    if (idx >= VBE_TR_MC_LEN)
        return 0xFF;
    return s_memc[idx];
}

void __far __pascal VxdSetInCallback( uint16_t on )
{
    s_inVddCallback = on ? 1 : 0;
    /* 【r102】同一个哨兵兼任"日志闸门" —— 见 display.c 的 g_logBlocked。
     * 两个哨兵必须同步: 一个是 VbeExecBios 的护栏依据, 一个是汇编侧
     * DIAGWRITELOG 的闸门。开、关只此一处, 免得将来只改一边。        */
    g_logBlocked = s_inVddCallback;
    /* 【r111】同一个状态也要告诉 mini-VDD: 它要在 api_exec 里做"回调期
     * 当前 VM 必须是 Windows VM"的自检(见 vbeshare.inc 的 VBE_API_SET_INCB)。
     * 这是一次纯内存的 VxD API 调用, 不发 BIOS、不碰 DOS —— 回调期安全。
     * 【为什么用 size 那个形参传值】VxDCallApi 把第 3 个形参放进 BX,
     * 而本功能的输入约定正是 EBX = 0/1。                             */
    {
        int cbOk = 0;

        VxDCallApi( VBE_API_SET_INCB, 0, (uint16_t)(on ? 1 : 0), &cbOk );
    }
}

/* 回调期事件记录: 纯内存, 绝不碰 DOS/文件/BIOS/HW */
void __far __pascal CbRingPut( uint16_t code )
{
    if (s_cbRingN < (uint8_t)(sizeof(s_cbRing)/sizeof(s_cbRing[0])))
        s_cbRing[s_cbRingN++] = code;
    else
        s_cbRingDrop++;                 /* r136: 丢一条就记一条(模 256) */
}

/* 【r136】只读查询: 从上一次落盘以来丢掉的条数。 */
uint16_t __far __pascal CbRingDropped( void )
{
    return (uint16_t)s_cbRingDrop;
}

/* 【r136】只读查询: 环里有没有待落盘的记录(见 drv.h 的注)。 */
uint16_t __far __pascal CbRingPending( void )
{
    /* 【r137 修正】r136 这里返回的是 0/1, 而调用点写 `> 1` —— 那个条件永远
     *   为假, "每次进入安全上下文就落盘"这条改动因此一次都没执行过
     *   (./136 整份日志里 CB 环只在重建路径上落盘过一次, 就是证据)。
     *   返回条数才是调用者要的量。 */
    return (uint16_t)s_cbRingN;
}

/* 在安全上下文里把回调期事件落盘(此处的 DOS 调用是安全的) */
void __far __pascal CbRingDump( void )
{
    uint8_t i;

    /* 【r136】先报"上次落盘之后丢掉了几条" —— 它必须在事件之前落盘,
     *   否则一旦本次落盘又被闸门部分丢掉, 这条声明也跟着丢。
     *   0 = 完整; 非 0 = 本段日志不完整(判读时按证据缺失处理)。      */
    if (s_cbRingDrop)
    {
        DiagHex( "CB dropped", (uint16_t)s_cbRingDrop );
        s_cbRingDrop = 0;
    }
    for (i = 0; i < s_cbRingN; i++)
        DiagHex( "CB evt", s_cbRing[i] );
    if (s_cbRingN)
        FlushLog();
    s_cbRingN = 0;
}

/* 取走一次"切屏事务已收尾"信号(取走即清) */
uint16_t __far __pascal VxdTakeSafeToSetMode( void )
{
    uint16_t v = s_safeToSetMode;

    s_safeToSetMode = 0;
    return v;
}

/* 【r104】取走一次"DOS 虚拟化会话已收尾"信号(取走即清)。
 * 产生者: VxdTraceDump 里 TRAPS 位的下降沿; 消费者: display.c 的
 * MaybeDeferredRestore —— 它据此自己发起一次桌面重建(不依赖主 VDD 是否
 * 回调我们的 ResetHiResMode)。见 drv.c 里 s_vgaSessEnd 的注释。 */
uint16_t __far __pascal VxdTakeVgaSessionEnd( void )
{
    uint16_t v = s_vgaSessEnd;

    s_vgaSessEnd = 0;
    return v;
}

/* 【r108】取走一次"主 VDD 刚走完一趟 hires→VGA→hires"的请求(取走即清)。
 * 产生者: VxdTraceDump 里 VBE_TR_Evt[POST_VGA_TO_HIRES] 的变化(= 7 号服务
 * 的累计次数 +1, 见 drv.c 里 s_v2hEdge 的长注); 消费者: display.c 的
 * MaybeDeferredRestore —— 它据此在【会话还在进行时】就把桌面重建回来,
 * 而不是像 r104..r107 那样干等 TRAPS 下降沿(实测晚 1.5 秒)。 */
uint16_t __far __pascal VxdTakeVgaToHiresEdge( void )
{
    uint16_t v = s_v2hEdge;

    s_v2hEdge = 0;
    return v;
}

/* 【r135】读一次最近一次"会话边沿"的【方向】(不清、无副作用)。
 *   返回 1 = 回到 Windows, 2 = DOS 盒拿走屏幕, 0 = 判不出。
 *   完整设计说明在 drv.h 的同名声明处 —— 一句话: 它是【有序】证据
 *   (主 VDD 自己最近一次为 VM 设了什么模式), 不是会被采样错过的瞬时位。 */
uint16_t __far __pascal VxdEdgeDirNow( void )
{
    return (uint16_t)s_edgeDirNow;
}

/* 【r145】VxdEdgeBudgetReset —— 由 display.c 在一次【成功重建】之后调用。
 *   为什么需要它: 三个边沿计数器从 r104 起就是"每次开机最多 8 次"
 *   的硬上限, 而上限一旦用满就永久不再置请求
 *   (用户 ./144 碰到的就是这个)。本轮把它们降为"突发预算"
 *   并在硬件被确认拉回我们模式的那一刻清零 —— 于是
 *   "正常用一止会话”永远不会碰到预算, 而真的风暴
 *   (位被误读成高频翻转)仍受上限保护。 */
void __far __pascal VxdEdgeBudgetReset( void )
{
    s_vgaEdgeN    = 0;
    s_entryN      = 0;
    s_v2hN        = 0;
    s_edgeSkipEnd = 0;
    s_edgeSkipEnt = 0;
    /* 【r146】这三个计数器现在【只作记账】, 不再有任何抑制语义 ——
     *   保留清零是为了让"一场会话里发生了几次边沿"仍然可读。 */
}

/* 【r108】取走一次"DOS 会话刚开始"的请求(取走即清) —— 见 s_entryEdge 注。 */
uint16_t __far __pascal VxdTakeSessionStartEdge( void )
{
    uint16_t v = s_entryEdge;

    s_entryEdge = 0;
    return v;
}

/* 【r109】读一次"最近一次采样的切屏状态位"(不清、不改变任何东西)。
 * 产生者: VxdTraceDump 里对 blob 头 204 字节的采样(见 s_stBitsNow 注)。
 * 消费者: display.c 的 MaybeDeferredRestore —— 入口侧补救在动手之前要判
 *   "这次 DOS 会话是不是真全屏": bit0(VGA 通路)置起说明主 VDD 真的把硬件
 *   切给了 DOS 盒(全屏), 那时我们插一次 4F02 就是跟 DOS 盒抢硬件; bit0
 *   清着(窗口化)才可以重建。返回值就是那一个字节, 由调用方自己判位。
 * 【为什么不用 s_stFlags】那是"上一次落盘时的值", 语义是差分用的。 */
uint16_t __far __pascal VxdStateBitsNow( void )
{
    return (uint16_t)s_stBitsNow;
}

/*
 * VxdTraceDump —— 取回 mini-VDD 分派追踪并落盘(只打新记录)。
 *   【零副作用】只读 VxD 锁定数据 + 写日志; 任何一步失败都静默返回。
 *   【为何 32 位读】记录里的 ECX/EDX 是 32 位; x86 非对齐访问安全, 且
 *   缓冲在本模块 DGROUP 内(近指针直读)。
 */
/* 【r144】VxdBoxArmedNow —— "全屏 DOS 盒正持屏"的权威判据。
 *   来源: mini-VDD 的 4 号(PRE_HIRES_TO_VGA)置 1 / 7 号(POST_VGA_TO_HIRES)清 0
 *   —— DDK 对这一对钩子的定义就是"在 Windows 与全屏 MS-DOS 会话
 *   之间切换视频焦点", 所以它就是"屏幕归谁"的官方边界。
 *   【为什么不能用 VBE_TR_ST_VGA】那个位由 4/5/11 号置、0/7/12 号清,
 *   而 11/12 号(ACCESS_VGA/LINEAR_MEMORY_MODE)在【窗口化】会话里就来回拨
 *   它 —— ./143 的第一场窗口会话正是因为它恰好置起而被
 *   当成"盒子拿走了屏"处理(延后不重建), 结果整场 9 分钟
 *   没有任何一次模式建立。详见 R144_DELIVERY §1。 */
uint16_t __far __pascal VxdBoxArmedNow( void )
{
    /* 【r145 修正 —— r144 的语义错误】
     *   blob 里那个字节有三个取值:
     *     0 = 本次开机还没走过一次 4→7;
     *     1 = 正在武装期(盒子持屏);
     *     2 = 已经被 7 号解除(证据用, 见 minivdd.asm 的 TraceStub_7)。
     *   r144 把原始字节直接返回, 而调用处写的是
     *   `if (VxdBoxArmedNow())` ⇒ 2 也被当成"盒子持屏"。一次全屏往返之后
     *   这个字节【永远】是 2 ⇒ 每一条入口边沿都走"只重绘、不重建"。
     *   ./144 的硬证据(日志 16443 行, MARK ENTR T=0002A7B8):
     *       PH: fbox arm==0002
     *       DeferredRestore: entry edge -> repaint only (box owns the screen)
     *   —— 盒子刚把屏还回来, 我们却按"盒子还拿着屏"处理,
     *   于是桌面从头到尾没人重建 —— 用户看到的"窗口 command
     *   持续粉-橙 / 全屏切回窗口持续青色"就是这一支。
     *   ⇒ 本函数从 r145 起只把 1 当"持屏", 0 与 2 都算"屏幕不在盒子手里"。 */
    return (uint16_t)((s_boxArmed == 1) ? 1 : 0);
}

uint16_t __far __pascal VxdTraceDump( void )
{
    uint8_t  *b = s_trcBuf;
    uint32_t  total, seq, oldest, f;
    int       ok = 0;                 /* 与 VxDCallApi 的 int far * 形参同型 */
    uint16_t  printed = 0;

    if (!VxD_Connect())
        return 0;

    /* 【r107】心跳: 本函数是"光标每次被移动 / 每个 DDI 首调"都会经过的地方,
     * 于是这里每 ~2 秒落一行 MARK HB —— 崩溃前系统还活到第几拍, 日志里
     * 直接可数; 尾部与主机 mtime 的差也就有了读数(见 drv.h 的说明)。 */
    DiagBeat();

    /* 【r100 / D1 修正】api_gettrace 走的是【请求块约定】(与 GET_MODELIST
     * 完全同形): VxD 从 [ESI+VXE_bufLin]/[+VXE_bufLen] 取客户端输出缓冲。
     * r99 把"输出缓冲自己的线性地址"当请求块地址传了进去 —— s_trcBuf 是
     * BSS(全 0别), VxD 读到的 VXE_bufLin = 0, 判定"缓冲不可用"回 CY+92,
     * drv 侧 ok=0 后【静默返回】: 整个 r99 的 49 项分派追踪一条都没落盘,
     * 连"我失败了"都看不见。这里改成请求块写法, 并且失败必须留痕。
     * 【g_xlin==0 守卫】请求块未就绪时传 0 会让 VxD 在 ring 0 读线性 0
     * —— 那是 0E 蓝屏。                                                    */
    if (g_xlin == 0)
        return 0;
    g_xreq.bufLen = sizeof(s_trcBuf);
    g_xreq.bufLin = LinearOf(s_trcBuf);
    {
        uint16_t st = VxDCallApi(VBE_API_GET_TRACE, g_xlin, sizeof(g_xreq), &ok);

        if (st == 0xFFFF)
            return 0;
        if (!ok)
        {
            DiagHex( "TRC fail: status", st );
            FlushLog();
            return 0;
        }
    }

    /* --- 首帧: 打一遍非零计数表(这是"开机以来主 VDD 问过什么"的基线) */
    if (!s_trcHdrDone)
    {
        s_trcHdrDone = 1;
        DIAG( "TRC hdr: nonzero func counters (func,count)" );
        for (f = 0; f < VBE_TRACE_FUNCS; f++)
        {
            uint32_t c = *(uint32_t *)(void *)(b + VBE_TR_Counts + f * 4);

            if (c != 0)
                DiagTraceCnt( (uint16_t)f, c );
        }
        FlushLog();
    }

    /* --- 【r120】ring 0 物理读 DISPI 寄存器 ---------------------------
     * 【为什么必须单独一问】./119 的 ring 3 "写—回读—还原"梯形在整场
     * 会话里跑了 28 次, 每一次都是 pre=FFFF / 写 BE02 / 回读 FFFF, 于是
     * 探针判定"本机没有 DISPI"。但那【不是】定论, 它有两种解释, 而两者的
     * 后续动作完全相反:
     *   ① 这台机器的 VBE 设备没有在 1CEh/1CFh 上注册处理器 ⇒ DISPI 这条
     *      线彻底关门, 下一轮去查 CRTC/时序器;
     *   ② 端口在, 但【我们 ring 3 的那次 I/O 没抵达硬件】(主 VDD 在为
     *      Windows VM 虚拟化这两个端口) ⇒ 通道是通的, 只是问法不对。
     * ring 3 分不出这两种。ring 0 的 VDD_Do_Physical_IO 绕开 VM 的端口
     * 虚拟化, 是唯一能分开它们的通路 —— 所以这一枪值一轮。
     * 【开关】INI DispiPhys(默认 0 = 关)。关着时一个新字节都不写、一次
     * 物理 I/O 都不发, 与 r119 完全同形。                                  */
    if (g_runDispiPhys)
    {
        /* 【为什么把"可用性"和"读数"分开】MIN 构建启动时 minivdd.vxd 可能还是
         * 旧的那一个(本轮改了它 ⇒ 必须重装)。旧 VxD 不认识功能号 15, 会回
         * 87(ERROR_INVALID_PARAMETER) ⇒ VxDCallApi 报 ok=0。此时若把整段
         * 都放进 DIAG 通道, MIN 构建会【静默什么都不说】, 用户只能看到"没有
         * DPHYS 行", 分不清"开关没生效 / VxD 没换 / 物理读失败"。
         * 所以可用性走 DIAGMIN(一条), 详细读数才走 DIAG。                */
        uint16_t dpn = VxDCallApi( VBE_API_DISPI_PHYS, 0, 0, &ok );

        if (!ok || dpn == 0xFFFF)
        {
            DIAGMIN( "DPHYS unavailable: mini-VDD does not answer VBE_API_DISPI_PHYS(15) - old minivdd.vxd installed?" );
            FlushLog();
        }
        else
        {
            uint8_t *dp = b + VBE_TR_DispiPhys;
            uint8_t  dn = *(uint8_t *)(void *)(dp + VBE_TR_DP_N);   /* 读次数 */
            uint8_t  dok= *(uint8_t *)(void *)(dp + VBE_TR_DP_OK);  /* 命中   */
            uint8_t  der= *(uint8_t *)(void *)(dp + VBE_TR_DP_ERR);
            uint16_t did = *(uint16_t *)(void *)dp;
            uint16_t dxr = *(uint16_t *)(void *)(dp + 2);
            uint16_t dyr = *(uint16_t *)(void *)(dp + 4);
            uint16_t dbp = *(uint16_t *)(void *)(dp + 6);
            uint16_t den = *(uint16_t *)(void *)(dp + 8);
            uint16_t dvw = *(uint16_t *)(void *)(dp + 10);
            int      chg = (dn  != s_dpSeen)  || (dok != s_dpOkLast) ||
                           (did != s_dpIdLast) || (dxr != s_dpXrLast) ||
                           (dyr != s_dpYrLast) || (dbp != s_dpBpLast) ||
                           (den != s_dpEnLast) || (dvw != s_dpVwLast);

            /* 记账搬到 VxD 侧了(DispiPhysN), 所以第一次就落盘; 之后只在
             * 读数变化时落 —— 这条路径每次 VxdTraceDump 都会跑, 不能刷屏。 */
            if (chg)
            {
                s_dpSeen = dn; s_dpOkLast = dok;
                s_dpIdLast = did; s_dpXrLast = dxr; s_dpYrLast = dyr;
                s_dpBpLast = dbp; s_dpEnLast = den; s_dpVwLast = dvw;

                DiagMark( "DPHYS" );
                DiagHexMin( "DPHYS reads (ring0 physical)", (uint16_t)dn );
                DiagHexMin( "DPHYS id echo 1=port alive",  (uint16_t)dok );
                DiagHexMin( "DPHYS fails",                 (uint16_t)der );
                DiagHexMin( "DPHYS id",   did );
                DiagHexMin( "DPHYS xres", dxr );
                DiagHexMin( "DPHYS yres", dyr );
                DiagHexMin( "DPHYS bpp",  dbp );
                DiagHexMin( "DPHYS enable", den );
                DiagHexMin( "DPHYS virtw",  dvw );
                /* 【结论行】把两种可能写成一句可判读的话 —— 下一轮看日志
                 * 不需要再回头读源码:
                 *   echo=1 ⇒ 端口活着, ring 3 那条路是被 VM 端口虚拟化吃掉的;
                 *   echo=0 且 reads 在涨 ⇒ 物理 I/O 也问不到, DISPI 关门。 */
                if (dok)
                    DIAGMIN( "DPHYS verdict: port ALIVE (ring0 echo ok) -> ring3 probe was blocked by VM port virtualization" );
                else
                    DIAGMIN( "DPHYS verdict: no echo even from ring0 physical I/O -> this VBE has no DISPI port" );
                if (dok && dxr != 0)
                    DiagHexMin( "DPHYS pitch bytes", (uint16_t)(((DWORD)dvw * (DWORD)dbp) / 8UL) );
                else if (der != 0)
                    DiagHexMin( "DPHYS write-probe error count", (uint16_t)der );
                FlushLog();
            }
        }
    }

    /* --- 【r100】VxD 侧事件计数器(VBE_TR_Evt) -------------------------
     * byte0 = POST_VGA_TO_HIRES(7) 的累计次数。它的变化 = 主 VDD 的切屏
     * 事务已收尾(DDK 时序第 6 步) ⇒ 现在是"可以安全重建模式"的时刻。
     * 回调内部绝不能做 BIOS 调用, 所以这个信号是延迟重建的唯一安全门。 */
    {
        uint8_t e0 = *(uint8_t *)(void *)(b + VBE_TR_Evt + VBE_TR_EVT_POST_VGA_TO_HIRES);
        uint8_t e1 = *(uint8_t *)(void *)(b + VBE_TR_Evt + VBE_TR_EVT_PRE_HIRES_TO_VGA);
        uint8_t e2 = *(uint8_t *)(void *)(b + VBE_TR_Evt + VBE_TR_EVT_DISPLAY_DISABLING);
        uint8_t e3 = *(uint8_t *)(void *)(b + VBE_TR_Evt + VBE_TR_EVT_PRE_VGA_TO_HIRES);
        /* 【r131 修正一处"假仪器"】r129 在这里读 `VBE_TR_Evt + 4` 与 `+5`,
         *   当作 "HiResClockFix 真动手 / 被挡下" 的计数。可是 blob 里
         *   `VBE_TR_Evt = 4` 而 `VBE_TR_Counts = 8` —— 事件区只有【4 字节】
         *   (+0..+3), 于是 +4/+5 落在 `b + 8` / `b + 9`, 也就是
         *   **VBE_TR_Counts[0]/[1] 的低字节** = "分派功能 0 被调用了几次"
         *   (功能 0 = REGISTER_DISPLAY_DRIVER)的计数器。所以 r129/r130 日志里
         *   那句
         *       TRC evt HiResClk fix=0001
         *   一直只是"0 号功能被调用过 1 次", 与补写毫无关系 ——
         *   **r129 关于"回高分辨率那一刻三个位确实被补过"的证据是幻影, 该
         *   结论撤回**(仍然成立的是 atc10/gcr06/SR01 三个【直接读数】)。
         *   minivdd.asm 侧确实把 TraceEvt 声明成 8 字节, 但那 8 字节在 VxD
         *   自己的变量区里; api_gettrace 只把一个 dword([TraceEvt+0..3])搬进
         *   blob, 后 4 字节从来没有到过 ring 3。
         *   本轮处置: 不再读这两个偏移、也不再往这块区域加任何新"计数器";
         *   改成在同一个事件边沿【直接读硬件】—— SR01 / GCR06 / AR10 / CR17h
         *   四个读数就是"这一刻硬件到底对不对"的现场证据, 比计数器强。
         *   (blob 头部 4..235 已经被 49 项调用计数/状态/Stat2/DispiPhys 占满,
         *    没有空字节可放新计数器 —— 这也是"改读硬件"的直接理由。)
         *   r131-marker: TraceEvt +4/+5 are NOT in the blob (they alias
         *                VBE_TR_Counts[0..1]); read the hardware instead. */
        if (e0 != s_evtPost || e1 != s_evtPreH2V || e3 != s_evtPreV2H)
        {
            uint16_t a10;
            uint16_t g06;
            uint16_t c17;

            /* 【r135】增量方向 —— 必须在下面改写 s_evtPost/s_evtPreH2V 【之前】
             *   算, 否则比较的基准就没了。只记"这一次变化里是谁涨了"。   */
            s_evtDir = 0;
            if (e0 != s_evtPost)
                s_evtDir |= 0x01;       /* POST_VGA_TO_HIRES(7) = 回 Windows */
            if (e1 != s_evtPreH2V)
                s_evtDir |= 0x02;       /* PRE_HIRES_TO_VGA(4)  = 盒子拿屏  */

            s_evtPost   = e0;
            s_evtPreH2V = e1;
            s_evtPreV2H = e3;
            DiagHexMin( "TRC evt postVH", e0 );
            DiagHexMin( "TRC evt preHV",  e1 );
            DiagHexMin( "TRC evt preVH",  e3 );
            PHASEV( "PH: evt postVH=", e0 );
            /* 【r129】同一刻把"文本还是图形"的另两个判定位也落盘, 它只发生
             *   在事件计数器变化时(一个 Windows 会话里个位数次), 所以不受
             *   Probe2 门管辖 —— 少了它, "fix 计数为 0" 就分不清是"本来就
             *   好"还是"读错了端口"。见 drv.c 的 VxdReadAtcReg 长注。
             * 【r129 先取值再打印 —— argaudit 当场抓到过】这两行第一版写成
             *   `DiagHexMin( "...", VxdReadAtcReg( 0x10 ) )`, 也就是把【端口读】
             *   写在日志宏的实参里。而发布构建的 DiagHexMin 是不展开实参的空
             *   宏 ⇒ release 里那两个 in 指令会【整条消失】, 同一个 MIN 与
             *   release 就不是同一台仪器了。r127 已经栽过一模一样的跟头(端口
             *   读写在 DiagHex 实参里), 所以本项目的纪律是: 一切端口操作先落
             *   成语句, 只把结果交给日志宏。argaudit.py 就是查这件事的。
             * 【r129 又一个坑: 声明必须放在块首】第一版把这两个变量声明写在
             *   了语句【之后】, wcc 立刻报
             *     drv.c(3584): Error! E1058: Cannot use typedef 'uint16_t' as a variable
             *   —— 本文件按 C89 编译(声明必须在块首), 且 drv.h 里 uint16_t/WORD
             *   都是 typedef, 于是"语句后声明"被当成把 typedef 当变量名用。 */
            a10 = VxdReadAtcReg( 0x10 );
            g06 = VxdCrtProbeRead( 0x206 );
            /* 【r131】同一刻把 CRTC 17h 也读出来 —— 这是"长条"的现场判据。
             *   bit7 = SE(Sync Enable, FreeVGA Index 17h): 我们期望它是 1。 */
            c17 = VxdCrtMode17();

            DiagHexMin( "TRC atc10 bit0 (1=graphics)", a10 );
            DiagHexMin( "TRC gcr06 bit0 (1=graphics)", g06 );
            DiagHexMin( "TRC cr17 raw (bit7=SE,bit6=Word/Byte)", c17 );
            DiagHexMin( "TRC cr17 SE state (1=SET,0=CLEAR,FFFF=unreadable)",
                        (uint16_t)((c17 == 0xFFFF) ? 0xFFFF : ((c17 & 0x80) ? 1 : 0)) );
            PHASEV( "PH: evt CR17=", c17 );
            PHASEV( "PH: evt SE=",
                    (c17 == 0xFFFF) ? 0xFFFF : ((c17 & 0x80) ? 1 : 0) );
            FlushLog();
        }
        if (e0 != s_evtPostSeen)
        {
            s_evtPostSeen = e0;
            s_safeToSetMode = 1;        /* 交给 VxdTakeSafeToSetMode 取走 */
            /* 【r108】同一个事件再产出一个"该重建了"的请求 —— 见 s_v2hEdge
             * 的长注。这一行就是"运行 command 花屏"与"exit 才恢复"之间那
             * 1.5 秒时差的来源: 事件在这里被看到, 重建却要等到 TRAPS 落下。*/
            /* 【r145】POST_VGA_TO_HIRES 是【官方】的"硬件已经还给 Windows"
             *   证据(DDK 第 6 步), 同样不能有"用满 8 次就永远不报"。 */
            if (s_seenVgaSess)
            {
                s_v2hN++;
                s_v2hEdge = 1;
                DiagMark( "V2H" );
                DIAGMIN( "V2H: POST_VGA_TO_HIRES event -> arm bottom-half rebuild" );
                DiagHexMin( "V2H: evt postVH count", (uint16_t)e0 );
                DiagHexMin( "V2H: evt preHV count",  (uint16_t)e1 );
                DiagHexMin( "V2H: evt preVH count",  (uint16_t)e3 );
                DiagHexMin( "V2H: state bits",       (uint16_t)s_stFlags );
                DiagHexMin( "V2H: i10 n",            (uint16_t)s_stI10Cnt );
                DiagHexMin( "V2H: i10 ax",           s_stI10Ax );
                DiagHexMin( "V2H: arm n",            (uint16_t)s_v2hN );
            }
            else if (!s_seenVgaSess)
            {
                DIAGMIN( "V2H: event seen but no VGA session yet -> ignored" );
            }
            FlushLog();
        }
        (void)e2;
    }

    /* --- 【r101】mini-VDD 切屏状态机(blob 头 204..207) ------------------
     * 0/4/5/7/11/12/13/14/26 号服务维护的 4 个字节, 变化才落盘:
     *   204 db 状态位: bit0 = 硬件已被切到 VGA(4 平面/文本)通路
     *                   bit1 = ENABLE_TRAPS(13) 已生效
     *                   bit2 = DISPLAY_DRIVER_DISABLING(26) 已发生
     *   205 db PRE_INT_10_MODE_SET(48) 被调用次数(模 256)
     *   206 dw 48 号最近一次请求的模式号 —— "DOS 应用到底请求了什么模式"
     *          的第一现场(环形记录会被 23 号的轮询冲掉, 这个位置不会)。
     * 这三行是 r101 回答"主 VDD 把硬件当成 VGA 还是 hires"的唯一直接证据。 */
    {
        uint8_t  sf  = *(uint8_t  *)(void *)(b + VBE_TR_State);
        uint8_t  i10c= *(uint8_t  *)(void *)(b + VBE_TR_I10Cnt);
        uint16_t i10a= *(uint16_t *)(void *)(b + VBE_TR_I10Ax);

        /* 【r108】粘滞标记: 只要见过一次 VGA 位或 TRAPS 位, 就说明本 Windows
         * 会话里确实发生过一次 DOS 虚拟化会话。s_v2hEdge 靠它区分"会话里
         * 的那趟往返"与"开机期主 VDD 自己的往返"(见 s_v2hEdge 的长注)。
         * 必须放在"变化才落盘"的 if 之外: 它要的是"见过", 不是"变了"。 */
        if (sf & (VBE_TR_ST_VGA | VBE_TR_ST_TRAPS))
            s_seenVgaSess = 1;
        /* 【r109】每次 dump 都刷新"最近一次采样的状态位" —— display.c 的
         * 入口侧补救拿它判"这次 DOS 会话是不是真全屏"(见 s_stBitsNow 注)。
         * 同样必须在"变化才落盘"之外。                              */
        s_stBitsNow = sf;
        /* 【r144】持屏期监视位 + 持屏期指纹 —— 必须在"变化才落盘"
         *   的 if 之外每拍刷新: display.c 的入口边沿判据要用它
         *   (VxdBoxArmedNow), 而它在一场会话里会变。 */
        s_boxArmed    = *(uint8_t *)(void *)(b + VBE_TR_Fbox + VBE_TR_FBOX_DISARM);
        /* 【r146】节流倒计时已撤除(见文件上方 s_edgeSkipEnd 的长注)。 */
        {
            uint16_t k;
            for (k = 0; k < VBE_TR_FS_LEN; k++)
                s_fboxSnap[k] = *(uint8_t *)(void *)(b + VBE_TR_FboxSnap + k);
            for (k = 0; k < 8; k++)
                s_hires[k] = *(uint8_t *)(void *)(b + VBE_TR_Hires + k);
            /* 【r147】六个采集槽 + 补写记账 */
            for (k = 0; k < (VBE_TR_PS_SLOTS * VBE_TR_PS_LEN); k++)
                s_phase[k] = *(uint8_t *)(void *)(b + VBE_TR_PhaseSnap + k);
            for (k = 0; k < VBE_TR_MC_LEN; k++)
                s_memc[k] = *(uint8_t *)(void *)(b + VBE_TR_MemC + k);
            /* 【r151】所有权 / 特殊 VM / 35 号 AL */
            for (k = 0; k < VBE_TR_OWN_LEN; k++)
                s_own[k] = *(uint8_t *)(void *)(b + VBE_TR_Own + k);
            for (k = 0; k < VBE_TR_PR_LEN; k++)
                s_probe[k] = *(uint8_t *)(void *)(b + VBE_TR_Probe + k);
        }
        /* 【r142】“盒子拿走屏幕”边沿(见 s_prevVgaBit 注)。只认上升沿,
         *   且只把它当“可以安全动手”的第二条信号 —— 不用它发起重建。 */
        if ((sf & VBE_TR_ST_VGA) && !(s_prevVgaBit & VBE_TR_ST_VGA))
        {
            s_safeToSetMode = 1;
            DiagHexMin( "TRC evt boxTookScreen (VGA bit rising, r142)", (uint16_t)sf );
            FlushLog();
        }
        s_prevVgaBit = sf;

        if (sf != s_stFlags || i10c != s_stI10Cnt || i10a != s_stI10Ax)
        {
            uint8_t i10Prev = s_stI10Cnt;   /* r109: 判"48 号是否又发生" */

            (void)i10Prev;      /* 发布构建里下面的用户被 #ifdef 编掉 */
            s_stFlags  = sf;
            s_stI10Cnt = i10c;
            s_stI10Ax  = i10a;
            /* 【r107】时间轴: 这一行是整个分析里最要紧的时间戳 —— 它给出
             * "主 VDD 把硬件切进/切出 DOS 会话"发生在第几拍。./104 的 t4
             * 蓝屏就发生在这一瞬间(好会话在这里之后才继续), 而 .bsod 日志
             * 里这一行【根本没出现过】—— 有了 tick 才能判定那意味着
             * "边沿没发生"还是"边沿的日志没写出去"。                  */
            DiagMark( "STATE" );
            DiagHexMin( "TRC st bits",  (uint16_t)sf );   /* bit0 VGA/bit1 traps */
            /* 【r116】把 bit3..bit7 摊成一行可读的读数 —— 这几个位是本轮
             * 两次改动是否真的执行过的唯一证据(VBE_VXD_VERSION 没变, 新旧
             * VxD 可混搭部署, 所以"装了 r116 的 minivdd.vxd"只能靠它们):
             *   vgafix   = 文本态修补 跑过(bit3, r114);
             *   dispi    = 1CEh/1CFh 全局陷阱最近一次是开着的(bit4);
             *   dispiEat = 至少吃掉过一次 DISPI I/O(bit5, 置起后不清零);
             *   dispiPass= 至少放行过一次(bit6);
             *   vtfBlock = 文本态修补 被保守闸门挡下过(bit7, 跑通即清)。
             * 读法: dispiEat=1 证明"窗口 DOS 盒真的去写 VBE_DISPI 了, 而
             * 我们挡住了"; dispiEat=0/dispiPass=1 说明陷阱生效但没有需要挡
             * 的访问(那么花屏另有原因); dispi=0 说明安装或策略没到位。 */
            DiagHexMin( "TRC st decode vgafix/dispi/dispiEat/dispiPass/vtfBlock",
                     (uint16_t)(((sf & VBE_TR_ST_VGAFIX)     ? 1 : 0) |
                                ((sf & VBE_TR_ST_DISPI)      ? 2 : 0) |
                                ((sf & VBE_TR_ST_DISPI_EAT)  ? 4 : 0) |
                                ((sf & VBE_TR_ST_DISPI_PASS) ? 8 : 0) |
                                ((sf & VBE_TR_ST_VTFBLOCK)   ? 16 : 0)) );
            DiagHexMin( "TRC st i10 n", (uint16_t)i10c );
            DiagHexMin( "TRC st i10 ax",i10a );
            /* 【r135】48 号又发生了一次 ⇒ 刷新"最近一次设模式请求的方向"。
             *   判据只用模式号本身(标准 BIOS 模式 <= 0x13), 与机种无关:
             *   主 VDD 为 DOS 盒设的必然是标准 VGA 模式, 为 Windows 设的
             *   必然是扩展/伪模式(本机 0x0072)。0xFFFF = 还没见过任何一次。 */
            if (i10c != i10Prev)
                s_lastSetDir = (uint8_t)(((i10a != 0) && (i10a <= 0x13)) ? 2 : 1);
            /* 【r117】DISPI 记账块(blob 头 208..211) —— 上一轮整轮零信息的
             * 那三个问题, 现在各自有直接读数:
             *   inst = 装成的端口位图(bit0=1CEh, bit1=1CFh; 3 = 都装成)。
             *          r116 恒 0 的原因是端口号写在了 EAX 而不是 EDX(DDK
             *          原文: mov edx, Port), 第一次装到 30h 上、第二次撞
             *          "该端口已有 handler"⇒ CY ⇒ 整块跳过。
             *   trap = DispiTrapOn 当前值(0/1)。
             *   acts = Enable/Disable_Global_Trapping 真正执行过的次数 ——
             *          它是"钩子确实跑到过"的证据(策略关/没装成时不动)。
             *   errs = Install_IO_Handler 失败次数。
             * 读法: inst=0003/errs=0000 且 acts 随边沿增长 ⇒ 机制活着;
             *       inst=0000/errs=0002 ⇒ 又没装上(把这一行寄回来即可)。 */
            DiagHexMin( "TRC st dispi inst", (uint16_t)*(uint8_t *)(void *)(b + VBE_TR_Stat2 + VBE_TR_ST2_INST) );
            DiagHexMin( "TRC st dispi trap", (uint16_t)*(uint8_t *)(void *)(b + VBE_TR_Stat2 + VBE_TR_ST2_TRAP) );
            DiagHexMin( "TRC st dispi acts", (uint16_t)*(uint8_t *)(void *)(b + VBE_TR_Stat2 + VBE_TR_ST2_ACT) );
            DiagHexMin( "TRC st dispi errs", (uint16_t)*(uint8_t *)(void *)(b + VBE_TR_Stat2 + VBE_TR_ST2_ERR) );
            FlushLog();
            /* 【r107】这一段窗口里主 VDD 到底问了什么: 49 个桶的增量。
             * 旧的日志只有"开机以来"那张表(在第一次状态边沿时打, 那时
             * DOS 会话还没开始), 于是"DOS 会话期间被调用的是哪些服务"
             * 在 ./106 里只能靠环里被 35 号冲剩下的残渣去猜。       */
#ifdef VBE9X_DIAG
            DIAG( "TRC + : delta counters since last state edge" );
            TraceCntDelta( b );
            /* 【r144】绝对计数表 —— 每次状态边沿都打一份。
             *   【为什么非要有它】`TRC + f=NN` 是【增量】行, 而日志闸门
             *   会在一个块中间关掉, 把后半段整段吞掉: ./142 的
             *   line 5716 与 ./143/stop 的 line 1371 都是一份"表头 + 零条目"的
             *   增量块, 而同一窗口里计数确实涨过(环记录为证)。
             *   于是"某个功能号到底被调用过没有"这类问题在增量
             *   表上永远答不了 —— r142 就因此把"4/5/6/7 从不被调用"
             *   这个错结论写进了分析。单写者累加量
             *   (VBE_TR_Counts[f]) 不受丢行影响。 */
            DIAG( "TRC abs: absolute per-function counters (absent = never dispatched)" );
            {
                uint16_t fa;

                for (fa = 0; fa < VBE_TRACE_FUNCS; fa++)
                {
                    uint32_t ca = *(uint32_t *)(void *)(b + VBE_TR_Counts + fa * 4);

                    if (ca != 0)
                        DiagTraceCnt( fa, ca );
                }
            }
            FlushLog();
            /* 【r107】边沿这一刻把环里【非 35 号】的记录整段打出来:
             * 这是"会话开始/结束那一瞬间主 VDD 在做什么"的唯一可见窗口
             * (常规打印按序号走, 打出来的 16 条全是 35 号)。       */
            {
                uint32_t tot2    = *(uint32_t *)(void *)b;
                uint32_t oldest2 = (tot2 > VBE_TRACE_RECS)
                                 ? (tot2 - VBE_TRACE_RECS + 1) : 1;
                uint32_t f23     = 0;
                uint32_t storm2[3];

                storm2[0] = storm2[1] = storm2[2] = 0;
                (void)TraceDumpFiltered( tot2, oldest2, 48, &f23, storm2 );
                if (f23)
                    DiagHex32( "TRC f23 at edge (hires poll, suppressed)", f23 );
                /* 【r110】银行循环洪泛: 边沿这一刻它的量最要紧(它正是
                 * "主 VDD 在搬屏"的度量), 所以在这里把三个数打出来。*/
                if (storm2[0] || storm2[1] || storm2[2])
                {
                    DIAG( "TRC storm at edge: RESET_BANK/RESTORE_REG/MAKE_NOT_BUSY" );
                    DiagHex32( "TRC storm RESET_BANK",  storm2[0] );
                    DiagHex32( "TRC storm RESTORE_REG", storm2[1] );
                    DiagHex32( "TRC storm MAKE_NOT_BUSY", storm2[2] );
                }
                /* 【r109】48 号(设模式请求)的完整记录单独捞一次 ——
                 * 条件: 这次状态变化里 48 号的计数确实动了。见
                 * TraceDumpFunc48 头注(它为什么必须单独捞)。       */
                if (i10c != i10Prev)
                    TraceDumpFunc48( tot2, oldest2 );
            }
#endif
            FlushLog();
            /* 【r103】状态位一变 = 主 VDD 正在把硬件切进/切出 DOS 会话;
             * 正是花屏发生的那一刻, 顺手取一份帧缓冲原始字节(见 drv.h 的
             * DiagFbSample 说明)。放这里而不是别处: 只在"边沿"上取,
             * 不会变成每条记录都取样。                                */
            DiagFbSample();
            FlushLog();
            /* 【r107】边沿探针(display.c): 问硬件此刻在什么模式 + 探一次
             * 用户配置的 Crash=<段号> —— 段号若是在这条边沿之后才被分配
             * 的, 只有在这里问才可能问到它。                          */
            DiagEdgeProbe();
        }

        /* --- 【r104】DOS 虚拟化会话"收尾边沿"(见文件上方 s_vgaSessEnd 的
         * 长注释)。必须放在"状态位变化才落盘"的判断【之外】: 这里关心的是
         * 边沿本身, 而边沿的定义就是"这一次采样里 bit1 从 1 变成 0"。
         * 注意 s_prevTrcFlags 初值 0: 开机后第一次采样绝不会误判成边沿。 */
        if ((s_prevTrcFlags & VBE_TR_ST_TRAPS) && !(sf & VBE_TR_ST_TRAPS))
        {
            /* 【r145 —— 永久失锁的修正】上游 r104..r144 写的是
             *   `if (s_vgaEdgeN < 8)`, 而那个 8 是【每次开机】的硬上限:
             *   用满之后 s_vgaSessEnd 再也不置 ⇒ 显示驱动再也收不到
             *   "会话收尾"这条边沿。见文件上方 s_edgeSkipEnd 的长注(r146 起
 *   边沿一律置请求, 不再有任何节流)。 */
            {
                s_vgaEdgeN++;
                s_vgaSessEnd = 1;
                DiagHexMin( "TRC evt sessEnd flags", (uint16_t)sf );
                DiagHexMin( "TRC evt sessEnd n",     (uint16_t)s_vgaEdgeN );
                DiagHexMin( "TRC evt sessEnd i10ax", i10a );
                DiagHexMin( "TRC evt sessEnd skipN", (uint16_t)s_edgeSkipEnd );
                DiagHex32( "TRC evt sessEnd calls",
                           *(uint32_t *)(void *)b );  /* = VBE_TR_WriteIdx 现值 */
                FlushLog();
            }
        }
        /* --- 【r108】"DOS 虚拟化会话开始"边沿(TRAPS 上升沿)-------------
         * 与上面那条收尾边沿严格对称, 供 EntryHeal 使用(默认关):
         * 它回答的是"整屏花屏是从打开 DOS 盒那一刻就开始的吗"这一个问题。
         * 见文件上方 s_entryEdge 的长注释。                            */
        if (!(s_prevTrcFlags & VBE_TR_ST_TRAPS) && (sf & VBE_TR_ST_TRAPS))
        {
            /* 【r145】同上: 上限 8 改成"边沿一律置请求 + 独立节流"。 */
            {
                s_entryN++;
                s_entryEdge = 1;
                /* 【r135】在这一拍把方向【定格】。优先级: 先看"主 VDD 最近一次
                 *   为 VM 设了什么模式"(有序证据, 每次设模式都刷新, 不可能被
                 *   采样错过), 它没有才退到事件计数器增量。
                 *   为什么必须在【这一拍】定格: TRAPS 上升沿 = 一次切屏事务的
                 *   收尾(DDK PRE_HIRES_TO_VGA 页第 3/7 步), 而模式设置(第 6 步)
                 *   就在它之前 ⇒ 读到的就是【本次】事务的方向。        */
                s_edgeDirNow = (uint8_t)((s_lastSetDir != 0) ? s_lastSetDir :
                                         ((s_evtDir == 0x01) ? 1 :
                                          ((s_evtDir == 0x02) ? 2 : 0)));
                CbRingPut( (uint16_t)(((uint16_t)CB_EVT_EDGE_DIR << 8) |
                                      (s_edgeDirNow & 0xFF)) );
                DiagMark( "ENTR" );
                DIAG( "ENTR: TRAPS rising edge (DOS session start) -> request rebuild" );
                DiagHex( "ENTR: state bits", (uint16_t)sf );
                DiagHex( "ENTR: i10 n",      (uint16_t)i10c );
                DiagHex( "ENTR: i10 ax",     i10a );
                DiagHex( "ENTR: arm n",      (uint16_t)s_entryN );
                /* 【r135】方向与两条证据一起落盘 —— 下一次"因果判不出来"就是
                 *   因为缺这几行。 */
                DiagHex( "ENTR: dir (1=back to Windows,2=box took screen,0=unknown)",
                         (uint16_t)s_edgeDirNow );
                DiagHex( "ENTR: dir ev last mode set AX", s_stI10Ax );
                DiagHex( "ENTR: dir ev evt delta (bit0=postVH,bit1=preHV)",
                         (uint16_t)s_evtDir );
                DiagHex( "ENTR: skip n (edgeHold>0)", (uint16_t)s_edgeSkipEnt );
                FlushLog();
            }
        }
        s_prevTrcFlags = sf;

        /* --- 【r144】盒子持屏期取证(blob 212..215 / 232..235) ---------
         *   【为什么改成"无条件落盘"】r143 的 S1/S2 是"变了才打",
         *   而它们始终是 0 ⇒ 整轮零读数, 分不清"没采到"
         *   "还是"没变过"。现在每个状态边沿都打, 一行一个数。 */
        {
            uint8_t fam  = *(uint8_t *)(void *)(b + VBE_TR_Fbox + VBE_TR_FBOX_I10MAP);
            uint8_t fn35 = *(uint8_t *)(void *)(b + VBE_TR_Fbox + VBE_TR_FBOX_N35);
            uint8_t fn48 = *(uint8_t *)(void *)(b + VBE_TR_Fbox + VBE_TR_FBOX_N48);
            uint8_t farm = *(uint8_t *)(void *)(b + VBE_TR_Fbox + VBE_TR_FBOX_DISARM);
            /* 【r146】本拍与上一拍是否完全一样 —— 一样就整块不写。
             *   键 = 计数器 4 + 指纹 16 + hi-res 记账 4 = 24 字节。 */
            uint8_t key[VBE9X_FBOX_KEYLEN];
            uint16_t kk;
            uint8_t same = 1;

            key[0] = farm; key[1] = fn35; key[2] = fn48; key[3] = fam;
            for (kk = 0; kk < VBE_TR_FS_LEN; kk++)
                key[4 + kk] = s_fboxSnap[kk];
            for (kk = 0; kk < 4; kk++)
                key[20 + kk] = s_hires[kk];
            for (kk = 0; kk < VBE_TR_MC_LEN; kk++)
                key[24 + kk] = s_memc[kk];
            /* 【r150】48 号分类计数也进"变了才落盘"的键: 它们一变就说明又有
             *   新的设模式请求被分类过 —— 那正是本轮要看见的事。 */
            for (kk = 0; kk < 4; kk++)
                key[40 + kk] = s_hires[4 + kk];
            /* 【r152】取指基址探针整块进键: 探针的任何一个字节变了
             *   (nb/nt/nx 变了、fixN 涨了、写进去的 start 变了)都是本轮
             *   要看见的事, 所以它必须能把"变化才落盘"这道闸门推开。 */
            for (kk = 0; kk < VBE_TR_PR_LEN; kk++)
                key[44 + kk] = s_probe[kk];
            /* --- 【r147】每个边沿无条件摘要 ---------------------------
             *   ./146 的教训: 整块落盘挂在"与上一拍不同"上, 于是"计数器
             *   在长、指纹不变"的重复会话被全部跳过 —— 13 次切屏只剩 4 行
             *   证据, 而那 4 行值完全一样, 看不出会话之间的进展。这一组
             *   PH 行在"变化才写"的闸门【之外】, 每个边沿都写一份。
             *   它是判读本轮的入口: fixN 长得起来 = 补写真的在动手;
             *   p35 那几行 = 盒子持屏期我们的六个寄存器被修成了什么。 */
            /* 【r151】屏幕所有权 + 特殊 VM + 35 号的 AL ------------------
             *   【为什么放在这里】这一组与下面那一组一样落在"变化才落盘"
             *   的闸门【之外】。r150 的教训(同一个坑第 9 次): 把读数放在
             *   策略闸门/变化闸门后面, 它就永远读不出东西 —— 仪器不能
             *   挂在"事后状态"上, 也不能挂在"还没开的行为"上。
             *   判读:
             *     crtccls/memccls: 全屏盒子持屏时应当是 BOX(2);
             *       窗口化盒子时 DDK 说 CRTC 属 Windows、MemC 属那个 DOS VM
             *       ⇒ 预期 crtccls=1(Windows) 而 memccls=2(盒子)。
             *       如果全屏时 crtccls 仍是 1, 那"屏幕归谁"的记账就没翻过来
             *       —— 那正是"画面按 Windows 的 hi-res 解释"的成因。
             *     p35al: 02h/03h = 主 VDD 做了 DBCS 折算; 72h/73h = 【没有】
             *       (见 vbeshare.h 长注 7 —— 72h > 13h, 一个跑文本的盒子会
             *        被主 VDD 判成高分辨率 VM)。
             *     pairn: (CRTC,MemC) 分类对变化的次数 —— 一场会话里它应该
             *       动几次; 恒 0 就说明 9 号那两个句柄从来没变过。        */
            PHASEV( "PH: own crtccls=",  (uint16_t)s_own[VBE_TR_OWN_CRTCCLS] );
            PHASEV( "PH: own memccls=",  (uint16_t)s_own[VBE_TR_OWN_MEMCCLS] );
            PHASEV( "PH: own p35al=",    (uint16_t)s_own[VBE_TR_OWN_P35AL] );
            PHASEV( "PH: own p35mask=",  (uint16_t)s_own[VBE_TR_OWN_P35MASK] );
            PHASEV( "PH: own pairn=",    (uint16_t)s_own[VBE_TR_OWN_PAIRN] );
            PHASEV( "PH: own savecls=",  (uint16_t)s_own[VBE_TR_OWN_SAVECLS] );
            PHASEV( "PH: own lastpair=", (uint16_t)s_own[VBE_TR_OWN_LASTPAIR] );
            /* 【r152】持屏期显存映射选择变化次数 + 最近值。
             *   一次会话里 mapchg 恒 0 ⇒ "4 平面孔径状态"只在进场那一刻
             *   被摆过一次(r150 的判读成立); 不为 0 ⇒ 会话中途有东西反复
             *   重新施加它, 而我们每 4 拍补写一次就是在和它打架。      */
            PHASEV( "PH: own mapchg=",   (uint16_t)s_own[VBE_TR_OWN_MAPCHG] );
            PHASEV( "PH: own lastmap=",  (uint16_t)s_own[VBE_TR_OWN_LASTMAP] );
            /* 【r175】R0 只读取证(两行, 都在"变化才落盘"的闸门之外):
             *   bda449 = 35 号本场第一拍读到的 BIOS 数据区 0:449;
             *            0FFh = 本场还没采到(4 号刚立的哨兵);
             *            03h  = 盒子认为自己在 mode 3;
             *            072h = 盒子认为自己在 72h(> 13h)。
             *   p35vm  = 1 = 35 号跑在盒子 VM 里 / 2 = 跑在 Windows VM 里 /
             *            3 = 两种都见过 / 0 = 都不是(特殊 VM)。
             *   【为什么必须跟在这里】r150/r152 的教训: 读数一旦挂在
             *   策略闸门或变化闸门后面就永远读不出东西。 */
            PHASEV( "PH: own bda449=",  (uint16_t)s_own[VBE_TR_OWN_BDA449] );
            PHASEV( "PH: own p35vm=",
                    (uint16_t)((s_own[VBE_TR_OWN_P35MASK] >> 4) & 3) );
            /* --- 【r152】取指基址探针(无条件组) -------------------------
             *   判读表见 vbeshare.h 的 VBE_TR_Probe 长注。
             *   【r182】NB/NT/NX/NA/CELLS 这五个查位由全屏文本页那条路写,
             *   r152 的四个"打印格数"落到了 VxD 的 旧的逐格读数暂存(不再进 blob)。
             *   从 r182 起的含义是:
             *     nbnt = (进门时字符字节的【种类数】) << 8 | (进门时"规范格"
             *            数 / 8), 规范格 = 字符 20h 且属性 07h, 也就是一次
             *            BIOS mode 3 清页之后整页该有的格子;
             *     nxna = (清页后字模指纹的低字节) << 8 | (清页后仍不规范的
             *            格数 / 8)。
             *   判读(必须连着 cells 那一位图读):
             *     nbnt 低字节 = 0FAh(250) ⇒ 进门那一页【整页都是空格】, 说明
             *       上一次清页之后没有任何人写过它 —— 盒子自己不画页面;
             *     nbnt 低字节 = 00 且高字节小 ⇒ 进门那一页是一页真文本;
             *     nbnt 高字节 > 0C0h(192) ⇒ 一页里出现过近两百种字符字节,
             *       那是像素残留的标准长相(16bpp 像素按 (字符,属性) 读出来),
             *       因为 DOS 文本屏的字符种类数通常只有几十;
             *     nxna 低字节 = 00 ⇒ 清页那一笔真的写进去了(读回自证)。
             *   打包是为了让这一组只占 11 行而信息一个不少:
             *     nbnt = nb<<8 | nt          nxna = nx<<8 | na
             *     start= cr0d<<8 | cr0c      fix  = fixN<<8 | fixOk
             *     fixb = fixBad<<8 | latch   cand = cand<<8 | nxa
             *     new  = newh<<8 | newl      rawb / rawt = 各头两个字节 */
            PHASEV( "PH: probe nbnt=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_NB] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_NT]) );
            PHASEV( "PH: probe nxna=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_NX] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_NA]) );
            PHASEV( "PH: probe start=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_CR0D] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_CR0C]) );
            PHASEV( "PH: probe fix=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_FIXN] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_FIXOK]) );
            PHASEV( "PH: probe fixb=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_FIXBAD] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_LATCH]) );
            /* 【r160】安全网读数: 我们自己写错的起点被收回来的次数。
             *   判读: 恒 0 = r160 那三道新闸门够用(期望值);
             *        > 0 = 还有一笔错的写通过了闸门, 但画面在同一拍被收回
             *              —— 此时要连着看 start/new/agree 三个读数。
             *   它与 fixb 的高字节(FIXBAD)含义不同, 见 vbeshare.h 的长注。 */
            /* 【r161】MemC 半边搬到官方事件(9 号)之后的四个读数。
             *   判读(必须连着读):
             *     s9n   = 本场 9 号被调次数。恒 0 ⇒ 这条钩子根本没被主 VDD
             *             调用, "挂在官方事件上"这个前提是假的 —— 那时要回头
             *             看 35 号那条回落路(读数在下面的 phz 段里)。
             *     s9ok  = 其中"五个不碰 ATC 的字节本来就与文本目标逐个相等"
             *             的次数(主 VDD 自己恢复对了 ⇒ 我们一个字节都不写)。
             *     s9fix = 因为不等而真的补写的次数(累计)。> 0 且画面同时变正常
             *             = 本轮这一枪成立; 恒 0 而画面照样花 ⇒ 判据没放行,
             *             下一轮要问的就是"六个字节不等时它为什么不写"。
             *     s9bad = 补写之后当拍读回仍不成立的次数(累计, 期望 0)。
             *   另一条同样关键的读数在 phz 段: `l35sr04 / l35gcr06` 应当从
             *   06 / 04 变成 02 / 0E —— 那正是 1602.png 那种花屏消失的指纹。 */
            PHASEV( "PH: probe s9n=",   (uint16_t)s_probe[VBE_TR_PR_S9N] );
            PHASEV( "PH: probe s9ok=",  (uint16_t)s_probe[VBE_TR_PR_S9OK] );
            /* 【r161】所有者核对的成败: s9same > 0 且 s9fix > 0 = 官方事件这条路
             *   真的在补; s9same 恒 0 而 s9diff 在涨 = 9 号给出的 MemC 所有者
             *   不是 4 号那个全屏 VM, "挂在官方事件上"在本机走不通 —— 那时是
             *   35 号那条回落层(节流已撤)在扛, 下一轮要查的是句柄本身。 */
            PHASEV( "PH: probe s9same=", (uint16_t)s_probe[VBE_TR_PR_S9SAME] );
            PHASEV( "PH: probe s9diff=", (uint16_t)s_probe[VBE_TR_PR_S9DIFF] );
            /* r166: 9 号里被"CRTC 正在显示标准 mode 3 文本"判据挡下的次数。
             * 判读: 它应当【增长】(闸门在真判断), 而 s9fix/s9ok 至少有一个
             * 也在增长; 若它恒为 0 而 s9fix 也恒为 0, 说明 9 号那条路没跑到
             * 判据上(那就是另一个故障, 不许读成"修好了")。 */
            PHASEV( "PH: probe s9geof=", (uint16_t)s_probe[VBE_TR_PR_S9GEOF] );
            /* 【r162】官方那一拍(4/5 号)的"标准 mode 3 集合差在哪"三个读数。
             *   判读表(一次开机就能出结论, 见 vbeshare.h 的 r162 长注):
             *     sr01val == 00  ⇒ 字符时钟本来就是标准值(9 点), 我们的补写
             *                      一个字节都没发生 ⇒ 时钟不是灰屏的病根,
             *                      下一轮照 geomask 走;
             *     sr01val != 00  ⇒ 那一次切屏的【整组设模式没有落到硬件上】
             *                      (BIOS/主 VDD 的设模式被跳过了) —— 我们已经
             *                      把这一格写成 00h, 画面应当在下一拍就恢复;
             *     geomask == 00  ⇒ 720x400 的几何六字节都对(设模式生效);
             *     geomask != 00  ⇒ 几何不是 720x400 文本(位序: bit0 CR00
             *                      bit1 CR01 bit2 CR12 bit3 CR13 bit4 CR15
             *                      bit5 CR17) ⇒ 下一轮按标准表连 CR11 bit7
             *                      的关/开序列一起写(本轮只读, 不做)。
             *   sr01bad 是"这一格被补过几次"的累计值(4 号不清)。 */
            PHASEV( "PH: probe geomask=", (uint16_t)s_probe[VBE_TR_PR_GEOMASK] );
            /* 【r163】15 号(MAKE_HARDWARE_NOT_BUSY) —— 官方文档里唯一一处把
             *   "MemC 状态正从 Windows hi-res 切到 VGA 平面态"写成事件的地方,
             *   而且一次交出三个所有者句柄:
             *     EAX = CRTC 所有者  EBX = 当前 VM  ECX = MemC 所有者。
             *   判读表:
             *     n15   = 15 号被调次数(模 256)。恒 0 ⇒ 这个钩子在本机根本不
             *             响, "按所有者分状态"那条路自动作废, 回到 35 号采样。
             *     own15 = (最近一次 << 8) | (武装期内那一次), 位序:
             *               bit0 EAX==WinVM   bit1 ECX==WinVM   bit2 EBX==WinVM
             *               bit3 EAX==盒子VM  bit4 ECX==盒子VM  bit5 EBX==盒子VM
             *             低字节(武装期内)出现 bit3/bit4 ⇒ 主 VDD 在盒子持屏
             *             期间确实把 CRTC/MemC 交给盒子, 那条路可以走;
             *             只有 bit0/bit1/bit2 ⇒ 它在为 Windows 摆 MemC 状态,
             *             也就是 ./162 里我们一直在撞的那个"别人的状态"。
             *     yield = 1 ⇒ 本场补写被就地扳回、已经认输(不再写硬件);
             *             它应当与 fixedN 停止增长同时出现。 */
            PHASEV( "PH: probe n15=", (uint16_t)s_probe[VBE_TR_PR_N15] );
            PHASEV( "PH: probe own15=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_OWN15] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_OWN15A]) );
            /* r163-marker: MAKE_HARDWARE_NOT_BUSY owners are printed. */
            PHASEV( "PH: probe cand=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_CAND] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_NXA]) );
            PHASEV( "PH: probe new=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_NEWH] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_NEWL]) );
            /* 【r182】cells = 全屏文本页那一条路的判定位图, 八位全用:
             *   bit0 = 本拍把整页清成了 (20h,07h)。判据是 VgaPageJudge 的两条:
             *          "规范格(字符 20h 且属性 07h) >= 1000/2000" 且 "属性种类
             *          <= 48"; 两条不成立(桌面像素残留就是这种)⇒ 清页, 因为一次
             *          文本模式设置在硬件上的语义就是"这一屏从空格开始"。
             *          【为什么没有"字符种类"那一条】中文 Win98 的 DBCS 屏上
             *          字符种类 routinely 过 180, 拿它当闸门会在正确页面上开火;
             *   bit1 = 判据说"这一页已经是真文本", 于是【没有】清页 —— 清它会
             *          冲掉别人的成果;
             *   bit2 = 4000 字节快照已取(字模拷贝之后会原样写回);
             *   bit3 = 清页后读回自证成立(2000 格全部是 (20h,07h)) —— 缺这一位
             *          就是"写进去了但没读到", 那说明读路径和写路径不是同一个
             *          地方, 下一轮该查取指一侧而不是页面;
             *   bit4 = 进门那一页本来就已经全规范(2000 格);
             *   bit5 = 进门那一页过半规范(>= 1000 格);
             *   bit6 = 清页之后字模指纹仍等于 FontRef ⇒ 这一笔没有伤到平面 2;
             *   bit7 = 进门之后这一页被改过(与 4000 字节快照逐字节不同)。改它
             *          的【可能是我们自己的 8KB 字模拷贝】, 所以这一位不能读成
             *          "别人写的" —— 它是"判据吃现场还是吃进门读数"的开关, 也是
             *          下一轮查"到底是谁在写页面"的入口。 */
            /* --- 【r153】新的取证: 等式 + 铺开采样 + 扫描 + 颜色管线 -----
             *   判读表(一次开机就能读出结论):
             *     agree=0001  ⇒ 取指就是 CPU 写的地方, 不需要修(正常态);
             *     agree=0000  ⇒ 两者不同。此时看 nbf/nbv:
             *        nbv>0 且 nbf=0000 ⇒ 盒子的字在 B8000 那边而不在取指处
             *         ⇒ 挪起点(value=off/2, off/4, off 依次自证)就是对症;
             *     agree=0002  ⇒ 取指地址不在当前映射窗口内, 读不到, 不猜。
             *   topidx/topval = 在场(可能不同)的映射下, 显存里"最像盒子屏幕"
             *   的那一段: 偏移 = topidx * 2000h, topval = 该处的非空格数。
             *   若 topidx 指向 4(=8000h)而且 topval 大, 说明盒子的文本页在
             *   显存偏移 8000h —— 那正是"映射 01b 时 CPU 的 B8000"。
             *   ar* = 属性调色板 16 个(文本模式的标准值是
             *   00 01 02 03 04 05 14 07 38 39 3A 3B 3C 3D 3E 3F);
             *   dac* = RAMDAC 条目 0/7/15(标准值 000000 / 2A2A2A / 3F3F3F)。
             *   这两个读数合起来回答 ./152 的灰屏: 全屏纯浅灰(#AAAAAA)
             *   必然是"背景色 0 被调成了 DAC 里的 2Ah 灰"或"属性调色板
             *   错位", 而内存里明明是 20 07 的空屏。 */
            /* 【r153 追加】cover = 当前映射解不解码主机 B8000(00b/11b 才行),
             *   nocov = "不解码"被探针看到过多少次。
             *   【为什么这一对是判决性的】FreeVGA 的显存映射选择决定的是
             *   "VGA 解码哪一段主机地址": 01b = A0000-AFFFF(64K)、
             *   10b = B0000-B7FFF(32K) —— 这两种下 **B8000 根本不在解码范围
             *   里**, 盒子写 B8000 的字节压根没进显存。于是:
             *     cover=0  ⇒ r153 的"挪 CR0C/CR0D"修复在物理上不可能成立
             *                (探针会报 agree=3 并拒绝开火, 不再白写三次);
             *                能救这种状态的只有把映射换成 00b/11b ——
             *                也就是 MemCFix=1 那一档(六字节补写里的
             *                GCR06=0Eh), r153 已修好它的 AR 相位。
             *     cover=1  ⇒ 走正常的等式判据(agree/nbf/nbv/top)。 */
            PHASEV( "PH: probe tg=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_TGONCE] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_TGTRIED]) );
            PHASEV( "PH: probe tgok=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_TGOK] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_TGBAD]) );
            /* 【r155 追加】默认档那一笔的"预算 / 去抖 / 存活"三个读数。
             *   tgtry  = (TGWATCH<<8)|TGTRY: 本场用掉几笔(预算 8) + 是否
             *            正在观察最后那一笔的存活;
             *   tgrun  = (TGRUNMAX<<8)|TGRUN: 当前连续坏拍 + 本场最长连续
             *            坏拍 —— 后者是"连续两拍才补第二笔"这条去抖到底
             *            挡掉了多少东西"的直接证据(恒为 1 ⇒ 去抖要撤掉);
             *   tgrev  = 上一笔补写活过了几个坏拍才被扳回去(0 = 一直活到
             *            会话结束, 也就是"写站得住、花屏另有原因")。
             *   判读表见 R155_DELIVERY.md 第 2 节(r156 未改这一段)。 */
            PHASEV( "PH: probe tgtry=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_TGWATCH] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_TGTRY]) );
            PHASEV( "PH: probe tgrun=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_TGRUNMAX] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_TGRUN]) );
            /* 【r155 追加】A0000 窗口的 8 个字节在这一拍动过没有。
             *   它回答的是本轮唯一还没有答案的问题: 全屏期间盒子的文本写
             *   落在 B8000(被映射挡掉 ⇒ 画面停住)还是落在 A0000(写得进
             *   去、但被奇偶/平面配置搅乱 ⇒ 画面是花屏)。两者正确修法
             *   不同。tgaa = (TGAACHG<<8)|TGAASAME。 */
            PHASEV( "PH: probe tgaa=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_TGAACHG] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_TGAASAME]) );
            /* 【r156 追加】DAC 读索引自证。
             *   r153..r155 的 单个 DAC 项读取 把条目号写进 3C8h(DAC【写】索引)
             *   再读 3C9h, 而 3C9h 的读走的是 3C7h 设定的【读】索引 ——
             *   ./154 的 `dac7 = 2A 2A 00` 正是标准 16 色表里【条目 6】
             *   (棕色)的值, 也就是"错位一条"的指纹。
             *   修正(改写 3C7h)之后, 本过程在三个条目读完后【再读一次
             *   3C7h】—— 依文档它是状态寄存器: "bit 0-1: 0 indicates the
             *   DAC is in Write Mode and 3 indicates Read mode"(见
             *   minivdd.asm 的 单个 DAC 项读取 长注)。判读:
             *     3  = 读模式 ⇒ 本轮的颜色读数是可信的;
             *     0  = 写模式 ⇒ 还在用写索引选条目(或该位被实现成 0);
             *     其它(常见 FF) = 该寄存器未实现, 这条读数无结论。
             *   在 dacst 不是 3 的那份日志里, 不要去解释 dac0/dac7/dac15。 */
            /* 【r157 追加】属性控制器的原始读数 + 逐寄存器写入计数。
             *   watc  = (ATCRAW<<8)|W_CR17。ATCRAW 是从 3C0h 读回的字节:
             *           bit5(PAS)=1 ⇒ r133 那套"读回原索引再保留 PAS"的旧
             *           写法无害; bit5=0(例如 00h/10h) ⇒ 旧写法【每次都在
             *           关视频】—— 那就是 ./148/./152 花屏与灰屏的直接来源。
             *   wsr   = (SR01 写次数<<8)|SR02 写次数
             *   wsr2  = (SR03<<8)|SR04
             *   wgcr  = (GCR05<<8)|GCR06
             *   wcrt  = (CR0C<<8)|CR0D
             *   口径(必须连着读): 数的是【被路由到我们这里】的端口写。
             *   全屏会话里主 VDD 也可能把 VM 的写直接放行, 那时计数不动 ——
             *   所以"计数不动"是弱证据, "计数增长"是强证据(如果 GCR06 的
             *   计数在会话期间涨了, 那就是 VM 自己在写那一组寄存器)。 */
            PHASEV( "PH: probe watc=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_ATCRAW] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_W_CR17]) );
            PHASEV( "PH: probe wsr=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_W_SR01] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_W_SR02]) );
            PHASEV( "PH: probe wsr2=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_W_SR03] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_W_SR04]) );
            PHASEV( "PH: probe wgcr=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_W_GCR05] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_W_GCR06]) );
            PHASEV( "PH: probe wcrt=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_W_CR0C] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_W_CR0D]) );
            /* 【r158 追加】"盒子上一次交回屏幕时, 哪几个字节和我们存的不一样"。
             *   ./154 只落了 `TRC hires lastDiff=0001` —— 一个数字, 说不出
             *   是哪一个字节, 于是永远无法据以行动。现在同一个比较动作把
             *   "哪一位漂了"也写进 blob, 这里按位图打印(位序见 vbeshare.h):
             *     bit0 SR04  bit1 GCR06  bit2 CR13  bit3 CR17  bit4 SR01
             *     bit5 SR02  bit6 SR03  bit7 GCR03 bit8 GCR04 bit9 GCR05
             *     bit10 CR14
             *   其中 SR02/SR04/GCR05/GCR06 正是 MemC 那半边 ⇒ 若漂的是它们,
             *   那就是"盒子把 MemC 状态改了却没还回来"的直接证据。 */
            PHASEV( "PH: probe hdiff=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_HDIFFH] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_HDIFFL]) );
            PHASEV( "PH: probe cover=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_COVER] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_NOCOVER]) );
            PHASEV( "PH: probe nbf=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_NBF] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_NBV]) );
            PHASEV( "PH: probe varf=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_VARF] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_VARV]) );
            PHASEV( "PH: probe top=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_TOPOFF] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_TOPVAL]) );
            PHASEV( "PH: probe scan=",   (uint16_t)s_probe[VBE_TR_PR_SCANN] );
            PHASEV( "PH: probe boff=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_BOFFH] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_BOFFL]) );
            PHASEV( "PH: probe fetch=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_FETH] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_FETL]) );
            /* 【r412】调色板仪器(替掉从来不可信的三格 DAC 条目读数)。 */
            PHASEV( "PH: r412 swinst=",  s_swInst );      /* 5A5A=装成, 1111=装失败 */
            PHASEV( "PH: r412 swold=",   s_swOldOff );    /* 旧向量偏移: 证明读到 */
            PHASEV( "PH: r412 swoldseg=", s_swOldSeg );
            PHASEV( "PH: r412 swnew=",   s_swNewOff );
            PHASEV( "PH: r412 sw4001=", s_sw4001 );
            PHASEV( "PH: r412 sw4002=", s_sw4002 );
            PHASEV( "PH: r412 p5sum=",    (uint16_t)s_probe[VBE_TR_PR_I10N] );
            PHASEV( "PH: r412 pal_st=",  (uint16_t)s_probe[VBE_TR_PR_PAL_ST] );
            PHASEV( "PH: r412 p15sum=",   (uint16_t)s_probe[VBE_TR_PR_I10M0] );
            PHASEV( "PH: r412 p5xor=",  (uint16_t)s_probe[VBE_TR_PR_I10FIX] );
            PHASEV( "PH: r412 i10hnd=",  (uint16_t)0x024C );
            /* 【r412】"完美模拟 mode co80" 的可行性探针(见 minivdd.asm 的 M2Probe 头注)。
             *   判读: st b2 = 15 号这一拍当前 VM 就是盒子; b4 = 嵌套 Exec_Int 改写了 AX;
             *   b5 = AL 是 03h/72h(真实模式号)。三者同时成立 ⇒ r260 可直接以盒子身份发
             *   INT 10h AX=0003。cur/own 是两边的分类码(1=Windows 2=盒子 3=其它)。 */
            PHASEV( "PH: probe fntreg=",
                    (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_AR10] << 8) |
                                (uint16_t)s_probe[VBE_TR_PR_AR10 + 1]) );
            /* 【r412】fontb 读的是【真的】计数器了。r412 这一行读的是
             *   Probe[AR10+2..3], 而 VgaBiosFont 把"子功能号 / BIOS 回来的 AL"
             *   写在同两格 ⇒ ./185 印出 `fontb=1414`, 那是 "AH=11h AL=14h"
             *   (8x16 ROM 字模那一枪), 不是字数。本轮改成从 MemC 的
             *   VBE_TR_MC_FONTBADG 取低 6 位 —— 那是 VfcLoad 收尾时落的同一个
             *   计数(高 2 位是 BIOS 重设 mode 3 的状态)。
             *   含义: 字符 41h 的 16 个字节在 base+0820h 上与源字模不同的【字节】
             *   数(0..16)。0 = 逐字节相同。
             *   【本机读不到真值】./185 的 `fontp0 == fontfp == 1818` 证明这张卡
             *   忽略 GCR04 的读平面选择, 所以本计数器【没有】读平面 2 的能力;
             *   它只回答"同一扇窗里读回来的是不是我们写的那 16 个字节"。
             *   本轮不再用它做判据(见 minivdd.asm 的 VfcGlyphCmp 与
             *   VgaFontProv), 只当观察值。 */
            PHASEV( "PH: probe fontb=",
                    (uint16_t)(s_memc[VBE_TR_MC_FONTBADG] & 0x3Fu) );
            /* 【r412】两行新仪器(占用 AR10+2 / AR10+3 两格, 它们原来是
             *   VgaBiosFont 的"子功能号 / BIOS 回来的 AL")。
             *   fontsr03 = 写平面 2 那一拍读回来的 SR03(字符映射选择):
             *     位 1..0 = 字符集 B 的 8K 段号, 位 4..2 = 字符集 A 的段号。
             *     0 = 两套都落在 0 号段(我们写的就是 0 号段);
             *     非 0 ⇒ 字符发生器可能从别的段取字模 —— 那就是"写对了却看不见"
             *     的另一种解释, 下一轮按这个数决定要不要写别的段。
             *   fontpk = 打包(chain-4)视图的写读自证(见 minivdd.asm 的 VfcPacked):
             *     1 = SR02=0Fh / SR04=0Eh 那一组能把 A0000h+i 当成视频字节 i
             *     写并读回; 0 = 那扇窗的打包视图也不通。
             *   【判读】fontpk=1 而画面仍不变 ⇒ 取指既不是平面 2 也不是打包视图
             *     的 0 号段, 下一轮只剩 VBE 线性帧缓冲(LfbMap 的 linear 地址)。 */
            /* 【r178】两行新仪器(占用 AR00..AR03 四个已死的格子):
             *   fontp0  = 同一格在【平面 0】上的指纹(GCR04=00 读);
             *   fontexp = VfcLoad 从源字模算出的【应有】指纹。
             *   fontp0 与 fontfp 相同 ⇒ 读平面选择被忽略, 回读仪器瞎;
             *   fontfp 与 fontexp 相同 ⇒ 平面 2 里就是我们写进去的字模。 */
            {
                uint16_t aw0 = (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_AR00] << 8) |
                                           (uint16_t)s_probe[VBE_TR_PR_AR00 + 1]);
                uint16_t aw1 = (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_AR00 + 2] << 8) |
                                           (uint16_t)s_probe[VBE_TR_PR_AR00 + 3]);
                PHASEV( "PH: probe fontp0=", aw0 );
                PHASEV( "PH: probe fontexp=", aw1 );
            }
            /* 【r412】fontwin = 本版两档里, 哪一档【写进去并在同一扇窗读回来了】(见
             *   minivdd.asm 的 VfcCanary / VfcSetCand / VgaFontProv)。
             *     0 = 两档都过不了 canary + 基址闸门;
             *     1 = 04h / 07h / 0A0000h(GCR06 的 01b: A0000-AFFFF 64K) —— 默认;
             *     2 = 00h / 07h / 0A0000h(GCR06 的 00b: A0000-BFFFF 128K)。
             *   与 r173..r412 的根本区别: 那十三轮的档里, 默认档是
             *   0B8000h + GCR06=0Ch(B8000-BFFFF = 图形偏移 18000h), 另一头
             *   0B0000h + GCR06=08h(= 偏移 10000h); 按 VGA 图形控制器 GCR06 的
             *   Memory Map Select 定义, 【两档都写不到字模所在的 0 号 8K 段】。
             *   FreeVGA 的字体一节写明: 平面 2 分成八个 8K 段, 每字符 32 字节,
             *   字符 c 的格子在 c*32 —— 所以只有 A0000 的两档对得上。
             *   判读: fontwin=1 而画面仍花屏 ⇒ 字模段【确实】被我们写了, 病根
             *   在取指/时序一侧(CR13/CR17/CR0C/CR0D/SR01/AR10 或卡自己的
             *   扩展状态), 下一轮只查那边。 */
            PHASEV( "PH: probe fontwin=", (uint16_t)s_probe[VBE_TR_PR_FONTWIN] );
            /* 【r184】三个时刻的【活的】标准文本寄存器组。每组 7 字节:
             *   SR02, SR04, GCR04, GCR05, GCR06, CR0C, CR0D。
             *   rrX / rrX2 / rrX3 / rrX4 依次是
             *     SR02:SR04 / GCR04:GCR05 / GCR06:CR0C / CR0D:0
             *   读法(与 VgaTextGroup 写进去的值对照):
             *     文本态 = 03 / 02 || 00 / 10 || 0E / 00 || 00
             *   三个样本:
             *     r5in  = 5 号钩子进门(主 VDD 第 6 步之后、我们动手之前)
             *     r5out = 5 号钩子收尾(我们交给主 VDD 之前) + r5fix/r5ok
             *     r48   = 盒子自己的 INT 10h 里(主 VDD 收工、盒子正在持屏)
             *   判读:
             *     r5out 就已经不是文本态 ⇒ 我们没写进去(或写不进);
             *     r5out 是、r48 不是    ⇒ 我们在 5 号之后被谁扳回去了
             *                              (主 VDD 的 POST 收尾 / 盒子自己的 BIOS);
             *     两个都是文本态       ⇒ 寄存器这一层已经对了, 病根在取指/卡自己的
             *                              扩展状态, 下一轮只查那边。
             *   另: fontrun(r412 起这一格改义) = VgaFontProv 真的进了几次门。
             *       0 就说明这一轮又没测(闸门没过 / 没被调用), 一切字模读数无效。 */
            {
                struct { uint16_t off; const char *nm; } rg[3];
                uint8_t ri;
                rg[0].off = VBE_TR_PR_R5IN;  rg[0].nm = "PH: r412 r5in";
                rg[1].off = VBE_TR_PR_R5OUT; rg[1].nm = "PH: r412 r5out";
                rg[2].off = VBE_TR_PR_R48;   rg[2].nm = "PH: r412 r48";
                for (ri = 0; ri < 3; ri++)
                {
                    uint16_t o = rg[ri].off;
                    uint16_t w0 = (uint16_t)(((uint16_t)s_probe[o] << 8) | s_probe[o + 1]);
                    uint16_t w1 = (uint16_t)(((uint16_t)s_probe[o + 2] << 8) | s_probe[o + 3]);
                    uint16_t w2 = (uint16_t)(((uint16_t)s_probe[o + 4] << 8) | s_probe[o + 5]);
                    uint16_t w3 = (uint16_t)(s_probe[o + 6]);
                    PHASEV( rg[ri].nm, w0 );
                    PHASEV( "  +2", w1 );
                    PHASEV( "  +4", w2 );
                    PHASEV( "  +6", w3 );
                }
            }
            /* 【r412】4 号那一枪的硬件读数(见 minivdd.asm 的 TraceStub_4):
             *   crt  = 进门时的 (CR13<<8)|CR01 —— 此刻还是 hi-res, 所以它【不该】
             *          是标准 mode 3 的 28/4F;
             *   crt2 = 开完那一枪之后的同一对 —— 若这一枪真的让卡进了 mode 3,
             *          它会变成 28xx/4Fxx; 若它与 crt 完全相同, 说明 BIOS 又被
             *          主 VDD 的虚拟处理器吞了一次(与 ./197 的 bios3=1 同形)。
             *   判读:
             *     crt2 = 284F 并且画面变好 ⇒ 修好;
             *     crt2 = 284F 而画面仍花屏 ⇒ 卡是 mode 3、字模却没进平面 2 ⇒
             *        病根在"内存窗口/虚拟化"(下一轮查 ACCESS_VGA_MEMORY_MODE
             *        与窗口化虚拟化区), 不再查模式设置;
             *     crt2 == crt   ⇒ PRE 这一拍也被吞, 这条"让 BIOS 设模式"的路整条
             *        作废, 下一轮直接实现 ACCESS_VGA_MEMORY_MODE(11 号)。 */
            {
                uint16_t w0 = (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_R48] << 8)
                                         | s_probe[VBE_TR_PR_R48 + 1]);
                uint16_t w1 = (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_R48 + 2] << 8)
                                         | s_probe[VBE_TR_PR_R48 + 3]);
                PHASEV( "PH: r412 crt=",  w0 );
                PHASEV( "PH: r412 crt2=", w1 );
                /* 【r412】实心块实验的回读: 我们把字符 20h 的 16 字节写成
                 * FFh, 再从同一扇窗读回来。它只证明"这一笔落在我们这扇窗里";
                 * 是否落在【显示器取指那一块】要由屏幕回答(见交付说明)。*/
                /* 【r412】字模搬进 4 号 8K 段之后的两条读数:
                 *   fp   = 1 号段里字符 41h 的 +2/+6(应为 10C6, 与 fontexp 同一个源)
                 *   sr03 = 我们写进去的字符映射选择(应为 30 = A/B 都指 1 号段)
                 *   fw   = 写完之后 SR02 的回读(应为 04, 说明平面 2 独占的窗口还在)
                 *   判读: 屏幕变好 ⇒ 取指确实按 SR03 走, 病根就是"页写打穿平面 2";
                 *         屏幕不变 ⇒ 连字符映射选择也不起作用, 下一轮做 MemC(11 号)。*/
                {
                    uint16_t wfp = (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_R48] << 8)
                                              | s_probe[VBE_TR_PR_R48 + 1]);
                    PHASEV( "PH: r412 fp=", wfp );
                }
                /* 【r412】POST 最后一笔的六个"写路径"字节(见 VgaPostTail):
                 *   wpA = GCR00/GCR01(写前)  wpB = GCR02/GCR03  wpC = GCR08/SR02
                 *   wqA/wqB/wqC = 写完之后的同一组; wflag: bit0 写前已全对(没写)
                 *   bit1 写前不对(写了整组) bit2 图形态(一个字节没碰) bit3 写后全对
                 * 判读: 有 02h 且有 08h 且画面变好 ⇒ 写路径就是病根;
                 *       只有 01h ⇒ 写路径本来就对, 病根在"CPU 怎么看显存";
                 *       有 04h ⇒ 这一场不是文本屏, 本轮什么都没做。 */
                {
                    uint16_t wpA = (uint16_t)(((uint16_t)0 << 8)
                                              | 0);
                    uint16_t wpB = (uint16_t)(((uint16_t)0 << 8)
                                              | 0);
                    uint16_t wpC = (uint16_t)(((uint16_t)0 << 8)
                                              | 0);
                    uint16_t wqA = (uint16_t)(((uint16_t)0 << 8)
                                              | 0);
                    uint16_t wqB = (uint16_t)(((uint16_t)0 << 8)
                                              | 0);
                    uint16_t wqC = (uint16_t)(((uint16_t)0 << 8)
                                              | 0);
                    PHASEV( "PH: r412 wn=",    (uint16_t)0 );
                    PHASEV( "PH: r412 wflag=", (uint16_t)0 );
                    PHASEV( "PH: r412 wpA=", wpA );
                    PHASEV( "PH: r412 wpB=", wpB );
                    PHASEV( "PH: r412 wpC=", wpC );
                    PHASEV( "PH: r412 wqA=", wqA );
                    PHASEV( "PH: r412 wqB=", wqB );
                    PHASEV( "PH: r412 wqC=", wqC );
                                                            /* 【r412】字符集两个平面 2 基均与复制状态(槽位已修):
                     *   cs03 = 4 号进门那一刻的 SR03 原值;
                     *   csA / csB = 由它算出的 A / B 在平面 2 的基均(字节);
                     *   【r412】csA/csB 必须按小端 16 位读(上一版只读了一个字节:
                     *   8000h 会印成 0000), 而且它们不再被
                     *   老 字节块拷贝 仪器覆写(r412 把那两笔改到 DbgRaw*)。
                     *   csn = 本场真正执行了复制的次数;
                     *   csst bit0 还回去后相等 bit1 SR03!=0 bit3 复制过 bit4 复制后相等
                     *        bit5 A==B bit6 没存到 bit7 复制前不同。
                     *   屏幕第 24 行左侧印的就是 csA/csB。 */
                    PHASEV( "PH: r412 cs03=", (uint16_t)s_probe[VBE_TR_PR_CS03] );
                    PHASEV( "PH: r412 csst=", (uint16_t)s_probe[VBE_TR_PR_CSST] );
                    PHASEV( "PH: r412 csA=",
                            (uint16_t)((uint16_t)s_probe[VBE_TR_PR_CSA] |
                                        ((uint16_t)s_probe[VBE_TR_PR_CSA + 1] << 8)) );
                    PHASEV( "PH: r412 csB=",
                            (uint16_t)((uint16_t)s_probe[VBE_TR_PR_CSB] |
                                        ((uint16_t)s_probe[VBE_TR_PR_CSB + 1] << 8)) );
                    PHASEV( "PH: r412 bs3st=",  (uint16_t)s_probe[VBE_TR_PR_BS3ST] );
                    PHASEV( "PH: r412 n11=", (uint16_t)s_probe[VBE_TR_PR_N11] );
                    PHASEV( "PH: r412 m11=", (uint16_t)s_probe[VBE_TR_PR_M11] );
                    PHASEV( "PH: r412 g11=", (uint16_t)s_probe[VBE_TR_PR_G11] );
                    PHASEV( "PH: r412 s11=", (uint16_t)s_probe[VBE_TR_PR_S11] );
                    PHASEV( "PH: r412 t11=", (uint16_t)s_probe[VBE_TR_PR_T11] );
                    PHASEV( "PH: r412 n12=", (uint16_t)s_probe[VBE_TR_PR_N12] );
                    PHASEV( "PH: r412 g12=", (uint16_t)s_probe[VBE_TR_PR_G12] );
                    PHASEV( "PH: r412 s12=", (uint16_t)s_probe[VBE_TR_PR_S12] );
                    PHASEV( "PH: r412 t12=", (uint16_t)s_probe[VBE_TR_PR_T12] );
                    PHASEV( "PH: r412 vsm=",
                            (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_VSMX] << 8) |
                                        (uint16_t)s_probe[VBE_TR_PR_VSML]) );
                    PHASEV( "PH: r412 vsmh=", (uint16_t)s_probe[VBE_TR_PR_VSMH] );
                }
            }
            PHASEV( "PH: phz edge n35=",      (uint16_t)fn35 );
            PHASEV( "PH: phz edge seenText=", (uint16_t)s_memc[VBE_TR_MC_SEENTEXT] );
            PHASEV( "PH: phz edge fixN=",     (uint16_t)s_memc[VBE_TR_MC_FIXED] );
            PHASEV( "PH: phz edge stillBad=", (uint16_t)s_memc[VBE_TR_MC_STILLBAD] );
            PHASEV( "PH: phz edge gfx=",      (uint16_t)s_memc[VBE_TR_MC_GFX] );
            PHASEV( "PH: phz edge dirty=",    (uint16_t)s_memc[VBE_TR_MC_DIRTY] );
            PHASEV( "PH: phz p4sr04=",
                    (uint16_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SR04] );
            PHASEV( "PH: phz p4gcr06=",
                    (uint16_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_GCR06] );
            PHASEV( "PH: phz p4sr01=",
                    (uint16_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SR01] );
            PHASEV( "PH: phz p4src=",
                    (uint16_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SRC] );
            PHASEV( "PH: phz p5src=",
                    (uint16_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SRC] );
            PHASEV( "PH: phz p5gcr06=",
                    (uint16_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_GCR06] );
            PHASEV( "PH: phz p5sr04=",
                    (uint16_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SR04] );
            /* 【r162】5 号那一拍(主 VDD 刚做完 INT 10h 设模式、我们刚补完
             *   时钟那一格)的 SR01 与 AR10 —— 这一对是"设模式到底落没落到
             *   硬件上"的现场证据, 而且它是【已有的采样】, 不需要新仪器:
             *     p5sr01 应当是 00h(9 点文本时钟), p5ar10 应当是 0Ch。
             *   若 p5sr01 仍是 01h/03h, 说明我们的补写没落地; 若它已经是
             *   00h 而 sr01val 非 0, 说明补写生效(两者互为对照)。 */
            PHASEV( "PH: phz p5sr01=",
                    (uint16_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SR01] );
            PHASEV( "PH: phz p5ar10=",
                    (uint16_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_AR10] );
            PHASEV( "PH: phz p7src=",
                    (uint16_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SRC] );
            PHASEV( "PH: phz p7sr01=",
                    (uint16_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SR01] );
            PHASEV( "PH: phz p7gcr06=",
                    (uint16_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_GCR06] );
            PHASEV( "PH: phz f35src=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SRC] );
            PHASEV( "PH: phz f35sr04=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SR04] );
            PHASEV( "PH: phz f35gcr06=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_GCR06] );
            PHASEV( "PH: phz f35gcr05=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_GCR05] );
            PHASEV( "PH: phz f35sr02=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SR02] );
            PHASEV( "PH: phz f35ar10=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_AR10] );
            /* 【r149】取指基址(CR0C)。判读: 0 = 基址 B8000, 与盒子写字符的
             *   地方一致; 非 0 = 扫描从别处读 —— 见 minivdd.asm FboxSnap。 */
            PHASEV( "PH: phz f35cr0c=",
                    (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_CR0C] );
            PHASEV( "PH: phz l35src=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SRC] );
            PHASEV( "PH: phz l35gcr06=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_GCR06] );
            /* 【r151】P35 槽是【活的那一拍】(持屏期最近一次 35 号采样),
             *   与 P35FIRST(本场第一拍)配对。r150 只导出了它的 GCR06 ——
             *   于是"盒子持屏期另外五个字节长什么样"只能靠 MemCFix 那条
             *   被闸门挡住的路去猜(见 vbeshare.h 长注 4)。这里补齐。 */
            PHASEV( "PH: phz l35sr01=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR01] );
            PHASEV( "PH: phz l35sr02=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR02] );
            PHASEV( "PH: phz l35sr03=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR03] );
            PHASEV( "PH: phz l35sr04=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR04] );
            PHASEV( "PH: phz l35gcr05=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_GCR05] );
            PHASEV( "PH: phz l35ar10=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_AR10] );
            /* 【r152】持屏期"活的那一拍"的显示起始地址低位。
             *   它是本轮修复的目标寄存器, 也是判读修复有没有落地的
             *   直接读数(与 probe start 互相印证)。                */
            PHASEV( "PH: phz l35cr0c=",
                    (uint16_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_CR0C] );
            if (s_fboxHave)
            {
                for (kk = 0; kk < VBE9X_FBOX_KEYLEN; kk++)
                    if (key[kk] != s_fboxLast[kk]) { same = 0; break; }
            }
            else
                same = 0;
            if (same)
            {
                FlushLog();                     /* 摘要行也要落盘 */
                goto fbox_dump_done;            /* 与上一拍逐字节相同: 不写块 */
            }
            s_fboxHave = 1;
            for (kk = 0; kk < VBE9X_FBOX_KEYLEN; kk++)
                s_fboxLast[kk] = key[kk];

            /* 【r146】构建自证的第二条通路。./142、./143、./145 三次开机相
             *   的前几行都被日志闸门吞掉过(日志里没有 "VBE9X build tag" 与
             *   "PH: build tag=", 而二进制里三个串都在, 且之后 PH 通道正常),
             *   于是"这份日志到底是哪一版"不能只挂在 DrvInitCore 那三行上。
             *   状态边沿这一块是【已经证明能到盘】的通路(./142 起每轮都在),
             *   所以把同一条断言再写一遍 —— 值与 display.c 的 0x01F5 相同。 */
            PHASEV( "PH: fbox build tag=", 0x0279 );
            /* 【r171】prem3 = 4 号(PRE_HIRES_TO_VGA)那一枪的结果位图。r170
             *   在这一格上印的是 48 号改写计数(已撤), 本轮换了含义 —— 同一
             *   格两种含义是本项目踩过四次的坑, 所以标签一起换掉, 旧标签
             *   `i10map` 全文件不再出现。
             *      bit0..2 = 状态: 0 没跑 / 1 执行且 BIOS 回 AH=0 /
             *                      2 IVT[10h] 为 0(没敢执行) /
             *                      3 执行了但 AH!=0 /
             *                      4 盒子的模式不是 80 列文本(不动手)
             *      bit3    = 盒子自己的模式是 72h(远东 DBCS 文本)
             *      bit4    = 73h
             *      bit5    = 02h/03h/07h(VGA 文本)
             *      bit6    = 嵌套执行时的当前 VM == 4 号入口的 EBX(盒子)
             *   判读全表见 R171_DELIVERY.md 第三节; 一句话:
             *     0x09 = 盒子在 72h 里, 我们把它换成了 mode 3, 而且 BIOS 认了
             *            —— 这是本轮假说(模式记账不一致才是花屏的病根)的物证;
             *     0x04 = 盒子在图形模式里, 我们没动手(该程序本来就不该换); */
            /* 【r170】本轮新增的两个读数。它们必须落在这条【已经证明能到盘】
             *   的通路里: ./169 把 `PH: r169 Int10Fix armed=` 写在 DrvInitCore
             *   开头, 那份日志里它一个字都没有(那一刻日志通道还没开), 于是
             *   "这个开关到底拨到哪一档"整轮都无从判断。教训当场用上。
             *   ① int10fix = drv.c 真正推给 VxD 的那一档(0/1), 不是 INI 原文;
             *   ② bios3    = 开场那一枪(BIOS 状态回放, 嵌套 V86 INT 10h AX=0003)
             *      的结果, 高 2 位来自 blob 第 15 字节(VBE_TR_MC_FONTBADG):
             *        0 = 本场没开火(策略关 / 没武装 / 本场预算已用)
             *        1 = 开火了, BIOS 回 AH=0
             *        2 = IVT[10h] 是 0, 没敢执行(记 2 后放弃, 不会往 0:0 跳)
             *        3 = 开火了, BIOS 回 AH!=0
             *      判读: bios3=1 而画面仍花屏 ⇒ "让 BIOS 重设一次"这一手不足,
             *      病灶在 VDD 与盒子之间(模式记账 / BDA 0:449); bios3=0 而
             *      int10fix=1 ⇒ 5 号没把枪开出去(看 n35/arm 与该场是否真切过)。
             *      【./170 已经把它判成"不足"】bios3=1 而全屏照样花屏, 于是
             *      r171 把同一枪挪到 4 号(第 5 步存 BDA 之前), 见 prem3。 */
            /* 【r165】字模守卫的三个读数(判读表见包内 PACKAGE_MANIFEST 第三节)。
             *   【为什么这两条 r151 的标签必须在这里改掉】r164 换了字段却漏了标签:
             *   这两行印的已经是 s_hires[FONTN/FONTBAD], 而标签还写着
             *   "n48 72/73 total / while armed" —— ./164 的日志自己就是证据:
             *   "TRC fbox n48 72/73 total (r151)=0001" 紧跟着
             *   "PH: fbox fontn==0001"。标签会撒谎的仪器比没有仪器更糟。 */
            /* 【r183】`PH: probe fontst=` —— 字模(平面 2)这一条链的最终判决。
             *   取值(完整判读见 minivdd.asm 的 VgaFontProv 头注):
             *     2 = 某一档窗口【过了 8 字节记号自证】、8KB 整块写进去、整块回读
             *         也成立(第几档看 `PH: probe fontwin=`);
             *     3 = 四档都不行、BIOS 的 AX=1104h 之后指纹对上了;
             *     5 = 四档都不行、BIOS 的 AX=1114h 之后指纹对上了;
             *     4 = 窗口能读写、字模就是装不上 ⇒ 病灶在取指一侧;
             *     6 = 四档连 8 字节记号都读不回来 ⇒ 回读仪器是瞎的, 本场的
             *         fontp0 / fontfp / fntreg 全都不能当硬件结论读。
             *   如果 stat = 4 或 6, 同一份日志里另外三行仍然有用:
             *           `PH: probe fntreg=` = GCR06 / SR04 的回读(哪一档 = 哪个值,
             *             每档期望值见 `PH: probe fontwin=` 的注释);
             *           `PH: probe fontp0=` = 同一格在【平面 0】上的内容;
             *           `PH: fbox fontfp=`  = 同一格在【平面 2】上的内容;
             *         fontp0 == fontfp => 这张卡的读平面选择不参与, 回读是瞎的。
             *   【r183 为什么删掉了 "1 = 开场指纹就对"】./182 里指纹 == FontExp
             *   从来没成立过, 而"看一眼就跳过"还有一个更坏的副作用: 一旦那个读数
             *   (在一扇读不出平面的窗里)碰巧相等, 整条字模路一次都不跑。现在每场
             *   第一次一定走完"记号 → 8KB → 整块回读", 写进去的内容与已存在的字模
             *   逐字节相同, 所以多写一遍没有代价。
             *   【r177 那一枪为什么不算数】r177 发给 BIOS 的是 AX=1122h, 而它在参考
             *   实现(VGABIOS vgabios.c 的 AH=11h 分派表)里落到
             *   biosfn_load_gfx_8_14_chars —— 一个只有 unimplemented() 的空函数。 */
            PHASEV( "PH: probe fontst=", (uint16_t)s_probe[VBE_TR_PR_BOFFL] );
            /* 【r146】模式/寻址签名 —— 决定"同一块 16bpp 线性显存是按
             *   打包像素扫出, 还是按 4 平面/奇偶解释"的就是这几个字节。
             *   相位见行尾: 1/5/7/8/35 = 4 号/5 号/7 号/48 号/35 号。
             *   打包顺序与 VBE_TR_FS_* 一致(高字节在前)。 */
            /* 【r146】hi-res 快照/回放记账: valid / savedN / restN / lastDiff。
             *   lastDiff = 回到 Windows 时"那 10 个字节里有几个与我们的
             *   快照不同" —— 0 说明主 VDD 自己已经恢复好了(我们那一枪是
             *   空枪), >0 说明它漏了几个, 数字就在这一行。 */
            DiagHexMin( "TRC hires valid (1=have snapshot, r146)",
                        (uint16_t)s_hires[VBE_TR_HR_VALID] );
            DiagHexMin( "TRC hires savedN (r146)",
                        (uint16_t)s_hires[VBE_TR_HR_SAVEDN] );
            DiagHexMin( "TRC hires restoredN (r146)",
                        (uint16_t)s_hires[VBE_TR_HR_RESTN] );
            DiagHexMin( "TRC hires lastDiff (bytes wrong at switch-back, r146)",
                        (uint16_t)s_hires[VBE_TR_HR_DIFF] );
            /* 【r158】同一件事的"是哪一个": 位图, 图例就在标签里, 于是这一行
             *   在日志里自解释(本项目的老规矩: 读数必须带着自己的判读表)。
             *   位序: 0=SR04 1=GCR06 2=CR13 3=CR17 4=SR01 5=SR02 6=SR03
             *         7=GCR03 8=GCR04 9=GCR05 10=CR14
             *   与 `PH: probe hdiff=` 同一个值(那条走三档都写的 PH 通道)。 */
            DiagHexMin( "TRC hires diff bits (0=SR04 1=GCR06 2=CR13 3=CR17 4=SR01"
                        " 5=SR02 6=SR03 7=GCR03 8=GCR04 9=GCR05 10=CR14, r158)",
                        (uint16_t)(((uint16_t)s_probe[VBE_TR_PR_HDIFFH] << 8) |
                                    (uint16_t)s_probe[VBE_TR_PR_HDIFFL]) );
            /* 【r146 无条件通道】签名与相位也走 PH 通道(三档构建都写):
             *   判读时先看这几行 —— 缺了它们 = 测的不是本构建。 */
            PHASEV( "PH: hires diff=",  (uint16_t)s_hires[VBE_TR_HR_DIFF] );
            PHASEV( "PH: hires restN=", (uint16_t)s_hires[VBE_TR_HR_RESTN] );
            /* --- 【r147】六个采集槽逐个落盘(整块只在值变化时写) -------
             *   槽名就是相位: p4=进盒【前】(期望还是我们的模式) /
             *   p5=文本态修补 写完、链给主 VDD 之前 / p7=回到 Windows 之后
             *   (HiresRestore 内部) / p48=盒子设模式之前 / p35=持屏期最近
             *   一拍 / p35FIRST=本场会话的第一拍(主 VDD 刚设完盒子模式)。
             *   每槽两行, 每行四个字节(高字节在前):
             *     A = SR04 / GCR06 / SR01 / GCR05
             *     B = SR02 / SR03 / AR10 / 相位(src)
             *   【判读】p35FIRST 的 A 行就是"盒子花屏时硬件的寻址态":
             *   文本该是 020E0110, 我们高彩是 06050100。                 */
            DiagHex32( "TRC phz p4 A SR04/GCR06/SR01/GCR05",
                       (((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SR04]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_GCR06] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SR01]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_GCR05])) );
            DiagHex32( "TRC phz p4 B SR02/SR03/AR10/src",
                       (((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SR02]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SR03]  << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_AR10]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P4 + VBE_TR_FS_SRC])) );
            DiagHex32( "TRC phz p5 A SR04/GCR06/SR01/GCR05",
                       (((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SR04]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_GCR06] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SR01]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_GCR05])) );
            DiagHex32( "TRC phz p5 B SR02/SR03/AR10/src",
                       (((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SR02]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SR03]  << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_AR10]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P5 + VBE_TR_FS_SRC])) );
            DiagHex32( "TRC phz p7 A SR04/GCR06/SR01/GCR05",
                       (((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SR04]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_GCR06] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SR01]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_GCR05])) );
            DiagHex32( "TRC phz p7 B SR02/SR03/AR10/src",
                       (((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SR02]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SR03]  << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_AR10]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P7 + VBE_TR_FS_SRC])) );
            DiagHex32( "TRC phz p48 A SR04/GCR06/SR01/GCR05",
                       (((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_SR04]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_GCR06] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_SR01]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_GCR05])) );
            DiagHex32( "TRC phz p48 B SR02/SR03/AR10/src",
                       (((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_SR02]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_SR03]  << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_AR10]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P48 + VBE_TR_FS_SRC])) );
            DiagHex32( "TRC phz p35 A SR04/GCR06/SR01/GCR05 (latest)",
                       (((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR04]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_GCR06] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR01]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_GCR05])) );
            DiagHex32( "TRC phz p35 B SR02/SR03/AR10/src (latest)",
                       (((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR02]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SR03]  << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_AR10]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35 + VBE_TR_FS_SRC])) );
            DiagHex32( "TRC phz p35FIRST A SR04/GCR06/SR01/GCR05 (r147)",
                       (((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SR04]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_GCR06] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SR01]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_GCR05])) );
            DiagHex32( "TRC phz p35FIRST B SR02/SR03/AR10/src (r147)",
                       (((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SR02]  << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SR03]  << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_AR10]  <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_SRC])) );
            DiagHexMin( "TRC phz p35FIRST D CR0C (display start low, r151)",
                        (uint16_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_CR0C] );
            DiagHex32( "TRC phz p35FIRST C CR17/CR13/CR00/CR01 (r147)",
                       (((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_CR17] << 24) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_CR13] << 16) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_CR00] <<  8) |
                        ((uint32_t)s_phase[VBE_TR_PS_P35FIRST + VBE_TR_FS_CR01])) );
            /* 【r147】补写记账 —— 判据有没有放行、写了几次、写住没有。 */
            /* r167: 整组 mode 3 写入(17 字节)的逐字节失配位图 + 写入次数。
             * 判读(三条必须一起看):
             *   mcw 不涨            = 整组那条路一次都没跑(闸门或调用点的问题);
             *   mcbad0/1/2 全 0000  = 17 个字节都读回相等, 我们的写站住了;
             *   某一位恒置          = 那一格被别的东西按住, 位序见 vbeshare.h。
             * stillBad 本轮的含义改成 0 = 17 个字节全部读回相等 / 1 = 有字节
             * 没站住 —— 它不再是"个数", 个数由位图逐位给出。 */
            /* 【r168】字模整块自证 + "让卡自己的 BIOS 重设 mode 3"的结果。
             * 判读(两条独立):
             *   低 6 位(fontbadb & 3F) = 最近一次 8KB 整块回读里不一致的 dword
             *     数(饱和 63)。== 0 ⇒ 那 8KB 逐字节相等, "字模没装上"这个
             *     假设被自己的读数否掉; 非 0 ⇒ 我们确实没能把字模整块写对,
             *     而拷完的第一次失败已经记在 `fbox fontbad` 里。
             *   高 2 位(bits 6-7) = 嵌套 V86 INT 10h AX=0003 的结果:
             *     0 = 没执行(拷贝成立 / VgaFix=0 / 本场预算用尽),
             *     1 = 执行了且 BIOS 回 AH=0,
             *     2 = IVT[10h] 为 0, 没敢执行,
             *     3 = 执行了但 BIOS 回 AH!=0。
             *   位序与字段边界见 vbeshare.h 的 VBE_TR_MC_FONTBADG 注。 */
            FlushLog();
fbox_dump_done:
            ;
        }
    }

    /* --- 【r102】日志闸门丢弃计数 ---------------------------------------
     * 三条计数都由 vddapi16.asm 的 DIAGWRITELOG 无条件累加(它不发任何
     * INT 21h 就返回, 所以记录这件事本身不会再把我们拖回 DOS):
     *   InDOS = 调用瞬间 DOS 的 InDOS 标志已置位(DOS 临界区中)
     *   Reent = 日志例程自身被重入
     *   Cb    = 处在主 VDD 回调窗口(g_logBlocked)
     * 任何一条非 0 都是硬证据; 三条全 0 说明闸门一次都没拦到东西 ——
     * 那时"冻结与日志通道无关"这句话才有资格说。                        */
    {
        uint16_t s1 = DiagSkipInDOS;
        uint16_t s2 = DiagSkipReent;
        uint16_t s3 = DiagSkipCb;

        if (s1 != s_skipInDos || s2 != s_skipReent || s3 != s_skipCb)
        {
            s_skipInDos = s1;
            s_skipReent = s2;
            s_skipCb    = s3;
            DIAG( "LOG skip: InDOS / Reent / Cb" );
            DiagHex( "LOG skip InDOS", s1 );
            DiagHex( "LOG skip Reent", s2 );
            DiagHex( "LOG skip Cb",    s3 );
            FlushLog();
        }
    }

    /* --- 【r103】InDOS 闸门(InDOS 远址)的自洽性证据 ----------------------
     * 三行合起来就是第②道闸门到底能不能用的判词:
     *   gate ok=1 → 拿到了 AH=34h 的远址且未被判定"不可用"(闸门② 生效中);
     *   gate ok=0 且 val=FFFF → 从来没取到过远址(闸门② 从未生效);
     *   gate ok=0 且 val≠0 → 【首次读到就是非 0】: 要么指针不可信, 要么
     *        DOS 的 InDOS 计数被谁漏减后永不归零 —— 两种情况都说明这个
     *        标志不能当硬闸门用, DLOGGUARD 已按连续 8 次自动停用它。
     *   gate busy = 当前连续"忙"次数(未停用时有意义)。
     * 【为什么非要打出来】r102 就是被这道闸门悄悄锁死的: 日志里没有任何
     * 一行告诉读者"闸门拦掉了全部内容", 于是只剩 PROBE 一行。          */
    {
        uint16_t ok  = DiagInDosOk;
        uint16_t val = DiagInDosVal;

        if (ok != s_dosOk || val != s_dosVal)
        {
            s_dosOk  = ok;
            s_dosVal = val;
            DiagHex( "LOG gate InDOS ok",  ok );
            DiagHex( "LOG gate InDOS val", val );
            DiagHex( "LOG gate InDOS busy", DiagInDosBusy );
            FlushLog();
        }
    }

    /* --- DISPLAY_DRIVER_DISABLING(26) 计数变化 → Heal 的触发条件 ------- */
    {
        uint32_t d26 = *(uint32_t *)(void *)(b + VBE_TR_Counts + 26 * 4);

        if (d26 != s_trcDis26)
        {
            s_trcDis26 = d26;
            DiagHex( "TRC dis26 cnt", (uint16_t)d26 );
            if (s_healWanted)
            {
                s_healReq = 1;
                DIAG( "TRC dis26 -> heal request" );
            }
            FlushLog();
        }
    }

    /* --- 新记录 ------------------------------------------------------- */
    /* [r412] ACCESS_LINEAR_MEMORY_MODE(12) -> heal 请求
     *   12 号 = "访问权交回 Windows(线性帧缓冲)"; 此刻必须让显示重新对齐到
     *   我们的 VBE 模式. mini-VDD 已在该钩子里做过 TraceRecord, 这里用与
     *   26 号完全相同的"计数变化"机制取走, 无需新增 VxD 调用. */
    {
        uint32_t d12 = *(uint32_t *)(void *)(b + VBE_TR_Counts + 12 * 4);

        if (d12 != s_trcDis12)
        {
            s_trcDis12 = d12;
            s_healReq  = 1;
            DIAG( "TRC dis12 -> heal request" );
            FlushLog();
        }
    }

    total = *(uint32_t *)(void *)b;                 /* VBE_TR_WriteIdx */
    /* 【r100】WriteIdx 总数: 把"主 VDD 从来没调用过"与"取了但失败"彻底
     * 分开 —— r99 整轮追踪数据为 0, 就是因为这两种情况在日志里长得一样。 */
    if (total != s_trcTotalSeen)
    {
        s_trcTotalSeen = total;
        DiagHex32( "TRC calls", total );     /* r104: 不再截成 16 位 */
        FlushLog();
    }
    if (total <= s_trcLastSeq)
        return (uint16_t)(printed ? 1 : 0);

    oldest = (total > VBE_TRACE_RECS) ? (total - VBE_TRACE_RECS + 1) : 1;
    if (s_trcLastSeq + 1 < oldest)
    {
        DiagHex32( "TRC lost", oldest - s_trcLastSeq - 1 );   /* r104 */
        s_trcLastSeq = oldest - 1;
    }

    /* 【r107】按序号打新记录, 但跳过 35 号(CHECK_HIRES_MODE)洪泛 ——
     * 旧写法把 TRC_MAXPRINT 条预算全花在 35 号上, 一个窗口化 DOS 会话的
     * 十万次分派里其余服务【一条都看不见】(./106 实测)。跳过的那部分
     * 由计数桶(含下面的增量表)负责, 信息不丢。
     * 【r110】再加两道: ① 03/09/0F 银行循环洪泛同样只计数不打明细
     * (见 TRC_FN_BANK_* 头注); ② 单次跨度超过 TRC_FLOOD_SPAN 就整段
     * 只报一条 —— 那种量级下明细本来就是环被冲掉之后的残渣。
     * 发布构建走 r106 的原循环: 那里 DiagTraceRec 是空宏, 扫全环纯属浪费。 */
    seq = s_trcLastSeq + 1;
#ifdef VBE9X_DIAG
    {
        uint32_t f23 = 0;
        uint32_t storm[3];

        storm[0] = storm[1] = storm[2] = 0;
        if (total - s_trcLastSeq > TRC_FLOOD_SPAN)
        {
            /* 跨度太大: 明细无意义(环只有 64 条), 只报量与三个洪泛计数。
             * budget=0 ⇒ TraceDumpFiltered 只扫计数、一条明细都不打。*/
            DiagHex32( "TRC flood: records suppressed (span)", total - s_trcLastSeq );
            printed = TraceDumpFiltered( total, seq, 0, &f23, storm );
        }
        else
        {
            printed = TraceDumpFiltered( total, seq, TRC_MAXPRINT, &f23, storm );
        }
        if (f23 >= 16)      /* r110: 见 r110 说明 —— ./109 t5 的日志里这一行
                             * 本身出现了 600+ 次(234x0x0C、289x0x0F、55x0x40…),
                             * "每次 dump 都有几条被跳过"是常态, 不值得每拍
                             * 写一行盘; 35 号的量在 TRC + 增量表里本来就有。*/
            DiagHex32( "TRC f23 skipped (hires poll, suppressed)", f23 );
        if (storm[0] || storm[1] || storm[2])
        {
            DIAG( "TRC storm: RESET_BANK/RESTORE_REG/MAKE_NOT_BUSY suppressed" );
            DiagHex32( "TRC storm RESET_BANK",    storm[0] );
            DiagHex32( "TRC storm RESTORE_REG",   storm[1] );
            DiagHex32( "TRC storm MAKE_NOT_BUSY", storm[2] );
        }
        s_trcLastSeq = total;
    }
#else
    for (seq = s_trcLastSeq + 1; seq <= total; seq++)
    {
        uint8_t *r = b + VBE_TR_Recs +
                     (uint16_t)((seq - 1) % (uint32_t)VBE_TRACE_RECS) * VBE_TRACE_RECB;

        (void)r;                        /* 发布构建下 DiagTraceRec 是空宏 */
        DiagTraceRec( seq,
                      *(uint16_t *)(void *)(r + VBE_TREC_Func),
                      *(uint32_t *)(void *)(r + VBE_TREC_Ecx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Edx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Ebx),
                      *(uint32_t *)(void *)(r + VBE_TREC_Eax) );
        s_trcLastSeq = seq;
        if (++printed >= TRC_MAXPRINT)
            break;
    }
#endif
    /* 【为何有条件】DiagWriteLog 每行已自带 commit, 这里的 FlushLog 只是
     * 里程碑兜底; 而本函数在每次 MoveCursor 入口都会被调一次, 无条件
     * commit 等于"鼠标每动一下就 commit 一次文件" —— 光标是排障通道,
     * 不能被日志拖慢(卡顿本身还会掩盖花屏的复现)。                 */
    if (printed)
        FlushLog();
    return (uint16_t)(printed ? 1 : 0);
}

/*==========================================================================
 *
 *  VGA 标准 DAC 与 VDD 陷阱开关(display.c 使用)
 *
 *========================================================================*/

/* 8 位端口输出。MSVC 1.52 内联汇编只认 16 位寄存器，out 正好够用。
 * 【为何用内存变量中转】与 VxDCallApi 相同的理由：避开两种编译器
 * 在寄存器生存期上的方言差异。                                         */
static void outp8(uint16_t port, uint8_t val)
{
    static uint16_t sPort;
    static uint8_t  sVal;

    sPort = port;
    sVal  = val;
    _asm {
        push    dx
        push    ax
        mov     dx, [sPort]
        mov     al, [sVal]
        out     dx, al
        pop     ax
        pop     dx
    }
}

/* 8 位端口输入(inp8)。与 outp8 同一套写法与理由。
 * 【为什么本轮才敢加输入】见 PortProbeRead 头注: 端口读是 VGA 显示驱动
 * 的标准动作(在售的 VGA.DRV 就在 ring 3 读写这些寄存器), 但本驱动此前
 * 只用过"输出"(DAC 写), 从未证过"读"在这台机器上可行 —— 所以它挂在
 * 默认关闭的 PortProbe 开关后面, 由用户单独跑一次去证。
 * 【r111: 为什么从 #ifdef VBE9X_DIAG 里搬出来】r109..r110 里它只服务
 * PortProbeRead —— 而那个函数的全部调用点都写在 DiagHex 的实参里, 发布
 * 构建里连实参都不展开, 于是"读端口"在发布版里根本不存在。r111 的硬件
 * 指纹(VxdHwFpRead)是【功能性】的: 它决定回落层要不要动硬件, 发布构建
 * 也必须跑, 所以这两个函数必须常驻。代价: 发布构建多几十字节。        */
static uint8_t inp8(uint16_t port)
{
    static uint16_t sPort;
    static uint8_t  sVal;

    sPort = port;
    sVal  = 0;
    _asm {
        push    dx
        push    ax
        mov     dx, [sPort]
        in      al, dx
        mov     [sVal], al
        pop     ax
        pop     dx
    }
    return sVal;
}

/* 读写一对"索引/数据"端口(CRTC 3D4h/3D5h, 时序器 3C4h/3C5h, GCR 3CEh/3CFh)。
 * 【只写索引、只读数据】写索引寄存器对硬件无副作用(数据寄存器不动就等于
 * 什么都没改), 读数据寄存器是纯读 —— 整条路径不改变任何显示状态。   */
static uint8_t idxread(uint16_t idxPort, uint16_t dataPort, uint8_t idx)
{
    outp8(idxPort, idx);
    return inp8(dataPort);
}

/* --- 【r117】16 位版本的端口读写: 专给 VBE_DISPI(1CEh/1CFh) ------------
 * 【为什么必须是 16 位】DISPI 的每一个寄存器都是 16 位, 而它的数据端口
 * 在字节访问下的语义【各家不同】: QEMU/Bochs 的 vbe_ioport_write_data 对
 * 1 字节写是"把整个 16 位寄存器赋成这个字节"(高字节被清 0), 而不是
 * "先低字节后高字节"; 读侧同理由 memory core 按 size 截断 ⇒ 字节读只能
 * 拿到低字节。XRES=1024=0x0400 的低字节是 0x00、ID=0xB0C5 的低字节是
 * 0xC5 —— 用 8 位访问会让"ID 认不出来"(探测直接静默)且"XRES 恒读成 0"
 * (永远误报不符)。Bochs 的 VBE BIOS 自己就是用 outw/inw 访问这两个端口,
 * 所以 16 位是这条总线上唯一被所有实现共同支持的字长。
 * 【为什么按 inp8/outp8 的写法来】同一套 push/pop + 静态中转, 端口号与
 * 数据都经内存变量, 不在内联汇编里直接用 C 表达式。                */
static uint16_t inp16(uint16_t port)
{
    static uint16_t sPort;
    static uint16_t sVal;

    sPort = port;
    sVal  = 0;
    _asm {
        push    dx
        push    ax
        mov     dx, [sPort]
        in      ax, dx
        mov     [sVal], ax
        pop     ax
        pop     dx
    }
    return sVal;
}

static void outp16(uint16_t port, uint16_t val)
{
    static uint16_t sPort;
    static uint16_t sVal;

    sPort = port;
    sVal  = val;
    _asm {
        push    dx
        push    ax
        mov     dx, [sPort]
        mov     ax, [sVal]
        out     dx, ax
        pop     ax
        pop     dx
    }
}

static uint16_t idxread16(uint16_t idxPort, uint16_t dataPort, uint16_t idx)
{
    outp16(idxPort, idx);
    return inp16(dataPort);
}

static void idxwrite16(uint16_t idxPort, uint16_t dataPort,
                       uint16_t idx, uint16_t val)
{
    outp16(idxPort, idx);
    outp16(dataPort, val);
}

/* ---------------------------------------------------------------------------
 * 【r119】ProbeDispiId —— 用"写—回读—还原"确认 VBE_DISPI 寄存器 0(ID)
 *
 * 【r117/r118 为什么一行效果都没有】两者都是【只读】索引 0 就下结论:
 *     id = idxread16( 0x1CE, 0x1CF, 0x0000 );
 *     if (id < 0xB0C0 || id > 0xB0C7) { 永久静默 }
 * 而 DISPI 的 ID 寄存器是【只写】的。以本测试机的实现(VirtualBox
 * DevVGA.cpp)为例, 写数据端口时:
 *     case VBE_DISPI_INDEX_ID:
 *         if (val == VBE_DISPI_ID0 || … || val == VBE_DISPI_ID_ANYX ||
 *             val == VBE_DISPI_ID_HGSMI || val == VBE_DISPI_ID_CFG)
 *                 pThis->vbe_regs[VBE_DISPI_INDEX_ID] = val;   // 只有这一条赋值
 *         break;
 * 读的时候没有任何分支: "default: val = pThis->vbe_regs[idxVbe];" —— 直接
 * 返回上一次【被写进去】的那个值。VM 复位时 vbe_regs[] 全 0, 于是
 *   ① 没人写过索引 0 ⇒ 读回 0x0000;
 *   ② 我们自己刚用过 outp16(0x1CE, 0x0000) ⇒ vbe_index = 0 ⇒ "读回"的其实
 *      是 pThis->vbe_index(0x0000), 或者是端口不存在时的浮空 0xFFFF。
 * ./118 的日志把这件事记死了: "DISPI absent id=FFFF" —— 而 0xFFFF 正好
 * 【单调一条】就能落进这个分支(它不在 0xB0C0..0xB0C7 里), 于是探针把自己
 * 永久关掉(见本函数下方的说明), 修复路径从未执行过一次。这不是"这台机器
 * 没有 DISPI", 而是"探针从来没有问对问题":
 *   · 0xFFFF = 1CEh/1CFh 这对端口【根本没人应答】时的浮空总线值;
 *   · 0x0000 = 有人应答, 但索引 0 从未被写过。
 * 两种情况必须分开, 而《VBE_DISPI 的通用识别手法》本来就只有一个:
 * 先写一个已知合法的 ID 再回读 —— Bochs 的 VBE BIOS 自己就是这么认的,
 * QEMU/VBox 的 guest driver 也都是这么认的。
 *
 * 【写 ID 会破坏什么吗】不会。索引 0 的唯一用途就是能力探测: 它不参与
 * XRES/YRES/BPP/ENABLE 任何一个决定扫描格式的字段。反过来, 把 BIOS 自己
 * 写进去的值原样放回去, 连"能力探测结果"都不会被我们改掉。
 *
 * 【顺序】① 读一次索引 0, 结果记为 pre(仅作日志, 不做判据);
 *        ② 逐个写候选 ID、每次回读, 第一个读回相同值的就是本机 ID;
 *        ③ 无论成败都把 pre 写回, 让端口回到我们发现它时的状态。
 *
 * 【候选表】0xBE02(VBE_DISPI_ID_ANYX)优先 —— 它是 VBox 扩展, 明确表达
 * "任意分辨率", 也是上游写分支里最后一个被接受的值; 之后是标准 Bochs
 * ID4/ID3/ID2/ID1/ID0。VirtualBox 的写分支对 0xBE02 是接受的, Bochs 对
 * 不认识的 ID 只是不存, 所以这一串在两大家族上都不会造成副作用。
 * ------------------------------------------------------------------------- */
static const WORD s_dispiIdCand[6] = { 0xBE02, 0xB0C4, 0xB0C3, 0xB0C2, 0xB0C1, 0xB0C0 };

int __far __pascal DispiIdProbe( void )
{
    WORD pre, back;
    uint16_t i;

    pre = idxread16( 0x1CE, 0x1CF, 0x0000 );

    DiagMark( "DISPIID" );
    DiagHexMin( "DISPIID policy (1=write-probe on)", g_runDispiIdProbe );
    DiagHexMin( "DISPIID pre (read-only, not a verdict)", pre );

    for (i = 0; i < 6; i++)
    {
        idxwrite16( 0x1CE, 0x1CF, 0x0000, s_dispiIdCand[i] );
        back = idxread16( 0x1CE, 0x1CF, 0x0000 );
        DiagHexMin( "DISPIID wrote", s_dispiIdCand[i] );
        DiagHexMin( "DISPIID read back", back );
        if (back == s_dispiIdCand[i])
        {
            /* 认出来了: 把端口恢复成我们进来时的样子, 然后落一行结论。 */
            idxwrite16( 0x1CE, 0x1CF, 0x0000, pre );
            DIAGMIN( "DISPIID confirmed: index 0 echoes a legal ID -> DISPI present" );
            FlushLog();
            return 1;
        }
        /* 【为什么 0xFFFF 要单独判】它不是"写不进去", 而是"没人应答":
         * 浮空总线上读什么都是全 1。把它和"端口活着但不认这个 ID"分开记,
         * 下一轮看日志就能一眼区分"没有 DISPI"与"有 DISPI 但候选表不全"。 */
        if (back == 0xFFFF)
        {
            DiagHexMin( "DISPIID no response at candidate", s_dispiIdCand[i] );
            break;
        }
    }

    idxwrite16( 0x1CE, 0x1CF, 0x0000, pre );
    DIAGMIN( "DISPIID not confirmed: no candidate echoed -> DISPI unusable" );
    FlushLog();
    return 0;
}

/*==========================================================================
 *  【r111】硬件指纹 —— 用 VGA 寄存器本身回答"硬件在不在我们的模式里"。
 *
 *  【为什么它是本轮的关键件】驱动侧此前所有"模式对不对"的判据都是自证的:
 *    · VerifyDisplayMode: VxD 软件状态里 currentMode 没人改过(当然还是
 *      0x117)+ 帧缓冲写/读回(选择子映的是显存, 与 CRTC 在什么模式无关);
 *    · INT 10h AH=0Fh: 在 Windows VM 里被主 VDD 的虚拟 BIOS 接走, 回的是
 *      它的记账(本机恒为 ax=8072), 与硬件无关。
 *   ./109 实测: 屏幕整屏花, 而 pre-verify 照旧 ok=0001 —— 于是"验证优先"
 *   的回落层只能退化成"每次都强制 4F02", 那正是"跟 DOS 盒抢硬件"的来源。
 *   真正能读的只有寄存器, 而 r110 的两个采样点(重建前/重建后)已证明:
 *   在本机 ring 3 读 CRTC/时序器/GCR 是安全且自洽的。
 *
 *  【怎么比】12 个字节 = 光栅几何 + 像素格式:
 *     CRTC 00/01/12/13/0C/0D/14/17, 时序器 01/04, GCR 05/06。
 *    基线只在一个时刻建立: 一次【成功】的重建之后(post-verify 通过)。
 *    之后每次回落层要动手之前先比一次 —— 全等 ⇒ 硬件就是我们的模式,
 *    这一次【不发 4F02】, 只做"重绘 + 重新登记"; 不等 ⇒ 才动硬件。
 *
 *  【开关】HwFp(默认 1)。关掉 = r110 行为(不看寄存器, 每次都强制重设)。
 * 【分组】读哪几组沿用 PortGrp 的位图(默认 7 = 前三组); 第 4 组(3CCh/
 *    3C6h)是 ./109 t3 的嫌疑组, 指纹【永不】读它。                   */
#define HWFP_N 12
static uint8_t s_hwFpCur[HWFP_N];
static uint8_t s_hwFpBase[HWFP_N];
static uint8_t s_hwFpBaseOk = 0;

static void HwFpFill(uint8_t *d)
{
    /* 顺序与 PortProbeRead 的打印顺序逐项一致, 便于两处对照。 */
    d[0]  = idxread(0x3D4, 0x3D5, 0x00);
    d[1]  = idxread(0x3D4, 0x3D5, 0x01);
    d[2]  = idxread(0x3D4, 0x3D5, 0x12);
    d[3]  = idxread(0x3D4, 0x3D5, 0x13);
    d[4]  = idxread(0x3D4, 0x3D5, 0x0C);
    d[5]  = idxread(0x3D4, 0x3D5, 0x0D);
    d[6]  = idxread(0x3D4, 0x3D5, 0x14);
    d[7]  = idxread(0x3D4, 0x3D5, 0x17);
    d[8]  = idxread(0x3C4, 0x3C5, 0x01);
    d[9]  = idxread(0x3C4, 0x3C5, 0x04);
    d[10] = idxread(0x3CE, 0x3CF, 0x05);
    d[11] = idxread(0x3CE, 0x3CF, 0x06);
}

void __far __pascal VxdHwFpRead( void )
{
    HwFpFill( s_hwFpCur );
}

int __far __pascal VxdHwFpStore( void )
{
    uint16_t i;

    /* 【r111 门槛: 只把"看起来就是我们的模式"的那一枪记成基线】
     * 为什么需要: post-verify 是【自证】的(见 display.c 的
     * ApplyDesktopRestore 注), 所以一次"没能把格式修回来"的重建也会报 ok
     * —— 若把那一枪的寄存器记成基线, 之后所有比较都会说"匹配", 回落层就
     * 再也不动手了。这里用三个与我们自己的模式定义绑定的位做门槛(不依赖
     * 任何来自 BIOS 的数值):
     *   ① 时序器 01h bit0 = 1   ⇒ 8 点时钟(9 点时钟是文本模式)
     *   ② 时序器 04h bit3 = 1   ⇒ chain4 关闭(平面模式才会打开)
     *   ③ CRTC   17h bit6 = 1   ⇒ 字寻址(offset 13h 的单位 ×2)
     * 任一条不成立 ⇒ 不记基线, 回落层保持 r110 的"每次都真重设"行为。
     * 【r112 更正 —— 上一行的 ① 原先写反了】r111 的实现是
     * `if (s_hwFpCur[8] & 0x01) reject;`, 也就是"bit0 = 1 ⇒ 拒收", 与上面
     * 这条注释恰好相反。./111 的实测把后果照得很清楚: 十二个采样点的
     * `HwFp base` 全是 EEEE、`HwFp diff` 全是 EEEE —— 基线【一次都没建成】,
     * 于是 HwFp 这一轮等于没干活(判断永远退回"每次都重设")。而采样值
     * (seq01=0001 / seq04=000E / crtc17=00E3)三个门槛其实【全都满足】。
     * 现在按正确极性判, 并且把三个判定位原样打进日志(见 VxdHwFpDump),
     * 让"极性对不对"下一轮可以从日志直接核对, 不必再信注释。            */
    if (!(s_hwFpCur[8] & 0x01) || !(s_hwFpCur[9] & 0x08) || !(s_hwFpCur[7] & 0x40))
    {
        s_hwFpBaseOk = 0;
        return 0;
    }
    for (i = 0; i < HWFP_N; i++)
        s_hwFpBase[i] = s_hwFpCur[i];
    s_hwFpBaseOk = 1;
    return 1;
}

int __far __pascal VxdHwFpDiff( void )
{
    uint16_t i, n = 0;

    if (!s_hwFpBaseOk)
        return -1;                      /* 还没有基线: 调用方按旧行为处理 */
    for (i = 0; i < HWFP_N; i++)
        if (s_hwFpCur[i] != s_hwFpBase[i])
            n++;
    return (int)n;
}

/* 落盘一份"基线 vs 当前"(仅 diag 构建有内容); 返回 Diff。 */
int __far __pascal VxdHwFpDump( const char *why )
{
    int d;
    uint16_t i;

    (void)why;                          /* 发布构建里 DIAG(why) 是空宏 */
    d = VxdHwFpDiff();
    DIAG( why );
    DiagHex( "HwFp diff (0=same)", (uint16_t)((d < 0) ? 0xEEEE : d) );
    /* 【r112】把三个门槛位原样打出来 —— 极性这种事以后只认读数不认注释。
     * 位序 = 8dot(时序器 01h bit0) / chain4off(04h bit3) / word(CRTC 17h bit6)。*/
    DiagHex( "HwFp gate bits (8dot,ch4off,word)",
             (uint16_t)(((s_hwFpCur[8] & 0x01) ? 1 : 0) |
                        ((s_hwFpCur[9] & 0x08) ? 2 : 0) |
                        ((s_hwFpCur[7] & 0x40) ? 4 : 0)) );
    DiagHex( "HwFp base present", s_hwFpBaseOk );
    for (i = 0; i < HWFP_N; i++)
    {
        DiagHex( "HwFp cur ", s_hwFpCur[i] );
        DiagHex( "HwFp base", s_hwFpBaseOk ? s_hwFpBase[i] : 0xEEEE );
    }
    FlushLog();
    return d;
}

/*==========================================================================
 *  【r112】HwReg —— 硬件模式寄存器组: 存一份 / 回放一份。
 *
 *  【为什么必须有它】./111 把"官方路径"这条路证到了分岔口上:
 *    · t5(CbRestore=0, 回调里不发 BIOS): 主 VDD 的回调来了 3 次
 *      (CB evt=0101/0102/0103), 机器活到正常关机;
 *    · t8(Restore=3): 回调来了, 里面那次 4F02 被 r100 护栏【拒掉】
 *      (CB evt=0002), 机器也活到正常关机(带花屏);
 *    · t2/t3/t6(CbRestore=1, 回调里 4F02 领到预算、真的发出去): 三份日志
 *      全部以冻结收尾(没有任何 Disable/关机记录), 而且连一条 `CB evt=0Axx`
 *      都没能落盘 —— 环里的记录随冻结一起丢了。
 *  三组读数指向同一个结论: 【在主 VDD 的切屏事务里做嵌套 V86 调用】这台
 *  机器不答应(与 r99 在真机上三次死机的位置完全一致: 就是那次 4F02)。
 *
 *  可是 DDK 明写(§2.2): "It must set the display hardware back into the
 *  Windows running mode that it was in before" —— 这条义务不能不做。
 *  真正的显示驱动是怎么做的? 【直接重编程寄存器】, 而不是回头问 BIOS。
 *  本文件已经有读寄存器的现成机制(idxread, ./110/./111 两个采样点已证明
 *  ring 3 读 CRTC/时序器/GCR 安全), 于是本块补上"写"这一半:
 *    · VxdHwRegSave(): 开机时(4F02 之后、桌面已起)把整组寄存器存下来;
 *    · VxdHwRegRestore(): 回调里把这组值【写回】, 然后【读回比对】,
 *      返回差异个数 —— 0 就是"硬件确实被设回了我们的模式"。
 *  全程没有一个 BIOS 调用、没有一次 V86 执行 —— 这就是"官方路径"在
 *  本机可行的形态, 也是回落层(延迟 4F02)继续存在的理由。
 *
 *  【写哪些、为什么这么选】
 *    · CRTC 00h..18h: 光栅几何 + 起始地址(offset 13h / start 0Ch,0Dh)——
 *      DOS 盒一旦动过 CRTC, 花屏就出在这里。
 *      ⚠ CR11h 的 bit7 = "保护位": 置起时 CR00..CR07 只读。回放前必须
 *      先清掉它(这与写什么值无关, 是硬件协议), 写完再按保存值恢复。
 *    · 时序器 01h..04h: 8/9 点时钟、bandwidth、memory mode(chain4/odd-even)。
 *      ⚠ SR00h = 复位寄存器, 【不写】: 写它会把时序器推进复位态, 收益零、
 *      风险最大。这一条是这一块里最要紧的安全边界。
 *    · GCR 00h..08h: 写模式/读模式/奇偶/移位/内存映射选择。
 *  【不回放的】(诚实列出) ① 属性控制器(16bpp 线性模式不用它);
 *    ② Misc Output(3C2h 只写, 读不回来 ⇒ 存不下; 时钟选择若被改过, 只有
 *    回落层那次 4F02 能修); ③ 任何芯片扩展寄存器(通用 VBE 驱动不知道
 *    它们在哪)。所以回放【不是万灵药】, 它只是"不需要嵌套 BIOS 的那一半",
 *    剩下的仍然交给回落层 —— 两条路的分工就这样定死。
 *========================================================================*/
#define HWREG_CRTC_N 0x19              /* 00h..18h */
#define HWREG_SEQ_N  0x05              /* 00h..04h(00h 只存不写) */
#define HWREG_GCR_N  0x09              /* 00h..08h */
/* 【r137 新增: 属性控制器 AR10h..AR14h】
 *   r112..r136 的注释在这里写着"属性控制器(16bpp 线性模式不用它)", 那句话
 *   被 ./136 的日志【当场证伪】: 屏幕从干净变成竖条纹的那一刻, 卡上变化的
 *   寄存器只有两个 —— SR01h(0001 -> 0003) 和 AR10h(0041 -> 000Dh)。也就是说
 *   "模式状态"里至少有一位住在属性控制器里, 而我们既没有采样它、也没有回放
 *   它、更没有在任何自检里看它。缺了它, `HwReg` 这套"已知良好样本 -> 回放"
 *   的闭环在结构上就是漏的, 而漏掉的那一半恰好是能在画面上看出来的一半。
 *   只存 AR10h..AR14h(模式/平面/平移那五个): AR00h..AR0Fh 是调色板, 由
 *   DAC_SetPalette 那条官方通道负责, 不塞进这里。                     */
#define HWREG_AR_N   0x05              /* AR10h..AR14h */
static uint8_t s_hwRegCrtc[HWREG_CRTC_N];
static uint8_t s_hwRegSeq[HWREG_SEQ_N];
static uint8_t s_hwRegGcr[HWREG_GCR_N];
static uint8_t s_hwRegAr[HWREG_AR_N];
static uint8_t s_hwRegArOk = 0;        /* r137: AR 那一段这次读到了吗 */
static uint8_t s_hwRegOk = 0;
/* 【r128】回放时 SR01 被归一化(9 点 -> 8 点)的次数。见 VxdHwRegSaveClocking
 * 的长注: 样本本身存成 9 点这件事必须先被量出来, 而不是靠推断。 */
static uint16_t g_hwRegNorm = 0;

/* 三个门槛位与 HwFp 完全同一套判据(线性打包像素模式的特征), 理由见
 * VxdHwFpStore 的注 —— 一份样本只有在"看起来就是我们的模式"时才有资格
 * 当回放源, 否则回放会把一个错误的模式当成目标。
 *
 * 【r131 加第四道门槛: CR17h bit7 = SE(Sync Enable)必须为 1】
 *   这是 r112..r130 那个"自证闭环"里唯一的漏洞, 也是"长条"能一路活下来
 *   的原因: 采样点在"一次成功重建之后"——可那个"成功"是【我们自己判的】
 *   (INT 10h 走主 VDD 的假 BIOS、显存读写自测与显示几何无关)。如果采样
 *   那一刻 CRT 同步本来就是关着的(CR17h bit7 = 0), 这份"已知良好样本"
 *   就把"屏幕不亮/宽度塌掉"当成了目标状态, 之后每一次回放都在忠实地
 *   复现它 —— 而 CR17h 的 bit7 从来没被任何一行代码检查过(前三道门槛
 *   只看 SR01/bit0、SR04/bit3、CR17h 的 bit6)。
 *   FreeVGA 原文(Index 17h): "SE -- Sync Enable: when set to 0, this bit
 *   disables the horizontal and vertical retrace signals". 任何我们期望
 *   有画面的模式, 这一位都必须是 1 —— 所以带 0 的样本一律不采纳
 *   (返回 0 = 没有可用样本 ⇒ 回调里的回放自动跳过, 交给回落层), 并且
 *   由 VxdCrtSyncFix() 在安全点把它补回 1。                            */
static int HwRegLooksOurs(void)
{
    /* 【r137 加第五道门槛: AR10h】线性打包像素模式的属性控制器形状。
     *   判据来自本机实测的"干净态"读数(SNAP ATC10 = 0041h):
     *     bit0 = 1  图形模式(与 GCR06h bit0 一起决定"文本/图形")
     *     bit6 = 1  palette address source —— ./136 里花屏态是 000Dh,
     *               这一位是 0。它是这两个读数之间唯一一个"必须是 1"
     *               的结构性差异, 而且 86Box 的通用 SVGA 渲染器正是用
     *               这一位(lowres = attrregs[10h] & 40h)挑渲染路径的。
     *   只在 AR 那段真的读到了(s_hwRegArOk)时才启用这道门槛 —— 读不到
     *   就不是证据, 不能拿它否决一份本来可用的样本。                  */
    if (!((s_hwRegSeq[1] & 0x01) && (s_hwRegSeq[4] & 0x08) &&
          (s_hwRegCrtc[0x17] & 0x40) && (s_hwRegCrtc[0x17] & 0x80)))
        return 0;
    if (s_hwRegArOk && !((s_hwRegAr[0] & 0x01) && (s_hwRegAr[0] & 0x40)))
        return 0;
    return 1;
}

/* 【r137】属性控制器索引/数据的两条原始通路。
 *   触发器纪律与 VxdReadAtcReg 完全一致(那里有 FreeVGA 原文与 ./132 那次
 *   "探针自己把 ATC10h 写坏"的教训): 读 3DAh 强制回索引态; 索引字节的
 *   bit5 = palette enable(读出来只有 0/1 的值, 不保证能回读到原样), 所以
 *   本文件一律用【1】当它的值 —— 一个期望有画面的模式, 这一位必须是 1,
 *   把它写成 0 等于把屏关掉。这也是本函数比"照抄读回来的索引字节"更安全的
 *   唯一理由。                                                          */
static void AtcSetIndex( uint8_t idx )
{
    (void)inp8( 0x3DA );                       /* -> INDEX phase */
    outp8( 0x3C0, (uint8_t)(0x20 | (idx & 0x1F)) );  /* PAS = 1, select idx */
}

static uint8_t AtcReadIdx( uint8_t idx )
{
    uint8_t v;

    AtcSetIndex( idx );
    v = inp8( 0x3C1 );                         /* data (leaves DATA phase) */
    (void)inp8( 0x3DA );                       /* hand back INDEX phase  */
    return v;
}

uint16_t __far __pascal VxdWriteAtcReg( uint16_t idx, uint16_t val )
{
    if (idx > 0x1F)
        return 0xFFFF;
    AtcSetIndex( (uint8_t)idx );               /* -> DATA phase */
    outp8( 0x3C0, (uint8_t)(val & 0xFF) );     /* data write    */
    (void)inp8( 0x3DA );                       /* hand back INDEX phase */
    return (uint16_t)(val & 0xFF);
}

int __far __pascal VxdHwRegSave( void )
{
    uint16_t i;

    for (i = 0; i < HWREG_CRTC_N; i++)
        s_hwRegCrtc[i] = idxread( 0x3D4, 0x3D5, (uint8_t)i );
    for (i = 0; i < HWREG_SEQ_N; i++)
        s_hwRegSeq[i]  = idxread( 0x3C4, 0x3C5, (uint8_t)i );
    for (i = 0; i < HWREG_GCR_N; i++)
        s_hwRegGcr[i]  = idxread( 0x3CE, 0x3CF, (uint8_t)i );

    /* 【r137】AR10h..AR14h。0xFF 是"读不到"的哨兵(端口被虚拟化挡住时,
     *   属性控制器可能整个回 0xFF); 只要有一个读成 0xFF 就整段作废 ——
     *   宁可没有 AR 样本, 也不要一个假的"已知良好"。                    */
    s_hwRegArOk = 1;
    for (i = 0; i < HWREG_AR_N; i++)
    {
        s_hwRegAr[i] = AtcReadIdx( (uint8_t)(0x10 + i) );
        if (s_hwRegAr[i] == 0xFF)
            s_hwRegArOk = 0;
    }

    if (!HwRegLooksOurs())
    {
        s_hwRegOk = 0;
        return 0;
    }
    s_hwRegOk = 1;
    return 1;
}

int __far __pascal VxdHwRegOk( void )
{
    return (int)s_hwRegOk;
}

/* 【r128】把样本里的两个"模式判定"读数暴露出来。
 *
 * 【为什么要它】./127 的 r1 日志给出了本轮最有价值的一行:
 *     HwReg diff seq idx=0001   now=0003   sav=0002
 * 也就是说: 硬件当时是 0x0003, 而我们【存下来的已知良好样本】是 0x0002 ——
 * 0x0002 的 bit0 = 0 = **9 点字符时钟**, 那是 VGA 文本时序, 不可能是
 * 高分辨率图形模式该有的值。⇒ 我们一直在拿一个【本身就错】的样本当基准:
 *   · VxdHwRegDiff 报"不同", 我们会去"修", 但修的方向是错的(往 9 点修);
 *   · 更糟的是它会掩盖真正的偏差 —— 基准错了, 全等的结论就没有意义。
 *
 * 【为什么样本会存成 9 点】采样点在"一次成功重建之后", 而 ./123 已经证明:
 * 主 VDD 那次 4F02 才把时钟改成 8 点, 而采样发生在重建序列内部 ——
 * 具体在哪一步之前/之后, 此前没有任何读数。所以本函数先把这件事【量出来】,
 * 不再靠推断。                                                     */
uint16_t __far __pascal VxdHwRegSaveClocking( void )
{
    return (uint16_t)(s_hwRegSeq[1] & 0xFF);
}

/* 样本里的 SR01 bit0: 1 = 8 点(高分辨率图形模式应有的值), 0 = 9 点(文本)。 */
uint16_t __far __pascal VxdHwRegSaveIs8Dot( void )
{
    return (uint16_t)(s_hwRegSeq[1] & 0x01);
}

/* 回放时把 SR01 从 9 点归一化到 8 点的累计次数(见 VxdHwRegRestore 内的说明)。 */
uint16_t __far __pascal VxdHwRegNormCount( void )
{
    return g_hwRegNorm;
}

/*==========================================================================
 *  【r131】CRTC 17h 的显示使能位(SE)—— 读数 + 补写。
 *
 *  【为什么是这一位】./130 的读数把"长条"锁到了一个具体数字上:
 *    · 主机截图 1_host.png: 整幅桌面(六个图标 + 整条任务栏)被压进 320 像素
 *      宽, 纵向却 1:1(768 行)。用任务栏上 14 段前景做最小二乘, 变换是
 *        host_x = 2.516 * fb_x + 1.0   ⇒ 可见的帧缓冲列 = 320/2.516 ≈ 127
 *    · 同一轮的日志: `HwReg: geo CR01=007F / width=0400 / verdict=0001`,
 *      `SR01=0001`, `GCR06=0005`, `ATC10=0041` —— **全都是对的**。
 *    · 127 ≈ 128 = CR01 + 1 = (CR01 按【字符】而不是按【像素】算出来的宽度)。
 *      也就是说: 硬件把 CR01 当成了像素数, 而不是字符数 —— 恰好是
 *      "点时钟乘法器没有被使能"的样子。
 *
 *  【标准怎么说】FreeVGA《CRT Controller Registers》Index 17h:
 *      bit7 SE  -- Sync Enable: "when set to 0, this bit disables the
 *                  horizontal and vertical retrace signals and forces them
 *                  to an inactive level. When set to 1, this bit enables
 *                  the horizontal and vertical retrace signals."
 *    任何我们期望有画面的模式, SE 都必须是 1。它不在 CR00..CR18 的几何
 *    字段里(所以 r130 那套几何自检看不见它), 也不在 r129 补的三个"文本/
 *    图形"位里(SR01 bit0 / GCR06 bit0 / AR10 bit0)——**这是一百多轮里从
 *    来没有任何一行代码看过的一位**。
 *
 *  【为什么会是 0】本卡是 3dfx Voodoo3(VBE 3.0 / LFB / 16MB)。在它的
 *    "扩展模式"(VBE LFB 高彩)里, 真正驱动行场同步的是 Voodoo 自己的
 *    视频时序发生器; VGA CRTC 的同步发生器在这类模式下不参与输出, 于是
 *    BIOS 把 SE 留在 0 是完全可能的 —— 而且**对真机没有副作用**。
 *    可在"以 VGA CRTC 为准"的实现里(86box 的 SVGA 核心就是这样, 见
 *    vid_svga.c: hdisp = crtc[1] (+1 if odd), 再乘 dots_per_clock, 而乘法
 *    那一段的门槛正是 `!scrblank && (crtc[0x17] & 0x80)`), SE = 0 会把
 *    行宽算成 CR01+1 = 128 ⇒ 桌面只剩最左 128 列 ⇒ 用户看到的"长条"。
 *    ⇒ 把 SE 补成 1: 对"以 CRTC 为准"的实现是【唯一正确的值】(标准要求
 *    有画面就必须为 1), 对"以芯片自身时序为准"的实现是【无关位】。
 *    两条路都不亏, 而且只动一个 bit —— 不碰任何时序/几何字段。
 *
 *  【为什么先读再写】正常路径(SE 已经为 1)一个字节都不写; 只有读回 0 才
 *    写, 写完再读回自证(返回 1 = 补回并确认, -1 = 写了但读不回 1)。
 *    与 VgaFix/HiResClockBits 完全同一条纪律(见 display.c 的 CrtSyncEnsure)。
 *   r131-marker: VxdCrtSyncFix (CR17h bit7, Sync Enable).
 *========================================================================*/
uint16_t __far __pascal VxdCrtMode17( void )
{
    return (uint16_t)idxread( 0x3D4, 0x3D5, 0x17 );
}

/* 样本里的 CR11h bit7 = 保护位, 用来自证 r131 那处寄存器号修正真的生效。 */
uint16_t __far __pascal VxdHwRegProtect( void )
{
    return (uint16_t)(s_hwRegCrtc[0x11] & 0x80);
}

/* 0 = SE 本来就是 1(不写); 1 = 读回 0 后补成 1 并读回确认; -1 = 写了但读不回 1。 */
int __far __pascal VxdCrtSyncFix( void )
{
    uint16_t v;

    v = (uint16_t)idxread( 0x3D4, 0x3D5, 0x17 );
    if (v & 0x80)
        return 0;                           /* 已经使能: 零写入 */

    outp8( 0x3D4, 0x17 );
    outp8( 0x3D5, (uint8_t)((v & 0xFF) | 0x80) );

    v = (uint16_t)idxread( 0x3D4, 0x3D5, 0x17 );
    return (v & 0x80) ? 1 : -1;
}


/*==========================================================================
 * [r133] VxdCrtRelatch -- re-establish the whole VGA mode with the display
 *        gate closed, and open it again LAST.
 *
 *   Why this step exists (evidence, not a guess):
 *     . ./129 ./131 host captures measure the visible frame-buffer columns at
 *       127..128 = CR01h + 1 (7Fh + 1) with the vertical geometry untouched:
 *       the raster collapsed to "characters counted as pixels" while the
 *       dot-clock multiplier was not applied;
 *     . every register self-check in the same sessions reports a correct
 *       geometry: CR00h = A3h, CR01h = 7Fh, CR17h = E3h (SE = 1), SR01h = 01h,
 *       CR11h bit7 = 0 -- and ./132's six snapshots repeat that for the whole
 *       session, including the ones taken while the screen was wrong;
 *     . a correct register file with a wrong raster has one explanation that
 *       needs no vendor knowledge: the display was LATCHED from an earlier
 *       state and never re-latched.  Writes re-latch; reads do not.
 *   Why this order is the general VGA answer:
 *     . FreeVGA, CRT Controller Registers, Index 17h bit7 (SE, Sync Enable):
 *       "when set to 0, this bit disables the horizontal and vertical retrace
 *       signals".  Programming the timing registers with SE = 0 and enabling
 *       the display last is what VGA BIOS mode tables have always done;
 *     . writing a register that already holds the wanted value is a no-op for
 *       any "re-latch when the value changes" implementation, so the LAST
 *       register we touch has to be one whose value really changes.  SE is
 *       driven 1 -> 0 -> 1 here, so the final write is always a change and it
 *       lands after every timing, clock and mode register has been written:
 *       whatever the earlier state was, the surviving latch is built from the
 *       complete, correct set.
 *   Every value written is the value our own known-good sample (VxdHwRegSave)
 *   holds.  Nothing here invents a register value, and nothing here touches
 *   the attribute controller, the DAC, the misc output or any chip-extended
 *   register, so this stays a generic-VGA step.
 *   Return: -1 = no usable sample (nothing written); otherwise the difference
 *           count read back afterwards (0 = the sample was re-established).
 *   Switch: system.ini [omninine] Relatch=0 turns it off without a rebuild.
 *   r133-marker: VxdCrtRelatch (gate closed, programmed, gate opened last).
 *========================================================================*/
int __far __pascal VxdCrtRelatch( void )
{
    uint16_t i;
    uint8_t  seOn;
    uint8_t  saved11;

    if (!s_hwRegOk)
        return -1;

    saved11 = s_hwRegCrtc[0x11];
    seOn    = (uint8_t)(s_hwRegCrtc[0x17] | 0x80);   /* SE = 1 at the end */

    /* 1. Protect off, so that CR00h..CR07h really take the writes below. */
    outp8( 0x3D4, 0x11 );
    outp8( 0x3D5, (uint8_t)(saved11 & 0x7F) );

    /* 2. Gate closed (SE = 0).  This is a real change, so everything from
     *    here on is re-latched step by step. */
    outp8( 0x3D4, 0x17 );
    outp8( 0x3D5, (uint8_t)(seOn & 0x7F) );

    /* 3. CRTC 00h..18h ascending (11h and 17h are written separately). */
    for (i = 0; i < HWREG_CRTC_N; i++)
    {
        if (i == 0x11 || i == 0x17)
            continue;
        outp8( 0x3D4, (uint8_t)i );
        outp8( 0x3D5, s_hwRegCrtc[i] );
    }

    /* 4. Sequencer 01h..04h.  SR01h: bit0 = 1 (8-dot clock), bit5 = 0
     *    ("screen off" clear).  SR00h is the reset register: never written. */
    for (i = 1; i < HWREG_SEQ_N; i++)
    {
        uint8_t v = s_hwRegSeq[i];

        if (i == 1)
            v = (uint8_t)((v | 0x01) & ~0x20);
        outp8( 0x3C4, (uint8_t)i );
        outp8( 0x3C5, v );
    }

    /* 5. Graphics controller 00h..08h. */
    for (i = 0; i < HWREG_GCR_N; i++)
    {
        outp8( 0x3CE, (uint8_t)i );
        outp8( 0x3CF, s_hwRegGcr[i] );
    }

    /* 5b. 【r137】属性控制器 AR10h..AR14h —— 与 VxdHwRegRestore 同一份值。
     *     两条路径写同一个集合, 否则"回放"与"重锁存"的终态又会不一样。 */
    if (s_hwRegArOk)
    {
        for (i = 0; i < HWREG_AR_N; i++)
            (void)VxdWriteAtcReg( (uint16_t)(0x10 + i), (uint16_t)s_hwRegAr[i] );
    }

    /* 6. Last timing write: the gate opens, and it opens after everything. */
    outp8( 0x3D4, 0x17 );
    outp8( 0x3D5, seOn );

    /* 7. Put the Protect bit back exactly as the sample had it, so that the
     *    state after a relatch is identical to the state after a replay. */
    outp8( 0x3D4, 0x11 );
    outp8( 0x3D5, saved11 );

    /* [r134] Same honest comparison as the replay: this path normalises SR01h
     * too, so it must be measured against the values it wrote. */
    return VxdHwRegDiffVsWritten();
}

/*==========================================================================
 * [r133] VxdRastProbe -- measure the ACTUAL scan-out from inside the guest.
 *
 *   Every instrument this project has built so far reads REGISTERS, and every
 *   one of them has reported a correct 1024x768 raster in sessions whose host
 *   capture shows 128 columns.  A register read cannot see a stale latch, so
 *   this probe reads the one signal that can: Input Status 1 (3DAh), whose
 *   bit0 (Display Enable) follows the raster as it is really being generated.
 *
 *   On the mode we asked for (hdisp = 1024, htotal = (CR00h + 5) * 8 = 1344)
 *   the duty cycle of that bit is hdisp/htotal = 76%.  If the raster really
 *   has collapsed to CR01h + 1 = 128 pixels, the same measurement gives
 *   128/1344 = 9.5%.  The two cannot be confused, and unlike the register
 *   reads this number does not care which implementation is underneath.
 *
 *   Bounds, deliberately: a fixed maximum number of reads and no waiting loop
 *   of any kind (3DAh is read-only, and our ATC read has read it safely since
 *   r123).  Nothing here writes a port.  If the machine does not advance its
 *   raster between two port reads the sample comes back constant; the caller
 *   reports that as "inconclusive" instead of dressing it up as a measurement,
 *   and VxdRastVbl() -- how many samples sat inside vertical retrace -- says
 *   whether the sweep covered any part of a frame at all.
 *   r133-marker: VxdRastProbe (display-enable duty cycle).
 *========================================================================*/
#define RAST_MAX 20000
static uint16_t s_rastN   = 0;
static uint16_t s_rastDe1 = 0;
static uint16_t s_rastTr  = 0;
static uint16_t s_rastVb  = 0;

uint16_t __far __pascal VxdRastRun( void )
{
    uint16_t i;
    uint8_t  v;
    uint8_t  prev;

    s_rastN = s_rastDe1 = s_rastTr = s_rastVb = 0;

    prev = (uint8_t)(inp8( 0x3DA ) & 0x01);
    for (i = 0; i < RAST_MAX; i++)
    {
        v = (uint8_t)(inp8( 0x3DA ) & 0x09);        /* bit0 = DE, bit3 = VSYNC */

        s_rastN++;
        if (v & 0x01)
            s_rastDe1++;
        if ((uint8_t)(v & 0x01) != prev)
        {
            s_rastTr++;
            prev = (uint8_t)(v & 0x01);
        }
        if (v & 0x08)
            s_rastVb++;
    }
    return s_rastN;
}

uint16_t __far __pascal VxdRastDe1( void )    { return s_rastDe1; }
uint16_t __far __pascal VxdRastTrans( void )  { return s_rastTr; }
uint16_t __far __pascal VxdRastVbl( void )    { return s_rastVb; }

/* Duty cycle of "display enable" in per mille (0..1000); 0xFFFF = no sample.
 * 1024/1344 = 762; 128/1344 = 95. */
uint16_t __far __pascal VxdRastPermille( void )
{
    uint32_t pm;

    if (s_rastN == 0)
        return 0xFFFF;
    pm = ((uint32_t)s_rastDe1 * 1000UL) / (uint32_t)s_rastN;
    if (pm > 1000UL)
        pm = 1000UL;
    return (uint16_t)pm;
}

/*==========================================================================
 * [r132] VxdCrtProtectClear -- clear CR11h bit7 before touching the timing
 *        registers.  This is the general VGA rule, not an emulator quirk:
 *
 *   FreeVGA, "CRT Controller Registers", Index 11h (Vertical Retrace End):
 *     bit7 = Protect -- "when this field is set to 1, the CRTC register
 *     indexes 00h-07h ignore write access, with the exception of bit 4 of the
 *     Overflow Register (07h), which holds bit 8 of the Line Compare field."
 *   So while Protect is set:
 *     . CR00h..CR06h writes are dropped outright -- horizontal total, end
 *       horizontal display, blanking start/end and sync positions are frozen;
 *     . CR07h keeps everything except bit4 -- and bit1/bit6 of CR07h are the
 *       8th/9th bits of the vertical display end (VDE8/VDE9), so the vertical
 *       geometry is frozen as well.
 *   Standard VGA text mode leaves CR11h = 8Eh (Protect = 1), therefore every
 *   "back to Windows hi-res" path must clear it first.  Our 4F02h goes to the
 *   card's VBE BIOS: with Protect set, the BIOS's whole CR00h..CR07h group is
 *   discarded while 4F02h still returns 004F ("the BIOS says fine") -- which is
 *   exactly why the desktop can come back as a narrow strip / small black
 *   screen while every self-check in the log reports OK.
 *
 *   Return: 0 = was already clear (zero writes), 1 = cleared + verified,
 *          -1 = wrote it but bit7 read back as 1 (port not writable).
 *   The bit is deliberately left clear: after returning to hi-res the only
 *   writers of these registers are ourselves, the main VDD and the card BIOS.
 *   Switch: system.ini [omninine] ProtClr=0 disables this step without a rebuild.
 *   r132-marker: VxdCrtProtectClear (CRTC Protect bit, index 11h).
 *========================================================================*/
int __far __pascal VxdCrtProtectClear( void )
{
    uint16_t v;

    v = (uint16_t)idxread( 0x3D4, 0x3D5, 0x11 );
    if (!(v & 0x80))
        return 0;                           /* already clear: zero writes */

    outp8( 0x3D4, 0x11 );
    outp8( 0x3D5, (uint8_t)(v & 0x7F) );    /* clear bit7 only; the low nibble
                                             * (Vertical Retrace End) and
                                             * bit5/bit6 stay untouched */

    v = (uint16_t)idxread( 0x3D4, 0x3D5, 0x11 );
    return (v & 0x80) ? -1 : 1;
}

/* 当前值 vs 存下的样本的差异字节数; -1 = 没有样本。 */
int __far __pascal VxdHwRegDiff( void )
{
    uint16_t i, n = 0;

    if (!s_hwRegOk)
        return -1;
    for (i = 0; i < HWREG_CRTC_N; i++)
        if (idxread( 0x3D4, 0x3D5, (uint8_t)i ) != s_hwRegCrtc[i])
            n++;
    for (i = 1; i < HWREG_SEQ_N; i++)          /* SR00 不回放, 也不比 */
        if (idxread( 0x3C4, 0x3C5, (uint8_t)i ) != s_hwRegSeq[i])
            n++;
    for (i = 0; i < HWREG_GCR_N; i++)
        if (idxread( 0x3CE, 0x3CF, (uint8_t)i ) != s_hwRegGcr[i])
            n++;
    return (int)n;
}

/* [r132] replay.  Returns the number of differing bytes after writing the
 * sample back (0 = byte-for-byte identical), -1 = no usable sample (caller
 * falls back).  Order: clear CRTC Protect -> CRTC -> sequencer -> GCR ->
 * restore CRTC Protect.
 *
 * [r132 fix of an r131 self-inflicted bug]
 *   r131 changed the register number here from 11h to 0Bh and claimed in the
 *   comment that "s_hwRegCrtc[0x11] is CRTC 17h".  That is wrong twice over:
 *     . a CRTC register's index IS its number, so CR11h is index 11h and
 *       CR17h is index 17h;
 *     . s_hwRegCrtc[] is filled by index (VxdHwRegSave uses the same i), so
 *       s_hwRegCrtc[0x11] always was CR11h.
 *   The effect was that r131 wrote the CR11h value into CR0B (cursor start)
 *   and never cleared the Protect bit at all -- the replay's own "clear
 *   protect first" behaviour silently disappeared.
 *   r132 restores index 11h; VxdCrtProtectClear() does the same thing in front
 *   of every mode set.
 *   r132-marker: VxdHwRegRestore protects via index 11h.
 *                                                                    */
int __far __pascal VxdHwRegRestore( void )
{
    uint16_t i;
    uint8_t  saved11;

    if (!s_hwRegOk)
        return -1;

    saved11 = s_hwRegCrtc[0x11];                    /* CR11h 才是保护位 */
    outp8( 0x3D4, 0x11 );
    outp8( 0x3D5, (uint8_t)(saved11 & 0x7F) );      /* 清保护位(CR11h bit7) */

    for (i = 0; i < HWREG_CRTC_N; i++)
    {
        if (i == 0x11)
            continue;                               /* 最后单独恢复 */
        outp8( 0x3D4, (uint8_t)i );
        outp8( 0x3D5, s_hwRegCrtc[i] );
    }
    outp8( 0x3D4, 0x11 );
    outp8( 0x3D5, saved11 );                        /* 恢复保护位原值 */

    for (i = 1; i < HWREG_SEQ_N; i++)               /* SR00 跳过(复位寄存器) */
    {
        uint8_t v = s_hwRegSeq[i];

        /* 【r128】SR01 归一化 —— 见 VxdHwRegSaveClocking 的长注。
         * ./127 实测样本存成了 0x0002(9 点时钟, 文本时序)。把 9 点当"已知
         * 良好"回放回去, 就是在往错的方向修, 而且会掩盖真正的偏差。
         * 所以回放 SR01 时把 bit0 置 1(8 点) —— 这是 VGA 标准对【所有】
         * 高分辨率打包像素/线性模式的要求, 不需要知道具体是哪张卡。
         * 【为什么可以安全地改】2/4 平面模式下这一位由 VBE BIOS 自己按模式设,
         * 我们回放的只是 SR02..SR04; SR01 只服务"线性/打包像素"这一类模式,
         * 而那正是本回放存在的前提(见 HwRegLooksOurs 的三个门槛位)。
         * 这一步【从不静默】: g_hwRegNorm 记账, 日志里有专门一行。      */
        if (i == 1)
        {
            /* [r134] The replay now applies exactly the value HwRegWrittenVal()
             * promises the caller: bit0 = 1 (8-dot clock) and bit5 = 0 (screen
             * on).  The counter keeps its r128 meaning (how often the sample
             * itself was stored as 9-dot); clearing "screen off" is not a
             * normalisation of the sample's intent, it is what a restore means.*/
            if (!(v & 0x01))
                g_hwRegNorm++;
            v = (uint8_t)((v | 0x01) & ~0x20);
        }
        outp8( 0x3C4, (uint8_t)i );
        outp8( 0x3C5, v );
    }
    for (i = 0; i < HWREG_GCR_N; i++)
    {
        outp8( 0x3CE, (uint8_t)i );
        outp8( 0x3CF, s_hwRegGcr[i] );
    }

    /* 【r137】属性控制器 AR10h..AR14h —— 这一段是"盒子走后卡上留在文本
     *   形状"的那一半。r136 及以前这里什么都不写, 于是 ./136 里 AR10h
     *   一直停在 000Dh(文本形状 | bit0), 而屏幕一直是竖条纹。
     *   写的是【样本里的整字节】, 不是 `读回值 | 某一位` —— 后者正是把它
     *   变成 000Dh 的那种做法(见 minivdd.asm 的 r137 注)。            */
    if (s_hwRegOk && s_hwRegArOk)
    {
        for (i = 0; i < HWREG_AR_N; i++)
            (void)VxdWriteAtcReg( (uint16_t)(0x10 + i), (uint16_t)s_hwRegAr[i] );
    }

    /* [r134] SR01h was written normalised (8-dot clock, screen on), so the
     * read-back test must compare against THAT value.  Comparing against the
     * raw sample turned a deliberate normalisation into a "mismatch" -- and
     * that single byte is what made the official path veto itself. */
    return VxdHwRegDiffVsWritten();
}

/*==========================================================================
 * [r134] The official-path verdict, split into evidence and meaning.
 *
 *   Why the split (./133's own ledger):
 *       CbRestore: callbacks seen=0006  inline ok=0001  inline failed=0005
 *       CbRestore: hwreg replay diff last=0001
 *   The single "ok" is the callback that had no sample at all (d < 0), so the
 *   register replay -- the one half of the official path we can do inside the
 *   VDD callback without nesting a BIOS call -- was judged FAILED every single
 *   time by a one-byte read-back difference.  A byte count cannot answer the
 *   question the caller is asking ("is the hardware in our mode?"), so it now
 *   stays as evidence and the answer comes from the bits that decide whether
 *   the picture can be right at all.
 *   All three functions below are read-only; none of them writes a port.
 *   r134-marker: VxdHwRegDiffVsWritten / VxdHwRegEssentialsOk / VxdHwRegMismatch.
 *========================================================================*/

/* The value the replay really leaves in register (grp,idx).
 *   grp: 0 = CRTC, 1 = sequencer, 2 = GCR.  SR00h is never written. */
static uint8_t HwRegWrittenVal( uint16_t grp, uint16_t idx )
{
    if (grp == 1 && idx == 1)
    {
        uint8_t v = s_hwRegSeq[1];

        /* 8-dot clock (bit0 = 1) and screen on (bit5 = 0): the two bits the
         * replay normalises.  Both are required of every packed-pixel/linear
         * mode, so normalising them is not a vendor assumption. */
        return (uint8_t)((v | 0x01) & ~0x20);
    }
    if (grp == 0)
        return s_hwRegCrtc[idx];
    if (grp == 1)
        return s_hwRegSeq[idx];
    if (grp == 3)                               /* r137: AR10h..AR14h */
        return s_hwRegAr[idx];
    return s_hwRegGcr[idx];
}

int __far __pascal VxdHwRegDiffVsWritten( void )
{
    uint16_t i;
    int      n = 0;

    if (!s_hwRegOk)
        return -1;

    for (i = 0; i < HWREG_CRTC_N; i++)
        if (idxread( 0x3D4, 0x3D5, (uint8_t)i ) != HwRegWrittenVal( 0, i ))
            n++;
    for (i = 1; i < HWREG_SEQ_N; i++)       /* SR00h = reset register: skipped */
        if (idxread( 0x3C4, 0x3C5, (uint8_t)i ) != HwRegWrittenVal( 1, i ))
            n++;
    for (i = 0; i < HWREG_GCR_N; i++)
        if (idxread( 0x3CE, 0x3CF, (uint8_t)i ) != HwRegWrittenVal( 2, i ))
            n++;
    /* 【r137】AR10h..AR14h —— ./136 里花屏态与干净态之间变化的那一个寄存器
     *   就在这一组里。不数它, 差异计数就永远看不见它。 */
    if (s_hwRegArOk)
    {
        for (i = 0; i < HWREG_AR_N; i++)
            if (AtcReadIdx( (uint8_t)(0x10 + i) ) != HwRegWrittenVal( 3, i ))
                n++;
    }

    return n;
}

int __far __pascal VxdHwRegEssentialsOk( void )
{
    uint8_t cr00, cr01, cr13, cr17, cr11, sr01, gcr06, ar10;

    if (!s_hwRegOk)
        return 0;

    cr00  = idxread( 0x3D4, 0x3D5, 0x00 );
    cr01  = idxread( 0x3D4, 0x3D5, 0x01 );
    cr13  = idxread( 0x3D4, 0x3D5, 0x13 );
    cr17  = idxread( 0x3D4, 0x3D5, 0x17 );
    cr11  = idxread( 0x3D4, 0x3D5, 0x11 );
    sr01  = idxread( 0x3C4, 0x3C5, 0x01 );
    gcr06 = idxread( 0x3CE, 0x3CF, 0x06 );
    ar10  = (s_hwRegArOk) ? AtcReadIdx( 0x10 ) : s_hwRegAr[0];

    /* Geometry: the two horizontal registers, plus the row offset.  These are
     * the values ./129 and ./131 measured as "collapsed to CR01h + 1 = 128
     * columns" while a stale latch was in force -- a disagreement here is
     * exactly the case that must still veto. */
    if (cr00 != s_hwRegCrtc[0x00] || cr01 != s_hwRegCrtc[0x01] ||
        cr13 != s_hwRegCrtc[0x13])
        return 0;
    /* Sync Enable: FreeVGA (CRTC 17h bit7) -- with SE = 0 the horizontal and
     * vertical retrace signals are disabled, i.e. no raster. */
    if (!(cr17 & 0x80))
        return 0;
    /* Protect: while CR11h bit7 is set, CR00h..CR06h ignore writes. */
    if (cr11 & 0x80)
        return 0;
    /* SR01h bit0 = 8-dot character clock, bit5 = screen off (must be clear). */
    if (!(sr01 & 0x01) || (sr01 & 0x20))
        return 0;
    /* GCR06h bit0 = graphics mode (1) vs alphanumeric (0). */
    if (!(gcr06 & 0x01))
        return 0;
    /* 【r137】AR10h —— "文本还是图形"的第二半(GCR06h bit0 只是第一半),
     *   外加 palette address source。./136 的现场读数把这两个位钉死了:
     *       干净态 ATC10 = 0041h   -> bit0 = 1, bit6 = 1
     *       花屏态 ATC10 = 000Dh   -> bit0 = 1, bit6 = 0
     *   也就是说, r136 为止这一整套"硬件是不是我们的模式"的语义判据【从头
     *   到尾没有看过 bit6】, 而它恰好是那两次读数之间唯一一个"本该为 1"
     *   却变成 0 的位。补上它, 这个判据才第一次能区分那两种状态。
     *   AR 读不到(s_hwRegArOk=0)时不做这一问 —— 读不到不是证据。      */
    if (s_hwRegArOk && (!(ar10 & 0x01) || !(ar10 & 0x40)))
        return 0;

    return 1;
}

/* The n-th register whose live value differs from the value the replay wrote,
 * packed as (group << 8) | index; 0xFFFF = there is no such mismatch.
 *   Scalar interface on purpose (see drv.h): a pointer parameter here would mix
 *   near/far pointer sizes in this memory model and wcc would warn W112. */
static uint16_t HwRegMismatchEntry( int n )
{
    uint16_t i;
    int      k = 0;

    for (i = 0; i < HWREG_CRTC_N; i++)
        if (idxread( 0x3D4, 0x3D5, (uint8_t)i ) != HwRegWrittenVal( 0, i ))
        {
            if (k == n) return i;                       /* group 0 = CRTC */
            k++;
        }
    for (i = 1; i < HWREG_SEQ_N; i++)
        if (idxread( 0x3C4, 0x3C5, (uint8_t)i ) != HwRegWrittenVal( 1, i ))
        {
            if (k == n) return (uint16_t)(0x0100 | i);  /* group 1 = sequencer */
            k++;
        }
    for (i = 0; i < HWREG_GCR_N; i++)
        if (idxread( 0x3CE, 0x3CF, (uint8_t)i ) != HwRegWrittenVal( 2, i ))
        {
            if (k == n) return (uint16_t)(0x0219 | i);  /* group 2 = GCR */
            k++;
        }
    /* 【r137】group 3 = 属性控制器 AR10h..AR14h(索引就是 10h+i)。 */
    if (s_hwRegArOk)
    {
        for (i = 0; i < HWREG_AR_N; i++)
            if (AtcReadIdx( (uint8_t)(0x10 + i) ) != HwRegWrittenVal( 3, i ))
            {
                if (k == n) return (uint16_t)(0x0300 | (0x10 + i));
                k++;
            }
    }

    return 0xFFFF;                  /* no more mismatches */
}

uint16_t __far __pascal VxdHwRegMismatchInfo( int n )
{
    if (!s_hwRegOk)
        return 0xFFFE;
    return HwRegMismatchEntry( n );
}

uint16_t __far __pascal VxdHwRegMismatchVal( int n, int which )
{
    uint16_t e;

    if (!s_hwRegOk)
        return 0xFFFF;
    e = HwRegMismatchEntry( n );
    if (e == 0xFFFF)
        return 0xFFFF;
    if (which)
        return (uint16_t)HwRegWrittenVal( (uint16_t)(e >> 8), (uint16_t)(e & 0xFF) );

    if ((e >> 8) == 0)
        return (uint16_t)idxread( 0x3D4, 0x3D5, (uint8_t)(e & 0xFF) );
    if ((e >> 8) == 1)
        return (uint16_t)idxread( 0x3C4, 0x3C5, (uint8_t)(e & 0xFF) );
    if ((e >> 8) == 3)                          /* r137: AR10h..AR14h */
        return (uint16_t)AtcReadIdx( (uint8_t)(e & 0xFF) );

    return (uint16_t)idxread( 0x3CE, 0x3CF, (uint8_t)(e & 0xFF) );
}

/* 【r127】VxdHwRegDiffDetail —— 只列【不同】的那几项, 逐字节点名。
 *
 * 【为什么要它】VxdHwRegDiff 只给一个数字(差异字节数)。而 ./123 那一轮
 *   最需要知道的恰恰是"哪几个字节不同" —— 上一轮因为 CRTP 把 diff 写成了
 *   常量, 连这个数字都没拿到, 更别说点名。全量转储(HwReg=2)是 117 行/次,
 *   在这里太重; 所以补一个"只打印不同项"的形式:
 *       HwReg diff idx=nn now=xx sav=yy
 *   零差异时打一行 "HwReg diff: none" —— 这本身就是结论。
 * 【只在 HwReg 有可用样本时才有意义】没有样本(s_hwRegOk==0)时直接说明。
 * 返回不同项数(与 VxdHwRegDiff 同源, 便于交叉验证)。
 *========================================================================*/
int __far __pascal VxdHwRegDiffDetail( const char *why )
{
    uint16_t i;
    uint16_t n = 0;

    (void)why;
    DIAG( why );
    if (!s_hwRegOk)
    {
        DIAG( "HwReg diff: no usable sample (s_hwRegOk=0)" );
        FlushLog();
        return -1;
    }

    for (i = 0; i < HWREG_CRTC_N; i++)
    {
        uint8_t now = idxread( 0x3D4, 0x3D5, (uint8_t)i );

        if (now != s_hwRegCrtc[i])
        {
            DiagHex( "HwReg diff crtc idx", i );
            DiagHex( "HwReg diff crtc now", (uint16_t)now );
            DiagHex( "HwReg diff crtc sav", s_hwRegCrtc[i] );
            n++;
        }
    }
    for (i = 1; i < HWREG_SEQ_N; i++)          /* SR00 不比(见回放注) */
    {
        uint8_t now = idxread( 0x3C4, 0x3C5, (uint8_t)i );

        if (now != s_hwRegSeq[i])
        {
            DiagHex( "HwReg diff seq idx", i );
            DiagHex( "HwReg diff seq now", (uint16_t)now );
            DiagHex( "HwReg diff seq sav", s_hwRegSeq[i] );
            n++;
        }
    }
    for (i = 0; i < HWREG_GCR_N; i++)
    {
        uint8_t now = idxread( 0x3CE, 0x3CF, (uint8_t)i );

        if (now != s_hwRegGcr[i])
        {
            DiagHex( "HwReg diff gcr idx", i );
            DiagHex( "HwReg diff gcr now", (uint16_t)now );
            DiagHex( "HwReg diff gcr sav", s_hwRegGcr[i] );
            n++;
        }
    }
    /* 【r137】属性控制器: 逐项点名, 与上面三组同格式。 */
    if (s_hwRegArOk)
    {
        for (i = 0; i < HWREG_AR_N; i++)
        {
            uint8_t now = AtcReadIdx( (uint8_t)(0x10 + i) );

            if (now != s_hwRegAr[i])
            {
                DiagHex( "HwReg diff ar idx", (uint16_t)(0x10 + i) );
                DiagHex( "HwReg diff ar now", (uint16_t)now );
                DiagHex( "HwReg diff ar sav", s_hwRegAr[i] );
                PHASEV( "PH: hwreg diff ar=", (uint16_t)now );   /* r137: unconditional channel */
                n++;
            }
        }
    }
    if (n == 0)
        DIAG( "HwReg diff: none (all compared bytes match the sample)" );
    DiagHex( "HwReg diff total", n );
    FlushLog();
    return (int)n;
}

int __far __pascal VxdHwRegDump( const char *why )
{
    int d = VxdHwRegDiff();
    uint16_t i;

    (void)why;
    DIAG( why );
    DiagHex( "HwReg diff (0=same)", (uint16_t)((d < 0) ? 0xEEEE : d) );
    DiagHex( "HwReg sample ok", (uint16_t)s_hwRegOk );
    DiagHex( "HwReg gate bits (8dot,ch4off,word)",
             (uint16_t)(((s_hwRegSeq[1] & 0x01) ? 1 : 0) |
                        ((s_hwRegSeq[4] & 0x08) ? 2 : 0) |
                        ((s_hwRegCrtc[0x17] & 0x40) ? 4 : 0)) );
    for (i = 0; i < HWREG_CRTC_N; i++)
    {
        /* 【r127】先读成语句再打印 —— 见 PortProbeRead 里那段说明:
         * 写在 DiagHex 实参里的端口读在发布构建里【根本不存在】。   */
        uint8_t now = idxread( 0x3D4, 0x3D5, (uint8_t)i );

        DiagHex( "HwReg crtc idx", i );
        DiagHex( "HwReg crtc now", (uint16_t)now );
        DiagHex( "HwReg crtc sav", s_hwRegCrtc[i] );
    }
    for (i = 0; i < HWREG_SEQ_N; i++)
    {
        uint8_t now = idxread( 0x3C4, 0x3C5, (uint8_t)i );

        DiagHex( "HwReg seq idx", i );
        DiagHex( "HwReg seq now", (uint16_t)now );
        DiagHex( "HwReg seq sav", s_hwRegSeq[i] );
    }
    for (i = 0; i < HWREG_GCR_N; i++)
    {
        uint8_t now = idxread( 0x3CE, 0x3CF, (uint8_t)i );

        DiagHex( "HwReg gcr idx", i );
        DiagHex( "HwReg gcr now", (uint16_t)now );
        DiagHex( "HwReg gcr sav", s_hwRegGcr[i] );
    }
    /* 【r137】属性控制器那五个 + "这一段到底读到了没有"。 */
    DiagHex( "HwReg ar ok (1=readable)", (uint16_t)s_hwRegArOk );
    for (i = 0; i < HWREG_AR_N; i++)
    {
        DiagHex( "HwReg ar idx", (uint16_t)(0x10 + i) );
        DiagHex( "HwReg ar sav", (uint16_t)s_hwRegAr[i] );
        PHASEV( "PH: hwreg ar sav=", (uint16_t)s_hwRegAr[i] );   /* r137 */
    }
    /* 【r128】把"样本本身对不对"直接打在日志里 —— ./127 的 r1 是靠
     * HwReg diff detail 那一行才看出 sav=0002(9 点)的, 那是绕了一圈。
     * 现在每次采样/转储都带这两行, 一眼可判。                        */
    DiagHex( "HwReg sample SR01 (bit0: 1=8dot, 0=9dot)", (uint16_t)s_hwRegSeq[1] );
    DiagHex( "HwReg replay normalized 9dot->8dot count", g_hwRegNorm );
    FlushLog();
    return d;
}

/* 【r137】VxdHwRegArOk —— 只读判据: AR10h/AR11h/AR12h/AR13h/AR14h 现在
 *   是否等于样本。没有样本(或 AR 读不到)时返回 1(不是证据, 不否决)。
 *   这是给 display.c 的入口/收尾边沿用的"硬件的模式状态是不是我们的"里
 *   补上的那一半 —— 原来那一半(CRTC/SEQ/GCR)一直有, 属性控制器一直没有。*/
int __far __pascal VxdHwRegArOk( void )
{
    uint16_t i;

    if (!s_hwRegOk || !s_hwRegArOk)
        return 1;
    for (i = 0; i < HWREG_AR_N; i++)
        if (AtcReadIdx( (uint8_t)(0x10 + i) ) != s_hwRegAr[i] )
            return 0;
    return 1;
}

/*==========================================================================
 *  【r109】PortProbeRead —— 直接读 VGA/VBE 寄存器, 拿【硬件真相】。
 *
 *  【为什么必须要有它】./108 的 t2/t3 把问题钉死了: 同一时刻
 *    · 虚拟机内截图(走 GDI 读显存)= 完好桌面;
 *    · 主机截图 = 桌面被"2 像素周期"地重解出来的条纹, 且 DOS 盒的黑区
 *      从第 278 行开始(guest 是 136 行, 正好 2 倍)。
 *  两者合起来说明显存没错、错的是【光栅/像素格式】。可是驱动侧所有探针
 *  都读不到它:
 *    · VerifyDisplayMode 的两条判据全是自证(VxD 软件状态 + 帧缓冲回读);
 *    · INT 10h(AH=0Fh)在 Windows VM 里被主 VDD 的虚拟 BIOS 接走, 回的是
 *      主 VDD 的模式记账(见 display.c 里 HwProbe 的 r108 注), 不是硬件。
 *  唯一能读硬件的地方就是 VGA 寄存器本身 —— 显示驱动在 ring 3 读写这些
 *  端口是 DDK 时代的标准做法(本文件下面的 DAC_SetPalette 已经在写 3C8h/
 *  3C9h), 只是本驱动从未做过"读"。
 *
 *  【读哪几个, 为什么】
 *    CRTC(3D4h/3D5h) —— 光栅几何与扫描行长度都在这里:
 *      00h Horizontal Total      01h Horizontal Display End
 *      12h Vertical Display End  13h Offset(= 每扫描行字节数的编码值)
 *      0Ch/0Dh Start Address 高/低
 *      14h Underline Location(bit6 = 双字寻址)
 *      17h Mode Control(bit6 = 字节/字模式, 决定 13h 的单位)
 *    ⇒ 拿开机基线(此刻硬件确实在 1024x768x16bpp、pitch=0x800)对一遍,
 *      就能把"13h 的编码单位"标定出来; 之后每次边沿再读, 一眼看出
 *      主 VDD/DOS 盒到底把 pitch 与起址改成了什么。
 *    时序器(3C4h/3C5h) —— 01h Clocking Mode(bit0 8 点时钟, bit5 屏幕关),
 *      04h Memory Mode(bit2 奇偶, bit3 chain4) ⇒ 8bpp/16bpp 的分野。
 *    GCR(3CEh/3CFh) —— 05h Mode(bit6 256 色移位, bit4 奇偶),
 *      06h Miscellaneous(bits3-2 显存映射 A0000/B0000/B8000) ⇒ 文本模式
 *      与图形模式的直接证据。
 *    3CCh Miscellaneous Output(只读) / 3C6h DAC 像素掩码(只读)。
 *    【故意不读】3DAh(读它会复位属性控制器的翻转触发器)、3C0h/3C1h
 *      (属性控制器, 读之前要先同步翻转触发器)、3C7h/3C8h/3C9h(读会自增
 *      DAC 索引, 有副作用)。
 *
 *  【风险与开关】端口读在"驱动已接管屏幕"之后才可能直达硬件(VDD_StopIOTrap
 *  之后主 VDD 不再虚拟化系统 VM 的显示端口, 见 DAC_SetPalette 头注); 万一
 *  某个端口在本机不被允许, 那条 in 指令就是一次 0D。所以本函数挂在
 *  system.ini [omninine] PortProbe=1 后面(默认 0): 默认配置一个端口都不读,
 *  要证它就必须单独跑一次 —— 这是本轮唯一的新变量之一。
 *========================================================================*/
/* 【r110】PortGrp —— 端口读的【分组位图】。见 r110 的 ./109 t3 读数:
 * 开了 PortProbe 的那次开机, 桌面没起来(实机 640x480 的空白青底 + 光标,
 * 2 行文本残留)且随后整机冻结在 DIB_DibToDevice。端口读本身回了完全
 * 合理的值(13h=0x80/17h=0xE3 与 1024x768x16bpp、pitch=0x800 自洽), 所以
 * "读到了什么"没问题; 有问题的是"读哪些"里可能混着有副作用的端口 ——
 * 3C6h(DAC 像素掩码)与 3CCh(Misc Output)在标准 VGA 上是只读的, 但
 * 这两个是 RAMDAC/时钟合成器侧的寄存器, 各家实现的自副作用(翻转触发器、
 * 内部索引)最不可控, 而它们对"当前光栅几何"这一问毫无贡献。
 * 于是 r110 把端口读切成四组, 默认只读前三组(几何与像素格式), 第四组
 * 必须显式打开:
 *   1 = CRTC(3D4h/3D5h 索引+数据, 纯索引写 + 纯数据读)
 *   2 = 时序器(3C4h/3C5h)
 *   4 = GCR(3CEh/3CFh)
 *   8 = 3CCh(Misc Out) + 3C6h(DAC 像素掩码) —— 默认【关】
 * 另外 r110 起, 端口读只发生在"重建前/重建后"这一对位置(display.c),
 * 不再在开机基线与每次状态边沿上读 —— 少一次读就少一次未知。      */
uint16_t g_runPortGrp = 7;              /* INI PortGrp, 默认 7 = CRTC|seq|GCR */

/* r109-marker: PortProbeRead reads the real CRTC/sequencer/GCR state */
void __far PortProbeRead(const char *why)
{
    /* 【r127 修 debug/release 不一致】
     * 下面这些端口读原来是直接写在 DiagHex 的实参里的, 而发布构建里
     *     #define DiagHex( m, v )  ((void)0)
     * 【不展开实参】⇒ release 里那些 in 指令根本不存在, 于是同一个
     * PortProbe=1 在 debug 下真的读端口、在 release 下一个都不读。
     * 现在统统先提成语句取进局部变量, 再交给日志宏打印 —— 无论 DiagHex
     * 是不是空宏, "读了什么"都完全一致。                              */
    WORD vHtotal = 0, vHdispe = 0, vVdispe = 0, vOffset = 0;
    WORD vStartHi = 0, vStartLo = 0, vUnderln = 0, vMode17 = 0;
    WORD vClockng = 0, vMemmode = 0, vMode05 = 0, vMisc06 = 0;
    WORD vMisc3CC = 0, vDac3C6 = 0;

    DIAG( why );
    DiagMark( "PORT" );
    if (g_runPortGrp & 1)
    {
        vHtotal  = idxread(0x3D4, 0x3D5, 0x00);
        vHdispe  = idxread(0x3D4, 0x3D5, 0x01);
        vVdispe  = idxread(0x3D4, 0x3D5, 0x12);
        vOffset  = idxread(0x3D4, 0x3D5, 0x13);
        vStartHi = idxread(0x3D4, 0x3D5, 0x0C);
        vStartLo = idxread(0x3D4, 0x3D5, 0x0D);
        vUnderln = idxread(0x3D4, 0x3D5, 0x14);
        vMode17  = idxread(0x3D4, 0x3D5, 0x17);
    }
    if (g_runPortGrp & 2)
    {
        vClockng = idxread(0x3C4, 0x3C5, 0x01);
        vMemmode = idxread(0x3C4, 0x3C5, 0x04);
    }
    if (g_runPortGrp & 4)
    {
        vMode05  = idxread(0x3CE, 0x3CF, 0x05);
        vMisc06  = idxread(0x3CE, 0x3CF, 0x06);
    }
    if (g_runPortGrp & 8)
    {
        vMisc3CC = inp8(0x3CC);
        vDac3C6  = inp8(0x3C6);
    }

    DiagHex( "PORT grp mask    ", g_runPortGrp );
    DiagHex( "PORT crtc Htotal ", vHtotal );
    DiagHex( "PORT crtc Hdispe ", vHdispe );
    DiagHex( "PORT crtc Vdispe ", vVdispe );
    DiagHex( "PORT crtc offset ", vOffset );
    DiagHex( "PORT crtc starthi", vStartHi );
    DiagHex( "PORT crtc startlo", vStartLo );
    DiagHex( "PORT crtc underln", vUnderln );
    DiagHex( "PORT crtc mode17 ", vMode17 );
    DiagHex( "PORT seq  clockng", vClockng );
    DiagHex( "PORT seq  memmode ", vMemmode );
    DiagHex( "PORT gcr  mode05  ", vMode05 );
    DiagHex( "PORT gcr  misc06  ", vMisc06 );
    DiagHex( "PORT misc 3CC     ", vMisc3CC );
    DiagHex( "PORT dac  3C6 mask", vDac3C6 );
    FlushLog();
}

/*==========================================================================
 * 【r126】VxdCrtProbeRead —— 借出一次"索引 + 读数据"的端口读给 display.c。
 *
 * 【为什么要有它】display.c 的 CRTP 现场读数要在"屏幕正花着"的那一刻把
 *   光栅几何原样落盘, 用来分开两种此前无法区分的可能(见 display.c 里
 *   CrtProbeNow 的历史长注(该函数已于 r407 删除), r126 实测把问题逼成了这个二选一):
 *     A. CRTC 真的停在错误的值上  ⇒ 病灶在硬件寄存器, 我们能自己修;
 *     B. CRTC 与已知良好样本逐字节相同而屏幕还是花的 ⇒ 病灶在"VDD 把谁的
 *        画面送到 DAC"这一层, 寄存器回放先天无效。
 *   display.c 里没有端口读的原语(那是本文件的一个 static), 与其在那边再
 *   写一份 in/out, 不如把【同一个 idxread】借出去 —— 读的全是
 *   PortProbeRead / HwRegSave 已经在真机上读过千百次的那些索引。
 *
 * 【选择子编码】高半字节选组, 低字节索引(见 drv.h 的声明处):
 *   0x0nn CRTC(3D4h/3D5h)   0x1nn 时序器(3C4h/3C5h)   0x2nn GCR(3CEh/3CFh)
 *   其它值一律返回 0xFFFF 并且【一个端口都不碰】—— 让"选择子写错"表现为
 *   一个不可能是硬件读数的值, 而不是去读一个随机端口(那可能是一次 0D)。
 * 【安全边界】与 PortProbeRead 逐条相同: 只读这几个索引, 绝不碰 3DAh /
 *   3C0h / 3C7h / 3C8h(前两个动属性控制器翻转触发器, 后两个动 DAC 索引)。
 * 【返回值】正常 = 读到的字节(0x00..0xFF); 0xFFFF = 选择子非法。
 *   端口后面没有设备时硬件本身回 0xFF —— 与 0xFFFF 区分得开。
 *========================================================================*/
uint16_t __far VxdCrtProbeRead(uint16_t sel)
{
    uint8_t idx = (uint8_t)(sel & 0xFF);

    switch (sel >> 8)
    {
    case 0:  return (uint16_t)idxread( 0x3D4, 0x3D5, idx );
    case 1:  return (uint16_t)idxread( 0x3C4, 0x3C5, idx );
    case 2:  return (uint16_t)idxread( 0x3CE, 0x3CF, idx );
    case 3:  return VxdReadAtcReg( idx );   /* r127: 属性控制器 3C0h/3C1h */
    case 4:  return VxdReadMiscOut();       /* r127: Misc Output 3CCh     */
    default: return 0xFFFF;
    }
}

/*==========================================================================
 * 【r127】点时钟归位 —— "文本模式会话结束后把时钟恢复成 8 点"
 *
 * 【为什么这是【通用】要求, 不是为某张卡/某个模拟器做的】
 *   VGA 标准的时序器 01h(Clocking Mode Register):
 *       bit0 = 0  ->  字符时钟 9 点/字符 (80 列文本模式的 720 像素宽)
 *       bit0 = 1  ->  字符时钟 8 点/字符 (640 像素宽, 高分辨率图形模式)
 *   9 点字符是【文本模式的历史产物】: 80 列 × 9 点 = 720 点。所有高分辨率
 *   打包像素/线性模式都按 8 点算。文本会话把这一位置 0 之后, 若退出时没有
 *   谁把它置回 1, 显示控制器就会继续按"9 点字符"的规则解释一行像素 ——
 *   而桌面画面是按 8 点写进显存的。
 *   ⇒ 任何 VGA 卡、任何 Win9x 上都有这条收尾义务, 与型号、与是不是模拟器
 *     无关。所以它写在这里, 只用最普通的端口序列。
 *
 * 【为什么这一版必须显式做】./123 的 7 次 CRTP 现场读数(step1):
 *   · 会话结束、屏幕正花那一刻 SR01 = 0x0002(9 点)          [CRTP n=0005]
 *   · 我们的重放(含 SR01)跑过之后仍然是 0x0002               [行 3213-3226]
 *   · 唯独主 VDD 那次 4F02 把它变成 0x0001(8 点)             [行 3199]
 *   而 4F02 之后的清屏/重绘才是按 8 点(640)栅格做的 —— 顺序反了。
 *   这里把它做成一个显式、可读回自证的收尾步骤: 无论 4F02 有没有做到,
 *   都要在"清屏/重绘之前"确认它已经做到。
 *
 * 【安全性】只有两个动作: outp8(3C4h, 01h) 选索引、outp8(3C5h, val) 写数据
 *   —— 与 VxdHwRegRestore 早已在做的事情完全同一条路径(它写 SR01..SR04),
 *   不新开任何端口写路径; 并且【只在读回确认 bit0 为 0 时才写】,
 *   正常情况一个字节都不写。
 *
 * 【返回值】0 = 本来就好(没动硬件); 1 = 写了并读回确认修好;
 *          -1 = 写了但读回仍不是 8 点(硬件不认, 或端口被虚拟化挡住)。
 *========================================================================*/
int __far __pascal VxdClockEnsure8Dot(void)
{
    uint8_t v = idxread( 0x3C4, 0x3C5, 0x01 );

    /* 【r138 把判据从一个位扩到两个位 —— 这正是 r137 记下的纪律】
     *   r137 的核心教训是"判据必须看【一组】位, 不能看单个位": 一个
     *   `bit0=1 就算是图形模式` 的判据, 在最坏的那种混合状态上恰好答"对"。
     *   同一件事在这里发生过, 而 ./137 的日志把它拍了下来:
     *       3731  CrtSync: SR01 raw (bit5=screen off,bit0=8dot)=0021
     *       3714  Restore: clock 8dot (...)=0000        <-- 报"本来就好"
     *   SR01 = 0x21 = bit5【屏幕关】| bit0 8 点。旧判据只看 bit0 ⇒ 说"没事",
     *   而那一刻的硬件是把显示关掉的。
     *   VGA 标准(时序器 01h Clocking Mode Register):
     *       bit0 = 0 -> 9 点字符时钟(文本历史);  bit1 = 1 -> 8 点(高分辨率图形)
     *       bit5 = 1 -> Screen Off: 显示被关掉
     *   任何我们期望有画面的模式, 都要求 bit0 = 1【并且】bit5 = 0 —— 所以这
     *   两个位必须一起判、一起写(与 r134 的回放里 `(v|0x01)&~0x20` 同一条规则,
     *   只是那一条只在"有样本"时才跑, 而这一条是无条件的收尾步骤)。
     *   【写入条件因此改成"两个位里有一个不对"】正常情况(0x01)仍然一个字节
     *   都不写。                                                    */
    if ((v & 0x01) && !(v & 0x20))
        return 0;                       /* bit0 = 1 且屏幕开着: 不写 */
    outp8( 0x3C4, 0x01 );
    outp8( 0x3C5, (uint8_t)((v | 0x01) & ~0x20) );
    v = idxread( 0x3C4, 0x3C5, 0x01 );
    return ((v & 0x01) && !(v & 0x20)) ? 1 : -1;
}

uint16_t __far __pascal VxdSeqClocking(void)
{
    return (uint16_t)idxread( 0x3C4, 0x3C5, 0x01 );
}

/* 【r128】只观察版 —— 读回 SR01 并回答"是不是 8 点", 【一个字节都不写】。
 *   返回  0 = 已经是 8 点;
 *        -2 = 不是 8 点(【只报告, 不动硬件】)。
 *   【为什么要降级成只读】./127 的 R2 用的是 Clk8=0(即这一步根本不执行),
 *   而"横向缩小"那个新 bug 在 R2 里【照样出现】, 同时 R1/R2 的花屏行为
 *   完全一致。也就是说: 我们【没有证据】说明那次写是必要的, 却有证据说明
 *   它没有解决问题。而它会真的写硬件 —— 所以在拿到"写确实有效"的证据
 *   之前, 默认只留读数。要恢复 r127 那一枪用 Clk8=2。               */
int __far __pascal VxdClockObserve8Dot(void)
{
    uint8_t v = idxread( 0x3C4, 0x3C5, 0x01 );

    /* 【r138 同一处扩位】只读版也必须问两个位: bit0 = 1(8 点)【且】
     *   bit5 = 0(屏幕开着)才算"本来就好"。旧版只看 bit0, 于是 ./137 那次
     *   SR01 = 0x21(屏幕关)被报成 0000 = "本来就好"。返回值语义不变:
     *     0 = 已经是对的;  -2 = 不对(只报告, 一个字节都不写)。      */
    if ((v & 0x01) && !(v & 0x20))
        return 0;
    return -2;
}

/*==========================================================================
 * 【r130】光栅几何现场读数 —— "扫描输出一行到底有多少个像素"。
 *
 * 【为什么 r130 必须把这两个寄存器读出来】
 *   r129 拿到了三条硬证据, 把病灶从"三个模式状态位"逼到了"光栅几何":
 *     ① 主机截图与虚拟机内截屏【逐行配色完全一致】:
 *          主机 320x771 每行主色 (0,125,123) 出现 237/243/319/319 列,
 *          虚拟机内截屏同一行主色 (0,127,127) 出现 992/1004/1024 列;
 *          两者的青底【纵向都是 740 行, 起止完全对齐】
 *        ⇒ 纵向几何是对的(1:1), 横向却只有右侧那一个点被采到
 *          ⇒ 扫描输出【一行只走了约 320 个像素】, 而帧缓冲是按 1024 写的。
 *     ② 同一份日志里 `HwReg sample diff now=0000` + `SR01 bit0=1` +
 *        `atc10 bit0=1` + `gcr06 bit0=1` ⇒ r129 补的那三个模式状态位
 *        【已经全部正确】, 屏幕却依然收缩 ⇒ r129 的机制被实测证伪。
 *     ③ 剩下的、唯一能决定"一行多少像素"的就是这两个寄存器:
 *           CRTC 00h = 水平总数 (Horizontal Total)
 *           CRTC 01h = 水平显示结束 (Horizontal Display End)
 *        标准 VGA 上【只有 CR01】决定一行显示多少【字符】, 再乘
 *        每字符的点数(时序器 01h 的 bit0/bit3)才是一行的像素数 ——
 *        86Box 的通用 SVGA 渲染器逐字就是这么算的:
 *            svga->hdisp = crtc[1];  if (crtc[1] & 1) hdisp++;
 *            hdisp *= (seqregs[1] & 1) ? 8 : 9;   [bit3 = 1 时是 16/18]
 *        1024 像素需要 128 个字符 ⇒ CR01 = 0x7F; 若它停在文本模式的小值
 *        上(例如 0x27 = 40 字符 = 320 像素), 症状就逐像素对上"窗口 320 宽"。
 *   ⇒ 这两个字节此前【从未被读过】: 0x00 与 0x01 既不在 `CRTP` 的读数里,
 *     也不在任何探针里, 所以"几何到底是多少"在 r129 之前的所有日志里
 *     都没有数据。r130 补上它 —— 先有读数, 再谈修法。
 *
 * 【安全性】纯端口读, 与 PortProbeRead / HwRegSave 已在真机上读过千百次的
 *   同一条路径(索引写 3D4h、数据读 3D5h); 不写任何寄存器。
 *   r130-marker: CrtGeometryLooksRight (scan-line pixel count vs framebuffer width).
 *========================================================================*/
uint16_t __far VxdReadCrtHTotal(void)
{
    return (uint16_t)idxread( 0x3D4, 0x3D5, 0x00 );
}

uint16_t __far VxdReadCrtHDisplay(void)
{
    return (uint16_t)idxread( 0x3D4, 0x3D5, 0x01 );
}

/* 【r130】四个读数留在这里供日志使用(取用时刻 = 上一次调用 CrtGeometryLooksRight
 * 那一刻)。非 static 是刻意的: display.c 也要把它们打进日志, 见 drv.h 的声明。 */
uint16_t CrtHDe    = 0xEEEE;
uint16_t CrtHTo    = 0xEEEE;
uint16_t CrtHDots  = 0xEEEE;
uint16_t CrtHWidth = 0xEEEE;

/*==========================================================================
 * 【r130】CrtGeometryLooksRight —— "扫描输出的一行像素数"是否等于
 *   帧缓冲的一行像素数。
 *
 * 【判据】VGA 标准:  一行像素 = (CR01 + 1) * 每字符点数
 *         每字符点数 = 8, 若时序器 01h bit3 = 1(每字符时钟 2 个字)则 16;
 *         否则由 bit0 决定 8(图形)/ 9(文本)。
 *   我们的模式永远是打包像素图形模式(bit0 已由 VxdClockEnsure8Dot 保证为 1),
 *   所以只可能是 8 或 16。
 * 【期望值从参数进来(wantW)】不在这里引用 display.c 的 wScrX —— 那会让
 *   drv.c 依赖 display.c 的文件级全局(本项目一直避免这种反向依赖)。
 *   调用方传进来的就是桌面宽度, 也正是交给主 VDD 的可见屏宽度(ECX =
 *   wScreenPitchBytes × wScreenY 里那一项)、帧缓冲选择子的来源, 三者必须自洽。
 *
 * 【为什么判据取"相等"而不是"足够大"】CR01 比期望大 = 扫描输出跑到帧缓冲
 *   之外(右侧是垃圾或回卷), 同样必须修, 所以只认精确相等。
 *
 * 【返回值】1 = 扫描几何与帧缓冲一致;
 *          0 = 不一致(这是 r130 认定的病灶);
 *          2 = 读不到可信值(CR00 与 CR01 都是 0xFF, 端口后面没有东西),
 *              或 wantW 为 0 —— 报"不知道", 不冒充结论, 于是也不动硬件。
 * 【副作用】把四个读数留在文件级变量里, 供日志使用(见 drv.h 的声明)。
 *========================================================================*/
int __far __pascal CrtGeometryLooksRight(uint16_t wantW)
{
    uint16_t htot;
    uint16_t hde;
    uint32_t dots;
    uint32_t width;

    if (wantW == 0)
        return 2;

    htot = VxdReadCrtHTotal();
    hde  = VxdReadCrtHDisplay();
    if (htot == 0xFFFF && hde == 0xFFFF)
        return 2;       /* port does not answer: report "unknown", do not touch */

    /* 【r138 修一个判据盲区: 每字符点数有三个可能值, 不是两个】
     *   旧式: dots = (SR01 & 0x08) ? 16 : 8 —— 它把 bit0 = 0(SR01 = 0x02,
     *   【9 点字符时钟】, 文本模式的历史时序)也当成 8 点算。
     *   后果不是理论上的: ./123 的 7 次 CRTP 实测里 SR01 = 0x0002 出现过,
     *   ./136 的 6118 行起 SR01 = 0x0003, 而 ./127 正是靠这条读数才判出
     *   "样本存成了 9 点"。若硬件真的停在 bit0 = 0, 一行像素 = (CR01+1)*9,
     *   而旧式会算出 (CR01+1)*8 并【报"几何正确"】—— 一个把错状态判成对的
     *   判据, 正是 r137 记下的那条纪律要消灭的东西。
     *   VGA 标准(时序器 01h Clocking Mode Register):
     *       bit3 = 1 -> 每字符时钟 2 个字(16 点)
     *       bit3 = 0 -> bit0 = 1 ⇒ 8 点;  bit0 = 0 ⇒ 9 点
     *   三个值都要如实算出来。                                        */
    {
        uint16_t sr01 = VxdSeqClocking();

        if (sr01 & 0x08)      dots = 16UL;
        else if (sr01 & 0x01) dots = 8UL;
        else                  dots = 9UL;
        /* 【r138 无条件通道】把原始 SR01 与算出来的点数落盘 —— 判据改了,
         *   判据的输入也必须可读, 否则下一轮无法区分"点数算错"与"寄存器
         *   读错"。PHASEV 在 release 构建里同样有效。                 */
        PHASEV( "PH: crtsr01=", sr01 );
        PHASEV( "PH: crtdots=", (uint16_t)dots );
    }
    width = ((uint32_t)((hde & 0xFFUL) + 1UL)) * dots;

    CrtHDe    = (WORD)hde;
    CrtHTo    = (WORD)htot;
    CrtHDots  = (WORD)dots;
    CrtHWidth = (WORD)((width > 0xFFFFUL) ? 0xFFFF : width);

    return (width == (uint32_t)wantW) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 【r127】Misc Output 与 属性控制器 两个读数。
 *
 * 【为什么必须补这两个】86Box 的通用 SVGA 渲染器(recalctimings)判定
 *   "文本模式 / 图形模式"用的【不是】时钟, 而是两个位:
 *       !(gdcreg[6] & 1)  &&  !(attrregs[0x10] & 1)  ->  Text mode
 *   即 GCR6(Graphics Controller Miscellaneous)bit0 与属性控制器 10h bit0。
 *   这两位此前【完全没有读数】: CRTP 只读 CRTC/时序器/GCR05/GCR06, 偏偏
 *   把"文本还是图形"那两位漏在外面。没有它们, "屏幕花"与"模式状态"之间
 *   就只能靠推测 —— 这是补测量的最后一块。
 *
 * 【Misc Output 3CCh】标准 VGA 上【只读】(3C2h 才只写), 同步读安全。
 *   它给出 bit2 = 时钟选择, bit0 = 彩色/单色。
 *
 * 【属性控制器 3C0h/3C1h —— 诚实列出这条路径的风险与边界】
 *   属性控制器有一个内部触发器(flip-flop)在"索引/数据"之间翻转:
 *     · 读 3DAh(Input Status 1) -> 触发器强制回【索引】态(标准 VGA 行为)
 *     · 读 3C0h                  -> 返回当前索引(bit5 = palette enable)
 *     · 读 3C1h                  -> 返回该索引的数据
 *   于是可以【只读】地拿到任意一个属性寄存器的值。代价与边界:
 *     ① 读 3DAh 是读操作, 不改显示状态(它同时复位触发器, 正是我们要的);
 *     ② 索引寄存器 bit5(palette enable)回读【无法区分】—— 因为"写 3C0h"
 *        这个动作本身会把该位当作 palette enable 写下去。我们只在"目标
 *        索引与当前索引不同"时才写一次 3C0h(写的是【索引】, 不改任何
 *        属性寄存器内容), 读完立即把索引写回原值;
 *     ③ 每次只读【一个】寄存器(10h = 模式控制), 不遍历 0..1F: 遍历要
 *        反复动索引, 那是"改状态"而不是"读状态", 收益不值这个代价。
 *   ⚠ 主 VDD 接管 3C0h-3DFh 这些端口, 所以本函数与其它探针一样受
 *     PortProbe 开关管辖(默认关), 由用户单独跑一次去证。
 *========================================================================*/
uint16_t __far VxdReadMiscOut(void)
{
    return (uint16_t)inp8( 0x3CC );
}

uint16_t __far VxdReadAtcReg(uint16_t idx)
{
    uint8_t cur;
    uint8_t want;
    uint8_t val;

    if (idx > 0x1F)
        return 0xFFFF;                  /* out of range: touch no port at all */

    want = (uint8_t)idx;

    /* --- [r133] attribute-controller flip-flop discipline ------------------
     * The Attribute Controller has ONE flip-flop, shared by every writer in
     * the machine (the card BIOS, the main VDD, a DOS VM and us).  FreeVGA,
     * "Attribute Controller Registers":
     *     . reading 3DAh forces it back to the INDEX phase;
     *     . a write to 3C0h in the INDEX phase selects attraddr (bits 0-4),
     *       stores the palette-address-source state (bit 5) and flips to DATA;
     *     . a write to 3C0h in the DATA phase stores the value into the
     *       selected register and flips back to INDEX.
     * r129..r132 broke that rule in exactly one place.  After the data read
     * the flip-flop is in the DATA phase, so this line
     *         outp8( 3C0h, (cur & 1Fh) );   "put the index back"
     * was not an index write at all: it wrote (cur & 1Fh) INTO ATC 10h, the
     * Mode Control register that had just been selected.  Two consequences,
     * and neither one shows up in any register self-check:
     *     1. the graphics/alphanumeric bit (ATC10h bit0) and the palette bit
     *        were destroyed by the instrument itself -- and ./132's own
     *        snapshots recorded the corpse: ATC10 = 0041 at n=1, then 0000
     *        for every later snapshot, with nothing else in the log between;
     *     2. an implementation that re-latches the video timing when ATC10h
     *        changes (the family of implementations the striped host captures
     *        come from does exactly that) was told to re-latch at that
     *        arbitrary moment, from whatever half-programmed register set
     *        happened to be live then.
     * A probe must write nothing but the index, and it must not leave the
     * flip-flop in the data phase.  The sequence below writes the index twice
     * (select, then restore); both writes carry the palette bit through
     * unchanged, so both are no-ops on the wire and in the chip.
     * --------------------------------------------------------------------- */
    (void)inp8( 0x3DA );                /* read-only: flip-flop -> INDEX phase */
    cur = inp8( 0x3C0 );                /* index | PAS (bit5) */

    outp8( 0x3C0, (uint8_t)((cur & 0xE0) | want) );  /* select; keep PAS (now DATA) */
    val = inp8( 0x3C1 );                             /* data */

    (void)inp8( 0x3DA );                /* -> INDEX phase again, so that ... */
    outp8( 0x3C0, cur );                /* ... this restores the index, and only it */
    (void)inp8( 0x3DA );                /* hand back the INDEX phase, as VGA apps expect */
    return (uint16_t)val;
}

/* ---------------------------------------------------------------------------
 * 【r117】DispiPortProbe —— 直接读(并可选择修复)VBE_DISPI 寄存器 1CEh/1CFh
 *
 * 【端口布局】(Bochs 家族通用: QEMU std-vga / VirtualBox VBoxVGA)
 *   1CEh = 索引, 1CFh = 数据
 *     0 = ID(【只写 / 写后回读】, 0xB0C0|版本)   1 = XRES
 *     2 = YRES                       3 = BPP
 *     4 = ENABLE(bit0=ENABLED, bit6=LFB_ENABLED, bit7=NOCLEARMEM)
 *     5 = BANK                       6 = VIRT_WIDTH(= pitch / (bpp/8))
 *     7 = VIRT_HEIGHT                8/9 = X/Y_OFFSET
 *   ⚠ r119: 索引 0 【不是只读】。r117/r118 的注释写错了这一个字, 代价是
 *   两轮整机测试零效果 —— 详见 DispiIdProbe 的头注(含上游源码原文)。
 *
 * 【为什么本轮必须有这一枪】./116 的 D1(release)主机截图 9_host 给出了
 * "扫描输出格式被改过"的硬证据(逐单元取证见 R117_CHANGES.md §0.1:
 * 全部 384 个偶数行逐像素相同 ⇒ 显示行成对读同一段源数据 ⇒ 硬件按
 * "bpp=8, xres=1024, pitch=1024" 扫描 Windows 的 16bpp/2048 帧缓冲),
 * 而 1CEh/1CFh 是这台机器上【唯一】能在 Windows 完全不知情的情况下改掉
 * xres/yres/bpp/pitch 的通道。上一轮靠"状态位反推"装机成败, 结果是
 * 整轮零信息(dispi 位恒 0, 分不清"策略关"与"没装上")。这一枪把"猜"
 * 换成"读数": 每个状态边沿把 6 个寄存器原样落盘。
 *
 * 【为什么读它安全】"写索引 + 读数据"对 DISPI 无副作用(不写数据端口就
 * 不改任何模式)。与 PortProbeRead 读 3D4h/3C4h 不同: 1CEh/1CFh 【不在】
 * 主 VDD 接管的标准 VGA 端口集合(3C0h-3DFh)里, 不会碰主 VDD 的影子
 * 寄存器与索引锁 —— 而主 VDD 那套正是 ./109 开 PortProbe 起不来机的
 * 原因(见 g_runPortGrp 注)。
 *
 * 【探测门槛(r119 重写)】先用 DispiIdProbe 做"写—回读—还原"确认
 * (只写索引 0, 写完必还原)。确认过就记下来(省得每次都写), 【确认不了
 * 也不再永久静默】—— 只把本次数完, 下一次探针照问, 直到问出结论。
 * r117/r118 是"第一次问不到就永远闭嘴", 于是日志里只有一行
 * "DISPI absent id=FFFF", 之后整场会话 5378 行日志里再也看不到 DISPI
 * 的任何读数 —— 连"它到底被试过几次"都无从判断。这是本轮修的第二个
 * 设计缺陷。
 *
 * 【修复(allowFix)】只在"ID 认得出 + 调用方判定当前不是全屏 VGA 会话 +
 * 读到的 xres/yres/bpp/virtw/ENABLED 与本机当前模式不符"时才动手:
 * 依次写 XRES/YRES/VIRT_WIDTH/VIRT_HEIGHT/BPP, 最后写 ENABLE = 0xC1
 * (= ENABLED | LFB_ENABLED | NOCLEARMEM), 然后回读自证。用 NOCLEARMEM
 * 是刻意的 —— 修格式不该清掉桌面内容, 于是修好的瞬间画面就正确, 不需要
 * 重绘。VIRT_HEIGHT 是 r119 补上的: 上游 vgaR3GetResolution 里可见高度是
 *   RT_MIN( vbe_regs[YRES], vbe_regs[VIRT_HEIGHT] )
 * 只把 YRES 写对、VIRT_HEIGHT 仍留在旧值(比如文本模式的 25 行 ⇒ 400 像素)
 * 的话, 高度会被那个旧值截断, 扫描输出仍然不是我们要的几何。
 * 这是"官方路径(VBE BIOS 4F02)不成功时的回落层", 由 INI 的 DispiFix
 * 开关(默认 1)与 DispiIdProbe 开关(默认 1)共同管辖, 关掉即成纯观测。
 * ------------------------------------------------------------------------- */
static int  s_dispiIdOk  = 0;           /* 0 还没确认过 / 1 已确认有 DISPI */
static WORD s_dispiProbeN = 0;          /* 探针次数(限流用)                */

void __far DispiPortProbe(const char *why, WORD expW, WORD expH, WORD expBpp,
                          WORD expPitch, int allowFix)
{
    WORD xr, yr, bp, en, vw, vwExp, vh;
    int  bad;

    (void)why;                          /* 发布构建里 DIAG(why) 是空宏 */
    if (expBpp == 0 || expW == 0)
        return;                         /* 模式还没建立: 没有可比对象 */
    if (s_dispiProbeN >= 200)
        return;                         /* 绝对上限: 每会话最多 200 次 */

    /* --- 确认 DISPI 存在(r119: 写—回读—还原, 不是只读) ---------------- */
    if (!s_dispiIdOk)
    {
        if (!g_runDispiIdProbe)
        {
            DIAGMIN( "DISPIID write-probe disabled by policy (DispiIdProbe=0)" );
            s_dispiProbeN++;
            FlushLog();
            return;                     /* 策略关: 一次都不写, 也不下结论 */
        }
        if (!DispiIdProbe())
        {
            s_dispiProbeN++;
            return;                     /* 本次数: 下次照问, 不永久闭嘴 */
        }
        s_dispiIdOk = 1;
    }
    s_dispiProbeN++;

    /* 【r119: 不再读索引 0 当判据】它只写; 存在性已由上面的 DispiIdProbe
     * 用"写—回读—还原"确认过。这里直接读真正决定扫描格式的那几个。      */
    xr = idxread16( 0x1CE, 0x1CF, 0x0001 );
    yr = idxread16( 0x1CE, 0x1CF, 0x0002 );
    bp = idxread16( 0x1CE, 0x1CF, 0x0003 );
    en = idxread16( 0x1CE, 0x1CF, 0x0004 );
    vw = idxread16( 0x1CE, 0x1CF, 0x0006 );
    vh = idxread16( 0x1CE, 0x1CF, 0x0007 );
    vwExp = (WORD)(((DWORD)expPitch * 8UL) / (DWORD)expBpp);

    /* 判据: 四个几何/格式寄存器 + "ENABLED 位还在"。
     * 【VIRT_WIDTH==0 要当成"正确"】VBE_DISPI 的语义是"VIRT_WIDTH=0 时
     * 按 XRES 自动定行宽"(QEMU/Bochs 的 vbe_regs[VIRT_WIDTH] 允许为 0,
     * 有效 pitch 仍 = xres*bpp/8)。很多 BIOS 就是这么留的 —— 把 0 判成
     * "不符"会导致每一条边沿都白写一遍端口, 正是最该避免的事。      */
    bad = (xr != expW) || (yr != expH) || (bp != expBpp) ||
          ((vw != 0) && (vw != vwExp)) || ((en & 0x01) == 0);

    DIAG( why );
    DiagMark( "DISPI" );
    DiagHexMin( "DISPI probe n", s_dispiProbeN );
    DiagHexMin( "DISPI probe verdict", (WORD)(bad ? 0 : 1) );
    DiagHexMin( "DISPI xres", xr );
    DiagHexMin( "DISPI yres", yr );
    DiagHexMin( "DISPI bpp",  bp );
    DiagHexMin( "DISPI enable", en );
    DiagHexMin( "DISPI virtw",  vw );
    DiagHexMin( "DISPI virth",  vh );
    /* 【r119 新增】把"DISPI 侧每行字节数"直接落盘 —— 它与本机 pitch 不等
     * 就是花屏的定义式。r117/r118 只打了 VIRT_WIDTH(一个寄存器值), 读者
     * 还要自己乘除一次才知道对不对; 这里把两边都写成字节数, 一行对照。 */
    DiagHexMin( "DISPI pitch bytes", (WORD)(((DWORD)vw * (DWORD)bp) / 8UL) );
    DiagHex( "DISPI expect xres", expW );
    DiagHex( "DISPI expect yres", expH );
    DiagHex( "DISPI expect bpp",  expBpp );
    DiagHex( "DISPI expect virtw", vwExp );
    DiagHex( "DISPI expect virth", expH );
    DiagHex( "DISPI expect pitch bytes", expPitch );
    FlushLog();

    if (!bad)
        return;                         /* 一切正常: 不做任何写 */

    DIAGMIN( "DISPI MISMATCH: hardware scan format differs from our mode" );
    FlushLog();

    if (!allowFix)
    {
        DIAGMIN( "DISPI fix skipped (policy off / not a safe context)" );
        FlushLog();
        return;
    }
    /* 【只在 DISPI 确实"活着但配错"时才修】ENABLED 位没置起 = 硬件现在
     * 根本不在 DISPI 模式里(比如被切成了标准 VGA/文本) —— 那是另一类故障,
     * 由官方路径(VBE BIOS 4F02)负责, 在这里硬写 DISPI 只会把两种故障
     * 叠在一起, 让下一轮无法归因。                                     */
    if ((en & 0x01) == 0)
    {
        DIAGMIN( "DISPI fix skipped (DISPI not enabled: hardware is not in a DISPI mode)" );
        FlushLog();
        return;
    }

    /* --- 写回正确格式(顺序与 VBE BIOS 一致; ENABLE 放最后) ----------
     * 全部 16 位写: DISPI 每个寄存器都是 16 位的, 字节写的语义各家不同
     * (见 inp16 头注)。ENABLE = 0xC1 = ENABLED|LFB_ENABLED|NOCLEARMEM
     * —— NOCLEARMEM 是刻意的: 修格式不该清掉桌面内容。
     * 【r119 补 VIRT_HEIGHT】上游 vgaR3GetResolution 的可见高度是
     *   RT_MIN( vbe_regs[YRES], vbe_regs[VIRT_HEIGHT] )
     * 只写 YRES 而 VIRT_HEIGHT 留在旧值(文本模式常是 400)时高度会被截断,
     * 所以两者必须一起写。                                              */
    idxwrite16( 0x1CE, 0x1CF, 0x0001, expW );
    idxwrite16( 0x1CE, 0x1CF, 0x0002, expH );
    idxwrite16( 0x1CE, 0x1CF, 0x0006, vwExp );
    idxwrite16( 0x1CE, 0x1CF, 0x0007, expH );
    idxwrite16( 0x1CE, 0x1CF, 0x0003, expBpp );
    idxwrite16( 0x1CE, 0x1CF, 0x0004, 0x00C1 );
    DIAGMIN( "DISPI fix applied (xres/yres/virtw/virth/bpp then ENABLE=C1)" );
    FlushLog();

    xr = idxread16(0x1CE, 0x1CF, 0x0001);
    yr = idxread16(0x1CE, 0x1CF, 0x0002);
    bp = idxread16(0x1CE, 0x1CF, 0x0003);
    en = idxread16(0x1CE, 0x1CF, 0x0004);
    vw = idxread16(0x1CE, 0x1CF, 0x0006);
    vh = idxread16(0x1CE, 0x1CF, 0x0007);
    bad = (xr != expW) || (yr != expH) || (bp != expBpp) ||
          ((vw != 0) && (vw != vwExp)) || ((en & 0x01) == 0);
    DiagHexMin( "DISPI fix result", (WORD)(bad ? 0 : 1) );
    DiagHexMin( "DISPI fix xres", xr );
    DiagHexMin( "DISPI fix yres", yr );
    DiagHexMin( "DISPI fix bpp",  bp );
    DiagHexMin( "DISPI fix enable", en );
    DiagHexMin( "DISPI fix virtw",  vw );
    DiagHexMin( "DISPI fix virth",  vh );
    FlushLog();
}

/* 写 VGA 标准 DAC(3C8h/3C9h)。
 * 【这是卡专用代码吗？】不是。3C6h-3C9h 是 VGA 标准调色板端口，
 * 自 1987 年以来每一张 VGA 兼容卡(包括一切 VBE 卡，VBE 规范要求
 * 卡片同时具备完整 VGA 兼容性)都实现它们 —— 与"识别具体型号后
 * 直连私有寄存器"的卡专用方案是两回事。
 * 【为何要 STOP_IO_TRAP 配合】显示驱动接管屏幕后(int 2F 4000h)，
 * 主 VDD 不再虚拟化系统 VM 的显示端口，这里的端口写直达硬件。
 * 入口: pal4 = 每项 4 字节 B,G,R,0 的调色板(与 RGBQUAD/DIBColorEntry
 *       同布局)，VGA DAC 只取 6 位精度，故右移 2 位。                */
void __far DAC_SetPalette(uint16_t start, uint16_t count, const uint8_t far *pal4)
{
    uint16_t i;

    outp8(0x3C8, (uint8_t)start);       /* DAC 写索引 */
    for (i = 0; i < count; i++) {
        outp8(0x3C9, (uint8_t)(pal4[2] >> 2));  /* R */
        outp8(0x3C9, (uint8_t)(pal4[1] >> 2));  /* G */
        outp8(0x3C9, (uint8_t)(pal4[0] >> 2));  /* B */
        pal4 += 4;
    }
}

/*==========================================================================
 * 【r139】DAC 见证组 —— "整组模式寄存器看不见的那一半"的硬件读数。
 *
 * 【./138 的截图把病灶量成了数, 这是本组存在的唯一理由】
 *   1_host.png 逐像素量出来的东西(桌面底色与任务栏两处互证, 全部可复算):
 *     · 客机截图里的桌面底色 = (0,127,127) ⇒ RGB565 = 0x03EF, 字节序 EF,03
 *       主机上渲染成【4 像素一组: 黑 黑 青 青】
 *     · 客机截图里的任务栏底色 = (191,191,191) ⇒ RGB565 = 0xBDF7, 字节 F7,BD
 *       主机上整条渲染成【全黑】
 *     · 那个"青" = (0,170,170) = VGA 标准 16 色 EGA 调色板的【第 3 号】
 *     · 那个"黑" = 索引 ≥ 16 的项(16 色调色板没装载, 全 0)
 *   ⇒ 硬件在【按 8bpp 扫描我们的 16bpp 显存】: 每个字节横向复制成 2 个
 *     像素, 并把每个字节当【调色板索引】查一张"只有前 16 项"的表。
 *     这正是 DOS 盒设过一次 VGA 文本模式(INT 10h AH=00h AL=03h)留下的
 *     状态。文本模式本来就是 8bpp 扫描 + 16 色 DAC。
 *
 * 【为什么我们所有的仪器都答"正常"】
 *   像素格式与 RAMDAC 都【不是 VGA 寄存器】。主 VDD 按 DDK 的
 *   VDD_SAVE_DRIVER_STATE(0x82) 保存/恢复的是"标准 VGA 寄存器 +
 *   迷你 VDD 自己用 SAVE_REGISTERS(8 号) 申报的扩展寄存器" —— 而我们的
 *   8/9 号是空壳(见 minivdd.asm 的分派表)。于是它看得见的那一半回来了
 *   (我们读到的 CR00/CR01/CR13/SR01/GCR06/AR10/CR17 全部正确、几何判据
 *   也成立), 它看不见的那一半没有回来。
 *   ⇒ "整组模式寄存器逐字节等于已知良好样本"【推不出】"屏幕是对的"。
 *     这就是 ./138 能在花屏时打出 `sessend essentials (1=ours)=0001` 与
 *     `need rebuild=0000` 的原因 —— 见 display.c 的 r139 判据注释。
 *
 * 【本组提供两个独立于寄存器样本的见证】
 *   ① VBE 4F03h(取当前模式号)。VBE 规范里它【只有取回形态】, 因此它是
 *      "别人(BIOS)怎么看这台硬件"的读数, 不是我们自己的镜像。DOS 盒设了
 *      03h 而主 VDD 事后直接改寄存器回来, BIOS 的记账就还停在那个模式上
 *      ⇒ 与我们的模式号不符 = "BIOS 从没被告知我们回来了"。(r138 已经实现
 *      了这条读数, 但没人拿它当判据 —— 这里把它接进判据。)
 *   ② DAC 印章。我们在【每一次成功的模式建立之后】往 DAC 前 8 项写一组
 *      固定值。DOS 盒的文本模式设置【必然】重写 DAC 前 16 项(上面量到的
 *      "第 3 号 = 青"就是证据), 所以"印章没了"= 之后有人装载过调色板 =
 *      我们的模式没有重新建立过。这是【正向】判据: 印章还在 = 我们确实在
 *      这台硬件上做过一次模式建立。它不依赖任何推断。
 *
 * 【为什么写 DAC 是安全的】3C6h..3C9h 是 VGA 标准调色板端口(不是卡专用),
 *   本驱动早就在写它们(见上面的 DAC_SetPalette)。而目标模式是 16bpp
 *   打包像素, 此时 RAMDAC 不参与画面 ⇒ 写前 8 项对显示零影响。
 *   因此【只在 bpp != 8 时盖章】: 8bpp 会话下 DAC 就是画面本身, 一个字节
 *   都不许碰。
 *
 * 【为什么读 DAC 只在低频安全点做】写 3C7h 会把 DAC 切到读索引, 而主 VDD
 *   自己也在写这套端口(它虚拟化 VIRTUALIZE_DAC_OUT)。若在它的切屏事务
 *   【内部】插一次读, 它下一次 3C9h 写就可能落到错误的项上。所以本组的
 *   每一个读都只在 display.c 的 MaybeDeferredRestore(普通 ring 3 DDI 入口)
 *   里调用 —— 那是全工程公认的安全上下文。读之后不做"还原"写: 任何一次
 *   3C8h 写都会把 DAC 切回写模式, 多写一次反而会动到别人的写索引。
 *
 * 【返回值约定】三条判据一律把"读不到"与"读到不对"分开: 前者是"不知道"
 *   (不许据此动硬件), 后者才是"知道它不对"(该动手)。这是 r138 已经立下
 *   的规矩(见 VBE_ProbeCurrentMode 头注), 本组照抄。
 *========================================================================*/
#define DAC_STAMP_N 8

/* 印章值: 6 位精度(VGA DAC 只认 0..63), 8 项互不相同, 且没有一项是全黑、
 *   也没有一项等于 EGA 16 色里的常见值 —— 免得和"没盖过章"混淆。      */
static const uint8_t s_dacStamp[DAC_STAMP_N][3] =
{
    { 0x3F, 0x12, 0x24 },
    { 0x12, 0x24, 0x3F },
    { 0x24, 0x3F, 0x12 },
    { 0x3F, 0x24, 0x12 },
    { 0x12, 0x3F, 0x24 },
    { 0x24, 0x12, 0x3F },
    { 0x0A, 0x1E, 0x32 },
    { 0x32, 0x0A, 0x1E }
};

/* DAC 指纹要采样的 16 个索引: 前 11 个都在"16 色调色板"会装载的范围内,
 *   后 5 个都在范围外 —— 于是"只有前 16 项被装载"这个形状可以被直接数出来。*/
static const uint8_t s_dacProbe[16] =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0F, 0x10, 0x20, 0x40, 0xEF, 0xFF
};

/* 读一项 DAC。返回 1 = 读到了; 0 = 三次读全是 0xFF(与 idxread 同一条约定:
 *   端口后面没有设备时硬件自己回 0xFF)。
 *   【为什么结果走文件级静态量而不是指针参数】-mm 下 `uint8_t *` 实参会触发
 *   W112(指针截断), 而本项目的纪律是"三档构建的警告集合必须与基线逐条相同"。
 *   VBE_Probe* 那一族就是因为同一个原因改成"结果放文件级静态量"的。     */
static uint8_t s_dacR = 0;
static uint8_t s_dacG = 0;
static uint8_t s_dacB = 0;

static int DacReadEntry( uint8_t idx )
{
    uint8_t r, g, b;

    outp8( 0x3C7, idx );                /* DAC 读索引 */
    r = inp8( 0x3C9 );
    g = inp8( 0x3C9 );
    b = inp8( 0x3C9 );
    s_dacR = r;
    s_dacG = g;
    s_dacB = b;
    return ((r == 0xFF) && (g == 0xFF) && (b == 0xFF)) ? 0 : 1;
}

/*--------------------------------------------------------------------------
 * VxdDacStamp —— 往 DAC 0..7 盖一次章。调用方【必须】先确认 bpp != 8。
 *   为什么盖章这件事值得存在: 它是"我们在这台硬件上做过一次模式建立"
 *   在【硬件里】留下的物证, 而不是我们软件里的一个标志位。下一次从 DOS 盒
 *   回来时读它就够了 —— 不需要推断谁动过什么。
 *------------------------------------------------------------------------*/
void __far __pascal VxdDacStamp( void )
{
    uint16_t i;

    outp8( 0x3C8, 0x00 );               /* DAC 写索引 = 0 */
    for (i = 0; i < DAC_STAMP_N; i++)
    {
        outp8( 0x3C9, s_dacStamp[i][0] );
        outp8( 0x3C9, s_dacStamp[i][1] );
        outp8( 0x3C9, s_dacStamp[i][2] );
    }
}

/*--------------------------------------------------------------------------
 * VxdDacStampOk —— 印章还在不在。
 *   1     = 8 项全部逐位相符(掩到 6 位比较: 有些卡会把高两位回读成 1)
 *   0     = 至少一项不符 ⇒ 之后有人装载过调色板 ⇒ 我们的模式没重新建立
 *   0xFFFF= 8 项没能全读回来(端口被挡/无设备) ⇒ "不知道", 不许据此动手
 *------------------------------------------------------------------------*/
uint16_t __far __pascal VxdDacStampOk( void )
{
    uint16_t i, bad = 0, got = 0;

    for (i = 0; i < DAC_STAMP_N; i++)
    {
        if (!DacReadEntry( (uint8_t)i ))
            continue;
        got++;
        if (((uint8_t)(s_dacR & 0x3F) != s_dacStamp[i][0]) ||
            ((uint8_t)(s_dacG & 0x3F) != s_dacStamp[i][1]) ||
            ((uint8_t)(s_dacB & 0x3F) != s_dacStamp[i][2]))
            bad++;
    }
    if (got < DAC_STAMP_N)
        return 0xFFFF;
    return (bad == 0) ? 1 : 0;
}

/*--------------------------------------------------------------------------
 * VxdDacFingerprint —— DAC 形状(【只作证据, 不作判据】)。
 *   高字节 = 16 个采样索引里"非黑"的项数
 *   低字节 = 其中索引 < 16 的那些项数
 *   病灶形状 = 高字节 >= 8 且 高字节 == 低字节(非黑的项全在前 16 项里)
 *   —— 于是"16 色调色板 + 其余全黑"这件事在日志里是一个可读的数, 下一轮
 *   不必再靠截图猜。
 *------------------------------------------------------------------------*/
uint16_t __far __pascal VxdDacFingerprint( void )
{
    uint16_t i, nonBlack = 0, nonBlackLow = 0;

    for (i = 0; i < 16; i++)
    {
        if (!DacReadEntry( s_dacProbe[i] ))
            continue;
        if (s_dacR || s_dacG || s_dacB)
        {
            nonBlack++;
            if (s_dacProbe[i] < 0x10)
                nonBlackLow++;
        }
    }
    return (uint16_t)((nonBlack << 8) | (nonBlackLow & 0xFF));
}

/*--------------------------------------------------------------------------
 * VxdHwBlindNeed —— "寄存器样本看不见的那一半"的合并判据。
 *   返回 1 = 有见证说"我们的模式没有重新建立过"; 0 = 没有见证这么说。
 *   证据位落在 g_hwBlindEv, 由 display.c 无条件打印(PHASEV)。
 *   【参数 expectMode】调用方给出"我们认为现在应该在的模式号"(就是
 *   display.c 里的目标模式)。不在这里自己取, 是为了让调用方与
 *   VerifyDisplayMode 用同一个数 —— 判据不许有第二个来源。
 *------------------------------------------------------------------------*/
uint16_t g_hwBlindEv = 0;

int __far __pascal VxdHwBlindNeed( uint16_t expectMode )
{
    uint16_t ev = 0;
    uint16_t stamp, fp;

    /* ① BIOS 记账(4F03h)。VBE 规范里它只有取回形态, 所以这是"别人怎么看"
     *    的读数, 不是我们的镜像。
     *    expectMode = 0 表示调用方【没有期望值】(例如目标模式还没查出来):
     *    这时不下结论, 与"取不到"同处理 —— 不许拿一个不存在的期望去逼出
     *    一次模式建立。                                                */
    if (expectMode == 0)
        ev |= HWBLIND_MODE_UNREAD;
    else if (VBE_ProbeCurrentMode())
    {
        uint16_t hm = VBE_ProbeModeValue();

        if ((hm & 0x3FFF) != (expectMode & 0x3FFF))
            ev |= HWBLIND_MODE_MISMATCH;
    }
    else
        ev |= HWBLIND_MODE_UNREAD;

    /* ② DAC 印章 —— 正向物证。                                          */
    stamp = VxdDacStampOk();
    if (stamp == 0xFFFF)
        ev |= HWBLIND_STAMP_UNREAD;
    else if (stamp == 0)
        ev |= HWBLIND_STAMP_GONE;

    /* ③ DAC 形状(纯证据: 不参与返回值, 只落盘)。                        */
    fp = VxdDacFingerprint();
    if (((fp >> 8) >= 8) && (((fp >> 8) - (fp & 0xFF)) == 0))
        ev |= HWBLIND_TEXT_PALETTE;

    g_hwBlindEv = ev;
    return ((ev & (HWBLIND_MODE_MISMATCH | HWBLIND_STAMP_GONE)) != 0) ? 1 : 0;
}

/* r139-marker: VxdHwBlindNeed / VxdDacStamp / VxdDacStampOk / VxdDacFingerprint
 *   (the two witnesses the whole-register sample structurally cannot see:
 *    the VBE current-mode number from 4F03h, and a DAC stamp that only a real
 *    mode re-establishment can put back).  Kept in drv.c because inp8/outp8 and
 *    the VBE probe helpers live here. */

/*--------------------------------------------------------------------------
 * 【r140】VxdVgaOwner —— “这块 VGA 现在是不是 System VM 的?” 的【外部】见证。
 *
 *   【为什么需要它】r139 用的两条见证里有一条在 ./139 被量穿了:
 *   4F03h 在这台机器上【每一次】都答 0072h —— 而据 DDK 的 CHECK_HIRES_MODE
 *   页原文, 72h/73h 是【远东版 Windows 的文本模式号】:
 *     "you should use the value passed in AL rather than reading 0:449H
 *      yourself since Far East Windows changes the data passed in AL to
 *      compensate for Japanese text modes 72H and 73H.  If the Main VDD sees
 *      that Japanese Windows 95 is running, it passes these modes as modes
 *      02H and 03H instead of 72H and 73H."
 *   它与“我们的桌面在不在”毫无关系, 也就不是见证。本函数是补上的那一条:
 *   它读的是【端口答不答话】这件物理事实。
 *
 *   【为什么 0xFF 是证据 —— ./139 日志里的直接读数】
 *   同一套 idxread(0x3CE,0x3CF,0x06) 在 ./139 的 4972 行日志里被调用了 5 次:
 *       2686 / 2912 / 3079 / 4167  ->  0005   (桌面态: GCR06 = 05h, 位0=1 图形)
 *       2734                       ->  00FF   (整份日志里【唯一】一次)
 *   而 2734 那一拍前后各一行正好是:
 *       2733 TRC atc10 bit0 (1=graphics)=0000   <- ATC10 读回 0 = 文本态
 *       2734 TRC gcr06 bit0 (1=graphics)=00FF   <- GCR06 读回 FF = 端口不应答
 *   也就是说: 主 VDD 把屏幕交给 DOS VM 的那一刻, System VM 的 ring 3
 *   端口读被虚拟化层挡掉了, 读回全 1。这不是“寄存器值”, 是“这块
 *   VGA 现在不属于我”的物证。
 *
 *   【DDK 把这个协议写得很死】VDD_SET_USER_FLAGS 页原文:
 *     "The BUSY flag ... is automatically set by the Main VDD whenever the
 *      screen group is switched away from the Windows hi-res screen to a
 *      full-screen MS-DOS session.  When this flag is set, the display driver
 *      must not draw on the display surface since the video focus is on a
 *      full-screen MS-DOS session and doing so would corrupt the appearance
 *      of the MS-DOS session.  This protocol is necessary since Windows may
 *      call display driver functions such as SetCursor, MoveCursor, BitBlt,
 *      ExtTextOut, etc. even after the video focus is set onto a full-screen
 *      MS-DOS session."
 *   两件事因此同时成立: ① 官方要求我们在“不是我们的”时候一根线都不许动;
 *   ② ./139 的日志证明 Windows 确实在整个全屏期间一直在调我们(47 条
 *   `MARK HB` 心跳, 含全屏时段)。把这两条合起来, 就是本轮的门。
 *
 *   【返回位图】
 *     VGAOWN_GCR_DEAD  0001  GCR06 读回 FFh —— 图形控制器不应答
 *     VGAOWN_ATC_TEXT  0002  已有可用的 AR 样本且 AR10 位0=0 —— 属性控制器说“文本态”
 *   【副作用】一次 idxread(写 GCR 索引 + 读数据), 与 HwRegSave 用的是同一段
 *   已在三台机器上验证过的代码路径; 不碰 ATC(那里的写会动 PAS 位)。
 *------------------------------------------------------------------------*/
uint16_t g_vgaOwnEv  = 0;   /* 最近一次的见证位图(0 = 这是我们的)      */
uint16_t g_vgaOwnN   = 0;   /* 见证被调用过的次数(纯计数, 供日志校验)  */
uint16_t g_vgaOwnG06 = 0;   /* 最近一次 GCR06 的原始读数                   */

uint16_t __far __pascal VxdVgaOwner( void )
{
    uint16_t ev = 0;
    uint8_t  g06;

    g06 = idxread( 0x3CE, 0x3CF, 0x06 );
    g_vgaOwnG06 = (uint16_t)g06;
    g_vgaOwnN++;

    if (g06 == 0xFF)
        ev |= VGAOWN_GCR_DEAD;

    if (s_hwRegArOk && !(s_hwRegAr[0] & 0x01))
        ev |= VGAOWN_ATC_TEXT;

    g_vgaOwnEv = ev;
    return ev;
}

int __far __pascal VxdVgaIsOurs( void )
{
    return ((VxdVgaOwner() & (VGAOWN_GCR_DEAD | VGAOWN_ATC_TEXT)) == 0) ? 1 : 0;
}

/* r140-marker: VxdVgaOwner / VxdVgaIsOurs (external ownership witness,
 *   the replacement for the retracted 4F03h witness: it asks the port, not
 *   ourselves, and it is the gate the DDK's BUSY protocol demands). */

/* INT 2Fh AX=4000h/4007h：通知主 VDD 停止/恢复对系统 VM 的显示 I/O
 * 陷阱。这是 DDK 规定的显示驱动标准动作：驱动接管屏幕后停止陷阱
 * (此后我们写的 DAC 端口直达硬件)，卸载前恢复。                       */

void __interrupt __far SwInt2FHook(void)
{
    if (s_swInst != 0x5A5A) { _chain_intr(s_oldInt2F); }
    if (g_regs.x.ax == 0x4001)      { if (s_sw4001 < 0xFFFF) s_sw4001++; }
    else if (g_regs.x.ax == 0x4002) { if (s_sw4002 < 0xFFFF) s_sw4002++; }
    _chain_intr(s_oldInt2F);
}

static void __far SwHookInstall(void)
{
    if (s_swInst == 0x5A5A) return;
    g_regs.x.ax = 0x352Fu; CALL_INTX(0x21);          /* AH=35h: 读旧向量 */
    s_swOldSeg = g_sregs.es; s_swOldOff = g_regs.x.bx;
    s_oldInt2F = (void (__interrupt __far *)(void))MK_FP(g_sregs.es, g_regs.x.bx);
    s_swNewSeg = FP_SEG(SwInt2FHook); s_swNewOff = FP_OFF(SwInt2FHook);
    SwSetVec( 0x2F, s_swNewSeg, s_swNewOff );        /* r412: DS:DX 由我们自己设 */
    g_regs.x.ax = 0x352Fu; CALL_INTX(0x21);          /* 读回自证 */
    if (g_sregs.es == s_swNewSeg && g_regs.x.bx == s_swNewOff) s_swInst = 0x5A5A;
    else s_swInst = 0x1111;
}
void __far VDD_StopIOTrap(void)
{
    SwHookInstall();
    g_regs.x.ax = 0x4000;
    CALL_INT(0x2F);
}

void __far VDD_StartIOTrap(void)
{
    g_regs.x.ax = 0x4007;
    CALL_INT(0x2F);
}

/* 恢复 80x25 文本模式(Disable 时用)。标准 VGA BIOS 功能，
 * 主 VDD 会同步感知并恢复它对 DOS 虚拟化的管理。                    */
void __far BIOS_SetTextMode(void)
{
    g_regs.x.ax = 0x0003;
    CALL_INT(0x10);
}
