/**
 * Luster 的寒暄台词（纯函数，不碰 wx）
 *
 * 这一份只管「它自己冒出来的那几句话」：纯本地常量表 + 定时器触发，
 * 不读磁盘、不联网、不碰设备数据 —— 所以永远不会卡、永远不会因为断网哑掉。
 * 数据总结在 luster-brief.js，两份分开，改话术不用碰数据逻辑。
 *
 * 时段规则（用户定的，不要自己改）：
 *   8:00–11:59   上午
 *   12:00–17:59  下午
 *   其余         晚上
 * 但 0–5 点开口说「晚上好」很怪（凌晨三点谁会问好），所以晚上再细分一档
 * LATE，只换词、不改归类：归类还是 evening 系，问候语变成「这么晚还没睡」。
 */

const MORNING = 'morning'
const AFTERNOON = 'afternoon'
const EVENING = 'evening'
const LATE = 'late'

/** 按小时判时段。入参是 0-23 的整数（丢进 Date 也行，会自动取小时） */
function slotOf(hour) {
  let h = hour
  if (h instanceof Date) h = h.getHours()
  h = Math.floor(Number(h) || 0) % 24
  if (h < 0) h += 24
  if (h >= 8 && h < 12) return MORNING
  if (h >= 12 && h < 18) return AFTERNOON
  if (h >= 0 && h < 5) return LATE
  return EVENING
}

const GREETINGS = {}
GREETINGS[MORNING] = '早上好呀'
GREETINGS[AFTERNOON] = '下午好'
GREETINGS[EVENING] = '晚上好'
GREETINGS[LATE] = '这么晚还没睡？'

/** 用户给的 4 条固定话术，顺序不要动（看惯性，改顺序会让人觉得「换了个人」） */
const SMALL_TALK = ['你好', '吃饭了吗', '好久不见呀', '我好无聊😴']

/** 第一次出现时说一次，让人知道点它能干嘛；之后不再重复 */
const FIRST_LINE = '我是 Luster，点我一下，我把现在的数据讲给你听'

/** 睡着被点醒时先嘟囔一句，再讲数据 —— 少一次「点两下」的步骤 */
const WAKE_LINE = '唔…我睡着了'

/**
 * 当前可用的话术池：时段问候 + 4 条固定。
 * 每次调用现算，跨过 12 点会自然换词。
 */
function linesOf(date) {
  const s = slotOf(date || new Date())
  return [{ key: 'greet:' + s, text: GREETINGS[s] }].concat(
    SMALL_TALK.map(function (t, i) {
      return { key: 'talk:' + i, text: t }
    })
  )
}

/**
 * 随机挑一句，尽量不和上一句重复。
 * 池子只有 5 条，连续两次撞同一个会很假，所以拒绝采样 8 次；
 * 8 次都撞上（概率 5^-8，实际不会发生）就认了，避免死循环。
 */
function pick(date, lastKey) {
  const list = linesOf(date)
  let item = list[0]
  for (let i = 0; i < 8; i++) {
    item = list[Math.floor(Math.random() * list.length)]
    if (item.key !== lastKey) break
  }
  return item
}

/** 打过招呼没有 —— 用来决定要不要说 FIRST_LINE */
function isFirstMeet(storage) {
  return !storage || !storage.seen
}

module.exports = {
  MORNING: MORNING,
  AFTERNOON: AFTERNOON,
  EVENING: EVENING,
  LATE: LATE,
  GREETINGS: GREETINGS,
  SMALL_TALK: SMALL_TALK,
  FIRST_LINE: FIRST_LINE,
  WAKE_LINE: WAKE_LINE,
  slotOf: slotOf,
  linesOf: linesOf,
  pick: pick,
  isFirstMeet: isFirstMeet,
}
