/* sensor_task.c：四传感器融合监测任务（采样、告警、融合、执行器、LCD 显示） */
#include "sensor_task.h"
#include "gpio_api.h"        /* led_open/close/toggle, beep_open/close, keyscan */
#include "lcd.h"             /* LCD_Init, LCD_Fill, lcddev                     */
#include "gui.h"             /* Gui_StrCenter, Show_Str                         */
#include "stdio.h"
#include "string.h"
#include "fusion.h"          /* Fusion_Init / Fusion_Evaluate / FusionResult    */
#include "as312.h"           /* AS312_Init / Infrared_Read_State                */
#include "sht30.h"           /* SHT30_GPIO_Init / SHT30_read_result             */
#include "bh1750.h"          /* Init_BH1750 / BH1750_Handle                     */
#include "mq_5.h"            /* MQ5_Init / MQ5_Read_State                       */
#include "touch_key.h"       /* Touch_Key_Init / Touch_Key_State                */
#include "LD3320.h"          /* LD3320_Init / PlayDemoSound_Once / ld3320_Struct */
#include "net.h"             /* Get_Net_Module_State / Is_Net_Module_Connected  */
#include "ai_monitor.h"      /* 边缘 AI 离群检测（一行调用） */

UINT32 g_SensorTskHandle;   /* sensor task handle (LiteOS)                       */

/* ---- LED pin table, indexed by module id (MODULE_TEMP/GAS/BODY/LIGHT) ---- */
static const uint16_t s_led_pin[MODULE_NUM] =
{
    LED1_PORT,   /* MODULE_TEMP  -> PA4 */
    LED2_PORT,   /* MODULE_GAS   -> PA5 */
    LED3_PORT,   /* MODULE_BODY  -> PA6 */
    LED4_PORT    /* MODULE_LIGHT -> PA7 */
};

/* ---- Sensor data ---- */
static uint8_t  s_body  = 0;      /* 1 = someone, 0 = no one                    */
static uint8_t  s_gas   = 1;      /* 1 = safe,    0 = gas detected              */
static float    s_temp  = 0.0f;
static float    s_hum   = 0.0f;
static uint16_t s_light = 0;      /* lux                                          */

/* ---- 温度趋势（10 秒温升，环形缓冲 + 两端平均差分）---- */
#define TEMP_HIST_LEN   10              /* 窗口长度：10 个采样点（10 秒）   */
#define TEMP_AVG_WIN    3               /* 两端各取 3 点平均，抑制毛刺      */
static float   s_temp_hist[TEMP_HIST_LEN] = {0};
static uint8_t s_temp_hist_idx  = 0;    /* 环形缓冲写指针（指向最旧一格）   */
static uint8_t s_temp_hist_fill = 0;    /* 已填充计数（前 10 秒只填不算）    */
static float   s_temp_rise      = 0.0f; /* 温升速率 = 当前段平均 - 10 秒前段平均 */


#define AI_TREND_TEST_EN     0         /* 1=启用自检斜坡；验证完改 0       */
#define AI_TREND_TEST_DELAY  45000u    /* 上电后等待 ms，需 > 30s 让窗口填满 */
#define AI_TREND_TEST_RATE   0.5f      /* 斜坡速率 °C/min（改 0.5 复现慢升温）*/
#define AI_TREND_TEST_SPAN   4.0f      /* 最大抬升 °C，到 TEMP_MAX-1 提前停   */

#if AI_TREND_TEST_EN
static float    s_tt_ofs    = 0.0f;    /* 当前叠加偏置 °C                  */
static uint32_t s_tt_last_t = 0;       /* 上次积分 tick（增量积分用）       */
static uint8_t  s_tt_state  = 0;       /* 0=等待 1=爬升 2=已停             */
static uint8_t  s_tt_ds     = 0xFFu;   /* 上次打印的 10 秒档位，用于节流    */

static float AiTrendTest_Offset(float temp, float hum)
{
    uint32_t now = HAL_GetTick();

    /* 传感器不可信时不推进。
     * 若仍按绝对时间算偏置，掉线那几分钟会在空白期"空转"，
     * 恢复瞬间偏置已经爬到几度，趋势通道会立刻看到一次虚假阶跃。
     * 注意不能只判 temp：掉线时工程把温湿度一起清 0，而 0 仍是合法温度，
     * 必须和 ai_monitor 的 sensor_ok() 用同一组合判据。 */
    if ((temp == 0.0f && hum == 0.0f) || temp < -10.0f || temp > 60.0f)
    {
        s_tt_last_t = now;
        return 0.0f;
    }

    if (s_tt_state == 0u)
    {
        if (now < AI_TREND_TEST_DELAY)
        {
            s_tt_last_t = now;
            return 0.0f;
        }
        s_tt_last_t = now;
        s_tt_state  = 1u;
        s_tt_ds     = 0xFFu;
        printf("[TTEST] begin rate=%.2f C/min span=%.1f C\r\n",
               AI_TREND_TEST_RATE, AI_TREND_TEST_SPAN);
    }

    if (s_tt_state == 1u)
    {
        uint32_t dt = now - s_tt_last_t;

        if (dt > 5000u) dt = 5000u;     /* 任务被长时间阻塞时，别一次补太多 */
        s_tt_ofs += AI_TREND_TEST_RATE * (float)dt / 60000.0f;

        if (s_tt_ofs >= AI_TREND_TEST_SPAN ||
            (temp + s_tt_ofs) >= (TEMP_MAX - 1.0f))
        {
            if (s_tt_ofs > AI_TREND_TEST_SPAN)
                s_tt_ofs = AI_TREND_TEST_SPAN;
            s_tt_state = 2u;
            printf("[TTEST] ramp done, hold +%.2f C "
                   "(expect risk -> 0 within 10~15s, monotonic)\r\n", s_tt_ofs);
        }
    }
    s_tt_last_t = now;

    /* 每 10 秒等效运行时间打一行，便于和 [AI] 的 risk/R2 对齐时间轴。
     * 用偏置反算秒数而不是 now-t0，暂停期间才不会显示成"跑过了"。 */
    {
        uint32_t eqs = (AI_TREND_TEST_RATE > 0.0f)
                     ? (uint32_t)(s_tt_ofs / AI_TREND_TEST_RATE * 60.0f)
                     : 0u;
        uint8_t  ds  = (uint8_t)(eqs / 10u);

        if (s_tt_state != 0u && ds != s_tt_ds)
        {
            s_tt_ds = ds;
            printf("[TTEST] t=%lus ofs=+%.2fC virt=%.1fC\r\n",
                   (unsigned long)eqs, s_tt_ofs, temp + s_tt_ofs);
        }
    }

    return s_tt_ofs;
}
#endif

/* ---- Per-module alarm state ---- */
static AlarmLevel s_alarm[MODULE_NUM];     /* current alarm level per module     */
static uint8_t    s_suppress[MODULE_NUM];  /* 1 = alarm suppressed by long-press */

/* ---- Safety-module timing (duration-based escalation) ---- */
static uint32_t s_gas_start  = 0;   /* tick when gas first detected              */
static uint32_t s_temp_bad_start  = 0;  /* tick when temp/humi first out of range */
static uint32_t s_light_bad_start = 0;  /* tick when light first too dark         */

/* ---- Actuator state ---- */
static uint8_t s_fan  = 0;
static uint32_t s_fan_safe_tick  = 0;   /* last tick fan needed ON (auto-off timer) */
static uint32_t s_lamp_safe_tick = 0;   /* last tick lamp needed ON (auto-off timer) */
          /* 1 = ventilation relay on                  */
