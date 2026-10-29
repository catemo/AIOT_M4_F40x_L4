/**
 ******************************************************************************
 * @file    mod_mqtt.c
 * @brief   平台层（platform）—— MQTT 3.1.1 报文编解码实现
 *
 *  整个 MQTT 的"骨架"只有三行：
 *      固定头 = 类型 << 4 | 标志        （1 字节）
 *      剩余长度 = 变长编码              （1~4 字节）
 *      内容 = 主题 + [报文 ID] + 载荷
 *  剩下的都是这三行的排列组合。所以本文件的所有构造函数都长一个样：
 *      往 s_body 里写字段 → 交给 send_packet() 补头 → 让 mod_wifi 发出去。
 *
 *  ★ 剩余长度是唯一"需要想一下"的地方：
 *    每字节低 7 位有效，最高位是"还有后续"。
 *      127          → 0x7F                （1 字节）
 *      128          → 0x80 0x01           （2 字节）
 *      258 = 2+256  → 0x82 0x02           （2 字节）
 *    上报一帧 200 字节左右的 JSON，长度字段只占 2 字节——
 *    这就是这套编码存在的意义：小报文不浪费，大报文也能表达。
 *
 *  ★ 收发都用固定缓冲（s_body / s_tx / s_rx），运行期一次 malloc 都没有：
 *    RTOS 任务里动态分配会把堆切碎，跑几天才炸，极难定位。
 *
 *  ★ QoS1 的两个附加值（本文件都实现了）：
 *    ① 报文 ID：每条 PUBLISH 带一个自增 ID，Broker 回 PUBACK 才算送达；
 *    ② 重复容忍：没收到 PUBACK 就重发，可能重复到达——
 *       华为云按属性覆盖，重复无副作用（这条是"能用 QoS1"的前提）。
 ******************************************************************************
 */
#include "module_cfg.h"

#if MOD_MQTT_ENABLE

#include "mod_mqtt.h"
#include "mod_wifi.h"      /* 平台层内部：下面只剩"发字节 / 收字节" */
#include "cmsis_os.h"
#include <string.h>

/* ========== 内部常量 ========== */
#define MOD_MQTT_RX_BUF_SIZE     256U    /* 收包正文缓冲（超出部分丢弃但会排空） */
#define MOD_MQTT_ACK_TIMEOUT_MS  2000U   /* 等 PUBACK / SUBACK 的上限           */
#define MOD_MQTT_CONN_TIMEOUT_MS 5000U   /* 等 CONNACK 的上限                   */
#define MOD_MQTT_POLL_SLICE_MS   10U     /* 单次收包最多等多久                  */
#define MOD_MQTT_EMPTY_RETRY     50U     /* 收正文时"连续没数据"的容忍次数       */

/* ========== 内部资源 ========== */
static uint8_t  s_body[MOD_MQTT_TX_BUF_SIZE];              /* 正文（不含固定头） */
static uint8_t  s_tx[MOD_MQTT_TX_BUF_SIZE + 8U];           /* 固定头 + 正文      */
static uint8_t  s_rx[MOD_MQTT_RX_BUF_SIZE];                /* 收到的正文         */
static uint16_t s_next_id = 1U;                            /* 报文 ID 自增       */
static uint8_t  s_connected;                               /* 1 = CONNACK 已成功 */

/* ========== 编码小工具 ========== */

/* 两字节大端序号（协议里所有长度、报文 ID 都是大端） */
static uint32_t put_u16(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)(val & 0xFFU);
    return 2U;
}

/* MQTT 的"字符串"：2 字节长度 + 内容（与 C 字符串的 '\0' 无关） */
static uint32_t put_str(uint8_t *dst, const char *str)
{
    uint32_t len = (uint32_t)strlen(str);

    (void)put_u16(dst, (uint16_t)len);
    memcpy(&dst[2], str, len);
    return 2U + len;
}

/* 剩余长度：变长编码（每字节 7 位有效，最高位表示还有后续） */
static uint32_t put_remain_len(uint8_t *dst, uint32_t len)
{
    uint32_t n = 0U;
    uint8_t  b;

    do
    {
        b = (uint8_t)(len % 128U);
        len /= 128U;
        if (len > 0U)
        {
            b |= 0x80U;   /* 还有后续 */
        }
        dst[n++] = b;
    } while (len > 0U);

    return n;
}

