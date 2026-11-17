/**
 * 趋势页（tab 1）：单设备单属性的历史折线图 + 统计
 *
 * 不用图表库（设计规范第 5 节：零依赖、不增加包体积）。
 * 折线是「一堆绝对定位的细长 view」拼出来的：每段是一根 height:2rpx 的圆角条，
 * 绕自己的左端点旋转到相邻两点的连线角度。
 *
 * 为什么角度可以写死成常数：rpx 是按屏宽等比缩放的（750rpx 恒等于屏宽），
 * 所以 plot 的宽高比在 iPhone SE 到 iPad 上都一致，算出来的线段长度与旋转角
 * 在真机上不会走形。这也是为什么 PLOT_W 用 rpx 常量而不是运行时去量。
 *
 * 横轴不再标时间（上一版 30 点就已经挤成一排「20:xx」）。点只会越攒越多，
 * 只要标就一定放不下；时间范围改在卡片标题右边显示一次，图里只看形状。
 */
const api = require('../../utils/api')
const model = require('../../utils/model')
const l1 = require('../../utils/l1')
const auto = require('../../utils/auto')
const theme = require('../../utils/theme')

const PLOT_W = 630 // 卡片内容宽 638（750 - 页 24×2 - 卡 32×2），左右各留 4
const PLOT_H = 320
const PAD_Y = 18 // 上下留白，避免折线顶到卡片边
const MAX_POINTS = 60 // 超过就抽稀，段数可控
const DOT_LIMIT = 30 // 点数 ≤ 这个才画圆点，多了就是一整条连续线
const LIMIT = 120 // 一次向云端取多少点
const L1_FIELDS = l1.FIELDS // 汇总要的四个字段，正好等于可画趋势的四个

function pad(n) {
  return n < 10 ? '0' + n : '' + n
}

/**
 * 区间文案用的时间格式。
 *
 *   15 分钟内且不跨小时 → HH:mm:ss。设备 5 秒一报，30 个点才 2.5 分钟，
 *        用 HH:mm 会显示成「20:14 — 20:15」看不出变化；用 mm:ss 又会显示成
 *        「14:30 — 16:55」，看着像跨了 2.5 小时。短跨度必须带上小时。
 *   ≥ 20 小时（可能跨天）→ MM-DD，否则不同天会显示成同一时刻
 *   其他 → HH:mm
 */
function makeFmt(t0, t1) {
  const a = new Date(t0)
  const b = new Date(t1)
  const spanMs = Math.abs(t1 - t0)
  const sameHour =
    a.getFullYear() === b.getFullYear() &&
    a.getMonth() === b.getMonth() &&
    a.getDate() === b.getDate() &&
    a.getHours() === b.getHours()

  if (spanMs < 15 * 60 * 1000 && sameHour) {
    return function (ts) {
      const d = new Date(ts)
      return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds())
    }
  }
  if (spanMs >= 20 * 60 * 60 * 1000) {
    return function (ts) {
      const d = new Date(ts)
      return pad(d.getMonth() + 1) + '-' + pad(d.getDate())
    }
  }
  return function (ts) {
    const d = new Date(ts)
    return pad(d.getHours()) + ':' + pad(d.getMinutes())
  }
}

/**
 * LTTB（Largest Triangle Three Buckets）抽稀。
 *
 * 直接「每 N 个点取一个」会把尖峰整根砍掉——温度图上最关键的往往就是那个尖峰。
 * LTTB 把数据分成若干桶，每桶只保留「与前后桶连成的三角形面积最大」的那个点，
 * 也就是视觉上最能代表这个桶形状的点，尖峰会被留住。
 *
 * 首尾点固定保留，保证折线两端不缩。
 */
function lttb(points, threshold) {
  const n = points.length
  if (threshold >= n || threshold < 3) return points.slice()

  const sampled = [points[0]]
  const every = (n - 2) / (threshold - 2)
  let a = 0

  for (let i = 0; i < threshold - 2; i++) {
    // 后一个桶的平均点，作为三角形的第三个顶点
    let avgX = 0
    let avgY = 0
    let start = Math.floor((i + 1) * every) + 1
    let end = Math.floor((i + 2) * every) + 1
    if (end > n) end = n
    const len = end - start || 1
    for (let j = start; j < end; j++) {
      avgX += j
      avgY += points[j].v
    }
    avgX /= len
    avgY /= len

    // 当前桶：挑面积最大的那个
    const s = Math.floor(i * every) + 1
    const e = Math.min(Math.floor((i + 1) * every) + 1, n - 1)
    const ax = a
    const ay = points[a].v
    let maxArea = -1
    let maxIdx = s
    for (let j = s; j < e; j++) {
      const area = Math.abs((ax - avgX) * (points[j].v - ay) - (ax - j) * (avgY - ay))
      if (area > maxArea) {
        maxArea = area
        maxIdx = j
      }
    }
    sampled.push(points[maxIdx])
    a = maxIdx
  }

  sampled.push(points[n - 1])
  return sampled
}

