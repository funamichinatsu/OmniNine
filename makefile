#=============================================================================
#
#  MAKEFILE — 构建整个通用 VBE 显示驱动
#
#  两个产物:
#    minivdd.vxd —— ring 0 mini-VDD(minivdd.asm)
#    omninine.drv   —— 16 位 display.drv(display.c + drv.c + vddapi16.asm)
#
#  【r76 摘要】(详细分析见 R76_CHANGES.md)
#    · 帧缓冲映射改由 display.drv 在 ring 3 用 DPMI 0800h + DPMI LDT
#      选择子自建(原来由 mini-VDD 在 ring 0 建 GDT 选择子交下来,
#      实机第一次写显存就 0D 蓝屏 031F:00090B)。
#    · minivdd.asm 删除全部 MM/描述符服务实验代码;
#      VESA_SUPPORT 恒返回 NC(真 VESA BIOS 放行, 修 DOS 侧黑屏/花屏)。
#    · vddapi16.asm 新增 FBCLEAR(32 位偏移清屏)。
#    · vbeshare.h / vbeshare.inc 的 VBEEXECREQ 瘦身到 48 字节,
#      版本号 0x010C → 0x010D(两侧必须同时改)。
#
#  【工具链总览】
#   minivdd.vxd 必须用 Microsoft Win95/98 DDK 自带的工具构建:
#     ML.EXE   MASM 6.11+(-coff 生成 COFF 目标文件)
#     LINK.EXE DDK 版链接器(带 -VXD 选项; 不是 VC++ 的普通 LINK)
#   还需要 DDK 的 INC32 目录(VMM.INC)在 INCLUDE 路径中。
#
#   omninine.drv 有两条等价路线:
#     A. Open Watcom 1.9+/2.0(本 makefile 的默认目标 drv-ow):
#          wcc(16 位中模型) + wasm + wlink
#        优点: 免费可得, 一条命令出完整产物; wlink 直接写 NE。
#     B. MSVC 1.52 + Win9x DDK(见文件尾的 drv-msvc 伪目标注释):
#          cl /c /AMw /GD + masm + DDK LINK 5.x(omninine.def)
#
#  【为何 omninine.drv 用中模型(/AM, -mm)】
#   16 位 Windows 驱动的经典模型: 代码远、数据近。所有跨段调用
#   (_TEXT↔_INIT↔drv.c 段)因此天然合法; 单数据段 DGROUP 与
#   vddapi16.asm 中 "mov ax, DGROUP" 的假设一致。此外 drv.h 中的
#   跨模块函数与 display.c 中跨段的静态函数均显式标注了 __far,
#   双保险。
#
#  【为何 display.c 与 drv.c 的打包选项不同】
#   display.c 必须 -zp2(默认对齐): GDIINFO/DIBENGINE 由 GDI/DIBENG
#     按自然对齐读写, 若按 1 字节打包, DIBENGINE 里的 DWORD 字段
#     会错位, DIBENG.DLL 立即崩溃。
#   drv.c 用 -zp1: 历史上为 VBE BIOS 的紧凑结构服务; 现在 drv.h/
#     vbeshare.h 已自带 #pragma pack(1), 此开关是双保险, 无害。
#
#  【Win16 DLL 的 SS!=DS 前提】
#   MSVC /Aw、Watcom -zu 都声明"栈段不等于数据段"; drv.c 内部用
#   静态 REGS 缓冲区 + near 强转配合该模型(见 drv.c 文件头), 改动
#   drv.c 时请保持这一约定。
#
#=============================================================================

