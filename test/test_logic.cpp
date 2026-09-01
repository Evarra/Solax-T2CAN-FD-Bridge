#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/solax_bridge_logic.h"

static CanFrame soc1873(uint8_t soc) {
  CanFrame frame;
  frame.id = 0x1873;
  frame.extended = true;
  frame.length = 8;
  frame.data[4] = soc;
  return frame;
}

static CanFrame soc187e(uint8_t soc) {
  CanFrame frame;
  frame.id = 0x187E;
  frame.extended = true;
  frame.length = 8;
  frame.data[5] = soc;
  return frame;
}

static CanFrame limits1872(uint16_t chargeCurrentdA, uint16_t dischargeCurrentdA = 1234) {
  CanFrame frame;
  frame.id = 0x1872;
  frame.extended = true;
  frame.length = 8;
  frame.data[4] = chargeCurrentdA & 0xFF;
  frame.data[5] = chargeCurrentdA >> 8;
  frame.data[6] = dischargeCurrentdA & 0xFF;
  frame.data[7] = dischargeCurrentdA >> 8;
  return frame;
}

static uint16_t le16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

int main() {
  assert(SolaxBridgeLogic::mapSocForInverter(0) == 0);
  assert(SolaxBridgeLogic::mapSocForInverter(89) == 89);
  assert(SolaxBridgeLogic::mapSocForInverter(90) == 90);
  assert(SolaxBridgeLogic::mapSocForInverter(91) == 92);
  assert(SolaxBridgeLogic::mapSocForInverter(92) == 94);
  assert(SolaxBridgeLogic::mapSocForInverter(93) == 96);
  assert(SolaxBridgeLogic::mapSocForInverter(94) == 98);
  assert(SolaxBridgeLogic::mapSocForInverter(95) == 100);
  assert(SolaxBridgeLogic::mapSocForInverter(100) == 100);

  assert(SolaxBridgeLogic::taperChargeCurrent(1000, 90) == 1000);
  assert(SolaxBridgeLogic::taperChargeCurrent(1000, 91) == 800);
  assert(SolaxBridgeLogic::taperChargeCurrent(1000, 92) == 600);
  assert(SolaxBridgeLogic::taperChargeCurrent(1000, 93) == 400);
  assert(SolaxBridgeLogic::taperChargeCurrent(1000, 94) == 200);
  assert(SolaxBridgeLogic::taperChargeCurrent(1000, 95) == 0);

  SolaxBridgeLogic logic;

  // Startup fail-safe: without SOC, the charge limit is zero.
  CanFrame limits = limits1872(1000);
  logic.processBatteryToInverter(limits, 100);
  assert(le16(&limits.data[4]) == 0);
  assert(le16(&limits.data[6]) == 1234);  // discharge limit untouched

  // Real 92% becomes inverter 94%, and 100.0 A becomes 60.0 A.
  CanFrame soc = soc1873(92);
  logic.processBatteryToInverter(soc, 200);
  assert(soc.data[4] == 94);
  limits = limits1872(1000);
  logic.processBatteryToInverter(limits, 201);
  assert(le16(&limits.data[4]) == 600);
  assert(le16(&limits.data[6]) == 1234);

  // Ultra duplicate SOC field is modified consistently.
  CanFrame ultra = soc187e(94);
  logic.processBatteryToInverter(ultra, 300);
  assert(ultra.data[5] == 98);

  // At real 95%, inverter sees 100% and allowed charge current becomes zero.
  soc = soc1873(95);
  logic.processBatteryToInverter(soc, 400);
  assert(soc.data[4] == 100);
  limits = limits1872(1000);
  logic.processBatteryToInverter(limits, 401);
  assert(le16(&limits.data[4]) == 0);

  // Stale SOC also forces a safe zero charge limit.
  limits = limits1872(1000);
  logic.processBatteryToInverter(limits, 400 + bridge_config::SOC_FRESHNESS_TIMEOUT_MS + 1);
  assert(le16(&limits.data[4]) == 0);

  // Invalid SOC does not replace the last valid reading.
  soc = soc1873(255);
  logic.processBatteryToInverter(soc, 9999);
  assert(soc.data[4] == 100);  // still mapped from last valid 95%

  // Standard or unrelated frames remain byte-for-byte unchanged.
  CanFrame unrelated;
  unrelated.id = 0x123;
  unrelated.extended = false;
  unrelated.length = 8;
  memset(unrelated.data, 0xA5, sizeof(unrelated.data));
  CanFrame original = unrelated;
  logic.processBatteryToInverter(unrelated, 10000);
  assert(memcmp(&unrelated, &original, sizeof(CanFrame)) == 0);

  puts("All SolaX bridge logic tests passed.");
  return 0;
}

