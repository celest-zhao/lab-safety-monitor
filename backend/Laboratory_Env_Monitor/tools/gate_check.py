# -*- coding: utf-8 -*-
"""趋势通道门限验证：逐行复刻固件 task_user/ai_monitor.c 的 trend 计算。

对应源码
    ai_monitor.c  63-75 行   宏定义
    ai_monitor.c 142-264 行  入队 / 回归 / 判据 / 风险映射 / 平滑

与固件逐项对齐的处理（改动固件后必须同步本文件）
    1. 30 点环形窗；样本按 (int16)(temp * 100) 量化，模拟 SHT30 的 0.01 ℃ 分辨率
    2. 最小二乘斜率；斜率再过 EMA（alpha = 0.10，首帧直接对齐，不从 0 爬）
    3. R² 拟合优度、相邻帧最大跳变 jmp，与斜率同一遍遍历算出
    4. 三判据 AND：slope > 0.005 ℃/s（= 0.30 ℃/min）、R² ≥ 0.80、jmp ≤ 0.25 ℃
    5. 近端外推：slope × 5 ≥ 0.02 ℃ 才算"事件仍在进行"（判人手停在半空）
    6. eta = (40 − T) / (EMA 斜率 × 60) 分钟；eta ≤ 5 → risk 100，eta ≥ 40 → risk 0
    7. 判据失效后释放去抖 3 帧（保持前值不归零）
    8. 单帧上升 ≤ 15、下降 ≤ 10，再钳 0~100

口径：risk 是 0~100 的"升温风险"，进入融合层前乘 0.30 权重；
      本脚本不算 sim，也不涉及规则引擎与分级。

用法
    python gate_check.py            # 跑内置场景，打印每个场景的使能时刻与峰值
    python gate_check.py --trace    # 附逐帧明细（t / T / slope / R² / jmp / risk）
"""

import sys

# ---- 与 ai_monitor.c 一致的常量 -------------------------------------------
WIN          = 30      # AI_TREND_WIN       点数（1 Hz 采样，即 30 秒）
RATE_MIN     = 0.005   # AI_TREND_RATE_MIN  ℃/s，即 0.30 ℃/min
R2_MIN       = 0.80    # AI_TREND_R2_MIN
NEAR_K       = 5       # AI_TREND_NEAR_K
NEAR_MIN     = 0.02    # AI_TREND_NEAR_MIN  ℃
ETA_SLOW     = 40.0    # AI_TREND_ETA_SLOW  分钟
ETA_FAST     = 5.0     # AI_TREND_ETA_FAST  分钟
STEP_MAX     = 15.0    # AI_TREND_STEP_MAX  risk 单帧最大上升
FALL_MAX     = 10.0    # AI_TREND_FALL_MAX  risk 单帧最大下降
RELEASE_N    = 3       # AI_TREND_RELEASE_N 释放去抖帧数
JUMP_MAX     = 0.25    # AI_TREND_JUMP_MAX  ℃
SLOPE_ALPHA  = 0.10    # AI_TREND_SLOPE_ALPHA
TEMP_MAX     = 40.0    # TEMP_MAX（handle_config.h）温度上限，eta 的终点


def _least_squares(hist):
    """返回 (slope ℃/s, r2, jmp ℃)。hist 为按时间排序的 30 个量化值（单位 0.01 ℃）。"""
    n = WIN
    Sx = n * (n - 1) // 2
    Sxx = (n - 1) * n * (2 * n - 1) // 6
    Sy = sum(hist)
    Sxy = sum(i * y for i, y in enumerate(hist))
    den = n * Sxx - Sx * Sx
    num = n * Sxy - Sx * Sy
    slope = (num / den) / 100.0 if den else 0.0     # 0.01 ℃/帧 → ℃/s

    ybar = Sy / n / 100.0
    a = ybar - slope * (n - 1) / 2.0
    ss_tot = ss_res = 0.0
    jmp = 0
    prev = None
    for i, v in enumerate(hist):
        y = v / 100.0
        d = y - ybar;             ss_tot += d * d
        d = y - (a + slope * i);  ss_res += d * d
        if prev is not None:
            dj = abs(v - prev)
            if dj > jmp:
                jmp = dj
        prev = v
    r2 = (1.0 - ss_res / ss_tot) if ss_tot > 1e-6 else 0.0
    return slope, r2, jmp / 100.0


class Trend(object):
    """逐帧状态机，字段与 ai_monitor.c 的静态变量一一对应。"""

    def __init__(self):
        self.hist = []
        self.slope_s = 0.0
        self.slope_ok = False
        self.risk_f = 0.0
        self.rel_cnt = 0

    def step(self, temp):
        q = int(round(temp * 100))            # (int16)(temp * 100.0f)
        self.hist.append(q)
        if len(self.hist) > WIN:
            self.hist.pop(0)
        if len(self.hist) < WIN:              # 开机前 30 秒：无趋势可算
            return dict(slope=0.0, r2=0.0, jmp=0.0, risk=0.0, armed=False)

        slope, r2, jmp = _least_squares(self.hist)

        if not self.slope_ok:                 # 首帧直接对齐，否则要从 0 爬一分钟
            self.slope_s = slope
            self.slope_ok = True
        else:
            self.slope_s += SLOPE_ALPHA * (slope - self.slope_s)

        armed = (slope > RATE_MIN) and (r2 >= R2_MIN) and (jmp <= JUMP_MAX)

        risk = 0.0
        if armed:
            near_fit = slope * NEAR_K         # 固件用斜率外推，不是取窗口首尾差
            if near_fit >= NEAR_MIN:
                rate = self.slope_s * 60.0
                if rate < 0.01:
                    rate = 0.01
                eta = (TEMP_MAX - temp) / rate
                if eta < ETA_SLOW:
                    risk = (ETA_SLOW - eta) / (ETA_SLOW - ETA_FAST) * 100.0
                    risk = max(0.0, min(100.0, risk))

        # 释放去抖：判据失效后保持前值，连续 RELEASE_N 帧才归零
        if risk > 0.0:
            self.rel_cnt = 0
        elif self.rel_cnt < RELEASE_N:
            self.rel_cnt += 1
            if self.rel_cnt < RELEASE_N:
                risk = self.risk_f

        # 单帧限速
        if risk > self.risk_f + STEP_MAX:
            risk = self.risk_f + STEP_MAX
        if risk < self.risk_f - FALL_MAX:
            risk = self.risk_f - FALL_MAX
        risk = max(0.0, min(100.0, risk))

        self.risk_f = risk
        return dict(slope=slope, r2=r2, jmp=jmp, risk=risk, armed=armed)


