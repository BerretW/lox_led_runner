#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ==========================================
// ========== HARDWARE NASTAVENÍ ============
// ==========================================
#define MAX_STRIPS 4
#define CONFIG_VERSION 4
// Piny pro pásky. Na NodeMCU/Wemos to je: 5(D1), 4(D2), 14(D5), 12(D6)
const int STRIP_PINS[MAX_STRIPS] = {0, 2, 4, 5}; 

// ==========================================
// ====== STRUKTURA PRO ULOŽENÍ DO EEPROM ===
// ==========================================
struct Config {
  char magic[4];
  uint16_t version;
  char ssid[32];
  char password[64];
  char moduleName[32];
  bool useDhcp;
  char staticIp[16];
  char gateway[16];
  char subnet[16];
  char dns1[16];
  int numStrips;
  int numPixels[MAX_STRIPS];
  int fillSpeedMs[MAX_STRIPS];
  uint8_t maxBrightness;
  uint16_t checksum;
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
  uint8_t maxBrightness = 255;
  uint8_t colorLevel = 255;
  unsigned long lastShowMillis = 0;

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

  uint8_t scaleLevel(uint8_t level) const {
    return (uint8_t)(((uint16_t)level * (uint16_t)maxBrightness) / 255U);
  }

  uint32_t whiteColor(uint8_t level) const {
    if (pixels == nullptr) return 0;
    uint8_t scaled = scaleLevel(level);
    return pixels->Color(scaled, scaled, scaled);
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
    if (numPixels <= 0 || pixels == nullptr) return;

    unsigned long now = millis();

    if (state == STATE_FILLING) {
      unsigned long elapsed = now - fillStartMillis;
      unsigned long totalTime = (unsigned long)numPixels * fillSpeedMs;

      if (elapsed >= totalTime) {
        for (int i = 0; i < numPixels; i++)
          pixels->setPixelColor(i, whiteColor(colorLevel));
        yield();
        pixels->show();
        lastShowMillis = now;
        state = STATE_ON;
      } else {
        if (now - lastShowMillis < 20) return;

        float progress = (float)elapsed / fillSpeedMs;
        int currentPix = (int)progress;
        float fraction = progress - currentPix;

        if (currentPix < 0) currentPix = 0;
        if (currentPix > numPixels) currentPix = numPixels;

        int partialIdx = currentPix;
        uint32_t fullColor = whiteColor(colorLevel);
        uint8_t partialLevel = (uint8_t)(colorLevel * fraction);

        for (int i = 0; i < numPixels; i++) {
          if (i < partialIdx) {
            pixels->setPixelColor(i, fullColor);
          } else if (i == partialIdx && partialIdx < numPixels) {
            pixels->setPixelColor(i, whiteColor(partialLevel));
          } else {
            pixels->setPixelColor(i, 0);
          }
        }

        lastLitPixel = currentPix;
        yield();
        pixels->show();
        lastShowMillis = now;
      }
    }
  }
};

NeoStrip strips[MAX_STRIPS];

uint16_t calculateConfigChecksum(const Config& value) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&value);
  const size_t length = offsetof(Config, checksum);
  uint16_t checksum = 0xA5A5;

  for (size_t i = 0; i < length; i++) {
    checksum = (uint16_t)((checksum << 5) | (checksum >> 11));
    checksum ^= data[i];
  }

  return checksum;
}

void applyDefaultConfig(Config& value) {
  memset(&value, 0, sizeof(value));
  strncpy(value.magic, "CFG", sizeof(value.magic));
  value.version = CONFIG_VERSION;
  strncpy(value.moduleName, "ESP8266_Modul", sizeof(value.moduleName) - 1);
  value.useDhcp = true;
  strncpy(value.staticIp, "192.168.1.50", sizeof(value.staticIp) - 1);
  strncpy(value.gateway, "192.168.1.1", sizeof(value.gateway) - 1);
  strncpy(value.subnet, "255.255.255.0", sizeof(value.subnet) - 1);
  strncpy(value.dns1, "192.168.1.1", sizeof(value.dns1) - 1);
  value.numStrips = 1;
  value.numPixels[0] = 60;
  for (int i = 0; i < MAX_STRIPS; i++) {
    value.fillSpeedMs[i] = 30;
  }
  value.maxBrightness = 255;
}

