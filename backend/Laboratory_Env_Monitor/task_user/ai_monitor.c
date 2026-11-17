/* ai_monitor.c —— 边缘 AI 模块：离群检测 + 温度趋势预判
 *
 * 完全自包含：状态机 + 特征提取 + 推理 + 平滑 + 相似度输出。
 * 不依赖融合层：只产出相似度 sim(0~100)，
 * 由 sensor_task 通过 AiMonitor_GetSim() 取出并交给 Fusion_ApplyAi 融合。
 *
 * 两条通道并联，最终 sim 取二者较小的那个：
 *
 *   ① 模型通道（判"现在是"）：
 *      neai_outlier(二元) -> N/K 多数表决(抑边界抖动) -> 双向线性计数
 *      -> 偏离档位 n(0~AI_HOLD_F) -> sim_model = 100 - 100*n/AI_HOLD_F
 *      局限：FISVDD 只看当前这一帧像不像训练时的正常数据。温度从 26.7 爬到
 *      28.4 这种"还没越界但正在爬"的过程，它一路判正常、sim 恒 100；等它真
 *      掉下来时规则层早就报警了，等于没有提前量。
 *
 *   ② 趋势通道（判"要去哪"）：
 *      30 点温度线性回归 -> 斜率 slope + 线性度 R2 + 最大相邻帧跳变
 *      三判据：速率 > 阈值、R2 >= 门槛、无阶跃（跳变 <= 0.25C），
 *      近端"仍在涨"用 slope x K 外推判断（不用 5 帧实测差分，信噪比太低）
 *      -> eta = (TEMP_MAX - temp)/rate，即"还有几分钟到上限"
 *      -> risk = (ETA_SLOW - eta)/(ETA_SLOW - ETA_FAST)*100，钳在 0~100
 *      -> 输出级再过三道：释放去抖 3 帧 + 上升限速 15/帧 + 下降限速 10/帧
 *      -> sim_trend = 100 - risk
 *      用 eta 而不是纯速率，是因为速率恒定时温度仍在积累，risk 必须随
 *      "离 40C 还有多远"单调上升，否则慢升温会给出恒定且偏低的分数。
 *
 * 显示用 sim = min(sim_model, sim_trend)；计分用 sim_model。
 * 趋势另经 AiMonitor_GetTrendRisk() 单独进入融合层，避免经 sim 二次计分。 */

#include "ai_monitor.h"
#include "NanoEdgeAI.h"
#include "stm32l4xx_hal.h"   /* HAL_GetTick */
#include "handle_config.h"   /* TEMP_MAX：趋势通道算 eta 用 */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 私有常量                                                           */
/* ------------------------------------------------------------------ */

#define AI_INPUT_DIM        (NEAI_INPUT_SIGNAL_LENGTH * NEAI_INPUT_AXIS_NUMBER)

/* 编译期校验：模型必须是 3 维输入 */
typedef char ai_input_dim_check_t[(AI_INPUT_DIM == 3) ? 1 : -1];

/* 多数表决窗口：最近 AI_VOTE_N 帧中至少 AI_VOTE_K 帧判离群，才算"偏离"。
 * 原来用 3 点中值，只能挡单帧误报；raw 一旦在 0/1 之间交替（占空比 50%），
 * 中值输出仍是 0/1 交替，sim 会永久卡在 50 附近抖动、停不下来。
 * 7/10 表决下 50% 占空比永远凑不到 7 帧，sim 稳定停在 100；
 * 真实异常是 100% 占空比，7 秒后照样触发。 */
#define AI_VOTE_N           10u         /* 表决窗口帧数                       */
#define AI_VOTE_K           7u          /* 判定偏离所需的最少离群帧数         */
/* 双向线性计数：偏离档位直接对应"连续异常秒数"。
 * 原来用一阶指数平滑(α=0.2)，问题在"渐近"：从满值回到 0 要走 26 帧，
 * 尾巴上全是 96/97/98/99，读数看着像卡住不动，也读不出异常严重程度。
 * 改成线性档位后：下降 15 帧到底 / 恢复每帧退 2 档≈8 秒回满。 */
