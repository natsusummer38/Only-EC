#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include "config.h"

Adafruit_ADS1115 adsSoil;

float soilPct[SOIL_N] = {0};
Health soilHealth[SOIL_N] = {H_NOT_FOUND, H_NOT_FOUND, H_NOT_FOUND, H_NOT_FOUND};
bool adsInitialized = false; // ตัวแปรเช็กสถานะการเชื่อมต่อ

inline const char* soilHealthStr(Health h) {
  switch (h) {
    case H_OK:           return "OK";
    case H_TIMEOUT:      return "TIMEOUT";
    case H_NOT_FOUND:    return "NOT_FOUND";
    case H_OUT_OF_RANGE: return "OUT_OF_RANGE";
    default:             return "UNKNOWN";
  }
}

inline void setupSoilADS() {
  Wire.beginTransmission(ADS_SOIL_ADDR);
  byte error = Wire.endTransmission();
  
  if (error == 0) {
    if (adsSoil.begin(ADS_SOIL_ADDR)) {
      adsSoil.setGain(GAIN_ONE);
      adsInitialized = true;
      Serial.println("[SOIL] ADS1115 Soil Sensor initialized successfully.");
    } else {
      adsInitialized = false;
      Serial.println("[SOIL] Warning: ADS1115 found but failed to start.");
    }
  } else {
    adsInitialized = false;
    Serial.println("[SOIL] Notice: ADS1115 not detected.");
  }
}

inline void readSoilAll() {
  if (!adsInitialized) {
    Wire.beginTransmission(ADS_SOIL_ADDR);
    if (Wire.endTransmission() == 0) {
      if (adsSoil.begin(ADS_SOIL_ADDR)) {
        adsSoil.setGain(GAIN_ONE);
        adsInitialized = true;
        Serial.println("[SOIL] ADS1115 reconnected successfully!");
      }
    }
    return; 
  }

  for (uint8_t i = 0; i < SOIL_N; i++) {
    int16_t raw = adsSoil.readADC_SingleEnded(i);
    
    if (raw < 0) {
      soilHealth[i] = H_NOT_FOUND;
      soilPct[i] = 0.0f;
      continue;
    }

    if (raw < 100 || raw > 30000) {
      soilHealth[i] = H_OUT_OF_RANGE;
      continue;
    }

    float dry = (float)SOIL_RAW_DRY[i];
    float wet = (float)SOIL_RAW_WET[i];

    if (dry == wet) {
      soilPct[i] = 0.0f;
    } else {
      // แก้ไขวงเล็บเพื่อความถูกต้องแม่นยำในการคำนวณเปอร์เซ็นต์
      float pct = ((dry - (float)raw) / (dry - wet)) * 100.0f;
      if (pct < 0.0f) pct = 0.0f;
      if (pct > 100.0f) pct = 100.0f;
      soilPct[i] = pct;
    }

    soilHealth[i] = H_OK;
  }

  // แสดงผลลัพธ์ออก Serial Monitor
  Serial.print("[SOIL] ");
  for (uint8_t i = 0; i < SOIL_N; i++) {
    String label = (i == 0) ? "T1" : "Z" + String(i);
    if (soilHealth[i] == H_OK) {
      Serial.printf("%s=%.1f%%  ", label.c_str(), soilPct[i]);
    } else {
      Serial.printf("%s=%s  ", label.c_str(), soilHealthStr(soilHealth[i]));
    }
  }
  Serial.println();
}