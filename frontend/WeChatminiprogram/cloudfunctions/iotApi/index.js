/**
 * iotApi —— 小程序唯一的数据网关云函数
 *
 * 为什么合并成一个函数：
 *   微信云开发每个函数都要单独部署一次，功能拆太散会让部署步骤变多、容易漏。
 *   这里用 { action } 分发，一个函数覆盖全部接口，部署一次即可。
 *
 * 支持的 action：
 *   login    登录：返回 openid、是否在授权名单、配置状态
 *   overview 首页：设备列表 + 最新数据 + 报警汇总
 *   detail   详情：单设备最新数据 + 最近报警
 *   trend    趋势：单设备某个数值属性的历史点
 *   alarms   报警列表
 *   alarmRead 把报警标记为已读
 *   control  控制：补光 / 风扇
 *   sync     强制从 IoTDA 拉一次数据
 *   health   自检：配置是否完整、能否连通 IoTDA
 */
const cloud = require('wx-server-sdk')
const config = require('./config')
const iotda = require('./iotda')
const mock = require('./mock')
const model = require('./model')

cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV })

const db = cloud.database()
const _ = db.command

const COLL = {
  LATEST: 'device_latest',
  HISTORY: 'device_history',
  ALARMS: 'alarms',
  USERS: 'users',
}

const LEVEL_ORDER = { info: 1, warning: 2, severe: 3 }

/* ---------------------------------- 工具 ---------------------------------- */

function ok(data) {
  return Object.assign({ ok: true, ts: Date.now() }, data || {})
}

function fail(code, message, extra) {
  return Object.assign({ ok: false, code: code, message: message, ts: Date.now() }, extra || {})
}

/** 错误信息脱敏：只保留前 300 字，避免把签名串之类回传给前端 */
function errText(e) {
  const s = String((e && e.message) || e || 'unknown')
  return s.length > 300 ? s.slice(0, 300) + '…' : s
}

/**
 * 解析设备上报时间（影子里的 event_time）。
 * IoTDA 返回形如 20261106T094018Z，也兼容 ISO 串和毫秒数字；解析不出来返回 null。
 * 为什么要单独解析：updatedAt 是"云函数同步时间"，设备离线时它照样在刷新，
 * 不能拿它代表"数据有多新"。
 */
function parseEventTime(v) {
  if (v === null || v === undefined || v === '') return null
  if (typeof v === 'number') return isFinite(v) ? v : null
  const s = String(v).trim()
  const m = /^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z$/.exec(s)
  if (m) {
    const t = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6])
    return isFinite(t) ? t : null
  }
  const t2 = Date.parse(s)
  return isFinite(t2) ? t2 : null
}

function isConfigured() {
  const i = config.iotda
  return !!(i.ak && i.sk && i.projectId && i.host)
}

function highestLevel(hits) {
  let lv = null
  for (let i = 0; i < hits.length; i++) {
    const cur = hits[i].level
    if (!lv || (LEVEL_ORDER[cur] || 0) > (LEVEL_ORDER[lv] || 0)) lv = cur
  }
  return lv
}

/* --------------------------------- 权限 ---------------------------------- */

async function checkAuth(openid) {
  const mode = (config.auth && config.auth.mode) || 'open'
  if (mode !== 'whitelist') return { allowed: true, mode: mode }

  const list = (config.auth && config.auth.whitelist) || []
  const allowed = list.indexOf(openid) >= 0
  if (allowed) return { allowed: true, mode: mode }

  // 兜底：也允许库里的 users 记录标记为 allowed 的 openid
  try {
    const r = await db.collection(COLL.USERS).where({ openid: openid, allowed: true }).limit(1).get()
    if (r.data && r.data.length) return { allowed: true, mode: mode }
  } catch (e) {
    /* 忽略：查不到就按不允许处理 */
  }
  return { allowed: false, mode: mode }
}

