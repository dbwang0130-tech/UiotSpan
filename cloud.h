// =====================================================================
//  cloud.h — 编排层
//  · 云 MQTT 订阅:实时状态下行(state_report)→ 解密 → 回写 HomeKit
//  · 自动发现:device.list → 按型号/属性归类 → 建配件(云端真实名字)
//  · 全量状态同步:device.list → 解析 → 经 applyQ 交主线程回写
//  · 后台云任务(core 0):承载所有阻塞式 HTTP,主循环永不被阻塞
//  · 诊断状态页(:8080)
// =====================================================================
#pragma once
#include <PubSubClient.h>
#include <WebServer.h>
#include "config.h"
#include "codec.h"
#include "uiot_api.h"
#include "accessories.h"

static WiFiClientSecure g_mqttSec;
static PubSubClient     g_mqtt(g_mqttSec);
static unsigned long    g_mqttNext = 0;
static unsigned long    g_mqttRx   = 0;      // 收包计数(诊断)
static unsigned long    g_lastRxMs = 0;
static WebServer        g_web(8080);         // 诊断状态页

// -------------------- 实时状态下行(主线程) --------------------
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  g_mqttRx++; g_lastRxMs = millis();
  if (String(topic).indexOf("state_report") < 0) return;

  String pl; pl.reserve(len);
  for (unsigned int i = 0; i < len; i++) pl += (char)payload[i];
  int dp = pl.indexOf("\"data\":\"");
  if (dp < 0) return;
  dp += 8;
  String inner = aesEcbDecrypt(pl.substring(dp, pl.indexOf('"', dp)));
  if (!inner.length()) return;

  bool ok, hasPos;
  String devId = jsonNum(inner, "deviceId", &ok);
  if (!ok) return;
  String posS = jsonNum(inner, "curtainPosition", &hasPos);
  int pos = hasPos ? posS.toInt() : -1; if (pos > 100) pos = 100;
  String mst = jsonStr(inner, "motorState");      // 运动指示只认 motorState(同官方)

  for (int i = 0; i < g_curtainCount; i++) {
    UiotCurtain* c = g_curtains[i];
    if (devId != String(c->devId)) continue;
    if (pos >= 0) c->cur.setVal(pos);
    if      (mst == "opening") c->state.setVal(1);
    else if (mst == "closing") c->state.setVal(0);
    else { c->state.setVal(2); if (pos >= 0) c->tgt.setVal(pos); }   // 无 motorState → 停止
    c->lastLiveMs = millis();
  }
  if (jsonStr(inner, "powerSwitch").length())
    for (int i = 0; i < g_lightCount; i++)
      if (devId == String(g_lights[i]->devId)) { g_lights[i]->applyPower(inner); g_lights[i]->lastLiveMs = millis(); }
  for (int i = 0; i < g_climateCount; i++)
    if (devId == String(g_climates[i]->devId)) { g_climates[i]->applyProps(inner); g_climates[i]->lastLiveMs = millis(); }
  for (int i = 0; i < g_fanCount; i++)
    if (devId == String(g_fans[i]->devId)) { g_fans[i]->applyProps(inner); g_fans[i]->lastLiveMs = millis(); }
  for (int i = 0; i < g_sensorCount; i++)
    if (devId == String(g_sensors[i]->devId)) { g_sensors[i]->applyProps(inner); g_sensors[i]->lastLiveMs = millis(); }
}

static void mqttReconnect() {
  if (millis() < g_mqttNext) return;
  g_mqttNext = millis() + 5000;
  String ts   = nowMs();
  String cid  = String("uiotop_sdk@HA_UIOT_") + ts;
  String user = String(APP_KEY) + "|HA_UIOT|thirdCloud|" + ts + "|";
  if (g_mqtt.connect(cid.c_str(), user.c_str(), md5Hex(user + APP_SECRET).c_str())) {
    g_mqtt.subscribe((String("uiotsdk/") + APP_KEY + "/+/" + g_hostsn + "/#").c_str());
    g_mqtt.subscribe((String("uiotop/")  + APP_KEY + "/"   + g_hostsn + "/#").c_str());
    g_needSync = true;                        // 连上后补拉一次全量
    Serial.println("[MQTT] connected");
  } else {
    Serial.printf("[MQTT] connect failed state=%d\n", g_mqtt.state());
  }
}

