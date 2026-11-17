/**
 * 我的页（tab 3）
 *
 * 主要作用是把排障入口集中起来：
 *   - 看到自己的 openid（加白名单要用）
 *   - 连接自检（一次看清 AK/SK/项目ID/接入地址哪里没配对）
 *   - 立即同步
 *   - 外观模式切换（跟随系统 / 浅色 / 深色）
 */
const api = require('../../utils/api')
const theme = require('../../utils/theme')

const VERSION = '1.0.0'

/**
 * 云端判定的问答通道 → 一句人话。
 *
 * 这六个值来自 aiApi 的 describeChatAuth：
 *   list  配置名单（config.js / FREE_CHAT_OPENIDS）命中
 *   db    users 集合里这条 openid 有 allowed:true
 *   open  库里一条 allowed:true 都没有 —— 视为没配白名单，放行
 *   deny  库里已经有人被授权，这台手机不在其中
 *   error 查库失败（云端放行）
 *   no-open-id 请求里没有 openid（只有控制台测试会这样）
 */
function authText(how) {
  if (how === 'list') return '配置名单命中'
  if (how === 'db') return '数据库 users 已授权'
  if (how === 'open') return '库里还没配白名单，自动放行'
  if (how === 'deny') return '被名单挡住（库里有人被授权，这台不在其中）'
  if (how === 'error') return '查库失败，已放行'
  if (how === 'no-open-id') return '请求里没有 openid'
  // 旧版 aiApi 还没这个字段
  return how ? how : '旧版云函数没报这一项'
}

/**
 * 「为什么没有输入框」→ 一句人话 + 该改哪儿。
 *
 * freeChat=false 有四种来源，以前界面上只能看出「没开」，
 * 实际最常见的是 config.js 里那份 openids 写的不是这台手机的 openid ——
 * 那时聊天气泡照样有 AI 回答（走了「没配白名单就放行」那条），却始终没有输入框。
 */
function freeText(ai) {
  if (ai.freeChat) return '已开（这台手机可以打字）'
  if (ai.chatAuth === 'deny') return '被问答名单挡住，打字也用不了'
  const why = ai.freeChatWhy
  if (why === 'not-in-list') return '打字开关开着，但名单里没有本机标识'
  if (why === 'mode-off') return '云端 FREE_CHAT 是 off'
  if (why === 'no-open-id') return '请求里没带 openid（只有控制台测试会这样）'
  if (ai.freeChatMode === 'on') return '开关 on，但这台手机不在打字名单'
  return '没开'
}

/** 开关 / 名单各自是从哪儿读来的（env 会盖住 config.js，这是最容易白改一通的地方） */
function freeSrcText(src) {
  if (!src) return ''
  const m = src.mode === 'env' ? '环境变量' : src.mode === 'file' ? 'config.js' : '默认值'
  const i =
    src.ids === 'env'
      ? '环境变量 FREE_CHAT_OPENIDS'
      : src.ids === 'file'
      ? 'config.js 的 openids'
      : '空（不限制）'
  return '（开关来自' + m + '，名单来自' + i + '）'
}

/** 模式 → 展示文案 */
function modeLabel(mode, current) {
  if (mode === 'system') {
    return '跟随系统（当前' + (current === 'dark' ? '深色' : '浅色') + '）'
  }
  return mode === 'dark' ? '深色' : '浅色'
}

