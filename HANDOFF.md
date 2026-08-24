# Inkloop 项目交接文档

最后更新：2026-08-04  
新工作目录：`/Users/zhuzhe/Workspace/inkloop`  
线上地址：<https://inkloop-todoo.zhuzhe1983.chatgpt.site>  
当前线上基线：Sites 第 39 版，Git commit `64f4c13`  

## 1. 项目目标

Inkloop 面向 TodooCard 528 × 792 六色蓝牙电子墨水屏，提供一条完整的创作与写屏流程：

1. 用户用自然语言描述想展示的内容，LLM 或本地模板生成结构化应用。
2. 在网页 Canvas 中以真实六色色板预览，支持手动开关、拖拽和调整文字、时间、天气、二维码等元素。
3. 保存到本机，或公开到发现页供其他用户复用。
4. 选择 Web Bluetooth 设备后单次写入，或在浏览器页面存活期间定时刷新。

当前支持天气、海报/图片、时钟、专注卡、倒计时、指标卡、月历、课程表、多日智能日程和静态地图。智能日程可读取多个 iCal / webcal 数据源，支持横竖屏、三日/五日时间轴、重叠日程并排和空闲时段压缩；地图支持右侧选点、浏览器定位、IP 城市级兜底、横竖屏和可调 zoomLevel。

## 2. 技术栈与运行环境

- React 19、Next 16 API/页面约定、vinext、Vite 8。
- Cloudflare Workers 兼容输出，Sites 托管。
- Cloudflare D1 保存公开应用。
- Canvas 负责画面排版、六色量化和设备帧生成。
- Web Bluetooth 负责浏览器内设备授权、连接和写屏。
- Node.js 最低版本：`22.13.0`。
- 包管理器：npm；应保留 `package-lock.json`，优先使用 `npm ci`。

## 3. 目录说明

| 路径 | 作用 |
| --- | --- |
| `app/ink-studio.tsx` | 主界面、Canvas 渲染、图片/天气/iCal 加载、设备与定时任务管理；当前最大的业务文件。 |
| `app/lib/app-model.ts` | 应用、屏幕、元素、表格和定时计划的数据模型；本地模板生成器。 |
| `app/lib/todoo-card-core.js` | 无框架的蓝牙协议核心、六色编码、GATT 连接、握手、分包和错误类型；协议修改的主文件。 |
| `app/lib/todoo-card.ts` | React UI 使用的 TypeScript 适配层，负责进度文案和重连封装。 |
| `app/api/generate/route.ts` | LLM 网关调用、返回 JSON 的校验与降级到本地模板。 |
| `app/api/artwork/route.ts` | 主题图片代理与素材源降级。 |
| `app/api/weather/route.ts` | 天气代理；Open-Meteo 优先，wttr.in 备用。 |
| `app/api/calendar/route.ts` | iCal/webcal 下载、SSRF 防护、解析、时间范围裁剪与多源合并。 |
| `app/api/map/route.ts` | 百度地点解析、坐标转换、IP 粗定位和静态地图图片的服务端代理；AK 不下发浏览器。 |
| `app/api/apps/route.ts` | 发现页公开应用的 D1 读写。 |
| `db/schema.ts`、`drizzle/` | D1 的 `public_apps` 表和迁移。 |
| `tests/todoo-card-core.test.mjs` | 蓝牙协议、六色编码、GATT 模拟和资源上限测试。 |
| `.openai/hosting.json` | Sites 项目和逻辑 D1 绑定声明。 |
| `.env.example` | 本地 LLM 环境变量模板，不包含真实密钥。 |

## 4. 本地开发

```bash
cd /Users/zhuzhe/Workspace/inkloop
npm ci
cp .env.example .env
npm run dev
```

常用检查：

```bash
npm run build
npm test
npm run lint
```

`npm test` 会先执行生产构建，再运行协议测试。协议没有改动时，不需要每次连接真机重复测试；只有 `todoo-card-core.js`、协议来源或设备固件发生变化时，才安排真机回归。

