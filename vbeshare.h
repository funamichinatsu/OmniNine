/*=============================================================================
 * vbeshare.h —— 16 位驱动(C) 与 mini-VDD(汇编) 之间的共享定义
 *
 * 【设计约束】
 *  1. 本文件的【字段顺序就是 ABI】。VbeState 由驱动侧按 C 结构填充、由汇编侧按
 *     vbeshare.inc 里的同名偏移读取。任何字段的插入、删除或改序，都必须两侧同时修改
 *     并同时重建，否则两个视图会错位 —— 而错位的症状是"读到合理的错值"，极难定位。
 *  2. 新增字段只能【追加在末尾】，不要插入中间；追加后要同步更新长度常量。
 *  3. Probe 块里有一部分槽位被汇编自身当状态机读回(cmp [Probe+...])，
 *     它们不是日志；判断"能不能删"必须同时看 asm 自读与 C 侧读者。
 *  4. 这里的每个策略字段都只表达"驱动愿意做什么"，不代表盒子当前的真实硬件状态。
 *
 * 【大体思路】
 *  驱动侧把配置与状态写进 VbeState 的固定偏移；VxD 侧只按偏移读，不认识 C 的结构名。
 *===========================================================================*/
/*============================================================================
 *
 *  VBESHARE.H — ring 3(显示驱动)与 ring 0(mini-VDD)之间的共享契约
 *
 *  【这个文件是干什么的】
 *  Win9x 的显示子系统由 16 位 display.drv(GDI 一侧) 和 ring 0 的
 *  VDD.VXD + mini-VDD(硬件一侧) 组成。本驱动是【通用 VBE 驱动】：
 *  不识别任何具体显卡型号，只使用 VBE 标准(INT 10h AX=4Fxx)能获得的
 *  信息，目标是让任何符合 VBE 规范的显卡都能稳定地实现 SVGA/真彩色
 *  2D 显示。
 *
 *  【r76 契约变更 —— 帧缓冲映射的责任方换边】
 *  旧版(r73-r75)让 mini-VDD 用 _MapPhysToLinear + _BuildDescriptorDWORDs
 *  + _Allocate_GDT_Selector 建一个 GDT 屏选择子交给 display.drv 直接用。
 *  实机实证: 该选择子装进 ES 的瞬间就是 0D(蓝屏 031F:00090B =
 *  _fmemset_ 里的 "mov es,dx")。根因两条, 都是架构性的:
 *    ① 那个描述符的 DPL 由 VMM 按"保护模式应用"的 RPL 位填
 *       (_BuildDescriptorDWORDs 的 flags 不给 BDDExplicitDPL 时如此),
 *       16 位显示驱动(ring 3)载入它时 max(CPL,RPL) > DPL → #GP;
 *    ② 即便选择子能载入, 它的基址是 VxD 系统区线性(>=0xC0000000),
 *       那些页是 supervisor 页, ring 3 一写就是 #PF。
 *  r76 起改为【display.drv 自己在 ring 3 完成映射】(这也是在售的
 *  boxv9x / vmdisp9x 两条生产路线的做法):
 *      DPMI 0800h(物理地址映射) → 得到本 VM 可访问的线性地址
 *      DPMI 0000h/0007h/0008h   → 自建 LDT 选择子指向它
 *  所得选择子由 DPMI 宿主按 ring 3 客户端生成(DPL=3), 必然可载入。
 *  mini-VDD 只负责把它登记给主 VDD(VDD_Register_Extra_Screen_Selector,
 *  DDK 文档明确要求 LDT/GDT 选择子由 mini-VDD 代显示驱动登记),
 *  不再碰任何 VMM 内存/描述符服务。
 *
 *  【为何结构必须 1 字节对齐】
 *  同一块结构会被 16 位编译器(MSVC 1.52 / Watcom wcc)、32 位编译器
 *  (测试程序) 以及 MASM(vbeshare.inc 中的镜像) 三方读写。三方默认
 *  对齐各不相同，一旦某侧按 4 字节对齐插入填充字节，另一侧读到的
 *  就是错位数据。pack(1) 让布局完全由字段顺序决定，可精确镜像到
 *  汇编。布局若有任何改动，必须同步修改 vbeshare.inc！
 *
 *  【16/32 位兼容】
 *  MSVC 1.52(Win9x DDK 的 16 位编译器) 没有 stdint.h，这里用宏保护
 *  自行定义定长类型；unsigned long 在 16 位与 32 位 Windows 下都是
 *  32 位，因此这套 typedef 在两种编译器下布局一致。
 *
 *==========================================================================*/

#ifndef VBESHARE_H
#define VBESHARE_H

/* --- 定长类型：优先使用系统 stdint.h，否则自行定义 -------------------- */
#if !defined(_STDINT_H_) && !defined(_STDINT_H) && !defined(_STDINT) && \
    !defined(_STDINT_H_INCLUDED) && !defined(VBESHARE_TYPES_DEFINED)
#define VBESHARE_TYPES_DEFINED
#if defined(__LP64__) || defined(_LP64)
/* LP64(64 位 Unix)下 long 是 64 位，改用 int 保证 32 位宽度；
 * 本驱动的目标平台(16/32 位 Windows)走下面的 long 分支。      */
typedef unsigned int   uint32_t;
#else
typedef unsigned long  uint32_t;   /* 16 位与 32 位 Windows 下均为 32 位 */
#endif
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
#endif

#pragma pack(push, 1)

/* --- 共享状态结构：总长 44 字节(r101 由 40 扩到 44: 新增 4 个策略字节)，
 *     与 vbeshare.inc 严格一致。两侧的偏移/长度由包内 abi_check.py 机械
 *     核对, 不靠人眼。(旧注释写"40 字节 / 10 个双字", 已过时。)      */
typedef struct _VBESHAREDSTATE {

    uint32_t signature;          /*  0 魔数 'VBES'，握手校验用。
                                    收到数据先验签名，可挡住错传进来的
                                    任意缓冲区 —— 见 VBESTATE_SIG      */
    uint16_t currentMode;        /*  4 当前 VBE 模式号(如 0x4111)      */
    uint16_t bytesPerScanLine;   /*  6 每扫描行字节数(VBE 4F01h 返回)  */
    uint16_t xRes;               /*  8 水平分辨率                      */
    uint16_t yRes;               /* 10 垂直分辨率                      */
    uint8_t  bitsPerPixel;       /* 12 每像素位数(8/16/24/32)          */
    uint8_t  bankSizeKB;         /* 13 bank 粒度，单位 KB(VBE 4F01h 的
                                    WinGranularity，通常 64)。主 VDD 用
                                    它把显存偏移换算成 4F05h 的 bank 号
                                    —— 粒度报错的直接后果就是花屏。
                                    A000 窗口最大 64KB，故 uint8 足够；
                                    为 0 或越界时由 VxD 修正为 64      */
    uint32_t physBasePtr;        /* 14 线性帧缓冲(LFB)物理地址，0=无。
                                    由 display.drv 从 VBE 4F01h 取来,
                                    ring 3 用它做 DPMI 0800h 物理映射 */
    uint8_t  hasLFB;             /* 18 非 0：存在线性帧缓冲            */
    uint8_t  reserved0;          /* 19 保留，置 0                      */
    uint32_t structSize;         /* 20 sizeof(VBESHAREDSTATE)，版本协商用：
                                    将来结构加长时，旧 VxD 可按 size 只读
                                    自己认识的前缀                      */
    uint32_t vramSize;           /* 24 显存总字节数(VBE 4F00h 的
                                    TotalMemory*64KB)。主 VDD 保存/恢复
                                    高分辨率屏幕时依赖它；为 0 表示未知，
                                    VxD 对 GET_TOTAL_VRAM_SIZE 返回 NC，
                                    让主 VDD 自己调 VESA BIOS 查询      */
    uint16_t vbeVersion;         /* 28 VBE BIOS 版本(BCD，如 0x0219)   */
    uint32_t lfbLinear;          /* 30 【drv 填写】DPMI 0800h 映射出来的
                                    LFB 线性地址(信息/诊断用; VxD 只把它
                                    抄进 VbeState 供回读)。             */
    uint16_t lfbSelector;        /* 34 【drv 填写】display.drv 用 DPMI
                                    0000h/0007h/0008h 自建的 LDT 屏选择子
                                    (DPL=3, ring 3 可载)。mini-VDD 在
                                    ApplyState 收到非 0 值时, 代它调用
                                    VDD_Register_Extra_Screen_Selector
                                    登记给主 VDD(DDK 规定的登记通道:
                                    主 VDD 据此在窗口化 VGA 4 平面虚拟化
                                    时切回 Windows VM 需要的 MemC 状态,
                                    不登记就可能出现 DOS 图形程序退出后
                                    桌面花屏)。0=未建。                 */
    /* 36 【r99】策略字节组 —— 复用原 4 字节保留区, 结构总长仍为 40,
     * 因此新旧两侧的尺寸校验/rep movsd 长度都不变。
     * 由 display.drv 从 system.ini [omninine] 读入后随 PushState 推给 VxD;
     * VxD 在分派处理函数里【现场读】这几个字节, 于是"改 INI + 重启"
     * 就能 A/B, 不必重编译。全部默认 0 = r98 的既有行为。            */
    uint8_t  policyVddEdx;       /* 36 VDD_DRIVER_REGISTER 的 EDX:
                                    0 = EDX 0(允许窗口化 4 平面虚拟化)
                                    1 = EDX -1(要求 VGA 4 平面图形全屏)
                                    drv 已把 VddVirtualize=0|1|2 折算成
                                    0/1(2 = 按当前位深折算: ≥16bpp 取 1) */
    uint8_t  policyVesa;         /* 37 VESA_SUPPORT(41) 策略:
                                    0 = 恒 NC(全部放行给真 VESA BIOS)
                                    1 = 非 Windows VM 发起的"改硬件"类
                                        4Fxx(4F02/03/05/06/07/09) 回
                                        CY+0160h; 只读查询(4F00/4F01)与
                                        Windows 自己的调用照旧放行     */
    uint8_t  policyHires;        /* 38 CHECK_HIRES_MODE(35):
                                    0 = 不挂钩(主 VDD 默认 NC = 标准 VGA)
                                    1 = 处于 VBE 模式(StateValid 且
                                        currentMode ≥ 100h) 时回 CY     */
    uint8_t  policyRestore;      /* 39 ResetHiResMode 策略(0/1/2):
                                    VxD 不读, 只为让 PUSH/GET 的整块
                                    状态里带着它, 日志一处可对齐      */
    /* 40 【r101】策略字节(结构 40 → 44 字节, 仍是 4 的倍数, 两侧的
     * rep movsd 长度自动跟随 VBE_STATE_SIZE/4) --------------------- */
    uint8_t  policyBank;         /* 40 GET_VDD_BANK(1) 策略:
                                    0 = 不挂钩(r98/r99; 主 VDD 自己的
                                        默认实现决定虚拟化区, 实测会得到
                                        "0 字节虚拟化区" ⇒ 窗口化 DOS 的
                                        4 平面访问落到可见屏 ⇒ 花屏)
                                    1 = 【r101 新默认】处于 VBE 高分辨率
                                        模式且尚未切到 VGA 时, 按 DDK 原文
                                        "If not allowing VGA 4 plane
                                        virtualization, set EDX = ECX"
                                        答 Client_EDX = 入口 Client_ECX
                                        + CY(拒绝窗口化 4 平面)。
                                        DDK 明说这样【仍允许文本与非平面
                                        图形(如 13h)在窗口里跑】        */
    uint8_t  policyInt10;        /* 41 PRE_INT_10_MODE_SET(48) 策略:
                                    0 = 【r101 新默认】只记录被请求的模式
                                        (不改变主 VDD 的行为)
                                    1 = Windows 屏幕在场(未切到 VGA)时
                                        吃掉这次 INT 10h 设模式(CY)。
                                        DDK 对该服务无文档页, 返回约定属
                                        推断, 故默认关闭 —— 下一轮的 A/B */
    uint8_t  policyVgaFix;       /* 42 【r114】全屏 VGA 交接后是否补写
                                    "文本取指/平面"寄存器组:
                                    0 = 【r114 新默认】一个字都不写(与
                                        r113 行为逐字节相同);
                                    1 = POST_HIRES_TO_VGA(5) 里写
                                        SR02/03/04、GCR05/06、CR14/17、
                                        AR10 这一组"取指与平面"寄存器
                                        (绝不碰任何时序/时钟寄存器)。
                                    依据: ./110 t3 的截图取证(见
                                    R114_CHANGES.md §0)——Alt+Enter 进全屏
                                    后, 文本模式的三种取指(字符/属性/字模)
                                    全部塌到plane 0 上: 字符正确, 而字模
                                    变成"字符码重复 16 行"的竖条纹(空格的
                                    0x20 就是每格第 2 列一条竖线),
                                    属性字节 = 字符码本身(空白格 0x20 ⇒
                                    背景调到调色板 2 = 整屏绿)。
                                    注意: 该字节原本是 policyResv0, 现在
                                     启用它【不改变结构大小(仍 44)】,
                                     所以新旧 minivdd.vxd 可以混搭部署。 */
    uint8_t  policyDispi;        /* 43 【r116】Bochs/VBox VBE_DISPI 端口
                                    (1CEh/1CFh)是否对非 Windows VM 关死:
                                    0 = 不装陷阱(与 r115 行为逐字节相同);
                                    1 = 【r116 默认】装 I/O handler 并在
                                        Windows 拥有 CRTC 期间打开全局陷阱:
                                        非 Windows VM 写 1CEh/1CFh 被吃掉,
                                        真硬件 I/O 只在"调用者就是 CRTC
                                        所有者"时放行。
                                    依据: boxv9x 的 mini-VDD(vxd/boxvmini.asm,
                                    Philip Kelley 2023)只做这一件事, 原文:
                                    "it virtualizes accesses to the Bochs VGA
                                    registers on I/O ports 1CEh and 1CFh …
                                    when you start an MS-DOS Prompt, the screen
                                    doesn't go wonky when the DOS VM, calling
                                    into the VGA ROM BIOS, ends up trying to
                                    perform I/O to those ports"; vmdisp9x 的
                                    virtual_0x1ce() 注释同义("fix of broken
                                    screen when open DOS window")。
                                    注意: 该字节原本是 policyResv1, 现在启用
                                    它【不改变结构大小(仍 44)】, 所以新旧
                                    minivdd.vxd 可以混搭部署 —— 旧 VxD 只是
                                    不读它(也就没有这一层保护)。          */

    uint8_t  policyInt10Fix;     /* 44 【r144】48 号(PRE_INT_10_MODE_SET)里
                                    是否把 Far-East 文本模式 72h/73h 改写成
                                    标准 03h(只在一个【全屏】DOS 盒持屏期间,
                                    即 mini-VDD 的 4→7 号武装期内):
                                    0 = 一个字都不改(与 r143 行为逐字节相同);
                                    1 = 【r144 默认】改写。
                                    依据(实证): ./143 的日志里盒子交替请求
                                    03h 与 72h, 而用户必须打 `mode co80`
                                    (= INT 10h AX=0003) 才能把全屏画面从花屏
                                    救回来 —— 也就是说 03h 这条路【在这台
                                    机器上是通的】, 72h 那条不通。我们只是
                                    把用户已经证明有效的那一手, 挪到官方
                                    钩子(48 号)上自动做一次。
                                    注意: 结构 44 → 48 字节, 两侧的
                                    VBE_STATE_SIZE 与 rep movsd 长度必须
 *                                    【r150 —— 值域扩成 0/1/2】
 *                                      0 = 【r150 默认】一个字节都不改;
 *                                      1 = 只在武装期(全屏盒子持屏)改;
 *                                      2 = 无条件改(精确复现 ./149, 只为
 *                                          A/B 对照 —— 已知会让桌面回不来,
 *                                          见 SYMGUARD 上方 r150 注第 2 条)。
                                    同时改(vbeshare.inc 已同步)。      */
    /* 【r146】policyHiresFix —— mini-VDD 侧的"进盒前存一份 / 回 Windows 后
     *   写回一份"的模式/寻址字节快照(见 VBE_TR_Hires)。
     *     0 = 一个字节都不写(与 r145 行为逐字节相同);
     *     1 = 【默认】存 + 写回。
     *   依据: DDK 把"超出标准 VGA 的那部分状态"划给 mini-VDD(主 VDD 只保存
     *   标准 VGA 寄存器); ./144 的三张主机截图量出的形态(2~4 像素周期竖条纹
     *   + 调色板颜色 + 画面只剩左半并横向拉伸 —— 即同一块 16bpp 线性显存被按
     *   4 平面/奇偶解释)与 ./145 的 `SNAP SR01=0003`(应为 0001)同时指向
     *   "寻址/平面那几位没人恢复"。
     *   本字节复用 r144 那三个填充字节里的第一个 ⇒ VBE_STATE_SIZE 仍是 48,
     *   推送长度、rep movsd 长度、两侧结构体大小都不变。               */
    uint8_t  policyHiresFix;     /* 45 【r146】mini-VDD hi-res 快照/回放 */
    uint8_t  policyMemCFix;      /* 46 【r147】盒子持屏期那六个"CPU 怎么看
                                    显存"寄存器(SR02/SR03/SR04/GCR05/GCR06
                                    /AR10)的补写开关:
                                      0 = 持屏期一个字节都不写(= r146);
                                      1 = 【默认】按"与文本目标逐字节比较"
                                          的结果决定写不写。
                                    仍然复用 r144 的填充字节(第二个)⇒
                                    VBE_STATE_SIZE 依旧是 48, 推送长度、
                                    rep movsd 长度、两侧结构体大小都不变。 */
    uint8_t  reserved2[1];       /* 47 补齐到 VBE_STATE_SIZE(必须是 4 的倍数)
                                    【为什么这 3 个字节不能省】r144 把
                                    VBE_STATE_SIZE 从 44 提到 48, 而结构只长到
                                    45: 于是 drv 侧 sizeof(g_state)=45 是推送
                                    长度, 而 VxD 侧 MiniVDD_ApplyState 的头一道
                                    闸门是 `cmp ecx, VBE_STATE_SIZE / jb as_fail`
                                    ⇒ 45 < 48, 【整包状态被静默拒绝】:
                                    StateValid 恒 0, 所有策略字节(VgaFix /
                                    Dispi / HiresAns / VddBank / Int10Mode /
                                    Int10Fix)在 VxD 里恒 0, 而 ring 3 的
                                    `run VgaFix=0001` 照旧打印 —— 日志看不出来。
                                    r143 之所以是好的: 那时 VBE_STATE_SIZE 与
                                    sizeof 都是 44, 两侧相等。(实测判据见
                                    R144_DELIVERY.md §1: 编译器 sizeof 探针 +
                                    ./143 的 `TRC st decode ...=000B`。)
                                    除这 3 字节填充外, 结构布局一个字节都没动。 */
} VBESHAREDSTATE;

