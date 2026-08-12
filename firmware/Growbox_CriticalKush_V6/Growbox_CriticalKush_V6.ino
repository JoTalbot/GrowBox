#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_task_wdt.h>
#include "time.h"
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include <ctype.h>

// ================= НАЗНАЧЕНИЕ ПИНОВ =================
// Реле 220V
#define RELAY_LIGHT       16  // IN1: Свет 220V
#define RELAY_EXHAUST     17  // IN2: Вытяжка + Угольный фильтр
#define RELAY_HEATER      18  // IN3: Обогрев
#define RELAY_FAN         19  // IN4: Обдув (ветер)

// 12V Нагрузки (Реле или MOSFET)
#define RELAY_PUMP        22  // IN5: Помпа
#define RELAY_VALVE1      23  // IN6: Клапан 1
#define RELAY_VALVE2      25  // IN7: Клапан 2
#define RELAY_VALVE3      26  // IN8: Клапан 3
#define RELAY_HUMIDIFIER  21  // отдельное реле увлажнителя (не на 8-канальном модуле)

// ШИМ Диммирование (Рассвет/Закат)
#define PIN_LIGHT_PWM     15

// Датчики
#define DHT_PIN           4
#define DHTTYPE           DHT22
#define ONE_WIRE_BUS      27
#define SOIL1_PIN         32
#define SOIL2_PIN         34
#define SOIL3_PIN         35
#define WATER_LEVEL_PIN   13
#define FLOOD_SENSOR_PIN  14
#define POWER_SENSE_PIN   33

#define RELAY_ON          LOW
#define RELAY_OFF         HIGH

#define WDT_TIMEOUT_SEC   45
#define FIRMWARE_VERSION  "6.3.1"

enum GrowStage {
  STAGE_VEG = 0,
  STAGE_BLOOM = 1,
  STAGE_DRY = 2
};

enum OverrideMode : uint8_t {
  MODE_AUTO = 0,
  MODE_ON = 1,
  MODE_OFF = 2
};

GrowStage currentStage = STAGE_VEG;

// Настройки (меняются с дашборда, живут в NVS)
float tempTargetDay   = 24.5;
float tempTargetNight = 20.5;
float tempTargetDry   = 16.0;
float tempHysteresis  = 1.0;
float tempEmergency   = 32.5;

int soilDryThreshold = 28;
unsigned long wateringDurationMs = 8000;
unsigned long soilSoakDelayMs    = 2700000UL;
unsigned long fallbackWateringMs = 86400000UL;
unsigned long windOnMs           = 15UL * 60UL * 1000UL;
unsigned long windOffMs          = 5UL * 60UL * 1000UL;

int vegStartHour = 6;
int vegEndHour = 24;
int bloomStartHour = 8;
int bloomEndHour = 20;
int sunriseMin = 30;

float vpdVegMin = 0.80;
float vpdVegMax = 1.20;
float vpdBloomMin = 1.00;
float vpdBloomMax = 1.50;

bool enableHumidifier = false;

const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 2 * 3600;
const int   daylightOffset_sec = 3600;

WebServer server(80);
DHT dht(DHT_PIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature waterSensors(&oneWire);
Preferences prefs;

#define HISTORY_POINTS 96
struct HistoryPoint {
  int8_t temp;
  uint8_t hum;
  uint8_t vpd10;
  int8_t waterTemp;
  uint8_t soil1;
  uint8_t soil2;
  uint8_t soil3;
};

HistoryPoint history[HISTORY_POINTS];
int historyIndex = 0;
int historyCount = 0;
unsigned long lastHistoryLog = 0;
const unsigned long HISTORY_INTERVAL = 15 * 60 * 1000;

float temperature = 0.0;
float humidity = 0.0;
float vpd = 0.0;
float waterTemperature = -127.0;

bool dhtConnected   = false;
bool ds18Connected  = false;
bool soilConnected[3] = {false, false, false};
bool powerGridOk    = true;

int soilMoisture[3] = {0, 0, 0};
int soilRaw[3] = {0, 0, 0};
int soilCalibDry[3] = {3200, 3200, 3200};
int soilCalibWet[3] = {1400, 1400, 1400};

bool stateLight   = false;
int lightPwmDuty  = 0;
bool stateExhaust = true;
bool stateHeater  = false;
bool stateFan     = true;
bool statePump    = false;
bool stateHumid   = false;
bool stateValves[3] = {false, false, false};

OverrideMode modeLight   = MODE_AUTO;
OverrideMode modeExhaust = MODE_AUTO;
OverrideMode modeHeater  = MODE_AUTO;
OverrideMode modeFan     = MODE_AUTO;
OverrideMode modeHumid   = MODE_AUTO;

bool enableSafetySensors = false;
bool enablePowerSense   = false;
bool isWaterLow = false;
bool isFloodDetected = false;
bool thermalShutdown = false;

String tgBotToken = "";
String tgChatId   = "";
String camIp      = "http://esp32-cam.local";
bool tgEnabled    = false;
long lastTgUpdateId = 0;
unsigned long lastTgPoll = 0;
const unsigned long TG_POLL_INTERVAL = 3500;
unsigned long lastTgAlertTime = 0;
unsigned long lastPowerAlertTime = 0;

unsigned long cycleStartTimestamp = 0;
unsigned long lastWateredTime[3]  = {0, 0, 0};
unsigned long lastWateredUnix[3]  = {0, 0, 0};
int activeWateringZone = -1;
unsigned long wateringStartTime = 0;
bool wdtStarted = false;

unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2500;

unsigned long windTimer = 0;
bool windState = true;
unsigned long dryVentTimer = 0;
bool dryVentState = false;

// Удалённый агент (исходящие HTTPS, NAT не нужен)
String ntfyTopic = "";
String remoteKey = "";
String versionUrl = "https://raw.githubusercontent.com/JoTalbot/GrowBox/main/firmware/remote/version.json";
String inboxUrl = "";
bool remoteEnabled = false;
bool autoOta = false;
String lastRemoteEvent = "-";
String lastOtaResult = "-";
unsigned long lastRemotePoll = 0;
unsigned long lastRemoteHeartbeat = 0;
const unsigned long REMOTE_POLL_MS = 25000;
const unsigned long REMOTE_HEARTBEAT_MS = 10UL * 60UL * 1000UL;
String lastNtfySince = "5m";
long lastInboxId = 0;
bool otaPending = false;
String otaPendingUrl = "";

void persistRemote();
void loadRemoteSettings();
void pollRemoteAgent();
void publishRemoteStatus(const String& event);
void requestOta(const String& url);
void performPendingOta();
void executeRemoteCommand(const String& cmd, const String& arg, const String& key);
void checkVersionFile(bool flashIfNewer);
String deviceId();
String statusJson(const String& event);
void handleSaveRemote();
void handleRemotePull();
void handleOtaCheck();

void feedWatchdog() {
  if (wdtStarted) esp_task_wdt_reset();
}

void startWatchdog() {
  if (wdtStarted) return;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = WDT_TIMEOUT_SEC * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true
  };
  esp_task_wdt_init(&twdt_config);
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);
  wdtStarted = true;
}

bool ntpReady() {
  time_t now;
  time(&now);
  return now > 1700000000UL;
}

void persistULong(const char* key, unsigned long value) {
  prefs.begin("growbox", false);
  prefs.putULong(key, value);
  prefs.end();
}

void ensureCycleStart() {
  if (cycleStartTimestamp != 0 || !ntpReady()) return;
  time_t now;
  time(&now);
  cycleStartTimestamp = (unsigned long)now;
  persistULong("start", cycleStartTimestamp);
}

void startNewCycle() {
  if (!ntpReady()) return;
  time_t now;
  time(&now);
  cycleStartTimestamp = (unsigned long)now;
  persistULong("start", cycleStartTimestamp);
}

void markZoneWatered(int zone) {
  if (zone < 0 || zone >= 3) return;
  lastWateredTime[zone] = millis();
  if (ntpReady()) {
    time_t now;
    time(&now);
    lastWateredUnix[zone] = (unsigned long)now;
    persistULong((String("wunix") + zone).c_str(), lastWateredUnix[zone]);
  }
}

bool soakDelayPassed(int zone) {
  unsigned long nowMs = millis();
  if (lastWateredTime[zone] != 0 && (nowMs - lastWateredTime[zone] < soilSoakDelayMs)) {
    return false;
  }
  if (lastWateredUnix[zone] != 0 && ntpReady()) {
    time_t now;
    time(&now);
    if ((unsigned long)now >= lastWateredUnix[zone] &&
        ((unsigned long)now - lastWateredUnix[zone]) < (soilSoakDelayMs / 1000UL)) {
      return false;
    }
  }
  return true;
}

bool fallbackWaterDue(int zone) {
  if (lastWateredUnix[zone] != 0 && ntpReady()) {
    time_t now;
    time(&now);
    return ((unsigned long)now - lastWateredUnix[zone]) >= (fallbackWateringMs / 1000UL);
  }
  if (lastWateredTime[zone] == 0) {
    lastWateredTime[zone] = millis();
    return false;
  }
  return (millis() - lastWateredTime[zone]) >= fallbackWateringMs;
}

void setRelay(uint8_t pin, bool state) {
  digitalWrite(pin, state ? RELAY_ON : RELAY_OFF);
}

bool applyOverride(OverrideMode mode, bool autoState) {
  if (mode == MODE_ON) return true;
  if (mode == MODE_OFF) return false;
  return autoState;
}

const char* modeLabel(OverrideMode mode) {
  if (mode == MODE_ON) return "РУЧН ВКЛ";
  if (mode == MODE_OFF) return "РУЧН ВЫКЛ";
  return "АВТО";
}

OverrideMode parseModeArg(String s) {
  s.toLowerCase();
  s.trim();
  if (s == "on" || s == "1") return MODE_ON;
  if (s == "off" || s == "2") return MODE_OFF;
  return MODE_AUTO;
}

