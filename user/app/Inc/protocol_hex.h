/**
 ******************************************************************************
 * @file    protocol_hex.h
 * @brief   应用层（app）—— 8 字节固定帧十六进制协议：帧格式 / 功能码 / 寄存器表
 *
 *  本文件只描述"帧长什么样"，不含任何业务动作：
 *    解析（逐字节收帧 + CRC8）在 protocol_hex.c，
 *    执行（功能码 → 寄存器 → 点灯/取值 → 应答）在 app_hex_cmd.c。
 *  这样拆是为了让"协议"能整体搬到别的通道（RS485 / TCP / MQTT 主题），
 *  搬的时候只改喂字节的人，帧格式一个字都不用动。
 *
 *  帧格式（固定 8 字节，大端序）：
 *    B0: [7:4] 版本号(=0x1)  [3:0] 设备地址(0~15，0 = 广播)
 *    B1: 功能码 CMD
 *    B2: 寄存器地址高 8 位 REG_H
 *    B3: 寄存器地址低 8 位 REG_L
 *    B4: 数据高 8 位 DATA_H
 *    B5: 数据低 8 位 DATA_L
 *    B6: [7] 应答标志(1 = 应答帧)  [6:0] 帧序号(0~127)
 *    B7: CRC8（前 7 字节，poly=0x07, init=0x00）
 *
 *  ★ 与文本协议共存的判据只有一条：B0 高 4 位 == 0x1（即 0x10~0x1F）。
 *    ASCII 可打印字符从 0x20 起，天然撞不上帧头 —— 所以一个字节流里
 *    "先试帧、后试行"可以互不误伤（分流逻辑在 app_cmd.c）。
 *
 *  设计目标（与课件 2.5 一致）：
 *    - 类 Modbus 寄存器模型：固定帧长 + 功能码 + 寄存器地址 + 数据 + 校验
 *    - 加功能只加寄存器 / 功能码，帧结构不改
 *    - 寄存器地址可直接映射成 MQTT 主题（dev/01/reg/0010），协议不必重写
 ******************************************************************************
 */
#ifndef PROTOCOL_HEX_H
#define PROTOCOL_HEX_H

#include <stdint.h>

/* ===================== 协议常量 ===================== */
#define PROTO_HEX_FRAME_SIZE      8U    /* 固定帧长（字节） */
#define PROTO_HEX_VERSION         0x1U  /* B0 高 4 位 = 版本号 */
#define PROTO_HEX_DEVICE_ADDR     0x1U  /* 本机默认设备地址 */
#define PROTO_HEX_BROADCAST_ADDR  0x0U  /* 广播地址：全员执行、不应答 */

/* ===================== 功能码（B1） ===================== */
typedef enum
{
    PROTO_HEX_CMD_READ_REG      = 0x01U,  /* 读寄存器：应答 DATA = 寄存器值      */
    PROTO_HEX_CMD_WRITE_REG     = 0x02U,  /* 写寄存器：成功回 0xEE，失败回 0xFF  */
    PROTO_HEX_CMD_LED_CTRL      = 0x10U,  /* 灯控制：DATA bit0 = LED1, bit1 = LED2 */
    PROTO_HEX_CMD_SENSOR_REPORT = 0x11U,  /* 传感器主动上报（收到不应答）       */
    PROTO_HEX_CMD_HEARTBEAT     = 0x12U,  /* 心跳：应答 DATA = 运行秒数低 16 位  */
    PROTO_HEX_CMD_ACK           = 0xEEU,  /* 通用成功应答                       */
    PROTO_HEX_CMD_ERROR         = 0xFFU   /* 错误应答：DATA = 错误码            */
} proto_hex_cmd_t;

/* ===================== 错误码（ERROR 帧的 DATA） ===================== */
#define PROTO_HEX_ERR_INVALID_CMD    0x01U  /* 功能码不在表里           */
#define PROTO_HEX_ERR_INVALID_REG    0x02U  /* 寄存器地址不在表里       */
#define PROTO_HEX_ERR_INVALID_VALUE  0x03U  /* 写了不允许的值           */
#define PROTO_HEX_ERR_CRC            0x04U  /* 第 8 字节 CRC 算错       */

/* ===================== 寄存器表（本工程子集） =====================
 * ★ 只放"本工程真的有的东西"：两颗灯、DHT11 温湿度、两个阈值、运行时间。
 *   参考工程里那些 WiFi 密码 / TCP IP / 超声波的寄存器，本工程没有对应硬件，
 *   宁可不登记，也不要"读得到但永远返回 0"的假寄存器。 */
#define REG_DEVICE_ID         0x0000U  /* 只读：设备 ID，固定 0x0001            */
#define REG_FW_VERSION        0x0001U  /* 只读：固件版本，高 8 主 / 低 8 次     */
#define REG_PROTO_VERSION     0x0002U  /* 只读：协议版本，固定 0x0100           */
#define REG_LED_STATE         0x0010U  /* 读写：bit0 = LED1, bit1 = LED2        */
#define REG_TEMPERATURE       0x0020U  /* 只读：温度 int16，单位 0.1 ℃          */
#define REG_HUMIDITY          0x0021U  /* 只读：湿度 uint16，单位 0.1 %RH       */
#define REG_TEMP_THRESHOLD    0x0030U  /* 读写：LED1 温度阈值（℃）             */
#define REG_HUMI_THRESHOLD    0x0031U  /* 读写：LED2 湿度阈值（%RH）            */
#define REG_RUNTIME_LOW       0x0100U  /* 只读：运行时间低 16 位（秒）          */
#define REG_RUNTIME_HIGH      0x0101U  /* 只读：运行时间高 16 位（秒）          */

