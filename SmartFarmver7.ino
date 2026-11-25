#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

#include "config.h"
#include "sensors.h"
#include "alerts.h"
#include "soil_ads.h"
#include "flow.h"
#include "rs485_bus.h"
#include "ec_profile.h"
#include "soil_logic.h"
#include "water_quality.h"
#include "t2_batch.h"
#include "irrigation.h"
#include "mqtt_manager.h"
#include "safety.h"

// =========================================================================
// WIFI & MQTT
// =========================================================================
const char* WIFI_SSID     = "Natsu_2.4";
const char* WIFI_PASSWORD = "natsu3138";
const char* MQTT_SERVER   = "192.168.1.38";
const uint16_t MQTT_PORT  = 1883;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ค่าคาลิเบรตความชื้นดิน (4 โซน)
const int16_t SOIL_RAW_DRY[SOIL_N] = { 26000, 26000, 26000, 26000 };
const int16_t SOIL_RAW_WET[SOIL_N] = { 12000, 12000, 12000, 12000 };

// ส่ง MQTT จริง (ฟังก์ชันที่ไฟล์ .h อื่นเรียกใช้)
void mqttPublish(const char* topic, const String& payload) {
  if (mqttClient.connected()) {
    if (!mqttClient.publish(topic, payload.c_str())) {
      Serial.printf("[MQTT] publish ล้มเหลว [%s] ขนาด %u\n", topic, (unsigned)payload.length());
    }
  } else {
    Serial.printf("[MQTT] offline ข้าม [%s]\n", topic);
  }
}

// เชื่อมต่อ Wi-Fi ตอนบูต (รอได้สูงสุด 20 วินาที)
void setupWiFi() {
  Serial.printf("\n[WIFI] Connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WIFI] Connection timeout, continuing offline mode...");
  }
}

void reconnectMQTT() {
  Serial.print("[MQTT] Attempting connection...");
  String clientId = "SmartPomeloClient-" + String((uint32_t)esp_random(), HEX);

  if (mqttClient.connect(clientId.c_str())) {
    Serial.println(" connected");
    mqttClient.subscribe("smartfarm/control");
    mqttClient.subscribe("smartfarm/cmd/ec_profile");
    mqttClient.subscribe("smartfarm/cmd/safety_reset");
    mqttClient.subscribe("smartfarm/cmd/t2_reset");
  } else {
    Serial.printf(" failed, rc=%d\n", mqttClient.state());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.printf("[MQTT] Message arrived [%s]: %s\n", topic, message.c_str());
  mqttHandleCommand(String(topic), message);
}

// จัดการเครือข่ายแบบไม่บล็อก: ถ้าเน็ตหลุด ระบบควบคุมและ safety ยังทำงานต่อ
void networkTick() {
  static uint32_t lastTry = 0;
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastTry >= 10000UL) {
      lastTry = now;
      Serial.println("[WIFI] reconnecting...");
      WiFi.reconnect();
    }
    return;
  }
  if (!mqttClient.connected()) {
    if (now - lastTry >= 5000UL) {
      lastTry = now;
      reconnectMQTT();
    }
    return;
  }
  mqttClient.loop();
}

// ตั้งขารีเลย์ทั้งหมดเป็น OUTPUT และปิดก่อนเสมอ
static void relaysInit() {
  const uint8_t pins[] = { O_T2_SV1, O_T2_SV2, O_T2_SV3, O_T1_VALVE,
                           O_T1_WATER, O_T2_WATER, O_T2_IRRIG,
                           O_T1_FERT, O_T2_DOSE, O_T2_MIXER, O_T2_DRAIN };
  for (uint8_t p : pins) {
    digitalWrite(p, RELAY_OFF);   // ตั้งระดับก่อน แล้วค่อยเปลี่ยนเป็น OUTPUT กันรีเลย์กระตุก
    pinMode(p, OUTPUT);
    digitalWrite(p, RELAY_OFF);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[SYSTEM] Initializing Smart Pomelo Ver.6...");

  // 1. ปิดรีเลย์ทั้งหมดก่อนทำอย่างอื่น
  relaysInit();
  setupSafety();

  // 2. I2C (ADS1115 ทั้งสองตัว)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // 3. เครือข่ายและเวลา
  setupWiFi();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");   // เวลาไทย UTC+7

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);      // ค่าเริ่มต้น 256 ไบต์ ไม่พอสำหรับ telemetry
  mqttClient.setSocketTimeout(3);

  // 4. เซนเซอร์และระบบหลัก
  pinMode(MIX_LOW_PIN, INPUT_PULLUP);
  setupSoilADS();
  setupWaterQuality();
  setupFlow();
  setupRS485();
  loadFertHistory();
  ecProfileLoad();
  t1AllOff();
  t2AllOff();

  Serial.println("[SYSTEM] Setup completed. Entering main loop.");
}

void loop() {
  safetyTick();      // ต้องอยู่บรรทัดแรกเสมอ
  networkTick();

  flowTick();

  // ความชื้นดิน (ADS1115) ทุก 5 วินาที
  static uint32_t lastSoilCheck = 0;
  if (millis() - lastSoilCheck >= 5000UL) {
    lastSoilCheck = millis();
    readSoilAll();
  }

  // EC และอุณหภูมิน้ำในถังผสม
  waterQualityTick();

  // เซนเซอร์อากาศ (RS485 ID 1) ทุก 5 วินาที
  static uint32_t lastAirCheck = 0;
  if (millis() - lastAirCheck >= 5000UL) {
    lastAirCheck = millis();
    float airTemp = 0, airHum = 0;
    Health airSt = rs485GetAir(airTemp, airHum);
    if (airSt == H_OK) Serial.printf("[AIR] Temp: %.1f°C | RH: %.1f%%\n", airTemp, airHum);
    else               Serial.printf("[AIR] Status: %s\n", rsHealthStr(airSt));
  }

  // เซนเซอร์ดิน 7-in-1 (RS485 ID 3) ทุก 5 วินาที: ใช้ T / RH / EC
  static uint32_t lastSoil7Check = 0;
  if (millis() - lastSoil7Check >= 5000UL) {
    lastSoil7Check = millis();
    float t = 0, rh = 0, ec = 0;
    Health soil7St = rs485GetSoil7in1(t, rh, ec);
    if (soil7St == H_OK) Serial.printf("[SOIL-7in1] T: %.1f°C | RH: %.1f%% | EC: %.0f uS/cm\n", t, rh, ec);
    else                 Serial.printf("[SOIL-7in1] Status: %s\n", rsHealthStr(soil7St));
  }

  soilLogicTick();

  // ปรับเป้า EC ตามสัปดาห์ ทุก 60 วินาที
  static uint32_t lastEc = 0;
  if (millis() - lastEc >= 60000UL) {
    lastEc = millis();
    ecProfileUpdate();
  }

  t1Tick();
  t2AutoTick();
  t2Tick();

  // ส่ง telemetry ทุก 10 วินาที
  static uint32_t lastTelemetry = 0;
  if (millis() - lastTelemetry >= 10000UL) {
    lastTelemetry = millis();
    sendTelemetryToGrafana();
  }
}