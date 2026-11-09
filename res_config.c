/*=============================================================================
 *
 *  res_config.c —— 资源 1("oembin" 类型, 名 "1")的内容定义: CONFIG_BIN。
 *
 *  【这是什么】Windows 3.x DDK 里显示驱动的 "CONFIG" 资源: 一组与显示
 *  硬件相关的界面度量(滚动条滑块尺寸、图标/光标缩放比、窗口边框宽度)
 *  加 20 个默认系统颜色。Win9x 的 USER 在显示驱动初始化阶段仍会按
 *  "向后兼容要求"去取它 —— 在售参考驱动(boxv9x / vmdisp9x 系列)的
 *  display.rc 里都带着它, 注释原文: "Most of the contents is defined by
 *  backward compatibility requirements."
 *
 *  【为什么本文件存在】r78 之前的 vbe9x.rc 只有一个占位的 RCDATA 资源
 *  (它的作用是让 wlink/wrc 生成正常的 NE 资源表, 否则 KRNL386 会按
 *  "可执行文件损坏"拒载)。显示驱动该有的 oembin 资源一个都没有。
 *  本文件与 res_colortab.c 一起把 config/colortab 两个资源补齐。
 *
 *  【本文件同时是 res_config.bin 的"生成配方"】res_config.bin 就是本
 *  文件编译出来的数据段的原样字节(参考驱动 makefile 的做法):
 *      wcc -q res_config.c
 *      wlink op quiet disable 1014, 1023 name res_config.bin sys dos \
 *            output raw file res_config.obj
 *  包内已附带生成好的 res_config.bin(98 字节), 正常情况下无需重跑。
 *  【自包含】故意不 #include 任何头文件(不依赖 windows.h/gdidefs.h 的
 *  包含路径), 结构体在本文件内按 DDK 定义内联, 保证任何工具链都能编。
 *
 *===========================================================================*/

typedef unsigned char  rc_BYTE;
typedef unsigned short rc_WORD;
typedef unsigned long  rc_DWORD;

/* DDK 的 RGBQUAD: 蓝、绿、红、保留 */
typedef struct {
    rc_BYTE rgbBlue;
    rc_BYTE rgbGreen;
    rc_BYTE rgbRed;
    rc_BYTE rgbReserved;
} rc_RGBQUAD;

/* Windows 3.1 DDK 的 CONFIG_BIN(显示驱动 CONFIG 资源) */
typedef struct {
    short       VertThumHeight;     /* 垂直滚动条滑块高(像素) */
    short       HorizThumWidth;     /* 水平滚动条滑块宽(像素) */
    short       IconXRatio;         /* 图标宽缩放比           */
    short       IconYRatio;         /* 图标高缩放比           */
    short       CurXRatio;          /* 光标宽缩放比           */
    short       CurYRatio;          /* 光标高缩放比           */
    short       Reserved;
    short       XBorder;            /* 垂直边框宽             */
    short       YBorder;            /* 水平边框宽             */

    /* 默认系统颜色 */
    rc_RGBQUAD  clrScrollbar;
    rc_RGBQUAD  clrDesktop;
    rc_RGBQUAD  clrActiveCaption;
    rc_RGBQUAD  clrInactiveCaption;
    rc_RGBQUAD  clrMenu;
    rc_RGBQUAD  clrWindow;
    rc_RGBQUAD  clrWindowFrame;
    rc_RGBQUAD  clrMenuText;
    rc_RGBQUAD  clrWindowText;
    rc_RGBQUAD  clrCaptionText;
    rc_RGBQUAD  clrActiveBorder;
    rc_RGBQUAD  clrInactiveBorder;
    rc_RGBQUAD  clrAppWorkspace;
    rc_RGBQUAD  clrHiliteBk;
    rc_RGBQUAD  clrHiliteText;
    rc_RGBQUAD  clrBtnFace;
    rc_RGBQUAD  clrBtnShadow;
    rc_RGBQUAD  clrGrayText;
    rc_RGBQUAD  clrBtnText;
    rc_RGBQUAD  clrInactiveCaptionText;
} rc_CONFIG_BIN;                    /* 18 + 20*4 = 98 字节 */

rc_CONFIG_BIN Config = {
    17,
    17,
    2,
    2,
    1,
    1,
    0,
    1,
    1,

    192, 192, 192, 0,
    192, 192, 192, 0,
    000, 000, 128, 0,
    255, 255, 255, 0,
    255, 255, 255, 0,
    255, 255, 255, 0,
    000, 000, 000, 0,
    000, 000, 000, 0,
    000, 000, 000, 0,
    255, 255, 255, 0,
    192, 192, 192, 0,
    192, 192, 192, 0,
    255, 255, 255, 0,
    000, 000, 128, 0,
    255, 255, 255, 0,
    192, 192, 192, 0,
    128, 128, 128, 0,
    192, 192, 192, 0,
    000, 000, 000, 0,
    000, 000, 000, 0
};
