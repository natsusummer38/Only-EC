#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include "alerts.h"

// ---------- โครงสร้างเวลาสำหรับ T1 Schedule ----------
struct HM { uint8_t hour; uint8_t minute; };

// ตารางเวลาให้น้ำของ T1 (ปรับแก้เวลาตามจริงได้)
static const HM T1_WATER_SCHEDULE[] = { {9, 30}, {12, 0}, {14, 0}, {16, 30} };
static const uint8_t T1_WATER_COUNT = sizeof(T1_WATER_SCHEDULE) / sizeof(HM);

// ตารางเวลาให้ปุ๋ยของ T1
static const HM T1_FERT_SCHEDULE[]  = { {7, 0} };
static const uint8_t T1_FERT_COUNT  = sizeof(T1_FERT_SCHEDULE) / sizeof(HM);

// ระยะเวลาทำงานของ T1
#define T1_WATER_DUR_S    180UL
#define T1_FERT_DUR_S     60UL
#define T1_PRIME_MS       2000UL   // เปิดวาล์วก่อนเปิดปั๊ม 2 วินาที
#define T1_DRAIN_MS       3000UL   // ปิดปั๊มแล้วหน่วงก่อนปิดวาล์ว

// ---------- State Machine ของ T1 ----------
enum LineSt : uint8_t { LS_IDLE, LS_PRIME, LS_RUN, LS_DRAIN };
static const char* LS_NAMES[] = { "IDLE", "PRIME", "RUN", "DRAIN" };

struct T1Job {
  bool isFert = false;
  uint32_t durationMs = 0;
} t1Job;

LineSt t1State = LS_IDLE;
static uint32_t t1Since = 0;
static int16_t lastWaterMin = -1, lastFertMin = -1;   // นาทีของวัน 0-1439 ต้องใช้ int16_t

static inline void t1Rly(uint8_t pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

void t1AllOff() {
  t1Rly(O_T1_VALVE, false);
  t1Rly(O_T1_WATER, false);
  t1Rly(O_T1_FERT, false);
}

static void t1Go(LineSt s) {
  t1State = s;
  t1Since = millis();
  Serial.printf("[T1] State -> %s\n", LS_NAMES[s]);
}

// เริ่มงาน T1 (น้ำ หรือ ปุ๋ย)
bool t1Start(bool isFert) {
  if (estopActive || t1State != LS_IDLE) return false;
  t1Job.isFert = isFert;
  t1Job.durationMs = (isFert ? T1_FERT_DUR_S : T1_WATER_DUR_S) * 1000UL;

  Serial.printf("[T1] เริ่มทำงาน: %s (ระยะเวลา %lu วินาที)\n",
                isFert ? "ให้ปุ๋ย" : "ให้น้ำ", (unsigned long)(t1Job.durationMs / 1000));

  t1Rly(O_T1_VALVE, true);   // เปิดวาล์วก่อนเสมอ
  t1Go(LS_PRIME);
  return true;
}

// เรียกใน loop() ทุกรอบ
void t1Tick() {
  uint32_t el = millis() - t1Since;

  switch (t1State) {
    case LS_IDLE: {
      struct tm tm;
      if (!getLocalTime(&tm, 10)) return;   // ยังไม่ได้เวลาจาก NTP

      int16_t curMin = tm.tm_hour * 60 + tm.tm_min;

      if (curMin != lastWaterMin) {
        for (uint8_t i = 0; i < T1_WATER_COUNT; i++) {
          if (tm.tm_hour == T1_WATER_SCHEDULE[i].hour && tm.tm_min == T1_WATER_SCHEDULE[i].minute) {
            lastWaterMin = curMin;
            t1Start(false);
            return;
          }
        }
      }

      if (curMin != lastFertMin) {
        for (uint8_t i = 0; i < T1_FERT_COUNT; i++) {
          if (tm.tm_hour == T1_FERT_SCHEDULE[i].hour && tm.tm_min == T1_FERT_SCHEDULE[i].minute) {
            lastFertMin = curMin;
            t1Start(true);
            return;
          }
        }
      }
      break;
    }

    case LS_PRIME:
      if (el >= T1_PRIME_MS) {
        if (t1Job.isFert) t1Rly(O_T1_FERT, true);
        else              t1Rly(O_T1_WATER, true);
        t1Go(LS_RUN);
      }
      break;

    case LS_RUN:
      if (el >= t1Job.durationMs) {
        t1Rly(O_T1_WATER, false);
        t1Rly(O_T1_FERT, false);
        t1Go(LS_DRAIN);
      }
      break;

    case LS_DRAIN:
      if (el >= T1_DRAIN_MS) {
        t1Rly(O_T1_VALVE, false);   // ปิดวาล์วเป็นขั้นตอนสุดท้าย

        JsonDocument doc;
        doc["system"]     = "T1";
        doc["type"]       = t1Job.isFert ? "fert" : "water";
        doc["duration_s"] = t1Job.durationMs / 1000;
        String payload; serializeJson(doc, payload);
        mqttPublish("smartfarm/water_event", payload);

        t1Go(LS_IDLE);
      }
      break;
  }
}

void fillT1Telemetry(JsonDocument& d) {
  d["t1_state"] = LS_NAMES[t1State];
  d["t1_valve"] = (digitalRead(O_T1_VALVE) == RELAY_ON);
  d["t1_water"] = (digitalRead(O_T1_WATER) == RELAY_ON);
  d["t1_fert"]  = (digitalRead(O_T1_FERT) == RELAY_ON);
}