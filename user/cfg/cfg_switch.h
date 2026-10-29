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
#define APP_LED_TASK_ENABLE (MOD_LED_ENABLE && 0)
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

/* ---------- DHT11 链（PG9 单总线 + TIM2 微秒底座；温度 → LED1，湿度 → LED2） ----------
 * 四层的 DHT11 链只有【两层】（app → perception），与 LED 链一致、与串口链不同：
 *   感知层管"40 个脉宽怎么变成 4 个字节"（协议翻译），
 *   应用层管"读到的数意味着什么"（周期采集 / 阈值联动 / 打印）。
 *   ★ 为什么中间不设平台层：这条链没有"协议位 / 数据格式化"这类中间知识，
 *     硬塞一层只会多出几十行纯转发代码。层的价值在于"有自己的知识"，
 *     不在于"看起来对齐" —— 串口有三层、DHT11 有两层，都是对的。
 *   开关顺序 = 依赖顺序（自下而上）：per_delay / per_dht11 → app_dht11 → app_link。 */
#define MOD_DHT11_ENABLE    1   /* 模块总开关：DHT11 链（置 0 = 温湿度与联动整体不参与编译） */
#define PER_DELAY_ENABLE    (MOD_DHT11_ENABLE && 1)   /* 感知层 per_delay.c —— TIM2 微秒计时底座 */
#define PER_DHT11_ENABLE    (MOD_DHT11_ENABLE && 1)   /* 感知层 per_dht11.c —— PG9 单总线协议翻译 */
#define APP_DHT11_ENABLE    (MOD_DHT11_ENABLE && 1)   /* 应用层 app_dht11.c —— 周期采集 + 重试 + 打印 */
#define APP_LINK_ENABLE     (MOD_DHT11_ENABLE && 1)   /* 应用层 app_link.c  —— 阈值联动：温度 → LED1、湿度 → LED2
                                                       * ★ 它横跨两条链：自己属于 DHT11 链，
                                                       *   却要借 LED 链的 per_led 去点灯，
                                                       *   所以 MOD_LED_ENABLE 也必须保持 1（cfg_check.h 有闸门看着） */

/* ---------- WiFi-MQTT 上云链（USART3 → ESP8266 → 华为云 IoTDA） ----------
 * 四层的上云链有【五跳】（app → platform ×2 → network ×2）：
 *   app_mqtt      报什么 / 多久报 / 命令怎么响应
 *   mod_mqtt      MQTT 报文编解码 + 报文 ID 会话（平台层第一跳）
 *   mod_wifi      连 AP / 建 TCP / 收发 —— 语义动作（平台层第二跳）
 *   net_esp8266   AT 文本协议 + +IPD 二进制分流
 *   net_uart      USART3 实例：DMA + IDLE + 环形缓冲（复用 1.3 的代码）
 * ★ 为什么平台层两跳、网络层两跳：这是四种【不同的知识】——
 *   AT 怎么发怎么等 / "连网建链"是什么意思 / 报文长什么样 / 报什么。
 *   每一层都能单独被替换（换模组只改 net_esp8266，换云平台只改 mod_mqtt
 *   的主题与 JSON），所以它们各自配单独成模块。
 * 开关顺序 = 依赖顺序（自下而上）：network → platform → app。 */
#define MOD_WIFI_ENABLE       1   /* 模块总开关：上云链（置 0 = 整条链不参与编译，本地功能照常） */
#define NET_UART3_ENABLE      (MOD_WIFI_ENABLE && 1)   /* ③ 网络层 net_uart.c 的 USART3 实例（字节流） */
#define NET_ESP8266_ENABLE    (MOD_WIFI_ENABLE && 1)   /* ③ 网络层 net_esp8266.c —— AT 会话 + +IPD 分流 */
#define MOD_WIFI_LINK_ENABLE  (MOD_WIFI_ENABLE && 1)   /* ② 平台层 mod_wifi.c  —— 五个语义动作（AT 到此为止） */
#define MOD_MQTT_ENABLE       (MOD_WIFI_ENABLE && 1)   /* ② 平台层 mod_mqtt.c  —— 四种报文 + 剩余长度编码 */
#define APP_MQTT_ENABLE       (MOD_WIFI_ENABLE && 1)   /* ① 应用层 app_mqtt.c  —— 连接分段 / 周期上报 / 命令响应 */