/* 取下一个报文 ID（跳过 0：0 在协议里表示"没有报文 ID"） */
static uint16_t next_packet_id(void)
{
    s_next_id++;
    if (s_next_id == 0U)
    {
        s_next_id = 1U;
    }
    return s_next_id;
}

/* 补固定头并发送：hdr = 类型<<4 | 标志；body_len = s_body 里已写好的字节数 */
static MOD_MQTT_Status_t send_packet(uint8_t hdr, uint32_t body_len)
{
    uint32_t n = 0U;
    uint32_t total;

    s_tx[n++] = hdr;
    n += put_remain_len(&s_tx[n], body_len);

    if ((n + body_len) > sizeof(s_tx))
    {
        return MOD_MQTT_ERR_OVERFLOW;   /* 拼出来的报文超过发送缓冲 */
    }
    memcpy(&s_tx[n], s_body, body_len);
    total = n + body_len;

    if (MOD_WIFI_Send(s_tx, (uint16_t)total) != MOD_WIFI_OK)
    {
        s_connected = 0U;               /* 发不出去 = 链路有问题 */
        return MOD_MQTT_ERR_LINK;
    }
    return MOD_MQTT_OK;
}

/* ========== 解码小工具 ========== */

/* 等一个字节：超时或对端关闭都返回 0 */
static uint8_t wait_byte(uint8_t *b, uint32_t timeout_ms)
{
    uint32_t elapsed = 0U;

    for (;;)
    {
        if (MOD_WIFI_Recv(b, 1U) == 1U)
        {
            return 1U;
        }
        if ((MOD_WIFI_IsTcpClosed() != 0U) || (elapsed >= timeout_ms))
        {
            return 0U;
        }
        osDelay(MOD_MQTT_POLL_SLICE_MS);
        elapsed += MOD_MQTT_POLL_SLICE_MS;
    }
}

/* 读一帧：固定头 + 剩余长度 + 正文（正文进 s_rx）。
 * 返回 1 = 读到一个完整帧；0 = 没数据 / 断了 / 正文没收全 */
static uint8_t read_frame(uint8_t *type, uint8_t *flags, uint32_t *body_len, uint32_t timeout_ms)
{
    uint8_t  hdr = 0U;
    uint8_t  b;
    uint32_t rl = 0U;
    uint32_t mult = 1U;
    uint32_t i;
    uint32_t keep;
    uint32_t got = 0U;
    uint32_t drain;
    uint32_t empty = 0U;

    if (wait_byte(&hdr, timeout_ms) == 0U)
    {
        return 0U;
    }

    /* 剩余长度：最多 4 字节（协议规定），每字节 7 位 */
    for (i = 0U; i < 4U; i++)
    {
        if (wait_byte(&b, timeout_ms) == 0U)
        {
            return 0U;
        }
        rl += (uint32_t)(b & 0x7FU) * mult;
        if ((b & 0x80U) == 0U)
        {
            break;
        }
        mult *= 128U;
    }

    /* 正文：能装多少装多少，装不下的排空（否则下一帧会从头错位） */
    keep = (rl < sizeof(s_rx)) ? rl : (uint32_t)sizeof(s_rx);
    while (got < keep)
    {
        uint16_t n = MOD_WIFI_Recv(&s_rx[got], (uint16_t)(keep - got));
        if (n == 0U)
        {
            if ((MOD_WIFI_IsTcpClosed() != 0U) || (++empty >= MOD_MQTT_EMPTY_RETRY))
            {
                return 0U;   /* 断线，或正文迟迟不来：这一帧放弃 */
            }
            osDelay(MOD_MQTT_POLL_SLICE_MS);
            continue;
        }
        got += n;
        empty = 0U;
    }

    drain = rl - keep;
    while (drain > 0U)
    {
        uint8_t  junk[32];
        uint32_t d = (drain > sizeof(junk)) ? (uint32_t)sizeof(junk) : drain;
        uint16_t n = MOD_WIFI_Recv(junk, (uint16_t)d);

        if (n == 0U)
        {
            if (++empty >= MOD_MQTT_EMPTY_RETRY)
            {
                return 0U;
            }
            osDelay(MOD_MQTT_POLL_SLICE_MS);
            continue;
        }
        drain -= n;
        empty = 0U;
    }

    *type = (uint8_t)(hdr >> 4);
    *flags = (uint8_t)(hdr & 0x0FU);
    *body_len = rl;
    return 1U;
}

