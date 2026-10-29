/**
 ******************************************************************************
 * @file    app_mqtt.c
 * @brief   应用层（app）—— WiFi-MQTT 上云任务实现
 *
 *  三段式结构，从上到下读就是它的全部逻辑：
 *    ① 小工具：自己拼字符串 / 拼 JSON / 拼主题（不引 printf）
 *    ② 连接：六段依次做，任一段失败就退避重连（第 5 次复位模块）
 *    ③ 主循环：周期上报 → 收一包 → 执行命令并回响应 → 心跳 → 睡 50ms
 *
 *  ★ 上报的是"快照"，不是"新读一次"：
 *    DHT11 2 秒采一次，本任务只读 APP_DHT11_GetSnapshot()。
 *    采集与上传因此互不拖累——这是分层解耦最直观的一处收益。
 *
 *  ★ 命令落到哪：还走 1.3 章节那条老路
 *    paras.led = 1 → APP_BLINK_On(LED1)：与串口命令同一个入口，
 *    手动接管的语义早在 1.3 就写好了，云端只是多了一个"遥控器"。
 *    led = 2 → PER_LED_On(PER_LED2)：LED2 一直是 app_link 的湿度指示灯，
 *    没有闪烁状态机（它不闪），所以这里直接落到感知层。
 *
 *  ★ 离线缓存为什么必须有界：
 *    16 条 × 5 秒 ≈ 80 秒的断网窗口，刚好覆盖"路由器重启"这段典型时长。
 *    不设上限的队列会把 128 KB RAM 吃光——Device 挂掉比丢几条数据严重得多。
 *
 *  ★ 心跳纪律：Keepalive 30s → 两次报文之间必须小于 15s。
 *    主循环 osDelay(50) 就是为了让 Poll 够频繁；心跳靠"空闲时发 PINGREQ"
 *    维持，而不是靠某个宏自己定时。
 ******************************************************************************
 */
#include "module_cfg.h"

#if APP_MQTT_ENABLE

#include "app_mqtt.h"
#include "mod_mqtt.h"      /* 平台层：报文编解码 */
#include "mod_wifi.h"      /* 平台层：五个语义动作 */
#include "mod_uart1.h"     /* 平台层：调试打印只认这个入口 */
#include "cmsis_os.h"
#include <string.h>

#if APP_DHT11_ENABLE
#include "app_dht11.h"     /* 同层：温湿度快照（只读） */
#endif
#if APP_BLINK_ENABLE
#include "app_blink.h"     /* 同层：LED1 的入口（与串口命令同一个） */
#endif
#if PER_LED_ENABLE
#include "per_led.h"       /* 感知层：LED2 没有状态机，直接落这里 */
#endif

/* ========== 内部常量 ========== */
#define APP_MQTT_PRINT_BUF_SIZE   144U   /* 一行日志的最大长度（含主题时会很长） */
#define APP_MQTT_HEX_DUMP_MAX     32U    /* 失败时最多打印多少字节原始响应 */
#define APP_MQTT_FLUSH_GAP_MS     500U   /* 补传节拍：离线队列每 500ms 放一条 */
#define APP_MQTT_LOOP_MS          50U    /* 主循环节拍：Keepalive 靠它维持     */

/* ========== 内部状态 ========== */
static MOD_MQTT_Msg_t s_msg;                              /* 收包结果（static：约 250 B，不放栈） */
static char           s_payload[APP_MQTT_PAYLOAD_MAX];    /* 上报 JSON                          */
static char           s_topic[MOD_MQTT_TOPIC_MAX];        /* 主题（上报 / 订阅 / 响应共用）      */
static uint32_t       s_last_pub;                         /* 上次上报时刻（tick）                */
static uint32_t       s_last_tx;                          /* 上次发出报文的时刻（心跳用）         */
static uint32_t       s_last_flush;                       /* 上次补传时刻                        */
static uint32_t       s_publish_period_ms = APP_MQTT_PUBLISH_PERIOD_MS; /* 上报周期：云端属性设置可改，掉电恢复默认 */
static uint8_t        s_fail_count;                       /* 连续连接失败次数（退避用）           */

/* 离线缓存：断网期间的数据进这里，恢复后按序补传；队满丢最旧 */
typedef struct
{
    uint8_t temp_int;
    uint8_t temp_dec;
    uint8_t humi_int;
    uint8_t humi_dec;
} app_mqtt_cache_item_t;

static app_mqtt_cache_item_t s_cache[APP_MQTT_OFFLINE_CACHE_MAX];
static uint16_t s_cache_head;    /* 读 */
static uint16_t s_cache_tail;    /* 写 */
static uint16_t s_cache_count;

/* ==========================================================================
 *  一、小工具：拼字符串 / 拼 JSON（不引 printf）
 * ========================================================================== */

