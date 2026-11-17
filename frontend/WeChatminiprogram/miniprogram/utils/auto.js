/**
 * 页面自动刷新（方案 A：前端定时刷新）
 *
 * 规则：
 *   - 只在页面「可见」时跑：onShow 启动，onHide / onUnload 停止，切到后台不空转；
 *   - 上一轮还没回来就跳过本轮，避免慢网络下请求堆积；
 *   - 间隔默认 30 秒，与云函数 sync.staleSeconds=30 对齐（调用时另带 force，见各页面）。
 *
 * 用法：
 *   const auto = require('../../utils/auto')
 *   onShow: function () { auto.start(this, function () { return this.load(false, true) }) },
 *   onHide: function () { auto.stop(this) },
 */
const INTERVAL_MS = 30000

/**
 * @param {Object} page 页面实例（this）
 * @param {Function} tick 每次触发时调用，可返回 Promise
 * @param {number} [ms] 自定义间隔
 */
function start(page, tick, ms) {
  if (!page || typeof tick !== 'function') return
  stop(page)
  page.__autoTimer = setInterval(function () {
    if (page.__autoBusy) return
    page.__autoBusy = true
    let p = null
    try {
      p = tick.call(page)
    } catch (e) {
      page.__autoBusy = false
      return
    }
    if (p && typeof p.then === 'function') {
      p.then(
        function () {
          page.__autoBusy = false
        },
        function () {
          page.__autoBusy = false
        }
      )
    } else {
      page.__autoBusy = false
    }
  }, ms || INTERVAL_MS)
}

/** 停止定时器（页面隐藏 / 卸载时必须调） */
function stop(page) {
  if (!page) return
  if (page.__autoTimer) {
    clearInterval(page.__autoTimer)
    page.__autoTimer = null
  }
  page.__autoBusy = false
}

module.exports = {
  start: start,
  stop: stop,
  INTERVAL_MS: INTERVAL_MS,
}
