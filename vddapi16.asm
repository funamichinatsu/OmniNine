;=============================================================================
;
;  VDDAPI16.ASM — display.drv 的 16 位汇编辅助模块
;
;  【这个文件为什么存在】
;  16 位 C 编译器(MSVC 1.52 / Open Watcom wcc)的内联汇编不认识 386 的
;  32 位寄存器，而 Win9x 主 VDD 的 API 约定偏偏要用 EAX/EBX/ECX 传参。
;  因此所有"需要 32 位寄存器"或"不能信任 C 原型签名"的胶水都集中在
;  这个汇编模块里，C 侧(display.c/drv.c)保持纯 16 位可移植。
;
;  本模块包含四类东西：
;    1) DriverInit —— 整个 display.drv 的 DLL 入口(END 指令指定的
;       模块起始地址)。只做一件事：把调用转给 C 写的 DrvInitCore()。
;    2) VDD API 封装(VddAcquire/VddCall/VddRegister/VddGetDispConf)——
;       即显示驱动到主 VDD(设备 0x0A)的"第 ③ 环注册"通道。
;    3) DIB 转发器(DIBFWD/DIBTHK)—— 大部分 GDI 入口与 DIB 引擎函数
;       完全同型，直接 jmp 过去最省也最不容易错；Ext 版本多一个尾参，
;       用弹栈-压栈法插入(DIBTHK)。纯转发不进 C 的另一个原因：
;       个别函数(如 CreateDIBitmap)DDK 头文件里的原型与真实参数
;       不符，用 C 转发会因 pascal 弹栈字节数不符而破坏栈，jmp 转发
;       则原样透传，永远正确。
;       【跳转目标是动态解析的】所有 DIB 入口的地址不在链接期静态
;       导入(序号导入修正失败会让 KRNL386 无声拒载整个驱动，入口
;       都执行不到)，而是由 display.c 的 ResolveDIBEngine() 在
;       初始化时按名字(序号兜底)解析进 DGROUP 的 g_dibfn[] 表，
;       这里一律 jmp dword ptr [_g_dibfn + slot*4] 间接跳转。
;       槽位编号与 display.c 的 g_dibfnTab[] 严格一一对应，改动
;       必须两侧同步。
;    4) SetCursor 别名 —— C 侧不能与 windows.h 的 SetCursor 重名，
;       故实体叫 SetCursor_driver，这里补一个公开别名。
;
;  【寄存器保存约定】16 位 pascal 约定下 AX/BX/CX/DX/ES 可由被调方
;  破坏，但 VDD API 的实际行为没有完整文档；稳妥起见，凡是我们用到的
;  寄存器(含 EBX/ESI/EDI 高半部与 ES)一律保存 —— 参考实现 vmdisp9x
;  曾在这里踩过 BX 被 VDD 破坏的坑(见该项目 init.c 的注释)。
;
;  【汇编兼容性】纯 MASM 6 语法，同时可用 Win9x DDK 的 ML.EXE 与
;  Open Watcom 的 WASM 汇编(产出 16 位 OMF 目标文件)。
;
;=============================================================================

        .386                        ; 允许 32 位寄存器(自动加 66h 前缀)

;-----------------------------------------------------------------------------
; 段与组
; 【为何不放 _INIT】VDD API 封装在驱动整个生命周期都可能被调用
;   (Disable 时还要调 VDD_DRIVER_UNREGISTER)，放 _TEXT 常驻最稳妥；
;   体积只有几百字节，代价可忽略。(不锁代码段 —— vmdisp9x 等参考
;   驱动同样不锁, 见 display.c DrvInitCore 注释)
;-----------------------------------------------------------------------------
; 【use16 必须显式写出】上面的 .386 在段定义之前, MASM/WASM 会把它
; 之后定义的段默认成 USE32, 产出 32 位目标文件 → wlink 报
; E2064(不能与 C 模块的 16 位段合并)。显式 use16 后段保持 16 位,
; 32 位寄存器/指令仍可用(自动加 66h/67h 前缀), 功能不受影响。
_DATA   segment public use16 'DATA'

;-----------------------------------------------------------------------------
; 【extern 必须写进目标段块 —— 本文件最要紧的一条规则】
; MASM/WASM 规则：extrn 写在哪个 segment…ends 块内部，汇编器就假定
; 该符号属于哪个段，并给它的引用加上对应的段前缀。这四个数据符号
; 的 extrn 如果写在 _TEXT 块里，汇编器会把它们当成代码段变量：
;   · mov [_VDDEntryPoint],di 被编成 mov cs:[0300h],di —— 往只读
;     代码段写数据，保护模式下直接 GP 故障；即使写入成功，毁掉的
;     也是代码段中部的指令(实测 0x300 处正是 VddRegister 的指令流)；
;   · C 侧(display.c)读的是 DGROUP 里初始化为 0 的真全局变量，永远
;     读到 0 → DrvInitCore 误判"未取得 VDD 入口"→ return 0 →
;     NE 加载器报 LoadFail Failure code 0000(入口例程返回失败)。
; 写进 _DATA 块后，所有引用按 assume ds:DGROUP 生成 DS 相对寻址，
; 汇编与 C 读写的是同一块物理存储，两侧才真正同步。
;-----------------------------------------------------------------------------
        extrn   _VDDEntryPoint:dword        ; C 侧定义：VDD API 入口(16:16)
        extrn   _OurVMHandle:word           ; C 侧定义：本 VM 句柄
        extrn   _lpDriverPDevice : dword    ; C 侧定义：DIBTHK 额外尾参
        extrn   _wPalettized : word         ; C 侧定义：DIBTHK 额外尾参
        extrn   _g_dibfn:dword              ; C 侧定义：DIB 入口地址表
                                            ; (36 个 dword，ResolveDIBEngine
                                            ;  动态填充；槽位见 display.c)
        extrn   _wDibDgroup:word            ; C 侧定义：DIBENG 的 DGROUP
                                            ; 选择子(jmp 进其入口前装 AX)
        extrn   _g_dibfnReady:word          ; C 侧定义：非 0 = 地址表已填
                                            ; 真实地址(解析失败保持 0，每个
                                            ;  转发器重试；表内桩函数保底)
        extrn   _FbClearSel:word            ; C 侧定义：清屏用的帧缓冲选择子
        extrn   _FbClearBytes:dword         ; C 侧定义：清屏字节数(4 的倍数)
        extrn   _FbClearOff:dword           ; C 侧定义：清屏起始偏移(r80)
        extrn   _FbTestSel:word             ; C 侧定义：自检用的帧缓冲选择子(r82)
        extrn   _FbTestOff:dword            ; C 侧定义：自检起始偏移(r82)
        extrn   _FbTestLen:dword            ; C 侧定义：自检字节数(4 的倍数)
        extrn   _FbTestPat:dword            ; C 侧定义：自检基花样(r82)
        extrn   _CdpdInfo:dword             ; C 侧定义：CreateDIBPDevice 的 lpInfo(r84)
        extrn   _CdpdDevice:dword           ; C 侧定义：lpDevice
        extrn   _CdpdBits:dword             ; C 侧定义：lpBits(屏选择子:0)
        extrn   _CdpdFlags:word             ; C 侧定义：wFlags
        extrn   _g_traceDone:byte           ; C 侧定义：DDI 首次调用追踪标志
                                            ; (每槽 1 字节, r76c 诊断;
                                            ;  release 构建里 TRACESLOT 是
                                            ;  空函数, 只花 2 条指令)
; --- r95: 转发器的"调用方现场"LIFO 区(在 C 侧 DGROUP 里) --------------------
;   为什么不用栈: 见 DIBSAVE 宏上方注释 —— 栈上多一个字就会顶掉被调方的参数。
        extrn   _g_dibSaveIdx:word          ; C 侧定义：LIFO 深度/写指针(不回绕)
; 【r113】返回面包屑位图: 每槽 1 字节, 非 0 = 这一槽已经报告过"从引擎返回"。
; 与 _g_traceDone 严格配对(入口 vs 返回), 两边都在 DGROUP, 都显式初始化。
extrn   _g_retDone:byte
        extrn   _g_dibSaveDS:word           ; C 侧定义：每层 调用方 DS
        extrn   _g_dibSaveOff:word          ; C 侧定义：每层 调用方返回偏移
        extrn   _g_dibSaveSeg:word          ; C 侧定义：每层 调用方返回段
        extrn   _g_dibDgTrusted:word        ; C 侧定义：1 = _wDibDgroup 来自
                                            ;   已验证的装载器桩, 可以装 DS
; --- r96: DDI 上下文的环境自证 ----------------------------------------------
;   0367 段的引擎代码里有 `mov fs:[di],al` 这类 FS 相对访问, 而 16 位 C
;   读不到段寄存器 —— 只能在这里用 mov r16,Sreg 抄出来交给 C 侧落盘。
        extrn   _g_envFS:word               ; C 侧定义：最近一次探针看到的 FS
        extrn   _g_envGS:word               ; C 侧定义：同上, GS
;--- r97: 引擎"非 Ext"入口的 EDX 协议(见 DIBJUMP 里的长注释) ---------------
        extrn   _g_runEdx:word              ; C 侧定义：1 = 远跳前把设备指针装进 EDX
        extrn   _g_edxCount:word            ; C 侧定义：注入计数(日志自证用)
        extrn   _g_vddEdxValue:dword        ; r99: REGISTER 的 EDX 策略值(0 / -1)
;--- r98: 跳板插入的那个尾部额外参数的【实际取值】(诊断自证用) --------------
;   为什么必须落盘: 槽 20..26 的尾参是 DGROUP 里的变量(_lpDriverPDevice /
;   _wPalettized), 而 r98 的定案是"读它的时候 DS 还是调用方的" —— 于是被
;   插进参数帧的是【调用方数据段里同一偏移处的内容】。把插进去的值原样
;   打出来, 就能一眼分辨:
;     · 值 = 我们的 PDEV(2056:0000 之类) → DS 本来就是本模块的;
;     · 值 = 别的选择子:别的偏移           → DS 不是本模块的(r98 病灶);
;   两个变量由 TRACESLOTMAC 在"首次调用"那一刻从 param 抄进来。
        extrn   _g_thunkPOff:word           ; C 侧定义：插入尾参的低字
        extrn   _g_thunkPSeg:word           ; C 侧定义：插入尾参的高字

;======= 诊断探针数据(配合 DriverInit 里的探针代码；定位完成后整体删除) =======
        probePath  db 'C:\OMNININE.LOG', 0
        probePath2 db '\OMNININE.LOG', 0   ; 引导盘根目录(当前盘), 兜 C: 失败
        probeMsg  db 'PROBE: DriverInit entry reached', 0Dh, 0Ah
; 【r152】把构建标记也放进【无闸门】的探针里。
;   ./151 的日志头缺了五行(DrvInitCore: enter / BUILD R151 identity /
;   VBE9X build tag r151 / PH: build tag==0151 / ok 0: ...), 而紧随其后的
;   "ok 1: VddAcquire" 在 —— 因为闸门②(DOS 的 InDOS 标志)在开机窗口里
;   连续 8 次读到忙, r103 的自愈把它前面那七行全丢了(r103 的设计就是
;   "连续 8 次忙就永久停用第②道", 第 8 次放行)。日志里
;   `LOG skip InDOS=0008` 且此后再不增长, 正是这个数。
;   这是已知且无害的竞态, 但它恰好抹掉"到底部署了哪一版"的唯一证据。
;   探针不经过任何闸门(见上面注释① ② ③), 所以构建标记放这里最稳:
;   即使整条 DIAG 通道在开机窗口被闸门挡掉, 这一行也一定落盘。
        probeMsg2 db 'VBE9X build tag r447 (vddapi16 probe line; keep in sync with VBE9X_TAG)', 0Dh, 0Ah
PROBE_MSG_LEN equ $ - probeMsg
;======= DiagWriteLog 用的路径(与探针同机制, 纯 INT 21h, 见该函数注释) ======
        logPathC    db 'C:\OMNININE.LOG', 0
        logPathRoot db '\OMNININE.LOG', 0   ; 引导盘根目录兜底
        logCrlf     db 0Dh, 0Ah

