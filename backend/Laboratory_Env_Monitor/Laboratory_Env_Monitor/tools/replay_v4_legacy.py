# -*- coding: utf-8 -*-
"""【历史脚本】v4→v5 融合逻辑对照回放 —— 仅用于复现迁移当时的日志等级，勿作当前固件验收依据。

用途
    拿两份历史串口日志，先按 v4 逻辑重算，确认能复现日志里记录的等级（证明解析与
    模型正确），再按 v5 逻辑重算，量化"规则只做 floor、趋势只压 L2 门槛"这一改动
    把等级拉回了多少。这是迁移期的等价性证据，不是当前固件的验收工具。

⚠ 固化常量已与现固件不一致，属刻意保留
    W_GAS   0.35  →  现固件 0.55   （人体模块并入气体，不再单独计分）
    W_BODY  0.20  →  现固件 0.00
    BOOST   14.0  →  趋势项上限 14 分（v5 起趋势只做 L2 门槛折扣，不进总分）
    level_filter 3s/5s 与现固件的 FUSION_LEVEL_UP_MS / DOWN_MS 仍一致
    保留旧权重，才能让"v4 逻辑复现当时日志"这个对照成立；若拿新日志跑，等级会偏低，
    这是预期行为，不是 bug。

当前固件的门限验证请用 gate_check.py（已与 ai_monitor.c 逐行对齐）。

旧逻辑 = 规则写 score（规则 1/2 置 75）+ 趋势 0.14×risk 加分 + 规则 3 入侵 +20 / floor L3
新逻辑 = 规则只做 floor + 趋势只做 L2 门槛折扣 14×risk/100

用法
    python replay_v4_legacy.py 基线日志.txt 调整后日志.txt
"""
import re, sys

TEMP_MAX, TEMP_MIN = 40.0, 0.0
HUMI_MIN, HUMI_MAX, HUMI_STEP = 20.0, 80.0, 30.0
LIGHT_LOW, LIGHT_DARK = 100, 20
W_TEMP, W_GAS, W_BODY, W_LIGHT = 0.30, 0.35, 0.20, 0.15   # v4 权重，见文件头
TEMP_MIX = 0.50
L2, L3, L4 = 25.0, 50.0, 75.0
BOOST = 14.0
AI_DEADBAND, AI_GAIN, AI_MIN_SIM = 25.0, 0.10, 10.0
UP_MS, DOWN_MS = 3, 5


def score_temp(T, risk):
    dev = 0.0
    if T < TEMP_MIN: dev = TEMP_MIN - T
    elif T > TEMP_MAX: dev = T - TEMP_MAX
    s = min(100.0, dev / 10.0 * 100.0)
    return max(dev and s, risk)


def score_humi(H):
    if HUMI_MIN <= H <= HUMI_MAX: return 0.0
    dev = (HUMI_MIN - H) if H < HUMI_MIN else (H - HUMI_MAX)
    return min(100.0, dev / HUMI_STEP * 100.0)


def score_light(L):
    if L >= LIGHT_LOW: return 0.0
    if L <= LIGHT_DARK: return 100.0
    return (LIGHT_LOW - L) * 100.0 / (LIGHT_LOW - LIGHT_DARK)


def level_of(score, discount=0.0):
    l2 = max(0.0, L2 - discount)
    if score < l2: return 0
    if score < L3: return 1
    if score < L4: return 2
    return 3


def level_filter(state, raw, t):
    """state = [stable, pending, pending_t]"""
    st, pend, pt = state
    if raw == st:
        return [st, raw, t]
    if raw == 3:
        return [3, 3, t]
    if pend != raw:
        return [st, raw, t]
    hold = UP_MS if raw > st else DOWN_MS
    if t - pt >= hold:
        st = raw
    return [st, raw, pt]


def parse(path):
    rows, ai = [], {}
    t0 = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        ts = re.match(r'\[(\d\d):(\d\d):(\d\d)\.(\d+)\]', line)
        if not ts: continue
        sec = int(ts.group(1))*3600+int(ts.group(2))*60+int(ts.group(3))+int(ts.group(4))/1000
        if '[TTEST] begin' in line and t0 is None: t0 = sec
        m = re.search(r'T:([\d.]+)C H:([\d.]+)% L:\s*(\d+) lx \| Body:(\w+[ a-z]*) Gas:L(\d) \| Fuse:[\d.]+ L(\d)', line)
        if m:
            rows.append(dict(sec=sec, t=sec-(t0 or sec), T=float(m.group(1)), H=float(m.group(2)),
                             L=int(m.group(3)), body=(m.group(4).strip()=='Someone'),
                             gas=int(m.group(5)), logL=int(m.group(6))))
            continue
        m = re.search(r'\[AI\] in=\[[\d.]+ .*model=(\d+) trend=(\d+) risk=(\d+) sim=\d+', line)
        if m:
            ai[int(round(sec))] = dict(model=int(m.group(1)), risk=int(m.group(3)))
    return rows, ai


