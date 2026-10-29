/**
 ******************************************************************************
 * @file    net_spi.c
 * @brief   网络层（network）—— SPI1 总线实现（2.3 外部存储器新增）
 *
 *  接口的完整说明写在 net_spi.h。本文件的注释侧重"包络为什么这么做"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if NET_SPI_ENABLE

#include "net_spi.h"
#include "spi.h"     /* hspi1（外设本体在 Core，main 无条件初始化） */
#include "main.h"    /* F_CS_Pin / F_CS_GPIO_Port（main.h 的引脚宏） */

/*----------------------------------------------------------------------------
 * HAL 状态 → 本层返回码
 *----------------------------------------------------------------------------*/
static int hal_to_net(HAL_StatusTypeDef st)
{
    switch (st)
    {
        case HAL_OK:      return NET_SPI_OK;
        case HAL_BUSY:    return NET_SPI_BUSY;
        case HAL_TIMEOUT: return NET_SPI_TIMEOUT;
        default:          return NET_SPI_ERR_IO;
    }
}

int net_spi_transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    HAL_StatusTypeDef st;

    if ((tx == NULL) || (rx == NULL) || (len == 0U) || (len > NET_SPI_XFER_MAX))
    {
        return NET_SPI_ERR_PARAM;
    }

    /* —— 片选包络开始：拉低 = 开始一句话 —— */
    HAL_GPIO_WritePin(F_CS_GPIO_Port, F_CS_Pin, GPIO_PIN_RESET);

    st = HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)tx, rx, len,
                                 NET_SPI_TIMEOUT_MS);

    /* —— 片选包络结束：无论成败先释放（拉高 = 句号）。
     *    顺序不能反：先释放再判结果，保证外设状态再差也不会把从机
     *    永久摁在选中态，下一帧还能干净地重新开始 —— */
    HAL_GPIO_WritePin(F_CS_GPIO_Port, F_CS_Pin, GPIO_PIN_SET);

    return hal_to_net(st);
}

#endif /* NET_SPI_ENABLE */
