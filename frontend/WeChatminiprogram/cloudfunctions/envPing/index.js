// envPing：只读诊断函数，用于 EnvId 硬门禁验证
// 只读取集合列表，不写入任何数据
const cloud = require('wx-server-sdk')

cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV })

exports.main = async (event = {}) => {
  const res = {
    ok: true,
    action: 'envPing',
    ts: Date.now(),
  }

  try {
    const db = cloud.database()
    const r = await db.collection('device_latest').limit(1).get()
    res.dbOk = true
    res.sample = r.data || []
  } catch (e) {
    // 读取失败也返回 ok:true —— 只要本函数能被成功调用，
    // 就说明前端 wx.cloud.init 的 env 配置有效
    res.dbOk = false
    res.dbErr = String((e && e.message) || e)
    res.dbErrCode = e && e.errCode
  }

  return res
}
