// #include <Arduino.h>

// // put function declarations here:
// int myFunction(int, int);

// void setup() {
//   // put your setup code here, to run once:
//   int result = myFunction(2, 3);
// }

// void loop() {
//   // put your main code here, to run repeatedly:
// }

// // put function definitions here:
// int myFunction(int x, int y) {
//   return x + y;
// }

/*
 * ============================================================
 *  LabLink Server — ESP32 DevKit V1 Firmware
 *  COAL Lab, University Project
 * ============================================================
 *  Features:
 *    • Dual-mode WiFi: AP (fast local hotspot) + STA (router/eduroam)
 *    • Async HTTP server on port 80 (AsyncTCP + ESPAsyncWebServer)
 *    • SD Card file storage (SPI, FAT32)
 *    • RTC DS3231 timestamps on every upload
 *    • Captive-portal DNS so users just type "lab.local"
 *    • OLED SSD1306 128×64 status display
 *    • 4 LEDs: STA, AP, Upload, Buffer-Full
 *    • Dynamic config stored in /config.json on SD
 *    • Admin PIN stored in /admin_pass.txt on SD
 *    • Upload timestamps stored in /uploadTime.json on SD
 * ============================================================
 *  Required Libraries (install via PlatformIO / Arduino IDE):
 *    AsyncTCP, ESPAsyncWebServer, SD, RTClib,
 *    Adafruit_GFX, Adafruit_SSD1306, DNSServer (bundled in core)
 *    ESP32 core ≥ 2.x (for esp_wpa2.h)
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_wpa2.h"   // WPA2-Enterprise (Eduroam)

// ─────────────────────────────────────────────
//  PIN ASSIGNMENTS
// ─────────────────────────────────────────────
#define SD_CS        5    // SD Card Chip Select (SPI)
#define LED_STA     13    // Green  — STA Wi-Fi connected
#define LED_AP      14    // Blue   — AP hotspot active
#define LED_UPLOAD  12    // Yellow — file transfer in progress
#define LED_BUFFER  27    // Red    — max clients reached

// ─────────────────────────────────────────────
//  OLED
// ─────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_ADDR    0x3C

// ─────────────────────────────────────────────
//  SERVER / DNS
// ─────────────────────────────────────────────
#define DNS_PORT     53
#define LOCAL_DOMAIN "lab.local"
#define MAX_CLIENTS   4     // Max simultaneous AP clients

// ─────────────────────────────────────────────
//  GLOBAL OBJECTS
// ─────────────────────────────────────────────
AsyncWebServer server(80);
DNSServer      dnsServer;
RTC_DS3231     rtc;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ─────────────────────────────────────────────
//  RUNTIME STATE
// ─────────────────────────────────────────────
volatile bool uploadInProgress = false;
bool          staConnected     = false;
bool          apActive         = false;

// ─────────────────────────────────────────────
//  CONFIG STRUCT
// ─────────────────────────────────────────────
struct Config {
  String ap_ssid;
  String ap_pass;
  String sta_type;    // "standard" | "enterprise"
  String sta_ssid;
  String sta_email;   // Used as identity for Eduroam
  String sta_pass;
};
Config cfg;

// ─────────────────────────────────────────────
//  SD HELPERS
// ─────────────────────────────────────────────

/** Read entire file from SD into a String. Returns "" on failure. */
String sdRead(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

/** Write a String to SD, overwriting any existing content. */
bool sdWrite(const char* path, const String& content) {
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();
  return true;
}

/** Extract a JSON string value: "key":"value" */
String jsonGet(const String& json, const String& key) {
  String search = "\"" + key + "\":\"";
  int s = json.indexOf(search);
  if (s < 0) return "";
  s += search.length();
  int e = json.indexOf('"', s);
  return (e < 0) ? "" : json.substring(s, e);
}

/** Strip leading directory path from SD filename */
String stripPath(const String& fullName) {
  int slash = fullName.lastIndexOf('/');
  return (slash >= 0) ? fullName.substring(slash + 1) : fullName;
}

// ─────────────────────────────────────────────
//  CONFIG LOAD / SAVE
// ─────────────────────────────────────────────

void loadConfig() {
  String raw = sdRead("/config.json");
  if (raw.isEmpty()) {
    cfg = { "COAL_Lab_Server", "lab12345", "standard", "", "", "" };
    return;
  }
  cfg.ap_ssid  = jsonGet(raw, "ap_ssid");
  cfg.ap_pass  = jsonGet(raw, "ap_pass");
  cfg.sta_type = jsonGet(raw, "sta_type");
  cfg.sta_ssid = jsonGet(raw, "sta_ssid");
  cfg.sta_email= jsonGet(raw, "sta_email");
  cfg.sta_pass = jsonGet(raw, "sta_pass");
}

void saveConfig() {
  String j  = "{\n";
  j += "  \"ap_ssid\": \""  + cfg.ap_ssid  + "\",\n";
  j += "  \"ap_pass\": \""  + cfg.ap_pass  + "\",\n";
  j += "  \"sta_type\": \"" + cfg.sta_type + "\",\n";
  j += "  \"sta_ssid\": \"" + cfg.sta_ssid + "\",\n";
  j += "  \"sta_email\": \""+ cfg.sta_email+ "\",\n";
  j += "  \"sta_pass\": \"" + cfg.sta_pass + "\"\n}";
  sdWrite("/config.json", j);
}

// ─────────────────────────────────────────────
//  ADMIN PASSWORD
// ─────────────────────────────────────────────

String getAdminPass() {
  String p = sdRead("/admin_pass.txt");
  p.trim();
  return p;
}

// ─────────────────────────────────────────────
//  RTC + UPLOAD-TIME JSON
// ─────────────────────────────────────────────

/** Format DateTime → "hh:mm AM/PM - DD Mon YYYY" */
String formatTime(const DateTime& dt) {
  const char* months[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  int h12  = dt.hour() % 12;
  if (h12 == 0) h12 = 12;
  const char* ampm = (dt.hour() < 12) ? "AM" : "PM";
  char buf[40];
  snprintf(buf, sizeof(buf), "%02d:%02d %s - %02d %s %04d",
           h12, dt.minute(), ampm,
           dt.day(), months[dt.month() - 1], dt.year());
  return String(buf);
}

/** Read uploadTime.json; returns "{}" array or "[]" if missing */
String readUploadTimes() {
  String raw = sdRead("/uploadTime.json");
  raw.trim();
  return (raw.isEmpty()) ? "[]" : raw;
}

/** Append a new entry {name, time} to uploadTime.json */
void recordUploadTime(const String& filename) {
  DateTime now = rtc.now();
  String   ts  = formatTime(now);

  String raw = readUploadTimes();
  raw.trim();
  // Pop trailing ']'
  if (raw.endsWith("]")) raw = raw.substring(0, raw.length() - 1);

  String entry = "{\"name\":\"" + filename + "\",\"time\":\"" + ts + "\"}";
  if (raw == "[") {
    raw += entry + "]";
  } else {
    raw += "," + entry + "]";
  }
  sdWrite("/uploadTime.json", raw);
}

/** Remove a file's entry from uploadTime.json */
void removeUploadTime(const String& filename) {
  String raw = readUploadTimes();
  String rebuilt = "[";
  bool   first   = true;
  int    pos     = 0;

  while (pos < (int)raw.length()) {
    int objStart = raw.indexOf("{\"name\":\"", pos);
    if (objStart < 0) break;
    int nameStart = objStart + 9;
    int nameEnd   = raw.indexOf('"', nameStart);
    String name   = raw.substring(nameStart, nameEnd);
    int objEnd    = raw.indexOf('}', objStart) + 1;

    if (name != filename) {
      if (!first) rebuilt += ",";
      rebuilt += raw.substring(objStart, objEnd);
      first = false;
    }
    pos = objEnd;
  }
  rebuilt += "]";
  sdWrite("/uploadTime.json", rebuilt);
}

/** Find the upload time string for a given filename */
String getTimeForFile(const String& times, const String& filename) {
  String search = "\"name\":\"" + filename + "\"";
  int pos = times.indexOf(search);
  if (pos < 0) return "Unknown";
  int ts = times.indexOf("\"time\":\"", pos) + 8;
  int te = times.indexOf('"', ts);
  if (ts < 8 || te < 0) return "Unknown";
  return times.substring(ts, te);
}

// ─────────────────────────────────────────────
//  OLED + LED
// ─────────────────────────────────────────────

void updateOLED() {
  int clients = WiFi.softAPgetStationNum();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Row 0 — title bar
  display.fillRect(0, 0, 128, 10, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(28, 1);
  display.print("LabLink Server");
  display.setTextColor(WHITE);

  // Row 1 — AP status
  display.setCursor(0, 13);
  display.print("AP: ");
  display.print(apActive ? cfg.ap_ssid : "OFF");

  // Row 2 — STA status
  display.setCursor(0, 23);
  display.print("STA: ");
  display.print(staConnected ? WiFi.localIP().toString() : "Disconnected");

  // Row 3 — Clients
  display.setCursor(0, 33);
  display.print("Clients: ");
  display.print(clients);
  display.print("/");
  display.print(MAX_CLIENTS);

  // Row 4 — Domain / local IP hint
  display.setCursor(0, 43);
  display.print("http://");
  display.print(LOCAL_DOMAIN);

  // Row 5 — Upload indicator
  if (uploadInProgress) {
    display.setCursor(0, 53);
    display.print(">> Uploading file...");
  }

  display.display();
}

void updateLEDs() {
  int clients = WiFi.softAPgetStationNum();
  digitalWrite(LED_STA,    staConnected ? HIGH : LOW);
  digitalWrite(LED_AP,     apActive     ? HIGH : LOW);
  digitalWrite(LED_UPLOAD, uploadInProgress ? HIGH : LOW);
  digitalWrite(LED_BUFFER, (clients >= MAX_CLIENTS) ? HIGH : LOW);
}

// ─────────────────────────────────────────────
//  WI-FI SETUP
// ─────────────────────────────────────────────

void startWiFi() {
  WiFi.mode(WIFI_AP_STA);

  // ── AP Mode ──────────────────────────────
  WiFi.softAP(cfg.ap_ssid.c_str(), cfg.ap_pass.c_str(),
              /*channel*/1, /*hidden*/0, MAX_CLIENTS);
  apActive = true;
  Serial.printf("[AP] SSID: %s  IP: %s\n",
    cfg.ap_ssid.c_str(), WiFi.softAPIP().toString().c_str());

  // ── STA Mode ─────────────────────────────
  if (cfg.sta_ssid.isEmpty()) {
    Serial.println("[STA] No SSID configured — AP only.");
    return;
  }

  if (cfg.sta_type == "enterprise") {
    // WPA2-Enterprise (Eduroam)
    WiFi.disconnect(true);
    esp_wifi_sta_wpa2_ent_set_identity(
      (uint8_t*)cfg.sta_email.c_str(), cfg.sta_email.length());
    esp_wifi_sta_wpa2_ent_set_username(
      (uint8_t*)cfg.sta_email.c_str(), cfg.sta_email.length());
    esp_wifi_sta_wpa2_ent_set_password(
      (uint8_t*)cfg.sta_pass.c_str(), cfg.sta_pass.length());
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(cfg.sta_ssid.c_str());
    Serial.printf("[STA] Connecting to Eduroam: %s\n", cfg.sta_ssid.c_str());
  } else {
    // Standard WPA2
    WiFi.begin(cfg.sta_ssid.c_str(), cfg.sta_pass.c_str());
    Serial.printf("[STA] Connecting to: %s\n", cfg.sta_ssid.c_str());
  }

  // Wait up to 10 s for STA
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  staConnected = (WiFi.status() == WL_CONNECTED);
  if (staConnected) {
    Serial.printf("[STA] Connected! IP: %s\n",
      WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[STA] Failed — running AP-only mode.");
  }
}

// ─────────────────────────────────────────────
//  WEB SERVER ROUTES
// ─────────────────────────────────────────────

// Shared body buffer for /api/save-config (one request at a time)
static String configBodyBuf = "";

void setupRoutes() {

  // ── Static files from SD ─────────────────────────────────────────────────
  // Serves /index.html as the default page from root
  server.serveStatic("/", SD, "/").setDefaultFile("index.html");
  // Allow direct download of uploaded files
  server.serveStatic("/Uploads/", SD, "/Uploads/");

  // ── GET /api/status ───────────────────────────────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    int clients = WiFi.softAPgetStationNum();
    String ap_ip = WiFi.softAPIP().toString();
    String sta_ip = staConnected ? WiFi.localIP().toString() : "";
    String json = "{";
    json += "\"sta\":"     + String(staConnected ? "true" : "false") + ",";
    json += "\"ap\":"      + String(apActive     ? "true" : "false") + ",";
    json += "\"clients\":" + String(clients)                          + ",";
    json += "\"max\":"     + String(MAX_CLIENTS)                      + ",";
    json += "\"ap_ip\":\""  + ap_ip  + "\","    ;
    json += "\"sta_ip\":\"" + sta_ip + "\","    ;
    json += "\"domain\":\"" + String(LOCAL_DOMAIN) + "\"";
    json += "}";
    req->send(200, "application/json", json);
  });

  // ── GET /api/files ────────────────────────────────────────────────────────
  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest* req) {
    String times = readUploadTimes();
    String json  = "[";
    bool   first = true;

    File dir = SD.open("/Uploads");
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          String name = stripPath(String(f.name()));
          long   size = f.size();
          String time = getTimeForFile(times, name);

          if (!first) json += ",";
          json += "{\"name\":\"" + name + "\","
                  "\"size\":"    + size + ","
                  "\"time\":\"" + time + "\"}";
          first = false;
        }
        f = dir.openNextFile();
      }
    }
    json += "]";
    req->send(200, "application/json", json);
  });

  // ── POST /api/login ───────────────────────────────────────────────────────
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* req) {
    // PIN sent as URL param ?pin=xxxx  OR as form field
    String pin = "";
    if (req->hasParam("pin", true))  pin = req->getParam("pin", true)->value();
    else if (req->hasParam("pin"))   pin = req->getParam("pin")->value();

    if (pin == getAdminPass()) {
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(401, "application/json", "{\"ok\":false,\"msg\":\"Wrong PIN\"}");
    }
  });

  // ── POST /api/upload ──────────────────────────────────────────────────────
  server.on("/api/upload", HTTP_POST,
    // onRequest — fires after all file data received
    [](AsyncWebServerRequest* req) {
      uploadInProgress = false;
      updateLEDs();
      req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Upload complete\"}");
    },
    // onUpload — fires for each chunk of file data
    [](AsyncWebServerRequest* req,
       const String& filename, size_t index,
       uint8_t* data, size_t len, bool final) {

      uploadInProgress = true;
      updateLEDs();
      String path = "/Uploads/" + filename;

      if (!index) {
        // First chunk: open (create) the file
        Serial.printf("[Upload] Start: %s\n", filename.c_str());
        req->_tempFile = SD.open(path, FILE_WRITE);
      }

      if (req->_tempFile) {
        if (len) req->_tempFile.write(data, len);
        if (final) {
          req->_tempFile.close();
          recordUploadTime(filename);
          Serial.printf("[Upload] Done: %s (%u bytes)\n",
            filename.c_str(), index + len);
        }
      } else {
        Serial.println("[Upload] ERROR: Could not open file on SD!");
      }
    }
  );

  // ── DELETE /api/delete?file=filename ──────────────────────────────────────
  server.on("/api/delete", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("file")) {
      req->send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing ?file param\"}");
      return;
    }
    String filename = req->getParam("file")->value();
    String path     = "/Uploads/" + filename;

    if (SD.exists(path)) {
      SD.remove(path);
      removeUploadTime(filename);
      Serial.printf("[Delete] Removed: %s\n", filename.c_str());
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(404, "application/json", "{\"ok\":false,\"msg\":\"File not found\"}");
    }
  });

  // ── POST /api/save-config (JSON body) ────────────────────────────────────
  server.on("/api/save-config", HTTP_POST,
    // onRequest — fires after body is complete
    [](AsyncWebServerRequest* req) {
      // Parse collected body
      cfg.ap_ssid  = jsonGet(configBodyBuf, "ap_ssid");
      cfg.ap_pass  = jsonGet(configBodyBuf, "ap_pass");
      cfg.sta_type = jsonGet(configBodyBuf, "sta_type");
      cfg.sta_ssid = jsonGet(configBodyBuf, "sta_ssid");
      cfg.sta_email= jsonGet(configBodyBuf, "sta_email");
      cfg.sta_pass = jsonGet(configBodyBuf, "sta_pass");
      String newPin= jsonGet(configBodyBuf, "new_pin");

      saveConfig();
      if (newPin.length() > 0) sdWrite("/admin_pass.txt", newPin);

      req->send(200, "application/json",
        "{\"ok\":true,\"msg\":\"Saved. Rebooting in 2s...\"}");

      Serial.println("[Config] Saved. Scheduling reboot...");
      // Delayed reboot using a one-shot timer
      static esp_timer_handle_t rebootTimer;
      const esp_timer_create_args_t args = {
        .callback = [](void*){ ESP.restart(); },
        .name     = "reboot"
      };
      esp_timer_create(&args, &rebootTimer);
      esp_timer_start_once(rebootTimer, 2000000); // 2 s
    },
    nullptr,  // onUpload (not needed here)
    // onBody — collect JSON body chunks
    [](AsyncWebServerRequest* req,
       uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) configBodyBuf = "";
      configBodyBuf += String((char*)data).substring(0, len);
    }
  );

  // ── Captive-portal catch-all ──────────────────────────────────────────────
  server.onNotFound([](AsyncWebServerRequest* req) {
    // Redirect any unknown URL to the main page
    req->redirect("http://" + String(LOCAL_DOMAIN));
  });
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n===== LabLink Server Booting =====");

  // 1. LEDs
  pinMode(LED_STA,    OUTPUT); digitalWrite(LED_STA,    LOW);
  pinMode(LED_AP,     OUTPUT); digitalWrite(LED_AP,     LOW);
  pinMode(LED_UPLOAD, OUTPUT); digitalWrite(LED_UPLOAD, LOW);
  pinMode(LED_BUFFER, OUTPUT); digitalWrite(LED_BUFFER, LOW);
  // Blink all once as boot indicator
  for (int pin : {LED_STA, LED_AP, LED_UPLOAD, LED_BUFFER}) {
    digitalWrite(pin, HIGH); delay(80);
    digitalWrite(pin, LOW);
  }

  // 2. I2C → OLED + RTC (shared bus)
  Wire.begin(21 /*SDA*/, 22 /*SCL*/);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] Init failed!");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(30, 20);
    display.println("LabLink Server");
    display.setCursor(40, 35);
    display.println("Booting...");
    display.display();
    Serial.println("[OLED] OK");
  }

  // 3. RTC
  if (!rtc.begin()) {
    Serial.println("[RTC] Not found — timestamps will be unavailable.");
  } else {
    if (rtc.lostPower()) {
      Serial.println("[RTC] Lost power; setting compile-time as fallback.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.printf("[RTC] Time: %s\n", formatTime(rtc.now()).c_str());
  }

  // 4. SD Card
  Serial.println("[SD] Initializing SPI and SD card...");
  SPI.begin(18, 19, 23, SD_CS); // SCK, MISO, MOSI, SS
  delay(1000);
  if (!SD.begin(SD_CS, SPI, 4000000)) {
    Serial.println("[SD] FAILED — halting.");
    display.clearDisplay();
    display.setCursor(20, 25);
    display.setTextSize(1);
    display.println("SD CARD ERROR!");
    display.println("1. Check wiring.");
    display.println("1. Format FAT32.");
    display.display();
    while (true) delay(1000);
  }
  Serial.println("[SD] Mounted OK");

  // Ensure required directories / files exist
  if (!SD.exists("/Uploads"))    SD.mkdir("/Uploads");
  if (sdRead("/uploadTime.json").isEmpty()) sdWrite("/uploadTime.json", "[]");

  // 5. Load config from SD
  loadConfig();
  Serial.printf("[Config] AP SSID: %s  STA type: %s  STA SSID: %s\n",
    cfg.ap_ssid.c_str(), cfg.sta_type.c_str(), cfg.sta_ssid.c_str());

  // 6. WiFi (AP + STA)
  startWiFi();

  // 7. DNS Server — wildcards all DNS to ESP AP IP (captive portal)
  //    This means typing "lab.local" or any address resolves to the ESP
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.printf("[DNS] Started on port %d → %s\n",
    DNS_PORT, WiFi.softAPIP().toString().c_str());

  // 8. Web server routes + start
  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Server started on port 80");

  // 9. Final status
  updateLEDs();
  updateOLED();
  Serial.println("===== LabLink Ready =====");
  Serial.printf("  Open browser → http://%s\n", LOCAL_DOMAIN);
  if (apActive)      Serial.printf("  AP:  http://%s\n", WiFi.softAPIP().toString().c_str());
  if (staConnected)  Serial.printf("  STA: http://%s\n", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────

void loop() {
  // Process DNS captive-portal requests
  dnsServer.processNextRequest();

  // Refresh OLED + LEDs every 3 seconds
  static unsigned long lastRefresh = 0;
  if (millis() - lastRefresh > 3000) {
    lastRefresh    = millis();
    staConnected   = (WiFi.status() == WL_CONNECTED);
    updateLEDs();
    updateOLED();
  }
}