static uint32_t app_cat(char *dst, uint32_t pos, uint32_t cap, const char *src)
{
    while ((src != NULL) && (*src != '\0') && ((pos + 1U) < cap))
    {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
    return pos;
}

static uint32_t app_cat_u32(char *dst, uint32_t pos, uint32_t cap, uint32_t val)
{
    char     tmp[10];
    uint32_t n = 0U;

    if (val == 0U)
    {
        tmp[n++] = '0';
    }
    while (val > 0U)
    {
        tmp[n++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while ((n > 0U) && ((pos + 1U) < cap))
    {
        dst[pos++] = tmp[--n];
    }
    dst[pos] = '\0';
    return pos;
}

/* 拼"整数.一位小数"：DHT11 的数据天然就是 int + dec，正好一位 */
static uint32_t app_cat_dec1(char *dst, uint32_t pos, uint32_t cap,
                             uint8_t int_part, uint8_t dec_part)
{
    pos = app_cat_u32(dst, pos, cap, (uint32_t)int_part);
    if ((pos + 1U) < cap)
    {
        dst[pos++] = '.';
    }
    if ((pos + 1U) < cap)
    {
        dst[pos++] = (char)('0' + (dec_part % 10U));
    }
    dst[pos] = '\0';
    return pos;
}

/* 十六进制字符（失败现场 dump 用） */
static char app_hex(unsigned v)
{
    v &= 0x0FU;
    return (char)((v < 10U) ? ('0' + v) : ('A' + (v - 10U)));
}

/*----------------------------------------------------------------------------
 * 把上一次 AT 操作的原始响应打出来（排障第一现场）
 *
 * 读法三条判据：
 *   全空   → 接线（TX/RX 没交叉）或供电（EN 悬空 / 电流不够）
 *   有乱码 → 波特率不对
 *   有内容 → 模块活着，只是这条指令不认（看内容就知道卡在哪一句）
 *----------------------------------------------------------------------------*/
static void app_mqtt_dump_resp(const char *tag)
{
    uint8_t  raw[APP_MQTT_HEX_DUMP_MAX];
    char     line[APP_MQTT_PRINT_BUF_SIZE];
    uint32_t n;
    uint32_t i;
    uint32_t pos = 0U;

    n = MOD_WIFI_GetLastResp(raw, sizeof(raw));

    pos = app_cat(line, pos, sizeof(line), "[MQTT] ");
    pos = app_cat(line, pos, sizeof(line), tag);
    pos = app_cat(line, pos, sizeof(line), " resp=");
    pos = app_cat_u32(line, pos, sizeof(line), n);

    if (n == 0U)
    {
        pos = app_cat(line, pos, sizeof(line), " (empty: check TX/RX & power)\r\n");
        MOD_UART1_SendString(line);
        return;
    }

    pos = app_cat(line, pos, sizeof(line), ": ");
    for (i = 0U; (i < n) && ((pos + 4U) < sizeof(line)); i++)
    {
        line[pos++] = app_hex(raw[i] >> 4);
        line[pos++] = app_hex(raw[i]);
        line[pos++] = ' ';
    }
    line[pos++] = '|';
    line[pos++] = ' ';
    for (i = 0U; (i < n) && ((pos + 2U) < sizeof(line)); i++)
    {
        char c = (char)raw[i];
        line[pos++] = ((c >= 0x20) && (c < 0x7F)) ? c : '.';
    }
    line[pos++] = '\r';
    line[pos++] = '\n';
    line[pos] = '\0';
    MOD_UART1_SendString(line);
}

/* ==========================================================================
 *  二、主题与载荷
 * ========================================================================== */

/* 上报主题：$oc/devices/{device_id}/sys/properties/report */
static void app_mqtt_build_topic_report(void)
{
    uint32_t n = 0U;

    n = app_cat(s_topic, n, sizeof(s_topic), "$oc/devices/");
    n = app_cat(s_topic, n, sizeof(s_topic), APP_MQTT_DEVICE_ID);
    (void)app_cat(s_topic, n, sizeof(s_topic), "/sys/properties/report");
}

/* 订阅主题：$oc/devices/{device_id}/sys/commands/#
 * ★ 末尾的 # 是通配符：云端给每条命令都带一个 request_id 后缀，
 *   不订阅 /# 就只能收到"恰好叫那个名字"的主题——实际一条也收不到 */
static void app_mqtt_build_topic_cmd_sub(void)
{
    uint32_t n = 0U;

    n = app_cat(s_topic, n, sizeof(s_topic), "$oc/devices/");
    n = app_cat(s_topic, n, sizeof(s_topic), APP_MQTT_DEVICE_ID);
    (void)app_cat(s_topic, n, sizeof(s_topic), "/sys/commands/#");
}

/* 响应主题：$oc/devices/{device_id}/sys/commands/response/request_id={rid}
 * ★ request_id 必须与下发时那条完全一致，否则云端匹配不到"命令-响应"这一对，
 *   控制台上就会一直显示"等待响应" */
static void app_mqtt_build_topic_resp(const char *request_id)
{
    uint32_t n = 0U;

    n = app_cat(s_topic, n, sizeof(s_topic), "$oc/devices/");
    n = app_cat(s_topic, n, sizeof(s_topic), APP_MQTT_DEVICE_ID);
    n = app_cat(s_topic, n, sizeof(s_topic), "/sys/commands/response/request_id=");
    (void)app_cat(s_topic, n, sizeof(s_topic), request_id);
}

/* 属性设置订阅主题：$oc/devices/{device_id}/sys/properties/set/#
 * ★ 华为云"属性设置"下行走这个主题（控制台 → 设备 → 属性设置），
 *   载荷是物模型可写属性的 JSON，与命令下发是两条独立通道 */
static void app_mqtt_build_topic_set_sub(void)
{
    uint32_t n = 0U;

    n = app_cat(s_topic, n, sizeof(s_topic), "$oc/devices/");
    n = app_cat(s_topic, n, sizeof(s_topic), APP_MQTT_DEVICE_ID);
    (void)app_cat(s_topic, n, sizeof(s_topic), "/sys/properties/set/#");
}

/* 属性设置响应主题：$oc/devices/{device_id}/sys/properties/set/response/request_id={rid}
 * ★ 与命令响应同理：rid 必须原样带回，否则云端"属性设置"一直转圈 */
static void app_mqtt_build_topic_set_resp(const char *request_id)
{
    uint32_t n = 0U;

    n = app_cat(s_topic, n, sizeof(s_topic), "$oc/devices/");
    n = app_cat(s_topic, n, sizeof(s_topic), APP_MQTT_DEVICE_ID);
    n = app_cat(s_topic, n, sizeof(s_topic), "/sys/properties/set/response/request_id=");
    (void)app_cat(s_topic, n, sizeof(s_topic), request_id);
}

/* 消息下行订阅主题：$oc/devices/{device_id}/sys/messages/down
 * ★ 华为云"消息下发"走这里（异步通道）：云端发什么设备收什么，
 *   不需要也【不应该】回响应——应答通道不存在 */
static void app_mqtt_build_topic_msg_down_sub(void)
{
    uint32_t n = 0U;

    n = app_cat(s_topic, n, sizeof(s_topic), "$oc/devices/");
    n = app_cat(s_topic, n, sizeof(s_topic), APP_MQTT_DEVICE_ID);
    (void)app_cat(s_topic, n, sizeof(s_topic), "/sys/messages/down");
}

/* 上报载荷（华为云的物模型格式）：
 * {"services":[{"service_id":"Sensor","properties":{"temperature":29.4,"humidity":35.0}}]}
 *
 * ★ service_id 与属性名必须与控制台"功能定义"逐字一致：
 *   不一致的后果是华为云静默丢弃 —— 设备在线、日志显示 Published，
 *   但"最新数据"永远是空的。这是最容易被误判成网络故障的一类失败。 */
static void app_mqtt_build_payload(uint8_t temp_int, uint8_t temp_dec,
                                   uint8_t humi_int, uint8_t humi_dec)
{
    uint32_t n = 0U;

    n = app_cat(s_payload, n, sizeof(s_payload), "{\"services\":[{\"service_id\":\"");
    n = app_cat(s_payload, n, sizeof(s_payload), APP_MQTT_SERVICE_ID);
    n = app_cat(s_payload, n, sizeof(s_payload), "\",\"properties\":{\"temperature\":");
    n = app_cat_dec1(s_payload, n, sizeof(s_payload), temp_int, temp_dec);
    n = app_cat(s_payload, n, sizeof(s_payload), ",\"humidity\":");
    n = app_cat_dec1(s_payload, n, sizeof(s_payload), humi_int, humi_dec);
    (void)app_cat(s_payload, n, sizeof(s_payload), "}}]}");
}

/* ==========================================================================
 *  三、离线缓存（有界队列，队满丢最旧）
 * ========================================================================== */

static void app_mqtt_cache_push(const app_mqtt_cache_item_t *item)
{
    if (s_cache_count >= APP_MQTT_OFFLINE_CACHE_MAX)
    {
        /* 队满：丢掉最旧的一条，腾出位置给最新值。
         * ★ "丢最旧"而不是"拒绝新的"：云端要的是最新状态，
         *   留着一堆过期数据没意义 */
        s_cache_head = (uint16_t)((s_cache_head + 1U) % APP_MQTT_OFFLINE_CACHE_MAX);
        s_cache_count--;
    }
    s_cache[s_cache_tail] = *item;
    s_cache_tail = (uint16_t)((s_cache_tail + 1U) % APP_MQTT_OFFLINE_CACHE_MAX);
    s_cache_count++;
}

static uint8_t app_mqtt_cache_pop(app_mqtt_cache_item_t *item)
{
    if (s_cache_count == 0U)
    {
        return 0U;
    }
    *item = s_cache[s_cache_head];
    s_cache_head = (uint16_t)((s_cache_head + 1U) % APP_MQTT_OFFLINE_CACHE_MAX);
    s_cache_count--;
    return 1U;
}

/* ==========================================================================
 *  四、连接：六段依次做
 * ========================================================================== */

/* 发一条上报，成功返回 1。
 * ★ 一条报文发成功就算"链路活着"，顺手刷新心跳计时——这样 Keepalive
 *   只在真正空闲时才发 PINGREQ，不浪费流量 */
static uint8_t app_mqtt_publish_item(const app_mqtt_cache_item_t *item)
{
    app_mqtt_build_payload(item->temp_int, item->temp_dec, item->humi_int, item->humi_dec);
    app_mqtt_build_topic_report();

    if (MOD_MQTT_Publish(s_topic, s_payload) != MOD_MQTT_OK)
    {
        return 0U;
    }
    s_last_tx = osKernelGetTickCount();
    return 1U;
}

/* 连接状态机：六段，任一段失败立刻返回 0（不继续往下一段瞎试）
 * ★ 分段的收益在重连：WiFi 还好好的，就没必要重连 WiFi */
static uint8_t app_mqtt_connect_all(void)
{
    char line[APP_MQTT_PRINT_BUF_SIZE];
    uint32_t pos;

    /* ① AT 测活：模块在不在 */
    MOD_UART1_SendString("[MQTT] AT test...\r\n");
    if (MOD_WIFI_TestAt() != MOD_WIFI_OK)
    {
        app_mqtt_dump_resp("AT failed,");
        return 0U;
    }
    MOD_UART1_SendString("[MQTT] AT OK\r\n");

    /* ② Station 模式 */
    if (MOD_WIFI_SetSta() != MOD_WIFI_OK)
    {
        app_mqtt_dump_resp("CWMODE failed,");
        return 0U;
    }

    /* ③ 连 AP */
    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT] WiFi join AP \"");
    pos = app_cat(line, pos, sizeof(line), APP_MQTT_WIFI_SSID);
    (void)app_cat(line, pos, sizeof(line), "\" ...\r\n");
    MOD_UART1_SendString(line);

    if (MOD_WIFI_JoinAp(APP_MQTT_WIFI_SSID, APP_MQTT_WIFI_PWD) != MOD_WIFI_OK)
    {
        app_mqtt_dump_resp("CWJAP failed,");
        return 0U;
    }
    MOD_UART1_SendString("[MQTT] WiFi connected\r\n");

    /* ④ 建 TCP */
    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT] TCP connecting ");
    pos = app_cat(line, pos, sizeof(line), APP_MQTT_BROKER_HOST);
    pos = app_cat(line, pos, sizeof(line), ":");
    pos = app_cat_u32(line, pos, sizeof(line), APP_MQTT_BROKER_PORT);
    (void)app_cat(line, pos, sizeof(line), " ...\r\n");
    MOD_UART1_SendString(line);

    if (MOD_WIFI_OpenTcp(APP_MQTT_BROKER_HOST, APP_MQTT_BROKER_PORT) != MOD_WIFI_OK)
    {
        app_mqtt_dump_resp("CIPSTART failed,");
        return 0U;
    }
    MOD_UART1_SendString("[MQTT] TCP connected\r\n");

    /* ⑤ MQTT 会话 */
    MOD_UART1_SendString("[MQTT] sending MQTT CONNECT ...\r\n");
    if (MOD_MQTT_Connect(APP_MQTT_CLIENT_ID, APP_MQTT_USERNAME,
                         APP_MQTT_AUTH_PASSWORD,
                         (uint16_t)APP_MQTT_KEEPALIVE_SEC) != MOD_MQTT_OK)
    {
        /* ★ 半开连接的处理：TCP 可能还在，但 MQTT 已经死了。
         *   别在旧连接上死磕——关掉重建，代价只有一次 CIPSTART */
        app_mqtt_dump_resp("CONNECT failed,");
        MOD_WIFI_Close();
        return 0U;
    }

    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT] MQTT connected (keepalive=");
    pos = app_cat_u32(line, pos, sizeof(line), APP_MQTT_KEEPALIVE_SEC);
    (void)app_cat(line, pos, sizeof(line), "s)\r\n");
    MOD_UART1_SendString(line);

    /* ⑥ 订阅下行主题（命令 / 属性设置 / 消息下行）
     *    命令订阅失败 → 整段失败（可上报不可遥控是最差的"半吊子"）；两个新通道
     *    失败 → 只告警继续（至少保住命令与上报，退化是可接受的降级） */
    app_mqtt_build_topic_cmd_sub();
    if (MOD_MQTT_Subscribe(s_topic, 1U) != MOD_MQTT_OK)
    {
        app_mqtt_dump_resp("SUBSCRIBE failed,");
        return 0U;
    }
    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT] subscribed: ");
    pos = app_cat(line, pos, sizeof(line), s_topic);
    (void)app_cat(line, pos, sizeof(line), "\r\n");
    MOD_UART1_SendString(line);

    app_mqtt_build_topic_set_sub();
    if (MOD_MQTT_Subscribe(s_topic, 1U) != MOD_MQTT_OK)
    {
        MOD_UART1_SendString("[MQTT] properties/set subscribe failed (continue)\r\n");
    }
    else
    {
        pos = 0U;
        pos = app_cat(line, pos, sizeof(line), "[MQTT] subscribed: ");
        pos = app_cat(line, pos, sizeof(line), s_topic);
        (void)app_cat(line, pos, sizeof(line), "\r\n");
        MOD_UART1_SendString(line);
    }

    app_mqtt_build_topic_msg_down_sub();
    if (MOD_MQTT_Subscribe(s_topic, 1U) != MOD_MQTT_OK)
    {
        MOD_UART1_SendString("[MQTT] messages/down subscribe failed (continue)\r\n");
    }
    else
    {
        pos = 0U;
        pos = app_cat(line, pos, sizeof(line), "[MQTT] subscribed: ");
        pos = app_cat(line, pos, sizeof(line), s_topic);
        (void)app_cat(line, pos, sizeof(line), "\r\n");
        MOD_UART1_SendString(line);
    }

    s_last_tx   = osKernelGetTickCount();
    s_last_pub  = s_last_tx;
    return 1U;
}

