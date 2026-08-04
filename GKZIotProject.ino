

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_MPU6050.h>
#include <RTClib.h>

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1     
#define I2C_ADDRESS   0x3C  

// ---------- I2C pins ----------
#define I2C_SDA 21
#define I2C_SCL 22

// ---------- TCA9548A I2C multiplexer ----------
#define TCA9548A_ADDRESS 0x70   
#define TCA_CHANNEL_OLED 0
#define TCA_CHANNEL_BME  1
#define TCA_CHANNEL_MPU  2
#define TCA_CHANNEL_RTC  3


void tcaSelect(uint8_t channel) {
  if (channel > 7) return;
  Wire.beginTransmission(TCA9548A_ADDRESS);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

// ---------- BME280 ----------
#define BME_ADDRESS_PRIMARY   0x76  
#define BME_ADDRESS_SECONDARY 0x77  

// ---------- MPU6050 ----------
#define MPU_ADDRESS_PRIMARY   0x68
#define MPU_ADDRESS_SECONDARY 0x69

// ---------- MQ7 (analog, ADC1 only - ADC2 conflicts with WiFi) ----------
const int MQ7_PIN = 34;
const float VCC = 3.3;       
const int ADC_MAX = 4095;    

// ---------- Page cycling (button-driven) ----------
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
bool heartbeat = false; 

// ---------- Page header helper ----------
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

// ---------- Environment page (BME280) ----------
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

// ---------- IMU page (MPU6050) ----------
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

// ---------- Gas page (MQ7) ----------
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

// ---------- Time page (DS3231) ----------
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

  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println(timeBuf);
  
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
}

const unsigned long REFRESH_INTERVAL_MS = 500; 
unsigned long lastRefresh = 0;

void loop() {
  bool pageChanged = false;
  if (buttonPressed()) {
    currentPage = static_cast<Page>((currentPage + 1) % PAGE_COUNT);
    pageChanged = true;
    Serial.print("[BTN] Switched to page ");
    Serial.println(currentPage);
  }

  unsigned long now = millis();


  if (pageChanged || now - lastRefresh >= REFRESH_INTERVAL_MS) {
    lastRefresh = now;
    heartbeat = !heartbeat; 
    switch (currentPage) {
      case PAGE_TIME: showTimePage(); break;
      case PAGE_ENV: showEnvPage(); break;
      case PAGE_IMU: showImuPage(); break;
      case PAGE_GAS: showGasPage(); break;
      default: break;
    }
  }
}