static uint8_t s_fill = 0;          /* 1 = fill light on                         */

/* ================= 平台对接影子数据区 =================*/
static volatile float    p_temp = 0.0f;   /* 温度 ℃                     */
static volatile float    p_humi = 0.0f;   /* 湿度 %RH                   */
static volatile uint16_t p_lux  = 0;      /* 光照 lux                   */
static volatile uint8_t  p_body = 0;      /* 1 = 有人（人体红外）        */
static volatile uint8_t  p_gas  = 0;      /* 1 = 检测到烟雾/可燃气体     */

static volatile uint8_t p_manual  = 0;    /* 平台是否下发过指令（供 LCD/语音标签，主循环接过去再执行） */
static volatile uint8_t p_fan_cmd = 0;    /* 平台设定的风扇状态           */
static volatile uint8_t p_lamp_cmd = 0;   /* 平台设定的照明状态           */
static volatile uint8_t p_act_dirty = 0;  /* 1 = 需要主循环立即刷新执行器 */

/* ---- Fused risk result ---- */
static FusionResult s_fusion;

/* ---- Software timers ---- */
static uint32_t s_sample_tick = 0;
static uint32_t s_led_tick[MODULE_NUM];   /* per-LED blink timer                 */
static uint32_t s_beep_tick   = 0;

/* ---- LCD page & key state ---- */
static uint8_t s_page = FUSION_PAGE;   /* fusion overview = 5, module = 0..3     */

typedef struct
{
    uint8_t  cur;           /* debounced current level: 1=pressed, 0=released    */
    uint32_t press_tick;    /* tick when the key was pressed down                */
    uint8_t  long_done;     /* 1 = long-press already handled                    */
} KeyState;
static KeyState s_key[MODULE_NUM];

/* ---- Touch pad (PD14) one-key mute ---- */
static uint8_t  s_mute       = 0;
/* ---- external push buttons (one-key toggle) ---- */
static uint8_t  s_btn_fan_last  = 0;
static uint8_t  s_btn_lamp_last = 0;
static uint32_t s_btn_fan_tick  = 0;
static uint32_t s_btn_lamp_tick = 0;
   /* 1 = alarm sound muted (beep/voice off)    */
static uint8_t  s_touch_cur  = 0;   /* debounced touch level: 1 = touched        */
static uint32_t s_touch_tick = 0;   /* cooldown timer                            */

/* ---- Forward declarations (static helpers) ---- */
static void Sensor_Sample(void);
static void Alarm_Evaluate(void);
static void Actuator_Update(void);
static void LED_Beep_Update(void);
static void Key_Process(void);
static void Alarm_Suppress(uint8_t module);
static void Page_Render(void);
static void Page_Render_Fusion(void);
static void Page_Render_All(void);
static void Sensor_LCD_Init(void);
static void Actuator_Init(void);
static void Button_Init(void);
static void Button_Process(void);
static void Local_ToggleSwitch(uint8_t which);

/* ================= 低功耗（暂停采集）模式 V5 =================
 * 目的：非采集时段人工把节点切到"只监视、不采集"的状态，
 *       替代逐个关闭传感器与执行器的操作。
 * 进入：KEY4 按住 3 s（LP_ENTER_MS）；退出：任意键，或 LP 采样判定越界自动退出。
 * 边界：本模式降低的是采集频次与执行器驱动开销，主控、RTOS 与网络仍照常运行，
 *       不涉及 STOP2 与断网；L2 态告警时延 = 30 s 采样周期 + 传感器 T90。 */
static uint8_t  s_lp            = 0u;          /* 1 = 低功耗页生效              */
static uint32_t s_lp_enter_t    = 0;           /* 进入时刻                      */
static uint32_t s_lp_ms         = 0;           /* 本次停留时长 ms               */
static uint32_t s_lp_total_ms   = 0;           /* 累计停留时长 ms               */
static uint32_t s_lp_sample_t   = 0;           /* 上次 LP 采样时刻              */
static uint32_t s_lp_lock_t     = 0;           /* 退出后按键封锁截止时刻         */
static uint32_t s_lp_draw_t     = 0;           /* 计时行刷新节流                */
static uint32_t s_lp_deep_dm    = 0xFFFFFFFFu; /* 已绘制的 Deep 值（0.1 min）   */
static uint32_t s_lp_tot_dm     = 0xFFFFFFFFu; /* 已绘制的 Total 值（0.1 min）  */

static void    Page_Render_LowPower(void);
static void    LP_Enter(void);
static void    LP_Exit(uint8_t by_alarm);
static void    LP_Sample(void);
static void    LP_Tick(void);
static uint8_t LP_OutOfRange(void);
static void    LP_DrawTimers(uint8_t force);


/* 简易格式化浮点数：保留 1 位小数（microlib 不支持 %f） */
static void Fmt1(char *dst, float v)
{
    int neg = (v < 0.0f);
    float a = neg ? -v : v;
    int ip = (int)a;
    int dp = (int)((a - (float)ip) * 10.0f + 0.5f);
    if (dp >= 10) { dp -= 10; ip += 1; }
    if (neg) sprintf(dst, "-%d.%d", ip, dp);
    else     sprintf(dst, "%d.%d", ip, dp);
}

/* 仅在升到 L4 沿触发一次语音播报（下降沿不重复播报） */
/* 即只在升为“紧急 L4”时触发一次 */
static void Voice_Update(void)
{
    static uint8_t s_voice_was_high = 0;
    uint8_t high = (s_alarm[MODULE_GAS] == ALARM_EMERGENCY);   /* gas L4 only */

    if (high && !s_voice_was_high && !s_mute)
    {
        ld3320_Struct.play_num = 3;   /* 3 = smoke-alarm sound in DemoSound1.h */
        PlayDemoSound_Once();
    }
    s_voice_was_high = high;
}

/* LCD 初始化：清屏，画标题栏 + 综合风险首页 */
static void Sensor_LCD_Init(void)
{
    LCD_Init();
    LCD_Fill(0, 0, lcddev.width, 20, BLUE);                            /* top bar     */
    LCD_Fill(0, lcddev.height - 20, lcddev.width, lcddev.height, BLUE);/* bottom bar  */
    Gui_StrCenter(0, 2, WHITE, BLUE, "Lab Safety Monitor", 16, 1);
    Gui_StrCenter(0, lcddev.height - 18, WHITE, BLUE, "key:page  hold:cancel", 16, 1);

    Page_Render_All();   /* default: fusion overview page */
}

/* 执行器初始化：FAN(PA8)/FILL(PB15) 清零 */
static void Actuator_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    /* 补光灯在 PB15，需使能 GPIOB 时钟 */
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin   = FAN_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_PULLDOWN;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(FAN_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin   = FILL_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_PULLDOWN;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(FILL_PORT, &GPIO_InitStruct);

    FAN_OFF();
    FILL_OFF();
}

