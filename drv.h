/*============================================================================
 *
 *  DRV.H — display.drv 内部契约：drv.c(硬件/VxD 胶水) ↔ display.c(GDI 骨架)
 *
 *  【为何单独立这个头】drv.c 里的 MODEINFOBLOCK/VBEINFOBLOCK 是 VBE 规范
 *  定义的结构，display.c 枚举/筛选模式时也要用；原先它们藏在 drv.c 内部，
 *  display.c 无从引用。集中到这里后，两个编译单元看到的布局保证一致
 *  (1 字节对齐，因为 VBE BIOS 返回的内存块就是紧凑排列的)。
 *
 *  本头文件只描述"驱动内部"接口；ring3↔ring0 的契约在 vbeshare.h。
 *
 *==========================================================================*/

#ifndef DRV_H
#define DRV_H

#include "vbeshare.h"   /* 定长类型 uint8_t/uint16_t/uint32_t */

#pragma pack(push, 1)

/* --- VBE 模式信息块(INT 10h/AX=4F01h 返回，共 256 字节) ---------------- */
typedef struct _MODEINFOBLOCK {
    uint16_t ModeAttributes;        /*   0 模式属性                       */
    uint8_t  WinAAttributes;        /*   2 窗口 A 属性                    */
    uint8_t  WinBAttributes;        /*   3 窗口 B 属性                    */
    uint16_t WinGranularity;        /*   4 窗口(bank)粒度，单位 KB        */
    uint16_t WinSize;               /*   6 窗口大小，单位 KB              */
    uint16_t WinASegment;           /*   8 窗口 A 段址(通常 A000)         */
    uint16_t WinBSegment;           /*  10 窗口 B 段址                    */
    uint32_t WinFuncPtr;            /*  12 实模式 bank 切换函数远指针     */
    uint16_t BytesPerScanLine;      /*  16 每扫描行字节数                 */
    uint16_t XResolution;           /*  18 水平分辨率                     */
    uint16_t YResolution;           /*  20 垂直分辨率                     */
    uint8_t  XCharSize;             /*  22 字符宽                         */
    uint8_t  YCharSize;             /*  23 字符高                         */
    uint8_t  NumberOfPlanes;        /*  24 位平面数                       */
    uint8_t  BitsPerPixel;          /*  25 每像素位数                     */
    uint8_t  NumberOfBanks;         /*  26 bank 数                        */
    uint8_t  MemoryModel;           /*  27 显存模型(4=紧凑像素 6=直接色)  */
    uint8_t  BankSize;              /*  28 bank 大小，单位 KB             */
    uint8_t  NumberOfImagePages;    /*  29 可同时驻留的图像页数           */
    uint8_t  Reserved1;             /*  30                                */
    uint8_t  RedMaskSize;           /*  31 以下 8 字节为直接色掩码/位置   */
    uint8_t  RedFieldPosition;
    uint8_t  GreenMaskSize;
    uint8_t  GreenFieldPosition;
    uint8_t  BlueMaskSize;
    uint8_t  BlueFieldPosition;
    uint8_t  RsvdMaskSize;
    uint8_t  RsvdFieldPosition;
    uint8_t  DirectColorModeInfo;   /*  39                                */
    uint32_t PhysBasePtr;           /*  40 LFB 物理地址(VBE 2.0+)         */
    uint32_t OffScreenMemOffset;    /*  44 屏外显存偏移                   */
    uint16_t OffScreenMemSize;      /*  48 屏外显存大小，单位 KB          */
    uint8_t  Reserved2[206];        /*  50 .. 255                         */
} MODEINFOBLOCK;

/* --- VBE 信息块(INT 10h/AX=4F00h 返回，取前 20 字节已够用) -------------- */
typedef struct _VBEINFOBLOCK {
    uint8_t  VESASignature[4];      /*  0 'VESA'                          */
    uint16_t VESAVersion;           /*  4 VBE 版本(BCD，如 0x0219)        */
    uint32_t OEMStringPtr;          /*  6 OEM 字符串远指针                */
    uint8_t  Capabilities[4];       /*  10 能力位                         */
    uint32_t VideoModePtr;          /*  14 支持的模式列表远指针           */
    uint16_t TotalMemory;           /*  18 显存总量，单位 64KB            */
    uint8_t  Reserved[236];         /*  20 .. 255 (VBE 2.0 还有 OEM 区)   */
} VBEINFOBLOCK;

#pragma pack(pop)

/* --- VBE 模式属性位(ModeAttributes) ------------------------------------ */
#define VBE_ATTR_SUPPORTED  0x01    /* 该模式受支持                       */
#define VBE_ATTR_GRAPHICS   0x10    /* 图形模式(否则是文本模式)           */
#define VBE_ATTR_LFB        0x80    /* 有线性帧缓冲(VBE 2.0+)             */

/* --- VBE 显存模型(MemoryModel) ----------------------------------------- */
#define VBE_MODEL_PACKED    4       /* 紧凑像素(8bpp 调色板模式)          */
#define VBE_MODEL_DIRECT    6       /* 直接色(16/24/32bpp)                */

/*==========================================================================
 *  drv.c 导出的函数
 *========================================================================*/

/* VBE BIOS 层
 * 【为何全部 __far】drv.c 与 display.c 的代码会落到不同段(drv.c 默认
 * 段、display.c 的 _INIT/_TEXT); 显式 __far 使跨模块调用在任何内存
 * 模型下都正确, 不依赖"两个模块恰好同段"的脆弱假设。               */
int      __far VBE_SetMode(uint16_t mode);
int      __far VBE_GetModeInfo(uint16_t mode, MODEINFOBLOCK far *mi);
/* r106: INT 10h AX=0F00h 取真实显示模式。AX 返回, AL = 模式(0FFh = VBE 模式),
 * AH = 每行字符数; 0xFFFF = 直通不可用(取不到)。只读, 不改硬件。          */
uint16_t __far VBE_GetVideoMode(void);
/* r107: 问 VxD"此刻的当前 VM 句柄"(VBE_API_VM_HANDLE_PROBE)。
 * 用途: 与分派记录里的 EBX 对表 —— ./106 实测 Windows 侧是 C2920074、
 * 窗口化 DOS 盒是 D5520074, 于是"HiresAns=2(只答 Windows VM)"到底能不
 * 能用, 变成一次读数而不是猜测。只读, 0 = 取不到。                     */
uint32_t __far VBE_GetVMHandle(void);
/*=== 【r138】硬件真相的三个探针: 4F03h / 4F06h / 4F07h ====================
 *  【为什么要有它们】r112..r137 每一条"硬件在不在我们的模式里"的判据读的
 *  都是 VGA 寄存器(CRTC/时序器/GCR/属性控制器)。./137 把这条路的边界量
 *  清楚了: CR00/CR01 算出来的行宽 = 1024(对)、CR17h SE = 1(对)、AR10h
 *  干净(对), 而画面仍然是一屏"按 8bpp 重解出来的条纹"。决定画面成立与否
 *  的【像素格式】不在 VGA 寄存器里 —— 它是芯片扩展状态, 通用 VBE 驱动
 *  结构上不知道它在哪个端口。唯一通用、标准、厂商无关的问法是 VBE 自己的
 *  读回功能。
 *  【谁参与判据, 谁只作证据】
 *    · VBE_ProbeCurrentMode (4F03h) 【参与判据】—— 它没有"设置"形态, 只回
 *      BX = 当前 VBE 模式号。返回 1 = 拿到, 0 = 拿不到(通道不可用/调用失败)。
 *      "拿到一个不是 0117h 的模式号" = "硬件不在我们的模式里"最硬的证据。
 *    · VBE_ProbeScanLine (4F06h, 只用 BL=01h 取回形态) 【只作证据】—— 4F06h
 *      有设置形态, 输入寄存器选错就变成"改一行长度", 那是把硬件改坏的写法,
 *      所以它的结果只落日志与内存, 绝不单独触发重建。
 *    · VBE_ProbeDisplayStart (4F07h, 只用 BL=01h 取回形态) 【只作证据】。
 *  【拿不到时一律回 0xFFFF, 不保留旧值】旧值会被后来的判断误当成"现在的
 *   硬件状态", 那正是本项目反复吃过的"自证"陷阱。
 *  【依赖】需要与 display.c 配套的 minivdd.vxd(VBE_VXD_VERSION 0x0119):
 *   旧 VxD 的请求块是 48 字节, 不会回填 BX/CX/DX, 这六个字段会恒为 0,
 *   于是三个探针一律报"拿不到"并落 `PH: hwmode=FFFF` —— 明确的失效,
 *   不是静默的错误答案。版本不匹配时 QueryMiniVDDVersion 会再报一次警。 */