# --- Win9x DDK 工具(minivdd.vxd) -------------------------------------------
ML       = ml
# 【为何必须 -DMASM6、且不能用 -Zm】DDK 的 INC32 头文件(含 VMM.INC)
# 依赖命令行定义的 MASM6 条件宏来选择 MASM 6 语法的解析路径; 不定义
# 会落入 6.11c 无法解析的旧分支(VMM.INC 报 error A2008 : EQU, 并级联
# 出 A4004/A2107)。-Zm 是 MASM 5.1 兼容模式, DDK VxD 源码不用它构建。
# 标准配方(所有 Win95/98 DDK VxD 教程一致):
#   ml -coff -c -Cx -DMASM6 -DBLD_COFF -DIS_32 file.asm
MLFLAGS  = -c -coff -DBLD_COFF -DIS_32 -DMASM6 -W2 -Cx
# 【坑】这里必须是 Win9x DDK 的 LINK.EXE(唯一支持 -vxd 的链接器)。
# Open Watcom 2.0 的 binnt 里有一个同名的 link.exe 克隆(横幅是
# "Compatible with LINK Version 7.0.0", 警告编号 Wxxxx), 它会
# "Ignoring unsupported option(s): /VXD" 并产出不是 VxD 的假文件。
# 若 PATH 里 OW 排在前面就会抓错: 构建 VxD 时请让 DDK 的 BIN 排在
# PATH 最前, 或用全路径/命令行覆盖:
#   nmake LINK="C:\DDK\BIN\LINK.EXE" minivdd.vxd
# 成功判据: 横幅为 Microsoft ... Linker Version 5.xx, 无任何
# Ignoring/W1023 警告; 产物是 LE 格式(0x3C 指向 "LE" 签名)。
LINK     = link
# 注: 老资料里的 -nopadcode/-nopaddata(段不填充对齐, 纯体积优化)在
# 部分 DDK 链接器上不被认识, 只会刷无害的 LNK4044 警告——不采用。
LINKFLAGS = -vxd -map:minivdd.map

# --- Open Watcom 工具(omninine.drv) -------------------------------------------
WCC      = wcc
# 公共: -q 安静 -wx 全警告 -s 关栈检查 -zu SS!=DS -zls 去掉默认库记录
#       -zW DLL 出入口序列为 __loadds 导出 -mm 中模型 -bt=windows
WCFLAGS  = -q -wx -s -zu -zls -zW -mm -bt=windows -i=ddk
WASM     = wasm
WASMFLAGS= -q
WLINK    = wlink
WLIB     = wlib
# 【wrc 挂资源 —— 不可省略!】wlink 在模块没有任何资源时不生成 NE
# 资源表, 而让 NE+0x24 的"资源表偏移"指向驻留名表; KRNL386 加载期
# 遍历资源表会读到垃圾数据, 直接按"可执行文件损坏"拒载(bootlog:
# LoadFail code 0000, 入口不会被调用, Win98SE/WinME 一致)。挂上
# omninine.rc 里的占位资源后资源表恢复正常。vmdisp9x 的构建同样有
# 这一步。必须在 fixver 之前执行(fixver 改的是 NE 头固定偏移,
# 不受资源表影响)。
WRC      = wrc
# 【wlink 路线的必要收尾】wlink 把 NE 头"期望 Windows 版本"写成 3.0
# 且没有选项可改；Win9x GDI 拒载标 3.x 的显示驱动(入口都不会被调用,
# bootlog 报 LoadFail Failure code is 0000)。fixver 把该字段改成 4.0,
# 与 vga.drv 一致。MSVC/DDK LINK 5.x 路线无此问题(自动写 4.0)。
FIXVER   = fixver.exe

all: minivdd.vxd omninine.drv

#=============================================================================
# minivdd.vxd(DDK 工具链)
# 【坑: DDB 必须导出】Declare_Virtual_Device 只在 obj 里生成公开符号
# MINIVDD_DDB(无 C 风格前导下划线), VxD 加载器按 LE 入口表序号 1 找
# DDB, 必须经 minivdd.def 的 "EXPORTS MINIVDD_DDB @1" 显式引出; def 里
# 的 SEGMENTS 块再把各段的 PRELOAD/NONDISCARDABLE 属性标齐(加载器会逐
# 段校验类型)。缺了 def 时 LINK 照样产出 LE 文件(头部完全正常), 但
# VMM 找不到/不接受 DDB, bootlog 报:
#   Dynamic load failed minivdd.vxd : Not a valid device - possibly corrupt
# 且 ring3 驱动永远等不到它(系统回退 VGA)。
#=============================================================================
minivdd.vxd: minivdd.obj minivdd.def
	$(LINK) $(LINKFLAGS) -def:minivdd.def -out:minivdd.vxd minivdd.obj

