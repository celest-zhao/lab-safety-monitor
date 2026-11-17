/**
 * 报警页（tab 2）
 *
 * 三级规则（严重/警告/提示；AI 评分口径 2026-11-12 改为减分制）：
 *   严重：Gas ≠ Safe，或 AI_Score < 20（100 才正常，越低越偏离）→ 列表 + 红点 + 订阅消息推送
 *   警告：SHT30_T ≥ 35℃，或 Body_IR = true → 仅列表
 *   提示：设备离线 > 5 分钟 → 仅列表
 *
 * 删除（2026-11-09 新增）：
 *   ① 单条：左滑露出「删除」（不弹确认框 —— 左滑 + 点击已经是两步，再弹一次太烦）；
 *   ② 批量：工具条「清空」，按当前筛选清（选「全部」就清全部），必须二次确认。
 *   报警是三人共享数据，删掉即真删，没有回收站。
 */
const api = require('../../utils/api')
const model = require('../../utils/model')
const auto = require('../../utils/auto')
const theme = require('../../utils/theme')

/** 左滑露出的删除区宽度（rpx）。和 index.wxss 里 .swipe-del / .swipe-del-hit 是同一个数 */
const DEL_RPX = 152

/** 把云函数的错误码翻成「知道该怎么办」的话 */
function actionError(err, fallback) {
  const code = (err && err.code) || ''
  // BAD_ACTION = 云端还是旧版本，没有这个 action。不说清楚的话，
  // 界面只显示「请求失败」，会以为前端坏了，其实是忘了重新部署 iotApi。
  if (code === 'BAD_ACTION') return '云端函数还是旧版，请重新部署 iotApi'
  return (err && err.message) || fallback
}

const FILTERS = [
  { key: 'all', label: '全部' },
  { key: 'severe', label: '严重' },
  { key: 'warning', label: '警告' },
  { key: 'info', label: '提示' },
]

