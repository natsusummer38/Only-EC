#pragma once
#include <Arduino.h>
#include <ModbusMaster.h>
#include "config.h"
#include "sensors.h"     // enum Health

#define MB_RAW_MAX 8

inline const char* rsHealthStr(Health h) {
  switch (h) {
    case H_OK:           return "OK";
    case H_TIMEOUT:      return "TIMEOUT";
    case H_NOT_FOUND:    return "NOT_FOUND";
    case H_OUT_OF_RANGE: return "OUT_OF_RANGE";
    case H_SPIKE:        return "SPIKE";
    case H_NO_SENSOR:    return "NO_SENSOR";
    default:             return "UNKNOWN";
  }
}

struct MbDev {
  const char*  name;
  ModbusMaster node;
  uint8_t      id;
  uint8_t      fc;
  uint16_t     reg;
  uint16_t     cnt;
  uint32_t     pollMs;
  uint32_t     staleMs;
  uint32_t     lastPoll;
  uint32_t     lastOk;
  uint16_t     raw[MB_RAW_MAX];
  uint16_t     lastErr;
  uint16_t     failCnt;
  uint32_t     okTotal;
  uint32_t     errTotal;
  bool         enabled;
};

static_assert(AIR_REG_COUNT   <= MB_RAW_MAX, "AIR_REG_COUNT too large");
static_assert(SOIL7_REG_COUNT <= MB_RAW_MAX, "SOIL7_REG_COUNT too large");

MbDev devAir   = { "Air_Sensor", ModbusMaster(), AIR_SLAVE_ID,   AIR_FC,   AIR_REG_START,   AIR_REG_COUNT,
                   AIR_POLL_MS,   AIR_STALE_MS,   0, 0, {0}, 0, 0, 0, 0, (RS485_EN_AIR   != 0) };
MbDev devSoil7 = { "Soil_7in1",  ModbusMaster(), SOIL7_SLAVE_ID, SOIL7_FC, SOIL7_REG_START, SOIL7_REG_COUNT,
                   SOIL7_POLL_MS, SOIL7_STALE_MS, 0, 0, {0}, 0, 0, 0, 0, (RS485_EN_SOIL7 != 0) };

SemaphoreHandle_t rsDataMux = nullptr;
SemaphoreHandle_t rsBusMux  = nullptr;
static TaskHandle_t rsTaskH = nullptr;