## 5. 环境变量

| 变量 | 必填 | 说明 |
| --- | --- | --- |
| `LLM_API_KEY` | 线上生成需要 | LLM 网关 App Key。未配置时自动使用本地模板。 |
| `LLM_BASE_URL` | 否 | OpenAI 兼容网关根地址；默认使用项目内置网关。不要以 `/` 结尾。 |
| `LLM_MODEL` | 否 | 界面默认模型 ID；用户可从 `/models` 返回的列表中切换，为空时默认自动选择。 |
| `BAIDU_MAP_AK` | 地图功能需要 | 百度地图 Web 服务端 AK。只允许配置在服务端，未配置时地图编辑器会给出可恢复提示。 |
| `BAIDU_MAP_BASE_URL` | Docker 需要 | 百度 API 根地址；Docker 指向 Compose 私有 Node 代理，Sites 与本地留空直连百度。 |
| `OUTBOUND_PROXY_BASE_URL` | Docker 需要 | 图片和天气出站代理；Docker 指向 `http://llm-proxy:8788/outbound`，Sites 与本地留空直连上游。 |

本地密钥只写入 `.env`，不要提交。线上变量通过 Sites 运行环境维护；不要把密钥写入 `.openai/hosting.json` 或前端代码。

## 6. 数据与持久化

### 浏览器本地数据

- `inkloop-apps-v1`：用户保存在当前浏览器的应用。
- `inkloop-weather-city-v1`：最近使用的天气城市。
- `inkloop-calendar-sources-v1`：个人日历名称、地址和启用状态。

个人 iCal 地址只保存在本地浏览器，不写入公开应用。清除站点数据、换浏览器或换域名后，本地数据不会自动迁移。

### 服务端数据

公开发现页使用 D1 的 `public_apps` 表。应用 API 会在缺表时创建表和索引；正式修改表结构时仍应更新 `db/schema.ts` 并生成迁移：

```bash
npm run db:generate
```

### 当前限制

设备定时任务只保存在当前页面内存中，不会跨刷新或浏览器重启恢复。要让任务持续执行，需要页面保持打开、电脑不休眠并且设备处于蓝牙范围内。

## 7. 蓝牙与写屏协议

### 浏览器条件

- 页面必须处于安全上下文：`localhost` 或 HTTPS。
- 推荐桌面 Chrome；Safari/Firefox 当前不适合作为 Web Bluetooth 主流程。
- 首次必须由用户点击触发系统设备选择器。授权后，支持 `navigator.bluetooth.getDevices()` 的浏览器可在同一站点来源下恢复设备对象，减少重复选择。
- 浏览器授权按站点来源保存；`localhost`、Sites 域名和自定义域名彼此不是同一个授权来源。

### 设备兼容

已兼容或纳入筛选的名称前缀包括 `NEMR`、`TodooCard`、`PotatoCard`、`PICKSMART` 和 `T3`。Web Bluetooth 不向网页暴露系统扫描列表中的 MAC 地址，因此不要用 `AD:99:9E:BE:7B:26` 作为网页筛选条件。

上游 `TodooCard_Skills` 已同步到提交 `990f21caeaa74e2488ab72f9e343c04b1586689e`。v0x8C+ 安全固件使用厂商 `0x5053`、屏幕类型 `0x134C` 和加密 GATT；首次绑定需长按背面按钮 10 秒，并在前灯快闪后的 60 秒窗口内显式读取加密 Battery Level。普通 BLE 连接不能作为配对成功依据。核心驱动已提供显式安全配对能力，但不会在旧设备写屏路径中自动触发，以保持现有体验。

### 协议基线

- 可见尺寸：528 × 792。
- 色码：黑 0、白 1、黄 2、红 3、蓝 5、绿 6。
- 真机默认帧：219120 bytes。
- 默认传输：913 包。
- 支持 FEF 与兼容 FDF GATT profile；当前项目设备默认继续使用真机验证过的 profile。
- `skill-t3` 的 218893-byte QuickLZ-stored 帧和部分兼容参数保留为显式可选项，不替换真机验证默认值。
- T3 使用固定 `todoocard-correct` 底层面板校准；调用方提供正向源图，不得叠加旋转或镜像。

