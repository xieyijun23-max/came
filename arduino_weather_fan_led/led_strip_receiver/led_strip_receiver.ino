#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <esp_wifi.h>
#include <stdint.h>

// LED receiver for the fan-driven weather installation.
// It listens to FanLightPacket broadcasts from the fan controller and adds
// wind-synchronized light motion only while the physical fans are active.

#define LED_PIN 18
#define NUMPIXELS 144
#define FAN_COUNT 5

const uint8_t PROTOCOL_MAGIC = 0xA7;
const uint8_t FAN_LIGHT_KIND = 2;
const uint8_t PROTOCOL_VERSION = 2;

struct AtmosphericBias { float r, g, b, w; };

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

FanLightPacket incomingData = {
  PROTOCOL_MAGIC, PROTOCOL_VERSION, FAN_LIGHT_KIND, 0,
  140.0f, 1, 60.0f, 35, 1, 0, 12, 0, 180.0f, 800, 1200,
  0, 0, 0, 0, 0.0f, {85, 85, 85, 85, 85}
};

Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRBW + NEO_KHZ800);

float wBuffer[NUMPIXELS], astroBuffer[NUMPIXELS];
float physicsTime = 0.0f, windFactor = 0.2f, targetWindFactor = 0.2f;
byte lr[NUMPIXELS], lg[NUMPIXELS], lb[NUMPIXELS], lw[NUMPIXELS];
float localDayProgress = 0.5f;
int lightningTimer = 0;
unsigned long lastUp = 0;
unsigned long lastPacketAt = 0;
uint32_t lastSeq = 0;

struct Particle { float pos, alpha, life, spd, turbulence; bool isSnow, active; } p[100];

float norm360(float deg) {
  while (deg < 0.0f) deg += 360.0f;
  while (deg >= 360.0f) deg -= 360.0f;
  return deg;
}

float circularDistance(float a, float b, float total) {
  float d = fabsf(a - b);
  if (d > total / 2.0f) d = total - d;
  return d;
}


bool isRainWeather(int wc) {
  return (wc >= 51 && wc <= 67) || (wc >= 80 && wc <= 82);
}

bool isSnowWeather(int wc) {
  return (wc >= 71 && wc <= 77) || wc >= 85;
}

bool isFogWeather(int wc) {
  return wc >= 45 && wc <= 48;
}

bool isStormWeather(int wc) {
  return wc >= 95;
}

float weatherEnergy() {
  float cloud = incomingData.cloudCover / 100.0f;
  float humidity = incomingData.humidity / 100.0f;
  float fan = incomingData.fanIntensity / 255.0f;
  float e = cloud * 0.32f + humidity * 0.18f + fan * 0.36f;
  if (isRainWeather(incomingData.weatherCode)) e += 0.18f;
  if (isSnowWeather(incomingData.weatherCode)) e += 0.12f;
  if (isStormWeather(incomingData.weatherCode)) e += 0.32f;
  return constrain(e, 0.0f, 1.0f);
}

AtmosphericBias getBias(float el, float moonPhase, int wc) {
  float r = 1.0f, g = 1.0f, b = 1.0f, w = 1.0f;
  if (wc <= 3) {
    r = 1.00f; g = 0.92f; b = 0.82f; w = 1.00f;
  } else if ((wc >= 51 && wc <= 67) || (wc >= 80 && wc <= 82)) {
    r = 0.45f; g = 0.62f; b = 1.15f; w = 0.70f;
  } else if ((wc >= 71 && wc <= 77) || (wc >= 85)) {
    r = 0.85f; g = 0.95f; b = 1.35f; w = 1.25f;
  } else if (wc >= 45 && wc <= 48) {
    r = 0.75f; g = 0.75f; b = 0.85f; w = 0.90f;
  }

  if (el > 20.0f) return {r, g, b, w};
  if (el > -6.0f) {
    float f = powf(constrain(1.0f - fabsf(el) / 15.0f, 0.0f, 1.0f), 2.2f);
    return {r + f * 1.2f, g + f * 0.35f, b - f * 0.4f, w - f * 0.3f};
  }
  return {0.12f * r, 0.12f * g, 1.00f * b, 0.25f + moonPhase * 0.25f};
}

