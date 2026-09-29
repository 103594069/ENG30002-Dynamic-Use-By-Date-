#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include <RTClib.h>

#define sensor_t camera_sensor_t
#include "esp_camera.h"
#undef sensor_t
#include "img_converters.h"

// ==================== PINS ====================

#define SPI_SCK 14
#define SPI_MOSI 15
#define SPI_MISO 2

#define BACKUP_SD_CS 13
#define DISPLAY_SD_CS 33

#define TFT_CS 0
#define TFT_DC 1
#define TFT_RST -1

#define I2C_SDA 26
#define I2C_SCL 27
#define MCP_ADDR 0x20

#define MCP_ACTION 0
#define MCP_PAGE 1
#define MCP_PHOTO_LED 2

#define MQ135_PIN 32

#define CAM_XCLK 21
#define CAM_SIOD 26
#define CAM_SIOC 27
#define CAM_Y9 35
#define CAM_Y8 34
#define CAM_Y7 39
#define CAM_Y6 36
#define CAM_Y5 19
#define CAM_Y4 18
#define CAM_Y3 5
#define CAM_Y2 4
#define CAM_VSYNC 25
#define CAM_HREF 23
#define CAM_PCLK 22
#define CAM_PWDN -1
#define CAM_RESET -1

const unsigned long DEBOUNCE_MS = 40;
const unsigned long LONG_PRESS_MS = 2000;
const unsigned long BOTH_HOLD_MS = 5000;

// ==================== OBJECTS ====================

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
Adafruit_MCP23X17 mcp;
Adafruit_BME280 bme;
BH1750 lightMeter;
RTC_DS3231 rtc;

// ==================== STATE ====================

bool backupSDReady = false;
bool displaySDReady = false;
bool backupWritable = false;
bool displayWritable = false;
bool backupOnlyMode = false;

bool mcpReady = false;
bool bmeReady = false;
bool bhReady = false;
bool rtcReady = false;
bool cameraReady = false;

struct SensorData {
  float temperature;
  float humidity;
  float pressure;
  float lux;
  int mq135;
  DateTime timestamp;
};

SensorData lastReading;

bool previousAction = HIGH;
bool previousPage = HIGH;
unsigned long actionPressedAt = 0;
unsigned long pagePressedAt = 0;
unsigned long bothPressedAt = 0;
bool bothGestureTriggered = false;

uint8_t currentPage = 0;
const uint8_t PAGE_COUNT = 4;

// ==================== DISPLAY ====================

void messageScreen(const char *title, const char *a, const char *b = nullptr, const char *c = nullptr) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextWrap(true);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(4, 4);
  tft.println(title);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(4, 34);
  if (a) { tft.println(a); tft.println(); }
  if (b) { tft.println(b); tft.println(); }
  if (c) tft.println(c);
}

void bootHeader() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(4, 3);
  tft.print("FOOD MONITOR");
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(4, 21);
  tft.print("Hardware self-test");
}

void bootStatus(uint8_t row, const char *name, bool good) {
  int y = 34 + row * 10;
  tft.fillRect(0, y, 160, 10, ST7735_BLACK);
  tft.setCursor(4, y);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.print(name);
  tft.setCursor(128, y);
  tft.setTextColor(good ? ST7735_GREEN : ST7735_RED);
  tft.print(good ? "OK" : "FAIL");
}

void bootWorking(uint8_t row, const char *name) {
  int y = 34 + row * 10;
  tft.fillRect(0, y, 160, 10, ST7735_BLACK);
  tft.setCursor(4, y);
  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.print(name);
  tft.setCursor(112, y);
  tft.setTextColor(ST7735_YELLOW);
  tft.print("TEST...");
}

// ==================== SD ====================

void deselectEverything() {
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(BACKUP_SD_CS, HIGH);
  digitalWrite(DISPLAY_SD_CS, HIGH);
}

bool mountCard(uint8_t cs) {
  SD.end();
  deselectEverything();
  delay(10);

  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);

  // Start conservatively at 1 MHz.
  bool ok = SD.begin(cs, SPI, 1000000);

  if (!ok) {
    SD.end();
    deselectEverything();
    return false;
  }

  return true;
}

