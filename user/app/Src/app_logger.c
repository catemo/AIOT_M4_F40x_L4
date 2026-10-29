/**
 ******************************************************************************
 * @file    app_logger.c
 * @brief   应用层（app）—— W25Q64 环形日志实现（2.3 外部存储器新增）
 *
 *  接口的完整说明写在 app_logger.h。本文件的注释侧重"环形怎么转"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_LOGGER_ENABLE

#include "app_logger.h"
#include "app_dht11.h"   /* 跨链依赖：日志的取材就是 DHT11 快照（闸门看着） */
#include "mod_flash.h"
#include "mod_uart1.h"
#include "cmsis_os.h"    /* osKernelGetTickCount：记录的 tick_s */
#include <string.h>

/* ===================== 分区常量（全部由 cfg/器件参数推导，不手写死数） ===================== */

#define LOG_RECS_PER_SECTOR  (W25Q64_SECTOR_SIZE / sizeof(app_log_rec_t))  /* 4096/8 = 512 */
#define LOG_SECTOR_COUNT     ((W25Q64_SIZE_BYTES - APP_LOG_AREA_ADDR) / W25Q64_SECTOR_SIZE)  /* 2047 */
#define LOG_TOTAL_RECS       ((uint32_t)LOG_SECTOR_COUNT * LOG_RECS_PER_SECTOR)

/* 头扇区身份（8 字节：'L''O''G''1' + 版本 + 保留） */
static const uint8_t s_hdr_magic[4] = { 'L', 'O', 'G', '1' };
#define LOG_HDR_VERSION   1u

/* ===================== 运行期状态 ===================== */

static uint32_t s_write_pos = 0U;   /* 下一条要写的记录序号（0 ~ TOTAL-1） */
static uint32_t s_count     = 0U;   /* 有效记录数（首次写满前 = s_write_pos） */
static uint16_t s_seq       = 0U;   /* 下一条的记录号（65536 一圈，环形覆盖的依据） */

/* ===================== 打印小工具 ===================== */

static void out_str(const char *s) { MOD_UART1_SendString(s); }

