// =====================================================================
//  config.h — 配置层
//  · UIOT 云端点与应用级常量
//  · 运行时配置(WiFi / UIOT 账号 / 主机 SN)存取于 NVS
//  · 首启/重配用的 Wi-Fi 配网门户(强制门户,自动弹出)
// =====================================================================
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ---- UIOT 云端点 + 应用级常量(UIOT 官方 App 公开,所有用户相同) ----
static const char* APP_KEY     = "8vj99vwcn9qxk295oqmq7q3p2ni417q7";
static const char* APP_SECRET  = "i6AFqgDm8Bg2m9qrvYiqW3BqRhB3NEbN";   // 32B = AES-256 key
static const char* AUTHURL     = "https://oauth.unisiot.com:8165/oauth/oauth/token";
static const char* GATEWAY     = "https://openapi.unisiot.com/gateway";
static const char* MQTT_BROKER = "bmqt.unisiot.com";
static const int   MQTT_PORT   = 51322;

// ---- 运行时配置(经配网门户写入 NVS) ----
static String g_ssid, g_pass;        // WiFi
static String g_user, g_pwd;         // UIOT 账号(SN 后6位) / 密码
static String g_hostsn;              // 主机完整 SN

// ---- 跨模块共享状态标志 ----
static volatile bool g_needSync = true;   // 置位 → 后台任务拉一次全量
static volatile bool g_loggedIn = false;  // 登录状态(供状态页跨线程安全读取)

// ---------------------------- NVS 读写 -------------------------------
static Preferences g_prefs;

static void loadConfig() {
  g_prefs.begin("uiot", true);
  g_ssid   = g_prefs.getString("ssid", "");
  g_pass   = g_prefs.getString("pass", "");
  g_user   = g_prefs.getString("user", "");
  g_pwd    = g_prefs.getString("pwd",  "");
  g_hostsn = g_prefs.getString("sn",   "");
  g_prefs.end();
}
static void saveConfig(const String& ssid, const String& pass, const String& user,
                       const String& pwd, const String& sn) {
  g_prefs.begin("uiot", false);
  g_prefs.putString("ssid", ssid); g_prefs.putString("pass", pass);
  g_prefs.putString("user", user); g_prefs.putString("pwd",  pwd);
  g_prefs.putString("sn",   sn);
  g_prefs.end();
}
static void clearConfig() { g_prefs.begin("uiot", false); g_prefs.clear(); g_prefs.end(); }

static bool configComplete() {
  return g_ssid.length() && g_user.length() && g_hostsn.length();
}

// ---------------------------- 配网门户 -------------------------------
// 扫描周边 WiFi,生成 <select> 选项(去重,带信号强度);选中即填入 SSID 输入框
static String wifiSelect() {
  int n = WiFi.scanNetworks();
  String opts = "<option value=''>— 选择附近 WiFi 或手动输入 —</option>";
  String seen = "|";
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length() || seen.indexOf("|" + ss + "|") >= 0) continue;
    seen += ss + "|";
    opts += "<option value='" + ss + "'>" + ss + "  (" + WiFi.RSSI(i) + "dBm)</option>";
  }
  WiFi.scanDelete();
  return "<select onchange=\"document.getElementsByName('ssid')[0].value=this.value\">" + opts + "</select>";
}

