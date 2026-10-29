# IOT_M4_L4 · STM32F407 四层架构「串口控灯 + DHT11 + WiFi-MQTT 上云例程」

![MCU](https://img.shields.io/badge/MCU-STM32F407ZETx-1E6BFF?style=flat-square)
![Architecture](https://img.shields.io/badge/Architecture-4%20Layer-12B8A6?style=flat-square)
![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS%20CMSIS--RTOS2-0B1F3A?style=flat-square)
![UART](https://img.shields.io/badge/UART-USART1%20%2F%20USART3-1FA055?style=flat-square)
![DHT11](https://img.shields.io/badge/DHT11-PG9%20单总线-EE8B1F?style=flat-square)
![MQTT](https://img.shields.io/badge/MQTT-ESP8266%20AT%20%E2%86%92%20华为云%20IoTDA-7A5AF8?style=flat-square)
![Build](https://img.shields.io/badge/Build-CMake%20%2B%20Ninja-FF7A29?style=flat-square)
![Toolchain](https://img.shields.io/badge/Toolchain-arm--none--eabi--gcc%2014.3.1-6B7A90?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-12B8A6?style=flat-square)

> 在**刻意做到最小**的 STM32 工程上再进三步：先接一条串口（1.3），再接一个单总线传感器（1.4），
> 最后把数据经 ESP8266 用 MQTT 送上华为云并接收云端命令（2.2），
> 把「四层怎么分、不定长帧怎么收、单总线时序怎么守、AT 会话与 MQTT 报文怎么落到四层里、配置怎么收口、三个控灯来源怎么不打架」讲透。
>
> 演进：纯闪烁 `v2.1b`（15 文件 / 1091 行）→ 加串口（21 文件 / 2267 行）→ 加 DHT11（29 文件 / 3677 行）
> → 加 WiFi-MQTT 上云（**37 文件 / 6626 行**，另有横切 `common/` 3 文件 / 127 行）。
> 本次上云章节新增 **8 个文件 / 2563 行**，并把 `net_uart` 从"单实例"改成**实例表**（一条代码管两个串口）。
>
> 最终形态：**四条链 · 两个合流点** —— LED 链（两跳）、串口链（三跳）、DHT11 链（两跳）、上云链（五跳）。
> 本工程是培训课件《1.3 项目实操-构架搭建与分层与 LED 灯控制》《1.4 项目实操-DHT11 传感器与 LED 灯联动》
> 与《2.2 项目实操-WiFi-MQTT 上云》的配套源码。

**适合谁看**：已经跑通四层闪灯、想接串口与传感器并对齐"感知 / 网络 / 平台 / 应用"四层口径的嵌入式新手；需要一套干净的"DMA + IDLE + 环形缓冲"、"单总线微秒时序"或"ESP8266 AT + MQTT 报文手搓"模板的开发者；要讲 1.3 / 1.4 / 2.2 章节的老师。

---

## 目录

- [这是什么](#这是什么)
- [硬件与环境](#硬件与环境)
- [3 分钟跑起来](#3-分钟跑起来)
- [串口命令怎么玩](#串口命令怎么玩)
- [目录结构](#目录结构)
- [改配置：只动 cfg 一个目录](#改配置只动-cfg-一个目录)
- [四层架构一览](#四层架构一览)
- [用户代码清单](#用户代码清单37-个文件--6626-行)
- [设计要点](#设计要点)
- [资源占用](#资源占用)
- [文档](#文档)
- [常见问题](#常见问题)
- [与 v2.1b 的差异](#与-v21b-的差异)
- [与五层参考工程的关系](#与五层参考工程的关系)
- [参考资料与致谢](#参考资料与致谢)
- [许可证](#许可证)

---

## 这是什么

`proL4_p1.3_blink_v2.1b` 是一个"会闪灯"的四层工程。本工程（`IOT_M4_L4`）在它上面**加了三件事：串口、DHT11、WiFi-MQTT 上云**。

```text
【1.3 串口章节】
串口助手敲：LED1 ON  ↵        →  板上 LED1 亮，回 "OK: LED1 ON"
串口助手敲：LED1 OFF ↵        →  板上 LED1 灭，回 "OK: LED1 OFF"
串口助手敲：BLINK    ↵        →  恢复 1 Hz 闪烁，回 "OK: BLINK"
串口助手敲：STATUS   ↵        →  回 "LED1=ON"（读真实引脚电平）
串口助手敲：HELP     ↵        →  回命令表

【1.4 DHT11 章节】板子自己每 2 秒吐一行：
[DHT11] Temp=28.0C | Hum=55.0%        →  温度 ≥31 ℃ 点亮 LED1，湿度 ≥60 % 点亮 LED2
[DHT11] Read FAILED (stage=1)         →  读不到就报"卡在哪一步"，且两颗灯都不动

【2.2 上云章节】同一块板子自己连 AP、连 Broker、每 5 秒上报一组：
[MQTT] MQTT connected (keepalive=30s)
[MQTT] subscribed: $oc/devices/<id>/sys/commands/#
[MQTT] Published: {"services":[{"service_id":"Sensor","properties":{"temperature":28.0,"humidity":55.0}}]}
云端下发命令 → [MQTT-CMD] LED_Control led=1 → 板上 LED1 亮 → 回 result_code=0
```

四层里的空目录，是分三步被填满的：

| 层 | v2.1b（纯闪烁） | +串口章节 | +DHT11 章节 | +上云章节（2.2） |
|---|---|---|---|---|
| ① app | `app_led` / `app_blink` / `app_task` | 新增 **`app_cmd`**（命令解析） | 新增 **`app_dht11`**（周期采集）+ **`app_link`**（阈值联动） | 新增 **`app_mqtt`**（连接分段 / 周期上报 / 命令响应） |
| ② platform | 空目录 | **`mod_uart1`**（统一入口） | **不设**（DHT11 没有中间知识） | 新增 **`mod_mqtt`**（四种报文 + 剩余长度编码）+ **`mod_wifi`**（连 AP / 建 TCP 的语义动作） |
| ③ network | 空目录 | **`net_uart`**（DMA + IDLE + 环形缓冲） | 无 | 新增 **`net_esp8266`**（AT 会话 + `+IPD` 分流）；`net_uart` 扩成**实例表**，一条代码带 USART1 与 USART3 |
| ④ perception | `per_led` / `per_gpio` | 扩出 `PER_LED_On/Off/IsOn`、`per_gpio_write/is_on` | 新增 **`per_delay`**（TIM2 微秒底座）+ **`per_dht11`**（单总线） | **零新增**（UART 引脚与 DMA 在 CubeMX 里配；AT 是文本帧处理，属网络层） |

读完这个工程，你应该能回答：

- 为什么串口要占三层（app → platform → network），而 LED 和 DHT11 都只占两层？
- 为什么上云链要占**五跳**（app → platform ×2 → network ×2）？哪四种互不重叠的知识？
- IDLE 中断里那 20 行判帧数学到底在算什么？为什么换成两个串口时**一行都没改**？
- 为什么"LED1 ON"不能直接点灯，非要过一遍闪烁状态机？
- 为什么 `app_taskCreate()` 和 `net_uart_rx_idle_handler()` 必须"永远存在"？
- 为什么把 `s_led_hw[].active_low` 这个字段留到串口章节才用上？
- **为什么 DHT11 的 40 位必须在关中断里读，而 18 ms 起始信号却刻意放在关中断之前？**
- **LED1 同时被闪烁、串口命令、温度联动、云端命令四处惦记，它们怎么不打架？**
- **为什么"一键断 LED 链"在接了 DHT11 之后就不成立了？**
- **为什么只关 `APP_MQTT_ENABLE` 时 FLASH 能省下来、RAM 却省不掉那 776 B？**

---

## 硬件与环境

| 项目 | 配置 |
|---|---|
| MCU | STM32F407ZETx（Cortex-M4F，168 MHz） |
| 用户 LED | **LED1 = PG14**（温度指示）、**LED2 = PG13**（湿度指示），均为**低电平点亮**（写 0 亮 / 写 1 灭） |
| 调试串口 | **USART1：PA9 = TX，PA10 = RX**，**115200-8-N-1** |
| 接收方式 | **DMA2_Stream2 循环模式** + USART **IDLE 空闲中断判帧** + 环形缓冲 |
| 上云串口 | **USART3：PB10 = TX，PB11 = RX**，**115200-8-N-1**（接 ESP8266 的 RX/TX，**交叉接**） |
| 上云接收 | **DMA1_Stream1 / Channel 4 循环模式** + USART3 IDLE 判帧 + 两段缓冲（AT 文本 512 B + `+IPD` 载荷 256 B 环形） |
| WiFi 模组 | **ESP8266（ESP-01S / ESP-12F 均可），AT 固件**；**只支持 2.4 GHz AP**，5 GHz 连不上 |
| 云平台 | **华为云 IoTDA**，MQTT **明文 1883**（AT 固件不做 TLS 握手，不用 8883） |
| 温湿度传感器 | **DHT11 单总线，DATA = PG9**（上电必须 High）；模块 VCC 3.3 V；裸传感器须在 DATA-VCC 间外接 4.7k~10k 上拉 |
| 微秒时基 | **TIM2**，APB1 定时器时钟 84 MHz → `Prescaler = 83` → **1 计数 = 1 µs**，只 `Base_Start` 不进中断 |
| 系统时钟 | HSE 8 MHz → PLL（M=8 / N=336 / P=÷2）→ SYSCLK **168 MHz**，AHB 168 / APB1 42 / APB2 84 MHz |
| HAL 时基 | **TIM6**（供 `HAL_Delay` 等使用） |
| RTOS 节拍 | **SysTick**，`configTICK_RATE_HZ = 1000` → 1 tick = 1 ms |
| RTOS | FreeRTOS 10.x + CMSIS-RTOS2（`osThreadNew` / `osDelay`），动态创建，堆 15360 B（四个任务栈 512+1024+768+2048） |
| 中断优先级 | USART1 / USART3 / DMA1_Stream1 抢占优先级均为 **5**（与 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 相等，本工程统一纪律：ISR 内不调 RTOS API） |
| 调试口 | SWD（ST-Link / J-Link 均可） |

> ⚠ **APB1 定时器时钟是 84 MHz，不是 42 MHz**：APB1 预分频 ≠1 时定时器时钟 ×2。
> 所以 TIM2 的 PSC 必须是 **83**（84 MHz / 84 = 1 MHz）。填 41 会让所有 µs 计时减半，DHT11 必然读不到数。
> 三个定时器的分工：**SysTick = FreeRTOS 独占、TIM6 = HAL 时基、TIM2 = 本工程微秒底座**；
> DWT 的 `CYCCNT` 刻意不用（调试器下载后它常常不计数）。

**构建/开发环境**（本工程实际验证过的组合）：

| 工具 | 版本 | 说明 |
|---|---|---|
| arm-none-eabi-gcc | **14.3.1**（GNU Tools for STM32 14.3.rel1） | 交叉编译器 |
| CMake | **4.4.0** | 构建系统 |
| Ninja | **1.13.2** | 生成器 |
| VS Code + STM32Cube 扩展 | 推荐 | 工程自带 `.vscode/tasks.json`（configure / build / clean / rebuild / flash） |
| 串口助手 | 任意（sscom / Xshell / MobaXterm…） | 115200-8-N-1，**发送新行**（`\n`） |

> 工具链可一次装齐：下载 ST 官方的 **STM32CubeCLT**（含 gcc / cmake / ninja）。
> 注意：工程文件叫 `IOT_M4_L4.ioc`（与目录同名），里面已配好 USART1 + DMA + NVIC，用 CubeMX 打开可直接重新生成代码。

---

## 3 分钟跑起来

### 1. 编译

```bash
cmake --preset Debug
cmake --build build/Debug
```

编译产物（文件名取自工程文件夹名）：

```text
build/Debug/IOT_M4_L4.elf   可执行文件（调试用）
build/Debug/IOT_M4_L4.hex   Intel HEX（烧录用）
```

预期输出（本次实测，**58 个构建目标，0 warning / 0 error**）：

```text
Memory region         Used Size  Region Size  %age Used
             RAM:       24496 B       128 KB     18.69%
          CCMRAM:           0 B        64 KB      0.00%
           FLASH:       46452 B       512 KB      8.86%
```

### 2. 烧录

VS Code 里跑任务 `flash (ST-Link)` 或 `flash (J-Link)`；命令行等价写法：

```bash
STM32_Programmer_CLI -c port=SWD -w build/Debug/IOT_M4_L4.hex -v -rst
```

### 3. 连串口、看现象

1. USB 转串口接到 **PA9(TX) / PA10(RX)**，与板子**共地**；
2. 串口助手选对应 COM 口，**115200 / 8 / N / 1**，打开串口；
3. 板子重新上电（或按复位）。

预期现象：

```text
① 上电瞬间：LED1 快闪 3 次          ← 串口任务已启动的自检信号（旧固件没有这段）
② 之后：LED1 以 1 Hz 稳定闪烁        ← 亮 500ms / 灭 500ms
③ 串口助手收到开机横幅：
     === UART1 LED Control (4-Layer) ===
     Commands (end with Enter, case-insensitive):
       LED1 ON / LED1 OFF   LED1(PG14) on/off
       BLINK                blink 1Hz (half period from cfg)
       STATUS               query real LED state
       HELP                 show this help
     >
④ 紧接着 DHT11 的三行上电信息：
     === IOT_M4_L4 DHT11 Temp/Humi (PG9) ===
     [DHT11] ST: TIM2=1000us loop=1,0 bus=1 resp=289     ← 自检四字段，逐个核对
     [DHT11] Link: Temp>=31C -> LED1 | Hum>=60% -> LED2   ← 生效的阈值（取自 cfg_param.h）
⑤ 稳定约 1 秒后，每 2 秒一行：
     [DHT11] Temp=28.0C | Hum=55.0%
⑥ 敲字符有回显                        ← 回显有 = 接收链路通
⑦ 上云任务紧接着连网（SSID / 密码 / 设备三元组填对时）：
     === IOT_M4_L4 WiFi-MQTT Cloud (USART3) ===
     [MQTT] AT test...  →  [MQTT] AT OK
     [MQTT] WiFi join AP "your-ssid" ...  →  [MQTT] WiFi connected
     [MQTT] TCP connecting xxx.iot-mqtts.cn:1883 ...  →  [MQTT] TCP connected
     [MQTT] sending MQTT CONNECT ...  →  [MQTT] MQTT connected (keepalive=30s)
     [MQTT] subscribed: $oc/devices/<id>/sys/commands/#
     [MQTT] Published: {"services":[{"service_id":"Sensor","properties":{"temperature":28.0,"humidity":55.0}}]
```

> 上云是**可选**的：`APP_MQTT_WIFI_SSID` 等 8 个宏在 `user/app/Inc/app_mqtt.h` 顶部，
> 不填就只会看到 `[MQTT] retry #1 in 2s` 一路退避——**本地三条链照常工作**，不会因为连不上云而卡住板子。

两个**动手就能验证**的动作：

- **手指捏住 DHT11**（或对它哈气）→ 温度升到 ≥31 ℃ 时 **LED1 亮**；
- **隔着 5 cm 对传感器哈气** → 湿度升到 ≥60 % 时 **LED2 亮**；
- **拔掉 DATA 线** → 每 2 秒报 `Read FAILED (stage=1)`，且**两颗灯保持不乱动**；
- **在华为云控制台下发 `LED_Control {led:1}`** → 板上 LED1 亮，串口吐 `[MQTT-CMD] LED_Control led=1` + `[MQTT-CMD] response sent`。

灯不亮、串口没反应、或 DHT11 读不到，先按[常见问题](#常见问题)排查。

---

## 串口命令怎么玩

| 命令 | 作用 | 回应 |
|---|---|---|
| `LED1 ON` | 点亮 LED1（并让它退出闪烁） | `OK: LED1 ON` |
| `LED1 OFF` | 熄灭 LED1（并让它退出闪烁） | `OK: LED1 OFF` |
| `BLINK` | 恢复 1 Hz 闪烁（半周期取自 cfg） | `OK: BLINK` |
| `STATUS` | 查询 LED1 真实电平 | `LED1=ON` / `LED1=OFF` |
| `HELP` | 打印命令表 | 见上 |
| 其它 | 未知命令原样回显在方括号里 | `ERR: unknown cmd [XXX]` |

- **大小写不限**（`led1 on` == `LED1 ON`），命令以**回车**结束；
- 不发回车也能用：串口安静后会自动结算上一行（兼容"点一次发一次"的助手）；
- 可打印字符会**回显**，这是判断"收链路通不通"最快的信号；
- 输出全部是**英文**：源文件是 UTF-8，中文发到 GBK 编码的串口助手上会变乱码。

---

## 目录结构

```text
IOT_M4_L4/
├── user/                       ★ 用户代码：四层架构（37 文件 / 6626 行）
│   ├── app/Inc, Src/           ① 应用层    app_led（闪烁业务）/ app_blink（状态机）
│   │                                      app_cmd（命令解析）/ app_task（任务创建）
│   │                                      app_dht11（周期采集）/ app_link（阈值联动）
│   │                                      app_mqtt（连接分段 / 周期上报 / 命令响应）
│   ├── platform/Inc, Src/      ② 平台层    mod_uart1（串口1 统一入口：MOD_UART1_*）
│   │                                      mod_mqtt（四种报文 + 剩余长度编码 + 报文 ID 会话）
│   │                                      mod_wifi（连 AP / 建 TCP 的语义动作，AT 到此为止）
│   ├── network/Inc, Src/       ③ 网络层    net_uart（DMA 循环 + IDLE 判帧 + 环形缓冲，★ 实例表管 2 个串口）
│   │                                      net_esp8266（AT 文本会话 + +IPD 二进制分流）
│   ├── perception/Inc, Src/    ④ 感知层    per_led（门卫）/ per_gpio（s_led_hw[] 表 + 唯一碰 HAL）
│   │                                      per_delay（TIM2 微秒）/ per_dht11（单总线协议翻译）
│   │                                      ★ 上云章节本层零新增（UART 引脚在 CubeMX 配，AT 属文本帧处理）
│   │
│   ├── cfg/                    ※ 横切·配置（纯头文件，无 .c；5 文件 / 750 行）
│   │   ├── module_cfg.h            ◆ 总入口：全工程唯一配置入口，只做按序包含
│   │   ├── cfg_switch.h            两级开关 22 个 + 九个裁剪场景说明
│   │   ├── cfg_print.h             打印开关（业务回复直出，总闸保持 0）
│   │   ├── cfg_param.h             参数 32 个：节拍/半周期/串口与上云缓冲/任务栈/优先级/阈值
│   │   └── cfg_check.h             依赖校验 18 条 + 参数体检 21 条（共 39 道 #error）
│   └── common/                 ※ 横切·工具（3 文件 / 127 行）
│       ├── ringbuf.h / ringbuf.c       无锁环形缓冲（ISR 写 / 任务读，容量 256）
│       └── log.h                       日志级别（配合 CFG_LOG_LEVEL）
│
├── Core/                       CubeMX 生成的初始化与启动代码
│   ├── Src/usart.c / dma.c         ★ USART1 + DMA2_Stream2（1.3）、USART3 + DMA1_Stream1（2.2）
│   ├── Src/freertos.c              ★ 唯一被手改的业务挂载点（USER CODE 段 2 行）
│   ├── Src/stm32f4xx_it.c          ★ USART1/USART3 的 IDLE 判帧入口 + DMA1_Stream1 中断
│   ├── Src/tim.c                   ★ DHT11 章节：TIM2 的 PSC 改 83 + 启动
│   ├── Src/gpio.c                  PG9(DHT11) / PG13(LED2) / PG14(LED1) 三引脚合并配置
│   ├── Src/main.c                  时钟、外设、GPIO；三章共新增 3 行 MX 调用
│   └── Inc/stm32f4xx_hal_conf.h    串口章节打开 HAL_UART_MODULE_ENABLED
│
├── Drivers/                    STM32F4 HAL 库 + CMSIS（串口章节补入 stm32f4xx_hal_uart.c/.h）
├── Middlewares/                FreeRTOS 内核（CubeMX 生成，未改动）
├── cmake/stm32cubemx/          CubeMX 生成的 CMake 目标（登记 usart.c / dma.c / hal_uart.c）
│
├── docs/                       ★ 中文文档（Markdown + 同名单文件网页版）+ 配套课件
│   ├── README.md                   文档索引与按角色的阅读路径
│   ├── architecture/               四层架构解析（最简 LED 版 / 串口控灯版 / DHT11 版 / 上云版）
│   ├── software-engineering/       配置体系与两级开关 / 串口三件套 / 闪烁状态机与契约式注释
│   ├── tutorials/                  t01 四层 LED、t02 接串口、t03 接 DHT11、t04 接 WiFi-MQTT 上云
│   ├── reference/                  引脚与符号速查表
│   ├── test/                       LED 链 / 串口链 / 上板操作卡 / DHT11 链 / 上云链
│   ├── teaching/                  配套课件 pptx（本工程对应 1.3 + 1.4 + 2.2）
│   ├── build_docs.py              Markdown → HTML 渲染器
│   ├── check_stats.py             规模数字对账
│   └── _tools/                     取数脚本：体积 / 链接产物 / 函数计数
│
├── CMakeLists.txt              构建入口（file(GLOB) 分四层收编 user，新增文件无需改脚本）
├── CMakePresets.json            Debug / Release 预设
├── STM32F407xx_FLASH.ld        链接脚本
├── startup_stm32f407xx.s       启动文件
├── IOT_M4_L4.ioc               CubeMX 工程文件（PG9/13/14 / TIM2 / TIM6 / CMSIS-RTOS2 / USART1+DMA / USART3+DMA）
└── .vscode/tasks.json          VS Code 任务：configure / build / clean / rebuild / flash
```

---

## 改配置：只动 cfg 一个目录

所有可调项都收口在 `user/cfg/` 下，业务代码**一行都不用动**：

| 想改什么 | 改哪个宏 | 在哪个文件 | 当前值 |
|---|---|---|---|
| 闪烁快慢 | `APP_LED1_BLINK_MS`（半周期，ms） | `cfg_param.h` | `500`（= 1 Hz） |
| 状态机节拍 | `APP_BLINK_TICK_MS`（ms） | `cfg_param.h` | `1` |
| LED 任务栈 | `APP_LED_TASK_STACK_SIZE`（**字节**） | `cfg_param.h` | `512` |
| LED 任务优先级 | `APP_LED_TASK_PRIO`（1~55） | `cfg_param.h` | `1` |
| DMA 接收缓冲 | `NET_UART1_RX_DMA_BUF_SIZE`（字节） | `cfg_param.h` | `256`（≈ 22 ms 数据量） |
| 命令最大长度 | `APP_UART1_LINE_MAX`（字节） | `cfg_param.h` | `64` |
| 串口任务栈 | `APP_UART1_TASK_STACK_SIZE`（**字节**） | `cfg_param.h` | `1024` |
| 串口任务优先级 | `APP_UART1_TASK_PRIO`（1~55） | `cfg_param.h` | `1` |
| 环形缓冲容量 | `CFG_RINGBUF_SIZE`（**必须 2 的幂**） | `cfg_param.h` | `256` |
| **温度阈值**（点亮 LED1） | `APP_LINK_TEMP_THRESHOLD`（℃） | `cfg_param.h` | `31` |
| **湿度阈值**（点亮 LED2） | `APP_LINK_HUMI_THRESHOLD`（%） | `cfg_param.h` | `60` |
| **采集周期** | `APP_DHT11_PERIOD_MS`（ms，≥1000） | `cfg_param.h` | `2000` |
| 读取失败重试次数 | `APP_DHT11_RETRY_COUNT` | `cfg_param.h` | `3` |
| DHT11 任务栈 | `APP_DHT11_TASK_STACK_SIZE`（**字节**） | `cfg_param.h` | `768` |
| **WiFi 名字 / 密码** | `APP_MQTT_WIFI_SSID` / `APP_MQTT_WIFI_PWD` | `app/Inc/app_mqtt.h` | `"your-ssid"` / `"your-password"` |
| **Broker 地址 / 端口** | `APP_MQTT_BROKER_HOST` / `APP_MQTT_BROKER_PORT` | `app/Inc/app_mqtt.h` | 华为云 `*.iot-mqtts.cn` / `1883` |
| **设备三元组** | `APP_MQTT_DEVICE_ID` / `CLIENT_ID` / `USERNAME` / `AUTH_PASSWORD` | `app/Inc/app_mqtt.h` | 从控制台复制 |
| **物模型 service_id** | `APP_MQTT_SERVICE_ID` | `app/Inc/app_mqtt.h` | `"Sensor"`（必须与控制台逐字一致） |
| **上报周期** | `APP_MQTT_PUBLISH_PERIOD_MS`（ms，≥1000） | `cfg_param.h` | `5000` |
| **心跳间隔** | `APP_MQTT_KEEPALIVE_SEC`（s，≥10） | `cfg_param.h` | `30` |
| 离线缓存条数 | `APP_MQTT_OFFLINE_CACHE_MAX` | `cfg_param.h` | `16`（× 5 s ≈ 80 秒窗口） |
| 上云任务栈 | `APP_MQTT_TASK_STACK_SIZE`（**字节**） | `cfg_param.h` | `2048` |
| 整条上云链砍掉 | `MOD_WIFI_ENABLE` 改 0 | `cfg_switch.h` | `1` |
| 整条 DHT11 链砍掉 | `MOD_DHT11_ENABLE` 改 0 | `cfg_switch.h` | `1` |
| **只要温湿度读数、不要联动** | `APP_LINK_ENABLE` 末尾 1 改 0 | `cfg_switch.h` | `(MOD_DHT11_ENABLE && 1)` |
| 整条串口链砍掉 | `MOD_UART1_ENABLE` 改 0（**必须连带处理 DHT11 链**） | `cfg_switch.h` | `1` |
| 整条 LED 链砍掉 | ⚠ **不成立**，见下方坑 2 | `cfg_switch.h` | —— |
| 只裁某一层 | 该层子开关末尾的 `1` 改 `0` | `cfg_switch.h` | `(MOD_XXX_ENABLE && 1)` |

改完**必须重新编译**才生效（这是编译期配置，不是在线可调参数）。

> ⚠️ **六个容易踩的坑**
> 1. `*_TASK_STACK_SIZE` 的单位是**字节**，不是"字"。填 256 以为省 RAM，实际只分到 256 字节 → 栈溢出会**静默写坏 FreeRTOS 堆**（不报错、不进 HardFault），现象是"跑几行后整机没反应"。
> 2. **"一键断 LED 链"在本工程已不成立**：`MOD_LED_ENABLE` 改 0 会同时撞上 `APP_UART1 requires APP_BLINK` 与 `APP_LINK requires PER_LED` 两条闸门——串口命令、DHT11 联动、云端命令都要借 LED 链点灯。想让灯不自动闪，应关 `APP_LED_ENABLE`（保留状态机），让串口/云端接管。
> 3. **关串口链要连带处理 DHT11 链与上云链**：两者都要用串口打印，否则撞 `APP_DHT11 requires MOD_UART1` / `NET_UART3 requires HAL_UART_MODULE`。
> 4. **关到"没任务可建"会被拦**：四个任务对应的开关都关、而 `APP_TASK_ENABLE` 仍为 1 → `#error`。
> 5. **只关 `APP_MQTT_ENABLE` 省不下那 776 B RAM**：UART3 的 DMA 缓冲（512 B）与环形缓冲（268 B）被 `net_uart` 的实例表项钉住，`--gc-sections` 不敢动。想真省 RAM 必须关总闸 `MOD_WIFI_ENABLE`。
> 6. **物模型对不上 = 静默丢弃**：`service_id` 或属性名与控制台"功能定义"不一致时，华为云**丢弃且不报错**——设备在线、串口打印 `Published`、云端"最新数据"永远空。

---

## 四层架构一览

命令**只能从上往下传**，下层永远不知道上层存在。

| 层 | 目录 | 文件 | 职责 | 关键符号 |
|---|---|---|---|---|
| ① app | `user/app/` | `app_led.c` / `app_blink.c` / `app_cmd.c` / `app_dht11.c` / `app_link.c` / `app_mqtt.c` / `app_task.c` | 业务："闪多快"；闪烁状态机；"一行命令 → 一个动作"；"读到的数意味着什么"；"报什么、多久报、命令怎么响应"；任务集中创建 | `app_led_init` / `APP_BLINK_process` / `APP_BLINK_On|Off|IsOn` / `app_cmd_task` / `app_dht11_task` / `APP_LINK_ApplyTemp|ApplyHum` / `app_mqtt_task` / `app_taskCreate` |
| ② platform | `user/platform/` | `mod_uart1.c` / `mod_wifi.c` / `mod_mqtt.c` | 统一入口与协议位：串口的收/发名字；"连 AP / 建 TCP"的五个语义动作；MQTT 四种报文与剩余长度编码 | `MOD_UART1_Init` / `MOD_WIFI_JoinAp` / `MOD_WIFI_OpenTcp` / `MOD_MQTT_Connect` / `MOD_MQTT_Publish` / `MOD_MQTT_Poll` |
| ③ network | `user/network/` | `net_uart.c` / `net_esp8266.c` | 字节流：DMA 循环接收 + IDLE 判帧 + 环形缓冲（**实例表管 USART1 与 USART3**）；AT 文本会话 + `+IPD` 二进制分流 | `net_uart_init(id)` / `net_uart_rx_idle_handler(id)` / `net_esp8266_join_ap` / `net_esp8266_send` / `net_esp8266_recv` |
| ④ perception | `user/perception/` | `per_led.c` / `per_gpio.c` / `per_delay.c` / `per_dht11.c` | 门卫（编号→动作）+ 硬件描述表 `s_led_hw[]` + GPIO 读写 + 微秒时基 + 单总线协议翻译（**上云章节零新增**） | `PER_LED_On|Off|IsOn|Toggle` / `per_gpio_write` / `per_gpio_is_on` / `per_delay_us` / `PER_DHT11_Read` |
| ※ cfg | `user/cfg/` | `module_cfg.h` + 4 子文件 | 配置：开关与参数；编译期校验（只被读，不被调） | `MOD_LED_ENABLE` / `MOD_WIFI_ENABLE` / `APP_LINK_TEMP_THRESHOLD` / `APP_MQTT_PUBLISH_PERIOD_MS` … |
| ※ common | `user/common/` | `ringbuf.*` / `log.h` | 横切工具：不占层、不依赖任何层 | `rb_init` / `rb_write` / `rb_read` |

**四条依赖红线**：

```text
① 只许 上层 → 下层 单向调用，不许回头、不许横向 include
② 例外：应用层可直达感知层（本地闭环，如按键点灯）
③ cfg/ 只被读、不被调用；common/ 被各层共用但自身不依赖任何层
④ HAL 只出现在感知层与网络层（网络层受 HAL_UART_MODULE_ENABLE 看管）
```

**★ 命名约定**：统一为 `<前缀>_<模块>_<动作>`，前缀标"在哪一层、是对内还是对外"：

- **小写蛇形** = 驱动实现 / 业务流程：`per_gpio_write`、`app_led_init`、`app_taskCreate`、`net_uart_read`
- **大写前缀** = 对外的"门卫 / 状态机 / 入口"接口：`PER_LED_On`、`APP_BLINK_On`、`MOD_UART1_SendString`

每层有**自己独立的 LED 编号枚举**（`APP_BLINK_LED_t` / `PER_LED_t` / `PER_GPIO_LED_t`），末位都是哨兵量 `XXX_LED_COUNT`——既作数组长度，也作越界判断。
注意三套的 COUNT **并不相同**：感知层两套 = **2**（管 LED1 与 LED2），`APP_BLINK_LED_t` = **1**（LED2 不归闪烁状态机管，它只被湿度联动直接控制）。

### 四条链，两个合流点

```text
LED 链（两跳）    app_led   → APP_BLINK_*  → PER_LED_* → per_gpio_* → HAL
串口链（三跳）    app_cmd   → MOD_UART1_*  → net_uart(NET_UART_DBG) → HAL
DHT11 链（两跳）  app_dht11 → PER_DHT11_*  → per_delay_* → HAL
上云链（五跳）    app_mqtt  → MOD_MQTT_*   → MOD_WIFI_*
                            → net_esp8266_* → net_uart(NET_UART_WIFI, USART3) → HAL

  ★ 合流点① APP_BLINK_*（同在 app 层）
     串口命令与自动闪烁在此汇合：命令先让灯"退出闪烁"再点灯 —— 手动与闪烁绝不抢同一颗灯。
     （云端命令也走这里：app_mqtt → APP_BLINK_On，而不是直接写引脚——
       否则闪烁状态机会在半周期后把它翻回来，现象是"云端下发成功，灯闪一下又灭了"。）
  ★ 合流点② per_led（感知层）
     温度联动、自动闪烁、云端命令（LED2）在此汇合：联动用【边沿触发】——只在跨越阈值那一帧写灯，
     平时把灯让给 1 Hz 闪烁。于是"闪烁"与"温度到了就亮"能同时成立。

  ★ 依赖图：串口链 → LED 链（借 APP_BLINK_* 点灯）
             DHT11 链 → LED 链（借 PER_LED_* 点灯，APP_LINK 跨链）
             DHT11 链 → 串口链（借 MOD_UART1_* 打印）
             上云链  → LED 链（借 APP_BLINK/PER_LED 点灯）
             上云链  → DHT11 链（只读快照 APP_DHT11_GetSnapshot，可选）
    所以 LED 链是四条里最底的一条 —— "一键断 LED 链"在本工程已不成立。
```

> **为什么上云链要五跳？** 因为它有四种**互不重叠的知识**，任何一种都不能塞进别处：
> AT 指令怎么发、怎么等（`net_esp8266`）／"连网建链"是什么意思（`mod_wifi`）／
> MQTT 报文长什么样（`mod_mqtt`）／报什么、多久报（`app_mqtt`）。
> 换模组只改 `net_esp8266`，换云平台只改 `mod_mqtt` 的主题与 JSON —— 这就是五跳的回报。
>
> **为什么感知层这次一个字都没加？** UART 的引脚与 DMA 在 CubeMX 里配（那是 `Core/` 的事），
> AT 是**文本帧处理**不是引脚时序 —— 它属于网络层。四层不是每层都必须加东西。

### 串口命令的 14 跳旅程

```text
PC 发 "LED1 ON\n"
 → USART1 总线安静 → IDLE 中断
 → Core/Src/stm32f4xx_it.c  USART1_IRQHandler()
      ├─ __HAL_UART_CLEAR_IDLEFLAG()      先清标志（顺序是命门）
      └─ net_uart_rx_idle_handler()       【③network】判帧 + 搬进 ringbuf
 → app_cmd_task          MOD_UART1_Read() 【②platform → ③network】取字节
 → 攒到 '\n' → cmd_exec()                【①app】解析（去回车 → 转大写 → 匹配）
 → APP_BLINK_On(APP_BLINK_LED1)          【①app】清半周期 → 退出闪烁
 → PER_LED_On(PER_LED1)                  【④perception】门卫：编号 → 动作
 → per_gpio_write(PER_GPIO_LED1, 1)      【④perception】查 s_led_hw[]，active_low 翻译成电平
 → HAL_GPIO_WritePin(GPIOG, PIN_14, RESET)  【HAL】低电平点亮
 → MOD_UART1_SendString("OK: LED1 ON\r\n")  【①app → ②platform → ③network】回一句
```

### 云命令的 16 跳旅程（2.2 章节）

```text
华为云控制台下发 {"service_id":"Sensor","command_name":"LED_Control","paras":{"led":1}}
 → Broker 转成 PUBLISH 发到 $oc/devices/<id>/sys/commands/request_id=<rid>
 → ESP8266 收到 TCP 包，先吐一行 "+IPD,<len>:" 再跟二进制报文
 → USART3 总线安静 → IDLE 中断
 → Core/Src/stm32f4xx_it.c  USART3_IRQHandler()
      └─ net_uart_rx_idle_handler(NET_UART_WIFI)   【③network】按实例表判帧 + 搬进 ringbuf
 → net_esp8266_pump_rx()  扫 "+IPD," → 取 data_len → 数据没到齐就退出下次再拼 → 整段搬进载荷环
 → MOD_WIFI_Recv()                              【②platform】语义动作：收一帧
 → MOD_MQTT_Poll()        【②platform】读固定头 → 解剩余长度 → 读正文 → 是 PUBLISH
      ├─ 先回 PUBACK（不回 Broker 会一直重发）
      └─ 填出 topic / payload 结构体
 → app_mqtt_handle_cmd()                        【①app】从 topic 取 request_id、从 JSON 取 led
 → APP_BLINK_On(APP_BLINK_LED1)                 【①app】先退出闪烁（否则被状态机翻回来）
 → PER_LED_On(PER_LED1) → per_gpio_write → HAL_GPIO_WritePin(GPIOG, PIN_14, RESET)
 → 回响应：MOD_MQTT_Publish(".../commands/response/request_id=<rid>", "{\"result_code\":0,...}")
 → MOD_WIFI_Send → net_esp8266_send → AT+CIPSEND → net_uart_send(NET_UART_WIFI) → HAL_UART_Transmit
```

### 启动流程（从复位到四条链都跑起来）

```text
复位 → startup_stm32f407xx.s → main()
  → HAL_Init()（HAL 时基切到 TIM6）
  → SystemClock_Config()（168 MHz）
  → MX_GPIO_Init() / MX_DMA_Init() / MX_USART1_UART_Init() / MX_USART3_UART_Init()
  → osKernelInitialize() → MX_FREERTOS_Init()
       └─ app_taskCreate()        ★ 唯一挂载点，就这一行
            ├─ osThreadNew(app_led_task,   …)   灯：初始化引脚 + 设 1Hz
            ├─ osThreadNew(app_cmd_task,   …)   串口：拉起 DMA 接收 + 快闪 3 次自检
            ├─ osThreadNew(app_dht11_task, …)   传感器：自检 → 稳定 1s → 每 2s 采集
            └─ osThreadNew(app_mqtt_task,  …)   上云：AT 测活 → 连 AP → 建 TCP → MQTT → 订阅
  → osKernelStart()  →  调度器接管  →  四个任务各自跑起来
```

---

## 用户代码清单（37 个文件 / 6626 行）

> 行数含契约式注释；去掉注释后的代码量约为其三分之一。
> 口径：`user/` 下除 `common/` 外的 .c/.h，由 `docs/check_stats.py` 逐行核对。

| 层 | 文件 | 行数 | 说明 |
|---|---|---|---|
| ※ cfg | `module_cfg.h` | 66 | 总入口，只做按序包含 + 四条链的链路表 |
| ※ cfg | `cfg_switch.h` | 195 | 两级开关（22 个）+ 九个裁剪场景说明 |
| ※ cfg | `cfg_print.h` | 45 | 打印总闸（业务回复直出，保持 0） |
| ※ cfg | `cfg_param.h` | 168 | 32 个参数 + 栈单位与缓冲大小说明 |
| ※ cfg | `cfg_check.h` | 276 | 18 条依赖校验 + 21 条参数体检 |
| ④ perception | `Inc/per_gpio.h` | 108 | 接口契约：枚举（2 颗灯）+ 四个动作声明 |
| ④ perception | `Src/per_gpio.c` | 178 | `s_led_hw[]` 表（2 项）+ init / write / is_on / toggle |
| ④ perception | `Inc/per_led.h` | 111 | 接口契约（门卫，2 颗灯） |
| ④ perception | `Src/per_led.c` | 119 | 编号翻译 + 越界拦截的薄转发 |
| ④ perception | `Inc/per_delay.h` | 75 | 微秒底座接口（`getTickUs` / `us` / `ms`） |
| ④ perception | `Src/per_delay.c` | 77 | 读 TIM2 计数器 + 无符号减法处理回绕 |
| ④ perception | `Inc/per_dht11.h` | 157 | DHT11 接口 + `Status` / `Data` / `FailStage` / `Diag` |
| ④ perception | `Src/per_dht11.c` | 353 | 单总线时序 + 切方向 + 关中断 + 自检 |
| ③ network | `Inc/net_uart.h` | 151 | 接口契约（串口三件套 + `NET_UART_Id_t` 实例编号） |
| ③ network | `Src/net_uart.c` | 302 | 实例表（2 项）+ DMA 循环 + IDLE 判帧 + ringbuf + 错误自愈 |
| ③ network | `Inc/net_esp8266.h` | 202 | AT 会话接口（12 个）+ 返回码 + 超时表说明 |
| ③ network | `Src/net_esp8266.c` | 586 | AT 命令拼接与等待 + `+IPD` 四步分流 + 两段缓冲 + 断线判定 |
| ② platform | `Inc/mod_uart1.h` | 76 | 接口契约（统一入口） |
| ② platform | `Src/mod_uart1.c` | 54 | 五个函数，各一行转发（带 `NET_UART_DBG`） |
| ② platform | `Inc/mod_wifi.h` | 145 | 连网语义接口（11 个）+ `MOD_WIFI_Status_t` |
| ② platform | `Src/mod_wifi.c` | 116 | 状态归一 + 建链后切 MQTT 模式 + 收帧切片 |
| ② platform | `Inc/mod_mqtt.h` | 162 | 报文接口（8 个）+ `MOD_MQTT_Msg_t` 结构体 |
| ② platform | `Src/mod_mqtt.c` | 565 | 剩余长度变长编码 + 四种报文 + 报文 ID 会话 + PUBACK |
| ① app | `Inc/app_blink.h` | 124 | 接口契约（状态机，含 On/Off/IsOn） |
| ① app | `Src/app_blink.c` | 186 | 闪烁状态机 + 手动接管 |
| ① app | `Inc/app_led.h` | 53 | 接口契约 |
| ① app | `Src/app_led.c` | 96 | 闪烁业务 |
| ① app | `Inc/app_cmd.h` | 50 | 接口契约（串口命令） |
| ① app | `Src/app_cmd.c` | 242 | 命令解析 + 回显 + 上电自检快闪 |
| ① app | `Inc/app_dht11.h` | 78 | DHT11 采集任务接口 + `APP_DHT11_Snapshot_t`（快照） |
| ① app | `Src/app_dht11.c` | 293 | 周期读 + 重试 + 写快照 + 自己拼串输出（不引 printf） |
| ① app | `Inc/app_link.h` | 71 | 阈值联动接口（温度→LED1、湿度→LED2） |
| ① app | `Src/app_link.c` | 113 | 边沿触发：只在跨阈值那一帧写灯 |
| ① app | `Inc/app_mqtt.h` | 78 | 上云接口 + **8 个用户配置宏**（SSID / 三元组 / 物模型） |
| ① app | `Src/app_mqtt.c` | 709 | 六段连接 + 退避重连 + 周期上报 + 离线缓存 + 命令响应 |
| ① app | `Inc/app_task.h` | 52 | 接口契约（四个任务） |
| ① app | `Src/app_task.c` | 194 | 四个任务集中创建 |
| — | **合计** | **6626** | |

另有：

- **横切 common 3 文件 / 127 行**：`ringbuf.h`（28）+ `ringbuf.c`（56）+ `log.h`（43）；
- **8 个 `.gitkeep`**：四层的 `Inc/` 与 `Src/` 各一个；
- **Core 挂载点与 MX 改动**：见下节。

> 四章的规模演进：**15 文件 / 1091 行**（纯闪烁）→ **21 文件 / 2267 行**（加串口）
> → **29 文件 / 3677 行**（加 DHT11）→ **37 文件 / 6626 行**（加上云）；横切 common 三段始终 127 行未变。
>
> 上云章节的 8 个新文件（2563 行）分别是：`net_esp8266.h/.c`（202/586）、`mod_wifi.h/.c`（145/116）、
> `mod_mqtt.h/.c`（162/565）、`app_mqtt.h/.c`（78/709）；
> 另有一批既有文件被扩写共 386 行：`net_uart.h/.c` +27/+96（实例表）、`app_dht11.h/.c` +33/+30（快照）、
> `app_task.c` +31（第四个任务）、cfg 四件 +168（开关 / 参数 / 闸门 / 链路表）、`app_led.c` +1。

---

## 设计要点

### 1. 串口三件套（网络层 `net_uart.c`）

```text
① DMA 循环接收  一次配置，硬件自己转圈搬运，写满自动绕回，永不停止
② IDLE 判帧     总线一安静就进空闲中断，此刻把"新到的字节"切成一帧
③ 环形缓冲      中断只管塞，任务有空了再来取（SPSC，无需关中断）
```

判帧的核心数学（缓冲区 256、NDTR 递减计数）：

```c
pos = 256 - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);   /* 硬件写到哪儿了 */
len = (pos >= start) ? (pos - start)                  /* 没绕回 */
                     : (256 - start + pos);           /* 绕回了，补一整圈 */
s_dma_last_pos = pos;                                 /* 记下搬到哪儿 */
/* 再从 DMA 缓冲拷进 ringbuf：跨末尾时分两段写 */
```

**ISR 三条纪律**：只搬数据、不解析不打印；不阻塞不等空位；不调任何 RTOS API。

### 2. 手动与闪烁互斥（应用层 `app_blink.c`）

```text
错误：敲 LED1 ON → 直接点灯 → 500ms 后状态机又翻一次 → 灯莫名灭了
正确：敲 LED1 ON → APP_BLINK_On()
                     ├─ half_period_ms = 0    状态机从此跳过这颗灯
                     ├─ counter_ms     = 0    下次重启闪烁从零起算
                     └─ PER_LED_On()          再去点灯
```

顺序不能反：先停闪、后点灯。

### 3. 感知层为什么现在才用上 `active_low`

`toggle` 与亮灭方向无关（只翻 ODR），所以闪烁用不到它；`write` / `is_on` 是"亮灭语义"，必须查表翻译：

```c
level = s_led_hw[i].active_low ? (on ? RESET : SET) : (on ? SET : RESET);
```

于是换一块"高电平点亮"的板子，改 `s_led_hw[]` 表里那**一个数字**就行——函数体、上层代码全不动。

### 4. 两条"永远存在"的接口

MX 生成文件里的调用点不受本工程开关控制，所以被它们调用的函数必须"永远存在"、关开关时变空壳：

| 函数 | 调用点 |
|---|---|
| `app_taskCreate()` | `Core/Src/freertos.c` |
| `net_uart_rx_idle_handler()` | `Core/Src/stm32f4xx_it.c` |

> 判断标准：**只要调用点在 MX 生成文件里，被调用的函数就必须永远存在。**
> 否则"关开关"会变成 undefined reference——而关开关本该只是配置操作，不该炸链接。

### 5. 编译期闸门：40 道

| 位置 | 数量 | 拦什么 |
|---|---|---|
| `cfg_check.h` · 依赖校验 | 18 | 悬挂配置（如 APP_UART1 开而 APP_BLINK 关）、串口链三层断档、DHT11 两层断档、**上云链五跳断档（5 条串成一条链）**、**跨链借用（APP_LINK → PER_LED）、三条链都关而 APP_TASK 仍开** |
| `cfg_check.h` · 参数体检 | 21 | 节拍 ≥1、半周期是节拍整数倍且 ≥2×节拍、四个任务栈 ≥512 字节、四个优先级 1~55、行缓冲 ≥8、DHT11 周期 ≥1000 ms、重试耗时 ≤ 半个周期、湿度阈值 20..90、**上报周期 ≥1000 ms、keepalive ≥10 s、MQTT 发送缓冲 ≥256 B、命令主题缓冲 ≥72 B** |
| `cfg_print.h` · 打印链 | 1 | 打印总闸拨 1 却没有串口链 |

最值得记住的两条：
- **`APP_LINK_ENABLE requires PER_LED_ENABLE`**：`app_link` 属于 DHT11 链，却要借 LED 链的 `per_led` 点灯；
- **上云链那五条串成一条**：`APP_MQTT → MOD_MQTT → MOD_WIFI_LINK → NET_ESP8266 → NET_UART3 → HAL_UART_MODULE`，
  少一环就是"编好的报文没有传输层可发"这种运行期谜题。故意只关 `NET_UART3_ENABLE` 试一次，会看到
  `NET_ESP8266_ENABLE requires NET_UART3_ENABLE`（脚本 L 档已实测）。

> ⚠️ 阈值写的是**字面量**（`512`、`56`），**不能引用** `configMINIMAL_STACK_SIZE` 之类的 FreeRTOS 宏：`module_cfg.h` 是各 `.c` 的第一个 `#include`，此时 FreeRTOS 头文件还没进来，宏名在 `#if` 里会被当成 0 → 校验"永远通过"，变成假安全。

### 6. 契约式注释（头文件即接口说明书）

每个函数在 `.h` 里都写同一张**五联表**：

```text
/*--------------------------------------------------------------
 * 函数功能：
 * 参数    ：
 * 返回值  ：（写具体取值，如 PER_LED_OK(0) / 1 = 编号越界）
 * 调用处  ：（写跨层调用链，如 app_cmd → APP_BLINK_On → PER_LED_On → per_gpio_write）
 * 注意事项：（①②③ 编号，写"为什么这么设计 / 改错会怎样"）
 *--------------------------------------------------------------*/
```

`.c` 里写侧重实现的一份，并标注 `【接口契约见 xxx.h】`。

### 7. 刻意"没做"的事

| 没做 | 为什么 |
|---|---|
| DMA 发送 | 本册只回几十字节的短句，阻塞发送占用几十微秒，够用；量产应换 DMA 发送（只改 `net_uart.c`） |
| 统一 `err_code` 错误码 | 返回值只有 `0 = OK` / `1 = ERR_PARAM` 两种语义，规模不够 |
| `printf` 重定向 | 中文会乱码、体积大、ISR 里还是灾难；业务回复用 `SendString` 直出即可 |
| 命令互斥量 | 两个任务共享状态机状态数组，靠"uint16_t 对齐读写原子 + 抖动 1ms 无所谓"过关；状态变复杂时应在 app 层内补互斥 |
| Keil 工程文件 | 用 CMake + Ninja 描述构建，一条命令就能编 |
| **用 `printf` 打温湿度** | 格式化库会拉进几 KB FLASH；本册自己写 5 个拼串函数（几十行）换来这份余量 |

### 8. DHT11 单总线的四道纪律（1.4 章节）

```text
① 微秒底座必须是硬件定时器
     APB1 定时器时钟 84 MHz → PSC=83 → 1 µs/计数。
     不用 DWT 的 CYCCNT（调试器下载后常常不计数）、不用 HAL_Delay（只有 ms）。
     回绕靠无符号减法自动处理：(now - start) < us，32 位 71.6 分钟回绕一次。

② 关中断要"成对"，且所有失败路径汇到同一个出口
     40 位数据是 26~70 µs 的脉宽，任务切换或任意中断抢走几百 µs 就测错了。
     所以从"拉高起始信号"到"读完 40 位"之间 __disable_irq()（约 4~5 ms/次，占 2s 周期 0.3%）。
     ★ 所有失败路径一律 goto exit，在 exit 里统一 __enable_irq()
       —— 否则一次超时就让系统永远关着中断，这是本驱动最容易写错的地方。

③ 18 ms 起始信号刻意放在关中断【之前】
     它不是时序敏感段（差几毫秒传感器都认），放外面能少关 18 ms 中断。
     "能少关就少关"是硬规矩。

④ 输入方向配【浮空】而不是上拉
     配成上拉会与模块内已有的 4.7k 上拉形成分压，高电平拉不到位。
```

**失败时报 `stage=1~6`**，让"读不到数"这个笼统现象有 6 种具体答案：

| stage | 含义 | 去查 |
|---|---|---|
| 1 | 完全没回应 | 供电 / 接线 / PG9 / 上拉电阻 |
| 2 / 3 | 响应只回了一半 | 上拉 / 接触不良 / 线太长 |
| 4 / 5 | 数据段异常 | 电源纹波 / 线长 / 被长中断打断 |
| 6 | 校验和错 | 电气质量（干扰、供电不足） |

### 9. 边沿触发：让闪烁与联动共存

LED1 同时被**三处**惦记：1 Hz 闪烁（Toggle 翻转语义）、串口命令（On/Off 绝对语义）、温度联动（On/Off 绝对语义）。
前两者靠"命令先清半周期退出闪烁"解决；温度联动若是每 2 秒都写一次 On/Off，闪烁就被挤掉了。

```c
static uint8_t s_led1_state = 0xFFU;   /* 0xFF = 还没联动过，首帧必写 */
void APP_LINK_ApplyTemp(uint8_t temp_int)
{
    uint8_t want = (temp_int >= APP_LINK_TEMP_THRESHOLD) ? 1U : 0U;
    if (want == s_led1_state) return;  /* 状态没变：把灯让给闪烁 */
    s_led1_state = want;
    (void)(want ? PER_LED_On(PER_LED1) : PER_LED_Off(PER_LED1));
}
```

代价是"跨越阈值的那一瞬间可能与闪烁撞车一次"（概率极低、后果是灯闪一下），
比"为了让位而关掉闪烁"划算得多。

### 10. 感知层只探测、不打印

`PER_DHT11_SelfTest()` 把自检结果填进 `PER_DHT11_Diag_t` 结构体，**自己一句都不打印**。
打印要走上层的 `mod_uart1 → net_uart`，感知层依赖上层会让依赖方向倒转、分层直接破产。
所以"测出来"与"表达出来"分在两层——应用层拿到数字，再决定把它变成哪一行文本。

同一条纪律在上云链上重演：`net_esp8266` **只把现场递上去**（`net_esp8266_get_last_resp()`），
由应用层的 `app_mqtt_dump_resp()` 决定把它变成 hex + ascii 一行文本。

### 11. 一条代码管两个串口：`net_uart` 的实例表（2.2 章节）

判帧那 20 行数学（NDTR → 位置 → 绕回 → 两段拷贝）与"是哪个串口"**完全无关**。
所以复制一份 `net_uart.c` 是错的——应该把"变的东西"进表、"不变的"留代码：

```c
typedef struct {
    UART_HandleTypeDef *huart;      DMA_HandleTypeDef *hdma_rx;
    uint8_t *dma_buf;  uint32_t dma_size;
    ringbuf_t *ring;   volatile uint32_t last_pos;  uint8_t ready;
} net_uart_ctx_t;

static net_uart_ctx_t s_uart[NET_UART_COUNT] = { /* USART1 / USART3 各一项 */ };
```

函数首参从"无"变成 `NET_UART_Id_t id`（`NET_UART_DBG=0` / `NET_UART_WIFI=1`），
`net_uart_ctx(id)` 做**越界 + ready 双门卫**，判帧代码一个字都没改。

> ★ **这张表是有代价的**：只关 `APP_MQTT_ENABLE` 时 FLASH 能掉到与关总闸相同（35116 B），
> 但 **RAM 多 776 B** —— UART3 的 512 B DMA 缓冲 + 268 B 环形缓冲被表项**钉住**，
> `--gc-sections` 不敢动。想真省 RAM 必须关 `MOD_WIFI_ENABLE`（详见架构文档第二节）。

### 12. 两条缓冲物理隔离 + `+IPD` 四步分流

AT 会话是**文本行**，MQTT 报文是**二进制**，混在一条串口上。所以分两个缓冲，谁也不踩谁：

| 缓冲 | 大小 | 装什么 | 满了怎么办 |
|---|---|---|---|
| `s_resp_buf` | 512 B | `OK` / `ERROR` / `WIFI GOT IP` / `SEND OK` 这些**文本行** | 丢最旧的一半，保住最新状态 |
| `s_payload` | 256 B **环形** | `+IPD` 里的**二进制载荷**（不追加 `\n`） | 丢最旧并置溢出标志（`net_esp8266_take_overflow()`） |

`+IPD` 的坑不在"找"，在"**没找全**"——一帧可能被拆成两次 IDLE 中断才到齐：

```text
① 找 "+IPD,"                  → 找到就往下走
② 读到冒号，取 data_len        → "IPD,45:" → 45 字节
③ 数据没到齐就退出，下次再拼   ★ 灵魂所在：这一步不写，报文必错位
④ 整段搬进载荷环
```

> **断线只认"独立成行"的 `CLOSED`**（`\r\nCLOSED\r\n` 或行首 8 字节），不能按子串匹配——
> MQTT 报文里也可能出现这 6 个字节。也不用 `AT+CIPSTATUS`：它的回显会插进载荷流里。

### 13. 六段连接 + 分段重连 + 退避（`app_mqtt.c`）

连接不是"一次调用"，是**六段**，每段都有自己的超时和失败语义：

| 段 | 动作 | 超时 | 给短了 / 给长了会怎样 |
|---|---|---|---|
| ① | `AT` 测活 | 2 s | —— |
| ② | `AT+CWMODE=1` | 2 s | —— |
| ③ | `AT+CWJAP="ssid","pwd"` | **10 s** | 短了：DHCP 还没回就判失败；长了：拖住重连 |
| ④ | `AT+CIPSTART="TCP","host",1883` | **8 s** | 短了：DNS 解析没完就放弃 |
| ⑤ | MQTT `CONNECT` | 2 s | 半开连接（TCP 在、MQTT 死）→ 强制 `CIPCLOSE` 重建 |
| ⑥ | `SUBSCRIBE .../commands/#` | 2 s | 订阅不上 = 永远收不到命令 |

- **一律 `osDelay`，不用 `HAL_Delay`**：`HAL_Delay` 是死等计数器、不让出 CPU，CWJAP 等 10 秒会让 1 Hz 闪烁定住、看着像死机。
- **退避 2 / 4 / 8 / 16 / 30 秒**，第 5 次起 `AT+RST` 复位模组。封顶 30 s 是因为路由器重启通常几十秒内恢复。
- **三种"断"分开处理**：WiFi 掉（`CWJAP` 失败）/ TCP 断（收到 `CLOSED`）/ MQTT 掉（`CONNACK` 超时）；
  重连只补没成功的那几段，不是每次从头再来。
- **离线缓存 16 条**（≈80 秒窗口），队满丢最旧，恢复后每 500 ms 补一条。**必须有界**——不设上限会吃光 128 KB RAM。

### 14. 快照而不是队列（`APP_DHT11_GetSnapshot`）

上报任务不该"为了发一条数据去催传感器再读一次"——那会把 2 s 的采集周期打乱。
改成**采集任务写、上报任务只读**的三行接口：

```c
APP_DHT11_Snapshot_t snap;
APP_DHT11_GetSnapshot(&snap);      /* 整体结构体拷贝，不持锁 */
if (snap.valid == 0U) { /* 还没采到过：如实说"没数据"，不编一个 0 上去 */ }
```

最差情况读到的是"上一组"，对 5 秒一次的上报完全够用，所以**不需要互斥量**。

---

## 资源占用

以 `Debug` 预设、arm-none-eabi-gcc 14.3.1 实测（**58 个构建目标，0 warning / 0 error**）。
每一档都是"删掉 `build/` 全量重建"取的数，脚本固化在 `docs/_tools/measure_scenarios.py`。

| 形态 | FLASH | RAM |
|---|---|---|
| **默认（四链全开）** | **46452 B** / 512 KB（8.86%） | **24496 B** / 128 KB（18.69%） |
| A 关自动闪烁（`APP_LED=0`，串口控灯形态） | 46232 B（8.82%） | 24496 B（18.69%） |
| B 裁联动（`APP_LINK=0`，纯采集器） | 46232 B（8.82%） | 24496 B（18.69%） |
| C 关整条 DHT11 链（`MOD_DHT11=0`） | 43300 B（8.26%） | 24480 B（18.68%） |
| D 只留 DHT11 感知层（`APP_DHT11`+`APP_LINK`=0） | 43300 B（8.26%） | 24480 B（18.68%） |
| **E 关整条上云链（`MOD_WIFI=0`，= 1.4 功能集）** | **35116 B**（6.70%） | **21456 B**（16.37%） |
| **F 只裁上云应用层（`APP_MQTT=0`）** | **35116 B**（6.70%） | **22232 B**（16.96%） |
| G 再裁掉 MQTT 报文层（`+MOD_MQTT=0`） | 35116 B（6.70%） | 22232 B（16.96%） |
| H 回退纯闪烁（再关 DHT11/上云/串口链 + `HAL_UART`） | 29168 B（5.56%） | 20904 B（15.95%） |
| I 裸机骨架（四链全关 + `APP_TASK=0`） | 28756 B（5.49%） | 20896 B（15.94%） |

五点值得知道：

- **基准体积的用法**：只想改注释时，编译后体积**应当完全不变**——体积一变，说明动到了真实代码。
- **上云链共 11336 B FLASH / 3040 B RAM**（46452 − 35116、24496 − 21456）。这是一整条 MQTT + AT + 第二个串口的价钱。
- **E / F / G 三档 FLASH 完全相同（35116 B）**："关开关"真的能把 `app_mqtt` / `mod_mqtt` / `mod_wifi` / `net_esp8266` 全部 gc 掉，一行都不剩。
- **但 F 的 RAM 比 E 多 776 B**：UART3 的 512 B DMA 缓冲 + 268 B 环形缓冲被 `net_uart` 的**实例表项钉住**了。
  这是"参数化"的代价——想真省 RAM 只能关总闸（E 档）。这条是本册最值得记住的实测结论。
- **未用代码会被自动回收**："C 关整条 DHT11 链"与"D 只留 DHT11 感知层"**完全同体积**，
  说明留下的 `per_delay` + `per_dht11` 被 `-Wl,--gc-sections` 100% 回收；
  H 与 I 只差 412 B，那 412 B 全是 Core 层与 HAL 的固定开销。

> **cfg 的开关管不到 Core 层**：H / I 档的 28~29 KB 里仍含 CubeMX 生成的
> `usart.c`（USART1 + **USART3**）/`dma.c`（DMA2_Stream2 + **DMA1_Stream1**）/`tim.c` 及它们拖进来的
> `HAL_UART_Init`（已用 `nm` 验证）。所以本表**不能**与"纯闪烁工程 v2.1b（20836 B）"直接比——
> 那个工程连 `usart.c` 都不存在。想省这部分得去 `.ioc` 里删外设重新生成。

> ⚠ 改过 `cfg_switch.h` 后必须 `rm -rf build/` 全量重建再取数，否则 ninja 会直接复用旧 .elf。
> 本工程的体积表就曾因此整体失真过。
> 上表 10 档由 `docs/_tools/measure_scenarios.py` 逐个改开关 + 全量重建测出（2026-10-29 重跑复核）。

---

## 文档

全部为中文，正文在 `docs/` 下；每篇 `.md` 都有**同名单文件网页版**（样式内嵌、无外部依赖，可直接双击打开或打印成 PDF）。

| 分类 | 文档 | 内容 |
|---|---|---|
| 索引 | [`docs/README.md`](docs/README.md) | 分类导航 + 按角色的阅读路径 + 维护脚本清单 |
| architecture | [四层架构解析 · WiFi-MQTT 上云](docs/architecture/四层架构解析-WiFi-MQTT上云-1.0.md) | **最新一册主文档**：四条链、`net_uart` 参数化与"776 B 钉住"的代价、七个设计决策、十档体积、40 道闸门、**与课件三处差异** |
| architecture | [四层架构解析 · DHT11 温湿度与 LED 联动](docs/architecture/四层架构解析-DHT11温湿度与LED联动-1.0.md) | 上一册：三条链两个合流点、六个设计决策、七档体积、29 道闸门、与五层 H743 对照 |
| architecture | [四层架构解析 · 串口控灯例程](docs/architecture/四层架构解析-串口控灯例程-1.0.md) | 再上一册：两条链与合流点、14 跳旅程、感知层扩接口、互斥设计、实测数据 |
| architecture | [四层架构解析 · 最简 LED 例程](docs/architecture/四层架构解析-最简LED例程-1.0.md) | 入门主文档（v2.1b 纯闪烁） |
| software-engineering | [module_cfg 配置体系与两级开关](docs/software-engineering/module_cfg配置体系与两级开关-1.0.md) | 一拆四、包含顺序、**40 道闸门逐条**、四链依赖图、九个裁剪场景、MX 调用点纪律 |
| software-engineering | [串口三件套 · DMA 循环接收与 IDLE 判帧](docs/software-engineering/串口三件套-DMA循环接收与IDLE判帧-1.0.md) | 判帧数学、环形缓冲、ISR 纪律、出错自愈、缓冲大小怎么算、坑清单（实例表就是对它的复用） |
| software-engineering | [闪烁状态机与契约式注释](docs/software-engineering/闪烁状态机与契约式注释-1.0.md) | 状态机三原则、500 ms 逐拍推演、五联表模板 |
| tutorials | [t04 · 从零给工程接 WiFi-MQTT 上云](docs/tutorials/t04-从零给工程接WiFi-MQTT上云.md) | **本册**：Step 0~9（接线 + 云端六步 + 六段连接）+ 三板斧 + 4 道练习 |
| tutorials | [t03 · 从零给工程接 DHT11 与阈值联动](docs/tutorials/t03-从零给工程接DHT11与阈值联动.md) | Step 0~9 手写顺序 + **三板斧**（看 stage / 看自检行 / 确认物理条件）+ 5 道练习 |
| tutorials | [t02 · 从零给工程接串口控灯](docs/tutorials/t02-从零给工程接串口控灯.md) | 前置：Step 0~9 接 USART1，每步含"目标 / 做法 / 自检" |
| tutorials | [t01 · 从零搭建四层 LED 闪烁](docs/tutorials/t01-从零搭建四层LED闪烁.md) | 再前置：先会闪灯 |
| reference | [引脚与符号速查表](docs/reference/引脚与符号速查表.md) | 引脚 / 时基 / 37 个文件 / 76 个函数 / 四条链 / 命令表 + DHT11 与 MQTT 输出 / 宏与枚举 / 多档体积 / 排障速查 |
| test | [05 · WiFi-MQTT 上云链测试文档与用例](docs/test/05-WiFi-MQTT链-测试文档和用例.md) | 五层分层用例 + 集成 6 条 + 11 条闸门 + **多档裁剪态（已实测）** + 健康度清单 |
| test | [04 · DHT11 链测试文档与用例](docs/test/04-DHT11链-测试文档和用例.md) | 四组分层用例 + 12 条闸门 + **7 档裁剪态 + 3 个反向用例**（均已实测） |
| test | [03 · 上板实测操作卡](docs/test/03-上板实测操作卡.md) | **照做清单**：接线 → 烧录 → 快闪自检 → 五条命令 → **Step 9+ DHT11 三动作（捏热/哈气/拔线）** → 20 项记录表 → 故障速查 |
| test | [02 · 串口链测试文档与用例](docs/test/02-UART链-测试文档和用例.md) | 分层 + 集成 + 11 条闸门（已实测）+ 3 组裁剪态（已实测） |
| test | [01 · LED 链测试文档与用例](docs/test/01-LED链-测试文档和用例.md) | 上一版的闪烁用例 |
| teaching | `docs/teaching/*.pptx` | 培训课件：1.1 架构与运维职责、1.2 开发环境搭建、**1.3 构架搭建与分层与 LED 灯控制**、**1.4 DHT11 传感器与 LED 灯联动**、**2.2 WiFi-MQTT 上云（本册对应）**、其余 2.1 / 3.x~5.x |

> 文档里的**代码片段、行数、体积、报错信息**全部取自磁盘上的真实文件，不是二手转述。
> 改完 `.md` 后在 `docs/` 下跑 `python build_docs.py` 重新生成网页版。

---

## 常见问题

### 串口一个字都收不到，怎么排查？

**三板斧**，从最可能到最不可能：

1. **线接对了吗** —— 板子 **PA9(TX) → 转串口 RX**、**PA10(RX) → 转串口 TX**，**必须共地**；波特率 115200-8-N-1。
2. **IDLE 判帧的顺序对吗** —— `USART1_IRQHandler()` 里 `__HAL_UART_CLEAR_IDLEFLAG()` + `net_uart_rx_idle_handler()` 必须写在 `HAL_UART_IRQHandler()` **之前**。顺序写反 = 永远检测不到总线安静，一个字节都收不到。
3. **DMA 真的拉起来了吗** —— `HAL_UART_Receive_DMA()` 返回 `HAL_BUSY` 时会一个寄存器都不写；`net_uart_start_rx_dma()` 里已做了"复位 RxState 再启动一次"的处理，别把它删掉。

**还有一个最快的信号**：敲字符有没有**回显**。

- 有回显 → 收（RX）和发（TX）都通；
- 没回显但板子在跑 → 问题在收；
- 连开机横幅都没有 → 检查任务是否建起来（上电 LED1 有没快闪 3 次）。

### 灯不亮 / 不闪

1. **烧录成功了吗** —— 看 verify 是否 OK，必要时全片擦除再烧；
2. **引脚对吗** —— LED1 = **PG14**，**低电平点亮**；换板子要改 `s_led_hw[]` 与 `.ioc`；
3. **节拍在跑吗** —— 在 `APP_BLINK_process()` 下断点，看是否每 1 ms 进来一次。

### 敲了 LED1 ON，灯亮了但过一会儿自己灭了

说明命令绕过了状态机：正确的入口是 `APP_BLINK_On()`（先清半周期），不是直接 `PER_LED_On()`。

### 串口每 2 秒冒出的 DHT11 读数，看着很乱

正常：DHT11 是周期吐字的，与命令行的回显/应答交织在同一条串口上。敲命令时看 `OK:` 即可。

### DHT11 一直报 `Read FAILED (stage=1)`，怎么排查？

**三板斧**，从最可靠的证据开始：

1. **看自检行的四个字段**（上电时那一行为准）：
   - `TIM2=` 不是 ≈1000 → TIM2 的 PSC 配错，**所有 µs 计时都错**，必然读不到；
   - `loop=` 不是 `1,0` → PG9 引脚不通（虚焊、引脚号写错）；
   - `bus=0` → 总线被外部拉低（缺上拉或短路）；
   - `resp=0` → 传感器完全没应答。
2. **确认物理条件**：模块 VCC = 3.3 V、DATA 与 VCC 之间有 4.7k~10k 上拉（裸传感器必须外接）、线长 < 20 cm、采集间隔 ≥1 秒。
3. **看 stage 定位**：`1` = 完全没回应（查供电接线）；`6` = 校验和错（查电气质量）。

### 温度到了，LED1 却没亮

先确认 `APP_LINK_ENABLE` 是 1（被裁掉了就只打印不点灯），再看阈值：
`APP_LINK_TEMP_THRESHOLD` 默认 28 ℃——手指捏住传感器或对它哈气才能越过。

### DHT11 读数在跳，但两颗灯都不动

要么联动被裁了（`APP_LINK_ENABLE=0`），要么阈值没到。
拔掉 DATA 线验证"失败安全"：应当只报 `stage=1`，**两颗灯保持现状不乱动**。

### 改了 `cfg_param.h` 但没生效

配置是**编译期**的：改完必须重新编译并重新烧录，不是在线可调的。

### 报 `#error "..."` 编译不过

这是**设计好的**。40 道闸门会在编译期拦下配置矛盾——
比如关了闪烁状态机却还开着串口控灯，或关了串口链却还留着要打印的 DHT11，
或留着 `NET_ESP8266_ENABLE` 却把 `NET_UART3_ENABLE` 关了。
读 `#error` 后面的文字，它会告诉你是哪两个宏冲突。

### 上云一直 `[MQTT] retry #1 in 2s`，连不上

按**报错那一行的 `resp=` 内容**分三种情况（`app_mqtt_dump_resp()` 会把模组原始回显打出来）：

| `resp=` 的样子 | 说明 | 去查 |
|---|---|---|
| `resp=0 (empty: check TX/RX & power)` | 模组一个字都没回 | **TX/RX 没交叉**、模组没供电（3.3 V，峰值 300 mA）、EN 悬空 |
| 有内容但都是乱码 | 波特率不对 | 模组出厂常是 **115200**，但有些是 9600 —— 改 `MX_USART3_UART_Init` 的 `BaudRate` 或先用 AT 改模组 |
| 有内容且能读到 `ERROR` | 模组活着，只是这句指令不认 | 看内容：`CWJAP` 报 `FAIL` = 密码错 / **5 GHz AP**；`CIPSTART` 报 `ERROR` = 域名或端口错 |

### `[MQTT] WiFi connected` 之后卡在 `TCP connecting`

- **域名 vs IP**：先用 `ping` 确认 `xxx.iot-mqtts.cn` 能解析；
- **端口必须是 1883**（明文 MQTT）。8883 是 TLS，AT 固件不做 TLS 握手，会一直等到 8 秒超时；
- ESP8266 的 AT 固件对长域名 + DNS 较慢，`CIPSTART` 给了 8 秒，别改小。

### 串口打印 `[MQTT] Published: ...`，但云端"最新数据"是空的

**最经典的一类"静默失败"**：物模型对不上。华为云收到报文后按 `service_id` 和属性名匹配"功能定义"，
**匹配不上就丢弃、且不回任何错误**。所以设备在线、日志有 Published、云端却没数据。

逐个核对：`APP_MQTT_SERVICE_ID`（默认 `"Sensor"`）与属性名 `temperature` / `humidity`
必须与控制台**逐字一致**（大小写也算）。

### 云端下发了命令，串口没反应

1. **订阅上了吗** —— 看有没有 `[MQTT] subscribed: $oc/devices/<id>/sys/commands/#`；
   少一个 `#` 就只能收到"恰好叫那个名字"的主题，实际一条也收不到；
2. **命令名对吗** —— 处理函数只认 `led` 这个字段（`app_mqtt_parse_led()`），别的字段会被忽略；
3. **是不是立刻又灭了** —— 那说明绕过了状态机。正确路径是 `APP_BLINK_On()`（先退出闪烁），
   直接写引脚会被 1 Hz 状态机在半周期后翻回来。

### 只在关掉 `APP_MQTT_ENABLE` 时 RAM 没降

**不是 bug，是实例表的代价**：UART3 的 512 B DMA 缓冲 + 268 B 环形缓冲被 `net_uart` 的实例表项**钉住**，
`--gc-sections` 不敢回收。FLASH 能省（35116 B），RAM 仍多 776 B。想真省 RAM 必须关总闸 `MOD_WIFI_ENABLE`。

### 换了文件夹名，编译产物名字变了

`CMakeLists.txt` 用 `get_filename_component(... NAME)` 自动取文件夹名作为工程名。这是有意设计——方便一个模板复制成多个工程。

---

## 四章演进：v2.1b → 加串口 → 加 DHT11 → 加上云

`v2.1b` 是会闪灯的四层工程（15 文件 / 1091 行），后面分三步长成现在这样：

| 章节 | 改动类型 | 内容 |
|---|---|---|
| **1.3 串口** | 新增（user） | 6 个文件 / **748 行**：`net_uart.h/.c`（330）、`mod_uart1.h/.c`（130）、`app_cmd.h/.c`（288） |
| | 改动（user） | 感知层补 `write/is_on` + `On/Off/IsOn`（+195）、状态机补手动接管（+94）、`app_task` 加串口任务（+40）、cfg 五件套（+95） |
| | 新增（Core） | `usart.c` / `dma.c`；`stm32f4xx_it.c` 的 `USART1_IRQHandler` |
| | 改动（Core） | `main.c` 加 2 行 MX 调用；`hal_conf.h` 开 `HAL_UART_MODULE_ENABLED`；`.ioc` 配 USART1 + DMA + NVIC |
| | 小计 | 15 文件 / 1091 行 → **21 文件 / 2267 行**（+1176 = 新增 748 + 补齐 428） |
| **1.4 DHT11** | 新增（user） | 8 个文件 / **1159 行**：`per_delay.h/.c`（152）、`per_dht11.h/.c`（510）、`app_dht11.h/.c`（308）、`app_link.h/.c`（184） |
| | 改动（user） | `per_gpio` / `per_led` 从 1 颗灯扩到 2 颗（+75）、cfg 五件套加 5 条依赖 + 7 条体检 + 10 个参数（+283） |
| | 改动（Core） | `tim.c` 的 TIM2 PSC 改 83 + 启动；`gpio.c` 三引脚合并；`main.h` 加 PG9/PG13 宏；`.ioc` 同步 |
| | 小计 | 21 文件 / 2267 行 → **29 文件 / 3677 行**（+1517 = 新增 1159 + 扩已有 358） |
| **2.2 上云** | 新增（user） | 8 个文件 / **2563 行**：`net_esp8266.h/.c`（788）、`mod_wifi.h/.c`（261）、`mod_mqtt.h/.c`（727）、`app_mqtt.h/.c`（787） |
| | 改动（user） | `net_uart` 改实例表（+123）、`app_dht11` 加快照（+63）、`app_task` 加第四个任务（+31）、cfg 五件套加 5 条依赖 + 6 条体检 + 12 个参数（+168）、`app_led.c` +1 |
| | 新增（Core） | `usart.c` 的 `USART3` 段 + `dma.c` 的 `DMA1_Stream1`；`stm32f4xx_it.c` 的 `USART3_IRQHandler` 与 `DMA1_Stream1_IRQHandler` |
| | 改动（Core） | `main.c` 加 1 行 `MX_USART3_UART_Init()`；`.ioc` 配 PB10/PB11 + DMA1_Stream1 + NVIC |
| | 小计 | 29 文件 / 3677 行 → **37 文件 / 6626 行**（+2949 = 新增 2563 + 扩已有 386） |
| **未动** | | 1.4 章节没动任何串口文件；本册没动感知层四件（`per_gpio`/`per_led`/`per_delay`/`per_dht11`）与 `app_blink`/`app_cmd`/`app_link`；`freertos.c` 的两行挂载三次都没改 |

> 注意每一章都**没动上一章已有的核心文件**——四条链在代码上是彼此隔离的，
> 它们在 `APP_BLINK_*` 与 `per_led` 两个合流点相遇，而不是在源码里互相穿插。
> 唯一被两次改写的只有 `net_uart`（1.3 从无到有、2.2 改成实例表），
> 而它改的正是"**同一份知识要服务第二个串口**"这件事。

`freertos.c` 的挂载（**本工程唯一**的业务挂载点，v2.1b 就有的两行）：

```diff
@@ Private includes 段
 /* USER CODE BEGIN Includes */
+#include "app_task.h"
 /* USER CODE END Includes */

@@ MX_FREERTOS_Init() · RTOS_THREADS 段
 /* USER CODE BEGIN RTOS_THREADS */
 /* add threads, ... */
+  app_taskCreate();      /* 四层用户任务集中创建（app_task.c） */
 /* USER CODE END RTOS_THREADS */
```

> **为什么只改 `USER CODE` 段？** `freertos.c` 是 CubeMX 生成文件，段外的任何注释和代码在下次重新生成时会**丢失**。

串口章节新增的 `stm32f4xx_it.c` 挂载点同样写在 USER CODE 段内
（2.2 章节给 USART3 复制了同样一段，只把实例编号换成 `NET_UART_WIFI`）：

```c
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */
  if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE)) {
      __HAL_UART_CLEAR_IDLEFLAG(&huart1);
      net_uart_rx_idle_handler(NET_UART_DBG);   /* 网络层：按实例判帧 + 搬进环形缓冲 */
  }
  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
}

void USART3_IRQHandler(void)        /* 2.2 章节新增，逐字对称 */
{
  /* USER CODE BEGIN USART3_IRQn 0 */
  if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_IDLE)) {
      __HAL_UART_CLEAR_IDLEFLAG(&huart3);
      net_uart_rx_idle_handler(NET_UART_WIFI);
  }
  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
}
```

> ★ **中断里只调字节流层**，而**不**直接调 `net_esp8266_*`：
> `+IPD` 分流要把"文本响应"和"二进制载荷"分到两条缓冲、还要跨中断拼半包，
> 这些是任务上下文的活。ISR 里只做"把新到的字节搬进环形缓冲"这一件事，
> 三条纪律（只搬数据 / 不阻塞 / 不调 RTOS API）一条都没破。

---

## 与五层参考工程的关系

本工程是**四层**实现；`src/refer/` 下有**五层**对照版本，两者讲同一件事、代码可互相对照：

| 五层（refer） | 四层（本工程） | 说明 |
|---|---|---|
| L1_app（`app_uart` / `app_led` / `app_task` / `app_dht11` / `app_mqtt`） | `user/app`（`app_cmd` / `app_led` / `app_blink` / `app_dht11` / `app_link` / `app_mqtt` / `app_task`） | 命令解析与业务不变；上云业务同样是"连接 + 上报 + 命令"三件事 |
| L2_svc（`svc_led` 闪烁状态机 / `svc_env` 环境服务 / `svc_conn` 连接状态机） | `user/app`（`app_blink` / 并入 `app_dht11` / **并入 `app_mqtt`**） | 服务层收进应用层：本工程只有一条上云链、一个联网模组，连接分段不值得单独成层 |
| L3_plat（`plat_uart` / `plat_led` / `plat_dht11` / `plat_esp8266` 31 行 / `mqtt_port` 168 行） | `user/platform`（`mod_uart1` / **`mod_wifi` 116 行** / **`mod_mqtt` 565 行**）+ `user/perception`（`per_led`）+ **（DHT11 不设）** | 纯转发的 `plat_esp8266` 被扩成"状态归一 + 建链后切 MQTT 模式"；`mqtt_port` 原来是 paho 的移植层，四层版改成**手搓报文**（不再依赖 `Middlewares/Third_Party/paho`） |
| L4_bsp（`bsp_uart` / `bsp_led` / `bsp_dht11` / `bsp_delay` / `bsp_esp8266` 604 行） | `user/network`（`net_uart` **实例表** / **`net_esp8266` 586 行**）+ `user/perception`（`per_gpio` / `per_dht11` / `per_delay`） | 串口驱动与 AT 会话都进网络层；GPIO / 单总线 / 微秒底座进感知层 |
| L5_com（`module_cfg` + 4 子文件 + `ring_buffer`） | `user/cfg` + `user/common` | 配置体系原样，环形缓冲移入横切 `common/` |

**两边口径怎么统一**：四层（感知 / 网络 / 平台 / 应用）既是**责任口径**（出问题时定界"找谁"），也是本工程的**代码口径**（写码时定"文件放哪"）。

**不变的纪律**：命令只许上层 → 下层单向走；cfg 只被读不被调；错误"越靠上越能上报、越靠下越静默兜底"；
**层要有自己的知识才配单独存在**（所以串口三层、上云五跳、LED 两跳、DHT11 两层，四者都对）。

> DHT11 章节移植自 **`src/refer/H743ZI_V6.x`**（H743 + 五层）。移植时改动的三件事：
> 引脚 PF11 → **PG9**；定时器时钟 240 MHz → **84 MHz**（PSC 239 → **83**）；
> 命名 `BSP_*` → `PER_*`，并按四层砍掉纯转发的 `plat_dht11`。
> 至于自检如何上报，也从"驱动内部直接打印"改成了"只填结构体、交给应用层表达"。
>
> 上云章节同样参考它（`app_mqtt` 1084 行 / `bsp_esp8266` 604 行 / `mqtt_port` 168 行），
> 但**三处刻意不同**：① 不引 paho，四种报文手搓（省掉整个 `MQTTPacket` 目录）；
> ② 用明文 1883 而不是 TLS 8883（AT 固件不做 TLS 握手）；③ 中断里只调字节流层，
> `+IPD` 分流放在任务上下文。逐条理由见架构文档第七节。

---

## 参考资料与致谢

- **`src/refer/pro_5L_v2.2_uart_led`** —— 串口控灯的五层对照版，本工程的 `net_uart`（对应其 `bsp_uart`）、`mod_uart1`（对应 `plat_uart`）、`app_cmd`（对应 `app_uart`）均与之对齐。
- **`src/refer/pro_5L_v2.1_ledblink`** —— 纯闪烁的五层对照版。
- **`src/proL4_p1.3_blink_v2.1b`** —— 本工程的上一版（纯闪烁四层）。
- **H743ZI_V6.x** —— 配置体系（总入口 + 开关 / 打印 / 参数 / 校验）与契约式注释规范的来源例程；
  上云链的 `app_mqtt` / `bsp_esp8266` / `mqtt_port` 也参考了它（但那版走 paho + TLS，本工程改成手搓报文 + 明文 1883）。
  本工程是它的**最简教学版**。
- **《2.2 项目实操-WiFi-MQTT 上云》课件** —— 上云章节的出处（`docs/teaching/`）：
  华为云三主题、`+IPD` 分流、QoS1 与报文 ID、AT 超时表、断线重连与离线缓存都在那 29 页里。
- ST 官方 **STM32CubeF4** HAL 库与 **FreeRTOS** 内核。

---

## 许可证

本项目 `user/` 下的用户代码采用 [MIT 许可证](LICENSE)。

> 两点说明：`Drivers/`（STM32 HAL 库）、`Middlewares/`（FreeRTOS）等目录中的第三方代码遵循其各自原有的许可证，不在 MIT 许可范围内；本工程目前未附带 `LICENSE` 文本文件，需要时可直接使用 `src/refer/` 下的同名文件。
