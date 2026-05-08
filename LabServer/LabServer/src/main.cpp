/*
 * ============================================================
 *  LabLink Server — ESP32 DevKit V1 Firmware
 *  COAL Lab, University Project
 * ============================================================
 *  SD Card REMOVED — storage now uses LittleFS (internal flash)
 *  Default WiFi credentials are hardcoded at compile time.
 *  Dynamic config can be changed at runtime via web portal.
 * ============================================================
 *  Features:
 *    • Dual-mode WiFi: AP (fast local hotspot) + STA (router/eduroam)
 *    • Async HTTP server on port 80 (AsyncTCP + ESPAsyncWebServer)
 *    • LittleFS file storage (internal ESP32 flash, no SD needed)
 *    • RTC DS3231 timestamps on every upload
 *    • Captive-portal DNS so users just type "lab.local"
 *    • OLED SSD1306 128×64 status display
 *    • 4 LEDs: STA, AP, Upload, Buffer-Full
 *    • Dynamic config stored in /config.json on LittleFS
 *    • Admin PIN stored in /admin_pass.txt on LittleFS
 *    • Upload timestamps stored in /uploadTime.json on LittleFS
 * ============================================================
 *  Required Libraries (PlatformIO lib_deps):
 *    mathieucarbou/AsyncTCP, mathieucarbou/ESPAsyncWebServer,
 *    RTClib, Adafruit GFX Library, Adafruit SSD1306
 *    LittleFS comes bundled with ESP32 Arduino core ≥ 2.x
 * ============================================================
 *
 *  ── DEFAULT CREDENTIALS (edit before first flash) ──────────
 *  These are used on the very first boot (or after flash-erase).
 *  Once saved via the web portal they persist in LittleFS.
 */
#define DEFAULT_AP_SSID   "COAL_Lab_Server"
#define DEFAULT_AP_PASS   "lab12345"
#define DEFAULT_STA_TYPE  "standard"        // "standard" or "enterprise"
#define DEFAULT_STA_SSID  "Sweet-2G"        // ← your router SSID
#define DEFAULT_STA_PASS  "123456789"       // ← your router password
#define DEFAULT_STA_EMAIL ""               // only for enterprise/eduroam
#define DEFAULT_ADMIN_PIN "1234"
/*  ─────────────────────────────────────────────────────────── */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>          // ← replaces SD + SPI
#include "esp_wpa2.h"          // WPA2-Enterprise (Eduroam)

// ─────────────────────────────────────────────
//  PIN ASSIGNMENTS  (SD_CS pin freed — no longer used)
// ─────────────────────────────────────────────
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
#define MAX_CLIENTS   4

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
String        currentUploadFile = "";
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
//  LITTLEFS HELPERS  (drop-in replacements for SD helpers)
// ─────────────────────────────────────────────

/** Read entire file from LittleFS into a String. Returns "" on failure. */
String fsRead(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

/** Write a String to LittleFS, overwriting any existing content. */
bool fsWrite(const char* path, const String& content) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.print(content);
  f.close();
  return true;
}

/** Delete a file from LittleFS (safe to call even if not present). */
void fsRemove(const char* path) {
  if (LittleFS.exists(path)) LittleFS.remove(path);
}

/** Extract a JSON string value: "key":"value" */
/** Extract a JSON string value robustly, ignoring spaces */
String jsonGet(const String& json, const String& key) {
  String search = "\"" + key + "\"";
  int keyPos = json.indexOf(search);
  if (keyPos < 0) return "";
  
  // Find the colon after the key
  int colonPos = json.indexOf(':', keyPos + search.length());
  if (colonPos < 0) return "";
  
  // Find the first quote after the colon (start of value)
  int startQuote = json.indexOf('"', colonPos);
  if (startQuote < 0) return "";
  
  // Find the ending quote (end of value)
  int endQuote = json.indexOf('"', startQuote + 1);
  if (endQuote < 0) return "";
  
  return json.substring(startQuote + 1, endQuote);
}

