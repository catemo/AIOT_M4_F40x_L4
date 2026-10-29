/**
 ******************************************************************************
 * @file    net_uart.c
 * @brief   网络层（network）—— UART 字节流实现（实例表 + DMA 循环 + IDLE 判帧 + 环形缓冲）
 *
 *  本文件是"串口三件套"的落点：
 *    ① DMA 循环接收：一次性配置，硬件持续搬运，缓冲写满自动绕回，永不停止
 *    ② IDLE 判帧：总线一安静就进空闲中断，把"这段时间新到的字节"切成一帧
 *    ③ 环形缓冲：中断只管把字节塞进 ringbuf，任务有空了再来取（解耦、不丢）
 *
 *  ★ 本册的核心改动：一份代码、两个实例
 *    1.3 章节里这一层写死了 huart1；2.2 章节要同时服务 USART1（调试）与
 *    USART3（ESP8266）。改法不是"复制一份 net_uart3.c"，而是把"哪一个串口"
 *    收进一张表：表里每行 = 一个串口所需的全部东西（句柄、DMA 句柄、
 *    DMA 缓冲、环形缓冲、上次消费位置）。函数的形参只多了个 id。
 *
 *    为什么不做成两份文件：判帧的数学（NDTR 递减 → pos = size - NDTR →
 *    处理绕回）与实例无关，复制一份只会多出一份将来要同步维护的代码。
 *    这正是"参数化"与"复制粘贴"的分水岭：**变的东西进表，不变的东西留代码**。
 *
 *  ★ 三个刻意的取舍（都是为了让新手少踩坑）：
 *    ① ISR 里只搬数据，不解析、不打印、不调 RTOS API —— 外设不能拖死 CPU
 *    ② 环形缓冲满了就丢字节，不阻塞 —— 宁可丢，也不能让中断卡住
 *    ③ 发送用阻塞轮询 —— 本例只回几句短提示 / 发几百字节的报文；
 *       量产应换 DMA 发送（换时只改本文件，上层零改动）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 net_uart.h。
 *
 *  ★ 分层纪律：本文件只 include net_uart.h / ringbuf.h / usart.h，
 *    绝不 include mod_uart1.h（反向依赖）或 app_cmd.h（越级依赖）。
 ******************************************************************************
 */
#include "module_cfg.h"

#include "net_uart.h"
#include "ringbuf.h"     /* common 横切：无锁环形缓冲（ISR 写 / 任务读） */
#include "usart.h"       /* CubeMX 生成的全局句柄 huart1 / huart3 */

/* RX DMA 句柄：usart.c 里定义（CubeMX 没把它们导出到头文件，这里 extern 一下）。
 * IDLE 判帧要读它们的 NDTR 计数器 */
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart3_rx;

/* ==========================================================================
 *  每个串口一份资源（受开关控制：关掉某条链，它的 RAM 一个字节都不占）
 * ========================================================================== */

#if NET_UART1_ENABLE
/* DMA 硬件环形缓冲：DMA 硬件写、CPU 读。
 * F407 的 SRAM 在 0x20000000，DMA1/2 都能直接访问，普通静态数组即可
 * （H7 工程里要用链接脚本定向到 AXI SRAM，本工程不需要） */
static uint8_t   s_dma_buf1[NET_UART1_RX_DMA_BUF_SIZE];

/* 软件环形缓冲：IDLE 中断写（生产者）、任务读（消费者）。
 * 容量由 common 横切层的 CFG_RINGBUF_SIZE 决定（必须是 2 的幂） */
static ringbuf_t s_ring1;
#endif

#if NET_UART3_ENABLE
/* USART3 的同一套资源。DMA 缓冲取 512：AT 响应（"SEND OK"、"CLOSED"）
 * 与 +IPD 载荷会挤在同一条线上，缓冲太小会在模块连发时截断 */
static uint8_t   s_dma_buf3[NET_UART3_RX_DMA_BUF_SIZE];
static ringbuf_t s_ring3;
#endif

/* ==========================================================================
 *  实例表：id 就是下标，一行描述一个串口
 * ========================================================================== */
typedef struct
{
    UART_HandleTypeDef *huart;      /* CubeMX 的 UART 句柄                    */
    DMA_HandleTypeDef  *hdma_rx;    /* RX DMA 句柄（判帧要读它的 NDTR）        */
    uint8_t            *dma_buf;    /* DMA 循环接收缓冲                       */
    uint32_t            dma_size;   /* 缓冲大小（判帧数学要用）                */
    ringbuf_t          *ring;       /* 软件环形缓冲（ISR 写、任务读）          */
    volatile uint32_t   last_pos;   /* 上次消费到 DMA 缓冲的哪个位置           */
    uint8_t             ready;      /* 0 = 被开关裁掉：所有接口直接返回        */
} net_uart_ctx_t;