/* 主采样：读传感器、判告警、驱动执行器、融合、刷新 LCD+串口上报 */
static void Sensor_Sample(void)
{
    /* ---- Read sensors ---- */
    /* 软件 I2C 逐位时序敏感，保证位/时序不被抢占 */
    LOS_TaskLock();

    s_body  = Infrared_Read_State();
    s_gas   = MQ5_Read_State();
    SHT30_read_result(&s_temp, &s_hum);
    s_light = BH1750_Handle();

    LOS_TaskUnlock();

    /* 越界值视为无效，置 0，LCD 显示 "--.-" */
    if (s_temp < -40.0f || s_temp > 125.0f || s_hum < 0.0f || s_hum > 100.0f)
    {
        printf("[WARN] SHT30 read out of range (T=%.1f H=%.1f), clamped to 0\r\n",
               s_temp, s_hum);
        s_temp = 0.0f;
        s_hum  = 0.0f;
    }
    if (s_light >= 54612U)      /* BH1750 max raw / 1.2 ~= 54612 lx */
    {
        s_light = 0U;
    }

    /* ---- 温度趋势：10 秒温升（两端平均差分）----*/
    if (s_temp_hist_fill < TEMP_HIST_LEN)
    {
        s_temp_hist[s_temp_hist_idx] = s_temp;
        s_temp_hist_idx = (s_temp_hist_idx + 1) % TEMP_HIST_LEN;
        s_temp_hist_fill++;
        s_temp_rise = 0.0f;
    }
    else
    {
        float cur_sum = 0.0f;   /* 当前段平均：最近 TEMP_AVG_WIN 个采样点 */
        float prev_sum = 0.0f;  /* 历史段平均：10 秒前 TEMP_AVG_WIN 个采样点 */
        uint8_t i;

        /* 当前段：刚写入的 s_temp 未入缓冲，先手动取「当前点 + 缓冲里最新 2 点」。
         * s_temp_hist_idx 指向最旧一格，最新一格是 (idx-1)、次新是 (idx-2)。 */
        cur_sum = s_temp;                                    /* 当前点        */
        cur_sum += s_temp_hist[(s_temp_hist_idx + TEMP_HIST_LEN - 1) % TEMP_HIST_LEN];
        cur_sum += s_temp_hist[(s_temp_hist_idx + TEMP_HIST_LEN - 2) % TEMP_HIST_LEN];

        /* 历史段：从 idx 起连续 TEMP_AVG_WIN 格（idx 是最旧的，即 10 秒前那点） */
        for (i = 0; i < TEMP_AVG_WIN; i++)
        {
            prev_sum += s_temp_hist[(s_temp_hist_idx + i) % TEMP_HIST_LEN];
        }

        s_temp_rise = (cur_sum - prev_sum) / (float)TEMP_AVG_WIN;

        /* 覆盖最旧一格，写指针前移 */
        s_temp_hist[s_temp_hist_idx] = s_temp;
        s_temp_hist_idx = (s_temp_hist_idx + 1) % TEMP_HIST_LEN;
    }

    /* ---- Evaluate alarm levels + update actuators ---- */
    Alarm_Evaluate();
    Actuator_Update();

    /* ---- Fused risk level (FCE + expert rules) ---- */
    Fusion_Evaluate(s_temp, s_hum, s_light, s_body,
                    s_suppress, s_alarm, &s_fusion);

    /* ---- 边缘 AI 离群检测：独立模块只产出 sim，融合统一在此编排 ---- */
#if AI_TREND_TEST_EN
    /* 自检模式：只给趋势通道喂虚拟温度，其余一切用真实值（见文件头说明） */
    AiMonitor_Process(s_temp + AiTrendTest_Offset(s_temp, s_hum), s_hum, (float)s_light);
#else
    AiMonitor_Process(s_temp, s_hum, (float)s_light);
#endif

#if AI_SCORE_EN
    {
        /* 计分用纯模型通道：温度趋势已通过 AiMonitor_GetTrendRisk() 单独计分，
         * 若这里仍用 GetSim()（= min(model, trend)）会把趋势算两遍。
         * 显示侧（LCD/云端）仍取 AiMonitor_GetSim()，口径不冲突。 */
        uint8_t ai_sim = AiMonitor_GetModelSim();
        Fusion_ApplyAi(&s_fusion, (float)ai_sim,
                       (ai_sim != AI_SIM_INVALID) ? 1U : 0U);
    }
#else
    Fusion_ApplyAi(&s_fusion, (float)AiMonitor_GetModelSim(), 0U);
#endif
    Fusion_Stabilize(&s_fusion);


    /* ---- voice announce when gas reaches L4 (unless muted) ---- */
    Voice_Update();

    /* ---- Refresh current LCD page ---- */
    Page_Render_All();

    /* ---- 串口调试摘要（1 帧/秒，不影响平台上报） ---- */
    {
        char t[8], h[8], f[8];
        Fmt1(t, s_temp); Fmt1(h, s_hum); Fmt1(f, s_fusion.score);
    printf("| T:%sC H:%s%% L:%5d lx | Body:%s Gas:L%d | Fuse:%s L%d | Plat:%s P:F%d L%d | F:%d L:%d | AI:%3d %-4s |\r\n",
           t, h, (int)s_light,
           s_body ? "Someone" : "No one",
           s_alarm[MODULE_GAS] + 1,
           f, s_fusion.level + 1,
           p_manual ? "ON " : "OFF", (int)p_fan_cmd, (int)p_lamp_cmd,
           (int)s_fan, (int)s_fill,
           (int)AiMonitor_GetSim(), AiMonitor_GetStateName());
    }

    /* ---- 更新平台影子数据，供 control_task 打包上报 ---- */
    p_temp = s_temp;
    p_humi = s_hum;
    p_lux  = s_light;
    p_body = s_body ? 1 : 0;
    p_gas  = (s_gas == 0) ? 1 : 0;      /* 平台侧：1=检测到气体，0=正常 */
}

