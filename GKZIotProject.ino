
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_MPU6050.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <vector>


//  OLED 
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1     // No dedicated reset pin on these modules
#define I2C_ADDRESS   0x3C   

//  I2C pins 
#define I2C_SDA 21
#define I2C_SCL 22

//  TCA9548A I2C multiplexer 
#define TCA9548A_ADDRESS 0x70   // default address (A0/A1/A2 unconnected)
#define TCA_CHANNEL_OLED 0
#define TCA_CHANNEL_BME  1
#define TCA_CHANNEL_MPU  2
#define TCA_CHANNEL_RTC  3

//  WiFi (for the dashboards' REST API) 
const char* WIFI_SSID     = "WIFI SSD"; //WIFI SSD
const char* WIFI_PASSWORD = "WIFI PASSWORD"; //WIFI PASSWORD

const bool USE_STATIC_IP = true;
IPAddress STATIC_IP(192, 168, 1, 50);
IPAddress GATEWAY_IP(192, 168, 1, 1);
IPAddress SUBNET_MASK(255, 255, 255, 0);
IPAddress DNS_IP(192, 168, 1, 1);

//  Web server / REST API 
WebServer server(80);

const char* HISTORY_FILE      = "/history.csv";
const char* HISTORY_FILE_OLD  = "/history_old.csv";
const char* HISTORY_HEADER    = "ts,tempC,humidity,pressureHpa,coPpm,voltageV,vibrationG,uptimeS\n";
const int   MAX_ROWS_PER_FILE = 500;
const unsigned long LOG_INTERVAL_MS   = 60000;
const unsigned long SAMPLE_INTERVAL_MS = 2000;
int historyRowCount = 0; 
bool fsOK = false; // true once LittleFS mounts successfully

const float MQ7_RL_OHMS  = 10000.0;  
const float MQ7_RO_OHMS  = 10000.0;
const float MQ7_CURVE_M  = -1.5;
const float MQ7_CURVE_B  = 0.53;

//  Live sensor cache 
struct SensorSnapshot {
  bool  envAvailable   = false;
  float tempC = 0, humidity = 0, pressureHpa = 0;
  bool  imuAvailable   = false;
  float vibrationG = 0;
  int   gasRaw = 0;
  float gasVoltage = 0, coPpmEstimate = 0;
  bool  timeAvailable  = false;
  char  isoTime[25] = "";
  unsigned long uptimeS = 0;
} liveData;

void tcaSelect(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(TCA9548A_ADDRESS);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

//  BME280 
#define BME_ADDRESS_PRIMARY   0x76 
#define BME_ADDRESS_SECONDARY 0x77

//  MPU6050 
#define MPU_ADDRESS_PRIMARY   0x68
#define MPU_ADDRESS_SECONDARY 0x69

//  MQ7
const int MQ7_PIN = 34;
const float VCC = 3.3;       // ESP32 ADC reference
const int ADC_MAX = 4095;    // 12-bit ADC

//  Page cycling (button-driven) 
#define BUTTON_PIN 27 
const unsigned long DEBOUNCE_MS = 50;

enum Page { PAGE_TIME = 0, PAGE_ENV = 1, PAGE_IMU = 2, PAGE_GAS = 3, PAGE_COUNT = 4 };
Page currentPage = PAGE_TIME;

bool lastButtonReading = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280 bme;
Adafruit_MPU6050 mpu;
RTC_DS3231 rtc;

bool bmeOK = false;
bool mpuOK = false;
bool rtcOK = false;
bool heartbeat = false; // toggled once per screen refresh, drawn on every page

//  Page header helper 
void drawHeader(const char* title) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(title);
  display.drawFastHLine(0, 10, 128, SH110X_WHITE);
  drawPageDots();

  display.fillRect(121, 1, 6, 6, heartbeat ? SH110X_WHITE : SH110X_BLACK);
}

void drawPageDots() {
  int spacing = 10;
  int startX = (SCREEN_WIDTH - (PAGE_COUNT - 1) * spacing) / 2;
  int y = SCREEN_HEIGHT - 4;
  for (int i = 0; i < PAGE_COUNT; i++) {
    int x = startX + i * spacing;
    if (i == currentPage) {
      display.fillCircle(x, y, 2, SH110X_WHITE);
    } else {
      display.drawCircle(x, y, 2, SH110X_WHITE);
    }
  }
}

