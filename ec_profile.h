#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include "alerts.h"

struct EcProfile {
  float soilLow;      // EC ดินต่ำกว่านี้ = ดินขาดปุ๋ย (mS/cm)
  float soilHigh;     // EC ดินสูงกว่านี้ = ดินเกินปุ๋ย
  float mixTarget;    // เป้า EC ถังผสมปกติ
  float mixTargetHi;  // เป้า EC ถังผสมเมื่อดินขาดมาก
};

// ===== ตารางรายสัปดาห์ (แถวแรก = สัปดาห์ที่ 1) =====
// ค่าด้านล่างเป็น "ตัวอย่าง" ต้องแก้ตามพืชของคุณ
// สัปดาห์ที่เกินจำนวนแถว จะใช้แถวสุดท้าย
static const EcProfile EC_WEEKLY[] = {
  // soilLow soilHigh mixTarget mixTargetHi
  {  0.3f,   1.5f,    0.8f,     1.2f },   // สัปดาห์ 1
  {  0.4f,   1.8f,    1.0f,     1.5f },   // สัปดาห์ 2
  {  0.5f,   2.2f,    1.3f,     1.8f },   // สัปดาห์ 3
  {  0.6f,   2.5f,    1.5f,     2.0f },   // สัปดาห์ 4 เป็นต้นไป
};
static const uint8_t EC_WEEKLY_N = sizeof(EC_WEEKLY) / sizeof(EC_WEEKLY[0]);

// ===== ค่าที่ระบบอื่นอ่าน =====
EcProfile ecCfg = EC_WEEKLY[0];
static bool        ecManual     = false;
static uint32_t    ecStartEpoch = 0;
static uint8_t     ecWeek       = 1;
static Preferences ecPrefs;

// ให้ soil_logic.h และ t2_batch.h ใช้ชื่อเดิมได้
#define SOIL_EC_LOW       (ecCfg.soilLow)
#define SOIL_EC_HIGH      (ecCfg.soilHigh)
#define EC_MIX_TARGET     (ecCfg.mixTarget)
#define EC_MIX_TARGET_HI  (ecCfg.mixTargetHi)

// ตรวจความสมเหตุสมผล กันตั้งค่าผิดจนชนระบบ safety
static bool ecValid(const EcProfile& p) {
  return p.soilLow >= 0 && p.soilLow < p.soilHigh && p.soilHigh <= 5.0f &&
         p.mixTarget > EC_WATER_BASE && p.mixTarget <= p.mixTargetHi &&
         (p.mixTargetHi + EC_MIX_TOL) < SAFETY_EC_MAX;
}

void fillEcTelemetry(JsonDocument& d) {
  d["ec_mode"]         = ecManual ? "manual" : "auto";
  d["ec_week"]         = ecWeek;
  d["ec_soil_low"]     = ecCfg.soilLow;
  d["ec_soil_high"]    = ecCfg.soilHigh;
  d["ec_mix_target"]   = ecCfg.mixTarget;
  d["ec_mix_target_hi"] = ecCfg.mixTargetHi;
}

static void ecPublishStatus() {
  JsonDocument d;
  d["mode"]        = ecManual ? "manual" : "auto";
  d["week"]        = ecWeek;
  d["soilLow"]     = ecCfg.soilLow;
  d["soilHigh"]    = ecCfg.soilHigh;
  d["mixTarget"]   = ecCfg.mixTarget;
  d["mixTargetHi"] = ecCfg.mixTargetHi;
  String s; serializeJson(d, s);
  mqttPublish("smartfarm/ec_profile", s);
}

void ecProfileLoad() {
  for (uint8_t i = 0; i < EC_WEEKLY_N; i++)
    if (!ecValid(EC_WEEKLY[i])) Serial.printf("[EC] ตารางสัปดาห์ %u ไม่ผ่านการตรวจสอบ\n", i + 1);

  ecPrefs.begin("ecprof", false);
  ecStartEpoch = ecPrefs.getULong("start", 0);
  ecManual     = ecPrefs.getBool("man", false);
  if (ecManual) {
    EcProfile p = { ecPrefs.getFloat("lo", ecCfg.soilLow),  ecPrefs.getFloat("hi", ecCfg.soilHigh),
                    ecPrefs.getFloat("mx", ecCfg.mixTarget), ecPrefs.getFloat("mh", ecCfg.mixTargetHi) };
    if (ecValid(p)) ecCfg = p; else ecManual = false;
  }
}

// เรียกทุก ~60 วินาที
void ecProfileUpdate() {
  if (ecManual) return;
  time_t now = time(nullptr);
  if (now < 1700000000 || ecStartEpoch == 0) return;   // เวลายังไม่ซิงค์ หรือยังไม่ตั้งวันเริ่ม
  uint8_t w = ((uint32_t)now - ecStartEpoch) / 604800UL + 1;
  if (w > EC_WEEKLY_N) w = EC_WEEKLY_N;
  ecWeek = w;
  if (ecValid(EC_WEEKLY[w - 1])) ecCfg = EC_WEEKLY[w - 1];
}

// "ตอนนี้อยู่สัปดาห์ที่ w"
bool ecSetWeek(uint8_t w) {
  time_t now = time(nullptr);
  if (now < 1700000000 || w < 1) return false;
  ecStartEpoch = (uint32_t)now - (uint32_t)(w - 1) * 604800UL;
  ecPrefs.putULong("start", ecStartEpoch);
  return true;
}

bool ecSetManual(const EcProfile& p) {
  if (!ecValid(p)) return false;
  ecCfg = p; ecManual = true;
  ecPrefs.putBool("man", true);
  ecPrefs.putFloat("lo", p.soilLow);   ecPrefs.putFloat("hi", p.soilHigh);
  ecPrefs.putFloat("mx", p.mixTarget); ecPrefs.putFloat("mh", p.mixTargetHi);
  return true;
}

void ecSetAuto() {
  ecManual = false;
  ecPrefs.putBool("man", false);
  ecProfileUpdate();
}

// เรียกเมื่อได้ข้อความที่ smartfarm/cmd/ec_profile
void ecHandleCommand(const String& payload) {
  JsonDocument d;
  if (deserializeJson(d, payload)) { Serial.println("[EC] JSON ผิดรูปแบบ"); return; }
  const char* mode = d["mode"] | "auto";

  if (strcmp(mode, "manual") == 0) {
    EcProfile p = { d["soilLow"] | ecCfg.soilLow,       d["soilHigh"] | ecCfg.soilHigh,
                    d["mixTarget"] | ecCfg.mixTarget,   d["mixTargetHi"] | ecCfg.mixTargetHi };
    if (!ecSetManual(p)) Serial.println("[EC] ค่าไม่ผ่านการตรวจสอบ ไม่เปลี่ยน");
  } else {
    if (!d["week"].isNull()) {
      if (!ecSetWeek(d["week"].as<uint8_t>())) Serial.println("[EC] ตั้งสัปดาห์ไม่ได้: เวลายังไม่ซิงค์");
    }
    ecSetAuto();
  }
  ecPublishStatus();
}