def replay(path, tag):
    rows, ai = parse(path)
    ai_keys = sorted(ai.keys())
    body_run = 0            # 连续"有人"秒数
    st_old = [0, 0, 0]
    st_new = [0, 0, 0]
    last_risk, last_model = 0, 100
    out = []
    ki = 0
    for r in rows:
        while ki < len(ai_keys) and ai_keys[ki] <= r['sec'] + 0.001:
            last_risk, last_model = ai[ai_keys[ki]]['risk'], ai[ai_keys[ki]]['model']
            ki += 1
        body_run = body_run + 1 if r['body'] else 0
        body_lvl = 0 if body_run == 0 else (1 if body_run < 3 else (2 if body_run < 8 else 3))
        body_score = {0: 0.0, 1: 40.0, 2: 70.0, 3: 100.0}[body_lvl]
        gas_score = 0.0
        tfac = score_temp(r['T'], last_risk) * TEMP_MIX + score_humi(r['H']) * (1 - TEMP_MIX)
        base = W_TEMP*tfac + W_GAS*gas_score + W_BODY*body_score + W_LIGHT*score_light(r['L'])

        model_boost = max(0.0, (100.0 - max(last_model, AI_MIN_SIM) - AI_DEADBAND) * AI_GAIN)

        # 旧：趋势 +0.14*risk
        s_old = min(100.0, base + BOOST*last_risk/100.0 + model_boost)
        base_lvl = level_of(base)
        l_old = level_of(s_old)
        if base_lvl >= 1 and l_old < base_lvl: l_old = base_lvl
        if base_lvl < 1 and l_old > 1: l_old = 1
        if l_old >= 3 and base_lvl < 3: l_old = 2

        # 新：趋势只压 L2 门槛
        s_new = min(100.0, base + model_boost)
        disc = BOOST*last_risk/100.0 if last_risk > 0 else 0.0
        l_new = level_of(s_new, disc)
        if base_lvl >= 1 and l_new < base_lvl: l_new = base_lvl
        if base_lvl < 1 and l_new > 1: l_new = 1
        if l_new >= 3 and base_lvl < 3: l_new = 2

        st_old = level_filter(st_old, l_old, r['t'])
        st_new = level_filter(st_new, l_new, r['t'])
        out.append((r['t'], s_old, s_new, st_old[0]+1, st_new[0]+1, r['logL'], last_risk, disc))

    print('='*78)
    print('%s  样本 %d 帧' % (tag, len(out)))
    old_match = sum(1 for o in out if o[3] == o[5])
    print('旧逻辑复现日志等级：%d/%d 帧一致 (%.1f%%)' % (old_match, len(out), 100.0*old_match/len(out)))
    diff = [o for o in out if o[4] != o[3]]
    print('新旧等级差异帧：%d' % len(diff))
    for o in diff[:8]:
        print('   t=%+5.0fs  oldScore=%5.1f newScore=%5.1f  L%d->L%d  (risk=%3d disc=%.1f) 日志L%d'
              % (o[0], o[1], o[2], o[3], o[4], o[6], o[7], o[5]))
    if diff:
        print('   首次分歧 t=%+.0fs，末次 t=%+.0fs' % (diff[0][0], diff[-1][0]))
    mx_old = max(o[1] for o in out); mx_new = max(o[2] for o in out)
    print('峰值分：旧 %.1f  新 %.1f  （差 %.1f）' % (mx_old, mx_new, mx_old-mx_new))
    lv_old = max(o[3] for o in out); lv_new = max(o[4] for o in out)
    print('最高等级：旧 L%d  新 L%d' % (lv_old, lv_new))
    return out


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    replay(sys.argv[1], '基线日志（应能复现日志等级）')
    replay(sys.argv[2], '调整后日志（看新逻辑把它拉回多少）')
