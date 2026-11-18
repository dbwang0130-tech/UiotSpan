// =====================================================================
//  UiotSpan — 把 UIOT(超级智慧家/紫光物联)整套设备接入 Apple HomeKit
//  的独立网桥。ESP32-S3 + HomeSpan,走 UIOT 云(OAuth + 云 MQTT)。
//
//  分层(均为 header-only,按依赖顺序 include,单一编译单元):
//    config.h      配置/配网门户      codec.h    编解码(AES/MD5/JSON)
//    uiot_api.h    云 API(登录/控制)  accessories.h  HomeKit 配件
//    cloud.h       MQTT/发现/后台任务
//
//  双核:阻塞式云 HTTP 跑在 core 0(cloudTask),主循环(core 1)只做
//  HomeKit + MQTT,互不阻塞。详见 README。
// =====================================================================
#include "config.h"
#include "codec.h"
#include "uiot_api.h"
#include "accessories.h"
#include "cloud.h"

// 开机后数秒内按一次 BOOT(GPIO0)强制进配网。(不能复位时按住——那会进下载模式。)
static void checkBootButton() {
  pinMode(0, INPUT_PULLUP);
  Serial.println("[CFG] 3 秒内按 BOOT 可进入配网");
  for (unsigned long t = millis(); millis() - t < 3000; ) {
    if (digitalRead(0) == LOW) { delay(40); if (digitalRead(0) == LOW) runConfigPortal(); }
    delay(10);
  }
}

// 拉完整设备清单(重试,确保开机设备集完整 → AID 稳定,否则 HomeKit 恒"未响应")
static bool fetchListWithRetry(String& list) {
  for (int r = 0; r < 6; r++) {
    list = "";
    if (WiFi.status() == WL_CONNECTED && fetchDeviceListRaw(list) && list.length() > 1000) return true;
    Serial.printf("[BOOT] device.list 重试 %d\n", r + 1);
    delay(1500);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  loadConfig();
  checkBootButton();
  if (!configComplete()) { Serial.println("[CFG] 无配置 → 配网"); runConfigPortal(); }

  WiFi.mode(WIFI_STA);
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[CFG] WiFi 失败 → 配网"); runConfigPortal(); }

  String list;
  bool got = fetchListWithRetry(list);

  homeSpan.setWifiCredentials(g_ssid.c_str(), g_pass.c_str());
  homeSpan.begin(Category::Bridges, "UiotSpan");

  new SpanAccessory();                          // 桥本体(AID 1)
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("UiotSpan");
  if (got) discoverAll(list);
  else     Serial.println("[DISCOVER] device.list 获取失败;重启可重试");

  g_mqttSec.setInsecure();
  g_mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  g_mqtt.setBufferSize(4096);
  g_mqtt.setKeepAlive(60);
  g_mqtt.setCallback(mqttCallback);

  startStatusWeb();

  g_cmdQ   = xQueueCreate(16, sizeof(CmdItem));
  g_applyQ = xQueueCreate(16, sizeof(ApplyItem));
  xTaskCreatePinnedToCore(cloudTask, "cloud", 16384, nullptr, 1, nullptr, 0);  // core 0
}

void loop() {
  homeSpan.poll();
  g_web.handleClient();

  for (int i = 0; i < g_curtainCount; i++) g_curtains[i]->flushPending();   // 去抖后入队

  ApplyItem a;                                  // 后台拉到的全量状态,主线程回写
  while (xQueueReceive(g_applyQ, &a, 0) == pdTRUE) {
    if      (a.kind == 1) applyLight(a.dev, a.pos);
    else if (a.kind == 2) applyClimate(a.dev, a.props);
    else if (a.kind == 3) applyFan(a.dev, a.props);
    else if (a.kind == 4) applySensor(a.dev, a.props);
    else                  applyPosition(a.dev, a.pos);
  }

  if (WiFi.status() == WL_CONNECTED) {          // 维护 MQTT 长连接(永不被 HTTP 阻塞)
    if (!g_mqtt.connected()) mqttReconnect();
    g_mqtt.loop();
  }
}
