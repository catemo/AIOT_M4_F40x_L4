/**
 ******************************************************************************
 * @file    protocol_hex.c
 * @brief   应用层（app）—— 8 字节固定帧解析器 + CRC8 实现
 *
 *  这里只有"怎么把字节流切成一帧"和"怎么算校验"，不认功能码、不认寄存器：
 *  执行层（app_hex_cmd.c）拿到完整帧之后才决定"该点哪颗灯、该回什么值"。
 *
 *  ★ 状态机只有一个变量 len：
 *      len == 0         → 找帧头（B0 高 4 位 = 版本号），不是帧头就丢掉
 *      0 < len < 8      → 收字节，收满即校验
 *      收满后 len 归 0  → 无论 CRC 对错，立刻回到找帧头状态
 *  三态循环，没有第四种情况 —— 这就是"固定帧长协议好写"的原因。
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 protocol_hex.h。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_HEX_CMD_ENABLE

#include "protocol_hex.h"
#include <stddef.h>

/*----------------------------------------------------------------------------
 * 【接口契约见 protocol_hex.h】
 *----------------------------------------------------------------------------*/
void ProtoHex_Init(proto_hex_parser_t *parser)
{
    if (parser == NULL)
    {
        return;
    }
    parser->len    = 0U;
    parser->tx_seq = 0U;
}

proto_hex_ret_t ProtoHex_Feed(proto_hex_parser_t *parser, uint8_t byte, uint8_t *frame_out)
{
    uint8_t i;

    if ((parser == NULL) || (frame_out == NULL))
    {
        return PROTO_HEX_RET_BUSY;
    }

    /* ---------- 状态一：找帧头 ---------- */
    if (parser->len == 0U)
    {
        if (((byte >> 4) & 0x0FU) != PROTO_HEX_VERSION)
        {
            return PROTO_HEX_RET_BUSY;   /* 不是帧头：交给文本协议去处理 */
        }
        parser->buf[0] = byte;
        parser->len    = 1U;
        return PROTO_HEX_RET_BUSY;       /* 帧刚开始，还没收满 */
    }

    /* ---------- 状态二：收字节 ---------- */
    parser->buf[parser->len] = byte;
    parser->len++;

    if (parser->len < PROTO_HEX_FRAME_SIZE)
    {
        return PROTO_HEX_RET_BUSY;
    }

    /* ---------- 状态三：收满，校验并复位 ----------
     * ★ 先复制再校验：CRC 错时调用方仍需要 REG / SEQ 来造 ERROR 应答，
     *   所以 frame_out 无论对错都填满 8 字节。 */
    for (i = 0U; i < PROTO_HEX_FRAME_SIZE; i++)
    {
        frame_out[i] = parser->buf[i];
    }
    parser->len = 0U;   /* 立刻复位：错帧也不影响下一帧 */

    if (ProtoHex_CRC8(parser->buf, PROTO_HEX_FRAME_SIZE - 1U) !=
        parser->buf[PROTO_HEX_FRAME_SIZE - 1U])
    {
        return PROTO_HEX_RET_CRC_ERROR;
    }

    return PROTO_HEX_RET_FRAME;
}

uint8_t ProtoHex_BuildFrame(uint8_t *buf, uint8_t dev_addr, uint8_t cmd,
                            uint16_t reg, uint16_t data, uint8_t seq, uint8_t is_ack)
{
    if (buf == NULL)
    {
        return 0U;
    }

    buf[0] = (uint8_t)((PROTO_HEX_VERSION << 4) | (dev_addr & 0x0FU));
    buf[1] = cmd;
    buf[2] = (uint8_t)(reg >> 8);
    buf[3] = (uint8_t)(reg & 0xFFU);
    buf[4] = (uint8_t)(data >> 8);
    buf[5] = (uint8_t)(data & 0xFFU);
    buf[6] = (uint8_t)((is_ack != 0U) ? (0x80U | (seq & 0x7FU)) : (seq & 0x7FU));
    buf[7] = ProtoHex_CRC8(buf, PROTO_HEX_FRAME_SIZE - 1U);

    return PROTO_HEX_FRAME_SIZE;
}

uint8_t ProtoHex_CRC8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0x00U;
    uint8_t i;
    uint8_t j;

    if (data == NULL)
    {
        return 0U;
    }

    for (i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0U; j < 8U; j++)
        {
            if ((crc & 0x80U) != 0U)
            {
                crc = (uint8_t)((crc << 1) ^ 0x07U);   /* 多项式 x^8+x^2+x+1 */
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

#endif /* APP_HEX_CMD_ENABLE */
