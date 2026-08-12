// Home Assistant через MQTT Discovery (LAN Mosquitto).
#define MQTT_MAX_PACKET_SIZE 2048
#include <PubSubClient.h>

// mqttHost / mqttPort / mqttUser / mqttPass / mqttEnabled / lastMqttEvent / mqttConnected
// объявлены в основном скетче.

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
unsigned long lastMqttReconnect = 0;
unsigned long lastMqttPub = 0;
bool mqttDiscoverySent = false;

static String mqttId() {
  return deviceId();
}

static String mqttBase() {
  return "growbox/" + mqttId();
}

void persistMqtt() {
  prefs.begin("growbox", false);
  prefs.putString("mqttH", mqttHost);
  prefs.putUShort("mqttP", mqttPort);
  prefs.putString("mqttU", mqttUser);
  prefs.putString("mqttPw", mqttPass);
  prefs.putBool("mqttEn", mqttEnabled);
  prefs.end();
}

void loadMqttSettings() {
  mqttHost = prefs.getString("mqttH", mqttHost);
  mqttPort = prefs.getUShort("mqttP", mqttPort);
  mqttUser = prefs.getString("mqttU", mqttUser);
  mqttPass = prefs.getString("mqttPw", mqttPass);
  mqttEnabled = prefs.getBool("mqttEn", mqttEnabled);
}

static void mqttPublish(const String& topic, const String& payload, bool retain = false) {
  mqtt.publish(topic.c_str(), payload.c_str(), retain);
}

static String haDeviceJson() {
  String d = "{\"identifiers\":[\"growbox_";
  d += mqttId();
  d += "\"],\"name\":\"GrowBox\",\"manufacturer\":\"GrowBox\",\"model\":\"Critical Kush\",\"sw_version\":\"";
  d += FIRMWARE_VERSION;
  d += "\",\"connections\":[[\"mac\",\"";
  d += mqttId();
  d += "\"]]}";
  return d;
}

static void haConfig(const char* type, const char* obj, const String& fields) {
  feedWatchdog();
  String topic = String("homeassistant/") + type + "/growbox_" + mqttId() + "/" + obj + "/config";
  String uniq = String("gb_") + mqttId() + "_" + obj;
  String body = "{";
  body += fields;
  body += ",\"uniq_id\":\"" + uniq + "\"";
  body += ",\"avty_t\":\"" + mqttBase() + "/status\"";
  body += ",\"dev\":" + haDeviceJson();
  body += "}";
  mqttPublish(topic, body, true);
}

