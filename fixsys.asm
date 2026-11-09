;=============================================================================
; FIXSYS.ASM (v2) --- 以 CONFIG.SYS 的 DEVICE= 方式加载的修复驱动(带当前路径提示符)
;
; 【为什么用 .SYS 而不是 .COM】
;   .COM TSR 必须留着一个 256 字节的 PSP; 设备驱动没有 PSP, 常驻只需
;   设备头(18 字节) + 代码 + 数据。省下约 256 字节, 代价是多出约 60 字节的
;   设备驱动框架(设备头 + 策略例程 + 中断例程)。
;
; 【结构】
;   dev_header  : 设备头(字符设备, 名字 FIXSYS)
;   strategy    : 只保存请求头指针(ES:BX), 必须 retf
;   interrupt   : 命令 0 = INIT:
;                   1) 打印一行启动信息(INT 10h AH=0Eh, 不经过 DOS)
;                   2) 保存 INT 2Fh / 1Ch / 28h 旧向量, 装本驱动的处理程序
;                   3) 在请求头偏移 0Eh 写入"常驻末尾地址"(DOS 据此决定保留多少)
;
; 【运行时行为(与已验证的 FIXTSR12 相同)】
;   INT 2Fh AX=4001h  -> 置标志
;   INT 1Ch           -> 修复: 0500h / 1114h(BL=0) / 0600h 清屏 / 光标归位
;   INT 28h           -> 查询当前盘(AH=19h)与当前目录(AH=47h, DL=0),
;                        立即用 BIOS 电传打印 "X:\路径>"
;   全程没有任何 DOS 输出调用(INT 28h 里调 AH=02h 会蓝屏, 已实测)。
;
; 【编译】
;   wasm -bt=DOS -mt -q fixsys.asm
;   wlink system dos com file fixsys.obj name fixsys.sys
;   (COM 格式输出即平坦二进制, 正是 .SYS 需要的)
;   MASM 备用: ml /AT /Zm fixsys.asm  +  link16 /TINY fixsys.obj,fixsys.sys;
;
; 【安装】CONFIG.SYS 加一行:
;   DEVICE=FIXSYS.SYS
;   开机时应看到: FIXSYS INIT OK
;=============================================================================
        .model  tiny
        .code
        org     0

;----------------------------------------------------------------- 设备头
dev_header:
        dd      -1                      ; 下一个设备(由 DOS 填写)
        dw      8000h                   ; 属性: 字符设备
        dw      OFFSET strategy
        dw      OFFSET interrupt
        db      'FIXSYS  '              ; 设备名(8 字符)
        db      0                       ; 单字符设备保留

;----------------------------------------------------------------- 策略例程
strategy:
        mov     word ptr cs:[req_off], bx
        mov     word ptr cs:[req_seg], es
        retf

;----------------------------------------------------------------- 中断例程
interrupt:
        pushf
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        ; 【v2 修正1】先把请求头指针存进驱动自己的变量 —— 后面任何 INT 21h
        ;   都会破坏 BX/ES, 而收尾时要往这个请求头里写"常驻末尾地址"。
        mov     word ptr cs:[req_off], bx
        mov     word ptr cs:[req_seg], es
        ; 命令码在请求头偏移 2
        mov     al, es:[bx+2]
        or      al, al
        jnz     i_done
        ; ---------------- INIT ----------------
        push    cs
        pop     ds
        ; 打印启动信息(纯 BIOS)
        mov     si, offset ban
i_p1:
        mov     al, [si]
        or      al, al
        je      i_p2
        mov     ah, 0Eh
        mov     bx, 0007h
        int     10h
        inc     si
        jmp     i_p1
