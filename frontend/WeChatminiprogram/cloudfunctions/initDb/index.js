// initDb：初始化数据库集合（幂等，已存在会跳过）
// 说明：云函数用管理权限运行，可以直接创建集合，
// 因此在云开发控制台页面打不开时，用本函数代替手动建集合
const cloud = require('wx-server-sdk')

cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV })

const COLLECTIONS = ['device_latest', 'device_history', 'alarms', 'users']

exports.main = async (event = {}) => {
  const db = cloud.database()
  const results = []

  for (const name of COLLECTIONS) {
    try {
      await db.createCollection(name)
      results.push({ name, ok: true, note: 'created' })
    } catch (e) {
      const msg = String((e && e.message) || e)
      // 集合已存在视为成功（幂等）
      const existed = /already exists|已存在|exist/i.test(msg)
      results.push({
        name,
        ok: existed,
        note: existed ? 'already exists' : 'failed',
        err: existed ? undefined : msg,
      })
    }
  }

  return {
    ok: results.every((r) => r.ok),
    action: 'initDb',
    ts: Date.now(),
    results,
  }
}