void unmountCard() {
  SD.end();
  deselectEverything();
  delay(5);
}

bool testCard(uint8_t cs) {
  if (!mountCard(cs)) return false;

  SD.remove("/BOOTTEST.TXT");
  File f = SD.open("/BOOTTEST.TXT", FILE_WRITE);

  if (!f) {
    unmountCard();
    return false;
  }

  const char msg[] = "Food Monitor SD test\n";
  size_t written = f.write((const uint8_t *)msg, sizeof(msg) - 1);
  f.close();

  bool ok = written == sizeof(msg) - 1;
  SD.remove("/BOOTTEST.TXT");
  unmountCard();

  return ok;
}

bool ensureCSV() {
  if (SD.exists("/TESTLOG.CSV")) return true;

  File f = SD.open("/TESTLOG.CSV", FILE_WRITE);
  if (!f) return false;

  f.println("date,time,temp_C,humidity_pct,pressure_hPa,lux,mq135,source");
  f.close();
  return true;
}

bool writeMeasurementToCard(uint8_t cs, const SensorData &d, const char *source) {
  if (!mountCard(cs)) return false;

  if (!ensureCSV()) {
    unmountCard();
    return false;
  }

  File f = SD.open("/TESTLOG.CSV", FILE_APPEND);
  if (!f) {
    unmountCard();
    return false;
  }

  char line[200];
  snprintf(line, sizeof(line),
    "%04d-%02d-%02d,%02d:%02d:%02d,%.2f,%.2f,%.2f,%.2f,%d,%s",
    d.timestamp.year(), d.timestamp.month(), d.timestamp.day(),
    d.timestamp.hour(), d.timestamp.minute(), d.timestamp.second(),
    d.temperature, d.humidity, d.pressure, d.lux, d.mq135, source);

  size_t written = f.println(line);
  f.close();
  unmountCard();
  return written > 0;
}

bool writeJPEGToCard(uint8_t cs, const char *filename, uint8_t *buf, size_t len) {
  if (!mountCard(cs)) return false;

  SD.remove(filename);
  File f = SD.open(filename, FILE_WRITE);

  if (!f) {
    unmountCard();
    return false;
  }

  size_t written = f.write(buf, len);
  f.close();
  unmountCard();
  return written == len;
}

// ==================== CAMERA ====================

bool initCamera() {
  camera_config_t c = {};

  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;

  c.pin_d0 = CAM_Y2;
  c.pin_d1 = CAM_Y3;
  c.pin_d2 = CAM_Y4;
  c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6;
  c.pin_d5 = CAM_Y7;
  c.pin_d6 = CAM_Y8;
  c.pin_d7 = CAM_Y9;

  c.pin_xclk = CAM_XCLK;
  c.pin_pclk = CAM_PCLK;
  c.pin_vsync = CAM_VSYNC;
  c.pin_href = CAM_HREF;

  c.pin_sccb_sda = -1;
  c.pin_sccb_scl = -1;
  c.sccb_i2c_port = 0;

  c.pin_pwdn = CAM_PWDN;
  c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = 10000000;

  c.pixel_format = PIXFORMAT_RGB565;
  c.frame_size = FRAMESIZE_QVGA;
  c.jpeg_quality = 10;
  c.fb_count = 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  return esp_camera_init(&c) == ESP_OK;
}

// ==================== SENSORS ====================

SensorData readSensors() {
  SensorData d;
  d.temperature = bmeReady ? bme.readTemperature() : NAN;
  d.humidity = bmeReady ? bme.readHumidity() : NAN;
  d.pressure = bmeReady ? bme.readPressure() / 100.0F : NAN;
  d.lux = bhReady ? lightMeter.readLightLevel() : NAN;
  d.mq135 = analogRead(MQ135_PIN);
  d.timestamp = rtcReady ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);
  return d;
}

// ==================== MEASUREMENT ====================

