/**
 ******************************************************************************
 * @file    app_watchdog.c
 * @brief   应用层（app）—— 看门狗喂狗任务实现
 *
 *  任务主循环（每 APP_WDT_FEED_PERIOD_MS = 500 ms 一拍）：
 *    先看有没有 HANG 演示请求 → 有就打印一行、进死循环（不再喂狗）
 *    没有就喂狗 → 每喂 APP_WDT_ALIVE_EVERY 次打一行 Alive → 睡 500 ms
 *
 *  ★ 依赖方向（四层纪律）：
 *    喂狗方向：本文件 → PER_IWDG_*（perception）→ HAL_IWDG_* → IWDG 硬件
 *    打印方向：本文件 → MOD_UART1_*（platform）—— 借串口链出口，
 *              与 app_dht11 / 存储链同一条跨链依赖（cfg_check.h 闸门看着）
 *    本文件绝不直接 include stm32f4xx_hal.h，也绝不直接碰 RCC / IWDG 寄存器
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_watchdog.h，
 *  本文件的注释侧重"为什么这么排顺序"。
 ******************************************************************************
 */
#include "module_cfg.h"

#if APP_WATCHDOG_ENABLE

#include "app_watchdog.h"
#include "per_iwdg.h"
#include "mod_uart1.h"
#include "cmsis_os.h"

/* HANG 演示请求标志：app_cmd 置 1，本任务见到就"罢工"。
 * volatile 不能少——写的一方在串口任务上下文，读的一方在本任务。 */
static volatile uint8_t s_hang_request = 0U;

void APP_WATCHDOG_HangDemo(void)
{
    s_hang_request = 1U;
}

/*----------------------------------------------------------------------------
 * 小工具：打印一个无符号十进制数（不引 printf，与全工程纪律一致）
 *----------------------------------------------------------------------------*/
static void wdt_print_dec(uint32_t v)
{
    char b[10];
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
 * 任务体：复位原因 → 清标志 → 启动 IWDG → 500 ms 喂狗循环   【契约见 .h】
 *----------------------------------------------------------------------------*/
void app_watchdog_task(void *argument)
{
    uint32_t feed_count = 0U;
    uint8_t  by_iwdg;

    (void)argument;

    /* ① 先读复位原因、随即清标志：顺序不能反——
     *   清标志会连 IWDGRSTF 一起抹掉，先清就永远读不到"被狗咬醒" */
    by_iwdg = PER_IWDG_LastResetByIwdg();
    PER_IWDG_ClearResetFlags();

    /* ② 启动 IWDG（3 秒超时 + 调试冻结 + 启动即喂第一口）。
     *   从这一刻起，"死亡倒计时"开始，本任务必须按时喂狗。 */
    PER_IWDG_Init();

    /* ③ 等串口链路就绪再打印启动两行：
     *   串口任务（app_cmd）先被创建，它的 MOD_UART1_Init() 在头几毫秒就跑完；
     *   这里让出 100 ms，启动行才不会丢。100 ms ≪ 3 秒超时，不咬狗。 */
    osDelay(100U);

    MOD_UART1_SendString("[WDT] boot: last reset = ");
    MOD_UART1_SendString((by_iwdg != 0U) ? "IWDG watchdog" : "power-on/others");
    MOD_UART1_SendString("\r\n");

    MOD_UART1_SendString("[WDT] IWDG started, timeout=");
    wdt_print_dec(PER_IWDG_TIMEOUT_MS);
    MOD_UART1_SendString("ms, feed every ");
    wdt_print_dec(APP_WDT_FEED_PERIOD_MS);
    MOD_UART1_SendString("ms (type HANG to simulate hang)\r\n");

    for (;;)
    {
        /* ④ HANG 演示：收到请求 → 打最后一行 → 死循环罢工。
         * 死循环放在喂狗【之前】检查：一旦罢工就一口都不再喂，
         * 约 3 秒后 IWDG 计数倒到 0，整机复位。
         * 期间与本任务平级的其它任务仍在轮转（LED 照闪、采集照跑），
         * 演示的正是"一个任务卡死、看门狗把整机救回来"。 */
        if (s_hang_request != 0U)
        {
            MOD_UART1_SendString("[WDT] HANG cmd: stop feeding, reset in ~3s...\r\n");
            for (;;)
            {
                /* 卡死：什么都不做，等看门狗咬 */
            }
        }

        PER_IWDG_Feed();
        feed_count++;

        if ((feed_count % APP_WDT_ALIVE_EVERY) == 0U)
        {
            MOD_UART1_SendString("[WDT] Alive, feed=");
            wdt_print_dec(feed_count);
            MOD_UART1_SendString("\r\n");
        }

        osDelay(APP_WDT_FEED_PERIOD_MS);
    }
}

#endif /* APP_WATCHDOG_ENABLE */
