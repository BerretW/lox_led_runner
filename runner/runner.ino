#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ==========================================
// ========== HARDWARE NASTAVENÍ ============
// ==========================================
#define MAX_STRIPS 4
// Piny pro pásky. Na NodeMCU/Wemos to je: 5(D1), 4(D2), 14(D5), 12(D6)
const int STRIP_PINS[MAX_STRIPS] = {0, 2, 4, 5}; 

// ==========================================
// ====== STRUKTURA PRO ULOŽENÍ DO EEPROM ===
// ==========================================
struct Config {
  char magic[4];
  char ssid[32];
  char password[64];
  char moduleName[32];
  int numStrips;
  int numPixels[MAX_STRIPS];
  int fillSpeedMs[MAX_STRIPS];
  bool reverseOff;
};

Config config;
ESP8266WebServer server(80);
DNSServer dnsServer;           

bool apMode = false;           
const byte DNS_PORT = 53;

enum StripState { STATE_OFF, STATE_FILLING, STATE_ON, STATE_UNFILLING };

// ==========================================
// ====== TŘÍDA PRO JEDEN NEOPIXEL PÁSEK ====
// ==========================================
struct NeoStrip {
  Adafruit_NeoPixel* pixels = nullptr;
  int pin;
  int numPixels;
  
  StripState state = STATE_OFF;
  unsigned long fillStartMillis = 0;
  int lastLitPixel = -1;
  
  int fillSpeedMs = 30;
  bool reverseOff = false;
  int colorRed = 150, colorGreen = 150, colorBlue = 150;

  void init(int p, int count) {
    pin = p;
    numPixels = count;
    if (numPixels > 0) {
      if (pixels) { delete pixels; pixels = nullptr; }
      pixels = new Adafruit_NeoPixel(numPixels, pin, NEO_GRB + NEO_KHZ800);
      if (!pixels) { numPixels = 0; return; }
      pixels->begin();
      pixels->clear();
      pixels->show();
    }
  }

  void turnOn() {
    if (numPixels <= 0 || pixels == nullptr) return;
    state = STATE_FILLING;
    fillStartMillis = millis();
    lastLitPixel = -1;
    pixels->clear();
    pixels->show();
  }

  void turnOff() {
    if (numPixels <= 0 || pixels == nullptr) return;
    if (reverseOff && state == STATE_ON) {
      state = STATE_UNFILLING;
      fillStartMillis = millis();
      lastLitPixel = 0;
    } else {
      state = STATE_OFF;
      pixels->clear();
      pixels->show();
    }
  }

  void update() {
    if (numPixels <= 0 || pixels == nullptr) return;

    if (state == STATE_FILLING) {
      unsigned long elapsed = millis() - fillStartMillis;
      unsigned long totalTime = (unsigned long)numPixels * fillSpeedMs;

      if (elapsed >= totalTime) {
        for (int i = 0; i < numPixels; i++)
          pixels->setPixelColor(i, pixels->Color(colorRed, colorGreen, colorBlue));
        pixels->show();
        state = STATE_ON;
      } else {
        float progress = (float)elapsed / fillSpeedMs;
        int currentPix = (int)progress;
        float fraction = progress - currentPix;

        if (lastLitPixel != -1 && currentPix > lastLitPixel) {
          for (int i = lastLitPixel; i < currentPix; i++)
            if (i < numPixels)
              pixels->setPixelColor(i, pixels->Color(colorRed, colorGreen, colorBlue));
        }
        lastLitPixel = currentPix;

        if (currentPix < numPixels) {
          pixels->setPixelColor(currentPix, pixels->Color(
            (int)(colorRed * fraction),
            (int)(colorGreen * fraction),
            (int)(colorBlue * fraction)
          ));
        }
        pixels->show();
      }
    }

    else if (state == STATE_UNFILLING) {
      unsigned long elapsed = millis() - fillStartMillis;
      unsigned long totalTime = (unsigned long)numPixels * fillSpeedMs;

      if (elapsed >= totalTime) {
        pixels->clear();
        pixels->show();
        state = STATE_OFF;
      } else {
        float progress = (float)elapsed / fillSpeedMs;
        int currentOff = (int)progress;
        float fraction = 1.0f - (progress - currentOff);

        // turn off newly completed pixels (from end)
        if (currentOff > lastLitPixel) {
          for (int i = lastLitPixel; i < currentOff; i++) {
            int idx = numPixels - 1 - i;
            if (idx >= 0) pixels->setPixelColor(idx, 0);
          }
          lastLitPixel = currentOff;
        }

        // dim the pixel currently transitioning
        int idx = numPixels - 1 - currentOff;
        if (idx >= 0) {
          pixels->setPixelColor(idx, pixels->Color(
            (int)(colorRed * fraction),
            (int)(colorGreen * fraction),
            (int)(colorBlue * fraction)
          ));
        }
        pixels->show();
      }
    }
  }
};