int      __far __pascal VBE_ProbeCurrentMode(void);   /* 1=拿到 0=拿不到 */
uint16_t __far __pascal VBE_ProbeModeValue(void);     /* 拿到时为模式号  */
int      __far __pascal VBE_ProbeScanLine(void);      /* 1=拿到 0=拿不到 */
uint16_t __far __pascal VBE_ProbePitchBytes(void);    /* 字节/扫描行     */
uint16_t __far __pascal VBE_ProbePitchPixels(void);   /* 像素/扫描行     */
int      __far __pascal VBE_ProbeDisplayStart(void);  /* 1=拿到 0=拿不到 */
uint16_t __far __pascal VBE_ProbeStartLine(void);
uint16_t __far __pascal VBE_ProbeStartPixel(void);

/*=== 【r139】DAC 见证组 ====================================================
 *  【为什么 r138 的三个 VBE 探针还不够】./138 的主机截图把病灶量成了数:
 *  桌面底色(RGB565 03EFh, 字节 EF,03)渲染成"黑 黑 青 青"四像素一组、
 *  任务栏底色(BDF7h, 字节 F7,BD)整条渲染成黑, 而那个青 = VGA 标准 16 色
 *  EGA 调色板的第 3 号、那个黑 = 索引 ≥ 16 的项。⇒ 硬件在【按 8bpp 扫描
 *  我们的 16bpp 显存】, 每字节横向复制成 2 像素, 并用一张"只有前 16 项"的
 *  调色板。像素格式与 RAMDAC 都【不是 VGA 寄存器】, 所以整组寄存器可以
 *  逐字节正确、几何判据成立, 而屏幕仍然是花的 —— ./138 的
 *  `sessend essentials (1=ours)=0001` / `need rebuild=0000` 就是这么来的。
 *  【两条新见证】
 *    · 4F03h(VBE_ProbeCurrentMode)接进判据 —— r138 只把它当打印, 这里让
 *      它参与"要不要重建"的决定。
 *    · DAC 印章(VxdDacStamp / VxdDacStampOk): 每次成功建立模式之后往
 *      DAC 前 8 项写固定值; DOS 盒的文本模式设置必然重写 DAC 前 16 项,
 *      所以"印章没了"= 我们的模式没有重新建立过。这是【硬件里的物证】,
 *      不是我们软件里的标志位。只在 bpp != 8 时盖章(8bpp 下 DAC 就是画面)。
 *    · VxdDacFingerprint 只作证据: 把"16 色调色板 + 其余全黑"这个形状变成
 *      可读的数, 下一轮不必再靠截图猜。
 *  【安全边界】三个读都只在 display.c 的 MaybeDeferredRestore(普通 ring 3
 *   DDI 入口)里调用: 写 3C7h 会把 DAC 切到读索引, 而主 VDD 自己也在写这套
 *   端口, 绝不能在它的切屏事务内部插读。
 *  【判据纪律】"读不到"一律回 0xFFFF / 置 *_UNREAD 位 = 不知道, 不许据此
 *   动硬件; 只有"读到不对"才回 0 / 置 *_GONE|*_MISMATCH 位 = 该动手。   */
#define HWBLIND_MODE_MISMATCH   0x0001  /* 4F03h 的模式号 != 我们的模式    */
#define HWBLIND_MODE_UNREAD     0x0002  /* 4F03h 取不到(不下结论)          */
#define HWBLIND_STAMP_GONE      0x0004  /* DAC 印章没了                    */
#define HWBLIND_STAMP_UNREAD    0x0008  /* DAC 读不全(不下结论)            */
#define HWBLIND_TEXT_PALETTE    0x0010  /* DAC 形状像"前 16 项 + 其余全黑" */

extern uint16_t g_hwBlindEv;                    /* 上面那五个位的当前值 */
int      __far __pascal VxdHwBlindNeed(uint16_t expectMode);   /* 1=该重建 */
void     __far __pascal VxdDacStamp(void);                     /* 盖章     */
uint16_t __far __pascal VxdDacStampOk(void);    /* 1=在 0=没了 FFFF=读不到 */
uint16_t __far __pascal VxdDacFingerprint(void);/* 高字节=非黑项数, 低字节=其中<16 的 */

/*--------------------------------------------------------------------------
 * 【r140】“这块 VGA 现在是不是 System VM 的” —— 外部见证(不是我们的标志位)。
 *   见 drv.c 的 VxdVgaOwner 长注: 读 GCR06h(3CEh/3CFh), 读回 FFh 就是
 *   “端口不回话”。./139 的日志里它恰好在“主 VDD 把屏交给 DOS VM”那一拍
 *   从 0005h 变成 00FFh, 而 00FFh 不是任何合法的 GCR06 值
 *   (bit0..3 = 内存映射, 只有 0..F)。
 *   DDK 的 VDD_SET_USER_FLAGS 页原文: "When this flag is set, the display
 *   driver must not draw on the display surface ... doing so would corrupt
 *   the appearance of the MS-DOS session." —— 本轮把这条要求落成端口级的门。
 *------------------------------------------------------------------------*/
#define VGAOWN_GCR_DEAD         0x0001  /* GCR06 读回 FFh: 图形控制器不回话 */
#define VGAOWN_ATC_TEXT         0x0002  /* 已有 AR 样本且说“在文本态”       */
extern uint16_t g_vgaOwnEv;             /* 最近一次的见证位图(0 = 我们的)   */
extern uint16_t g_vgaOwnN;              /* 见证被问过多少次(校验用)         */
extern uint16_t g_vgaOwnG06;            /* 最近一次 GCR06 的原始读数        */
uint16_t __far __pascal VxdVgaOwner(void);
int      __far __pascal VxdVgaIsOurs(void);


int      __far VBE_EnumModes(uint16_t far *outModes, int maxModes,
                       uint32_t far *pVram, uint16_t far *pVer);

/* mini-VDD(本项目的 VxD) 通道 */
BOOL     __far VxD_Connect(void);
uint16_t __far QueryMiniVDDVersion(void);
uint16_t __far QueryVesaSupportCount(void);
BOOL     __far PushStateToMiniVDD(MODEINFOBLOCK far *mi, uint16_t modeNum);
BOOL     __far FetchStateFromMiniVDD(VBESHAREDSTATE far *out);
/* --- r99: mini-VDD 分派调用追踪的落盘与 Heal 通道(实现在 drv.c) --------
 * VxdTraceDump:     在任意 ring-3 入口调用; 只打新记录, 已限流+静默失败。
 * VxdSetHeal:       Heal=1 时置位 —— 之后 DISPLAY_DRIVER_DISABLING(26)
 *                   计数一变就置一次自愈请求。
 * VxdTakeHealPending: 取走(并清)自愈请求; display.c 据此重建模式。      */
uint16_t __far __pascal VxdTraceDump(void);  /* 返回: 本次是否打出了新记录 */
void     __far __pascal VxdSetHeal(uint16_t on);
uint16_t __far __pascal VxdTakeHealPending(void);

/* --- r100: 让"回调路径"变安全的四件工具(实现在 drv.c) ------------------
 * 【为什么需要】ResetHiResMode 回调由主 VDD 在它自己的切屏事务内部调用,
 * 而且实测会切进我们 ring-3 代码执行中途(日志里出现过两行粘在一条物理
 * 行上 —— 外层正卡在 DiagWriteLog 的两次 INT 21h 之间)。所以回调里:
 *   ① 不能发 BIOS/VESA 调用 → VxdSetInCallback(1) 期间 VbeExecBios 直接
 *      拒绝并返回 0xFFFF(真机三次蓝屏都在那次嵌套 4F02 上);
 *   ② 不能打日志(DOS 重入) → 只能 CbRingPut(纯内存), 由 CbRingDump 在
 *      安全上下文里落盘;
 *   ③ 策略字节(VddVirtualize/VesaPolicy/HiresAns/Restore, r101 又加了
 *      VddBank/Int10Mode)由 VxdSetPolicies 存下, PushStateToMiniVDD 推送
 *      时填进共享状态;
 *   ④ 真正的模式重建必须等"切屏事务已收尾": VxdTakeSafeToSetMode 返回
 *      1 = VxD 侧 POST_VGA_TO_HIRES(7) 的计数刚刚变化(DDK 时序第 6 步)。 */