void takeMeasurement() {
  messageScreen("MEASURING", "Reading sensors...");
  delay(100);

  lastReading = readSensors();

  bool b = backupSDReady && backupWritable &&
    writeMeasurementToCard(BACKUP_SD_CS, lastReading, "MANUAL");

  bool p = displaySDReady && displayWritable &&
    writeMeasurementToCard(DISPLAY_SD_CS, lastReading, "MANUAL");

  if (backupSDReady && !b) backupWritable = false;
  if (displaySDReady && !p) displayWritable = false;

  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_GREEN);
  tft.setCursor(4, 4);
  tft.println("MEASURED");

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(4, 35);

  tft.print("MQ135: ");
  tft.println(lastReading.mq135);

  tft.print("Temp: ");
  tft.print(lastReading.temperature, 1);
  tft.println(" C");

  tft.println();
  tft.print("Primary: ");
  tft.setTextColor(p ? ST7735_GREEN : ST7735_RED);
  tft.println(p ? "SAVED" : "FAIL");

  tft.setTextColor(ST7735_WHITE);
  tft.print("Backup: ");
  tft.setTextColor(b ? ST7735_GREEN : ST7735_RED);
  tft.println(b ? "SAVED" : "FAIL");

  delay(1800);
}

// ==================== PHOTO ====================

void takePhoto() {
  if (!cameraReady) {
    messageScreen("CAMERA", "Camera unavailable.");
    delay(1500);
    return;
  }

  messageScreen("PHOTO", "Illumination on...", "Capturing...");

  if (mcpReady) mcp.digitalWrite(MCP_PHOTO_LED, HIGH);
  delay(300);

  camera_fb_t *discard = esp_camera_fb_get();
  if (discard) esp_camera_fb_return(discard);

  camera_fb_t *frame = esp_camera_fb_get();

  if (!frame) {
    if (mcpReady) mcp.digitalWrite(MCP_PHOTO_LED, LOW);
    messageScreen("PHOTO FAIL", "No camera frame.");
    delay(1500);
    return;
  }

  uint8_t *jpg = nullptr;
  size_t len = 0;

  bool converted = frame2jpg(frame, 80, &jpg, &len);
  esp_camera_fb_return(frame);

  if (mcpReady) mcp.digitalWrite(MCP_PHOTO_LED, LOW);

  if (!converted || !jpg || !len) {
    messageScreen("PHOTO FAIL", "JPEG conversion failed.");
    delay(1500);
    return;
  }

  DateTime now = rtcReady ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);

  char filename[48];
  snprintf(filename, sizeof(filename),
    "/IMG_%04d%02d%02d_%02d%02d%02d.jpg",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second());

  bool b = backupSDReady && backupWritable &&
    writeJPEGToCard(BACKUP_SD_CS, filename, jpg, len);

  bool p = displaySDReady && displayWritable &&
    writeJPEGToCard(DISPLAY_SD_CS, filename, jpg, len);

  if (backupSDReady && !b) backupWritable = false;
  if (displaySDReady && !p) displayWritable = false;

  free(jpg);

  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_GREEN);
  tft.setCursor(4, 4);
  tft.println("PHOTO DONE");

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(4, 34);
  tft.println(filename);
  tft.println();

  tft.print("Primary: ");
  tft.setTextColor(p ? ST7735_GREEN : ST7735_RED);
  tft.println(p ? "SAVED" : "FAIL");

  tft.setTextColor(ST7735_WHITE);
  tft.print("Backup: ");
  tft.setTextColor(b ? ST7735_GREEN : ST7735_RED);
  tft.println(b ? "SAVED" : "FAIL");

  delay(2000);
}

// ==================== PAGES ====================

void drawSystemPage() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(4, 3);
  tft.println("SYSTEM");

  struct Item { const char *name; bool ok; };

  Item items[] = {
    {"Primary SD", displaySDReady && displayWritable},
    {"Backup SD", backupSDReady && backupWritable},
    {"MCP23017", mcpReady},
    {"BME280", bmeReady},
    {"BH1750", bhReady},
    {"DS3231", rtcReady},
    {"Camera", cameraReady}
  };

  tft.setTextSize(1);
  int y = 29;

  for (int i = 0; i < 7; i++) {
    tft.setCursor(5, y);
    tft.setTextColor(ST7735_WHITE);
    tft.print(items[i].name);
    tft.setCursor(125, y);
    tft.setTextColor(items[i].ok ? ST7735_GREEN : ST7735_RED);
    tft.print(items[i].ok ? "OK" : "FAIL");
    y += 11;
  }

  if (backupOnlyMode) {
    tft.setTextColor(ST7735_YELLOW);
    tft.setCursor(5, 111);
    tft.print("BACKUP ONLY MODE");
  }
}