#pragma pack(pop)

/* --- 签名魔数：字节序 V,B,E,S ------------------------------------------
 * 原注释: 'VBES'                                                       */
#define VBESTATE_SIG        0x53454256UL

/* --- 结构大小常量，供两侧校验 ------------------------------------------ */
#define VBE_STATE_SIZE      48      /* r101: 40 → 44; r144: 44 → 48
                                     *   (新增 policyInt10Fix, 必须是 4 的倍数:
                                     *    api_get 用 rep movsd 整块搬) */

/* --- VxD 设备 ID --------------------------------------------------------
 * INT 2F/AX=1684h 靠这个 ID 找到我们的 VxD，因此必须非零且不与系统内
 * 其它 VxD 冲突。正式商用驱动应向 Microsoft 申请分配；本项目沿用
 * 开源驱动的惯例，取已知设备列表中未被占用的 0x4342。                  */
#define VBE_VXD_DEVICE_ID   0x4342

/* --- VxD 版本(API 功能 0 返回) ----------------------------------------
 * r76: 删除 VxD 侧全部 MM/描述符服务(帧缓冲映射改由 ring 3 的 DPMI
 *      0800h 完成, 见文件头), VESA_SUPPORT 恒返回 NC(真 VESA BIOS
 *      放行, DOS/VESA 程序与 Windows 侧直通枚举都拿到真数据),
 *      ApplyState 新增"代 drv 登记屏选择子"。
 * 版本不等 = 部署错位(drv 跑的是旧 vxd), drv 直接拒用。
 * r99: mini-VDD 分派调用追踪(每个分派入口环形记录, drv 经 API 13 取回)
 *      + 三个策略字节(结构体偏移 36..39, 复用原保留区, 总长仍 40)
 *      + VDD_DRIVER_REGISTER 的 EDX 可选 -1
 *      + CHECK_HIRES_MODE/VESA_SUPPORT 可选取代主 VDD 默认作答。
 * r100: ① 修 r99 的三处缺陷(D1 drv 误用输出缓冲当请求块 / D2 VxD 记录区
 *      偏移差 4 字节 / D3 四个策略字节从未被赋值 ⇒ HiresAns、VesaPolicy
 *      是死开关); ② 追踪记录 16→24 字节, 新增 EBX(发起调用的 VM 句柄)与
 *      入口 EAX(分辨"谁在问/问什么"); ③ blob 头部的保留双字改成 4 个
 *      事件计数器(POST_VGA_TO_HIRES 等), 作为"切屏事务已收尾"的安全信号;
 *      ④ ResetHiResMode 回调改成"零 BIOS/零 DOS/可重入", 重建推迟到安全
 *      上下文; ⑤ CHECK_HIRES_MODE 只对 Windows VM 答 CY。
 * r101: 实测(./100/x230 + ./100/vm)给出的统一根因是【声明与实现不符】:
 *      mini-VDD 在 VDD_DRIVER_REGISTER 里传 EDX=0(声明"支持窗口化 VGA
 *      4 平面虚拟化"), 却没有实现让这个声明成立的那一族服务, 于是主 VDD
 *      用"banked/4 平面"的假设去保存、恢复、虚拟化一块【线性打包像素】
 *      的显存。改动:
 *      ① 结构 40→44: 新增 policyBank(GET_VDD_BANK 拒绝窗口化 4 平面,
 *         默认开)与 policyInt10(48 号改模式前钩子, 默认只记录);
 *      ② CHECK_HIRES_MODE 改为【按状态如实回答】(处于 VBE 模式且未被
 *         切到 VGA ⇒ CY), 不再按调用者身份猜测;
 *      ③ mini-VDD 内部新增切屏状态机(VGA/hires、陷阱开关、驱动停用),
 *         由 0/4/5/6/7/11/12/13/14/26 号服务维护, 经 blob 头 204..207
 *         回传给 drv 落盘;
 *      ④ 48 号服务把被请求的模式号(AX)与调用次数记进 blob, 供"DOS 应用
 *         到底请求了什么模式"取证。
 *
 * 【r103 变更】共享结构/服务号【仍然一个字都没动】(44 字节, 服务号照旧)。
 *   版本号的唯一作用是标记"两侧是同一轮构建", 以及让日志/排障时能对齐:
 *      · vddapi16.asm 的日志通道修复(r102 把自己锁死的那一处): 出口只在
 *        "登记过重入"的路径上递减计数 —— 被闸门拦下的那次没有 +1 却照样 -1,
 *        计数变成 0xFFFF 后, 第③道闸门 "≠0 即忙" 就永久成立, 日志从此
 *        只剩 PROBE 一行(./102 的 debug 构建实拍);
 *      · InDOS 闸门自愈: 连续 8 次读到"忙"即判定该标志不可用并永久停用
 *        第②道, 由 "LOG gate InDOS ok/val/busy" 三行落盘作证;
 *      · display.c: Offscreen 默认 1 → 0(见该处长注释: 主 VDD 自留 0 字节,
 *        DIBENG 离屏堆与它想放虚拟化区的位置完全重合);
 *      · 新增 DiagFbSample(帧缓冲原始字节取样)与 DiagCfg(一行配置自证)。
 *   minivdd.vxd 未改 ⇒ r102 的 vxd 与 r103 的 drv 混搭功能上仍然安全,
 *   但版本核对行会报不一致 —— 仍建议成对部署, 免得排障时误读。
 *
 * 【r121 变更】0x0113 → 0x0114。共享结构【仍然一个字都没改】(44 字节),
 *   涨版本的原因是 mini-VDD 的 PM API 多了一张 15 号(VBE_API_DISPI_PHYS,
 *   ring 0 物理读 VBE_DISPI 寄存器) —— 接口面变了就该涨版本。
 *   副作用正是我们要的: 测机若还跑旧 minivdd.vxd, 日志 "VxD version" 一行
 *   会明确报不一致(r120 实测过的坑: 旧 VxD 对 API 15 直接返回 0, 新 drv 只能
 *   看到 "DPHYS reads=0000", 分不清"VxD 是旧的"还是"端口不存在")。
 *   因此 r121 起【必须成对部署】: drv 与 vxd 同一轮一起编。
 *
 * 【r121 自证标记 —— 别删, symguard.py 与 buildvxd.bat 都读它】
 *   形态固定为: SYMGUARD_SRC r<N> vxd=<version>
 *   与 vbeshare.inc 顶部那一行必须逐字一致(两侧不一致 = 混放, 立即报错)。
 *   【r138 把它更新成真话】此前它一直写着 r129 / vxd=0x0114, 而 VBE_VXD_VERSION
 *   已经从 0x0114 涨到 0x0118 —— 一个"自证标记"停在九轮之前, 恰恰是这一类
 *   标记最不该有的状态(它会让人以为手里是 r129 的 ABI)。现在它与
 *   VBE_VXD_VERSION 同步, 并且 buildcheck/CHECK 的判据也一并改成 r138 的字面。 */
/*
 * ============================================================================
 * 【r151】./150 的判决, 以及本轮为什么把"官方路径"往前挪一格
 * ============================================================================
 * 1. **72h 改写这条线到此为止(路径删除)**。r150 新增的三个分类计数器给出
 *    了判决性读数: `PH: fbox n48 m72==0000001C`(28 次 72h/73h 请求)而
 *    `PH: fbox n48 arm72==00000000` —— **盒子持屏期间一次都没有**。于是:
 *      * 挂在"武装期"的闸门(Int10Fix=1)在本机【永远静默】;
 *      * 无条件档(Int10Fix=2)打的全部是【窗口化】会话的请求 ——
 *        那正是 ./149 把桌面弄丢的那一发。
 *    更硬的依据来自 DDK《CHECK_HIRES_MODE (Function 35)》原文:
 *      "Far East Windows changes the data passed in AL to compensate for
 *       Japanese text modes 72H and 73H. ... it passes these modes as modes
 *       02H and 03H instead of 72H and 73H."
 *    ⇒ 官方的对应关系是 **72h <-> 02h、73h <-> 03h**, 不是我们三轮以来假设
 *    的 "72h <-> 03h"; 改写连目标值都是错的。`Int10Fix` 仍解析、仍上报,
 *    但 r151 起被忽略。
 *
 * 2. **CHECK_HIRES_MODE(35) 的答案必须描述 EBX 里的那个 VM**。
 *    DDK 原文: "AL: Contains the Interrupt 10h video mode. EBX: Contains the
 *    VM handle to inquire about. ... CY returned means that the VM is in a
 *    hi-res mode." r133 把默认拨回 1(按【我们自己的】Windows 模式回答每个
 *    调用者)是为了压 f23 轮询风暴, 而 ./132 证明风暴没变小 —— 那次回退的
 *    收益是零, 代价却是"一个跑 80x25 文本的 DOS VM 被告知自己是高分辨率
 *    VM"。r151 把默认改回 4(DDK 答案); `HiresAns=1` 仍可一行回退。
 *
 * 3. **PRE/POST_VGA_TO_HIRES 的陷阱纪律漏了一半**。DDK《PRE_HIRES_TO_VGA
 *    (Function 4)》原文: "During PRE_HIRES_TO_VGA **or PRE_VGA_TO_HIRES**,
 *    remove trapping on your device's extended registers ... During
 *    POST_HIRES_TO_VGA **or POST_VGA_TO_HIRES**, reestablish the trapping."
 *    而 r150 的代码里只有 4 号关陷阱、5 号开陷阱; **6 号与 7 号什么都没做**
 *    —— 回 Windows 的那一整段(含 display.drv 的 ResetHiResMode)是在陷阱
 *    仍然开着的状态下跑的。r151 按原文补齐 6 号 TrapOff / 7 号 TrapOn。
 *
 * 4. **仪器不能放在闸门后面 —— 同一个坑第 9 次**。r150 的判读表要靠
 *    `TRC memcfix seenText/gfx/dirty` 读"盒子持屏期六个寄存器长什么样",
 *    而 `持屏期改写 SR02/SR04/GCR04/05/06 的旧写法` 第一条就是 `cmp VBS_policyMemCFix,0 / je out`;
 *    MemCFix 默认 0 ⇒ 那三个读数**在结构上恒为 0**。r151 把**探针**
 *    (`显存寄存器回读` + 文本/图形判定 + 六字节比较 + lastBefore)整体挪到策略
 *    闸门【之前】: 闸门只管"写", 不再管"看"。
 *
 * 5. **"谁在请求"必须带高位字**。r150 只记了 72h/73h 请求者 EBX 的低字, 而
 *    ./150 的日志里同时出现 **C2920074**(Windows VM)与 **D5520074**(DOS 盒子
 *    VM)两个句柄 —— **低字相同**。所以 r150 判读表里"i10 ebx 与 VMH edge
 *    低字相等 ⇒ 是 Windows 发的"那一行**作废**; 新块记完整 32 位句柄。
 *
 * 6. 仍然没做、但 DDK 明确要求的(下一轮): `VDD_Get_Special_VM_IDs`
 *    (EDI = Blue Screen Message Mode 伪 VM, ESI = Forced Planar Mode 伪 VM;
 *    只能在 Init_Complete 之前调用)。本轮**故意不调用**: 本机没有 ml.exe,
 *    若用户的 VDD.INC 没有声明这个服务名, 整包会编不过 —— 那比少一个读数
 *    贵得多。本轮用"Windows / 盒子 / 其它"三分类替代, 已足够回答"切屏那一刻
 *    屏幕归谁"。
 *
 * 7. **VBE_TR_Own(新块, 24 字节)**: 屏幕所有权 + 特殊 VM + 35 号的 AL。
 *    DDK《CHECK_HIRES_MODE》里还有一句判据级原文: 主 VDD 判断一个 VM 是不是
 *    高分辨率时 "checks first for a VESA mode, then for an Interrupt 10h BIOS
 *    mode above 13h, and then queries the mini-VDD"; 而 72h > 13h —— 如果
 *    DBCS 折算没有发生, 一个跑文本的盒子会被主 VDD 当成高分辨率 VM。
 *    `+14 P35AL` / `+15 P35MASK` 就是这条的物证: 见到 02h/03h = 折算发生了;
 *    见到 72h/73h = **没有**。
 */
/* SYMGUARD_SRC r412 vxd=0x01CA */
/* 【r150 —— ./149 的判决: 上一轮那处"放开闸门"就是本轮全部故障的根】
 *   1. 把 Int10Fix 的改写从"武装期(4→7)"放开成"无条件"之后, 第一个被改写的
 *      请求是【窗口化会话/控制台】的: 盒子的 0072h 被换成 0003h, 主 VDD 照办,
 *      于是它自己的模式记账从 0072h 变成 0003h。三条独立读数同时变脸:
 *        · TRC st i10 ax  0072 -> 0003 (该计数器在改写【之前】取 Client_EAX,
 *          所以 0003 是盒子【真的】在请求 03h —— 改写是粘的);
 *        · Verify hw mode (4F03h)  10/10 = 0072 -> 9/10 = 0003;
 *        · Verify hw pitch (4F06h) 0800/0400(我们的模式) -> 00A0/0280
 *          (00A0h = 160 字节 = 80 列 × 2 字节/字符, 0280h = 640 像素
 *           = 80 列 × 8 点 —— 教科书式的 80×25 文本几何)。
 *   2. 后果比"模式记账难看"严重得多: 此后【每一次】桌面重建的 VBE 4F02 都被
 *      拒(exec ax=014F, 而 r148 同样的调用 8/8 次是 004F), 直到日志末尾
 *      (第 54717 行)仍然是 014F —— 桌面【再也回不来】。用户看到的就是
 *      ./149 的 2.png: 全黑 + 左上角一个光标 = 真的被切进了 80×25 文本模式。
 *   3. 所以 r150 把闸门放回去(1 = 只在武装期改, 那正是用户打 mode co80 的
 *      现场), 并把 r149 的行为保留成显式的 A/B 档(Int10Fix=2 无条件改),
 *      默认【关闭】: Int10Fix=0。
 *   4. 【为什么上一轮会判断错】当时只有两个笼统计数器(n48 总数 / i10map 改写
 *      次数), 看不出"48 号到底是谁在问、问的是几号、当时武装没有"。本轮把
 *      VBE_TR_Hires 的四个空字节变成分类计数器(N48M72 / N48ARM72 / I10EBX)
 *      —— 仪器不够细, 结论就只能是猜; 这是同一个坑的第 8 次。
 *   5. 【支线】./149 的 stop/ 与 stop2/ 都是"换驱动后第一次启动、拉 explorer
 *      时"的崩溃, 而两次 i10map 全篇 0000 ⇒ 与上面这条改写【无关】; 两次都
 *      停在 DIB_DibToDevice 的 DDI 返回之后(MARK HB 心跳到此为止), 且
 *      stop2 已经成功跑过 5 次同样的 4F06h 探针 —— 间歇性, 详见
 *      R150_DELIVERY.md 的支线一节。 */
