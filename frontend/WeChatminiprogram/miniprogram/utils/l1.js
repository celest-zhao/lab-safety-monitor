/**
 * L1 异常判定 + 「最近一次越线的时间点」（趋势页「异常汇总」用的纯函数）
 *
 * 为什么要单独一个文件：
 *   这套阈值和趋势页的汇总、以后可能出现的日报都有关，写在页面里就变成
 *   「只有打开趋势页才测得到」。纯函数放在 utils 下才好写单测。
 *
 * 阈值本身只在 model.js 的 THRESHOLD 里定义一次，这里只引用。
 *
 * 2026-11-12 用户定的 L1 口径：
 *   温度 ≥ 35℃ / 湿度 ≥ 80% / 光强过亮 / AI 评分 < 20
 *   AI 评分是**减分制**：100 才正常，越低越偏离，所以方向是「小于」。
 *
 * 2026-11-12 晚改版：原先每行显示「累计异常时长」，用户不接受
 *   （原话：「异常汇总怎么显示几分钟啊，让他显示那个异常的时间点」）。
 *   现在每项只显示**最近一次越线的时间点**（外加那次的读数），一项一行。
 *   只有 4 个指标，所以最多 4 行，永远不会堆成一坨。
 *
 *   为什么不做「列出全部异常时段」：历史点只在数值变化时写入，一次抖动就是
 *   一堆碎点，列出来会变成几十行；而且用户在手机上要的是「什么时候不对」，
 *   不是审计表。要看形状去上面的折线图。
 */

const model = require('./model')

const T = model.THRESHOLD

const RULES = [
  { key: 'SHT30_T', label: '温度', rule: '高于 ' + T.TEMP_WARNING + '℃', kind: 'high', limit: T.TEMP_WARNING },
  { key: 'SHT30_H', label: '湿度', rule: '高于 ' + T.HUMI_WARNING + '%', kind: 'high', limit: T.HUMI_WARNING },
  { key: 'Light', label: '光强', rule: '高于 ' + T.LIGHT_HIGH, kind: 'high', limit: T.LIGHT_HIGH },
  { key: 'AI_Score', label: 'AI 评分', rule: '低于 ' + T.AI_SCORE_L1, kind: 'low', limit: T.AI_SCORE_L1 },
]

const FIELDS = RULES.map(function (r) {
  return r.key
})

/**
 * 单个读数是否越线。
 * kind='low' 时要先排除哨兵值：设备上报过 255，那是无效读数，
 * 直接判 < 20 会凭空造出一段「评分极低」的异常时间。
 */
function hit(rule, v) {
  if (v === null || v === undefined) return false
  if (rule.kind === 'high') return v >= rule.limit
  return v >= 0 && v <= T.AI_SCORE_MAX && v < rule.limit
}

function pad2(n) {
  return n < 10 ? '0' + n : '' + n
}

/**
 * 时间点文案。
 *
 *   当天       → HH:mm:ss   设备 5 秒一报，异常常常只持续十几秒，
 *                            不给秒的话「18:32」会和另一个 18:32 的时段混起来
 *   同一年     → MM-DD HH:mm
 *   跨年       → YYYY-MM-DD HH:mm
 *
 * 不判断「几秒前」这种相对时间：卡片是定时刷新的，相对时间会站着不动地骗人。
 */
function fmtPoint(ts, now) {
  if (!ts) return ''
  const d = new Date(ts)
  const n = new Date(now || Date.now())
  const sameDay =
    d.getFullYear() === n.getFullYear() && d.getMonth() === n.getMonth() && d.getDate() === n.getDate()
  if (sameDay) {
    return pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds())
  }
  if (d.getFullYear() === n.getFullYear()) {
    return pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes())
  }
  return (
    d.getFullYear() +
    '-' +
    pad2(d.getMonth() + 1) +
    '-' +
    pad2(d.getDate()) +
    ' ' +
    pad2(d.getHours()) +
    ':' +
    pad2(d.getMinutes())
  )
}

/** 读数文案，用 model.FIELD_META 的小数位与单位，不自己写第二份格式 */
function valText(key, v) {
  const meta = model.FIELD_META[key] || {}
  const num = Number(v)
  if (v === null || v === undefined || !isFinite(num)) return ''
  const digits = meta.digits === undefined ? 0 : meta.digits
  return num.toFixed(digits) + (meta.unit || '')
}

/**
 * 扫一遍某个指标的历史点，找出最近一次越线。
 *
 * 历史点应该是升序的，但这里用「取最大 ts」而不是「取最后一个命中」：
 * 万一云端返回的顺序变了，也不会把最旧的一次当成「最近一次」摆到用户面前。
 *
 * @param {Array} points [{ ts, v }]
 * @param {Object} rule  RULES 里的一条
 * @returns {{ count, hits, ts, v }}  hits = 越线点数；ts/v 是最近一次越线的点
 */
function scan(points, rule) {
  const list = points || []
  let hits = 0
  let lastTs = null
  let lastV = null
  for (let i = 0; i < list.length; i++) {
    const pt = list[i]
    if (!hit(rule, pt.v)) continue
    hits += 1
    if (lastTs === null || (pt.ts || 0) >= lastTs) {
      lastTs = pt.ts || 0
      lastV = pt.v
    }
  }
  return { count: list.length, hits: hits, ts: lastTs, v: lastV }
}

/**
 * 四个指标一起算，直接给页面用的结构。
 *
 * timeText 三种文案必须能区分：
 *   「无数据」= 这个属性根本没有历史点（设备没上报过）
 *   「正常」= 有数据，都在阈值以内
 *   「18:32:05」= 有越线，给最近一次的时间点
 *
 * @param {Object} byField { SHT30_T: [...], Light: [...], ... }
 */
function summarize(byField, now) {
  const at = now || Date.now()
  const map = byField || {}
  let bad = 0
  const items = RULES.map(function (r) {
    const out = scan(map[r.key], r)
    const isBad = out.hits > 0
    if (isBad) bad += 1
    return {
      key: r.key,
      label: r.label,
      rule: r.rule,
      bad: isBad,
      hits: out.hits,
      timeText: !out.count ? '无数据' : isBad ? fmtPoint(out.ts, at) : '正常',
      valText: isBad ? valText(r.key, out.v) : '',
    }
  })
  return { items: items, bad: bad }
}

module.exports = {
  RULES: RULES,
  FIELDS: FIELDS,
  hit: hit,
  fmtPoint: fmtPoint,
  valText: valText,
  scan: scan,
  summarize: summarize,
}