bool buttonPressed() {
  bool reading = digitalRead(BUTTON_PIN);
  bool pressed = false;

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        pressed = true;
      }
    }
  }

  lastButtonReading = reading;
  return pressed;
}

// sensor sampling + on-flash "database" + REST API

float estimateCoPpm(float voltage) {
  if (voltage <= 0.001) voltage = 0.001;
  float rs = (VCC - voltage) * MQ7_RL_OHMS / voltage;
  float ratio = rs / MQ7_RO_OHMS;
  if (ratio <= 0) ratio = 0.001;
  float logPpm = (log10(ratio) - MQ7_CURVE_B) / MQ7_CURVE_M;
  float ppm = pow(10, logPpm);
  if (ppm < 0 || isnan(ppm) || isinf(ppm)) ppm = 0;
  return ppm;
}

void sampleAllSensors() {
  if (bmeOK) {
    tcaSelect(TCA_CHANNEL_BME);
    liveData.tempC       = bme.readTemperature();
    liveData.humidity    = bme.readHumidity();
    liveData.pressureHpa = bme.readPressure() / 100.0F;
    liveData.envAvailable = true;
  } else {
    liveData.envAvailable = false;
  }

  if (mpuOK) {
    tcaSelect(TCA_CHANNEL_MPU);
    sensors_event_t accel, gyro, temp;
    mpu.getEvent(&accel, &gyro, &temp);
    float mag = sqrt(accel.acceleration.x * accel.acceleration.x +
                      accel.acceleration.y * accel.acceleration.y +
                      accel.acceleration.z * accel.acceleration.z);
    liveData.vibrationG = fabs(mag / 9.80665 - 1.0); // deviation from 1g at rest
    liveData.imuAvailable = true;
  } else {
    liveData.imuAvailable = false;
  }

  // MQ7 is analog and bypasses the mux entirely.
  liveData.gasRaw       = analogRead(MQ7_PIN);
  liveData.gasVoltage    = (liveData.gasRaw / (float)ADC_MAX) * VCC;
  liveData.coPpmEstimate = estimateCoPpm(liveData.gasVoltage);

  if (rtcOK) {
    tcaSelect(TCA_CHANNEL_RTC);
    DateTime now = rtc.now();
    snprintf(liveData.isoTime, sizeof(liveData.isoTime), "%04d-%02d-%02dT%02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    liveData.timeAvailable = true;
  } else {
    liveData.timeAvailable = false;
    liveData.isoTime[0] = '\0';
  }

  tcaSelect(TCA_CHANNEL_OLED);
  liveData.uptimeS = millis() / 1000;
}

//  On-flash "database" (LittleFS CSV log) 

int countLines(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    if (f.read() == '\n') count++;
  }
  f.close();
  return count > 0 ? count - 1 : 0;
}

void initHistoryLog() {
  if (!LittleFS.exists(HISTORY_FILE)) {
    File f = LittleFS.open(HISTORY_FILE, "w");
    if (f) {
      f.print(HISTORY_HEADER);
      f.close();
    }
    historyRowCount = 0;
  } else {
    historyRowCount = countLines(HISTORY_FILE);
  }
  Serial.printf("[DB] History log ready - %d row(s) currently stored\n", historyRowCount);
}

void appendHistoryRow() {
  if (historyRowCount >= MAX_ROWS_PER_FILE) {
    LittleFS.remove(HISTORY_FILE_OLD);
    LittleFS.rename(HISTORY_FILE, HISTORY_FILE_OLD);
    File f = LittleFS.open(HISTORY_FILE, "w");
    if (f) {
      f.print(HISTORY_HEADER);
      f.close();
    }
    historyRowCount = 0;
    Serial.println("[DB] Log full - rotated to history_old.csv");
  }

  File f = LittleFS.open(HISTORY_FILE, "a");
  if (!f) {
    Serial.println("[DB] Failed to open history file for append");
    return;
  }
  char row[160];
  const char* ts = liveData.timeAvailable ? liveData.isoTime : "";
  snprintf(row, sizeof(row), "%s,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%lu\n",
           ts, liveData.tempC, liveData.humidity, liveData.pressureHpa,
           liveData.coPpmEstimate, liveData.gasVoltage, liveData.vibrationG,
           liveData.uptimeS);
  f.print(row);
  f.close();
  historyRowCount++;
}

//  REST API 

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCorsHeaders();
  server.send(204);
}