static String cfgPage() {
  String h =
    "<!doctype html><html lang=zh><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>UiotSpan 配置</title>"
    "<style>:root{--a:#10b981;--a2:#059669}*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;"
    "background:linear-gradient(135deg,#0f766e,#064e3b);display:flex;align-items:center;justify-content:center;padding:20px}"
    ".card{width:100%;max-width:400px;background:#fff;border-radius:18px;box-shadow:0 20px 50px rgba(0,0,0,.3);overflow:hidden}"
    ".hd{background:linear-gradient(135deg,var(--a),var(--a2));color:#fff;padding:26px 24px;text-align:center}"
    ".hd .ic{font-size:40px;line-height:1}.hd h1{margin:8px 0 2px;font-size:20px;font-weight:600}.hd p{margin:0;font-size:13px;opacity:.9}"
    ".bd{padding:22px 24px 26px}.grp{margin-bottom:16px}"
    "label{display:block;font-size:13px;font-weight:600;color:#374151;margin-bottom:6px}.hint{font-weight:400;color:#9ca3af}"
    "input,select{width:100%;padding:12px 14px;font-size:16px;border:1.5px solid #e5e7eb;border-radius:10px;outline:none;background:#f9fafb;transition:border-color .15s,box-shadow .15s}"
    "input:focus,select:focus{border-color:var(--a);background:#fff;box-shadow:0 0 0 3px rgba(16,185,129,.15)}"
    "button{width:100%;margin-top:6px;padding:14px;font-size:16px;font-weight:600;color:#fff;border:0;border-radius:10px;background:linear-gradient(135deg,var(--a),var(--a2))}"
    "button:active{transform:translateY(1px)}.ft{text-align:center;font-size:12px;color:#9ca3af;margin-top:14px}</style>"
    "<div class=card><div class=hd><div class=ic>&#127968;</div><h1>UiotSpan</h1><p>连接 WiFi 与 UIOT 账号</p></div>"
    "<div class=bd><form method=POST action=/save>";
  h += "<div class=grp><label>WiFi 名称 <span class=hint>(仅 2.4G · <a href=/ style='color:var(--a);text-decoration:none'>重新扫描</a>)</span></label>"
       + wifiSelect()
       + "<input name=ssid value='" + g_ssid + "' placeholder='选上面或手动输入' style='margin-top:8px'></div>";
  h += "<div class=grp><label>WiFi 密码</label><input name=pass type=password value='" + g_pass + "' placeholder='WiFi密码'></div>";
  h += "<div class=grp><label>UIOT 账号 <span class=hint>(SN 后6位)</span></label><input name=user value='" + g_user + "' placeholder='SN 后6位'></div>";
  h += "<div class=grp><label>UIOT 密码</label><input name=pwd type=password value='" + g_pwd + "' placeholder='UIOT 登录密码'></div>";
  h += "<div class=grp><label>主机完整 SN</label><input name=sn value='" + g_hostsn + "' placeholder='主机序列号'></div>";
  h += "<button type=submit>保存并重启</button><div class=ft>保存后设备自动重启并接入 Apple 家庭</div></form></div></div>";
  return h;
}

// 阻塞式配网:开热点 + 强制门户(DNS 通配 → 系统自动弹出)。提交后写 NVS 并重启。
static void runConfigPortal() {
  WiFi.mode(WIFI_AP_STA);                       // AP_STA:边开热点边可扫描周边 WiFi
  WiFi.softAP("UiotSpan-Setup");
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[CFG] 配网热点 UiotSpan-Setup  ip=%s\n", apIP.toString().c_str());

  DNSServer dns; dns.start(53, "*", apIP);     // 所有域名 → 本机,触发强制门户
  WebServer web(80);
  auto form = [&web]() { web.send(200, "text/html; charset=utf-8", cfgPage()); };
  web.on("/", HTTP_GET, form);
  web.on("/save", HTTP_POST, [&web]() {
    saveConfig(web.arg("ssid"), web.arg("pass"), web.arg("user"), web.arg("pwd"), web.arg("sn"));
    web.send(200, "text/html; charset=utf-8",
      "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
      "font-family:-apple-system,'PingFang SC',sans-serif;background:linear-gradient(135deg,#0f766e,#064e3b);color:#fff;text-align:center}"
      ".b{font-size:48px}h2{font-weight:600;margin:14px 0 6px}p{opacity:.85;font-size:14px;margin:0}</style>"
      "<div><div class=b>&#9989;</div><h2>配置已保存</h2><p>设备正在重启并接入网络…</p></div>");
    delay(1200); ESP.restart();
  });
  web.onNotFound(form);                        // 联网探测地址一律返回配置页
  web.begin();
  for (;;) { dns.processNextRequest(); web.handleClient(); delay(2); }
}
