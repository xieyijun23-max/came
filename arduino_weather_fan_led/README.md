# ESP32 Weather Fan + LED ESP-NOW Pair

This folder contains an optimized two-node Arduino IDE setup based on the provided sketches.

## Sketches

- `fan_controller_sender/fan_controller_sender.ino`
  - Receives upstream weather commands.
  - Drives five 12V fans with a continuous directional wind-field model instead of hard fan snapping.
  - Uses weather severity, humidity, cloud cover, gust flags, and PWM to shape gust envelopes and lull duration.
  - Broadcasts a compact `FanLightPacket` every ~50 ms, and immediately on fan state changes.

- `led_strip_receiver/led_strip_receiver.ino`
  - Receives `FanLightPacket` broadcasts.
  - Keeps the original weather rendering model.
  - Adds fan-synchronized light waves, wakes, fan-position glows, and weather-specific texture layers.
  - Uses fresh fan telemetry to increase cloud turbulence, rain streak speed, snow drift, and fog breathing only when the physical fans are active.

## Protocol notes

Both sketches intentionally duplicate the same packed protocol fields. If you change one packet layout, change the other sketch at the same time.

The fan node expects upstream weather commands with:

- `magic = 0xA7`
- `version = 2`
- `kind = 1`

The LED node listens for fan-light telemetry with:

- `magic = 0xA7`
- `version = 2`
- `kind = 2`

The fan node broadcasts telemetry, so you do not need to hard-code the LED receiver MAC address during early testing.

## Important hardware notes

- ESP32 GPIO pins must not drive 12V fans directly. Use MOSFET or driver modules and common ground.
- Keep the LED power supply sized for the strip current.
- If you combine Wi-Fi weather fetching and ESP-NOW, keep all nodes on the same 2.4 GHz channel.


## Effect model updates

### Fan simulation

The fan sketch now treats the five physical fans as samples around a circular wind field. Each fan receives a feathered amount of the requested wind direction, so a direction between two fans blends across both instead of jumping to the nearest output. Wind cycles still ramp, hold, and decay, but weather severity shortens the lull for intense rain/storm conditions and allows calm weather to breathe out longer.

### LED weather layers

The LED sketch renders the base atmosphere first, then adds weather texture layers before the final fan wave:

- Rain adds fast directional streaks that accelerate when the fan packet reports active airflow.
- Snow adds slower drifting white flecks with more white-channel energy.
- Fog adds a low-contrast breathing wash rather than sharp particles.
- Fan waves add a bright moving head plus a softer wake and local highlights at active fan positions.

These layers are intentionally driven by the fan node's real `fanActive`, `fanIntensity`, `activeMask`, `fanPhase`, and `fanDuty[]` values so the light sculpture visibly follows the physical fan rhythm rather than the API weather value alone.

## Wi-Fi credentials

The fan and LED sketches in this folder use the ESP32 Wi-Fi radio for ESP-NOW, but they do **not** connect to your router by themselves. That is why there is no `WiFi.begin(...)` in the receiver sketches: ESP-NOW only needs `WiFi.mode(WIFI_STA)` on these nodes.

Use router Wi-Fi only on the ESP32 that fetches the weather API. Keep your real password in a local `secrets.h` file instead of committing it:

1. Copy `secrets.example.h` to `secrets.h`.
2. Replace `WIFI_SSID` and `WIFI_PASSWORD` with your 2.4 GHz router credentials.
3. Include `secrets.h` only from the weather-fetching sketch.

Example weather-master connection snippet:

```cpp
#include <WiFi.h>
#include "secrets.h"

void connectRouterWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
}
```

Important: ESP-NOW and router Wi-Fi must use the same 2.4 GHz channel. The simplest setup is usually:

- Weather/API master connects to the router and sends ESP-NOW packets on that router channel.
- Fan and LED nodes stay in `WIFI_STA` ESP-NOW mode and listen on the same channel.
- If packets are unreliable, fix your router to a known 2.4 GHz channel and set the same channel on ESP-NOW-only nodes.
