/**
 ******************************************************************************
 * @file    app_param.c
 * @brief   应用层（app）—— 参数配置块实现 + 存储任务体（2.3 外部存储器新增）
 *
 *  接口的完整说明写在 app_param.h。本文件的注释侧重"加载五步与寿命保护"。
 *
 *  ★ 任务归属说明：app_storage_task 同时驱动参数链（3 s 读回）与日志链
 *    （5 s 追加），放在哪边都不完全对 —— 放在先建的 param 侧，
 *    logger 只提供 Append/Periodic；两边都关时本文件整体退出编译。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_PARAM_ENABLE || APP_LOGGER_ENABLE

#include "app_param.h"
#include "mod_uart1.h"
#include "cmsis_os.h"
#include <stddef.h>   /* offsetof：CRC 覆盖范围自动推导 */
#include <string.h>

#if APP_LOGGER_ENABLE
#include "app_logger.h"
#endif
#if APP_PARAM_ENABLE
#include "mod_eeprom.h"

/* ===================== 参数链内部状态 ===================== */

static app_param_cfg_t s_cfg;      /* RAM 副本：业务只读它，不碰总线 */
static uint8_t         s_valid = 0U;   /* 1 = 配置块已从芯片加载成功 */

/* CRC 覆盖范围自动推导：crc16 字段之后的全部正文 —— 加字段不用手改偏移 */
#define APP_PARAM_CRC_BODY_OFF  ((uint32_t)offsetof(app_param_cfg_t, temp_offset_x10))
#define APP_PARAM_CRC_BODY_LEN  ((uint32_t)(sizeof(app_param_cfg_t) - APP_PARAM_CRC_BODY_OFF))

/* ===================== 打印小工具（本工程不引 printf，自己拼） ===================== */

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

static void out_hex16(uint16_t v)
{
    static const char H[] = "0123456789ABCDEF";
    char b[5];

    b[0] = H[(v >> 12) & 0xFU];
    b[1] = H[(v >> 8)  & 0xFU];
    b[2] = H[(v >> 4)  & 0xFU];
    b[3] = H[v & 0xFU];
    b[4] = '\0';
    out_str(b);
}

static void out_hex8(uint8_t v)
{
    static const char H[] = "0123456789ABCDEF";
    char b[3];

    b[0] = H[(v >> 4) & 0xFU];
    b[1] = H[v & 0xFU];
    b[2] = '\0';
    out_str(b);
}

/* 有符号返回码打印（rc 只有 -1..-5，取负安全） */
static void out_rc(int rc)
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

/* ===================== CRC16-CCITT（逐位移位，省 512 B 查表 ROM） ===================== */