/* 回 PUBACK（收到 QoS1 的 PUBLISH 必须回，否则 Broker 会一直重发） */
static void send_puback(uint16_t packet_id)
{
    uint8_t ack[4];

    ack[0] = 0x40U;                       /* PUBACK */
    ack[1] = 0x02U;                       /* 剩余长度 = 2 */
    (void)put_u16(&ack[2], packet_id);
    (void)MOD_WIFI_Send(ack, 4U);
}

/* ==========================================================================
 *  对外接口
 * ========================================================================== */

void MOD_MQTT_Init(void)
{
    s_next_id   = 1U;
    s_connected = 0U;
}

MOD_MQTT_Status_t MOD_MQTT_Connect(const char *client_id,
                                   const char *username,
                                   const char *password,
                                   uint16_t    keepalive_s)
{
    uint32_t n = 0U;
    uint32_t elapsed = 0U;
    uint8_t  type;
    uint8_t  flags;
    uint32_t body_len;
    MOD_MQTT_Status_t st;

    if ((client_id == NULL) || (username == NULL) || (password == NULL))
    {
        return MOD_MQTT_ERR_PARAM;
    }

    /* ---------- 可变头 ---------- */
    n += put_str(&s_body[n], "MQTT");          /* 协议名                     */
    s_body[n++] = 4U;                          /* 协议级别：3.1.1 = 4        */
    /* 连接标志：CleanSession(0x02) + Username(0x80) + Password(0x40)
     * ★ CleanSession = 1：每次连接都是干净会话，Broker 不替我们存离线消息。
     *   离线缓存是本工程自己在 RAM 里做的（见 app_mqtt.c 的 16 条队列）。 */
    s_body[n++] = (uint8_t)(0x02U | 0x80U | 0x40U);
    n += put_u16(&s_body[n], keepalive_s);

    /* ---------- 载荷：三元组按 ClientId / Username / Password 顺序 ---------- */
    n += put_str(&s_body[n], client_id);
    n += put_str(&s_body[n], username);
    n += put_str(&s_body[n], password);

    st = send_packet(0x10U, n);               /* CONNECT */
    if (st != MOD_MQTT_OK)
    {
        return st;
    }

    /* ---------- 等 CONNACK ---------- */
    while (elapsed < MOD_MQTT_CONN_TIMEOUT_MS)
    {
        if (read_frame(&type, &flags, &body_len, MOD_MQTT_POLL_SLICE_MS) != 0U)
        {
            if (type == MOD_MQTT_TYPE_CONNACK)
            {
                /* 正文：连接确认标志(1) + 返回码(1)；返回码 0 才是成功 */
                if ((body_len >= 2U) && (s_rx[1] == 0U))
                {
                    s_connected = 1U;
                    return MOD_MQTT_OK;
                }
                return MOD_MQTT_ERR_REJECT;   /* 三元组不对：重连也没用 */
            }
            /* 其它报文（极少见）：忽略，继续等 CONNACK */
        }
        if (MOD_WIFI_IsTcpClosed() != 0U)
        {
            return MOD_MQTT_ERR_LINK;
        }
        osDelay(MOD_MQTT_POLL_SLICE_MS);
        elapsed += MOD_MQTT_POLL_SLICE_MS;
    }

    /* 超时：多半是半开连接（TCP 通但 Broker 不理）→ 上层会关 TCP 重建 */
    return MOD_MQTT_ERR_TIMEOUT;
}

