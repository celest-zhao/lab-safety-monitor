/**
 * 首页模块配置（自定义看板）
 *
 * 设计要点：
 * - 模块配置存在**手机本地**（wx.setStorageSync），不写云数据库、不调接口、不产生费用。
 * - 模块分两类：
 *   field   —— 单个属性（来自 13 个物模型属性里的 11 个，Fill/Fan 归到 control 里）
 *   control —— 补光 / 风扇开关（一个模块管两个开关）
 *   alarm   —— 报警摘要
 * - 用户可新建、删除、恢复默认，顺序按添加顺序排列。
 */
const model = require('./model')

const STORAGE_KEY = 'dash_modules_v1'

/** 可选的属性分组（Fill / Fan 不单列，统一在 control 模块里） */
const FIELD_GROUPS = [
  {
    title: '环境',
    fields: ['SHT30_T', 'SHT30_H', 'Light', 'Sound', 'Gas'],
  },
  {
    title: '安防',
    fields: ['Body_IR', 'AI_Score', 'Temp_Rise', 'Fuse_Level', 'Fuse_Score', 'Dominant'],
  },
]

/** 特殊模块 */
const SPECIAL_GROUPS = [
  {
    title: '控制与提醒',
    items: [
      { kind: 'control', label: '补光 / 风扇开关' },
      { kind: 'alarm', label: '报警摘要' },
    ],
  },
]

/**
 * 字段 → 模块色系（只影响浅色主题；深色主题统一玻璃卡，见 app.wxss）
 * 每个属性固定一个色，用户加同一个属性永远同一个颜色，不会跳。
 * 可用值需与 app.wxss 里的 .mod-c-* 一一对应。
 */
const FIELD_COLOR = {
  SHT30_T: 'amber',
  SHT30_H: 'cyan',
  Gas: 'green',
  Body_IR: 'purple',
  Light: 'yellow',
  Sound: 'pink',
  AI_Score: 'violet',
  Temp_Rise: 'orange',
  Fuse_Level: 'teal',
  Fuse_Score: 'blue',
  Dominant: 'sky',
}

const CONTROL_COLOR = 'blue'
const ALARM_COLOR = 'red'

/** 取模块色系；未知字段回退到 blue，保证 class 始终存在 */
function colorOf(module) {
  if (!module) return CONTROL_COLOR
  if (module.kind === 'control') return CONTROL_COLOR
  if (module.kind === 'alarm') return ALARM_COLOR
  return FIELD_COLOR[module.field] || CONTROL_COLOR
}

/** 默认模块：常用 6 项 + 控制 + 报警 */
const DEFAULT_MODULES = [
  { id: 'f_SHT30_T', kind: 'field', field: 'SHT30_T' },
  { id: 'f_SHT30_H', kind: 'field', field: 'SHT30_H' },
  { id: 'f_Gas', kind: 'field', field: 'Gas' },
  { id: 'f_Body_IR', kind: 'field', field: 'Body_IR' },
  { id: 'f_Light', kind: 'field', field: 'Light' },
  { id: 'f_Sound', kind: 'field', field: 'Sound' },
  { id: 's_control', kind: 'control' },
  { id: 's_alarm', kind: 'alarm' },
]

function isKnownField(f) {
  return !!model.FIELD_META[f]
}

/** 规范单条模块；非法返回 null */
function normalize(m) {
  if (!m || typeof m !== 'object') return null
  if (m.kind === 'field') {
    if (!isKnownField(m.field)) return null
    return { id: 'f_' + m.field, kind: 'field', field: m.field }
  }
  if (m.kind === 'control') return { id: 's_control', kind: 'control' }
  if (m.kind === 'alarm') return { id: 's_alarm', kind: 'alarm' }
  return null
}

/** 去重 + 过滤非法项 */
function normalizeList(list) {
  const out = []
  const seen = {}
  ;(list || []).forEach(function (m) {
    const n = normalize(m)
    if (!n || seen[n.id]) return
    seen[n.id] = true
    out.push(n)
  })
  return out
}

/** 读本地配置；没存过返回默认 */
function load() {
  try {
    const raw = wx.getStorageSync(STORAGE_KEY)
    if (!raw || !raw.length) return DEFAULT_MODULES.slice()
    const list = normalizeList(raw)
    // 存过但全非法（比如属性改名了），回退默认，避免首页空白
    return list.length ? list : DEFAULT_MODULES.slice()
  } catch (e) {
    return DEFAULT_MODULES.slice()
  }
}

function save(list) {
  try {
    wx.setStorageSync(STORAGE_KEY, normalizeList(list))
    return true
  } catch (e) {
    return false
  }
}

function reset() {
  try {
    wx.removeStorageSync(STORAGE_KEY)
  } catch (e) {
    /* 忽略 */
  }
  return DEFAULT_MODULES.slice()
}

/**
 * 选择器数据：可选属性 + 特殊模块，并标出哪些已经加过
 * @param {Array} modules 当前模块列表
 */
function catalog(modules) {
  const used = {}
  ;(modules || []).forEach(function (m) {
    used[m.id] = true
  })

  const groups = FIELD_GROUPS.map(function (g) {
    return {
      title: g.title,
      items: g.fields.map(function (f) {
        const meta = model.FIELD_META[f] || {}
        return {
          id: 'f_' + f,
          kind: 'field',
          field: f,
          label: meta.label || f,
          // 原来这里写的是属性原始 key（SHT30_T 这种），界面上像调试信息，去掉
          desc: '',
          added: !!used['f_' + f],
        }
      }),
    }
  })

  SPECIAL_GROUPS.forEach(function (g) {
    groups.push({
      title: g.title,
      items: g.items.map(function (it) {
        return {
          id: 's_' + it.kind,
          kind: it.kind,
          label: it.label,
          desc: '',
          added: !!used['s_' + it.kind],
        }
      }),
    })
  })

  return groups
}

module.exports = {
  STORAGE_KEY: STORAGE_KEY,
  DEFAULT_MODULES: DEFAULT_MODULES,
  FIELD_COLOR: FIELD_COLOR,
  colorOf: colorOf,
  load: load,
  save: save,
  reset: reset,
  catalog: catalog,
  normalizeList: normalizeList,
}
