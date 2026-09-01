#pragma once

#include <stdint.h>
#include <string.h>

#include "config.h"

struct CanFrame {
  uint32_t id = 0;
  bool extended = false;
  uint8_t length = 0;
  uint8_t data[8] = {0};
};

class SolaxBridgeLogic {
 public:
  // Call only for frames travelling from the battery toward the inverter.
  // The frame is modified in place. All unrelated frames remain unchanged.
  void processBatteryToInverter(CanFrame& frame, uint32_t nowMs) {
    if (!frame.extended) {
      return;
    }

    // BMS_PackData: SOC is byte 4 in whole percent.
    if (frame.id == 0x1873 && frame.length >= 5) {
      updateActualSoc(frame.data[4], nowMs);
      if (haveSoc_) {
        frame.data[4] = mapSocForInverter(actualSocPct_);
      }
      return;
    }

    // SolaX Ultra repeats SOC in byte 5 of BMS capacity/SOH message 0x187E.
    if (frame.id == 0x187E && frame.length >= 6) {
      updateActualSoc(frame.data[5], nowMs);
      if (haveSoc_) {
        frame.data[5] = mapSocForInverter(actualSocPct_);
      }
      return;
    }

    // BMS_Limits: max charge current is little-endian bytes 4..5, unit 0.1 A.
    // Bytes 6..7 (max discharge current) are intentionally left untouched.
    if (frame.id == 0x1872 && frame.length >= 6) {
      lastOriginalChargeCurrentdA_ = readLe16(&frame.data[4]);
      if (socIsFresh(nowMs)) {
        lastLimitedChargeCurrentdA_ = taperChargeCurrent(lastOriginalChargeCurrentdA_, actualSocPct_);
      } else {
        // On startup or after stale/invalid SOC, never invent a permissive limit.
        lastLimitedChargeCurrentdA_ = 0;
      }
      writeLe16(&frame.data[4], lastLimitedChargeCurrentdA_);
    }
  }

  bool haveSoc() const { return haveSoc_; }
  uint8_t actualSocPct() const { return actualSocPct_; }
  uint8_t inverterSocPct() const { return haveSoc_ ? mapSocForInverter(actualSocPct_) : 0; }
  uint16_t lastOriginalChargeCurrentdA() const { return lastOriginalChargeCurrentdA_; }
  uint16_t lastLimitedChargeCurrentdA() const { return lastLimitedChargeCurrentdA_; }

  bool socIsFresh(uint32_t nowMs) const {
    return haveSoc_ && static_cast<uint32_t>(nowMs - lastSocTimestampMs_) <=
                           bridge_config::SOC_FRESHNESS_TIMEOUT_MS;
  }

  static uint8_t mapSocForInverter(uint8_t actualSocPct) {
    using namespace bridge_config;
    if (actualSocPct <= TAPER_START_SOC_PCT) {
      return actualSocPct;
    }
    if (actualSocPct >= TAPER_STOP_SOC_PCT) {
      return 100;
    }

    const uint16_t inputRange = TAPER_STOP_SOC_PCT - TAPER_START_SOC_PCT;
    const uint16_t outputRange = 100 - TAPER_START_SOC_PCT;
    const uint16_t inputOffset = actualSocPct - TAPER_START_SOC_PCT;
    return static_cast<uint8_t>(TAPER_START_SOC_PCT +
                                ((inputOffset * outputRange) + (inputRange / 2)) / inputRange);
  }

  static uint16_t taperChargeCurrent(uint16_t originalCurrentdA, uint8_t actualSocPct) {
    using namespace bridge_config;
    if (actualSocPct <= TAPER_START_SOC_PCT) {
      return originalCurrentdA;
    }
    if (actualSocPct >= TAPER_STOP_SOC_PCT) {
      return 0;
    }

    const uint32_t range = TAPER_STOP_SOC_PCT - TAPER_START_SOC_PCT;
    const uint32_t remaining = TAPER_STOP_SOC_PCT - actualSocPct;
    return static_cast<uint16_t>((static_cast<uint32_t>(originalCurrentdA) * remaining + (range / 2)) /
                                 range);
  }

 private:
  bool haveSoc_ = false;
  uint8_t actualSocPct_ = 0;
  uint32_t lastSocTimestampMs_ = 0;
  uint16_t lastOriginalChargeCurrentdA_ = 0;
  uint16_t lastLimitedChargeCurrentdA_ = 0;

  void updateActualSoc(uint8_t candidateSocPct, uint32_t nowMs) {
    if (candidateSocPct <= 100) {
      actualSocPct_ = candidateSocPct;
      lastSocTimestampMs_ = nowMs;
      haveSoc_ = true;
    }
  }

  static uint16_t readLe16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8);
  }

  static void writeLe16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xFF);
    destination[1] = static_cast<uint8_t>(value >> 8);
  }
};

