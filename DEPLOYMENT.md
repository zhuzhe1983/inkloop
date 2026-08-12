# Inkloop 发布与运行配置

Inkloop 默认采用“双端发布”：同一个通过构建和测试的工作区版本，同时发布到 ChatGPT Sites 和内网 Synology Docker。只有在发布请求明确写明“仅 Sites”或“仅 Docker”时，才跳过另一端。

## 1. 默认发布顺序

1. 在项目根目录执行构建和测试，任何失败都停止发布。
2. 发布 ChatGPT Sites，保留现有公开域名和云端 D1。
3. 将同一工作区同步到 `192.168.199.80:/volume1/docker/inkloop`。
4. 在本机生成 `linux/amd64` 镜像并传入 NAS，再执行 `docker compose up -d --no-build`。这样不依赖 NAS 访问 npm registry。
5. 分别检查 Sites 页面、Docker 页面、`/api/health`、公开应用数据库、LLM 配置状态和地图配置状态。

两个环境的数据存储彼此独立：Sites 使用云端 D1；Docker 使用挂载在 `./data` 的本地 D1/SQLite 状态。发布代码不会自动同步两边“发现”页的公开应用数据。

## 2. 环境变量

| 变量 | 必需 | 用途 |
| --- | --- | --- |
| `LLM_API_KEY` | 在线 LLM 必需 | LLM 网关密钥 |
| `LLM_BASE_URL` | 推荐 | Sites 使用 OpenAI 兼容网关 URL；Docker 默认使用 `http://llm-proxy:8788/v1` |
| `LLM_MODEL` | 可选 | 界面默认模型；用户可从 `/v1/models` 返回的列表切换，留空时默认自动选择 |
| `BAIDU_MAP_AK` | 地图必需 | 百度地图 Web Service AK，只在服务端使用 |
| `LLM_PROXY_UPSTREAM` | Docker 可选 | Docker 私有代理的 HTTPS 上游，默认 `https://hub.tsingfly.com` |

Sites 的值由 Sites 运行环境管理。Docker 的 Worker 变量保存在服务器项目目录的 `config/.dev.vars`，模板为 `config/.dev.vars.example`；若需更换代理上游，在项目根目录 `.env` 中设置 `LLM_PROXY_UPSTREAM`。密钥文件不会被 Git 或 Docker 构建上下文收录。

Docker 中的 `llm-proxy` 只暴露在 Compose 私有网络，不映射宿主机端口。它使用 Node 系统 CA 校验并转发到固定 HTTPS 上游，解决本地 Workers 运行时与部分网关证书链的兼容差异；API Key 仍由 Inkloop Worker 在请求时通过 Authorization 传入，不保存在代理镜像或日志中。

## 3. ChatGPT Sites 发布

- 项目标识保存在 `.openai/hosting.json`，不要删除或替换现有 `project_id`。
- 使用 Sites 发布流程保存新版本并部署到现有站点。
- 发布后至少验证首页、`/api/health`、`/api/apps`、一次 LLM 生成和地图配置状态。
- 公开站点仍使用：<https://inkloop-todoo.zhuzhe1983.chatgpt.site/>

## 4. Synology Docker 发布

### 目录和端口

- 主机：`192.168.199.80`
- 项目：`/volume1/docker/inkloop`
- 默认局域网端口：`38080`
- 数据目录：`/volume1/docker/inkloop/data`
- 密钥文件：`/volume1/docker/inkloop/config/.dev.vars`

服务器 3000 端口已由 InkOS 使用，因此 Inkloop 映射到 38080。需要换端口时，在项目目录创建 `.env` 并设置 `INKLOOP_PORT=新端口`。

### 首次部署

```bash
mkdir -p /volume1/docker/inkloop/config /volume1/docker/inkloop/data
cp config/.dev.vars.example config/.dev.vars
chmod 600 config/.dev.vars
/usr/local/bin/docker compose up -d --build
```

Synology 的非交互 SSH PATH 不包含 `/usr/local/bin`，脚本和远程命令应使用 `/usr/local/bin/docker`。

### 更新

先将工作区同步到服务器，保留远端 `config/.dev.vars` 与 `data/`。推荐在开发机跨平台构建并传输镜像，避免 NAS 直接安装 npm 依赖：

```bash
docker buildx build --platform linux/amd64 -t inkloop:local --load .
docker save inkloop:local | gzip -1 | ssh 192.168.199.80 '/usr/local/bin/docker load'
ssh 192.168.199.80 \
  'cd /volume1/docker/inkloop && /usr/local/bin/docker compose up -d --no-build --remove-orphans'
```

### 健康检查

```bash
/usr/local/bin/docker compose ps
curl -fsS http://127.0.0.1:38080/api/health
curl -fsS http://127.0.0.1:38080/api/apps
/usr/local/bin/docker compose logs --tail=100 inkloop
```

`/api/health` 返回 `status: ok` 且数据库为 `ready` 才代表核心服务健康。`llmConfigured` 和 `mapConfigured` 分别表示 LLM、地图密钥是否已注入，不返回密钥内容。

### 回滚

更新前可以给当前镜像加时间标签：

```bash
/usr/local/bin/docker image tag inkloop:local inkloop:rollback-YYYYMMDD-HHMM
```

如果新版本不健康，将 `compose.yaml` 中镜像临时改为该回滚标签，并关闭 `build` 后重新启动。`data/` 与 `config/.dev.vars` 不随镜像删除。

## 5. HTTPS 与 Web Bluetooth

`http://192.168.199.80:38080` 可用于页面、LLM、天气、日历和地图测试，但浏览器通常不会在普通局域网 HTTP 地址开放 Web Bluetooth。要在 Docker 版本写入设备，需要在 Synology 反向代理或其他网关上配置可信 HTTPS 域名，并反向代理到 `http://127.0.0.1:38080`。

完成 HTTPS 后，验证浏览器地址栏为安全连接，再检查“蓝牙可用”和设备选择器。自签名但未被客户端信任的证书通常仍不满足安全上下文要求。
