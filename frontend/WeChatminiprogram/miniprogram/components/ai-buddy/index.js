/**
 * Luster —— 设备看板上的桌宠
 *
 * 它是从「数据小助手」长出来的：同一套立绘、同一套三层兜底，
 * 但行为从「点一下才动」变成了「自己在页面上过日子」。
 *
 * 五个行为，互不打扰：
 *   1. 寒暄   每 42–105 秒自己冒一句（纯本地话术表，不读盘不联网），3.6 秒后收
 *   2. 单击   立刻讲一段当前数据（本地拼，0 延迟），底部可再让云端 AI 细说
 *   3. 双击   打开对话窗口 —— P1 只留空壳，见 onChatMessage
 *   4. 拖拽   按住拖到任意位置，松手贴左右边缘会吸附，位置记进缓存
 *   5. 休眠   70 秒没人碰屏幕就缩回角落闭眼，点一下醒
 *
 * 为什么单击不走云端：
 *   点一下就要看到东西。云函数要 0.5–2 秒，冷启动、超时、断网任何一种都会变成
 *   「点了没反应」。所以默认读内存里已有的数据同步拼一段（utils/luster-brief.js），
 *   云端降级成「让 AI 细说」这个可选项。
 *
 * 三层 transform 不能合，会互相覆盖：
 *   .bfab-roam → 人在哪（拖拽 / 走路位移）
 *   .bfab      → 人怎么动（呼吸浮动 / 碎步 / 站定）
 *   .bfab-img  → 缩放（按下、休眠）
 */
const api = require('../../utils/api')
// 落点几何（纯函数，能用 node 单测；预览生成器也复用同一份）
const geo = require('../../utils/buddy-geo')
// 寒暄话术 + 时段判断
const lines = require('../../utils/luster-lines')
// 单击时的本地数据总结
const brief = require('../../utils/luster-brief')
// 双击后的预设问题表 + 端上兜底答复
const chatMod = require('../../utils/luster-chat')
// 触摸判定（拖拽 / 单击 / 双击）
const gestureMod = require('../../utils/luster-gesture')

// 展示给模型的字段（13 个真实属性，别改名）
const FIELDS = [
  'SHT30_T',
  'SHT30_H',
  'Gas',
  'Body_IR',
  'Light',
  'Sound',
  'Fill',
  'Fan',
  'AI_Score',
  'Temp_Rise',
  'Fuse_Level',
  'Fuse_Score',
  'Dominant',
]

// 图片路径集中在表里：以后换图只改这里，不用翻 wxml
const BASE = '/assets/buddy/'
const FRAMES = {
  navy: {
    idle: BASE + 'buddy-idle-navy.png',
    blink: BASE + 'buddy-blink-navy.png',
    wave: BASE + 'buddy-wave-navy.png',
    walk1: BASE + 'buddy-walk1-navy.png',
    walk2: BASE + 'buddy-walk2-navy.png',
    walk3: BASE + 'buddy-walk3-navy.png',
    sleep: BASE + 'buddy-sleep-navy.png',
  },
  white: {
    idle: BASE + 'buddy-idle-white.png',
    blink: BASE + 'buddy-blink-white.png',
    wave: BASE + 'buddy-wave-white.png',
    walk1: BASE + 'buddy-walk1-white.png',
    walk2: BASE + 'buddy-walk2-white.png',
    walk3: BASE + 'buddy-walk3-white.png',
    sleep: BASE + 'buddy-sleep-white.png',
  },
}

/**
 * 走路循环：左腿在前 → 并拢 → 右腿在前 → 并拢。
 * 三张图走四步，中间那张当两次过渡 —— 直接 1→2→3→1 少一帧过渡，
 * 左右换边时会"啪"地跳一下，看着像瘸。
 */
const WALK_CYCLE = ['walk1', 'walk2', 'walk3', 'walk2']
const HEADS = {
  navy: BASE + 'buddy-head-navy.png',
  white: BASE + 'buddy-head-white.png',
}

/**
 * 主题 → 衣服，**交叉**搭配，不是同色系。
 * 深色主题穿白装：近黑底上深蓝外套会糊成一团，只有头发和脸浮着；
 * 浅色主题穿深蓝装：浅色外套贴白底会淡到几乎看不见。
 */
const OUTFIT = {
  dark: 'white',
  light: 'navy',
}

// 本地缓存的 key（只存两个东西：停靠位置、有没有自我介绍过）
const DOCK_KEY = 'luster:dock'
const GREET_KEY = 'luster:greet'

const TICK_MS = 32
const CHARS_PER_TICK = 3
const BLINK_EVERY = 4200 // 多久眨一次眼
const BLINK_HOLD = 240 // 闭眼持续多久
const WAVE_HOLD = 1500 // 点开后挥手多久

/* ------------------------------ 节奏参数 ------------------------------ */

const IDLE_MS = 70000 // 多久没人碰屏幕就休眠
const SAY_MIN = 42000 // 寒暄最短间隔
const SAY_MAX = 105000 // 寒暄最长间隔
const SAY_HOLD = 3600 // 一句寒暄停多久

/* ------------------------------ 对话参数 ------------------------------ */

// 「能不能打字」的探测结果缓存：能打字缓存久一点（5 分钟），不能打字只缓存 15 秒。
// 以前统一 5 分钟，结果在云端开开关 / 重传云函数之后，端上还得对着 5 分钟前那个
// false 发呆，看着就像「改了没用」。
const CAPS_TTL_ON = 5 * 60 * 1000
const CAPS_TTL_OFF = 15 * 1000
const FREE_MAX_LEN = 120 // 和云端 FREE_MAX_LEN 一致，超了在前端就截断
// 带几轮历史给模型。3 轮够理解「那湿度呢」「它呢」这类追问，
// 再多既费 token 也容易让模型顺着旧数据答（数据 5 秒一变，旧数字早过期了）
const HISTORY_TURNS = 6
const SAY_GAP = 22000 // 两次寒暄至少隔这么久（拖拽、说话会推后）
const DRAG_THROTTLE = 32 // 拖拽跟手的 setData 节流（约 30fps）

const CLICK_DELAY = gestureMod.DOUBLE_WINDOW + 20 // 等第二下的时间

/* ------------------------------ 小工具 ------------------------------ */

function rand(min, max) {
  return min + Math.random() * (max - min)
}

function winInfo() {
  try {
    if (typeof wx.getWindowInfo === 'function') return wx.getWindowInfo()
    return wx.getSystemInfoSync()
  } catch (e) {
    return { windowWidth: 375, windowHeight: 667 }
  }
}

