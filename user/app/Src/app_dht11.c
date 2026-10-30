/**
 ******************************************************************************
 * @file    app_dht11.c
 * @brief   应用层（app）—— DHT11 温湿度周期采集任务实现
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 app_dht11.h，
 *  本文件的注释侧重"每一步为什么这么做"。
 *
 *  任务的时间账（本册最需要算清楚的一笔）：
 *    每 2 秒一轮，其中真正占用 CPU 的只有
 *      40 位读取约 4~5 ms（关中断，忙等——必须连续占用）
 *      = 约 5 ms
 *    起始信号的 18 ms 已改走 osDelay（引脚电平由 ODR 硬件保持，
 *    CPU 可以睡，见 per_dht11.c 的 dht11_start_low_wait），
 *    占 2 秒周期的 0.25%，其余时间都在 osDelay 里把 CPU 让出去。
 *
 *  ★ 失败路径刻意什么都不做：
 *    读失败 → 不调 app_link → 灯保持上一次的状态。
 *    "没有输入就没有动作"，绝不会因为读不到数就把灯乱闪一通。
 *    这条让"传感器没接"和"联动逻辑写错"在现象上完全不同，好排查。
 ******************************************************************************
 */
#include "module_cfg.h"
#if APP_DHT11_ENABLE

#include "app_dht11.h"
#include "per_dht11.h"      /* 感知层：PG9 单总线（40 位 → 4 字节） */
#if APP_LINK_ENABLE         /* 同层：阈值联动（温度 → LED1，湿度 → LED2）
                             * ★ 必须连 include 一起守卫：联动被裁掉时，
                             *   它的三个函数不存在，留着调用点就是链接期
                             *   undefined reference（编译期看不出来，
                             *   因为声明在头文件里、错误要到链接才爆）。 */
#include "app_link.h"
#endif
#include "mod_uart1.h"      /* 平台层：串口1 统一入口（打印只认它） */
#if APP_OLED_ENABLE         /* 平台层之上的显示链：刷新 Temp/Hum 两行（2.9）
                             * ★ include 也要跟着开关走：关掉 OLED 链时
                             *   app_oled.c 不参与编译，调用点必须一起消失 */
#include "app_oled.h"
#endif
#include "cmsis_os.h"       /* osDelay */
#include <string.h>         /* memcpy（只用它搬字面常量，不引入 printf） */

/* ========== 最近一组读数的快照（给上云任务读） ==========
 * ★ 上云任务每 5 秒要一组温湿度，但传感器 2 秒才采一次，而且不能催。
 *   所以采集任务顺手把结果放在这里，上报任务只读——两边互不等待。
 * ★ static 上电清零：valid = 0 天然表示"还没采到过"，不需要额外初始化代码。 */
static APP_DHT11_Snapshot_t s_snapshot;

/* ========== 内部工具：不引入 printf，自己拼字符串 ==========
 * 为什么不 snprintf：格式化库会拉进几 KB 的 FLASH，而本册只需要
 * 拼三种固定格式的短句。手写这三四十行的代价，换来的是
 * "不需要 -u _printf_float、不用管 newlib 的栈开销"——
 * 对 512 KB FLASH 的教学工程，这几 KB 是实打实的余量。 */

/*----------------------------------------------------------------------------
 * 内部函数：把无符号整数写成十进制字符串
 * 返回值  ：写入的字符数（1~10）
 * 注意事项：① 先倒着存进临时数组再正着输出——不用递归、不用堆、栈开销固定
 *           ② 0 单独处理：否则 while(v>0) 一次都不进，会写出空字符串
 *----------------------------------------------------------------------------*/
static uint8_t uint_to_dec(char *dst, uint32_t v)
{
    char   tmp[10];
    uint8_t len = 0U;
    uint8_t i;

    if (v == 0U)
    {
        dst[0] = '0';
        return 1U;
    }

    while ((v > 0U) && (len < 10U))
    {
        tmp[len] = (char)('0' + (uint8_t)(v % 10U));
        v /= 10U;
        len++;
    }

    for (i = 0U; i < len; i++)
    {
        dst[i] = tmp[len - 1U - i];
    }
    return len;
}

/*----------------------------------------------------------------------------
 * 内部函数：把一段字面常量追加到缓冲尾部
 * 注意事项：调用方传 sizeof(lit) - 1 作为长度——减 1 是为了去掉结尾的 '\0'，
 *           而 sizeof 是编译期常量，不占运行时的 strlen 开销
 *----------------------------------------------------------------------------*/
static uint16_t cat_lit(char *dst, uint16_t n, const char *lit, uint16_t len)
{
    (void)memcpy(&dst[n], lit, len);
    return (uint16_t)(n + len);
}

/*----------------------------------------------------------------------------
 * 内部函数：拼一行温湿度 —— "[DHT11] Temp=29.4C | Hum=35.0%\r\n"
 * 注意事项：小数位只取 1 位（v % 10）：DHT11 的小数部分恒为 0，
 *           照搬协议字段是为了格式对齐，不是为了假装它有 0.1 的分辨率
 *----------------------------------------------------------------------------*/
