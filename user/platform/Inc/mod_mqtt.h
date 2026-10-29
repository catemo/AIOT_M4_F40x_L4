/**
 ******************************************************************************
 * @file    mod_mqtt.h
 * @brief   平台层（platform）—— MQTT 报文编解码（自己写，不引第三方库）
 *
 *  本层只管一件事：把"要发什么"变成符合 MQTT 3.1.1 的字节流，
 *  以及把收到的字节流解回"谁发来的什么内容"。
 *  它不知道华为云的物模型，也不知道灯该怎么亮——那是应用层的事。
 *
 *  ★ 只做四种报文（够上云，且每一种都能讲清楚）：
 *      CONNECT    0x10   建会话：协议名 + Level4 + Keepalive + 三元组
 *      PUBLISH    0x30/0x32  上报：主题 + 载荷（QoS1 多一个报文 ID）
 *      SUBSCRIBE  0x82   订阅：报文 ID + 主题 + 请求 QoS
 *      PINGREQ    0xC0 0x00  心跳：两个字节
 *    不做 QoS2、遗嘱、TLS——要做得加报文类型，不用动上面两层。
 *
 *  ★ 为什么不用 Paho：
 *    官方库 MQTTClient + MQTTPacket 起步就是七八个文件，还要自己实现
 *    Timer / Network 两个平台抽象层；它的 SUCCESS 宏还会和 HAL 的枚举撞名。
 *    出问题时得先读第三方的状态机，才能读自己的代码。
 *    教学阶段：可读性 > 功能完备性。自写只有几百行，每一行都能讲。
 *
 *  ★ MQTT 报文只有三样东西：
 *      ① 固定头：1 字节 = 类型<<4 | 标志
 *      ② 剩余长度：1~4 字节变长编码（每字节低 7 位有效，最高位表示"还有后续"）
 *      ③ 内容：主题 + 载荷 / 报文 ID
 *    所以"自己写并不难"不是逞强，是这套协议本来就简单：
 *    写类型、写长度、写字段，没有对象、没有回调栈，出问题直接看 hex。
 ******************************************************************************
 */
#ifndef MOD_MQTT_H
#define MOD_MQTT_H

#include <stdint.h>
#include "module_cfg.h"   /* 报文长度上限来自 cfg 横切层 */

/* ===================== 状态码 ===================== */
typedef enum
{
    MOD_MQTT_OK = 0,
    MOD_MQTT_ERR_TIMEOUT,   /* 报文发出去了，但等不到对应的回应（CONNACK/SUBACK） */
    MOD_MQTT_ERR_LINK,      /* 链路层失败：发不出去，或 TCP 已断                */
    MOD_MQTT_ERR_REJECT,    /* 对端明确拒绝：CONNACK rc≠0 / SUBACK 返回 0x80    */
    MOD_MQTT_ERR_PARAM,     /* 参数非法（空指针 / 超长）                       */
    MOD_MQTT_ERR_OVERFLOW   /* 报文超过发送缓冲（主题 + 载荷太长）             */
} MOD_MQTT_Status_t;

/* ===================== 报文类型（固定头高 4 位） ===================== */
#define MOD_MQTT_TYPE_CONNACK   2U
#define MOD_MQTT_TYPE_PUBLISH   3U
#define MOD_MQTT_TYPE_PUBACK    4U
#define MOD_MQTT_TYPE_SUBACK    9U
#define MOD_MQTT_TYPE_PINGRESP  13U

/* ===================== 收到的报文 ===================== */
/* ★ 用固定缓冲而不是动态分配：RTOS 任务里绝不在运行期 malloc。
 *   代价是这个结构约 250 字节，所以调用者应当把它做成 static（见 app_mqtt.c）。 */
typedef struct
{
    uint8_t  type;                          /* 报文类型：2/3/4/9/13                */
    uint8_t  qos;                           /* PUBLISH 的 QoS                     */
    uint8_t  code;                          /* CONNACK 返回码 / SUBACK 返回码      */
    uint16_t packet_id;                     /* QoS>0 时才有                       */
    uint16_t topic_len;
    uint16_t payload_len;
    char     topic[MOD_MQTT_TOPIC_MAX];     /* 以 '\0' 结尾，方便上层做前缀比较     */
    char     payload[MOD_MQTT_MSG_MAX];     /* 以 '\0' 结尾，JSON 解析按字符串处理  */
} MOD_MQTT_Msg_t;

/*----------------------------------------------------------------------------
 * 函数功能：初始化协议层（报文 ID 归零、连接标志清零）
 * 参数    ：无
 * 返回值  ：无
 * 调用处  ：app_mqtt.c 的 app_mqtt_task() 启动序列
 * 注意事项：本函数不碰任何硬件，也不建链
 *----------------------------------------------------------------------------*/
void MOD_MQTT_Init(void);

