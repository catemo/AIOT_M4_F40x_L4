/**
 ******************************************************************************
 * @file    mod_wifi.c
 * @brief   平台层（platform）—— WiFi 统一入口实现（AT 语义 → 五个动作）
 *
 *  本文件只有一百来行，但它承担两件"薄转发做不到"的事：
 *    ① 状态归一：网络层的 OK / TIMEOUT / ERR_RESPONSE 三种结局，
 *       映射成上层关心的 OK / 超时 / 模块拒绝。上层据此决定
 *       "退避多久"与"要不要复位模块"，不用去认 AT 的关键字。
 *    ② 时机固定：TCP 建链成功后立刻切 MQTT 二进制接收模式。
 *       这一步放在这里，是为了让"上层忘了切"这种事故不可能发生。
 *
 *  ★ 本文件不打印、不重试、不睡眠（除了下层内部的等待）：
 *    它只回答"这个动作成没成"。决策留给 app_mqtt——谁懂全局谁决策。
 ******************************************************************************
 */
#include "module_cfg.h"

#if MOD_WIFI_LINK_ENABLE

#include "mod_wifi.h"
#include "net_esp8266.h"   /* 平台层允许依赖网络层，方向是 上→下 */

/* 收一次最多等多久：10ms。与主循环的 50ms 节拍配合，
 * 保证 Keepalive 的轮询足够频繁（keepalive 30s，要求 < 15s 一次） */
#define MOD_WIFI_RECV_SLICE_MS  10U

/* 网络层状态 → 平台层状态 */
static MOD_WIFI_Status_t mod_wifi_map(NET_ESP8266_Status_t st)
{
    switch (st)
    {
        case NET_ESP8266_OK:           return MOD_WIFI_OK;
        case NET_ESP8266_ERR_TIMEOUT:  return MOD_WIFI_ERR_TIMEOUT;
        case NET_ESP8266_ERR_RESPONSE: return MOD_WIFI_ERR_FAIL;
        default:                       return MOD_WIFI_ERR_PARAM;
    }
}

void MOD_WIFI_Init(void)
{
    net_esp8266_init();   /* 网络层：拉起 USART3 + 等模块上电稳定 */
}

MOD_WIFI_Status_t MOD_WIFI_TestAt(void)
{
    /* 普通指令 2 秒足够；模块刚上电时第一句可能被吃掉，所以给足 2 秒 */
    return mod_wifi_map(net_esp8266_test_at(2000U));
}

MOD_WIFI_Status_t MOD_WIFI_SetSta(void)
{
    return mod_wifi_map(net_esp8266_set_sta());
}

MOD_WIFI_Status_t MOD_WIFI_JoinAp(const char *ssid, const char *pwd)
{
    return mod_wifi_map(net_esp8266_join_ap(ssid, pwd));
}

MOD_WIFI_Status_t MOD_WIFI_OpenTcp(const char *host, uint16_t port)
{
    NET_ESP8266_Status_t st = net_esp8266_open_tcp(host, port);

    if (st != NET_ESP8266_OK)
    {
        return mod_wifi_map(st);
    }

    /* ★ 建链成功，立刻切 MQTT 二进制接收模式：
     *   从这一刻起 +IPD 里的字节不再留在 AT 响应缓冲，而是进独立载荷缓冲。
     *   晚一步切 = CONNACK 被当成 AT 文本吃掉 = "发了 CONNECT 却没回应" */
    net_esp8266_enter_mqtt_mode();
    net_esp8266_clear_closed();
    return MOD_WIFI_OK;
}

MOD_WIFI_Status_t MOD_WIFI_Send(const uint8_t *buf, uint16_t len)
{
    return mod_wifi_map(net_esp8266_send(buf, (uint32_t)len));
}

uint16_t MOD_WIFI_Recv(uint8_t *buf, uint16_t max)
{
    uint32_t got = net_esp8266_recv(buf, (uint32_t)max, MOD_WIFI_RECV_SLICE_MS);

    /* 超过 uint16 上限是不可能的（调用者的缓冲远远小于 64 KB），
     * 这里只做一次收敛，避免编译器的隐式截断告警 */
    if (got > 0xFFFFU)
    {
        got = 0xFFFFU;
    }
    return (uint16_t)got;
}

void MOD_WIFI_Close(void)
{
    (void)net_esp8266_close();
}

MOD_WIFI_Status_t MOD_WIFI_Reset(void)
{
    return mod_wifi_map(net_esp8266_reset());
}

uint8_t MOD_WIFI_IsTcpClosed(void)
{
    return net_esp8266_is_tcp_closed();
}

uint32_t MOD_WIFI_GetLastResp(uint8_t *buf, uint32_t cap)
{
    return net_esp8266_get_last_resp(buf, cap);
}

#endif /* MOD_WIFI_LINK_ENABLE */
