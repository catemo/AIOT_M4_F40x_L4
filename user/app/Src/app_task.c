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
 *  本工程的七个任务（都在本文件登记，每个各套一层开关）：
 *      app_led_task      —— LED 闪烁任务（每 APP_BLINK_TICK_MS=10ms 推进 APP_BLINK_process）
 *      app_cmd_task      —— 串口命令任务（串口控灯章节：收命令 → 控灯）
 *      app_dht11_task    —— DHT11 采集任务（DHT11 章节：2 秒读一次 → 阈值联动）
 *      app_light_task    —— 光敏采集任务（2.8 章节：500 ms 读一次 → 分级控灯）
 *      app_mqtt_task     —— 上云任务（2.2 章节：先连网，再每 5 秒上报）
 *      app_storage_task  —— 存储任务（2.3 章节：一个任务驱动参数与日志两条链）
 *      app_watchdog_task —— 看门狗喂狗任务（2.4 章节：500 ms 喂狗 + HANG 演示）
 *  ★ OLED 显示（2.9）不建新任务：Temp/Hum 由 DHT11 任务刷、Light/Level 由光敏任务刷，
 *    本文件只负责在建任务之前把屏初始化好（APP_OLED_CreateMutex + APP_OLED_Init）。
 *  创建顺序刻意是"先 LED、再串口、再 DHT11、再光敏、再上云、再存储、最后看门狗"：
 *    - 先 LED：把灯引脚初始化好，后面的任务才能直接点灯
 *    - 再串口：把接收链路拉起来（DMA + IDLE），同时也做"快闪 3 次"上电自检
 *    - 再 DHT11：它要用串口打印，且启动自检要读 25 ms 的传感器
 *    - 再光敏：它要用 LED 的点灯接口与串口打印，放在这两条链之后最稳妥
 *    - 再上云与存储：它们要读 DHT11 的快照，采集先跑起来首条才不是空数据
 *    - 最后看门狗：它打印要走串口链，串口链路先就位启动行才不会丢
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
#include "app_dht11.h" /* DHT11 采集任务（DHT11 章节新增） */
#include "app_light.h" /* 光敏采集任务（2.8 章节新增） */
#include "app_oled.h"  /* OLED 显示（2.9 章节新增：只做上电初始化，不单独建任务） */
#include "app_mqtt.h"  /* WiFi-MQTT 上云任务（2.2 章节新增） */
#include "app_param.h" /* 存储任务（2.3 章节新增：一个任务驱动两条链） */
#include "app_watchdog.h" /* 看门狗喂狗任务（2.4 章节新增） */
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

#if APP_DHT11_ENABLE
/* ========== DHT11 采集任务 ========== */

/* 任务句柄：创建成功非 NULL、失败为 NULL（堆内存不足时） */
static TaskHandle_t s_app_dht11_task_handle = NULL;

/* 任务属性：名字 / 栈 / 优先级，数值全部来自 cfg 横切层 cfg_param.h。
 * 栈 768 字节（比 LED 大、比串口小）：
 *   - 比 LED 大：自检路径的函数调用深度深于普通采集
 *   - 比串口小：不用 printf 格式化库，省掉那一截栈开销（见 app_dht11.c） */
static const osThreadAttr_t s_app_dht11_task_attr = {
    .name       = "app_dht11_task",
    .stack_size = APP_DHT11_TASK_STACK_SIZE,         /* 栈大小（单位：字节！） */
    .priority   = (osPriority_t)APP_DHT11_TASK_PRIO, /* 数字越大优先级越高 */
};
#endif /* APP_DHT11_ENABLE */

#if APP_LIGHT_ENABLE
/* ========== 光敏采集任务（2.8 新增） ========== */

/* 任务句柄：创建成功非 NULL、失败为 NULL（堆内存不足时） */
static TaskHandle_t s_app_light_task_handle = NULL;

/* 任务属性：名字 / 栈 / 优先级，数值全部来自 cfg 横切层 cfg_param.h。
 * 栈 512（与看门狗任务同为最小档）：链路浅（app → per_light → HAL），
 * 不用 printf，只有一条 80 字节的打印行缓冲。 */
