/**
 ******************************************************************************
 * @file    mod_flash.c
 * @brief   平台层（platform）—— W25Q64 指令集实现（2.3 外部存储器新增）
 *
 *  接口的完整说明写在 mod_flash.h。本文件的注释侧重"指令帧怎么拼"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if MOD_FLASH_ENABLE

#include "mod_flash.h"
#include "net_spi.h"
#include <string.h>
#include "cmsis_os.h"   /* osDelay：忙等待期间让出 CPU（铁律②） */

/* 上一次读到的容量码（0 = 还没读过）：型号判定与型号名查询共用一次读操作 */
static uint8_t s_capacity = 0U;

/* ===================== 内部工具 ===================== */

/*----------------------------------------------------------------------------
 * 拼一条"指令 + 24 位地址"的发送帧并整包交换
 * ★ 指令、地址、数据必须待在同一个片选包络里（net_spi 一次搞定）
 *----------------------------------------------------------------------------*/
static int xfer_cmd_addr(uint8_t cmd, uint32_t addr,
                         const uint8_t *data, uint8_t *rx,
                         uint16_t data_len)
{
    uint8_t  tx[4U + 256U];      /* 4 B 指令头 + 最多一页数据 */
    uint8_t  rxbuf[4U + 256U];
    uint16_t total = (uint16_t)(4U + data_len);

    tx[0] = cmd;
    tx[1] = (uint8_t)(addr >> 16);
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)(addr);

    if ((data != NULL) && (data_len > 0U))
    {
        (void)memcpy(&tx[4], data, data_len);
    }

    return net_spi_transfer(tx, (rx != NULL) ? rx : rxbuf, total);
}

/*----------------------------------------------------------------------------
 * 读状态寄存器
 *----------------------------------------------------------------------------*/
static int read_status(uint8_t *sr)
{
    uint8_t tx[2] = { W25Q64_CMD_READ_STATUS, 0x00U };  /* 第二字节是哑字节 */
    uint8_t rx[2] = { 0U, 0U };
    int     rc    = net_spi_transfer(tx, rx, 2U);

    if (rc != NET_SPI_OK)
    {
        return rc;    /* 负值直接透传 */
    }
    *sr = rx[1];      /* rx[0] 是发指令时收到的垃圾 */
    return MOD_FLASH_OK;
}

int mod_flash_busy(void)
{
    uint8_t sr = 0U;

    if (read_status(&sr) != MOD_FLASH_OK)
    {
        return MOD_FLASH_ERR_IO;
    }
    return ((sr & W25Q64_SR_BUSY) != 0U) ? 1 : 0;
}

/*----------------------------------------------------------------------------
 * 忙等待：轮询 RDSR 直到 BUSY 清零或超时；每轮 osDelay(1) 让出 CPU
 *----------------------------------------------------------------------------*/
static int wait_not_busy(uint32_t timeout_ms)
{
    uint32_t waited = 0U;

    while (waited < timeout_ms)
    {
        int b = mod_flash_busy();

        if (b < 0)
        {
            return b;               /* 总线错误 */
        }
        if (b == 0)
        {
            return MOD_FLASH_OK;    /* 空闲了 */
        }
        osDelay(1U);
        waited++;
    }
    return MOD_FLASH_ERR_TIMEOUT;
}

int mod_flash_wren(void)
{
    uint8_t sr = 0U;

    /* WREN 本身不带地址不帽数据：第二字节哑字节凑一次交换 */
    uint8_t tx[2] = { W25Q64_CMD_WRITE_ENABLE, 0x00U };
    uint8_t rx[2] = { 0U, 0U };

    if (net_spi_transfer(tx, rx, 2U) != NET_SPI_OK)
    {
        return MOD_FLASH_ERR_IO;
    }

    if (read_status(&sr) != MOD_FLASH_OK)
    {
        return MOD_FLASH_ERR_IO;
    }
    if ((sr & W25Q64_SR_WEL) == 0U)
    {
        return MOD_FLASH_ERR_WREN;  /* 指令被静默丢弃：查片选包络/时序 */
    }
    return MOD_FLASH_OK;
}

