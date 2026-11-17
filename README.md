# 实验室环境监测系统 · Env_lab

**STM32 端侧采集与边缘判级，两条完全不同的链路把数据送到人眼前：本机 Web 页看现场，微信小程序看随时随地。**

> Z.Z.J 队作品。当前小程序以体验版在队内分发，未正式发布。
> 技术栈：C / Huawei LiteOS / Keil AC5 · 单文件 HTML + Chart.js · 微信原生小程序 + 云函数 · 华为云 IoTDA

---

## 1. 这个仓库是什么

仓库里是三块**并列**的东西，不是三个版本、也不是前后端严格分层：

| 部分 | 位置 | 技术形态 | 跑在哪 | 给谁用 |
|---|---|---|---|---|
| **固件** | `backend/Laboratory_Env_Monitor/` | C，Keil MDK + ARMCC 5，LiteOS 三任务 | STM32L431VCTx（256 KB Flash / 64 KB RAM） | 设备本身 |
| **Web 监控页** | `frontend/多传感器环境监控平台.html` | 单文件 HTML + 原生 JS + Chart.js（CDN） | 浏览器，需本机后端托管 | 实验室现场大屏 / 电脑 |
| **微信小程序** | `frontend/WeChatminiprogram/` | 原生小程序 + 4 个云函数 | 微信 | 手机上随时随地 |

三块之间唯一的硬约定是**属性名和阈值口径**，见第 5 节。其余各改各的，互不引用源码。

---

## 2. 两条数据链路

设备侧一次采集出四路物理量 + 两路算法结果：SHT30（温/湿）、BH1750（光照）、MQ-5（可燃气，开关量）、AS312（人体，开关量），
经加权融合成 0~100 分并映射 L1~L4，同时跑一路端侧 AI（模型判"现在"，回归判"要去哪"）。

数据出去有两条路，**互不备份**：

```
【链路 A · 本机直连】现场看数，信息密度优先

  STM32 ──USB──► CH340 ──► COM6 @115200 ──pyserial──► serial_bridge.py
                                                          │
                                              POST /api/ingest（落库 20 s 节流）
                                                          ▼
                                                     server.py ──► MySQL（连不上自动回退 SQLite）
                                                          │
                                     GET /api/{latest, readings, summary, status}
                                                          ▼
                                                   浏览器（2 s 轮询）
```

```
【链路 B · 云端】随时随地看，跨网络

  STM32 ──WiFi──► ESP8285 ──MQTT──► 华为云 IoTDA（13 项属性，5 s 一报）
                                          │  AK/SK 只存在云函数里
                                          ▼
                                  云函数 iotApi ──wx.cloud──► 小程序（30 s 刷新）
                                          │
                                  云函数 aiApi ──► 百炼 deepseek ──► 桌宠 Luster 气泡
```

后端没起，小程序照样能看；IoTDA 断了，现场大屏照样有数。这也是为什么两端的阈值不是一套（见 5.2）。

---

## 3. 目录结构

```
Env_lab/
├── backend/
│   ├── README.md                      固件说明：引脚、阈值速查、规则、串口格式
│   ├── design-spec.md                 阈值推导：每个数字为什么是这个数（最该读的一份）
│   └── Laboratory_Env_Monitor/        Keil 工程
│       ├── task_user/                 main / sensor_task / control_task / view_task / ai_monitor
│       ├── Hardware/                  驱动；HANDLE/handle_config.h 集中放阈值，FUSION/fusion.c 放评分与规则
│       ├── Components/                oc 协议、offline_cache 离线缓存、GUI、cJSON、esp8285、nb_at
│       ├── Middlewares/NanoEdgeAI/    端侧模型库 libneai.a
│       ├── MDK-ARM/                   .uvprojx / .uvoptx
│       └── tools/                     离线分析脚本（Python，不参与固件编译）
└── frontend/
    ├── FRONTEND.md                    两端对照：Web 页与小程序的设计差异、阈值口径
    ├── 多传感器环境监控平台.html         Web 监控页，单文件，无构建
    └── WeChatminiprogram/             小程序工程（含 project.config.json 的这一层）
        ├── miniprogram/               pages(设备/趋势/报警/我的/详情) + components/ai-buddy + utils
        ├── cloudfunctions/            iotApi / aiApi / envPing / initDb
        ├── FRONTEND.md                小程序侧说明（与上级那份不完全一致，见第 7 节）
        ├── README.md                  小程序独立 README（部署、排错、数据可见性）
        └── 首次配置.txt                第一次跑必须改的 3 处，照做最快
```

---

## 4. 三个部分怎么跑起来

### 4.1 固件

```bash
# 需要 Keil MDK-ARM 5 + ARM Compiler 5.0.6（AC5），无需外网依赖
backend/Laboratory_Env_Monitor/MDK-ARM/Laboratory_Env_Monitor.uvprojx
```

全量编译 → 烧录 → 串口 **115200 8N1**。上电后每秒一行摘要：

```
| T:28.3C H:49.5% L:382 lx | Body:Someone Gas:L1 | Fuse:12.4 L1 | ... | AI: 88 RUN |
```

每 10 秒另有一行 `[AI] in=... model=... trend=... risk=... sim=... r=... R2=... jmp=...`。

三个周期：`sensor_task`（prio 2，采样 + 融合 + 执行器 + 显示）、`view_task`（prio 1）、`control_task`（prio 0，协议 + 网络）。
单周期顺序：`Sensor_Sample → Alarm_Evaluate → Actuator_Update → Fusion_Evaluate → AiMonitor_Process → Fusion_ApplyAi → Fusion_Stabilize → 显示/上报`。

