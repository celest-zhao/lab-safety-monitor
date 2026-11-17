/**
 * 单击时的一段数据总结（纯函数，不碰 wx）
 *
 * 为什么不用现成的云端 aiApi：
 *   单击是最高频的动作，用户点完就等着看。走云函数要 0.5–2 秒，
 *   还会因为冷启动、超时、断网变成「说不出来」。所以默认走本地：
 *   内存里已经有实时数据，同步拼一段就出来，0 延迟、0 成本、离线可用。
 *   想要更细的解读，气泡底部有「让 AI 细说」按钮，那时才走云端。
 *
 * 写作原则（用户原话「全面不冗余」）：
 *   - 覆盖：设备在线情况 / 环境数值 / AI 评分 / 报警 / 该干什么
 *   - 不冗余：13 个属性不全念。气体正常就不提，温度不超阈值就不提，
 *     报警为 0 就一句「没有未读报警」带过。
 *   - 说人话：不出现「综上所述」「当前状态如下」这种。
 */

const model = require('./model')

/** 把离线设备名念出来，最多两个，多了就说「等 N 台」 */
function nameList(list, limit) {
  const max = limit || 2
  const names = list.map(function (d) {
    return d.name || '未命名设备'
  })
  if (names.length <= max) return names.join('、')
  return names.slice(0, max).join('、') + ' 等 ' + names.length + ' 台'
}

function brief(input) {
  const ds = (input && input.devices) || []
  const alarm = (input && input.alarmSummary) || {}

  if (!ds.length) {
    return '我这边还没收到设备数据。下拉刷新一次看看；还是空的，就去「我的」页点一下连接自检。'
  }

  const live = ds.filter(function (d) {
    return d.online !== false
  })
  const dead = ds.filter(function (d) {
    return d.online === false
  })
  const parts = []

  /* --- 1. 在线情况 --- */
  if (ds.length === 1 && live.length === 1) {
    parts.push('你的设备在线')
  } else if (!dead.length) {
    parts.push(ds.length + ' 台设备都在线')
  } else if (!live.length) {
    parts.push(ds.length + ' 台设备全部离线，我只能看到最后上报的数据')
  } else {
    parts.push(ds.length + ' 台里 ' + live.length + ' 台在线，' + nameList(dead) + '掉线了')
  }

  /* --- 2. 环境数值（挑关键的，不全念）--- */
  const d = live[0] || ds[0]
  const p = d.props || {}
  const t = model.toNum(p.SHT30_T)
  const h = model.toNum(p.SHT30_H)
  const bits = []
  if (t !== null) bits.push(t.toFixed(1) + '℃')
  if (h !== null) bits.push('湿度 ' + h.toFixed(1) + '%')
  if (bits.length) {
    const where = d.name ? d.name + '现在 ' : '现在 '
    let line = where + bits.join('、')
    // 离线设备的数值是旧的，必须点明，不能让人以为是实时
    if (d.online === false && d.updatedText && d.updatedText !== '--') {
      line += '（最后一次上报 ' + d.updatedText + '）'
    }
    parts.push(line)
  }

  /* --- 3. 异常项：只有真异常才占一句 --- */
  if (model.isGasSafe(p.Gas) === false) {
    parts.push('可燃气体读数不正常，这个要立刻看')
  } else if (t !== null && t >= model.THRESHOLD.TEMP_WARNING) {
    parts.push('温度偏高，留意一下通风')
  }

  /* --- 4. 评分 + 报警 --- */
  const ai = d.ai || {}
  const hasScore = ai.score && ai.score !== '--'
  const total = alarm.total || 0
  if (hasScore) {
    let line = 'AI 评分 ' + ai.score + (ai.text ? '，' + ai.text : '')
    if (!total) line += '，也没有未读报警'
    parts.push(line)
  }

  if (total > 0) {
    let line = '还有 ' + total + ' 条未读报警'
    const severe = alarm.severe || 0
    const warning = alarm.warning || 0
    if (severe > 0) line += '，其中 ' + severe + ' 条严重'
    else if (warning > 0) line += '，' + warning + ' 条是警告'
    parts.push(line)
  }

  /* --- 5. 该干什么（只在有事时给建议）--- */
  if ((alarm.severe || 0) > 0) {
    parts.push('先去报警页把那几条严重的处理掉')
  } else if ((alarm.warning || 0) > 0) {
    parts.push('抽空去报警页看一眼就行')
  } else if (total > 0) {
    // 只剩提示级的：别刚说完「还有 1 条报警」又来一句「整体平稳」，自相矛盾
    parts.push('都是提示级的，不急')
  } else if (!dead.length) {
    parts.push('整体还算平稳')
  } else {
    parts.push('掉线的那台可以到「我的」页重新同步一下')
  }

  return parts.join('。') + '。'
}

module.exports = {
  brief: brief,
}
