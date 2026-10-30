/**
 ******************************************************************************
 * @file    app_hex_cmd.c
 * @brief   应用层（app）—— 十六进制帧执行器实现（功能码 / 寄存器 → 业务动作）
 *
 *  一帧请求进来，走四步：
 *    ① 门卫：非本机地址直接吞掉；广播地址执行动作但不应答（总线礼仪）
 *    ② 分发：按功能码进入 READ_REG / WRITE_REG / LED_CTRL / HEARTBEAT
 *    ③ 执行：寄存器地址 → 读值或写动作（点灯、存阈值、取快照）
 *    ④ 应答：用 ProtoHex_BuildFrame 拼 8 字节，B6 bit7 = 1，回显请求序号
 *
 *  ★ 寄存器表与 protocol_hex.h 一一对应，本文件不出现裸地址数字，
 *    加寄存器只改头文件 + 这里一个 case。
 *
 *  ★ 两种依赖各自有条件编译：
 *    温湿度寄存器 → APP_DHT11_ENABLE（关 DHT11 链时这两个寄存器自动消失）
 *    阈值寄存器   → APP_PARAM_ENABLE（没有 EEPROM 就没有"掉电保存"这回事）
 *    门禁在 cfg_check.h，编译期就会拦住"开了 HEX 却没开点灯链"这类悬挂配置。
 *
 *  接口的完整说明写在 app_hex_cmd.h，本文件注释侧重"功能码是怎么落到动作上的"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_HEX_CMD_ENABLE

#include "app_hex_cmd.h"
#include "protocol_hex.h"
#include "app_blink.h"
#include "per_led.h"
#include "cmsis_os.h"
#if APP_DHT11_ENABLE
#include "app_dht11.h"
#endif
#if APP_PARAM_ENABLE
#include "app_param.h"
#endif

/* 固件版本 / 协议版本：高 8 位主版本、低 8 位次版本 */
#define HEX_FW_VERSION      0x0100U
#define HEX_PROTO_VERSION   0x0100U

/* ===================== 内部工具 ===================== */

/*----------------------------------------------------------------------------
 * 读当前 LED 位域：bit0 = LED1，bit1 = LED2
 * ★ 读的是真实引脚电平（一路问到底层），不是软件记忆值：
 *   闪烁中的 LED1 查出来是"此刻"的亮灭，排障时以它为准
 *----------------------------------------------------------------------------*/
static uint8_t hex_read_led_state(void)
{
    uint8_t state = 0U;

#if APP_BLINK_ENABLE
    if (APP_BLINK_IsOn(APP_BLINK_LED1) != 0U)
    {
        state |= 0x01U;
    }
#endif
#if PER_LED_ENABLE
    if (PER_LED_IsOn(PER_LED2) != 0U)
    {
        state |= 0x02U;
    }
#endif
    return state;
}

/*----------------------------------------------------------------------------
 * 写 LED 位域：1 = 亮、0 = 灭
 * ★ LED1 必须经 APP_BLINK_On / Off：它先把闪烁半周期清 0，再点灯，
 *   否则闪烁状态机会和帧命令抢同一颗灯（表现为"刚点亮又自己灭了"）
 * ★ LED2 不经闪烁状态机（本工程只让 LED1 闪），直接走感知层，
 *   与 app_link 的湿度联动是同一条路子
 *----------------------------------------------------------------------------*/
static void hex_write_led_state(uint8_t state)
{
#if APP_BLINK_ENABLE
    if ((state & 0x01U) != 0U)
    {
        (void)APP_BLINK_On(APP_BLINK_LED1);
    }
    else
    {
        (void)APP_BLINK_Off(APP_BLINK_LED1);
    }
#endif
#if PER_LED_ENABLE
    if ((state & 0x02U) != 0U)
    {
        (void)PER_LED_On(PER_LED2);
    }
    else
    {
        (void)PER_LED_Off(PER_LED2);
    }
#endif
}

/*----------------------------------------------------------------------------
 * 运行秒数：直接用 RTOS 节拍算，不额外开定时器
 * ★ osKernelGetTickFreq() 取当前 tick 频率（本工程 1000 Hz），
 *   不写死 1000，改了 configTICK_RATE_HZ 这里仍然正确
 *----------------------------------------------------------------------------*/
static uint32_t hex_runtime_seconds(void)
{
    uint32_t freq = osKernelGetTickFreq();

    if (freq == 0U)
    {
        return 0U;
    }
    return (uint32_t)(osKernelGetTickCount() / freq);
}

/*----------------------------------------------------------------------------
 * 读寄存器
 * 返回值：0 = 成功（值写进 *data）；非 0 = 地址非法
 *----------------------------------------------------------------------------*/