/* 【r148 —— 版本号为什么又 +1, 而语义一个字没改】
 *   r147 那个包在你机器上编不过:
 *       VBESHARE.INC(43) : error A2041: string or text literal too long
 *       minivdd.asm(3117) : error A2006: undefined symbol : VBE_VXD_VERSION
 *   根因是 vbeshare.inc 第 43 行(EQU 那行)的【行尾注释】被四轮历史撑到 483 字节
 *   (注释本身 451 字节)。ML 6.11c 对行尾注释(它当 text literal 存)的上限是 255
 *   字节, 超限时【整行丢弃】—— EQU 没执行, 符号不存在, 报错因此指向另一个文件里
 *   一个毫不相干的符号名。
 *   本版(r148)与 r147 的差别只有那一行注释: minivdd.asm 的行为、日志里的判读行、
 *   以及 R147_DELIVERY.md 的判读表全部原样适用。版本号照涨, 只是为了让"日志里哪
 *   一行代表哪个包"没有歧义 —— r147 的包永远不会产生日志。
 *   【硬规矩】vbeshare.inc 里任何【语句行】的行尾注释都要短; 要写历史就另起注释
 *   行。机械防线: buildvxd.bat(ml 之前 findstr 量字节, >=250 即停) +
 *   buildcheck.ps1(逐语句行/行尾注释分别量字节)。 */
/* 【r149 —— ./148 的三条硬结论, 决定了本轮的三处改动】
 *   1. r147/r148 的六字节"MemC 补写"在真机上【开火了, 而且把画面弄得更糟】:
 *      ./148 的 TRC memcfix = seenText 111 / fixedN 52 / dirty 52 / stillBad 0
 *      —— 写进去了、读回来也对, 可是症状从"花屏(可被 co80 救回)"变成
 *      "灰屏 + co80 失效"。依据 DDK《Virtual Display Minidriver》原文:
 *      窗口里的 DOS 会话 CRTC owner 是 Windows 而 MemC owner 是那个 DOS VM,
 *      "the screen looks like it is running in Windows hi-res packed pixel mode
 *      ... but the CPU sees the video memory as a 4 plane VGA" —— 也就是说
 *      ./146/./148 反复量到的那一组 (SR02=0F SR04=06 GCR05=00 GCR06=04)
 *      【就是 DDK 让主 VDD 摆出来的合法状态】(4 平面/强制平面), 不是"我们
 *      高彩的残留"。我们每 4 拍把它改写成传统 mode 3 的值, 是在拆主 VDD
 *      自己搭好的台 ⇒ 默认关掉(MemCFix=0), 代码与开关都留着做 A/B。
 *   2. 同一页还写了 VDD 有两个【伪 VM】: Blue Screen Message Mode(Far East
 *      DBCS 版的蓝屏是 Mode 12h)与 Forced Planar Mode("a VM containing the
 *      4 plane MemC state (such as that in Mode 12h)"), 并明确要求 mini-VDD
 *      在 Dynamic_Init 取到这两个句柄、在 RESTORE_REGISTERS(9) 里比对。
 *      ./148 里 f=08 SAVE_REGISTERS 38 次 / f=09 RESTORE_REGISTERS 51 次,
 *      我们两个都还是空壳 ⇒ 官方路径上真正缺的那一块在这里。
 *   3. CR0C(显示起始地址低位)从来没有被读过 —— 若它不为 0, 取指基址就不是
 *      B8000, 那才是"盒子的文本写在一处、扫描从另一处读"的判据。r149 把它
 *      加进持屏期指纹(用 FS_LEN=16 里最后一个空槽, 结构布局一个字节不动)。
 *   【本轮的开关变化】MemCFix 默认 1 → 0; Int10Fix 的改写不再受"已武装"限制
 *   (见 minivdd.asm 的 48 号钩子的实现体: ./148 里 Int10Fix=1 而 i10map 全篇 0 ——
 *    改写的闸门挂在 4→7 武装期内, 而真正决定全屏文本模式的那些设模式请求
 *    发生在【武装之前】, 所以三轮里一次都没轮到它)。 */
/* 【r161 这一轮为什么动版本号】行为改了: "MemC 那半边"的补写从
 *   "35 号上的节拍 + 每场 8 次预算 + 连续两拍去抖"挪到
 *   【9 号 RESTORE_REGISTERS 之后的官方恢复事件】上, 并且撤掉预算与去抖
 *   (判据是六字节逐个比较, 它自己会停下来)。
 *   依据: DDK 的 CRTC owner / MemC owner 分工(全文见 VBE_TR_PR_S9N 段)
 *   + ./160 的现场读数 —— 同一场全屏会话的第一拍(f35)是教科书文本值、
 *   最后一拍(l35)已经回到 4 平面那一组(SR02=0F SR04=06 GCR05=00 GCR06=04),
 *   而整场只补到 2..3 次, 于是画面一直是 1602.png 那种"三种取指塌到一个
 *   平面"的花屏。blob 布局仍然一个字节没动(四个新字段落在 r153 就留出的
 *   208..247 留白里, VBE_TR_PR_LEN 仍 248)。 */
#define VBE_VXD_VERSION     0x01CA   /* r162: 官方那一拍(4/5 号)补上标准 mode 3
                                      *   集合里被漏掉的 SR01(字符时钟)= 00h,
                                      *   并把"几何哪几个字节不是标准值"变成
                                      *   GEOMASK 读数(见 VBE_TR_PR_SR01VAL 段)。
                                      * r160: 取指基址修复变成 fail-closed 且自
                                      *   收回(见 VBE_TR_PR_NETC 段)。
                                      * r155 的行为改动: 默认档那一笔从"整机一次"变成
                                      *   "每场一笔预算 + 当场算映射覆盖 + 第二笔起要求连续两拍"。
                                      * r156 只改取证端: 单个 DAC 项读取 原本把条目号写进 3C8h
                                      *   (DAC 写索引)再读 3C9h, 而 3C9h 的读走 3C7h 设定的
                                      *   读索引; ./154 的 dac7=2A2A00 正是标准表条目 6 的值,
                                      *   即"错位一条"的指纹。blob 布局仍然一个字节没动。
                                      * (原始注释留档) r155: 版本号动了, 但【blob 布局一个字节都没动】
                                      * (新增的 7 个 Probe 字段全部落在 r153 就留出的
                                      * 208..247 留白里, VBE_TR_PR_LEN 仍是 248,
                                      * FBOX 键长与 api_gettrace 的 62 个双字不变)。
                                      * 动版本号的理由只有一条: 行为变了 —— 默认档那
                                      * 一笔从"整机一次"变成"每场一笔预算(8) + 当场
                                      * 算映射覆盖 + 第二笔起要求连续两拍都坏"。
                                      * 依据见 vbeshare.h 的 VBE_TR_PR_TGTRY 段与
                                      * R155_DELIVERY.md: ./154 的日志里 fixedN 恒为 1
                                      * 而 dirty 涨到 34 —— 那正是"花屏和以前一样"。
                                      * r154: see the note above the marker. */   /* r148: number moved for the reason above.
                                      * r147: the in-box repair can finally fire.  ./146 measured the full-screen DOS box's hardware WHILE THE BOX OWNED THE SCREEN (blob VBE_TR_FboxSnap, phase byte = 35 = a live hook-35 sample taken between PRE_HIRES_TO_VGA and POST_VGA_TO_HIRES) and the reading is a half-programmed mode: the CRTC is a correct 720x400 text mode (CR00=5F CR01=4F CR13=28 = offset 40 words = 80 columns, CR14=1F, CR17=A3) while the six registers that decide HOW THE FETCH AND THE CPU SEE VIDEO MEMORY are still our hi-res packed-pixel values -- SR02=0F (text wants 03), SR03=00 (already right), SR04=06 (odd/even host addressing DISABLED; text wants 02), GCR05=00 (host odd/even off; text wants 10), GCR06=04 (memory map A0000 64K; text wants 0E = B8000 32K + chain O/E), AR10 unmeasured.  The box's text therefore never lands where the display fetch reads, which is exactly why `mode co80` (a full BIOS mode set) cures it and nothing else does.  This is the DDK's CRTC-owner/MemC-owner split read literally: the Main VDD restores the states that preserve the appearance (it did, CRTC included) and the mini-VDD owns the states that control how the CPU sees the video memory (we did not).  r143-r146 HAD a routine that writes exactly those bytes, but it was gated by a ONE-BIT proxy -- "if GCR06 bit0 == 0 then the fetch is already text, do nothing" -- and the measured state has GCR06 bit0 == 0 with five other bytes wrong, so the repair never executed once in four rounds; the 64-shot counter that also gated it was a second, independent way to go permanently silent.  r147 replaces the proxy with a full six-byte comparison against the text targets, writes only when a byte really differs, reads back and counts what is still wrong, and deletes the budget (the criterion is self-terminating now); the repair never touches a CRTC/timing register, and it refuses to act unless GCR06 bit0 says alphanumeric mode, so a full-screen VGA graphics mode is never sprayed with text values.  Instrument: AR10 joins the fingerprint (14 registers) and each call site gets its OWN 16-byte slot (blob VBE_TR_PhaseSnap, 6 slots: phases 4/5/7/48/35 plus "the first in-box sample of this session") because ./146 proved the last writer wins and phases 4/5/7 were invisible.  New VBE_TR_MemC block carries seenText/fixedN/stillBad/gfx/dirtySeen plus the six "before" bytes.  Blob header 256 -> 368.  r146: the mini-VDD now keeps its own extended state across the hires<->VGA switch (SEQ 01..04 / GCR 03..06 / CRTC 14,17 are snapshotted in PRE_HIRES_TO_VGA and written back in POST_VGA_TO_HIRES -- the DDK divides exactly that half of the state to the mini-VDD), and the in-box hardware fingerprint grew from 4 to 13 mode/addressing registers plus a phase byte, sampled at hooks 4/5/7/48 and live in-box (first call + every 16th) instead of "every 256th call" (which never fired: ./145 had n35=0x66).  Blob header 236 -> 256, VBE_TR_FboxSnap 4 -> 16 bytes, new VBE_TR_Hires block, new policyHiresFix byte in r144's first pad byte (VBE_STATE_SIZE stays 48).  r145: two defects of r144 fixed + the in-box fingerprint now also samples while the box owns the screen.  r145: (a) VxdBoxArmedNow() returned the raw 0/1/2 byte and display.c tested it with `if (...)`, so "released by hook 7" (2) was read as "box owns the screen" -- ./144 line 16443 shows the entry edge taking "repaint only" while PH: fbox arm==0002, which is why a windowed command stayed pink-orange and a full-screen->windowed return stayed cyan; it now returns 1 only for the byte value 1.  (b) the session-end / entry / V2H edge flags were capped at 8 per boot and then never set again (./144 line 40502 "cap 8 reached, further edges ignored", followed by 16700 lines with zero SetDisplayMode) -- they are burst budgets now, throttled independently per side and cleared by VxdEdgeBudgetReset() after any successful rebuild.  r143: minivdd.asm changed -- the DDK's step-4 "set the screen out of hi-res mode" is finally done in PRE_HIRES_TO_VGA, the same repair is repeated (bounded, self-terminating) while the box owns the screen, and the two SR04/GCR06 samples (S1 before the chain, S2 during the box's ownership) are carried back in blob 212..215.  r142 was display.c only -- its "wait for the switch to finish" arm never executed (wArmedEntry is 0 on the EntryAlways path), so ./142 is behaviourally an r141 run.  r138: VBEEXECREQ grew the full BIOS output register set (out_bx..out_es, 48 -> 60 bytes) -- the round that finally lets ring 3 read the hardware's CURRENT mode (4F03h) instead of trusting our own mirror.  r137 was one asm instruction (AR10h 0041h), r136 AR10h sampling, r133 the ATC flip-flop discipline. */

/* --- Win32 DeviceIoControl 控制码(W32_DEVICEIOCONTROL 路径) ------------
 * 供 32 位程序(如配置/测试工具)用 CreateFile("\\\\.\\MINIVDD.VXD")
 * 打开后调用。注意：16 位 display.drv 不能走这条路，应走下面的
 * INT 2F/1684h + PM API 路径。功能 1 与原草案 drv.c 保持一致。        */
#define VBE_VXD_IOCTL_PUSH_STATE   1   /* 入参: VBESHAREDSTATE           */
#define VBE_VXD_IOCTL_GET_STATE    2   /* 出参: VBESHAREDSTATE           */

/* --- V86/PM API 功能号(INT 2F/AX=1684h 取得入口后远调用) ---------------
 * 调用约定(刻意只用 16 位寄存器，原因见下)：
 *   入口: AX = 功能号
 *         DX:CX = 缓冲区在当前 VM 中的 32 位【线性】地址(DX 为高字)
 *         BX    = 缓冲区字节数
 *   出口: CF=1 表示出错；AX = 状态码/版本号
 *
 * 【为何传线性地址而不是 ES:DI】
 *   V86 客户端的 ES:DI 是 段:偏移，PM16 客户端的 ES:DI 是 选择子:偏移，
 *   VxD 若收段地址就必须分两套代码换算。让客户端自己换算成线性地址
 *   (V86: seg*16+off；PM16: DPMI int 31h AX=0006h 查选择子基址)，
 *   VxD 侧一份代码同时服务两种客户端，少一条容易出错的换算路径。
 * 【为何只用 16 位寄存器】
 *   MSVC 1.52(Win9x DDK 标配 16 位编译器)的内联汇编不支持 386 的
 *   32 位寄存器；把 32 位线性地址拆成 DX:CX 两个 16 位寄存器传递，
 *   drv.c 就能用纯 8086 指令完成调用，MSVC 1.52 与 Watcom 均可编译。 */
#define VBE_API_GET_VERSION 0           /* 返回 AX = VBE_VXD_VERSION     */
#define VBE_API_PUSH_STATE  1           /* 客户端 → VxD 推送共享状态     */
#define VBE_API_GET_STATE   2           /* VxD → 客户端 取回共享状态     */
#define VBE_API_GET_DBG     3           /* 返回 AX = VESA_SUPPORT 调用计数 */
#define VBE_API_EXEC_BIOS   4           /* VxD 嵌套 V86 直通 VBE BIOS    */
#define VBE_API_EXEC_NOOP   6           /* 直通诊断 1 相: 单字节空桩(CF=
                                           纯 iret)走完整嵌套执行, 不调
                                           BIOS (./34: 原 pushf;iret 双字节
                                           桩栈失衡致死, 已废)            */
#define VBE_API_IVT_PROBE   8           /* 取证: 经 Get_V86_Int_Vector 读
                                           真 V86 IVT[10h] 与 [借用向量] 并
                                           回填(假成功层身份判定用)      */
#define VBE_API_VM_HANDLE_PROBE 9       /* 探针: VMMCall Get_Cur_VM_Handle,
                                           int 20h 机制基线              */
#define VBE_API_NEST_BASE   10          /* 探针: 纯嵌套执行机制基线 ——
                                           Begin_Nest + Exec_Int(21h,
                                           AH=30h 取 DOS 版本, 无害) +
                                           End_Nest, 【完全不碰 IVT】。   */
#define VBE_API_GET_MAPINFO 11          /* 回报 V86 中转缓冲/状态位图;
                                           回报前 VxD 惰性建立 V86 中转
                                           缓冲(EnsureV86, 故本 API 兼作
                                           分配重试)。除 EnsureV86(嵌套
                                           执行, 与 api_exec 同机制)外
                                           只读锁定段全局量, PM API 上
                                           下文安全                      */
#define VBE_API_GET_MODELIST 12         /* VxD 侧直读 V86 模式表: 从 V86 缓冲
                                           的 VbeInfo 读 VideoModePtr(far),
                                           VxD 线性遍历模式号表(BIOS 常无视
                                           预置指针, ./51 实锤), 整批拷进
                                           客户端 bufLin(2B/项, ≤bufLen/2
                                           且≤128), out_ax=项数。调用前提:
                                           缓冲就绪(EnsureV86 内部再保险)
                                           且 4F00 已把 VbeInfo 留在 V86
                                           缓冲                          */
#define VBE_API_GET_TRACE   13          /* VxD → 客户端 取回 mini-VDD 分派
                                           调用追踪 blob(见下方布局)。
                                           入参用请求块的 bufLin/bufLen,
                                           out_ax = 实际拷贝字节数        */
#define VBE_API_SET_INCB    14          /* 【r111】置/清"显示驱动正在主 VDD
                                           回调里"的标记(EBX = 0/1)。VxD 据此
                                           在 api_exec 里加一道 VM 自检: 当前
                                           VM != Windows VM 时回 93 并拒绝
                                           嵌套 V86 执行(回调可能切在别的 VM
                                           上下文里 —— 见 drv.h 的 VxdAllowBios) */
