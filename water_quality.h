#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sensors.h"
#include "alerts.h"

// ---------- ฮาร์ดแวร์ถังผสม ----------
Adafruit_ADS1115 adsMix;           // ADS1115 (0x48) ช่อง A1 = EC (ช่อง A0 เดิมคือ pH ว่างไว้)
OneWire oneWire(TEMP_PIN);
DallasTemperature waterTempSensor(&oneWire);
static bool adsMixOk = false;

// ค่าที่วัดได้
float waterTempC = 25.0f;
float ecVal      = 0.0f;

// สถานะเซนเซอร์
Health ecHealth   = H_NOT_FOUND;
Health tempHealth = H_NOT_FOUND;

static uint8_t ecBad = 0, tempBad = 0;
static float ecEver = 0.0f, tempEver = 25.0f;
static float ecSpk  = 0.0f, tempSpk  = 0.0f;

// กรองค่ากระโดด (Spike Rejection)
static Health acceptWQ(bool validRaw, float val, float minV, float maxV,
                       float& ever, float& runningVal, float& spkAcc) {
  if (!validRaw) return H_NOT_FOUND;
  if (val < minV || val > maxV) return H_OUT_OF_RANGE;

  float diff = fabsf(val - ever);
  if (diff > 1.5f) {
    spkAcc += diff;
    ever = 0.8f * ever + 0.2f * val;
    if (spkAcc > 4.5f) { runningVal = ever; return H_SPIKE; }
  } else {
    spkAcc = max(0.0f, spkAcc - 0.5f);
  }
  ever = 0.7f * ever + 0.3f * val;
  runningVal = val;
  return H_OK;
}

void setupWaterQuality() {
  if (adsMix.begin(0x48, &Wire)) {
    adsMix.setGain(GAIN_ONE);       // ±4.096V
    adsMixOk = true;
    Serial.println("[WQ] ADS1115 (ถังผสม) พร้อมทำงาน");
  } else {
    adsMixOk = false;
    Serial.println("[WQ] ไม่พบ ADS1115 ที่ 0x48!");
  }

  waterTempSensor.begin();
  waterTempSensor.setWaitForConversion(false);   // Non-blocking
  waterTempSensor.requestTemperatures();
}

void readWaterQuality() {
  static uint32_t lastTempReq = 0;
  uint32_t now = millis();

  // 1. อุณหภูมิน้ำ (DS18B20)
  if (now - lastTempReq >= 1000) {
    float t = waterTempSensor.getTempCByIndex(0);
    waterTempSensor.requestTemperatures();
    lastTempReq = now;

    if (t != DEVICE_DISCONNECTED_C && t > -10 && t < 80) {
      tempHealth = acceptWQ(true, t, 0, 60, tempEver, waterTempC, tempSpk);
    } else {
      tempHealth = H_NOT_FOUND;
    }
  }

  // 2. EC จาก ADS1115 ช่อง A1 (ชดเชยอุณหภูมิที่ 25°C)
  if (!adsMixOk) { ecHealth = H_NOT_FOUND; return; }
  int16_t rawEc = adsMix.readADC_SingleEnded(1);
  float ve = adsMix.computeVolts(rawEc);
  float rawEcVal = (ve * 1.0f);                  // 1.0f = EC_K ต้องคาลิเบรตกับน้ำยามาตรฐาน
  float ec = rawEcVal / (1.0f + 0.02f * (waterTempC - 25.0f));
  ecHealth = acceptWQ(rawEc >= 0 && rawEc < 32000, ec, 0, 20, ecEver, ecVal, ecSpk);
}

void checkWaterQuality() {
  ecBad   = (ecHealth == H_OK)   ? 0 : ecBad + 1;
  tempBad = (tempHealth == H_OK) ? 0 : tempBad + 1;

  if (ecBad >= SENSOR_FAULT_CNT) {
    notifyAlert("ec_sensor", String("เซนเซอร์ EC ถังผสมผิดปกติ: ") + healthStr(ecHealth));
    ecBad = 0;
  }
  if (tempBad >= SENSOR_FAULT_CNT) {
    notifyAlert("temp_sensor", String("เซนเซอร์อุณหภูมิน้ำผิดปกติ: ") + healthStr(tempHealth));
    tempBad = 0;
  }
}

// เรียกใน loop() ทุกรอบ ทำงานจริงวินาทีละครั้ง
void waterQualityTick() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  readWaterQuality();
  checkWaterQuality();
}

void fillWaterQualityTelemetry(JsonDocument& d) {
  d["water_temp"] = (tempHealth == H_OK) ? roundf(waterTempC * 10) / 10 : (float)NAN;
  d["mix_ec"]     = (ecHealth == H_OK)   ? roundf(ecVal * 100) / 100     : (float)NAN;
}