/**
 ******************************************************************************
 * @file    net_i2c_sw.c
 * @brief   网络层（network）—— 软件 I2C 总线实现（2.9 OLED 新增）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 net_i2c_sw.h，
 *  本文件的注释侧重"时序是怎么做对的"。
 *
 *  ★ 本文件只有三件事：起始、停止、写字节。
 *    搜不到 "SSD1306"、搜不到 "0x78" —— 那些是器件知识，属于平台层。
 *
 *  ▍延时为什么直接读 TIM2->CNT：
 *    TIM2 由 Core/Src/tim.c 的 MX_TIM2_Init() 无条件启动（1 tick = 1 µs，
 *    PSC = 83），是公共外设；本层直接读它的 CNT 就能得到微秒刻度。
 *    不 include 感知层的 per_delay.h：网络层不反向依赖感知层模块
 *    （与 net_i2c.c 的九时钟自救用本地粗延时是同一条纪律）。
 ******************************************************************************
 */
#include "module_cfg.h"
#if NET_I2C_SW_ENABLE

#include "net_i2c_sw.h"
#include "main.h"     /* HAL GPIO 与 TIM2 寄存器定义（TIM2 已在 main() 里启动） */

/* ========== 引脚绑定（板级事实，换板只改这几行） ========== */
#define NET_I2CSW_SCL_PORT   GPIOC
#define NET_I2CSW_SCL_PIN    GPIO_PIN_4     /* OLED SCL -> PC4 */
#define NET_I2CSW_SDA_PORT   GPIOC
#define NET_I2CSW_SDA_PIN    GPIO_PIN_5     /* OLED SDA -> PC5 */

/* ========== 微秒忙等 ==========
 * 只用于 I2C 半周期（2 µs）这种"必须连续占用"的场合；
 * 毫秒级等待一律 osDelay，绝不用它做业务节拍。
 * 回绕安全：用无符号减法 (now - start)，跨 0xFFFFFFFF 也正确。 */
static void sw_delay_us(uint32_t us)
{
    uint32_t start = TIM2->CNT;

    while ((uint32_t)(TIM2->CNT - start) < us)
    {
        /* 空转：这里刻意不关中断——关中断只会拖长总线的绝对时间 */
    }
}

/* ========== 引脚电平操作 ========== */
static void scl_high(void) { HAL_GPIO_WritePin(NET_I2CSW_SCL_PORT, NET_I2CSW_SCL_PIN, GPIO_PIN_SET); }
static void scl_low(void)  { HAL_GPIO_WritePin(NET_I2CSW_SCL_PORT, NET_I2CSW_SCL_PIN, GPIO_PIN_RESET); }
static void sda_high(void) { HAL_GPIO_WritePin(NET_I2CSW_SDA_PORT, NET_I2CSW_SDA_PIN, GPIO_PIN_SET); }
static void sda_low(void)  { HAL_GPIO_WritePin(NET_I2CSW_SDA_PORT, NET_I2CSW_SDA_PIN, GPIO_PIN_RESET); }

/* ===================== 对外接口 ===================== */

/*----------------------------------------------------------------------------
 * 初始化：引脚配置 + 空闲态（两条线都拉高）
 *----------------------------------------------------------------------------*/
void NET_I2CSW_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();   /* 漏了这一行：后面所有寄存器写操作全部无效 */

    /* 先把输出寄存器写成"高"，再切成输出态：I2C 空闲态就是两条线都高，
     * 顺序反过来会在配置瞬间把总线拉低一下，从机可能误判成起始信号 */
    HAL_GPIO_WritePin(NET_I2CSW_SCL_PORT,
                      NET_I2CSW_SCL_PIN | NET_I2CSW_SDA_PIN, GPIO_PIN_SET);

    g.Pin   = NET_I2CSW_SCL_PIN | NET_I2CSW_SDA_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;      /* 推挽：能主动输出高与低（上拉在模块上） */
    g.Pull  = GPIO_NOPULL;              /* 模块自带上拉，内部电阻一挂就改变了上升沿 */
    g.Speed = GPIO_SPEED_FREQ_HIGH;     /* 高速：保证 250 kHz 下边沿够陡 */
    HAL_GPIO_Init(NET_I2CSW_SCL_PORT, &g);

    sw_delay_us(NET_I2CSW_DELAY_US);    /* 让电平稳定后再允许第一条事务 */
}

/*----------------------------------------------------------------------------
 * 起始：SCL 高 → SDA 高→低 → SCL 拉低
 *----------------------------------------------------------------------------*/
void NET_I2CSW_Start(void)
{
    sda_high();
    scl_high();
    sw_delay_us(NET_I2CSW_DELAY_US);

    sda_low();                          /* SCL 高电平期间 SDA 下降 = START */
    sw_delay_us(NET_I2CSW_DELAY_US);

    scl_low();                          /* 拉低 SCL：占住总线，准备发字节 */
    sw_delay_us(NET_I2CSW_DELAY_US);
}

/*----------------------------------------------------------------------------
 * 停止：SCL 高 → SDA 低→高（总线归位）
 *----------------------------------------------------------------------------*/
void NET_I2CSW_Stop(void)
{
    sda_low();
    sw_delay_us(NET_I2CSW_DELAY_US);

    scl_high();
    sw_delay_us(NET_I2CSW_DELAY_US);

    sda_high();                         /* SCL 高电平期间 SDA 上升 = STOP */
    sw_delay_us(NET_I2CSW_DELAY_US);
}

/*----------------------------------------------------------------------------
 * 写一个字节：MSB 在前 + 第 9 个 ACK 时钟（产生但不等应答）
 *----------------------------------------------------------------------------*/
void NET_I2CSW_WriteByte(uint8_t byte)
{
    uint8_t i;

    for (i = 0U; i < 8U; i++)
    {
        scl_low();                      /* SCL 低电平期间才允许改 SDA */
        sw_delay_us(1U);

        if ((byte & 0x80U) != 0U)
        {
            sda_high();
        }
        else
        {
            sda_low();
        }
        byte <<= 1U;
        sw_delay_us(1U);

        scl_high();                     /* SCL 上升沿：从机在这一刻采样 SDA */
        sw_delay_us(NET_I2CSW_DELAY_US);
    }

    /* 第 9 个时钟：主机释放 SDA，从机拉低表示 ACK。
     * 本层不读回 SDA（引脚是推挽输出），但时钟一定要给：
     * 少了它，SSD1306 的 I2C 状态机不认为这一字节结束，后续全部失效。 */
    scl_low();
    sda_high();                         /* 释放 SDA，让从机有机会拉低 */
    sw_delay_us(1U);
    scl_high();
    sw_delay_us(NET_I2CSW_DELAY_US);
    scl_low();
    sw_delay_us(1U);
}

#endif /* NET_I2C_SW_ENABLE */
