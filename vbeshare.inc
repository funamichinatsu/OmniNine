;=============================================================================
; vbeshare.inc —— vbeshare.h 的汇编镜像（两边必须逐字段一致）
;
; 【设计约束】
;  1. 这里的每个 VBS_xxx 都是 VbeState 里某个字段的【偏移】。C 侧结构改一处，
;     这里就必须改对应的那一处，否则汇编会安静地读写到错误的字节。
;  2. 只允许在末尾追加并同步长度；不要调整已有字段的相对次序。
;  3. 探针槽位中有一部分被本文件的使用者当状态机读回，不是纯日志，删除前要查自读。
;=============================================================================
;=============================================================================
;
;  VBESHARE.INC — vbeshare.h 的 MASM 镜像
;
;  【为何需要这个文件】
;  minivdd.asm 是汇编，无法 #include C 头文件；而共享状态结构的布局
;  又必须与 vbeshare.h 逐字节一致，否则 ring 3 写入的字段在 ring 0
;  会读错位。两边任何一处改动都必须同步另一处。
;
;  【r76 变更】删掉 r69-r75 "VxD 侧自建 LFB 映射"实验用的全部诊断字段
;  (lad*/gt*/MM*)。帧缓冲映射改由 display.drv 在 ring 3 用 DPMI 0800h
;  完成, mini-VDD 只把 drv 建好的屏选择子登记给主 VDD —— 详见
;  vbeshare.h 文件头。
;
;  本文件还集中定义：
;    1) 设备 ID / API 功能号 / IOCTL 码(与 vbeshare.h 相同数值)
;    2) VDD 的服务序列表(VxDcall VDD_xxx 要靠它生成 INT 20h 服务号)
;    3) mini-VDD 分派表的 49 个功能索引(与 Win9x DDK minivdd.h 一致)
;    4) VDD_CY / VDD_NC 宏(mini-VDD 处理函数通过客户端 EFlags 的
;       进位位向主 VDD 报告"已处理/未处理")
;
;=============================================================================

;-----------------------------------------------------------------------------
; 与 vbeshare.h 相同的常量
;-----------------------------------------------------------------------------
; 【r121 自证标记 —— 别删, symguard.py 与 buildvxd.bat 都读它】
; 形态固定为: "; SYMGUARD_SRC r<N> vxd=<version>"
; 作用: 用户机器上如果【新旧文件混放】(例如新 minivdd.asm 配旧 vbeshare.inc),
; 这一行就是唯一能分辨"手里到底是哪一轮"的东西 —— r121 之前的包没有任何
; 轮次标记, 混放时只能靠猜(见 symguard.py 的长注)。
; 【r138】同步到真值(此前停在 r129 / 0x0114, 而版本已涨到 0x0118)。
; ============================================================================
; 【r151】见 vbeshare.h 同名长注。ASM 侧同步的五件事:
;   1. 72h/73h 改写路径【删除】(./150: m72=28 而 arm72=0, 闸门永远够不着;
;      且 DDK《CHECK_HIRES_MODE》原文的对应关系是 72h<->02h / 73h<->03h)。
;   2. 35 号按 DDK 答(答案描述 EBX 里的那个 VM), 默认 4(HiresAns=1 可回退)。
;   3. 6 号补 TrapOff / 7 号补 TrapOn —— DDK《PRE_HIRES_TO_VGA》原文要求
;      PRE_VGA_TO_HIRES 也关陷阱、POST_VGA_TO_HIRES 也恢复陷阱。
;   4. 持屏期改写 SR02/SR04/GCR04/05/06 的旧写法 的【探针】挪到 policyMemCFix 闸门之前(仪器不能放在闸门后)。
;   5. 头部 368 -> 392: 新增 VBE_TR_Own 24 字节块(屏幕所有权/特殊 VM/35 号 AL)。
; ============================================================================
; 【r152】见 vbeshare.h 同名长注。ASM 侧同步的四件事:
;   1. 头部 392 -> 424: 新增 VBE_TR_Probe 32 字节块(取指基址探针/修复记账)。
;   2. 新增 显存逐格采样(纯读) 与 VgaFetchFix(只写 CR0C/CR0D, 写后自证,
;      不成立立刻还原)。**一个模式/寻址寄存器都不写。**
;   3. MemCFix 的档位语义扩展: 0=关 1=六字节文本补写(./148 灰屏) 2=取指基址。
;      ASM 侧那条写六个寄存器的路只在 policyMemCFix==1 时才走。
;   4. OwnWord 尾部三个字节启用: 持屏期映射选择变化次数 / 最近值。
; ============================================================================
; SYMGUARD_SRC r412 vxd=0x01CA

VBESTATE_SIG        EQU 53454256h   ; 'VBES'
VBE_STATE_SIZE      EQU 48          ; sizeof(VBESHAREDSTATE) (r101: 40→44; r144: 44→48)

VBE_VXD_DEVICE_ID   EQU 4342h       ; INT 2F/1684h 用的设备 ID(必须非零)
; r121: 0113h→0114h。原因不是共享结构改了(它一个字没改, 仍是 44 字节),
; 而是 mini-VDD 的 PM API 多了一张 15 号(DISPI_PHYS 物理读) —— 接口面变了
; 就该涨版本。副作用正是我们要的: 旧 minivdd.vxd 配新 drv 时日志里
; "VxD version" 一行会明确报不一致, 而不是默默把 API 15 当成"不支持"。
; 【r148 —— 这一行为什么不再是一整行长注释; 别再把它写回一行】
;   r147 把四轮历史全塞进下面那条 EQU 的【行尾注释】里, 那行因此长到 483 字节
;   (光注释就 451 字节)。ML 6.11c 把行尾注释当 text literal 存, 上限 255 字节,
;   超了报:
;       VBESHARE.INC(43) : error A2041: string or text literal too long
;   而且【整行被丢弃】—— EQU 那一行根本没执行, 于是 VBE_VXD_VERSION 这个符号
;   压根没被定义, 汇编接着在毫不相干的地方报:
;       minivdd.asm(3117) : error A2006: undefined symbol : VBE_VXD_VERSION
;   一条写长了的注释, 报出来的却是另一个文件里另一个符号名。
;   修法: 历史搬到独立注释行(独立注释行不受这个上限约束), 行尾只留几个字。
;   两处机械防线, 都别删:
;     buildvxd.bat   —— ml 之前用 findstr 逐文件量【字节】长度, 250 以上直接停机
;                      (那台编译机没有 PowerShell, 所以这道闸只能是 cmd);
;     buildcheck.ps1 —— 逐条语句行量字节数, 并单独量行尾注释字节数(更紧)。
;
; 【版本变更史 —— 以下照抄 r147 那一行行尾注释的原文】
;   r152: asm changed -- NEW read-only probe 显存逐格采样 locates the console's
;         4000 text cells in CPU address space (nPrint at B8000 / at the CPU
;         image of the B8000 display-memory offset / at the CURRENT fetch
;         address / at A0000) plus the raw bytes and the GCR06 map select,
;         because r143..r151 measured every mode/addressing register and
;         never once measured WHERE the text buffer is or WHAT the CRTC
;         fetch is pointing at.  NEW VgaFetchFix writes ONLY CR0C/CR0D
;         (the display start address -- not a timing register, and exactly
;         what hardware scrolling writes) when, and only when, the probe
;         proves the console's bytes are reachable in video memory at the
;         CPU's B8000 image while the fetch is not reading them; it re-probes
;         immediately, keeps the first candidate that verifies, and restores
;         the original start when none does.  It never touches Sequencer,
;         Graphics Controller, Attribute Controller or any CRTC timing
;         register -- that is the deliberate opposite of r147's six-byte
;         rewrite, which ./148 measured as harmful.  policyMemCFix grows a
;         third value: 0 = off, 1 = the r147 six-byte text rewrite (./148
;         grey screen), 2 = [default] fetch-base repair.  Blob header
;         392 -> 424 (new 32-byte VBE_TR_Probe block).
;   r151: asm changed -- the 72h/73h -> 03h rewrite is DELETED (./150:
;         m72=28 requesters, arm72=0 while the box owns the screen, and
;         the DDK maps 72h<->02h / 73h<->03h); hook 35 answers per the DDK
;         (default 4); hooks 6/7 now drop/reestablish the extended-register
;         trapping as the DDK's PRE_HIRES_TO_VGA page requires; hook 9
;         records the CRTC (ECX) and MemC (ESI) owner classes; the six-
;         register in-box probe moved ABOVE the policyMemCFix gate; blob
;         header 368 -> 392 (new 24-byte VBE_TR_Own block).
;   r150: asm changed -- hook 48 classifies the requester (requested mode
;         72h/73h, in-box arm state, requester EBX) and the 72h/73h -> 03h
;         rewrite is gated back to the in-box arm window (r149's
;         unconditional rewrite stays reachable as Int10Fix=2).
;   r147: the in-box text-mode repair can finally fire (one-bit proxy ->
;         six-byte compare, budget deleted) + AR10 joins the fingerprint +
;         one 16-byte slot per call site.
;   r146: asm changed (mini-VDD keeps its own extended state across the
;         hires<->VGA switch; fingerprint 4 -> 16 bytes).
;   r145: asm changed (in-box fingerprint also sampled from hook 35 while armed).
;   r144: asm changed -- fbox counters + in-box snapshot + Int10Fix
;         (72h/73h->03h).
;   r143: (原文在此断句, 下面两行是它原本的续行)
;         returns BX/CX/DX/SI/DI/ES (VBE 4F03/4F06/4F07).
;         r137: 一条指令(AR10h 0041h); r136: AR 采样.
; 【r148 的语义】汇编源码与 r147 一字不差 —— r147 的包只是编不过, 重出一次;
;   版本号照样 +1, 免得两个不同的包(一个能编、一个编不过)都自称 r147,
;   混放时无法分辨。r147 的 A2041 永远不会有日志, 所以日志里
;   "PH: build tag==0148" 是本版, "==0147" 不可能出现。
VBE_VXD_VERSION     EQU 01CAh   ; r162: keep this line SHORT -- see note above