`app/lib/todoo-card-core.js` 是协议事实来源，`app/lib/todoo-card.ts` 只做 UI 适配。原始协议整理项目仍位于 `/Users/zhuzhe/Workspace/todoo/web/`；若那里更新，应先备份当前 core、对比差异、更新测试，再考虑同步。

### 重连与定时

应用使用一次性 `setTimeout` 链式调度：每次传输结束后再计算下次时间，避免 `setInterval` 在写屏约一分钟时产生重叠写入。断线或 “Bluetooth Device is no longer in range” 会清理旧 GATT 会话、重新发现服务并按退避时间重试；用户也可在设备任务面板立即重试。

同一设备上，小于 5 分钟的高频任务只允许一个；新任务会询问是否替换。任务按设备分组显示成功次数、失败原因、剩余时间和最近一次 Canvas。

M5 PaperColor 已接入 ESP32 Wi‑Fi adapter。该路径将计划持久化到设备 LittleFS，由设备每 30 秒同步服务器 revision 并在本机定时拉取 PNG；浏览器无需保持打开。服务器删除任务后会提升 `desired_revision`，设备在线时以完整任务集替换本地状态。SKU 元数据、adapter 边界与扩展流程见 `docs/esp32-device-architecture.md`。

## 8. 画面与外部数据源

### 六色渲染

屏幕只支持黑、白、黄、红、蓝、绿，不存在真正的浅色。预览画布保持全彩原图，不再预量化。TodooCard 写入只做一次官方 `TodooCard_Skills` 量化：T3 色板（绿 `[0,255,0]`）、普通 RGB 距离、逐行 Floyd-Steinberg。传输仍用已验证的 219120-byte FEF 帧，不切换 218893-byte skill 短帧。

纯文字、日历和课程表默认 `Inkloop text`：写入按最近六色、不再抖动。照片默认 `Official Skill`。日程卡的浅底继续用规则稀疏网点模拟，不要用 CSS 半透明替代。

### 图片

`/api/artwork` 按屏幕方向请求 528 × 792 或 792 × 528 图片。Docker 中这些请求经 Node 私有代理出站，避免 workerd 与部分上游证书链不兼容：

1. LoremFlickr 主题图片。
2. Wikimedia Commons 搜索结果。
3. Picsum 仅作为通用抽象主题的确定性后备。

接口会返回素材来源和真实访问地址供预览显示。外部服务限流或返回默认猫雕塑时，用户应点击“重新生成”换 seed。

### 天气

Open-Meteo 优先，wttr.in 备用；常见中国城市使用内置坐标以减少地理编码失败。两个来源都失败时，接口返回 `available: false` 而不是让海报生成流程整体失败。

### 日历

支持 HTTPS iCal 和 `webcal://`。服务端会把 webcal 转成 HTTPS，并限制重定向、端口、私网地址和 1 MB 文件大小。当前可同时读取最多 5 个个人来源，并可叠加中国公众假期和农历显示。

### 地图

地图使用百度静态地图 `staticimage/v2`，逻辑尺寸按屏幕方向请求 264 × 396 或 396 × 264，并以 `scale=2` 得到 528 × 792 或 792 × 528，减少额外下载和缩放。所有百度接口都由 `/api/map` 代理，`BAIDU_MAP_AK` 不会写入前端 URL、应用 JSON 或 Canvas。

- LLM 只生成 `kind=map`、地点意图和初始 zoomLevel；精确位置由右侧编辑器确认。
- 地图选点使用 BD-09；浏览器定位先获取 WGS84，再由服务端转换为 BD-09。
- IP 定位只作为未选点时的城市级兜底，界面会明确标注“估算位置”。
- zoomLevel 支持 3—19，可手输、滑动或用加减按钮调整，调整后立即刷新预览。
- 选点画布基于静态地图做点击位置换算，适合日常选点；对厘米级或边界测绘场景应改用百度交互地图 SDK 做最终确认。