/*----------------------------------------------------------------------------
 * 函数功能：建 MQTT 会话（发 CONNECT，等 CONNACK）
 * 参数    ：client_id  —— 客户端标识（华为云要求与 device_id 一致或用其生成值）
 *           username   —— 用户名（华为云：device_id）
 *           password   —— 密码（华为云：用密钥算出的签名）
 *           keepalive_s—— 心跳周期（秒）
 * 返回值  ：MOD_MQTT_OK / ERR_TIMEOUT / ERR_LINK / ERR_REJECT
 * 调用处  ：app_mqtt 的连接状态机第 ⑤ 段
 * 注意事项：① ★ 失败时返回 ERR_REJECT 还是 ERR_TIMEOUT，决定了上层怎么处理：
 *              REJECT = 三元组不对（重连也没用，得改配置）；
 *              TIMEOUT = 半开连接或链路不通（该关 TCP 重建）
 *           ② Clean Session 置 1：每次连接都是干净会话，Broker 不替我们存离线消息
 *              （离线缓存是本工程自己在 RAM 里做的，见 app_mqtt.c）
 *----------------------------------------------------------------------------*/
MOD_MQTT_Status_t MOD_MQTT_Connect(const char *client_id,
                                   const char *username,
                                   const char *password,
                                   uint16_t    keepalive_s);

/*----------------------------------------------------------------------------
 * 函数功能：订阅主题（发 SUBSCRIBE，等 SUBACK）
 * 参数    ：topic —— 主题（可含通配符 + / #）；qos —— 请求的服务质量
 * 返回值  ：MOD_MQTT_OK / ERR_TIMEOUT / ERR_LINK / ERR_REJECT
 * 调用处  ：app_mqtt 的连接状态机第 ⑥ 段
 * 注意事项：华为云的命令下发主题是 $oc/devices/{device_id}/sys/commands/#
 *           订阅成功才可能收到云端命令——漏了这一步的现象是"云端显示在线、
 *           但下发命令一直转圈"
 *----------------------------------------------------------------------------*/
MOD_MQTT_Status_t MOD_MQTT_Subscribe(const char *topic, uint8_t qos);

/*----------------------------------------------------------------------------
 * 函数功能：发布一条消息（QoS1：发 PUBLISH，等 PUBACK）
 * 参数    ：topic   —— 主题
 *           payload —— 载荷（JSON 文本，以 '\0' 结尾）
 * 返回值  ：MOD_MQTT_OK / ERR_TIMEOUT / ERR_LINK / ERR_OVERFLOW
 * 调用处  ：app_mqtt 的周期上报与命令响应
 * 注意事项：① QoS1 的两个附加值：报文 ID（每条自增，Broker 回 PUBACK 才算送达）；
 *              重复容忍（没收到 PUBACK 就重发，可能重复到达——
 *              华为云按属性覆盖，重复无副作用）
 *           ② 载荷是文本 JSON，所以这里可以用 strlen；
 *              但发送函数传的是"长度 + 指针"，二进制安全靠的是长度
 *----------------------------------------------------------------------------*/
MOD_MQTT_Status_t MOD_MQTT_Publish(const char *topic, const char *payload);

/*----------------------------------------------------------------------------
 * 函数功能：发心跳（PINGREQ，两个字节）并等 PINGRESP
 * 参数    ：无
 * 返回值  ：MOD_MQTT_OK / ERR_TIMEOUT / ERR_LINK
 * 调用处  ：app_mqtt 主循环的 Poll 内部（按空闲时间自动触发）
 * 注意事项：★ 心跳的纪律：Keepalive 30s，则两次报文之间【必须】小于 15s。
 *           主循环 osDelay(50) 就是为了让它够频繁——间隔超过 keepalive/2，
 *           Broker 会直接断开，而设备侧看不到任何错误提示
 *----------------------------------------------------------------------------*/
MOD_MQTT_Status_t MOD_MQTT_Ping(void);

/*----------------------------------------------------------------------------
 * 函数功能：收一包（解析 CONNACK / SUBACK / PUBLISH / PUBACK / PINGRESP）
 * 参数    ：msg —— 输出：解析结果（PUBLISH 时含主题与载荷）
 * 返回值  ：1 = 收到一包 PUBLISH（msg 有效）；0 = 没收到业务消息
 * 调用处  ：app_mqtt 主循环
 * 注意事项：① 非 PUBLISH 的报文（PUBACK / PINGRESP / SUBACK）在这里被就地消化，
 *              不打扰上层——上层只关心"有没有命令下来"
 *           ② 收到 QoS1 的 PUBLISH 会自动回 PUBACK，否则 Broker 会一直重发
 *           ③ 函数内部最多等 10ms 就走，绝不死等：主循环还有别的活要干
 *----------------------------------------------------------------------------*/
uint8_t MOD_MQTT_Poll(MOD_MQTT_Msg_t *msg);

/*----------------------------------------------------------------------------
 * 函数功能：查询会话是否已建立（CONNECT 成功过、且没被关闭）
 * 参数    ：无
 * 返回值  ：1 = 已连接
 * 调用处  ：app_mqtt 主循环（决定是"上报"还是"重连"）
 *----------------------------------------------------------------------------*/
uint8_t MOD_MQTT_IsConnected(void);

/*----------------------------------------------------------------------------
 * 函数功能：把会话标记为断开（链路上出现不可恢复错误时由上层调用）
 * 参数    ：无
 * 返回值  ：无
 * 调用处  ：app_mqtt 在 Publish / Poll 判定链路失败后
 *----------------------------------------------------------------------------*/
void MOD_MQTT_MarkDisconnected(void);

#endif /* MOD_MQTT_H */
