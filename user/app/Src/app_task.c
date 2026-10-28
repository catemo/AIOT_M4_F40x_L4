/**
 ******************************************************************************
 * @file    app_task.c
 * @brief   应用层（app）—— 用户任务集中创建
 *
 *  把 FreeRTOS 的任务创建都收在这一个文件里，好处是：
 *    新建任务时只来这里加几行，不用到处找代码。
 *
 *  为什么不能直接写在 freertos.c 里：
 *    Core/Src/freertos.c 是 CubeMX 生成的，直接在里面写任务代码，
 *    一旦 CubeMX 重新生成就有被冲掉的风险。所以只在它的 USER CODE 区里
 *    留一行 app_taskCreate() 调用，把真正的任务创建都收在 user/ 里 ——
 *    物理隔离，重新生成代码永不丢用户工作。
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_task.h，
 *  本文件的注释侧重"任务是怎么登记进内核的"。
 *
 *  本工程的两个任务（都在本文件登记，每个各套一层开关）：
 *      app_led_task   —— LED 闪烁任务（每 1ms 推进 APP_BLINK_process）
 *      app_cmd_task   —— 串口命令任务（串口控灯章节新增：收命令 → 控灯）
 *  创建顺序刻意是"先 LED 后串口"：LED 任务先把灯初始化好，
 *  串口任务起来后才能直接做"快闪 3 次"的上电自检。
 *
 *  ★ 本文件被 cfg 的开关控制，但函数 app_taskCreate() 本身【永远存在】：
 *      APP_TASK_ENABLE = 1 → 正常建任务（当前情形）
 *      APP_TASK_ENABLE = 0 → 函数体变空壳，只为让 Core/Src/freertos.c 里那句
 *                            无条件调用 app_taskCreate() 仍能链接通过。
 *    为什么不能把整个文件用 #if 包起来：那样关掉开关会变成 undefined
 *    reference（链接错误），而"关开关"本该只是配置操作，不该炸链接。
 ******************************************************************************
 */
#include "module_cfg.h"
#include "app_task.h"   /* 先拿到函数原型：本文件必须无条件提供 app_taskCreate() */

#if APP_TASK_ENABLE
#include "app_led.h"
#include "app_cmd.h"   /* 串口命令任务（串口控灯章节新增） */
#include "cmsis_os.h"  /* FreeRTOS 的 CMSIS-RTOS V2 封装：osThreadNew 等 */
#include "main.h"      /* 包含 stm32f4xx_hal.h，提供 Error_Handler 与 LED 引脚宏 */
#include "task.h"      /* FreeRTOS 原生任务 API（TaskHandle_t 等） */

#if APP_LED_ENABLE
/* ========== LED 应用任务 ========== */

/* 任务句柄：osThreadNew() 的返回值存这里。
 * 本例只创建、不删除、不用它做同步，所以其实可以不留；但保留句柄有实际意义：
 *   - 以后可以拿它做 osThreadSuspend() / osThreadTerminate() 等操作
 *   - 也是判断"任务是否创建成功"的唯一依据（成功非 NULL、失败为 NULL） */
static TaskHandle_t s_app_led_task_handle = NULL;

/* 任务属性：名字（调试器里看得到的线程名）、栈大小、优先级。
 * 数值全部来自 cfg 横切层 cfg_param.h，改参数不用动本文件。
 * 用 static const 是为了放进 Flash 且防止被意外改写。 */
static const osThreadAttr_t s_app_led_task_attr = {
    .name       = "app_led_task",
    .stack_size = APP_LED_TASK_STACK_SIZE,        /* 栈大小（单位：字节！），来自 cfg */
    .priority   = (osPriority_t)APP_LED_TASK_PRIO, /* 数字越大优先级越高，来自 cfg */
};

#endif /* APP_LED_ENABLE */

#if APP_UART1_ENABLE
/* ========== 串口命令任务 ========== */

/* 任务句柄：创建成功非 NULL、失败为 NULL（堆内存不足时） */
static TaskHandle_t s_app_cmd_task_handle = NULL;