; [r412] 只加"每秒行数额度": 写入路径保持 r412 原样(已证明能写出干净日志)
s_logBudget dw 32
; [r447] 日志缓冲: DGROUP 内 8KB 静态数组(见 s_logBase / _g_logBuf)
s_logCanary dw 05A5Ah          ; [r412] 缓冲末尾金丝雀(应为 5A5A)
s_logN      dw 0
s_logBytes  dw 0              ; [r412] 已用字节
s_logBase   dw 0                  ; [r447] C 侧静态缓冲在 DGROUP 内的偏移
s_bootFlush dw 0                  ; [r447] 1 = 本次落盘是开机阶段 flush
s_logRec    dw 0              ; [r412] 已缓冲条数
s_fbIdx     dw 0
s_fbHandle  dw 0              ; [r412] 文件句柄(循环里 BX 会被占用, 必须另存)              ; [r412] flush 游标
s_logDrop   dw 0              ; [r412] 因满丢弃条数
s_logTickLast dw 0           ; [r412] 上次补额度时的 BIOS tick(低字)
s_logUsed   dw 0

        szVddGetDispConf db 'VDDGETDISPCONF: calling main VDD', 0
;======= r102: 日志通道闸门数据 ==============================================
;   【为什么要闸门】DIAGWRITELOG 每行 6 次 INT 21h。真机冻结(b0/b1)的
;   日志尾部正好停在"光标路径连续落盘"的那一段, 而同一时刻主 VDD 正在
;   做 VGA 文本屏的保存/恢复(日志尾部那批 edx=0000B800 的记录)——
;   也就是"DOS 临界区 + 我们正在做 DOS 文件 I/O"的重叠窗口。
;   三道闸门全部只读内存/写内存, 绝不发 INT 21h:
;     ① _g_logBlocked  : C 侧 VxdSetInCallback 置位的主 VDD 回调窗口
;                        (drv.c: RestoreDesktopMode 入口置 1, 出口清 0)
;     ② _DiagInDosOff/_DiagInDosSeg : DOS 的 InDOS 标志远地址
;                        (首次落盘时用 INT 21h AH=34h 取, 见 DLOGGUARD)
;     ③ _DiagInLog     : 本过程自身的重入计数
;   被挡下的行数记进三个 C 全局, 由 drv.c 的 VxdTraceDump 落盘 ——
;   "闸门一次都没拦到东西"本身就是要看的结论。
        extrn   _g_logBlocked:word
        extrn   _LogBufSel:word
extrn   _LogReady:word            ; [r447] 1 = 已进入 DDI, 允许常规落盘
extrn   _DiagSkipInDOS:word
        extrn   _DiagSkipReent:word
        extrn   _DiagSkipCb:word
; 【r166】日志总闸: C 侧按构建宏把它初始化成 1(debug/MIN)或 0(release)。
;   它挡在 DLOGGUARD 的【最前面】, 于是 release 档既不做探针的建文件,
;   也不做任何一行落盘 —— "release 不写任何日志文件"从运行期指望变成
;   编译期事实。见 R166_DELIVERY.md 第二节。
        extrn   _g_logEnable:word
        _DiagInDosOff   dw 0        ; InDOS 标志偏移  ┐ 两字都为 0 = 尚未取得
        _DiagInDosSeg   dw 0        ; InDOS 标志段    ┘ (0 是合法值, 故以"或"判)
        _DiagInLog      dw 0        ; 本过程重入计数
;======= r103: 第②道闸门的自愈数据 ==========================================
;   【r102 的病】r102 的两次 debug 会话日志都只有 PROBE 一行(r101 同构建
;   有 6027 行) —— 日志通道在第一次 DIAG 调用就被永久锁死。两个原因:
;     ① DIAGWRITELOG/DIAGFLUSH 的出口【无条件】dec _DiagInLog: 被闸门
;        拦下的那次并没有 inc 过, 却照样 -1 ⇒ 计数变成 0xFFFF ⇒ 闸门③
;        "≠0 即忙" 从此永远成立(已修: 只有登记过才释放);
;     ② InDOS 标志在 Win9x 的 System VM 里经常【长期非 0】(DLL 入口就是
;        从 DOS/VMM 上下文里进来的, 而 DOS 的 InDOS 计数一旦被谁漏减就
;        再也不归零) ⇒ 闸门② 把每一行都拦掉, 而且拦得"正确"到无法察觉。
;   自愈: 连续 8 次读到"忙"就判定该标志不可用, 从此【永久停用第②道】
;   (只保留 ①回调窗口 ②自身重入 两道纯内存闸门), 并把这件事记进
;   _DiagSkipInDOS/_DiagInDosVal —— 下一行成功的日志就会把它打出来。
;   _DiagInDosVal = 第一次读到的字节值(0FFFFh = 从未读到), 用来判"一上来
;   就是非 0"(指针无效或计数泄漏)还是"偶尔非 0"(正常的 DOS 临界区)。
;   【必须 extrn, 不能在两侧各定义一份】这三个量 C 侧(VxdTraceDump)要读,
;   片内局部定义会让 C 读到另一个永远不动的存储单元 —— 正是本项目吃过
;   多次亏的"静默死开关"。与 _DiagSkip* 完全同一套路。
        extrn   _DiagInDosOk:word
        extrn   _DiagInDosVal:word
        extrn   _DiagInDosBusy:word
DLG_BUSY_MAX    equ 8               ; 连续这么多次"忙"就停用第②道
;======= EmuInt86 核心的静态暂存(DGROUP 寻址, 单线程安全) ============
        _eiRp       dw 0            ; EI_core: rp 暂存
        _eiSp       dw 0            ; EI_core: sp 暂存(0=无)
;==============================================================================

_DATA   ends

DGROUP  group   _DATA

_TEXT   segment public use16 'CODE'
        assume  cs:_TEXT, ds:DGROUP, es:nothing

; C 侧的初始化主体(pascal far，无参数，返回 AX=成功标志)
; (far 符号不受上述段假定影响：远调用本身带段地址)
        extrn   DRVINITCORE:far
; C 侧的 DIBENG 懒解析(pascal far，无参，返回 AX=1 成功)——转发器
; 首次被调用且 _g_dibfnReady=0 时触发，见下方 DIBTHK/DIBFWD/CDIB 宏。
        extrn   DIBENSURERESOLVED:far
; C 侧的 DDI 首次调用追踪(r76c 诊断; pascal far, 一个 WORD 槽号参数,
; 被调方 ret 2 清参)。release 构建里函数体为空, 调用代价 2 条指令。
        extrn   TRACESLOT:far
; 【r113】C 侧的 DDI 【返回】面包屑(pascal far, 一个 WORD 槽号参数)。
; 由 DIBSTUB 在"被调方已返回、DS 已钉回本模块"之后调用, 每槽一次 ——
; 它把"崩溃发生在引擎调用内部还是返回之后"变成日志最后两行可读的事实。
        extrn   TRACESLOTRET:far

;-----------------------------------------------------------------------------
; TRACESLOTMAC —— 每个转发器【首次】被调用时记一行日志(r76c)
;   槽号 0..35 对应 display.c 的 g_dibfnTab[](0=DIB_BitBlt, 2=DIB_Control,
;   14=DIB_ExtTextOut, 19=DIB_Inquire, ...); 日志里会打出 DIBENG 的
;   导出名, 于是"0D 蓝屏落在别的模块内部某个偏移"时, 最后一行日志
;   直接指出是哪个 DDI 入口进去之后死的 —— 这是把故障坐标从
;   "GDI/DIBENG +0x2B70" 变成"某个具体调用"的唯一手段。
;   已记录过则只花 cmp/jne 两条指令。
;   【为什么必须保存 ES】DIBTHK 的约定是"DS/ES 都指向我们的数据段",
;   而插进去的额外尾参是 DGROUP 里的 near 变量(如 _lpDriverPDevice),
;   读它靠的就是 DS(r98 定案: 老代码在装 DS 之前就读了它 —— 那就是病灶)。
;   TraceSlot 是 C 函数，按 Watcom 的 16 位 ABI 它可以破坏 AX/BX/CX/DX/ES ——
;   漏保 ES 就等于把额外尾参读成别的东西。所以这里把 AX/BX/CX/DX/ES
;   全部保护(只发生在每槽第一次, 之后的调用只剩 cmp/jne)。
;
;   【r98 新增: 第二个(可选)参数 = 尾部额外参数】带尾参的槽(DIBTHK, 20..26)
;   在"首次调用"这一刻把【真正插进参数帧的那两个字节】抄进 DGROUP, 由
;   C 侧 TraceSlot 落盘。不带尾参的槽(DIBFWD/DIBXW)显式写 0, 免得日志里
;   出现上一槽的残留值 —— 读的是同一个 DGROUP, 两个宏都写才自洽。
;   【为什么在宏里读而不是在 C 里读】插进去的是"调用方 DS 下的同偏移内容",
;   只有在这里(装好 DS 之后、跳进引擎之前)才拿得到同一份值。
;-----------------------------------------------------------------------------
TRACESLOTMAC macro slot, param
        cmp     byte ptr [_g_traceDone + slot], 0
        jne     @F
        mov     byte ptr [_g_traceDone + slot], 1
        IFNB <param>
        mov     ax, word ptr [param]            ; 尾参低字(远指针 = 偏移)
        mov     [_g_thunkPOff], ax
        mov     ax, word ptr [param+2]          ; 尾参高字(远指针 = 选择子;
        mov     [_g_thunkPSeg], ax              ;  槽 22 是 WORD, 该字是相邻变量, 只记录)
        ELSE
        mov     word ptr [_g_thunkPOff], 0
        mov     word ptr [_g_thunkPSeg], 0
        ENDIF
        push    ax
        push    bx
        push    cx
        push    dx
        push    es
        mov     ax, slot
        push    ax
        call    TRACESLOT
        pop     es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
@@:
        endm

;=============================================================================
; DriverInit —— DLL 入口(NE 头中的起始地址)
;
; 16 位 DLL 加载时，KERNEL 远调用此入口，约定：
;   CX = 堆大小, DI = 模块句柄, ES:SI = 命令行, DS = DGROUP(已设好)
;   返回 AX != 0 表示加载成功
;
; 【为何不初始化局部堆】标准 LIBENTRY 会做 LocalInit 建立 DLL 局部堆，
; 本驱动不使用任何 LocalAlloc，故跳过 —— 少一个失败点。
; 所有初始化决策(读配置、枚举 VBE 模式、向 VDD 注册)都在 DrvInitCore
; 里用 C 完成，这里只做透明的远调用转发。
;=============================================================================
        public  DriverInit
DriverInit proc far
;======= 诊断探针(裁决"入口是否被调用"；定位完成后整段删除) ==============
; 不经过任何 C 代码、不依赖调用者设置的 DS，直接在入口建日志文件并写
; 一行标记。三种结果：
;   ① 有文件、有 PROBE 行、后面还有 "DrvInitCore: enter" 等行
;      → 入口被调用且 C 侧活着，看日志最后一行定位失败点；
;   ② 有文件但只有 PROBE 一行
;      → 入口被调用，但 DrvInitCore 没执行到(入口→C 之间出问题)；
;   ③ 根本没有文件
;      → 入口从未被调用，加载器在 NE 层就拒载了(查导入表/段表)。
;========================================================================
        push    ax
        push    bx
        push    cx
        push    dx
        push    ds

        mov     ax, DGROUP          ; 强制 DS=DGROUP，排除 DS 未设好的可能
        mov     ds, ax
        ; 【r166】闸门 0: release 档连文件都不建(见 R166_DELIVERY.md 第二节)
        cmp     word ptr [_g_logEnable], 0
        je      probe_out

        ; 先清掉上次运行可能残留的只读属性 (AX=4301h CX=0, 失败无碍)
        mov     ds, ax
        mov     ax, 4301h
        xor     cx, cx
        mov     dx, offset DGROUP:probePath
        int     21h

        mov     ah, 3Ch             ; 建文件(存在则截断重建)
        xor     cx, cx
        mov     dx, offset DGROUP:probePath
        int     21h
        jnc     probe_got           ; C: 建成功
        ; C: 建失败(盘不存在/满/只读) → 改试引导盘根目录 "\OMNININE.LOG"
        mov     ax, 4301h
        xor     cx, cx
        mov     dx, offset DGROUP:probePath2
        int     21h
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, offset DGROUP:probePath2
        int     21h
        jc      probe_out           ; 两处都建不了则放弃(结果③)
probe_got:
        mov     bx, ax              ; BX = 文件句柄
        mov     ah, 40h             ; 写探针标记
        mov     cx, PROBE_MSG_LEN
        mov     dx, offset DGROUP:probeMsg
        int     21h
        mov     ax, 6800h           ; commit 推落盘(close 不做这件事, r44 实锤)
        int     21h                 ; (DOS4+; 不支持则 CF=1, 无害)
        mov     ah, 3Eh             ; 关文件
        int     21h
probe_out:
        pop     ds
        pop     dx
        pop     cx
        pop     bx
        pop     ax
;======= 诊断探针结束 =====================================================

        call    DRVINITCORE         ; 返回值已在 AX
        ret
DriverInit endp