bool isValidIpString(const char* value) {
  IPAddress ip;
  return ip.fromString(value);
}

bool sanitizeConfig(Config& value) {
  bool changed = false;

  if (strncmp(value.magic, "CFG", sizeof(value.magic)) != 0) {
    strncpy(value.magic, "CFG", sizeof(value.magic));
    changed = true;
  }
  if (value.version != CONFIG_VERSION) {
    value.version = CONFIG_VERSION;
    changed = true;
  }

  value.ssid[sizeof(value.ssid) - 1] = '\0';
  value.password[sizeof(value.password) - 1] = '\0';
  value.moduleName[sizeof(value.moduleName) - 1] = '\0';
  value.staticIp[sizeof(value.staticIp) - 1] = '\0';
  value.gateway[sizeof(value.gateway) - 1] = '\0';
  value.subnet[sizeof(value.subnet) - 1] = '\0';
  value.dns1[sizeof(value.dns1) - 1] = '\0';

  if (strlen(value.moduleName) == 0) {
    strncpy(value.moduleName, "ESP8266_Modul", sizeof(value.moduleName) - 1);
    changed = true;
  }
  if (!value.useDhcp) {
    if (!isValidIpString(value.staticIp)) {
      strncpy(value.staticIp, "192.168.1.50", sizeof(value.staticIp) - 1);
      value.staticIp[sizeof(value.staticIp) - 1] = '\0';
      changed = true;
    }
    if (!isValidIpString(value.gateway)) {
      strncpy(value.gateway, "192.168.1.1", sizeof(value.gateway) - 1);
      value.gateway[sizeof(value.gateway) - 1] = '\0';
      changed = true;
    }
    if (!isValidIpString(value.subnet)) {
      strncpy(value.subnet, "255.255.255.0", sizeof(value.subnet) - 1);
      value.subnet[sizeof(value.subnet) - 1] = '\0';
      changed = true;
    }
    if (!isValidIpString(value.dns1)) {
      strncpy(value.dns1, "192.168.1.1", sizeof(value.dns1) - 1);
      value.dns1[sizeof(value.dns1) - 1] = '\0';
      changed = true;
    }
  }
  if (value.numStrips < 1 || value.numStrips > MAX_STRIPS) {
    value.numStrips = 1;
    changed = true;
  }
  if (value.maxBrightness < 1 || value.maxBrightness > 255) {
    value.maxBrightness = 255;
    changed = true;
  }

  for (int i = 0; i < MAX_STRIPS; i++) {
    int clampedPixels = value.numPixels[i];
    int clampedSpeed = value.fillSpeedMs[i];

    if (clampedPixels < 0) clampedPixels = 0;
    if (clampedPixels > 1000) clampedPixels = 1000;
    if (clampedSpeed < 1) clampedSpeed = 30;
    if (clampedSpeed > 500) clampedSpeed = 500;

    if (value.numPixels[i] != clampedPixels) {
      value.numPixels[i] = clampedPixels;
      changed = true;
    }
    if (value.fillSpeedMs[i] != clampedSpeed) {
      value.fillSpeedMs[i] = clampedSpeed;
      changed = true;
    }
  }

  return changed;
}

bool isConfigValid(const Config& value) {
  if (strncmp(value.magic, "CFG", sizeof(value.magic)) != 0) return false;
  if (value.version != CONFIG_VERSION) return false;
  return value.checksum == calculateConfigChecksum(value);
}

bool loadConfig() {
  EEPROM.get(0, config);

  if (!isConfigValid(config)) {
    applyDefaultConfig(config);
    return false;
  }

  return true;
}

void saveConfig() {
  sanitizeConfig(config);
  config.checksum = calculateConfigChecksum(config);
  EEPROM.put(0, config);
  EEPROM.commit();
}