async function touchUser(openid) {
  if (!openid) return
  const now = new Date()
  try {
    const r = await db.collection(COLL.USERS).where({ openid: openid }).limit(1).get()
    if (r.data && r.data.length) {
      await db
        .collection(COLL.USERS)
        .doc(r.data[0]._id)
        .update({ data: { lastLoginAt: now, loginCount: _.inc(1) } })
    } else {
      await db.collection(COLL.USERS).add({
        data: {
          openid: openid,
          allowed: false,
          createdAt: now,
          lastLoginAt: now,
          loginCount: 1,
          remark: '',
        },
      })
    }
  } catch (e) {
    // users 集合写入失败不影响主流程
  }
}

/* -------------------------------- 数据同步 -------------------------------- */

/**
 * @param {boolean} [forceMock] 真实接口失败后强制走示例数据，让界面不至于一片空白
 */
async function pullDeviceList(forceMock) {
  if (forceMock || !isConfigured()) {
    return { source: 'mock', devices: mock.listDevices().devices }
  }
  const r = await iotda.listDevices(config.iotda)
  return { source: 'iotda', devices: (r && r.devices) || [] }
}

async function pullShadow(deviceId, source) {
  if (source === 'mock') return mock.getShadow(deviceId)
  return iotda.getShadow(config.iotda, deviceId)
}

/** 写入历史点（属性没变化就跳过，省数据库写入次数） */
async function appendHistory(deviceId, props, prevProps) {
  const now = new Date()
  const a = JSON.stringify(props || {})
  const b = JSON.stringify(prevProps || {})
  if (a === b) return false
  await db.collection(COLL.HISTORY).add({ data: { deviceId: deviceId, ts: now, props: props } })
  return true
}

/** 裁剪历史，避免无限增长。最多 10 分钟做一次 */
async function maybeTrimHistory(deviceId, lastTrimAt) {
  const keep = (config.sync && config.sync.historyKeep) || 500
  if (Date.now() - (lastTrimAt ? new Date(lastTrimAt).getTime() : 0) < 10 * 60 * 1000) {
    return lastTrimAt
  }
  try {
    const c = await db.collection(COLL.HISTORY).where({ deviceId: deviceId }).count()
    const overflow = c.total - keep
    if (overflow > 0) {
      const old = await db
        .collection(COLL.HISTORY)
        .where({ deviceId: deviceId })
        .orderBy('ts', 'asc')
        .limit(Math.min(overflow, 100))
        .field({ _id: true })
        .get()
      const ids = (old.data || []).map(function (d) {
        return d._id
      })
      if (ids.length) {
        await db
          .collection(COLL.HISTORY)
          .where({ _id: _.in(ids) })
          .remove()
      }
    }
  } catch (e) {
    // 裁剪失败不影响主流程
  }
  return new Date()
}

/** 拉一台设备并落库 */
async function syncOneDevice(device, source) {
  const deviceId = device.device_id
  const shadow = await pullShadow(deviceId, source)
  const parsed = iotda.parseShadow(shadow, config.iotda.serviceId)
  const norm = model.normalizeProps(parsed.props)
  const online = model.isOnline(device.status)

  // 读旧记录（拿 alarmState 做去重，拿 props 判断是否有变化）
  let prev = null
  try {
    const r = await db.collection(COLL.LATEST).doc(deviceId).get()
    prev = r.data || null
  } catch (e) {
    prev = null
  }

  const ruleRes = model.evaluateRules(norm, online, prev && prev.alarmState)

  const now = new Date()
  // IoTDA 设备列表里的显示名是 device_name；本地示例数据用 name。两个都兼容。
  const displayName =
    (config.iotda.deviceNames && config.iotda.deviceNames[deviceId]) ||
    device.device_name ||
    device.name ||
    deviceId

  const doc = {
    deviceId: deviceId,
    name: displayName,
    serviceId: config.iotda.serviceId,
    status: device.status || '',
    online: online,
    props: parsed.props,
    desired: parsed.desired,
    reportedVersion: parsed.reportedVersion,
    reportedTime: parsed.reportedTime,
    // 设备最后一次上报的时刻（ms）。设备离线时它不会变，而 updatedAt 会变。
    reportedAt: parseEventTime(parsed.reportedTime),
    source: source,
    alarmState: ruleRes.state,
    alarmLevel: highestLevel(ruleRes.hits),
    alarmTitles: ruleRes.hits.map(function (h) {
      return h.title
    }),
    updatedAt: now,
  }
  await db.collection(COLL.LATEST).doc(deviceId).set({ data: doc })

  const changed = await appendHistory(deviceId, parsed.props, prev && prev.props)
  const lastTrimAt = await maybeTrimHistory(deviceId, prev && prev.lastTrimAt)
  if (lastTrimAt && lastTrimAt !== (prev && prev.lastTrimAt)) {
    try {
      await db.collection(COLL.LATEST).doc(deviceId).update({ data: { lastTrimAt: lastTrimAt } })
    } catch (e) {
      /* 忽略 */
    }
  }

  // 只对"新出现或等级升高"的规则落库，天然去重
  const newAlarms = []
  for (let i = 0; i < ruleRes.raised.length; i++) {
    const h = ruleRes.raised[i]
    const rec = {
      deviceId: deviceId,
      deviceName: displayName,
      code: h.code,
      level: h.level,
      title: h.title,
      detail: h.detail,
      ts: now,
      read: false,
      source: source,
    }
    await db.collection(COLL.ALARMS).add({ data: rec })
    newAlarms.push(rec)
  }

  return {
    ok: true,
    deviceId: deviceId,
    online: online,
    changed: changed,
    alarmLevel: doc.alarmLevel,
    newAlarms: newAlarms.length,
  }
}

