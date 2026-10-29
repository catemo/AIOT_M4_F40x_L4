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

#if APP_TASK_ENABLE && !APP_LED_ENABLE && !APP_UART1_ENABLE && !APP_DHT11_ENABLE && !APP_MQTT_ENABLE && !APP_LOGGER_ENABLE && !APP_WATCHDOG_ENABLE
#error "APP_TASK_ENABLE requires APP_LED_ENABLE or APP_UART1_ENABLE or APP_DHT11_ENABLE or APP_MQTT_ENABLE or APP_LOGGER_ENABLE or APP_WATCHDOG_ENABLE"   /* 至少要有一个任务可建，否则排班表是空的 */
#endif

/* ==========================================================================
 *  二、串口链依赖校验（4 条：应用层 → 平台层 → 网络层 → HAL）
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
 *  三、DHT11 链依赖校验（5 条：应用层 → 感知层，外加一条跨链依赖）
 *  ★ 第 ② 条是本册最值得记住的一条：APP_LINK 属于 DHT11 链，
 *    却要借 LED 链的 per_led 去点灯。跨链依赖在代码里只有一行 include，
 *    一旦漏配，错误要到链接期才炸（满屏 undefined reference）。
 *    把它写进闸门，改开关时就会被人看见。
 * ========================================================================== */

/* ① 联动要有数据来源：app_link 只做判定、自己不读传感器 */
#if APP_LINK_ENABLE && !APP_DHT11_ENABLE
#error "APP_LINK_ENABLE requires APP_DHT11_ENABLE"   /* 阈值判定的输入就是采集结果 */
#endif

/* ② 联动借 LED 链的点灯接口（跨链依赖） */
#if APP_LINK_ENABLE && !PER_LED_ENABLE
#error "APP_LINK_ENABLE requires PER_LED_ENABLE"   /* 温度/湿度联动要用 per_led 去点 LED1 / LED2 */
#endif

/* ③ 周期采集要落到单总线驱动 */
#if APP_DHT11_ENABLE && !PER_DHT11_ENABLE
#error "APP_DHT11_ENABLE requires PER_DHT11_ENABLE"   /* 采集任务读的是 per_dht11 的 40 位 */
#endif

/* ④ 单总线时序要 µs 计时底座：没有 TIM2，"26 µs vs 70 µs"根本分辨不出来 */
#if PER_DHT11_ENABLE && !PER_DELAY_ENABLE
#error "PER_DHT11_ENABLE requires PER_DELAY_ENABLE"   /* 位脉宽判定离不开 1 µs 时基 */
#endif

/* ⑤ 打印要走串口链的平台层入口（与 app_cmd 同一条规矩）：
 *    直接调 net_uart_* 就等于绕过平台层，将来换传输方式要改一堆地方 */
#if APP_DHT11_ENABLE && !MOD_UART1_ENABLE
#error "APP_DHT11_ENABLE requires MOD_UART1_ENABLE"   /* 打印只认平台层的 MOD_UART1_* 入口 */
#endif

/* ==========================================================================
 *  四、参数体检（"能编过但跑不对"的参数组合，编译期就拒绝）—— 5 条
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
 *  五、串口链参数体检（3 条）
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

/* ==========================================================================
 *  六、DHT11 链参数体检（7 条）
 * ========================================================================== */

/* 采集周期下限：DHT11 最快 1 Hz。比 1 秒还密，传感器根本来不及转换，
 * 表现是"读到的永远是上一次的旧值"或直接超时 —— 且极难与接线故障区分。 */
#if APP_DHT11_ENABLE && (APP_DHT11_PERIOD_MS < 1000)
#error "APP_DHT11_PERIOD_MS requires >= 1000 (DHT11 max sampling rate is 1 Hz)"
#endif

/* 重试次数下限：填 0 等于"一次都不试"，采集循环会直接走进失败分支 */
#if APP_DHT11_ENABLE && (APP_DHT11_RETRY_COUNT < 1)
#error "APP_DHT11_RETRY_COUNT requires >= 1"
#endif

/* 重试会耗时间：N 次重试最坏要 (N-1) × 间隔 毫秒，必须留在一个周期内，
 * 否则采集周期会被重试悄悄撑长，周期就不再是你设定的那个数。 */
