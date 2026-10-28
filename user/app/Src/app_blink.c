/**
 ******************************************************************************
 * @file    app_blink.c
 * @brief   应用层（app）—— LED 闪烁服务实现（闪烁状态机）
 *
 *  核心思路（就 3 行逻辑）：
 *    每调用一次 APP_BLINK_process()（间隔 1ms）：
 *      计时器 +1
 *      计时器 >= 半周期 → 计时器清零，翻转该灯
 *
 *  用"计毫秒数"而不是"读时间戳"，是为了让新手一眼看懂，
 *  也避免了溢出、时基换算这些坑。
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_blink.h，
 *  本文件的注释侧重"状态机是怎么一步步走的"。
 ******************************************************************************
 */
#include "module_cfg.h"
#include <stdint.h>
#if APP_BLINK_ENABLE
#include "app_blink.h"
#include "per_led.h"     /* 只允许调用感知层统一接口，不直接碰 per_gpio */

/* ========== 每颗 LED 的闪烁状态 ========== */
typedef struct
{
    uint16_t half_period_ms;  /* 闪烁半周期(ms)，0 = 不闪烁（该灯被跳过） */
    uint16_t counter_ms;      /* 计数器，每 1ms 递增一次，表示"已累计的毫秒数" */
} APP_BLINK_state_t;

/* 状态表：一颗灯一项，下标 = 状态机 LED 编号。
 * 因为带 static，上电时由 C 运行环境自动清零（half_period_ms = 0 即"还不闪"）。
 * 静态数组自动清零 = 安全的上电默认状态：没有任何灯会意外闪起来。 */
static APP_BLINK_state_t s_led_blink[APP_BLINK_LED_COUNT];

/* ========== 接口实现 ========== */

/*----------------------------------------------------------------------------
 * 函数功能：初始化 LED 硬件（实现：纯转发给感知层 PER_LED_Init）
 * 参数    ：无
 * 返回值  ：感知层返回码（本层不加工，原样上抛）
 * 调用处  ：app_led.c 的 app_led_init()   【接口契约见 app_blink.h】
 * 注意事项：本层对初始化不做任何加工——引脚怎么配是感知层的事，
 *           本文件只负责"把命令按层级往下传"
 *----------------------------------------------------------------------------*/
uint8_t APP_BLINK_Init(void)
{
    return PER_LED_Init();
}

/*----------------------------------------------------------------------------
 * 函数功能：给某颗灯设定闪烁半周期，并把它的计时器清零
 * 参数    ：led            —— 状态机 LED 编号，取值 APP_BLINK_LED1(0)
 *           half_period_ms —— 半周期(ms)，必须 > 0
 * 返回值  ：APP_BLINK_OK(0) = 成功；APP_BLINK_ERR_PARAM(1) = 编号越界或参数为 0
 * 调用处  ：app_led.c 的 app_led_init()，上电时为灯调用一次
 *           【接口契约见 app_blink.h】
 * 注意事项：① 先做参数校验再动状态，校验失败时不会留下一半的脏数据
 *           ② 编号越界拒收，防止写到数组外面去
 *           ③ 半周期传 0 也拒收：本实现用 0 表示"这颗灯不闪"，
 *              与"设成 0"语义冲突，所以参数一律要求 > 0；
 *              若要停闪，应另加 stop 接口，不要传 0
 *           ④ 写状态分两步（先记周期、再清计时器），顺序不能反：
 *              否则中间被打断时会短暂出现"周期是新值、计时器是旧值"的错配
 *           ⑤ 设置成功即把计数器清零，保证"改了周期立刻从新周期起算"，
 *              不残留旧计数
 *----------------------------------------------------------------------------*/
uint8_t APP_BLINK_blink_set(APP_BLINK_LED_t led, uint16_t half_period_ms)
{
    if ((led >= APP_BLINK_LED_COUNT) || (half_period_ms == 0))
    {
        return APP_BLINK_ERR_PARAM;  /* 越界或非法参数：直接返回，不破口 */
    }

    s_led_blink[led].half_period_ms = half_period_ms;
    s_led_blink[led].counter_ms     = 0;  /* 重置计数器，从这一刻重新计时 */

    return APP_BLINK_OK;
}

/*----------------------------------------------------------------------------
 * 函数功能：闪烁状态机 —— 逐颗灯累加毫秒，数够半周期就翻转它一次
 * 参数    ：无
 * 返回值  ：APP_BLINK_OK(0)，固定成功（无参数可校验，不会失败）
 * 调用处  ：app_led.c 的 app_led_task() 任务循环内，每 1ms 调用一次
 *           【必须严格保持 1ms 节拍，见 app_blink.h 的注意事项】
 * 注意事项：① 计数用 counter_ms++ 累加，1 次调用 = 1ms，
 *              因此"半周期"与"调用间隔"必须配套：把 osDelay(1) 改成
 *              osDelay(5)，灯的实际闪烁周期就会变成设定值的 5 倍
 *           ② 判断用 >= 而不是 ==，即使某次节拍被延迟、counter_ms 一次跨过
 *              半周期，也能正确翻转并重新计数，不会漏掉或卡死
 *           ③ 翻转后立即把 counter_ms 清 0（而不是减去 half_period_ms），
 *              这样计时误差不会累积，长期运行也不会漂移
 *           ④ half_period_ms == 0 用 continue 跳过，代表"这颗灯不参与闪烁"，
 *              这也是上电默认状态（静态数组自动清零）
 *           ⑤ 本函数不做任何延时、不阻塞，进出都很快，
 *              所以可以安全地放在 FreeRTOS 任务的循环体里
 *           ⑥ 只通过 PER_LED_Toggle() 操作硬件，保持"上→下单向调用"的分层纪律
 *----------------------------------------------------------------------------*/
