// Удалённый агент: исходящий HTTPS через ntfy + optional inbox.json + HTTP OTA.
// ESP32 за NAT сам открывает соединение наружу.

String deviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[18];
  snprintf(buf, sizeof(buf), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(buf);
}

void persistRemote() {
  prefs.begin("growbox", false);
  prefs.putString("ntfy", ntfyTopic);
  prefs.putString("rkey", remoteKey);
  prefs.putString("verUrl", versionUrl);
  prefs.putString("inboxUrl", inboxUrl);
  prefs.putBool("remEn", remoteEnabled);
  prefs.putBool("autoOta", autoOta);
  prefs.putLong("inboxId", lastInboxId);
  prefs.end();
}

void loadRemoteSettings() {
  ntfyTopic = prefs.getString("ntfy", ntfyTopic);
  remoteKey = prefs.getString("rkey", remoteKey);
  String vu = prefs.getString("verUrl", "");
  if (vu.length() > 8) versionUrl = vu;
  inboxUrl = prefs.getString("inboxUrl", inboxUrl);
  remoteEnabled = prefs.getBool("remEn", remoteEnabled);
  autoOta = prefs.getBool("autoOta", autoOta);
  lastInboxId = prefs.getLong("inboxId", lastInboxId);
}

String jsonGet(const String& src, const char* key) {
  String pat = String("\"") + key + "\":";
  int i = src.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '\t')) i++;
  if (i < (int)src.length() && src[i] == '"') {
    int j = i + 1;
    String out;
    while (j < (int)src.length()) {
      char c = src[j];
      if (c == '\\' && j + 1 < (int)src.length()) {
        out += src[j + 1];
        j += 2;
        continue;
      }
      if (c == '"') break;
      out += c;
      j++;
    }
    return out;
  }
  int j = i;
  while (j < (int)src.length() && src[j] != ',' && src[j] != '}' && src[j] != ' ' && src[j] != '\n') j++;
  return src.substring(i, j);
}

int versionCode(const String& v) {
  int maj = 0, minor = 0;
  sscanf(v.c_str(), "%d.%d", &maj, &minor);
  return maj * 100 + minor;
}

String httpsGet(const String& url, uint16_t timeoutMs = 4500) {
  if (WiFi.status() != WL_CONNECTED) return "";
  feedWatchdog();
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(timeoutMs / 1000);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  http.setUserAgent("GrowBox/" FIRMWARE_VERSION);
  if (!http.begin(client, url)) return "";
  int code = http.GET();
  String body = "";
  if (code >= 200 && code < 300) body = http.getString();
  http.end();
  feedWatchdog();
  return body;
}

bool httpsPost(const String& url, const String& body, const char* title) {
  if (WiFi.status() != WL_CONNECTED) return false;
  feedWatchdog();
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4000);
  http.setUserAgent("GrowBox/" FIRMWARE_VERSION);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  if (title && title[0]) http.addHeader("Title", title);
  http.addHeader("Tags", "seedling");
  int code = http.POST(body);
  http.end();
  feedWatchdog();
  return code >= 200 && code < 300;
}

String statusJson(const String& event) {
  String st = "veg";
  if (currentStage == STAGE_BLOOM) st = "bloom";
  else if (currentStage == STAGE_DRY) st = "dry";
  String json = "{";
  json += "\"event\":\"" + event + "\",";
  json += "\"fw\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"id\":\"" + deviceId() + "\",";
  json += "\"stage\":\"" + st + "\",";
  json += "\"day\":" + String(getGrowDay()) + ",";
  json += "\"temp\":" + String(dhtConnected ? temperature : -127, 1) + ",";
  json += "\"hum\":" + String(dhtConnected ? humidity : -1, 1) + ",";
  json += "\"vpd\":" + String(dhtConnected ? vpd : -1, 2) + ",";
  json += "\"light\":" + String(stateLight ? 1 : 0) + ",";
  json += "\"exhaust\":" + String(stateExhaust ? 1 : 0) + ",";
  json += "\"heater\":" + String(stateHeater ? 1 : 0) + ",";
  json += "\"fan\":" + String(stateFan ? 1 : 0) + ",";
  json += "\"humid\":" + String(stateHumid ? 1 : 0) + ",";
  json += "\"thermal\":" + String(thermalShutdown ? 1 : 0) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  return json;
}

