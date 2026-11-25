#pragma once
#include <ArduinoJson.h>
#include "config.h"
#include "alerts.h"
#include "flow.h"
#include "soil_logic.h"
#include "water_quality.h"

enum T2State : uint8_t { T2_IDLE, T2_DISCARD, T2_FILL, T2_DOSE, T2_MIX, T2_SETTLE,
                         T2_EVAL, T2_PRIME, T2_RUN, T2_VCLOSE, T2_FAULT };
static const char* T2_NAMES[] = {"IDLE","DISCARD","FILL","DOSE","MIX","SETTLE",
                                 "EVAL","PRIME","RUN","VCLOSE","FAULT"};
static const uint8_t SV_PIN[3] = {O_T2_SV1, O_T2_SV2, O_T2_SV3};

struct Batch {
  Plan plan = PL_NONE; uint8_t zoneMask = 0; String reason;
  float ecTarget = 0, waterL = 0, fertMl = 0, fillStepL = 0, doseStepMl = 0;
  bool doseAfterFill = false, fillAfterDrain = false;
  uint8_t trims = 0;
  int8_t zone = -1, pendingClose = -1;
  uint32_t zoneMs = 0, zoneStart = 0, closeAt = 0, emptySince = 0;
  float ecFinal = NAN;
} B;

T2State t2 = T2_IDLE;
static uint32_t t2Since = 0, lastBatchEndMs = 0;
uint8_t mixFails = 0; bool mixLocked = false; String t2FaultMsg;

static inline void rly(uint8_t p, bool on) { digitalWrite(p, on ? RELAY_ON : RELAY_OFF); }
void t2AllOff() {
  rly(O_T2_WATER, false); rly(O_T2_DOSE, false); rly(O_T2_MIXER, false);
  rly(O_T2_IRRIG, false); rly(O_T2_DRAIN, false);
  for (uint8_t p : SV_PIN) rly(p, false);
}
static void go(T2State s) { t2 = s; t2Since = millis(); Serial.printf("[T2] -> %s\n", T2_NAMES[s]); }
static uint32_t el()      { return millis() - t2Since; }
static bool tankEmpty()   { return digitalRead(MIX_LOW_PIN) == MIX_LOW_ACTIVE; }
static uint8_t nZones(uint8_t m) { return __builtin_popcount(m); }
static int8_t nextZone(int8_t from) {
  for (int8_t z = from + 1; z < 3; z++) if (B.zoneMask & (1 << z)) return z;
  return -1;
}

// เซนเซอร์ถังผสมที่ต้องพร้อม: EC และอุณหภูมิ
static bool mixSensorsOk() { return ecHealth == H_OK && tempHealth == H_OK; }

void t2Fault(const char* key, const String& msg) {
  t2AllOff(); t2FaultMsg = msg; go(T2_FAULT);
  alertSend(key, "[T2 FAULT] " + msg, L_ERROR);
}

// ปลดล็อก T2 (เรียกจากคำสั่ง MQTT)
void t2Reset() {
  t2AllOff();
  mixFails = 0; mixLocked = false; t2FaultMsg = "";
  go(T2_IDLE);
  Serial.println("[T2] reset");
}

static void startFill(float liters, bool thenDose) {
  B.fillStepL = liters; B.doseAfterFill = thenDose;
  flowMark(fmWater); rly(O_T2_WATER, true); go(T2_FILL);
}
static void startDose(float ml) {
  B.doseStepMl = ml; flowMark(fmFert);
  rly(O_T2_MIXER, true); rly(O_T2_DOSE, true); go(T2_DOSE);
}
static void startDrain(bool thenFill) {
  t2AllOff(); B.fillAfterDrain = thenFill; B.emptySince = 0;
  rly(O_T2_DRAIN, true); go(T2_DISCARD);
}
static void batchFail(const String& why) {
  mixFails++;
  notifyAlert("mix_fail", "สารละลายไม่ผ่าน: " + why + " (" + String(mixFails) + "/" + String(MIX_MAX_RETRY) + ")");
  if (mixFails >= MIX_MAX_RETRY) {
    mixLocked = true;
    alertSend("mix_locked", "T2 ถูกล็อก: ตรวจถังปุ๋ยและโพรบ แล้วสั่ง t2_reset", L_ERROR);
  }
  startDrain(false);
}

