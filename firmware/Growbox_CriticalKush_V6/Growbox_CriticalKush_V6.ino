#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include "time.h"
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

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

// ШИМ Диммирование (Рассвет/Закат)
#define PIN_LIGHT_PWM     15

// Датчики
#define DHT_PIN           4   // Климат DHT22
#define DHTTYPE           DHT22

#define ONE_WIRE_BUS      27  // Вода в баке (DS18B20)

#define SOIL1_PIN         32  // Почва 1 (ADC1)
#define SOIL2_PIN         34  // Почва 2 (ADC1)
#define SOIL3_PIN         35  // Почва 3 (ADC1)

#define WATER_LEVEL_PIN   13  // Поплавок бака (к GND)
#define FLOOD_SENSOR_PIN  14  // Датчик протечки пола (к GND)
#define POWER_SENSE_PIN   33  // Контроль сети 220V (к GND через оптопару)

// Уровни реле (Active-LOW)
#define RELAY_ON          LOW
#define RELAY_OFF         HIGH

#define WDT_TIMEOUT_SEC   8
#define FIRMWARE_VERSION  "6.1"

// ================= СТАДИИ ГРОВА =================
enum GrowStage {
  STAGE_VEG = 0,   // Вегетация 18/6
  STAGE_BLOOM = 1, // Цветение 12/12
  STAGE_DRY = 2    // Сушка и пролечка 60/60 (Свет 0%, Темп ~16°C, Влажность ~60%)
};

GrowStage currentStage = STAGE_VEG;

// Пороги температуры (°C)
float tempTargetDay   = 24.5;
float tempTargetNight = 20.5;
float tempTargetDry   = 16.0;
float tempHysteresis  = 1.0;
const float TEMP_EMERGENCY = 32.5;

// Полив Dry-Back
int soilDryThreshold = 28;
const unsigned long WATERING_DURATION = 8000;    // 8 сек
const unsigned long SOIL_SOAK_DELAY   = 2700000; // 45 мин
const unsigned long FALLBACK_WATERING_INTERVAL = 86400000; // Резервный таймерный полив раз в 24ч

// Обдув (15 мин дует / 5 мин отдых)
const unsigned long WIND_ON  = 15 * 60 * 1000;
const unsigned long WIND_OFF = 5 * 60 * 1000;

// Длительность рассвета/заката (30 мин)
const int SUNRISE_DURATION_MIN = 30;

// NTP Время
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 2 * 3600;
const int   daylightOffset_sec = 3600;

// ================= ОБЪЕКТЫ =================
WebServer server(80);
DHT dht(DHT_PIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature waterSensors(&oneWire);
Preferences prefs;

// ================= 24-ЧАСОВОЙ БУФЕР ИСТОРИИ =================
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

// ================= ПЕРЕМЕННЫЕ СОСТОЯНИЯ =================
float temperature = 0.0;
float humidity = 0.0;
float vpd = 0.0;
float waterTemperature = -127.0;

// Флаги автоматического обнаружения датчиков
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
bool stateValves[3] = {false, false, false};

bool enableSafetySensors = false;
bool enablePowerSense   = false;
bool isWaterLow = false;
bool isFloodDetected = false;
bool thermalShutdown = false;

// Telegram Bot
String tgBotToken = "";
String tgChatId   = "";
String camIp      = "http://esp32-cam.local";
bool tgEnabled    = false;
long lastTgUpdateId = 0;
unsigned long lastTgPoll = 0;
const unsigned long TG_POLL_INTERVAL = 3500;
unsigned long lastTgAlertTime = 0;
unsigned long lastPowerAlertTime = 0;

// Таймеры
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

// Таймер микро-проветривания для режима сушки
unsigned long dryVentTimer = 0;
bool dryVentState = false;

void feedWatchdog() {
  if (wdtStarted) {
    esp_task_wdt_reset();
  }
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
  if (lastWateredTime[zone] != 0 && (nowMs - lastWateredTime[zone] < SOIL_SOAK_DELAY)) {
    return false;
  }
  if (lastWateredUnix[zone] != 0 && ntpReady()) {
    time_t now;
    time(&now);
    if ((unsigned long)now >= lastWateredUnix[zone] &&
        ((unsigned long)now - lastWateredUnix[zone]) < (SOIL_SOAK_DELAY / 1000UL)) {
      return false;
    }
  }
  return true;
}

bool fallbackWaterDue(int zone) {
  if (lastWateredUnix[zone] != 0 && ntpReady()) {
    time_t now;
    time(&now);
    return ((unsigned long)now - lastWateredUnix[zone]) >= (FALLBACK_WATERING_INTERVAL / 1000UL);
  }
  if (lastWateredTime[zone] == 0) {
    lastWateredTime[zone] = millis();
    return false;
  }
  return (millis() - lastWateredTime[zone]) >= FALLBACK_WATERING_INTERVAL;
}

void setRelay(uint8_t pin, bool state) {
  digitalWrite(pin, state ? RELAY_ON : RELAY_OFF);
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
  if (now < cycleStartTimestamp) return 1;
  return (int)((now - cycleStartTimestamp) / 86400) + 1;
}

// ================= TELEGRAM =================
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

    sendTelegramMessage("🚿 <b>Полив:</b> Запущен полив горшка #" + String(zone + 1) + " (8 сек)");
  }
}