/* 单模块告警：温湿度/光照按阈值与时长判定状态 */
static void Alarm_Evaluate(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t dur;

    /* ---- temp/humi (2-state) ---- */
    {
        uint8_t ok = (s_temp >= TEMP_MIN && s_temp <= TEMP_MAX &&
                      s_hum  >= HUMI_MIN && s_hum  <= HUMI_MAX);
        if (s_suppress[MODULE_TEMP])
        {
            if (ok) s_suppress[MODULE_TEMP] = 0;
            s_temp_bad_start = 0;
            s_alarm[MODULE_TEMP] = ALARM_NORMAL;
        }
        else if (ok)
        {
            s_temp_bad_start = 0;
            s_alarm[MODULE_TEMP] = ALARM_NORMAL;
        }
        else
        {
            /* 连续越界 ENV_CONFIRM_MS 才判 HINT，避免单帧跳变 */
            if (s_temp_bad_start == 0) s_temp_bad_start = now;
            s_alarm[MODULE_TEMP] = ((now - s_temp_bad_start) >= ENV_CONFIRM_MS)
                                   ? ALARM_HINT : ALARM_NORMAL;
        }
    }

    /* ---- light (2-state) ---- */
    {
        uint8_t ok = (s_light > 0 && s_light >= LIGHT_LOW);
        if (s_suppress[MODULE_LIGHT])
        {
            if (ok) s_suppress[MODULE_LIGHT] = 0;
            s_light_bad_start = 0;
            s_alarm[MODULE_LIGHT] = ALARM_NORMAL;
        }
        else if (ok)
        {
            s_light_bad_start = 0;
            s_alarm[MODULE_LIGHT] = ALARM_NORMAL;
        }
        else
        {
            /* 连续偏暗 ENV_CONFIRM_MS 才判 HINT，避免云影/短暂遮挡触发 */
            if (s_light_bad_start == 0) s_light_bad_start = now;
            s_alarm[MODULE_LIGHT] = ((now - s_light_bad_start) >= ENV_CONFIRM_MS)
                                    ? ALARM_HINT : ALARM_NORMAL;
        }
    }

    /* ---- gas (4-level duration) ---- */
    if (s_suppress[MODULE_GAS])
    {
        if (s_gas == 1) s_suppress[MODULE_GAS] = 0;
        s_alarm[MODULE_GAS] = ALARM_NORMAL;
    }
    else if (s_gas == 0)                          /* 0 = gas detected */
    {
        if (s_gas_start == 0) s_gas_start = now;
        dur = now - s_gas_start;
        if (dur < SAFE_L2_TIME)
            s_alarm[MODULE_GAS] = ALARM_HINT;
        else if (dur < SAFE_L3_TIME)
            s_alarm[MODULE_GAS] = ALARM_WARN;
        else
            s_alarm[MODULE_GAS] = ALARM_EMERGENCY;
    }
    else
    {
        s_gas_start = 0;
        s_alarm[MODULE_GAS] = ALARM_NORMAL;
    }

    /* ---- body IR：只提示"有人"，永不报警（2026-11-13 改） ----
     * 原实现按"有人持续时长"逐级升到 L2/L3/L4（8 秒即紧急），
     * 实验室日常有人，等于给融合分长期垫 20 分底噪，并让规则2 一命中就跳 L4。
     * 现在人体等级恒为 NORMAL：LED3 不闪、蜂鸣器不响、综合分不计，
     * 只在 LCD 人体页用 s_body 显示 Someone / No one。
     * 人体的安全意义保留在规则2：气体持续 8 秒（气体 L4）且有人 → 融合直接 L4。 */
    s_alarm[MODULE_BODY] = ALARM_NORMAL;
}
static void Actuator_Update(void)
{
    uint32_t now = HAL_GetTick();
    uint8_t fan_auto  = (s_alarm[MODULE_TEMP] != ALARM_NORMAL ||
                         s_alarm[MODULE_GAS]  >= ALARM_WARN) ? 1 : 0;
    uint8_t lamp_auto = (s_alarm[MODULE_LIGHT] != ALARM_NORMAL) ? 1 : 0;
    uint8_t safety    = (s_alarm[MODULE_GAS] == ALARM_EMERGENCY ||
                         s_fusion.level      == ALARM_EMERGENCY) ? 1 : 0;

    /* 气体报警必须一直 ON（安全优先），强制风扇 ON */
    /* auto-off: device opened by cloud/local button auto-releases once env is
       safe for AUTO_OFF_HOLD_MS, so it no longer "stays on until manual off". */
    uint8_t fan_need  = (fan_auto || safety);
    uint8_t lamp_need = lamp_auto;
    if (fan_need)  s_fan_safe_tick  = now;   /* refresh timer while triggered */
    if (lamp_need) s_lamp_safe_tick = now;
    if (p_fan_cmd  && !fan_need  && (now - s_fan_safe_tick)  >= AUTO_OFF_HOLD_MS) { p_fan_cmd  = 0;  Scene.event_sendflag = 1; }
    if (p_lamp_cmd && !lamp_need && (now - s_lamp_safe_tick) >= AUTO_OFF_HOLD_MS) { p_lamp_cmd = 0;  Scene.event_sendflag = 1; }

    s_fan  = (fan_auto  || p_fan_cmd  || safety) ? 1 : 0;
    s_fill = (lamp_auto || p_lamp_cmd)         ? 1 : 0;

    s_fan  = s_fan  ? 1 : 0;
    s_fill = s_fill ? 1 : 0;

    if (s_fan)  FAN_ON();   else FAN_OFF();
    if (s_fill) FILL_ON();  else FILL_OFF();
}

void Plat_SetSwitch(uint8_t which, uint8_t on)
{
    p_manual = 1;   /* 标记 LCD/语音标签：平台已下发过指令 */

    if (which == PLAT_SW_FAN)   { p_fan_cmd  = on ? 1 : 0;  if (p_fan_cmd)  s_fan_safe_tick  = HAL_GetTick(); }
    else                        { p_lamp_cmd = on ? 1 : 0;  if (p_lamp_cmd) s_lamp_safe_tick = HAL_GetTick(); }

    p_act_dirty = 1;
}

uint8_t Plat_GetFan(void)    { return s_fan;  }
uint8_t Plat_GetLamp(void)   { return s_fill; }
uint8_t Plat_IsManual(void)  { return p_manual; }

/* ============ external button: one-key toggle (no cloud command) ============ */
void Local_ToggleSwitch(uint8_t which)
{
    p_manual = 1;   /* local manual, flag for LCD/voice */

    if (which == PLAT_SW_FAN)
    {
        p_fan_cmd = p_fan_cmd ? 0 : 1;
        if (p_fan_cmd) s_fan_safe_tick = HAL_GetTick();   /* restart auto-off timer */
    }
    else
    {
        p_lamp_cmd = p_lamp_cmd ? 0 : 1;
        if (p_lamp_cmd) s_lamp_safe_tick = HAL_GetTick();
    }

    p_act_dirty = 1;          /* refresh actuator immediately */
    Scene.event_sendflag = 1; /* sync new state to Huawei cloud (platform no longer wrong) */
    printf("[LOCAL] toggle %s -> %s\r\n",
           which == PLAT_SW_FAN ? "FAN" : "LAMP",
           which == PLAT_SW_FAN ? (p_fan_cmd ? "ON" : "OFF")
                                : (p_lamp_cmd ? "ON" : "OFF"));
}

static void Button_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = BTN_PULL;
    GPIO_InitStruct.Pin  = BTN_FAN_PIN;
    HAL_GPIO_Init(BTN_FAN_PORT, &GPIO_InitStruct);
    GPIO_InitStruct.Pin  = BTN_LAMP_PIN;
    HAL_GPIO_Init(BTN_LAMP_PORT, &GPIO_InitStruct);
}

static void Button_Process(void)
{
    uint32_t now = HAL_GetTick();

    /* fan button: toggle on falling edge (pressed=low), with debounce */
    uint8_t f = (HAL_GPIO_ReadPin(BTN_FAN_PORT, BTN_FAN_PIN) == GPIO_PIN_RESET) ? 1 : 0;
    if (f && !s_btn_fan_last)
    {
        if (now - s_btn_fan_tick > BTN_DEBOUNCE_MS)
        {
            Local_ToggleSwitch(PLAT_SW_FAN);
            s_btn_fan_tick = now;
        }
    }
    s_btn_fan_last = f;

    /* lamp button */
    uint8_t l = (HAL_GPIO_ReadPin(BTN_LAMP_PORT, BTN_LAMP_PIN) == GPIO_PIN_RESET) ? 1 : 0;
    if (l && !s_btn_lamp_last)
    {
        if (now - s_btn_lamp_tick > BTN_DEBOUNCE_MS)
        {
            Local_ToggleSwitch(PLAT_SW_LAMP);
            s_btn_lamp_tick = now;
        }
    }
    s_btn_lamp_last = l;
}


/* [华为云] 上报所需取值接口：直接返回影子区里的温湿度原始值。*/
float Plat_GetTemp(void)     { return p_temp; }
float Plat_GetHumi(void)     { return p_humi; }

/* [华为云·按物模型补齐 8 属性] Gas / Body_IR / Light 取值接口 --------------- */
uint8_t  Plat_GetGas(void)   { return p_gas;  }
uint8_t  Plat_GetBody(void)  { return p_body; }
uint16_t Plat_GetLight(void) { return p_lux;  }