/* 退避：2 / 4 / 8 / 16 / 30 秒；第 5 次起复位模块
 * ★ 为什么封顶 30s 而不是继续翻倍：
 *   路由器重启通常几十秒内恢复；退避太长会让"其实已经好了"看起来像"还没好"。 */
static void app_mqtt_backoff(uint8_t fail_count)
{
    char     line[APP_MQTT_PRINT_BUF_SIZE];
    uint32_t pos = 0U;
    uint32_t wait_ms;

    if (fail_count >= 5U)
    {
        pos = app_cat(line, pos, sizeof(line), "[MQTT] 5 fails, AT+RST module\r\n");
        MOD_UART1_SendString(line);
        (void)MOD_WIFI_Reset();
        wait_ms = 30000U;
    }
    else
    {
        wait_ms = 2000U << (fail_count - 1U);   /* 1→2s 2→4s 3→8s 4→16s */
    }

    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT] retry #");
    pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)fail_count);
    pos = app_cat(line, pos, sizeof(line), " in ");
    pos = app_cat_u32(line, pos, sizeof(line), wait_ms / 1000U);
    (void)app_cat(line, pos, sizeof(line), "s\r\n");
    MOD_UART1_SendString(line);

    osDelay(wait_ms);   /* ★ 退避期间让出 CPU：本地控制照常工作 */
}

