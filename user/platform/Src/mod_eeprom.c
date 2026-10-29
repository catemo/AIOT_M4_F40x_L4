/**
 ******************************************************************************
 * @file    mod_eeprom.c
 * @brief   平台层（platform）—— AT24C02 EEPROM 器件语义实现（2.3 新增）
 *
 *  接口的完整说明写在 mod_eeprom.h。本文件的注释侧重"页拆分怎么不掉数据"。
 ******************************************************************************
 */
#include "module_cfg.h"
#if MOD_EEPROM_ENABLE

#include "mod_eeprom.h"
#include "net_i2c.h"
#include "cmsis_os.h"   /* osDelay：写周期轮询的间隔 */
#include <string.h>     /* memcmp：逐页读回比对 */

/* ===================== 内部状态 ===================== */

/* 当前器件型号：默认按小器件口径（保守），init 探测后改正。
 * 放在内部工具之前：写周期等待也要看它 */
static mod_eeprom_chip_t s_chip = MOD_EEPROM_CHIP_AT24C02;

/* 字地址位宽：16 位器件发 2 个地址字节，8 位器件发 1 个
 * （放在内部工具之前：写周期等待与逐页读写都要看它） */
static uint8_t mem_is_wide(void)
{
    return (s_chip == MOD_EEPROM_CHIP_AT24C256) ? 1U : 0U;
}

/* ===================== 内部工具 ===================== */

/*----------------------------------------------------------------------------
 * 写周期等待：先给足一个完整写周期，再轮询 ACK 确认
 * ★ 上限随型号变：AT24C02 = 5 ms，AT24C256 = 10 ms
 * ★ 为什么"先等足 tWR"而不是纯轮询（2026-10-29 真机踩坑）：
 *   教科书说"写周期内器件不应答，探到 ACK 即写完"，但部分 AT24C02（含兼容片）
 *   在写周期内【照样 ACK】，于是纯轮询立刻返回"假就绪"，紧接着发出的下一页
 *   写被器件吞掉 —— 首跑的现象正是"第 0 页（magic/version/crc）写进去了，
 *   第 1、2 页没进去"，于是 magic/version 读回正确、CRC 却永远对不上。
 *   先等足 tWR 是数据手册口径，代价只有几毫秒，换来的是确定性。
 *----------------------------------------------------------------------------*/
static int wait_write_done(void)
{
    uint32_t waited = 0U;
    uint32_t twr    = (s_chip == MOD_EEPROM_CHIP_AT24C256)
                          ? (uint32_t)AT24C256_WRITE_CYCLE_MS
                          : (uint32_t)AT24C02_WRITE_CYCLE_MS;

    osDelay(twr);                      /* ① 数据手册口径：先给足一个完整写周期 */

    while (waited < twr)               /* ② 再轮询 ACK 确认（最多再等一个 tWR） */
    {
        if (net_i2c_probe(AT24C02_ADDR) == NET_I2C_OK)
        {
            return MOD_EEPROM_OK;
        }
        osDelay(1U);
        waited++;
    }
    return MOD_EEPROM_ERR_TIMEOUT;
}

/*----------------------------------------------------------------------------
 * 写一页并逐页读回比对（不一致整页重来一次，再不一致如实报 VERIFY）
 * ★ 逐页校验的排障价值：20 B 配置块跨 3 页，全块 memcmp 只能说"有字节不对"；
 *   逐页比对能直接指出"第几页没写进去"。"magic/ver 对、CRC 错"这个现象
 *   用全块比对根本定位不到页，用逐页比对一眼就看出来。
 *----------------------------------------------------------------------------*/