;-----------------------------------------------------------------------------



;=============================================================================
; DiagWriteLog —— C 侧诊断日志的汇编实现(display.c 的 DIAG 宏最终调到这)。
;   C 原型: void __far __pascal DiagWriteLog( const char __near *msg )
;   pascal far 约定: [bp+6] = msg 偏移(DS=DGROUP 由 C 调用方保证)。
;
; 【为什么不用 clibm int86】引导期实测(./5 构建): 探针用同样几条
;   INT 21h(4301/3C/40/3E)能稳定建文件写行, 而 C 侧经 clibm int86 的
;   同一组调用全部静默失败(日志只有 PROBE 一行)——int86 链路里
;   的 segread_/int86x_/_DoINTR_ 在此阶段有未查明的问题。日志通道
;   是排障的命根子, 直接换成与探针逐字节等价的纯汇编 INT 21h,
;   把 int86 从引导路径上彻底摘出去。
;   每条日志: 打开(不存在则创建)→ 移到文件尾(追加) → 写正文 → 写
;   CRLF → commit(AX=6800h, 推落盘) → 关闭。C: 失败回退引导盘根目录,
;   与探针一致。
;   【r45】close 并不把 VFAT 卷缓存推落盘(r44 实锤: 硬死机后整份日
;   志连同建文件一起丢); r43 日志能幸存纯属 AH=0Dh 副作用。故每行自
;   带 DOS4+ commit, 不支持则 CF=1 无害。
;
; 【r102: 为什么必须有三道闸门(真机冻结的根因修复)】
;   ./100 的 x230 b0/b1 两次"全屏冻结"都发生在: 没有任何 DOS 程序在跑,
;   鼠标能动、画面正常、不蓝屏, 但系统再也不响应; 而 BOOTLOG.freeze 与
;   正常 BOOTLOG.TXT 的【前 1014 行逐字节相同】, 说明 VMM 自己的落盘也
;   停在同一处。两次的日志尾部形态一致: 光标三件套连续落盘(每行 6 次
;   INT 21h), 紧接着就是主 VDD 在做 VGA 文本屏的保存/恢复(那批
;   edx=0000B800 / eax=00000720 / ecx 步长 800h 的记录)。
;   ⇒ "我们正在 DOS 文件 I/O" 与 "主 VDD 正在切屏/切换 VM" 撞在一起。
;     从 ring 0 发 INT 21h 本身就不合规: 一旦那一刻 DOS 已在调用中
;     (InDOS 置位)或正被挂起/切换, 再进 DOS 就是经典的死锁形态 ——
;     表现恰好是"系统整体不动、画面停在最后一帧、鼠标由 VDD 继续搬"。
;   三道闸门全部只读/写内存, 绝不发 INT 21h:
;     ① 回调窗口: _g_logBlocked(由 drv.c 的 VxdSetInCallback 维护,
;        RestoreDesktopMode 入口置 1、出口清 0) —— 那一整段运行在主 VDD
;        自己的切屏事务内部, 是绝对禁区;
;     ② DOS 临界区: InDOS 标志(远地址由首次落盘时的 INT 21h AH=34h 取得,
;        取不到就跳过这道闸门, 不改变老行为);
;     ③ 自身重入: _DiagInLog。
;   三条闸门都只在【入口】判断一次, 因此被挡下的那行连 open 都不做。
;   挡下的行数记进 _DiagSkip* 三个 C 全局(drv.c 定义), 由 VxdTraceDump
;   在安全上下文落盘 —— 这样"闸门是否真的拦到过东西"是可直接读的结论。
;
;   【r103: r102 的闸门把自己锁死了 —— 两处修正】
;   ① 出口的 dec 必须配对。r102 的 DLOG_out 无条件 dec _DiagInLog,
;      而被闸门拦下的那条路径【从没 inc 过】⇒ 计数 -1 = 0xFFFF ⇒
;      第③道闸门 "cmp ... ,0 / jnz busy" 从此永远成立 ⇒ 日志永久死亡。
;      实拍: ./102 的 debug 构建两份会话日志都只剩 PROBE 一行(r101 同
;      构建有 6027 行); release 构建因为首个 DIAG 调用在更晚的
;      VDDGETDISPCONF, 反而漏出 2 行 —— 这个"构建间差异"本身就是
;      "计数器被前一次调用污染"的指纹。
;      现在: 只有 DLOGGUARD 返回非 0(登记过重入)的路径才执行 dec。
;   ② 第②道(InDOS)必须能自愈。AH=34h 取到的那个字节在 Win9x 里经常
;      长期非 0(计数被谁漏减就永不归零), 于是每一行都被"正确地"拦掉,
;      而且日志里没有任何一行说明这件事。现在连续 8 次读到"忙"就判定
;      该标志不可用, **永久停用第②道**, 并把首次读到的值记进
;      _DiagInDosVal(0FFFFh = 从未取到) —— 由 VxdTraceDump 的
;      "LOG gate InDOS ok/val/busy" 三行落盘作证。
;      剩下的 ①(主 VDD 回调窗口, 我们自己的哨兵)与 ③(自身重入)都是
;      纯内存判断, 100% 可靠, 冻结防护的主体仍在。
;=============================================================================
        public  DIAGWRITELOG
DIAGWRITELOG proc far
        push    bp
        mov     bp, sp
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        mov     ax, DGROUP
        mov     ds, ax
        ; [r447] 追加只是"写内存", 不需要 DLOGGUARD;
        ;   只有真正落盘(DIAGFLUSH/DIAGFLUSHBUF)才要过 InDOS/回调闸门。
        ;   以前在这里调 DLOGGUARD, 整场被拒 => 缓冲里 0 字节(439 的日志形态)。
        cmp     word ptr [_g_logEnable], 0
        je      DLOG_skip
        cmp     word ptr [_LogBufSel], 0
        je      DLOG_skip
        mov     si, [bp+6]          ; SI = msg
        ; --- 长度: repne scasb ---
        mov     di, si
        push    ds
        pop     es
        mov     cx, 0FFFFh
        xor     al, al
        cld
        repne   scasb
        not     cx
        dec     cx                  ; cx = 正文长度
        ; --- 空间检查 ---
        mov     ax, [s_logBytes]
        add     ax, cx
        add     ax, 2
        cmp     ax, 8128
        ja      LOGB_drop
        ; --- 追加到 DGROUP:offset ---
        push    ds
        mov     ax, DGROUP
        mov     es, ax
        mov     di, [s_logBytes]
        add     di, word ptr [s_logBase]
        push    cx
        cld
        rep     movsb
        mov     al, 0Dh
        stosb
        mov     al, 0Ah
        stosb
        pop     cx
        pop     ds
        add     word ptr [s_logBytes], cx
        add     word ptr [s_logBytes], 2
        inc     word ptr [s_logRec]
        jmp     DLOG_out
LOGB_drop:
        inc     word ptr [s_logDrop]
DLOG_out:
DLOG_skip:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     bp
        ret     2
DIAGWRITELOG endp

;-----------------------------------------------------------------------------
; [r447] DIAGSETLOGBASE —— C 侧把静态缓冲偏移交进来
;   C 原型: void __far __pascal DiagSetLogBase( unsigned short off )
;-----------------------------------------------------------------------------
        public  DIAGSETLOGBASE
DIAGSETLOGBASE proc far
        push    bp
        mov     bp, sp
        push    ax
        mov     ax, DGROUP
        mov     ds, ax
        mov     ax, word ptr [bp+6]
        mov     word ptr [s_logBase], ax
        pop     ax
        pop     bp
        ret     2
DIAGSETLOGBASE endp

;-----------------------------------------------------------------------------
; [r412] DIAGGETDROP —— 返回"因缓冲满而丢弃"的条数(C 侧周期打印)
;-----------------------------------------------------------------------------
        public  DIAGGETDROP
DIAGGETDROP proc far
        mov     ax, [s_logDrop]
        ret
DIAGGETDROP endp

;-----------------------------------------------------------------------------
; [r412] DIAGFLUSHBUF —— 把内存缓冲一次性落盘(安全点调用)
;   只在"顶层 DDI 且非重入"时由 C 侧调用; 失败时保留缓冲(不丢信息).
;-----------------------------------------------------------------------------
        public  DIAGFLUSHBUF
DIAGFLUSHBUF proc far
        push    bp
        mov     bp, sp
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        cmp     word ptr [_LogBufSel], 0
        je      FB_out
        cmp     word ptr [s_logBytes], 0
        je      FB_out
        ; [r447] 驱动初始化阶段禁止 INT 21h 落盘(DOS 尚未就绪, 实测 debug 版 0D)
        cmp     word ptr [_LogReady], 0
        jne     short FB_rdy
        ; [r447] 开机阶段唯一允许的落盘: DIAGFLUSHBOOT 置位时放行
        cmp     word ptr [s_bootFlush], 0
        je      FB_out
FB_rdy:
        ; [r447] 重入/回调期间也不落盘
        cmp     word ptr [_DiagInLog], 0
        jne     FB_out
        mov     dx, offset DGROUP:logPathC
        call    DLOGOPEN
        test    ax, ax
        jnz     FB_have
        mov     dx, offset DGROUP:logPathRoot
        call    DLOGOPEN
        test    ax, ax
        jz      FB_out
FB_have:
        mov     bx, ax
        cmp     bx, 2
        jbe     FB_out
        mov     word ptr [s_fbHandle], bx
        mov     ax, DGROUP
        mov     ds, ax
        mov     bx, word ptr [s_fbHandle]
        mov     ax, 4202h
        xor     cx, cx
        xor     dx, dx
        int     21h
        mov     cx, [s_logBytes]
        cmp     cx, 8128
        jbe     FB_lenok
        mov     cx, 8128
FB_lenok:
        mov     bx, word ptr [s_fbHandle]
        mov     dx, word ptr [s_logBase]
        mov     ax, 4000h
        int     21h
        mov     bx, word ptr [s_fbHandle]
        mov     ax, 6800h
        int     21h
        mov     bx, word ptr [s_fbHandle]
        mov     ax, 3E00h
        int     21h
        mov     word ptr [s_logBytes], 0
        mov     word ptr [s_logRec], 0
FB_out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     bp
        ret
DIAGFLUSHBUF endp

;-----------------------------------------------------------------------------
; [r447] DIAGFLUSHBOOT —— 开机阶段的"阶段落盘"(积压 >= 1KB 才写)
;-----------------------------------------------------------------------------
        public  DIAGFLUSHBOOT
DIAGFLUSHBOOT proc far
        cmp     word ptr [s_logBytes], 1024
        jb      DFB_ret
        mov     word ptr [s_bootFlush], 1
        call    DIAGFLUSHBUF
        mov     word ptr [s_bootFlush], 0
DFB_ret:
        ret
DIAGFLUSHBOOT endp

;-----------------------------------------------------------------------------
; [r412] DIAGFLUSHTHRESH —— 仅当缓冲已用 >= 4KB 时才落盘(r412 起; 原 4KBB)
;-----------------------------------------------------------------------------
        public  DIAGFLUSHTHRESH
DIAGFLUSHTHRESH proc far
        cmp     word ptr [s_logBytes], 4096    ; [r412] 16KB->4KB: 压掉开机突发丢弃
        jb      DTH_ret
        jmp     DIAGFLUSHBUF
DTH_ret:
        ret
DIAGFLUSHTHRESH endp