#define VBE_API_DISPI_PHYS  15          /* 【r120】ring 0 物理读 VBE_DISPI
                                           (1CEh/1CFh)的六个寄存器。用
                                           VDD_Do_Physical_IO 绕开 VM 的
                                           端口虚拟化, 回答"ring 3 读到的
                                           FFFF 是端口不存在, 还是我们的
                                          那次 I/O 没抵达硬件"。
                                           出口 out_ax = 物理读次数(模 256);
                                           六个寄存器的值随下一次
                                           GET_TRACE 的 blob offset 4..15
                                           带出(见下方 VBE_TRACE_HDR 注)。
                                           这是唯一一条能区分那两种可能的
                                           通路, 故按 INI DispiPhys 开关
                                           (默认 0 = 关)由用户显式打开。 */

/* --- r99: mini-VDD 分派调用追踪 blob 布局 ------------------------------
 * 【为什么必须有它】vbe9xlog 里每一行都是 ring 3 写的, 主 VDD 对我们的
 * 49 个分派服务的调用在日志里一个都看不见 —— 于是"窗口化 DOS 花屏到底
 * 是谁把硬件改了"只能靠推断。现在每个分派入口先记一条记录(r99 的桩
 * 与"未挂钩"行为等价: 记完原样跳到主 VDD 的原处理函数), drv 在任意
 * ring-3 入口整块取回落盘。
 * 与 vbeshare.inc 的 VBE_TR_xxx / VBE_TREC_xxx 严格一致。               */
#define VBE_TRACE_FUNCS     49          /* 分派功能号个数(计数桶数)      */
#define VBE_TRACE_RECS      64          /* 环形记录条数                  */
#define VBE_TRACE_RECB      24          /* 每条记录字节数(r100: 16→24)   */
/* 【头部长度 368 的来历】逐字段累加:
 *     0 WriteIdx(4) + 4 Evt(4) + 8 Counts(49*4=196) + 204 State(1)
 *     + 205 I10Cnt(1) + 206 I10Ax(2) + 208 Stat2(4) = 212
 *     212..215 Fbox(4)
 *     + 216 DispiPhys(16) = 232
 *     + 232 FboxSnap(16)  = 248
 *     + 248 Hires(8)      = 256
 *     + 256 PhaseSnap(96) = 352   【r147】
 *     + 352 MemC(16)      = 368   【r147】
 *     + 368 Own(24)       = 392   【r151】
 *     + 392 Probe(32)     = 424   【r152】
 *     + 392 Probe(248)    = 640   【r153】(Probe 扩容, 见其长注)
 *   即 640。r144 之前这里是 236: 232..235 与 DispiPhys 块尾部那四个
 *   "保留 0"字节重合, 于是不用动头部长度也能多放 4 字节。r146 要放的
 *   读数更多(13 个模式/寻址寄存器 + 相位 + hi-res 快照记账), 4 字节不够,
 *   头部正式长到 256; r147 又加了两块(每个采集点独立的 16 字节槽 + 补写
 *   记账), 头部再长到 368, 记录区随之从 256 挪到 368; r151 再加一块
 *   (Own, 24 字节: 屏幕所有权 + 特殊 VM + 35 号的 AL), 头部 368 -> 392,
 *   记录区随之 368 -> 392; r152 再加一块(Probe, 32 字节: 取指基址探针 +
 *   修复记账), 头部 392 -> 424; r153 把 Probe 从 32 扩到 248(四处 32 字节
 *   原文 + 属性调色板 + DAC + 扫描结果), 头部 424 -> 640, 记录区 424 -> 640。
 *   ⚠ 这是一次 ABI 变更: vbeshare.inc 的两侧常量、api_gettrace 的搬运用
 *   偏移、ring 3 的读取偏移必须同步; VBE_VXD_VERSION 已随之 +1,
 *   混放(旧 vxd + 新 drv)会被 drv 的版本核对当场拒绝。               */
#define VBE_TRACE_HDR       648         /* 头部长度(见上方逐字段推导)    */
#define VBE_TRACE_SIZE      (VBE_TRACE_HDR + VBE_TRACE_RECS * VBE_TRACE_RECB)

/* blob 内偏移 */
#define VBE_TR_WriteIdx     0           /* dd 累计写入条数(= 下一条序号)  */
#define VBE_TR_Evt          4           /* 【r100】4 个 8 位事件计数器,
                                           VxD 写 / drv 读(只比较变化,
                                           不要求清零 —— 与既有"26 计数
                                           比较"同一套路)。见 VBE_TR_EVT_* */
#define VBE_TR_Counts       8           /* 49 × dd 每功能累计调用次数     */
#define VBE_TR_State        204         /* 【r101】db 切屏状态机(见 VBE_TR_ST_*):
                                           0/4/5/6/7/11/12/13/14/26 号服务
                                           维护。drv 落盘 ⇒ "主 VDD 到底把
                                           硬件当成 VGA 还是 hires"可见 */
#define VBE_TR_I10Cnt       205         /* 【r101】db PRE_INT_10_MODE_SET(48)
                                           被调用次数(模 256)              */
#define VBE_TR_I10Ax        206         /* 【r101】dw 48 号最近一次被请求的
                                           模式号(入口 Client_EAX 低字)。
                                           DOS 应用设模式的第一现场证据   */
#define VBE_TR_Stat2        208         /* 【r117】db×4 VxD→drv 的 DISPI
                                           记账区(见 VBE_TR_ST2_*)。为它
                                           把记录区从 208 挪到 212: 头部的
                                           204..207 已被状态块占满, 而这一
                                           组读数必须能被 ring 3 直接读到,
                                           不能再靠"状态位反推"。         */
#define VBE_TR_ST2_INST     0           /* 0..2 = 1CEh/1CFh 各自装成的
                                           位图(bit0=1CE, bit1=1CF)。
                                           r116 的 0 值有两种可能(策略 0 /
                                           安装失败), 这一字节把它们分开 */
#define VBE_TR_ST2_TRAP     1           /* DispiTrapOn 的当前值(0/1)     */
#define VBE_TR_ST2_ACT      2           /* MiniVDD_DispiTrapOn/Off 真正
                                           执行过 Enable/Disable_Global_
                                           Trapping 的次数(模 256)       */
#define VBE_TR_ST2_ERR      3           /* Install_IO_Handler 失败次数
                                           (模 256) —— 非 0 就必须解释   */
#define VBE_TR_Fbox         212         /* 【r144】db×4 ring 0 盒子持屏期取证块。
                                           【取代 r143 的 VgaS 块 —— 那四个字节
                                           从来没被写过(见 R144_DELIVERY
                                           §2), 所以本轮取证改成"计数器 +
                                           一拍硬件指纹", 而且读数一律【无条件】
                                           落盘 —— 静默的仪器等于没有仪器。】
                                           四个字节必须连续: api_gettrace 用
                                           一个 mov eax/dword 整块搬走。   */
#define VBE_TR_FBOX_I10MAP  0           /* 【r171】4 号(PRE_HIRES_TO_VGA)那一
                                           枪的结果位图。r169/r170 这一格是
                                           "48 号 72h/73h->03h 改写计数", 那一
                                           条路 ./169 判死(i10map 恒 0)并在
                                           r170 撤掉, 于是这一格空了一轮。
                                           现在装的是 VgaPreTextMode 的结论:
                                             bit0..2 = 状态(0 没跑 / 1 执行且
                                               BIOS 回 AH=0 / 2 IVT[10h]=0 /
                                               3 执行但 AH!=0 / 4 盒子不是
                                               80 列文本, 不动手)
                                             bit3 = 盒子是 72h, bit4 = 73h,
                                             bit5 = 02h/03h/07h,
                                             bit6 = 当前 VM == 盒子 VM
                                           为什么这一枪挪到 4 号: DDK 的六步
                                           里第 5 步存 BDA、第 6 步设模式、
                                           第 7 步把 BDA 写回去 —— 5 号打在
                                           第 5 步【之后】, 硬件是 mode 3 而
                                           盒子的 BDA 又被写回 72h, 两边不
                                           一致就是 ./170 的花屏。          */
#define VBE_TR_FBOX_N35     1           /* +1 武装(4 号→7 号)期间 35 号被
                                           调用的次数(模 256)。0 = 全屏期间
                                           主 VDD 根本不问 35 号 ⇒ 挂在 35
                                           号上的持屏期补写是死代码。     */
#define VBE_TR_FBOX_N48     2           /* +2 武装期间 48 号(设模式前钩子)
                                           被调用的次数(模 256)。它是【已
                                           证实】会在全屏期间发生的钩子
                                           (i10 n 在 ./143 日志里 +6)。      */
#define VBE_TR_FBOX_DISARM  3           /* +3 0=本次开机没武装过
                                           1=武装中(盒子正持屏)
                                           2=已被 7 号(回 Windows)解除    */
#define VBE_TR_FboxSnap     232         /* 【r144】db×16 硬件指纹(盒子持屏期
                                           与切屏两拍的现场读数)。
                                           【r144 只读 4 个, r146 扩到 13 个】
                                           原四个(SR04/GCR06/CR13/CR17)只能
                                           回答"硬件在谁的模式里"; 而 ./144
                                           的用户截图量出了一个更细的形态:
                                           画面是"2~4 像素周期竖条纹 + 调色板
                                           颜色"(1.png 的粉橙、3.png 的青底
                                           梳状) —— 那是【同一块 16bpp 线性
                                           显存被按 4 平面/奇偶(odd-even)
                                           解释】的样子。决定这件事的位不在
                                           原来那四个里: 它们在
                                             SR02/SR03/SR04(时序器内存模式)
                                             GCR03/GCR04/GCR05(寻址/奇偶)
                                           ⇒ 本轮把这一组补齐, 并把 CR00/CR01
                                           (行宽)一起带上, 于是一次读数就能
                                           同时回答"几何对不对"与"寻址对不对"。
                                           【相位字节 +13】这一拍是谁采的
                                           (4/5/7/8/35 号), 见 minivdd.asm 的
                                           FboxSnap 头注 —— 同一个寄存器组在
                                           "进盒前/进盒后/回 Windows 后/持屏期"
                                           四拍上的值就是整条因果链。       */
#define VBE_TR_FS_SR04      0           /* 块内偏移: 时序器 SR04(chain4/奇偶) */
#define VBE_TR_FS_GCR06     1           /*           GCR06(显存映射+链位)   */
#define VBE_TR_FS_CR13      2           /*           CRTC CR13(每行偏移)   */
#define VBE_TR_FS_CR17      3           /*           CRTC CR17(SE/字宽)    */
#define VBE_TR_FS_SR01      4           /* 【r146】时序器 SR01(8 点/带宽)  */
#define VBE_TR_FS_SR02      5           /* 【r146】时序器 SR02(写平面屏蔽) */
#define VBE_TR_FS_SR03      6           /* 【r146】时序器 SR03(字符映射)   */
#define VBE_TR_FS_GCR03     7           /* 【r146】GCR03(数据旋转/功能)    */
#define VBE_TR_FS_GCR04     8           /* 【r146】GCR04(读平面选择)       */
#define VBE_TR_FS_GCR05     9           /* 【r146】GCR05(奇偶/256 色位移)  */
#define VBE_TR_FS_CR14      10          /* 【r146】CRTC CR14(下划线/双字)  */
#define VBE_TR_FS_CR00      11          /* 【r146】CRTC CR00(水平总数)     */
#define VBE_TR_FS_CR01      12          /* 【r146】CRTC CR01(水平显示末)   */
#define VBE_TR_FS_SRC       13          /* 【r146】采集相位(4/5/7/8/35)    */
#define VBE_TR_FS_CR0C      15          /* 【r149】CR0Ch = 显示起始地址低位。
                                         *   取指基址 = (CR0C/CR0D 拼出的起始
                                         *   地址) —— 它决定扫描从显存哪个字
                                         *   开始读字符/属性对。r143..r148 量了
                                         *   13 个寄存器, 唯独没量它: 所以"盒子
                                         *   的文本写在一处、扫描从另一处读"这个
                                         *   假设一直无法证伪。映射到 A0000 的
                                         *   传统文本模式下它应当是 0(基址 =
                                         *   B8000)。占用 FS_LEN=16 的最后一个
                                         *   空槽, 指纹长度不变。              */
#define VBE_TR_FS_AR10      14          /* 【r147】AR10(属性控制器模式寄存器:
                                           bit0 文本/图形, bit1-3 闪烁与行
                                           图形, bit6 8 位色/palette 地址源)。
                                           ./136 的两个现场读数给出本机标定:
                                           干净态 41h / 花屏态 0Dh —— 它决定
 *                                           【r150 补充位义】bit0 = 0 文本 /
 *                                           1 图形; bit6 = 1 时 8 位色(调色板
 *                                           地址来自 PEL), 0 时 16 色。于是
 *                                           41h = 图形 + 8 位色 = 我们的线性
 *                                           打包像素模式; 0Ch = 文本 + 闪烁
 *                                           + 行图形。./148 的整条相位链:
 *                                           p4 AR10=41(我们) -> p5/p48 0C
 *                                           (文本态修补 写的文本值) ->
 *                                           p35FIRST 0C(主 VDD 设完盒子模式
 *                                           后【仍是】文本) —— 也就是说 AR10
 *                                           这条路上【没有】不一致, 它与"盒子
 *                                           花屏"无关。下面几行同此。
                                           "扫描输出从哪条路径取颜色", 是
                                          取指链上的第三个开关, 而 r146 的
                                           13 个寄存器里没有它。           */
#define VBE_TR_FS_LEN       16          /* 块长(最后一个字节保留 0)        */

/* 【r147】VBE_TR_PhaseSnap —— 每个采集点各占一个 16 字节槽。
 *   【为什么必须分开】./146 的日志里 4/5/7 三拍【一次都没出现过】:
 *   六次落盘的相位字节全是 0x23(35 号持屏期活采样), 因为 35 号在持屏期
 *   被调用几百次, 每次都把同一块 FboxSnapVal 覆盖掉 —— "最后一个写的人
 *   赢"让整条因果链只剩终点。分开存以后, "进盒前 = 我们的模式 / 我们写完
 *   文本值之后 / 主 VDD 设完模式之后 / 回到 Windows 之后"四拍可以并列在
 *   同一次落盘里比较。
 *   【35 号为什么两个槽】P35 是"最近一次"(持屏期一直在更新); P35FIRST 是
 *   "本场会话的第一拍" —— 4 号会把 FboxLiveTick 预置成 0Fh, 所以 35 号在
 *   一场会话里的第一次调用必然采样, 那一拍就是"主 VDD 刚把盒子模式设完"
 *   的现场, 也正是花屏现场; 它不会被后续采样覆盖。                  */
#define VBE_TR_PhaseSnap    256         /* 【r147】db×96: 6 槽 × 16 字节   */
#define VBE_TR_PS_P4        0           /* 4 号(PRE_HIRES_TO_VGA)采样     */
#define VBE_TR_PS_P5        16          /* 5 号(POST_HIRES_TO_VGA)采样    */
#define VBE_TR_PS_P7        32          /* 7 号(POST_VGA_TO_HIRES)采样    */
#define VBE_TR_PS_P48       48          /* 48 号(PRE_INT_10_MODE_SET)采样 */
#define VBE_TR_PS_P35       64          /* 35 号持屏期"最近一次"采样      */
#define VBE_TR_PS_P35FIRST  80          /* 35 号持屏期"本场第一拍"采样    */
#define VBE_TR_PS_SLOTS     6           /* 槽数(api_gettrace 按 6×16 搬)  */
#define VBE_TR_PS_LEN       16          /* 槽长(布局同 VBE_TR_FS_*)       */

/* 【r147】VBE_TR_MemC —— 持屏期"文本取指/平面"补写的记账(16 字节)。
 *   【为什么要这么细】./146 证明了"写了"和"写住了"是两件事, 而补写这条
 *   路(r143 起)在四轮里一次都没执行过。本块让每一次"发现不一致 → 写 →
 *   读回"都留下数字: 见得多而写得少 = 判据还在挡; 写了而 stillBad>0 =
 *   我们的写没落进硬件(端口虚拟化/写保护); 写了且 stillBad=0 而画面仍花
 *   = 目标值本身不对(下一轮就该去量别的寄存器了)。                 */
#define VBE_TR_MemC         352         /* 【r147】db×16                   */
#define VBE_TR_MC_SEENTEXT  0           /* +0 文本态(GCR06.bit0=0)里被
                                           看一眼的次数(模 256)          */
#define VBE_TR_MC_FIXED     1           /* +1 真正写了那 6 个寄存器的次数 */
#define VBE_TR_MC_STILLBAD  2           /* +2 写完读回仍不一致的个数(最近) */
#define VBE_TR_MC_GFX       3           /* +3 见到"图形模式"的次数(模 256) */
#define VBE_TR_MC_LASTSR02  4           /* +4..+9 最近一次"发现不一致"时
                                           +5        的现场读数, 顺序:
                                           +6        SR02/SR03/SR04/
                                           +7        GCR05/GCR06/AR10  */
