# user/ —— 四层源码架构（串口控灯版）

本工程为**四层框架**：应用层 app、平台层 platform、网络层 network、感知层 perception。
这四层同时回答两个问题：**讲责任**（问题出在哪一层、找谁）和**写代码**（文件放哪个目录、谁可以调谁）。

## 目录

```
user/
├── app/            应用层：业务规则、命令解析、任务编排
│   ├── Inc/  Src/  app_led（闪烁业务）/ app_blink（闪烁状态机）
│                   app_cmd（串口命令）/ app_task（任务创建）
├── platform/       平台层：命令协议与统一入口（mod_uart1）
├── network/        网络层：字节流收发（net_uart：DMA + IDLE 判帧 + 环形缓冲）
├── perception/     感知层：引脚表与时序（per_gpio / per_led）
├── cfg/            横切：module_cfg.h 总开关（纯头文件，只放开关与参数）
└── common/         横切：ringbuf 环形缓冲、log 日志（不占层，任何层可用）
```

## 两条链，一个合流点

```text
LED 链（两跳）    app_led → APP_BLINK_* → PER_LED_* → per_gpio_* → HAL
                  ①app     ①app           ④perception  ④perception

串口链（三跳）    app_cmd → MOD_UART1_* → net_uart_* → HAL
                  ①app     ②platform     ③network

  合流点：app_cmd 的每条命令最终都落到 APP_BLINK_*（都在 ①app 层内），
          由它先让灯"退出闪烁"再点灯 —— 手动与闪烁绝不抢同一颗灯。
```

## 依赖规则（红线）

- **只许上层 → 下层单向调用，不许回头、不许横向 include**；
- 例外：应用层可直达感知层（本地闭环，如按键点灯）；
- `cfg/` 只被读、不被调用；`common/` 被各层共用但自身不依赖任何层；
- **HAL 只出现在感知层与网络层**（网络层受 `HAL_UART_MODULE_ENABLE` 看管，只碰 `HAL_UART_*` / `HAL_DMA_*`）；
- 应用层只认 `MOD_UART1_*`，**禁止** include `net_uart.h`。

## 四条"永远存在"的接口

MX 生成文件里的调用点不受本工程的开关控制，所以被它们直接调用的函数必须"永远存在"、关开关时变空壳：

| 函数 | 调用点 | 为什么 |
|---|---|---|
| `app_taskCreate()` | `Core/Src/freertos.c` | 关开关只该是配置操作，不该炸链接 |
| `net_uart_rx_idle_handler()` | `Core/Src/stm32f4xx_it.c` | ISR 入口由 CubeMX 管理，工程开关关不掉它 |

## 串口链的三件套

1. **DMA 循环接收** —— 一次性配置，硬件自己转圈搬运，写满自动绕回，永不停止；
2. **IDLE 判帧** —— 总线一安静就进空闲中断，此刻把新到的字节切成一帧；
3. **环形缓冲**（`common/ringbuf`）—— 中断只管塞，任务有空了再来取，ISR 与任务解耦。

ISR 里只做"判帧 + 搬数据"这一件事：**不解析、不打印、不调任何 RTOS API**
（USART1 抢占优先级 = 5，与 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 恰好相等；本工程统一纪律：ISR 内不调任何 RTOS API）。

## 接入构建

`CMakeLists.txt` 用 `file(GLOB)` 分四层收编 `user/*/Src/*.c` 与 `common/*.c`，并加入六个 Inc 头路径；
`cmake/stm32cubemx/CMakeLists.txt` 里还要登记 CubeMX 生成的 `usart.c` / `dma.c` 与 `stm32f4xx_hal_uart.c`。
详见课件《1.3 项目实操-构架搭建与分层与 LED 灯控制》与 `docs/tutorials/t02`。