#if APP_DHT11_ENABLE && \
    (((APP_DHT11_RETRY_COUNT - 1) * APP_DHT11_RETRY_INTERVAL_MS) > (APP_DHT11_PERIOD_MS / 2))
#error "APP_DHT11_RETRY_COUNT x APP_DHT11_RETRY_INTERVAL_MS must fit in half of APP_DHT11_PERIOD_MS"
#endif

/* 温度阈值范围：DHT11 量程 0~50℃。阈值设到量程外，这颗灯就永远不会亮——
 * 与"灯坏了"的表现一模一样，是最冤的一种调参事故。 */
#if APP_LINK_ENABLE && (APP_LINK_TEMP_THRESHOLD > 50)
#error "APP_LINK_TEMP_THRESHOLD requires <= 50 (DHT11 range: 0..50 C)"
#endif

/* 湿度阈值范围：DHT11 量程 20~90 %RH（量程外同理） */
#if APP_LINK_ENABLE && ((APP_LINK_HUMI_THRESHOLD < 20) || (APP_LINK_HUMI_THRESHOLD > 90))
#error "APP_LINK_HUMI_THRESHOLD requires 20 .. 90 (DHT11 range: 20..90 %RH)"
#endif

/* DHT11 任务栈下限：与另两个任务同一条 512 底线。
 * 本任务实际给 768：自检路径的函数调用深度深于普通采集。 */
#if APP_DHT11_ENABLE && (APP_DHT11_TASK_STACK_SIZE < 512)
#error "APP_DHT11_TASK_STACK_SIZE requires >= 512 (unit: BYTES = configMINIMAL_STACK_SIZE x 4)"
#endif

/* DHT11 任务优先级范围：与另两个任务同一条规则 */
#if APP_DHT11_ENABLE && ((APP_DHT11_TASK_PRIO < 1) || (APP_DHT11_TASK_PRIO >= 56))
#error "APP_DHT11_TASK_PRIO requires 1 .. (configMAX_PRIORITIES - 1), configMAX_PRIORITIES = 56"
#endif

/* ==========================================================================
 *  七、上云链（WiFi-MQTT）依赖校验（5 条）
 *  ★ 这条链有五跳，比前三条都长：app → mod_mqtt → mod_wifi → net_esp8266
 *    → net_uart(USART3)。五条闸门串成一条链，少一环就在编译期报出来。
 * ========================================================================== */

/* ① 业务要发报文：app_mqtt 调的是 MOD_MQTT_* 的四个动作 */
#if APP_MQTT_ENABLE && !MOD_MQTT_ENABLE
#error "APP_MQTT_ENABLE requires MOD_MQTT_ENABLE"   /* 越过报文层直接发裸数据 = 没有协议可言 */
#endif

/* ② 报文要有落点：mod_mqtt 发出去的每一帧都要经 mod_wifi 的 Send */
#if MOD_MQTT_ENABLE && !MOD_WIFI_LINK_ENABLE
#error "MOD_MQTT_ENABLE requires MOD_WIFI_LINK_ENABLE"   /* 编好的报文没有传输层可发 */
#endif

/* ③ 统一入口要有后端：mod_wifi 的五个动作最终落到 AT 会话 */
#if MOD_WIFI_LINK_ENABLE && !NET_ESP8266_ENABLE
#error "MOD_WIFI_LINK_ENABLE requires NET_ESP8266_ENABLE"   /* 语义动作没有 AT 层可落 */
#endif

/* ④ AT 指令要有字节流通道：net_esp8266 收发都走 USART3 实例 */
#if NET_ESP8266_ENABLE && !NET_UART3_ENABLE
#error "NET_ESP8266_ENABLE requires NET_UART3_ENABLE"   /* AT 文本没有串口可发 */
#endif

/* ⑤ 串口还要 HAL 的 UART 驱动真的被编进来（与 MOD_UART1 同一条规矩） */
#if NET_UART3_ENABLE && !HAL_UART_MODULE_ENABLE
#error "NET_UART3_ENABLE requires HAL_UART_MODULE_ENABLE"   /* 否则 usart.c 里 HAL_UART_Init 不编译 */
#endif