async function doSync(forceMock) {
  const started = Date.now()
  const listed = await pullDeviceList(forceMock)
  let devices = listed.devices || []

  const allow = config.iotda.deviceIds || []
  if (allow.length) {
    devices = devices.filter(function (d) {
      return allow.indexOf(d.device_id) >= 0
    })
  }

  const results = []
  for (let i = 0; i < devices.length; i++) {
    try {
      results.push(await syncOneDevice(devices[i], listed.source))
    } catch (e) {
      results.push({ ok: false, deviceId: devices[i].device_id, error: errText(e) })
    }
  }

  // 清掉库里「这次没拉到」的设备。
  // 为什么要有这一步（2026-11-09 用户反馈）：
  //   syncOneDevice 只做 upsert，从不删除。调试期写进去的两台示例设备
  //   （mock-device-01 / 02，「示例设备 · 客厅 / 卧室」）在 mock 关掉之后没人删，
  //   于是一直挂在首页、点进去详情页标题也还是这个名字，看起来像数据卡住不动。
  // 只在**这次真的拉到设备**时清（devices.length > 0）：
  //   接口抖动返回空列表是很常见的，那种情况下清库会把真实设备一起抹掉。
  let removed = 0
  if (devices.length) {
    const keep = {}
    devices.forEach(function (d) {
      keep[d.device_id] = true
    })
    try {
      const r = await db.collection(COLL.LATEST).limit(100).field({ deviceId: true }).get()
      const staleIds = (r.data || [])
        .filter(function (d) {
          return d.deviceId && !keep[d.deviceId]
        })
        .map(function (d) {
          return d._id
        })
      if (staleIds.length) {
        await db
          .collection(COLL.LATEST)
          .where({ _id: _.in(staleIds) })
          .remove()
        removed = staleIds.length
      }
    } catch (e) {
      /* 清理失败不影响本次同步结果 */
    }
  }

  return {
    source: listed.source,
    deviceCount: devices.length,
    removed: removed,
    results: results,
    costMs: Date.now() - started,
  }
}

/** 库里数据是否过期（过期才拉 IoTDA，避免每次进页面都打接口） */
async function isStale() {
  const staleMs = ((config.sync && config.sync.staleSeconds) || 30) * 1000
  const r = await db.collection(COLL.LATEST).limit(100).field({ updatedAt: true }).get()
  const rows = r.data || []
  if (!rows.length) return true
  const now = Date.now()
  for (let i = 0; i < rows.length; i++) {
    const t = rows[i].updatedAt ? new Date(rows[i].updatedAt).getTime() : 0
    if (now - t > staleMs) return true
  }
  return false
}

/* --------------------------------- actions -------------------------------- */