/* ===================== 解析结果 ===================== */
typedef enum
{
    PROTO_HEX_RET_BUSY = 0,   /* 收帧中；或该字节不是帧头（已被忽略） */
    PROTO_HEX_RET_FRAME,      /* 收满 8 字节且 CRC 正确               */
    PROTO_HEX_RET_CRC_ERROR   /* 收满 8 字节但 CRC 错（应答 0x04）    */
} proto_hex_ret_t;

/**
 * @brief 十六进制帧解析器（状态由调用方持有，可多通道各持一份）
 * @note  len 是三态状态机的唯一变量：0 = 找帧头，0 < len < 8 = 收字节。
 *        tx_seq 供"设备主动上报"用（本工程只用应答，保留字段以便扩展）。
 */
typedef struct
{
    uint8_t buf[PROTO_HEX_FRAME_SIZE];  /* 收帧缓冲 */
    uint8_t len;                        /* 已收字节数（0 = 空闲）   */
    uint8_t tx_seq;                     /* 主动上报帧序号计数器     */
} proto_hex_parser_t;

/*----------------------------------------------------------------------------
 * 函数功能：初始化解析器（len 清 0，准备找帧头）
 * 参数    ：parser —— 调用方持有的解析器
 * 返回值  ：无
 * 调用处  ：app_cmd_task() 进入主循环之前，每个串口通道各调一次
 * 注意事项：parser 为 NULL 时直接返回，不做任何事
 *----------------------------------------------------------------------------*/
void ProtoHex_Init(proto_hex_parser_t *parser);

/*----------------------------------------------------------------------------
 * 函数功能：逐字节喂入，检测一帧完整的 8 字节十六进制帧
 * 参数    ：parser    —— 解析器
 *           byte      —— 本次喂入的字节
 *           frame_out —— 输出缓冲（≥ 8 字节）；收满时复制整帧到此
 * 返回值  ：PROTO_HEX_RET_BUSY      还没收满，或该字节不是帧头（已忽略）
 *           PROTO_HEX_RET_FRAME     收满且 CRC 正确
 *           PROTO_HEX_RET_CRC_ERROR 收满但 CRC 错（frame_out 仍已填好，
 *                                   供调用方用 REG/SEQ 造 ERROR 应答）
 * 调用处  ：app_cmd.c 的 app_cmd_task()，每收到一个字节先喂本函数
 * 注意事项：① 帧头判据：B0 高 4 位 == PROTO_HEX_VERSION，否则忽略该字节
 *              —— 文本字符（0x20~0x7E）不可能被当成帧头，这是双协议共存的基础
 *           ② 收满 8 字节后 len 立即清 0，无论 CRC 对错都能接着收下一帧；
 *              连续发帧不会因为上一帧出错而卡死
 *           ③ 不阻塞、不打印、不碰硬件：纯函数式状态机，便于单元测试
 *----------------------------------------------------------------------------*/
proto_hex_ret_t ProtoHex_Feed(proto_hex_parser_t *parser, uint8_t byte, uint8_t *frame_out);

/*----------------------------------------------------------------------------
 * 函数功能：构建 8 字节帧（请求帧或应答帧都走它）
 * 参数    ：buf      —— 输出缓冲（≥ 8 字节）
 *           dev_addr —— 设备地址（0~15）
 *           cmd      —— 功能码
 *           reg      —— 寄存器地址
 *           data     —— 数据字段
 *           seq      —— 帧序号（0~127）
 *           is_ack   —— 1 = 应答帧（B6 bit7 置 1），0 = 请求帧
 * 返回值  ：固定 8（buf 为 NULL 时返回 0）
 * 调用处  ：app_hex_cmd.c 的每个功能码分支
 * 注意事项：B7 由前 7 字节实时计算，调用方不许自己填 CRC
 *----------------------------------------------------------------------------*/
uint8_t ProtoHex_BuildFrame(uint8_t *buf, uint8_t dev_addr, uint8_t cmd,
                            uint16_t reg, uint16_t data, uint8_t seq, uint8_t is_ack);

/*----------------------------------------------------------------------------
 * 函数功能：CRC8 计算（poly = 0x07, init = 0x00）
 * 参数    ：data —— 待校验数据；len —— 字节数
 * 返回值  ：8 位校验值（data 为 NULL 时返回 0）
 * 调用处  ：ProtoHex_Feed / ProtoHex_BuildFrame 内部
 * 注意事项：8 字节短帧 + 低速串口，CRC8 足够且只要 7×8 次移位，任何 MCU 无压力
 *----------------------------------------------------------------------------*/
uint8_t ProtoHex_CRC8(const uint8_t *data, uint8_t len);

/* ===================== 帧字段提取宏（只读，不改帧） ===================== */
#define PROTO_HEX_GET_VERSION(f)  (((f)[0] >> 4) & 0x0FU)
#define PROTO_HEX_GET_DEVADDR(f)  ((f)[0] & 0x0FU)
#define PROTO_HEX_GET_CMD(f)      ((f)[1])
#define PROTO_HEX_GET_REG(f)      (((uint16_t)(f)[2] << 8) | (uint16_t)(f)[3])
#define PROTO_HEX_GET_DATA(f)     (((uint16_t)(f)[4] << 8) | (uint16_t)(f)[5])
#define PROTO_HEX_GET_SEQ(f)      ((f)[6] & 0x7FU)
#define PROTO_HEX_IS_ACK(f)       (((f)[6] & 0x80U) != 0U)

#endif /* PROTOCOL_HEX_H */
