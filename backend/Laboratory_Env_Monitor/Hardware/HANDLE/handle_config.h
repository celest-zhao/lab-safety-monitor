/*******************************************************************************
 * handle_config.h：统一配置头文件（阈值/权限/开关/规则开关集中在此）
 ******************************************************************************/
#ifndef __HANDLE_CONFIG_H
#define __HANDLE_CONFIG_H

#include "stm32l4xx_hal.h"

/* 模块编号（对应 LED/key/LCD 页） */
#define MODULE_TEMP      0
#define MODULE_GAS       1
#define MODULE_BODY      2
#define MODULE_LIGHT     3
#define MODULE_NUM       4

/* 告警等级（越大越严重） */
typedef enum
{
    ALARM_NORMAL    = 0,    /* 正常（LED 灭） */
    ALARM_HINT,             /* 提示（LED 慢闪 500ms，蜂鸣器不响，最低级别） */
    ALARM_WARN,             /* 预警（LED 快闪 200ms + 短蜂鸣） */
    ALARM_EMERGENCY         /* 紧急（LED 急闪 50ms  + 长鸣不止） */
} AlarmLevel;

/* 温湿度阈值 */
#define TEMP_MIN         0.0f    /* 温度下限 ℃ */
#define TEMP_MAX         40.0f   /* 温度上限 ℃ */
#define HUMI_MIN         20.0f   /* 湿度下限 %RH */
#define HUMI_MAX         80.0f   /* 湿度上限 %RH */

/* 光照阈值 */
#define LIGHT_LOW        100U    /* 低于此 lux 视为偏暗（触发补光条件） */

/* 安全分级计时（ms） */
#define SAFE_L2_TIME     3000U   /* <3s  -> 提示 */
#define SAFE_L3_TIME     8000U

/* command(cloud/local button) opened device auto-closes after env safe for this long.
   Fixes "Huawei cloud on => stays on until manual off". Tune smaller e.g. 10000=10s. */
#define AUTO_OFF_HOLD_MS   60000U   /* default 60s */
   /* 3~8s -> 预警，>8s -> 紧急 */

/* 按键参数 */
#define KEY_LONG_MS      1000U   /* 长按超过 1000ms 判定为长按 */
#define KEY_SCAN_MS      20U     /* 按键扫描周期 20ms */

/* 采样参数 */
#define SAMPLE_MS        1000U   /* 传感器采样周期 1s */

/* 执行器配置 */
/* 通风继电器（风扇电机）PA8 高电平吸合 */
#define FAN_PIN          GPIO_PIN_8
#define FAN_PORT         GPIOA
#define FAN_ON()         HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_SET)
#define FAN_OFF()        HAL_GPIO_WritePin(FAN_PORT, FAN_PIN, GPIO_PIN_RESET)

/* 补光灯：PB15 高电平点亮（兼作告警灯） */
#define FILL_PIN         GPIO_PIN_15
#define FILL_PORT        GPIOB
/* 备选占位：FILL_PIN GPIO_PIN_9 / FILL_PORT GPIOC */
#define FILL_ON()        HAL_GPIO_WritePin(FILL_PORT, FILL_PIN, GPIO_PIN_SET)
#define FILL_OFF()       HAL_GPIO_WritePin(FILL_PORT, FILL_PIN, GPIO_PIN_RESET)

/* ---- external push buttons: one-key toggle, no cloud command needed ----
   default PA4=fan button, PA5=lamp button, other end to GND, internal pull-up.
   Change these two pins to any free GPIO on your board. */
#define BTN_FAN_PIN      GPIO_PIN_4
#define BTN_FAN_PORT     GPIOA
#define BTN_LAMP_PIN     GPIO_PIN_5
#define BTN_LAMP_PORT    GPIOA
#define BTN_PULL         GPIO_PULLUP
#define BTN_DEBOUNCE_MS  50U       /* debounce window(ms) */


/* 综合风险评估融合（V3.1 参数） */

/* 模块权重（各模块之和 = 1.0）
 * 2026-11-13：人体红外的 0.20 权重并入气体模块。
 * 理由：实验室人员进出是常态，"有人在"不应作为独立风险源计分；
 *       人体的作用改为"暴露条件"，只参与规则2（有人 + 漏气 -> 紧急），
 *       不再往综合分里加分。气体模块因此成为唯一的安全主通道：
 *       0.30 + 0.55 + 0.00 + 0.15 = 1.00 */
#define FUSION_W_TEMP_HUMI  0.30f   /* 温湿度模块权重                         */
#define FUSION_W_GAS        0.55f   /* 气体模块权重（原 0.35 + 人体让出的 0.20）*/
#define FUSION_W_BODY       0.00f   /* 人体模块权重：已并入气体，不再计分      */
#define FUSION_W_LIGHT      0.15f   /* 光照模块权重                           */

/* 温湿度模块内温度占比 */
//#define FUSION_TEMP_MIX     0.50f

/* 综合分 → 等级边界 */
#define FUSION_L2_BOUND     25.0f   /* [0,25)   正常 L1 */
#define FUSION_L3_BOUND     50.0f   /* [25,50)  提示 L2 */
#define FUSION_L4_BOUND     75.0f   /* [50,75)  预警 L3，[75,100] 紧急 L4 */