;-----------------------------------------------------------------------------
; DiagFlush —— 里程碑落盘: 打开日志 → DOS4+ commit(AX=6800h) → 关闭。
;   【r45 教训】close 并不把 VFAT 卷缓存推落盘: r43 日志能活到死点
;   纯属 AH=0Dh 副作用; r44 去掉 flush 后, 硬死机把整份日志(连建文
;   件)一起带走(./44)。AH=0Dh 本身是否有罪未决(43/44 表现都与"加载
;   期 EnsureModeTable 直通枚举硬死机"兼容), 不再用它; 改用文件级
;   AX=6800h commit —— 不支持则 CF=1 无害。DiagWriteLog 每行已自带
;   commit, 本函数给 FLUSH() 里程碑语义兜底。
;   C 原型: void __far __pascal DiagFlush( void )
;   【r102 闸门】与 DIAGWRITELOG 同一批闸门(回调窗口 / DOS 临界区 / 重入)。
;   被挡下时直接返回: flush 是"把已有的行催落盘", 丢一次没有任何信息损失。
;-----------------------------------------------------------------------------
        public  DIAGFLUSH
        public  DIAGREFILL
; [r412] DLOGREFILL: 读 BIOS 计时器(0040:006C), 每 18 tick(约1秒)补满额度。
DLOGREFILL:
; [r412] 读 BIOS 计时器(0040:006C)并每 18 tick 补满额度。
;   r342 的致命错: 压栈 5 次(push ds/es/ax/bx/es) 只弹 4 次,
;   每次写日志栈都长 2 字节 -> 早期 #NP(0B) 落在 DIBENG 段。
        push    ds
        push    es
        push    ax
        push    bx
        mov     ax, 40h
        mov     es, ax
        mov     bx, es:[6Ch]
        mov     ax, DGROUP
        mov     ds, ax
        mov     ax, bx
        sub     ax, [s_logTickLast]
        cmp     ax, 18
        jb      DRF_out
        mov     [s_logTickLast], bx
        mov     word ptr [s_logBudget], 32
DRF_out:
        pop     bx
        pop     ax
        pop     es
        pop     ds
        retn

DIAGREFILL proc far
        push    ds
        push    ax
        mov     ax, DGROUP
        mov     ds, ax
        mov     word ptr [s_logBudget], 32
        pop     ax
        pop     ds
        retf
DIAGREFILL endp

DIAGFLUSH proc far
        push    bx
        push    dx
        push    ds
        mov     ax, DGROUP          ; DS=DGROUP(DLOGOPEN 与路径偏移依赖)
        mov     ds, ax
        push    es                  ; (DLOGGUARD 自取 InDOS 段, 不依赖调用方 ES;
        push    ds                  ;  这两条只是保持"ES 也指向 DGROUP"的
        pop     es                  ;  老约定, 零风险)
        call    DLOGGUARD
        pop     es
        test    ax, ax
        jz      DF_skip             ; 闸门关着: 不做任何 INT 21h(也没登记重入)
        ; [r447] 与 DIAGFLUSHBUF 同一门槛: 首次 DDI 之前绝不落盘
        cmp     word ptr [_LogReady], 0
        je      DF_out
        mov     dx, offset DGROUP:logPathC
        call    DLOGOPEN
        test    ax, ax
        jnz     DF_have
        mov     dx, offset DGROUP:logPathRoot
        call    DLOGOPEN
        test    ax, ax
        jz      DF_out
DF_have:
        mov     bx, ax              ; BX = 句柄
        mov     ax, 6800h           ; commit: 本文件缓存推落盘
        int     21h
        mov     ax, 3E00h           ; 关闭
        int     21h
DF_out:
        dec     word ptr [_DiagInLog]   ; 释放重入计数(仅"登记过"的路径到这里)
DF_skip:
        pop     ds
        pop     dx
        pop     bx
        retf
DIAGFLUSH endp

;-----------------------------------------------------------------------------
; DLOGGUARD —— r102 日志闸门(近程): 出口 AX != 0 = 放行(已登记重入), 0 = 丢弃。
;   入口: DS = DGROUP。破坏: AX/BX/CX/DX。除"首次取 InDOS 远地址"那一次
;   (INT 21h AH=34h)之外, 全部只读内存 —— 因此"丢弃"这条路径绝不会再进 DOS。
;   【为什么必须在这里做】见 DIAGWRITELOG 头部的长注释: 从 ring 0 发 INT 21h,
;   一旦撞上"DOS 已在调用中"或"主 VDD 正在切屏/切 VM", 就是整体死锁。
;   三道闸门的顺序按"代价从低到高 + 危险度从高到低"排:
;     ① 回调窗口(1 次内存读) —— 主 VDD 的切屏事务内部, 绝对禁区;
;     ② InDOS 标志(1 次远内存读) —— DOS 临界区中(取不到地址就跳过这道);
;     ③ 自身重入(1 次内存读) —— 上一次落盘还没跑完。
;-----------------------------------------------------------------------------
DLOGGUARD:
        ; 【r166】闸门 0: 日志总闸(C 侧按构建宏置 1/0)。放在最前面 ——
        ;   它比"回调窗口"还便宜(一次内存读), 而且它是唯一一个能让 release
        ;   档【一个文件都不建】的闸门: 探针与每一行落盘都要经过这里。
        cmp     word ptr [_g_logEnable], 0
        jne     DLG_on
        xor     ax, ax
        retn
DLG_on:
        cmp     word ptr [_g_logBlocked], 0
        jz      DLG_notcb
        inc     word ptr [_DiagSkipCb]
        xor     ax, ax
        retn
DLG_notcb:
        cmp     word ptr [_DiagInDosOk], 0
        je      DLG_reent           ; 第②道未取得/已停用: 直接到最后一道
        mov     bx, [_DiagInDosOff]
        mov     ax, [_DiagInDosSeg]
        or      ax, bx
        jnz     DLG_havedos         ; 两个都为 0 = 还没取过
        push    ds
        push    si
        mov     ax, 3400h           ; INT 21h AH=34h → ES:BX = InDOS 标志远址
        int     21h                 ; 只可能在首次那一行执行一次
        pop     si
        pop     ds
        mov     [_DiagInDosOff], bx
        mov     [_DiagInDosSeg], es
        push    es                  ; 段寄存器不能直接做 or 的源操作数
        pop     ax
        or      ax, bx
        jz      DLG_nodos           ; 取不到: 永久停用第②道, 行为退回 r101
        mov     word ptr [_DiagInDosOk], 1
DLG_havedos:
        push    ds
        mov     ds, [_DiagInDosSeg]
        mov     bx, [_DiagInDosOff]
        mov     al, [bx]
        pop     ds
        cmp     word ptr [_DiagInDosVal], 0FFFFh
        jne     DLG_haveval
        mov     ah, 0                   ; 记下"第一次读到的值"(高位置 0)
        mov     [_DiagInDosVal], ax
DLG_haveval:
        test    al, al
        jz      DLG_idle
; --- InDOS 置位: 计一次, 并在连续多次后判定该标志不可用(r103 自愈) -------
        inc     word ptr [_DiagSkipInDOS]
        mov     ax, [_DiagInDosBusy]
        inc     ax
        mov     [_DiagInDosBusy], ax
        cmp     ax, DLG_BUSY_MAX
        jb      DLG_blockdos
        mov     word ptr [_DiagInDosOk], 0  ; 连续 8 次都"忙" ⇒ 标志不可用
        jmp     DLG_reent                   ; 这一次放行(也让证据落盘)
DLG_blockdos:
        xor     ax, ax
        retn
DLG_idle:
        mov     word ptr [_DiagInDosBusy], 0
DLG_reent:
        cmp     word ptr [_DiagInLog], 0
        jnz     DLG_busy
        mov     word ptr [_DiagInLog], 1
        mov     ax, 1
        retn
DLG_busy:
        inc     word ptr [_DiagSkipReent]
        xor     ax, ax
        retn
DLG_nodos:
        mov     word ptr [_DiagInDosOk], 0  ; 取不到远址: 永久停用第②道
        jmp     DLG_reent

;-----------------------------------------------------------------------------
; DLOGOPEN —— DiagWriteLog 的近程助手: DX=路径 → AX=句柄(失败 0)。
;   只破坏 AX/CX(与 INT 21h 约定一致), 调用方保存其余寄存器。
;-----------------------------------------------------------------------------
DLOGOPEN:
        mov     ax, 4301h           ; 先清可能残留的只读属性(失败无碍)
        xor     cx, cx
        int     21h
        mov     ax, 3D02h           ; 打开读写(文件已被探针建出)
        int     21h
        jnc     DLOGOPEN_ok
        mov     ah, 3Ch             ; 不存在则创建
        xor     cx, cx
        int     21h
        jc      DLOGOPEN_fail
DLOGOPEN_ok:
        retn
DLOGOPEN_fail:
        xor     ax, ax
        retn

;=============================================================================
; FBCLEAR —— 用 32 位偏移清空帧缓冲(display.c 的 ClearVisibleScreen 调)
;   C 原型: void __far __pascal FbClear( void )   —— 【无参数】
;   入参经 DGROUP 全局量: _FbClearSel(选择子) / _FbClearBytes(字节数) /
;                        _FbClearOff(起始偏移, r80 新增)
;
;   【r80 为何要 _FbClearOff】屏选择子现在优先建成"0 基址 + 线性偏移"
;   形态(见 drv.c LfbSelector_Ensure 的注释: DDK 对 deBits 的原文是
;   "minidriver must allocate a 0-based buffer and fill in the selector
;   part", 参考驱动 vmdisp9x 也是 0 基址选择子 + 线性偏移)。这种形态下
;   帧缓冲不再是"选择子基址 + 0", 而是"选择子基址(0) + 线性地址";
;   若建不成 0 基址选择子会【自动退回】旧的"基址=帧缓冲"形态, 此时
;   _FbClearOff = 0。两种形态下本过程代码完全相同, 差别只在这个变量。
;
;   【为何必须用汇编】帧缓冲可达数 MB, 而 16 位远指针的偏移只有 16 位:
;   超过 64KB 就回绕。Watcom 的 __huge 指针算术是"加选择子号"型的
;   归一化(保护模式 __HShift=3, 每 8 字节 +1 个选择子), 而我们手里
;   只有一个选择子 —— 用它等于去写别的 LDT 表项, 灾难。
;   这里显式发 67h(地址尺寸前缀), 让 rep stosd 以 ES:EDI 的 32 位
;   偏移寻址, 一次覆盖整屏。
;
;   【为何不带参数】Watcom 与 MSVC 的 __pascal 参数压栈顺序相反
;   (见 EMUINT86 头注的血泪史), 无参函数两端都无歧义; 参数经 DGROUP
;   全局量传递, 零 ABI 风险。
;   【寄存器】只用 EAX/ECX/EDI/ES, 全部压栈保护; DS 由 C 调用方保证
;   为 DGROUP。字节数必须是 4 的倍数(调用方已与 ~3 对齐)。
;=============================================================================
        public  FBCLEAR
FBCLEAR proc far
        push    es
        push    edi
        push    ecx
        push    eax

        mov     es, [_FbClearSel]
        mov     ecx, [_FbClearBytes]
        shr     ecx, 2                  ; 字节数 → 双字数
        xor     eax, eax                ; 填充值 0(黑屏)
        mov     edi, [_FbClearOff]      ; 32 位起始偏移(r80: 0 或线性地址)
        cld
        db      67h                     ; a32 前缀: stosd 寻址用 ES:EDI
        rep     stosd

        pop     eax
        pop     ecx
        pop     edi
        pop     es
        retf
FBCLEAR endp

;=============================================================================
; FBTEST —— 帧缓冲"写/读回"自检(r82): 用 32 位指令整段写入可识别的
;   位置相关花样, 再整段读回比对。
;   C 原型: WORD __far __pascal FbTest( void )   —— 【无参数】
;   入参经 DGROUP 全局量: _FbTestSel / _FbTestOff / _FbTestLen / _FbTestPat
;   返回: AX = 0 全部一致; 否则 AX = 第一个不一致处的【双字序号 + 1】
;         (1 起; 0xFFFF 保留给"参数非法/越界")
;
;   【r83 修正了一个把返回值毁掉的错误】r82 的收尾是 pop eax / pop edx ...
;   —— 而 AX 正是 WORD 返回值的寄存器: pop eax 把调用方原来的 EAX 装回 AX,
;   刚算出的返回值当场被覆盖, 调用方拿到的是"它自己 EAX 的低 16 位"。
;   日志里的 "FbTest head ret=0007 / tail ret=FF00" 就是这么来的(0007 与
;   FF00 都不是可能的返回值: 序号最大 64, 非法值是 0xFFFF)。后果不是少一行
;   日志, 而是自检结论恒为"失败" → OFFSCREEN 被撤 + 显存上报被钳到可见
;   帧缓冲大小, 等于 r82 的自检一行也没生效。
;   本版把结果暂存在 SI(callee-saved, 全工程都在保存), 并且【把写 AX 放到
;   所有 pop 之后】—— 顺序见 ft_out, 这是本过程唯一不能改的地方。
;
;   【为什么需要它 —— 真机专用故障】VBE 报的显存总量与"我们真正能访问到的
;   孔径"在真机上经常不一致(报 256MB、孔径只有 64MB; 或映射成功但高地址
;   别名回低地址)。一旦按上报值把离屏显存交给 GDI/DIBENG, 离屏面就落进
;   空洞或别名区 —— 症状是随机花屏/黑屏, 而虚拟机里"显存=宿主内存",
;   这个故障永远不会出现。唯一的通用判据就是: 写进去、读回来、比一比。
;
;   【为什么花样随位置变化】固定常数花样无法发现"高地址别名回起点"
;   (0xDE000000 与 0xDE800000 映到同一物理页时, 常量花样照样读得出)。
;   这里写入 pat + i(i 为双字序号), 别名一定导致不一致。
;
;   【32 位指令】全程用 EAX/ECX/EDX/EDI + 67h(a32)前缀, 一次搬 4 字节 ——
;   与 FBCLEAR 同款写法, 满足"能用 32 位就不用 16 位"的性能方针。
;   【不碰写合并的注意事项】本自检读回的是刚写的显存, 目前帧缓冲还是
;   UC(未申请 MTRR/WC); 将来若启用 WC, 读回前需要 WC 冲刷 —— 见 ROADMAP T4.3。
;
;   【边界安全】调用方必须保证 off+len 落在【已映射且已声称的显存】之内;
;   本过程额外做一次长度合法性检查(len 必须 ≥4 且是 4 的倍数, 且 < 64KB),
;   不合规直接返回 0xFFFF, 绝不发起越界访问。
;=============================================================================
        public  FBTEST
FBTEST proc far
        push    esi                     ; 【为何 ESI 压在栈底】结果暂存 SI,
        push    es                      ; 而 ESI 要最后才弹(见 ft_out) —— 先压
        push    edi                     ; 的就在最深, 恰好最后弹。
        push    ecx
        push    edx

        mov     ecx, [_FbTestLen]
        cmp     ecx, 4
        jb      ft_bad                  ; 太短: 不测
        cmp     ecx, 00010000h          ; 64KB 上限: 自检永远是"抽样", 不是搬运
        jae     ft_bad
        test    ecx, 3
        jnz     ft_bad                  ; 必须双字对齐

        mov     ax, [_FbTestSel]        ; 【为何要经 AX 中转】不存在 "test es, es"
        test    ax, ax                  ; 这种编码 —— 段寄存器不能做 test 的操作数
        jz      ft_bad                  ; (r82 首版在此处汇编报 E040, 就是这么来的)。
        mov     es, ax                  ; 选择子为 0: 不碰

        ; ---- 第一遍: 写入 pat+i ----
        ; 【为何用 stosd 而不是"把 EDI 写进方括号"的显式内存操作数】后者在 16 位
        ; 段里必须靠 67h 前缀才认 EDI, 而"前缀由谁补、补在指令前还是操作数前"
        ; 各汇编器写法不一; stosd/scasd 是隐式 ES:(E)DI 寻址指令, 与 FBCLEAR 里
        ; 已在 r79 真机验证过的 rep stosd 完全同款(67h + 隐式寻址), 零歧义。
        ; (CHECK.bat 有一条反向断言: 本文件不得再出现显式 32 位内存操作数)
        cld
        shr     ecx, 2                  ; 字节数 → 双字数
        mov     edi, [_FbTestOff]
        mov     eax, [_FbTestPat]
ft_w:
        db      67h                     ; a32 前缀: 隐式寻址改用 EDI(32 位偏移)
        stosd                           ; ES:EDI <- EAX, EDI += 4
        inc     eax                     ; 花样随位置变化(抓高地址别名)
        loop    ft_w                    ; 16 位 loop 用 CX(计数 ≤ 16384)

        ; ---- 第二遍: 读回比对 pat+i ----
        mov     edi, [_FbTestOff]
        mov     ecx, [_FbTestLen]
        shr     ecx, 2
        mov     eax, [_FbTestPat]
        xor     edx, edx                ; EDX = 双字序号 i
        cld
ft_r:
        db      67h
        scasd                           ; EAX - ES:EDI(a32), EDI += 4
        jne     ft_mismatch             ; 与写入值不符 → 别名/空洞
        inc     eax                     ; 复原本位置应有的花样
        inc     edx
        loop    ft_r

        xor     si, si                  ; 全部一致
        jmp     ft_out
ft_mismatch:
        mov     si, dx                  ; 序号(0 起)
        inc     si                      ; 返回 1 起
        jmp     ft_out
ft_bad:
        mov     si, 0FFFFh              ; 参数非法: 调用方按"未测"处理

ft_out:
        pop     edx                     ; ★ 顺序不可改: 结果暂存在 SI, 而 AX
        pop     ecx                     ;   必须在【所有 pop 之后】才写 ——
        pop     edi                     ;   r82 先写 AX 再 pop eax, 返回值当场
        pop     es                      ;   被调用方原来的 EAX 覆盖(见头注)。
        mov     ax, si
        pop     esi
        retf
FBTEST endp

;=============================================================================
; DIBFNSTUB —— g_dibfn[] 的占位桩(DrvInitCore 先整表填它)。
;   DIBENG 解析失败/未发生时, 转发器跳到它: 返回 0 而不是跳进 0:0。
;   注意 pascal 调用方按约定期待被调方 retf n 清参, 本桩一律 retf 0,
;   参数会残留在调用方栈上 —— 仅用于 DIBENG 缺失的退化场景(此时日志
;   已点名缺失项, GDI 回退 VGA), 正常路径解析成功后永不到达。
;=============================================================================
        public  DIBFNSTUB
DIBFNSTUB proc far
        xor     ax, ax
        retf
DIBFNSTUB endp

;=============================================================================
; EmuInt86 / EmuInt86x —— clibm int86/int86x 的汇编替代(仅限 DS=DGROUP
; 的近指针寄存器包, 布局同 Watcom union REGS: +0 ax +2 bx +4 cx +6 dx
; +8 si +10 di +12 cflag +14 flags; x 版的 SREGS 只读写 +0 es)。
;   C 原型(pascal far):
;     void EmuInt86 ( WORD intno, union  REGS __near *rp );
;     void EmuInt86x( WORD intno, union  REGS __near *rp,
;                     struct SREGS __near *sp );
;
; 【参数栈序 —— Watcom __pascal 与 MSVC 相反, 血泪实锤】Watcom 的
; pascal 约定按【从左到右】的顺序压参(arg1 最先压), 因此 arg1 在
; 【最高】栈位; MSVC 的 PASCAL 从右到左压, arg1 在 [bp+6]。蓝屏
; 0d:0327:00000374 即因本函数最初按 MSVC 序读参:SI=[bp+10] 实际
; 拿到的是 intno(0x2F), "mov ax,[si]" 读到 DGROUP:0x2F 的垃圾非零
; 值, "mov es,ax" 装非法选择子 GP(报 0x374 = 故障指令 0x372 + 长度2)。
; 正确偏移(arg1 在顶):
;   EmuInt86 : intno=[bp+8]  rp=[bp+6]
;   EmuInt86x: intno=[bp+10] rp=[bp+8] sp=[bp+6]
; 单参函数(VDDCALL/DIAGWRITELOG)两种序相同, 不受影响。
;
; 【存在的理由 —— ./6 构建实锤】clibm int86x 把寄存器包拷进栈上缓冲,
; _DoINTR_ 却以 "mov ds,cx"(cx=int86x 入口 DS) 寻址它: DS==SS 的
; EXE 下正确, 本驱动 -zu(SS=调用方栈/DS=DGROUP) 下变成用 DGROUP
; 选择子读栈偏移 —— 轻则静默失败(旧 DiagLog), 重则段界限 GP
; (蓝屏 0d:0327:00000907, _DoINTR_ 首条 [bx])。本实现不碰 SS,
; 缓冲区约定在 DGROUP(static), 与探针/DiagWriteLog 同机制。
; 只支持本驱动用到的 10h(VBE BIOS)/21h(DOS)/2Fh(取 VxD 入口)/
; 31h(DPMI); 其余中断号置 cflag=1 返回。
;=============================================================================
        public  EMUINT86
EMUINT86 proc far
        push    bp
        mov     bp, sp
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     ax, [bp+8]          ; intno(Watcom pascal: arg1 在最高位)
        mov     bx, [bp+6]          ; rp
        xor     si, si              ; 无 SREGS
        call    EI_core
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     bp
        ret     4
EMUINT86 endp

        public  EMUINT86X
EMUINT86X proc far
        push    bp
        mov     bp, sp
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        mov     ax, [bp+10]         ; intno(Watcom pascal: arg1 在最高位)
        mov     bx, [bp+8]          ; rp
        mov     si, [bp+6]          ; sp(SREGS 近指针, 只读写 es 字段)
        call    EI_core
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     bp
        ret     6
EMUINT86X endp

;-----------------------------------------------------------------------------
; EI_core —— 近程公共核心。入口: AL=中断号, BX=rp, SI=sp(0=无)。
;   约定: DS=DGROUP(调用方保证), 缓冲区均在 DGROUP。破坏 AX..DI。
;   _eiRp/_eiSp 为静态暂存(单线程协作式环境安全)。
;-----------------------------------------------------------------------------
EI_core:
        mov     [_eiRp], bx
        mov     [_eiSp], si
        mov     cx, [bx+4]          ; cx_val
        mov     dx, [bx+6]          ; dx_val
        push    word ptr [bx+8]     ; si_val    \
        push    word ptr [bx+10]    ; di_val     | 逆序压栈, 正序弹回
        push    word ptr [bx+2]     ; bx_val     |
        push    word ptr [bx+0]     ; ax_val    /
        test    si, si
        jz      EI_noes
        push    ax                  ; 【保护 intno】下面的装载会经过 AX,
        mov     ax, [si+0]          ;  而分派比较用的是 AL —— 先存后取。
        test    ax, ax
        jz      EI_es_skip          ; 【0 不能装进段寄存器】VxD_Connect 把
        mov     es, ax              ;  es=0 当 INT 2F/1684 的输入约定,
EI_es_skip:                         ;  装空选择子即 #GP(0d:...:036e 实锤)
        pop     ax                  ;  —— 跳过保留原 ES; 1684 不读输入
EI_noes:                            ;  ES:DI, 安全。
        cmp     al, 10h
        je      EI_i10
        cmp     al, 21h
        je      EI_i21
        cmp     al, 2Fh
        je      EI_i2f
        cmp     al, 31h
        je      EI_i31
        add     sp, 8               ; 不支持: 弹掉 4 个值, 标错返回
        mov     bx, [_eiRp]
        mov     word ptr [bx+12], 1 ; cflag = 1
        retn
EI_i10: pop     ax                  ; ax_val
        pop     bx                  ; bx_val
        pop     di                  ; di_val
        pop     si                  ; si_val
        int     10h
        jmp     EI_fin
EI_i21: pop     ax
        pop     bx
        pop     di
        pop     si
        int     21h
        jmp     EI_fin
EI_i2f: pop     ax
        pop     bx
        pop     di
        pop     si
        int     2Fh
        jmp     EI_fin
EI_i31: pop     ax
        pop     bx
        pop     di
        pop     si
        int     31h
EI_fin:
        push    ax                  ; 返回 ax(栈底)
        push    bx                  ; 返回 bx
        push    di                  ; 返回 di
        pushf                       ; 返回 flags(栈顶)
        mov     di, [_eiRp]         ; DI = rp(返回 di 已入栈)
        pop     bx                  ; flags
        mov     [di+14], bx
        and     bx, 1
        mov     [di+12], bx         ; cflag
        pop     bx                  ; 返回 di
        mov     [di+10], bx
        pop     bx                  ; 返回 bx
        mov     [di+2], bx
        pop     bx                  ; 返回 ax
        mov     [di+0], bx
        mov     [di+4], cx
        mov     [di+6], dx
        mov     [di+8], si
        mov     bx, [_eiSp]
        test    bx, bx
        jz      EI_nost
        mov     [bx+0], es          ; ES 写回 SREGS.es
EI_nost:
        retn

;=============================================================================
; VddAcquire —— 取得主 VDD 的 API 入口与本 VM 句柄
;   C 原型: void __far __pascal VddAcquire(void)
;
; INT 2Fh AX=1684h BX=设备ID 是 Win9x 取 VxD API 入口的标准机制；
; BX=000Ah 即主 VDD(VDD.VXD)。INT 2Fh AX=1683h 返回 BX=当前 VM 句柄，
; 之后每次 VDD API 调用都要带着它(标识"我是显示驱动所在的 VM")。
;=============================================================================
        public  VDDACQUIRE
VDDACQUIRE proc far
        push    bx
        push    di
        push    es

        xor     di, di
        mov     es, di              ; ES:DI = 0 进入(约定)
        mov     ax, 1684h
        mov     bx, 000Ah           ; 主 VDD 的设备 ID
        int     2Fh
        mov     word ptr [_VDDEntryPoint], di
        mov     word ptr [_VDDEntryPoint+2], es

        mov     ax, 1683h
        int     2Fh                 ; 出口: BX = 当前 VM 句柄
        mov     [_OurVMHandle], bx

        pop     es
        pop     di
        pop     bx
        ret
VDDACQUIRE endp

;=============================================================================
; VddCall —— 最简单的 VDD API 调用(只需功能号)
;   C 原型: DWORD __far __pascal VddCall(WORD func)
;   EAX = func, EBX = VM 句柄 → 返回 EAX(以 DX:AX 交给 C)
;
; 用于 VDD_REGISTER_DISPLAY_DRIVER_INFO(0x83，第 ③ 环注册)、
; VDD_PRE_MODE_CHANGE(0x86)、VDD_POST_MODE_CHANGE(0x87)、
; VDD_SAVE_DRIVER_STATE(0x82)、VDD_DRIVER_UNREGISTER(0x81)。
;=============================================================================
        public  VDDCALL
VDDCALL proc far
        push    bp
        mov     bp, sp
        push    ebx
        push    esi
        push    edi
        push    es

        movzx   eax, word ptr [bp+6]        ; 功能号
        movzx   ebx, word ptr [_OurVMHandle]
        call    dword ptr [_VDDEntryPoint]  ; 16:16 远调用进 VDD

        mov     edx, eax                    ; EAX → DX:AX
        shr     edx, 16

        pop     es
        pop     edi
        pop     esi
        pop     ebx
        pop     bp
        ret     2
VDDCALL endp

;=============================================================================
; VddRegister —— VDD_DRIVER_REGISTER(0x80) 的完整调用
;   C 原型: DWORD __far __pascal VddRegister(WORD func, WORD pitchBytes,
;                                            WORD height, void __far *cb)
;
; DDK 规定的寄存器约定(与 vmdisp9x modes.c 的 CallVDDRegister 一致)：
;   入口: EAX = 功能号(VDD_DRIVER_REGISTER)
;         EBX = 本 VM 句柄
;         ECX = 全部可见扫描线总字节数(pitch × height)
;         EDX = 0(让 VDD 尝试虚拟化)
;         ES:DI = "切回桌面"恢复回调(本驱动为 RestoreDesktopMode)
;   出口: EAX = VDD 占用的显存字节数；若等于功能号本身则说明调用失败
;
; 【这个调用的意义】它是 DOS 全屏兼容的关键：注册回调后，每当 DOS
; 全屏程序退出/切回 Windows，主 VDD 都会回调 RestoreDesktopMode，
; 由我们重设 VBE 模式、重载 DAC 调色板 —— 没有它，DOS 全屏程序
; 一退出就是黑屏/花屏。
;=============================================================================
        public  VDDREGISTER
VDDREGISTER proc far
        push    bp
        mov     bp, sp
        push    ebx
        push    esi
        push    edi
        push    es
        ; 栈布局(Watcom __pascal: arg1 最先压→在最高位; 远指针参内
        ; 部为 seg 先压/off 后压 → off 在低位):
        ;   [bp+6]=cb 偏移 [bp+8]=cb 段 [bp+10]=height [bp+12]=pitch
        ;   [bp+14]=func
        movzx   eax, word ptr [bp+12]   ; pitchBytes
        movzx   ecx, word ptr [bp+10]   ; height
        mul     ecx                     ; EAX = pitch × height
        mov     ecx, eax                ; → ECX
        movzx   eax, word ptr [bp+14]   ; 功能号
        movzx   ebx, word ptr [_OurVMHandle]
        ; r99: EDX 由 C 侧策略折算好(0 = 允许窗口化 4 平面虚拟化,
        ; 0FFFFFFFFh = -1 = 要求 VGA 4 平面图形全屏运行)。
        ; 默认值 0 = r98 行为, 因此不设开关时的行为逐字节不变。
        mov     edx, dword ptr [_g_vddEdxValue]
        les     di, dword ptr [bp+6]    ; ES:DI = 恢复回调
        call    dword ptr [_VDDEntryPoint]

        mov     edx, eax
        shr     edx, 16

        pop     es
        pop     edi
        pop     esi
        pop     ebx
        pop     bp
        ret     10
VDDREGISTER endp

;=============================================================================
; VddGetDispConf —— VDD_GET_DISPLAY_CONFIG(0x85)：读注册表显示配置
;   C 原型: DWORD __far __pascal VddGetDispConf(WORD func, WORD bufSize,
;                                               void __far *buf)
;   入口: EAX=功能号, EBX=VM 句柄, ECX=DISPLAYINFO 结构大小,
;         EDX=0, ES:EDI=接收缓冲区
;   出口: EAX=0 成功
;
; 【用途】显示属性对话框里选的分辨率/色深存在注册表；主 VDD 读到后
; 通过此 API 转交给我们 —— 这是"显示属性"设置生效的通道。
; 注意 ES:EDI 的 EDI 必须零扩展(偏移是 16 位的)。
;=============================================================================
        public  VDDGETDISPCONF
VDDGETDISPCONF proc far
        push    bp
        mov     bp, sp
        push    ebx
        push    esi
        push    edi
        push    es
        ; 栈布局(Watcom __pascal, 见 VDDREGISTER 注释):
        ;   [bp+6]=buf 偏移 [bp+8]=buf 段 [bp+10]=bufSize [bp+12]=func
        movzx   eax, word ptr [bp+12]   ; 功能号
        movzx   ebx, word ptr [_OurVMHandle]
        movzx   ecx, word ptr [bp+10]   ; bufSize
        xor     edx, edx
        les     di, dword ptr [bp+6]    ; ES:DI = 接收缓冲区
        movzx   edi, di                 ; ES:EDI(EDI 高 16 位清零)
        ; 括日志: 即将远调主 VDD。DiagWriteLog 保护全部寄存器与 ES,
        ; 不破坏已装好的参数。若日志停在本行之后, 即实锤主 VDD 的
        ; 0x85 处理程序内部 fault(0d:032f:0000088d/0acd 两案发现场)。
        push    ax
        mov     ax, offset DGROUP:szVddGetDispConf
        push    ax
        call    DIAGWRITELOG
        pop     ax
        call    dword ptr [_VDDEntryPoint]

        mov     edx, eax
        shr     edx, 16

        pop     es
        pop     edi
        pop     esi
        pop     ebx
        pop     bp
        ret     8
VDDGETDISPCONF endp

;=============================================================================
; DIB 引擎转发器(全部改为经 g_dibfn[] 间接跳转)
;
; DIBTHK name, param, slot —— 转发到 DIB_name##Ext(地址在 g_dibfn[slot])，
;   并在所有参数之后插入一个额外参数(param 是 DGROUP 里的变量：
;   _lpDriverPDevice 或 _wPalettized)。原理：弹出 16:16 返回地址到 ECX，
;   压入额外参数，再压回返回地址，然后 jmp。pascal 约定下 AX/ECX/ES
;   本来就是可破坏寄存器，这样玩栈是安全的。
;
; DIBFWD name, slot —— 参数签名与 DIB_name(地址在 g_dibfn[slot])完全
;   一致，直接 jmp 透传。
;
; 【为什么不再 extrn DIB_*】那是静态序号导入的来源：链接器经
; dibeng.lib 把每个 DIB_ 符号写成对 DIBENG.DLL 的序号导入，KRNL386
; 加载时逐条做导入修正，任何一条序号在本机 DIBENG 里不存在就整个
; 拒载(Failure code 0000，入口都执行不到)。改成间接跳转后，本模块
; 对 DIBENG 零静态导入，版本无关；地址表由 C 侧 ResolveDIBEngine
; 按名字填充。
;
; 【为什么每个转发器都查 _g_dibfnReady 懒解析】本驱动由 KRNL386 引导
; 期直接加载(bootlog 中与 system.drv 同列)，彼时 GDI 未启动、
; DIBENG.DLL 不在内存，加载期解析必然失败、LoadLibrary 兜底更会
; 重入加载器在 KERNEL 内 GP(真机 error 0D 蓝屏, ./5 构建)。故:
;   · 引导期(DrvInitCore)只把 g_dibfn[] 填成 DIBFNSTUB 保底;
;   · 首次转发(GDI 已就绪, DIBENG 随 GDI 加载)时经 DIBENSURERESOLVED 解析,
;     成功填真实地址并置 _g_dibfnReady=1, 之后全程零开销直跳。
; 这把"解析时机"从加载期推迟到 GDI 期, 与 vmdisp9x(其 DriverInit
; 完全不碰 DIBENG)的实证行为一致。
;
; 【__loadds 约定：jmp 前必须把 DIBENG 的 DGROUP 装进 AX】16 位 DLL
; 导出函数的序章是 "mov ds, ax"，约定调用方在 AX 里传入被调 DLL 的
; DGROUP。静态导入时代理 thunk 自动做这件事；现在 jmp 的是 DIBENG
; 原始入口，必须显式补装 —— 否则 DIBENG 一进门就把我们的 DGROUP
; 当它的用。选择子在 _wDibDgroup(初始化时由 C 侧从模块数据库取出)。
; 读 _wDibDgroup/_g_dibfn 需要 DS=我们的 DGROUP：用 "mov ax, DGROUP"
; 自力建立，不依赖调用者是否按 __loadds 约定在 AX 里传了我们的
; DGROUP(GDI 实际上会传，但显式装载零风险零代价)。
;=============================================================================
;=============================================================================
; 【r86 关键修正】转发器必须【保住调用方的 DS】—— 这就是本次 0D 蓝屏的根因
;
;   16 位 Windows 调用约定: DS/SI/DI/BP/SS 由【被调方保留】, AX/BX/CX/DX/ES
;   可由被调方破坏。旧的 DIBFWD/DIBTHK 是"设好 DS 后 jmp 进 DIBENG"的【尾跳】:
;   DIBENG 每个导出函数的序章都是 mov ds,ax(装它自己的 DGROUP), 于是它返回时
;   直接把 retf 交回给【我们的调用者】, 而调用者的 DS 从此变成 DIBENG 的
;   DGROUP —— 选择子 0337, 界限只有 0x2BF = 703 字节。调用者下一条 DS 相对
;   访问只要偏移超过 703 就 #GP → 0D, 而崩点当然落在【调用者自己的代码】里。
;   这与 r76b..r85 的现场完全吻合: 崩在别的模块段内、没有任何 DDI 探针触发、
;   而且同一条指令在 call 之前访问同一地址时【明明是成功的】。
;
;   r85 的 512 字节窗口把这个现场钉死了(见 R76_CHANGES.md):
;       2B4F: and word [si+26h], 0DFh    ← 同一地址, 访问成功
;       2B69: call far [di+8]            ← 经函数表的一次远调用
;       2B70: or  word [si+26h], 20h     ← 同一地址, 这次 #GP → 0D
;   [si+0x26] 在 call 前后一模一样, SI 也没变(被调方必须保留 SI) —— 变的只有
;   段寄存器, 即被调方没有恢复调用方的 DS。
;
;   参考驱动(boxv9x 等)用的是 DIBENG 的【静态导入】, 由加载器的引入桩负责
;   保存/恢复 DS, 所以不会有这个问题; 我们当年为了"对 DIBENG 零静态导入"
;   改成了动态 jmp, 顺手把 DS 的保存一起丢了。本版保留动态解析(不动导入表),
;   只把尾跳改成【带桩的远调用】。
;
;   【修法】弹掉调用方返回地址 → 压入我们的桩(需要插尾参的先压尾参, 位置见下)
;           → 把调用方 DS/BP/返回地址存进栈上自己的一块 → DS/ES=本模块
;           → 懒解析 + 追踪 → jmp DIBENG → DIBENG 的 retf N 回到桩: 参数已被
;           它弹净, SP 恰好是调用方期望的值 → 恢复 DS/BP → 跳回调用方。
;   【栈布局要点】内存里参数是 [返回地址][最后一个参数]…[第一个参数], 所以
;   额外尾参必须紧贴返回地址、返回地址必须与最后一个参数相邻; 我们保存现场的
;   那几个字必须压在【返回地址之下】(更低地址), 绝不能插到返回地址与参数
;   之间, 否则被调方读到的参数全错。
;   【AX/DX 一个字都不许碰】桩里恢复 DS/BP 与跳转只用 BX/CX: 返回值在 DX:AX。
;=============================================================================
;-----------------------------------------------------------------------------
; 【r95 关键修正 —— 转发器三处病灶(r94 的 DS 假设被本次实测改写)】
;
;  ① 装载器桩的真实语义: DIBENG 的导出函数【自己不装 DS】—— 桩(035F:0182)
;     干的是 "mov ax,<DIBENG DGROUP>" 并装 DS, 然后跳进真实入口; 静态导入
;     (CreateDIBPDevice)也由同一套机制代劳。我们动态解析到的是 36 个入口里
;     的 35 个【原始入口】, 旧代码只装 AX 不装 DS: DIBENG 于是全程在我们
;     自己的数据段里读写它的全局量, 从中取出一个垃圾远指针去 call/jmp →
;     0D 蓝屏 0607:00001B5C(0607 = GDI 的数据段: "把数据当函数调"的形态)。
;     唯一没崩的 DIB_Enable 恰好被解析到桩上 —— 桩自己装 DS, 所以它一直对。
;  ② 调用方现场绝不能压栈: pascal 约定下内存布局是 [返回地址][参数…], 而
;     转发器必须把返回地址换成自己的桩, 于是 SP 必须【正好】指向桩 —— 中间
;     多一个字都会被被调方当成参数。旧实现把现场 push 在桩之后, SP 因此指向
;     保存的 BP: DIBENG 的 "retf N" 把 [BP][DS] 当返回地址, 直接跳进调用方
;     的数据段(与 ① 同形态的第二种解释)。r95 起现场存进 DGROUP 的 LIFO 区
;     (深度 8, 取模回绕: DIBENG→我们回调→DIBENG 的嵌套配对顺序天然成立)。
;  ③ 被调方回来时 DS 是 DIBENG 的(它 push/pop ds 保护的是"进去时"的 DS),
;     所以桩必须自己把调用方 DS 还回去 —— 与 CDPD 用 push ds/pop ds 包住
;     CreateDIBPDevice 是同一个道理(那里早就这么干了, 这里是补上跳板)。
;-----------------------------------------------------------------------------
;-----------------------------------------------------------------------------
; 【r98 关键修正 —— 0D @ 0367:00003FA5 的根因(字节级定案)】
;
;   病灶: DIBTHK 宏里 `push param`(param = DGROUP 里的 _lpDriverPDevice /
;   _wPalettized)是【用 DS 相对寻址】读的, 而它在旧代码里的位置是
;   "弹返回地址之后、DIBSAVE(装 DS)之前" —— 也就是说: 它读的是
;   【调用方 DS 下、同一偏移处】的内容, 不是我们的变量。
;
;   为何一直没被发现: 本模块所有 C 侧 DDI 都带 __loadds, 而 __loadds 是
;   【被调方自己装 DS】。用本工程的编译选项实测(探针源 + 同参数编译):
;       WORD __far __pascal __loadds f(void) { return g; }
;       → 1E | B8 <seg g> | 8E D8 | A1 <g> | 1F | CB
;         push ds / mov ax,DGROUP / mov ds,ax / ... / pop ds / retf
;   于是 C 侧函数无论调用方给什么 DS 都能正确读全局量 —— 日志里所有
;   C 侧探针(CAP:/PDEV:/SetPalette:) 一直是对的, 唯独汇编跳板里
;   这一个操作数依赖调用方的 DS, 而它与 C 侧探针没有任何交叉验证点。
;
;   现场吻合(r95 与 r97 两次同址蓝屏, r96 因 SetPalette 先崩而掩盖):
;     · Enable 里我们自己从 C 调 DIB_SetPaletteTranslateExt(NULL, PDEV)
;       (槽 32, DS = 本模块) → 日志 "Enable: xlat call returned" ✓ 正常;
;     · GDI 随后调 DDI 序号 24 = 本文件的 SetPaletteTranslate(DIBTHK 槽 24,
;       尾参 = _lpDriverPDevice, 目标 g_dibfn[24] = 0367:3F82) —— 引擎
;       Ext 正文读 [bp+06] 当设备指针, 走到
;           0367:3FA2  C4 7E 06        les di,[bp+06]
;           0367:3FA5  26 81 65 1C FF EF   and word es:[di+1Ch],0EFFFh
;       就 GP —— 蓝屏地址正是这一条。而 [bp+06] 就是被插进去的那个尾参,
;       也就是"调用方 DS 下 0x3558 处的 4 字节"(omninine.map: _lpDriverPDevice
;       = 0002:3558), 不是我们的 2056:0000。
;     · 同一时刻的 C 侧日志(SetPalette: PDEV seg=2056 / deFlags=800B /
;       PDEV desc lim=045F)已证明真设备指针完全合法 ⇒ 被插错的就是【读它的
;       DS】, 不可能是别的。
;
;   修法: 把"留住调用方 DS + 装本模块 DS"提到读尾参之前(DIBSAVEDS), 尾参
;   于是从我们自己的 DGROUP 读出。调用方 DS 仍然原样存进 LIFO、由 DIBSTUB
;   还回 —— r86/r95 的两条不变量(现场不压栈、DS 归还)一个字节都没动。
;
;   为什么对 DIBFWD/DIBXW 也做: 这两个宏旧代码在装 DS 之前不读任何 DGROUP
;   变量, 本来就对; 统一走 DIBSAVEDS 之后三个宏的序章完全同构, 少一种
;   "哪个宏记得先装 DS"的记忆负担(r97 之前的三次 DS 事故都出在这类
;   不一致上)。
;-----------------------------------------------------------------------------
DIBFWD_SAVE_MAX     equ 8
DIBFWD_SAVE_MASK    equ (DIBFWD_SAVE_MAX-1)

; DIBSAVEDS —— ★ r98: 先留住调用方 DS, 再把 DS/ES 装成本模块 DGROUP。
;   进: DS = 调用方的数据段;  出: CX = 调用方 DS, DS/ES = 本模块 DGROUP,
;       AX/DX 原样(返回地址还在里面), BX 被破坏(16 位 ABI 允许)。
;   位置: 必须在任何"读 DGROUP 变量"之前(尤其 DIBTHK 的 push param)。
DIBSAVEDS macro
        mov     cx, ds                      ; 调用方 DS(换 DS 之前先取到寄存器)
        mov     bx, DGROUP
        mov     ds, bx                      ; DS = 我们的 DGROUP
        mov     es, bx
        assume  ds:DGROUP, es:DGROUP
        endm
; r98-marker: DIBSAVEDS is the only place that may read the caller DS

; DIBSAVE —— 保存调用方现场到 DGROUP 的 LIFO 区(栈上一个字都不占)
;   进: AX = 调用方返回偏移, DX = 调用方返回段, CX = 调用方 DS,
;       DS/ES = 本模块 DGROUP(由 DIBSAVEDS 建立; r98 起本宏不再自己装 DS,
;       否则会把"已经换成我们的 DS"当成调用方 DS 存进去, 归还时把调用方
;       的 DS 改成我们的 —— r86 的病会以另一种形式回来)
;   出: BX 被破坏
; r98-marker: DIBSAVE takes the caller DS in CX and must not reload DS
DIBSAVE macro
        mov     bx, [_g_dibSaveIdx]
        inc     word ptr [_g_dibSaveIdx]
        and     bx, DIBFWD_SAVE_MASK        ; 取模回绕: 天然配对, 无失败分支
        add     bx, bx                      ; 每槽 3 个 WORD(三个并行数组)
        mov     [_g_dibSaveDS  + bx], cx    ; 调用方 DS
        mov     [_g_dibSaveOff + bx], ax    ; 调用方返回偏移
        mov     [_g_dibSaveSeg + bx], dx    ; 调用方返回段
        endm

; DIBSTUB —— 被调方(经我们的远跳)返回后的落点。只许用 BX/CX/DS:
;   返回值在 DX:AX, SI/DI/BP 必须原样还给调用方, 栈上不许有多余动作。
;   【r113 新增返回面包屑】每槽第一次走到这里时进 C 落一行
;   (`DDI ret -> DIB_xxx`)。判断在汇编侧(读 _g_retDone[slot]), 所以
;   之后的调用只多一条 cmp/jne —— 与入口侧 TRACESLOTMAC 完全同构。
;   DX:AX 是被调方的返回值、ES 按 ABI 是易失寄存器, 两者都保护起来再 call,
;   于是"返回值/参数段一个字节不动"这条老约定继续成立。
DIBSTUB macro   slot
dibret_&slot:
        mov     cx, DGROUP
        mov     ds, cx                      ; 此刻 DS 还是被调方的, 先钉回自己的
        ; r113-marker: DIBSTUB return breadcrumb (one line per slot, ever)
        cmp     byte ptr [_g_retDone + slot], 0
        jne     short dibnoret_&slot
        mov     byte ptr [_g_retDone + slot], 1
        push    ax
        push    dx
        push    es
        mov     ax, slot
        push    ax
        call    TRACESLOTRET
        pop     es
        pop     dx
        pop     ax
dibnoret_&slot:
        mov     bx, [_g_dibSaveIdx]
        dec     bx
        mov     [_g_dibSaveIdx], bx
        and     bx, DIBFWD_SAVE_MASK
        add     bx, bx
        mov     cx, [_g_dibSaveOff + bx]        ; 调用方返回偏移
        push    word ptr [_g_dibSaveSeg + bx]   ; 先压段
        push    cx                              ; 后压偏移 → [SP] = 偏移
        mov     ds, [_g_dibSaveDS + bx]         ; ★ 还回调用方 DS
        retf                                    ; 参数已由被调方 retf N 清掉
        endm

; DIBJUMP —— 交给 DIBENG。先取目标地址(此刻 DS 还是我们的, 读表必须趁现在),
;   再按需要把 DS 装成 DIBENG 的 DGROUP(装载器桩语义), 最后远跳过去。
;   用 push/retf 而不是 "jmp far [表]": 净栈效果相同, 但读表不受 DS 影响。
;   【r97 新增: EDX = 设备(DIBENGINE)指针】DIBENG 有一族"非 Ext"导出, 它们
;   的入口就是 6 字节的跳板, 从 r96 的 DIBCODE 转储里可以直接读到(r96 日志
;   第 879 行的 off=4018 窗口):
;       DIB_SetPalette    = 0367:401A:  66 59 66 52 66 51   (= pop ecx/32位;
;                                        push edx; push ecx)
;       DIB_SetPaletteExt = 0367:4020:  55 8B EC ...        (同一函数体)
;   这三条指令的作用是【把 EDX 插到返回地址与参数之间】——也就是给"少一个
;   尾参"的非 Ext 入口补上第 4 个参数 lpDIBEngine。反过来读: 调用方必须
;   在 EDX 里放好这个指针(DIBLINK 在标准小驱动里做的就是这件事)。DDK 头
;   ddk/dibeng.h 的声明与之吻合: DIB_SetPaletteExt(WORD,WORD,LPVOID,LPPDEVICE),
;   而函数体紧接着用 [bp+06] 当 DIBENGINE: lds di,[bp+06] → lds di,[di+0x20]
;   (deBitmapInfo, DDK 布局 +0x20) → inc dword [di+0x24]。
;   【r96 的蓝屏 0367:00004036 就是这条 inc】r96 默认的 Pal=0 直接调了非 Ext
;   入口却没设 EDX ⇒ 引擎拿我们上一次调用残留的 EDX 当 DIBENGINE, 从那里读
;   出的 deBitmapInfo 是个非法远指针, 写 +0x24 时 GP。
;   【为什么放在这里】取表必须趁 DS 还是我们的(_g_runEdx/_lpDriverPDevice 都是
;   本模块 DGROUP 的 near 变量), 而 DIBENG 的序章会自己重装 DS, 所以注入点
;   必须在 mov ds,ax 之前。Edx=0 可关掉注入以复现 r96 的行为(A/B 用)。
DIBJUMP macro   slot
        mov     ax, [_wDibDgroup]
        mov     bx, word ptr [_g_dibfn + (slot)*4]      ; 目标偏移
        mov     cx, word ptr [_g_dibfn + (slot)*4 + 2]  ; 目标段
        cmp     byte ptr [_g_runEdx], 0
        je      short dibnoedx_&slot
        mov     edx, dword ptr [_lpDriverPDevice]       ; ★ r97: 非 Ext 入口的 lpDIBEngine
        inc     word ptr [_g_edxCount]                  ; 自证: 日志里能看到计数
dibnoedx_&slot:
        cmp     byte ptr [_g_dibDgTrusted], 0
        je      short dibnosl_&slot
        mov     ds, ax                      ; ★ DS = DIBENG 的 DGROUP
dibnosl_&slot:
        push    cx                          ; 先段
        push    bx                          ; 后偏移
        retf                                ; 净栈效果 0 → 参数布局一个字节不动
        endm

DIBTHK  macro   name, param, slot
        public  name
name:
        pop     ax                          ; 调用方返回偏移
        pop     dx                          ; 调用方返回段
        DIBSAVEDS                           ; ★ r98: 先保住调用方 DS 并装好我们的 DS
        push    param                       ; ★ 额外尾参(必须趁 DS 已是本模块时读)
                                            ; r98-marker: DIBTHK loads DS before the tail param
                                            ;   位置: 必须在返回地址【之上】
        push    cs
        push    offset dibret_&slot          ; ★ 我们的桩 = [SP], 参数紧随其后
        DIBSAVE                             ; 调用方现场 → DGROUP LIFO 区
        cmp     byte ptr [_g_dibfnReady], 0
        jne     short dibres_&slot
        call    DIBENSURERESOLVED           ; 首次转发: 懒解析 DIBENG(栈保持完整)
dibres_&slot:
        TRACESLOTMAC slot, param             ; r76c/r98: 首次调用落一行日志
        DIBJUMP slot
        DIBSTUB slot
        endm

DIBFWD  macro   name, slot
        public  name
name:
        pop     ax                          ; 调用方返回偏移
        pop     dx                          ; 调用方返回段
        DIBSAVEDS                           ; ★ r98: 统一序章(本宏不含 DGROUP 参数,
                                            ;   但三个宏同构, 少一处不一致)
        push    cs
        push    offset dibret_&slot          ; ★ 我们的桩 = [SP], 参数紧随其后
        DIBSAVE                             ; 调用方现场 → DGROUP LIFO 区
        cmp     byte ptr [_g_dibfnReady], 0
        jne     short dibres_&slot
        call    DIBENSURERESOLVED           ; 首次转发: 懒解析 DIBENG
dibres_&slot:
        TRACESLOTMAC slot                    ; r76c: 首次调用落一行日志
        DIBJUMP slot
        DIBSTUB slot
        endm

; CDIB —— C 侧(DIBX_*)的同一套跳板, r95 起与 DIBFWD 完全同构:
;   C 调用方同样需要"DS 装成 DIBENG 的 + 回来还我 DS", 否则 C 侧后面按
;   DS 相对读全局量全错(CDPD 早就手工 push ds/pop ds 了, 这里把同一件事
;   做进跳板, C 侧九个调用点一个都不用改)。
DIBXW   macro   name, slot
        public  name
name:
        pop     ax                          ; 调用方返回偏移
        pop     dx                          ; 调用方返回段
        DIBSAVEDS                           ; ★ r98: 统一序章(同 DIBTHK/DIBFWD)
        push    cs
        push    offset dibret_&slot          ; ★ 我们的桩 = [SP], 参数紧随其后
        DIBSAVE                             ; 调用方现场 → DGROUP LIFO 区
        cmp     byte ptr [_g_dibfnReady], 0
        jne     short dibres_&slot
        call    DIBENSURERESOLVED           ; 首次转发: 懒解析 DIBENG
dibres_&slot:
        TRACESLOTMAC slot
        DIBJUMP slot
        DIBSTUB slot
        endm

DIBXW   DIBX_ENABLE,                27
DIBXW   DIBX_EXTTEXTOUT,            28
DIBXW   DIBX_BEGINACCESS,           29
DIBXW   DIBX_ENDACCESS,             30
DIBXW   DIBX_SETPALETTEEXT,         31
DIBXW   DIBX_SETPALETTETRANSLATEEXT, 32
DIBXW   DIBX_SETCURSOREXT,          33
DIBXW   DIBX_MOVECURSOREXT,         34
DIBXW   DIBX_CHECKCURSOREXT,        35
; 【r96 新增 / r97 修正】槽位 36 = DIB_SetPalette(非 Ext, 3 参)。
;   r96 当时的注释写的是"设备由引擎自己记着(CreateDIBPDevice 建的), 所以
;   跳板不需要插尾参" —— 【错】。r96 的 0367:00004036 蓝屏(r95 的 0367:3FA5
;   是同一族问题的另一张脸)就出在这句话上: 非 Ext 入口不是"自己记着", 而是
;   要求调用方把设备指针放在 EDX 里(见 DIBJUMP 里的字节级证据)。现在由
;   DIBJUMP 统一注入, 本槽位与 DIBFWD 依旧同构(不插尾参), 但多了 EDX。
DIBXW   DIBX_SETPALETTE,            36

