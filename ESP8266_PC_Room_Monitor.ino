/*
  ESP8266 + 1.8" ST7735 (128x160) + DHT11 PC/Room Monitor
  Two pages: PC stats and room/environment.
  PC Python helper posts JSON to http://ESP_IP/api every 2 seconds.
*/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ---------- CHANGE THESE ----------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// ----------------------------------

#define TFT_CS   D1
#define TFT_DC   D2
#define TFT_RST  D0
#define DHT_PIN  D6
#define DHT_TYPE DHT11

// Most 1.8" 128x160 modules use ST7735. If colors/offsets are wrong, try INITR_GREENTAB.
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);

struct PcData {
  float cpuUsage = -1;
  float ramUsage = -1;
  float ramUsed = -1;
  float ramTotal = -1;
  float cpuTemp = -999;
  float gpuUsage = -1;
  float gpuTemp = -999;
  unsigned long updatedAt = 0;
} pc;

float roomTemp = NAN, humidity = NAN;
uint8_t page = 0;
unsigned long lastDhtRead = 0, lastDraw = 0, lastPageChange = 0;
const unsigned long PAGE_INTERVAL = 7000;
const unsigned long DRAW_INTERVAL = 1000;
const unsigned long DHT_INTERVAL = 2500;

String fmtTemp(float v) {
  if (v < -100 || isnan(v)) return "--";
  return String(v, 1) + " C";
}
String fmtPct(float v) {
  if (v < 0 || isnan(v)) return "--";
  return String((int)round(v)) + "%";
}
String fmtRam() {
  if (pc.ramUsed < 0 || pc.ramTotal <= 0) return "--";
  return String(pc.ramUsed, 1) + "/" + String(pc.ramTotal, 1) + "G";
}

void handleApi() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
    return;
  }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
    return;
  }
  pc.cpuUsage = doc["cpu"] | -1.0f;
  pc.ramUsage = doc["ram_percent"] | -1.0f;
  pc.ramUsed = doc["ram_used_gb"] | -1.0f;
  pc.ramTotal = doc["ram_total_gb"] | -1.0f;
  pc.cpuTemp = doc["cpu_temp"] | -999.0f;
  pc.gpuUsage = doc["gpu"] | -1.0f;
  pc.gpuTemp = doc["gpu_temp"] | -999.0f;
  pc.updatedAt = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

void drawHeader(const char* title) {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 128, 20, 0x001F);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 6);
  tft.print(title);
  tft.drawFastHLine(0, 21, 128, 0x7BEF);
}

void drawRow(int y, const char* label, String value, uint16_t color) {
  tft.setTextSize(1);
  tft.setTextColor(0xBDF7);
  tft.setCursor(4, y);
  tft.print(label);
  tft.setTextColor(color);
  tft.setCursor(69, y);
  tft.print(value);
}

void drawPcPage() {
  drawHeader("PC SYSTEM MONITOR");
  bool fresh = pc.updatedAt && (millis() - pc.updatedAt < 10000);
  drawRow(29, "CPU LOAD", fresh ? fmtPct(pc.cpuUsage) : "--", ST77XX_CYAN);
  drawRow(43, "CPU TEMP", fresh ? fmtTemp(pc.cpuTemp) : "--", ST77XX_YELLOW);
  drawRow(57, "GPU LOAD", fresh ? fmtPct(pc.gpuUsage) : "--", ST77XX_CYAN);
  drawRow(71, "GPU TEMP", fresh ? fmtTemp(pc.gpuTemp) : "--", ST77XX_YELLOW);
  drawRow(85, "RAM USED", fresh ? fmtRam() : "--", ST77XX_GREEN);
  drawRow(99, "RAM LOAD", fresh ? fmtPct(pc.ramUsage) : "--", ST77XX_GREEN);

  tft.drawFastHLine(4, 116, 120, 0x7BEF);
  tft.setTextColor(fresh ? ST77XX_GREEN : ST77XX_RED);
  tft.setCursor(4, 123);
  tft.print(fresh ? "PC LINK: CONNECTED" : "PC LINK: WAITING");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 143);
  tft.print("Page 1/2  Auto-switch");
}

void drawRoomPage() {
  drawHeader("ROOM & CLOCK");
  tft.setTextColor(0xBDF7);
  tft.setCursor(4, 29);
  tft.print("TEMPERATURE");
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 42);
  tft.print(fmtTemp(roomTemp));
  tft.setTextSize(1);
  tft.setTextColor(0xBDF7);
  tft.setCursor(4, 72);
  tft.print("HUMIDITY");
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(4, 85);
  if (isnan(humidity)) tft.print("--");
  else { tft.print((int)round(humidity)); tft.print("%"); }
  tft.setTextSize(1);
  tft.drawFastHLine(4, 112, 120, 0x7BEF);
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 120);
  if (ti && ti->tm_year > 120) {
    char timeBuf[16]; strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", ti);
    tft.setTextSize(2); tft.print(timeBuf);
    tft.setTextSize(1);
    char dateBuf[20]; strftime(dateBuf, sizeof(dateBuf), "%d %b %Y", ti);
    tft.setCursor(4, 143); tft.print(dateBuf);
  } else {
    tft.print("Syncing time...");
  }
}

void handleRoot() {
  server.send(200, "text/plain", "ESP8266 monitor is online. POST PC metrics to /api.");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  dht.begin();

  // Connect TFT BL directly to 3V3 for always-on backlight.

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 35);
  tft.println("PC + ROOM MONITOR");
  tft.println("Connecting Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 25000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("ESP IP: ");
    Serial.println(WiFi.localIP());
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(4, 30);
    tft.println("Wi-Fi connected");
    tft.print("IP: ");
    tft.println(WiFi.localIP());
    configTime(5 * 3600 + 30 * 60, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("\nWi-Fi failed. Check credentials.");
    tft.println("Wi-Fi failed");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_POST, handleApi);
  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastDhtRead >= DHT_INTERVAL) {
    lastDhtRead = millis();
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h)) humidity = h;
    if (!isnan(t)) roomTemp = t;
  }

  if (millis() - lastPageChange >= PAGE_INTERVAL) {
    lastPageChange = millis();
    page = 1 - page;
    lastDraw = 0;
  }
  if (millis() - lastDraw >= DRAW_INTERVAL) {
    lastDraw = millis();
    if (page == 0) drawPcPage();
    else drawRoomPage();
  }
}