function round1(x) {
  return Math.round(x * 10) / 10
}

/** 值序列 → 折线段 + 数据点（单位 rpx / deg） */
function buildChart(series, min, max) {
  const n = series.length
  const span = max - min
  const usable = PLOT_H - PAD_Y * 2

  const xs = []
  const ys = []
  for (let i = 0; i < n; i++) {
    xs.push(n === 1 ? PLOT_W / 2 : (i / (n - 1)) * PLOT_W)
    // 数值全等（span 为 0）时统一落在竖中线，不除零
    const r = span === 0 ? 0.5 : (series[i].v - min) / span
    ys.push(PAD_Y + (1 - r) * usable)
  }

  const segs = []
  for (let i = 0; i < n - 1; i++) {
    const dx = xs[i + 1] - xs[i]
    const dy = ys[i + 1] - ys[i]
    const len = Math.sqrt(dx * dx + dy * dy)
    // atan2 给的是弧度，转成 deg 交给 WXSS 的 rotate()
    segs.push({
      k: i,
      l: round1(xs[i]),
      t: round1(ys[i]),
      // +1rpx 让相邻两段有一点重叠，转角处不会露缝
      w: round1(len + 1),
      r: round1((Math.atan2(dy, dx) * 180) / Math.PI),
    })
  }

  const dots = []
  if (n <= DOT_LIMIT) {
    for (let i = 0; i < n; i++) {
      dots.push({ k: i, l: round1(xs[i]), t: round1(ys[i]), last: i === n - 1 })
    }
  } else if (n > 1) {
    // 点多了只留最后一点：用户要靠它看出「现在在哪」，
    // 中间的圆点会糊成一条毛边，所以不画。
    dots.push({ k: n - 1, l: round1(xs[n - 1]), t: round1(ys[n - 1]), last: true })
  }

  return { segs: segs, dots: dots }
}