static uint16_t crc16_ccitt(const uint8_t *p, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint8_t  k;

    for (i = 0U; i < len; i++)
    {
        crc ^= (uint16_t)((uint16_t)(*p++) << 8);
        for (k = 0U; k < 8U; k++)
        {
            crc = ((crc & 0x8000u) != 0u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                          : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void cfg_refresh_crc(app_param_cfg_t *c)
{
    c->crc16 = crc16_ccitt((const uint8_t *)c + APP_PARAM_CRC_BODY_OFF,
                           APP_PARAM_CRC_BODY_LEN);
}

/* 三关校验：魔数 → 版本 → CRC，一关不过全弃用 */
static uint8_t cfg_is_valid(const app_param_cfg_t *c)
{
    if (c->magic != APP_PARAM_MAGIC)        { return 0U; }  /* 第一关 */
    if (c->version != APP_PARAM_CFG_VERSION){ return 0U; }  /* 第二关 */
    if (c->crc16 != crc16_ccitt((const uint8_t *)c + APP_PARAM_CRC_BODY_OFF,
                                APP_PARAM_CRC_BODY_LEN))    { return 0U; }  /* 第三关 */
    return 1U;
}

static void cfg_fill_default(app_param_cfg_t *c)
{
    (void)memset(c, 0, sizeof(*c));
    c->magic            = APP_PARAM_MAGIC;
    c->version          = APP_PARAM_CFG_VERSION;
    c->temp_offset_x10  = 0;
    c->temp_threshold   = (uint8_t)APP_LINK_TEMP_THRESHOLD;    /* 31（cfg 统一口径） */
    c->humi_threshold   = (uint8_t)APP_LINK_HUMI_THRESHOLD;    /* 60                 */
    c->report_period_ms = (uint32_t)APP_MQTT_PUBLISH_PERIOD_MS;
    c->reboot_count     = 0U;
    cfg_refresh_crc(c);
}

/* 写整块 + 读回确认（写后必须能读出来，两处写路径共用）
 * ★ 失败时必须说出"第几页"：全块 memcmp 只能说"有字节不对"，
 *   报出首个不一致字节落在第几页，才能一眼看出是哪一页没写进去 */
static int cfg_write_and_check(void)
{
    uint8_t   rb[APP_PARAM_CFG_SIZE];
    uint16_t  i;
    int       rc;

    rc = mod_eeprom_write(APP_PARAM_CFG_OFFSET, (const uint8_t *)&s_cfg,
                          APP_PARAM_CFG_SIZE);
    if (rc != MOD_EEPROM_OK)
    {
        out_str("[PARAM] write FAIL rc=");
        out_rc(rc);
        out_str(" (-2=IO, -3=TIMEOUT, -4=VERIFY: a page did not store)\r\n");
        return APP_PARAM_ERR_STORAGE;
    }

    if (mod_eeprom_read(APP_PARAM_CFG_OFFSET, rb, APP_PARAM_CFG_SIZE) != MOD_EEPROM_OK)
    {
        out_str("[PARAM] write-verify READ FAIL\r\n");
        return APP_PARAM_ERR_STORAGE;
    }

    if (memcmp(rb, &s_cfg, APP_PARAM_CFG_SIZE) != 0)
    {
        uint16_t first_bad = 0U;

        for (i = 0U; i < APP_PARAM_CFG_SIZE; i++)
        {
            if (rb[i] != ((const uint8_t *)&s_cfg)[i])
            {
                first_bad = i;
                break;
            }
        }
        out_str("[PARAM] verify FAIL: first bad byte ");
        out_u32(first_bad);
        out_str(" = page ");
        out_u32(first_bad / mod_eeprom_page_size());
        out_str("\r\n");
        return APP_PARAM_ERR_VERIFY;
    }
    return APP_PARAM_OK;
}

/* 打印一行配置摘要：ver/crc | reboot th period */
static void print_cfg_line(const char *head)
{
    out_str(head);
    out_str(" ver=");
    out_u32(s_cfg.version);
    out_str(" crc=0x");
    out_hex16(s_cfg.crc16);
    out_str(" | reboot=");
    out_u32(s_cfg.reboot_count);
    out_str(" th=");
    out_u32(s_cfg.temp_threshold);
    out_str("C/");
    out_u32(s_cfg.humi_threshold);
    out_str("% period=");
    out_u32(s_cfg.report_period_ms);
    out_str("ms\r\n");
}

/* ===================== 对外接口 ===================== */

int APP_PARAM_Inspect(void)
{
    uint8_t  raw[32];
    uint16_t i;
    int      rc = mod_eeprom_read(0U, raw, (uint16_t)sizeof(raw));

    if (rc != MOD_EEPROM_OK)
    {
        out_str("[PARAM] EEPROM read FAILED rc=");
        out_rc(rc);
        out_str(" (-2=IO/NACK: check PB8/PB9 + pull-up + WP)\r\n");
        return APP_PARAM_ERR_STORAGE;
    }

    out_str("[PARAM] EEPROM[00..1F] =");
    for (i = 0U; i < sizeof(raw); i++)
    {
        out_str(" ");
        out_hex8(raw[i]);
    }
    out_str("\r\n");

    {
        app_param_cfg_t probe;

        (void)memcpy(&probe, raw, sizeof(probe));
        out_str("[PARAM] -> magic=0x");
        out_hex16((uint16_t)(probe.magic >> 16));
        out_hex16((uint16_t)(probe.magic & 0xFFFFu));
        out_str(" ver=");
        out_u32(probe.version);
        out_str(" crc=0x");
        out_hex16(probe.crc16);

        if (cfg_is_valid(&probe) == 0U)
        {
            out_str(" : INVALID (raw 0xFF = blank chip)\r\n");
            return APP_PARAM_ERR_VERIFY;
        }
        out_str(" : VALID  reboot=");
        out_u32(probe.reboot_count);
        out_str(" th=");
        out_u32(probe.temp_threshold);
        out_str("C/");
        out_u32(probe.humi_threshold);
        out_str("%\r\n");
    }
    return APP_PARAM_OK;
}

void APP_PARAM_GetThresholds(uint8_t *temp_th, uint8_t *humi_th)
{
    if (temp_th != NULL)
    {
        *temp_th = (s_valid != 0U) ? s_cfg.temp_threshold
                                   : (uint8_t)APP_LINK_TEMP_THRESHOLD;
    }
    if (humi_th != NULL)
    {
        *humi_th = (s_valid != 0U) ? s_cfg.humi_threshold
                                   : (uint8_t)APP_LINK_HUMI_THRESHOLD;
    }
}

int APP_PARAM_SetThresholds(uint8_t temp_th, uint8_t humi_th)
{
    /* 范围与 DHT11 量程一致（cfg_check 对宏值体检，运行值在这里体检） */
    if ((temp_th > 50U) || (humi_th < 20U) || (humi_th > 90U))
    {
        return APP_PARAM_ERR_PARAM;
    }

    s_cfg.temp_threshold = temp_th;
    s_cfg.humi_threshold = humi_th;
    cfg_refresh_crc(&s_cfg);

    {
        int rc = cfg_write_and_check();
        if (rc != APP_PARAM_OK)
        {
            return rc;   /* 写失败：RAM 副本同步回滚由上层决定（本例保持新值不回滚） */
        }
    }
    s_valid = 1U;
    return APP_PARAM_OK;
}

int APP_PARAM_ResetDefault(void)
{
    cfg_fill_default(&s_cfg);

    if (mod_eeprom_write(APP_PARAM_CFG_OFFSET, (const uint8_t *)&s_cfg,
                         APP_PARAM_CFG_SIZE) != MOD_EEPROM_OK)
    {
        return APP_PARAM_ERR_STORAGE;
    }
    s_valid = 1U;
    return APP_PARAM_OK;
}

int APP_PARAM_Init(void)
{
    /* ① 探测器件：不在也继续跑，用默认值（不白耗寿命去写） */
    if (mod_eeprom_init() != MOD_EEPROM_OK)
    {
        out_str("[PARAM] probe 0xA0 ... NACK -> use DEFAULT (check wiring/pull-up)\r\n");
        cfg_fill_default(&s_cfg);
        s_valid = 0U;
        return APP_PARAM_ERR_STORAGE;
    }
    out_str("[PARAM] probe 0xA0 ... ACK\r\n");
    /* 型号与几何参数打出来：字地址位宽/页宽/容量搞错时，这一行是第一现场 */
    out_str("[PARAM] EEPROM chip = ");
    out_str(mod_eeprom_chip_name());
    out_str(" page=");
    out_u32(mod_eeprom_page_size());
    out_str("B size=");
    out_u32(mod_eeprom_total_size());
    out_str("B\r\n");

    /* ② 读配置块：读的是芯片，不是 RAM */
    if (mod_eeprom_read(APP_PARAM_CFG_OFFSET, (uint8_t *)&s_cfg,
                        APP_PARAM_CFG_SIZE) != MOD_EEPROM_OK)
    {
        out_str("[PARAM] read FAILED -> use DEFAULT\r\n");
        cfg_fill_default(&s_cfg);
        s_valid = 0U;
        return APP_PARAM_ERR_STORAGE;
    }

    /* ③ 三关校验 */
    if (cfg_is_valid(&s_cfg) == 0U)
    {
        /* ④ 不过 → 写默认值并回读确认 */
        out_str("[PARAM] cfg INVALID (magic/ver/crc) -> write DEFAULT\r\n");
        cfg_fill_default(&s_cfg);
        {
            int rc = cfg_write_and_check();
            if (rc != APP_PARAM_OK)
            {
                out_str("[PARAM] write DEFAULT ... FAIL\r\n");
                s_valid = 0U;
                return APP_PARAM_ERR_STORAGE;
            }
        }
        s_valid = 1U;
        /* 首次写默认后直接走加载完的打印（reboot=1） */
    }
    else
    {
        out_str("[PARAM] loaded from storage (magic/ver/crc OK)\r\n");
        s_valid = 1U;
    }

    /* ⑤ 重启计数 +1 写回 —— 掉电保持最直观的证据（一次启动一次写，寿命可控） */
    s_cfg.reboot_count++;
    {
        int rc = cfg_write_and_check();
        if (rc != APP_PARAM_OK)
        {
            out_str("[PARAM] WARN: reboot count save FAILED\r\n");
            /* 计数写回失败不判死刑：配置本体仍是有效的 */
        }
    }

    print_cfg_line("[PARAM]");
    return APP_PARAM_OK;
}

#else /* !APP_PARAM_ENABLE */
/* 参数链关掉时本段没有内容：日志链仍靠下方的任务体跑起来 */
typedef int app_param_unused_t;   /* 防止空翻译单元告警 */
#endif /* APP_PARAM_ENABLE */

#if APP_LOGGER_ENABLE
#include "app_logger.h"

static uint8_t s_log_ready = 0U;   /* 1 = W25Q64 在线、日志链可用 */
#endif

/* ===================== 存储任务体（一个任务驱动两条链） ===================== */

void app_storage_task(void *argument)
{
    uint32_t tick = 0U;
    (void)argument;

    out_str("\r\n=== IOT_M4_L4 Storage (AT24C02 + W25Q64) ===\r\n");

#if APP_PARAM_ENABLE
    (void)APP_PARAM_Init();     /* 加载五步走（全过程已打印，失败用默认值） */
#endif

#if APP_LOGGER_ENABLE
    if (APP_LOGGER_Init() == APP_LOGGER_OK)
    {
        s_log_ready = 1U;
    }
    /* 器件不在：Init 内部已给排查提示，日志链自降级，其余功能照常 */
#endif

    for (;;)
    {
        osDelay(APP_STORAGE_TICK_MS);
        tick++;

#if APP_PARAM_ENABLE
        if ((tick % (APP_PARAM_READBACK_MS / APP_STORAGE_TICK_MS)) == 0U)
        {
            /* ★ 只读不写（寿命保护）：从芯片重读 + 三关校验 + 打一行
             *   ★ 分诊：读【失败】与读【到坏数据】是两回事，必须分开报：
             *     读失败 → 查总线（PB8/PB9、上拉、WP）；坏数据 → 查页拆分/写周期 */
            app_param_cfg_t rb;
            int rc = mod_eeprom_read(APP_PARAM_CFG_OFFSET, (uint8_t *)&rb,
                                     APP_PARAM_CFG_SIZE);

            if (rc != MOD_EEPROM_OK)
            {
                out_str("[PARAM] readback READ FAIL rc=");
                out_rc(rc);
                out_str(" (-2=IO/NACK, -1=param)\r\n");
            }
            else if (cfg_is_valid(&rb) == 0U)
            {
                out_str("[PARAM] readback INVALID magic=0x");
                out_hex16((uint16_t)(rb.magic >> 16));
                out_hex16((uint16_t)(rb.magic & 0xFFFFu));
                out_str(" ver=");
                out_u32(rb.version);
                out_str(" (blank chip = 0xFF..)\r\n");
            }
            else
            {
                (void)memcpy(&s_cfg, &rb, sizeof(rb));   /* 与芯片保持同步 */
                print_cfg_line("[PARAM] readback");
            }
        }
#endif

#if APP_LOGGER_ENABLE
        if ((s_log_ready != 0U) &&
            ((tick % (APP_LOGGER_PERIOD_MS / APP_STORAGE_TICK_MS)) == 0U))
        {
            APP_LOGGER_Periodic();   /* 取 DHT11 快照 → 追加一条 → 读回比对 → 打印 */
        }
#endif
    }
}

#endif /* APP_PARAM_ENABLE || APP_LOGGER_ENABLE */
