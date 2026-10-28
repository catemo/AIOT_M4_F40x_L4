/**
 ******************************************************************************
 * @file    app_led.h
 * @brief   应用层（app）—— LED 闪烁业务（1 颗灯：LED1）
 *
 *  这是"业务"：只表达需求 —— 想让哪颗灯、以多快的速度闪，
 *  具体怎么闪（app_blink）、怎么点灯（感知层）完全不用管。
 *
 *  想改闪烁快慢：改 cfg 的 cfg_param.h 里 APP_LED1_BLINK_MS（单位 ms，半周期）
 *
 *  ★ 本文件只 include app_blink.h，绝不 include per_led.h / per_gpio.h。
 ******************************************************************************
 */
#ifndef APP_LED_H
#define APP_LED_H

/*----------------------------------------------------------------------------
 * 函数功能：应用层业务初始化 —— 把灯要闪的速度告诉闪烁状态机
 * 参数    ：无
 * 返回值  ：无
 * 调用处  ：app_led.c 的 app_led_task() 开头，任务第一次被调度到时调用一次
 * 注意事项：① 必须在任务开始运行后调用（而不是在 main 里提前调用），
 *              位置放在任务体开头最安全
 *           ② 只负责"下命令"，不负责"让灯动起来"；
 *              真正让灯闪的是 app_led_task() 循环里的 APP_BLINK_process()
 *           ③ 参数取自 cfg 横切层的 APP_LED1_BLINK_MS；
 *              想把 LED1 改成 2Hz，只改 cfg_param.h 一个宏（编译期生效）
 *           ④ 完整初始化链：
 *              app_led_init → APP_BLINK_Init → PER_LED_Init → per_gpio_init，
 *              一条命令从最上层一路走到最底层——这就是"四层架构"最直观的例子
 *----------------------------------------------------------------------------*/
void app_led_init(void);

/*----------------------------------------------------------------------------
 * 函数功能：LED 应用任务体（FreeRTOS 任务函数）—— 给状态机提供 1ms 心跳
 * 参数    ：argument —— 任务参数，本任务不用，由 app_task.c 创建时传 NULL
 * 返回值  ：无（void 函数，而且是死循环，永远不会返回）
 * 调用处  ：不是被代码直接调用的！而是由 app_task.c 的 app_taskCreate() 通过
 *           osThreadNew() 注册给 FreeRTOS 内核，由调度器在启动后自动调用
 *           （创建链：MX_FREERTOS_Init → app_taskCreate → osThreadNew）
 * 注意事项：① ★ 函数体内是 for(;;) 死循环，任务永不退出：
 *              FreeRTOS 任务如果返回，行为是未定义的（通常是 HardFault），
 *              所以任何任务函数都必须写成死循环或最后自我删除
 *           ② 循环里必须 osDelay()，让出 CPU 给其它任务；
 *              去掉 osDelay 会导致本任务一直占着 CPU，其它任务饿死
 *           ③ osDelay(APP_BLINK_TICK_MS) 目前是 1ms，与 FreeRTOSConfig.h 的
 *              configTICK_RATE_HZ = 1000 配套；改这个值会直接改变闪烁速度
 *           ④ 本任务优先级为 APP_LED_TASK_PRIO(1)，仅在 cfg 里配
 *           ⑤ 不要在任务里写 HAL_Delay()，那会空等、阻塞调度，应该用 osDelay()
 *----------------------------------------------------------------------------*/
void app_led_task(void *argument);

#endif /* APP_LED_H */
