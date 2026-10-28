/**
 * @file    log.h
 * @brief   统一日志宏（common 横切层）
 *
 * 用法：在 .c 顶部 #define LOG_TAG "per_led" 再 include 本头文件。
 * 注意：printf 走 syscalls.c 的 _write，需在串口章节完成 UART1 重定向后
 *       才有真实输出；在此之前 LOG_* 只参与编译、不产生副作用。
 * 级别由 cfg/module_cfg.h 的 CFG_LOG_LEVEL 控制，编译期裁剪，0 开销。
 */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include "main.h"      /* HAL_GetTick()：CubeMX 工程经由 main.h 引入 HAL */
#include "module_cfg.h"

#ifndef LOG_TAG
#define LOG_TAG "app"
#endif

#define LOG(level, fmt, ...) \
    do { \
        if ((level) <= CFG_LOG_LEVEL) { \
            (void)printf("[%s][%s][%d] " fmt "\r\n", \
                         LOG_TAG, log_level_name(level), \
                         (unsigned)(HAL_GetTick() & 0x7FFFFFFFu), ##__VA_ARGS__); \
        } \
    } while (0)

static inline const char *log_level_name(int level)
{
    switch (level) {
    case 1: return "E";
    case 2: return "W";
    default: return "I";
    }
}

#define LOG_I(fmt, ...) LOG(3, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) LOG(2, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) LOG(1, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