; 额外参数变量 _lpDriverPDevice / _wPalettized 与地址表 _g_dibfn 的
; extrn 都在文件顶部的 _DATA 段块内(见该处注释：写在 _TEXT 块里会被
; 编成 CS 相对寻址，push/jmp 用到的将是代码段垃圾)。

; --- 需要插入尾参的转发(槽位 20..26，对应 DIB_*Ext) ----------------------
DIBTHK  RealizeObject,          _lpDriverPDevice,   20
DIBTHK  EnumObj,                _lpDriverPDevice,   21
DIBTHK  DibBlt,                 _wPalettized,       22
DIBTHK  GetPalette,             _lpDriverPDevice,   23
DIBTHK  SetPaletteTranslate,    _lpDriverPDevice,   24
DIBTHK  GetPaletteTranslate,    _lpDriverPDevice,   25
DIBTHK  UpdateColors,           _lpDriverPDevice,   26

; --- 纯透传转发(槽位 0..19，对应 DIB_同名函数) -----------------------------
DIBFWD  BitBlt,         0
DIBFWD  ColorInfo,      1
DIBFWD  Control,        2
DIBFWD  EnumDFonts,     3
DIBFWD  Output,         4
DIBFWD  Pixel,          5
DIBFWD  Strblt,         6
DIBFWD  ScanLR,         7
DIBFWD  DeviceMode,     8
DIBFWD  GetCharWidth,   9
DIBFWD  DeviceBitmap,   10
DIBFWD  FastBorder,     11
DIBFWD  SetAttribute,   12
DIBFWD  CreateDIBitmap, 13
DIBFWD  DibToDevice,    14
DIBFWD  StretchBlt,     15
DIBFWD  StretchDIBits,  16
DIBFWD  SelectBitmap,   17
DIBFWD  BitmapBits,     18
DIBFWD  Inquire,        19

