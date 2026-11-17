/**
 * llm.js —— 大模型调用层（云函数与本地探测脚本共用）
 *
 * 为什么单独拆一个文件：
 *   1. 云函数里 `index.js` 需要 wx-server-sdk，本地脚本 require 不了；把纯 HTTPS 调用
 *      抽出来，`tools/probe_llm.js` 就能用**同一份代码**先在本机验证 key/base/model，
 *      不用先部署再去线上猜是哪个参数错了。
 *   2. 供应商差异（接入地址、模型名、是否要关思考）集中在一张表里，换家只改一个变量。
 *
 * 环境变量（云开发控制台 → 云函数 aiApi → 配置 → 环境变量）：
 *   LLM_KEY        必填，API Key
 *   LLM_PROVIDER   选填，dashscope（默认，阿里云百炼）| deepseek（官方）
 *   LLM_BASE_URL   选填，完整接入地址，带不带 /chat/completions 都行。
 *                  百炼新版接入点带 WorkspaceId，务必从控制台复制：
 *                  https://<WorkspaceId>.cn-beijing.maas.aliyuncs.com/compatible-mode/v1
 *   LLM_MODEL      选填，默认按供应商取
 *   LLM_TIMEOUT    选填，单次请求毫秒数，默认 12000
 *   LLM_THINKING   选填，on / off。百炼 DeepSeek v4 默认开思考，短文案关掉更快更省
 */
const https = require('https')

/* ------------------------------- 供应商表 ------------------------------- */

const PROVIDERS = {
  dashscope: {
    label: '阿里云百炼',
    base: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    model: 'deepseek-v4.1-flash',
    // 百炼 DeepSeek v4 系列默认开思考，思维链按输出 token 计费，还会把响应拖到十几秒。
    // 我们只要一段 100 字人话，关掉；需要推理时用 LLM_THINKING=on 打开。
    thinking: false,
    sendThinking: true,
  },
  deepseek: {
    label: 'DeepSeek 官方',
    base: 'https://api.deepseek.com/v1',
    model: 'deepseek-chat',
    thinking: false,
    // 官方接口不认 enable_thinking，乱传会 400
    sendThinking: false,
  },
}

const DEFAULT_TIMEOUT = 12000
const DEFAULT_MAX_TOKENS = 320

/* ------------------------------ 地址解析 ------------------------------ */

/**
 * 把用户从控制台复制的 base_url 拆成 host + path。
 * 不用 url.parse：云函数运行时版本不固定，这里手写更稳，也顺便容忍漏写协议头。
 */