i_p2:
        ; ---- 保存旧向量 ----
        mov     ax, 352Fh
        int     21h
        mov     word ptr [old2f],   bx
        mov     word ptr [old2f+2], es
        mov     ax, 351Ch
        int     21h
        mov     word ptr [old1c],   bx
        mov     word ptr [old1c+2], es
        mov     ax, 3528h
        int     21h
        mov     word ptr [old28],   bx
        mov     word ptr [old28+2], es
        ; ---- 装新向量 ----
        push    ds
        push    cs
        pop     ds
        mov     dx, offset new2f
        mov     ax, 252Fh
        int     21h
        mov     dx, offset new1c
        mov     ax, 251Ch
        int     21h
        mov     dx, offset new28
        mov     ax, 2528h
        int     21h
        pop     ds
        ; ---- 告诉 DOS 常驻到哪 ----
        ; 【v2 修正2】上面那些 INT 21h 已经破坏了 BX/ES, 必须重新装载请求头指针。
        mov     bx, word ptr cs:[req_off]
        mov     ax, word ptr cs:[req_seg]
        mov     es, ax
        mov     word ptr es:[bx+0Eh], OFFSET resident_end
        mov     word ptr es:[bx+10h], cs
i_done:
        mov     bx, word ptr cs:[req_off]
        mov     ax, word ptr cs:[req_seg]
        mov     es, ax
        mov     word ptr es:[bx+3], 0100h      ; 状态: 完成
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        popf
        retf

;----------------------------------------------------------------- 运行时
new2f:
        cmp     ax, 4001h
        jne     n2f_chain
        mov     byte ptr cs:[flag], 1
n2f_chain:
        jmp     dword ptr cs:[old2f]

new1c:
        cmp     byte ptr cs:[flag], 0
        je      n1c_chain
        cmp     byte ptr cs:[busy], 0
        jne     n1c_chain
        mov     byte ptr cs:[busy], 1
        mov     byte ptr cs:[flag], 0
        push    ax
        push    bx
        push    cx
        push    dx
        mov     ax, 0500h
        int     10h
        mov     ax, 1114h
        mov     bl, 0
        int     10h
        mov     ax, 0600h
        mov     bh, 07h
        xor     cx, cx
        mov     dx, 184Fh
        int     10h
        mov     ah, 02h
        xor     bh, bh
        xor     dx, dx
        int     10h
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        mov     byte ptr cs:[needp], 1
        mov     byte ptr cs:[busy], 0
n1c_chain:
        jmp     dword ptr cs:[old1c]

new28:
        cmp     byte ptr cs:[needp], 0
        je      n28_chain
        cmp     byte ptr cs:[busy], 0
        jne     n28_chain
        mov     byte ptr cs:[busy], 1
        mov     byte ptr cs:[needp], 0
        push    ax
        push    bx
        push    dx
        push    si
        push    ds
        push    cs
        pop     ds
        mov     ah, 19h
        int     21h
        mov     [drive], al
        xor     dl, dl                  ; DL=0: 当前盘(注意 1 基编号)
        mov     si, offset pathbuf
        mov     ah, 47h
        int     21h
        jc      q_fail
        jmp     q_ok
q_fail:
        mov     byte ptr [pathbuf], 0
q_ok:
        mov     ah, 02h
        xor     bh, bh
        xor     dx, dx
        int     10h
        mov     al, [drive]
        add     al, 'A'
        call    tput
        mov     al, ':'
        call    tput
        mov     al, '\'
        call    tput
        mov     si, offset pathbuf
pp_lp:
        mov     al, [si]
        or      al, al
        je      pp_gt
        call    tput
        inc     si
        jmp     pp_lp
pp_gt:
        mov     al, '>'
        call    tput
        pop     ds
        pop     si
        pop     dx
        pop     bx
        pop     ax
        mov     byte ptr cs:[busy], 0
n28_chain:
        jmp     dword ptr cs:[old28]

tput:
        push    ax
        push    bx
        mov     ah, 0Eh
        xor     bh, bh
        mov     bl, 07h
        int     10h
        pop     bx
        pop     ax
        ret

;----------------------------------------------------------------- 数据
req_off dd 0
req_seg dw 0
old2f   dd 0
old1c   dd 0
old28   dd 0
flag    db 0
busy    db 0
needp   db 0
drive   db 0
pathbuf db 64 dup (0)
ban     db 'FIXSYS v2 INIT OK',13,10,0

resident_end:

        end