/* [AI] AI 相似度取值接口（0~100，100=正常），供 ocprotocol.c 上报 AI_Score */
uint8_t  Plat_GetAiSim(void)  { return AiMonitor_GetSim(); }
float    Plat_GetTempRise(void){ return s_temp_rise; }

/* [新增] 融合研判结论取值接口*/
uint8_t  Plat_GetFuseLevel(void) { return (uint8_t)s_fusion.level; }
float    Plat_GetFuseScore(void) { return s_fusion.score; }
uint8_t  Plat_GetDominant(void)  { return s_fusion.dominant; }

/* [Sound=触摸键一键静音] 报警声音状态取值接口 -------------------------------- */
uint8_t  Plat_GetSoundOn(void){ return s_mute ? 0 : 1; }

/* 采样数据 + 执行器实际状态 -> 平台上报结构体*/
void Plat_FillReport(PTL_DATA_STRUCT *dst)
{
    char buf[8];

    if (dst == NULL)
    {
        return;
    }

    /* ---- 处理温度：按湿度传回的格式 -> 换算出温度； ---- */
    {
        float t = p_temp;
        if (t < -9.9f)  t = -9.9f;
        if (t > 99.9f)  t = 99.9f;
        Fmt1(buf, t);
        memset(dst->Temperature, 0, sizeof(dst->Temperature));
        strncpy(dst->Temperature, buf, sizeof(dst->Temperature) - 1);
    }

    /* ---- 处理光强：光照传感器 -> 归一化到 0~9999 ---- */
    {
        uint16_t lux = p_lux;
        if (lux > 9999U) lux = 9999U;       /* 4 位整数 + '\0' 刚好 5 字节 */
        sprintf(buf, "%d", (int)lux);
        memset(dst->Light, 0, sizeof(dst->Light));
        strncpy(dst->Light, buf, sizeof(dst->Light) - 1);
    }

    /* ---- 气体、人体：1=正常 / 1=检测到 ---- */
    dst->dInput_Body = p_body ? 1 : 0;
    dst->dInput_Gas  = p_gas  ? 1 : 0;

    /* ---- 执行器实际状态：回填平台输出值，保证“界面与实物一致” ---- */
    dst->dOutput_LED_State   = s_fill ? 1 : 0;
    dst->dOutput_Fan_State   = s_fan  ? 1 : 0;
    dst->dOutput_Fan_Level   = s_fan  ? 1 : 0;

    /* ---- 本工程未接的器件，固定 0（平台页面显示 OFF，不报错） ---- */
    dst->dOutput_Curtains    = 0;
    dst->dOutput_EV_State    = 0;
    dst->dOutput_Servo_State = 0;
    dst->dInput_Servo_Red    = 0;

    dst->err_code = 0;
}

/* LED 闪烁 + 蜂鸣器状态机 */
static void LED_Beep_Update(void)
{
    uint32_t now = HAL_GetTick();
    uint8_t  i;

    for (i = 0; i < MODULE_NUM; i++)
    {
        uint16_t led = s_led_pin[i];
        switch (s_alarm[i])
        {
            case ALARM_NORMAL:
                led_close(led);
                break;

            case ALARM_HINT:                     /* slow blink 500ms */
                if (now - s_led_tick[i] >= 500U) { s_led_tick[i] = now; led_toggle(led); }
                break;

            case ALARM_WARN:                     /* fast blink 200ms */
                if (now - s_led_tick[i] >= 200U) { s_led_tick[i] = now; led_toggle(led); }
                break;

            default: /* ALARM_EMERGENCY */       /* burst blink 50ms */
                if (now - s_led_tick[i] >= 50U)  { s_led_tick[i] = now; led_toggle(led); }
                break;
        }
    }

    /* buzzer driven by fused risk level (silenced while muted) */
    if (s_mute)
    {
        beep_close();
    }
    else switch (s_fusion.level)
    {
        case ALARM_EMERGENCY:
            beep_open();
            break;

        case ALARM_WARN:
            if (now - s_beep_tick >= 500U) { s_beep_tick = now; beep_open(); }
            if (now - s_beep_tick >= 100U) beep_close();
            break;

        default:
            beep_close();
            break;
    }
}

/* 手动取消某模块报警 */
static void Alarm_Suppress(uint8_t module)
{
    s_suppress[module] = 1;
    s_alarm[module] = ALARM_NORMAL;

    if (module == MODULE_GAS)  s_gas_start  = 0;

    Actuator_Update();
    Page_Render_All();
}

/* 按键处理：短按翻页，长按取消报警 */
static void Key_Process(void)
{
    static uint32_t last_scan = 0;
    uint32_t now = HAL_GetTick();
    uint8_t  keynum;
    uint8_t  i;

    if (now - last_scan < KEY_SCAN_MS)
        return;
    last_scan = now;

    keynum = keyscan();   /* bit0=KEY1 .. bit3=KEY4 */

    /* ---- 低功耗页：任意键退出；退出后 2 s 内屏蔽，防"按住即醒" ---- */
    if (s_lp)
    {
        uint8_t any = 0u;

        for (i = 0; i < MODULE_NUM; i++)
        {
            s_key[i].cur = (keynum & (1u << i)) ? 1u : 0u;
            if (s_key[i].cur)
                any = 1u;
        }

        if (any && now >= s_lp_lock_t)
        {
            LP_Exit(0u);
            return;
        }
        return;   /* 低功耗页不参与翻页与取消报警 */
    }

    for (i = 0; i < MODULE_NUM; i++)
    {
        uint8_t pressed = (keynum & (1u << i)) ? 1u : 0u;

        /* edge detect -> release triggers short press */
        if (pressed != s_key[i].cur)
        {
            s_key[i].cur = pressed;
            if (pressed)
            {
                s_key[i].press_tick = now;
                s_key[i].long_done  = 0;
            }
            else
            {
                if (!s_key[i].long_done)
                {
                    /* toggle: module page i <-> fusion overview page */
                    if (s_page == i)
                        s_page = FUSION_PAGE;
                    else
                        s_page = i;
                    Page_Render_All();
                }
            }
        }

        /* long press (held >= KEY_LONG_MS, handled once) */
        if (pressed && !s_key[i].long_done &&
            (now - s_key[i].press_tick) >= KEY_LONG_MS)
        {
            s_key[i].long_done = 1;
            Alarm_Suppress(i);
        }

        /* KEY4 held >= LP_ENTER_MS -> enter low power page.
         * KEY1..KEY3 keep the original 1 s suppress behaviour;
         * KEY4 keeps it too, and 2 s later becomes the LP entry. */
        if (i == 3u && pressed && (now - s_key[i].press_tick) >= LP_ENTER_MS)
        {
            LP_Enter();
            return;
        }
    }
}

/* 触摸按键一键静音 */
static void Touch_Process(void)
{
    /* 触摸为高电平有效（1=按下） */
#define TOUCH_ACTIVE_LEVEL  1u
    uint8_t touched = (Touch_Key_State() == TOUCH_ACTIVE_LEVEL) ? 1u : 0u;

    if (touched && !s_touch_cur)
    {
        /* rising edge of "touched": debounce with a 200ms cooldown */
        if (HAL_GetTick() - s_touch_tick >= 200U)
        {
            s_touch_tick = HAL_GetTick();
            s_mute = !s_mute;
            printf("[Touch] alarm sound %s\r\n", s_mute ? "MUTED" : "ON");
            if (s_mute) beep_close();
        }
    }
    s_touch_cur = touched;
}