void drawSensorPage() {
  lastReading = readSensors();

  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(4, 3);
  tft.println("SENSORS");

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);

  int y = 31;

  tft.setCursor(5, y);
  tft.print("Temp: ");
  tft.print(lastReading.temperature, 2);
  tft.println(" C");
  y += 15;

  tft.setCursor(5, y);
  tft.print("Humidity: ");
  tft.print(lastReading.humidity, 2);
  tft.println(" %");
  y += 15;

  tft.setCursor(5, y);
  tft.print("Pressure: ");
  tft.print(lastReading.pressure, 1);
  tft.println(" hPa");
  y += 15;

  tft.setCursor(5, y);
  tft.print("Light: ");
  tft.print(lastReading.lux, 1);
  tft.println(" lux");
  y += 15;

  tft.setCursor(5, y);
  tft.print("MQ135: ");
  tft.setTextColor(ST7735_YELLOW);
  tft.println(lastReading.mq135);
}

void drawStoragePage() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(4, 3);
  tft.println("STORAGE");

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);

  tft.setCursor(5, 33);
  tft.print("Primary SD: ");
  tft.setTextColor(displaySDReady && displayWritable ? ST7735_GREEN : ST7735_RED);
  tft.println(displaySDReady && displayWritable ? "ONLINE" : "OFFLINE");

  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(5, 51);
  tft.print("Backup SD: ");
  tft.setTextColor(backupSDReady && backupWritable ? ST7735_GREEN : ST7735_RED);
  tft.println(backupSDReady && backupWritable ? "ONLINE" : "OFFLINE");

  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(5, 75);
  tft.println("SCK14 MOSI15 MISO2");

  tft.setCursor(5, 90);
  tft.println("Primary CS33");

  tft.setCursor(5, 105);
  tft.println("Backup CS13");
}

void drawControlsPage() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(4, 3);
  tft.println("CONTROLS");

  tft.setTextSize(1);
  tft.setTextColor(ST7735_WHITE);

  tft.setCursor(5, 34);
  tft.println("PAGE   = GPA1");

  tft.setCursor(5, 51);
  tft.println("ACTION = GPA0");

  tft.setCursor(5, 73);
  tft.println("ACTION tap:");

  tft.setCursor(15, 85);
  tft.println("Take measurement");

  tft.setCursor(5, 101);
  tft.println("ACTION hold:");

  tft.setCursor(15, 113);
  tft.println("Take photo");
}

void drawCurrentPage() {
  switch (currentPage) {
    case 0: drawSystemPage(); break;
    case 1: drawSensorPage(); break;
    case 2: drawStoragePage(); break;
    case 3: drawControlsPage(); break;
  }
}

// ==================== DEGRADED MODE ====================

