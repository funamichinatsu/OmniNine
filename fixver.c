/*
 * fixver.c — 把 NE 可执行头里的"期望 Windows 版本"字段改写为 4.0
 *
 * 【为什么需要这个工具】
 * Open Watcom 的 wlink 生成 16 位 NE 模块时，把 NE 头偏移 0x3E 处的
 * "期望 Windows 版本"写成 3.0（wlink 没有提供修改该字段的选项）。
 * 而 Windows 95/98/Me 的 GDI 在加载显示驱动程序时检查这个字段：
 * 标着 3.x 的驱动被视为"为旧版 Windows 编写、DDI 契约不同"，在调用
 * 驱动入口点之前就直接拒载 —— bootlog 表现为：
 *     LoadStart = omninine.drv
 *     LoadStart = DIBENG.DLL
 *     LoadSuccess = DIBENG.DLL
 *     LoadFail = omninine.drv Failure code is 0000
 * 且驱动代码一行都不会执行（连入口处的 int 21h 探针都不留痕迹）。
 * 同一字段在 Win98SE 自带的 vga.drv 里是 4.0；vmdisp9x 项目也专门带
 * 一个 drvfix.c 干同一件事（其 makefile 里 "fixlink.exe -40 $@），
 * 证明这是 wlink 路线构建 Win9x 显示驱动的必经一步。
 *
 * MSVC 路线不需要本工具：Win9x DDK 的 LINK 5.x 会按 DDK 头文件的
 * 要求自动把该字段写成 4.0。
 *
 * 用法: fixver <file.drv>      返回 0 = 成功(或本来就是 4.0)
 */
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f;
    long neoff;
    unsigned char b[2];

    if (argc != 2) {
        fprintf(stderr, "usage: fixver <file.drv>\n");
        return 1;
    }
    f = fopen(argv[1], "r+b");
    if (!f) {
        perror("fixver: open");
        return 2;
    }
    /* MZ 头 0x3C 处是 NE 头的文件偏移 */
    if (fseek(f, 0x3C, SEEK_SET) || fread(b, 1, 2, f) != 2) {
        fprintf(stderr, "fixver: 不是有效的 MZ/NE 文件\n");
        fclose(f);
        return 3;
    }
    neoff = (long)b[0] | ((long)b[1] << 8);
    /* 校验 NE 签名 */
    if (fseek(f, neoff, SEEK_SET) || fread(b, 1, 2, f) != 2
        || b[0] != 'N' || b[1] != 'E') {
        fprintf(stderr, "fixver: 找不到 NE 头\n");
        fclose(f);
        return 4;
    }
    /* NE+0x3E = 版本次号, NE+0x3F = 版本主号; 写 0x00,0x04 即 4.0 */
    b[0] = 0x00;
    b[1] = 0x04;
    if (fseek(f, neoff + 0x3E, SEEK_SET) || fwrite(b, 1, 2, f) != 2) {
        fprintf(stderr, "fixver: 写入失败\n");
        fclose(f);
        return 5;
    }
    fclose(f);
    return 0;
}