static const osThreadAttr_t s_app_light_task_attr = {
    .name       = "app_light_task",
    .stack_size = APP_LIGHT_TASK_STACK_SIZE,         /* 栈大小（单位：字节！） */
    .priority   = (osPriority_t)APP_LIGHT_TASK_PRIO, /* 与其余任务平级 */
};
#endif /* APP_LIGHT_ENABLE */

#if APP_MQTT_ENABLE
/* ========== WiFi-MQTT 上云任务 ========== */

/* 任务句柄：创建成功非 NULL、失败为 NULL（堆内存不足时） */
static TaskHandle_t s_app_mqtt_task_handle = NULL;

/* 任务属性：名字 / 栈 / 优先级，数值全部来自 cfg 横切层 cfg_param.h。
 * 栈给 2048（四个任务里最大）：
 *   - 报文拼接路径最深：app_mqtt → mod_mqtt → mod_wifi → net_esp8266 → net_uart
 *   - 局部变量里有 JSON 载荷与命令报文缓冲（各一百多字节）
 *   - 解析路径要过 strstr / memcmp 这类库函数 */
static const osThreadAttr_t s_app_mqtt_task_attr = {
    .name       = "app_mqtt_task",
    .stack_size = APP_MQTT_TASK_STACK_SIZE,         /* 栈大小（单位：字节！） */
    .priority   = (osPriority_t)APP_MQTT_TASK_PRIO, /* 数字越大优先级越高 */
};
#endif /* APP_MQTT_ENABLE */

#if APP_PARAM_ENABLE || APP_LOGGER_ENABLE
/* ========== 存储任务（一个任务驱动两条链） ========== */

/* 任务句柄：创建成功非 NULL、失败为 NULL（堆内存不足时） */
static TaskHandle_t s_app_storage_task_handle = NULL;

/* 任务属性：名字 / 栈 / 优先级，数值全部来自 cfg 横切层 cfg_param.h。
 * 栈给 2048：mod_flash 页编程要在栈上放 260 字节发送缓冲（七个任务里第二大户） */
static const osThreadAttr_t s_app_storage_task_attr = {
    .name       = "app_storage_task",
    .stack_size = APP_STORAGE_TASK_STACK_SIZE,      /* 栈大小（单位：字节！） */
    .priority   = (osPriority_t)APP_STORAGE_TASK_PRIO,
};
#endif /* APP_PARAM_ENABLE || APP_LOGGER_ENABLE */

#if APP_WATCHDOG_ENABLE
/* ========== 看门狗喂狗任务（2.4 新增） ========== */

/* 任务句柄：创建成功非 NULL、失败为 NULL（堆内存不足时） */
static TaskHandle_t s_app_wdt_task_handle = NULL;

/* 任务属性：名字 / 栈 / 优先级，数值全部来自 cfg 横切层 cfg_param.h。
 * 栈 512（七个任务里最小）：链路最浅（app → per_iwdg → HAL 就到底），
 * 不用 printf，行缓冲只有几十字节。 */
static const osThreadAttr_t s_app_wdt_task_attr = {
    .name       = "app_watchdog_task",
    .stack_size = APP_WDT_TASK_STACK_SIZE,         /* 栈大小（单位：字节！） */
    .priority   = (osPriority_t)APP_WDT_TASK_PRIO, /* 与其余任务平级 */
};
#endif /* APP_WATCHDOG_ENABLE */
#endif /* APP_TASK_ENABLE */

/*----------------------------------------------------------------------------
 * 函数功能：创建全部用户任务（本例程最多 7 个：app_led_task + app_cmd_task
 *           + app_dht11_task + app_light_task + app_mqtt_task + app_storage_task
 *           + app_watchdog_task，各自受 cfg 开关控制）
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
 *           ⑦ 三个任务的优先级都是 1（平级）：同优先级按时间片轮转，
 *              谁先 osDelay 谁先让出 CPU，互不抢占——本例刻意不设高低之分，
 *              避免"串口一来数据就把闪烁节拍挤掉"这类隐性优先级问题。
 *              ★ DHT11 任务也是 1：它虽然有关中断的 4~5 ms 关键段，
 *                但那是"必须连续占用"的硬件时序，与优先级无关
 *                （提优先级也挡不住别的任务被关中断前已经进来的那段）
 *           ⑧ ★ 本函数【永远存在】，但函数体受开关控制：
 *              APP_TASK_ENABLE = 0 时它是个空壳——什么任务都不建、直接返回；
 *              存在的唯一目的就是让 Core/Src/freertos.c 里那句调用永远能链接通过。
 *              正确的裁链改法见 cfg_switch.h 的四个场景（至少留一个任务可建）。
 *----------------------------------------------------------------------------*/