void renderAstro(float az, float el, float en, bool isSun, float sunsetF, float moonScale) {
  if (el < -16.0f) return;
  float sIdx = (az / 360.0f) * NUMPIXELS;
  float sRad = isSun ? (28.0f + sunsetF * 75.0f) : (16.0f * moonScale);
  for (int i = 0; i < NUMPIXELS; i++) {
    float d = circularDistance((float)i, sIdx, (float)NUMPIXELS);
    if (d < sRad) {
      float bright = powf(1.0f - (d / sRad), isSun ? 5.2f : 3.2f) * en;
      astroBuffer[i] = fmaxf(astroBuffer[i], bright);
    }
  }
}

void handleParticles() {
  int wc = incomingData.weatherCode;
  if (wc < 51) return;

  float intensity = incomingData.humidity / 100.0f;
  bool isSnow = (wc >= 71);
  float fanBoost = incomingData.fanActive ? (0.45f + incomingData.fanIntensity / 255.0f) : 0.35f;

  if (random(100) < (isSnow ? 15 : 30) * intensity * (0.6f + windFactor * 0.6f) * fanBoost) {
    for (int i = 0; i < 100; i++) {
      if (!p[i].active) {
        p[i].active = true;
        p[i].isSnow = isSnow;
        p[i].pos = fmodf((incomingData.windDir / 360.0f) * NUMPIXELS + random(-40, 40) + NUMPIXELS, (float)NUMPIXELS);
        p[i].life = 1.0f;
        p[i].alpha = isSnow ? 255.0f : 190.0f;
        p[i].spd = isSnow ? 0.12f : 0.65f;
        p[i].turbulence = random(80, 200) / 100.0f;
        break;
      }
    }
  }

  float windCenter = (incomingData.windDir / 360.0f) * NUMPIXELS;
  for (int i = 0; i < 100; i++) {
    if (p[i].active) {
      float d = circularDistance(p[i].pos, windCenter, (float)NUMPIXELS);
      float localWindPush = max(0.0f, cosf(d * 2.0f * PI / NUMPIXELS));
      p[i].pos = fmodf(p[i].pos + p[i].spd + (targetWindFactor * 2.2f * localWindPush) + NUMPIXELS, (float)NUMPIXELS);
      int idx = (int)p[i].pos % NUMPIXELS;
      wBuffer[idx] = fmaxf(wBuffer[idx], p[i].alpha * p[i].life);
      p[i].life -= p[i].isSnow ? 0.007f : 0.045f;
      if (p[i].life <= 0) p[i].active = false;
    }
  }
}

void renderFanLinkedWave() {
  if (!incomingData.fanActive || incomingData.fanIntensity < 8) return;

  float drive = incomingData.fanIntensity / 255.0f;
  float windIdx = (incomingData.windDir / 360.0f) * NUMPIXELS;
  float travel = incomingData.fanPhase * NUMPIXELS;
  float waveHead = fmodf(windIdx + travel + NUMPIXELS, (float)NUMPIXELS);
  float fanWidth = 9.0f + drive * 24.0f;
  float wakeWidth = fanWidth * 2.4f;

  for (int i = 0; i < NUMPIXELS; i++) {
    float dHead = circularDistance((float)i, waveHead, (float)NUMPIXELS);
    if (dHead < fanWidth) {
      float pulse = powf(1.0f - dHead / fanWidth, 2.0f) * drive;
      wBuffer[i] = fmaxf(wBuffer[i], 42.0f + pulse * 150.0f);
      astroBuffer[i] = fmaxf(astroBuffer[i], pulse * 72.0f);
    } else if (dHead < wakeWidth) {
      float wake = powf(1.0f - (dHead - fanWidth) / (wakeWidth - fanWidth), 2.8f) * drive;
      wBuffer[i] = fmaxf(wBuffer[i], 18.0f + wake * 64.0f);
    }

    for (int fan = 0; fan < FAN_COUNT; fan++) {
      float fanIdx = ((float)fan / (float)FAN_COUNT) * NUMPIXELS;
      float dFan = circularDistance((float)i, fanIdx, (float)NUMPIXELS);
      float fanDutyNorm = incomingData.fanDuty[fan] / 255.0f;
      if ((incomingData.activeMask & (1 << fan)) != 0 && dFan < 9.0f) {
        wBuffer[i] = fmaxf(wBuffer[i], 30.0f + fanDutyNorm * 105.0f);
        astroBuffer[i] = fmaxf(astroBuffer[i], fanDutyNorm * 32.0f);
      }
    }
  }
}

