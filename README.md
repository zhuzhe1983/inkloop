# Inkloop

Inkloop 是 TodooCard 六色蓝牙电子墨水屏的网页应用工坊。用户可以用自然语言或手动配置生成画面，在 528 × 792 / 792 × 528 Canvas 中预览，然后单次或定时写入 Web Bluetooth 设备。

线上地址：<https://inkloop-todoo.zhuzhe1983.chatgpt.site>

## 本地启动

要求 Node.js `>=22.13.0`。

```bash
npm ci
cp .env.example .env
npm run dev
```

浏览器访问终端显示的本地地址。Web Bluetooth 只在安全上下文中工作：本机可使用 `localhost`，线上必须使用 HTTPS，推荐桌面版 Chrome。

```bash
npm run build
npm test
```

完整的架构、环境变量、蓝牙协议、数据源、测试和发布说明见 [HANDOFF.md](./HANDOFF.md)。

ChatGPT Sites 与 Synology Docker 的双端发布、环境变量、健康检查和回滚说明见 [DEPLOYMENT.md](./DEPLOYMENT.md)。默认发布会同时更新两个环境。