void app_taskCreate(void)
{
#if APP_TASK_ENABLE && APP_OLED_ENABLE
    /* ========== OLED 上电初始化（2.9 新增，先于所有任务） ==========
     * ★ 必须在这里做，不能放进某个任务里：
     *   ① DHT11 / 光敏任务的第一帧刷新就要写屏，屏晚初始化 = 头几帧丢掉；
     *   ② 此刻调度器还没启动，HAL_Delay 是忙等，600 ms 的电荷泵 + 自检
     *      不会耽误任何任务（进了调度之后再等 600 ms 就是真卡顿）。
     * ★ 互斥锁先建、再初始化：初始化本身就要写屏（自检 + 静态标签），
     *   而写屏函数一律先拿锁（锁为 NULL 时放行，见 app_oled.c）。 */
    APP_OLED_CreateMutex();
    APP_OLED_Init();
#endif /* APP_TASK_ENABLE && APP_OLED_ENABLE */

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

#if APP_TASK_ENABLE && APP_DHT11_ENABLE
    /* 创建 DHT11 采集任务：进入调度后由 app_dht11_task() 每 2 秒读一次。
     * ★ 创建顺序放在串口之后：它要用 LED 的点灯接口（灯必须先被初始化），
     *   也要用串口打印（串口链路先起来，启动自检的信息才不会丢）。 */
    s_app_dht11_task_handle = osThreadNew(app_dht11_task, NULL, &s_app_dht11_task_attr);
    if (s_app_dht11_task_handle == NULL)
    {
        Error_Handler();   /* 堆内存不足：停机保留现场 */
    }
#endif /* APP_TASK_ENABLE && APP_DHT11_ENABLE */

#if APP_TASK_ENABLE && APP_LIGHT_ENABLE
    /* 创建光敏采集任务：进入调度后由 app_light_task() 每 500 ms 读一次。
     * ★ 创建顺序放在 DHT11 之后：它要用 LED 的点灯接口与串口打印，
     *   这两条链先就位，启动行与控灯才不会落空。 */
    s_app_light_task_handle = osThreadNew(app_light_task, NULL, &s_app_light_task_attr);
    if (s_app_light_task_handle == NULL)
    {
        Error_Handler();   /* 堆内存不足：停机保留现场 */
    }
#endif /* APP_TASK_ENABLE && APP_LIGHT_ENABLE */

#if APP_TASK_ENABLE && APP_MQTT_ENABLE
    /* 创建上云任务：进入调度后由 app_mqtt_task() 先连网，再每 5 秒上报一次。
     * ★ 创建顺序放在最后一个，理由是"它要读 DHT11 的快照"——
     *   先让采集任务跑起来，第一条上报才不是空值。
     *   这也是本工程四个任务的固定次序：LED → 串口 → DHT11 → MQTT。 */
    s_app_mqtt_task_handle = osThreadNew(app_mqtt_task, NULL, &s_app_mqtt_task_attr);
    if (s_app_mqtt_task_handle == NULL)
    {
        Error_Handler();   /* 堆内存不足：停机保留现场 */
    }
#endif /* APP_TASK_ENABLE && APP_MQTT_ENABLE */

#if APP_TASK_ENABLE && (APP_PARAM_ENABLE || APP_LOGGER_ENABLE)
    /* 创建存储任务：进入调度后由 app_storage_task() 驱动两条存储链。
     * ★ 创建顺序放在最后：它要读 DHT11 的快照（采集先跑起来，首条日志才
     *   不是空数据），也是本工程五个任务的固定次序：
     *   LED → 串口 → DHT11 → MQTT → 存储。 */
    s_app_storage_task_handle = osThreadNew(app_storage_task, NULL, &s_app_storage_task_attr);
    if (s_app_storage_task_handle == NULL)
    {
        Error_Handler();   /* 堆内存不足：停机保留现场 */
    }
#endif /* APP_TASK_ENABLE && (APP_PARAM_ENABLE || APP_LOGGER_ENABLE) */

#if APP_TASK_ENABLE && APP_WATCHDOG_ENABLE
    /* 创建看门狗喂狗任务：进入调度后由 app_watchdog_task() 每 500 ms 喂一次狗。
     * ★ 创建顺序放在最后一个，理由是"它打印要走串口链、且只守护别人"：
     *   串口任务先起跑把 MOD_UART1 链路拉起来，它的启动两行才不会丢
     *   （任务内部还另留了 100 ms 余量，双保险）。
     *   至此本工程七个任务的固定次序：LED → 串口 → DHT11 → 光敏 → MQTT → 存储 → 看门狗。 */
    s_app_wdt_task_handle = osThreadNew(app_watchdog_task, NULL, &s_app_wdt_task_attr);
    if (s_app_wdt_task_handle == NULL)
    {
        Error_Handler();   /* 堆内存不足：停机保留现场 */
    }
#endif /* APP_TASK_ENABLE && APP_WATCHDOG_ENABLE */
}

