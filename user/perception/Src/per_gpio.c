/**
 ******************************************************************************
 * @file    per_gpio.c
 * @brief   感知层（perception）—— GPIO 驱动实现（LED1 = PG14）
 *
 *  本文件是 LED 链路里唯一直接操作 GPIO 寄存器的地方，上层全部通过
 *  per_gpio.h 里的函数来操作灯，换硬件只改这一个文件（确切说：s_led_hw[] 表）。
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 per_gpio.h，
 *  本文件的注释侧重"代码是怎么实现的"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if PER_GPIO_ENABLE

#include "per_gpio.h"
#include "main.h"          /* 提供 LED1_Pin / LED1_GPIO_Port 与 HAL 头文件 */

/* ========== 硬件描述表（硬件抽象核心：换板子只改这里） ==========
 * 每颗灯一行：
 *   port        —— GPIO 端口（本例 LED1 在 GPIOG）
 *   pin         —— GPIO 引脚（本例 GPIO_PIN_14）
 *   active_low  —— 1 = 低电平点亮（写 RESET 亮），0 = 高电平点亮
 *
 * 把"端口 / 引脚 / 亮灭方向"都放进表里，逻辑代码就完全不认识 PG14，
 * 换引脚、改板子、发现"亮灭反了"，都只改这张表，不要去改函数体。
 */
typedef struct
{
    GPIO_TypeDef *port;  /* GPIO 端口 */
    uint16_t pin;        /* GPIO 引脚 */
    uint8_t active_low;  /* 亮灭逻辑：1 = 低电平点亮，0 = 高电平点亮 */
} PER_GPIO_HW_t;

static const PER_GPIO_HW_t s_led_hw[PER_GPIO_LED_COUNT] =
{
    {LED1_GPIO_Port, LED1_Pin, 1},  /* LED1 = PG14，低电平点亮 */
};

/* ========== 对外接口实现 ========== */

/*----------------------------------------------------------------------------
 * 函数功能：初始化 LED1 引脚（时钟 + 推挽输出 + 先置为熄灭）
 * 参数    ：无
 * 返回值  ：PER_GPIO_LED_OK(0) = 成功
 * 调用处  ：per_led.c 的 PER_LED_Init()   【接口契约见 per_gpio.h】
 * 注意事项：① 时钟必须先开：GPIOG 时钟没使能时后面所有寄存器写操作全部无效，
 *              表现是"程序不报错，但灯就是不亮"——排查点灯问题时先查这一行
 *           ② 低电平点亮的灯，"灭" = 写高电平(SET)；写反了一上电就常亮
 *           ③ 上电防闪：引脚复位后输出寄存器默认是 0(低)。本例顺序是
 *              "先配置成输出，再写灭电平"，功能上完全正确；
 *              若要连配置瞬间的微弱闪动都避免，可把 HAL_GPIO_WritePin(SET)
 *              提到 HAL_GPIO_Init() 之前——先把输出寄存器写成"灭"，
 *              再切成输出态
 *           ④ 幂等：重复调用只是把寄存器重写一遍，状态不乱
 *----------------------------------------------------------------------------*/
uint8_t per_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 使能 GPIO 时钟：LED1 挂在 GPIOG 上（这一步漏了，后面全部无效） */
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* 配置 GPIO 引脚为输出模式 */
    gpio.Pin   = LED1_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;   /* 推挽输出：能主动输出高、也能主动拉低 */
    gpio.Pull  = GPIO_NOPULL;           /* 无上下拉：点灯是强驱动，不需要内部电阻 */
    gpio.Speed = GPIO_SPEED_FREQ_LOW;   /* 低速：点灯不需要高速翻转，低速可减小干扰 */
    HAL_GPIO_Init(LED1_GPIO_Port, &gpio);

    /* 初始化为熄灭状态：低电平点亮 → 灭 = 高电平(SET) */
    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);  /* 高电平熄灭 */

    return PER_GPIO_LED_OK;
}