Page({
  data: {
    theme: theme.resolve(),
    loading: true,
    error: '',
    devices: [],
    deviceNames: [],
    deviceIndex: 0,
    deviceId: '',
    fields: model.TREND_FIELDS,
    field: 'SHT30_T',
    fieldLabel: '温度',
    unit: '℃',
    segs: [],
    dots: [],
    sampled: 0,
    stats: { cur: '--', max: '--', min: '--', avg: '--' },
    count: 0,
    rangeText: '',
    // 异常汇总（L1）
    summary: [],
    summaryBad: 0,
  },

  onShow: function () {
    theme.apply(this)
    this.bootstrap()
    // 自动刷新：静默模式（不闪 loading），并强制同步一次，历史点才会长出来
    auto.start(this, function () {
      return this.bootstrap({ silent: true, force: true })
    })
  },

  onHide: function () {
    auto.stop(this)
  },

  onPullDownRefresh: function () {
    this.fetch().then(function () {
      wx.stopPullDownRefresh()
    })
  },

  /** 先拿设备列表，再拉趋势 */
  bootstrap: function (opts) {
    const self = this
    const silent = !!(opts && opts.silent)
    const force = !!(opts && opts.force)
    if (this.__loading) return Promise.resolve()
    this.__loading = true
    if (!silent) this.setData({ loading: true, error: '' })

    const app = getApp()
    const ready = (app && app.ready) || Promise.resolve(null)

    return ready
      .then(function () {
        return api.overview({ force: force })
      })
      .then(function (r) {
        const devices = r.devices || []
        if (!devices.length) {
          self.__loading = false
          self.setData({ loading: false, devices: [], deviceNames: [], deviceId: '' })
          return null
        }
        // 尽量保留用户已选的设备
        let idx = 0
        for (let i = 0; i < devices.length; i++) {
          if (devices[i].deviceId === self.data.deviceId) {
            idx = i
            break
          }
        }
        self.setData({
          devices: devices,
          deviceNames: devices.map(function (d) {
            return d.name || d.deviceId
          }),
          deviceIndex: idx,
          deviceId: devices[idx].deviceId,
        })
        self.__loading = false
        return self.fetch(silent)
      })
      .catch(function (e) {
        self.__loading = false
        self.setData({ loading: false, error: (e && e.message) || '加载失败' })
        return null
      })
  },

  /** 拉当前设备的历史点：四个指标一次拉齐，画图只用其中一个，其余留给异常汇总 */
  fetch: function (silent) {
    const self = this
    if (!this.data.deviceId) {
      this.setData({ loading: false, segs: [], dots: [], count: 0, summary: [], summaryBad: 0 })
      return Promise.resolve()
    }
    if (this.__fetching) return Promise.resolve()
    this.__fetching = true
    if (!silent) this.setData({ loading: true })

    const deviceId = this.data.deviceId
    const jobs = L1_FIELDS.map(function (f) {
      // 单个字段失败不该让整页报错：那一行汇总显示「无数据」就够了
      return api.trend(deviceId, f, LIMIT).catch(function () {
        return { points: [] }
      })
    })

    return Promise.all(jobs)
      .then(function (list) {
        self.__fetching = false
        const byField = {}
        L1_FIELDS.forEach(function (f, i) {
          byField[f] = (list[i] && list[i].points) || []
        })
        self.__byField = byField
        self.applyPoints(byField[self.data.field] || [])
        self.applySummary(byField)
      })
      .catch(function (e) {
        self.__fetching = false
        self.setData({ loading: false, error: (e && e.message) || '加载失败' })
      })
  },

  /**
   * 四个指标各自「最近一次越线的时间点」，算在抽稀之前（用原始点）。
   * 抽稀后的点是画图用的，漏掉的尖峰恰恰是异常点，不能拿它算。
   */
  applySummary: function (byField) {
    const r = l1.summarize(byField, Date.now())
    this.setData({ summary: r.items, summaryBad: r.bad })
  },

  applyPoints: function (points) {
    const field = this.data.field
    const meta = model.FIELD_META[field] || {}
    const digits = meta.digits === undefined ? 0 : meta.digits
    const unit = meta.unit || ''

    if (!points.length) {
      this.setData({
        loading: false,
        segs: [],
        dots: [],
        count: 0,
        sampled: 0,
        stats: { cur: '--', max: '--', min: '--', avg: '--' },
        rangeText: '',
      })
      return
    }

    // 统计用原始数据算，不受抽稀影响
    const vs = points.map(function (p) {
      return p.v
    })
    let max = vs[0]
    let min = vs[0]
    let sum = 0
    for (let i = 0; i < vs.length; i++) {
      if (vs[i] > max) max = vs[i]
      if (vs[i] < min) min = vs[i]
      sum += vs[i]
    }

    const t0 = points[0].ts
    const t1 = points[points.length - 1].ts
    const fmt = makeFmt(t0 || 0, t1 || 0)

    const series = lttb(points, MAX_POINTS)
    const chart = buildChart(series, min, max)

    this.setData({
      loading: false,
      segs: chart.segs,
      dots: chart.dots,
      count: points.length,
      sampled: series.length,
      unit: unit,
      stats: {
        cur: vs[vs.length - 1].toFixed(digits),
        max: max.toFixed(digits),
        min: min.toFixed(digits),
        avg: (sum / vs.length).toFixed(digits),
      },
      rangeText: t0 && t1 ? fmt(t0) + ' — ' + fmt(t1) : '',
    })
  },

  onFieldTap: function (e) {
    this.switchField(e.currentTarget.dataset.key)
  },

  /** 点汇总行 = 把曲线切到那一项（用户看到「温度 18:32 越线」自然想看温度曲线） */
  onSummaryTap: function (e) {
    this.switchField(e.currentTarget.dataset.key)
  },

  switchField: function (key) {
    if (!key || key === this.data.field) return
    const f = model.TREND_FIELDS.filter(function (x) {
      return x.key === key
    })[0]
    this.setData(
      {
        field: key,
        fieldLabel: f ? f.label : key,
        unit: f ? f.unit : '',
      },
      () => {
        // 四个指标是一次拉齐的，切属性只是换一条曲线，不用再打一次云函数
        const cached = this.__byField && this.__byField[key]
        if (cached && cached.length) this.applyPoints(cached)
        else this.fetch()
      }
    )
  },

  onDeviceChange: function (e) {
    const idx = parseInt(e.detail.value, 10) || 0
    const d = this.data.devices[idx]
    if (!d) return
    this.setData({ deviceIndex: idx, deviceId: d.deviceId }, () => this.fetch())
  },

  onRetry: function () {
    this.bootstrap()
  },
})
