# -*- coding: utf-8 -*-
import re, sys

def parse(path):
    rows, ai, tt, t0 = [], [], [], None
    for line in open(path, 'r', encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line: continue
        ts = re.match(r'\[(\d\d):(\d\d):(\d\d)\.(\d+)\]', line)
        sec = int(ts.group(1))*3600+int(ts.group(2))*60+int(ts.group(3))+int(ts.group(4))/1000.0 if ts else None
        m = re.search(r'T:([\d.]+)C H:([\d.]+)% L:\s*(\d+) lx \| Body:(\w+[ a-z]*) Gas:(\w+) \| Fuse:([\d.]+) (\w+) \| Plat:(\w+) P:(\w+) (\w+) \| F:(\d+) L:(\d+) \| AI:\s*(\d+)', line)
        if m and sec is not None:
            rows.append(dict(sec=sec, T=float(m.group(1)), H=float(m.group(2)), L=int(m.group(3)), fuse=float(m.group(6)),
                             lvl=m.group(7), plat=m.group(8), P=m.group(9)+m.group(10), F=int(m.group(11)), ai=int(m.group(13))))
            continue
        m = re.search(r'\[AI\] in=\[([\d.]+) [\d.]+ \d+\] raw=(\d+) vote=(\d+)/(\d+) model=(\d+) trend=(\d+) risk=(\d+) sim=(\d+) r=([+-]?[\d.]+)C/min R2=([\d.]+)', line)
        if m and sec is not None:
            ai.append(dict(sec=sec, T=float(m.group(1)), raw=int(m.group(2)), vote=int(m.group(3)), n=int(m.group(4)),
                           model=int(m.group(5)), trend=int(m.group(6)), risk=int(m.group(7)), sim=int(m.group(8)),
                           r=float(m.group(9)), R2=float(m.group(10))))
            continue
        if '[TTEST]' in line and sec is not None and t0 is None:
            t0 = sec
    return rows, ai, t0

if len(sys.argv) < 3:
    print('用法: python cmp_detail.py 日志A 日志B')
    sys.exit(1)
A, B = sys.argv[1], sys.argv[2]
ra, aa, ta = parse(A)
rb, ab, tb = parse(B)

def near(lst, sec, key='sec', tol=3.0):
    if not lst: return None
    best = min(lst, key=lambda x: abs(x[key]-sec))
    return best if abs(best[key]-sec) <= tol else None

print('%-6s | %-42s | %-42s' % ('t(s)', 'ADJ(11123)', 'UNADJ(111233)'))
print('%-6s | %-8s %-6s %-6s %-6s %-6s %-7s | %-8s %-6s %-6s %-6s %-6s %-7s' % ('', 'T/Fuse', 'AI', 'risk', 'r', 'R2', 'F', 'T/Fuse', 'AI', 'risk', 'r', 'R2', 'F'))
print('-'*100)
step = 10
t = 0
while t <= 480:
    sa, sb = ta+t, tb+t
    xa, xb = near(ra, sa), near(rb, sb)
    ya, yb = near(aa, sa), near(ab, sb)
    if xa is None and xb is None:
        t += step; continue
    fa = '%.1f/%.1f' % (xa['T'], xa['fuse']) if xa else '-'
    fb = '%.1f/%.1f' % (xb['T'], xb['fuse']) if xb else '-'
    def g(y, k): return ('%d' % y[k]) if y else '-'
    def gf(y, k): return ('%.2f' % y[k]) if y else '-'
    print('%-6d | %-8s %-6s %-6s %-6s %-6s %-7s | %-8s %-6s %-6s %-6s %-6s %-7s' % (
        t, fa, g(ya,'model'), g(ya,'risk'), gf(ya,'r'), gf(ya,'R2'), (xa['F'] if xa else '-'),
        fb, g(yb,'model'), g(yb,'risk'), gf(yb,'r'), gf(yb,'R2'), (xb['F'] if xb else '-')))
    t += step

print()
print('--- 关键事件：F>0 / Plat / 级别 / AI 触底 ---')
for name, rows, ai_, t0_ in (('ADJ', ra, aa, ta), ('UNADJ', rb, ab, tb)):
    print('[%s]' % name)
    for x in rows:
        if x['F'] > 0 or x['plat'] != 'OFF' or x['lvl'] != 'L1':
            print('  t=%+5.0fs T=%.1f Fuse=%.1f %s Plat=%s %s F=%d AI=%d' % (x['sec']-t0_, x['T'], x['fuse'], x['lvl'], x['plat'], x['P'], x['F'], x['ai']))
    lo = min(ai_, key=lambda y: y['ai'])
    print('  AI min=%d at t=%+.0fs (risk=%d r=%.2f R2=%.2f)' % (lo['ai'], lo['sec']-t0_, lo['risk'], lo['r'], lo['R2']))
    rr = max(ai_, key=lambda y: y['r'])
    print('  r max=%.2f at t=%+.0fs (risk=%d)' % (rr['r'], rr['sec']-t0_, rr['risk']))
    print('  vote>0 count=%d' % sum(1 for y in ai_ if y['vote'] > 0))
