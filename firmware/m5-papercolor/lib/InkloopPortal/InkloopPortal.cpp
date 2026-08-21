#include "InkloopPortal.h"

#include <stdlib.h>

#include <algorithm>
#include <sstream>

#include "PortalEncoding.h"
#include "PortalSecurityPrimitives.h"

namespace inkloop {
namespace portal {

namespace {

static const size_t kMaximumRequestBodyBytes = 4096;
static const size_t kMaximumAssistantPromptBytes = 512;
static const size_t kMaximumNegativePromptBytes = 384;
static const size_t kMaximumImagePromptTemplateBytes = 512;
static const size_t kMaximumManualAigcPromptBytes = 1024;
static const size_t kMaximumPortalScriptBytes = 24576;
static const uint64_t kConfirmationLifetimeSeconds = 30;
static const uint32_t kMaximumRateWindowSeconds = 3600;

const char kPortalCss[] = R"PORTALCSS(
:root{color-scheme:light;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#172019;background:#f3f5f1}*{box-sizing:border-box}body{margin:0;background:#f3f5f1}main{width:min(920px,100%);margin:auto;padding:24px 16px 64px}h1{font-size:clamp(1.7rem,5vw,2.5rem);margin:0 0 18px}h2{font-size:1.15rem;margin:0 0 14px}section{background:#fff;border:1px solid #dfe5dc;border-radius:16px;padding:18px;margin:14px 0;box-shadow:0 6px 22px rgba(20,35,24,.05)}p{line-height:1.55}.muted{color:#667068}.notice{padding:12px 14px;border-radius:10px;background:#eef5ea;border-left:4px solid #4b7f45}.warning{background:#fff4dd;border-left-color:#c47813}.code-help strong{color:#111}.status{min-height:24px;padding:10px 12px;border-radius:10px;background:#edf1ec}.status[data-kind="error"]{background:#fde9e6;color:#9d261d}.status[data-kind="success"]{background:#e5f5e8;color:#17642c}form{display:grid;gap:12px;margin:12px 0}label{display:grid;gap:6px;font-weight:600}input,select,textarea,button{font:inherit}input,select,textarea{width:100%;padding:10px 12px;border:1px solid #b9c4b9;border-radius:10px;background:#fff}textarea{min-height:92px;resize:vertical}button{border:0;border-radius:10px;padding:11px 16px;background:#172019;color:#fff;font-weight:700;cursor:pointer}button:hover{background:#2b3a2d}button:disabled{opacity:.55;cursor:wait}button.danger{background:#a32b20}.actions{display:flex;gap:10px;flex-wrap:wrap}.inline{display:inline-grid;margin:6px 6px 6px 0}ul{list-style:none;padding:0;display:grid;gap:10px}li{padding:12px;border:1px solid #e3e7e0;border-radius:10px;overflow-wrap:anywhere}.myai-qr a{font-weight:700}.grid{display:grid;gap:14px}.access-groups{display:flex;gap:6px;flex-wrap:wrap}.access-groups span{font-family:ui-monospace,SFMono-Regular,monospace;padding:4px 7px;background:#edf1ec;border-radius:6px}.upload-preview{display:none;max-width:330px;max-height:220px;border:1px solid #b9c4b9;background:#fff}.upload-preview[data-ready="1"]{display:block}.album-grid{grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:14px}.album-card{display:grid;align-content:start;gap:9px;padding:10px}.album-thumb{display:block;width:100%;aspect-ratio:2/3;object-fit:contain;background:#f4f4ee;border:1px solid #dfe5dc;border-radius:8px}.album-meta{margin:0}.album-source{display:inline-flex;width:max-content;padding:3px 8px;border-radius:999px;background:#e8f1fb;color:#24547d;font-size:.75rem;font-weight:700}.album-actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;width:100%}.album-actions .inline{min-width:0;margin:0}.album-actions .inline:only-child{grid-column:1/-1}.album-actions button{width:100%;min-width:0;padding:10px 8px}.tabs{display:flex;gap:8px;overflow-x:auto;margin:16px 0;padding:4px;background:#e6ebe4;border-radius:12px}.tabs button{white-space:nowrap;background:transparent;color:#38443a}.tabs button.active{background:#172019;color:#fff}.tab-panel{display:none}.tab-panel.active{display:block}@media(min-width:720px){.grid.two{grid-template-columns:1fr 1fr}.settings-grid{grid-template-columns:1fr 1fr}.settings-grid .wide{grid-column:1/-1}}
)PORTALCSS";

const char kSettingsCss[] = R"PORTALCSS(
.settings-stack{display:grid;gap:16px}.settings-stack section{margin:0}.section-heading{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;flex-wrap:wrap}.section-heading h2{margin:0}.effect-badge{display:inline-flex;align-items:center;min-height:28px;padding:4px 9px;border-radius:999px;font-size:.78rem;font-weight:700;background:#e9efe7;color:#36513a}.effect-badge.preview{background:#e8f1fb;color:#24547d}.effect-badge.next{background:#f0ecfb;color:#55418a}.effect-badge.restart{background:#fff0d8;color:#895710}.setting-help{margin:8px 0 14px;color:#667068}.settings-grid button[type="submit"]{justify-self:start;min-height:44px}.settings-grid .wide-action{grid-column:1/-1}.maintenance{border-color:#ead9d4;background:#fffdfa}.maintenance .danger{justify-self:start}.field-note{font-weight:400;color:#667068}.setting-divider{height:1px;background:#e3e7e0;margin:4px 0}.timing-list{margin:8px 0 0;padding-left:20px;display:grid;gap:6px}.timing-list li{padding:0;border:0;border-radius:0;overflow:visible}@media(max-width:719px){.settings-grid{grid-template-columns:1fr}.settings-grid .wide,.settings-grid .wide-action{grid-column:auto}.section-heading{display:grid}}
)PORTALCSS";

const char kLoginScript[] = R"PORTALJS(
(()=>{"use strict";const f=document.querySelector("#login-form"),s=document.querySelector("#page-status");if(!f||!s)return;const show=(m,k)=>{s.textContent=m;s.dataset.kind=k||"info"};f.addEventListener("submit",async e=>{e.preventDefault();const b=f.querySelector("button"),n=f.elements.nonce.value;if(!n||n.length<8||n.length>63){show("请输入完整的本地管理密码（8–63位）。","error");return}b.disabled=true;show("正在验证本地管理密码…","info");const c=new AbortController(),t=setTimeout(()=>c.abort(),10000);try{const r=await fetch("/api/session",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:new URLSearchParams({nonce:n}),signal:c.signal});const l=Number(r.headers.get("content-length")||0);if(l>4096)throw new Error("response_too_large");const x=await r.text();if(x.length>4096)throw new Error("response_too_large");let j={};try{j=JSON.parse(x)}catch(_){throw new Error("invalid_response")}if(!r.ok||!j.ok)throw new Error(j.error||"login_failed");show("验证成功，正在进入设置…","success");location.replace("/")}catch(err){let m="登录失败，请稍后重试。";if(err.name==="AbortError")m="设备响应超时，请重试。";else if(err.message==="invalid_bootstrap_nonce")m="本地管理密码不正确，请重新输入。";else if(err.message==="rate_limited")m="尝试次数过多，请稍后再试。";show(m,"error");b.disabled=false}finally{clearTimeout(t)}})})();
)PORTALJS";

const char kDashboardScript[] = R"PORTALJS(
(()=>{"use strict";const meta=document.querySelector('meta[name="inkloop-csrf"]'),status=document.querySelector("#page-status");if(!meta||!status)return;const csrf=meta.content,errText={app_not_registered:"inkloop 应用尚未在 MyAI 注册，暂时无法生成六位绑定码。",device_busy:"设备正在写屏、生成图片、播放语音或测试灯光，请稍后重试。",audio_busy:"语音或麦克风正在使用，请稍后再试听。",myai_not_activated:"请先完成 MyAI 绑定与激活。",invalid_aigc_prompt:"请输入1–1024字节的图片主题。",album_item_not_found:"相册中找不到这张图片，请刷新页面。",page_queue_failed:"图片上屏任务未能排队，请稍后重试。",confirmation_expired:"确认已超时，请重新发起操作。",origin_or_csrf_rejected:"安全校验失败，请刷新页面后重试。",rate_limited:"操作过于频繁，请稍后重试。",invalid_local_management_password:"本地管理密码需为8–63位。",local_password_mismatch:"两次输入的本地管理密码不一致。",image_prompt_template_invalid:"AIGC 图片提示词模板不能为空且不能超过512字节。"};const show=(m,k)=>{status.textContent=m;status.dataset.kind=k||"info";status.scrollIntoView({block:"nearest"})};async function read(r){const l=Number(r.headers.get("content-length")||0);if(l>4096)throw new Error("response_too_large");const x=await r.text();if(x.length>4096)throw new Error("response_too_large");let j={};try{j=JSON.parse(x)}catch(_){throw new Error("invalid_response")}if(!r.ok||j.ok===false){const e=j.error||("http_"+r.status);throw new Error(e)}return j}async function api(path,body){const c=new AbortController(),t=setTimeout(()=>c.abort(),10000);try{return await read(await fetch(path,{method:body===undefined?"GET":"POST",headers:body===undefined?{}:{"Content-Type":"application/x-www-form-urlencoded","X-Inkloop-CSRF":csrf},body,signal:c.signal}))}finally{clearTimeout(t)}}function message(e){return e.name==="AbortError"?"设备响应超时，请重试。":(errText[e.message]||("操作失败："+e.message))}const tabs=[...document.querySelectorAll("[data-tab]")],panels=[...document.querySelectorAll("[data-tab-panel]")];function openTab(name){tabs.forEach(x=>x.classList.toggle("active",x.dataset.tab===name));panels.forEach(x=>x.classList.toggle("active",x.dataset.tabPanel===name));try{localStorage.setItem("inkloop-tab",name)}catch(_){}}tabs.forEach(x=>x.addEventListener("click",()=>openTab(x.dataset.tab)));if(tabs.length)openTab((()=>{try{return localStorage.getItem("inkloop-tab")||"device"}catch(_){return"device"}})());const volume=document.querySelector('input[name="volume"]'),volumeOut=document.querySelector("#volume-value");if(volume&&volumeOut){let previewing=false;const sync=()=>{volumeOut.textContent=volume.value+"%"};volume.addEventListener("input",sync);volume.addEventListener("change",async()=>{if(previewing)return;previewing=true;try{const value=volume.value;show("正在试听 "+value+"% 音量…","info");await api("/api/audio/preview",new URLSearchParams({volume:value}));show("音量试听已开始；保存设置后才会正式生效。","success")}catch(x){show(message(x),"error")}finally{previewing=false}});sync()}function params(form){const p=new URLSearchParams(new FormData(form));p.delete("_csrf");return p}function actionName(v){return v==="format_sd"?"格式化 TF / SD":v==="clear_album"?"清空用户相册":"删除这张图片"}async function waitPhysical(){for(let i=0;i<34;i++){await new Promise(r=>setTimeout(r,900));const j=await api("/api/state"),p=j.physicalConfirmation||{};if(p.state==="awaiting_device_button")continue;if(p.state==="complete"){show("设备顶部按钮确认成功，操作已完成。","success");setTimeout(()=>location.reload(),700);return}if(p.state==="failed")throw new Error(p.error||"confirmed_operation_failed");if(p.state==="expired")throw new Error("confirmation_expired")}throw new Error("confirmation_expired")}async function destructive(form){const prep=await api("/api/actions/prepare",params(form));if(!confirm("二次确认：确定要"+actionName(prep.action)+"吗？此操作不可撤销。")){show("已取消，设备上不会执行任何操作。","info");return}const p=new URLSearchParams({confirmation_id:prep.confirmationId,phrase:prep.requiredPhrase});await api("/api/actions/confirm",p);show("请在30秒内短按设备顶部按钮（语音键 BtnC / GPIO1）确认；不要按左右翻页键。","info");await waitPhysical()}document.querySelectorAll("form[data-portal]").forEach(form=>form.addEventListener("submit",async e=>{e.preventDefault();const b=form.querySelector("button");if(!b||b.disabled)return;b.disabled=true;try{if(form.dataset.destructive==="1")await destructive(form);else{const j=await api(form.action.replace(location.origin,""),params(form));if(form.action.endsWith("/api/led/test"))show("灯光测试已开始（约4.6秒）：先双灯白亮0.8秒验证供电和总线，再由语音角色蓝/青闪2次、图片角色黄/橙闪2次；结束后恢复当前状态。","success");else if(form.action.endsWith("/api/album/display"))show("已排队上屏；彩色墨水屏刷新需要一些时间，请勿重复点击。","success");else if(form.action.endsWith("/api/aigc/generate"))show("MyAI 已开始生成；完成后会自动缓存并上屏。","success");else if(form.action.endsWith("/api/settings")){if(j.localManagementPassword&&j.localManagementPassword.restartRequired)show("设置已保存。本地管理密码将在重启后同时用于 Settings Wi‑Fi 和网页登录；请记住新密码。","success");else{show("设置已保存。","success");setTimeout(()=>location.reload(),700)}}else if(form.action.endsWith("/api/tutorial/restart")){show("语音教程已重新排队。","success");setTimeout(()=>location.reload(),700)}else if(form.action.endsWith("/api/onboarding/myai/start")){show("已发起 MyAI 配对请求；只有拿到六位码后才会显示二维码。","success");setTimeout(()=>location.reload(),1000)}else show(j.state||"操作成功。","success")}}catch(x){show(message(x),"error")}finally{b.disabled=false}}));let polls=0;const timer=setInterval(async()=>{if(++polls>60){clearInterval(timer);return}try{const j=await api("/api/state"),m=j.myAi||{},el=document.querySelector("#myai-status");if(el&&m.message)el.textContent=m.message;if(j.onboarding&&j.onboarding.onboardingCode&&!document.querySelector(".myai-qr"))location.reload()}catch(_){clearInterval(timer)}},3000)})();
)PORTALJS";

const char kLedBrightnessScript[] = R"PORTALJS(
(()=>{"use strict";const slider=document.querySelector('input[name="led_brightness"]'),output=document.querySelector("#led-brightness-value");if(!slider||!output)return;const sync=()=>{output.textContent=slider.value+"%"};slider.addEventListener("input",sync);sync()})();
)PORTALJS";

