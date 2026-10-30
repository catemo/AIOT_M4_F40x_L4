/**
 ******************************************************************************
 * @file    per_dht11.c
 * @brief   感知层（perception）—— DHT11 单总线驱动实现（PG9 + TIM2 微秒计时）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 per_dht11.h，
 *  本文件的注释侧重"代码是怎么实现的"与"每个数字为什么是它"。
 *
 *  ▍本文件是本册唯一的"协议翻译器"：
 *    上层说"读一次温湿度"，本文件负责把这句话翻译成 PG9 上的一串电平跳变，
 *    再把 DHT11 回的 40 个脉宽翻译成 4 个字节。协议细节全部锁在这里，
 *    换传感器（比如换成 DHT22）只改本文件，上层一个字不动。
 *
 *  ▍时序参数为什么写死在 .c 里而不放进 cfg：
 *    下面这几个数是 DHT11 数据手册规定的协议常量，不是"可以调的业务参数"——
 *    调了它协议就不成立。可调的东西（采集周期、阈值）才放进 cfg_param.h。
 *    这条界线是本工程参数管理的一贯口径。
 ******************************************************************************
 */
#include "module_cfg.h"
#if PER_DHT11_ENABLE

#include "per_dht11.h"
#include "per_delay.h"     /* 同层：TIM2 微秒计时底座（1 tick = 1 µs） */
#include "main.h"          /* DHT11_Pin / DHT11_GPIO_Port（PG9，CubeMX 生成） */
#include "cmsis_os.h"      /* 起始信号 18 ms 等待：调度器在跑就用 osDelay 让出 CPU */

/* ========== 时序参数（微秒，DHT11 协议常量：改了协议就不成立） ==========
 * 这几个数字是怎么来的（以最关键的 200 为例，见 per_dht11.h 的 stage 说明）：
 *   数据手册说响应低电平典型值 80 µs，留 2 倍余量并覆盖最大响应延迟 ——
 *   设 100 µs 会把正常模块误判成超时；设 1 ms 则关中断太久，串口会丢字节。
 *   200 是"既不放过故障、又不误伤正常"的平衡点。 */
#define DHT11_START_LOW_MS      18U     /* 起始信号低电平 ≥18 ms */
#define DHT11_START_HIGH_US     30U     /* 起始信号高电平 20~40 µs */
#define DHT11_RESPONSE_TIMEOUT  200U    /* 响应超时阈值（µs） */
#define DHT11_BIT_LOW_TIMEOUT   100U    /* 数据位：等低电平超时（µs） */
#define DHT11_BIT_HIGH_TIMEOUT  120U    /* 数据位：测高电平脉宽超时（µs） */
#define DHT11_BIT_THRESHOLD_US  40U     /* 数据位 0/1 判定阈值：>40 µs 判 1 */

/* ========== 失败阶段记录（应用层打印的 stage 就是它） ==========
 * 声明为 static 的原因与代价：
 *   - 好处：感知层不需要为此多暴露一个"写入接口"，对外只有读取接口
 *   - 代价：本驱动因此【不是并发安全】的——两个任务同时读 DHT11 会互相覆盖
 *     stage。本工程只有一个采集任务读它，且单总线本来也不允许并发访问，
 *     所以这个代价可接受；若将来要多任务访问，需改用互斥或按任务存。 */
static uint8_t s_fail_stage = (uint8_t)PER_DHT11_FAIL_NONE;

/* ========== 引脚操作（全部内联：这些函数在时序关键段里被反复调用） ========== */
static inline void dht11_pin_low(void)
{
    HAL_GPIO_WritePin(DHT11_GPIO_Port, DHT11_Pin, GPIO_PIN_RESET);
}

static inline void dht11_pin_high(void)
{
    HAL_GPIO_WritePin(DHT11_GPIO_Port, DHT11_Pin, GPIO_PIN_SET);
}

