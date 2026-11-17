/**
 * 小人落点计算（纯函数，不碰 wx）
 *
 * 为什么单独拎出来：位置算错在真机上只是"坐歪了一点"，肉眼很难判断对错，
 * 只能靠断言。放成纯函数就能用 node 直接跑（tools/test_buddy_geo.js），
 * 预览生成器也复用同一份，改公式不用改两处。
 *
 * 坐标约定：
 *   home  = 小人"站在右下角原位"时盒子的 rect（left/top，屏幕坐标）
 *   pos   = { dx, dy } 相对原位的位移，直接喂给 transform: translate3d
 *   win   = { w, h } 页面可视区（windowWidth / windowHeight）
 */

const BOX = 96 // 盒子边长（px，和 wxss 的 .bfab 一致）
const FEET_RATIO = 0.85 // 脚底在画布里的位置：源图脚底 y=435 / 512
const FEET_PX = BOX * FEET_RATIO // 81.6
const EDGE_PAD = 10 // 晃悠时离屏幕边至少留这么多
const SINK = 2 // 落点再往下 2px，看着像踩上去，而不是浮在线上头
const MIN_FEET_TOP = FEET_PX + 10 // 线上方至少要能装下大半个身子

function clamp(v, min, max) {
  return Math.min(Math.max(v, min), max)
}

/**
 * 走路速度（px/s）。
 * 之前是 435（walkTime=dist×2.3 换算出来的），相当于每秒 4.5 个身位 ——
 * 人走路约每秒 0.8 个身位。没有腿部动画时代只是"平移"，看不出来；
 * 一旦有了迈步帧，滑步会立刻变得刺眼。130 大概是"小跑"，视觉上还跟得上腿。
 */
const SPEED = 130
/** 一步迈多远（px）。角色本体高 72px，真人步幅约 0.4 倍身高 ≈ 29px */
const STRIDE = 30

/** 距离 → 用时。太近别磨蹭，太远也别走到天黑 */
function walkTime(dist) {
  return Math.round(clamp((dist / SPEED) * 1000, 320, 3400))
}

/**
 * 每帧停多久（ms）。
 *
 * 关键：帧率必须由速度和步幅推出来，写死就会滑步 ——
 * 一个走路循环 = 左右各一步（2×STRIDE），腿在循环里换 4 次帧位，
 * 所以每帧时长 = 走完一步的时间 ÷ 2。
 * 速度一改，这里自动跟着变，不用手动同步两个数。
 */
function frameMs(speed) {
  const v = speed || SPEED
  return Math.round(clamp(((STRIDE / v) * 1000) / 2, 55, 130))
}

/**
 * 落脚点（每次出行现量现算）。
 *   主卡：上沿上方有地方站就站上沿（身子在卡片外）；上沿贴着屏幕顶（首页就是这样）
 *         就站下沿 —— 身子压在卡片下缘，看着还是"坐在卡片上"。
 *   虚线框：站它的上沿，也就是"坐到下面那条线上"。
 * 站不下（贴着屏幕边、或者线在可视区外）就返回空数组，不硬塞。
 */
function perches(hero, line, home, win) {
  const out = []

  function add(x, feetY, sel) {
    out.push({
      dx: Math.round(x - home.left),
      // 盒子顶 = 脚底位置 - 脚底在盒子里占的高度，不然整个人会往下沉一个身位
      dy: Math.round(feetY + SINK - FEET_PX - home.top),
      perch: true,
      sel: sel,
    })
  }

  if (hero && hero.width && hero.height) {
    // 站主卡中间：卡片下沿这一行左边是 AI 评分、右边是「查看详情」，中间正好是空的
    const midX = hero.left + hero.width / 2 - BOX / 2
    if (hero.top >= MIN_FEET_TOP) {
      add(midX, hero.top, '.hero')
    } else if (hero.bottom >= MIN_FEET_TOP && hero.bottom <= win.h) {
      add(midX, hero.bottom, '.hero')
    }
  }

  if (line && line.width && line.height) {
    if (line.top >= MIN_FEET_TOP && line.top + BOX <= win.h) {
      add(line.left + line.width / 2 - BOX / 2, line.top, '.add-module')
    }
  }

  return out
}