/* ===================== 对外接口 ===================== */

int mod_flash_init(void)
{
    uint8_t id[3] = { 0U, 0U, 0U };

    if (mod_flash_read_jedec(id) != MOD_FLASH_OK)
    {
        return MOD_FLASH_ERR_IO;
    }
    if ((id[0] != W25Q64_JEDEC_MANUF) ||
        (id[1] != W25Q64_JEDEC_MEMTYPE) ||
        ((id[2] != W25Q64_JEDEC_CAPACITY) &&
         (id[2] != W25Q128_JEDEC_CAPACITY)))
    {
        return MOD_FLASH_ERR_IO;    /* 型号不符：串口侧会打出实际 ID */
    }
    return MOD_FLASH_OK;
}

int mod_flash_capacity_id(void)
{
    if (s_capacity == 0U)
    {
        uint8_t id[3] = { 0U, 0U, 0U };

        if (mod_flash_read_jedec(id) != MOD_FLASH_OK)
        {
            return MOD_FLASH_ERR_IO;
        }
        s_capacity = id[2];
    }
    return (int)s_capacity;
}

const char *mod_flash_model_name(void)
{
    int cap = mod_flash_capacity_id();

    if (cap == (int)W25Q64_JEDEC_CAPACITY)
    {
        return "W25Q64 (8 MB)";
    }
    if (cap == (int)W25Q128_JEDEC_CAPACITY)
    {
        return "W25Q128 (16 MB)";
    }
    return "unknown (not Winbond 0xEF/0x40 or bad capacity)";
}

/*----------------------------------------------------------------------------
 * 总容量随型号变：W25Q64 = 8 MB，W25Q128 = 16 MB（板上实测是 128）
 * 容量还没读过时按小的算（保守），init 后自然就准了
 *----------------------------------------------------------------------------*/
uint32_t mod_flash_total_size(void)
{
    return (s_capacity == W25Q128_JEDEC_CAPACITY)
               ? (uint32_t)W25Q128_SIZE_BYTES
               : (uint32_t)W25Q64_SIZE_BYTES;
}

int mod_flash_read_jedec(uint8_t id[3])
{
    uint8_t tx[4] = { W25Q64_CMD_JEDEC_ID, 0U, 0U, 0U };
    uint8_t rx[4] = { 0U, 0U, 0U, 0U };

    if ((id == NULL) || (net_spi_transfer(tx, rx, 4U) != NET_SPI_OK))
    {
        return MOD_FLASH_ERR_IO;
    }
    id[0] = rx[1];
    id[1] = rx[2];
    id[2] = rx[3];
    s_capacity = id[2];      /* 顺手记住容量，型号名查询不再多读一次 */
    return MOD_FLASH_OK;
}

int mod_flash_read(uint32_t addr, uint8_t *buf, uint16_t len)
{
    uint8_t tx[4U + 256U];      /* 指令头 + 哑字节：全双工必须发满等长 */
    uint8_t rx[4U + 256U];      /* rx 前 4 字节是发指令头时收到的垃圾 */
    uint16_t i;

    if ((buf == NULL) || (len == 0U) || (len > W25Q64_PAGE_SIZE) ||
        (((uint32_t)addr + (uint32_t)len) > mod_flash_total_size()))
    {
        return MOD_FLASH_ERR_PARAM;
    }

    tx[0] = W25Q64_CMD_READ_DATA;
    tx[1] = (uint8_t)(addr >> 16);
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)(addr);
    for (i = 0U; i < len; i++)
    {
        tx[4U + i] = 0x00U;     /* 哑字节：读的时候主机只出时钟不出数据 */
    }

    if (net_spi_transfer(tx, rx, (uint16_t)(4U + len)) != NET_SPI_OK)
    {
        return MOD_FLASH_ERR_IO;
    }
    (void)memcpy(buf, &rx[4], len);   /* 有效数据从第 5 字节开始 */
    return MOD_FLASH_OK;
}

