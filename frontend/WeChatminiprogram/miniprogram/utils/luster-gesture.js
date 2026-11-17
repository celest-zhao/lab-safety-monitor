/**
 * 触摸判定状态机（纯逻辑，不碰 wx）
 *
 * 为什么单独拎出来：一次触摸要拆成「拖拽 / 单击 / 双击」三种意图，
 * 而这三者天生互相干扰 —— 拖完手一松如果还判成单击，桌宠会一边被拖一边说话；
 * 双击如果每次先当单击处理，气泡会闪一下再消失。
 * 这种错真机上只是「偶尔怪一下」，根本没法稳定复现，只能靠断言盯住。
 *
 * 判定规则：
 *   位移 > 6px        → 拖拽（此后这次触摸不再产生任何点击）
 *   抬手，280ms 没再按 → 单击（由调用方延时调 flush 确认）
 *   抬手，280ms 内再按 → 双击（立刻返回，不等超时）
 *
 * 用法：
 *   const g = createGesture()
 *   onTouchStart: g.start(x, y)
 *   onTouchMove:  const e = g.move(x, y);  if (e) 跟手移动(e.dx, e.dy)
 *   onTouchEnd:   const e = g.end(x, y, Date.now())
 *                 if (e.type === 'drop') 吸附停靠()
 *                 if (e.type === 'dbl')  开对话()
 *                 if (e.type === 'pending') setTimeout(() => {
 *                   const r = g.flush(Date.now()); if (r === 'click') 说话()
 *                 }, 280)
 */

const MOVE_THRESHOLD = 6 // px，手指轻微抖动不算拖
const DOUBLE_WINDOW = 280 // ms，两次点击的间隔上限

function createGesture(opts) {
  const cfg = opts || {}
  const MOVE = cfg.moveThreshold == null ? MOVE_THRESHOLD : cfg.moveThreshold
  const DBL = cfg.doubleWindow == null ? DOUBLE_WINDOW : cfg.doubleWindow

  let sx = 0
  let sy = 0
  let dragging = false
  let moved = false
  let pending = false // 有一次点击在等「是不是第二下」
  let lastAt = 0

  function resetTouch() {
    dragging = false
    moved = false
    sx = 0
    sy = 0
  }

  return {
    config: { moveThreshold: MOVE, doubleWindow: DBL },

    /** 手指落下。注意：不清 pending —— 第二下点击的 start 不能把上一段的判定擦掉 */
    start: function (x, y) {
      resetTouch()
      sx = x
      sy = y
    },

    /** 手指移动。返回 null 表示还只是抖动，不算拖 */
    move: function (x, y) {
      const dx = x - sx
      const dy = y - sy
      if (!moved) {
        if (Math.sqrt(dx * dx + dy * dy) <= MOVE) return null
        moved = true
        dragging = true
        return { type: 'drag', dx: dx, dy: dy, first: true }
      }
      return { type: 'drag', dx: dx, dy: dy, first: false }
    },

    /** 手指抬起 */
    end: function (x, y, now) {
      const dx = x - sx
      const dy = y - sy
      const t = now == null ? Date.now() : now

      if (dragging) {
        resetTouch()
        // 拖完这一下不算点击：清掉 pending，避免「拖完再点一下」被判成双击
        pending = false
        lastAt = 0
        return { type: 'drop', dx: dx, dy: dy }
      }

      resetTouch()

      if (pending && t - lastAt <= DBL) {
        pending = false
        lastAt = 0
        return { type: 'dbl', dx: dx, dy: dy }
      }

      pending = true
      lastAt = t
      return { type: 'pending', dx: dx, dy: dy }
    },

    /** 超时后调：确认这是一个单击，还是已经被后面的操作顶掉了 */
    flush: function (now) {
      const t = now == null ? Date.now() : now
      if (!pending) return null
      if (t - lastAt < DBL) return null // 还没到窗口，别急着确认
      pending = false
      lastAt = 0
      return 'click'
    },

    /** 触摸被系统打断（来电、切后台、弹层抢走）：什么都不产生 */
    cancel: function () {
      resetTouch()
      pending = false
      lastAt = 0
    },

    isDragging: function () {
      return dragging
    },

    isPending: function () {
      return pending
    },

    /** 测试用：看内部还剩什么，别在生产里调 */
    _state: function () {
      return { dragging: dragging, moved: moved, pending: pending, lastAt: lastAt }
    },
  }
}

module.exports = {
  MOVE_THRESHOLD: MOVE_THRESHOLD,
  DOUBLE_WINDOW: DOUBLE_WINDOW,
  createGesture: createGesture,
}
