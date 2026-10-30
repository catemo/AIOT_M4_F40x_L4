# user/ —— 四层源码架构（串口控灯 + DHT11 联动 + WiFi-MQTT 上云 + EEPROM/FLASH 存储 + IWDG 看门狗 + 光敏传感器 + OLED 显示）

本工程为**四层框架**：应用层 app、平台层 platform、网络层 network、感知层 perception。
这四层同时回答两个问题：**讲责任**（问题出在哪一层、找谁）和**写代码**（文件放哪个目录、谁可以调谁）。

## 目录

```
user/
├── app/            应用层：业务规则、命令解析、阈值判定、上报节拍、任务编排
│   ├── Inc/  Src/  app_led（闪烁业务）/ app_blink（闪烁状态机）
│                   app_cmd（串口命令）/ app_task（任务创建，共 7 个）
│                   app_dht11（周期采集 + 快照）/ app_link（阈值联动）
│                   app_mqtt（连接分段 / 周期上报 / 云端命令响应）
│                   app_param（20 字节配置块：三关校验 + 读回）/ app_logger（8 B 环形日志）
│                   app_watchdog（喂狗任务 + 复位原因上报 + HANG 卡死演示）
│                   app_light（500 ms 采集 + 三档分级 + 控灯 + 打印）
│                   app_oled（2.9 四行布局 + 整行定宽覆盖 + osMutex 串行化写屏）
├── platform/       平台层：命令协议与统一入口、存储器件语义
│   ├── Inc/  Src/  mod_uart1（串口入口）/ mod_wifi（连网语义动作）
│                   mod_mqtt（四种报文 + 剩余长度编码 + 报文 ID）
│                   mod_eeprom（AT24C02：页拆分 + 写周期轮询）/ mod_flash（W25Q64：指令集 + 先擦后写）
│                   mod_oled（2.9 SSD1306：19 步初始化 + 8 页显存 + 6x8 字库实时放大 8x16）
├── network/        网络层：字节流、AT 会话、存储总线
│   ├── Inc/  Src/  net_uart（DMA + IDLE + 环形缓冲，★ 实例表管 2 个串口）
│                   net_esp8266（AT 文本会话 + +IPD 二进制分流）
│                   net_i2c（I2C1 时序：地址帧/ACK/超时/九时钟卡死恢复）
│                   net_spi（SPI1 全双工 + 片选包络）
│                   net_i2c_sw（2.9 软件 I2C：PC4=SCL / PC5=SDA，半周期 2 µs，不检测 ACK）
├── perception/     感知层：引脚表与时序（★ 上云与存储章节零新增）
│   ├── Inc/  Src/  per_gpio（灯引脚表）/ per_led（编号门卫）
│                   per_delay（TIM2 微秒底座）/ per_dht11（单总线协议翻译）
│                   per_iwdg（独立看门狗：KR 钥匙 + 复位标志先读后清，片上外设不占引脚）
│                   per_light（PA5/ADC1_IN5 查询四步采集）
├── cfg/            横切：module_cfg.h 总开关（纯头文件，只放开关与参数）
└── common/         横切：ringbuf 环形缓冲、log 日志（不占层，任何层可用）
```

## 九条链，三个合流点