void mqttPublishDiscovery() {
  String st = mqttBase() + "/state";
  String cmd = mqttBase() + "/cmd";

  haConfig("sensor", "temp", "\"name\":\"Температура\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.temp }}\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\"");
  haConfig("sensor", "hum", "\"name\":\"Влажность\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.hum }}\",\"unit_of_meas\":\"%\",\"dev_cla\":\"humidity\",\"stat_cla\":\"measurement\"");
  haConfig("sensor", "vpd", "\"name\":\"VPD\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.vpd }}\",\"unit_of_meas\":\"kPa\",\"stat_cla\":\"measurement\"");
  haConfig("sensor", "water_temp", "\"name\":\"Вода в баке\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.waterTemp }}\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"");
  haConfig("sensor", "soil1", "\"name\":\"Почва 1\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.soil1 }}\",\"unit_of_meas\":\"%\"");
  haConfig("sensor", "soil2", "\"name\":\"Почва 2\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.soil2 }}\",\"unit_of_meas\":\"%\"");
  haConfig("sensor", "soil3", "\"name\":\"Почва 3\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.soil3 }}\",\"unit_of_meas\":\"%\"");
  haConfig("sensor", "day", "\"name\":\"День цикла\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.day }}\"");
  haConfig("sensor", "stage", "\"name\":\"Стадия\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stage }}\"");
  haConfig("sensor", "fw", "\"name\":\"Прошивка\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.fw }}\"");

  haConfig("binary_sensor", "light", "\"name\":\"Свет\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.light }}\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"light\"");
  haConfig("binary_sensor", "exhaust", "\"name\":\"Вытяжка\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.exhaust }}\",\"pl_on\":\"1\",\"pl_off\":\"0\"");
  haConfig("binary_sensor", "heater", "\"name\":\"Обогрев\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.heater }}\",\"pl_on\":\"1\",\"pl_off\":\"0\"");
  haConfig("binary_sensor", "fan", "\"name\":\"Обдув\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.fan }}\",\"pl_on\":\"1\",\"pl_off\":\"0\"");
  haConfig("binary_sensor", "humid", "\"name\":\"Увлажнитель\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.humid }}\",\"pl_on\":\"1\",\"pl_off\":\"0\"");
  haConfig("binary_sensor", "pump", "\"name\":\"Помпа\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.pump }}\",\"pl_on\":\"1\",\"pl_off\":\"0\"");
  haConfig("binary_sensor", "thermal", "\"name\":\"Перегрев\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.thermal }}\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"problem\"");
  haConfig("binary_sensor", "flood", "\"name\":\"Протечка\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.flood }}\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"moisture\"");

  haConfig("select", "light_mode", "\"name\":\"Свет режим\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.mLight }}\",\"cmd_t\":\"" + cmd + "/light\",\"ops\":[\"auto\",\"on\",\"off\"]");
  haConfig("select", "exhaust_mode", "\"name\":\"Вытяжка режим\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.mExh }}\",\"cmd_t\":\"" + cmd + "/exhaust\",\"ops\":[\"auto\",\"on\",\"off\"]");
  haConfig("select", "heater_mode", "\"name\":\"Обогрев режим\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.mHeat }}\",\"cmd_t\":\"" + cmd + "/heater\",\"ops\":[\"auto\",\"on\",\"off\"]");
  haConfig("select", "fan_mode", "\"name\":\"Обдув режим\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.mFan }}\",\"cmd_t\":\"" + cmd + "/fan\",\"ops\":[\"auto\",\"on\",\"off\"]");
  haConfig("select", "humid_mode", "\"name\":\"Увлажнитель режим\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.mHumid }}\",\"cmd_t\":\"" + cmd + "/humid\",\"ops\":[\"auto\",\"on\",\"off\"]");
  haConfig("select", "stage_set", "\"name\":\"Стадия\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stageKey }}\",\"cmd_t\":\"" + cmd + "/stage\",\"ops\":[\"veg\",\"bloom\",\"dry\"]");

  haConfig("button", "water1", "\"name\":\"Полив 1\",\"cmd_t\":\"" + cmd + "/water\",\"pl_prs\":\"0\"");
  haConfig("button", "water2", "\"name\":\"Полив 2\",\"cmd_t\":\"" + cmd + "/water\",\"pl_prs\":\"1\"");
  haConfig("button", "water3", "\"name\":\"Полив 3\",\"cmd_t\":\"" + cmd + "/water\",\"pl_prs\":\"2\"");
  haConfig("button", "all_auto", "\"name\":\"Все в авто\",\"cmd_t\":\"" + cmd + "/auto\",\"pl_prs\":\"1\"");

  mqttDiscoverySent = true;
  lastMqttEvent = "discovery";
}

static const char* modeStr(OverrideMode m) {
  if (m == MODE_ON) return "on";
  if (m == MODE_OFF) return "off";
  return "auto";
}

void mqttPublishState() {
  if (!mqtt.connected()) return;
  String stg = "veg";
  if (currentStage == STAGE_BLOOM) stg = "bloom";
  else if (currentStage == STAGE_DRY) stg = "dry";
  String json = "{";
  json += "\"temp\":" + String(dhtConnected ? temperature : NAN, 1) + ",";
  json += "\"hum\":" + String(dhtConnected ? humidity : NAN, 1) + ",";
  json += "\"vpd\":" + String(dhtConnected ? vpd : NAN, 2) + ",";
  json += "\"waterTemp\":" + String(ds18Connected ? waterTemperature : NAN, 1) + ",";
  json += "\"soil1\":" + String(soilMoisture[0]) + ",";
  json += "\"soil2\":" + String(soilMoisture[1]) + ",";
  json += "\"soil3\":" + String(soilMoisture[2]) + ",";
  json += "\"day\":" + String(getGrowDay()) + ",";
  json += "\"stage\":\"" + String(currentStage == STAGE_BLOOM ? "Цветение" : (currentStage == STAGE_DRY ? "Сушка" : "Вегетация")) + "\",";
  json += "\"stageKey\":\"" + stg + "\",";
  json += "\"fw\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"light\":" + String(stateLight ? 1 : 0) + ",";
  json += "\"exhaust\":" + String(stateExhaust ? 1 : 0) + ",";
  json += "\"heater\":" + String(stateHeater ? 1 : 0) + ",";
  json += "\"fan\":" + String(stateFan ? 1 : 0) + ",";
  json += "\"humid\":" + String(stateHumid ? 1 : 0) + ",";
  json += "\"pump\":" + String(statePump ? 1 : 0) + ",";
  json += "\"thermal\":" + String(thermalShutdown ? 1 : 0) + ",";
  json += "\"flood\":" + String(isFloodDetected ? 1 : 0) + ",";
  json += "\"mLight\":\"" + String(modeStr(modeLight)) + "\",";
  json += "\"mExh\":\"" + String(modeStr(modeExhaust)) + "\",";
  json += "\"mHeat\":\"" + String(modeStr(modeHeater)) + "\",";
  json += "\"mFan\":\"" + String(modeStr(modeFan)) + "\",";
  json += "\"mHumid\":\"" + String(modeStr(modeHumid)) + "\"";
  json += "}";
  mqttPublish(mqttBase() + "/state", json, false);
}

static void mqttOnMessage(char* topic, byte* payload, unsigned int len) {
  String t = topic;
  String msg;
  msg.reserve(len + 1);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  String prefix = mqttBase() + "/cmd/";
  if (!t.startsWith(prefix)) return;
  String what = t.substring(prefix.length());
  lastMqttEvent = "cmd:" + what;
  if (what == "water") triggerWatering(msg.toInt());
  else if (what == "auto") allAuto();
  else if (what == "stage") {
    if (msg == "veg") setGrowStage(STAGE_VEG);
    else if (msg == "bloom") setGrowStage(STAGE_BLOOM);
    else if (msg == "dry") setGrowStage(STAGE_DRY);
  } else {
    setDeviceMode(what, parseModeArg(msg));
  }
  mqttPublishState();
}

static bool mqttConnectNow() {
  if (mqttHost.length() < 3) {
    lastMqttEvent = "no-host";
    return false;
  }
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(mqttOnMessage);
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(30);
  String clientId = "growbox-" + mqttId();
  String status = mqttBase() + "/status";
  bool ok;
  if (mqttUser.length()) {
    ok = mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str(), status.c_str(), 1, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), status.c_str(), 1, true, "offline");
  }
  if (!ok) {
    lastMqttEvent = String("fail:") + mqtt.state();
    mqttConnected = false;
    return false;
  }
  mqtt.publish(status.c_str(), "online", true);
  String cmd = mqttBase() + "/cmd/#";
  mqtt.subscribe(cmd.c_str());
  mqttConnected = true;
  lastMqttEvent = "connected";
  mqttDiscoverySent = false;
  return true;
}