MOD_MQTT_Status_t MOD_MQTT_Subscribe(const char *topic, uint8_t qos)
{
    uint32_t n = 0U;
    uint32_t elapsed = 0U;
    uint16_t pid;
    uint8_t  type;
    uint8_t  flags;
    uint32_t body_len;
    MOD_MQTT_Status_t st;

    if ((topic == NULL) || (qos > 1U))
    {
        return MOD_MQTT_ERR_PARAM;
    }

    pid = next_packet_id();
    n += put_u16(&s_body[n], pid);
    n += put_str(&s_body[n], topic);
    s_body[n++] = qos;                 /* 请求的 QoS */

    st = send_packet(0x82U, n);        /* SUBSCRIBE（固定头低 4 位必须是 0b0010） */
    if (st != MOD_MQTT_OK)
    {
        return st;
    }

    while (elapsed < MOD_MQTT_ACK_TIMEOUT_MS)
    {
        if (read_frame(&type, &flags, &body_len, MOD_MQTT_POLL_SLICE_MS) != 0U)
        {
            if ((type == MOD_MQTT_TYPE_SUBACK) && (body_len >= 3U))
            {
                /* 返回码 0x80 表示订阅失败（华为云常见原因：主题拼错/无权限） */
                return (s_rx[2] == 0x80U) ? MOD_MQTT_ERR_REJECT : MOD_MQTT_OK;
            }
        }
        if (MOD_WIFI_IsTcpClosed() != 0U)
        {
            return MOD_MQTT_ERR_LINK;
        }
        osDelay(MOD_MQTT_POLL_SLICE_MS);
        elapsed += MOD_MQTT_POLL_SLICE_MS;
    }
    return MOD_MQTT_ERR_TIMEOUT;
}

MOD_MQTT_Status_t MOD_MQTT_Publish(const char *topic, const char *payload)
{
    uint32_t tlen;
    uint32_t plen;
    uint32_t n = 0U;
    uint32_t need;
    uint16_t pid;
    uint8_t  type;
    uint8_t  flags;
    uint32_t body_len;
    uint32_t elapsed = 0U;
    MOD_MQTT_Status_t st;

    if ((topic == NULL) || (payload == NULL))
    {
        return MOD_MQTT_ERR_PARAM;
    }
    if (s_connected == 0U)
    {
        return MOD_MQTT_ERR_LINK;
    }

    tlen = (uint32_t)strlen(topic);
    plen = (uint32_t)strlen(payload);
    need = 2U + tlen + 2U + plen;          /* 主题长度 + 主题 + 报文ID + 载荷 */
    if (need > sizeof(s_body))
    {
        return MOD_MQTT_ERR_OVERFLOW;      /* 主题或 JSON 太长，先改 cfg */
    }

    n += put_str(&s_body[n], topic);
    pid = next_packet_id();
    n += put_u16(&s_body[n], pid);
    memcpy(&s_body[n], payload, plen);
    n += plen;

    st = send_packet(0x32U, n);            /* PUBLISH + QoS1（0x32 = 0x30|0x02） */
    if (st != MOD_MQTT_OK)
    {
        return st;
    }

    /* 等 PUBACK：收到才算送达。等不到就返回超时，由上层决定重发还是重连 */
    while (elapsed < MOD_MQTT_ACK_TIMEOUT_MS)
    {
        if (read_frame(&type, &flags, &body_len, MOD_MQTT_POLL_SLICE_MS) != 0U)
        {
            if ((type == MOD_MQTT_TYPE_PUBACK) && (body_len >= 2U))
            {
                uint16_t ack_id = (uint16_t)(((uint16_t)s_rx[0] << 8) | s_rx[1]);
                if (ack_id == pid)
                {
                    return MOD_MQTT_OK;
                }
            }
            /* 顺路收到别的报文（比如云端命令）先记着不处理：
             * 主循环下一圈会再来收——这里不抢 Poll 的活 */
        }
        if (MOD_WIFI_IsTcpClosed() != 0U)
        {
            s_connected = 0U;
            return MOD_MQTT_ERR_LINK;
        }
        osDelay(MOD_MQTT_POLL_SLICE_MS);
        elapsed += MOD_MQTT_POLL_SLICE_MS;
    }
    return MOD_MQTT_ERR_TIMEOUT;
}

