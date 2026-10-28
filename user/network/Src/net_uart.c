/**
 ******************************************************************************
 * @file    net_uart.c
 * @brief   网络层（network）—— UART1 字节流实现（DMA 循环接收 + IDLE 判帧 + 环形缓冲）
 *
 *  本文件是"串口三件套"的落点：
 *    ① DMA 循环接收：一次性配置，硬件持续搬运，缓冲写满自动绕回，永不停止
 *    ② IDLE 判帧：总线一安静就进空闲中断，把"这段时间新到的字节"切成一帧
 *    ③ 环形缓冲：中断只管把字节塞进 ringbuf，任务有空了再来取（解耦、不丢）
 *
 *  ★ 三个刻意的取舍（都是为了让新手少踩坑）：
 *    ① ISR 里只搬数据，不解析、不打印、不调 RTOS API —— 外设不能拖死 CPU
 *    ② 环形缓冲满了就丢字节，不阻塞 —— 宁可丢，也不能让中断卡住
 *    ③ 发送用阻塞轮询 —— 本例只回几句短提示，几十微秒的占用可以接受；
 *       量产应换 DMA 发送（换时只改本文件，上层零改动）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 net_uart.h，
 *  本文件的注释侧重"判帧那段最核心的数学"与"为什么这么写"。
 *
 *  ★ 分层纪律：本文件只 include net_uart.h / ringbuf.h / usart.h，
 *    绝不 include mod_uart1.h（反向依赖）或 app_cmd.h（越级依赖）。
 ******************************************************************************
 */
#include "module_cfg.h"

#include "net_uart.h"
#include "ringbuf.h"     /* common 横切：无锁环形缓冲（ISR 写 / 任务读） */
#include "usart.h"       /* CubeMX 生成的全局句柄 huart1 */

#if NET_UART1_ENABLE

/* RX DMA 句柄：usart.c 里定义（CubeMX 没把它导出到头文件，这里 extern 一下）。
 * IDLE 判帧要读它的 NDTR 计数器 */
extern DMA_HandleTypeDef hdma_usart1_rx;

/* ========== 静态资源 ========== */

/* DMA 硬件环形缓冲：DMA 硬件写、CPU 读。
 * F407 的 SRAM 在 0x20000000，DMA1/2 都能直接访问，普通静态数组即可
 * （H7 工程里要用链接脚本定向到 AXI SRAM，本工程不需要） */
static uint8_t s_dma_rx_buf[NET_UART1_RX_DMA_BUF_SIZE];

/* 软件环形缓冲：IDLE 中断写（生产者）、串口任务读（消费者）。
 * 容量由 common 横切层的 CFG_RINGBUF_SIZE 决定（必须是 2 的幂） */
static ringbuf_t s_rx_ring;

/* DMA 缓冲"已消费位置"：上次 IDLE 中断搬到了哪里，下次从这里继续算新数据。
 * volatile：本变量在 ISR 与主循环两条路径上都会被改写 */
static volatile uint32_t s_dma_last_pos;

/* ========== 内部函数 ========== */
static void net_uart_start_rx_dma(void);

/*----------------------------------------------------------------------------
 * 启动 DMA 循环接收 + 使能 IDLE 中断（每次错误自愈也走它）
 *----------------------------------------------------------------------------*/
static void net_uart_start_rx_dma(void)
{
    /* ★ HAL_UART_Receive_DMA 只在 RxState == HAL_UART_STATE_READY 时才真正启动：
     *   若上一次接收没被 HAL 认为"已结束"（RxState != READY），它会直接返回
     *   HAL_BUSY 并且一个寄存器都不写——接收链路就这么静默地没拉起来，
     *   现象是"串口收不到任何数据，但程序其它部分一切正常"。
     *   所以必须检查返回值：失败就把 HAL 的接收状态机复位成 READY 再启动一次，
     *   保证"开机初始化"与"出错自愈"两条路径都真的把链路拉起来。 */
    if (HAL_UART_Receive_DMA(&huart1, s_dma_rx_buf, NET_UART1_RX_DMA_BUF_SIZE) != HAL_OK)
    {
        huart1.RxState = HAL_UART_STATE_READY;   /* 复位 HAL 内部接收状态机 */
        (void)HAL_UART_Receive_DMA(&huart1, s_dma_rx_buf, NET_UART1_RX_DMA_BUF_SIZE);
    }
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);  /* 打开"总线空闲"中断 */
}

#endif /* NET_UART1_ENABLE */

/*----------------------------------------------------------------------------
 * 对外：启动接收链路（永远存在，关开关时变空壳）  【接口契约见 net_uart.h】
 *----------------------------------------------------------------------------*/