// ---------------------------------------------------------------------------
// DE/RE direction control
// ---------------------------------------------------------------------------
static void rsPre() {
  while (Serial2.available()) Serial2.read();   // ล้างขยะใน RX ก่อนส่ง
  digitalWrite(RS485_DE_RE_PIN, HIGH);
  delayMicroseconds(RS485_DIR_DELAY_US);
}
static void rsPost() {
  Serial2.flush();                              // รอให้ส่งครบก่อนสลับเป็นรับ
  delayMicroseconds(RS485_DIR_DELAY_US);
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

static void attachDev(MbDev& d) {
  d.node.begin(d.id, Serial2);
  d.node.preTransmission(rsPre);
  d.node.postTransmission(rsPost);
}

// ---------------------------------------------------------------------------
// Poll 1 อุปกรณ์ (เลือก FC3/FC4 ตาม d.fc)
// ---------------------------------------------------------------------------
static void pollDevice(MbDev& d) {
  if (xSemaphoreTake(rsBusMux, pdMS_TO_TICKS(3000)) != pdTRUE) return;

  uint8_t result = (d.fc == 4)
      ? d.node.readInputRegisters(d.reg, d.cnt)
      : d.node.readHoldingRegisters(d.reg, d.cnt);

  uint16_t buf[MB_RAW_MAX] = {0};
  uint8_t  n = (d.cnt < MB_RAW_MAX) ? d.cnt : MB_RAW_MAX;

  if (result == d.node.ku8MBSuccess) {
    for (uint8_t i = 0; i < n; i++) buf[i] = d.node.getResponseBuffer(i);
  }
  xSemaphoreGive(rsBusMux);

  if (xSemaphoreTake(rsDataMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (result == d.node.ku8MBSuccess) {
      for (uint8_t i = 0; i < n; i++) d.raw[i] = buf[i];
      d.lastOk  = millis();
      d.failCnt = 0;
      d.okTotal++;
      Serial.printf("[RS485] %s (ID%u FC%u) OK | raw0=%u raw1=%u\n",
                    d.name, d.id, d.fc, buf[0], (n > 1) ? buf[1] : 0);
    } else {
      if (d.failCnt < 0xFFFF) d.failCnt++;
      d.errTotal++;
      d.lastErr = result;
      Serial.printf("[RS485] %s (ID%u FC%u) FAILED err=0x%02X fail=%u\n",
                    d.name, d.id, d.fc, result, d.failCnt);
    }
    xSemaphoreGive(rsDataMux);
  }
}

static void pollIfDue(MbDev& d) {
  if (!d.enabled) return;
  uint32_t now = millis();
  if (d.lastPoll != 0 && (now - d.lastPoll) < d.pollMs) return;
  d.lastPoll = (now == 0) ? 1 : now;
  pollDevice(d);
  vTaskDelay(pdMS_TO_TICKS(RS485_GAP_MS));
}

static Health rsSnapshot(MbDev& d, uint16_t* out) {
  if (rsDataMux == nullptr || !d.enabled) return H_NOT_FOUND;
  if (xSemaphoreTake(rsDataMux, pdMS_TO_TICKS(50)) != pdTRUE) return H_TIMEOUT;

  for (uint8_t i = 0; i < MB_RAW_MAX; i++) out[i] = d.raw[i];
  uint32_t lastOk  = d.lastOk;
  uint16_t failCnt = d.failCnt;
  xSemaphoreGive(rsDataMux);

  if (lastOk == 0) return H_NOT_FOUND;
  if (failCnt >= RS485_FAULT_CNT) return H_TIMEOUT;
  if ((millis() - lastOk) > d.staleMs) return H_TIMEOUT;
  return H_OK;
}

#if RS485_SCAN_ON_BOOT
static void rs485Scan() {
  const uint32_t bauds[] = {4800, 9600, 19200};
  Serial.println("[RS485] SCAN start...");
  for (uint32_t b : bauds) {
    Serial2.end();
    Serial2.begin(b, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    for (uint8_t id = 1; id <= 10; id++) {
      ModbusMaster m;
      m.begin(id, Serial2);
      m.preTransmission(rsPre);
      m.postTransmission(rsPost);
      uint8_t r4 = m.readInputRegisters(0, 2);
      delay(50);
      uint8_t r3 = m.readHoldingRegisters(0, 2);
      if (r4 == m.ku8MBSuccess || r3 == m.ku8MBSuccess) {
        Serial.printf("[SCAN] FOUND baud=%lu id=%u FC4=%s FC3=%s\n",
                      (unsigned long)b, id,
                      r4 == m.ku8MBSuccess ? "OK" : "no",
                      r3 == m.ku8MBSuccess ? "OK" : "no");
      }
      delay(100);
    }
  }
  Serial2.end();
  Serial2.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial.println("[RS485] SCAN done");
}
#endif

static void rs485Task(void* pvParameters) {
  (void)pvParameters;
  vTaskDelay(pdMS_TO_TICKS(1000));
  while (true) {
    pollIfDue(devAir);
    pollIfDue(devSoil7);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setupRS485() {
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);

  Serial2.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);

  rsDataMux = xSemaphoreCreateMutex();
  rsBusMux  = xSemaphoreCreateMutex();

  attachDev(devAir);
  attachDev(devSoil7);

#if RS485_SCAN_ON_BOOT
  rs485Scan();
#endif

  xTaskCreatePinnedToCore(rs485Task, "rs485Task", 4096, nullptr, 1, &rsTaskH, 0);

  Serial.printf("[RS485] Bus started @%d baud | Air:%s(ID%d) SOIL7:%s(ID%d)\n",
                RS485_BAUD,
                devAir.enabled   ? "ON" : "off", AIR_SLAVE_ID,
                devSoil7.enabled ? "ON" : "off", SOIL7_SLAVE_ID);
}

Health rs485GetAir(float& tC, float& rh) {
  uint16_t r[MB_RAW_MAX];
  Health h = rsSnapshot(devAir, r);
  if (h != H_OK) return h;

  tC = (float)((int16_t)r[AIR_IDX_TEMP]) / AIR_DIV;
  rh = (float)r[AIR_IDX_RH] / AIR_DIV;
  if (tC < -40.0f || tC > 85.0f || rh < 0.0f || rh > 100.0f) return H_OUT_OF_RANGE;
  return H_OK;
}

// เซนเซอร์ดิน: ได้ อุณหภูมิ, ความชื้น, EC (µS/cm)
Health rs485GetSoil7in1(float& tC, float& rh, float& ec) {
  uint16_t r[MB_RAW_MAX];
  Health h = rsSnapshot(devSoil7, r);
  if (h != H_OK) return h;

  rh = (float)r[SOIL7_IDX_RH] / 10.0f;
  tC = (float)((int16_t)r[SOIL7_IDX_TEMP]) / 10.0f;
  ec = (float)r[SOIL7_IDX_EC];                 // µS/cm
  if (rh > 100.0f || tC < -40.0f || tC > 85.0f) return H_OUT_OF_RANGE;
  return H_OK;
}

// EC ดินหน่วย mS/cm (ใช้เทียบกับค่าใน ec_profile.h)
Health rs485GetSoilEC(float& ec) {
  float tC, rh;
  Health h = rs485GetSoil7in1(tC, rh, ec);
  if (h == H_OK) ec *= SOIL7_EC_TO_MS;
  return h;
}

bool rs485ChangeId(uint8_t oldId, uint8_t newId, uint16_t idReg = 0x0000) {
  ModbusMaster tempNode;
  tempNode.begin(oldId, Serial2);
  tempNode.preTransmission(rsPre);
  tempNode.postTransmission(rsPost);

  if (xSemaphoreTake(rsBusMux, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
  uint8_t result = tempNode.writeSingleRegister(idReg, newId);
  xSemaphoreGive(rsBusMux);

  return (result == tempNode.ku8MBSuccess);
}