const char kDisplayStatusScript[] = R"PORTALJS(
(()=>{"use strict";const status=document.querySelector("#page-status");if(!status)return;let tracking=false;async function poll(){try{const response=await fetch("/api/state",{headers:{Accept:"application/json"}});if(!response.ok)return;const text=await response.text();if(text.length>4096)return;const state=JSON.parse(text).display?.state;if(state==="refreshing"){tracking=true;status.textContent="屏幕正在刷新，请稍候…";status.dataset.kind="display"}else if(state==="cooldown"){tracking=true;status.textContent="屏幕刷新已完成，设备正在进行 30 秒保护冷却。";status.dataset.kind="display"}else if(state==="ready"&&tracking){tracking=false;status.textContent="屏幕已就绪。";status.dataset.kind="success"}}catch(_){}}poll();setInterval(poll,2000)})();
)PORTALJS";

const char kSettingsScript[] = R"PORTALJS(
(()=>{"use strict";
const meta=document.querySelector('meta[name="inkloop-csrf"]'),status=document.querySelector("#page-status");
if(!meta||!status)return;
const show=(message,kind)=>{status.textContent=message;status.dataset.kind=kind||"info";status.scrollIntoView({block:"nearest"})};
const friendly={device_busy:"设备当前忙碌，LED 检测已经保留；空闲后会自动开始。",led_count_must_be_two:"未检测到两颗 RGB 灯，请检查硬件。",led_role_test_failed:"RGB 检测无法启动，请查看诊断日志。",settings_not_saved:"设置没有保存，请重试。"};
async function submit(form){
  const button=form.querySelector('button[type="submit"]');if(!button||button.disabled)return;
  button.disabled=true;const old=button.textContent;button.textContent="正在保存…";
  try{
    const body=new URLSearchParams(new FormData(form));body.delete("_csrf");
    const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),12000);
    let response;
    try{response=await fetch("/api/settings",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded","X-Inkloop-CSRF":meta.content},body,signal:controller.signal})}finally{clearTimeout(timer)}
    const text=await response.text();if(text.length>4096)throw new Error("response_too_large");
    let json={};try{json=JSON.parse(text)}catch(_){throw new Error("invalid_response")}
    if(!response.ok||json.ok===false)throw new Error(json.error||("http_"+response.status));
    if(json.ledDiagnosticRequested){
      if(!json.ledDiagnosticAccepted)throw new Error(json.ledDiagnosticError||"led_role_test_failed");
      show("声音与指示灯设置已保存。RGB 检测已排队：双灯白亮，然后依次蓝/青、黄/橙闪烁；设备忙时会在空闲后自动开始。","success");
    }else if(json.localManagementPassword&&json.localManagementPassword.restartRequired){
      show("本地访问密码已保存，重启设备后用于 Settings Wi-Fi 和网页登录。","success");
    }else{
      show(form.dataset.success||"设置已保存。","success");
    }
    if(form.dataset.reload==="1")setTimeout(()=>location.reload(),900);
  }catch(error){
    const message=error.name==="AbortError"?"设备响应超时，请重试。":(friendly[error.message]||("保存失败："+error.message));show(message,"error");
  }finally{button.disabled=false;button.textContent=old}
}
document.addEventListener("submit",event=>{const form=event.target.closest&&event.target.closest("form[data-settings-group]");if(!form)return;event.preventDefault();event.stopImmediatePropagation();submit(form)},true);
})();
)PORTALJS";

const char kAlbumUploadScript[] = R"PORTALJS(
(()=>{"use strict";const form=document.querySelector("#album-upload-form"),input=document.querySelector("#album-upload-file"),canvas=document.querySelector("#album-upload-preview"),info=document.querySelector("#album-upload-info"),button=document.querySelector("#album-upload-button"),status=document.querySelector("#page-status"),capacity=document.querySelector("#album-capacity"),meta=document.querySelector('meta[name="inkloop-csrf"]');if(!form||!input||!canvas||!info||!button||!status||!capacity||!meta)return;const max=1500000,targetW=Number(form.dataset.width),targetH=Number(form.dataset.height),backend=capacity.dataset.backend,free=Number(capacity.dataset.free||0);let blob=null,name="upload.png";const show=(m,k)=>{status.textContent=m;status.dataset.kind=k||"info";status.scrollIntoView({block:"nearest"})},bytes=n=>n>=1048576?(n/1048576).toFixed(1)+" MiB":n>=1024?Math.ceil(n/1024)+" KiB":n+" B";function fail(m){blob=null;button.disabled=true;canvas.dataset.ready="0";info.textContent=m;show(m,"error")}input.addEventListener("change",()=>{const file=input.files&&input.files[0];if(!file){fail("请选择图片。");return}if(file.size>12582912){fail("源图片超过12 MiB，请先压缩。");return}const url=URL.createObjectURL(file),img=new Image;img.onload=()=>{try{if(!img.width||!img.height||img.width>8192||img.height>8192||img.width*img.height>32000000)throw new Error("图片像素过大，请先缩小。");canvas.width=targetW;canvas.height=targetH;const ctx=canvas.getContext("2d",{alpha:false});ctx.fillStyle="#fff";ctx.fillRect(0,0,targetW,targetH);const scale=Math.max(targetW/img.width,targetH/img.height),w=img.width*scale,h=img.height*scale;ctx.drawImage(img,(targetW-w)/2,(targetH-h)/2,w,h);canvas.toBlob(b=>{URL.revokeObjectURL(url);if(!b){fail("浏览器无法转换 PNG。");return}if(b.size>max){fail("转换后的 PNG 为 "+bytes(b.size)+"，超过设备 1.5 MB 上限；请换更简单的图片。");return}blob=b;name=(file.name||"upload").replace(/[^A-Za-z0-9 ._-]/g,"_").slice(0,59)+".png";canvas.dataset.ready="1";button.disabled=false;info.textContent="预计 PNG："+bytes(b.size)+"；目标后端："+backend+"；当前剩余："+bytes(free)+"。请确认裁切预览。"},"image/png") }catch(e){URL.revokeObjectURL(url);fail(e.message)}};img.onerror=()=>{URL.revokeObjectURL(url);fail("无法读取这张图片。")} ;img.src=url});form.addEventListener("submit",async e=>{e.preventDefault();if(!blob||blob.size>max)return;if(!confirm("确认按当前预览裁切并上传到设备相册？"))return;button.disabled=true;show("正在上传并写入相册，请勿翻页或生成图片…","info");const data=new FormData;data.append("image",blob,name);const c=new AbortController,t=setTimeout(()=>c.abort(),90000);try{const r=await fetch("/api/album/upload",{method:"POST",headers:{"X-Inkloop-CSRF":meta.content,"X-Inkloop-Image-Bytes":String(blob.size)},body:data,signal:c.signal}),x=await r.text();if(x.length>4096)throw new Error("response_too_large");let j={};try{j=JSON.parse(x)}catch(_){throw new Error("invalid_response")}if(!r.ok||!j.ok)throw new Error(j.error||("http_"+r.status));const album=await fetch("/api/album",{signal:c.signal}),albumText=await album.text();if(albumText.length>12288)throw new Error("album_response_too_large");const state=JSON.parse(albumText);if(!album.ok)throw new Error(state.error||"album_refresh_failed");const total=Math.max(0,Number(state.storage.activeTotalBytes||0)),remaining=Math.max(0,Math.min(total,Number(state.storage.activeFreeBytes||0))),used=total-remaining,mounted=state.storage.activeMounted===true&&state.storage.activeWritable===true,active=String(state.storage.activeBackend||"unavailable");capacity.dataset.free=String(remaining);capacity.dataset.total=String(total);capacity.dataset.backend=active;capacity.classList.toggle("warning",!mounted);capacity.textContent=mounted?"实际后端："+active+" · 图片数："+state.page.totalItems+" · 已用："+bytes(used)+" · 剩余："+bytes(remaining)+" · 总计："+bytes(total):"实际后端："+active+" · 图片数："+state.page.totalItems+" · 未挂载或不可写，需要恢复后才能上传。";show("图片上传成功，相册与容量已刷新。","success");setTimeout(()=>location.reload(),900)}catch(x){show(x.name==="AbortError"?"上传超时，临时文件已中止或将在恢复时清理。":"上传失败："+x.message,"error");button.disabled=false}finally{clearTimeout(t)}})})();
)PORTALJS";

