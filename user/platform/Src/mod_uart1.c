/**
 ******************************************************************************
 * @file    mod_uart1.c
 * @brief   平台层（platform）—— 串口1 统一入口实现（网络层薄转发）
 *
 *  每个函数只有一行转发：存在的意义不是"干活"，而是"统一名字"。
 *  上层只认识 MOD_UART1_*，不认识 net_uart_*，更不认识 huart1。
 *
 *  ★ 看起来是"空转一层"，实际买到了三样东西：
 *    ① 换硬件不换上层：USART1 → USART2、或干脆换成 MQTT 通道时，
 *       只改本文件转发到谁，app_cmd.c 一行不动
 *    ② 名字稳定：net_uart_xxx 将来改签名（比如加超时参数、改 DMA 发送），
 *       改动被挡在本文件里，不扩散到业务层
 *    ③ 位置明确：本册它是薄转发，K05 章节要往里放协议接入与物模型上报，
 *       那时它就不是薄的了——现在先把位置占好
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 mod_uart1.h。
 ******************************************************************************
 */
#include "module_cfg.h"
#if MOD_UART1_ENABLE

#include "mod_uart1.h"
#include "net_uart.h"    /* 平台层允许依赖网络层，方向是 上→下 */
#include "cmsis_os.h"    /* 发送互斥量：多任务共用一条调试串口必须串行化 */

/*----------------------------------------------------------------------------
 * ★ 发送串行化（2026-10-29 真机踩坑后加，是本次最重要的一处修复）
 *
 * 现象：5 个任务（LED 不用 / 串口 / DHT11 / 上云 / 存储）都在打日志，
 *       但真机上整行整行地"凭空消失"——本次首跑 [FLASH] JEDEC 行、
 *       [LOG#N] 行、存储任务 banner、[PARAM] probe 行全部不见了。
 *
 * 根因：MOD_UART1_SendString 直接落到 HAL_UART_Transmit，HAL 内部用
 *       __HAL_LOCK(huart) 保护；并发调用时第二个调用者拿到 HAL_BUSY，
 *       【一个字节都不发】就返回了。函数没有返回值，调用方无从得知，
 *       于是这一行就像从没写过一样 —— 排障时极具误导性。
 *
 * 修法：在平台层（OS 知识归平台层，网络层不认识 RTOS）加一把互斥量，
 *       懒创建：首次发送时才建，用 osKernelLock 保证只建一次。
 *       ★ 边界：只有任务上下文才会调 Send（上电 [BOOT] 探针在调度器启动
 *       之前，走的是 main.c 里直接 HAL_UART_Transmit，不经过这里），
 *       所以 osMutexAcquire(osWaitForever) 是安全的。
 *----------------------------------------------------------------------------*/
static osMutexId_t s_tx_mutex = NULL;

static void tx_lock(void)
{
    if (s_tx_mutex == NULL)
    {
        osKernelLock();                     /* 建锁期间不许被切走，防重复创建 */
        if (s_tx_mutex == NULL)
        {
            s_tx_mutex = osMutexNew(NULL);
        }
        osKernelUnlock();
    }
    if (s_tx_mutex != NULL)
    {
        (void)osMutexAcquire(s_tx_mutex, osWaitForever);
    }
}

static void tx_unlock(void)
{
    if (s_tx_mutex != NULL)
    {
        (void)osMutexRelease(s_tx_mutex);
    }
}

/*----------------------------------------------------------------------------
 * 【接口契约见 mod_uart1.h】
 *----------------------------------------------------------------------------*/
void MOD_UART1_Init(void)
{
    net_uart_init(NET_UART_DBG);      /* 网络层：启动 USART1 实例的 DMA 循环接收 + IDLE 中断 */
}

uint32_t MOD_UART1_Available(void)
{
    return net_uart_available(NET_UART_DBG);
}

uint32_t MOD_UART1_Read(uint8_t *buf, uint32_t len)
{
    return net_uart_read(NET_UART_DBG, buf, len);
}

void MOD_UART1_Send(const uint8_t *buf, uint32_t len)
{
    tx_lock();
    net_uart_send(NET_UART_DBG, buf, len);
    tx_unlock();
}

void MOD_UART1_SendString(const char *str)
{
    tx_lock();
    net_uart_send_string(NET_UART_DBG, str);
    tx_unlock();
}

#endif /* MOD_UART1_ENABLE */