void publishRemoteStatus(const String& event) {
  lastRemoteEvent = event;
  if (ntfyTopic.length() < 5) return;
  httpsPost("https://ntfy.sh/" + ntfyTopic, statusJson(event), "status");
}

void requestOta(const String& url) {
  String u = url;
  u.trim();
  if (!u.startsWith("http://") && !u.startsWith("https://")) {
    lastOtaResult = "bad url";
    lastRemoteEvent = "ota-bad-url";
    return;
  }
  otaPendingUrl = u;
  otaPending = true;
  lastRemoteEvent = "ota-queued";
}

void performPendingOta() {
  if (!otaPending) return;
  otaPending = false;
  String url = otaPendingUrl;
  lastOtaResult = "flashing";
  lastRemoteEvent = "ota-start";
  publishRemoteStatus("ota-start");
  sendTelegramMessage("📦 <b>OTA:</b> качаю " + url);

  feedWatchdog();
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.onProgress([](int cur, int total) {
    feedWatchdog();
  });

  t_httpUpdate_return ret = httpUpdate.update(client, url);
  if (ret == HTTP_UPDATE_OK) {
    lastOtaResult = "ok";
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    lastOtaResult = "no-update";
    lastRemoteEvent = "ota-none";
    sendTelegramMessage("📦 OTA: обновлений нет");
  } else {
    lastOtaResult = httpUpdate.getLastErrorString();
    lastRemoteEvent = "ota-fail";
    sendTelegramMessage("❌ OTA: " + lastOtaResult);
    publishRemoteStatus("ota-fail");
  }
}

void checkVersionFile(bool flashIfNewer) {
  if (versionUrl.length() < 12) {
    lastRemoteEvent = "no-version-url";
    return;
  }
  String body = httpsGet(versionUrl);
  if (body.length() < 5) {
    lastRemoteEvent = "version-fetch-fail";
    sendTelegramMessage("📦 Не удалось скачать version.json");
    return;
  }
  String ver = jsonGet(body, "version");
  String url = jsonGet(body, "url");
  lastRemoteEvent = "version=" + ver;
  if (ver.length() == 0) {
    sendTelegramMessage("📦 version.json без поля version");
    return;
  }
  bool newer = versionCode(ver) > versionCode(FIRMWARE_VERSION);
  String msg = "📦 Сейчас v" + String(FIRMWARE_VERSION) + ", в канале v" + ver;
  msg += newer ? " (новее)" : " (не новее)";
  sendTelegramMessage(msg);
  publishRemoteStatus(newer ? "update-available" : "up-to-date");
  if (newer && url.length() > 8 && (flashIfNewer || autoOta)) {
    requestOta(url);
  }
}

void executeRemoteCommand(const String& cmd, const String& arg, const String& key) {
  if (remoteKey.length() > 0 && key != remoteKey) {
    lastRemoteEvent = "bad-key";
    return;
  }
  lastRemoteEvent = "cmd:" + cmd;
  if (cmd == "ping" || cmd == "status") {
    publishRemoteStatus(cmd == "ping" ? "pong" : "status");
    sendTelegramMessage("📡 агент: " + cmd + " ok, v" + String(FIRMWARE_VERSION));
  } else if (cmd == "ota") {
    requestOta(arg);
  } else if (cmd == "reboot") {
    publishRemoteStatus("reboot");
    sendTelegramMessage("🔁 Ребут по команде агента");
    delay(400);
    ESP.restart();
  } else if (cmd == "stage") {
    if (arg == "veg") setGrowStage(STAGE_VEG);
    else if (arg == "bloom") setGrowStage(STAGE_BLOOM);
    else if (arg == "dry") setGrowStage(STAGE_DRY);
    publishRemoteStatus("stage");
  } else if (cmd == "water") {
    triggerWatering(arg.toInt());
  } else if (cmd == "auto") {
    allAuto();
    publishRemoteStatus("auto");
  } else if (cmd == "mode") {
    int c = arg.indexOf(':');
    if (c > 0) {
      setDeviceMode(arg.substring(0, c), parseModeArg(arg.substring(c + 1)));
      publishRemoteStatus("mode");
    }
  } else if (cmd == "checkota") {
    checkVersionFile(false);
  } else if (cmd == "flash") {
    checkVersionFile(true);
  } else {
    lastRemoteEvent = "unknown-cmd";
  }
}

