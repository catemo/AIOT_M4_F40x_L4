/**
 ******************************************************************************
 * @file    app_oled.c
 * @brief   应用层（app）—— OLED 显示业务实现（2.9 新增）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_oled.h，
 *  本文件的注释侧重"为什么这样拼字符串、为什么这样加锁"。
 *
 *  ▍本文件的三个关键词：
 *    定宽   —— 每个数字字段都占固定宽度，位数变化不残留旧字符
 *    一把锁 —— 两个任务写同一块屏，所有写操作串行化
 *    不 printf —— 数字转字符串手写，不把格式化库拉进固件
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_OLED_ENABLE

#include "app_oled.h"
#include "mod_oled.h"    /* 平台层：页 / 列 / 6x8 / 8x16 显示接口 */
#include "main.h"        /* HAL_Delay（初始化自检的 500 ms） */
#include "cmsis_os.h"    /* osMutexNew / osMutexAcquire / osMutexRelease */

/* ========== 四行布局（8x16 每字符占 2 页，起始页 0/2/4/6） ========== */
#define APP_OLED_LINE_TEMP   0U   /* 页 0-1：温度 */
#define APP_OLED_LINE_HUM    2U   /* 页 2-3：湿度 */
#define APP_OLED_LINE_LIGHT  4U   /* 页 4-5：光照 ADC */
#define APP_OLED_LINE_LEVEL  6U   /* 页 6-7：光照等级 */

/* 行缓冲：最长 13 字符，24 字节留足余量（只在调用者栈上用） */
#define APP_OLED_LINE_BUF    24U

/* ========== 互斥锁（全局唯一，保护所有写屏操作） ========== */
static osMutexId_t s_oled_mutex = NULL;

/* ========== 内部工具：拼定宽字符串 ========== */

static uint16_t put_lit(char *dst, uint16_t n, const char *lit)
{
    while ((lit != NULL) && (*lit != '\0'))
    {
        dst[n++] = *lit++;
    }
    return n;
}

/* 写无符号数并左补空格到 width 位（右对齐）：
 * 例：v=9、width=2 → " 9"；v=3027、width=4 → "3027" */
static uint16_t put_u32_w(char *dst, uint16_t n, uint32_t v, uint8_t width)
{
    char    tmp[10];
    uint8_t len = 0U;
    uint8_t pad;

    if (v == 0U)
    {
        tmp[len++] = '0';
    }
    while ((v > 0U) && (len < 10U))
    {
        tmp[len] = (char)('0' + (uint8_t)(v % 10U));
        v /= 10U;
        len++;
    }
    for (pad = len; pad < width; pad++)
    {
        dst[n++] = ' ';
    }
    while (len > 0U)
    {
        dst[n++] = tmp[--len];
    }
    return n;
}

/* 写等级文本并右补空格到 6 字符（"DARK  " / "MEDIUM" / "BRIGHT"）：
 * 长度固定，刷完这一行不会留下上一帧的尾巴 */
static uint16_t put_level6(char *dst, uint16_t n, const char *level)
{
    uint8_t i = 0U;

    if (level == NULL)
    {
        level = "------";
    }
    while ((level[i] != '\0') && (i < 6U))
    {
        dst[n++] = level[i];
        i++;
    }
    while (i < 6U)
    {
        dst[n++] = ' ';
        i++;
    }
    return n;
}

/* ========== 锁 ========== */

void APP_OLED_CreateMutex(void)
{
    s_oled_mutex = osMutexNew(NULL);
    /* 创建失败不停机：后续写操作见 NULL 直接放行，只是失去并发保护 */
}

/* 拿锁：成功返回 1；超时/锁未创建见下 */
static uint8_t oled_lock(void)
{
    if (s_oled_mutex == NULL)
    {
        return 1U;   /* 锁没建起来：直接放行（功能优先于保护） */
    }
    return (osMutexAcquire(s_oled_mutex, APP_OLED_LOCK_TIMEOUT_MS) == osOK) ? 1U : 0U;
}

static void oled_unlock(void)
{
    if (s_oled_mutex != NULL)
    {
        (void)osMutexRelease(s_oled_mutex);
    }
}

