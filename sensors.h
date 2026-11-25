#pragma once
#include <Arduino.h>

// --- สถานะสุขภาพของเซนเซอร์ (Sensor Health Status) ---
enum Health : uint8_t {
  H_OK = 0,          // ทำงานปกติ อ่านค่าได้สมบูรณ์
  H_TIMEOUT,         // หมดเวลาการเชื่อมต่อ (ไม่ตอบสนอง)
  H_OUT_OF_RANGE,    // ค่าที่อ่านได้อยู่นอกเหนือช่วงความจริง
  H_SPIKE,           // ค่ากระโดดผิดปกติ (Spike / แกว่งกะทันหัน)
  H_NOT_FOUND,       // ไม่พบอุปกรณ์บนบัส (I2C / Modbus)
  H_NO_SENSOR        // ไม่มีเซนเซอร์ติดตั้งอยู่
};

// --- ฟังก์ชันแปลง Health enum เป็นข้อความภาษาไทยสำหรับใช้งานทั่วไปหรือแจ้งเตือน ---
inline const char* healthStr(Health h) {
  switch (h) {
    case H_OK:           return "ปกติ";
    case H_TIMEOUT:      return "หมดเวลาเชื่อมต่อ (Timeout)";
    case H_OUT_OF_RANGE: return "ค่านอกช่วง (Out of Range)";
    case H_SPIKE:        return "ค่าแกว่งผิดปกติ (Spike)";
    case H_NOT_FOUND:    return "ไม่พบอุปกรณ์ (Not Found)";
    case H_NO_SENSOR:    return "ไม่มีเซนเซอร์";
    default:             return "ไม่ทราบสถานะ";
  }
}