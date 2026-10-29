/**
 ******************************************************************************
 * @file    module_cfg.h
 * @brief   cfg 横切配置层 —— 全工程唯一配置入口（总入口文件）
 *
 *  本目录只有头文件、没有 .c 文件，其它四层都要包含它。
 *  作用：把"哪个模块编进程序""闪烁多快""栈开多大"这类可调项集中放一处，
 *        改参数只改这一个入口即可，上层业务代码一行都不用动。
 *
 *  ============================ 四层链路 ============================
 *   应用层 app        app_led.c / app_blink.c / app_cmd.c / app_task.c
 *                     app_dht11.c / app_link.c / app_mqtt.c
 *                                            业务：闪多快 / 命令解析 / 周期采集 / 阈值联动 / 上云
 *   平台层 platform   mod_uart1.c / mod_wifi.c / mod_mqtt.c
 *                                            接口：串口1 入口 / WiFi 五个动作 / MQTT 报文
 *   网络层 network    net_uart.c（实例表：USART1 + USART3）/ net_esp8266.c
 *                                            收发：DMA+IDLE+环形缓冲 / AT 会话 + +IPD 分流
 *   感知层 perception per_led.c / per_gpio.c / per_dht11.c / per_delay.c
 *                                            硬件：灯引脚表 / 单总线时序 / µs 时基
 *   cfg 横切          module_cfg.h + 4 个子文件   配置：开关与参数（本文件）
 *  ==================================================================
 *  命令只能从上往下传（应用层 → 感知层），下层永远不知道上层存在。
 *  应用层可直达感知层（本地闭环，如按键控灯），禁止回头与横调。
 *
 *  四条链、两个合流点：
 *    LED 链    app_blink → per_led → per_gpio                  （两跳）
 *    串口链    app_cmd → mod_uart1 → net_uart(USART1)          （三跳）
 *    DHT11 链  app_dht11 → per_dht11                           （两跳，联动 app_link 借 per_led 点灯）
 *    上云链    app_mqtt → mod_mqtt → mod_wifi → net_esp8266
 *                        → net_uart(USART3)                     （五跳，两层平台 + 两层网络）
 *    ★ 串口 / DHT11 / 上云三条链共用 MOD_UART1_* 打印出口，
 *      手动命令与自动闪烁则在 APP_BLINK_* 处互斥（合流点①），
 *      温度联动与自动闪烁在 per_led 处按边沿写灯（合流点②，见 app_link.c）。
 *
 *  本例（IOT_M4_L4）规模：2 颗灯（LED1 = PG14 温度指示、LED2 = PG13 湿度指示，
 *  均低电平点亮）、DHT11 单总线（PG9）+ TIM2 微秒时基；
 *  1 Hz 闪烁 + 串口控灯（USART1）+ 每 2 秒温湿度采集与阈值联动
 *  + 每 5 秒 MQTT 上报华为云（USART3 → ESP8266 → 1883）；
 *  FreeRTOS 任务由 app_task.c 集中创建（LED / 串口命令 / DHT11 采集 / 上云，共 4 个）。
 *
 *  ★ 本文件是"总入口"，配置按【职责】拆成 4 个子文件——各管一摊，
 *    要改哪一类就进哪个文件，改坏了也碰不到别的：
 *      1. cfg_switch.h  两级开关 —— 编不编看这里
 *      2. cfg_print.h   串口调试打印开关 —— 业务输出直出，总闸保持 0
 *      3. cfg_param.h   模块参数 —— 节拍 / 半周期 / 栈 / 优先级 / 上云参数
 *      4. cfg_check.h   依赖校验 + 参数体检 —— 配错 #error，编译期闸门
 *    包含顺序有讲究：cfg_check.h 必须最后，因为它要引用前面三份的宏做校验。
 *    业务代码一律 #include "module_cfg.h"，不要直接包含子文件。
 ******************************************************************************
 */
#ifndef MODULE_CFG_H
#define MODULE_CFG_H

/* 1. 两级开关（编不编、怎么编） */
#include "cfg_switch.h"

/* 2. 串口调试打印开关（本例业务回复直出，总闸保持 0，结构留着） */
#include "cfg_print.h"

/* 3. 模块参数（节拍 / 闪烁半周期 / 任务栈 / 优先级） */
#include "cfg_param.h"

/* 4. 依赖校验 + 参数体检（必须最后：校验要用上面三份的宏） */
#include "cfg_check.h"

#endif /* MODULE_CFG_H */
