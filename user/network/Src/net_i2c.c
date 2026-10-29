/**
 ******************************************************************************
 * @file    net_i2c.c
 * @brief   网络层（network）—— I2C1 总线实现（2.3 外部存储器新增）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 net_i2c.h，
 *  本文件的注释侧重"时序是怎么做对的"。
 *
 *  ★ 本层四个知识点（对应课件 16 页）：
 *    ① 超时不死等：所有等待带 NET_I2C_TIMEOUT_MS 上限，到点返回 TIMEOUT；
 *    ② 返回码分四种：NACK 与 TIMEOUT 分开报——前者查地址，后者查接线；
 *    ③ 卡死自救：SDA 被从机拉死 → 切 GPIO 发 9 个 SCL + STOP → 再开外设；
 *    ④ 只懂字节不懂器件：本文件搜不到 "AT24" 三个字母。
 ******************************************************************************
 */
#include "module_cfg.h"
#if NET_I2C_ENABLE

#include "net_i2c.h"
#include "i2c.h"      /* hi2c1 + MX_I2C1_Init（总线本体在 Core，main 无条件初始化） */
#include "main.h"     /* HAL GPIO（九时钟自救要临时把 AF 引脚当 GPIO 用）           */

/* ===================== 内部工具 ===================== */

/*----------------------------------------------------------------------------
 * 恢复时序用的粗延时（约几十 µs 级，不要求精确）
 * 为什么不用 per_delay（TIM2 微秒底座）：那是感知层的模块，网络层不能反向
 * 依赖；恢复时序只要求"别快于 100 kHz"，粗延时反而更安全
 *----------------------------------------------------------------------------*/
static void bus_delay(void)
{
    volatile uint32_t n = 1000U;

    while (n-- != 0U)
    {
        __NOP();
    }
}

/*----------------------------------------------------------------------------
 * HAL 状态 → 本层四态返回码
 * 错误码里 AF（Acknowledge Failure）= NACK，超时 = TIMEOUT，分头报
 *----------------------------------------------------------------------------*/
static int hal_to_net(HAL_StatusTypeDef st, uint32_t err)
{
    if (st == HAL_OK)
    {
        return NET_I2C_OK;
    }
    if (st == HAL_BUSY)
    {
        return NET_I2C_BUSY;
    }
    if ((err & HAL_I2C_ERROR_AF) != 0U)
    {
        return NET_I2C_NACK;    /* 器件没应答：查地址脚与接线 */
    }
    return NET_I2C_TIMEOUT;     /* 其余一律按链路问题报 */
}

/*----------------------------------------------------------------------------
 * 引脚临时切换：AF4 开漏 ↔ GPIO 开漏（九时钟自救专用）
 *----------------------------------------------------------------------------*/
static void pins_to_gpio_od(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    g.Pin       = GPIO_PIN_8 | GPIO_PIN_9;   /* PB8 = SCL, PB9 = SDA */
    g.Mode      = GPIO_MODE_OUTPUT_OD;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &g);

    /* 双双置高 = 释放总线（开漏 + 上拉，靠板载 R33/R34 拉起） */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_SET);
    bus_delay();
}

static void pins_to_af(void)
{
    GPIO_InitTypeDef g = {0};

    g.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    g.Mode      = GPIO_MODE_AF_OD;           /* I2C 引脚必须开漏 */
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);
}

/* ===================== 对外接口 ===================== */

/*----------------------------------------------------------------------------
 * 九时钟自救：SDA 被从机拉死时的标准动作
 * 原理：从机多半是在等第 9 个（或后续）时钟好把 SDA 松开——
 *       主机补足时钟，从机放掉 SDA，再补一个 STOP 让所有器件归位。
 *----------------------------------------------------------------------------*/
int net_i2c_recover(void)
{
    uint8_t i;

    pins_to_gpio_od();

    for (i = 0U; i < 9U; i++)
    {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);   /* SCL 低 */
        bus_delay();
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);     /* SCL 高 */
        bus_delay();

        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET)
        {
            break;   /* 从机松手了：不必把 9 个时钟数满 */
        }
    }

    /* 补一个 STOP：SCL 高电平期间 SDA 由低变高 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    bus_delay();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_SET);
    bus_delay();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    bus_delay();

    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) != GPIO_PIN_SET)
    {
        pins_to_af();
        MX_I2C1_Init();   /* 外设复位，别带着坏状态跑 */
        return NET_I2C_TIMEOUT;   /* 救不回来：不是时序卡死，查供电/地线 */
    }

    pins_to_af();
    MX_I2C1_Init();       /* 切回 AF 后重跑初始化，外设状态归零 */
    return NET_I2C_OK;
}