void setGrowStage(GrowStage stage) {
  currentStage = stage;
  prefs.begin("growbox", false);
  prefs.putInt("stage", (int)currentStage);
  prefs.end();
}

void handleTelegramCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "/start" || cmd == "/help") {
    String helpMsg = "🌿 <b>Команды GrowBox Enterprise v";
    helpMsg += FIRMWARE_VERSION;
    helpMsg += ":</b>\n\n";
    helpMsg += "/status - Полная сводка параметров\n";
    helpMsg += "/photo - Снимок с камеры\n";
    helpMsg += "/water1, /water2, /water3 - Полив зон\n";
    helpMsg += "/veg - Вегетация (18/6)\n";
    helpMsg += "/bloom - Цветение (12/12)\n";
    helpMsg += "/dry - Режим сушки (60/60, свет ВЫКЛ)\n";
    helpMsg += "/resetday - Сбросить счётчик дня цикла\n";
    helpMsg += "/help - Справка";
    sendTelegramMessage(helpMsg);
  }
  else if (cmd == "/photo") {
    sendTelegramMessage("📸 <b>ESP32-CAM Камера:</b>\n" + camIp + "/capture");
  }
  else if (cmd == "/status") {
    struct tm timeinfo;
    char timeStr[32] = "--:--";
    if (getLocalTime(&timeinfo)) {
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    }
    String s = "🌿 <b>Статус GrowBox [Enterprise v";
    s += FIRMWARE_VERSION;
    s += "]</b>\n\n";
    String stName = "Вегетация (18/6)";
    if (currentStage == STAGE_BLOOM) stName = "Цветение (12/12)";
    else if (currentStage == STAGE_DRY) stName = "Сушка 60/60 (Урожай)";
    
    s += "📅 <b>Режим:</b> " + stName + " | <b>День:</b> " + String(getGrowDay()) + "\n";
    s += "🕒 <b>Время:</b> " + String(timeStr) + "\n";
    s += "⚡ <b>Сеть 220V:</b> " + String(powerGridOk ? "✅ В норме" : "🚨 НЕТ ПИТАНИЯ (АКБ)") + "\n\n";

    if (dhtConnected) {
      s += "🌡️ <b>Воздух:</b> " + String(temperature, 1) + " °C | <b>Влажность:</b> " + String(humidity, 1) + " %\n";
      s += "💨 <b>VPD:</b> " + String(vpd, 2) + " kPa (" + (vpd >= 0.8 && vpd <= 1.5 ? "✅ Идеал" : (vpd < 0.8 ? "⚠️ Риск плесени" : "⚠️ Сухо")) + ")\n";
    } else {
      s += "🌡️ <b>Воздух:</b> ⚠️ <i>Датчик DHT отключен (Авто-таймер)</i>\n";
    }

    if (ds18Connected) {
      s += "🧪 <b>Вода в баке:</b> " + String(waterTemperature, 1) + " °C\n";
    }

    s += "\n🪴 <b>Влажность почвы:</b>\n";
    for (int i = 0; i < 3; i++) {
      s += " • Горшок #" + String(i+1) + ": " + (soilConnected[i] ? (String(soilMoisture[i]) + "%") : "⚠️ <i>Отключен</i>") + "\n";
    }

    s += "\n⚡ <b>Реле:</b> Свет=" + String(stateLight ? "ВКЛ" : "ВЫКЛ") + ", Вытяжка=" + String(stateExhaust ? "ВКЛ" : "ВЫКЛ") + 
         ", Обогрев=" + String(stateHeater ? "ВКЛ" : "ВЫКЛ") + ", Обдув=" + String(stateFan ? "ВКЛ" : "ВЫКЛ");
    sendTelegramMessage(s);
  }
  else if (cmd == "/water1" || cmd == "/water 1") triggerWatering(0);
  else if (cmd == "/water2" || cmd == "/water 2") triggerWatering(1);
  else if (cmd == "/water3" || cmd == "/water 3") triggerWatering(2);
  else if (cmd == "/veg") {
    setGrowStage(STAGE_VEG);
    sendTelegramMessage("🌱 <b>Стадия:</b> ВЕГЕТАЦИЯ (18/6)");
  }
  else if (cmd == "/bloom") {
    setGrowStage(STAGE_BLOOM);
    sendTelegramMessage("🌸 <b>Стадия:</b> ЦВЕТЕНИЕ (12/12)");
  }
  else if (cmd == "/dry") {
    setGrowStage(STAGE_DRY);
    sendTelegramMessage("🍂 <b>Стадия:</b> СУШКА И ПРОЛЕЧКА 60/60 (Свет выключен, удержание 16°C и 60% влажности)");
  }
  else {
    sendTelegramMessage("❓ Неизвестная команда. Напишите /help");
  }
}