void     __far __pascal VxdSetPolicies(uint16_t vddEdx, uint16_t vesa,
                                       uint16_t hires, uint16_t restore,
                                       uint16_t bank, uint16_t int10,
                                       uint16_t vgafix, uint16_t dispi,
                                       uint16_t int10fix, uint16_t hiresfix,
                                       uint16_t memcfix );  /* r147 */
void     __far __pascal VxdSetInCallback(uint16_t on);
void     __far __pascal CbRingPut(uint16_t code);
void     __far __pascal CbRingDump(void);
/* 【r136】自上一次 CbRingDump 以来【没能放进环】的条数(只读, 不清)。
 *   非 0 ⇒ 这一轮的环记录不完整, 判读时必须按"证据缺失"处理。      */
uint16_t __far __pascal CbRingDropped(void);
/* 【r136/r137】环里【待落盘的记录条数】(只读)。调用者必须先问它。
 * 【为什么不能省这一步】MaybeDeferredRestore 挂在【每一次光标 tick】上
 *   (CheckCursor/MoveCursor), 那是每秒几十到几百次的热路径, 而 CbRingDump
 *   即使一条记录都没有也要走一遍 open/seek/write/commit/close(6 次 INT 21h)。
 *   把"有没有东西要写"变成一次近调用 + 一条比较, 这一枪才是免费的。
 * 【r137 修正 —— r136 的实现与这里的语义不一致】
 *   r136 里这个函数返回的是 `(s_cbRingN != 0) ? 1 : 0`, 也就是一个【标志】,
 *   而调用点写的是 `CbRingPending() > 1` —— 那个条件【永远为假】, 于是
 *   "每次进入安全上下文就落盘"这条 r136 的改动一次都没有执行过(./136 的
 *   整份日志里 CbRingDump 只在重建路径上响过一次, 就是这条死代码的直接
 *   证据)。返回条数才是调用者真正要知道的量, 阈值也就有了意义。 */
#define CB_RING_MIN_DUMP 4
uint16_t __far __pascal CbRingPending(void);
/* 【r137】属性控制器读/写(3C0h/3C1h, 触发器纪律与 VxdReadAtcReg 同一套)。
 *   写路径只用于"把已知良好样本里的 AR10..AR14 放回去": 索引字节的
 *   bit5(palette enable / PAS)一律保持 1, 写完把触发器交回索引态。 */
uint16_t __far __pascal VxdWriteAtcReg(uint16_t idx, uint16_t val);
/* 【r137】只读判据: 属性控制器那几个"模式状态"寄存器现在是否等于样本。
 *   1 = 相等(或没有样本), 0 = 不相等。不写任何端口。 */
int      __far __pascal VxdHwRegArOk(void);
uint16_t __far __pascal VxdTakeSafeToSetMode(void);
/* 【r104】VxdTakeVgaSessionEnd: 取走(并清)一次"DOS 虚拟化会话已收尾"请求。
 *   产生: drv.c 的 VxdTraceDump 看到 VBE_TR_State 的 TRAPS 位(bit1)
 *         从 1 落到 0 —— ./103 四份日志里, 那是"主 VDD 这次 DOS 虚拟化
 *         结束了"唯一持续可判的边界(bit0 在窗口化会话里被 11/12 号反复
 *         开关, 不能当边界用)。
 *   消费: display.c 的 MaybeDeferredRestore -> 强制重建一次桌面。
 *   【为什么需要它】./103 的 B0/B1FS/B1WS 里主 VDD 的 ResetHiResMode 回调
 *   一次都没被调用(只有 B2 调了一次), 而 bRestorePending 原本只有"那个
 *   回调"和"Heal=1"两个置位者 ⇒ 没有任何人去重建桌面。这个是第三个。
 *   开关: system.ini [omninine] VgaEdge=0 可关闭该路径(回到 r103 行为)。   */
uint16_t __far __pascal VxdTakeVgaSessionEnd(void);
/* 【r108】VxdTakeVgaToHiresEdge: 取走(并清)一次"主 VDD 刚走完一趟
 *   hires→VGA→hires 往返"的请求。
 *   产生: drv.c 的 VxdTraceDump 看到 VBE_TR_Evt[POST_VGA_TO_HIRES](= 7 号
 *         服务的累计次数)变化。./107 的 t3a 时间轴实测: 这一趟往返发生在
 *         DOS 会话【进行中】(会话开始 +2.5 秒), 而 TRAPS 位落下(会话收尾)
 *         还要再等 1.5 秒 —— 旧代码只认后者, 于是那 1.5 秒里屏幕就是花的。
 *   消费: display.c 的 MaybeDeferredRestore -> 立刻强制重建一次桌面。
 *   开关: system.ini [omninine] VhEdge=0 可关闭该路径(回到 r107 行为)。   */
uint16_t __far __pascal VxdTakeVgaToHiresEdge(void);
/* 【r108】VxdTakeSessionStartEdge: 取走(并清)一次"DOS 虚拟化会话刚开始"
 *   请求(TRAPS 上升沿), 供 display.c 的 EntryHeal 使用(默认关)。
 *   它存在的唯一理由是回答"花屏是从打开 DOS 盒那一刻就开始的吗":
 *   若是, 病根在【入口】(DOS VM 的 ROM BIOS 写了主 VDD 不管辖的扩展
 *   寄存器), VhEdge 只能晚几秒把屏幕救回来; 那条边沿就是入口侧的激进
 *   补救。开关: system.ini [omninine] EntryHeal=1。                     */
uint16_t __far __pascal VxdTakeSessionStartEdge(void);
/* 【r109】VxdStateBitsNow: 读一次"最近一次采样的切屏状态位"(不清、无副作用)。
 *   返回值 = mini-VDD 在 blob 头 204 维护的那个字节:
 *       bit0 = 硬件已被切到 VGA(4 平面/文本)通路
 *       bit1 = ENABLE_TRAPS(13) 生效中
 *       bit2 = DISPLAY_DRIVER_DISABLING(26) 已发生
 *   消费: display.c 的 MaybeDeferredRestore —— 入口侧补救(EntryHeal)在动手
 *   之前拿 bit0 判"这次 DOS 会话是不是真全屏": 置起说明主 VDD 真的把硬件
 *   交给了 DOS 盒, 那时我们插一次 4F02 就是跟 DOS 盒抢硬件。
 *   【语义边界】它只在 VxdTraceDump(我们的 ring-3 入口)被刷新, 是"最近一次
 *   采样", 两次采样之间主 VDD 若自己开关过这里看不到 —— 所以它只配当
 *   "抑制条件", 不能当"必须动手"的依据。                              */
uint16_t __far __pascal VxdStateBitsNow(void);
/* 【r109】PortProbeRead: 直接读 VGA/VBE 寄存器(CRTC/时序器/GCR/3CCh/3C6h),
 *   把"硬件此刻的光栅几何与像素格式"落进日志 —— 这是破解 ./108 那屏
 *   "2 像素周期条纹"(显存对、输出不对)唯一能读硬件的手段。只写索引、只读
 *   数据, 不改任何显示状态; 但"端口读"在本机是否被允许此前未证, 故由
 *   display.c 挂在 [omninine] PortProbe=1(默认 0)后面。调用前提: 驱动已接管
 *   屏幕(Init 之后、PhysicalEnable 之后), 否则读到的可能只是 VDD 的虚拟值。*/
void     __far PortProbeRead(const char *why);