/* 单模块页面渲染 */
static void Page_Render(void)
{
    char str[32];

    LCD_Fill(0, 21, lcddev.width - 1, lcddev.height - 21, WHITE);

    switch (s_page)
    {
        /* ===== page 1: temp/humi ===== */
        case MODULE_TEMP:
            Gui_StrCenter(0, 32, BLACK, WHITE, (uint8_t*)"Temp & Humi", 16, 1);

            Show_Str(10, 80,  BLACK, WHITE, (uint8_t*)"Temperature:", 16, 1);
            Show_Str(10, 110, BLACK, WHITE, (uint8_t*)"Humidity   :", 16, 1);
            Show_Str(10, 140, BLACK, WHITE, (uint8_t*)"Status     :", 16, 1);
            Show_Str(10, 170, BLACK, WHITE, (uint8_t*)"Fan        :", 16, 1);

            if (s_temp < 0.0f || s_temp > 55.0f) sprintf(str, "--.- C");
            else { Fmt1(str, s_temp); strcat(str, " C"); }
            Show_Str(130, 80, BLUE, WHITE, (uint8_t*)str, 16, 1);

            if (s_hum < 20.0f || s_hum > 90.0f) sprintf(str, "--.- %%");
            else { Fmt1(str, s_hum); strcat(str, " %%"); }
            Show_Str(130, 110, BLUE, WHITE, (uint8_t*)str, 16, 1);

            if (s_alarm[MODULE_TEMP] == ALARM_NORMAL)
                Show_Str(130, 140, GREEN, WHITE, (uint8_t*)"Normal", 16, 1);
            else
                Show_Str(130, 140, RED, WHITE, (uint8_t*)"Alarm!", 16, 1);

            Show_Str(130, 170, BLUE, WHITE, (uint8_t*)(s_fan ? "ON " : "OFF"), 16, 1);
            break;

        /* ===== page 2: gas ===== */
        case MODULE_GAS:
            Gui_StrCenter(0, 32, BLACK, WHITE, (uint8_t*)"Gas Monitor", 16, 1);

            Show_Str(10, 80,  BLACK, WHITE, (uint8_t*)"State:", 16, 1);
            Show_Str(10, 110, BLACK, WHITE, (uint8_t*)"Level:", 16, 1);
            Show_Str(10, 140, BLACK, WHITE, (uint8_t*)"Fan  :", 16, 1);

            switch (s_alarm[MODULE_GAS])
            {
                case ALARM_NORMAL:
                    Show_Str(130, 80, GREEN, WHITE, (uint8_t*)"Safe", 16, 1);
                    Show_Str(130, 110, BLUE, WHITE, (uint8_t*)"L1", 16, 1);
                    break;
                case ALARM_HINT:
                    Show_Str(130, 80, YELLOW, WHITE, (uint8_t*)"LEAK", 16, 1);
                    Show_Str(130, 110, YELLOW, WHITE, (uint8_t*)"L2", 16, 1);
                    break;
                case ALARM_WARN:
                    Show_Str(130, 80, RED, WHITE, (uint8_t*)"LEAK", 16, 1);
                    Show_Str(130, 110, RED, WHITE, (uint8_t*)"L3", 16, 1);
                    break;
                default:
                    Show_Str(130, 80, RED, WHITE, (uint8_t*)"LEAK!!", 16, 1);
                    Show_Str(130, 110, RED, WHITE, (uint8_t*)"L4", 16, 1);
                    break;
            }

            Show_Str(130, 140, BLUE, WHITE, (uint8_t*)(s_fan ? "ON " : "OFF"), 16, 1);
            break;

        /* ===== page 3: body IR ===== */
        case MODULE_BODY:
            Gui_StrCenter(0, 32, BLACK, WHITE, (uint8_t*)"Body IR", 16, 1);

            Show_Str(10, 80,  BLACK, WHITE, (uint8_t*)"State:", 16, 1);
            Show_Str(10, 110, BLACK, WHITE, (uint8_t*)"Level:", 16, 1);

            /* 人体不再判级：这里只提示"有没有人"，等级恒为 L1，不参与报警 */
            if (s_body)
            {
                Show_Str(130, 80, BLUE, WHITE, (uint8_t*)"Someone", 16, 1);
            }
            else
            {
                Show_Str(130, 80, GREEN, WHITE, (uint8_t*)"No one", 16, 1);
            }
            Show_Str(130, 110, BLUE, WHITE, (uint8_t*)"L1", 16, 1);
            break;

        /* ===== page 4: light ===== */
        default: /* MODULE_LIGHT */
            Gui_StrCenter(0, 32, BLACK, WHITE, (uint8_t*)"Light", 16, 1);

            Show_Str(10, 80,  BLACK, WHITE, (uint8_t*)"Illum :", 16, 1);
            Show_Str(10, 110, BLACK, WHITE, (uint8_t*)"Status:", 16, 1);
            Show_Str(10, 140, BLACK, WHITE, (uint8_t*)"Fill  :", 16, 1);

            if (s_light > 0 && s_light < 54612) sprintf(str, "%d lx", s_light);
            else sprintf(str, "--- lx");
            Show_Str(130, 80, BLUE, WHITE, (uint8_t*)str, 16, 1);

            if (s_alarm[MODULE_LIGHT] == ALARM_NORMAL)
                Show_Str(130, 110, GREEN, WHITE, (uint8_t*)"Normal", 16, 1);
            else
                Show_Str(130, 110, RED, WHITE, (uint8_t*)"Dark!", 16, 1);

            Show_Str(130, 140, BLUE, WHITE, (uint8_t*)(s_fill ? "ON " : "OFF"), 16, 1);
            break;
    }
}

/* ================= 低功耗页（暂停采集） ================= */

/* 计时行刷新：0.1 min 分辨率，值变化才重画，避免整屏闪烁 */
static void LP_DrawTimers(uint8_t force)
{
    char str[16];
    uint32_t dm = s_lp_ms / 6000u;                    /* 6000 ms = 0.1 min */
    uint32_t tm = (s_lp_total_ms + s_lp_ms) / 6000u;

    if (force || dm != s_lp_deep_dm)
    {
        s_lp_deep_dm = dm;
        LCD_Fill(100, 170, 238, 188, WHITE);
        sprintf(str, "%lu.%lu min", (unsigned long)(dm / 10u), (unsigned long)(dm % 10u));
        Show_Str(100, 172, BLUE, WHITE, (uint8_t *)str, 16, 1);
    }

    if (force || tm != s_lp_tot_dm)
    {
        s_lp_tot_dm = tm;
        LCD_Fill(100, 206, 238, 224, WHITE);
        sprintf(str, "%lu.%lu min", (unsigned long)(tm / 10u), (unsigned long)(tm % 10u));
        Show_Str(100, 208, BLUE, WHITE, (uint8_t *)str, 16, 1);
    }
}