static net_uart_ctx_t s_uart[NET_UART_COUNT] =
{
#if NET_UART1_ENABLE
    /* 下标 0：NET_UART_DBG → USART1（PA9/PA10） */
    { &huart1, &hdma_usart1_rx, s_dma_buf1, NET_UART1_RX_DMA_BUF_SIZE, &s_ring1, 0U, 1U },
#else
    { NULL, NULL, NULL, 0U, NULL, 0U, 0U },
#endif
#if NET_UART3_ENABLE
    /* 下标 1：NET_UART_WIFI → USART3（PB10/PB11） */
    { &huart3, &hdma_usart3_rx, s_dma_buf3, NET_UART3_RX_DMA_BUF_SIZE, &s_ring3, 0U, 1U },
#else
    { NULL, NULL, NULL, 0U, NULL, 0U, 0U },
#endif
};

/* ========== 内部函数 ========== */

/* 取实例：越界或未启用一律返回 NULL，调用者统一按"没这件事"处理 */
static net_uart_ctx_t *net_uart_ctx(NET_UART_Id_t id)
{
    if ((uint32_t)id >= (uint32_t)NET_UART_COUNT)
    {
        return NULL;
    }
    if (s_uart[id].ready == 0U)
    {
        return NULL;
    }
    return &s_uart[id];
}

/*----------------------------------------------------------------------------
 * 启动 DMA 循环接收 + 使能 IDLE 中断（每次错误自愈也走它）
 *----------------------------------------------------------------------------*/
static void net_uart_start_rx_dma(net_uart_ctx_t *ctx)
{
    /* ★ HAL_UART_Receive_DMA 只在 RxState == HAL_UART_STATE_READY 时才真正启动：
     *   若上一次接收没被 HAL 认为"已结束"（RxState != READY），它会直接返回
     *   HAL_BUSY 并且一个寄存器都不写——接收链路就这么静默地没拉起来，
     *   现象是"串口收不到任何数据，但程序其它部分一切正常"。
     *   所以必须检查返回值：失败就把 HAL 的接收状态机复位成 READY 再启动一次，
     *   保证"开机初始化"与"出错自愈"两条路径都真的把链路拉起来。 */
    if (HAL_UART_Receive_DMA(ctx->huart, ctx->dma_buf, (uint16_t)ctx->dma_size) != HAL_OK)
    {
        ctx->huart->RxState = HAL_UART_STATE_READY;   /* 复位 HAL 内部接收状态机 */
        (void)HAL_UART_Receive_DMA(ctx->huart, ctx->dma_buf, (uint16_t)ctx->dma_size);
    }
    __HAL_UART_ENABLE_IT(ctx->huart, UART_IT_IDLE);  /* 打开"总线空闲"中断 */
}

/*----------------------------------------------------------------------------
 * 对外：启动接收链路（永远存在，实例被裁掉时什么都不做）
 *----------------------------------------------------------------------------*/
void net_uart_init(NET_UART_Id_t id)
{
    net_uart_ctx_t *ctx = net_uart_ctx(id);

    if (ctx == NULL)
    {
        return;
    }

    rb_init(ctx->ring);          /* 环形缓冲读写指针归零 */
    ctx->last_pos = 0U;

    /* USART / DMA / 引脚 / NVIC 已由 CubeMX 在 main() 里初始化完毕
     *（usart.c 的 HAL_UART_MspInit 里已把两个串口的抢占优先级都配成 5，
     *  与 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 相等；
     *  本工程统一纪律是 ISR 内不调任何 RTOS API），
     *  这里只管把接收链路拉起来 */
    net_uart_start_rx_dma(ctx);
}

/*----------------------------------------------------------------------------
 * 对外：IDLE 判帧核心（永远存在，实例被裁掉时是空壳）
 *
 * 原理（最值得读懂的 20 行）：
 *   DMA 计数器(NDTR)是"递减计数"——每搬走一个字节减 1。
 *     已写入位置 pos = 缓冲大小 - 当前计数值
 *     新数据长度 len = pos - 上次消费位置 start（绕回了就补一整圈）
 *   然后把 [start, pos) 这段从 DMA 缓冲拷进软件环形缓冲
 *   （源数据可能跨 DMA 缓冲末尾，所以分两段写）
 *
 *   ★ 两个串口共用这 20 行：变的只是 ctx 里的四个指针。
 *----------------------------------------------------------------------------*/
