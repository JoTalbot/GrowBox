// Админка LAN: http://growbox.local/admin

void handleReboot() {
  server.send(200, "text/plain", "REBOOT");
  delay(300);
  ESP.restart();
}

void handleAdmin() {
  feedWatchdog();
  String st = "veg";
  if (currentStage == STAGE_BLOOM) st = "bloom";
  else if (currentStage == STAGE_DRY) st = "dry";

  String html = F("<!DOCTYPE html><html lang='ru'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>GrowBox Admin</title><style>"
    "body{font-family:sans-serif;background:#070a08;color:#d8f3dc;max-width:560px;margin:16px auto;padding:0 12px}"
    "h1{color:#52b788;font-size:20px}a{color:#74c69d}"
    ".c{background:#121a14;border:1px solid #1d2a20;border-radius:10px;padding:12px;margin:0 0 10px}"
    ".r{display:flex;justify-content:space-between;margin:5px 0;font-size:14px}"
    "b.warn{color:#ffe066}b.bad{color:#e76f51}b.ok{color:#95d5b2}"
    "button,.b{display:inline-block;background:#2d6a4f;color:#fff;border:0;padding:8px 10px;border-radius:6px;margin:4px 4px 0 0;font-weight:700;text-decoration:none;font-size:13px}"
    "button.sec{background:#18201a;border:1px solid #253328}"
    "</style></head><body><h1>🛠️ GrowBox Admin</h1>");

  html += "<div class='c'>";
  html += "<div class='r'><span>Прошивка</span><b>v";
  html += FIRMWARE_VERSION;
  html += "</b></div>";
  html += "<div class='r'><span>Device ID</span><b>";
  html += deviceId();
  html += "</b></div>";
  html += "<div class='r'><span>IP</span><b>";
  html += WiFi.localIP().toString();
  html += "</b></div>";
  html += "<div class='r'><span>RSSI</span><b>";
  html += String(WiFi.RSSI());
  html += " dBm</b></div>";
  html += "<div class='r'><span>Стадия / день</span><b>";
  html += st;
  html += " / ";
  html += String(getGrowDay());
  html += "</b></div>";
  html += "<div class='r'><span>NTP</span><b class='";
  html += ntpReady() ? "ok" : "bad";
  html += "'>";
  html += ntpReady() ? "OK" : "нет";
  html += "</b></div>";
  html += "<div class='r'><span>Free heap</span><b>";
  html += String(ESP.getFreeHeap());
  html += "</b></div></div>";

  html += "<div class='c'><div class='r'><span>DHT22</span><b class='";
  html += dhtConnected ? "ok" : "bad";
  html += "'>";
  html += dhtConnected ? (String(temperature, 1) + " °C / " + String(humidity, 0) + "%") : "НЕТ";
  html += "</b></div>";
  html += "<div class='r'><span>DS18 вода</span><b class='";
  html += ds18Connected ? "ok" : "bad";
  html += "'>";
  html += ds18Connected ? (String(waterTemperature, 1) + " °C") : "НЕТ";
  html += "</b></div>";
  for (int i = 0; i < 3; i++) {
    html += "<div class='r'><span>Почва ";
    html += String(i + 1);
    html += " raw</span><b>";
    html += soilConnected[i] ? (String(soilMoisture[i]) + "% / ADC " + String(soilRaw[i])) : ("ОТКЛ ADC " + String(soilRaw[i]));
    html += "</b></div>";
  }
  html += "</div>";

  html += "<div class='c'>";
  html += "<div class='r'><span>Свет</span><b>";
  html += String(stateLight ? "ВКЛ" : "ВЫКЛ");
  html += " / ";
  html += modeShort(modeLight);
  html += "</b></div>";
  html += "<div class='r'><span>Вытяжка</span><b>";
  html += String(stateExhaust ? "ВКЛ" : "ВЫКЛ");
  html += " / ";
  html += modeShort(modeExhaust);
  html += "</b></div>";
  html += "<div class='r'><span>Обогрев</span><b>";
  html += String(stateHeater ? "ВКЛ" : "ВЫКЛ");
  html += " / ";
  html += modeShort(modeHeater);
  html += "</b></div>";
  html += "<div class='r'><span>Обдув</span><b>";
  html += String(stateFan ? "ВКЛ" : "ВЫКЛ");
  html += " / ";
  html += modeShort(modeFan);
  html += "</b></div></div>";

  html += "<div class='c'><b>Канал ntfy</b>";
  html += "<div class='r'><span>Опрос</span><b class='";
  html += remoteEnabled ? "ok" : "bad";
  html += "'>";
  html += remoteEnabled ? "ВКЛ" : "ВЫКЛ";
  html += "</b></div>";
  html += "<div class='r'><span>Событие</span><b>";
  html += lastRemoteEvent;
  html += "</b></div>";
  html += "<div class='r'><span>OTA</span><b>";
  html += lastOtaResult;
  html += "</b></div>";
  html += "<div class='r'><span>Автопрошивка</span><b>";
  html += autoOta ? "да" : "нет";
  html += "</b></div>";
  html += "<div class='r'><span>MQTT</span><b>";
  html += mqttEnabled ? (mqttConnected ? "ONLINE" : "нет связи") : "выкл";
  html += " ";
  html += lastMqttEvent;
  html += "</b></div>";
  html += "<button onclick=\"fetch('/remotePull').then(r=>r.text()).then(t=>alert(t))\">Проверить ntfy</button>";
  html += "<button onclick=\"fetch('/otaCheck').then(r=>r.text()).then(t=>alert(t))\">Проверить version.json</button>";
  html += "</div>";

  html += "<div class='c'>";
  html += "<a class='b' href='/'>Дашборд</a>";
  html += "<a class='b' href='/update'>OTA файл</a>";
  html += "<a class='b' href='/export.csv'>CSV</a>";
  html += "<button class='sec' onclick=\"fetch('/allAuto')\">Все в авто</button>";
  html += "<button class='sec' onclick=\"if(confirm('Ребут?'))fetch('/reboot')\">Ребут</button>";
  html += "</div>";
  html += "<p style='font-size:12px;color:#8aa392'>Только LAN. С улицы — пульт ntfy.</p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}
