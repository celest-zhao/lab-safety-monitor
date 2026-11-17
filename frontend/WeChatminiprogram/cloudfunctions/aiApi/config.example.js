/**
 * aiApi 本地配置模板。
 *
 * 用法：复制本文件为 config.js，按需填值。config.js 已 gitignore，不会被提交。
 * 优先级（LLM 三项）：环境变量 > config.js > 代码默认值。
 * 例外：**freeChat 一旦写了 mode，就以本文件为准**，环境变量不再覆盖
 *      （控制台那个看不见的 FREE_CHAT_OPENIDS 曾静默盖掉文件里已改对的配置，
 *        导致「清空 openids」完全无效，health 还一直报「开关是 on」）。
 */
module.exports = {
  // 大模型 key（阿里云百炼 / DeepSeek，sk- 开头）。留空则走规则答复，不花钱。
  llmKey: '',

  // 以下三项一般不用填，默认百炼 + deepseek-v4.1-flash
  llmBaseUrl: '',   // 例：https://dashscope.aliyuncs.com/compatible-mode/v1
  llmModel: '',     // 例：deepseek-v4.1-flash
  llmProvider: '',  // dashscope | deepseek

  freeChat: {
    mode: 'off',    // 'on' 打开自由输入；默认 'off'
    openids: [],    // 只让这些人能打字；留空 = 白名单内所有人
  },

  // 说明：上面 freeChat.openids 同时也是**问答白名单**（能打字的人当然能问预设问题）。
  // 聊天通道的判定顺序：这份名单 → users 集合里的 allowed:true →
  // 库里一条 allowed:true 都没有时放行（视为「白名单还没配」，和 iotApi 的 open 默认一致）。
  // 想让云端转严格模式：在控制台给任意一个人勾上 allowed，或在 openids 里写死名单。
}