bool askBackupOnly() {
  messageScreen("PRIMARY SD", "Display SD not found.", "ACTION = backup only", "PAGE = retry");

  while (true) {
    bool action = mcp.digitalRead(MCP_ACTION);
    bool page = mcp.digitalRead(MCP_PAGE);

    if (action == LOW) {
      delay(50);
      while (mcp.digitalRead(MCP_ACTION) == LOW) delay(10);
      backupOnlyMode = true;
      return true;
    }

    if (page == LOW) {
      delay(50);
      while (mcp.digitalRead(MCP_PAGE) == LOW) delay(10);

      messageScreen("RETRYING", "Checking display SD...");
      delay(300);

      displaySDReady = testCard(DISPLAY_SD_CS);
      displayWritable = displaySDReady;

      if (displaySDReady) {
        backupOnlyMode = false;
        return true;
      }

      messageScreen("PRIMARY SD", "Still not found.", "ACTION = backup only", "PAGE = retry");
    }

    delay(10);
  }
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(250);

  pinMode(TFT_CS, OUTPUT);
  pinMode(BACKUP_SD_CS, OUTPUT);
  pinMode(DISPLAY_SD_CS, OUTPUT);

  deselectEverything();

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);

  Serial.flush();
  Serial.end();

  // TFT first so we can see exactly where startup stops.
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  bootHeader();

  bootWorking(0, "Backup SD");
  backupSDReady = testCard(BACKUP_SD_CS);
  backupWritable = backupSDReady;
  bootStatus(0, "Backup SD", backupSDReady);

  bootWorking(1, "Display SD");
  displaySDReady = testCard(DISPLAY_SD_CS);
  displayWritable = displaySDReady;
  bootStatus(1, "Display SD", displaySDReady);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  bootWorking(2, "MCP23017");
  mcpReady = mcp.begin_I2C(MCP_ADDR, &Wire);

  if (mcpReady) {
    mcp.pinMode(MCP_ACTION, INPUT_PULLUP);
    mcp.pinMode(MCP_PAGE, INPUT_PULLUP);
    mcp.pinMode(MCP_PHOTO_LED, OUTPUT);
    mcp.digitalWrite(MCP_PHOTO_LED, LOW);
  }

  bootStatus(2, "MCP23017", mcpReady);

  if (!mcpReady) {
    messageScreen("FATAL", "MCP23017 missing.", "Check I2C wiring.");
    while (true) delay(1000);
  }

  bootWorking(3, "BME280");
  bmeReady = bme.begin(0x76, &Wire);
  if (!bmeReady) bmeReady = bme.begin(0x77, &Wire);
  bootStatus(3, "BME280", bmeReady);

  bootWorking(4, "BH1750");
  bhReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  bootStatus(4, "BH1750", bhReady);

  bootWorking(5, "DS3231");
  rtcReady = rtc.begin(&Wire);
  bootStatus(5, "DS3231", rtcReady);

  bootWorking(6, "MQ135");
  pinMode(MQ135_PIN, INPUT);
  lastReading = readSensors();
  bootStatus(6, "MQ135", true);

  bootWorking(7, "Camera");
  cameraReady = initCamera();
  bootStatus(7, "Camera", cameraReady);

  delay(1500);

  if (!displaySDReady) {
    if (!backupSDReady) {
      messageScreen("NO STORAGE", "Both SD cards failed.", "Check SPI wiring.");
      while (true) delay(1000);
    }

    askBackupOnly();
  }

  messageScreen(
    "READY",
    backupOnlyMode ? "Running backup only." : "Dual SD operational.",
    "PAGE = screens",
    "ACTION = measurement"
  );

  delay(2000);
  drawCurrentPage();
}

// ==================== LOOP ====================

void loop() {
  bool action = mcp.digitalRead(MCP_ACTION);
  bool page = mcp.digitalRead(MCP_PAGE);
  unsigned long now = millis();

  if (action == LOW && page == LOW) {
    if (!bothPressedAt) bothPressedAt = now;

    if (!bothGestureTriggered && now - bothPressedAt >= BOTH_HOLD_MS) {
      bothGestureTriggered = true;
      messageScreen("MAINTENANCE", "Both-button gesture", "detected.", "FORMAT DISABLED TEST");
    }
  } else {
    if (bothGestureTriggered) {
      delay(1000);
      drawCurrentPage();
    }

    bothPressedAt = 0;
    bothGestureTriggered = false;
  }

  if (action == LOW && page == LOW) {
    previousAction = action;
    previousPage = page;
    delay(10);
    return;
  }

  if (previousPage == HIGH && page == LOW) pagePressedAt = now;

  if (previousPage == LOW && page == HIGH && now - pagePressedAt >= DEBOUNCE_MS) {
    currentPage = (currentPage + 1) % PAGE_COUNT;
    drawCurrentPage();
  }

  if (previousAction == HIGH && action == LOW) actionPressedAt = now;

  if (previousAction == LOW && action == HIGH) {
    unsigned long held = now - actionPressedAt;

    if (held >= DEBOUNCE_MS) {
      if (held >= LONG_PRESS_MS) takePhoto();
      else takeMeasurement();

      drawCurrentPage();
    }
  }

  previousAction = action;
  previousPage = page;
  delay(10);
}