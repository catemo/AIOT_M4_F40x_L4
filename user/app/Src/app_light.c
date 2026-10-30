/**
 ******************************************************************************
 * @file    app_light.c
 * @brief   应用层（app）—— 光敏传感器周期采集任务实现（2.8 新增）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_light.h，
 *  本文件的注释侧重"每一步为什么这么做"。
 *
 *  任务的时间账：
 *    每 500 ms 一轮，真正占用 CPU 的只有一次 ADC 转换（约 1 µs）
 *    和几十字节的拼行发送（115200 波特率下约 5 ms）。
 *    其余时间都在 osDelay 里把 CPU 让出去。
 *
 *  ▍打印为什么自拼字符串：
 *    本工程刻意不引入 printf 格式化库（会拉进几 KB FLASH）。
 *    这里只需要一种固定格式，手写三四十行拼接，换来的是一块实打实的余量。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_LIGHT_ENABLE

#include "app_light.h"
#include "per_light.h"      /* 感知层：PA5/ADC1_IN5 原始读数 */
#include "app_blink.h"      /* 同层：LED1 经闪烁状态机点灯（退出闪烁再点亮） */
#include "per_led.h"        /* 感知层：LED2 直接点灯 */
#include "mod_uart1.h"      /* 平台层：打印只认统一入口 */
#if APP_OLED_ENABLE         /* 显示链：刷新 Light/Level 两行（2.9）
                             * ★ include 跟开关走：关掉 OLED 链时 app_oled.c
                             *   不参与编译，调用点必须一起消失（否则链接期炸） */
#include "app_oled.h"
#endif
#include "cmsis_os.h"       /* osDelay */
#include <string.h>         /* memcpy（搬字面常量，不引入 printf） */

/* 打印行缓冲(字节)：最长一行约 62 字节，80 留余量。
 * 只在任务栈上用，不占静态 RAM。 */
#define APP_LIGHT_PRINT_BUF_SIZE   80U

/* ========== 最近一组读数的快照（给将来的上云任务读） ========== */
static APP_LIGHT_Snapshot_t s_snapshot;

/* ========== 4 点滑动平均的环形缓冲 ==========
 * 先平均再分级：单次 ADC 抖动（电源纹波、灯光频闪）可能让等级在阈值附近
 * 来回跳，灯就会闪。4 点平均把随机抖动压下去，代价是响应慢 1.5 个周期
 * （500 ms × 1.5 = 0.75 s），对光照这种缓变量完全可接受。
 * ★ 上电时 count 从 0 开始增长：前 3 轮用"已有几点算几点"，
 *   避免开头几轮因为补零而把平均值拉低。 */
static uint16_t s_filter_buf[APP_LIGHT_FILTER_SIZE];
static uint8_t  s_filter_idx   = 0U;
static uint8_t  s_filter_count = 0U;

/*----------------------------------------------------------------------------
 * 内部工具：拼字符串（与 app_dht11.c 同款，各层自备一份，不引公共库）
 *----------------------------------------------------------------------------*/
static uint16_t cat_lit(char *dst, uint16_t n, const char *lit, uint16_t len)
{
    (void)memcpy(&dst[n], lit, len);
    return (uint16_t)(n + len);
}

/* 写十进制无符号数（不补空格），返回新的写入位置 */
static uint16_t cat_u32(char *dst, uint16_t n, uint32_t v)
{
    char    tmp[10];
    uint8_t len = 0U;
    uint8_t i;

    if (v == 0U)
    {
        dst[n++] = '0';
        return n;
    }
    while ((v > 0U) && (len < 10U))
    {
        tmp[len] = (char)('0' + (uint8_t)(v % 10U));
        v /= 10U;
        len++;
    }
    for (i = len; i > 0U; i--)
    {
        dst[n++] = tmp[i - 1U];
    }
    return n;
}

/* 写十进制无符号数并左补空格到 4 位（对齐课件 %4u 的打印效果） */
static uint16_t cat_u32_pad4(char *dst, uint16_t n, uint32_t v)
{
    char    tmp[10];
    uint8_t len = 0U;
    uint8_t i;
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
    for (pad = len; pad < 4U; pad++)
    {
        dst[n++] = ' ';
    }
    for (i = len; i > 0U; i--)
    {
        dst[n++] = tmp[i - 1U];
    }
    return n;
}