void checkTelegramUpdates() {
  if (!tgEnabled || tgBotToken.length() < 15 || tgChatId.length() < 3) return;

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

// ================= ВЕБ-СЕРВЕР =================
void handleApiData() {
  struct tm timeinfo;
  char timeStr[32] = "--:--:--";
  if (getLocalTime(&timeinfo)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }

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
  json += "\"pump\":" + String(statePump ? 1 : 0) + ",";
  json += "\"v1\":" + String(stateValves[0] ? 1 : 0) + ",";
  json += "\"v2\":" + String(stateValves[1] ? 1 : 0) + ",";
  json += "\"v3\":" + String(stateValves[2] ? 1 : 0) + ",";
  
  String stLabel = "Вегетация (18/6)";
  if (currentStage == STAGE_BLOOM) stLabel = "Цветение (12/12)";
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
  json += "\"tgEn\":" + String(tgEnabled ? 1 : 0) + ",";
  json += "\"camIp\":\"" + camIp + "\",";
  json += "\"fw\":\"" + String(FIRMWARE_VERSION) + "\",";
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

// 📥 Экспорт истории в Excel CSV файл
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
  html += "</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; font-family: -apple-system, system-ui, sans-serif; }";
  html += "body { background: #070a08; color: #e2e8e3; margin: 0; padding: 10px; }";
  html += ".wrap { max-width: 980px; margin: 0 auto; }";
  html += "header { text-align: center; margin-bottom: 10px; }";
  html += "h1 { color: #52b788; margin: 0; font-size: 20px; font-weight: 800; }";
  html += ".sub { color: #829285; font-size: 12px; margin-top: 2px; }";
  html += ".alert { background: #780000; border: 1px solid #c1121f; color: #fff; padding: 6px 10px; border-radius: 6px; margin-bottom: 8px; font-weight: bold; display: none; font-size: 12px; text-align: center; }";
  html += ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(270px, 1fr)); gap: 10px; }";
  html += ".card { background: #101612; border-radius: 10px; padding: 12px; border: 1px solid #1a241c; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }";
  html += ".card-t { font-size: 13px; font-weight: 700; color: #74c69d; margin-bottom: 6px; border-bottom: 1px solid #1a241c; padding-bottom: 4px; display: flex; justify-content: space-between; align-items: center; }";
  html += ".row { display: flex; justify-content: space-between; align-items: center; margin: 5px 0; font-size: 13px; }";
  html += ".big { font-size: 22px; font-weight: 800; color: #fff; }";
  html += ".badge { padding: 2px 7px; border-radius: 4px; font-size: 10px; font-weight: 700; text-transform: uppercase; }";
  html += ".on { background: #2d6a4f; color: #d8f3dc; }";
  html += ".off { background: #49111c; color: #ffb4a2; }";
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
  html += ".box { margin-top: 10px; background: #0c100d; border: 1px solid #1a241c; border-radius: 8px; padding: 10px; }";
  html += "input[type=text] { width: 100%; background: #151d17; border: 1px solid #253328; color: #fff; padding: 5px; border-radius: 4px; margin: 3px 0 6px; font-size: 11px; }";
  html += "svg { width: 100%; height: 140px; background: #080c09; border-radius: 6px; margin-top: 6px; }";
  html += ".chart-legend { display: flex; justify-content: space-around; font-size: 10px; margin-top: 4px; }";
  html += ".footer { text-align: center; margin-top: 12px; font-size: 11px; color: #74c69d; }";
  html += ".footer a { color: #74c69d; text-decoration: none; }";
  html += "</style></head><body>";

  html += "<div class='wrap'>";
  html += "<div id='alertBanner' class='alert'>⚠️ АВАРИЙНЫЙ ПЕРЕГРЕВ (>32.5°C)! Свет отключен.</div>";
  html += "<div id='powerAlert' class='alert' style='background:#b02a37;'>🚨 ПРОПАЛО ПИТАНИЕ 220V! Система работает от резерва.</div>";
  html += "<div id='waterAlert' class='alert' style='background:#9d0208;'>🪣 БАК ПУСТ! Полив заблокирован.</div>";
  html += "<div id='floodAlert' class='alert' style='background:#d00000;'>🚨 ПРОТЕЧКА В ПОДДОНЕ! Помпа отключена.</div>";

  html += "<header>";
  html += "<h1>🌿 Critical Kush Enterprise v";
  html += FIRMWARE_VERSION;
  html += "</h1>";
  html += "<div class='sub'>Режим: <b id='stTxt'>...</b> | <b id='dayTxt'>День ...</b> | Время: <b id='tmTxt'>--:--</b></div>";
  html += "</header>";

  html += "<div class='grid'>";

  // Климат и Питание
  html += "<div class='card'>";
  html += "<div class='card-t'><span>🌡️ Климат и Питание</span><span id='vpdTag' class='vpd-tag vpd-ok'>VPD OK</span></div>";
  html += "<div class='row'><span>Воздух:</span><span class='big' id='tVal'>--.- °C</span></div>";
  html += "<div class='row'><span>Влажность:</span><span class='big' id='hVal'>--.- %</span></div>";
  html += "<div class='row'><span>VPD Дефицит:</span><b id='vpdVal' style='color:#52b788;'>--.- kPa</b></div>";
  html += "<div class='row'><span>Вода в баке (DS18):</span><b id='wtVal' style='color:#48cae4;'>--.- °C</b></div>";
  html += "</div>";

  // Почва
  html += "<div class='card'>";
  html += "<div class='card-t'><span>💧 Почва (Dry-Back)</span></div>";
  html += "<div style='margin-bottom:5px;'><div class='row'><span>Горшок #1:</span><b id='s1V'>--%</b></div><div class='bar-bg'><div id='s1B' class='bar-fill'></div></div></div>";
  html += "<div style='margin-bottom:5px;'><div class='row'><span>Горшок #2:</span><b id='s2V'>--%</b></div><div class='bar-bg'><div id='s2B' class='bar-fill'></div></div></div>";
  html += "<div style='margin-bottom:5px;'><div class='row'><span>Горшок #3:</span><b id='s3V'>--%</b></div><div class='bar-bg'><div id='s3B' class='bar-fill'></div></div></div>";
  html += "<div class='btn-grid' style='grid-template-columns: 1fr 1fr 1fr;'>";
  html += "<button class='btn btn-w' onclick='fetch(\"/water?z=0\")'>💧 #1</button>";
  html += "<button class='btn btn-w' onclick='fetch(\"/water?z=1\")'>💧 #2</button>";
  html += "<button class='btn btn-w' onclick='fetch(\"/water?z=2\")'>💧 #3</button>";
  html += "</div></div>";

  // Нагрузки и Стадии
  html += "<div class='card'>";
  html += "<div class='card-t'><span>⚡ Оборудование и Стадии</span></div>";
  html += "<div class='row'><span>💡 Свет (ШИМ):</span><span id='bLight' class='badge off'>ВЫКЛ (0%)</span></div>";
  html += "<div class='row'><span>🌪️ Вытяжка:</span><span id='bExh' class='badge on'>ВКЛ</span></div>";
  html += "<div class='row'><span>🔥 Обогрев:</span><span id='bHeat' class='badge off'>ВЫКЛ</span></div>";
  html += "<div class='row'><span>🌀 Обдув (Ветер):</span><span id='bFan' class='badge on'>ВКЛ</span></div>";
  html += "<div class='btn-grid' style='grid-template-columns: 1fr 1fr 1fr;'>";
  html += "<button class='btn btn-sec' onclick='fetch(\"/setStage?s=veg\")'>🌱 Вега</button>";
  html += "<button class='btn btn-sec' onclick='fetch(\"/setStage?s=bloom\")'>🌸 Цвет</button>";
  html += "<button class='btn btn-dry' onclick='fetch(\"/setStage?s=dry\")'>🍂 Сушка</button>";
  html += "</div></div>";

  // Помпа и Сенсоры
  html += "<div class='card'>";
  html += "<div class='card-t'><span>🚿 Помпа и Безопасность</span></div>";
  html += "<div class='row'><span>⚙️ Помпа:</span><span id='bPump' class='badge off'>СТОП</span></div>";
  html += "<div class='row'><span>Клапаны [1, 2, 3]:</span><b id='vStat'>[0, 0, 0]</b></div>";
  html += "<div class='row'><span>Питание 220V:</span><span id='bPower' class='badge on'>В СЕТИ</span></div>";
  html += "<div id='wProg' style='font-size:11px; color:#f4a261; text-align:center; min-height:14px; margin-top:2px;'></div>";
  html += "<div class='btn-grid'>";
  html += "<a id='camBtn' href='#' target='_blank' class='btn btn-sec'>📸 Камера</a>";
  html += "<a href='/export.csv' class='btn btn-sec'>📥 Скачать CSV</a>";
  html += "</div></div>";

  html += "</div>"; // grid

  // 📈 ГРАФИКИ
  html += "<div class='box'>";
  html += "<div style='display:flex; justify-content:space-between; align-items:center;'>";
  html += "<h3 style='margin:0; font-size:13px; color:#74c69d;'>📈 Графики за 24 часа</h3>";
  html += "<a href='/export.csv' style='font-size:11px; color:#48cae4; text-decoration:none;'>📥 Экспорт CSV в Excel</a>";
  html += "</div>";
  html += "<svg id='chartSvg' viewBox='0 0 500 140'></svg>";
  html += "<div class='chart-legend'>";
  html += "<span style='color:#e63946;'>● Воздух (°C)</span>";
  html += "<span style='color:#48cae4;'>● Вода (°C)</span>";
  html += "<span style='color:#457b9d;'>● Влажность (%)</span>";
  html += "<span style='color:#52b788;'>● VPD (x10)</span>";
  html += "<span style='color:#e9c46a;'>● Почва #1 (%)</span>";
  html += "</div></div>";

  // ⚖️ КАЛИБРОВКА ПОЧВЫ
  html += "<div class='box'>";
  html += "<h3 style='margin:0 0 6px; font-size:13px; color:#74c69d;'>⚖️ Калибровка датчиков почвы</h3>";
  for (int z = 0; z < 3; z++) {
    html += "<div class='row' style='border-bottom:1px solid #151d17; padding:4px 0;'>";
    html += "<span>Горшок #" + String(z+1) + " (АЦП: <b id='raw" + String(z+1) + "'>----</b>)</span>";
    html += "<div>";
    html += "<button class='btn btn-cal' onclick='fetch(\"/calib?z=" + String(z) + "&t=dry\")'>0% Сухо (<span id='dry" + String(z+1) + "'>--</span>)</button> ";
    html += "<button class='btn btn-cal' onclick='fetch(\"/calib?z=" + String(z) + "&t=wet\")'>100% Влажно (<span id='wet" + String(z+1) + "'>--</span>)</button>";
    html += "</div></div>";
  }
  html += "</div>";

  // ⚙️ НАСТРОЙКИ
  html += "<div class='box'>";
  html += "<h3 style='margin:0 0 8px; font-size:13px; color:#74c69d;'>⚙️ Настройки Telegram и Защиты</h3>";
  html += "<form action='/saveConfig' method='POST'>";
  html += "<label style='font-size:11px;'>Telegram Bot Token:</label><input type='text' name='tgToken' value='" + tgBotToken + "'>";
  html += "<label style='font-size:11px;'>Telegram Chat ID:</label><input type='text' name='tgChat' value='" + tgChatId + "'>";
  html += "<label style='font-size:11px;'>IP ESP32-CAM камеры:</label><input type='text' name='camIp' value='" + camIp + "'>";
  html += "<div class='btn-grid' style='grid-template-columns: 1fr 1fr;'>";
  html += "<button type='submit' class='btn'>💾 Сохранить</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/resetCycle\")'>📅 Сбросить день цикла</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/toggleSafety\")'>🛡️ Поплавок / протечка</button>";
  html += "<button type='button' class='btn btn-sec' onclick='fetch(\"/togglePower\")'>⚡ Датчик 220V</button>";
  html += "</div></form></div>";

  html += "<div class='footer'><a href='/update'>📦 Прошивка по воздуху (OTA /update)</a> | WDT Watchdog | v";
  html += FIRMWARE_VERSION;
  html += "</div>";
  html += "</div>";

  html += "<script>";
  html += "function drawCharts(data) {";
  html += "  let svg = document.getElementById('chartSvg');";
  html += "  if (!data || data.length < 2) { svg.innerHTML = '<text x=\"200\" y=\"70\" fill=\"#666\" font-size=\"11\">Сбор данных...</text>'; return; }";
  html += "  let w = 500, h = 140, p = 15;";
  html += "  let buildPath = (key, mult, minV, maxV, col) => {";
  html += "    let pts = data.map((d, i) => {";
  html += "      let x = p + (i / (data.length - 1)) * (w - 2 * p);";
  html += "      let v = (d[key] * mult);";
  html += "      let y = h - p - ((v - minV) / (maxV - minV)) * (h - 2 * p);";
  html += "      return x.toFixed(1) + ',' + y.toFixed(1);";
  html += "    }).join(' ');";
  html += "    return '<polyline fill=\"none\" stroke=\"' + col + '\" stroke-width=\"1.5\" points=\"' + pts + '\"/>';";
  html += "  };";
  html += "  let out = '<line x1=\"15\" y1=\"15\" x2=\"15\" y2=\"125\" stroke=\"#1a241c\"/><line x1=\"15\" y1=\"125\" x2=\"485\" y2=\"125\" stroke=\"#1a241c\"/>';";
  html += "  out += buildPath('t', 1, 10, 40, '#e63946');";
  html += "  out += buildPath('wt', 1, 10, 40, '#48cae4');";
  html += "  out += buildPath('h', 1, 20, 100, '#457b9d');";
  html += "  out += buildPath('v', 10, 0, 30, '#52b788');";
  html += "  out += buildPath('s1', 1, 0, 100, '#e9c46a');";
  html += "  svg.innerHTML = out;";
  html += "}";

  html += "function upd() {";
  html += "  fetch('/api/data').then(r => r.json()).then(d => {";
  html += "    document.getElementById('tVal').innerText = (typeof d.temp === 'number') ? (d.temp.toFixed(1) + ' °C') : '--.- °C';";
  html += "    document.getElementById('hVal').innerText = (typeof d.hum === 'number') ? (d.hum.toFixed(1) + ' %') : '--.- %';";
  html += "    document.getElementById('vpdVal').innerText = (typeof d.vpd === 'number') ? (d.vpd.toFixed(2) + ' kPa') : '--.- kPa';";
  html += "    document.getElementById('wtVal').innerText = (typeof d.waterTemp === 'number') ? (d.waterTemp.toFixed(1) + ' °C') : '--.- °C';";
  html += "    document.getElementById('stTxt').innerText = d.stage;";
  html += "    document.getElementById('dayTxt').innerText = 'День ' + d.day;";
  html += "    document.getElementById('tmTxt').innerText = d.time;";
  html += "    document.getElementById('camBtn').href = d.camIp;";
  
  html += "    document.getElementById('alertBanner').style.display = d.thermal ? 'block' : 'none';";
  html += "    document.getElementById('powerAlert').style.display = d.powerOk ? 'none' : 'block';";
  html += "    document.getElementById('waterAlert').style.display = (d.safetyEn && d.waterLow) ? 'block' : 'none';";
  html += "    document.getElementById('floodAlert').style.display = (d.safetyEn && d.flood) ? 'block' : 'none';";

  html += "    let vb = document.getElementById('vpdTag');";
  html += "    if (typeof d.vpd === 'number') {";
  html += "      if (d.vpd >= 0.8 && d.vpd <= 1.5) { vb.className='vpd-tag vpd-ok'; vb.innerText='VPD OK'; }";
  html += "      else if (d.vpd < 0.8) { vb.className='vpd-tag vpd-bad'; vb.innerText='РИСК ПЛЕСЕНИ'; }";
  html += "      else { vb.className='vpd-tag vpd-bad'; vb.innerText='СУХО'; }";
  html += "    } else { vb.className='vpd-tag vpd-ok'; vb.innerText='ТАЙМЕР'; }";

  html += "    document.getElementById('s1V').innerText = d.s1Ok ? (d.soil1 + '%') : 'ОТКЛЮЧЕН';";
  html += "    document.getElementById('s1B').style.width = (d.s1Ok ? d.soil1 : 0) + '%';";
  html += "    document.getElementById('s2V').innerText = d.s2Ok ? (d.soil2 + '%') : 'ОТКЛЮЧЕН';";
  html += "    document.getElementById('s2B').style.width = (d.s2Ok ? d.soil2 : 0) + '%';";
  html += "    document.getElementById('s3V').innerText = d.s3Ok ? (d.soil3 + '%') : 'ОТКЛЮЧЕН';";
  html += "    document.getElementById('s3B').style.width = (d.s3Ok ? d.soil3 : 0) + '%';";

  html += "    document.getElementById('raw1').innerText = d.raw1; document.getElementById('dry1').innerText = d.dry1; document.getElementById('wet1').innerText = d.wet1;";
  html += "    document.getElementById('raw2').innerText = d.raw2; document.getElementById('dry2').innerText = d.dry2; document.getElementById('wet2').innerText = d.wet2;";
  html += "    document.getElementById('raw3').innerText = d.raw3; document.getElementById('dry3').innerText = d.dry3; document.getElementById('wet3').innerText = d.wet3;";

  html += "    let setB = (id, st, onT, offT) => { let el = document.getElementById(id); el.className = 'badge ' + (st ? 'on' : 'off'); el.innerText = st ? onT : offT; };";
  html += "    setB('bLight', d.light, 'ВКЛ (' + d.lightPwm + '%)', 'НОЧЬ/СУШКА');";
  html += "    setB('bExh', d.exhaust, 'ВКЛ', 'ВЫКЛ');";
  html += "    setB('bHeat', d.heater, 'ГРЕЕТ', 'ВЫКЛ');";
  html += "    setB('bFan', d.fan, 'ВЕТЕР', 'ПАУЗА');";
  html += "    setB('bPump', d.pump, 'КАЧАЕТ', 'СТОП');";
  html += "    setB('bPower', d.powerOk, 'В СЕТИ', 'НЕТ СЕТИ');";

  html += "    document.getElementById('vStat').innerText = '[' + (d.v1?'1':'0') + ', ' + (d.v2?'1':'0') + ', ' + (d.v3?'1':'0') + ']';";
  html += "    document.getElementById('wProg').innerText = d.watering >= 0 ? ('⏳ Полив зоны #' + (d.watering+1) + '...') : '';";
  html += "  }).catch(e => console.error(e));";
  html += "}";

  html += "function loadHistory() { fetch('/api/history').then(r => r.json()).then(drawCharts).catch(e => console.error(e)); }";
  html += "setInterval(upd, 2000); upd();";
  html += "setInterval(loadHistory, 60000); loadHistory();";
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
  if (!enablePowerSense) {
    powerGridOk = true;
  }
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
    sendTelegramMessage(String("🌿 <b>GrowBox Enterprise Connected!</b>\nКонтроллер Critical Kush Enterprise v") + FIRMWARE_VERSION + " готов к работе. Напишите /help");
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  // 1. Watchdog (8 сек)
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

  // 2. Реле и ШИМ
  uint8_t relayPins[] = {RELAY_LIGHT, RELAY_EXHAUST, RELAY_HEATER, RELAY_FAN, 
                         RELAY_PUMP, RELAY_VALVE1, RELAY_VALVE2, RELAY_VALVE3};
  for (int i = 0; i < 8; i++) {
    digitalWrite(relayPins[i], RELAY_OFF);
    pinMode(relayPins[i], OUTPUT);
  }

  pinMode(PIN_LIGHT_PWM, OUTPUT);
  analogWrite(PIN_LIGHT_PWM, 0);

  // 3. Защитные датчики и контроль 220V
  pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  pinMode(FLOOD_SENSOR_PIN, INPUT_PULLUP);
  pinMode(POWER_SENSE_PIN, INPUT_PULLUP);

  // 4. Загрузка NVS Flash
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
  prefs.end();

  // 5. Сенсоры
  dht.begin();
  waterSensors.begin();
  waterSensors.setWaitForConversion(false);
  pinMode(SOIL1_PIN, INPUT);
  pinMode(SOIL2_PIN, INPUT);
  pinMode(SOIL3_PIN, INPUT);

  // 6. WiFi
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

  // 7. Сервер
  server.on("/", handleRoot);
  server.on("/api/data", handleApiData);
  server.on("/api/history", handleApiHistory);
  server.on("/export.csv", handleExportCsv);
  server.on("/water", handleWater);
  server.on("/setStage", handleSetStage);
  server.on("/toggleSafety", handleToggleSafety);
  server.on("/togglePower", handleTogglePower);
  server.on("/resetCycle", handleResetCycle);
  server.on("/calib", handleCalib);
  server.on("/saveConfig", HTTP_POST, handleSaveConfig);

  ElegantOTA.begin(&server);
  server.begin();

  Serial.print("[GrowBox Enterprise v");
  Serial.print(FIRMWARE_VERSION);
  Serial.println("] Запущен!");
}

