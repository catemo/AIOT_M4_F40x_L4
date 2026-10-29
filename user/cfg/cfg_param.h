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

/* 发送超时(ms)：阻塞发送的最长等待时间，超时就放弃这一帧，绝不死等。
 * ★ 本册两个串口共用这一个值（发送路径是同一份代码，超时没有理由分家） */
#define NET_UART_TX_TIMEOUT_MS      100

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

/* ---------- DHT11 链（PG9 单总线）参数 ---------- */
/* 采集周期(ms)：DHT11 最快 1 Hz，2 秒是稳妥值。
 * 取 1000 会在临界处翻车（传感器自己还没准备好）；取 5000 则对哈气这类
 * 快速变化响应太慢。2 秒是"既不催它、又不迟钝"的平衡点。 */
#define APP_DHT11_PERIOD_MS         2000

/* 读取失败重试次数：连续失败这么多次才放弃本轮。
 * 单次读取约 25 ms + 重试间隔 100 ms，3 次最坏约 375 ms，
 * 仍远小于 2 秒周期——重试不会把周期撑爆，这是取 3 而不是取 10 的依据。 */
#define APP_DHT11_RETRY_COUNT       3

/* 重试间隔(ms)：两次尝试之间给传感器一点恢复时间（用 osDelay，会出让 CPU） */
#define APP_DHT11_RETRY_INTERVAL_MS 100

/* 上电稳定期(ms)：DHT11 上电后需约 1 秒越过不稳定期。
 * 少了它，第一次读取大概率 stage=1，表现是"上电必失败一次"的假故障。 */
#define APP_DHT11_STABILIZE_MS      1000

/* ---------- 阈值联动（app_link）----------
 * ★ 阈值放在 cfg 而不是 app_link.c 里：改行为只改这两行，
 *   联调时（比如想把湿度阈值调到 55%）不用去翻业务代码，
 *   也不用重新理解判定逻辑——这就是"参数与逻辑分离"的收益。 */
#define APP_LINK_TEMP_THRESHOLD     31    /* 温度阈值(℃)：≥ 该值点亮 LED1(PG14) */
#define APP_LINK_HUMI_THRESHOLD     60    /* 湿度阈值(%)：≥ 该值点亮 LED2(PG13) */

/* 打印行缓冲(字节)：一行 Temp/Hum 只要 30 来个字符，64 留足余量。
 * ★ 本册刻意【不做】"上电 LED1 快闪 3 次"这个动作：
 *   串口章节（app_cmd）已经在启动时快闪过 3 次做自检，
 *   这里再来一遍是重复动作；更要紧的是 LED1 此刻还得留给 1 Hz 闪烁与
 *   温度联动，再插一段 720 ms 的闪烁会把三者搅在一起，看不出谁在控制灯。
 *   本册改用【串口打印自检行】当启动标志——既不与别人争灯，还多报了信息。 */
#define APP_DHT11_PRINT_BUF_SIZE       64

/* ---------- DHT11 任务栈 / 优先级 ----------
 * 栈比 LED 任务大、比串口任务小（768 字节）：
 *   ① 要放 PER_DHT11_Data_t + PER_DHT11_Diag_t + 行缓冲（合计不到 80 字节）
 *   ② 自检时会走一次完整的 DHT11 读取路径（函数调用深度深于普通采集）
 *   ③ 不用 printf：本任务自己拼字符串，省掉格式化库的栈开销（见 app_dht11.c） */
#define APP_DHT11_TASK_STACK_SIZE   768   /* DHT11 采集任务栈大小(字节) */
#define APP_DHT11_TASK_PRIO         1     /* DHT11 任务优先级：与另两个任务平级 */

/* ---------- 上云链（WiFi-MQTT）网络层参数 ---------- */
/* USART3 的 DMA 接收缓冲(字节)：取 512，是 USART1 的两倍。
 * 原因是这一条线上同时跑三样东西：AT 文本响应（OK / SEND OK / CLOSED）、
 * +IPD 头、以及后面的二进制 MQTT 报文。模块在连发时会一口气吐上百字节，
 * 缓冲小了就会截断——而 MQTT 报文被截断的现象是"CONNACK 永远等不到"，
 * 极难与"三元组错"区分。 */
#define NET_UART3_RX_DMA_BUF_SIZE   512

/* ESP8266 响应累积缓冲(字节)：AT 响应与 +IPD 头都先落在这里，
 * 再由 net_esp8266 分流。要能装下一次 CWJAP 的完整回显，512 有余量。 */