/* L2 迟滞：进入用 FUSION_L2_BOUND(25)，退出要求分数低于 (门槛 - 本值)。
 * 缓慢升温时综合分会在 25 分附近长时间横盘，3/5 秒防抖挡不住，
 * 实测 300 秒内等级翻转 5 次；加 3 分迟滞后退回 1 次。 */
#define FUSION_L2_HYST      3.0f

/* 单模块打分步长参数 */
#define TEMP_SCORE_STEP     10.0f   /* 温度每越界 10℃ 加满 100 分（线性） */
#define HUMI_SCORE_STEP     30.0f   /* 湿度每越界 30%RH 加满 100 分 */
#define LIGHT_DARK_MAX      20U     /* 光照 <20lx 视为极暗，直接 100 分 */

/* 光照因子是否要求"有人"（2026-11-13 D3）
 * 1 = 只有检测到有人时，偏暗/极暗才计分（默认）
 * 0 = 旧行为，不看人体，无人关灯照样给满 100 x 0.15 = 15 分
 * 理由：无人关灯是夜间常态，不是风险信号。旧行为让"无人 + 灯灭"凭空
 * 得到 15 分，叠上气体 L4 的 55 分就是 70，再乘规则4 的 1.2 就是 84，
 * 使"要不要长鸣"实际由灯的开合决定。 */
#define LIGHT_NEED_BODY     1

/* 安全分级 → 分数 */
#define SAFE_SCORE_L2       40.0f   /* 提示 L2 */
#define SAFE_SCORE_L3       70.0f   /* 预警 L3 */
#define SAFE_SCORE_L4       100.0f  /* 紧急 L4 */

/* 专家规则开关（1=启用）
 * 规则3（无人值守期突现人员 → 入侵加分）已删除：实验室人员进出属常态，实测每天误触发。 */
#define RULE_FIRE_EN        1   /* 规则1：气体预警 + 温度越界 → 直接进入紧急（火情） */
#define RULE_PERSON_DANGER  1   /* 规则2a：漏气 >=8s → L4（不看人体）；2b：有人 + 漏气 >=3s → 提前 L4 */
#define RULE_PERSIST_EN     0   /* 规则4：安全项持续超时 → 综合分 ×1.2。
                                 * 2026-11-13 D3 关闭：气体模块自身已按持续时长
                                 * 判级（<3s 提示 / 3~8s 预警 / >=8s 紧急），
                                 * 融合层再对"气体 L4"整体乘 1.2 等于把同一段
                                 * 时长算两遍；而且它会放大所有无关因子，
                                 * 正是"无人关灯 15 分"能推过 75 的推手。
                                 * 置 1 可立即恢复旧行为（用于对比测试）。 */

/* 人体红外：只提示"有人"，不判级、不报警（2026-11-13）
 * - s_alarm[MODULE_BODY] 恒为 ALARM_NORMAL：LED3 不闪、蜂鸣器不响、综合分不计；
 * - LCD 人体页直接用 s_body 显示 Someone / No one，等级固定显示 L1；
 * - 人体的安全意义只保留在规则2：有人 + 漏气超过 8 秒 → 融合直接 L4。
 * 这样"有人在"本身永远不会触发报警，权重 0.20 已经并入气体。 */

/* 综合分容错页号 */
#define FUSION_PAGE         (MODULE_NUM + 1)

/* ================= 低功耗（暂停采集）模式 V5 =================
 * 用途：非采集时段人工把节点切到"只监视、不采集"的状态，
 *       替代逐个关闭传感器/执行器的操作。
 * 进入：KEY4 按住 LP_ENTER_MS 不放；退出：任意键，或 LP 采样判定越界自动退出。
 * 说明：本模式降低的是采集频次与执行器驱动开销，主控与网络仍在运行，
 *       因此不涉及 STOP2 与网络断开。 */
#define LP_ENTER_MS         3000U   /* KEY4 长按 3s 进入              */
#define LP_SAMPLE_MS        30000U  /* 低功耗态采样周期 30s            */
#define LP_WAKE_LOCK_MS     2000U   /* 退出后 2s 内屏蔽按键，防误触     */
#define LP_DRAW_MS          500U    /* 计时行刷新节流                  */


/* ================= 边缘 AI =================
 * 由 task_user/ai_monitor.c 独立模块管理（状态机 + 推理 + 融合注入）。
 * AI 评分参数（AI_SCORE_EN / AI_SCORE_DEADBAND / AI_SCORE_GAIN 等）
 * 定义在 task_user/ai_monitor.h。 */

/* 温湿度/光照 HINT 的时间确认 */
#define ENV_CONFIRM_MS       3000U

/* 综合等级防抖：分数在阈值附近抖动时，等级需要持续一段时间才切换。
 * 紧急等级仍然立即生效，安全优先。 */
#define FUSION_LEVEL_UP_MS   3000U
#define FUSION_LEVEL_DOWN_MS 5000U

#endif /* __HANDLE_CONFIG_H */