function parseBase(base) {
  let s = String(base || '').trim().replace(/\s+/g, '')
  if (!s) throw new Error('接入地址为空')

  let protocol = 'https:'
  const m = s.match(/^(https?):\/\//i)
  if (m) {
    protocol = m[1].toLowerCase() + ':'
    s = s.slice(m[0].length)
  }

  s = s.replace(/^\/+/, '')
  const slash = s.indexOf('/')
  const host = slash >= 0 ? s.slice(0, slash) : s
  let path = slash >= 0 ? s.slice(slash) : ''
  path = path.replace(/\/+$/, '')

  if (!host) throw new Error('接入地址里没有主机名：' + base)
  return { protocol: protocol, host: host, path: path }
}

/** 拼出最终的 chat completions 路径，容忍用户直接贴了完整地址 */
function joinChatPath(base) {
  const p = parseBase(base)
  let full
  if (/\/chat\/completions$/i.test(p.path)) {
    full = p.path
  } else if (!p.path) {
    full = '/v1/chat/completions'
  } else {
    full = p.path + '/chat/completions'
  }
  return { protocol: p.protocol, host: p.host, path: full }
}

/* ------------------------------- 配置解析 ------------------------------- */

/**
 * 纯函数：给一份 env，算出这次要用什么。
 * 单独导出是为了本地测试 —— 不联网也能断言「换 base 会不会拼错路径」。
 */
function resolveLLM(env) {
  const e = env || {}

  const wanted = String(e.LLM_PROVIDER || 'dashscope').trim().toLowerCase()
  const provider = PROVIDERS[wanted] ? wanted : 'dashscope'
  const preset = PROVIDERS[provider]

  // 兼容老写法：只给了 LLM_HOST 时自己补协议与路径前缀。
  // 注意 LLM_HOST 里可能已经带了 /v1，再补一次就变成 /v1/v1。
  let base = String(e.LLM_BASE_URL || '').trim()
  if (!base && e.LLM_HOST) {
    const raw = String(e.LLM_HOST).trim()
    const body = raw.replace(/^https?:\/\//i, '')
    const withProto = /^https?:\/\//i.test(raw) ? raw : 'https://' + body
    const prefix = e.LLM_PATH ? String(e.LLM_PATH).trim().replace(/\/+$/, '') : '/v1'
    base = body.indexOf('/') >= 0 ? withProto : withProto + prefix
  }
  if (!base) base = preset.base

  const target = joinChatPath(base)

  const key = String(e.LLM_KEY || e.DASHSCOPE_API_KEY || e.DEEPSEEK_KEY || '').trim()
  const timeout = Number(e.LLM_TIMEOUT) > 0 ? Number(e.LLM_TIMEOUT) : DEFAULT_TIMEOUT

  // 思考开关：默认关。off 时显式传 enable_thinking=false，
  // 不然百炼 v4 会默认进思考模式，200 字要等十几秒。
  const thinkRaw = String(e.LLM_THINKING || '').trim().toLowerCase()
  const thinking = thinkRaw === 'on' || thinkRaw === 'true' || thinkRaw === '1'

  return {
    provider: provider,
    label: preset.label,
    base: base,
    protocol: target.protocol,
    host: target.host,
    path: target.path,
    model: String(e.LLM_MODEL || '').trim() || preset.model,
    key: key,
    configured: !!key,
    timeout: timeout,
    maxTokens: DEFAULT_MAX_TOKENS,
    thinking: thinking,
    sendThinking: preset.sendThinking,
  }
}

/** 日志里只出现打码后的 key，永远别把整串写进控制台 */
function maskKey(key) {
  const s = String(key || '')
  if (!s) return ''
  if (s.length <= 8) return '****'
  return s.slice(0, 3) + '****' + s.slice(-4)
}

/**
 * 从文本里挑出 key。
 *
 * 宽容到什么程度：行首的 # 会先剥掉 —— 用户很容易把 key 粘在某句注释的末尾
 * （实测就发生过：文件第 3 行本来是单独一个 #，粘贴后成了 "#sk-ws-..."，
 * 整行变成注释，key 就"消失"了）。剥掉 # 之后只要长得像 key 就认。
 * 同时容忍 BOM、CRLF、粘贴带进来的前后空格。
 *
 * 判断"像 key"：以 sk- 开头，后面只有字母数字和 . _ - 。
 * 说明文字里的「key（sk- 开头…）」不会命中，因为 sk- 后面跟的是空格。
 */
const KEY_RE = /sk-[A-Za-z0-9._-]{16,}/

function pickKey(text) {
  const lines = String(text || '')
    .replace(/^\uFEFF/, '')
    .split(/\r?\n/)

  for (let i = 0; i < lines.length; i++) {
    let s = lines[i].trim()
    if (!s) continue
    if (s.charAt(0) === '#') s = s.replace(/^#+\s*/, '').trim()
    if (!s) continue
    const m = s.match(KEY_RE)
    if (m) return m[0]
  }
  return ''
}

/** 读 key 文件；文件不存在就返回空串，不抛错（探测脚本据此给友好提示） */
function readKeyFile(filePath) {
  try {
    return pickKey(require('fs').readFileSync(filePath, 'utf8'))
  } catch (e) {
    return ''
  }
}
function describe(llm) {
  return [
    llm.label + '(' + llm.provider + ')',
    llm.host + llm.path,
    'model=' + llm.model,
    'thinking=' + (llm.thinking ? 'on' : 'off'),
    'timeout=' + llm.timeout + 'ms',
    'key=' + (llm.key ? maskKey(llm.key) : '未配置'),
  ].join(' · ')
}

/* -------------------------------- 调用 -------------------------------- */

function request(llm, body) {
  return new Promise(function (resolve, reject) {
    const started = Date.now()
    const payload = JSON.stringify(body)

    const req = https.request(
      {
        protocol: llm.protocol,
        hostname: llm.host,
        path: llm.path,
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(payload),
          Authorization: 'Bearer ' + llm.key,
        },
        timeout: llm.timeout,
      },
      function (res) {
        let buf = ''
        res.setEncoding('utf8')
        res.on('data', function (c) {
          buf += c
        })
        res.on('end', function () {
          const ms = Date.now() - started
          let j = null
          try {
            j = JSON.parse(buf)
          } catch (e) {
            return reject(
              new Error('模型返回的不是 JSON（HTTP ' + res.statusCode + '）：' + buf.slice(0, 120))
            )
          }

          if (j.error) {
            const em = (j.error && (j.error.message || j.error.code)) || '模型调用失败'
            return reject(new Error('HTTP ' + res.statusCode + ' ' + em))
          }
          if (res.statusCode && res.statusCode >= 400) {
            return reject(new Error('HTTP ' + res.statusCode + ' ' + buf.slice(0, 120)))
          }

          const choice = (j.choices || [])[0]
          if (!choice) return reject(new Error('模型没有返回内容（HTTP ' + res.statusCode + '）'))

          const msg = choice.message || {}
          const text = String(msg.content || '').trim()
          if (!text) {
            // 只开了思考没给正文时的典型症状：多半是 enable_thinking 没关或 max_tokens 被思维链吃光
            if (msg.reasoning_content) {
              return reject(new Error('模型只返回了思考过程，正文是空的（把 LLM_THINKING 设为 off 或调大 max_tokens）'))
            }
            return reject(new Error('模型返回了空内容（finish_reason=' + (choice.finish_reason || '?') + '）'))
          }

          resolve({
            text: text,
            usage: j.usage || null,
            ms: ms,
            finish: choice.finish_reason || '',
            requestId: j.id || '',
          })
        })
      }
    )

    req.on('timeout', function () {
      req.destroy(new Error('模型超时（>' + llm.timeout + 'ms），换个快点的模型或调大 LLM_TIMEOUT'))
    })
    req.on('error', function (e) {
      if (e && e.code === 'ENOTFOUND') {
        return reject(new Error('域名解析失败：' + llm.host + '，检查 LLM_BASE_URL'))
      }
      reject(e)
    })
    req.write(payload)
    req.end()
  })
}

/**
 * 组装请求体。单独拆出来是为了能离线断言：
 * 百炼要带 enable_thinking=false、官方接口绝不能带（会 400）。
 */
function buildBody(llm, messages, opts) {
  const o = opts || {}
  const body = {
    model: llm.model,
    messages: messages,
    temperature: typeof o.temperature === 'number' ? o.temperature : 0.3,
    max_tokens: o.maxTokens || llm.maxTokens,
    stream: false,
  }
  if (llm.sendThinking) body.enable_thinking = !!llm.thinking
  return body
}

/**
 * 发一次对话。
 * @param {Array} messages [{role, content}]
 * @param {Object} opts { llm, maxTokens, temperature }
 */
function chat(messages, opts) {
  const o = opts || {}
  const llm = o.llm
  if (!llm) return Promise.reject(new Error('缺少 llm 配置'))
  if (!llm.key) return Promise.reject(new Error('没有配置 LLM_KEY'))

  return request(llm, buildBody(llm, messages, o))
}

module.exports = {
  PROVIDERS: PROVIDERS,
  resolveLLM: resolveLLM,
  parseBase: parseBase,
  joinChatPath: joinChatPath,
  maskKey: maskKey,
  pickKey: pickKey,
  readKeyFile: readKeyFile,
  describe: describe,
  buildBody: buildBody,
  chat: chat,
}