/** 页面里随便找个地方站（尽量别和现在的位置重合） */
function spot(win, home, cur) {
  let dx = 0
  let dy = 0
  for (let i = 0; i < 6; i++) {
    const x = EDGE_PAD + Math.random() * (win.w - BOX - EDGE_PAD * 2)
    const y = EDGE_PAD + Math.random() * (win.h - BOX - EDGE_PAD * 2)
    dx = x - home.left
    dy = y - home.top
    const d = Math.sqrt(Math.pow(dx - (cur ? cur.dx : 0), 2) + Math.pow(dy - (cur ? cur.dy : 0), 2))
    if (d > 70) break
  }
  return { dx: Math.round(dx), dy: Math.round(dy) }
}

/**
 * 拖拽时把位移夹在屏幕内。
 * dx/dy 是相对 home 的位移，所以边界也要用 home 换算 ——
 * 不能直接拿 0 到 win.w 去夹，否则小人会被夹到屏幕外（home 本身就在右下角）。
 */
function clampPos(dx, dy, home, win) {
  const minX = -home.left
  const maxX = win.w - BOX - home.left
  const minY = -home.top
  const maxY = win.h - BOX - home.top
  return {
    dx: Math.round(clamp(dx, minX, Math.max(minX, maxX))),
    dy: Math.round(clamp(dy, minY, Math.max(minY, maxY))),
  }
}

/**
 * 松手时贴着左右边缘就吸过去。
 * 只吸横向：桌宠停靠是"靠边站"，纵向本来就该停在手指放下的地方，
 * 上下也吸会变成"扔到角落"，很别扭。
 */
function snapEdge(p, home, win, snap) {
  const S = snap == null ? 44 : snap
  const leftEdge = -home.left
  const rightEdge = win.w - BOX - home.left
  let dx = p.dx
  if (Math.abs(dx - leftEdge) <= S) dx = leftEdge
  else if (Math.abs(dx - rightEdge) <= S) dx = rightEdge
  return { dx: Math.round(dx), dy: p.dy }
}

/**
 * 寒暄小气泡摆哪（相对小人盒子左边）+ 该多宽。
 *
 * 宽度必须在这里算，不能交给 CSS：'.say' 是绝对定位的，宽度若由 CSS 定死，
 * 三个字（「下午好」）和二十五个字（自我介绍）用同一个宽度，
 * 短的那条会显得空荡荡，而且居中位置也算不准。所以按字数估：
 * 中文 24rpx 字号 ≈ 25rpx 一个字，两侧 padding 共 44rpx，夹在 140–340rpx 之间。
 */
function tip(win, home, pos, textLen) {
  const chars = Math.max(1, Math.floor(Number(textLen) || 1))
  const wRpx = clamp(chars * 25 + 44, 140, 340)
  const w = (wRpx * win.w) / 750
  const half = w / 2
  const absLeft = home.left + (pos ? pos.dx : 0)
  const absTop = home.top + (pos ? pos.dy : 0)
  const cx = absLeft + BOX / 2
  const left = clamp(cx - half, 8, Math.max(8, win.w - 8 - w))
  return {
    x: Math.round(left - absLeft),
    w: Math.round(w),
    below: absTop + BOX / 2 < win.h * 0.46,
  }
}

/**
 * 气泡摆哪（相对小人盒子左上角）。
 * 气泡宽 600rpx = 0.8 倍屏宽，横向夹在屏幕里，箭头再指回小人中心 ——
 * 不然小人站在页面中间时，气泡要么出屏，要么箭头指到别处。
 */
function bubble(home, pos, win) {
  const absLeft = home.left + (pos ? pos.dx : 0)
  const absTop = home.top + (pos ? pos.dy : 0)
  const cx = absLeft + BOX / 2
  const cy = absTop + BOX / 2
  const bw = win.w * 0.8
  const bx = clamp(cx - bw / 2, 8, Math.max(8, win.w - bw - 8))
  const arrow = clamp(cx - bx - 6, 10, bw - 22)
  return {
    bpX: Math.round(bx - absLeft),
    bpArrow: Math.round(arrow),
    bpBelow: cy < win.h * 0.46,
  }
}

module.exports = {
  BOX: BOX,
  FEET_PX: FEET_PX,
  EDGE_PAD: EDGE_PAD,
  SINK: SINK,
  SPEED: SPEED,
  STRIDE: STRIDE,
  clamp: clamp,
  walkTime: walkTime,
  frameMs: frameMs,
  perches: perches,
  spot: spot,
  bubble: bubble,
  clampPos: clampPos,
  snapEdge: snapEdge,
  tip: tip,
}