static uint16_t build_th_line(char *dst, uint8_t t_int, uint8_t t_dec,
                              uint8_t h_int, uint8_t h_dec)
{
    uint16_t n = 0U;

    n = cat_lit(dst, n, "[DHT11] Temp=", 13U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], t_int));
    dst[n++] = '.';
    n = (uint16_t)(n + uint_to_dec(&dst[n], (uint8_t)(t_dec % 10U)));
    n = cat_lit(dst, n, "C | Hum=", 8U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], h_int));
    dst[n++] = '.';
    n = (uint16_t)(n + uint_to_dec(&dst[n], (uint8_t)(h_dec % 10U)));
    dst[n++] = '%';
    dst[n++] = '\r';
    dst[n++] = '\n';
    return n;
}

/*----------------------------------------------------------------------------
 * 内部函数：拼一行失败 —— "[DHT11] Read FAILED (stage=1)\r\n"
 * 注意事项：★ stage 是本册最有价值的一个字段：
 *           它把"读不到数"这个笼统现象拆成 6 种具体原因，
 *           看到 1 去查接线供电、看到 6 去查电气质量，不用从头猜
 *----------------------------------------------------------------------------*/
static uint16_t build_fail_line(char *dst, uint8_t stage)
{
    uint16_t n = 0U;

    n = cat_lit(dst, n, "[DHT11] Read FAILED (stage=", 27U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], stage));
    dst[n++] = ')';
    dst[n++] = '\r';
    dst[n++] = '\n';
    return n;
}

/*----------------------------------------------------------------------------
 * 内部函数：拼一行自检 —— "[DHT11] ST: TIM2=1000us loop=1,0 bus=1 resp=289"
 * 注意事项：四项各自回答一个问题（详见 per_dht11.h 的自检说明）：
 *   TIM2  ≈1000  → 否则 µs 计时全错，DHT11 必然读不到
 *   loop  = 1,0  → 否则 PG9 引脚不通
 *   bus   = 1    → 0 = 被外部拉低（缺上拉 / 短路）
 *   resp  = 0    → 传感器完全没应答；>0 = 有应答但时序不对
 *----------------------------------------------------------------------------*/
static uint16_t build_diag_line(char *dst, const PER_DHT11_Diag_t *diag)
{
    uint16_t n = 0U;

    n = cat_lit(dst, n, "[DHT11] ST: TIM2=", 17U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], diag->tim2_delta_us));
    n = cat_lit(dst, n, "us loop=", 8U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], diag->loopback_high));
    dst[n++] = ',';
    n = (uint16_t)(n + uint_to_dec(&dst[n], diag->loopback_low));
    n = cat_lit(dst, n, " bus=", 5U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], diag->idle_bus));
    n = cat_lit(dst, n, " resp=", 6U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], diag->resp_low_samples));
    dst[n++] = '\r';
    dst[n++] = '\n';
    return n;
}

/*----------------------------------------------------------------------------
 * 内部函数：拼一行阈值配置 —— "[DHT11] Link: Temp>=<温度阈值>C -> LED1 | Hum=<湿度阈值>% -> LED2"
 *           （数字来自 cfg_param.h 的两个宏，改 cfg 这一行就跟着变，注释里不写死）
 * 注意事项：上电把生效的阈值打出来，是为了避免"改了 cfg 却以为没生效"——
 *           串口看到的数字，就是真正在起作用的那两个数
 *----------------------------------------------------------------------------*/
static uint16_t build_cfg_line(char *dst)
{
    uint16_t n = 0U;

    n = cat_lit(dst, n, "[DHT11] Link: Temp>=", 20U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], APP_LINK_TEMP_THRESHOLD));
    n = cat_lit(dst, n, "C -> LED1 | Hum>=", 17U);
    n = (uint16_t)(n + uint_to_dec(&dst[n], APP_LINK_HUMI_THRESHOLD));
    n = cat_lit(dst, n, "% -> LED2", 9U);
    dst[n++] = '\r';
    dst[n++] = '\n';
    return n;
}

/*----------------------------------------------------------------------------
 * 函数功能：DHT11 温湿度采集任务体（永不返回）
 * 调用处  ：app_task.c 的 app_taskCreate()   【接口契约见 app_dht11.h】
 *----------------------------------------------------------------------------*/