const char kAlbumRenderScript[] = R"PORTALJS(
(()=>{const q=s=>document.querySelector(s),m=q('meta[name="inkloop-csrf"]'),z=q("#page-status");for(const s of document.querySelectorAll("[data-album-render]")){const v=s.dataset.current;s.textContent="";for(const o of [["official-quality","官方"],["classic-six-color","经典六色"],["reflectance-photo","照片"],["solid-clean","纯色"]])s.add(new Option(o[1],o[0],o[0]===v,o[0]===v));s.onchange=async()=>{s.disabled=true;try{const r=await fetch("/api/album/render",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded","X-Inkloop-CSRF":m.content},body:new URLSearchParams({asset_id:s.dataset.asset,render_strategy:s.value})}),x=await r.text();if(x.length>4096)throw Error("response_too_large");const j=JSON.parse(x);if(!r.ok||!j.ok)throw Error(j.error);s.dataset.current=s.value;z.textContent="渲染方式已保存，下次上屏生效。";z.dataset.kind="success"}catch(e){s.value=s.dataset.current;z.textContent="保存失败："+e.message;z.dataset.kind="error"}s.disabled=false}}})();
)PORTALJS";

bool strictUnsigned(const std::string& text, uint32_t* output) {
  if (!output || text.empty() || text.size() > 10) return false;
  uint64_t value = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] < '0' || text[index] > '9') return false;
    value = value * 10U + static_cast<uint64_t>(text[index] - '0');
    if (value > 0xffffffffULL) return false;
  }
  *output = static_cast<uint32_t>(value);
  return true;
}

const char* storageTargetName(StorageTarget target) {
  switch (target) {
    case StorageTarget::Automatic: return "auto";
    case StorageTarget::Internal: return "internal";
    case StorageTarget::SdCard: return "sd";
  }
  return "auto";
}

const char* refreshModeName(RefreshMode mode) {
  switch (mode) {
    case RefreshMode::OfficialQuality: return "official-quality";
    case RefreshMode::ExperimentalSixColor: return "classic-six-color";
    case RefreshMode::ReflectancePhoto: return "reflectance-photo";
    case RefreshMode::SolidClean: return "solid-clean";
  }
  return "official-quality";
}

bool validRenderStrategyName(const std::string& value) {
  return value == "official-quality" || value == "classic-six-color" ||
      value == "reflectance-photo" || value == "solid-clean";
}

const char* powerModeName(PowerMode mode) {
  return mode == PowerMode::Battery ? "battery" : "compatibility";
}

const char* activeStorageName(ActiveStorageBackend backend) {
  switch (backend) {
    case ActiveStorageBackend::Internal: return "internal";
    case ActiveStorageBackend::SdCard: return "sd";
    case ActiveStorageBackend::Unavailable: return "unavailable";
  }
  return "unavailable";
}

std::string humanBytes(uint64_t bytes) {
  std::ostringstream output;
  if (bytes >= 1024ULL * 1024ULL * 1024ULL)
    output << (bytes / (1024ULL * 1024ULL * 1024ULL)) << " GiB";
  else if (bytes >= 1024ULL * 1024ULL)
    output << (bytes / (1024ULL * 1024ULL)) << " MiB";
  else if (bytes >= 1024ULL)
    output << (bytes / 1024ULL) << " KiB";
  else
    output << bytes << " B";
  return output.str();
}

const char* destructiveActionName(DestructiveAction action) {
  switch (action) {
    case DestructiveAction::DeleteAsset: return "delete_asset";
    case DestructiveAction::ClearAlbum: return "clear_album";
    case DestructiveAction::FormatSdCard: return "format_sd";
  }
  return "unknown";
}

bool safeAccessValue(const std::string& value) {
  if (value.size() < 16 || value.size() > 128) return false;
  for (size_t index = 0; index < value.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(value[index]);
    if (character <= 0x20 || character >= 0x7f || character == ';' ||
        character == ',' || character == '"' || character == '\\') {
      return false;
    }
  }
  return true;
}

bool isMutationMethod(const std::string& method) {
  return method == "POST" || method == "PUT" || method == "PATCH" ||
      method == "DELETE";
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
  const size_t maximum = left.size() > right.size() ? left.size() : right.size();
  unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
  for (size_t index = 0; index < maximum; ++index) {
    const unsigned char leftByte = index < left.size()
        ? static_cast<unsigned char>(left[index]) : 0;
    const unsigned char rightByte = index < right.size()
        ? static_cast<unsigned char>(right[index]) : 0;
    difference |= static_cast<unsigned int>(leftByte ^ rightByte);
  }
  return difference == 0;
}

std::string cookieValue(const std::string& cookie, const std::string& name) {
  size_t start = 0;
  while (start < cookie.size()) {
    while (start < cookie.size() && (cookie[start] == ' ' || cookie[start] == ';')) ++start;
    const size_t end = cookie.find(';', start);
    const std::string item = cookie.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const size_t equals = item.find('=');
    if (equals != std::string::npos && item.substr(0, equals) == name) {
      return item.substr(equals + 1);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return std::string();
}

std::string originHost(const std::string& origin) {
  const size_t scheme = origin.find("://");
  if (scheme == std::string::npos || scheme == 0) return std::string();
  const size_t hostStart = scheme + 3;
  const size_t path = origin.find('/', hostStart);
  if (path != std::string::npos || hostStart >= origin.size()) return std::string();
  return origin.substr(hostStart);
}

std::string boolJson(bool value) { return value ? "true" : "false"; }

std::string typedMyAiState(
    const std::string& reported,
    const OnboardingState& onboarding) {
  static const char* known[] = {
      "app_not_registered", "unconfigured", "pairing", "bound", "inactive",
      "auth_rejected", "credential_recovery", "recovery", "offline", "error"};
  // Once both services are durably bound, a runtime authorization failure
  // must never regress the UI to "unconfigured" or invite another pairing.
  if (onboarding.terminalBindingComplete()) {
    if (reported == "bound") return "bound";
    if (reported == "inactive") return "inactive";
    if (reported == "unconfigured" || reported == "credential_recovery") {
      return "credential_recovery";
    }
    if (reported == "auth_rejected" || reported == "pairing") {
      return "auth_rejected";
    }
    if (reported == "recovery" || reported == "offline" ||
        reported == "error" || reported == "app_not_registered") {
      return reported;
    }
    return onboarding.myAiActive() ? "bound" : "auth_rejected";
  }
  for (size_t index = 0; index < sizeof(known) / sizeof(known[0]); ++index) {
    if (reported == known[index]) return reported;
  }
  if (onboarding.myAiActive()) return "bound";
  if (onboarding.stage() == OnboardingStage::MyAiPairingRequested ||
      onboarding.stage() == OnboardingStage::AwaitingMyAiActivation) {
    return "pairing";
  }
  return "error";
}

const char* myAiStateMessage(const std::string& state, bool hasCode) {
  if (state == "app_not_registered")
    return "inkloop 应用尚未在 MyAI 注册，暂时无法生成六位绑定码。";
  if (state == "bound") return "MyAI 已绑定并激活。";
  if (state == "inactive")
    return "MyAI 已绑定，但运行授权检查返回 402。即使设备列表显示已激活，试用或订阅过期、设备额度不足、降级后超额也会阻止服务。请检查订阅账单和授权设备数；处理后设备会自动重试，无需重新绑定。";
  if (state == "auth_rejected")
    return "MyAI 与 Inkloop 的绑定关系仍然保留，但运行凭证校验返回 HTTP 401。设备会自动重试授权；无需重新绑定或重新请求六位码。";
  if (state == "credential_recovery")
    return "MyAI 与 Inkloop 的绑定关系仍然保留，但本机缺少可用的 MyAI 运行凭证。无需重新绑定或重新请求六位码；请在 MyAI 设备管理中保留此设备，等待凭证恢复。";
  if (state == "recovery") return "MyAI 设备凭据需要恢复，请先处理原设备。";
  if (state == "offline") return "MyAI 服务暂时不可用，设备会继续重试。";
  if (state == "pairing") return hasCode
      ? "六位绑定码已就绪，请在 MyAI 完成绑定。"
      : "MyAI 配对请求已发出，但六位绑定码尚未返回；设备会继续重试。";
  if (state == "unconfigured") return "MyAI 尚未配置，可在此重新请求绑定码。";
  return "MyAI 当前不可用，请查看诊断后重试。";
}

bool validStorageTarget(StorageTarget target) {
  switch (target) {
    case StorageTarget::Automatic:
    case StorageTarget::Internal:
    case StorageTarget::SdCard:
      return true;
  }
  return false;
}

bool validRefreshMode(RefreshMode mode) {
  return mode == RefreshMode::OfficialQuality ||
      mode == RefreshMode::ExperimentalSixColor ||
      mode == RefreshMode::ReflectancePhoto ||
      mode == RefreshMode::SolidClean;
}

bool validPowerMode(PowerMode mode) {
  return mode == PowerMode::Compatibility || mode == PowerMode::Battery;
}

bool validBudget(const RateBudget& budget) {
  return budget.maximumRequests > 0 && budget.maximumRequests <= 1000 &&
      budget.windowSeconds > 0 &&
      budget.windowSeconds <= kMaximumRateWindowSeconds;
}

bool safeAlbumCursor(const std::string& cursor) {
  if (cursor.size() > kMaximumAlbumCursorBytes) return false;
  for (size_t index = 0; index < cursor.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(cursor[index]);
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '-' ||
          character == '_')) {
      return false;
    }
  }
  return true;
}

bool boundedAppend(
    std::string* output,
    const std::string& fragment,
    size_t maximumBytes) {
  if (!output || fragment.size() > maximumBytes ||
      output->size() > maximumBytes - fragment.size()) {
    return false;
  }
  output->append(fragment);
  return true;
}

const RateBudget& rateBudgetByKind(
    const PortalRateConfig& config,
    uint8_t kind) {
  if (kind == 1) return config.writes;
  if (kind == 2) return config.destructive;
  return config.reads;
}

}  // namespace

InkloopPortal::InkloopPortal(IPortalAdapter& adapter, const PortalAccessConfig& access)
    : adapter_(adapter),
      access_(access),
      accessValid_(validateAccess(access)),
      hydrated_(false),
      sessionIssued_(false),
      sessionExpiresAtSeconds_(0),
      snapshotRevision_(0),
      settings_(),
      onboarding_(),
      pending_(),
      physicalResult_(),
      managementPasswordRestartRequired_(false),
      rateEntries_() {}

bool InkloopPortal::validateAccess(const PortalAccessConfig& access) const {
  if (!validLocalManagementPassword(access.bootNonce) ||
      !safeAccessValue(access.sessionId) ||
      !safeAccessValue(access.csrfToken) || access.sessionLifetimeSeconds < 60 ||
      access.sessionLifetimeSeconds > 86400 || access.allowedOrigins.empty() ||
      access.allowedOrigins.size() > 8 || !validBudget(access.rate.reads) ||
      !validBudget(access.rate.writes) ||
      !validBudget(access.rate.destructive) ||
      access.rate.maximumTrackedKeys < 8 ||
      access.rate.maximumTrackedKeys > 128) {
    return false;
  }
  for (size_t index = 0; index < access.allowedOrigins.size(); ++index) {
    const std::string& origin = access.allowedOrigins[index];
    if (origin.size() > 128 ||
        (origin.find("http://") != 0 && origin.find("https://") != 0) ||
        originHost(origin).empty()) {
      return false;
    }
  }
  return true;
}

bool InkloopPortal::validateSettings(
    const PortalSettings& settings,
    std::string* error) const {
  if (!validStorageTarget(settings.storageTarget) ||
      !validRefreshMode(settings.refreshMode) ||
      !validPowerMode(settings.powerMode)) {
    if (error) *error = "snapshot_invalid_setting_enum";
    return false;
  }
  if (settings.volume > 100 ||
      settings.ledMaximumBrightnessPercent < 1 ||
      settings.ledMaximumBrightnessPercent > 100 ||
      settings.assistantPrompt.size() > kMaximumAssistantPromptBytes ||
      settings.imagePromptTemplate.empty() ||
      settings.imagePromptTemplate.size() > kMaximumImagePromptTemplateBytes ||
      !validLocalManagementPassword(settings.localManagementPassword) ||
      settings.image.negativePrompt.size() > kMaximumNegativePromptBytes ||
      settings.image.steps < 1 || settings.image.steps > 50 ||
      !((settings.image.width == 400 && settings.image.height == 600) ||
        (settings.image.width == 600 && settings.image.height == 400)) ||
      settings.idleTimeoutSeconds < 120 ||
      settings.idleTimeoutSeconds > 3600) {
    if (error) *error = "snapshot_invalid_setting_value";
    return false;
  }
  if (error) error->clear();
  return true;
}

bool InkloopPortal::validateSnapshot(
    const PortalPersistedSnapshot& snapshot,
    std::string* error) const {
  if (snapshot.schemaVersion != kPortalSnapshotSchemaVersion) {
    if (error) {
      *error = snapshot.schemaVersion > kPortalSnapshotSchemaVersion
          ? "snapshot_future_schema" : "snapshot_unsupported_schema";
    }
    return false;
  }
  if (snapshot.presentFields != kAllPortalSnapshotFields) {
    if (error) *error = "snapshot_incomplete";
    return false;
  }
  if (snapshot.revision == 0 || snapshot.revision == UINT64_MAX) {
    if (error) *error = "snapshot_invalid_revision";
    return false;
  }
  if (!validateSettings(snapshot.settings, error) ||
      !OnboardingState::validatePersistedState(snapshot.onboarding, error)) {
    return false;
  }
  if (error) error->clear();
  return true;
}

PortalPersistedSnapshot InkloopPortal::makeSnapshot(
    const OnboardingState& onboarding,
    const PortalSettings& settings,
    uint64_t revision) const {
  PortalPersistedSnapshot snapshot;
  snapshot.schemaVersion = kPortalSnapshotSchemaVersion;
  snapshot.presentFields = kAllPortalSnapshotFields;
  snapshot.revision = revision;
  snapshot.onboarding = onboarding.persistedState();
  snapshot.settings = settings;
  return snapshot;
}

bool InkloopPortal::hydrate(
    const PortalPersistedSnapshot& snapshot,
    uint64_t nowSeconds,
    std::string* error) {
  if (hydrated_) {
    if (error) *error = "snapshot_already_hydrated";
    return false;
  }
  if (!validateSnapshot(snapshot, error)) return false;
  OnboardingState nextOnboarding;
  if (!nextOnboarding.hydrate(snapshot.onboarding, error)) return false;
  uint64_t hydratedRevision = snapshot.revision;
  const bool scrubbedBoundCode = nextOnboarding.clearBoundCodeIfNeeded();
  if (scrubbedBoundCode || nextOnboarding.expireCodeIfNeeded(nowSeconds)) {
    PortalSnapshotPatch patch;
    patch.schemaVersion = kPortalSnapshotSchemaVersion;
    patch.expectedRevision = snapshot.revision;
    patch.nextRevision = snapshot.revision + 1;
    patch.dirtyFields = SnapshotOnboardingStage | SnapshotMyAiActive |
        SnapshotCodeOwnership | SnapshotOnboardingCode |
        SnapshotInkloopCode | SnapshotCodeExpiry |
        SnapshotInkloopReuseAccepted;
    patch.mergedSnapshot = makeSnapshot(
        nextOnboarding, snapshot.settings, patch.nextRevision);
    std::string adapterError;
    if (!adapter_.persistPortalSnapshot(patch, &adapterError)) {
      if (error) {
        *error = adapterError.empty()
            ? "snapshot_expiry_persistence_failed" : adapterError;
      }
      return false;
    }
    hydratedRevision = patch.nextRevision;
  }
  settings_ = snapshot.settings;
  onboarding_ = nextOnboarding;
  snapshotRevision_ = hydratedRevision;
  hydrated_ = true;
  if (error) error->clear();
  return true;
}

bool InkloopPortal::persistState(
    const OnboardingState& onboarding,
    const PortalSettings& settings,
    uint32_t dirtyFields,
    std::string* error) {
  if (!hydrated_) {
    if (error) *error = "snapshot_not_hydrated";
    return false;
  }
  if (dirtyFields == 0 ||
      (dirtyFields & ~kAllPortalSnapshotFields) != 0 ||
      snapshotRevision_ == UINT64_MAX) {
    if (error) *error = "invalid_snapshot_patch";
    return false;
  }
  PortalSnapshotPatch patch;
  patch.schemaVersion = kPortalSnapshotSchemaVersion;
  patch.expectedRevision = snapshotRevision_;
  patch.nextRevision = snapshotRevision_ + 1;
  patch.dirtyFields = dirtyFields;
  patch.mergedSnapshot = makeSnapshot(onboarding, settings, patch.nextRevision);
  if (!validateSnapshot(patch.mergedSnapshot, error)) return false;
  std::string adapterError;
  if (!adapter_.persistPortalSnapshot(patch, &adapterError)) {
    if (error) {
      *error = adapterError.empty()
          ? "snapshot_persistence_failed" : adapterError;
    }
    return false;
  }
  onboarding_ = onboarding;
  settings_ = settings;
  snapshotRevision_ = patch.nextRevision;
  if (error) error->clear();
  return true;
}

bool InkloopPortal::applyExpiredCode(
    uint64_t nowSeconds,
    std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.expireCodeIfNeeded(nowSeconds)) {
    if (error) error->clear();
    return true;
  }
  return persistState(
      next, settings_,
      SnapshotOnboardingStage | SnapshotMyAiActive |
          SnapshotCodeOwnership | SnapshotOnboardingCode |
          SnapshotInkloopCode | SnapshotCodeExpiry |
          SnapshotInkloopReuseAccepted,
      error);
}

bool InkloopPortal::rotateAccess(const PortalAccessConfig& access, std::string* error) {
  if (!validateAccess(access)) {
    if (error) *error = "invalid_access_configuration";
    return false;
  }
  access_ = access;
  accessValid_ = true;
  sessionIssued_ = false;
  sessionExpiresAtSeconds_ = 0;
  clearPending();
  physicalResult_ = PhysicalConfirmationResult();
  clearRateEntries();
  if (error) error->clear();
  return true;
}

bool InkloopPortal::validPeerIp(const std::string& peerIp) const {
  if (peerIp.empty() || peerIp.size() > 64) return false;
  for (size_t index = 0; index < peerIp.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(peerIp[index]);
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F') ||
          character == '.' || character == ':')) {
      return false;
    }
  }
  return true;
}

std::string InkloopPortal::normalizedRateRoute(const std::string& path) const {
  static const char* routes[] = {
      "/health", "/api/session", "/", "/api/state", "/api/settings",
      "/api/album", "/api/diagnostics", "/api/serial-log",
      "/api/album/upload", "/api/album/preview", "/api/album/display",
      "/api/album/render", "/api/aigc/generate",
      "/api/audio/preview",
      "/api/onboarding/myai/start", "/api/onboarding/myai/rebind",
      "/api/tutorial/advance",
      "/api/tutorial/complete", "/api/tutorial/restart",
      "/api/actions/prepare", "/api/actions/confirm"};
  const size_t query = path.find('?');
  const std::string route = path.substr(0, query);
  for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
    if (route == routes[index]) return route;
  }
  return "/__unknown__";
}

InkloopPortal::RequestBudget InkloopPortal::requestBudget(
    const PortalRequest& request) const {
  if (request.path == "/api/actions/prepare" ||
      request.path == "/api/actions/confirm") {
    return RequestBudget::Destructive;
  }
  return request.method == "GET" ? RequestBudget::Read : RequestBudget::Write;
}