static inline uint8_t dht11_pin_read(void)
{
    return (HAL_GPIO_ReadPin(DHT11_GPIO_Port, DHT11_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/*----------------------------------------------------------------------------
 * 内部函数：把 PG9 切成推挽输出（主机驱动总线时用）
 * 注意事项：单总线的方向切换是"每次通信都要做好几遍"的动作：
 *           主机发起始信号时要输出，读响应/数据时要输入，
 *           读完还要切回输出把总线拉成空闲高电平。
 *----------------------------------------------------------------------------*/
static void dht11_pin_output(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin   = DHT11_Pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;   /* 高速：边沿要陡，脉宽才测得准 */
    HAL_GPIO_Init(DHT11_GPIO_Port, &gpio);
}

/*----------------------------------------------------------------------------
 * 内部函数：把 PG9 切成浮空输入（读 DHT11 时用）
 * 注意事项：① 浮空输入而不是上拉输入——电平由【模块上的外部上拉】决定。
 *              若这里配成内部上拉，模块拉低时会与内部上拉形成分压，
 *              低电平可能到不了阈值，表现为数据位全判成 1
 *           ② 切模式时 ODR 不动：输出态下写过的最后一个电平会被保持，
 *              所以切回输出后总线电平是可预期的（空闲高）
 *----------------------------------------------------------------------------*/
static void dht11_pin_input(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin  = DHT11_Pin;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DHT11_GPIO_Port, &gpio);
}

/*----------------------------------------------------------------------------
 * 内部函数：起始信号——拉低总线并保持 ≥18 ms（DHT11_START_LOW_MS）
 *
 *  ★ 为什么这里用 osDelay 而不是 per_delay_ms 忙等（2026-10-30 修正）：
 *    旧注释说"osDelay 中途被切走会破坏时序"——这个理由是错的。
 *    拉低这个动作只写一次 ODR 寄存器，之后引脚电平由硬件保持：
 *    CPU 睡 20 ms 期间没有任何代码会碰 PG9（本驱动非并发安全、
 *    只有采集任务调用，见 s_fail_stage 的注释），总线就稳稳低着。
 *    "必须连续占用 CPU"的只有后面关中断的 4~5 ms 时序关键段，
 *    18 ms 的低电平保持根本不需要 CPU 醒着——忙等是纯浪费
 *    （每 2 s 白占 18 ms 时间片份额）。
 *
 *  实现分两路：
 *    调度器已运行（正常路径，本驱动的所有调用方都是任务）→ osDelay 睡过去；
 *    调度器未运行（防御路径，理论上不存在）→ 回退 per_delay_ms 忙等，
 *    因为 osDelay 在调度器启动前行为未定义，不能赌。
 *----------------------------------------------------------------------------*/
static void dht11_start_low_wait(void)
{
    dht11_pin_low();   /* 一次写入，之后电平由 ODR 硬件保持，CPU 睡不睡都一样 */

    if (osKernelGetState() == osKernelRunning)
    {
        /* +2 ms：DHT11 只要求 ≥18 ms，osDelay(20) 保证下限 ≥19 tick，余量充分 */
        osDelay(DHT11_START_LOW_MS + 2U);
    }
    else
    {
        per_delay_ms(DHT11_START_LOW_MS);
    }
}

/*----------------------------------------------------------------------------
 * 内部函数：等引脚变成指定电平，带超时
 * 返回值  ：0 = 等到了；1 = 超时
 * 注意事项：① 超时判断用 (now - start) 无符号减法，TIM2 回绕时依然正确
 *           ② 本函数是"忙等"：它只能在关中断的时序关键段里被调用
 *----------------------------------------------------------------------------*/
static uint8_t dht11_wait_level(uint8_t level, uint32_t timeout_us)
{
    uint32_t start = per_delay_getTickUs();

    while (dht11_pin_read() != level)
    {
        if ((per_delay_getTickUs() - start) >= timeout_us)
        {
            return 1U;   /* 超时 */
        }
    }
    return 0U;   /* 等到了 */
}

/*----------------------------------------------------------------------------
 * 内部函数：测当前高电平持续了多久（µs）
 * 返回值  ：高电平脉宽（µs）；超时时返回 0xFFFFFFFF 作为"测量失败"标记
 * 注意事项：为什么用 0xFFFFFFFF 当哨兵而不是返回 0：
 *           高电平短到 0 µs 是合法测量结果（虽然本例不会），
 *           用 0 当失败标记会让"测到 0"和"没测到"混在一起
 *----------------------------------------------------------------------------*/
static uint32_t dht11_measure_high_us(void)
{
    uint32_t start = per_delay_getTickUs();

    while (dht11_pin_read() == 1U)
    {
        if ((per_delay_getTickUs() - start) >= DHT11_BIT_HIGH_TIMEOUT)
        {
            return 0xFFFFFFFFU;   /* 超时：总线卡在高电平，说明时序断了 */
        }
    }
    return (per_delay_getTickUs() - start);
}

/*----------------------------------------------------------------------------
 * 函数功能：DHT11 初始化（把总线恢复成空闲态：输出 + 高电平）
 * 调用处  ：应用层 app_dht11.c            【接口契约见 per_dht11.h】
 * 注意事项：MX_GPIO_Init 已经配过一次，这里再配是为了"不依赖 .ioc 的初始电平"
 *----------------------------------------------------------------------------*/
void PER_DHT11_Init(void)
{
    dht11_pin_output();
    dht11_pin_high();
}

/*----------------------------------------------------------------------------
 * 函数功能：读取一次温湿度（40 位 + 校验和）
 * 调用处  ：应用层 app_dht11.c            【接口契约见 per_dht11.h】
 * 注意事项：① ★ 关中断必须成对，且所有退出路径都汇到同一个出口：
 *              本函数用 goto exit 把 6 条失败路径全部引到同一个收尾段，
 *              收尾段里先恢复总线、再 __enable_irq()。
 *              若某条路径直接 return，中断就永远关着——系统看起来在跑，
 *              但外设全哑，是本驱动最阴的 bug，排查成本极高
 *           ② 18 ms 的起始低电平在关中断【之前】：那段时间不需要精确计时，
 *              没必要为了它多关 18 ms 的中断（能少关就少关）
 *----------------------------------------------------------------------------*/
PER_DHT11_Status_t PER_DHT11_Read(PER_DHT11_Data_t *data)
{
    uint8_t  buf[5];
    uint8_t  i;
    uint8_t  bit;
    uint32_t high_us;
    uint8_t  checksum;
    PER_DHT11_Status_t status = PER_DHT11_OK;

    if (data == NULL)
    {
        return PER_DHT11_ERR_TIMEOUT;
    }

    s_fail_stage = (uint8_t)PER_DHT11_FAIL_NONE;   /* 每次读取前复位失败阶段 */

    /* ---- 步骤 0：确保总线空闲（输出高），给 100 µs 稳定时间 ---- */
    dht11_pin_output();
    dht11_pin_high();
    per_delay_us(100U);

    /* ---- 步骤 1：主机拉低 ≥18 ms（在关中断之前，且用 osDelay 让出 CPU：
     *      引脚电平由 ODR 硬件保持，见 dht11_start_low_wait 的说明） ---- */
    dht11_start_low_wait();

    /* ================================================================
     *  时序关键段开始
     * ================================================================ */
    __disable_irq();

    dht11_pin_high();                     /* 拉高 20~40 µs 后交出总线 */
    per_delay_us(DHT11_START_HIGH_US);

    dht11_pin_input();                    /* 切输入，准备接收 */

    /* ---- 步骤 2：等 DHT11 的三段响应（低 80 → 高 80 → 低 80） ---- */
    if (dht11_wait_level(0U, DHT11_RESPONSE_TIMEOUT) != 0U)
    {
        s_fail_stage = (uint8_t)PER_DHT11_FAIL_RESP_LOW;
        status = PER_DHT11_ERR_TIMEOUT;
        goto exit;
    }
    if (dht11_wait_level(1U, DHT11_RESPONSE_TIMEOUT) != 0U)
    {
        s_fail_stage = (uint8_t)PER_DHT11_FAIL_RESP_HIGH;
        status = PER_DHT11_ERR_TIMEOUT;
        goto exit;
    }
    if (dht11_wait_level(0U, DHT11_RESPONSE_TIMEOUT) != 0U)
    {
        s_fail_stage = (uint8_t)PER_DHT11_FAIL_RESP_END;
        status = PER_DHT11_ERR_TIMEOUT;
        goto exit;
    }

    /* ---- 步骤 3：收 40 位（每位：先等 50 µs 低，再测高电平脉宽） ---- */
    for (i = 0U; i < 5U; i++)
    {
        buf[i] = 0U;
        for (bit = 0U; bit < 8U; bit++)
        {
            if (dht11_wait_level(1U, DHT11_BIT_LOW_TIMEOUT) != 0U)
            {
                s_fail_stage = (uint8_t)PER_DHT11_FAIL_BIT_LOW;
                status = PER_DHT11_ERR_TIMEOUT;
                goto exit;
            }
            high_us = dht11_measure_high_us();
            if (high_us == 0xFFFFFFFFU)
            {
                s_fail_stage = (uint8_t)PER_DHT11_FAIL_BIT_HIGH;
                status = PER_DHT11_ERR_TIMEOUT;
                goto exit;
            }
            buf[i] <<= 1;
            if (high_us > DHT11_BIT_THRESHOLD_US)
            {
                buf[i] |= 0x01U;   /* 高电平 >40 µs = 1（26~28 µs 的则是 0） */
            }
        }
    }

exit:
    /* 收尾：先恢复总线空闲态，再开中断（顺序固定，见函数头注意事项①） */
    dht11_pin_output();
    dht11_pin_high();
    /* ================================================================
     *  时序关键段结束
     * ================================================================ */
    __enable_irq();

    if (status != PER_DHT11_OK)
    {
        return status;   /* 失败时【不改写 data】：上层可以沿用上一次的值 */
    }

    /* ---- 步骤 4：校验和 = 前 4 字节之和 & 0xFF ---- */
    checksum = (uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]);
    if (checksum != buf[4])
    {
        s_fail_stage = (uint8_t)PER_DHT11_FAIL_CHECKSUM;
        return PER_DHT11_ERR_CHECKSUM;
    }

    /* ---- 步骤 5：填输出数据（原样上报，不做任何换算） ---- */
    data->humidity_int    = buf[0];
    data->humidity_dec    = buf[1];
    data->temperature_int = buf[2];
    data->temperature_dec = buf[3];

    return PER_DHT11_OK;
}

/*----------------------------------------------------------------------------
 * 函数功能：取最近一次读取的失败阶段
 * 调用处  ：应用层 app_dht11.c            【接口契约见 per_dht11.h】
 *----------------------------------------------------------------------------*/
PER_DHT11_FailStage_t PER_DHT11_GetLastFailStage(void)
{
    return (PER_DHT11_FailStage_t)s_fail_stage;
}

/*----------------------------------------------------------------------------
 * 函数功能：硬件自检——只探测、不打印（打印是应用层的事）
 * 参数    ：diag —— 诊断结果输出结构体（NULL 时直接返回）
 * 调用处  ：应用层 app_dht11.c 启动时调一次，由应用层决定怎么把这些数字
 *           变成人能读的文本
 * 注意事项：① ★ 为什么感知层不做打印：
 *              打印要走 mod_uart1 → net_uart，那是【上层】的模块。
 *              感知层依赖上层 = 依赖方向倒转，分层直接破产。
 *              所以本函数只把物理量测出来装进结构体，表达交给应用层——
 *              这也让"换一种输出方式（比如只在 LCD 上显示）"不用改本文件
 *           ② 自检会占用约 25 ms（含一次完整起始信号），只在启动时跑一次
 *           ③ 诊断项各自回答一个问题，看到异常就知道该查哪：
 *                tim2_delta ≈1000  → 否则 TIM2 没跑起来或 PSC 配错（µs 计时全错）
 *                loopback 1,0      → 否则 PG9 引脚不通（焊点/排针/引脚号写错）
 *                idle_bus = 1      → 0 表示被外部拉低（接线短路/上拉缺失）
 *                resp_low_samples  → 0 = 传感器完全没应答；>0 = 有应答但时序不对
 *----------------------------------------------------------------------------*/
void PER_DHT11_SelfTest(PER_DHT11_Diag_t *diag)
{
    uint32_t low_count = 0U;
    uint32_t start;

    if (diag == NULL)
    {
        return;
    }

    /* ⓪ TIM2 计时自检：延时 1000 µs，实测差值应 ≈1000 */
    {
        uint32_t t0 = per_delay_getTickUs();
        per_delay_us(1000U);
        diag->tim2_delta_us = per_delay_getTickUs() - t0;
    }

    /* ① GPIO 环回：输出高/低再读回，验证 PG9 通路 */
    dht11_pin_output();
    dht11_pin_high();
    per_delay_us(10U);
    diag->loopback_high = dht11_pin_read();
    dht11_pin_low();
    per_delay_us(10U);
    diag->loopback_low  = dht11_pin_read();

    /* ② 空闲总线电平：正常应被上拉为 1 */
    dht11_pin_high();
    per_delay_us(200U);
    diag->idle_bus = dht11_pin_read();

    /* ③ 发一次完整起始信号，统计应答窗口内总线被拉低的次数 */
    dht11_start_low_wait();
    __disable_irq();
    dht11_pin_high();
    per_delay_us(DHT11_START_HIGH_US);
    dht11_pin_input();
    start = per_delay_getTickUs();
    while ((per_delay_getTickUs() - start) < (DHT11_RESPONSE_TIMEOUT + 100U))
    {
        if (dht11_pin_read() == 0U)
        {
            low_count++;
        }
    }
    dht11_pin_output();
    dht11_pin_high();
    __enable_irq();

    diag->resp_low_samples = low_count;
}

#endif /* PER_DHT11_ENABLE */
