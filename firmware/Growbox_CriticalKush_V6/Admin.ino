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
    "b.bad{color:#e76f51}b.ok{color:#95d5b2}"
    "button,.b{display:inline-block;background:#2d6a4f;color:#fff;border:0;padding:8px 10px;border-radius:6px;margin:4px 4px 0 0;font-weight:700;text-decoration:none;font-size:13px}"
    "button.sec{background:#18201a;border:1px solid #253328}"
    "label{display:block;font-size:11px;color:#9aaf9e;margin:6px 0 2px}"
    "input{width:100%;background:#151d17;border:1px solid #253328;color:#fff;padding:6px;border-radius:4px;box-sizing:border-box}"
    ".g{display:grid;grid-template-columns:1fr 1fr;gap:6px 10px}"
    "</style></head><body><h1>🛠️ GrowBox Admin</h1>");

  html += "<div class='c'>";
  html += "<div class='r'><span>Прошивка</span><b>v" + String(FIRMWARE_VERSION) + "</b></div>";
  html += "<div class='r'><span>Device ID</span><b>" + deviceId() + "</b></div>";
  html += "<div class='r'><span>IP</span><b>" + WiFi.localIP().toString() + "</b></div>";
  html += "<div class='r'><span>Стадия / день</span><b>" + st + " / " + String(getGrowDay()) + "</b></div>";
  html += "<div class='r'><span>NTP</span><b class='" + String(ntpReady() ? "ok" : "bad") + "'>" + String(ntpReady() ? "OK" : "нет") + "</b></div></div>";

  html += "<div class='c'>";
  html += "<div class='r'><span>DHT22</span><b class='" + String(dhtConnected ? "ok" : "bad") + "'>";
  html += dhtConnected ? (String(temperature, 1) + " °C / " + String(humidity, 0) + "%") : "НЕТ";
  html += "</b></div>";
  html += "<div class='r'><span>DS18 вода</span><b class='" + String(ds18Connected ? "ok" : "bad") + "'>";
  html += ds18Connected ? (String(waterTemperature, 1) + " °C") : "НЕТ";
  html += "</b></div>";
  for (int i = 0; i < 3; i++) {
    html += "<div class='r'><span>Почва " + String(i + 1) + "</span><b>";
    html += soilConnected[i] ? (String(soilMoisture[i]) + "% / ADC " + String(soilRaw[i])) : ("ОТКЛ ADC " + String(soilRaw[i]));
    html += "</b></div>";
  }
  html += "</div>";

  html += "<div class='c'><b>Климат и свет</b><form action='/saveSettings' method='POST'>";
  html += "<input type='hidden' name='next' value='admin'><input type='hidden' name='clim' value='1'><div class='g'>";
  html += "<label>Вега старт, ч<input type='number' name='vegH0' min='0' max='23' value='" + String(vegStartHour) + "'></label>";
  html += "<label>Вега стоп, ч<input type='number' name='vegH1' min='1' max='24' value='" + String(vegEndHour) + "'></label>";
  html += "<label>Цвет старт, ч<input type='number' name='blmH0' min='0' max='23' value='" + String(bloomStartHour) + "'></label>";
  html += "<label>Цвет стоп, ч<input type='number' name='blmH1' min='1' max='24' value='" + String(bloomEndHour) + "'></label>";
  html += "<label>Рассвет, мин<input type='number' name='sunrise' min='0' max='120' value='" + String(sunriseMin) + "'></label>";
  html += "<label>Темп день<input type='number' step='0.1' name='tDay' value='" + String(tempTargetDay, 1) + "'></label>";
  html += "<label>Темп ночь<input type='number' step='0.1' name='tNight' value='" + String(tempTargetNight, 1) + "'></label>";
  html += "<label>Темп сушка<input type='number' step='0.1' name='tDry' value='" + String(tempTargetDry, 1) + "'></label>";
  html += "<label>Гистерезис<input type='number' step='0.1' name='tHyst' value='" + String(tempHysteresis, 1) + "'></label>";
  html += "<label>Авария °C<input type='number' step='0.1' name='tEmerg' value='" + String(tempEmergency, 1) + "'></label>";
  html += "<label>VPD вега min<input type='number' step='0.05' name='vpdVmin' value='" + String(vpdVegMin, 2) + "'></label>";
  html += "<label>VPD вега max<input type='number' step='0.05' name='vpdVmax' value='" + String(vpdVegMax, 2) + "'></label>";
  html += "<label>VPD цвет min<input type='number' step='0.05' name='vpdBmin' value='" + String(vpdBloomMin, 2) + "'></label>";
  html += "<label>VPD цвет max<input type='number' step='0.05' name='vpdBmax' value='" + String(vpdBloomMax, 2) + "'></label>";
  html += "<label>Обдув вкл, мин<input type='number' name='windOn' min='1' max='60' value='" + String(windOnMs / 60000) + "'></label>";
  html += "<label>Обдув пауза, мин<input type='number' name='windOff' min='1' max='60' value='" + String(windOffMs / 60000) + "'></label>";
  html += "</div><label><input type='checkbox' name='humidEn' value='1'";
  if (enableHumidifier) html += " checked";
  html += "> Увлажнитель GPIO21</label>";
  html += "<button type='submit'>💾 Сохранить климат</button></form></div>";

  html += "<div class='c'><b>Полив</b><form action='/saveSettings' method='POST'>";
  html += "<input type='hidden' name='next' value='admin'><div class='g'>";
  html += "<label>Полив, сек<input type='number' name='waterSec' min='1' max='120' value='" + String(wateringDurationMs / 1000) + "'></label>";
  html += "<label>Порог почвы %<input type='number' name='soilDry' min='5' max='80' value='" + String(soilDryThreshold) + "'></label>";
  html += "<label>Soak, мин<input type='number' name='soakMin' min='5' max='240' value='" + String(soilSoakDelayMs / 60000) + "'></label>";
  html += "<label>Резерв, ч<input type='number' name='fbHours' min='6' max='72' value='" + String(fallbackWateringMs / 3600000UL) + "'></label>";
  html += "</div><button type='submit'>💾 Сохранить полив</button></form>";
  html += "<div style='margin-top:8px'>";
  html += "<button onclick=\"fetch('/water?z=0')\">💧 Зона 1</button>";
  html += "<button onclick=\"fetch('/water?z=1')\">💧 Зона 2</button>";
  html += "<button onclick=\"fetch('/water?z=2')\">💧 Зона 3</button></div>";
  html += "<div style='margin-top:6px'>";
  html += "<button class='sec' onclick=\"fetch('/setStage?s=veg')\">🌱 Вега</button>";
  html += "<button class='sec' onclick=\"fetch('/setStage?s=bloom')\">🌸 Цвет</button>";
  html += "<button class='sec' onclick=\"fetch('/setStage?s=dry')\">🍂 Сушка</button></div></div>";

  html += "<div class='c'><b>Калибровка почвы</b>";
  html += "<p style='font-size:12px;color:#8aa392'>Датчик в сухом грунте → 0%. В мокром → 100%.</p>";
  for (int z = 0; z < 3; z++) {
    html += "<div class='r'><span>#" + String(z + 1) + " ADC " + String(soilRaw[z]);
    html += " · сухо " + String(soilCalibDry[z]) + " / мокро " + String(soilCalibWet[z]) + "</span></div>";
    html += "<button class='sec' onclick=\"fetch('/calib?z=" + String(z) + "&t=dry').then(()=>location.reload())\">0% сухо</button>";
    html += "<button class='sec' onclick=\"fetch('/calib?z=" + String(z) + "&t=wet').then(()=>location.reload())\">100% мокро</button>";
  }
  html += "</div>";

  html += "<div class='c'><b>Канал</b>";
  html += "<div class='r'><span>ntfy</span><b class='" + String(remoteEnabled ? "ok" : "bad") + "'>" + String(remoteEnabled ? "ВКЛ" : "ВЫКЛ") + "</b></div>";
  html += "<div class='r'><span>Событие</span><b>" + lastRemoteEvent + "</b></div>";
  html += "<div class='r'><span>OTA</span><b>" + lastOtaResult + "</b></div>";
  html += "<button onclick=\"fetch('/remotePull').then(r=>r.text()).then(t=>alert(t))\">ntfy</button>";
  html += "<button onclick=\"fetch('/otaCheck').then(r=>r.text()).then(t=>alert(t))\">version.json</button></div>";

  html += "<div class='c'>";
  html += "<a class='b' href='/'>Дашборд</a>";
  html += "<a class='b' href='/update'>OTA файл</a>";
  html += "<button class='sec' onclick=\"fetch('/allAuto')\">Все в авто</button>";
  html += "<button class='sec' onclick=\"if(confirm('Ребут?'))fetch('/reboot')\">Ребут</button></div>";
  html += "<p style='font-size:12px;color:#8aa392'>LAN only · /admin</p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}