// GET /api/sensors
void handleApiSensors() {
  sendCorsHeaders();
  char json[640];
  snprintf(json, sizeof(json),
    "{"
      "\"env\":{\"available\":%s,\"tempC\":%.2f,\"humidity\":%.2f,\"pressureHpa\":%.2f},"
      "\"gas\":{\"coPpmEstimate\":%.2f,\"voltageV\":%.3f},"
      "\"imu\":{\"available\":%s,\"vibrationG\":%.3f},"
      "\"device\":{\"wifiRssiDbm\":%d,\"uptimeS\":%lu,\"ip\":\"%s\",\"freeHeapBytes\":%u},"
      "\"time\":{\"available\":%s,\"iso\":\"%s\"}"
    "}",
    liveData.envAvailable ? "true" : "false", liveData.tempC, liveData.humidity, liveData.pressureHpa,
    liveData.coPpmEstimate, liveData.gasVoltage,
    liveData.imuAvailable ? "true" : "false", liveData.vibrationG,
    WiFi.RSSI(), liveData.uptimeS, WiFi.localIP().toString().c_str(), ESP.getFreeHeap(),
    liveData.timeAvailable ? "true" : "false", liveData.isoTime);
  server.send(200, "application/json", json);
}

void loadCsvLines(const char* path, std::vector<String> &out) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  f.readStringUntil('\n'); // skip header
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) out.push_back(line);
  }
  f.close();
}