;-----------------------------------------------------------------------------
; SetCursor 公开别名：实体是 display.c 的 SetCursor_driver
; (C 侧若直接叫 SetCursor 会与 windows.h 的 USER 函数冲突)
;-----------------------------------------------------------------------------
        extrn   SETCURSOR_DRIVER : far
        public  SetCursor
SetCursor:
        jmp     SETCURSOR_DRIVER

;=============================================================================
; CDPD —— 用 DDK 自己的 CreateDIBPDevice(dibeng.lib 序号 300) 建 PDEVICE(r84)
;   C 原型: WORD __far __pascal Cdpd( void )   —— 【无参数】
;   入参经 DGROUP 全局量(16:16 远指针打包成 DWORD, 低字=偏移):
;       _CdpdInfo / _CdpdDevice / _CdpdBits / _CdpdFlags
;   返回: AX = CreateDIBPDevice 返回值的低 16 位(0 = 失败, 非 0 = 位图字节数)
;
;   【为什么必须走汇编】dibeng.lib(ddk/dibeng.lbc) 里的名字是混合大小写
;   "++CreateDIBPDevice.DIBENG.300", 而 Watcom 对 WINAPI(__pascal) 函数一律
;   生成【大写】符号 —— C 侧直接声明会链接不上。汇编里 extrn 用原样名字即可
;   (与下方 DIBENGIMPORTANCHOR 的 DIB_Enable 同一手法)。
;
;   【参数怎么压】Watcom __pascal 的栈布局在本文件里有权威实例(见
;   VDDGETDISPCONF 的注释): 第一个参数在【最高】地址、最后一个参数最低,
;   即"从左到右依次压栈"; 远指针在栈上是"低地址放偏移、高地址放选择子"
;   (同处: [bp+6]=buf 偏移 [bp+8]=buf 段)。故顺序 = lpInfo→lpDevice→lpBits
;   →wFlags, 每个远指针先压段再压偏移。DIBTHK 宏"把额外参数插在所有参数
;   之后"的做法(弹返回地址→压尾参→压回返回地址)与此互为印证。
;
;   【AX 必须装 DIBENG 的 DGROUP】DIBENG 所有导出函数的序章都是 mov ds,ax
;   (见 DIBFWD/DIBTHK 处注释), CreateDIBPDevice 亦然。
;   【返回值约定】DDK 明写结果在 EAX(不是 DX:AX); C 侧只取低 16 位判成败,
;   故这里原样返回 AX 不做转换。
;   【DS 必须恢复】引擎内部会把 DS 装成它自己的 DGROUP, 直接 retf 回去会让
;   C 侧后面按 DS 相对读全局量时全错 —— 故 push ds / pop ds 包住这次调用。
;=============================================================================
        extrn   CreateDIBPDevice:FAR
        public  CDPD