/*----------------------------------------------------------------------------
 * 栈高水位上报（2026-10-30 新增，诊断命令 STACK 的后端）
 *
 *  为什么要有它：FreeRTOSConfig.h 的 configTOTAL_HEAP_SIZE = 32768 是
 *  拍脑袋留的 5.2 倍余量；要把它裁下来省 RAM（估算可回收 ~16 KB），
 *  必须先有七个任务的真实高水位数据——uxTaskGetStackHighWaterMark()
 *  返回"任务栈从创建以来被用到的最深水位之下还剩多少字"，
 *  乘 4 换算成字节。数出来之前不动堆大小，这是"取数必须机器化"的纪律。
 *
 *  读数口径：值越小越接近栈溢出；观察窗口要覆盖全部业务路径
 *  （连上云 + 打过诊断命令之后再读，才是真实峰值）。
 *----------------------------------------------------------------------------*/
#if APP_TASK_ENABLE
#if MOD_UART1_ENABLE
#include "mod_uart1.h"   /* 打印走平台层统一入口（与 app_cmd 同一套） */

/* 拼字符串 / 拼无符号数：与 app_mqtt.c、net_esp8266.c 同款小工具，
 * 各层自备一份，不引 printf（纪律见 net_esp8266.c 的说明） */
static uint32_t stk_cat(char *dst, uint32_t pos, uint32_t cap, const char *src)
{
    while ((src != NULL) && (*src != '\0') && ((pos + 1U) < cap))
    {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
    return pos;
}

static uint32_t stk_cat_u32(char *dst, uint32_t pos, uint32_t cap, uint32_t val)
{
    char     tmp[10];
    uint32_t n = 0U;

    if (val == 0U)
    {
        tmp[n++] = '0';
    }
    while (val > 0U)
    {
        tmp[n++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while ((n > 0U) && ((pos + 1U) < cap))
    {
        dst[pos++] = tmp[--n];
    }
    dst[pos] = '\0';
    return pos;
}

/* 上报一个任务的高水位；句柄 NULL = 该任务未创建（对应链被裁掉），跳过 */
static void app_task_report_one(const char *name, TaskHandle_t handle)
{
    char     line[56];
    uint32_t pos      = 0U;
    uint32_t hwm_bytes;

    if (handle == NULL)
    {
        return;
    }
    /* uxTaskGetStackHighWaterMark 的单位是"字"（StackType_t = 4 字节），
     * ×4 换算成字节——忘了乘就是把字节报小了 4 倍，会误导裁堆决策 */
    hwm_bytes = (uint32_t)uxTaskGetStackHighWaterMark(handle) * 4U;

    pos = stk_cat(line, 0U, sizeof(line), "[STACK] ");
    pos = stk_cat(line, pos, sizeof(line), name);
    pos = stk_cat(line, pos, sizeof(line), " min_free=");
    pos = stk_cat_u32(line, pos, sizeof(line), hwm_bytes);
    (void)stk_cat(line, pos, sizeof(line), " B\r\n");
    MOD_UART1_SendString(line);
}
#endif /* MOD_UART1_ENABLE */
#endif /* APP_TASK_ENABLE */

void app_task_report_stacks(void)
{
#if APP_TASK_ENABLE && MOD_UART1_ENABLE
    /* 打印次序与创建次序一致（LED → 串口 → DHT11 → 光敏 → 上云 → 存储 → 看门狗），
     * 方便和 app_task.c 里的创建代码逐行对照。
     * ★ 每一行都要跟着它自己的 APP_x_ENABLE 走：句柄是在同名的 #if 里定义的，
     *   链被裁掉时这里若还引用，就是链接期 / 编译期的 undeclared（2.8 实测撞过）。 */
#if APP_LED_ENABLE
    app_task_report_one("app_led_task",      s_app_led_task_handle);
#endif
#if APP_UART1_ENABLE
    app_task_report_one("app_cmd_task",      s_app_cmd_task_handle);
#endif
#if APP_DHT11_ENABLE
    app_task_report_one("app_dht11_task",    s_app_dht11_task_handle);
#endif
#if APP_LIGHT_ENABLE
    app_task_report_one("app_light_task",    s_app_light_task_handle);
#endif
#if APP_MQTT_ENABLE
    app_task_report_one("app_mqtt_task",     s_app_mqtt_task_handle);
#endif
#if APP_PARAM_ENABLE || APP_LOGGER_ENABLE
    app_task_report_one("app_storage_task",  s_app_storage_task_handle);
#endif
#if APP_WATCHDOG_ENABLE
    app_task_report_one("app_watchdog_task", s_app_wdt_task_handle);
#endif
#endif
}

/*----------------------------------------------------------------------------
 * 空闲钩子：CPU 没活干就睡（FreeRTOS 空闲任务每次循环调用一次）
 *
 *  ★ 这是本次性能/功耗优化收益最大的一行（2026-10-30）：
 *    不开钩子时，所有任务都在 osDelay 睡觉的空档里，idle 任务在
 *    168 MHz 下原地空转——本固件任务负载占空比 <10%，等于九成时间在白烧电。
 *    __WFI() 让内核进 Sleep 模式（SLEEPDEEP 复位默认 0，不是 STOP）：
 *      - 任一中断（SysTick 1kHz / TIM6 1kHz / 串口 IDLE / DMA）即唤醒，
 *        唤醒后从中断返回继续调度，不需要任何恢复代码
 *      - 外设时钟全部保持：TIM2 微秒计数照跑、DMA 照搬、串口照收
 *      - RAM 与寄存器全保持，唤醒后第一拍就正确
 *    预期收益：空闲期核电流降 50~70%（Sleep 模式典型值，需整机实测确认）。
 *
 *  注意事项：① 本函数【无条件定义】：configUSE_IDLE_HOOK=1 时内核按名字调用，
 *              不能藏在 APP_TASK_ENABLE 里，否则裁剪场景会 undefined reference
 *           ② 为什么不顺便开 configUSE_TICKLESS_IDLE：HAL 时基 TIM6 每毫秒
 *              中断一次，会把刚睡下去的 CPU 立刻叫醒，tickless 形同虚设；
 *              要吃到 tickless 的收益得先把 HAL 时基迁走或门控，属于
 *              "需要真机专项验证"的后续项，本次不做
 *           ③ 调试器下 WFI 的表现取决于 DBGMCU 配置，可能不真睡——
 *              以整机平均电流实测为准，不要拿调试器挂着的功耗当结论
 *----------------------------------------------------------------------------*/
#include "main.h"   /* __WFI() 来自 CMSIS（经 stm32f4xx_hal.h），无条件包含 */

void vApplicationIdleHook(void)
{
    __WFI();
}