/* ==========================================================================
 *  八、上云链参数体检（6 条）
 *  这几条都是"能编过、上云才发现不对"的组合：要么刷爆云端额度，
 *  要么报文被截断，要么任务栈溢出（静默写坏堆）。
 * ========================================================================== */

/* 上报周期下限：5 秒已经在"够快"的边缘，比 1 秒还密纯粹是浪费流量与额度 */
#if APP_MQTT_ENABLE && (APP_MQTT_PUBLISH_PERIOD_MS < 1000)
#error "APP_MQTT_PUBLISH_PERIOD_MS requires >= 1000 (broker quota & DHT11 is 1 Hz max)"
#endif

/* 心跳下限：keepalive 太小会让 Broker 频繁判定超时；10 秒是常用下限 */
#if APP_MQTT_ENABLE && (APP_MQTT_KEEPALIVE_SEC < 10)
#error "APP_MQTT_KEEPALIVE_SEC requires >= 10"
#endif

/* 任务栈下限：2048 是实际给的值，比它小就先担心一下报文缓冲与解析路径 */
#if APP_MQTT_ENABLE && (APP_MQTT_TASK_STACK_SIZE < 1024)
#error "APP_MQTT_TASK_STACK_SIZE requires >= 1024 (unit: BYTES)"
#endif

/* 优先级范围：与前三个任务同一条规则（0 = 空闲优先级，>= 56 越界） */
#if APP_MQTT_ENABLE && ((APP_MQTT_TASK_PRIO < 1) || (APP_MQTT_TASK_PRIO >= 56))
#error "APP_MQTT_TASK_PRIO requires 1 .. (configMAX_PRIORITIES - 1), configMAX_PRIORITIES = 56"
#endif

/* 发送缓冲下限：主题（约 60 字节）+ JSON（约 90 字节）+ 报文头，
 * 缓冲不够的后果不是"发失败"，而是"发出去半帧"——Broker 直接断链 */
#if MOD_MQTT_ENABLE && (MOD_MQTT_TX_BUF_SIZE < 256)
#error "MOD_MQTT_TX_BUF_SIZE requires >= 256 (topic + JSON + fixed header)"
#endif

/* 主题缓冲下限 = 命令响应主题的固定部分(约 60) + rid 缓冲(47) + 结尾 '\0'。
 * ★ 装不下的后果不是"订阅不到命令"，而是更隐蔽的一种：
 *   上报和订阅都正常，唯独"命令响应"那个最长的主题被 app_cat 静默截断，
 *   云端匹配不上，控制台永远停在"等待响应"，而串口日志什么都看不出来。 */
#if MOD_MQTT_ENABLE && (MOD_MQTT_TOPIC_MAX < 108)
#error "MOD_MQTT_TOPIC_MAX requires >= 108 (fixed part 60 + request_id 47 + NUL)"
#endif

/* ==========================================================================
 *  九、存储链依赖校验（2.3 新增，4 条 + 2 条跨链）
 *  两条新链与串口链同构：app → platform → network。
 *  ★ 总闸写法不同（NET_I2C_/NET_SPI_ENABLE 直接当总闸），所以这里的闸门
 *    不是摆设：就算哪天有人把子开关改成独立数值，依赖关系仍会被拦下。
 * ========================================================================== */

/* ① 业务只许经平台层碰总线：越过 mod_eeprom/mod_flash 直接调 net_*，
 *    "换器件只改平台层"的余地就全丢了 */
#if APP_PARAM_ENABLE && !MOD_EEPROM_ENABLE
#error "APP_PARAM_ENABLE requires MOD_EEPROM_ENABLE"   /* 参数业务只认平台层的器件语义 */
#endif
#if APP_LOGGER_ENABLE && !MOD_FLASH_ENABLE
#error "APP_LOGGER_ENABLE requires MOD_FLASH_ENABLE"   /* 日志业务只认平台层的指令集 */
#endif

