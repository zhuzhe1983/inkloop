"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import {
  featuredApps,
  generateInkApp,
  intervalFor,
  scheduleLabel,
  starterApp,
  starterPrompt,
  type InkApp,
  type ScheduleMode,
  type ScreenSpec,
} from "./lib/app-model";
import { TodooCard, type TodooProgress } from "./lib/todoo-card";

type Tab = "studio" | "mine" | "explore" | "device";
type Toast = { tone: "success" | "error" | "info"; message: string } | null;
type ToastTone = NonNullable<Toast>["tone"];
type GeneratorStatus = "checking" | "online" | "local";

const LOCAL_APPS_KEY = "inkloop-apps-v1";

const navItems: Array<{ id: Tab; label: string; glyph: string }> = [
  { id: "studio", label: "创作台", glyph: "✦" },
  { id: "mine", label: "我的应用", glyph: "▦" },
  { id: "explore", label: "发现", glyph: "◎" },
  { id: "device", label: "设备中心", glyph: "⌁" },
];

const samplePrompts = [
  "每天 8 点显示上海天气和带伞提醒",
  "显示新品发布倒计时",
  "每 15 分钟更新会议室状态",
  "每小时显示本月销售目标进度",
];

const accentColors = {
  red: "#dc3f2f",
  blue: "#2756c7",
  green: "#087c4e",
  yellow: "#e5c900",
};

function fitText(ctx: CanvasRenderingContext2D, text: string, maxWidth: number, startSize: number) {
  let size = startSize;
  while (size > 24) {
    ctx.font = `800 ${size}px Arial, "PingFang SC", sans-serif`;
    if (ctx.measureText(text).width <= maxWidth) break;
    size -= 2;
  }
  return size;
}

