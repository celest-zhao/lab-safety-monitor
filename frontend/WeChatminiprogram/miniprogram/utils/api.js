/**
 * 云函数调用封装
 *
 * 所有数据都通过 wx.cloud.callFunction 走云函数，前端不直连数据库、不直连 IoTDA。
 * 这样 AK/SK 只存在服务端，小程序包里没有任何密钥。
 */

const FN_NAME = 'iotApi'

let loginPromise = null
let loginResult = null

/**
 * 把平台级错误码翻译成人能看懂、知道该怎么做的提示。
 * 不然界面上只会显示 -501000 这种东西。
 */
function friendly(message, fnName) {
  const name = fnName || FN_NAME
  const m = String(message || '')
  // -501000 FUNCTION_NOT_FOUND：云函数没部署，或函数名写错
  if (m.indexOf('-501000') >= 0 || m.indexOf('FunctionName parameter could not be found') >= 0) {
    return '云函数 ' + name + ' 还没部署。请在开发者工具里右键 cloudfunctions/' + name + ' → 创建并部署'
  }
  // -501005 / INVALID_ENV：EnvId 写错
  if (m.indexOf('INVALID_ENV') >= 0 || m.indexOf('-501005') >= 0) {
    return '云环境 ID 不对，请检查 app.js 里的 env 是否为完整 EnvId（不是环境名）'
  }
  // -504002 等超时
  if (m.indexOf('ESOCKETTIMEDOUT') >= 0 || m.indexOf('timed out') >= 0) {
    return '云函数响应超时（可能是首次冷启动），再试一次'
  }
  if (m.indexOf('-1') >= 0 && m.indexOf('cloud.callFunction:fail') >= 0) {
    return '云函数调用失败，请确认已部署且网络正常'
  }
  return m || '请求失败'
}

/**
 * 统一调用入口：把 result.ok === false 转成异常，页面只需处理 success / fail
 * name 不传就走默认的 iotApi，第二个云函数（aiApi）复用同一套错误翻译。
 */
function callFn(name, action, data) {
  return wx.cloud
    .callFunction({
      name: name,
      data: Object.assign({ action: action }, data || {}),
    })
    .then(function (res) {
      const r = (res && res.result) || {}
      if (r.ok === false) {
        const err = new Error(r.message || '请求失败')
        err.code = r.code || 'UNKNOWN'
        err.result = r
        throw err
      }
      return r
    })
    .catch(function (e) {
      // 平台级错误（函数不存在、环境不存在、超时）在这里统一改写文案
      const raw = (e && (e.message || e.errMsg)) || ''
      const err = new Error(friendly(raw, name))
      err.raw = raw
      err.code = (e && e.code) || 'PLATFORM'
      throw err
    })
}

/** 默认走 iotApi */
function call(action, data) {
  return callFn(FN_NAME, action, data)
}

/** 登录：缓存结果，多个页面同时调用也只发一次 */
function login(force) {
  if (force) loginPromise = null
  if (!loginPromise) {
    loginPromise = call('login')
      .then(function (r) {
        loginResult = r
        return r
      })
      .catch(function (e) {
        loginPromise = null
        throw e
      })
  }
  return loginPromise
}

function getLoginResult() {
  return loginResult
}

module.exports = {
  call: call,
  callFn: callFn,
  login: login,
  getLoginResult: getLoginResult,
  /** 数据小助手：把首页快照交给云端，拿回一段人话总结 */
  aiBrief: function (snapshot) {
    return callFn('aiApi', 'brief', { snapshot: snapshot })
  },
  /** 桌宠对话：预设 qid（worst / temp / todo / alarm），没有自由文本 */
  aiChat: function (qid, snapshot) {
    return callFn('aiApi', 'chat', { qid: qid, snapshot: snapshot })
  },
  /**
   * 自由输入。只有云端判定「这个 openid 开了 FREE_CHAT」才会成功，
   * 否则返回 FREE_CHAT_OFF —— 端上不会自己判断权限，开关只有一处。
   *
   * history 是最近几轮 `[{role:'user'|'assistant', content}]`。
   * 不带它的话「那湿度呢」这种追问必然答非所问（云端每轮只看眼前这一句），
   * 这正是最初「AI 有点笨」的主因。云端会重新清洗一遍，端上别指望它被信任。
   */
  aiAsk: function (text, snapshot, history) {
    const body = { text: text, snapshot: snapshot }
    if (history && history.length) body.history = history
    return callFn('aiApi', 'ask', body)
  },
  /** 能力探测：这个账号能不能打字（顺带看模型配没配） */
  aiHealth: function () {
    return callFn('aiApi', 'health', {})
  },
  overview: function (opts) {
    // opts.force = true：跳过云函数 30 秒节流，强制去拉一次 IoTDA（自动刷新用）
    return call('overview', opts || {})
  },
  detail: function (deviceId, opts) {
    return call('detail', Object.assign({ deviceId: deviceId }, opts || {}))
  },
  trend: function (deviceId, field, limit) {
    return call('trend', { deviceId: deviceId, field: field, limit: limit })
  },
  alarms: function (limit) {
    return call('alarms', { limit: limit })
  },
  alarmRead: function (id) {
    return call('alarmRead', { id: id })
  },
  alarmReadAll: function () {
    return call('alarmReadAll')
  },
  /** 删一条报警（报警页左滑露出的「删除」） */
  alarmRemove: function (id) {
    return call('alarmRemove', { id: id })
  },
  /** 清空报警：level 传 'all' 清全部，或传 severe/warning/info 只清该类 */
  alarmClear: function (level) {
    return call('alarmClear', { level: level || 'all' })
  },
  control: function (deviceId, field, value) {
    return call('control', { deviceId: deviceId, field: field, value: value })
  },
  sync: function () {
    return call('sync')
  },
  health: function () {
    return call('health')
  },
}
