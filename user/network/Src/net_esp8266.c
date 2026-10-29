/**
 ******************************************************************************
 * @file    net_esp8266.c
 * @brief   网络层（network）—— ESP8266 AT 会话 + +IPD 二进制分流（实现）
 *
 *  一条 AT 会话只有三步（本文件所有函数都是这三步的组合）：
 *    ① 发指令：自动补 \r\n，发完立刻清接收缓冲
 *    ② 等关键字：命中期盼值 = 成功；命中 ERROR/FAIL = 失败；都不是 = 继续等
 *    ③ 超时就 dump：把收到的原始字节原样留在内部，交给上层判断
 *
 *  ★ 两条缓冲，互不打架（这是本文件最值得看懂的地方）：
 *
 *      USART3 → net_uart 的环形缓冲（原始字节：AT 响应 + 二进制载荷混在一起）
 *                     │
 *          net_esp8266_pump_rx() 把新字节搬进 ↓
 *                     │
 *          ┌──────────┴───────────┐
 *     响应缓冲 s_resp_buf      载荷缓冲 s_payload
 *     "OK" / "SEND OK"         +IPD,42:<42 字节 MQTT 报文>
 *     "WIFI GOT IP" / "CLOSED"  二进制安全：不追加 \n、不按字节切
 *     按行切没问题              满了丢最旧，并置溢出标志
 *     满了丢最旧（丢的是状态）     （丢的是数据，必须能感知）
 *
 *  ★ 为什么不用 HAL_Delay 而用 osDelay：
 *    参考工程（H5 项目）在裸跑语境里用 HAL_Delay，它本质是"死等计数器"，
 *    不把 CPU 让给别的任务。本工程 CWJAP 最长要等 10 秒，若用 HAL_Delay，
 *    这 10 秒里 LED 任务一次都跑不到——灯会定住，像死机。
 *    osDelay 会主动让出 CPU，等 10 秒的代价被摊到"别人照常干活"上。
 *
 *  ★ 本层不打印：超时现场留在内部缓冲里，由 app_mqtt 取出来打成 hex + ascii。
 *    依赖方向只有"上层调下层"，下层永远不调上层。
 ******************************************************************************
 */
#include "module_cfg.h"

#if NET_ESP8266_ENABLE

#include "net_esp8266.h"
#include "net_uart.h"     /* 网络层内部：USART3 实例的字节流 */
#include "ringbuf.h"      /* common 横切：载荷缓冲（二进制安全） */
#include "cmsis_os.h"     /* osDelay：长等待必须让出 CPU */
#include <string.h>

/* ========== 内部常量（协议层面的经验值，不对外开放） ==========
 * 每条指令的超时为什么不一样：
 *   AT / CWMODE / CIPMUX / CIPSEND 都是"模块自己的事"，2 秒足够；
 *   CWJAP 要等路由器 + DHCP 分配地址，给 10 秒；
 *   CIPSTART 要做域名解析 + 三次握手，给 8 秒。
 * ★ 给短了会误判失败（本来能连上却当失败），给长了会拖住重连
 *   （断网时每次重试都卡满超时，退避就失去意义）。 */
#define NET_ESP8266_POLL_MS         10U     /* 轮询步长，也是超时的计数单位 */
#define NET_ESP8266_CMD_TIMEOUT_MS  2000U   /* 普通指令                   */
#define NET_ESP8266_JOIN_TIMEOUT_MS 10000U  /* CWJAP：等 DHCP             */
#define NET_ESP8266_TCP_TIMEOUT_MS  8000U   /* CIPSTART：解析 + 握手       */
#define NET_ESP8266_SEND_TIMEOUT_MS 2000U   /* CIPSEND：只等一个 '>'       */
#define NET_ESP8266_RESET_TIMEOUT_MS 10000U /* AT+RST：等 "ready"         */
#define NET_ESP8266_READY_DELAY_MS  2000U   /* 上电后等模块稳定            */
#define NET_ESP8266_CMD_BUF_SIZE    192U    /* AT 指令拼装缓冲（SSID+密码占大头） */