#define AI_HOLD_F           15u         /* 100 -> 0 所需连续离群帧数      */
#define AI_RELEASE_STEP     2u          /* 恢复时每帧回退的档位数          */
#define AI_FAULT_RETRY_MS   30000u      /* 库故障重试间隔                     */


#define AI_TREND_WIN        30u         /* 回归窗口点数（=秒数）              */
#define AI_TREND_RATE_MIN   0.005f      /* 斜率死区（°C/s）= 0.30°C/min        */
#define AI_TREND_R2_MIN     0.80f       /* 线性度门槛：区分持续升温与阶跃        */
#define AI_TREND_NEAR_K     5u          /* 近端判据窗口（最近 K 帧）            */
#define AI_TREND_NEAR_MIN   0.02f       /* 近端涨幅门槛（°C），低于此视为已停止  */
#define AI_TREND_ETA_SLOW   40.0f       /* eta >= 此值视为无风险（分钟）        */
#define AI_TREND_ETA_FAST   5.0f        /* eta <= 此值视为满偏（分钟）          */
#define AI_TREND_STEP_MAX   15.0f       /* risk 单帧最大上升量，抑制边界抖动     */
#define AI_TREND_FALL_MAX   10.0f       /* risk 单帧最大下降量，平滑回落         */
#define AI_TREND_RELEASE_N  3u          /* 判据失效后保持帧数（释放去抖），≈3 秒  */
#define AI_TREND_JUMP_MAX   0.25f       /* 相邻帧跳变阈值 °C，超过判为阶跃/毛刺   */

#define AI_TREND_SLOPE_ALPHA 0.10f

/* ------------------------------------------------------------------ */
/* 私有状态                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ST_OFF = 0,
    ST_RUN,
    ST_FAULT
} AiState;

static AiState  s_state     = ST_OFF;
static uint32_t s_retry_ms  = 0;

static float    s_in[AI_INPUT_DIM];     /* 模型输入（3 维原始量）            */
static uint8_t  s_raw       = 0;        /* neai_outlier 二元输出              */

static uint8_t  s_vote[AI_VOTE_N] = {0};/* 最近 N 帧离群标志环形缓冲           */
static uint8_t  s_vote_i    = 0;
static uint8_t  s_vote_cnt  = 0;        /* 当前窗口内离群帧数（调试用）        */

static uint8_t  s_run       = 0;        /* 当前偏离档位 0~AI_HOLD_F             */
static uint8_t  s_sim_model = 100u;     /* 模型通道相似度 0~100                */
static uint8_t  s_sim       = AI_SIM_INVALID;
static uint16_t s_dbg       = 0;