#define VBE_TR_MC_LASTSR03  5
#define VBE_TR_MC_LASTSR04  6
#define VBE_TR_MC_LASTGCR05 7
#define VBE_TR_MC_LASTGCR06 8
#define VBE_TR_MC_LASTAR10  9
#define VBE_TR_MC_DIRTY     10          /* +10 发现不一致的次数(模 256)   */
/* 【r167】整组 mode 3 写入(17 字节)的逐字节读回结果。
 *   为什么要有它: ./166 的读数把"写了"与"写对了"彻底分开 ——
 *   stillBad=0(六字节每次都读回成立)而画面照旧花屏, 于是"到底是哪一格没站住"
 *   必须成为一个可读的数, 而不是再猜一轮。位序(写入顺序即位序):
 *     MCBAD0  bit0 SR02    bit1 SR03    bit2 SR04    bit3 GCR00
 *             bit4 GCR01   bit5 GCR02   bit6 GCR03   bit7 GCR04
 *     MCBAD1  bit0 GCR05   bit1 GCR06   bit2 GCR07   bit3 GCR08
 *             bit4 CR0C    bit5 CR0D    bit6 AR10    bit7 AR12
 *     MCBAD2  bit0 AR13
 *   判读: 全部 0 = 17 个字节都读回相等(我们的写站住了); 某一位恒置 = 那一格
 *   被别的东西按住, 而且日志直接说出是哪一格。
 *   MCWR = 整组写入的次数(模 256); 它恒 0 而 mcbad 也恒 0 = 这条路根本没跑。 */
#define VBE_TR_MC_MCBAD0    11
#define VBE_TR_MC_MCBAD1    12
#define VBE_TR_MC_MCBAD2    13
#define VBE_TR_MC_MCWR      14
/* 【r168】VBE_TR_MC_FONTBADG —— r147 起留作"保留 0"的最后一格, 含义在此落地。
 *   低 6 位 = 最近一次"整块回读"里不一致的 dword 数(0..63 饱和):
 *       0 = 那 8KB 逐字节相等(字模整块站住了);
 *       非 0 = 我们的拷贝没有整块落地 —— 这【不是】"画面为什么花"的结论,
 *              而是"我们没有能力把字模写对"这一条事实的读数。
 *   高 2 位 = "让卡自己的 BIOS 重设一次 mode 3"(嵌套 V86 INT 10h AX=0003)
 *   的结果: 0 = 没执行(拷贝成立/策略关/本场预算用尽), 1 = 执行且 BIOS 回
 *   AH=0, 2 = IVT[10h] 为 0 没敢执行, 3 = 执行但 BIOS 回 AH!=0。
 *   判读: `fontbadb` 的 00..3F = 0 而画面仍花 ⇒ 字模这一环已被证清, 病灶在
 *   标准 VGA 寄存器集之外; 高两位非 0 ⇒ 那一手到底跑没跑、BIOS 怎么回答的。
 *   【为什么两个含义共用一格】块长固定 16 字节(块外紧接 VBE_TR_PhaseSnap),
 *   而坏 dword 数只用来回答"整块有没有站住", 精度超过 6 位没有意义。两个
 *   子字段的边界写在这里, 免得下一轮把高两位当成坏格数去读。 */
#define VBE_TR_MC_FONTBADG  15
#define VBE_TR_MC_LEN       16

/* 【r146】VBE_TR_Hires —— 4 号(PRE_HIRES_TO_VGA)采、7 号(POST_VGA_TO_HIRES)
 *   回放的"我们那一份模式/寻址字节"的记账。
 *   【为什么这件事归 mini-VDD】DDK 的分工: 主 VDD 保存/恢复的是【标准 VGA】
 *   寄存器; "超出标准 VGA 的那部分"由 mini-VDD 自己负责(见 About Virtual
 *   Display Device 的 CRTC owner / MemC owner 一节)。对一个【线性打包像素】
 *   的 VBE 模式来说, "超出标准 VGA"的部分正是"显存寻址/平面"那几位 ——
 *   而 ./145 的现场读数把它们指了出来: 回到 Windows 之后
 *   `SNAP GCR06=0005`(对) 而 `SNAP SR01=0003`(我们的模式是 0001):
 *   "图形位"补回来了(r129 的 HiResClockFix 只补 bit0 那一族), 而
 *   寻址/平面/带宽那几位没人管。本块就是这条机制的读数。            */
#define VBE_TR_Hires        248         /* 【r146】8 字节: +0 valid/+1 savedN/
                                           +2 restoredN/+3 lastDiff/
                                           +4..+7 【r150】48 号分类计数器
                                           (N48M72/N48ARM72/I10EBX —— 见下面
                                           三个偏移常量)。VxD 侧原名
                                           HiresSpare: 一直在搬, 一直是 0。 */
#define VBE_TR_HR_VALID     0           /* 1 = 手里有一份可用快照          */
#define VBE_TR_HR_SAVEDN    1           /* 保存次数(模 256)               */
#define VBE_TR_HR_RESTN     2           /* 回放次数(模 256)               */
#define VBE_TR_HR_DIFF      3           /* 回放前"与现场不同的字节数"      */
/* 【r150】+4..+7 的用途 —— 48 号(PRE_INT_10_MODE_SET)的分类计数。
 *   【为什么非要有它】r149 把改写放开成无条件, 依据是"武装期里 48 号只有 6 次、
 *   且请求的是 03h", 可那 6 次之外还有 44 次, 日志里【分不出】它们是窗口化盒子、
 *   控制台还是 Windows 自己发的, 也分不出请求的是 72h 还是 03h —— 两个笼统计数器
 *   把因果链的两端粘成了一个数。于是那一轮只能靠推断, 推断错了, 代价是桌面。
 *   这四个字节零成本(VxD 侧 HiresSpare 早就在 api_gettrace 里被整块搬走):
 *     +4 N48M72   请求 72h/73h 的次数(模 256, 在改写【之前】判)
 *     +5 N48ARM72 其中发生在 VgaFixArm != 0 的次数 —— 判"闸门放回去之后
 *                 改写还够不够得着"的唯一读数。0 = 武装期里根本没人请求 72h
 *                 (那 r151 就该换判据, 而不是再把闸门放开); >0 = 够得着。
 *     +6..+7 I10EBX 最近一次 72h/73h 请求者的 EBX 低字。与日志里的
 *                 `VMH edge lo=.. hi=..`(WinVMHandle)对照即可判"这次是不是
 *                 Windows 自己发的" —— 若是, 改写永远不该碰它。
 *   【纪律】任何闸门都必须留下"它开过没有"的数字, 否则下一轮又会像 r149 一样
 *   把一个静默的闸门读成"条件不成立"。 */
/* 【r164】这三个字节换了含义(旧的 72h/73h 分类问题 ./150..163 已经答完:
 *   武装期里盒子请求的是 03h, 72h/73h 一律发生在武装之前)。新含义 = 字模守卫:
 *   见 minivdd.asm 的 字模一致性检查 —— 平面 2 的字模会被"写全部四个平面"的
 *   文本写覆盖, 于是每格都变成属性前景色的实心块(./148 的整片 #AAAAAA)。 */
#define VBE_TR_HR_FONTN     4           /* 8x16 字模重装次数(模 256)       */
#define VBE_TR_HR_FONTBAD   5           /* 发现字模被覆盖的次数(模 256)    */
#define VBE_TR_HR_FONTVER   6           /* +6..+7 最后一次读回的字模指纹   */
#define VBE_TR_DispiPhys    216         /* 【r120】ring 0 物理 I/O 读回的
                                           DISPI 寄存器块(16 字节)。
                                           【为什么值得单开一块】./119 的
                                           ring 3 "写—回读—还原"梯形跑了
                                           28 次全是 FFFF, 但那【不能证明
                                           端口不存在】—— 它也可能是"我们
                                           那次 I/O 没抵达硬件"(主 VDD 在为
                                           Windows VM 虚拟化这两个端口)。
                                           ring 0 的 VDD_Do_Physical_IO 绕开
                                           VM 端口虚拟化, 是唯一能分开这两
                                           种可能的通路, 所以两种结论的
                                           后续动作完全相反, 必须测。
                                           布局:
                                             +0..+11  六个字, 顺序:
                                               ID / XRES / YRES / BPP /
                                               ENABLE / VIRT_WIDTH
                                             +12  db  物理读跑过的次数
                                             +13  db  1 = 回读命中(端口活)
                                             +14  db  失败次数(模 256)
                                             +15  db  保留 0
                                           旧 vxd + 新 drv 时这一段为 0,
                                           drv 按"还没读过"处理(反之亦然,
                                           两侧都不看对方这一块的长度)。 */
#define VBE_TR_DP_N         12          /* 块内记账字节偏移: 读次数     */
#define VBE_TR_DP_OK        13          /*              回读命中(0/1)   */
#define VBE_TR_DP_ERR       14          /*              失败次数        */
/* 【r151】屏幕所有权 / 特殊 VM / 35 号 AL(24 字节, 必须连续:
 *   api_gettrace 用 6 条 mov dword 整块搬走)。
 *   【为什么需要它】DDK《Virtual Display Minidriver》给了一条判据级的定义:
 *     "The CRTC owner VM is the VM that owns the screen mode. ... If you are
 *      running a MS-DOS session in a window, then the CRTC owner is Windows
 *      but the MemC owner is the MS-DOS VM. ... you can restore those states
 *      from the CRTC owner that preserve the appearance of the screen while
 *      restoring those states from the MemC owner that control how the CPU
 *      sees the video memory."
 *   而 DDK《RESTORE_REGISTERS (Function 9)》给出这两个所有者的【取法】:
 *     "ESI: Contains the VM handle for the MemC owner VM.
 *      ECX: Contains the VM handle for the CRTC owner VM."
 *   于是 9 号是整台机器上唯一能【直接读出】"屏幕归谁"的地方 —— 而 ./150 里
 *   它一直是 `TraceStub 9`(只记一条就转交), 什么都没看。本块就是把它变成
 *   读数; 本轮【不写任何硬件】(670 次/场的调用里改行为太贵)。          */
/* 【r152】VBE_TR_Probe —— 取指基址探针 + 修复记账(32 字节)。
 *
 * 【为什么这一块是 r152 的全部重点】
 *   r143..r151 把 SR/GCR/AR 与 CRTC 的模式寄存器量了个遍, 却从没有量过
 *   两件事: ① 控制台那 4000 个文本单元此刻落在 CPU 地址空间/显存的哪里;
 *   ② CRTC 的取指地址读到的到底是不是同一块内存。没有这两个数,
 *   "写在一处、读在另一处"永远只是假设。
 *
 *   而它恰好是唯一能同时解释三件实测事实的机制:
 *     · ./144/1.png 全屏花屏: 屏幕上是"位置对、内容错"的东西;
 *     · ./148 (MemCFix=1 六字节补写打开) 变成灰屏/黑屏, 且 co80 失效;
 *     · `mode co80`(盒子里自己发 INT 10h AX=0003) 一按就好 —— 因为
 *       DDK《PRE_HIRES_TO_VGA (Function 4)》第 6 步写得很清楚:
 *       "The Main VDD executes an Interrupt 10h, function 0 mode change
 *        to enter the mode that the VM is running in."
 *       也就是说, 一次完整的 BIOS 设模式会把【取指基址与显存映射一起】
 *       摆成自洽的一组; 而我们从来没有量过那一组里的"基址"。
 *
 * 【判读表(一次开机就能读出结论)】
 *   PR_NB 高 + PR_NT 高 + PR_NX 低
 *       ⇒ 控制台数据在显存里、CPU 的 B8000 能看到它, 而 CRTC 的取指
 *         没读到它 ⇒ 只需要挪显示起始地址(CR0C/CR0D)。这就是本轮的修复。
 *   PR_NB 高 + PR_NT 低
 *       ⇒ CPU 在 B8000 看到的东西【不是这块显存】(多半是落到了 RAM 的
 *         传统 B8000 影子)。挪起点永远救不了, 必须改 GCR06 的显存映射
 *         选择并让盒子重画 —— 下一轮的事。
 *   PR_NX 高
 *       ⇒ 取指本来就对着控制台, 花屏另有原因(那就回到属性/调色板那条线)。
 *
 * 【写什么】VgaFetchFix 只写 CR0C/CR0D(显示起始地址) —— 硬件滚屏每帧
 *   都在改的一对寄存器, 不是时序寄存器。它【绝不】写 SR/GCR/AR,
 *   与 r147 那六字节补写(./148 实测有害)是两件事。
 * 【怎么保证不越修越糟】写完立刻按写进去的值重算取指地址并重数 32 格,
 *   >=24 才算成立; 三个单位候选(off/2, off/4, off/1)全不成立就把原值
 *   写回去并锁定本场。总写次数上限 8。
 * 【偏移纪律】VxD 侧直接用 VBE_TR_PR_* 这套名字: Probe 是这一块的逐字节
 *   映像(与 MemC 同一套路), 两侧偏移相同, 不存在 OwnWord 那种错位风险。
 *
 * 【r153 —— r152 这一块为什么什么都没看见, 以及改成什么】
 *   ./152 的现场把这个探针的三处错一次全暴露了:
 *     ① 判据"像不像文本"(字符 20..7E + 属性 01..7F)对【空格屏幕】恒真。
 *        控制台的左上角本来就是 4000 格里最空的 32 格 —— 实测
 *        `PH: probe rawb==2007`(空格 + 属性 07)、`cells==0020`(32/32 全中),
 *        于是三处地址全判"很好", `fix/cand/new` 三个计数器【恒 0】:
 *        探针一次都没修过, 也一次都没看过真正有字的地方。
 *     ② 采样只落在偏移 0 的 64 字节上 —— 而那 64 字节在 ./152 里正好是
 *        主 VDD 摆好的 B8000 32K 映射下"显存偏移 0", 也就是【取指本来就
 *        对着的地方】, 所以三处读数必然相同(NB=NT=NX=32/32)。
 *     ③ 真正要问的问题从来不是"像不像文本", 而是【取指读到的是不是
 *        CPU 在 B8000 写进去的那块内存】—— 这是一个可以逐字节证明的
 *        等式, 不是启发式。
 *   r153 因此把 Probe 从 32 字节扩到 248 字节, 0..31 一个字节不动
 *   (./152 的读数保持可比), 新增的全是【把真字节抄回来】:
 *     · AGREE: 取指地址 32 字节 与 CPU B8000 32 字节 是否逐字节相等 ——
 *       文本模式能工作的定义就是这个等式成立;
 *     · 铺开采样(4 行 × 8 格, 步长 320 字节)的非空格数与字符种类数;
 *     · 显存偏移 0..E000h 逐 2000h 扫描, 报"最像盒子屏幕"的那一个偏移
 *       及其非空格数(找盒子那 4000 格到底在哪);
 *     · 四处 32 字节原文: CPU B8000 / 取指地址 / CPU A0000 / 显存 8000h;
 *     · AR00..AR0F 属性调色板 16 个 + AR10..AR13 + DAC 条目 0/7/15。
 *       最后这一组是 ./152 灰屏(1.png 全屏 #AAAAAA 纯浅灰 + 一个白条)
 *       的唯一未测环节: 显存里明明是 20 07 的空屏(黑底灰字), 屏幕却是
 *       浅灰 ⇒ 颜色管线(属性调色板或 DAC)与内存不一致。r153 把它读出来。
 *     · POLICY: VxD 侧真正收到的 MemCFix 档位。./152 的 `CFG27 ... 0002`
 *       是【驱动侧】的值, 而 VxD 侧收到的是 1(drv.c 的 `memcfix ? 1 : 0`
 *       把 2 夹回了 1)⇒ 默认档 2 从未生效, 跑的是 ./148 那条已被证伪的
 *       六字节补写。两侧各印一次, 这一级错位以后不可能再隐藏。
 */
#define VBE_TR_Probe        392         /* db×256                       */
/* 【r182】+0..+4 的主人是全屏文本页那条路(r181 起接管 r152 的"打印格数"
 *   那五个查位, 见 minivdd.asm 数据区 PgStart 那段长注与 drv.c 的
 *   nbnt/nxna/cells 判读表)。r182 换掉了 r181 的含义 —— ./181 证明
 *   "00h 格数"分不出文本页与像素残留(像素残留里 00h 也不多), 于是改成:
 *   +0    = 进门时【字符字节的种类数】(0..255, 256 记 0FFh)
 *   +1    = 进门时【规范格数】(字符 20h 且属性 07h) >> 3, 0..250
 *   +2    = 清页后【字模指纹的低字节】(是否等于 FontRef 看 +4 的 bit6)
 *   +3    = 清页后【仍不规范的格数】>> 3, 0..250
 *   +4    = 判定位图, 八位全用:
 *             bit0 本拍清过页            bit1 进门那页"看着像文本页"
 *             bit2 快照已取              bit3 清页后读回自证成立
 *             bit4 进门即全规范          bit5 进门过半规范
 *             bit6 清页后字模指纹没变    bit7 这一拍有别人写过页面
 *   一页刚被清过的文本屏: +0 小(几十), +1 = 0FAh(250), +3 = 0, +4 含 bit0
 *   +bit2 +bit3。像素残留: +0 接近 0C0h..0FFh 而 +1 很小。 */