void persistModes() {
  prefs.begin("growbox", false);
  prefs.putUChar("mLight", modeLight);
  prefs.putUChar("mExh", modeExhaust);
  prefs.putUChar("mHeat", modeHeater);
  prefs.putUChar("mFan", modeFan);
  prefs.putUChar("mHumid", modeHumid);
  prefs.end();
}

void persistSettings() {
  prefs.begin("growbox", false);
  prefs.putFloat("tDay", tempTargetDay);
  prefs.putFloat("tNight", tempTargetNight);
  prefs.putFloat("tDry", tempTargetDry);
  prefs.putFloat("tHyst", tempHysteresis);
  prefs.putFloat("tEmerg", tempEmergency);
  prefs.putInt("soilDry", soilDryThreshold);
  prefs.putULong("waterMs", wateringDurationMs);
  prefs.putULong("soakMs", soilSoakDelayMs);
  prefs.putULong("fbMs", fallbackWateringMs);
  prefs.putULong("windOn", windOnMs);
  prefs.putULong("windOff", windOffMs);
  prefs.putInt("vegH0", vegStartHour);
  prefs.putInt("vegH1", vegEndHour);
  prefs.putInt("blmH0", bloomStartHour);
  prefs.putInt("blmH1", bloomEndHour);
  prefs.putInt("sunrise", sunriseMin);
  prefs.putFloat("vpdVmin", vpdVegMin);
  prefs.putFloat("vpdVmax", vpdVegMax);
  prefs.putFloat("vpdBmin", vpdBloomMin);
  prefs.putFloat("vpdBmax", vpdBloomMax);
  prefs.putBool("humidEn", enableHumidifier);
  prefs.end();
}

void loadSettings() {
  tempTargetDay = prefs.getFloat("tDay", tempTargetDay);
  tempTargetNight = prefs.getFloat("tNight", tempTargetNight);
  tempTargetDry = prefs.getFloat("tDry", tempTargetDry);
  tempHysteresis = prefs.getFloat("tHyst", tempHysteresis);
  tempEmergency = prefs.getFloat("tEmerg", tempEmergency);
  soilDryThreshold = prefs.getInt("soilDry", soilDryThreshold);
  wateringDurationMs = prefs.getULong("waterMs", wateringDurationMs);
  soilSoakDelayMs = prefs.getULong("soakMs", soilSoakDelayMs);
  fallbackWateringMs = prefs.getULong("fbMs", fallbackWateringMs);
  windOnMs = prefs.getULong("windOn", windOnMs);
  windOffMs = prefs.getULong("windOff", windOffMs);
  vegStartHour = prefs.getInt("vegH0", vegStartHour);
  vegEndHour = prefs.getInt("vegH1", vegEndHour);
  bloomStartHour = prefs.getInt("blmH0", bloomStartHour);
  bloomEndHour = prefs.getInt("blmH1", bloomEndHour);
  sunriseMin = prefs.getInt("sunrise", sunriseMin);
  vpdVegMin = prefs.getFloat("vpdVmin", vpdVegMin);
  vpdVegMax = prefs.getFloat("vpdVmax", vpdVegMax);
  vpdBloomMin = prefs.getFloat("vpdBmin", vpdBloomMin);
  vpdBloomMax = prefs.getFloat("vpdBmax", vpdBloomMax);
  enableHumidifier = prefs.getBool("humidEn", enableHumidifier);
  modeLight = (OverrideMode)prefs.getUChar("mLight", MODE_AUTO);
  modeExhaust = (OverrideMode)prefs.getUChar("mExh", MODE_AUTO);
  modeHeater = (OverrideMode)prefs.getUChar("mHeat", MODE_AUTO);
  modeFan = (OverrideMode)prefs.getUChar("mFan", MODE_AUTO);
  modeHumid = (OverrideMode)prefs.getUChar("mHumid", MODE_AUTO);
}

bool setDeviceMode(const String& dev, OverrideMode mode) {
  if (dev == "light") modeLight = mode;
  else if (dev == "exhaust") modeExhaust = mode;
  else if (dev == "heater") modeHeater = mode;
  else if (dev == "fan") modeFan = mode;
  else if (dev == "humid") modeHumid = mode;
  else return false;
  persistModes();
  return true;
}

void allAuto() {
  modeLight = modeExhaust = modeHeater = modeFan = modeHumid = MODE_AUTO;
  persistModes();
}

void getVpdTargets(float& vmin, float& vmax) {
  if (currentStage == STAGE_BLOOM) {
    vmin = vpdBloomMin;
    vmax = vpdBloomMax;
  } else {
    vmin = vpdVegMin;
    vmax = vpdVegMax;
  }
}

float calculateVPD(float t, float h) {
  if (t <= 0 || h <= 0) return 0.0;
  float svp = 0.61078 * exp((17.27 * t) / (t + 237.3));
  return svp * (1.0 - (h / 100.0));
}

int readSoilPercent(int idx, int rawVal) {
  int dry = soilCalibDry[idx];
  int wet = soilCalibWet[idx];
  int percent = map(rawVal, dry, wet, 0, 100);
  return constrain(percent, 0, 100);
}

int getGrowDay() {
  if (cycleStartTimestamp == 0) return 1;
  time_t now;
  time(&now);
  if (now < (time_t)cycleStartTimestamp) return 1;
  return (int)((now - (time_t)cycleStartTimestamp) / 86400) + 1;
}

String urlEncode(const String& value) {
  String encoded;
  encoded.reserve(value.length() * 3 / 2);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

void sendTelegramMessage(String msg) {
  if (!tgEnabled || tgBotToken.length() < 15 || tgChatId.length() < 3) return;
  if (WiFi.status() != WL_CONNECTED) return;

  feedWatchdog();
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);
  if (client.connect("api.telegram.org", 443)) {
    String payload = "chat_id=" + urlEncode(tgChatId) + "&text=" + urlEncode(msg) + "&parse_mode=HTML";
    client.print(String("POST /bot") + tgBotToken + "/sendMessage HTTP/1.1\r\n" +
                 "Host: api.telegram.org\r\n" +
                 "Content-Type: application/x-www-form-urlencoded\r\n" +
                 "Content-Length: " + payload.length() + "\r\n" +
                 "Connection: close\r\n\r\n" + payload);
  }
  client.stop();
  feedWatchdog();
}

void triggerWatering(int zone) {
  if (zone < 0 || zone >= 3) return;
  if (enableSafetySensors && (isWaterLow || isFloodDetected)) return;

  if (activeWateringZone == -1) {
    activeWateringZone = zone;
    wateringStartTime = millis();
    stateValves[zone] = true;
    setRelay(zone == 0 ? RELAY_VALVE1 : (zone == 1 ? RELAY_VALVE2 : RELAY_VALVE3), true);
    statePump = true;
    setRelay(RELAY_PUMP, true);
    sendTelegramMessage("🚿 <b>Полив:</b> Запущен полив горшка #" + String(zone + 1) +
                        " (" + String(wateringDurationMs / 1000) + " сек)");
  }
}

void setGrowStage(GrowStage stage) {
  currentStage = stage;
  prefs.begin("growbox", false);
  prefs.putInt("stage", (int)currentStage);
  prefs.end();
}

String modeShort(OverrideMode m) {
  if (m == MODE_ON) return "ON";
  if (m == MODE_OFF) return "OFF";
  return "AUTO";
}

bool applyTelegramMode(const String& cmd, const char* prefix, const char* dev, const char* title) {
  String p = prefix;
  if (!cmd.startsWith(p)) return false;
  String rest = cmd.substring(p.length());
  rest.trim();
  OverrideMode parsed = (rest.length() == 0) ? MODE_AUTO : parseModeArg(rest);
  setDeviceMode(dev, parsed);
  sendTelegramMessage(String("🎛️ <b>") + title + ":</b> " + modeLabel(parsed));
  return true;
}