/* 【r117】DispiPortProbe —— 读(并可选择修复)VBE_DISPI(1CEh/1CFh)。
 *   与 PortProbeRead 并列但【互不替代】: 那一个读的是标准 VGA 端口
 *   (3C0h-3DFh, 主 VDD 会虚拟化/影子化), 这一个读的是 Bochs 家族的私有
 *   扩展端口 —— 后者才是"Windows 不知情之下 xres/yres/bpp/pitch 被改掉"
 *   的唯一通道。只写索引、只读数据, 不改显示状态; 只有"ID 认得出 +
 *   传进来的期望值不符 + allowFix"三条同时成立时才写回正确格式。
 *   参数: why=日志行(可为 NULL), expW/expH/expBpp=本机当前模式的宽/高/位深,
 *         expPitch=本机当前每行字节数, allowFix=是否允许修复(策略位)。
 *   自证: 每次落 DISPI probe verdict=(1 一致 / 0 不符); 修完再落
 *         DISPI fix result=(1 成功 / 0 仍然不符)。                     */
void     __far DispiPortProbe(const char *why, WORD expW, WORD expH,
                              WORD expBpp, WORD expPitch, int allowFix);

/* --- 【r111】硬件指纹: 用 VGA 寄存器本身判"硬件在不在我们的模式里" ------
 * 【为什么必须要它】到目前为止, 驱动侧所有"模式对不对"的判据都是自证的:
 *   VerifyDisplayMode = VxD 软件状态(currentMode, 没人改过它)+ 帧缓冲
 *   写/读回(选择子映的是显存, 与 CRTC 在什么模式无关)⇒ ./109 的实测是
 *   "整屏花而 pre-verify ok=0001"。唯一能读【硬件真相】的就是 VGA 寄存器,
 *   而 r110 的 ./109 t3 / ./110 t8 已经证明: 在"重建前/重建后"这两个位置
 *   读 CRTC(3D4h)/时序器(3C4h)/GCR(3CEh)是安全的(值自洽、开机正常)。
 * 【怎么用】一组 12 个字节:
 *   读一次(VxdHwFpRead) → 与基线比(VxdHwFpDiff) → 0 才敢说"硬件是我们的";
 *   一次成功的重建之后(VxdHwFpStore)把它记成新基线。
 * 【为什么挂在 HwFp 开关后面】端口读在本机是否永远安全, 只有两个采样点
 *   的证据; 默认开(它决定回落层要不要动手), 但随时可关。             */
void     __far __pascal VxdHwFpRead(void);       /* 读当前值(不发日志)   */
int      __far __pascal VxdHwFpStore(void);      /* 当前值 -> 基线; 1=收下, 0=不像我们的模式(拒收) */
int      __far __pascal VxdHwFpDiff(void);       /* 0=与基线一致; -1=无基线; >0=差异位数+1 */
int      __far __pascal VxdHwFpDump(const char *why); /* 打日志(diag); 返回 Diff */
/* 【r111】VxdAllowBios(budget): 给"回调上下文里的 BIOS 调用"开一份有界预算。
 *   r100 的硬护栏是"回调期一律拒绝"(x230 三次死在回调里那次嵌套 4F02)。
 *   r111 按微软文档把恢复路径搬回回调里, 于是这道护栏变成"默认拒绝 +
 *   官方路径显式领预算"; 预算用完立即作废(嵌套的第二层仍然被拒)。
 *   一次官方恢复需要 3~6 次 BIOS 往返(4F02 + 0Fh 探针 + 4F01), 所以给的
 *   是有界预算而不是"一张一次性通行证"。开关见 display.c 的 CbRestore。 */
void     __far __pascal VxdAllowBios(uint16_t budget);
void     __far __pascal VxdDenyBios(void);       /* 作废没用完的预算 */
/* 【r112】g_inRebuild: "我们自己的那一段模式重建正在跑"。
 *   【为什么必须有】./111 抓到了硬证据: t5 的回落层里, 4F02 【成功】之后的
 *   那次 4F01 被 r100 护栏拒了(日志 `DeferredRestore: rebuild refused
 *   (in VDD callback)=0001`, 而且 4F01 之后没有 `exec4F01 ax=` 那一行)
 *   —— 也就是说主 VDD 的 ResetHiResMode 回调【切进了我们自己的重建序列
 *   中间】。护栏按设计把回调期的一切 BIOS 调用都拒了, 于是我们自己的
 *   第二次调用也被拒 ⇒ `SDM fail 4: no LFB` ⇒ 重挂, 再被切, 再失败。
 *   【语义】置起期间允许 BIOS 直通(那是"同一条序列的下一次调用", 不是
 *   回调里的嵌套调用); 与此配对的是 display.c 的回调侧: 回调发现
 *   g_inRebuild != 0 就【立刻返回】(只清 BUSY + 记环), 绝不在别人的
 *   重建里再套一层官方恢复。                                          */
void     __far __pascal VxdSetRebuild(uint16_t on);
uint16_t __far __pascal VxdGetRebuild(void);
/* 【r131】无条件骨架日志 —— 见 drv.c 的长注。三种构建(release/MIN/debug)
 *   都会写这一行, 不看 g_logLevel、不受任何构建宏影响: 这是 release 档
 *   唯一的行为遥测, 也是"release 与 debug 行为不一致"能被定位的前提。
 *   r131-marker: DiagHexPhase (unconditional skeleton log, all three builds). */
void     __far __pascal DiagHexPhase( const char __near *msg, uint16_t v );
/* 【r112】HwReg —— 硬件模式寄存器组的保存/回放(官方路径的"重设模式")。
 *   见 drv.c 的 VxdHwRegSave 长注: 这是"在回调里把硬件设回 Windows 模式"
 *   这条 DDK 义务的一条【不需要嵌套 V86 调用】的实现路径。              */
int      __far __pascal VxdHwRegSave(void);      /* 读并存下整组; 1=收下 */
int      __far __pascal VxdHwRegRestore(void);   /* 写回并【按写进去的值】读回比对; 返回差异数 */
/* [r134] The two functions that replace "a byte count decides the official
 *   path" with "the meaning decides it, the byte count stays as evidence":
 *     VxdHwRegDiffVsWritten : like VxdHwRegDiff, but compared against the
 *         values the replay actually wrote (the sample with SR01 normalised to
 *         the 8-dot clock / screen on) -- the r133 comparison was against the
 *         raw sample, so a deliberate normalisation was reported as a failure.
 *     VxdHwRegEssentialsOk  : the live bits that define "the hardware is in
 *         OUR mode": CR00/CR01/CR13 equal to the sample, CR17h bit7 (SE) set,
 *         CR11h bit7 (Protect) clear, SR01h bit0 (8-dot) set and bit5 (screen
 *         off) clear, GCR06h bit0 (graphics) set.  Returns 1 = yes.
 *     VxdHwRegMismatch      : enumerate the n-th differing register, one
 *         scalar at a time (NO pointer arguments on purpose: in this memory
 *         model a pointer parameter to a far pascal function mixes near/far
 *         sizes and wcc emits W112 -- see the same note in PredictEmuHdisp).
 *         VxdHwRegMismatchInfo(n) = (group << 8) | index, or
 *             0xFFFF = no such mismatch, 0xFFFE = no usable sample;
 *         VxdHwRegMismatchVal(n, which) with which = 0 -> the live value,
 *             which = 1 -> the value the replay wrote, 0xFFFF = no such entry.
 *         group 0 = CRTC, 1 = sequencer, 2 = GCR.
 *   r134-marker: VxdHwRegDiffVsWritten / VxdHwRegEssentialsOk / VxdHwRegMismatch. */
int      __far __pascal VxdHwRegDiffVsWritten(void);
int      __far __pascal VxdHwRegEssentialsOk(void);
/* r144: 4→7 武装期。
 *   【r145 修正语义】mini-VDD 那个字节有三个取值(0=本次开机没武装过 /
 *   1=正在持屏 / 2=已被 7 号解除, 见 minivdd.asm 的 TraceStub_7)。
 *   r144 的实现把原始字节直接返回, 而调用处写的是
 *   `if (VxdBoxArmedNow())` —— 于是 2(已释放)也被当成"盒子持屏"。
 *   r145 起本函数只在字节 == 1 时返回 1。 */
