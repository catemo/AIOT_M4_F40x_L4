/**
 ******************************************************************************
 * @file    app_cmd.c
 * @brief   应用层（app）—— 串口1 指令控 LED 实现
 *
 *  业务逻辑（就三步）：
 *    ① 收：从 MOD_UART1_Read 读字节，攒成一行
 *    ② 译：把一行命令翻译成"灯做什么"
 *    ③ 行：调 APP_BLINK_* 干活，并回一句 OK / ERR
 *
 *  ★ 依赖方向（四层纪律）：
 *    串口方向：本文件 → MOD_UART1_*（platform）→ net_uart_*（network）→ HAL
 *    点灯方向：本文件 → APP_BLINK_*（同层的闪烁状态机）→ PER_LED_*（perception）
 *    本文件绝不直接 include mod_uart1 以下任何一层，也绝不直接碰 GPIO
 *
 *  判帧策略（新手友好版）：
 *    网络层的 IDLE 中断已经把"一帧"切好了，本任务只管攒行：
 *    - 收到 '\n'            → 立即结算这一行
 *    - 缓冲里没新数据了     → 上一帧没换行符也结算（兼容不发回车的串口助手）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_cmd.h，
 *  本文件的注释侧重"一行命令是怎么变成灯的亮灭的"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_UART1_ENABLE

#include "app_cmd.h"
#include "app_blink.h"
#include "mod_uart1.h"
#include "cmsis_os.h"
#include <string.h>
#if APP_PARAM_ENABLE
#include "app_param.h"   /* TH / PDUMP / PINIT：阈值与配置块的诊断取证 */
#endif
#if MOD_EEPROM_ENABLE
#include "mod_eeprom.h"  /* ISCAN：扫 I2C 总线上到底挂了谁（诊断用） */
#endif
#if MOD_FLASH_ENABLE
#include "mod_flash.h"   /* FID / FDUMP：SPI Flash 的原始字节取证 */
#endif
#if APP_LOGGER_ENABLE
#include "app_logger.h"  /* LOG：日志回读（index 0 = 最新） */
#endif

/* ===================== 命令表（帮助文本） =====================
 * ★ 输出全部用英文：源文件是 UTF-8，中文发出去遇到 GBK 编码的串口助手会
 *   显示乱码；英文在任何编码下都稳定显示 */
static const char *const s_help =
    "\r\n=== UART1 LED Control (4-Layer) ===\r\n"
    "Commands (end with Enter, case-insensitive):\r\n"
    "  LED1 ON / LED1 OFF   LED1(PG14) on/off\r\n"
    "  BLINK                blink 1Hz (half period from cfg)\r\n"
    "  STATUS               query real LED state\r\n"
    "  TH <t> <h>           set temp/humi thresholds, save to EEPROM\r\n"
    "  ISCAN                scan I2C addr 0x50..0x57 (who is on the bus)\r\n"
    "  PINFO                re-detect EEPROM: model / page / size\r\n"
    "  PDUMP [len]          dump EEPROM raw bytes from 0x0000\r\n"
    "  PARAM                parse the 20B config block (magic/ver/crc)\r\n"
    "  FID                  read SPI Flash JEDEC ID + model name\r\n"
    "  FDUMP [addr] [len]   dump Flash raw bytes (addr supports 0x)\r\n"
    "  LOG [n]              read log record n (0 = newest)\r\n"
    "  HELP                 show this help\r\n"
    "> ";

/* ========== 内部工具 ========== */

/*----------------------------------------------------------------------------
 * 原地转大写，使命令"大小写不限"（led1 on == LED1 ON）
 *----------------------------------------------------------------------------*/
static void to_upper(char *s)
{
    while (*s != '\0')
    {
        if ((*s >= 'a') && (*s <= 'z'))
        {
            *s = (char)(*s - 'a' + 'A');
        }
        s++;
    }
}

/*----------------------------------------------------------------------------
 * TH 命令："TH 31 60" → 新阈值写进 EEPROM，断电重启后仍生效（2.3 新增）。
 * ★ 保存走 APP_PARAM_SetThresholds（更新 RAM + 立即写 + 读回确认），
 *   本函数只管解析与应答，不直接碰 mod_eeprom —— 层内各模块各管一段。
 *----------------------------------------------------------------------------*/