void mqttLoop() {
  if (!mqttEnabled || WiFi.status() != WL_CONNECTED) {
    mqttConnected = mqtt.connected();
    return;
  }
  if (!mqtt.connected()) {
    mqttConnected = false;
    unsigned long now = millis();
    if (now - lastMqttReconnect < 8000 && lastMqttReconnect != 0) return;
    lastMqttReconnect = now;
    feedWatchdog();
    mqttConnectNow();
    return;
  }
  mqttConnected = true;
  mqtt.loop();
  if (!mqttDiscoverySent) mqttPublishDiscovery();
  unsigned long now = millis();
  if (now - lastMqttPub >= 5000 || lastMqttPub == 0) {
    lastMqttPub = now;
    mqttPublishState();
  }
}

void handleSaveMqtt() {
  if (server.hasArg("mqttH")) mqttHost = server.arg("mqttH");
  if (server.hasArg("mqttP")) mqttPort = (uint16_t)constrain(server.arg("mqttP").toInt(), 1, 65535);
  if (server.hasArg("mqttU")) mqttUser = server.arg("mqttU");
  if (server.hasArg("mqttPw")) mqttPass = server.arg("mqttPw");
  mqttEnabled = server.hasArg("mqttEn");
  persistMqtt();
  mqtt.disconnect();
  mqttDiscoverySent = false;
  lastMqttReconnect = 0;
  server.sendHeader("Location", "/");
  server.send(303);
}