/* ==========================================================================
 *  五、命令处理：云端下发 → 执行 → 回响应
 * ========================================================================== */

/* 从 JSON 里取指定 key 的整数值（不引 JSON 库：字段都平铺在 payload 里，够用）
 * 内部按 "\"key\"" 精确匹配（带引号），避免 "led" 误中 "led_xxx" 之类子串
 * 返回 -1 = 没找到 */
static int16_t app_mqtt_parse_int(const char *json, const char *key)
{
    char     search[24];
    uint32_t i = 0U;
    const char *p;
    int16_t     val = -1;

    search[i++] = '"';
    while ((*key != '\0') && ((i + 1U) < sizeof(search)))
    {
        search[i++] = *key++;
    }
    search[i++] = '"';
    search[i] = '\0';

    p = strstr(json, search);
    if (p == NULL)
    {
        return -1;
    }
    p = strchr(p, ':');
    if (p == NULL)
    {
        return -1;
    }
    p++;
    while ((*p == ' ') || (*p == '\t'))
    {
        p++;
    }
    val = 0;
    while ((*p >= '0') && (*p <= '9'))
    {
        val = (int16_t)(val * 10 + (*p - '0'));
        p++;
    }
    return val;
}

/* 从主题里取 request_id：.../commands/request_id=xxxx */
static void app_mqtt_parse_request_id(const char *topic, char *out, uint32_t cap)
{
    const char *p;
    uint32_t    n = 0U;

    out[0] = '\0';
    p = strstr(topic, "request_id=");
    if (p == NULL)
    {
        return;
    }
    p += 11;   /* strlen("request_id=") */
    while ((*p != '\0') && (*p != '&') && ((n + 1U) < cap))
    {
        out[n++] = *p++;
    }
    out[n] = '\0';
}