function pickProps(props) {
  const out = {}
  const src = props || {}
  FIELDS.forEach(function (k) {
    if (src[k] !== undefined && src[k] !== null && src[k] !== '') out[k] = src[k]
  })
  return out
}

function readStorage(key, fallback) {
  try {
    const v = wx.getStorageSync(key)
    return v === '' || v === null || v === undefined ? fallback : v
  } catch (e) {
    return fallback
  }
}

function writeStorage(key, value) {
  try {
    wx.setStorageSync(key, value)
  } catch (e) {
    /* 存不进去不影响用，最多下次不记得停在哪 */
  }
}

Component({
  properties: {
    devices: {
      type: Array,
      value: [],
    },
    alarmSummary: {
      type: Object,
      value: { severe: 0, warning: 0, info: 0, total: 0 },
    },
    theme: {
      type: String,
      value: 'light',
    },
  },

  data: {
    // 气泡
    open: false,
    mode: 'data', // data = 讲数据 | chat = 预设问答
    thinking: false,
    busy: false,
    shown: '',
    full: '',
    typing: false,
    hint: '正在翻你的数据…',
    source: '',
    sourceText: '',
    // 对话：msgs = [{ id, role: 'me'|'her', text, src, live }]
    msgs: [],
    presets: chatMod.PRESETS,
    asking: false,
    chatAnchor: '',
    // 自由输入（默认关）。真正的开关在云端 FREE_CHAT，这里只是「health 说能用」的缓存结果。
    freeChat: false,
    draft: '',
    // 键盘顶起来时的处理：kbStyle 盖在气泡上做垂直定位，kbOn 时对话区压矮一档
    kbOn: false,
    kbStyle: '',
    // 寒暄小气泡（和主气泡互斥，主气泡开着时不出）
    say: '',
    sayX: 0,
    sayW: 0,
    sayBelow: false,
    // 形象
    outfit: 'navy',
    frame: 'idle',
    imgSrc: FRAMES.navy.idle,
    headSrc: HEADS.navy,
    // 站位：dx/dy 是相对"原位"的位移（px）。原位由 wxss 的 right/bottom 决定，
    // 这里只叠一个位移量，好处是不用去算 rpx 和安全区，屏幕怎么变原位都跟着走。
    // 拖拽后 __dock 会变，"原位"跟着搬过去，漫游始终围绕它。
    pos: { dx: 0, dy: 0 },
    dur: 0, // 这一段的移动时长（0 = 瞬移，拖拽/进出场时用）
    walking: false, // 移动中：碎步动画
    perched: false, // 站在卡片边沿或虚线上：停止上下浮动
    dragging: false, // 手指正在拖
    sleeping: false, // 休眠中
    // 气泡相对小人的位置（每次打开时按小人在哪算一遍）
    bpX: 0,
    bpArrow: 180,
    bpBelow: false,
  },

  observers: {
    // 切主题时换整套衣服，但保持当前帧（正在眨眼就换闭眼的那套）
    theme: function () {
      this.applyTone()
    },
  },

  attached: function () {
    this.__visible = true
    this.__dock = { dx: 0, dy: 0 }
    this.__perches = []
    this.__g = gestureMod.createGesture()
    this.__greet = readStorage(GREET_KEY, null)
    this.__savedDock = readStorage(DOCK_KEY, null)
    this.__caps = null

    // 键盘顶起来时自己抬气泡：气泡是 position:absolute，微信的 adjust-position
    // 只推页面、不推它，不自己处理就会被键盘盖住输入框。
    if (wx.onKeyboardHeightChange) {
      const self = this
      this.__kbHandler = function (res) {
        self.applyKb((res && res.height) || 0)
      }
      wx.onKeyboardHeightChange(this.__kbHandler)
    }

    this.applyTone()
    this.warmup()
    this.startBlink()
    this.resetIdle()
    this.scheduleSay(rand(12000, 22000)) // 头一句早点说，别让人等两分钟
  },

  ready: function () {
    // 首次渲染完才量得到原位；量到之后才会开始晃
    this.measureHome(6)
  },

  detached: function () {
    this.stopType()
    this.stopAnim()
    this.stopRoam()
    this.stopSay()
    this.stopIdle()
    this.clearClickWait()
    // 全局键盘监听不解绑会随着页面切换越积越多，每个实例都收到一遍
    if (this.__kbHandler && wx.offKeyboardHeightChange) {
      wx.offKeyboardHeightChange(this.__kbHandler)
      this.__kbHandler = null
    }
  },

  pageLifetimes: {
    hide: function () {
      this.__visible = false
      this.stopType()
      this.stopAnim()
      this.stopRoam()
      this.stopSay()
      this.stopIdle()
      this.clearClickWait()
      // 拖到一半切走（切 tab / 锁屏）：必须把拖拽收尾并通知页面解除滚动冻结，
      // 否则再回到这个页面，页面被 overflow:hidden 钉住，怎么滚都滚不动。
      if (this.data.dragging) {
        this.__g.cancel()
        this.__dock = { dx: this.data.pos.dx, dy: this.data.pos.dy }
        this.saveDock()
        this.__lastDragAt = 0
        this.setData({ dragging: false, walking: false })
      }
      // 无条件解锁：页面锁着不放，切回来就滚不动了
      this.triggerEvent('drag', { on: false })
      this.goHome(true)
      if (this.data.open) this.setData({ open: false, kbOn: false, kbStyle: '' })
      if (this.data.asking) this.setData({ asking: false })
      if (this.data.say) this.setData({ say: '' })
    },
    show: function () {
      this.__visible = true
      this.startBlink()
      this.resetIdle()
      this.scheduleTrip(rand(2000, 5000))
      this.scheduleSay(rand(10000, 20000))
    },
  },

  methods: {
    /** 吃掉冒泡，避免点面板时把后面的页面滚动/点击带出来 */
    noop: function () {},

    /* ------------------------------ 形象控制 ------------------------------ */

    applyTone: function () {
      const outfit = OUTFIT[this.properties.theme] || 'navy'
      const frame = this.data.frame || 'idle'
      this.setData({
        outfit: outfit,
        imgSrc: FRAMES[outfit][frame] || FRAMES[outfit].idle,
        headSrc: HEADS[outfit],
      })
      // 换了套衣服，待机帧和走路帧都还没解码过。
      // 只预热走路帧会漏掉最常见的那一次：切完主题的第一下眨眼现解码，照样闪。
      this.warmup(outfit)
      this.warmWalk()
    },

    setFrame: function (frame) {
      const outfit = this.data.outfit || 'navy'
      if (this.data.frame === frame && this.data.imgSrc === FRAMES[outfit][frame]) return
      this.setData({ frame: frame, imgSrc: FRAMES[outfit][frame] })
    },

    /**
     * 预加载当前这套衣服的待机三帧 + 头像。
     * 不预热的话，第一次眨眼时 image 还在解码，会出现一瞬间的空白 —— 看着像闪了一下。
     * outfit 显式传进来：applyTone 里刚 setData 完，不能保证 this.data 已经更新。
     */
    warmup: function (outfit) {
      outfit = outfit || this.data.outfit || 'navy'
      const list = [FRAMES[outfit].idle, FRAMES[outfit].blink, FRAMES[outfit].wave, HEADS[outfit]]
      list.forEach(function (src) {
        wx.getImageInfo({ src: src, fail: function () {} })
      })
    },

    /**
     * 走路帧和睡姿晚一点再预热。
     * 放到首屏之后：这四张加起来 200KB，跟首屏数据抢解码不划算，
     * 而第一次走路最短也要等 9 秒，来得及。
     */
    warmWalk: function () {
      const self = this
      const outfit = this.data.outfit || 'navy'
      if (this.__warmed === outfit) return
      this.__warmed = outfit
      setTimeout(function () {
        ;['walk1', 'walk2', 'walk3', 'sleep'].forEach(function (k) {
          wx.getImageInfo({ src: FRAMES[outfit][k], fail: function () {} })
        })
      }, 800)
    },

    startBlink: function () {
      const self = this
      if (this.__blinkTimer) return
      this.__blinkTimer = setInterval(function () {
        // 只在待机时眨：说话/挥手/睡着时眨眼会打架
        if (self.data.open || self.data.frame !== 'idle' || self.data.thinking || self.data.sleeping) return
        self.setFrame('blink')
        self.__blinkBack = setTimeout(function () {
          if (self.data.frame === 'blink') self.setFrame('idle')
        }, BLINK_HOLD)
      }, BLINK_EVERY)
    },

    stopAnim: function () {
      if (this.__blinkTimer) {
        clearInterval(this.__blinkTimer)
        this.__blinkTimer = null
      }
      if (this.__blinkBack) {
        clearTimeout(this.__blinkBack)
        this.__blinkBack = null
      }
      if (this.__waveTimer) {
        clearTimeout(this.__waveTimer)
        this.__waveTimer = null
      }
      this.stopWalkAnim()
    },

    wave: function () {
      const self = this
      this.setFrame('wave')
      if (this.__waveTimer) clearTimeout(this.__waveTimer)
      this.__waveTimer = setTimeout(function () {
        self.__waveTimer = null
        if (self.data.frame === 'wave') self.setFrame('idle')
      }, WAVE_HOLD)
    },

    /**
     * 走路帧循环。帧时长由 geo.frameMs() 按速度算出来，不写死 ——
     * 写死就会和位移速度脱钩，看着像穿着溜冰鞋。
     * 只在 walking 为真时跑，落地必须停，否则人站着腿还在迈。
     */
    startWalkAnim: function () {
      const self = this
      this.stopWalkAnim()
      let i = 0
      this.setFrame(WALK_CYCLE[0])
      const ms = geo.frameMs()
      this.__walkTimer = setInterval(function () {
        i = (i + 1) % WALK_CYCLE.length
        self.setFrame(WALK_CYCLE[i])
      }, ms)
    },

    stopWalkAnim: function () {
      if (this.__walkTimer) {
        clearInterval(this.__walkTimer)
        this.__walkTimer = null
      }
    },

    /* ------------------------------ 手势 ------------------------------ */

    onTouchStart: function (e) {
      const t = e.touches && e.touches[0]
      if (!t) return
      // 手指一落下就通知页面锁滚动。不能等 6px 判定出「这是拖拽」——
      // 那 6px 的下拉已经把页面拉动、把刷新头拽出来了，锁得再准也晚了。
      this.triggerEvent('drag', { on: true })
      // 有任何触摸就推迟休眠（页面上的其它地方也算，见 pages/device 的 onUserTouch）
      this.resetIdle()
      /**
       * 走路途中被按住的坑：data.pos 已经是这一段的目标值，视觉上还在半路上，
       * 直接拿它当拖拽起点，手指一动整个人就会跳半个身位。
       * 所以先停步（dur:0 让过渡立即结束，视觉瞬移到终点），再按那个位置跟手。
       */
      if (this.data.walking) {
        this.stopRoam()
        this.stopWalkAnim()
        this.setData({ walking: false, dur: 0 })
        this.setFrame('idle')
      }
      this.__dragBase = { dx: this.data.pos.dx, dy: this.data.pos.dy }
      this.__g.start(t.clientX, t.clientY)
    },

    onTouchMove: function (e) {
      const t = e.touches && e.touches[0]
      if (!t) return
      const ev = this.__g.move(t.clientX, t.clientY)
      if (!ev) return // 只是手指抖，还没到 6px

      this.resetIdle()

      if (ev.first) {
        // 正式进入拖拽：停掉所有自动行为，关掉气泡，去掉位移过渡（不然跟手会滞后）
        this.stopRoam()
        this.stopSay()
        this.stopType() // 光把 open 置 false 不够，逐字输出的定时器还在跑
        this.hideSay()
        this.clearClickWait()
        if (this.data.sleeping) this.setData({ sleeping: false })
        this.setData({
          dragging: true,
          dur: 0,
          walking: false,
          perched: false,
          open: false,
          typing: false,
          asking: false,
          kbOn: false,
          kbStyle: '',
        })
        this.stopWalkAnim()
        this.setFrame('idle')
        if (!this.__blinkTimer) this.startBlink()
      }

      this.applyDrag(ev.dx, ev.dy)
    },

    onTouchEnd: function (e) {
      // 一松手就解锁：桌宠是 fixed，页面恢复滚动不会影响它的吸附动画
      this.triggerEvent('drag', { on: false })
      const t = (e.changedTouches && e.changedTouches[0]) || null
      const x = t ? t.clientX : 0
      const y = t ? t.clientY : 0
      const ev = this.__g.end(x, y, Date.now())
      this.resetIdle()

      if (ev.type === 'drop') {
        this.finishDrag(ev.dx, ev.dy)
        return
      }
      if (ev.type === 'dbl') {
        this.openChat()
        return
      }

      // pending：等 300ms 看有没有第二下
      const self = this
      this.clearClickWait()
      this.__clickTimer = setTimeout(function () {
        self.__clickTimer = null
        if (self.__g.flush(Date.now()) === 'click') self.onClick()
      }, CLICK_DELAY)
    },

    onTouchCancel: function () {
      this.__g.cancel()
      this.clearClickWait()
      if (this.data.dragging) {
        // 拖到一半被打断（来电、切后台、系统手势）：就停在这，当成新的停靠位
        this.__dock = { dx: this.data.pos.dx, dy: this.data.pos.dy }
        this.saveDock()
        this.__lastDragAt = 0
        this.setData({ dragging: false, walking: false })
        this.scheduleTrip(rand(14000, 22000))
      }
      // 无条件解锁：touchstart 一按下就锁了，这里少发一次页面就永远滚不动
      this.triggerEvent('drag', { on: false })
    },

    clearClickWait: function () {
      if (this.__clickTimer) {
        clearTimeout(this.__clickTimer)
        this.__clickTimer = null
      }
    },

    /** 拖拽跟手：位移夹在屏幕内，30fps 节流（每帧 setData 会掉帧） */
    applyDrag: function (rawDx, rawDy) {
      const home = this.__home
      const win = this.__win
      const base = this.__dragBase
      if (!home || !win || !base) return
      const now = Date.now()
      if (this.__lastDragAt && now - this.__lastDragAt < DRAG_THROTTLE) return
      this.__lastDragAt = now
      const p = geo.clampPos(base.dx + rawDx, base.dy + rawDy, home, win)
      this.setData({ pos: p })
    },

    /** 松手：吸附到左右边缘、记进缓存、歇一会儿再恢复漫游 */
    finishDrag: function (rawDx, rawDy) {
      const home = this.__home
      const win = this.__win
      const base = this.__dragBase || { dx: 0, dy: 0 }
      this.__lastDragAt = 0
      if (!home || !win) {
        this.setData({ dragging: false })
        return
      }
      const clamped = geo.clampPos(base.dx + rawDx, base.dy + rawDy, home, win)
      const p = geo.snapEdge(clamped, home, win)
      this.__dock = p
      this.saveDock()
      this.setData({
        dragging: false,
        dur: 0,
        pos: p,
        perched: false,
        walking: false,
      })
      // 刚放下就被自己走开会很突兀，隔一会儿再动
      this.scheduleTrip(rand(14000, 22000))
      this.scheduleSay(rand(12000, 20000))
    },

    /** 存绝对坐标而不是相对位移：换台机器 home 会变，相对位移就错了 */
    saveDock: function () {
      const home = this.__home
      if (!home || !this.__dock) return
      writeStorage(DOCK_KEY, {
        x: Math.round(home.left + this.__dock.dx),
        y: Math.round(home.top + this.__dock.dy),
      })
    },

    /* ------------------------------ 晃悠 ------------------------------ */

    /**
     * 量原位。
     * .bfab-roam 是 fixed + right/bottom，所以它的 rect 就是"站在角落"时盒子的位置；
     * 之后所有移动都是相对这个位置的位移，不用去碰 rpx 和安全区（那是 CSS 的事）。
     */
    measureHome: function (retry) {
      const self = this
      this.createSelectorQuery()
        .select('.bfab-roam')
        .boundingClientRect(function (r) {
          if (!r || !r.width) {
            // 组件所在的 tab 还没显示时量出来是 0，等一下再量
            if (retry > 0) setTimeout(function () { self.measureHome(retry - 1) }, 200)
            return
          }
          self.__home = { left: r.left, top: r.top }
          const w = winInfo()
          self.__win = { w: w.windowWidth, h: w.windowHeight }

          // 恢复上次拖到哪了（存的是绝对坐标，换成相对位移后再夹一次屏内）
          const saved = self.__savedDock
          self.__savedDock = null
          if (saved && typeof saved.x === 'number' && typeof saved.y === 'number') {
            const p = geo.clampPos(saved.x - r.left, saved.y - r.top, self.__home, self.__win)
            self.__dock = p
            self.setData({ pos: p })
          }

          self.scheduleTrip(rand(5000, 9000))
        })
        .exec()
    },

    /** 现在能不能自己走动 */
    canRoam: function () {
      return (
        this.__visible !== false &&
        !!this.__home &&
        !!this.__win &&
        !this.data.open &&
        !this.data.thinking &&
        !this.data.dragging &&
        !this.data.sleeping
      )
    },

    /** 歇够了出发 */
    scheduleTrip: function (ms) {
      const self = this
      if (this.__tripTimer) clearTimeout(this.__tripTimer)
      this.__tripTimer = setTimeout(function () {
        self.__tripTimer = null
        if (!self.canRoam()) {
          self.scheduleTrip(4000)
          return
        }
        self.beginTrip()
      }, ms == null ? rand(9000, 16000) : ms)
    },

    /**
     * 一次出行 = 2~3 站，最后一个回停靠位。
     * 点位是固定的三个：停靠位 / 主卡下沿 / 虚线上沿（见 geo.perches），
     * 不再随机乱站 —— 随机站位在小屏上经常压住文字，而且没有"有目的地"的观感。
     */
    beginTrip: function () {
      const self = this
      this.__steps = 2 + Math.floor(Math.random() * 2)
      this.__perchedThisTrip = false
      this.measurePerches(function () {
        self.goStep()
      })
    },

    goStep: function () {
      const self = this
      // 中途被叫停（点开气泡 / 拖拽 / 切走）就原地不动，别把人拽回来
      if (!this.canRoam()) return

      const dest = this.pickDest()

      // 没地方可去了 / 走完了 → 回停靠位，然后歇着等下一趟
      if (this.__steps <= 0 || !dest) {
        const dock = this.__dock || { dx: 0, dy: 0 }
        const ddx = this.data.pos.dx - dock.dx
        const ddy = this.data.pos.dy - dock.dy
        const atHome = Math.sqrt(ddx * ddx + ddy * ddy) < 2
        // 回原位也按时长算：写死 900ms 的话，离得远（500px）就成了 555px/s，
        // 腿还在按 130px/s 迈，走起来像被拖回去
        const dur = atHome ? 0 : this.moveTo(dock.dx, dock.dy, { perch: false })
        this.__stepTimer = setTimeout(function () {
          self.__stepTimer = null
          self.scheduleTrip()
        }, dur + rand(800, 1600))
        return
      }

      this.__steps -= 1
      const dur = this.moveTo(dest.dx, dest.dy, { perch: !!dest.perch, sel: dest.sel })
      this.__stepTimer = setTimeout(function () {
        self.__stepTimer = null
        self.goStep()
      }, dur + rand(1500, 2600))
    },

    /**
     * 挑下一个点：只在「停靠位 + 页面里量到的落脚点」里选，并且跳过离当前位置太近的
     * （不然会出现"走 3px 再走回来"这种原地抽搐）。
     * 一趟里最多坐一次卡片 —— 连坐两次会像卡住了。
     */
    pickDest: function () {
      const dock = this.__dock || { dx: 0, dy: 0 }
      const cur = this.data.pos
      const list = [{ dx: dock.dx, dy: dock.dy, perch: false, sel: '', key: 'dock' }]

      const perches = this.__perches || []
      for (let i = 0; i < perches.length; i++) {
        const p = perches[i]
        list.push({ dx: p.dx, dy: p.dy, perch: true, sel: p.sel, key: p.sel || 'perch' + i })
      }

      const near = list.filter(function (d) {
        return Math.sqrt(Math.pow(d.dx - cur.dx, 2) + Math.pow(d.dy - cur.dy, 2)) > 40
      })
      if (!near.length) return null

      // 已经坐过一次就只说"回去"和"随便站"，不再坐卡片
      let pool = near
      if (this.__perchedThisTrip) {
        const homes = near.filter(function (d) {
          return !d.perch
        })
        if (homes.length) pool = homes
      }

      const seats = pool.filter(function (d) {
        return d.perch
      })
      if (seats.length && Math.random() < 0.7) {
        this.__perchedThisTrip = true
        return seats[Math.floor(Math.random() * seats.length)]
      }
      const homes = pool.filter(function (d) {
        return !d.perch
      })
      if (homes.length) return homes[0]
      return pool[0]
    },

    /**
     * 量落脚点。
     *   主卡（.hero）、虚线框（.add-module）—— 具体站哪儿由 geo.perches 决定。
     * 页面里没有就不坐，只往返停靠位，不会卡死。
     */
    measurePerches: function (cb) {
      const self = this
      const home = this.__home
      const win = this.__win
      if (!home) {
        this.__perches = []
        cb()
        return
      }
      const q = wx.createSelectorQuery()
      q.select('.hero').boundingClientRect()
      q.select('.add-module').boundingClientRect()
      q.exec(function (res) {
        self.__perches = geo.perches(res && res[0], res && res[1], home, win)
        cb()
      })
    },

    /**
     * 移过去。返回用时，外层按这个时长排下一次。
     * perched 要等落地才置真：路上还是走路动画，落地才"站定"。
     */
    moveTo: function (dx, dy, opt) {
      const self = this
      opt = opt || {}
      // 离开落脚点就不必再盯着那条线了
      if (!opt.perch) this.stopWatch()
      const cur = this.data.pos
      const dist = Math.sqrt(Math.pow(dx - cur.dx, 2) + Math.pow(dy - cur.dy, 2))
      const dur = opt.dur != null ? opt.dur : geo.walkTime(dist)
      const walk = dur > 60
      this.setData({
        pos: { dx: Math.round(dx), dy: Math.round(dy) },
        dur: dur,
        walking: walk,
        perched: false,
      })
      // 位移过渡在跑，腿也得跟着迈；只位移不迈腿就是滑行
      if (walk) this.startWalkAnim()
      else this.stopWalkAnim()
      if (this.__arriveTimer) clearTimeout(this.__arriveTimer)
      this.__arriveTimer = setTimeout(function () {
        self.__arriveTimer = null
        self.stopWalkAnim()
        self.setData({ walking: false, perched: !!opt.perch })
        // 落地一定回站姿：不然人站着腿还在迈。
        // 但睡着时不能碰 —— hide 时 goHome(true) 会走这条路，会把睡姿盖成站姿，
        // 切回页面就变成"站着的缩小版"，状态却是 sleeping。
        if (!self.data.sleeping) self.setFrame('idle')
        if (opt.perch && opt.sel) self.watchPerch(opt.sel)
      }, dur + 40)
      return dur
    },

    /**
     * 站在卡片上时盯一下落脚点。
     * 页面一滚，卡片就跑了，小人还钉在原来的屏幕坐标上 —— 看着就是坐在空气上。
     * 发现位置变了就直接回停靠位（不追，追着跑更像 bug）。
     */
    watchPerch: function (sel) {
      const self = this
      this.stopWatch()
      const q = wx.createSelectorQuery()
      q.select(sel).boundingClientRect()
      q.exec(function (res) {
        const r = res && res[0]
        if (!r) return
        self.__perchTop = r.top
      })
      this.__watchTimer = setInterval(function () {
        const q2 = wx.createSelectorQuery()
        q2.select(sel).boundingClientRect()
        q2.exec(function (res) {
          const r = res && res[0]
          if (!r || Math.abs(r.top - self.__perchTop) > 12) self.goHome(false)
        })
      }, 1200)
    },

    stopWatch: function () {
      if (this.__watchTimer) {
        clearInterval(this.__watchTimer)
        this.__watchTimer = null
      }
    },

    /** 回停靠位。instant=true 是瞬移（切页面、初始化），否则按距离走回去 */
    goHome: function (instant) {
      this.stopWatch()
      const dock = this.__dock || { dx: 0, dy: 0 }
      return this.moveTo(dock.dx, dock.dy, { perch: false, dur: instant ? 0 : null })
    },

    /** 停掉晃悠（说话、拖拽、休眠时用），保留眨眼定时器 */
    stopRoam: function () {
      this.stopWatch()
      if (this.__tripTimer) {
        clearTimeout(this.__tripTimer)
        this.__tripTimer = null
      }
      if (this.__stepTimer) {
        clearTimeout(this.__stepTimer)
        this.__stepTimer = null
      }
      if (this.__arriveTimer) {
        clearTimeout(this.__arriveTimer)
        this.__arriveTimer = null
      }
    },

    /**
     * 气泡该摆哪：跟着小人走。
     * 小人可能站在页面任何地方，气泡还钉在右下角就会"人话分离"，所以位置现算。
     */
    bubblePos: function () {
      const win = this.__win || { w: 375, h: 667 }
      const home = this.__home || { left: win.w - geo.BOX, top: win.h - geo.BOX - 40 }
      return geo.bubble(home, this.data.pos, win)
    },

    /* ------------------------------ 休眠 ------------------------------ */

    /** 页面任何地方被碰一下都算「有人在用」。由 pages/device 的根节点转发进来 */
    poke: function () {
      this.resetIdle()
    },

    resetIdle: function () {
      const self = this
      if (this.__idleTimer) clearTimeout(this.__idleTimer)
      this.__idleTimer = setTimeout(function () {
        self.__idleTimer = null
        if (self.data.open || self.data.dragging) {
          self.resetIdle()
          return
        }
        self.sleep()
      }, IDLE_MS)
    },

    stopIdle: function () {
      if (this.__idleTimer) {
        clearTimeout(this.__idleTimer)
        this.__idleTimer = null
      }
      if (this.__sleepTimer) {
        clearTimeout(this.__sleepTimer)
        this.__sleepTimer = null
      }
    },

    /** 缩回停靠位睡着：先走回去，落地再躺下，不然像在半路上睡着了 */
    sleep: function () {
      if (this.data.sleeping) return
      const self = this
      this.stopRoam()
      this.stopSay()
      this.hideSay()
      this.setData({ walking: false, perched: false })
      const dur = this.goHome(false)
      if (this.__sleepTimer) clearTimeout(this.__sleepTimer)
      // 等走到家再躺：时长跟着距离走，写死 820 的话远距离会"边走边躺"
      this.__sleepTimer = setTimeout(function () {
        self.__sleepTimer = null
        if (self.data.open || self.data.dragging) return
        if (self.__blinkTimer) {
          clearInterval(self.__blinkTimer)
          self.__blinkTimer = null
        }
        self.setData({ sleeping: true })
        // 换成真的睡姿：之前是拿「闭眼站姿」缩小冒充，像站着睡着
        self.setFrame('sleep')
      }, dur + 120)
    },

    /**
     * 醒过来。speak=true 时顺手讲一段数据 —— 睡着时点一下，一步到位，
     * 不用「先点醒、再点一次才说话」。
     */
    wake: function (speak) {
      const wasSleeping = this.data.sleeping
      this.resetIdle()
      if (!wasSleeping) return
      this.setData({ sleeping: false })
      this.setFrame('idle')
      this.startBlink()
      this.scheduleSay(rand(20000, 40000))
      this.scheduleTrip(rand(9000, 15000))
      if (speak) this.openData(lines.WAKE_LINE)
    },

    /* ------------------------------ 寒暄 ------------------------------ */

    scheduleSay: function (ms) {
      const self = this
      if (this.__sayTimer) clearTimeout(this.__sayTimer)
      this.__sayTimer = setTimeout(function () {
        self.__sayTimer = null
        self.trySay()
      }, ms == null ? rand(SAY_MIN, SAY_MAX) : ms)
    },

    /** 到点了，看这会儿方不方便说 */
    trySay: function () {
      const busy =
        this.__visible === false ||
        this.data.sleeping ||
        this.data.open ||
        this.data.dragging ||
        this.data.walking
      const tooSoon = this.__lastSayAt && Date.now() - this.__lastSayAt < SAY_GAP
      if (busy || tooSoon) {
        this.scheduleSay(rand(15000, 30000))
        return
      }
      this.sayOnce()
      this.scheduleSay()
    },

    sayOnce: function () {
      let item = null
      if (lines.isFirstMeet(this.__greet)) {
        item = { key: 'first', text: lines.FIRST_LINE }
        this.__greet = { seen: true }
        writeStorage(GREET_KEY, this.__greet)
      } else {
        item = lines.pick(new Date(), this.__lastSayKey)
      }
      this.__lastSayKey = item.key
      this.__lastSayAt = Date.now()
      this.speak(item.text)
    },

    speak: function (text) {
      const win = this.__win
      const home = this.__home
      if (!win || !home) return
      const t = geo.tip(win, home, this.data.pos, text.length)
      const self = this
      this.setData({ say: text, sayX: t.x, sayW: t.w, sayBelow: t.below })
      if (this.__sayHold) clearTimeout(this.__sayHold)
      this.__sayHold = setTimeout(function () {
        self.__sayHold = null
        self.hideSay()
      }, SAY_HOLD)
    },

    hideSay: function () {
      if (this.__sayHold) {
        clearTimeout(this.__sayHold)
        this.__sayHold = null
      }
      if (this.data.say) this.setData({ say: '' })
    },

    stopSay: function () {
      if (this.__sayTimer) {
        clearTimeout(this.__sayTimer)
        this.__sayTimer = null
      }
      if (this.__sayHold) {
        clearTimeout(this.__sayHold)
        this.__sayHold = null
      }
    },

    /* ------------------------------- 交互 ------------------------------- */

    /** 单击（已确认不是双击、不是拖拽） */
    onClick: function () {
      if (this.data.sleeping) {
        this.wake(true)
        return
      }
      if (this.data.open) {
        this.close()
        return
      }
      this.openData('')
    },

    /**
     * 讲一段数据。默认走本地（0 延迟），底部有「让 AI 细说」按钮才走云端。
     * prefix 用于唤醒时先说一句「唔…我睡着了」。
     */
    openData: function (prefix) {
      this.stopRoam()
      this.stopSay()
      this.hideSay()
      this.stopType()
      const bp = this.bubblePos()
      const body = brief.brief({
        devices: this.properties.devices,
        alarmSummary: this.properties.alarmSummary,
      })
      const text = prefix ? prefix + '。' + body : body

      this.setData({
        open: true,
        mode: 'data',
        thinking: false,
        busy: false,
        // 从对话切回简报：把上一轮的对话清掉，下次双击重新开始
        msgs: [],
        asking: false,
        bpX: bp.bpX,
        bpArrow: bp.bpArrow,
        bpBelow: bp.bpBelow,
        full: text,
        shown: '',
        typing: true,
        source: 'local',
        sourceText: '本地汇总',
        walking: false,
        say: '',
      })
      this.wave()
      this.startType()
    },

    /**
     * 双击：预设问答。
     *
     * 为什么本来没有输入框：个人主体拿不到「深度合成-AI 问答」类目。
     * 现在做成「云端开关 + 名单」：只有 FREE_CHAT 打开且这个 openid 在名单里，
     * health 才会返回 freeChat: true，端上才渲染输入框。默认关。
     */
    openChat: function () {
      this.stopRoam()
      this.stopSay()
      this.hideSay()
      this.stopType()
      if (this.data.sleeping) this.wake(false)
      const bp = this.bubblePos()
      this.__chatSeq = this.__chatSeq || 0
      const free = this.capsCached()
      this.setData({
        open: true,
        mode: 'chat',
        thinking: false,
        busy: false,
        asking: false,
        bpX: bp.bpX,
        bpArrow: bp.bpArrow,
        bpBelow: bp.bpBelow,
        msgs: [{ id: 'welcome', role: 'her', text: chatMod.welcome(free), src: '' }],
        chatAnchor: '',
        full: '',
        shown: '',
        typing: false,
        source: 'local',
        sourceText: '',
        walking: false,
        say: '',
        freeChat: free,
        draft: '',
        kbOn: false,
        kbStyle: '',
      })
      this.wave()
      this.scrollChat()
      // 缓存里没有就顺手问一次云端；拿到结果再改开场白（不阻塞渲染）
      this.syncCaps()
    },

    /* --------------------------- 能力探测（能否打字） --------------------------- */

    /** 缓存没过期就直接复用，避免每次双击都打一次云函数；失败结果只留 15 秒 */
    capsCached: function () {
      const c = this.__caps
      if (!c) return false
      return Date.now() - c.at < (c.free ? CAPS_TTL_ON : CAPS_TTL_OFF) && c.free
    },

    syncCaps: function () {
      const self = this
      const c = this.__caps
      const ttl = c && c.free ? CAPS_TTL_ON : CAPS_TTL_OFF
      if (c && Date.now() - c.at < ttl) return Promise.resolve(c.free)

      return api
        .aiHealth()
        .then(function (r) {
          const free = !!(r && r.freeChat)
          self.__caps = { at: Date.now(), free: free }
          if (self.data.mode !== 'chat') return free
          const patch = { freeChat: free }
          // 开场白只在还停在欢迎语时改，别把用户已经问出来的记录覆盖掉
          const list = self.data.msgs || []
          if (list.length === 1 && list[0].id === 'welcome') {
            patch['msgs[0].text'] = chatMod.welcome(free)
          }
          self.setData(patch)
          return free
        })
        .catch(function () {
          // health 都不通（没部署 / 没网）：当没开，别让用户对着假输入框打字
          self.__caps = { at: Date.now(), free: false }
          self.setData({ freeChat: false })
          return false
        })
    },

    /* ------------------------------ 自由输入 ------------------------------ */

    onInput: function (e) {
      this.setData({ draft: (e && e.detail && e.detail.value) || '' })
    },

    onFocus: function () {
      // 高度要等 onKeyboardHeightChange 才知道，这里先不动作
      this.resetIdle()
    },

    onBlur: function () {
      this.setData({ kbOn: false, kbStyle: '' })
    },

    /**
     * 键盘高度 → 气泡上移量。
     * 不用 transform：transform 在部分基础库上会让子元素点击命中区错位
     * （报警页左滑删除就栽在这上面），这里改成改 bottom/top 的布局值。
     */
    applyKb: function (h) {
      if (!this.data.open || this.data.mode !== 'chat') {
        if (this.data.kbOn || this.data.kbStyle) this.setData({ kbOn: false, kbStyle: '' })
        return
      }
      if (h <= 0) {
        this.setData({ kbOn: false, kbStyle: '' })
        return
      }
      const win = this.__win || { w: 375, h: 667 }
      // 最多抬到屏高 55%：再高对话区就只剩一条缝了
      const lift = Math.min(h, Math.round(win.h * 0.55))
      // 气泡挂在下方（bpBelow）时本来就贴着顶，改成 top:8 避免被键盘切掉底部
      const style = this.data.bpBelow ? 'top: 8px' : 'bottom: ' + (lift + 8) + 'px'
      this.setData({ kbOn: true, kbStyle: style })
    },

    onSend: function () {
      if (this.data.asking) return
      if (!this.data.freeChat) return
      const text = String(this.data.draft || '')
        .trim()
        .slice(0, FREE_MAX_LEN)
      if (!text) return
      this.setData({ draft: '' })
      return this.askFree(text)
    },

    /**
     * 最近几轮对话，给云端当上下文。
     *
     * 只收「我问的」和「它真答出来的」：
     *   - 开场白（id = welcome）不收，它不是一问一答；
     *   - 降级成「本地答复」的不收 —— 那是我自己拼的兜底句（「我这边没连上云端」），
     *     带上去模型会以为自己说过这话，接着往下编；
     *   - 正在打字（live）的空消息跳过。
     * 云端还会再截一次（条数、单条字数、角色白名单），这里只管别把整段历史塞上去。
     */
    buildHistory: function () {
      const list = this.data.msgs || []
      const out = []
      for (let i = 0; i < list.length; i++) {
        const m = list[i]
        if (!m || !m.text || m.live) continue
        if (m.id === 'welcome') continue
        if (m.role !== 'me' && m.role !== 'her') continue
        if (m.role === 'her' && String(m.src || '').indexOf('本地答复') >= 0) continue
        out.push({ role: m.role === 'me' ? 'user' : 'assistant', content: String(m.text) })
      }
      return out.slice(-HISTORY_TURNS)
    },

    /** 自由输入：和 ask 走同一套气泡/打字机，只是多带一段用户文本 */
    askFree: function (text) {
      this.stopSay()
      this.hideSay()
      this.stopRoam()
      if (this.data.sleeping) this.wake(false)

      // 必须在 push 当前问题**之前**取，否则「那湿度呢」会把自己也算进历史
      const history = this.buildHistory()

      const seq = (this.__chatSeq = (this.__chatSeq || 0) + 2)
      const myId = 'q' + seq
      const herId = 'a' + seq
      const msgs = (this.data.msgs || []).concat([
        { id: myId, role: 'me', text: text, src: '' },
        { id: herId, role: 'her', text: '', src: '', live: true },
      ])

      this.setData({ msgs: msgs, asking: true })
      this.scrollChat()

      const self = this
      return api
        .aiAsk(text, this.snapshot(), history)
        .then(function (r) {
          self.fill(herId, r.text, r.source || 'rule')
        })
        .catch(function (e) {
          console.warn('[luster] 自由输入失败：', (e && e.message) || e)
          // 原因也带上：这一行是排查「为什么老降级」的唯一线索
          self.fill(herId, chatMod.freeFallback(e), 'local', chatMod.errTag(e))
        })
    },

    /** 点胶囊：先在对话流里放自己的问题，再挂一个空的「她在想」占位 */
    onPreset: function (e) {
      const id = (e && e.currentTarget && e.currentTarget.dataset && e.currentTarget.dataset.id) || ''
      if (!chatMod.isPreset(id)) return
      if (this.data.asking) return
      const item = chatMod.PRESETS.filter(function (p) {
        return p.id === id
      })[0]
      this.ask(item.id, item.label)
    },

    ask: function (qid, label) {
      this.stopSay()
      this.hideSay()
      this.stopRoam()
      if (this.data.sleeping) this.wake(false)

      const seq = (this.__chatSeq = (this.__chatSeq || 0) + 2)
      const myId = 'q' + seq
      const herId = 'a' + seq
      const msgs = (this.data.msgs || []).concat([
        { id: myId, role: 'me', text: label, src: '' },
        { id: herId, role: 'her', text: '', src: '', live: true },
      ])

      this.setData({ msgs: msgs, asking: true })
      this.scrollChat()
      return this.onChatMessage(qid, herId)
    },

    /**
     * 对话的唯一出口。
     * 云端只接受白名单 qid；失败 / 超时 / 没部署 / 没配 key 一律降到本地答复，
     * 所以「点下去一定有话说」不依赖网络。
     */
    onChatMessage: function (qid, msgId) {
      const self = this
      if (!chatMod.isPreset(qid)) return Promise.reject(new Error('不支持的预设问题：' + qid))

      return api
        .aiChat(qid, this.snapshot())
        .then(function (r) {
          self.fill(msgId, r.text, r.source || 'rule')
        })
        .catch(function (e) {
          console.warn('[luster] 对话失败，改用本地答复：', (e && e.message) || e)
          self.fill(
            msgId,
            chatMod.answer(qid, {
              devices: self.properties.devices,
              alarmSummary: self.properties.alarmSummary,
            }),
            'local',
            chatMod.errTag(e)
          )
        })
    },

    /** 找到某条消息的下标（消息少，线性找足够） */
    msgIndex: function (id) {
      const list = this.data.msgs || []
      for (let i = 0; i < list.length; i++) {
        if (list[i].id === id) return i
      }
      return -1
    },

    /** 按 setData 路径改单条消息，避免整段数组重渲染 */
    updateMsg: function (id, patch) {
      const i = this.msgIndex(id)
      if (i < 0) return
      const data = {}
      Object.keys(patch).forEach(function (k) {
        data['msgs[' + i + '].' + k] = patch[k]
      })
      this.setData(data)
    },

    /**
     * 滚到底。
     * 先清空再设 chatEnd：scroll-into-view 的值不变时不会重新滚，
     * 连续两条回答会卡在中间不动。
     */
    scrollChat: function () {
      const self = this
      this.setData({ chatAnchor: '' })
      setTimeout(function () {
        self.setData({ chatAnchor: 'chatEnd' })
      }, 30)
    },

    /** 回答落到某条消息里，并逐字吐出来（和 data 模式共用同一套节奏常量） */
    fill: function (id, text, source, why) {
      // 请求还在路上时用户关了气泡 / 切了简报：消息已经清空，直接丢弃这次回答
      if (this.msgIndex(id) < 0) return
      // 降级原因只进日志，不摆在气泡上（那是给开发者看的，用户只需要知道「本地答复」）
      if (why) console.warn('[luster] 降级原因：' + why)
      const body = String(text || '').trim() || '这个我暂时答不上来，点别的问题试试。'
      const label =
        source === 'model'
          ? '由 AI 生成 · 仅供参考'
          : source === 'rule'
          ? '数据汇总 · 仅供参考'
          : '本地答复'
      this.setData({ asking: false })
      this.updateMsg(id, { src: label, live: false, text: '' })
      this.typeInto(id, body)
    },

    typeInto: function (id, full) {
      const self = this
      let i = 0
      let tick = 0
      this.stopType()
      this.__timer = setInterval(function () {
        i += CHARS_PER_TICK
        tick += 1
        if (i >= full.length) {
          self.stopType()
          self.updateMsg(id, { text: full, live: false })
          self.scrollChat()
          return
        }
        self.updateMsg(id, { text: full.slice(0, i) })
        // 每 12 帧滚一次：逐帧滚会把 setData 打满，完全不滚又会把回答顶出可视区
        if (tick % 12 === 0) self.scrollChat()
      }, TICK_MS)
    },

    /** 「让 AI 细说」：这时才走云函数 */
    askCloud: function () {
      const self = this
      if (this.data.busy) return
      this.stopType()
      this.setData({
        thinking: true,
        busy: true,
        shown: '',
        full: '',
        typing: false,
        source: '',
        sourceText: '',
        hint: this.properties.devices.length ? '正在翻你的数据…' : '正在看…',
      })

      api
        .aiBrief(this.snapshot())
        .then(function (r) {
          self.finish(r.text, r.source || 'rule')
        })
        .catch(function (e) {
          // 云端不可用不是错误，是降级：本地再拼一段，并说明是本地
          console.warn('[luster] 云端失败，改用本地汇总：', (e && e.message) || e)
          self.finish(
            brief.brief({
              devices: self.properties.devices,
              alarmSummary: self.properties.alarmSummary,
            }),
            'local'
          )
        })
    },

    close: function () {
      this.stopType()
      if (this.__waveTimer) {
        clearTimeout(this.__waveTimer)
        this.__waveTimer = null
      }
      this.setData({
        open: false,
        typing: false,
        thinking: false,
        busy: false,
        asking: false,
        msgs: [],
        // 关掉就清空草稿：下次点开还是那句话挂在输入框里会很怪
        draft: '',
        kbOn: false,
        kbStyle: '',
      })
      this.setFrame('idle')
      this.resetIdle()
      // 聊完走回停靠位，接着晃悠
      this.goHome(false)
      this.scheduleTrip(rand(4000, 8000))
      this.scheduleSay(rand(18000, 34000))
    },

    finish: function (text, source) {
      const body =
        String(text || '').trim() ||
        brief.brief({
          devices: this.properties.devices,
          alarmSummary: this.properties.alarmSummary,
        })
      const label =
        source === 'model'
          ? '由 AI 生成 · 仅供参考'
          : source === 'rule'
          ? '云端汇总 · 仅供参考'
          : '本地汇总 · 仅供参考'
      this.setData({
        thinking: false,
        busy: false,
        full: body,
        shown: '',
        typing: true,
        source: source,
        sourceText: label,
      })
      this.startType()
    },

    /** 逐字吐出来，比整段砸下来更像"在说话" */
    startType: function () {
      const self = this
      const full = this.data.full || ''
      let i = 0
      this.stopType()
      this.__timer = setInterval(function () {
        i += CHARS_PER_TICK
        if (i >= full.length) {
          self.stopType()
          self.setData({ shown: full, typing: false })
          return
        }
        self.setData({ shown: full.slice(0, i) })
      }, TICK_MS)
    },

    stopType: function () {
      if (this.__timer) {
        clearInterval(this.__timer)
        this.__timer = null
      }
    },

    /** 交给云端的快照：只带必要字段，最多 4 台，别把整包数据扔过去 */
    snapshot: function () {
      const ds = this.properties.devices || []
      return {
        at: Date.now(),
        alarmSummary: this.properties.alarmSummary || {},
        devices: ds.slice(0, 4).map(function (d) {
          return {
            name: d.name || d.deviceId || '设备',
            online: d.online !== false,
            reportedAt: d.reportedAt || null,
            alarmLevel: d.alarmLevel || null,
            alarmTitles: d.alarmTitles || [],
            props: pickProps(d.props),
          }
        }),
      }
    },
  },
})