void handleTelegramCommand(String cmd) {
  cmd.trim();
  String original = cmd;
  cmd.toLowerCase();

  if (cmd.startsWith("/ota http")) {
    requestOta(original.substring(5));
    sendTelegramMessage("📦 OTA поставлена в очередь");
    return;
  }
  if (cmd.startsWith("/ntfy ")) {
    ntfyTopic = original.substring(6);
    ntfyTopic.trim();
    persistRemote();
    sendTelegramMessage("🛰️ ntfy-топик сохранён");
    return;
  }
  if (cmd.startsWith("/remotekey ")) {
    remoteKey = original.substring(11);
    remoteKey.trim();
    persistRemote();
    sendTelegramMessage("🔑 Ключ агента сохранён");
    return;
  }

  if (cmd == "/start" || cmd == "/help") {
    String helpMsg = "🌿 <b>Команды GrowBox Enterprise v";
    helpMsg += FIRMWARE_VERSION;
    helpMsg += ":</b>\n\n";
    helpMsg += "/status /photo /settings\n";
    helpMsg += "/water1 /water2 /water3\n";
    helpMsg += "/veg /bloom /dry /resetday\n";
    helpMsg += "/light on|off|auto\n";
    helpMsg += "/exhaust /heat /fan /humid on|off|auto\n";
    helpMsg += "/auto — все реле обратно в авто\n";
    helpMsg += "/remoteon /remoteoff /pull /otacheck\n";
    helpMsg += "/ota &lt;url&gt; /ntfy &lt;topic&gt; /remotekey &lt;key&gt;\n";
    helpMsg += "/reboot";
    sendTelegramMessage(helpMsg);
  }
  else if (cmd == "/photo") {
    sendTelegramMessage("📸 <b>ESP32-CAM Камера:</b>\n" + camIp + "/capture");
  }
  else if (cmd == "/status") {
    struct tm timeinfo;
    char timeStr[32] = "--:--";
    if (getLocalTime(&timeinfo)) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    String s = "🌿 <b>Статус GrowBox [Enterprise v";
    s += FIRMWARE_VERSION;
    s += "]</b>\n\n";
    String stName = "Вегетация";
    if (currentStage == STAGE_BLOOM) stName = "Цветение";
    else if (currentStage == STAGE_DRY) stName = "Сушка 60/60";

    s += "📅 <b>Режим:</b> " + stName + " | <b>День:</b> " + String(getGrowDay()) + "\n";
    s += "🕒 <b>Время:</b> " + String(timeStr) + "\n";
    s += "⚡ <b>Сеть 220V:</b> " + String(powerGridOk ? "✅ В норме" : "🚨 НЕТ ПИТАНИЯ") + "\n\n";

    if (dhtConnected) {
      float vmin, vmax;
      getVpdTargets(vmin, vmax);
      String vpdMark = (vpd >= vmin && vpd <= vmax) ? "✅ Идеал" : (vpd < vmin ? "⚠️ Риск плесени" : "⚠️ Сухо");
      s += "🌡️ <b>Воздух:</b> " + String(temperature, 1) + " °C | <b>Влажность:</b> " + String(humidity, 1) + " %\n";
      s += "💨 <b>VPD:</b> " + String(vpd, 2) + " kPa (" + vpdMark + ")\n";
    } else {
      s += "🌡️ <b>Воздух:</b> ⚠️ <i>DHT отключен</i>\n";
    }
    if (ds18Connected) s += "🧪 <b>Вода в баке:</b> " + String(waterTemperature, 1) + " °C\n";

    s += "\n🪴 <b>Почва:</b>\n";
    for (int i = 0; i < 3; i++) {
      s += " • #" + String(i + 1) + ": " + (soilConnected[i] ? (String(soilMoisture[i]) + "%") : "⚠️ откл") + "\n";
    }

    s += "\n⚡ <b>Реле</b> [режим]:\n";
    s += " Свет=" + String(stateLight ? "ВКЛ" : "ВЫКЛ") + " [" + modeShort(modeLight) + "]\n";
    s += " Вытяжка=" + String(stateExhaust ? "ВКЛ" : "ВЫКЛ") + " [" + modeShort(modeExhaust) + "]\n";
    s += " Обогрев=" + String(stateHeater ? "ВКЛ" : "ВЫКЛ") + " [" + modeShort(modeHeater) + "]\n";
    s += " Обдув=" + String(stateFan ? "ВКЛ" : "ВЫКЛ") + " [" + modeShort(modeFan) + "]\n";
    s += " Увлажн=" + String(stateHumid ? "ВКЛ" : "ВЫКЛ") + " [" + modeShort(modeHumid) + "]";
    sendTelegramMessage(s);
  }
  else if (cmd == "/settings") {
    String s = "⚙️ <b>Настройки v" + String(FIRMWARE_VERSION) + "</b>\n";
    s += "Вега: " + String(vegStartHour) + ":00–" + String(vegEndHour) + ":00\n";
    s += "Цвет: " + String(bloomStartHour) + ":00–" + String(bloomEndHour) + ":00\n";
    s += "Рассвет: " + String(sunriseMin) + " мин\n";
    s += "Темп день/ночь/сушка: " + String(tempTargetDay, 1) + " / " + String(tempTargetNight, 1) + " / " + String(tempTargetDry, 1) + " °C\n";
    s += "Полив: порог " + String(soilDryThreshold) + "%, " + String(wateringDurationMs / 1000) + " сек, soak " + String(soilSoakDelayMs / 60000) + " мин\n";
    s += "VPD вега " + String(vpdVegMin, 2) + "–" + String(vpdVegMax, 2) + ", цвет " + String(vpdBloomMin, 2) + "–" + String(vpdBloomMax, 2) + "\n";
    s += "Увлажнитель: " + String(enableHumidifier ? "включён (GPIO21)" : "выключен");
    sendTelegramMessage(s);
  }
  else if (cmd == "/water1" || cmd == "/water 1") triggerWatering(0);
  else if (cmd == "/water2" || cmd == "/water 2") triggerWatering(1);
  else if (cmd == "/water3" || cmd == "/water 3") triggerWatering(2);
  else if (cmd == "/veg") {
    setGrowStage(STAGE_VEG);
    sendTelegramMessage("🌱 <b>Стадия:</b> ВЕГЕТАЦИЯ");
  }
  else if (cmd == "/bloom") {
    setGrowStage(STAGE_BLOOM);
    sendTelegramMessage("🌸 <b>Стадия:</b> ЦВЕТЕНИЕ");
  }
  else if (cmd == "/dry") {
    setGrowStage(STAGE_DRY);
    sendTelegramMessage("🍂 <b>Стадия:</b> СУШКА 60/60");
  }
  else if (cmd == "/resetday") {
    if (!ntpReady()) sendTelegramMessage("⏳ NTP ещё не синхронизирован.");
    else {
      startNewCycle();
      sendTelegramMessage("📅 <b>Цикл сброшен.</b> Сегодня день 1.");
    }
  }
  else if (cmd == "/auto") {
    allAuto();
    sendTelegramMessage("🔄 Все реле переведены в <b>АВТО</b>");
  }
  else if (cmd == "/remoteon") {
    remoteEnabled = true;
    persistRemote();
    sendTelegramMessage("🛰️ Удалённый канал <b>включён</b>");
  }
  else if (cmd == "/remoteoff") {
    remoteEnabled = false;
    persistRemote();
    sendTelegramMessage("🛰️ Удалённый канал выключен");
  }
  else if (cmd == "/pull") {
    lastRemotePoll = 0;
    sendTelegramMessage("📥 Форсирую опрос канала");
  }
  else if (cmd == "/otacheck") {
    executeRemoteCommand("checkota", "", remoteKey);
  }
  else if (cmd == "/reboot") {
    sendTelegramMessage("🔁 Ребут...");
    delay(300);
    ESP.restart();
  }
  else if (applyTelegramMode(cmd, "/light", "light", "Свет")) {}
  else if (applyTelegramMode(cmd, "/exhaust", "exhaust", "Вытяжка")) {}
  else if (applyTelegramMode(cmd, "/heat", "heater", "Обогрев")) {}
  else if (applyTelegramMode(cmd, "/fan", "fan", "Обдув")) {}
  else if (applyTelegramMode(cmd, "/humid", "humid", "Увлажнитель")) {}
  else {
    sendTelegramMessage("❓ Неизвестная команда. Напишите /help");
  }
}

void checkTelegramUpdates() {
  if (!tgEnabled || tgBotToken.length() < 15 || tgChatId.length() < 3) return;
  if (WiFi.status() != WL_CONNECTED) return;

  feedWatchdog();
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(2500);

  if (client.connect("api.telegram.org", 443)) {
    String url = "/bot" + tgBotToken + "/getUpdates?offset=" + String(lastTgUpdateId + 1) + "&limit=3&timeout=0";
    client.print(String("GET ") + url + " HTTP/1.1\r\nHost: api.telegram.org\r\nConnection: close\r\n\r\n");

    String response = "";
    unsigned long st = millis();
    while (client.connected() && millis() - st < 2000) {
      while (client.available()) {
        char c = client.read();
        if (response.length() < 3000) response += c;
      }
    }
    client.stop();

    int updateIndex = response.indexOf("\"update_id\":");
    while (updateIndex != -1) {
      int idEnd = response.indexOf(",", updateIndex);
      if (idEnd != -1) {
        long uId = response.substring(updateIndex + 12, idEnd).toInt();
        if (uId > lastTgUpdateId) lastTgUpdateId = uId;
      }

      int chatIdx = response.indexOf("\"chat\":{\"id\":", updateIndex);
      if (chatIdx != -1) {
        int chatEnd = response.indexOf(",", chatIdx);
        String senderChat = response.substring(chatIdx + 13, chatEnd);
        senderChat.trim();

        int textIdx = response.indexOf("\"text\":\"", updateIndex);
        if (textIdx != -1 && (textIdx < response.indexOf("\"update_id\":", updateIndex + 1) || response.indexOf("\"update_id\":", updateIndex + 1) == -1)) {
          int textEnd = response.indexOf("\"", textIdx + 8);
          String text = response.substring(textIdx + 8, textEnd);
          if (senderChat == tgChatId) handleTelegramCommand(text);
        }
      }
      updateIndex = response.indexOf("\"update_id\":", updateIndex + 1);
    }
  }
  feedWatchdog();
}

void logHistoryPoint() {
  history[historyIndex].temp = dhtConnected ? (int8_t)constrain((int)round(temperature), -20, 80) : 0;
  history[historyIndex].hum = dhtConnected ? (uint8_t)constrain((int)round(humidity), 0, 100) : 0;
  history[historyIndex].vpd10 = dhtConnected ? (uint8_t)constrain((int)round(vpd * 10.0), 0, 50) : 0;
  history[historyIndex].waterTemp = ds18Connected ? (int8_t)constrain((int)round(waterTemperature), -20, 80) : 0;
  history[historyIndex].soil1 = (uint8_t)soilMoisture[0];
  history[historyIndex].soil2 = (uint8_t)soilMoisture[1];
  history[historyIndex].soil3 = (uint8_t)soilMoisture[2];
  historyIndex = (historyIndex + 1) % HISTORY_POINTS;
  if (historyCount < HISTORY_POINTS) historyCount++;
}