void net_uart_rx_idle_handler(NET_UART_Id_t id)
{
    net_uart_ctx_t *ctx = net_uart_ctx(id);
    uint32_t pos;
    uint32_t start;
    uint32_t len;
    uint32_t first;

    if (ctx == NULL)
    {
        return;
    }

    pos   = ctx->dma_size - __HAL_DMA_GET_COUNTER(ctx->hdma_rx);
    start = ctx->last_pos;

    if (pos >= start)
    {
        len = pos - start;                                   /* 没绕回：直接相减 */
    }
    else
    {
        len = ctx->dma_size - start + pos;                   /* 绕回了：补一整圈 */
    }
    ctx->last_pos = pos;   /* 记下本次搬到哪儿，下次从这里接着算 */

    if (len == 0U)
    {
        return;             /* 中断来了但没新数据（例如刚自愈重启过）：直接返回 */
    }

    /* 分段拷贝：先搬 [start, 缓冲末尾)，再搬 [缓冲开头, pos) */
    first = ctx->dma_size - start;
    if (len <= first)
    {
        (void)rb_write(ctx->ring, &ctx->dma_buf[start], len);
    }
    else
    {
        (void)rb_write(ctx->ring, &ctx->dma_buf[start], first);
        (void)rb_write(ctx->ring, &ctx->dma_buf[0], len - first);
    }
}

/*----------------------------------------------------------------------------
 * 对外：查询可读字节数
 *----------------------------------------------------------------------------*/
uint32_t net_uart_available(NET_UART_Id_t id)
{
    net_uart_ctx_t *ctx = net_uart_ctx(id);

    if (ctx == NULL)
    {
        return 0U;
    }
    return (uint32_t)rb_count(ctx->ring);
}

/*----------------------------------------------------------------------------
 * 对外：读出数据（不阻塞）
 *----------------------------------------------------------------------------*/
uint32_t net_uart_read(NET_UART_Id_t id, uint8_t *buf, uint32_t len)
{
    net_uart_ctx_t *ctx = net_uart_ctx(id);

    if ((ctx == NULL) || (buf == NULL) || (len == 0U))
    {
        return 0U;
    }
    return (uint32_t)rb_read(ctx->ring, buf, len);
}

/*----------------------------------------------------------------------------
 * 对外：阻塞发送一段数据
 *----------------------------------------------------------------------------*/
void net_uart_send(NET_UART_Id_t id, const uint8_t *buf, uint32_t len)
{
    net_uart_ctx_t *ctx = net_uart_ctx(id);

    if ((ctx == NULL) || (buf == NULL) || (len == 0U))
    {
        return;
    }
    (void)HAL_UART_Transmit(ctx->huart, (uint8_t *)buf, (uint16_t)len, NET_UART_TX_TIMEOUT_MS);
}

/*----------------------------------------------------------------------------
 * 对外：阻塞发送字符串
 *----------------------------------------------------------------------------*/
void net_uart_send_string(NET_UART_Id_t id, const char *str)
{
    uint32_t len = 0U;

    if (str == NULL)
    {
        return;
    }
    while (str[len] != '\0')
    {
        len++;
    }
    net_uart_send(id, (const uint8_t *)str, len);
}

/* ==========================================================================
 *  错误自愈回调（重写 HAL 弱函数）
 *
 *  触发时机：接收溢出(ORE)、DMA 错误、帧错误、噪声等。
 *  此时 HAL 已经停止接收，这里统一清缓冲、重启 DMA——链路自动恢复，
 *  不用按复位键，也不用拔串口线。
 *
 *  ★ 两个串口都要管：ESP8266 那条线在 WiFi 信号差时更容易出 ORE，
 *    没有这条自愈，一次溢出就会让"上云"永久失效，只能按复位。
 * ========================================================================== */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    net_uart_ctx_t *ctx = NULL;
    uint32_t i;

    for (i = 0U; i < (uint32_t)NET_UART_COUNT; i++)
    {
        if ((s_uart[i].ready != 0U) && (s_uart[i].huart == huart))
        {
            ctx = &s_uart[i];
            break;
        }
    }
    if (ctx == NULL)
    {
        return;   /* 不是本层管的串口，交给别人的回调 */
    }

    __HAL_UART_DISABLE_IT(ctx->huart, UART_IT_IDLE);
    rb_init(ctx->ring);          /* 丢掉残缺数据，宁缺毋滥 */
    ctx->last_pos = 0U;
    net_uart_start_rx_dma(ctx);  /* 重新拉起接收链路 */
}
