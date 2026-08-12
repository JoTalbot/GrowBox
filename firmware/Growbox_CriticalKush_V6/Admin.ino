// Админка: /admin
void handleReboot() {
  server.send(200, "text/plain", "REBOOT");
  delay(300);
  ESP.restart();
}

static void adminNum(String& html, const char* lab, const char* name, const String& val) {
  html += "<label>";
  html += lab;
  html += "<input name='";
  html += name;
  html += "' value='";
  html += val;
  html += "'></label>";
}

void handleAdmin() {
  feedWatchdog();
  String html = F("<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Admin</title><style>"
    "body{font-family:sans-serif;background:#070a08;color:#d8f3dc;max-width:540px;margin:12px auto;padding:0 10px}"
    "h1{color:#52b788;font-size:18px}a{color:#74c69d}"
    ".c{background:#121a14;border:1px solid #1d2a20;border-radius:8px;padding:10px;margin:0 0 8px}"
    ".r{display:flex;justify-content:space-between;margin:4px 0;font-size:13px}"
    "button,.b{background:#2d6a4f;color:#fff;border:0;padding:7px 8px;border-radius:5px;margin:3px 3px 0 0;font-weight:700;text-decoration:none;font-size:12px;display:inline-block}"
    "button.s{background:#18201a;border:1px solid #253328}"
    "label{font-size:11px;color:#9aaf9e}input{width:100%;background:#151d17;border:1px solid #253328;color:#fff;padding:5px;border-radius:4px;box-sizing:border-box}"
    ".g{display:grid;grid-template-columns:1fr 1fr;gap:5px 8px}"
    "</style></head><body><h1>🛠️ Admin v");
  html += FIRMWARE_VERSION;
  html += "</h1><div class='c'>";
  html += "<div class='r'><span>ID</span><b>" + deviceId() + "</b></div>";
  html += "<div class='r'><span>IP</span><b>" + WiFi.localIP().toString() + "</b></div>";
  html += "<div class='r'><span>День</span><b>" + String(getGrowDay()) + "</b></div>";
  html += "<div class='r'><span>DHT</span><b>" + String(dhtConnected ? String(temperature, 1) : "НЕТ") + "</b></div>";
  for (int i = 0; i < 3; i++) {
    html += "<div class='r'><span>Почва" + String(i + 1) + "</span><b>ADC " + String(soilRaw[i]);
    html += soilConnected[i] ? (" " + String(soilMoisture[i]) + "%") : " ОТКЛ";
    html += "</b></div>";
  }
  html += "</div><div class='c'><b>Реле</b>";
  html += "<div class=r><span>Свет " + String(stateLight ? "ВКЛ" : "ВЫКЛ") + " / " + String(modeShort(modeLight)) + "</span></div>";
  html += "<button class=s onclick=\"fetch('/setMode?d=light&m=auto').then(()=>location.reload())\">Авто</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=light&m=on').then(()=>location.reload())\">Вкл</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=light&m=off').then(()=>location.reload())\">Выкл</button>";
  html += "<div class=r><span>Вытяжка " + String(stateExhaust ? "ВКЛ" : "ВЫКЛ") + " / " + String(modeShort(modeExhaust)) + "</span></div>";
  html += "<button class=s onclick=\"fetch('/setMode?d=exhaust&m=auto').then(()=>location.reload())\">Авто</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=exhaust&m=on').then(()=>location.reload())\">Вкл</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=exhaust&m=off').then(()=>location.reload())\">Выкл</button>";
  html += "<div class=r><span>Обогрев " + String(stateHeater ? "ВКЛ" : "ВЫКЛ") + " / " + String(modeShort(modeHeater)) + "</span></div>";
  html += "<button class=s onclick=\"fetch('/setMode?d=heater&m=auto').then(()=>location.reload())\">Авто</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=heater&m=on').then(()=>location.reload())\">Вкл</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=heater&m=off').then(()=>location.reload())\">Выкл</button>";
  html += "<div class=r><span>Обдув " + String(stateFan ? "ВКЛ" : "ВЫКЛ") + " / " + String(modeShort(modeFan)) + "</span></div>";
  html += "<button class=s onclick=\"fetch('/setMode?d=fan&m=auto').then(()=>location.reload())\">Авто</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=fan&m=on').then(()=>location.reload())\">Вкл</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=fan&m=off').then(()=>location.reload())\">Выкл</button>";
  html += "<div class=r><span>Увлажн " + String(stateHumid ? "ВКЛ" : "ВЫКЛ") + " / " + String(modeShort(modeHumid)) + "</span></div>";
  html += "<button class=s onclick=\"fetch('/setMode?d=humid&m=auto').then(()=>location.reload())\">Авто</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=humid&m=on').then(()=>location.reload())\">Вкл</button>";
  html += "<button class=s onclick=\"fetch('/setMode?d=humid&m=off').then(()=>location.reload())\">Выкл</button>";
  html += "<br><button class=s onclick=\"fetch('/allAuto').then(()=>location.reload())\">Все в авто</button></div>";

  html += "<div class='c'><b>Климат</b><form action=/saveSettings method=POST>";
  html += "<input type=hidden name=next value=admin><input type=hidden name=clim value=1><div class=g>";
  adminNum(html, "Вега с", "vegH0", String(vegStartHour));
  adminNum(html, "Вега по", "vegH1", String(vegEndHour));
  adminNum(html, "Цвет с", "blmH0", String(bloomStartHour));
  adminNum(html, "Цвет по", "blmH1", String(bloomEndHour));
  adminNum(html, "Рассвет мин", "sunrise", String(sunriseMin));
  adminNum(html, "Т день", "tDay", String(tempTargetDay, 1));
  adminNum(html, "Т ночь", "tNight", String(tempTargetNight, 1));
  adminNum(html, "Т сушка", "tDry", String(tempTargetDry, 1));
  adminNum(html, "Гистер", "tHyst", String(tempHysteresis, 1));
  adminNum(html, "Авария", "tEmerg", String(tempEmergency, 1));
  adminNum(html, "VPD v min", "vpdVmin", String(vpdVegMin, 2));
  adminNum(html, "VPD v max", "vpdVmax", String(vpdVegMax, 2));
  adminNum(html, "VPD b min", "vpdBmin", String(vpdBloomMin, 2));
  adminNum(html, "VPD b max", "vpdBmax", String(vpdBloomMax, 2));
  html += "</div><button>Сохранить климат</button></form></div>";

  html += "<div class='c'><b>Полив</b><form action=/saveSettings method=POST>";
  html += "<input type=hidden name=next value=admin><div class=g>";
  adminNum(html, "Сек", "waterSec", String(wateringDurationMs / 1000));
  adminNum(html, "Порог %", "soilDry", String(soilDryThreshold));
  adminNum(html, "Soak мин", "soakMin", String(soilSoakDelayMs / 60000));
  adminNum(html, "Резерв ч", "fbHours", String(fallbackWateringMs / 3600000UL));
  html += "</div><button>Сохранить полив</button></form>";
  html += "<button onclick=\"fetch('/water?z=0')\">💧1</button>";
  html += "<button onclick=\"fetch('/water?z=1')\">💧2</button>";
  html += "<button onclick=\"fetch('/water?z=2')\">💧3</button>";
  html += "<button class=s onclick=\"fetch('/setStage?s=veg')\">Вега</button>";
  html += "<button class=s onclick=\"fetch('/setStage?s=bloom')\">Цвет</button>";
  html += "<button class=s onclick=\"fetch('/setStage?s=dry')\">Сушка</button></div>";

  html += "<div class='c'><b>Калибровка</b>";
  for (int z = 0; z < 3; z++) {
    html += "<div class=r><span>#" + String(z + 1) + " raw " + String(soilRaw[z]);
    html += " dry " + String(soilCalibDry[z]) + " wet " + String(soilCalibWet[z]) + "</span></div>";
    html += "<button class=s onclick=\"fetch('/calib?z=" + String(z) + "&t=dry').then(()=>location.reload())\">сухо</button>";
    html += "<button class=s onclick=\"fetch('/calib?z=" + String(z) + "&t=wet').then(()=>location.reload())\">мокро</button>";
  }
  html += "</div><div class=c>";
  html += "<a class=b href=/>Дашборд</a><a class=b href=/update>OTA</a>";
  html += "<button class=s onclick=\"fetch('/allAuto')\">Авто</button>";
  html += "<button class=s onclick=\"if(confirm('reboot'))fetch('/reboot')\">Ребут</button></div></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}
