/**
 * 设备页（tab 0）：环境监测看板
 *
 * 2026-11-06 重构：
 * - 首页不再是固定 6 项，改成**可自定义模块**（新建 / 删除 / 恢复默认），配置存手机本地。
 * - 移除示例设备「客厅 / 卧室」：云函数已关闭 mock 降级，拉不到数据就显示失败原因。
 * - 视觉升级为深色高级风（样式在 app.wxss）。
 */
const api = require('../../utils/api')
const model = require('../../utils/model')
const auto = require('../../utils/auto')
const dash = require('../../utils/dashboard')
const theme = require('../../utils/theme')

const EMPTY_SUMMARY = { severe: 0, warning: 0, info: 0, total: 0 }


Page({
  data: {
    // 模块加载时就算好，首屏直接是正确主题，不会先闪一帧浅色
    theme: theme.resolve(),
    loading: true,
    error: '',
    syncError: '',
    devices: [],
    chips: [],
    idx: 0,
    device: null,
    // 渲染后的模块
    modules: [],
    // 原始模块定义（用于增删）
    modulesRaw: [],
    editMode: false,
    sheetOpen: false,
    catalog: [],
    alarmSummary: EMPTY_SUMMARY,
    hero: { label: '温度', value: '--', unit: '℃', tone: 'muted' },
    emptyDesc: '检查一下网络，或者到「我的」页做一次连接自检。',
    /**
     * 桌宠正在被拖：把 scroll-view 的 refresher 关掉（双保险，手指在小人上本来也拉不到）。
     * 页面本身已经 disableScroll，不存在页面级下拉刷新。
     */
    lusterDrag: false,
    // scroll-view 下拉刷新：refresher-enabled / refresher-triggered 两个开关
    refreshEnabled: true,
    refreshing: false,
  },

  onLoad: function () {
    this.data.modulesRaw = dash.load()
  },

  onShow: function () {
    theme.apply(this)
    this.load(true)
    auto.start(this, function () {
      return this.load(false, true)
    })
  },

  onHide: function () {
    auto.stop(this)
    // 拖到一半切走：把 refresher 恢复，否则回到本页下拉刷新就废了。
    // 这里只改一个 data 字段，不碰滚动位置 —— scroll-view 的 scrollTop 自己会留着
    if (this.data.lusterDrag) this.unlockPage()
  },

  /**
   * 下拉刷新（scroll-view 的 refresher）。
   *
   * 页面级 enablePullDownRefresh 已关：那套是原生 overscroll，运行时关不掉，
   * 拖桌宠时按不住。换成 refresher-enabled 之后，拖拽时把开关一关就真的拉不动。
   */
  onRefresh: function () {
    if (this.data.refreshing) return
    const self = this
    // 三个出口都复位：成功、失败、以及 load 同步抛错（getApp 拿不到之类）。
    // 漏一个就是刷新头一直转，只能杀进程。
    const done = function () {
      self.setData({ refreshing: false })
    }
    this.setData({ refreshing: true })
    try {
      this.load(false, true).then(done, done)
    } catch (e) {
      done()
    }
  },

  /**
   * 桌宠开始/结束拖拽。
   *
   * 拖小人时手指往下走，以前会被 iOS 当成页面下拉 → 刷新头弹出、数据重载；
   * 改成 scroll-view 结构后页面本身不可滚，这里再把 refresher 关掉，
   * 两条路都堵死。松手只恢复开关，不动滚动位置，所以不会出现「页面下来一下」。
   */
  onLusterDrag: function (e) {
    const on = !!(e.detail && e.detail.on)
    if (on) {
      if (this.data.lusterDrag) return
      this.setData({ lusterDrag: true, refreshEnabled: false })
      return
    }
    this.unlockPage()
  },

  /** 恢复下拉刷新开关（松手 / 触摸中断 / 切后台都会走到） */
  unlockPage: function () {
    if (!this.data.lusterDrag) return
    this.setData({ lusterDrag: false, refreshEnabled: true })
  },

  /**
   * 页面任何位置被碰一下 → 通知桌宠「有人在用」，把休眠倒计时往后推。
   * 只在桌宠自己身上监听是不够的：用户一直在滚列表，桌宠也会睡过去。
   * 每次 touchstart 都 selectComponent 有点浪费，所以缓存住；实例失效时再取一次。
   */
  onUserTouch: function () {
    let c = this.__luster
    if (!c || !c.poke) {
      c = this.selectComponent('#luster')
      this.__luster = c
    }
    if (c && c.poke) c.poke()
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
        return api.overview({ force: !!force })
      })
      .then(function (r) {
        const devices = (r.devices || []).map(model.decorateDevice)
        const chips = devices.map(function (d) {
          return { deviceId: d.deviceId, name: d.name, online: d.online }
        })

        // 保持当前选中的设备；找不到就回到第一台
        const curId = (self.data.device && self.data.device.deviceId) || ''
        let idx = 0
        for (let i = 0; i < devices.length; i++) {
          if (devices[i].deviceId === curId) {
            idx = i
            break
          }
        }

        const device = devices.length ? devices[idx] : null
        const summary = r.alarmSummary || EMPTY_SUMMARY

        self.__loading = false
        self.setData({
          loading: false,
          error: '',
          syncError: r.syncError || '',
          devices: devices,
          chips: chips,
          idx: idx,
          device: device,
          alarmSummary: summary,
        })
        self.renderModules()
        self.updateTabBadge(summary)
      })
      .catch(function (e) {
        self.__loading = false
        self.setData({
          loading: false,
          error: ((e && e.message) || '加载失败') + '（点击可重试）',
        })
      })
  },

  /** 把「模块定义 + 当前设备属性」合成为可渲染数据 */
  renderModules: function () {
    const device = this.data.device
    const raw = this.data.modulesRaw || []
    const summary = this.data.alarmSummary || EMPTY_SUMMARY
    const props = (device && device.props) || {}

    const modules = raw.map(function (m) {
      // color 决定浅色主题下的模块配色（深色主题统一玻璃卡，不受影响）
      const color = dash.colorOf(m)
      if (m.kind === 'field') {
        const f = model.formatField(m.field, props)
        return {
          id: m.id,
          kind: 'field',
          field: m.field,
          color: color,
          label: f.label,
          value: f.value,
          unit: f.unit,
          tone: f.tone,
          big: f.big,
          wide: false,
        }
      }
      if (m.kind === 'control') {
        return { id: m.id, kind: 'control', color: color, wide: true }
      }
      return {
        id: m.id,
        kind: 'alarm',
        color: color,
        wide: true,
        value: summary.total,
        tone: summary.severe > 0 ? 'error' : summary.total > 0 ? 'warning' : 'success',
        severe: summary.severe,
        warning: summary.warning,
        info: summary.info,
      }
    })

    // 顶部主数值固定用温度；温度模块被删也照样展示（这是环境监测的核心指标）
    const temp = model.formatField('SHT30_T', props)
    this.setData({
      modules: modules,
      hero: { label: '当前温度', value: temp.value, unit: temp.unit, tone: temp.tone },
    })
  },

  /* ----------------------------- 设备切换 ----------------------------- */

  switchDevice: function (e) {
    const idx = Number(e.currentTarget.dataset.idx)
    if (isNaN(idx) || idx === this.data.idx) return
    const device = this.data.devices[idx]
    if (!device) return
    this.setData({ idx: idx, device: device })
    this.renderModules()
  },

  /* ----------------------------- 模块管理 ----------------------------- */

  toggleEdit: function () {
    this.setData({ editMode: !this.data.editMode })
  },

  openSheet: function () {
    this.setData({
      sheetOpen: true,
      catalog: dash.catalog(this.data.modulesRaw),
    })
  },

  closeSheet: function () {
    this.setData({ sheetOpen: false })
    // 弹层开着时不动后面的看板（每次 setData 都会让首页重排，看起来像「屏晃一下」）
    // 关掉弹层时统一刷一次，用户看不到中间过程
    if (this.__dirty) {
      this.__dirty = false
      this.renderModules()
    }
  },

  /**
   * 弹层里点某一行：没加过就加，加过的再点一下就删（一行两态，用户不用去翻「编辑」）
   * 只改这一行的 added 标记，不重建整个 catalog——重建会让列表跳动
   */
  addModule: function (e) {
    const ds = e.currentTarget.dataset
    const id = ds.id
    const raw = this.data.modulesRaw || []

    for (let i = 0; i < raw.length; i++) {
      if (raw[i].id === id) {
        this.removeById(id, ds.gi, ds.ii)
        return
      }
    }

    let item = null
    if (ds.kind === 'field') item = { id: 'f_' + ds.field, kind: 'field', field: ds.field }
    else if (ds.kind === 'control') item = { id: 's_control', kind: 'control' }
    else if (ds.kind === 'alarm') item = { id: 's_alarm', kind: 'alarm' }
    if (!item) return

    const next = raw.concat([item])
    this.data.modulesRaw = next
    dash.save(next)
    this.__dirty = true
    this.setCatalogAdded(ds.gi, ds.ii, true)
  },

  /** 占位：控制胶囊上吃掉长按，避免误触「删除模块」 */
  noop: function () {},

  /** 编辑态里点 ✕ */
  removeModule: function (e) {
    this.removeById(e.currentTarget.dataset.id)
  },

  /** 长按卡片直接删（不必先进「编辑」） */
  onModuleLongPress: function (e) {
    const self = this
    const id = e.currentTarget.dataset.id
    const list = this.data.modules || []
    let label = '这个模块'
    for (let i = 0; i < list.length; i++) {
      if (list[i].id === id) label = list[i].label || '这个模块'
    }
    wx.showModal({
      title: '删除模块',
      content: '把「' + label + '」从看板移除？随时可以在「新建模块」里加回来。',
      confirmText: '删除',
      success: function (res) {
        if (res.confirm) self.removeById(id)
      },
    })
  },

  /**
   * 统一删除入口：三种操作（弹层再点一次 / 编辑态 ✕ / 长按卡片）都走这里
   * @param {string} id 模块 id
   * @param {number} [gi] 弹层里所在分组下标（有就顺手把那一行的标记恢复）
   * @param {number} [ii] 分组内下标
   */
  removeById: function (id, gi, ii) {
    const next = (this.data.modulesRaw || []).filter(function (m) {
      return m.id !== id
    })
    if (next.length === this.data.modulesRaw.length) return

    this.data.modulesRaw = next
    dash.save(next)

    if (typeof gi === 'number' && gi >= 0) {
      this.setCatalogAdded(gi, ii, false)
    } else {
      // 从编辑态 / 长按进来的：弹层没开，catalog 可能还是旧的，扫一遍同步标记
      this.syncCatalogFlag(id, false)
    }

    // 弹层开着时延后到关闭再刷看板，避免每次点都重排首页
    if (this.data.sheetOpen) this.__dirty = true
    else this.renderModules()

    wx.showToast({ title: '已移除', icon: 'none' })
  },

  /** 只更新弹层里某一行的「已添加」标记，改一个叶子节点，不动整棵树 */
  setCatalogAdded: function (gi, ii, added) {
    if (typeof gi !== 'number' || gi < 0) return
    const patch = {}
    patch['catalog[' + gi + '].items[' + ii + '].added'] = added
    this.setData(patch)
  },

  /** 按 id 找到弹层里的那一行，同步「已添加」标记 */
  syncCatalogFlag: function (id, added) {
    const catalog = this.data.catalog || []
    for (let g = 0; g < catalog.length; g++) {
      const items = catalog[g].items || []
      for (let i = 0; i < items.length; i++) {
        if (items[i].id === id) {
          this.setCatalogAdded(g, i, added)
          return
        }
      }
    }
  },

  resetModules: function () {
    const self = this
    wx.showModal({
      title: '恢复默认模块',
      content: '会恢复成「温度 / 湿度 / 气体 / 人体 / 光照 / 声音 + 控制 + 报警」。',
      confirmText: '恢复',
      success: function (res) {
        if (!res.confirm) return
        const list = dash.reset()
        self.data.modulesRaw = list
        self.__dirty = false
        self.setData({ catalog: dash.catalog(list) })
        self.renderModules()
        wx.showToast({ title: '已恢复默认', icon: 'none' })
      },
    })
  },

  /* ----------------------------- 控制 ----------------------------- */

  /** 首页直接控制补光 / 风扇（走云函数命令通道） */
  onToggleControl: function (e) {
    const self = this
    const field = e.currentTarget.dataset.field
    const value = e.currentTarget.dataset.value === true || e.currentTarget.dataset.value === 'true'
    const device = this.data.device
    if (!device) return

    wx.showLoading({ title: '下发中', mask: true })
    api
      .control(device.deviceId, field, value)
      .then(function () {
        wx.hideLoading()
        wx.showToast({ title: value ? '已开启，等待设备确认' : '已关闭，等待设备确认', icon: 'none' })
        // 设备实测 2~3 秒回报，稍后刷新一次拿到真实状态
        setTimeout(function () {
          self.load(false, true)
        }, 2200)
      })
      .catch(function (err) {
        wx.hideLoading()
        const res = err && err.result
        wx.showModal({
          title: '控制失败',
          content: (res && res.advice) || (err && err.message) || '下发失败',
          showCancel: false,
        })
      })
  },

  /* ----------------------------- 跳转 ----------------------------- */

  updateTabBadge: function (summary) {
    if (summary && summary.severe > 0) {
      wx.showTabBarRedDot({ index: 2, fail: function () {} })
    } else {
      wx.hideTabBarRedDot({ index: 2, fail: function () {} })
    }
  },

  goDetail: function () {
    const device = this.data.device
    if (!device) return
    wx.navigateTo({
      url: '/pages/detail/index?deviceId=' + encodeURIComponent(device.deviceId),
    })
  },

  goAlarm: function () {
    wx.switchTab({ url: '/pages/alarm/index' })
  },

  onRetry: function () {
    this.load(true, true)
  },

  onShareAppMessage: function () {
    return {
      title: '环境监测',
      path: '/pages/device/index',
    }
  },
})
