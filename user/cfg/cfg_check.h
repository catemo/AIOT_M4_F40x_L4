/**
 ******************************************************************************
 * @file    cfg_check.h
 * @brief   cfg 横切配置层 —— 依赖校验 + 参数体检（编译期闸门）
 *
 *  本文件是 module_cfg.h 的子文件（由 module_cfg.h 统一包含），不要单独包含。
 *  职责：配置写错就直接 #error，不让错误活过编译期——
 *    ① 层间依赖：上层开、下层关 = 悬挂配置，编译期拦下；
 *    ② 参数体检：栈太小、优先级越界、半周期不是节拍的整数倍，当场报错。
 *  开关在 cfg_switch.h，参数在 cfg_param.h，打印开关在 cfg_print.h。
 *
 *  ★ 本段基本不需要手改：将来加新模块时，在这里补一条互锁即可。
 *  ★ 阈值为什么写字面量（512、56）而不是 configMINIMAL_STACK_SIZE？
 *    因为 module_cfg.h 是各 .c 文件【第一个】被包含的头文件，此时 FreeRTOS.h
 *    还没进来，写宏名会被预处理当成 0 → "校验永远通过"，是假安全。
 *    所以这里写字面量，并在注释里注明它对应 FreeRTOSConfig.h 的哪一项配置。
 ******************************************************************************
 */
#ifndef CFG_CHECK_H
#define CFG_CHECK_H

/* ==========================================================================
 *  一、层间依赖校验（4 条：应用层 → 感知层，逐层向下）
 *  规则只有一条：上层可以依赖下层，下层绝不能反过来依赖上层。
 *  裁剪顺序：先保"下面几层全开"，再按需关掉上面的层。
 * ========================================================================== */

#if PER_LED_ENABLE && !PER_GPIO_ENABLE
#error "PER_LED_ENABLE requires PER_GPIO_ENABLE"   /* per_led 的编号动作要落到 per_gpio 的引脚表上 */
#endif

#if APP_BLINK_ENABLE && !PER_LED_ENABLE
#error "APP_BLINK_ENABLE requires PER_LED_ENABLE"  /* 闪烁状态机翻转灯要经过 per_led 门卫 */
#endif

#if APP_LED_ENABLE && !APP_BLINK_ENABLE
#error "APP_LED_ENABLE requires APP_BLINK_ENABLE"  /* 业务要靠闪烁状态机干活，两者必须同开 */
#endif

#if APP_TASK_ENABLE && !APP_LED_ENABLE && !APP_UART1_ENABLE
#error "APP_TASK_ENABLE requires APP_LED_ENABLE or APP_UART1_ENABLE"   /* 至少要有一个任务可建，否则排班表是空的 */
#endif

/* ==========================================================================
 *  二、串口链依赖校验（3 条：应用层 → 平台层 → 网络层 → HAL）
 *  串口链与 LED 链在 APP_BLINK 这一层"合流"：
 *  串口命令不直接点灯，而是交给闪烁状态机，否则两者会抢同一颗灯。
 * ========================================================================== */

/* ① 串口命令要落到闪烁状态机：APP_BLINK_On/Off/blink_set 都在这里，
 *    关掉它，app_cmd.c 里的调用就变成 undefined reference（链接期才炸，
 *    所以必须在编译期先拦下） */
#if APP_UART1_ENABLE && !APP_BLINK_ENABLE
#error "APP_UART1_ENABLE requires APP_BLINK_ENABLE"   /* 串口控灯要经闪烁状态机，不然会与闪烁抢灯 */
#endif

/* ② 应用层不许越过统一入口：直接调 net_uart_* 会让"换传输方式"的余地全丢 */
#if APP_UART1_ENABLE && !MOD_UART1_ENABLE
#error "APP_UART1_ENABLE requires MOD_UART1_ENABLE"   /* 业务只认平台层的 MOD_UART1_* 入口 */
#endif

/* ③ 平台层入口要落到网络层字节流；网络层还要 HAL 的 UART 驱动真的被编进来
 *    （HAL_UART_MODULE_ENABLE 必须与 stm32f4xx_hal_conf.h 的 ENABLED 成对出现，
 *     否则 usart.c 里的 HAL_UART_Init 根本不会被编译 → 链接期满屏 undefined） */
