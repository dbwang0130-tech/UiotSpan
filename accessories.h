// =====================================================================
//  accessories.h — HomeKit 适配层
//  · 把 UIOT 设备映射为 HomeSpan 配件(窗帘/灯/空调/地暖/新风/传感器)
//  · 配件的 update() 只把控制意图入队(g_cmdQ),由后台核真正发 HTTP
//  · apply*() 由主线程调用,把云端状态 setVal 回写给 HomeKit
//  线程约定:HomeSpan 非线程安全 → 所有 setVal 只在主线程发生。
// =====================================================================
#pragma once
#include "HomeSpan.h"
#include "codec.h"

// -------- 主线程 → 后台核:控制命令队列 --------
struct CmdItem { char dev[16]; char props[72]; bool scene; };   // 设备控制 或 场景触发
static QueueHandle_t g_cmdQ = nullptr;
static void enqueueCtrl(const char* dev, const char* props) {
  CmdItem c; strlcpy(c.dev, dev, sizeof c.dev);
  strlcpy(c.props, props, sizeof c.props); c.scene = false;
  if (g_cmdQ) xQueueSend(g_cmdQ, &c, 0);
}
static void enqueueScene(int smartId) {
  CmdItem c; snprintf(c.dev, sizeof c.dev, "%d", smartId);
  c.props[0] = 0; c.scene = true;
  if (g_cmdQ) xQueueSend(g_cmdQ, &c, 0);
}

// -------- 后台核 → 主线程:状态回写队列 --------
// kind: 0=窗帘位置 1=灯on/off 2=空调(props) 3=新风(props) 4=传感器(props)
struct ApplyItem { char dev[12]; int pos; uint8_t kind; char props[128]; };
static QueueHandle_t g_applyQ = nullptr;

// -------- 配件登记表(供状态回写按 deviceId 查找) --------
struct UiotCurtain;  static UiotCurtain* g_curtains[16]; static int g_curtainCount = 0;
struct UiotLight;    static UiotLight*   g_lights[48];   static int g_lightCount   = 0;
struct UiotClimate;  static UiotClimate* g_climates[16]; static int g_climateCount = 0;
struct UiotFanDev;   static UiotFanDev*  g_fans[8];      static int g_fanCount     = 0;
struct SensorBase { const char* devId; unsigned long lastLiveMs = 0;
                    virtual void applyProps(const String& s) = 0; };
static SensorBase*  g_sensors[48]; static int g_sensorCount = 0;

static const unsigned long LIVE_GRACE_MS = 60000;   // 最近有实时推送则兜底跳过,不覆盖

// =====================================================================
//  窗帘 — WindowCovering(0=全关,100=全开,与 UIOT 同义)
// =====================================================================
struct UiotCurtain : Service::WindowCovering {
  const char* devId; unsigned long lastLiveMs = 0;
  int pendingTarget = -1; unsigned long pendingMs = 0;     // 去抖:只记最新目标
  Characteristic::CurrentPosition cur{0};
  Characteristic::TargetPosition  tgt{0};
  Characteristic::PositionState   state{2};               // 0关闭中 1打开中 2停止
  UiotCurtain(const char* id) { devId = id; if (g_curtainCount < 16) g_curtains[g_curtainCount++] = this; }
  boolean update() override {
    if (tgt.updated()) { pendingTarget = tgt.getNewVal(); pendingMs = millis(); lastLiveMs = millis(); }
    return true;
  }
  // 停手 250ms 后发最新目标(合并快速拖动;新目标直接覆盖旧的 → 立即改向,符合 HAP)
  void flushPending() {
    if (pendingTarget >= 0 && (millis() - pendingMs) > 250) {
      int t = pendingTarget; pendingTarget = -1;
      enqueueCtrl(devId, (String("{\"curtainPosition\":\"") + t + "\"}").c_str());
      lastLiveMs = millis();
    }
  }
};

// =====================================================================
//  灯 / 开关 — LightBulb(on/off)
// =====================================================================
struct UiotLight : Service::LightBulb {
  const char* devId; unsigned long lastLiveMs = 0;
  Characteristic::On on{0};
  UiotLight(const char* id) { devId = id; if (g_lightCount < 48) g_lights[g_lightCount++] = this; }
  boolean update() override {
    if (on.updated()) {
      enqueueCtrl(devId, on.getNewVal() ? "{\"powerSwitch\":\"on\"}" : "{\"powerSwitch\":\"off\"}");
      lastLiveMs = millis();
    }
    return true;
  }
  void applyPower(const String& s) {
    String p = jsonStr(s, "powerSwitch");
    if (p == "on") on.setVal(true); else if (p == "off") on.setVal(false);
  }
};