/* ========== 对外接口 ========== */

void APP_OLED_Init(void)
{
    /* ① 器件初始化（含清屏）：电荷泵、页寻址、对比度、扫描方向 */
    MOD_OLED_Init();

    /* ② 全屏点亮 500 ms：一次自检三件事——I2C 通、供电足、像素全好。
     *    屏只亮一部分 → 查对比度 / VCOM / 预充电；完全不亮 → 查供电与接线 */
    MOD_OLED_Fill(0xFFU);
    HAL_Delay(500U);
    MOD_OLED_Clear();

    /* ③ 静态标签：只写这一次，后面的周期刷新只覆盖数值部分。
     *    所有占位符都是"定宽"的，与真实数据的宽度一致，不会先宽后窄。 */
    if (oled_lock() == 0U)
    {
        return;
    }
    MOD_OLED_ShowString8x16(APP_OLED_LINE_TEMP,  0U, "Temp: --.- C");
    MOD_OLED_ShowString8x16(APP_OLED_LINE_HUM,   0U, "Hum:  --.- %");
    MOD_OLED_ShowString8x16(APP_OLED_LINE_LIGHT, 0U, "Light: ----");
    MOD_OLED_ShowString8x16(APP_OLED_LINE_LEVEL, 0U, "Level: ------");
    oled_unlock();
}

void APP_OLED_UpdateTempHum(uint8_t temp_int, uint8_t temp_dec,
                            uint8_t humi_int, uint8_t humi_dec)
{
    char     buf[APP_OLED_LINE_BUF];
    uint16_t n;

    if (oled_lock() == 0U)
    {
        return;   /* 100 ms 内拿不到锁：放弃本帧，下个周期再来 */
    }

    /* Temp: xx.x C（12 字符） */
    n = put_lit(buf, 0U, "Temp: ");
    n = put_u32_w(buf, n, temp_int, 2U);
    buf[n++] = '.';
    n = put_u32_w(buf, n, temp_dec, 1U);
    n = put_lit(buf, n, " C");
    buf[n] = '\0';
    MOD_OLED_ShowString8x16(APP_OLED_LINE_TEMP, 0U, buf);

    /* Hum:  xx.x %（12 字符） */
    n = put_lit(buf, 0U, "Hum:  ");
    n = put_u32_w(buf, n, humi_int, 2U);
    buf[n++] = '.';
    n = put_u32_w(buf, n, humi_dec, 1U);
    n = put_lit(buf, n, " %");
    buf[n] = '\0';
    MOD_OLED_ShowString8x16(APP_OLED_LINE_HUM, 0U, buf);

    oled_unlock();
}

void APP_OLED_UpdateLight(uint16_t adc, const char *level_str)
{
    char     buf[APP_OLED_LINE_BUF];
    uint16_t n;

    if (oled_lock() == 0U)
    {
        return;
    }

    /* Light: xxxx（11 字符，4 位右对齐） */
    n = put_lit(buf, 0U, "Light: ");
    n = put_u32_w(buf, n, adc, 4U);
    buf[n] = '\0';
    MOD_OLED_ShowString8x16(APP_OLED_LINE_LIGHT, 0U, buf);

    /* Level: xxxxxx（13 字符，等级固定 6 字符） */
    n = put_lit(buf, 0U, "Level: ");
    n = put_level6(buf, n, level_str);
    buf[n] = '\0';
    MOD_OLED_ShowString8x16(APP_OLED_LINE_LEVEL, 0U, buf);

    oled_unlock();
}

void APP_OLED_ShowError(void)
{
    if (oled_lock() == 0U)
    {
        return;
    }

    /* 只覆盖温湿度两行：光敏两行照旧，故障隔离在"行"这一级 */
    MOD_OLED_ShowString8x16(APP_OLED_LINE_TEMP, 0U, "Temp: ERROR");
    MOD_OLED_ShowString8x16(APP_OLED_LINE_HUM,  0U, "Hum:  ERROR");

    oled_unlock();
}

#endif /* APP_OLED_ENABLE */