/* ---- 趋势通道状态 ---- */
static int16_t  s_hist[AI_TREND_WIN];   /* 温度历史 ×100（0.01°C），环形缓冲   */
static uint8_t  s_hist_i    = 0;        /* 写指针，同时也指向"最旧一格"         */
static uint8_t  s_hist_cnt  = 0;        /* 已填充点数，未满 AI_TREND_WIN 不算趋势 */
static float    s_risk_f    = 0.0f;     /* 风险值浮点中间量（限速用）           */
static uint8_t  s_trend_risk = 0u;      /* 趋势风险 0~100，对外计分用           */
static uint8_t  s_sim_trend = 100u;     /* = 100 - risk，显示相似度 0~100      */
static float    s_slope     = 0.0f;     /* 最近一次回归斜率（°C/s），调试用      */
static float    s_slope_s   = 0.0f;     /* 斜率 EMA（°C/s），只用于 eta 映射     */
static uint8_t  s_slope_ok  = 0u;       /* 1=EMA 已对齐首帧，0=待对齐            */
static float    s_r2        = 0.0f;     /* 最近一次回归决定系数，线性度判据      */
static uint8_t  s_rel_cnt   = 0u;       /* 判据连续失效帧数（释放去抖）          */
static float    s_jmp_max   = 0.0f;     /* 窗口内最大相邻帧跳变（°C），调试用    */

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static uint8_t sensor_ok(float t, float h, float light)
{
    if (t == 0.0f && h == 0.0f) return 0;
    if (t < -40.0f || t > 125.0f) return 0;
    if (h < 0.0f || h > 100.0f) return 0;
    if (light < 0.0f) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 趋势通道                                                           */
/* ------------------------------------------------------------------ */

/* 推入一个温度样本，做线性回归 → 线性度检验 → 外推 → 与基线比较，得出 sim_trend。
 * 统计量用 int32 定点累加（温度 ×100 存 int16，30 点共 60 字节 RAM），
 * 只有斜率和 R² 的收尾是浮点，每秒一次、两次 30 点遍历，开销可忽略。 */
static void trend_step(float temp)
{
    int32_t Sx = 0, Sxx = 0, Sy = 0, Sxy = 0;
    int32_t num, den;
    float   risk;

    /* ---- 入队 ---- */
    s_hist[s_hist_i] = (int16_t)(temp * 100.0f);
    s_hist_i++;
    if (s_hist_i >= AI_TREND_WIN) s_hist_i = 0;
    if (s_hist_cnt < AI_TREND_WIN) s_hist_cnt++;

    /* ---- 窗口未填满（开机前 30 秒）：没有趋势可算 ---- */
    if (s_hist_cnt < AI_TREND_WIN) {
        s_slope   = 0.0f;
        s_r2      = 0.0f;
        s_jmp_max = 0.0f;
        s_rel_cnt = 0u;
        return;
    }

    /* s_hist_i 此时指向最旧一格，按时间顺序 i=0 最旧、i=29 最新累加 */
    {
        uint8_t i;
        for (i = 0; i < AI_TREND_WIN; i++) {
            uint8_t k = (uint8_t)(s_hist_i + i);
            int32_t y;
            if (k >= AI_TREND_WIN) k = (uint8_t)(k - AI_TREND_WIN);
            y = (int32_t)s_hist[k];
            Sx  += (int32_t)i;
            Sxx += (int32_t)i * (int32_t)i;
            Sy  += y;
            Sxy += (int32_t)i * y;
        }
    }

    /* 最小二乘斜率：slope = (n·Σxy − Σx·Σy) / (n·Σx² − (Σx)²) */
    num = (int32_t)AI_TREND_WIN * Sxy - Sx * Sy;
    den = (int32_t)AI_TREND_WIN * Sxx - Sx * Sx;
    s_slope = (den != 0) ? ((float)num / (float)den) / 100.0f : 0.0f;   /* °C/s */

    /* 斜率 EMA：首帧直接对齐，否则从 0 慢慢爬会让 risk 开机后多等 1 分钟才起。
     * 之后按 α 平滑，供 eta 映射使用；判据仍看 s_slope 瞬时值。 */
    if (!s_slope_ok) {
        s_slope_s  = s_slope;
        s_slope_ok = 1u;
    } else {
        s_slope_s += AI_TREND_SLOPE_ALPHA * (s_slope - s_slope_s);
    }

    /* ---- 线性度检验 R²（第二遍遍历算残差）+ 相邻帧跳变 ----
     * 一次阶跃（有人靠近、热源移入）在窗口里同样能拟出正斜率，外推 60 秒
     * 会得到荒谬的高温：实测把 26.7→28.4 的阶跃当成 2.9°C/min 的持续升温，
     * 误报 16 秒。R² 能区分二者——真实斜坡 ≈0.99，阶跃+平台 <0.76。
     * 同一遍里顺带取"最大相邻帧跳变"：阶跃在相邻样本上是突变，在拟合上
     * 只是斜率，用跳变做二次判据比靠 5 帧差分可靠（见下方三判据注释）。 */
    {
        float   ybar = (float)Sy / (float)AI_TREND_WIN / 100.0f;
        float   b    = s_slope;
        float   a    = ybar - b * (float)(AI_TREND_WIN - 1u) * 0.5f;
        float   ss_tot = 0.0f, ss_res = 0.0f;
        float   jmp  = 0.0f;
        int16_t prev = 0;
        uint8_t i;

        for (i = 0; i < AI_TREND_WIN; i++) {
            uint8_t k = (uint8_t)(s_hist_i + i);
            float   y, fit, d;
            if (k >= AI_TREND_WIN) k = (uint8_t)(k - AI_TREND_WIN);
            y   = (float)s_hist[k] / 100.0f;
            fit = a + b * (float)i;
            d = y - ybar;  ss_tot += d * d;
            d = y - fit;   ss_res += d * d;
            if (i > 0u) {
                int16_t dj = (int16_t)(s_hist[k] - prev);
                if (dj < 0) dj = (int16_t)-dj;
                if (dj > jmp) jmp = dj;
            }
            prev = s_hist[k];
        }
        s_r2      = (ss_tot > 1e-6f) ? (1.0f - ss_res / ss_tot) : 0.0f;
        s_jmp_max = jmp / 100.0f;
    }

   
    risk = 0.0f;
    if (s_slope > AI_TREND_RATE_MIN &&
        s_r2 >= AI_TREND_R2_MIN &&
        s_jmp_max <= AI_TREND_JUMP_MAX)
    {
        
        float near_fit = s_slope * (float)AI_TREND_NEAR_K;

        if (near_fit >= AI_TREND_NEAR_MIN) {          /* 事件仍在进行 */
            float rate = s_slope_s * 60.0f;           /* °C/min，平滑后 */
            float eta;

            /* 平滑值可能在事件刚起或释放边缘贴近 0，钳一下避免 eta 出现 inf。 */
            if (rate < 0.01f) rate = 0.01f;
            eta = (TEMP_MAX - temp) / rate;           /* 还有几分钟到上限 */

            if (eta < AI_TREND_ETA_SLOW) {
                risk = (AI_TREND_ETA_SLOW - eta)
                     / (AI_TREND_ETA_SLOW - AI_TREND_ETA_FAST) * 100.0f;
                if (risk > 100.0f) risk = 100.0f;
                if (risk < 0.0f)   risk = 0.0f;
            }
        }
    }

    
    if (risk > 0.0f) {
        s_rel_cnt = 0u;
    } else if (s_rel_cnt < AI_TREND_RELEASE_N) {
        s_rel_cnt++;
        if (s_rel_cnt < AI_TREND_RELEASE_N)
            risk = s_risk_f;                          /* 保持上一帧，不归零 */
    }


    if (risk > s_risk_f + AI_TREND_STEP_MAX) risk = s_risk_f + AI_TREND_STEP_MAX;
    if (risk < s_risk_f - AI_TREND_FALL_MAX) risk = s_risk_f - AI_TREND_FALL_MAX;
    if (risk < 0.0f)   risk = 0.0f;
    if (risk > 100.0f) risk = 100.0f;

    s_risk_f     = risk;
    s_trend_risk = (uint8_t)(risk + 0.5f);
    s_sim_trend  = (uint8_t)(100u - s_trend_risk);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void AiMonitor_Init(void)
{
    enum neai_state st;

    memset(s_in, 0, sizeof(s_in));
    s_state   = ST_OFF;
    s_retry_ms = 0;
    s_raw     = 0;
    s_dbg     = 0;
    memset(s_vote, 0, sizeof(s_vote));
    s_vote_i   = 0;
    s_vote_cnt = 0;
    s_run      = 0;
    s_sim_model = 100u;
    s_sim      = AI_SIM_INVALID;

    /* 趋势通道复位 */
    memset(s_hist, 0, sizeof(s_hist));
    s_hist_i      = 0;
    s_hist_cnt    = 0;
    s_risk_f      = 0.0f;
    s_trend_risk  = 0u;
    s_sim_trend   = 100u;
    s_slope       = 0.0f;
    s_slope_s     = 0.0f;
    s_slope_ok    = 0u;
    s_r2          = 0.0f;
    s_rel_cnt     = 0u;
    s_jmp_max     = 0.0f;

    st = neai_outlier_init();
    if (st != NEAI_OK) {
        s_state = ST_FAULT;
        printf("[AI] init FAIL %d\r\n", (int)st);
        return;
    }
    s_state = ST_RUN;
    printf("[AI] FISVDD ready, axis=%d\r\n", neai_get_axis_number());
}

void AiMonitor_Process(float temp, float hum, float light)
{
    enum neai_state st;
    uint32_t now = HAL_GetTick();

    /* ---- 故障恢复：每 30 秒重试一次 init ---- */
    if (s_state == ST_OFF || s_state == ST_FAULT) {
        if (s_state == ST_FAULT && (now - s_retry_ms) < AI_FAULT_RETRY_MS)
            return;
        if (neai_outlier_init() != NEAI_OK) {
            s_state    = ST_FAULT;
            s_retry_ms = now;
            return;
        }
        s_state   = ST_RUN;
        s_run     = 0;
        memset(s_vote, 0, sizeof(s_vote));
        s_vote_i   = 0;
        s_vote_cnt = 0;
        s_sim_model = 100u;
        s_sim     = AI_SIM_INVALID;
        s_dbg     = 0;
    }

    if (s_state != ST_RUN) return;

    
    if (!sensor_ok(temp, hum, light)) {
        s_risk_f     = 0.0f;
        s_trend_risk = 0u;
        s_sim_trend  = 100u;
        s_slope      = 0.0f;
        s_slope_s    = 0.0f;
        s_slope_ok   = 0u;
        s_r2         = 0.0f;
        s_rel_cnt    = 0u;
        s_jmp_max    = 0.0f;
        s_hist_cnt   = 0;
        s_hist_i     = 0;
        return;
    }

    /* ---- 趋势通道：必须排在 sensor_ok 之后，只在数据可信时推进 ---- */
    trend_step(temp);

    /* ---- 推理 ---- */
    s_in[0] = temp;
    s_in[1] = hum;
    s_in[2] = light;
    st = neai_outlier(s_in, &s_raw);
    if (st != NEAI_OK) {
        s_state    = ST_FAULT;
        s_retry_ms = now;
        s_sim      = AI_SIM_INVALID;
        printf("[AI] detect err %d\r\n", (int)st);
        return;
    }

    /* ---- 多数表决：窗口内离群帧数达到 K 才算偏离 ---- */
    s_vote[s_vote_i] = s_raw;
    s_vote_i = (uint8_t)((s_vote_i + 1u) % AI_VOTE_N);
    {
        uint8_t vi;
        uint8_t cnt = 0;
        uint8_t majority;

        for (vi = 0; vi < AI_VOTE_N; vi++)
        {
            if (s_vote[vi] != 0u) cnt++;
        }
        s_vote_cnt = cnt;
        majority   = (cnt >= AI_VOTE_K) ? 1u : 0u;

        /* ---- 双向线性计数 → 连续偏离度 ---- */
        if (majority) {
            if (s_run < AI_HOLD_F) s_run++;
        } else {
            s_run = (s_run > AI_RELEASE_STEP)
                  ? (uint8_t)(s_run - AI_RELEASE_STEP) : 0u;
        }
    }

    /* ---- 模型通道相似度 = 正常概率，0~100 ---- */
    s_sim_model = (uint8_t)(100u - (100u * (uint16_t)s_run + AI_HOLD_F / 2u) / AI_HOLD_F);

    
    s_sim = (s_sim_model < s_sim_trend) ? s_sim_model : s_sim_trend;

    
    s_dbg++;
    if ((s_dbg % 10u) == 0u) {
        printf("[AI] in=[%.1f %.1f %.0f] raw=%u vote=%u/%u model=%u trend=%u risk=%u sim=%u r=%+.2fC/min R2=%.2f jmp=%.2f\r\n",
               temp, hum, light, (unsigned)s_raw,
               (unsigned)s_vote_cnt, (unsigned)AI_VOTE_N,
               (unsigned)s_sim_model, (unsigned)s_sim_trend,
               (unsigned)s_trend_risk, (unsigned)s_sim,
               s_slope * 60.0f, s_r2, s_jmp_max);
    }
}

uint8_t AiMonitor_GetSim(void)
{
    return (s_state == ST_RUN) ? s_sim : AI_SIM_INVALID;
}

uint8_t AiMonitor_GetModelSim(void)
{
    return (s_state == ST_RUN) ? s_sim_model : AI_SIM_INVALID;
}

uint8_t AiMonitor_GetTrendSim(void)
{
    return (s_state == ST_RUN) ? s_sim_trend : AI_SIM_INVALID;
}


uint8_t AiMonitor_GetTrendRisk(void)
{
    return (s_state == ST_RUN) ? s_trend_risk : 0u;
}

const char *AiMonitor_GetStateName(void)
{
    switch (s_state) {
        case ST_RUN:   return "RUN";
        case ST_FAULT: return "ERR";
        default:       return "OFF";
    }
}
