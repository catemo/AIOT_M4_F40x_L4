/**
 ******************************************************************************
 * @file    cfg_param.h
 * @brief   cfg 横切配置层 —— 模块参数（按模块分小节）
 *
 *  本文件是 module_cfg.h 的子文件（由 module_cfg.h 统一包含），不要单独包含。
 *  职责：节拍 / 闪烁半周期 / 任务栈与优先级这类"数值"。
 *  模块编不编（*_ENABLE 开关）在 cfg_switch.h；
 *  参数是否合法（下限、整数倍、越界）在 cfg_check.h 里体检。
 *
 *  ★ 任务栈单位说明见下方"任务栈 / 优先级"小节 —— CMSIS-RTOS2 的
 *    osThreadNew 里 stack_size 单位是【字节】，不是"字"！
 ******************************************************************************
 */
#ifndef CFG_PARAM_H
#define CFG_PARAM_H

/* ---------- 公共节拍 ---------- */
/* 状态机节拍(ms)：每 1ms 调用一次 APP_BLINK_process() 推进一次状态机。
 * 取值必须与 Core/Inc/FreeRTOSConfig.h 的 configTICK_RATE_HZ 对齐
 * （本工程 = 1000，即 1 tick = 1ms），因为 app_led 任务里用的是
 * osDelay(APP_BLINK_TICK_MS)——节拍改了而这里没改，闪烁就会整体变快变慢。 */
#define APP_BLINK_TICK_MS       1

/* ---------- LED 闪烁参数 ---------- */
/* LED1 半周期(ms)：每 500ms 翻转一次 → 1Hz 闪烁（亮 500ms + 灭 500ms）。
 * 改这一个数就改快慢：3000 → 慢闪；100 → 快闪。
 * 必须是 APP_BLINK_TICK_MS 的整数倍，否则实际周期会与设定值有偏差
 * （原因写在 cfg_check.h 的"参数体检"里）。 */
#define APP_LED1_BLINK_MS       500

/* ---------- 任务栈 / 优先级 ----------
 * ★ 单位说明：CMSIS-RTOS2 的 osThreadNew 里 stack_size 单位是【字节】！
 *   对照 Core/Inc/FreeRTOSConfig.h：configMINIMAL_STACK_SIZE = 128 字 = 512 字节，
 *   所以下面填 512 表示刚好等于"最小任务栈"，一个字都不多。
 *   若按"字"的思路填 256 想省 RAM，实际只分到 256 字节 → 栈溢出会静默写坏
 *   FreeRTOS 堆（不报错、不进 HardFault），现象是"跑几行后整机没反应"，
 *   极难定位。这条注释就是为了防止再犯。 */
#define APP_LED_TASK_STACK_SIZE 512   /* LED 任务栈大小(字节)：512 字节 = 128 字 = configMINIMAL_STACK_SIZE */
#define APP_LED_TASK_PRIO       1     /* LED 任务优先级：数字越大越高，须 < configMAX_PRIORITIES = 56 */

/* ---------- common 横切工具参数 ---------- */
#define CFG_LOG_LEVEL           2     /* 日志级别：0 关 / 1 错 / 2 警+错 / 3 全部（log.h） */
#define CFG_RINGBUF_SIZE        256   /* 环形缓冲容量，必须是 2 的幂（ringbuf.h，串口章节使用） */

/* ---------- 串口链（USART1）参数 ---------- */
/* DMA 硬件环形缓冲(字节)：DMA 循环模式转圈写的仓库，IDLE 中断判帧时从这里搬数据。
 * 256 = 115200 波特率下约 22ms 的数据量。取太小的后果：任务来不及消费时，
 * DMA 会绕回覆盖掉还没搬走的字节，表现为"命令缺字"。 */
#define NET_UART1_RX_DMA_BUF_SIZE   256

/* 发送超时(ms)：阻塞发送的最长等待时间，超时就放弃这一帧，绝不死等 */
#define NET_UART1_TX_TIMEOUT_MS     100

/* 行缓冲(字节)：串口任务攒一条命令的容器上限，超长字符直接丢弃防溢出。
 * 必须装得下最长命令 + 1：本例最长 "LED1 OFF" = 8 字节，64 留足扩展空间。 */
#define APP_UART1_LINE_MAX          64

/* ---------- 串口任务栈 / 优先级 ----------
 * ★ 单位仍是【字节】！串口任务比 LED 任务多两样开销：
 *   ① 行缓冲 APP_UART1_LINE_MAX（64 字节）
 *   ② 字符串处理（strcmp / strlen）+ 发送路径的调用深度
 *   所以给 1024（LED 任务的两倍），留出余量——栈溢出是静默的，不值得省这点 RAM。 */
#define APP_UART1_TASK_STACK_SIZE   1024  /* 串口命令任务栈大小(字节) */
#define APP_UART1_TASK_PRIO         1     /* 串口任务优先级：与 LED 任务平级，各让各的 CPU */

#endif /* CFG_PARAM_H */