void handlePlainCommand(const String& line) {
  String s = line;
  s.trim();
  if (s.length() == 0) return;
  // формат: cmd [arg...] [key]
  int first = s.indexOf(' ');
  String cmd, rest;
  if (first < 0) {
    cmd = s;
  } else {
    cmd = s.substring(0, first);
    rest = s.substring(first + 1);
    rest.trim();
  }
  cmd.toLowerCase();
  String key = "";
  String arg = rest;
  if (remoteKey.length() > 0) {
    int sp = rest.lastIndexOf(' ');
    if (sp >= 0) {
      key = rest.substring(sp + 1);
      key.trim();
      arg = rest.substring(0, sp);
      arg.trim();
    } else {
      key = rest;
      arg = "";
    }
  }
  executeRemoteCommand(cmd, arg, key);
}

void pollNtfy() {
  if (ntfyTopic.length() < 5) return;
  String url = "https://ntfy.sh/" + ntfyTopic + "/json?poll=1&since=" + lastNtfySince;
  String body = httpsGet(url, 5000);
  if (body.length() == 0) return;

  int start = 0;
  while (start < (int)body.length()) {
    int nl = body.indexOf('\n', start);
    String line = (nl < 0) ? body.substring(start) : body.substring(start, nl);
    start = (nl < 0) ? body.length() : nl + 1;
    line.trim();
    if (line.length() < 8) continue;
    String ev = jsonGet(line, "event");
    if (ev.length() && ev != "message") continue;
    String title = jsonGet(line, "title");
    if (title == "status") continue;
    String msgId = jsonGet(line, "id");
    if (msgId.length() > 0) lastNtfySince = msgId;
    String msg = jsonGet(line, "message");
    if (msg.startsWith("{")) {
      executeRemoteCommand(jsonGet(msg, "cmd"), jsonGet(msg, "arg"), jsonGet(msg, "key"));
    } else {
      handlePlainCommand(msg);
    }
  }
}

void pollInbox() {
  if (inboxUrl.length() < 12) return;
  String body = httpsGet(inboxUrl);
  if (body.length() < 5) return;
  long id = jsonGet(body, "id").toInt();
  if (id <= lastInboxId) return;
  lastInboxId = id;
  persistRemote();
  executeRemoteCommand(jsonGet(body, "cmd"), jsonGet(body, "arg"), jsonGet(body, "key"));
}

void pollRemoteAgent() {
  if (!remoteEnabled || WiFi.status() != WL_CONNECTED) return;
  pollNtfy();
  pollInbox();
}

void handleSaveRemote() {
  if (server.hasArg("ntfy")) ntfyTopic = server.arg("ntfy");
  if (server.hasArg("rkey")) remoteKey = server.arg("rkey");
  if (server.hasArg("verUrl")) versionUrl = server.arg("verUrl");
  if (server.hasArg("inboxUrl")) inboxUrl = server.arg("inboxUrl");
  remoteEnabled = server.hasArg("remEn");
  autoOta = server.hasArg("autoOta");
  persistRemote();
  if (remoteEnabled) publishRemoteStatus("online");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRemotePull() {
  lastRemotePoll = 0;
  pollRemoteAgent();
  server.send(200, "text/plain", lastRemoteEvent);
}

void handleOtaCheck() {
  checkVersionFile(false);
  server.send(200, "text/plain", lastRemoteEvent);
}
