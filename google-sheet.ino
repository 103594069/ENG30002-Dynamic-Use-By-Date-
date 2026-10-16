#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// ---------------- WIFI DETAILS ----------------

const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// ---------------- GOOGLE SHEET ----------------

String googleScriptURL =
"https://script.google.com/macros/s/AKfycbytbksBysBiYWpC7RWCml9sezy8kfqh0GU_VYWkZ2IRZziozBTN3H0n6Euy9t4h3hA2/exec";

// ---------------- MQ135 ----------------

// Change this pin if your MQ135 AO is connected somewhere else
#define MQ135_PIN 34

// ---------------- BME280 ----------------

Adafruit_BME280 bme;

// ---------------- SETTINGS ----------------

// Upload every 10 seconds
const unsigned long uploadInterval = 10000;

unsigned long previousUploadTime = 0;

void connectWiFi() {

  Serial.print("Connecting to WiFi");

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");

  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());
}

void sendToGoogleSheets(
  int mq135Raw,
  float temperature,
  float humidity,
  float pressure
) {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("WiFi disconnected");
    Serial.println("Trying to reconnect");

    WiFi.disconnect();
    connectWiFi();

    return;
  }

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  String url = googleScriptURL;

  url += "?mq135=";
  url += String(mq135Raw);

  url += "&temp=";
  url += String(temperature, 2);

  url += "&humidity=";
  url += String(humidity, 2);

  url += "&pressure=";
  url += String(pressure, 2);

  Serial.println();
  Serial.println("Sending data to Google Sheets");

  Serial.print("URL: ");
  Serial.println(url);

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (http.begin(client, url)) {

    int httpResponseCode = http.GET();

    Serial.print("HTTP response code: ");
    Serial.println(httpResponseCode);

    if (httpResponseCode > 0) {

      String response = http.getString();

      Serial.print("Google response: ");
      Serial.println(response);

    } else {

      Serial.println("Error sending data");
    }

    http.end();

  } else {

    Serial.println("Unable to connect to Google Apps Script");
  }
}

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("Food Spoilage Monitoring System");
  Serial.println("Starting system");

  // ---------------- MQ135 ----------------

  pinMode(MQ135_PIN, INPUT);

  // ---------------- BME280 ----------------

  Wire.begin();

  bool bmeStatus = bme.begin(0x76);

  if (!bmeStatus) {

    Serial.println("BME280 not detected at 0x76");
    Serial.println("Trying 0x77");

    bmeStatus = bme.begin(0x77);
  }

  if (bmeStatus) {

    Serial.println("BME280 detected successfully");

  } else {

    Serial.println("BME280 not found");
    Serial.println("Check BME280 wiring");
  }

  // ---------------- WIFI ----------------

  connectWiFi();

  Serial.println();
  Serial.println("System ready");
}

void loop() {

  if (millis() - previousUploadTime >= uploadInterval) {

    previousUploadTime = millis();

    // ---------------- READ MQ135 ----------------

    int mq135Raw = analogRead(MQ135_PIN);

    // ---------------- READ BME280 ----------------

    float temperature = bme.readTemperature();

    float humidity = bme.readHumidity();

    float pressure = bme.readPressure() / 100.0F;

    // ---------------- SERIAL MONITOR ----------------

    Serial.println();
    Serial.println("----------------------------");

    Serial.print("MQ135 Raw: ");
    Serial.println(mq135Raw);

    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println(" C");

    Serial.print("Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");

    Serial.print("Pressure: ");
    Serial.print(pressure);
    Serial.println(" hPa");

    // ---------------- SEND TO GOOGLE SHEETS ----------------

    sendToGoogleSheets(
      mq135Raw,
      temperature,
      humidity,
      pressure
    );

    Serial.println("----------------------------");
  }
}