int mod_flash_page_program(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    int rc;

    if ((buf == NULL) || (len == 0U) || (len > W25Q64_PAGE_SIZE) ||
        (((uint32_t)addr + (uint32_t)len) > mod_flash_total_size()) ||
        (((addr % W25Q64_PAGE_SIZE) + (uint32_t)len) > W25Q64_PAGE_SIZE))
    {
        return MOD_FLASH_ERR_PARAM;     /* ★ 跨页在这里直接拒绝 */
    }

    rc = mod_flash_wren();              /* WEL 每次编程前都要重新置位 */
    if (rc != MOD_FLASH_OK)
    {
        return rc;
    }

    if (xfer_cmd_addr(W25Q64_CMD_PAGE_PROGRAM, addr, buf, NULL, len) != NET_SPI_OK)
    {
        return MOD_FLASH_ERR_IO;
    }

    return wait_not_busy(W25Q64_PAGE_PROG_TIMEOUT_MS);
}

int mod_flash_sector_erase(uint32_t addr)
{
    int rc;

    if ((addr % W25Q64_SECTOR_SIZE) != 0U)
    {
        return MOD_FLASH_ERR_PARAM;     /* 扇区擦必须 4 KB 对齐 */
    }

    rc = mod_flash_wren();              /* 擦也要先解锁 */
    if (rc != MOD_FLASH_OK)
    {
        return rc;
    }

    if (xfer_cmd_addr(W25Q64_CMD_SECTOR_ERASE, addr, NULL, NULL, 0U) != NET_SPI_OK)
    {
        return MOD_FLASH_ERR_IO;
    }

    return wait_not_busy(W25Q64_SECTOR_ERASE_TIMEOUT_MS);
}

int mod_flash_verify(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    uint8_t chunk[32U];     /* 分块读回比对：省栈 + 第一块不符立刻返回 */
    uint16_t done = 0U;

    while (done < len)
    {
        uint16_t n = (uint16_t)((uint32_t)(len - done) > sizeof(chunk)
                                    ? sizeof(chunk) : (len - done));
        uint16_t i;

        if (mod_flash_read(addr + done, chunk, n) != MOD_FLASH_OK)
        {
            return MOD_FLASH_ERR_IO;
        }
        for (i = 0U; i < n; i++)
        {
            if (chunk[i] != buf[done + i])
            {
                return MOD_FLASH_ERR_VERIFY;
            }
        }
        done = (uint16_t)(done + n);
    }
    return MOD_FLASH_OK;
}

int mod_flash_sector_write(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    uint32_t off;
    int      rc;

    if ((buf == NULL) || (len == 0U) || (len > W25Q64_SECTOR_SIZE) ||
        (((uint32_t)addr + (uint32_t)len) > mod_flash_total_size()))
    {
        return MOD_FLASH_ERR_PARAM;
    }

    /* 1. 擦：4 KB 全变 0xFF（NOR 只能把 1 写成 0，不擦就写 = 残渣） */
    rc = mod_flash_sector_erase(addr);
    if (rc != MOD_FLASH_OK)
    {
        return rc;
    }

    /* 2. 逐页编程：每页前重新 WREN（WEL 在每次编程后自动清零） */
    for (off = 0U; off < (uint32_t)len; off += W25Q64_PAGE_SIZE)
    {
        uint16_t n = (uint16_t)(((uint32_t)len - off) > W25Q64_PAGE_SIZE
                                    ? W25Q64_PAGE_SIZE : ((uint32_t)len - off));

        rc = mod_flash_page_program(addr + off, buf + off, n);
        if (rc != MOD_FLASH_OK)
        {
            return rc;
        }
    }

    /* 3. 读回比对："写进去的必须读得出来"（不信任单点成功，铁律③） */
    return mod_flash_verify(addr, buf, len);
}

#endif /* MOD_FLASH_ENABLE */
