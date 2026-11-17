/**
 * 桌宠对话的预设问题表 + 端上兜底答复（纯函数，不碰 wx）
 *
 * 为什么要有这一层：
 *   云函数没部署、断网、超时、没配 key，任何一种都会让「点一下胶囊」变成没反应。
 *   所以点下去立刻用内存里的数据拼一段，和 aiApi 的规则答复同一口径；
 *   云端回了就用云端那句覆盖掉。用户感知不到哪一层在答。
 *
 * 关于输入框：
 *   默认没有输入框 —— 个人主体拿不到「深度合成-AI 问答」类目。
 *   想自己用可以打开云端 FREE_CHAT（只对名单里的 openid 生效），端上按 health 返回的
 *   freeChat 决定显不显示；自由输入失败时的兜底就是本文件底部的 freeFallback。
 *
 * 修改提示：
 *   这四个 qid 必须和 cloudfunctions/aiApi/index.js 的 CHAT_PRESETS 一致，
 *   改一边不改另一边会变成「云端不认识这个问题」。
 */

const model = require('./model')

// 阈值口径和 aiApi 的 THRESHOLD 对齐：
// 32℃/10℃ 是「舒适区间」，页面上 35℃ 是告警红线，不是一回事，别混。
const TEMP_HIGH = 32
const TEMP_LOW = 10
const HUMI_HIGH = 75
const LIGHT_DARK = 60
const AI_L1 = 20 // 减分制：100 正常，低于 20 才算异常
const AI_MAX = 100 // 挡 255 这类哨兵值

const PRESETS = [
  { id: 'worst', label: '哪台最不对劲' },
  { id: 'temp', label: '温度正常吗' },
  { id: 'todo', label: '我该做什么' },
  { id: 'alarm', label: '报警要紧吗' },
]

const IDS = PRESETS.map(function (p) {
  return p.id
})

const WELCOME = '想了解什么？点下面几个问题，我按现在的数据回答。'
// 自由输入打开时的开场白：先引导打字，也保留胶囊（胶囊离线也能答，输入不行）
const WELCOME_FREE = '想问什么直接打字，或者点下面几个问题，我按现在的数据回答。'

function welcome(free) {
  return free ? WELCOME_FREE : WELCOME
}

/**
 * 自由输入失败时的那句话。
 * 失败码由 utils/api.js 从云端 result.code 带过来，这里按码给不同说法 ——
 * 用户分得清「开关没开」「太快了」「今天够了」「网没通」，比统一报错有用得多。
 */
const FREE_FALLBACKS = {
  FREE_CHAT_OFF: '打字还没开，先点下面的问题吧。',
  FORBIDDEN: '这台手机还没获得授权。',
  TOO_FAST: '问得有点快，缓一秒再发。',
  TOO_MANY: '一分钟问太多了，等我喘口气。',
  QUOTA: '今天问得有点多了，明天再来。',
  UNSAFE: '这句我不好回答，换个问法吧。',
  EMPTY: '先说点什么吧。',
  BAD_ACTION: '这个功能还没准备好，稍后再试。',
}

function freeFallback(err) {
  const code = (err && err.code) || ''
  if (FREE_FALLBACKS[code]) return FREE_FALLBACKS[code]
  // 断网 / 超时 / 函数没部署：不要假装回答，直接把用户引到离线也能答的胶囊上
  return '没连上，先点下面几个问题试试。'
}

/**
 * 失败原因的一行字，只进 console（气泡上只写「本地答复」）。
 *
 * 出问题时界面不该摆技术词，但排查又必须有线索，所以留在日志里：
 * 超时、没部署、名单没开在日志里能分清，用户看到的仍然是一句人话。
 *
 * 注意 utils/api.js 的 friendly() 已经把平台错误翻成中文长句，
 * 所以「超时」「没部署」「环境 ID」都要按中文关键词匹配，别只认错误码。
 */
function errTag(err) {
  const code = (err && err.code) || ''
  const m = String((err && (err.message || err.errMsg)) || '')
  const low = m.toLowerCase()
  if (code === 'FREE_CHAT_OFF') return '云端说没开自由输入'
  if (code === 'FORBIDDEN') return '账号不在云端名单'
  if (code === 'QUOTA') return '今天额度用完了'
  if (code === 'TOO_FAST') return '发得太快'
  if (code === 'TOO_MANY') return '一分钟内太多'
  if (code === 'UNSAFE') return '内容没过审'
  if (code === 'BAD_ACTION') return '云端是旧版'
  // ESOCKETTIMEDOUT 去掉空格后是 timedout，两种写法都要认
  if (m.indexOf('超时') >= 0 || low.indexOf('timeout') >= 0 || low.indexOf('timed out') >= 0 || low.indexOf('timedout') >= 0) {
    return '云函数超时'
  }
  if (m.indexOf('还没部署') >= 0 || low.indexOf('not deployed') >= 0) return '云函数没部署'
  if (m.indexOf('环境 ID') >= 0 || low.indexOf('invalid_env') >= 0) return '云环境 ID 不对'
  if (m.indexOf('调用失败') >= 0 || m.indexOf('网络') >= 0) return '网络不通'
  // 认不出来的错误就截一段原文，总比「未知原因」有用
  return m ? m.slice(0, 14) : '未知原因'
}

function isPreset(id) {
  return IDS.indexOf(id) >= 0
}

/** 设备名太长会把一段话撑爆 */
function shortName(d) {
  const raw = String((d && (d.name || d.deviceId)) || '设备')
  return raw.length > 6 ? raw.slice(0, 6) : raw
}

function devicesOf(input) {
  const ds = (input && input.devices) || []
  // 页面偶尔会传成 undefined / 对象，一律当空处理，别让对话崩在这儿
  return Array.isArray(ds) ? ds : []
}