```text
LED 链（两跳）    app_led → APP_BLINK_* → PER_LED_* → per_gpio_* → HAL
                  ①app     ①app           ④perception  ④perception

串口链（三跳）    app_cmd → MOD_UART1_* → net_uart_* → HAL
                  ①app     ②platform     ③network

DHT11 链（两跳）  app_dht11 → PER_DHT11_* → per_delay_* → HAL
                  ①app        ④perception   ④perception

上云链（五跳）    app_mqtt → MOD_MQTT_* → MOD_WIFI_* → net_esp8266_* → net_uart(USART3) → HAL
                  ①app      ②platform    ②platform   ③network        ③network

参数链（三跳，2.3） app_param → mod_eeprom → net_i2c(I2C1: PB8/PB9) → HAL
                  ①app        ②platform     ③network

日志链（三跳，2.3） app_logger → mod_flash → net_spi(SPI1: PB3/4/5 + PG10) → HAL
                  ①app         ②platform    ③network

看门狗链（两跳，2.4） app_watchdog → PER_IWDG_* → HAL（IWDG 片上外设，不占引脚）
                  ①app            ④perception

光敏链（两跳，2.8） app_light → PER_LIGHT_* → HAL（ADC1_IN5 / PA5）
                   ①app        ④perception

OLED 链（三跳，2.9） app_oled → MOD_OLED_* → NET_I2CSW_* → HAL（PC4=SCL / PC5=SDA）
                   ①app        ②platform    ③network

  合流点① APP_BLINK_*（同在 ①app 层）
      串口命令与自动闪烁在此汇合：命令先让灯"退出闪烁"再点灯 —— 手动与闪烁绝不抢同一颗灯。

  合流点② per_led（④perception）
      温度联动与自动闪烁在此汇合：联动用【边沿触发】—— 只在跨越阈值那一帧写灯，
      平时把灯让给 1 Hz 闪烁。于是"闪烁"与"温度到了就亮"能同时成立。

  合流点③ 调试串口 MOD_UART1_*（②platform）
      DHT11 / 上云 / 参数 / 日志 / 看门狗五方都借它打印；
      打印总闸 UART1_PRINT_MASTER_ENABLE 一处可全关。

  ★ 依赖图：串口链 → LED 链（借 APP_BLINK_* 点灯）
             DHT11 链 → LED 链（借 PER_LED_* 点灯，APP_LINK 跨链）
             DHT11 链 → 串口链（借 MOD_UART1_* 打印）
             上云链  → LED 链（借 APP_BLINK/PER_LED 点灯）
             上云链  → DHT11 链（只读快照，可选）
             上云链  → 串口链（借 MOD_UART1_* 打日志）
             参数链  → 串口链（借 MOD_UART1_* 打日志）
             日志链  → 串口链（借 MOD_UART1_* 打日志）
             日志链  → DHT11 链（读快照，闸门强制）
             看门狗链 → 串口链（借 MOD_UART1_* 打启动行与 Alive）
             光敏链  → LED 链（借 APP_BLINK/PER_LED 点灯）
             光敏链  → 串口链（借 MOD_UART1_* 打印）
             DHT11 链 → OLED 链（借 APP_OLED_UpdateTempHum 刷 Temp/Hum 两行）
             光敏链  → OLED 链（借 APP_OLED_UpdateLight 刷 Light/Level 两行）
             串口链  → 看门狗链（HANG 命令置 volatile 标志，唯一反向）
    LED 链是九条里最底的一条 —— 所以"一键断 LED 链"在本工程【不成立】。
```

**九条链为什么层数不同**：层要有自己的知识才配单独存在。

| 链 | 层数 | 各层的知识 |
|---|---|---|
| 串口 | 3 | 字节流（network）/ 入口 + 协议位（platform）/ 命令语义（app） |
| LED | 2 | 引脚表 + 门卫（perception）/ 闪烁节奏 + 业务（app） |
| DHT11 | 2 | 单总线协议翻译 + 微秒底座（perception）/ 周期 + 阈值语义（app） |
| 上云 | **5** | AT 会话（network）/ 连网语义（platform）/ 报文格式（platform）/ 报什么（app） |
| 参数 | 3 | I2C 时序 + 卡死恢复（network）/ 页与写周期（platform）/ 配置块 + 三关校验（app） |
| 日志 | 3 | SPI 包络（network）/ W25Q 指令集（platform）/ 分区 + 环形决策（app） |
| 看门狗 | 2 | KR 钥匙 + 复位标志"先读后清"（perception）/ 喂狗节奏 + HANG 演示（app） |
| 光敏 | 2 | ADC 查询四步 + 电压换算（perception）/ 阈值分级 + 控灯 + 打印（app） |
| OLED | 3 | 软件 I2C 时序（network）/ SSD1306 命令与显存布局（platform）/ 四行排版 + 互斥锁（app） |

DHT11 中间没有第三种知识，硬塞一个 `mod_dht11` 只会多出几十行纯转发。
OLED 恰恰相反：I2C 起始/停止/第 9 个时钟是 network 的活，0x8D/0xAF/0xA4 这些"生死线"是平台层对 SSD1306 的知识，
"哪一行放什么、两个任务怎么排队"是应用层的事——**三跳没有一下就够的替代方案，也没有感知层可塞**（屏不是传感器）。
上云恰恰相反：它有四种互不重叠的知识，所以**平台层与网络层各两跳**不是冗余，而是必然——
换模组只改 `net_esp8266`，换云平台只改 `mod_mqtt` 的主题与 JSON。

## 依赖规则（红线）