async function actionLogin(openid) {
  const auth = await checkAuth(openid)
  await touchUser(openid)
  return ok({
    openid: openid,
    auth: auth,
    configured: isConfigured(),
    mock: !isConfigured() && !!(config.mock && config.mock.enabled),
    serviceId: config.iotda.serviceId,
  })
}

async function actionOverview(ev) {
  if (!isConfigured() && !(config.mock && config.mock.enabled)) {
    return fail('NO_CONFIG', '还没有填写华为云 AK/SK/项目ID，且示例数据已关闭', { configured: false })
  }

  // force=true：前端自动刷新时用，跳过 30 秒节流，保证每次刷新都拿到新数据
  const force = !!(ev && ev.force)

  let syncInfo = null
  let syncError = null
  if (force || (await isStale())) {
    try {
      syncInfo = await doSync()
    } catch (e) {
      syncError = errText(e)
      syncInfo = { error: syncError }
    }
  }

  const readDevices = async function () {
    const r = await db.collection(COLL.LATEST).limit(100).get()
    return (r.data || []).map(function (d) {
      return {
        deviceId: d.deviceId,
        name: d.name,
        online: !!d.online,
        status: d.status || '',
        props: d.props || {},
        desired: d.desired || {},
        alarmLevel: d.alarmLevel || null,
        alarmTitles: d.alarmTitles || [],
        updatedAt: d.updatedAt ? new Date(d.updatedAt).getTime() : null,
        // 设备上报时刻：老记录没有这个字段，用 reportedTime 兜底解析
        reportedAt:
          typeof d.reportedAt === 'number' ? d.reportedAt : parseEventTime(d.reportedTime),
        source: d.source || 'iotda',
      }
    })
  }

  let devices = await readDevices()

  // 真实接口拉不到数据（权限/网络/未开通）时，降级到示例数据让界面可用。
  // 但必须把失败原因一并返回，界面上要标清楚，不能让用户误以为是真实数据。
  let degraded = false
  if (!devices.length && syncError && config.mock && config.mock.enabled && config.mock.fallbackOnError) {
    try {
      syncInfo = await doSync(true)
      devices = await readDevices()
      degraded = true
    } catch (e) {
      /* 示例数据也失败就算了，界面显示错误态 */
    }
  }

  devices.sort(function (a, b) {
    if (a.online !== b.online) return a.online ? -1 : 1
    return String(a.name).localeCompare(String(b.name))
  })

  // 报警汇总（只统计未读）
  let summary = { severe: 0, warning: 0, info: 0, total: 0 }
  try {
    const ar = await db.collection(COLL.ALARMS).where({ read: false }).limit(100).field({ level: true }).get()
    const rows = ar.data || []
    rows.forEach(function (a) {
      if (summary[a.level] === undefined) return
      summary[a.level] += 1
      summary.total += 1
    })
  } catch (e) {
    /* 忽略 */
  }

  return ok({
    configured: isConfigured(),
    source: isConfigured() ? 'iotda' : 'mock',
    devices: devices,
    alarmSummary: summary,
    sync: syncInfo,
    // 拉取失败的原因（成功时为 null）
    syncError: syncError,
    // true = 接口失败、当前显示的是示例数据
    degraded: degraded,
  })
}

async function actionDetail(ev) {
  const deviceId = ev.deviceId
  if (!deviceId) return fail('BAD_ARG', '缺少 deviceId')
  try {
    if (ev.force || (await isStale())) await doSync()
  } catch (e) {
    /* 拉取失败时仍返回库里的旧数据 */
  }

  let device = null
  try {
    const r = await db.collection(COLL.LATEST).doc(deviceId).get()
    device = r.data
  } catch (e) {
    return fail('NOT_FOUND', '没有这台设备的数据，可能还没同步过')
  }

  const ar = await db
    .collection(COLL.ALARMS)
    .where({ deviceId: deviceId })
    .orderBy('ts', 'desc')
    .limit(20)
    .get()

  return ok({
    device: Object.assign({}, device, {
      updatedAt: device.updatedAt ? new Date(device.updatedAt).getTime() : null,
      reportedAt:
        typeof device.reportedAt === 'number' ? device.reportedAt : parseEventTime(device.reportedTime),
    }),
    alarms: (ar.data || []).map(function (a) {
      return Object.assign({}, a, { ts: a.ts ? new Date(a.ts).getTime() : null })
    }),
  })
}

