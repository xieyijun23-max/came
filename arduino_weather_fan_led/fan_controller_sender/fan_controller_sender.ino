#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <stdint.h>

// Fan controller + ESP-NOW lighting telemetry sender.
// This node still accepts upstream weather commands, drives 5 fans, and broadcasts
// the real fan runtime state so the LED node can animate only when the fans move.

const int fanPins[] = {13, 18, 14, 27, 26}; // North, North-East, South-East, South-West, North-West
#define FAN_COUNT 5

const uint8_t ESP_NOW_BROADCAST[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t PROTOCOL_MAGIC = 0xA7;
const uint8_t WEATHER_COMMAND_KIND = 1;
const uint8_t FAN_LIGHT_KIND = 2;
const uint8_t PROTOCOL_VERSION = 2;

int POWER_FLOOR = 110;
int READY_PWM = 85;
int RAMP_TIME = 600;
const int MIN_LULL_TIME = 700;
const int MAX_LULL_TIME = 3600;
const int FAN_ARC_DEGREES = 72;

struct __attribute__((packed)) WeatherCommand {
  uint8_t magic;
  uint8_t version;
  uint8_t kind;
  float fanPWM;
  int16_t weatherCode;
  float humidity;
  uint8_t cloudCover;
  uint8_t isDay;
  uint8_t isGusting;
  uint8_t hour;
  uint8_t minute;
  float windDir;
  uint16_t propSpeed;
  uint16_t sustainTime;
};

struct __attribute__((packed)) FanLightPacket {
  uint8_t magic;
  uint8_t version;
  uint8_t kind;
  uint32_t seq;
  float fanPWM;
  int16_t weatherCode;
  float humidity;
  uint8_t cloudCover;
  uint8_t isDay;
  uint8_t isGusting;
  uint8_t hour;
  uint8_t minute;
  float windDir;
  uint16_t propSpeed;
  uint16_t sustainTime;
  uint8_t fanActive;
  uint8_t fanIntensity;
  uint8_t activeMask;
  uint8_t currentTier;
  float fanPhase;
  uint8_t fanDuty[FAN_COUNT];
};

WeatherCommand command = {
  PROTOCOL_MAGIC, PROTOCOL_VERSION, WEATHER_COMMAND_KIND,
  140.0f, 1, 60.0f, 35, 1, 0, 12, 0, 180.0f, 800, 1200
};

FanLightPacket packet;
unsigned long cycleStart = 0;
bool isInLull = false;
uint32_t packetSeq = 0;
unsigned long lastSendAt = 0;
uint8_t lastActiveMask = 0;
uint8_t lastIntensity = 0;
uint8_t fanDuty[FAN_COUNT] = {0, 0, 0, 0, 0};

int g_propSpeed = 800;
int g_sustainTime = 1200;
uint8_t currentMask = 0;
uint8_t currentTier = 0;
uint8_t currentIntensity = 0;
float currentPhase = 0.0f;
bool currentFanActive = false;
float gustOsc = 0.0f;

float normalizedWindDir(float deg) {
  while (deg < 0.0f) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return deg;
}


float angularDistanceDeg(float a, float b) {
  float d = fabsf(normalizedWindDir(a) - normalizedWindDir(b));
  return d > 180.0f ? 360.0f - d : d;
}

float smoothStep01(float x) {
  x = constrain(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

bool isWetWeather(int16_t wc) {
  return (wc >= 51 && wc <= 67) || (wc >= 80 && wc <= 82);
}

bool isSnowWeather(int16_t wc) {
  return (wc >= 71 && wc <= 77) || wc >= 85;
}

bool isStormWeather(int16_t wc) {
  return wc >= 95;
}

float weatherSeverity() {
  float pwmEnergy = constrain(command.fanPWM / 255.0f, 0.0f, 1.0f);
  float cloudEnergy = command.cloudCover / 100.0f;
  float humidityEnergy = command.humidity / 100.0f;
  float weatherBoost = 0.0f;
  if (isWetWeather(command.weatherCode)) weatherBoost = 0.18f;
  if (isSnowWeather(command.weatherCode)) weatherBoost = 0.12f;
  if (isStormWeather(command.weatherCode)) weatherBoost = 0.32f;
  return constrain(pwmEnergy * 0.58f + cloudEnergy * 0.22f + humidityEnergy * 0.12f + weatherBoost, 0.0f, 1.0f);
}

uint16_t dynamicLullDuration() {
  float severity = weatherSeverity();
  if (command.isGusting) severity = constrain(severity + 0.18f, 0.0f, 1.0f);
  return (uint16_t)(MAX_LULL_TIME - (MAX_LULL_TIME - MIN_LULL_TIME) * severity);
}

float gustEnvelope(unsigned long now) {
  float severity = weatherSeverity();
  float t = now * 0.001f;
  float slow = 0.5f + 0.5f * sinf(t * (0.55f + severity * 0.45f) + gustOsc);
  float fast = 0.5f + 0.5f * sinf(t * (2.3f + severity * 2.4f) + gustOsc * 2.31f);
  float gust = 0.72f + severity * 0.28f + slow * 0.18f + fast * severity * 0.16f;
  if (command.isGusting) gust += 0.18f;
  return constrain(gust, 0.60f, 1.28f);
}

void arcticWrite(int pinIdx, int val) {
  int finalVal = 0;
  if (val > READY_PWM + 5) {
    finalVal = map(val, READY_PWM, 255, POWER_FLOOR, 255);
  } else if (val > 10) {
    finalVal = READY_PWM;
  }

  finalVal = constrain(finalVal, 0, 255);
  fanDuty[pinIdx] = (uint8_t)finalVal;
  ledcWrite(fanPins[pinIdx], finalVal);
}

void fillPacket() {
  packet.magic = PROTOCOL_MAGIC;
  packet.version = PROTOCOL_VERSION;
  packet.kind = FAN_LIGHT_KIND;
  packet.seq = ++packetSeq;
  packet.fanPWM = command.fanPWM;
  packet.weatherCode = command.weatherCode;
  packet.humidity = command.humidity;
  packet.cloudCover = command.cloudCover;
  packet.isDay = command.isDay;
  packet.isGusting = command.isGusting;
  packet.hour = command.hour;
  packet.minute = command.minute;
  packet.windDir = command.windDir;
  packet.propSpeed = g_propSpeed;
  packet.sustainTime = g_sustainTime;
  packet.fanActive = currentFanActive ? 1 : 0;
  packet.fanIntensity = currentIntensity;
  packet.activeMask = currentMask;
  packet.currentTier = currentTier;
  packet.fanPhase = currentPhase;
  for (int i = 0; i < FAN_COUNT; i++) packet.fanDuty[i] = fanDuty[i];
}

void sendFanLightPacket(bool force) {
  unsigned long now = millis();
  bool changedEnough = abs((int)currentIntensity - (int)lastIntensity) >= 6 || currentMask != lastActiveMask;
  if (!force && !changedEnough && now - lastSendAt < 50) return;

  fillPacket();
  esp_now_send(ESP_NOW_BROADCAST, (const uint8_t *)&packet, sizeof(packet));
  lastSendAt = now;
  lastIntensity = currentIntensity;
  lastActiveMask = currentMask;
}

void applyWeatherCommand(const WeatherCommand &nextCommand) {
  command = nextCommand;
  command.magic = PROTOCOL_MAGIC;
  command.version = PROTOCOL_VERSION;
  command.kind = WEATHER_COMMAND_KIND;
  command.fanPWM = constrain(command.fanPWM, 0.0f, 255.0f);
  command.humidity = constrain(command.humidity, 0.0f, 100.0f);
  command.cloudCover = constrain(command.cloudCover, 0, 100);
  command.windDir = normalizedWindDir(command.windDir);
  g_propSpeed = constrain((int)command.propSpeed, 200, 5000);
  g_sustainTime = constrain((int)command.sustainTime, 200, 8000);
  sendFanLightPacket(true);
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incoming, int len) {
  if (len != sizeof(WeatherCommand)) return;

  WeatherCommand nextCommand;
  memcpy(&nextCommand, incoming, sizeof(nextCommand));
  if (nextCommand.magic != PROTOCOL_MAGIC || nextCommand.version != PROTOCOL_VERSION) return;
  if (nextCommand.kind != WEATHER_COMMAND_KIND) return;

  applyWeatherCommand(nextCommand);
}

void computeFanFrame() {
  unsigned long now = millis();
  unsigned long elapsed = now - cycleStart;
  long tierDuration = (long)RAMP_TIME * 2L + g_sustainTime;
  long flowDuration = (long)g_propSpeed * 2L + tierDuration;
  uint16_t lullDuration = dynamicLullDuration();

  currentMask = 0;
  currentTier = 0;
  currentIntensity = 0;
  currentPhase = 0.0f;
  currentFanActive = !isInLull;

  if (!isInLull) {
    if (elapsed > (unsigned long)flowDuration) {
      isInLull = true;
      cycleStart = now;
      currentFanActive = false;
      sendFanLightPacket(true);
      return;
    }

    currentPhase = constrain((float)elapsed / (float)flowDuration, 0.0f, 1.0f);
    float targetPWM = max((float)READY_PWM, command.fanPWM);
    float gust = gustEnvelope(now);
    float maxIntensity = 0.0f;

    for (int i = 0; i < FAN_COUNT; i++) {
      float fanAngle = (360.0f / FAN_COUNT) * i;
      float angularTier = angularDistanceDeg(fanAngle, command.windDir) / FAN_ARC_DEGREES;
      long tElapsed = (long)elapsed - (long)(angularTier * g_propSpeed);
      float intensity = 0.0f;

      if (tElapsed >= 0 && tElapsed < tierDuration) {
        if (tElapsed < RAMP_TIME) {
          intensity = smoothStep01((float)tElapsed / (float)RAMP_TIME);
        } else if (tElapsed < RAMP_TIME + g_sustainTime) {
          intensity = 1.0f;
        } else {
          float dec = (float)(tElapsed - (RAMP_TIME + g_sustainTime));
          intensity = 1.0f - smoothStep01(dec / (float)RAMP_TIME);
        }
      }

      // Continuous directional feathering avoids hard snapping between the five fans.
      float directionalFeather = max(0.18f, 1.0f - angularTier * 0.34f);
      float weatherBreath = 0.92f + 0.08f * sinf((now * 0.001f) + i * 1.7f + command.cloudCover * 0.03f);
      float shaped = constrain(intensity * directionalFeather * gust * weatherBreath, 0.0f, 1.0f);
      int out = READY_PWM + (int)((targetPWM - READY_PWM) * shaped);
      arcticWrite(i, out);

      if (out > READY_PWM + 8) currentMask |= (1 << i);
      maxIntensity = max(maxIntensity, shaped);
    }

    currentTier = constrain((int)(currentPhase * 3.0f), 0, 2);
    currentIntensity = (uint8_t)constrain(maxIntensity * 255.0f, 0.0f, 255.0f);
  } else {
    currentFanActive = false;
    if (elapsed > lullDuration) {
      isInLull = false;
      cycleStart = now;
      gustOsc += 1.173f;
      sendFanLightPacket(true);
      return;
    }

    // During lull, keep motors barely awake only for strong weather; calm weather can fully breathe out.
    int idle = weatherSeverity() > 0.55f ? READY_PWM : 0;
    for (int i = 0; i < FAN_COUNT; i++) arcticWrite(i, idle);
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, ESP_NOW_BROADCAST, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (!esp_now_is_peer_exist(ESP_NOW_BROADCAST)) {
    esp_now_add_peer(&peerInfo);
  }

  esp_now_register_recv_cb(OnDataRecv);
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < FAN_COUNT; i++) {
    ledcAttach(fanPins[i], 25000, 8);
    ledcWrite(fanPins[i], READY_PWM);
    fanDuty[i] = READY_PWM;
  }

  setupEspNow();
  cycleStart = millis();
  sendFanLightPacket(true);
}

void loop() {
  computeFanFrame();
  sendFanLightPacket(false);
}