CDPD proc far
        push    ds                          ; Windows 约定: DS 必须保留
        push    es                          ; (ES 本可破坏, 一并保留, 零代价)
        mov     ax, DGROUP
        mov     ds, ax
        mov     es, ax
        assume  ds:DGROUP, es:DGROUP
        push    word ptr [_CdpdInfo+2]      ; lpInfo   (第 1 个参数, 最深)
        push    word ptr [_CdpdInfo]
        push    word ptr [_CdpdDevice+2]    ; lpDevice
        push    word ptr [_CdpdDevice]
        push    word ptr [_CdpdBits+2]      ; lpBits
        push    word ptr [_CdpdBits]
        push    [_CdpdFlags]                ; wFlags   (最后压, 紧贴返回地址)
        mov     ax, [_wDibDgroup]           ; DIBENG 的 DGROUP(其序章要用)
        call    CreateDIBPDevice
        pop     es
        pop     ds
        retf
CDPD endp

;-----------------------------------------------------------------------------
; DIBENVPROBE —— r96: 把当前的 FS/GS 抄进 DGROUP(调色板取证用)
;   C 原型: void __far __pascal DibEnvProbe( void )
;   【为什么需要它】16 位 C 没有任何办法读段寄存器, 而 r95 的日志已证明
;   DIBENG 的代码段(0367)里存在 `64 88 05` = mov fs:[di],al 这样的
;   FS 相对访问 —— 引擎的内层循环靠 FS 直接碰表面。若进引擎那一刻 FS 是 0
;   或已失效的选择子, 第一条 fs: 访问就是 0D 蓝屏, 而崩点会精确落在某条
;   带 FS 前缀的指令上。本过程只"读", 一个字节的环境都不改。
;   【写法】mov r16,Sreg 用显式机器码(8C E0 = mov ax,fs, 8C E8 = mov ax,gs),
;   不依赖汇编器对"段寄存器做源操作数"的支持程度 —— 与本文件既有纪律一致:
;   只用跑通过的那一套写法。
;-----------------------------------------------------------------------------
        public  DIBENVPROBE