# 【vbeshare.h 也在依赖表里 —— 故意的, 不要删】
# minivdd.asm 只 INCLUDE vbeshare.INC(汇编侧镜像), 编译期确实不读 .h。
# 但这两份文件是【同一份 ABI 的手写拷贝】: 谁只改了 .h 而没同步 .inc,
# 或者同步了 .inc 却没重建 VxD, 都会让 ring 0 与 ring 3 对同一块共享内存
# 读出不同的字段 —— 典型症状正是"寄存器看着全对, 屏幕却是花的"。
# r121 已经栽过一次(CHECK.bat 里那条 "a constant added to vbeshare.h but
# NOT to vbeshare.inc")。把 .h 挂进依赖表, 至少保证"改了 .h 就一定重建
# VxD", 把"部署了旧 VxD"这一种可能直接消掉。
# 偏移/长度是否真的一致, 由 abi_check.py 机械核对(本包内附带)。
minivdd.obj: minivdd.asm vbeshare.inc vbeshare.h
	$(ML) $(MLFLAGS) -Fominivdd.obj minivdd.asm

#=============================================================================
# omninine.drv(Open Watcom 工具链)
#
# 【r127 修】这里原来是一条【用反斜杠续行的依赖表】:
#     omninine.drv: display.obj ... dibeng.lib \
#                res_config.bin res_colortab.bin
#   本机实测: 某些 wmake 版本会把那条【续行】当成一条【命令】去执行,
#   于是报
#     Error(E14): Cannot execute (res_config.bin): Exec format error
#   —— 症状出现在【debug 目标】上, 而同一份 makefile 的 min/rel 目标
#   (它们用的是【多条同名依赖行】而不是续行)完全正常。也就是说:
#   续行写法在本工具链上不可靠。
#   现在 debug 目标也改成多条同名依赖行, 与 min/rel 结构一致 ——
#   三种构建不再有"某个目标用了另一种语法"这种差异。
#=============================================================================
omninine.drv: display.obj drv.obj vddapi16.obj omninine.lnk omninine.rc fixver.exe dibeng.lib
omninine.drv: res_config.bin res_colortab.bin
	$(WLINK) op quiet @omninine.lnk
	$(WRC) -q -bt=windows omninine.rc omninine.drv
	$(FIXVER) omninine.drv

#=============================================================================
# r126: 三个构建各自独立的目标 —— 开关只出现在【这一处】, 命令行上没有
# 任何需要你记住的东西。
#
#   nmake omninine.drv        debug   (默认, 写日志)
#   nmake omninine-min.drv    MIN     (release 同序 + 决定性行)
#   nmake omninine-rel.drv    release (零日志代码)
#
# 【为什么这样改】./122 与 ./123 两次 MIN 构建都白跑, 根因是命令行参数里
# 漏掉前导 -D:
#     nmake DIAGFLAG=DVBE9X_DIAG_MIN omninine.drv   <-- 错
#     wcc -q ... DVBE9X_DIAG_MIN -zp2 -fo=display.obj display.c
#     Error! E1139: Command line contains more than one file to compile
# wcc 把它当成了第二个源文件。而横幅里紧跟的三条无关 W303 警告把这行真
# 错因盖住了, 读起来像"编译器不支持这种构建"。
#
# 【所以 r126 那种"把正确写法写进注释"的修法不算修法】—— 它要求你每次
# 都记得一个前缀, 于是下一次照样会漏。这里改成: 【DIAGFLAG 这个变量
# 彻底不存在了】。变量不存在, 那条错误路径就物理上不可能再被走到;
# 而下面三个目标各自把开关写死在配方里, 互相之间不会串味。
#
# 【三个目标各自删 obj】这是必须的: 三种构建共用 display.obj / drv.obj,
# 上一次的 obj 被复用会让这一轮的结论完全失真(而且不留痕迹)。所以每个
# 目标开跑前先清掉旧 obj —— 你不需要记得先 nmake clean。
#=============================================================================
omninine-min.drv: display.c drv.c drv.h vbeshare.h vddapi16.asm vbeshare.inc
omninine-min.drv: omninine.lnk omninine.rc fixver.exe dibeng.lib
omninine-min.drv: res_config.bin res_colortab.bin
	@echo [omninine] build MIN  (release ordering + decisive lines)  -DVBE9X_DIAG_MIN
	-del display.obj drv.obj vddapi16.obj omninine.drv omninine.map
	$(WCC) $(WCFLAGS) -DVBE9X_DIAG_MIN -zp2 -fo=display.obj display.c
	$(WCC) $(WCFLAGS) -DVBE9X_DIAG_MIN -zp1 -fo=drv.obj drv.c
	$(WASM) $(WASMFLAGS) -fo=vddapi16.obj vddapi16.asm
	$(WLINK) op quiet @omninine.lnk
	$(WRC) -q -bt=windows omninine.rc omninine.drv
	$(FIXVER) omninine.drv
	copy /y omninine.drv omninine-min.drv >nul
	@echo [omninine] built omninine-min.drv  (reference size, r129 sources: 39272)

