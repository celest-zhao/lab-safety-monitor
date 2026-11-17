/**
 * 华为云 AK/SK 签名（SDK-HMAC-SHA256）
 *
 * 依据：@huaweicloud/huaweicloud-sdk-core 的 auth/AKSKSigner.js 逐行对齐实现，
 * 只保留本项目需要的部分，并且不引入 moment / lodash 等三方依赖
 * （云函数体积小、冷启动快、无需在云端安装依赖）。
 *
 * 算法要点（顺序不能改）：
 *   1. X-Sdk-Date = UTC 的 YYYYMMDDTHHmmss + "Z"
 *   2. host 必须参与签名
 *   3. CanonicalURI 每段单独 URI 编码，且必须以 / 结尾（不以 / 结尾要补）
 *   4. CanonicalQueryString 按 key 升序，key 与 value 都编码
 *   5. CanonicalHeaders 按 header 名小写升序，格式 "name:value\n"
 *   6. SignedHeaders = 小写 header 名按小写升序，用 ; 连接
 *   7. payload 为空时用空串的 sha256 常量
 *   8. stringToSign = "SDK-HMAC-SHA256" \n xSdkDate \n hex(sha256(canonicalRequest))
 *   9. signature = hex(HMAC-SHA256(SK, stringToSign))
 */
const crypto = require('crypto')
const nodeUrl = require('url')

const SDK_SIGNING_ALGORITHM = 'SDK-HMAC-SHA256'
// 标准版/企业版 IoTDA 使用 V11-HMAC-SHA256 + HKDF 派生密钥
const DERIVED_SIGNING_ALGORITHM = 'V11-HMAC-SHA256'
const EMPTY_BODY_SHA256 =
  'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'

function sha256Hex(str) {
  return crypto.createHash('sha256').update(str, 'utf8').digest('hex')
}

function hmacSha256Hex(secretKey, str) {
  return crypto.createHmac('sha256', secretKey).update(str, 'utf8').digest('hex')
}

/**
 * 华为云派生密钥（HKDF-SHA256）。
 * 与官方 @huaweicloud/huaweicloud-sdk-core 的 HKDF.getDerKeySHA256 对齐：
 *   prk = HMAC-SHA256(key=ak, data=sk)
 *   okm = HMAC-SHA256(key=prk, data=info || 0x01)
 * 返回小写十六进制字符串（官方签名时把它当作 UTF-8 字符串作为 HMAC 的 key）。
 */
function hkdfDeriveKey(ak, sk, info) {
  const prk = crypto
    .createHmac('sha256', Buffer.from(String(ak), 'utf8'))
    .update(Buffer.from(String(sk), 'utf8'))
    .digest()
  const data = Buffer.concat([Buffer.from(String(info), 'utf8'), Buffer.from([0x01])])
  return crypto.createHmac('sha256', prk).update(data).digest('hex').toLowerCase()
}

/**
 * 按华为云规则做 URI 编码：只保留 A-Z a-z 0-9 - . _ ~ 不转义。
 * encodeURIComponent 会保留 ! * ' ( )，这几个华为云要转义，所以补一次替换。
 */
function urlEncode(value) {
  const s = typeof value === 'string' ? value : String(value)
  return encodeURIComponent(s).replace(/[!*'()]/g, function (c) {
    return '%' + c.charCodeAt(0).toString(16).toUpperCase()
  })
}

/** UTC 时间戳，格式 YYYYMMDDTHHmmssZ */
function sdkDate(date) {
  const d = date || new Date()
  const p = function (n) {
    return n < 10 ? '0' + n : '' + n
  }
  return (
    d.getUTCFullYear() +
    p(d.getUTCMonth() + 1) +
    p(d.getUTCDate()) +
    'T' +
    p(d.getUTCHours()) +
    p(d.getUTCMinutes()) +
    p(d.getUTCSeconds()) +
    'Z'
  )
}

function canonicalURI(pathname) {
  if (!pathname) return '/'
  const parts = pathname.split('/')
  let out = ''
  for (let i = 0; i < parts.length; i++) {
    out += (i === 0 ? '' : '/') + urlEncode(parts[i])
  }
  // 华为云要求 CanonicalURI 以 / 结尾
  if (out.charAt(out.length - 1) !== '/') out += '/'
  return out
}

function canonicalQueryString(query) {
  if (!query) return ''
  const keys = Object.keys(query).sort()
  const pairs = []
  for (let i = 0; i < keys.length; i++) {
    const k = keys[i]
    const v = query[k]
    if (Array.isArray(v)) {
      const arr = v.slice().sort()
      for (let j = 0; j < arr.length; j++) pairs.push(urlEncode(k) + '=' + urlEncode(arr[j]))
    } else if (v !== undefined && v !== null) {
      pairs.push(urlEncode(k) + '=' + urlEncode(v))
    }
  }
  return pairs.join('&')
}