/* ========== 内部资源 ========== */
static uint8_t   s_resp_buf[NET_ESP8266_RESP_BUF_SIZE];  /* AT 响应累积缓冲 */
static uint32_t  s_resp_len;                             /* 已用长度        */
static ringbuf_t s_payload;                              /* +IPD 载荷缓冲   */
static uint8_t   s_mqtt_mode;   /* 1 = 已进入 MQTT 模式，+IPD 改投载荷缓冲 */
static uint8_t   s_tcp_closed;  /* 1 = 收到独立成行的 CLOSED（纯内存标志） */
static uint8_t   s_overflow;    /* 1 = 载荷缓冲曾丢过最旧的数据            */

/* ========== 内部函数声明 ========== */
static void net_esp8266_scan_ipd(void);

/* ==========================================================================
 *  一、小工具：自己拼字符串，不引 printf
 *
 *  ★ 为什么不用 snprintf：本工程从 DHT11 章节起就守这条规矩——
 *    格式化库会拉进几 KB 的 FLASH，而这里要拼的东西只有
 *    "字符串 + 字符串 + 十进制数"三种，几十行就够。
 * ========================================================================== */

/* 追加字符串，返回新的写位置（永远是"下一个可写下标"，便于链式追加） */
static uint32_t net_esp8266_cat(char *dst, uint32_t pos, uint32_t cap, const char *src)
{
    while ((src != NULL) && (*src != '\0') && ((pos + 1U) < cap))
    {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
    return pos;
}

/* 追加无符号十进制数（AT 里只有端口号和长度两个数字，用不着更复杂的） */
static uint32_t net_esp8266_cat_u32(char *dst, uint32_t pos, uint32_t cap, uint32_t val)
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

/* ==========================================================================
 *  二、接收：把 UART 缓冲里的新字节搬进响应缓冲，并顺手分流 +IPD
 * ========================================================================== */

/* 清空接收链路（UART 环形缓冲 + 响应缓冲 + 载荷缓冲） */
static void net_esp8266_flush_rx(void)
{
    uint8_t  tmp[32];

    while (net_uart_available(NET_UART_WIFI) > 0U)
    {
        (void)net_uart_read(NET_UART_WIFI, tmp, sizeof(tmp));
    }
    s_resp_len = 0U;
    s_resp_buf[0] = '\0';
    rb_init(&s_payload);
    s_overflow = 0U;
}

/* 从 UART 读新数据追加到响应缓冲；MQTT 模式下再扫一遍 +IPD。
 * 返回值：本次新读到的字节数 */
static uint32_t net_esp8266_pump_rx(void)
{
    uint32_t avail;
    uint32_t space;
    uint32_t want;
    uint32_t got;

    avail = net_uart_available(NET_UART_WIFI);
    if (avail == 0U)
    {
        return 0U;
    }

    /* 响应缓冲满：丢掉最旧的一半腾地方。
     * ★ 这里丢的是"状态文本"（OK / SEND OK / CLOSED），丢了不影响数据正确性；
     *   真正的数据在载荷缓冲里，那条通路的策略是"丢最旧 + 置溢出标志"。 */
    if (s_resp_len >= NET_ESP8266_RESP_BUF_SIZE)
    {
        uint32_t half = NET_ESP8266_RESP_BUF_SIZE / 2U;
        memmove(s_resp_buf, &s_resp_buf[half], NET_ESP8266_RESP_BUF_SIZE - half);
        s_resp_len = NET_ESP8266_RESP_BUF_SIZE - half;
    }

    space = NET_ESP8266_RESP_BUF_SIZE - s_resp_len;
    want  = (avail < space) ? avail : space;
    got   = net_uart_read(NET_UART_WIFI, &s_resp_buf[s_resp_len], want);
    s_resp_len += got;

    /* MQTT 模式下：把 +IPD 里的二进制数据剥出去放载荷缓冲，
     * 否则下行报文会把响应缓冲撑满，"SEND OK" 这类回复就无处可放 */
    if (s_mqtt_mode != 0U)
    {
        net_esp8266_scan_ipd();
    }
    return got;
}

/* 响应缓冲里有没有某个关键字（子串匹配；AT 响应都是文本，可以放心用 strstr） */
static uint8_t net_esp8266_resp_has(const char *keyword)
{
    if ((keyword == NULL) || (s_resp_len == 0U))
    {
        return 0U;
    }
    /* 保证以 '\0' 结尾：strstr 需要合法 C 字符串 */
    if (s_resp_len < (NET_ESP8266_RESP_BUF_SIZE - 1U))
    {
        s_resp_buf[s_resp_len] = '\0';
    }
    else
    {
        s_resp_buf[NET_ESP8266_RESP_BUF_SIZE - 1U] = '\0';
    }
    return (strstr((const char *)s_resp_buf, keyword) != NULL) ? 1U : 0U;
}

/* 等关键字：成功 / 失败 / 超时三种结局 */
static NET_ESP8266_Status_t net_esp8266_wait(const char *expect,
                                            const char *fail,
                                            uint32_t    timeout_ms)
{
    uint32_t elapsed = 0U;

    while (elapsed < timeout_ms)
    {
        (void)net_esp8266_pump_rx();

        if ((expect != NULL) && (net_esp8266_resp_has(expect) != 0U))
        {
            return NET_ESP8266_OK;
        }
        if ((fail != NULL) && (net_esp8266_resp_has(fail) != 0U))
        {
            return NET_ESP8266_ERR_RESPONSE;
        }

        osDelay(NET_ESP8266_POLL_MS);   /* ★ 让出 CPU：等的这 10ms 里别人照常跑 */
        elapsed += NET_ESP8266_POLL_MS;
    }

    /* 超时前最后再泵一次：模块可能正好在最后一刻回了话 */
    (void)net_esp8266_pump_rx();
    if ((expect != NULL) && (net_esp8266_resp_has(expect) != 0U))
    {
        return NET_ESP8266_OK;
    }
    return NET_ESP8266_ERR_TIMEOUT;
}

/* 发一条 AT 指令：先清缓冲，再发指令 + \r\n */
static void net_esp8266_send_cmd(const char *cmd)
{
    net_esp8266_flush_rx();
    net_uart_send_string(NET_UART_WIFI, cmd);
    net_uart_send_string(NET_UART_WIFI, "\r\n");
}

/* ==========================================================================
 *  三、+IPD 分流：四步走完一包
 *
 *  单连接模式（AT+CIPMUX=0）下 +IPD 的格式是：
 *      +IPD,<data_len>:<data>
 *  多连接模式（CIPMUX=1）才是 +IPD,<link_id>,<len>:<data> —— 本工程不用。
 *
 *  四步：① 找 "+IPD,"  ② 读到冒号，数字段就是 data_len  ③ 等数据到齐
 *        ④ 整段搬走（连同头部一起从响应缓冲移除）
 *
 *  ★ 第 ③ 步是这一段的灵魂：包可能被拆成两次中断送进来
 *    （头部到了、数据还没到），此时必须【什么都不做、原样留着】，
 *    等下次 pump 再拼。心急地"有多少搬多少"会把二进制报文切碎，
 *    MQTT 那边收到的就是一帧永远对不上长度的残包。
 * ========================================================================== */
static void net_esp8266_payload_push(const uint8_t *data, uint32_t len)
{
    uint32_t space = (uint32_t)rb_space(&s_payload);

    if (space < len)
    {
        /* 丢最旧：先把放不下的那部分从队头读出来扔掉，再压入新数据。
         * ★ 丢的是数据，所以要置标志让上层知道——这与响应缓冲的策略相反 */
        uint8_t  junk[32];
        uint32_t drop = len - space;

        while (drop > 0U)
        {
            uint32_t n = (drop > sizeof(junk)) ? sizeof(junk) : drop;
            (void)rb_read(&s_payload, junk, n);
            drop -= n;
        }
        s_overflow = 1U;
    }
    (void)rb_write(&s_payload, data, len);
}

static void net_esp8266_scan_ipd(void)
{
    uint32_t i = 0U;

    while (i < s_resp_len)
    {
        uint32_t pos;
        uint32_t len_start;
        uint32_t data_len = 0U;
        uint32_t remove_end;
        uint32_t j;

        /* ① 找 "+IPD," */
        if ((s_resp_buf[i] != '+') ||
            ((i + 4U) >= s_resp_len) ||
            (s_resp_buf[i + 1U] != 'I') ||
            (s_resp_buf[i + 2U] != 'P') ||
            (s_resp_buf[i + 3U] != 'D') ||
            (s_resp_buf[i + 4U] != ','))
        {
            i++;
            continue;
        }

        /* ② 读到冒号，数字段就是 data_len */
        pos       = i + 5U;
        len_start = pos;
        while ((pos < s_resp_len) && (s_resp_buf[pos] != ':'))
        {
            pos++;
        }
        if (pos >= s_resp_len)
        {
            break;   /* 只有头部、没有冒号：等下次拼 */
        }
        for (j = len_start; j < pos; j++)
        {
            if ((s_resp_buf[j] >= '0') && (s_resp_buf[j] <= '9'))
            {
                data_len = (data_len * 10U) + (uint32_t)(s_resp_buf[j] - '0');
            }
        }
        pos++;   /* 跳过冒号 */

        /* ③ 数据没到齐：原样留着等下次 */
        if ((pos + data_len) > s_resp_len)
        {
            break;
        }

        /* ④ 整段搬走：数据进载荷缓冲，头部 + 数据一起从响应缓冲移除 */
        net_esp8266_payload_push(&s_resp_buf[pos], data_len);

        remove_end = pos + data_len;
        memmove(&s_resp_buf[i], &s_resp_buf[remove_end], s_resp_len - remove_end);
        s_resp_len -= (remove_end - i);
        /* 不递增 i：同一个位置可能紧跟下一个 +IPD 包 */
    }
}

/* 断线检测：只认"独立成行"的 CLOSED。
 * ★ 为什么不能按子串匹配：MQTT 报文里也可能碰巧出现 C-L-O-S-E-D 这六个字节，
 *   按子串匹配会把它当成掉线——一次误判就要多花十几秒重连。
 *   模块真正报断开时，CLOSED 一定单独占一行（\r\nCLOSED\r\n）。 */
static void net_esp8266_check_closed(void)
{
    if (s_resp_len == 0U)
    {
        return;
    }
    s_resp_buf[s_resp_len] = '\0';

    if (strstr((const char *)s_resp_buf, "\r\nCLOSED\r\n") != NULL)
    {
        s_tcp_closed = 1U;
        return;
    }
    if ((s_resp_len >= 8U) && (memcmp(s_resp_buf, "CLOSED\r\n", 8U) == 0))
    {
        s_tcp_closed = 1U;
    }
}

/* ==========================================================================
 *  四、对外接口
 * ========================================================================== */

void net_esp8266_init(void)
{
    uint32_t remain = NET_ESP8266_READY_DELAY_MS;

    /* 拉起 USART3 的接收链路（DMA 循环 + IDLE；串口本体 CubeMX 已配好） */
    net_uart_init(NET_UART_WIFI);
    net_esp8266_flush_rx();

    /* 等模块上电稳定：分片等待，每片 200ms —— 一次性 osDelay(2000) 也行，
     * 但分片的好处是万一以后要加"等模块期间还要做点别的"，改起来只在循环里 */
    while (remain > 0U)
    {
        uint32_t slice = (remain > 200U) ? 200U : remain;
        osDelay(slice);
        remain -= slice;
    }

    /* 上电期间模块会吐一堆乱码 + "ready"，全部丢掉：
     * 它们不参与后续任何判断，留着只会让第一条 AT 的关键字匹配误命中 */
    net_esp8266_flush_rx();
}

NET_ESP8266_Status_t net_esp8266_test_at(uint32_t timeout_ms)
{
    net_esp8266_send_cmd("AT");
    return net_esp8266_wait("OK", "ERROR", timeout_ms);
}

NET_ESP8266_Status_t net_esp8266_set_sta(void)
{
    net_esp8266_send_cmd("AT+CWMODE=1");
    return net_esp8266_wait("OK", "ERROR", NET_ESP8266_CMD_TIMEOUT_MS);
}

NET_ESP8266_Status_t net_esp8266_join_ap(const char *ssid, const char *pwd)
{
    char     cmd[NET_ESP8266_CMD_BUF_SIZE];
    uint32_t n;

    if ((ssid == NULL) || (pwd == NULL))
    {
        return NET_ESP8266_ERR_PARAM;
    }

    /* 先断开可能存在的旧连接：模块在"已连接"状态下再 CWJAP 会回 busy，
     * 现象是"明明密码没错却一直 FAIL" */
    net_esp8266_send_cmd("AT+CWQAP");
    (void)net_esp8266_wait("OK", "ERROR", NET_ESP8266_CMD_TIMEOUT_MS);

    /* 拼 AT+CWJAP="ssid","pwd" —— 引号必须保留 */
    n = net_esp8266_cat(cmd, 0U, sizeof(cmd), "AT+CWJAP=\"");
    n = net_esp8266_cat(cmd, n, sizeof(cmd), ssid);
    n = net_esp8266_cat(cmd, n, sizeof(cmd), "\",\"");
    n = net_esp8266_cat(cmd, n, sizeof(cmd), pwd);
    (void)net_esp8266_cat(cmd, n, sizeof(cmd), "\"");

    net_esp8266_send_cmd(cmd);

    /* 成功标志是 "WIFI GOT IP"（DHCP 拿到地址），不是 "OK"：
     * 只看 OK 会把"指令被接受但没连上"误判成成功 */
    return net_esp8266_wait("WIFI GOT IP", "FAIL", NET_ESP8266_JOIN_TIMEOUT_MS);
}

NET_ESP8266_Status_t net_esp8266_open_tcp(const char *host, uint16_t port)
{
    char                cmd[NET_ESP8266_CMD_BUF_SIZE];
    uint32_t            n;
    NET_ESP8266_Status_t st;

    if (host == NULL)
    {
        return NET_ESP8266_ERR_PARAM;
    }

    /* ① 单连接模式：多连接模式下 +IPD 会多一个 link_id 字段 */
    net_esp8266_send_cmd("AT+CIPMUX=0");
    st = net_esp8266_wait("OK", "ERROR", NET_ESP8266_CMD_TIMEOUT_MS);
    if (st != NET_ESP8266_OK)
    {
        return st;
    }

    /* ② 建链：AT+CIPSTART="TCP","host",port */
    n = net_esp8266_cat(cmd, 0U, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"");
    n = net_esp8266_cat(cmd, n, sizeof(cmd), host);
    n = net_esp8266_cat(cmd, n, sizeof(cmd), "\",");
    (void)net_esp8266_cat_u32(cmd, n, sizeof(cmd), (uint32_t)port);

    net_esp8266_send_cmd(cmd);
    st = net_esp8266_wait("CONNECT", "ERROR", NET_ESP8266_TCP_TIMEOUT_MS);
    if (st == NET_ESP8266_OK)
    {
        s_tcp_closed = 0U;
    }
    return st;
}

NET_ESP8266_Status_t net_esp8266_send(const uint8_t *data, uint32_t len)
{
    char                cmd[32];
    uint32_t            n;
    NET_ESP8266_Status_t st;

    if ((data == NULL) || (len == 0U))
    {
        return NET_ESP8266_ERR_PARAM;
    }

    /* ① 报长度：AT+CIPSEND=<len> */
    n = net_esp8266_cat(cmd, 0U, sizeof(cmd), "AT+CIPSEND=");
    (void)net_esp8266_cat_u32(cmd, n, sizeof(cmd), len);
    net_esp8266_send_cmd(cmd);

    /* ② 等 '>'：模块准备好"接下来这 len 个字节是数据，不是指令" */
    st = net_esp8266_wait(">", "ERROR", NET_ESP8266_CMD_TIMEOUT_MS);
    if (st != NET_ESP8266_OK)
    {
        return NET_ESP8266_ERR_RESPONSE;
    }

    /* ③ 发数据：二进制，不追加 \r\n，发多少就是多少 */
    net_uart_send(NET_UART_WIFI, data, len);

    /* ④ 等 SEND OK / SEND FAIL */
    return net_esp8266_wait("SEND OK", "SEND FAIL", NET_ESP8266_SEND_TIMEOUT_MS);
}

uint32_t net_esp8266_recv(uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t got     = 0U;
    uint32_t elapsed = 0U;

    if ((buf == NULL) || (len == 0U) || (s_mqtt_mode == 0U))
    {
        return 0U;
    }

    while (got < len)
    {
        /* 先把载荷缓冲里已有的数据取干净 */
        while ((got < len) && (rb_count(&s_payload) > 0U))
        {
            got += (uint32_t)rb_read(&s_payload, &buf[got], len - got);
        }
        if (got >= len)
        {
            break;
        }

        /* 再泵一次接收链路（可能带来新的 +IPD） */
        (void)net_esp8266_pump_rx();
        net_esp8266_check_closed();

        while ((got < len) && (rb_count(&s_payload) > 0U))
        {
            got += (uint32_t)rb_read(&s_payload, &buf[got], len - got);
        }
        if (got >= len)
        {
            break;
        }

        if ((s_tcp_closed != 0U) || (elapsed >= timeout_ms))
        {
            break;   /* 断了就别再等；超时也返回已拿到的部分 */
        }
        osDelay(NET_ESP8266_POLL_MS);
        elapsed += NET_ESP8266_POLL_MS;
    }
    return got;
}

NET_ESP8266_Status_t net_esp8266_close(void)
{
    net_esp8266_send_cmd("AT+CIPCLOSE");
    /* 未连接时模块可能回 ERROR，这边一律当"已关闭"处理：
     * 上层关心的是"现在没有连接"，不是"这条指令成不成" */
    (void)net_esp8266_wait("OK", "ERROR", NET_ESP8266_CMD_TIMEOUT_MS);
    s_tcp_closed = 1U;
    return NET_ESP8266_OK;
}

NET_ESP8266_Status_t net_esp8266_reset(void)
{
    NET_ESP8266_Status_t st;

    net_esp8266_send_cmd("AT+RST");
    /* 复位后模块吐乱码 + "ready"，看到 ready 就算复位完成 */
    st = net_esp8266_wait("ready", "ERROR", NET_ESP8266_RESET_TIMEOUT_MS);
    s_tcp_closed = 0U;
    s_mqtt_mode  = 0U;   /* 复位后一切归零：要重新建链、重新进 MQTT 模式 */
    return st;
}

uint8_t net_esp8266_is_tcp_closed(void)
{
    net_esp8266_check_closed();   /* 顺手再判一次：不额外发 AT，只读缓冲 */
    return s_tcp_closed;
}

void net_esp8266_clear_closed(void)
{
    s_tcp_closed = 0U;
}

void net_esp8266_enter_mqtt_mode(void)
{
    s_mqtt_mode = 1U;
    rb_init(&s_payload);
}

uint32_t net_esp8266_get_last_resp(uint8_t *buf, uint32_t cap)
{
    uint32_t n;

    if ((buf == NULL) || (cap == 0U) || (s_resp_len == 0U))
    {
        return 0U;
    }
    n = (s_resp_len < cap) ? s_resp_len : cap;
    memcpy(buf, s_resp_buf, n);
    return n;
}

uint8_t net_esp8266_take_overflow(void)
{
    uint8_t flag = s_overflow;
    s_overflow = 0U;
    return flag;
}

#endif /* NET_ESP8266_ENABLE */