/* 从 JSON 里解析 LED 控制命令（华为云物模型 LED_Control）：
 *   新格式：{"led_index":0~4,"state":0/1}
 *             led_index 0=全部灯 1=LED1 2=LED2 3/4=本板无此灯（回 result_code=1）
 *             state     0=关 1=开（非 0 一律按开处理）
 *   旧格式（兼容）：{"led":0/1/2} → 视作 state=1 点亮；led=0 全灭
 * 返回 1 = 解析成功（*idx / *state 有效），0 = 解析失败（两字段都没有） */
static int16_t app_mqtt_parse_led_cmd(const char *json, int16_t *idx, int16_t *state)
{
    int16_t i = app_mqtt_parse_int(json, "led_index");
    int16_t s = app_mqtt_parse_int(json, "state");

    if ((i >= 0) && (s >= 0))
    {
        *idx = i;
        *state = s;
        return 1;
    }
    i = app_mqtt_parse_int(json, "led");
    if (i >= 0)
    {
        *idx = i;
        *state = (i == 0) ? 0 : 1;
        return 1;
    }
    return 0;
}

/* 执行 LED 控制（与串口命令同一个入口：云端只是多了一个"遥控器"）
 * 返回 result_code：0=成功，1=执行失败（LED 链被裁剪 / 灯号无板载 / 值非法）
 * ★ 手动接管的语义 1.3 就写好了。若直接操作引脚，闪烁状态机会在
 *   半周期后把它翻回来——现象是"云端下发成功，灯闪了一下又灭了"。 */