uint16_t __far __pascal VxdBoxArmedNow(void);
/* 【r145】边沿预算复位 —— 由 display.c 在一次【成功重建】之后
 *   (post-verify ok=1)调用。语义: 硬件刚被确认拉回我们的模式
 *   ⇒ 三个边沿计数清零(见 drv.c 的 s_edgeSkipEnd 长注; r146 起边沿
 *   一律置请求, 这三个计数只作记账)。 */
/* 【r146】VxdHiresInfo —— mini-VDD 侧"hi-res 快照/回放"的四个记账字节
 *   (blob VBE_TR_Hires + 0..3: valid / savedN / restoredN / lastDiff)。
 *   为什么让 ring 3 也读得到: 这三个数字回答的是本轮的那个问题 ——
 *   "回到 Windows 时, 我们自己那一份模式/寻址字节到底有没有被恢复?"
 *     valid=0        ⇒ 从来没在"我们的模式"里采到过快照(4 号那一拍
 *                      的线性指纹不成立) —— 那本身就是一个结论;
 *     restoredN=0    ⇒ 7 号那一枪没开(策略关 / 没有快照);
 *     lastDiff=0     ⇒ 回到 Windows 时那 10 个字节【本来就全对】
 *                      (主 VDD 自己恢复好了, 我们这一枪是空枪);
 *     lastDiff>0     ⇒ 它漏了几个字节, 数字在这一行。            */
uint8_t  __far __pascal VxdHiresInfo( uint16_t idx );
/* 【r147】VxdMemCInfo —— mini-VDD 侧持屏期补写的六个记账字节
 *   (blob VBE_TR_MemC)。它回答的是本轮唯一那个行为改动到底有没有发生:
 *     seenText>0 而 fixedN=0 ⇒ 判据还在挡(这次不该发生了: 判据已经是
 *                                "六个字节逐个比较");
 *     fixedN>0 且 stillBad=0  ⇒ 我们真的把文本取指值写进硬件了;
 *     fixedN>0 且 stillBad>0  ⇒ 写被挡回去了(端口虚拟化/写保护) —— 那是
 *                                完全不同的一条线索;
 *     gfx>0                    ⇒ 全屏进过 VGA 图形模式(我们一个字节没写,
 *                                这是有意的: 文本值不能喷到 mode 12h/13h 上)。
 *   idx: 0=seenText 1=fixedN 2=stillBad 3=gfx 4..9=lastBefore 10=dirty。 */
uint8_t  __far __pascal VxdMemCInfo( uint16_t idx );

void     __far __pascal VxdEdgeBudgetReset(void);
/* 【r145】边沿节流常量(单位 = DDI 调用次数)。
 *   用于 CFG23 落盘, 也用于判读"为什么某条边沿没有被接受"。 */
/* 【r146】VBE9X_EDGE_BURST / VBE9X_EDGE_HOLD 已删除 —— 见 drv.c 里
 *   s_edgeSkipEnd 上方那段长注: 那两个宏代表的"边沿节流"在 ./145 里
 *   事实上永久关掉了会话收尾这条边沿(0x2000 次 D D I 调用 ≈ 30 分钟),
 *   而那正是本轮要修的回归。边沿现在一律置请求。 */
/* 【r135】VxdEdgeDirNow: 最近一次"会话边沿"的【方向】(不清、无副作用)。
 *   返回: 1 = 回到 Windows(VGA -> hires), 2 = DOS 盒拿走屏幕(hires -> VGA),
 *         0 = 判不出(两条证据都没有)。
 * 【与 VxdStateBitsNow 的区别 —— 这就是本轮的核心结论】
 *   VxdStateBitsNow 读的是 mini-VDD 维护的【瞬时】位: bit0 由 4/5 号
 *   (PRE/POST_HIRES_TO_VGA)置起、由 7 号(POST_VGA_TO_HIRES)清掉, 而主 VDD
 *   在一次切屏事务里会成对地走这两路(所以 ./133 里它能置起, ./134 里同型
 *   的一拍却读不到)。它只能当"抑制条件"。
 *   本函数读的是【有序】证据, 两条, 都来自主 VDD 自己的动作:
 *     ① 事件计数器的【增量方向】(+0 POST_VGA_TO_HIRES = 回来; +1
 *        PRE_HIRES_TO_VGA = 盒子拿屏)。两者都动 = 事务里走了个来回, 单靠
 *        它判不出, 退到 ②。
 *     ② 48 号(PRE_INT_10_MODE_SET)记下的"主 VDD 最近一次为 VM 设了什么
 *        模式"(blob 头 206)。AX = 0x0003 这类【标准 BIOS/VGA 模式】
 *        (<= 0x13) = 它把屏幕交给了 DOS 盒; AX = 0x0072 这类扩展/伪模式
 *        = 它把 VM 设成了"高分辨率"。它是【有先后顺序的最后一个动作】,
 *        不会被采样错过 —— 这正是瞬时位做不到的事。
 *   【为什么可信】TRAPS 上升沿 = 一次切屏事务的【收尾】(DDK PRE_HIRES_TO_VGA
 *   页第 3/7 步: 事务开始时关陷阱、结束时恢复), 而模式设置(第 6 步)就在
 *   它之前 ⇒ 读到的 ② 必然是【本次】事务设的, 不是上一次的残留。    */
uint16_t __far __pascal VxdEdgeDirNow(void);
uint16_t __far __pascal VxdHwRegMismatchInfo(int n);
uint16_t __far __pascal VxdHwRegMismatchVal(int n, int which);
int      __far __pascal VxdHwRegDiff(void);      /* 当前 vs 存下的差异数; -1=无样本 */
int      __far __pascal VxdHwRegDump(const char *why);  /* 打日志; 返回 Diff */
int      __far __pascal VxdHwRegDiffDetail(const char *why); /* r127: 只列不同项 */
int      __far __pascal VxdHwRegOk(void);        /* 有没有可用样本 */
/* 【r128】把"样本本身对不对"暴露出来 —— ./127 的 r1 是靠
 *   `HwReg diff seq idx=0001 now=0003 sav=0002` 才看出样本存成了 9 点时钟,
 *   绕了一大圈。这两个读数让调用方可以【当场】把样本质量打出来。 */
uint16_t __far __pascal VxdHwRegSaveClocking(void);  /* 样本里的 SR01 原值 */
uint16_t __far __pascal VxdHwRegSaveIs8Dot(void);    /* 样本 SR01 bit0 */
uint16_t __far __pascal VxdHwRegNormCount(void);     /* 回放时归一化次数 */
/* 【r131】CRTC 17h(CRTC Mode Control)的显示使能位 —— 见 drv.c 的长注。
 *   VxdCrtMode17     : 读回 17h 原值(bit7 = SE 同步使能, bit6 = Word/Byte)
 *   VxdCrtSyncFix    : 先读; 只有在 SE = 0 时才把它补成 1 并读回自证。
 *                      0 = 本来就是 1(零写入) / 1 = 补回并确认 / -1 = 写不进去
 *   VxdHwRegProtect  : 样本里 CR11h bit7(保护位)的值 —— 用来证明 r131 那处
 *                      "寄存器号 11h -> 0Bh" 的修正真的生效(修正前这里恒 0)。
 *   r131-marker: VxdCrtSyncFix (CR17h bit7, Sync Enable).                 */
uint16_t __far __pascal VxdCrtMode17(void);
int      __far __pascal VxdCrtSyncFix(void);
uint16_t __far __pascal VxdHwRegProtect(void);
/* [r132] CR11h bit7 is the CRTC "Protect" bit (FreeVGA, CRT Controller
 *   Registers, Index 11h -- "when this field is set to 1, the CRTC register
 *   indexes 00h-07h ignore write access, with the exception of bit 4 of the
 *   Overflow Register (07h)").  While it is set every write to CR00h-CR06h is
 *   dropped by the chipset and CR07h keeps all bits except bit4, so neither a
 *   VBE mode set nor our register replay can put the horizontal/vertical
 *   timing back.  Clearing it before programming the timing registers is the
 *   textbook VGA rule; r131 wrote index 0Bh instead and therefore stopped
 *   clearing anything at all.
 *   VxdCrtProtectClear: read CR11h, and ONLY if bit7 is set write it back with
 *   bit7 cleared, then read back for self-check.
 *     0 = was already clear (not one byte written), 1 = cleared and verified,
 *    -1 = write did not take effect.
 *   r132-marker: VxdCrtProtectClear (CRTC Protect bit, index 11h). */