omninine-rel.drv: display.c drv.c drv.h vbeshare.h vddapi16.asm vbeshare.inc
omninine-rel.drv: omninine.lnk omninine.rc fixver.exe dibeng.lib
omninine-rel.drv: res_config.bin res_colortab.bin
	@echo [omninine] build RELEASE  (no logging code at all)
	-del display.obj drv.obj vddapi16.obj omninine.drv omninine.map
	$(WCC) $(WCFLAGS) -zp2 -fo=display.obj display.c
	$(WCC) $(WCFLAGS) -zp1 -fo=drv.obj drv.c
	$(WASM) $(WASMFLAGS) -fo=vddapi16.obj vddapi16.asm
	$(WLINK) op quiet @omninine.lnk
	$(WRC) -q -bt=windows omninine.rc omninine.drv
	$(FIXVER) omninine.drv
	copy /y omninine.drv omninine-rel.drv >nul
	@echo [omninine] built omninine-rel.drv  (reference size, r129 sources: 34628)

# 便捷别名: nmake min / nmake rel
min: omninine-min.drv
rel: omninine-rel.drv

# r78: omninine.rc 里那两个 oembin 资源的二进制内容(CONFIG / 控制面板色表)。
# 随包提供, 正常构建无需重新生成; 万一被删, 用下面的配方从 .c 重建
# (与在售参考驱动 makefile 的做法逐字一致: 编译成 obj, 再让 wlink 以
#  "output raw" 把数据段原样倒成 .bin)。
res_config.bin: res_config.c
	$(WCC) -q res_config.c
	$(WLINK) op quiet disable 1014, 1023 name res_config.bin sys dos output raw file res_config.obj

res_colortab.bin: res_colortab.c
	$(WCC) -q res_colortab.c
	$(WLINK) op quiet disable 1014, 1023 name res_colortab.bin sys dos output raw file res_colortab.obj

# fixver: NE 头版本修正工具(见 fixver.c 文件头注释), 用宿主机的
# 32 位编译器构建一次即可
fixver.exe: fixver.c
	wcl386 -q -fe=fixver.exe fixver.c

# 真机排错(回退 VGA/代码 24 时): 诊断构建会写 C:\OMNININE.LOG,
# 据此定位失败点。
# 【默认即诊断构建】真机联调期忘了带开关是最常见的返工源(已两次
# 白跑), 故默认打开; 发布构建显式覆盖为空:
#   nmake omninine.drv                    ← 诊断构建(默认, 写日志)
#   nmake DIAGFLAG= omninine.drv          ← 发布构建(零日志代码)
#   nmake DIAGFLAG=-DVBE9X_DIAG_MIN omninine.drv   ← MIN 构建(release 同序 + 决定性行)
#
# 【r126 修正: 三种写法都收】./122 的 DVBE9X_DIAG_MIN 那次构建失败是
#   `E1139: Command line contains more than one file to compile` —— 用户按
#   README 里写的 `nmake DIAGFLAG=DVBE9X_DIAG_MIN ...`(漏了前导 -D),
#   wcc 于是把它当成【第二个源文件】, 而横幅里的 warning 全是无关的 W303,
#   真正的错因(第一行)很容易被当成"编译器不支持"。
#   修法: 本变量名【自带前导】, nmake 里直接写 $(DIAGFLAG)。于是
#     nmake DIAGFLAG=-DVBE9X_DIAG_MIN omninine.drv   ← 正确
#     nmake DIAGFLAG=-dVBE9X_DIAG_MIN omninine.drv   ← 也对(/D 与 /d 等价)
#     nmake DIAGFLAG=DVBE9X_DIAG_MIN  omninine.drv   ← 仍然错, 但见下
#   最后那种写法的失败在 ./122 里被 W303 的噪声盖住了, 所以 r126 起在
#   buildvxd.bat 的检查清单里把这三条【逐字】列出来(见该文件"三种构建")。
DIAGFLAG = -dVBE9X_DIAG