- **只许上层 → 下层单向调用，不许回头、不许横向 include**；
- 例外：应用层可直达感知层（本地闭环，如按键点灯）；
- `cfg/` 只被读、不被调用；`common/` 被各层共用但自身不依赖任何层；
- **HAL 只出现在感知层与网络层**（网络层受 `HAL_UART_MODULE_ENABLE` 看管，只碰 `HAL_UART_*` / `HAL_DMA_*`）；
- 应用层只认 `MOD_UART1_*`，**禁止** include `net_uart.h`；
- **感知层不许打印**：自检结果只填结构体（如 `PER_DHT11_Diag_t`），"怎么表达"由应用层决定——
  否则感知层依赖上层，依赖方向倒转、分层直接破产。

## 三个"永远存在"的接口

MX 生成文件里的调用点不受本工程的开关控制，所以被它们直接调用的函数必须"永远存在"、关开关时变空壳：

| 函数 | 调用点 | 为什么 |
|---|---|---|
| `app_taskCreate()` | `Core/Src/freertos.c` | 关开关只该是配置操作，不该炸链接 |
| `net_uart_rx_idle_handler(id)` | `USART1_IRQHandler` / `USART3_IRQHandler`（it.c） | ISR 入口由 CubeMX 管理，工程开关关不掉它；`id` 越界或实例 `ready==0` 时静默返回 |
| `HAL_DMA_IRQHandler(&hdma_usart3_rx)` | `DMA1_Stream1_IRQHandler`（it.c） | 句柄在 `usart.c` 里无条件定义，只是没人启动它 |

> 判断标准：**只要调用点在 MX 生成文件里，被调用的函数就必须永远存在。**

## 串口链的三件套

1. **DMA 循环接收** —— 一次性配置，硬件自己转圈搬运，写满自动绕回，永不停止；
2. **IDLE 判帧** —— 总线一安静就进空闲中断，此刻把新到的字节切成一帧；
3. **环形缓冲**（`common/ringbuf`）—— 中断只管塞，任务有空了再来取，ISR 与任务解耦。

ISR 里只做"判帧 + 搬数据"这一件事：**不解析、不打印、不调任何 RTOS API**
（USART1 抢占优先级 = 5，与 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 恰好相等；本工程统一纪律：ISR 内不调任何 RTOS API）。

## DHT11 链的四道纪律

1. **微秒底座必须是硬件定时器** —— TIM2，APB1 定时器时钟 84 MHz → `PSC = 83` → 1 计数 = 1 µs。
   不用 DWT 的 `CYCCNT`（调试器下载后常常不计数）、不用 `HAL_Delay`（只有 ms 精度）。
   回绕靠无符号减法自动处理：`(now - start) < us`。
2. **关中断要成对，且所有失败路径汇到同一个出口** —— 40 位是 26~70 µs 的脉宽，
   被任务切换抢走几百 µs 就测错了。从"拉高起始信号"到"读完 40 位"之间 `__disable_irq()`；
   **所有失败路径一律 `goto exit`**，在 `exit` 里统一 `__enable_irq()`。
3. **18 ms 起始信号刻意放在关中断之前** —— 它不是时序敏感段，放外面能少关 18 ms 中断。
4. **输入方向配浮空，不配上拉** —— 上拉会与模块内已有的 4.7k 形成分压，高电平拉不到位。

失败时报 **`stage=1~6`**：1 = 完全没回应；2/3 = 响应只回一半；4/5 = 数据段异常；6 = 校验和错。
让"读不到数"这个笼统现象有 6 种具体答案，而不是笼统地"怀疑软件"。

## 上云链的四道纪律

1. **AT 等待一律 `osDelay`** —— `HAL_Delay` 是死等计数器、不让出 CPU；
   用它在 `CWJAP` 等 10 秒，1 Hz 闪烁会定住、看着像死机。
2. **两条缓冲物理隔离** —— AT 响应（文本行，512 B）与 `+IPD` 载荷（二进制，256 B 环形）分开；
   否则 MQTT 报文里出现的 `0x0A` 会被当成行尾。
3. **`+IPD` 必须能"下次再拼"** —— 一帧可能被拆成两次 IDLE 中断到达；
   数据没到齐就退出，绝不"先取到多少算多少"。
4. **断线只认独立成行的 `CLOSED`** —— 不能按子串匹配（报文正文里也可能出现这 6 个字节），
   也不用 `AT+CIPSTATUS`（它的回显会插进载荷流里）。

失败时先看日志里的 `resp=`：`0` = 模组没说话（接线/供电）；乱码 = 波特率；有内容 = 模组活着，
只是这条指令不认——三个方向互不重叠，比"连不上云"这种笼统描述好查得多。