void renderWeatherTexture() {
  float energy = weatherEnergy();
  if (energy <= 0.02f) return;

  float windIdx = (incomingData.windDir / 360.0f) * NUMPIXELS;
  float fanDrive = incomingData.fanActive ? incomingData.fanIntensity / 255.0f : 0.0f;

  if (isRainWeather(incomingData.weatherCode)) {
    int drops = 3 + (int)(energy * 11.0f);
    for (int k = 0; k < drops; k++) {
      float seed = k * 37.0f + physicsTime * (32.0f + fanDrive * 55.0f);
      int idx = ((int)(windIdx + seed)) % NUMPIXELS;
      wBuffer[idx] = fmaxf(wBuffer[idx], 95.0f + energy * 110.0f);
      wBuffer[(idx + NUMPIXELS - 1) % NUMPIXELS] = fmaxf(wBuffer[(idx + NUMPIXELS - 1) % NUMPIXELS], 40.0f + energy * 45.0f);
    }
  }

  if (isSnowWeather(incomingData.weatherCode)) {
    for (int k = 0; k < 10; k++) {
      float idx = fmodf(k * 14.4f + sinf(physicsTime * 0.8f + k) * 5.5f + windIdx * 0.18f, (float)NUMPIXELS);
      int led = (int)(idx + NUMPIXELS) % NUMPIXELS;
      wBuffer[led] = fmaxf(wBuffer[led], 52.0f + energy * 90.0f);
      astroBuffer[led] = fmaxf(astroBuffer[led], 18.0f + energy * 36.0f);
    }
  }

  if (isFogWeather(incomingData.weatherCode)) {
    for (int i = 0; i < NUMPIXELS; i++) {
      float breath = 0.5f + 0.5f * sinf(physicsTime * 0.72f + i * 0.055f);
      wBuffer[i] = fmaxf(wBuffer[i], 28.0f + breath * 42.0f * energy);
    }
  }
}

