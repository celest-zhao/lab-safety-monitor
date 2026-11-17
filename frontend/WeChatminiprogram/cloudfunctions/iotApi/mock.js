/**
 * 示例数据（当 config.iotda.ak/sk/projectId 未填时使用）
 *
 * 用途：让界面在没接入 IoTDA 之前就能跑通、联调交互。
 * 原则：返回结果里 source = 'mock'，前端必须明确标注「示例数据」，
 *      不能把示例值当成真实读数展示给用户。
 *
 * 属性名与取值范围严格按 docs/miniapp/design-system.md 第 10 节，不臆造字段。
 */

const SERVICE_ID = 'stm32'

// 基准值取自用户提供的 IoTDA 控制台真实上报截图
const BASE = {
  SHT30_T: 27.1,
  SHT30_H: 48.9,
  Gas: 'Safe',
  Body_IR: false,
  Light: 235,
  Sound: false,
  Fill: false,
  Fan: false,
  AI_Score: 39,
  Temp_Rise: 0,
  Fuse_Level: 1,
  Fuse_Score: 0,
  Dominant: 'Temp/Humi',
}

const DEVICES = [
  {
    deviceId: 'mock-device-01',
    name: '示例设备 · 客厅',
    status: 'ONLINE',
  },
  {
    deviceId: 'mock-device-02',
    name: '示例设备 · 卧室（离线）',
    status: 'OFFLINE',
  },
]

/** 生成一个带轻微漂移的示例属性集 */
function makeProps(deviceId) {
  const seed = deviceId === 'mock-device-02' ? 1.7 : 1
  const t = Date.now() / 60000
  const wave = Math.sin(t / 7) * 0.6
  const props = Object.assign({}, BASE)
  props.SHT30_T = Math.round((BASE.SHT30_T + wave) * seed * 10) / 10
  props.SHT30_H = Math.round((BASE.SHT30_H + wave * 1.5) * 10) / 10
  props.Light = Math.round(BASE.Light + Math.sin(t / 5) * 12)
  props.AI_Score = Math.round(BASE.AI_Score + Math.max(0, wave) * 8)
  return props
}

function listDevices() {
  return {
    devices: DEVICES.map(function (d) {
      return { device_id: d.deviceId, name: d.name, status: d.status }
    }),
  }
}

function getShadow(deviceId) {
  const props = makeProps(deviceId)
  const now = new Date().toISOString()
  return {
    device_id: deviceId,
    shadow: [
      {
        service_id: SERVICE_ID,
        desired: { properties: { Fill: props.Fill, Fan: props.Fan }, version: 1, event_time: now },
        reported: { properties: props, version: Math.floor(Date.now() / 1000), event_time: now },
      },
    ],
  }
}

module.exports = {
  SERVICE_ID: SERVICE_ID,
  listDevices: listDevices,
  getShadow: getShadow,
  makeProps: makeProps,
}
