#ifndef AI_MONITOR_H
#define AI_MONITOR_H

#include <stdint.h>

/* ============================================================
 * ai_monitor — 边缘 AI 模块（FISVDD 离群检测 + 温度趋势预判）
 *
 * 内部封装：状态机 + 特征提取 + 推理 + 平滑 + 相似度输出。
 * 不依赖融合层：外部只需 AiMonitor_Init() + 每周期 AiMonitor_Process()，
 * 再用 AiMonitor_GetSim() 取出 sim；是否/如何并入综合评分由调用方
 * （Fusion_ApplyAi / Fusion_Stabilize）统一编排，本模块不反向调用融合。
 *
 * 输出 sim 是两条通道的较小值：
 *   model — 当前帧离群程度（FISVDD 二值 + 表决 + 线性计数）
 *   trend — 温度整体趋势的提前预判（30 秒回归外推 60 秒）
 * 模型只会"事后"判定，温度缓慢爬升时它一直报 100；趋势通道补的就是这段提前量。
 * ============================================================ */

/* ---- AI 评分参数 ---- */
#define AI_SCORE_EN         1           /* 1=参与评分  0=仅显示          */
#define AI_SCORE_DEADBAND   25.0f       /* 偏离度死区                    */
#define AI_SCORE_GAIN       0.10f       /* 加分增益                      */
#define AI_SCORE_MIN_SIM    10.0f       /* sim 下限：低于此值按此值计，不丢弃 */

/* 注：原 AI_TREND_BOOST_MAX（趋势独立加分上限 14.0f）已移除。
 * 它曾让趋势在温度因子之外再独立加一次分，与 Score_Temp() 内的趋势分量
 * 重复计算同一物理量，使趋势被计两遍。
 *
 * 现趋势只经 Score_Temp() 进入温度因子。Score_Temp 用 max() 而非加权平均，
 * 趋势满值直接透传 100 分 x 0.30 权重 = 30 分，risk >= 84 时单独可跨 L2。
 * 这是 2026-11-13 D1 的决策：保留趋势的独立 L2 能力，换取约 11 分钟提前量。
 * 30 分 < L3 门槛 50，所以趋势再强也不会触发 L3/L4，最坏是一次 L2 提示。 */

/* sim 无效值（故障期返回） */
#define AI_SIM_INVALID      255u

/* ---- 对外接口 ---- */

/* 上电调用一次：加载 FISVDD 预训练模型 */
void AiMonitor_Init(void);

/* 每采样周期调用（1Hz）。内部完成：
 *   ① 趋势通道：温度入 30 点历史 → 线性回归 → 外推 60s → 与 τ=300s 基线比较 → sim_trend
 *   ② 模型通道：neai_outlier 推理 → N/K 多数表决 → 双向线性计数 → sim_model
 *   ③ 合成：sim = min(sim_model, sim_trend)，通过 AiMonitor_GetSim() 对外
 * 不直接触碰融合层，便于独立替换模型或单测。
 */
void AiMonitor_Process(float temp, float hum, float light);

/* 最终相似度 0~100（两通道取小），255=未就绪。LCD/串口/云端/融合都用这个 */
uint8_t AiMonitor_GetSim(void);

/* 分通道取值，调试与定标用：
 *   GetModelSim — 仅 FISVDD 离群通道（判"现在"）
 *   GetTrendSim — 仅温度趋势通道（判"要去哪"）
 * 两者都返回 AI_SIM_INVALID 表示模块未就绪。
 */
uint8_t AiMonitor_GetModelSim(void);
uint8_t AiMonitor_GetTrendSim(void);

/* 温度趋势风险 0~100（0=无趋势，100=满偏）。供融合层计分用。
 * 与 GetSim 的区别：GetSim 是"像不像正常"，TrendRisk 是"离 40C 还有多远"。
 * 故障期返回 0（不参与计分），不是 255。 */
uint8_t AiMonitor_GetTrendRisk(void);

/* 状态名（"RUN"/"ERR"/"OFF"，显示用） */
const char *AiMonitor_GetStateName(void);

#endif