#define VBE_TR_PR_NB        0           /* +0 进门: 字符种类数(0..255)   */
#define VBE_TR_PR_NT        1           /* +1 进门: 规范格数 >> 3        */
#define VBE_TR_PR_NX        2           /* +2 清页后: 字模指纹低字节     */
#define VBE_TR_PR_NA        3           /* +3 清页后: 仍不规范格数 >> 3  */
#define VBE_TR_PR_CELLS     4           /* +4 页面判定位图(见上)        */
#define VBE_TR_PR_MAP       5           /* +5 GCR06 & 0Ch(显存映射选择) */
#define VBE_TR_PR_CR0C      6           /* +6 显示起始地址低位          */
#define VBE_TR_PR_CR0D      7           /* +7 显示起始地址高位          */
#define VBE_TR_PR_FIXN      8           /* +8 写 CR0C/CR0D 的次数       */
#define VBE_TR_PR_FIXOK     9           /* +9 其中"写后自证成立"的次数   */
#define VBE_TR_PR_FIXBAD    10          /* +10 候选全败并已还原的次数    */
#define VBE_TR_PR_LATCH     11          /* +11 1 = 本场会话已放弃       */
#define VBE_TR_PR_CAND      12          /* +12 生效的候选(1=off/2 2=off/4 3=off) */
#define VBE_TR_PR_NXA       13          /* +13 写完重测的取指打印格数    */
#define VBE_TR_PR_NEWL      14          /* +14 写进去的低字节           */
#define VBE_TR_PR_NEWH      15          /* +15 写进去的高字节           */
/* --- r153 起新增(0..31 与 r152 逐字节一致) --- */
/* r412: charset A/B plane-2 bases + clone status, on the dead RAWB bytes */
#define VBE_TR_PR_CS03      16
#define VBE_TR_PR_CSST      17
#define VBE_TR_PR_CSA       18
#define VBE_TR_PR_CSB       20
#define VBE_TR_PR_CSN       22
#define VBE_TR_PR_BS3POL    23
#define VBE_TR_PR_BS3BDA    24   /* r412: BDA 0:0449 read inside the nest */
#define VBE_TR_PR_BS3MD     25   /* r412: the mode actually sent */
#define VBE_TR_PR_BS3ST     26   /* r412: Bios3Stat read directly */   /* r412: policy byte the BIOS-replay gate saw */
#define VBE_TR_PR_N11       27   /* r412: 11 号次数 */
#define VBE_TR_PR_M11       28   /* r412: 11 号的 BDA 0449 */
#define VBE_TR_PR_G11       29   /* r412: 11 号的 GCR06 */
#define VBE_TR_PR_S11       30   /* r412: 11 号的 SR04 */
#define VBE_TR_PR_T11       31   /* r412: 11 号结果(0/1/2) */
#define VBE_TR_PR_N12       65   /* r412: 12 号次数 */
#define VBE_TR_PR_G12       66   /* r412: 12 号的 GCR06 */
#define VBE_TR_PR_S12       67   /* r412: 12 号的 SR04 */
#define VBE_TR_PR_T12       69   /* r412: 12 号的 BDA 0449 */
#define VBE_TR_PR_PBDA      70   /* r412: PRE 读到的 BDA 0449 */
#define VBE_TR_PR_PBXMD     73   /* r412: PRE 真正发出的模式号 */
#define VBE_TR_PR_CRTB      74   /* r412: CRTC geometry mismatch bitmap, before */
#define VBE_TR_PR_CRTA      75   /* r412: the same, after the repair (FF = was fine) */
#define VBE_TR_PR_CR0CB     77   /* r412: CR0C before */
#define VBE_TR_PR_CR0DB     78   /* r412: CR0D before */
#define VBE_TR_PR_CR0CA     19   /* r412: CR0C after */
#define VBE_TR_PR_CR0DA     21   /* r412: CR0D after */
#define VBE_TR_PR_RAWB      16   /* alias */
/* r412: the RAWT alias is gone -- nothing has written 24..31 since r209 */
#define VBE_TR_PR_POLICY    32          /* +32 VxD 真正收到的 MemCFix 档位 */
#define VBE_TR_PR_AGREE     33          /* +33 1 = 取指 32 字节 == CPU B8000 */
#define VBE_TR_PR_NBF       34          /* +34 取指处非空格数(铺开采样)   */
#define VBE_TR_PR_NBV       35          /* +35 CPU B8000 处非空格数       */
#define VBE_TR_PR_VARF      36          /* +36 取指处字符种类数           */
#define VBE_TR_PR_VARV      37          /* +37 CPU B8000 处字符种类数     */
#define VBE_TR_PR_TOPOFF    38          /* +38 最佳扫描序号 0..15(偏移=n*2000h) */
#define VBE_TR_PR_TOPVAL    39          /* +39 该序号处的非空格数         */
#define VBE_TR_PR_SCANN     40          /* +40 扫描过的候选偏移个数       */
#define VBE_TR_PR_SCAND     41          /* +41 1 = 本会话的扫描已做过     */
#define VBE_TR_PR_BOFFL     42          /* +42 B8000 映射到的显存偏移低字节 */
#define VBE_TR_PR_BOFFH     43          /* +43 高字节                     */
#define VBE_TR_PR_C17       44          /* +44 CR17 原文(bit6 = 字/字节)  */
#define VBE_TR_PR_FETL      45          /* +45 取指显存偏移低字节         */
#define VBE_TR_PR_FETH      46          /* +46 高字节                     */
/* r166: the one byte 0..47 that was never used is 47 (45/46 jump to 48).
 * How many times the official RESTORE_REGISTERS(9) repair was stopped by the
 * physical "the CRTC is displaying standard mode 3 text" test; pair it with
 * VBE_TR_PR_GEOMASK, so a permanently closed gate is visible as a number. */
#define VBE_TR_PR_S9GEOF    47
#define VBE_TR_PR_AR00      48          /* +48..+63 AR00..AR0F 属性调色板 */
#define VBE_TR_PR_AR10      64          /* +64..+67 AR10..AR13            */
/* 【r412】68 与 71 归"文本调色板"仪器; 旧 DAC0/7/15 三格删除 ——
 *   它们的字节与 T12/PBDA/PBXMD/CRTB/CRTA/CR0CB/CR0DB 重叠。 */
#define VBE_TR_PR_I10N      68   /* r412: V86 INT 10h chain hook calls   */
#define VBE_TR_PR_I10M0     72   /* r412: of which AH=0 (mode set)      */
#define VBE_TR_PR_I10FIX    76   /* r412: AL 72h/73h -> 03h rewrites    */
#define VBE_TR_PR_PAL_ST    71   /* r412: palette + font status bits    */
/* 【r183】plane-2 window AND addressing mode that survived the self-test.
 *   0    = none of the five combinations worked (the fingerprint readout is
 *          blind in that case -- do not read fontst as a hardware verdict);
 *   1..5 = the combination that held, in the order minivdd.asm tries them:
 *          1 = GCR06 0Ch / SR04 06h / base 0B8000h -- the box's own text
 *              window with sequential addressing (the r183 default);
 *          2 = GCR06 0Ch / SR04 07h / base 0B8000h;
 *          3 = GCR06 00h / SR04 07h / base 0A0000h -- get_font_access()
 *              verbatim (A0000, 128K);
 *          4 = GCR06 04h / SR04 07h / base 0A0000h -- what r176..r182
 *              used (A0000, 64K);
 *          5 = GCR06 08h / SR04 07h / base 0B0000h -- the monochrome
 *              window (B0000-B7FFF, 32K), which r182 also tried.
 *   Why candidate 1 moved from A0000 to B8000: in ./182 the A0000 side
 *   returned the same two bytes whichever plane was selected
 *   (`fontp0 == fontfp == 1818`) and the 8 KB read-back was 63/63 dwords
 *   wrong, while the text window read a page that was 2000/2000 cells
 *   canonical (`nbnt=01FA`).  A window that cannot even tell the planes
 *   apart cannot be used to decide whether a write landed; the window the
 *   box itself uses for text can.
 *   The r180 value 00FF ("the aperture gate refused the base") is gone:
 *   every candidate base is now a literal inside VfcSetCand and is still
 *   re-checked by VfcBaseCheck, so "refused" simply means "try the next
 *   one".  On five failures VgaFontProv parks the window on candidate 4
 *   (r176..r182's), because 字模一致性检查/VgaFontBoot outside the ladder
 *   still write through [旧的字模窗口基址变量]. */
#define VBE_TR_PR_FONTWIN   79
#define VBE_TR_PR_VSML      201  /* r412: 4F03 mode low */
#define VBE_TR_PR_VSMX      202  /* r412: 4F03 mode high */
#define VBE_TR_PR_VSMH      203  /* r412: 4F03 return AH */
#define VBE_TR_PR_MEMB      80          /* +80..+111  32 字节 @ CPU B8000 */
#define VBE_TR_PR_MEMF      112         /* +112..+143 32 字节 @ 取指地址  */
#define VBE_TR_PR_MEMA      144         /* +144..+175 32 字节 @ CPU A0000 */
/* 【r184】176..207 这 32 字节原来是"显存 8000h 的 32 字节原文"(恒 0),
 *   现在改成三个时刻的【活的】标准文本寄存器组, 每组 7 字节:
 *   SR02, SR04, GCR04, GCR05, GCR06, CR0C, CR0D。
 *   ./183 已经把字模这一半判死(fontwin=1 / fontst=2), 剩下的候选全在
 *   "盒子持屏那一刻, 卡到底是文本态还是别的东西"上 —— 而这一组是唯一
 *   从来没有被【读回来】过的量(以前的读数要么是保存下来的 hi-res 快照,
 *   要么是"我们写了什么"的记录)。                                      */
#define VBE_TR_PR_R5IN       176        /* +176..+182 5 号进门(动手之前)   */
#define VBE_TR_PR_R5OUT      183        /* +183..+189 5 号收尾(交给 VDD 前) */
#define VBE_TR_PR_R5FIX      190        /* +190 收尾改写用掉的次数 0..3    */
#define VBE_TR_PR_R5OK       191        /* +191 1 = 收尾读回 == 文本值     */
#define VBE_TR_PR_R48        192        /* +192..+198 盒子 INT 10h 里那一组 */
#define VBE_TR_PR_R48N       199        /* +199 48 号样本数(上限 4)        */
#define VBE_TR_PR_PG16       200        /* +200..+207 收尾时 B8000 起 8 字节 */
/* --- r153 追加(占 208..247 的留白, 块长与所有既有偏移都不变) --- */
#define VBE_TR_PR_NOCOVER   208         /* +208 映射不解码 B8000 的累计次数  */
#define VBE_TR_PR_COVER     209         /* +209 1 = 当前映射解码主机 B8000   */
#define VBE_TR_PR_TGONCE    210         /* +210 r154: 本场已补过一次文本孔径组 */
#define VBE_TR_PR_TGTRIED   211         /* +211 补写尝试次数               */
#define VBE_TR_PR_TGOK      212         /* +212 读回确认映射已覆盖 B8000   */
#define VBE_TR_PR_TGBAD     213         /* +213 读回发现被扳回去           */         /* +209 1 = 当前映射解码主机 B8000   */
/* 【r155 追加 —— 全部落在 VBE_TR_PR_LEN(248) 之内】
 *   ① 为什么默认档那一手必须变成"每场一笔预算"而不是"整机一次":
 *      r154 的 TGONCE 本意是"本场只补一次", 但它[从不]在 4 号(PRE_HIRES_TO_VGA)
 *      复位 ⇒ 实际语义是"整个开机只补一次"。./154 的日志把这个差别变成了数字:
 *      一场会话里 seenText 从 8 涨到 83、dirty 从 4 涨到 34, 而 fixedN 恒为 1
 *      —— 修过的那一场之后, 三场会话一次都没再动手, 用户看到的正是"花屏
 *      和以前一模一样"。
 *   ② 为什么闸门里那句 Probe[COVER] 必须换成当场算: COVER 是 显存逐格采样 写
 *      的, 而探针在 持屏期修正 里排在 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 [之后]、而且每 32 次
 *      35 号才跑一次 —— 于是闸门读到的永远是上一场(或开机初值 0)的结论。
 *      ./154 的第一拍就是证据: cover=0001(COVER=0, 从未采过)而 tg=0101
 *      (TGONCE 已经置起) —— 它在"不知道映射是什么"的情况下就把那一笔用掉了。
 *   ③ 去抖与存活: 一条"六字节坏"的读数可能是盒子正在搬字体/中间态, 所以
 *      本场的第一笔立即补, 之后要求连续两拍都坏(TGRUN>=2)才补第二笔;
 *      TGRUNMAX 记下本场最长连续坏拍 —— 它若恒为 1, 说明这个去抖会把我们
 *      彻底挡在门外, 下一轮就该去掉它。
 *   ④ A0000 窗口有没有被动过: 全屏期间盒子的文本写到底落在 B8000(被映射
 *      挡掉 ⇒ 画面停住)还是落在 A0000(写得进去、但平面/奇偶配置把它搅乱
 *      ⇒ 画面是花屏), 这两种情况的正确修法不同。它只读 8 个字节, 不当闸门。 */
#define VBE_TR_PR_TGTRY     214         /* +214 本场默认档补写已用掉的次数(预算 8) */
#define VBE_TR_PR_TGRUN     215         /* +215 连续"六字节坏"的拍数(补写后清零)   */
#define VBE_TR_PR_TGREV     216         /* +216 上一笔补写活过的坏拍数(= 被扳回的延迟) */
#define VBE_TR_PR_TGWATCH   217         /* +217 1 = 正在观察上一笔补写的存活       */
#define VBE_TR_PR_TGRUNMAX  218         /* +218 本场最长连续坏拍数                 */
#define VBE_TR_PR_TGAACHG   219         /* +219 A0000 窗口与上一拍不同的次数       */
#define VBE_TR_PR_TGAASAME  220         /* +220 其中相同的次数(盒子没在写 A0000)   */
/* 【r156】DAC 读索引自证 —— 见 minivdd.asm 的 单个 DAC 项读取 长注。
 *   r153..r155 把条目号写进 3C8h(DAC【写】索引)再读 3C9h, 而 3C9h 的读走
 *   3C7h 设定的【读】索引; ./154 的 `dac7 = 2A 2A 00` 正是标准表里条目 6
 *   (棕色)的值 —— "错位一条"的指纹。修正后再读一次 3C7h(状态寄存器):
 *   它此时应报读模式(低两位 = 3), 否则本轮这条颜色读数就是无效的。 */
#define VBE_TR_PR_DACST     221         /* +221 3C7h 状态寄存器原文(3 = 读模式) */
/* 【r157】属性控制器纪律 + "谁在写这六个寄存器"的逐寄存器计数。
 *
 *   ① ATCRAW = 从 3C0h 读回来的那个字节。
 *      3C0h 是属性控制器的索引/数据共用端口, 它的【可读性不是 VGA 标准的一
 *      部分】。r133 起本项目一直用 "in al,3C0h 拿原索引 -> and 0E0h 保留 PAS
 *      -> or 10h 选 AR10 -> out 3C0h,al" 这个写法; r153 判定它在读回 0 时会
 *      把 PAS(bit5) 写成 0 = "正在装载调色板" = 【视频被关】, 于是修了
 *      显存寄存器回读 / AR10 写入 两处 —— 但同一个写法在
 *      FboxSnap(持屏期每 16 拍一次!) / HiResClockBits(7 号两处) /
 *      文本态修补(4+5 号) 里【原样留着】, 而 r153 的注释还写着"整个 VxD
 *      只有两处碰属性控制器"。r157 把剩下三处一并改成 r153 的纪律
 *      (自己写索引字节 30h = AR10 | PAS=1, 不读 3C0h, 也不再把读来的值写回),
 *      并且把那次"只为取证"的 3C0h 读结果留在这个字节里:
 *        读到 30h/0B0h/0FFh 等 bit5=1 的值  ⇒ 旧写法无害;
 *        读到 00h/10h 等 bit5=0 的值        ⇒ 旧写法【每次都在关视频】,
 *          那它就是我们找了很多轮的"花屏/灰屏"的直接来源。
 *
 *   ② 十个逐寄存器写入计数器。日志里 `TRC cnt f=..` 已经证明 VM 的端口写
 *      会被路由给我们(VIRTUALIZE_*_OUT 的绝对值计数), 而 `TRC n=` 环形记录
 *      会丢(./154: TRC lost=825, GCR_OUT 的绝对值是 106 而环形里一条不剩)。
 *      计数器不受环形丢弃影响, 于是"会话期间到底有没有人写 SR02/SR04/
 *      GCR05/GCR06"第一次变成可回答的问题 —— 而且读数就是"次数"本身。
 *      注意口径: 它们数的是【被路由到我们这里的】写; 全屏会话里主 VDD 也
 *      可能把 VM 的写直接放行(那时计数不动, 这是本读数的已知盲区)。 */