# 注意: display.c 故意【不】加 -zp1(见文件头说明)
display.obj: display.c drv.h vbeshare.h ddk/gdidefs.h ddk/dibeng.h ddk/minivdd.h
	$(WCC) $(WCFLAGS) $(DIAGFLAG) -zp2 -fo=display.obj display.c

# 【drv.obj 也必须带 $(DIAGFLAG)】DrvInitCore 的 VxD 初始化步骤
# (VxD_Connect/VxDCallApi)的括日志在 drv.c 里; 此前本行漏掉该开关,
# 即使命令行带了 DIAGFLAG, drv.c 的 DIAG 也全被编成空 —— 排障时
# "VxD 阶段无日志"的元凶。
drv.obj: drv.c drv.h vbeshare.h
	$(WCC) $(WCFLAGS) $(DIAGFLAG) -zp1 -fo=drv.obj drv.c

vddapi16.obj: vddapi16.asm
	$(WASM) $(WASMFLAGS) -fo=vddapi16.obj vddapi16.asm

# 【dibeng.lib 已恢复】(./22 实锤后 reversal) 此前为零静态导入:
# Enable 期 DIBENG.DLL 尚未装载, GetModuleHandle("DIBENG")=0,
# LoadLibrary 兜底在 KERNEL 内 GP(0d 蓝屏)。四个在售参考驱动
# (vmdisp9x 系列)的导入表全部静态包含 DIBENG —— 这是 DDK 的原生
# 机制: KERNEL 加载显示驱动时把 DIBENG 作为依赖先行装入。
# 恢复时的收敛原则: 整张 lbc 只锚定 DIB_Enable(5)一个序号
# (lbc 46 条全建进 lib, 但只有被引用的条目会进导入表), 锚点
# 是 vddapi16.asm 里永不执行的死代码; 运行期调用仍全走
# g_dibfn[] 动态解析。DIBENG 导出表自 Win95 冻结(与 KERNEL 同类),
# 不存在"另一个版本"的兼容问题。
dibeng.lib: ddk/dibeng.lbc
	$(WLIB) -c -q dibeng.lib @ddk/dibeng.lbc

# 【已废弃注释的更正】旧注释"dibeng.lib 已废弃, 可直接删除"不再成立,
# 见上方 dibeng.lib 规则说明。

#=============================================================================
# omninine.drv(MSVC 1.52 + Win9x DDK 备选路线) —— 手工命令, 供参考:
#
#   cl /c /W3 /AMw /GD /Zp2 /I ddk display.c
#   cl /c /W3 /AMw /GD /Zp1 drv.c
#   masm /Mx vddapi16.asm;
#   link /NOD /M /L display.obj drv.obj vddapi16.obj, omninine.drv, omninine.map, \
#        mlibcew.lib libw.lib, omninine.def
#
# 说明: mlibcew.lib = MSVC 中模型 Windows C 运行库; libw.lib = Windows
# 导入库(GetModuleHandle/GetProcAddress/GlobalSmartPageLock 等)。
# 不再需要 dibeng.lib —— DIBENG 入口全部动态解析(见 display.c)。
# /NOD 禁止默认库(避免与 DLL 入口模型冲突)。
#=============================================================================

clean:
	-del minivdd.obj minivdd.vxd minivdd.map
	-del display.obj drv.obj vddapi16.obj omninine.drv omninine.map omninine.res
	-del omninine-min.drv omninine-rel.drv
	-del fixver.exe fixver.obj
	-del dibeng.lib
	-del res_config.obj res_colortab.obj