async function actionTrend(ev) {
  const deviceId = ev.deviceId
  const field = ev.field
  if (!deviceId) return fail('BAD_ARG', '缺少 deviceId')
  if (!field) return fail('BAD_ARG', '缺少 field')
  const n = Math.min(Math.max(parseInt(ev.limit, 10) || 30, 5), 200)

  const r = await db
    .collection(COLL.HISTORY)
    .where({ deviceId: deviceId })
    .orderBy('ts', 'desc')
    .limit(n)
    .field({ ts: true, props: true })
    .get()

  const rows = (r.data || []).slice().reverse()
  const points = []
  rows.forEach(function (d) {
    const v = model.toNum((d.props || {})[field])
    if (v === null) return
    points.push({ ts: d.ts ? new Date(d.ts).getTime() : null, v: v })
  })

  return ok({ deviceId: deviceId, field: field, points: points })
}

async function actionAlarms(ev) {
  const n = Math.min(Math.max(parseInt(ev.limit, 10) || 50, 1), 100)
  const r = await db.collection(COLL.ALARMS).orderBy('ts', 'desc').limit(n).get()
  const rows = (r.data || []).map(function (a) {
    return Object.assign({}, a, { ts: a.ts ? new Date(a.ts).getTime() : null })
  })
  // 未读数必须单独统计，不能在分页结果里数（limit=1 时会永远算出 0 或 1）
  let unread = 0
  try {
    const c = await db.collection(COLL.ALARMS).where({ read: false }).count()
    unread = c.total
  } catch (e) {
    unread = 0
  }
  return ok({ alarms: rows, unread: unread })
}

async function actionAlarmRead(ev) {
  const id = ev.id
  if (!id) return fail('BAD_ARG', '缺少 id')
  await db.collection(COLL.ALARMS).doc(id).update({ data: { read: true, readAt: new Date() } })
  return ok({ id: id })
}

async function actionAlarmReadAll() {
  await db.collection(COLL.ALARMS).where({ read: false }).update({ data: { read: true, readAt: new Date() } })
  return ok({})
}

/**
 * 删一条报警（2026-11-09 新增：报警页左滑删除）
 *
 * 报警是共享数据：三个人看到的是同一批记录，谁删都是真删。
 * 这里不做软删除标记，理由是这个集合只用来「提醒」，删掉就是「不想再看见」，
 * 留着 status 反而会让列表查询多一层过滤、各处都要记得带上。
 */
async function actionAlarmRemove(ev) {
  const id = ev.id
  if (!id) return fail('BAD_ARG', '缺少 id')
  try {
    await db.collection(COLL.ALARMS).doc(id).remove()
  } catch (e) {
    // doc 不存在时 SDK 会抛错，对用户来说「已经没了」= 删除成功，不必报错
    return ok({ id: id, existed: false })
  }
  return ok({ id: id, existed: true })
}

/**
 * 清空报警（2026-11-09 新增）
 *
 * level 传具体级别就只清那一类（配合页面上的筛选），传 all / 不传就清全部。
 * 先 count 再 remove：删完就查不到了，前端要拿这个数字告诉用户「清了几条」。
 */
async function actionAlarmClear(ev) {
  const level = ev.level || 'all'
  const where = level === 'all' ? { _id: _.exists(true) } : { level: level }

  let count = 0
  try {
    const c = await db.collection(COLL.ALARMS).where(where).count()
    count = c.total
  } catch (e) {
    count = 0
  }
  if (!count) return ok({ removed: 0, level: level })

  await db.collection(COLL.ALARMS).where(where).remove()
  return ok({ removed: count, level: level })
}