static int16_t app_mqtt_exec_led_cmd(int16_t idx, int16_t state)
{
    int on = (state != 0);   /* state 非 0 一律按"开"处理，与参考工程一致 */

    switch (idx)
    {
        case 0:
            /* 全部 LED 一起操作 */
#if APP_BLINK_ENABLE
            if (on != 0) { (void)APP_BLINK_On(APP_BLINK_LED1); }
            else         { (void)APP_BLINK_Off(APP_BLINK_LED1); }
#endif
#if PER_LED_ENABLE
            if (on != 0) { (void)PER_LED_On(PER_LED2); }
            else         { (void)PER_LED_Off(PER_LED2); }
#endif
            MOD_UART1_SendString((on != 0) ? "[MQTT-CMD] all LEDs ON\r\n"
                                           : "[MQTT-CMD] all LEDs OFF\r\n");
            return 0;

        case 1:
#if APP_BLINK_ENABLE
            if (on != 0) { (void)APP_BLINK_On(APP_BLINK_LED1); }
            else         { (void)APP_BLINK_Off(APP_BLINK_LED1); }
            MOD_UART1_SendString((on != 0) ? "[MQTT-CMD] LED1 ON\r\n"
                                           : "[MQTT-CMD] LED1 OFF\r\n");
            return 0;
#else
            MOD_UART1_SendString("[MQTT-CMD] LED1 unavailable (blink chain off)\r\n");
            return 1;
#endif

        case 2:
#if PER_LED_ENABLE
            if (on != 0) { (void)PER_LED_On(PER_LED2); }
            else         { (void)PER_LED_Off(PER_LED2); }
            MOD_UART1_SendString((on != 0) ? "[MQTT-CMD] LED2 ON\r\n"
                                           : "[MQTT-CMD] LED2 OFF\r\n");
            return 0;
#else
            MOD_UART1_SendString("[MQTT-CMD] LED2 unavailable (per_led off)\r\n");
            return 1;
#endif

        case 3:
        case 4:
        {
            char     line[APP_MQTT_PRINT_BUF_SIZE];
            uint32_t pos = 0U;

            pos = app_cat(line, pos, sizeof(line), "[MQTT-CMD] LED");
            pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)idx);
            (void)app_cat(line, pos, sizeof(line),
                          " unavailable on this board (only LED1/LED2)\r\n");
            MOD_UART1_SendString(line);
            return 1;
        }

        default:
            MOD_UART1_SendString("[MQTT-CMD] unknown led_index -> result_code=1\r\n");
            return 1;
    }
}

/* 同步命令通道（$oc/devices/{id}/sys/commands/#）：
 * 执行后必须回响应，request_id 原样带回 */
static void app_mqtt_handle_cmd(MOD_MQTT_Msg_t *msg)
{
    char     line[APP_MQTT_PRINT_BUF_SIZE];
    char     rid[48];   /* ★ 与 MOD_MQTT_TOPIC_MAX 是一对：主题缓冲必须装得下
                         *   "固定部分 + 这里能装下的最长 rid"，否则响应主题被截断 */
    uint32_t pos;
    int16_t  idx;
    int16_t  state;
    int16_t  result_code;

    app_mqtt_parse_request_id(msg->topic, rid, sizeof(rid));

    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT-CMD] LED_Control");
    if (app_mqtt_parse_led_cmd(msg->payload, &idx, &state) != 0)
    {
        pos = app_cat(line, pos, sizeof(line), " led_index=");
        pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)idx);
        pos = app_cat(line, pos, sizeof(line), " state=");
        pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)state);
        result_code = app_mqtt_exec_led_cmd(idx, state);
    }
    else
    {
        pos = app_cat(line, pos, sizeof(line), " no led_index/state field");
        result_code = 1;
    }
    (void)app_cat(line, pos, sizeof(line), "\r\n");
    MOD_UART1_SendString(line);

    /* 回响应：主题里的 request_id 必须与下发时一致，
     * 否则控制台上的"命令-响应"配不成对，一直显示等待响应 */
    if (rid[0] == '\0')
    {
        MOD_UART1_SendString("[MQTT-CMD] no request_id in topic, response skipped\r\n");
        return;
    }
    app_mqtt_build_topic_resp(rid);
    if (result_code == 0)
    {
        (void)MOD_MQTT_Publish(s_topic,
                               "{\"result_code\":0,\"response_name\":\"COMMAND_RESPONSE\","
                               "\"paras\":{\"result\":\"success\"}}");
    }
    else
    {
        (void)MOD_MQTT_Publish(s_topic,
                               "{\"result_code\":1,\"response_name\":\"COMMAND_RESPONSE\","
                               "\"paras\":{\"result\":\"bad led command\"}}");
    }
    s_last_tx = osKernelGetTickCount();
    MOD_UART1_SendString("[MQTT-CMD] response sent\r\n");
}

/* 属性设置通道（$oc/devices/{id}/sys/properties/set/#）：
 * 载荷格式 {"service_id":"Sensor","properties":{...}}，支持的可写属性：
 *   publish_period_s —— 上报周期（秒，1~3600），运行时生效（M4 无 EEPROM 持久化，
 *                        掉电恢复编译期默认值 APP_MQTT_PUBLISH_PERIOD_MS）
 *   led_index/state（或旧 led）—— 驱动执行器（与命令下发同一入口）
 * 应答发到 properties/set/response/request_id={rid}，{"result_code":0/1} */
