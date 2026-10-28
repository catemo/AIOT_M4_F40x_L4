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

/*----------------------------------------------------------------------------
 * 【接口契约见 mod_uart1.h】
 *----------------------------------------------------------------------------*/
void MOD_UART1_Init(void)
{
    net_uart_init();      /* 网络层：启动 DMA 循环接收 + 打开 IDLE 中断 */
}

uint32_t MOD_UART1_Available(void)
{
    return net_uart_available();
}

uint32_t MOD_UART1_Read(uint8_t *buf, uint32_t len)
{
    return net_uart_read(buf, len);
}

void MOD_UART1_Send(const uint8_t *buf, uint32_t len)
{
    net_uart_send(buf, len);
}

void MOD_UART1_SendString(const char *str)
{
    net_uart_send_string(str);
}

#endif /* MOD_UART1_ENABLE */