static void t2Deliver() {
  mixFails = 0;
  float perZoneL = B.waterL / nZones(B.zoneMask);
  B.zoneMs = (uint32_t)(perZoneL / IRRIG_LPM * 60000.0f);
  B.zone = nextZone(-1);
  rly(SV_PIN[B.zone], true);                 // เปิดวาล์วก่อนปั๊มเสมอ
  go(T2_PRIME);
}

static void t2Finish() {
  lastIrrigEndMs = lastBatchEndMs = millis();
  if (B.plan == PL_FERT) saveFertHistory();
  JsonDocument d;
  d["system"] = "T2"; d["plan"] = PLAN_NAMES[B.plan]; d["zones"] = B.zoneMask;
  d["water_L"] = roundf(B.waterL * 10) / 10; d["fert_mL"] = roundf(B.fertMl);
  d["ec"] = B.ecFinal; d["trims"] = B.trims; d["reason"] = B.reason;
  String s; serializeJson(d, s);
  mqttPublish("smartfarm/water_event", s);
  go(T2_IDLE);
}

static void t2Evaluate() {
  if (!mixSensorsOk()) { batchFail("เซนเซอร์ในถังผสมอ่านค่าไม่ได้"); return; }
  float ec = ecVal, tc = waterTempC;
  B.ecFinal = ec;
  Serial.printf("[T2 EVAL] %s EC %.2f (เป้า %.2f) T %.1f\n",
                PLAN_NAMES[B.plan], ec, B.ecTarget, tc);

  if (tc < MIX_T_MIN || tc > MIX_T_MAX) { batchFail("อุณหภูมิ " + String(tc, 1) + " C"); return; }

  if (B.plan != PL_FERT) {
    if (ec <= EC_WATER_MAX) t2Deliver();
    else batchFail("น้ำมี EC " + String(ec, 2) + " (มีปุ๋ยค้างถัง?)");
    return;
  }

  float lo = B.ecTarget - EC_MIX_TOL, hi = B.ecTarget + EC_MIX_TOL;
  if (ec >= lo && ec <= hi) { t2Deliver(); return; }
  if (B.trims >= TRIM_MAX_STEPS) { batchFail("ปรับ EC ครบแล้วยังไม่เข้าเป้า"); return; }
  B.trims++;

  if (ec < lo) {                                       // ปุ๋ยน้อย: เติมตามสัดส่วน
    float gain  = (ec - EC_WATER_BASE) / max(B.fertMl, 1.0f);   // mS/cm ต่อ mL
    float addMl = (gain > 1e-5f) ? (B.ecTarget - ec) / gain : B.fertMl * 0.25f;
    addMl = min(addMl, B.waterL * DOSE_MAX_ML_PER_L - B.fertMl);
    if (addMl < 5.0f) { batchFail("ถึงเพดานปุ๋ยแล้ว EC ยังต่ำ (ถังปุ๋ยหมด?)"); return; }
    Serial.printf("[T2 TRIM] เติมปุ๋ย +%.0f mL\n", addMl);
    startDose(addMl);
  } else {                                             // ปุ๋ยเข้ม: เติมน้ำเจือจาง
    float addL = B.waterL * (ec - B.ecTarget) / (B.ecTarget - EC_WATER_BASE);
    if (B.waterL + addL > MIX_TANK_MAX_L) { batchFail("EC สูงและถังไม่พอเจือจาง"); return; }
    Serial.printf("[T2 TRIM] เติมน้ำ +%.1f L\n", addL);
    startFill(addL, false);
  }
}

