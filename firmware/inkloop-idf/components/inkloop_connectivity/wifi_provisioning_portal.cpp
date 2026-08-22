#include "inkloop/wifi_provisioning_portal.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace inkloop {
namespace {

WifiProvisioningResponse response(int status, std::string body,
                                  const char* content_type =
                                      "application/json; charset=utf-8") {
  WifiProvisioningResponse value;
  value.status = status;
  value.content_type = content_type;
  value.body = std::move(body);
  return value;
}

int hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decode(const std::string& value, size_t maximum, std::string& output) {
  output.clear();
  for (size_t at = 0; at < value.size(); ++at) {
    unsigned char ch = static_cast<unsigned char>(value[at]);
    if (ch == '+') ch = ' ';
    else if (ch == '%') {
      if (at + 2U >= value.size()) return false;
      const int high = hex(value[at + 1U]);
      const int low = hex(value[at + 2U]);
      if (high < 0 || low < 0) return false;
      ch = static_cast<unsigned char>((high << 4U) | low);
      at += 2U;
    }
    if (ch == 0U || output.size() >= maximum) return false;
    output.push_back(static_cast<char>(ch));
  }
  return true;
}

bool parseCredentials(const std::string& body, std::string& ssid,
                      std::string& password) {
  bool saw_ssid = false;
  bool saw_password = false;
  size_t start = 0;
  while (start <= body.size()) {
    const size_t separator = body.find('&', start);
    const size_t end = separator == std::string::npos ? body.size() : separator;
    if (start == end) return false;
    const size_t equal = body.find('=', start);
    if (equal == std::string::npos || equal >= end) return false;
    const std::string key = body.substr(start, equal - start);
    const std::string encoded = body.substr(equal + 1U, end - equal - 1U);
    if (key == "ssid" && !saw_ssid) {
      if (!decode(encoded, kMaximumWifiSsidBytes, ssid)) return false;
      saw_ssid = true;
    } else if (key == "password" && !saw_password) {
      if (!decode(encoded, kMaximumWifiPasswordBytes, password)) return false;
      saw_password = true;
    } else {
      return false;
    }
    if (separator == std::string::npos) break;
    start = separator + 1U;
  }
  return saw_ssid && saw_password;
}

bool formContentType(const std::string& value) {
  static constexpr char kForm[] = "application/x-www-form-urlencoded";
  return value == kForm || value.rfind(std::string(kForm) + ";", 0) == 0;
}

bool validUtf8NoControl(const std::string& value) {
  size_t at = 0;
  while (at < value.size()) {
    const uint8_t first = static_cast<uint8_t>(value[at]);
    if (first < 0x80U) {
      if (first < 0x20U || first == 0x7fU) return false;
      ++at;
      continue;
    }
    size_t continuation = 0;
    uint32_t codepoint = 0;
    if ((first & 0xe0U) == 0xc0U) {
      continuation = 1U;
      codepoint = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
      continuation = 2U;
      codepoint = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
      continuation = 3U;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (continuation > value.size() - at - 1U) return false;
    for (size_t offset = 1U; offset <= continuation; ++offset) {
      const uint8_t next = static_cast<uint8_t>(value[at + offset]);
      if ((next & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    if ((continuation == 1U && codepoint < 0x80U) ||
        (continuation == 2U && codepoint < 0x800U) ||
        (continuation == 3U && codepoint < 0x10000U) ||
        codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      return false;
    }
    at += continuation + 1U;
  }
  return true;
}

}  // namespace

bool WifiProvisioningPortal::validSsid(const std::string& value) {
  return !value.empty() && value.size() <= kMaximumWifiSsidBytes &&
         validUtf8NoControl(value);
}

bool WifiProvisioningPortal::validPassword(const std::string& value) {
  if (!value.empty() && (value.size() < 8U ||
                         value.size() > kMaximumWifiPasswordBytes)) {
    return false;
  }
  for (unsigned char ch : value) {
    if (ch < 0x20U || ch > 0x7EU) return false;
  }
  return true;
}

WifiProvisioningResponse WifiProvisioningPortal::handle(
    const WifiProvisioningRequest& request) {
  if (request.method == "GET" && request.path == "/") {
    return response(200, pageHtml(), "text/html; charset=utf-8");
  }
  if (request.method != "POST" || request.path != "/configure") {
    return response(404, "{\"ok\":false,\"error\":\"route_not_found\"}");
  }
  if (!formContentType(request.content_type)) {
    return response(415,
                    "{\"ok\":false,\"error\":\"form_required\"}");
  }
  if (request.body.empty() ||
      request.body.size() > kMaximumWifiProvisioningBodyBytes) {
    return response(413,
                    "{\"ok\":false,\"error\":\"body_too_large\"}");
  }
  std::string ssid;
  std::string password;
  if (!parseCredentials(request.body, ssid, password) || !validSsid(ssid) ||
      !validPassword(password)) {
    return response(422,
                    "{\"ok\":false,\"error\":\"invalid_credentials\"}");
  }
  const WifiProvisioningSubmitResult submitted =
      sink_.trySubmitCredentials(ssid, password);
  std::fill(password.begin(), password.end(), '\0');
  switch (submitted) {
    case WifiProvisioningSubmitResult::Accepted:
      // Never echo or log the submitted credential.
      return response(202,
          "{\"ok\":true,\"state\":\"saving_and_connecting\"}");
    case WifiProvisioningSubmitResult::Busy:
      return response(409, "{\"ok\":false,\"error\":\"busy\"}");
    case WifiProvisioningSubmitResult::Invalid:
      return response(422,
          "{\"ok\":false,\"error\":\"invalid_credentials\"}");
    case WifiProvisioningSubmitResult::Unavailable:
      return response(503,
          "{\"ok\":false,\"error\":\"provisioning_unavailable\"}");
  }
  return response(500, "{\"ok\":false,\"error\":\"internal\"}");
}

const char* WifiProvisioningPortal::pageHtml() {
  return R"INKLOOP(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Inkloop Wi-Fi</title><style>body{font:16px/1.5 system-ui;margin:0;background:#f5f0df;color:#18201c}main{max-width:520px;margin:8vh auto;padding:24px}form{background:#fffdf5;border:1px solid #c9c1a8;border-radius:14px;padding:20px}label{display:block;margin:14px 0}input{display:block;width:100%;box-sizing:border-box;padding:10px;margin-top:5px}button{padding:10px 18px;border:0;border-radius:9px;background:#18201c;color:white}#status{min-height:1.5em}</style></head><body><main><h1>连接 Wi-Fi</h1><p>输入家庭或办公网络。保存成功后设备会关闭 Settings 热点并连接新网络。</p><form id="wifi"><label>Wi-Fi 名称<input name="ssid" maxlength="32" autocomplete="username" required></label><label>Wi-Fi 密码<input name="password" type="password" minlength="8" maxlength="63" autocomplete="current-password"></label><button>保存并连接</button></form><p id="status"></p></main><script>document.querySelector('#wifi').onsubmit=async e=>{e.preventDefault();const s=document.querySelector('#status');s.textContent='正在保存…';try{const r=await fetch('/configure',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))}),v=await r.json();if(!r.ok||!v.ok)throw Error(v.error||r.status);s.textContent='已保存，设备正在连接。热点将自动关闭。'}catch(x){s.textContent='保存失败：'+x.message}}</script></body></html>)INKLOOP";
}

const char* wifiProvisioningSubmitResultName(
    WifiProvisioningSubmitResult result) {
  switch (result) {
    case WifiProvisioningSubmitResult::Accepted: return "ACCEPTED";
    case WifiProvisioningSubmitResult::Busy: return "BUSY";
    case WifiProvisioningSubmitResult::Invalid: return "INVALID";
    case WifiProvisioningSubmitResult::Unavailable: return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

}  // namespace inkloop