static uint8_t hex_reg_read(uint16_t reg, uint16_t *data)
{
    switch (reg)
    {
    case REG_DEVICE_ID:
        *data = PROTO_HEX_DEVICE_ADDR;
        return 0U;

    case REG_FW_VERSION:
        *data = HEX_FW_VERSION;
        return 0U;

    case REG_PROTO_VERSION:
        *data = HEX_PROTO_VERSION;
        return 0U;

    case REG_LED_STATE:
        *data = (uint16_t)hex_read_led_state();
        return 0U;

#if APP_DHT11_ENABLE
    case REG_TEMPERATURE:
    {
        APP_DHT11_Snapshot_t s;

        APP_DHT11_GetSnapshot(&s);
        *data = (s.valid != 0U) ? (uint16_t)((uint16_t)s.temp_int * 10U + s.temp_dec) : 0U;
        return 0U;
    }
    case REG_HUMIDITY:
    {
        APP_DHT11_Snapshot_t s;

        APP_DHT11_GetSnapshot(&s);
        *data = (s.valid != 0U) ? (uint16_t)((uint16_t)s.humi_int * 10U + s.humi_dec) : 0U;
        return 0U;
    }
#endif

    case REG_TEMP_THRESHOLD:
    {
        uint8_t t = 0U;
        uint8_t h = 0U;

#if APP_PARAM_ENABLE
        APP_PARAM_GetThresholds(&t, &h);
#else
        t = (uint8_t)APP_LINK_TEMP_THRESHOLD;
#endif
        *data = (uint16_t)t;
        return 0U;
    }
    case REG_HUMI_THRESHOLD:
    {
        uint8_t t = 0U;
        uint8_t h = 0U;

#if APP_PARAM_ENABLE
        APP_PARAM_GetThresholds(&t, &h);
#else
        h = (uint8_t)APP_LINK_HUMI_THRESHOLD;
#endif
        *data = (uint16_t)h;
        return 0U;
    }

    case REG_RUNTIME_LOW:
        *data = (uint16_t)(hex_runtime_seconds() & 0xFFFFU);
        return 0U;
    case REG_RUNTIME_HIGH:
        *data = (uint16_t)(hex_runtime_seconds() >> 16);
        return 0U;

    default:
        return 1U;   /* 不在寄存器表里 */
    }
}

/*----------------------------------------------------------------------------
 * 写寄存器
 * 返回值：0 = 成功；非 0 = PROTO_HEX_ERR_* 错误码
 *----------------------------------------------------------------------------*/
static uint8_t hex_reg_write(uint16_t reg, uint16_t data)
{
    switch (reg)
    {
    case REG_LED_STATE:
        if ((data & 0xFFFCU) != 0U)
        {
            return PROTO_HEX_ERR_INVALID_VALUE;   /* 本工程只有两颗灯，bit2 以上无意义 */
        }
        hex_write_led_state((uint8_t)(data & 0x0003U));
        return 0U;   /* 注意：不存 EEPROM（参数块没有 led_state 字段） */

    case REG_TEMP_THRESHOLD:
    case REG_HUMI_THRESHOLD:
    {
        uint8_t t = 0U;
        uint8_t h = 0U;

#if APP_PARAM_ENABLE
        APP_PARAM_GetThresholds(&t, &h);
        if (reg == REG_TEMP_THRESHOLD)
        {
            t = (uint8_t)data;
        }
        else
        {
            h = (uint8_t)data;
        }
        /* 范围校验与"RAM + EEPROM + 读回确认"都在 APP_PARAM_SetThresholds 里，
         * 本文件不重复实现，也不直接碰 mod_eeprom */
        if (APP_PARAM_SetThresholds(t, h) != APP_PARAM_OK)
        {
            return PROTO_HEX_ERR_INVALID_VALUE;   /* 越界或存储失败：主站按 0x03 处理 */
        }
        return 0U;
#else
        (void)t;
        (void)h;
        (void)data;
        return PROTO_HEX_ERR_INVALID_VALUE;       /* 没开参数链：写不进去 */
#endif
    }

    case REG_DEVICE_ID:
    case REG_FW_VERSION:
    case REG_PROTO_VERSION:
#if APP_DHT11_ENABLE
    case REG_TEMPERATURE:
    case REG_HUMIDITY:
#endif
    case REG_RUNTIME_LOW:
    case REG_RUNTIME_HIGH:
        return PROTO_HEX_ERR_INVALID_REG;         /* 只读寄存器：写它算地址非法 */

    default:
        return PROTO_HEX_ERR_INVALID_REG;
    }
}