bool t2Start(const T2Request& r) {
  if (estopActive) return false;
  if (t2 != T2_IDLE || mixLocked || r.plan == PL_NONE || !r.zoneMask) return false;
  B = Batch();
  B.plan = r.plan; B.zoneMask = r.zoneMask; B.reason = r.reason; B.ecTarget = r.ecTarget;
  float total = min(r.volPerZoneL * nZones(r.zoneMask), MIX_TANK_MAX_L * 0.85f); // เผื่อพื้นที่เจือจาง
  B.fillStepL = total;
  Serial.printf("[T2] START %s zones=0x%X %.0f L | %s\n",
                PLAN_NAMES[B.plan], B.zoneMask, total, B.reason.c_str());
  if (!tankEmpty()) startDrain(true);          // ถังมีของเก่าค้างอยู่ ต้องทิ้งก่อน
  else startFill(total, B.plan == PL_FERT);
  return true;
}

void t2Tick() {
  switch (t2) {
  case T2_IDLE: {                              // ตรวจรั่ว: ปั๊มปิดแต่ยังมีน้ำไหล
    static uint32_t leakSince = 0, lastLeakAlert = 0;
    bool leak = fmWater.lpm > FLOW_LEAK_LPM || fmFert.lpm > FLOW_LEAK_LPM;
    if (!leak) leakSince = 0;
    else if (!leakSince) leakSince = millis();
    else if (millis() - leakSince > 30000 && millis() - lastLeakAlert > 1800000UL) {
      notifyAlert("flow_leak", "มีการไหลขณะปั๊มปิด: ตรวจวาล์วกันกลับและรอยรั่ว");
      lastLeakAlert = millis();
    }
    break; }

  case T2_FILL: {
    float l = flowLitersSinceMark(fmWater);
    if (l >= B.fillStepL) {
      rly(O_T2_WATER, false); B.waterL += l;
      if (B.doseAfterFill) startDose(B.waterL * FERT_ML_PER_L);
      else { rly(O_T2_MIXER, true); go(T2_MIX); }
    } else if (el() > FLOW_GRACE_MS && fmWater.lpm < FLOW_MIN_LPM_W) {
      t2Fault("no_flow_water", "ปั๊มน้ำเปิดแต่ไม่มีน้ำไหล: ถังน้ำหมด / ปั๊มเสีย / ท่ออุดตัน");
    } else if (el() > FILL_TIMEOUT_MS) t2Fault("fill_timeout", "เติมน้ำนานเกินกำหนด");
    break; }

  case T2_DOSE: {
    float ml = flowLitersSinceMark(fmFert) * 1000.0f;
    if (ml >= B.doseStepMl) { rly(O_T2_DOSE, false); B.fertMl += ml; go(T2_MIX); }
    else if (el() > FLOW_GRACE_MS && fmFert.lpm < FLOW_MIN_LPM_F)
      t2Fault("no_flow_fert", "ปั๊มปุ๋ยเปิดแต่ไม่มีการไหล: ถังปุ๋ยหมด / ปั๊มเสีย");
    else if (el() > DOSE_TIMEOUT_MS) t2Fault("dose_timeout", "จ่ายปุ๋ยนานเกินกำหนด");
    break; }

  case T2_MIX:    if (el() >= MIX_RUN_MS) { rly(O_T2_MIXER, false); go(T2_SETTLE); } break;
  case T2_SETTLE: if (el() >= MIX_SETTLE_S * 1000UL) go(T2_EVAL); break;
  case T2_EVAL:   if (el() >= EVAL_WAIT_MS) t2Evaluate(); break;   // รอให้ค่า WQ อัปเดต

  case T2_PRIME:
    if (el() >= VALVE_LEAD_MS) { rly(O_T2_IRRIG, true); B.zoneStart = millis(); go(T2_RUN); }
    break;

  case T2_RUN:
    if (tankEmpty()) {                         // กันปั๊มทำงานตอนถังแห้ง
      rly(O_T2_IRRIG, false); Serial.println("[T2] ถังผสมหมดก่อนกำหนด"); go(T2_VCLOSE); break;
    }
    if (millis() - B.zoneStart >= B.zoneMs) {
      int8_t nz = nextZone(B.zone);
      if (nz >= 0) {                           // เปิดตัวใหม่ก่อนปิดตัวเก่า
        rly(SV_PIN[nz], true);
        B.pendingClose = B.zone; B.closeAt = millis() + 1000;
        B.zone = nz; B.zoneStart = millis();
      } else { rly(O_T2_IRRIG, false); go(T2_VCLOSE); }
    }
    if (B.pendingClose >= 0 && (int32_t)(millis() - B.closeAt) >= 0) {
      rly(SV_PIN[B.pendingClose], false); B.pendingClose = -1;
    }
    break;

  case T2_VCLOSE:
    if (el() >= 2000) { for (uint8_t p : SV_PIN) rly(p, false); t2Finish(); }
    break;

  case T2_DISCARD: {
    if (tankEmpty()) { if (!B.emptySince) B.emptySince = millis(); } else B.emptySince = 0;
    bool done = B.emptySince && millis() - B.emptySince >= DRAIN_EXTRA_MS;
    if (done || el() >= DRAIN_MAX_MS) {
      rly(O_T2_DRAIN, false);
      if (!done) notifyAlert("drain_timeout", "ระบายถังไม่หมด: ตรวจ Drain valve / ลูกลอย");
      if (B.fillAfterDrain && done) startFill(B.fillStepL, B.plan == PL_FERT);
      else { lastBatchEndMs = millis(); go(T2_IDLE); }
    }
    break; }

  case T2_FAULT: break;                        // ค้างไว้จนกว่าจะสั่ง t2Reset()
  }
}