static void out_u32(uint32_t v)
{
    char b[11];
    int  i = 10;

    b[10] = '\0';
    do {
        b[--i] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    out_str(&b[i]);
}

static void out_rc(int rc)   /* 返回码带符号打印：-2 / -3 / -4 直接进日志 */
{
    if (rc < 0)
    {
        out_str("-");
        out_u32((uint32_t)(-rc));
    }
    else
    {
        out_u32((uint32_t)rc);
    }
}

static void out_hex8(uint8_t v)
{
    static const char H[] = "0123456789ABCDEF";
    char b[3];

    b[0] = H[v >> 4];
    b[1] = H[v & 0xFU];
    b[2] = '\0';
    out_str(b);
}

static void rec_to_bytes(const app_log_rec_t *rec, uint8_t *b)
{
    (void)memcpy(b, rec, sizeof(*rec));   /* pack(1)：结构与字节序一一对应 */
}

static void bytes_to_rec(const uint8_t *b, app_log_rec_t *rec)
{
    (void)memcpy(rec, b, sizeof(*rec));
}

/* ===================== 对外接口 ===================== */

int APP_LOGGER_Init(void)
{
    uint8_t id[3];
    uint8_t hdr[8];
    uint32_t sector;
    uint8_t  found = 0U;

    /* ① 验明正身：JEDEC ID 打出来（EF 40 17 = W25Q64，EF 40 18 = W25Q128） */
    if (mod_flash_read_jedec(id) != MOD_FLASH_OK)
    {
        out_str("[FLASH ] JEDEC read FAILED (check PB3/4/5 + PG10 CS + mode 0)\r\n");
        return APP_LOGGER_ERR_NO_DEV;
    }
    out_str("[FLASH ] JEDEC ID = ");
    out_hex8(id[0]); out_str(" ");
    out_hex8(id[1]); out_str(" ");
    out_hex8(id[2]); out_str("\r\n");

    if (mod_flash_init() != MOD_FLASH_OK)
    {
        out_str("[FLASH ] unexpected ID (expect EF 40 17/18) -> logger disabled\r\n");
        return APP_LOGGER_ERR_NO_DEV;
    }
    out_str("[FLASH ] model = ");
    out_str(mod_flash_model_name());   /* 型号翻译在平台层，本层不认芯片 */
    out_str("\r\n");

    /* ② 头扇区：身份不对就重写（新芯片是 0xFF，天然命中这条路） */
    if ((mod_flash_read(APP_LOG_HDR_ADDR, hdr, 8U) != MOD_FLASH_OK) ||
        (memcmp(hdr, s_hdr_magic, 4U) != 0U) ||
        (hdr[4] != LOG_HDR_VERSION))
    {
        /* ★ 失败必须说出返回码：三种失败方向的排查方法完全不同，只说
         *   "erase FAILED" 等于让使用者从零开始猜（本册首跑就踩过这一条） */
        {
            int rc = mod_flash_sector_erase(APP_LOG_HDR_ADDR);
            if (rc != MOD_FLASH_OK)
            {
                out_str("[FLASH ] header erase FAILED rc=");
                out_rc(rc);
                out_str(" (-2=IO: PB3/4/5+PG10 CS, -3=TIMEOUT: tSE 超时, -4=WREN: WEL 没置上)\r\n");
                return APP_LOGGER_ERR_IO;
            }
        }
        hdr[0] = s_hdr_magic[0]; hdr[1] = s_hdr_magic[1];
        hdr[2] = s_hdr_magic[2]; hdr[3] = s_hdr_magic[3];
        hdr[4] = LOG_HDR_VERSION; hdr[5] = 0U; hdr[6] = 0U; hdr[7] = 0U;
        if (mod_flash_page_program(APP_LOG_HDR_ADDR, hdr, 8U) != MOD_FLASH_OK)
        {
            out_str("[FLASH ] header program FAILED\r\n");
            return APP_LOGGER_ERR_IO;
        }
    }

    /* ③ 扫描写指针：逐扇区读首记录，找第一个空位（首字节 0xFF 的记录）
     *    —— 掉电重启不重头记的关键 */
    s_write_pos = LOG_TOTAL_RECS;
    s_count     = LOG_TOTAL_RECS;
    s_seq       = 0U;

    for (sector = 0U; sector < (uint32_t)LOG_SECTOR_COUNT; sector++)
    {
        uint32_t base = APP_LOG_AREA_ADDR + sector * W25Q64_SECTOR_SIZE;
        uint8_t  first[8];

        if (mod_flash_read(base, first, 8U) != MOD_FLASH_OK)
        {
            return APP_LOGGER_ERR_IO;
        }
        if (first[0] == 0xFFU)   /* 这一扇区还没写满：空位就在扇区里 */
        {
            uint32_t rec;
            found = 1U;
            for (rec = 0U; rec < (uint32_t)LOG_RECS_PER_SECTOR; rec++)
            {
                uint8_t b[8];
                if (mod_flash_read(base + rec * 8U, b, 8U) != MOD_FLASH_OK)
                {
                    return APP_LOGGER_ERR_IO;
                }
                if (b[0] == 0xFFU)
                {
                    s_write_pos = sector * LOG_RECS_PER_SECTOR + rec;
                    s_count     = s_write_pos;
                    /* seq 接着上一条涨：环形覆盖的依据 */
                    if ((s_write_pos > 0U) && (s_count > 0U))
                    {
                        app_log_rec_t prev;
                        uint8_t       pb[8];
                        if (mod_flash_read(APP_LOG_AREA_ADDR +
                                           (s_write_pos - 1U) * 8U,
                                           pb, 8U) == MOD_FLASH_OK)
                        {
                            bytes_to_rec(pb, &prev);
                            s_seq = (uint16_t)(prev.seq + 1U);
                        }
                    }
                    break;
                }
            }
            break;
        }
    }

    if (found == 0U)
    {
        /* 整片 8 MB 全满：环形重启（教学工程简化，见 app_logger.h 说明） */
        s_write_pos = 0U;
        s_count     = 0U;
        s_seq       = 0U;
        out_str("[FLASH ] sector scan -> FULL, ring restart from 0\r\n");
    }
    else if (s_write_pos == 0U)
    {
        out_str("[FLASH ] sector scan -> empty head\r\n");
    }
    else
    {
        out_str("[FLASH ] sector scan -> resume at rec ");
        out_u32(s_write_pos);
        out_str(" seq=");
        out_u32(s_seq);
        out_str("\r\n");
    }
    return APP_LOGGER_OK;
}

int APP_LOGGER_Append(uint32_t tick_s, int16_t temp_x10, uint16_t humi_x10)
{
    app_log_rec_t rec;
    uint8_t       b[8];
    uint32_t      addr;
    uint32_t      sector_base;
    uint8_t       slot[8];
    int           rc;

    rec.tick_s   = tick_s;
    rec.temp_x10 = temp_x10;
    rec.humi_x10 = humi_x10;
    rec.seq      = s_seq;
    rec_to_bytes(&rec, b);

    addr        = APP_LOG_AREA_ADDR + s_write_pos * 8U;
    sector_base = APP_LOG_AREA_ADDR +
                  (s_write_pos / LOG_RECS_PER_SECTOR) * W25Q64_SECTOR_SIZE;

    /* 先擦后写决策：要写的位置不是空的（环形回到旧数据）→ 整扇区重擦。
     * "擦最旧"这个判断只有业务层知道 —— 指令序列在平台层，决策在本层 */
    if (mod_flash_read(addr, slot, 8U) != MOD_FLASH_OK)
    {
        return APP_LOGGER_ERR_IO;
    }
    if (slot[0] != 0xFFU)
    {
        rc = mod_flash_sector_erase(sector_base);   /* 典型 45 ms，忙等待在平台层 */
        if (rc != MOD_FLASH_OK)
        {
            return rc;
        }
    }

    /* 8 字节记录、8 字节对齐：一次页编程绝不跨 256 B 页 */
    rc = mod_flash_page_program(addr, b, 8U);
    if (rc != MOD_FLASH_OK)
    {
        return rc;
    }
    rc = mod_flash_verify(addr, b, 8U);   /* "写进去的必须能读出来"，每条都验 */
    if (rc != MOD_FLASH_OK)
    {
        return rc;
    }

    /* 三本账一起推进 */
    s_write_pos++;
    if (s_write_pos >= LOG_TOTAL_RECS)
    {
        s_write_pos = 0U;      /* 环形回绕自动接回头部 */
    }
    if (s_count < LOG_TOTAL_RECS)
    {
        s_count++;
    }
    s_seq++;                   /* u16 一圈 65536：覆盖判据只看"接不接得上" */

    return APP_LOGGER_OK;
}

int APP_LOGGER_Read(uint32_t index, app_log_rec_t *out)
{
    uint32_t physical;
    uint8_t  b[8];

    if ((out == NULL) || (index >= s_count))
    {
        return APP_LOGGER_ERR_PARAM;
    }
    /* index=0 → 最新一条（写指针的前一格），往回数 */
    physical = (s_write_pos + LOG_TOTAL_RECS - 1U - index) % LOG_TOTAL_RECS;

    if (mod_flash_read(APP_LOG_AREA_ADDR + physical * 8U, b, 8U) != MOD_FLASH_OK)
    {
        return APP_LOGGER_ERR_IO;
    }
    bytes_to_rec(b, out);
    return APP_LOGGER_OK;
}

uint32_t APP_LOGGER_Count(void)
{
    return s_count;
}

void APP_LOGGER_Periodic(void)
{
    APP_DHT11_Snapshot_t snap;
    static uint8_t       warned = 0U;
    int                  rc;
    app_log_rec_t        chk;

    APP_DHT11_GetSnapshot(&snap);
    if (snap.valid == 0U)
    {
        if (warned == 0U)
        {
            out_str("[LOG] skip: DHT11 snapshot not ready yet\r\n");
            warned = 1U;    /* 只提醒一次，不每 5 秒刷屏 */
        }
        return;
    }

    /* DHT11 快照 → ×10 定点：31.8 ℃ = 318，与记录格式一致 */
    rc = APP_LOGGER_Append(osKernelGetTickCount() / 1000U,
                           (int16_t)((uint16_t)snap.temp_int * 10U + snap.temp_dec),
                           (uint16_t)((uint16_t)snap.humi_int * 10U + snap.humi_dec));

    if (rc != APP_LOGGER_OK)
    {
        out_str("[LOG] append FAILED (");
        out_u32((uint32_t)(-rc));
        out_str(")\r\n");
        return;
    }

    /* 读回比对已经发生在 Append 内部（verify），这里只做"读 0 即最新"的示范 */
    if ((APP_LOGGER_Read(0U, &chk) == APP_LOGGER_OK) && (chk.seq == (uint16_t)(s_seq - 1U)))
    {
        out_str("[LOG#");
        out_u32(s_count);
        out_str(" ] t=");
        out_u32(chk.tick_s);
        out_str("s T=");
        out_u32((uint32_t)chk.temp_x10 / 10U);
        out_str(".");
        out_u32((uint32_t)chk.temp_x10 % 10U);
        out_str("C H=");
        out_u32((uint32_t)chk.humi_x10 / 10U);
        out_str(".");
        out_u32((uint32_t)chk.humi_x10 % 10U);
        out_str("% verify OK\r\n");
    }
    else
    {
        out_str("[LOG] readback mismatch\r\n");
    }
}

#endif /* APP_LOGGER_ENABLE */