bool InkloopPortal::consumeRate(
    const PortalRequest& request,
    PortalResponse* rejected) {
  const RequestBudget kind = requestBudget(request);
  const RateBudget* budget = &access_.rate.reads;
  if (kind == RequestBudget::Write) budget = &access_.rate.writes;
  if (kind == RequestBudget::Destructive) budget = &access_.rate.destructive;
  const bool validSessionScope = request.path != "/api/session" &&
      sessionIssued_ && request.nowSeconds < sessionExpiresAtSeconds_ &&
      hasSessionCookie(request.cookie);
  const std::string sessionScope = validSessionScope
      ? access_.sessionId : "anonymous";
  const std::string route = normalizedRateRoute(request.path);
  const uint32_t now = static_cast<uint32_t>(request.nowSeconds);

  RateEntry* found = NULL;
  for (size_t index = 0; index < rateEntries_.size(); ++index) {
    RateEntry& entry = rateEntries_[index];
    if (entry.peerIp == request.peerIp && entry.sessionScope == sessionScope &&
        entry.route == route && entry.budget == kind) {
      found = &entry;
      break;
    }
  }
  if (!found) {
    if (rateEntries_.size() >= access_.rate.maximumTrackedKeys) {
      for (size_t index = 0; index < rateEntries_.size(); ++index) {
        RateEntry& candidate = rateEntries_[index];
        const uint32_t elapsed = now - candidate.windowStarted;
        const RateBudget& candidateBudget = rateBudgetByKind(
            access_.rate, static_cast<uint8_t>(candidate.budget));
        if (elapsed <= 0x7fffffffUL &&
            elapsed >= candidateBudget.windowSeconds) {
          found = &candidate;
          break;
        }
      }
      if (!found) {
        if (rejected) {
          *rejected = errorResponse(429, "rate_limit_capacity_reached");
          rejected->retryAfterSeconds = budget->windowSeconds;
        }
        return false;
      }
    }
    RateEntry entry;
    entry.peerIp = request.peerIp;
    entry.sessionScope = sessionScope;
    entry.route = route;
    entry.budget = kind;
    entry.windowStarted = now;
    entry.count = 1;
    if (found) *found = entry;
    else rateEntries_.push_back(entry);
    return true;
  }

  const uint32_t elapsed = now - found->windowStarted;
  if (elapsed > 0x7fffffffUL) {
    if (rejected) {
      *rejected = errorResponse(429, "rate_clock_rejected");
      rejected->retryAfterSeconds = budget->windowSeconds;
    }
    return false;
  }
  if (elapsed >= budget->windowSeconds) {
    found->windowStarted = now;
    found->count = 1;
    return true;
  }
  if (found->count >= budget->maximumRequests) {
    if (rejected) {
      *rejected = errorResponse(429, "rate_limited");
      rejected->retryAfterSeconds = budget->windowSeconds - elapsed;
    }
    return false;
  }
  ++found->count;
  return true;
}

void InkloopPortal::clearRateEntries() {
  rateEntries_.clear();
}

bool InkloopPortal::hostAllowed(const std::string& host) const {
  if (host.empty() || host.size() > 128 || host.find('/') != std::string::npos ||
      host.find('\\') != std::string::npos) {
    return false;
  }
  for (size_t index = 0; index < access_.allowedOrigins.size(); ++index) {
    if (originHost(access_.allowedOrigins[index]) == host) return true;
  }
  return false;
}

bool InkloopPortal::originAllowed(const std::string& origin) const {
  return std::find(access_.allowedOrigins.begin(), access_.allowedOrigins.end(), origin) !=
      access_.allowedOrigins.end();
}

bool InkloopPortal::hasSessionCookie(const std::string& cookie) const {
  return constantTimeEquals(cookieValue(cookie, "inkloop_session"), access_.sessionId);
}

bool InkloopPortal::sessionAuthorized(const PortalRequest& request) const {
  return accessValid_ && sessionIssued_ && request.nowSeconds < sessionExpiresAtSeconds_ &&
      hostAllowed(request.host) && hasSessionCookie(request.cookie);
}

bool InkloopPortal::mutationAuthorized(const PortalRequest& request) const {
  if (!sessionAuthorized(request) || !originAllowed(request.origin)) return false;
  if (constantTimeEquals(request.csrfToken, access_.csrfToken)) return true;
  if (request.contentType != "application/x-www-form-urlencoded") return false;
  std::map<std::string, std::string> fields;
  std::string error;
  if (!parseFormBody(request.body, kMaximumRequestBodyBytes, &fields, &error)) return false;
  const std::map<std::string, std::string>::const_iterator csrf = fields.find("_csrf");
  return csrf != fields.end() && constantTimeEquals(csrf->second, access_.csrfToken);
}

PortalResponse InkloopPortal::jsonResponse(int status, const std::string& body) {
  PortalResponse response;
  response.status = status;
  response.contentType = "application/json; charset=utf-8";
  response.body = body;
  return response;
}

PortalResponse InkloopPortal::htmlResponse(int status, const std::string& body) {
  PortalResponse response;
  response.status = status;
  response.contentType = "text/html; charset=utf-8";
  response.body = body;
  return response;
}

PortalResponse InkloopPortal::errorResponse(int status, const std::string& code) {
  return jsonResponse(status, "{\"ok\":false,\"error\":\"" + jsonEscape(code) + "\"}");
}