Page({
  data: {
    theme: theme.resolve(),
    themeModes: theme.MODES,
    themeMode: theme.getMode(),
    themeLabel: '',
    openid: '',
    configured: false,
    mock: false,
    authMode: 'open',
    unread: 0,
    version: VERSION,
    checking: false,
    syncing: false,
    healthText: '',
    healthAdvice: '',
  },

  onShow: function () {
    const t = theme.apply(this)
    this.setData({ themeMode: t.mode, themeLabel: modeLabel(t.mode, t.theme) })
    this.refresh()
  },

  /**
   * 切换外观模式。
   * 生效范围：所有已打开页面的根节点 class（→ CSS 变量）＋ 导航栏 / tabBar / 图标。
   * 选择存在本机，下次打开还是这个模式；「跟随系统」不写死，系统变了会跟着变。
   */
  onThemeMode: function (e) {
    const key = (e && e.currentTarget && e.currentTarget.dataset.key) || 'system'
    if (key === this.data.themeMode) return

    const t = theme.setMode(key)
    const app = getApp()
    if (app && app.globalData) app.globalData.themeMode = t.mode
    this.setData({ themeMode: t.mode, themeLabel: modeLabel(t.mode, t.theme) })

    wx.showToast({
      title: key === 'system' ? '跟随系统' : key === 'dark' ? '深色模式' : '浅色模式',
      icon: 'none',
      duration: 1200,
    })
  },

  refresh: function () {
    const self = this
    const app = getApp()
    const ready = (app && app.ready) || Promise.resolve(null)

    return ready
      .then(function () {
        const g = (getApp() && getApp().globalData) || {}
        self.setData({
          openid: g.openid || '',
          configured: !!g.configured,
          mock: !!g.mock,
          authMode: (g.auth && g.auth.mode) || 'open',
        })
        return api.alarms(1).catch(function () {
          return null
        })
      })
      .then(function (r) {
        if (r) self.setData({ unread: r.unread || 0 })
      })
      .catch(function () {})
  },

  copyOpenid: function () {
    if (!this.data.openid) {
      wx.showToast({ title: '还没拿到 openid', icon: 'none' })
      return
    }
    wx.setClipboardData({
      data: this.data.openid,
      success: function () {
        wx.showToast({ title: '已复制 openid', icon: 'none' })
      },
    })
  },

  /** 连接自检：不写库，只验证配置与 IoTDA 是否连通 */
  onHealth: function () {
    const self = this
    if (this.data.checking) return
    this.setData({ checking: true })
    wx.showLoading({ title: '自检中…', mask: true })

    // 华为云与大模型是两条独立链路，一条通不代表另一条通，所以各查各的。
    // aiApi 没部署 / 没权限时不能让整个自检挂掉，所以单独 catch 成 null。
    return Promise.all([api.health(), api.aiHealth().catch(function () { return null })])
      .then(function (all) {
        const r = all[0]
        const ai = all[1]
        wx.hideLoading()
        const lines = []
        lines.push('配置是否完整：' + (r.configured ? '是' : '否'))
        lines.push('接入地址：' + (r.host || '未填'))
        lines.push('服务 ID：' + (r.serviceId || '未填'))
        lines.push('项目 ID：' + (r.projectIdSet ? '已填' : '未填'))
        lines.push('AK：' + (r.akTail ? '已填 ' + r.akTail : '未填'))
        if (r.iotda && r.iotda.ok) {
          lines.push('IoTDA 连通：正常，设备 ' + r.iotda.deviceCount + ' 台')
        } else {
          lines.push('IoTDA 连通：失败')
          if (r.iotda && r.iotda.reason) lines.push('原因：' + r.iotda.reason)
        }
        // 大模型这一段是给「配了 key 还是走本地答复」准备的：
        // llm=ready 只说明 key 被解析出来了，能不能真调用要看回答气泡上的小字
        if (!ai) {
          lines.push('大模型：aiApi 没响应（没部署或缺权限）')
        } else if (ai.llm !== 'ready') {
          lines.push('大模型：没读到 key，只走云端规则')
        } else {
          lines.push('大模型：已配 key · ' + (ai.provider || '?') + ' / ' + (ai.model || '?'))
          lines.push('模型地址：' + (ai.host || '未填'))
          lines.push('思考模式：' + (ai.thinking ? '开（慢）' : '关'))
          // chatAuth 是云端算出来的「这台手机凭什么能问」，比只报 freeChat 有用得多：
          // 以前 deny 和「开关没开」在界面上是同一句话，得翻代码才知道差在哪
          lines.push('问答通道：' + authText(ai.chatAuth))
          lines.push('自由输入：' + freeText(ai) + freeSrcText(ai.freeChatSrc))
          if (ai.chatAuth === 'deny') {
            lines.push('→ 数据库 users 里把本机标识那条的 allowed 改成 true')
          } else if (ai.freeChatWhy === 'not-in-list') {
            // 名单来自环境变量时，改 config.js 是白改 —— 环境变量优先级更高
            if (ai.freeChatSrc && ai.freeChatSrc.ids === 'env') {
              lines.push('→ 名单来自控制台环境变量 FREE_CHAT_OPENIDS，删掉它（或改成 *）')
            } else {
              lines.push('→ 把 config.js 的 freeChat.openids 换成上面的本机标识（或清空数组）')
            }
          }
        }
        const text = lines.join('\n')
        // 结果内联显示在页面上（弹窗会截断长文本，修复建议往往很长）
        self.setData({
          checking: false,
          healthText: text,
          healthAdvice: (r.iotda && r.iotda.advice) || '',
        })
        wx.showToast({
          title: r.iotda && r.iotda.ok ? '连接正常' : '连接失败',
          icon: r.iotda && r.iotda.ok ? 'success' : 'none',
        })
      })
      .catch(function (e) {
        wx.hideLoading()
        self.setData({ checking: false })
        wx.showModal({
          title: '自检失败',
          content: (e && e.message) || '调用失败',
          showCancel: false,
        })
      })
  },

  /** 立即同步一次 */
  onSync: function () {
    const self = this
    if (this.data.syncing) return
    this.setData({ syncing: true })
    wx.showLoading({ title: '同步中…', mask: true })

    api
      .sync()
      .then(function (r) {
        wx.hideLoading()
        self.setData({ syncing: false })
        let msg = '设备 ' + r.deviceCount + ' 台，耗时 ' + r.costMs + 'ms'
        const bad = (r.results || []).filter(function (x) {
          return !x.ok
        })
        if (bad.length) {
          msg += '\n失败 ' + bad.length + ' 台：\n' + bad.map(function (x) {
            return x.deviceId + ' → ' + x.error
          }).join('\n')
        }
        wx.showModal({ title: '同步完成', content: msg, showCancel: false })
        self.refresh()
      })
      .catch(function (e) {
        wx.hideLoading()
        self.setData({ syncing: false })
        wx.showModal({ title: '同步失败', content: (e && e.message) || '调用失败', showCancel: false })
      })
  },

  goAlarm: function () {
    wx.switchTab({ url: '/pages/alarm/index' })
  },

  onAbout: function () {
    wx.showModal({
      title: '关于',
      content:
        'IoT 设备监控 v' +
        VERSION +
        '\n\n报警规则：\n严重 — 可燃气体异常 / AI 评分低于 20\n警告 — 温度 ≥ 35℃ / 检测到人体\n提示 — 设备离线',
      showCancel: false,
    })
  },
})