/* 等级 → 固定 6 字符的文本（"DARK  " / "MEDIUM" / "BRIGHT"），列宽不跳 */
static const char *level_text(APP_LIGHT_Level_t level)
{
    switch (level)
    {
        case APP_LIGHT_LEVEL_DARK:   return "DARK  ";
        case APP_LIGHT_LEVEL_MEDIUM: return "MEDIUM";
        case APP_LIGHT_LEVEL_BRIGHT: return "BRIGHT";
        default:                     return "UNKNWN";
    }
}

/* 灯状态 → 固定 3 字符（"ON " / "OFF"）：读真实引脚电平，不缓存 */
static const char *led_text(uint8_t is_on)
{
    return (is_on != 0U) ? "ON " : "OFF";
}

/*----------------------------------------------------------------------------
 * 内部函数：4 点滑动平均
 * 参数    ：raw —— 本次原始 ADC 值
 * 返回值  ：平均后的 ADC 值
 *----------------------------------------------------------------------------*/
static uint16_t light_filter(uint16_t raw)
{
    uint32_t sum = 0U;
    uint8_t  i;

    s_filter_buf[s_filter_idx] = raw;
    s_filter_idx = (uint8_t)((s_filter_idx + 1U) % APP_LIGHT_FILTER_SIZE);
    if (s_filter_count < APP_LIGHT_FILTER_SIZE)
    {
        s_filter_count++;
    }

    for (i = 0U; i < s_filter_count; i++)
    {
        sum += s_filter_buf[i];
    }
    return (uint16_t)(sum / s_filter_count);
}

/*----------------------------------------------------------------------------
 * 内部函数：按 ADC 值判定光照等级
 * 注意事项：方向不能反：光越强 → 光敏阻值越小 → ADC 越小 → BRIGHT
 *----------------------------------------------------------------------------*/
static APP_LIGHT_Level_t light_classify(uint16_t adc)
{
    if (adc >= APP_LIGHT_TH_HIGH)
    {
        return APP_LIGHT_LEVEL_DARK;
    }
    if (adc >= APP_LIGHT_TH_LOW)
    {
        return APP_LIGHT_LEVEL_MEDIUM;
    }
    return APP_LIGHT_LEVEL_BRIGHT;
}

/*----------------------------------------------------------------------------
 * 内部函数：按等级控制 LED1 / LED2
 * 注意事项：LED1 必须走 APP_BLINK_On/Off（先退出闪烁再点灯），
 *           否则会被 1 Hz 闪烁状态机下一个节拍翻转掉。
 *----------------------------------------------------------------------------*/
static void light_control_led(APP_LIGHT_Level_t level)
{
    switch (level)
    {
        case APP_LIGHT_LEVEL_DARK:
            /* 暗光：双灯全亮（补光指示） */
            (void)APP_BLINK_On(APP_BLINK_LED1);
            (void)PER_LED_On(PER_LED2);
            break;

        case APP_LIGHT_LEVEL_MEDIUM:
            /* 中等光：仅 LED2 亮（过渡指示） */
            (void)APP_BLINK_Off(APP_BLINK_LED1);
            (void)PER_LED_On(PER_LED2);
            break;

        case APP_LIGHT_LEVEL_BRIGHT:
        default:
            /* 强光：双灯全灭（节能） */
            (void)APP_BLINK_Off(APP_BLINK_LED1);
            (void)PER_LED_Off(PER_LED2);
            break;
    }
}

/*----------------------------------------------------------------------------
 * 内部函数：拼一行采集结果
 * 格式（与课件 2.8 一致）：
 *   [Light] ADC=xxxx | xxxxmV | Level=BRIGHT | LED1=OFF | LED2=OFF
 *----------------------------------------------------------------------------*/
static uint16_t build_light_line(char *dst, uint16_t adc, uint32_t mv,
                                 APP_LIGHT_Level_t level)
{
    uint16_t n = 0U;

    n = cat_lit(dst, n, "[Light] ADC=", 12U);
    n = cat_u32_pad4(dst, n, adc);
    n = cat_lit(dst, n, " | ", 3U);
    n = cat_u32_pad4(dst, n, mv);
    n = cat_lit(dst, n, "mV | Level=", 11U);
    n = cat_lit(dst, n, level_text(level), 6U);
    n = cat_lit(dst, n, " | LED1=", 8U);
    n = cat_lit(dst, n, led_text(APP_BLINK_IsOn(APP_BLINK_LED1)), 3U);
    n = cat_lit(dst, n, " | LED2=", 8U);
    n = cat_lit(dst, n, led_text(PER_LED_IsOn(PER_LED2)), 3U);
    dst[n++] = '\r';
    dst[n++] = '\n';
    dst[n]   = '\0';
    return n;
}

