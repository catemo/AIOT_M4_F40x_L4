/**
 ******************************************************************************
 * @file    per_iwdg.c
 * @brief   感知层（perception）—— 独立看门狗 IWDG 驱动实现
 *
 *  本层是全工程【唯一】直接碰 HAL_IWDG 与 RCC 复位标志的地方：
 *    - 启动：HAL_IWDG_Init（分频 / 重载来自 cfg_param.h，改超时不用翻本文件）
 *    - 喂狗：HAL_IWDG_Refresh（往 KR 写 0xAAAA 的 HAL 封装）
 *    - 复位原因：RCC_CSR 的 IWDGRSTF 标志（读 / 清分成两个函数，理由见 .h）
 *    - 调试冻结：__HAL_DBGMCU_FREEZE_IWDG()（halt 期间停表，单步不咬狗）
 *
 *  接口的完整说明（功能/参数/返回值/调用处/注意事项）写在 per_iwdg.h，
 *  本文件的注释侧重"硬件为什么会这样"。
 *
 *  ★ 与课件 2.4 的一处刻意不同：
 *    课件的 IWDG 由 CubeMX 生成 Core/Src/iwdg.c（MX_IWDG_Init），
 *    本工程把它收进感知层——参数进 cfg、初始化进 per_iwdg，
 *    Core 保持原样，改超时只改 cfg_param.h 两行。
 *    代价是 hal_conf.h 里那行 HAL_IWDG_MODULE_ENABLED 是手改的，
 *    CubeMX 重新生成后要加回去（cfg_switch.h 有注释提醒，编译期也会报错）。
 ******************************************************************************
 */
#include "module_cfg.h"

#if PER_IWDG_ENABLE

#include "per_iwdg.h"
#include "main.h"            /* Error_Handler + 经它带入 stm32f4xx_hal.h（HAL_IWDG_* / RCC 标志宏） */

/* IWDG 句柄：本驱动自有一份，不与 CubeMX 共享（Core 里没有 iwdg.c）。
 * 句柄内容其实只有 Instance + Init 两个字段，但 HAL 的接口约定就是
 * "传句柄指针"，照约定来，换芯片时不用改调用方式。 */
static IWDG_HandleTypeDef s_hiwdg;

/*----------------------------------------------------------------------------
 * 分频值映射：cfg 写的是分频倍数（64），HAL 要的是寄存器编码（IWDG_PRESCALER_64）。
 * 为什么不直接在 cfg 里写 IWDG_PRESCALER_64：
 *   module_cfg.h 是全工程【第一个】被包含的头文件，那时 HAL 头还没进来，
 *   宏体里出现 IWDG_PRESCALER_64 虽能编译（延迟展开），但 cfg_check.h 的
 *   "合法集校验"就没法用纯数字做——写数字让闸门能算术，是本工程 cfg 的规矩。
 *----------------------------------------------------------------------------*/
static uint32_t iwdg_prescaler_code(uint32_t div)
{
    uint32_t code;

    switch (div)
    {
    case 4U:    code = IWDG_PRESCALER_4;   break;
    case 8U:    code = IWDG_PRESCALER_8;   break;
    case 16U:   code = IWDG_PRESCALER_16;  break;
    case 32U:   code = IWDG_PRESCALER_32;  break;
    case 64U:   code = IWDG_PRESCALER_64;  break;
    case 128U:  code = IWDG_PRESCALER_128; break;
    case 256U:  code = IWDG_PRESCALER_256; break;
    default:    code = 0xFFFFFFFFU;        break;   /* 非法值：cfg_check 闸门正常情况下已拦 */
    }
    return code;
}

void PER_IWDG_Init(void)
{
    uint32_t code = iwdg_prescaler_code(PER_IWDG_DIV);

    if (code == 0xFFFFFFFFU)
    {
        /* 闸门失效时的兜底：宁可停机保留现场，也不带一个"超时未知"的看门狗跑 */
        Error_Handler();
        return;
    }

    s_hiwdg.Instance       = IWDG;
    s_hiwdg.Init.Prescaler = code;
    s_hiwdg.Init.Reload    = PER_IWDG_RELOAD;
    (void)HAL_IWDG_Init(&s_hiwdg);
    /* HAL_IWDG_Init 内部：写访问钥匙 → 装 PR/RLR → 等 SR 清零 →
     * 重载一次 → 写 0xCCCC 启动。返回后看门狗已经在跑，且硬件强制 LSI 打开。 */

    /* 调试冻结：halt 期间 IWDG 停表，单步超过 3 秒也不会被复位打断。
     * 没有这一行，调试器里断点多停几秒，程序就"莫名其妙从头开始"。 */
    __HAL_DBGMCU_FREEZE_IWDG();

    /* 启动即喂一次：让"第一口狗"从初始化完成这一刻起算满 3 秒 */
    (void)HAL_IWDG_Refresh(&s_hiwdg);
}

void PER_IWDG_Feed(void)
{
    (void)HAL_IWDG_Refresh(&s_hiwdg);   /* 往 KR 写 0xAAAA，计数器回到 RELOAD */
}

uint8_t PER_IWDG_LastResetByIwdg(void)
{
    /* RCC_CSR 的 IWDGRSTF：上次复位由 IWDG 触发时置 1，且【不会】自动清零 */
    return (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) ? 1U : 0U;
}

void PER_IWDG_ClearResetFlags(void)
{
    __HAL_RCC_CLEAR_RESET_FLAGS();   /* 置 RMVF：清全部复位标志（不止 IWDG 那一位） */
}

#endif /* PER_IWDG_ENABLE */
