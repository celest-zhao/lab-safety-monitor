# -*- coding: utf-8 -*-
"""量化对比：融合分 Fuse 与趋势风险 risk 的比例系数 k = Fuse/risk"""
import re, sys

def parse(path):
    rows, ai, t0 = [], {}, None
    for line in open(path, 'r', encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line: continue
        ts = re.match(r'\[(\d\d):(\d\d):(\d\d)\.(\d+)\]', line)
        sec = int(ts.group(1))*3600+int(ts.group(2))*60+int(ts.group(3))+int(ts.group(4))/1000.0 if ts else None
        m = re.search(r'\| T:([\d.]+)C.*Body:(\w+[ a-z]*) Gas:\w+ \| Fuse:([\d.]+) \w+ \|.*\| AI:\s*(\d+)', line)
        if m and sec is not None:
            rows.append(dict(sec=sec, T=float(m.group(1)), body=m.group(2).strip(), fuse=float(m.group(3)), ai=int(m.group(4))))
            continue
        m = re.search(r'\[AI\] in=\[([\d.]+).*risk=(\d+) sim=(\d+) r=([+-]?[\d.]+)C/min R2=([\d.]+)', line)
        if m and sec is not None:
            ai[int(round(sec))] = dict(risk=int(m.group(2)), r=float(m.group(4)), R2=float(m.group(5)))
            continue
        if '[TTEST] begin' in line and sec is not None and t0 is None:
            t0 = sec
    return rows, ai, t0

if len(sys.argv) < 2:
    print('用法: python fit_k.py 日志1 [日志2 ...]')
    sys.exit(1)
for p in sys.argv[1:]:
    rows, ai, t0 = parse(p)
    print('='*72)
    print(p.split('\\')[-1], ' TTEST@', t0)
    ks, pairs = [], []
    for x in rows:
        if x['body'] != 'No one':
            continue
        a = ai.get(int(round(x['sec'])))
        if not a or a['risk'] <= 30:
            continue
        k = x['fuse'] / a['risk']
        ks.append(k); pairs.append((x['sec']-t0, a['risk'], x['fuse'], k))
    if ks:
        ks_sorted = sorted(ks)
        print('样本 %d 个（Body=No one 且 risk>30）' % len(ks))
        print('k = Fuse/risk : min=%.3f 中位=%.3f max=%.3f 均值=%.3f' % (
            ks_sorted[0], ks_sorted[len(ks)//2], ks_sorted[-1], sum(ks)/len(ks)))
        print('逐点：')
        for t, r, f, k in pairs[::max(1, len(pairs)//12)]:
            print('   t=%+5.0fs risk=%3d Fuse=%5.1f  k=%.3f' % (t, r, f, k))