VBE_VXD_IOCTL_PUSH_STATE EQU 1      ; DeviceIoControl: 推送状态
VBE_VXD_IOCTL_GET_STATE  EQU 2      ; DeviceIoControl: 取回状态

VBE_API_GET_VERSION EQU 0           ; V86/PM API: 取版本
VBE_API_PUSH_STATE  EQU 1           ; V86/PM API: 推送状态
VBE_API_GET_STATE   EQU 2           ; V86/PM API: 取回状态
VBE_API_GET_DBG     EQU 3           ; V86/PM API: 取 VESA_SUPPORT 调用计数
VBE_API_EXEC_BIOS   EQU 4           ; V86/PM API: 嵌套 V86 直通 VBE BIOS
VBE_API_EXEC_NOOP   EQU 6           ; V86/PM API: 直通诊断1相(空桩嵌套执行,不调BIOS)
VBE_API_IVT_PROBE   EQU 8           ; V86/PM API: 取证: Get_V86_Int_Vector 读 IVT[10h]/[借用向量]
VBE_API_VM_HANDLE_PROBE EQU 9       ; V86/PM API: 探针: VMMCall Get_Cur_VM_Handle 基线
VBE_API_NEST_BASE   EQU 10          ; V86/PM API: 探针: 纯嵌套执行基线(Exec_Int 21h AH=30h, 不碰 IVT)
VBE_API_GET_MAPINFO EQU 11          ; V86/PM API: 回报缓冲/状态位图(回报前惰性 EnsureV86)
VBE_API_GET_MODELIST EQU 12         ; V86/PM API: 从 V86 缓冲的 VbeInfo 读
VBE_API_GET_TRACE   EQU 13          ; V86/PM API: 取回 mini-VDD 分派调用追踪
                                    ;  VideoModePtr, VxD 侧线性遍历模式号表
                                    ;  (BIOS 常无视预置指针 ./51), 整批拷进
                                    ;  客户端 bufLin(2B/项, ≤bufLen/2 且≤128),
                                    ;  out_ax=项数; 需缓冲就绪且 4F00 已执行
VBE_API_SET_INCB    EQU 14          ; V86/PM API【r111】: 置/清"显示驱动正在
                                    ;  主 VDD 回调里"的标记(EBX=0/1)。VxD 用它
                                    ;  在 api_exec 里加一道 VM 自检 —— 只有
                                    ;  当前 VM == Windows VM 才允许嵌套 V86
                                    ;  执行(回调可能切在别的 VM 上下文里)。
VBE_API_DISPI_PHYS  EQU 15          ; PM API【r120】: ring 0 物理读 VBE_DISPI
                                    ;  (1CEh/1CFh)的六个寄存器, 用
                                    ;  VDD_Do_Physical_IO 绕开 VM 的端口虚拟化。
                                    ;  【为什么必须有它】./119 的 ring 3 梯形跑了
                                    ;  28 次全是 FFFF, 但那有两种解释(端口不存在 /
                                    ;  我们那次 I/O 没抵达硬件), 而两者的后续
                                    ;  动作完全相反; ring 3 分不出来。物理 I/O 是
                                    ;  唯一能分开它们的通路。
                                    ;  出口 out_ax = 物理读次数(模 256); 六个
                                    ;  寄存器的值随下一次 GET_TRACE 的 blob
                                    ;  offset 216 带出(见下面 VBE_TR_DispiPhys)。
                                    ;  ⚠ 这个 EQU 是 minivdd.asm 汇编期就要用的,
                                    ;  只写在 vbeshare.h 里【不够】(A2006)。

; VBEEXECREQ 字段偏移(与 vbeshare.h 的 struct 严格同布局, 共 60 字节)
; 【r138 为什么多了 12 字节】r112..r137 的回放/判据全部只能看 VGA 寄存器
;   (3D4h/3C4h/3CEh/3C0h 那一套), 而 ./137 的日志证明了那套读数的边界:
;   几何(CR00/CR01)可以完全正确、SE(CR17 bit7)可以是 1、AR10h 可以是
;   0041h, 而画面仍然是"按 8bpp 重解出来的条纹" —— 因为【扫描输出的像素
;   格式】根本不在 VGA 寄存器里(它是芯片扩展状态, 通用 VBE 驱动不知道
;   它在哪)。唯一通用且标准地问到它的通路是 VBE 自己的读回功能:
;       4F03h -> BX = 当前模式号   (当前模式不可能"看起来对"却是 8bpp)
;       4F06h -> BX = 每扫描行字节数, CX = 每扫描行像素数
;       4F07h -> BX = 显示起始
;   它们的结果都在【寄存器】里, 而旧版请求块只回填 AX 与 flags —— 所以
;   本项目的 ROADMAP 里 T1.2/T2.2 一直写着"要拿到它们就得改 VxD 结构 +
;   递增 VBE_VXD_VERSION + 用户重编 minivdd.vxd"。r138 就做这一件事。
;   【为什么追加在最后】所有旧字段的偏移一个都不动 —— 采集侧与消费侧
;   即使一个忘了改, 也只是新字段读到 0, 不会把老字段读错位。
VXE_out_bx      EQU 48              ;  【r138】VxD 回填: BIOS 返回 BX
VXE_out_cx      EQU 50              ;  【r138】VxD 回填: BIOS 返回 CX
VXE_out_dx      EQU 52              ;  【r138】VxD 回填: BIOS 返回 DX
VXE_out_si      EQU 54              ;  【r138】VxD 回填: BIOS 返回 SI
VXE_out_di      EQU 56              ;  【r138】VxD 回填: BIOS 返回 DI
VXE_out_es      EQU 58              ;  【r138】VxD 回填: BIOS 返回 ES
VXE_in_ax       EQU  0              ;  BIOS 输入寄存器
VXE_in_bx       EQU  2
VXE_in_cx       EQU  4
VXE_in_dx       EQU  6
VXE_in_si       EQU  8
VXE_in_di       EQU 10
VXE_in_ds       EQU 12
VXE_in_es       EQU 14
VXE_stubSeg     EQU 16              ;  api_exec 借向量前改写为 VxD 自有桩段;
                                    ;  GET_MAPINFO 借此字段回传 InitStep
VXE_stubOff     EQU 18              ;  VxD 自有桩偏移(VBE_EXEC_STUB_OFF)
VXE_vec         EQU 20              ;  借用的 IVT 向量(0x63)
VXE_out_ax      EQU 22              ;  VxD 回填: BIOS 返回 AX
VXE_out_flags   EQU 24              ;  VxD 回填: 返回 flags(bit0=CF)
VXE_stubLin     EQU 26              ;  VxD 回填: 本次 callf 目标(cx:dx 打包
                                    ;   hi=seg), 判 ROM 还是 Win386 反射桩
VXE_bufLen      EQU 30              ;  数据中转字节数(4 的倍数), 0=无
VXE_bufLin      EQU 32              ;  客户端缓冲线性地址(dword)
VXE_v86Seg      EQU 36              ;  VxD 回填: V86 中转缓冲段
VXE_v86Ok       EQU 38              ;  VxD 回填: 缓冲可用
VXE_stat        EQU 40              ;  VxD 回填: VBE_VXD_STAT_* 位图
VXE_step        EQU 42              ;  VxD 回填: InitStep 状态机步码
VXE_lastEax     EQU 44              ;  VxD 回填: LastEax 快照(dword)
VBE_EXECREQ_SIZE EQU 60
VBE_EXEC_STUB_OFF EQU 768
VBE_EXEC_VEC    EQU 63h             ;  借用向量: IBM 保留用户区

; stat 位(VBE_VXD_STAT_*)
VBE_ST_DOSBUF   EQU 0001h           ;  V86 缓冲经 DOS AH=48h 建立
VBE_ST_B0000    EQU 0004h           ;  V86 缓冲走 B0000 物理暂存回退
VBE_ST_HOOKED   EQU 0010h           ;  分派表 5 项已挂上
VBE_ST_IVT10    EQU 0020h           ;  IVT[10h] 目标已取证
VBE_ST_SELREG   EQU 0040h           ;  屏选择子已登记给主 VDD
VBE_ST_EXEC     EQU 0080h           ;  api_exec 至少跑过一次
VBE_ST_MDLIST   EQU 0100h           ;  GET_MODELIST 至少跑过一次

VBE_LFB_MAP_CAP     EQU 32*1024*1024 ; LFB 映射字节数上限(32MB), 见 vbeshare.h