#define VBE_TR_PR_W_SR01    222         /* +222 SR01 写(时序器同步复位)  */
#define VBE_TR_PR_W_SR02    223         /* +223 SR02 写平面屏蔽          */
#define VBE_TR_PR_W_SR03    224         /* +224 SR03 字符映射选择        */
#define VBE_TR_PR_W_SR04    225         /* +225 SR04 内存模式(奇偶)      */
#define VBE_TR_PR_W_GCR05   226         /* +226 GCR05 图形模式           */
#define VBE_TR_PR_W_GCR06   227         /* +227 GCR06 显存映射选择       */
#define VBE_TR_PR_W_CR0C    228         /* +228 CR0C 显示起始低位        */
#define VBE_TR_PR_W_CR0D    229         /* +229 CR0D 显示起始高位        */
#define VBE_TR_PR_W_CR17    230         /* +230 CR17 CRTC 模式控制       */
#define VBE_TR_PR_ATCRAW    231         /* +231 从 3C0h 读回的原始字节   */
/* 【r158】切换回 Windows 那一刻的"漂移位图" —— 11 个被比较的字节各占一位。
 *   ./154 落盘的是 `TRC hires lastDiff (bytes wrong at switch-back)=0001`:
 *   一个非零读数, 却**说不出是哪一个字节** —— 于是它永远无法被据以行动
 *   (本项目第 N 次遇到"读数存在、含义却不可用")。而 HiresRestore 比较的
 *   11 个字节里恰好包含 MemC 那半边(SR02/SR04/GCR05/GCR06), 所以"哪一个
 *   漂了"是又一个观察"盒子交回屏幕时留下了什么"的窗口。
 *   位序(与 minivdd.asm 里插入的 `or` 一一对应):
 *     bit0 SR04  bit1 GCR06  bit2 CR13  bit3 CR17  bit4 SR01  bit5 SR02
 *     bit6 SR03  bit7 GCR03  bit8 GCR04 bit9 GCR05  bit10 CR14
 *   HDIFFL = bit0..7, HDIFFH = bit8..10。计数 HiresLastDiff 照旧保留。 */
#define VBE_TR_PR_HDIFFL    232         /* +232 漂移位图低字节(bit0..7)  */
#define VBE_TR_PR_HDIFFH    233         /* +233 高字节(bit8..10)         */
/* 【r160】安全网计数 —— "我们自己写错的那一笔被收回来了几次"。
 *
 *   ./159 的核心事故: 取指基址修复(VgaFetchFix)把一个哨兵值当偏移用了,
 *   把 CR0C/CR0D 写成 FF/FF(显示起始地址 = 1FFFEh = 128K 处), 而它自己的
 *   等式"自证成立"(原因见 minivdd.asm 的 r160 长注: MapToCpu 会把越界偏移
 *   按窗口大小回卷, 0x1FFFE & 7FFFh = 7FFEh -> BFFFEh, 与 B8000h 恰好相等),
 *   于是那一笔【没有被还原】; 而此后每一拍的 AGREE 都恒为 2(取指读不到),
 *   修复的第一条判据又把它自己永远挡在门外 —— 也就是说这个错【不会被它
 *   自己纠正】, 而 CR0C/CR0D 是我们绕过主 VDD 直接写的, 主 VDD 的 CRTC
 *   影子仍以为是 0 ⇒ 用户在盒子里敲的 `mode co80` 对它无效。
 *
 *   r160 因此加了一条安全网: 写完之后如果取指落到了当前映射窗口之外,
 *   而硬件里那两个字节还是我们写进去的值, 就把原始值写回去并锁定本场。
 *   这个字节数它执行过几次:
 *     > 0  ⇒ 还有一笔错的写通过了三道闸门(说明闸门仍需收紧), 但画面
 *            已经在同一拍被收回;
 *     = 0  ⇒ 三道闸门够用(这是期望值)。
 *   为什么不并进 FIXBAD: 两者含义不同 —— FIXBAD 是"三个候选都没通过当场
 *   的等式", NETC 是"通过了等式、事后才被证明把取指挪出了窗口"。混在一
 *   起就再也分不出是哪一种, 而 ./159 恰好是后一种。 */
#define VBE_TR_PR_NETC      234         /* +234 安全网写回原值的次数     */
/* --- r161: 把 MemC 半边的补写挂到 9 号(RESTORE_REGISTERS)之后的四个读数 ---
 *   官方依据(DDK《Virtual Display Minidriver》, "CRTC owner and MemC owner"):
 *     "If you are running a full-screen MS-DOS session, then that MS-DOS
 *      session's VM is the CRTC owner. ... you can restore those states from
 *      the CRTC owner that preserve the appearance of the screen while
 *      restoring those states from the MemC owner that control how the CPU
 *      sees the video memory."
 *   而 9 号页(Call With)把这两个所有者写成了寄存器: "ESI: Contains the VM
 *   handle for the MemC owner VM.  ECX: Contains the VM handle for the CRTC
 *   owner VM." ⇒ "CPU 怎么看到显存"归 mini-VDD、并且在【官方恢复寄存器
 *   状态的那一拍】落地, 是文档的原文分工, 不是我们的发明。
 *   ./160 的现场把"为什么必须挪到这一拍"写成了数字: 一场全屏会话的第一拍
 *   (f35)读到的是教科书文本值, 最后一拍(l35)已经回到 4 平面那一组
 *   (SR02=0F SR04=06 GCR05=00 GCR06=04), 而同一场里我们只补了 2..3 次
 *   (预算 8 + 去抖 2)。于是画面一直是 1602.png 那种"三种取指塌到一个平面"
 *   的花屏, 只有整组模式设置(mode co80)能救。
 *   四个读数:
 *     S9N   = 本场 9 号被调次数(4 号清零)。恒 0 ⇒ 这条钩子没被调用,
 *             "挂在官方事件上"这个前提是假的, 必须回头查 35 号那条路。
 *     S9OK  = 其中"五个不碰 ATC 的字节与文本目标逐个相等"的次数(4 号清零)。
 *     S9FIX = 因为不等而真的补写的次数(累计, 无上限 —— 判据自己会停)。
 *     S9BAD = 补写后当拍读回仍不成立的次数(累计, 期望 0)。 */
#define VBE_TR_PR_S9N       235         /* +235 本场 9 号被调次数        */
#define VBE_TR_PR_S9OK      236         /* +236 其中本来就对的次数       */
#define VBE_TR_PR_S9FIX     237         /* +237 真的补写的次数(累计)     */
#define VBE_TR_PR_S9BAD     238         /* +238 补写后读回仍不对(累计)   */
/* 所有者核对的成败次数(4 号清零)。9 号页把两个所有者写成寄存器, 而"该不该
 * 由我们补 MemC 那半边"取决于 MemC 所有者是谁:
 *   S9SAME = ESI == OW_BOXVM(4 号交来的"正在切进去的那个 VM") ⇒ 全屏会话里
 *            两侧同一个 VM, 补写成立;
 *   S9DIFF = 不是它 ⇒ 主 VDD 正在为窗口化的 Windows 或某个伪 VM 摆 MemC
 *            状态, 我们一个字节都不动(./148 的灰屏就是在那儿拆了它的台)。
 * 判读: S9SAME 恒 0 而 S9DIFF 在涨 ⇒ 这条路在本机走不通, 回落层(35 号那条,
 * 节流已撤)在扛; 下一轮要查的就是 9 号给的句柄到底是谁。 */
#define VBE_TR_PR_S9SAME    239         /* +239 MemC 所有者 == 盒子 VM   */
#define VBE_TR_PR_S9DIFF    240         /* +240 不是它(按所有者拒写)     */
/* --- r162: 官方那一拍上"标准 mode 3 集合"到底缺了哪几个字节 ---------------
 *   依据(本仓 vga-register-reference.md §6b/§6c, 三个独立来源: DOSBox 的
 *   int10_modes 模式表 / 一份真实的 DOS 设模式清单 modes.asm / FreeVGA):
 *   mode 3 的标准寄存器集合是
 *     MCR(3C2h)=67  SR01=00 SR02=03 SR03=00 SR04=02
 *     GCR05=10 GCR06=0E
 *     CRTC CR00..CR18 = 5F 4F 50 82 55 81 BF 1F 00 4F 0D 0E 00 00 00 00
 *                       9C 8E 8F 28 1F 96 B9 A3 FF
 *     AR00..AR13 = 00 01 02 03 04 05 14 07 38 39 3A 3B 3C 3D 3E 3F 0C 00 0F 08
 *   而 文本态修补(4/5 号这一拍)写的是其中 8 个: SR02/SR03/SR04/GCR05/GCR06/
 *   CR14/CR17/AR10 —— 【唯一的遗漏是 SR01(Clocking Mode)= 00h】。
 *   SR01 bit0 就是"8/9 点字符时钟": 文本态要 0(9 点), 高分辨率要 1(8 点)。
 *   ./161 的读数里 SR01 只出现过 01h(4 号那一拍)与 03h(7 号那一拍),
 *   【整份日志没有一次读到标准值 00h】; 而 r158 的 hdiff 又恰好说"每次交回
 *   屏幕时唯一漂掉的字节就是 SR01(bit4)"。本轮因此把这一格补上, 并把
 *   "官方那一拍上到底哪几个字节不是标准值"变成三个读数:
 *     SR01VAL  = 那一拍读到的 SR01 原文(00 = 本来就是标准值, 我们没写)
 *     SR01BAD  = 因为不是 00h 而补写的次数(累计)
 *     GEOMASK  = 6 位几何掩码, 位序 = CR00 CR01 CR12 CR13 CR15 CR17,
 *                某位 = 1 表示该寄存器在那一拍与标准 mode 3 值不同
 *   ⇒ SR01VAL 恒 0 而画面依旧 ⇒ 时钟不是病根, 下一轮照 GEOMASK 走;
 *      SR01VAL 非 0 ⇒ 那次切屏的整组模式设置【根本没有落到硬件上】,
 *      几何掩码会同时亮起(两者同属一次设模式)。 */
#define VBE_TR_PR_SR01VAL   241         /* +241 官方那一拍读到的 SR01     */
#define VBE_TR_PR_SR01BAD   242         /* +242 因它非 0 而补写的次数     */
#define VBE_TR_PR_GEOMASK   243         /* +243 几何 6 字节的失配位图     */
/* 【r163 追加 —— 仍在 VBE_TR_PR_LEN(248) 之内, 最后 4 个字节用完】
 *   依据: DDK《MAKE_HARDWARE_NOT_BUSY (Function 15)》
 *     "EAX: Contains the CRTC owner's VM handle.
 *      EBX: Contains the currently running VM handle.
 *      ECX: Contains the MemC owner's VM handle.
 *      EDX: Contains the CRTC index register (which must be saved over call)."
 *     "If your hardware must complete a BLTer drawing operation before having
 *      its MemC state switched from Windows hi-res mode to VGA planar mode,
 *      then you must implement MAKE_HARDWARE_NOT_BUSY. ... This routine gives
 *      the mini-VDD notice that a switch of the MemC state will be occurring."
 *   ⇒ 15 号是官方文档里【唯一】把"MemC 状态正要从 Windows hi-res 切到 VGA
 *     平面态"写成事件的钩子, 并且一次交出三个所有者句柄。r163 用它取证:
 *     全屏盒子持屏期间那些 15 号调用里到底谁拿着 CRTC/MemC —— 这决定了
 *     "会话中途把盒子态改回孔径态的那只手"是谁(r143..r162 只能靠 35 号猜)。
 *   OWN15 位序(与 .inc 逐位一致):
 *     bit0 EAX==WinVM  bit1 ECX==WinVM  bit2 EBX==WinVM
 *     bit3 EAX==BoxVM  bit4 ECX==BoxVM  bit5 EBX==BoxVM
 *   YIELD: r163 的"认输"闸门 —— 补写被扳回一次之后, 本场不再和它对着干。
 *     它的依据是 ./162 的 TGRUNMAX 恒 1: 坏态从来没有连续出现过两拍, 也就是
 *     "那不是我们的状态"; 见 minivdd.asm 里 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 的 r163 长注。 */
#define VBE_TR_PR_N15       244         /* +244 15 号被调次数(模 256)     */
#define VBE_TR_PR_OWN15     245         /* +245 最近一次 15 号的身份位图  */
#define VBE_TR_PR_OWN15A    246         /* +246 武装期内那一次的身份位图  */
#define VBE_TR_PR_YIELD     247         /* +247 1 = 本场已认输(不再补写)  */
/* r163-marker: N15/OWN15/OWN15A/YIELD fill the last four probe bytes. */
/* r412: the six Graphics-Controller write-path bytes, before and after */
/* r412: the guest SR03 (character-map select), saved at PRE and restored at POST */
/* ---------------------------------------------------------------
 * 【r412】VBE 显示起始地址的一次性取证块(8 字节, 248..255)。
 *   见 minivdd.asm 的 ArmEvxDispStart 头注与 vbeshare.inc 的同名段。
 *   判据: EXV_RUN=1 表示这一枪发过; EXV_ST 位序见 .inc;
 *         EXV_RET = BIOS 亲口说的模式号(非 0 就是摸到真 ROM 了)。
 * --------------------------------------------------------------- */
#define VBE_TR_PR_RS_RUN   248
#define VBE_TR_PR_RS_ST   249
#define VBE_TR_PR_RS_AX   250
#define VBE_TR_PR_RS_MCR   251
#define VBE_TR_PR_RS_VBE   252
#define VBE_TR_PR_RS_P1   253
#define VBE_TR_PR_RS_P2   254
#define VBE_TR_PR_RS_P3   255
#define VBE_TR_PR_LEN       256         /* r153: 32 -> 248; r412: -> 256 */
#define VBE_TR_Own          368         /* db×24(见上)                   */
#define VBE_TR_OWN_MSGVM    0           /* +0..3  dd Blue Screen Message Mode
                                           伪 VM 句柄(本轮恒 0: 见长注 6) */
#define VBE_TR_OWN_PLANARVM 4           /* +4..7  dd Forced Planar Mode 伪 VM
                                           句柄(本轮恒 0: 见长注 6)        */
#define VBE_TR_OWN_BOXVM    8           /* +8..11 dd 4 号入口的 EBX —— DDK
                                           "the handle of the VM that is
                                           being switched to", 即盒子 VM */
#define VBE_TR_OWN_CRTCCLS  12          /* +12 db 最近一次 9 号 ECX(CRTC 所有者)
                                           的分类码(见 VBE_TR_OWN_CLS_*)  */
#define VBE_TR_OWN_MEMCCLS  13          /* +13 db 最近一次 9 号 ESI(MemC 所有者)
                                           的分类码                        */
#define VBE_TR_OWN_P35AL    14          /* +14 db 最近一次 35 号入口的 AL */
#define VBE_TR_OWN_P35MASK  15          /* +15 db 见过的 AL 种类位图(见
                                           VBE_TR_OWN_AL_*)               */
#define VBE_TR_OWN_PAIRN    16          /* +16 db (CRTC,MemC) 分类对发生变化
                                           的次数(模 256)                 */
#define VBE_TR_OWN_SAVECLS  17          /* +17 db 最近一次 8 号 EBX 的分类码 */
#define VBE_TR_OWN_PLANARN  18          /* +18 db ESI/ECX 命中 PlanarVM 的次数
                                           (模 256)                       */
#define VBE_TR_OWN_MSGN     19          /* +19 db ESI/ECX 命中 MsgVM 的次数
                                           (模 256)                       */
#define VBE_TR_OWN_LEN      24          /* 块长 */
#define VBE_TR_OWN_MAPCHG   21          /* 【r152】+21 持屏期显存映射选择          */
                                            /*   (GCR06 bit3-2)变化次数 —— "谁在   */
                                            /*   会话中途重新施加 4 平面孔径状态"   */
                                            /*   的直接读数。VxD 侧名字 OW_MAPCHG   */
#define VBE_TR_OWN_LASTMAP  22          /* 【r152】+22 最近一次看到的映射选择。    */
                                            /*   VxD 侧名字 OW_LASTMAP           */
#define VBE_TR_OWN_LASTPAIR 20          /* +20 上一次的 (CRTC,MemC) 分类
                                           对(高半字节 CRTC, 低半字节
                                           MemC); +21..+23 保留 0 */