DIBENVPROBE proc far
        mov     ax, DGROUP
        mov     ds, ax                      ; 不信任调用方 DS(与转发器同规矩)
        db      8Ch, 0E0h                   ; mov ax, fs
        mov     [_g_envFS], ax
        db      8Ch, 0E8h                   ; mov ax, gs
        mov     [_g_envGS], ax
        retf
DIBENVPROBE endp

;-----------------------------------------------------------------------------
; DIBENGIMPORTANCHOR —— DIBENG.DLL 静态导入锚点(【永不被调用】)
;   【为何存在】(./22 实锤) 四个在售参考驱动(vmdisp9x 系列的
;   vesamini/boxvmini/qemumini/vmwsmini)的 NE 导入表全部静态包含
;   DIBENG —— KERNEL 加载显示驱动时就把 DIBENG.DLL 作为依赖装入,
;   Enable 期 GetModuleHandle("DIBENG") 因此必有值。本驱动此前
;   零静态导入: Enable 期 DIBENG 尚未装载(GetModuleHandle=0),
;   LoadLibrary 兜底在 KERNEL 内 GP(0d:00cf:1b31 蓝屏)。
;   【只做装载牵引】本过程没有任何调用点; 唯一的那条远调用重定位
;   把 DIBENG.5(经 dibeng.lib 解析)钉进导入表。运行期的全部 DIB
;   引擎调用仍走 g_dibfn[] 动态解析(按名+序号兜底), 行为完全动态。
;   【序号兼容性】DIBENG 导出表是 Win95 起冻结的系统 ABI(DDK 的
;   dibeng.lbc 即其权威清单), 与 KERNEL/GDI 同类; 锚定资历最老的
;   DIB_Enable(5), 与所有原厂显示驱动等价。
; 【符号为何写在汇编】规避 Watcom C 的符号装饰差异, extrn 名即
;   链接名, 与 dibeng.lib 的 lbc 定义逐字符一致。
;-----------------------------------------------------------------------------
        extrn   DIB_Enable:FAR
DIBENGIMPORTANCHOR proc far
        ret                         ; 无调用者; 到此为止永不执行
        call    DIB_Enable          ; 死代码: 仅为产生导入重定位
        ret
DIBENGIMPORTANCHOR endp

_TEXT   ends

;-----------------------------------------------------------------------------
; END 指令指定模块入口 —— LINK(MSVC)/WLINK(Watcom) 据此填写
; NE 头的起始 CS:IP，即整个 display.drv 的加载入口。
;-----------------------------------------------------------------------------
        END     DriverInit
