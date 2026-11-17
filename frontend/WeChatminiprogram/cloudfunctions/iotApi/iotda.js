/**
 * 华为云 IoTDA 北向接口封装
 * 只依赖 Node 内置 https + 自研 signer，不装华为云 SDK。
 *
 * 用到的 4 个接口：
 *   GET  /v5/iot/{project_id}/devices                    查询设备列表（含在线状态）
 *   GET  /v5/iot/{project_id}/devices/{device_id}/shadow 查询设备影子（属性最新值）
 *   PUT  /v5/iot/{project_id}/devices/{device_id}/shadow 修改设备影子（下发期望值）
 *   POST /v5/iot/{project_id}/devices/{device_id}/commands 下发命令
 */
const https = require('https')
const { signRequest } = require('./signer')

const TIMEOUT_MS = 8000

/**
 * 发一个已签名的 HTTPS 请求
 */
function signedRequest(cfg, opts) {
  return new Promise(function (resolve, reject) {
    // 基础版用普通 AK/SK；标准版/企业版必须用实例专用地址 + V11 派生签名。
    const derived = !!(cfg.derivedAuth || cfg.authType === 'derived')
    const signed = signRequest({
      method: opts.method,
      host: cfg.host,
      path: opts.path,
      query: opts.query,
      body: opts.body,
      ak: cfg.ak,
      sk: cfg.sk,
      // 标准版建议携带 Instance-Id；带上后它会一起参与签名
      headers: cfg.instanceId ? { 'Instance-Id': cfg.instanceId } : undefined,
      derived: derived,
      region: cfg.region || 'cn-north-4',
      serviceName: cfg.derivedAuthServiceName || 'iotdm',
    })

    let settled = false
    const done = function (fn, arg) {
      if (settled) return
      settled = true
      fn(arg)
    }

    const req = https.request(
      {
        host: cfg.host,
        port: 443,
        method: opts.method,
        path: signed.fullPath,
        headers: signed.headers,
        timeout: TIMEOUT_MS,
      },
      function (res) {
        const chunks = []
        res.on('data', function (c) {
          chunks.push(c)
        })
        res.on('end', function () {
          const text = Buffer.concat(chunks).toString('utf8')
          let json = null
          try {
            json = text ? JSON.parse(text) : null
          } catch (e) {
            json = null
          }
          if (res.statusCode >= 200 && res.statusCode < 300) {
            done(resolve, json)
          } else {
            const err = new Error(
              'IoTDA ' +
                opts.path +
                ' 返回 ' +
                res.statusCode +
                ': ' +
                (json ? JSON.stringify(json) : text).slice(0, 500)
            )
            err.statusCode = res.statusCode
            err.body = json || text
            done(reject, err)
          }
        })
      }
    )

    req.on('timeout', function () {
      req.destroy(new Error('IoTDA 请求超时（' + TIMEOUT_MS + 'ms）：' + opts.path))
    })
    req.on('error', function (e) {
      done(reject, e)
    })

    if (signed.payload) req.write(signed.payload)
    req.end()
  })
}

/** 查询设备列表（含 status: ONLINE / OFFLINE / ABNORMAL / INACTIVE） */
function listDevices(cfg) {
  return signedRequest(cfg, {
    method: 'GET',
    path: '/v5/iot/' + cfg.projectId + '/devices',
    // 标准版 IoTDA 对 limit 的限制是 50，超过会报 IOTDA.000006
    query: { limit: 50, offset: 0 },
  })
}

/** 查询单个设备详情（含 status、last_online_time） */
function getDevice(cfg, deviceId) {
  return signedRequest(cfg, {
    method: 'GET',
    path: '/v5/iot/' + cfg.projectId + '/devices/' + deviceId,
  })
}

/** 查询设备影子 */
function getShadow(cfg, deviceId) {
  return signedRequest(cfg, {
    method: 'GET',
    path: '/v5/iot/' + cfg.projectId + '/devices/' + deviceId + '/shadow',
  })
}

/**
 * 修改设备影子（写期望值 / desired）
 * @param {Object} properties 形如 { Fan: true }
 */
function updateShadowDesired(cfg, deviceId, serviceId, properties) {
  return signedRequest(cfg, {
    method: 'PUT',
    path: '/v5/iot/' + cfg.projectId + '/devices/' + deviceId + '/shadow',
    body: {
      shadow: [
        {
          service_id: serviceId,
          desired: { properties: properties },
        },
      ],
    },
  })
}

/**
 * 下发命令（属性设置失败时的回退通道）
 * @param {string} commandName 设备侧定义的命令名
 * @param {Object} paras 命令参数
 */
function createCommand(cfg, deviceId, serviceId, commandName, paras) {
  return signedRequest(cfg, {
    method: 'POST',
    path: '/v5/iot/' + cfg.projectId + '/devices/' + deviceId + '/commands',
    body: {
      service_id: serviceId,
      command_name: commandName,
      paras: paras,
    },
  })
}

/**
 * 把影子响应整理成 { props: {...}, desired: {...}, reportedTime, desiredVersion, reportedVersion }
 * 影子响应结构：
 *   { device_id, shadow: [ { service_id, desired: { properties, version, event_time },
 *                            reported: { properties, version, event_time } } ] }
 */
function parseShadow(shadowResp, serviceId) {
  const out = { props: {}, desired: {}, reportedTime: null, desiredVersion: null, reportedVersion: null }
  const list = (shadowResp && shadowResp.shadow) || []
  for (let i = 0; i < list.length; i++) {
    const s = list[i]
    if (!s) continue
    if (serviceId && s.service_id && s.service_id !== serviceId) continue
    const reported = s.reported || {}
    const desired = s.desired || {}
    out.props = Object.assign(out.props, reported.properties || {})
    out.desired = Object.assign(out.desired, desired.properties || {})
    if (reported.event_time) out.reportedTime = reported.event_time
    // 实测：标准版把 version 放在 service 层（reported 内部没有 version），两种都兼容
    const ver = typeof reported.version === 'number' ? reported.version : s.version
    out.reportedVersion = typeof ver === 'number' ? ver : null
    out.desiredVersion = typeof desired.version === 'number' ? desired.version : null
    break
  }
  return out
}

module.exports = {
  listDevices: listDevices,
  getDevice: getDevice,
  getShadow: getShadow,
  updateShadowDesired: updateShadowDesired,
  createCommand: createCommand,
  parseShadow: parseShadow,
}