static void app_mqtt_handle_set(MOD_MQTT_Msg_t *msg)
{
    char     line[APP_MQTT_PRINT_BUF_SIZE];
    char     rid[48];
    uint32_t pos;
    int16_t  period_s;
    uint8_t  applied = 0U;

    app_mqtt_parse_request_id(msg->topic, rid, sizeof(rid));
    if (rid[0] == '\0')
    {
        MOD_UART1_SendString("[MQTT-SET] no request_id in topic, response skipped\r\n");
        return;
    }

    /* 可写属性 1：上报周期 publish_period_s（秒）→ 运行时生效 */
    period_s = app_mqtt_parse_int(msg->payload, "publish_period_s");
    if (period_s > 0)
    {
        if ((period_s >= 1) && (period_s <= 3600))
        {
            s_publish_period_ms = (uint32_t)period_s * 1000U;
            pos = 0U;
            pos = app_cat(line, pos, sizeof(line), "[MQTT-SET] publish_period_s=");
            pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)period_s);
            pos = app_cat(line, pos, sizeof(line), "s -> ");
            pos = app_cat_u32(line, pos, sizeof(line), s_publish_period_ms);
            (void)app_cat(line, pos, sizeof(line), "ms (runtime only)\r\n");
            MOD_UART1_SendString(line);
            applied = 1U;
        }
        else
        {
            pos = 0U;
            pos = app_cat(line, pos, sizeof(line), "[MQTT-SET] publish_period_s out of range: ");
            pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)period_s);
            (void)app_cat(line, pos, sizeof(line), " (1~3600)\r\n");
            MOD_UART1_SendString(line);
        }
    }

    /* 可写属性 2：led_index/state（或旧 led）驱动执行器 */
    {
        int16_t idx;
        int16_t state;

        if (app_mqtt_parse_led_cmd(msg->payload, &idx, &state) != 0)
        {
            if (app_mqtt_exec_led_cmd(idx, state) == 0)
            {
                applied = 1U;
            }
        }
    }

    app_mqtt_build_topic_set_resp(rid);
    if (applied != 0U)
    {
        (void)MOD_MQTT_Publish(s_topic, "{\"result_code\":0}");
    }
    else
    {
        MOD_UART1_SendString("[MQTT-SET] no writable property applied\r\n");
        (void)MOD_MQTT_Publish(s_topic, "{\"result_code\":1}");
    }
    s_last_tx = osKernelGetTickCount();
    MOD_UART1_SendString("[MQTT-SET] response sent\r\n");
}

/* 消息下行通道（$oc/devices/{id}/sys/messages/down，异步）：
 * 云端发什么收什么；识别到 JSON 命令（led_index/state 或旧 led）就执行，
 * 【不回响应】——应答通道不存在，硬回会破坏"消息"语义 */
static void app_mqtt_handle_msg(MOD_MQTT_Msg_t *msg)
{
    char     line[APP_MQTT_PRINT_BUF_SIZE];
    uint32_t pos;
    int16_t  idx;
    int16_t  state;

    pos = 0U;
    pos = app_cat(line, pos, sizeof(line), "[MQTT-MSG] async message: ");
    (void)app_cat(line, pos, sizeof(line), msg->payload);
    (void)app_cat(line, pos, sizeof(line), "\r\n");
    MOD_UART1_SendString(line);

    if (app_mqtt_parse_led_cmd(msg->payload, &idx, &state) != 0)
    {
        (void)app_mqtt_exec_led_cmd(idx, state);
        MOD_UART1_SendString("[MQTT-MSG] async command executed (no response)\r\n");
    }
    else
    {
        MOD_UART1_SendString("[MQTT-MSG] no led_index/state (or led) field, ignored (async, no response)\r\n");
    }
}

/* 下行分发：按主题区分三个通道
 *   properties/set → 属性设置（回属性设置响应）
 *   messages/down → 消息下行（执行不回响应）
 *   其余（commands/#）→ 同步命令（回命令响应） */
static void app_mqtt_handle_downlink(MOD_MQTT_Msg_t *msg)
{
    if (strstr(msg->topic, "properties/set") != NULL)
    {
        app_mqtt_handle_set(msg);
        return;
    }
    if (strstr(msg->topic, "messages/down") != NULL)
    {
        app_mqtt_handle_msg(msg);
        return;
    }
    app_mqtt_handle_cmd(msg);
}

/* ==========================================================================
 *  六、任务体
 * ========================================================================== */