// =====================================================================
//  场景 — Switch(一次性触发,自动复位)。默认不实例化(用户选择不入 HomeKit)。
// =====================================================================
struct UiotScene : Service::Switch {
  int smartId; Characteristic::On on{0};
  UiotScene(int sid) : smartId(sid) {}
  boolean update() override {
    if (on.updated() && on.getNewVal()) { enqueueScene(smartId); on.setVal(false); }
    return true;
  }
};

// =====================================================================
//  空调 / 地暖 — Thermostat(heatOnly=true 仅制热,用于地暖)
//  注意:UIOT 网关「只支持单属性控制」,开机+模式需拆成两条命令。
// =====================================================================
struct UiotClimate : Service::Thermostat {
  const char* devId; bool heatOnly; unsigned long lastLiveMs = 0;
  Characteristic::CurrentHeatingCoolingState curState{0};
  Characteristic::TargetHeatingCoolingState  tgtState{0};
  Characteristic::CurrentTemperature curTemp{22};
  Characteristic::TargetTemperature  tgtTemp{22};
  Characteristic::TemperatureDisplayUnits units{0};
  UiotClimate(const char* id, bool ho) : devId(id), heatOnly(ho) {
    if (g_climateCount < 16) g_climates[g_climateCount++] = this;
    tgtTemp.setRange(ho ? 5 : 16, ho ? 45 : 32, 1);
    curTemp.setRange(0, 50, 1);
    if (ho) tgtState.setValidValues(2, 0, 1);               // 地暖仅 off/heat
  }
  boolean update() override {
    if (tgtState.updated()) {
      int m = tgtState.getNewVal();
      if (m == 0) enqueueCtrl(devId, "{\"powerSwitch\":\"off\"}");
      else {
        enqueueCtrl(devId, "{\"powerSwitch\":\"on\"}");      // 单属性:先开机
        if (!heatOnly) {
          const char* mode = (m == 1) ? "heat" : (m == 2) ? "cool" : "auto";
          enqueueCtrl(devId, (String("{\"thermostatMode\":\"") + mode + "\"}").c_str());
        }
      }
      lastLiveMs = millis();
    }
    if (tgtTemp.updated()) {
      int t = (int)(tgtTemp.getNewVal<float>() + 0.5f);
      enqueueCtrl(devId, (String("{\"targetTemperature\":\"") + t + "\"}").c_str());
      lastLiveMs = millis();
    }
    return true;
  }
  void applyProps(const String& s) {
    bool okt; String tt = jsonNum(s, "targetTemperature", &okt);
    if (okt) tgtTemp.setVal((float)tt.toInt());
    bool okc; String ct = jsonNum(s, "currentTemperature", &okc);
    if (okc && ct.toInt() > 0) curTemp.setVal((float)ct.toInt());
    String ps = jsonStr(s, "powerSwitch");
    String mode = jsonStr(s, "thermostatMode"); if (!mode.length()) mode = jsonStr(s, "workMode");
    if (ps == "off") { tgtState.setVal(0); curState.setVal(0); return; }
    if (ps == "on" || mode.length()) {
      int ts, cs;
      if (heatOnly)            { ts = 1; cs = 1; }
      else if (mode == "cool") { ts = 2; cs = 2; }
      else if (mode == "heat") { ts = 1; cs = 1; }
      else if (mode.length())  { ts = 3; cs = 0; }           // fan/dry/auto
      else { ts = tgtState.getVal(); if (ts == 0) ts = 3; cs = (ts == 1) ? 1 : (ts == 2) ? 2 : 0; }
      tgtState.setVal(ts); curState.setVal(cs);
    }
  }
};

// =====================================================================
//  新风 — Fan(On + 风速 RotationSpeed → off/low/mid/high)
// =====================================================================
struct UiotFanDev : Service::Fan {
  const char* devId; unsigned long lastLiveMs = 0;
  Characteristic::On on{0};
  Characteristic::RotationSpeed speed{0};
  UiotFanDev(const char* id) : devId(id) {
    if (g_fanCount < 8) g_fans[g_fanCount++] = this;
    speed.setRange(0, 100, 1);
  }
  boolean update() override {
    bool o  = on.updated()    ? on.getNewVal()        : on.getVal();
    int  sp = speed.updated() ? speed.getNewVal<int>() : speed.getVal<int>();
    if (!o || sp == 0) enqueueCtrl(devId, "{\"powerSwitch\":\"off\"}");
    else {
      enqueueCtrl(devId, "{\"powerSwitch\":\"on\"}");         // 单属性:先开
      const char* ws = (sp <= 33) ? "low" : (sp <= 66) ? "mid" : "high";
      enqueueCtrl(devId, (String("{\"windSpeed\":\"") + ws + "\"}").c_str());
    }
    lastLiveMs = millis();
    return true;
  }
  void applyProps(const String& s) {
    String ps = jsonStr(s, "powerSwitch"), ws = jsonStr(s, "windSpeed");
    if (ps.length()) { on.setVal(ps == "on"); if (ps == "off") speed.setVal(0); }
    if (ws.length()) {
      int sp = (ws == "off") ? 0 : (ws == "low") ? 33 : (ws == "mid") ? 66
             : (ws == "high") ? 100 : (ws == "auto") ? 100 : 50;
      speed.setVal(sp); if (sp > 0) on.setVal(true);
    }
  }
};