def run(name, temp_at, seconds=480, trace=False, expect_peak=None, expect_zero=False):
    tr = Trend()
    armed_at = first_risk = None
    peak = end_risk = 0.0
    end_temp = temp_at(0)

    for t in range(seconds + 1):
        temp = temp_at(t)
        st = tr.step(temp)
        end_temp, end_risk = temp, st["risk"]
        if st["armed"] and armed_at is None:
            armed_at = t
        if st["risk"] > 0 and first_risk is None:
            first_risk = t
        if st["risk"] > peak:
            peak = st["risk"]
        if trace and t % 10 == 0:
            print("      t=%3d  T=%6.2f  slope=%+.4f  R2=%.3f  jmp=%.2f  risk=%5.1f%s"
                  % (t, temp, st["slope"], st["r2"], st["jmp"], st["risk"],
                     "  <-armed" if st["armed"] else ""))

    ok, note = True, ""
    if expect_zero and peak > 0:
        ok, note = False, "  [FAIL] 期望全程零风险，实测峰值 %.0f" % peak
    if expect_peak is not None and peak < expect_peak:
        ok, note = False, "  [FAIL] 期望出风险，实测峰值 %.0f" % peak

    print("%-24s 使能 t=%-4s 风险首现 t=%-4s 峰值 risk=%3.0f  末温 %6.2f℃  末 risk=%3.0f%s"
          % (name,
             armed_at if armed_at is not None else "—",
             first_risk if first_risk is not None else "—",
             peak, end_temp, end_risk, note))
    return ok


def main():
    trace = "--trace" in sys.argv

    print("趋势通道门限验证（对齐 ai_monitor.c:63-75 / 142-264）")
    print("  判据：slope > %.3f ℃/s (%.2f ℃/min)  AND  R² ≥ %.2f  AND  跳变 ≤ %.2f ℃"
          % (RATE_MIN, RATE_MIN * 60, R2_MIN, JUMP_MAX))
    print("  近端：slope × %d ≥ %.2f ℃      释放去抖 %d 帧      单帧 +%.0f / -%.0f"
          % (NEAR_K, NEAR_MIN, RELEASE_N, STEP_MAX, FALL_MAX))
    print("")

    results = []

    # A 纯环境漂移 0.32 ℃/min：刚越过 0.30 死区，但 eta 门限要到约 12 min 才破
    results.append(run("A 纯漂移 0.32℃/min", lambda t: 23.4 + 0.32 * t / 60.0,
                       seconds=1200, trace=trace, expect_peak=1))

    # B 漂移 + 注入，合计 1.32 ℃/min
    results.append(run("B 漂移+注入 1.32℃/min", lambda t: 23.4 + 1.32 * t / 60.0,
                       trace=trace, expect_peak=1))

    # C 无漂移、纯注入 1.00 ℃/min
    results.append(run("C 纯注入 1.00℃/min", lambda t: 23.4 + 1.00 * t / 60.0,
                       trace=trace, expect_peak=1))

    # D 文档下限场景 0.50 ℃/min
    results.append(run("D 纯注入 0.50℃/min", lambda t: 23.4 + 0.50 * t / 60.0,
                       trace=trace, expect_peak=1))

    # E 亚门限 0.25 ℃/min：低于 0.30 死区，必须全程零风险
    results.append(run("E 亚门限 0.25℃/min", lambda t: 23.4 + 0.25 * t / 60.0,
                       trace=trace, expect_zero=True))

    # F 人手贴近：5 s 内跃升 3 ℃，随后平台维持（干扰总时长 83 s）
    #   真实热传导的斜率在 1~3 ℃/min 量级，5 s 涨 3 ℃ 等价 36 ℃/min，
    #   既超跳变门限 0.25 ℃/帧，也是物理上不可能的热传导速率。
    def step83(t):
        if t < 60:
            return 23.4
        if t < 65:
            return 23.4 + 3.0 * (t - 60) / 5.0
        return 26.4
    results.append(run("F 阶跃 3℃/5s + 平台83s", step83, trace=trace, expect_zero=True))

    # G 单帧跳变 0.30 ℃（刚超 0.25 门限）→ 必须被 jmp 判据单独拦下，
    #   此时 R² 与斜率都可能"合格"，专测跳变这一条判据是否生效。
    def bump(t):
        return 23.4 + (0.30 if t >= 60 else 0.0)
    results.append(run("G 单帧跳变 0.30℃", bump, trace=trace, expect_zero=True))

    print("")
    if all(results):
        print("全部 %d 个场景符合预期。" % len(results))
        return 0
    print("存在不符合预期的场景，见上方 [FAIL]。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
