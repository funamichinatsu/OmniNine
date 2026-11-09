;=============================================================================
;=============================================================================
; 【设计约束 —— 改动本文件之前请先读这一段】
;
; 一、运行上下文决定了"什么手段有用"
;   本驱动作为 mini-VDD 运行在 Windows VM 里, 不是跑在盒子(全屏 DOS)里。
;   因此:
;     · 向 VGA 端口(SR/GCR/CRTC/AR)写的值只落到 VDD 为 Windows 维护的影子状态,
;       既不会改变盒内硬件, 也不会被盒内程序看到。任何"靠写端口去修盒子"的做法都无效。
;     · 读同样的端口读到的也是影子, 不是盒子当前的真实状态。
;       (把影子读数当成盒子状态, 是过去若干净屏与误判的主要来源。)
;     · 需要真正影响盒子时, 唯一可靠的手段是把动作放到盒内执行 —— 见盒内 TSR(FIXSYS)。
;
; 二、必须保留、不能因为"没人调用"而删除的东西
;     · 钩子表里的每一条(dd 序号, OFFSET32 ...)：VMM 按序号调用;
;       缺一个序号等于声明"本驱动不支持该操作", 会改变 VMM 的行为。
;     · 所有 MiniVDD_* 入口：VMM 按注册表调用, 源码里看不到 call 是正常的。
;     · ACCESS_VGA_MEMORY_MODE / ACCESS_LINEAR_MEMORY_MODE 的实现。
;     · VgaM3Apply / VgaM3Crtc / VgaDotClock：文本态恢复链, 多处共用。
;     · VgaFontProv 及其四个全局：被多条活路径共享。
;     · Probe 块里有一部分槽位被汇编自身当状态机读回(cmp [Probe+...]),
;       它们不是日志, 不能按"没人打印"删除。
;
; 三、清理时使用过的判据(可直接复用)
;    一个过程可以整块删除, 当且仅当同时满足:
;      1) 全树没有 call 它;
;      2) 没有 OFFSET32/OFFSET/dd/dw 形式的取地址;
;      3) 过程体外没有代码引用它的名字(注释不算);
;      4) 它体内定义的全局标号与数据标号, 在体外也没有被引用。
;    第 4 条最容易被漏掉, 也最容易造成"删了才炸"。
;=============================================================================

;=============================================================================
; 【r412 C3】Vfc 一族(共 14 个过程)已整体删除。
;   它们全部服务于"往 DOS 盒的显存/寄存器写字模"：
;     · 寻址把物理地址当平坦指针用(r264 已判死), 从来写不到卡真正读字模的地方;
;     · 驱动跑在 Windows VM 上下文, 读写只落在 VDD 为 Windows 维护的影子;
;     · 盒内文本模式的字形本来就由视频 BIOS 提供, 全屏修复由盒内 TSR 负责。
;   整族一起删, 避免"拆掉一环、调用方还在"的半删除状态。
;=============================================================================
;
;  MINIVDD.ASM — Windows 9x 通用 VBE 迷你 VDD(Mini-VDD)
;
;  【本驱动在系统中的位置】
;  Win9x 显示子系统分层：
;     GDI.EXE → 16 位 DISPLAY.DRV → DIB Engine
;                                    ↓ (模式切换/全屏 DOS 虚拟化)
;                              VDD.VXD(主虚拟显示驱动)
;                                    ↓ (49 项分派表，卡相关部分)
;                              MINIVDD.VXD ←—— 本文件
;
;  【设计定位：通用 VBE 驱动，不针对任何具体显卡】
;  本驱动相当于 Win9x 下的"基本 VBE 显示驱动"(类似现代操作系统的
;  Basic Display / vesafb)：只用 VBE 标准特性，任何符合 VBE 规范的
;  显卡都能工作，目标是高质量的 SVGA/真彩色 2D 显示与绝对稳定
;  (包括运行 DOS 程序时不花屏、不黑屏)。
;
;  【实现策略：把一切硬件操作留给 VESA BIOS】
;  主 VDD 的 49 项分派表自带一套默认处理器，其回退路径就是
;  VESA BIOS(INT 10h 4F02h 设模式 / 4F05h 切 bank)——这条路径在
;  每一张 VBE 卡上都正确，且久经考验。因此：
;  * 我们只覆盖 5 个表项(见下)，其余 44 项保留主 VDD 默认；
;  * v1 中的 PCI 扫描、已知卡表(CardTable)、bank 端口编程、
;    I/O 端口陷阱已全部删除。那些是卡专用代码：对标准 VBE 卡毫无
;    必要；对 VBE 支持不完善的老卡(它们应使用各自的官方 Win9x
;    驱动)，叠加私有代码只会带来不稳定与不可预测行为。
;
;  【保留的 5 个覆盖项及存在理由】
;  1. GET_TOTAL_VRAM_SIZE(36) — 告诉主 VDD 真实显存总量
;     (VBE 4F00h 的 TotalMemory，由 display.drv 推送)。主 VDD 保存/
;     恢复高分辨率屏幕、计算合法 bank 范围都依赖它；缺省或错报
;     会直接花屏。
;  2. GET_BANK_SIZE(37) — 告诉主 VDD bank 粒度(VBE 4F01h 的
;     WinGranularity)。主 VDD 用 4F05h 切 bank 时按它把显存偏移
;     换算成 bank 号；粒度错误 = 切错 bank = 花屏。
;     这两项是"不花屏"的数据基础，也是通用 VBE 下唯一必须提供、
;     且纯 VBE 就能获得的信息。
;  3. DISPLAY_DRIVER_DISABLING(26) — 显示驱动卸载时作废旧状态，
;     防止过期几何参数在下次会话被误用。
;  4. VESA_CALL_POST_PROCESSING(47) — 纯记账：DOS 程序自己调
;     4F02h 换模式时同步 currentMode，不拦截任何硬件操作。
;  5. VESA_SUPPORT(41) —— 恒返回 NC(未处理)。理由:
;     DDK 原文: "CY returned means that the mini-VDD completely handled
;     the VESA call and that the VESA.COM or VESA BIOS should not be
;     called. NC returned means that the mini-VDD did not completely
;     handle the call and that the VESA.COM or VESA BIOS should be
;     called."
;     本驱动【完全不实现 ring 0 VESA】(那是给卡专用 mini-VDD 的可选
;     加速项), 所以唯一正确的回答是 NC —— 让 VDD 按它自己的策略去
;     处理(该虚拟化的虚拟化、该转真 BIOS 的转真 BIOS)。旧实现按
;     "发起 VM 是不是系统 VM"分别返回 NC/CY, 把 CY 误解成"请主 VDD
;     自己做默认虚拟化" —— 实际语义是"我已完全处理, 别再调 BIOS",
;     结果是 DOS VM(后台/全屏的 DOS 程序, 含各种 VESA 游戏与工具)
;     的 4Fxx 调用被无声吞掉、拿到垃圾返回值, 典型的黑屏/花屏来源。
;     系统 VM 侧本来就需要 NC(Windows 侧直通枚举要真 BIOS 数据),
;     现在两种 VM 都返回 NC, 语义统一且与 DDK 文档一致。
;
;  【DOS 应用稳定性论证】
;  窗口化/全屏 DOS 程序的 VESA 调用与端口访问由主 VDD 的默认路径
;  处理(BIOS 直通或虚拟化)。本驱动不安装任何端口陷阱、不拦截
;  任何 INT 10h、不向任何端口写一个字节 —— DOS 程序看到的硬件
;  行为与"没有安装本驱动"时完全一致，这是不花屏/不黑屏的最强保证。
;
;  【v1 → v2 变更摘要】
;  删除：PCI 配置空间枚举(DetectCard)、已知卡表(CardTable)、
;        bank 端口编程(ProgramBank)、3 个 I/O 陷阱处理函数、
;        陷阱装卸(SetupTraps/RemoveTraps/EnableTraps/DisableTraps)、
;        bank 影子寄存器、SET_DEVICE_FOCUS 焦点跟踪、
;        以及 20 个"只返回 NC"的冗余分派覆盖(默认表项已等价)。
;  共享结构同步瘦身(见 vbeshare.h 的 v2 说明)。
;
;  【v3-v5 → r76 变更摘要(帧缓冲映射责任换边)】
;  r53-r75 一直在尝试"由 VxD(ring 0)建一个帧缓冲选择子交给
;  display.drv 用": 先恒等映射, 再 _PageReserve/_PageCommitPhys 阶梯,
;  最后 _MapPhysToLinear + _BuildDescriptorDWORDs + _Allocate_GDT_Selector
;  建 GDT 选择子。实机结局是一句蓝屏 0D:
;      031F:00090B —— _fmemset_ 里的 "mov es,dx"(第一次写帧缓冲)
;  根因(vbeshare.h 文件头有完整说明):
;    ① 那个描述符的 DPL 由 VMM 按"保护模式应用的 RPL 位"填, ring 3
;       的 16 位显示驱动载入时 max(CPL,RPL) > DPL → #GP;
;    ② 就算描述符合法, 其基址是 VxD 系统区线性(>=0xC0000000),
;       是 supervisor 页, ring 3 一访问还是 #PF。
;  即【ring 0 造不出 ring 3 能用的帧缓冲指针】。
;  r76 起本 VxD 彻底退出映射业务:
;    · 删除全部 MM/描述符服务调用(_MapPhysToLinear / _PageReserve /
;      _PageCommitPhys / _PageDecommit / _PageAllocate /
;      _BuildDescriptorDWORDs / _Allocate_GDT_Selector)与 PCI BAR
;      自枚举(MiniVDD_FindLfb)、VM 事件重试(MiniVDD_IdleMap);
;    · 帧缓冲映射由 display.drv 在 ring 3 用 DPMI 0800h + DPMI LDT
;      选择子自建(在售参考驱动 boxv9x 的同款做法), 物理地址取自
;      VBE 4F01h 的 PhysBasePtr;
;    · 本 VxD 只做一件事: ApplyState 收到 drv 推来的屏选择子后,
;      代它调用 VDD_Register_Extra_Screen_Selector 登记给主 VDD
;      (DDK 明确要求由 mini-VDD 代登记: 主 VDD 需要知道显示驱动还
;      用了哪些屏选择子, 才能在窗口化 VGA 4 平面虚拟化时把 MemC
;      状态切回 Windows VM 需要的值 —— 不登记就可能"DOS 图形程序
;      退出后桌面花屏")。
;    · VESA_SUPPORT 改为【恒返回 NC(未处理)】: 见该函数头注。
;
;  【保留的 5 个覆盖项及存在理由】
;
;  【对最初骨架的主要修正】(v1 引入，仍然有效)
;  1. 原骨架用 VMMCall Get_VxD_Service_Table_Address 拿 VDD 服务表——
;     该服务不存在。正确做法是 VxDcall VDD_Get_Mini_Dispatch_Table，
;     它返回 EDI=分派表地址、ECX=表项数。
;  2. 原骨架自创了一套 IOCTL 功能码当作"mini-VDD 接口"。真实的
;     mini-VDD 接口是上述 49 项分派表；Win32 DeviceIoControl 走
;     W32_DEVICEIOCONTROL 控制消息 + DIOCParams，与分派表无关。
;
;  【构建】见 makefile(Win9x DDK: MASM 6.11+ 与 DDK 版 LINK)。
;
;=============================================================================

        .386p

;-----------------------------------------------------------------------------
; 包含文件
;   VMM.INC      —— Win9x DDK 提供：VMM 服务、控制消息、Client_Reg_Struc、
;                   DIOCParams、BeginProc/Control_Dispatch 等宏
;   VBESHARE.INC —— 本项目：共享结构镜像、VDD 服务表、分派表索引
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【目标系统版本 ①: 预定义 DDK_VERSION=400h】
; Declare_Virtual_Device 会把 DDK_VERSION 写进 DDB 的 SDK 版本字段
; (DDB_SDK_Version, 即"本 VxD 最低需要的 VMM 版本"); VMM 加载器据此
; 拒绝版本过高的 VxD, 报 "Not a valid device - possibly corrupt"(与
; 文件损坏同一句, 极具误导性)。Win98 DDK 的 VMM.INC 默认 40Ah(Win98),
; 在 Win95(4.00)上必被拒。这里预先定义为 400h(Win95): 若 VMM.INC 对
; DDK_VERSION 有 IFNDEF 保护, 它会让步给我们的取值; 再配合 INCLUDE
; 之后的二次赋值(见 DECLARE_VIRTUAL_DEVICE 上方), 两种写法都被覆盖。
; 低版本声明向上兼容: Win95/98/98SE/Me 均可加载。
;-----------------------------------------------------------------------------
IFNDEF DDK_VERSION
DDK_VERSION = 400h
ENDIF

        INCLUDE VMM.INC
        ; 尖括号形式只搜 INCLUDE 路径; 配 buildvxd.bat 的 INCLUDE=.;... 使
        ; 当前构建目录恒居首, 陈旧副本(如被人误拷进 C:\DDK\INC32 的
        ; VBESHARE.INC, r42 实锤其致版本号 0104)再无被采纳之路。引号形式
        ; 不用: MASM 6.11c 会把引号当文件名一部分(A1000 实锤)。
        INCLUDE <VBESHARE.INC>

;-----------------------------------------------------------------------------
; 保险：个别精简环境的 VMM.INC 可能缺 DIOCParams 定义
;-----------------------------------------------------------------------------
IFNDEF DIOCParams
DIOCParams STRUC
    Internal1           dd ?
    VMHandle            dd ?
    Internal2           dd ?
    dwIoControlCode     dd ?
    lpvInBuffer         dd ?
    cbInBuffer          dd ?
    lpvOutBuffer        dd ?
    cbOutBuffer         dd ?
    lpcbBytesReturned   dd ?
    lpoOverlapped       dd ?
    hDevice             dd ?
    tagProcess          dd ?
DIOCParams ENDS
ENDIF

;=============================================================================
; 虚拟设备描述块(DDB)
; 【版本号为何是 4.0】对应 VMM 4.0(Windows 95)；低于 4.0 的 VxD 在
;   Win95+ 上会被拒绝加载。
; 【设备 ID 为何必须非零】INT 2F/AX=1684h 以设备 ID 检索 VxD 的
;   V86/PM API 入口；ID=0(UNDEFINED_DEVICE_ID)的设备无法被找到。
; 【初始化顺序】UNDEFINED_INIT_ORDER 保证我们晚于 VDD 初始化——
;   Device_Init 时才能安全调用 VDD_Get_Mini_Dispatch_Table。
; 【V86/PM API】两个入口都要给出，INT 2F/1684h 会按调用方所处的
;   模式(V86 或保护模式)返回对应入口。
;-----------------------------------------------------------------------------
; 【目标系统版本 ②: 强制 DDB_SDK_Version = 4.00(Win95)】
; 必须放在 INCLUDE VMM.INC 之后、DECLARE_VIRTUAL_DEVICE 之前(宏展开时
; 才取值)。若你的 VMM.INC 是用 EQU 无条件定义 DDK_VERSION 的(不可重
; 定义), 本行会报 A2005 —— 那就复制 C:\DDK\INC32\VMM.INC 到
; D:\vga\ddk\VMM.INC, 把其中的 DDK_VERSION 值改为 400h, 并让 ML 的
; INCLUDE 路径里 D:\vga\ddk 排在 C:\DDK\INC32 之前。
;-----------------------------------------------------------------------------
DDK_VERSION = 400h

;=============================================================================
DECLARE_VIRTUAL_DEVICE MINIVDD, 4, 0, MiniVDD_Control, VBE_VXD_DEVICE_ID, \
                       UNDEFINED_INIT_ORDER, MiniVDD_V86_API, MiniVDD_PM_API

;=============================================================================
; 锁定数据段
; 【为何全部放锁定段】分派表处理函数与 API 随时可能被任何 VM 在任何
; 时机调用(包括分页敏感路径)，所在数据若被换出会造成致命页错误。
;=============================================================================
VxD_LOCKED_DATA_SEG

PUBLIC  VbeState
VbeState            VBESHAREDSTATE <>     ; 共享状态(全 0 = 尚未推送)

StateValid          db 0                  ; 非 0 = VbeState 内容有效
                    align 4
VesaCallCount       dd 0                  ; VESA_SUPPORT(41) 被主 VDD 调用的次数
                                          ; (排障: 判别钩子是否被调用/放行是否生效)

;--- r99: mini-VDD 分派调用追踪 -------------------------------------------
; 【为什么必须有】vbe9xlog 的每一行都是 ring 3 写的, 主 VDD 对我们的
; 49 个分派服务的调用在日志里【一个都看不见】。r98 之前"主 VDD 到底
; 问了我们什么"全靠推断。现在每个分派入口都先记一条 16 字节记录进
; 下面的环形缓冲, drv 在任意 ring-3 入口经 VBE_API_GET_TRACE(13) 整块
; 取回落盘, 于是"窗口化 DOS 时谁动了硬件/主 VDD 问了哪些服务"有据可查。
; 【全部放锁定段】分派处理函数随时可能被任何 VM 在任何时机调用。
TraceTotal          dd 0                  ; 累计写入条数(= 下一条的序号)
TracePtr            dd 0                  ; 环形写指针 0..VBE_TRACE_RECS-1
TraceCounts         dd VBE_TRACE_FUNCS dup (0)   ; 每功能累计调用次数
TraceEvt            dd 2 dup (0)             ; 【r100/r129】事件计数器块, 8 字节
                                        ;  (VBE_TR_EVT_*): +0 POST_VGA_TO_HIRES/
                                        ;  +1 PRE_HIRES_TO_VGA/+2 26 号/
                                        ;  +3 PRE_VGA_TO_HIRES/
                                        ;  +4【r129】HiResClockFix 动手次数/
                                        ;  +5【r129】HiResClockFix 挡下次数
                                        ;  drv 只比较"有没有变化", 不要求清零
                                        ;  【为什么必须显式写成 dd 2 dup(0)】
                                        ;  这一段【没有】任何填充字节: 紧跟其后的
                                        ;  TraceRecs 是 db 数组, MASM 不为 db 数组
                                        ;  做对齐。所以若仍是 `dd 0`(4 字节),
                                        ;  TraceEvt+4/+5 就正好落在 TraceRecs[0..1]
                                        ;  上, 会被环形记录【静默覆写】—— 计数随机
                                        ;  归零, 判读时看起来像"这条路从没跑过"。
                                        ;  显式多留 4 字节 = 计数器自己独占一块。
                                        ;  r129-marker: TraceEvt is dd 2 dup (0).
TraceRecs           db VBE_TRACE_RECS * VBE_TRACE_RECB dup (0)
;--- r101: 切屏状态机(4 字节, 经 blob 头 204..207 回传给 drv) -------------
; 【为什么需要它】"主 VDD 现在把硬件当成 VGA 还是 hires"是所有 hires 族
;  服务(35/1/41/48)唯一正确的判据, 而这件事只有主 VDD 的切屏时序知道:
;  PRE_HIRES_TO_VGA(4)/POST_HIRES_TO_VGA(5)/ACCESS_VGA_MEMORY_MODE(11)
;  ⇒ 硬件已被切成 VGA 通路(置 VBE_TR_ST_VGA);
;  POST_VGA_TO_HIRES(7)/ACCESS_LINEAR_MEMORY_MODE(12)/REGISTER(0)
;  ⇒ 回到 Windows 的线性高分辨率(清 VBE_TR_ST_VGA)。
;  三个字节必须【连续】—— api_gettrace 用一个 mov eax/dword 整块带走。
TraceState          db 0                  ; VBE_TR_ST_* 位(见 vbeshare.inc)
TraceI10Cnt         db 0                  ; PRE_INT_10_MODE_SET(48) 次数
TraceI10Ax          dw 0                  ; 48 号最近请求的模式号(AX)
Ctr35              dw 0                  ; [r412] 35 号采样计数
WinVMHandle         dd 0                  ; Windows VM 句柄(首次调用者锁定;
                                          ;  drv 的 PM API 调用就在 Windows
                                          ;  VM 上下文里, 故用它辨识"谁在
                                          ;  发 VESA 调用")
;--- r111: 回调窗口标记 + 回调期 VM 自检 -----------------------------------
; 【为什么需要】r111 让显示驱动在主 VDD 的 ResetHiResMode 回调里【直接】
;   做一次 4F02(微软文档要求的"官方恢复路径")。回调可能被切在任意 VM 的
;   上下文里, 而 api_exec 的嵌套 V86 执行是"在【当前 VM】里跑一次 INT 10h"
;   —— 若当前 VM 不是 Windows VM, 那等于在别人的 VM 里设我们的模式:
;   r99 那三次蓝屏/冻结(全部停在回调里那次嵌套 4F02)最可疑的就是这一点。
;   于是: 回调期加一道自检 —— 当前 VM != Windows VM 就【拒绝】并把次数
;   记账, 由 ring 3 侧走回落路径(而不是撞死)。
InCbFlag            db 0                  ; 1 = 显示驱动正在主 VDD 回调里
CbBiosRefused       dd 0                  ; 回调期因"VM 不是 Windows VM"被拒次数
;--- r116/r117: VBE_DISPI(1CEh/1CFh)端口虚拟化的 4 个状态量 -----------------
; 【为什么放锁定段】I/O handler 会被任何 VM 在任何时刻调用, 它读这几个
;   字节; 换出就是致命页错误。策略字节在 VbeState.VBS_policyDispi(43)。
; 【r117 与 r116 的差别 —— 上一轮这条路【一次都没生效】】
;   ./116 六份日志里 `TRC st decode ...` 的 dispi 位恒 0, 说明
;   DispiInstalled 一直是 0。原因在 Device_init 的调用约定:
;   VMM 的 Install_IO_Handler / Enable_Global_Trapping /
;   Disable_Global_Trapping 三个服务的端口号都在【EDX】(DDK 原文:
;   "mov esi, IOCallback / mov edx, Port / VMMCall Install_IO_Handler"),
;   r116 写成了 EAX —— 于是端口号取的是注册循环结束时残留在 EDX 里的
;   值(最后一个 RegList 索引 = 48 = 30h), 第一次调用把 handler 装到了
;   端口 30h 上, 第二次调用因为"该端口已有一个 handler"而按规定返回
;   CY ⇒ 直接跳到 di_dispi_skip。两个端口一个都没装上, 而且 30h 上还
;   白留了一个 handler + 开着的全局陷阱。r117 全部改成 EDX, 并且
;   每个端口装成功后立刻 Disable_Global_Trapping(Install_IO_Handler
;   会顺手把陷阱打开, 参考实现 boxvmini.asm 逐字如此)。
;
; DispiInstalled(bit0=1CEh, bit1=1CFh): 两个端口【都】装成才是 3;
;   判据一律用 "== 3"。为什么不是"装一个也能用": 索引端口(1CEh)被吃掉
;   而数据端口(1CFh)放行时, DOS 盒仍能往【当前索引】写值 —— 那是最坏的
;   组合(挡住了改索引, 却没挡住改数据)。宁可整条路退回 r115 行为。
; DispiTrapOn: 只反映"最近一次 MiniVDD_DispiTrapOn/Off 的动作结果",
;   供日志判读(与 TraceState 的 bit4 同步)。决策不依赖它 —— 吃掉还是
;   放行完全看 VDD_Get_VM_Info 报的 CRTC 所有者。
; DispiActs: 真正执行过 Enable/Disable_Global_Trapping 的次数(模 256)。
;   它是"钩子确实跑到过"的直接证据(策略关/没装成时它不涨)。
; DispiErrs: Install_IO_Handler 的失败次数(模 256)。
; 【这四个字节必须连续】api_gettrace 用一个 mov eax/dword 整块搬走,
;   顺序与 vbeshare.inc 的 VBE_TR_ST2_INST/TRAP/ACT/ERR 一一对应。
DispiStat2          label byte            ; r117: 4 字节记账块的起点
DispiInstalled      db 0                  ; +0 bit0=1CEh 装成, bit1=1CFh 装成
DispiTrapOn         db 0                  ; +1 最近一次开关的结果(0/1)
DispiActs           db 0                  ; +2 Global_Trapping 真正执行次数
DispiErrs           db 0                  ; +3 Install_IO_Handler 失败次数
;--- 【r120】ring 0 物理 I/O 读回的 DISPI 寄存器(24 字节, 12 个字) ----------
; 顺序(必须与 vbeshare.h 的 VBE_TR_DispiPhys 块逐字节对应):
;   +0  ID       +2  XRES     +4  YRES    +6  BPP
;   +8  ENABLE   +10 VIRT_WIDTH
;   +12 DispiPhysN(模 256)  +13 DispiPhysOk(1=回读命中)
;   +14 DispiPhysErrs(模 256)  +15 保留
; 这一段通过 api_gettrace 搬到 blob 的 offset 216..239(VBE_TR_DispiPhys),
; 见 MiniVDD_DispiPhysRead 的长注与 api_gettrace 里那段偏移注释。
; DispiPhysN 是"到底跑没跑"的证据: 0 = 一次都没读(策略关/旧 drv), 非 0
; = 读数有效。
DispiPhysVal        dw 12 dup (0)         ; 六个寄存器 + 记账 + 保留
; DispiPhysN / DispiPhysOk / DispiPhysErrs 必须物理连续且紧跟在
; DispiPhysVal 之后 —— api_gettrace 用两条 mov 把 [DispiPhysVal+12..] 整块
; 搬进 blob 的 +12..+15。
DispiPhysN          db 0                  ; 物理读次数(模 256)
DispiPhysOk         db 0                  ; 1 = 回读命中(端口确实活着)
DispiPhysErrs       db 0                  ; 物理读失败次数(模 256)
;--- r143: 文本取指寄存器的两拍取证(经 blob 212..215 回传给 ring 3) ------------------
; 【为什么到这一轮才做】./142 的日志给出两条互相矛盾的事实:
;   ① `TRC st decode vgafix/dispi/dispiEat/dispiPass/vtfBlock=000B` —— bit3
;      置起, 即 r114 的 文本态修补 在那一场会话里【确实执行过】;
;   ② 用户同一场的报告与 r141 逐字相同("全屏花屏, 等一段时间无用,
;      必须 mode co80")。
;   ⇒ "写了"不等于"写住了"。要把它分成三件事, 只有在 ring 0 读回硬件:
;      S1 = 我们这一手写完、还没链给主 VDD 原处理函数时的读数;
;      S2 = 盒子持屏期间(若干次 35 号之后)的读数。
;   S1 与 S2 一比, "没写进去"和"写进去又被覆盖"立刻分开 —— 前提是这两个
;   读数必须能被 ring 3 看到, 而整场全屏 DOS 期间一个 DDI 都进不来
;   (./142: 心跳整整停了 116 秒), 所以只能由 VxD 存在 blob 里。
; 【为什么存 SR04 + GCR06】./110 t3 的截图取证把故障定成"文本模式的三种
;   取指(字符/属性/字模)全部塌到 plane 0"。决定这件事的两组位就是
;   SR04(bit2 = odd/even 关闭, bit3 = chain4)与 GCR06(bit3-2 = 显存映射,
;   bit1 = odd/even, bit0 = 文本/图形)。文本态修补 写的正是这两个值
;   (SR04=02h, GCR06=0Eh); 它们若真的留在硬件里, 故障就不在取指层。
; 【有效性判据】4 号(PRE_HIRES_TO_VGA)一进来就把这 4 个字节清零 ——
;   全 0 = "这一场还没有样本", 而硬件上 SR04/GCR06 同时为 0 是不可能的
;   (GCR06=0 意味着显存映射到 A0000 且是文本模式, 与任何真实模式都不符)。
; 【位置】必须 4 字节连续: api_gettrace 用一个 mov eax/dword 整块搬到
;   blob 的 212..215(VBE_TR_VgaS)。这一段原本就是保留区, 结构不变。
; --- 【r144】盒子持屏期取证与武装期 ---------------------------------
; FboxWord 四个字节必须连续: api_gettrace 用一个
;   mov eax/dword 把它整块搬到 blob 212..215。
;   字节顺序必须与 vbeshare.h/.inc 的
;   VBE_TR_FBOX_I10MAP/N35/N48/DISARM 一致。
FboxWord            db 4 dup(0)            ; +0 I10MAP / +1 N35 / +2 N48 / +3 DISARM
; FboxSnap 同样必须连续: 整块搬到 blob 232..247(r146 起 16 字节)。
;   【r146 为什么从 4 字节长到 16】见 vbeshare.h 的 VBE_TR_FboxSnap 注:
;   原四个寄存器只能回答"硬件在谁的模式里", 而 ./144 的截图量出的是
;   "同一块 16bpp 线性显存被按 4 平面/奇偶解释"(2~4 像素周期竖条纹 +
;   调色板颜色 + 只剩左半并横向拉伸)。决定这件事的位在
;   SR02/SR03/SR04 与 GCR03/GCR04/GCR05 里, 原来一个都没读。
;   块内布局与 blob 的 VBE_TR_FS_* 完全一致(所以 int 的直接搬运)。
FboxSnapVal         db 16 dup(0)           ; 见 VBE_TR_FS_* 的 13 个偏移 + 相位
; --- 【r146】hi-res 侧的"我们那一份模式/寻址字节" -----------------------
;   【DDK 的分工】主 VDD 保存/恢复的是【标准 VGA】寄存器; "超出标准 VGA 的
;   那部分"由 mini-VDD 自己负责(About Virtual Display Device 的
;   CRTC owner / MemC owner 一节: 窗口里的 DOS VM 时, "屏幕看起来还是
;   Windows 的 hi-res 打包像素模式, 而 CPU 看到的显存是 4 平面 VGA" ——
;   让这两件事同时成立、以及在回到 Windows 时把显存通路换回来, 都是
;   mini-VDD 的活)。我们这套是【线性打包像素】的 VBE 模式, 所以"超出标准
;   VGA"的那几位就是显存寻址/平面/带宽那几位。
;   【./145 的现场读数把缺口指出来了】回到 Windows 之后
;     SNAP GCR06=0005(对, 我们的值) 而 SNAP SR01=0003(我们的模式是 0001)
;   ⇒ "图形位"补回来了(r129 的 HiResClockFix 只 OR bit0 那一族),
;     而寻址/平面/带宽那几位没有任何人管。
;   【本轮的机制】4 号(PRE_HIRES_TO_VGA, 硬件还是我们的)读一份;
;   7 号(POST_VGA_TO_HIRES, 主 VDD 刚把屏还回来)写回一份。
;   一次快照只回放一次(用完 HiresValid 清 0) ⇒ 用户换分辨率这类
;   合法变化不会被旧值覆盖: 下一次 4 号会重新采一份。
;   【判据 r147 修正】只有三个位同时成立才收下这份快照:
;     SR01.bit0=1 八点字符时钟 / GCR06.bit0=1 图形模式(不是文本取指) /
;     CR17.bit6=1 字寻址。r146 这里写的是 SR04.bit3(chain4), 而本机我们
;     的线性打包模式 SR04 = 06h(bit3=0) ⇒ 四轮里一个样本都没收下,
;     ./146 的 savedN/restoredN 恒为 0000 就是物证。不成立就不采,
;     于是"从来没在好状态里采到过"在日志里表现为 savedN=0。
HiresValid          db 0                   ; 1 = HiresSav 里有一份可用快照
HiresSavedN         db 0                   ; 保存次数(模 256, 无条件通道可读)
HiresRestN          db 0                   ; 回放次数(模 256)
HiresLastDiff       db 0                   ; 回放前"与现场不同的字节数"(10 个里)
HiresSpare          db 4 dup(0)            ; 上面四个字节必须连续: 整块搬到 248
; 【r164】字模守卫的两格私有现场:
;   FontRef = 本次开机第一次成功重装之后读回的"字模指纹"(平面 2, 字符 41h 那
;   一格的第 2 与第 6 字节)。0 = 还没有参照, 于是第一次一定装。
;   指纹由我们自己定标 —— 不去猜 ROM 字模长什么样(避免把"我以为的字模"写成判据)。
FontRef             dw 0
; 【r165】FontExp = 这一笔要拷进去的【源】字节(源里字符 41h 的第 2 与第 6 字节,
;   源是连续 16 字节/格 ⇒ 偏移 410h+2 与 410h+6)。它是"装完之后应当读回什么"
;   的正确答案, 与 HS_FONTBAD 一起把 r164 分不开的两种情况分开:
;     读回 != FontExp  ⇒ 我们的写被挡回去/盖掉了(HS_FONTBAD 计数);
;     读回 == FontExp 而 FontExp 不是 ROM 字模 ⇒ INT 43h 向量指错了地方。
FontExp             dw 0
FontN               db 0                  ; 本场会话已重装次数(r165 预算 = 16)
; 【r168】字模整块自证用到的三个变量。
;   【为什么还要一个 FontSrc】./167 的判决把最后一个没被证过的环节指到了这里:
;   VfcLoad 的 4KB 拷贝【只回读一格】(字符 41h 的第 2/6 字节)。只要那一格碰巧
;   落在拷贝已经写完的位置, 整块就算"成功"—— ./167 最后一次读到 C610(标准
;   8x16 ROM 字模 'A' 的第 2/6 行), 于是 fontbad 不再涨, 而画面照样花屏。一个
;   只看 8192 字节里 2 个字节的仪器, 无法把"整块到位"和"拷贝在 2000 字节处被
;   打断"分开。所以 r168 在拷贝前把源地址存下来(FontSrc), 拷完把【整块】
;   4KB 逐 dword 比一遍, 数出不一致的 dword 数(FontBadW, 饱和 63)。
FontSrc             dd 0                  ; r168: 字模源的线性地址(拷贝与回读共用)
FontBadW            db 0                  ; r168: 整块回读不一致的 dword 数(0..63 饱和)
; 【r177】字模两条路各自的预算与结果:
;   FontBN = 本场"BIOS 装字模"(INT 10h AH=11h AL=04h/14h)用过几次, 4 号清 0;
;   BiosFontStat = 上一次的结果(0 没执行 / 1 执行了 / 2 没敢执行)。
FontBN              db 0
BiosFontStat        db 0
; 【r178】嵌套 V86 INT 10h 的参数块(VgaBiosInt10 用, 不进 blob)
BcAx                dw 0
BcBx                dw 0
BcCx                dw 0
BcDx                dw 0
BcEs                dw 0
BcRet               dw 0                  ; BIOS 回来的 AX
BcOk                db 0                  ; 1 = 执行了 / 2 = IVT[10h] 为 0
BiosFontReq         db 0                  ; 本次要发的 AH=11h 子功能号(04h/14h)
; 【r179】平面 2 的"窗口"—— 本机到底从哪个地址、用哪一档显存映射能读写它。
;
;   【为什么这一轮才想起来问它 —— ./178 的三条读数】
;     fntreg  = 0407  ⇒ 我们写的 SR04=07 / GCR06=04 被卡【锁存了】(寄存器无罪);
;     fontp0  = 1818 且与 fbox fontfp=1818 完全相同 ⇒ 读平面选择不参与;
;     fontexp = 10C6  = 0xC610 = 标准 8x16 ROM 字模 'A' 的第 2/6 行 ⇒ 源是好的。
;   三条合起来把"寄存器没写进去""源字模不对"都排除掉了, 只剩下一个变量:
;   【A0000 + GCR06=04h(A0000-AFFFF 64K)】这个窗口本身。r176..r178 一直只
;   用这一个 —— 而 GCR06 的 bit3-2 一共有四种显存映射(见本包
;   vga-register-reference.md 的 GCR06 表), 另外三种当时从来没试过(本版把 B0000 单色窗口也补进候选, 见 VfcSetCand 的五档)。
;
;   【做法】本场第一次装字模之前, 按 VfcSetCand 的 1..4 顺序逐个试:
;   摆好 get_font_access() 那一组寄存器 → 在该窗口的【字符 FFh 的第 16..23 行】
;   上写 8 个字节的记号 → 立刻读回。字符发生成器只读每格的前 16 行, 后 16 行
;   永不显示 ⇒ 记号不会污染画面, 也不会毁掉任何一个可见字形。
;   第一个读回相等的候选就是本机可用的窗口; 四个都不行就退回第一档
;   (行为与 r176..r178 完全相同, 不会比上一版更差)。
; 【r183】字模窗口 —— 三个字节决定"CPU 从哪个地址、按哪一档映射看到平面 2"。
;
;   默认值 = 字模阶梯的第 1 档: GCR06=0Ch(B8000-BFFFF 32K, Chain O/E 关,
;   字符模式) + SR04=06h(顺序寻址 + >64K) + 基址 0B8000h —— 也就是
;   【盒子自己那扇文本窗口】。
;   【为什么默认它】./182 的 `nbnt=01FA`(2000/2000 格规范)证明这扇窗口的
;   字符/属性一侧是真在读写的; 而 A0000 那一侧(`fontp0=1818` 与
;   `fontfp` 逐字节相同、`fontbadb=007F` 整块 63 个 dword 全不一致)连
;   "读到的是哪个平面"都分不出来。二十几轮里 A0000 一直是唯一入口,
;   把"装不上"记在字模头上之前, 先换到能用的那扇窗。
;   【谁写进来的】VgaFontProv 的四档阶梯: 赢的那一档把三个值留在这里,
;   之后 VfcFpRead / 平面 2 字模拷贝 / VfcCanary 全都用它。
;   【GCR06 的两位纪律】(FreeVGA graphreg.htm, 见本包 vga-register-reference.md)
;     bit0 Alpha Dis. = 1 ⇒ 关掉字符发生器锁存, 平面 2 立刻不再是字模 ⇒ 必须 0;
;     bit1 Chain O/E  = 1 ⇒ A0 被换成高位, 顺序写会被送到平面 1/3(SR02=04 只开平面 2, 那一半就丢了) ⇒ 必须 0;
;     bits3-2 = 11 ⇒ B8000-BFFFF 32K(文本态的窗口), 与 0Eh 的差别只有 bit1。
;   【SR04 的两位纪律】bit2=1 顺序寻址(bit2=0 时偶数地址走平面 0/2, 复制会散),
;     bit1=1 (>64K) 是字符映射选择生效的前提。
; r412 C7c: 旧的窗口 GCR06 影子 removed -- no references left after C7b.
                                               ;   bit1=0 链位关, bit0=0 仍是字符模式。
                                               ;   r176..r184 这里是 00Ch(B8000 32K) ——
                                               ;   而那扇窗是【字符/属性】窗, 往它写平面 2
                                               ;   会被链位摊成 2:1, 见 R185_DELIVERY.md。
; r412 C7c: 旧的窗口 SR04 影子 removed -- no references left after C7b.
                                               ;   (get_font_access() 原值; r184 这里是 006h)
HexDigits           db '0123456789ABCDEF'
Sr3Saved            db 0
CsAOff             dd 0          ; 【r412】A 字符集在平面 2 的基均(字节)
CsBOff             dd 0          ; 【r412】B 字符集在平面 2 的基均(字节)
; r412 C7c: 旧的 GCR 暂存 removed -- no references left after C7b.
; r412 C7c: 旧的 SR04 暂存 removed -- no references left after C7b.
; r412 C7c: 旧的基址暂存 removed -- no references left after C7b.
; r412 C7c: 旧的字模窗口基址变量 removed -- no references left after C7b.
;   【r183】记号的两个结果字节:
;     旧的可用性即时值 = 本次调用过没过(每次进 VfcCanary 先清 0);
;     旧的可用性汇总值  = 本场【至少有一档】过过记号 —— 它是 fontst 里 4 与 6 的唯一分界:
;                 4 = 记号过了但整块 4KB 装不上(取指一侧的问题);
;                 6 = 连记号都过不了(回读仪器是瞎的, 本场字模读数不许当结论)。
; 【r184】收尾这一笔用的两个量。
;   旧的尾部计数  = 本拍改写用掉的遍数(0 = 第一遍就读回成立);
;   旧的字模窗口期望表   = 读回应该得到的七个字节(SR02 SR04 GCR04 GCR05 GCR06 CR0C CR0D)。
;   与 VfcTextGroup 写进去的值逐字节相同 —— 两边必须一起改。
; r412 C7c: 旧的尾部计数 removed -- no references left after C7b.
; r412 C7c: 旧的字模窗口期望表 removed -- no references left after C7b.
; r412 C7c: 旧的可用性即时值 removed -- no references left after C7b.
VfcCanOk            db 0
; 【r183】屏幕标记的字符串(见 VgaMark): 10 个字符, 写在第 24 行第 70..79 列。
MarkText            db 'vbe9x r447'
MarkLen             EQU 10
;   【r180 把候选表【删了】—— 它是 r179 那次 06 蓝屏的根】
;
;     r179 在这里放了一张"每项 8 字节"的表(`db 004h` + `dd 0000A0000h` ...),
;     而 MASM 的 `dd` 【不会】自动对齐到 +4:`db` 之后它就从 +1 开始。
;     ./179 回传的 24,922 字节 minivdd.vxd 里, 文件偏移 0x1FC5 上就是
;
;         04 00 00 0A 00 | 00 00 00 0A 00 | 08 00 00 0B 00 | 0C 00 80 0B 00
;
;     —— 步长 5, 而 r179 的探测代码按 `add esi, 8` 走表、用 `[esi + 4]` 取
;     基址。于是第 1 档的基址读成 `[tab+4] = 00000000h`, 接下来每一环都"成功":
;
;       ① 自证标记写在自己数据段的 1FF0h 上(不是显存), 回读当然相等
;          ⇒ 判成"第 1 档站得住", 窗口基址 = 0;
;       ② VfcFpRead 从 0822h 读音符指纹(读到的是我们自己的别的变量);
;       ③ VfcLoad 把 256 格 x 32 字节 = 4KB 的 ROM 字模从 0000h 开始写
;          ⇒ 抄掉 VxD 自己那一段(LE 对象 1 = 0x471D 字节, 代码在里面)的
;          头 4KB —— 记录器、钩子派发全在那 4KB 里, 所以日志恰好在那一刻
;          停死, 而 CPU 随后在残骸里取到非法指令(06)。
;
;     这台机器上"第一次进全屏"就是钩子 5(VgaFontProv 的调用点), 于是症状
;     正好是"切全屏时蓝屏"。
;
;   【现在的做法】表一个都不留: 第几档 → (GCR06, SR04, 基址) 全部是 VfcSetCand
;   里的立即数, 不再有任何"数据布局"的假设; 再加 VfcBaseCheck —— 任何一次
;   碰显存之前先确认基址 4K 对齐且落在 0A0000h..0B8000h, 不合法就整条路
;   放弃(fontwin 落 0FFh, 一个字节都不写)。
; 【r168】Bios3N —— 本场会话用过"让卡自己的 BIOS 重设 mode 3"的次数。
;   只在【我们自己的 4KB 拷贝回读不成立】时才会用掉一次(见 VfcLoad 尾部与
;   BIOS 状态回放 的长注): 判据是硬件自己回答的, 不是我们推断的。
Bios3N              db 0
Bios3Stat           db 0                  ; r168: 上一次"让 BIOS 重设"的结果(0..3)
; 【r412】会话中期的那一枪(见 模式设置插入 的头注)的三个私有字节。
;   Bios3SN    = 本场已经开过枪(0/1), 4 号清零 ⇒ 每进一次全屏最多一次;
;   Bios3SStat = 结果: 0 没执行 / 1 执行且 AH=0 / 2 没有 IVT[10h] / 3 执行但 AH!=0;
;   Bios3Busy  = 我们正处在嵌套 V86 设模式里(1), 用来挡住 17/19/21 号的重入。
Bios3SN             db 0
Bios3SStat          db 0
Bios3Busy           db 0
; 【r412】字模块探测 的总开关: 1 = 把字符 20h 写成实心块(对照实验), 0 = 不写。
;   它是"我们的字模到底有没有落到显示器取指那一块"的唯一判定实验, 做完就应当关掉。
FontBlockOn         db 1
FontBlockN          db 0                  ; 本场已经写过一次(4 号清零)
; 【r412】把字模搬进 4 号 8K 段的那个实验(见 字模银行探测)。
FontBankOn          db 1
FontBankN           db 0
ApertureProbe       db 1
ApertureN           db 0
VbeWinN             db 0
VbeWinOk            db 0
; 【r171】PRE 那一枪(见 进文本模式前的预置)的三个私有字节。
;   【为什么要在 4 号再开一枪, 而不是沿用 r170 的 5 号】DDK《PRE_HIRES_TO_VGA》
;   的六步时序写得很清楚: 第 5 步"save some data from the BIOS data area of the
;   VM ... including cursor position, BIOS mode", 第 6 步"executes an Interrupt
;   10h, function 0 mode change", 第 7 步"saves/restores the BIOS data area ...
;   calls POST_HIRES_TO_VGA".  也就是说【BIOS 数据区里那个模式字节在 5 号之后
;   会被主 VDD 按第 5 步存下的值写回去】。r170 的那一枪打在 5 号: 硬件被我们
;   设成 mode 3 了, 但盒子自己的 BDA 模式字节随后被写回 72h ⇒ 硬件说"我是
;   80x25 彩色文本", 盒子说"我是 72h(DBCS 文本)"。两边不一致时, 控制台按
;   72h 的语法画屏, 而卡按 mode 3 的取指出字模 —— 这正是 ./170 的形态:
;   字模读回 C610(我们装对了)、寄存器是标准 mode 3、画面却仍是满屏实心块。
;   ⇒ 正确的时机是【第 5 步存 BDA 之前】, 也就是 4 号(PRE_HIRES_TO_VGA)。
;   那时第 3 步已经关掉 3B0h-3DFh 的陷阱(我们的 out/BIOS 调用落真硬件), 而且
;   DDK 原文把"把屏带出 hi-res"这件事明确划给这一拍, 连 Exec_Int 的用法
;   (Get_Cur_VM_Handle + EBP = 客户端寄存器)都在同一页末尾写出来了。
;   嵌套执行里的 INT 10h 跑在【盒子 VM 的上下文】里, 所以 BIOS 写 BDA 时写的
;   是盒子的那一份 ⇒ 第 5 步存到的就是 03h, 第 6 步拿到的也是 03h, 第 7 步
;   写回去的仍然是 03h。开关前后自洽, 与用户手打 `mode co80` 的状态等同。
PreM3N              db 0                  ; 本场已开火次数(4 号清零, 预算 1)
PreM3Busy           db 0                  ; 重入闸门: 嵌套执行期间置 1
; 【r171】PreM3Stat 直接用 blob 里那一格 VBE_TR_FBOX_I10MAP(r170 起它是空的:
;   48 号改写已经撤掉)。位图:
;     bit0..2 = 状态: 0 没跑 / 1 执行且 BIOS 回 AH=0 / 2 IVT[10h] 为 0 /
;                    3 执行了但 AH!=0 / 4 盒子的模式不是 80 列文本(不动手)
;     bit3    = 盒子的模式是 72h(远东 DBCS 文本)
;     bit4    = 盒子的模式是 73h
;     bit5    = 盒子的模式是 02h/03h/07h(VGA 文本)
;   判读: bit3 与 (bit0..2)==1 同时出现 = "盒子确实在 72h 里, 而我们已经
;   把它换成了 mode 3, 并且 BIOS 认了"。那是本轮假说的直接物证。
; 【r150】HiresSpare 那四个字节的【VxD 侧】名字。
;   【为什么必须另起名字 —— 这一步差点写坏 VxD 的数据区】blob 里这四个字节
;   落在 VBE_TR_Hires+4..+7, 所以 ring 3 侧的下标是 4/5/6(vbeshare.h 的
;   VBE_TR_HR_FONTN/FONTBAD/FONTVER); 而 VxD 侧它们是【相对 HiresSpare 的】
;   0/1/2。两者数值不同, 混用就是一次静默地写到 HiresSpare 之后 4 个字节
;   (r150 的静态审计抓到的正是这一处)。名字刻意不一样, 免得下一轮再混。
; 【r164】这三个字节换了含义(r150 的 72h/73h 分类问题已经被 ./150..163 回答完毕:
;   武装期里盒子请求的是 03h, 72h/73h 一律发生在武装之前)。新含义 = 字模守卫:
HS_FONTN            EQU 0                   ; → blob 252 重装字模的次数(模 256)
HS_FONTBAD          EQU 1                   ; → blob 253 【r165】"读回 != 拷进去的
                                            ;   源字节"的次数 = 我们的写被挡回去
                                            ;   或就地被盖。r164 的旧含义(每次
                                            ;   装载 +1)与 HS_FONTN 恒等 —— 一个
                                            ;   永远不可能失败的仪器, ./164 的
                                            ;   fontbad=3 就是它的算术产物。
HS_FONTVER          EQU 2                   ; → blob 254..255 最后一次读回的字模指纹
HiresSav            db 16 dup(0)           ; 块内布局同 VBE_TR_FS_*
VgaFixArm           db 0                  ; 1 = 全屏盒子持屏期(4 号置 / 7 号清)
;--- 【r182】全屏文本页的"内容" -------------------------------------------
;   【./181 给出了 r181 缺的那个读数】五次进全屏, 每一次进门读到的都是
;   `PH: probe nbnt==001D`: 字符字节为 00h 的格数 = 0, 属性字节为 00h 的格数
;   = 232; 而 `PH: probe nxna` 与它【逐字节相同】(我们那 4KB 字模拷贝一个
;   字节都没碰平面 0/1)。五次一模一样、一整场会话不变 —— 一页真正被写过的
;   文本屏不会这样。4000 字节里没有一个 00h 字符、却有 232 个属性字节是 0,
;   那是 Windows 桌面那一屏的残留(16bpp 的像素按 (字符,属性) 重新读出来的
;   样子): ⇒ 进全屏时【没有任何人在清页】。
;   【谁该清】DDK《PRE_HIRES_TO_VGA (Function 4)》第 6 步: 主 VDD 执行
;   "an Interrupt 10h, function 0 mode change"。一次真正的 BIOS mode 3 做三件
;   事 —— 写寄存器、装 8x16 字模、把整页清成 (20h,07h)。这台机器上前两件
;   ./169 与 ./180 已经证过没人做(r164 起我们自己装字模), 第三件由本轮补上。
;   【为什么由我们清、清在哪一拍】5 号(POST_HIRES_TO_VGA)在主 VDD 那句
;   INT 10h 之后、盒子开始画之前 —— 一次文本模式设置在硬件上的语义就是
;   "这一屏从空格开始"。而 ./181 的 nbnt == nxna 恒等还说明这一拍里没有
;   别人在写页面, 所以清页不会冲掉任何人的成果。
;   【闸门 = "它已经是一页真文本了吗"】只有三条同时成立才【不】清:
;     ① 过半格子(>= 1000/2000)就是 (20h,07h);  ② 字符种类数 <= 180;
;     ③ 属性种类数 <= 40。 一页 DOS 文本屏三条都成立 ⇒ 已经有人替我们做过
;   这件事(盒子自己画完 / 上一拍的清页活了下来), 一个字节都不动; 桌面像素
;   残留三条全不成立(它那一页里没有一样东西会重复一千次)⇒ 清。
;   r181 的闸门用的是"00h 格数", ./181 证明它对像素残留不敏感(nbnt=001D
;   落在闸门外面, 清页一次都没发生) —— 判据换了, 不是把闸门拆了。
;   【预算】按拍, 不设总量: 每一次进全屏都是新的一屏。
PgStart             dw 0                    ; 进门时 CR0D:CR0C(显示起始地址)
PgN                 db 0                    ; 1 = PgSave 里有一份可用快照
PgCan               dw 0                    ; 最近一次清点: 规范格数(字符 20h 且属性 07h)
PgChr               dw 0                    ;               字符字节的种类数(0..256)
PgAtt               dw 0                    ;               属性字节的种类数(0..256)
PgCanE              dw 0                    ; 进门那一拍: 规范格数
PgChrE              dw 0                    ;             字符种类数
PgAttE              dw 0                    ;             属性种类数
PgDif               dw 0                    ; 快照与页面的差异字节数(0..4000)
PgChrM              db 32 dup(0)            ; 字符种类位图(256 位, 每一种一个 bit)
PgAttM              db 32 dup(0)            ; 属性种类位图(紧挨着上一张: 清两张位图
                                            ;   正好是一次 rep stosd 打 16 个 dword)
PgSave              db 4000 dup(0)          ; 80x25 的交错页(字符, 属性, ...)
;   【r181 借用 r152 那五个查位】VBE_TR_PR_NB/NT/NX/NA/CELLS 原来记的是
;   显存逐格采样 的"打印格数"(见它第 7 段, 那一段自己写着"r152 的老读数"),
;   那一组从来没被当过证据。r181 把它们改成了页面读数; r182 再改一次含义
;   (r181 那两个"00h 格数"已经被 ./181 读完了, 结论是它们【分不出】
;   "文本页"和"看着像文本的像素残留" —— 像素残留里 00h 也不多):
;     `PH: probe nbnt=` = 进门那一拍: 高字节 = 字符种类数, 低字节 = 规范格数/8
;     `PH: probe nxna=` = 高字节 = 清页后字模指纹的【低字节】(判决看 bit6),
;                         低字节 = 清页后仍不规范的格数/8(0 = 写进去并读到了)
;     `PH: probe cells=` = 判定位图, 八位全用:
;              bit0 = 本拍清过页
;              bit1 = 判据说"这一页已经是真文本" ⇒ 没清(见 VgaPageJudge)
;              bit2 = 快照已取
;              bit3 = 清页后读回自证成立(2000 格全部是 (20h,07h))
;              bit4 = 进门那一页本来就已经全规范(2000 格)
;              bit5 = 进门那一页过半规范(>= 1000 格)
;              bit6 = 清页之后字模指纹仍等于 FontRef(清页没伤到平面 2)
;              bit7 = 进门之后这一页被改过(快照与现场不同)。改它的【可能是
;                     我们自己的 4KB 字模拷贝】, 所以这一位不能读成"别人写的";
;                     它是"判据吃现场还是吃进门读数"的开关, 也是下一轮要查
;                     "到底是谁在写页面"的入口
;   显存逐格采样 里那五条 store 相应改写到 旧的逐格读数暂存, 于是"谁写了这一格"没有歧义。
; r412 C7c: 旧的逐格读数暂存 removed -- no references left after C7b.
; 【r412】老仪器(字节块拷贝)的落点: 原来直接写进 Probe+16 与 Probe+24,
;   而 16..23 现在是字符集字段(CS03/CSST/CSA/CSB/CSN) ⇒ 探针一旦跑起来,
;   就会把本轮要读的基均覆盖成 B8000 上的 ASCII 字符(如 "C:\WIND")
;   —— 读数看起来像数字, 实际上是屏幕上那一行文字。改写这两个内部缓冲,
;   drv.c 不再打印它们(r412 起删掉 rawb/rawt 两行)。
DbgRawB            db 8 dup(0)
; 【r412】把用户手打的 "mode co80"+CR 写成 BIOS 键盘码
;   (scancode, ASCII) 对; 由 KbdTypeMode 写进缓冲区
KbdStr             db 032h,06Dh, 018h,06Fh, 020h,064h, 012h,065h
                   db 039h,020h, 02Eh,063h, 00Bh,030h, 009h,038h
                   db 01Ch,00Dh
KbdStrN            equ 9
KbdDone            db 0
Sim10N             db 0          ; r412: Simulate_Int(10h) used this session
PageWitMode         db 0          ; r412: 1 = 5 号拍, 2 = 15 号拍
PalDone             db 0          ; r412: 本场的文本调色板复位已做过(4 号清 0)
I10On               db 0          ; r412: INT 10h 链钩子已安装
I10Hnd              dd 0          ; r412: Hook_V86_Int_Chain 返回的句柄
FontTopN            db 0          ; r412: 本场的字模上半区自检已做过(4 号清 0)
;-----------------------------------------------------------------------------
; 【r412】标准 VGA mode 3 文本态的两张表(6 位 DAC)
;   PalArStd  = 属性控制器的 AR00..AR0F。这不是我们发明的值: 它是 VGA BIOS
;               设 mode 2/3 时装进去的那十六个字节(EGA/CGA 文本调色板)。
;               低四位是"前景色 → DAC 索引", 高四位是"背景色 → DAC 索引"。
;   PalDacStd = DAC 条目 0..15 的 R/G/B。标准 16 色(0/85/170/255)换算到
;               6 位就是 00h/15h/2Ah/3Fh。
;   【为什么这两张表就是修法】`mode co80`(唯一被实测证明有效的动作)做的
;   事里, 与"颜色"有关的就是把这两组重新装一遍。
;-----------------------------------------------------------------------------
PalArStd    db 00h,01h,02h,03h,04h,05h,014h,07h
            db 038h,039h,03Ah,03Bh,03Ch,03Dh,03Eh,03Fh
PalDacStd   db 00h,00h,00h, 00h,00h,02Ah, 00h,02Ah,00h, 00h,02Ah,02Ah
            db 02Ah,000h,000h, 02Ah,000h,02Ah, 02Ah,015h,000h, 02Ah,02Ah,02Ah
            db 015h,015h,015h, 015h,015h,03Fh, 015h,03Fh,015h, 015h,03Fh,03Fh
            db 03Fh,015h,015h, 03Fh,015h,03Fh, 03Fh,03Fh,015h, 03Fh,03Fh,03Fh
CrtcIdx6           db 000h,001h,012h,013h,015h,017h
CrtcVal6           db 05Fh,04Fh,08Fh,028h,096h,0A3h
CrtcN6             equ 6
DbgRawT            db 8 dup(0)
; 【r145】持屏期"活的"硬件指纹的节流计数: 35 号每被调一次 +1,
;   回绕(256 次)时才采一拍 FboxSnap —— 按 35 号实测频率
;   (约 320 次/tick)算, 约每 0.8 秒一份新鲜读数。
;   为什么要它: r144 的指纹只在 48 号(= 设模式之前)采, 于是
;   日志里只有"设模式前"的一拍, 没有盒子花屏期的连续读数,
;   也就看不出 `mode co80` 到底把哪一个寄存器改回来了。
;   它只在数据区, 不进 blob, 不改任何 ABI。
FboxLiveTick        db 0
FontProvN           db 0                  ; 【r412】本场"字模修好"这一枪用过几次(4 号清 0, 上限 2)
FontProvStat        db 0                  ; 【r412】上一次的结果(1..6, 见 VgaFontProv)
; 【r412】持屏期现场修补 的"本场已经采过"标志。4 号(r194 那段复位清单里)清零,
;   17 号第一次进来时置起 —— 于是每场会话最多采一次、最多复写一次视图。
LiveDone            db 0
; 【r146】采集相位 —— 由调用点写好, FboxSnap 自己抄进 [VbeSnap+13]。
;   1/5/7/8/35 = 4 号 / 5 号 / 7 号 / 48 号 / 35 号(持屏期活采样)。
;   同一个寄存器组在这几拍上的值就是整条因果链, 所以相位必须落盘。
FboxSnapPhase       db 0
VbeSnap             db 16 dup(0)           ; FboxSnap 的原始读数(布局同上)
; 【补写的两个预算】4 号武装, 7 号解除。
;   【r147: 预算【整个删掉】了】r143/r144 用 VgaFixN(4 号置 0x40, 每次
;   35 号减一, 走完就 `je vfdb_out`)当"最多补 64 次"的闸门。它是本项目
;   第四次犯同一个错: 一个按【调用次数】计的预算, 在这台机器上等于一个
;   永久开关(前三次: r104 的边沿上限、r141 的 PITCH_ARM_MAX、r145 的
;   边沿节流; 见 STATUS_AND_PLAN 的两条纪律)。本轮的判据换成"六个字节
;   与文本目标逐字节比较" —— 它【自己会停下来】(相等就什么都不做),
;   所以不需要任何总量上限, 只留一个 1/4 的节流(见 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法)。
; VgaFixN 已删除: 数据区里换成 r147 的相位槽与补写记账(见下)。
;--- 【r147】采集槽与补写记账 -------------------------------------------
; PhaseSnap 6 槽 × 16 字节, 顺序与 vbeshare.inc 的 VBE_TR_PS_* 一致:
;   0=P4 / 16=P5 / 32=P7 / 48=P48 / 64=P35(最近) / 80=P35FIRST(本场第一拍)。
;   为什么每个采集点各占一槽: ./146 的六次落盘相位字节全是 0x23(35 号) ——
;   所有采集点共用 FboxSnapVal 时"最后一个写的人赢", 4/5/7 三拍整条因果链
;   被抹掉了。分开存以后一次落盘就能并列比较。
PhaseSnap           db 96 dup(0)
; MemC 16 字节必须【物理连续】: api_gettrace 用 4 条 mov dword 整块搬到
;   blob 352..367(VBE_TR_MemC)。字节顺序见 vbeshare.inc 的 VBE_TR_MC_*。
MemC                db 16 dup(0)
; MemCNow = 六个寄存器的一次现场读数(SR02/SR03/SR04/GCR05/GCR06/AR10),
;   只在 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 内部用, 不进 blob。
MemCNow             db 6 dup(0)
;--- 【r152】取指基址探针数据 -------------------------------------------
; Probe 256 字节必须【物理连续】: api_gettrace 用 rep movsd 整块搬到
;   blob 的 VBE_TR_Probe = 392..639。字节顺序见 vbeshare.inc 的
;   VBE_TR_PR_* —— 这里【直接用】那套名字, 因为 Probe 是 blob 块的逐字节
;   映像(与 MemC 同一套路); 只有 OwnWord 那种"VxD 侧偏移 != blob 侧偏移"
;   的块才需要另起 OW_* 名字。
; 【这一块存在的理由】r143..r151 量遍了 SR/GCR/AR 与 CRTC 的模式寄存器,
;   却从没有量过两件事: ① 控制台那 2000 个文本单元此刻落在 CPU 地址空间
;   的哪里; ② CRTC 的取指地址读到的到底是不是同一块内存。没有这两个数,
;   "写在一处、读在另一处"永远只是假设, 而它是唯一能同时解释 ./144 花屏、
;   ./148 灰屏/黑屏、以及 mode co80 一按就好的机制。
; 【r153 为什么 32 -> 248】./152 的 32 字节版本只回答了"像不像文本",
;   而控制台左上角 32 格本来就是空格 ⇒ 判据恒真、修复恒不触发。
;   新的 32..207 全是【真字节】: 四处 32 字节原文 + 是否逐字节相等 +
;   铺开采样的非空格/种类计数 + 显存扫描到的最像盒子屏的偏移 + 属性
;   调色板 16 个 + DAC 三个入口。判据改成"取指读到的 == CPU 在 B8000
;   写进去的", 这是一个可以逐字节证明的等式。
Probe               db 256 dup(0)
; PrOffB    = CPU B8000 在【当前显存映射选择】下对应的显存字节偏移
; PrTarget  = 那个显存偏移在 CPU 地址空间里的映像(= "CPU 看到的 B8000")
; PrFetch   = CRTC 当前取指地址对应的 CPU 地址
; PrFetchB  = CRTC 当前取指地址对应的【显存字节偏移】(CR0D:CR0C * 2)
PrOffB              dd 0
PrTarget            dd 0
PrFetch             dd 0
PrFetchB            dd 0
PrSaved             db 0        ; 本次修复是否已经存过原始 CR0C/CR0D
PrSave0C            db 0
PrSave0D            db 0
; CharSeen = 256 字节"这个字符见过没有"位图(CellsStats 自己清零)。
; ScanBestVal/ScanBestIdx/ScanN = 显存扫描的三个累加器(每场会话一次)。
CharSeen            db 256 dup(0)
ScanBestVal         db 0
ScanBestIdx         db 0
ScanN               db 0
; 【r155】AaLast/AaSeen —— "A0000 窗口这一拍动过没有"的上一拍快照(8 字节)
;   + 一个"这是不是第一拍"的标志。纯读, 不写任何硬件。
; 【r155】MemcWrote —— 本拍 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 是否真的写了那六个字节。它是
;   【过程内的局部事实】, 所以放在 VxD 数据区而不是 blob: r154 用
;   Probe[TGONCE] 兼当这个标志, 于是一旦 TGONCE 的语义变成"每场预算",
;   读回判定的依据就跟着漂 —— 两个不同的东西不该共用一个字节。
AaLast              db 8 dup(0)
AaSeen              db 0
MemcWrote           db 0
; 【r161】MemCPush / MemCSrc —— "这一拍是不是官方恢复事件(9 号)要求的补写"。
;   为什么需要两个字节: 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 原本只从 35 号那条路被调用, 于是它的
;   预算与去抖都是为"节拍"设计的; 本轮 9 号成了主入口, 那一拍必须【跳过】
;   节流(它是一次真实的状态恢复, 慢半拍就等于没修), 而预算记账仍要照旧
;   留给 35 号那条路。两个标志都只活在一次调用内。
;     MemCPush: 调用方置 1 ⇒ 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 跳过预算/去抖, 进它自己的判据;
;               持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 在读到它的那一刻清零(一次有效)。
;     MemCSrc : 1 = 这一笔来自 9 号 ⇒ 单独记进 S9FIX/S9BAD(验收读数)。
MemCPush            db 0
MemCSrc             db 0
; 【r166】r165 在这里加过两个字节(HoldSrc / BoxHoldTick), 现在撤销。
;   它们服务的是 r165 那条"未武装期每 512 拍补一次"的支路; ./165 证明那条
;   支路在【切回窗口】的那一刻开了三枪(fontn 0009->000C、fixN 000A->000D
;   同一次 dump), 打在主 VDD 自己的 4 平面 MemC 所有者状态上 —— 白窗口。
;   ⇒ 硬件写退出这条支路; 全屏会话那一半改由 9 号(官方 RESTORE_REGISTERS)
;   承担, 见 VgaMemCHold9。两个字节都删掉, 免得留下一个没人读的死开关。
; 【r151】OwnWord = 屏幕所有权 / 特殊 VM / 35 号 AL 的记账块
;   (VxD 侧 24 字节【物理连续】; api_gettrace 用 5 条 mov dword 整块搬到
;    blob 的 VBE_TR_Own = 368..391)。
;   【偏移纪律 —— r150 在这里踩过一次】下面这些 OW_* 是【相对 OwnWord 的】
;   0/4/8/...; blob 侧是 368/372/376/..., 两组数值不同, 所以名字刻意不同。
;   把 blob 偏移当 VxD 偏移用会静默写到块外, 是最难查的一类 VxD 数据区
;   损坏。两边都有反向断言(buildcheck/CHECK)。
;   【为什么现在才有这块】DDK《RESTORE_REGISTERS (Function 9)》给出屏幕
;   所有者的取法:"ESI: Contains the VM handle for the MemC owner VM.
;   ECX: Contains the VM handle for the CRTC owner VM." —— 9 号是整台机器上
;   唯一能直接读出"屏幕归谁"的地方, 而 ./150 里它在 670 次调用里什么都没看。
OwnWord             db 24 dup(0)
OW_MSGVM            EQU 0                   ; → blob 368  蓝屏消息模式伪 VM
OW_PLANARVM         EQU 4                   ; → blob 372  强制平面模式伪 VM
OW_BOXVM            EQU 8                   ; → blob 376  4 号入口 EBX(盒子 VM)
OW_CRTCCLS          EQU 12                  ; → blob 380
OW_MEMCCLS          EQU 13                  ; → blob 381
OW_P35AL            EQU 14                  ; → blob 382
OW_P35MASK          EQU 15                  ; → blob 383
OW_PAIRN            EQU 16                  ; → blob 384
OW_SAVECLS          EQU 17                  ; → blob 385
OW_PLANARN          EQU 18                  ; → blob 386
OW_MSGN             EQU 19                  ; → blob 387
OW_MAPCHG           EQU 21                  ; 【r152】持屏期显存映射选择
                                            ;   (GCR06 bit3-2)变化次数 ——
                                            ;   "谁在会话中途重新施加 4 平面
                                            ;   孔径状态"的唯一直接读数
OW_LASTMAP          EQU 22                  ;   最近一次看到的映射选择
OW_LASTPAIR         EQU 20                  ; → blob 388  上一次的
                                            ;   (CRTC,MemC) 分类对(高半字节
                                            ;   = CRTC, 低半字节 = MemC)
                                            ;   20..23 传送, 24 起是 24 字节
                                            ;   的边界外, 不再用

; 【r175】byte 23【一直是 0、一直在搬】。本轮把它用成
;   R0 取证的一个格子: "盒子持屏期间, BIOS 数据区 0:449
;   (当前显示模式)说的是什么"。
;   【为什么问这个】MS DDK《CHECK_HIRES_MODE》原文:
;     "checks first for a VESA mode, then for an Interrupt 10h BIOS mode
;      above 13h, and then queries the mini-VDD"
;   —— 主 VDD 自己看的就是 0:449 这一个字节。而到现在
;   为止我们从来没在【盒子持屏时】在它那个 VM 里读过它。
;   判读: 0FFh = 本场还没采到; 03h = 盒子认为自己在 mode 3;
;         072h = 盒子认为自己在 72h(> 13h ⇒ 会被判成高分辨率 VM)。
OW_BDA449           EQU 23                  ; → blob 391
; FboxFirstDone: 一场会话里 35 号的"第一拍"是否已经存进 P35FIRST 槽。
;   4 号清零 ⇒ 每一场全屏会话都有一份"主 VDD 刚设完盒子模式"的现场。
FboxFirstDone       db 0
OwBdaDone           db 0                  ; 【r412】本场是否已采过 0:449(4 号清零)
g_ivtOrigSeg        dw 0                  ; IVT[vec] 借用期间暂存: 原向量 seg
g_ivtOrigOff        dw 0                  ;                       原向量 off
MiniDispatchTable   dd 0                  ; 主 VDD 分派表地址
OldDispatch         dd NBR_MINI_VDD_FUNCTIONS dup (0) ; 原表项备份(卸载时恢复)

; 【r76 状态/取证量】全部只读或只记账, 不再有任何 MM/描述符服务调用。
; 帧缓冲映射已改由 display.drv 在 ring 3 用 DPMI 完成(见文件头)。
MapLfbStat          dd 0                  ; VBE_VXD_STAT_* 里程碑位图:
                                        ;  0x0001=V86 缓冲经 DOS AH=48h 建立
                                        ;  0x0004=V86 缓冲走 B0000 回退
                                        ;  0x0010=分派表 5 项已挂上
                                        ;  0x0020=IVT[10h] 目标已取证
                                        ;  0x0040=屏选择子已登记给主 VDD
                                        ;  0x0080=api_exec 跑过
                                        ;  0x0100=GET_MODELIST 跑过
InitStep            dd 0                  ; 状态机步码(见 InitMapLfb/EnsureV86 头注)
LastEax             dd 0                  ; 步失败子码/服务返回的 EAX 快照
; V86 中转缓冲(EnsureV86 在 drv 期分得, 常驻不释放)
g_v86Lin            dd 0                  ; 缓冲线性地址
g_v86Seg            dw 0                  ; 缓冲 V86 段
g_v86Off            dw 0                  ; 缓冲 V86 偏移
g_romSeg            dw 0                  ; V86 IVT[10h] 目标段(Device_Init 取证)
g_romOff            dw 0                  ;                目标偏移
g_v86Ok             dd 0                  ; r48: 中转缓冲可用(EnsureV86 分配+触摸
                                        ; 测试过置 1); api_exec 据它决定可否
                                        ; 数据中转
SelRegistered       dd 0                  ; r76: drv 的屏选择子已登记给主 VDD
                                        ; (DDK 要求"每个会话只登记一次")

; 注册清单：(分派表索引, 处理函数) 对，以 -1 结束。
; 【为何是这 5 项】见文件头"保留的 5 个覆盖项"。初始化与卸载都遍历
; 它，因此必须放在锁定段(卸载发生在非初始化时刻)。
; 【r99: 为什么变成 49 项】主 VDD 只在【被我们替换过表项】的功能上回到
; 我们这里, 没替换的走它自己的默认处理函数 —— 我们永远看不到。于是
; "窗口化 DOS 时主 VDD 问了哪些服务"在日志里完全不可见, 只能推断。
; r99 给其余 44 个索引各装一个【只记录】的桩(TraceStub_n): 记一条 →
; 原样跳到主 VDD 的原处理函数(OldDispatch[n]), 寄存器/栈/返回值全不动
; —— 行为与"未挂钩"逐字节等价, 但调用变得可见。索引 1/35 是带策略的
; 桩(见代码段), 故这里不重复列。
RegList LABEL DWORD
    dd MINIVDD_GET_TOTAL_VRAM_SIZE,       OFFSET32 MiniVDD_GetTotalVramSize
    dd MINIVDD_GET_BANK_SIZE,             OFFSET32 MiniVDD_GetBankSize
    dd MINIVDD_DISPLAY_DRIVER_DISABLING,  OFFSET32 MiniVDD_DisplayDriverDisabling
    dd MINIVDD_VESA_CALL_POST_PROCESSING, OFFSET32 MiniVDD_VesaCallPost
    dd MINIVDD_VESA_SUPPORT,              OFFSET32 MiniVDD_VesaSupport
    ; --- r99 追踪桩: 序号取自 DDK 的 MINIVDD.H, 只标注已核实过的名字 --
        dd 0,  OFFSET32 TraceStub_0            ; REGISTER_DISPLAY_DRIVER(r412: 回退透传)
    dd 1,  OFFSET32 TraceStub_1        ; GET_VDD_BANK(带策略)
    dd 2,  OFFSET32 TraceStub_2
    dd 3,  OFFSET32 TraceStub_3
        dd 4,  OFFSET32 MiniVDD_PreHiResToVga   ; PRE_HIRES_TO_VGA(r412: 记 VGA 模式)
    dd 5,  OFFSET32 TraceStub_5        ; POST_HIRES_TO_VGA
    dd 6,  OFFSET32 TraceStub_6        ; PRE_VGA_TO_HIRES
    dd 7,  OFFSET32 TraceStub_7        ; POST_VGA_TO_HIRES
    dd 8,  OFFSET32 TraceStub_8        ; SAVE_REGISTERS
    dd 9,  OFFSET32 TraceStub_9        ; RESTORE_REGISTERS
    dd 10, OFFSET32 TraceStub_10
    dd 11, OFFSET32 MiniVDD_AccessVgaMemoryMode     ; r412: 真正实现(以前是空桩)
    dd 12, OFFSET32 MiniVDD_AccessLinearMemoryMode  ; r412: 对称的那一半
    dd 13, OFFSET32 TraceStub_13       ; ENABLE_TRAPS
    dd 14, OFFSET32 TraceStub_14       ; DISABLE_TRAPS
    dd 15, OFFSET32 TraceStub_15
    dd 16, OFFSET32 TraceStub_16
    dd 17, OFFSET32 TraceStub_17
    dd 18, OFFSET32 TraceStub_18
    dd 19, OFFSET32 TraceStub_19
    dd 20, OFFSET32 TraceStub_20
    dd 21, OFFSET32 TraceStub_21
    dd 22, OFFSET32 TraceStub_22
    dd 23, OFFSET32 TraceStub_23
    dd 24, OFFSET32 TraceStub_24
    dd 25, OFFSET32 TraceStub_25
    dd 27, OFFSET32 TraceStub_27
    dd 28, OFFSET32 TraceStub_28
    dd 29, OFFSET32 TraceStub_29
;   r412: hook 30 (PRE_INT_10_MODE_SET) is not installed on purpose
;   r412: hook 31 (POST_INT_10_MODE_SET) is not installed on purpose
    dd 32, OFFSET32 TraceStub_32       ; GET_CURRENT_BANK_WRITE
    dd 33, OFFSET32 TraceStub_33       ; GET_CURRENT_BANK_READ
    dd 34, OFFSET32 TraceStub_34       ; SET_BANK
        dd 35, OFFSET32 MiniVDD_CheckHiResMode    ; CHECK_HIRES_MODE(r412: 按官方 AL 判据回答)
        dd 38, OFFSET32 MiniVDD_SetHiResMode     ; SET_HIRES_MODE(r412: 返回 NC -> 主 VDD 做 VESA 设置)
    dd 39, OFFSET32 TraceStub_39       ; PRE_HIRES_SAVE_RESTORE
    dd 40, OFFSET32 TraceStub_40       ; POST_HIRES_SAVE_RESTORE
    dd 42, OFFSET32 TraceStub_42
    dd 43, OFFSET32 TraceStub_43       ; CHECK_SCREEN_SWITCH_OK
    dd 44, OFFSET32 TraceStub_44
    dd 45, OFFSET32 TraceStub_45
    dd 46, OFFSET32 TraceStub_46
;   r412: hook 48 (INT 10h) is not installed on purpose       ; PRE_INT_10_MODE_SET(无文档页)
    dd -1

;-----------------------------------------------------------------------------
; 【r136】本轮的 VxD 侧零改动 —— 版本号为什么还是动了
;   本轮的行为修正在 C 侧(display.c 的会话开始边沿 / 环落盘纪律), 这份汇编
;   与 r135 逐字节相同。仍然递增 VBE_VXD_VERSION(0x0116 -> 0x0117)的理由只有
;   一条, 而且是被 ./135 逼出来的:
;     ./135 的日志里 "VxD version=0116" 同时对应【两种】可能的机器状态 ——
;     (a) 用户按说明只换了 omninine.drv(旧 minivdd.vxd 仍在), 或
;     (b) 用户确实换了两个文件, 而本轮 VxD 恰好没改。
;   这两种状态在日志上无法区分, 于是"VxD 侧到底跑的是哪一版"这个最基本的
;   前提在每一轮都要靠猜。递增一次, 就把它变成一行可读的断言。
;   r137-marker: AR10h repair also sets bit6 (0041h instead of 0001h), i.e. the
;                repair no longer leaves a hybrid value behind; drv.c snapshots
;                and replays AR10h..AR14h as whole bytes from now on.
;   r136-marker: VBE_VXD_VERSION bumped for pairing clarity only (no asm change
;   in this file versus r135; both files must be deployed together).
;-----------------------------------------------------------------------------

;-----------------------------------------------------------------------------
; 【r162】标准 mode 3 的六个几何寄存器(索引表 + 标准值表, 文本态修补 只读比较)
;   值逐字节取自 vga-register-reference.md §6c 的 CRTC 25 字节串:
;     CR00..CR18 = 5F 4F 50 82 55 81 BF 1F 00 4F 0D 0E 00 00 00 00
;                  9C 8E 8F 28 1F 96 B9 A3 FF
;   ⇒ CR00=5F CR01=4F CR12=8F CR13=28 CR15=96 CR17=A3。
;   两个表长度必须相同(6); 位序 = 这里的顺序, 见 VBE_TR_PR_GEOMASK。
;   放在锁定段: 文本态修补 在 4/5 号(切屏期)跑来读它。
;   r162-marker: GeoStdIdx/GeoStdVal (standard mode 3 geometry, read-only).
;-----------------------------------------------------------------------------
GeoStdIdx           db 000h, 001h, 012h, 013h, 015h, 017h
GeoStdVal           db 05Fh, 04Fh, 08Fh, 028h, 096h, 0A3h
; 【r166】8 个寄存器的完整"标准 mode 3 文本屏"指纹(索引/值同序)。
;   前六格与 GeoStdIdx/GeoStdVal 逐字节相同 —— 位序 0..5 的含义不变, 于是
;   r162/r163 的 geomask 读数与旧日志可以直接对比。后两格是本轮新增:
;     bit6 = CR09(最大扫描行)  文本 mode 3 = 4Fh, mode 13h = 41h
;     bit7 = CR14(下划线位置)  文本 mode 3 = 1Fh, mode 13h = 40h
;   【为什么必须要这两格】判据要回答的是"CRTC 此刻是不是【在显示文本屏】",
;   而 mode 13h(320x200)的 CR00/CR01/CR12/CR13/CR15/CR17 与 mode 3 逐字节
;   相同(两者共用同一组水平/垂直几何字段) —— 只比六格会把一个全屏图形 VM
;   判成文本屏, 而那正是 r148 灰屏那一类事故的形状。
;   ⇒ 位序 = 这里的先后顺序, 见 VBE_TR_PR_GEOMASK。
;   r166-marker: GeoM3* = the eight-register "is the CRTC displaying mode 3" test.
GeoM3Idx            db 000h, 001h, 012h, 013h, 015h, 017h, 009h, 014h
GeoM3Val            db 05Fh, 04Fh, 08Fh, 028h, 096h, 0A3h, 04Fh, 01Fh

VxD_LOCKED_DATA_ENDS

;=============================================================================
; 锁定代码段
;=============================================================================
VxD_LOCKED_CODE_SEG

;=============================================================================
; r99: mini-VDD 分派调用追踪
;
; 【桩必须"行为等价于未挂钩"】三条硬要求:
;   1. 记一条后【原样跳到 OldDispatch[n]】(主 VDD 自己的原处理函数),
;      EAX/ECX/EDX/EBX/EBP/EFLAGS/栈全部保持 —— 参数与返回语义都不动;
;   2. 原表项为 0(该功能主 VDD 未实现)时回落成 VDD_NC, 绝不跳空;
;   3. 每个索引一个独立桩(地址必须彼此不同, 否则主 VDD 的分派表项
;      分不清调用的是哪一个功能)。
; 【为何不自己写 49 个处理函数】绝大多数功能我们没有足够信息实现,
; 唯一安全的做法就是"让主 VDD 自己继续做", 我们只在旁边记账。
;=============================================================================

;-----------------------------------------------------------------------------
; MiniVDD_TraceRecord —— 记一条分派调用(纯记账)
; 入口: EAX = 分派功能号; EBP → 客户端寄存器帧
; 出口: 全部寄存器与标志保持(内部全保存)
; 【坑】EBX/EBP 是主 VDD 的活寄存器(VM 句柄/客户端帧), 本过程若破坏
;       它们就是随机蓝屏 —— 故全部现场保存。
; 【WinVMHandle 不在这里取】第一次分派调用可能早于 drv 加载(那时
;       当前 VM 未必是 Windows VM), 故改在 PM API 入口取(那里必是
;       drv 在 Windows VM 里调用)。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_TraceRecord

        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     edi, eax                   ; EDI = 功能号
        cmp     edi, VBE_TRACE_FUNCS
        jb      short tr_f_ok
        mov     edi, VBE_TRACE_FUNCS-1     ; 越界钳制: 永不越界写
tr_f_ok:
        inc     dword ptr [TraceCounts + edi*4]

        ; 环形写入: 序号 / 功能号 / flags / Client_ECX / Client_EDX
        ;           + 【r100】EBX(发起调用的 VM) / Client_EAX
        mov     eax, [TraceTotal]
        inc     dword ptr [TraceTotal]
        inc     eax                        ; 序号从 1 起
        mov     ecx, [TracePtr]
        mov     edx, ecx
        lea     edx, [edx + edx*2]         ; ×3
        shl     edx, 3                     ; ×24 = VBE_TRACE_RECB
        mov     esi, OFFSET32 TraceRecs
        add     esi, edx                   ; ESI → 本次记录
        mov     [esi + VBE_TREC_Seq], eax
        mov     word ptr [esi + VBE_TREC_Func], di
        mov     byte ptr [esi + VBE_TREC_Flags], 0
        mov     byte ptr [esi + VBE_TREC_Pad], 0
        mov     eax, [ebp].Client_ECX
        mov     [esi + VBE_TREC_Ecx], eax
        mov     eax, [ebp].Client_EDX
        mov     [esi + VBE_TREC_Edx], eax
        ; 【r100】EBX 在本过程里从未被改写 —— 它就是主 VDD 分派约定里
        ;   的"发起调用的 VM 句柄", 也是分辨 Windows VM / DOS VM 的唯一依据
        mov     [esi + VBE_TREC_Ebx], ebx
        mov     eax, [ebp].Client_EAX      ; 入口 EAX(如 48 号 = 请求的模式号)
        mov     [esi + VBE_TREC_Eax], eax
        inc     ecx
        cmp     ecx, VBE_TRACE_RECS
        jb      short tr_ptr_ok
        xor     ecx, ecx
tr_ptr_ok:
        mov     [TracePtr], ecx

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret

EndProc MiniVDD_TraceRecord

;-----------------------------------------------------------------------------
; TraceStub_n —— 只记录的分派桩(索引 n)
;   pushfd/push eax → 记一条 → 复原 → 跳主 VDD 原处理函数。
;   【为何 pushfd/popfd】进入分派时主 VDD 可能带着自己的标志位语义,
;   记录过程(inc/call)会改标志, 必须原样还回去。
;   r99-marker: trace stubs pass through to the VDD's own handler.
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; [r412] MiniVDD_SetHiResMode (38)
;   DDK 样例(S3MINI.ASM L3337)原文:
;     Exit: CY returned if MiniVDD did the mode change.
;           NC returned if VDD is to do a VESA HiRes mode set.
;   我们不做模式设置, 直接返回 NC -> 让主 VDD 走它的 VESA 官方路径.
;   (样例里的 mov eax,86h + Exec_VxD_Int 是 S3 专用 ROM bug 规避, 通用实现不需要)
;-----------------------------------------------------------------------------

;-----------------------------------------------------------------------------
; [r412] MiniVDD_CheckHiResMode (35)
;   DDK: CY = VM 在 HiRes; NC = VM 在标准 VGA.
;   DDK 官方(win98ddk: CHECK_HIRES_MODE 35): AL = 该 VM 的 Int 10h 模式,
;   CY = HiRes / NC = 标准 VGA; 远东版已把 72h/73h 归一化为 02h/03h.
;-----------------------------------------------------------------------------
BeginProc MiniVDD_CheckHiResMode
        inc     word ptr [Ctr35]                ; [r412] 1/256 采样(修正: 用 0FFh 判低位)
        test    byte ptr [Ctr35], 0FFh
        jnz     short CHM404_judge
        push    eax
        push    edi
        mov     eax, 35
        call    MiniVDD_TraceRecord
        pop     edi
        pop     eax
CHM404_judge:
        ; 官方(win98ddk: CHECK_HIRES_MODE 35): AL = 该 VM 的 Int 10h 模式
        ;   CY = hi-res ; NC = 标准 VGA
        ; [r447] 采用 r424 的分支(实测不花屏):
        ;   1) 若已捕获 WinVMHandle 且 EBX 就是它 => 桌面 => CY
        ;   2) 其它(含"尚未捕获") => 当作盒子 VM => 72h/73h 折算成 02h/03h 后判 13h
        ; 注: r425/r426 把"尚未捕获"改成答 CY, 结果窗口盒花屏 —— 说明
        ;     未捕获时被问的其实是盒子; 这一版恢复 r424 的行为。
        cmp     dword ptr [WinVMHandle], 0
        je      CHM424_clamp
        cmp     ebx, dword ptr [WinVMHandle]
        jne     short CHM424_clamp
        stc
        ret
CHM424_clamp:
        ; 盒子 VM: 若上游没把 72h/73h 折算成 02h/03h, 这里兜底
        cmp     al, 72h
        jne     short CHM422_c73
        mov     al, 02h
        jmp     short CHM422_cmp
CHM422_c73:
        cmp     al, 73h
        jne     short CHM422_cmp
        mov     al, 03h
CHM422_cmp:
        cmp     al, 13h
        ja      CHM404_hires
        clc
        ret
CHM404_hires:
        stc
        ret
EndProc MiniVDD_CheckHiResMode

;-----------------------------------------------------------------------------
; [r412] MiniVDD_RegisterDisplayDriver (0)
;   捕获"调用本钩子的 VM" = Windows VM 句柄(WinVMHandle), 供 CHECK_HIRES_MODE
;   比较使用. 之后仍转发给默认实现.
;-----------------------------------------------------------------------------
BeginProc MiniVDD_RegisterDisplayDriver
        push    eax
        mov     eax, 0
        call    MiniVDD_TraceRecord
        pop     eax
        mov     dword ptr [WinVMHandle], ebx     ; 记下 Windows VM
        cmp     dword ptr [OldDispatch + 0*4], 0
        je      RDD_null
        jmp     dword ptr [OldDispatch + 0*4]
RDD_null:
        VDD_NC
        ret
EndProc MiniVDD_RegisterDisplayDriver

;-----------------------------------------------------------------------------
; [r412] MiniVDD_PreHiResToVga (4) —— 只记账并转发(不再维护每 VM 模式; 35 号已按官方 AL 判据)
;-----------------------------------------------------------------------------
BeginProc MiniVDD_PreHiResToVga
        push    eax
        push    edi
        mov     eax, 4
        call    MiniVDD_TraceRecord
        pop     edi
PHV_done:
        pop     eax
        cmp     dword ptr [OldDispatch + 4*4], 0
        je      PHV_null
        jmp     dword ptr [OldDispatch + 4*4]
PHV_null:
        VDD_NC
        ret
EndProc MiniVDD_PreHiResToVga

;-----------------------------------------------------------------------------
; [r412] MiniVDD_SetHiResMode (38) —— 返回 NC 让主 VDD 设 VESA 模式(文档要求)
;-----------------------------------------------------------------------------
BeginProc MiniVDD_SetHiResMode
        push    eax
        push    edi
        mov     eax, 38
        call    MiniVDD_TraceRecord
        pop     edi
SHR_done:
        pop     eax
        clc
        ret
EndProc MiniVDD_SetHiResMode

;-----------------------------------------------------------------------------
TraceStub MACRO fn
BeginProc TraceStub_&fn
        pushfd
        push    eax
        mov     eax, fn
        call    MiniVDD_TraceRecord
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + fn*4], 0
        je      short ts_null_&fn
        jmp     dword ptr [OldDispatch + fn*4]
ts_null_&fn:
        VDD_NC
        ret
EndProc TraceStub_&fn
        ENDM

;-----------------------------------------------------------------------------
; TraceStubEvt —— 与 TraceStub 完全同形, 额外把一个 8 位事件计数器 +1
;   【r100 用途】POST_VGA_TO_HIRES(7) 的计数变化 = "主 VDD 的切屏事务已
;   收尾(DDK 时序第 6 步: 恢复陷阱)" —— 这是 drv 判断"现在可以安全重建
;   模式"的唯一可靠信号(回调内部绝不能做 BIOS 调用)。
;   inc 在 pushfd/popfd 窗口内, 且 TraceRecord 自持现场, 语义与未挂钩等价。
;-----------------------------------------------------------------------------
TraceStubEvt MACRO fn, evt
BeginProc TraceStub_&fn
        pushfd
        push    eax
        mov     eax, fn
        call    MiniVDD_TraceRecord
        inc     byte ptr [TraceEvt + evt]
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + fn*4], 0
        je      short tse_null_&fn
        jmp     dword ptr [OldDispatch + fn*4]
tse_null_&fn:
        VDD_NC
        ret
EndProc TraceStub_&fn
        ENDM

; r101-marker: screen-switch state machine + GET_VDD_BANK refuse + 48 recorder
;-----------------------------------------------------------------------------
; 【r101】TraceStubState —— 与 TraceStubEvt 同形, 额外维护切屏状态机
;   入口参数: fn = 功能号, evt = 事件计数桶(-1 = 不计数),
;             setMask = 要置的状态位, andMask = 要保留的状态位(按位与)
;   例: TraceStubState 4, VBE_TR_EVT_PRE_HIRES_TO_VGA, VBE_TR_ST_VGA, 0FFh
;   【为何不用一条带指令参数的宏】本机没有 MASM, 不能试错 —— 这里只用
;   工程里已经被汇编过无数次的 or/and/inc 三个指令与 [mem] 直接寻址。
;-----------------------------------------------------------------------------
TraceStubState MACRO fn, evt, setMask, andMask
BeginProc TraceStub_&fn
        pushfd
        push    eax
        mov     eax, fn
        call    MiniVDD_TraceRecord
        IF evt GE 0
        inc     byte ptr [TraceEvt + evt]
        ENDIF
        IF setMask NE 0
        or      byte ptr [TraceState], setMask
        ENDIF
        IF andMask NE 0FFh
        and     byte ptr [TraceState], andMask
        ENDIF
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + fn*4], 0
        je      short tst_null_&fn
        jmp     dword ptr [OldDispatch + fn*4]
tst_null_&fn:
        VDD_NC
        ret
EndProc TraceStub_&fn
        ENDM

        ; 【r101 变更】0/4/5/7/11/12/13/14/48 号从下面的"只记录"名单里
        ;   移出, 改由 TraceStubState / 专用处理函数维护切屏状态机与
        ;   48 号的模式号记录(语义仍是"记完原样转给主 VDD 原处理函数",
        ;   与未挂钩等价的只有 48 号那条可选策略)。
        TraceStub 2
        TraceStub 3
        ; 【r151】8/9 号不再是宏: 见下面手写的 TraceStub_8/9(9 号的
        ;   ESI/ECX 是 DDK 定义的 MemC/CRTC 所有者, 是屏幕归谁的唯一
        ;   直接读数)。
        ;   r151-marker: TraceStub_8/9 are hand-written now.
        TraceStub 10
        ; 【r157】17(CRTC_OUT)/19(SEQ_OUT)/21(GCR_OUT) 不再是宏: 它们要按
        ;   寄存器下标计数(见下面手写的三个过程)。15 号是
        ;   MAKE_HARDWARE_NOT_BUSY, 【不是】GCR_OUT —— 本表用的是 DDK 的
        ;   十进制功能号(0x11/0x13/0x15 就是 17/19/21), 别按十六进制读。
        ; r157-marker: the dispatch table is indexed in DECIMAL (15 is
        ;              MAKE_HARDWARE_NOT_BUSY, not GCR_OUT).
        ; 【r163】15 号也不再是宏: DDK《MAKE_HARDWARE_NOT_BUSY (Function 15)》
        ;   把 CRTC 所有者(EAX)/当前 VM(EBX)/MemC 所有者(ECX)三个句柄一次
        ;   交出来, 而"MemC 状态正从 Windows hi-res 切到 VGA 平面态"这句话
        ;   是官方文档里对"会话中途谁在动硬件"唯一的直接描述。见下面手写的
        ;   TraceStub_15(只读取证, 一个寄存器都不碰)。
        ;   r163-marker: TraceStub_15 is hand-written now.
        TraceStub 16
        TraceStub 18
        TraceStub 20
        TraceStub 22
        TraceStub 23
        TraceStub 24
        TraceStub 25
        TraceStub 27
        TraceStub 28
        TraceStub 29
        TraceStub 30
        TraceStub 31
        TraceStub 32
        TraceStub 33
        ; r412: 34 号改手写（真正换 bank），见 TraceStub_34
        TraceStub 38
        TraceStub 39
        TraceStub 40
        TraceStub 42
        TraceStub 43
        TraceStub 44
        ; 【r412】45/46 改成手写观察器: 这两个是官方文档里唯一落在
        ;   "主 VDD 把硬件强行切成平面(文本)态"那一刻的钩子, 而它们的
        ;   调用现场本项目从未观测过(宏桩只记函数号)。见下面两个过程。
        ;   r412-marker: TraceStub_45/46 are hand-written now.

        ; 【r100】切屏族: 除记录之外再维护事件计数器(TraceStubEvt 头注)
        ; 【r151】6 号不再是宏: 它要按 DDK 原文在 PRE_VGA_TO_HIRES
        ;   这一拍关掉扩展寄存器陷阱(见下面手写的 TraceStub_6)。
        ;   r151-marker: TraceStub_6 is hand-written now.

        ; 【r101】切屏族 + 状态机
        ;   第 4 个参数是"保留位掩码"(按位与), 一律写成 8 位十六进制立即数
        ;   —— 不用 NOT 表达式, 免得 MASM 对超过 0FFh 的字节立即数报错。
        ; 【r116】4/13/14 号不再是 TraceStubState 宏: 它们要在同一现场
        ;   开关 1CEh/1CFh 的全局陷阱(见下面手写的 TraceStub_4/13/14)。
        ;   15 号(ENABLE_VGA_TRAPS/其它)与 16/17 仍是纯记账宏。
        ;   r116-marker: TraceStub_4/13/14 are hand-written now.
        ; 【r129】7 号不再是 TraceStubState 宏: 它现在与 5 号【对称】——
        ;   宏展开的内容被原样手写进下面的 TraceStub_7, 多出来的只有转交
        ;   主 VDD 原处理函数【之前】那一次 HiResClockFix 调用(理由见该
        ;   函数的长注: 5 号补文本取指, 7 号补图形时钟位)。
        ;   r129-marker: TraceStub_7 is hand-written now (was TraceStubState 7).
        TraceStubState 11, -1,                          VBE_TR_ST_VGA, 0FFh
        TraceStubState 12, -1,                          0, 0FEh

;=============================================================================
; 【r116】VBE_DISPI(1CEh/1CFh)端口虚拟化 —— 参考实现的那一件事
;
; 【为什么需要它(症状逐字对得上)】
;   boxv9x 的 mini-VDD(vxd/boxvmini.asm, Philip Kelley 2023)文件头原文:
;     "Having this mini-VDD present fixes a couple of problems with the
;      BOXV9X driver. First, it virtualizes accesses to the Bochs VGA
;      registers on I/O ports 1CEh and 1CFh (VBE_DISPI_IOPORT_INDEX and
;      VBE_DISPI_IOPORT_DATA). This means that when you start an MS-DOS
;      Prompt, the screen doesn't go wonky when the DOS VM, calling into
;      the VGA ROM BIOS, ends up trying to perform I/O to those ports."
;   vmdisp9x 的 vxd_main.c::virtual_0x1ce() 注释同义:
;     "This is fix of broken screen when open DOS window".
;   两台独立实现、同一类 VBE(Bochs/VBox VBE_DISPI: LFB=0xDE000000、
;   16MB、VBE 3.0 —— 正是本测试机的 VBE_EnumModes 画像)、同一个症状。
;
; 【机制】Bochs/VBox 的 VBE 模式不是靠传统 VGA 寄存器决定的, 而是靠
;   1CEh(索引)/1CFh(数据)这对端口里的 VBE_DISPI 寄存器(XRES/YRES/BPP/
;   ENABLE/VIRT_WIDTH…)。窗口化 DOS 盒的 BIOS 调 INT 10h 设模式时也会写
;   这对端口 —— 于是【Windows 的 1024x768x16bpp 现场】被 DOS 盒的模式
;   参数改掉: 显存内容还是 Windows 画的(所以虚拟机内截图完好), 扫描
;   输出却按另一个几何/像素格式解释(所以主机端花屏)。这解释了本项目
;   追了很多轮的"客户机对、主机错"以及"反复 Alt+Enter 后越弄越脏"。
;
; 【判据只一条: 谁拥有 CRTC】DDK 的 VDD_Get_VM_Info 原文:
;   "Return Values: EDI contains the VM handle of the CRTC owner. ESI
;    contains the VM handle of the MemC owner. Remarks: … used by mini-VDDs
;    to decide whether to virtualize or pass through I/O when virtualizing
;    extended registers."
;   于是: CRTC 在 Windows 手里 + 发起 I/O 的不是 Windows ⇒ 吃掉;
;   其余(CRTC 不在 Windows 手里 = 全屏 DOS 会话; 或发起者就是 CRTC
;   所有者)⇒ 原样放行给真硬件。窗口化 DOS 被挡的只有"它不是屏幕主人
;   却想改显示格式"这一件事。
;
; 【安全阀(我们比参考实现多两条)】策略 0、或 handler 没装成、或还不知道
;   Windows VM 句柄 ⇒ 一律放行。失败方向永远是"回到 r115 的行为",
;   不会出现"把 Windows 自己的 I/O 吃掉"这种不可恢复的场面。
;=============================================================================

;-----------------------------------------------------------------------------
; MiniVDD_Virtual1CE —— 1CEh/1CFh 的 I/O handler
; 入口(I/O handler 约定): AL = 值, EBX = 发起 I/O 的 VM 句柄,
;   ECX = 方向/宽度, EDX = 端口号。
; 出口: 要么 VxDJmp VDD_Do_Physical_IO(真硬件 I/O), 要么直接 ret(吃掉)。
; 【现场纪律】与 boxv9x 逐条同形: 只用 EAX/EDI(以及 ESI 的返回值), 不碰
;   ECX/EDX —— 真 I/O 的参数由 VDD 自己保存的陷阱现场恢复, 这也是参考
;   实现敢在跳到 VDD_Do_Physical_IO 之前做 VxDCall 的原因。
;   r116-marker: MiniVDD_Virtual1CE (1CEh/1CFh I/O virtualization).
;-----------------------------------------------------------------------------
BeginProc MiniVDD_Virtual1CE
        cmp     byte ptr [VbeState.VBS_policyDispi], 0
        je      short dv_phys                  ; 策略关: 永不吃
        cmp     byte ptr [DispiInstalled], 3
        jne     short dv_phys                  ; 两个端口没都装成: 永不吃
        cmp     dword ptr [WinVMHandle], 0
        je      short dv_phys                  ; 还不知道 Windows VM: 永不吃
        VxDCall VDD_Get_VM_Info                ; EDI = CRTC 所有者
        cmp     edi, [WinVMHandle]
        jne     short dv_phys                  ; CRTC 不在 Windows 手里: 放行
        cmp     ebx, edi
        je      short dv_phys                  ; 发起者就是 CRTC 所有者: 放行
        or      byte ptr [TraceState], VBE_TR_ST_DISPI_EAT
        ret                                    ; 吃掉: 不改 AL, 不做真 I/O
dv_phys:
        or      byte ptr [TraceState], VBE_TR_ST_DISPI_PASS
        VxDJmp  VDD_Do_Physical_IO
EndProc MiniVDD_Virtual1CE

;-----------------------------------------------------------------------------
; 【r120】MiniVDD_DispiPhysRead —— 用 ring 0 的【物理 I/O】读 DISPI 寄存器
;
; 【为什么必须换一条路读】./119 把 ring 3 的读判了死刑: r119 的
; "写—回读—还原"梯形在整场会话里跑了 28 次, 每一次都是
;     DISPIID pre(只读)=FFFF  写了 BE02  回读=FFFF  -> 没有应答
; 两种可能:
;   ① 这台机器的 VBE 设备【根本没有】在 1CEh/1CFh 上注册处理器
;      (那 VBE BIOS 就是用别的手段设模式的, DISPI 这条线彻底关门);
;   ② 端口在, 但【我们 ring 3 的那次 I/O 没有抵达硬件】—— 主 VDD 在为
;      Windows VM 虚拟化这两个端口(它不是标准 VGA 端口, 但它认得 Bochs
;      VBE), 于是我们的 in/out 被吃掉, 读回来就是浮空的全 1。
; 这两种可能对"下一步该做什么"的影响完全相反, 而 ring 3 分不出来。
;
; 【怎么分】绕开 VM 的端口虚拟化, 直接做物理 I/O。DDK 的 VDD_Do_Physical_IO
;   原文(library.thedatadungeon.com 的 Win98 DDK 页):
;       EAX: 要写出去的值(只写操作)
;       EBX: 正在做这次 I/O 的 VM 句柄
;       ECX: I/O 类型(见 VMM.INC)
;       EDX: 端口号
;       "This service may be used, not only by mini-VDDs, but by any Ring 0
;        VxD that wishes to perform physical I/O on behalf of a VM client."
;   我们本来就在 I/O handler 里 `VxDJmp VDD_Do_Physical_IO` 放行过真 I/O,
;   所以这条路在本项目里是【已验证可达】的; 区别只在于这次是我们主动调,
;   而不是被动放行。
;
; 【为什么这在 Windows VM 里也安全】EBX 传当前 VM 句柄(Get_Cur_VM_Handle),
;   这正是"代表本 VM 做物理 I/O"的原意; 只读 6 个 16 位寄存器, 不写数据
;   端口, 不改任何模式。ECX = Word_Input 由 VMM.INC 解析成正确位模式。
;
; 【结果放哪】api_gettrace 的 blob 里 offset 4..27 那 24 字节 —— 布局原本
;   是 "0 dd WriteIdx / 4 dd 保留 / 8 49×dd 计数", 保留字段从未被用过,
;   不用动 VBE_TRACE_HDR 就不怕旧 drv 读新 vxd(反之亦然: 旧 vxd 给新 drv
;   时这段是 0, drv 侧按"还没读过"处理)。
;   六个字顺序: +0 ID / +2 XRES / +4 YRES / +6 BPP / +8 ENABLE / +10 VIRTW,
;   +12 起留 0。
; 【现场纪律】VDD_Do_Physical_IO 会改 EAX/ECX/EDX/flags ⇒ 全程自己保存
;   恢复(含 EBX/ESI/EDI), 与 MiniVDD_DispiTrapOn/Off 同一套纪律。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_DispiPhysRead

        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     eax, dword ptr [DispiPhysVal]   ; 先清 24 字节(12 dw)
        xor     eax, eax
        mov     dword ptr [DispiPhysVal],      eax
        mov     dword ptr [DispiPhysVal + 4],  eax
        mov     dword ptr [DispiPhysVal + 8],  eax
        mov     dword ptr [DispiPhysVal + 12], eax
        mov     dword ptr [DispiPhysVal + 16], eax
        mov     dword ptr [DispiPhysVal + 20], eax

        VMMCall Get_Cur_VM_Handle               ; EBX = 当前 VM 句柄
        mov     edi, ebx                        ; EDI = 保留住它(下面要用两次)
        mov     esi, OFFSET32 DispiPhysVal      ; ESI = 输出区

        ; --- 索引 0x0000: ID ---
        xor     eax, eax
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     [esi], ax

        ; --- 索引 0x0001: XRES ---
        mov     eax, 1
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     [esi+2], ax

        ; --- 索引 0x0002: YRES ---
        mov     eax, 2
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     [esi+4], ax

        ; --- 索引 0x0003: BPP ---
        mov     eax, 3
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     [esi+6], ax

        ; --- 索引 0x0004: ENABLE ---
        mov     eax, 4
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     [esi+8], ax

        ; --- 索引 0x0006: VIRT_WIDTH ---
        mov     eax, 6
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     [esi+10], ax

        ; --- 决定性一问: 把已知 ID 写进索引 0 再回读 ---
        ; 【为什么必须写一次】DISPI 的索引 0 是只写寄存器, 不先写就读,
        ;   读到的只是"索引寄存器残值"或浮空值 —— ./119 的 ring 3 梯形
        ;   已经用 28 次 FFFF 演示过这件事。只有"写进去能读回来"才证明
        ;   这对端口【确实有人应答】。写的是 ID, 不参与 xres/yres/bpp/
        ;   enable 任何一个决定扫描格式的字段, 且写完立刻把原值写回。
        mov     ax, [esi]                       ; 进来时读到的 ID(可能是 FFFF)
        push    eax                             ; 保存(只低 16 位有用)
        mov     eax, 0BE02h
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     eax, 0BE02h
        mov     edx, 01CFh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     eax, 0
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        mov     edx, 01CFh
        mov     ecx, Word_Input
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        cmp     ax, 0BE02h
        jne     short dpr_restore               ; 没命中: 端口不存在/不认这个 ID
        mov     byte ptr [DispiPhysOk], 1       ; 命中: 端口确实活着
dpr_restore:
        pop     eax                             ; 取回原 ID
        mov     edx, 01CEh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO
        movzx   eax, ax                         ; 原 ID 写回(低 16 位)
        mov     edx, 01CFh
        mov     ecx, Word_Output
        mov     ebx, edi
        VxDCall VDD_Do_Physical_IO

        inc     byte ptr [DispiPhysN]           ; 记账: 这条路真的跑过一次

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
EndProc MiniVDD_DispiPhysRead

;-----------------------------------------------------------------------------
; MiniVDD_DispiTrapOn / MiniVDD_DispiTrapOff —— 开关两个端口的全局陷阱
;   调用者: 4 号(关: DOS VM 即将接管屏幕)、5 号(开)、13 号(开)、
;   14 号(关)、26 号(关: 显示驱动卸载, Windows 不再是屏幕主人)。
;   DDK 原文(§2.9): "Within your ENABLE_TRAPS and DISABLE_TRAPS routines,
;   you must call Enable_Global_Trapping or Disable_Global_Trapping once for
;   each port."
;   【现场纪律】VMM 服务会改 EAX/ECX/EDX/flags, 而 4/5 号之后还要把现场
;   交给主 VDD 的原处理函数 ⇒ 两个过程自己保存恢复一切(含 EBX/ESI/EDI)。
;   r116-marker: MiniVDD_DispiTrapOn/Off (Enable/Disable_Global_Trapping).
;   【r117 逐条对齐 boxvmini.asm 的调用约定】端口号在 EDX:
;       mov edx, 1ceh
;       VMMcall Enable_Global_Trapping
;   r116 用的是 EAX(错), 于是"陷阱开关"作用在端口 30h 上, 而 1CEh/1CFh
;   从头到尾没有受到任何保护 —— 这正是 ./116 全部日志 dispi 位恒 0 的
;   另一半原因(第一位原因是 Install_IO_Handler 也没装对)。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_DispiTrapOn
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        cmp     byte ptr [VbeState.VBS_policyDispi], 0
        je      short dton_out
        cmp     byte ptr [DispiInstalled], 3
        jne     short dton_out
        mov     edx, 01CEh
        VMMcall Enable_Global_Trapping
        mov     edx, 01CFh
        VMMcall Enable_Global_Trapping
        inc     byte ptr [DispiActs]
        mov     byte ptr [DispiTrapOn], 1
        or      byte ptr [TraceState], VBE_TR_ST_DISPI
dton_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc MiniVDD_DispiTrapOn

BeginProc MiniVDD_DispiTrapOff
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        cmp     byte ptr [VbeState.VBS_policyDispi], 0
        je      short dtoff_out
        cmp     byte ptr [DispiInstalled], 3
        jne     short dtoff_out
        mov     edx, 01CEh
        VMMcall Disable_Global_Trapping
        mov     edx, 01CFh
        VMMcall Disable_Global_Trapping
        inc     byte ptr [DispiActs]
        mov     byte ptr [DispiTrapOn], 0
        and     byte ptr [TraceState], 0EFh
dtoff_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc MiniVDD_DispiTrapOff

;-----------------------------------------------------------------------------
; 【r151】MiniVDD_OwnClass —— 把一个 32 位 VM 句柄分类。
;   入口: EAX = 句柄。出口: AL = 分类码(VBE_TR_OWN_CLS_*)。**只改 AL**。
;   【为什么必须比 32 位】r150 只记了句柄低字, 而 ./150 的日志里同时出现
;   C2920074(Windows VM)与 D5520074(DOS 盒子 VM) —— 低字相同。低字比较
;   什么也证明不了。
;   【为什么先测 0】OW_BOXVM / WinVMHandle 在早期可能是 0, 不先排除就会把
;   "空句柄"误判成某个 VM。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_OwnClass
        test    eax, eax
        jz      short mooc_none
        cmp     eax, dword ptr [WinVMHandle]
        je      short mooc_win
        cmp     eax, dword ptr [OwnWord + OW_BOXVM]
        je      short mooc_box
        cmp     eax, dword ptr [OwnWord + OW_PLANARVM]
        je      short mooc_planar
        cmp     eax, dword ptr [OwnWord + OW_MSGVM]
        je      short mooc_msg
        mov     al, VBE_TR_OWN_CLS_OTHER
        ret
mooc_none:
        mov     al, VBE_TR_OWN_CLS_NONE
        ret
mooc_win:
        mov     al, VBE_TR_OWN_CLS_WIN
        ret
mooc_box:
        mov     al, VBE_TR_OWN_CLS_BOX
        ret
mooc_planar:
        inc     byte ptr [OwnWord + OW_PLANARN]
        mov     al, VBE_TR_OWN_CLS_OTHER
        ret
mooc_msg:
        inc     byte ptr [OwnWord + OW_MSGN]
        mov     al, VBE_TR_OWN_CLS_OTHER
        ret
EndProc MiniVDD_OwnClass

;-----------------------------------------------------------------------------
; 【r151】TraceStub_8 —— SAVE_REGISTERS(8), 手写(原来是 TraceStub 宏)
;   DDK 原文: "EBX: Contains the VM handle to save. EBP: May or may not be
;   pointing the VM's client registers. The mini-VDD should save any register
;   states that it needs to in the CB data area for the VM in EBX."
;   本轮【只记账不写硬件】: 151 次/场的调用里改行为太贵, 而且我们还没有
;   每 VM 的 CB 数据区(见 vbeshare.h 长注 6)。EBX 的分类就是读数。
;   r151-marker: hand-written TraceStub_8 (records the saved VM's class).
;-----------------------------------------------------------------------------
BeginProc TraceStub_8
        pushfd
        push    eax
        mov     eax, ebx
        call    MiniVDD_OwnClass
        mov     byte ptr [OwnWord + OW_SAVECLS], al
        pop     eax
        popfd
        mov     eax, 8
        call    MiniVDD_TraceRecord
        cmp     dword ptr [OldDispatch + 8*4], 0
        je      short ts8_nc
        jmp     dword ptr [OldDispatch + 8*4]
ts8_nc:
        VDD_NC
        ret
EndProc TraceStub_8

;-----------------------------------------------------------------------------
; 【r151】TraceStub_9 —— RESTORE_REGISTERS(9), 手写(原来是 TraceStub 宏)
;   DDK 原文: "EBX: Is undefined but must be saved over this call.
;              ESI: Contains the VM handle for the MemC owner VM.
;              ECX: Contains the VM handle for the CRTC owner VM.
;              EBP: May or may not be pointing to the VM's client registers."
;   而《Virtual Display Minidriver》解释了这两个所有者为什么是判据级的:
;   "the screen looks like it is running in Windows hi-res packed pixel mode
;    from the user's perspective, but the CPU sees the video memory as a 4
;    plane VGA. Therefore, you can restore those states from the CRTC owner
;    that preserve the appearance of the screen while restoring those states
;    from the MemC owner that control how the CPU sees the video memory."
;   ⇒ "画面对不对"与"CPU 看到的内存对不对"是【两个不同所有者】的事。
;   本轮把这两个句柄变成读数(670 次/场, 每次两条 cmp 与几个 inc)。
;   【r151 当时"不写硬件"的理由, 与 r161 为什么现在开始写】
;   r151 写下的是: 在拿到"谁是谁"之前按所有者恢复寄存器等于又一次盲改,
;   而 ./148 的灰屏已经证明盲改的代价。那条纪律在 r161 仍然成立 —— 所以
;   本轮【只写六字节里那五个"取指/平面"字节 + AR10】, 而且只在两条判据
;   同时成立时写:
;     ① VgaFixArm != 0  —— 4/7 号这一对的定义就是 DDK 的 "whenever it
;        changes the video focus between Windows and a full-screen MS-DOS
;        prompt" ⇒ 武装期【就是】全屏 DOS 会话, 而全屏会话里 CRTC owner
;        与 MemC owner 是同一个 VM(DDK 原话), 两侧要的是同一套状态;
;     ② 五个不碰 ATC 的字节与文本目标逐个比较【不相等】—— 判据就是
;        r147/r153 那一条(六字节比较), 它自己会停下来, 不需要预算。
;   窗口化会话(4 号不响 ⇒ 武装为 0)一个字节都不动, 于是 ./148 那种"拆掉
;   主 VDD 自己搭好的 4 平面虚拟化台子"的事不会重演。
;   【现场纪律】EBX 必须原样交还; EAX/EDX/EDI 自己保存恢复。
;   r151-marker: hand-written TraceStub_9 (records CRTC/MemC owner classes).
;
;   【r161 —— 为什么这一条从 jmp 改成 call】
;   9 号是 DDK 里唯一"主 VDD 正在恢复寄存器状态"的官方事件, 而
;   《Virtual Display Minidriver》的 CRTC owner / MemC owner 一条把这个动作
;   分成两半: "you can restore those states from the CRTC owner that preserve
;   the appearance of the screen while restoring those states from the MemC
;   owner that control how the CPU sees the video memory."
;   ⇒ "CPU 怎么看到显存"那半边的落点【就在这一拍之后】—— 写在转交【之前】
;     会被主 VDD 自己的恢复盖掉(5 号/7 号那两条"我们的写是最后一手"的理由
;     在这里不成立: 那两条的原处理函数不写这组寄存器, 这一条恰恰是写它的)。
;   所以本桩改成 call 转交 -> 转交函数返回后再跑 VgaMemCHold9 -> 原样 ret。
;   返回值约定(CF)在 call/ret 之间全程保留: VgaMemCHold9 自己 pushfd/popfd,
;   而 ret 不动标志位 —— 主 VDD 拿到的仍然是它自己那个处理函数的回答。
;   r161-marker: chain by call, then assert the MemC half after the restore.
;-----------------------------------------------------------------------------
BeginProc TraceStub_9
        pushfd
        push    eax
        push    edx
        push    edi
        mov     eax, ecx                        ; ECX = CRTC 所有者
        call    MiniVDD_OwnClass
        mov     dl, al
        mov     byte ptr [OwnWord + OW_CRTCCLS], dl
        mov     eax, esi                        ; ESI = MemC 所有者
        call    MiniVDD_OwnClass
        mov     byte ptr [OwnWord + OW_MEMCCLS], al
        shl     dl, 4                           ; 高半字节 = CRTC, 低 = MemC
        or      dl, al
        cmp     dl, byte ptr [OwnWord + OW_LASTPAIR]
        je      short ts9_nopair
        mov     byte ptr [OwnWord + OW_LASTPAIR], dl
        inc     byte ptr [OwnWord + OW_PAIRN]
ts9_nopair:
        pop     edi
        pop     edx
        pop     eax
        popfd
        mov     eax, 9
        call    MiniVDD_TraceRecord
        cmp     dword ptr [OldDispatch + 9*4], 0
        je      short ts9_nc
        call    dword ptr [OldDispatch + 9*4]   ; 【r161】转交(原来是 jmp)
        call    VgaMemCHold9                    ; 【r161】官方恢复【之后】补 MemC 半边
        ret                                     ; CF 是转交函数的回答, 原样交还
ts9_nc:
        ; --- 【r167】没有可转交的原处理函数时, 【也要】跑这一笔 -------------
        ;   【这一处是本轮最确凿的一个 bug】./166 的日志同时给了两个数:
        ;     `TRC cnt f=09 RESTORE_REGISTERS n=00000233`(9 号被调了 563 次)
        ;     `PH: probe s9n == 0000`(整场、每一拍都是 0)
        ;   而 S9N 是 VgaMemCHold9 的【第一条指令】。两个数并存只有一个解释:
        ;   每一次调用都走 `OldDispatch[9] == 0` 这条分支, 于是 r161 起写的
        ;   整条官方修复(六字节/几何判据/所有者核对)【一次都没有执行过】——
        ;   六轮的努力全部落在这个 je 上。
        ;   【为什么主 VDD 那一格是 0】8/9 号是"卡自己的寄存器存取", DDK 把
        ;   它整个划给 mini-VDD(见 About Virtual Display Minidrivers 的
        ;   CRTC owner / MemC owner 一段), 主 VDD 没有默认处理函数。对一个
        ;   通用 VBE 驱动来说, 这一格本来就该由我们来填 —— 我们却一直在
        ;   "转交给一个空指针然后跳过自己的补写"。
        ;   【返回值不动】NC 仍然是"未处理", 与 r161..r166 逐字节一致: 本轮
        ;   只让修复真的跑起来, 不改变主 VDD 看到的语义。
        call    VgaMemCHold9                    ; r167: 没有可转交的原处理函数也要补
        VDD_NC
        ret
EndProc TraceStub_9

;-----------------------------------------------------------------------------
; TraceStub_4 —— PRE_HIRES_TO_VGA(4)
;   与 TraceStubState 4 宏展开逐条等价(记录 + 事件计数 + 置 VGA 位 +
;   保留位掩码 0FFh), 唯一差别是在转交原处理函数【之前】关掉 1CEh/1CFh
;   的全局陷阱: 紧接着主 VDD 就会把屏幕交给 DOS VM(它要自己设模式),
;   那时 DOS VM 就是 CRTC 所有者, 但交接瞬间的记账未必已经翻过来 ——
;   参考实现(boxv9x)正是在这个钩子里 Disable_Global_Trapping。
;   r116-marker: hand-written TraceStub_4 (was TraceStubState 4).
;-----------------------------------------------------------------------------
BeginProc TraceStub_4
        ; 【r412】方案 A 的"切屏时降级"已【退役】—— 实测见下面 RETIRED 段。
        ; 【r412】r236 的读数判决了这一条路:
        ;   `PH: r412 i10n==0000` —— 盒子每一条 INT 10h 都【没有】进这个钩子,
        ;   一次都没有。钩子是死的, 因此它不可能是 r235/r236 症状的原因,
        ;   也不可能看见主 VDD 第 6 步。⇒ 不再安装(过程体保留, 供日后核对)。
        ;   DDK 的 Hook_V86_Int_Chain 只对"走 V86 中断链"的中断生效;
        ;   本机的 INT 10h 由主 VDD 直接虚拟化, 不进这条链。
        pushfd
        ; 【r151】入口 EBX = DDK《PRE_HIRES_TO_VGA》原文的
        ;   "the handle of the VM that is being switched to" = 盒子 VM。
        ;   放在最前面取: 此刻它一定还是调用者的原值。
        mov     dword ptr [OwnWord + OW_BOXVM], ebx
        push    eax
        mov     eax, 4
        call    MiniVDD_TraceRecord
        inc     byte ptr [TraceEvt + VBE_TR_EVT_PRE_HIRES_TO_VGA]
        or      byte ptr [TraceState], VBE_TR_ST_VGA
        pop     eax
        popfd
        call    MiniVDD_DispiTrapOff
        ; --- 【r143】DDK 时序第 4 步就在这里 -----------------------------
        ;   PRE_HIRES_TO_VGA 页原文(第 4 步):
        ;     "The Main VDD calls the function PRE_HIRES_TO_VGA in the
        ;      mini-VDD. The mini-VDD may if it needs to (see below) set the
        ;      screen out of hi-res mode."
        ;   而第 6 步紧接着说:
        ;     "If this is sufficient to get the hardware out of hi-res mode
        ;      and back to a standard VGA mode, then the mini-VDD need not
        ;      explicitly set the screen into VGA mode (see step 4 above)."
        ;   ⇒ "把屏带出 hi-res"这件事, 文档【明确划给】4 号这一拍; 而这
        ;     也是"去 DOS"方向上唯一会跑到我们代码的时刻(全屏期间 ring 3
        ;     一个 DDI 都进不来)。r114 的 文本态修补 本来只挂在 5 号上,
        ;     等于把这一拍空着。现在两拍都做, 幂等, 不引入新寄存器。
        ;   此时第 3 步已关掉 3B0h-3DFh 的陷阱(所以我们 ring 0 的 out 一定
        ;   落到真硬件), 而本函数只写取指/平面那一组, 绝不碰任何时序寄存器。
        ; 【r146】先把"我们的那一份模式/寻址字节"存下来 —— 必须排在
        ;   文本态修补 【之前】: 那个函数正是把文本值写进同一批寄存器的。
        call    HiresSave
        ; 【r412】在我们任何一笔写之前, 先把客户机的 SR03 存下来
        xor     al, al
        call    Sr3Keep
        ; 【r412】先把卡"冷冷地交还 VGA": 两个 VBE 窗口 bank 归零 + 用盒子自己的
        ;   模式走一次非线性的 VBE 设模式。一个 VGA 寄存器都不写(见 冷启动 VGA 复位)。
        ; 【r412】这一枪回来了, 但只认一个模式号: 03h。
        ;   ./213/./214(PRE 设 72h)→ GUI 截片冻屏; ./215(PRE 不开火)→ 回到
        ;   字符花屏。两头都不对, 而用户手打 `mode co80`(= INT 10h
        ;   AX=0003)是唯一被证实正确的状态 ⇒ 就给它这个状态。
        ; 【r412】PRE 那一枪又关了(见下面那段判据)。
        ;   (r216 在这里 `call 冷启动 VGA 复位`)
        ; 【r412】★★ 本轮唯一的行为改动: 让盒子自己的 BIOS 就在【这一拍】设一次 mode 3 ★★
        ;   为什么是这一拍、为什么不是 5 号或 17 号, 见 R199_DELIVERY.md。
        ;   顺序: 先读一次 CR13/CR01(纯端口读, 进门时是 hi-res 的几何), 再开这一枪,
        ;   再读同一对 —— 于是"这一枪有没有让卡真的进 mode 3"第一次有了硬件读数,
        ;   而不是只看 BIOS 的返回值(./197 就是被那个返回值骗了一轮)。
        mov     dx, 03D4h
        mov     al, 013h
        out     dx, al
        inc     dx
        in      al, dx
        mov     ah, al
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [Probe + VBE_TR_PR_R48], ah       ; 进门时的 CR13
        mov     [Probe + VBE_TR_PR_R48 + 1], al   ; 进门时的 CR01
        ; 【r412】这一枪【摘掉】—— ./199 的 `crt=284F / crt2=284F` 说得很清楚:
        ;   开枪前后 CRTC 一模一样(而且进门读到的就已经是 284F: 冷启动 VGA 复位 已经把卡
        ;   带回文本几何了)。于是"让盒子 BIOS 设一次模式"三条边全部判死:
        ;     5 号(POST, ./197)  bios3=1 而画面不变;
        ;     17 号(会话内, ./198) 这一拍在全屏期根本不会被调到;
        ;     4 号(PRE, ./199)  有硬件读数: crt == crt2。
        ;   本版不再尝试任何"让 BIOS 设模式"的变体, 改用一个能一眼判定的对照实验
        ;   (见 字模块探测): 把字符 20h 的字模写成实心块, 看屏幕变不变。
        ;   (历史: r199 在这里 `mov byte ptr [Bios3N], 0` / `call BIOS 状态回放`。)
        mov     dx, 03D4h
        mov     al, 013h
        out     dx, al
        inc     dx
        in      al, dx
        mov     ah, al
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [Probe + VBE_TR_PR_R48 + 2], ah   ; 开完枪之后的 CR13
        mov     [Probe + VBE_TR_PR_R48 + 3], al   ; 开完枪之后的 CR01
        ; 【r412】这一拍【不写任何寄存器】。依据: 同一台机器上用标准 VGA 驱动
        ;   (没有 mini-VDD)全屏文本是完好的(./188/1881.png), 而我们这十几轮
        ;   里唯一的"额外动作"就是在这一拍/下一拍往卡上写字。先把写字全停掉,
        ;   让主 VDD 的官方第 6 步成为唯一动硬件的人 —— 这就是 DDK 的默认路径。
        ;   原: cmp [VbeState.VBS_policyVgaFix],0 / je ts4_arm / call 文本态修补
ts4_arm:
        mov     byte ptr [FboxLiveTick], 0Fh    ; r146: 下一次 35 号就采第一帧
        ; 【r176】本场的孔径修正从头计(见 盒内窗口映射)
        mov     byte ptr [FontProvN], 0
        mov     byte ptr [FontProvStat], 0
; r412: LiveDone reset removed -- 持屏期现场修补 is gone
        ; 【r412】会话中期那一枪的预算与重入位, 每场重来(见 模式设置插入)。
        mov     byte ptr [Bios3SN], 0
        mov     byte ptr [Bios3SStat], 0
        mov     byte ptr [Bios3Busy], 0
        ; 【r412】实心块对照实验也是每场一次(见 字模块探测)。
        mov     byte ptr [FontBlockN], 0
        mov     byte ptr [FontBankN], 0
; r412: mov     byte ptr [ApertureN], 0 -- the procedure that set it is gone
        mov     byte ptr [VbeWinN], 0
        mov     byte ptr [VbeWinOk], 0
        mov     byte ptr [FontBN], 0
        mov     byte ptr [BiosFontStat], 0
        mov     byte ptr [BiosFontReq], 0
        mov     byte ptr [VgaFixArm], 1         ; 盒子持屏: 允许持屏期补写
        mov     byte ptr [FboxWord + VBE_TR_FBOX_DISARM], 1
        ; 【r147】本场会话的"35 号第一拍"从这里开始重新计:
        ;   它保证每一场全屏会话都留一份"主 VDD 刚把盒子模式设完"的现场
        ;   (存进 VBE_TR_PS_P35FIRST 槽), 不会被后续几百次活采样覆盖。
        ;   r143..r146 在这里放的 `mov word ptr [VgaFixN], 0040h`(补写 64 次
        ;   的预算)已删除 —— 见 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 的头注: 判据换成逐字节比较之后
        ;   它自己会停下来, 不需要任何总量上限。
        mov     byte ptr [FboxFirstDone], 0

        ; 【r175】本场的 R0 取证也从这里重新开始:
        ;   先在 0:449 那一格立哨兵(0FFh = 本场还没采到),
        ;   再把"已采过"清零 ⇒ 下一次 35 号就是本场第一拍。
        mov     byte ptr [OwnWord + OW_BDA449], 0FFh
        mov     byte ptr [OwBdaDone], 0
        ; 【r152】每一场全屏会话给取指基址修复一次全新的机会: 上一场
        ;   放弃过(比如当时控制台数据还没进显存)不代表这一场也不行。
        ;   FIXN/FIXBAD 保留 —— 它们是跨场累计的证据。PrSaved 清零则
        ;   保证每场都重新记一次"原始 CR0C/CR0D", 还原时不会写回旧场值。
        mov     byte ptr [Probe + VBE_TR_PR_LATCH], 0
        mov     byte ptr [PrSaved], 0
        ; 【r153】写预算与失败计数也按【场】重来 —— 见 VgaFetchFix 的头注:
        ;   预算全场通用而 LATCH 每场清零是一种"看起来全新、其实已经用完"
        ;   的静默停止。FIXOK(成功次数)是世界状态, 故意不清。
        mov     byte ptr [Probe + VBE_TR_PR_FIXN], 0
        mov     byte ptr [Probe + VBE_TR_PR_FIXBAD], 0
        ; 【r153】显存扫描与"取指可达性"也每场重来: 上一场的映射可能完全
        ;   不同(窗口大小都不一样), 沿用旧结论等于拿上一场的坐标读这一场。
        ;   AGREE 不复位: 它是"上一拍的事实", 本场第一拍就会重算, 而留着
        ;   旧值比留 0 更诚实(0 会被读成"已经证明不相等")。
        mov     byte ptr [Probe + VBE_TR_PR_SCAND], 0
        mov     byte ptr [Probe + VBE_TR_PR_SCANN], 0
        mov     byte ptr [Probe + VBE_TR_PR_TOPOFF], 0
        mov     byte ptr [Probe + VBE_TR_PR_TOPVAL], 0
        mov     byte ptr [ScanBestVal], 0
        mov     byte ptr [ScanBestIdx], 0
        mov     byte ptr [ScanN], 0
        ; 【r155】默认档那一笔(映射不解码 B8000 时补文本孔径组)也按【场】重来。
        ;   这是 r154 最贵的一处偏差: TGONCE 的注释写着"本场只补一次",
        ;   而 4 号【从来没有】复位它 ⇒ 真实语义是"整个开机只补一次"。
        ;   ./154 的日志把差别变成了数字: 一场里 seenText 8→83、dirty 4→34
        ;   而 fixedN 恒为 1 —— 第一场补过之后, 后面三场一次都没再动手,
        ;   用户看到的正是"花屏和以前一模一样"。
        ;   TGTRIED/TGOK/TGBAD/TGREV 【不清】: 它们是跨场累计的证据
        ;   (与 FIXN/FIXBAD 的"按场重来"、FIXOK 的"累计"同一套纪律)。
        mov     byte ptr [Probe + VBE_TR_PR_TGONCE], 0
        mov     byte ptr [Probe + VBE_TR_PR_TGTRY], 0
        mov     byte ptr [Probe + VBE_TR_PR_TGRUN], 0
        mov     byte ptr [Probe + VBE_TR_PR_TGWATCH], 0
        mov     byte ptr [Probe + VBE_TR_PR_TGRUNMAX], 0
        ; 【r170】字模预算恢复成 0(= 本场可用 4 次)。这是对 r169 的一条回退:
        ;   r169 把这里写成 4, 等于整场会话一次都不重装; 结果 ./169 里全屏
        ;   花屏一个字都没变, 而【唯一有效的自救路径】反而坏了: 手动
        ;   `mode co80` 之后画面变成全黑 + 一个光标块(1691.png), 一个字符都
        ;   没有。把这两件事放在一起, 只剩一个解释 ——
        ;     这台机器上, 平面 2 的 8x16 字模不是 BIOS 装好的, 是我们自己装
        ;     的。手动 `mode co80` 历来"能救画面", 一半是 BIOS 重设模式, 另一
        ;     半正是本过程后面那次 VfcLoad 又把字模写进平面 2; r169 把后者关
        ;     掉之后, 同一个动作就只剩下"清屏 + 光标"。
        ;   所以本轮把 4 次预算还回来, 并且在 5 号(POST_HIRES_TO_VGA)【开场
        ;   就先装一次】—— 判据与经过见 TraceStub_5 的 r170 注。
        mov     byte ptr [FontN], 0
        ; 【r167】把【日志里那个】计数也一起清零 —— 否则 `PH: fbox fontn=`
        ;   数的是"开机以来重装了几次", 而预算(FontN)是"本场最多 4 次":
        ;   ./166 读到 fontn=0007 就判"预算没生效", 而那个数根本回答不了
        ;   这个问题(HiresSpare 从来没有被复位过)。两个数从此同源同场。
        mov     byte ptr [HiresSpare + HS_FONTN], 0
        ; 【r168】HS_FONTBAD 用同一个口径清零。./167 的 `fontbad=0003 fontn=0003`
        ;   之所以读不出结论, 正是因为它把【开机累计】的坏次数与【本场】的
        ;   重装次数放在一起比 —— 与 r167 修 fontn 时踩的是同一个坑, 只是
        ;   那一次只修了一半。两个数从此都按场。
        mov     byte ptr [HiresSpare + HS_FONTBAD], 0
        ; 【r168】整块自证的读数与"让 BIOS 重设 mode 3"的预算也按场重来。
        mov     byte ptr [MemC + VBE_TR_MC_FONTBADG], 0
        mov     byte ptr [Bios3N], 0
        mov     byte ptr [KbdDone], 0
        mov     byte ptr [Sim10N], 0
        mov     byte ptr [PalDone], 0
        mov     byte ptr [FontTopN], 0
        mov     byte ptr [FontBadW], 0
        ; 【r171】PRE 那一枪的每场预算与读数归零。I10MAP 那一格从 r170 起就是
        ;   空的(48 号改写已撤), 本轮把它当作 PreM3Stat 的落盘格 —— 清零放在
        ;   这里, 于是日志里 `PH: fbox prem3=` 读到的永远是【本场】那一枪的
        ;   结果, 不是上一次开机的残留。
        mov     byte ptr [PreM3N], 0
        mov     byte ptr [PreM3Busy], 0
        mov     byte ptr [FboxWord + VBE_TR_FBOX_I10MAP], 0
        ; 【r155】A0000 活动探针也按场重来(第一拍只当基准, 不计一次"动过")
        mov     byte ptr [AaSeen], 0
        mov     byte ptr [MemcWrote], 0
        ; 【r161】9 号那条路的两个"本场"读数按场重来(S9FIX/S9BAD 是累计证据,
        ;   故意不清); 两个一次性标志也顺手归零, 免得上一场的尾巴影响本场。
        mov     byte ptr [Probe + VBE_TR_PR_S9N], 0
        mov     byte ptr [Probe + VBE_TR_PR_S9OK], 0
        mov     byte ptr [Probe + VBE_TR_PR_S9SAME], 0
        mov     byte ptr [Probe + VBE_TR_PR_S9DIFF], 0
        mov     byte ptr [MemCPush], 0
        mov     byte ptr [MemCSrc], 0
        ; 【r171】PRE 那一枪 —— 放在【所有每场复位之后、转交主 VDD 之前】。
        ;   位置的三条理由:
        ;     ① 必须在 HiresSave 之后: 那一格存的是"hi-res 的样子", 用来在
        ;        7 号把桌面写回去; 先设 mode 3 再存, 存下来的就是文本值。
        ;     ② 必须在每场复位之后: 它要用的预算(PreM3N)与落盘格(I10MAP)
        ;        刚刚被清零。
        ;     ③ 必须在 文本态修补 之后: 这一枪会让 BIOS 自己写一遍整组文本
        ;        寄存器, 我们的补写只是"再来一遍", 顺序无关但省得互相干扰。
        ;   失败一律不改画面: 拿不到 IVT、BIOS 回 AH!=0、盒子的模式不是
        ;   80 列文本 —— 三种情况都只是记一个数字然后走人。
        ; 【r412】预设置模式那一枪也停: 主 VDD 第 6 步自己会按盒子模式设一次,
        ;   而我们在同一拍再设一次(而且还是我们自己挑的模式号)是多余的动作。
        cmp     dword ptr [OldDispatch + 4*4], 0
        je      short tst_null_4
        jmp     dword ptr [OldDispatch + 4*4]
tst_null_4:
        VDD_NC
        ret
EndProc TraceStub_4

;-----------------------------------------------------------------------------
; TraceStub_13 / TraceStub_14 —— ENABLE_TRAPS(13) / DISABLE_TRAPS(14)
;   与 TraceStubState 13/14 宏展开逐条等价(13: 置 TRAPS 位; 14: 清 TRAPS
;   位; 两者都不计事件)。
;   【r117: 13 号开我们自己的陷阱, 14 号【不】关 —— 与参考实现逐条一致】
;   boxvmini.asm 只挂 4 个分派项(PRE_HIRES_TO_VGA / POST_HIRES_TO_VGA /
;   ENABLE_TRAPS / DISPLAY_DRIVER_DISABLING), 其中开陷阱的是 13 号与 5 号,
;   关陷阱的是 4 号与 26 号 —— 【没有 14 号】。语义上也说得通: 14 号关的是
;   主 VDD 自己那套标准 VGA 端口陷阱; 而"CRTC 属于 Windows 且发起者不是
;   Windows ⇒ 吃掉 DISPI 写"这个判据在任何时刻都成立(全屏 DOS 会话里
;   CRTC 属于 DOS VM, 我们的 handler 本来就原样放行)。r116 在这里也关了
;   一次, 于是 14 号一响保护就消失一段 —— 第三处偏差, 撤掉。
;   r116-marker: hand-written TraceStub_13/14 (Global_Trapping mirror).
;   r117-marker: TraceStub_14 no longer calls MiniVDD_DispiTrapOff (mirror boxvmini).
;-----------------------------------------------------------------------------
BeginProc TraceStub_13
        pushfd
        push    eax
        mov     eax, 13
        call    MiniVDD_TraceRecord
        or      byte ptr [TraceState], VBE_TR_ST_TRAPS
        pop     eax
        popfd
        call    MiniVDD_DispiTrapOn
        cmp     dword ptr [OldDispatch + 13*4], 0
        je      short tst_null_13
        jmp     dword ptr [OldDispatch + 13*4]
tst_null_13:
        VDD_NC
        ret
EndProc TraceStub_13

BeginProc TraceStub_14
        pushfd
        push    eax
        mov     eax, 14
        call    MiniVDD_TraceRecord
        and     byte ptr [TraceState], 0FDh
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + 14*4], 0
        je      short tst_null_14
        jmp     dword ptr [OldDispatch + 14*4]
tst_null_14:
        VDD_NC
        ret
EndProc TraceStub_14


;-----------------------------------------------------------------------------
; 【r157】TraceStub_17 / _19 / _21 —— VIRTUALIZE_CRTC_OUT(0x11) /
;   VIRTUALIZE_SEQUENCER_OUT(0x13) / VIRTUALIZE_GCR_OUT(0x15)。
;   与 TraceStub 宏展开逐条等价(pushfd/push eax -> 记一条 -> 复原 ->
;   转交主 VDD 原处理函数, 没有原函数就 VDD_NC), 唯一的差别是【在记录之前】
;   按寄存器下标把次数记进 blob。
;
;   【为什么要这三个计数器】
;   ① `TRC cnt f=..` 的绝对值已经证明 VM 的端口写会被路由到我们这里
;      (./154: CRTC_OUT 0x7D2=2002、SEQ_OUT 0x60=96、GCR_OUT 0x6A=106),
;      而 `TRC n=` 环形记录只有 64 条、./154 丢了 825 条 —— 于是
;      **GCR_OUT 的 106 次里, 环形里一条都没剩下**, 我因此在上一轮
;      错判成"VM 根本没写 GCR"。计数器不受环形丢弃影响。
;   ② 全项目的核心悬案是"会话期间是谁把 SR02/SR04/GCR05/GCR06 摆成
;      把 B8000 挡在解码范围外的那一组"。日志能证明的只有硬件状态
;      (f35/l35 采样), 不能证明作者。逐寄存器计数第一次把"作者"变成
;      可回答的问题: 若 GCR06 的计数在会话期间增长, 那就是 VM 自己写的;
;      若纹丝不动, 那就是 ring 0 的主 VDD 写的。
;   ③ 口径必须写清楚(否则又是一条"含义是假的"读数): 它数的是【被路由到
;      我们这里的】写。全屏会话里主 VDD 也可能把 VM 的写直接放行(那时
;      计数不动) —— 所以"计数为 0"是弱证据, "计数增长"是强证据。
;
;   【编码依据】EAX = (数据字节 << 8) | 寄存器下标, EDX = 端口。
;      由 r153 §1.8 从日志逐条核对得出(0x0E06 = GCR06<-0Eh, 0x1005 =
;      GCR05<-10h, 0x0302 = SR02<-03h, 0x0219 = SR04<-02h), 并在本轮用
;      `TRC cnt f=` 的绝对值复核了家族归属。
;   【现场纪律】EAX/EDX 自己保存恢复(EAX 在记录里当功能号用, 不能带着
;      被我们改过的 AH 去记录); EBP/EBX/ESI/EDI 一律不碰。
;   r157-marker: per-register write counters in the three VIRTUALIZE_*_OUT stubs.
;
;   【r159 修正 —— r157 这三条手写桩漏掉的一步, 也是 ./158 退化的原因】
;   宏的顺序是: pushfd / push eax / mov eax,fn / call 记录 / pop eax / popfd /
;   转交。r157 手写这三条时, 把 mov eax,fn 与 call 记录挪到了 pop eax 与
;   popfd 【之后】, 于是转交那一刻 EAX = 功能号 17/19/21, 而不是客户端
;   "要输出到端口的值"。
;   官方依据 —— Win95 DDK 的四页逐字核对(本机可用同样的四页复核):
;     VIRTUALIZE_CRTC_IN(16, S28D1) / VIRTUALIZE_CRTC_OUT(17, S28D2) /
;     VIRTUALIZE_SEQUENCER_OUT(19, S28D8) / VIRTUALIZE_GCR_OUT(21, S28D6)。
;   17/19/21 三页各自只有一句正文:
;     "The parameters and return values for this function are the same as for
;      VIRTUALIZE_CRTC_IN. See VIRTUALIZE_CRTC_IN for details."
;   而 16 号页写的是:
;     ① Call With: "EAX: Contains the value to output to port (on an OUT call)."
;     ② Return Values: "NC set indicates that the mini-VDD may have taken some
;        action but wants the Main VDD to do the default action.  In this case
;        the mini-VDD must not have destroyed ANY registers."
;   转交给 OldDispatch 走的正是 NC 那条语义(让主 VDD 做默认动作), 而主 VDD
;   的默认动作要拿 EAX 去写端口 —— 被换成功能号, 等于把每一次被路由到我们
;   这里的端口写都改成了一个错值(0x11/0x13/0x15)。
;   【为什么这不是"理论问题"】这三条桩在【使能期就会跑】。./154 里第一份
;   TRC cnt 时(它落在 Enable 内部, 在 PhysicalEnable 之后)的绝对值已经是
;   CRTC_OUT 87 次、SEQ_OUT 28 次、GCR_OUT 31 次; 同一份读数里还有
;   PRE_CRTC_MODE_CHANGE 1 次、POST_CRTC_MODE_CHANGE 1 次、PRE_INT_10_MODE_SET
;   2 次 —— 也就是"主 VDD 替我们设模式"的那一批寄存器写, 正是这 146 次里的
;   大头。于是: r154(宏桩)带着正确的值转交, 模式设置照常; r157/r158 带着
;   错值转交, 同一批写被改坏。./158 的另外两半证据: BOOTLOG 里第一次出现
;   vga.drv(Windows 判定 omninine.drv 不可用而回退; 132..158 十九轮里只有
;   ./158 有这一行), VBE9XLOG 只剩到 DrvInitCore 为止的 22 行 624 字节。
;   修法: 把 mov/call 挪回宏的位置(在 pop 之前)。功能号仍在 EAX 里进记录,
;   客户端现场在 pop 之后原样交还 —— 与"未挂钩"逐字节等价。
;   r159-marker: pass-through restores client EAX before chaining (DDK 17/19/21).
;-----------------------------------------------------------------------------
BeginProc TraceStub_19                      ; VIRTUALIZE_SEQUENCER_OUT(0x13)
        pushfd
        push    eax
        push    edx
        mov     ah, al                  ; AH = 寄存器下标(低字节)
        cmp     ah, 001h
        jne     short ts19_sr02
        inc     byte ptr [Probe + VBE_TR_PR_W_SR01]
        jmp     short ts19_rec
ts19_sr02:
        cmp     ah, 002h
        jne     short ts19_sr03
        inc     byte ptr [Probe + VBE_TR_PR_W_SR02]
        jmp     short ts19_rec
ts19_sr03:
        cmp     ah, 003h
        jne     short ts19_sr04
        inc     byte ptr [Probe + VBE_TR_PR_W_SR03]
        jmp     short ts19_rec
ts19_sr04:
        cmp     ah, 004h
        jne     short ts19_rec
        inc     byte ptr [Probe + VBE_TR_PR_W_SR04]
ts19_rec:
        mov     eax, 19                 ; 【r159】功能号只进记录
        call    MiniVDD_TraceRecord
        pop     edx                     ; 【r159】复原客户端现场后才转交
        pop     eax                     ;   EAX = 要输出到端口的值
        popfd
        cmp     dword ptr [OldDispatch + 19*4], 0
        je      short tst_null_19
        jmp     dword ptr [OldDispatch + 19*4]
tst_null_19:
        VDD_NC
        ret
EndProc TraceStub_19

BeginProc TraceStub_21                      ; VIRTUALIZE_GCR_OUT(0x15)
        pushfd
        push    eax
        push    edx
        mov     ah, al
        cmp     ah, 005h
        jne     short ts21_g06
        inc     byte ptr [Probe + VBE_TR_PR_W_GCR05]
        jmp     short ts21_rec
ts21_g06:
        cmp     ah, 006h
        jne     short ts21_rec
        inc     byte ptr [Probe + VBE_TR_PR_W_GCR06]
ts21_rec:
        mov     eax, 21                 ; 【r159】功能号只进记录
        call    MiniVDD_TraceRecord
        pop     edx                     ; 【r159】复原客户端现场后才转交
        pop     eax                     ;   EAX = 要输出到端口的值
        popfd
        cmp     dword ptr [OldDispatch + 21*4], 0
        je      short tst_null_21
        jmp     dword ptr [OldDispatch + 21*4]
tst_null_21:
        VDD_NC
        ret
EndProc TraceStub_21

;   17 号(CRTC_OUT)在 ./154 里被调了 2002 次, 绝大多数是 CR0E/CR0F 的
;   光标更新(盒子在打印)。这里只数三个与会话状态直接相关的:
;   CR0C/CR0D(显示起始地址)与 CR17(CRTC 模式控制: bit6 字/字节寻址)。
BeginProc TraceStub_17                      ; VIRTUALIZE_CRTC_OUT(0x11)
        pushfd
        push    eax
        push    edx
        mov     ah, al
        cmp     ah, 00Ch
        jne     short ts17_cr0d
        inc     byte ptr [Probe + VBE_TR_PR_W_CR0C]
        jmp     short ts17_rec
ts17_cr0d:
        cmp     ah, 00Dh
        jne     short ts17_cr17
        inc     byte ptr [Probe + VBE_TR_PR_W_CR0D]
        jmp     short ts17_rec
ts17_cr17:
        cmp     ah, 017h
        jne     short ts17_rec
        inc     byte ptr [Probe + VBE_TR_PR_W_CR17]
ts17_rec:
        mov     eax, 17                 ; 【r159】功能号只进记录
        call    MiniVDD_TraceRecord
        pop     edx                     ; 【r159】复原客户端现场后才转交
        pop     eax                     ;   EAX = 要输出到端口的值
        popfd
        ; 【r412】盒子持屏期间的现场采样 + 一次视图复写(见 持屏期现场修补 头注)。
        ;   放在这里是因为 17 号(CRTC_OUT)是全屏期间【确实会被调到】的钩子
        ;   (光标每动一次就来一次), 而 4/5/6/7 号都在切屏那一拍的两侧。
        ; 【r412】17 号这一枪【已摘掉】—— ./198 的日志证明了它不可能落地:
        ;   `PH: r412 insn=0001` 第一次出现是在 `MARK V2H`(盒子已经离开全屏)之后,
        ;   而整段持屏期里 `PH: fbox n35==0000`、`insn=0000` ⇒ 【盒子在全屏持屏期
        ;   根本不会被主 VDD 调到我们这里】(全屏 = 客户机直接访问硬件, 没有虚拟化
        ;   调用)。所以"会话中期由我们发一次"在这台机器上不是时机不好, 是没有时机。
        ;   `模式设置插入` 的本体保留(它是"17 号到底会不会被调到"的探针),
        ;   但不再从任何钩子里调用; 真正动手的是 4 号里那一枪(见 R199_DELIVERY.md)。
        ;   (历史: r198 在这里 `call 模式设置插入`。)
        cmp     dword ptr [OldDispatch + 17*4], 0
        je      short tst_null_17
        jmp     dword ptr [OldDispatch + 17*4]
tst_null_17:
        VDD_NC
        ret
EndProc TraceStub_17

;-----------------------------------------------------------------------------
; 【r163】TraceStub_15 —— MAKE_HARDWARE_NOT_BUSY(15), 手写(原来是 TraceStub 宏)。
;   为什么到这一版才动它 —— 它是官方文档里【唯一】一处把"主 VDD 正在切
;   硬件的 MemC 状态"写成事件的钩子, 而且一次交出三个所有者句柄。
;   DDK《MAKE_HARDWARE_NOT_BUSY (Function 15)》原文:
;     Call With:  EAX = CRTC 所有者的 VM 句柄
;                 EBX = 当前正在运行的 VM 句柄
;                 ECX = MemC 所有者的 VM 句柄
;                 EDX = CRTC 索引寄存器("which must be saved over call")
;     Remarks:    "If your hardware must complete a BLTer drawing operation
;                  before having its MemC state switched from Windows hi-res
;                  mode to VGA planar mode, then you must implement
;                  MAKE_HARDWARE_NOT_BUSY. ... This routine gives the mini-VDD
;                  notice that a switch of the MemC state will be occurring and
;                  allows it to delay that switch until the drawing engine has
;                  completed its work."
;   ⇒ ① 盒子持屏期间主 VDD【照样】会把硬件的 MemC 状态在"Windows hi-res"与
;        "VGA 平面态"之间来回切 —— 这正是 r154/r155 那条孔径补写一直在撞的
;        东西, 而这句话就是它的官方名字;
;     ② 三个句柄一起给出来, "会话中途那一下是谁的状态"第一次有了官方读数,
;        不必再靠 35 号的采样去猜(见 vbeshare.inc 的 VBE_TR_PR_OWN15 位序)。
;   本轮【只读取证】: 只数次数 + 记身份位图, 一个寄存器都不碰; EDX 按原文
;   要求保存过调用(与 17/19/21 三条"客户端现场原样交还"同一条纪律)。
;   位序: bit0 EAX==WinVM bit1 ECX==WinVM bit2 EBX==WinVM
;         bit3 EAX==BoxVM bit4 ECX==BoxVM bit5 EBX==BoxVM
;   r163-marker: hand-written TraceStub_15 (MAKE_HARDWARE_NOT_BUSY owners).
;-----------------------------------------------------------------------------
BeginProc TraceStub_15
        ; 【r412】已退役(实测无效且有害) —— 见下面 RETIRED(CO80) 段。
        ; 【r412】此处原有的 4F07h 动作已【退役】—— 依据见下面 RETIRED 段。
        ; 【r412】盒子运行期: 再取一次同一处的指纹, 用来比对"有没有写手"
        pushfd
        push    eax
        mov     byte ptr [PageWitMode], 2
        pop     eax
        popfd
        pushfd
        push    eax
        push    edx
        inc     byte ptr [Probe + VBE_TR_PR_N15]
        xor     dl, dl
        cmp     dword ptr [OwnWord + OW_BOXVM], 0
        je      short ts15_win
        cmp     eax, dword ptr [OwnWord + OW_BOXVM]
        jne     short ts15_bx1
        or      dl, 008h                        ; CRTC 所有者 = 盒子 VM
ts15_bx1:
        cmp     ecx, dword ptr [OwnWord + OW_BOXVM]
        jne     short ts15_win
        or      dl, 010h                        ; MemC 所有者 = 盒子 VM
ts15_win:
        cmp     dword ptr [WinVMHandle], 0
        je      short ts15_st
        cmp     eax, dword ptr [WinVMHandle]
        jne     short ts15_w1
        or      dl, 001h                        ; CRTC 所有者 = Windows VM
ts15_w1:
        cmp     ecx, dword ptr [WinVMHandle]
        jne     short ts15_w2
        or      dl, 002h                        ; MemC 所有者 = Windows VM
ts15_w2:
        cmp     ebx, dword ptr [WinVMHandle]
        jne     short ts15_st
        or      dl, 004h                        ; 当前 VM = Windows VM
ts15_st:
        mov     [Probe + VBE_TR_PR_OWN15], dl
        cmp     byte ptr [VgaFixArm], 0
        je      short ts15_rec
        mov     [Probe + VBE_TR_PR_OWN15A], dl
ts15_rec:
        mov     eax, 15
        call    MiniVDD_TraceRecord
        pop     edx
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + 15*4], 0
        je      short ts15_nc
        jmp     dword ptr [OldDispatch + 15*4]
ts15_nc:
        VDD_NC
        ret
EndProc TraceStub_15




; r412 D: 文本模式重做 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 重描武装 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;=============================================================================
; 【r412】方案 A —— 在 4 号(PRE_HIRES_TO_VGA)让卡离开 VBE 线性模式
;
; 【为什么在这里、为什么是这个模式】
;   DDK《PRE_HIRES_TO_VGA (Function 4)》第 4 步原文允许 mini-VDD 在这一拍
;   "set the screen out of hi-res mode", 并在同页写明: 若硬件不能靠
;   `INT 10h AH=0` 回到标准 VGA 模式, 就 "do whatever it takes to restore your
;   hardware to a standard VGA mode at this time"。
;   而我们 r249–r255 已把 VBE 一侧量完: 模式、七个 mode 3 寄存器、BIOS 字模装载、
;   显示起始 —— 没有一项是错的。剩下的差别只有【架构】:
;     我们是 VBE 线性帧缓冲桌面; 而 VDD 的全屏机制是围绕【银行式 VGA 桌面】
;     (64K A000 孔径) 设计的。同一台机器上标准 VGA 驱动全屏正常(./188 有图)。
;
; 【这一枪做什么】用 r249 证明可用的 `Exec_VxD_Int` 发 `INT 10h AX=0012h`
;   (640x480x16, 银行式平面模式)。这一枪会让卡的 BIOS 把显存访问方式与扫描源
;   从线性帧缓冲切回标准 VGA 孔径 —— 也就是"降级"。之后主 VDD 第 6 步再发
;   `INT 10h AH=0` 时, 它面对的已经是一个银行式 VGA 世界。
;
; 【为什么不用 4F02h 设桌面模式】r213/214 就是那样做的: 用被吞掉的 Exec_Int 发
;   VBE 设模式, 结果 GUI 冻屏。本次用的是【标准 VGA BIOS】的模式设置, 而且走的是
;   已经证明能拿到真实应答的原语。
;
; 【回程】不动。ResetHiResMode -> ApplyDesktopRestore -> VBE_SetMode 本来就在
;   ring 3 重设 VBE 桌面, 这条路径一直是好的。
;
; 【闸门】policyInt10Fix(Int10Fix, 默认 1) + 本场会话一次(4 号每场只响一次)。
; 【判据】`PH: va st=` —— b1 = 设模式回 AL=12h; b2 = 之后 4F03h 仍报非 VBE 模式;
;   b3 = 3CCh == 67h; b4 = 4F07h GET 有应答。画面本身是主判据。

; r412 D: 12h 模式进入 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r114-marker: hand-written TraceStub_5
BeginProc TraceStub_5
        ; 【r412】已退役(实测无效且有害) —— 见下面 RETIRED(CO80) 段。
        pushfd
        push    eax
        mov     eax, 5
        call    MiniVDD_TraceRecord
        or      byte ptr [TraceState], VBE_TR_ST_VGA
        pop     eax
        popfd
        ; --- 【r170】开场这一枪: 让卡自己的 BIOS 把 mode 3 真设一次 ----------
        ;   【为什么在这里、为什么是现在】
        ;     · DDK《PRE_HIRES_TO_VGA (Function 4)》的六步时序里, 第 6 步是
        ;       "The Main VDD executes an Interrupt 10h, function 0 mode change
        ;        to enter the mode that the VM is running in"; 第 7 步才是
        ;       POST_HIRES_TO_VGA。也就是说【我们此刻站的位置】正是"主 VDD
        ;       刚按盒子的模式号设完模式、盒子还没开始画"的那一拍 —— 这是
        ;       一个改动画面内容【不会丢任何东西】的唯一时刻(盒子随后才画)。
        ;     · 同一页末尾写明, mini-VDD 需要为了让硬件从 hi-res 回到 VGA 而
        ;       调 BIOS 时, 要"make sure that EBP points to client registers for
        ;       the Current VM ... Get_Cur_VM_Handle ... before making any calls
        ;       to the BIOS"。BIOS 状态回放 就是照这条写的(嵌套 V86 + Exec_Int),
        ;       而 r168 把它挂在"字模回读不成立"这个判据上 —— ./168 的日志
        ;       证明那个判据从来没成立过(`bios3` 全篇不出现, Bios3N 恒 0),
        ;       所以这条路【一次都没跑过】。本轮把它挂到开场这一拍上。
        ;     · 为什么值这一枪: 用户自己给出的、唯一反复有效的判决是
        ;       "全屏花屏时打 mode co80 就能救回来"。./169 的相位采样同时证明
        ;       硬件寄存器【已经是】标准 mode 3 文本值(SR04=02/GCR06=0E/SR01=00),
        ;       而画面依旧是花屏 ⇒ 病灶在标准 VGA 寄存器集【之外】的那些
        ;       "卡自己的"状态里(扩展寄存器/取指基址/VRAM 窗口与 bank 的对应
        ;       关系)。标准 VGA 的九组寄存器我们每家都写遍了, 而【只有卡自己的
        ;       BIOS 知道怎么把自己那部分关掉】—— 这正是 `mode co80` 做的事。
        ;   【预算】Bios3N, 每场会话 1 次(4 号清零), 闸门 policyInt10Fix。
        ;   【失败形态】Bios3Stat: 1 = 执行且 BIOS 回 AH=0; 2 = IVT[10h] 为 0;
;   3 = 执行了但 AH!=0; 4 = 策略关(policyInt10Fix=0); 5 = 本场预算已用完
        ;       没敢执行; 3 = 执行了但 AH!=0。它会进 blob 第 15 字节的高 2 位,
        ;       日志里读 `PH: fbox bios3=` 与 `PH: memcfix fontbadb=`。
        ; --- 【r181】先把"这一拍页面原来长什么样"记下来 -------------------
        ;   放在 BIOS 状态回放 之前: 那一枪是一次嵌套 V86 INT 10h, 它自己
        ;   也可能动页面(真跑了 BIOS 的话)。我们要的是【我们动手之前】的
        ;   那一份读数, 否则"页面进来就是坏的"这条判据会被自己搅浑。
        ;   同一过程里还取一份 4000 字节快照, 供 换页后置 原样写回。
        ; 【r184】进门第一件事: 把七个寄存器【读回来】。此刻我们一个字节都
        ;   还没写, 所以这 7 个字节就是"主 VDD 第 6 步执行完之后, 卡的真实
        ;   样子" —— 项目历史上第一次有这组数。
        mov     edi, OFFSET32 Probe
        add     edi, VBE_TR_PR_R5IN
        ; 【r412】盒子持屏期: 【一个字节都不写】。
        ;   r412-marker: hook 5 is NOT read-only -- it writes the three probes
        ;   below (font, CRTC geometry, the BIOS mode set) and nothing else,
        ;   plus (new in r412) the whole text palette AR00..AR0F + DAC 0..15
        ;   as its very last action.
        ;   去掉的七笔写: 换页前置(取页快照) / BIOS 状态回放(嵌套 BIOS 设模式) /
        ;   文本态修补(文本寄存器组) / VgaFontProv(平面 2 字模) / 换页后置(写回
        ;   快照并清页) / VgaMark(第 24 行标记串) / 文本尾部校验(再写一遍文本组并读回)。
        ;   依据: ./188/1881.png —— 同一台机器、同一个 DOS 框, 用标准 VGA 驱动时
        ;   全屏文本完全正常 ⇒ 卡和 BIOS 都能做, 唯一的差别就是"我们做了什么"。
        ;   于是这一版把"我们做了什么"清零, 只保留纯读的仪器(VfcReadGroup 在上,
        ;   FboxSnap 在下), 读寄存器不影响任何状态。
        ; 【r178】字模两条路都留着, 但结局的含义变了:
        ;   r177 把 BIOS 那一枪放在"我们自己拷贝失败"之后, 而那一枪
        ;   (AX=1122h)在参考实现里是空函数 ⇒ 它永远不可能被判为有效。
        ;   本轮换成 1104h/1114h(真正的字形装载), 并且它自带
        ;   get_font_access() 那一套寄存器 —— 与本轮 VfcLoad 对齐的是
        ;   同一段代码。完整判读见 VgaFontProv 头注与 R178_DELIVERY.md。
        ; 【r412】把【字模那一笔】放回来 —— 只放这一笔。
        ;   证据: ./191 的日志里 `PH: probe fontsr02=0000`。r412 新加的
        ;   "复位解除后再写 SR02" 那段在 VfcAccessOn 里, 而 VfcAccessOn 只有
        ;   字模那条路会调; r189 把 `call VgaFontProv` 也一起摘掉了 ⇒
        ;   r412 的新代码一次都没执行, 那一轮等于没测。
        ;   本版: 4 号仍然不写, 5 号只调 VgaFontProv(它自带 canary 与收尾),
        ;   其它六笔写(换页前置/BIOS 状态回放/文本态修补/换页后置/VgaMark/
        ;   文本尾部校验)仍然不调 —— 它们已经被 r189 证明与画面无关。
        call    VgaFontProv
        ; 【r412】★★ 本轮唯一的行为改动: 一张截图就能判定的对照实验 ★★
        ;   把字符 20h(空格)的 16 字节字模写成 0FFh(实心块), 然后【看屏幕】:
        ;     · 全屏的空白格变成实心块 ⇒ 我们在 5 号里写的平面 2 【就是】显示器
        ;       取指的那一块 ⇒ 花屏的病根在"我们写完之后又有谁把它盖掉";
        ;     · 空白格仍是那条虚线垃圾    ⇒ 我们写的平面 2 【不是】显示器取指的那一块
        ;       ⇒ 病根在内存窗口/虚拟化(下一轮做 ACCESS_VGA_MEMORY_MODE 与 bank),
        ;       【不再】在字模内容上花任何一轮。
        ;   它只写字符 20h 那 16 字节 + 紧接着的 16 字节 0(32 字节一格),
        ;   闸门与字模那条路完全相同(Int10Fix + 持屏中 + FontBlockOn), 每场只写一次。
        ; 【r412】字模装完, 再把【扫描几何】按标准 mode 3 装一遍。
        ;   依据: ./193 第一次证明字模这一侧已经做到位了 ——
        ;     fontrun=3, fontwin=1, fntreg=0407, fontsr02=0004, fontsr03=0000
        ;   (平面屏蔽 = 平面 2、字符映射 = 0 号段、SR04/GCR06 回读都对),
        ;   而画面一个像素都没变。同一份日志里能读到的"文本态"只有
        ;     p5sr04=02  p5gcr06=0E  p5sr01=00  p5ar10=0C
        ;   —— 即【显存视图】这一层是对的; 而 CRTC 的 25 个时序寄存器
        ;   (CR00..CR18) 从头到尾【没有一次读数】, 也从没被我们写过。
        ;   几何错正是"字符/属性/字模全对、屏幕却是一格格竖线"的形状。
        call    VgaM3Crtc
        ; 【r412】把同样的 ROM 字模写进平面 2 的【八个 8K 段】。
        ;   依据见 VfcFillBanks 的头注: ./184 的两张截图逐像素解出来之后,
        ;   "属性字节 bit3 = 1 的格子字形正确、bit3 = 0 的格子是同一张废物
        ;   字形" —— 而 FreeVGA 的 SR03 正是用 bit3 在 A/B 两个字符集之间
        ;   切换, 每个字段是【八个 8K 段】的 3 位下标。r177..r194 只填了
        ;   0 号段(0000h-1FFFh), 所以另一个字符集取指取到的是上一场
        ;   Windows 留下的像素 —— 屏幕上那片"9 像素一格的竖虚线"就是它。
        ; 【r412】把 r195 的"八段填字模"从调用链里摘掉(函数本体保留)。
        ;   理由: ./196 那次八段填过之后, 全屏的**非 ASCII 字符**全部变成空白
        ;   (1961.png 对比 1962.png) —— 远东 DBCS 文本模式的第二字符集字模就住在
        ;   1..7 号段, 被我们盖成了 8x16 ASCII 字模。这条路上它既没修好画面
        ;   (./195), 又有明确副作用, 所以退出调用链。
        ; ----------------------------------------------------------------
        ; 【r197】★★ 上一轮那一处 ★★ 让盒子自己的 BIOS 重设一次 mode 3。
        ;   【r412 把它挪到 17 号了】./197 的对照: 这一枪在这里执行了
        ;   (bios3=0001, BIOS 回 AH=0)而画面一个像素没变 ⇒ 5 号那一拍主 VDD
        ;   还在它自己的切屏事务【里面】, 它对自己的 INT 10h 虚拟处理器重入即
        ;   忽略。现在真正动手的是 TraceStub_17 里的 模式设置插入; 这一行保留
        ;   作"事务内 vs 事务外"的对照基准(两处各记一个结果)。
        ;
        ;   依据是 ./196 交回来的第一份**正面**证据: 全屏花屏时用户按 `mode co80`
        ;   (就是 `INT 10h AX=0003`)之后, 画面**恢复正常**(1961.png: dir 输出、
        ;   目录项、行尾对齐全对)。而同一场会话里, 主 VDD 的第 6 步已经在 hook 5
        ;   之前执行过一次 INT 10h AH=0 —— 也就是说: 同一个 BIOS 调用, 由主 VDD
        ;   在切屏事务里发一次**不够**, 由 VM 自己再发一次**够**。
        ;   差别只可能在"那一刻谁在持屏 / 端口陷阱在谁手里": 我们在 5 号里跑的是
        ;   DDK 时序的第 7 步(POST), 而 DDK 在这一步的正文里明确允许 mini-VDD
        ;   调 BIOS:
        ;     "If the mini-VDD needs to make calls to Exec_Int or Exec_VxD_Int in
        ;      order to set the hardware from hi-res mode back to VGA mode during a
        ;      PRE_HIRES_TO_VGA call, it should make sure that EBP points to client
        ;      registers for the Current VM ... you must make the call to the
        ;      function Get_Cur_VM_Handle and then establish EBP ... before making
        ;      any calls to the BIOS."
        ;   BIOS 状态回放 就是照这段写的(Get_Cur_VM_Handle → 查 IVT[10h] →
        ;   Push_Client_State → Begin_Nest_V86_Exec → AX=0003 → Exec_Int →
        ;   End_Nest_Exec → Pop_Client_State), 而且 ./176 的 `bios3=0001`
        ;   已经证明它在本机**真的执行到了 BIOS**(AH 回来 = 0)。
        ;   本轮把它放到 5 号的最后一笔(字模/CRTC 之后), 于是:
        ;     · 它跑成功 ⇒ 盒子拿到的是"BIOS 亲手设好的 mode 3", 与用户手打
        ;       `mode co80` 逐条相同 —— 这正是唯一被实测证明有效的动作;
        ;     · 它没跑(IVT=0)或 BIOS 回错 ⇒ `PH: fbox bios3=` 会告诉我们
        ;       (2 = 没有 IVT, 3 = 执行了但 AH!=0), 下一轮换路。
        ;   闸门: policyInt10Fix(默认 1) + 本场一次(Bios3N, 4 号清零)。
        ; 【r412】★★ POST 的最后一笔: 把"写路径"按标准 mode 3 收尾并读回 ★★
        ;   位置: 在 BIOS 状态回放(嵌套的 BIOS mode 3)之后、重开陷阱之前 ——
        ;   字模/彩色/几何/嵌套 BIOS 每一笔都先把寄存器摆成"字模窗口"再摆回来,
        ;   收尾必须是【我们最后一个写寄存器的地方】。判读见 VgaPostTail 头注。
        call    VgaPostTail
        ; 【r412】★★ 把 A 字符集那 4KB 字模整块复制到 B 字符集那一块 ★★
        ;   依据: ./206b 的 `PH: r206 cs03==001C` 与 ./194 的 attr bit3 分析。
        ;   它必须在 VgaPostTail(写路径)之后、Sr3Keep(还 SR03)之前。
        ;   【r412】这一枪已经移到本钩子【最后一笔】
        ;   (Sr3Keep 之后、MiniVDD_DispiTrapOn 之前) —— 见下面那一段。
        ;   这里只剩下前置的写。
        ; 【r412】基址标注 已从本钩子【退役】—— 依据见文件后部 RETIRED 段。
        ; r412: fill plane-2 banks 1..7 with the ROM font too (r195 behaviour).
        ;   ./221: geometry standard, start 0, ROM glyph present at plane-2 0x822,
        ;   every write verifies -- and the screen is still wrong.  The only action
        ;   in our own record that ever made the ASCII side correct is this fill.
        ; 【r412】VfcFillBanks 已从本钩子【退役】—— 依据见文件后部 RETIRED 段。
        ; 【r412】起始地址校验 已从本钩子【退役】—— 依据见文件后部 RETIRED 段。
        ; 【r412】字符时钟改到 12 号里做(见下面那一行)
        call    VbeScanProbe                       ; r412: read VBE 4F03h while the box holds the screen
        ; 【r412】冷启动 VGA 复位 已从本钩子【退役】—— 依据见文件后部 RETIRED 段。
        ; 【r412】CRTC 一致性检查 已从本钩子【退役】—— 依据见文件后部 RETIRED 段。
        ; 【r412】★ 显示侧标记(r183 就有的那一只): 第 24 行右侧写 vbe9x r412 ★
        call    VgaMark
        ; 【r412】r412 的 `call 模拟 INT 10h`(VMMCall Simulate_Int(10h))已【删除】:
        ;   ./231 的结果是"完全无变化", 而本文件 r170 的长注早就写明原因 ——
        ;   全屏期间盒子的 INT 10h 落在硬件上, 不经过 mini-VDD, 所以那条链
        ;   上一发 V86 中断也不会让主 VDD 的模式记账动起来。留着它只是让
        ;   每一场全屏都多走一次 VMMCall。
        ; 【r412】★★ 最后一笔: 把客户机自己的 SR03 写回去 ★★
        ;   它在上面整条链(VgaM3Apply/VfcTextGroup/VgaMark)里被硬写成 00h,
        ;   而它本来是客户机(DBCS 文本模式)的字符集映射。
        mov     al, 1
        ; 【r412】Sr3Keep 已从本钩子【退役】—— 依据见文件后部 RETIRED 段。
        ; 【r412】★★ 本轮唯一的行为改动 ★★
        ;   把“让卡自己的 BIOS 设一次 mode 3”(INT 10h AX=0003,
        ;   即用户手打的 `mode co80`)放到【本钩子的最后一笔】。
        ;   理由: ./196/./198 里唯一被实测证明有效的动作就是它,
        ;   而 r209 的次序是「BIOS 设完 ⇒ 我们再写 VgaPostTail/
        ;   字模窗口克隆/基址标注/VgaMark/Sr3Keep」—— 最后一个碰硬件的
        ;   是我们, 不是 BIOS。现在倒过来: BIOS 是最后一个。
        ;   DDK《PRE_HIRES_TO_VGA (Function 4)》第 6/7 步正是这个次序
        ;   (第 6 步 VDD 自己发 INT 10h AH=0, 第 7 步才叫 POST)。
        ;   预算在调用前一行重置, 所以每一次切屏
        ;   都会真的发一次, 不会因为“预算用完”静默跳过。
        ; 【r412】★★ 本轮唯一的行为改动: POST 这一枪也关了 ★★
        ;   四轮对照(r212 02h / r213-214 72h / r215 不开火 / r216 03h) 说明:
        ;   把画面变成"冻住的 GUI 截片"的是 PRE 那一枪; 而 POST
        ;   那一枪单独也从来没把画面修好过。两枪都是【嵌套
        ;   V86里的 BIOS 调用】(不经主 VDD 的 INT 10h 虚拟处理器),
        ;   而唯一被证实有效的 `mode co80` 恰恰是【经过它】的。
        ;   DDK 第 6 步: 主 VDD 自己会发 INT 10h AH=0, "如果够了, mini-VDD 不必
        ;   自己把屏幕设成 VGA 模式"。本轮就是那一句的字面执行:
        ;   两枪都不开, 切屏完全交给主 VDD。
        ;   (r216 在这里 `mov byte ptr [Bios3N],0` + `call BIOS 状态回放`)
        ; 【r412】最后一笔: 把显示起点也定到 0(VBE 4F07h)。
        ;   ./213 的"伪冻屏 GUI 截片"就是起点没定
        ;   (CR0C/CR0D 写了也不管用, 见 r212 的 t11=0)。
        call    VbeStartZero
        ; 【r412】★★ 本轮唯一的行为改动 ★★ 把标准 VGA 文本调色板整套写回。
        ;   位置: 本钩子的最后一笔(排在 VbeStartZero 之后), 因为任何一次
        ;   VBE 调用都可能把 DAC 重新装载, 而 DDL 第 6 步的模式设模式就是
        ;   在 5 号之前做完的 —— 我们是最后一个动颜色的人。
        ;   闸门与自证见 VgaPalRestore 的头注。
        call    VgaPalRestore
        ; 【r412】调色板之后: 对页头取一次指纹(此时盒子还没开始画)
        mov     byte ptr [PageWitMode], 1
        ; 【r412】★★ 主线: 真正执行 DDK 第 4 步 —— 让卡离开线性(LFB)模式 ★★
        ;   ./240 证明页【有写手】(p5sum=002F -> p15sum=00F0), 而屏仍不是页的内容
        ;   => 卡根本没离开 VBE 线性模式, VGA 寄存器与文本页都不参与扫描。
        ;   这里在【VDD 自己的设模式之后】再让 BIOS 走一次 VBE 设模式(非线性),
        ;   紧跟着用 4F03h 回读, 判据只有一个: 4F03 到底有没有离开 0072h。
        ;   (r213/214 把这一枪放在 PRE, 在 VDD 记账之前, 于是冻屏; 5 号是记账之后。)
        ; 【r412】先放哨兵: 若日志里这三格还是 AA/BB/CC, 就说明这段【根本没执行】,
        ;   那样 vsmh=0000 不能读成"4F03 回了 0"(r231 的日志就是一直 0)。
        ; 【r412】★★ 这两枪【摘掉】★★
        ;   r242 的哨兵证明它们【确实执行了】(AA/BB/CC 被覆盖), 但 4F03h 的
        ;   返回 AH=0000 —— 5 号这一拍 INT 10h 已被主 VDD 虚拟化, 嵌套调用
        ;   拿不到 VBE 应答。再加上 r100/r141/r241 三条独立证据把蓝屏指向
        ;   "回调里的嵌套 V86 调用", 于是这一枪【无效且有风险】, 一并去掉。
        ;   ⇒ "离开 LFB"改到显示驱动侧 INT 2Fh AX=4001h 做(下一版本), 那里还在
        ;     ring 3 / Windows VM 上下文, VBE 可用, 且不动主 VDD 的记账。
        ; 【r412】本轮的行为改动: 字模上半区自检。调色板之后紧接着做,
        ;   因为这两件事都属于 "一次真设模式该做的事"。只在 "ASCII 有字模、
        ;   DBh 空" 这一种状态下才动手; 两个字都在就一个字节都不写。
        ;   见 VgaFontTop 的头注。
        ; 【r412】r233 的"让 BIOS 装 ROM 字模"那一枪【摘掉】: 它只服务非 ASCII(缺陷 B),
        ;   不属于主线(进入全屏花屏), 而且它会改画面, 会把 A 的归因搅浑。
        ;   VgaFontHalf 仍是纯读仪器, 留在这里备用。
        call    MiniVDD_DispiTrapOn            ; r116: Windows 屏幕回来了 → 开陷阱
        ; 【r146】进盒【之后】再采一拍(相位 5): 与 4 号那一拍(相位 4)
        ;   配对, "文本态修补 到底把硬件写成了什么样"从此有读数。
        ;   r144 曾把采样从 5 号挪到 48 号(理由是 r143 的两个样本一直是 0),
        ;   而 ./145 证明 48 号那一拍永远是"设模式之前"的文本值 ——
        ;   要看的是切屏【两拍】的差别, 所以两处都要采。
        mov     byte ptr [FboxSnapPhase], 5
        call    FboxSnap
        ; 【r412】此处原有的 4F07h 动作已【退役】—— 依据见下面 RETIRED 段。
        cmp     dword ptr [OldDispatch + 5*4], 0
        je      short tst_null_5
        jmp     dword ptr [OldDispatch + 5*4]
tst_null_5:
        VDD_NC
        ret
EndProc TraceStub_5

;-----------------------------------------------------------------------------
; TraceStub_7 —— POST_VGA_TO_HIRES(7)
;   与原来的 TraceStubState 7 宏展开【逐条等价】(记录 + 事件计数 + 清 VGA 位 +
;   保留位掩码 0FEh = 只清 bit0), 唯一差别是在转交主 VDD 原处理函数【之前】
;   调一次 HiResClockFix。为什么必须在这一刻动手, 见 HiResClockFix 的长注。
;   与 5 号的对称结构: 5 号进去时补"文本取指", 7 号回来时补"图形时钟位"。
;   r129-marker: hand-written TraceStub_7 (was TraceStubState 7).
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r151】TraceStub_6 —— PRE_VGA_TO_HIRES(6), 手写(原来是 TraceStubEvt 宏)
;   DDK《PRE_HIRES_TO_VGA (Function 4)》原文(回 Windows 的那一半时序):
;     "During PRE_HIRES_TO_VGA **or PRE_VGA_TO_HIRES**, remove trapping on
;      your device's extended registers or make sure that they are not
;      virtualized (meaning that any register I/O that the VM does during the
;      mode change is actually written to the physical hardware). During
;      POST_HIRES_TO_VGA **or POST_VGA_TO_HIRES**, reestablish the trapping."
;   r150 的代码只做到了一半: 4 号 TrapOff / 5 号 TrapOn, 而 6 号与 7 号
;   【什么都没做】—— 也就是说"回 Windows"的那一整段(含 display.drv 的
;   ResetHiResMode, DDK 明说它负责重建 hi-res、恢复 DAC/硬件光标/字库)是在
;   1CEh/1CFh 的陷阱仍然开着的状态下跑的。本桩按原文补上, 与 4/5 号对称。
;   语义其余部分与 TraceStubEvt 6 的宏展开逐条等价。
;   r151-marker: hand-written TraceStub_6 (PRE_VGA_TO_HIRES now drops trapping).
;-----------------------------------------------------------------------------
BeginProc TraceStub_6
        pushfd
        push    eax
        mov     eax, 6
        call    MiniVDD_TraceRecord
        inc     byte ptr [TraceEvt + VBE_TR_EVT_PRE_VGA_TO_HIRES]
        pop     eax
        popfd
        call    MiniVDD_DispiTrapOff            ; r151: DDK 原文要求的那一半
        cmp     dword ptr [OldDispatch + 6*4], 0
        je      short ts6_nc
        jmp     dword ptr [OldDispatch + 6*4]
ts6_nc:
        VDD_NC
        ret
EndProc TraceStub_6

BeginProc TraceStub_7
        pushfd
        push    eax
        mov     eax, 7
        call    MiniVDD_TraceRecord
        inc     byte ptr [TraceEvt + VBE_TR_EVT_POST_VGA_TO_HIRES]
        and     byte ptr [TraceState], 0FEh
        mov     byte ptr [VgaFixArm], 0         ; r143: 回到 Windows 了, 停止持屏期补写
        mov     byte ptr [FboxWord + VBE_TR_FBOX_DISARM], 2   ; r144: 证据里留一笔
        pop     eax
        popfd
        call    HiResClockFix
        ; 【r146】最后一句: 把 4 号存下的模式/寻址字节写回。
        ;   放在 HiResClockFix 【之后】是刻意的 —— 它只 OR 三个 bit0,
        ;   而我们这一枪写的是整字节, 应该是最后落地的那个值。
        call    HiresRestore
        ; 【r151】DDK 原文要求 POST_VGA_TO_HIRES 也要【恢复】陷阱
        ;   ("...or POST_VGA_TO_HIRES, reestablish the trapping")。
        ;   放在最后一句: 前面 HiResClockFix/HiresRestore 的写要落到真硬件。
        call    MiniVDD_DispiTrapOn
        cmp     dword ptr [OldDispatch + 7*4], 0
        je      short tst_null_7
        jmp     dword ptr [OldDispatch + 7*4]
tst_null_7:
        VDD_NC
        ret
EndProc TraceStub_7

;-----------------------------------------------------------------------------
; 【r129】HiResClockFix —— 回到 Windows 高分辨率那一刻, 补回"图形模式 + 8 点
;   字符时钟"的三个位(SR01/AR10/GCR06 的 bit0)。
;   r129-marker: HiResClockFix (graphics/8-dot bits repair at POST_VGA_TO_HIRES).
;
;   【为什么需要它 —— 5 号与 7 号此前完全不对称】
;   5 号(POST_HIRES_TO_VGA)有 文本态修补 去补文本取指/平面那 8 个寄存器,
;   而 7 号(POST_VGA_TO_HIRES)只记账、一个寄存器都不写。也就是说
;   "DOS 会话 → 回桌面"这个方向【从来没有过修补】, 全靠主 VDD 那次模式设置
;   (对 VESA 模式 = 主 VDD 自己调 4F02h)。而 ./127 的 r1 现场读数(CRTP n=0011
;   /0012, 会话结束之后、重建之前)是:
;       SR01 = 0x0003   bit0 = 0  ⇒ 9 点字符时钟(80 列文本的取景宽度)
;       AR10 = 0x0000   bit0 = 0  ⇒ 属性控制器认为【在文本模式】
;       Misc 3CC = 0x0067 (会话前是 0x002F, 点时钟选择位也变了)
;   这三个位都是标准 VGA 的【模式状态位】, 桌面画面(1024x768x16bpp 线性
;   打包像素)与它们自相矛盾。后果可见: ./127 r1 的 1.png —— 主机窗口被
;   收缩成 320x769、画面横向压扁, 而虚拟机内截图 1_guest.png 完全正常
;   (显存对、扫描输出按另一套几何解释)。这正是"文本会话把时钟位留在
;   文本态、退出时没人置回"这一类通用 VGA 收尾义务, 与卡型号、与是不是
;   模拟器无关: 任何 VGA 上 8/9 点字符时钟都只有这一个位。
;
;   【本函数只写什么】只写三个【模式状态位】, 一律"先读、不对才写":
;       SR01(时序器 01h)  bit0 -> 1   8 点字符时钟(高分辨率打包像素模式)
;       GCR06(图形控制器 06h) bit0 -> 1   图形模式(文本模式该位为 0)
;       AR10(属性控制器 10h)  bit0 -> 1   图形模式(文本模式该位为 0)
;   【绝不碰的】任何时序/几何字段(CR00..CR18、SR02..SR04、GCR05…)。
;   画面几何本来就该由主 VDD 那次模式设置决定, 我们只把与它矛盾的三个
;   状态位摆正 —— 这与 文本态修补 那条"只写取指/平面, 绝不动时序"的
;   纪律完全同构。
;   【AR10 的现场纪律】属性控制器有内部触发器(索引/数据相)。本函数用
;   标准的 3DAh->索引态 序列读回当前索引, 只在 bit0 需要改时写一次数据,
;   写完按原索引与【原 palette enable 位】还原索引寄存器 —— 调色板内容的
;   使能状态与原来逐位相同。索引字节本身在 bit4:0 上原样送回。
;
;   【保守闸门】只有一条: [VbeState.VBS_policyVgaFix] != 0(用户显式开
;   VgaFix=1;r129 起它同时管两个方向的收尾, 且默认值改为 1)。
;   【为什么【不】再按"VM 请求过的模式"设第二道闸门(r129 实测教训)】
;   本函数的第一版照 文本态修补 的样子加了 `TraceI10Ax < 100h` 这一条,
;   那是错的: TraceI10Ax 保存的是 48 号【最近一次】被请求的模式, 而在
;   "DOS 会话结束 → 回桌面"这一刻它记的正是【进会话】那次(实测 0003h,
;   恰好能过闸门)—— 可一旦主 VDD 在别处也走过 48 号(把桌面模式记成
;   0072h/0117h), 同一个闸门就会把真正需要补的那一次挡在外面。
;   闸门与结论之间没有因果关系, 就不该拿它当判据。
;   【替代它的是什么】下面三个位【逐位先读、不对才写】:
;     · 硬件本来就对(正常路径) ⇒ 一个字节都不写, 零风险;
;     · 硬件不对 ⇒ 写进去的也只有 bit0(另外 7 位原样保留)。
;   于是"该不该写"由硬件自己回答, 而不是由我们推断 —— 比任何闸门都准。
;
;   【触发点为什么是 POST(7) 而不是 PRE(6)】与 文本态修补 同源理由:
;   DDK 时序里第 6 步主 VDD 才执行进 VM 的模式设置, 第 7 步调 POST ——
;   写在 POST 里等于我们的写是【最后一手】, 不会被主 VDD 后面那一步覆盖。
;
;   【现场保护】EAX/EDX/EBX 由本函数自己保存恢复; 不碰 ECX/ESI/EDI/EBP
;   (上层 stub 还要把它们交给主 VDD 的原处理函数)。BL 只是"读回来几个 1"
;   的小计数, 但按 MSDN 的 "Save everything that you use" 一律保存。
;-----------------------------------------------------------------------------
BeginProc HiResClockFix
        pushfd
        push    eax

        cmp     byte ptr [VbeState.VBS_policyVgaFix], 0
        jne     short hcf_go                   ; 策略开: 交给下面那个过程
        jmp     short hcf_nop                  ; 策略关: 一个寄存器都不碰
hcf_go:
        call    HiResClockBits                 ; BL = 三个位里读回来几个 1
        cmp     bl, 3
        je      short hcf_fixed                ; 3/3 = 现在是图形/8 点态
;   [r132] The two `inc byte ptr [TraceEvt + ...]` counters that used to live
;   here wrote PAST the end of the blob's event block: VBE_TR_Evt is 4 bytes
;   (+0..+3) and VBE_TR_Counts starts at +8, so +4/+5 landed on the low bytes of
;   VBE_TR_Counts[0]/[1] -- "function 0 was called N times".  r131 stopped
;   READING them (the "TRC evt HiResClk fix=0001" line was that artefact), this
;   round stops WRITING them.
hcf_nop:
        jmp     short hcf_out
hcf_fixed:
hcf_out:
        pop     eax
        popfd
        ret
EndProc HiResClockFix

;-----------------------------------------------------------------------------
; 【r129】HiResClockBits —— 三个模式状态位的"先读、不对才写"; 出口 BL =
;   把三个位读回来数出的 1 的个数(0..3): 3 = 硬件现在确实是图形/8 点态。
;   【为什么返回 BL 而不是 AX】本过程必须原样恢复 EAX(它是调用方 — 主 VDD
;   分派约定 — 的活寄存器), 而"先算返回值再 pop eax"会把返回值盖掉; BL
;   本来就是这个计数, 直接把它当返回值最省事, 也不破坏任何约定。
;   【为什么拆成两个过程】第一版把闸门、补写、读回计数全部写在一个过程里,
;   于是"策略关"那条跳转要跨过 100 多字节去落在末尾的计数上, 只能靠 MASM
;   把无条件跳转升成近跳 —— 而那条路径的【目标】距离已经超过 128 字节,
;   ML 6.11c 与 文本态修补 当年一样会报 A2075。拆开以后两个过程都只有几十
;   字节, 全部跳转都落在短跳射程内, 过程以后再长也不会重犯。
;
;   【只写这三个位, 别的位一律原样保留】
;     SR01(时序器 01h)  bit0 -> 1   8 点字符时钟(高分辨率打包像素模式)
;     GCR06(图形控制器 06h) bit0 -> 1   图形模式(文本模式该位为 0)
;     AR10(属性控制器 10h)  bit0 -> 1   图形模式(文本模式该位为 0)
;   任何时序/几何字段(CR00..CR18、SR02..SR04、GCR05…)一个都不碰 —— 与
;   文本态修补 同一条纪律。AR10 的索引在出口【按原字节还原】(含 palette
;   enable 位), 属性控制器的使能状态因此逐位不变。
;
;   【现场保护】EAX/EDX/EBX/ECX 全部保存恢复。ECX 必须自己保存: 上层
;   TraceStub_7 转交主 VDD 原处理函数时它还是活寄存器, 而分派约定要求处理
;   函数保存自己用到的一切(MSDN: "Save everything that you use")。
;   BL 是本过程唯一的输出, 不恢复。
;-----------------------------------------------------------------------------
BeginProc HiResClockBits
        pushfd
        push    eax
        push    ecx
        push    edx

        ; --- SR01(时序器 01h)bit0: 1 = 8 点字符时钟 ---
        mov     dx, 03C4h
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        test    al, 01h
        jnz     short hcb_gc                   ; 已经是 8 点: 一个字节都不写
        or      al, 001h
        out     dx, al                         ; 3C5h

        ; --- GCR06(图形控制器 06h)bit0: 1 = 图形模式 ---
hcb_gc:
        mov     dx, 03CEh
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        test    al, 01h
        jnz     short hcb_ar                   ; 已经是图形模式: 不写
        or      al, 001h
        out     dx, al

        ; --- AR10(属性控制器 10h)bit0: 1 = 图形模式 ---
        ;   【r157】索引字节自己写 30h(AR10 | PAS=1)。旧写法读 3C0h 取
        ;   "原索引"再保留 PAS —— 而 3C0h 的可读性不是标准的一部分, 读回 0
        ;   时 PAS 就被清成 0(视频被关); 收尾写回那个 0 又是一次。这里与
        ;   AR10 写入(r153)/FboxSnap(r157) 用同一条纪律。
        ;   读来的字节仍然留下当取证(与 FboxSnap 同一个字段, 值由硬件决定)。
hcb_ar:
        mov     dx, 03DAh                      ; 复位 ATC 触发器 -> 索引态
        in      al, dx
        mov     dx, 03C0h
        in      al, dx                         ; AL = 当前索引(+ palette enable)
        mov     [Probe + VBE_TR_PR_ATCRAW], al ; 【只取证】

        ; r157-marker: HiResClockBits keeps PAS=1 by WRITING the index byte.
        ;   【r159 说明(源码卫生, 不改行为)】这一行在 r157 的批量替换里被粘成了
        ;   "注释 + mov dx, 03C0h" 同一行 —— MASM 把 ';' 之后整行当注释, 那条
        ;   指令就【静默消失】了。此处 DX 已在上方两条指令前设为 03C0h, 所以
        ;   删掉这段被粘进来的指令文本并不改变生成的代码; 留着它反而会让后来
        ;   的人以为"这里有一条 mov dx,03C0h"。同一类事故本轮在别处也出现过
        ;   (packaging/buildcheck 的括号), 所以静态扫描(an154/sw_scan.py)现在
        ;   专门查"注释尾巴里粘着指令"这种形状。
        mov     al, 030h                       ; 索引 10h | PAS=1
        out     dx, al                         ; index write -> flip-flop to DATA
hcb_ar_cur:
        ; 【r129 实机教训 A2070】`in al, imm8` / `out imm8, al` 这两种【立即数端口】
        ;   形式, ML 6.11c 认为"操作数无效"(实测 minivdd.asm(1231)/(1235)/(1271)
        ;   三处 error A2070: invalid instruction operands)。第一版在
        ;   `in al, 03C1h` / `out 03C1h, al` 上栽了 —— 而注释里还写着"这样写没问题",
        ;   那句话是错的, 保留在这里当反面教材。
        ;   正确形式只有一种: 端口先装进 DX, 再用 `in al, dx` / `out dx, al`。
        ;   本文件其它所有地方(文本态修补 等)本来就是这么写的。
        ;   r129-marker: attribute-controller access uses DX only (A2070 fix).
        mov     dx, 03C1h                      ; AC data port (DX form only: A2070)
        in      al, dx
        test    al, 01h
        jnz     short hcb_ar_out               ; already graphics: no data write
        ; 【r137 修正 —— 这一行原来只补 bit0, 会造出一个"混合值"】
        ;   ./136 的日志给出了本机两个现场读数:
        ;       干净态  SNAP ATC10 = 0041h
        ;       花屏态  SNAP ATC10 = 000Dh   (从那一拍起再没回来)
        ;   000Dh = 000Ch | 01h —— 000Ch 正是标准 VGA【文本模式】的 AR10h,
        ;   而本驱动里唯一会写 AR10h 的代码就是这一行(先读、bit0=0 才写、
        ;   写 `读回值 | 1`)。也就是说: 当盒子把 AR10h 留在文本形状时, 这一行
        ;   把它"修"成 000Dh —— 一个既不是文本形状(bit2/bit3 还是文本值)
        ;   也不是本卡高彩形状(bit6 = 0)的第三种东西, 而且它能通过我们
        ;   自己那位"bit0=1 就算图形模式"的检查, 于是再没有任何人去纠正它。
        ;   ⇒ 两个改法:
        ;     ① (本行) 补 bit0 的同时把 bit6(palette address source)也补上
        ;        —— 本卡干净态这两个位都是 1, 而 bit6 = 0 在通用 SVGA 模型里
        ;        会把扫描输出切到"半分辨率/低分辨"那条渲染路径上(86Box 的
        ;        `lowres = attrregs[10h] & 40h` 就是这一位; ./108 与 r117 §0.1
        ;        早就逐像素取证过"每 2 像素一周期"的同族故障)。
        ;     ② (drv.c, r137) 已知良好样本从这一轮起【包含 AR10h..AR14h】,
        ;        回放写的是样本里的整字节, 不再是 `读回值 | 某一位`。
        ;   ① 只是把"没有样本时"的兜底值从 000Dh 改成 004Dh; ② 才是根治。
        ;   ⚠ 指令长度与跳转距离都不变(0C 01 -> 0C 41), 不新增任何跳转。
        or      al, 041h
        mov     dx, 03C0h                      ; [r133] data writes belong on 3C0h;
        out     dx, al                         ;        3C1h is read-only on real VGA
hcb_ar_out:
        mov     dx, 03DAh                      ; 【r157】只把索引相交还, 不写回
        in      al, dx                         ;   任何"读来的原索引": 索引寄存器
                                               ;   现在是我们写的 30h = PAS=1
                                               ;   (视频保持打开), 这就够了

        ; --- 读回自证: 三个位再数一遍, 3/3 才算"这一刻硬件确实是图形/8 点" ---
        mov     bl, 0
        mov     dx, 03C4h
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        test    al, 01h
        jz      short hcb_cnt
        inc     bl
        mov     dx, 03CEh
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        test    al, 01h
        jz      short hcb_cnt
        inc     bl
        mov     dx, 03DAh
        in      al, dx
        mov     dx, 03C0h
        mov     al, 030h                       ; 【r157】索引 10h | PAS=1
        out     dx, al                         ;   (旧写法读 3C0h 再 or 10h:
                                               ;   读回 0 时 PAS 被清 = 关视频)
hcb_cnt_cur:
        mov     dx, 03C1h                      ; AC 数据口: 必须经 DX(立即数端口非法)
        in      al, dx
        test    al, 01h
        jz      short hcb_cnt_restore
        inc     bl
hcb_cnt_restore:
        mov     dx, 03DAh                      ; 【r157】只交还索引相, 不写回旧索引
        in      al, dx                         ;   (索引寄存器 = 30h, PAS=1)
hcb_cnt:
        ; BL 就是返回值(见头注), 这里不再改它; 3/3 还是不到由调用方判断。
        ; 刻意【不放任何跳转】在这里: 整个过程因此没有一条跨过出口的长跳转,
        ; 与 文本态修补 那条 A2075 教训彻底绝缘。
        pop     edx
        pop     ecx
        pop     eax
        popfd
        ret
EndProc HiResClockBits


;-----------------------------------------------------------------------------
; 【r114】文本态修补 —— 全屏 VGA 交接后补写"文本取指 / 平面"寄存器组
;   r114-marker: 文本态修补 (text fetch/plane repair at POST_HIRES_TO_VGA).
;
;   【为什么需要它】./110 t3 的两张主机截图(同一台机、同一个 DOS 盒、同一屏
;   `mem` 输出)做逐单元取证后得到的事实(细节与全部数字见 R114_CHANGES.md §0):
;     · 直接以全屏启动 command: 字符、字模、属性全部正确(9 点 720x400 光栅);
;     · 窗口 DOS → Alt+Enter → 再 Alt+Enter 回全屏: 字符的"占位"仍对得上
;       (空白格仍是空白格、文字格仍有墨), 但是
;         ① 每个字符格的字模 = 该格字符码字节重复 16 行 —— 空格的 0x20 于是
;            在每格第 2 列画出一条满高竖线(实测 1872 个有墨格里 1335 个是
;            这种"条纹格", 其中 1187 个正是"只亮第 2 列"; 干净那份只有
;            23.5%, 且全是真正横线字形 '─' 那一类);
;         ② 每格的属性字节 = 该格字符本身 —— 空白格(0x20)于是成了"绿底
;            黑字"(调色板 2 = 纯绿), 这就是整屏 73% 绿 + 文字处彩条的来源;
;         ③ 逐格比对 `Memory Summary:` 那一行 15 格: 属性高半字节与该格
;            字符的高半字节【逐格相等】(4,6,6,6,7,7,2,5,7,6,6,6,7,7,3)。
;   ⇒ 文本模式的三种取指(字符 / 属性 / 字模)在那一刻【全部塌到 plane 0】。
;     字符本身是对的(所以画面"位置对、内容错"), 坏的只是"取指从哪个平面来"。
;   这不是 ring 3 能修的: 全屏 DOS 期间 System VM 里没人调显示驱动(./110 t3
;   日志里心跳整整停了 59 秒 ⇒ 那段时间一个 DDI 都没进来)。而 DDK 把这一刻
;   明确划给 mini-VDD(PRE/POST_HIRES_TO_VGA 页原文):
;     "The Main VDD calls the function PRE_HIRES_TO_VGA in the mini-VDD.
;      The mini-VDD may if it needs to set the screen out of hi-res mode.
;      It should also set flags and/or remove traps on any extended registers
;      that it's trapping."
;   我们这两个钩子(r101 起)只记账、一个寄存器都不写 —— 这就是缺口。
;
;   【本函数只写什么】只写"取指 / 平面 / 文本模式"这 8 个寄存器, 【绝不碰
;   任何时序或时钟寄存器】(SR00/SR01、CR00..CR07 的定时字段、CR10..CR13…):
;   r114-marker: timing/clock registers are never touched (fetch/plane only).
;   花屏画面的光栅几何本来就是对的(80x25、9 点格、720x400), 坏的是取指;
;   动时序只会把画面弄没, 一点好处都没有。
;     SR02 内存屏蔽   = 03h  写平面 0/1 —— DOS 文本字节的落点
;     SR03 字符映射   = 00h  字模表 A/B 都选 0(我们没有任何理由换字体)
;     SR04 内存模式   = 02h  非 chain4、odd/even 打开
;     GCR05 图形模式  = 10h  odd/even 模式开、移位 0
;     GCR06 杂项      = 0Eh  显存映射回 B8000-BFFFF 32K
;     CR14 下划线     = 00h  bit6(DW)=0
;     CR17 模式控制   = A3h  字节模式 + 文本/兼容位(我们自己的高彩模式是
;                            E3h —— 见 ./111 t8 的端口读数; 老值 0x40 的
;                            "字寻址"位还在, 正是"两种取指混在一起"的形态)
;     AR10 属性模式   = 0Ch  blink + line-graphics, 文本模式的标准值
;
;   【触发点为什么是 POST(5) 而不是 PRE(4)】DDK 时序: 第 6 步主 VDD
;   "executes an Interrupt 10h, function 0 mode change"去进 VM 的模式, 第 7 步
;   才调 POST_HIRES_TO_VGA。若那次模式设置是真 BIOS 调用, 它本该把这一切都写
;   好(那正是"直接全屏启动"能正常的原因); 对我们的 VBE 线性模式它显然没生效
;   (否则不会有这个 bug)。写在 POST 里 = 我们的写是【最后一手】。
;
;   【保守闸门】三条全满足才动手, 任何一条不满足就是一个寄存器都不写
;   (与 r113 行为逐字节相同):
;     ① [VbeState.VBS_policyVgaFix] != 0   用户显式开(VgaFix=1);
;     ② CR17 读回 bit0 非 0                硬件此刻确在文本/兼容态;
;     ③ 48 号记下的"VM 请求过的模式" <100h VM 要的是标准 VGA 模式。
;   【r116 修正——闸门 ③ 读错了变量】r114/r115 这一条读的是
;   [VbeState.VBS_currentMode], 那是 ring 3 推上来的【Windows 自己的模式】
;   (实测恒为 0117h)。于是 `cmp eax,100h / jb` 永远不成立 ⇒ 这个补写
;   【一次都没执行过】—— ./115 三份带 VgaFix=1 的日志里
;   `TRC st bits=` 从来没有 bit3(0008/000A/000B) 就是证据。
;   本函数头注写的判据本来就是"48 号记下的 VM 请求模式", 代码却读了
;   另一个变量。r116 起改读 [TraceI10Ax](由 48 号在设模式前写入, 实测
;   会话边沿 = 0003h < 100h ⇒ 闸门放行)。
;   ③ 被挡下时置 bit7(VBE_TR_ST_VTFBLOCK), 成功执行时清掉 —— 于是日志
;   能区分"没跑"的两种原因(闸门挡下 vs 策略关/新 VxD 没到位)。
;   r116-marker: 文本态修补 gate3 reads TraceI10Ax (was VBS_currentMode).
;   动手之后把 TraceState 的 VBE_TR_ST_VGAFIX 位置起 —— 日志里
;   `TRC st bits=` 出现 bit3(0008/000A/000B) 就证明这条路真的执行过。
;
;   【现场保护】EAX/EDX/flags 由本函数自己保存恢复; 不碰 EBX/ECX/ESI/EDI/EBP
;   (上层 stub 还要把它们交给主 VDD 的原处理函数)。
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r166】VgaGeoTest —— "CRTC 此刻是不是在显示标准 mode 3 文本屏"。
;   入口: 无。出口: AL = 失配位图(0 = 8 个几何寄存器逐个等于标准 mode 3)。
;   位序(与 GeoM3Idx/GeoM3Val 同序):
;     bit0 CR00  bit1 CR01  bit2 CR12  bit3 CR13
;     bit4 CR15  bit5 CR17  bit6 CR09  bit7 CR14
;   【为什么这是本轮唯一的新判据】它是【物理的那一件事】的直读: 硬件是不是
;   正在扫描一张 720x400 的文本屏。r147..r165 用的两个代理("GCR06 bit0"、
;   "映射是否解码 B8000")在 ./165 里被两个方向同时证伪(见 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 与
;   VgaMemCHold9 的注), 而这一条把三个要分开的世界一次分开:
;     全屏文本会话(CRTC = mode 3)      → 掩码 0  → 该补就补;
;     桌面(1024x768: CR13=80/CR17=E3) → 掩码非 0 → 一个字节都不碰;
;     全屏图形 VM(12h/13h)             → 掩码非 0 → 一个字节都不碰。
;   【只读】8 次索引写 + 8 次数据读, 与 文本态修补 里既有那一段同一手法。
;   【现场保护】EAX/EBX/ECX/EDX/ESI/EDI 全部自己保存(调用者含 4/5 号桩)。
;   r166-marker: VgaGeoTest (the physical "is a mode 3 text screen displayed").
;-----------------------------------------------------------------------------
BeginProc VgaGeoTest
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        xor     bl, bl                          ; BL = 失配位图
        mov     cl, 001h                        ; CL = 当前位
        mov     ch, 008h                        ; CH = 剩余格数
        mov     esi, OFFSET32 GeoM3Idx
        mov     edi, OFFSET32 GeoM3Val
vgt_loop:
        mov     dx, 03D4h
        mov     al, [esi]
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, [edi]
        je      short vgt_ok
        or      bl, cl
vgt_ok:
        shl     cl, 001h
        inc     esi
        inc     edi
        dec     ch
        jnz     short vgt_loop
        mov     al, bl
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret
EndProc VgaGeoTest


;-----------------------------------------------------------------------------
; 【r167】VgaM3Apply —— 把【标准 mode 3 的整组 CPU 写/取指路径】写进硬件, 写完
;   逐字节读回自证, 失配位图落盘。
;
;   【为什么必须是"整组", 而不是继续补那六个字节】
;     ./166 的读数把这件事判死了: 六个字节(SR02/SR03/SR04/GCR05/GCR06/AR10)每一笔
;     都读回成立(fixedN=11, stillBad=0, s9bad=0), 而画面照旧是花屏 —— 说明"显示
;     那一侧"(CRTC 几何 + 映射 + 平面使能)已经对了, 坏的是【写进显存的那一侧】。
;     mode 3 的写路径由另一组寄存器决定, 而它们在本项目里【从来没有被写过一次】:
;       GCR00/GCR01/GCR02  复位/置位(set-reset)与颜色比较。图形模式(含 VBE 线性
;                          模式的残留)常把 GCR01 摆成"置位使能", 那时 CPU 写显存
;                          得到的是置位值, 而不是它写下去的那个字节;
;       GCR03              数据旋转 + 功能选择。非 0 时每个字节先被旋转、再被
;                          AND/OR/XOR —— 字符与属性都会变成别的东西;
;       GCR08              位屏蔽。非 FFh 时一部分位根本写不进去;
;       GCR04/GCR07        读平面选择 / 颜色无关位(读回路径);
;       AR12/AR13          平面使能与像素平移。AR13 非 0 时整屏横向错位。
;     ⇒ 盒子的全屏文本页(字符进平面 0、属性进平面 1)只要碰上其中任何一个残留
;       值, 写进去的就不是它想写的字节: 字符变成别的字形、属性变成别的颜色
;       —— ./160/1601.png 里【同一格里出现多种颜色】正是"属性字节本身是错的"
;       的形态(文本模式一格只有一前一后两种颜色, 不可能一格里出三色)。
;       而字模(平面 2)也会被自己的文本写覆盖(r164 起 fontbad 在涨)。
;     用户按 `mode co80` 能救回来, 就是因为 BIOS 的模式设置【整组】都写 ——
;     包括上面这七个字节。r147..r166 一直只写"显示那一侧", 所以"写了也白写"。
;
;   【写哪 17 个字节、值从哪来】
;     全部取自标准 mode 3 的教科书表(vga-register-reference.md §6c 的 IBM BIOS
;     模式 3 串, 与本文件 GeoM3Val 同源):
;       SR02=03h SR03=00h SR04=02h
;       GCR00=00h GCR01=00h GCR02=00h GCR03=00h GCR04=00h
;       GCR05=10h GCR06=0Eh GCR07=00h GCR08=FFh
;       CR0Ch=00h CR0Dh=00h            (显示起始地址 = 显存偏移 0)
;       AR10=0Ch AR12=0Fh AR13=00h
;     【一个时序/时钟寄存器都不碰】CR00..CR09、CR10..CR18(除 0C/0D 之外)、
;     SR00/SR01 一律不写 —— 与 文本态修补 的 r114 纪律逐条相同: 光栅几何本来
;     就是对的, 动它只会把画面弄没。
;     【CR0C/CR0D = 0 的依据】盒子在全屏会话里写 B8000, 而 GCR06=0Eh 把
;     B8000-BFFFF 映射到显存偏移 0 开始(32K) ⇒ 取指基址必须也是 0 才"写在
;     一处、读在一处"。./166 的 `probe fetch=0000`(CR0D:CR0C*2)说明它现在
;     就是 0 —— 这一笔是把那个巧合变成【保证】, 而不是去改一个已知的好值。
;
;   【自证】写完逐字节读回 17 次, 失配位图存 MemC+MCBAD0/1/2(位序见
;   vbeshare.h 的 VBE_TR_MC_MCBAD* 注释), MemC+STILLBAD = 0(全部相等)/1,
;   MemC+MCWR 递增。于是"哪一格没站住"第一次是一个可读的数。
;
;   【属性控制器纪律】照 r153/r157: 自己写索引字节(PAS=1), 从不读 3C0h,
;   收尾只读 3DAh 交还索引相 —— 这一条在 4/5 号上尤其重要(它是盒子接屏前
;   的最后两拍, PAS=0 等于把视频关掉)。
;
;   【现场】EAX/EBX/ECX/EDX/ESI/EDI + flags 全部自己保存恢复; 不碰 EBP
;   (9 号页写着 "EBP: May or may not be pointing to the VM's client registers")。
;   r167-marker: VgaM3Apply -- the whole documented mode 3 CPU-path set.
;-----------------------------------------------------------------------------
BeginProc VgaM3Apply
        pushfd
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        ; ---- 0. 【r169 加过、r170 撤回】Misc Output (3C2h) = 67h ----------
        ;   r169 在这里加过一次 `out 3C2h, 67h`(理由: 它是只写寄存器, 本项目
        ;   "写进去读回来"的自证结构性覆盖不到它, 而标准 mode 3 的三份资料
        ;   (本仓 vga-register-reference.md §6b/§6c/§6d)都写着 67h)。
        ;   ./169 把它判掉了, 判据是行为而不是资料: 加上这一笔之后, 那条
        ;   【用户自己反复验证有效的自救路径】当场坏掉 —— 手动 `mode co80`
        ;   从"画面恢复正常"退化成"全黑 + 一个光标"(1691.png)。也就是说这张
        ;   卡的 3C2h 现状值与 67h 并不自洽(bit0 的 I/O AS / bit1 的显存使能
        ;   在它的 VBE 形态下另有含义), 我们写进去的那一字节正是"字符发生器
        ;   读不出字模"的那类故障的合理来源。资料的结论没错, 错的是"这张卡此刻
        ;   就是这个值"这个前提 —— 只写寄存器不能用"资料说应该是多少"去覆盖
        ;   硬件自己设好的值。
        ;   宁可少写一个字节, 也不要在唯一的自救路径上再插一刀。
        ;   r170-marker: the r169 3C2h=67h write is withdrawn.

        ; ---- 1. 时序器: SR02=03 SR03=00 SR04=02 ------------------------
        mov     dx, 03C4h
        mov     al, 002h
        out     dx, al
        inc     dx
        mov     al, 003h                        ; SR02 写平面 0/1
        out     dx, al
        dec     dx
        mov     al, 003h
        out     dx, al
        inc     dx
        xor     al, al                          ; SR03 字符映射 0
        out     dx, al
        dec     dx
        mov     al, 004h
        out     dx, al
        inc     dx
        mov     al, 002h                        ; SR04 非 chain4 顺序寻址
        out     dx, al

        ; ---- 2. 图形控制器: GCR00..GCR04 = 00 --------------------------
        ;   set-reset 关、颜色比较关、数据不旋转(功能=替换)、读平面 0。
        mov     dx, 03CEh
        xor     al, al                          ; 索引 00
        out     dx, al
        inc     dx
        xor     al, al                          ; GCR00 = 00
        out     dx, al
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        xor     al, al                          ; GCR01 = 00 置位使能关
        out     dx, al
        dec     dx
        mov     al, 002h
        out     dx, al
        inc     dx
        xor     al, al                          ; GCR02 = 00 颜色比较关
        out     dx, al
        dec     dx
        mov     al, 003h
        out     dx, al
        inc     dx
        xor     al, al                          ; GCR03 = 00 不旋转/替换
        out     dx, al
        dec     dx
        mov     al, 004h
        out     dx, al
        inc     dx
        xor     al, al                          ; GCR04 = 00 读平面 0
        out     dx, al

        ; ---- 3. GCR05=10 GCR06=0E GCR07=00 GCR08=FF --------------------
        dec     dx
        mov     al, 005h
        out     dx, al
        inc     dx
        mov     al, 010h                        ; GCR05 奇偶主机寻址开
        out     dx, al
        dec     dx
        mov     al, 006h
        out     dx, al
        inc     dx
        mov     al, 00Eh                        ; GCR06 映射 B8000 32K + 文本
        out     dx, al
        dec     dx
        mov     al, 007h
        out     dx, al
        inc     dx
        xor     al, al                          ; GCR07 = 00 颜色无关位清
        out     dx, al
        dec     dx
        mov     al, 008h
        out     dx, al
        inc     dx
        mov     al, 0FFh                        ; GCR08 = FF 位屏蔽全开
        out     dx, al

        ; ---- 4. CRTC: 显示起始地址 = 0(先高后低, 免得经过 64K 外) ------
        mov     dx, 03D4h
        mov     al, 00Dh
        out     dx, al
        inc     dx
        xor     al, al                          ; CR0D = 00
        out     dx, al
        dec     dx
        mov     al, 00Ch
        out     dx, al
        inc     dx
        xor     al, al                          ; CR0C = 00
        out     dx, al

        ; ---- 5. 属性控制器: AR10=0C AR12=0F AR13=00 --------------------
        mov     dx, 03DAh
        in      al, dx                          ; 复位触发器 -> 索引相
        mov     dx, 03C0h
        mov     al, 030h                        ; 索引 10h | PAS(20h)
        out     dx, al
        mov     al, 00Ch                        ; AR10 闪烁 + 行图形
        out     dx, al
        mov     al, 032h                        ; 索引 12h | PAS
        out     dx, al
        mov     al, 00Fh                        ; AR12 四个平面全开
        out     dx, al
        mov     al, 033h                        ; 索引 13h | PAS
        out     dx, al
        xor     al, al                          ; AR13 不做像素平移
        out     dx, al
        mov     dx, 03DAh
        in      al, dx                          ; 索引相交还

        ; ---- 6. 逐字节读回: 失配位图 -> MemC+MCBAD0/1/2 ---------------
        xor     ebx, ebx                        ; BL = MCBAD0, BH = MCBAD1
        xor     ecx, ecx                        ; CL 的 bit0 = AR13
        mov     dx, 03C4h
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 003h
        je      short vma_ok01
        or      bl, 001h
vma_ok01:
        mov     dx, 03C4h
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok02
        or      bl, 002h
vma_ok02:
        mov     dx, 03C4h
        mov     al, 004h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 002h
        je      short vma_ok03
        or      bl, 004h
vma_ok03:
        mov     dx, 03CEh
        xor     al, al
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok04
        or      bl, 008h
vma_ok04:
        mov     dx, 03CEh
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok05
        or      bl, 010h
vma_ok05:
        mov     dx, 03CEh
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok06
        or      bl, 020h
vma_ok06:
        mov     dx, 03CEh
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok07
        or      bl, 040h
vma_ok07:
        mov     dx, 03CEh
        mov     al, 004h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok08
        or      bl, 080h
vma_ok08:
        mov     dx, 03CEh
        mov     al, 005h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 010h
        je      short vma_ok09
        or      bh, 001h
vma_ok09:
        mov     dx, 03CEh
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 00Eh
        je      short vma_ok10
        or      bh, 002h
vma_ok10:
        mov     dx, 03CEh
        mov     al, 007h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok11
        or      bh, 004h
vma_ok11:
        mov     dx, 03CEh
        mov     al, 008h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 0FFh
        je      short vma_ok12
        or      bh, 008h
vma_ok12:
        mov     dx, 03D4h
        mov     al, 00Ch
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok13
        or      bh, 010h
vma_ok13:
        mov     dx, 03D4h
        mov     al, 00Dh
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, 000h
        je      short vma_ok14
        or      bh, 020h
vma_ok14:
        mov     dx, 03DAh                       ; AR10 读回: 每次都要重新复位
        in      al, dx                          ;   触发器才能写索引字节
        mov     dx, 03C0h
        mov     al, 030h
        out     dx, al
        mov     dx, 03C1h
        in      al, dx
        cmp     al, 00Ch
        je      short vma_ok15
        or      bh, 040h
vma_ok15:
        mov     dx, 03DAh
        in      al, dx
        mov     dx, 03C0h
        mov     al, 032h
        out     dx, al
        mov     dx, 03C1h
        in      al, dx
        cmp     al, 00Fh
        je      short vma_ok16
        or      bh, 080h
vma_ok16:
        mov     dx, 03DAh
        in      al, dx
        mov     dx, 03C0h
        mov     al, 033h
        out     dx, al
        mov     dx, 03C1h
        in      al, dx
        cmp     al, 000h
        je      short vma_ok17
        or      cl, 001h
vma_ok17:
        mov     dx, 03DAh
        in      al, dx                          ; 索引相交还

        ; ---- 7. 落盘: 位图 + 0/1 + 次数 ---------------------------------
        mov     edi, OFFSET32 MemC
        mov     [edi + VBE_TR_MC_MCBAD0], bl
        mov     [edi + VBE_TR_MC_MCBAD1], bh
        mov     [edi + VBE_TR_MC_MCBAD2], cl
        inc     byte ptr [edi + VBE_TR_MC_MCWR]
        xor     al, al
        mov     dl, bl
        or      dl, bh
        or      dl, cl
        jz      short vma_allok
        mov     al, 1
vma_allok:
        mov     [edi + VBE_TR_MC_STILLBAD], al

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        popfd
        ret
EndProc VgaM3Apply


; r412 D: 文本态修补 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r144】FboxSnap —— 盒子持屏期的一拍硬件指纹: SR04/GCR06/CR13/CR17。
;   【为什么只读这四个】它们一起足以分开"硬件在谁的模式里":
;     我们的 1024x768x16 : GCR06=05 CR13=80 CR17=E3
;     标准 VGA 文本 3   : GCR06=0E CR13=28 CR17=A3
;   而它们在 ring 3 永远读不到 —— 全屏期间 ring 3 一次都不进来。
;   【只读】三个索引端口各写一次索引(标准做法), 数据端口只读;
;   不碰 ATC(它要动 flip-flop), 不碰任何时序寄存器。
;   【现场保护】本过程只改 EAX/EDX, 自己保存恢复。
;-----------------------------------------------------------------------------
;   【r145 调用点变两个】除了 48 号(设模式之前的一拍), 35 号在武装期
;   内每 256 次也采一拍(见 FboxLiveTick)。两者写同一块 4 字节,
;   所以日志里那四行永远是"最近一次采样时硬件的样子"
;   (持屏期就是盒子屏幕的现场)。
;   【r146 —— 采样点与采样内容都改了】
;     ① 【内容】13 个寄存器: SEQ 01..04 / GCR 03..06 / CRTC 00,01,13,14,17
;        (+ 相位字节)。原四个(SR04/GCR06/CR13/CR17)看不见"寻址/平面"
;        那几位, 而 ./144 的截图形态指向的正是那几位(见 vbeshare.h)。
;     ② 【采样点】4 号(进盒【前】, 期望还是我们的模式 —— 也是 HiresSave
;        用的那一拍)、5 号(进盒【后】)、7 号(回 Windows 【后】)、
;        48 号(设模式前)、35 号(持屏期活采样)。
;     ③ 【活采样节拍】35 号从"每 256 次"改成"武装后第一拍 + 之后每 16 拍"。
;        依据: ./145 整场 N35 = 0x66 = 102 次 < 256 ⇒ r145 的持屏期指纹
;        一次都没采到(日志里那四行始终是 48 号采的"设模式前"值)。
;        16 拍 ≈ 0.8 秒, 102 拍 ⇒ 6~7 帧, 足够看出"持屏期硬件是什么样"。
;   【现场保护】只改 EAX/EDX(与 r144 相同); 显式只读写 VxD 自己的数据区。
;   【相位由调用点先写好】FboxSnapPhase 是数据区的一个字节, 采完即抄进
;   [VbeSnap+VBE_TR_FS_SRC]。它是纯诊断量, 不参与任何判据。
; r144-marker: FboxSnap -- in-box hardware fingerprint (SR04/GCR06/CR13/CR17).
; r146-marker: FboxSnap -- 13 mode/addressing registers + phase byte, five call sites.
BeginProc FboxSnap
        pushfd
        push    eax
        push    edx
        mov     al, [FboxSnapPhase]             ; 相位先落盘
        mov     [VbeSnap + VBE_TR_FS_SRC], al
        mov     dx, 03C4h                       ; --- 时序器 SR01..SR04 ---
        mov     al, 01h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_SR01], al
        dec     dx
        mov     al, 02h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_SR02], al
        dec     dx
        mov     al, 03h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_SR03], al
        dec     dx
        mov     al, 04h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_SR04], al
        mov     dx, 03CEh                       ; --- GCR03..GCR06 ---
        mov     al, 03h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_GCR03], al
        dec     dx
        mov     al, 04h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_GCR04], al
        dec     dx
        mov     al, 05h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_GCR05], al
        dec     dx
        mov     al, 06h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_GCR06], al
        mov     dx, 03D4h                       ; --- CRTC ---
        mov     al, 00h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_CR00], al
        dec     dx
        mov     al, 01h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_CR01], al
        dec     dx
        mov     al, 13h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_CR13], al
        dec     dx
        mov     al, 14h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_CR14], al
        dec     dx
        mov     al, 17h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_CR17], al
        ; --- 【r149】CR0Ch = 显示起始地址低位 ------------------------------
        ;   【为什么它是最后一个未知量】./146/./148 反复量到: 盒子持屏期
        ;   CRTC 是教科书式的 720x400 文本(CR00=5F CR01=4F CR13=28 CR17=A3),
        ;   六个"取指/平面"寄存器也被证明可以修到教科书的文本值
        ;   (./148 的 stillBad=0), 而画面照样是垃圾 —— 那就只剩一个变量没量:
        ;   【扫描从显存哪个字开始读】。CR0C/CR0D 就是它。它 = 0 时基址是
        ;   B8000(与盒子写字符的地方一致); 不为 0 就说明取指基址被挪走了,
        ;   "写在一处、读在另一处"当场成立, 而且它与我们写的六个寄存器无关
        ;   —— 这正是"co80 能救 / 我们修不动"的分水岭。
        ;   只读, 走的是 3D4/3D5 的标准索引/数据舞(与上面同一套), 不写回。
        dec     dx
        mov     al, 0Ch
        out     dx, al
        inc     dx
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_CR0C], al
        ; --- 【r147】AR10(属性控制器 10h): 只读, 且原样交还触发器相位 -------
        ;   【为什么要加它】r146 的 13 个寄存器把"取指/平面"那组补齐了, 而
        ;   ./136 早就量出本机 AR10 的两个现场值(干净态 41h / 花屏态 0Dh),
        ;   r137 的结论是"它决定扫描输出走哪条渲染路径"(bit0 文本/图形,
        ;   bit6 palette 地址源)。它是取指链上的第三个开关, 而 r146 一次
        ;   都没读过它 —— 花屏如果真的不在 SR/GCR 那五个里, 就必须有它的
        ;   读数才能继续往下判。
        ;   【相位纪律照抄 r133】先读 3DAh 回索引相 -> 选择 AR10 -> 读 3C1h
        ;   取数据 -> 再读 3DAh 把索引相交还给后续代码。
        ;   【r157 修正 —— 这里原来是全项目里最危险的一处】
        ;   旧写法是 r133 那套"读 3C0h 拿原索引 -> and 0E0h 保留 PAS ->
        ;   or 10h 选 AR10 -> 写回原索引"。r153 已经判定: `in al,3C0h` 的
        ;   可读性【不是 VGA 标准的一部分】, 读回 0 时 and 0E0h 得 0、or 10h
        ;   得 10h ⇒ PAS(bit5)=0 = "正在装载调色板" = 【视频被关】; 而收尾
        ;   把读来的 0 写回去又是第二次 PAS=0。r153 据此修了 显存寄存器回读 与
        ;   AR10 写入 —— 但漏掉了本过程, 而本过程恰恰是【唯一会在
        ;   盒子正持屏时跑】的那一个(持屏期每 16 次 35 号采一拍)。
        ;   现在: 索引字节自己写 30h(AR10 | PAS=1), 从 3C0h 读来的那个字节
        ;   只当【取证】存进 VBE_TR_PR_ATCRAW, 不参与任何写; 收尾只读 3DAh
        ;   把索引相交还 —— 与 r153 对另外两处的纪律逐条相同。
        mov     dx, 03DAh
        in      al, dx                          ; 复位触发器 -> 索引相
        mov     dx, 03C0h
        in      al, dx                          ; AL = 当前索引(+ bit5 PAS)
        mov     [Probe + VBE_TR_PR_ATCRAW], al  ; 【只取证】可不可信由日志回答
        mov     dx, 03C0h
        mov     al, 030h                        ; 索引 10h | PAS(20h): 视频保持打开
        ; r157-marker: FboxSnap keeps PAS=1 by WRITING the index byte.
        out     dx, al                          ; 索引写 -> 数据相
        mov     dx, 03C1h                       ; AC 数据口只能经 DX(A2070 教训)
        in      al, dx
        mov     [VbeSnap + VBE_TR_FS_AR10], al
        mov     dx, 03DAh                       ; 索引相交还(PAS 保持 1)
        in      al, dx
        ; --- 整块搬进 VxD 数据区的 FboxSnapVal(16 字节) ---------
        mov     eax, dword ptr [VbeSnap + 0]
        mov     dword ptr [FboxSnapVal + 0], eax
        mov     eax, dword ptr [VbeSnap + 4]
        mov     dword ptr [FboxSnapVal + 4], eax
        mov     eax, dword ptr [VbeSnap + 8]
        mov     dword ptr [FboxSnapVal + 8], eax
        mov     eax, dword ptr [VbeSnap + 12]
        mov     dword ptr [FboxSnapVal + 12], eax
        ; --- 【r147】相位槽: 每个采集点各占 16 字节 --------------------
        ;   ./146 的六次落盘相位字节全是 0x23(35 号): 所有采集点共用
        ;   FboxSnapVal 时"最后一个写的人赢", 4/5/7 三拍整条因果链被抹掉。
        ;   这里按相位算槽偏移, 各写各的(见 vbeshare.inc 的 VBE_TR_PS_*)。
        movzx   eax, byte ptr [FboxSnapPhase]
        mov     edx, VBE_TR_PS_P35              ; 默认 = 持屏期"最近"
        cmp     al, 4
        je      short fs_p4
        cmp     al, 5
        je      short fs_p5
        cmp     al, 7
        je      short fs_p7
        cmp     al, 8
        je      short fs_p48
        jmp     short fs_slot
fs_p4:  xor     edx, edx
        jmp     short fs_slot
fs_p5:  mov     edx, VBE_TR_PS_P5
        jmp     short fs_slot
fs_p7:  mov     edx, VBE_TR_PS_P7
        jmp     short fs_slot
fs_p48: mov     edx, VBE_TR_PS_P48
fs_slot:
        push    edi                             ; EDI 是分派约定里的活寄存器
        mov     edi, OFFSET32 PhaseSnap
        add     edi, edx
        mov     eax, dword ptr [VbeSnap + 0]
        mov     dword ptr [edi + 0], eax
        mov     eax, dword ptr [VbeSnap + 4]
        mov     dword ptr [edi + 4], eax
        mov     eax, dword ptr [VbeSnap + 8]
        mov     dword ptr [edi + 8], eax
        mov     eax, dword ptr [VbeSnap + 12]
        mov     dword ptr [edi + 12], eax
        ; 35 号的"本场第一拍"另存一槽(4 号把 FboxFirstDone 清零 ⇒ 每场
        ;   全屏会话都留一份"主 VDD 刚设完盒子模式"的现场, 不会被后续
        ;   几百次活采样覆盖)。
        cmp     byte ptr [FboxSnapPhase], 35
        jne     short fs_no1st
        cmp     byte ptr [FboxFirstDone], 0
        jne     short fs_no1st
        mov     byte ptr [FboxFirstDone], 1
        mov     edi, OFFSET32 PhaseSnap
        add     edi, VBE_TR_PS_P35FIRST
        mov     eax, dword ptr [VbeSnap + 0]
        mov     dword ptr [edi + 0], eax
        mov     eax, dword ptr [VbeSnap + 4]
        mov     dword ptr [edi + 4], eax
        mov     eax, dword ptr [VbeSnap + 8]
        mov     dword ptr [edi + 8], eax
        mov     eax, dword ptr [VbeSnap + 12]
        mov     dword ptr [edi + 12], eax
fs_no1st:
        pop     edi
        pop     edx
        pop     eax
        popfd
        ret
EndProc FboxSnap

;-----------------------------------------------------------------------------
; 【r146】HiresSave —— 4 号(PRE_HIRES_TO_VGA)里存一份"我们的模式"字节。
;   【为什么在这一拍】DDK 的 4 号页: 主 VDD 调 PRE_HIRES_TO_VGA 时, 屏幕
;   【还没】离开 hi-res ⇒ 此刻读到的就是我们的模式 (文本态修补 也会在
;   同一拍写文本值, 所以本函数必须排在它【之前】)。
;   采到的 12 字节里真正回放的是 10 个(见 HiresRestore)。
;-----------------------------------------------------------------------------
BeginProc HiresSave
        pushfd
        push    eax
        push    edx
        cmp     byte ptr [VbeState.VBS_policyHiresFix], 0
        je      short hs_out
        mov     byte ptr [FboxSnapPhase], 4
        call    FboxSnap                        ; 顺带把这一拍落进 blob(相位=4)
        test    byte ptr [VbeSnap + VBE_TR_FS_SR01], 01h
        jz      short hs_out                    ; 不是八点字符时钟: 不收
        ; 【r147 判据修正 —— 这一条四轮里一个样本都没收下】
        ;   r146 这里写的是 `test SR04, 08h` (chain4 位), 而【本机我们的
        ;   线性打包模式 SR04 = 06h】: bit3=0 ⇒ 每一场会话都在这里跳出,
        ;   ./146 的四次落盘 savedN/restoredN 全是 0000 就是它的物证。
        ;   chain4 是 mode 13h 那一族的形态, 与"线性打包"正好相反 ——
        ;   要问的是"图形模式位", 而它就是 GCR06 的 bit0(FreeVGA:
        ;   Miscellaneous Graphics Register bit0 = Alphanumeric Mode
        ;   Disable, 1 = 图形模式并关掉字符发生器锁存)。我们 05h(1) /
        ;   文本 0Eh(0) / 花屏态 04h(0), 一位就把两者分开。
        test    byte ptr [VbeSnap + VBE_TR_FS_GCR06], 01h
        jz      short hs_out                    ; 文本/兼容取指: 不是我们的模式
        test    byte ptr [VbeSnap + VBE_TR_FS_CR17], 40h
        jz      short hs_out                    ; 字寻址位不成立: 不是我们的模式
        mov     eax, dword ptr [VbeSnap + 0]
        mov     dword ptr [HiresSav + 0], eax
        mov     eax, dword ptr [VbeSnap + 4]
        mov     dword ptr [HiresSav + 4], eax
        mov     eax, dword ptr [VbeSnap + 8]
        mov     dword ptr [HiresSav + 8], eax
        mov     byte ptr [HiresValid], 1
        inc     byte ptr [HiresSavedN]
hs_out:
        pop     edx
        pop     eax
        popfd
        ret
EndProc HiresSave

;-----------------------------------------------------------------------------
; 【r146】HiresRestore —— 7 号(POST_VGA_TO_HIRES)里把那一份写回去。
;   【为什么是 POST(7) 而不是 PRE(6)】与 4/5 号同构的 DDK 时序: 第 6 步
;   主 VDD 自己发 INT 10h 把屏切回 Windows, 第 7 步才调 POST_VGA_TO_HIRES。
;   "我们的最后一句"必须落在 POST 这一拍(否则会被主 VDD 那一步盖掉) ——
;   与 r114 的 文本态修补 选 POST(5) 是同一条理由。
;   【写哪些】只写"模式/寻址"这 10 个字节, 绝不碰任何定时字段:
;     SEQ 02/03/04 (写平面屏蔽 / 字符映射 / 内存模式: chain4 与奇偶)
;     GCR 03/04/05/06 (功能选择 / 读平面 / 奇偶与 256 色位移 / 显存映射)
;     CRTC 14/17 (双字/下划线位, 字寻址与 SE)
;     SEQ 01 放【最后】写: 它管字符时钟与移位寄存器装载带宽, 放最后
;     可以缩短"半个配置被扫出去"的窗口(与 VxdCrtRelatch 同一纪律)。
;   【先数差异再写】回放前先把现场读一遍并与快照逐字节比较, 差值落盘:
;     0 = 主 VDD 自己已经恢复好了(那我们这一枪是空枪, 也是重要读数);
;     >0 = 它漏了哪几个字节这件事在日志里有数字。
;   【一次快照只回放一次】写完把 HiresValid 清 0 ⇒ 用户换分辨率之后
;   不会被旧值覆盖(下一次 4 号重新采)。
;-----------------------------------------------------------------------------
BeginProc HiresRestore
        pushfd
        push    eax
        push    edx
        ; 【r115 教训】hr_out 在本过程末尾(离这里 400+ 字节), 所以这两道
        ;   闸门必须写成 jcc/jmp 成对式: 显式 `short` 会强制短编码,
        ;   ML 6.11c 会报 A2075: jump destination too far。
        cmp     byte ptr [VbeState.VBS_policyHiresFix], 0
        jne     short hr_g1
        jmp     hr_out
hr_g1:
        cmp     byte ptr [HiresValid], 0
        jne     short hr_g2
        jmp     hr_out
hr_g2:
        mov     byte ptr [FboxSnapPhase], 7
        call    FboxSnap                        ; 回 Windows 之后的第一拍现场
        mov     byte ptr [HiresLastDiff], 0
        ; r158-marker: the drift mask names the bytes, not just counts them.
        mov     byte ptr [Probe + VBE_TR_PR_HDIFFL], 0
        mov     byte ptr [Probe + VBE_TR_PR_HDIFFH], 0
        mov     al, [VbeSnap + VBE_TR_FS_SR04]
        cmp     al, [HiresSav + VBE_TR_FS_SR04]
        je      short hr_sr04
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 001h        ; bit 0 = SR04
hr_sr04:
        mov     al, [VbeSnap + VBE_TR_FS_GCR06]
        cmp     al, [HiresSav + VBE_TR_FS_GCR06]
        je      short hr_gcr06
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 002h        ; bit 1 = GCR06
hr_gcr06:
        mov     al, [VbeSnap + VBE_TR_FS_CR13]
        cmp     al, [HiresSav + VBE_TR_FS_CR13]
        je      short hr_cr13
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 004h        ; bit 2 = CR13
hr_cr13:
        mov     al, [VbeSnap + VBE_TR_FS_CR17]
        cmp     al, [HiresSav + VBE_TR_FS_CR17]
        je      short hr_cr17
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 008h        ; bit 3 = CR17
hr_cr17:
        mov     al, [VbeSnap + VBE_TR_FS_SR01]
        cmp     al, [HiresSav + VBE_TR_FS_SR01]
        je      short hr_sr01
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 010h        ; bit 4 = SR01
hr_sr01:
        mov     al, [VbeSnap + VBE_TR_FS_SR02]
        cmp     al, [HiresSav + VBE_TR_FS_SR02]
        je      short hr_sr02
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 020h        ; bit 5 = SR02
hr_sr02:
        mov     al, [VbeSnap + VBE_TR_FS_SR03]
        cmp     al, [HiresSav + VBE_TR_FS_SR03]
        je      short hr_sr03
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 040h        ; bit 6 = SR03
hr_sr03:
        mov     al, [VbeSnap + VBE_TR_FS_GCR03]
        cmp     al, [HiresSav + VBE_TR_FS_GCR03]
        je      short hr_gcr03
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFL], 080h        ; bit 7 = GCR03
hr_gcr03:
        mov     al, [VbeSnap + VBE_TR_FS_GCR04]
        cmp     al, [HiresSav + VBE_TR_FS_GCR04]
        je      short hr_gcr04
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFH], 001h        ; bit 8 = GCR04
hr_gcr04:
        mov     al, [VbeSnap + VBE_TR_FS_GCR05]
        cmp     al, [HiresSav + VBE_TR_FS_GCR05]
        je      short hr_gcr05
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFH], 002h        ; bit 9 = GCR05
hr_gcr05:
        mov     al, [VbeSnap + VBE_TR_FS_CR14]
        cmp     al, [HiresSav + VBE_TR_FS_CR14]
        je      short hr_cr14
        inc     byte ptr [HiresLastDiff]
        or      byte ptr [Probe + VBE_TR_PR_HDIFFH], 004h        ; bit 10 = CR14
hr_cr14:
        mov     dx, 03C4h                       ; --- SEQ 02/03/04 ---
        mov     al, 02h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_SR02]
        out     dx, al
        dec     dx
        mov     al, 03h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_SR03]
        out     dx, al
        dec     dx
        mov     al, 04h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_SR04]
        out     dx, al
        mov     dx, 03CEh                       ; --- GCR 03/04/05/06 ---
        mov     al, 03h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_GCR03]
        out     dx, al
        dec     dx
        mov     al, 04h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_GCR04]
        out     dx, al
        dec     dx
        mov     al, 05h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_GCR05]
        out     dx, al
        dec     dx
        mov     al, 06h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_GCR06]
        out     dx, al
        mov     dx, 03D4h                       ; --- CRTC 14 / 17 ---
        mov     al, 14h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_CR14]
        out     dx, al
        dec     dx
        mov     al, 17h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_CR17]
        out     dx, al
        mov     dx, 03C4h                       ; --- SEQ 01 最后 ---
        mov     al, 001h
        out     dx, al
        inc     dx
        mov     al, [HiresSav + VBE_TR_FS_SR01]
        out     dx, al
        inc     byte ptr [HiresRestN]
        mov     byte ptr [HiresValid], 0        ; 一份快照只回放一次
hr_out:
        pop     edx
        pop     eax
        popfd
        ret
EndProc HiresRestore

;-----------------------------------------------------------------------------
; 【r147】显存寄存器回读 —— 把"CPU 怎么看显存"那六个寄存器读进 MemCNow。
;   顺序固定: MemCNow+0=SR02 / +1=SR03 / +2=SR04 / +3=GCR05 / +4=GCR06
;             / +5=AR10
;   【为什么是这六个】DDK 的 CRTC owner / MemC owner 分工里, 属于 MemC 的
;   就是"怎么让 CPU 看到显存"这一组: 时序器 02/03/04(写平面屏蔽 / 字符
;   映射 / chain4 与奇偶)与图形控制器 05/06(奇偶与 256 色位移 / 显存映射
;   与文本位), 加上属性控制器的 10h(它决定扫描输出从哪条路径取颜色)。
;   【只读】三个索引端口各写一次索引(标准做法), 数据端口只读。
;   【现场保护】EAX/EDX/EBX + flags 全部自己保存恢复 —— 调用方(35 号)的
;   BL 是"不一致个数"计数器, 必须原样活着回来。
;-----------------------------------------------------------------------------
; 【r164】字模一致性检查 —— 字模(字符发生器)守卫。
;
;   【为什么需要它】r162 把这条链的每一环都写成了数字: 六字节补写【每次都
;   读回成立】(fixedN=27 / stillBad=0 / TGBAD=0), 而画面是整片 #AAAAAA 加一个
;   白块(./148 逐像素: 99.72% 均匀灰 + 一个 9x2 白条 = 文本光标)。一页 20h 07h
;   的空文本页【只可能渲染成黑底】—— 要让每一格都变成属性前景色的实心块,
;   唯一的可能是【字模(平面 2)里已经不是字模】。
;
;   【为什么会这样】DDK《About Virtual Display Minidrivers》的 owner 段写明:
;   窗口化 DOS VM 的 MemC 组是 "the CPU sees the video memory as a 4 plane VGA"
;   —— 也就是 SR02 = 0F(写全部四个平面)、SR04 = 06、GCR05 = 00、GCR06 = 04。
;   ./163 的读数正是这一组(p35 A = 06 04 00 00)。在这个组里, 盒子写它的文本页
;   (字符进平面 0、属性进平面 1)会【同时写平面 2】—— 字模就这么被自己的文本
;   覆盖掉了。用户打 `mode co80` 能救回来, 正因为 BIOS 的设模式会把字模重新
;   装进平面 2: 这正是本项目十四轮以来【从来没有补过的另一半】。
;
;   【字模在哪儿、怎么装】本仓 vga-register-reference.md §7a/§7b 引 FreeVGA 与
;   OSDev 原文: "Display memory plane 2 is divided up into eight 8K banks of
;   characters, each of which holds 256 character bitmaps. Each character is on a
;   32 byte boundary and is 32 bytes long. The offset in plane 2 of a character
;   within a bank is determined by taking the character's value and multiplying it
;   by 32." ⇒ 0 号字库 = 平面 2 偏移 0000h..1FFFh, 字符 c 在 c*32; 8x16 只用
;   其中前 16 字节。来源用 BIOS 的 INT 43h 向量(线性 10Ch) —— DDK 4 号页第 5 步
;   就是"存 BIOS 数据区与 INT 43h 向量", 说明这个向量在全屏切换里是官方照顾的
;   对象; 向量不可信时回落到经典的 F000:FA6E = 线性 0FFA6Eh。
;
;   【纪律】
;     · 只在【脏拍 + 连续两拍(TGRUN>=2) + 武装期 + 本场预算未用完】时才动;
;     · 先读指纹, 与 FontRef 相同就【一个字节都不写】;
;     · 装载只写平面 2(SR02 = 04), 其余三个平面一个字节都不碰;
;     · 收尾把六个寄存器写回文本值(SR02=03 SR03=00 SR04=02 GCR05=10 GCR06=0E
;       + AR10=0Ch) —— 因此它本身就是一次 mini 版 `mode co80`;
;     · 每场最多 4 次(FontN), 不做每拍对打; 让位(YIELD)闸门只约束 r163 的
;       每拍补写, 不约束这个"整体重装"。
;
;   现场: EAX/EBX/ECX/EDX/ESI/EDI + flags 全部自己保存恢复。
; r164-marker: 字模一致性检查 -- reload the ROM 8x16 character generator.

; r412 D: 字模一致性检查 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r170】VgaFontBoot —— 全屏文本开场把 8x16 字模装进平面 2(每场最多一次)。
;
;   【为什么需要"开场就装"这一条路】字模一致性检查 的闸门是"六字节窗口脏了
;   两拍"—— 它回答的是"字模被别人的文本写覆盖了没有"。而 ./169 给出的
;   形态不是"被覆盖"这一种:
;     · 全屏刚进去就花屏(每一格都是属性前景色的实心块);
;     · 手动 `mode co80` 能救回来, 而 r169 把重装关掉之后, 同一个动作只剩
;       下"清屏 + 一个光标"(1691.png)。
;   两件事合起来说明: 进入全屏那一刻, 平面 2 里【不是】可用的字模, 而给它
;   装字模的人是我们自己(BIOS 设模式并不负责这件事, 至少在这台机器上不
;   负责 —— 否则 r169 的 co80 应该照旧能显示文字)。
;   既然"脏拍"这个前提在开场并不成立(开场窗口本来就是干净的文本值),
;   就必须有一条【不看脏、只看时机】的路。时机 = POST_HIRES_TO_VGA: 此时
;   主 VDD 刚设完盒子模式, 盒子还没开始画, 写平面 2 不会覆盖任何文本。
;
;   【安全边界】只写平面 2(SR02=04), 收尾用 VgaM3Apply 把整组文本寄存器
;   写回(它自己就是一次 mini 版 mode 3), 拷贝段关中断; 每场一次(FontN),
;   整条路受 policyInt10Fix 控制(Int10Fix=0 一行全关, 回到"只记录")。
;   过程保护 EAX + flags; VfcLoad 自己保存其余现场。

;=============================================================================
; 【r412】RETIRED(GLYPH: VgaFontBoot) —— 字模写入停手（r260 的 A 部分保留, B 部分撤回）
;   依据: r259 已把 5 号那一拍的写卡动作删净, 而手打 mode co80 之后
;   【非 ASCII 仍然空白】 ⇒ 破坏 DBCS 字模的是字模写入过程本身。
;   远东 DBCS 的第二字符集字模住在平面 2 的 1..7 号段; 只要往平面 2 写过
;   8x16 ASCII 字模, 那几段就被覆盖, 而 mode co80 只重装 0 号段。
;   ⇒ 平面 2 完全交给 BIOS 与控制台。本过程本体与判据全部保留, 入口直接返回。
;
;   r260 的 B 部分(M2Probe 挂到 41 VESA_SUPPORT / 47 VESA_POST)【已撤回】:
;   r260 在本机引导期崩在 VMPOLL, 连 VBE9XLOG 都没生成。41/47 是在主 VDD
;   处理 INT 10h 的中途被调的, 在里面再嵌套一次 INT 10h 会造成重入 ⇒
;   探针恢复成 r259 的样子(只挂 15 号), 那是已证明能正常引导的配置。
;=============================================================================
;-----------------------------------------------------------------------------
BeginProc VgaFontBoot
        ; 【r412】入口直接返回 —— 见上。
        ret
        pushfd
        push    eax
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      short vfb_done
        cmp     byte ptr [VgaFixArm], 0
        je      short vfb_done
        cmp     byte ptr [FontN], 4
        jae     short vfb_done
vfb_done:
        pop     eax
        popfd
        ret
EndProc VgaFontBoot
;-----------------------------------------------------------------------------
; 【r177】VgaFontProv —— "字符发生器(平面 2)到底对不对"的唯一入口。
;
;   【它为什么取代了 r164..r176 的三条路】
;     · r164/r165 的 字模一致性检查 冲着"脏拍"去, 而开场那一拍的窗口
;       本来就是干净的文本值 ⇒ 开场不会装(r170 头注);
;     · r170 的 VgaFontBoot 只在 5 号开一枪, 而 r173 因为 ./172 的
;       `fontfp=1818` 把它从 5 号撤掉了;
;     · r176 把它挂到 35 号尾部的孔径修正里 ⇒ 只在"孔径原本错了"
;       时才跑, 而 ./176 证明孔径本来就是对的 ⇒ 又一次不可达。
;   【本轮的结构】一个入口、三条通路、一个可读的结果字节:
;     ① 先读指纹(VfcFpRead, 图形孔径)。已经等于源字节 ⇒ 一个字节都不写, stat=1;
;     ② 否则走我们自己的 4KB 拷贝(VfcLoad, get_font_access 那一组值),
;       拷完整块回读自证; 站住了 stat=2;
;     ③ 我们没站住 ⇒ 让卡自己的 BIOS 装: 先 AX=1104h, 再 AX=1114h,
;        每一枪之后都读指纹; 对了 stat=3 / stat=5, 两枪都不对 stat=4。
;   判读表(日志里的 `PH: probe fontst=`, 占用已不可达的 boff 查位):
;   【r183 的判读表 —— 与 r182 的差别是"1"没了】
;     2 = 某一档窗口【先过了 8 字节记号自证】、再把 4KB 整块写进去、整块回读
;         也成立 ⇒ 字模确实装上了。第几档看 `PH: probe fontwin=`:
;           1 = 0Ch / B8000 / SR04=06h(盒子自己的文本窗口, 默认)
;           2 = 0Ch / B8000 / SR04=07h
;           3 = 00h / A0000 / SR04=07h(get_font_access() 原样)
;           4 = 04h / A0000 / SR04=07h(r176..r182 那一档)
;     3 = 四档都不行、BIOS AX=1104h 之后指纹对上了;
;     5 = 四档都不行、BIOS AX=1114h 之后指纹对上了;
;     4 = 窗口能读写、字模就是装不上(取指一侧的问题, 下一版只查那一侧);
;     6 = 四档连 8 字节记号都读不回来 ⇒ 回读仪器是瞎的, 本轮任何字模结论都不
;         能从 `fontst` 读, 只能从 `fontwin` 与画面读。
;   【为什么"1 = 开场就对"被删了】./182 里"指纹 == FontExp"从来没成立过, 而
;   r182 那个 1 还有个更坏的副作用: 一旦指纹读数(在瞎的那扇窗里)碰巧相等,
;   整条字模路就一次都不跑。r183 不再有"看一眼就跳过"的捷径 —— 每场第一次
;   进来一律走完"记号 → 4KB → 整块回读", 写进去的内容与已存在的字模逐字节
;   相同, 所以"多写一遍"没有代价。
;   `PH: probe fontwin=` 是"第几档窗口站住的"这个读数, 与 fontst 独立:
;     0 = 四档全败(此时 fontst 只可能是 4 或 6);
;     1..4 = 该档既通过了 8 字节记号、也通过了 4KB 整块回读 ⇒ 字模确实装上了
;            (具体是哪一档见 VfcSetCand 的四行); 若此时画面仍花屏, 病根就在
;            取指一侧(CR13/CR17/CR0C/CR0D/SR01/bank), 或者"卡扫描的不是我们
;            读写的那一页" —— 后者正是 `VgaMark` 那一行字符串要回答的问题。
;   【现场】EAX/EBX/ECX/EDX + flags 自己保存恢复(ECX 是档号)。

;=============================================================================
; 【r412】RETIRED(GLYPH: VgaFontProv) —— 字模写入停手（r260 的 A 部分保留, B 部分撤回）
;   依据: r259 已把 5 号那一拍的写卡动作删净, 而手打 mode co80 之后
;   【非 ASCII 仍然空白】 ⇒ 破坏 DBCS 字模的是字模写入过程本身。
;   远东 DBCS 的第二字符集字模住在平面 2 的 1..7 号段; 只要往平面 2 写过
;   8x16 ASCII 字模, 那几段就被覆盖, 而 mode co80 只重装 0 号段。
;   ⇒ 平面 2 完全交给 BIOS 与控制台。本过程本体与判据全部保留, 入口直接返回。
;
;   r260 的 B 部分(M2Probe 挂到 41 VESA_SUPPORT / 47 VESA_POST)【已撤回】:
;   r260 在本机引导期崩在 VMPOLL, 连 VBE9XLOG 都没生成。41/47 是在主 VDD
;   处理 INT 10h 的中途被调的, 在里面再嵌套一次 INT 10h 会造成重入 ⇒
;   探针恢复成 r259 的样子(只挂 15 号), 那是已证明能正常引导的配置。
;=============================================================================
;-----------------------------------------------------------------------------
; r412-marker: VgaFontProv -- canary, 8 KB copy, whole-block verify: four windows, then the BIOS.
; r412-marker: VgaFontProv -- canary, 8 KB copy, whole-block verify: five windows, then the BIOS.
BeginProc VgaFontProv
        ; 【r412】入口直接返回 —— 见上。
        ret
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      vfp_out
        cmp     byte ptr [VgaFixArm], 0
        je      vfp_out
        cmp     byte ptr [FontProvN], 2
        jae     vfp_out
        ; --- 【r183 第五道闸门: 只对字符模式动手】 -------------------------
        ;   复核的第 5 条发现: 这个文件里最重的一支写(最多 5 次记号 + 5 次 4KB)
        ;   原来没有任何"当前是字符模式"的闸门。钩子 5 把硬件交给的是【盒子
        ;   自己的模式】, 那可以是 DOS 图形模式: 此时 base+0..1FFF 就是可见
        ;   显存(mode 13h 里那是屏幕顶部), 而收尾的 VgaM3Apply 还会把文本寄存器
        ;   组写进那个图形模式。判据直接借用 换页前置 的那三道闸门的结果:
        ;   PgN != 0 表示"这一拍是 00..03/07/08..0C/14..FF 的字符模式"。
        ; --- 【r412】第五道闸门: 只对字符模式动手 —— 【自己读】, 不再借别人的 --
        ;   r183 这里读的是 [PgN], 而 PgN 全项目只有一个写者: 换页前置。
        ;   r189 为了做"零写入"实验把 换页前置 从 5 号摘掉之后, PgN 恒为 0
        ;   ⇒ 本过程在 r189..r192 四轮里【一次都没进去过】。这就是为什么
        ;   r192 把 `call VgaFontProv` 放回去也毫无变化:
        ;       ./189../.192 的日志里 fontst / fontwin / fontsr02 / fontsr03
        ;       全是 0000, 而 `TRC fbox font loads (8x16 chargen reloads)` 恒
        ;       为 0 —— 那个 0 不是"装失败了", 是"根本没装"。
        ;   判据改成【自己读】0:0449(BDA 的当前 BIOS 模式字节):
        ;       00..03 / 07 = 字符模式, 动手;
        ;       72h / 73h   = 远东 DBCS 文本(DDK《CHECK_HIRES_MODE》原文写明
        ;                     远东 Windows 用 02h/03h 表达它们), 也动手;
        ;       其余(含 04h..06h、0Dh..13h 与 VBE 模式号)一个字节都不碰。
        mov     edi, 00449h
        mov     al, [edi]
        cmp     al, 003h
        jbe     vfp_mode_ok
        cmp     al, 007h
        je      vfp_mode_ok
        cmp     al, 072h
        je      vfp_mode_ok
        cmp     al, 073h
        jne     vfp_out
vfp_mode_ok:
        ; --- 【r412】"本过程真的进来了" —— 唯一的"这一轮到底测了没有"凭据 --
        ;   落在 Probe 的 PG16 格, 日志里读 `PH: fontrun=`(0 = 又没跑)。
        ;   它与 fontst 独立: fontst 讲"装到第几步", 这一格讲"进没进门"。
        inc     byte ptr [Probe + VBE_TR_PR_PG16]
        ; --- 源字模与期望指纹(必须在阶梯之前: 两档全败时 BIOS 那两枪要用它) --
        ; --- 【r180 的崩溃阶梯】每过一步在 fontst 上落一个数字 -------------
        ;   9 = 进了闸门 / 10 = 正试某一档 / 11 = 正在写 4KB / 12 = 写完了
        mov     byte ptr [Probe + VBE_TR_PR_FONTWIN], 0
        mov     byte ptr [VfcCanOk], 0          ; "至少有一档能写能读"(判 4/6 用)
        mov     byte ptr [Probe + VBE_TR_PR_BOFFL], 9
        mov     ecx, 1
        ; 【为什么这里【不】关中断】这一整段里真正不可打断的是记号与那 4KB,
        ;   两者各自在自己的过程里 pushfd/cli(VfcCanary / VfcLoad)。外面再套
        ;   一层会让本过程出现"半路 pushfd、两个出口各 popfd"的形状, 而
        ;   an158/az_stackpath.py 当场把它报成"ret 落在非零栈深"(REAL-FINDING)。
        ;   栈只留【入口进的、出口出的】那五个, 一个都不多。
vfp_try:
        mov     byte ptr [Probe + VBE_TR_PR_BOFFL], 10
        jc      short vfp_next
        jc      short vfp_next
        mov     byte ptr [Probe + VBE_TR_PR_BOFFL], 11
        ; 【r412】不再用指纹把这一档否掉。依据是 ./185 的两个读数:
        ;   `PH: probe fontp0=1818` 与 `PH: fbox fontfp=1818` —— 逐字节相同,
        ;   即这张卡【忽略 GCR04 的读平面选择】(本文件 r178 那条注早就写过
        ;   这个可能性)。于是任何"回读平面 2 再比对"的判据都会把【正确的】
        ;   那一档也判成失败: 上一版(./185)的五档因此全败, 退回 B0000(偏移 10000h)
        ;   那扇写不到字模段的窗, 整轮等于一个字节都没装进字模段。
        ;   现在只要 VfcCanary(写并读回)成立就接受这一档, 指纹只当观察值。
        jmp     short vfp_win
vfp_next:
        inc     ecx
        cmp     ecx, 2
        ja      short vfp_bios
        ; 【短跳射程】回路从这一句到 vfp_try 现在只有五十几字节, 远不到 127;
        ;   这里仍然写近跳, 是因为回路里任何一次改动都可能把它推过上限, 而
        ;   A2075 是【打包时才】暴露的那一类错误(an153/qc_asm153.py 会量)。
        jmp     vfp_try
vfp_win:
        mov     byte ptr [Probe + VBE_TR_PR_BOFFL], 12
        mov     eax, ecx
        mov     [Probe + VBE_TR_PR_FONTWIN], al  ; 第几档站住的
        mov     byte ptr [FontProvStat], 2
        jmp     vfp_end
vfp_bios:
        ; --- ③ 两档都不行 ⇒ 先把窗口停在第 1 档(唯一写得到字模段的窗), 再问 BIOS --
        ;   复核的第 3 条发现: 原来这里选的是【默认档】(刚刚被记号否掉的那一扇)。
        ;   [旧的字模窗口基址变量]/[旧的窗口 GCR06 影子]/[旧的窗口 SR04 影子] 会留在那里, 而后面不走阶梯的
        ;   VfcLoad(字模一致性检查 由 35 号调用 / VgaFontBoot)还会用它盲写 4KB ——
        ;   那就比 r182 更差。退回第 4 档(0A0000 / GCR06=04h / SR04=07h),
        ;   也就是 r176..r182 用了二十几轮的那一扇。
        ; r412-marker: BIOS fallback window = candidate 1 (A0000), never B0000
        mov     ecx, 1
        mov     byte ptr [BiosFontReq], 004h
        call    VgaBiosFont
        mov     [FontRef], ax
        mov     bx, [FontExp]
        test    bx, bx
        je      short vfp_st4
        cmp     ax, bx
        je      short vfp_st3
        mov     byte ptr [BiosFontReq], 014h
        call    VgaBiosFont
        mov     [FontRef], ax
        mov     bx, [FontExp]
        test    bx, bx
        je      short vfp_st4
        cmp     ax, bx
        je      short vfp_st5
vfp_st4:
        ;   4 = 记号过了(能写能读)但字模就是装不上 ⇒ 取指一侧;
        ;   6 = 连记号都读不回来 ⇒ 回读仪器是瞎的, 本场的字模读数不许当硬件结论。
        ;   【r183 修】这里原来读 Probe+FONTWIN 来分 4/6, 而那一格只有在【整档
        ;   全胜】时才写 ⇒ 永远读到 0 ⇒ stat 永远是 6, 4 不可达。改用 旧的可用性汇总值,
        ;   它在每一档过记号时就置 1(复核的第 2 条发现)。
        mov     byte ptr [FontProvStat], 4
        cmp     byte ptr [VfcCanOk], 0
        jne     short vfp_st4w
        mov     byte ptr [FontProvStat], 6
vfp_st4w:
        jmp     short vfp_end
vfp_st5:
        mov     byte ptr [FontProvStat], 5
        jmp     short vfp_end
vfp_st3:
        mov     byte ptr [FontProvStat], 3
vfp_end:
        mov     al, [FontProvStat]
        mov     byte ptr [Probe + VBE_TR_PR_BOFFL], al
        inc     byte ptr [FontProvN]
vfp_out:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaFontProv

; r412 C7b: 持屏期现场修补 removed.

;-----------------------------------------------------------------------------
; 【r412】VfcFillBanks —— 把 ROM 8x16 字模写进平面 2 的【八个 8K 段】。
;
;   【它为什么存在 —— 两件事凑在一起才看得见】
;   ① ./184 的 `1841.png` / `1842.png` 被逐像素解出来之后(work/an194/pngtile.py):
;      同一屏里**有些格子字形完全正确、有些格子是同一张废物字形**。按颜色
;      区分得干干净净:
;        · 字是白字黑底(attr = 0Fh, bit3 = 1) ⇒ 字形正确;
;        · 空白区(1842 绿底 attr = 27h, 1841 cls 后灰字黑底 attr = 07h,
;          两者 bit3 都 = 0) ⇒ 全部是那张废物字形(9 像素一格、隔行一个点)。
;   ② FreeVGA《Sequencer Registers》对 SR03(Character Map Select)的原文:
;        "Character Set A Select: This field is used to select the font that is
;         used in text mode **when bit 3 of the attribute byte is set to 1**",
;        "Character Set B Select: ... **when bit 3 of the attribute byte is set
;         to 0**",
;      而两个字段各是 3 位, 指向平面 2 的【八个 8K 段】:
;        000b -> 0000h-1FFFh   001b -> 4000h-5FFFh   010b -> 8000h-9FFFh
;        011b -> C000h-DFFFh   100b -> 2000h-3FFFh   101b -> 6000h-7FFFh
;        110b -> A000h-BFFFh   111b -> E000h-FFFFh
;   ⇒ r177..r194 每次都只填【0 号段】。于是: 用 A 字符集的格子正常,
;     用 B 字符集的格子取指取到的是【上一场 Windows 留在这个段里的像素】 ——
;     正是那片竖虚线。这也解释了 `mode co80` 之后"一个字符都看不见":
;     清屏把属性写成 0?h(bit3=0) ⇒ 整屏走 B 字符集 ⇒ 整屏废物字形。
;
;   【本函数做什么】把同一份 256x16 的 ROM 字模, 按 32 字节/格的步长,
;   写进八个 8K 段(0000h..FFFFh 整整 64KB 平面 2)。这样无论 SR03 的
;   A/B 两个字段在各场会话里被盒子写成什么, 取指都能取到一份真字模。
;   段内布局与 平面 2 字模拷贝 完全一致: 前 16 字节是字形, 后 16 字节写 0。
;
;   【闸门】① policyVgaFix != 0; ② VgaFixArm != 0(盒子持屏期间);
;           ③ BDA 0:0449 ∈ {02h, 03h, 07h} —— 【不含 72h/73h】:
;              那两种是远东 DBCS 字符模式, 盒子的双字节字模就住在
;              1..7 号段里, 把它们统统盖成 8x16 ASCII 字模会把中文显示弄坏。
;   【落盘】写完仍把 SR02 读回 `PH: probe fontsr02=`(期望 04h 的那一格是
;     字模窗口; 收尾由 VgaM3Apply 把 SR02 写回 03h)。
;   【现场】EAX/EBX/ECX/EDX/ESI/EDI + flags 自己保存恢复。


;-----------------------------------------------------------------------------
; 【r412】VgaM3Crtc —— 把【标准 mode 3 的 CRTC 25 字节】装回卡上。
;
;   【为什么轮到它】./193 把字模这一侧彻底做通并读回来了:
;       PH: fontrun==0003   fontwin==0001   fntreg==0407
;       PH: probe fontsr02==0004   fontsr03==0000
;   即"平面屏蔽 = 平面 2 / 字符映射 = 0 号段 / SR04=07 / GCR06=04"全部成立,
;   而画面与 r173 逐像素相同。同一份日志里唯一能读到的文本态是
;       p5sr04=02  p5gcr06=0E  p5sr01=00  p5ar10=0C
;   —— 显存【视图】这一层是对的。从来没读过、也从没写过的是 CRTC 的
;   25 个时序寄存器(CR00..CR18): 水平显示结束、垂直总数、字符高度、
;   起始地址全在里面。几何错的表现恰好就是"字符、属性、字模都对,
;   屏幕上却是一格格的竖线/花屏, 而且 cls 之后底色变了、字形不变"。
;
;   【这 25 个字节从哪来 —— 三个独立来源逐字节一致】
;   vga-register-reference.md 第 6c 节把三条独立的 mode 3 数据表对过:
;       QEMU vgabios、VirtualBox、MODES.ASM(1997, 不调 BIOS 直接设模式)
;   三者对 CRTC 00h-18h 的 25 个字节完全一致:
;       5F 4F 50 82 55 81 BF 1F 00 4F 0D 0E 00 00 00 00 9C 8E 8F 28 1F 96 B9 A3 FF
;   即 720x400 / 80x25 / 9x16 / 起始地址 0 —— 正是 DOS 盒子要的那一屏。
;   序号 11 = CR11h = 8Eh: 位 7 是【写保护】。所以先写 0Eh 解锁, 再写
;   00h..0Ah, 最后写 8Eh 重新锁上 —— 这是 VGA BIOS 设模式的标准顺序,
;   否则写保护会把前面那些字节全部吃掉。
;
;   【闸门】① policyVgaFix != 0(默认 1, 用户可用 VgaFix=0 一行关掉);
;           ② VgaFixArm != 0(盒子持屏期间才动, 回到 Windows 后绝不碰);
;           ③ BDA 0:0449 是 02h/03h/07h(字符模式)。
;   【只写的】3C2h(Misc, 67h) + CRTC 00h..18h。不碰属性控制器(那里的写会
;     动 PAS 位、改调色板), 不碰 SR00/SR01(9 点字符时钟实测已是 00h),
;     不碰显存。
;   【落盘】写之前先把 CR13 / CR01 读进 `PH: probe nbnt=`(高字节 CR13,
;     低字节 CR01), 写之后再读一遍进 `PH: probe nxna=`。这两格在 r412
;     之前恒为 0(死仪器), 从此有含义: 期望两格都是 28 / 4F。
;     判读:
;       nbnt≠284F 而 nxna=284F ⇒ 几何原本是错的, 本版把它装对了(画面应变);
;       nbnt=nxna=284F        ⇒ 几何本来就是对的 ⇒ 病灶不在 CRTC 时序;
;       nxna≠284F             ⇒ 连写都写不进去(写保护/端口被虚拟化)。
;   【现场】EAX/EBX/ECX/EDX/EDI + flags 自己保存恢复。
;-----------------------------------------------------------------------------
; r412-marker: VgaM3Crtc -- the canonical mode 3 CRTC table, three sources agree.
BeginProc VgaM3Crtc
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        cmp     byte ptr [VbeState.VBS_policyVgaFix], 0
        je      vm3_out
        cmp     byte ptr [VgaFixArm], 0
        je      vm3_out
        mov     edi, 00449h
        mov     al, [edi]
        cmp     al, 002h
        je      short vm3_go
        cmp     al, 003h
        je      short vm3_go
        cmp     al, 007h
        jne     vm3_out
vm3_go:
        ; --- 0. 先读回 CR13 / CR01(改之前的样子) ---------------------------
        mov     dx, 03D4h
        mov     al, 013h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [Probe + VBE_TR_PR_NB], al
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [Probe + VBE_TR_PR_NT], al
        dec     dx
        ; --- 1. 【r412 刻意不写 3C2h】(原稿写了 67h, 被 CHECK.bat 第 8431 行
        ;   的纪律挡下): 杂项输出寄存器【读不回来】, 而 r169 的实测是
        ;   "写过 3C2h 之后 `mode co80` 的自救就失灵了"。这条纪律比"按
        ;   VGA BIOS 的顺序完整设模式"更重要 —— 时钟/内存映射位既然在
        ;   hook 4/5 的读数里已经是文本态该有的样子, 就不该由我们重写。
        ; --- 2. CR11h 解锁(位 7 = 0) --------------------------------------
        mov     dx, 03D4h
        mov     al, 011h
        out     dx, al
        inc     dx
        mov     al, 00Eh
        out     dx, al
        dec     dx
        ; --- 3. CR00h..CR0Ah(时序与脉冲宽度), 表里每项是"下标, 数据" ------
        mov     ebx, OFFSET32 VgaM3CrtcTab
        mov     ecx, 0000Bh
vm3_lo:
        mov     al, [ebx]
        out     dx, al
        inc     dx
        inc     ebx
        mov     al, [ebx]
        out     dx, al
        dec     dx
        inc     ebx
        dec     ecx
        jnz     short vm3_lo
        ; --- 4. CR11h = 8Eh(重新锁上) --------------------------------------
        mov     al, 011h
        out     dx, al
        inc     dx
        mov     al, 08Eh
        out     dx, al
        dec     dx
        ; --- 5. CR12h..CR18h(垂直时序 / 字符高度 / 起始地址 / 模式控制) ----
        mov     ebx, OFFSET32 VgaM3CrtcTab
        add     ebx, 00016h                 ; 第 11 项之后 = 11 * 2 字节
        mov     ecx, 00007h
vm3_hi:
        mov     al, [ebx]
        out     dx, al
        inc     dx
        inc     ebx
        mov     al, [ebx]
        out     dx, al
        dec     dx
        inc     ebx
        dec     ecx
        jnz     short vm3_hi
        ; --- 6. 读回 CR13 / CR01(改之后的样子) -----------------------------
        mov     al, 013h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [Probe + VBE_TR_PR_NX], al
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [Probe + VBE_TR_PR_NA], al
vm3_out:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaM3Crtc

;   【r412】mode 3 的 CRTC 数据表(每项: 下标, 数据)。CR11h(写保护)不在这里,
;   它被单独写两次(解锁 0Eh / 加锁 8Eh), 见上。
VgaM3CrtcTab       db 000h,05Fh, 001h,04Fh, 002h,050h, 003h,082h
                   db 004h,055h, 005h,081h, 006h,0BFh, 007h,01Fh
                   db 008h,000h, 009h,04Fh, 00Ah,00Dh
                   db 012h,08Fh, 013h,028h, 014h,01Fh, 015h,096h
                   db 016h,0B9h, 017h,0A3h, 018h,0FFh



;-----------------------------------------------------------------------------
; 【r182】全屏文本页 —— 从"写寄存器"走到"让这一页真的能用"。
;
;   【一句话】这台机器上 DDK 第 6 步那次 mode change 只做了三件事里的第一件:
;   写寄存器。字模(r164 起)和"把整页写成空格"(本轮)都由我们补。
;   证据链(每一条都是只读读数或用户自己的实验):
;     · ./173 的 1731.png 按 9x16 解码: 模式/几何/字模/属性/调色板全在工作,
;       坏的是页里的【内容】;
;     · 用户自己的 cls 实验: 把平面 0/1 的 0..0F9Fh 写成 (20h,07h) 就能恢复;
;     · ./181 的 nbnt/nxna: 五次进全屏读到的页面读数一模一样(0 个 00h 字符、
;       232 个 00h 属性), 而 nxna 与 nbnt 恒等 ⇒ 那一页从头到尾没人写过。
;
;   【硬边界: 这条路永远不许碰平面 2】下面所有读写过程都先 call
;   VfcTextGroup, 于是 CPU 视图是标准文本态(GCR06=0Eh / SR02=03h /
;   SR04=02h / GCR05=10h / GCR04=00h): 顺序读写 0B8000h 得到的是
;   "字符, 属性, 字符, ..." 交错字节, 而映射掩码 03h 只开平面 0/1 的写。
;   字模在平面 2 —— 这一组操作无论怎么写都碰不到它; 而且清页之后
;   换页后置 会重新读一次字模指纹来证明这一条。
;
;   【读数】见数据区 旧的逐格读数暂存 上方那几行的说明(VBE_TR_PR_NB/NT/NX/NA/CELLS)。
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; VgaPageStartRead —— 读 CR0Ch/CR0Dh(显示起始地址)。出口 AX = CR0D:CR0C。
;   读 CRTC 的索引/数据口不需要先解 CR11 bit7(那一位只保护 CR00..CR07 的写),
;   所以这里既不写 CR11 也不碰任何别的寄存器。现场: ECX/EDX 自己保存恢复。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageStartRead -- CR0C/CR0D, the display start of the text mode.
BeginProc VgaPageStartRead
        pushfd
        push    ecx
        push    edx
        cli                                 ; 3D4h 的索引锁存是共享状态: 与
                                            ;   VgaPageStat 等过程同一纪律
        mov     dx, 03D4h
        mov     al, 00Ch
        out     dx, al
        inc     dx
        in      al, dx                      ; CR0C = 起始地址低位
        mov     cl, al
        dec     dx
        mov     al, 00Dh
        out     dx, al
        inc     dx
        in      al, dx                      ; CR0D = 起始地址高位
        mov     ch, al
        mov     ax, cx
        pop     edx
        pop     ecx
        popfd
        ret
EndProc VgaPageStartRead

;-----------------------------------------------------------------------------
; 【r182】VgaPageBitSet —— 把 AL 那一字节在 32 字节位图里对应的那一位置 1。
;   【为什么要位图】"这一页里出现过多少种不同的字符字节/属性字节"是唯一能
;   分开"文本页"与"看着像文本的像素残留"的量: 一页 DOS 文本屏的字符种类数
;   通常几十种、属性种类数只有几种; 而 16bpp 像素残留的低/高字节会铺满
;   一两百种。r181 用的"00h 格数"把它们分不开 —— ./181 的 nbnt=001D 就是
;   一份像素残留给出的读数, 而它落在 r181 的闸门外面。
;   【现场】只改 EAX 与 EDX; ECX/EBX/ESI/EDI 一个字节都不动(调用者的循环
;   计数器就在 ECX/ESI 上)。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageBitSet -- set one bit in a 32-byte (256-bit) bitmap.
BeginProc VgaPageBitSet
        push    ecx
        movzx   edx, al
        mov     ecx, edx
        and     ecx, 7
        shr     edx, 3
        mov     al, 1
        shl     al, cl
        or      byte ptr [edi + edx], al
        pop     ecx
        ret
EndProc VgaPageBitSet

;-----------------------------------------------------------------------------
; 【r182】VgaPageBits —— 数一张 32 字节位图里被置起来的位。入口 EDI = 基址,
;   出口 AX = 0..256。不查表(本项目不许"按布局读数据"): 就是 256 次移位 +
;   条件加一。
;   【现场】只改 EAX/ECX/EDX/ESI; EBX 与 EDI 不进来(EBX 在调用者那边就是
;   规范格计数器)。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageBits -- population count of a 256-bit bitmap.
BeginProc VgaPageBits
        push    ecx
        push    edx
        push    esi
        mov     esi, edi
        mov     ecx, 32
        xor     edx, edx
vpb_byte:
        mov     al, [esi]
        mov     ah, 8
vpb_bit:
        shr     al, 1
        jnc     short vpb_skip
        inc     edx
vpb_skip:
        dec     ah
        jnz     short vpb_bit
        inc     esi
        dec     ecx
        jnz     short vpb_byte
        mov     eax, edx
        pop     esi
        pop     edx
        pop     ecx
        ret
EndProc VgaPageBits



; r412 C7a: 文本尾部校验 removed -- zero callers once the Vfc family was deleted.

;-----------------------------------------------------------------------------
; 【r182】VgaPageStat —— 一遍扫完 80x25 格, 同时得到三个量(落进数据区):
;     PgCan = "规范格"数 —— 字符 20h 且属性 07h, 也就是一次 BIOS mode 3
;             清页之后整页该有的样子;
;     PgChr = 出现过的字符字节种类数(0..256);
;     PgAtt = 出现过的属性字节种类数(0..256)。
;   【为什么一遍】5 号里只跑一次, 而一页是 2000 格 x 2 字节; 分两遍扫等于
;   把这一拍的时间翻倍, 没有理由。
;   【只经 VfcTextGroup】与 r181 逐条相同: 先写回标准文本态(GCR06=0Eh /
;   SR02=03h / SR04=02h / GCR05=10h / GCR04=00h), 于是读的是"CPU 在 0B8000h
;   看到的字符/属性", 不是某个图形孔径里的别的东西。
;   【硬边界: 这条读路径碰不到平面 2】SR02=03h 只开平面 0/1 的写; 读在这一组
;   寄存器下走的是奇偶地址, 每格 = (字符, 属性) 两字节。字模在平面 2。
;   【现场】EAX/EBX/ECX/EDX/ESI/EDI + flags 全部自己保存恢复; 全程 cli。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageStat -- canonical cells + distinct char/attr bytes, one pass.
BeginProc VgaPageStat
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        cli
        ; --- ① 清两张位图(各 32 字节) --------------------------------------
        ;   【分两次清, 不假设两张位图相邻】一次 16 个 dword 的 rep stosd 会
        ;   跳过第二张图的边界去写布局 —— 那正是 r179 蓝屏那一类错(表的数据
        ;   布局 ≠ 读它的代码)。这里两条 rep stosd, 各自的基址都是符号。
        cld
        xor     eax, eax
        mov     edi, OFFSET32 PgChrM
        mov     ecx, 8
        rep     stosd
        mov     edi, OFFSET32 PgAttM
        mov     ecx, 8
        rep     stosd
        ; --- ② 扫 2000 格 -------------------------------------------------
        ;   【007D0h = 2000, 不是 00219h】80x25 = 2000 格 = 07D0h。r181 在这里
        ;   写的是 00219h(= 512), 于是它只扫了前 512 格(第 0..5 行 + 第 6 行
        ;   的前 32 格) —— ./181 的 `nbnt=001D` 那 232 个属性为 0 的格是对
        ;   【512 格】说的(45%), 不是对 2000 格说的。本轮一并改正。
        xor     ebx, ebx                        ; EBX = 规范格数
        mov     esi, 000B8000h
        mov     ecx, 007D0h
vps_loop:
        mov     al, [esi]
        mov     edi, OFFSET32 PgChrM
        call    VgaPageBitSet
        mov     al, [esi + 1]
        mov     edi, OFFSET32 PgAttM
        call    VgaPageBitSet
        cmp     byte ptr [esi], 020h
        jne     short vps_next
        cmp     byte ptr [esi + 1], 007h
        jne     short vps_next
        inc     ebx
vps_next:
        add     esi, 2
        dec     ecx
        jnz     short vps_loop
        ; --- ③ 两张位图各数一次 -------------------------------------------
        mov     edi, OFFSET32 PgChrM
        call    VgaPageBits
        mov     [PgChr], ax
        mov     edi, OFFSET32 PgAttM
        call    VgaPageBits
        mov     [PgAtt], ax
        mov     [PgCan], bx
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaPageStat

;-----------------------------------------------------------------------------
; VgaPageSave —— 4000 字节(80x25 交错页)的快照。
;   【为什么必须有这一步】r164 起我们在 5 号里写 4KB 字模, 而字模与文本页
;   在显存里【共用同一段偏移】(平面 2 与平面 0/1 都在 0..0FFFh 一带)。
;   写字模那一组寄存器写着 SR02=04h(只开平面 2), 可 ./180 的
;   `PH: probe fontp0=` 与 `fontfp` 恒等 —— 这张卡上的回读并不能证明掩码
;   生效。于是"我们自己的 4KB 拷贝把平面 0/1 也写脏了"这件事【必须被
;   排除】, 而排除的办法不是再信一次掩码, 是先存后还: 进门存一份,
;   字模装完原样写回, 于是页面不可能比进门时更差。
;   【r182 起没有"写回"那半边了】判定改成"页面被改过就吃现场读数"之后,
;   写回要么是空转(页面没变), 要么紧接着就被清页覆盖 —— 它不再是一个动作。
;   PgSave 现在只服务 VgaPageDiff 这一件事。
;   【现场】ECX/ESI/EDI + flags; 全程 cli; 只经 VfcTextGroup。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageSave -- 4000-byte snapshot of the visible text page.
BeginProc VgaPageSave
        pushfd
        push    ecx
        push    esi
        push    edi
        cli
        mov     esi, 000B8000h
        mov     edi, OFFSET32 PgSave
        mov     ecx, 0FA0h
        cld
        rep     movsb
        pop     edi
        pop     esi
        pop     ecx
        popfd
        ret
EndProc VgaPageSave

;-----------------------------------------------------------------------------
; 【r182】VgaPageDiff —— 快照与现场逐字节比较, 数出不同的字节数(PgDif)。
;   它回答 r181 用两个计数近似回答的那个问题: 【这一拍里有没有别人写页面】
;   (盒子、主 VDD、或者我们自己的字模拷贝)。同一个读数顺带给 cells 的
;   bit7 —— 若它非 0, 说明清页可能冲掉的是"别人刚写进去的东西", 那时下一轮
;   就该把清页放回闸门后面。
;   【现场】EAX/ECX/EDX/ESI/EDI + flags; 全程 cli; 只经 VfcTextGroup。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageDiff -- how many bytes the page changed since the snapshot.
BeginProc VgaPageDiff
        pushfd
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        cli
        mov     esi, 000B8000h
        mov     edi, OFFSET32 PgSave
        xor     edx, edx
        mov     ecx, 0FA0h
vpd_loop:
        mov     al, [esi]
        cmp     al, [edi]
        je      short vpd_same
        inc     edx
vpd_same:
        inc     esi
        inc     edi
        dec     ecx
        jnz     short vpd_loop
        mov     [PgDif], dx
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        popfd
        ret
EndProc VgaPageDiff

;-----------------------------------------------------------------------------
; VgaPageClear —— 把整页写成 (空格, 属性 07h)。这正是真正的 BIOS mode 3
;   设模式会做的那一步(也是用户手里的 cls 的效果), 只是由我们在 5 号补上。
;   【为什么是 0720h】DOS 缺省属性 = 07h(黑底灰字), 空格 = 20h; 一次
;   word 写 = 字符 + 属性。文本态下 A0 选平面(0 号字节进平面 0, 1 号进
;   平面 1), 而 SR02=03h 把平面 2/3 关在外面 ⇒ 字模不受影响(清完之后
;   换页后置 会重新读一次字模指纹来证明这一条)。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageClear -- clear the page to space + attribute 07h.
BeginProc VgaPageClear
        pushfd
        push    eax
        push    ecx
        push    edi
        cli
        mov     edi, 000B8000h
        mov     ecx, 007D0h                 ; 2000 格 = 07D0h(不是 00219h!)
        mov     ax, 0720h                   ; 低字节 20h(空格) 高字节 07h(属性)
vpc_loop:
        mov     word ptr [edi], ax
        add     edi, 2
        dec     ecx
        jnz     short vpc_loop
        pop     edi
        pop     ecx
        pop     eax
        popfd
        ret
EndProc VgaPageClear

;-----------------------------------------------------------------------------
; 【r182】VgaPageJudge —— "这一页看着就是一页真文本吗"。
;   入口: AX = 规范格数(字符 20h 且属性 07h), CX = 属性种类数。
;   出口: CF = 1 ⇒ 是文本页(不许清); CF = 0 ⇒ 不是(清)。
;   【判据只有两条, 而且这是刻意的】
;     ① 规范格 >= 1000/2000 —— 一页真文本屏过半是"空格 + 07h"; 桌面像素残留
;        是"一万个字节里没有一个重复到一千次"的东西, 这一条它过不去;
;     ② 属性种类 <= 48 种 —— 一页 DOS 屏的属性就那几种(07h/17h/1Fh/70h...);
;        像素残留的高字节会铺满几十上百种。
;   【为什么没有"字符种类"这一条】这台机器是中文 Win98(DBCS, 远东模式 72h),
;   一页正确的中文文本屏里, 字符平面同时装着前导字节与后继字节, 种类数 routinely
;   超过 180 —— 用"字符种类少"当判据会在【正确的页面】上开火, 等于把闸门写反。
;   字符种类数仍然采(PgChr, 落进 `PH: probe nbnt=` 的高字节), 但它只做读数。
;   【现场】只读 AX/CX 与 flags 的 CF; 不碰任何寄存器与内存。
;-----------------------------------------------------------------------------
; r412-marker: VgaPageJudge -- two tests; CF=1 means "this already is a text page".
BeginProc VgaPageJudge
        cmp     ax, 003E8h                  ; 1000 / 2000
        jb      short vpj_not
        cmp     cx, 48
        ja      short vpj_not
        stc
        ret
vpj_not:
        clc
        ret
EndProc VgaPageJudge

; r412 D: 换页前置 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 换页后置 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r183】VgaMark —— 在页面最后一行的右侧写 "vbe9x r183"(10 格, 属性 07h)。
;
;   【它为什么存在】./182 把"平面 2 里的字模"和"页面内容"分别测过了, 但有一件
;   事两个仪器都测不到:【我们的 CPU 写到底有没有落到卡正在扫描的那一页上】。
;   同一份日志里, 我们按文本组寄存器读 0B8000h 得到一页 2000/2000 规范的空页,
;   而屏幕上是花屏 —— 这两件事只有三种解释:
;     ① 我们读写的偏移与卡取指的偏移不是同一处(CR0C/CR0D、或是显存映射的
;        N 位把窗口挪到了别处);
;     ② 卡根本没在扫描我们以为的那一页;
;     ③ 页面确实是一页空页, 花屏全部来自平面 2(那就是本轮的字模路径)。
;   一行字符串就能把 ①② 与 ③ 分开:
;     · 截图上看到 `vbe9x r183` ⇒ 我们的写与卡的取指是同一处(③ 成立);
;     · 看不到 ⇒ 是 ①②, 下一版只查 CR0C/CR0D/CR11 与 VBE 的显示起点。
;   【代价】覆盖第 24 行第 70..79 列。盒子进全屏后会自己重画这一行, 所以它只是
;   一个"进门那一刻"的记号; 关掉它的开关与页面/字模两条路相同: `Int10Fix=0`。
;   【现场】EAX/ECX/EDX/ESI/EDI + flags 全部自己保存恢复; 只经 VfcTextGroup。
;-----------------------------------------------------------------------------
; r412-marker: VgaMark -- put "vbe9x r183" on the last line so a screenshot can prove it.
BeginProc VgaMark
        pushfd
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      short vmk_out
        ;   同一个"这是字符模式"凭据: PgN != 0 表示 换页前置 过了三道闸门。
        ;   没有它, 一个 VGA 图形程序的画面会被这 20 个字节改掉(基址 0B8F8Ch 在
        ;   mode 12h 的 150KB 显存里)。
        cmp     byte ptr [PgN], 0
        je      short vmk_out
        mov     edi, 000B8F8Ch                  ; 0B8000h + 24*160 + 70*2
        mov     esi, OFFSET32 MarkText
        mov     ecx, MarkLen
vmk_loop:
        mov     al, [esi]
        mov     [edi], al
        mov     byte ptr [edi + 1], 007h
        inc     esi
        add     edi, 2
        dec     ecx
        jnz     short vmk_loop
vmk_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        popfd
        ret
EndProc VgaMark






;-----------------------------------------------------------------------------
; VfcLoad —— 从 BIOS 的 INT 43h 向量(或经典的 F000:FA6E)把 256 个 8x16 字模
;   装进平面 2 的 0 号字库, 然后写回文本组的六个寄存器并重记指纹。
;   只写平面 2: SR02 = 04(映射掩码只开平面 2), SR04 = 字模阶梯那一档(默认 06h), GCR05 = 00,
;   GCR06/基址 = 字模阶梯找出来的那一档, 收尾恢复文本组。
;   源地址不可信(段 < C000h 或落不到 ROM 区)时用 F000:FA6E。




; r412 C2: 平面 2 字模拷贝 removed. It copied the ROM 8x16 font into plane 2 (the C3 旧的字模窗口基址变量
;   work that r264 already showed used a physical address as a flat pointer). The driver
;   never needed to install a font: in-box text modes get their glyphs from the BIOS, and
;   the full-screen repair is done by the in-box TSR.



; r412 D: BIOS 状态回放 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 模式设置插入 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).
; r412 C7a: 字模银行探测 removed -- zero callers once the Vfc family was deleted.
;-----------------------------------------------------------------------------
; 【r412】窗口基址探测 —— 自证孔径的测量: 四个地址各写一个"声明自己身份"
;   的记号, 再原样读回。折回不再靠推断, 而是【记号的内容】自己说出来。
;
;   四个地址: 基址+0000h / +2000h / +4000h / +6000h, 各写 1 个字节:
;   00h / 20h / 40h / 60h(就是它自己偏移的高字节)。读回如果还是这一组值,
;   说明没有折回; 如果 2000h 位置上读回 20h 却出现在 0000h, 那折回就在数据里。
;   两次: ① GCR06=04h(A0000 窗口) ② GCR06=0Ch(B8000 32K 窗口)。
;   只写这 4 个字节/次, 不碰 CRTC、不碰属性控制器、不碰页面。
;   闸门: ApertureProbe != 0 + 持屏中 + 每场一次; 现场 EAX/ECX/EDX/EDI + flags 自保。
;   读数: PH: r412 p0..p3 (窗口①) 与 q0..q3 (窗口②)。

; r412 C7b: 窗口基址探测 removed.
;-----------------------------------------------------------------------------
; 【r412】r205 的 VBE 窗口探测已删除。
;   为什么: VBE 3.0 里 4F06h 是 Set/Get Logical Scan Line Length(扫描线长度),
;   不是粒度; 4F05h 的 BH 是 set/get 选择位(BH=00h 写, BH=01h 读),
;   所以 r205 那三抡里的 BX=0000h/0001h 实际上是"把窗口 bank 写成 DX"。
;   ./205 的日志里 `PH: r205 ax=0002`(AH!=4Fh)与 fp==crt==6060 同时出现,
;   就是"调用不成功 + 读的是别人的槽" 这两件事。
;-----------------------------------------------------------------------------
; 【r178】VgaBiosInt10 —— 一次嵌套 V86 INT 10h 的通用铲子。
;
;   与 BIOS 状态回放 逐条同形(那一套已被 ./176 的 `bios3=0001` 证明能真执行):
;   Get_Cur_VM_Handle → 查 IVT[10h] → Push_Client_State → Begin_Nest_V86_Exec
;   → 填客户端 AX/BX/CX/DX/ES → Exec_Int → 取回 AX → End_Nest_Exec
;   → Pop_Client_State。
;   DDK《PRE_HIRES_TO_VGA (Function 4)》页末原文要求"在调 BIOS 之前
;   Get_Cur_VM_Handle 并把 EBP 指向该 VM 的客户端寄存器", 这里逐字照办。
;
;   入口: [BcAx] [BcBx] [BcCx] [BcDx] [BcEs] 已由调用者写好。
;   出口: [BcRet] = BIOS 回来的 AX; [BcOk] = 1 执行了 / 2 没有 IVT。
;   【现场】EAX/EBX/ECX/EDX/ESI/EDI/EBP + flags 全部自己保存恢复。
;-----------------------------------------------------------------------------
; r178-marker: VgaBiosInt10 -- parameterised nested V86 INT 10h.
;-----------------------------------------------------------------------------
; 【r412】VgaPostTail —— POST 的最后一笔: 把"写路径"按标准 mode 3 收尾并读回
;
;   【为什么偏偏是这一组】一字节写进显存, 存下来的值不等于写下去的值, 只可能由
;   Graphics Controller 这几个字节造成(r167 的 VgaM3Apply 头注已逐条列出):
;     GCR00/GCR01  set/reset 与"置位使能"。GCR01 != 0 时 CPU 写显存存下来的是
;                  GCR00 那个值 —— 于是"整屏每一格都是同一个图案";
;     GCR03        数据旋转 + 功能选择。非 0 时每个字节先旋转再 AND/OR/XOR;
;     GCR08        位屏蔽。非 FFh 时一部分位根本写不进去;
;     GCR02        颜色比较(只影响读回)。
;   VBE 线性模式(Windows 桌面)会把这几个摆成它自己那一套; 只要有任何一个字节
;   留到盒子持屏期, 盒子的字符/属性/字模写入全部变成"别的东西"。DDK 第 4 步
;   "The mini-VDD may if it needs to set the screen out of hi-res mode" 说的就是
;   这一层状态, 而 5 号(POST)是它之后我们最后一次能动硬件的机会。
;
;   【做什么】读 GCR00/GCR01/GCR02/GCR03/GCR08 + SR02 六个字节; 与标准 mode 3
;   的期望值(00 00 00 00 FF / 03)逐个比; 只要一个不等, 就调 r167 的整组写
;   (VgaM3Apply) 写一遍, 再把这六个字节读回来。写前/写后两套读数各自落盘。
;   【闸门】CR17 bit6 == 0(CRTC 此刻在文本形态)。图形态一个字节都不碰。
;   【现场】EAX/EBX/ECX/EDX/ESI/EDI + flags 全自保; 不碰 EBP。
;-----------------------------------------------------------------------------
; r412-marker: VgaPostTail -- assert the GC write path as the last POST act.
;-----------------------------------------------------------------------------
; 【r412】Sr3Keep —— 把客户机自己的 SR03(Character Map Select)存下来、再还回去
;
;   【为什么】SR03 的 bits1-0 / bits3-2 选的是【A / B 两个字符集】的基址,
;   而 attr 字节 bit3 决定一个字符走 A 还是走 B。./194 的逐像素分析:
;   bit3 = 1 的格子字形正确、bit3 = 0 的格子是同一张废物字形 ——
;   即 B 字符集那一块没有可用字模。
;
;   而本项目自己的 POST 链(VgaM3Apply / VfcTextGroup / VgaMark)里 SR03 是硬写 00h 的 ——
;   也就是说:【我们把客户机(BIOS 为 DBCS 文本模式)摆好的字符集映射盖掉了】。
;   DDK《About Virtual Display Minidrivers》说的是"从它的所有者那里恢复"——
;   SR03 属于客户机的字符集选择, 不属于我们。
;
;   【做什么】
;     AL = 0: 在 4 号(PRE, 我们任何一笔写之前) 把 SR03 存进 Sr3Saved, 原值落盘;
;     AL = 1: 在 5 号的最后把它写回去, 并读回自证(相等 ⇒ CSST bit0)。
;   【现场】EAX/EDX + flags 自保; 不碰 EBX/ECX/ESI/EDI/EBP。
;-----------------------------------------------------------------------------
; r412-marker: Sr3Keep -- save/restore the guest's character-map select.
BeginProc Sr3Keep
        pushfd
        push    eax
        push    edx
        mov     dx, 03C4h
        test    al, al
        jnz     short sr3_restore
        ; --- 存 -----------------------------------------------------------
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     [Sr3Saved], al
        mov     [Probe + VBE_TR_PR_CS03], al
        test    al, al
        jz      short sr3_out
        or      byte ptr [Probe + VBE_TR_PR_CSST], 002h   ; 确实在分字符集
        jmp     short sr3_out
sr3_restore:
        ; --- 还 -----------------------------------------------------------
        mov     al, [Sr3Saved]
        mov     ah, al
        mov     al, 003h
        out     dx, al
        inc     dx
        mov     al, ah
        out     dx, al
        dec     dx
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        cmp     al, [Sr3Saved]
        jne     short sr3_out
        or      byte ptr [Probe + VBE_TR_PR_CSST], 001h
sr3_out:
        pop     edx
        pop     eax
        popfd
        ret
EndProc Sr3Keep

;-----------------------------------------------------------------------------
; 【r412】字模窗口克隆 —— 把 A 字符集的 4KB 字模整块复制到 B 字符集那一块
;
;   【它回答的就是已测到的那条不对称】
;     ./206b: `PH: r206 cs03==001C` —— 4 号进门那一刻客户机的 SR03 = 1Ch:
;       A = (1Ch & 3) = 0, bit4 = 1  ⇒ 线性 8002h  ⇒ 平面 2 字节 2000h
;       B = (1Ch>>2)&3 = 3, bit5 = 0 ⇒ 线性 30002h ⇒ 平面 2 字节 C000h
;     ./194 逐像素: attr bit3 = 1(走 A)的格子字形正确, bit3 = 0(走 B)的格子
;     是同一张废物字形 ⇒【A 那块有字模, B 那块没有】。
;
;   【做什么】把 A 那 4KB(平面 2 字节 2000h..3FFFh)整块复制到 B(..C000h..DFFFh),
;   然后用 'A' 字模那个 word 读回自证。【SR03 一个字节都不改】——
;   它是客户机的东西, 我们只把它指着的地方填上字模。
;
;   【怎么写进去】借 r183 已验证的字模窗口那一套(VfcAccessOn / VfcTextGroup),
;   但临时把 [旧的窗口 GCR06 影子]=04h(A0000 64K)、[旧的窗口 SR04 影子]=06h、[旧的字模窗口基址变量]=0A0000h ——
;   原默认是 0Ch(B8000 32K), 而 B 在 C000h,超出 32K 窗口。用完再把三个临时值还回。
;   平面 2 的寻址: SR02=04(只写平面 2) + GCR04=02(读平面 2) + SR04=06(奇偶/链四都关)
;   ⇒ CPU 偏移 n 落在 vram[4n+2], 即平面 2 的第 n 个字节。
;
;   【闸门】Sr3Saved == 0 ⇒ 说明卡没分字符集, 一个字节都不写。
;   【现场】EAX/EBX/ECX/EDX/ESI/EDI + flags 全自保; 不碰 EBP。
;   【读数】VBE_TR_PR_CSST: bit3 复制过 / bit4 复制后相等 /
;     bit5 A==B 不用复制 / bit6 Sr3Saved==0 / bit7 复制前两边不同

; r412 C7b: 字模窗口克隆 removed.

; r412 D: 基址标注 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

BeginProc VgaPostTail
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        ; --- ① 闸门: CR17 bit6 ------------------------------------------
        mov     dx, 03D4h
        mov     al, 017h
        out     dx, al
        inc     dx
        in      al, dx
        test    al, 040h
        jz      short vwpt_text
        jmp     vwpt_out
vwpt_text:
        ; --- ② 写前读数: GCR00 GCR01 GCR02 GCR03 GCR08 ------------------
        mov     dx, 03CEh
        xor     al, al
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 008h
        out     dx, al
        inc     dx
        in      al, dx
        mov     dx, 03C4h                      ; SR02 写平面屏蔽
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        ; --- ③ 已经是标准 mode 3 就一个字节都不写 -----------------------
        cmp     al, 003h
        jne     short vwpt_bad
        jne     short vwpt_bad
        jne     short vwpt_bad
        jne     short vwpt_bad
        jne     short vwpt_bad
        jne     short vwpt_bad
        jmp     vwpt_out
vwpt_bad:
        call    VgaM3Apply
        ; --- ④ 写后读数 -------------------------------------------------
        mov     dx, 03CEh
        xor     al, al
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        dec     dx
        mov     al, 008h
        out     dx, al
        inc     dx
        in      al, dx
        mov     dx, 03C4h
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        ; --- ⑤ 写后自证: 六个字节是否都到位 -----------------------------
        cmp     al, 003h
        jne     vwpt_out
        jne     vwpt_out
vwpt_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaPostTail

BeginProc VgaBiosInt10
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     byte ptr [BcOk], 0
        mov     word ptr [BcRet], 0
        VMMCall Get_Cur_VM_Handle               ; EBX = 当前 VM
        mov     eax, 10h
        VMMCall Get_V86_Int_Vector              ; CX:DX = 目标
        test    cx, cx
        jnz     short vbi_have
        mov     byte ptr [BcOk], 2
        jmp     vbi_out                         ; 近跳: 跨着整个嵌套执行段(~184B), `short` 会 A2075
vbi_have:
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        mov     ax, [BcAx]
        mov     word ptr [ebp].Client_ax, ax
        mov     ax, [BcBx]
        mov     word ptr [ebp].Client_bx, ax
        mov     ax, [BcCx]
        mov     word ptr [ebp].Client_cx, ax
        mov     ax, [BcDx]
        mov     word ptr [ebp].Client_dx, ax
        mov     ax, [BcEs]
        mov     word ptr [ebp].Client_es, ax
        mov     word ptr [ebp].Client_si, 0
        mov     word ptr [ebp].Client_di, 0
        mov     word ptr [ebp].Client_ds, 0
        mov     eax, 10h
        VMMCall Exec_Int
        mov     ax, word ptr [ebp].Client_ax     ; BIOS 回来的 AX
        mov     [BcRet], ax
        VMMCall Get_Cur_VM_Handle               ; End_Nest_Exec 要 EBX = VM
        VMMCall End_Nest_Exec
        Pop_Client_State
        mov     byte ptr [BcOk], 1
vbi_out:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaBiosInt10
;-----------------------------------------------------------------------------
; 【r178】VgaBiosFont —— 让【卡自己的 BIOS】把 8x16 ROM 字模装进平面 2。
;
;   【r177 那一枪为什么不可能成功】r177 发的是 `INT 10h AX=1122h`。
;   AH=11h 这一族里 AL=11h/12h/14h 才是"把 ROM 字形装进字符发生器",
;   而 AL=21h/22h/23h/24h 是"设置 INT 43h 指针"。最硬的证据是参考实现的
;   源码本身 —— Bochs/QEMU VGABIOS 的 vgabios.c 里, AH=11h 的分派表写着:
;       case 0x04:
;       case 0x14:  biosfn_load_text_8_16_pat(GET_AL(),GET_BL()); break;
;       case 0x22:  biosfn_load_gfx_8_14_chars(GET_BL()); break;
;   而 biosfn_load_gfx_8_14_chars 的函数体只有一句
;       #ifdef DEBUG  unimplemented();  #endif
;   —— 也就是说 AL=22h 在参考实现里是一个空函数。所以
;   `PH: probe fontst=0004` 证明不了"BIOS 也不行", 只能证明"我们问错了人"。
;
;   【本轮的两枪】都走同一把通用铲子 VgaBiosInt10(参数在 [BcAx..]):
;     ① AX=1104h, BL=00h —— "把 ROM 8x16 字形装进 0 号字库"。参考实现的
;        biosfn_load_text_8_16_pat 正是这个 AL: 先 get_font_access()
;        (时序器复位 → SR02=04 → SR04=07 → 复位解除 → GCR04=02/GCR05=00/
;        GCR06=04), 再把 256×16 字节按 32 字节步长拷到 A000:0000,
;        最后 release_font_access()。AL<10h ⇒ 它【不改扫描行数、不设模式】,
;        所以这一枪只写字模, 不可能清屏。
;     ② AX=1114h, BL=00h —— 同一个函数, 但 AL>=10h ⇒ 额外调 set_scan_lines(16)。
;        只有 ① 没把指纹改对时才发这一枪。
;   两枪之后都由调用者(VgaFontProv)读指纹自证。
;
;   【闸门】policyInt10Fix != 0 + 本场最多两次(FontBN)。
;   【落盘】子功能号与 BIOS 回来的 AL 被抄进 Probe 的 AR12/AR13,
;     日志里读 `PH: probe fontb=`(高字节 = 子功能号, 低字节 = 回来的 AL)。
;   【现场】EAX/EBX/ECX/EDX + flags 自己保存恢复(ECX 是档号)。
;-----------------------------------------------------------------------------
; r178-marker: VgaBiosFont -- INT 10h AX=1104h/1114h through VgaBiosInt10.
BeginProc VgaBiosFont
        pushfd
        push    eax
        push    ebx
        push    edx
        mov     byte ptr [BiosFontStat], 0
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      vbf_out
        cmp     byte ptr [FontBN], 2
        jae     vbf_out
        movzx   eax, byte ptr [BiosFontReq]      ; 子功能号(04h/14h)
        or      ax, 01100h                       ; AX = 1100h | 子功能号
        mov     [BcAx], ax
        mov     word ptr [BcBx], 0               ; BL = 0: 0 号字库
        mov     word ptr [BcCx], 0
        mov     word ptr [BcDx], 0
        mov     word ptr [BcEs], 0
        call    VgaBiosInt10
        inc     byte ptr [FontBN]
        cmp     byte ptr [BcOk], 1
        je      short vbf_ran
        mov     byte ptr [BiosFontStat], 2       ; IVT[10h] 为 0, 没敢执行
        jmp     short vbf_out
vbf_ran:
        mov     byte ptr [BiosFontStat], 1       ; 执行了, 成败由指纹判
        ; --- 【r412】删掉这里的两笔 Probe 写 ---------------------------------
        ;   r178 把"子功能号 / BIOS 回来的 AL"抄进了 Probe 的 AR10+2 与
        ;   AR10+3 两格, 而 r191 起这两格是 `PH: probe fontsr03=`(SR03 回读)
        ;   与 `PH: probe fontsr02=`(SR02 回读)的读数格。字模路一旦走到 BIOS
        ;   兜底, 那两格就会被 14h/04h 之类的数覆盖, 读数彻底失去意义。
        ;   子功能号与返回值仍然留在 BiosFontStat / BiosFontReq 里, 并且由
        ;   `PH: probe fontst=` 的 3/5 值表达"哪一枪打中了" —— 不缺可读性。
vbf_out:
        pop     edx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaBiosFont

; r412 D: 冷启动 VGA 复位 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 进文本模式前的预置 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).


; r412 D: 显存寄存器回读 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: AR10 写入 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 C1: 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 removed. It rewrote SR02/SR04/GCR04/05/06 while the box held the
;   screen (r147) and produced the grey screen; the driver runs in the Windows VM context
;   so those writes only touched the VDD shadow. Full-screen repair now lives in the
;   in-box TSR (FIXTSR12/FIXSYS).

;-----------------------------------------------------------------------------
; 【r161】VgaMemCQuick —— 只读"取指/平面"那五个【不经过属性控制器】的字节。
;   出口: MemCNow+0..+4 = SR02 / SR03 / SR04 / GCR05 / GCR06
;         (MemCNow+5 = AR10 【不动】, 保持上一次 显存寄存器回读 的值)。
;   【为什么单独一个过程 —— 这一轮的全部成本都在这里】
;   本轮把补写挂到了 9 号(RESTORE_REGISTERS)上, 而一场全屏会话里它被调
;   300 次以上。完整的六字节读(显存寄存器回读)里有一步是【写 3C0h 的索引
;   字节 30h】(AR10 | PAS)才能读回 AR10 —— 那是属性控制器的内部相位,
;   而全屏 DOS 期间属性控制器属于那个 VM: 我们凭空插一次索引写, 有可能
;   让它紧跟着的那句"写数据"落到别的寄存器上(调色板写坏)。
;   所以在稳态下(五个字节已经全对)这一枪【一个 ATC 周期都不该碰】:
;   只有五字节已经不对(=确实需要判断与动手)时, 调用方才去读 AR10。
;   【现场保护】EAX/EDX + flags 自己保存恢复。
; r161-marker: read the five non-ATC MemC bytes only.
;-----------------------------------------------------------------------------
BeginProc VgaMemCQuick
        pushfd
        push    eax
        push    edx
        mov     dx, 03C4h                       ; --- 时序器 SR02/SR03/SR04 ---
        mov     al, 002h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [MemCNow + 0], al
        dec     dx
        mov     al, 003h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [MemCNow + 1], al
        dec     dx
        mov     al, 004h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [MemCNow + 2], al
        mov     dx, 03CEh                       ; --- 图形控制器 GCR05/GCR06 ---
        mov     al, 005h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [MemCNow + 3], al
        dec     dx
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [MemCNow + 4], al
        pop     edx
        pop     eax
        popfd
        ret
EndProc VgaMemCQuick

;-----------------------------------------------------------------------------
; 【r161】VgaMemCHold9 —— 官方"恢复寄存器状态"(RESTORE_REGISTERS, 9 号)
;   转交【之后】把 MemC 那半边重新落成"文本取指/平面"那一组。
;
;   【为什么是 9 号 —— DDK 原文(逐字)】
;   《Virtual Display Minidriver》的 "CRTC owner and MemC owner" 条:
;     "If you are running a full-screen MS-DOS session, then that MS-DOS
;      session's VM is the CRTC owner.  If you are running a MS-DOS session in
;      a window, then the CRTC owner is Windows but the MemC owner is the
;      MS-DOS VM.  This distinction is necessary when restoring register
;      states. ... Therefore, you can restore those states from the CRTC owner
;      that preserve the appearance of the screen while restoring those states
;      from the MemC owner that control how the CPU sees the video memory."
;   9 号页的 Call With 把这两个所有者写成了寄存器:
;     "ESI: Contains the VM handle for the MemC owner VM.
;      ECX: Contains the VM handle for the CRTC owner VM."
;   ⇒ "CPU 怎么看到显存"归 mini-VDD、并且落在【主 VDD 恢复寄存器状态那一拍】
;     之后, 是文档的原文分工, 不是我们的发明。
;
;   【为什么必须挪到这一拍 —— ./160 的现场读数】
;   同一场全屏会话里:
;     f35(P35FIRST, 本场第一拍) = SR02=03 SR03=00 SR04=02 GCR05=10 GCR06=0E
;                                 AR10=0C CR0C=0 —— 教科书 720x400 文本;
;     l35(P35,      活的那一拍) = SR02=0F SR03=00 SR04=06 GCR05=00 GCR06=04
;                                 AR10=0C CR0C=0 —— 4 平面(离屏虚拟化)那一组;
;   而整场(持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 从 35 号那条路上)只补到 13 次/5 场, 于是画面一直是
;   `1602.png` 那种"字符/属性/字模三种取指塌到一个平面"的花屏 ——
;   那一张 720x400 的图逐像素量出来是每格 9 像素、整格单色(880 格纯绿),
;   只有整组模式设置(mode co80)能救。
;
;   【r166 的闸门 —— 三条, 每条都有读数】
;     ① ESI(=MemC 所有者 VM 句柄) == 4 号交来的那个全屏 DOS VM ⇒
;        S9SAME/S9DIFF 把"是谁"写成两个数;
;     ② TraceI10Ax < 100h(VM 请求的是标准 VGA 模式);
;     ③ VgaGeoTest == 0(硬件此刻【正在显示】标准 mode 3 文本屏)⇒
;        S9GEOF 把"被这一条挡下多少次"写成一个数。
;   【为什么撤掉 r161 的 VgaFixArm 闸门】./165: VgaFixArm 在 9 号那些拍上
;   从来不为 1(s9n 整场恒 0, 而同一场 f9 被调了约 94 次) —— 它是把整条
;   官方路结构性地关死, 不是保险。
;   【判据自己会停】五个字节与文本目标全等 ⇒ S9OK 记一笔, 一个字节都不写;
;   不等 ⇒ 交给 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法(它跳过预算与去抖, 写完立刻读回自证)。

;   9 号的【转交函数返回之后】, 上面还有主 VDD 的分派器; 而 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 内部
;   会用 ESI 当 A0000 的指针(它自己只保存 EAX/EDX/EBX, 因为 35 号那边从来
;   没有人需要 ESI)。DDK 的 9 号页写着 "ESI: Contains the VM handle for the
;   MemC owner VM"、9 号页还写着 "EBP: May or may not be pointing to the VM's
;   client registers" ⇒ 这两个寄存器在这一拍都是有意义的, 不许被我们的补写
;   带走。用七个 push/pop 而不是 pushad/popad: 本文件通篇没有 pushad, 而
;   an158/ax_stubaudit.py 的线性栈深审计不认那一条(它会把它读成"ret 时栈深
;   -28"), 于是"审计干净"这个信号会被一条工具盲区污染 —— 工具的历史版本
;   是证据, 不改工具, 改写法。
; r161-marker: assert the text-mode MemC group right after the official restore.
; r166-marker: the arm gate is gone and the physical test decides -- ./165 had
;              s9n = 0 while f9 ran ~94 times inside the garbled session.
;-----------------------------------------------------------------------------
BeginProc VgaMemCHold9
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        inc     byte ptr [Probe + VBE_TR_PR_S9N]
        ; --- 【r166】闸门 1: 官方所有者 -------------------------------------
        ;   DDK 9 号页: ESI = MemC 所有者 VM 句柄。它必须就是 4 号交来的
        ;   那个全屏 DOS VM(OW_BOXVM)。窗口化会话里主 VDD 摆的 4 平面组
        ;   也会通过字节判据, 而句柄核对是与状态无关的第二道保险。
        ;   【r166 撤掉了 r161 的武装闸门】./165 的读数: VgaFixArm 在 9 号
        ;   那些拍上【从来不为 1】(s9n 整场恒 0, 而同一场 f9 被调了约 94 次,
        ;   其中大部分正落在用户看到花屏的那几分钟里) —— 那道闸门不是"保险",
        ;   而是把整条官方路结构性地关死。现在它换成三条闸门, 每一条挡下时
        ;   都有一个能与不能都读得出来的数(所有者 / VM 请求的模式 / 硬件
        ;   此刻在显示什么)。S9N 仍然在最前面递增: 它回答"9 号到底进没进
        ;   我们这条路", 与闸门是否放行是两个问题。
        cmp     esi, dword ptr [OwnWord + OW_BOXVM]
        je      short vm9_owner_ok
        inc     byte ptr [Probe + VBE_TR_PR_S9DIFF]
        jmp     vm9_out
vm9_owner_ok:
        inc     byte ptr [Probe + VBE_TR_PR_S9SAME]
        ; --- 【r166】闸门 2: VM 请求过的是标准 VGA 模式(48 号记下的 AX) ----
        ;   TraceI10Ax >= 100h ⇒ 这个 VM 要的是 VBE/线性模式, 那套状态归我们
        ;   自己的模式程序管, 绝不在这一拍插文本取指组。
        movzx   eax, word ptr [TraceI10Ax]
        cmp     eax, 100h
        jae     short vm9_n1
        jmp     short vm9_go
vm9_n1:
        jmp     vm9_out
vm9_go:
        call    VgaMemCQuick
        cmp     byte ptr [MemCNow + 0], 003h    ; SR02 写平面屏蔽
        jne     short vm9_need
        cmp     byte ptr [MemCNow + 1], 000h    ; SR03 字符映射
        jne     short vm9_need
        cmp     byte ptr [MemCNow + 2], 002h    ; SR04 奇偶主机寻址开
        jne     short vm9_need
        cmp     byte ptr [MemCNow + 3], 010h    ; GCR05 奇偶模式开
        jne     short vm9_need
        cmp     byte ptr [MemCNow + 4], 00Eh    ; GCR06 映射 B8000 + 文本
        jne     short vm9_need
        inc     byte ptr [Probe + VBE_TR_PR_S9OK]
        jmp     short vm9_out                   ; 主 VDD 自己恢复对了: 不写
vm9_need:

        ; --- 【r166】闸门 3: 硬件此刻在显示标准 mode 3 文本屏 ---------------
        ;   与 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 里那一条是同一个过程、同一张表(定义上不可能不一致)。
        ;   【为什么放在"五字节确实不同"之后】VgaGeoTest 要写 8 次 CRTC 索引
        ;   端口(3D4h), 那是 VGA 上有状态的端口 —— 只在"确实可能开火"的那
        ;   一拍才去动它, 是这一枪的纪律: 状态全对时一个端口都不碰。
        ;   挡住时把【为什么没动手】记成一个数: S9GEOF 增长 = 闸门在真判断;
        ;   S9GEOF 与 S9FIX/S9OK 一起恒 0 = 这条官方路根本没跑到判据上。
        call    VgaGeoTest
        mov     [Probe + VBE_TR_PR_GEOMASK], al
        test    al, al
        jz      short vm9_geo_ok
        inc     byte ptr [Probe + VBE_TR_PR_S9GEOF]
        jmp     vm9_out
vm9_geo_ok:
        mov     byte ptr [MemCPush], 1
        mov     byte ptr [MemCSrc], 1           ; 这一笔记进 S9FIX/S9BAD
        mov     byte ptr [MemCSrc], 0
vm9_out:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaMemCHold9

; r412 D: 映射与起始地址读取 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: B8000 映射判定 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r152】MapToCpu —— 显存字节偏移 -> 该偏移在 CPU 地址空间的映像。
;   入口 EAX = 显存字节偏移, DL = 映射选择(0..3); 出口 ESI = CPU 线性地址。
;   【为什么 VxD 能直接读这些线性地址】VxD 在 System VM 的地址空间里运行,
;   低 1MB(含 A0000h-BFFFFh)是恒等映射的 —— 与 VxD 读 0:400h 的 BIOS
;   数据区是同一件事。而读 A0000h/B8000h 恰好就是"CPU 会看到什么",
;   因为 VGA 的孔径解码发生在内存总线上, 与发起访问的环无关。
; r152-marker: display-memory offset -> CPU linear address (map aware).
;-----------------------------------------------------------------------------
BeginProc MapToCpu
        test    dl, 002h
        jnz     mtc_b                   ; 目标 10 行外: 不写 short
        test    dl, 001h
        jnz     short mtc_a64
        and     eax, 001FFFFh
        add     eax, 000A0000h
        mov     esi, eax
        ret
mtc_a64:
        and     eax, 000FFFFh
        add     eax, 000A0000h
        mov     esi, eax
        ret
mtc_b:
        and     eax, 0007FFFh
        test    dl, 001h
        jnz     short mtc_b8
        add     eax, 000B0000h
        mov     esi, eax
        ret
mtc_b8:
        add     eax, 000B8000h
        mov     esi, eax
        ret
EndProc MapToCpu

; r412 D: 格数统计 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 字节块拷贝 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r153】取指基址探针与修复 —— 一组纯读取证过程 + 一个只写 CR0C/CR0D 的
;   修复过程。
;
;   【要证明的那件事】文本模式能工作的定义只有一条:
;       CRTC 取指读到的显存  ==  CPU 在 B8000 写进去的那块显存
;   这是一个可以逐字节比较的等式。r143..r152 一直在量"寄存器对不对", 而
;   寄存器对不对只是这条等式的【代理】—— ./152 就是代理失效的一次:
;   六字节补写把 GCR06 写成 0Eh(B8000 32K 映射)之后, 主 VDD 又在会话中途
;   把映射改回 04h(A0000 64K), 于是 CPU 的 B8000 与 CRTC 的取指相差
;   8000h 字节 —— 而两者各自的读数都好看(那 32 格都是 20h 07h 的空屏),
;   旧判据判"没事", 一个字节都没修。
;
;   【r153 的判据】不再问"像不像文本", 直接问两个 32 字节块是否相等:
;     PR_AGREE = 1  ⇒ 取指就是 CPU 写的地方, 收工(一个字节都不写);
;     PR_AGREE = 2  ⇒ 取指地址不在当前映射的窗口里, 读不到, 不猜;
;     PR_AGREE = 0  ⇒ 把显示起始地址挪到"CPU 的 B8000 所对应的显存偏移"
;                     上去 —— 这一步是【构造性】的, 不是猜: 无论当前映射
;                     是哪一个, 盒子写文本都要经过 B8000, 所以 CRTC 必须
;                     从 B8000 映射判定(当前映射) 处取指。
;   单位由 CR17 的 bit6(字/字节寻址)决定(FreeVGA CR17: 0 = word mode,
;   地址计数器左移一位 ⇒ CR0C/CR0D 是【字地址】= 字节偏移 / 2), 另两个
;   单位是回退候选; 写完一律重测那个等式, 不成立就还原并锁定本场。
;
;   【为什么这一组是纯读】读显存、读 AR 调色板、读 DAC、扫显存都不改变
;   画面; 只有 VgaTryStart 会写 —— 而且只写 CR0C/CR0D 这一对"硬件滚屏
;   每帧都在改"的地址寄存器, 绝不碰 SR/GCR/AR(那是 ./148 灰屏的来源)。
;
;   【r160 —— 这一组必须 fail-closed, 而且必须能收回自己】
;   ./159 的判决: 桌面回来了(说明 r159 那三行桩的修法对了), 但出现了一条
;   全新的、更坏的故障 —— `mode co80` 再也修不好全屏花屏, 看到的是一种
;   新的花屏(./159/159_1.png), 切回桌面时 winoldap/VGAFULL.3GR 还报了
;   无效页错误。日志把这条链的每一环都写成了数字:
;     ① `PH: probe new=FFFF` + `cand=0100` + `fix=0101`(FIXN=1, FIXOK=1)
;        ⇒ 取指基址修复【写进去了 FF/FF】并自称"自证成立";
;     ② `PH: probe start=FFFF`(硬件 CR0D:CR0C 原文) ⇒ 显示起始地址 =
;        FFFFh 字地址 = 1FFFEh 字节 = 显存 128K 处, 而文本页在偏移 0;
;     ③ 从此每一拍 `agree=0002`(取指不在当前 32K 窗口里) ⇒ 修复自己的
;        第一条判据("AGREE = 2, 不猜")把它永久挡在门外;
;     ④ `mode co80` 那一拍(INT 10h AX=0003)的边沿计数是完整的模式设置
;        (CRTC_OUT 215 次、DAC_OUT 194 次、SEQ_OUT 12 次、GCR_OUT 13 次、
;        MODIFY_REGISTER_STATE 1 次、ACCESS_VGA/LINEAR_MEMORY_MODE 各 1 次),
;        并且 `wcrt` 的 CR0C/CR0D 写计数当场 +1/+2 —— 也就是说 VM 的写
;        【确实路由到了我们这里】, 可是同一刻之后探针读回的 start 仍是 FFFF。
;   三个原因, 全都在这一组过程里, 全部与"闸门/判据"有关:
;     (a) 【第三态被自己踩掉】显存逐格采样 在 COVER=0 时置 AGREE=3, 但流程
;         随后落进第 3 段, 那里有一句无条件的 AGREE=0 ⇒ 3 从来没被任何
;         人或任何读数看见过(r154 的日志里 agree 只有 0/1, r159 只有 1/2),
;         于是 `AGREE == 3 ⇒ 拒绝开火` 这条 r153 写下的规矩【一次都没执行】;
;     (b) 【哨兵被当成偏移】COVER=0 时 PR_OFFB 被写成 FFFFFFFFh(占位符),
;         而 AGREE 被踩成 0 之后走到候选循环: FFFFFFFFh >> 1 = 7FFFFFFFh,
;         取低 16 位正好是 FFFFh —— 这就是 ② 里那个值的出处;
;     (c) 【自证会回卷】MapToCpu 的每一支都以 `and 7FFFh/0FFFFh/1FFFFh`
;         收尾(按窗口大小取模), 于是 1FFFEh 被回卷成 7FFEh -> CPU 地址
;         BFFFEh, 与 B8000h 那 32 字节逐字节相等(文本空屏到处是 20h 07h),
;         "取指处至少一格非空"也成立 ⇒ 它被判成成功, 那个值被【保留】下来。
;   修法(r160, 四条都只动这一组, 一行硬件行为都没有新增):
;     1. 显存逐格采样 出口处把 AGREE=3 重新写回去(第 11 段): 第三态第一次
;        真正可达, r153 那条拒绝开火的规矩第一次真正生效;
;     2. 闸门补两条出口: AGREE=4(同址, 等式什么都没证明)与 PR_OFFB 哨兵
;        —— 都不许进入写路径;
;     3. VgaTryStart 自证前先要求"候选值的字节偏移在【当前映射窗口内】",
;        越界直接判不成立(回卷再相等也不算);
;     4. 安全网: 写完之后如果取指落到窗口之外, 而硬件里还是我们写进去的
;        两个字节, 就把原始值写回去、锁定本场、并把次数记进 NETC。
;   ⇒ 一句话: 【没有真凭实据就不写; 写了之后一旦不成立, 一拍之内收回】。
;      这样主 VDD 的 CRTC 影子不会再被我们偷偷改掉, 用户证明有效的
;      `mode co80`(官方路径)也就不会再被我们挡住。
;   r160-marker: the fetch-base repair is fail-closed and self-reverting.
;-----------------------------------------------------------------------------

;-----------------------------------------------------------------------------
; 【r153】MapWin —— 当前显存映射下, CPU 能看到哪一段显存。
;   入口 DL = 映射选择(0..3); 出口 ESI = CPU 基址, EAX = 可达字节数。
;   【依据(FreeVGA, Graphics Controller Miscellaneous Register, index 06h)】
;     00b = A0000h-BFFFFh 128K; 01b = A0000h-AFFFFh 64K;
;     10b = B0000h-B7FFFh  32K; 11b = B8000h-BFFFFh  32K。
;   窗口一律从"显存字节偏移 0"开始 —— 这正是"CPU 的 B8000 对应哪个显存
;   偏移"这件事的另一半(B8000 映射判定 给偏移, 本过程给基址)。
;   【为什么上限走 EAX 而不是 EDX】本项目的 buildcheck 把
;   `mov edx, NNNNh` 一律当成"给 DX 装端口号"来审(那是 A2070 的纪律),
;   于是在这里用 EDX 装一个 20000h/8000h 的窗口上限会被判成"访问了
;   非标准 VGA 端口"。EAX 没有这个问题。
;   【注意】EAX/ECX/ESI 会被破坏, 其余寄存器不动。
; r153-marker: host window base/limit for the current display-memory map.
;-----------------------------------------------------------------------------
BeginProc MapWin
        mov     cl, dl
        mov     esi, 000A0000h
        mov     eax, 0020000h
        test    cl, 002h
        jz      short mwin_m01
        mov     esi, 000B0000h
        mov     eax, 0008000h
        test    cl, 001h
        jz      short mwin_out
        mov     esi, 000B8000h
        ret
mwin_m01:
        test    cl, 001h
        jz      short mwin_out
        mov     eax, 0010000h
mwin_out:
        ret
EndProc MapWin

; r412 D: B8000 覆盖判定 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 32 位块拷贝 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r153】MemEq32 —— ESI/EDI 两处的 32 字节是否逐字节相等。
;   出口 AL = 1 相等 / 0 不等。这就是"取指 == CPU 写的地方"那条等式。
; r153-marker: byte-for-byte equality of two 32-byte windows.
;-----------------------------------------------------------------------------
BeginProc MemEq32
        push    ecx
        push    esi
        push    edi
        mov     ecx, 32
me32_loop:
        mov     al, [esi]
        cmp     al, [edi]
        jne     short me32_ne
        inc     esi
        inc     edi
        dec     ecx
        jnz     short me32_loop
        mov     al, 1
        jmp     short me32_out
me32_ne:
        xor     al, al
me32_out:
        pop     edi
        pop     esi
        pop     ecx
        ret
EndProc MemEq32

;-----------------------------------------------------------------------------
; 【r153】CellsStats —— 铺开采样 4 行 × 16 格, 数"非空格数"与"字符种类数"。
;   入口 ESI = 文本页起始 CPU 地址; 出口 AL = 非空格数, AH = 种类数。
;   【为什么是 4 行 × 16 格】./152 的教训: 只看开头 32 格等于只看控制台
;   最空的地方(4000 格里最空的 32 格)。行的步长 = 80 列 × 2 字节 = 160,
;   取 8 行一个采样点 == 1280 字节, 覆盖屏幕的上/中/下。
;   【"非空"的定义】字符既不是 00h 也不是 20h。这是"这一格有没有东西"的
;   最小判据; 它不看属性(属性 00h 也不可见, 但那是另一件事, 见 PR_AGREE)。
;   【种类数怎么数】CharSeen = 256 字节位图, 本过程自己清零(那是 256 次
;   写, 与它买到的信息相比不值一提)。
;   【现场保护】EBX/ECX/EDX/ESI/EDI 全部自己保存恢复。
; r153-marker: spread sampling of non-blank cells and distinct chars.
;-----------------------------------------------------------------------------
BeginProc CellsStats
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     edi, OFFSET32 CharSeen
        xor     eax, eax
        mov     ecx, 64
cst_zero:
        mov     [edi], eax
        add     edi, 4
        dec     ecx
        jnz     short cst_zero
        xor     bl, bl                  ; BL = 非空格数
        xor     bh, bh                  ; BH = 种类数
        mov     dl, 4                   ; 4 个采样行
cst_row:
        mov     ecx, 16                 ; 每行 16 格
cst_cell:
        mov     al, [esi]
        cmp     al, 020h
        je      short cst_skip
        test    al, al
        jz      short cst_skip
        inc     bl
        movzx   edi, al
        cmp     byte ptr [CharSeen + edi], 0
        jne     short cst_skip
        mov     byte ptr [CharSeen + edi], 1
        inc     bh
cst_skip:
        add     esi, 2
        dec     ecx
        jnz     short cst_cell
        add     esi, 1248               ; 1280 - 32: 跳到 8 行之后
        dec     dl
        jnz     short cst_row
        mov     al, bl
        mov     ah, bh
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret
EndProc CellsStats

; r412 D: 整屏扫描 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 属性控制器调色板读取 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 单个 DAC 项读取 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 C46: 显存逐格采样 removed together with 取指基址修复.
;   The pair read the fetch state out of the VDD shadow and, when the policy
;   allowed it, wrote GCR06/SR04 back into the same shadow.  Neither can ever
;   reach the box hardware, and the box maintains its own fetch/dot clock.
;   VgaDotClock is NOT removed: L10276 calls it independently.

; r412 D: VgaTryStart removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

; r412 D: 起始地址还原 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r152/r153】VgaFetchFix —— 取指基址探针/修复的闸门(很短, 全部是短跳)。
;   【为何与 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 分成两个过程】那个是 r147 的六字节文本补写,
;   ./148 实测"读回来对了"却把花屏变成灰屏 + co80 失效; 本过程与它
;   【不做同一件事】: 一个模式/寻址寄存器都不写, 只挪显示起始地址,
;   而且写完自证、失败还原。
;   【r153: 8 次上限改成"每场会话 8 次"】r152 把 FIXN 当【跨场累计】用,
;   于是 LATCH 每场都清、预算却全场通用 —— 一场会话可以在"看起来是全新
;   机会"的状态下撞上已经用掉的预算, 然后一声不响地什么都不做。
;   这正是本项目点过名的反模式(用满即沉默: r104 的次数上限、r141 的
;   PITCH_ARM_MAX、r143 的 64 次预算、r145 的节流、r147 的一比特代理),
;   而 r147 自己也写下过"判据会自己停下来 ⇒ 不需要总量预算"。
;   现在的语义:
;     FIXN   = 【本场会话】写 CR0C/CR0D 的次数, 4 号清零(每场一份全新预算);
;     FIXOK  = 【跨场累计】"写后自证成立"的次数 —— 它是成果, 不该被清;
;     FIXBAD = 【本场会话】三个候选全败的次数, 4 号清零;
;     LATCH  = 本场放弃(4 号清零)。
;   预算本身仍然有用: 它的作用不是"省时间", 而是"当有东西每拍都把起点
;   扳回去时, 不许我们跟着无限对打"(那正是 r147 在 ./148 里做的事)。
;   而"自证成立"的那条路不需要预算: 下一拍的 AGREE 会自己变成 1。
; r152-marker: fetch-base probe/repair gate.
; r153-marker: the write budget is per session now (was per boot).
;-----------------------------------------------------------------------------
BeginProc VgaFetchFix
        pushfd
        push    eax
        cmp     byte ptr [VgaFixArm], 0
        je      short vff_out
        cmp     byte ptr [Probe + VBE_TR_PR_LATCH], 0
        jne     short vff_out
        cmp     byte ptr [Probe + VBE_TR_PR_FIXN], 8
        jae     short vff_out
vff_out:
        pop     eax
        popfd
        ret
EndProc VgaFetchFix

; r412 C46: 取指基址修复 removed together with 取指基址修复.
;   The pair read the fetch state out of the VDD shadow and, when the policy
;   allowed it, wrote GCR06/SR04 back into the same shadow.  Neither can ever
;   reach the box hardware, and the box maintains its own fetch/dot clock.
;   VgaDotClock is NOT removed: L10276 calls it independently.

; r412 D: 盒内窗口映射 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).
; r412 D: 持屏期修正 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; TraceStub_0 —— REGISTER_DISPLAY_DRIVER(0)
;   主 VDD 在 display.drv 调 VDD_DRIVER_REGISTER 之后回调我们。一次
;   (重新)注册 = Windows 又拿回了显示硬件且处于我们的高分辨率模式 ⇒
;   把状态机复位(VGA 位、陷阱位、停用位全清)。
;   【为何必须有这一步】26 号会把 VGA 位置 1, 而它之后并不总有 7 号来清;
;   下一次注册(如用户切换分辨率后 GDI 重新 Enable)是最干净的复位点。
;-----------------------------------------------------------------------------
TraceStubState 0, -1, 0, 0F8h


;-----------------------------------------------------------------------------
; TraceStub_1 —— GET_VDD_BANK(1), 带策略
;   【DDK 原文(S2829)】主 VDD 在 VDD_DRIVER_REGISTER 里回调问我们"32K/64K
;   的 VGA 4 平面虚拟化区放哪": ECX/Client_ECX = 该区起始字节偏移;
;   我们要回 AH(相对 bank 的页偏移)与 EDX(分配给该区的显存大小)。
;   原文关键句:
;     "If not allowing VGA 4 plane virtualization, set EDX = ECX or
;      Client_ECX" —— 即【把入口给的偏移原样当大小答回去】= 拒绝。
;     "It is not called if the display driver passed Client_EDX == -1."
;
;   【r101 为什么默认拒绝】./100 的 x230 与 vm 日志里这条服务每次注册都被
;   调用一次, 记录的 ecx=00300000(正好是可见屏字节数)、edx=00000000, 而
;   注册返回值(Total bytes = 可见屏 + VDD 自留)算出来 VDD 自留 = 0 字节
;   —— 也就是说: 我们声明了 EDX=0("支持窗口化 4 平面虚拟化"), 却让主 VDD
;   带着 0 字节的虚拟化区去做 4 平面虚拟化, 于是 DOS 会话的 VGA 访问
;   (文本单元 0x0720)落进可见屏 —— 这正是实拍的"整屏绿色(0x0720 按
;   RGB565 就是纯绿)+ 2KB 宽的陈旧条 + Alt+Enter 后near-black"。
;   DDK 明说拒绝后"仍允许【文本】与非平面图形(如 13h)在窗口里跑",
;   所以窗口 DOS 文本盒不受影响, 受影响的只有"窗口化 VGA 图形虚拟化"。
;
;   策略字节 policyBank: 0 = 不挂钩(r98/r99 行为, 留给 A/B);
;                        1 = 处于 VBE 高分辨率且尚未切到 VGA 时拒绝。
;-----------------------------------------------------------------------------
BeginProc TraceStub_1
        pushfd
        push    eax
        mov     eax, 1
        call    MiniVDD_TraceRecord
        pop     eax
        popfd
        cmp     byte ptr [VbeState.VBS_policyBank], 0
        je      short ts1_pass                 ; 策略关: 与未挂钩一致
        cmp     [StateValid], 0
        je      short ts1_pass                 ; 状态未知: 保守放行
        movzx   eax, word ptr [VbeState.VBS_currentMode]
        cmp     eax, 100h
        jb      short ts1_pass                 ; 标准 VGA 模式: 允许虚拟化
        mov     al, [TraceState]
        test    al, VBE_TR_ST_VGA
        jnz     short ts1_pass                 ; 已切到 VGA: 硬件归 DOS VM
        mov     eax, [ebp].Client_ECX          ; DDK: 拒绝时 EDX = ECX
        mov     [ebp].Client_EDX, eax
        VDD_CY
        ret
ts1_pass:
        cmp     dword ptr [OldDispatch + 1*4], 0
        je      short ts1_nc
        jmp     dword ptr [OldDispatch + 1*4]
ts1_nc:
        VDD_NC
        ret
EndProc TraceStub_1

;-----------------------------------------------------------------------------
; MiniVDD_ReadBdaMode35 —— 【r175】R0: 在 35 号里【只读地】取一次
;   BIOS 数据区的当前模式字节, 并记下"这一拍我们在谁的 VM 里"。
;   入口: 无。出口: 不改任何调用者寄存器, 不写任何硬件寄存器。
;   写两处(都在我们自己的数据区):
;     ① OwnWord+OW_BDA449 = 线性地址 0449h 的一个字节;
;     ② OwnWord+OW_P35MASK 上的 VBE_TR_OWN_AL_BOXVM / AL_WINVM 位。
;   【为什么一场只做一次】./142 实测一个状态窗口内 35 号被叫
;   0x1F18E 次, 而 0:449 在会话中途不会变; 把这段放进主 VDD 最热
;   的轮询里不值得。「一场」的起点是 4 号(见那里的哨兵清零)。
;   【寄存器纪律】Get_Cur_VM_Handle 会改 EBX(返回当前 VM 句柄),
;   而 35 号入口的 EBX 是 DDK 说的"the handle of the VM"——
;   后面的策略分支还要用它 ⇒ 全部保存/恢复,
;   与 MiniVDD_DispiPhysRead 同一套纪律。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_ReadBdaMode35
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     byte ptr [OwBdaDone], 1         ; 先立哨兵: 下面即使出差也只做一次

        ; --- ② 这一拍我们在哪个 VM 里 -------------------------------
        VMMCall Get_Cur_VM_Handle               ; EBX = 当前 VM 句柄
        cmp     dword ptr [OwnWord + OW_BOXVM], 0
        je      short rb35_win
        cmp     ebx, dword ptr [OwnWord + OW_BOXVM]
        jne     short rb35_win
        or      byte ptr [OwnWord + OW_P35MASK], VBE_TR_OWN_AL_BOXVM
rb35_win:
        cmp     dword ptr [WinVMHandle], 0
        je      short rb35_bda
        cmp     ebx, dword ptr [WinVMHandle]
        jne     short rb35_bda
        or      byte ptr [OwnWord + OW_P35MASK], VBE_TR_OWN_AL_WINVM
rb35_bda:
        ; --- ① 0:449 那一个字节(只读) ---------------------------------
        ;   VxD 的 DS 是扁平选择子 ⇒ [ESI+0449h](ESI=0) 就是物理 0:449。
        ;   【为什么不写 `[0449h]`】gp_opscan164 的 R2: 纯常数位移的
        ;   内存操作数要靠 `byte ptr` 单独把尺寸补上, ML 在这种写法上
        ;   有过 A2070 的先例(r171)。改成基址+位移, 尺寸与寻址都不含糊。
        xor     esi, esi
        mov     al, byte ptr [esi + 0449h]
        mov     byte ptr [OwnWord + OW_BDA449], al

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret
EndProc MiniVDD_ReadBdaMode35
;-----------------------------------------------------------------------------
; TraceStub_35 —— CHECK_HIRES_MODE(35), 带策略
;   【DDK 语义】CY = 该 VM 处于高分辨率模式; NC = "the VM is in a standard
;   VGA mode"。且原文说明主 VDD 的判断顺序是"先看 VESA 模式, 再看
;   INT 10h BIOS 模式是否 > 13h, 最后才问 mini-VDD" —— 也就是说:
;   当 BIOS 数据区(0:449)写着"模式 3(80x25 文本)"而硬件其实在
;   1024x768 图形模式时, 我们这条回答是主 VDD 唯一的真相来源。
;
;   【r99 错在哪】未挂钩 ⇒ 主 VDD 默认 NC ⇒ 它一直以为硬件是"标准 VGA",
;   于是用 banked/4 平面的假设去保存与恢复我们的线性打包像素屏。
;   【r100 错在哪】改成了"只对 Windows VM 答 CY" —— 但实测那条 1729 次
;   (vm 会话 7869 次)的窗口化 DOS 轮询里, 传进来的 EBX 并不是 PM API 入口
;   记下的那个句柄(两种上下文取到的 ebx 值不同), 于是全部走了 pass ⇒ 仍然
;   NC ⇒ 主 VDD 继续在错路上轮询(1729 次就是它在反复问"到底是不是 hires")。
;
;   【r101: 按状态答, 与调用者无关】硬件此刻是不是在 hires 通路, 只有主
;   VDD 自己的切屏时序知道 —— 我们用 0/4/5/7/11/12/26 号服务维护的
;   TraceState 位来回答, 不再猜调用者身份。
;   [r132] policyHires = 4 = answer per the DDK: the question is about the VM
;     handle in EBX, so a VM that is not the Windows VM is never "ours" and
;     always gets NC.  [r133] it is NO LONGER the default: ./132 showed the
;     f23 poll storm did not shrink with it ("TRC + f=23 CHECK_HIRES_MODE
;     n=0001297D", the same order as ./130 without it), so the default went
;     back to 1 (answer from our own state, the r131 behaviour) and 0..4 stay
;     reachable from system.ini [omninine] HiresAns for a one-line A/B.
;   policyHires: 0 = 不挂钩(r98/r99);
;                [r133] 1 = 【默认】按状态答(回到 r131 的行为);
;                2 = 只答 Windows VM(r100 行为, 留作 A/B);
;                3 = 只答非 Windows VM(反向对照, 用来证明 EBX 的身份);
;                4 = 按 DDK 答(见 ts35_ddk)。r132 把它设成默认, 而
;                    ./132 的 f23 轮询风暴并没有变小 ⇒ 默认拨回 1。
;-----------------------------------------------------------------------------
BeginProc TraceStub_35
        pushfd
        ; 【r151】入口 AL = 被询问的那个 VM 的 INT 10h 模式号(DDK 原文:
        ;   "AL: Contains the Interrupt 10h video mode.")。必须在这条
        ;   `mov eax,35` 之前读 —— 之后就没了。
        ;   DDK《CHECK_HIRES_MODE》还写明: 主 VDD 判断一个 VM 是不是高
        ;   分辨率时"checks first for a VESA mode, then for an Interrupt 10h
        ;   BIOS mode above 13h, and then queries the mini-VDD"; 而且
        ;   "Far East Windows ... passes these modes as modes 02H and 03H
        ;   instead of 72H and 73H"。⇒ **AL=72h/73h 就意味着折算没有发生**,
        ;   而 72h > 13h, 于是一个跑 80x25 文本的盒子会被主 VDD 的
        ;   "BIOS 模式 > 13h"那一关判成高分辨率 VM。这两行就是物证。
        ; 【r175】R0: 本场第一次跑到 35 号时, 只读地记一次 BDA
        ;   的当前模式字节 + "我们在谁的 VM 里"(见 MiniVDD_ReadBdaMode35
        ;   与 OW_BDA449 的头注)。每拍代价 = 一条 cmp。
        cmp     byte ptr [OwBdaDone], 0
        jne     short ts35_bda
        call    MiniVDD_ReadBdaMode35
ts35_bda:
        mov     byte ptr [OwnWord + OW_P35AL], al
        cmp     al, 072h
        je      short ts35_al72
        cmp     al, 073h
        je      short ts35_al72
        cmp     al, 002h
        je      short ts35_al23
        cmp     al, 003h
        je      short ts35_al23
        cmp     al, 013h
        jbe     short ts35_allo
        or      byte ptr [OwnWord + OW_P35MASK], VBE_TR_OWN_AL_HI
        jmp     short ts35_aldone
ts35_al72:
        or      byte ptr [OwnWord + OW_P35MASK], VBE_TR_OWN_AL_72
        jmp     short ts35_aldone
ts35_al23:
        or      byte ptr [OwnWord + OW_P35MASK], VBE_TR_OWN_AL_23
        jmp     short ts35_aldone
ts35_allo:
        or      byte ptr [OwnWord + OW_P35MASK], VBE_TR_OWN_AL_LO
ts35_aldone:
        push    eax
        mov     eax, 35
        call    MiniVDD_TraceRecord
        pop     eax
        popfd
        ; 【r143】盒子持屏期间的补写 + S2 取证。
        ;   为什么挂在 35 号上: 主 VDD 在 DOS 会话里唯一高频调用的钩子就是
        ;   它(./142 实测一个状态窗口内 0x1F18E 次), 所以"盒子拿着屏而我们
        ;   还被叫到"的唯一位置就是这里。文本态修补 原本只在 5 号里跑, 而
        ;   5 号之后紧接着就是链下去的【主 VDD 原处理函数】—— r114 的头注
        ;   写着"写在 POST 里 = 我们的写是最后一手", 但代码是先写后 jmp,
        ;   那句话从来没有被验证过。这一拍把它补在链之后。
        ;   本过程自己保存恢复 EAX/EDX/flags, 不改动分派约定。
        ; 【r176】孔径形态修正(两道只读闸门 + 写完回读,
        ;   见 盒内窗口映射 的头注)。没证据就一个字节都不写。
        ; 【r177】孔径修正已停用: ./176 里它动手的那一次证明"修好孔径也没用",
        ;   见 盒内窗口映射 头注里的【r177 已停用】段。
        ; 【r173】盒子持屏期间【一个寄存器都不写】。
        ;   【r176 修正了这条"一律不写"】上面那一枪(盒内窗口映射)确实会在
        ;   盒子持屏期写四个字节, 但它有【两道只读闸门】, 而且正是 r173 那条
        ;   规矩的例外条件: MS DDK 要求 mini-VDD 负责"CPU 怎么看显存"这一半
        ;   (CRTC/MemC owner 分工), 而这几个字节在切屏期【不虚拟化】, 主 VDD
        ;   不会替我们写。r173 当年一刀切掉, 代价是 ./173..175 三个版本连
        ;   "孔径到底对不对"都无法回答。现在的写法是: 只有【读出来证明它错了】
        ;   才写, 写完立刻回读并把回读值打进日志。
        ;   MS DDK《DISPLAY_DRIVER_DISABLING (Function 26)》原文:
        ;   "The mini-VDD should free all of its trapping and set flags so
        ;    that RESTORE_REGISTERS and SAVE_REGISTERS do not do anything so
        ;    that the application has free and clear access to the hardware."
        ;   ./172 的现场证据: fontfp 在两个会话里读回 1818(不是 ROM 的
        ;   C610), 而画面在 cls 之后反而从"绿条"变成"字符破碎" ——
        ;   这两条都指向"盒子自己正在用硬件的时候我们还在写它"。
        ;   所以这里把"盒子持屏期间修寄存器"那一手【物理删掉】,
        ;   只保留一次计数, 让日志能证明它确实被调用过。
        ;   (不写任何字节 —— 连计数都不写, 免得污染 prem3 那一格)
        cmp     byte ptr [VbeState.VBS_policyHires], 4
        je      ts35_ddk                        ; [r132] DDK answer (near jmp)
        cmp     byte ptr [VbeState.VBS_policyHires], 0
        je      ts35_pass
        cmp     byte ptr [VbeState.VBS_policyHires], 2
        je      short ts35_winonly
        cmp     byte ptr [VbeState.VBS_policyHires], 3
        je      short ts35_othervm
        jmp     short ts35_state               ; 1 = 按状态答
ts35_winonly:
        cmp     dword ptr [WinVMHandle], 0
        je      short ts35_pass
        cmp     ebx, [WinVMHandle]
        jne     short ts35_pass
        jmp     short ts35_state
ts35_othervm:
        cmp     dword ptr [WinVMHandle], 0
        je      short ts35_state
        cmp     ebx, [WinVMHandle]
        je      short ts35_pass
ts35_state:
        cmp     [StateValid], 0
        je      short ts35_pass                ; 状态未推送: 不撒谎
        movzx   eax, word ptr [VbeState.VBS_currentMode]
        cmp     eax, 100h
        jb      short ts35_pass                ; 真是标准 VGA 模式
        mov     al, [TraceState]
        test    al, VBE_TR_ST_VGA
        jnz     short ts35_pass                ; 已被切到 VGA 通路
        VDD_CY                                 ; 如实回答: 这是高分辨率
        ret
ts35_pass:
        cmp     dword ptr [OldDispatch + 35*4], 0
        je      short ts35_nc
        jmp     dword ptr [OldDispatch + 35*4]
ts35_nc:
        VDD_NC
        ret

;-----------------------------------------------------------------------------
; [r132] ts35_ddk -- CHECK_HIRES_MODE(35) answered the way the DDK says.
;   DDK, "CHECK_HIRES_MODE (Function 35)":
;     "AL: Contains the Interrupt 10h video mode.
;      EBX: Contains the VM handle to inquire about.
;      EBP: Points to the Windows VM's Client Registers.
;      CY returned means that the VM is in a hi-res mode.  NC returned means
;      that the VM is in a standard VGA mode."
;   So the answer describes the VM in EBX.  Until r131 this stub answered by
;   OUR OWN Windows mode for every caller, i.e. a windowed/full-screen DOS VM
;   polling with AL=03h/06h was told "this VM is in hi-res mode".  The Main VDD
;   then treated a text-mode DOS VM as a hi-res VM and never settled: the
;   r129/r130/r131 logs show poll bursts of 1.1e5 .. 3.0e5 calls, each burst
;   interleaved with a hires->VGA->hires focus change (hooks 4/5/6/7), and that
;   mode-3 excursion is what leaves CR11h = 8Eh (Protect) behind.
;   No register may be modified on the NC path, so the state test below uses
;   only flags and a byte compare -- the old code did `movzx eax` here, which
;   destroyed the caller's EAX before chaining on to the VDD default handler.
;-----------------------------------------------------------------------------
ts35_ddk:
        cmp     dword ptr [WinVMHandle], 0
        je      ts35_ddk_state                  ; unknown: fall back to state
        cmp     ebx, [WinVMHandle]
        jne     ts35_nc                         ; a DOS VM: standard VGA mode
ts35_ddk_state:
        cmp     [StateValid], 0
        je      ts35_nc
        cmp     word ptr [VbeState.VBS_currentMode], 100h
        jb      ts35_nc
        test    byte ptr [TraceState], VBE_TR_ST_VGA
        jnz     ts35_nc
        VDD_CY
        ret
EndProc TraceStub_35

; r412 D: 48 号钩子的实现体 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; MiniVDD_Control — 系统控制消息分发
; 入口: EAX = 控制消息号；其余寄存器视消息而定
; 出口: CF=0 成功/不反对；CF=1 失败(仅初始化类消息会真正失败)
;-----------------------------------------------------------------------------
BeginProc MiniVDD_Control

        Control_Dispatch SYS_CRITICAL_INIT,       MiniVDD_SysCriticalInit
        Control_Dispatch DEVICE_INIT,             MiniVDD_DeviceInit
        Control_Dispatch SYS_DYNAMIC_DEVICE_INIT, MiniVDD_DeviceInit
        Control_Dispatch SYS_DYNAMIC_DEVICE_EXIT, MiniVDD_DynamicExit
        Control_Dispatch SYSTEM_EXIT,             MiniVDD_SystemExit
        Control_Dispatch INIT_COMPLETE,           MiniVDD_InitComplete
        Control_Dispatch W32_DEVICEIOCONTROL,     MiniVDD_W32IOC
        ; 未处理的消息一律 CF=0 —— 告诉 VMM"我不关心，继续"，
        ; 决不能随意 stc，否则某些广播消息会因一个设备报错而中断
        clc
        ret

EndProc MiniVDD_Control

;=============================================================================
; mini-VDD 分派表处理函数(仅 4 项，其余保持主 VDD 默认)
;
; 统一调用约定(主 VDD 通过分派表调用时)：
;   EBX = 当前 VM 句柄(必须保持)
;   EBP → 调用方 VM 的 Client_Reg_Struc(必须保持)
;   EAX/ECX/EDX = 各功能特定的参数
; 返回：
;   结果写入 [ebp].Client_EAX/ECX/EDX…；
;   "是否已处理"用 VDD_CY/VDD_NC 宏写客户端 EFlags 的进位位。
;   用到的寄存器都要保存(MSDN: "Save everything that you use")；
;   下面每个函数都只使用参数/结果寄存器本身，故无需额外保存。
;=============================================================================

;-----------------------------------------------------------------------------
; GET_TOTAL_VRAM_SIZE (36)
; 主 VDD 保存高分辨率屏幕时会把显存写入交换文件，需要真实总显存大小。
; 数值来自 display.drv 推送的 VBE 4F00h 结果——纯 VBE、与卡无关。
; 【何时拒绝回答】状态未推送或 vramSize=0 时返回 NC，主 VDD 会自己
; 调 VESA BIOS 查询(慢但正确)；谎报一个错值比不回答危险得多。
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r412】ACCESS_VGA_MEMORY_MODE (11) —— 真正实现(以前是 TraceStub_11 空桩)
;
;   依据(Microsoft Win95/98 DDK《ACCESS_VGA_MEMORY_MODE (Function 11)》原文):
;     "Certain pieces of display hardware are 'modal' and must have some of their
;      extended registers set to certain values depending on whether the
;      application is accessing memory at one of the VGA apertures (such as
;      A000h:xxxx) or if it is accessing memory using a flat linear frame buffer.
;      This routine is called by the Main VDD to explicitly tell the mini-VDD to
;      setup for accessing at the VGA 64K aperture."
;   ⇒ 主 VDD 叫我们把卡切成【CPU 走 VGA 孔径】的那一态。我们从 r99 到 r211 一直是空桩。
;   【现场证据】./150 的记录: 第一次全屏会话里钩子 4/5 一次都没响,
;   "把 VGA 位置起来的是 ACCESS_VGA_MEMORY_MODE(11)"。
;
;   【做什么】按【盒子当前的 BIOS 模式】选一组, 写完自证:
;     文本模式 00/01/02/03/07 ⇒ 调 VgaM3Apply(已有代码, r167 写的标准
;       mode 3 六字节组, 它自带读回自证, AL=0/1);
;     4 平面图形 0Ch..12h / 其它 ⇒ 【不动硬件】, 只记录
;       (T11=2)。理由: 用户现场是文本; 4 平面那一组写错会直接
;       把屏弄坏, 在没有自证手段之前不猜。
;   【闸门】policyInt10Fix(默认 1)——与全屏开场自救同一档。
;   【为何还要链到旧处理函数】与 TraceStub 同形: 有就交给它, 没有就回 NC。
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r412】VbeStartZero —— 用 VBE 4F07h 把显示起始扫描行置 0
;
;   【为什么是它】./213 的画面变成"一块 GUI 的截片 + 伪冻屏",
;   而用户手打 `mode co80` 能修好 —— 说明【几何生效了、显示起点
;   没生效】(屏幕仍在扫描旧的线性帧缓存, 所以是"冻住的
;   GUI 截片")。而 r212 的 t11=0 已经证明: 我们写 CR0C/CR0D=0 读回对,
;   画面却不动 ⇒ **真正生效的显示起点不是 CR0C/CR0D**,
;   它在 VBE/linear 那一层。VBE 3.0 的 4F07h 就是"设显示起点"的
;   标准函数(BH=0 设, CX/DX = 第一扫描行)—— 不猜寄存器,
;   也不依赖卡型。
;   【位置】5 号最后一笔(在 BIOS 状态回放 之后) —— 模式设完之后
;   才能把起点定下来。好在 VBE 这一枪不会清屏(它不是模式设置)。
;   【闸门】policyInt10Fix(与全屏自救同一档)。
;-----------------------------------------------------------------------------
BeginProc VbeStartZero
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      short vsz_out
        VMMCall Get_Cur_VM_Handle               ; EBX = 当前 VM
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        mov     word ptr [ebp].Client_ax, 04F07h
        mov     word ptr [ebp].Client_bx, 00000h        ; BH=0 设置
        mov     word ptr [ebp].Client_cx, 0             ; 第一扫描行像素
        mov     word ptr [ebp].Client_dx, 0             ; 第一扫描行
        mov     eax, 10h
        VMMCall Exec_Int
        mov     ax, [ebp].Client_ax
        ; 【r412】原来这里存 4F07h 的返回 AH(VSD0, 字节 71)。
        ;   71 已改归 PAL_ST(文本调色板状态), 这一格不再有读者。
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State
vsz_out:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VbeStartZero

; r412 D: 起始地址校验 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r412】VbeScanProbe —— 盒子持屏时读一次 VBE 4F03h(当前模式)
;
;   【为什么只读】r212(t11=0:17 字节 VGA 组读回全对、画面不动)
;   与 r224(无条件填 1..7 段也无变化) 合起来说明:
;   VGA 寄存器与平面 2 的内容都不决定屏幕上那张图。
;   于是要回答的是:"卡到底在用什么模式扫描屏幕"。
;   以前我们只在会话结束时读过(0072), 从来没在盒子持屏那一刻读。
;   判据: 4F03 读出 0072 ⇒ 卡确实在 VGA 6a21式 72h(那 VGA 寄存器就是对的,
;   矛盾在别处);读出 0117 ⇒ 卡根本没离开桌面的 VBE 模式,那就是根因。
;   【不清显存】本过程只发 4F03(取当前模式),发完立即退出,不调 4F02。
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r412】VgaDotClock —— 按盒子自己的模式定字符时钟(SR01 bit0: 1 = 8 点 / 0 = 9 点)
;
;   依据: ./225 的 vsm=0072 —— 盒子持屏时卡确实在 VGA 模式 72h。
;   而 72h 是 80x30 @640x480(每字符 8 点), 我们的文本组却恒写 SR01=00(9 点,
;   720x400 的值) ⇒ 字形被拉宽/发虚 + 约 9 像素周期竖纹,
;   正是 ./190b/190b2.png 那幅"绿底 + 竖条纹、ASCII 可读但字形不对"的签名。
;   定义(FreeVGA SR01 Clocking Mode, bit0): 0 = 9 dots/char, 1 = 8 dots/char。
;   做法: 72h ⇒ bit0=1; 73h 与 00..07 ⇒ bit0=0;写完读回(不另存探针)。
;   只动 SR01 一位, 不动任何其它寄存器。
;-----------------------------------------------------------------------------
BeginProc VgaDotClock
        pushfd
        push    eax
        push    edx
        push    edi
        ; 【r412】模式取自探针 T12——它是【12 号回调】里读到的盒子模式
        ;   (r212/r225 两次都是 t12=0072);而 5 号里读 BDA 只会得到
        ;   Windows VM 的 0003 —— r226 就是因此走了"9 点"那一支,一模一样。
        movzx   eax, byte ptr [Probe + VBE_TR_PR_T12]
        mov     dx, 03C4h
        mov     al, 001h                        ; 选中 SR01
        out     dx, al
        inc     dx
        in      al, dx                          ; AL = 当前 SR01
        mov     ah, al                          ; 先存一份
        movzx   eax, byte ptr [Probe + VBE_TR_PR_T12]
        cmp     al, 072h
        je      short vdc_8
        ; ---- 73h / 00..07: 9 点 ----
        mov     al, ah
        and     al, 0FEh
        jmp     short vdc_w
vdc_8:
        mov     al, ah
        or      al, 001h
vdc_w:
        mov     dx, 03C5h
        out     dx, al
        ; ---- 读回(不另存探针) ----
        mov     dx, 03C4h
        mov     al, 001h
        out     dx, al
        inc     dx
        in      al, dx
        pop     edi
        pop     edx
        pop     eax
        popfd
        ret
EndProc VgaDotClock

BeginProc VbeScanProbe
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    ebp
        VMMCall Get_Cur_VM_Handle
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        mov     word ptr [ebp].Client_ax, 04F03h
        mov     word ptr [ebp].Client_bx, 0
        mov     eax, 10h
        VMMCall Exec_Int
        mov     ax, [ebp].Client_ax
        mov     bx, [ebp].Client_bx
        mov     [Probe + VBE_TR_PR_VSMH], ah            ; 4Fh = VBE 认了
        mov     [Probe + VBE_TR_PR_VSML], bl            ; 当前模式低字节
        mov     [Probe + VBE_TR_PR_VSMX], bh            ; 当前模式高字节
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State
        pop     ebp
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VbeScanProbe

; r412 D: 模拟 INT 10h removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).


;-----------------------------------------------------------------------------
; 【r412】VgaPalRestore —— 把标准 VGA mode 3 文本调色板整套写回, 并读回自证
;
;   【为什么要写这一笔】./190b/190b1.png 与 ./190b/190b2.png 是同一次缺陷的
;   两拍, 看完这两张图, 问题就不再是"整屏乱码"一件事:
;     · 190b1(第一次进全屏): 页里还是上一态的像素数据 —— 内容问题;
;     · 190b2(第二次及以后): 内容【完全正确】。`dir` 的每一行都认得出,
;       顶部的标题条还是彩色的(红/青/蓝), 说明字符发生器、字模、属性图都在
;       正常工作。唯一错的是【颜色】: 所有属性为 0(黑)的单元被画成了绿色。
;   ⇒ 颜色由两层决定, 而这两层本项目部【从未读过、也从未写过】:
;       ① 属性控制器的 AR00..AR0F(属性字节的高低四位各自映射到哪个 DAC 索引);
;       ② DAC 的 16 个条目(那个索引到底是多少 R/G/B)。
;     真实 VGA 的 mode 2/3 设模式会把这两组装成固定表; 用户手打 `mode co80`
;     之所以能"完全修好"(r198), 正是因为它又装了一遍。
;   ⇒ 本过程把那一遍搬到 DDK 的 POST_HIRES_TO_VGA(第 7 步)上, 也就是
;     "主 VDD 已经按盒子的模式号设完模式、盒子还没开始画"的那一刻。
;
;   【闸门】硬件此刻必须真的是标准文本态: SR04 = 02h(奇偶 + 64K) 且
;     GCR06 & 0Fh = 0Eh(B8000 / 奇偶 / 文本)。不满足就一个端口都不写 ——
;     图形 DOS 程序(13h / VESA)的 256 色 DAC 绝不能被我们碰。
;   【预算】每场一次(PalDone, 4 号清 0)。5 号与 11 号都调, 谁先到谁做。
;   【自证】PAL_ST: b0 = 闸门通过, b1 = 已写, b2 = AR00/07/0F 读回全对,
;     b3 = DAC[0] 读回全 0, b7 = 闸门拒绝。PAL_AR0 = 写之前的 AR00。
;     ⇒ 若 PAL_ST 的 b1/b2/b3 都置起来而画面仍是绿底, 那就等于证明
;       "颜色不来自这两层", 下一轮不必再猜。
;   【只读口纪律】3C0h 只写不读(部分芯片读回 0, 会把 PAS 写成 0 直接关视频);
;     读 AR 数据一律走 3C1h, 且每次先读 3DAh 把 3C0h 的触发器复位到索引相;
;     DAC 的【读】索引用 3C7h(3C8h 是写索引), 写完 48 字节后 3C9h 自增回 0。
;-----------------------------------------------------------------------------
BeginProc VgaPalRestore
        pushfd
        ; 【r412】摘掉 r237 挂在这里的 Int10Fix 闸门。
        ;   ./237 的两次测试证明: Int10Fix 不是"本功能开关", 它同时关掉 VgaFontBoot /
        ;   VgaBiosFont / VgaFontProv / fn11 的 VgaM3Apply / 48 号等既有路径 ——
        ;   置 0 会直接把盒子打到 0E 蓝屏(0028:c159f217)与"非法页面错误"。
        ;   本过程是【只读 + 写调色板】的独立动作, 不该受那条遗留总闸管辖;
        ;   它是否生效由 pal_st 的 b1/b2/b3 自证。
        push    eax
        push    ebx
        push    edx
        push    esi
        push    edi
        ; ---- ① 每场一次 ----
        cmp     byte ptr [PalDone], 1
        jae     vpr_out                     ; 远端: 必须让 MASM 自己选 near(>127 字节)
        mov     byte ptr [PalDone], 1
        ; ---- ② 闸门: SR04 = 02 且 GCR06 & 0Fh = 0E ----
        mov     dx, 03C4h
        mov     al, 004h
        out     dx, al
        inc     dx
        in      al, dx
        mov     bl, al                          ; BL = SR04
        mov     dx, 03CEh
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        and     al, 0Fh
        cmp     al, 00Eh
        jne     short vpr_nogate
        cmp     bl, 002h
        jne     short vpr_nogate
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 001h
        jmp     short vpr_run
vpr_nogate:
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 080h
        jmp     vpr_out                     ; 远端: near
vpr_run:
        ; ---- ③ 写之前的读数: AR00(背景) ----
        xor     al, al
        call    VgaPalReadAr
        ; ---- ④ AR00..AR0F ----
        mov     dx, 03DAh
        in      al, dx                          ; 复位 3C0h 触发器到索引相
        mov     edi, OFFSET32 PalArStd
        xor     ebx, ebx
vpr_ar:
        mov     dx, 03C0h
        mov     al, bl
        or      al, 020h                        ; PAS=1: 视频输出保持打开
        out     dx, al                          ; -> 索引相
        mov     al, byte ptr [edi + ebx]
        out     dx, al                          ; -> 数据相(随后自动回到索引相)
        inc     ebx
        cmp     ebx, 16
        jb      short vpr_ar
        ; ---- ⑤ AR10..AR13 ----
        ;   AR10 = 0Ch(blink 关 + 行图形开, 与标准 mode 3 相同)
        ;   AR11 = 00h(overscan 黑边框)
        ;   AR12 = 0Fh(颜色平面全开)  AR13 = 00h(水平平移 0)
        mov     dx, 03C0h
        mov     al, 030h
        out     dx, al
        mov     al, 00Ch
        out     dx, al
        mov     al, 031h
        out     dx, al
        xor     al, al
        out     dx, al
        mov     al, 032h
        out     dx, al
        mov     al, 00Fh
        out     dx, al
        mov     al, 033h
        out     dx, al
        xor     al, al
        out     dx, al
        ; ---- ⑥ PEL mask = FFh(6 位 DAC) + DAC 条目 0..15 ----
        mov     dx, 03C6h
        mov     al, 0FFh
        out     dx, al
        mov     dx, 03C8h
        xor     al, al
        out     dx, al                          ; 写索引 = 条目 0
        mov     dx, 03C9h
        mov     edi, OFFSET32 PalDacStd
        xor     ebx, ebx
vpr_dac:
        mov     al, byte ptr [edi + ebx]
        out     dx, al
        inc     ebx
        cmp     ebx, 48
        jb      short vpr_dac
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 002h
        ; ---- ⑦ 读回自证: AR00 / AR07 / AR0F ----
        xor     al, al
        call    VgaPalReadAr
        test    al, al
        jnz     short vpr_chkend
        mov     al, 007h
        call    VgaPalReadAr
        cmp     al, 007h
        jne     short vpr_chkend
        mov     al, 00Fh
        call    VgaPalReadAr
        cmp     al, 03Fh
        jne     short vpr_chkend
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 004h
vpr_chkend:
        ; ---- ⑧ 读回自证: DAC[0] 的 R/G/B 必须全 0 ----
        mov     dx, 03C7h
        xor     al, al
        out     dx, al                          ; 读索引 = 条目 0
        mov     dx, 03C9h
        in      al, dx
        mov     bl, al
        in      al, dx
        or      bl, al
        in      al, dx
        or      bl, al
        test    bl, bl
        jnz     short vpr_out
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 008h
vpr_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc VgaPalRestore

;-----------------------------------------------------------------------------
; 【r412】VgaPalReadAr —— 读一个属性控制器寄存器
;   入口: AL = 索引(0..13h); 出口: AL = 值; 只读端口, 不动其它寄存器
;   相位纪律(FreeVGA: Attribute Controller): 3C0h 是索引/数据共用口,
;   读 3DAh 可以把内部 flip-flop 复位回索引相; 数据一律从 3C1h 读。
;-----------------------------------------------------------------------------
BeginProc VgaPalReadAr
        push    edx
        push    ecx
        mov     cl, al
        mov     dx, 03DAh
        in      al, dx                          ; 复位触发器
        mov     dx, 03C0h
        mov     al, cl
        or      al, 020h                        ; PAS=1
        out     dx, al
        inc     dx                              ; 3C1h = AR 数据读口
        in      al, dx
        pop     ecx
        pop     edx
        ret
EndProc VgaPalReadAr

;-----------------------------------------------------------------------------
; 【r412】VgaFontHalf —— 同时量 "ASCII 半区" 与 "上半区" 到底有没有字模
;
;   【为什么必须同时量两个】本项目的字模自证从 r167 到现在只验一个字形:
;     VfcFpRead     读 base+0822h / +0826h      (= 字符 41h, 32 字节步长)
;     VfcGlyphCmp   比 base+0820h 起的 16 字节  (= 字符 41h)
;   fontexp 与 fontp0 都只是 'A' 的指纹。而缺陷 B 落在码位 >= 80h 的上半区
;   (字符 c 的格子在 c*32, 所以 80h 从 base+1000h 才开始) —— 'A' 对、上半区
;   全 0, 所有闸门照样报绿。这就是 B 能 "恒定 60 轮" 的唯一原因: 不是它没变,
;   是我们从来没测过它。
;
;   【本过程做什么】只读。走与 VfcFpRead 完全相同的窗口纪律:
;     VfcBaseCheck -> VfcAccessOn -> 读两个字形 -> VfcTextGroup 写回文本组。
;   每个字形数 "16 个字节里有几个非零": 空白字形 = 0, 真字形 = 6..16。
;   落盘: FNT41(72) / FNTDB(76)。
;   【现场】EBX/ECX/EDX/ESI/EDI + flags 自己保存恢复; 全程 cli。
;-----------------------------------------------------------------------------

;-----------------------------------------------------------------------------
; 【r412】VgaFontTop —— 上半区没字模时, 让 卡自己的 BIOS 把 ROM 8x16 装回来
;
;   【判据(全部来自用户 2026-10-18 的三张新图)】
;     · 190b3 (r190, mode co80 之后): 页与颜色都对, 一个字符都画不出来
;       => 字模全空;
;     · 1961  (r196, mode co80 之后): ASCII 对, 码位 >= 80h 空白
;       => 只有 ASCII 半区;
;     · 1981  (r198, mode co80 之后): 两个半区都有 => 唯一的好状态。
;   => 目标状态只有一个: 1981。它是在一次真设模式之后出现的, 说明 "装字模"
;      对这颗卡是做得到的; 我们要补的就是这一步。
;
;   【为什么用 BIOS 而不是再拷一遍】我们自己的 4KB 拷贝里有一处机械性错位:
;   平面 2 字模拷贝 里源指针每格 +10h、目标每格 +20h, 而源确实是 32 字节/格的 VGA
;   字模镜像(证据: VfcSrc 的 fontexp 读的是源 +0822h/+0826h, 得到 10C6 =
;   真 CP437 'A')。所以 "再拷一遍" 不能保证把上半区写对。
;   BIOS 的 INT 10h AH=11h AL=14h (VGA: 装 ROM 8x16 字模) 就是参考实现里
;   biosfn_load_text_8_16_pat 的同一条路: 它自己 get_font_access() -> 按 32
;   字节步长写 256x16 -> release_font_access(), 不清屏、不改模式、不动调色板。
;
;   【闸门(一条都不能少)】
;     1. 本场一次(FontTopN, 4 号清 0);
;     2. ASCII 半区必须已经有字模(FNT41 >= 1) —— 否则交给老路(VfcLoad)去修,
;        这里一个字节都不写, 保证 "现在能显示的 ASCII" 绝不会被这一手弄坏;
;     3. 只有 FNTDB == 0 才动手; 两个字区都有就什么都不做;
;     4. VgaBiosFont 自己还有 policyInt10Fix 与本场两次的预算。
;   【状态位】PAL_ST: b4 = 检测到上半区空, b5 = BIOS 那一枪执行了,
;     b6 = 那一枪之后 DBh 变成非零。
;-----------------------------------------------------------------------------

;-----------------------------------------------------------------------------
; 【r412】VgaInt10Hook —— 挂在 VMM 的 V86 中断链上, 看盒子自己的每一条 INT 10h
;
;   【为什么必须是这一层】主线缺陷 A1: 从窗口 DOS 进入全屏, 页里不是控制台的屏, 而
;   是上一态的残留(190b1); 第二次进(190b2)才出现控制台的屏 —— 说明【控制台从没画过】,
;   它只在"一次真正的设模式"之后才整屏重画。r198 之所以全对, 就是因为你手打的
;   `mode co80` 是那样一次设模式。
;
;   我们已经证明这两条路【够不着】:
;     · mini-VDD 的 48 号 PRE_INT_10_MODE_SET: 全屏期间一次都不响
;       (`TRC fbox n48 while armed=0000`), 说明盒子的 INT 10h 绕过 mini-VDD 分派;
;     · 嵌套 V86 里自己发 INT 10h(AX=0003): 那是替盒子发, 不是盒子自己发, 不触发重画。
;
;   而 Hook_V86_Int_Chain 挂在 VMM 的 V86 中断链上, 位置【比 mini-VDD 分派更高】:
;   盒子每一条 INT 10h 都会经过它, 包括那些绕过我们的。DDK 原文(VMM 服务):
;   VMMCall Hook_V86_Int_Chain(EAX = 中断号, ESI = 过程地址) -> ESI = 句柄。
;   服务号 65 / 280 取自 DDK 的 VMM.INC 镜像(ref/vmdisp9x/vmm.h 的
;   VMM__Hook_V86_Int_Chain / VMM__Unhook_V86_Int_Chain)。
;
;   【本过程做什么】只认 AH=0(设模式), 且【只在盒子持屏期间】:
;   把客户帧里的 AL 从 72h/73h(远东文本模式, 这颗 BIOS 不认)改成 03h ——
;   于是 BIOS 收到一个它认识的模式 3, 做的是【完整的】设模式:
;   清显存 + 装字模 + 装调色板 + 让控制台重画。这正是 r198 那一次的内容。
;   其它任何中断、任何模式号: 一个字节都不改。
;   【返回约定】CF=0 = 继续走原来的链(不吞中断)。这一条必须正确, 否则盒子的
;   INT 10h 会被吞掉 —— 那是本版唯一的高风险点, 失败形态一眼可认(盒子当场死)。
;   【读数】I10N(68) / I10M0(72) / I10FIX(76), 全部饱和计数。
;-----------------------------------------------------------------------------
BeginProc VgaInt10Hook
        pushfd
        push    eax
        push    ebx
        ; 【r412】EBP 必须像一个客户寄存器帧才准碰它。V86 链上的约定是
        ;   "EBP = 当前 VM 的 Client_Reg_Struc", 但本项目无法在本机核对 DDK 原文,
        ;   而写坏 EBP 会直接毁掉 VMM 的内存 —— 所以先做范围检查, 不像就原样放行。
        cmp     ebp, 00001000h
        jb      vih_chain                      ; 远端: 让 MASM 自己选 near
        cmp     ebp, 0C0000000h
        jae     vih_chain                      ; 远端: near
        cmp     byte ptr [Probe + VBE_TR_PR_I10N], 0FFh
        jae     short vih_c1
        inc     byte ptr [Probe + VBE_TR_PR_I10N]
vih_c1:
        mov     ax, word ptr [ebp].Client_ax
        test    ah, ah
        jnz     short vih_chain                 ; AH != 0: 不是设模式
        cmp     byte ptr [Probe + VBE_TR_PR_I10M0], 0FFh
        jae     short vih_c2
        inc     byte ptr [Probe + VBE_TR_PR_I10M0]
vih_c2:
        cmp     byte ptr [VgaFixArm], 0          ; 只在盒子持屏期间动手
        je      short vih_chain
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      short vih_chain
        ; 【r412】★★ 这里【不再改写任何东西】★★
        ;   r235 证明: 把 VDD 送出去的 72h 改成 03h, BIOS 照 03h 设了模式,
        ;   而【主 VDD 的模式记账也变成 03h】, 盒子却仍以为自己是 72h ⇒
        ;   虚拟显存映射错位: 全屏只剩桌面(2351.png, 720x400 的桌面),
        ;   窗口里把桌面帧缓冲当字符单元画(2352.png)。与 r149/r150 同源。
        ;   ⇒ 模式号永久不许动。本版只【看】不看错:
        ;     见过 03h -> PAL_ST b4;  见过 72h -> b5;  见过 73h -> b6;
        ;     72h/73h 的"本来会被改写"次数仍然记在 I10FIX(纯计数, 不写硬件)。
        cmp     al, 003h
        jne     short vih_c3
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 010h
vih_c3:
        cmp     al, 072h
        jne     short vih_c72
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 020h
vih_c72:
        cmp     al, 073h
        jne     short vih_c73
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 040h
vih_c73:
        cmp     al, 072h
        je      short vih_count
        cmp     al, 073h
        jne     short vih_chain
vih_count:
        cmp     byte ptr [Probe + VBE_TR_PR_I10FIX], 0FFh
        jae     short vih_chain
        inc     byte ptr [Probe + VBE_TR_PR_I10FIX]
vih_chain:
        pop     ebx
        pop     eax
        popfd
        clc                                      ; CF=0: 继续原来的链
        ret
EndProc VgaInt10Hook

; r412 D: INT 10h 钩子启用 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

;-----------------------------------------------------------------------------
; 【r412】TraceStub_45 —— SAVE_MESSAGE_MODE_STATE(45), 手写
;
;   官方语义(DDK《Mini-VDD》函数表): 主 VDD 在把硬件"强行"切成平面文本态
;   (给全屏 DOS 盒子用)的前后各调一次 45/46, 让 mini-VDD 保存/记录自己的状态。
;   本机每次开机各响一次(日志 `TRC cnt f=2D/f=2E n=00000001`)。
;
;   【本过程做什么】只读 + 只记账, 【仍然返回 NC】:
;     · 不改语义(不接管, 主 VDD 照旧做它自己的事);
;     · 不写任何硬件端口(除了 3C4/3CE 的索引-数据读, 那是读寄存器必须的);
;     · 记: (SR04<<4)|(GCR06&0F) 与 客户帧里的 AL。
;   这两个数就是"切平面态这一刻, 卡的显存视图到底是什么"——A1 的落点若存在,
;   只能在这里看到, 因为寄存器层我们自己已经写遍了。
;
;   【槽位】68/72/76 是已死的 V86 INT 10h 钩子(I10N/I10M0/I10FIX)的格子:
;   那个钩子【不再安装】(r237 证据), 所以这三格被本观察器复用, 不新开格子。
;-----------------------------------------------------------------------------
BeginProc TraceStub_45
        pushfd
        push    eax
        push    ebx
        push    edx
        mov     eax, 45
        call    MiniVDD_TraceRecord
        ; ---- 客户帧里的 AL ----
        mov     ax, word ptr [ebp].Client_ax
        mov     [Probe + VBE_TR_PR_I10FIX], al     ; 45: 客户 AL
        ; ---- 显存视图: SR04 与 GCR06 的低四位打成一字节 ----
        mov     dx, 03C4h
        mov     al, 004h
        out     dx, al
        inc     dx
        in      al, dx
        shl     al, 4
        mov     bl, al
        mov     dx, 03CEh
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        and     al, 00Fh
        or      bl, al
        mov     [Probe + VBE_TR_PR_I10N], bl       ; 45: (SR04<<4)|(GCR06&0F)
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 010h   ; b4 = 45 响过
        pop     edx
        pop     ebx
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + 45*4], 0
        je      short ts45_nc
        jmp     dword ptr [OldDispatch + 45*4]
ts45_nc:
        VDD_NC
        ret
EndProc TraceStub_45

;-----------------------------------------------------------------------------
; 【r412】TraceStub_46 —— SAVE_FORCED_PLANAR_STATE(46), 手写
;   与 45 完全对称: 同一组读数写进 72 号格, PAL_ST 的 b5 = 46 响过。
;-----------------------------------------------------------------------------
BeginProc TraceStub_46
        pushfd
        push    eax
        push    ebx
        push    edx
        mov     eax, 46
        call    MiniVDD_TraceRecord
        mov     dx, 03C4h
        mov     al, 004h
        out     dx, al
        inc     dx
        in      al, dx
        shl     al, 4
        mov     bl, al
        mov     dx, 03CEh
        mov     al, 006h
        out     dx, al
        inc     dx
        in      al, dx
        and     al, 00Fh
        or      bl, al
        mov     [Probe + VBE_TR_PR_I10M0], bl       ; 46: (SR04<<4)|(GCR06&0F)
        or      byte ptr [Probe + VBE_TR_PR_PAL_ST], 020h   ; b5 = 46 响过
        pop     edx
        pop     ebx
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + 46*4], 0
        je      short ts46_nc
        jmp     dword ptr [OldDispatch + 46*4]
ts46_nc:
        VDD_NC
        ret
EndProc TraceStub_46

; r412 C7b: VgaPageWitness removed.

;-----------------------------------------------------------------------------
; r412: TraceStub_34 -- SET_BANK(34), 手写, 真正用 VBE 4F05h 换 bank
;   [文档] DDK: EAX = 读 bank, EDX = 写 bank; CY = 已处理, NC = 主 VDD 自己用 VESA。
;   实现: BH=1 设读窗, BH=0 设写窗 (4F05h), 每次调用后看 BcRet 的 AH 是否为 4F。
;   闸门: policyBank != 0 才动手; 任何一步失败 -> NC (让主 VDD 走它的 VESA 兜底)。
;   读数: BS3ST(26) = 1 成功 / 2 失败 / 3 策略关 / 0 未调用。
;-----------------------------------------------------------------------------
BeginProc TraceStub_34
        pushfd
        push    eax
        push    ebx
        push    edx
        mov     eax, 34
        call    MiniVDD_TraceRecord
        cmp     byte ptr [VbeState.VBS_policyBank], 0
        je      ts34_off                     ; 远端: near
        mov     ax, word ptr [ebp].Client_eax      ; 读 bank
        mov     dx, word ptr [ebp].Client_edx      ; 写 bank
        mov     [Probe + VBE_TR_PR_BS3ST], 1
        cmp     ax, dx
        je      short ts34_w
        ; BH=1: 设读窗 = AX
        mov     word ptr [BcAx], 04F05h
        mov     word ptr [BcBx], 0100h
        mov     word ptr [BcCx], 0
        mov     word ptr [BcDx], ax
        mov     word ptr [BcEs], 0
        call    VgaBiosInt10
        cmp     word ptr [BcRet], 004Fh
        jne     short ts34_bad
        mov     ax, word ptr [ebp].Client_eax
        mov     dx, word ptr [ebp].Client_edx
ts34_w:
        ; BH=0: 设写窗 = DX
        mov     word ptr [BcAx], 04F05h
        mov     word ptr [BcBx], 0000h
        mov     word ptr [BcCx], 0
        mov     word ptr [BcDx], dx
        mov     word ptr [BcEs], 0
        call    VgaBiosInt10
        cmp     word ptr [BcRet], 004Fh
        jne     short ts34_bad
        pop     edx
        pop     ebx
        pop     eax
        popfd
        VDD_CY
        ret
ts34_bad:
        mov     byte ptr [Probe + VBE_TR_PR_BS3ST], 2
ts34_off:
        cmp     byte ptr [Probe + VBE_TR_PR_BS3ST], 1
        jne     ts34_nc                      ; 远端: near
        mov     byte ptr [Probe + VBE_TR_PR_BS3ST], 3
ts34_nc:
        pop     edx
        pop     ebx
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + 34*4], 0
        je      short ts34_done
        jmp     dword ptr [OldDispatch + 34*4]
ts34_done:
        VDD_NC
        ret
EndProc TraceStub_34

; r412 D: CRTC 一致性检查 removed -- zero code references (calls, addresses, outside labels,
;   and data labels inside it all verified in r412 analysis).

BeginProc KbdTypeMode
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      ktm_out
        cmp     byte ptr [KbdDone], 0
        jne     ktm_out
        mov     byte ptr [KbdDone], 1
        ; 现在的尾位置
        mov     edi, 000041Eh
        movzx   ebx, word ptr [edi]
        ; 尾位置越界就回绕
        cmp     ebx, 000001Eh
        jae     short ktm_ok
        mov     ebx, 000001Eh
ktm_ok:
        mov     esi, OFFSET32 KbdStr
        mov     ecx, KbdStrN
ktm_loop:
        mov     edi, 0000400h
        add     edi, ebx
        mov     ax, [esi]
        mov     [edi], ax
        add     ebx, 2
        cmp     ebx, 000003Eh
        jb      short ktm_nowrap
        mov     ebx, 000001Eh
ktm_nowrap:
        add     esi, 2
        dec     ecx
        jnz     short ktm_loop
        ; 写回尾
        mov     edi, 000041Eh
        mov     [edi], bx
ktm_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc KbdTypeMode

BeginProc MiniVDD_AccessVgaMemoryMode
        pushfd
        push    eax
        mov     eax, 11
        call    MiniVDD_TraceRecord
        pop     eax
        popfd
        ; ---- 本轮的工作(现场自保) ----
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        inc     byte ptr [Probe + VBE_TR_PR_N11]
        ; r412: key injection moved to fn 12 (that is where the box is current)
        ; ① 盒子当前的 BIOS 模式(当前 VM 的 BDA 0:0449)
        mov     edi, 00449h
        movzx   eax, byte ptr [edi]
        mov     [Probe + VBE_TR_PR_M11], al
        ; ② 进门先读一次"模态"的两个关键寄存器(GCR06 / SR04)
        mov     dx, 03CEh
        mov     al, 006h
        inc     dx
        mov     [Probe + VBE_TR_PR_G11], al
        mov     dx, 03C4h
        mov     al, 004h
        inc     dx
        mov     [Probe + VBE_TR_PR_S11], al
        ; ③ 闸门 + 模式判定
        cmp     byte ptr [VbeState.VBS_policyInt10Fix], 0
        je      short afn11_out
        mov     al, [Probe + VBE_TR_PR_M11]
        cmp     al, 003h
        je      short afn11_text
        cmp     al, 002h
        je      short afn11_text
        cmp     al, 007h
        je      short afn11_text
        cmp     al, 000h
        je      short afn11_text
        cmp     al, 001h
        je      short afn11_text
        mov     byte ptr [Probe + VBE_TR_PR_T11], 002h
        jmp     short afn11_out
afn11_text:
        call    VgaM3Apply                          ; 写正则文本组 + 自证, AL = 0/1
        mov     [Probe + VBE_TR_PR_T11], al
afn11_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        call    VgaPalRestore                        ; r412: 11 号也补一次(先到先做)
        cmp     dword ptr [OldDispatch + 11*4], 0
        je      short av11_null
        jmp     dword ptr [OldDispatch + 11*4]
av11_null:
        VDD_NC
        ret
EndProc MiniVDD_AccessVgaMemoryMode

;-----------------------------------------------------------------------------
; 【r412】ACCESS_LINEAR_MEMORY_MODE (12) —— 对称的那一半
;
;   DDK 对这一个没有给出独立页(它与 11 号成对: 一个说"走 VGA 孔径",
;   一个说"走 linear 帧缓冲"), 所以本轮它【不写硬件】:
;   只记次数与进门时的 GCR06/SR04。理由很直接 —— linear 态是显示驱动
;   与 BIOS 自己要的状态, 我们凭一个未证实的猜测去写它只会
;   把桌面弄坏。这一格的读数是下一轮写它的依据。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_AccessLinearMemoryMode
        pushfd
        push    eax
        mov     eax, 12
        call    MiniVDD_TraceRecord
        pop     eax
        popfd
        pushfd
        push    eax
        push    edx
        push    edi
        inc     byte ptr [Probe + VBE_TR_PR_N12]
        call    VgaDotClock                        ; r412: SR01 bit0 = f(box mode), in the box context
        call    KbdTypeMode
        mov     edi, 00449h
        movzx   eax, byte ptr [edi]
        mov     [Probe + VBE_TR_PR_T12], al
        mov     dx, 03CEh
        mov     al, 006h
        inc     dx
        mov     [Probe + VBE_TR_PR_G12], al
        mov     dx, 03C4h
        mov     al, 004h
        inc     dx
        mov     [Probe + VBE_TR_PR_S12], al
        pop     edi
        pop     edx
        pop     eax
        popfd
        cmp     dword ptr [OldDispatch + 12*4], 0
        je      short al12_null
        jmp     dword ptr [OldDispatch + 12*4]
al12_null:
        VDD_NC
        ret
EndProc MiniVDD_AccessLinearMemoryMode

BeginProc MiniVDD_GetTotalVramSize

        ; 【r174】这里【故意】直接回答 NC。
        ;   MS DDK: 只要 GET_TOTAL_VRAM_SIZE(36) / GET_BANK_SIZE(37) 给了值,
        ;   主 VDD 就会用 SET_BANK(34) / GET_VDD_BANK(1) / SET_VDD_BANK(2) /
        ;   RESET_BANK(3) 去换 bank; 而这四个在本驱动里是空桩 ⇒ 盒子经 VDD
        ;   窗口写进来的字节会落到卡片不译码的地方。./173 的物证: mode co80
        ;   之后字模永久丢掉(fontn=0), 而把 ROM 字模直接写进平面 2 偏移 0
        ;   就能恢复字 —— 说明 VDD 的窗口与卡片译码不一致。
        ;   在 1/2/3/34 用 VBE 4F05h 真正实现之前, 36/37 必须闭嘴。
        ; r412: 36 号不再一律答 NC, 由 INI 的 Bank 开关决定。
        ;   8bpp 的盒子卡死证明主 VDD 真的会走银行路径, 而一律沉默会逼它先去
        ;   问 VESA 4F00h —— 那是一次发生在盒子上下文里的慢调用。
        cmp     byte ptr [VbeState.VBS_policyBank], 0
        je      short gtv_nc
        pushfd
        push    eax
        mov     eax, 36
        call    MiniVDD_TraceRecord
        pop     eax
        popfd

        cmp     [StateValid], 0
        je      short gtv_nc
        mov     eax, [VbeState.VBS_vramSize]
        test    eax, eax
        jz      short gtv_nc
        mov     [ebp].Client_ECX, eax
        VDD_CY
        ret
gtv_nc:
        VDD_NC
        ret

EndProc MiniVDD_GetTotalVramSize

;-----------------------------------------------------------------------------
; GET_BANK_SIZE (37)
; 告诉主 VDD 每个 bank 多少字节(它据此把显存偏移换算成 4F05h 的
; bank 号)，以及从哪个物理地址访问显存。
; Client_EAX=0 表示标准 A000:0 银行窗口 —— 即使卡上存在 LFB，
; 主 VDD 的银行路径也始终针对 A000 窗口；LFB 只由 display.drv
; 按恒等映射地址(经 ApplyState 页对齐填入 lfbLinear)用于 DIBENG
; 帧缓冲，与主 VDD 的 DOS 虚拟化路径互不相干。
; bankSizeKB 的非 0/上限修正已在 ApplyState 完成，此处直接使用。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_GetBankSize

        ; 【r174】这里【故意】直接回答 NC。
        ;   MS DDK: 只要 GET_TOTAL_VRAM_SIZE(36) / GET_BANK_SIZE(37) 给了值,
        ;   主 VDD 就会用 SET_BANK(34) / GET_VDD_BANK(1) / SET_VDD_BANK(2) /
        ;   RESET_BANK(3) 去换 bank; 而这四个在本驱动里是空桩 ⇒ 盒子经 VDD
        ;   窗口写进来的字节会落到卡片不译码的地方。./173 的物证: mode co80
        ;   之后字模永久丢掉(fontn=0), 而把 ROM 字模直接写进平面 2 偏移 0
        ;   就能恢复字 —— 说明 VDD 的窗口与卡片译码不一致。
        ;   在 1/2/3/34 用 VBE 4F05h 真正实现之前, 36/37 必须闭嘴。
        ; r412: 37 号同上 —— 由 Bank 开关决定是否把显存几何告诉主 VDD。
        cmp     byte ptr [VbeState.VBS_policyBank], 0
        je      short gbs_nc
        pushfd
        push    eax
        mov     eax, 37
        call    MiniVDD_TraceRecord
        pop     eax
        popfd

        cmp     [StateValid], 0
        je      short gbs_nc
        movzx   eax, byte ptr [VbeState.VBS_bankSizeKB]
        shl     eax, 10                 ; KB → 字节
        mov     [ebp].Client_EDX, eax
        mov     dword ptr [ebp].Client_EAX, 0
        VDD_CY
        ret
gbs_nc:
        VDD_NC
        ret

EndProc MiniVDD_GetBankSize

;-----------------------------------------------------------------------------
; DISPLAY_DRIVER_DISABLING (26)
; 显示驱动即将卸载(如切回 VGA)。作废旧状态：驱动一卸，其中的几何
; 参数不再可信，此后 GET_TOTAL_VRAM_SIZE / GET_BANK_SIZE 会因
; StateValid=0 返回 NC，主 VDD 回到 BIOS 查询路径 —— 安全降级。
; 返回 NC：这是纯通知，主 VDD 无需我们做更多。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_DisplayDriverDisabling

        ; 【r100 / D4 修正】r99 里这三个"实处理函数"(26/36/37)没有记账,
        ;   于是 drv 侧按 TraceCounts[26] 变化的 Heal 判据永远不成立 ——
        ;   与 D3 同族的静默死开关。此处补上记录与事件计数。
        pushfd
        push    eax
        mov     eax, 26
        call    MiniVDD_TraceRecord
        inc     byte ptr [TraceEvt + VBE_TR_EVT_DISPLAY_DISABLING]
        pop     eax
        popfd

        ; 【r101】状态机: 驱动被停用 ⇒ 硬件即将(或已经)回到 VGA/文本,
        ;   此后 35 号不能再答 "CY"(那会让主 VDD 以为 Windows 的高分辨率
        ;   屏还在)。1 = VBE_TR_ST_VGA, 4 = VBE_TR_ST_DISABLED。
        or      byte ptr [TraceState], 05h

        ; 【r116】驱动被停用 ⇒ Windows 也不再是屏幕主人: 撤掉 1CEh/1CFh 的
        ;   全局陷阱(参考实现 boxv9x 的 DISPLAY_DRIVER_DISABLING 分支同此:
        ;   "关机/卸载: 解除陷阱")。策略关或没装成时它是一个立即返回。
        call    MiniVDD_DispiTrapOff

        mov     [StateValid], 0
        VDD_NC
        ret

EndProc MiniVDD_DisplayDriverDisabling

;-----------------------------------------------------------------------------
; VESA_CALL_POST_PROCESSING (47)
; 每次 VESA 调用后的收尾钩子。EDX 低字 = 刚执行的 VESA 功能号；
; 若是 4F02h(设模式)，高字 = 新模式号。
; 【纯记账】DOS 全屏应用直接调 VESA BIOS 设模式时不经过 display.drv，
; 借这个钩子把 currentMode 同步回真实值；不拦截、不修改任何硬件
; 行为，返回 NC 让主 VDD 继续它自己的收尾。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_VesaCallPost

        ; r99: 先记一条(func=47) —— 必须放在下面 shr 之前, 否则功能号会
        ; 被模式号覆盖。记录里 EDX 低字 = VESA 功能号, 高字 = 4F02 的模式
        ; 号, 于是"窗口化 DOS 期间到底有几次改模式、改成什么"完全可见。
        pushfd
        push    eax
        mov     eax, 47
        call    MiniVDD_TraceRecord
        pop     eax
        popfd

        cmp     dx, 4F02h
        jne     short vcp_done
        shr     edx, 16
        mov     [VbeState.VBS_currentMode], dx
vcp_done:
        VDD_NC
        ret

EndProc MiniVDD_VesaCallPost

;-----------------------------------------------------------------------------
; VESA_SUPPORT (41)
; 主 VDD 检测到 VESA 调用(INT 10h AX=4Fxx)时的决策钩子。
; DDK 约定:
;   CY = "mini-VDD 已完全处理, 不要去调 VESA BIOS"(客户端寄存器里应有
;        本 VxD 填好的返回值);
;   NC = "未处理, 请按你自己的策略处理"(主 VDD 去调真 VESA BIOS, 或者
;        在窗口化/后台 VM 情况下走它自己的虚拟化)。
;
; 【r76: 恒返回 NC】本驱动完全不实现 ring 0 VESA —— 那是卡专用
; mini-VDD 的可选加速项(DDK 原话 "This is recommended..."), 通用 VBE
; 驱动把它留给真 BIOS 才是正确且最兼容的选择。
; 旧实现按"发起 VM 是不是系统 VM"分别返回 NC/CY, 把 CY 误解成"请主
; VDD 做默认虚拟化", 而实际语义是"我已经处理完了, 别再调 BIOS" ——
; 于是 DOS VM(后台/全屏 DOS 程序, 包括各种 VESA 游戏与检测工具)的
; 4Fxx 调用被无声吞掉、寄存器拿到垃圾值: 典型的黑屏/花屏来源。
; 系统 VM 侧本来就必须 NC(Windows 侧的直通枚举要真 BIOS 数据),
; 现在两种 VM 统一 NC, 语义与 DDK 文档完全一致。
; 入口: EBX = 发起调用的 VM 句柄；EBP → 客户端寄存器结构。
; 注: 本驱动【不】读 EBX, 只保留 VesaCallCount 计数供排障(drv 可经
;     VBE_API_GET_DBG 取; 也能看出主 VDD 是否在走本钩子)。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_VesaSupport

        inc     dword ptr [VesaCallCount] ; 排障计数(每次主 VDD 调用都+1)

        ; r99: 记一条(func=41, Client_ECX/EDX 原样入记录)。这是回答
        ; "窗口化 DOS 时那串 4Fxx 到底是谁发的"的第一现场: 记录里同时
        ; 有功能号候选与两个寄存器, 事后按数据判定主 VDD 的寄存器约定。
        pushfd
        push    eax
        mov     eax, 41
        call    MiniVDD_TraceRecord
        pop     eax
        popfd

        cmp     byte ptr [VbeState.VBS_policyVesa], 0
        je      short vs_nc

        ; 【r101 判据修正】原来的判据只有"调用者不是 Windows VM", 但实测
        ;   分派入口的 EBX 与 PM API 入口记下的句柄并不是同一套取值(见
        ;   TRC 记录里三种不同的 ebx), 单靠它会误伤主 VDD 自己的 VESA 调用
        ;   (每次会话 43 次, 其中就包含它设模式的 4F02)。
        ;   现在改成两个条件【同时】成立才拒绝:
        ;     ① Windows 屏幕仍在场(状态机没被切成 VGA) —— 说明硬件此刻是
        ;        我们的线性打包像素屏, 谁都不该改它;
        ;     ② 调用者不是 Windows VM(即不是主 VDD/display.drv 自己)。
        ;   全屏 DOS 会话期间(bit0=1)完全放行, 那是 DOS VM 的合法权利。
        mov     al, [TraceState]
        test    al, VBE_TR_ST_VGA
        jnz     short vs_nc                     ; 已切到 VGA: 放行
        cmp     dword ptr [WinVMHandle], 0
        je      short vs_nc                     ; 未知 Windows VM: 保守
        cmp     ebx, [WinVMHandle]
        je      short vs_nc                     ; Windows 自己的调用: 放行
        mov     eax, [ebp].Client_EDX
        mov     ecx, [ebp].Client_EBX
        call    MiniVDD_VesaFnClass             ; AL = 1 表示"改硬件"类
        test    al, al
        jz      short vs_nc
        mov     eax, 0177h                      ; VESA: 功能调用不成功
        mov     [ebp].Client_EAX, eax
        VDD_CY
        ret

vs_nc:
        VDD_NC                          ; 未处理 → 主 VDD / 真 VESA BIOS
        ret

EndProc MiniVDD_VesaSupport

;-----------------------------------------------------------------------------
; MiniVDD_VesaFnClass —— 判定候选寄存器里是不是"改硬件"类 4Fxx
;   入口: EDX、ECX 各持一个候选功能号(DDK 未明确约定用哪个)
;   出口: AL = 1 改硬件(4F02/03/05/06/07/09) / 0 只读或无法识别
;   叶子函数, 只动 EAX。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_VesaFnClass

        mov     eax, edx
        call    MiniVDD_VesaFnOne
        test    al, al
        jnz     short vfc_yes
        mov     eax, ecx
        call    MiniVDD_VesaFnOne
        ret
vfc_yes:
        mov     al, 1
        ret

EndProc MiniVDD_VesaFnClass

BeginProc MiniVDD_VesaFnOne

        movzx   eax, ax
        cmp     ax, 4F00h
        jb      short vfo_no
        cmp     ax, 4F01h
        jbe     short vfo_no                    ; 只读查询: 放行
        cmp     ax, 4F02h
        je      short vfo_yes
        cmp     ax, 4F03h
        je      short vfo_yes
        cmp     ax, 4F05h
        je      short vfo_yes
        cmp     ax, 4F06h
        je      short vfo_yes
        cmp     ax, 4F07h
        je      short vfo_yes
        cmp     ax, 4F09h
        je      short vfo_yes
vfo_no:
        xor     al, al
        ret
vfo_yes:
        mov     al, 1
        ret

EndProc MiniVDD_VesaFnOne

;=============================================================================
; V86 / PM API —— INT 2F/AX=1684h 返回的入口
;
; 入口: EBP → 调用方 VM 的 Client_Reg_Struc
; 约定(与 vbeshare.h 中注释一致，刻意只用 16 位寄存器)：
;   AX    = 功能号(VBE_API_*)
;   DX:CX = 缓冲区在当前 VM 中的 32 位线性地址
;   BX    = 缓冲区字节数
; 出口: 客户端 CF=1 出错；AX = 状态码/版本号
;
; 【为何两个入口共用一个实现】
;   客户端传的是线性地址，VxD 无需关心对方是 V86 还是 PM ——
;   这就是契约里让客户端自己换算地址换来的好处。
; 【为何线性地址在当前上下文有效】
;   API 调用发生时，VMM 已切到调用方 VM 的地址空间；该 VM 的
;   低 1MB(V86)与其 LDT 选择子覆盖的区域(PM)此刻都可直接访问。
;=============================================================================
BeginProc MiniVDD_V86_API

        call    MiniVDD_API_Common
        ret

EndProc MiniVDD_V86_API

BeginProc MiniVDD_PM_API

        ; r99: 记下 Windows VM 句柄 —— PM API 只会被 display.drv 调用,
        ; 而 drv 一定跑在 Windows VM 里。VESA_SUPPORT(41) 的策略靠它
        ; 区分"Windows 自己的 VESA 调用"(必须放行)与"DOS VM 的"(可拒绝)。
        ; 【r111 收紧为"首次调用者锁定"】原实现每次 PM API 调用都覆盖它,
        ; 而 r111 起 VxdSetInCallback 会在【主 VDD 的回调窗口里】走一次 PM
        ; API(VBE_API_SET_INCB) —— 那一刻的"当前 VM"未必是 Windows VM,
        ; 覆盖它就等于把"谁是 Windows VM"这份参照改成了"当前是谁", 于是
        ; api_exec 里那道 VM 自检会退化成恒真。变量注释本来写的就是
        ; "首次调用者锁定", 这里让代码与注释一致。                     */
        ; [r132] Take the Windows VM handle from the VMM, not from the caller's
        ; EBX.  The VDD's own callback window also enters this API (VBE_API_
        ; SET_INCB), and "current VM" there is not necessarily the Windows VM;
        ; an earlier round recorded EBX and then measured that the EBX seen at
        ; a CHECK_HIRES_MODE call did not match it -- which is exactly why the
        ; per-VM answer had to be given up back then.  The comparison was
        ; against a hand-me-down value, not against a real VM handle.
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        VMMCall Get_Cur_VM_Handle               ; EBX = current VM handle
        test    ebx, ebx
        jz      short pm_no_vm
        cmp     dword ptr [WinVMHandle], 0
        jne     short pm_no_vm
        mov     [WinVMHandle], ebx
pm_no_vm:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        call    MiniVDD_API_Common
        ret

EndProc MiniVDD_PM_API

BeginProc MiniVDD_API_Common

        ; 把 16 位寄存器参数重组为 32 位：
        ;   ESI = 线性地址(DX 高字 : CX 低字), ECX = 大小(BX), AX = 功能号
        movzx   esi, word ptr [ebp].Client_EDX
        shl     esi, 16
        mov     si, word ptr [ebp].Client_ECX
        movzx   ecx, word ptr [ebp].Client_EBX
        movzx   eax, word ptr [ebp].Client_EAX

        cmp     ax, VBE_API_GET_VERSION
        je      api_ver                 ; 不带 short: 分派项增多后目标超出
        cmp     ax, VBE_API_PUSH_STATE  ; 128B 射程(A2075 实锤), 交给 MASM
        je      api_push                ; 自选 short/near 编码
        cmp     ax, VBE_API_GET_STATE
        je      api_get
        cmp     ax, VBE_API_GET_DBG
        je      api_dbg
        cmp     ax, VBE_API_EXEC_BIOS
        je      api_exec
        cmp     ax, VBE_API_EXEC_NOOP
        je      api_noop
        cmp     ax, VBE_API_IVT_PROBE
        je      api_ivtprobe
        cmp     ax, VBE_API_VM_HANDLE_PROBE
        je      api_vmhprobe
        cmp     ax, VBE_API_NEST_BASE
        je      api_nestbase
        cmp     ax, VBE_API_GET_MAPINFO
        je      api_getmapinfo
        cmp     ax, VBE_API_GET_MODELIST
        je      api_getmodelist
        cmp     ax, VBE_API_GET_TRACE
        je      api_gettrace
        cmp     ax, VBE_API_SET_INCB
        je      api_setincb
        cmp     ax, VBE_API_DISPI_PHYS
        je      api_dispi_phys
        mov     word ptr [ebp].Client_EAX, 87   ; ERROR_INVALID_PARAMETER
        VDD_CY
        ret

;-----------------------------------------------------------------------------
; api_dispi_phys —— VBE_API_DISPI_PHYS(15): ring 0 物理读 DISPI 寄存器。
;   【r120】入口: EAX=15, 无其它参数。
;   出口: Client_EAX = DispiPhysN 的新值(这条路径跑过几次, 模 256),
;         CF=0 = 读过; 六个寄存器的值在锁定段的 DispiPhysVal[6], 由下一次
;         VBE_API_GET_TRACE(13) 随 blob 的 VBE_TR_DispiPhys(=216) 带出去。
;   【为什么不在本调用里回值】请求块的 16 位线性地址只有一个字段可用,
;   而 6 个字走 blob 那条已经验证过的通路更省事(也不增加新的约定)。
;-----------------------------------------------------------------------------
api_dispi_phys:
        call    MiniVDD_DispiPhysRead
        movzx   eax, byte ptr [DispiPhysN]
        mov     word ptr [ebp].Client_EAX, ax
        VDD_NC
        ret

;-----------------------------------------------------------------------------
; api_setincb —— VBE_API_SET_INCB(14): 置/清"回调窗口"标记。
;   【r111】入口: EAX=14, EBX = 0(离开回调) / 非 0(进入回调)。
;   只写一个字节, 绝不做别的事 —— 它会在主 VDD 的回调里被调用, 那一刻
;   任何多余动作都是风险(见 InCbFlag 的注释)。
;-----------------------------------------------------------------------------
api_setincb:
        movzx   eax, byte ptr [ebp].Client_EBX
        mov     [InCbFlag], al
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret

api_dbg:
        mov     eax, [VesaCallCount]
        mov     word ptr [ebp].Client_EAX, ax
        VDD_NC
        ret

api_ver:
        mov     word ptr [ebp].Client_EAX, VBE_VXD_VERSION
        VDD_NC
        ret

api_push:
        ; 【r76】PUSH 只做一件事: 把 drv 推来的共享状态收进 VbeState,
        ; 顺带代 drv 把它的屏选择子登记给主 VDD(见 ApplyState)。
        ; r53-r75 在这里跑过的映射阶梯/选择子自建已整体删除 —— 帧缓冲
        ; 映射现在由 display.drv 在 ring 3 用 DPMI 0800h 完成(文件头)。
        mov     edx, esi
        call    MiniVDD_ApplyState              ; EDX=源, ECX=长度
        test    eax, eax
        jnz     short api_err
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret
api_err:
        mov     word ptr [ebp].Client_EAX, ax
        VDD_CY
        ret

api_get:
        cmp     [StateValid], 0
        je      short api_notready
        cmp     ecx, VBE_STATE_SIZE
        jb      short api_err2
        mov     edi, esi
        mov     esi, OFFSET32 VbeState
        mov     ecx, VBE_STATE_SIZE/4
        cld
        rep     movsd
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret
api_notready:
        ; 状态从未推送过：交还一堆全 0 比报错更危险(调用方可能当真)
        mov     word ptr [ebp].Client_EAX, 21   ; ERROR_NOT_READY
        VDD_CY
        ret
api_err2:
        mov     word ptr [ebp].Client_EAX, 87
        VDD_CY
        ret

;-----------------------------------------------------------------------------
; api_exec —— VBE_API_EXEC_BIOS(4): 嵌套 V86 直通 VBE BIOS(最终方案)
;   入口: ESI = VBEEXECREQ 线性地址(当前 VM 上下文可直接访问)。
;   【r46 起】数据中转缓冲与桩全部 VxD 自给, 客户端零低内存接触:
;     · bufLen!=0 → 执行前 VxD 把客户端 bufLin 处 bufLen 字节拷入
;       自有 V86 常驻缓冲(EnsureV86 在 drv 期经嵌套 AH=48h 分得, 触摸
;       测试过 g_v86Ok=1; 未建则先惰性建立, 见 EnsureV86 头注), 执行
;       客户端输入), 执行后整批拷回 —— 数据交换全经线性地址, 客户
;       端不建选择子、不分配 DOS 内存(./45 实锤: 客户端自建选择子
;       写低内存在全新 Win98 Enable 期 GP 死机)。
;     · 桩字节(9C 9A ip:cs CF)由 VxD 执行前写入自有缓冲+768, callf
;       目标当次现取(Get_V86_Int_Vector 读真 V86 IVT[10h]); IVT[vec]
;       借道前把请求块 stubSeg/stubOff 字段改写成 g_v86Seg:
;       VBE_EXEC_STUB_OFF, 复用 ae_borrow_vec 的回读自校验(api_noop
;       诊断相仍走客户端桩路径, 但它已不在生产路径上)。
;   流程: [拷入+写桩] → ae_borrow_vec → Push/Begin_Nest/填客户寄存器/
;   Exec_Int → 读回 → [拷出] → End_Nest/Pop → ae_restore_vec。
;   【本过程不碰任何原始页 0 内存】(./31 实锤: IVT 区域恒等直读直写
;   不可用; V86 常规内存 g_v86Lin 的直读直写由 EnsureV86 触摸测试背书
;   (r48 起)。借/还向量全部经
;   Get/Set_V86_Int_Vector —— VMM 按 VM 安全换算地址。
;-----------------------------------------------------------------------------
api_exec:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        movzx   eax, word ptr [esi + VXE_vec]
        cmp     eax, 40h
        jb      ae_fail

        ; --- 【r111】回调期 VM 自检 ---------------------------------------
        ; 只在"显示驱动正处在主 VDD 回调里"(InCbFlag != 0)时生效: 那时
        ; 嵌套 V86 执行会落在【当前 VM】里, 必须是 Windows VM。判定规则
        ; 保守 —— 只有拿到确凿的 Windows VM 句柄且二者【不等】才拒绝;
        ; 句柄未知(0)时放行(与 r110 的"回调里一律拒绝"相比, 那是另一道
        ; 护栏, 见 drv.c 的 VbeExecBios)。
        cmp     byte ptr [InCbFlag], 0
        je      short ae_vm_ok
        cmp     dword ptr [WinVMHandle], 0
        je      short ae_vm_ok
        VMMCall Get_Cur_VM_Handle
        cmp     ebx, [WinVMHandle]
        je      short ae_vm_ok
        inc     dword ptr [CbBiosRefused]
        mov     word ptr [ebp].Client_EAX, 93   ; 回调上下文里 VM 不是 Windows
        VDD_CY
        ret
ae_vm_ok:

        ; --- 需要数据中转但缓冲未建 → 惰性建立; 仍不可用才报错 ---------
        ; 【r48】分配延迟到 drv 期 PM API 上下文: ./47 实锤 Device_Init
        ; (PNP 装载, 系统 VM 正忙于实模式引导)期嵌套 Exec_Int(21h)
        ; AH=48h 失败(步3→103, DOS 引导期未就绪是最大嫌疑); 而 drv 期
        ; 嵌套执行是 api_exec 的既有生产机制。EnsureV86 自持 ESI/EDI。
        movzx   eax, word ptr [esi + VXE_bufLen]
        test    eax, eax
        jz      ae_bufok
        cmp     dword ptr [g_v86Ok], 1
        je      ae_bufok
        call    MiniVDD_EnsureV86
        cmp     dword ptr [g_v86Ok], 1
        jne     ae_nobuf
ae_bufok:

        ; --- 拷入(客户端线性缓冲 → V86 缓冲), 随后写桩 --------------------
        movzx   ecx, word ptr [esi + VXE_bufLen]
        test    ecx, ecx
        jz      ae_stub
        shr     ecx, 2
        mov     edi, [g_v86Lin]
        push    esi                     ; rep movsd 占用 ESI, 存请求块指针
        mov     esi, [esi + VXE_bufLin]
        cld
        rep movsd
        pop     esi
ae_stub:
        ; 当次现取真 V86 IVT[10h] 目标, 写 7 字节桩到 V86 缓冲+768
        VMMCall Get_Cur_VM_Handle
        mov     eax, 10h
        VMMCall Get_V86_Int_Vector      ; CX:DX = 目标(ROM 或反射桩)
        ; 【r51 回退】./50 实锤: 系统 VM 的 IVT[10h] 在 drv 期为 0:0
        ; (INT10 走 IDT 反射链, V86 向量表项闲置清零, Device_Init 期
        ; 读到的 0070:03DC jmpf 跳板才是真身)。cx=0 → 用 Device_Init
        ; 存下的 trampoline; 非 0 (其它机器有真向量) → 照旧现取值。
        test    cx, cx
        jnz     ae_tgt_ok
        mov     cx, [g_romSeg]
        mov     dx, [g_romOff]
ae_tgt_ok:
        ; 【r48 取证】回填本次 callf 目标到 stubLin(该字段输入义已废):
        ; 判 drv 期目标是 BIOS ROM(C000:xxxx) 还是 Win386 V86 反射桩
        ; (低内存, ./47 Device_Init 期读到 0070:03DC) —— 若设备 logo 后
        ; 仍假成功(AX=004F 缓冲不动), 本字段即铁证。
        mov     eax, ecx
        shl     eax, 16
        mov     ax, dx
        mov     [esi + VXE_stubLin], eax
        mov     edi, [g_v86Lin]
        add     edi, VBE_EXEC_STUB_OFF
        mov     byte ptr [edi], 9Ch     ; pushf: 给 BIOS 的 iret 备帧
        mov     byte ptr [edi+1], 9Ah   ; call far 真 V86 IVT[10h] 目标
        mov     [edi+2], dx             ; ip
        mov     [edi+4], cx             ; cs
        mov     byte ptr [edi+6], 0CFh  ; iret: 弹回 Exec_Int 模拟帧
        ; 借道向量指向 VxD 自有桩(改写请求块字段给 ae_borrow_vec)
        mov     ax, [g_v86Seg]
        mov     [esi + VXE_stubSeg], ax
        mov     word ptr [esi + VXE_stubOff], VBE_EXEC_STUB_OFF

        ; --- 借 IVT[vec](写后回读自校验, 序不符自动交换重试) ------------
        call    ae_borrow_vec
        test    eax, eax
        jz      ae_bfail

        ; --- 嵌套 V86 同步执行 -------------------------------------------
        VMMCall Get_Cur_VM_Handle       ; EBX=当前 VM; Begin_Nest/Exec_Int/
                                        ; End_Nest 以 EBX=VM 工作。取句柄
                                        ; 必须用此服务: Curr_VM_Handle 变量
                                        ; 在 VMM.INC 中不导出(A2006 实锤)
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        mov     bx, [esi + VXE_in_ax]
        mov     [ebp].Client_ax, bx
        mov     bx, [esi + VXE_in_bx]
        mov     [ebp].Client_bx, bx
        mov     bx, [esi + VXE_in_cx]
        mov     [ebp].Client_cx, bx
        mov     bx, [esi + VXE_in_dx]
        mov     [ebp].Client_dx, bx
        mov     bx, [esi + VXE_in_si]
        mov     [ebp].Client_si, bx
        mov     bx, [esi + VXE_in_di]
        mov     [ebp].Client_di, bx
        mov     bx, [esi + VXE_in_ds]
        mov     [ebp].Client_ds, bx
        mov     bx, [esi + VXE_in_es]
        mov     [ebp].Client_es, bx
        ; 【r46】有数据中转时, ES:DI 必须指向 VxD V86 中转缓冲
        movzx   eax, word ptr [esi + VXE_bufLen]
        test    eax, eax
        jz      ae_es_done
        mov     ax, [g_v86Seg]
        mov     [ebp].Client_es, ax
        mov     word ptr [ebp].Client_di, 0
ae_es_done:
        movzx   eax, word ptr [esi + VXE_vec]
        VMMCall Exec_Int                ; 同步执行桩(内含对 BIOS 的远调用)
        mov     bx, [ebp].Client_ax
        mov     [esi + VXE_out_ax], bx
        mov     ebx, [ebp].Client_EFlags
        mov     [esi + VXE_out_flags], bx
        ; --- 【r138】把 BIOS 的其余输出寄存器也回填给 ring 3 -----------------
        ;   为什么必须补这一段(详见 vbeshare.h 里 VBEEXECREQ 的尾注):
        ;   决定"画面按什么像素格式扫出来"的那份状态【不在 VGA 寄存器里】,
        ;   只能靠 VBE 自己的读回功能问 —— 4F03h 回 BX=当前模式号,
        ;   4F06h 回 BX=字节/行 CX=像素/行, 4F07h 回 BX/CX=显示起始。
        ;   而旧版请求块只搬 AX 与 flags, 于是"硬件此刻到底在什么模式"
        ;   在 ring 3 侧根本读不到, VerifyDisplayMode 只能拿我们自己的软件
        ;   镜像当判据 —— ./137 里那句 `mode already correct, no 4F02
        ;   issued` 就是这么来的(判据说"对", 用户看到的是条纹)。
        ; 【纯搬寄存器, 不改执行路径】六条 mov 与上面那两条同形: 不新增
        ;   分支、不新增跳转、不动栈。与 r137 那条"只改一条指令"同一条
        ;   纪律 —— 汇编侧改动面越小, 下一轮越不必怀疑它。
        mov     bx, [ebp].Client_bx
        mov     [esi + VXE_out_bx], bx
        mov     bx, [ebp].Client_cx
        mov     [esi + VXE_out_cx], bx
        mov     bx, [ebp].Client_dx
        mov     [esi + VXE_out_dx], bx
        mov     bx, [ebp].Client_si
        mov     [esi + VXE_out_si], bx
        mov     bx, [ebp].Client_di
        mov     [esi + VXE_out_di], bx
        mov     bx, [ebp].Client_es
        mov     [esi + VXE_out_es], bx
        ; r138-marker: api_exec returns BX/CX/DX/SI/DI/ES too (VBE 4F03/4F06/4F07).

        ; --- 拷出(V86 缓冲 → 客户端线性缓冲; 成败都拷: post 状态取证) --
        movzx   ecx, word ptr [esi + VXE_bufLen]
        test    ecx, ecx
        jz      ae_noout
        shr     ecx, 2
        push    esi                     ; 存请求块指针(ae_restore_vec 还要用)
        mov     edi, [esi + VXE_bufLin]
        mov     esi, [g_v86Lin]
        cld
        rep movsd
        pop     esi
ae_noout:
        VMMCall Get_Cur_VM_Handle       ; 读回结果借用了 EBX, 重装给 End_Nest
        VMMCall End_Nest_Exec
        Pop_Client_State

        ; --- 恢复 IVT[vec] ------------------------------------------------
        call    ae_restore_vec

        or      dword ptr [MapLfbStat], VBE_ST_EXEC  ; 迹: 直通执行跑过
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret
ae_nobuf:
        mov     word ptr [ebp].Client_EAX, 90   ; V86 中转缓冲不可用
        VDD_CY
        ret
ae_bfail:
        mov     word ptr [ebp].Client_EAX, 89   ; 向量写后回读校验失败
        VDD_CY
        ret


ae_fail:
        mov     word ptr [ebp].Client_EAX, 87
        VDD_CY
        ret

;-----------------------------------------------------------------------------
; api_noop —— VBE_API_EXEC_NOOP(6): 直通诊断第 1 相(嵌套机制, 不调 BIOS)
;   单字节空桩(CF = 纯 iret)【由客户端在自己 VM 上下文预写】到低内存
;   缓冲 +768; 借 IVT[vec] 走完整 Push/Begin_Nest/Exec_Int/
;   End_Nest/Pop 后恢复。IVT 借用/恢复经 Get/Set_V86_Int_Vector
;   (见 api_exec 头部注释: ./31 实锤原始页 0 在 VxD 全局上下文不可用)。
;   【./34 裁决】本相 halt 元凶 = 旧 2 字节空桩 9C CF 栈失衡: Exec_Int
;   已压 [IP][CS][FLAGS] 模拟帧, pushf 多压 1 字使 iret 弹错槽位。
;   空桩必须只有 CF。本相现验证: Set/恢复向量 + 借道执行全链路。
;-----------------------------------------------------------------------------
api_noop:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        movzx   eax, word ptr [esi + VXE_vec]
        cmp     eax, 40h
        jb      ae_fail

        ; --- 借 IVT[vec](写后回读自校验, 序不符自动交换重试) ------------
        call    ae_borrow_vec
        test    eax, eax
        jz      ae_bfail
        mov     [esi + VXE_in_si], ax   ; 回传所用序(1=常序 2=交换序)

        ; --- 嵌套 V86 同步执行(空桩, 不调任何 BIOS 代码) ---------------
        VMMCall Get_Cur_VM_Handle
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        movzx   eax, word ptr [esi + VXE_vec]
        VMMCall Exec_Int
        mov     bx, [ebp].Client_ax     ; 顺手回填, 验证嵌套后客户区可读
        mov     [esi + VXE_out_ax], bx
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State

        ; --- 恢复 IVT[vec] ------------------------------------------------
        call    ae_restore_vec

        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret

;-----------------------------------------------------------------------------
; api_ivtprobe —— VBE_API_IVT_PROBE(8): V86 IVT 取证探针
;   经 VMMCall Get_V86_Int_Vector 取【真 V86 IVT】向量(VMM 按 VM
;   安全换算; ./31 实锤 VxD 全局上下文线性 0 不是系统 VM 的 V86
;   IVT, 恒等直读只读到 0x40 垃圾值, 不可用):
;     out_ax:off_flags = IVT[10h] —— 真 10h 向量, 假成功层身份判定
;                          (seg 落 C000/E000 等 ROM 段 = 未被钩, 直通
;                           方案可绕过; 落低内存 = VMM 反射桩/V86 过滤)
;     in_bx:in_cx     = IVT[0xC0] —— 借用向量的原状(判定是否干净)
;-----------------------------------------------------------------------------
api_ivtprobe:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        ; --- V86 IVT[10h] ------------------------------------------------
        VMMCall Get_Cur_VM_Handle       ; EBX = 当前 VM
        mov     eax, 10h
        VMMCall Get_V86_Int_Vector      ; CX:DX = seg:off
        mov     [esi + VXE_out_ax], dx  ; off
        mov     [esi + VXE_out_flags], cx ; seg
        ; --- V86 IVT[0xC0](借用向量的原状) ------------------------------
        VMMCall Get_Cur_VM_Handle
        mov     eax, VBE_EXEC_VEC
        VMMCall Get_V86_Int_Vector
        mov     [esi + VXE_in_bx], dx   ; 复用输入字段回传(off)
        mov     [esi + VXE_in_cx], cx   ; (seg)
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret

;-----------------------------------------------------------------------------
; api_vmhprobe —— VBE_API_VM_HANDLE_PROBE(9): int 20h 机制基线探针
;   只调 VMMCall Get_Cur_VM_Handle(服务号由 VMM.INC 解析), 把 EBX=当前
;   VM 句柄回填 out_ax(lo):out_flags(hi)。【裁决意义】./30 实锤手写号
;   0x6C 的 int 20h 调用致死; 本探针用【按名解析】的良性服务验证
;   "API 过程里做 int 20h VMMCall" 机制本身是否健康: 本探针也 halt =
;   机制/上下文问题; 返回 = 机制健康, 手写 0x6C 纯系号码错误。
;-----------------------------------------------------------------------------
api_vmhprobe:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        VMMCall Get_Cur_VM_Handle       ; EBX = 当前 VM 句柄
        mov     [esi + VXE_out_ax], bx
        shr     ebx, 16
        mov     [esi + VXE_out_flags], bx
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret

;-----------------------------------------------------------------------------
; api_nestbase —— VBE_API_NEST_BASE(10): 纯嵌套执行机制基线(不碰 IVT)
;   Begin_Nest + 客户 AX=3000h( int 21h AH=30h 取 DOS 版本, 无害 ) +
;   Exec_Int(21h) + 读回 + End_Nest。【裁决意义】./32 halt 于 NOOP, 该相
;   相比已验证的 IVT 取证新增"Set_V86_Int_Vector"与"嵌套执行块"两个未
;   知量; 本基线单独验证后者: halt 在本相 = Begin_Nest/Exec_Int 机制与
;   16 位 PM 客户端上下文不兼容; 通过而 NOOP halt = 锅在 Set/桩向量。
;-----------------------------------------------------------------------------
api_nestbase:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        VMMCall Get_Cur_VM_Handle
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        mov     word ptr [ebp].Client_ax, 3000h
        mov     eax, 21h
        VMMCall Exec_Int
        mov     bx, [ebp].Client_ax     ; DOS 版本号(无害回显)
        mov     [esi + VXE_out_ax], bx
        mov     ebx, [ebp].Client_EFlags
        mov     [esi + VXE_out_flags], bx
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret

;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; api_getmapinfo —— VBE_API_GET_MAPINFO(11): 回报 V86 中转缓冲与状态位图
;   【r48】回报前先惰性建立 V86 中转缓冲(EnsureV86): drv 在 Enable 期
;   首次调用本 API 即触发分配(嵌套 DOS AH=48h, 只此一次, 之后 g_v86Ok
;   =1 立即返回)。分配从 Device_Init 移到此处的原因见 EnsureV86 头部
;   注释(./47 实锤 Device_Init 期嵌套 DOS 调用失败)。
;   【r76 瘦身】r69-r75 为"VxD 侧自建 LFB 映射"实验加的透出字段
;   (阶梯六档 lad*/ground truth gt*/MMGR 探针/Reserve/PCP/MPTL 原始
;   返回)随该实验一并删除; 现在只回报生产路径上真正有意义的东西:
;     v86Seg/v86Ok  = V86 中转缓冲段/可用(客户端据此决定能否数据中转)
;     stat          = VBE_VXD_STAT_* 里程碑位图
;     step/lastEax  = EnsureV86 状态机步码与失败子码快照
;     vec           = MapLfbStat(与 stat 同源, 兼容旧客户端字段)
;   除 EnsureV86(自身经 Begin_Nest 嵌套, 与 api_exec 同一已验证机制,
;   只调 DOS 的 AH=30h/48h/49h, 不碰任何 MM 服务)外只读锁定段全局量
;   —— PM API 上下文安全。
;-----------------------------------------------------------------------------
api_getmapinfo:
        cmp     ecx, VBE_EXECREQ_SIZE   ; r46: 尾部有 v86Seg/v86Ok 字段,
        jb      api_err2                ; 旧客户端缓冲短 —— 拒写防越界
        call    MiniVDD_EnsureV86       ; r48: 惰性建立中转缓冲(自持 ESI/EDI)
        mov     eax, [MapLfbStat]
        mov     [esi + VXE_stat], ax
        mov     [esi + VXE_vec], ax     ; 兼容槽(旧客户端读这个)
        mov     eax, [InitStep]
        mov     [esi + VXE_step], ax
        mov     eax, [LastEax]
        mov     [esi + VXE_lastEax], eax
        mov     ax, [g_v86Seg]
        mov     [esi + VXE_v86Seg], ax
        mov     eax, [g_v86Ok]
        mov     [esi + VXE_v86Ok], ax
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret

;-----------------------------------------------------------------------------
; api_getmodelist —— VBE_API_GET_MODELIST(12): VxD 侧直读 V86 模式号表
;   【为何存在】./51 实锤: 本机(Voodoo3)VBE BIOS 无视 4F00 预置的
;   VideoModePtr, 模式表只在 BIOS 自己的指针所指处(ROM/低内存) ——
;   那里 16 位 drv 不能读(./45 GP 死机), 而 VxD 线性直读低内存/ROM
;   早已验证(InitMapLfb 取证/触摸测试同机制)。
;   流程: 缓冲就绪保险(EnsureV86) → 读 g_v86Lin+0Eh 的 far 指针
;   (seg:off) → 线性 seg*16+off(只信 <1MB 恒等映射区) → 逐项读模式号
;   (0xFFFF 止, 越界止, 上限 min(bufLen/2,128)) → 整批写客户端 bufLin。
;   出口: out_ax=项数, CF=0; 失败 CF=1 EAX=91(缓冲未就绪/指针异常)。
;   纯线性读写, 无嵌套执行, PM API 上下文安全。
;-----------------------------------------------------------------------------
api_getmodelist:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        call    MiniVDD_EnsureV86       ; 保险(已就绪则立即返回; 自持 ESI/EDI)
        cmp     dword ptr [g_v86Ok], 1
        jne     aml_fail
        movzx   eax, word ptr [esi + VXE_bufLen]
        shr     eax, 1                  ; 每项 2 字节
        test    eax, eax
        jz      aml_fail
        cmp     eax, 128
        jbe     aml_capok
        mov     eax, 128
aml_capok:
        push    edi
        push    ebx
        mov     edi, [g_v86Lin]
        mov     ebx, [edi + 0Eh]        ; VideoModePtr: BX=off, 高字=seg
        mov     ecx, ebx
        shr     ecx, 16                 ; CX = seg
        and     ebx, 0FFFFh             ; BX = off
        shl     ecx, 4
        add     ecx, ebx                ; ECX = 表线性地址
        cmp     ecx, 0F0000h            ; 只信低内存/ROM 阴影恒等映射区
        jae     aml_popfail
        mov     edi, [esi + VXE_bufLin] ; EDI = 客户端输出(线性)
        test    edi, edi
        jz      aml_popfail
        xor     ebx, ebx                ; EBX = 计数
aml_loop:
        cmp     ebx, eax
        jae     aml_done
        cmp     ecx, 0F0000h            ; 表指针越界保护
        jae     aml_done
        movzx   edx, word ptr [ecx]
        cmp     dx, 0FFFFh              ; 表终止
        je      aml_done
        mov     [edi + ebx*2], dx
        inc     ebx
        add     ecx, 2
        jmp     aml_loop
aml_done:
        mov     [esi + VXE_out_ax], bx
        pop     ebx
        pop     edi
        or      dword ptr [MapLfbStat], VBE_ST_MDLIST ; 迹: 模式表已直读
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret
aml_popfail:
        pop     ebx
        pop     edi
aml_fail:
        mov     word ptr [ebp].Client_EAX, 91   ; 模式表不可用
        VDD_CY
        ret

;-----------------------------------------------------------------------------
; api_gettrace(VBE_API_GET_TRACE=13) —— 取回 mini-VDD 分派调用追踪
;   入口: 请求块 bufLin/bufLen
;   出口: out_ax = 实际拷贝字节数; CF=0。失败 CF=1, EAX=92。
;   布局(vbeshare.h 的 VBE_TR_*/VBE_TREC_*, 与 vbeshare.inc 一致):
;       0    dd   累计写入条数(= 下一条序号)
;       4    dd   事件计数器块(VBE_TR_EVT_*: +0 POST_VGA_TO_HIRES, +1 PRE_HIRES_TO_VGA,
;                +2 DISPLAY_DISABLING, +3 PRE_VGA_TO_HIRES,
;                +4/+5 = 【r129】HiResClockFix 动手/挡下次数 —— 下面一次
;                dword 搬运把 [TraceEvt+0..3] 带出, +4/+5 就在同一块里,
;                drv 直接按 VBE_TR_Evt+4/+5 读即可; 该块已声明成 8 字节,
;                见 TraceEvt 处的 r129-marker 说明)
;       8    49×dd 每功能累计调用次数
;       204  4×db 切屏状态块(VBE_TR_State/I10Cnt/I10Ax)
;       208  4×db r117 DISPI 记账块(VBE_TR_Stat2)
;       212  4×db 保留(原 DPHYS 记账区占位)
;       216  4×dw+4×db VBE_DISPI 物理读回块(VBE_TR_DispiPhys)
;       236  64×24 环形记录 { seq, func, flags, pad, ecx, edx, ebx, eax }
;   【为什么整块搬】VxD 与 drv 之间只走"客户端线性缓冲"这一条通道, 少
;   一层解析就少一类错位事故; 排序/去重/限流全交给 drv。
;   纯锁定段读 + 客户端缓冲写, 无嵌套执行, PM API 上下文安全。
;-----------------------------------------------------------------------------
api_gettrace:
        cmp     ecx, VBE_EXECREQ_SIZE
        jb      api_err2
        mov     edi, [esi + VXE_bufLin]         ; EDI = 客户端输出(线性)
        test    edi, edi
        jz      agt_fail
        cmp     word ptr [esi + VXE_bufLen], VBE_TRACE_SIZE
        jb      agt_fail                        ; drv 缓冲必须够大
        push    esi                             ; 保存请求块指针
        push    ebx                             ; 【r100】借用 EBX 存输出基址
        mov     ebx, edi                        ; (出口原样恢复: 它是活寄存器)
        mov     eax, [TraceTotal]
        mov     [edi], eax
        mov     eax, [TraceEvt]                 ; 【r100】4 个事件计数器原样带出
        mov     [edi+4], eax
        add     edi, VBE_TR_COUNTS
        mov     esi, OFFSET32 TraceCounts
        mov     ecx, VBE_TRACE_FUNCS * 4
agt_c1:
        mov     al, [esi]
        mov     [edi], al
        inc     esi
        inc     edi
        dec     ecx
        jnz     agt_c1
        ; 【r101】此刻 EDI 正好 = 输出基址 + VBE_TR_Counts + 49*4 = base+204,
        ;   也就是计数区之后那 4 字节空隙 —— 把切屏状态块整块放进去
        ;   (VBE_TR_State/I10Cnt/I10Ax 三个变量在数据区里是连续的 4 字节)。
        mov     eax, dword ptr [TraceState]
        mov     [edi], eax
        ; 【r117】紧接着的 4 字节 = DISPI 记账块 VBE_TR_Stat2(208..211):
        ;   +0 DispiInstalled(bit0=1CE,bit1=1CF) / +1 DispiTrapOn /
        ;   +2 DispiActs / +3 DispiErrs —— 数据区里 DispiStat2 是 label,
        ;   四个字节物理连续(见那里的声明)。
        mov     eax, dword ptr [DispiStat2]
        mov     [edi+4], eax
        ; 【r143】紧接着的 4 字节 = 文本取指取证块 VBE_TR_VgaS(212..215):
        ;   四个字节物理连续(见 VgaS1Sr04 处的声明)。edi 此刻仍 = base+204,
        ;   所以 edi+8 正好是 212; 而下面 DispiPhys 从 edi+12(=216) 开始。
        mov     eax, dword ptr [FboxWord]       ; 【r144】四个计数器
        mov     [edi+8], eax                    ;   整块到 blob 212..215
        ; 【r120】紧接着的 24 字节 = ring 0 物理读回的 DISPI 寄存器块。
        ;   【偏移必须算准】edi 此刻 = base+204(计数区之后), 而
        ;   VBE_TR_DispiPhys = 216 ⇒ 块内 +0(ID) 落在 edi+12。
        ;   数据区里 DispiPhysVal(12 字 = 24 字节) 的 +0/hi 就是 ID,
        ;   +0..+11 六个字依次为 ID/XRES/YRES/BPP/ENABLE/VIRTW,
        ;   +12..+14 三个记账字节(DispiPhysN/Ok/Errs 物理连续),
        ;   +15 保留。
        mov     eax, dword ptr [DispiPhysVal]
        mov     [edi+12], eax
        mov     eax, dword ptr [DispiPhysVal + 4]
        mov     [edi+16], eax
        mov     eax, dword ptr [DispiPhysVal + 8]
        mov     [edi+20], eax
        mov     eax, dword ptr [DispiPhysVal + 12]
        mov     [edi+24], eax                   ; +8 ENABLE(低字) + +10 VIRTW
                                                ; +12 读次数 + +13 命中
        ; 【r146】持屏期/切屏期硬件指纹 16 字节 → blob 232..247。
        ;   edi 此刻仍 = base+204, 所以 edi+28 = 232。
        ;   (r144 只搬 4 字节, 落在 DispiPhysVal 尾部那四个"保留 0"上;
        ;    r146 头部正式长到 256, 记录区随之从 236 挪到 256。)
        mov     eax, dword ptr [FboxSnapVal]
        mov     [edi+28], eax
        mov     eax, dword ptr [FboxSnapVal + 4]
        mov     [edi+32], eax
        mov     eax, dword ptr [FboxSnapVal + 8]
        mov     [edi+36], eax
        mov     eax, dword ptr [FboxSnapVal + 12]
        mov     [edi+40], eax
        ; 【r146】hi-res 快照/回放记账 8 字节 → blob 248..255。
        mov     eax, dword ptr [HiresValid]     ; valid/savedN/restN/diff 连续
        mov     [edi+44], eax
        mov     eax, dword ptr [HiresSpare]
        mov     [edi+48], eax
        ; 【r147】六个采集槽 96 字节 → blob 256..351(VBE_TR_PhaseSnap)。
        ;   【为什么用循环而不是 24 条 mov】24 条 mov 会让本过程再长约
        ;   100 字节, 而这里已经有两条手写块搬; 96 字节 = 6 槽 × 16,
        ;   槽内布局与 VBE_TR_FS_* 一致。EDI 直接从头算起(不再依赖
        ;   "edi 恰好 = base+204"这条小算式), 少一类错位事故。
        mov     edi, ebx
        add     edi, VBE_TR_PhaseSnap
        mov     esi, OFFSET32 PhaseSnap
        mov     ecx, VBE_TR_PS_SLOTS * VBE_TR_PS_LEN
agt_c3:
        mov     al, [esi]
        mov     [edi], al
        inc     esi
        inc     edi
        dec     ecx
        jnz     agt_c3
        ; 【r147】补写记账 16 字节 → blob 352..367(VBE_TR_MemC)。
        ;   循环结束时 edi 正好 = base+352, 所以这四条 mov 不用再算偏移。
        mov     eax, dword ptr [MemC]
        mov     [edi], eax
        mov     eax, dword ptr [MemC + 4]
        mov     [edi + 4], eax
        mov     eax, dword ptr [MemC + 8]
        mov     [edi + 8], eax
        mov     eax, dword ptr [MemC + 12]
        mov     [edi + 12], eax
        ; 【r151】所有权/特殊 VM 记账 24 字节 → blob 368..391
        ;   (VBE_TR_Own)。【偏移自己算】不依赖"edi 恰好 = base+368"。
        mov     edi, ebx
        add     edi, VBE_TR_Own
        mov     eax, dword ptr [OwnWord]
        mov     [edi], eax
        mov     eax, dword ptr [OwnWord + 4]
        mov     [edi + 4], eax
        mov     eax, dword ptr [OwnWord + 8]
        mov     [edi + 8], eax
        mov     eax, dword ptr [OwnWord + 12]
        mov     [edi + 12], eax
        mov     eax, dword ptr [OwnWord + 16]
        mov     [edi + 16], eax
        mov     eax, dword ptr [OwnWord + 20]
        mov     [edi + 20], eax
        ; 【r153】取证块 248 字节 -> blob 392..639(VBE_TR_Probe)。
        ;   【偏移自己算】和上面几块一样, 不依赖 edi 的残余值。
        ;   【为什么用 rep movsd 而不是 62 条 mov】62 条 mov 会让这一段
        ;   超过 MASM 的行数/分支距离舒适区, 而 rep movsd 只需要 ESI/EDI/
        ;   ECX 三个寄存器(本过程尾部本来就在做别的搬运)。方向标志在
        ;   VxD 里恒为"向上"(cld 是 VxD 入口纪律的一部分), 但这里仍然
        ;   显式 cld 一次, 免得将来有人在上面加一条 std。
        mov     edi, ebx
        add     edi, VBE_TR_Probe
        mov     esi, OFFSET32 Probe
        mov     ecx, VBE_TR_PR_LEN / 4
        cld
        rep     movsd
        mov     edi, ebx                        ; 【D2 修正】记录区固定从
        add     edi, VBE_TR_Recs                ; VBE_TR_Recs(=424) 开始,
                                                ; 不是紧随计数之后的 204
        mov     esi, OFFSET32 TraceRecs
        mov     ecx, VBE_TRACE_RECS * VBE_TRACE_RECB
agt_c2:
        mov     al, [esi]
        mov     [edi], al
        inc     esi
        inc     edi
        dec     ecx
        jnz     agt_c2
        pop     ebx
        pop     esi
        mov     word ptr [esi + VXE_out_ax], VBE_TRACE_SIZE
        mov     word ptr [ebp].Client_EAX, 0
        VDD_NC
        ret
agt_fail:
        mov     word ptr [ebp].Client_EAX, 92   ; 追踪缓冲不可用/太小
        VDD_CY
        ret

;-----------------------------------------------------------------------------
; ae_borrow_vec —— 借用 IVT[vec]: 存原向量 → 置桩 → 写后回读自校验
;   (回读不符则自动交换 seg/off 序重试一次; 两序皆败则尽力恢复原向量
;   并返回 0)。返回 EAX = 1(常序 CX=seg,DX=off 成功) / 2(交换序成功)
;   / 0(失败, 已尽力恢复)。入口 ESI=请求块; 依服务约定破坏 EAX/EBX/
;   CX/DX, 保持 ESI/EBP。【为何回读校验】./32 首跑 Set_V86_Int_Vector
;   后 halt; 若 seg/off 序与 VMM 约定相反, 向量被写成垃圾, V86 跳入
;   即死 —— 回读校验把"写错向量"从"嵌套机制故障"里摘出来并自愈。
;-----------------------------------------------------------------------------
ae_borrow_vec:
        VMMCall Get_Cur_VM_Handle
        movzx   eax, word ptr [esi + VXE_vec]
        VMMCall Get_V86_Int_Vector      ; CX:DX = 原 IVT[vec]
        mov     [g_ivtOrigSeg], cx
        mov     [g_ivtOrigOff], dx
        ; 常序: CX=seg DX=off
        VMMCall Get_Cur_VM_Handle
        movzx   eax, word ptr [esi + VXE_vec]
        mov     cx, [esi + VXE_stubSeg]
        mov     dx, [esi + VXE_stubOff]
        VMMCall Set_V86_Int_Vector
        VMMCall Get_Cur_VM_Handle
        movzx   eax, word ptr [esi + VXE_vec]
        VMMCall Get_V86_Int_Vector
        cmp     cx, [esi + VXE_stubSeg]
        jne     abv_swap
        cmp     dx, [esi + VXE_stubOff]
        jne     abv_swap
        mov     eax, 1
        ret
abv_swap:
        ; 交换序: CX=off DX=seg
        VMMCall Get_Cur_VM_Handle
        movzx   eax, word ptr [esi + VXE_vec]
        mov     cx, [esi + VXE_stubOff]
        mov     dx, [esi + VXE_stubSeg]
        VMMCall Set_V86_Int_Vector
        VMMCall Get_Cur_VM_Handle
        movzx   eax, word ptr [esi + VXE_vec]
        VMMCall Get_V86_Int_Vector
        cmp     cx, [esi + VXE_stubOff]
        jne     abv_fail
        cmp     dx, [esi + VXE_stubSeg]
        jne     abv_fail
        mov     eax, 2
        ret
abv_fail:
        VMMCall Get_Cur_VM_Handle       ; 尽力恢复原向量后报告失败
        movzx   eax, word ptr [esi + VXE_vec]
        mov     cx, [g_ivtOrigSeg]
        mov     dx, [g_ivtOrigOff]
        VMMCall Set_V86_Int_Vector
        xor     eax, eax
        ret

;-----------------------------------------------------------------------------
; ae_restore_vec —— 恢复 IVT[vec] 为借用前的原向量
;-----------------------------------------------------------------------------
ae_restore_vec:
        VMMCall Get_Cur_VM_Handle
        movzx   eax, word ptr [esi + VXE_vec]
        mov     cx, [g_ivtOrigSeg]
        mov     dx, [g_ivtOrigOff]
        VMMCall Set_V86_Int_Vector
        ret

;-----------------------------------------------------------------------------
; MiniVDD_EnsureV86 —— 惰性建立 V86 中转缓冲(drv 期首次需要时)
;   【r48 为何从 Device_Init 移到 drv 期】./47 实锤: 本机(全新 Win98)
;   Device_Init(PNP 装载, 系统 VM 正忙于实模式引导)期嵌套 Exec_Int(21h)
;   AH=48h 失败(InitStep=103) —— DOS 引导期未就绪是最大嫌疑;
;   而 drv 期 PM API 上下文嵌套执行是 api_exec 的既有生产机制。r48 移
;   后 ./48 实锤: 嵌套机制本机首通 —— DOS 正经应答 CF=1 AX=8(常规内存
;   不足, 64/40/33 段三连败)。故 r49 加"DOS 对话阶梯"定位拒绝原因:
;     探针A: AH=30h 取 DOS 版本(永不成 CF; AX=0 = 无应答 → 步码 106)
;            → LastEax 低字 = 版本 AX
;     探针B: AH=48h BX=1(16B) 成功随即 AH=49h 归还 —— 区分"arena 可
;            分配"与"全盘无内存"
;     正式:  AH=48h 64→40→33 段; 末败 LastEax=0xCF000000|err(DOS 错误码)
;            (DOS 规范: 48h 失败时 BX 回报最大空闲块段数 —— r76 起只作
;             现场信息, 不再单独留槽透出)
;   【r50 物理暂存回退】./49 裁决: dosv=070A(真 DOS 7.10) 但 b16ok=0
;   且 freepar=0 —— 系统 VM 的 DOS arena 全满, AH=48h 死路(47/48/49
;   三轮实锤)。故任何嵌套内失败 → 回退 0xB0000 单色帧缓冲暂存区(彩色
;   系统恒为闲置 RAM, 恒等地址可达, 触摸测试把关; 详见 ev_try_b0000
;   头注)。B0000 也失败才落 103。
;   成功: g_v86Ok=1, InitStep=4(DOS 版)/5(B0000 版), LastEax=分得段,
;         MapLfbStat 置 VBE_ST_DOSBUF / VBE_ST_B0000 记路径。
;   其它失败子码: 0x0A0A0000=AX=0; 0xF00000|ax=段越界(嵌套未生效迹象,
;   预置 AX=4800h 原样返回也落此); 0x7E7E0000|读回=触摸不符。
;   失败不置 g_v86Ok, 下次调用自动重试。
;   寄存器: 自持 ESI/EDI(压栈保存), 用 EAX/EBX/ECX/EDX/EBP(=嵌套客户
;   帧, 与调用方同值, 出口保持)。不借用任何 IVT 向量(纯 DOS 调用)。
;-----------------------------------------------------------------------------
MiniVDD_EnsureV86:
        cmp     dword ptr [g_v86Ok], 1
        je      ev_ok_ret               ; 已就绪: 立即返回(未压栈)
        push    esi
        push    edi
        ; --- VM 基线 + 客户帧 ------------------------------------------
        VMMCall Get_Cur_VM_Handle
        test    ebx, ebx
        jz      ev_fail_novm
        mov     ebp, [ebx + CB_Client_Pointer]
        ; --- 进入嵌套 ----------------------------------------------------
        VMMCall Get_Cur_VM_Handle
        Push_Client_State
        VMMCall Begin_Nest_V86_Exec
        ; --- 探针 A: AH=30h 取 DOS 版本 ----------------------------------
        mov     word ptr [ebp].Client_ax, 3000h
        VMMCall Get_Cur_VM_Handle
        mov     eax, 21h
        VMMCall Exec_Int
        movzx   ebx, word ptr [ebp].Client_ax
        test    bx, bx
        jz      ev_fail_dosver
        mov     [LastEax], ebx         ; 取证: DOS 版本(如 070Ah)在低字
        ; --- 探针 B: AH=48h BX=1(16B 最小分配) ---------------------------
        mov     word ptr [ebp].Client_ax, 4800h
        mov     word ptr [ebp].Client_bx, 1
        VMMCall Get_Cur_VM_Handle
        mov     eax, 21h
        VMMCall Exec_Int
        mov     ebx, [ebp].Client_EFlags
        test    bl, 01h
        jnz     ev_realtry              ; 16B 也失败 → 仍走正式(留 BX 证据)
        mov     ax, [ebp].Client_ax
        test    ax, ax
        jz      ev_realtry
        cmp     ax, 0F00h
        jae     ev_realtry
        mov     [ebp].Client_es, ax         ; 随即归还(AH=49h, ES=分得段)
        mov     word ptr [ebp].Client_ax, 4900h
        VMMCall Get_Cur_VM_Handle
        mov     eax, 21h
        VMMCall Exec_Int
ev_realtry:
        ; --- 正式尝试 AH=48h 64→40→33 段 ---------------------------------
        mov     word ptr [ebp].Client_ax, 4800h
        mov     word ptr [ebp].Client_bx, 64
        VMMCall Get_Cur_VM_Handle
        mov     eax, 21h
        VMMCall Exec_Int
        mov     ebx, [ebp].Client_EFlags
        test    bl, 01h                 ; CF=1 → 试更小
        jz      ev_alloc_ok
        mov     word ptr [ebp].Client_ax, 4800h
        mov     word ptr [ebp].Client_bx, 40
        VMMCall Get_Cur_VM_Handle
        mov     eax, 21h
        VMMCall Exec_Int
        mov     ebx, [ebp].Client_EFlags
        test    bl, 01h
        jz      ev_alloc_ok
        mov     word ptr [ebp].Client_ax, 4800h
        mov     word ptr [ebp].Client_bx, 33
        VMMCall Get_Cur_VM_Handle
        mov     eax, 21h
        VMMCall Exec_Int
        mov     ebx, [ebp].Client_EFlags
        test    bl, 01h
        jnz     ev_fail_cf
ev_alloc_ok:
        mov     ax, [ebp].Client_ax
        test    ax, ax
        jz      ev_fail_ax0
        cmp     ax, 0F00h               ; 段越界(常规 AH=48h 不可能返 UMA)
        jae     ev_fail_range
        mov     [g_v86Seg], ax
        mov     word ptr [g_v86Off], 0
        movzx   esi, ax
        shl     esi, 4                  ; ESI = 线性 = seg*16
        mov     [g_v86Lin], esi
        mov     eax, 12345678h          ; 触摸测试: VxD 侧可见可写
        mov     [esi], eax
        cmp     [esi], eax
        jne     ev_fail_touch
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State
        pop     edi
        pop     esi
        mov     dword ptr [g_v86Ok], 1
        mov     dword ptr [InitStep], 4
        movzx   eax, word ptr [g_v86Seg]
        mov     [LastEax], eax
        or      dword ptr [MapLfbStat], VBE_ST_DOSBUF ; 迹: DOS 缓冲就绪
ev_stat_done:
        mov     eax, 1
        ret
ev_ok_ret:
        mov     eax, 1
        ret
ev_fail_dosver:                         ; 探针 A 失败: DOS 无应答
        mov     eax, [ebp].Client_EFlags
        and     eax, 0FFFFh
        shl     eax, 16
        mov     ax, [ebp].Client_ax
        mov     [LastEax], eax          ; hi=flags lo=ax 现场快照
        jmp     ev_try_b0000
ev_fail_cf:                             ; DOS CF=1: err=Client_ax
        movzx   eax, word ptr [ebp].Client_ax
        or      eax, 0CF000000h
        mov     [LastEax], eax
        jmp     ev_try_b0000
ev_fail_ax0:
        mov     dword ptr [LastEax], 0A0A0000h
        jmp     ev_try_b0000
ev_fail_range:                          ; AX>=0xF00: 越界/未生效
        movzx   eax, word ptr [ebp].Client_ax
        or      eax, 0F0000000h
        mov     [LastEax], eax
        jmp     ev_try_b0000
ev_fail_touch:                          ; DOS 缓冲触摸读回不符
        mov     eax, [esi]
        and     eax, 0FFFFh
        or      eax, 07E7E0000h
        mov     [LastEax], eax
        ; fall through: DOS 缓冲不可用, 试物理暂存回退
;-----------------------------------------------------------------------------
; ev_try_b0000 —— 0xB0000 单色帧缓冲物理暂存回退(./49 裁决)
;   DOS arena 在系统 VM 上下文全满(b16ok=0 且 freepar=0), AH=48h 死路。
;   0xB0000-0xB7FFF 是单色适配器帧缓冲, 彩色系统(能跑 VBE 高彩的必然
;   是彩色系统)上恒为闲置 RAM: DOS 不管理(无 MCB), VDD 仅为 VGA 兼容
;   映射该区间, 非 EMM386 默认帧页; VxD/V86 恒等地址均可直达。VxD 侧
;   触摸测试(写读回)把关后启用; 缓冲布局与 DOS 版相同(0-511 VbeInfo,
;   512-767 ModeInfo, 768 桩)。MapLfbStat=4 标记此路径。
;-----------------------------------------------------------------------------
ev_try_b0000:
        movzx   eax, word ptr [ebp].Client_ax   ; 末次 DOS 返回值(取证)
        or      eax, 0B0B00000h
        mov     [LastEax], eax
        mov     word ptr [g_v86Seg], 0B000h
        mov     word ptr [g_v86Off], 0
        mov     dword ptr [g_v86Lin], 0B0000h
        mov     esi, 0B0000h
        mov     eax, 12345678h          ; 触摸测试: 写读回
        mov     [esi], eax
        cmp     [esi], eax
        jne     ev_fail_b0000
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State
        pop     edi
        pop     esi
        mov     dword ptr [g_v86Ok], 1
        or      dword ptr [MapLfbStat], VBE_ST_B0000 ; 迹: B0000 回退
        mov     dword ptr [InitStep], 5
        mov     eax, 1
        ret
ev_fail_b0000:                          ; 连 B0000 都摸不通: 记 103
        mov     dword ptr [LastEax], 0B0B0AAAAh
        mov     dword ptr [InitStep], 103
ev_fail_common:                         ; 嵌套收尾(步码各失败路径已记)
        VMMCall Get_Cur_VM_Handle
        VMMCall End_Nest_Exec
        Pop_Client_State
        pop     edi
        pop     esi
        xor     eax, eax
        ret
ev_fail_novm:                           ; 无当前 VM(未嵌套, 无收尾)
        mov     dword ptr [LastEax], 0
        pop     edi
        pop     esi
        mov     dword ptr [InitStep], 101
        xor     eax, eax
        ret

EndProc MiniVDD_API_Common

;=============================================================================
; 【r76 删除说明】本处原为 r53-r75 的"VxD 侧自建 LFB 映射"实验代码:
;   MiniVDD_FindLfb(PCI BAR 自枚举) / MiniVDD_MapLfb(_PageReserve +
;   _PageCommitPhys 阶梯) / MiniVDD_MapLfbSel(_MapPhysToLinear +
;   _BuildDescriptorDWORDs + _Allocate_GDT_Selector 建 GDT 选择子) /
;   MiniVDD_MapLfbPa(PageAllocate 路径) / MiniVDD_IdleMap(VM 事件重试)。
; 该路线已被实机否决: 造出来的选择子 ring 3 载入即 #GP(蓝屏 0D
; 031F:00090B), 因为描述符 DPL 由 VMM 按"保护模式应用"的 RPL 填,
; 且其基址落在 ring 3 不可访问的 supervisor 系统区 —— 详见 vbeshare.h
; 与 display.c 文件头。帧缓冲映射现在完全由 display.drv 在 ring 3 用
; DPMI 0800h 完成, 本 VxD 只代它把选择子登记给主 VDD(见 ApplyState)。
; 相关全局量、服务调用与诊断字段已一并清除, 勿再参考旧版注释。
;=============================================================================
;=============================================================================
; W32_DEVICEIOCONTROL —— Win32 程序 DeviceIoControl 通道
; 入口: ESI → DIOCParams
; 出口: EAX = 0 成功；非 0 = Win32 错误码
;
; 【为何 lpvInBuffer 可直接访问】
;   该控制消息在调用方线程的 VM 上下文中分发，lpvInBuffer 是调用
;   进程的平展地址，此刻正好映射——无需额外锁定/映射。
; 【DIOC_OPEN 为何必须返回 0】
;   CreateFile("\\\\.\\MINIVDD.VXD") 会触发 DIOC_OPEN；返回非 0
;   会导致 CreateFile 失败，调用方连设备都打不开。
;=============================================================================
BeginProc MiniVDD_W32IOC

        mov     eax, [esi].dwIoControlCode
        cmp     eax, DIOC_OPEN
        je      short ioc_ok
        cmp     eax, DIOC_CLOSEHANDLE
        je      short ioc_ok
        cmp     eax, VBE_VXD_IOCTL_PUSH_STATE
        je      short ioc_push
        cmp     eax, VBE_VXD_IOCTL_GET_STATE
        je      short ioc_get
        mov     eax, 1                          ; ERROR_INVALID_FUNCTION
        ret

ioc_ok:
        xor     eax, eax
        ret

ioc_push:
        mov     edx, [esi].lpvInBuffer
        mov     ecx, [esi].cbInBuffer
        call    MiniVDD_ApplyState              ; 返回 EAX=0 成功
        ret

ioc_get:
        cmp     [StateValid], 0
        je      short ioc_notready
        mov     ecx, [esi].cbOutBuffer
        cmp     ecx, VBE_STATE_SIZE
        jb      short ioc_bad
        mov     edi, [esi].lpvOutBuffer
        push    esi                             ; 保留 DIOCParams 指针
        mov     esi, OFFSET32 VbeState
        mov     ecx, VBE_STATE_SIZE/4
        cld
        rep     movsd
        pop     esi
        mov     eax, [esi].lpcbBytesReturned
        test    eax, eax
        jz      short ioc_ok
        mov     dword ptr [eax], VBE_STATE_SIZE
        xor     eax, eax
        ret

ioc_notready:
        mov     eax, 21                         ; ERROR_NOT_READY
        ret
ioc_bad:
        mov     eax, 87                         ; ERROR_INVALID_PARAMETER
        ret

EndProc MiniVDD_W32IOC

;=============================================================================
; MiniVDD_ApplyState —— 接收并应用共享状态(IOCTL 与 API 共用)
; 入口: EDX = 源线性地址(当前 VM 上下文), ECX = 源字节数
; 出口: EAX = 0 成功 / 87 参数错误
;
; 处理步骤：校验长度与签名 → 拷入锁定数据段 → 防御性修正 →
;          【r76】代 drv 登记它的屏选择子 → 置有效标志。
; 【为何先验签名】ring 3 传错缓冲区是最高发的事故；一个魔数校验
; 能挡住绝大多数错位数据，代价几乎为零。
; 【防御性修正的依据】全部来自 VBE 规范本身，与任何具体卡无关：
; 规范规定 WinGranularity 是 bank 号换算单位，但个别 BIOS 对"不可
; 再分"的窗口返回 0；A000 窗口物理上不可能超过 64KB。这两种异常
; 值若原样交给主 VDD，都会被换算成错误的 bank 号 —— 直接花屏。
; 统一钳到 64 是规范内唯一合理的解释。
; 【r76: 屏选择子登记】lfbSelector 由 display.drv 在 ring 3 用 DPMI
; 自建(DPL=3, ring 3 可载), 这里【不再清零、也不再自己映射】, 而是
; 把它转交主 VDD:
;     VDD_Register_Extra_Screen_Selector(EAX = 选择子)
; DDK 明确要求这一步由 mini-VDD 代显示驱动做, 且每个会话只登记一次
; ("It should only be registered once per Windows session"), 否则
; 主 VDD 的额外选择子数组(上限 8)会被反复占用。主 VDD 拿到它之后,
; 在窗口化 VGA 4 平面虚拟化时才能把 MemC 状态切回 Windows VM 需要的
; 值 —— 不登记就可能出现"DOS 图形程序退出后桌面花屏"。
; 该服务"除 EBX 外全部寄存器都会被破坏"(DDK 原文), 而本过程被
; api_push 调用时正持有 ESI(客户端缓冲线性地址)/EDX/ECX 与 EBP
; (客户端寄存器帧), 故调用前后必须整体保护 —— 少存一个就是又一次
; 随机蓝屏。
;=============================================================================
BeginProc MiniVDD_ApplyState

        cmp     ecx, VBE_STATE_SIZE
        jb      as_fail
        mov     esi, edx
        cmp     dword ptr [esi].VBS_signature, VBESTATE_SIG
        jne     as_fail
        ; structSize 二次校验：结构将来演进时，比本 VxD 还老的客户端
        ; 会被拒绝，而不是被读出一份错位数据
        cmp     dword ptr [esi].VBS_structSize, VBE_STATE_SIZE
        jb      as_fail

        mov     edi, OFFSET32 VbeState
        mov     ecx, VBE_STATE_SIZE/4
        cld
        rep     movsd

        ; --- WinGranularity 钳制到 (0, 64] --------------------------
        cmp     byte ptr [VbeState.VBS_bankSizeKB], 0
        jne     short as_gran_hi
        mov     byte ptr [VbeState.VBS_bankSizeKB], 64
as_gran_hi:
        cmp     byte ptr [VbeState.VBS_bankSizeKB], 64
        jbe     short as_gran_ok
        mov     byte ptr [VbeState.VBS_bankSizeKB], 64
as_gran_ok:

        ; --- 代 drv 登记屏选择子(仅首次) ------------------------------
        movzx   eax, word ptr [VbeState.VBS_lfbSelector]
        test    eax, eax
        jz      short as_done                   ; drv 还没建/建失败: 不登记
        cmp     dword ptr [SelRegistered], 0
        jne     short as_done                   ; 一个会话只登记一次
        mov     dword ptr [SelRegistered], 1
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        movzx   eax, word ptr [VbeState.VBS_lfbSelector]
        VxDcall VDD_Register_Extra_Screen_Selector  ; EAX=选择子
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        or      dword ptr [MapLfbStat], VBE_ST_SELREG

as_done:
        mov     [StateValid], 1
        xor     eax, eax
        ret

as_fail:
        mov     eax, 87
        ret

EndProc MiniVDD_ApplyState

;=============================================================================
; SYSTEM_EXIT / SYS_DYNAMIC_DEVICE_EXIT —— 清理
; 把覆盖过的分派表项还给主 VDD(动态卸载时必须，否则主 VDD 会跳进
; 已卸载的代码)。
; 【r117 新增第二项】端口陷阱必须在离开之前【关掉】: r116 起本 VxD 在
;   1CEh/1CFh 上装了 I/O handler(而这个 VxD 是"Dynamic load device
;   minivdd.vxd", 随时可能被卸掉)。陷阱若还开着, 卸载后第一次有人碰
;   这两个端口, VMM 就会跳进一块已经不存在的代码 —— 这正是"不可预测的
;   0D/冻结"最经典的一种成因。MiniVDD_DispiTrapOff 会被策略/装机位短路,
;   所以这里直接按 VBE_TR_ST2_INST 记账自己关(装过才关, 关两次无害)。
;   注: 我们【不】调 Remove_IO_Handler —— 参考实现(boxvmini.asm)没有这一步,
;   而 VMM 会随 VxD 卸载回收其登记的 handler; 多一次未知签名的服务调用
;   反而是新增风险。
;=============================================================================
BeginProc MiniVDD_SystemExit

        call    MiniVDD_DispiTrapShutdown
        call    MiniVDD_RestoreDispatch
        cmp     dword ptr [WinVMHandle], 0      ; [r412] 首个 EBX = Windows VM
        jne     short CAP398_done
        test    ebx, ebx
        jz      short CAP398_done
        mov     dword ptr [WinVMHandle], ebx
CAP398_done:
        clc
        ret

EndProc MiniVDD_SystemExit

BeginProc MiniVDD_DynamicExit

        call    MiniVDD_DispiTrapShutdown
        call    MiniVDD_RestoreDispatch
        clc
        ret

EndProc MiniVDD_DynamicExit

;-----------------------------------------------------------------------------
; MiniVDD_DispiTrapShutdown —— 卸载前无条件关掉两个端口的全局陷阱
;   与 MiniVDD_DispiTrapOff 的区别: 不受策略字节与 DispiInstalled==3 短路,
;   只看"到底装过没有"(DispiInstalled 非 0)。VM 句柄/策略在这时都可能
;   已经不可信, 而"关陷阱"这个动作本身在任何情况下都是安全的。
;   保存/恢复全部寄存器: VMM 服务会破坏寄存器。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_DispiTrapShutdown
        pushfd
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        cmp     byte ptr [DispiInstalled], 0
        je      short dsd_out
        mov     edx, 01CEh
        VMMcall Disable_Global_Trapping
        mov     edx, 01CFh
        VMMcall Disable_Global_Trapping
        mov     byte ptr [DispiTrapOn], 0
        and     byte ptr [TraceState], 0EFh
dsd_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        ret
EndProc MiniVDD_DispiTrapShutdown

;-----------------------------------------------------------------------------
; MiniVDD_RestoreDispatch —— 按注册清单把原表项写回主 VDD 分派表
;-----------------------------------------------------------------------------
BeginProc MiniVDD_RestoreDispatch

        push    esi
        push    edi
        mov     edi, [MiniDispatchTable]
        test    edi, edi
        jz      short rd_done
        mov     esi, OFFSET32 RegList
        cld
rd_loop:
        lodsd                                   ; EAX = 功能索引
        cmp     eax, -1
        je      short rd_done
        mov     edx, eax
        lodsd                                   ; 跳过处理函数地址
        mov     ecx, [OldDispatch + edx*4]
        mov     [edi + edx*4], ecx
        jmp     short rd_loop
rd_done:
        mov     [MiniDispatchTable], 0
        pop     edi
        pop     esi
        ret

EndProc MiniVDD_RestoreDispatch

VxD_LOCKED_CODE_ENDS

;=============================================================================
; 初始化代码段(初始化完成后被丢弃，因此只能放初始化路径用到的代码)
;=============================================================================
VxD_ICODE_SEG

;-----------------------------------------------------------------------------
; Sys_Critical_Init —— 此时中断关闭、很多服务不可用，无事可做
;-----------------------------------------------------------------------------
BeginProc MiniVDD_SysCriticalInit

        clc
        ret

EndProc MiniVDD_SysCriticalInit

;-----------------------------------------------------------------------------
; Device_Init(兼作 SYS_DYNAMIC_DEVICE_INIT)
; 取得主 VDD 的 mini-VDD 分派表并登记我们要覆盖的 5 个表项。
; 【v2 不再做的事】不枚举 PCI、不查卡表、不装端口陷阱 —— 通用 VBE
; 驱动没有任何卡专用的初始化动作。
; 出口: CF=0 成功；CF=1 失败(表项数不足 49 说明主 VDD 过旧，
;       拒绝加载比带病运行安全)
;-----------------------------------------------------------------------------
BeginProc MiniVDD_DeviceInit

        push    ebx
        push    esi
        push    edi

        ; VDD_Get_Mini_Dispatch_Table 返回 EDI=表地址, ECX=表项数
        VxDcall VDD_Get_Mini_Dispatch_Table
        cmp     ecx, NBR_MINI_VDD_FUNCTIONS
        ; r71 修复: di_fail 因插入 PageAllocate 探针已超 rel8 范围
        ; (A2075 by 33), 改"反向短跳+近跳转"显式结构, 不依赖 MASM
        ; 自动扩展; +5 字节内无其它跨区短跳, 无连锁影响
        jae     short di_table_ok
        jmp     di_fail
di_table_ok:
        mov     [MiniDispatchTable], edi

        ; 遍历注册清单：先备份原表项(卸载时恢复)，再换上我们的函数
        mov     esi, OFFSET32 RegList
        cld
di_regloop:
        lodsd                                   ; EAX = 功能索引
        cmp     eax, -1
        je      short di_regdone
        mov     edx, eax
        lodsd                                   ; EAX = 处理函数地址
        mov     ecx, [edi + edx*4]
        mov     [OldDispatch + edx*4], ecx
        mov     [edi + edx*4], eax
        jmp     short di_regloop
di_regdone:
        or      dword ptr [MapLfbStat], VBE_ST_HOOKED  ; 迹: 49 项已挂上
                                                       ; (5 真实 + 44 追踪桩)

        ; --- r116/r117: 装 VBE_DISPI(1CEh/1CFh)的 I/O handler -----------------
        ; 【为什么装在这里】handler 必须在任何 VM 碰这两个端口之前就位;
        ;   而"装 handler"本身不改任何行为 —— 装完立刻 Disable_Global_Trapping,
        ;   只有 5/13 号把陷阱打开之后它才可能被调用。
        ; 【r117 修正 —— 端口号在 EDX】DDK 原文(Install_IO_Handler):
        ;       mov     esi, IOCallback     ; handler 地址
        ;       mov     edx, Port           ; I/O 端口号
        ;       VMMCall Install_IO_Handler
        ;       jc      not_installed
        ;   参考实现 boxvmini.asm 逐字如此(每个端口一对
        ;   Install_IO_Handler + Disable_Global_Trapping —— 因为
        ;   Install_IO_Handler 会【顺手把该端口的陷阱打开】)。
        ;   r116 把端口写进了 EAX, EDX 里残留的是注册循环最后一个 RegList
        ;   索引(48 = 30h) ⇒ 第一次调用把 handler 装到了端口 30h 上, 第二次
        ;   调用撞上"该端口已有 handler"⇒ CY ⇒ 整块跳过。后果有两个:
        ;     ① 1CEh/1CFh 完全没有保护(本轮要修的主症状);
        ;     ② 端口 30h 上白留了一个 handler + 开着的全局陷阱 ——
        ;        而我们的 handler 对"非 Windows VM"是【吃掉】,
        ;        这足以解释一部分不可预测的 0D/冻结(见 R117_CHANGES.md §0)。
        ; 【失败怎么办】任一端口没装成 ⇒ DispiInstalled != 3 ⇒
        ;   MiniVDD_Virtual1CE 与两个 Trap 函数全部立即放行/返回,
        ;   硬件足迹与 r115 逐字节相同; 不因此拒绝加载(加载失败会让整机
        ;   失去高彩显示, 而这个功能只是加固)。失败次数记进 DispiErrs,
        ;   ring 3 在每份日志里都能读到它。
        mov     esi, OFFSET32 MiniVDD_Virtual1CE
        mov     edx, 01CEh
        VMMcall Install_IO_Handler
        jc      short di_dispi_err1
        mov     edx, 01CEh
        VMMcall Disable_Global_Trapping         ; 装完先关着(boxvmini 同序)
        or      byte ptr [DispiInstalled], 1    ; bit0 = 1CEh 装成
        jmp     short di_dispi_p2
di_dispi_err1:
        inc     byte ptr [DispiErrs]
di_dispi_p2:
        mov     esi, OFFSET32 MiniVDD_Virtual1CE
        mov     edx, 01CFh
        VMMcall Install_IO_Handler
        jc      short di_dispi_err2
        mov     edx, 01CFh
        VMMcall Disable_Global_Trapping
        or      byte ptr [DispiInstalled], 2    ; bit1 = 1CFh 装成
        jmp     short di_dispi_done
di_dispi_err2:
        inc     byte ptr [DispiErrs]
di_dispi_done:
        cmp     byte ptr [DispiInstalled], 3
        jne     short di_dispi_skip
        call    MiniVDD_DispiTrapOn            ; 按策略开(策略关时立即返回)
di_dispi_skip:

        ; r42: 登记分派表后自查 VBE BIOS 的 V86 入口(纯只读探针)。结果
        ; (成败/地址/步码)存锁定段全局量, 失败不影响加载 —— drv 经
        ; GET_MAPINFO(11) 查询, v86ok=0 时优雅回退。
        ; 【r76】这里不再做任何 PCI 枚举/LFB 映射/PageAllocate 探针:
        ; 帧缓冲映射已由 display.drv 在 ring 3 用 DPMI 完成(文件头)。
        call    MiniVDD_InitMapLfb

        pop     edi
        pop     esi
        pop     ebx
        clc
        ret

di_fail:
        pop     edi
        pop     esi
        pop     ebx
        stc
        ret

EndProc MiniVDD_DeviceInit

;-----------------------------------------------------------------------------
; INIT_COMPLETE —— 动态 VxD 未必收到此消息。
; 【r76】无事可做: LFB 映射在 drv 侧(DPMI), V86 中转缓冲在 drv 首次
; 调用 GET_MAPINFO/api_exec 时惰性建立(EnsureV86)。保留本消息处理
; 只是为了"收到也回 CF=0", 不参与任何加载失败判定。
;-----------------------------------------------------------------------------
BeginProc MiniVDD_InitComplete

        clc
        ret

EndProc MiniVDD_InitComplete

;-----------------------------------------------------------------------------
; MiniVDD_InitMapLfb —— Device_Init 期只做无副作用探针
;   【r76 起只剩一件事】把系统 VM 的 V86 IVT[10h] 目标(段:偏移)记下来,
;   供 api_exec 在"drv 期 IVT[10h] 已被清零(./50 实锤)"时回退使用
;   (见 api_exec 的 ae_tgt_ok)。全程只调 VMMCall Get_Cur_VM_Handle /
;   Get_V86_Int_Vector 两个只读服务, 不碰内存映射、不嵌套执行。
;
; 【为何不做更多】./47 实锤: 本机 Device_Init(PNP 装载, 系统 VM 正忙于
; 实模式引导)期第一个嵌套 Exec_Int(21h) 就会失败(InitStep=103) ——
; DOS 引导期未就绪是最大嫌疑; 而 drv 期 PM API 上下文嵌套执行是
; api_exec 既有生产机制。故 V86 中转缓冲的建立延迟到 drv 期惰性进行
; (MiniVDD_EnsureV86)。
;
; 步码(InitStep): 1=Get_Cur_VM_Handle 基线过(LastEax=句柄)
;                 2=V86 IVT[10h] 目标取证过(LastEax=cx:dx)
; 失败: 101=VM 句柄服务异常 102=向量段为 0
; 后续步码由 EnsureV86 续用: 4=缓冲就绪(DOS)/5=缓冲就绪(B0000)/
;       103=分配失败(LastEax 里有子码)
;-----------------------------------------------------------------------------
BeginProc MiniVDD_InitMapLfb

        push    ebp
        push    ebx
        push    esi
        push    edi
        cld

        ; --- 步 1: VMM 服务基线探针(Get_Cur_VM_Handle) ------------------
        VMMCall Get_Cur_VM_Handle               ; → EBX = 当前 VM 句柄
        mov     [LastEax], ebx
        test    ebx, ebx
        jz      im_fail1
        mov     dword ptr [InitStep], 1

        ; --- 步 2: V86 IVT[10h] 目标取证 --------------------------------
        mov     eax, 10h
        VMMCall Get_V86_Int_Vector              ; CX:DX = 10h 目标
        mov     eax, ecx
        shl     eax, 16
        mov     ax, dx
        mov     [LastEax], eax
        test    cx, cx                          ; 段为 0 = 异常(./50 实锤
        jz      im_fail2                        ;  drv 期本机就是 0:0, 但
        mov     [g_romSeg], cx                  ;  Device_Init 期能读到真值)
        mov     [g_romOff], dx
        or      dword ptr [MapLfbStat], VBE_ST_IVT10
        mov     dword ptr [InitStep], 2
        jmp     im_out

im_fail1:
        mov     dword ptr [InitStep], 101
        jmp     im_out
im_fail2:
        mov     dword ptr [InitStep], 102
        jmp     im_out

im_out:
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

EndProc MiniVDD_InitMapLfb

VxD_ICODE_ENDS

        END
