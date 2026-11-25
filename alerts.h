#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"

// ฟังก์ชันส่ง MQTT (เขียนจริงไว้ในไฟล์ .ino)
void mqttPublish(const char* topic, const String& payload);

enum AlertLevel : uint8_t {
  L_INFO = 0,
  L_WARN,
  L_ERROR,
  L_CRITICAL
};

inline const char* levelStr(AlertLevel lvl) {
  switch (lvl) {
    case L_INFO:     return "INFO";
    case L_WARN:     return "WARNING";
    case L_ERROR:    return "ERROR";
    case L_CRITICAL: return "CRITICAL";
    default:         return "UNKNOWN";
  }
}

struct AlertThrottle {
  const char* key;
  uint32_t    lastSentMs;
  uint32_t    cooldownMs;   // 0 = ส่งทุกครั้ง
};

static AlertThrottle throttles[] = {
  {"ec_sensor",     0, 300000UL},
  {"temp_sensor",   0, 300000UL},
  {"mix_fail",      0, 60000UL},
  {"mix_locked",    0, 0UL},
  {"no_flow_water", 0, 30000UL},
  {"no_flow_fert",  0, 30000UL},
  {"flow_leak",     0, 600000UL},
  {"drain_timeout", 0, 60000UL},
  {"estop",         0, 0UL},
  {"interlock",     0, 0UL},
  {"no_valve",      0, 0UL},
  {"max_runtime",   0, 0UL},
  {"ec_high",       0, 60000UL}
};
static const uint8_t THROTTLE_COUNT = sizeof(throttles) / sizeof(throttles[0]);

// ส่งแจ้งเตือนพร้อมระบุระดับ
void alertSend(const char* key, const String& msg, AlertLevel lvl) {
  uint32_t now = millis();

  for (uint8_t i = 0; i < THROTTLE_COUNT; i++) {
    if (strcmp(throttles[i].key, key) == 0) {
      if (throttles[i].cooldownMs > 0 && throttles[i].lastSentMs != 0 &&
          (now - throttles[i].lastSentMs) < throttles[i].cooldownMs) {
        return;   // ยังอยู่ในช่วง cooldown
      }
      throttles[i].lastSentMs = now ? now : 1;
      break;
    }
  }

  Serial.printf("[ALERT][%s] (%s): %s\n", levelStr(lvl), key, msg.c_str());

  JsonDocument doc;
  doc["key"]       = key;
  doc["level"]     = levelStr(lvl);
  doc["message"]   = msg;
  doc["timestamp"] = millis();
  time_t t = time(nullptr);
  if (t > 1700000000) doc["epoch"] = (uint32_t)t;

  String payload;
  serializeJson(doc, payload);
  mqttPublish("smartfarm/alert", payload);
}

// แบบสั้น: ระดับ WARNING (ใช้กับโค้ดเดิมที่เรียก notifyAlert(key, msg))
void notifyAlert(const char* key, const String& msg) {
  alertSend(key, msg, L_WARN);
}