/* 低功耗页版面 */
static void Page_Render_LowPower(void)
{
    LCD_Fill(0, 21, lcddev.width - 1, lcddev.height - 21, WHITE);

    Gui_StrCenter(0, 32, BLUE, WHITE, (uint8_t *)"LOW POWER MODE", 16, 1);

    Show_Str(10, 64,  BLACK, WHITE, (uint8_t *)"Sample:", 16, 1);
    Show_Str(100, 64, BLUE, WHITE, (uint8_t *)"30 s", 16, 1);

    Show_Str(10, 100, BLACK, WHITE, (uint8_t *)"Wake :", 16, 1);
    Show_Str(100, 100, BLUE, WHITE, (uint8_t *)"KEY / TICK", 16, 1);

    Show_Str(10, 136, BLACK, WHITE, (uint8_t *)"Clock:", 16, 1);
    Show_Str(100, 136, BLUE, WHITE, (uint8_t *)"SysTick", 16, 1);

    Show_Str(10, 172, BLACK, WHITE, (uint8_t *)"Deep :", 16, 1);
    Show_Str(10, 208, BLACK, WHITE, (uint8_t *)"Total:", 16, 1);

    Show_Str(10, 244, BLACK, WHITE, (uint8_t *)"Exit :", 16, 1);
    Show_Str(100, 244, RED, WHITE, (uint8_t *)"any key", 16, 1);

    s_lp_deep_dm = 0xFFFFFFFFu;
    s_lp_tot_dm  = 0xFFFFFFFFu;
    LP_DrawTimers(1u);
}

/* 越界判定：只认"安全相关"的量。
 * 光照不计入——无人关灯是夜间常态，工程里已按 LIGHT_NEED_BODY 处理。
 * SHT30 读数 0/0 或越出器件量程视为无效，不据此退出，避免误醒。 */
static uint8_t LP_OutOfRange(void)
{
    uint8_t sht_ok = ((s_temp == 0.0f && s_hum == 0.0f) ||
                      s_temp < -10.0f || s_temp > 60.0f) ? 0u : 1u;

    if (s_gas == 0u)                                          /* 可燃气检出 */
        return 1u;
    if (sht_ok && (s_temp < TEMP_MIN || s_temp > TEMP_MAX))    /* 温度越界   */
        return 1u;
    if (sht_ok && (s_hum  < HUMI_MIN || s_hum  > HUMI_MAX))    /* 湿度越界   */
        return 1u;

    return 0u;
}

/* 进入低功耗页：关执行器与指示，这就是"不用逐个关设备"的那一步 */
static void LP_Enter(void)
{
    uint32_t now = HAL_GetTick();
    uint8_t  i;

    for (i = 0; i < MODULE_NUM; i++)
        led_close(s_led_pin[i]);
    beep_close();

    FAN_OFF();
    FILL_OFF();
    s_fan  = 0u;
    s_fill = 0u;

    s_lp          = 1u;
    s_lp_enter_t  = now;
    s_lp_lock_t   = now + LP_WAKE_LOCK_MS;   /* 进入同样封锁 2 s：手还按在 KEY4 上 */
    s_lp_ms       = 0u;
    s_lp_sample_t = now;
    s_lp_draw_t   = now;
    s_lp_deep_dm  = 0xFFFFFFFFu;
    s_lp_tot_dm   = 0xFFFFFFFFu;

    Page_Render_LowPower();

    printf("[LP] enter, sample=%us, sensors/actuators idle\r\n",
           (unsigned)(LP_SAMPLE_MS / 1000u));
}