void app_dht11_task(void *argument)
{
    PER_DHT11_Data_t  data;
    PER_DHT11_Diag_t  diag;
    char              buf[APP_DHT11_PRINT_BUF_SIZE];
    uint8_t           retry;
    uint8_t           ok;
    uint16_t          n;

    (void)argument;

    /* ① 打招呼：串口一打开就能确认"这个任务起来了" */
    MOD_UART1_SendString("\r\n=== IOT_M4_L4 DHT11 Temp/Humi (PG9) ===\r\n");

    /* ② 传感器初始化 + 上电自检（只在启动时跑一次，约 25 ms） */
    PER_DHT11_Init();
    PER_DHT11_SelfTest(&diag);
    n = build_diag_line(buf, &diag);
    buf[n] = '\0';
    MOD_UART1_SendString(buf);

    /* ③ 联动初始化：确保两颗灯的引脚已配好、且进入确定的熄灭状态 */
#if APP_LINK_ENABLE
    APP_LINK_Init();
#endif

    /* ④ 把生效的阈值打出来：改了 cfg 一眼就能在串口上确认 */
    n = build_cfg_line(buf);
    buf[n] = '\0';
    MOD_UART1_SendString(buf);

    /* ⑤ 上电稳定期：DHT11 上电后需约 1 秒越过不稳定期。
     *    少了这一步，第一次读取大概率 stage=1，
     *    表现是"上电必失败一次"的假故障，很容易被误判成接线问题。 */
    osDelay(APP_DHT11_STABILIZE_MS);

    /* ⑥ 预热：丢弃首次读取（复位后第一次采样常无效，协议使然） */
    (void)PER_DHT11_Read(&data);

    for (;;)
    {
        /* ⓵ 采集：失败最多重试 APP_DHT11_RETRY_COUNT 次，中间隔 100 ms。
         *    用 osDelay 而不是忙等：等传感器恢复的这段时间可以让出 CPU。 */
        ok = 0U;
        for (retry = 0U; retry < APP_DHT11_RETRY_COUNT; retry++)
        {
            if (PER_DHT11_Read(&data) == PER_DHT11_OK)
            {
                ok = 1U;
                break;
            }
            if ((retry + 1U) < APP_DHT11_RETRY_COUNT)
            {
                osDelay(APP_DHT11_RETRY_INTERVAL_MS);
            }
        }

        if (ok != 0U)
        {
            /* ⓶ 联动：成功才动灯（app_link 内部是边沿触发，平时不碰灯） */
#if APP_LINK_ENABLE
            APP_LINK_ApplyTemp(data.temperature_int);
            APP_LINK_ApplyHum(data.humidity_int);
#endif

            /* ⓷ 快照：把这一轮结果留给上云任务（只写结构体，不通知、不阻塞）。
             *    seq 自增让读方能看出"这期间有没有更新过"。 */
            s_snapshot.temp_int = data.temperature_int;
            s_snapshot.temp_dec = data.temperature_dec;
            s_snapshot.humi_int = data.humidity_int;
            s_snapshot.humi_dec = data.humidity_dec;
            s_snapshot.valid    = 1U;
            s_snapshot.seq++;

            /* ⓸ 打印：整数部分 + 一位小数 */
            n = build_th_line(buf, data.temperature_int, data.temperature_dec,
                                   data.humidity_int,    data.humidity_dec);

            /* ⓸' 显示：同一组数顺手刷到 OLED 前两行（2.9）。
             *     ★ 放在"快照更新之后"：屏上看到的数与上云读到的是同一组。
             *     ★ 写屏在 app_oled.c 里拿 osMutex（100 ms 超时），拿不到就放弃
             *       本帧——光敏任务可能正在写后两行，不让两条 I2C 事务交叠。 */
#if APP_OLED_ENABLE
            APP_OLED_UpdateTempHum(data.temperature_int, data.temperature_dec,
                                   data.humidity_int,    data.humidity_dec);
#endif
        }
        else
        {
            /* ⓹ 失败：只报 stage，不调 app_link、不更新快照 ——
             *    灯保持现状，快照保持上一次的有效值（故障安全）。
             *    上云任务因此上报的仍是"最后一个可信值"，而不是 0。 */
            n = build_fail_line(buf, (uint8_t)PER_DHT11_GetLastFailStage());

            /* ⓹' 显示：只把 Temp/Hum 两行写成 ERROR，Light/Level 两行不动 ——
             *    一个传感器坏了，另一个照常显示（故障隔离在"行"这一级）。 */
#if APP_OLED_ENABLE
            APP_OLED_ShowError();
#endif
        }
        buf[n] = '\0';
        MOD_UART1_SendString(buf);

        /* ⓺ 节拍：DHT11 最快 1 Hz，2 秒稳妥。
         *    必须用 osDelay 而不是 per_delay_ms —— 业务节拍一律让出 CPU。 */
        osDelay(APP_DHT11_PERIOD_MS);
    }
}

/*----------------------------------------------------------------------------
 * 【接口契约见 app_dht11.h】
 *----------------------------------------------------------------------------*/
void APP_DHT11_GetSnapshot(APP_DHT11_Snapshot_t *out)
{
    if (out == NULL)
    {
        return;
    }
    *out = s_snapshot;   /* 结构体整体拷贝：4 个 uint8 + 1 个 uint32，对齐读写 */
}

#endif /* APP_DHT11_ENABLE */