## 9. LLM 生成契约

`/api/generate` 使用 OpenAI 兼容的 `/chat/completions` 和 `/models`。GET 会向前端返回可选模型 ID，用户选择保存在浏览器中，POST 会再次校验所选模型仍在网关列表中。LLM 只负责返回受约束的结构化应用 JSON；服务端会重新校验 kind、方向、元素、表格、文字长度和刷新周期，不能直接执行模型返回的任意代码。

提示词和规范的重点：

- 六色限制、无边框默认、全屏图片裁剪。
- 主题图需要明确英文检索词和 stylist/style 描述。
- 时间变量、天气、时钟、月历、课程表、智能日程、二维码。
- 地图意图、地点关键词和初始 zoomLevel；精确坐标、标记与地址显示留给右侧手动配置。
- 无图片的纯文字界面优先 `Inkloop text`。
- 表格或日程带图时要先保证文字信息层级，不让背景抢占可读性。

## 10. 测试策略

协议测试覆盖：

- 产品和协议常量只读性。
- 219120-byte 真机帧与 218893-byte skill 兼容短帧。
- 六色像素编码、透明像素白底、方向变换。
- 错误帧长度、块头、色码和尾部填充拒绝。
- 50 MP 输入资源上限。
- 模拟 FEF/FDF GATT 服务、三段握手、913 包、完成通知和错误属性。
- v0x95 厂商广播解析、实体配对窗口和加密 Battery Level 配对验证。

普通 UI 修改至少运行 `npm run build`。协议文件或依赖变更运行 `npm test`。真机回归应单独记录设备名、浏览器版本、固件状态、写入耗时和最终显示照片。

## 11. Sites 发布

`.openai/hosting.json` 已绑定现有 Sites 项目，逻辑 D1 名称为 `DB`。发布必须使用 Sites 工作流，保留现有 project id，不要新建站点，也不要把 `.openai/hosting.json` 改成真实 Cloudflare 资源 ID。

标准流程：

1. 确认工作树和本次变更范围。
2. `npm run build`，必要时运行协议测试。
3. 提交并推送当前精确源码。
4. 打包并保存一个 Sites 版本。
5. 由于当前站点是公开访问，生产发布必须获得用户明确确认。
6. 发布后等待状态成功，再打开线上地址检查。

当前 Git 仓库没有持久化普通 `origin`；Sites 会按发布流程提供短时源码写入凭证。不要把凭证写入 remote URL、Git 配置或文档。

## 12. 已知风险与后续建议

1. `app/ink-studio.tsx` 体积很大。后续优先拆分 Canvas 渲染器、外部数据 hooks、蓝牙任务管理器和四个主标签页。
2. TodooCard 蓝牙定时任务仍依赖前台页面，无法保证系统级长期调度；M5 PaperColor 已通过设备端 Wi‑Fi 调度解决这一限制。其他蓝牙 SKU 若要脱离浏览器，仍需原生应用、常驻服务或新增硬件 adapter。
3. 外部图片、天气和 iCal 都可能限流或超时；继续保留超时、缓存、降级和明确来源信息。
4. 公开应用写入当前没有用户级审核或频率限制。规模扩大前应增加滥用控制、内容审核和写入配额。
5. 个人 iCal URL 常带访问令牌。不要写日志、提交仓库、写入 D1 或公开分享。

## 13. 本次迁移说明

- 迁移目标：`/Users/zhuzhe/Workspace/inkloop`。
- 已复制 Git 历史和全部源码。
- 没有复制 `node_modules`、`dist`、`.vinext`、`.wrangler`、Playwright 临时状态、截图输出和 TypeScript 缓存。
- 已在新目录执行干净依赖安装、生产构建和完整测试；12 项协议测试全部通过。
- 原 Codex 工作目录暂时保留，作为迁移回滚备份；确认新目录稳定后再由项目所有者决定是否删除。