/**
 * 控制补光 / 风扇
 *
 * 实测结论（2026-11-06，标准版实例 + 真实设备）：
 *   ① POST /commands，command_name=Fill，paras={Fill:true} → 2.5 秒内 reported.Fill 变 true，硬件动作 ✅
 *   ② PUT /shadow 写 desired.Fill              → 400 IOTDA.013002（属性虽标 RW，影子仍拒写）
 * 因此默认优先命令下发，影子作为回退。
 */

/** 把控制类报错翻译成「去控制台改什么」，前端直接展示 */
function controlAdvice(raw) {
  const t = String(raw || '')
  if (t.indexOf('IOTDA.013002') >= 0) {
    return (
      '写设备影子被拒（IOTDA.013002）。本产品应走「命令下发」：' +
      '确认 产品 → 物模型 → 命令 里有 Fill / Fan 两条命令、参数为 boolean，' +
      '并把 config.js 的 controlChannel 设为 command。'
    )
  }
  if (t.indexOf('IOTDA.014108') >= 0 || t.indexOf("command_name") >= 0) {
    return (
      '物模型里没有这条命令：到 产品 → 物模型 → 命令，新增与字段同名的命令（Fill / Fan），' +
      '参数类型 boolean；或在 config.js 的 commandNameByField 里改成你已定义的命令名。'
    )
  }
  if (t.indexOf('IOTDA.013005') >= 0 || t.indexOf('IOTDA.014016') >= 0) {
    return '设备不在线或命令未被接收，等设备上线后再试。'
  }
  if (t.indexOf('IOTDA.000022') >= 0) {
    return '当前 AK/SK 没有下发权限：到 IAM 给该用户授予 IoTDA FullAccess（只读权限不够）。'
  }
  return ''
}

/** 字段 → 物模型命令名 */
function controlCommandName(cfg, field) {
  const map = (cfg && cfg.commandNameByField) || {}
  return map[field] || field
}

/** 按物模型定义组装命令参数（本产品是 boolean） */
function controlParas(cfg, field, val) {
  const type = (cfg && cfg.commandParamType) || 'boolean'
  const paras = {}
  if (type === 'number') paras[field] = val ? 1 : 0
  else if (type === 'string') paras[field] = val ? 'open' : 'close'
  else paras[field] = !!val
  return paras
}

async function actionControl(ev) {
  const deviceId = ev.deviceId
  const field = ev.field
  if (!deviceId) return fail('BAD_ARG', '缺少 deviceId')
  if (model.CONTROL_FIELDS.indexOf(field) < 0) {
    return fail('BAD_FIELD', '只能控制 ' + model.CONTROL_FIELDS.join(' / '))
  }
  const val = !!ev.value
  const channels = []

  if (!isConfigured()) {
    // 示例模式：只改库里的期望值，让界面能演示
    try {
      const r = await db.collection(COLL.LATEST).doc(deviceId).get()
      const desired = Object.assign({}, (r.data && r.data.desired) || {}, {})
      desired[field] = val
      await db.collection(COLL.LATEST).doc(deviceId).update({ data: { desired: desired } })
      return ok({ channel: 'mock', field: field, value: val, channels: [{ channel: 'mock', ok: true }] })
    } catch (e) {
      return fail('CONTROL_FAILED', errText(e), { channels: [{ channel: 'mock', ok: false }] })
    }
  }

  const cfg = config.iotda
  const preferCommand = (cfg.controlChannel || 'command') !== 'shadow'
  const order = preferCommand ? ['command', 'shadow'] : ['shadow', 'command']
  let lastError = ''

  for (let i = 0; i < order.length; i++) {
    const which = order[i]
    if (which === 'command') {
      const cmdName = controlCommandName(cfg, field)
      try {
        const r = await iotda.createCommand(cfg, deviceId, cfg.serviceId, cmdName, controlParas(cfg, field, val))
        channels.push({ channel: 'command', ok: true, commandName: cmdName })
        return ok({
          channel: 'command',
          field: field,
          value: val,
          commandName: cmdName,
          commandId: (r && r.command_id) || null,
          channels: channels,
        })
      } catch (e) {
        lastError = errText(e)
        channels.push({ channel: 'command', ok: false, commandName: cmdName, error: lastError })
      }
    } else {
      const props = {}
      props[field] = val
      try {
        await iotda.updateShadowDesired(cfg, deviceId, cfg.serviceId, props)
        channels.push({ channel: 'shadow', ok: true })
        return ok({ channel: 'shadow', field: field, value: val, channels: channels })
      } catch (e) {
        lastError = errText(e)
        channels.push({ channel: 'shadow', ok: false, error: lastError })
      }
    }
  }

  const advice = controlAdvice(lastError)
  return fail('CONTROL_FAILED', advice || '两个通道都失败了', { channels: channels, advice: advice })
}

