# UiotSpan

**Bring your UIOT (超级智慧家 / 紫光物联) smart‑home system into Apple HomeKit — on a single ESP32.**

UiotSpan is a standalone bridge that runs on an ESP32‑S3 and exposes the devices
of a UIOT whole‑home system to Apple Home. It talks to the UIOT cloud over the
same channel the official Home Assistant plugin uses (OAuth + cloud MQTT), and
presents everything to HomeKit through [HomeSpan](https://github.com/HomeSpan/HomeSpan).

No Home Assistant, no Raspberry Pi, no hub software — just a ~¥30 microcontroller.

> 中文说明见文末「中文速览」。

---

## Features

- **Zero‑hardcode auto‑discovery** — on boot it pulls the device list from the
  cloud, classifies each device by model, and creates the matching HomeKit
  accessory **using the device's real name**. Add/rename/remove devices in the
  UIOT app, reboot, done.
- **Wi‑Fi captive‑portal provisioning** — no credentials in the firmware. First
  boot opens a `UiotSpan-Setup` hotspot with a config page that **pops up
  automatically**. Re‑configure anytime by tapping **BOOT** within 3 s of power‑up.
- **Real‑time + reliable state** — live push via cloud MQTT, plus a low‑frequency
  full resync as a safety net for dropped telemetry.
- **Dual‑core, non‑blocking** — all blocking cloud HTTP runs on core 0; the main
  loop (core 1) only serves HomeKit + MQTT, so nothing ever stalls or drops.
- **Stable identities** — each accessory's HomeKit AID is derived from its device
  ID, so adding/removing devices never scrambles the others.

## Supported devices

| UIOT device (model)                 | HomeKit accessory            |
|-------------------------------------|------------------------------|
| Curtain motor (`*curtain_motor*`)   | Window Covering (0–100 %)    |
| Light / wall switch (`l_*_switch*`) | Lightbulb (on/off)           |
| Central AC (`*_ac`)                 | Thermostat (off/heat/cool/auto + temp) |
| Water floor heating (`*floor_heating*`) | Thermostat (heat‑only)   |
| Fresh‑air unit (`*fresh_air*`)      | Fan (on/off + speed)         |
| Door/window sensor (`*door_sensor*`)| Contact Sensor               |
| Motion / curtain‑IR detector        | Motion Sensor                |
| Water‑leak detector                 | Leak Sensor                  |
| Gas detector                        | Smoke Sensor (closest match) |

Battery level, sensor work‑mode, SOS buttons and scenes have no HomeKit
equivalent and are intentionally skipped.

## Hardware

- **ESP32‑S3 with PSRAM** (8 MB recommended). PSRAM is **required** — decrypting
  the device list needs it. A classic ESP32 without PSRAM will not work.
- A 2.4 GHz Wi‑Fi network and a working UIOT account.

## Build & flash

Requires [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the
`esp32` core and the **HomeSpan** + **PubSubClient** libraries.

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install HomeSpan PubSubClient

./flash.sh          # compile → upload → watchdog‑reset boot
```

`flash.sh` uses the required build flags (`huge_app` partition, USB‑CDC, OPI
PSRAM) and performs a watchdog reset — native‑USB S3 boards otherwise stay in
download mode after flashing.

## First‑time setup (captive portal)

1. Join the Wi‑Fi hotspot **`UiotSpan-Setup`**.
2. The config page pops up automatically (or open `http://192.168.4.1`).
3. Enter: Wi‑Fi name/password (2.4 GHz), UIOT account (SN last 6 digits) /
   password, and the full host **SN**.
4. Save — the device reboots, connects, discovers everything and joins HomeKit.

To re‑configure later: press **BOOT within 3 s of power‑up**, or open
`http://<device-ip>:8080/reset`.

## Pair with HomeKit

iPhone → Home app → Add Accessory → *More options…* → **UiotSpan**.
Default setup code: `466-37-726`.

## Architecture

```
          core 0 (cloudTask)                 core 1 (Arduino loop)
   ┌───────────────────────────┐       ┌──────────────────────────────┐
   │ OAuth login / token        │       │ homeSpan.poll()  (HomeKit)   │
   │ device.control (1 attr/req)│       │ g_mqtt.loop()    (live state)│
   │ device.list  (discovery /  │  ⇄    │ drain apply-queue → setVal   │
   │              periodic sync)│ queues│ curtain debounce → cmd-queue │
   └───────────────────────────┘       └──────────────────────────────┘
```

Source is split into single‑responsibility header modules, included in
dependency order as one translation unit:

| File             | Layer            | Responsibility                               |
|------------------|------------------|----------------------------------------------|
| `config.h`       | Configuration    | endpoints, NVS config, captive portal        |
| `codec.h`        | Encoding         | AES‑256‑ECB / MD5 / base64, JSON field extract |
| `uiot_api.h`     | Cloud API        | login, one signed `apiPost`, control/scene/list |
| `accessories.h`  | HomeKit adapter  | device classes, command queue, state write‑back |
| `cloud.h`        | Orchestration    | MQTT, discovery, sync, background task, status page |
| `UiotSpan.ino`   | Application      | `setup()` / `loop()` wiring                  |

### How it works

- **Control (up):** a HomeKit write is debounced (250 ms, newest wins) and queued;
  the background task sends it as a signed HTTPS request. The UIOT gateway accepts
  **one property per request**, so multi‑property changes are split automatically.
- **State (down):** cloud MQTT `state_report` is decrypted and written to HomeKit
  immediately. Curtain motion state follows the device's `motorState` field only
  (matching the official plugin). A full `device.list` resync runs at
  boot/reconnect and every 5 minutes to recover any dropped push.
- **Diagnostics:** `http://<device-ip>:8080/` returns JSON (wifi/mqtt/token/heap/
  psram + per‑type accessory counts). Serial logging is unreliable on native‑USB
  S3 boards, so use this page.

## Limitations

- **Cloud‑dependent.** Control and state both go through the UIOT cloud. The
  local gateway channel uses per‑session ECDH keys and could not be used offline.
- **No per‑device "No Response".** HomeSpan can't mark a bridged accessory
  unreachable, and UIOT's `deviceOnlineState` is unreliable for sleepy Zigbee
  devices, so offline devices still appear.
- **Current temperature** is not reported by some AC/heating units; the target
  temperature is shown as a fallback.

## Security notes

- Wi‑Fi and UIOT credentials are stored in NVS via the portal — **never** in the
  source. `APP_KEY` / `APP_SECRET` in `config.h` are the UIOT app's public
  constants (identical for all users), not personal secrets.
- The TLS clients use `setInsecure()` (no certificate validation). Fine for a LAN
  bridge to a fixed vendor endpoint; harden if you need strict validation.

## Credits

- Protocol constants and device semantics were learned from the official
  open‑source UIOT Home Assistant integration (`uiotlink/ha_uiot_home`).
- Built on [HomeSpan](https://github.com/HomeSpan/HomeSpan) and
  [PubSubClient](https://github.com/knolleary/pubsubclient).

This project is not affiliated with or endorsed by UIOT / 紫光物联 or Apple.
Use with your own equipment and account.

## License

MIT — see `LICENSE`.

---

## 中文速览

把 UIOT(超级智慧家/紫光物联)整套设备接入 Apple 家庭的独立网桥,跑在一块
**带 PSRAM 的 ESP32‑S3** 上,无需 Home Assistant / 树莓派 / 任何中枢软件。

- **开机自动发现**所有设备,按型号归类、用云端真实名字建配件;
- **配网门户**(连热点自动弹出)配置 WiFi + UIOT 账号,代码里无任何密钥;
  上电 3 秒内按 **BOOT** 可随时重新配网;
- **双核**:阻塞式云请求在 core 0,主循环只跑 HomeKit + MQTT,不卡不丢包;
- 实时 MQTT 推送 + 5 分钟兜底全量同步;配件用 deviceId 作稳定 AID。

构建:装 `arduino-cli` + `esp32` 核心 + `HomeSpan`/`PubSubClient` 库,运行
`./flash.sh`。配对码 `466-37-726`。详见上文各节。