/* ② 平台层要有总线后端 */
#if MOD_EEPROM_ENABLE && !NET_I2C_ENABLE
#error "MOD_EEPROM_ENABLE requires NET_I2C_ENABLE"     /* 器件语义没有总线可落 */
#endif
#if MOD_FLASH_ENABLE && !NET_SPI_ENABLE
#error "MOD_FLASH_ENABLE requires NET_SPI_ENABLE"      /* 指令集没有 SPI 包络可发 */
#endif

/* ③ 打印要走串口链的平台层入口（与 app_dht11 同一条规矩） */
#if (APP_PARAM_ENABLE || APP_LOGGER_ENABLE) && !MOD_UART1_ENABLE
#error "storage chain requires MOD_UART1_ENABLE (its log lines go through MOD_UART1_SendString)"
#endif

/* ④ 日志的取材是 DHT11 快照（跨链依赖）
 *   ★ 2.4 已把这条依赖写进 cfg_switch.h 的开关表达式：
 *     APP_LOGGER_ENABLE = (MOD_FLASH_ENABLE && APP_DHT11_ENABLE)
 *     ——所以关 DHT11 链时日志层自动跟着关，正常配置下本闸门【永远不会响】。
 *   留着它是"回归闸门"：谁把那个表达式改回 (MOD_FLASH_ENABLE)，关 DHT11 时
 *   就会在这里被编译期拦下，而不是等到链接期才发现 APP_DHT11_GetSnapshot 找不到。 */
#if APP_LOGGER_ENABLE && !APP_DHT11_ENABLE
#error "APP_LOGGER_ENABLE requires APP_DHT11_ENABLE (log records come from the DHT11 snapshot)"
#endif

/* ==========================================================================
 *  十、存储链参数体检（2.3 新增，5 条）
 * ========================================================================== */

/* 容量闸门：AT24C02 一共 256 字节，配置块放不下就是配置错误，
 * 编译期就拦——否则运行期写越界会把 EEPROM 后半段悄悄写花 */
#if APP_PARAM_ENABLE && ((APP_PARAM_CFG_OFFSET + APP_PARAM_CFG_SIZE) > AT24C02_SIZE_BYTES)
#error "APP_PARAM_CFG_OFFSET + APP_PARAM_CFG_SIZE exceeds AT24C02 capacity (256 B)"
#endif

/* 栈闸门：页编程缓冲(260 B) + 记录缓冲 + 拼行打印都在栈上 */
#if (APP_PARAM_ENABLE || APP_LOGGER_ENABLE) && (APP_STORAGE_TASK_STACK_SIZE < 2048)
#error "APP_STORAGE_TASK_STACK_SIZE requires >= 2048 (unit: BYTES! page-program buffer + record + print line)"
#endif

/* 基础节拍下限：存储任务每拍要睡够，0 会吃满 CPU */
#if (APP_PARAM_ENABLE || APP_LOGGER_ENABLE) && (APP_STORAGE_TICK_MS < 1)
#error "APP_STORAGE_TICK_MS requires >= 1"
#endif

/* 两个周期都必须是基础节拍的整数倍：否则实际读回/追加的间隔会与设定值偏差 */
#if APP_PARAM_ENABLE && ((APP_PARAM_READBACK_MS % APP_STORAGE_TICK_MS) != 0)
#error "APP_PARAM_READBACK_MS requires an integer multiple of APP_STORAGE_TICK_MS"
#endif
#if APP_LOGGER_ENABLE && ((APP_LOGGER_PERIOD_MS % APP_STORAGE_TICK_MS) != 0)
#error "APP_LOGGER_PERIOD_MS requires an integer multiple of APP_STORAGE_TICK_MS"
#endif

/* 追加周期下限：一次追加最坏 = 擦扇区 400 ms + 页编程 + 读回比对，
 * 周期太短会把存储任务自己拍满，还挤占同拍的上云上报 */
#if APP_LOGGER_ENABLE && (APP_LOGGER_PERIOD_MS < 1000)
#error "APP_LOGGER_PERIOD_MS requires >= 1000 (worst append = sector erase 400 ms + program + verify)"
#endif

/* ==========================================================================
 *  十一、看门狗链依赖校验（2.4 新增，3 条：应用层 → 感知层 → HAL，含一条跨链）
 * ========================================================================== */