static int write_page_checked(uint16_t addr, const uint8_t *buf, uint16_t n)
{
    uint8_t rb[AT24C256_PAGE_SIZE];    /* 按最大页宽开：两种器件都够用 */
    uint8_t try;
    int     rc = MOD_EEPROM_ERR_IO;

    for (try = 0U; try < 2U; try++)
    {
        if (net_i2c_write_ex(AT24C02_ADDR, addr, buf, n, mem_is_wide()) != NET_I2C_OK)
        {
            continue;                  /* 写事务失败：整页重来一次 */
        }
        rc = wait_write_done();
        if (rc != MOD_EEPROM_OK)
        {
            continue;                  /* 没写完（超时）：整页重来一次 */
        }
        if (net_i2c_read_ex(AT24C02_ADDR, addr, rb, n, mem_is_wide()) != NET_I2C_OK)
        {
            rc = MOD_EEPROM_ERR_IO;
            continue;
        }
        if (memcmp(rb, buf, n) == 0)
        {
            return MOD_EEPROM_OK;      /* 写进去的能读出来 —— 这一页才算数 */
        }
        rc = MOD_EEPROM_ERR_VERIFY;    /* 读回来对不上：再来一次 */
    }
    return rc;
}

/* ===================== 对外接口 ===================== */

mod_eeprom_chip_t mod_eeprom_chip(void)
{
    return s_chip;
}

const char *mod_eeprom_chip_name(void)
{
    return (s_chip == MOD_EEPROM_CHIP_AT24C256) ? "AT24C256" : "AT24C02";
}

uint16_t mod_eeprom_page_size(void)
{
    return (uint16_t)((s_chip == MOD_EEPROM_CHIP_AT24C256)
                          ? AT24C256_PAGE_SIZE : AT24C02_PAGE_SIZE);
}

uint32_t mod_eeprom_total_size(void)
{
    return (s_chip == MOD_EEPROM_CHIP_AT24C256)
               ? (uint32_t)AT24C256_SIZE_BYTES : (uint32_t)AT24C02_SIZE_BYTES;
}

/*----------------------------------------------------------------------------
 * 型号探测（两步，判别点是"8 位字地址写读成不成立"）：
 *
 *  第一步：按【1 字节字地址】在 0x00F0 写特征串再按 1 字节读回
 *    - AT24C02（真 8 位）：0xF0..0xF2 原样存取 → 读回一致 → 判 AT24C02
 *    - AT24C256：它把 0xF0 当【高位】地址、特征串第一个字节当【低位】地址，
 *      写落到 0xF0xx；随后的 1 字节读给不出低位地址、只能从内部指针接着读，
 *      读回来是空白 ≠ 特征串 → 不一致，进入第二步
 *
 *  第二步：按【2 字节字地址】在 0xFFF0 写读（32 KB 末尾，16 位器件的真地址）
 *    - 一致 → 判 AT24C256；仍不一致 → 保守回落 AT24C02（PDUMP 会揭穿真相）
 *
 *  ★ 为什么不能只做"16 位写读一致"这一刀：在 8 位器件上，16 位写和 16 位读
 *    会【一致地】错位（地址高位字节被当成字地址、低位字节被当成数据写进去，
 *    读的时候又原样错回来），读回特征值照样成立 —— 首版探测就栽在这里。
 *    唯一不对称的动作是"8 位写读"：它对 8 位器件成立、对 16 位器件必不成立。
 *
 *  ★ 两个牺牲地址都远离配置块（0x0000..0x13），探测写入碰不到业务数据：
 *    8 位器件落在 0xF0..0xF2；16 位器件第一步落到 0xF0xx、第二步 0xFFF0。
 *----------------------------------------------------------------------------*/
static uint8_t roundtrip_ok(uint16_t addr, uint8_t wide)
{
    const uint8_t pat[3] = { EEPROM_CHIP_PROBE_PATTERN,
                             (uint8_t)(~EEPROM_CHIP_PROBE_PATTERN & 0xFFU),
                             EEPROM_CHIP_PROBE_PATTERN };
    uint8_t rb[3];
    uint8_t i;

    if (net_i2c_write_ex(AT24C02_ADDR, addr, pat, 3U, wide) != NET_I2C_OK)
    {
        return 0U;
    }
    if (wait_write_done() != MOD_EEPROM_OK)
    {
        return 0U;
    }
    if (net_i2c_read_ex(AT24C02_ADDR, addr, rb, 3U, wide) != NET_I2C_OK)
    {
        return 0U;
    }
    for (i = 0U; i < 3U; i++)
    {
        if (rb[i] != pat[i])
        {
            return 0U;
        }
    }
    return 1U;
}