uint8_t APP_BLINK_process(void)
{
    uint8_t i;

    /* 逐颗灯推进：灯少时看不出差别，灯一多这里就是"每颗灯各自独立计时的调度员" */
    for (i = 0; i < APP_BLINK_LED_COUNT; i++)
    {
        if (s_led_blink[i].half_period_ms == 0)
        {
            continue;  /* 这颗灯不闪烁，跳过（不计数、不翻转） */
        }

        s_led_blink[i].counter_ms++;  /* 计数器每 1ms 递增一次 */

        if (s_led_blink[i].counter_ms >= s_led_blink[i].half_period_ms)
        {
            s_led_blink[i].counter_ms = 0;        /* 到点：清零重计，误差不累积 */
            PER_LED_Toggle((PER_LED_t)i);         /* 翻转这颗灯的状态（编号按同序号映射） */
        }
    }

    return APP_BLINK_OK;
}

/*----------------------------------------------------------------------------
 * 函数功能：点亮某颗 LED 并让它退出闪烁（实现：先清半周期 → 再经感知层点灯）
 * 参数    ：led —— 状态机 LED 编号
 * 返回值  ：APP_BLINK_OK(0) / APP_BLINK_ERR_PARAM(1)   【接口契约见 app_blink.h】
 * 调用处  ：app_cmd.c 的 cmd_exec()（串口命令 "LED1 ON"）
 * 注意事项：① 顺序不能反：先清 half_period_ms（状态机立刻跳过这颗灯），
 *              再点灯。若反过来，两步之间若被 APP_BLINK_process() 插一脚，
 *              就会看到"点亮后又被翻灭"的鬼畜现象
 *           ② counter_ms 一并清零：下次 BLINK 命令重启闪烁时从零起算，
 *              不残留旧计数，第一下翻转的时间才准
 *----------------------------------------------------------------------------*/
uint8_t APP_BLINK_On(APP_BLINK_LED_t led)
{
    if (led >= APP_BLINK_LED_COUNT)
    {
        return APP_BLINK_ERR_PARAM;
    }

    s_led_blink[led].half_period_ms = 0;  /* 退出闪烁：状态机不再碰这颗灯 */
    s_led_blink[led].counter_ms     = 0;  /* 给下次重启闪烁留干净状态 */

    return PER_LED_On((PER_LED_t)led);
}

/*----------------------------------------------------------------------------
 * 函数功能：熄灭某颗 LED 并让它退出闪烁（实现：与 On 完全对称）
 * 参数    ：led —— 状态机 LED 编号
 * 返回值  ：APP_BLINK_OK(0) / APP_BLINK_ERR_PARAM(1)   【接口契约见 app_blink.h】
 * 调用处  ：app_cmd.c 的 cmd_exec()（串口命令 "LED1 OFF"）
 * 注意事项：与 APP_BLINK_On() 的差别只有最后一行 PER_LED_Off()
 *----------------------------------------------------------------------------*/
uint8_t APP_BLINK_Off(APP_BLINK_LED_t led)
{
    if (led >= APP_BLINK_LED_COUNT)
    {
        return APP_BLINK_ERR_PARAM;
    }

    s_led_blink[led].half_period_ms = 0;
    s_led_blink[led].counter_ms     = 0;

    return PER_LED_Off((PER_LED_t)led);
}

/*----------------------------------------------------------------------------
 * 函数功能：查询某颗 LED 的真实亮灭（实现：纯转发 PER_LED_IsOn）
 * 参数    ：led —— 状态机 LED 编号
 * 返回值  ：1 = 亮；0 = 灭（越界固定 0）   【接口契约见 app_blink.h】
 * 调用处  ：app_cmd.c 的 cmd_exec()（串口命令 "STATUS"）
 * 注意事项：本层刻意【不缓存】灯的软件状态——一路读到真实引脚，
 *           串口 STATUS 报出来的就是硬件此刻的真实状态
 *----------------------------------------------------------------------------*/
uint8_t APP_BLINK_IsOn(APP_BLINK_LED_t led)
{
    if (led >= APP_BLINK_LED_COUNT)
    {
        return 0U;
    }

    return PER_LED_IsOn((PER_LED_t)led);
}

#endif /* APP_BLINK_ENABLE */
