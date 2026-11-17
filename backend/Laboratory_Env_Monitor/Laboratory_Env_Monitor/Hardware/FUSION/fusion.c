/* fusion.c：多传感器融合综合风险等级（评分 → 模糊综合评价 → 专家规则修正） */
#include "fusion.h"
#include "ai_monitor.h"     /* AI_SCORE_DEADBAND / AI_SCORE_GAIN / AI_SCORE_MIN_SIM */

/* 模块权重表 */
static const float s_weight[MODULE_NUM] =
{
    FUSION_W_TEMP_HUMI,   /* [0] MODULE_TEMP  温湿度 */
    FUSION_W_GAS,         /* [1] MODULE_GAS   气体   */
    FUSION_W_BODY,        /* [2] MODULE_BODY  人体   */
    FUSION_W_LIGHT        /* [3] MODULE_LIGHT 光强   */
};

/* 内部状态 */
/* 等级防抖状态：raw -> stable 需要持续一段时间 */
static AlarmLevel s_level_stable     = ALARM_NORMAL;
static AlarmLevel s_level_pending    = ALARM_NORMAL;
static uint32_t   s_level_pending_ms = 0;

/* 静态函数声明 */
static float Score_Temp(float temp, float trend_risk);
static float Score_Humi(float hum);
static float Score_Light(uint16_t light);
static float Score_SafeLevel(uint8_t lvl);
static uint8_t Level_From_Score(float score);

/* 融合模块初始化 */
void Fusion_Init(void)
{
    s_level_stable      = ALARM_NORMAL;
    s_level_pending     = ALARM_NORMAL;
    s_level_pending_ms  = 0;
}

/* 温度软肩：36°C 起给分，到 TEMP_MAX(40) 时给 TEMP_SOFT_SCORE 分。
 * 取 36 而不是 35：夏季无空调实验室 35°C 属常见工况，从 35 起分会给出
 * 一个常驻 8~12 分的假基线，与"关灯"(15 分)叠加后贴近 L2 门槛。
 * 取 40 分：40°C 时贡献 12 分(0.30x40)，45°C 时 90 分 -> 27 分刚好跨 L2；
 * 46°C 起封顶 30 分，不会在更高温度时反而压过气体因子的主导地位。 */
#define TEMP_SOFT_START   36.0f
#define TEMP_SOFT_SCORE   40.0f

/* 温度连续评分（实测越限 + 趋势预判，取较大者）
 *
 * 两个分量分工：
 *   实测 = 当前温度的证据，含"软肩"：36~40°C 线性给 0~40 分，越限后
 *          每 TEMP_SCORE_STEP(10°C) 再加满 100。软肩补的是原实现的空档——
 *          36~40°C 已有明显过热征兆却给 0 分，导致实测 45°C 的真实烘箱
 *          只拿 15 分，反而低于"26°C 时 0.5°C/min 正常回温"的 22.5 分。
 *   趋势 = 预判，未越限时补位。满偏 100 x 0.30 权重 = 30 分，
 *          risk >= 84 时单独可跨 L2 门槛(25)。这是 2026-11-13 D1 的
 *          有意决策：趋势的价值就是提前约 11 分钟，砍到 15 分等于废掉它。
 *          30 分 < L3 门槛(50)，所以趋势再强也够不到 L3/L4，
 *          最坏后果是一次 L2 提示（LED + 短鸣 + 一条告警入库）。
 *
 * 两者取 max 而不是相加：同一个物理量的两种证据，相加就是重复计分。 */
static float Score_Temp(float temp, float trend_risk)
{
    float dev = 0.0f, s = 0.0f;

    if (temp > TEMP_SOFT_START && temp <= TEMP_MAX)
    {
        /* 软肩段：36 -> 0 分，40 -> TEMP_SOFT_SCORE，线性 */
        s = (temp - TEMP_SOFT_START)
          * TEMP_SOFT_SCORE / (TEMP_MAX - TEMP_SOFT_START);
    }
    else if (temp > TEMP_MAX)
    {
        /* 越限段：从软肩终点继续累积，45°C -> 90 分（27 分融合，进 L2） */
        dev = temp - TEMP_MAX;
        s = TEMP_SOFT_SCORE + dev / TEMP_SCORE_STEP * 100.0f;
    }
    else if (temp < TEMP_MIN)
    {
        dev = TEMP_MIN - temp;
        s = dev / TEMP_SCORE_STEP * 100.0f;
    }

    if (s > 100.0f) s = 100.0f;

    return (trend_risk > s) ? trend_risk : s;
}