void resetConfig() {
  Config emptyConfig = {};
  EEPROM.put(0, emptyConfig);
  EEPROM.commit();
}

// WiFi scan se provede jednou při startu a výsledek se cachuje.
// Opakované skenování uvnitř HTTP handleru by blokovalo smyčku a mohlo crashnout WDT.
String cachedWifiOptions;
int cachedWifiCount = 0;

void buildWifiOptions() {
  int n = WiFi.scanNetworks();
  cachedWifiOptions = "";
  cachedWifiCount = (n > 0) ? n : 0;
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

bool applyWifiNetworkConfig() {
  if (config.useDhcp) {
    return WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  }

  IPAddress localIp;
  IPAddress gatewayIp;
  IPAddress subnetMask;
  IPAddress dnsServerIp;

  if (!localIp.fromString(config.staticIp)) return false;
  if (!gatewayIp.fromString(config.gateway)) return false;
  if (!subnetMask.fromString(config.subnet)) return false;
  if (!dnsServerIp.fromString(config.dns1)) return false;

  return WiFi.config(localIp, gatewayIp, subnetMask, dnsServerIp);
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

      function toggleStaticIpFields() {
        var useDhcp = document.getElementById('useDhcp');
        var staticWrap = document.getElementById('static-ip-fields');
        if (!useDhcp || !staticWrap) return;
        staticWrap.style.display = useDhcp.checked ? 'none' : 'block';
      }

      window.addEventListener('load', toggleStaticIpFields);
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
        )rawliteral";

  if (cachedWifiOptions.length() > 0) {
    html += "<select style='width:100%;padding:10px;margin-top:5px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;background:#fff;font-size:15px;' "
            "onchange=\"if(this.value){document.querySelector('[name=ssid]').value=this.value;} this.selectedIndex=0;\">"
            "<option value=''>-- Vybrat ze seznamu (" + String(cachedWifiCount) + " siti) --</option>";
    html += cachedWifiOptions;
    html += "</select>";
  } else {
    html += "<p style='color:#999;font-size:13px;margin:4px 0 6px;'>Zadna sit nenalezena (restartujte pro obnovu seznamu)</p>";
  }

  html += R"rawliteral(
        <input type="text" name="ssid" maxlength="31" placeholder="SSID sit" value=")rawliteral" + String(config.ssid) + R"rawliteral(">
        
        <label>WiFi Heslo:</label>
        <input type="password" name="pass" maxlength="63" placeholder="(Beze změny nechte prázdné)">

        <div style='margin-top:18px; padding:12px; background:#f7f7f7; border-radius:6px;'>
          <label for='useDhcp' style='margin:0; cursor:pointer;'>
            <input type='checkbox' id='useDhcp' name='useDhcp' value='1' onchange='toggleStaticIpFields()')rawliteral" + String(config.useDhcp ? " checked" : "") + R"rawliteral( style='width:18px;height:18px;vertical-align:middle;margin-right:8px;'>
            Získat IP adresu z DHCP
          </label>
          <span class='note'>Pokud checkbox vypnete, modul použije pevnou IP adresu.</span>
        </div>

        <div id='static-ip-fields' style='margin-top:12px;'>
          <label>Pevná IP adresa:</label>
          <input type="text" name="staticIp" maxlength="15" placeholder="192.168.1.50" value=")rawliteral" + String(config.staticIp) + R"rawliteral(">

          <label>Brána:</label>
          <input type="text" name="gateway" maxlength="15" placeholder="192.168.1.1" value=")rawliteral" + String(config.gateway) + R"rawliteral(">

          <label>Maska sítě:</label>
          <input type="text" name="subnet" maxlength="15" placeholder="255.255.255.0" value=")rawliteral" + String(config.subnet) + R"rawliteral(">

          <label>DNS server:</label>
          <input type="text" name="dns1" maxlength="15" placeholder="192.168.1.1" value=")rawliteral" + String(config.dns1) + R"rawliteral(">
        </div>
        
        <label>Počet aktivních LED pásků (1-4):</label>
        <input type="number" name="numStrips" min="1" max="4" value=")rawliteral" + String(config.numStrips) + R"rawliteral(">

          <label>Maximální jas (1-255):<span class='note'>255 = plný jas, nižší hodnota omezí svit všech pásků.</span></label>
          <input type="number" name="maxBrightness" min="1" max="255" value=")rawliteral" + String(config.maxBrightness) + R"rawliteral(">
  )rawliteral";

  for (int i = 0; i < MAX_STRIPS; i++) {
    html += "<label>Počet LED - Pásek " + String(i + 1) + " (GPIO" + String(STRIP_PINS[i]) + "):</label>";
    html += "<input type='number' name='led" + String(i) + "' min='0' max='1000' value='" + String(config.numPixels[i]) + "'>";
    html += "<label>Rychlost rozsvícení - Pásek " + String(i + 1) + " (ms/LED):<span class='note'>Nižší = rychlejší. Výchozí: 30</span></label>";
    html += "<input type='number' name='spd" + String(i) + "' min='1' max='500' value='" + String(config.fillSpeedMs[i] > 0 ? config.fillSpeedMs[i] : 30) + "'>";
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
// =========== SÉRIOVÉ PŘÍKAZY ==============
// ==========================================
String serialBuf;

void printSerialHelp() {
  Serial.println(F("Prikazy (115200 baud, LF nebo CR+LF):"));
  Serial.println(F("  on [1-4]   zapnout pasek (bez cisla = vse)"));
  Serial.println(F("  off [1-4]  vypnout pasek (bez cisla = vse)"));
  Serial.println(F("  status     stav vsech pasku"));
  Serial.println(F("  info       konfigurace a sit"));
  Serial.println(F("  restart    restart ESP"));
  Serial.println(F("  reset      smazat EEPROM a restart"));
}

void processSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  int spaceIdx = cmd.indexOf(' ');
  String verb = (spaceIdx >= 0) ? cmd.substring(0, spaceIdx) : cmd;
  String arg  = (spaceIdx >= 0) ? cmd.substring(spaceIdx + 1) : String("");
  arg.trim();
  verb.toLowerCase();

  if (verb == "help") {
    printSerialHelp();

  } else if (verb == "on" || verb == "off") {
    bool turningOn = (verb == "on");
    int id = arg.length() > 0 ? arg.toInt() : 0;
    int count = min(config.numStrips, (int)MAX_STRIPS);

    if (id > 0 && id <= MAX_STRIPS) {
      turningOn ? strips[id - 1].turnOn() : strips[id - 1].turnOff();
      Serial.print(F("Pasek ")); Serial.print(id);
      Serial.println(turningOn ? F(": ON") : F(": OFF"));
    } else if (id == 0) {
      for (int i = 0; i < count; i++)
        turningOn ? strips[i].turnOn() : strips[i].turnOff();
      Serial.println(turningOn ? F("Vsechny pasky: ON") : F("Vsechny pasky: OFF"));
    } else {
      Serial.println(F("Chyba: ID pasku 1-4"));
    }

  } else if (verb == "status") {
    int count = min(config.numStrips, (int)MAX_STRIPS);
    for (int i = 0; i < count; i++) {
      Serial.print(F("Pasek ")); Serial.print(i + 1); Serial.print(F(": "));
      switch (strips[i].state) {
        case STATE_OFF:       Serial.println(F("OFF")); break;
        case STATE_ON:        Serial.println(F("ON")); break;
        case STATE_FILLING:   Serial.println(F("Rozsviuji...")); break;
      }
    }

  } else if (verb == "info") {
    Serial.println(F("=== Konfigurace ==="));
    Serial.print(F("Modul: "));    Serial.println(config.moduleName);
    Serial.print(F("SSID: "));     Serial.println(config.ssid);
    Serial.print(F("Rezim: "));    Serial.println(apMode ? F("AP") : F("STA"));
    Serial.print(F("Sit: "));      Serial.println(config.useDhcp ? F("DHCP") : F("Pevna IP"));
    if (!config.useDhcp) {
      Serial.print(F("Static IP: ")); Serial.println(config.staticIp);
      Serial.print(F("Brana: "));     Serial.println(config.gateway);
      Serial.print(F("Maska: "));     Serial.println(config.subnet);
      Serial.print(F("DNS: "));       Serial.println(config.dns1);
    }
    Serial.print(F("IP: "));       Serial.println(apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
    Serial.print(F("Pasky: "));    Serial.println(config.numStrips);
    for (int i = 0; i < min(config.numStrips, (int)MAX_STRIPS); i++) {
      Serial.print(F("  Pasek ")); Serial.print(i + 1);
      Serial.print(F(": "));       Serial.print(config.numPixels[i]);
      Serial.print(F(" LED, "));   Serial.print(config.fillSpeedMs[i]);
      Serial.println(F(" ms/LED"));
    }
    Serial.print(F("Max jas: "));     Serial.println(config.maxBrightness);
    Serial.print(F("Volna heap: ")); Serial.print(ESP.getFreeHeap()); Serial.println(F(" B"));

  } else if (verb == "restart") {
    Serial.println(F("Restartuji..."));
    delay(200);
    ESP.restart();

  } else if (verb == "reset") {
    Serial.println(F("Mazu EEPROM a restartuji..."));
    resetConfig();
    delay(200);
    ESP.restart();

  } else {
    Serial.print(F("Neznamy prikaz: ")); Serial.println(cmd);
    Serial.println(F("Napiste 'help'"));
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.length() > 0) {
        processSerialCommand(serialBuf);
        serialBuf = "";
      }
    } else if (serialBuf.length() < 64) {
      serialBuf += c;
    }
  }
}

