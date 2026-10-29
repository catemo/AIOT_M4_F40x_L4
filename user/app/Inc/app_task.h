/**
 ******************************************************************************
 * @file    app_task.h
 * @brief   应用层（app）—— 用户任务集中创建（任务的"排班表"）
 *
 *  为什么要单独搞一个 app_task.c？
 *  因为 Core/Src/freertos.c 是 CubeMX 生成的，直接在里面写任务代码，
 *  一旦 CubeMX 重新生成就有被冲掉的风险。所以只在它的 USER CODE 区里
 *  留一行 app_taskCreate() 调用，把真正的任务创建都收在 user/ 里。
 *
 *  本文件只做"建任务"这一件事，不含任何业务逻辑；
 *  任务具体干什么，写在各自的 app_xxx.c 里（例如 app_led.c / app_cmd.c）。
 *
 *  本工程最多建六个任务（每新增一个模块，就来这里登记一个）：
 *      app_led_task      —— LED 闪烁：每 1ms 推进 APP_BLINK_process()
 *      app_cmd_task      —— 串口命令：收命令 → 解析 → 控灯（含 HANG 演示）
 *      app_dht11_task    —— DHT11 采集：每 2s 读温湿度 → 阈值联动 LED1/LED2
 *      app_mqtt_task     —— 上云：先连网，再每 5s 上报快照
 *      app_storage_task  —— 存储：参数读回 + 日志追加（一个任务两条链）
 *      app_watchdog_task —— 看门狗：500ms 喂狗 + 复位原因打印（2.4 新增）
 *  创建顺序是 LED → 串口 → DHT11 → 上云 → 存储 → 看门狗：
 *    - 先 LED：灯引脚先初始化好，别的任务才能点灯
 *    - 再串口：接收链路先起来，DHT11 的启动自检信息才不会丢
 *    - 再 DHT11：它既要灯（联动）、又要串口（打印），两个上游都得先就位
 *    - 再上云与存储：它们要读 DHT11 快照，采集先跑起来首条才不是空数据
 *    - 最后看门狗：它打印要走串口链，串口链路先就位启动行才不会丢
 ******************************************************************************
 */
#ifndef APP_TASK_H
#define APP_TASK_H

/*----------------------------------------------------------------------------
 * 函数功能：创建全部用户任务（最多 6 个，各自受 cfg 开关控制：
 *           LED / 串口 / DHT11 / 上云 / 存储 / 看门狗）
 * 参数    ：无
 * 返回值  ：无
 * 调用处  ：Core/Src/freertos.c 的 MX_FREERTOS_Init() 里，
 *           osKernelStart() 之前被调用
 *           （位于该文件的 USER CODE BEGIN RTOS_THREADS 区域内）
 *           完整启动链：main() → osKernelInitialize() → MX_FREERTOS_Init()
 *                       → app_taskCreate() → osThreadNew()
 * 注意事项：① 必须在调度器启动前调用。osThreadNew() 只是"登记"，任务要等
 *              osKernelStart() 之后才会真正开始运行
 *           ② 本函数不做任何业务初始化，只负责"建任务"；
 *              业务初始化放在各任务自己的开头（例如 app_led_init()）
 *           ③ 新增任务（按键、传感器…）统一写成
 *              osThreadNew(xxx_task, NULL, &xxx_attr) 加在本函数里，
 *              不要分散到各个 .c 去找
 *           ④ 创建失败时句柄为 NULL；本例在这里进 Error_Handler() 停机，
 *              避免任务静默丢失后"灯不亮又查不出原因"
 *           ⑤ 任务栈大小/优先级来自 cfg 的 cfg_param.h，
 *              注意堆空间由 FreeRTOSConfig.h 的 configTOTAL_HEAP_SIZE
 *              (32768 字节) 限制，栈开太大可能创建失败
 *----------------------------------------------------------------------------*/
void app_taskCreate(void);

#endif /* APP_TASK_H */