PortalResponse InkloopPortal::handle(const PortalRequest& request) {
  if (!accessValid_) return errorResponse(503, "portal_access_not_initialized");
  if (request.method != "GET" && request.method != "POST") {
    return errorResponse(405, "method_not_allowed");
  }
  if (!validPeerIp(request.peerIp)) return errorResponse(400, "invalid_peer_ip");
  PortalResponse rateRejection;
  if (!consumeRate(request, &rateRejection)) return rateRejection;
  if (!hostAllowed(request.host)) return errorResponse(400, "invalid_host");
  if (request.path == "/health" && request.method == "GET") {
    return jsonResponse(200, "{\"ok\":true,\"service\":\"inkloop-portal\"}");
  }
  if (!hydrated_) return errorResponse(503, "snapshot_not_hydrated");
  if (request.path == "/api/session" && request.method == "POST") {
    return handleSession(request);
  }
  if (request.path == "/" && request.method == "GET" && !sessionAuthorized(request)) {
    const std::string login = std::string(
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Inkloop PaperColor</title><style>") + kPortalCss +
        "</style></head><body><main><h1>设备本地设置</h1>"
        "<section class=\"code-help\"><h2>输入本地管理密码</h2>"
        "<p class=\"notice\"><strong>首次默认使用你保存的家庭 Wi‑Fi 密码</strong>，"
        "同一个值用于连接 Settings Wi‑Fi 和登录本地设置网页；除非你在设置中修改，否则重启后不变。</p>"
        "<p class=\"muted\">它不是 MyAI / Inkloop 的六位绑定码。请按设备屏幕完整输入。</p>"
        "<form id=\"login-form\" method=\"post\" action=\"/api/session\">"
        "<label>本地管理密码<input name=\"nonce\" minlength=\"8\" maxlength=\"63\" "
        "autocomplete=\"current-password\" spellcheck=\"false\" required></label>"
        "<button type=\"submit\">进入设置</button></form>"
        "<p id=\"page-status\" class=\"status\" aria-live=\"polite\"></p></section>"
        "</main><script>" + kLoginScript + "</script></body></html>";
    return htmlResponse(401, login);
  }
  if (!sessionAuthorized(request)) return errorResponse(401, "session_required");
  if (isMutationMethod(request.method) && !mutationAuthorized(request)) {
    return errorResponse(403, "origin_or_csrf_rejected");
  }
  expirePending(request.nowSeconds);
  std::string expiryError;
  if (!applyExpiredCode(request.nowSeconds, &expiryError)) {
    return errorResponse(
        503, expiryError.empty() ? "snapshot_persistence_failed" : expiryError);
  }
  return handleAuthenticated(request);
}

PortalResponse InkloopPortal::authorizeStreamingAlbumUpload(
    const PortalRequest& request) {
  if (!accessValid_ || !hydrated_)
    return errorResponse(503, "portal_access_not_initialized");
  if (request.method != "POST" || request.path != "/api/album/upload")
    return errorResponse(405, "method_not_allowed");
  if (!validPeerIp(request.peerIp)) return errorResponse(400, "invalid_peer_ip");
  PortalResponse rejected;
  if (!consumeRate(request, &rejected)) return rejected;
  if (!hostAllowed(request.host)) return errorResponse(400, "invalid_host");
  if (!sessionAuthorized(request)) return errorResponse(401, "session_required");
  if (!mutationAuthorized(request))
    return errorResponse(403, "origin_or_csrf_rejected");
  if (request.contentType.compare(0, 19, "multipart/form-data") != 0 ||
      request.contentType.find("boundary=") == std::string::npos ||
      request.contentType.size() > 256) {
    return errorResponse(415, "multipart_form_data_required");
  }
  if (adapter_.mutationBusy()) return errorResponse(409, "device_busy");
  return jsonResponse(202, "{\"ok\":true,\"state\":\"upload_authorized\"}");
}

PortalResponse InkloopPortal::authorizeStreamingAlbumPreview(
    const PortalRequest& request) {
  if (!accessValid_ || !hydrated_)
    return errorResponse(503, "portal_access_not_initialized");
  if (request.method != "GET" || request.path != "/api/album/preview")
    return errorResponse(405, "method_not_allowed");
  if (!validPeerIp(request.peerIp)) return errorResponse(400, "invalid_peer_ip");
  PortalResponse rejected;
  if (!consumeRate(request, &rejected)) return rejected;
  if (!hostAllowed(request.host)) return errorResponse(400, "invalid_host");
  if (!sessionAuthorized(request)) return errorResponse(401, "session_required");
  if (adapter_.mutationBusy()) return errorResponse(409, "device_busy");
  return jsonResponse(200, "{\"ok\":true,\"state\":\"preview_authorized\"}");
}

PortalResponse InkloopPortal::handleSession(const PortalRequest& request) {
  if (!originAllowed(request.origin)) return errorResponse(403, "origin_rejected");
  std::map<std::string, std::string> fields;
  PortalResponse failure;
  if (!parseFields(request, &fields, &failure)) return failure;
  const std::map<std::string, std::string>::const_iterator nonce = fields.find("nonce");
  if (fields.size() != 1 || nonce == fields.end() ||
      !constantTimeEquals(nonce->second, access_.bootNonce)) {
    return errorResponse(401, "invalid_bootstrap_nonce");
  }
  if (request.nowSeconds > UINT64_MAX - access_.sessionLifetimeSeconds) {
    return errorResponse(400, "invalid_request_clock");
  }
  // The local management password is a reusable credential, not a one-shot
  // bootstrap secret. Reissuing the same boot-scoped session lets another
  // browser log in and lets an expired 15-minute session renew without
  // forcing a device reboot. Login rate limits and constant-time password
  // comparison still apply before this point.
  sessionIssued_ = true;
  sessionExpiresAtSeconds_ = request.nowSeconds + access_.sessionLifetimeSeconds;
  PortalResponse response = jsonResponse(
      200,
      "{\"ok\":true,\"csrf\":\"" + jsonEscape(access_.csrfToken) +
          "\",\"expiresAt\":" + std::to_string(sessionExpiresAtSeconds_) + "}");
  response.setCookie = "inkloop_session=" + access_.sessionId +
      "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
      std::to_string(access_.sessionLifetimeSeconds);
  return response;
}

bool InkloopPortal::parseFields(
    const PortalRequest& request,
    std::map<std::string, std::string>* fields,
    PortalResponse* failure) const {
  if (!request.body.empty() &&
      request.contentType != "application/x-www-form-urlencoded") {
    if (failure) *failure = errorResponse(415, "unsupported_content_type");
    return false;
  }
  std::string error;
  if (!parseFormBody(request.body, kMaximumRequestBodyBytes, fields, &error)) {
    if (failure) *failure = errorResponse(
        error == "request_body_too_large" ? 413 : 400, error);
    return false;
  }
  return true;
}

PortalResponse InkloopPortal::handleAuthenticated(const PortalRequest& request) {
  if (request.method == "GET" && request.path == "/") {
    return renderDashboardResponse();
  }
  if (request.method == "GET" && request.path == "/portal.js") {
    PortalResponse response;
    response.status = 200;
    response.contentType = "application/javascript; charset=utf-8";
    response.body.reserve(kMaximumPortalScriptBytes);
    const char* scripts[] = {
        kDashboardScript, kLedBrightnessScript, kDisplayStatusScript,
        kSettingsScript, kAlbumUploadScript, kAlbumRenderScript};
    for (size_t index = 0; index < sizeof(scripts) / sizeof(scripts[0]); ++index) {
      if (!boundedAppend(
              &response.body, scripts[index], kMaximumPortalScriptBytes)) {
        return errorResponse(503, "portal_script_too_large");
      }
      if (!boundedAppend(&response.body, "\n", kMaximumPortalScriptBytes)) {
        return errorResponse(503, "portal_script_too_large");
      }
    }
    return response;
  }
  if (request.method == "GET" && request.path == "/api/state") {
    return jsonResponse(200, renderStateJson());
  }
  if (request.method == "GET" && request.path == "/api/settings") {
    return jsonResponse(200, renderSettingsJson());
  }
  if (request.method == "GET" &&
      (request.path == "/api/album" ||
       request.path.compare(0, 11, "/api/album?") == 0)) {
    std::string cursor;
    PortalResponse failure;
    if (!parseAlbumCursor(request.path, &cursor, &failure)) return failure;
    return renderAlbumResponse(cursor);
  }
  if (request.method == "GET" && request.path == "/api/diagnostics") {
    return jsonResponse(200, renderDiagnosticsJson(false));
  }
  if (request.method == "GET" && request.path == "/api/serial-log") {
    return jsonResponse(200, renderDiagnosticsJson(true));
  }

  std::map<std::string, std::string> fields;
  PortalResponse failure;
  if (!parseFields(request, &fields, &failure)) return failure;
  fields.erase("_csrf");

  if (request.method == "POST" && request.path == "/api/onboarding/myai/start") {
    if (!fields.empty()) return errorResponse(400, "unexpected_field");
    std::string error;
    OnboardingState next = onboarding_;
    if (!next.requestMyAiPairing(adapter_, &error)) {
      return errorResponse(409, error);
    }
    if (!persistState(next, settings_, SnapshotOnboardingStage, &error)) {
      return errorResponse(503, error);
    }
    return jsonResponse(202, "{\"ok\":true,\"appId\":\"inkloop\"}");
  }
  if (request.method == "POST" && request.path == "/api/onboarding/myai/rebind") {
    if (!fields.empty()) return errorResponse(400, "unexpected_field");
    std::string error;
    OnboardingState next = onboarding_;
    if (!next.requestMyAiRebind(adapter_, &error)) {
      return errorResponse(409, error.empty() ? "myai_rebind_not_started" : error);
    }
    if (!persistState(
            next, settings_, SnapshotOnboardingStage | SnapshotMyAiActive |
                SnapshotCodeOwnership | SnapshotOnboardingCode |
                SnapshotInkloopCode | SnapshotCodeExpiry,
            &error)) {
      return errorResponse(503, error);
    }
    return jsonResponse(
        202, "{\"ok\":true,\"state\":\"myai_rebind_started\"}");
  }
  if (request.method == "POST" && request.path == "/api/tutorial/advance") {
    if (!fields.empty()) return errorResponse(400, "unexpected_field");
    std::string error;
    OnboardingState next = onboarding_;
    if (!next.advanceTutorial(&error)) return errorResponse(409, error);
    if (!persistState(
            next, settings_, SnapshotOnboardingStage | SnapshotTutorialStep,
            &error)) {
      return errorResponse(503, error);
    }
    return jsonResponse(200, onboarding_.toJson());
  }
  if (request.method == "POST" && request.path == "/api/tutorial/complete") {
    if (!fields.empty()) return errorResponse(400, "unexpected_field");
    std::string error;
    OnboardingState next = onboarding_;
    if (!next.completeTutorial(&error)) return errorResponse(409, error);
    if (!persistState(
            next, settings_, SnapshotOnboardingStage | SnapshotTutorialStep,
            &error)) {
      return errorResponse(503, error);
    }
    return jsonResponse(200, onboarding_.toJson());
  }
  if (request.method == "POST" && request.path == "/api/tutorial/restart") {
    if (!fields.empty()) return errorResponse(400, "unexpected_field");
    std::string error;
    OnboardingState next = onboarding_;
    if (!next.restartTutorial(&error)) return errorResponse(409, error);
    if (!persistState(
            next, settings_, SnapshotOnboardingStage | SnapshotTutorialStep,
            &error)) {
      return errorResponse(503, error);
    }
    return jsonResponse(200, onboarding_.toJson());
  }
  if (request.method == "POST" && request.path == "/api/settings") {
    return handleSettings(request, fields);
  }
  if (request.method == "POST" && request.path == "/api/audio/preview") {
    if (fields.size() != 1 || fields.find("volume") == fields.end()) {
      return errorResponse(400, "invalid_volume_preview");
    }
    uint32_t value = 0;
    if (!strictUnsigned(fields.find("volume")->second, &value) || value > 100) {
      return errorResponse(400, "invalid_volume");
    }
    std::string error;
    if (!adapter_.previewVolume(static_cast<uint8_t>(value), &error)) {
      return errorResponse(409, error.empty() ? "audio_preview_failed" : error);
    }
    return jsonResponse(
        202, "{\"ok\":true,\"state\":\"volume_preview_started\",\"volume\":" +
                 std::to_string(value) + "}");
  }
  if (request.method == "POST" && request.path == "/api/album/display") {
    const std::map<std::string, std::string>::const_iterator asset =
        fields.find("asset_id");
    if (fields.size() != 1 || asset == fields.end() ||
        !isSafeAssetId(asset->second)) {
      return errorResponse(400, "invalid_asset_target");
    }
    std::string error;
    if (!adapter_.displayAlbumItem(asset->second, &error)) {
      return errorResponse(409, error.empty() ? "album_display_failed" : error);
    }
    return jsonResponse(202, "{\"ok\":true,\"state\":\"display_queued\"}");
  }
  if (request.method == "POST" && request.path == "/api/album/render") {
    const std::map<std::string, std::string>::const_iterator asset =
        fields.find("asset_id");
    const std::map<std::string, std::string>::const_iterator strategy =
        fields.find("render_strategy");
    if (fields.size() != 2 || asset == fields.end() ||
        strategy == fields.end() || !isSafeAssetId(asset->second) ||
        !validRenderStrategyName(strategy->second)) {
      return errorResponse(400, "invalid_render_strategy");
    }
    std::string error;
    if (!adapter_.setAlbumRenderStrategy(
            asset->second, strategy->second, &error)) {
      return errorResponse(
          409, error.empty() ? "album_render_strategy_failed" : error);
    }
    return jsonResponse(
        200, "{\"ok\":true,\"state\":\"render_strategy_saved\"}");
  }
  if (request.method == "POST" && request.path == "/api/aigc/generate") {
    const std::map<std::string, std::string>::const_iterator prompt =
        fields.find("prompt");
    if (fields.size() != 1 || prompt == fields.end() ||
        prompt->second.empty() ||
        prompt->second.size() > kMaximumManualAigcPromptBytes) {
      return errorResponse(400, "invalid_aigc_prompt");
    }
    std::string error;
    if (!adapter_.generateImage(prompt->second, &error)) {
      return errorResponse(409, error.empty() ? "aigc_start_failed" : error);
    }
    return jsonResponse(202, "{\"ok\":true,\"state\":\"aigc_started\"}");
  }
  if (request.method == "POST" && request.path == "/api/actions/prepare") {
    return handlePrepareAction(request, fields);
  }
  if (request.method == "POST" && request.path == "/api/actions/confirm") {
    return handleConfirmAction(request, fields);
  }
  return errorResponse(404, "route_not_found");
}

PortalResponse InkloopPortal::handleSettings(
    const PortalRequest&,
    const std::map<std::string, std::string>& fields) {
  if (fields.empty()) return errorResponse(400, "settings_required");
  static const char* allowed[] = {
      "storage", "volume", "voice_assistance", "voice_assistance_present", "led_brightness", "assistant_prompt", "image_size", "image_steps",
      "image_prompt_template", "negative_prompt", "led_swap", "refresh_mode",
      "power_mode", "idle_timeout", "local_password",
      "local_password_confirm"};
  for (std::map<std::string, std::string>::const_iterator field = fields.begin();
       field != fields.end(); ++field) {
    bool known = false;
    for (size_t index = 0; index < sizeof(allowed) / sizeof(allowed[0]); ++index) {
      if (field->first == allowed[index]) known = true;
    }
    if (!known) return errorResponse(400, "unknown_setting");
  }

  PortalSettings next = settings_;
  uint32_t dirtyFields = 0;
  std::map<std::string, std::string>::const_iterator field;
  field = fields.find("storage");
  if (field != fields.end()) {
    if (field->second == "auto") next.storageTarget = StorageTarget::Automatic;
    else if (field->second == "internal") next.storageTarget = StorageTarget::Internal;
    else if (field->second == "sd") next.storageTarget = StorageTarget::SdCard;
    else return errorResponse(400, "invalid_storage_target");
    dirtyFields |= SnapshotStorageTarget;
  }
  field = fields.find("volume");
  if (field != fields.end()) {
    uint32_t value = 0;
    if (!strictUnsigned(field->second, &value) || value > 100) {
      return errorResponse(400, "invalid_volume");
    }
    next.volume = static_cast<uint8_t>(value);
    dirtyFields |= SnapshotVolume;
  }
  const bool voiceAssistancePresent =
      fields.find("voice_assistance_present") != fields.end();
  field = fields.find("voice_assistance");
  if (voiceAssistancePresent) {
    if (fields.find("voice_assistance_present")->second != "1" ||
        (field != fields.end() && field->second != "1")) {
      return errorResponse(400, "invalid_voice_assistance");
    }
    next.voiceAssistanceEnabled = field != fields.end();
    // SnapshotVolume owns the local-audio preference group for schema v2.
    dirtyFields |= SnapshotVolume;
  } else if (field != fields.end()) {
    return errorResponse(400, "voice_assistance_presence_required");
  }
  field = fields.find("led_brightness");
  if (field != fields.end()) {
    uint32_t value = 0;
    if (!strictUnsigned(field->second, &value) || value < 1 || value > 100) {
      return errorResponse(400, "invalid_led_brightness");
    }
    next.ledMaximumBrightnessPercent = static_cast<uint8_t>(value);
    dirtyFields |= SnapshotLedRoles;
  }
  field = fields.find("assistant_prompt");
  if (field != fields.end()) {
    if (field->second.size() > kMaximumAssistantPromptBytes) {
      return errorResponse(400, "assistant_prompt_too_long");
    }
    next.assistantPrompt = field->second;
    dirtyFields |= SnapshotAssistantPrompt;
  }
  field = fields.find("image_prompt_template");
  if (field != fields.end()) {
    if (field->second.empty() ||
        field->second.size() > kMaximumImagePromptTemplateBytes) {
      return errorResponse(400, "image_prompt_template_invalid");
    }
    next.imagePromptTemplate = field->second;
    dirtyFields |= SnapshotImagePromptTemplate;
  }
  field = fields.find("image_size");
  if (field != fields.end()) {
    if (field->second == "400x600") {
      next.image.width = 400; next.image.height = 600;
    } else if (field->second == "600x400") {
      next.image.width = 600; next.image.height = 400;
    } else {
      return errorResponse(400, "invalid_image_size");
    }
    dirtyFields |= SnapshotImageSettings;
  }
  field = fields.find("image_steps");
  if (field != fields.end()) {
    uint32_t value = 0;
    if (!strictUnsigned(field->second, &value) || value < 1 || value > 50) {
      return errorResponse(400, "invalid_image_steps");
    }
    next.image.steps = static_cast<uint8_t>(value);
    dirtyFields |= SnapshotImageSettings;
  }
  field = fields.find("negative_prompt");
  if (field != fields.end()) {
    if (field->second.size() > kMaximumNegativePromptBytes) {
      return errorResponse(400, "negative_prompt_too_long");
    }
    next.image.negativePrompt = field->second;
    dirtyFields |= SnapshotImageSettings;
  }
  field = fields.find("led_swap");
  if (field != fields.end()) {
    if (field->second == "1") next.ledRolesSwapped = true;
    else if (field->second == "0") next.ledRolesSwapped = false;
    else return errorResponse(400, "invalid_led_swap");
    dirtyFields |= SnapshotLedRoles;
  }
  field = fields.find("refresh_mode");
  if (field != fields.end()) {
    if (field->second == "official-quality" || field->second == "official") {
      next.refreshMode = RefreshMode::OfficialQuality;
    } else if (field->second == "classic-six-color" ||
               field->second == "experimental-six-color") {
      next.refreshMode = RefreshMode::ExperimentalSixColor;
    } else if (field->second == "reflectance-photo") {
      next.refreshMode = RefreshMode::ReflectancePhoto;
    } else if (field->second == "solid-clean") {
      next.refreshMode = RefreshMode::SolidClean;
    } else return errorResponse(400, "invalid_refresh_mode");
    dirtyFields |= SnapshotRefreshMode;
  }
  field = fields.find("power_mode");
  if (field != fields.end()) {
    if (field->second == "compatibility") next.powerMode = PowerMode::Compatibility;
    else if (field->second == "battery") next.powerMode = PowerMode::Battery;
    else return errorResponse(400, "invalid_power_mode");
    dirtyFields |= SnapshotPowerMode;
  }
  field = fields.find("idle_timeout");
  if (field != fields.end()) {
    uint32_t value = 0;
    if (!strictUnsigned(field->second, &value) || value < 120 || value > 3600) {
      return errorResponse(400, "invalid_idle_timeout");
    }
    next.idleTimeoutSeconds = static_cast<uint16_t>(value);
    dirtyFields |= SnapshotIdleTimeout;
  }

  const std::map<std::string, std::string>::const_iterator password =
      fields.find("local_password");
  const std::map<std::string, std::string>::const_iterator confirmation =
      fields.find("local_password_confirm");
  bool passwordChanged = false;
  if (password != fields.end() || confirmation != fields.end()) {
    if (password == fields.end() || confirmation == fields.end()) {
      return errorResponse(400, "local_password_confirmation_required");
    }
    if (!password->second.empty() || !confirmation->second.empty()) {
      if (!validLocalManagementPassword(password->second)) {
        return errorResponse(400, "invalid_local_management_password");
      }
      if (!constantTimeEquals(password->second, confirmation->second)) {
        return errorResponse(400, "local_password_mismatch");
      }
      next.localManagementPassword = password->second;
      dirtyFields |= SnapshotLocalManagementPassword;
      passwordChanged = next.localManagementPassword !=
          settings_.localManagementPassword;
    }
  }

  if (!dirtyFields) return jsonResponse(200, renderSettingsJson());

  const bool brightnessChanged =
      next.ledMaximumBrightnessPercent !=
      settings_.ledMaximumBrightnessPercent;
  const bool ledMappingChanged =
      next.ledRolesSwapped != settings_.ledRolesSwapped;
  const bool ledDiagnosticRequested = brightnessChanged || ledMappingChanged;

  std::string error;
  if (!persistState(onboarding_, next, dirtyFields, &error)) {
    return errorResponse(
        503, error.empty() ? "settings_not_saved" : error);
  }
  if (passwordChanged) managementPasswordRestartRequired_ = true;
  std::string response = renderSettingsJson();
  if (ledDiagnosticRequested && !response.empty() &&
      response[response.size() - 1] == '}') {
    std::string diagnosticError;
    const bool accepted = adapter_.testLedRoles(
        settings_.ledRolesSwapped,
        settings_.ledMaximumBrightnessPercent,
        &diagnosticError);
    response.erase(response.size() - 1);
    response += ",\"ledDiagnosticRequested\":true";
    response += ",\"ledDiagnosticAccepted\":";
    response += boolJson(accepted);
    // Keep the original field for older local Portal clients.
    response += ",\"ledDiagnosticStarted\":";
    response += boolJson(accepted);
    if (!accepted) {
      response += ",\"ledDiagnosticError\":\"";
      response += jsonEscape(
          diagnosticError.empty() ? "led_role_test_failed" : diagnosticError);
      response += "\"";
    }
    response += "}";
  }
  return jsonResponse(200, response);
}

bool InkloopPortal::assetExistsAndMutable(const std::string& assetId) const {
  AlbumItem item;
  const AlbumReadStatus status = adapter_.findAlbumItem(assetId, &item);
  if (status != AlbumReadStatus::Ok || item.id != assetId ||
      item.id.size() > kMaximumAlbumIdBytes ||
      item.title.size() > kMaximumAlbumTitleBytes ||
      item.origin.size() > kMaximumAlbumOriginBytes) {
    return false;
  }
  return !item.factoryAsset;
}

PortalResponse InkloopPortal::handlePrepareAction(
    const PortalRequest& request,
    const std::map<std::string, std::string>& fields) {
  if (adapter_.mutationBusy()) return errorResponse(409, "device_busy");
  if (fields.size() < 1 || fields.size() > 2 || fields.find("action") == fields.end()) {
    return errorResponse(400, "invalid_action_request");
  }
  const std::string action = fields.find("action")->second;
  PendingAction next;
  next.present = true;
  next.expiresAtSeconds = request.nowSeconds + kConfirmationLifetimeSeconds;
  if (action == "format_sd") {
    if (fields.size() != 1) return errorResponse(400, "unexpected_target");
    next.action = DestructiveAction::FormatSdCard;
    next.target = "sd";
    next.phrase = "FORMAT SD";
  } else if (action == "clear_album") {
    if (fields.size() != 1) return errorResponse(400, "unexpected_target");
    next.action = DestructiveAction::ClearAlbum;
    next.target = "user_album";
    next.phrase = "CLEAR ALBUM";
  } else if (action == "delete_asset") {
    const std::map<std::string, std::string>::const_iterator target = fields.find("target");
    if (fields.size() != 2 || target == fields.end() ||
        !isSafeAssetId(target->second) || !assetExistsAndMutable(target->second)) {
      return errorResponse(400, "invalid_asset_target");
    }
    next.action = DestructiveAction::DeleteAsset;
    next.target = target->second;
    next.phrase = "DELETE " + target->second;
  } else {
    return errorResponse(400, "unknown_action");
  }
  next.confirmationId = adapter_.createNonce("portal-destructive-confirmation");
  if (!safeAccessValue(next.confirmationId)) {
    return errorResponse(503, "confirmation_nonce_unavailable");
  }
  pending_ = next;
  physicalResult_.state = PhysicalConfirmationState::BrowserConfirmationRequired;
  physicalResult_.action = next.action;
  physicalResult_.expiresAtSeconds = next.expiresAtSeconds;
  physicalResult_.error.clear();
  std::ostringstream output;
  output << "{\"ok\":true,\"action\":\"" << destructiveActionName(next.action)
         << "\",\"target\":\"" << jsonEscape(next.target)
         << "\",\"confirmationId\":\"" << jsonEscape(next.confirmationId)
         << "\",\"requiredPhrase\":\"" << jsonEscape(next.phrase)
         << "\",\"expiresAt\":" << next.expiresAtSeconds
         << ",\"physicalConfirmationRequired\":true}";
  return jsonResponse(202, output.str());
}

PortalResponse InkloopPortal::handleConfirmAction(
    const PortalRequest& request,
    const std::map<std::string, std::string>& fields) {
  if (!pending_.present || request.nowSeconds >= pending_.expiresAtSeconds) {
    clearPending();
    return errorResponse(410, "confirmation_expired");
  }
  if (fields.size() != 2 || fields.find("confirmation_id") == fields.end() ||
      fields.find("phrase") == fields.end() ||
      fields.find("confirmation_id")->second != pending_.confirmationId ||
      fields.find("phrase")->second != pending_.phrase) {
    return errorResponse(403, "confirmation_rejected");
  }
  pending_.webConfirmed = true;
  physicalResult_.state = PhysicalConfirmationState::AwaitingDeviceButton;
  physicalResult_.action = pending_.action;
  physicalResult_.expiresAtSeconds = pending_.expiresAtSeconds;
  physicalResult_.error.clear();
  return jsonResponse(
      202,
      "{\"ok\":true,\"state\":\"awaiting_physical_confirmation\","
      "\"confirmationId\":\"" + jsonEscape(pending_.confirmationId) +
          "\",\"expiresAt\":" + std::to_string(pending_.expiresAtSeconds) + "}");
}

void InkloopPortal::expirePending(uint64_t nowSeconds) {
  if (pending_.present && nowSeconds >= pending_.expiresAtSeconds) {
    physicalResult_.state = PhysicalConfirmationState::Expired;
    physicalResult_.action = pending_.action;
    physicalResult_.expiresAtSeconds = pending_.expiresAtSeconds;
    physicalResult_.error = "confirmation_expired";
    clearPending();
  }
}

void InkloopPortal::clearPending() {
  pending_ = PendingAction();
}

bool InkloopPortal::confirmPhysical(
    const std::string& confirmationId,
    uint64_t nowSeconds,
    std::string* error) {
  expirePending(nowSeconds);
  if (!pending_.present || !pending_.webConfirmed ||
      confirmationId != pending_.confirmationId) {
    if (error) *error = "physical_confirmation_rejected";
    return false;
  }
  if (adapter_.mutationBusy()) {
    if (error) *error = "device_busy";
    return false;
  }
  ConfirmedOperation operation;
  operation.action = pending_.action;
  operation.target = pending_.target;
  operation.confirmationId = pending_.confirmationId;
  const DestructiveAction action = pending_.action;
  clearPending();  // Consume before dispatch so a callback cannot be replayed.
  std::string adapterError;
  if (!adapter_.executeConfirmedOperation(operation, &adapterError)) {
    physicalResult_.state = PhysicalConfirmationState::Failed;
    physicalResult_.action = action;
    physicalResult_.expiresAtSeconds = 0;
    physicalResult_.error = adapterError.empty() ? "operation_failed" : adapterError;
    if (error) *error = physicalResult_.error;
    return false;
  }
  physicalResult_.state = PhysicalConfirmationState::Complete;
  physicalResult_.action = action;
  physicalResult_.expiresAtSeconds = 0;
  physicalResult_.error.clear();
  if (error) error->clear();
  return true;
}

bool InkloopPortal::onWifiConfigured(bool configured, std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.setWifiConfigured(configured, error)) return false;
  return persistState(
      next, settings_, SnapshotWifiConfigured | SnapshotOnboardingStage,
      error);
}

