/**
 ******************************************************************************
 * @file    app_mqtt.h
 * @brief   应用层（app）—— WiFi-MQTT 上云任务（业务侧）
 *
 *  本层只回答三个问题，一个比一个上层：
 *      报什么？      DHT11 的温湿度快照（不是"再读一次传感器"）
 *      多久报？      每 5 秒一次（云端可改）；断了就进离线缓存，恢复后按序补传
 *      命令怎么应？  云端下发 LED_Control，落到与串口命令同一个入口，然后回响应
 *
 *  华为云下行有三条通道（按主题分发，见 app_mqtt.c 的 app_mqtt_handle_downlink）：
 *      commands/#      同步命令：执行 LED_Control 并回命令响应（request_id 原样带回）
 *      properties/set/# 属性设置：publish_period_s（运行时改上报周期）/ led，
 *                        回属性设置响应 {"result_code":0/1}
 *      messages/down   消息下行：异步执行命令，不回响应（应答通道不存在）
 *
 *  ★ 连接是【分段】做的，不是一锤子买卖：
 *      ① AT 测活 → ② STA 模式 → ③ 连 AP → ④ 建 TCP → ⑤ MQTT CONNECT → ⑥ 订阅三个下行主题
 *    分段的收益在"重连"上：重连时只补没成功的段，不从头再来一遍
 *    （WiFi 还好好的却去重连 WiFi，是最浪费时间的一种重连）。
 *
 *  ★ 断线是常态，不是异常：
 *    指数退避 2/4/8/16/30 秒，第 5 次直接 AT+RST 复位模块，
 *    发不出去的数据进 16 条 RAM 队列，恢复后按序补传。
 *    能自愈的设备才谈得上"交付"，否则每次掉线都要人去按复位。
 *
 *  ★ 本任务只读快照、不催采集：
 *    DHT11 最快 1 Hz，若为上报去催它，就会把传感器的时序压到临界，
 *    现象是"上云之后本地的温湿度读数也开始失败"。分层解耦的收益在此。
 ******************************************************************************
 */
#ifndef APP_MQTT_H
#define APP_MQTT_H

#include <stdint.h>

/* ==========================================================================
 *  ★★★ 用户配置区：这 8 个宏是【唯一】需要你改的地方 ★★★
 *
 *  ★ 凭证不入库（2026-10-29 起）：
 *    本文件里的值全是占位符。真实值写在 user/app/Inc/app_mqtt_secret.h
 *    （被 .gitignore 忽略，只留在本机）。该文件存在时本文件的所有配置被它
 *    覆盖；不存在（如新克隆的仓库）就回落到下面的占位符，编译不报错。
 *    首次使用：复制 app_mqtt_secret.h.template 为 app_mqtt_secret.h 再填值。
 *
 *  从哪里来（华为云 IoTDA 控制台）：
 *    ① 创建产品（协议 MQTT / 数据格式 JSON）→ 记下产品 ID
 *    ② 定义服务 Sensor，属性 temperature / humidity（decimal）
 *    ③ 添加命令 LED_Control，参数 led（int）
 *    ④ 注册设备 → 保存 device_id 与 device_secret
 *    ⑤ 总览 → 接入信息 → 复制 MQTT 域名（cn-north-4）
 *    ⑥ 在线工具生成鉴权参数（选"不校验时间戳"）→ ClientId / 用户名 / 密码
 *
 *  ★ 端口是 1883 而不是密钥文件里的 8883：
 *    8883 是 TLS 端口，ESP8266 的 AT 固件不做 TLS 握手。
 *    域名保持 iot-mqtts 不变，只把端口改 1883 —— 代价是数据在公网上是明文。
 *  ★ 鉴权参数由在线工具算好再填：不在 MCU 上实现 HMAC-SHA256，
 *    否则要拉进一整套加密库（几 KB FLASH + 一堆移植工作）。
 * ========================================================================== */
/* 本机私有凭证（git 忽略）：存在则先引入 —— 下面占位符只在它没定义时生效 */
#if __has_include("app_mqtt_secret.h")
#include "app_mqtt_secret.h"
#endif

/* ---- 占位符（仅当私密文件没提供同名宏时才编译进来，避免 redefined 警告）---- */
#ifndef APP_MQTT_WIFI_SSID
#define APP_MQTT_WIFI_SSID      "your-wifi-ssid"         /* 必须是 2.4GHz！ESP8266 不支持 5GHz */
#define APP_MQTT_WIFI_PWD       "your-wifi-password"

#define APP_MQTT_BROKER_HOST    "xxxxxxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com"
#define APP_MQTT_BROKER_PORT    1883                    /* 明文 MQTT；8883 是 TLS，本工程不用 */

#define APP_MQTT_DEVICE_ID      "your-device-id"                           /* 设备 ID：主题里要用它 */
#define APP_MQTT_CLIENT_ID      "your-device-id_0_0_0000000000"            /* 在线工具生成的 ClientId */
#define APP_MQTT_USERNAME       "your-device-id"                           /* 在线工具生成的用户名 */
#define APP_MQTT_AUTH_PASSWORD  "your-hmac-password"                       /* 在线工具生成的密码 */
#endif /* !APP_MQTT_WIFI_SSID */

/* 物模型里的服务 ID：必须与控制台"功能定义"里完全一致，
 * 不一致的后果是华为云静默丢弃属性——设备在线、日志显示 Published，
 * 但"最新数据"永远是空的。这是最常见、也最容易被误判成网络故障的一种失败。 */
#define APP_MQTT_SERVICE_ID     "Sensor"

/*----------------------------------------------------------------------------
 * 函数功能：WiFi-MQTT 上云任务体（永不返回）
 * 参数    ：argument —— CMSIS-RTOS2 的任务参数槽（本任务不使用，恒为 NULL）
 * 返回值  ：无
 * 调用处  ：app_task.c 的 app_taskCreate() 里被 osThreadNew 登记，
 *          调度器启动后由内核调用（创建顺序排在最后）
 * 注意事项：① 启动序列会先打一行横幅，再依次尝试六段连接；
 *              连不上就按退避策略重试，绝不阻塞启动流程
 *           ② 主循环三件事：周期上报 / 收一包 / 有命令就执行并回响应
 *           ③ 优先级与其他三个任务同为 1：慢链路不该抢占本地控制——
 *              WiFi 掉线时，灯还得照常闪、串口还得照常收命令
 *----------------------------------------------------------------------------*/
void app_mqtt_task(void *argument);

#endif /* APP_MQTT_H */
