/**
 * 前端数据模型：属性元数据 + 展示格式化
 *
 * 注意：这里和 cloudfunctions/iotApi/model.js 有一小段重复的 toNum / toBool / isGasSafe，
 * 是刻意的 —— 云函数和小程序运行在不同环境，云开发不支持直接共享源码目录。
 * 两边的规则必须保持一致：改一边就要同步改另一边。
 */

// 13 个真实属性的展示元数据（属性名来自 IoTDA 物模型，不得改名）
const FIELD_META = {
  SHT30_T: { label: '温度', unit: '℃', kind: 'num', digits: 1 },
  SHT30_H: { label: '湿度', unit: '%', kind: 'num', digits: 1 },
  Gas: { label: '可燃气体', unit: '', kind: 'gas' },
  Body_IR: { label: '人体红外', unit: '', kind: 'bodyIr' },
  Light: { label: '光强', unit: '', kind: 'num', digits: 0 },
  Sound: { label: '声音', unit: '', kind: 'sound' },
  Fill: { label: '补光', unit: '', kind: 'onoff' },
  Fan: { label: '风扇', unit: '', kind: 'onoff' },
  AI_Score: { label: 'AI 评分', unit: '', kind: 'ai' },
  Temp_Rise: { label: '温升', unit: '℃', kind: 'num', digits: 1 },
  Fuse_Level: { label: '融合等级', unit: '', kind: 'num', digits: 0 },
  Fuse_Score: { label: '融合分数', unit: '', kind: 'num', digits: 0 },
  Dominant: { label: '主导因素', unit: '', kind: 'text' },
}

// 首页 6 项核心指标（顺序固定）
const CORE_FIELDS = ['SHT30_T', 'SHT30_H', 'Gas', 'Body_IR', 'Light', 'Sound']
// 详情页折叠区 4 项
const EXTRA_FIELDS = ['Temp_Rise', 'Fuse_Level', 'Fuse_Score', 'Dominant']
// 可画趋势的数值型属性
const TREND_FIELDS = [
  { key: 'SHT30_T', label: '温度', unit: '℃' },
  { key: 'SHT30_H', label: '湿度', unit: '%' },
  { key: 'Light', label: '光强', unit: '' },
  { key: 'AI_Score', label: 'AI 评分', unit: '' },
]

/**
 * 阈值口径（2026-11-12 用户确认）
 *
 * AI 评分是**减分制**：100 = 完全正常，分数越低越偏离，不是「越高越危险」。
 *   < 20  → 异常（L1）
 *   20~50 → 偏离，注意
 *   ≥ 50  → 正常
 * 所以下面所有用到评分的地方都得用「小于」，方向搞反会变成「分数越健康越报警」。
 *
 * 其余三项是 L1 异常的判定线，趋势页的异常汇总也读这里，改一处两边一起变。
 */
const THRESHOLD = {
  AI_SCORE_L1: 20, // 评分 < 20 → 异常
  AI_SCORE_WARN: 50, // 评分 < 50 → 偏离
  AI_SCORE_MAX: 100, // 合理上限；真实设备出现过 255 这种哨兵值，超过就当无效，不能当「满分」
  TEMP_WARNING: 35, // 温度 ≥ 35℃ → 异常
  HUMI_WARNING: 80, // 湿度 ≥ 80% → 异常
  LIGHT_HIGH: 800, // 光强 ≥ 800 → 过亮（实测常态 235~386，单位未确认，阈值可调）
}

function toNum(v) {
  if (v === null || v === undefined || v === '') return null
  if (typeof v === 'number') return isFinite(v) ? v : null
  if (typeof v === 'boolean') return v ? 1 : 0
  const m = String(v).match(/-?\d+(\.\d+)?/)
  if (!m) return null
  const n = parseFloat(m[0])
  return isFinite(n) ? n : null
}

function toBool(v) {
  if (v === null || v === undefined || v === '') return null
  if (typeof v === 'boolean') return v
  const s = String(v).trim().toLowerCase()
  if (s === 'true' || s === '1' || s === 'on' || s === 'yes') return true
  if (s === 'false' || s === '0' || s === 'off' || s === 'no') return false
  return null
}

function isGasSafe(v) {
  const s = String(v === null || v === undefined ? '' : v).trim().toLowerCase()
  if (!s) return null
  return s === 'safe'
}

/**
 * 评分 → 色调。
 *
 * 减分制：**低于** 20 才是异常。方向写反会变成「分数越高越报警」，
 * 之前就是按「>= 70 报警」写的，和设备的真实语义正好相反。
 */