// ================= LOOP =================
void loop() {
  feedWatchdog();
  server.handleClient();
  ElegantOTA.loop();
  ensureCycleStart();

  unsigned long currentMillis = millis();

  // 1. ОПРОС И АВТО-ДЕТЕКЦИЯ ДАТЧИКОВ (каждые 2.5 сек)
  if (currentMillis - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = currentMillis;

    // --- Опрос климата (DHT) ---
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > -30.0 && t < 70.0 && h >= 0.0 && h <= 100.0) {
      dhtConnected = true;
      temperature = t;
      humidity = h;
      vpd = calculateVPD(temperature, humidity);
    } else {
      dhtConnected = false; // Датчик отключен или сбоит -> переход на безопасную таймерную логику!
    }

    // --- Опрос температуры воды (DS18B20) ---
    float wt = waterSensors.getTempCByIndex(0);
    if (wt > -40.0 && wt < 65.0) {
      ds18Connected = true;
      waterTemperature = wt;
    } else {
      ds18Connected = false;
      waterTemperature = -127.0;
    }
    waterSensors.requestTemperatures();

    // --- Опрос емкостных датчиков почвы ---
    for (int i = 0; i < 3; i++) {
      int pin = (i == 0 ? SOIL1_PIN : (i == 1 ? SOIL2_PIN : SOIL3_PIN));
      int raw = analogRead(pin);
      soilRaw[i] = raw;
      // Если пин в воздухе или замкнут (>4050 или <50), датчик не подключен
      if (raw > 100 && raw < 4000) {
        soilConnected[i] = true;
        soilMoisture[i] = readSoilPercent(i, raw);
      } else {
        soilConnected[i] = false;
        soilMoisture[i] = 0;
      }
    }

    // --- Контроль сети 220V (только если датчик включён в настройках) ---
    if (enablePowerSense) {
      powerGridOk = (digitalRead(POWER_SENSE_PIN) == LOW); // LOW = оптопара открыта (220V есть)
      if (!powerGridOk && (currentMillis - lastPowerAlertTime > 1800000 || lastPowerAlertTime == 0)) {
        lastPowerAlertTime = currentMillis;
        sendTelegramMessage("🚨 <b>ВНИМАНИЕ:</b> Отключение сети 220V! Система работает от резервного аккумулятора.");
      }
    } else {
      powerGridOk = true;
    }

    // --- Защитные датчики ---
    if (enableSafetySensors) {
      isWaterLow = (digitalRead(WATER_LEVEL_PIN) == HIGH);
      isFloodDetected = (digitalRead(FLOOD_SENSOR_PIN) == LOW);
    } else {
      isWaterLow = false;
      isFloodDetected = false;
    }

    // ================= 🌿 ЛОГИКА УПРАВЛЕНИЯ ПО СТАДИЯМ =================
    struct tm timeinfo;
    bool isDay = false;
    int calculatedPwm = 0;

    if (getLocalTime(&timeinfo)) {
      int curHour = timeinfo.tm_hour;
      int curMin  = timeinfo.tm_min;
      int curSecOfDay = curHour * 3600 + curMin * 60 + timeinfo.tm_sec;

      // 1. РЕЖИМ СУШКИ И ПРОЛЕЧКИ (60/60)
      if (currentStage == STAGE_DRY) {
        isDay = false;
        stateLight = false;
        lightPwmDuty = 0;
        setRelay(RELAY_LIGHT, false);
        analogWrite(PIN_LIGHT_PWM, 0);

        // Мягкое импульсное микро-проветривание: 30 сек каждые 10 мин (для сохранения терпенов)
        if (dryVentState && (currentMillis - dryVentTimer >= 30000)) {
          dryVentState = false;
          dryVentTimer = currentMillis;
          stateExhaust = false;
        } else if (!dryVentState && (currentMillis - dryVentTimer >= 600000)) {
          dryVentState = true;
          dryVentTimer = currentMillis;
          stateExhaust = true;
        }
        setRelay(RELAY_EXHAUST, stateExhaust);

        // Обогрев сушки (поддержание ~16°C)
        if (dhtConnected) {
          if (temperature < (tempTargetDry - tempHysteresis)) stateHeater = true;
          else if (temperature >= tempTargetDry) stateHeater = false;
        } else {
          stateHeater = false;
        }
        setRelay(RELAY_HEATER, stateHeater);
      }
      // 2. РЕЖИМЫ ВЕГИ И ЦВЕТЕНИЯ
      else {
        int startSec = (currentStage == STAGE_VEG ? 6 : 8) * 3600;
        int endSec   = (currentStage == STAGE_VEG ? 24 : 20) * 3600;
        int rampSec  = SUNRISE_DURATION_MIN * 60;

        if (curSecOfDay >= startSec && curSecOfDay < endSec) {
          isDay = true;
          if (curSecOfDay < startSec + rampSec) {
            calculatedPwm = map(curSecOfDay - startSec, 0, rampSec, 10, 255);
          } else if (curSecOfDay >= endSec - rampSec) {
            calculatedPwm = map(endSec - curSecOfDay, 0, rampSec, 10, 255);
          } else {
            calculatedPwm = 255;
          }
        } else {
          isDay = false;
          calculatedPwm = 0;
        }

        // Термозащита света (>32.5°C)
        if (dhtConnected && temperature >= TEMP_EMERGENCY) {
          if (!thermalShutdown) {
            thermalShutdown = true;
            sendTelegramMessage("🔥 <b>ПЕРЕГРЕВ:</b> " + String(temperature, 1) + "°C! Свет отключен.");
          }
        } else if (dhtConnected && temperature < (TEMP_EMERGENCY - 2.0)) {
          thermalShutdown = false;
        }

        if (thermalShutdown) {
          stateLight = false;
          lightPwmDuty = 0;
        } else {
          stateLight = isDay;
          lightPwmDuty = calculatedPwm;
        }

        setRelay(RELAY_LIGHT, stateLight);
        analogWrite(PIN_LIGHT_PWM, lightPwmDuty);

        // Вытяжка 24/7
        stateExhaust = true;
        setRelay(RELAY_EXHAUST, stateExhaust);

        // Обогрев с авто-переключением день/ночь
        if (dhtConnected) {
          float currentTargetTemp = isDay ? tempTargetDay : tempTargetNight;
          if (temperature < (currentTargetTemp - tempHysteresis)) stateHeater = true;
          else if (temperature >= currentTargetTemp) stateHeater = false;
        } else {
          // Если датчик воздуха отключен — обогрев безопасен (выключен)
          stateHeater = false;
        }
        setRelay(RELAY_HEATER, stateHeater);

        // Предупреждение о риске плесени
        if (dhtConnected && currentStage == STAGE_BLOOM && vpd < 0.8 && (currentMillis - lastTgAlertTime > 14400000)) {
          lastTgAlertTime = currentMillis;
          sendTelegramMessage("⚠️ <b>Риск плесени:</b> VPD слишком низкий (" + String(vpd, 2) + " kPa). Влажность: " + String(humidity, 1) + "%");
        }
      }
    }
  }

  // 2. ИСТОРИЯ 24 ЧАСА (каждые 15 минут, первую точку пишем после опроса датчиков)
  if (lastSensorRead != 0 && (lastHistoryLog == 0 || currentMillis - lastHistoryLog >= HISTORY_INTERVAL)) {
    lastHistoryLog = currentMillis;
    logHistoryPoint();
  }

  // 3. ТЕЛЕГРАМ БОТ (каждые 3.5 сек)
  if (tgEnabled && (currentMillis - lastTgPoll >= TG_POLL_INTERVAL)) {
    lastTgPoll = currentMillis;
    checkTelegramUpdates();
  }

  // 4. ОБДУВ ВЕТЕР (15 МИН / 5 МИН)
  if (currentStage != STAGE_DRY) {
    if (windState && (currentMillis - windTimer >= WIND_ON)) {
      windState = false;
      windTimer = currentMillis;
      stateFan = false;
      setRelay(RELAY_FAN, false);
    } else if (!windState && (currentMillis - windTimer >= WIND_OFF)) {
      windState = true;
      windTimer = currentMillis;
      stateFan = true;
      setRelay(RELAY_FAN, true);
    }
  } else {
    // На сушке прямой обдув выключен (чтобы не пересушить внешнюю корку)
    stateFan = false;
    setRelay(RELAY_FAN, false);
  }

  // 5. УМНЫЙ АВТОПОЛИВ (С АВТО-ОПРЕДЕЛЕНИЕМ ДАТЧИКОВ)
  if (currentStage != STAGE_DRY) {
    if (activeWateringZone == -1) {
      if (!enableSafetySensors || (!isWaterLow && !isFloodDetected)) {
        for (int i = 0; i < 3; i++) {
          // Если датчик подключен -> полив по порогу влажности Dry-Back
          if (soilConnected[i]) {
            if (soilMoisture[i] < soilDryThreshold && soakDelayPassed(i)) {
              triggerWatering(i);
              break;
            }
          }
          // Если датчик отключен -> РЕЗЕРВНЫЙ ТАЙМЕРНЫЙ ПОЛИВ (1 раз в сутки)
          else {
            if (fallbackWaterDue(i)) {
              triggerWatering(i);
              break;
            }
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
        sendTelegramMessage("🚨 <b>АВАРИЯ:</b> Полив остановлен из-за датчика протечки!");
      } else if (currentMillis - wateringStartTime >= WATERING_DURATION) {
        statePump = false;
        setRelay(RELAY_PUMP, false);

        stateValves[activeWateringZone] = false;
        setRelay(activeWateringZone == 0 ? RELAY_VALVE1 : (activeWateringZone == 1 ? RELAY_VALVE2 : RELAY_VALVE3), false);

        lastWateredTime[activeWateringZone] = currentMillis;
        activeWateringZone = -1;
      }
    }
  }
}