/* 湿度连续评分 */
static float Score_Humi(float hum)
{
    float dev, s;

    if (hum >= HUMI_MIN && hum <= HUMI_MAX)
        return 0.0f;

    dev = (hum < HUMI_MIN) ? (HUMI_MIN - hum) : (hum - HUMI_MAX);
    s = dev / HUMI_SCORE_STEP * 100.0f;
    return (s > 100.0f) ? 100.0f : s;
}

/* 光强连续评分：只看亮度，不看人体。
 * "没人时关灯不该算风险"这条约束由调用方按 LIGHT_NEED_BODY 门控，
 * 保持本函数是纯亮度映射，便于单独测试。 */
static float Score_Light(uint16_t light)
{
    if (light >= LIGHT_LOW)
        return 0.0f;
    if (light <= LIGHT_DARK_MAX)
        return 100.0f;

    return (LIGHT_LOW - light) * 100.0f / (LIGHT_LOW - LIGHT_DARK_MAX);
}

/* 开关量等级映射分数 */
static float Score_SafeLevel(uint8_t lvl)
{
    switch (lvl)
    {
        case ALARM_HINT:      return SAFE_SCORE_L2;   /* 提示 40 分 */
        case ALARM_WARN:      return SAFE_SCORE_L3;   /* 预警 70 分 */
        case ALARM_EMERGENCY: return SAFE_SCORE_L4;   /* 紧急 100 分 */
        default:              return 0.0f;            /* 正常 0 分 */
    }
}

/* 综合分映射等级 */
static uint8_t Level_From_Score(float score)
{
    if (score < FUSION_L2_BOUND) return ALARM_NORMAL;    /* <25  正常 */
    if (score < FUSION_L3_BOUND) return ALARM_HINT;      /* <50  提示 */
    if (score < FUSION_L4_BOUND) return ALARM_WARN;      /* <75  预警 */
    return ALARM_EMERGENCY;                              /* >=75 紧急 */
}

/* 等级防抖：避免分数在 25/50/75 附近每秒跳变。
 * 升级需要持续 3 秒，降级需要持续 5 秒；紧急等级立即生效，安全优先。 */
static AlarmLevel Fusion_LevelFilter(AlarmLevel raw, uint32_t now)
{
    uint32_t hold;

    if (raw == s_level_stable)
    {
        s_level_pending    = raw;
        s_level_pending_ms = now;
        return s_level_stable;
    }

    if (raw == ALARM_EMERGENCY)
    {
        s_level_stable     = raw;
        s_level_pending    = raw;
        s_level_pending_ms = now;
        return raw;
    }

    if (s_level_pending != raw)
    {
        s_level_pending    = raw;
        s_level_pending_ms = now;
        return s_level_stable;
    }

    hold = (raw > s_level_stable) ? FUSION_LEVEL_UP_MS : FUSION_LEVEL_DOWN_MS;
    if ((now - s_level_pending_ms) >= hold)
    {
        s_level_stable = raw;
    }
    return s_level_stable;
}