void handleApiData() {
  struct tm timeinfo;
  char timeStr[32] = "--:--:--";
  if (getLocalTime(&timeinfo)) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

  float vmin, vmax;
  getVpdTargets(vmin, vmax);

  String json = "{";
  json += "\"temp\":" + (dhtConnected ? String(temperature, 1) : "\"--.-\"") + ",";
  json += "\"hum\":" + (dhtConnected ? String(humidity, 1) : "\"--.-\"") + ",";
  json += "\"vpd\":" + (dhtConnected ? String(vpd, 2) : "\"--.-\"") + ",";
  json += "\"waterTemp\":" + (ds18Connected ? String(waterTemperature, 1) : "\"--.-\"") + ",";
  json += "\"dhtOk\":" + String(dhtConnected ? 1 : 0) + ",";
  json += "\"ds18Ok\":" + String(ds18Connected ? 1 : 0) + ",";
  json += "\"s1Ok\":" + String(soilConnected[0] ? 1 : 0) + ",";
  json += "\"s2Ok\":" + String(soilConnected[1] ? 1 : 0) + ",";
  json += "\"s3Ok\":" + String(soilConnected[2] ? 1 : 0) + ",";
  json += "\"soil1\":" + String(soilMoisture[0]) + ",";
  json += "\"soil2\":" + String(soilMoisture[1]) + ",";
  json += "\"soil3\":" + String(soilMoisture[2]) + ",";
  json += "\"raw1\":" + String(soilRaw[0]) + ",";
  json += "\"raw2\":" + String(soilRaw[1]) + ",";
  json += "\"raw3\":" + String(soilRaw[2]) + ",";
  json += "\"dry1\":" + String(soilCalibDry[0]) + ",";
  json += "\"dry2\":" + String(soilCalibDry[1]) + ",";
  json += "\"dry3\":" + String(soilCalibDry[2]) + ",";
  json += "\"wet1\":" + String(soilCalibWet[0]) + ",";
  json += "\"wet2\":" + String(soilCalibWet[1]) + ",";
  json += "\"wet3\":" + String(soilCalibWet[2]) + ",";
  json += "\"light\":" + String(stateLight ? 1 : 0) + ",";
  json += "\"lightPwm\":" + String(map(lightPwmDuty, 0, 255, 0, 100)) + ",";
  json += "\"exhaust\":" + String(stateExhaust ? 1 : 0) + ",";
  json += "\"heater\":" + String(stateHeater ? 1 : 0) + ",";
  json += "\"fan\":" + String(stateFan ? 1 : 0) + ",";
  json += "\"humid\":" + String(stateHumid ? 1 : 0) + ",";
  json += "\"pump\":" + String(statePump ? 1 : 0) + ",";
  json += "\"v1\":" + String(stateValves[0] ? 1 : 0) + ",";
  json += "\"v2\":" + String(stateValves[1] ? 1 : 0) + ",";
  json += "\"v3\":" + String(stateValves[2] ? 1 : 0) + ",";
  json += "\"mLight\":" + String((int)modeLight) + ",";
  json += "\"mExh\":" + String((int)modeExhaust) + ",";
  json += "\"mHeat\":" + String((int)modeHeater) + ",";
  json += "\"mFan\":" + String((int)modeFan) + ",";
  json += "\"mHumid\":" + String((int)modeHumid) + ",";

  String stLabel = "Вегетация";
  if (currentStage == STAGE_BLOOM) stLabel = "Цветение";
  else if (currentStage == STAGE_DRY) stLabel = "Сушка 60/60";

  json += "\"stage\":\"" + stLabel + "\",";
  json += "\"stageId\":" + String((int)currentStage) + ",";
  json += "\"day\":" + String(getGrowDay()) + ",";
  json += "\"time\":\"" + String(timeStr) + "\",";
  json += "\"thermal\":" + String(thermalShutdown ? 1 : 0) + ",";
  json += "\"powerOk\":" + String(powerGridOk ? 1 : 0) + ",";
  json += "\"waterLow\":" + String(isWaterLow ? 1 : 0) + ",";
  json += "\"flood\":" + String(isFloodDetected ? 1 : 0) + ",";
  json += "\"safetyEn\":" + String(enableSafetySensors ? 1 : 0) + ",";
  json += "\"powerSenseEn\":" + String(enablePowerSense ? 1 : 0) + ",";
  json += "\"humidEn\":" + String(enableHumidifier ? 1 : 0) + ",";
  json += "\"tgEn\":" + String(tgEnabled ? 1 : 0) + ",";
  json += "\"camIp\":\"" + camIp + "\",";
  json += "\"fw\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"vpdMin\":" + String(vmin, 2) + ",";
  json += "\"vpdMax\":" + String(vmax, 2) + ",";
  json += "\"remoteEn\":" + String(remoteEnabled ? 1 : 0) + ",";
  json += "\"autoOta\":" + String(autoOta ? 1 : 0) + ",";
  json += "\"ntfyOn\":" + String(ntfyTopic.length() > 4 ? 1 : 0) + ",";
  json += "\"devId\":\"" + deviceId() + "\",";
  json += "\"rEvent\":\"" + lastRemoteEvent + "\",";
  json += "\"otaRes\":\"" + lastOtaResult + "\",";
  json += "\"watering\":" + String(activeWateringZone);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiHistory() {
  String json = "[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + HISTORY_POINTS) % HISTORY_POINTS;
    if (i > 0) json += ",";
    json += "{\"t\":" + String(history[idx].temp) + ",";
    json += "\"h\":" + String(history[idx].hum) + ",";
    json += "\"v\":" + String(history[idx].vpd10 / 10.0, 1) + ",";
    json += "\"wt\":" + String(history[idx].waterTemp) + ",";
    json += "\"s1\":" + String(history[idx].soil1) + ",";
    json += "\"s2\":" + String(history[idx].soil2) + ",";
    json += "\"s3\":" + String(history[idx].soil3) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleExportCsv() {
  String csv = "Index,Temp_C,Humidity_Pct,VPD_kPa,WaterTemp_C,Soil1_Pct,Soil2_Pct,Soil3_Pct\n";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + HISTORY_POINTS) % HISTORY_POINTS;
    csv += String(i + 1) + ",";
    csv += String(history[idx].temp) + ",";
    csv += String(history[idx].hum) + ",";
    csv += String(history[idx].vpd10 / 10.0, 2) + ",";
    csv += String(history[idx].waterTemp) + ",";
    csv += String(history[idx].soil1) + ",";
    csv += String(history[idx].soil2) + ",";
    csv += String(history[idx].soil3) + "\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=growbox_history.csv");
  server.send(200, "text/csv", csv);
}