NeoStrip strips[MAX_STRIPS];

void loadConfig() { EEPROM.get(0, config); }
void saveConfig() { EEPROM.put(0, config); EEPROM.commit(); }
void resetConfig() { memset(config.magic, 0, sizeof(config.magic)); saveConfig(); }

// WiFi scan se provede jednou při startu a výsledek se cachuje.
// Opakované skenování uvnitř HTTP handleru by blokovalo smyčku a mohlo crashnout WDT.
String cachedWifiOptions;

void buildWifiOptions() {
  int n = WiFi.scanNetworks();
  cachedWifiOptions = "";
  if (n > 0) {
    cachedWifiOptions.reserve(n * 72);
    for (int i = 0; i < n; i++) {
      cachedWifiOptions += "<option value=\"";
      cachedWifiOptions += WiFi.SSID(i);
      cachedWifiOptions += "\">";
      cachedWifiOptions += WiFi.SSID(i);
      cachedWifiOptions += " (";
      cachedWifiOptions += String(WiFi.RSSI(i));
      cachedWifiOptions += " dBm)</option>";
    }
  }
  WiFi.scanDelete();
}

// ==========================================
// ======== HTML STRÁNKA NASTAVENÍ ==========
// ==========================================
String getSetupPage() {

  String html;
  html.reserve(4096);
  html += R"rawliteral(
  <!DOCTYPE html>
  <html lang="cs">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Nastavení ESP8266</title>
    <style>
      body { font-family: Arial, sans-serif; background-color: #f4f4f9; padding: 10px; color: #333; }
      .container { max-width: 450px; margin: auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }
      h2, h3 { text-align: center; color: #0056b3; }
      label { font-weight: bold; display: block; margin-top: 15px; }
      input[type="text"], input[type="password"], input[type="number"] { width: 100%; padding: 10px; margin-top: 5px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
      .btn { width: 100%; color: white; border: none; padding: 12px; margin-top: 10px; border-radius: 4px; cursor: pointer; font-size: 16px; font-weight: bold; }
      .btn-save { background-color: #28a745; margin-top: 25px; }
      .btn-on { background-color: #007bff; width: 48%; }
      .btn-off { background-color: #6c757d; width: 48%; }
      .test-row { display: flex; justify-content: space-between; align-items: center; margin-top: 10px; padding: 10px; background: #f9f9f9; border-radius: 5px; }
      .note { font-size: 0.85em; color: #666; display: block; margin-top: 3px; font-weight: normal; }
      hr { margin: 30px 0; border: 0; border-top: 1px solid #eee; }
    </style>
    <style>
      .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); background: #333; color: #fff; padding: 10px 22px; border-radius: 6px; font-size: 15px; opacity: 0; transition: opacity 0.3s; pointer-events: none; z-index: 999; }
      .toast.show { opacity: 1; }
      .toast.ok { background: #28a745; }
      .toast.err { background: #dc3545; }
      .status-dot { display: inline-block; width: 12px; height: 12px; border-radius: 50%; background: #ccc; margin-right: 8px; vertical-align: middle; transition: background 0.4s; }
      .status-dot.on { background: #007bff; box-shadow: 0 0 6px #007bff; }
      .btn:disabled { opacity: 0.6; cursor: not-allowed; }
    </style>
    <script>
      var toastTimer;
      function showToast(msg, ok) {
        var t = document.getElementById('toast');
        t.textContent = msg;
        t.className = 'toast show ' + (ok ? 'ok' : 'err');
        clearTimeout(toastTimer);
        toastTimer = setTimeout(function() { t.className = 'toast'; }, 2200);
      }

      function setDot(id, on) {
        var dots = id === 0
          ? document.querySelectorAll('.status-dot')
          : [document.getElementById('dot-' + id)];
        dots.forEach(function(d) { if (d) d.className = 'status-dot' + (on ? ' on' : ''); });
      }

      function test(action, id, btn) {
        if (btn) { btn.disabled = true; btn.textContent = '...'; }
        var orig = btn ? (action === 'on' ? 'ON' : 'OFF') : '';
        fetch('/' + action + '?id=' + id)
          .then(function(r) {
            if (!r.ok) throw new Error(r.status);
            return r.text();
          })
          .then(function() {
            var label = id === 0 ? 'Vše' : 'Pásek ' + id;
            showToast(label + ': ' + (action === 'on' ? 'Zapnuto' : 'Vypnuto'), true);
            setDot(id, action === 'on');
          })
          .catch(function() { showToast('Chyba spojení!', false); })
          .finally(function() { if (btn) { btn.disabled = false; btn.textContent = orig; } });
      }
    </script>
  </head>
  <body>
    <div id="toast" class="toast"></div>
    <div class="container">
      <h2>Konfigurace Modulu</h2>
      <form action="/save" method="POST">
        <label>Jméno modulu (Hostname):</label>
        <input type="text" name="moduleName" maxlength="31" value=")rawliteral" + String(config.moduleName) + R"rawliteral(">

        <label>WiFi SSID:</label>
        <input type="text" name="ssid" list="wifi-list" maxlength="31" value=")rawliteral" + String(config.ssid) + R"rawliteral(">
        <datalist id="wifi-list">)rawliteral" + cachedWifiOptions + R"rawliteral(</datalist>
        
        <label>WiFi Heslo:</label>
        <input type="password" name="pass" maxlength="63" placeholder="(Beze změny nechte prázdné)">
        
        <label>Počet aktivních LED pásků (1-4):</label>
        <input type="number" name="numStrips" min="1" max="4" value=")rawliteral" + String(config.numStrips) + R"rawliteral(">
  )rawliteral";

  for (int i = 0; i < MAX_STRIPS; i++) {
    html += "<label>Počet LED - Pásek " + String(i + 1) + " (GPIO" + String(STRIP_PINS[i]) + "):</label>";
    html += "<input type='number' name='led" + String(i) + "' min='0' max='1000' value='" + String(config.numPixels[i]) + "'>";
    html += "<label>Rychlost rozsvícení - Pásek " + String(i + 1) + " (ms/LED):<span class='note'>Nižší = rychlejší. Výchozí: 30</span></label>";
    html += "<input type='number' name='spd" + String(i) + "' min='1' max='500' value='" + String(config.fillSpeedMs[i] > 0 ? config.fillSpeedMs[i] : 30) + "'>";
  }

  String revChecked = config.reverseOff ? " checked" : "";
  html += R"rawliteral(
        <div style='margin-top:20px; padding:12px; background:#f0f4ff; border-radius:6px; display:flex; align-items:center; gap:12px;'>
          <input type='checkbox' name='reverseOff' id='revOff' value='1')rawliteral" + revChecked + R"rawliteral( style='width:20px;height:20px;cursor:pointer;'>
          <label for='revOff' style='margin:0; font-weight:bold; cursor:pointer;'>Zpětny chod pri shasnutí
            <span class='note'>LED pásky zhasínají animovaně od konce.</span>
          </label>
        </div>
        <input type="submit" class="btn btn-save" value="Uložit a Restartovat">
      </form>

      <hr>
      <h3>Test výstupů</h3>
  )rawliteral";

  // Generování tlačítek pro testování
  for (int i = 0; i < config.numStrips; i++) {
    if (config.numPixels[i] > 0) {
      html += "<div class='test-row'>";
      html += "<span><span class='status-dot' id='dot-" + String(i + 1) + "'></span>Pásek " + String(i + 1) + ":</span>";
      html += "<div>";
      html += "<button class='btn btn-on' onclick=\"test('on'," + String(i + 1) + ",this)\">ON</button> ";
      html += "<button class='btn btn-off' onclick=\"test('off'," + String(i + 1) + ",this)\">OFF</button>";
      html += "</div></div>";
    }
  }
  
  html += "<button class='btn btn-on' style='width:100%; margin-top:15px;' onclick=\"test('on',0,this)\">Zapnout vše</button>";
  html += "<button class='btn btn-off' style='width:100%;' onclick=\"test('off',0,this)\">Vypnout vše</button>";

  html += R"rawliteral(
    </div>
    <p style='text-align:center; font-size:12px; color:#aaa;'>IP: )rawliteral" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + R"rawliteral(</p>
  </body>
  </html>
  )rawliteral";

  return html;
}

// ==========================================
// ============== SETUP & LOOP ==============
// ==========================================
void setup() {
  Serial.begin(115200);
  EEPROM.begin(512); 
  delay(500);

  loadConfig();
  bool validConfig = (strcmp(config.magic, "CFG") == 0);

  if (!validConfig || strlen(config.moduleName) == 0) strncpy(config.moduleName, "ESP8266_Modul", 32);
  if (!validConfig || config.numStrips < 1 || config.numStrips > MAX_STRIPS) config.numStrips = 1;
  
  WiFi.hostname(config.moduleName);
  
  for (int i = 0; i < MAX_STRIPS; i++) {
    if (!validConfig) config.numPixels[i] = (i == 0) ? 60 : 0;
    if (config.fillSpeedMs[i] <= 0) config.fillSpeedMs[i] = 30;
    strips[i].init(STRIP_PINS[i], config.numPixels[i]);
    strips[i].fillSpeedMs = config.fillSpeedMs[i];
    strips[i].reverseOff = config.reverseOff;
  }

  WiFi.mode(WIFI_STA);
  buildWifiOptions();

  if (validConfig && strlen(config.ssid) > 0) {
    WiFi.begin(config.ssid, config.password);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500);
      retries++;
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(config.moduleName); 
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  }
  
  server.on("/", []() { server.send(200, "text/html", getSetupPage()); });

  server.on("/save", HTTP_POST, []() {
    strncpy(config.magic, "CFG", 4);
    if (server.hasArg("moduleName")) {
      strncpy(config.moduleName, server.arg("moduleName").c_str(), sizeof(config.moduleName) - 1);
      config.moduleName[sizeof(config.moduleName) - 1] = '\0';
    }
    if (server.hasArg("ssid")) {
      strncpy(config.ssid, server.arg("ssid").c_str(), sizeof(config.ssid) - 1);
      config.ssid[sizeof(config.ssid) - 1] = '\0';
    }
    if (server.hasArg("pass") && server.arg("pass").length() > 0) {
      strncpy(config.password, server.arg("pass").c_str(), sizeof(config.password) - 1);
      config.password[sizeof(config.password) - 1] = '\0';
    }
    if (server.hasArg("numStrips")) {
      int ns = server.arg("numStrips").toInt();
      config.numStrips = (ns < 1) ? 1 : (ns > MAX_STRIPS) ? MAX_STRIPS : ns;
    }
    config.reverseOff = server.hasArg("reverseOff");
    for (int i = 0; i < MAX_STRIPS; i++) {
      String ledArg = "led" + String(i);
      if (server.hasArg(ledArg)) {
        int px = server.arg(ledArg).toInt();
        config.numPixels[i] = (px < 0) ? 0 : (px > 1000) ? 1000 : px;
      }
      String spdArg = "spd" + String(i);
      if (server.hasArg(spdArg)) {
        int spd = server.arg(spdArg).toInt();
        config.fillSpeedMs[i] = (spd < 1) ? 1 : (spd > 500) ? 500 : spd;
      }
    }
    saveConfig();
    server.send(200, "text/html", "Ulozeno. Restartuji...");
    delay(1000);
    ESP.restart();
  });
  
  server.on("/on", []() {
    int id = server.hasArg("id") ? server.arg("id").toInt() : 0;
    int count = min(config.numStrips, (int)MAX_STRIPS);
    if (id > 0 && id <= MAX_STRIPS) strips[id - 1].turnOn();
    else { for (int i = 0; i < count; i++) strips[i].turnOn(); }
    server.send(200, "text/plain", "OK");
  });

  server.on("/off", []() {
    int id = server.hasArg("id") ? server.arg("id").toInt() : 0;
    int count = min(config.numStrips, (int)MAX_STRIPS);
    if (id > 0 && id <= MAX_STRIPS) strips[id - 1].turnOff();
    else { for (int i = 0; i < count; i++) strips[i].turnOff(); }
    server.send(200, "text/plain", "OK");
  });

  server.onNotFound([]() {
    if (apMode) { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "Not Found");
  });

  server.begin();
}

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  for (int i = 0; i < MAX_STRIPS; i++) strips[i].update();
}