#if MOD_UART1_ENABLE && !NET_UART1_ENABLE
#error "MOD_UART1_ENABLE requires NET_UART1_ENABLE"   /* 统一入口要落到网络层的字节流实现上 */
#endif

#if MOD_UART1_ENABLE && !HAL_UART_MODULE_ENABLE
#error "MOD_UART1_ENABLE requires HAL_UART_MODULE_ENABLE"   /* 还须 hal_conf.h 里打开 HAL_UART_MODULE_ENABLED */
#endif

/* ==========================================================================
 *  三、参数体检（"能编过但跑不对"的参数组合，编译期就拒绝）—— 8 条
 * ========================================================================== */

/* 节拍不能为 0：osDelay(0) 不阻塞、不让出 CPU，状态机不再按毫秒推进；
 * 而且下面那条"整数倍"校验会出现除数为 0，所以先单独拦住，再进下一步。 */
#if APP_BLINK_TICK_MS < 1
#error "APP_BLINK_TICK_MS requires >= 1"
#else
/* 半周期必须是节拍的整数倍：
 * 状态机的写法是"每个节拍累加 tick_ms，累加值 >= 半周期才翻转"。
 * 若半周期填 500 而节拍是 3，实际会在 501ms 才翻转 → 闪烁周期偏大，
 * 并且这个偏差会一直累积。这就是"数毫秒"写法必须守的规矩。 */
#if (APP_LED1_BLINK_MS % APP_BLINK_TICK_MS) != 0
#error "APP_LED1_BLINK_MS requires an integer multiple of APP_BLINK_TICK_MS"
#endif
#endif

/* 半周期太短：一个节拍就跨过一个半周期，肉眼只看到常亮（或干脆看不出在闪）。 */
#if APP_LED1_BLINK_MS < (2 * APP_BLINK_TICK_MS)
#error "APP_LED1_BLINK_MS requires at least 2x APP_BLINK_TICK_MS"
#endif

/* 任务栈下限：512 字节 = configMINIMAL_STACK_SIZE(128 字) × 4。
 * 比它还小就会侵占 FreeRTOS 堆——栈溢出是静默的，不会当场报错。 */
#if APP_LED_ENABLE && (APP_LED_TASK_STACK_SIZE < 512)
#error "APP_LED_TASK_STACK_SIZE requires >= 512 (unit: BYTES = configMINIMAL_STACK_SIZE x 4)"
#endif

/* 任务优先级范围：0 等同于空闲任务优先级（任务可能永远得不到调度）；
 * >= 56 越界（FreeRTOSConfig.h 的 configMAX_PRIORITIES = 56）。 */
#if APP_LED_ENABLE && ((APP_LED_TASK_PRIO < 1) || (APP_LED_TASK_PRIO >= 56))
#error "APP_LED_TASK_PRIO requires 1 .. (configMAX_PRIORITIES - 1), configMAX_PRIORITIES = 56"
#endif

/* ==========================================================================
 *  四、串口链参数体检（3 条）
 * ========================================================================== */

/* 串口任务栈下限：与 LED 任务同一条 512 底线，但本例实际给的是 1024
 *（行缓冲 + 字符串处理要吃栈）。改小到 512 以下就会静默写坏 FreeRTOS 堆。 */
#if APP_UART1_ENABLE && (APP_UART1_TASK_STACK_SIZE < 512)
#error "APP_UART1_TASK_STACK_SIZE requires >= 512 (unit: BYTES = configMINIMAL_STACK_SIZE x 4)"
#endif

/* 串口任务优先级范围：与 LED 任务同一条规则 */
#if APP_UART1_ENABLE && ((APP_UART1_TASK_PRIO < 1) || (APP_UART1_TASK_PRIO >= 56))
#error "APP_UART1_TASK_PRIO requires 1 .. (configMAX_PRIORITIES - 1), configMAX_PRIORITIES = 56"
#endif

/* 行缓冲下限：8 字节才装得下最长的 "LED1 OFF"。
 * 装不下会怎样：命令被截断成 "LED1 O"，strcmp 永远匹配不上 →
 * 敲什么都是 ERR: unknown cmd，还查不出是缓冲太小的锅。 */
#if APP_UART1_ENABLE && (APP_UART1_LINE_MAX < 8)
#error "APP_UART1_LINE_MAX requires >= 8 (longest cmd: \"LED1 OFF\")"
#endif

#endif /* CFG_CHECK_H */