/*----------------------------------------------------------------------------
 * 内部函数：拼一行生效阈值（改了 cfg 一眼就能在串口确认）
 * 格式： [Light] TH: BRIGHT<200 | MEDIUM 200..499 | DARK>=500
 *----------------------------------------------------------------------------*/
static uint16_t build_th_line(char *dst)
{
    uint16_t n = 0U;

    n = cat_lit(dst, n, "[Light] TH: BRIGHT<", 19U);
    n = cat_u32(dst, n, APP_LIGHT_TH_LOW);
    n = cat_lit(dst, n, " | MEDIUM ", 10U);
    n = cat_u32(dst, n, APP_LIGHT_TH_LOW);
    n = cat_lit(dst, n, "..", 2U);
    n = cat_u32(dst, n, (uint32_t)APP_LIGHT_TH_HIGH - 1U);
    n = cat_lit(dst, n, " | DARK>=", 9U);
    n = cat_u32(dst, n, APP_LIGHT_TH_HIGH);
    dst[n++] = '\r';
    dst[n++] = '\n';
    dst[n]   = '\0';
    return n;
}

/*----------------------------------------------------------------------------
 * 函数功能：光敏采集任务体（永不返回）
 * 调用处  ：app_task.c 的 app_taskCreate()   【接口契约见 app_light.h】
 *----------------------------------------------------------------------------*/
void app_light_task(void *argument)
{
    char              buf[APP_LIGHT_PRINT_BUF_SIZE];
    uint16_t          raw;
    uint16_t          adc;
    uint32_t          mv;
    APP_LIGHT_Level_t level;
    uint16_t          n;          /* 行缓冲长度：只做自检，打印仍走 SendString */

    (void)argument;

    /* ① 打招呼：串口一打开就能确认"这个任务起来了" */
    MOD_UART1_SendString("\r\n=== IOT_M4_L4 Light Sensor (PA5/ADC1_IN5) ===\r\n");

    /* ② 预热：ADC1 本体已由 main() 的 MX_ADC1_Init 初始化，
     *    这里只丢弃第一次转换，消除采样电容初始状态的影响 */
    PER_LIGHT_Init();

    /* ③ 把生效阈值打出来（数字来自 cfg_param.h，注释里不写死） */
    n = build_th_line(buf);
    MOD_UART1_SendString(buf);

    for (;;)
    {
        /* ⓵ 采集 + 4 点平均：先平均再分级，避免阈值附近灯来回闪 */
        raw = PER_LIGHT_ReadRaw();
        adc = light_filter(raw);

        /* ⓶ 换算电压（整数 mV，避免浮点库） */
        mv = ((uint32_t)adc * PER_LIGHT_VREF_MV) / PER_LIGHT_ADC_MAX;

        /* ⓷ 分级 → 控灯 */
        level = light_classify(adc);
        light_control_led(level);

        /* ⓸ 更新快照（供将来上云使用；本册只打印与控灯） */
        s_snapshot.adc   = adc;
        s_snapshot.mv    = (uint16_t)mv;
        s_snapshot.level = (uint8_t)level;
        s_snapshot.valid = 1U;

        /* ⓹ 打印一行：ADC / mV / 等级 / 两颗灯的真实状态 */
        n = build_light_line(buf, adc, mv, level);
        MOD_UART1_SendString(buf);   (void)n;   /* 长度只做自检，发送交给 SendString */

        /* ⓹' 显示：同一次分级结果刷到 OLED 后两行（2.9）。
         *     ★ 与控灯同拍：屏上 Level 与两颗灯的实际状态永远一致。
         *     ★ level_text() 返回定宽 6 字符（"DARK  "/"MEDIUM"/"BRIGHT"），
         *       直接交给显示层，列宽就不会跳。 */
#if APP_OLED_ENABLE
        APP_OLED_UpdateLight(adc, level_text(level));
#endif

        /* ⓺ 节拍：500 ms 一轮。必须用 osDelay，业务节拍一律让出 CPU。 */
        osDelay(APP_LIGHT_PERIOD_MS);
    }
}

/*----------------------------------------------------------------------------
 * 【接口契约见 app_light.h】
 *----------------------------------------------------------------------------*/
void APP_LIGHT_GetSnapshot(APP_LIGHT_Snapshot_t *out)
{
    if (out == NULL)
    {
        return;
    }
    *out = s_snapshot;   /* 结构体整体拷贝：4 个字段，读写都是对齐的 */
}

#endif /* APP_LIGHT_ENABLE */