int      __far __pascal VxdCrtProtectClear(void);
/* [r133] What this machine needs is a raster that matches the frame buffer,
 *   and a raster can be latched from an earlier register state without any
 *   register read being able to show it (./129 ./130 ./132: every self-check
 *   says OK, the visible width is 128).  Two steps are exported for that:
 *     VxdCrtRelatch : re-program the whole mode from the known-good sample
 *                     with the display gate (CR17h bit7, SE) closed and then
 *                     opened LAST, so the surviving latch is built from the
 *                     complete register set.  -1 = no sample; otherwise the
 *                     difference count read back afterwards (0 = exact).
 *     VxdRastRun/De1/Trans/Vbl/Permille : display-enable duty cycle measured
 *                     on Input Status 1 (3DAh) -- read-only, bounded, no wait
 *                     loop.  Expected 762 per mille for 1024/1344, and 95 per
 *                     mille if the raster really is 128 wide.
 *   r133-marker: VxdCrtRelatch / VxdRastProbe. */
int      __far __pascal VxdCrtRelatch(void);
uint16_t __far __pascal VxdRastRun(void);
uint16_t __far __pascal VxdRastDe1(void);
uint16_t __far __pascal VxdRastTrans(void);
uint16_t __far __pascal VxdRastVbl(void);
uint16_t __far __pascal VxdRastPermille(void);
/* 【r126】VxdCrtProbeRead —— 给 display.c 的 CRTP 现场读数用的一次读端口。
 *   【为什么要一个函数而不是让 display.c 自己 in/out】CrtProbeNow 读 CRTC
 *   的 13h/0Ch/0Dh/17h/12h、时序器的 01h/04h、GCR 的 05h/06h —— 这九个
 *   (端口, 索引) 组合在 drv.c 里【已经】被 PortProbeRead 与 HwRegSave 读过
 *   无数次(./110/./111/./122 三台机器都活着回来了), 所以这里不新开一条
 *   端口访问路径, 只把同一套 idxread 借出去 —— 少一条路径就少一个未知。
 *   【选择子编码】高半字节选组, 低字节是索引:
 *     0x0nn = CRTC(3D4h/3D5h)   0x1nn = 时序器(3C4h/3C5h)
 *     0x2nn = GCR(3CEh/3CFh)    0x3nn = 属性控制器(3C0h/3C1h, r127)
 *     0x4nn = Misc Output(3CCh, r127, 索引被忽略)
 *     其它 = 返回 0xFFFF(不碰任何端口)
 *   读不到/端口不存在时返回 0xFF(硬件常态)—— 调用方据此区分"读到 FFFF"
 *   与"读到 FF"(前者只可能来自本函数的拒绝分支)。                     */
uint16_t  __far VxdCrtProbeRead(uint16_t sel);

/* 【r127】点时钟归位与状态读数(实现见 drv.c 同名长注)。
 *   VxdClockEnsure8Dot: 见 drv.c 的长注 —— "文本会话结束后把点时钟恢复成
 *     8 点"是【通用 VGA 收尾义务】(SR01 bit0), 不是为某张卡做的。
 *     0 = 本来就好; 1 = 写了并读回确认; -1 = 写了但读回仍未修好。
 *   VxdSeqClocking:     读回时序器 01h 原值(自证用)。
 *   VxdReadMiscOut:     读 3CCh(标准 VGA 上只读)。
 *   VxdReadAtcReg:      只读任意属性控制器索引(内部会瞬时动一次索引;
 *                       越界返回 0xFFFF 且不碰端口)。                  */
int       __far __pascal VxdClockEnsure8Dot(void);
/* 【r128】只观察版: 读回 SR01 判断是否 8 点, 【一个字节都不写】。
 *   0 = 已经是 8 点; -2 = 不是 8 点。默认路径用的是它(Clk8=1)。 */
int       __far __pascal VxdClockObserve8Dot(void);
uint16_t  __far __pascal VxdSeqClocking(void);
uint16_t  __far VxdReadMiscOut(void);
uint16_t  __far VxdReadAtcReg(uint16_t idx);

/* 【r130】光栅几何 —— "扫描输出一行有多少像素"的直接读数与判据。
 *
 * 【为什么必须有它】r130 的三条实测证据(主机截图与虚拟机截屏逐行配色一致、
 *   纵向 740 行完全对齐、而横向只有 320 列)把病灶锁死在"扫描输出每行的
 *   像素数"上, 而标准 VGA 上决定它的只有 CRTC 01h(再乘每字符点数)。
 *   r129 之前 00h/01h 两个字节从未被读过, 所以"几何到底是多少"一直没有数据。
 *
 *   VxdReadCrtHTotal   -> CRTC 00h 水平总数
 *   VxdReadCrtHDisplay -> CRTC 01h 水平显示结束
 *   CrtGeometryLooksRight(wantW) -> 1 = 一行像素数 == wantW(自洽);
 *                            0 = 不一致(r130 认定的病灶);
 *                            2 = 端口无响应或 wantW=0, 报"不知道"
 *                                (不去动硬件)。
 *   期望宽度【由参数传入】而不是在 drv.c 里引用 display.c 的 wScrX ——
 *   保持 drv.c 不反向依赖 display.c 的文件级全局。
 *   四个读数留在下面的全局里供日志使用(取用时刻 = 上一次调用那一刻)。 */
uint16_t  __far VxdReadCrtHTotal(void);
uint16_t  __far VxdReadCrtHDisplay(void);
int       __far __pascal CrtGeometryLooksRight(uint16_t wantW);
extern uint16_t CrtHDe;      /* CRTC 01h 原值          */
extern uint16_t CrtHTo;      /* CRTC 00h 原值          */
extern uint16_t CrtHDots;    /* 每字符点数(8 或 16)    */
extern uint16_t CrtHWidth;   /* 算出来的"一行像素数"   */

extern uint16_t g_cbVmRefuse;                    /* r111: 回调期 VM 自检拒绝次数 */
extern uint16_t g_cbBiosUsed;                    /* r111: 官方路径用掉的 BIOS 次数 */
extern uint16_t g_cbBiosBusy;                    /* r111: 被"同时只有一次"闩拒了几次 */

/* --- r103: 可见帧缓冲原始字节取样(实现在 display.c) ---------------------
 * VxdTraceDump 里, 每当 mini-VDD 的 VGA/hires 状态位变化(主 VDD 正在
 * 进/出 DOS 全屏会话)就打一次: 4 组字, 每组 2 个字 = 8 行 "FB sample"。
 * 作用是把"花屏到底是谁写的"从"看缩放过的截图猜"变成"读帧缓冲的字节"。
 * 没有屏选择子/几何未定时静默返回。                                    */
void     __far __pascal DiagFbSample(void);

/* 回调期事件码(CbRingPut 的 code, 会在安全上下文里打成 "CB evt=XXXX") */
#define CB_EVT_CALLBACK_ENTER   1   /* 回调进来(低字节=会话内第几次)      */
#define CB_EVT_BIOS_REFUSED     2   /* 回调上下文里的 BIOS 调用被护栏拒绝 */
#define CB_EVT_SAFE_SIGNAL      3   /* 收到 POST_VGA_TO_HIRES 安全信号    */
#define CB_EVT_WORKER_RUN       4   /* 延迟重建真正执行                   */
#define CB_EVT_WORKER_NO_SIGNAL 5   /* 等不到安全信号, 走了 N 次兜底      */
#define CB_EVT_HEAL_IMMEDIATE   7   /* 【r109】入口侧补救: 不等信号直接做 */
#define CB_EVT_VH_ARM           8   /* 【r110】VhEdge 边沿被取走(低字节=第几次)*/
#define CB_EVT_REARM            9   /* 【r110】重建被拒 -> 重新挂起(低字节=第几次)*/
#define CB_EVT_CB_RESTORE       10  /* 【r111】回调里直接执行官方恢复(低字节=第几次)*/
#define CB_EVT_CB_VMREFUSE      11  /* 【r111】回调期 VM 自检拒绝嵌套 BIOS(低字节=第几次)*/
#define CB_EVT_GATE_HOLD        12  /* 【r111】回落层被抑制闸门拦下(低字节=第几次)*/
#define CB_EVT_BIOS_BUSY        13  /* 【r111】嵌套 BIOS 直通被"同时只有一次"闩拒绝 */
#define CB_EVT_CB_HWREG         14  /* 【r112】回调里寄存器回放的读回差异数(低字节) */
#define CB_EVT_CB_NESTED        15  /* 【r112】回调切进了我们自己的重建序列(低字节=第几次) */
/* 【r134】官方路径判据 / 重绘的硬证据(全部走内存环, 因为文本日志会被
 * 闸门成片丢掉 —— ./133 的 `LOG skip Reent=0640` 就是 1600 行)。
 * 逐项点名用 19..22 四个码: 每个不等的寄存器占两条环记录,
 * 第一条 = (码<<8)|((组<<5)|索引), 第二条 = (now<<8)|sav。
 * 【编号不许重叠】19..22 是"码 + k", 所以 16..18 留给别的用途。 */