/* ===================== 对外接口 ===================== */

uint8_t APP_HexCmd_Process(const uint8_t *in_frame, uint8_t *out_frame)
{
    uint8_t  cmd;
    uint8_t  seq;
    uint8_t  dev_addr;
    uint8_t  no_reply;
    uint16_t reg;
    uint16_t data;

    if ((in_frame == NULL) || (out_frame == NULL))
    {
        return 0U;
    }

    cmd      = PROTO_HEX_GET_CMD(in_frame);
    reg      = PROTO_HEX_GET_REG(in_frame);
    data     = PROTO_HEX_GET_DATA(in_frame);
    seq      = PROTO_HEX_GET_SEQ(in_frame);
    dev_addr = PROTO_HEX_GET_DEVADDR(in_frame);

    /* ①② 门卫：非本机地址直接吞掉；广播地址照常执行动作，但一律不应答
     *   （总线上多台设备同时回包会把总线撞死） */
    if ((dev_addr != PROTO_HEX_BROADCAST_ADDR) && (dev_addr != PROTO_HEX_DEVICE_ADDR))
    {
        return 0U;
    }
    no_reply = (dev_addr == PROTO_HEX_BROADCAST_ADDR) ? 1U : 0U;

    /* ③④ 分发 + 执行 + 应答 */
    switch (cmd)
    {
    case PROTO_HEX_CMD_READ_REG:
    {
        uint16_t val = 0U;

        if (hex_reg_read(reg, &val) == 0U)
        {
            (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                      PROTO_HEX_CMD_READ_REG, reg, val, seq, 1U);
        }
        else
        {
            (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                      PROTO_HEX_CMD_ERROR, reg, PROTO_HEX_ERR_INVALID_REG, seq, 1U);
        }
        return (uint8_t)(no_reply == 0U);
    }

    case PROTO_HEX_CMD_WRITE_REG:
    {
        uint8_t err = hex_reg_write(reg, data);

        if (err == 0U)
        {
            (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                      PROTO_HEX_CMD_ACK, reg, PROTO_HEX_CMD_WRITE_REG, seq, 1U);
        }
        else
        {
            (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                      PROTO_HEX_CMD_ERROR, reg, err, seq, 1U);
        }
        return (uint8_t)(no_reply == 0U);
    }

    case PROTO_HEX_CMD_LED_CTRL:
        /* 位域控制：DATA bit0 = LED1, bit1 = LED2；
         * 应答 DATA 回"执行后的真实状态"，主站一眼看出有没有落地 */
        hex_write_led_state((uint8_t)(data & 0x0003U));
        (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                  PROTO_HEX_CMD_LED_CTRL, reg,
                                  (uint16_t)hex_read_led_state(), seq, 1U);
        return (uint8_t)(no_reply == 0U);

    case PROTO_HEX_CMD_HEARTBEAT:
        (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                  PROTO_HEX_CMD_HEARTBEAT, reg,
                                  (uint16_t)(hex_runtime_seconds() & 0xFFFFU), seq, 1U);
        return (uint8_t)(no_reply == 0U);

    case PROTO_HEX_CMD_SENSOR_REPORT:
        /* 上报帧是设备主动发的；本机收到别人的上报不应答（半双工总线礼仪） */
        return 0U;

    default:
        (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                                  PROTO_HEX_CMD_ERROR, reg, PROTO_HEX_ERR_INVALID_CMD, seq, 1U);
        return (uint8_t)(no_reply == 0U);
    }
}

uint8_t APP_HexCmd_BuildCrcError(const uint8_t *bad_frame, uint8_t *out_frame)
{
    uint8_t dev_addr;

    if ((bad_frame == NULL) || (out_frame == NULL))
    {
        return 0U;
    }

    dev_addr = PROTO_HEX_GET_DEVADDR(bad_frame);
    if (dev_addr == PROTO_HEX_BROADCAST_ADDR)
    {
        return 0U;   /* 广播帧算错也不应答，避免全员同时回包 */
    }
    if (dev_addr != PROTO_HEX_DEVICE_ADDR)
    {
        return 0U;   /* 不是发给本机的，静默 */
    }

    (void)ProtoHex_BuildFrame(out_frame, PROTO_HEX_DEVICE_ADDR,
                              PROTO_HEX_CMD_ERROR,
                              PROTO_HEX_GET_REG(bad_frame),
                              PROTO_HEX_ERR_CRC,
                              PROTO_HEX_GET_SEQ(bad_frame), 1U);
    return 1U;
}

#endif /* APP_HEX_CMD_ENABLE */
