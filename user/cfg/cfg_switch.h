/**
 ******************************************************************************
 * @file    cfg_switch.h
 * @brief   cfg 横切配置层 —— 两级开关：模块总闸 + 分层子开关
 *
 *  本文件是 module_cfg.h 的子文件（由 module_cfg.h 统一包含），不要单独包含。
 *  职责只有一件事：决定"哪个模块编进程序"。
 *  参数（节拍 / 半周期 / 栈大小）在 cfg_param.h，
 *  打印开关在 cfg_print.h，依赖校验在 cfg_check.h。
 *
 *  ★【两级开关体系】每条模块链 = 1 个模块总开关 + 每层各自单独的子开关：
 *      MOD_LED_ENABLE                       模块总开关：0 = 整条链（感知→应用）全灭
 *      PER_GPIO_/PER_LED_/APP_BLINK_/APP_LED_ENABLE   分层子开关：(MOD_LED_ENABLE && 子值)
 *        —— 子值单独改 0 可只裁掉某一层（如只关应用层、保留感知层，
 *           这样驱动仍会被初始化，可以单独验证硬件）；
 *        —— 总开关置 0 时无论子值是什么整链全灭，排查硬件问题时一键断链。
 *    开关按链路分组，每组自上而下（与依赖方向一致）。
 *
 *  ★ 写法为什么要是 (MOD_LED_ENABLE && 1)？
 *    这样 #if 与普通 C 表达式两种场合都能直接用：
 *        #if PER_GPIO_ENABLE          → (1 && 1) → 真，代码参与编译
 *        if (PER_GPIO_ENABLE) {...}   → 整数 1   → 条件成立
 *    总闸拨 0 时整体变 0，两种场合同时失效——一处开关、处处同步。
 ******************************************************************************
 */
#ifndef CFG_SWITCH_H
#define CFG_SWITCH_H

/* ==========================================================================
 *  一、模块总开关（MOD_，每个模块一个） + 分层子开关（每层各自单独）
 *  编号顺序 = 依赖顺序：命令只能从上层往感知层传。
 * ========================================================================== */

/* ---------- LED 链（1 颗灯：LED1 = PG14，低电平点亮） ---------- */
#define MOD_LED_ENABLE      1   /* 模块总开关：LED 链（置 0 = 整条链一刀断，灯不再闪也不会报错） */
#define PER_GPIO_ENABLE     (MOD_LED_ENABLE && 1)   /* 感知层 per_gpio.c —— 唯一直接碰 HAL / 引脚号的一层        */
#define PER_LED_ENABLE      (MOD_LED_ENABLE && 1)   /* 感知层 per_led.c  —— 统一接口：编号 → 动作，层内门卫      */
#define APP_BLINK_ENABLE    (MOD_LED_ENABLE && 1)   /* 应用层 app_blink.c —— 闪烁状态机（整条链路的心脏）        */
#define APP_LED_ENABLE      (MOD_LED_ENABLE && 1)   /* 应用层 app_led.c  —— 业务："哪颗灯、多久闪一次"           */

/* ---------- 串口链（USART1：串口控灯，PA9=TX / PA10=RX，115200-8-N-1） ----------
 * 四层的串口链有三层（app → platform → network），与 LED 链的两层不同：
 *   网络层管"字节"（DMA / IDLE / 环形缓冲），平台层管"入口"（统一名字 + 协议位），
 *   应用层管"语义"（一行命令让灯做什么）。
 * 开关顺序 = 依赖顺序（自下而上）：network → platform → app。 */
#define MOD_UART1_ENABLE    1   /* 模块总开关：串口链（置 0 = 串口整体不参与编译，灯照闪）
                                 * ★ 它同时是【平台层】mod_uart1.c 的开关：平台层就是这条链的"入口"，
                                 *   入口没了整条链就不成立，所以平台层不单设子开关、与总闸同生共死 */
#define NET_UART1_ENABLE    (MOD_UART1_ENABLE && 1)   /* ③ 网络层 net_uart.c —— 字节流：DMA 循环 + IDLE 判帧 + 环形缓冲 */
#define APP_UART1_ENABLE    (MOD_UART1_ENABLE && 1)   /* ① 应用层 app_cmd.c —— 业务：收命令 → 解析 → 调 APP_BLINK_* 控灯 */

