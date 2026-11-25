#pragma once
#include <Preferences.h>
#include <algorithm>
#include "config.h"
#include "sensors.h"
#include "alerts.h"
#include "rs485_bus.h"
#include "soil_ads.h"
#include "ec_profile.h"

enum Plan : uint8_t { PL_NONE, PL_WATER, PL_FERT, PL_LEACH };
static const char* PLAN_NAMES[] = {"none", "water", "fert", "leach"};

struct T2Request {
  Plan plan = PL_NONE; uint8_t zoneMask = 0;
  float volPerZoneL = ZONE_VOL_L; float ecTarget = EC_MIX_TARGET; String reason;
};

struct SoilChem { float tC = NAN, ec = NAN; uint32_t takenMs = 0; } soilChem;
float  soilTLive = NAN, soilEcLive = NAN;
Health soilSensorHealth = H_NOT_FOUND;     // สถานะเซนเซอร์ดิน 7-in-1 (เดิมชื่อ soilPhtHealth)
uint32_t lastIrrigEndMs = 0;
time_t   lastFertEpoch  = 0;
static Preferences prefs;

static float rbT[SOIL_MEDIAN_N], rbEc[SOIL_MEDIAN_N];
static uint8_t  rbN = 0;
static uint32_t lastSoilSample = 0;

static float medianOf(const float* a, uint8_t n) {
  float t[SOIL_MEDIAN_N]; memcpy(t, a, n * sizeof(float)); std::sort(t, t + n);
  return (n & 1) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

bool soilChemValid() { return soilChem.takenMs && millis() - soilChem.takenMs < SOIL_CHEM_MAX_AGE_MS; }

void loadFertHistory() { prefs.begin("t2", false); lastFertEpoch = (time_t)prefs.getULong64("lastFert", 0); }
void saveFertHistory() { lastFertEpoch = time(nullptr); prefs.putULong64("lastFert", (uint64_t)lastFertEpoch); }

void soilLogicTick() {
  float t = NAN, rh = NAN, ecRaw = NAN;
  soilSensorHealth = rs485GetSoil7in1(t, rh, ecRaw);
  if (soilSensorHealth == H_OK) soilTLive = t;
#if SOIL_EC_ENABLED
  float ec;
  soilEcLive = (rs485GetSoilEC(ec) == H_OK) ? ec : NAN;
#endif

  if (millis() - lastSoilSample < SOIL_SAMPLE_MS) return;
  lastSoilSample = millis();

  bool cond = soilSensorHealth == H_OK
           && soilHealth[SOIL_Z2] == H_OK && soilPct[SOIL_Z2] >= MOIST_CHEM_MIN_PCT
           && millis() - lastIrrigEndMs >= SOIL_SETTLE_MS;
#if SOIL_EC_ENABLED
  cond = cond && !isnan(soilEcLive);
#endif
  if (!cond) { rbN = 0; return; }          // ต้องเป็นช่วงต่อเนื่องเท่านั้น

  rbT[rbN] = soilTLive; rbEc[rbN] = soilEcLive; rbN++;
  if (rbN < SOIL_MEDIAN_N) return;

  soilChem.tC = medianOf(rbT, rbN);
  soilChem.ec = SOIL_EC_ENABLED ? medianOf(rbEc, rbN) : NAN;
  soilChem.takenMs = millis();
  rbN = 0;
  Serial.printf("[SOIL Z2] snapshot %.1f C | EC %.2f\n", soilChem.tC, soilChem.ec);
}

T2Request decideT2() {
  T2Request r;
  uint8_t dry = 0; bool allOk = true, roomForFert = true;
  for (uint8_t z = 0; z < 3; z++) {
    uint8_t i = z + 1;                                   // soil index 1..3 = Z1..Z3
    if (soilHealth[i] != H_OK) { allOk = false; continue; }
    if (soilPct[i] < MOIST_START_PCT)    dry |= (1 << z);
    if (soilPct[i] > MOIST_FERT_MAX_PCT) roomForFert = false;
  }
  bool  chem  = soilChemValid();
  float hFert = lastFertEpoch ? (time(nullptr) - lastFertEpoch) / 3600.0f : 1e6f;

  bool ecKnown = chem && SOIL_EC_ENABLED;
  bool ecHigh  = ecKnown && soilChem.ec > SOIL_EC_HIGH;
  bool ecLow   = ecKnown && soilChem.ec < SOIL_EC_LOW;
  bool calDue  = FERT_CALENDAR_FALLBACK && !ecKnown && hFert >= FERT_MAX_INTERVAL_H;
  bool fertDue = hFert >= FERT_MIN_INTERVAL_H && !ecHigh && (ecLow || calDue);

  if (fertDue && allOk && roomForFert) {
    r.plan = PL_FERT; r.zoneMask = 0b111;              // ปุ๋ยให้ทั้งบล็อกเท่ากัน
    r.ecTarget = (ecKnown && soilChem.ec < SOIL_EC_LOW * 0.5f) ? EC_MIX_TARGET_HI : EC_MIX_TARGET;
    r.reason = ecLow ? String("EC ดินต่ำ ") + String(soilChem.ec, 2)
                     : String("ครบรอบปุ๋ย ") + String(hFert / 24.0f, 1) + " วัน";
    return r;
  }
  if (dry) {
    r.plan = ecHigh ? PL_LEACH : PL_WATER;
    r.zoneMask = dry;
    if (ecHigh) r.volPerZoneL *= LEACH_FACTOR;
    r.reason = ecHigh ? "ดินแห้ง + EC ดินสูง → ชะล้าง" : "ดินแห้ง";
  }
  return r;
}