bool InkloopPortal::requestMyAiPairing(std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.requestMyAiPairing(adapter_, error)) return false;
  return persistState(next, settings_, SnapshotOnboardingStage, error);
}

bool InkloopPortal::onMyAiPairingResumed(std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.resumeMyAiPairing(error)) return false;
  return persistState(next, settings_, SnapshotOnboardingStage, error);
}

bool InkloopPortal::onMyAiPairingCancelled(std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.cancelMyAiPairing(error)) return false;
  return persistState(
      next, settings_, SnapshotOnboardingStage | SnapshotCodeOwnership |
          SnapshotOnboardingCode | SnapshotInkloopCode | SnapshotCodeExpiry |
          SnapshotInkloopReuseAccepted,
      error);
}

bool InkloopPortal::onAuthoritativeMyAiCode(
    const std::string& onboardingCode,
    uint64_t expiresAtSeconds,
    uint64_t nowSeconds,
    std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.receiveAuthoritativeMyAiCode(
          onboardingCode, expiresAtSeconds, nowSeconds, adapter_, error)) {
    return false;
  }
  return persistState(
      next, settings_,
      SnapshotOnboardingStage | SnapshotMyAiActive |
          SnapshotCodeOwnership | SnapshotOnboardingCode |
          SnapshotInkloopCode | SnapshotCodeExpiry |
          SnapshotInkloopReuseAccepted,
      error);
}

bool InkloopPortal::retryInkloopCodeReuse(std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.retryInkloopCodeReuse(adapter_, error)) return false;
  return persistState(
      next, settings_,
      SnapshotCodeOwnership | SnapshotInkloopCode |
          SnapshotInkloopReuseAccepted,
      error);
}

bool InkloopPortal::onInkloopBound(std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.markInkloopBound(error)) return false;
  if (persistState(
          next, settings_, SnapshotInkloopBound | SnapshotCodeOwnership |
              SnapshotOnboardingCode | SnapshotInkloopCode |
              SnapshotCodeExpiry,
          error)) {
    return true;
  }
  // Inkloop-first is recoverable: keep the last durable MyAI code and replay
  // it after reboot. Only a terminal erase failure must redact RAM and disable
  // the Portal so the completed secret can never be served again.
  if (next.terminalBindingComplete()) {
    onboarding_ = next;
    hydrated_ = false;
    if (error && error->empty()) *error = "bound_code_scrub_not_durable";
  }
  return false;
}

bool InkloopPortal::onMyAiActivation(bool active, std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.setMyAiActivation(active, error)) return false;
  if (persistState(
          next, settings_, SnapshotMyAiActive | SnapshotOnboardingStage |
              SnapshotCodeOwnership | SnapshotOnboardingCode |
              SnapshotInkloopCode | SnapshotCodeExpiry,
          error)) {
    return true;
  }
  if (next.terminalBindingComplete()) {
    onboarding_ = next;
    hydrated_ = false;
    if (error && error->empty()) *error = "bound_code_scrub_not_durable";
  }
  return false;
}

bool InkloopPortal::onVoiceTutorialComplete(std::string* error) {
  OnboardingState next = onboarding_;
  if (!next.completeTutorial(error)) return false;
  return persistState(
      next, settings_, SnapshotOnboardingStage | SnapshotTutorialStep, error);
}

bool InkloopPortal::replaceSettings(
    const PortalSettings& settings, std::string* error) {
  if (!validateSettings(settings, error)) return false;
  return persistState(
      onboarding_, settings,
      SnapshotStorageTarget | SnapshotVolume | SnapshotAssistantPrompt |
          SnapshotImagePromptTemplate | SnapshotLocalManagementPassword |
          SnapshotImageSettings | SnapshotLedRoles | SnapshotRefreshMode |
          SnapshotPowerMode | SnapshotIdleTimeout,
      error);
}

std::string InkloopPortal::renderSettingsJson() const {
  std::ostringstream output;
  output << "{\"storage\":\"" << storageTargetName(settings_.storageTarget)
         << "\",\"volume\":" << static_cast<unsigned int>(settings_.volume)
         << ",\"voiceAssistanceEnabled\":"
         << boolJson(settings_.voiceAssistanceEnabled)
         << ",\"ledMaximumBrightness\":"
         << static_cast<unsigned int>(
                settings_.ledMaximumBrightnessPercent)
         << ",\"assistantPrompt\":\"" << jsonEscape(settings_.assistantPrompt)
         << "\",\"imagePromptTemplate\":\""
         << jsonEscape(settings_.imagePromptTemplate)
         << "\",\"image\":{\"width\":" << settings_.image.width
         << ",\"height\":" << settings_.image.height
         << ",\"steps\":" << static_cast<unsigned int>(settings_.image.steps)
         << ",\"negativePrompt\":\"" << jsonEscape(settings_.image.negativePrompt)
         << "\"},\"ledRolesSwapped\":" << boolJson(settings_.ledRolesSwapped)
         << ",\"refreshMode\":\"" << refreshModeName(settings_.refreshMode)
         << "\",\"powerMode\":\"" << powerModeName(settings_.powerMode)
         << "\",\"idleTimeoutSeconds\":" << settings_.idleTimeoutSeconds
         << ",\"localManagementPassword\":{\"configured\":true,\"minimumLength\":"
         << kMinimumLocalManagementPasswordBytes
         << ",\"maximumLength\":" << kMaximumLocalManagementPasswordBytes
         << ",\"restartRequired\":"
         << boolJson(managementPasswordRestartRequired_) << "}}";
  return output.str();
}

std::string InkloopPortal::renderPhysicalConfirmationJson() const {
  const char* state = "idle";
  switch (physicalResult_.state) {
    case PhysicalConfirmationState::Idle:
      state = "idle";
      break;
    case PhysicalConfirmationState::BrowserConfirmationRequired:
      state = "browser_confirmation_required";
      break;
    case PhysicalConfirmationState::AwaitingDeviceButton:
      state = "awaiting_device_button";
      break;
    case PhysicalConfirmationState::Complete:
      state = "complete";
      break;
    case PhysicalConfirmationState::Failed:
      state = "failed";
      break;
    case PhysicalConfirmationState::Expired:
      state = "expired";
      break;
  }
  std::ostringstream output;
  output << "{\"state\":\"" << state << "\",\"action\":\""
         << destructiveActionName(physicalResult_.action)
         << "\",\"expiresAt\":" << physicalResult_.expiresAtSeconds
         << ",\"error\":\"" << jsonEscape(sanitizedDiagnosticLine(
                physicalResult_.error, 128)) << "\"}";
  return output.str();
}

std::string InkloopPortal::renderStateJson() const {
  const DiagnosticsSnapshot diagnostics = adapter_.diagnostics();
  const std::string myAiState = typedMyAiState(
      sanitizedDiagnosticLine(diagnostics.myAiState, 64), onboarding_);
  return "{\"onboarding\":" + onboarding_.toJson() +
      ",\"settings\":" + renderSettingsJson() +
      ",\"pendingPhysicalConfirmation\":" + boolJson(pending_.present && pending_.webConfirmed) +
      ",\"physicalConfirmation\":" + renderPhysicalConfirmationJson() +
      ",\"display\":{\"state\":\"" +
          jsonEscape(sanitizedDiagnosticLine(diagnostics.displayState, 32)) +
          "\"}" +
      ",\"myAi\":{\"state\":\"" + jsonEscape(myAiState) +
      "\",\"message\":\"" + jsonEscape(myAiStateMessage(
          myAiState, !onboarding_.onboardingCode().empty())) + "\"}" +
      "}";
}

std::string InkloopPortal::renderAlbumJson() const {
  return renderAlbumResponse(std::string()).body;
}

bool InkloopPortal::parseAlbumCursor(
    const std::string& path,
    std::string* cursor,
    PortalResponse* failure) const {
  if (!cursor) return false;
  cursor->clear();
  if (path == "/api/album") return true;
  static const std::string prefix = "/api/album?cursor=";
  if (path.compare(0, prefix.size(), prefix) != 0 ||
      path.find('&', prefix.size()) != std::string::npos) {
    if (failure) *failure = errorResponse(422, "invalid_album_cursor");
    return false;
  }
  *cursor = path.substr(prefix.size());
  if (!safeAlbumCursor(*cursor)) {
    cursor->clear();
    if (failure) *failure = errorResponse(422, "invalid_album_cursor");
    return false;
  }
  return true;
}