void handleRoot() {
  feedWatchdog();
  String html = "<!DOCTYPE html><html lang='ru'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Critical Kush Enterprise v";
  html += FIRMWARE_VERSION;
  html += "</title><style>";
  html += "* { box-sizing: border-box; font-family: -apple-system, system-ui, sans-serif; }";
  html += "body { background: #070a08; color: #e2e8e3; margin: 0; padding: 10px; }";
  html += ".wrap { max-width: 980px; margin: 0 auto; }";
  html += "header { text-align: center; margin-bottom: 10px; }";
  html += "h1 { color: #52b788; margin: 0; font-size: 20px; font-weight: 800; }";
  html += ".sub { color: #829285; font-size: 12px; margin-top: 2px; }";
  html += ".alert { background: #780000; border: 1px solid #c1121f; color: #fff; padding: 6px 10px; border-radius: 6px; margin-bottom: 8px; font-weight: bold; display: none; font-size: 12px; text-align: center; }";
  html += ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(270px, 1fr)); gap: 10px; }";
  html += ".card { background: #101612; border-radius: 10px; padding: 12px; border: 1px solid #1a241c; }";
  html += ".card-t { font-size: 13px; font-weight: 700; color: #74c69d; margin-bottom: 6px; border-bottom: 1px solid #1a241c; padding-bottom: 4px; display: flex; justify-content: space-between; align-items: center; }";
  html += ".row { display: flex; justify-content: space-between; align-items: center; margin: 5px 0; font-size: 13px; }";
  html += ".big { font-size: 22px; font-weight: 800; color: #fff; }";
  html += ".badge { padding: 2px 7px; border-radius: 4px; font-size: 10px; font-weight: 700; text-transform: uppercase; }";
  html += ".on { background: #2d6a4f; color: #d8f3dc; }";
  html += ".off { background: #49111c; color: #ffb4a2; }";
  html += ".man { outline: 1px solid #e9c46a; }";
  html += ".bar-bg { background: #18201a; border-radius: 5px; height: 7px; overflow: hidden; margin-top: 2px; }";
  html += ".bar-fill { height: 100%; background: #40916c; transition: width 0.4s; }";
  html += ".vpd-tag { padding: 2px 6px; border-radius: 5px; font-size: 10px; font-weight: bold; }";
  html += ".vpd-ok { background: #1b4332; color: #95d5b2; }";
  html += ".vpd-bad { background: #7f4f24; color: #ffe066; }";
  html += ".btn-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; margin-top: 8px; }";
  html += ".btn { background: #2d6a4f; color: white; border: none; padding: 7px; border-radius: 5px; font-weight: 700; cursor: pointer; text-align: center; font-size: 11px; text-decoration: none; }";
  html += ".btn:hover { background: #40916c; }";
  html += ".btn-sec { background: #18201a; border: 1px solid #253328; }";
  html += ".btn-dry { background: #8d5b4c; }";
  html += ".btn-w { background: #1b4965; }";
  html += ".btn-cal { background: #19201a; border: 1px solid #27342a; font-size: 10px; padding: 3px 5px; }";
  html += ".modes { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 3px; margin: 2px 0 8px; }";
  html += ".modes button { background: #18201a; border: 1px solid #253328; color: #cfe3d4; padding: 3px; border-radius: 4px; font-size: 10px; cursor: pointer; }";
  html += ".modes button.sel { background: #2d6a4f; border-color: #40916c; color: #fff; }";
  html += ".box { margin-top: 10px; background: #0c100d; border: 1px solid #1a241c; border-radius: 8px; padding: 10px; }";
  html += "input[type=text], input[type=number] { width: 100%; background: #151d17; border: 1px solid #253328; color: #fff; padding: 5px; border-radius: 4px; margin: 3px 0 6px; font-size: 11px; }";
  html += "label.f { font-size: 11px; color: #9aaf9e; display: block; }";
  html += ".set-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 6px 10px; }";
  html += "svg { width: 100%; height: 140px; background: #080c09; border-radius: 6px; margin-top: 6px; }";
  html += ".chart-legend { display: flex; justify-content: space-around; font-size: 10px; margin-top: 4px; }";
  html += ".footer { text-align: center; margin-top: 12px; font-size: 11px; color: #74c69d; }";
  html += ".footer a { color: #74c69d; text-decoration: none; }";
  html += "</style></head><body><div class='wrap'>";

  html += "<div id='alertBanner' class='alert'>⚠️ АВАРИЙНЫЙ ПЕРЕГРЕВ! Свет отключен.</div>";
  html += "<div id='powerAlert' class='alert' style='background:#b02a37;'>🚨 ПРОПАЛО ПИТАНИЕ 220V!</div>";
  html += "<div id='waterAlert' class='alert' style='background:#9d0208;'>🪣 БАК ПУСТ! Полив заблокирован.</div>";
  html += "<div id='floodAlert' class='alert' style='background:#d00000;'>🚨 ПРОТЕЧКА! Помпа отключена.</div>";

  html += "<header><h1>🌿 Critical Kush Enterprise v";
  html += FIRMWARE_VERSION;
  html += "</h1>";
  html += "<div class='sub'>Режим: <b id='stTxt'>...</b> | <b id='dayTxt'>День ...</b> | Время: <b id='tmTxt'>--:--</b></div></header>";
  html += "<div class='grid'>";

  html += "<div class='card'><div class='card-t'><span>🌡️ Климат</span><span id='vpdTag' class='vpd-tag vpd-ok'>VPD OK</span></div>";
  html += "<div class='row'><span>Воздух:</span><span class='big' id='tVal'>--.- °C</span></div>";
  html += "<div class='row'><span>Влажность:</span><span class='big' id='hVal'>--.- %</span></div>";
  html += "<div class='row'><span>VPD:</span><b id='vpdVal' style='color:#52b788;'>--.- kPa</b></div>";
  html += "<div class='row'><span>Вода в баке:</span><b id='wtVal' style='color:#48cae4;'>--.- °C</b></div></div>";

  html += "<div class='card'><div class='card-t'><span>💧 Почва (Dry-Back)</span></div>";
  html += "<div style='margin-bottom:5px;'><div class='row'><span>Горшок #1:</span><b id='s1V'>--%</b></div><div class='bar-bg'><div id='s1B' class='bar-fill'></div></div></div>";
  html += "<div style='margin-bottom:5px;'><div class='row'><span>Горшок #2:</span><b id='s2V'>--%</b></div><div class='bar-bg'><div id='s2B' class='bar-fill'></div></div></div>";
  html += "<div style='margin-bottom:5px;'><div class='row'><span>Горшок #3:</span><b id='s3V'>--%</b></div><div class='bar-bg'><div id='s3B' class='bar-fill'></div></div></div>";
  html += "<div class='btn-grid' style='grid-template-columns: 1fr 1fr 1fr;'>";
  html += "<button class='btn btn-w' onclick='fetch(\"/water?z=0\")'>💧 #1</button>";
  html += "<button class='btn btn-w' onclick='fetch(\"/water?z=1\")'>💧 #2</button>";
  html += "<button class='btn btn-w' onclick='fetch(\"/water?z=2\")'>💧 #3</button></div></div>";

  html += "<div class='card'><div class='card-t'><span>⚡ Оборудование</span><button class='btn-cal' onclick='fetch(\"/allAuto\")'>Все в авто</button></div>";
  html += "<div class='row'><span>💡 Свет</span><span id='bLight' class='badge off'>ВЫКЛ</span></div>";
  html += "<div class='modes' id='mdLight'></div>";
  html += "<div class='row'><span>🌪️ Вытяжка</span><span id='bExh' class='badge on'>ВКЛ</span></div>";
  html += "<div class='modes' id='mdExh'></div>";
  html += "<div class='row'><span>🔥 Обогрев</span><span id='bHeat' class='badge off'>ВЫКЛ</span></div>";
  html += "<div class='modes' id='mdHeat'></div>";
  html += "<div class='row'><span>🌀 Обдув</span><span id='bFan' class='badge on'>ВКЛ</span></div>";
  html += "<div class='modes' id='mdFan'></div>";
  html += "<div class='row'><span>💧 Увлажнитель</span><span id='bHumid' class='badge off'>ВЫКЛ</span></div>";
  html += "<div class='modes' id='mdHumid'></div>";
  html += "<div class='btn-grid' style='grid-template-columns: 1fr 1fr 1fr;'>";
  html += "<button class='btn btn-sec' onclick='fetch(\"/setStage?s=veg\")'>🌱 Вега</button>";
  html += "<button class='btn btn-sec' onclick='fetch(\"/setStage?s=bloom\")'>🌸 Цвет</button>";
  html += "<button class='btn btn-dry' onclick='fetch(\"/setStage?s=dry\")'>🍂 Сушка</button></div></div>";

  html += "<div class='card'><div class='card-t'><span>🚿 Помпа и защита</span></div>";
  html += "<div class='row'><span>Помпа:</span><span id='bPump' class='badge off'>СТОП</span></div>";
  html += "<div class='row'><span>Клапаны:</span><b id='vStat'>[0, 0, 0]</b></div>";
  html += "<div class='row'><span>Питание 220V:</span><span id='bPower' class='badge on'>В СЕТИ</span></div>";
  html += "<div id='wProg' style='font-size:11px; color:#f4a261; text-align:center; min-height:14px;'></div>";
  html += "<div class='btn-grid'>";
  html += "<a id='camBtn' href='#' target='_blank' class='btn btn-sec'>📸 Камера</a>";
  html += "<a href='/export.csv' class='btn btn-sec'>📥 CSV</a></div></div>";
  html += "</div>";

  html += "<div class='box'><div style='display:flex; justify-content:space-between;'><h3 style='margin:0; font-size:13px; color:#74c69d;'>📈 24 часа</h3>";
  html += "<a href='/export.csv' style='font-size:11px; color:#48cae4; text-decoration:none;'>CSV</a></div>";
  html += "<svg id='chartSvg' viewBox='0 0 500 140'></svg>";
  html += "<div class='chart-legend'><span style='color:#e63946;'>● Воздух</span><span style='color:#48cae4;'>● Вода</span>";
  html += "<span style='color:#457b9d;'>● RH</span><span style='color:#52b788;'>● VPD</span><span style='color:#e9c46a;'>● Почва</span></div></div>";

  html += "<div class='box'><h3 style='margin:0 0 6px; font-size:13px; color:#74c69d;'>⚖️ Калибровка почвы</h3>";
  for (int z = 0; z < 3; z++) {
    html += "<div class='row' style='border-bottom:1px solid #151d17; padding:4px 0;'>";
    html += "<span>Горшок #" + String(z + 1) + " (АЦП: <b id='raw" + String(z + 1) + "'>----</b>)</span><div>";
    html += "<button class='btn btn-cal' onclick='fetch(\"/calib?z=" + String(z) + "&t=dry\")'>0% Сухо (<span id='dry" + String(z + 1) + "'>--</span>)</button> ";
    html += "<button class='btn btn-cal' onclick='fetch(\"/calib?z=" + String(z) + "&t=wet\")'>100% Влажно (<span id='wet" + String(z + 1) + "'>--</span>)</button>";
    html += "</div></div>";
  }
  html += "</div>";

  html += "<div class='box'><h3 style='margin:0 0 8px; font-size:13px; color:#74c69d;'>🎛️ Параметры (без перепрошивки)</h3>";
  html += "<form action='/saveSettings' method='POST'><div class='set-grid'>";
  html += "<label class='f'>Вега старт, ч<input type='number' name='vegH0' min='0' max='23' value='" + String(vegStartHour) + "'></label>";
  html += "<label class='f'>Вега стоп, ч (24=00:00)<input type='number' name='vegH1' min='1' max='24' value='" + String(vegEndHour) + "'></label>";
  html += "<label class='f'>Цвет старт, ч<input type='number' name='blmH0' min='0' max='23' value='" + String(bloomStartHour) + "'></label>";
  html += "<label class='f'>Цвет стоп, ч<input type='number' name='blmH1' min='1' max='24' value='" + String(bloomEndHour) + "'></label>";
  html += "<label class='f'>Рассвет/закат, мин<input type='number' name='sunrise' min='0' max='120' value='" + String(sunriseMin) + "'></label>";
  html += "<label class='f'>Полив, сек<input type='number' name='waterSec' min='1' max='120' value='" + String(wateringDurationMs / 1000) + "'></label>";
  html += "<label class='f'>Порог почвы %<input type='number' name='soilDry' min='5' max='80' value='" + String(soilDryThreshold) + "'></label>";
  html += "<label class='f'>Soak, мин<input type='number' name='soakMin' min='5' max='240' value='" + String(soilSoakDelayMs / 60000) + "'></label>";
  html += "<label class='f'>Резерв полива, ч<input type='number' name='fbHours' min='6' max='72' value='" + String(fallbackWateringMs / 3600000UL) + "'></label>";
  html += "<label class='f'>Обдув вкл, мин<input type='number' name='windOn' min='1' max='60' value='" + String(windOnMs / 60000) + "'></label>";
  html += "<label class='f'>Обдув пауза, мин<input type='number' name='windOff' min='1' max='60' value='" + String(windOffMs / 60000) + "'></label>";
  html += "<label class='f'>Темп день °C<input type='number' step='0.1' name='tDay' value='" + String(tempTargetDay, 1) + "'></label>";
  html += "<label class='f'>Темп ночь °C<input type='number' step='0.1' name='tNight' value='" + String(tempTargetNight, 1) + "'></label>";
  html += "<label class='f'>Темп сушка °C<input type='number' step='0.1' name='tDry' value='" + String(tempTargetDry, 1) + "'></label>";
  html += "<label class='f'>Гистерезис °C<input type='number' step='0.1' name='tHyst' value='" + String(tempHysteresis, 1) + "'></label>";
  html += "<label class='f'>Авария °C<input type='number' step='0.1' name='tEmerg' value='" + String(tempEmergency, 1) + "'></label>";
  html += "<label class='f'>VPD вега min<input type='number' step='0.05' name='vpdVmin' value='" + String(vpdVegMin, 2) + "'></label>";
  html += "<label class='f'>VPD вега max<input type='number' step='0.05' name='vpdVmax' value='" + String(vpdVegMax, 2) + "'></label>";
  html += "<label class='f'>VPD цвет min<input type='number' step='0.05' name='vpdBmin' value='" + String(vpdBloomMin, 2) + "'></label>";
  html += "<label class='f'>VPD цвет max<input type='number' step='0.05' name='vpdBmax' value='" + String(vpdBloomMax, 2) + "'></label>";
  html += "</div>";
  html += "<label class='f' style='margin-top:8px;'><input type='checkbox' name='humidEn' value='1'";
  if (enableHumidifier) html += " checked";
  html += "> Увлажнитель подключён (GPIO 21, отдельное реле)</label>";
  html += "<button type='submit' class='btn' style='margin-top:8px;width:100%;'>💾 Сохранить параметры</button></form></div>";

  html += "<div class='box'><h3 style='margin:0 0 8px; font-size:13px; color:#74c69d;'>🛰️ Удалённый агент (за NAT)</h3>";
  html += "<div class='row'><span>Device ID</span><b id='devId'>...</b></div>";
  html += "<div class='row'><span>Канал</span><span id='bRemote' class='badge off'>ВЫКЛ</span></div>";
  html += "<div class='row'><span>Последнее:</span><b id='rEvent'>-</b></div>";
  html += "<div class='row'><span>OTA:</span><b id='otaRes'>-</b></div>";
  html += "<form action='/saveRemote' method='POST'>";
  html += "<label class='f'>ntfy-топик (секрет)</label>";
  html += "<input type='text' name='ntfy' value='" + ntfyTopic + "' placeholder='gb-ck-........'>";
  html += "<label class='f'>Ключ команд</label>";
  html += "<input type='text' name='rkey' value='" + remoteKey + "'>";
  html += "<label class='f'>URL version.json</label>";
  html += "<input type='text' name='verUrl' value='" + versionUrl + "'>";
  html += "<label class='f'>URL inbox.json (необязательно)</label>";
  html += "<input type='text' name='inboxUrl' value='" + inboxUrl + "'>";
  html += "<label class='f'><input type='checkbox' name='remEn' value='1'";
  if (remoteEnabled) html += " checked";
  html += "> Включить опрос канала</label>";
  html += "<label class='f'><input type='checkbox' name='autoOta' value='1'";
  if (autoOta) html += " checked";
  html += "> Автопрошивка, если version.json новее</label>";
  html += "<div class='btn-grid' style='margin-top:8px;'>";
  html += "<button type='submit' class='btn'>💾 Сохранить канал</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/remotePull\")'>📥 Проверить сейчас</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/otaCheck\")'>📦 Проверить OTA</button>";
  html += "</div></form></div>";

  html += "<div class='box'><h3 style='margin:0 0 8px; font-size:13px; color:#74c69d;'>⚙️ Telegram и защита</h3>";
  html += "<form action='/saveConfig' method='POST'>";
  html += "<label class='f'>Bot Token</label><input type='text' name='tgToken' value='" + tgBotToken + "'>";
  html += "<label class='f'>Chat ID</label><input type='text' name='tgChat' value='" + tgChatId + "'>";
  html += "<label class='f'>ESP32-CAM</label><input type='text' name='camIp' value='" + camIp + "'>";
  html += "<div class='btn-grid' style='grid-template-columns:1fr 1fr;'>";
  html += "<button type='submit' class='btn'>💾 Сохранить</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/resetCycle\")'>📅 Сбросить день</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/toggleSafety\")'>🛡️ Поплавок / протечка</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/togglePower\")'>⚡ Датчик 220V</button>";
  html += "</div></form></div>";

  html += "<div class='footer'><a href='/update'>📦 OTA /update</a> | v";
  html += FIRMWARE_VERSION;
  html += "</div></div>";

  html += "<script>";
  html += "function modeBtns(id,dev,cur){const names=['Авто','Вкл','Выкл'],vals=['auto','on','off'];let h='';";
  html += "for(let i=0;i<3;i++){h+='<button class=\"'+(cur===i?'sel':'')+'\" onclick=\"fetch(\\'/setMode?d='+dev+'&m='+vals[i]+'\\')\">'+names[i]+'</button>';}";
  html += "document.getElementById(id).innerHTML=h;}";
  html += "function drawCharts(data){let svg=document.getElementById('chartSvg');";
  html += "if(!data||data.length<2){svg.innerHTML='<text x=\"200\" y=\"70\" fill=\"#666\" font-size=\"11\">Сбор данных...</text>';return;}";
  html += "let w=500,h=140,p=15;";
  html += "let build=(key,mult,minV,maxV,col)=>{let pts=data.map((d,i)=>{let x=p+(i/(data.length-1))*(w-2*p);let v=d[key]*mult;";
  html += "let y=h-p-((v-minV)/(maxV-minV))*(h-2*p);return x.toFixed(1)+','+y.toFixed(1);}).join(' ');";
  html += "return '<polyline fill=\"none\" stroke=\"'+col+'\" stroke-width=\"1.5\" points=\"'+pts+'\"/>';};";
  html += "let out='<line x1=\"15\" y1=\"15\" x2=\"15\" y2=\"125\" stroke=\"#1a241c\"/><line x1=\"15\" y1=\"125\" x2=\"485\" y2=\"125\" stroke=\"#1a241c\"/>';";
  html += "out+=build('t',1,10,40,'#e63946')+build('wt',1,10,40,'#48cae4')+build('h',1,20,100,'#457b9d');";
  html += "out+=build('v',10,0,30,'#52b788')+build('s1',1,0,100,'#e9c46a');svg.innerHTML=out;}";
  html += "function upd(){fetch('/api/data').then(r=>r.json()).then(d=>{";
  html += "document.getElementById('tVal').innerText=(typeof d.temp==='number')?(d.temp.toFixed(1)+' °C'):'--.- °C';";
  html += "document.getElementById('hVal').innerText=(typeof d.hum==='number')?(d.hum.toFixed(1)+' %'):'--.- %';";
  html += "document.getElementById('vpdVal').innerText=(typeof d.vpd==='number')?(d.vpd.toFixed(2)+' kPa'):'--.- kPa';";
  html += "document.getElementById('wtVal').innerText=(typeof d.waterTemp==='number')?(d.waterTemp.toFixed(1)+' °C'):'--.- °C';";
  html += "document.getElementById('stTxt').innerText=d.stage;";
  html += "document.getElementById('dayTxt').innerText='День '+d.day;";
  html += "document.getElementById('tmTxt').innerText=d.time;";
  html += "document.getElementById('camBtn').href=d.camIp;";
  html += "document.getElementById('alertBanner').style.display=d.thermal?'block':'none';";
  html += "document.getElementById('powerAlert').style.display=(d.powerSenseEn&&!d.powerOk)?'block':'none';";
  html += "document.getElementById('waterAlert').style.display=(d.safetyEn&&d.waterLow)?'block':'none';";
  html += "document.getElementById('floodAlert').style.display=(d.safetyEn&&d.flood)?'block':'none';";
  html += "let vb=document.getElementById('vpdTag');";
  html += "if(typeof d.vpd==='number'){if(d.vpd>=d.vpdMin&&d.vpd<=d.vpdMax){vb.className='vpd-tag vpd-ok';vb.innerText='VPD OK';}";
  html += "else if(d.vpd<d.vpdMin){vb.className='vpd-tag vpd-bad';vb.innerText='РИСК ПЛЕСЕНИ';}";
  html += "else{vb.className='vpd-tag vpd-bad';vb.innerText='СУХО';}}else{vb.className='vpd-tag vpd-ok';vb.innerText='ТАЙМЕР';}";
  html += "['1','2','3'].forEach(i=>{document.getElementById('s'+i+'V').innerText=d['s'+i+'Ok']?(d['soil'+i]+'%'):'ОТКЛ';";
  html += "document.getElementById('s'+i+'B').style.width=(d['s'+i+'Ok']?d['soil'+i]:0)+'%';";
  html += "document.getElementById('raw'+i).innerText=d['raw'+i];document.getElementById('dry'+i).innerText=d['dry'+i];document.getElementById('wet'+i).innerText=d['wet'+i];});";
  html += "let setB=(id,st,onT,offT,mode)=>{let el=document.getElementById(id);el.className='badge '+(st?'on':'off')+(mode?' man':'');el.innerText=st?onT:offT;};";
  html += "setB('bLight',d.light,'ВКЛ ('+d.lightPwm+'%)','НОЧЬ/СУШКА',d.mLight);";
  html += "setB('bExh',d.exhaust,'ВКЛ','ВЫКЛ',d.mExh);";
  html += "setB('bHeat',d.heater,'ГРЕЕТ','ВЫКЛ',d.mHeat);";
  html += "setB('bFan',d.fan,'ВЕТЕР','ПАУЗА',d.mFan);";
  html += "setB('bHumid',d.humid,d.humidEn?'ПАРИТ':'ТЕСТ','ВЫКЛ',d.mHumid);";
  html += "setB('bPump',d.pump,'КАЧАЕТ','СТОП',0);";
  html += "setB('bPower',d.powerOk,'В СЕТИ','НЕТ СЕТИ',0);";
  html += "modeBtns('mdLight','light',d.mLight);modeBtns('mdExh','exhaust',d.mExh);";
  html += "modeBtns('mdHeat','heater',d.mHeat);modeBtns('mdFan','fan',d.mFan);modeBtns('mdHumid','humid',d.mHumid);";
  html += "document.getElementById('vStat').innerText='['+(d.v1?'1':'0')+', '+(d.v2?'1':'0')+', '+(d.v3?'1':'0')+']';";
  html += "document.getElementById('wProg').innerText=d.watering>=0?('⏳ Полив зоны #'+(d.watering+1)+'...'):'';";
  html += "if(document.getElementById('devId')){document.getElementById('devId').innerText=d.devId||'';";
  html += "document.getElementById('rEvent').innerText=d.rEvent||'-';document.getElementById('otaRes').innerText=d.otaRes||'-';";
  html += "let br=document.getElementById('bRemote');br.className='badge '+(d.remoteEn?'on':'off');br.innerText=d.remoteEn?'ОНЛАЙН':'ВЫКЛ';}";
  html += "}).catch(e=>console.error(e));}";
  html += "function loadHistory(){fetch('/api/history').then(r=>r.json()).then(drawCharts).catch(e=>console.error(e));}";
  html += "setInterval(upd,2000);upd();setInterval(loadHistory,60000);loadHistory();";
  html += "</script></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleWater() {
  if (server.hasArg("z")) triggerWatering(server.arg("z").toInt());
  server.send(200, "text/plain", "OK");
}