void renderUltimate() {
  float targetProg = ((float)incomingData.hour + (float)incomingData.minute / 60.0f) / 24.0f;
  float tDiff = targetProg - localDayProgress;
  if (tDiff > 0.5f) tDiff -= 1.0f;
  else if (tDiff < -0.5f) tDiff += 1.0f;
  localDayProgress += tDiff * 0.012f;

  bool packetFresh = millis() - lastPacketAt < 1800;
  float fanDrive = (packetFresh && incomingData.fanActive) ? incomingData.fanIntensity / 255.0f : 0.0f;
  float weatherDrive = powf(constrain(incomingData.fanPWM / 255.0f, 0.0f, 1.0f), 1.5f);
  targetWindFactor = 0.15f + (weatherDrive * 2.45f) + (fanDrive * 2.40f);
  windFactor = (windFactor * 0.70f) + (targetWindFactor * 0.30f);
  physicsTime += 0.0085f * (0.25f + windFactor * 0.85f);

  float sunEl = 90.0f * sinf((localDayProgress * 2.0f * PI) - PI / 2.0f);
  AtmosphericBias bias = getBias(sunEl, 0.5f + 0.5f * cosf(physicsTime * 0.012f * PI), incomingData.weatherCode);

  bool isFog = (incomingData.weatherCode >= 45 && incomingData.weatherCode <= 48);
  float decay = isFog ? 0.98f : (0.92f + (1.0f - (windFactor / 5.5f)) * 0.035f);
  for (int i = 0; i < NUMPIXELS; i++) {
    astroBuffer[i] = 0;
    wBuffer[i] *= decay;
  }

  float sunAz = localDayProgress * 360.0f;
  float sunsetF = powf(constrain(1.0f - fabsf(sunEl - 1.5f) / 16.5f, 0.0f, 1.0f), 2.2f);
  renderAstro(sunAz, sunEl, 255.0f, true, sunsetF, 1.0f);
  if (sunEl <= 5.0f) renderAstro(fmodf(sunAz + 180.0f, 360.0f), -sunEl, 180.0f, false, 0, 0.8f);

  float sunIdx = (sunAz / 360.0f) * NUMPIXELS;
  for (int i = 0; i < NUMPIXELS; i++) {
    float dSun = circularDistance((float)i, sunIdx, (float)NUMPIXELS);
    float sunCenterWeight = isFog ? 0.35f : powf(max(0.0f, cosf(dSun * 2.0f * PI / NUMPIXELS)), 1.25f);
    float stormDim = (incomingData.weatherCode >= 95 && lightningTimer > 15) ? 0.3f : 1.0f;

    wBuffer[i] = fmaxf(wBuffer[i], (14.0f * stormDim) * (1.0f + sunCenterWeight * targetWindFactor * 1.25f));

    float angle = (i * 2.0f * PI) / NUMPIXELS;
    float gustPulse = targetWindFactor * 0.18f + fanDrive * sinf(incomingData.fanPhase * 2.0f * PI) * 0.45f;
    float n1 = sinf(angle + physicsTime * 0.65f + gustPulse);
    float n2 = sinf(angle * 2.0f - physicsTime * 1.45f - gustPulse * 2.2f) * 0.42f;
    float n3 = sinf(angle * 4.0f + physicsTime * 2.85f + gustPulse * 4.5f) * 0.18f;

    float totalNoise = n1 + n2 + n3;
    if (totalNoise > 0) wBuffer[i] += (totalNoise * totalNoise) * (incomingData.cloudCover / 1.6f);
  }

  renderWeatherTexture();
  handleParticles();
  renderFanLinkedWave();

  if (incomingData.weatherCode >= 95 && lightningTimer <= 0 && random(1000) > 993) lightningTimer = 22;
  if (lightningTimer > 0) {
    for (int i = 0; i < NUMPIXELS; i++) wBuffer[i] = fmaxf(wBuffer[i], (lightningTimer % 2 == 0 ? 255.0f : 0.0f));
    lightningTimer--;
  }

  for (int i = 0; i < NUMPIXELS; i++) {
    float whiteMix = (incomingData.weatherCode >= 71) ? 0.48f : 0.28f;
    float tW = wBuffer[i] * whiteMix * bias.w;
    float cP = wBuffer[i] * (1.0f - whiteMix);
    lr[i] = (lr[i] * 5 + (int)constrain(cP * bias.r + astroBuffer[i] * 0.75f, 0, 255)) / 6;
    lg[i] = (lg[i] * 5 + (int)constrain(cP * bias.g + astroBuffer[i] * 0.85f, 0, 255)) / 6;
    lb[i] = (lb[i] * 5 + (int)constrain(cP * bias.b + astroBuffer[i] * 1.05f, 0, 255)) / 6;
    lw[i] = (lw[i] * 5 + (int)constrain(tW + astroBuffer[i] * 0.95f, 0, 255)) / 6;
    strip.setPixelColor(i, lr[i], lg[i], lb[i], lw[i]);
  }
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incoming, int len) {
  if (len != sizeof(FanLightPacket)) return;

  FanLightPacket packet;
  memcpy(&packet, incoming, sizeof(packet));
  if (packet.magic != PROTOCOL_MAGIC || packet.version != PROTOCOL_VERSION) return;
  if (packet.kind != FAN_LIGHT_KIND) return;
  if (packet.seq == lastSeq) return;

  packet.fanPWM = constrain(packet.fanPWM, 0.0f, 255.0f);
  packet.humidity = constrain(packet.humidity, 0.0f, 100.0f);
  packet.cloudCover = constrain(packet.cloudCover, 0, 100);
  packet.windDir = norm360(packet.windDir);
  packet.fanIntensity = constrain(packet.fanIntensity, 0, 255);
  packet.activeMask &= 0x1F;
  packet.currentTier = constrain(packet.currentTier, 0, 2);
  packet.fanPhase = constrain(packet.fanPhase, 0.0f, 1.0f);

  incomingData = packet;
  lastSeq = packet.seq;
  lastPacketAt = millis();
}

void setup() {
  strip.begin();
  strip.show();
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
  }
}

void loop() {
  if (millis() - lastUp > 16) {
    lastUp = millis();
    renderUltimate();
    strip.show();
  }
}