/* 退出低功耗页：by_alarm=1 表示越界自动退出 */
static void LP_Exit(uint8_t by_alarm)
{
    uint32_t now = HAL_GetTick();
    uint8_t  i;

    s_lp_ms       = now - s_lp_enter_t;   /* 以退出时刻为准，避免差一个刷新周期 */
    s_lp_total_ms += s_lp_ms;
    s_lp           = 0u;
    s_lp_lock_t    = now + LP_WAKE_LOCK_MS;

    /* 同步按键状态：退出时手仍按着，避免松开被当成"短按翻页" */
    for (i = 0; i < MODULE_NUM; i++)
    {
        s_key[i].cur       = 1u;
        s_key[i].long_done = 1u;
    }

    /* 外部按钮与触摸的边沿基准同步一次，
     * 避免 LP 期间按过、退出瞬间被当成一次新按下导致执行器误动作 */
    s_btn_fan_last  = (HAL_GPIO_ReadPin(BTN_FAN_PORT,  BTN_FAN_PIN)  == GPIO_PIN_RESET) ? 1u : 0u;
    s_btn_lamp_last = (HAL_GPIO_ReadPin(BTN_LAMP_PORT, BTN_LAMP_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
    s_touch_cur     = (Touch_Key_State() == 1u) ? 1u : 0u;

    /* 让主循环下一轮立刻做一次完整采样（报警判级 + 执行器 + 页面） */
    s_sample_tick = now - SAMPLE_MS;

    printf("[LP] exit (%s), this=%lumin total=%lumin\r\n",
           by_alarm ? "alarm" : "key",
           (unsigned long)(s_lp_ms / 60000u),
           (unsigned long)(s_lp_total_ms / 60000u));

    Page_Render_All();
}

/* 低功耗态采样：只读四个量并判越界，不跑融合/AI/上报/执行器 */
static void LP_Sample(void)
{
    char t[8], h[8];

    LOS_TaskLock();
    s_body  = Infrared_Read_State();
    s_gas   = MQ5_Read_State();
    SHT30_read_result(&s_temp, &s_hum);
    s_light = BH1750_Handle();
    LOS_TaskUnlock();

    if (s_temp < -40.0f || s_temp > 125.0f || s_hum < 0.0f || s_hum > 100.0f)
    {
        s_temp = 0.0f;
        s_hum  = 0.0f;
    }
    if (s_light >= 54612U)
        s_light = 0U;

    /* 影子数据照常更新，平台侧看到的是"低频仍在线的节点" */
    p_temp = s_temp;
    p_humi = s_hum;
    p_lux  = s_light;
    p_body = s_body ? 1 : 0;
    p_gas  = (s_gas == 0) ? 1 : 0;

    Fmt1(t, s_temp);
    Fmt1(h, s_hum);
    printf("[LP] sample T:%sC H:%s%% Gas:%d -> %s\r\n",
           t, h, (int)s_gas,
           LP_OutOfRange() ? "OUT OF RANGE, back to L0" : "ok");

    if (LP_OutOfRange())
        LP_Exit(1u);
}

/* 低功耗页每轮调用的计时刷新 */
static void LP_Tick(void)
{
    uint32_t now = HAL_GetTick();

    if (!s_lp)
        return;

    s_lp_ms = now - s_lp_enter_t;

    if (now - s_lp_draw_t >= LP_DRAW_MS)
    {
        s_lp_draw_t = now;
        LP_DrawTimers(0u);
    }
}

/* 综合风险概览页渲染 */
static void Page_Render_Fusion(void)
{
    char str[32];

    LCD_Fill(0, 21, lcddev.width - 1, lcddev.height - 21, WHITE);

    Gui_StrCenter(0, 32, BLACK, WHITE, (uint8_t*)"Fusion Risk", 16, 1);

    Show_Str(10, 80,  BLACK, WHITE, (uint8_t*)"Score:", 16, 1);
    Fmt1(str, s_fusion.score);
    Show_Str(130, 80, BLUE, WHITE, (uint8_t*)str, 16, 1);

    Show_Str(10, 110, BLACK, WHITE, (uint8_t*)"Level:", 16, 1);
    switch (s_fusion.level)
    {
        case ALARM_HINT:      Show_Str(130, 110, YELLOW, WHITE, (uint8_t*)"L2 Hint", 16, 1); break;
        case ALARM_WARN:      Show_Str(130, 110, RED, WHITE, (uint8_t*)"L3 Warn", 16, 1); break;
        case ALARM_EMERGENCY: Show_Str(130, 110, RED, WHITE, (uint8_t*)"L4 Emerg", 16, 1); break;
        default:              Show_Str(130, 110, GREEN, WHITE, (uint8_t*)"L1 Normal", 16, 1); break;
    }

    Show_Str(10, 140, BLACK, WHITE, (uint8_t*)"Source:", 16, 1);
    switch (s_fusion.dominant)
    {
        case MODULE_GAS:   Show_Str(130, 140, RED, WHITE, (uint8_t*)"Gas", 16, 1); break;
        case MODULE_BODY:  Show_Str(130, 140, BLUE, WHITE, (uint8_t*)"Body IR", 16, 1); break;
        case MODULE_LIGHT: Show_Str(130, 140, BLUE, WHITE, (uint8_t*)"Light", 16, 1); break;
        default:           Show_Str(130, 140, GREEN, WHITE, (uint8_t*)"Temp/Humi", 16, 1); break;
    }

    Show_Str(10, 180, BLACK, WHITE, (uint8_t*)"Gas :", 16, 1);
    sprintf(str, "%3d", (int)(s_fusion.factor_score[MODULE_GAS] + 0.5f));
    Show_Str(70, 180, BLUE, WHITE, (uint8_t*)str, 16, 1);

    Show_Str(120, 180, BLACK, WHITE, (uint8_t*)"Temp:", 16, 1);
    sprintf(str, "%3d", (int)(s_fusion.factor_score[MODULE_TEMP] + 0.5f));
    Show_Str(180, 180, BLUE, WHITE, (uint8_t*)str, 16, 1);

    Show_Str(10, 210, BLACK, WHITE, (uint8_t*)"Body :", 16, 1);
    sprintf(str, "%3d", (int)(s_fusion.factor_score[MODULE_BODY] + 0.5f));
    Show_Str(70, 210, BLUE, WHITE, (uint8_t*)str, 16, 1);

    Show_Str(120, 210, BLACK, WHITE, (uint8_t*)"Light:", 16, 1);
    sprintf(str, "%3d", (int)(s_fusion.factor_score[MODULE_LIGHT] + 0.5f));
    Show_Str(180, 210, BLUE, WHITE, (uint8_t*)str, 16, 1);

    /* mute indicator */
    Show_Str(10, 240, BLACK, WHITE, (uint8_t*)"Sound:", 16, 1);
    Show_Str(70, 240, s_mute ? RED : GREEN, WHITE, (uint8_t*)(s_mute ? "MUTED" : "ON   "), 16, 1);
    /* 边缘 AI：相似度 + 状态 */
    Show_Str(140, 240, BLACK, WHITE, (uint8_t*)"AI:", 16, 1);
    {
        uint8_t sim = AiMonitor_GetSim();
        if (sim == AI_SIM_INVALID) sprintf(str, "--- %s", AiMonitor_GetStateName());
        else                       sprintf(str, "%3d %s", (int)sim, AiMonitor_GetStateName());
    }
    Show_Str(168, 240, BLUE, WHITE, (uint8_t*)str, 16, 1);

    /* ---- 平台状态行（PLAT=平台已下发 / AUTO=本地自动；P:平台指令 F/L:实际状态 N:联网） ---- */
    sprintf(str, "PLAT:%s P:F%d L%d | F:%d L:%d N:%d",
            p_manual ? "ON " : "OFF",
            (int)p_fan_cmd, (int)p_lamp_cmd,
            (int)s_fan, (int)s_fill,
            Is_Net_Module_Connected() ? 1 : 0);
    Show_Str(10, 272, p_manual ? RED : BLUE, WHITE, (uint8_t*)str, 16, 1);
}

/* 页面渲染分发 */
static void Page_Render_All(void)
{
    if (s_lp)
        Page_Render_LowPower();
    else if (s_page == FUSION_PAGE)
        Page_Render_Fusion();
    else
        Page_Render();
}

/* 传感器任务初始化 */
void task_sensor_init(void)
{
    Sensor_LCD_Init();       /* 先点亮屏幕并显示综合风险首页，方便看出进度        */
    gpio_module_open(KEYS);  /* KEY pins PD5/4/3/2 as input (for keyscan)              */
    Touch_Key_Init();        /* PD14 touch pad -> one-key mute                         */
    LD3320_Init();           /* voice module (SPI3) -> gas L4 alarm announce           */
    Actuator_Init();         /* FAN PA8 / FILL PB15（按引脚表，原来实验不变）              */
    AS312_Init();            /* PC8  body IR                                          */
    SHT30_GPIO_Init();       /* PB13/PB14 SHT30 (software I2C)                        */
    Init_BH1750();           /* PB13/PB14 + PD12 BH1750 (software I2C)                */
    MQ5_Init();              /* PD15 gas                                              */
    Fusion_Init();           /* fusion internal state                                 */

    AiMonitor_Init();
    printf("[OK] AI monitor init, state=%s\r\n", AiMonitor_GetStateName());

    printf("[OK] 4-sensor env task init done (platform: HOME 0x02)\r\n");
}

/* 传感器任务主循环 */
static VOID sensor_task(VOID)
{
    UINT32 uwRet = LOS_OK;

    Button_Init();
    while (1)
    {
        Key_Process();       /* page switch / alarm cancel / LP enter-exit */

        if (s_lp)
        {
            /* 低功耗页：停常规采集与指示，只保留 30 s 一次越界监视 */
            if (HAL_GetTick() - s_lp_sample_t >= LP_SAMPLE_MS)
            {
                s_lp_sample_t = HAL_GetTick();
                LP_Sample();
            }
            LP_Tick();
        }
        else
        {
            LED_Beep_Update();   /* LED + buzzer, non-blocking */
            Touch_Process();     /* touch pad -> toggle mute  */
            Button_Process();    /* external push buttons: one-key toggle */

            /* 平台指令到达：10ms 内刷新执行器，不等 1s 采样周期，
               保证点一下开关立刻有动作 */
            if (p_act_dirty)
            {
                p_act_dirty = 0;
                Actuator_Update();
                Page_Render_All();
            }

            /* 处理 LD3320 MP3 FIFO */
            if (ld3320_Struct.int_flag)
            {
                ld3320_Struct.int_flag = 0;
                ProcessInt0();
            }

            if (HAL_GetTick() - s_sample_tick >= SAMPLE_MS)
            {
                s_sample_tick = HAL_GetTick();
                Sensor_Sample(); /* sample + fuse + actuators + render + shadow */
            }
        }

        uwRet = LOS_TaskDelay(10);
        if (uwRet != LOS_OK)
        {
            return;
        }
    }
}

/* 创建传感器任务 */
UINT32 creat_sensor_task(VOID)
{
    UINT32 uwRet = LOS_OK;
    TSK_INIT_PARAM_S task_init_param = {0};   /* 未使用字段必须清零 */

    task_init_param.usTaskPrio = 2;                       /* below control(0)/view(1) */
    task_init_param.pcName = "sensor_task";
    task_init_param.pfnTaskEntry = (TSK_ENTRY_FUNC)sensor_task;
    task_init_param.uwStackSize = 0xC00;                  /* 3 KB stack for LCD/sample */

    uwRet = LOS_TaskCreate(&g_SensorTskHandle, &task_init_param);
    if (LOS_OK != uwRet)
    {
        return uwRet;
    }
    return uwRet;
}