void t2AutoTick() {
  static uint32_t last = 0;
  if (millis() - last < 60000) return;
  last = millis();
  if (t2 != T2_IDLE || mixLocked || estopActive) return;
  if (lastBatchEndMs && millis() - lastBatchEndMs < T2_MIN_GAP_MS) return;
  struct tm tm;
  if (!getLocalTime(&tm, 10)) return;          // ยังไม่ได้เวลาจาก NTP: ไม่ทำงานอัตโนมัติ
  if (tm.tm_hour < T2_HOUR_START || tm.tm_hour >= T2_HOUR_END) return;
  T2Request r = decideT2();
  if (r.plan != PL_NONE) t2Start(r);
}

void fillT2Telemetry(JsonDocument& d) {
  const char* k[SOIL_N] = {"soil_t1", "soil_z1", "soil_z2", "soil_z3"};
  for (uint8_t i = 0; i < SOIL_N; i++)
    if (soilHealth[i] == H_OK) d[k[i]] = roundf(soilPct[i] * 10) / 10; else d[k[i]] = nullptr;
  if (soilSensorHealth == H_OK) d["soil_temp_z2"] = soilTLive; else d["soil_temp_z2"] = nullptr;
  if (!isnan(soilEcLive)) d["soil_ec_z2"] = soilEcLive;
  if (soilChemValid()) {
    d["chem_ec"] = soilChem.ec;
    d["chem_age_min"] = (millis() - soilChem.takenMs) / 60000;
  }
  d["t2_state"] = T2_NAMES[t2];   d["plan"] = PLAN_NAMES[B.plan];
  d["water_L"] = B.waterL;        d["fert_mL"] = B.fertMl;
  d["ec_target"] = B.ecTarget;    d["trims"] = B.trims;
  d["flow_w_lpm"] = fmWater.lpm;  d["flow_f_lpm"] = fmFert.lpm;
  d["tank_empty"] = tankEmpty();  d["mix_locked"] = mixLocked;
  d["fert_age_h"] = lastFertEpoch ? (time(nullptr) - lastFertEpoch) / 3600 : -1;
}