/**
 * app.js
 *
 * 启动顺序：落地主题 → 初始化云环境 → 调用 iotApi 的 login → 把 openid / 配置状态放进 globalData。
 * 页面通过 getApp().ready 等这个流程完成后再拉数据。
 */
const api = require('./utils/api')
const theme = require('./utils/theme')

App({
  globalData: {
    // 必须是完整 EnvId，不能只写环境名 cloud1
    env: 'cloud1-d2gjf2luuab8574fa',
    openid: '',
    // 云函数是否已填好华为云密钥
    configured: false,
    // 是否处于示例数据模式
    mock: false,
    auth: { allowed: true, mode: 'open' },
    // 当前主题模式：system / light / dark
    themeMode: 'system',
  },

  // 登录流程的 Promise，页面用它来等初始化完成
  ready: null,

  onLaunch: function () {
    // 主题最先落地：导航栏 / tabBar 越早设好，越不会看到一帧系统默认色
    theme.applyNative(theme.resolve())
    this.globalData.themeMode = theme.getMode()

    // 只有「跟随系统」需要跟着系统变；手动选了浅/深色就忽略系统变化
    if (wx.onThemeChange) {
      wx.onThemeChange(function () {
        theme.onSystemChange()
      })
    }

    if (!wx.cloud) {
      console.error('请使用 2.2.3 或以上的基础库以使用云能力')
      return
    }

    wx.cloud.init({
      env: this.globalData.env,
      traceUser: true,
    })

    const self = this
    this.ready = api
      .login()
      .then(function (r) {
        self.globalData.openid = r.openid || ''
        self.globalData.configured = !!r.configured
        self.globalData.mock = !!r.mock
        self.globalData.auth = r.auth || { allowed: true, mode: 'open' }
        console.log('[login] openid=' + r.openid, 'configured=' + r.configured, 'mock=' + r.mock)
        return r
      })
      .catch(function (e) {
        console.error('[login] 失败:', e && (e.message || e.errMsg || e))
        return null
      })
  },
})
