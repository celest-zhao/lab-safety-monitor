/**
 * 设备详情页：控制（补光 / 风扇）+ 全部指标 + 最近报警
 *
 * 控制采用「乐观更新 + 失败回退」：
 *   点开关先改 UI，云函数返回失败则恢复原状态并提示，
 *   成功后再拉一次真实数据做校正（设备侧可能有自动关闭逻辑）。
 */
const api = require('../../utils/api')
const model = require('../../utils/model')
const auto = require('../../utils/auto')
const theme = require('../../utils/theme')

/**
 * 原生 switch 组件的 color 属性不支持 CSS 变量，只能传具体色值。
 * 取当前生效主题（不是系统主题）—— 手动选了深色时也要跟着变。
 */
function readSwitchColor() {
  try {
    return theme.resolve() === 'dark' ? '#6AA6FF' : '#4C6FFF'
  } catch (e) {
    return '#4C6FFF'
  }
}

Page({
  data: {
    theme: theme.resolve(),
    deviceId: '',
    loading: true,
    error: '',
    device: null,
    coreMetrics: [],
    extraMetrics: [],
    extraOpen: false,
    alarms: [],
    // 控制项 UI 状态
    fillOn: false,
    fanOn: false,
    fillPending: false,
    fanPending: false,
    online: false,
    updatedText: '',
    source: 'iotda',
    switchColor: '#4C6FFF',
  },

  onLoad: function (query) {
    const deviceId = decodeURIComponent((query && query.deviceId) || '')
    if (!deviceId) {
      this.setData({ loading: false, error: '缺少设备 ID' })
      return
    }
    this.setData({ deviceId: deviceId, switchColor: readSwitchColor() })
    this.load(true)
  },

  onShow: function () {
    theme.apply(this)
    this.setData({ switchColor: readSwitchColor() })
    // onLoad 里已经加载过一次，首次 onShow 不重复请求；
    // 从后台/其他页返回时补一次，并开启自动刷新（详情页此前缺 onShow，会一直显示旧值）
    if (this.__shown) this.load(false, true)
    this.__shown = true
    auto.start(this, function () {
      return this.load(false, true)
    })
  },

  onHide: function () {
    auto.stop(this)
  },

  onUnload: function () {
    auto.stop(this)
  },

  onPullDownRefresh: function () {
    this.load(false).then(function () {
      wx.stopPullDownRefresh()
    })
  },

  load: function (showLoading, force) {
    const self = this
    if (this.__loading) return Promise.resolve()
    this.__loading = true
    if (showLoading) this.setData({ loading: true, error: '' })

    const app = getApp()
    const ready = (app && app.ready) || Promise.resolve(null)

    return ready
      .then(function () {
        return api.detail(self.data.deviceId, { force: !!force })
      })
      .then(function (r) {
        const d = r.device || {}
        const props = d.props || {}
        const filled = model.buildCoreMetrics(props)
        const extras = model.buildExtraMetrics(props)

        // 控制项：用设备上报值初始化（正在下发的项不覆盖，避免闪烁）
        const patch = {
          loading: false,
          error: '',
          device: model.decorateDevice(d),
          coreMetrics: filled,
          extraMetrics: extras,
          alarms: (r.alarms || []).map(model.decorateAlarm),
          online: !!d.online,
          // 设备最后一次上报时间（设备离线时 updatedAt 仍在刷新，不能代表数据新鲜度）
          updatedText: model.formatTime(
            d.reportedAt || (d.updatedAt ? new Date(d.updatedAt).getTime() : null)
          ),
          source: d.source || 'iotda',
        }
        if (!self.data.fillPending) patch.fillOn = model.toBool(props.Fill) === true
        if (!self.data.fanPending) patch.fanOn = model.toBool(props.Fan) === true

        self.setData(patch)

        // 导航栏标题固定走 index.json 的「设备详情」，不跟设备名走。
        // 以前这里有一句 wx.setNavigationBarTitle({ title: d.name })：
        // 从首页点卡片进来时，顶部那行会突然从「设备监控」变成「示例设备 · 客厅」，
        // 看起来像页面名被数据覆盖了（用户 2026-11-09 反馈）。设备名在卡片里已经有了，
        // 导航栏再写一遍反而让人分不清自己在哪一页。
        self.__loading = false
      })
      .catch(function (e) {
        self.__loading = false
        self.setData({
          loading: false,
          error: (e && e.message) || '加载失败',
        })
      })
  },

  /** 补光 / 风扇开关 */
  onToggle: function (e) {
    const self = this
    const field = e.currentTarget.dataset.field // Fill | Fan
    const value = !!e.detail.value
    if (!field) return

    const pendingKey = field === 'Fill' ? 'fillPending' : 'fanPending'
    const onKey = field === 'Fill' ? 'fillOn' : 'fanOn'
    const before = this.data[onKey]

    const patch = {}
    patch[onKey] = value
    patch[pendingKey] = true
    this.setData(patch)

    if (!this.data.online) {
      wx.showToast({ title: '设备离线，指令会在上线后生效', icon: 'none', duration: 2500 })
    }

    api
      .control(this.data.deviceId, field, value)
      .then(function (r) {
        // 本项目实测走 command（物模型定义了 Fill / Fan 命令），2~3 秒内设备回报
        const ch = r.channel === 'command' ? '命令' : r.channel === 'shadow' ? '属性设置' : '本地'
        wx.showToast({ title: '已下发（' + ch + '），等待设备确认…', icon: 'none', duration: 1800 })
        // 不立刻解除 pending：先回读设备真实状态，确认到了才落定，避免"刚点开又弹回"
        self.confirmControl(field, value, 0)
      })
      .catch(function (err) {
        const p2 = {}
        p2[pendingKey] = false
        p2[onKey] = before // 回退到点击前的状态
        self.setData(p2)

        let msg = (err && err.message) || '下发失败'
        const res = err && err.result
        // 云函数已把 013002 / 014108 等错误码翻译成"去控制台改什么"，优先展示
        if (res && res.advice) msg = res.advice
        else if (res && res.channels && res.channels.length > 1) {
          msg = '命令下发与属性设置都失败，请检查物模型里的 Fill / Fan 命令是否已定义（详见交接文档 §7）'
        }
        wx.showModal({
          title: '控制失败',
          content: msg,
          showCancel: false,
        })
      })
  },

  /**
   * 控制后确认：轮询回读 reported，直到与期望值一致（最多 4 次，约 7 秒）
   *
   * 为什么需要：命令下发成功 ≠ 设备已执行。立刻 reload 会按旧值把开关覆盖，
   * 用户看到的就是"点了又自己弹回"。这里在确认期间保持 pending，不会被覆盖。
   * 实测本设备命令生效约 2~3 秒，所以首次回读 1.8 秒、最多 4 轮。
   */
  confirmControl: function (field, expected, attempt) {
    const self = this
    const pendingKey = field === 'Fill' ? 'fillPending' : 'fanPending'
    const onKey = field === 'Fill' ? 'fillOn' : 'fanOn'

    setTimeout(function () {
      api
        .detail(self.data.deviceId, { force: true })
        .then(function (r) {
          const d = r.device || {}
          const cur = model.toBool((d.props || {})[field])
          const patch = {}
          const hasReport = !!d.reportedAt

          if (cur === expected && hasReport) {
            patch[pendingKey] = false
            patch[onKey] = cur
            patch.online = !!d.online
            patch.updatedText = model.formatTime(d.reportedAt)
            self.setData(patch)
            if (attempt > 0) wx.showToast({ title: '设备已确认', icon: 'none', duration: 1200 })
            return
          }

          if (attempt < 3) {
            self.confirmControl(field, expected, attempt + 1)
            return
          }

          // 超时：以设备真实状态为准，并明确提示"下发成功但设备没确认"
          patch[pendingKey] = false
          patch[onKey] = cur === null ? self.data[onKey] : cur
          patch.online = !!d.online
          self.setData(patch)
          wx.showToast({
            title: '命令已下发，但设备 7 秒内未回报：检查固件是否订阅了命令 topic',
            icon: 'none',
            duration: 3000,
          })
        })
        .catch(function () {
          const patch = {}
          patch[pendingKey] = false
          self.setData(patch)
        })
    }, 1800)
  },

  toggleExtra: function () {
    this.setData({ extraOpen: !this.data.extraOpen })
  },

  goAlarm: function () {
    wx.switchTab({ url: '/pages/alarm/index' })
  },

  onRetry: function () {
    this.load(true)
  },
})