Page({
  data: {
    theme: theme.resolve(),
    loading: true,
    error: '',
    filters: FILTERS,
    filter: 'all',
    all: [],
    list: [],
    unread: 0,
    counts: { all: 0, severe: 0, warning: 0, info: 0 },
  },

  onLoad: function () {
    // rpx → px：滑动位移用 px 计算（touch 事件给的是 px），
    // 删除区宽度写在 wxss 里是 rpx，两边必须换算一致，否则滑开后会差几个像素
    let w = 375
    try {
      const info = wx.getWindowInfo ? wx.getWindowInfo() : wx.getSystemInfoSync()
      w = info.windowWidth || 375
    } catch (e) {
      /* 取不到就按 375 算 */
    }
    this.__delW = Math.round((w / 750) * DEL_RPX)
    // 当前滑开的是哪一条（按 _id 记，不是下标 —— 列表刷新后下标会变）
    this.__openId = ''
  },

  onShow: function () {
    theme.apply(this)
    this.load(true)
    // 自动刷新：先让云函数强制同步一次（新报警是在同步时判定的），再读列表
    auto.start(this, function () {
      const self = this
      return api
        .overview({ force: true })
        .then(function () {
          return self.load(false)
        })
        .catch(function () {
          /* 同步失败不影响列表显示 */
        })
    })
  },

  onHide: function () {
    auto.stop(this)
  },

  onPullDownRefresh: function () {
    const self = this
    this.load(false).then(function () {
      wx.stopPullDownRefresh()
    })
  },

  load: function (showLoading) {
    const self = this
    if (this.__loading) return Promise.resolve()
    this.__loading = true
    if (showLoading) this.setData({ loading: true, error: '' })

    const app = getApp()
    const ready = (app && app.ready) || Promise.resolve(null)

    return ready
      .then(function () {
        return api.alarms(60)
      })
      .then(function (r) {
        const all = (r.alarms || []).map(model.decorateAlarm)
        self.__loading = false
        self.setData({
          loading: false,
          error: '',
          all: all,
          counts: self.recount(all),
          unread: r.unread || 0,
        })
        self.applyFilter()
        self.updateTabBadge()
      })
      .catch(function (e) {
        self.__loading = false
        self.setData({ loading: false, error: (e && e.message) || '加载失败' })
      })
  },

  /** 各级别条数（保证和 all 永远一致，别再各写一遍） */
  recount: function (all) {
    const counts = { all: all.length, severe: 0, warning: 0, info: 0 }
    all.forEach(function (a) {
      if (counts[a.level] !== undefined) counts[a.level] += 1
    })
    return counts
  },

  applyFilter: function () {
    const f = this.data.filter
    const all = this.data.all
    const picked = f === 'all'
      ? all
      : all.filter(function (a) {
          return a.level === f
        })
    // tx：这一行左滑了多少像素；open：是否吸附在「已滑开」；
    // drag：正在拖（临时关掉过渡动画，否则会慢半拍）。
    //
    // 这里必须按 _id 记住哪一行是打开的：自动刷新每 30 秒重建一次列表，
    // 如果无脑把 tx 归零，用户刚滑开就被弹回去，看起来就是「滑不动 / 卡住」。
    // 切换筛选时如果那一行不在当前筛选里，就顺势清掉 __openId。
    const delW = this.__delW || 76
    const openId = this.__openId
    const list = picked.map(function (a) {
      const open = !!openId && a._id === openId
      return Object.assign({ tx: open ? -delW : 0, open: open, drag: false }, a)
    })
    if (openId) {
      const stillThere = list.some(function (x) {
        return x.open
      })
      if (!stillThere) this.__openId = ''
    }
    this.setData({ list: list })
  },

  onFilterTap: function (e) {
    const key = e.currentTarget.dataset.key
    if (!key || key === this.data.filter) return
    this.setData({ filter: key }, () => this.applyFilter())
  },

  /** 点一条报警：标记为已读（左滑开着时，点一下只收起，绝不标已读） */
  onAlarmTap: function (e) {
    const self = this
    const id = e.currentTarget.dataset.id
    const idx = e.currentTarget.dataset.idx
    if (!id) return

    const item = this.data.list[idx]

    // 顺序很重要：先看这一行是不是滑开的。
    // 滑开状态下点卡片 = 用户想收起来，绝不能顺手标已读
    // （之前点「删除」穿透到卡片时，就是这里把整张卡变成已读、看着像删不掉还变暗）。
    if (item && item.open) {
      this.closeRow(idx)
      return
    }

    // 刚滑完手指抬起也会走 tap，别把它当成「点开这条」
    if (this.__swipedAt && Date.now() - this.__swipedAt < 300) return

    if (item && item.read) return

    api
      .alarmRead(id)
      .then(function () {
        // 必须生成新对象：直接改 data 里的对象再 setData，微信会认为没变化而不刷新
        const all = self.data.all.map(function (a) {
          return a._id === id ? Object.assign({}, a, { read: true }) : a
        })
        self.setData({ all: all, unread: Math.max(0, self.data.unread - 1) })
        self.applyFilter()
        self.updateTabBadge()
      })
      .catch(function (err) {
        wx.showToast({ title: (err && err.message) || '操作失败', icon: 'none' })
      })
  },

  onReadAll: function () {
    const self = this
    if (!this.data.unread) {
      wx.showToast({ title: '没有未读报警', icon: 'none' })
      return
    }
    api
      .alarmReadAll()
      .then(function () {
        const all = self.data.all.map(function (a) {
          return a.read ? a : Object.assign({}, a, { read: true })
        })
        self.setData({ all: all, unread: 0 })
        self.applyFilter()
        self.updateTabBadge()
        wx.showToast({ title: '已全部标记为已读', icon: 'none' })
      })
      .catch(function (err) {
        wx.showToast({ title: (err && err.message) || '操作失败', icon: 'none' })
      })
  },

  /* --------------------------- 左滑删除（单条） --------------------------- */

  /**
   * 手指按下：记住起点和这一行当前的位移。
   *
   * 为什么不用 catchtouchmove：页面本身还要竖向滚动（这页没锁滚、还带下拉刷新），
   * 一旦 catch 住 touchmove，这一行里的上下滚动就死了。所以这里只「听」不「拦」，
   * 靠 touchstart→move 的方向判定来区分：横向才当成滑动，竖向就完全不管。
   */
  onRowTouchStart: function (e) {
    const idx = Number(e.currentTarget.dataset.idx)
    const t = e.touches && e.touches[0]
    const item = this.data.list[idx]
    if (!t || !item) return
    this.__sw = {
      idx: idx,
      x: t.clientX,
      y: t.clientY,
      base: item.tx || 0,
      moved: false, // 这次触摸到底动没动（动过就不该再当成点击）
      decided: false, // 方向判断过没有
      horiz: false, // 判断结果：是不是横向
    }
  },

  onRowTouchMove: function (e) {
    const s = this.__sw
    if (!s) return
    const t = e.touches && e.touches[0]
    if (!t) return
    const delW = this.__delW || 76
    const dx = t.clientX - s.x
    const dy = t.clientY - s.y

    if (!s.decided) {
      // 阈值放到 5px：手指还没动就别急着判定，动了才认方向
      if (Math.abs(dx) < 5 && Math.abs(dy) < 5) return
      s.moved = true
      s.decided = true
      // 横向要明显压过纵向才认，否则上下滑列表会被误判成左滑
      s.horiz = Math.abs(dx) > Math.abs(dy) * 1.1
      if (!s.horiz) return // 竖着走：交还给页面滚动
      this.closeOtherRows(s.idx)
      this.__openId = ''
      this.setData({ ['list[' + s.idx + '].drag']: true })
    }
    if (!s.horiz) return

    let tx = s.base + dx
    if (tx > 0) tx = 0
    // 超过删除区宽度后加阻尼，让人知道「到底了」
    if (tx < -delW) tx = -delW + (tx + delW) * 0.3
    this.setData({ ['list[' + s.idx + '].tx']: tx })
  },

  onRowTouchEnd: function () {
    const s = this.__sw
    this.__sw = null
    if (!s || !s.moved) return // 没动过：这是一次纯点击，交给 tap
    // 动过的触摸，抬手后 300ms 内的 tap 一律忽略（防止滑动结束被当成点开这条）
    this.__swipedAt = Date.now()
    if (!s.horiz) return
    const delW = this.__delW || 76
    const item = this.data.list[s.idx]
    const tx = (item && item.tx) || 0
    const open = tx < -delW * 0.5 // 过半就吸附到打开，否则弹回
    const patch = {}
    patch['list[' + s.idx + '].drag'] = false
    patch['list[' + s.idx + '].tx'] = open ? -delW : 0
    patch['list[' + s.idx + '].open'] = open
    this.setData(patch)
    this.__openId = open && item ? item._id : ''
  },

  /** 收起一行 */
  closeRow: function (idx) {
    const item = this.data.list[idx]
    if (item && item._id === this.__openId) this.__openId = ''
    this.setData({
      ['list[' + idx + '].tx']: 0,
      ['list[' + idx + '].open']: false,
    })
  },

  /** 同时只允许滑开一行：开新的，先把旧的收回去 */
  closeOtherRows: function (keep) {
    const patch = {}
    this.data.list.forEach(function (item, i) {
      if (i !== keep && item.open) {
        patch['list[' + i + '].tx'] = 0
        patch['list[' + i + '].open'] = false
      }
    })
    if (Object.keys(patch).length) this.setData(patch)
  },

  /* ------------------------------ 删除 / 清空 ----------------------------- */

  /** 点左滑露出的「删除」。不再弹确认框：左滑 + 点已经两步，够了 */
  onDelete: function (e) {
    const self = this
    const id = e.currentTarget.dataset.id
    if (!id) return
    if (this.__deleting === id) return // 连点两下只发一次请求
    this.__deleting = id
    this.__swipedAt = Date.now() // 防止这次点击又穿透成「标为已读」

    const item = this.data.all.filter(function (a) {
      return a._id === id
    })[0]

    api
      .alarmRemove(id)
      .then(function () {
        const all = self.data.all.filter(function (a) {
          return a._id !== id
        })
        const unread = item && !item.read ? Math.max(0, self.data.unread - 1) : self.data.unread
        self.__deleting = ''
        if (self.__openId === id) self.__openId = ''
        self.setData({ all: all, counts: self.recount(all), unread: unread })
        self.applyFilter()
        self.updateTabBadge()
        wx.showToast({ title: '已删除', icon: 'none' })
      })
      .catch(function (err) {
        self.__deleting = ''
        wx.showToast({ title: actionError(err, '删除失败'), icon: 'none', duration: 2500 })
      })
  },

  /** 工具条「清空」：按当前筛选清（选「全部」就清全部），必须二次确认 */
  onClearTap: function () {
    const self = this
    const f = this.data.filter
    const n = f === 'all' ? this.data.counts.all : this.data.counts[f] || 0
    if (!n) {
      wx.showToast({ title: '没有可清空的报警', icon: 'none' })
      return
    }
    wx.showModal({
      title: f === 'all' ? '清空全部报警' : '清空该类报警',
      content: '将删除' + (f === 'all' ? '全部 ' : '当前筛选的 ') + n + ' 条报警，删除后无法恢复。',
      confirmText: '清空',
      confirmColor: '#d92d20',
      success: function (res) {
        if (!res.confirm) return
        api
          .alarmClear(f)
          .then(function (r) {
            const removed = (r && r.removed) || 0
            return self.load(false).then(function () {
              wx.showToast({ title: '已清空 ' + removed + ' 条', icon: 'none' })
            })
          })
          .catch(function (err) {
            wx.showToast({ title: actionError(err, '清空失败'), icon: 'none', duration: 2500 })
          })
      },
    })
  },

  /** 只有严重级才给 tab 加红点（和推送规则保持一致） */
  updateTabBadge: function () {
    const severeUnread = this.data.all.filter(function (a) {
      return a.level === 'severe' && !a.read
    }).length
    if (severeUnread > 0) {
      wx.showTabBarRedDot({ index: 2, fail: function () {} })
    } else {
      wx.hideTabBarRedDot({ index: 2, fail: function () {} })
    }
  },

  onRetry: function () {
    this.load(true)
  },
})
