#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "alerts.h"
#include "flow.h"
#include "rs485_bus.h"
#include "irrigation.h"
#include "t2_batch.h"
#include "water_quality.h"
#include "ec_profile.h"

bool safetyReset();   // อยู่ใน safety.h

// ส่งข้อมูลทั้งหมดไป Raspberry Pi -> InfluxDB (topic: smartfarm/telemetry/grafana)
// ค่าที่อ่านไม่ได้จะส่งเป็น null และฝั่ง Pi จะข้ามไป ไม่บันทึกเป็น 0
void sendTelemetryToGrafana() {
  JsonDocument doc;

  // 1. อากาศ (RS485 ID 1) ใช้ชื่อเดียวกับระบบเดิม
  float airT = NAN, airH = NAN;
  if (rs485GetAir(airT, airH) == H_OK) {
    doc["air_temp"] = roundf(airT * 10) / 10;
    doc["air_humi"] = roundf(airH * 10) / 10;
  } else {
    doc["air_temp"] = nullptr;
    doc["air_humi"] = nullptr;
  }

  // 2. เซนเซอร์ดิน 7-in-1 (RS485 ID 3): ความชื้น
  float s7T = NAN, s7H = NAN, s7Ec = NAN;
  if (rs485GetSoil7in1(s7T, s7H, s7Ec) == H_OK) {
    doc["soil7_rh"] = roundf(s7H * 10) / 10;
  } else {
    doc["soil7_rh"] = nullptr;
  }

  // 3. การไหลและปริมาตรสะสม
  doc["flow_water_lpm"]     = roundf(fmWater.lpm * 100) / 100;
  doc["flow_fert_lpm"]      = roundf(fmFert.lpm * 100) / 100;
  doc["total_water_liters"] = roundf(flowTotalL(fmWater) * 10) / 10;
  doc["total_fert_liters"]  = roundf(flowTotalL(fmFert) * 10) / 10;

  // 4. สถานะปั๊ม (Pi จะบันทึกเป็น pumps_t1_water = 0/1)
  JsonObject pumps = doc["pumps"].to<JsonObject>();
  pumps["t1_water"]      = (digitalRead(O_T1_WATER) == RELAY_ON);
  pumps["t1_fert"]       = (digitalRead(O_T1_FERT)  == RELAY_ON);
  pumps["t2_water"]      = (digitalRead(O_T2_WATER) == RELAY_ON);
  pumps["t2_dosing"]     = (digitalRead(O_T2_DOSE)  == RELAY_ON);
  pumps["t2_irrigation"] = (digitalRead(O_T2_IRRIG) == RELAY_ON);
  pumps["t2_mixer"]      = (digitalRead(O_T2_MIXER) == RELAY_ON);
  pumps["t2_drain"]      = (digitalRead(O_T2_DRAIN) == RELAY_ON);

  // 5. สถานะวาล์ว
  JsonObject valves = doc["valves"].to<JsonObject>();
  valves["t1_valve"] = (digitalRead(O_T1_VALVE) == RELAY_ON);
  valves["sv_1"]     = (digitalRead(O_T2_SV1) == RELAY_ON);
  valves["sv_2"]     = (digitalRead(O_T2_SV2) == RELAY_ON);
  valves["sv_3"]     = (digitalRead(O_T2_SV3) == RELAY_ON);

  // 6. สถานะระบบ
  doc["estop"]     = estopActive;
  doc["uptime_s"]  = millis() / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();

  // 7. ข้อมูลจากระบบย่อย (ความชื้นดิน, T1, T2, EC ถังผสม, เป้า EC)
  fillT1Telemetry(doc);
  fillT2Telemetry(doc);
  fillWaterQualityTelemetry(doc);
  fillEcTelemetry(doc);

  String payload;
  serializeJson(doc, payload);
  mqttPublish("smartfarm/telemetry/grafana", payload);
}

// รับคำสั่งจาก MQTT
void mqttHandleCommand(const String& topic, const String& msg) {
  if (topic == "smartfarm/cmd/ec_profile") {
    ecHandleCommand(msg);
  } else if (topic == "smartfarm/cmd/safety_reset") {
    Serial.println(safetyReset() ? "[CMD] safety reset OK"
                                 : "[CMD] safety reset ไม่ผ่าน (ปุ่ม E-Stop ยังกดอยู่)");
  } else if (topic == "smartfarm/cmd/t2_reset") {
    t2Reset();
  } else {
    Serial.printf("[CMD] ไม่รู้จักคำสั่ง %s\n", topic.c_str());
  }
}