#if APP_PARAM_ENABLE
/* 小工具：把 0~99 打成两位十进制（TH 应答专用，不引 printf） */
static void print_u8(uint8_t v)
{
    char b[3];

    b[0] = (char)('0' + (v / 10U));
    b[1] = (char)('0' + (v % 10U));
    b[2] = '\0';
    MOD_UART1_SendString(b);
}

static void cmd_th(const char *args)
{
    uint32_t vals[2] = { 0U, 0U };
    uint8_t  got[2]  = { 0U, 0U };
    uint8_t  idx     = 0U;
    uint8_t  t;
    uint8_t  h;
    int      rc;

    /* 手工解析两个 0~99 的整数（不引 sscanf，与全工程"不拉格式化库"一致） */
    for (; *args != '\0'; args++)
    {
        if ((*args >= '0') && (*args <= '9'))
        {
            vals[idx] = (vals[idx] * 10U) + (uint32_t)(*args - '0');
            if (vals[idx] > 99U)
            {
                MOD_UART1_SendString("ERR: TH value range 0..99\r\n");
                return;
            }
            got[idx] = 1U;
        }
        else if (*args == ' ')
        {
            if ((got[idx] != 0U) && (idx == 0U))
            {
                idx++;      /* 第一个数已结算，开始收第二个；多余空格忽略 */
            }
        }
        else
        {
            MOD_UART1_SendString("ERR: TH usage: TH <temp> <humi>\r\n");
            return;
        }
    }

    if ((got[0] == 0U) || (got[1] == 0U))
    {
        MOD_UART1_SendString("ERR: TH usage: TH <temp> <humi>\r\n");
        return;
    }

    t = (uint8_t)vals[0];
    h = (uint8_t)vals[1];
    rc = APP_PARAM_SetThresholds(t, h);

    if (rc == APP_PARAM_OK)
    {
        MOD_UART1_SendString("OK: TH saved to EEPROM: Temp>=");
        print_u8(t);
        MOD_UART1_SendString("C Hum>=");
        print_u8(h);
        MOD_UART1_SendString("%\r\n");
    }
    else if (rc == APP_PARAM_ERR_PARAM)
    {
        MOD_UART1_SendString("ERR: range (temp 0..50, humi 20..90)\r\n");
    }
    else
    {
        MOD_UART1_SendString("ERR: EEPROM write failed (check AT24C02)\r\n");
    }
}
#endif /* APP_PARAM_ENABLE */

/* ===================== 存储链诊断命令（2.3 真机取证） =====================
 * 由来：2.3 首跑日志里反复出现 "[PARAM] readback INVALID"，但仅凭这条无法判断
 *       到底是"写没写进去"还是"读错了地方"——这类问题不能猜，要把字节打出来。
 *
 * 四条取证链，各答一个问题：
 *   ISCAN  → 总线上到底挂了谁（器件在不在 / 地址是几）
 *   PINFO  → 挂的是哪一颗（字地址几位 / 页多宽 / 容量多大）
 *   PDUMP  → 芯片里现在存的到底是什么字节（有没有写进去的终极证据）
 *   PARAM  → 这些字节按配置块解释成什么（magic / version / crc 逐项过堂）
 *   FID    → SPI Flash 验明正身（JEDEC 与型号名）
 *   FDUMP  → Flash 里现在存的字节（头扇区 'LOG1' + 日志区前几条）
 *   LOG    → 日志回读（index 0 = 最新）
 *
 * ★ 依赖方向：本文件 include mod_eeprom / mod_flash（platform），与 app_param
 *   一样是"应用层调平台层"，没有穿透到 network 层；扫总线用的
 *   mod_eeprom_probe_dev 就是为此专门在平台层开的口子（见 mod_eeprom.h）。
 * ★ 副作用：除 PINIT 会往牺牲地址写 1 字节探测字地址位宽外，其余全部只读。
 *----------------------------------------------------------------------------*/
#if MOD_EEPROM_ENABLE || MOD_FLASH_ENABLE

/*----------------------------------------------------------------------------
 * 打印两位十六进制（不引 printf，与全工程"不拉格式化库"的纪律一致）
 *----------------------------------------------------------------------------*/
static void print_hex2(uint8_t v)
{
    static const char H[] = "0123456789ABCDEF";
    char b[3];

    b[0] = H[(v >> 4) & 0x0FU];
    b[1] = H[v & 0x0FU];
    b[2] = '\0';
    MOD_UART1_SendString(b);
}