/**
 * 生成签名头
 * @param {Object} opts
 * @param {string} opts.method  HTTP 方法（GET/PUT/POST/DELETE）
 * @param {string} opts.host    不带协议的域名，如 iotda.cn-north-4.myhuaweicloud.com
 * @param {string} opts.path    路径，如 /v5/iot/{project_id}/devices/{device_id}/shadow
 * @param {Object} [opts.query] 查询参数对象
 * @param {Object|string} [opts.body] 请求体；字符串会原样发送，对象会 JSON.stringify
 * @param {string} opts.ak
 * @param {string} opts.sk
 * @returns {{headers: Object, payload: string, fullPath: string}}
 */
function signRequest(opts) {
  const method = String(opts.method || 'GET').toUpperCase()
  const host = opts.host
  const path = opts.path
  const query = opts.query

  let payload = ''
  if (opts.body !== undefined && opts.body !== null) {
    payload = typeof opts.body === 'string' ? opts.body : JSON.stringify(opts.body)
  }

  const xSdkDate = sdkDate(new Date())

  // 与官方 SDK 一致：先拼出完整 URL 再解析出 pathname 参与签名
  // （官方是 url.parse(endpoint) 后取 pathname，遇到需要转义的字符时行为以此为准）
  const parsedPath = nodeUrl.parse('https://' + host + path).pathname

  // 参与签名的 header（全部小写键）。注意：签名用的 header 必须与实际发送的完全一致。
  // 标准版/企业版若传了 Instance-Id 等额外 header，也必须一起参与签名。
  const signedHeaderMap = {}
  if (opts.headers) {
    Object.keys(opts.headers).forEach(function (k) {
      signedHeaderMap[String(k).toLowerCase()] = opts.headers[k]
    })
  }
  signedHeaderMap.host = host
  signedHeaderMap['x-sdk-date'] = xSdkDate
  if (payload) {
    signedHeaderMap['content-type'] = 'application/json;charset=UTF-8'
  }

  const names = Object.keys(signedHeaderMap).sort()

  let canonicalHeaders = ''
  for (let i = 0; i < names.length; i++) {
    canonicalHeaders += names[i] + ':' + signedHeaderMap[names[i]] + '\n'
  }
  const signedHeaders = names.join(';')

  const payloadHash = payload ? sha256Hex(payload) : EMPTY_BODY_SHA256

  const canonicalRequest = [
    method,
    canonicalURI(parsedPath),
    canonicalQueryString(query),
    canonicalHeaders,
    signedHeaders,
    payloadHash,
  ].join('\n')

  // 实际发送的 header：大小写不影响 HTTP，但值必须与签名时完全一致
  const headers = {}
  for (let i = 0; i < names.length; i++) {
    headers[names[i]] = signedHeaderMap[names[i]]
  }

  if (opts.derived) {
    // 标准版/企业版：V11-HMAC-SHA256 + 派生密钥
    const region = opts.region || 'cn-north-4'
    const serviceName = opts.serviceName || 'iotdm'
    const info = xSdkDate.slice(0, 8) + '/' + region + '/' + serviceName
    const stringToSign = [
      DERIVED_SIGNING_ALGORITHM,
      xSdkDate,
      info,
      sha256Hex(canonicalRequest),
    ].join('\n')
    const signature = hmacSha256Hex(hkdfDeriveKey(opts.ak, opts.sk, info), stringToSign)
    headers.Authorization =
      DERIVED_SIGNING_ALGORITHM +
      ' Credential=' +
      opts.ak +
      '/' +
      info +
      ', SignedHeaders=' +
      signedHeaders +
      ', Signature=' +
      signature
  } else {
    // 基础版：普通 AK/SK 签名
    const stringToSign = [SDK_SIGNING_ALGORITHM, xSdkDate, sha256Hex(canonicalRequest)].join('\n')
    const signature = hmacSha256Hex(opts.sk, stringToSign)
    headers.Authorization =
      SDK_SIGNING_ALGORITHM +
      ' Access=' +
      opts.ak +
      ', SignedHeaders=' +
      signedHeaders +
      ', Signature=' +
      signature
  }

  const qs = canonicalQueryString(query)
  const fullPath = qs ? path + '?' + qs : path

  return { headers: headers, payload: payload, fullPath: fullPath }
}

module.exports = {
  signRequest: signRequest,
  urlEncode: urlEncode,
  sdkDate: sdkDate,
  sha256Hex: sha256Hex,
  hkdfDeriveKey: hkdfDeriveKey,
  EMPTY_BODY_SHA256: EMPTY_BODY_SHA256,
}