/* 融合主入口：评分 → 模糊综合评价 → 规则修正 */
void Fusion_Evaluate(float temp, float hum, uint16_t light, uint8_t body,
                     const uint8_t supp[MODULE_NUM],
                     const AlarmLevel lvl[MODULE_NUM],
                     FusionResult *out)
{
    uint8_t i;
    float   t_score, h_score, maxv;

    /* body 只作为"提前量"参与规则2b（有人 + 漏气 >=3 秒 -> 提前 L4）。
     * 规则2a 的漏气 >=8 秒封顶不看人体：无人房间同样要长鸣。
     * 人体不再参与单因素评分（权重已并入气体），也不再判级。 */

    if (out == NULL)
        return;

    /* AI 字段每次评估清零，只有 Fusion_ApplyAi() 才会置位 */
    out->ai_sim   = 0.0f;
    out->ai_valid = 0;

    /* 第1层：单因素评分 */

    /* 温湿度：连续量评分，抑制时清零 */
    if (supp[MODULE_TEMP])
    {
        t_score = 0.0f;
        h_score = 0.0f;
    }
    else
    {
        t_score = Score_Temp(temp, (float)AiMonitor_GetTrendRisk());
        h_score = Score_Humi(hum);
    }
    /* 温湿度合并分 */
		out->factor_score[MODULE_TEMP] = (t_score >h_score )? t_score :h_score;
    //out->factor_score[MODULE_TEMP] = t_score * FUSION_TEMP_MIX
                                  // + h_score * (1.0f - FUSION_TEMP_MIX);

    /* 光强评分：取"连续亮度分"与"光照告警等级分"的较大者。
     * 原实现只按 lux 连续值计分，导致 sensor_task 里"连续偏暗 3 秒判 HINT"
     * 这条判定完全进不了融合分（等级算了却没用）。
     * 现在 HINT(40 分) 也能参与，按 0.15 权重贡献 6 分。
     *
     * 2026-11-13 D3：LIGHT_NEED_BODY=1 时，无人场景两条分路都归零。
     * 无人关灯是夜间常态，不是风险信号；旧行为给满 100 x 0.15 = 15 分，
     * 是纯假基线，还会把气体 L4 的 55 分推成 70、再过规则4 乘 1.2 变 84。
     * "有人在暗处"才是这条因子要表达的风险，无人时它不该有读数。 */
    {
        float ls = supp[MODULE_LIGHT] ? 0.0f : Score_Light(light);
        float la = supp[MODULE_LIGHT] ? 0.0f
                                     : Score_SafeLevel((uint8_t)lvl[MODULE_LIGHT]);
#if LIGHT_NEED_BODY
        if (body == 0)
        {
            ls = 0.0f;
            la = 0.0f;
        }
#endif
        out->factor_score[MODULE_LIGHT] = (la > ls) ? la : ls;
    }

    /* 气体/人体：按告警等级映射 */
    out->factor_score[MODULE_GAS]  = Score_SafeLevel((uint8_t)lvl[MODULE_GAS]);
    out->factor_score[MODULE_BODY] = Score_SafeLevel((uint8_t)lvl[MODULE_BODY]);

    /* 第2层：模糊综合评价（加权合成） */
    out->score = 0.0f;
    for (i = 0; i < MODULE_NUM; i++)
        out->score += s_weight[i] * out->factor_score[i];
    if (out->score > 100.0f)
        out->score = 100.0f;

    /* 主导风险源：权重×得分最大者 */
    out->dominant = MODULE_TEMP;
    maxv = -1.0f;
    for (i = 0; i < MODULE_NUM; i++)
    {
        float v = s_weight[i] * out->factor_score[i];
        if (v > maxv)
        {
            maxv = v;
            out->dominant = i;
        }
    }

    /* 等级初算 */
    out->level = Level_From_Score(out->score);

    /* 第3层：专家规则修正 */

    /* 规则3（无人值守期突现人员）已删除：实验室人员进出属常态，
     * 该规则实测每天误触发，且与"入侵"语义不符。原加分逻辑一并移除。 */

    /* 规则4：持续超时 → 综合分 ×1.2
     *
     * 2026-11-13 D3：RULE_PERSIST_EN 置 0，本规则默认不生效。
     *
     * 关闭原因（重复计分）：
     *   气体模块自己就按时长判级（<3s 提示 / 3~8s 预警 / >=8s 紧急，
     *   见 SAFE_L2_TIME / SAFE_L3_TIME），"漏气持续 8 秒"这件事已经被
     *   用掉一次——气体因子给出 100 分 x 0.55 = 55 分。规则4 再因为
     *   "气体处于 L4"把整个综合分乘 1.2，等于同一段时长算了第二遍。
     *
     * 关闭原因（放大无关因子）：
     *   ×1.2 作用在总分上，不区分分数的来源。无人关灯的光照分 15 分
     *   也被一起放大，于是 55 + 15 = 70 -> 84，越过了 75。也就是说
     *   长鸣与否由"灯开着还是关着"决定，跟气体浓度无关。
     *
     * 关闭后的行为变化：
     *   - 分级不受影响。气体 L4 由规则2a 直接封顶到 L4，与是否乘 1.2 无关。
     *   - Fuse 显示值下降。无人 + 灯灭 + 气体 L4 从 84 变成 55，
     *     数值更贴近"气体一项满偏 55 分"的真实构成，不再虚高。
     *
     * 保留代码是为了让 RULE_PERSIST_EN 保持可回滚：置 1 即恢复旧行为，
     * 便于做一轮 A/B 对比再定最终形态。 */
    if (RULE_PERSIST_EN)
    {
        if (lvl[MODULE_GAS] == ALARM_EMERGENCY || lvl[MODULE_BODY] == ALARM_EMERGENCY)
        {
            out->score *= 1.2f;
            if (out->score > 100.0f) out->score = 100.0f;
            out->level = Level_From_Score(out->score);
        }
    }

    /* 规则1：火灾隐患 → 直接紧急 */
    if (RULE_FIRE_EN)
    {
        if (lvl[MODULE_GAS] >= ALARM_WARN &&
            (temp < TEMP_MIN || temp > TEMP_MAX))
        {
            out->score = (out->score > FUSION_L4_BOUND) ? out->score : FUSION_L4_BOUND;
            out->level = ALARM_EMERGENCY;
        }
    }

    /* 规则2：人员危险 → 封顶紧急
     * 气体模块的判级是 <3s 提示 / 3~8s 预警 / >=8s 紧急（SAFE_L2_TIME / SAFE_L3_TIME），
     * 所以 lvl[MODULE_GAS] == ALARM_EMERGENCY 正好等价于"漏气超过 8 秒"。
     *
     * 2026-11-13 D2 决议：拆成 2a / 2b 两条，把"有没有人"从"能不能长鸣"
     * 改为"能不能提前长鸣"。
     *
     * 2a 漏气 >=8 秒 → 直接 L4，不看人体。
     *    这是直接测量 + 时长确认，不需要别的因子背书。原实现要求 body != 0
     *    才封顶，等于"无人房间漏气只能短鸣"——实验室夜间无人恰恰是最危险
     *    工况。而且原条件拦不住分数路径：无人关灯时 光照 15 + 气体 55 = 70，
     *    再过规则4 的 x1.2 就是 84，早已越过 75。也就是说长鸣与否实际由
     *    "灯开着还是关着"决定，跟气体浓度无关。2a 把这条路径封死。
     *
     * 2b 有人 + 漏气 >=3 秒 → 提前封顶 L4。
     *    人在场的价值体现在"更早"，而不是"许可"：有人时 3 秒就长鸣，
     *    无人时按气体自身 8 秒判定。 */
    if (RULE_PERSON_DANGER)
    {
        if (lvl[MODULE_GAS] == ALARM_EMERGENCY)
        {
            out->score = (out->score > FUSION_L4_BOUND) ? out->score : FUSION_L4_BOUND;
            out->level = ALARM_EMERGENCY;
        }
        else if (body != 0 && lvl[MODULE_GAS] >= ALARM_WARN)
        {
            out->score = (out->score > FUSION_L4_BOUND) ? out->score : FUSION_L4_BOUND;
            out->level = ALARM_EMERGENCY;
        }
    }
}