#define CB_EVT_CB_ESSENTIALS    16  /* 【r134】语义判据结果(低字节: 1=成立)   */
#define CB_EVT_REPAINT          17  /* 【r134】真的向 USER 请求了整屏重绘      */
#define CB_EVT_REPAINT_DEFER    18  /* 【r134】请求被 UserRepaintDisable 推迟  */
#define CB_EVT_CB_MM_REG        19  /* 【r134】第 k 个不等的寄存器(19..22)     */
/* 【r135】会话边沿的"方向"与"那一刻做了什么"(同样只走内存环)。
 * 【为什么非有不可】./134 的日志把 r134 的因果【证不出来】: 那条边沿闸门的
 * 输入(mini-VDD 的 VGA 状态位 bit0)在同一台机器、同一套操作下两次运行给了
 * 相反的读数 —— ./133 采样到 0x53(bit0 置起)于是抑制, ./134 采样到 0x52
 * 于是放行, 而两次的 ENTR 边沿完全同型(都是 `i10 n=0004 / i10 ax=0003`,
 * 即"主 VDD 刚把 VM 切进 VGA 文本模式 3")。bit0 只在设/清它的那对
 * PRE/POST 钩子之间为真(./133 实测: 会话开始后 2 个 tick 就清了, 而会话
 * 还跑了 650 个 tick), 所以"采样到哪个值"是一场赛跑, 不是判据。
 * r135 改成用【有序】证据判方向(见 VxdEdgeDirNow), 并把每一次判定写进
 * 内存环 + 日志 —— 日志会被闸门成片丢掉(./134 的 `LOG skip Reent=0DEB`
 * = 3563 行), 环不会。 */
#define CB_EVT_EDGE_DIR         23  /* 【r135】边沿方向(低字节: 1=回Windows,2=盒子拿屏,0=判不出) */
#define CB_EVT_SESS_NOSET       24  /* 【r135】会话收尾只重绘(绝不在主 VDD 事务里设模式) */
#define CB_EVT_SESS_DEFER       25  /* 【r135】会话收尾把重建推到事务之后(低字节=第几次) */
#define CB_EVT_SESS_SNAP        26  /* 【r135】会话期间的只读硬件快照已取(低字节=第几次) */
#define CB_EVT_POST_SET         27  /* 【r135】推迟的重建在事务之后真正执行(低字节=第几次) */
#define CB_EVT_VH_SKIP          28  /* 【r135】VhEdge 被方向判据挡下(盒子还拿着屏) */
/* 【r136】29 = 环溢出: 低字节 = 从上一次落盘以来【丢掉】的条数(模 256)。
 * 【为什么必须有它】r135 的环是"放不下就静默丢弃"(CbRingPut 满了直接 return),
 *   于是"日志里没有某条记录"既可能意味着"那次决策没发生", 也可能意味着
 *   "环满了被扔掉" —— 两者在 ./135 的日志里【无法区分】, 而那恰好是本轮
 *   要判定的东西(ENTR 边沿的方向记录到底写没写)。
 *   这一条把"丢了多少"变成显式读数: 只要它出现, 就必须按"日志不完整"看。
 * r136-marker: CB_EVT_RING_DROP (ring overflow is now visible, not silent).   */
#define CB_EVT_RING_DROP        29
/* 【r136】30 = 会话【开始】边沿的决定(低字节: bit0=硬件已是我们的模式,
 *   bit1=因此不设模式只重绘)。与 24 号(会话收尾)镜像, 让两侧的"进/出各
 *   自怎么决定"都能在环里被看见 —— ./135 的教训是进入方向的日志闸门正好
 *   在最关键那一拍把 DiagHex 丢掉, 而环不受闸门影响。               */
#define CB_EVT_SESS_START       30

/* 帧缓冲选择子：【r76】由本驱动(ring 3)用 DPMI 0800h 映射物理地址 +
 * DPMI LDT 选择子自建。物理地址取自 VBE 4F01h 的 PhysBasePtr。
 * 返回 0 = 失败(调用方让 Enable 干净失败, 回退 VGA —— 绝不允许拿
 * 一个不可用的选择子去写帧缓冲, 那正是 0D 蓝屏的现场)。            */
uint16_t __far LfbSelector_Ensure(uint32_t physBase, uint32_t bytes);
void     __far LfbSelector_Release(void);

/* LfbAccessOffset —— 访问帧缓冲要用的 32 位偏移(r80)。
 *   0 基址(flat)选择子形态 → DPMI 映射返回的线性地址;
 *   退回形态(选择子基址=帧缓冲) → 0。
 *   用途: ① DIBENGINE.deBitsOffset; ② FBCLEAR 的 _FbClearOff。同源! */
uint32_t __far LfbAccessOffset(void);

/* --- r82: 帧缓冲"可访问范围"与"可信显存上限" ---------------------------
 * LfbWindowBytes —— 从帧缓冲起点算起、选择子真正可访问的字节数
 *   (= 映射字节数 - 页内偏移)。自检与显存上报都必须止步于此。
 * LfbSetVramCeiling —— 自检发现"报的显存写不进去"时下调可信上限(0=不设限),
 *   此后 ReportVram 会用它钳制上报给主 VDD/DIBENG 的显存总量。      */
uint32_t __far LfbWindowBytes(void);
void     __far LfbSetVramCeiling(uint32_t bytes);

/* --- r78 取证导出 ---------------------------------------------------------
 * LfbSelRecheck —— 重新读一次帧缓冲选择子的描述符并落盘: 基址/访问权与
 *   建档时不一致(尤其是存在位被清)就是被 VDD/宿主改过, GDI 载入即 #GP。
 * DiagModuleMap —— 把各模块 DGROUP 选择子/基址、DIBENG 各入口代码段选择子
 *   一带, 以及 0x0320..0x04A0 区间所有存在的 LDT 描述符落盘, 用于把蓝屏
 *   地址 SSSS:OOOOOOOO 归属到具体模块(r76b/r76c/r77 的 0427:00002B70
 *   就是靠这个定性的)。非诊断构建下两者都是空函数。                  */
void     __far LfbSelRecheck(void);
void     __far DiagModuleMap(void);

/* --- r81 取证/运行时配置导出 ---------------------------------------------
 * 【安全原则】字节转储只对"有出处的段"执行(本模块段、模块数据库段表里的
 *   段、API 远指针指向的段) —— r79 对 LDT 盲扫读段内容, 读到 DPMI 惰性堆
 *   的未提交页, 直接蓝屏 0E。以下四个入口都遵守该原则。            */
void     __far DiagSegMap(void);          /* r81 主力: 出处可查的段地图   */
void     __far DiagLogPtr(const char *tag, uint32_t p); /* 标签=SSSS:OOOO  */
void     __far DiagSetDumpOff(uint16_t off);  /* 机器码窗口偏移(sys.ini) */