static mod_eeprom_chip_t detect_chip(void)
{
    /* 第一步：8 位字地址写读 —— 对 AT24C02 成立，对 AT24C256 必不成立 */
    if (roundtrip_ok((uint16_t)EEPROM_CHIP_PROBE_ADDR8, 0U) != 0U)
    {
        return MOD_EEPROM_CHIP_AT24C02;
    }
    /* 第二步：16 位字地址写读（只在第一步失败时才有意义） */
    if (roundtrip_ok((uint16_t)EEPROM_CHIP_PROBE_ADDR16, 1U) != 0U)
    {
        return MOD_EEPROM_CHIP_AT24C256;
    }
    return MOD_EEPROM_CHIP_AT24C02;   /* 都不通：保守按小器件走，PDUMP 会揭穿 */
}

int mod_eeprom_init(void)
{
    /* 先看总线（空闲电平 / 自救），再探器件（地址帧 ACK） */
    if (net_i2c_init() != NET_I2C_OK)
    {
        return MOD_EEPROM_ERR_IO;
    }
    if (mod_eeprom_probe() != MOD_EEPROM_OK)
    {
        s_chip = MOD_EEPROM_CHIP_AT24C02;   /* 器件不在：型号判断无意义 */
        return MOD_EEPROM_ERR_IO;
    }
    /* 器件在：再判型号（字地址位宽 + 页宽 + 容量都由它决定） */
    s_chip = detect_chip();
    return MOD_EEPROM_OK;
}

int mod_eeprom_probe(void)
{
    if (net_i2c_probe(AT24C02_ADDR) == NET_I2C_OK)
    {
        return MOD_EEPROM_OK;
    }
    return MOD_EEPROM_ERR_IO;
}

int mod_eeprom_probe_dev(uint8_t dev)
{
    if (net_i2c_probe(dev) == NET_I2C_OK)
    {
        return MOD_EEPROM_OK;
    }
    return MOD_EEPROM_ERR_IO;
}

int mod_eeprom_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    /* 越界与参数先卡死：不让非法访问穿透到总线层（容量随型号变） */
    if ((buf == NULL) || (len == 0U) ||
        (((uint32_t)addr + (uint32_t)len) > mod_eeprom_total_size()))
    {
        return MOD_EEPROM_ERR_PARAM;
    }

    /* 读没有页回绕问题：一包到底；字地址位宽随型号变 */
    if (net_i2c_read_ex(AT24C02_ADDR, addr, buf, len, mem_is_wide()) != NET_I2C_OK)
    {
        return MOD_EEPROM_ERR_IO;
    }
    return MOD_EEPROM_OK;
}

int mod_eeprom_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    uint16_t remain = len;

    if ((buf == NULL) || (len == 0U) ||
        (((uint32_t)addr + (uint32_t)len) > mod_eeprom_total_size()))
    {
        return MOD_EEPROM_ERR_PARAM;
    }

    /* ★ 页写自动拆分：一次写绝不越过页边界，逐页推进（上层无感拆页）
     *   页宽随型号变：AT24C02 = 8 B，AT24C256 = 64 B
     *   ★ 每一页都"写完就读回比对"（write_page_checked），不靠全块事后 memcmp */
    while (remain > 0U)
    {
        uint16_t page      = mod_eeprom_page_size();
        uint8_t  page_left = (uint8_t)(page - (addr % page));
        uint16_t n = ((uint16_t)remain < (uint16_t)page_left)
                         ? remain : (uint16_t)page_left;
        int rc = write_page_checked(addr, buf, n);

        if (rc != MOD_EEPROM_OK)
        {
            return rc;    /* IO = 没接好；TIMEOUT = 没写完；VERIFY = 读回对不上 */
        }

        addr   += n;      /* 逐页推进 */
        buf    += n;
        remain -= n;
    }
    return MOD_EEPROM_OK;
}

#endif /* MOD_EEPROM_ENABLE */