/* ---------- 任务集中创建（不属于某条链，单独一个开关） ---------- */
#define APP_TASK_ENABLE     1   /* 应用层 app_task.c —— 所有应用任务的"排班表" */

/* ---------- HAL 底层驱动（不属任何一条链，串口链的前置条件） ----------
 * 置 1 的两个前置（本工程已全部满足）：
 *   ① CubeMX 已配好 USART1 + DMA（Core 生成 usart.c / dma.c）
 *   ② Core/Inc/stm32f4xx_hal_conf.h 里已打开 HAL_UART_MODULE_ENABLED */
#define HAL_UART_MODULE_ENABLE   1   /* HAL UART 驱动编入（与 hal_conf 的 ENABLED 成对出现） */

/* ==========================================================================
 *  二、两级开关怎么用（四个真实场景，照着改就行）
 *
 *    场景① 一键断链——怀疑"灯不闪"不是 LED 链的问题，先把它整条摘掉：
 *              MOD_LED_ENABLE   改 0     ← 各层子开关自动跟着变 0，不用手改
 *              APP_TASK_ENABLE  改 0     ← 必须一起改，原因见下面的 ★
 *           程序照常跑，只是没有灯了。若此时系统正常，问题就锁定在链外。
 *
 *    场景② 只让驱动层参与编译——想省 Flash、或做最小验证：
 *              PER_GPIO_ENABLE   保持 1
 *              PER_LED_ENABLE / APP_BLINK_ENABLE / APP_LED_ENABLE  末尾 1 依次改 0
 *              APP_TASK_ENABLE  改 0
 *           ★ 注意：初始化是"从上往下调"的——
 *             app_led_task → app_led_init → APP_BLINK_Init → PER_LED_Init
 *             → per_gpio_init → HAL_GPIO_Init（链路图见 app_led.c 文件头）。
 *             裁掉上面几层后，per_gpio_init() 就没有人再调用它了，PG14 不会被
 *             配置成输出——这不是 bug，正是"依赖严格向下"的直接体现：
 *             下层不会自己动，要验证它就只能自己写几行测试代码去调。
 *
 *    场景③ 彻底不带这个模块（交付版/换项目时）：
 *           与场景①同一组改法（两行），各层子值一个字都不用动
 *           （总闸会把它们统一压成 0）。
 *
 *    场景④ 只要灯闪、不要串口（回退到 v2.1b 的纯闪烁版本）：
 *           MOD_UART1_ENABLE  改 0     ← NET_UART1 / APP_UART1 自动跟着变 0
 *           HAL_UART_MODULE_ENABLE 改 0（可选：连 HAL 驱动一起裁掉，再省一点 Flash）
 *           APP_TASK_ENABLE / MOD_LED_ENABLE  保持 1
 *         ★ 反过来"只要串口、不要闪灯"【不成立】：串口命令最终要经过
 *           APP_BLINK_* → PER_LED_* 去点灯，灯的下半段必须保留。
 *           想只留串口，应把 MOD_LED_ENABLE 保持 1（下半段在），
 *           再把 APP_LED_ENABLE 末尾的 1 改 0（只是不自动闪烁）：
 *           这样上电灯不闪，由串口命令接管——这才是本工程真正的"串口控灯"形态。
 *
 *    ★ 关开关时会不会把 APP_TASK_ENABLE 也关掉，看"还剩几个任务要建"：
 *      本例有两个任务——app_led_task（LED 链）与 app_cmd_task（串口链）。
 *      cfg_check.h 里那条校验是"APP_TASK 至少要有一个任务可建"：
 *        - 只关 LED 链（留串口）  → APP_TASK_ENABLE 保持 1
 *        - 只关串口链（留闪烁）  → APP_TASK_ENABLE 保持 1
 *        - 两条链都关（裸机骨架）→ APP_TASK_ENABLE 必须改 0，否则 #error
 *      这套闸门的目的只有一个：与其让编译过了、运行期莫名其妙，
 *      不如让人在改开关时就把依赖关系想清楚。
 * ========================================================================== */

#endif /* CFG_SWITCH_H */