/* DescRead —— r94: 描述符只读解码(DPMI 000Bh, 无副作用)。
 *   display.c 的 DIBENG DGROUP 自证要用它: 先判定"哪些入口落在小代码段
 *   (= 装载器造的数据段装载桩)", 再验证桩里的立即数是不是一个合法的 3 环
 *   可写数据段。出参必须是 __far *(本工程 -mm + SS≠DS)。            */
int      __far DescRead(uint16_t sel, uint32_t __far *pbase,
                        uint32_t __far *plim, uint16_t __far *pacc);

/* DiagSetDumpLen —— r85: 机器码窗口宽度(system.ini [omninine] DumpLen, 16..512)。
 *   加宽后, 蓝屏段那一段【整个函数体】都会落进日志, 便于反汇编定身份。 */
void     __far DiagSetDumpLen(uint16_t len);

/* DiagCrashSite —— r83: 把上次蓝屏的 SSSS:OOOO 变成"出事那条指令的机器码"。
 *   sel 存在/S=1/DPL=3/可读/界限覆盖/基址在模块内存区 六道闸门照旧, 只对
 *   "来自蓝屏的地址"放宽 <64KB 那一条(那条指令刚被执行过, 页必然已提交)。
 *   同一选择子重复喂入只打一次。                                    */
void     __far DiagCrashSite(uint16_t sel, uint16_t off);

/* DiagDumpWin —— r96: 按【相对入口的偏移】把引擎代码抄进日志。
 *   display.c 在 Enable 末尾对 DIBENG 调色板族(槽 23/24/25/26/31/36)各抄
 *   三窗(入口-0x40/-0x80/-0xC0): 选择子每台机器不同, 相对偏移却是常量,
 *   于是"崩在引擎某个入口前 0x7B 字节"这类现场可以在【没有任何地址输入】
 *   的情况下自证。闸门与转储同规矩(DiagDumpAt relaxBig=0, 六道一条不放)。*/
void     __far DiagDumpWin(uint16_t sel, uint16_t off);

/* --- r107 时间轴、选择子探针、边沿探针 ------------------------------------
 * 【为什么先做时间轴】./104 的 .bsod 日志尾部到底是不是"崩溃现场", 一直是
 *   所有蓝屏分析的隐含前提却从未被证实: 日志是静态缓冲 + 逐行 commit 的,
 *   崩溃完全可能发生在最后一行之后很久(那时尾部只剩"最后一次活动")。
 *   有了 tick, 把最后一行与文件落盘时刻(主机 mtime)一比即知。
 *
 * DiagTickNow —— BIOS 数据区 0040:006C 的 32 位 tick(18.2Hz, 55ms/拍,
 *   午夜归零; 只读一个 DWORD, 无 DOS 调用、无 VxD 调用, 任何上下文安全)。
 * DiagMark —— 一行 "MARK <tag> T=XXXXXXXX C=XXXX"。T = tick, C = 心跳序号。
 *   关键节点(config/状态边沿/每次 DDI 首调)与 ~2 秒一次的心跳都走它;
 *   若整份日志里 T 一直不变 = 这个 tick 在 System VM 里没人维护(那本身
 *   也是一个必须知道的读数, 下一轮改读 DOS 时间)。
 * DiagBeat —— 心跳(每 ~36 拍 = 2 秒最多一行), 只在 VxdTraceDump 里调。
 *
 * DiagSelProbe —— 把蓝屏段号 SSSS 尽量问清楚(它可能属于 GDT, 如
 *   `3A43:3D726964` 的 3A43 —— RPL=3 且 TI=0 ⇒ GDT 选择子, 本工程此前的
 *   LDT 地图【永远问不到它】)。两路, 逐路退化、每路都留痕:
 *     ① DPMI 000Bh(LDT 描述符, 只读)成功 → 完整描述符行(在 LDT 里);
 *     ② 否则 DPMI 0006h 取基址 —— 宿主若支持 GDT 选择子, 这一路给出
 *        "它在 GDT 里, 基址 = X"(基址非 0 = 真实存在的 32 位段);
 *        基址 0 = 宿主不支持, 或这个段号根本不存在(后者 = 野跳)。
 *   【曾试过并放弃的第三路】LAR/LSL(386 指令, 无效选择子只清 ZF)——
 *   本工程 C 编译不带 386 开关(lib286/win, 无 -3), Watcom 的 #pragma aux
 *   一写 eax 就 E1156。为这一路改 makefile 的 CPU 级别不划算, 见 drv.c。
 *
 * DiagEdgeProbe —— 切屏状态位发生边沿时的探针(display.c 实现)。
 *   drv.c 的 VxdTraceDump 在"状态位变化"那一刻调用它 —— 那是 DOS 会话
 *   开始/结束唯一可观测的时刻, 也正是 ./104 蓝屏所在的那一瞬间。      */
uint32_t __far DiagTickNow(void);
void     __far DiagMark(const char *tag);
void     __far DiagBeat(void);
void     __far DiagSelProbe(uint16_t sel);
void     __far DiagEdgeProbe(void);

/* LfbSelModeSet —— 帧缓冲选择子形态(0=0 基址 flat + 线性偏移[默认],
 *   1=选择子基址=帧缓冲 + 偏移 0)。来自 system.ini [omninine] SelMode;
 *   两种形态各自被拒时都会自动退到另一种, 所以设错只会换个形态, 不会
 *   拿不到选择子。                                                  */
void     __far LfbSelModeSet(int mode);

/* LfbSelVerifySet —— r89: 描述符回读校验层开关(1=开[默认], 0=回到 r88 的
 *   "只看存在位"行为)。来自 system.ini [omninine] Verify。
 * LfbSelVerify —— r89: 重读已建选择子的描述符, 校验 base/limit 是否真的
 *   覆盖"要交给 DIBENG 的访问窗口"(形态 A 还要求 base 必须为 0)。返回
 *   1 = 通过或无法判定, 0 = 确定不一致(此时窗口会被钳到真实可访问范围)。
 *   display.c 在拿到选择子后、帧缓冲自检之前调用一次。            */
void     __far LfbSelVerifySet(int on);
int      __far LfbSelVerify(void);

/* display.c 导出：模式缓存表里最大的帧缓冲字节数(pitch*yRes 的最大值)。
 * drv.c 用它做两件事: 兜底估算显存总量(见 GuessVramSize)、给 DPMI
 * 映射定尺寸。表空(未枚举)时返回 0。                              */
uint32_t __far VbeMaxFramebuffer(void);

/* VbeTotalVram —— VBE BIOS(4F00h)声称的显存总量(字节); 0 = 未知。
 * 定义在 drv.c, r82 的帧缓冲自检用它决定"显存尾"采样点。
 * 【为何必须在这里声明 —— 一次真实的教训】display.c 里漏了原型时, 编译器
 * 按 C 的老规矩假定它返回 int, 于是只从 AX 取值: 16MB = 0x01000000, 高半字
 * 被丢掉 → 读到 0 → "模式帧缓冲装不下显存"的检查被静默跳过(MR_VRAM 永不
 * 触发)。编译器只给一条 W131("没有原型"), 既不报错也不拦。凡跨模块调用的
 * 函数, 必须在本文件有原型。                                        */
uint32_t __far VbeTotalVram(void);

/* VGA 标准 DAC 调色板(8bpp 模式用) */
void     __far DAC_SetPalette(uint16_t start, uint16_t count,
                        const uint8_t far *pal4);

/* VDD I/O 陷阱开关与文本模式恢复(Disable 路径用) */
void     __far VDD_StopIOTrap(void);
void     __far VDD_StartIOTrap(void);
void     __far BIOS_SetTextMode(void);

/*==========================================================================
 *  vddapi16.asm 导出的函数(主 VDD API 通道，"第 ③ 环")
 *
 *  全部是 __far __pascal：符号为纯大写，与汇编侧 public 名一致，
 *  MSVC 1.52 与 Watcom 两种编译器生成相同的符号名。
 *========================================================================*/
void     __far __pascal VddAcquire(void);
uint32_t __far __pascal VddCall(uint16_t func);
uint32_t __far __pascal VddRegister(uint16_t func, uint16_t pitchBytes,
                                    uint16_t height, void __far *cb);
uint32_t __far __pascal VddGetDispConf(uint16_t func, uint16_t bufSize,
                                       void __far *buf);

#endif /* DRV_H */