/** Strip leading directory path from a filename */
String stripPath(const String& fullName) {
  int slash = fullName.lastIndexOf('/');
  return (slash >= 0) ? fullName.substring(slash + 1) : fullName;
}

// ─────────────────────────────────────────────
//  CONFIG LOAD / SAVE
// ─────────────────────────────────────────────

void loadConfig() {
  String raw = fsRead("/config.json");
  if (raw.isEmpty()) {
    // First boot — use hardcoded defaults and persist them
    cfg = {
      DEFAULT_AP_SSID,
      DEFAULT_AP_PASS,
      DEFAULT_STA_TYPE,
      DEFAULT_STA_SSID,
      DEFAULT_STA_EMAIL,
      DEFAULT_STA_PASS
    };
    Serial.println("[Config] No config.json found — using compiled defaults.");
    return;
  }
  cfg.ap_ssid  = jsonGet(raw, "ap_ssid");
  cfg.ap_pass  = jsonGet(raw, "ap_pass");
  cfg.sta_type = jsonGet(raw, "sta_type");
  cfg.sta_ssid = jsonGet(raw, "sta_ssid");
  cfg.sta_email= jsonGet(raw, "sta_email");
  cfg.sta_pass = jsonGet(raw, "sta_pass");
  Serial.println("[Config] Loaded from LittleFS.");
}

void saveConfig() {
  String j  = "{\n";
  j += "  \"ap_ssid\": \""  + cfg.ap_ssid  + "\",\n";
  j += "  \"ap_pass\": \""  + cfg.ap_pass  + "\",\n";
  j += "  \"sta_type\": \"" + cfg.sta_type + "\",\n";
  j += "  \"sta_ssid\": \"" + cfg.sta_ssid + "\",\n";
  j += "  \"sta_email\": \""+ cfg.sta_email+ "\",\n";
  j += "  \"sta_pass\": \"" + cfg.sta_pass + "\"\n}";
  fsWrite("/config.json", j);
  Serial.println("[Config] Saved to LittleFS.");
}

// ─────────────────────────────────────────────
//  ADMIN PASSWORD
// ─────────────────────────────────────────────

String getAdminPass() {
  String p = fsRead("/admin_pass.txt");
  p.trim();
  if (p.isEmpty()) return DEFAULT_ADMIN_PIN;  // fallback to compile-time default
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

/** Read uploadTime.json; returns "[]" if missing */
String readUploadTimes() {
  String raw = fsRead("/uploadTime.json");
  raw.trim();
  return (raw.isEmpty()) ? "[]" : raw;
}

/** Append a new entry {name, time} to uploadTime.json */
// void recordUploadTime(const String& filename) {
//   String ts  = "Unknown";
//   if (rtc.begin()) {
//     DateTime now = rtc.now();
//     ts = formatTime(now);
//   }

//   String raw = readUploadTimes();
//   raw.trim();
//   if (raw.endsWith("]")) raw = raw.substring(0, raw.length() - 1);

//   String entry = "{\"name\":\"" + filename + "\",\"time\":\"" + ts + "\"}";
//   if (raw == "[") {
//     raw += entry + "]";
//   } else {
//     raw += "," + entry + "]";
//   }
//   fsWrite("/uploadTime.json", raw);
// }
/** Append a new entry {name, time} to uploadTime.json */
void recordUploadTime(const String& filename) {
  String ts = "Unknown";
  
  // Directly ask for the time. Do NOT call rtc.begin() here!
  DateTime now = rtc.now();
  
  if (now.year() > 2000) {
    ts = formatTime(now);
  } else {
    Serial.println("[RTC] Warning: Invalid time read.");
  }

  String raw = readUploadTimes();
  raw.trim();
  if (raw.endsWith("]")) raw = raw.substring(0, raw.length() - 1);

  String entry = "{\"name\":\"" + filename + "\",\"time\":\"" + ts + "\"}";
  if (raw == "[") {
    raw += entry + "]";
  } else {
    raw += "," + entry + "]";
  }
  fsWrite("/uploadTime.json", raw);
}

/** Remove a file's entry from uploadTime.json */
void removeUploadTime(const String& filename) {
  String raw     = readUploadTimes();
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
  fsWrite("/uploadTime.json", rebuilt);
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

  display.fillRect(0, 0, 128, 10, WHITE);
  display.setTextColor(BLACK);
  display.setCursor(28, 1);
  display.print("LabLink Server");
  display.setTextColor(WHITE);

  display.setCursor(0, 13);
  display.print("AP: ");
  display.print(apActive ? cfg.ap_ssid : "OFF");

  display.setCursor(0, 23);
  display.print("STA: ");
  display.print(staConnected ? WiFi.localIP().toString() : "Disconnected");

  display.setCursor(0, 33);
  display.print("Clients: ");
  display.print(clients);
  display.print("/");
  display.print(MAX_CLIENTS);

  display.setCursor(0, 43);
  display.print("http://");
  display.print(LOCAL_DOMAIN);

  if (uploadInProgress) {
    display.setCursor(0, 53);
    String displayName = " " + currentUploadFile;
    // Truncate filename to fit on OLED (max ~18 chars with ">> uploading " prefix)
    if (displayName.length() > 18) {
      displayName = displayName.substring(0, 15) + "...";
    }
    char up = 24;
    display.print(up + displayName);
  }

  display.display();
}

void updateLEDs() {
  int clients = WiFi.softAPgetStationNum();
  digitalWrite(LED_STA,    staConnected     ? HIGH : LOW);
  digitalWrite(LED_AP,     apActive         ? HIGH : LOW);
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
    Serial.printf("[STA] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[STA] Failed — running AP-only mode.");
  }
}

