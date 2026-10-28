/**
 ******************************************************************************
 * @file    per_led.c
 * @brief   感知层（perception）—— LED 统一接口实现
 *
 *  实现特别简单：本层函数只做三件事 ——
 *    ① 检查编号有没有越界
 *    ② 把本层对外的编号翻译成 per_gpio 的编号（当前两者顺序一致，直接强转）
 *    ③ 调用同层的 per_gpio_xxx
 *
 *  这层"薄转发"看起来多余，但它把上下两层解耦了：
 *  以后驱动细节换函数名/换枚举顺序（比如加第二颗灯、改用 PWM 调光），
 *  只需改本文件的几行，应用层不受影响。
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 per_led.h，
 *  本文件的注释侧重"怎么转发、为什么这么写"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if PER_LED_ENABLE

#include "per_led.h"
#include "per_gpio.h"    /* 同层向下依赖，方向是 上→下 */

/* ========== 对外接口实现 ========== */

/*----------------------------------------------------------------------------
 * 函数功能：初始化 LED 硬件（实现：直接转发 per_gpio_init）
 * 参数    ：无
 * 返回值  ：per_gpio 层返回码原样上抛（PER_GPIO_LED_OK）
 * 调用处  ：app_blink.c 的 APP_BLINK_Init()   【接口契约见 per_led.h】
 * 注意事项：全链路最"薄"的转发，但保证了应用层不需要知道驱动的存在；
 *           本层对初始化不做任何加工——引脚怎么配是驱动细节的事
 *----------------------------------------------------------------------------*/
uint8_t PER_LED_Init(void)
{
    return per_gpio_init();
}

/*----------------------------------------------------------------------------
 * 函数功能：点亮指定编号的 LED（实现：越界检查 → 编号强转 → per_gpio_write(1)）
 * 参数    ：led —— 感知层 LED 编号（0 ~ PER_LED_COUNT-1）
 * 返回值  ：PER_LED_OK(0) = 成功；PER_LED_ERR_PARAM(1) = 编号越界
 * 调用处  ：app_blink.c 的 APP_BLINK_On()   【接口契约见 per_led.h】
 * 注意事项：① 先查编号再动手，非法请求一步都不到驱动层
 *           ② 本层只说"点亮"，不说"写低电平"——
 *              低电平点亮这件事是 per_gpio 表里的一个数字
 *----------------------------------------------------------------------------*/
uint8_t PER_LED_On(PER_LED_t led)
{
    if (led >= PER_LED_COUNT)
    {
        return PER_LED_ERR_PARAM;
    }

    per_gpio_write((PER_GPIO_LED_t)led, 1U);   /* 1 = 点亮（电平由驱动层翻译） */
    return PER_LED_OK;
}

/*----------------------------------------------------------------------------
 * 函数功能：熄灭指定编号的 LED（实现：越界检查 → 编号强转 → per_gpio_write(0)）
 * 参数    ：led —— 感知层 LED 编号
 * 返回值  ：PER_LED_OK(0) = 成功；PER_LED_ERR_PARAM(1) = 编号越界
 * 调用处  ：app_blink.c 的 APP_BLINK_Off()   【接口契约见 per_led.h】
 * 注意事项：与 PER_LED_On() 完全对称，只差最后传的 0 / 1
 *----------------------------------------------------------------------------*/
uint8_t PER_LED_Off(PER_LED_t led)
{
    if (led >= PER_LED_COUNT)
    {
        return PER_LED_ERR_PARAM;
    }

    per_gpio_write((PER_GPIO_LED_t)led, 0U);   /* 0 = 熄灭 */
    return PER_LED_OK;
}

/*----------------------------------------------------------------------------
 * 函数功能：查询指定编号 LED 的真实亮灭（实现：越界返回 0 → per_gpio_is_on）
 * 参数    ：led —— 感知层 LED 编号
 * 返回值  ：1 = 亮；0 = 灭（越界返回 0）
 * 调用处  ：app_blink.c 的 APP_BLINK_IsOn()   【接口契约见 per_led.h】
 * 注意事项：本层刻意不缓存"上次写的是什么"——每次都真去读引脚，
 *           这样 STATUS 命令报出来的状态与硬件永远一致
 *----------------------------------------------------------------------------*/
uint8_t PER_LED_IsOn(PER_LED_t led)
{
    if (led >= PER_LED_COUNT)
    {
        return 0U;
    }

    return per_gpio_is_on((PER_GPIO_LED_t)led);
}

/*----------------------------------------------------------------------------
 * 函数功能：翻转指定编号的 LED（实现：越界检查 → 编号强转 → per_gpio_toggle）
 * 参数    ：led —— 感知层 LED 编号（0 ~ PER_LED_COUNT-1）
 * 返回值  ：PER_LED_OK(0) = 成功；PER_LED_ERR_PARAM(1) = 编号越界
 * 调用处  ：app_blink.c 的 APP_BLINK_process()   【接口契约见 per_led.h】
 * 注意事项：① 越界时直接 return 错误码，非法编号绝不穿透到驱动细节（双保险之二）
 *           ② 强转成立的前提是 PER_LEDn 与 PER_GPIO_LEDn 编号顺序一致；
 *              驱动表改了枚举顺序必须同步改本文件
 *           ③ 本层与 per_gpio 对"越界"的处理策略刻意不同：本层返回错误码
 *              （让上层感知），per_gpio 静默吞掉（最后一道堤坝绝不能崩）
 *----------------------------------------------------------------------------*/
uint8_t PER_LED_Toggle(PER_LED_t led)
{
    if (led >= PER_LED_COUNT)
    {
        return PER_LED_ERR_PARAM;  /* 非法编号：直接返回，不触碰硬件、不破口 */
    }

    /* 转发同层的翻转函数：对外编号强转成 per_gpio 编号（当前两表一一对应） */
    per_gpio_toggle((PER_GPIO_LED_t)led);
    return PER_LED_OK;
}

#endif /* PER_LED_ENABLE */