;-----------------------------------------------------------------------------
; VBESHAREDSTATE —— 与 vbeshare.h 的 struct _VBESHAREDSTATE 严格同布局
; (pack 1，共 44 字节; r101 由 40 扩到 44, 仍是 4 的倍数 ⇒ 两侧
;  `mov ecx, VBE_STATE_SIZE/4` 的 rep movsd 长度自动跟随)
;-----------------------------------------------------------------------------
VBESHAREDSTATE STRUC
    VBS_signature           dd ?            ;  0  'VBES'
    VBS_currentMode         dw ?            ;  4
    VBS_bytesPerScanLine    dw ?            ;  6
    VBS_xRes                dw ?            ;  8
    VBS_yRes                dw ?            ; 10
    VBS_bitsPerPixel        db ?            ; 12
    VBS_bankSizeKB          db ?            ; 13
    VBS_physBasePtr         dd ?            ; 14 LFB 物理基址(4F01)
    VBS_hasLFB              db ?            ; 18
    VBS_reserved0           db ?            ; 19
    VBS_structSize          dd ?            ; 20
    VBS_vramSize            dd ?            ; 24
    VBS_vbeVersion          dw ?            ; 28
    VBS_lfbLinear           dd ?            ; 30 drv 的 DPMI 0800h 映射线性
    VBS_lfbSelector         dw ?            ; 34 drv 自建的 LDT 屏选择子(0=未建)
    ; --- r99 策略字节(复用原 4 字节保留区, 总长仍 40) --------------------
    VBS_policyVddEdx        db ?            ; 36 0=REGISTER 用 EDX 0; 1=用 -1
    VBS_policyVesa          db ?            ; 37 0=恒 NC; 1=拒绝改硬件类 4Fxx
    VBS_policyHires         db ?            ; 38 0=不挂钩; 1=按状态答; 2=只答
                                            ;    Windows VM; 3=只答非 Windows VM
    VBS_policyRestore       db ?            ; 39 VxD 不读(日志对齐用)
    ; --- r101 策略字节(结构 40 → 44) ------------------------------------
    VBS_policyBank          db ?            ; 40 1=GET_VDD_BANK 拒绝窗口化
                                            ;    4 平面(EDX=ECX+CY)
    VBS_policyInt10         db ?            ; 41 1=48 号吃掉非 VGA 期的设模式
    VBS_policyVgaFix        db ?            ; 42 【r114】1 = POST_HIRES_TO_
                                            ;    VGA(5) 里补写文本取指/平面
                                            ;    寄存器组(见 文本态修补);
                                            ;    0 = 不写(与 r113 等价)。
                                            ;    原来是 policyResv0 —— 沿用
                                            ;    同一偏移, 结构仍 44 字节。
    VBS_policyDispi         db ?            ; 43 【r116】1 = 1CEh/1CFh
                                            ;    (Bochs/VBox VBE_DISPI)装
                                            ;    I/O handler 并在 Windows
                                            ;    拥有 CRTC 时开全局陷阱(非
                                            ;    Windows VM 的访问被吃掉);
                                            ;    0 = 完全不装/不开(与 r115
                                            ;    行为逐字节相同)。
                                            ;    原来是 policyResv1 —— 沿用
                                            ;    同一偏移, 结构仍 44 字节。
    VBS_policyInt10Fix      db ?            ; 44 【r144】1 = 48 号里把
                                            ;    Far-East 文本模式 72h/73h
                                            ;    改写成标准 03h(仅全屏盒
                                            ;    子持屏期 = 4→7 武装期内);
                                            ;    0 = 不改(与 r143 逐字节相同)。
                                            ;    结构 44 → 48 字节。
    VBS_policyHiresFix      db ?            ; 45 【r146】1 = mini-VDD 在 4 号存一份
                                        ;    模式/寻址字节、7 号写回(默认 1)
    VBS_policyMemCFix       db ?            ; 46 【r147】1 = 盒子持屏期按"六字节
                                        ;    与文本目标比较"补写 SR02/SR03/
                                        ;    SR04/GCR05/GCR06/AR10(默认 1);
                                        ;    0 = 持屏期一个字节都不写(= r146)。
                                        ;    复用 r144 的第二个填充字节 ⇒
                                        ;    VBE_STATE_SIZE 仍是 48。
VBS_reserved2           db 1 dup(?)      ; 47 pad to VBE_STATE_SIZE
                                            ;    【为什么不能省】VBE_STATE_SIZE
                                            ;    48 而结构只到 44 ⇒ drv 推送
                                            ;    sizeof=45 字节, MiniVDD_ApplyState
                                            ;    的 `cmp ecx, VBE_STATE_SIZE /
                                            ;    jb as_fail` 直接拒收整包: 所有
                                            ;    策略字节在 VxD 里恒 0, 而 ring 3
                                            ;    照旧打印 run VgaFix=0001。
                                            ;    r143 两侧都是 44, 所以那时是好的。
VBESHAREDSTATE ENDS                         ; 48

;-----------------------------------------------------------------------------
; VDD 服务表
; 【为何自己定义而不 INCLUDE VDD.INC】
;   Win95 DDK 的 VDD.INC 只随"显示驱动 DDK 组件"安装，很多环境里只有
;   VMM.INC。VxDcall 需要的只是服务序号(生成 INT 20h 后的服务 DWORD)，
;   序号已由 Ralf Brown 中断列表(INT 20/VxD=000Ah)与开源实现双向核实，
;   在此按序声明即可。
; 【为何只调用一项也要保留全表】
;   服务号是【位置编码】：VDD_Register_Extra_Screen_Selector 是第 18 项，
;   删掉它前面的任何一项都会改变它的序号，生成错误的服务 DWORD。
;   因此整张表必须完整保序，中间不能缺项。
;-----------------------------------------------------------------------------
Begin_Service_Table VDD, VxD

VDD_Service VDD_Get_Version, LOCAL              ;  0 取 VDD 版本
VDD_Service VDD_PIF_State, LOCAL                ;  1
VDD_Service VDD_Get_GrabRtn, LOCAL              ;  2
VDD_Service VDD_Hide_Cursor, LOCAL              ;  3
VDD_Service VDD_Set_VMType, LOCAL               ;  4
VDD_Service VDD_Get_ModTime, LOCAL              ;  5
VDD_Service VDD_Set_HCurTrk, LOCAL              ;  6
VDD_Service VDD_Msg_ClrScrn, LOCAL              ;  7
VDD_Service VDD_Msg_ForColor, LOCAL             ;  8
VDD_Service VDD_Msg_BakColor, LOCAL             ;  9
VDD_Service VDD_Msg_TextOut, LOCAL              ; 10
VDD_Service VDD_Msg_SetCursPos, LOCAL           ; 11
VDD_Service VDD_Query_Access, LOCAL             ; 12
VDD_Service VDD_Check_Update_Soon, LOCAL        ; 13
VDD_Service VDD_Get_Mini_Dispatch_Table, LOCAL  ; 14 取 mini-VDD 分派表
                                                ;    返回 EDI=表地址 ECX=表项数