// -------------------- 全量状态同步(后台核) --------------------
// 解析 device.list,把每个设备的状态经 applyQ 交主线程回写。
static void syncFromList(const String& list) {
  auto region = [&](const String& did, int& p, int& end) -> bool {
    p = findDevicePos(list, did); if (p < 0) return false;
    int nd = list.indexOf("\"deviceId\":", p + 1);
    end = (nd < 0) ? list.length() : nd; return true;
  };
  auto push = [&](const String& did, uint8_t kind, int pos, const String& props) {
    ApplyItem a; a.kind = kind; a.pos = pos; a.props[0] = 0;
    strlcpy(a.dev, did.c_str(), sizeof a.dev);
    if (props.length()) strlcpy(a.props, props.c_str(), sizeof a.props);
    if (g_applyQ) xQueueSend(g_applyQ, &a, 0);
  };
  int p, end;
  for (int i = 0; i < g_curtainCount; i++) {
    String did = g_curtains[i]->devId; if (!region(did, p, end)) continue;
    bool ok; String v = jsonNum(list.substring(p, end), "curtainPosition", &ok);
    if (ok) push(did, 0, v.toInt(), "");
  }
  for (int i = 0; i < g_lightCount; i++) {
    String did = g_lights[i]->devId; if (!region(did, p, end)) continue;
    String v = jsonStr(list.substring(p, end), "powerSwitch");
    if (v == "on" || v == "off") push(did, 1, v == "on" ? 1 : 0, "");
  }
  auto pushProps = [&](const String& did, uint8_t kind) {
    if (!region(did, p, end)) return;
    int pp = list.indexOf("\"properties\":{", p);
    if (pp < 0 || pp >= end) return;
    int st = pp + 13, en = list.indexOf('}', st);
    if (en >= 0) push(did, kind, 0, list.substring(st, en + 1));
  };
  for (int i = 0; i < g_climateCount; i++) pushProps(g_climates[i]->devId, 2);
  for (int i = 0; i < g_fanCount; i++)     pushProps(g_fans[i]->devId, 3);
  for (int i = 0; i < g_sensorCount; i++)  pushProps(g_sensors[i]->devId, 4);
}