// ─────────────────────────────────────────────
//  WEB SERVER ROUTES
// ─────────────────────────────────────────────

static String configBodyBuf = "";

void setupRoutes() {

  // ── Static files from LittleFS ────────────────────────────────────────────
  // Serves /index.html as the default page
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  // Allow direct download of uploaded files
  server.serveStatic("/Uploads/", LittleFS, "/Uploads/");

  // ── GET /api/status ───────────────────────────────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    int clients   = WiFi.softAPgetStationNum();
    String ap_ip  = WiFi.softAPIP().toString();
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

    File dir = LittleFS.open("/Uploads");
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
    [](AsyncWebServerRequest* req) {
      uploadInProgress = false;      currentUploadFile = "";      updateLEDs();
      req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Upload complete\"}");
    },
    [](AsyncWebServerRequest* req,
       const String& filename, size_t index,
       uint8_t* data, size_t len, bool final) {

      uploadInProgress = true;
      currentUploadFile = filename;
      updateLEDs();
      String path = "/Uploads/" + filename;

      if (!index) {
        Serial.printf("[Upload] Start: %s\n", filename.c_str());
        req->_tempFile = LittleFS.open(path, "w");
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
        Serial.println("[Upload] ERROR: Could not open file on LittleFS!");
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

    if (LittleFS.exists(path)) {
      LittleFS.remove(path);
      removeUploadTime(filename);
      Serial.printf("[Delete] Removed: %s\n", filename.c_str());
      req->send(200, "application/json", "{\"ok\":true}");
    } else {
      req->send(404, "application/json", "{\"ok\":false,\"msg\":\"File not found\"}");
    }
 
  });

  // ── POST /api/save-config (JSON body) ────────────────────────────────────
  server.on("/api/save-config", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      cfg.ap_ssid  = jsonGet(configBodyBuf, "ap_ssid");
      cfg.ap_pass  = jsonGet(configBodyBuf, "ap_pass");
      cfg.sta_type = jsonGet(configBodyBuf, "sta_type");
      cfg.sta_ssid = jsonGet(configBodyBuf, "sta_ssid");
      cfg.sta_email= jsonGet(configBodyBuf, "sta_email");
      cfg.sta_pass = jsonGet(configBodyBuf, "sta_pass");
      String newPin= jsonGet(configBodyBuf, "new_pin");

      saveConfig();
      if (newPin.length() > 0) fsWrite("/admin_pass.txt", newPin);

      req->send(200, "application/json",
        "{\"ok\":true,\"msg\":\"Saved. Rebooting in 2s...\"}");

      Serial.println("[Config] Saved. Scheduling reboot...");
      static esp_timer_handle_t rebootTimer;
      const esp_timer_create_args_t args = {
        .callback = [](void*){ ESP.restart(); },
        .name     = "reboot"
      };
      esp_timer_create(&args, &rebootTimer);
      esp_timer_start_once(rebootTimer, 2000000);
    },
    nullptr,
    [](AsyncWebServerRequest* req,
       uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) configBodyBuf = "";
      configBodyBuf += String((char*)data).substring(0, len);
    }
  );

  // ── Captive-portal catch-all ──────────────────────────────────────────────
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->redirect("http://" + String(LOCAL_DOMAIN));
    if (req->url()== "/"){
      req->send(500, "text/plain","Error: index.html is missing.");
      return;
    }

    String redirectUrl = "http://" + WiFi.softAPIP().toString() + "/";
    req->redirect(redirectUrl);
  });
  // ── Captive-portal catch-all ──────────────────────────────────────────────
  // server.onNotFound([](AsyncWebServerRequest* req) {
  //   // If the browser requests the root but it fails, index.html is missing
  //   if (req->url() == "/") {
  //     req->send(500, "text/plain", "Error: index.html is missing from LittleFS. Did you run 'Upload Filesystem Image'?");
  //     return;
    // }

    // For all other requests (like /generate_204 from Android or /hotspot-detect from Apple)
    // Redirect them to the local domain to trigger the captive portal popup
  //   req->redirect("http://" + String(LOCAL_DOMAIN));
  // });
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
  for (int pin : {LED_STA, LED_AP, LED_UPLOAD, LED_BUFFER}) {
    digitalWrite(pin, HIGH); delay(80);
    digitalWrite(pin, LOW);
  }

  // 2. I2C → OLED + RTC
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
    Serial.println("[RTC] Not found — timestamps will show 'Unknown'.");
  } else {
    if (rtc.lostPower()) {
      Serial.println("[RTC] Lost power; setting compile-time as fallback.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.printf("[RTC] Time: %s\n", formatTime(rtc.now()).c_str());
  }

  // 4. LittleFS (replaces SD card entirely)
  // NOTE: formatOnFail=true means if the flash partition is corrupt or blank,
  //       it will be auto-formatted. Safe to leave on.
  Serial.println("[FS] Mounting LittleFS...");
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    // This should never happen on a healthy ESP32
    Serial.println("[FS] FAILED — halting.");
    display.clearDisplay();
    display.setCursor(10, 20);
    display.println("LittleFS ERROR!");
    display.println("Try re-flashing.");
    display.display();
    while (true) delay(1000);
  }
  Serial.println("[FS] LittleFS mounted OK");

  // Ensure required directories / files exist
  if (!LittleFS.exists("/Uploads")) LittleFS.mkdir("/Uploads");
  if (fsRead("/uploadTime.json").isEmpty()) fsWrite("/uploadTime.json", "[]");

  // 5. Load config (falls back to hardcoded defaults on first boot)
  loadConfig();
  Serial.printf("[Config] AP: %s  STA type: %s  STA SSID: %s\n",
    cfg.ap_ssid.c_str(), cfg.sta_type.c_str(), cfg.sta_ssid.c_str());

  // 6. WiFi (AP + STA)
  startWiFi();

  // 7. DNS Server
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
  if (apActive)     Serial.printf("  AP:  http://%s\n", WiFi.softAPIP().toString().c_str());
  if (staConnected) Serial.printf("  STA: http://%s\n", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────

void loop() {
  dnsServer.processNextRequest();

  static unsigned long lastRefresh = 0;
  if (millis() - lastRefresh > 3000) {
    lastRefresh  = millis();
    staConnected = (WiFi.status() == WL_CONNECTED);
    updateLEDs();
    updateOLED();
  }
}