VDD_Service VDD_Register_Virtual_Port, LOCAL    ; 15
VDD_Service VDD_Get_VM_Info, LOCAL              ; 16
VDD_Service VDD_Get_Special_VM_IDs, LOCAL       ; 17
VDD_Service VDD_Register_Extra_Screen_Selector, LOCAL ; 18 登记屏选择子
                                                ;    入口: AX/EAX=选择子
                                                ;    (DDK 原文: "A display
                                                ;    driver may also use the
                                                ;    screen selectors provided
                                                ;    by the Windows 95 Kernel
                                                ;    ... Whenever the display
                                                ;    driver uses a screen
                                                ;    selector other than the
                                                ;    one provided by VFLATD, it
                                                ;    must be registered with the
                                                ;    Main VDD so that the Main
                                                ;    VDD knows when to switch
                                                ;    the MemC state back to that
                                                ;    required by the Windows VM")
VDD_Service VDD_Takeover_VGA_Port, LOCAL        ; 19
VDD_Service VDD_Get_DISPLAYINFO, LOCAL          ; 20
VDD_Service VDD_Do_Physical_IO, LOCAL           ; 21 执行一次真实端口 I/O
VDD_Service VDD_Set_Sleep_Flag_Addr, LOCAL      ; 22
VDD_Service VDD_EnableDevice, LOCAL             ; 23

End_Service_Table VDD, VxD

;-----------------------------------------------------------------------------
; mini-VDD 分派表功能索引(与 Win9x DDK minivdd.h 一致，共 49 项)
; 注册方法：Device_Init 时 VxDcall VDD_Get_Mini_Dispatch_Table 取得
; 主 VDD 的表(EDI)与表长(ECX)，确认表长 >= 49 后，把我们要接管的
; 表项改写为自己的处理函数地址。
;
; 【v2 起只覆盖 5 项】通用 VBE 驱动把硬件操作全部留给主 VDD 的默认
; 处理器(其回退路径就是 VESA BIOS)；未覆盖的表项保持原样即等价于
; "返回 NC 走 BIOS"，因此不再注册任何空壳处理函数。★ = 实际覆盖。
;-----------------------------------------------------------------------------
MINIVDD_REGISTER_DISPLAY_DRIVER     EQU 0   ; 显示驱动注册
MINIVDD_GET_VDD_BANK                EQU 1
MINIVDD_SET_VDD_BANK                EQU 2
MINIVDD_RESET_BANK                  EQU 3
MINIVDD_PRE_HIRES_TO_VGA            EQU 4   ; 高分辨率 → VGA 之前
MINIVDD_POST_HIRES_TO_VGA           EQU 5   ; 高分辨率 → VGA 之后
MINIVDD_PRE_VGA_TO_HIRES            EQU 6   ; VGA → 高分辨率 之前
MINIVDD_POST_VGA_TO_HIRES           EQU 7   ; VGA → 高分辨率 之后
MINIVDD_SAVE_REGISTERS              EQU 8   ; 保存卡专有寄存器
MINIVDD_RESTORE_REGISTERS           EQU 9   ; 恢复卡专有寄存器
MINIVDD_MODIFY_REGISTER_STATE       EQU 10
MINIVDD_ACCESS_VGA_MEMORY_MODE      EQU 11  ; 窗口化 VGA 平面访问状态
MINIVDD_ACCESS_LINEAR_MEMORY_MODE   EQU 12  ; 线性帧缓冲访问状态
MINIVDD_ENABLE_TRAPS                EQU 13  ; 打开端口陷阱
MINIVDD_DISABLE_TRAPS               EQU 14  ; 关闭端口陷阱
MINIVDD_DISPLAY_DRIVER_DISABLING    EQU 26  ; ★ 显示驱动即将卸载
MINIVDD_GET_CURRENT_BANK_WRITE      EQU 32  ; 取当前写 bank
MINIVDD_GET_CURRENT_BANK_READ       EQU 33  ; 取当前读 bank
MINIVDD_SET_BANK                    EQU 34  ; 设置读/写 bank
MINIVDD_CHECK_HIRES_MODE            EQU 35
MINIVDD_GET_TOTAL_VRAM_SIZE         EQU 36  ; ★ 取显存总大小
MINIVDD_GET_BANK_SIZE               EQU 37  ; ★ 取 bank 大小与显存窗口
MINIVDD_SET_HIRES_MODE              EQU 38  ; 设置高分辨率模式
MINIVDD_PRE_HIRES_SAVE_RESTORE      EQU 39
MINIVDD_POST_HIRES_SAVE_RESTORE     EQU 40
MINIVDD_VESA_SUPPORT                EQU 41  ; ★ ring 0 VESA 支持钩子
MINIVDD_GET_CHIP_ID                 EQU 42  ; 取芯片 ID(即插即用校验)
MINIVDD_CHECK_SCREEN_SWITCH_OK      EQU 43  ; 全屏 DOS 可否切走
MINIVDD_VESA_CALL_POST_PROCESSING   EQU 47  ; ★ VESA 调用后的收尾
MINIVDD_PRE_INT_10_MODE_SET         EQU 48  ; INT 10h 模式设置之前
NBR_MINI_VDD_FUNCTIONS              EQU 49

;--- r99: mini-VDD 分派调用追踪 -------------------------------------------
; 【为什么需要它】vbe9xlog 的每一行都是 ring 3 写的; 主 VDD 对我们的
; 分派服务调用在日志里【完全不可见】。r98 之前所有"主 VDD 到底问了我们
; 什么"的判断都是推断。这里给每个分派入口装一个只记录、不改语义的桩,
; 环形记录 + 每功能计数由 drv 经 VBE_API_GET_TRACE(13) 整块取回落盘。
; 布局与 vbeshare.h 的 VBE_TR_*/VBE_TREC_* 严格一致。
VBE_TRACE_FUNCS     EQU 49          ; 分派功能号个数(计数桶数)
VBE_TRACE_RECS      EQU 64          ; 环形记录条数
VBE_TRACE_RECB      EQU 24          ; 每条记录字节数(r100: 16→24)
VBE_TRACE_HDR       EQU 648         ; 头部(4+4 + 49*4 + 12 + 4 + 16 + 16 + 8
                                    ;   + 96 + 16 + 24 + 256)
                                    ;   【逐项】8 + 196 + 12 + 4 + 16 + 16
                                    ;   + 8 = 252(计数区) + 96(六个采集槽)
                                    ;   + 16(补写记账) + 24(r151 所有权)
                                    ;   + 248(r153 取证: Probe 32→248) = 640
                                    ;   r146: 236→256; r147: 256→368;
                                    ;   r151: 368→392(多了 VBE_TR_Own 24);
                                    ;   r152: 392→424(多了 VBE_TR_Probe 32);
                                    ;   r153: 424→640(VBE_TR_Probe 32→248)
;   r412: 640→648(多了 8 字节 Exec_VxD_Int 探针; 见 VBE_TR_PR_DS_*)
                                    ;   (见 vbeshare.h 逐字段推导)
VBE_TRACE_SIZE      EQU VBE_TRACE_HDR + VBE_TRACE_RECS * VBE_TRACE_RECB

VBE_TR_WriteIdx     EQU 0           ; dd 累计写入条数(= 下一条序号)
VBE_TR_Evt          EQU 4           ; 【r100】4 个 8 位事件计数器(VxD 写/drv 读)
VBE_TR_Counts       EQU 8           ; 49 × dd 每功能累计调用次数
VBE_TR_State        EQU 204         ; 【r101】db 切屏状态机(见 VBE_TR_ST_*)
VBE_TR_I10Cnt       EQU 205         ; 【r101】db 48 号被调用次数(模 256)
VBE_TR_I10Ax        EQU 206         ; 【r101】dw 48 号最近请求的模式号
VBE_TR_Stat2        EQU 208         ; 【r117】db×4 DISPI 记账(VxD 写/drv 读)
VBE_TR_ST2_INST     EQU 0           ;   +0 装成的端口位图(bit0=1CE,bit1=1CF)
VBE_TR_ST2_TRAP     EQU 1           ;   +1 DispiTrapOn 当前值
VBE_TR_ST2_ACT      EQU 2           ;   +2 真正执行过 Global_Trapping 的次数
VBE_TR_ST2_ERR      EQU 3           ;   +3 Install_IO_Handler 失败次数
; 【r144】盒子持屏期取证块(取代 r143 的 VgaS 块; 见
;   vbeshare.h 同名段的长注)。四个字节必须连续 ——
;   api_gettrace 用一个 mov eax/dword 整块搬走。
VBE_TR_Fbox         EQU 212         ; db×4: +0 I10MAP / +1 N35 / +2 N48 / +3 DISARM
VBE_TR_FBOX_I10MAP  EQU 0           ;   +0 【r412】4 号那一枪的结果位图
                                    ;      (bit0..2 状态 / b3=72h / b4=73h /
                                    ;      b5=02/03/07, b6=当前VM=盒子VM);
                                    ;      旧含义(48 号改写次数)
                                    ;      已随 r170 撤掉, 见 vbeshare.h 长注
VBE_TR_FBOX_N35     EQU 1           ;   +1 武装期间 35 号被调用次数(模 256)
VBE_TR_FBOX_N48     EQU 2           ;   +2 武装期间 48 号被调用次数(模 256)
VBE_TR_FBOX_DISARM  EQU 3           ;   +3 0=没武装过 1=武装中 2=7 号解除
; 【r144】盒子持屏期的硬件指纹(武装期间的 48 号那一拍采)。
;   232..235 是 VBE_TR_DispiPhys 块尾部那四个"保留 0"字节:
;   api_gettrace 不再搬 DispiPhysVal 的保留字, 改搬本块 ⇒
;   r146 起头部正式长到 256(见下方 FboxSnap 与 Hires 两块)。
VBE_TR_FboxSnap     EQU 232         ; db×16: 13 个模式/寻址寄存器 + 相位
VBE_TR_FS_SR04      EQU 0
VBE_TR_FS_GCR06     EQU 1
VBE_TR_FS_CR13      EQU 2
VBE_TR_FS_CR17      EQU 3
VBE_TR_FS_SR01      EQU 4
VBE_TR_FS_SR02      EQU 5
VBE_TR_FS_SR03      EQU 6
VBE_TR_FS_GCR03     EQU 7
VBE_TR_FS_GCR04     EQU 8
VBE_TR_FS_GCR05     EQU 9
VBE_TR_FS_CR14      EQU 10
VBE_TR_FS_CR00      EQU 11
VBE_TR_FS_CR01      EQU 12
VBE_TR_FS_SRC       EQU 13          ;   采集相位(4/5/7/8/35/48)
VBE_TR_FS_CR0C      EQU 15          ; 【r149】CR0Ch = 显示起始地址低位(取指基址);
                                    ;   占 FS_LEN=16 的最后一个空槽, 长度不变。
VBE_TR_FS_AR10      EQU 14          ;   【r147】AR10(文本/图形 + 闪烁与行图形)
VBE_TR_FS_LEN       EQU 16
VBE_TR_Hires        EQU 248         ; db×8: valid/savedN/restoredN/diff
VBE_TR_HR_VALID     EQU 0
VBE_TR_HR_SAVEDN    EQU 1
VBE_TR_HR_RESTN     EQU 2
VBE_TR_HR_DIFF      EQU 3
; 【r150】+4..+7 原名 HiresSpare —— 四个字节一直是 0, 却【一直在搬】:
;   api_gettrace 的 `mov eax,dword ptr [HiresSpare]` 早就把它们写进 blob
;   252..255 ⇒ 本轮零传输改动就能用。48 号的分类计数:
; 【r164】这三个字节换了含义(旧的 72h/73h 分类问题 ./150..163 已答完) ——
;   新含义 = 字模守卫: 平面 2 的字模会被"写全部四个平面"的文本写覆盖,
;   见 minivdd.asm 的 字模一致性检查 与 drv.c 的三条新打印。
VBE_TR_HR_FONTN      EQU 4           ;   8x16 字模重装次数(模 256)
VBE_TR_HR_FONTBAD    EQU 5           ;   发现字模被覆盖的次数(模 256)
VBE_TR_HR_FONTVER    EQU 6           ;   +6..+7 最后一次读回的字模指纹
                                    ;   EBX 低字(与日志 VMH edge 对照)
; 【r147】每个采集点各占一个 16 字节槽(见 vbeshare.h 的长注:
;   ./146 的六次落盘相位字节全是 35 号, 因为所有采集点共用一块 ——
;   "最后一个写的人赢"把 4/5/7 三拍整条因果链抹掉了)。
VBE_TR_PhaseSnap    EQU 256         ; db×96: 6 槽 × 16 字节
VBE_TR_PS_P4        EQU 0           ;   4 号(PRE_HIRES_TO_VGA)
VBE_TR_PS_P5        EQU 16          ;   5 号(POST_HIRES_TO_VGA)
VBE_TR_PS_P7        EQU 32          ;   7 号(POST_VGA_TO_HIRES)
VBE_TR_PS_P48       EQU 48          ;   48 号(PRE_INT_10_MODE_SET)
VBE_TR_PS_P35       EQU 64          ;   35 号持屏期"最近一次"
VBE_TR_PS_P35FIRST  EQU 80          ;   35 号持屏期"本场第一拍"
VBE_TR_PS_SLOTS     EQU 6
VBE_TR_PS_LEN       EQU 16
; 【r147】持屏期"文本取指/平面"补写的记账(16 字节, 必须连续:
;   api_gettrace 用 4 条 mov dword 整块搬走)。
VBE_TR_MemC         EQU 352         ; db×16
VBE_TR_MC_SEENTEXT  EQU 0           ;   +0 文本态下被看一眼的次数(模 256)
VBE_TR_MC_FIXED     EQU 1           ;   +1 真正写了那 6 个寄存器的次数
VBE_TR_MC_STILLBAD  EQU 2           ;   +2 写完读回仍不一致的个数(最近)
VBE_TR_MC_GFX       EQU 3           ;   +3 见到"图形模式"的次数(模 256)
VBE_TR_MC_LASTSR02  EQU 4           ;   +4..+9 最近一次不一致时的现场值
VBE_TR_MC_LASTSR03  EQU 5
VBE_TR_MC_LASTSR04  EQU 6
VBE_TR_MC_LASTGCR05 EQU 7
VBE_TR_MC_LASTGCR06 EQU 8
VBE_TR_MC_LASTAR10  EQU 9
VBE_TR_MC_DIRTY     EQU 10          ;   +10 发现不一致的次数(模 256)
; 【r167】MCBAD0/1/2 = 整组 mode 3 写入(17 字节)后逐字节读回的失配位图;
;   MCWR = 整组写了几次。位序见 vbeshare.h 的同一段注释。
VBE_TR_MC_MCBAD0    EQU 11
VBE_TR_MC_MCBAD1    EQU 12
VBE_TR_MC_MCBAD2    EQU 13
VBE_TR_MC_MCWR      EQU 14
; 【r168】VBE_TR_MC_FONTBADG —— r147 留作"保留 0"的最后一格, 含义在此落地:
;   低 6 位 = 最近一次"整块回读"里不一致的 dword 数(0..63 饱和), 0 = 那
;   8KB 逐字节相等; 高 2 位 = "让卡自己的 BIOS 重设 mode 3"的结果:
;       0 = 没执行(拷贝成立 / 策略关 / 本场预算已用尽)
;       1 = 执行了, BIOS 回 AH=0
;       2 = IVT[10h] 为 0, 没敢执行
;       3 = 执行了, BIOS 回 AH!=0
;   【为什么两个含义共用一格】VBE_TR_MemC 块长固定 16 字节(块外紧接
;   VBE_TR_PhaseSnap), 而这两条读数都很小: 坏 dword 数只用来回答"整块
;   有没有站住"(0 与非 0), 精度超过 6 位没有意义。字段边界写在这里, 免得
;   下一轮把高两位当成坏格数去读。
VBE_TR_MC_FONTBADG  EQU 15
VBE_TR_MC_LEN       EQU 16
; 【r120】ring 0 物理 I/O 读回的 DISPI 寄存器块(16 字节)。
;   【为什么必须单开一块】./119 的 ring 3 梯形跑了 28 次全是 FFFF, 但那
;   【不能证明端口不存在】—— 也可能是"我们那次 I/O 没抵达硬件"。ring 0 的
;   VDD_Do_Physical_IO 绕开 VM 的端口虚拟化, 是唯一能分开这两种可能的通路,
;   而两种结论接下来的动作完全相反。旧 vxd + 新 drv 时这一块是 0。
VBE_TR_DispiPhys    EQU 216         ;   +0..+11 六个字: ID/XRES/YRES/BPP/
                                    ;            ENABLE/VIRT_WIDTH
VBE_TR_DP_N         EQU 12          ;   +12 db 物理读跑过的次数(模 256)
VBE_TR_DP_OK        EQU 13          ;   +13 db 1 = 回读命中(端口活着)
VBE_TR_DP_ERR       EQU 14          ;   +14 db 失败次数(模 256)
; 【r151】屏幕所有权 / 特殊 VM / 35 号 AL(24 字节, 必须连续:
;   api_gettrace 用 6 条 mov dword 整块搬走)。见 vbeshare.h 同名段的长注。
VBE_TR_Own          EQU 368         ; db×24
VBE_TR_OWN_MSGVM    EQU 0           ;   +0..3  dd 蓝屏消息模式伪 VM 句柄
VBE_TR_OWN_PLANARVM EQU 4           ;   +4..7  dd 强制平面模式伪 VM 句柄
VBE_TR_OWN_BOXVM    EQU 8           ;   +8..11 dd 4 号入口 EBX(= 盒子 VM)
VBE_TR_OWN_CRTCCLS  EQU 12          ;   +12 db 9 号 ECX 分类码
VBE_TR_OWN_MEMCCLS  EQU 13          ;   +13 db 9 号 ESI 分类码
VBE_TR_OWN_P35AL    EQU 14          ;   +14 db 35 号入口 AL
VBE_TR_OWN_P35MASK  EQU 15          ;   +15 db 见过的 AL 种类位图
VBE_TR_OWN_PAIRN    EQU 16          ;   +16 db (CRTC,MemC) 分类对变化次数
VBE_TR_OWN_SAVECLS  EQU 17          ;   +17 db 8 号 EBX 分类码
VBE_TR_OWN_PLANARN  EQU 18          ;   +18 db 命中 PlanarVM 次数
VBE_TR_OWN_MSGN     EQU 19          ;   +19 db 命中 MsgVM 次数
VBE_TR_OWN_LEN      EQU 24
VBE_TR_OWN_MAPCHG   EQU 21          ;   【r152】+21 持屏期显存映射选择
                                    ;      (GCR06 bit3-2)变化次数。
                                    ;      VxD 侧名字是 OW_MAPCHG
VBE_TR_OWN_LASTMAP  EQU 22          ;   【r152】+22 最近一次看到的映射选择。
                                    ;      VxD 侧名字是 OW_LASTMAP
VBE_TR_OWN_LASTPAIR EQU 20          ;   +20 上一次的 (CRTC,MemC) 分类对
                                    ;       (高半字节 CRTC / 低半字节 MemC)
VBE_TR_OWN_CLS_NONE EQU 0
VBE_TR_OWN_BDA449   EQU 23          ;   【r412】+23 db 35 号那一拍的
                                        ;   0:449(当前模式)字节; 0FFh = 本场还没采到
VBE_TR_OWN_CLS_WIN  EQU 1
VBE_TR_OWN_CLS_BOX  EQU 2
VBE_TR_OWN_CLS_OTHER EQU 3
VBE_TR_OWN_AL_72    EQU 01h
VBE_TR_OWN_AL_23    EQU 02h
VBE_TR_OWN_AL_HI    EQU 04h
VBE_TR_OWN_AL_LO    EQU 08h
VBE_TR_OWN_AL_BOXVM EQU 10h         ;   【r412】见过 35 号跑在盒子 VM 里
VBE_TR_OWN_AL_WINVM EQU 20h         ;   【r412】见过 35 号跑在 Windows VM 里
; 【r152】取指基址探针 + 修复记账(32 字节, 必须连续:
;   api_gettrace 用 8 条 mov dword 整块搬走)。见 vbeshare.h 同名段长注。
;   为什么需要这一块: r143..r151 量遍了 SR/GCR/AR/CRTC 的"模式与寻址"寄存器,
;   唯独没有人量过【控制台的那 4000 个文本单元此刻在 CPU 地址空间的哪里】,
;   以及【CRTC 的取指地址此刻读到的是不是同一块内存】。这两件事决定了
;   "写在一处、读在另一处"到底成不成立, 而它是唯一能同时解释
;   "花屏 + mode co80 能救 + 改映射反而更糟"的机制。
VBE_TR_Probe        EQU 392         ; db×32
VBE_TR_PR_NB        EQU 0           ;   +0 打印格数 @ CPU B8000
VBE_TR_PR_NT        EQU 1           ;   +1 打印格数 @ 目标(= MapToCpu(B8000 的显存偏移))
VBE_TR_PR_NX        EQU 2           ;   +2 打印格数 @ 当前取指地址
VBE_TR_PR_NA        EQU 3           ;   +3 打印格数 @ CPU A0000
VBE_TR_PR_CELLS     EQU 4           ;   +4 采样格数(32)
VBE_TR_PR_MAP       EQU 5           ;   +5 GCR06 & 0Ch(显存映射选择)
VBE_TR_PR_CR0C      EQU 6           ;   +6 显示起始地址低位
VBE_TR_PR_CR0D      EQU 7           ;   +7 显示起始地址高位
VBE_TR_PR_FIXN      EQU 8           ;   +8 写 CR0C/CR0D 的次数
VBE_TR_PR_FIXOK     EQU 9           ;   +9 其中"写后自证成立"的次数
VBE_TR_PR_FIXBAD    EQU 10          ;   +10 全部候选失败并已还原的次数
VBE_TR_PR_LATCH     EQU 11          ;   +11 1 = 本场会话已放弃
VBE_TR_PR_CAND      EQU 12          ;   +12 生效的候选(1=off/2 2=off/4 3=off)
VBE_TR_PR_NXA       EQU 13          ;   +13 写完重测的取指打印格数
VBE_TR_PR_NEWL      EQU 14          ;   +14 写进去的低字节
VBE_TR_PR_NEWH      EQU 15          ;   +15 写进去的高字节
;   【r153 扩容】0..31 逐字节不动(./152 的读数仍然可比), 32 起全是新取证。
;   r152 的教训: 判据只看"像不像文本", 而控制台左上角本来就是空的
;   (4000 格里的前 32 格是空格) ⇒ 判据恒真、修复恒不触发。r153 改成
;   【把真的字节抄回来给人看】: 四处 32 字节原文 + 属性调色板 + DAC。
;   【r412】字符集两个平面 2 基均与复制状态。
;   【为什么放在 16】r206/r207 把它们放在 246/247, 而那两个字节分别是
;   OWN15A 与 YIELD —— 所以那两轮的 cs03/csst 读的是别人的计数器。
;   16..23 原来是 RAWB(16..23)/RAWT(24..31)中的 RAWB 前 8 字节: 自 r183 起没有任何 asm
;   写入者(drv.c 只是把它们当 0 印出来)。本轮整块接管, 并保留 RAWB/RAWT 两个名字
;   作为【同一批字节的别名】(drv.c 那两行因此会把两个基均印成 rawb/rawt)。
VBE_TR_PR_CS03      EQU 16          ;   +16  4 号进门那一刻的 SR03 原值
VBE_TR_PR_CSST      EQU 17          ;   +17  bit0 还回去后相等 / bit1 SR03!=0 / bit3 复制过
                                    ;        bit4 复制后相等 / bit5 A==B / bit6 没存到
                                    ;        bit7 复制前两边不同
VBE_TR_PR_CSA       EQU 18          ;   +18..+19 A 在平面 2 的基均(字节)
VBE_TR_PR_CSB       EQU 20          ;   +20..+21 B 同上
VBE_TR_PR_CSN       EQU 22
VBE_TR_PR_BS3POL    EQU 23
VBE_TR_PR_BS3BDA    EQU 24          ;   +24  r412: nest 里读到的 BDA 0:0449
VBE_TR_PR_BS3MD     EQU 25          ;   +25  r412: 真正发出去的模式号
VBE_TR_PR_BS3ST     EQU 26          ;   +26  r412: Bios3Stat 直接读          ;   +23  r412: VgaBiosReplay 闸门读到的 policyInt10Fix          ;   +22  本场真正执行了复制的次数
VBE_TR_PR_N11       EQU 27          ;   +27  r412: 11 号次数
VBE_TR_PR_M11       EQU 28          ;   +28  r412: 11 号的 BDA 0449
VBE_TR_PR_G11       EQU 29          ;   +29  r412: 11 号的 GCR06
VBE_TR_PR_S11       EQU 30          ;   +30  r412: 11 号的 SR04
VBE_TR_PR_T11       EQU 31          ;   +31  r412: 11 号结果(0/1/2)
VBE_TR_PR_N12       EQU 65          ;   +65  r412: 12 号次数
VBE_TR_PR_G12       EQU 66          ;   +66  r412: 12 号的 GCR06
VBE_TR_PR_S12       EQU 67          ;   +67  r412: 12 号的 SR04
VBE_TR_PR_T12       EQU 69          ;   +69  r412: 12 号的 BDA 0449
VBE_TR_PR_PBDA      EQU 70          ;   +70  r412: PRE 读到的 BDA 0449
VBE_TR_PR_PBXMD     EQU 73          ;   +73  r412: PRE 真正发出的模式号
VBE_TR_PR_CRTB      EQU 74          ;   +74  r412: CRTC 几何差异位图(写前)
VBE_TR_PR_CRTA      EQU 75          ;   +75  r412: 写后的差异位图(FF=本来就对)
VBE_TR_PR_CR0CB     EQU 77          ;   +77  r412: CR0C before
VBE_TR_PR_CR0DB     EQU 78          ;   +78  r412: CR0D before
VBE_TR_PR_CR0CA     EQU 19          ;   +79  r412: CR0C after
VBE_TR_PR_CR0DA     EQU 21          ;   +80  r412: CR0D after
;   【别名】这两个名字与上面 16..23 同一批字节(r412 起不再写它们)
VBE_TR_PR_RAWB      EQU 16
;   【r412】删掉 RAWT 这个别名: r209 把它唯一的写入者(字节块拷贝)改到 DbgRawT
;   之后它就没有任何使用者, 而它的 24..31 正是本轮要用的空字节
;   (slots.py 当场报了 BS3BDA 与它重叠)。
;   【r412】上面两个别名现在彻底无人写: 老 字节块拷贝 仪器已改落 DbgRawB/DbgRawT
;   (r208 时它仍在往 16..23 写 B8000 的 ASCII 字节, 只是那一直不在本机开工所以没被发现)。
VBE_TR_PR_POLICY    EQU 32          ;   +32 VxD 真正收到的 MemCFix 档位
VBE_TR_PR_AGREE     EQU 33          ;   +33 1 = 取指 32 字节 == CPU B8000 的
VBE_TR_PR_NBF       EQU 34          ;   +34 取指处非空格数(铺开采样)
VBE_TR_PR_NBV       EQU 35          ;   +35 CPU B8000 处非空格数
VBE_TR_PR_VARF      EQU 36          ;   +36 取指处字符种类数
VBE_TR_PR_VARV      EQU 37          ;   +37 CPU B8000 处字符种类数
VBE_TR_PR_TOPOFF    EQU 38          ;   +38 最佳扫描序号 0..15(偏移 = n*2000h)
VBE_TR_PR_TOPVAL    EQU 39          ;   +39 该序号处的非空格数
VBE_TR_PR_SCANN     EQU 40          ;   +40 扫描过的候选偏移个数
VBE_TR_PR_SCAND     EQU 41          ;   +41 1 = 本会话的扫描已做过
VBE_TR_PR_BOFFL     EQU 42          ;   +42 B8000 映射到的显存偏移低字节
VBE_TR_PR_BOFFH     EQU 43          ;   +43 高字节
VBE_TR_PR_C17       EQU 44          ;   +44 CR17 原文(bit6 = 字/字节寻址)
VBE_TR_PR_FETL      EQU 45          ;   +45 取指显存偏移低字节
VBE_TR_PR_FETH      EQU 46          ;   +46 高字节
; 【r166】47 号是 0..47 里唯一从未用过的字节(45/46 之后直接跳到 48)。
;   9 号(RESTORE_REGISTERS)那条官方路上, 被"CRTC 正在显示标准 mode 3 文本"
;   这条物理判据挡下的次数。它必须能失败, 否则下一次读不出"闸门是不是恒关着"
;   —— r165 的教训正是一个不能失败的仪器等于没有仪器。
VBE_TR_PR_S9GEOF    EQU 47
VBE_TR_PR_AR00      EQU 48          ;   +48..+63 AR00..AR0F(属性调色板)
VBE_TR_PR_AR10      EQU 64          ;   +64..+67 AR10..AR13
; 【r412】68 与 71 从今天起归"文本调色板"这只仪器。旧的三格 DAC 条目
;   探针(68..70 / 72..74 / 76..78)【删掉】: 那九格里住着 T12(69)/PBDA(70)/
;   PBXMD(73)/CRTB(74)/CRTA(75)/CR0CB(77)/CR0DB(78), 于是日志里
;   `dac0=0072` 读到的其实是 T12=72h, 三个"DAC 条目"从来没有真值。
VBE_TR_PR_I10N      EQU 68          ;   +68 r412: V86 INT 10h 链钩子被调用的次数(饱和)
VBE_TR_PR_I10M0     EQU 72          ;   +72 r412: 其中 AH=0(设模式)的次数(饱和)
VBE_TR_PR_I10FIX    EQU 76          ;   +76 r412: AL 72h/73h -> 03h 真正改写的次数(饱和)
VBE_TR_PR_PAL_ST    EQU 71          ;   +71 r412: 调色板/字模状态位(b0 闸门过/b1 已写/
                                    ;       b2 AR 读回对/b3 DAC0 全 0/b7 闸门拒绝)
VBE_TR_PR_FONTWIN   EQU 79          ;   +79 【r412】站住的"窗口 + 寻址方式"
VBE_TR_PR_VSML      EQU 201         ;   +201 r412: 4F03 mode low
VBE_TR_PR_VSMX      EQU 202         ;   +202 r412: 4F03 mode high
VBE_TR_PR_VSMH      EQU 203         ;   +203 r412: 4F03 return AH
                                    ;     1 = GCR06 0Ch / SR04 06h / 基址 0B8000h
                                    ;         (盒子自己的文本窗口 + 顺序寻址, 默认档)
                                    ;     2 = GCR06 0Ch / SR04 07h / 基址 0B8000h
                                    ;     3 = GCR06 00h / SR04 07h / 基址 0A0000h
                                    ;         (get_font_access() 原样, 128K)
                                    ;     4 = GCR06 04h / SR04 07h / 基址 0A0000h
                                    ;         (r176..r182 那一档, 64K)
                                    ;     5 = GCR06 08h / SR04 07h / 基址 0B0000h
                                    ;         (单色窗口, r182 的候选 3)
                                    ;     0 = 五档全败(此时 fontst 只可能是 4 或 6)
                                    ;   每一档都是"先过 8 字节记号, 再整块 8KB 回读";
                                    ;   完整判读表在 drv.c 的同名块与 R183_DELIVERY.md。
VBE_TR_PR_MEMB      EQU 80          ;   +80..+111  32 字节 @ CPU B8000
VBE_TR_PR_MEMF      EQU 112         ;   +112..+143 32 字节 @ 取指地址
VBE_TR_PR_MEMA      EQU 144         ;   +144..+175 32 字节 @ CPU A0000
; 【r184】把 176..207 这 32 字节(原来那个"显存 8000h 的 32 字节原文",
;   它从来只印 0)改成三个时刻的【活的】标准文本寄存器组。每组 7 字节:
;   SR02, SR04, GCR04, GCR05, GCR06, CR0C, CR0D —— 就是"CPU 怎么看显存、
;   卡从哪里取指"那一组。上一轮所有"硬件"读数要么是保存下来的 hi-res 快照,
;   要么是"我们写了什么"的记录; 这一组是第一次【读回来】的真值。
VBE_TR_PR_R5IN       EQU 176         ;   +176..+182 5 号进门那一刻(我们动手之前)
VBE_TR_PR_R5OUT      EQU 183         ;   +183..+189 5 号收尾之后(交给主 VDD 之前)
VBE_TR_PR_R5FIX      EQU 190         ;   +190 收尾改写用掉的次数(0..3)
VBE_TR_PR_R5OK       EQU 191         ;   +191 1 = 收尾改写后读回与文本值逐字节相同
VBE_TR_PR_R48        EQU 192         ;   +192..+198 盒子自己的 INT 10h(48 号)里读到的那一组
VBE_TR_PR_R48N       EQU 199         ;   +199 采到的 48 号样本数(上限 4, 饱和)
VBE_TR_PR_PG16       EQU 200         ;   +200..+207 收尾那一刻 B8000 起 8 个字节(页面原文)
;   【r153 追加 —— 占 208..247 这段本来就留白的空间, 块长不变】
VBE_TR_PR_NOCOVER   EQU 208         ;   +208 探针发现"当前映射不解码 B8000"
                                    ;   的累计次数(>0 说明 r153 的挪起点
                                    ;   修复在物理上不可能成立)
VBE_TR_PR_COVER     EQU 209
;   【r154 追加 —— 仍在 208..247 的留白里, 块长与版本号都不动】
VBE_TR_PR_TGONCE    EQU 210         ;   +210 1 = 本场已补过一次文本孔径组
VBE_TR_PR_TGTRIED   EQU 211         ;   +211 补写尝试次数
VBE_TR_PR_TGOK      EQU 212         ;   +212 写后读回确认映射已覆盖 B8000
VBE_TR_PR_TGBAD     EQU 213         ;   +213 写后读回发现被别人扳回去         ;   +209 1 = 当前映射解码主机 B8000
                                    ;   (00b/11b), 0 = 不解码(01b/10b)
;   【r155 追加 —— 仍在 208..247 的留白里, VBE_TR_PR_LEN 与 blob 块长都不变】
;   为什么 TGONCE 那种"整机一次"必须改成"每场一笔预算": r154 的 TGONCE
;   本意是"本场只补一次", 却从不在 4 号复位 ⇒ 实际是"整个开机只补一次"。
;   ./154 的日志: 一场里 seenText 8→83、dirty 4→34, 而 fixedN 恒为 1。
VBE_TR_PR_TGTRY     EQU 214         ;   +214 本场已用掉的补写次数(预算 8)
VBE_TR_PR_TGRUN     EQU 215         ;   +215 连续"六字节坏"的拍数
VBE_TR_PR_TGREV     EQU 216         ;   +216 上一笔补写活过的坏拍数
VBE_TR_PR_TGWATCH   EQU 217         ;   +217 1 = 正在观察上一笔的存活
VBE_TR_PR_TGRUNMAX  EQU 218         ;   +218 本场最长连续坏拍数
VBE_TR_PR_TGAACHG   EQU 219         ;   +219 A0000 窗口与上一拍不同的次数
VBE_TR_PR_TGAASAME  EQU 220         ;   +220 其中相同的次数
;   【r156】DAC 读索引自证: 修正 单个 DAC 项读取 用 3C7h(读索引)之后, 再读一次
;   3C7h 的状态寄存器; 依文档此时应是"读模式"(低两位 = 3)。
VBE_TR_PR_DACST     EQU 221         ;   +221 3C7h 状态寄存器原文
;   【r157】ATCRAW = 从 3C0h 读回的那个字节(见 vbeshare.h 的长注):
;   bit5(PAS)=1 ⇒ 旧写法无害; bit5=0 ⇒ 旧写法每次都在关视频。
VBE_TR_PR_ATCRAW    EQU 231         ;   +231 3C0h 原始读数
;   【r157】逐寄存器写入计数(不受环形记录丢弃影响)。口径: 数的是被路由到
;   我们这里(VIRTUALIZE_*_OUT)的写; 主 VDD 直接放行的写不计数。
VBE_TR_PR_W_SR01    EQU 222         ;   +222 SR01 写次数
VBE_TR_PR_W_SR02    EQU 223         ;   +223 SR02
VBE_TR_PR_W_SR03    EQU 224         ;   +224 SR03
VBE_TR_PR_W_SR04    EQU 225         ;   +225 SR04
VBE_TR_PR_W_GCR05   EQU 226         ;   +226 GCR05
VBE_TR_PR_W_GCR06   EQU 227         ;   +227 GCR06
VBE_TR_PR_W_CR0C    EQU 228         ;   +228 CR0C
VBE_TR_PR_W_CR0D    EQU 229         ;   +229 CR0D
VBE_TR_PR_W_CR17    EQU 230         ;   +230 CR17
;   【r158】HiresRestore 比较的那 11 个字节, 哪位漂了就置哪位(见 vbeshare.h):
;     bit0 SR04  bit1 GCR06  bit2 CR13  bit3 CR17  bit4 SR01  bit5 SR02
;     bit6 SR03  bit7 GCR03  bit8 GCR04 bit9 GCR05  bit10 CR14
VBE_TR_PR_HDIFFL    EQU 232         ;   +232 漂移位图 bit0..7
VBE_TR_PR_HDIFFH    EQU 233         ;   +233 漂移位图 bit8..10
;   【r160】安全网计数: 我们写进 CR0C/CR0D 的那一笔如果被证明是错的
;   (写完之后取指地址落在当前映射窗口之外 ⇒ PR_AGREE = 2), 就把原始值
;   写回去并把本场锁死, 这个字节数它执行过几次。
;   判读: >0 说明"自证成立"这条路仍然放进过一笔错的写(也就是 r160 那三道
;   新闸门还有漏), 而画面已经在同一拍被收回; 恒 0 说明闸门够用。
;   为什么要单独一个字节而不是并进 FIXBAD: 两者含义不同 ——
;   FIXBAD = 三个候选都没通过【当场】的等式; NETC = 通过了等式、事后才
;   被证明把取指挪出了窗口(./159 的形态), 混在一起就再也分不出是哪一种。
VBE_TR_PR_NETC      EQU 234         ;   +234 安全网写回原值的次数
;   【r161】把"MemC 半边"的补写挪到官方事件(9 号 RESTORE_REGISTERS)上之后
;   的四个读数。它们回答的是同一件事的三个阶段:
;     S9N   = 本场 9 号被调了几次(仪器在不在路上 —— 恒 0 就说明这条钩子
;             根本没被主 VDD 调用, 那么"挂在官方事件上"这个前提是假的);
;     S9OK  = 其中"五个不碰 ATC 的字节本来就与文本目标逐个相等"的次数
;             (主 VDD 自己恢复对了 ⇒ 我们一个字节都不写);
;     S9FIX = 因为不等而真的补写的次数(本场/累计), 它是本轮"有没有起作用"的
;             直接见证; 与被撤掉的预算(TGTRY)不同, 它没有上限 —— 判据是
;             逐字节比较, 自己会停下来;
;     S9BAD = 补写之后【当拍读回】仍不成立的次数。恒 0 = 我们的写落进了硬件;
;             >0 = 有东西把写挡回去了(陷阱/写保护), 那是另一条线索。
;   口径必须连着读: S9OK + S9FIX 应当接近 S9N(其余被"非字母数字态"挡下)。
VBE_TR_PR_S9N       EQU 235         ;   +235 本场 9 号被调次数(4 号清零)
VBE_TR_PR_S9OK      EQU 236         ;   +236 其中"本来就对"的次数(4 号清零)
VBE_TR_PR_S9FIX     EQU 237         ;   +237 真的补写的次数(累计)
VBE_TR_PR_S9BAD     EQU 238         ;   +238 补写后读回仍不对的次数(累计)
;   【r161】所有者核对的成败次数(4 号清零)。9 号给出两个所有者句柄, 而
;   "该不该由我们补 MemC 那半边"取决于 MemC 所有者到底是谁:
;     SAME = ESI(9 号 MemC 所有者) == OW_BOXVM(4 号交来的"正在切进去的那个
;            VM", DDK 原文 "EBX: Contains the handle of the VM that is being
;            switched to") ⇒ 全屏会话里两侧是同一个 VM, 补写成立;
;     DIFF = 不是它 ⇒ 主 VDD 正在为别的所有者(窗口化的 Windows / 两个伪 VM
;            之一)摆 MemC 状态, 我们【一个字节都不动】(./148 的灰屏就是
;            在那里拆了主 VDD 的台)。
;   判读: SAME 恒 0 而 DIFF 在涨 ⇒ "挂在 9 号上"这条路在本机走不通, 回落层
;   (35 号那条, 已撤节流)在扛; 那时下一轮要查的是 9 号的句柄到底是谁。
VBE_TR_PR_S9SAME    EQU 239         ;   +239 MemC 所有者 == 4 号盒子 VM
VBE_TR_PR_S9DIFF    EQU 240         ;   +240 不是它(按所有者拒写)
; --- r162: 官方那一拍上"标准 mode 3 集合"缺了哪几个字节 -------------------
;   详见 vbeshare.h 的同一段长注: 标准 mode 3 集合里 文本态修补 只漏掉
;   SR01(Clocking Mode)= 00h 这一格, 而 ./161 整份日志没有一次读到 00h。
VBE_TR_PR_SR01VAL   EQU 241         ;   +241 官方那一拍读到的 SR01 原文
VBE_TR_PR_SR01BAD   EQU 242         ;   +242 因它非 0 而补写的次数
VBE_TR_PR_GEOMASK   EQU 243         ;   +243 几何 6 字节失配位图
; --- r163: 15 号(MAKE_HARDWARE_NOT_BUSY)的三个所有者句柄 ----------------
;   依据: DDK《MAKE_HARDWARE_NOT_BUSY (Function 15)》
;     "EAX: Contains the CRTC owner's VM handle.
;      EBX: Contains the currently running VM handle.
;      ECX: Contains the MemC owner's VM handle.
;      EDX: Contains the CRTC index register (which must be saved over call)."
;     "If your hardware must complete a BLTer drawing operation before having
;      its MemC state switched from Windows hi-res mode to VGA planar mode,
;      then you must implement MAKE_HARDWARE_NOT_BUSY. ... This routine gives
;      the mini-VDD notice that a switch of the MemC state will be occurring."
;   ⇒ 15 号是官方文档里【唯一】把"MemC 状态正要从 Windows hi-res 切到 VGA
;     平面态"写成事件的钩子, 并且一次交出三个所有者句柄。r163 用它取证:
;     全屏盒子持屏期间那些 15 号调用里, 到底谁拿着 CRTC/MemC —— 这决定了
;     "会话中途把盒子态改回孔径态的那只手"是谁(r143..r162 只能靠 35 号猜)。
VBE_TR_PR_N15       EQU 244         ;   +244 15 号被调次数(模 256, 累计)
VBE_TR_PR_OWN15     EQU 245         ;   +245 最近一次 15 号的身份位图
VBE_TR_PR_OWN15A    EQU 246         ;   +246 武装期内那一次的身份位图
VBE_TR_PR_YIELD     EQU 247         ;   +247 1 = 本场补写被扳回后已认输
; r163-marker: N15/OWN15/OWN15A/YIELD fill the last four probe bytes.
;   own15 的位序(与 vbeshare.h 逐位一致):
;     bit0 EAX==WinVM  bit1 ECX==WinVM  bit2 EBX==WinVM
;     bit3 EAX==盒子VM bit4 ECX==盒子VM bit5 EBX==盒子VM
;   【r412】POST 最后一笔的"写路径"六个字节(写前/写后各一套)
;   只读取值落在这里, 不动任何旧字段。

;   【r412】SR03(=Character Map Select, 客户机自己的字符集映射)的两个读数
                                    ;        bit1 = 存下来的 SR03 != 0(卡确实在分字符集)

; ----------------------------------------------------------------
; 【r412】VBE 显示起始地址的一次性取证块(8 字节, 248..255) —— 代替 r249 的只读探针。
;   为什么需要它: r197 在 5 号这一拍用【嵌套 V86 的 Exec_Int】发
;   INT 10h AX=0003, BIOS 回来 AH=0(没做), 画面一个像素没变;
;   r242 又证明同一拍 4F03h 回 AH=0000。两把走的都是 V86 中断链,
;   主 VDD 的 INT 10h 虚拟处理器看得见、会拦。
;   DDK 给的另一把是 VMM.INC 的官方宏 VxDint:
;       push <中断号> / VMMCall Exec_VxD_Int
;   (INC_WIN98_VMM.INC:4319)。7 份商业 mini-VDD 里 ati/cirrus/s3/xga/
;   video7 都用它从 ring 0 直接问 BIOS(s3v:6174 就是 "mov eax,86h /
;   push 10h / VMMCall Exec_VxD_Int" = 设文本模式 3 且不清屏)。
;   本驱动【一次都没真实调用过它】(只有三处注释引用)。
;   本轮只发 INT 10h AH=0Fh(取当前视频状态): 不改模式、不写端口、
;   不写显存。返回 AL=模式号 / AH=列数 / BH=活动页。
VBE_TR_PR_RS_RUN   EQU 248         ;   +248 15 号取样计数(5 号清零; 到 2 时动手)
VBE_TR_PR_RS_ST    EQU 249         ;   +249 状态位图(见 RedoCo80 头注)
VBE_TR_PR_RS_AX    EQU 250         ;   +250 INT 10h 回来的 AL
VBE_TR_PR_RS_MCR   EQU 251         ;   +251 动手后 3CCh
VBE_TR_PR_RS_VBE   EQU 252         ;   +252 动手后 VBE 4F03h 的 BX 低字节
VBE_TR_PR_RS_P1    EQU 253         ;   +253 保留
VBE_TR_PR_RS_P2    EQU 254         ;   +254 保留
VBE_TR_PR_RS_P3    EQU 255         ;   +255 保留
                                    ;        5 号入口 EBX 的分类码(OWN_CLS_*)
VBE_TR_PR_LEN       EQU 256         ; r153: 32→248; r412: 248→256;
                                    ;   392+256 = 648 = 头部
VBE_TR_Recs         EQU 648         ; 64 × 24 环形记录(r147: 256→368,
                                    ;   r151: 368→392; r152: 392→424;
                                    ;   r153: 424→640)
                                    ;  (204..207 切屏状态; 208..211 DISPI
                                    ;   记账; 212..215 盒子取证; 216..231
                                    ;   r120 物理读块; 232..247 硬件指纹;
                                    ;   248..255 hi-res 记账; 256..351 六
                                    ;   个采集槽; 352..367 补写记账;
                                    ;   368..391 r151 所有权/特殊 VM;
                                    ;   392..423 r152 取指基址探针)

; VBE_TR_State 的位(与 vbeshare.h 的 VBE_TR_ST_* 一致)
VBE_TR_ST_VGA       EQU 01h         ; 硬件已被切到 VGA(4 平面/文本)通路
VBE_TR_ST_TRAPS     EQU 02h         ; ENABLE_TRAPS(13) 已生效
VBE_TR_ST_DISABLED  EQU 04h         ; DISPLAY_DRIVER_DISABLING(26) 已发生
VBE_TR_ST_VGAFIX    EQU 08h         ; 【r114】VgaTextFix 已跑过(策略非 0,
                                    ;   且装载的是 r114 的 minivdd.vxd)
VBE_TR_ST_DISPI     EQU 10h         ; 【r116】1CEh/1CFh 全局陷阱最近一次
                                    ;   是"开"的
VBE_TR_ST_DISPI_EAT EQU 20h         ; 【r116】至少吃掉过一次 DISPI I/O
                                    ;   (置起后不清零)
VBE_TR_ST_DISPI_PASS EQU 40h        ; 【r116】至少放行过一次 DISPI I/O
VBE_TR_ST_VTFBLOCK  EQU 80h         ; 【r116】VgaTextFix 被保守闸门挡下过
                                    ;   (成功执行时清零)

; VBE_TR_Evt 的【4】个字节 —— 只有 +0..+3(与 vbeshare.h 的 VBE_TR_EVT_* 一致)
;   +0..+3 = r100 的四个事件计数器, api_gettrace 用一个 dword 搬进 blob。
; 【r131 实证: +4/+5 在 blob 里不存在, 别再往这里加计数器】
;   blob 布局是 `VBE_TR_Evt EQU 4` / `VBE_TR_Counts EQU 8` —— 事件区只有
;   4 字节。r129 把 HiResClockFix 的两个计数器放在 +4/+5, 而 ring 3 读的是
;   `blob[VBE_TR_Evt+4]` = `blob[8]` = VBE_TR_Counts[0] 的低字节
;   ("0 号分派功能被调用几次")。连续两轮日志里的 `TRC evt HiResClk fix=0001`
;   因此是个幻影读数(r129 据此得出的"三个位确实被补过"必须撤回)。
;   本文件把 TraceEvt 声明成 8 字节只是为了让 VxD 内部 +4/+5 不压到 TraceRecs
;   (见下面 TraceEvt 处的注释) —— 那 8 字节【不会】全部进 blob。
;   ⇒ 需要新证据时请像 r131 那样在事件边沿【直接读硬件】。
; r131-marker: TraceEvt +4/+5 are NOT in the blob (they alias VBE_TR_Counts).
VBE_TR_EVT_POST_VGA_TO_HIRES EQU 0  ; +0 POST_VGA_TO_HIRES(7) 次数
VBE_TR_EVT_PRE_HIRES_TO_VGA  EQU 1  ; +1 PRE_HIRES_TO_VGA(4) 次数
VBE_TR_EVT_DISPLAY_DISABLING EQU 2  ; +2 DISPLAY_DRIVER_DISABLING(26) 次数
VBE_TR_EVT_PRE_VGA_TO_HIRES  EQU 3  ; +3 PRE_VGA_TO_HIRES(6) 次数
VBE_TR_EVT_HIRES_CLK_FIX     EQU 4  ; RETIRED r131 - not in the blob (aliases Counts[0])
VBE_TR_EVT_HIRES_CLK_NOP     EQU 5  ; RETIRED r131 - not in the blob (aliases Counts[1])

VBE_TREC_Seq        EQU 0           ; dd 序号(1 起)
VBE_TREC_Func       EQU 4           ; dw 分派功能号
VBE_TREC_Flags      EQU 6           ; db bit0=出口 CY(已处理)
VBE_TREC_Pad        EQU 7           ; db 置 0
VBE_TREC_Ecx        EQU 8           ; dd 入口 Client_ECX
VBE_TREC_Edx        EQU 12          ; dd 入口 Client_EDX
VBE_TREC_Ebx        EQU 16          ; dd 【r100】发起调用的 VM 句柄(分派约定)
VBE_TREC_Eax        EQU 20          ; dd 【r100】入口 Client_EAX

;-----------------------------------------------------------------------------
; W32_DEVICEIOCONTROL 的系统控制码(若 VMM.INC 未定义则补上)
;-----------------------------------------------------------------------------
IFNDEF DIOC_OPEN
DIOC_OPEN        EQU 0            ; CreateFile 打开设备时发送
ENDIF
IFNDEF DIOC_CLOSEHANDLE
DIOC_CLOSEHANDLE EQU -1           ; CloseHandle 时发送
ENDIF

;-----------------------------------------------------------------------------
; VDD_CY / VDD_NC —— mini-VDD 处理函数的返回约定
; 【为何这样返回】
;   主 VDD 调用分派表函数时不看 CPU 标志位，而是看【客户端寄存器结构】
;   (EBP 指向的 Client_Reg_Struc)里 EFlags 的进位位：
;     CY(置位) = mini-VDD 已处理，主 VDD 不必再做；
;     NC(清零) = 未处理，主 VDD 回退到它自己的实现(通常是 VESA BIOS)。
;-----------------------------------------------------------------------------
VDD_CY MACRO
        or      dword ptr [ebp].Client_EFlags, 1
ENDM

VDD_NC MACRO
        and     dword ptr [ebp].Client_EFlags, NOT 1
ENDM
