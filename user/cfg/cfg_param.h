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

/* ---------- 存储链·器件参数（EEPROM / SPI Flash，2.3 新增） ----------
 * ★ 板上实际器件（2026-10-29 实物确认 + 真机 JEDEC 佐证）：
 *   - EEPROM = AT24C02：字地址 1 字节，页 8 B，容量 256 B，器件地址 0xA0
 *   - FLASH  = W25Q128：JEDEC EF 40 18，16 MB，指令集与 W25Q64 完全一致
 * 型号仍保留上电自动识别（mod_eeprom_init 的两步探测）：
 *   一是教学价值 —— "字地址位宽搞错时写照样'成功'、读回全是错位"这个坑
 *   只有让代码自己识别才讲得透；二是换板不换码。
 * ★ 位宽搞错的现象极具迷惑性：地址帧照样 ACK、写照样返回成功，
 *   但数据整体错位（16 位器件会把第一个数据字节当成地址低字节吃掉），
 *   现象就是"上电写成功、周期读回 INVALID"——2.3 首跑日志即此。 */
#define AT24C02_PAGE_SIZE       8      /* 页写最大粒度(字节)：跨页会页内回绕、覆盖本页开头   */
#define AT24C02_WRITE_CYCLE_MS  5      /* 写周期上限(ms)：期间不应答，轮询 ACK 探到即写完    */
#define AT24C02_SIZE_BYTES      256    /* 总容量(字节)：2Kbit = 256 B，容量闸门在 cfg_check  */
#define AT24C256_PAGE_SIZE      64     /* 页写最大粒度(字节)：大容量器件的页更宽             */
#define AT24C256_SIZE_BYTES     32768U /* 总容量(字节)：256 Kbit = 32 KB                     */
#define AT24C256_WRITE_CYCLE_MS 10     /* 写周期上限(ms)：大容量器件按手册留到 10 ms          */

/* 型号探测用的两个"牺牲地址"（都远离配置块 0x0000..0x0013）：
 *   PROBE_ADDR8  = 0x00F0：按 1 字节字地址写读（判 8 位器件，落在 0xF0..0xF2）
 *   PROBE_ADDR16 = 0xFFF0：按 2 字节字地址写读（判 16 位器件，落在 32 KB 末尾）
 * ★ 为什么必须分两步、且以"8 位写读成不成立"为准：
 *   在 8 位器件上用 16 位地址【写】+【读】，两个动作会一致地错位（读回来
 *   照样拿到特征值），单看"写读一致"判不出位宽 —— 首版探测就是栽在这里，
 *   把 AT24C02 误判成 AT24C256。真正的判别点是反过来的那一刀：
 *   8 位写读在【16 位器件】上不成立 —— 它把 0xF0 当高位地址，写落到 0xF0xx，
 *   而 8 位读给不出低位地址、只能从内部指针接着读，读回来是 0xFF ≠ 特征值。 */
#define EEPROM_CHIP_PROBE_ADDR8     0x00F0u
#define EEPROM_CHIP_PROBE_ADDR16    0xFFF0u
#define EEPROM_CHIP_PROBE_PATTERN   0xA5u

#define W25Q64_SECTOR_SIZE      4096   /* 扇区(字节)：擦除单位——先擦后写                     */
#define W25Q64_PAGE_SIZE        256    /* 页(字节)：编程单位，一次编程绝不跨页               */
#define W25Q64_SIZE_BYTES       0x800000u /* W25Q64 总容量 8 MB                             */
#define W25Q128_SIZE_BYTES      0x1000000u /* W25Q128 总容量 16 MB（板上实测型号）           */
#define W25Q64_SECTOR_ERASE_TIMEOUT_MS  800  /* 扇区擦典型 45 ms；W25Q64/128 的 tSE 上限都是
                                               * 400 ms，取 800 是给"边缘超时"留一倍余量——
                                               * 板载实测为 W25Q128，按上限卡 400 会偶发 TIMEOUT */
#define W25Q64_PAGE_PROG_TIMEOUT_MS     10   /* 页编程典型 0.6 ms，上限留 10 ms               */