/*----------------------------------------------------------------------------
 * 打印一个无符号十进制数
 *----------------------------------------------------------------------------*/
static void print_dec(uint32_t v)
{
    char b[12];
    uint8_t i = 0U;

    if (v == 0U)
    {
        MOD_UART1_SendString("0");
        return;
    }
    while (v > 0U)
    {
        b[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0U)
    {
        char one[2];
        one[0] = b[--i];
        one[1] = '\0';
        MOD_UART1_SendString(one);
    }
}

/*----------------------------------------------------------------------------
 * 解析一个无符号整数（支持 0x 十六进制）
 * 返回值：数字结束后的位置；没有数字返回 NULL（调用方据此判断"有没有带参数"）
 *----------------------------------------------------------------------------*/
static const char *scan_uint(const char *s, uint32_t *out)
{
    uint32_t v    = 0U;
    uint32_t base = 10U;
    uint8_t  got  = 0U;

    while ((*s == ' ') || (*s == '\t'))
    {
        s++;
    }
    if ((s[0] == '0') && ((s[1] == 'X') || (s[1] == 'x')))
    {
        base = 16U;
        s   += 2;
    }
    for (; *s != '\0'; s++)
    {
        uint32_t d;

        if ((*s >= '0') && (*s <= '9'))
        {
            d = (uint32_t)(*s - '0');
        }
        else if ((base == 16U) && (*s >= 'A') && (*s <= 'F'))
        {
            d = (uint32_t)(*s - 'A' + 10U);
        }
        else
        {
            break;
        }
        v   = (v * base) + d;
        got = 1U;
    }
    if (got == 0U)
    {
        return NULL;
    }
    *out = v;
    return s;
}

/*----------------------------------------------------------------------------
 * 打印一行内存：偏移 + 最多 16 字节十六进制
 *----------------------------------------------------------------------------*/
static void print_row(uint32_t off, const uint8_t *b, uint16_t n)
{
    uint16_t i;

    print_hex2((uint8_t)((off >> 8) & 0xFFU));
    print_hex2((uint8_t)(off & 0xFFU));
    MOD_UART1_SendString(": ");
    for (i = 0U; i < n; i++)
    {
        print_hex2(b[i]);
        MOD_UART1_SendString(" ");
    }
    MOD_UART1_SendString("\r\n");
}
#endif /* MOD_EEPROM_ENABLE || MOD_FLASH_ENABLE */

#if MOD_EEPROM_ENABLE
/*----------------------------------------------------------------------------
 * ISCAN：扫 EEPROM 地址族（7 位 0x50~0x57 = 8 位写地址 0xA0~0xAE）
 * ★ 只发地址帧、不读不写，对器件无副作用，可以放心反复跑
 * ★ 只扫这一族而不是全表：扫一次约 0.4 s，全表 0x08~0x77 要 5 s 以上，
 *   而这个工程要回答的只是"EEPROM 在不在、地址是几"
 *----------------------------------------------------------------------------*/
static void cmd_iscan(void)
{
    uint8_t a7;
    uint8_t found = 0U;

    MOD_UART1_SendString("ISCAN I2C1 0x50..0x57 (7bit):\r\n");
    for (a7 = 0x50U; a7 <= 0x57U; a7++)
    {
        if (mod_eeprom_probe_dev((uint8_t)(a7 << 1U)) == MOD_EEPROM_OK)
        {
            MOD_UART1_SendString("  ACK 7bit=0x");
            print_hex2(a7);
            MOD_UART1_SendString(" write=0x");
            print_hex2((uint8_t)(a7 << 1U));
            MOD_UART1_SendString("\r\n");
            found++;
        }
    }
    if (found == 0U)
    {
        MOD_UART1_SendString("  none -> check PB8(SCL)/PB9(SDA) + pull-up + WP\r\n");
    }
    else
    {
        MOD_UART1_SendString("  hint: 0xA0 = A2/A1/A0 all tied to GND\r\n");
    }
}

/*----------------------------------------------------------------------------
 * PINFO：重新探测器件，打印型号 / 页宽 / 容量
 * ★ 这一条是"字地址位宽"类故障的照妖镜：AT24C02 与 AT24C256 器件地址同为
 *   0xA0，probe 分辨不出，必须写读比对字地址位宽（detect_chip）
 *----------------------------------------------------------------------------*/
static void cmd_pinfo(void)
{
    MOD_UART1_SendString("PINFO EEPROM: ");
    if (mod_eeprom_init() != MOD_EEPROM_OK)
    {
        MOD_UART1_SendString("no device (probe NACK) -> check wiring\r\n");
        return;
    }
    MOD_UART1_SendString(mod_eeprom_chip_name());
    MOD_UART1_SendString(" page=");
    print_dec(mod_eeprom_page_size());
    MOD_UART1_SendString("B size=");
    print_dec(mod_eeprom_total_size());
    MOD_UART1_SendString("B\r\n");
}

/*----------------------------------------------------------------------------
 * PDUMP [len]：从 0x0000 起把 EEPROM 的原始字节打出来（默认 32，上限 256）
 * ★ "有没有真存进去"的终极证据：
 *     - 全 FF            → 芯片是空的，写根本没落到这颗器件上
 *     - 前 20 字节有内容 → 写进去了；再敲 PARAM 看它解不解得成配置块
 *     - 内容出现在别的地址 → 字地址位宽不对（回头看 PINFO 认的是哪颗）
 *----------------------------------------------------------------------------*/
static void cmd_pdump(const char *args)
{
    uint32_t len = 32U;
    uint32_t off = 0U;
    uint8_t  buf[16];

    (void)scan_uint(args, &len);
    if (len == 0U)
    {
        len = 32U;
    }
    if (len > 256U)
    {
        len = 256U;
    }

    MOD_UART1_SendString("PDUMP EEPROM @0x0000 len=");
    print_dec(len);
    MOD_UART1_SendString(":\r\n");

    while (off < len)
    {
        uint16_t n = (((len - off) > 16U)) ? 16U : (uint16_t)(len - off);

        if (mod_eeprom_read((uint16_t)off, buf, n) != MOD_EEPROM_OK)
        {
            MOD_UART1_SendString("  read FAIL at 0x");
            print_hex2((uint8_t)(off >> 8));
            print_hex2((uint8_t)(off & 0xFFU));
            MOD_UART1_SendString(" (-2=IO/NACK, -3=TIMEOUT)\r\n");
            return;
        }
        print_row(off, buf, n);
        off += n;
    }
}
#endif /* MOD_EEPROM_ENABLE */

#if APP_PARAM_ENABLE
/*----------------------------------------------------------------------------
 * PARAM：把 EEPROM 前 32 字节按配置块解释，逐项过堂（解析在 app_param 内）
 *----------------------------------------------------------------------------*/
static void cmd_param(void)
{
    (void)APP_PARAM_Inspect();
}
#endif /* APP_PARAM_ENABLE */

#if MOD_FLASH_ENABLE
/*----------------------------------------------------------------------------
 * FID：读 JEDEC ID 并翻译型号名
 * ★ 只要这三个字节读得出来，就证明 SPI 的片选 / 模式 / 时钟全对
 *   （时序不对时读回来是 00 00 00 或 FF FF FF，而不是正确的 ID）
 *----------------------------------------------------------------------------*/
static void cmd_fid(void)
{
    uint8_t id[3];

    if (mod_flash_read_jedec(id) != MOD_FLASH_OK)
    {
        MOD_UART1_SendString("FID: JEDEC read FAILED (check PB3/4/5 + PG10 CS + mode 0)\r\n");
        return;
    }
    MOD_UART1_SendString("FID JEDEC = ");
    print_hex2(id[0]);
    MOD_UART1_SendString(" ");
    print_hex2(id[1]);
    MOD_UART1_SendString(" ");
    print_hex2(id[2]);
    MOD_UART1_SendString(" model = ");
    MOD_UART1_SendString(mod_flash_model_name());
    MOD_UART1_SendString("\r\n");
}

/*----------------------------------------------------------------------------
 * FDUMP [addr] [len]：把 Flash 的原始字节打出来（默认 0x000000, 32 字节）
 * ★ 典型用法（都在默认参数范围内，直接敲 FDUMP 就能看到）：
 *     FDUMP                → 头扇区开头：应看到 'LOG1' = 4C 4F 47 31
 *     FDUMP 0x1000 32      → 日志区开头：每 8 字节一条记录
 *     FDUMP 0x1000 24      → 前三条：tick_s(4B) / temp_x10(2B) / humi_x10(2B)
 *----------------------------------------------------------------------------*/
static void cmd_fdump(const char *args)
{
    uint32_t addr = 0U;
    uint32_t len  = 32U;
    uint32_t off  = 0U;
    uint8_t  buf[16];
    const char *p;

    p = scan_uint(args, &addr);
    if (p != NULL)
    {
        (void)scan_uint(p, &len);
    }
    if (len == 0U)
    {
        len = 32U;
    }
    if (len > 256U)
    {
        len = 256U;
    }

    MOD_UART1_SendString("FDUMP FLASH @");
    print_hex2((uint8_t)((addr >> 16) & 0xFFU));
    print_hex2((uint8_t)((addr >> 8) & 0xFFU));
    print_hex2((uint8_t)(addr & 0xFFU));
    MOD_UART1_SendString(" len=");
    print_dec(len);
    MOD_UART1_SendString(":\r\n");

    while (off < len)
    {
        uint16_t      n  = (((len - off) > 16U)) ? 16U : (uint16_t)(len - off);
        uint32_t      at = addr + off;

        if (mod_flash_read(at, buf, n) != MOD_FLASH_OK)
        {
            MOD_UART1_SendString("  read FAIL at ");
            print_hex2((uint8_t)((at >> 16) & 0xFFU));
            print_hex2((uint8_t)((at >> 8) & 0xFFU));
            print_hex2((uint8_t)(at & 0xFFU));
            MOD_UART1_SendString("\r\n");
            return;
        }
        print_row(off, buf, n);
        off += n;
    }
}
#endif /* MOD_FLASH_ENABLE */

#if APP_LOGGER_ENABLE
/*----------------------------------------------------------------------------
 * LOG [n]：按序号回读日志（0 = 最新一条），并打出当前有效记录数
 * ★ 与 FDUMP 互补：FDUMP 看字节，LOG 看"解析出来的记录"——
 *   两者对得上，才说明 8 字节记录的 pack(1) 布局与真实存储一致
 *----------------------------------------------------------------------------*/
static void cmd_log(const char *args)
{
    uint32_t     idx = 0U;
    app_log_rec_t rec;
    int16_t      t;

    (void)scan_uint(args, &idx);

    if (APP_LOGGER_Read(idx, &rec) != APP_LOGGER_OK)
    {
        MOD_UART1_SendString("LOG: read FAIL (index out of range or flash IO)\r\n");
        return;
    }

    t = rec.temp_x10;
    MOD_UART1_SendString("LOG #");
    print_dec(idx);
    MOD_UART1_SendString(" seq=");
    print_dec(rec.seq);
    MOD_UART1_SendString(" tick=");
    print_dec(rec.tick_s);
    MOD_UART1_SendString("s T=");
    if (t < 0)
    {
        MOD_UART1_SendString("-");
        t = (int16_t)(-t);
    }
    print_dec((uint32_t)(t / 10));
    MOD_UART1_SendString(".");
    print_dec((uint32_t)(t % 10));
    MOD_UART1_SendString("C H=");
    print_dec((uint32_t)(rec.humi_x10 / 10U));
    MOD_UART1_SendString(".");
    print_dec((uint32_t)(rec.humi_x10 % 10U));
    MOD_UART1_SendString("%  (count=");
    print_dec(APP_LOGGER_Count());
    MOD_UART1_SendString(")\r\n");
}
#endif /* APP_LOGGER_ENABLE */

/*----------------------------------------------------------------------------
 * 执行一条已确认的命令并应答。
 * ★ 这里的每个动作都经过 APP_BLINK_*：不是直接点灯，而是"先让灯退出闪烁，
 *   再点灯"——否则闪烁状态机会跟串口命令抢同一颗灯。
 *----------------------------------------------------------------------------*/
static void cmd_exec(const char *cmd)
{
    if (strcmp(cmd, "LED1 ON") == 0)
    {
        (void)APP_BLINK_On(APP_BLINK_LED1);
        MOD_UART1_SendString("OK: LED1 ON\r\n");
    }
    else if (strcmp(cmd, "LED1 OFF") == 0)
    {
        (void)APP_BLINK_Off(APP_BLINK_LED1);
        MOD_UART1_SendString("OK: LED1 OFF\r\n");
    }
    else if (strcmp(cmd, "BLINK") == 0)
    {
        /* 半周期来自 cfg 横切层：改 APP_LED1_BLINK_MS 即改闪烁快慢 */
        (void)APP_BLINK_blink_set(APP_BLINK_LED1, APP_LED1_BLINK_MS);
        MOD_UART1_SendString("OK: BLINK\r\n");
    }
    else if (strcmp(cmd, "STATUS") == 0)
    {
        /* 读真实引脚电平（不是软件记忆值），排障时以它为准 */
        MOD_UART1_SendString("LED1=");
        MOD_UART1_SendString((APP_BLINK_IsOn(APP_BLINK_LED1) != 0U) ? "ON" : "OFF");
        MOD_UART1_SendString("\r\n");
    }
    else if (strcmp(cmd, "HELP") == 0)
    {
        MOD_UART1_SendString(s_help);
    }
#if APP_PARAM_ENABLE
    else if ((cmd[0] == 'T') && (cmd[1] == 'H') &&
             ((cmd[2] == ' ') || (cmd[2] == '\0')))
    {
        cmd_th(&cmd[2]);    /* "TH 31 60"：新阈值立即生效并写 EEPROM */
    }
#endif
#if MOD_EEPROM_ENABLE
    else if (strcmp(cmd, "ISCAN") == 0)
    {
        cmd_iscan();        /* 扫 I2C 总线上挂了谁（0x50~0x57 地址族） */
    }
    else if (strcmp(cmd, "PINFO") == 0)
    {
        cmd_pinfo();        /* 重新探测器件：型号 / 页宽 / 容量 */
    }
    else if ((strncmp(cmd, "PDUMP", 5U) == 0) &&
             ((cmd[5] == ' ') || (cmd[5] == '\0')))
    {
        cmd_pdump(&cmd[5]); /* "PDUMP 64"：EEPROM 原始字节取证 */
    }
#endif
#if APP_PARAM_ENABLE
    else if (strcmp(cmd, "PARAM") == 0)
    {
        cmd_param();        /* 配置块逐项过堂（magic/version/crc） */
    }
#endif
#if MOD_FLASH_ENABLE
    else if (strcmp(cmd, "FID") == 0)
    {
        cmd_fid();          /* SPI Flash 验明正身：JEDEC + 型号名 */
    }
    else if ((strncmp(cmd, "FDUMP", 5U) == 0) &&
             ((cmd[5] == ' ') || (cmd[5] == '\0')))
    {
        cmd_fdump(&cmd[5]); /* "FDUMP 0x1000 32"：Flash 原始字节取证 */
    }
#endif
#if APP_LOGGER_ENABLE
    else if ((strncmp(cmd, "LOG", 3U) == 0) &&
             ((cmd[3] == ' ') || (cmd[3] == '\0')))
    {
        cmd_log(&cmd[3]);   /* "LOG 0"：回读最新一条日志 */
    }
#endif
    else
    {
        MOD_UART1_SendString("ERR: unknown cmd [");
        MOD_UART1_SendString(cmd);
        MOD_UART1_SendString("]\r\n");
    }
    MOD_UART1_SendString("> ");   /* 命令提示符：一眼看出程序活着 */
}

/*----------------------------------------------------------------------------
 * 结算一行：去回车 → 转大写 → 忽略空行 → 执行
 *----------------------------------------------------------------------------*/
static void cmd_process(char *line)
{
    uint32_t len = strlen(line);

    while ((len > 0U) && ((line[len - 1U] == '\r') || (line[len - 1U] == '\n')))
    {
        line[--len] = '\0';   /* 去掉行尾的 \r / \n */
    }
    if (len == 0U)
    {
        return;               /* 空行忽略，不回 ERR 刷屏 */
    }
    to_upper(line);
    cmd_exec(line);
}

/*----------------------------------------------------------------------------
 * 字符回显：把收到的可打印字符原样发回 PC。
 * 为什么要有它（排障价值大于功能价值）：
 *   ① 用户"敲什么看到什么"——回显能出来，就意味着 MCU 先【收到了】再【吐回来了】，
 *      一进一出两条链路同时得到验证
 *   ② 命令不带回车的场景下也能看清自己发了什么
 * ★ 排障时的三段判据（要配合上电自动发出的帮助文本一起看，单看回显会误判）：
 *      有助文本 + 有回显  →  RX / TX 都通，放心往下测
 *      有助文本 + 无回显  →  TX 通、RX 没通（重点查模块 TX → PA10 这根线）
 *      连助文本都没有      →  TX 没通，或固件根本没跑起来（先看灯有没有快闪 3 下）
 * 只回显可打印 ASCII；'\r' 转成 "\r\n" 让显示换行；其余控制字符丢弃防刷屏。
 *----------------------------------------------------------------------------*/
static void uart_echo(uint8_t ch)
{
    if ((ch >= 0x20U) && (ch <= 0x7EU))
    {
        MOD_UART1_Send(&ch, 1U);                          /* 可打印字符：原样吐回 */
    }
    else if (ch == '\r')
    {
        MOD_UART1_Send((const uint8_t *)"\r\n", 2U);      /* 回车：回一个换行，看着整齐 */
    }
    else
    {
        /* 其它控制字符不回显，防止刷屏 */
    }
}

#if APP_LED_ENABLE
/*----------------------------------------------------------------------------
 * 启动自检：串口任务起来后让 LED1 快闪 3 次。
 * ★ 这是本页最有用的"排障信号"：串口线没接好、串口助手没打开时，
 *   光看串口什么都判断不出来；但灯闪 3 下就说明——
 *   ① 固件是含串口的新版（旧版固件没有这段代码）
 *   ② app_cmd_task 已被 osThreadNew 成功创建并跑起来了
 *   ③ MOD_UART1_Init() 已经执行（DMA 接收链路已拉起）
 * 闪完把控制权交还给闪烁状态机（1Hz），不影响正常业务。
 * 若上电后灯只是常规 1Hz 慢闪、没有这 3 下快闪 → 固件没烧进去或任务没建起来。
 *----------------------------------------------------------------------------*/
static void uart_self_check_blink(void)
{
    uint32_t i;

    for (i = 0U; i < 3U; i++)
    {
        (void)APP_BLINK_On(APP_BLINK_LED1);
        osDelay(80U);
        (void)APP_BLINK_Off(APP_BLINK_LED1);
        osDelay(170U);
    }
    (void)APP_BLINK_blink_set(APP_BLINK_LED1, APP_LED1_BLINK_MS);   /* 交还闪烁状态机 */
}
#endif /* APP_LED_ENABLE */

/*----------------------------------------------------------------------------
 * 任务体：FreeRTOS 调度的主循环   【接口契约见 app_cmd.h】
 *----------------------------------------------------------------------------*/
void app_cmd_task(void *argument)
{
    char     line[APP_UART1_LINE_MAX];  /* 行缓冲：攒一整条命令 */
    uint32_t pos     = 0U;              /* 行缓冲当前写位置 */
    uint8_t  pending = 0U;              /* 行缓冲里还有没有没结算的残留 */
    uint8_t  tmp[16];                   /* 每次搬运小块，避免一次搬太多占栈 */
    uint32_t n;
    uint32_t i;

    (void)argument;

    /* 初始化链路：APP 只调 platform 的名字（串口是基础设施） */
    MOD_UART1_Init();

#if APP_LED_ENABLE
    uart_self_check_blink();    /* 自检信号：LED1 快闪 3 次 = 串口任务已启动 */
#endif

    osDelay(20U);               /* 等发送端稳一稳，再打招呼 */
    MOD_UART1_SendString(s_help);

    for (;;)
    {
        n = MOD_UART1_Read(tmp, sizeof(tmp));

        if (n > 0U)
        {
            pending = 1U;   /* 有新数据进缓冲 */
            for (i = 0U; i < n; i++)
            {
                if (tmp[i] == '\n')
                {
                    line[pos] = '\0';       /* 见到换行：整行结算 */
                    pos       = 0U;
                    cmd_process(line);
                    pending   = 0U;
                }
                else if (pos < (APP_UART1_LINE_MAX - 1U))
                {
                    line[pos++] = (char)tmp[i];   /* 攒字符（超长的丢弃，防溢出） */
                    uart_echo(tmp[i]);            /* 回显：敲什么看到什么 */
                }
            }
        }
        else if (pending != 0U)
        {
            /* 没新数据 + 缓冲里有残留：串口已安静，即使没换行也结算
             *（兼容不发回车、点一次发一次的串口助手） */
            line[pos] = '\0';
            pos       = 0U;
            cmd_process(line);
            pending   = 0U;
        }
        else
        {
            osDelay(1U);    /* 完全空闲：让出 CPU，1ms 后再来看一眼 */
        }
    }
}

#endif /* APP_UART1_ENABLE */