function drawScreen(canvas: HTMLCanvasElement, spec: ScreenSpec) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const width = 528;
  const height = 792;
  const ink = "#151816";
  const paper = "#f4f0dc";
  const accent = accentColors[spec.accent];

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = paper;
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = ink;
  ctx.lineWidth = 3;
  ctx.strokeRect(22, 22, width - 44, height - 44);

  ctx.fillStyle = ink;
  ctx.font = "700 18px Arial, sans-serif";
  ctx.letterSpacing = "2px";
  ctx.fillText(spec.eyebrow.toUpperCase(), 48, 74);
  ctx.fillStyle = accent;
  ctx.fillRect(48, 96, 70, 10);
  ctx.fillStyle = ink;
  ctx.fillRect(124, 96, 356, 2);

  if (spec.kind === "weather") {
    ctx.fillStyle = "#e5c900";
    ctx.beginPath();
    ctx.arc(398, 208, 54, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = ink;
    ctx.lineWidth = 8;
    for (let angle = 0; angle < Math.PI * 2; angle += Math.PI / 4) {
      ctx.beginPath();
      ctx.moveTo(398 + Math.cos(angle) * 72, 208 + Math.sin(angle) * 72);
      ctx.lineTo(398 + Math.cos(angle) * 90, 208 + Math.sin(angle) * 90);
      ctx.stroke();
    }
    ctx.fillStyle = ink;
    ctx.font = "700 31px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 182);
    ctx.font = "800 164px Arial, sans-serif";
    ctx.fillText(spec.value, 42, 370);
    ctx.font = "800 62px Arial, sans-serif";
    ctx.fillText(spec.unit, 264, 280);
  } else if (spec.kind === "countdown") {
    ctx.fillStyle = ink;
    ctx.font = "700 34px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 180);
    ctx.fillStyle = accent;
    ctx.font = "900 238px Arial, sans-serif";
    ctx.fillText(spec.value, 34, 438);
    ctx.fillStyle = ink;
    ctx.font = "800 58px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.unit, 380, 408);
  } else if (spec.kind === "meeting") {
    ctx.fillStyle = accent;
    ctx.fillRect(48, 140, 432, 112);
    ctx.fillStyle = "#ffffff";
    ctx.font = "800 54px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.value, 74, 215);
    ctx.fillStyle = ink;
    ctx.font = `800 ${fitText(ctx, spec.title, 430, 70)}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.title, 48, 360);
  } else if (spec.kind === "metric") {
    ctx.fillStyle = ink;
    ctx.font = "700 34px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 178);
    ctx.font = "900 176px Arial, sans-serif";
    ctx.fillText(spec.value, 40, 370);
    ctx.font = "800 62px Arial, sans-serif";
    ctx.fillText(spec.unit, 330, 350);
    ctx.strokeStyle = ink;
    ctx.lineWidth = 5;
    ctx.strokeRect(48, 432, 432, 46);
    ctx.fillStyle = accent;
    ctx.fillRect(57, 441, 316, 28);
  } else {
    ctx.fillStyle = ink;
    ctx.font = "700 34px Arial, \"PingFang SC\", sans-serif";
    ctx.fillText(spec.title, 48, 180);
    ctx.fillStyle = accent;
    ctx.fillRect(48, 226, 432, 206);
    ctx.fillStyle = "#ffffff";
    const size = fitText(ctx, spec.value, 380, 74);
    ctx.font = `900 ${size}px Arial, "PingFang SC", sans-serif`;
    ctx.fillText(spec.value, 72, 346);
  }

  ctx.fillStyle = ink;
  ctx.font = "700 24px Arial, \"PingFang SC\", sans-serif";
  ctx.fillText(spec.detail, 48, 552);
  ctx.fillRect(48, 590, 432, 3);
  ctx.fillStyle = accent;
  ctx.beginPath();
  ctx.arc(68, 646, 18, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = ink;
  ctx.font = "700 27px Arial, \"PingFang SC\", sans-serif";
  const footer = spec.footer.length > 24 ? `${spec.footer.slice(0, 24)}…` : spec.footer;
  ctx.fillText(footer, 100, 655);

  ctx.font = "700 16px Arial, sans-serif";
  ctx.fillText("INKLOOP / TODOO 3.7", 48, 724);
  ctx.textAlign = "right";
  ctx.fillText("6-COLOR E-PAPER", 480, 724);
  ctx.textAlign = "left";
}

function MiniScreen({ app }: { app: InkApp }) {
  return (
    <div className={`mini-screen mini-${app.spec.accent}`}>
      <span className="mini-eyebrow">{app.spec.eyebrow}</span>
      <i />
      <b>{app.spec.value}</b>
      <em>{app.spec.unit}</em>
      <small>{app.spec.title}</small>
      <span className="mini-footer">{app.spec.detail}</span>
    </div>
  );
}

function AppCard({ app, onUse, local }: { app: InkApp; onUse: () => void; local?: boolean }) {
  return (
    <article className="app-card">
      <MiniScreen app={app} />
      <div className="app-card-copy">
        <div className="app-card-meta">
          <span>{local ? "本机" : `by ${app.author}`}</span>
          <span>{scheduleLabel(app)}</span>
        </div>
        <h3>{app.title}</h3>
        <p>{app.description}</p>
        <button type="button" onClick={onUse}>
          使用此应用 <span>→</span>
        </button>
      </div>
    </article>
  );
}

export default function InkStudio() {
  const [tab, setTab] = useState<Tab>("studio");
  const [prompt, setPrompt] = useState(starterPrompt);
  const [app, setApp] = useState<InkApp>(starterApp);
  const [localApps, setLocalApps] = useState<InkApp[]>([]);
  const [publicApps, setPublicApps] = useState<InkApp[]>(featuredApps);
  const [generating, setGenerating] = useState(false);
  const [generatorStatus, setGeneratorStatus] = useState<GeneratorStatus>("checking");
  const [generatorModel, setGeneratorModel] = useState("auto");
  const [codeOpen, setCodeOpen] = useState(false);
  const [toast, setToast] = useState<Toast>(null);
  const [deviceName, setDeviceName] = useState<string | null>(null);
  const [deviceStatus, setDeviceStatus] = useState<"idle" | "ready" | "writing" | "scheduled" | "error">("idle");
  const [progress, setProgress] = useState<TodooProgress | null>(null);
  const [scheduleActive, setScheduleActive] = useState(false);
  const [nextRun, setNextRun] = useState<Date | null>(null);
  const [bluetoothSupported, setBluetoothSupported] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const driverRef = useRef<TodooCard | null>(null);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const showToast = useCallback((message: string, tone: ToastTone = "info") => {
    setToast({ message, tone });
    setTimeout(() => setToast(null), 3400);
  }, []);

  useEffect(() => {
    try {
      const stored = JSON.parse(localStorage.getItem(LOCAL_APPS_KEY) ?? "[]") as InkApp[];
      if (Array.isArray(stored)) setLocalApps(stored);
    } catch {
      localStorage.removeItem(LOCAL_APPS_KEY);
    }
  }, []);

  useEffect(() => {
    fetch("/api/generate")
      .then(async (response) => {
        if (!response.ok) throw new Error("generator unavailable");
        return (await response.json()) as { configured?: boolean; model?: string };
      })
      .then((data) => {
        setGeneratorStatus(data.configured ? "online" : "local");
        setGeneratorModel(data.model || "auto");
      })
      .catch(() => setGeneratorStatus("local"));
  }, []);

  useEffect(() => {
    fetch("/api/apps")
      .then(async (response) => {
        if (!response.ok) throw new Error("gallery unavailable");
        return (await response.json()) as { apps?: InkApp[] };
      })
      .then((data) => {
        if (data.apps?.length) setPublicApps([...data.apps, ...featuredApps]);
      })
      .catch(() => undefined);
  }, []);

  useEffect(() => {
    setBluetoothSupported(
      Boolean((navigator as Navigator & { bluetooth?: unknown }).bluetooth && globalThis.isSecureContext),
    );
    const driver = new TodooCard(setProgress);
    driverRef.current = driver;
    driver
      .restoreAuthorizedDevice()
      .then((device) => {
        if (device) {
          setDeviceName(device.name ?? "已授权设备");
          setDeviceStatus("ready");
        }
      })
      .catch(() => undefined);
    return () => driver.disconnect();
  }, []);

  useEffect(() => {
    if (canvasRef.current) drawScreen(canvasRef.current, app.spec);
  }, [app.spec]);

  const generate = async () => {
    if (!prompt.trim()) {
      showToast("先描述你想让屏幕显示什么", "error");
      return;
    }
    setGenerating(true);
    try {
      const response = await fetch("/api/generate", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ prompt }),
      });
      if (!response.ok) throw new Error("生成服务暂时不可用");
      const result = (await response.json()) as {
        app?: InkApp;
        mode?: "llm" | "local";
        model?: string | null;
        warning?: string;
      };
      if (!result.app) throw new Error("生成结果不完整");
      setApp(result.app);
      if (result.mode === "llm") {
        setGeneratorStatus("online");
        setGeneratorModel(result.model || "auto");
        showToast(`已由 ${result.model || "在线模型"} 生成应用`, "success");
      } else {
        setGeneratorStatus("local");
        showToast(result.warning || "已使用本地模板生成", "info");
      }
    } catch (error) {
      setApp(generateInkApp(prompt));
      setGeneratorStatus("local");
      showToast(error instanceof Error ? `${error.message}，已使用本地模板` : "已使用本地模板", "info");
    } finally {
      setGenerating(false);
    }
  };

  const updateSchedule = (scheduleMode: ScheduleMode) => {
    setApp((current) => ({ ...current, scheduleMode }));
  };

  const saveApp = async () => {
    const saved = { ...app, id: app.id.startsWith("starter") ? `app-${Date.now()}` : app.id };
    const next = [saved, ...localApps.filter((item) => item.id !== saved.id)].slice(0, 30);
    setApp(saved);
    setLocalApps(next);
    localStorage.setItem(LOCAL_APPS_KEY, JSON.stringify(next));

    if (saved.isPublic) {
      try {
        const response = await fetch("/api/apps", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify(saved),
        });
        if (!response.ok) throw new Error("publish failed");
        const data = (await response.json()) as { app: InkApp };
        setPublicApps((items) => [data.app, ...items.filter((item) => item.id !== data.app.id)]);
        showToast("已保存到本机，并发布到发现页", "success");
      } catch {
        showToast("已保存到本机；公开发布暂时不可用", "info");
      }
    } else {
      showToast("应用已保存在这台设备上", "success");
    }
  };

  const useApp = (selected: InkApp) => {
    const cloned = {
      ...selected,
      id: `app-${Date.now()}`,
      author: "我",
      isPublic: false,
      createdAt: new Date().toISOString(),
    };
    setApp(cloned);
    setPrompt(cloned.prompt);
    setTab("studio");
    showToast("已复制到创作台，可以继续调整", "success");
  };

  const calculateNextDelay = useCallback((current: InkApp) => {
    if (current.scheduleMode !== "daily") return intervalFor(current);
    const [hour, minute] = current.dailyTime.split(":").map(Number);
    const now = new Date();
    const next = new Date(now);
    next.setHours(hour, minute, 0, 0);
    if (next <= now) next.setDate(next.getDate() + 1);
    return next.getTime() - now.getTime();
  }, []);

  const stopSchedule = useCallback(() => {
    if (timerRef.current) clearTimeout(timerRef.current);
    timerRef.current = null;
    setScheduleActive(false);
    setNextRun(null);
    setDeviceStatus(deviceName ? "ready" : "idle");
    driverRef.current?.disconnect();
  }, [deviceName]);

  const runTransfer = useCallback(async () => {
    const driver = driverRef.current;
    const canvas = canvasRef.current;
    if (!driver || !canvas) return false;
    if (document.visibilityState !== "visible") {
      showToast("页面在后台，已推迟到重新打开后写入", "info");
      return false;
    }
    setDeviceStatus("writing");
    try {
      await driver.writeCanvas(canvas, true);
      setDeviceStatus("ready");
      showToast("帧已发送，墨水屏可能还会显色几分钟", "success");
      return true;
    } catch (error) {
      const message = error instanceof Error ? error.message : "写入失败";
      setDeviceStatus("error");
      showToast(message, "error");
      return false;
    }
  }, [showToast]);

  const scheduleFollowingRun = useCallback(() => {
    const delay = calculateNextDelay(app);
    if (!delay) {
      setScheduleActive(false);
      return;
    }
    const due = new Date(Date.now() + delay);
    setNextRun(due);
    setScheduleActive(true);
    setDeviceStatus("scheduled");
    timerRef.current = setTimeout(async () => {
      const wrote = await runTransfer();
      if (!wrote && document.visibilityState !== "visible") {
        timerRef.current = setTimeout(scheduleFollowingRun, 60_000);
        return;
      }
      scheduleFollowingRun();
    }, delay);
  }, [app, calculateNextDelay, runTransfer]);

  const start = async () => {
    const driver = driverRef.current;
    if (!driver?.supported) {
      showToast("请在 HTTPS 下使用 Android 或桌面 Chromium 打开此网站", "error");
      setDeviceStatus("error");
      return;
    }
    try {
      let device = driver.selectedDevice;
      if (!device) device = await driver.restoreAuthorizedDevice();
      if (!device) device = await driver.requestDevice();
      setDeviceName(device.name ?? "TodooCard");
      const wrote = await runTransfer();
      if (wrote && app.scheduleMode !== "once") scheduleFollowingRun();
    } catch (error) {
      const message = error instanceof Error ? error.message : "没有选择设备";
      showToast(message, "error");
      setDeviceStatus("error");
    }
  };

  const contentTitle = tab === "mine" ? "我的应用" : tab === "explore" ? "发现灵感" : tab === "device" ? "设备中心" : null;

  return (
    <main className="app-shell">
      <aside className="sidebar">
        <button className="brand" type="button" onClick={() => setTab("studio")} aria-label="返回创作台">
          <span className="brand-mark">I</span>
          <span>Inkloop</span>
        </button>
        <nav aria-label="主导航">
          {navItems.map((item) => (
            <button
              type="button"
              key={item.id}
              className={tab === item.id ? "active" : ""}
              onClick={() => setTab(item.id)}
            >
              <span>{item.glyph}</span>
              {item.label}
            </button>
          ))}
        </nav>
        <div className="sidebar-device">
          <span className={`status-dot ${deviceStatus}`} />
          <div>
            <strong>{deviceName ?? "未连接设备"}</strong>
            <small>{deviceName ? "已记住，可自动重连" : "TodooCard · BLE"}</small>
          </div>
        </div>
        <a className="product-link" href="https://p.todoo.tech/?lang=zh" target="_blank" rel="noreferrer">
          产品信息 <span>↗</span>
        </a>
      </aside>

      <section className="workspace">
        <header className="topbar">
          <div>
            <span className="eyebrow">TODOO · 6-COLOR E-PAPER</span>
            <strong>{contentTitle ?? app.title}</strong>
          </div>
          <div className="topbar-actions">
            <span className={`support-chip ${bluetoothSupported ? "ok" : "warn"}`}>
              <i /> {bluetoothSupported ? "蓝牙可用" : "请使用 Chromium"}
            </span>
            {tab === "studio" && (
              <button type="button" className="save-button" onClick={saveApp}>
                保存应用
              </button>
            )}
          </div>
        </header>

        {tab === "studio" && (
          <>
            <div className="studio-grid">
              <section className="prompt-panel panel">
                <div className="panel-heading">
                  <span className="step-number">01</span>
                  <div>
                    <h2>描述你想看到的内容</h2>
                    <p>说人话就好，生成器会补全数据与排版逻辑。</p>
                  </div>
                </div>
                <label htmlFor="app-prompt">应用需求</label>
                <div className="prompt-box">
                  <textarea
                    id="app-prompt"
                    value={prompt}
                    onChange={(event) => setPrompt(event.target.value)}
                    placeholder="例如：每天早上 8 点显示上海天气…"
                    rows={7}
                  />
                  <div className="prompt-counter">{prompt.length} / 300</div>
                </div>
                <div className="suggestions">
                  <span>试试这些</span>
                  {samplePrompts.map((sample) => (
                    <button type="button" key={sample} onClick={() => setPrompt(sample)}>
                      {sample.replace("每天 8 点", "天气").replace("显示", "").slice(0, 10)}
                    </button>
                  ))}
                </div>
                <button className="generate-button" type="button" onClick={generate} disabled={generating}>
                  <span>{generating ? (generatorStatus === "online" ? "模型编码中" : "生成中") : "✦ 生成应用"}</span>
                  <i>{generating ? "•••" : "→"}</i>
                </button>
                <div className="generator-note">
                  <span className={generatorStatus === "online" ? "online" : ""}>LLM</span>
                  <p>
                    {generatorStatus === "checking"
                      ? "正在检查在线编码服务…"
                      : generatorStatus === "online"
                        ? `Tsingfly 在线编码已就绪 · ${generatorModel === "auto" ? "自动选择模型" : generatorModel}`
                        : "等待配置 LLM_API_KEY · 当前自动使用本地模板"}
                  </p>
                </div>
              </section>

              <section className="preview-panel panel">
                <div className="preview-toolbar">
                  <div>
                    <span className="step-number">02</span>
                    <div>
                      <h2>屏幕预览</h2>
                      <p>528 × 792 · 实际六色色板</p>
                    </div>
                  </div>
                  <span className="scale-chip">50%</span>
                </div>
                <div className="canvas-stage">
                  <div className="device-shadow" />
                  <div className="device-frame">
                    <div className="device-label">TODOO</div>
                    <canvas ref={canvasRef} width={528} height={792} aria-label="电子墨水屏预览" />
                    <div className="device-port" />
                  </div>
                </div>
                <div className="palette-strip" aria-label="屏幕支持六种颜色">
                  {[
                    ["黑", "#111"],
                    ["白", "#f6f2df"],
                    ["黄", "#e5c900"],
                    ["红", "#dc3f2f"],
                    ["蓝", "#2756c7"],
                    ["绿", "#087c4e"],
                  ].map(([label, color]) => (
                    <span key={label}><i style={{ background: color }} />{label}</span>
                  ))}
                </div>
              </section>

              <section className="settings-panel panel">
                <div className="panel-heading compact">
                  <span className="step-number">03</span>
                  <div>
                    <h2>保存与刷新</h2>
                    <p>选择写入频率与分享范围。</p>
                  </div>
                </div>
                <label>刷新计划</label>
                <div className="schedule-options">
                  {[
                    ["once", "单次写入", "立即执行一次"],
                    ["hourly", "每小时", "整点后循环"],
                    ["daily", "每天", app.dailyTime],
                    ["custom", "自定义", `${app.customMinutes} 分钟`],
                  ].map(([value, title, detail]) => (
                    <button
                      type="button"
                      key={value}
                      onClick={() => updateSchedule(value as ScheduleMode)}
                      className={app.scheduleMode === value ? "selected" : ""}
                    >
                      <i>{app.scheduleMode === value ? "●" : "○"}</i>
                      <span><strong>{title}</strong><small>{detail}</small></span>
                    </button>
                  ))}
                </div>
                {app.scheduleMode === "daily" && (
                  <div className="inline-field">
                    <label htmlFor="daily-time">每天执行时间</label>
                    <input
                      id="daily-time"
                      type="time"
                      value={app.dailyTime}
                      onChange={(event) => setApp((current) => ({ ...current, dailyTime: event.target.value }))}
                    />
                  </div>
                )}
                {app.scheduleMode === "custom" && (
                  <div className="inline-field">
                    <label htmlFor="custom-minutes">间隔分钟（建议 ≥ 5）</label>
                    <input
                      id="custom-minutes"
                      type="number"
                      min={5}
                      value={app.customMinutes}
                      onChange={(event) =>
                        setApp((current) => ({ ...current, customMinutes: Math.max(5, Number(event.target.value)) }))
                      }
                    />
                  </div>
                )}
                <div className="sharing-row">
                  <div>
                    <strong>公开到发现页</strong>
                    <small>其他人可以复制并使用</small>
                  </div>
                  <button
                    type="button"
                    role="switch"
                    aria-checked={app.isPublic}
                    className={`switch ${app.isPublic ? "on" : ""}`}
                    onClick={() => setApp((current) => ({ ...current, isPublic: !current.isPublic }))}
                  >
                    <span />
                  </button>
                </div>
                <button type="button" className="code-toggle" onClick={() => setCodeOpen((open) => !open)}>
                  <span><i>&lt;/&gt;</i> 查看生成逻辑</span><b>{codeOpen ? "−" : "+"}</b>
                </button>
                {codeOpen && <pre className="code-preview"><code>{app.code}</code></pre>}
              </section>
            </div>

            <div className="run-dock">
              <div className="run-status">
                <span className={`run-icon ${deviceStatus}`}>{deviceStatus === "writing" ? "↻" : "⌁"}</span>
                <div>
                  <strong>
                    {deviceStatus === "writing" ? progress?.message ?? "正在写入" : scheduleActive ? "定时任务运行中" : deviceName ?? "准备写入 TodooCard"}
                  </strong>
                  <small>
                    {nextRun
                      ? `下次执行 ${nextRun.toLocaleString("zh-CN", { hour: "2-digit", minute: "2-digit" })}`
                      : deviceName
                        ? "已授权设备不会再次弹出选择器"
                        : "首次需要手动选择设备 · 之后自动重连"}
                  </small>
                </div>
              </div>
              {deviceStatus === "writing" && (
                <div className="transfer-progress"><i style={{ width: `${progress?.percent ?? 0}%` }} /></div>
              )}
              <div className="run-actions">
                {scheduleActive && <button type="button" className="stop-button" onClick={stopSchedule}>停止任务</button>}
                <button type="button" className="start-button" onClick={start} disabled={deviceStatus === "writing"}>
                  <span>{deviceStatus === "writing" ? "正在写入" : scheduleActive ? "立即再写一次" : "开始写入"}</span>
                  <i>→</i>
                </button>
              </div>
            </div>
          </>
        )}

        {tab === "mine" && (
          <section className="collection-view">
            <div className="collection-hero">
              <span className="eyebrow">LOCAL LIBRARY</span>
              <h1>留在你设备里的应用</h1>
              <p>这些应用保存在浏览器本机，不上传个人数据。清理浏览器数据会一并删除。</p>
              <button type="button" onClick={() => setTab("studio")}>＋ 创建新应用</button>
            </div>
            {localApps.length ? (
              <div className="card-grid">
                {localApps.map((item) => <AppCard key={item.id} app={item} local onUse={() => useApp(item)} />)}
              </div>
            ) : (
              <div className="empty-state">
                <span>▦</span><h2>还没有保存的应用</h2><p>在创作台生成并保存，第一个应用就会出现在这里。</p>
              </div>
            )}
          </section>
        )}

        {tab === "explore" && (
          <section className="collection-view explore-view">
            <div className="collection-hero split">
              <div>
                <span className="eyebrow">PUBLIC GALLERY</span>
                <h1>把别人的灵感，变成你的屏幕</h1>
                <p>所有应用都能一键复制到创作台，再按自己的数据与频率修改。</p>
              </div>
              <div className="gallery-stat"><b>{publicApps.length}</b><span>公开应用</span></div>
            </div>
            <div className="filter-row"><button className="active">精选</button><button>生活</button><button>效率</button><button>数据</button></div>
            <div className="card-grid">
              {publicApps.map((item, index) => <AppCard key={`${item.id}-${index}`} app={item} onUse={() => useApp(item)} />)}
            </div>
          </section>
        )}

        {tab === "device" && (
          <section className="device-view">
            <div className="device-hero">
              <div>
                <span className="eyebrow">WEB BLUETOOTH · FEF0 / FEF1 / FEF2</span>
                <h1>一次选择，页面打开时自动写入。</h1>
                <p>首选 Android 或桌面版 Chromium。首次必须由点击唤起设备选择；授权后本页面会保留设备，并优先恢复历史授权。</p>
              </div>
              <div className="connection-card">
                <span className={`status-orb ${deviceName ? "connected" : ""}`}>⌁</span>
                <strong>{deviceName ?? "TodooCard 未连接"}</strong>
                <small>{deviceName ? "授权已保存 · 等待写入" : "NEMR99803797 / PICKSMART · 528 × 792"}</small>
                <button type="button" onClick={start}>{deviceName ? "测试写入" : "选择设备"}</button>
              </div>
            </div>
            <div className="feasibility-grid">
              <article className="verdict-card yes">
                <span>可以做到</span>
                <h2>同一会话自动重连</h2>
                <p>保留 BluetoothDevice，定时到点后连接 GATT、写入、断开。支持 getDevices() 时，下次访问也可找回已授权设备。</p>
              </article>
              <article className="verdict-card caution">
                <span>有条件</span>
                <h2>使用 setTimeout 调度</h2>
                <p>每次传输完成后再计算下一次时间，避免 setInterval 在写屏耗时较长时产生重叠任务。</p>
              </article>
              <article className="verdict-card no">
                <span>无法保证</span>
                <h2>浏览器关闭后无人值守</h2>
                <p>后台节流、系统休眠和设备唤醒都会打断任务；Service Worker 也不能在后台调用 Web Bluetooth。</p>
              </article>
            </div>
            <div className="protocol-table">
              <div><span>传输协议</span><strong>BLE GATT</strong></div>
              <div><span>设备服务</span><strong>FEF0 · FEF1 · FEF2</strong></div>
              <div><span>单次数据</span><strong>219,120 bytes · 913 包</strong></div>
              <div><span>显色时间</span><strong>复杂画面可能约 3 分钟</strong></div>
            </div>
            <div className="reliability-note">
              <span>长期无人值守建议</span>
              <p>如果必须在浏览器关闭后也准时刷新，建议把调度与蓝牙下沉到常开网关（树莓派 / ESP32 / 原生 App），网页只负责编辑应用与下发计划。</p>
            </div>
          </section>
        )}
      </section>

      {toast && <div className={`toast ${toast.tone}`} role="status"><span>{toast.tone === "success" ? "✓" : toast.tone === "error" ? "!" : "i"}</span>{toast.message}</div>}
    </main>
  );
}
