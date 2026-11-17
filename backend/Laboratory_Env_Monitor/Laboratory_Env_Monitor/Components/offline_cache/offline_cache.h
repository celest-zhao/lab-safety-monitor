#ifndef __OFFLINE_CACHE_H
#define __OFFLINE_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*******************************************************************************
 * 离线缓存模块（按《离线缓存对接文档》实现）
 *
 * 解决文档指出的三个核心缺口：
 *   A. 离线数据未持久化  -> W25Q128 环形日志（Flash 最后 512KB，断电不丢）
 *   B. 补传没有可靠底座  -> 发一条等 +MQTTPUB:OK 才推进读指针（ACK 驱动），
 *                            失败重发（上限后跳过），节流约 1 条/秒防打爆平台
 *   D. 无显式网络态      -> g_net_state 三态 + 3 秒滞回去抖
 *
 * 对外只有 5 个接口，见下方注释。
 ******************************************************************************/

/* ---------------- 网络三态（文档 P1：g_net_state） ----------------
 * OFFLINE : 上报出口走"写盘"（每 5 秒一条，512KB 可存约 12 小时）
 * RECONN  : 预留（当前简化为两态，滞回期间短暂经过）
 * ONLINE  : 直发；同时补传引擎把 Flash 里的历史按序重放 */
typedef enum
{
    NET_OFFLINE = 0,
    NET_RECONN  = 1,
    NET_ONLINE  = 2
} NET_STATE;

extern volatile NET_STATE g_net_state;   /* OfflineCache_Poll 内部维护，勿手动写 */

/* ---------------- 离线记录（文档第三节 A 的固定结构） ----------------
 * 在文档字段基础上扩展了 dom/sound/score（融合研判三件套离线也要能补报）。
 * 结构体 sizeof = 28 字节（含对齐），落盘时按 32 字节槽位存放：
 *   32 能整除 W25Q128 的 256 字节页，记录不会跨页，规避 w25qxx.c
 *   W25qxx_WriteSector() 跨页写指针步进错误导致的半条写坏（v3 修复）。
 * reserved 字段固定写 0xA6 作格式标识（v4 起加入 ai/temp_rise），
 * crc8 单条校验防半写/损坏。结构体仍为 28 字节，槽位 32 字节。 */
typedef struct
{
    uint32_t seq;          /* 设备自增序号：幂等去重键，掉电不回退      */
    uint32_t ts;           /* RTC 秒级时间戳（2000-01-01 起算的秒数）   */
    uint8_t  type;         /* 0x01=周期采样 0x02=告警事件               */
    uint8_t  level;        /* 融合等级 0~3（上报时 +1 对应 L1~L4）      */
    int16_t  temp;         /* 温度 ×10                                  */
    int16_t  humi;         /* 湿度 ×10                                  */
    uint16_t light;        /* 光照 lx                                   */
    uint8_t  gas;          /* 1=检测到烟雾/可燃气体                     */
    uint8_t  body;         /* 1=有人                                    */
    uint8_t  fan;          /* 风扇继电器实际状态                        */
    uint8_t  lamp;         /* 补光继电器实际状态                        */
    uint8_t  dom;          /* 主要风险源 MODULE_*                       */
    uint8_t  sound;        /* 报警声开关（触摸键静音状态）              */
    uint8_t  score;        /* 融合分数整数部分 0~100                    */
    uint8_t  ai;           /* AI 相似度 0~100，255=无效        */
    int8_t   temp_rise_x10;/* 10s 温升 x10（℃）                 */
    uint8_t  reserved;     /* [v4] 格式标识，固定 0xA6                  */
    uint8_t  crc8;         /* 单条校验（多项式 0x07，只算到 crc8 之前） */
} OFFLINE_RECORD;

/*******************************************************************************
 * OfflineCache_Init
 *   初始化 W25Q128 并扫描日志区（掉电恢复：从残条处续写、从最旧记录续读）。
 *   幂等：重复调用只生效一次；Flash 初始化失败则模块自动旁路，不影响主流程。
 ******************************************************************************/
void OfflineCache_Init(void);

/*******************************************************************************
 * OfflineCache_Poll( net_connected )
 *   主周期调用（control_task 的 10ms 循环里调一次），内部完成：
 *     1) g_net_state 滞回切换（3 秒去抖）；
 *     2) 离线时每 5 秒把当前采样+融合结论落盘；
 *     3) 在线时按节流补传 Flash 里的历史（每条等 ACK，见 NotifyPub）。
 *   参数：net_connected —— 传 Is_Net_Module_Connected() 的返回值。
 ******************************************************************************/
void OfflineCache_Poll(uint8_t net_connected);

/*******************************************************************************
 * OfflineCache_NotifyPub( ok )
 *   MQTT 发布结果回调：esp8285.c 收到 +MQTTPUB:OK 传 1，+MQTTPUB:FAIL 传 0。
 *   补传引擎据此推进/回退读指针（确认成功才删，失败重发）。
 ******************************************************************************/
void OfflineCache_NotifyPub(uint8_t ok);

uint32_t OfflineCache_Count(void);            /* 当前未补传条数（调试/显示用） */
NET_STATE OfflineCache_GetNetState(void);     /* 读当前网络态                   */

#ifdef __cplusplus
}
#endif

#endif /* __OFFLINE_CACHE_H */