function scoreTone(n) {
  if (n < THRESHOLD.AI_SCORE_L1) return 'error'
  if (n < THRESHOLD.AI_SCORE_WARN) return 'warning'
  return 'success'
}

/** 哨兵值（真实设备上报过 255）：既不是满分，也不该报警，当读数无效处理 */
function isScoreInvalid(n) {
  return n === null || n > THRESHOLD.AI_SCORE_MAX
}

/** 数字保留小数位，null 时返回占位符 */
function fixed(n, digits) {
  if (n === null || n === undefined) return '--'
  return n.toFixed(digits === undefined ? 0 : digits)
}

/**
 * 把单个属性格式化成可直接渲染的 { label, value, unit, tone }
 * tone: normal | success | warning | error | muted
 */
function formatField(field, props) {
  const meta = FIELD_META[field] || { label: field, unit: '', kind: 'text' }
  const raw = props ? props[field] : undefined
  // big = 数值型用大字号，文字型（有人/无人/有声音）用小字号避免换行
  const out = {
    key: field,
    label: meta.label,
    unit: meta.unit,
    value: '--',
    tone: 'muted',
    big: meta.kind === 'num' || meta.kind === 'ai',
  }

  if (raw === undefined || raw === null || raw === '') return out

  switch (meta.kind) {
    case 'num': {
      const n = toNum(raw)
      if (n === null) {
        out.value = String(raw)
        out.tone = 'normal'
      } else {
        out.value = fixed(n, meta.digits)
        out.tone = 'normal'
      }
      // 温度超阈值标红
      if (field === 'SHT30_T' && n !== null && n >= THRESHOLD.TEMP_WARNING) out.tone = 'error'
      break
    }
    case 'gas': {
      const safe = isGasSafe(raw)
      if (safe === null) {
        out.value = String(raw)
        out.tone = 'normal'
      } else if (safe) {
        out.value = '正常'
        out.tone = 'success'
      } else {
        out.value = String(raw)
        out.tone = 'error'
      }
      break
    }
    case 'bodyIr': {
      const b = toBool(raw)
      if (b === null) {
        out.value = String(raw)
        out.tone = 'normal'
      } else if (b) {
        out.value = '有人'
        out.tone = 'warning'
      } else {
        out.value = '无人'
        out.tone = 'normal'
      }
      break
    }
    case 'sound': {
      const b = toBool(raw)
      if (b === null) {
        out.value = String(raw)
        out.tone = 'normal'
      } else if (b) {
        // 用户要求：有声音 = 开，无声音 = 关
        out.value = '开'
        out.tone = 'warning'
      } else {
        out.value = '关'
        out.tone = 'muted'
      }
      break
    }
    case 'onoff': {
      const b = toBool(raw)
      if (b === null) {
        out.value = String(raw)
        out.tone = 'normal'
      } else if (b) {
        out.value = '已开启'
        out.tone = 'success'
      } else {
        out.value = '已关闭'
        out.tone = 'muted'
      }
      break
    }
    case 'ai': {
      const n = toNum(raw)
      if (n === null) {
        out.value = String(raw)
        out.tone = 'normal'
      } else if (isScoreInvalid(n)) {
        // 255 这种哨兵值不显示成「满分」，否则一屏绿看着像一切正常
        out.value = String(raw)
        out.tone = 'muted'
      } else {
        out.value = fixed(n, 0)
        out.tone = scoreTone(n)
      }
      break
    }
    default: {
      out.value = String(raw)
      out.tone = 'normal'
    }
  }
  return out
}

/** 首页 6 项核心指标 */
function buildCoreMetrics(props) {
  return CORE_FIELDS.map(function (f) {
    return formatField(f, props)
  })
}

/** 详情页折叠区 4 项 */
function buildExtraMetrics(props) {
  return EXTRA_FIELDS.map(function (f) {
    return formatField(f, props)
  })
}

/** AI 评分的整体风险描述（减分制：越低越差） */
function aiRisk(score) {
  const n = toNum(score)
  if (n === null) return { score: '--', text: '暂无评分', tone: 'muted', tagClass: 'tag-muted' }
  if (isScoreInvalid(n)) {
    return { score: String(Math.round(n)), text: '读数无效', tone: 'muted', tagClass: 'tag-muted' }
  }
  let text = '风险较低'
  let tone = 'success'
  let tagClass = 'tag-success'
  if (n < THRESHOLD.AI_SCORE_L1) {
    text = '风险高'
    tone = 'error'
    tagClass = 'tag-error'
  } else if (n < THRESHOLD.AI_SCORE_WARN) {
    text = '风险中等'
    tone = 'warning'
    tagClass = 'tag-warning'
  }
  return { score: fixed(n, 0), text: text, tone: tone, tagClass: tagClass }
}