#define NET_ESP8266_RESP_BUF_SIZE   512

/* MQTT 载荷环形缓冲(字节)：+IPD 里剥出来的二进制报文放这里（不分行、不加 \n）。
 * 一帧华为云下发命令约 120 字节，256 能存两帧，够主循环 50ms 走一圈。 */
#define NET_ESP8266_PAYLOAD_BUF_SIZE 256

/* ---------- 上云链（WiFi-MQTT）平台层参数 ---------- */
/* MQTT 发送缓冲(字节)：一帧 PUBLISH 要装下"固定头 + 剩余长度 + 主题 + JSON"。
 * 主题约 60 字节（含 32 位 device_id），JSON 约 100 字节，320 留足余量。
 * ★ 缓冲不够时的现象不是"发不出去"，而是"发出去一半"——报文被截断后
 *   Broker 直接断开连接，所以这条下限在 cfg_check.h 里有闸门看着。 */
#define MOD_MQTT_TX_BUF_SIZE        320

/* 主题缓冲(字节)：最长的主题是命令响应
 *   "$oc/devices/{device_id}/sys/commands/response/request_id={rid}"
 *   ├─ 固定部分："$oc/devices/" + device_id + "/sys/commands/response/request_id=" 约 60 字节
 *   └─ rid 部分：取自 app_mqtt.c 的 rid[]（48 字节，可装 47 字符）
 * ★ 必须 ≥ 固定部分 + rid 长度 + 1，否则响应主题会被 app_cat 静默截断：
 *   云端匹配不到"命令-响应"这一对，控制台会永远显示"等待响应"，
 *   而串口日志看起来一切正常（Published 了）——这是最难查的一类失败。
 * ★ 换了更长的 device_id 时，这个数要跟着往上调（闸门在 cfg_check.h 守着下限） */
#define MOD_MQTT_TOPIC_MAX          128

/* 收到报文的内容缓冲(字节)：云端下发的 JSON 约 120 字节，128 够用。
 * 超出部分会被截断（但报文本身仍被完整收下、不会破坏后续报文边界） */
#define MOD_MQTT_MSG_MAX            128

/* ---------- 上云链（WiFi-MQTT）应用层参数 ---------- */
/* 上报周期(ms)：5 秒。DHT11 自己 2 秒采一次，5 秒是"比采集慢、又不是太慢"
 * 的平衡：太快会把 Broker 的免费额度刷光，太慢则云端"最新数据"看着像掉线。 */
#define APP_MQTT_PUBLISH_PERIOD_MS  5000

/* MQTT 心跳(秒)：30 秒。Broker 在 1.5 × keepalive（45 秒）收不到任何报文就
 * 断开连接。所以主循环的 50ms 节拍必须频繁调 Poll —— 心跳靠"没数据时发
 * PINGREQ"维持，而不是靠这个宏自己定时。 */
#define APP_MQTT_KEEPALIVE_SEC      30

/* JSON 载荷缓冲(字节)：{"services":[{"service_id":"Sensor","properties":
 * {"temperature":29.4,"humidity":35.0}}]} 约 90 字节，144 留出改服务名的余量 */
#define APP_MQTT_PAYLOAD_MAX        144

/* 离线缓存条数：断网期间数据进 RAM 队列，恢复后按序补传；队满丢最旧。
 * 16 条 × 5 秒 ≈ 80 秒的断网窗口——刚好覆盖"路由器重启"这段典型时长。
 * ★ 必须有界：不设上限的队列会把 128 KB RAM 吃光，这比丢数据严重得多。 */
#define APP_MQTT_OFFLINE_CACHE_MAX  16

/* ---------- 上云任务栈 / 优先级 ----------
 * 栈给 2048（四个任务里最大）：
 *   ① 报文拼接路径最深：app_mqtt → mod_mqtt → mod_wifi → net_esp8266 → net_uart
 *   ② 局部变量里有 JSON 载荷缓冲与命令报文缓冲（各一百多字节）
 *   ③ 解析路径还要过 strstr / memcmp 这类库函数
 * 优先级仍是 1：慢链路不该抢占本地控制——WiFi 掉线时灯还得照常闪。 */
#define APP_MQTT_TASK_STACK_SIZE    2048  /* 上云任务栈大小(字节) */
#define APP_MQTT_TASK_PRIO          1     /* 上云任务优先级：与另三个任务平级 */

#endif /* CFG_PARAM_H */
