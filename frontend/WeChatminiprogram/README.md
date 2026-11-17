# Luster 值班

![平台](https://img.shields.io/badge/platform-WeChat_MiniProgram-07C160)
![后端](https://img.shields.io/badge/backend-Huawei_Cloud_IoTDA-c7000b)
![状态](https://img.shields.io/badge/status-beta-orange)
![License](https://img.shields.io/badge/license-MIT-blue)

> 实验室的温湿度、可燃气体、风扇与补光，一只名叫 **Luster** 的小人替你盯着。
> 点它一下，它用一句人话告诉你此刻实验室怎么样。

STM32 环境检测节点 → 华为云 IoTDA → 微信云开发 → 小程序。三人共享，用于实验室日常值守。

**Z.Z.J 队** 作品，尚未正式发布，当前以体验版在队内分发。

---

## 功能亮点

- **桌宠即入口**：单击讲数据、双击弹气泡、拖拽停靠、闲置自动休眠——不用记菜单在哪。
- **双击问一句**：四个预设问题（哪台最不对劲 / 温度正常吗 / 我该做什么 / 报警要紧吗），模型只依据当前快照作答；没配 key、断网、超时都退回端上答复，点下去一定有话说。
- **断网也有话说**：`luster-brief.js` 在端上做纯同步汇总，云函数挂了、没网，点它照样能讲出当前读数。
- **双主题**：深色「深空玻璃」为基调，浅色跟随系统；色值全走 CSS 变量，切主题不重启。
- **可配置看板**：首页模块（温度 / 湿度 / AI 评分 / 设备状态…）可增删，配置存本地，不上云。
- **报警可管理**：三级分级（严重 / 警告 / 提示），左滑删单条、按类别一键清空，带二次确认。
- **白名单鉴权**：云函数只认名单内的 openid，名单外的人能进小程序但拿不到任何设备数据。

## 界面

| 首页（浅色） | 首页（深色） |
|---|---|
| ![浅色首页](shots/light-home.png) | ![深色首页](shots/dark-home.png) |

> 上图为设计稿截图（`shots/`），真机截图待补。四个 tab：设备 / 趋势 / 报警 / 我的。

## 适用场景

- 实验室、机房的**环境值守**：谁值班都能看，不用装 App；
- 一个**小团队共享一台设备**（3 人以内最舒服，原因见「数据可见性」）；
- 想给课程 / 竞赛作品加一点**角色感**，而不是又一张仪表盘。

不适合：按人隔离数据、历史数据长期归档、多人并发控制的场景。

---

## 快速开始

### 环境要求

| 依赖 | 版本 | 用途 |
|---|---|---|
| 微信开发者工具 | 稳定版（≥ 1.06） | 编译、真机预览、上传云函数 |
| Node.js | ≥ 18 | 跑测试脚本、诊断工具 |
| Python | 3.9+，含 Pillow | 仅修改桌宠素材时需要 |

### 1. 导入项目

开发者工具 →「导入项目」→ 目录选**本仓库根目录**（含 `project.config.json` 的那一层）→ AppID 填自己的小程序 AppID。

> 测试号能跑通界面，但**云开发必须真实 AppID**。

### 2. 开通云开发并填环境 ID

1. 工具栏「云开发」→ 开通 → 记下环境 ID（形如 `cloud1-xxxxxxxx`）；
2. 填到 `miniprogram/app.js`：

```js
globalData: {
  env: 'cloud1-xxxxxxxx',   // ← 换成你的环境 ID
}
```

### 3. 填云函数配置

**首次搭建**（已有 `config.js` 就跳过，不要覆盖）：

```bash
cp cloudfunctions/iotApi/config.example.js cloudfunctions/iotApi/config.js
```

然后填四项，都在华为云 IoTDA 控制台：

| 字段 | 从哪取 |
|---|---|
| `iotda.ak` / `iotda.sk` | 「我的凭证 → 访问密钥」 |
| `iotda.projectId` | IoTDA 总览 → 项目 ID |
| `iotda.host` | IoTDA 总览 → 平台接入地址 → **应用侧**（不带 `https://`） |
| `iotda.instanceId` | 标准版/企业版实例 ID；基础版请把 `derivedAuth` 改 `false` |

其余留默认。完整字段说明见「配置」。

### 4. 部署云函数

左侧资源管理器，对每个函数右键 →「**上传并部署：云端安装依赖**」：

| 函数 | 必装 | 作用 |
|---|---|---|
| `iotApi` | ✅ | 唯一的数据网关：拉数、下发命令、报警读写 |
| `initDb` | ✅ 首次 | 建集合与索引，**部署后右键「云端测试」跑一次** |
| `aiApi` | 可选 | 让 Luster 的话更自然；不部署会自动降级到端上规则 |
| `envPing` | 可选 | 连通性探针，排查网络问题用 |

部署完 `aiApi` 后，去 **云开发控制台 → 云函数 → aiApi → 配置**，把**超时从 3 秒改成 20 秒**。不改的话接大模型必然超时（部署不会覆盖这个配置项）。

### 5. 接大模型（可选）

不接也能用：`aiApi` 会走规则文案，Luster 照样能聊，一分钱不花。想让它说人话更自然，再加两个环境变量就够：

| 变量 | 必填 | 值 | 说明 |
|---|---|---|---|
| `LLM_KEY` | ✅ | `sk-...` | 阿里云百炼的 API Key |
| `LLM_PROVIDER` | | `dashscope` | 默认就是百炼，可不填 |
| `LLM_BASE_URL` | | 控制台复制的 Base URL | 百炼新版接入点带 WorkspaceId 时填这个 |
| `LLM_MODEL` | | `deepseek-v4.1-flash` | 想更强换 `deepseek-v4-pro` |
| `LLM_THINKING` | | `off` | 默认关。开了会更慢、更贵，聊天不需要 |

填之前**先在本机验一次**，别去线上猜是哪个参数错了：

```bash
# 打开 tools/llm-key.txt，把 key 粘在说明头下面一行，保存后：
node tools/probe_llm.js
```

会打印实际请求的地址、模型、耗时、token 和一段真实回答；401/404/模型不存在会直接告诉你改哪里。验通了再把同一组值填进控制台，然后重传 `aiApi`。

默认组合（`dashscope.aliyuncs.com` + `deepseek-v4.1-flash`）实测 **约 1 秒 / 210 token**，回答 110 字以内、不带 markdown。

> 百炼旧的 `deepseek-v3` / `deepseek-r1` 已于 2026-10-10 下线，填老名字会 400。

**线上验证用 `brief`，别用 `chat`**。控制台的「云端测试」没有用户 openid，`chat` 会拿 `openid=anonymous` 去查 `users` 白名单，必然返回 `FORBIDDEN` —— 看着像 key 错了，其实只是没有身份。在云端测试里填：

```json
{"action":"brief","snapshot":{"devices":[{"name":"客厅","online":true,"props":{"SHT30_T":26.3,"SHT30_H":55,"Light":320,"AI_Score":92}}]}}
```

判据只看返回里的两个字段：

| 返回 | 含义 |
|---|---|
| `source: "model"` | 真的走了大模型，key / 地址 / 模型名全都对 |
| `source: "rule"` + `modelError` 有值 | 模型这条路断了，`modelError` 里写着原因（401 key 错 / 404 地址错 / 模型下线 / 欠费） |

`health` 的 `llm: "ready"` 只说明**key 被解析出来了**，不代表能调通；`freeChat: false` 在控制台测试里也必然是 false（没有 openid）。真机是否给输入框，看「我的」页连接自检那一行。

**问答名单（「账号不在云端名单」是怎么判的）**

`aiApi` 判定谁能问，按顺序命中就停：

1. **配置名单** —— `config.js` 的 `freeChat.openids`（文件里写了就只认文件，`*` = 不限制），文件没写才看环境变量 `FREE_CHAT_OPENIDS`；
2. **数据库** —— `users` 集合里这条 openid 的 `allowed` 是 `true`；
3. **没人被授权就放行** —— `users` 里一条 `allowed: true` 都没有，视为「白名单还没配」，按 `open` 处理；
4. 只有「库里已经有人被授权、这条 openid 不在其中」才返回 `FORBIDDEN`。

第 3 条是为了和 `iotApi` 对齐：`iotApi` 的 `config.auth.mode` 默认 `open`，而它 `touchUser` 写库时固定 `allowed: false`。只认第 2 条的话，必然出现「首页设备数据正常、双击桌宠说账号不在名单」。控制台给任意一个人勾上 `allowed`，这里立刻转严格模式。

排查看 `health` 返回的 `chatAuth`（真机上显示为「我的」页自检里的「问答通道」）：

| chatAuth | 含义 |
|---|---|
| `list` | 配置名单命中 |
| `db` | 数据库里已授权 |
| `open` | 库里没人被授权，放行 |
| `deny` | 有白名单但不含这个 openid |
| `error` | 查库失败，已放行 |
| `no-open-id` | 控制台测试没有 openid，正常现象 |

**「有 AI 回答，但没有输入框」** 是另一个字段：`freeChatWhy`。`freeChat` 只给布尔值，四种原因全长得一样：

| freeChatWhy | 含义 | 改哪儿 |
|---|---|---|
| `ok` | 这台手机可以打字 | —— |
| `mode-off` | 云端 `FREE_CHAT` 是 off | 环境变量或 `config.js` 的 `freeChat.mode` |
| `no-open-id` | 请求没带 openid | 只有控制台测试会这样，真机不会 |
| `not-in-list` | `freeChat.openids` 非空，但不含本机 openid | 把「我的」页显示的 openid 填进去，或把数组清空 |

`not-in-list` 是最常见的一种，而且现象很迷惑：**聊天气泡照样有模型回答**（问答走的是「库里没配白名单就放行」那条），
就是始终不出现输入框。排查时不要看有没有回答，直接看「我的」页自检的「自由输入」那行。

端上对「不能打字」这个结果只缓存 15 秒（能打字缓存 5 分钟），所以云端改完、重传云函数后，
最多等十几秒再双击一次桌宠就能看到输入框，不用重启小程序。

### 6. 验证

1. 「我的」页 → **连接自检**：`configured` 为 `true`、`iotda.ok` 为 `true`、`deviceCount` 有数；
2. 回「设备」页 → 下拉刷新：出现真实设备卡片（`mock.enabled` 默认 `false`）；
3. 点一下 Luster：气泡里应出现一段讲当前数据的白字。

### 7. 跑测试（可选，提 PR 前建议跑）

在项目根目录执行，十三条都应是「失败 0 项」：

```bash
node tools/verify_signer.js       # 华为云签名算法
node tools/test_cloudfn.js        # 云函数：真跑 exports.main + 内存数据库桩
node tools/check_miniprogram.js   # 静态结构：页面/样式/事件绑定一致性
node tools/test_alarm_swipe.js    # 报警左滑删除的手势状态机
node tools/test_luster.js         # 桌宠纯函数：寒暄/汇总/手势/对话兜底
node tools/test_luster_chat.js    # 桌宠对话：点胶囊→请求→兜底/连点/关闭
node tools/test_buddy_geo.js      # 桌宠落点与停靠区
node tools/test_ai_brief.js       # 云端简报文案
node tools/test_ai_chat.js        # 云端预设问答：白名单/排序/缓存/日限
node tools/test_llm_config.js     # 大模型配置解析：地址/模型/思考开关（不联网）
node tools/test_l1.js             # L1 异常判定与最近越线时间点（阈值/取最近/文案）
node tools/try_ask.js --q "湿度多少"   # 本机模拟真机提问：打印真实 prompt + 真实回答（改提示词用）
node tools/try_ask.js --dry --verbose # 只看拼好的 prompt，不花钱
node tools/test_trend_axis.js     # 趋势图坐标轴
node tools/test_module_toggle.js  # 首页看板模块开关
py -3 tools/test_assets.py        # 立绘资源一致性（需 Pillow）
```

---

## 配置

`cloudfunctions/iotApi/config.js`（从 `config.example.js` 复制；**不要提交真实密钥**）：

| 字段 | 必填 | 默认 | 说明 |
|---|---|---|---|
| `iotda.ak` / `iotda.sk` | ✅ | — | 华为云访问密钥，用于派生 IoTDA Token |
| `iotda.projectId` | ✅ | — | IoTDA 项目 ID |
| `iotda.host` | ✅ | — | 应用接入地址，不带协议头 |
| `iotda.region` | | `cn-north-4` | 区域，派生鉴权时用 |
| `iotda.instanceId` | | — | 标准版实例 ID，决定走哪套接口路径 |
| `iotda.derivedAuth` | | `true` | 标准版/企业版派签；基础版改 `false` |
| `iotda.serviceId` | | `stm32` | 物模型服务 ID |
| `iotda.deviceIds` | | `[]` | 留空 = 拉取实例下全部设备 |
| `iotda.controlChannel` | | `command` | `command` 命令下发 / `shadow` 设备影子 |
| `iotda.commandParamType` | | `boolean` | 命令参数类型：boolean / number / string |
| `auth.mode` | | `open` | `open` 不校验 / `whitelist` 只放行名单 |
| `auth.whitelist` | | `[]` | openid 数组，仅 `whitelist` 模式生效 |
| `sync.staleSeconds` | | `30` | 超过这么久没上报判为离线 |
| `sync.historyLimit` | | `60` | 单次同步写入的历史点数 |
| `sync.historyKeep` | | `500` | 每个设备保留的历史点上限 |
| `ai.enabled` | | `false` | 关掉只走规则文案，**不产生任何费用** |
| `ai.model` | | `deepseek-chat` | OpenAI 兼容接口的模型名 |
| `mock.enabled` | | `false` | 打开会用假数据，仅调试用 |
| `mock.fallbackOnError` | | `false` | 取数失败时是否伪装成成功 |

---

## 项目结构

```text
iot-mp/
├── miniprogram/                 # 小程序前端
│   ├── pages/
│   │   ├── device/              # 设备（首页）：主卡 + 看板 + 桌宠
│   │   ├── detail/              # 设备详情：全部指标与开关
│   │   ├── trend/               # 趋势：折线图 + 时间范围
│   │   ├── alarm/               # 报警：筛选 / 已读 / 左滑删除 / 清空
│   │   └── mine/                # 我的：主题、自检、openid、关于
│   ├── components/ai-buddy/     # 桌宠 Luster（状态机 + 手势 + 行走 + 预设问答）
│   ├── utils/                   # api / model / theme / auto / luster-*（含 luster-chat）
│   └── assets/buddy/            # 立绘：站定、走路三帧、睡姿、眨眼（双装 × 双尺寸）
├── cloudfunctions/
│   ├── iotApi/                  # 数据网关（iotda.js 签名、model.js 归一化）
│   ├── aiApi/                   # 云端简报 + 预设问答（可选，OpenAI 兼容）
│   │   ├── index.js             # action 分发、事实拼装、频控与降级
│   │   └── llm.js               # 供应商表 + 请求封装（与 probe 脚本共用）
│   ├── initDb/                  # 建集合
│   └── envPing/                 # 连通性探针
├── tools/                       # 测试、诊断、素材生成脚本
└── shots/                       # 设计稿与比对截图（不参与打包）
```

---

## 常见问题

**连接自检返回 `403 IOTDA.000021`**
不是权限问题，是「标准版当基础版调」——两者接口路径不同。确认 `iotda.instanceId` 已填，代码据此切到标准版接口。

**`AI_Score` 显示 255 并判为「严重」**
255 是设备端的哨兵值（表示「未评估」）。已按越界处理：仅 0–100 参与评级。

**首页一片空白，但「我的」页正常**
多半是白名单。看自检里的 `authMode`：若是 `whitelist`，说明你的 openid 不在名单里。**别改代码**——云开发控制台 → `users` 集合 → 找到你那行 → `allowed` 改 `true`，实时生效。

**切了白名单，新人拿不到数据也报不出 openid**
`login` 已排在鉴权之前，名单外的人也能拿到自己的 openid，并在 `users` 里留一条 `allowed: false` 的记录。若「我的」页一直显示「获取中…」，说明云端跑的是旧版函数，重新部署 `iotApi`。

**点报警的「删除」提示「云端函数还是旧版」**
`alarmRemove` / `alarmClear` 是后加的 action，云函数没重传。传一次 `iotApi`。

**下拉刷新只有三个小点，不是微信原生刷新头**
设备页改成了 `scroll-view` + `refresher`（为了拖桌宠时页面完全不动）。有意为之，其他页面仍是原生下拉刷新。

**双击 Luster，回答都标着「本地答复」**
说明云端那条路没通：`aiApi` 没部署、还是旧版（没有 `chat` action），或者网络不好。功能不会坏——本地会照同一份数据作答，只是措辞固定。重新部署一次 `aiApi` 即可。

**怎么接大模型？要不要花钱？**
云开发控制台 → 云函数 `aiApi` → 配置：环境变量加 `LLM_KEY`（阿里云百炼的 key），**超时改成 20 秒**。不配也能用，走规则答复、一分钱不花。配了之后单次约 250 输入 / 200 输出 token，`deepseek-v4.1-flash` 按刊例一次不到一分钱，另有每人每天 40 条上限。填之前先 `node tools/probe_llm.js` 在本机验一遍。

**默认只有四个预设问题，没有输入框** —— 个人主体拿不到「深度合成-AI 问答」类目，带输入框的包提交大概率被驳。想自己打字用，见下一条。

**想自己打字问（默认关，只对自己开）**
在 `aiApi` 的环境变量里加两条，然后重新部署云函数（**不用重传小程序**）：

| 变量 | 值 | 作用 |
|---|---|---|
| `FREE_CHAT` | `on` | 打开自由输入。不填 / `off` = 端上不显示输入框 |
| `FREE_CHAT_OPENIDS` | `oXXXX,oYYYY` | 只让名单里的 openid 能打字；留空 = 白名单内所有人 |

端上是否显示输入框由 `health` 返回的 `freeChat` 决定，所以关掉只要把 `FREE_CHAT` 改回 `off`。护栏：单条最多 120 字、同一人 2.5 秒一条、一分钟 8 条、每天仍受 40 条总上限约束；请求会过一次 `msgSecCheck`（接口不可用时放行，不拦正常提问）。

> **这一项的优先级和别处相反：`config.js` 里写了 `freeChat`，就以文件为准，环境变量只在文件没写时才用。**
> 原因是被坑过两次——控制台那个看不见的环境变量会静默盖掉文件里已经改对的配置。2026-11-12 就是：
> 把 `config.js` 的 `openids` 清空了，但控制台还留着早先填的 `FREE_CHAT_OPENIDS`，真机始终没有输入框，
> 界面上还照样显示「开关是 on」。现在 `health` 会返回 `freeChatSrc` 说明每一项读自哪里。
> 名单里写 `*` 也表示不限制。

> 正式提审前记得把 `FREE_CHAT` 关掉：体验版随便用，带输入框的包过不了个人主体的类目审核。

**它能答什么、答不了什么**

自由输入不是「接了个通用聊天」：它看到的只有**当前这一刻**的设备读数。

| 能答 | 不能答（会老实说看不出来） |
|---|---|
| 现在温度 / 湿度 / 光强 / 自评多少 | 昨天今天最高最低、平均、趋势 |
| 现在有没有异常、该做什么 | 上个月电费、设备历史故障次数 |
| 哪台设备离线了 | 没上报的属性（它只会说「没采到」） |
| 「那湿度呢」这类接着上一轮的追问 | 三句之前的设备状态（只带最近 3 轮） |

两个设计取舍值得知道：

- **喂给模型的是全量读数**。简报（`buildFacts`）为了读起来清爽，会省掉正常项，但自由输入用 `describeFull`：光强 386、自评 92 这种「正常但具体的值」也在里面，否则问「光强多少」只能答「看不出来」。
- **建议是「参考」不是「回答方向」**。原来 prompt 里塞一句 `回答方向：<最该做的一条>`，等于给模型下指令，问「湿度多少」也会答「建议开风扇」。现在改成条件句，只在用户真的问「该做什么」时才取用。

改提示词不用每次重传小程序：

```bash
node tools/try_ask.js --q "光强现在多少"          # 打印真实 prompt + 真实回答
node tools/try_ask.js --q "那湿度呢" --prev "温度多少|温度 26.3℃，正常。"
node tools/try_ask.js --q "我该做什么" --scene bad # 换成多项越线 + 有报警的数据
node tools/try_ask.js --dry --verbose            # 只看拼好的 prompt，不花钱
```

它和线上跑的是同一份 `buildAskFacts` / `ASK_SYSTEM` / `askUserMessage`，所以本机好使线上就好使。
多轮历史由端上传，但**云端会重新清洗**：只认 `user` / `assistant`（`system` 永远进不来），最多 6 条、单条 200 字——不对输入做这层白名单，一个能改包的人就能往 history 里塞指令。

**配了 key，回答还是「规则答复」，密钥是不是没生效？**
按这个顺序查，三步内必定位：
1. 本机跑 `node tools/probe_llm.js`：它也失败 → key / 地址 / 模型名的问题，报错里会说改哪个；
2. 本机通了但线上还是规则 → 云函数里的环境变量没保存、或者没重传 `aiApi`（改环境变量后要点「保存」并重新部署，配置才会生效到新实例）；
3. 都对了还是慢 → 云函数超时没改到 20 秒，请求还没回来就被平台掐掉，代码只能退回规则文案。

`aiApi` 支持 `{ "action": "health" }`，会返回 `provider / host / model / llm: ready|not-configured`，是排查「到底连到哪」最快的一步。响应里的 `modelError` 字段带着最后一次失败原因。

**云函数传了，手机界面没变**
云函数部署即时生效；**前端要重新上传体验版**才更新。另外「超时」这类配置项不会被部署覆盖，得手动改。

**包体会不会超 2 MB？**
当前 1.1 MB（立绘占 712 KB）。加素材前先看 `du -sh miniprogram`。

---

## 数据可见性（重要）

三个人共享**同一份**数据，不是各看各的：

| 操作 | 其他人 | 是否设计如此 |
|---|---|---|
| 开关风扇 / 补光 | 同步变化 | ✅ 物理设备只有一台 |
| 标已读 / 全部已读 | 红点一起消失 | ✅ 一套值班记录 |
| 删除 / 清空报警 | 他那条也没了，无回收站 | ✅ 同上 |
| 主题、看板模块、选中设备 | 不变 | ✅ 存本地 |

想改成「按人独立」，需新增 `alarm_reads` / `alarm_hidden` 两张按 openid 记录的表，代价见 [handover §5.27](../docs/miniapp/handover.md)。**当前版本不做。**

---

## 文档

文档位于工作区 `docs/miniapp/`（与 `iot-mp/` 同级）：

| 文档 | 内容 |
|---|---|
| [handover.md](../docs/miniapp/handover.md) | **接手必读**。架构决策、27 个已踩过的坑的复盘、待办清单 |
| [design-system.md](../docs/miniapp/design-system.md) | 色值、字号、间距、双主题变量表 |
| [project-plan.md](../docs/miniapp/project-plan.md) | 需求范围与阶段划分 |
| [environment-checklist.md](../docs/miniapp/environment-checklist.md) | 账号、密钥、环境 ID 的申请步骤 |
| [release-checklist.md](../docs/miniapp/release-checklist.md) | 上传体验版 / 正式发布前的检查项 |
| [iotda-credentials.md](../docs/miniapp/iotda-credentials.md) | 华为云侧的产品模型与命令定义 |
| [test-report.md](../docs/miniapp/test-report.md) | 测试口径与历史结果 |
| [decisions.md](../docs/miniapp/decisions.md) | 有争议的取舍及其理由 |

---

## 贡献

欢迎 PR，但请先跑一遍上面「跑测试」里的九条命令——它们覆盖云函数行为、页面结构、桌宠状态机和素材一致性，改坏任何一条都会红。

约定：

- 页面/组件改动由 `check_miniprogram.js` 兜结构；**新增断言前先确认「改坏会红」**；
- 改动云函数，`test_cloudfn.js` 必须同步加用例；
- 立绘、图标改动跑 `test_assets.py`。**别手改 PNG**——颜色是脚本算出来的（见 `tools/unify_walk_tone.py`）；
- 不要提交 `cloudfunctions/iotApi/config.js`（已在 `.gitignore` 里）。

## 安全

- `config.js` 含华为云 **AK/SK**，能读写你的全部设备。模板已拆成 `config.example.js`，真实配置被 `.gitignore` 排除。
- `openid` 不是敏感凭据：它只是「你是哪位」的编号，不能登录、不能伪造——云函数只认微信注入的 `getWXContext().OPENID`，前端传什么都不作数。
- 大模型调用按 token 计费（约 0.0014 元 / 次）。`ai.enabled` 默认 `false`，**默认不产生任何费用**。

## 致谢

- 立绘、走路帧、睡姿由 AI 生成，再用脚本做色调与镜像一致性校正（`tools/gen_luster_assets.py`、`unify_walk_tone.py`、`make_walk3_mirror.py`）；
- 图标由 `tools/gen_tabbar_icons.py` 生成；
- 华为云 IoTDA 接入参考官方 Node.js SDK 示例。

## License

MIT。素材（`miniprogram/assets/`）随项目一同授权，二次分发请保留出处。
