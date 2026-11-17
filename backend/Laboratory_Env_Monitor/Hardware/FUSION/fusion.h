/* fusion.h：融合结果结构体与对外接口 */
#ifndef __FUSION_H
#define __FUSION_H

#include "handle_config.h"

/* 融合结果结构体 */
typedef struct
{
    float   score;                    /* 综合风险分 0~100 */
    uint8_t level;                    /* 综合等级 L1~L4 */
    uint8_t dominant;                 /* 主导风险源 */
    float   factor_score[MODULE_NUM]; /* 各模块风险分 */
    float   ai_sim;                   /* 边缘 AI 平滑相似度 0~100 */
    uint8_t ai_valid;                 /* 1=本次评分已参考 AI */
} FusionResult;

/* 对外接口 */
extern void Fusion_Init(void);
extern void Fusion_Evaluate(float temp, float hum, uint16_t light, uint8_t body,
                            const uint8_t supp[MODULE_NUM],
                            const AlarmLevel lvl[MODULE_NUM],
                            FusionResult *out);

/* AI 辅助评分：只加不减。sim 为 0~100 相似度，valid=0 时忽略。
 * 偏离度超过死区后按增益加到综合分，并重新映射等级；
 * 规则本身未到"提示"时，AI 最多只能把等级抬到"提示"，不会独立报警。 */
extern void Fusion_ApplyAi(FusionResult *out, float sim, uint8_t valid);

/* 对最终等级做统一防抖（在 Evaluate 和 ApplyAi 之后调用一次） */
extern void Fusion_Stabilize(FusionResult *out);

#endif /* __FUSION_H */
