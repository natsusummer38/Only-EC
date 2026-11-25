#pragma once
#include <Arduino.h>
#include "config.h"

struct FlowMeter {
  const char* name;
  float pulsesPerL;
  volatile uint32_t pulses;
  volatile uint32_t lastUs;
  uint32_t prevPulses, prevMs, markPulses;
  float lpm;
};

FlowMeter fmWater = {"water", FLOW_WATER_PPL, 0, 0, 0, 0, 0, 0};
FlowMeter fmFert  = {"fert",  FLOW_FERT_PPL,  0, 0, 0, 0, 0, 0};

static void IRAM_ATTR isrFlowW() {
  uint32_t t = micros();
  if (t - fmWater.lastUs >= FLOW_MIN_PULSE_US) { fmWater.pulses++; fmWater.lastUs = t; }
}
static void IRAM_ATTR isrFlowF() {
  uint32_t t = micros();
  if (t - fmFert.lastUs >= FLOW_MIN_PULSE_US) { fmFert.pulses++; fmFert.lastUs = t; }
}

void setupFlow() {
  pinMode(FLOW_WATER_PIN, INPUT);           // 34/35 ไม่มี pull-up ภายใน
  pinMode(FLOW_FERT_PIN,  INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_WATER_PIN), isrFlowW, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_FERT_PIN),  isrFlowF, FALLING);
}

void flowTick() {                            // เรียกทุก loop
  FlowMeter* fms[] = {&fmWater, &fmFert};
  uint32_t now = millis();
  for (FlowMeter* f : fms) {
    if (now - f->prevMs < 1000) continue;
    uint32_t c = f->pulses;
    float minutes = (now - f->prevMs) / 60000.0f;
    f->lpm = ((c - f->prevPulses) / f->pulsesPerL) / minutes;
    f->prevPulses = c; f->prevMs = now;
  }
}

void  flowMark(FlowMeter& f)            { f.markPulses = f.pulses; }
float flowLitersSinceMark(FlowMeter& f) { return (f.pulses - f.markPulses) / f.pulsesPerL; }
float flowTotalL(FlowMeter& f)          { return f.pulses / f.pulsesPerL; }