void handleSetStage() {
  if (server.hasArg("s")) {
    String s = server.arg("s");
    if (s == "veg") setGrowStage(STAGE_VEG);
    else if (s == "bloom") setGrowStage(STAGE_BLOOM);
    else if (s == "dry") setGrowStage(STAGE_DRY);
  }
  server.send(200, "text/plain", "OK");
}

void handleSetMode() {
  if (!server.hasArg("d") || !server.hasArg("m")) {
    server.send(400, "text/plain", "NEED_D_M");
    return;
  }
  if (!setDeviceMode(server.arg("d"), parseModeArg(server.arg("m")))) {
    server.send(400, "text/plain", "BAD_DEV");
    return;
  }
  server.send(200, "text/plain", "OK");
}

void handleAllAuto() {
  allAuto();
  server.send(200, "text/plain", "OK");
}

void handleToggleSafety() {
  enableSafetySensors = !enableSafetySensors;
  prefs.begin("growbox", false);
  prefs.putBool("safetyEn", enableSafetySensors);
  prefs.end();
  server.send(200, "text/plain", enableSafetySensors ? "SAFETY_ON" : "SAFETY_OFF");
}

void handleTogglePower() {
  enablePowerSense = !enablePowerSense;
  prefs.begin("growbox", false);
  prefs.putBool("powerEn", enablePowerSense);
  prefs.end();
  if (!enablePowerSense) powerGridOk = true;
  server.send(200, "text/plain", enablePowerSense ? "POWER_SENSE_ON" : "POWER_SENSE_OFF");
}