// =====================================================================
//  只读传感器 — 门窗磁 / 人体·幕帘 / 漏水 / 煤气
// =====================================================================
struct UiotContact : Service::ContactSensor, SensorBase {
  Characteristic::ContactSensorState st{0};
  UiotContact(const char* id) { devId = id; if (g_sensorCount < 48) g_sensors[g_sensorCount++] = this; }
  void applyProps(const String& s) override {
    String c = jsonStr(s, "contactState");
    if (c == "close") st.setVal(0); else if (c == "open") st.setVal(1);   // 0闭合 1断开
  }
};
struct UiotMotion : Service::MotionSensor, SensorBase {
  Characteristic::MotionDetected md{false};
  UiotMotion(const char* id) { devId = id; if (g_sensorCount < 48) g_sensors[g_sensorCount++] = this; }
  void applyProps(const String& s) override {
    String h = jsonStr(s, "humanDetectedState"); if (!h.length()) h = jsonStr(s, "someonePass");
    String ca = jsonStr(s, "curtainAlarmState");
    if (h.length())       md.setVal(h == "havePerson" || h == "someone");
    else if (ca.length()) md.setVal(ca == "alarm");
  }
};
struct UiotLeak : Service::LeakSensor, SensorBase {
  Characteristic::LeakDetected ld{0};
  UiotLeak(const char* id) { devId = id; if (g_sensorCount < 48) g_sensors[g_sensorCount++] = this; }
  void applyProps(const String& s) override {
    String a = jsonStr(s, "alarmState"); if (a.length()) ld.setVal(a == "alarm" ? 1 : 0);
  }
};
struct UiotSmoke : Service::SmokeSensor, SensorBase {
  Characteristic::SmokeDetected sd{0};
  UiotSmoke(const char* id) { devId = id; if (g_sensorCount < 48) g_sensors[g_sensorCount++] = this; }
  void applyProps(const String& s) override {
    String a = jsonStr(s, "alarmState"); if (a.length()) sd.setVal(a == "alarm" ? 1 : 0);
  }
};

// =====================================================================
//  主线程状态回写(由 loop 排空 g_applyQ 调用);LIVE_GRACE_MS 内有实时
//  推送则跳过,避免兜底全量覆盖运动中/刚动完的状态。
// =====================================================================
static inline bool graceActive(unsigned long t) { return t != 0 && (millis() - t) < LIVE_GRACE_MS; }

static void applyPosition(const String& devId, int pos) {
  if (pos < 0) pos = 0; if (pos > 100) pos = 100;
  for (int i = 0; i < g_curtainCount; i++)
    if (devId == String(g_curtains[i]->devId)) {
      if (graceActive(g_curtains[i]->lastLiveMs)) return;
      g_curtains[i]->cur.setVal(pos); g_curtains[i]->tgt.setVal(pos); g_curtains[i]->state.setVal(2);
    }
}
static void applyLight(const String& devId, int on) {
  for (int i = 0; i < g_lightCount; i++)
    if (devId == String(g_lights[i]->devId)) {
      if (graceActive(g_lights[i]->lastLiveMs)) return;
      g_lights[i]->on.setVal(on ? true : false);
    }
}
static void applyClimate(const String& devId, const String& props) {
  for (int i = 0; i < g_climateCount; i++)
    if (devId == String(g_climates[i]->devId)) {
      if (graceActive(g_climates[i]->lastLiveMs)) return;
      g_climates[i]->applyProps(props);
    }
}
static void applyFan(const String& devId, const String& props) {
  for (int i = 0; i < g_fanCount; i++)
    if (devId == String(g_fans[i]->devId)) {
      if (graceActive(g_fans[i]->lastLiveMs)) return;
      g_fans[i]->applyProps(props);
    }
}
static void applySensor(const String& devId, const String& props) {
  for (int i = 0; i < g_sensorCount; i++)
    if (devId == String(g_sensors[i]->devId)) g_sensors[i]->applyProps(props);
}