## OLED 链的两道纪律（2.9）

1. **一条总线、两个任务 → 必须有互斥锁** —— DHT11 任务刷第 0~3 页、光敏任务刷第 4~7 页，
   都走同一条 PC4/PC5 软件 I2C。`app_oled` 用 `osMutexAcquire(..., 100 ms)` 把"一次四行更新"包成临界区，
   否则两次写屏的 Start/Stop 会交织，屏上出现半行乱码。
2. **整行定宽覆盖，不做"擦掉再写"** —— 每行按固定 16 字符宽度整块写满（尾部补空格），
   这样"31 → 9"这种位数变化不会留下残字；也不引 `printf`，手写 `put_u32_w` 把数字转成 6x8 字模。
   6x8 字库靠"纵向像素复制 2 次 + 左右补 1 空列 + 上下半页拆高低 4 位"实时放大成 8x16，省掉整张 8x16 字库。

三条"生死线"没写就全黑或只亮一半：电荷泵 `0x8D,0x14`、开显示 `0xAF`、按显存刷 `0xA4`。

## 三个刻意的设计

- **边沿触发**：`APP_LINK_ApplyTemp/ApplyHum` 只在跨越阈值那一帧写灯，平时 `return`。
  这样 LED1 的 1 Hz 闪烁与温度联动能共存。
- **失败路径不调 `app_link`**：读不到数就什么灯都不动（故障安全）。
  这让"传感器没接"和"联动逻辑写错"在现象上完全不同，好排查。
- **快照而不是队列**：`APP_DHT11_GetSnapshot()` 让上报任务"只读"采集结果，
  最差读到"上一组"，对 5 秒一次的上报完全够用，所以**不需要互斥量**；
  也避免了"为了发一条数据去催传感器再读一次"打乱 2 s 采集周期。

## 接入构建

`CMakeLists.txt` 用 `file(GLOB)` 分四层收编 `user/*/Src/*.c` 与 `common/*.c`，并加入六个 Inc 头路径；
`cmake/stm32cubemx/CMakeLists.txt` 里还要登记 CubeMX 生成的 `usart.c` / `dma.c` / `tim.c` / `i2c.c` / `spi.c` / `adc.c`
与 `stm32f4xx_hal_uart.c` / `stm32f4xx_hal_i2c.c` / `stm32f4xx_hal_spi.c` / `stm32f4xx_hal_iwdg.c`
/ `stm32f4xx_hal_adc.c` / `stm32f4xx_hal_adc_ex.c`。
⚠ `adc.c` 与 `usart.c` / `tim.c` 一样由 `cmake/stm32cubemx/CMakeLists.txt` 显式登记（`MX_Application_Src` + `stm32f4xx_hal_adc.c` / `stm32f4xx_hal_adc_ex.c`），`HAL_ADC_MODULE_ENABLED` 同样没有 USER CODE 区、需手动维护。
⚠ `Core/Inc/stm32f4xx_hal_conf.h` 的 `HAL_IWDG_MODULE_ENABLED` 所在位置**没有 USER CODE 区**，
CubeMX 重新生成会被冲掉——这是本工程少数必须手动维护的挂载点（完整清单见速查表 §三末）。

> ⚠ **cfg 的两级开关管不到 Core 层**：`usart.c` / `dma.c` / `tim.c` / `gpio.c` / `i2c.c` / `spi.c` 由 CMake
> 显式清单参与编译，把链全关掉，它们与 `HAL_UART_Init` / `HAL_I2C_Init` 依然在 .elf 里（已用 `nm` 验证）。
> 想省这部分体积得去 `.ioc` 里删外设重新生成。

⚠ OLED 的 PC4/PC5 是 `NET_I2CSW_Init()` 里自己配的推挽输出，**没有登记进 `.ioc`**，也不改 `Core/Src/gpio.c`
（与 `adc.c` 里 PA5 同一纪律）——CubeMX 重新生成不会把这两根线冲掉，但也不会自动帮你加上。

详见课件《1.3 项目实操-构架搭建与分层与 LED 灯控制》《1.4 项目实操-DHT11 传感器与 LED 灯联动》
《2.2 项目实操-WiFi-MQTT 上云》《2.3 项目实操-外部存储器 EEPROM/FLASH》《2.4 项目实操-看门狗》
《2.8 项目实操-光敏传感器实验》《2.9 项目实操-OLED 显示传感器数据》，
以及 `docs/tutorials/t02`~`t09` 与 `docs/teaching/2.3-外部存储器例程开发文档.md`。