AlbumReadStatus InkloopPortal::readValidatedAlbumPage(
    const std::string& cursor,
    AlbumPage* page) const {
  if (!page || !safeAlbumCursor(cursor)) return AlbumReadStatus::InvalidData;
  AlbumPageRequest request;
  request.cursor = cursor;
  const AlbumReadStatus status = adapter_.readAlbumPage(request, page);
  if (status != AlbumReadStatus::Ok) return status;
  if (page->items.size() > request.maximumItems ||
      !safeAlbumCursor(page->nextCursor) ||
      (!page->nextCursor.empty() && page->nextCursor == cursor)) {
    return AlbumReadStatus::InvalidData;
  }
  size_t totalFieldBytes = 0;
  for (size_t index = 0; index < page->items.size(); ++index) {
    const AlbumItem& item = page->items[index];
    if (item.id.empty() || item.id.size() > request.maximumIdBytes ||
        item.title.size() > request.maximumTitleBytes ||
        item.origin.size() > request.maximumOriginBytes ||
        !validRenderStrategyName(item.renderStrategy)) {
      return AlbumReadStatus::InvalidData;
    }
    const size_t itemBytes = item.id.size() + item.title.size() +
        item.origin.size() + item.renderStrategy.size();
    if (itemBytes > request.maximumTotalFieldBytes ||
        totalFieldBytes > request.maximumTotalFieldBytes - itemBytes) {
      return AlbumReadStatus::TooLarge;
    }
    totalFieldBytes += itemBytes;
  }
  return AlbumReadStatus::Ok;
}

PortalResponse InkloopPortal::albumReadError(
    AlbumReadStatus status,
    bool html) {
  int httpStatus = 503;
  const char* code = "album_unavailable";
  if (status == AlbumReadStatus::TooLarge) {
    httpStatus = 413;
    code = "album_page_too_large";
  } else if (status == AlbumReadStatus::InvalidData) {
    httpStatus = 422;
    code = "album_page_invalid";
  } else if (status == AlbumReadStatus::NotFound) {
    httpStatus = 422;
    code = "album_cursor_not_found";
  }
  if (!html) return errorResponse(httpStatus, code);
  return htmlResponse(
      httpStatus,
      std::string("<!doctype html><html><body><main data-error=\"") + code +
          "\"><h1>相册暂不可用</h1></main></body></html>");
}

PortalResponse InkloopPortal::renderAlbumResponse(
    const std::string& cursor) const {
  AlbumPage page;
  const AlbumReadStatus read = readValidatedAlbumPage(cursor, &page);
  if (read != AlbumReadStatus::Ok) return albumReadError(read, false);
  const StorageStatus storage = adapter_.storageStatus();
  std::ostringstream header;
  header << "{\"storage\":{\"internalMounted\":"
         << boolJson(storage.internalMounted)
         << ",\"internalRecoveryRequired\":"
         << boolJson(storage.internalRecoveryRequired)
         << ",\"taskStoreReady\":" << boolJson(storage.taskStoreReady)
         << ",\"sdPresent\":" << boolJson(storage.sdPresent)
         << ",\"sdWritable\":" << boolJson(storage.sdWritable)
         << ",\"internalFreeBytes\":" << storage.internalFreeBytes
         << ",\"internalTotalBytes\":" << storage.internalTotalBytes
         << ",\"sdFreeBytes\":" << storage.sdFreeBytes
         << ",\"sdTotalBytes\":" << storage.sdTotalBytes
         << ",\"activeBackend\":\"" << activeStorageName(storage.activeBackend)
         << "\",\"uploadBackend\":\"" << activeStorageName(storage.activeBackend)
         << "\",\"activeMounted\":" << boolJson(storage.activeMounted)
         << ",\"activeWritable\":" << boolJson(storage.activeWritable)
         << ",\"activeFreeBytes\":" << storage.activeFreeBytes
         << ",\"activeTotalBytes\":" << storage.activeTotalBytes
         << ",\"activeUsedBytes\":"
         << (storage.activeFreeBytes <= storage.activeTotalBytes
                 ? storage.activeTotalBytes - storage.activeFreeBytes : 0)
         << "},\"page\":{\"cursor\":\"" << jsonEscape(cursor)
         << "\",\"nextCursor\":\"" << jsonEscape(page.nextCursor)
         << "\",\"totalItems\":" << page.totalItems
         << ",\"limit\":" << kMaximumAlbumPageItems << "},\"items\":[";
  std::string output;
  output.reserve(kMaximumAlbumJsonBytes);
  if (!boundedAppend(&output, header.str(), kMaximumAlbumJsonBytes))
    return albumReadError(AlbumReadStatus::TooLarge, false);
  for (size_t index = 0; index < page.items.size(); ++index) {
    std::ostringstream item;
    if (index) item << ',';
    item << "{\"id\":\"" << jsonEscape(page.items[index].id)
         << "\",\"title\":\"" << jsonEscape(page.items[index].title)
         << "\",\"origin\":\"" << jsonEscape(page.items[index].origin)
         << "\",\"bytes\":" << page.items[index].bytes
         << ",\"current\":" << boolJson(page.items[index].current)
         << ",\"factoryAsset\":" << boolJson(page.items[index].factoryAsset)
         << ",\"renderStrategy\":\""
         << jsonEscape(page.items[index].renderStrategy) << "\""
         << '}';
    if (!boundedAppend(&output, item.str(), kMaximumAlbumJsonBytes))
      return albumReadError(AlbumReadStatus::TooLarge, false);
  }
  if (!boundedAppend(&output, "]}", kMaximumAlbumJsonBytes))
    return albumReadError(AlbumReadStatus::TooLarge, false);
  return jsonResponse(200, output);
}

std::string InkloopPortal::renderDiagnosticsJson(bool includeSerial) const {
  const DiagnosticsSnapshot diagnostics = adapter_.diagnostics();
  std::ostringstream output;
  output << "{\"firmwareVersion\":\"" << jsonEscape(sanitizedDiagnosticLine(
              diagnostics.firmwareVersion, 64))
         << "\",\"hardwareSku\":\"" << jsonEscape(sanitizedDiagnosticLine(
              diagnostics.hardwareSku, 64))
         << "\",\"wifiState\":\"" << jsonEscape(sanitizedDiagnosticLine(
              diagnostics.wifiState, 128))
         << "\",\"storageState\":\"" << jsonEscape(sanitizedDiagnosticLine(
              diagnostics.storageState, 128))
         << "\",\"displayState\":\"" << jsonEscape(sanitizedDiagnosticLine(
              diagnostics.displayState, 128))
         << "\",\"myAiState\":\"" << jsonEscape(sanitizedDiagnosticLine(
              diagnostics.myAiState, 128))
         << "\",\"freeHeapBytes\":" << diagnostics.freeHeapBytes
         << ",\"freePsramBytes\":" << diagnostics.freePsramBytes;
  if (includeSerial) {
    output << ",\"serialLines\":[";
    const size_t count = diagnostics.serialLines.size() < 200
        ? diagnostics.serialLines.size() : 200;
    for (size_t index = 0; index < count; ++index) {
      if (index) output << ',';
      output << '"' << jsonEscape(sanitizedDiagnosticLine(
          diagnostics.serialLines[index], 256)) << '"';
    }
    output << ']';
  }
  output << '}';
  return output.str();
}

std::string InkloopPortal::renderDashboardHtml() const {
  return renderDashboardResponse().body;
}

