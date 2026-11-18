// =====================================================================
//  uiot_api.h — 云 API 层(全部为阻塞式 HTTPS,只在后台核调用)
//  · OAuth2 登录 + token 缓存/刷新 + NTP 对时
//  · 单一 apiPost():统一做 AES 加密 body + MD5 签名 + 发送
//  · 设备控制 / 场景触发 / 拉设备清单(均复用 apiPost)
// =====================================================================
#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"
#include "codec.h"

static String        g_token      = "";
static unsigned long g_tokenExpMs = 0;      // millis() 到期点
static bool          g_ntpDone    = false;

// 确保已对时并持有有效 token(必要时登录)。仅在已连 WiFi 时有效。
static bool ensureReady() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (!g_ntpDone) {
    configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org");
    for (int i = 0; i < 20 && time(nullptr) < 1700000000; i++) delay(200);
    g_ntpDone = (time(nullptr) > 1700000000);
    Serial.printf("[UIOT] NTP %s epoch=%ld\n", g_ntpDone ? "ok" : "FAIL", (long)time(nullptr));
  }
  if (g_token.length() && millis() < g_tokenExpMs) { g_loggedIn = true; return true; }

  // OAuth2 password grant(参数走 query,body 空)
  String url = String(AUTHURL) + "?client_id=" + APP_KEY + "&client_secret=" + APP_SECRET +
               "&username=" + g_user + "&password=" + g_pwd + "&grant_type=password&scope=read%20write";
  WiFiClientSecure sec; sec.setInsecure();
  HTTPClient https; https.begin(sec, url);
  int code = https.POST("");
  String resp = https.getString();
  https.end();
  if (code != 200) { g_loggedIn = false; Serial.printf("[UIOT] login http=%d\n", code); return false; }

  int p = resp.indexOf("\"access_token\":\"");
  if (p < 0) { g_loggedIn = false; return false; }
  p += 16;
  g_token = resp.substring(p, resp.indexOf('"', p));
  long expires = 3600;
  int ep = resp.indexOf("\"expires_in\":");
  if (ep >= 0) expires = resp.substring(ep + 13).toInt();
  g_tokenExpMs = millis() + (unsigned long)(expires > 120 ? expires - 60 : expires) * 1000UL;
  g_loggedIn = true;
  Serial.printf("[UIOT] token ok expires=%ld\n", expires);
  return true;
}

// 统一的已签名 openapi 请求:AES 加密 body→data,MD5 签名,发送。
// 返回 HTTP code;respOut(可选)收原始响应体。
static int apiPost(const String& method, const String& body, String* respOut = nullptr) {
  if (!ensureReady()) return -1;
  String data = aesEcbB64(body);
  String ts   = nowMs();
  String sign = md5Hex(String("accessToken=") + g_token + "&appkey=" + APP_KEY +
                       "&data=" + data + "&isEncrypt=true&method=" + method +
                       "&timestamp=" + ts + "&version=1.0" + APP_SECRET);
  WiFiClientSecure sec; sec.setInsecure();
  HTTPClient https; https.begin(sec, GATEWAY);
  https.addHeader("Content-Type", "application/json; charset=utf-8");
  https.addHeader("method", method);     https.addHeader("appkey", APP_KEY);
  https.addHeader("timestamp", ts);      https.addHeader("version", "1.0");
  https.addHeader("isEncrypt", "true");  https.addHeader("accessToken", g_token);
  https.addHeader("data", data);         https.addHeader("sign", sign);
  int code = https.POST(data);
  if (respOut) *respOut = https.getString();
  https.end();
  return code;
}

// 公共 body 前缀:"thirdSn":..,"appKey":..,"sn":..
static String uiotBodyBase() {
  return String("\"thirdSn\":\"") + g_hostsn + "\",\"appKey\":\"" + APP_KEY +
         "\",\"sn\":\"" + g_hostsn + "\"";
}

// 控制单个设备。props 为一段 JSON 片段,如 {"powerSwitch":"on"}。
// 注意:UIOT 网关「只支持单属性控制」,多属性需拆成多次调用。
static int uiotControl(const char* deviceId, const String& props) {
  String body = "{" + uiotBodyBase() + ",\"deviceId\":\"" + deviceId + "\",\"properties\":" + props + "}";
  int code = apiPost("uiotsoft.openapi.device.control", body);
  Serial.printf("[UIOT] ctrl %s %s http=%d\n", deviceId, props.c_str(), code);
  return code;
}

// 触发场景(smartId)。
static int uiotSceneControl(int smartId) {
  String body = "{" + uiotBodyBase() + ",\"smartId\":" + smartId + "}";
  return apiPost("uiotsoft.openapi.smart.control", body);
}

// 拉取并解密 device.list,返回解密后的 JSON 字符串(发现 + 状态同步共用)。
static bool fetchDeviceListRaw(String& out) {
  String resp;
  int code = apiPost("uiotsoft.openapi.device.list", "{" + uiotBodyBase() + "}", &resp);
  if (code != 200) { Serial.printf("[LIST] http=%d\n", code); return false; }
  int dp = resp.indexOf("\"data\":\"");
  if (dp < 0) return false;
  dp += 8;
  out = aesEcbDecrypt(resp.substring(dp, resp.indexOf('"', dp)));
  return out.length() > 0;
}
