/**
 ******************************************************************************
 * @file    app_param.h
 * @brief   应用层（app）—— 20 字节参数配置块 + 存储任务（2.3 外部存储器新增）
 *
 *  参数链的业务侧："存什么"。这一层装的知识：
 *    配置块的字段与布局 / 魔数 + 版本 + CRC16 三关校验 / 默认值 / 加载五步。
 *  页怎么拆、写周期怎么等属于 mod_eeprom；总线怎么翻转属于 net_i2c ——
 *  本文件对 AT24C02 的全部认知就是"一个 256 字节的字节仓库，从 0 号地址起"。
 *
 *  ★ 配置块过三关（对应课件 20 页）：
 *    magic   —— "写过没有？"（新芯片全是 0xFF，魔数不对就写默认值）
 *    version —— "结构还是我认识的那个吗？"（升级后旧数据不兼容，按默认走）
 *    crc16   —— "内容被写坏没有？"（正文任何一位翻转都能被抓到）
 ******************************************************************************
 */
#ifndef APP_PARAM_H
#define APP_PARAM_H

#include <stdint.h>
#include "module_cfg.h"

/* ---- 配置块身份三件套 ---- */
#define APP_PARAM_MAGIC         0xAA55AA55u  /* 魔数：回答"写过没有"       */
#define APP_PARAM_CFG_VERSION   1u           /* 结构每改一次 +1（迁移路径）*/

/* ---- 20 字节配置块，布局钉死（对应课件 20 页） ---- */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* 0xAA55AA55：写过没有？   */
    uint16_t version;           /* 结构每改一次 +1          */
    uint16_t crc16;             /* CRC16-CCITT：内容坏没坏  */
    int16_t  temp_offset_x10;   /* 温度校准，单位 0.1 ℃    */
    uint8_t  temp_threshold;    /* LED1 阈值（℃）          */
    uint8_t  humi_threshold;    /* LED2 阈值（%RH）         */
    uint32_t report_period_ms;  /* 上云上报周期（暂存字段） */
    uint32_t reboot_count;      /* 重启计数：掉电保持的证据 */
} app_param_cfg_t;
#pragma pack(pop)

/* 尺寸闸门（第三道）：改字段忘同步就编译报错，把"忘了"堵在编译期 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(app_param_cfg_t) == APP_PARAM_CFG_SIZE,
               "app_param_cfg_t 尺寸必须等于 cfg_param.h 的 APP_PARAM_CFG_SIZE"
               "（改动字段后请同步该宏，并把 APP_PARAM_CFG_VERSION + 1）");
#endif

/* ---- 返回值 ---- */
#define APP_PARAM_OK            0
#define APP_PARAM_ERR_STORAGE  -1   /* 器件不在/读写失败：用默认值继续跑 */
#define APP_PARAM_ERR_PARAM    -2   /* 阈值越界（温度 0~50 / 湿度 20~90） */
#define APP_PARAM_ERR_VERIFY   -3   /* 写进去了但读回来不对 */

/*----------------------------------------------------------------------------
 * 函数功能：存储任务体（永不返回）—— 一个任务驱动两条链（对应课件 15 页）
 * 参数    ：argument —— CMSIS-RTOS2 任务参数槽（恒为 NULL）
 * 调用处  ：app_task.c 的 app_taskCreate() 登记
 * 注意事项：① 每 1 s（APP_STORAGE_TICK_MS）一拍：
 *              每 3 s（APP_PARAM_READBACK_MS）从 EEPROM 重读配置并校验——
 *              ★ 读回的是【芯片里的数据】而不是 RAM 副本，"写进去的能读出来"
 *                才是真验证（只打 RAM 的验证是假验证）；
 *              每 5 s（APP_LOGGER_PERIOD_MS）取 DHT11 快照追加一条日志。
 *           ② 周期任务【只读不写】：读回不限次数；写只发生在"首次上电写默认、
 *              用户改参数"两处 —— EEPROM 寿命约 100 万次，省着花（寿命保护）
 *           ③ 器件不在也照常跑：配置用默认值、日志跳过，串口会给出排查提示，
 *              绝不影响 LED / DHT11 / 上云三链
 *----------------------------------------------------------------------------*/
void app_storage_task(void *argument);

#if APP_PARAM_ENABLE
/*----------------------------------------------------------------------------
 * 函数功能：加载配置块（上电一次，加载五步走——对应课件 21 页）
 * 返回值  ：APP_PARAM_OK = 配置有效；APP_PARAM_ERR_STORAGE = 器件不在/写默认失败
 * 调用处  ：app_storage_task 开头（内部已打印全过程）
 *----------------------------------------------------------------------------*/
int APP_PARAM_Init(void);

/*----------------------------------------------------------------------------
 * 函数功能：取当前阈值（运行时读 RAM 副本，不碰总线）
 * 参数    ：temp_th / humi_th —— 输出（可任一为 NULL）
 * 注意事项：★ 阈值不再编译期写死：配置块有效时返回芯片里的值；
 *              没加载成功时回落 cfg_param.h 的 APP_LINK_*_THRESHOLD 宏
 *              —— 这正是 app_link 联动与串口 TH 命令的取值入口
 *----------------------------------------------------------------------------*/
void APP_PARAM_GetThresholds(uint8_t *temp_th, uint8_t *humi_th);

/*----------------------------------------------------------------------------
 * 函数功能：设置并保存新阈值（更新 RAM + 立即写 EEPROM + 读回确认）
 * 返回值  ：APP_PARAM_OK / ERR_PARAM / ERR_STORAGE / ERR_VERIFY
 * 调用处  ：app_cmd 的 TH 命令（串口保存阈值，断电不丢）
 *----------------------------------------------------------------------------*/
int APP_PARAM_SetThresholds(uint8_t temp_th, uint8_t humi_th);

/*----------------------------------------------------------------------------
 * 函数功能：抹掉配置块、强制回到默认值（寿命保护的例外出口）
 * 注意事项：正常业务永远不调它；调试想"重置出厂"时临时调用一次
 *----------------------------------------------------------------------------*/
int APP_PARAM_ResetDefault(void);

/*----------------------------------------------------------------------------
 * 函数功能：★ 诊断取证：从芯片重读前 32 字节，打原始 hex + 三关判定
 * 返回值  ：APP_PARAM_OK        = 读到了，且配置块三关全过（真存进去了）
 *           APP_PARAM_ERR_STORAGE = 读失败（总线/器件问题，不是数据问题）
 *           APP_PARAM_ERR_VERIFY  = 读到了，但 magic/version/crc 没过
 * 调用处  ：串口 PDUMP 命令
 * 注意事项：① "有没有存进去"只能靠读芯片回答 —— 打 RAM 副本是自证，不算证据
 *           ② 全 0xFF = 全新/被擦；全 0x00 = 写进去了但内容不对（查页拆分）
 *----------------------------------------------------------------------------*/
int APP_PARAM_Inspect(void);
#endif /* APP_PARAM_ENABLE */

#endif /* APP_PARAM_H */
