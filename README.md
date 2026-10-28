# IOT_M4_L4 · STM32F407 四层架构「串口控灯例程」

![MCU](https://img.shields.io/badge/MCU-STM32F407ZETx-1E6BFF?style=flat-square)
![Architecture](https://img.shields.io/badge/Architecture-4%20Layer-12B8A6?style=flat-square)
![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS%20CMSIS--RTOS2-0B1F3A?style=flat-square)
![UART](https://img.shields.io/badge/UART-USART1%20115200--8--N--1-1FA055?style=flat-square)
![Build](https://img.shields.io/badge/Build-CMake%20%2B%20Ninja-FF7A29?style=flat-square)
![Toolchain](https://img.shields.io/badge/Toolchain-arm--none--eabi--gcc%2014.3.1-6B7A90?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-12B8A6?style=flat-square)

> 在**刻意做到最小**的 STM32 工程上再进一步：一颗 LED + 一条串口，把「四层怎么分、不定长帧怎么收、配置怎么收口、手动与自动怎么不打架」四件事讲透。
> 从纯闪烁的 `proL4_p1.3_blink_v2.1b` 出发，新增 **6 个文件 / 748 行**并给已有 15 个文件**补齐 428 行**（共 +1176 行），用户代码长到 **21 个文件 / 2267 行**（另有横切 `common/` 3 文件 / 127 行），实现 **LED1（PG14，低电平点亮）1 Hz 闪烁 + 串口命令控灯**。
> 本工程是培训课件《1.3 项目实操-构架搭建与分层与 LED 灯控制》串口章节的配套源码。

**适合谁看**：已经跑通四层闪灯、想接串口并对齐"感知 / 网络 / 平台 / 应用"四层口径的嵌入式新手；需要一套干净的"DMA + IDLE + 环形缓冲"模板的开发者；要讲串口章节的老师。

---

## 目录

- [这是什么](#这是什么)
- [硬件与环境](#硬件与环境)
- [3 分钟跑起来](#3-分钟跑起来)
- [串口命令怎么玩](#串口命令怎么玩)
- [目录结构](#目录结构)
- [改配置：只动 cfg 一个目录](#改配置只动-cfg-一个目录)
- [四层架构一览](#四层架构一览)
- [用户代码清单](#用户代码清单21-个文件--2267-行)
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

`proL4_p1.3_blink_v2.1b` 是一个"会闪灯"的四层工程。本工程（`IOT_M4_L4`）在它上面**只加了一件事：串口**。

```text
串口助手敲：LED1 ON  ↵        →  板上 LED1 亮，回 "OK: LED1 ON"
串口助手敲：LED1 OFF ↵        →  板上 LED1 灭，回 "OK: LED1 OFF"
串口助手敲：BLINK    ↵        →  恢复 1 Hz 闪烁，回 "OK: BLINK"
串口助手敲：STATUS   ↵        →  回 "LED1=ON"（读真实引脚电平）
串口助手敲：HELP     ↵        →  回命令表
```

加这一件事，四层里**空着的两个目录终于有内容了**：

| 层 | v2.1b（纯闪烁） | v2.1c（串口控灯） |
|---|---|---|
| ① app | `app_led` / `app_blink` / `app_task` | 新增 **`app_cmd`**（命令解析） |
| ② platform | 空目录 | **`mod_uart1`**（统一入口） |
| ③ network | 空目录 | **`net_uart`**（DMA + IDLE + 环形缓冲） |
| ④ perception | `per_led` / `per_gpio` | 扩出 **`PER_LED_On/Off/IsOn`** 与 `per_gpio_write/is_on` |

读完这个工程，你应该能回答：

- 为什么串口要占三层（app → platform → network），而 LED 只占两层？
- IDLE 中断里那 20 行判帧数学到底在算什么？
- 为什么"LED1 ON"不能直接点灯，非要过一遍闪烁状态机？
- 为什么 `app_taskCreate()` 和 `net_uart_rx_idle_handler()` 必须"永远存在"？
- 为什么把 `s_led_hw[].active_low` 这个字段留到串口章节才用上？

---

## 硬件与环境

| 项目 | 配置 |
|---|---|
| MCU | STM32F407ZETx（Cortex-M4F，168 MHz） |
| 用户 LED | **LED1 = PG14**，**低电平点亮**（漏极接法，写 0 亮 / 写 1 灭） |
| 调试串口 | **USART1：PA9 = TX，PA10 = RX**，**115200-8-N-1** |
| 接收方式 | **DMA2_Stream2 循环模式** + USART **IDLE 空闲中断判帧** + 环形缓冲 |
| 系统时钟 | HSE 8 MHz → PLL（M=8 / N=336 / P=÷2）→ SYSCLK **168 MHz**，AHB 168 / APB1 42 / APB2 84 MHz |
| HAL 时基 | **TIM6**（供 `HAL_Delay` 等使用） |
| RTOS 节拍 | **SysTick**，`configTICK_RATE_HZ = 1000` → 1 tick = 1 ms |
| RTOS | FreeRTOS 10.x + CMSIS-RTOS2（`osThreadNew` / `osDelay`），动态创建，堆 15360 B |
| 中断优先级 | USART1 抢占优先级 **5**（与 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 相等，本工程统一纪律：ISR 内不调 RTOS API） |
| 调试口 | SWD（ST-Link / J-Link 均可） |

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

预期输出（本次实测，**50/50 目标，0 warning / 0 error**）：

```text
Memory region         Used Size  Region Size  %age Used
             RAM:       21208 B       128 KB     16.18%
          CCMRAM:           0 B        64 KB      0.00%
           FLASH:       31228 B       512 KB      5.96%
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
④ 敲字符有回显                        ← 回显有 = 接收链路通
```

灯不亮或串口没反应，先按[常见问题](#常见问题)排查。

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
├── user/                       ★ 用户代码：四层架构（21 文件 / 2267 行）
│   ├── app/Inc, Src/           ① 应用层    app_led（闪烁业务）/ app_blink（状态机）
│   │                                      app_cmd（命令解析）/ app_task（任务创建）
│   ├── platform/Inc, Src/      ② 平台层    mod_uart1（串口1 统一入口：MOD_UART1_*）
│   ├── network/Inc, Src/       ③ 网络层    net_uart（DMA 循环 + IDLE 判帧 + 环形缓冲）
│   ├── perception/Inc, Src/    ④ 感知层    per_led（门卫）/ per_gpio（s_led_hw[] 表 + 唯一碰 HAL）
│   │
│   ├── cfg/                    ※ 横切·配置（纯头文件，无 .c；5 文件 / 392 行）
│   │   ├── module_cfg.h            ◆ 总入口：全工程唯一配置入口，只做按序包含
│   │   ├── cfg_switch.h            两级开关：模块总闸 + 分层子开关（11 个）
│   │   ├── cfg_print.h             打印开关（本册业务回复直出，总闸保持 0）
│   │   ├── cfg_param.h             参数：节拍 / 半周期 / 串口缓冲 / 任务栈 / 优先级
│   │   └── cfg_check.h             依赖校验 + 参数体检（15 道 #error 闸门）
│   └── common/                 ※ 横切·工具（3 文件 / 127 行）
│       ├── ringbuf.h / ringbuf.c       无锁环形缓冲（ISR 写 / 任务读，容量 256）
│       └── log.h                       日志级别（配合 CFG_LOG_LEVEL）
│
├── Core/                       CubeMX 生成的初始化与启动代码
│   ├── Src/usart.c / dma.c         ★ 串口章节新增：USART1 + DMA2_Stream2 初始化
│   ├── Src/freertos.c              ★ 唯一被手改的业务挂载点（USER CODE 段 2 行）
│   ├── Src/stm32f4xx_it.c          ★ 串口章节新增 USART1_IRQHandler 的 IDLE 判帧入口
│   ├── Src/main.c                  时钟、外设、GPIO(PG14)；串口章节新增 2 行 MX 调用
│   └── Inc/stm32f4xx_hal_conf.h    串口章节打开 HAL_UART_MODULE_ENABLED
│
├── Drivers/                    STM32F4 HAL 库 + CMSIS（串口章节补入 stm32f4xx_hal_uart.c/.h）
├── Middlewares/                FreeRTOS 内核（CubeMX 生成，未改动）
├── cmake/stm32cubemx/          CubeMX 生成的 CMake 目标（串口章节登记 usart.c / dma.c / hal_uart.c）
│
├── docs/                       ★ 中文文档（Markdown + 同名单文件网页版）+ 配套课件
│   ├── README.md                   文档索引与按角色的阅读路径
│   ├── architecture/               四层架构解析（最简 LED 版 + 串口控灯版）
│   ├── software-engineering/       配置体系与两级开关 / 串口三件套 / 闪烁状态机与契约式注释
│   ├── tutorials/                  t01 从零搭建四层 LED 闪烁、t02 从零给工程接串口控灯
│   ├── reference/                  引脚与符号速查表
│   ├── test/                       LED 链用例、串口链用例
│   ├── teaching/                  配套课件 pptx（本工程对应 1.3）
│   └── build_docs.py              Markdown → HTML 渲染器
│
├── CMakeLists.txt              构建入口（file(GLOB) 分四层收编 user，新增文件无需改脚本）
├── CMakePresets.json            Debug / Release 预设
├── STM32F407xx_FLASH.ld        链接脚本
├── startup_stm32f407xx.s       启动文件
├── IOT_M4_L4.ioc               CubeMX 工程文件（PG14 / TIM6 / CMSIS-RTOS2 / USART1+DMA）
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
| 整条串口链砍掉 | `MOD_UART1_ENABLE` 改 0 | `cfg_switch.h` | `1` |
| 整条 LED 链砍掉 | `MOD_LED_ENABLE` 改 0（**不能连 APP_BLINK 一起关**） | `cfg_switch.h` | `1` |
| 只裁某一层 | 该层子开关末尾的 `1` 改 `0` | `cfg_switch.h` | `(MOD_XXX_ENABLE && 1)` |

改完**必须重新编译**才生效（这是编译期配置，不是在线可调参数）。

> ⚠️ **三个容易踩的坑**
> 1. `*_TASK_STACK_SIZE` 的单位是**字节**，不是"字"。填 256 以为省 RAM，实际只分到 256 字节 → 栈溢出会**静默写坏 FreeRTOS 堆**（不报错、不进 HardFault），现象是"跑几行后整机没反应"。
> 2. **关 LED 链时不能把 `APP_BLINK_ENABLE` 关掉**：串口命令要靠闪烁状态机点灯，`cfg_check.h` 会当场 `#error` 拦下。正确做法是关 `APP_LED_ENABLE`（不自动闪烁）而保留状态机，让串口接管。
> 3. **关到"没任务可建"会被拦**：`APP_LED_ENABLE` 与 `APP_UART1_ENABLE` 都关、而 `APP_TASK_ENABLE` 仍为 1 → `#error APP_TASK_ENABLE requires APP_LED_ENABLE or APP_UART1_ENABLE`。

---

## 四层架构一览

命令**只能从上往下传**，下层永远不知道上层存在。

| 层 | 目录 | 文件 | 职责 | 关键符号 |
|---|---|---|---|---|
| ① app | `user/app/` | `app_led.c` / `app_blink.c` / `app_cmd.c` / `app_task.c` | 业务："闪多快"；闪烁状态机；"一行命令 → 一个动作"；任务集中创建 | `app_led_init` / `APP_BLINK_process` / `APP_BLINK_On|Off|IsOn` / `app_cmd_task` / `app_taskCreate` |
| ② platform | `user/platform/` | `mod_uart1.c` | 统一入口：给上层一套与硬件无关的收/发名字，也是将来承载协议与 MQTT 的位置 | `MOD_UART1_Init` / `MOD_UART1_Read` / `MOD_UART1_SendString` |
| ③ network | `user/network/` | `net_uart.c` | 字节流：DMA 循环接收 + IDLE 判帧 + 环形缓冲（除感知层外唯一碰 HAL 的一层） | `net_uart_init` / `net_uart_rx_idle_handler` / `net_uart_read` / `net_uart_send` |
| ④ perception | `user/perception/` | `per_led.c` / `per_gpio.c` | 门卫（编号→动作）+ 硬件描述表 `s_led_hw[]` + GPIO 读写 | `PER_LED_On|Off|IsOn|Toggle` / `per_gpio_write` / `per_gpio_is_on` |
| ※ cfg | `user/cfg/` | `module_cfg.h` + 4 子文件 | 配置：开关与参数；编译期校验（只被读，不被调） | `MOD_LED_ENABLE` / `MOD_UART1_ENABLE` / `APP_LED1_BLINK_MS` … |
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

每层有**自己独立的 LED 编号枚举**（`APP_BLINK_LED_t` / `PER_LED_t` / `PER_GPIO_LED_t`），末位都是哨兵量 `XXX_LED_COUNT`——既作数组长度，也作越界判断（本例 = 1）。

### 两条链，一个合流点

```text
LED 链（两跳）    app_led → APP_BLINK_* → PER_LED_* → per_gpio_* → HAL
串口链（三跳）    app_cmd → MOD_UART1_* → net_uart_* → HAL

  ★ 合流点：app_cmd 的每条命令都落到 APP_BLINK_*（同在 app 层），
    由它先让灯"退出闪烁"再点灯 —— 手动与闪烁绝不抢同一颗灯。
```

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

### 启动流程（从复位到能控灯）

```text
复位 → startup_stm32f407xx.s → main()
  → HAL_Init()（HAL 时基切到 TIM6）
  → SystemClock_Config()（168 MHz）
  → MX_GPIO_Init() / MX_DMA_Init() / MX_USART1_UART_Init()    ← 引脚与波特率
  → osKernelInitialize() → MX_FREERTOS_Init()
       └─ app_taskCreate()        ★ 唯一挂载点，就这一行
            ├─ osThreadNew(app_led_task, …)   灯：初始化引脚 + 设 1Hz
            └─ osThreadNew(app_cmd_task, …)   串口：拉起 DMA 接收 + 快闪 3 次自检
  → osKernelStart()  →  调度器接管  →  两个任务各自跑起来
```

---

## 用户代码清单（21 个文件 / 2267 行）

> 行数含契约式注释；去掉注释后的代码量约为其三分之一。

| 层 | 文件 | 行数 | 说明 |
|---|---|---|---|
| ※ cfg | `module_cfg.h` | 50 | 总入口，只做按序包含 |
| ※ cfg | `cfg_switch.h` | 104 | 两级开关（10 个）+ 四个裁剪场景说明 |
| ※ cfg | `cfg_print.h` | 41 | 打印总闸（业务回复直出，保持 0） |
| ※ cfg | `cfg_param.h` | 67 | 11 个参数 + 栈单位与缓冲大小说明 |
| ※ cfg | `cfg_check.h` | 130 | 7 条依赖校验 + 8 条参数体检 |
| ④ perception | `Inc/per_gpio.h` | 101 | 接口契约：枚举 + 四个动作声明 |
| ④ perception | `Src/per_gpio.c` | 165 | `s_led_hw[]` 表 + init / write / is_on / toggle |
| ④ perception | `Inc/per_led.h` | 106 | 接口契约（门卫） |
| ④ perception | `Src/per_led.c` | 119 | 编号翻译 + 越界拦截的薄转发 |
| ③ network | `Inc/net_uart.h` | 124 | 接口契约（串口三件套） |
| ③ network | `Src/net_uart.c` | 206 | DMA 循环 + IDLE 判帧 + ringbuf + 错误自愈 |
| ② platform | `Inc/mod_uart1.h` | 76 | 接口契约（统一入口） |
| ② platform | `Src/mod_uart1.c` | 54 | 五个函数，各一行转发 |
| ① app | `Inc/app_blink.h` | 124 | 接口契约（状态机，含 On/Off/IsOn） |
| ① app | `Src/app_blink.c` | 186 | 闪烁状态机 + 手动接管 |
| ① app | `Inc/app_led.h` | 53 | 接口契约 |
| ① app | `Src/app_led.c` | 95 | 闪烁业务 |
| ① app | `Inc/app_cmd.h` | 50 | 接口契约（串口命令） |
| ① app | `Src/app_cmd.c` | 242 | 命令解析 + 回显 + 上电自检快闪 |
| ① app | `Inc/app_task.h` | 47 | 接口契约（两个任务） |
| ① app | `Src/app_task.c` | 127 | 两个任务集中创建 |
| — | **合计** | **2267** | |

另有：

- **横切 common 3 文件 / 127 行**：`ringbuf.h`（28）+ `ringbuf.c`（56）+ `log.h`（43）；
- **8 个 `.gitkeep`**：四层的 `Inc/` 与 `Src/` 各一个；
- **Core 三处改动 + 构建三行**：见下节。

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

### 5. 编译期闸门：16 道

| 位置 | 数量 | 拦什么 |
|---|---|---|
| `cfg_check.h` · 依赖校验 | 7 | 悬挂配置（如 APP_UART1 开而 APP_BLINK 关）、串口链三层断档、两条链都关而 APP_TASK 仍开 |
| `cfg_check.h` · 参数体检 | 8 | 节拍 ≥1、半周期是节拍整数倍且 ≥2×节拍、两个任务栈 ≥512 字节、两个优先级 1~55、行缓冲 ≥8 |
| `cfg_print.h` · 打印链 | 1 | 打印总闸拨 1 却没有串口链 |

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

---

## 资源占用

以 `Debug` 预设、arm-none-eabi-gcc 14.3.1 实测（**50/50 目标，0 warning / 0 error**）：

| 形态 | FLASH | RAM |
|---|---|---|
| **默认全开**（闪烁 + 串口控灯） | **31228 B** / 512 KB（5.96%） | **21208 B** / 128 KB（16.18%） |
| 关掉整条串口链（`MOD_UART1_ENABLE=0`） | 26760 B（5.10%） | 20680 B（15.78%） |
| 只裁串口应用层（`APP_UART1_ENABLE=0`） | 28492 B（5.43%） | 21208 B（16.18%） |
| 对照：v2.1b 纯闪烁工程 | 20836 B（3.97%） | 20512 B（15.65%） |

两点值得知道：

- **基准体积的用法**：只想改注释时，编译后体积**应当完全不变**——体积一变，说明动到了真实代码。
- **未用代码会被自动回收**：只裁串口应用层时，`mod_uart1` 与 `net_uart` 仍在编译列表里，但没人调用，被 `-Wl,--gc-sections` 整段裁掉。

---

## 文档

全部为中文，正文在 `docs/` 下；每篇 `.md` 都有**同名单文件网页版**（样式内嵌、无外部依赖，可直接双击打开或打印成 PDF）。

| 分类 | 文档 | 内容 |
|---|---|---|
| 索引 | [`docs/README.md`](docs/README.md) | 分类导航 + 按角色的阅读路径 |
| architecture | [四层架构解析 · 串口控灯例程](docs/architecture/四层架构解析-串口控灯例程-1.0.md) | **本册主文档**：两条链与合流点、14 跳旅程、感知层扩接口、互斥设计、实测数据、与五层对照 |
| architecture | [四层架构解析 · 最简 LED 例程](docs/architecture/四层架构解析-最简LED例程-1.0.md) | 上一版（v2.1b）的入门主文档 |
| software-engineering | [串口三件套 · DMA 循环接收与 IDLE 判帧](docs/software-engineering/串口三件套-DMA循环接收与IDLE判帧-1.0.md) | 判帧数学、环形缓冲、ISR 纪律、出错自愈、缓冲大小怎么算、坑清单 |
| software-engineering | [module_cfg 配置体系与两级开关](docs/software-engineering/module_cfg配置体系与两级开关-1.0.md) | 一拆四、包含顺序、16 道闸门、四个裁剪场景、MX 调用点纪律 |
| software-engineering | [闪烁状态机与契约式注释](docs/software-engineering/闪烁状态机与契约式注释-1.0.md) | 状态机三原则、500 ms 逐拍推演、五联表模板 |
| tutorials | [t02 · 从零给工程接串口控灯](docs/tutorials/t02-从零给工程接串口控灯.md) | Step 0~9 手写顺序，每步含"目标 / 做法 / 自检" |
| tutorials | [t01 · 从零搭建四层 LED 闪烁](docs/tutorials/t01-从零搭建四层LED闪烁.md) | 前置练习（先会闪灯，再接串口） |
| reference | [引脚与符号速查表](docs/reference/引脚与符号速查表.md) | 引脚 / 时基 / 21 个文件 / 30 个函数 / 三条链 / 命令表 / 宏与枚举 / 四档体积 |
| test | [03 · 上板实测操作卡](docs/test/03-上板实测操作卡.md) | **照做清单**：接线 → 烧录 → 快闪自检 → 五条命令逐条验 → 边界/兼容 → 记录表 → 故障速查 |
| test | [02 · 串口链测试文档与用例](docs/test/02-UART链-测试文档和用例.md) | 分层 + 集成 + 11 条闸门（已实测）+ 3 组裁剪态（已实测）+ 健康度清单 |
| test | [01 · LED 链测试文档与用例](docs/test/01-LED链-测试文档和用例.md) | 上一版的闪烁用例 |
| teaching | `docs/teaching/*.pptx` | 培训课件：1.1 架构与运维职责、1.2 开发环境搭建、**1.3 构架搭建与分层与 LED 灯控制（本工程对应）**、1.4 DHT11 联动、2.1~5.1 |

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

### 改了 `cfg_param.h` 但没生效

配置是**编译期**的：改完必须重新编译并重新烧录，不是在线可调的。

### 报 `#error "..."` 编译不过

这是**设计好的**。16 道闸门会在编译期拦下配置矛盾（比如关了闪烁状态机却还开着串口控灯）。读 `#error` 后面的文字，它会告诉你是哪两个宏冲突。

### 换了文件夹名，编译产物名字变了

`CMakeLists.txt` 用 `get_filename_component(... NAME)` 自动取文件夹名作为工程名。这是有意设计——方便一个模板复制成多个工程。

---

## 与 v2.1b 的差异

`v2.1b` 是会闪灯的四层工程（15 文件 / 1091 行），`v2.1c` 在它上面接串口：

| 改动类型 | 内容 |
|---|---|
| **新增**（user） | 6 个文件 / **748 行**：`net_uart.h/.c`（124+206=330）、`mod_uart1.h/.c`（76+54=130）、`app_cmd.h/.c`（50+238=288） |
| **改动**（user） | 感知层补 `write/is_on` + `On/Off/IsOn`（+195）、应用层状态机补手动接管（+94）、`app_task.c/.h` 加串口任务（+40）、cfg 五件套（+95，其中 `cfg_print.h` −1） |
| **合计**（user） | 15 文件 / 1091 行 → 21 文件 / **2267 行**（+6 文件 / **+1176 行** = 新增 748 + 补齐 428） |
| **新增**（Core） | `usart.c` / `dma.c` / `usart.h` / `dma.h`；`stm32f4xx_it.c` 的 `USART1_IRQHandler` |
| **改动**（Core） | `main.c` 加 `MX_DMA_Init()` + `MX_USART1_UART_Init()`（含 include）共 4 行；`stm32f4xx_hal_conf.h` 开 `HAL_UART_MODULE_ENABLED`；`.ioc` 配 USART1 + DMA + NVIC + PA9/PA10 |
| **改动**（构建） | `cmake/stm32cubemx/CMakeLists.txt` 登记 `usart.c` / `dma.c` / `stm32f4xx_hal_uart.c`；`Drivers/` 补入 HAL UART 驱动 |
| **未动** | `app_led.c/h`、`per_gpio.c/h` 的既有函数、`freertos.c` 的两行挂载（一个字没改） |

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

串口章节新增的 `stm32f4xx_it.c` 挂载点同样写在 USER CODE 段内：

```c
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */
  if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE)) {
      __HAL_UART_CLEAR_IDLEFLAG(&huart1);
      net_uart_rx_idle_handler();   /* 网络层：判帧 + 搬进环形缓冲 */
  }
  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
}
```

---

## 与五层参考工程的关系

本工程是**四层**实现；`src/refer/` 下有**五层**对照版本，两者讲同一件事、代码可互相对照：

| 五层（refer） | 四层（本工程） | 说明 |
|---|---|---|
| L1_app（`app_uart` / `app_led` / `app_task`） | `user/app`（`app_cmd` / `app_led` / `app_blink` / `app_task`） | 命令解析与业务不变 |
| L2_svc（`svc_led` 闪烁状态机） | `user/app`（`app_blink`） | 状态机收进应用层 |
| L3_plat（`plat_uart` / `plat_led`） | `user/platform`（`mod_uart1`）+ `user/perception`（`per_led`） | 串口入口进平台层，LED 门卫进感知层 |
| L4_bsp（`bsp_uart` / `bsp_led`） | `user/network`（`net_uart`）+ `user/perception`（`per_gpio`） | 串口驱动进网络层，GPIO 驱动进感知层 |
| L5_com（`module_cfg` + 4 子文件 + `ring_buffer`） | `user/cfg` + `user/common` | 配置体系原样，环形缓冲移入横切 `common/` |

**两边口径怎么统一**：四层（感知 / 网络 / 平台 / 应用）既是**责任口径**（出问题时定界"找谁"），也是本工程的**代码口径**（写码时定"文件放哪"）。

**不变的纪律**：命令只许上层 → 下层单向走；cfg 只被读不被调；错误"越靠上越能上报、越靠下越静默兜底"。

---

## 参考资料与致谢

- **`src/refer/pro_5L_v2.2_uart_led`** —— 串口控灯的五层对照版，本工程的 `net_uart`（对应其 `bsp_uart`）、`mod_uart1`（对应 `plat_uart`）、`app_cmd`（对应 `app_uart`）均与之对齐。
- **`src/refer/pro_5L_v2.1_ledblink`** —— 纯闪烁的五层对照版。
- **`src/proL4_p1.3_blink_v2.1b`** —— 本工程的上一版（纯闪烁四层）。
- **H743ZI_V6.x** —— 配置体系（总入口 + 开关 / 打印 / 参数 / 校验）与契约式注释规范的来源例程；本工程是它的**最简教学版**。
- ST 官方 **STM32CubeF4** HAL 库与 **FreeRTOS** 内核。

---

## 许可证

本项目 `user/` 下的用户代码采用 [MIT 许可证](LICENSE)。

> 两点说明：`Drivers/`（STM32 HAL 库）、`Middlewares/`（FreeRTOS）等目录中的第三方代码遵循其各自原有的许可证，不在 MIT 许可范围内；本工程目前未附带 `LICENSE` 文本文件，需要时可直接使用 `src/refer/` 下的同名文件。
