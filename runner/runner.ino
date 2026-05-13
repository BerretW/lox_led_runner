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
};

Config config;
ESP8266WebServer server(80);
DNSServer dnsServer;           

bool apMode = false;           
const byte DNS_PORT = 53;

enum StripState { STATE_OFF, STATE_FILLING, STATE_ON };

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
  int colorRed = 150, colorGreen = 150, colorBlue = 150;

  void init(int p, int count) {
    pin = p;
    numPixels = count;
    if (numPixels > 0) {
      if (pixels) delete pixels; 
      pixels = new Adafruit_NeoPixel(numPixels, pin, NEO_GRB + NEO_KHZ800);
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
    state = STATE_OFF;
    pixels->clear();
    pixels->show();
  }

  void update() {
    if (state != STATE_FILLING || numPixels == 0 || pixels == nullptr) return;

    unsigned long elapsed = millis() - fillStartMillis;
    unsigned long totalTime = numPixels * fillSpeedMs;

    if (elapsed >= totalTime) {
      for (int i = 0; i < numPixels; i++) {
        pixels->setPixelColor(i, pixels->Color(colorRed, colorGreen, colorBlue));
      }
      pixels->show();
      state = STATE_ON;
    } 
    else {
      float progress = (float)elapsed / fillSpeedMs;
      int currentPix = (int)progress;             
      float fraction = progress - currentPix;     

      if (lastLitPixel != -1 && currentPix > lastLitPixel) {
        for (int i = lastLitPixel; i < currentPix; i++) {
          if (i < numPixels) {
            pixels->setPixelColor(i, pixels->Color(colorRed, colorGreen, colorBlue));
          }
        }
      }
      lastLitPixel = currentPix;

      if (currentPix < numPixels) {
        int r = (int)(colorRed * fraction);
        int g = (int)(colorGreen * fraction);
        int b = (int)(colorBlue * fraction);
        pixels->setPixelColor(currentPix, pixels->Color(r, g, b));
      }
      pixels->show();
    }
  }
};

NeoStrip strips[MAX_STRIPS];

void loadConfig() { EEPROM.get(0, config); }
void saveConfig() { EEPROM.put(0, config); EEPROM.commit(); }
void resetConfig() { memset(config.magic, 0, sizeof(config.magic)); saveConfig(); }

// ==========================================
// ======== HTML STRÁNKA NASTAVENÍ ==========
// ==========================================
String getSetupPage() {
  int n = WiFi.scanNetworks();
  String wifiOptions = "";
  if (n > 0) {
    for (int i = 0; i < n; ++i) {
      wifiOptions += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }

  String html = R"rawliteral(
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
    <script>
      function test(action, id) {
        fetch('/' + action + '?id=' + id)
          .then(response => console.log('Test ' + action + ' ID ' + id));
      }
    </script>
  </head>
  <body>
    <div class="container">
      <h2>Konfigurace Modulu</h2>
      <form action="/save" method="POST">
        <label>Jméno modulu (Hostname):</label>
        <input type="text" name="moduleName" maxlength="31" value=")rawliteral" + String(config.moduleName) + R"rawliteral(">

        <label>WiFi SSID:</label>
        <input type="text" name="ssid" list="wifi-list" maxlength="31" value=")rawliteral" + String(config.ssid) + R"rawliteral(">
        <datalist id="wifi-list">)rawliteral" + wifiOptions + R"rawliteral(</datalist>
        
        <label>WiFi Heslo:</label>
        <input type="password" name="pass" maxlength="63" placeholder="(Beze změny nechte prázdné)">
        
        <label>Počet aktivních LED pásků (1-4):</label>
        <input type="number" name="numStrips" min="1" max="4" value=")rawliteral" + String(config.numStrips) + R"rawliteral(">
  )rawliteral";

  for (int i = 0; i < MAX_STRIPS; i++) {
    html += "<label>Počet LED - Pásek " + String(i + 1) + " (GPIO" + String(STRIP_PINS[i]) + "):</label>";
    html += "<input type='number' name='led" + String(i) + "' min='0' max='1000' value='" + String(config.numPixels[i]) + "'>";
  }

  html += R"rawliteral(
        <input type="submit" class="btn btn-save" value="Uložit a Restartovat">
      </form>

      <hr>
      <h3>Test výstupů</h3>
  )rawliteral";

  // Generování tlačítek pro testování
  for (int i = 0; i < config.numStrips; i++) {
    if (config.numPixels[i] > 0) {
      html += "<div class='test-row'>";
      html += "<span>Pásek " + String(i + 1) + ":</span>";
      html += "<div>";
      html += "<button class='btn btn-on' onclick=\"test('on', " + String(i + 1) + ")\">ON</button> ";
      html += "<button class='btn btn-off' onclick=\"test('off', " + String(i + 1) + ")\">OFF</button>";
      html += "</div></div>";
    }
  }
  
  html += "<button class='btn btn-on' style='width:100%; margin-top:15px;' onclick=\"test('on', 0)\">Zapnout vše</button>";
  html += "<button class='btn btn-off' style='width:100%;' onclick=\"test('off', 0)\">Vypnout vše</button>";

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
    strips[i].init(STRIP_PINS[i], config.numPixels[i]);
  }

  if (validConfig && strlen(config.ssid) > 0) {
    WiFi.mode(WIFI_STA);
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
    if (server.hasArg("moduleName")) strncpy(config.moduleName, server.arg("moduleName").c_str(), 31);
    if (server.hasArg("ssid")) strncpy(config.ssid, server.arg("ssid").c_str(), 31);
    if (server.hasArg("pass") && server.arg("pass").length() > 0) strncpy(config.password, server.arg("pass").c_str(), 63);
    if (server.hasArg("numStrips")) config.numStrips = server.arg("numStrips").toInt();
    for (int i = 0; i < MAX_STRIPS; i++) {
      String argName = "led" + String(i);
      if (server.hasArg(argName)) config.numPixels[i] = server.arg(argName).toInt();
    }
    saveConfig();
    server.send(200, "text/html", "Ulozeno. Restartuji...");
    delay(1000);
    ESP.restart();
  });
  
  server.on("/on", []() {
    int id = server.hasArg("id") ? server.arg("id").toInt() : 0;
    if (id > 0 && id <= MAX_STRIPS) strips[id - 1].turnOn();
    else { for (int i = 0; i < config.numStrips; i++) strips[i].turnOn(); }
    server.send(200, "text/plain", "OK");
  });

  server.on("/off", []() {
    int id = server.hasArg("id") ? server.arg("id").toInt() : 0;
    if (id > 0 && id <= MAX_STRIPS) strips[id - 1].turnOff();
    else { for (int i = 0; i < config.numStrips; i++) strips[i].turnOff(); }
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