/**
 * 一条读数是否越过 L1 异常线。
 *
 * 四个指标各自独立判断，返回布尔对象，首页/详情页按它给读数标红
 * （趋势页「异常汇总」走 utils/l1.js，那边也是读这几个阈值，不另写数字）。
 * AI 评分那边要排除 255 这类哨兵值 —— 否则无效读数会被当成「远低于 20」，
 * 硬生生造出一段不存在的异常时间。
 */
function l1Hits(props) {
  const p = props || {}
  const t = toNum(p.SHT30_T)
  const h = toNum(p.SHT30_H)
  const l = toNum(p.Light)
  const a = toNum(p.AI_Score)
  return {
    temp: t !== null && t >= THRESHOLD.TEMP_WARNING,
    humi: h !== null && h >= THRESHOLD.HUMI_WARNING,
    light: l !== null && l >= THRESHOLD.LIGHT_HIGH,
    ai: a !== null && !isScoreInvalid(a) && a < THRESHOLD.AI_SCORE_L1,
  }
}

const LEVEL_TEXT = { severe: '严重', warning: '警告', info: '提示' }

/** 时间格式化：ts 为毫秒数字 */
function formatTime(ts) {
  if (!ts) return '--'
  const d = new Date(ts)
  const diff = Date.now() - ts
  if (diff < 0) return '刚刚'
  if (diff < 60 * 1000) return '刚刚'
  if (diff < 60 * 60 * 1000) return Math.floor(diff / 60000) + ' 分钟前'
  if (diff < 24 * 60 * 60 * 1000) return Math.floor(diff / 3600000) + ' 小时前'

  const p = function (n) {
    return n < 10 ? '0' + n : '' + n
  }
  return p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes())
}

/** 报警条目的展示化 */
function decorateAlarm(a) {
  return {
    _id: a._id,
    deviceId: a.deviceId,
    deviceName: a.deviceName || a.deviceId,
    level: a.level,
    levelText: LEVEL_TEXT[a.level] || a.level,
    title: a.title,
    detail: a.detail,
    read: !!a.read,
    timeText: formatTime(a.ts),
    ts: a.ts,
  }
}

/** 设备条目的展示化（首页 / 抽屉列表共用） */
function decorateDevice(d) {
  const props = d.props || {}
  const level = d.alarmLevel || null
  const alarmClass =
    level === 'severe' ? 'tag-error' : level === 'warning' ? 'tag-warning' : level === 'info' ? 'tag-muted' : ''
  return {
    deviceId: d.deviceId,
    name: d.name || d.deviceId,
    online: !!d.online,
    statusText: d.online ? '在线' : '离线',
    statusClass: d.online ? 'tag-success' : 'tag-muted',
    // 原始属性：首页自定义模块需要按字段取值渲染
    props: props,
    metrics: buildCoreMetrics(props),
    ai: aiRisk(props.AI_Score),
    // 优先用「设备最后一次上报」的时间：设备离线时 updatedAt（同步时间）仍在变，
    // 拿它显示会出现「离线 · 刚刚更新」这种误导文案。
    updatedText: formatTime(d.reportedAt || d.updatedAt),
    updatedAt: d.updatedAt,
    reportedAt: d.reportedAt || null,
    alarmLevel: level,
    alarmText: level ? LEVEL_TEXT[level] : '',
    alarmClass: alarmClass,
    alarmTitles: d.alarmTitles || [],
    fill: toBool(props.Fill) === true,
    fan: toBool(props.Fan) === true,
    source: d.source || 'iotda',
  }
}

module.exports = {
  FIELD_META: FIELD_META,
  CORE_FIELDS: CORE_FIELDS,
  EXTRA_FIELDS: EXTRA_FIELDS,
  TREND_FIELDS: TREND_FIELDS,
  THRESHOLD: THRESHOLD,
  LEVEL_TEXT: LEVEL_TEXT,
  toNum: toNum,
  toBool: toBool,
  isGasSafe: isGasSafe,
  scoreTone: scoreTone,
  isScoreInvalid: isScoreInvalid,
  l1Hits: l1Hits,
  formatField: formatField,
  buildCoreMetrics: buildCoreMetrics,
  buildExtraMetrics: buildExtraMetrics,
  aiRisk: aiRisk,
  formatTime: formatTime,
  decorateAlarm: decorateAlarm,
  decorateDevice: decorateDevice,
}