/** 打「不对劲」分：燃气 > 离线 > 自评低 > 温度 > 湿度，和云端同一套权重 */
function scoreDevice(d) {
  const p = (d && d.props) || {}
  let s = 0
  const why = []

  if (model.isGasSafe(p.Gas) === false) {
    s += 100
    why.push('可燃气体读数不正常')
  }
  if (d && d.online === false) {
    s += 60
    why.push('已经离线')
  }
  const ai = model.toNum(p.AI_Score)
  if (ai !== null && ai >= 0 && ai <= AI_MAX && ai < AI_L1) {
    s += (AI_L1 - ai) * 4
    why.push('自评只有 ' + Math.round(ai) + ' 分')
  }
  const t = model.toNum(p.SHT30_T)
  if (t !== null && t >= TEMP_HIGH) {
    s += 20
    why.push('温度 ' + t.toFixed(1) + '℃ 偏高')
  }
  const h = model.toNum(p.SHT30_H)
  if (h !== null && h >= HUMI_HIGH) {
    s += 10
    why.push('湿度 ' + h.toFixed(1) + '% 偏高')
  }
  return { s: s, why: why }
}

function adviceByReason(why) {
  const w = String(why || '')
  if (w.indexOf('气体') >= 0) return '先开窗通风、检查气源，别开电器。'
  if (w.indexOf('离线') >= 0) return '先看供电和网络，回「我的」页重新同步一次。'
  if (w.indexOf('自评') >= 0) return '去详情页翻一下最近的报警。'
  if (w.indexOf('温度') >= 0) return '可以开风扇降一降。'
  if (w.indexOf('湿度') >= 0) return '通通风或开除湿。'
  return ''
}

function answerWorst(ds) {
  const ranked = ds
    .map(function (d) {
      const r = scoreDevice(d)
      return { d: d, s: r.s, why: r.why }
    })
    .sort(function (a, b) {
      return b.s - a.s
    })

  const top = ranked[0]
  if (!top || !top.why.length) {
    return '这几台都没什么明显问题，温湿度、空气、在线状态都正常。'
  }
  return shortName(top.d) + ' 最值得看一眼：' + top.why.join('，') + '。' + adviceByReason(top.why[0])
}

function answerTemp(ds) {
  const live = ds.filter(function (d) {
    return d.online !== false
  })
  const d = live[0] || ds[0]
  const p = (d && d.props) || {}
  const t = model.toNum(p.SHT30_T)
  const h = model.toNum(p.SHT30_H)
  const name = shortName(d)

  if (t === null) return name + '还没上报温度，等下一轮数据再看。'

  const high = t >= TEMP_HIGH
  const low = t <= TEMP_LOW
  let text = name + '现在 ' + t.toFixed(1) + '℃，' + (high ? '偏高' : low ? '偏低' : '正常')
  if (h !== null) text += '，湿度 ' + h.toFixed(1) + '%'
  text += '。'
  if (high) text += '可以打开风扇降一降。'
  else if (low) text += '注意保暖，也看看门窗是不是开着。'
  else if (h !== null && h >= HUMI_HIGH) text += '建议通通风。'
  else text += '不用动，保持这样就行。'
  return text
}

function answerTodo(ds, alarm) {
  const severe = (alarm && alarm.severe) || 0
  const warning = (alarm && alarm.warning) || 0
  const total = (alarm && alarm.total) || 0

  if (severe > 0) return '先去报警页把 ' + severe + ' 条严重报警处理掉，别留着。'

  // 逐台找第一件该做的事：燃气最急，其它按同样的权重顺序
  for (let i = 0; i < ds.length; i++) {
    const why = scoreDevice(ds[i]).why
    if (why.length) {
      const tip = adviceByReason(why[0])
      if (tip) return tip
    }
  }

  if (warning > 0) return '有几条警告没读，抽空去报警页看一眼就行。'
  if (total > 0) return '只剩提示级报警，不急，有空翻一下。'
  return '暂时不用做什么，保持现在这样就行。'
}

function answerAlarm(alarm) {
  const severe = (alarm && alarm.severe) || 0
  const warning = (alarm && alarm.warning) || 0
  const info = (alarm && alarm.info) || 0
  const total = (alarm && alarm.total) || severe + warning + info

  if (!total) return '现在没有未读报警，不用管。'

  let text = '有 ' + total + ' 条未读报警'
  if (severe) text += '，其中 ' + severe + ' 条严重'
  text += '。'
  if (severe) text += '先去报警页把严重的处理掉。'
  else if (warning) text += '都是警告和提示，抽空去看一眼就行。'
  else text += '都是提示级的，不急。'
  return text
}

/**
 * 端上兜底答复。
 * input 用组件里的 properties：{ devices, alarmSummary }
 */
function answer(id, input) {
  const ds = devicesOf(input)
  const alarm = (input && input.alarmSummary) || {}

  if (!ds.length) {
    return '我这边还没收到设备数据。下拉刷新一次，或者去「我的」页做一次连接自检。'
  }
  if (id === 'worst') return answerWorst(ds)
  if (id === 'temp') return answerTemp(ds)
  if (id === 'todo') return answerTodo(ds, alarm)
  if (id === 'alarm') return answerAlarm(alarm)
  return '这个问题我还没学会，先点上面几个吧。'
}

module.exports = {
  PRESETS: PRESETS,
  WELCOME: WELCOME,
  WELCOME_FREE: WELCOME_FREE,
  welcome: welcome,
  isPreset: isPreset,
  answer: answer,
  freeFallback: freeFallback,
  errTag: errTag,
  // 单测用
  _score: scoreDevice,
}
