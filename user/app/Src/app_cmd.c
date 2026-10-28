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

/* ===================== 命令表（帮助文本） =====================
 * ★ 输出全部用英文：源文件是 UTF-8，中文发出去遇到 GBK 编码的串口助手会
 *   显示乱码；英文在任何编码下都稳定显示 */
static const char *const s_help =
    "\r\n=== UART1 LED Control (4-Layer) ===\r\n"
    "Commands (end with Enter, case-insensitive):\r\n"
    "  LED1 ON / LED1 OFF   LED1(PG14) on/off\r\n"
    "  BLINK                blink 1Hz (half period from cfg)\r\n"
    "  STATUS               query real LED state\r\n"
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
