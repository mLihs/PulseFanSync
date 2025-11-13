// ==== Version & GitHub-Konfiguration ========================================

#define FW_CURRENT_VERSION "1.0.2"  // aktuelle Firmware-Version auf dem Gerät

// URL zu deiner latest.json in GitHub (raw content)
const char* GITHUB_LATEST_URL =
  "https://raw.githubusercontent.com/mLihs/PulseFanSync/main/firmware/lastest.json";

// ==== Includes ==============================================================
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>

#define WM_NO_OTA          // WiFiManager: OTA im Captive-Portal deaktivieren (falls unterstützt)
#include <WiFiManager.h>   // von tzapu (Library Manager)

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// ==== Persistenz (Preferences / NVS) ========================================

Preferences prefs;
const char* PREF_NS    = "app";
const char* KEY_MIN_HR = "minHR";
const char* KEY_MAX_HR = "maxHR";
const char* KEY_TOKEN  = "token";

// ==== Globale Objekte =======================================================

WebServer   server(80);
WiFiManager wm;

// Konfiguration im RAM
uint16_t cfgMinHR = 120;
uint16_t cfgMaxHR = 180;
String   cfgToken = "";

// ==== HTML-Setup-Seite ======================================================

const char PAGE_SETUP[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Setup</title>
<style>
  body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial,sans-serif;margin:2rem;max-width:720px}
  h1{margin:0 0 1rem}
  h2{margin:.5rem 0 0.5rem;font-size:1.1rem}
  form{display:grid;gap:.75rem}
  label{font-weight:600}
  input[type=number],input[type=text]{padding:.5rem;border:1px solid #ccc;border-radius:6px;font-size:1rem}
  .row{display:grid;grid-template-columns:1fr 1fr;gap:.75rem}
  .btns{display:flex;gap:.75rem;margin-top:.5rem;flex-wrap:wrap}
  button,.btn{appearance:none;border:0;border-radius:8px;padding:.7rem 1rem;font-size:1rem;cursor:pointer}
  .primary{background:#2563eb;color:#fff}
  .ghost{background:#eef2ff;color:#111}
  .danger{background:#dc2626;color:#fff;margin-left:auto}
  .note{font-size:.9rem;color:#555}
  .card{background:#fafafa;border:1px solid #eee;border-radius:10px;padding:1rem;margin-top:1rem}
  code{background:#eee;padding:2px 4px;border-radius:4px;font-size:.9rem}
</style>
</head><body>
  <h1>ESP32 – Setup</h1>

  <div class="card">
    <p><b>Gerätestatus:</b> <span id="status">Lade…</span></p>
    <p class="note">
      Firmware-Version: <code id="fwver"></code><br>
      Aufrufbar unter <code>/setup</code>. Reset löscht WLAN & gespeicherte Werte.
    </p>
  </div>

  <form method="POST" action="/save" autocomplete="off">
    <div class="row">
      <div>
        <label for="minHR">Min Herzfrequenz</label>
        <input id="minHR" name="minHR" type="number" min="0" max="300" required value="{{MINHR}}">
      </div>
      <div>
        <label for="maxHR">Max Herzfrequenz</label>
        <input id="maxHR" name="maxHR" type="number" min="0" max="300" required value="{{MAXHR}}">
      </div>
    </div>

    <label for="token">UDP Token</label>
    <input id="token" name="token" type="text" maxlength="128" placeholder="z.B. 32-stellige Hex-Kette" value="{{TOKEN}}">

    <div class="card">
      <h2>Firmware-Update (GitHub)</h2>
      <p class="note">
        Der ESP prüft ein JSON-File (<code>latest.json</code>) auf GitHub.<br>
        Dort ist die aktuellste Version &amp; die Binär-URL hinterlegt.
      </p>
      <div class="btns">
        <button class="primary" type="submit">Einstellungen speichern</button>
        <a class="btn ghost" href="/setup">Verwerfen</a>
        <button class="ghost" form="checkForm" type="submit">Auf neue Version prüfen</button>
        <button class="ghost" form="fwForm" type="submit">Update von GitHub installieren</button>
        <button class="danger" form="resetForm" type="submit">Reset (WLAN & Daten)</button>
      </div>
    </div>
  </form>

  <form id="resetForm" method="POST" action="/reset"></form>
  <form id="checkForm" method="POST" action="/checkupdate"></form>
  <form id="fwForm" method="POST" action="/fwupdate"></form>

<script>
async function loadStatus(){
  try{
    const r = await fetch('/api/status');
    const j = await r.json();
    const s = document.getElementById('status');
    const v = document.getElementById('fwver');
    s.textContent = `WLAN: ${j.ssid||'-'} – IP: ${j.ip||'-'} – RSSI: ${j.rssi??'-'} dBm`;
    v.textContent = j.version || '-';
  }catch(e){
    console.error(e);
  }
}
loadStatus();
</script>
</body></html>
)HTML";

// ==== Hilfsfunktionen Konfiguration =========================================

void loadPrefs() {
  prefs.begin(PREF_NS, true);
  cfgMinHR = prefs.getUShort(KEY_MIN_HR, cfgMinHR);
  cfgMaxHR = prefs.getUShort(KEY_MAX_HR, cfgMaxHR);
  cfgToken = prefs.getString(KEY_TOKEN, cfgToken);
  prefs.end();
}

void savePrefs(uint16_t minHR, uint16_t maxHR, const String& token) {
  prefs.begin(PREF_NS, false);
  prefs.putUShort(KEY_MIN_HR, minHR);
  prefs.putUShort(KEY_MAX_HR, maxHR);
  prefs.putString(KEY_TOKEN, token);
  prefs.end();
}

void clearPrefs() {
  prefs.begin(PREF_NS, false);
  prefs.clear();
  prefs.end();
}

String htmlWithValues() {
  String html(PAGE_SETUP);
  html.replace("{{MINHR}}", String(cfgMinHR));
  html.replace("{{MAXHR}}", String(cfgMaxHR));
  html.replace("{{TOKEN}}", cfgToken);
  return html;
}

// ==== Versionsvergleich =====================================================

int cmpVersion(const String &a, const String &b) {
  int ma[3] = {0,0,0};
  int mb[3] = {0,0,0};
  sscanf(a.c_str(), "%d.%d.%d", &ma[0], &ma[1], &ma[2]);
  sscanf(b.c_str(), "%d.%d.%d", &mb[0], &mb[1], &mb[2]);
  for (int i = 0; i < 3; i++) {
    if (ma[i] != mb[i]) return ma[i] - mb[i];
  }
  return 0;
}

// ==== Firmware-Update von GitHub ============================================

bool performHttpFirmwareUpdate(const String& url) {
  if (url.isEmpty()) {
    Serial.println("FW-Update: Keine URL");
    return false;
  }

  Serial.println("FW-Update von: " + url);

  WiFiClientSecure client;
  client.setInsecure();  // GitHub TLS ohne Zertifikatsprüfung

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // wichtig für GitHub-Redirects

  Serial.println("[HTTP] begin...");
  if (!http.begin(client, url)) {
    Serial.println("[HTTP] http.begin() fehlgeschlagen");
    return false;
  }

  Serial.println("[HTTP] GET...");
  int httpCode = http.GET();
  Serial.printf("[HTTP] GET Code: %d\n", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] Fehler, Code=%d\n", httpCode);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  bool hasContentLength = contentLength > 0;

  if (hasContentLength) {
    Serial.printf("Content-Length: %d Bytes\n", contentLength);
  } else {
    Serial.println("Keine Content-Length (chunked oder unbekannt).");
  }

  WiFiClient *stream = http.getStreamPtr();

  // Wenn keine Content-Length bekannt ist, UPDATE_SIZE_UNKNOWN verwenden
  if (!Update.begin(hasContentLength ? contentLength : (int)UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("Update.begin() fehlgeschlagen, Error=%d\n", Update.getError());
    http.end();
    return false;
  }

  const size_t BUF_SIZE = 2048;
  uint8_t buff[BUF_SIZE];

  size_t totalRead = 0;
  int lastPercent = -1;

  Serial.println("Starte Download & Flash...");
  if (hasContentLength) {
    Serial.print("Fortschritt: 0%");
  } else {
    Serial.print("Fortschritt (Bytes): 0");
  }

  while (http.connected()) {
    // wenn nichts da ist, kurz warten
    if (!stream->available()) {
      if (!http.connected()) break;
      delay(1);
      continue;
    }

    size_t avail = stream->available();
    size_t toRead = avail;
    if (toRead > BUF_SIZE) toRead = BUF_SIZE;

    int c = stream->readBytes(buff, toRead);
    if (c <= 0) {
      Serial.println("\nLesefehler aus dem Stream");
      break;
    }

    if (Update.write(buff, c) != (size_t)c) {
      Serial.printf("\nUpdate.write Fehler: %d\n", Update.getError());
      http.end();
      return false;
    }

    totalRead += c;

    if (hasContentLength) {
      int percent = (int)((totalRead * 100) / contentLength);
      if (percent != lastPercent) {
        Serial.print("\rFortschritt: ");
        Serial.print(percent);
        Serial.print("%");
        lastPercent = percent;
      }
    } else {
      // Fallback: nur Byte-Zähler ausgeben
      if (totalRead % (16 * 1024) < BUF_SIZE) { // ungefähr alle 16KB
        Serial.print("\rFortschritt (Bytes): ");
        Serial.print(totalRead);
      }
    }

    // Abbruch, falls der Server nichts mehr liefert
    if (hasContentLength && totalRead >= (size_t)contentLength) {
      break;
    }
  }

  Serial.println();

  if (hasContentLength && totalRead != (size_t)contentLength) {
    Serial.printf("Nur %u von %d Bytes gelesen\n", (unsigned)totalRead, contentLength);
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("Update.end Fehler: %d\n", Update.getError());
    http.end();
    return false;
  }

  http.end();

  if (!Update.isFinished()) {
    Serial.println("Update nicht fertig");
    return false;
  }

  Serial.println("Update erfolgreich – Neustart...");
  delay(500);
  ESP.restart();
  return true;  // praktisch nie erreicht
}



// Holt latest.json von GitHub, vergleicht Version, optional Auto-Install
bool checkGithubForUpdate(bool autoInstall) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, GITHUB_LATEST_URL)) {
    Serial.println("latest.json: http.begin fehlgeschlagen");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("latest.json: HTTP Fehler %d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  Serial.println("latest.json:\n" + payload);

  StaticJsonDocument<768> doc;
  auto err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON-Fehler: ");
    Serial.println(err.c_str());
    return false;
  }

  String remoteVer = doc["version"] | "";
  String fwUrl     = doc["url"]     | "";
  String notes     = doc["notes"]   | "";

  if (remoteVer.length() == 0 || fwUrl.length() == 0) {
    Serial.println("Ungültige latest.json");
    return false;
  }

  Serial.printf("Aktuell: %s, Remote: %s\n", FW_CURRENT_VERSION, remoteVer.c_str());
  if (notes.length() > 0) {
    Serial.println("Release-Notes: " + notes);
  }

  int cmp = cmpVersion(remoteVer, FW_CURRENT_VERSION);
  if (cmp <= 0) {
    Serial.println("Keine neuere Version verfügbar.");
    return false;
  }

  Serial.println("Neue Version gefunden!");

  if (autoInstall) {
    return performHttpFirmwareUpdate(fwUrl);
  } else {
    return true;
  }
}

// ==== HTTP-Handler ==========================================================

void handleRoot() {
  server.sendHeader("Location", "/setup", true);
  server.send(302, "text/plain", "");
}

void handleSetup() {
  server.send(200, "text/html; charset=utf-8", htmlWithValues());
}

void handleSave() {
  if (!server.hasArg("minHR") || !server.hasArg("maxHR") || !server.hasArg("token")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }

  uint16_t minHR = (uint16_t) constrain(server.arg("minHR").toInt(), 0, 300);
  uint16_t maxHR = (uint16_t) constrain(server.arg("maxHR").toInt(), 0, 300);
  String token   = server.arg("token");
  token.trim();

  if (minHR > maxHR) {
    uint16_t t = minHR; minHR = maxHR; maxHR = t;
  }

  savePrefs(minHR, maxHR, token);
  cfgMinHR = minHR;
  cfgMaxHR = maxHR;
  cfgToken = token;

  server.sendHeader("Location", "/setup", true);
  server.send(302, "text/plain", "Saved");
}

void handleReset() {
  clearPrefs();
  wm.resetSettings();            // WiFi-Creds löschen
  WiFi.disconnect(true, true);   // Sicherheitshalber
  delay(200);
  server.send(200, "text/plain", "Reset ok. Neustart...");
  delay(400);
  ESP.restart();
}

void handleStatus() {
  String ssid = WiFi.SSID();
  String ip   = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  int32_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;

  String json = "{";
  json += "\"ssid\":\""   + ssid + "\",";
  json += "\"ip\":\""     + ip   + "\",";
  json += "\"rssi\":"     + String(rssi) + ",";
  json += "\"minHR\":"    + String(cfgMinHR) + ",";
  json += "\"maxHR\":"    + String(cfgMaxHR) + ",";
  json += "\"token\":\""  + cfgToken + "\",";
  json += "\"version\":\"" + String(FW_CURRENT_VERSION) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleCheckUpdate() {
  bool available = checkGithubForUpdate(false);
  if (available) {
    server.send(200, "text/plain",
      "Neue Firmware-Version ist verfügbar.\n"
      "Klicke auf 'Update von GitHub installieren', um zu aktualisieren.");
  } else {
    server.send(200, "text/plain", "Keine neuere Version gefunden.");
  }
}

void handleFwUpdateGithub() {
  server.send(200, "text/plain",
              "Update von GitHub wird gestartet.\n"
              "Bitte Gerät nicht ausschalten.\n"
              "Bei Erfolg startet der ESP neu.");
  checkGithubForUpdate(true);   // lädt & installiert
}

// ==== Start Webserver & WiFi ================================================

void startWeb() {
  if (MDNS.begin("esp32-setup")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://esp32-setup.local/");
  }

  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/setup",     HTTP_GET,  handleSetup);
  server.on("/save",      HTTP_POST, handleSave);
  server.on("/reset",     HTTP_POST, handleReset);
  server.on("/api/status",HTTP_GET,  handleStatus);
  server.on("/checkupdate",HTTP_POST,handleCheckUpdate);
  server.on("/fwupdate",  HTTP_POST, handleFwUpdateGithub);

  server.begin();
  Serial.println("Webserver gestartet: /setup");
}

void startWifi() {
  wm.setConfigPortalTimeout(180); // 3 Minuten Captive-Portal

  bool ok = wm.autoConnect("ESP32-Setup", "esp32setup");
  if (!ok) {
    Serial.println("Keine WLAN-Verbindung. Neustart...");
    delay(2000);
    ESP.restart();
  } else {
    Serial.printf("Verbunden: %s  IP: %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
  }
}

// ==== setup & loop ==========================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  loadPrefs();
  startWifi();
  startWeb();

  // Optional: beim Start nur checken (log), nicht automatisch updaten
  // checkGithubForUpdate(false);
}

void loop() {
  server.handleClient();
}