/* 任务属性：名字 / 栈 / 优先级，数值全部来自 cfg 横切层 cfg_param.h。
 * 栈比 LED 任务大一倍：串口任务里有 APP_UART1_LINE_MAX 的行缓冲，
 * 还要容纳 strcmp / strlen 等字符串处理的调用深度 */
static const osThreadAttr_t s_app_cmd_task_attr = {
    .name       = "app_cmd_task",
    .stack_size = APP_UART1_TASK_STACK_SIZE,         /* 栈大小（单位：字节！） */
    .priority   = (osPriority_t)APP_UART1_TASK_PRIO, /* 数字越大优先级越高 */
};
#endif /* APP_UART1_ENABLE */
#endif /* APP_TASK_ENABLE */

/*----------------------------------------------------------------------------
 * 函数功能：创建全部用户任务（本例程创建 2 个：app_led_task + app_cmd_task）
 * 参数    ：无
 * 返回值  ：无
 * 调用处  ：Core/Src/freertos.c 的 MX_FREERTOS_Init()，在 osKernelStart() 之前
 *           【接口契约与完整启动链见 app_task.h】
 * 注意事项：① osThreadNew() 只是"登记"，函数返回时任务还没跑起来；
 *              真正的执行要等 main() 调完 osKernelStart()
 *           ② 第一个参数直接传函数名（app_led_task，函数名即函数指针），
 *              不要写成 app_led_task()，那样是"调用"而不是"传递"——
 *              编译能过但行为完全错（而且再也不会返回）
 *           ③ 第二个参数 NULL 表示不给任务传参
 *           ④ 每个任务都套一层 #if APP_xxx_ENABLE 开关：在 cfg 里关掉某个模块，
 *              对应任务就整体不参与编译，不需要手工删代码
 *           ⑤ 创建失败（堆内存不足）时返回 NULL，本例进 Error_Handler() 停机，
 *              比带着隐患继续跑更好定位
 *           ⑥ 新增任务时照抄这 3 步：定义句柄 → 定义属性 → osThreadNew，
 *              并确保新任务的 .c 文件已被 CMake 的 glob 收集到
 *           ⑦ 两个任务的优先级都是 1（平级）：同优先级按时间片轮转，
 *              谁先 osDelay 谁先让出 CPU，互不抢占——本例刻意不设高低之分，
 *              避免"串口一来数据就把闪烁节拍挤掉"这类隐性优先级问题
 *           ⑧ ★ 本函数【永远存在】，但函数体受开关控制：
 *              APP_TASK_ENABLE = 0 时它是个空壳——什么任务都不建、直接返回；
 *              存在的唯一目的就是让 Core/Src/freertos.c 里那句调用永远能链接通过。
 *              正确的裁链改法见 cfg_switch.h 的四个场景（至少留一个任务可建）。
 *----------------------------------------------------------------------------*/
void app_taskCreate(void)
{
#if APP_TASK_ENABLE && APP_LED_ENABLE
    /* 创建 LED 任务：进入调度后由 app_led_task() 一直跑下去 */
    s_app_led_task_handle = osThreadNew(app_led_task, NULL, &s_app_led_task_attr);
    if (s_app_led_task_handle == NULL)
    {
        /* 任务创建失败（多为 configTOTAL_HEAP_SIZE 不够）：停机并保留现场 */
        Error_Handler();
    }
#endif /* APP_TASK_ENABLE && APP_LED_ENABLE */

#if APP_TASK_ENABLE && APP_UART1_ENABLE
    /* 创建串口命令任务：进入调度后由 app_cmd_task() 一直跑下去。
     * ★ 创建顺序刻意放在 LED 任务之后：LED 任务先把灯初始化好，
     *   串口任务起来后才能直接做"快闪 3 次"的自检动作 */
    s_app_cmd_task_handle = osThreadNew(app_cmd_task, NULL, &s_app_cmd_task_attr);
    if (s_app_cmd_task_handle == NULL)
    {
        Error_Handler();   /* 堆内存不足：停机保留现场 */
    }
#endif /* APP_TASK_ENABLE && APP_UART1_ENABLE */
}