async function actionSync() {
  if (!isConfigured() && !(config.mock && config.mock.enabled)) {
    return fail('NO_CONFIG', '还没有填写华为云 AK/SK/项目ID')
  }
  const r = await doSync()
  return ok(r)
}

/** 自检：不落库，只验证配置和连通性 */
/** 把 IoTDA 的典型报错翻译成「该怎么修」 */
function advise(e) {
  const t = errText(e)
  if (t.indexOf('IOTDA.000021') >= 0) {
    return {
      reason: t,
      advice:
        '华为云返回「IAM Token 所在用户未订阅/无权限访问 IoTDA」。它不一定是子账号问题，常见有三种原因，请按顺序在控制台核对：' +
        '① 账号不一致：这组 AK/SK 所属账号，并不是拥有这个 IoTDA 实例的账号（很多人有不止一个华为云账号）。请在 IoTDA 控制台确认实例归属账号，并在「我的凭证」用同一个账号建 AK/SK。' +
        '② 区域不一致：实例实际所在区域，与 config.js 里的 host/projectId 不符（例如实例在 cn-north-1，却填了 cn-north-4）。请在 IoTDA 控制台「总览 → 接入信息」核对接入地址和项目 ID。' +
        '③ 权限不足：若确认是同一账号且同区域，再到「IAM → 用户组 → 授权」给该账号/子账号加 IoTDA FullAccess。' +
        '④ 标准版地址/签名不对：如果控制台能看到实例，确认实例是标准版后，host 必须换成实例专用「应用侧接入地址」，并开启 derivedAuth（V11 派生签名）；iotda.<region>.myhuaweicloud.com 只适用于基础版。' +
        '改完 AK/SK 或配置要重新部署 iotApi 云函数。',
      hint: 'IOTDA.000021',
    }
  }
  if (t.indexOf('IOTDA.000002') >= 0) {
    return {
      reason: t,
      advice:
        'IoTDA 签名认证失败。常见原因：① 标准版/企业版用了基础版通用地址，应改用实例专用「应用侧接入地址」；' +
        '② 标准版/企业版没有开启派生签名（config.js 需 derivedAuth: true、region 和 derivedAuthServiceName: "iotdm"）；' +
        '③ Instance-Id 没有参与签名（本项目会自动签名）。改完重新部署 iotApi。',
      hint: 'SIGNATURE_FAILED',
    }
  }
  if (t.indexOf('IOTDA.000022') >= 0) {
    return {
      reason: t,
      advice: '该用户没有操作权限。请到 IAM 给用户组授予 IoTDA FullAccess（只读权限不够，因为要下发控制指令）。',
      hint: 'IOTDA.000022',
    }
  }
  if (t.indexOf('APIGW.0101') >= 0 || t.indexOf('The API does not exist') >= 0) {
    return {
      reason: t,
      advice: '接口不存在。多半是 host 或区域填错了，请核对「设备接入 IoTDA 控制台 → 总览 → 接入信息」里的接入地址，以及对应的项目 ID。',
      hint: 'ENDPOINT_MISMATCH',
    }
  }
  if (t.indexOf('403') >= 0 || t.indexOf('401') >= 0) {
    return {
      reason: t,
      advice: '鉴权失败。请确认 AK/SK 正确、未被删除禁用，且 projectId 与 host 所在区域一致。',
      hint: 'AUTH_FAILED',
    }
  }
  if (t.indexOf('ENOTFOUND') >= 0 || t.indexOf('getaddrinfo') >= 0) {
    return {
      reason: t,
      advice:
        '域名解析失败，host 写错了。基础版形如 iotda.cn-north-4.myhuaweicloud.com；标准版/企业版必须用控制台「总览 → 平台接入地址 → 应用侧」的实例专用域名（不要带 https:// 和结尾斜杠）。',
      hint: 'BAD_HOST',
    }
  }
  if (t.indexOf('超时') >= 0 || t.indexOf('ESOCKETTIMEDOUT') >= 0) {
    return {
      reason: t,
      advice: '云函数访问 IoTDA 超时。可能是首次冷启动或网络抖动，再点一次自检。',
      hint: 'TIMEOUT',
    }
  }
  return { reason: t, advice: '把上面这段原始报错发给开发者排查。', hint: 'UNKNOWN' }
}