/* ---------- 任务集中创建（不属于某条链，单独一个开关） ---------- */
#define APP_TASK_ENABLE     1   /* 应用层 app_task.c —— 所有应用任务的"排班表" */

/* ---------- HAL 底层驱动（不属任何一条链，串口链的前置条件） ----------
 * 置 1 的两个前置（本工程已全部满足）：
 *   ① CubeMX 已配好 USART1 + DMA（Core 生成 usart.c / dma.c）
 *   ② Core/Inc/stm32f4xx_hal_conf.h 里已打开 HAL_UART_MODULE_ENABLED */
#define HAL_UART_MODULE_ENABLE   1   /* HAL UART 驱动编入（与 hal_conf 的 ENABLED 成对出现） */

/* ==========================================================================
 *  二、两级开关怎么用（九个真实场景，全部实测过能编能跑，照着改就行）
 *
 *  ★★ 先认清四条链的依赖关系，否则改开关必撞闸门 ★★
 *
 *     串口链   ──借 APP_BLINK_* 点灯──> LED 链
 *     DHT11 链 ─借 PER_LED_* 点灯────> LED 链    （APP_LINK 跨链）
 *     DHT11 链 ─借 MOD_UART1_* 打印──> 串口链
 *     上云链   ─借 APP_BLINK/PER_LED 点灯─> LED 链
 *     上云链   ─读 DHT11 快照（可选）──> DHT11 链
 *
 *    LED 链是四条里最底的一条：串口命令要经它的状态机点灯，DHT11 联动与
 *    云端命令要经它的感知层点灯。★ 所以"一键断 LED 链"在本工程【不成立】——
 *    MOD_LED_ENABLE 改 0 会同时撞上两条闸门：
 *        APP_UART1_ENABLE requires APP_BLINK_ENABLE
 *        APP_LINK_ENABLE  requires PER_LED_ENABLE
 *    真想不带 LED，得把串口链与联动一起断掉（等于换个项目了）。
 *    这不是设计缺陷，是"四条链共用两个合流点"的必然结果。
 *
 *  --------------------------------------------------------------------
 *    场景① 只要串口控灯、不要自动闪：
 *            APP_LED_ENABLE  末尾的 1 改 0     ← 只是不自动闪烁
 *            MOD_LED_ENABLE  保持 1            ← 下半段（APP_BLINK + PER_LED）必须在
 *          → 实测 FLASH 46232 B / RAM 24496 B（省 220 B）
 *          ★ 反过来"只留串口、不要 LED"【不成立】：命令最终要落到 APP_BLINK。
 *
 *    场景② 只留 DHT11 采集、不要 LED 联动（把 DHT11 当纯采集器用）：
 *            APP_LINK_ENABLE  末尾的 1 改 0
 *            MOD_DHT11_ENABLE 保持 1            ← 采集与打印照常
 *          → 实测 FLASH 46232 B / RAM 24496 B（省 220 B）
 *          串口仍每 2 秒打印一行温湿度，但两颗灯都不动。
 *          ★ 反过来"只要联动、不要采集"【不成立】：联动的输入就是采集结果。
 *
 *    场景③ 关整条 DHT11 链（回到 1.3 章节的功能集 + 上云）：
 *            MOD_DHT11_ENABLE  改 0     ← 四个子开关自动跟着变 0
 *          → 实测 FLASH 43300 B / RAM 24480 B（省 3152 B）
 *          ★ TIM2 不受本开关影响：它挂在 Core 里由 MX_TIM2_Init 启动，
 *            关掉整条链后计数器仍在跑（底座属于公共外设，不属于某个模块）。
 *
 *    场景④ 只留 DHT11 感知层（想省 Flash、或做最小验证）：
 *            APP_DHT11_ENABLE / APP_LINK_ENABLE  末尾的 1 改 0
 *            PER_DELAY_ENABLE / PER_DHT11_ENABLE 保持 1
 *          → 实测 FLASH 43300 B / RAM 24480 B —— 与场景③【完全相同】。
 *          ★ 这个"相同"不是巧合，而是 --gc-sections 的验收单：
 *            留下的两个感知层文件没有任何调用点，被 100% 回收。
 *
 *    场景⑤ 关整条上云链（回到 1.4 章节的功能集）：
 *            MOD_WIFI_ENABLE  改 0     ← 五个子开关自动跟着变 0
 *          → 实测 FLASH 35116 B / RAM 21456 B（省 11336 B FLASH / 3040 B RAM）
 *          ★ 这是本册最常用的一档：本地采集、联动、串口命令全都还在，
 *            只是不再上网。也是"取体积基准"时的对照档。
 *
 *    场景⑥ 只裁上云应用层（保留报文层与 AT 层）：
 *            APP_MQTT_ENABLE  末尾的 1 改 0
 *          → 实测 FLASH 35116 B / RAM 22232 B
 *            FLASH 与场景⑤ 相同：没人调用的 mod_mqtt / mod_wifi / net_esp8266
 *            被 gc 100% 回收（又一次验收"开关不删文件"能成立）。
 *            ★ 但 RAM 比场景⑤【多 776 B】—— 这是 UART3 实例的 DMA 缓冲与
 *              环形缓冲（512 + 268）。它俩被 net_uart 的实例表钉住：
 *              只要 NET_UART3_ENABLE 还是 1，表里就有一个指向它的表项，
 *              gc 不敢动。想真省 RAM，得关总闸（场景⑤）。
 *
 *    场景⑦ 彻底不带串口（会连带撞 DHT11 与上云链的闸门）：
 *            MOD_UART1_ENABLE 改 0 时，必须把 DHT11 链与上云链一起处理掉：
 *            要么 MOD_DHT11_ENABLE / MOD_WIFI_ENABLE 改 0，
 *            要么让它们各自的打印路径也退出编译。
 *          否则撞闸门：APP_DHT11_ENABLE requires MOD_UART1_ENABLE
 *          → 实测 FLASH 29168 B / RAM 20904 B（只剩 LED 链）
 *
 *    场景⑧ 裸机骨架（四条链全关，只剩 CubeMX + FreeRTOS 空转）：
 *            MOD_LED_ENABLE / MOD_UART1_ENABLE / MOD_DHT11_ENABLE / MOD_WIFI_ENABLE 都改 0
 *            HAL_UART_MODULE_ENABLE 改 0
 *            APP_TASK_ENABLE  改 0     ← 必须一起改，否则撞最后那条闸门
 *          → 实测 FLASH 28756 B / RAM 20896 B
 *          ★ 与场景⑦ 只差 412 B：剩下的都是 Core 层与 HAL 的固定开销，
 *            开关根本动不了它们（见下面最后一节）。
 *
 *    场景⑨ 关开关时 APP_TASK_ENABLE 要不要跟着改，看"还剩几个任务要建"：
 *          本例有四个任务——app_led_task（LED 链）、app_cmd_task（串口链）、
 *          app_dht11_task（DHT11 链）、app_mqtt_task（上云链）。
 *          cfg_check.h 里那条校验是"APP_TASK 至少要有一个任务可建"：
 *            - 只关一条链（另外三条还在）  → APP_TASK_ENABLE 保持 1
 *            - 四条链都关（裸机骨架）      → APP_TASK_ENABLE 必须改 0，否则 #error
 *
 *  ★ 这套闸门的目的只有一个：与其让编译过了、运行期莫名其妙，
 *    不如让人在改开关时就把依赖关系想清楚。每一条 #error 都是一次提醒。
 *
 *  ★★ 两级开关管不到的地方（取体积时必须知道，否则会误判）★★
 *    cfg 的开关只管辖 user/ 下的文件；Core/Src 里由 CubeMX 生成的
 *    usart.c / dma.c / tim.c / gpio.c 由 CMake 的 glob 一律参与编译，
 *    开关动不了它们。实测验证：即使把串口链与 HAL_UART_MODULE_ENABLE 全部关掉，
 *    MX_USART1_UART_Init、MX_USART3_UART_Init 与 HAL_UART_Init 依然出现在 .elf 里
 *    （真正被 gc 掉的只是没人调用的 HAL_UART_Transmit）。
 *    ★ 本册新增 USART3 之后，这一块的固定开销从 584 B 起（含 DMA1 与 NVIC），
 *      所以场景⑦/⑧ 的 28~29 KB 里有相当一部分是 Core 层与 HAL 的固定开销，
 *      不能与 1.4 章节记的 27048 B 直接比——那时的 Core 里还没有 USART3。
 * ========================================================================== */

#endif /* CFG_SWITCH_H */
