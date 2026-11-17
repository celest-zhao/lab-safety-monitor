/**
 * 云函数配置模板
 *
 * 用法：
 *   cp cloudfunctions/iotApi/config.example.js cloudfunctions/iotApi/config.js
 *   然后把下面所有 <<...>> 换成自己的值。
 *
 * 为什么真实配置要单独一个文件：
 *   本文件可以提交到仓库；config.js 含 AK/SK，不能提交（见根目录 .gitignore）。
 *   config.js 只在云函数里执行，不会下发到小程序前端。
 *
 * 改完必须重新部署 iotApi 才生效。
 */

module.exports = {
  iotda: {
    // ① 华为云 IAM 访问密钥（控制台右上角「我的凭证」→「访问密钥」）
    ak: '<<YOUR_AK>>',
    sk: '<<YOUR_SK>>',

    // ② 项目 ID（同一页面「项目列表」，32 位十六进制）
    projectId: '<<YOUR_PROJECT_ID>>',

    // ③ 接入地址：只写域名，不要带 https:// 和结尾斜杠
    //    基础版: iotda.<region>.myhuaweicloud.com
    //    标准版/企业版: 用控制台「总览 → 平台接入地址 → 应用侧」的实例专用域名
    host: '<<YOUR_IOTDA_HOST>>',
    region: 'cn-north-4',

    // ④ 标准版/企业版实例 ID 与派生签名参数；基础版请把 derivedAuth 改成 false
    instanceId: '<<YOUR_INSTANCE_ID>>',
    derivedAuth: true,
    derivedAuthServiceName: 'iotdm',

    // ⑤ 物模型服务 ID
    serviceId: 'stm32',

    // ⑥ 要展示的设备 ID。留空 [] = 自动拉取该实例下所有设备
    deviceIds: [],
    // 设备显示名（可选）：deviceId → 你想看到的名字
    deviceNames: {},

    // ⑦ 控制通道：'command' 优先下发命令（本项目实测可用）/ 'shadow' 写设备影子
    controlChannel: 'command',
    // 字段名 → 物模型里的命令名。不配则直接用字段名
    commandNameByField: { Fill: 'Fill', Fan: 'Fan' },
    // 命令参数类型：'boolean' → true/false，'number' → 1/0，'string' → 'open'/'close'
    commandParamType: 'boolean',
    // 仅当物模型没有同名命令时才需要改它
    fallbackCommandName: 'SET',
  },

  auth: {
    // 'open'      = 不校验，任何微信用户都能看（首次接入、调试期用）
    // 'whitelist' = 只允许 whitelist 里的 openid
    //
    // 自己（以及同队的人）的 openid：小程序「我的」页点一下即可复制。
    // 注意：切 whitelist 之前必须确认 iotApi 是「login 排在鉴权之前」的那一版，
    //      否则名单外的人连自己的 openid 都拿不到（死锁，见 handover §5.23）。
    mode: 'open',
    whitelist: [
      // 'oXXXXXXXXXXXXXXXXXXXXXXXXXXX',
    ],
  },

  sync: {
    // 库里数据超过这么多秒没更新，就先拉一次 IoTDA
    staleSeconds: 30,
    // 趋势查询最多返回多少个点
    historyLimit: 60,
    // device_history 每个设备保留的条数上限
    historyKeep: 500,
  },

  ai: {
    // 大模型润色：默认关闭，开启按 token 计费，需使用者明确同意
    enabled: false,
    apiKey: '',
    model: 'deepseek-chat',
    endpointHost: 'api.deepseek.com',
  },

  mock: {
    // 示例数据总开关。线上请保持 false，否则首页会出现假设备
    enabled: false,
    // 真实接口失败时是否降级到示例数据。保持 false = 直接显示失败原因
    fallbackOnError: false,
  },
}