void handleResetCycle() {
  if (!ntpReady()) {
    server.send(503, "text/plain", "NO_NTP");
    return;
  }
  startNewCycle();
  server.send(200, "text/plain", "OK");
}

void handleCalib() {
  if (server.hasArg("z") && server.hasArg("t")) {
    int z = server.arg("z").toInt();
    String t = server.arg("t");
    if (z >= 0 && z < 3) {
      prefs.begin("growbox", false);
      if (t == "dry") {
        soilCalibDry[z] = soilRaw[z];
        prefs.putInt(("dry" + String(z)).c_str(), soilCalibDry[z]);
      } else if (t == "wet") {
        soilCalibWet[z] = soilRaw[z];
        prefs.putInt(("wet" + String(z)).c_str(), soilCalibWet[z]);
      }
      prefs.end();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSaveConfig() {
  if (server.hasArg("tgToken")) tgBotToken = server.arg("tgToken");
  if (server.hasArg("tgChat"))  tgChatId = server.arg("tgChat");
  if (server.hasArg("camIp"))   camIp = server.arg("camIp");
  tgEnabled = (tgBotToken.length() > 10 && tgChatId.length() > 2);
  prefs.begin("growbox", false);
  prefs.putString("tgToken", tgBotToken);
  prefs.putString("tgChat", tgChatId);
  prefs.putString("camIp", camIp);
  prefs.putBool("tgEn", tgEnabled);
  prefs.end();
  if (tgEnabled) {
    sendTelegramMessage(String("🌿 <b>GrowBox Enterprise Connected!</b>\nКонтроллер v") + FIRMWARE_VERSION + " готов. /help");
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

float argF(const char* name, float cur, float lo, float hi) {
  if (!server.hasArg(name)) return cur;
  return constrain(server.arg(name).toFloat(), lo, hi);
}

int argI(const char* name, int cur, int lo, int hi) {
  if (!server.hasArg(name)) return cur;
  return constrain(server.arg(name).toInt(), lo, hi);
}

void handleSaveSettings() {
  vegStartHour = argI("vegH0", vegStartHour, 0, 23);
  vegEndHour = argI("vegH1", vegEndHour, 1, 24);
  bloomStartHour = argI("blmH0", bloomStartHour, 0, 23);
  bloomEndHour = argI("blmH1", bloomEndHour, 1, 24);
  sunriseMin = argI("sunrise", sunriseMin, 0, 120);
  soilDryThreshold = argI("soilDry", soilDryThreshold, 5, 80);
  wateringDurationMs = (unsigned long)argI("waterSec", (int)(wateringDurationMs / 1000), 1, 120) * 1000UL;
  soilSoakDelayMs = (unsigned long)argI("soakMin", (int)(soilSoakDelayMs / 60000), 5, 240) * 60000UL;
  fallbackWateringMs = (unsigned long)argI("fbHours", (int)(fallbackWateringMs / 3600000UL), 6, 72) * 3600000UL;
  windOnMs = (unsigned long)argI("windOn", (int)(windOnMs / 60000), 1, 60) * 60000UL;
  windOffMs = (unsigned long)argI("windOff", (int)(windOffMs / 60000), 1, 60) * 60000UL;
  tempTargetDay = argF("tDay", tempTargetDay, 10, 35);
  tempTargetNight = argF("tNight", tempTargetNight, 8, 30);
  tempTargetDry = argF("tDry", tempTargetDry, 8, 25);
  tempHysteresis = argF("tHyst", tempHysteresis, 0.2, 5);
  tempEmergency = argF("tEmerg", tempEmergency, 28, 45);
  vpdVegMin = argF("vpdVmin", vpdVegMin, 0.2, 2.5);
  vpdVegMax = argF("vpdVmax", vpdVegMax, 0.3, 3.0);
  vpdBloomMin = argF("vpdBmin", vpdBloomMin, 0.2, 2.5);
  vpdBloomMax = argF("vpdBmax", vpdBloomMax, 0.3, 3.0);
  if (vpdVegMax < vpdVegMin) vpdVegMax = vpdVegMin + 0.1;
  if (vpdBloomMax < vpdBloomMin) vpdBloomMax = vpdBloomMin + 0.1;
  enableHumidifier = server.hasArg("humidEn");
  persistSettings();
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  // WDT только после Wi-Fi: портал WiFiManager живёт до 180 сек.

  uint8_t relayPins[] = {RELAY_LIGHT, RELAY_EXHAUST, RELAY_HEATER, RELAY_FAN,
                         RELAY_PUMP, RELAY_VALVE1, RELAY_VALVE2, RELAY_VALVE3,
                         RELAY_HUMIDIFIER};
  for (int i = 0; i < 9; i++) {
    digitalWrite(relayPins[i], RELAY_OFF);
    pinMode(relayPins[i], OUTPUT);
  }

  pinMode(PIN_LIGHT_PWM, OUTPUT);
  analogWrite(PIN_LIGHT_PWM, 0);

  pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  pinMode(FLOOD_SENSOR_PIN, INPUT_PULLUP);
  pinMode(POWER_SENSE_PIN, INPUT_PULLUP);

  prefs.begin("growbox", false);
  currentStage = (GrowStage)prefs.getInt("stage", 0);
  cycleStartTimestamp = prefs.getULong("start", 0);
  enableSafetySensors = prefs.getBool("safetyEn", false);
  enablePowerSense   = prefs.getBool("powerEn", false);
  tgBotToken = prefs.getString("tgToken", "");
  tgChatId = prefs.getString("tgChat", "");
  camIp = prefs.getString("camIp", "http://esp32-cam.local");
  tgEnabled = prefs.getBool("tgEn", false);
  for (int i = 0; i < 3; i++) {
    soilCalibDry[i] = prefs.getInt(("dry" + String(i)).c_str(), 3200);
    soilCalibWet[i] = prefs.getInt(("wet" + String(i)).c_str(), 1400);
    lastWateredUnix[i] = prefs.getULong((String("wunix") + i).c_str(), 0);
  }
  loadSettings();
  loadRemoteSettings();
  prefs.end();

  dht.begin();
  waterSensors.begin();
  waterSensors.setWaitForConversion(false);
  pinMode(SOIL1_PIN, INPUT);
  pinMode(SOIL2_PIN, INPUT);
  pinMode(SOIL3_PIN, INPUT);

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("Growbox-Setup", "12345678")) {
    Serial.println("[WiFi] Перезапуск...");
    ESP.restart();
  }

  startWatchdog();
  MDNS.begin("growbox");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/", handleRoot);
  server.on("/api/data", handleApiData);
  server.on("/api/history", handleApiHistory);
  server.on("/export.csv", handleExportCsv);
  server.on("/water", handleWater);
  server.on("/setStage", handleSetStage);
  server.on("/setMode", handleSetMode);
  server.on("/allAuto", handleAllAuto);
  server.on("/toggleSafety", handleToggleSafety);
  server.on("/togglePower", handleTogglePower);
  server.on("/resetCycle", handleResetCycle);
  server.on("/calib", handleCalib);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);
  server.on("/saveRemote", HTTP_POST, handleSaveRemote);
  server.on("/remotePull", handleRemotePull);
  server.on("/otaCheck", handleOtaCheck);

  ElegantOTA.begin(&server);
  server.begin();

  Serial.print("[GrowBox Enterprise v");
  Serial.print(FIRMWARE_VERSION);
  Serial.println("] Запущен!");
}

void applyLight(bool autoOn, int autoPwm) {
  if (thermalShutdown) {
    stateLight = false;
    lightPwmDuty = 0;
  } else if (modeLight == MODE_ON) {
    stateLight = true;
    lightPwmDuty = 255;
  } else if (modeLight == MODE_OFF) {
    stateLight = false;
    lightPwmDuty = 0;
  } else {
    stateLight = autoOn;
    lightPwmDuty = autoPwm;
  }
  setRelay(RELAY_LIGHT, stateLight);
  analogWrite(PIN_LIGHT_PWM, lightPwmDuty);
}

bool computeHumidAuto() {
  if (!enableHumidifier || !dhtConnected) return false;
  if (currentStage == STAGE_DRY) return false;
  if (humidity >= 75.0) return false;

  float vmin, vmax;
  getVpdTargets(vmin, vmax);

  // слишком влажно / риск плесени — никогда не увлажняем
  if (vpd < vmin) return false;
  // слишком сухо — включаем
  if (vpd > vmax) return true;
  // в коридоре — гистерезис: оставляем как есть
  return stateHumid;
}

void loop() {
  feedWatchdog();
  server.handleClient();
  ensureCycleStart();
  performPendingOta();

  unsigned long currentMillis = millis();
  if (remoteEnabled && (currentMillis - lastRemotePoll >= REMOTE_POLL_MS || lastRemotePoll == 0)) {
    lastRemotePoll = currentMillis;
    pollRemoteAgent();
  }
  if (remoteEnabled && ntfyTopic.length() > 4 &&
      (currentMillis - lastRemoteHeartbeat >= REMOTE_HEARTBEAT_MS || lastRemoteHeartbeat == 0)) {
    lastRemoteHeartbeat = currentMillis;
    publishRemoteStatus("heartbeat");
    if (autoOta) checkVersionFile(true);
  }

  if (currentMillis - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = currentMillis;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > -30.0 && t < 70.0 && h >= 0.0 && h <= 100.0) {
      dhtConnected = true;
      temperature = t;
      humidity = h;
      vpd = calculateVPD(temperature, humidity);
    } else {
      dhtConnected = false;
    }

    float wt = waterSensors.getTempCByIndex(0);
    if (wt > -40.0 && wt < 65.0) {
      ds18Connected = true;
      waterTemperature = wt;
    } else {
      ds18Connected = false;
      waterTemperature = -127.0;
    }
    waterSensors.requestTemperatures();

    for (int i = 0; i < 3; i++) {
      int pin = (i == 0 ? SOIL1_PIN : (i == 1 ? SOIL2_PIN : SOIL3_PIN));
      int raw = analogRead(pin);
      soilRaw[i] = raw;
      if (raw > 100 && raw < 4000) {
        soilConnected[i] = true;
        soilMoisture[i] = readSoilPercent(i, raw);
      } else {
        soilConnected[i] = false;
        soilMoisture[i] = 0;
      }
    }

    if (enablePowerSense) {
      powerGridOk = (digitalRead(POWER_SENSE_PIN) == LOW);
      if (!powerGridOk && (currentMillis - lastPowerAlertTime > 1800000 || lastPowerAlertTime == 0)) {
        lastPowerAlertTime = currentMillis;
        sendTelegramMessage("🚨 <b>ВНИМАНИЕ:</b> Отключение сети 220V!");
      }
    } else {
      powerGridOk = true;
    }

    if (enableSafetySensors) {
      isWaterLow = (digitalRead(WATER_LEVEL_PIN) == HIGH);
      isFloodDetected = (digitalRead(FLOOD_SENSOR_PIN) == LOW);
    } else {
      isWaterLow = false;
      isFloodDetected = false;
    }

    bool autoLight = false;
    int autoPwm = 0;
    bool autoExhaust = true;
    bool autoHeater = false;
    bool isDay = false;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int curSecOfDay = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;

      if (currentStage == STAGE_DRY) {
        autoLight = false;
        autoPwm = 0;
        if (dryVentState && (currentMillis - dryVentTimer >= 30000)) {
          dryVentState = false;
          dryVentTimer = currentMillis;
        } else if (!dryVentState && (currentMillis - dryVentTimer >= 600000)) {
          dryVentState = true;
          dryVentTimer = currentMillis;
        }
        autoExhaust = dryVentState;
        if (dhtConnected) {
          if (temperature < (tempTargetDry - tempHysteresis)) autoHeater = true;
          else if (temperature >= tempTargetDry) autoHeater = false;
          else autoHeater = stateHeater && (modeHeater == MODE_AUTO);
        }
      } else {
        int startH = (currentStage == STAGE_VEG) ? vegStartHour : bloomStartHour;
        int endH   = (currentStage == STAGE_VEG) ? vegEndHour   : bloomEndHour;
        int startSec = startH * 3600;
        int endSec   = endH * 3600;
        int rampSec  = sunriseMin * 60;

        if (curSecOfDay >= startSec && curSecOfDay < endSec) {
          isDay = true;
          if (rampSec > 0 && curSecOfDay < startSec + rampSec) {
            autoPwm = map(curSecOfDay - startSec, 0, rampSec, 10, 255);
          } else if (rampSec > 0 && curSecOfDay >= endSec - rampSec) {
            autoPwm = map(endSec - curSecOfDay, 0, rampSec, 10, 255);
          } else {
            autoPwm = 255;
          }
          autoLight = true;
        }

        autoExhaust = true;
        if (dhtConnected) {
          float target = isDay ? tempTargetDay : tempTargetNight;
          if (temperature < (target - tempHysteresis)) autoHeater = true;
          else if (temperature >= target) autoHeater = false;
          else autoHeater = stateHeater && (modeHeater == MODE_AUTO);
        }

        float vmin, vmax;
        getVpdTargets(vmin, vmax);
        if (dhtConnected && currentStage == STAGE_BLOOM && vpd < vmin &&
            (currentMillis - lastTgAlertTime > 14400000)) {
          lastTgAlertTime = currentMillis;
          sendTelegramMessage("⚠️ <b>Риск плесени:</b> VPD " + String(vpd, 2) +
                              " kPa, RH " + String(humidity, 1) + "%");
        }
      }
    } else {
      // нет NTP — автосвет не трогаем, ручной режим всё равно сработает ниже
      autoExhaust = (currentStage != STAGE_DRY);
    }

    if (dhtConnected && temperature >= tempEmergency) {
      if (!thermalShutdown) {
        thermalShutdown = true;
        sendTelegramMessage("🔥 <b>ПЕРЕГРЕВ:</b> " + String(temperature, 1) + "°C! Свет отключен.");
      }
    } else if (dhtConnected && temperature < (tempEmergency - 2.0)) {
      thermalShutdown = false;
    }

    applyLight(autoLight, autoPwm);
    stateExhaust = applyOverride(modeExhaust, autoExhaust);
    stateHeater  = applyOverride(modeHeater, autoHeater);
    setRelay(RELAY_EXHAUST, stateExhaust);
    setRelay(RELAY_HEATER, stateHeater);

    bool autoHumid = computeHumidAuto();
    stateHumid = applyOverride(modeHumid, autoHumid);
    if (!enableHumidifier && modeHumid != MODE_ON) stateHumid = false;
    setRelay(RELAY_HUMIDIFIER, stateHumid);
  }

  if (lastSensorRead != 0 && (lastHistoryLog == 0 || currentMillis - lastHistoryLog >= HISTORY_INTERVAL)) {
    lastHistoryLog = currentMillis;
    logHistoryPoint();
  }

  if (tgEnabled && (currentMillis - lastTgPoll >= TG_POLL_INTERVAL)) {
    lastTgPoll = currentMillis;
    checkTelegramUpdates();
  }

  if (currentStage != STAGE_DRY) {
    if (windState && (currentMillis - windTimer >= windOnMs)) {
      windState = false;
      windTimer = currentMillis;
    } else if (!windState && (currentMillis - windTimer >= windOffMs)) {
      windState = true;
      windTimer = currentMillis;
    }
  } else {
    windState = false;
  }
  stateFan = applyOverride(modeFan, (currentStage != STAGE_DRY) ? windState : false);
  setRelay(RELAY_FAN, stateFan);

  if (currentStage != STAGE_DRY) {
    if (activeWateringZone == -1) {
      if (!enableSafetySensors || (!isWaterLow && !isFloodDetected)) {
        for (int i = 0; i < 3; i++) {
          if (soilConnected[i]) {
            if (soilMoisture[i] < soilDryThreshold && soakDelayPassed(i)) {
              triggerWatering(i);
              break;
            }
          } else if (fallbackWaterDue(i)) {
            triggerWatering(i);
            break;
          }
        }
      }
    } else {
      if (enableSafetySensors && isFloodDetected) {
        statePump = false;
        setRelay(RELAY_PUMP, false);
        for (int i = 0; i < 3; i++) {
          stateValves[i] = false;
          setRelay(i == 0 ? RELAY_VALVE1 : (i == 1 ? RELAY_VALVE2 : RELAY_VALVE3), false);
        }
        activeWateringZone = -1;
        sendTelegramMessage("🚨 <b>АВАРИЯ:</b> Полив остановлен из-за протечки!");
      } else if (currentMillis - wateringStartTime >= wateringDurationMs) {
        statePump = false;
        setRelay(RELAY_PUMP, false);
        stateValves[activeWateringZone] = false;
        setRelay(activeWateringZone == 0 ? RELAY_VALVE1 : (activeWateringZone == 1 ? RELAY_VALVE2 : RELAY_VALVE3), false);
        markZoneWatered(activeWateringZone);
        activeWateringZone = -1;
      }
    }
  }
}