/*----------------------------------------------------------------------------
 * 初始化 = 总线健康检查：空闲电平不对就先自救
 *----------------------------------------------------------------------------*/
int net_i2c_init(void)
{
    /* AF 复用下 IDR 仍能读到真实电平（输入通路与 AF 无关），
     * 正常空闲 = SDA/SCL 都被上拉拉高 */
    if ((HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_SET) &&
        (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_8) == GPIO_PIN_SET))
    {
        return NET_I2C_OK;
    }

    return net_i2c_recover();   /* 总线被拉死：先自救，成不成如实报 */
}

/*----------------------------------------------------------------------------
 * 事务策略：先试一次；（BUSY/TIMEOUT 时）自救一次再重试一次。
 * NACK 不重试：器件不应答是地址/接线问题，重试只是在刷超时。
 *----------------------------------------------------------------------------*/
int net_i2c_write_ex(uint8_t dev, uint16_t mem, const uint8_t *buf,
                     uint16_t len, uint8_t mem_16bit)
{
    HAL_StatusTypeDef st;
    uint32_t err;
    uint16_t memsize = (mem_16bit != 0U) ? I2C_MEMADD_SIZE_16BIT
                                         : I2C_MEMADD_SIZE_8BIT;

    if ((buf == NULL) || (len == 0U))
    {
        return NET_I2C_OK;          /* 空写无事可做，按成功处理（探器件走 probe） */
    }

    st  = HAL_I2C_Mem_Write(&hi2c1, dev, mem, memsize,
                            (uint8_t *)buf, len, NET_I2C_TIMEOUT_MS);
    err = hi2c1.ErrorCode;
    if (hal_to_net(st, err) == NET_I2C_OK)
    {
        return NET_I2C_OK;
    }

    /* 失败：自救一次再试一次（换一根线救回来的典型场景） */
    (void)net_i2c_recover();
    st  = HAL_I2C_Mem_Write(&hi2c1, dev, mem, memsize,
                            (uint8_t *)buf, len, NET_I2C_TIMEOUT_MS);
    return hal_to_net(st, hi2c1.ErrorCode);
}

int net_i2c_write(uint8_t dev, uint16_t mem, const uint8_t *buf, uint16_t len)
{
    return net_i2c_write_ex(dev, mem, buf, len, 0U);   /* 默认 8 位字地址 */
}

int net_i2c_read_ex(uint8_t dev, uint16_t mem, uint8_t *buf,
                    uint16_t len, uint8_t mem_16bit)
{
    HAL_StatusTypeDef st;
    uint16_t memsize = (mem_16bit != 0U) ? I2C_MEMADD_SIZE_16BIT
                                         : I2C_MEMADD_SIZE_8BIT;

    if ((buf == NULL) || (len == 0U))
    {
        return NET_I2C_TIMEOUT;     /* 没有缓冲就别读了，如实报错 */
    }

    st = HAL_I2C_Mem_Read(&hi2c1, dev, mem, memsize,
                          buf, len, NET_I2C_TIMEOUT_MS);
    if (hal_to_net(st, hi2c1.ErrorCode) == NET_I2C_OK)
    {
        return NET_I2C_OK;
    }

    (void)net_i2c_recover();
    st = HAL_I2C_Mem_Read(&hi2c1, dev, mem, memsize,
                          buf, len, NET_I2C_TIMEOUT_MS);
    return hal_to_net(st, hi2c1.ErrorCode);
}

int net_i2c_read(uint8_t dev, uint16_t mem, uint8_t *buf, uint16_t len)
{
    return net_i2c_read_ex(dev, mem, buf, len, 0U);    /* 默认 8 位字地址 */
}

int net_i2c_probe(uint8_t dev)
{
    /* IsDeviceReady 只发地址帧，不碰数据；Trials=2 给器件一点反应时间 */
    HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(&hi2c1, dev, 2U,
                                                 NET_I2C_TIMEOUT_MS);
    return hal_to_net(st, hi2c1.ErrorCode);
}

#endif /* NET_I2C_ENABLE */
