/**
 * 主题模式：跟随系统 / 浅色 / 深色
 *
 * 为什么不继续只用 darkmode + theme.json：
 *   官方那套只能「跟随系统」，没有提供手动指定主题的 API（不存在 wx.setTheme）。
 *   要支持手动切换，只能自己接管：
 *     - 页面外观：根节点挂 `theme-light` / `theme-dark` 类，CSS 变量随类切换
 *     - 原生外壳：导航栏 / tabBar / 下拉区域用运行时 API 覆盖
 *
 * 用法（三处，缺一不可）：
 *   1) 页面 data 里写 `theme: theme.resolve()`  —— 模块加载时就算好，首屏不闪
 *   2) 页面根节点 class 加 `theme-{{theme}}`     —— 决定 CSS 变量
 *   3) onShow 里调用 `theme.apply(this)`         —— 补齐导航栏 / tabBar
 *
 * 只在云函数侧或原生组件（如 <switch color>）需要具体色值时，才用 theme.chrome()。
 */

const KEY = 'theme_mode_v1'

/** 三个可选模式，顺序即 UI 顺序 */
const MODES = [
  { key: 'system', label: '跟随系统' },
  { key: 'light', label: '浅色' },
  { key: 'dark', label: '深色' },
]

const VALID = MODES.map(function (m) {
  return m.key
})

/** tabBar 四项，icon 文件名的前缀 */
const TABS = [
  { text: '设备', name: 'device' },
  { text: '趋势', name: 'trend' },
  { text: '报警', name: 'alarm' },
  { text: '我的', name: 'mine' },
]

/**
 * 原生外壳配色。
 * 必须和 theme.json / app.wxss 保持一致，改一处要三处一起改。
 */
const CHROME = {
  light: {
    navBg: '#F7F8FC',
    navFg: '#000000', // 官方只接受 #ffffff / #000000
    bg: '#F7F8FC',
    tabBg: '#FFFFFF',
    tabColor: '#9AA3B2',
    tabSel: '#4C6FFF',
    tabBorder: 'white',
    icon: function (name) {
      return 'assets/tabbar/' + name + '.png'
    },
    iconOn: function (name) {
      return 'assets/tabbar/' + name + '-active.png'
    },
  },
  dark: {
    navBg: '#0B1120',
    navFg: '#ffffff',
    bg: '#070A12',
    tabBg: '#0B1120',
    tabColor: '#7A869A',
    tabSel: '#6AA6FF',
    tabBorder: 'black',
    icon: function (name) {
      return 'assets/tabbar/' + name + '-dark.png'
    },
    iconOn: function (name) {
      return 'assets/tabbar/' + name + '-dark-active.png'
    },
  },
}

/** 当前选中的模式（system / light / dark），读不到就给 system */
function getMode() {
  let v = ''
  try {
    v = wx.getStorageSync(KEY) || ''
  } catch (e) {
    v = ''
  }
  if (VALID.indexOf(v) < 0) return 'system'
  return v
}

/** 系统当前是深色还是浅色 */
function systemTheme() {
  try {
    // getAppBaseInfo 在开了 darkmode 之后才有 theme 字段
    if (wx.getAppBaseInfo) {
      const info = wx.getAppBaseInfo()
      if (info && info.theme) return info.theme === 'dark' ? 'dark' : 'light'
    }
  } catch (e) {
    /* 老基础库没有这个 API，走下面的兜底 */
  }
  try {
    const s = wx.getSystemInfoSync()
    if (s && s.theme) return s.theme === 'dark' ? 'dark' : 'light'
  } catch (e) {
    /* 拿不到就当浅色 */
  }
  return 'light'
}

/**
 * 把模式解析成实际生效的主题。
 * mode 省略时用已保存的模式。
 */
function resolve(mode) {
  const m = mode || getMode()
  if (m === 'light' || m === 'dark') return m
  return systemTheme() === 'dark' ? 'dark' : 'light'
}

/** 当前生效主题对应的原生外壳配色（给 <switch color> 这类不支持 CSS 变量的地方用） */
function chrome(theme) {
  return CHROME[theme === 'dark' ? 'dark' : 'light']
}

/** 覆盖原生外壳：导航栏、下拉区域、tabBar（含图标） */
function applyNative(theme) {
  const c = chrome(theme)

  if (wx.setNavigationBarColor) {
    wx.setNavigationBarColor({
      frontColor: c.navFg,
      backgroundColor: c.navBg,
      animation: { duration: 0, timingFunc: 'linear' },
      fail: function () {},
    })
  }

  // 下拉/回弹时露出的窗口底色；iOS 才有 top/bottom
  if (wx.setBackgroundColor) {
    wx.setBackgroundColor({
      backgroundColor: c.bg,
      backgroundColorTop: c.bg,
      backgroundColorBottom: c.bg,
      fail: function () {},
    })
  }

  if (wx.setTabBarStyle) {
    wx.setTabBarStyle({
      color: c.tabColor,
      selectedColor: c.tabSel,
      backgroundColor: c.tabBg,
      borderStyle: c.tabBorder,
      fail: function () {},
    })
  }

  // 图标也得换，否则深色底上是一组发虚的浅色图标
  if (wx.setTabBarItem) {
    TABS.forEach(function (t, i) {
      wx.setTabBarItem({
        index: i,
        text: t.text,
        iconPath: c.icon(t.name),
        selectedIconPath: c.iconOn(t.name),
        fail: function () {},
      })
    })
  }
}

/** 把主题推到所有已打开的页面（含 tab 页和详情页） */
function applyToPages(theme) {
  let pages = []
  try {
    pages = getCurrentPages() || []
  } catch (e) {
    pages = []
  }
  pages.forEach(function (p) {
    if (p && p.setData && p.data && p.data.theme !== theme) {
      p.setData({ theme: theme })
    }
  })
}

/**
 * 页面 onShow 调用：补齐原生外壳，并把 theme 同步到 data。
 * 返回 { mode, theme }，方便页面顺带更新模式文案。
 */
function apply(page) {
  const mode = getMode()
  const theme = resolve(mode)
  applyNative(theme)
  if (page && page.setData && page.data && page.data.theme !== theme) {
    page.setData({ theme: theme })
  }
  return { mode: mode, theme: theme }
}

/** 用户手动选模式 */
function setMode(mode) {
  const m = VALID.indexOf(mode) >= 0 ? mode : 'system'
  try {
    wx.setStorageSync(KEY, m)
  } catch (e) {
    /* 存不进去也不影响本次生效，只是下次打开会回到跟随系统 */
  }
  const theme = resolve(m)
  applyNative(theme)
  applyToPages(theme)
  return { mode: m, theme: theme }
}

/** 系统主题变化时的处理：只有「跟随系统」才需要跟着变 */
function onSystemChange() {
  if (getMode() !== 'system') return
  const theme = resolve('system')
  applyNative(theme)
  applyToPages(theme)
}

module.exports = {
  KEY: KEY,
  MODES: MODES,
  getMode: getMode,
  systemTheme: systemTheme,
  resolve: resolve,
  chrome: chrome,
  apply: apply,
  applyNative: applyNative,
  applyToPages: applyToPages,
  setMode: setMode,
  onSystemChange: onSystemChange,
}