/* ---------- AI 辅助评分 ----------
 * 设计原则：
 *   1) 只加不减：AI 只能让分数变高，不会把规则判出的高风险拉低；
 *   2) 有死区：偏离度小于 AI_SCORE_DEADBAND 时不加分，避免噪声累积；
 *   3) 有上限：规则未到"提示"时，AI 最多把等级抬到"提示"，不独立报警。
 *
 * V4 注：移除了 V3 的 "relief" 分支（AI 判断非常正常时减分）。
 * 新模型 FISVDD 输出经滑动窗口转为 sim，粒度 10% 步进，
 * relief 在粗粒度 sim 下效果微弱且增加不必要的复杂度。
 * 若后续换回连续相似度模型可恢复该分支。 */
void Fusion_ApplyAi(FusionResult *out, float sim, uint8_t valid)
{
    AlarmLevel base;
    float      dev;
    float      boost;

    if (out == NULL)
        return;

    out->ai_sim   = 0.0f;
    out->ai_valid = 0;

    /* 规则层原始等级：模型加分以它为基准，此后不再改写 */
    base = (AlarmLevel)out->level;

    /* 温度趋势不在此处加分——它已通过 Fusion_Evaluate() 的
     * Score_Temp(temp, GetTrendRisk()) 进入温度因子：
     * 满偏 100 x 权重 0.30 = 30 分，risk >= 84 时单独可跨 L2 门槛(25)。
     *
     * 注意这里不是 0.15。旧注释按已删除的加权平均实现
     * (FUSION_TEMP_MIX=0.5，有效权重 0.15) 写，现实现是 max()，
     * 趋势满值直接透传，系数就是 0.30。2026-11-13 D1 决议保留这个能力：
     * 趋势的全部价值是"提前约 11 分钟"，压回 15 分等于把提前量砍半。
     * 30 分 < L3 门槛(50)，趋势永远够不到 L3/L4。
     *
     * 此处原先还有一笔 tr * AI_TREND_BOOST_MAX / 100 = 0.14 的独立加分，
     * 与温度因子里的趋势分量重复计算了同一个物理量，使趋势被计两遍。
     * 已于 2026-11-12 移除，详见 回滚说明_1112_1940_只留两个补丁.md。 */

    if (!valid)
        return;

    /* sim 低于下限时**钳位**，而不是丢弃。
     * 原实现是 return：当 sim 跌到 10 以下（模型明确判离群、
     * 异常最严重）时加分变成 0，而 sim 在 10~75 的过渡段反而有分，
     * 加分曲线非单调，实测会出现一秒内掉6.4 分的断崖。
     * 钳位后加分随偏离度单调递增，最大仍为
     * (100-25-10)*AI_SCORE_GAIN = 6.5，不会突破"不能独立报警"。 */
    if (sim < AI_SCORE_MIN_SIM)
        sim = AI_SCORE_MIN_SIM;

    if (sim < 0.0f)   sim = 0.0f;
    if (sim > 100.0f) sim = 100.0f;

    dev  = 100.0f - sim;

    if (dev > AI_SCORE_DEADBAND)   /* AI 检测到偏离：加分 */
    {
        boost = (dev - AI_SCORE_DEADBAND) * AI_SCORE_GAIN;
        out->score += boost;
        if (out->score > 100.0f)
            out->score = 100.0f;
        out->level = Level_From_Score(out->score);
    }
    /* else: 中间地带，不操作（V4：无 relief，保留只加不减） */

    /* 加分分支内已做只升不降，这里补 base 兜底 */
    if (base >= ALARM_HINT && out->level < base)
        out->level = base;

    /* 规则未到提示时，AI 最多抬到提示，不能独立升级报警 */
    if (base < ALARM_HINT && out->level > ALARM_HINT)
        out->level = ALARM_HINT;

}

/* 统一等级防抖入口。
 * 调用顺序：Fusion_Evaluate() -> Fusion_ApplyAi() -> Fusion_Stabilize()。
 * 放在最后只调用一次，避免两次滤波互相重置 pending 计时。 */
void Fusion_Stabilize(FusionResult *out)
{
    AlarmLevel raw;

    if (out == NULL)
        return;

    raw = (AlarmLevel)out->level;

    /* L2 迟滞：进入仍用 FUSION_L2_BOUND(25)，退出要求分数再低 FUSION_L2_HYST。
     * 背景：缓慢升温时综合分会在 25 分附近长时间横盘，3/5 秒防抖挡不住，
     * 实测 300 秒内等级翻转 5 次。加迟滞后退回 1 次。
     * 只有"已在提示及以上"时才启用，正常状态下提升门槛不变。 */
    if (raw == ALARM_NORMAL &&
        s_level_stable >= ALARM_HINT &&
        out->score >= (FUSION_L2_BOUND - FUSION_L2_HYST))
    {
        raw = ALARM_HINT;
    }

    out->level = (uint8_t)Fusion_LevelFilter(raw, HAL_GetTick());
}