String csvLineToJson(const String &line) {
  String parts[8];
  int p = 0, start = 0;
  for (unsigned int i = 0; i <= line.length() && p < 8; i++) {
    if (i == line.length() || line[i] == ',') {
      parts[p++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (p < 8) return "";
  char obj[220];
  snprintf(obj, sizeof(obj),
    "{\"ts\":\"%s\",\"tempC\":%s,\"humidity\":%s,\"pressureHpa\":%s,"
    "\"coPpm\":%s,\"voltageV\":%s,\"vibrationG\":%s,\"uptimeS\":%s}",
    parts[0].c_str(), parts[1].c_str(), parts[2].c_str(), parts[3].c_str(),
    parts[4].c_str(), parts[5].c_str(), parts[6].c_str(), parts[7].c_str());
  return String(obj);
}

void handleApiHistory() {
  sendCorsHeaders();
  int limit = 0; //
  if (server.hasArg("limit")) limit = server.arg("limit").toInt();

  std::vector<String> lines;
  loadCsvLines(HISTORY_FILE_OLD, lines);
  loadCsvLines(HISTORY_FILE, lines);

  int total = lines.size();
  int startIdx = 0;
  if (limit > 0 && total > limit) startIdx = total - limit;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"count\":" + String(total - startIdx) + ",\"rows\":[");
  bool first = true;
  for (int i = startIdx; i < total; i++) {
    String obj = csvLineToJson(lines[i]);
    if (obj.length() == 0) continue;
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent(obj);
  }
  server.sendContent("]}");
  server.sendContent("");
}

// GET /api/history/csv
void handleApiHistoryCsv() {
  sendCorsHeaders();
  server.sendHeader("Content-Disposition", "attachment; filename=history.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent(HISTORY_HEADER);
  if (LittleFS.exists(HISTORY_FILE_OLD)) {
    File f = LittleFS.open(HISTORY_FILE_OLD, "r");
    if (f) {
      f.readStringUntil('\n');
      while (f.available()) server.sendContent(f.readStringUntil('\n') + "\n");
      f.close();
    }
  }
  File f = LittleFS.open(HISTORY_FILE, "r");
  if (f) {
    f.readStringUntil('\n');
    while (f.available()) server.sendContent(f.readStringUntil('\n') + "\n");
    f.close();
  }
  server.sendContent("");
}

// POST /api/history/clear
void handleApiHistoryClear() {
  sendCorsHeaders();
  LittleFS.remove(HISTORY_FILE);
  LittleFS.remove(HISTORY_FILE_OLD);
  File f = LittleFS.open(HISTORY_FILE, "w");
  if (f) { f.print(HISTORY_HEADER); f.close(); }
  historyRowCount = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNotFound() {
  sendCorsHeaders();
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  if (USE_STATIC_IP) {
    if (!WiFi.config(STATIC_IP, GATEWAY_IP, SUBNET_MASK, DNS_IP)) {
      Serial.println("[WIFI] Static IP config failed - falling back to DHCP");
    }
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected - IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Failed to connect within 15s - will keep retrying in the background");
  }
}

void setupWebServer() {
  server.on("/api/sensors", HTTP_GET, handleApiSensors);
  server.on("/api/sensors", HTTP_OPTIONS, handleOptions);
  server.on("/api/history", HTTP_GET, handleApiHistory);
  server.on("/api/history", HTTP_OPTIONS, handleOptions);
  server.on("/api/history/csv", HTTP_GET, handleApiHistoryCsv);
  server.on("/api/history/clear", HTTP_POST, handleApiHistoryClear);
  server.on("/api/history/clear", HTTP_OPTIONS, handleOptions);

  if (fsOK) {
    server.serveStatic("/", LittleFS, "/www/mobile.html");
    server.serveStatic("/mobile.html", LittleFS, "/www/mobile.html");
    server.serveStatic("/index.html", LittleFS, "/www/index.html");
    server.serveStatic("/manifest.json", LittleFS, "/www/manifest.json");
    server.serveStatic("/service-worker.js", LittleFS, "/www/service-worker.js");
    server.serveStatic("/icon-192.png", LittleFS, "/www/icon-192.png");
    server.serveStatic("/icon-512.png", LittleFS, "/www/icon-512.png");
  }

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] API server started on port 80");
}

//  Environment page (BME280) 
void showEnvPage() {
  tcaSelect(TCA_CHANNEL_OLED);
  drawHeader("ENVIRONMENT");

  if (!bmeOK) {
    display.setCursor(0, 20);
    display.println("BME280 not found!");
    display.display();
    return;
  }

  tcaSelect(TCA_CHANNEL_BME);
  float tempC       = bme.readTemperature();
  float humidity    = bme.readHumidity();
  float pressureHPa = bme.readPressure() / 100.0F;

  tcaSelect(TCA_CHANNEL_OLED);
  display.setCursor(0, 16);
  display.print("Temp: ");
  display.print(tempC, 1);
  display.println(" C");

  display.setCursor(0, 30);
  display.print("Hum:  ");
  display.print(humidity, 1);
  display.println(" %");

  display.setCursor(0, 44);
  display.print("Pres: ");
  display.print(pressureHPa, 1);
  display.println(" hPa");

  display.display();

  Serial.printf("[ENV] Temp: %.1f C  Hum: %.1f %%  Pres: %.1f hPa\n",
                tempC, humidity, pressureHPa);
}

//  IMU page (MPU6050) 
void showImuPage() {
  tcaSelect(TCA_CHANNEL_OLED);
  drawHeader("MPU6050");

  if (!mpuOK) {
    display.setCursor(0, 20);
    display.println("MPU6050 not found!");
    display.display();
    return;
  }

  tcaSelect(TCA_CHANNEL_MPU);
  sensors_event_t accel, gyro, temp;
  mpu.getEvent(&accel, &gyro, &temp);

  float ax = accel.acceleration.x;
  float ay = accel.acceleration.y;
  float az = accel.acceleration.z;
  float gx = gyro.gyro.x;
  float gy = gyro.gyro.y;
  float gz = gyro.gyro.z;

  tcaSelect(TCA_CHANNEL_OLED);
  display.setCursor(0, 13);
  display.print("AX:");
  display.print(ax, 2);
  display.print(" AY:");
  display.println(ay, 2);

  display.setCursor(0, 25);
  display.print("AZ:");
  display.print(az, 2);
  display.println(" m/s2");

  display.setCursor(0, 37);
  display.print("GX:");
  display.print(gx, 2);
  display.print(" GY:");
  display.println(gy, 2);

  display.setCursor(0, 49);
  display.print("GZ:");
  display.print(gz, 2);
  display.println(" rad/s");

  display.display();

  Serial.printf("[IMU] AX:%.2f AY:%.2f AZ:%.2f  GX:%.2f GY:%.2f GZ:%.2f\n",
                ax, ay, az, gx, gy, gz);
}

//  Gas page (MQ7) 
void showGasPage() {

  tcaSelect(TCA_CHANNEL_OLED);
  drawHeader("MQ7 GAS SENSOR");

  int raw = analogRead(MQ7_PIN);
  float voltage = (raw / (float)ADC_MAX) * VCC;

  display.setCursor(0, 20);
  display.print("Raw ADC: ");
  display.println(raw);

  display.setCursor(0, 32);
  display.print("Voltage: ");
  display.print(voltage, 3);
  display.println(" V");

  display.display();

  Serial.print("[GAS] Raw: ");
  Serial.print(raw);
  Serial.print("  Voltage: ");
  Serial.print(voltage, 3);
  Serial.println(" V");
}

//  Time page (DS3231) 
const char* WEEKDAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

void showTimePage() {
  tcaSelect(TCA_CHANNEL_OLED);
  drawHeader("TIME");

  if (!rtcOK) {
    display.setCursor(0, 24);
    display.println("DS3231 not found!");
    display.display();
    return;
  }

  tcaSelect(TCA_CHANNEL_RTC);
  DateTime now = rtc.now();
  tcaSelect(TCA_CHANNEL_OLED);

  char timeBuf[9]; // "HH:MM:SS"
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  char dateBuf[20]; // "Sun 2026-08-04"
  snprintf(dateBuf, sizeof(dateBuf), "%s %04d-%02d-%02d",
           WEEKDAY_NAMES[now.dayOfTheWeek()], now.year(), now.month(), now.day());

  // Big clock, centered-ish
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println(timeBuf);

  // Date underneath, normal size
  display.setTextSize(1);
  display.setCursor(14, 44);
  display.println(dateBuf);

  display.display();

  Serial.printf("[TIME] %s %s\n", dateBuf, timeBuf);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000); 

  tcaSelect(TCA_CHANNEL_OLED);
  if (!display.begin(I2C_ADDRESS, true)) {
    Serial.println("SH1106 not found - check wiring, address, driver type, and mux channel");
    while (1) delay(10);
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();

  tcaSelect(TCA_CHANNEL_BME);
  bmeOK = bme.begin(BME_ADDRESS_PRIMARY) || bme.begin(BME_ADDRESS_SECONDARY);
  if (!bmeOK) {
    Serial.println("BME280 not found - check wiring/address/mux channel (tried 0x76 and 0x77)");
  }

  tcaSelect(TCA_CHANNEL_MPU);
  mpuOK = mpu.begin(MPU_ADDRESS_PRIMARY) || mpu.begin(MPU_ADDRESS_SECONDARY);
  if (mpuOK) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  } else {
    Serial.println("MPU6050 not found - check wiring/address/mux channel (tried 0x68 and 0x69)");
  }

  tcaSelect(TCA_CHANNEL_RTC);
  rtcOK = rtc.begin();
  if (rtcOK) {
    if (rtc.lostPower()) {
      Serial.println("RTC lost power - setting to compile time");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  } else {
    Serial.println("DS3231 not found - check wiring/mux channel (expected 0x68 on its own channel)");
  }

  tcaSelect(TCA_CHANNEL_OLED);
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("MQ7 warming up...");
  display.display();
  delay(3000);

  // WiFi + on-flash database + REST API
  setupWiFi();

  if (!LittleFS.begin(true)) { // true = format on mount failure (e.g. first boot)
    Serial.println("[DB] LittleFS mount failed - check Partition Scheme includes SPIFFS/LittleFS space");
  } else {
    fsOK = true;
    Serial.println("[DB] LittleFS mounted");
    initHistoryLog();
    if (!LittleFS.exists("/www/mobile.html")) {
      Serial.println("[HTTP] /www/mobile.html not found on flash - dashboard files haven't been uploaded yet");
      Serial.println("[HTTP] Upload the 'data' folder to LittleFS via the IDE's filesystem uploader, then reboot");
    }
  }

  setupWebServer();
  sampleAllSensors();
}

const unsigned long REFRESH_INTERVAL_MS = 500;
unsigned long lastRefresh = 0;
unsigned long lastSample = 0; 
unsigned long lastLog = 0; 

void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    sampleAllSensors();
  }

  if (now - lastLog >= LOG_INTERVAL_MS) {
    lastLog = now;
    appendHistoryRow();
    Serial.printf("[DB] Logged row (%d/%d in current file)\n", historyRowCount, MAX_ROWS_PER_FILE);
  }

  bool pageChanged = false;
  if (buttonPressed()) {
    currentPage = static_cast<Page>((currentPage + 1) % PAGE_COUNT);
    pageChanged = true;
    Serial.print("[BTN] Switched to page ");
    Serial.println(currentPage);
  }

  if (pageChanged || now - lastRefresh >= REFRESH_INTERVAL_MS) {
    lastRefresh = now;
    heartbeat = !heartbeat; // blinks on every refresh, on every page
    switch (currentPage) {
      case PAGE_TIME: showTimePage(); break;
      case PAGE_ENV: showEnvPage(); break;
      case PAGE_IMU: showImuPage(); break;
      case PAGE_GAS: showGasPage(); break;
      default: break;
    }
  }
}