async function actionHealth() {
  const base = {
    configured: isConfigured(),
    mock: !isConfigured() && !!(config.mock && config.mock.enabled),
    serviceId: config.iotda.serviceId,
    host: config.iotda.host,
    region: config.iotda.region || '',
    projectIdSet: !!config.iotda.projectId,
    instanceIdSet: !!config.iotda.instanceId,
    derivedAuth: !!(config.iotda.derivedAuth || config.iotda.authType === 'derived'),
    akTail: config.iotda.ak ? '****' + config.iotda.ak.slice(-4) : '',
    authMode: (config.auth && config.auth.mode) || 'open',
  }
  if (!isConfigured()) {
    return ok(Object.assign(base, { iotda: { ok: false, reason: '配置未填写', hint: 'NO_CONFIG' } }))
  }
  try {
    const r = await iotda.listDevices(config.iotda)
    const devices = (r && r.devices) || []
    return ok(
      Object.assign(base, {
        iotda: { ok: true, deviceCount: devices.length },
        devices: devices.slice(0, 10).map(function (d) {
          return { device_id: d.device_id, name: d.device_name, status: d.status }
        }),
      })
    )
  } catch (e) {
    return ok(Object.assign(base, { iotda: Object.assign({ ok: false }, advise(e)) }))
  }
}

/* --------------------------------- 入口 ---------------------------------- */

const ACTIONS = {
  login: actionLogin,
  overview: actionOverview,
  detail: actionDetail,
  trend: actionTrend,
  alarms: actionAlarms,
  alarmRead: actionAlarmRead,
  alarmReadAll: actionAlarmReadAll,
  alarmRemove: actionAlarmRemove,
  alarmClear: actionAlarmClear,
  control: actionControl,
  sync: actionSync,
  health: actionHealth,
}

exports.main = async function (event, context) {
  const ev = event || {}
  const action = ev.action || 'overview'
  const wx = cloud.getWXContext()
  const openid = wx.OPENID || ''

  try {
    // login 必须在鉴权之前。
    //
    // 为什么（2026-11-09 晚6c）：切到 whitelist 之后，如果 login 也被 checkAuth 拦住，
    // 名单外的人连自己的 openid 都拿不到 ——「我的」页显示「获取中…」，点复制提示「还没拿到 openid」，
    // 于是他没法把 openid 发给管理员，管理员也没法加他。死锁。
    //
    // login 只回「你是谁 + 是否允许 + 配置状态」，不含任何设备数据，放开它不泄露业务数据。
    // actionLogin 内部自己算 auth，把 allowed:false 一起返回；touchUser 也在那里，
    // 所以名单外的人会在 users 集合留一条 allowed:false 的记录，管理员在控制台把 allowed 勾上即可，
    // 不需要改代码重新部署。
    if (action === 'login') return await actionLogin(openid)

    const auth = await checkAuth(openid)
    if (!auth.allowed) {
      return fail('FORBIDDEN', '你不在授权名单里', {
        openid: openid,
        hint: '把这段 openid 发给管理员加白名单',
      })
    }

    // 其余 action 统一把整个 event 传进去
    const fn = ACTIONS[action]
    if (!fn) return fail('BAD_ACTION', '未知 action: ' + action)

    return await fn(ev)
  } catch (e) {
    return fail('INTERNAL', errText(e))
  }
}