/* ---------- 存储链·应用层参数 ---------- */
#define APP_PARAM_CFG_OFFSET    0      /* 配置块在 EEPROM 里的起始地址                        */
#define APP_PARAM_CFG_SIZE      20     /* 配置块字节数：app_param.h 有 static_assert 钉死布局 */
#define APP_PARAM_READBACK_MS   3000   /* 配置读回验证周期：每 3 s 从芯片重读一遍（只读不写） */
#define APP_LOG_HDR_ADDR        0x000000u /* 日志头扇区：magic + 版本（4 KB）                 */
#define APP_LOG_AREA_ADDR       0x001000u /* 日志区起始：一条 8 B × 512 条 / 扇区             */
#define APP_LOGGER_PERIOD_MS    5000   /* 日志追加周期：与上云上报同拍，取 DHT11 快照         */
#define APP_STORAGE_TICK_MS     1000   /* 存储任务基础节拍：3 s / 5 s 都是它的整数倍          */

/* ---------- 存储链·总线层参数 ---------- */
#define NET_I2C_TIMEOUT_MS      50     /* I2C 单次事务超时(ms)：到点返回 TIMEOUT，绝不死等    */
#define NET_SPI_TIMEOUT_MS      10     /* SPI 单次交换超时(ms)：10.5 MHz 下 1 KB 也就 1 ms    */
#define NET_SPI_XFER_MAX        1024   /* 单个片选包络最大字节：读 256 B 数据 + 4 B 指令头    */

/* ---------- 存储任务栈 / 优先级 ----------
 * 栈给 2048：mod_flash 页编程一次要在栈上放 4 + 256 字节的发送缓冲，
 * 加上记录缓冲、读回比对缓冲与拼行打印，是六个业务任务里的第二大户。
 * ★ 单位仍是【字节】！ */
#define APP_STORAGE_TASK_STACK_SIZE 2048  /* 存储任务栈大小(字节) */
#define APP_STORAGE_TASK_PRIO   1      /* 与其余业务任务平级：慢速存储不抢占本地控制 */

/* ---------- 看门狗链（IWDG）硬件参数（2.4 新增） ----------
 * 超时公式：timeout_ms = (分频 × 重载) / LSI(≈32 kHz) × 1000 = 两数相乘再除 32。
 *   本例 64 × 1500 / 32 = 3000 ms —— "死亡倒计时"3 秒。
 * 为什么不用 CubeMX 默认的 4 / 4095：那只有约 0.5 秒，引导流程（串口链路
 *   初始化 + 任务起跑）就可能超 0.5 s，上电即陷入"复位 → 重启 → 再复位"循环。
 * ★ 两数都写在 cfg 而不是 per_iwdg.c：改超时只改这两行，不用去翻驱动。 */
#define PER_IWDG_DIV            64      /* LSI 预分频：只能是 4/8/16/32/64/128/256 之一 */
#define PER_IWDG_RELOAD         1500    /* 重载值：12 位递减计数器初值，1..4095 */
#define PER_IWDG_TIMEOUT_MS     ((PER_IWDG_DIV * PER_IWDG_RELOAD) / 32U)  /* 派生：实际超时(ms) */

/* ---------- 看门狗链（IWDG）应用层参数 ---------- */
/* 喂狗周期(ms)：必须远小于超时——500 ms 喂一次，留 2.5 s 余量。
 * 闸门守着"喂狗周期 × 2 ≤ 超时"这条底线（cfg_check.h），
 * 改大到 1500 ms 以上就会在编译期被拦下。 */
#define APP_WDT_FEED_PERIOD_MS  500

/* Alive 打印节奏：每喂这么多次打一行 "[WDT] Alive"。
 * 4 × 500 ms = 每 2 秒一行——既能证明"狗一直有人喂"，又不刷屏。 */
#define APP_WDT_ALIVE_EVERY     4

/* ---------- 看门狗任务栈 / 优先级 ----------
 * 栈 512（六个任务里最小）：链路最浅（app → per_iwdg → HAL 就到底），
 *   不用 printf，行缓冲只有几十字节。
 * ★ 单位仍是【字节】！512 = configMINIMAL_STACK_SIZE(128 字) × 4，是下限。 */
#define APP_WDT_TASK_STACK_SIZE 512     /* 看门狗任务栈大小(字节) */
#define APP_WDT_TASK_PRIO       1       /* 与其余任务平级：喂狗不该抢占采集与上云 */

#endif /* CFG_PARAM_H */