/*----------------------------------------------------------------------------
 * 函数功能：把指定 LED 设置成"亮"或"灭"（实现：查表拿端口/引脚/亮灭方向 → 写电平）
 * 参数    ：led —— 本层 LED 编号；on —— 1 = 点亮，0 = 熄灭
 * 返回值  ：无（void）
 * 调用处  ：per_led.c 的 PER_LED_On() / PER_LED_Off()   【接口契约见 per_gpio.h】
 * 注意事项：① 越界与 toggle 同一策略：直接返回，不破口（最后一道堤坝）
 *           ② 亮灭方向的翻译就在这 6 行里：active_low 决定"亮"是 RESET 还是 SET。
 *              板子换成高电平点亮时，改 s_led_hw[] 表里的一个数字即可，
 *              函数体、上层代码全部不动——这就是"硬件描述表"的收益
 *----------------------------------------------------------------------------*/
void per_gpio_write(PER_GPIO_LED_t led, uint8_t on)
{
    uint8_t      i     = (uint8_t)led;
    GPIO_PinState level;

    if (i >= PER_GPIO_LED_COUNT)
    {
        return;  /* 越界直接返回，不破口 */
    }

    /* 把"亮/灭"翻译成"高/低电平"：低电平点亮的灯，亮 = 拉低 */
    if (s_led_hw[i].active_low != 0U)
    {
        level = (on != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET;
    }
    else
    {
        level = (on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }

    HAL_GPIO_WritePin(s_led_hw[i].port, s_led_hw[i].pin, level);
}

/*----------------------------------------------------------------------------
 * 函数功能：查询指定 LED 当前是亮还是灭（实现：读引脚电平 → 反向翻译成亮/灭）
 * 参数    ：led —— 本层 LED 编号
 * 返回值  ：1 = 亮；0 = 灭（越界返回 0）
 * 调用处  ：per_led.c 的 PER_LED_IsOn()   【接口契约见 per_gpio.h】
 * 注意事项：① 与 write 共用一个 active_low，方向相反：
 *              低电平点亮 → 读到 RESET(0) 才算"亮"
 *           ② 读的是 ODR 输出寄存器（本函数只用于推挽输出的灯），
 *              不做输入口读取；这样"软件写的"和"读回来的"必然一致
 *----------------------------------------------------------------------------*/
uint8_t per_gpio_is_on(PER_GPIO_LED_t led)
{
    uint8_t       i     = (uint8_t)led;
    GPIO_PinState level;

    if (i >= PER_GPIO_LED_COUNT)
    {
        return 0U;  /* 越界：当作灭（查询类接口静默兜底） */
    }

    level = HAL_GPIO_ReadPin(s_led_hw[i].port, s_led_hw[i].pin);

    /* 反向翻译：低电平点亮的灯，读到低电平 = 亮 */
    if (s_led_hw[i].active_low != 0U)
    {
        return (level == GPIO_PIN_RESET) ? 1U : 0U;
    }

    return (level == GPIO_PIN_SET) ? 1U : 0U;
}

/*----------------------------------------------------------------------------
 * 函数功能：翻转指定 LED 的电平（亮 ↔ 灭切换一次）
 * 参数    ：led —— 本层 LED 编号
 * 返回值  ：无（void）；编号越界时什么也不做
 * 调用处  ：per_led.c 的 PER_LED_Toggle()   【接口契约见 per_gpio.h】
 * 注意事项：① 越界检查是本层"最后一道堤坝"：宁可什么都不做，也绝不能去操作
 *              别的引脚（防止程序跑飞时误动外设）
 *           ② 翻转只对输出寄存器(ODR)做一次异或，与 active_low 无关——
 *              比"读状态 → 判断 → 写回"少两步、少一次竞态，
 *              所以闪烁一律用 Toggle，而不是 Set/Reset
 *----------------------------------------------------------------------------*/
void per_gpio_toggle(PER_GPIO_LED_t led)
{
    uint8_t i = (uint8_t)led;

    if (i >= PER_GPIO_LED_COUNT)
    {
        return;  /* 越界直接返回，不破口：宁可不动，也不能去操作别的引脚 */
    }

    /* 查硬件描述表拿到真实端口/引脚，再交给 HAL 翻转 */
    HAL_GPIO_TogglePin(s_led_hw[i].port, s_led_hw[i].pin);
}

#endif /* PER_GPIO_ENABLE */