MOD_MQTT_Status_t MOD_MQTT_Ping(void)
{
    uint8_t  ping[2];
    uint8_t  type;
    uint8_t  flags;
    uint32_t body_len;
    uint32_t elapsed = 0U;

    ping[0] = 0xC0U;   /* PINGREQ */
    ping[1] = 0x00U;   /* 剩余长度 = 0：心跳只有两个字节 */
    if (MOD_WIFI_Send(ping, 2U) != MOD_WIFI_OK)
    {
        s_connected = 0U;
        return MOD_MQTT_ERR_LINK;
    }

    while (elapsed < MOD_MQTT_ACK_TIMEOUT_MS)
    {
        if (read_frame(&type, &flags, &body_len, MOD_MQTT_POLL_SLICE_MS) != 0U)
        {
            if (type == MOD_MQTT_TYPE_PINGRESP)
            {
                return MOD_MQTT_OK;
            }
        }
        if (MOD_WIFI_IsTcpClosed() != 0U)
        {
            s_connected = 0U;
            return MOD_MQTT_ERR_LINK;
        }
        osDelay(MOD_MQTT_POLL_SLICE_MS);
        elapsed += MOD_MQTT_POLL_SLICE_MS;
    }
    return MOD_MQTT_ERR_TIMEOUT;
}

uint8_t MOD_MQTT_Poll(MOD_MQTT_Msg_t *msg)
{
    uint8_t  type;
    uint8_t  flags;
    uint32_t body_len;
    uint32_t avail;

    if (msg == NULL)
    {
        return 0U;
    }
    if (read_frame(&type, &flags, &body_len, MOD_MQTT_POLL_SLICE_MS) == 0U)
    {
        return 0U;
    }

    avail = (body_len < (uint32_t)sizeof(s_rx)) ? body_len : (uint32_t)sizeof(s_rx);

    if (type != MOD_MQTT_TYPE_PUBLISH)
    {
        /* 非业务报文就地消化，不打扰上层 */
        if (type == MOD_MQTT_TYPE_CONNACK)
        {
            if ((avail >= 2U) && (s_rx[1] != 0U))
            {
                s_connected = 0U;   /* Broker 拒绝了我们（比如 clientId 被顶掉） */
            }
        }
        return 0U;
    }

    /* ---------- PUBLISH ---------- */
    msg->type  = type;
    msg->qos   = (uint8_t)((flags >> 1) & 0x03U);
    msg->code  = 0U;
    msg->packet_id = 0U;
    msg->topic_len = 0U;
    msg->payload_len = 0U;
    msg->topic[0] = '\0';
    msg->payload[0] = '\0';

    {
        uint32_t off = 0U;
        uint32_t tl;

        if (avail < 2U)
        {
            return 0U;                       /* 连主题长度都不全：丢弃 */
        }
        tl = (uint32_t)(((uint16_t)s_rx[0] << 8) | s_rx[1]);
        off = 2U;
        if ((off + tl) > avail)
        {
            return 0U;                       /* 主题不完整：丢弃 */
        }

        /* 主题：长度超过缓冲就截断（但 off 仍按真实长度前进，报文边界才不会错） */
        msg->topic_len = (uint16_t)((tl < (MOD_MQTT_TOPIC_MAX - 1U)) ? tl : (MOD_MQTT_TOPIC_MAX - 1U));
        memcpy(msg->topic, &s_rx[off], msg->topic_len);
        msg->topic[msg->topic_len] = '\0';
        off += tl;

        if (msg->qos > 0U)
        {
            if ((off + 2U) > avail)
            {
                return 0U;
            }
            msg->packet_id = (uint16_t)(((uint16_t)s_rx[off] << 8) | s_rx[off + 1U]);
            off += 2U;
        }

        if (off < avail)
        {
            uint32_t plen = avail - off;
            msg->payload_len = (uint16_t)((plen < (MOD_MQTT_MSG_MAX - 1U)) ? plen : (MOD_MQTT_MSG_MAX - 1U));
            memcpy(msg->payload, &s_rx[off], msg->payload_len);
            msg->payload[msg->payload_len] = '\0';
        }
    }

    /* QoS1 的入站报文必须回 PUBACK，否则 Broker 会一直重发同一条命令 */
    if (msg->qos == 1U)
    {
        send_puback(msg->packet_id);
    }
    return 1U;
}

uint8_t MOD_MQTT_IsConnected(void)
{
    return s_connected;
}

void MOD_MQTT_MarkDisconnected(void)
{
    s_connected = 0U;
}

#endif /* MOD_MQTT_ENABLE */