void app_mqtt_task(void *argument)
{
    char     line[APP_MQTT_PRINT_BUF_SIZE];
    uint32_t pos;
    uint32_t now;
    app_mqtt_cache_item_t item;

    (void)argument;

    /* ① 打招呼：串口一打开就能确认"这个任务起来了" */
    MOD_UART1_SendString("\r\n=== IOT_M4_L4 WiFi-MQTT Cloud (USART3) ===\r\n");

    MOD_MQTT_Init();
    MOD_WIFI_Init();     /* 内部等 2 秒让模块上电稳定（osDelay，CPU 让得出去） */

    for (;;)
    {
        /* ② 没连上就先连：连不上退避，退避完再来一圈 */
        if (MOD_MQTT_IsConnected() == 0U)
        {
            if (app_mqtt_connect_all() != 0U)
            {
                s_fail_count = 0U;
            }
            else
            {
                s_fail_count++;
                app_mqtt_backoff(s_fail_count);
                continue;    /* 回到循环顶部，重新判断连接状态 */
            }
        }

        now = osKernelGetTickCount();

        /* ③ 周期上报：到点了就上报一组"快照"（周期可被云端属性设置运行时修改） */
        if ((now - s_last_pub) >= s_publish_period_ms)
        {
            uint8_t have_data = 1U;

            s_last_pub = now;

            item.temp_int = 0U;
            item.temp_dec = 0U;
            item.humi_int = 0U;
            item.humi_dec = 0U;

#if APP_DHT11_ENABLE
            {
                APP_DHT11_Snapshot_t snap;

                APP_DHT11_GetSnapshot(&snap);
                if (snap.valid != 0U)
                {
                    item.temp_int = snap.temp_int;
                    item.temp_dec = snap.temp_dec;
                    item.humi_int = snap.humi_int;
                    item.humi_dec = snap.humi_dec;
                }
                else
                {
                    /* 刚上电还没采到过：如实说明，不编一个 0 上去——
                     * 编 0 会让云端看到"温度 0 度"，比"没有数据"更容易误导人 */
                    have_data = 0U;
                }
            }
#endif

            if (have_data == 0U)
            {
                MOD_UART1_SendString("[MQTT] no DHT11 data yet, publish skipped\r\n");
            }
            else if (app_mqtt_publish_item(&item) != 0U)
            {
                pos = 0U;
                pos = app_cat(line, pos, sizeof(line), "[MQTT] Published: ");
                pos = app_cat(line, pos, sizeof(line), s_payload);
                (void)app_cat(line, pos, sizeof(line), "\r\n");
                MOD_UART1_SendString(line);
            }
            else
            {
                /* 发不出去 → 进离线缓存。缓存有界，队满丢最旧 */
                app_mqtt_cache_push(&item);
                pos = 0U;
                pos = app_cat(line, pos, sizeof(line), "[MQTT] publish failed, cached (");
                pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)s_cache_count);
                (void)app_cat(line, pos, sizeof(line), " pending)\r\n");
                MOD_UART1_SendString(line);
                MOD_MQTT_MarkDisconnected();
            }
        }

        /* ④ 补传：恢复连接后每隔 500ms 放一条，别一次把 Broker 刷爆 */
        if ((s_cache_count > 0U) && ((now - s_last_flush) >= APP_MQTT_FLUSH_GAP_MS))
        {
            s_last_flush = now;
            if ((MOD_MQTT_IsConnected() != 0U) && (app_mqtt_cache_pop(&item) != 0U))
            {
                if (app_mqtt_publish_item(&item) != 0U)
                {
                    pos = 0U;
                    pos = app_cat(line, pos, sizeof(line), "[MQTT] flushed 1 cached, left=");
                    pos = app_cat_u32(line, pos, sizeof(line), (uint32_t)s_cache_count);
                    (void)app_cat(line, pos, sizeof(line), "\r\n");
                    MOD_UART1_SendString(line);
                }
                else
                {
                    /* 又失败了：把它塞回队尾，下轮再试 */
                    app_mqtt_cache_push(&item);
                    MOD_MQTT_MarkDisconnected();
                }
            }
        }

        /* ⑤ 收一包：把 +IPD 里的报文解析成下行（内部顺带回 PUBACK），
         *    再按主题分发给 命令 / 属性设置 / 消息下行 三个通道 */
        if (MOD_MQTT_Poll(&s_msg) != 0U)
        {
            app_mqtt_handle_downlink(&s_msg);
        }

        /* ⑥ TCP 被对端关掉：立刻标记断开，下一圈走重连 */
        if (MOD_WIFI_IsTcpClosed() != 0U)
        {
            MOD_UART1_SendString("[MQTT] TCP closed by peer, will reconnect\r\n");
            MOD_MQTT_MarkDisconnected();
        }

        /* ⑦ 心跳：空闲超过 keepalive/2 就发 PINGREQ
         *    （30s 一半 = 15s；主循环 50ms 一圈，绝不会漏） */
        if ((osKernelGetTickCount() - s_last_tx) >=
            ((uint32_t)APP_MQTT_KEEPALIVE_SEC * 1000U / 2U))
        {
            if (MOD_MQTT_Ping() != MOD_MQTT_OK)
            {
                MOD_UART1_SendString("[MQTT] Ping failed, will reconnect\r\n");
                MOD_MQTT_MarkDisconnected();
            }
            s_last_tx = osKernelGetTickCount();
        }

        osDelay(APP_MQTT_LOOP_MS);
    }
}

#endif /* APP_MQTT_ENABLE */