void net_uart_init(void)
{
#if NET_UART1_ENABLE
    rb_init(&s_rx_ring);          /* 环形缓冲读写指针归零 */
    s_dma_last_pos = 0U;

    /* USART1 / DMA / 引脚 / NVIC 已由 CubeMX 在 main() 里初始化完毕
     *（usart.c 的 HAL_UART_MspInit 里已把 USART1 抢占优先级配成 5，
     *  与 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 相等；
     *  本工程统一纪律是 ISR 内不调任何 RTOS API），
     *  这里只管把接收链路拉起来 */
    net_uart_start_rx_dma();
#endif
}

/*----------------------------------------------------------------------------
 * 对外：IDLE 判帧核心（永远存在，关开关时变空壳）  【接口契约见 net_uart.h】
 *
 * 原理（最值得读懂的 20 行）：
 *   DMA 计数器(NDTR)是"递减计数"——每搬走一个字节减 1。
 *     已写入位置 pos = 缓冲大小 - 当前计数值
 *     新数据长度 len = pos - 上次消费位置 start（绕回了就补一整圈）
 *   然后把 [start, pos) 这段从 DMA 缓冲拷进软件环形缓冲
 *   （源数据可能跨 DMA 缓冲末尾，所以分两段写）
 *----------------------------------------------------------------------------*/
void net_uart_rx_idle_handler(void)
{
#if NET_UART1_ENABLE
    uint32_t pos   = NET_UART1_RX_DMA_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
    uint32_t start = s_dma_last_pos;
    uint32_t len;
    uint32_t first;

    if (pos >= start)
    {
        len = pos - start;                                    /* 没绕回：直接相减 */
    }
    else
    {
        len = NET_UART1_RX_DMA_BUF_SIZE - start + pos;        /* 绕回了：补一整圈 */
    }
    s_dma_last_pos = pos;   /* 记下本次搬到哪儿，下次从这里接着算 */

    if (len == 0U)
    {
        return;             /* 中断来了但没新数据（例如刚自愈重启过）：直接返回 */
    }

    /* 分段拷贝：先搬 [start, 缓冲末尾)，再搬 [缓冲开头, pos) */
    first = NET_UART1_RX_DMA_BUF_SIZE - start;
    if (len <= first)
    {
        (void)rb_write(&s_rx_ring, &s_dma_rx_buf[start], len);
    }
    else
    {
        (void)rb_write(&s_rx_ring, &s_dma_rx_buf[start], first);
        (void)rb_write(&s_rx_ring, &s_dma_rx_buf[0], len - first);
    }
#endif
}

#if NET_UART1_ENABLE

/*----------------------------------------------------------------------------
 * 对外：查询可读字节数  【接口契约见 net_uart.h】
 *----------------------------------------------------------------------------*/
uint32_t net_uart_available(void)
{
    return (uint32_t)rb_count(&s_rx_ring);
}

/*----------------------------------------------------------------------------
 * 对外：读出数据（不阻塞）  【接口契约见 net_uart.h】
 *----------------------------------------------------------------------------*/
uint32_t net_uart_read(uint8_t *buf, uint32_t len)
{
    return (uint32_t)rb_read(&s_rx_ring, buf, len);
}

/*----------------------------------------------------------------------------
 * 对外：阻塞发送一段数据  【接口契约见 net_uart.h】
 *----------------------------------------------------------------------------*/
void net_uart_send(const uint8_t *buf, uint32_t len)
{
    if ((buf == NULL) || (len == 0U))
    {
        return;
    }
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, NET_UART1_TX_TIMEOUT_MS);
}

/*----------------------------------------------------------------------------
 * 对外：阻塞发送字符串  【接口契约见 net_uart.h】
 *----------------------------------------------------------------------------*/
void net_uart_send_string(const char *str)
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
    net_uart_send((const uint8_t *)str, len);
}

/* ==========================================================================
 *  错误自愈回调（重写 HAL 弱函数）
 *
 *  触发时机：接收溢出(ORE)、DMA 错误、帧错误、噪声等。
 *  此时 HAL 已经停止接收，这里统一清缓冲、重启 DMA——链路自动恢复，
 *  不用按复位键，也不用拔串口线。
 * ========================================================================== */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        __HAL_UART_DISABLE_IT(&huart1, UART_IT_IDLE);
        rb_init(&s_rx_ring);          /* 丢掉残缺数据，宁缺毋滥 */
        s_dma_last_pos = 0U;
        net_uart_start_rx_dma();     /* 重新拉起接收链路 */
    }
}

#endif /* NET_UART1_ENABLE */
