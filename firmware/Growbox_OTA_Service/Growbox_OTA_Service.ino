// GrowBox Service OTA — тонкий загрузчик без WDT и датчиков.
// Заливается через старый ElegantOTA (<8 сек), затем принимает полный GrowBox-v6.3.bin.

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>

WebServer server(80);

static const char PAGE[] PROGMEM = R"html(
<!DOCTYPE html><html lang="ru"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GrowBox Service OTA</title>
<style>
body{font-family:sans-serif;background:#0b100c;color:#d8f3dc;max-width:520px;margin:24px auto;padding:0 12px}
h1{color:#52b788;font-size:20px}
.card{background:#121a14;border:1px solid #1d2a20;border-radius:10px;padding:14px}
.ok{color:#95d5b2}.warn{color:#ffe066}
input,button{font-size:15px;margin-top:8px}
button{background:#2d6a4f;color:#fff;border:0;padding:10px 14px;border-radius:6px}
#log{white-space:pre-wrap;font-size:12px;color:#9aaf9e;margin-top:10px}
</style></head><body>
<h1>🛠️ GrowBox Service OTA</h1>
<div class="card">
<p class="ok">Сервисная прошивка. Watchdog выключен — можно лить полный образ.</p>
<p>Залей <b>GrowBox-v6.3.bin</b> (не этот сервисный файл).</p>
<p>IP: <b>%IP%</b> &nbsp;|&nbsp; RSSI: <b>%RSSI%</b> dBm</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin" required>
<br><button type="submit">Прошить полную прошивку</button>
</form>
<p class="warn">Не выключай питание до ребута. Заливка 1.2 МБ занимает 20–60 сек.</p>
<div id="log"></div>
</div>
</body></html>
)html";

String htmlPage() {
  String s = FPSTR(PAGE);
  s.replace("%IP%", WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
  s.replace("%RSSI%", WiFi.isConnected() ? String(WiFi.RSSI()) : String(0));
  return s;
}

void connectWifi() {
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 18000) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Growbox-OTA", "12345678");
  }
}

void setup() {
  Serial.begin(115200);
  // Никакого watchdog — иначе снова оборвёт OTA.
  connectWifi();
  MDNS.begin("growbox");

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage());
  });

  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Location", "/");
    server.send(302);
  });

  server.on("/update", HTTP_POST, []() {
    const bool ok = !Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", ok ? "OK. Rebooting..." : "FAIL");
    delay(800);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      // Пишем в неактивный OTA-слот. Размер неизвестен заранее.
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!Update.end(true)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
    }
  });

  server.begin();
  Serial.println("[ServiceOTA] ready");
  if (WiFi.isConnected()) {
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("AP Growbox-OTA / 12345678");
  }
}

void loop() {
  server.handleClient();
  delay(2);
}
