#pragma once
#include <Arduino.h>
#include "config.h"
#include "alerts.h"
#include "irrigation.h"
#include "t2_batch.h"
#include "water_quality.h"

bool estopActive = false;
static String   safetyReason;
static uint32_t tEstop = 0, tPump = 0, tNoValve = 0, tEc = 0, tConflict = 0, tConflictT1 = 0;

static inline bool rOn(uint8_t pin) { return digitalRead(pin) == RELAY_ON; }

// คืน true เมื่อเงื่อนไข cond เป็นจริงต่อเนื่องครบ ms
static bool heldFor(bool cond, uint32_t& since, uint32_t ms, uint32_t now) {
  if (!cond) { since = 0; return false; }
  if (since == 0) since = now ? now : 1;
  return (now - since) >= ms;
}

static bool anyPumpOn() {
  return rOn(O_T1_WATER) || rOn(O_T1_FERT) ||
         rOn(O_T2_WATER) || rOn(O_T2_DOSE) || rOn(O_T2_IRRIG);
}

static bool anyLoadOn() {
  return anyPumpOn() || rOn(O_T2_MIXER) || rOn(O_T2_DRAIN) ||
         rOn(O_T1_VALVE) || rOn(O_T2_SV1) || rOn(O_T2_SV2) || rOn(O_T2_SV3);
}

static void safetyStopAll() {
  t1AllOff();
  t2AllOff();
  t1State = LS_IDLE;   // กัน state machine เดินต่อหลังตัดไฟ
  t2      = T2_IDLE;
}

static void safetyTrip(const char* key, const String& msg) {
  if (estopActive) return;
  estopActive  = true;
  safetyReason = key;
  safetyStopAll();
  Serial.printf("[SAFETY] TRIP %s : %s\n", key, msg.c_str());
  alertSend(key, "SAFETY: " + msg + " -> ปิดปั๊มและวาล์วทั้งหมดแล้ว", L_CRITICAL);
  mqttPublish("smartfarm/safety_event", String("{\"event\":\"") + key + "\"}");
}

void setupSafety() {
#if USE_ESTOP_BUTTON
  pinMode(ESTOP_PIN, INPUT_PULLUP);
#endif
}

bool estopButtonPressed() {
#if USE_ESTOP_BUTTON
  return digitalRead(ESTOP_PIN) == HIGH;   // NC: กดปุ่มหรือสายขาด = HIGH
#else
  return false;
#endif
}

// ปลดล็อก (เรียกจากคำสั่ง MQTT)
bool safetyReset() {
  if (estopButtonPressed()) return false;  // ต้องปล่อยปุ่มก่อน
  estopActive = false;
  safetyReason = "";
  tEstop = tPump = tNoValve = tEc = tConflict = tConflictT1 = 0;
  Serial.println("[SAFETY] reset");
  return true;
}

// เรียกเป็นบรรทัดแรกของ loop()
void safetyTick() {
  uint32_t now = millis();

  // 1) E-Stop (debounce 50 ms)
  if (heldFor(estopButtonPressed(), tEstop, 50, now)) {
    safetyTrip("estop", "กดปุ่มฉุกเฉินหรือสายขาด");
  }

  // ขณะล็อก: ถ้ามีโหลดเปิดอยู่ (เช่น บั๊กที่อื่นสั่งเปิด) ให้ตัดซ้ำ
  if (estopActive) {
    if (anyLoadOn()) safetyStopAll();
    return;
  }

  // 2) Interlock
#if SAFETY_T2_NO_WATER_WITH_DOSE
  if (heldFor(rOn(O_T2_WATER) && rOn(O_T2_DOSE), tConflict, 1000, now)) {
    safetyTrip("interlock", "ปั๊มน้ำและปั๊มปุ๋ยถังผสมทำงานพร้อมกัน");
    return;
  }
#endif
  if (heldFor(rOn(O_T1_WATER) && rOn(O_T1_FERT), tConflictT1, 1000, now)) {
    safetyTrip("interlock", "ปั๊มน้ำและปั๊มปุ๋ย T1 ทำงานพร้อมกัน");
    return;
  }

  // 3) ปั๊มเดินแต่ไม่มีวาล์วเปิด (กันปั๊มไหม้)
  bool t1Pump  = rOn(O_T1_WATER) || rOn(O_T1_FERT);
  bool t1Valve = rOn(O_T1_VALVE);
  bool t2Pump  = rOn(O_T2_IRRIG);
  bool t2Valve = rOn(O_T2_SV1) || rOn(O_T2_SV2) || rOn(O_T2_SV3);
  bool noValve = (t1Pump && !t1Valve) || (t2Pump && !t2Valve);
  if (heldFor(noValve, tNoValve, SAFETY_NOVALVE_MS, now)) {
    safetyTrip("no_valve", "ปั๊มทำงานแต่ไม่มีวาล์วเปิด");
    return;
  }

  // 4) Max runtime: ปั๊มใดๆ เปิดต่อเนื่องเกิน MAX_RUN_S
  if (heldFor(anyPumpOn(), tPump, MAX_RUN_S * 1000UL + 2000UL, now)) {
    safetyTrip("max_runtime", "ปั๊มทำงานต่อเนื่องเกิน " + String((uint32_t)MAX_RUN_S) + " วินาที");
    return;
  }

  // 5) EC ในถังผสมสูงเกินเพดานขณะจ่ายน้ำ
  bool ecHigh = (t2 == T2_RUN) && (ecHealth == H_OK) && (ecVal > SAFETY_EC_MAX);
  if (heldFor(ecHigh, tEc, SAFETY_EC_HOLD_MS, now)) {
    safetyTrip("ec_high", "EC " + String(ecVal, 2) + " สูงเกินเพดาน " + String(SAFETY_EC_MAX, 1));
    return;
  }
}