### 4.2 Web 监控页

```bash
pip install pyserial pymysql
python run.py            # 单进程内起 server.py + serial_bridge.py
# 浏览器打开 http://127.0.0.1:8000
```

页面本身是单文件、无构建，改完刷新即可。但它**不能双击打开**——请求走相对路径 `/api/*`，`file://` 下必然 404，必须由后端托管。

后端要提供 4 个读接口供页面用（`/api/latest`、`/api/readings`、`/api/summary`、`/api/status`），外加 1 个写接口 `POST /api/ingest` 供串口桥推帧。页面 2 秒轮询一次，`Promise.all` 并发，单接口失败只降级对应区块。

> ⚠️ **`run.py` / `server.py` / `serial_bridge.py` 当前不在本仓库里。** 见第 7 节，跑之前先补。

### 4.3 微信小程序

1. 微信开发者工具导入 **`frontend/WeChatminiprogram/`**（含 `project.config.json` 的那一层，不是里面的 `miniprogram/`）
2. `miniprogram/app.js` 第 13 行填自己的云开发环境 ID
3. `cloudfunctions/iotApi/config.js`、`aiApi/config.js` 从各自的 `config.example.js` 复制后填 AK/SK 与大模型 key
4. 右键两个云函数 →「上传并部署：**云端安装依赖**」（选"所有文件"会缺 `wx-server-sdk`）
5. 云开发控制台建三个集合：`users`（`allowed: true` 表示该 openid 可访问）、`messages`、`alarms`

细节和排错见 `frontend/WeChatminiprogram/首次配置.txt` 与同目录 `README.md`。

**前端不直连数据库、不直连 IoTDA，小程序包里没有任何密钥**，所有读写经 `wx.cloud.callFunction`。

---

## 5. 跨端契约（改这两样会同时坏两端）

### 5.1 属性名 · 13 个，直接对应 IoTDA 物模型

服务 ID 固定为 `stm32`，属性名定义在 `Components/esp8285/esp8285.h`：

| 类别 | 属性 | 类型 |
|---|---|---|
| 环境 | `SHT30_T` `SHT30_H` `Light` | 数值 |
| 气体 | `Gas` | 字符串 `"Safe"` / `"Leak"`（注意首字母大写） |
| 开关量 | `Body_IR` `Sound` `Fill` `Fan` | 布尔 |
| 融合 | `Fuse_Level`（1~4）`Fuse_Score` `Dominant` | 数值 / 字符串 |
| AI | `AI_Score` `Temp_Rise` | 数值 |

`Dominant` 取值：`Temp/Humi`、`Gas`、`Body IR`、`Light`。

固件侧在 `Components/oc/ocprotocol.c` 用 `snprintf` 拼 JSON（约 280~296 行），云函数侧在 `cloudfunctions/iotApi/model.js` 解析。**改名就断**。

### 5.2 阈值 · 三套口径，不要互相抄

| 指标 | 固件 | Web 页 | 小程序 |
|---|---|---|---|
| 温度 | 正常 0~40 ℃，36 ℃ 起软肩计分 | >40 或 <0 标红 | **≥35 ℃ 警告** |
| 湿度 | 20~80 %RH | >80 或 <20 标红 | **≥80 %** |
| 光照 | <100 lx 触发补光 | <100 lx 标琥珀 | **≥800 判过亮**（单位未定标） |
| 综合 | 融合分 25 / 50 / 75 分四级 | 融合分 >75 严重、>50 关注 | **AI_Score <20 异常** |
| 离线 | — | — | >5 分钟提示 |

两处**方向相反**，最容易写错：

- **AI_Score 是减分制**：100 才正常，越低越偏离，所以判定是 `< 20`。写成 `> 70` 就变成"越健康越报警"。
- 设备偶发上报 `AI_Score = 255`，那是**无效哨兵值不是满分**，前端必须显示"读数无效"并排除出异常统计。

小程序的阈值常量有两份（`miniprogram/utils/model.js` 与 `cloudfunctions/iotApi/model.js` 各一份 `THRESHOLD`），是刻意的重复（云函数与小程序不能共享源码目录），**改一处必须同步另一处**，否则会出现"前端显示正常、云端判报警"。

---

## 6. 文档索引

| 想搞清楚 | 看这份 |
|---|---|
| 阈值为什么是这个数、场景验算、AI 双通道推导 | `backend/design-spec.md` ← **信息密度最高** |
| 引脚表、规则表、执行器联动、串口字段 | `backend/README.md` |
| Web 页与小程序为什么设计得不一样 | `frontend/FRONTEND.md` |
| 小程序部署、排错、数据可见性、订阅消息 | `frontend/WeChatminiprogram/README.md` |
| 第一次跑必须先改的 3 处 | `frontend/WeChatminiprogram/首次配置.txt` |
| 趋势判据能不能复现（无需硬件） | `backend/Laboratory_Env_Monitor/tools/gate_check.py` |

---

## License

小程序侧为 MIT，素材（`WeChatminiprogram/miniprogram/assets/`）随项目授权，二次分发请保留出处（见 `frontend/WeChatminiprogram/README.md`）。固件所依赖的 STM32 HAL、Huawei LiteOS、NanoEdgeAI 库分别遵循各自上游许可，分发前请自行核对。