/* ① 喂狗业务要落到感知层驱动：app_watchdog 只认 PER_IWDG_* */
#if APP_WATCHDOG_ENABLE && !PER_IWDG_ENABLE
#error "APP_WATCHDOG_ENABLE requires PER_IWDG_ENABLE"   /* 喂狗动作最终要落到 HAL_IWDG_Refresh */
#endif

/* ② 感知层要 HAL 的 IWDG 驱动真的被编进来（与 UART 同一条规矩） */
#if PER_IWDG_ENABLE && !HAL_IWDG_MODULE_ENABLE
#error "PER_IWDG_ENABLE requires HAL_IWDG_MODULE_ENABLE"   /* 还须 hal_conf.h 里打开 HAL_IWDG_MODULE_ENABLED */
#endif

/* ③ 打印要走串口链的平台层入口（跨链依赖，与 app_dht11 / 存储链同一条规矩） */
#if APP_WATCHDOG_ENABLE && !MOD_UART1_ENABLE
#error "APP_WATCHDOG_ENABLE requires MOD_UART1_ENABLE"   /* [WDT] 日志行只认 MOD_UART1_* 出口 */
#endif

/* ==========================================================================
 *  十二、看门狗链参数体检（2.4 新增，6 条）
 * ========================================================================== */

/* 预分频合法集：IWDG 的 PR 寄存器只有 3 位有效，硬件只认这 7 个分频。
 * 填别的数编译能过，但 per_iwdg.c 的映射表查不到 → 初始化直接进 assert 停机，
 * 不如编译期就拦下。 */
#if PER_IWDG_ENABLE && (PER_IWDG_DIV != 4) && (PER_IWDG_DIV != 8) && \
    (PER_IWDG_DIV != 16) && (PER_IWDG_DIV != 32) && (PER_IWDG_DIV != 64) && \
    (PER_IWDG_DIV != 128) && (PER_IWDG_DIV != 256)
#error "PER_IWDG_DIV requires one of 4/8/16/32/64/128/256 (IWDG PR is 3-bit)"
#endif

/* 重载值上限：RLR 是 12 位寄存器，超过 4095 的高位会被硬件静默截断——
 * 实际超时比你想的短，"为什么不按设定的超时复位"能查到怀疑人生。 */
#if PER_IWDG_ENABLE && ((PER_IWDG_RELOAD < 1U) || (PER_IWDG_RELOAD > 4095U))
#error "PER_IWDG_RELOAD requires 1..4095 (IWDG RLR is 12-bit)"
#endif

/* 喂狗周期底线：喂狗间隔必须远小于超时，否则一次任务抖动就咬狗。
 * 取"周期 × 2 ≤ 超时"作底线（500 ms vs 3000 ms 时余量 2.5 s）。
 * 注意 PER_IWDG_TIMEOUT_MS 是派生宏，DIV/RELOAD 一改它自动跟着变。 */
#if APP_WATCHDOG_ENABLE && ((APP_WDT_FEED_PERIOD_MS * 2U) > PER_IWDG_TIMEOUT_MS)
#error "APP_WDT_FEED_PERIOD_MS x 2 must fit in PER_IWDG_TIMEOUT_MS (feed margin)"
#endif

/* Alive 节奏下限：填 0 会出现除 0，先单独拦住 */
#if APP_WATCHDOG_ENABLE && (APP_WDT_ALIVE_EVERY < 1U)
#error "APP_WDT_ALIVE_EVERY requires >= 1"
#endif

/* 看门狗任务栈下限：同一条 512 底线（本任务实际给的就是 512） */
#if APP_WATCHDOG_ENABLE && (APP_WDT_TASK_STACK_SIZE < 512)
#error "APP_WDT_TASK_STACK_SIZE requires >= 512 (unit: BYTES = configMINIMAL_STACK_SIZE x 4)"
#endif

/* 看门狗任务优先级范围：与其余任务同一条规则 */
#if APP_WATCHDOG_ENABLE && ((APP_WDT_TASK_PRIO < 1) || (APP_WDT_TASK_PRIO >= 56))
#error "APP_WDT_TASK_PRIO requires 1 .. (configMAX_PRIORITIES - 1), configMAX_PRIORITIES = 56"
#endif

#endif /* CFG_CHECK_H */