// ==========================================
// ============== SETUP & LOOP ==============
// ==========================================
void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  delay(500);
  Serial.println(F("\n=== LED Runner ==="));
  printSerialHelp();

  bool validConfig = loadConfig();
  bool configChanged = sanitizeConfig(config);
  if (!validConfig || configChanged) saveConfig();
  
  WiFi.hostname(config.moduleName);
  
  for (int i = 0; i < MAX_STRIPS; i++) {
    strips[i].maxBrightness = config.maxBrightness;
    strips[i].init(STRIP_PINS[i], config.numPixels[i]);
    strips[i].fillSpeedMs = config.fillSpeedMs[i];
  }

  WiFi.mode(WIFI_STA);
  applyWifiNetworkConfig();
  delay(150);
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
    config.useDhcp = server.hasArg("useDhcp");
    if (server.hasArg("staticIp")) {
      strncpy(config.staticIp, server.arg("staticIp").c_str(), sizeof(config.staticIp) - 1);
      config.staticIp[sizeof(config.staticIp) - 1] = '\0';
    }
    if (server.hasArg("gateway")) {
      strncpy(config.gateway, server.arg("gateway").c_str(), sizeof(config.gateway) - 1);
      config.gateway[sizeof(config.gateway) - 1] = '\0';
    }
    if (server.hasArg("subnet")) {
      strncpy(config.subnet, server.arg("subnet").c_str(), sizeof(config.subnet) - 1);
      config.subnet[sizeof(config.subnet) - 1] = '\0';
    }
    if (server.hasArg("dns1")) {
      strncpy(config.dns1, server.arg("dns1").c_str(), sizeof(config.dns1) - 1);
      config.dns1[sizeof(config.dns1) - 1] = '\0';
    }
    if (server.hasArg("numStrips")) {
      int ns = server.arg("numStrips").toInt();
      config.numStrips = (ns < 1) ? 1 : (ns > MAX_STRIPS) ? MAX_STRIPS : ns;
    }
    if (server.hasArg("maxBrightness")) {
      int brightness = server.arg("maxBrightness").toInt();
      config.maxBrightness = (brightness < 1) ? 1 : (brightness > 255) ? 255 : brightness;
    }
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
  handleSerial();
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  for (int i = 0; i < MAX_STRIPS; i++) strips[i].update();
}