// -------------------- 自动发现(setup 内调用) --------------------
// 遍历 device.list,按型号/属性归类,用云端真实 deviceName 创建配件。
// 场景不入 HomeKit(用户选择)。deviceId 作固定 AID,避免增删时编号漂移。
static void discoverAll(const String& list) {
  int from = 0, n = 0;
  while (true) {
    int p = list.indexOf("\"deviceId\":", from);
    if (p < 0) break;
    int nd = list.indexOf("\"deviceId\":", p + 11);
    String region = list.substring(p, (nd < 0) ? list.length() : nd);
    from = (nd < 0) ? (int)list.length() : nd;

    bool ok; String idN = jsonNum(region, "deviceId", &ok);
    if (!ok) continue;
    String nm = jsonStr(region, "deviceName"); if (!nm.length()) nm = String("UIOT ") + idN;
    String model = jsonStr(region, "model");
    auto hp = [&](const char* k) { return region.indexOf(String("\"") + k + "\"") >= 0; };
    auto md = [&](const char* k) { return model.indexOf(k) >= 0; };

    // deviceOnlineState 对休眠 zigbee 设备不可靠(会误报在用设备离线),故不据此过滤。
    bool supported = md("curtain_motor") || hp("curtainPosition") || md("floor_heating") ||
                     md("fresh_air") || md("_ac") || hp("thermostatMode") ||
                     md("door_sensor") || hp("contactState") || md("ir_human") || md("ir_curtain") ||
                     hp("humanDetectedState") || hp("someonePass") || hp("curtainAlarmState") ||
                     md("gas") || md("water_leak") ||
                     ((hp("powerSwitch") || md("switch") || md("light")) && !md("sos") && !md("button") && !md("panel"));
    if (!supported) continue;                 // 电量/工作模式/场景面板/紧急按钮/未知 → 跳过

    char* id = strdup(idN.c_str());           // 永久持有(供 AID/名字)
    char* name = strdup(nm.c_str());
    new SpanAccessory((uint32_t)idN.toInt());  // deviceId 作稳定 AID
      new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(name);
    if      (md("curtain_motor") || hp("curtainPosition"))       new UiotCurtain(id);
    else if (md("floor_heating"))                                new UiotClimate(id, true);
    else if (md("fresh_air"))                                    new UiotFanDev(id);
    else if (md("_ac") || hp("thermostatMode"))                  new UiotClimate(id, false);
    else if (md("door_sensor") || hp("contactState"))            new UiotContact(id);
    else if (md("ir_human") || md("ir_curtain") || hp("humanDetectedState") ||
             hp("someonePass") || hp("curtainAlarmState"))       new UiotMotion(id);
    else if (md("gas"))                                          new UiotSmoke(id);
    else if (md("water_leak"))                                   new UiotLeak(id);
    else                                                         new UiotLight(id);
    n++;
    Serial.printf("[DISCOVER] %s id=%s model=%s\n", name, id, model.c_str());
  }
  Serial.printf("[DISCOVER] 共创建 %d 个配件\n", n);
}

// -------------------- 诊断状态页(:8080) --------------------
static void handleStatus() {
  String j = "{";
  j += "\"wifi\":"   + String(WiFi.status() == WL_CONNECTED ? 1 : 0);
  j += ",\"ip\":\""  + WiFi.localIP().toString() + "\"";
  j += ",\"mqtt\":"  + String(g_mqtt.connected() ? 1 : 0);
  j += ",\"synced\":"+ String(g_needSync ? 0 : 1);
  j += ",\"token\":" + String(g_loggedIn ? 1 : 0);
  j += ",\"mqtt_rx\":" + String(g_mqttRx);
  j += ",\"heap\":"  + String((unsigned)ESP.getFreeHeap());
  j += ",\"psram\":" + String((unsigned)ESP.getFreePsram());
  j += ",\"acc\":{\"cur\":" + String(g_curtainCount) + ",\"light\":" + String(g_lightCount) +
       ",\"climate\":" + String(g_climateCount) + ",\"fan\":" + String(g_fanCount) +
       ",\"sensor\":" + String(g_sensorCount) + "}}";
  g_web.send(200, "application/json", j);
}
static void startStatusWeb() {
  g_web.on("/", handleStatus);
  g_web.on("/reset", []() {                   // 清配置重启 → 回配网门户
    g_web.send(200, "text/html; charset=utf-8", "<meta charset=utf-8>已清除配置,正在重启…");
    delay(600); clearConfig(); ESP.restart();
  });
  g_web.begin();
}

// -------------------- 后台云任务(core 0) --------------------
static void cloudTask(void*) {
  unsigned long lastSync = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      ensureReady();
      if (millis() - lastSync > 300000) g_needSync = true;     // 5 分钟兜底,捡回丢失推送
      if (g_needSync) {
        String list;
        if (fetchDeviceListRaw(list)) { syncFromList(list); g_needSync = false; lastSync = millis(); }
      }
      CmdItem c;
      while (xQueueReceive(g_cmdQ, &c, 0) == pdTRUE) {
        if (c.scene) uiotSceneControl(atoi(c.dev));
        else         uiotControl(c.dev, String(c.props));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