PortalResponse InkloopPortal::renderDashboardResponse() const {
  AlbumPage page;
  const AlbumReadStatus read = readValidatedAlbumPage(std::string(), &page);
  if (read != AlbumReadStatus::Ok) return albumReadError(read, true);
  const StorageStatus storage = adapter_.storageStatus();
  const uint64_t activeUsed =
      storage.activeFreeBytes <= storage.activeTotalBytes
          ? storage.activeTotalBytes - storage.activeFreeBytes : 0;
  const DiagnosticsSnapshot diagnostics = adapter_.diagnostics();
  const std::string myAiState = typedMyAiState(
      sanitizedDiagnosticLine(diagnostics.myAiState, 64), onboarding_);
  const bool canRetryMyAi =
      onboarding_.stage() == OnboardingStage::WifiConfigured &&
      onboarding_.onboardingCode().empty() &&
      !onboarding_.terminalBindingComplete();
  std::ostringstream prefix;
  prefix << "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
         << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         << "<meta name=\"inkloop-csrf\" content=\"" << htmlEscape(access_.csrfToken)
         << "\"><title>Inkloop PaperColor 设置</title><style>" << kPortalCss
         << kSettingsCss << "</style></head><body><main>"
         << "<h1>PaperColor 本地设置</h1>"
         << "<p id=\"page-status\" class=\"status\" aria-live=\"polite\">设备已连接，可以在本页完成设置。</p>"
         << "<nav class=\"tabs\" aria-label=\"设置分类\">"
         << "<button type=\"button\" data-tab=\"device\">设备</button>"
         << "<button type=\"button\" data-tab=\"album\">相册</button>"
         << "<button type=\"button\" data-tab=\"ai\">AI 生图</button>"
         << "<button type=\"button\" data-tab=\"settings\">设置</button></nav>"
         << "<div class=\"tab-panel active\" data-tab-panel=\"device\">"
         << "<section><h2>初始化</h2>"
         << "<p>状态：" << htmlEscape(onboarding_.terminalBindingComplete()
                ? (myAiState == "bound" ? "已就绪" : "已绑定，运行授权待恢复")
                : onboardingStageName(onboarding_.stage())) << "</p>"
         << "<p>MyAI 应用：<strong>inkloop</strong></p>"
         << "<p id=\"myai-status\" class=\"notice"
         << (myAiState == "app_not_registered" ? " warning" : "")
         << "\">" << htmlEscape(myAiStateMessage(
                myAiState, !onboarding_.onboardingCode().empty())) << "</p>"
         << (myAiState == "inactive" || myAiState == "auth_rejected" ||
                 myAiState == "credential_recovery"
                 ? "<div class=\"actions\"><a class=\"secondary-action\" href=\"https://myai.mess.host/#devices\" target=\"_blank\" rel=\"noopener noreferrer\">MyAI 设备</a><a class=\"secondary-action\" href=\"https://myai.mess.host/#billing\" target=\"_blank\" rel=\"noopener noreferrer\">检查 MyAI 订阅账单</a></div><p class=\"muted\">设备已激活时，以订阅和额度为准；保持联网 30 秒内自动更新，无需删除设备或重新绑定。</p>"
                 : "")
         << (myAiState == "auth_rejected" ||
                 myAiState == "credential_recovery"
                 ? "<form class=\"inline\" data-portal=\"1\" method=\"post\" action=\"/api/onboarding/myai/rebind\" onsubmit=\"return confirm('清除旧 MyAI 凭据并申请新六位码？Inkloop 和相册不受影响。')\"><button class=\"danger\" type=\"submit\">恢复 / 重新绑定 MyAI</button></form><p class=\"muted\">如提示凭据恢复，请先在 MyAI 停用旧设备。</p>"
                 : "")
         << (onboarding_.terminalBindingComplete()
                 ? "<p>MyAI 与 Inkloop 已绑定。</p>"
                 : (onboarding_.onboardingCode().empty()
                        ? std::string()
                        : std::string("<div class=\"myai-qr\" data-qr-target=\"") +
                              htmlEscape(onboarding_.myAiRegistrationUrl()) +
                              "\"><a rel=\"noreferrer\" href=\"" +
                              htmlEscape(onboarding_.myAiRegistrationUrl()) +
                              "\">注册或绑定 MyAI</a></div><p>六位码：<strong>" +
                              htmlEscape(onboarding_.onboardingCode()) +
                              "</strong></p>"));
  if (canRetryMyAi) {
    prefix << "<form data-portal=\"1\" method=\"post\" action=\"/api/onboarding/myai/start\">"
           << "<input type=\"hidden\" name=\"_csrf\" value=\""
           << htmlEscape(access_.csrfToken)
           << "\"><button type=\"submit\">重新请求六位绑定码</button></form>";
  }
  prefix << "</section><section class=\"code-help\"><h2>本地访问安全</h2>"
         << "<p class=\"notice\"><strong>设备屏幕上的本地管理密码</strong>同时用于 Settings Wi‑Fi "
         << "和本地网页登录；默认复用已保存的家庭 Wi‑Fi 密码，也可以在下方修改。</p>"
         << "<p class=\"muted\">它不是 MyAI / Inkloop 的六位绑定码。</p>"
         << "</section></div><div class=\"tab-panel\" data-tab-panel=\"album\"><section><h2>相册</h2>"
         << "<p id=\"album-capacity\" data-backend=\""
         << activeStorageName(storage.activeBackend)
         << "\" data-free=\"" << storage.activeFreeBytes
         << "\" data-total=\"" << storage.activeTotalBytes
         << "\" class=\"notice"
         << (!storage.activeMounted || !storage.activeWritable ? " warning" : "")
         << "\">实际后端：<strong>" << activeStorageName(storage.activeBackend)
         << "</strong> · 图片数：<strong>" << page.totalItems << "</strong> · ";
  if (!storage.activeMounted || !storage.activeWritable) {
    prefix << "未挂载或不可写，需要恢复后才能上传。</p>";
  } else {
    prefix << "已用 " << humanBytes(activeUsed)
           << " · 剩余 " << humanBytes(storage.activeFreeBytes)
           << " · 总计 " << humanBytes(storage.activeTotalBytes) << "</p>";
  }
  prefix << "<form id=\"album-upload-form\" data-width=\""
         << settings_.image.width << "\" data-height=\""
         << settings_.image.height << "\">"
         << "<label>上传图片<input id=\"album-upload-file\" type=\"file\" accept=\"image/*\""
         << (!storage.activeMounted || !storage.activeWritable ? " disabled" : "")
         << "></label><canvas id=\"album-upload-preview\" class=\"upload-preview\" aria-label=\"裁切预览\"></canvas>"
         << "<p id=\"album-upload-info\" class=\"muted\">浏览器会按当前 "
         << settings_.image.width << "×" << settings_.image.height
         << " 方向等比 cover/crop 并转为 PNG；设备会重新验证签名、实际尺寸、大小和容量。</p>"
         << "<button id=\"album-upload-button\" type=\"submit\" disabled>确认并上传</button></form>"
         << "<ul class=\"album-grid\" aria-label=\"设备相册\">";
  std::string output;
  output.reserve(kMaximumDashboardHtmlBytes);
  if (!boundedAppend(&output, prefix.str(), kMaximumDashboardHtmlBytes))
    return albumReadError(AlbumReadStatus::TooLarge, true);
  for (size_t index = 0; index < page.items.size(); ++index) {
    std::ostringstream item;
    const bool safeAssetId = isSafeAssetId(page.items[index].id);
    item << "<li class=\"album-card\" data-asset-id=\""
         << htmlEscape(page.items[index].id) << "\">";
    if (safeAssetId) {
      item << "<img class=\"album-thumb\" loading=\"lazy\" decoding=\"async\" width=\""
           << settings_.image.width << "\" height=\"" << settings_.image.height
           << "\" src=\"/api/album/preview?asset_id="
           << htmlEscape(page.items[index].id) << "\" alt=\""
           << htmlEscape(page.items[index].title) << " 预览\">";
    }
    item << "<span class=\"album-source\">"
         << (page.items[index].origin == "inkloop" ? "Inkloop 任务" :
             (page.items[index].origin == "myai" ? "MyAI 生成" : "用户图片"))
         << "</span><p class=\"album-meta\"><strong>"
         << htmlEscape(page.items[index].title) << "</strong><br><span class=\"muted\">"
         << humanBytes(page.items[index].bytes) << "</span></p>";
    if (safeAssetId) {
      item << "<label>渲染方式 <select data-album-render data-asset=\""
           << htmlEscape(page.items[index].id) << "\" data-current=\""
           << htmlEscape(page.items[index].renderStrategy)
           << "\"><option>加载中</option></select></label>";
    }
    if (safeAssetId) {
      item << "<div class=\"album-actions\"><form class=\"inline\" data-portal=\"1\" method=\"post\" "
              << "action=\"/api/album/display\">"
              << "<input type=\"hidden\" name=\"_csrf\" value=\""
              << htmlEscape(access_.csrfToken)
              << "\"><input type=\"hidden\" name=\"asset_id\" value=\""
              << htmlEscape(page.items[index].id) << "\"><button type=\"submit\""
              << (page.items[index].current ? " disabled" : "") << ">"
              << (page.items[index].current ? "正在显示" : "上屏")
              << "</button></form>";
    }
    if (!page.items[index].factoryAsset && safeAssetId) {
      item << "<form class=\"inline\" data-portal=\"1\" data-destructive=\"1\" "
             << "method=\"post\" action=\"/api/actions/prepare\">"
             << "<input type=\"hidden\" name=\"_csrf\" value=\""
             << htmlEscape(access_.csrfToken)
             << "\"><input type=\"hidden\" name=\"action\" value=\"delete_asset\">"
             << "<input type=\"hidden\" name=\"target\" value=\""
             << htmlEscape(page.items[index].id)
             << "\"><button class=\"danger\" type=\"submit\">删除</button></form>";
    }
    if (safeAssetId) item << "</div>";
    item << "</li>";
    if (!boundedAppend(&output, item.str(), kMaximumDashboardHtmlBytes))
      return albumReadError(AlbumReadStatus::TooLarge, true);
  }
  std::ostringstream suffix;
  suffix << "</ul>";
  if (!page.nextCursor.empty()) {
    suffix << "<p><a href=\"/api/album?cursor="
           << htmlEscape(page.nextCursor)
           << "\">继续查看相册（JSON）</a></p>";
  }
  suffix << "<form data-portal=\"1\" data-destructive=\"1\" method=\"post\" action=\"/api/actions/prepare\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><input type=\"hidden\" name=\"action\" value=\"clear_album\">"
         << "<button class=\"danger\" type=\"submit\">清空用户相册（需顶部键确认）</button></form></section></div>"
         << "<div class=\"tab-panel\" data-tab-panel=\"ai\"><section><h2>MyAI 图片生成</h2>"
         << "<p class=\"notice\">输入主题后会调用已绑定的 MyAI 渲染服务；生成结果会自动缓存到相册并上屏。</p>"
         << "<form data-portal=\"1\" method=\"post\" action=\"/api/aigc/generate\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><label>图片主题<textarea name=\"prompt\" maxlength=\"1024\" required "
         << "placeholder=\"例如：一只戴黄色雨帽的猫，站在高对比的蓝色海边，六色墨水屏海报风格\"></textarea></label>"
         << "<p class=\"muted\">会自动套用“设置”里的图片提示词模板、负面提示词、尺寸和生成步数。</p>"
         << "<button type=\"submit\">开始生成并上屏</button></form></section></div>"
         << "<div class=\"tab-panel\" data-tab-panel=\"settings\"><div class=\"settings-stack\">"
         << "<section><div class=\"section-heading\"><h2>声音与指示灯</h2>"
         << "<span class=\"effect-badge preview\">试听即时 · 保存后默认</span></div>"
         << "<p class=\"setting-help\">滑杆用于现场预览；保存后成为设备默认值。LED 亮度或左右角色发生变化时，设备会自动执行完整 RGB 检测。</p>"
         << "<form class=\"settings-grid grid two\" data-settings-group=\"sound-led\" data-success=\"声音与指示灯设置已保存。\" method=\"post\" action=\"/api/settings\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><label>音量 <span id=\"volume-value\">"
         << static_cast<unsigned int>(settings_.volume)
         << "%</span><input type=\"range\" name=\"volume\" min=\"0\" max=\"100\" value=\""
         << static_cast<unsigned int>(settings_.volume) << "\"><span class=\"field-note\">松开滑杆立即播放短提示音；保存后用于后续语音。</span></label>"
         << "<input type=\"hidden\" name=\"voice_assistance_present\" value=\"1\">"
         << "<label class=\"check-row\"><input type=\"checkbox\" name=\"voice_assistance\" value=\"1\""
         << (settings_.voiceAssistanceEnabled ? " checked" : "")
         << ">启用语音辅助提示<span class=\"field-note\">关闭本地操作播报，不影响主动语音对话。</span></label>"
         << "<label>LED 最大亮度 <span id=\"led-brightness-value\">"
         << static_cast<unsigned int>(settings_.ledMaximumBrightnessPercent)
         << "%</span><input type=\"range\" name=\"led_brightness\" min=\"1\" max=\"100\" value=\""
         << static_cast<unsigned int>(settings_.ledMaximumBrightnessPercent)
         << "\"><span class=\"field-note\">保存后立即应用，并可靠排队一次完整 RGB 检测。</span></label>"
         << "<label>左右灯角色 <select name=\"led_swap\"><option value=\"0\""
         << (!settings_.ledRolesSwapped ? " selected" : "")
         << ">默认</option><option value=\"1\""
         << (settings_.ledRolesSwapped ? " selected" : "")
         << ">交换</option></select><span class=\"field-note\">修改后同样会自动运行 RGB 检测。</span></label>"
         << "<button class=\"wide-action\" type=\"submit\">保存声音与指示灯</button></form></section>"

         << "<section><div class=\"section-heading\"><h2>智能体与图片生成</h2>"
         << "<span class=\"effect-badge next\">下次对话 / 生成生效</span></div>"
         << "<p class=\"setting-help\">这些内容只影响后续 MyAI 对话、生成和浏览器上传画布，不会主动刷新当前墨水屏。</p>"
         << "<form class=\"settings-grid grid two\" data-settings-group=\"ai-image\" data-reload=\"1\" data-success=\"AI 与图片生成设置已保存，下次请求开始使用。\" method=\"post\" action=\"/api/settings\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><label class=\"wide\">智能体提示词（设备身份、人格与本地工具） <textarea name=\"assistant_prompt\" maxlength=\"512\">"
         << htmlEscape(settings_.assistantPrompt) << "</textarea></label>"
         << "<label class=\"wide\">AIGC 图片提示词模板（与负面提示词分开） <textarea name=\"image_prompt_template\" maxlength=\"512\">"
         << htmlEscape(settings_.imagePromptTemplate) << "</textarea>"
         << "<span class=\"field-note\">建议保留 {prompt} 作为主题位置；默认针对底边朝下的 400×600 六色屏、鲜艳高对比、大色块和少细字。</span></label>"
         << "<label>图片尺寸 <select name=\"image_size\"><option value=\"400x600\""
         << (settings_.image.width == 400 ? " selected" : "")
         << ">400×600</option><option value=\"600x400\""
         << (settings_.image.width == 600 ? " selected" : "")
         << ">600×400</option></select></label>"
         << "<label>生成步数 <input type=\"number\" name=\"image_steps\" min=\"1\" max=\"50\" value=\""
         << static_cast<unsigned int>(settings_.image.steps) << "\"></label>"
         << "<label class=\"wide\">负面提示词 <textarea name=\"negative_prompt\" maxlength=\"384\">"
         << htmlEscape(settings_.image.negativePrompt) << "</textarea></label>"
         << "<button class=\"wide-action\" type=\"submit\">保存 AI 与图片设置</button></form></section>"

         << "<section><div class=\"section-heading\"><h2>显示与电源</h2>"
         << "<span class=\"effect-badge\">保存后立即生效</span></div>"
         << "<p class=\"setting-help\">调整后不会为展示中间状态而额外刷屏；新策略用于下一次正式上屏或休眠判断。</p>"
         << "<form class=\"settings-grid grid two\" data-settings-group=\"display-power\" data-success=\"显示与电源设置已保存。\" method=\"post\" action=\"/api/settings\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><label>默认刷新策略 <select name=\"refresh_mode\"><option value=\"official-quality\""
         << (settings_.refreshMode == RefreshMode::OfficialQuality ? " selected" : "")
         << ">官方画质</option><option value=\"classic-six-color\""
         << (settings_.refreshMode == RefreshMode::ExperimentalSixColor ? " selected" : "")
         << ">经典六色抖色</option><option value=\"reflectance-photo\""
         << (settings_.refreshMode == RefreshMode::ReflectancePhoto ? " selected" : "")
         << ">反射率照片（推荐照片）</option><option value=\"solid-clean\""
         << (settings_.refreshMode == RefreshMode::SolidClean ? " selected" : "")
         << ">纯色清晰（文字/表格）</option></select>"
         << "<span class=\"field-note\">相册中的单张图片可覆盖这个默认值。</span></label>"
         << "<label>电源模式 <select name=\"power_mode\"><option value=\"compatibility\""
         << (settings_.powerMode == PowerMode::Compatibility ? " selected" : "")
         << ">兼容模式</option><option value=\"battery\""
         << (settings_.powerMode == PowerMode::Battery ? " selected" : "")
         << ">电池模式</option></select></label>"
         << "<label>空闲休眠秒数 <input type=\"number\" name=\"idle_timeout\" min=\"120\" max=\"3600\" value=\""
         << settings_.idleTimeoutSeconds << "\"></label>"
         << "<button class=\"wide-action\" type=\"submit\">保存显示与电源</button></form></section>"

         << "<section><div class=\"section-heading\"><h2>存储</h2>"
         << "<span class=\"effect-badge\">保存后立即切换</span></div>"
         << "<p class=\"setting-help\">当前实际后端：<strong>" << activeStorageName(storage.activeBackend)
         << "</strong>；已用 " << humanBytes(activeUsed) << "，剩余 "
         << humanBytes(storage.activeFreeBytes) << "，总计 "
         << humanBytes(storage.activeTotalBytes) << "。</p>"
         << "<form class=\"settings-grid\" data-settings-group=\"storage\" data-success=\"存储位置已保存。\" method=\"post\" action=\"/api/settings\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><label>素材存储位置 <select name=\"storage\"><option value=\"auto\""
         << (settings_.storageTarget == StorageTarget::Automatic ? " selected" : "")
         << ">自动</option><option value=\"internal\""
         << (settings_.storageTarget == StorageTarget::Internal ? " selected" : "")
         << ">内置</option><option value=\"sd\""
         << (settings_.storageTarget == StorageTarget::SdCard ? " selected" : "")
         << ">TF / SD</option></select></label>"
         << "<button type=\"submit\">保存存储位置</button></form></section>"

         << "<section class=\"maintenance\"><div class=\"section-heading\"><h2>存储维护</h2>"
         << "<span class=\"effect-badge restart\">危险操作 · 实体确认</span></div>"
         << "<p class=\"setting-help\">格式化会删除 TF / SD 数据，需要浏览器二次确认，并在 30 秒内短按设备顶部语音键。</p>"
         << "<form data-portal=\"1\" data-destructive=\"1\" method=\"post\" action=\"/api/actions/prepare\"><input type=\"hidden\" name=\"_csrf\" value=\""
         << htmlEscape(access_.csrfToken) << "\"><input type=\"hidden\" name=\"action\" value=\"format_sd\">"
         << "<button class=\"danger\" type=\"submit\">格式化 TF / SD</button></form></section>"

         << "<section><div class=\"section-heading\"><h2>本地访问密码</h2>"
         << "<span class=\"effect-badge restart\">保存后重启生效</span></div>"
         << "<p class=\"setting-help\">当前默认与已保存的家庭 Wi-Fi 密码一致；你也可以改成任意 8–63 位新密码。它同时用于 Settings Wi-Fi 和本地网页登录。</p>"
         << "<form class=\"settings-grid grid two\" data-settings-group=\"password\" method=\"post\" action=\"/api/settings\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><label>新本地管理密码 <input type=\"password\" name=\"local_password\" minlength=\"8\" maxlength=\"63\" autocomplete=\"new-password\" required></label>"
         << "<label>再次输入新密码 <input type=\"password\" name=\"local_password_confirm\" minlength=\"8\" maxlength=\"63\" autocomplete=\"new-password\" required></label>"
         << "<p class=\"wide warning\">不要求大小写或特殊字符；只需满足 Wi-Fi 的 8–63 位长度。保存后请重启设备。</p>"
         << "<button class=\"wide-action\" type=\"submit\">保存新密码</button></form></section>"
         << "<section><h2>教程与诊断</h2><form data-portal=\"1\" method=\"post\" action=\"/api/tutorial/restart\">"
         << "<input type=\"hidden\" name=\"_csrf\" value=\"" << htmlEscape(access_.csrfToken)
         << "\"><button type=\"submit\">重播语音教程</button></form>"
         << "<p><a href=\"/api/diagnostics\">查看诊断</a> · <a href=\"/api/serial-log\">导出串口日志</a></p></section>"
         << "<section><h2>安全说明</h2><p>所有修改请求都需要本地会话、同源 Origin 和 CSRF。"
         << "格式化、删图和清空相册还必须完成浏览器二次确认，并在 30 秒内短按设备顶部语音键 "
         << "BtnC / GPIO1；左右翻页键不会确认。</p>"
         << "</section></div></div></main><script src=\"/portal.js\"></script>"
         << "</body></html>";
  if (!boundedAppend(&output, suffix.str(), kMaximumDashboardHtmlBytes))
    return albumReadError(AlbumReadStatus::TooLarge, true);
  return htmlResponse(200, output);
}

}  // namespace portal
}  // namespace inkloop
