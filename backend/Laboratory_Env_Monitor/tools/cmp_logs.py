# -*- coding: utf-8 -*-
import re, sys, os

def parse(path):
    rows = []      # stream lines
    ai = []        # AI lines
    tt = []        # TTEST lines
    t0 = None
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            ts = re.match(r'\[(\d\d):(\d\d):(\d\d)\.(\d+)\]', line)
            sec = None
            if ts:
                sec = int(ts.group(1))*3600 + int(ts.group(2))*60 + int(ts.group(3)) + int(ts.group(4))/1000.0
            m = re.search(r'T:([\d.]+)C H:([\d.]+)% L:\s*(\d+) lx \| Body:(\w+[ a-z]*) Gas:(\w+) \| Fuse:([\d.]+) (\w+) \| Plat:(\w+) P:(\w+) (\w+) \| F:(\d+) L:(\d+) \| AI:\s*(\d+) (\w+)', line)
            if m and sec is not None:
                rows.append(dict(sec=sec, T=float(m.group(1)), H=float(m.group(2)), L=int(m.group(3)),
                                 body=m.group(4).strip(), gas=m.group(5), fuse=float(m.group(6)), lvl=m.group(7),
                                 plat=m.group(8), F=int(m.group(11)), Lf=int(m.group(12)), ai=int(m.group(13))))
                continue
            m = re.search(r'\[AI\] in=\[([\d.]+) ([\d.]+) (\d+)\] raw=(\d+) vote=(\d+)/(\d+) model=(\d+) trend=(\d+) risk=(\d+) sim=(\d+) r=([+-]?[\d.]+)C/min R2=([\d.]+)', line)
            if m and sec is not None:
                ai.append(dict(sec=sec, T=float(m.group(1)), raw=int(m.group(4)), vote=int(m.group(5)), n=int(m.group(6)),
                               model=int(m.group(7)), trend=int(m.group(8)), risk=int(m.group(9)), sim=int(m.group(10)),
                               r=float(m.group(11)), R2=float(m.group(12))))
                continue
            m = re.search(r'\[TTEST\] (begin rate=([\d.]+) C/min span=([\d.]+) C|t=(\d+)s ofs=\+?([\d.-]+)C virt=([\d.]+)C|ramp done.*)', line)
            if m:
                if t0 is None and sec is not None:
                    t0 = sec
                tt.append(dict(sec=sec, raw=line))
    return rows, ai, tt, t0

if len(sys.argv) < 2:
    print('用法: python cmp_logs.py 日志1 [日志2 ...]')
    sys.exit(1)
for p in sys.argv[1:]:
    rows, ai, tt, t0 = parse(p)
    print('='*70)
    print(os.path.basename(p))
    print('stream=%d ai=%d ttest=%d ttest_start=%s' % (len(rows), len(ai), len(tt), t0))
    if t0:
        last = rows[-1]['sec'] - t0
        print('log covers t=0..%.0fs after TTEST begin' % last)
    fusemax = max(rows, key=lambda x: x['fuse'])
    print('Fuse max=%.1f at t=%+.0fs  AImin=%d  risk_max=%d  r_max=%.2f  R2max=%.2f' % (
        fusemax['fuse'], fusemax['sec']-(t0 or 0), min(x['ai'] for x in rows), max(x['risk'] for x in ai),
        max(x['r'] for x in ai), max(x['R2'] for x in ai)))
    print('Plat ON count=%d, F>0 count=%d, L level max=%s' % (
        sum(1 for x in rows if x['plat'] != 'OFF'), sum(1 for x in rows if x['F'] > 0),
        max(rows, key=lambda x: int(x['lvl'][1:]))['lvl']))
