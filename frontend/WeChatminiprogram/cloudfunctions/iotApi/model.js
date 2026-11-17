/**
 * 设备数据模型 + 报警规则
 *
 * 属性名来自 IoTDA 物模型（service_id = stm32），共 13 个，见
 * docs/miniapp/design-system.md 第 10 节。这里不改名、不臆造字段。
 */

// 首页展示的 6 项核心指标（顺序即展示顺序）
const CORE_FIELDS = ['SHT30_T', 'SHT30_H', 'Gas', 'Body_IR', 'Light', 'Sound']
// 详情页折叠区展示的 4 项（用户已确认要展示）
const EXTRA_FIELDS = ['Temp_Rise', 'Fuse_Level', 'Fuse_Score', 'Dominant']
// 控制项
const CONTROL_FIELDS = ['Fill', 'Fan']

/**
 * 把上报值安全转成数字。兼容 27.1 / "27.1" / "27.1 ℃" 等形态。
 * 转不了返回 null，绝不用 0 兜底（避免把"没数据"显示成 0）。
 */
function toNum(v) {
  if (v === null || v === undefined || v === '') return null
  if (typeof v === 'number') return isFinite(v) ? v : null
  if (typeof v === 'boolean') return v ? 1 : 0
  const m = String(v).match(/-?\d+(\.\d+)?/)
  if (!m) return null
  const n = parseFloat(m[0])
  return isFinite(n) ? n : null
}

/** 把上报值安全转成布尔。兼容 true / "true" / 1 / "1" / "ON" */
function toBool(v) {
  if (v === null || v === undefined || v === '') return null
  if (typeof v === 'boolean') return v
  const s = String(v).trim().toLowerCase()
  if (s === 'true' || s === '1' || s === 'on' || s === 'yes') return true
  if (s === 'false' || s === '0' || s === 'off' || s === 'no') return false
  return null
}

/**
 * 规范化原始属性对象：保留原始值，同时给出类型化取值
 * 返回 { raw, num, bool, text }
 */
function normalizeProps(raw) {
  const src = raw || {}
  const out = { raw: src, num: {}, bool: {}, text: {} }
  Object.keys(src).forEach(function (k) {
    const v = src[k]
    out.num[k] = toNum(v)
    out.bool[k] = toBool(v)
    out.text[k] = v === null || v === undefined ? '' : String(v)
  })
  return out
}

/** 判断燃气是否安全（Gas 是字符串枚举，默认只有 Safe 表示安全） */
function isGasSafe(text) {
  const s = String(text || '').trim().toLowerCase()
  if (!s) return null
  return s === 'safe'
}

/** 判断设备是否在线 */
function isOnline(status) {
  return String(status || '').toUpperCase() === 'ONLINE'
}

// 阈值（2026-11-12 用户确认：AI 评分是减分制，100 正常、越低越偏离）
const THRESHOLD = {
  AI_SCORE_L1: 20, // AI_Score < 20 → 严重（减分制，不是 >= 70）
  AI_SCORE_MAX: 100, // 合理上限；超过视为无效/哨兵值（真实影子出现过 255），不报警
  TEMP_WARNING: 35, // SHT30_T >= 35℃ → 警告
  OFFLINE_INFO_MIN: 5, // 离线 > 5 分钟 → 提示
}

const LEVEL_ORDER = { info: 1, warning: 2, severe: 3 }

/**
 * 评估报警规则
 * @param {Object} norm normalizeProps 的结果
 * @param {boolean} online
 * @param {Object} prevState 上一次的报警状态 { code: level }
 * @returns {{hits: Array, state: Object, raised: Array}}
 *   hits   当前命中的全部规则
 *   state  当前 code → level，用于下次比较
 *   raised 本次新出现或等级升高的规则（只有这些才落库 + 推送）
 */
function evaluateRules(norm, online, prevState) {
  const prev = prevState || {}
  const hits = []

  // 严重：可燃气体不是 Safe
  const gasSafe = isGasSafe(norm.text.Gas)
  if (gasSafe === false) {
    hits.push({
      code: 'GAS_LEAK',
      level: 'severe',
      title: '可燃气体异常',
      detail: '可燃气体上报为 ' + norm.text.Gas + '（非 Safe）',
    })
  }

  // 严重：设备端 AI 评分低于 20（减分制，100 正常、越低越偏离）
  // 上限 100 是为了挡哨兵值：真实影子出现过 255，那是无效读数，不能当「远低于 20」报警
  const ai = norm.num.AI_Score
  if (ai !== null && ai < THRESHOLD.AI_SCORE_L1 && ai >= 0 && ai <= THRESHOLD.AI_SCORE_MAX) {
    hits.push({
      code: 'AI_SCORE_LOW',
      level: 'severe',
      title: 'AI 评分偏低',
      detail: '设备端 AI 评分为 ' + ai + '，已低于 ' + THRESHOLD.AI_SCORE_L1 + ' 的严重阈值',
    })
  }

  // 警告：温度过高
  const t = norm.num.SHT30_T
  if (t !== null && t >= THRESHOLD.TEMP_WARNING) {
    hits.push({
      code: 'TEMP_HIGH',
      level: 'warning',
      title: '温度偏高',
      detail: '当前温度 ' + t + '℃，已超过 ' + THRESHOLD.TEMP_WARNING + '℃',
    })
  }

  // 警告：检测到人体
  if (norm.bool.Body_IR === true) {
    hits.push({
      code: 'BODY_DETECTED',
      level: 'warning',
      title: '检测到人体活动',
      detail: '人体红外传感器上报有人',
    })
  }

  // 提示：设备离线
  if (online === false) {
    hits.push({
      code: 'DEVICE_OFFLINE',
      level: 'info',
      title: '设备离线',
      detail: '设备当前不在线，超过 ' + THRESHOLD.OFFLINE_INFO_MIN + ' 分钟未上报',
    })
  }

  const state = {}
  const raised = []
  hits.forEach(function (h) {
    state[h.code] = h.level
    const before = prev[h.code]
    const beforeRank = before ? LEVEL_ORDER[before] || 0 : 0
    const nowRank = LEVEL_ORDER[h.level] || 0
    if (nowRank > beforeRank) raised.push(h)
  })

  return { hits: hits, state: state, raised: raised }
}

module.exports = {
  CORE_FIELDS: CORE_FIELDS,
  EXTRA_FIELDS: EXTRA_FIELDS,
  CONTROL_FIELDS: CONTROL_FIELDS,
  THRESHOLD: THRESHOLD,
  toNum: toNum,
  toBool: toBool,
  normalizeProps: normalizeProps,
  isGasSafe: isGasSafe,
  isOnline: isOnline,
  evaluateRules: evaluateRules,
}