/* 分类码 */
#define VBE_TR_OWN_CLS_NONE 0           /* 句柄为 0 或不认识 */
#define VBE_TR_OWN_BDA449   23          /* 【r175】+23 db 35 号那一拍的
                                           0:449(当前模式)字节。
                                           0FFh = 本场还没采到 */
#define VBE_TR_OWN_CLS_WIN  1           /* = WinVMHandle(Windows VM)      */
#define VBE_TR_OWN_CLS_BOX  2           /* = 4 号记下的盒子 VM            */
#define VBE_TR_OWN_CLS_OTHER 3          /* 既不是 Windows 也不是盒子 ——
                                           两个伪 VM 会落在这里           */
/* P35MASK 位 */
#define VBE_TR_OWN_AL_72    0x01        /* 见过 AL=72h/73h ⇒ 主 VDD 【没有】
                                           做 DBCS 折算(见长注 7)         */
#define VBE_TR_OWN_AL_23    0x02        /* 见过 AL=02h/03h ⇒ 做了折算     */
#define VBE_TR_OWN_AL_HI    0x04        /* 见过 AL>13h 的其它值           */
#define VBE_TR_OWN_AL_LO    0x08        /* 见过 AL<=13h 的其它值          */
#define VBE_TR_OWN_AL_BOXVM 0x10        /* 【r175】见过 35 号跑在盒子 VM 里 */
#define VBE_TR_OWN_AL_WINVM 0x20        /* 【r175】见过 35 号跑在 Windows VM 里 */

#define VBE_TR_Recs         648         /* 64 × 24 环形记录(r147: 256→368,
                                           r151: 368→392 —— 头部加了 Own) */

/* 【r101】VBE_TR_State 的位 */
#define VBE_TR_ST_VGA       0x01        /* 硬件已被主 VDD 切到 VGA(4 平面/
                                           文本)通路 —— 置位者: 4/5/11/26,
                                           清零者: 0/7/12                    */
#define VBE_TR_ST_TRAPS     0x02        /* ENABLE_TRAPS(13) 已生效          */
#define VBE_TR_ST_DISABLED  0x04        /* DISPLAY_DRIVER_DISABLING(26) 已发生 */
#define VBE_TR_ST_VGAFIX    0x08        /* 【r114】POST_HIRES_TO_VGA(5) 里
                                           VgaTextFix 确实被调用过(即: 装载的
                                           是 r114 的 minivdd.vxd, 且
                                           policyVgaFix 非 0)。日志里
                                           `TRC st bits=` 出现 0008/000A/000B
                                           就说明"文本取指补写"这条路被走到
                                           了 —— 这是判断测试机是否换上
                                           r114 的 minivdd.vxd 的直接依据
                                           (VBE_VXD_VERSION 刻意保持不变,
                                           新旧 VxD 可以混搭部署)。       */
#define VBE_TR_ST_DISPI     0x10        /* 【r116】1CEh/1CFh 的全局陷阱当前
                                           是【开着】的(装成 + 策略非 0 +
                                           最近一次是 TrapOn)。位只反映
                                           DispiTrapOn 的最近一次动作。    */
#define VBE_TR_ST_DISPI_EAT 0x20        /* 【r116】至少吃掉过一次 1CEh/1CFh
                                           的 I/O —— 即"有 VM 在 Windows
                                           拥有 CRTC 时想改 VBE_DISPI", 这
                                           正是本轮要挡的那件事。位置起后
                                           【不清零】, 便于一次会话内判读。*/
#define VBE_TR_ST_DISPI_PASS 0x40       /* 【r116】至少放行过一次(真硬件
                                           I/O) —— 与上一位配对读:
                                           EAT=0/PASS=1 说明陷阱生效但没
                                           有需要挡的访问; EAT=1/PASS=1
                                           说明两条路都走到了(全屏交接时
                                           本来就会放行)。                 */
#define VBE_TR_ST_VTFBLOCK  0x80        /* 【r116】VgaTextFix 被保守闸门
                                           挡下过(模式闸门或 CR17 闸门),
                                           且之后没有再成功执行过 —— 用来
                                           区分"VgaFix=1 但补写没跑"的两种
                                           原因。成功执行时会清掉这一位。  */

/* VBE_TR_Evt 的【4】个字节 —— 只有 +0..+3, 别再加了。
 * 【r131 实证】这里原本写着"8 个字节(+4..+7 是 r129 新增)", 那是错的:
 *   `VBE_TR_Evt = 4` 而 `VBE_TR_Counts = 8`, 中间只有 4 字节。r129 把
 *   HiResClockFix 的两个计数器放在 +4/+5, 于是 ring 3 读的
 *   `b[VBE_TR_Evt + 4]` = `b[8]` = **VBE_TR_Counts[0] 的低字节**
 *   ("0 号分派功能被调用几次") —— 连续两轮日志里的
 *   `TRC evt HiResClk fix=0001` 一直是个幻影读数。
 *   minivdd.asm 侧确实把 TraceEvt 声明成 8 字节, 但那 8 字节只在 VxD 自己的
 *   变量区里; api_gettrace 只搬 [TraceEvt+0..3] 一个 dword 进 blob。
 *   ⇒ 结论: 这一块【没有空位】放新计数器。需要新证据时请像 r131 那样在
 *     事件边沿【直接读硬件】(SR01/GCR06/AR10/CR17h), 不要再加计数器。 */
#define VBE_TR_EVT_POST_VGA_TO_HIRES 0  /* +0 POST_VGA_TO_HIRES(7) 次数:
                                           变化 = 主 VDD 的切屏事务已收尾
                                           ⇒ drv 可以安全地重建模式     */
#define VBE_TR_EVT_PRE_HIRES_TO_VGA  1  /* +1 PRE_HIRES_TO_VGA(4) 次数   */
#define VBE_TR_EVT_DISPLAY_DISABLING 2  /* +2 DISPLAY_DRIVER_DISABLING(26) */
#define VBE_TR_EVT_PRE_VGA_TO_HIRES  3  /* +3 PRE_VGA_TO_HIRES(6) 次数   */
/* 【r131 已作废, 不要再读】下面两个常量保留只是为了"出现过、别再犯":
 *   +4/+5 不在 blob 里(见上), 读到的是 VBE_TR_Counts[0]/[1]。
 *   替代物 = `TRC cr17 raw` / `TRC cr17 SE state` 与 SR01/GCR06/AR10 的直接
 *   读数(见 drv.c 的 VxdTraceDump 与 display.c 的 HwRegSampleAtRebuild)。 */
#define VBE_TR_EVT_HIRES_CLK_FIX     4  /* RETIRED r131 - not in the blob (aliases Counts[0]) */
#define VBE_TR_EVT_HIRES_CLK_NOP     5  /* RETIRED r131 - not in the blob (aliases Counts[1]) */

/* 【r134 为什么"内存环"从备用通道升成了主证据通道】
 *   THE TEXT LOG CAN DROP LINES.  ./133 的 `LOG skip InDOS / Reent / Cb` 一行
 *   写着 InDOS=0008 Reent=0640 Cb=004F: 一次会话有 1600 行在闸门处被丢掉,
 *   而闸门是按【写日志那一刻的上下文】丢的(回调窗口 / DOS 临界区 / 日志
 *   重入)。后果是: 任何"只写一次"的追踪(TraceOnce)只要第一次落在被丢的
 *   窗口里, 就【永久】消失 —— 日志里没有那一行, 并不等于那件事没发生。
 *   ./133 里 "DDI first call: USER repaint callback" 就是这样一行: 它在二进制
 *   里存在(字符串偏移 73547), 却一次都没落盘, 于是"桌面到底有没有被请求
 *   重绘"从日志里判不出来。
 *   ⇒ r134 起, 凡是【判定用】的事实(官方路径的语义判据结果、逐项寄存器
 *     差异、是否真的请求过重绘)一律同时写进 drv.c 的回调期内存环
 *     (s_cbRing, 只写内存、任何上下文都安全), 由下一次重建时的 CbRingDump
 *     落盘。日志仍然是第一通道, 但不再是唯一通道。
 *   r134-marker: the callback ring is load-bearing (the text log can drop lines). */

/* 记录内偏移(每 24 字节; r100 把 16 扩到 24, 新增 EBX/EAX) */
#define VBE_TREC_Seq        0           /* dd 序号(1 起)                  */
#define VBE_TREC_Func       4           /* dw 分派功能号                  */
#define VBE_TREC_Flags      6           /* db bit0 = 出口 CY(已处理)      */
#define VBE_TREC_Pad        7           /* db 置 0                        */
#define VBE_TREC_Ecx        8           /* dd 入口 Client_ECX             */
#define VBE_TREC_Edx        12          /* dd 入口 Client_EDX             */
#define VBE_TREC_Ebx        16          /* dd 【r100】EBX = 发起调用的
                                           VM 句柄(分派约定) —— 分辨
                                           "Windows 自己 vs DOS VM"的
                                           唯一依据                      */
#define VBE_TREC_Eax        20          /* dd 【r100】入口 Client_EAX,
                                           例: PRE_INT_10_MODE_SET 时
                                           就是被请求的模式号 AX        */

/* --- VBE_API_EXEC_BIOS 请求块 -------------------------------------------
 * ring3 填输入, VxD 回填输出。【r46 起】数据中转缓冲与桩全部由 VxD 侧
 * 自给: EnsureV86 经嵌套 AH=48h(或 B0000 物理暂存回退)分得常驻缓冲,
 * 触摸测试通过后置 g_v86Ok。ring3 只经自己 DGROUP 的静态缓冲 + 本块
 * bufLen/bufLin 与 VxD 交换数据 —— 【零低内存接触、零自建选择子】。
 *   真桩 9C 9A <ip:2> <cs:2> CF(= pushf; call far 真 V86 IVT[10h];
 *   iret)由 VxD 执行前写入自有缓冲+768, callf 目标当次现取; IVT[vec]
 *   (默认 0x63, IBM 保留用户区)的借用/恢复由 VxD 完成。
 * 全程不产生 int 10h —— VDD 的 INT 10h 钩子(假成功层)结构上无法介入。
 *
 * 【r76 瘦身】r69-r75 为"VxD 侧自建 LFB 映射"实验加的 lad / gt / MM
 * 系列诊断字段全部删除(该实验路线已整体废弃, 见文件头)。保留的字段
 * 都在生产路径上。
 * 【为何也套 pack(1)】本结构与 vbeshare.inc 的 VXE_* 偏移逐字节对齐,
 * 偏移是硬约定; 显式 pack(1) 后无论编译单元用 -zp1 还是 -zp2 布局都
 * 一致(drv.c 是 -zp1, display.c 是 -zp2 ——两份编译选项不同)。      */
#pragma pack(push, 1)
typedef struct {
    uint16_t in_ax, in_bx, in_cx, in_dx;    /* BIOS 输入寄存器           */
    uint16_t in_si, in_di, in_ds, in_es;
    uint16_t stubSeg;   /* api_exec 借向量前改写为 VxD 自有桩位置段;
                           GET_MAPINFO 借此字段回传 InitStep            */
    uint16_t stubOff;                       /* VxD 自有桩偏移(768)       */
    uint16_t vec;                           /* 借用的 IVT 向量(0x63)     */
    uint16_t out_ax;                        /* VxD 回填: BIOS 返回 AX    */
    uint16_t out_flags;                     /* VxD 回填: 返回 flags(bit0=CF) */
    uint32_t stubLin;   /* VxD 回填: 本次 callf 目标(cx:dx 打包, hi=seg);
                           —— 判目标是 BIOS ROM 还是 Win386 V86 反射桩 */
    uint16_t bufLen;    /* 数据中转字节数(4 的倍数); 0=无中转            */
    uint32_t bufLin;    /* 客户端(DGROUP)缓冲线性地址, 拷入/拷出         */
    uint16_t v86Seg;    /* VxD 回填: V86 中转缓冲实模式段                */
    uint16_t v86Ok;     /* VxD 回填: 缓冲可用(分配+触摸测试过)           */
    uint16_t stat;      /* VxD 回填: VBE_VXD_STAT_* 位图(里程碑取证)     */
    uint16_t step;      /* VxD 回填: InitStep 状态机步码                 */
    uint32_t lastEax;   /* VxD 回填: 步失败子码/服务返回快照             */
    /* 【r138 新增: BIOS 的完整输出寄存器组】
     *   为什么必须有(./137 的日志就是理由): r112..r137 每一条"硬件在我们
     *   的模式里吗"的判据, 读的都是 VGA 寄存器(3D4h/3C4h/3CEh/3C0h)。./137
     *   把这条路的边界量清楚了 ——
     *     CrtGeo: before verdict=0001  (CR00/CR01 算出来的行宽 = 1024, 对)
     *     CrtSync: CR17 raw=00E3 / SE state=0001        (同步使能, 对)
     *     SNAP ATC10=0041 / AR10h 干净                  (图形模式, 对)
     *   而用户看到的仍然是一屏"按 8bpp 重解出来的条纹"。也就是说, 决定画面
     *   成立与否的还有一份状态【不在 VGA 寄存器里】—— 线性打包像素模式的
     *   像素格式是芯片扩展状态(3dfx 的 vidProcCfg、各家 SVGA 的 pixel
     *   format 位), 通用 VBE 驱动结构上不知道它住在哪个端口。
     *   唯一通用、标准、厂商无关地问到它的通路是 VBE 自己的读回功能:
     *       4F03h Get Current Video Mode -> BX = 模式号
     *       4F06h Get Logical Scan Line  -> BX = 字节/行, CX = 像素/行
     *       4F07h Get Display Start      -> BX/CX
     *   一个"当前模式号不是 0117h"或"每行字节数不是 0800h"的读数, 是"
     *   硬件不在我们的模式里"最直接的证据 —— 比任何寄存器算术都硬。
     *   结果都在寄存器里, 而旧版请求块只回填 AX 与 flags, 所以这一段必须
     *   走 VxD: 与 ROADMAP 的 T1.2/T2.2 记的是同一件事。
     *   【为什么追加在结构末尾】旧字段偏移一个都不动; 采集侧与消费侧即使
     *   一边漏改, 也只是新字段读到 0, 老字段不会错位。
     *   【配套】VBE_VXD_VERSION 必须同步递增(0x0118 -> 0x0119), 否则旧
     *   minivdd.vxd 配新 display.c 会让这六个字段永远读到 0 —— 那种
     *   "静默无效"正是本项目最不想要的失败形态, 版本号把它变成一行明确
     *   的告警(见 display.c 的 QueryMiniVDDVersion)。                */
    uint16_t out_bx;                        /* VxD 回填: BIOS 返回 BX    */
    uint16_t out_cx;                        /* VxD 回填: BIOS 返回 CX    */
    uint16_t out_dx;                        /* VxD 回填: BIOS 返回 DX    */
    uint16_t out_si;                        /* VxD 回填: BIOS 返回 SI    */
    uint16_t out_di;                        /* VxD 回填: BIOS 返回 DI    */
    uint16_t out_es;                        /* VxD 回填: BIOS 返回 ES    */
} VBEEXECREQ;
#pragma pack(pop)
#define VBE_EXEC_STUB_OFF   768             /* 桩放在 1KB 缓冲的后 256 字节 */
#define VBE_EXEC_VEC        0x0063          /* 借用的空闲 IVT 向量。
                                           0x63=IBM 保留用户区, BIOS/DOS/
                                           Win9x VxD 均不占用; 原 0xC0 无法
                                           从本地排除 VMM 内部 V86 占用    */

/* --- stat 位(VBE_VXD_STAT_*)：VxD 里程碑取证, 排障时看 vbe9xlog ------- */
#define VBE_VXD_STAT_DOSBUF     0x0001  /* V86 缓冲经 DOS AH=48h 建立     */
#define VBE_VXD_STAT_B0000      0x0004  /* V86 缓冲走 B0000 物理暂存回退  */
#define VBE_VXD_STAT_HOOKED     0x0010  /* 分派表 5 项已挂上              */
#define VBE_VXD_STAT_IVT10      0x0020  /* IVT[10h] 目标已取证(InitMapLfb)*/
#define VBE_VXD_STAT_SELREG     0x0040  /* 屏选择子已登记给主 VDD         */
#define VBE_VXD_STAT_EXEC       0x0080  /* api_exec 至少跑过一次          */
#define VBE_VXD_STAT_MODELIST   0x0100  /* GET_MODELIST 至少跑过一次      */

/* --- LFB 映射上限 --------------------------------------------------------
 * drv 侧 DPMI 0800h 一次映射的字节数上限(也是模式过滤的上限):
 * 32MB 足以覆盖 2560x1600x32bpp(约 16MB)等所有 Win9x 时代实用模式；
 * 再大的模式会在 display.drv 的模式过滤中被剔除(见 display.c 的
 * IsModeOK)。                                                          */
#define VBE_LFB_MAP_CAP     (32UL*1024UL*1024UL)

#endif /* VBESHARE_H */
